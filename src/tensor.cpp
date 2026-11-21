// What Tensor<T> (src/std/tensor.ww, M9/M10 question 16) is built on: the built-in __tensor. A
// tensor is a packed list of its items (a List<int>, List<float>, List<bool>, a narrow width's)
// seen through an offset, a shape and a stride for each axis, so that a view, a transposed or
// reshaped tensor, shares the items it came from. Each operation here walks such views, the
// operands broadcast against each other as numpy does, with the answers WinterWolf's arithmetic gives:
// an int's overflow an ArithmeticException unless the ...Wrapped forms are asked for, a float
// with anything a float, a value written into a narrow width checked against its range.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <numeric>
#include <vector>

#include "builtins.h"
#include "fmath.h"
#include "vm.h"

namespace ww {

namespace {

[[noreturn]] void fail(VM& vm, Thrown kind, const std::string& message) { throw vm.runtimeError(kind, message); }

std::vector<int64_t> intsOf(VM& vm, Value v, const char* what) {
    if (!v.isObjType(ObjType::List)) fail(vm, Thrown::ClassCastException, std::string(what) + " is a list of ints");
    ObjList* list = asList(v);
    std::vector<int64_t> out(list->size());
    for (size_t i = 0; i < out.size(); i++) out[i] = vm.toInt(vm.listGet(list, i), what);
    return out;
}

// A tensor as the operations see it: its items' list, and where each item is in it.
struct View {
    ObjList* list = nullptr;
    PackedItems* pack = nullptr;
    int64_t offset = 0;
    std::vector<int64_t> shape, strides;
    size_t count = 1;
    bool isFloat = false;  // its items are floats
    bool isBool = false;
    // One number for every item, where it is not a tensor.
    bool single = false;
    Value one;

    size_t rank() const { return shape.size(); }
};

View viewOf(VM& vm, Value data, Value offset, Value shape, Value strides) {
    View v;
    if (!data.isObjType(ObjType::List) || !asList(data)->packed()) fail(vm, Thrown::IllegalStateException, "a tensor's items are a packed list");
    v.list = asList(data);
    v.pack = v.list->pack.get();
    v.offset = vm.toInt(offset, "offset");
    v.shape = intsOf(vm, shape, "shape");
    v.strides = intsOf(vm, strides, "strides");
    for (int64_t s : v.shape) v.count *= static_cast<size_t>(s);
    const PackedItems& p = *v.pack;
    v.isFloat = p.store == ListStore::Floats || (p.store == ListStore::Narrow && p.narrow == Narrow::Float32);
    v.isBool = p.store == ListStore::Bools;
    if (p.store == ListStore::Items || p.store == ListStore::Values || (p.store == ListStore::Narrow && p.narrow == Narrow::Char))
        fail(vm, Thrown::IllegalStateException, "a tensor holds numbers or bools");
    return v;
}

// A tensor's operand: a tensor (its view's five arguments from `args[at]` on) or one number.
View operand(VM& vm, Value* args, size_t at) {
    if (!args[at].isObjType(ObjType::List)) {
        View v;
        v.single = true;
        v.one = args[at];
        if (v.one.isByte()) v.one = Value::integer(v.one.as.i);
        if (v.one.isBool()) v.isBool = true;
        else if (v.one.isFloat()) v.isFloat = true;
        else if (!v.one.isInt()) fail(vm, Thrown::ClassCastException, "a tensor works with numbers and tensors, not a " + vm.typeName(args[at]));
        return v;
    }
    return viewOf(vm, args[at], args[at + 1], args[at + 2], args[at + 3]);
}

inline double readF(const View& v, int64_t at) {
    if (v.single) return v.one.isBool() ? (v.one.as.b ? 1.0 : 0.0) : v.one.toDouble();
    const PackedItems& p = *v.pack;
    const uint8_t* b = p.block.data() + static_cast<size_t>(at) * p.width;
    switch (p.store) {
        case ListStore::Ints: {
            int64_t x;
            std::memcpy(&x, b, 8);
            return static_cast<double>(x);
        }
        case ListStore::Floats: {
            double x;
            std::memcpy(&x, b, 8);
            return x;
        }
        case ListStore::Bools: return *b != 0 ? 1.0 : 0.0;
        default: {
            const Value x = readNarrow(p.narrow, b);
            return x.isFloat() ? x.as.d : static_cast<double>(x.as.i);
        }
    }
}

inline int64_t readI(const View& v, int64_t at) {
    if (v.single) return v.one.isBool() ? (v.one.as.b ? 1 : 0) : v.one.as.i;
    const PackedItems& p = *v.pack;
    const uint8_t* b = p.block.data() + static_cast<size_t>(at) * p.width;
    switch (p.store) {
        case ListStore::Ints: {
            int64_t x;
            std::memcpy(&x, b, 8);
            return x;
        }
        case ListStore::Bools: return *b != 0;
        default: return readNarrow(p.narrow, b).as.i;
    }
}

void range(Narrow n, int64_t& low, int64_t& high) {
    switch (n) {
        case Narrow::Byte: low = 0, high = 255; break;
        case Narrow::Int16: low = -32768, high = 32767; break;
        case Narrow::Uint16: low = 0, high = 65535; break;
        case Narrow::Int32: low = INT32_MIN, high = INT32_MAX; break;
        case Narrow::Uint32: low = 0, high = UINT32_MAX; break;
        default: low = INT64_MIN, high = INT64_MAX; break;
    }
}

const char* widthName(Narrow n) {
    switch (n) {
        case Narrow::Byte: return "a byte";
        case Narrow::Int16: return "an int16";
        case Narrow::Int32: return "an int32";
        case Narrow::Uint16: return "a uint16";
        default: return "a uint32";
    }
}

// An int written as an item, checked against a narrow width's range, or keeping only its low
// bits where `wrap` says.
inline void writeI(VM& vm, const View& v, int64_t at, int64_t x, bool wrap = false) {
    PackedItems& p = *v.pack;
    uint8_t* b = p.block.data() + static_cast<size_t>(at) * p.width;
    switch (p.store) {
        case ListStore::Ints: std::memcpy(b, &x, 8); return;
        case ListStore::Floats: {
            const double d = static_cast<double>(x);
            std::memcpy(b, &d, 8);
            return;
        }
        case ListStore::Bools: *b = x != 0; return;
        default: {
            if (p.narrow == Narrow::Float32) {
                const float f = static_cast<float>(x);
                std::memcpy(b, &f, 4);
                return;
            }
            int64_t low, high;
            range(p.narrow, low, high);
            if (!wrap && (x < low || x > high))
                fail(vm, Thrown::ArithmeticException, std::to_string(x) + " does not fit in " + widthName(p.narrow) + ", which keeps " + std::to_string(low) + " to " + std::to_string(high));
            if (p.width == 1) *b = static_cast<uint8_t>(x);
            else if (p.width == 2) {
                const uint16_t w = static_cast<uint16_t>(x);
                std::memcpy(b, &w, 2);
            } else {
                const uint32_t w = static_cast<uint32_t>(x);
                std::memcpy(b, &w, 4);
            }
            return;
        }
    }
}

// A float written as an item: into floats as it is, into ints only where it is whole (as
// toInt() would give it otherwise, which the program must ask for).
inline void writeF(VM& vm, const View& v, int64_t at, double x) {
    PackedItems& p = *v.pack;
    uint8_t* b = p.block.data() + static_cast<size_t>(at) * p.width;
    if (p.store == ListStore::Floats) {
        std::memcpy(b, &x, 8);
        return;
    }
    if (p.store == ListStore::Narrow && p.narrow == Narrow::Float32) {
        const float f = static_cast<float>(x);
        std::memcpy(b, &f, 4);
        return;
    }
    if (p.store == ListStore::Bools) {
        *b = x != 0;
        return;
    }
    fail(vm, Thrown::ClassCastException, "a tensor of ints cannot keep the floats this gives: make it floats first, with toFloat()");
}

// The strides of each operand against the shape of the answer: an axis it lacks or has once
// (broadcast) steps by 0.
std::vector<int64_t> aligned(const View& v, const std::vector<int64_t>& shape) {
    std::vector<int64_t> s(shape.size(), 0);
    if (v.single) return s;
    const size_t lead = shape.size() - v.rank();
    for (size_t i = 0; i < v.rank(); i++) s[lead + i] = v.shape[i] == 1 && shape[lead + i] != 1 ? 0 : v.strides[i];
    return s;
}

// The axes a walk goes over: those of length 1 left out, and two next to each other made one
// where every operand steps through them as one (a whole tensor in order is one long row).
struct Axes {
    std::vector<int64_t> shape;
    std::vector<int64_t> strides[4];
};
inline Axes merged(const std::vector<int64_t>& shape, const std::vector<std::vector<int64_t>>& strides) {
    Axes axes;
    const size_t n = strides.size();
    for (size_t axis = 0; axis < shape.size(); axis++) {
        if (shape[axis] == 1) continue;
        bool merge = !axes.shape.empty();
        for (size_t o = 0; o < n && merge; o++) merge = axes.strides[o].back() == strides[o][axis] * shape[axis];
        if (merge) {
            axes.shape.back() *= shape[axis];
            for (size_t o = 0; o < n; o++) axes.strides[o].back() = strides[o][axis];
        } else {
            axes.shape.push_back(shape[axis]);
            for (size_t o = 0; o < n; o++) axes.strides[o].push_back(strides[o][axis]);
        }
    }
    return axes;
}

// Calls `row` with where each operand's first item of a row is, how many items the row has, and
// how far apart each operand's items in it are: for every row along the last axis, in order.
// Four operands at most.
template <class F>
void walkRows(const std::vector<int64_t>& shape, const std::vector<std::vector<int64_t>>& strides, const std::vector<int64_t>& starts, F&& row) {
    const size_t n = strides.size();
    size_t total = 1;
    for (int64_t s : shape) total *= static_cast<size_t>(s);
    if (total == 0) return;
    const Axes axes = merged(shape, strides);
    int64_t at[4] = {0, 0, 0, 0}, step[4] = {0, 0, 0, 0};
    for (size_t o = 0; o < n; o++) at[o] = starts[o];
    if (axes.shape.empty()) {
        row(at, int64_t{1}, step);
        return;
    }
    const size_t rank = axes.shape.size();
    const int64_t inner = axes.shape.back();
    for (size_t o = 0; o < n; o++) step[o] = axes.strides[o][rank - 1];
    std::vector<int64_t> index(rank, 0);
    const size_t rows = total / static_cast<size_t>(inner);
    for (size_t r = 0; r < rows; r++) {
        row(at, inner, step);
        // The next row: the axes before the last forward, carrying into the ones before them.
        for (size_t axis = rank - 1; axis-- > 0;) {
            index[axis]++;
            for (size_t o = 0; o < n; o++) at[o] += axes.strides[o][axis];
            if (index[axis] < axes.shape[axis]) break;
            for (size_t o = 0; o < n; o++) at[o] -= axes.strides[o][axis] * axes.shape[axis];
            index[axis] = 0;
        }
    }
}

// Calls `f` with the item index of each operand, for every item of `shape` in order (the last
// axis fastest).
template <class F>
void walk(const std::vector<int64_t>& shape, const std::vector<std::vector<int64_t>>& strides, const std::vector<int64_t>& starts, F&& f) {
    const size_t n = strides.size();
    walkRows(shape, strides, starts, [&](const int64_t* first, int64_t count, const int64_t* step) {
        int64_t at[4] = {first[0], first[1], first[2], first[3]};
        for (int64_t k = 0; k < count; k++) {
            f(static_cast<const int64_t*>(at));
            for (size_t o = 0; o < n; o++) at[o] += step[o];
        }
    });
}

enum BinOp { kAdd, kSub, kMul, kDiv, kMod, kAddW, kSubW, kMulW, kLt, kLe, kGt, kGe, kEq, kNe, kAnd, kOr };

// ---- The quick way: items of a kind known before the loop ----

// How an operand's items are kept, where the loops below read them straight: 8-byte ints, 8-byte
// floats, bytes, or one number for every item. Anything else (bools, the other narrow widths)
// goes the general way, item by item through readI and readF.
enum class Lane { I64, F64, U8, One, Other };
inline Lane laneOf(const View& v) {
    if (v.single) return v.one.isBool() ? Lane::Other : Lane::One;
    switch (v.pack->store) {
        case ListStore::Ints: return Lane::I64;
        case ListStore::Floats: return Lane::F64;
        case ListStore::Narrow: return v.pack->narrow == Narrow::Byte ? Lane::U8 : Lane::Other;
        default: return Lane::Other;
    }
}

// An operand's items read as ints or floats, its lane fixed when compiling.
template <Lane L>
struct In {
    const uint8_t* base = nullptr;
    int64_t oneI = 0;
    double oneF = 0;
    explicit In(const View& v) {
        if constexpr (L == Lane::One) {
            oneI = v.one.isFloat() ? 0 : v.one.as.i;
            oneF = v.one.toDouble();
        } else {
            base = v.pack->block.data();
        }
    }
    int64_t i(int64_t at) const {
        if constexpr (L == Lane::I64) {
            int64_t x;
            std::memcpy(&x, base + at * 8, 8);
            return x;
        } else if constexpr (L == Lane::U8) {
            return base[at];
        } else if constexpr (L == Lane::F64) {
            double x;
            std::memcpy(&x, base + at * 8, 8);
            return static_cast<int64_t>(x);
        } else {
            return oneI;
        }
    }
    double f(int64_t at) const {
        if constexpr (L == Lane::F64) {
            double x;
            std::memcpy(&x, base + at * 8, 8);
            return x;
        } else if constexpr (L == Lane::I64) {
            int64_t x;
            std::memcpy(&x, base + at * 8, 8);
            return static_cast<double>(x);
        } else if constexpr (L == Lane::U8) {
            return base[at];
        } else {
            return oneF;
        }
    }
};

// Calls `fn` with the operand's reader, its lane a constant; false for the general way.
template <class Fn>
bool withLane(const View& v, Fn&& fn) {
    switch (laneOf(v)) {
        case Lane::I64: fn(In<Lane::I64>(v)); return true;
        case Lane::F64: fn(In<Lane::F64>(v)); return true;
        case Lane::U8: fn(In<Lane::U8>(v)); return true;
        case Lane::One: fn(In<Lane::One>(v)); return true;
        default: return false;
    }
}
// The same for two operands.
template <class Fn>
bool withLanes(const View& a, const View& b, Fn&& fn) {
    if (laneOf(a) == Lane::Other || laneOf(b) == Lane::Other) return false;
    withLane(a, [&](auto x) { withLane(b, [&](auto y) { fn(x, y); }); });
    return true;
}

inline void putF(uint8_t* base, int64_t at, double x) { std::memcpy(base + at * 8, &x, 8); }
inline void putI(uint8_t* base, int64_t at, int64_t x) { std::memcpy(base + at * 8, &x, 8); }
[[noreturn]] void notAByte(VM& vm, int64_t x) {
    fail(vm, Thrown::ArithmeticException, std::to_string(x) + " does not fit in a byte, which keeps 0 to 255");
}
// An int written into bytes: checked, or its low bits where `wrap` says.
inline void putU8(VM& vm, uint8_t* base, int64_t at, int64_t x, bool wrap) {
    if (!wrap && (x < 0 || x > 255)) notAByte(vm, x);
    base[at] = static_cast<uint8_t>(x);
}
// A float made an int, as toInt() makes it.
inline int64_t tensorFloatToInt(VM& vm, double d) {
    if (std::isnan(d) || d >= 9.2233720368547758e18 || d < -9.2233720368547758e18) fail(vm, Thrown::ArithmeticException, "a NaN or a float too large has no int");
    return static_cast<int64_t>(d);
}

// binary's quick way: + - * / % and the wrapping ones, and the comparisons, of operands of
// known lanes into 8-byte ints, floats, bytes or bools. False where it does not apply.
bool quickBinary(VM& vm, BinOp op, const View& a, const View& b, const View& out, const std::vector<std::vector<int64_t>>& strides) {
    const bool ints = !a.isFloat && !b.isFloat;
    const bool compare = op >= kLt && op <= kNe;
    const Lane lo = laneOf(out);
    uint8_t* o = out.pack->block.data();
    if (compare) {
        if (out.pack->store != ListStore::Bools) return false;
        return withLanes(a, b, [&](auto A, auto B) {
            walkRows(out.shape, strides, {a.offset, b.offset, out.offset}, [&](const int64_t* at, int64_t n, const int64_t* st) {
                const int64_t pa = at[0], pb = at[1], po = at[2], sa = st[0], sb = st[1], so = st[2];
#define WW_COMPARE(CMP)                                                                       \
    if (ints) {                                                                                 \
        for (int64_t k = 0; k < n; k++) o[po + k * so] = A.i(pa + k * sa) CMP B.i(pb + k * sb); \
    } else {                                                                                    \
        for (int64_t k = 0; k < n; k++) o[po + k * so] = A.f(pa + k * sa) CMP B.f(pb + k * sb); \
    }
                switch (op) {
                    case kLt: WW_COMPARE(<) break;
                    case kLe: WW_COMPARE(<=) break;
                    case kGt: WW_COMPARE(>) break;
                    case kGe: WW_COMPARE(>=) break;
                    case kEq: WW_COMPARE(==) break;
                    default: WW_COMPARE(!=) break;
                }
#undef WW_COMPARE
            });
        });
    }
    if (op > kMulW) return false;
    if (!ints) {
        if (lo != Lane::F64 || op >= kAddW) return false;
        return withLanes(a, b, [&](auto A, auto B) {
            walkRows(out.shape, strides, {a.offset, b.offset, out.offset}, [&](const int64_t* at, int64_t n, const int64_t* st) {
                const int64_t pa = at[0], pb = at[1], po = at[2], sa = st[0], sb = st[1], so = st[2];
#define WW_FLOATS(EXPR)                                        \
    for (int64_t k = 0; k < n; k++) {                            \
        const double x = A.f(pa + k * sa), y = B.f(pb + k * sb); \
        putF(o, po + k * so, EXPR);                              \
    }
                switch (op) {
                    case kAdd: WW_FLOATS(x + y) break;
                    case kSub: WW_FLOATS(x - y) break;
                    case kMul: WW_FLOATS(x * y) break;
                    case kDiv: WW_FLOATS(x / y) break;
                    default: WW_FLOATS(std::fmod(x, y)) break;
                }
#undef WW_FLOATS
            });
        });
    }
    if (lo != Lane::I64 && lo != Lane::U8) return false;
    const bool wrap = op >= kAddW;
    return withLanes(a, b, [&](auto A, auto B) {
        walkRows(out.shape, strides, {a.offset, b.offset, out.offset}, [&](const int64_t* at, int64_t n, const int64_t* st) {
            const int64_t pa = at[0], pb = at[1], po = at[2], sa = st[0], sb = st[1], so = st[2];
            for (int64_t k = 0; k < n; k++) {
                const int64_t x = A.i(pa + k * sa), y = B.i(pb + k * sb);
                int64_t z = 0;
                switch (op) {
                    case kAdd: if (addOverflows(x, y, z)) throw vm.overflowError(Op::Add, x, y); break;
                    case kSub: if (subOverflows(x, y, z)) throw vm.overflowError(Op::Sub, x, y); break;
                    case kMul: if (mulOverflows(x, y, z)) throw vm.overflowError(Op::Mul, x, y); break;
                    case kDiv:
                    case kMod:
                        if (y == 0) fail(vm, Thrown::ArithmeticException, "division by zero");
                        if (y == -1 && x == INT64_MIN) {
                            if (op == kDiv) throw vm.overflowError(Op::Div, x, y);
                            z = 0;
                        } else {
                            z = op == kDiv ? x / y : x % y;
                        }
                        break;
                    case kAddW: z = static_cast<int64_t>(static_cast<uint64_t>(x) + static_cast<uint64_t>(y)); break;
                    case kSubW: z = static_cast<int64_t>(static_cast<uint64_t>(x) - static_cast<uint64_t>(y)); break;
                    default: z = static_cast<int64_t>(static_cast<uint64_t>(x) * static_cast<uint64_t>(y)); break;
                }
                if (lo == Lane::I64) putI(o, po + k * so, z);
                else putU8(vm, o, po + k * so, z, wrap);
            }
        });
    });
}

// unary's quick way: a float function of each item into floats, an int one into ints, and the
// conversions between 8-byte ints, floats and bytes. False where it does not apply.
bool quickUnary(VM& vm, int op, const View& a, const View& out, double p, double q) {
    const Lane li = laneOf(a), lo = laneOf(out);
    if (li == Lane::Other || li == Lane::One || lo == Lane::Other) return false;
    uint8_t* o = out.pack->block.data();
    const std::vector<std::vector<int64_t>> strides = {a.strides, out.strides};
    const bool ints = !a.isFloat && !out.isFloat;
    auto rows = [&](auto&& item) {
        withLane(a, [&](auto A) {
            walkRows(a.shape, strides, {a.offset, out.offset}, [&](const int64_t* at, int64_t n, const int64_t* st) {
                for (int64_t k = 0; k < n; k++) item(A, at[0] + k * st[0], at[1] + k * st[1]);
            });
        });
    };
    if (op == 16 || op == 17) {
        const bool wrap = op == 17;
        if (lo == Lane::F64) rows([&](const auto& A, int64_t i, int64_t j) { putF(o, j, A.f(i)); });
        else if (li == Lane::F64 && lo == Lane::I64) rows([&](const auto& A, int64_t i, int64_t j) { putI(o, j, tensorFloatToInt(vm, A.f(i))); });
        else if (li == Lane::F64) rows([&](const auto& A, int64_t i, int64_t j) { putU8(vm, o, j, tensorFloatToInt(vm, A.f(i)), wrap); });
        else if (lo == Lane::I64) rows([&](const auto& A, int64_t i, int64_t j) { putI(o, j, A.i(i)); });
        else rows([&](const auto& A, int64_t i, int64_t j) { putU8(vm, o, j, A.i(i), wrap); });
        return true;
    }
    if (ints) {
        if (lo != Lane::I64 || !(op <= 1 || op == 14 || (op >= 11 && op <= 13))) return false;
        const int64_t low = static_cast<int64_t>(p), high = static_cast<int64_t>(q);
        rows([&](const auto& A, int64_t i, int64_t j) {
            const int64_t x = A.i(i);
            int64_t z = x;
            if (op == 0 || (op == 1 && x < 0)) {
                if (x == INT64_MIN) throw vm.overflowError(Op::Sub, 0, x);
                z = -x;
            } else if (op == 14) {
                z = std::clamp(x, low, high);
            }
            putI(o, j, z);
        });
        return true;
    }
    if (lo != Lane::F64 || op > 14) return false;
#define WW_UNARY(EXPR)                                         \
    rows([&](const auto& A, int64_t i, int64_t j) {              \
        const double x = A.f(i);                                 \
        putF(o, j, EXPR);                                        \
    });                                                          \
    break;
    switch (op) {
        case 0: WW_UNARY(-x)
        case 1: WW_UNARY(std::fabs(x))
        case 2: WW_UNARY(std::sqrt(x))
        case 3: WW_UNARY(fmath::exp(x))
        case 4: WW_UNARY(fmath::log(x))
        case 5: WW_UNARY(fmath::logBase(x, p))
        case 6: WW_UNARY(fmath::log10(x))
        case 7: WW_UNARY(fmath::pow(x, p))
        case 8: WW_UNARY(fmath::sin(x))
        case 9: WW_UNARY(fmath::cos(x))
        case 10: WW_UNARY(fmath::tan(x))
        case 11: WW_UNARY(std::floor(x))
        case 12: WW_UNARY(std::ceil(x))
        case 13: WW_UNARY(std::round(x))
        default: WW_UNARY(std::clamp(x, p, q))
    }
#undef WW_UNARY
    return true;
}

// copy's quick way for one number in every item (fill) of 8-byte ints, floats or bytes: the item
// made once in their own form, then laid down. False where it does not apply (a float into
// ints says what the general way says).
bool fillQuickly(VM& vm, const View& a, const View& out) {
    if (!a.single || laneOf(a) != Lane::One) return false;
    const Lane lo = laneOf(out);
    if (lo != Lane::I64 && lo != Lane::F64 && lo != Lane::U8) return false;
    uint8_t item[8];
    if (lo == Lane::F64) {
        const double d = a.one.toDouble();
        std::memcpy(item, &d, 8);
    } else if (a.one.isFloat()) {
        return false;
    } else if (lo == Lane::I64) {
        std::memcpy(item, &a.one.as.i, 8);
    } else {
        if (a.one.as.i < 0 || a.one.as.i > 255) notAByte(vm, a.one.as.i);
        item[0] = static_cast<uint8_t>(a.one.as.i);
    }
    const size_t width = lo == Lane::U8 ? 1 : 8;
    uint8_t* to = out.pack->block.data();
    walkRows(out.shape, {out.strides}, {out.offset}, [&](const int64_t* at, int64_t n, const int64_t* st) {
        if (width == 1 && st[0] == 1) std::memset(to + at[0], item[0], static_cast<size_t>(n));
        else for (int64_t k = 0; k < n; k++) std::memcpy(to + (at[0] + k * st[0]) * width, item, width);
    });
    return true;
}

}  // namespace

void registerTensor(VM& vm) {
    ObjClass* lib = vm.alloc<ObjClass>(vm.intern("__tensor"));
    auto def = [&](const char* name, int minArgs, int maxArgs, NativeFn fn) {
        lib->statics.set(vm.intern(name), Value::object(vm.alloc<ObjNative>(std::string("__tensor.") + name, minArgs, maxArgs, fn)));
    };

    // binary(op, a..., b..., out...): item by item, broadcast: + - * / % and the three that wrap,
    // the comparisons (into bools), and and or. `a` and `b` are each a tensor (data, offset,
    // shape, strides) or a number and three nulls; `out` a tensor of the broadcast shape.
    def("binary", 13, 13, [](VM& vm, int, Value* args) -> Value {
        const auto op = static_cast<BinOp>(vm.toInt(args[0], "op"));
        const View a = operand(vm, args, 1), b = operand(vm, args, 5), out = viewOf(vm, args[9], args[10], args[11], args[12]);
        const std::vector<std::vector<int64_t>> strides = {aligned(a, out.shape), aligned(b, out.shape), out.strides};
        if (quickBinary(vm, op, a, b, out, strides)) return Value::null();
        const bool ints = !a.isFloat && !b.isFloat;
        const bool compare = op >= kLt && op <= kNe;
        if (op == kAnd || op == kOr) {
            walk(out.shape, strides, {a.offset, b.offset, out.offset}, [&](const int64_t* at) {
                const bool x = readI(a, at[0]) != 0, y = readI(b, at[1]) != 0;
                writeI(vm, out, at[2], op == kAnd ? (x && y) : (x || y));
            });
            return Value::null();
        }
        if (compare) {
            walk(out.shape, strides, {a.offset, b.offset, out.offset}, [&](const int64_t* at) {
                bool r;
                if (ints) {
                    const int64_t x = readI(a, at[0]), y = readI(b, at[1]);
                    r = op == kLt ? x < y : op == kLe ? x <= y : op == kGt ? x > y : op == kGe ? x >= y : op == kEq ? x == y : x != y;
                } else {
                    const double x = readF(a, at[0]), y = readF(b, at[1]);
                    r = op == kLt ? x < y : op == kLe ? x <= y : op == kGt ? x > y : op == kGe ? x >= y : op == kEq ? x == y : x != y;
                }
                writeI(vm, out, at[2], r);
            });
            return Value::null();
        }
        if (ints && !out.isFloat) {
            walk(out.shape, strides, {a.offset, b.offset, out.offset}, [&](const int64_t* at) {
                const int64_t x = readI(a, at[0]), y = readI(b, at[1]);
                int64_t z = 0;
                switch (op) {
                    case kAdd: if (addOverflows(x, y, z)) throw vm.overflowError(Op::Add, x, y); break;
                    case kSub: if (subOverflows(x, y, z)) throw vm.overflowError(Op::Sub, x, y); break;
                    case kMul: if (mulOverflows(x, y, z)) throw vm.overflowError(Op::Mul, x, y); break;
                    case kDiv:
                    case kMod:
                        if (y == 0) fail(vm, Thrown::ArithmeticException, "division by zero");
                        if (y == -1 && x == INT64_MIN) {
                            if (op == kDiv) throw vm.overflowError(Op::Div, x, y);
                            z = 0;
                        } else {
                            z = op == kDiv ? x / y : x % y;
                        }
                        break;
                    case kAddW: z = static_cast<int64_t>(static_cast<uint64_t>(x) + static_cast<uint64_t>(y)); break;
                    case kSubW: z = static_cast<int64_t>(static_cast<uint64_t>(x) - static_cast<uint64_t>(y)); break;
                    default: z = static_cast<int64_t>(static_cast<uint64_t>(x) * static_cast<uint64_t>(y)); break;
                }
                writeI(vm, out, at[2], z, op >= kAddW);
            });
            return Value::null();
        }
        if (op >= kAddW) fail(vm, Thrown::ClassCastException, "the ...Wrapped forms are for ints");
        walk(out.shape, strides, {a.offset, b.offset, out.offset}, [&](const int64_t* at) {
            const double x = readF(a, at[0]), y = readF(b, at[1]);
            writeF(vm, out, at[2], op == kAdd ? x + y : op == kSub ? x - y : op == kMul ? x * y : op == kDiv ? x / y : std::fmod(x, y));
        });
        return Value::null();
    });

    // unary(op, a..., out..., p, q): each item through a function: 0 negated, 1 abs, 2 sqrt,
    // 3 exp, 4 ln, 5 log in base p, 6 log10, 7 to the power p, 8 sin, 9 cos, 10 tan, 11 floor,
    // 12 ceil, 13 round, 14 held from p to q, 15 not, 16 converted (checked), 17 converted
    // keeping the low bits.
    def("unary", 11, 11, [](VM& vm, int, Value* args) -> Value {
        const int op = static_cast<int>(vm.toInt(args[0], "op"));
        const View a = viewOf(vm, args[1], args[2], args[3], args[4]), out = viewOf(vm, args[5], args[6], args[7], args[8]);
        if (a.shape != out.shape) fail(vm, Thrown::IllegalArgumentException, "into is not the tensor's shape");
        const std::vector<std::vector<int64_t>> strides = {a.strides, out.strides};
        const bool ints = !a.isFloat && !out.isFloat;
        const double p = args[9].isNull() ? 0 : vm.toNumber(args[9], "p"), q = args[10].isNull() ? 0 : vm.toNumber(args[10], "q");
        if (quickUnary(vm, op, a, out, p, q)) return Value::null();
        if (op == 15) {
            walk(a.shape, strides, {a.offset, out.offset}, [&](const int64_t* at) { writeI(vm, out, at[1], readI(a, at[0]) == 0); });
            return Value::null();
        }
        if (op == 16 || op == 17) {
            walk(a.shape, strides, {a.offset, out.offset}, [&](const int64_t* at) {
                if (out.isFloat) {
                    writeF(vm, out, at[1], readF(a, at[0]));
                } else if (a.isFloat) {
                    const double d = readF(a, at[0]);
                    if (std::isnan(d) || d >= 9.2233720368547758e18 || d < -9.2233720368547758e18)
                        fail(vm, Thrown::ArithmeticException, "a NaN or a float too large has no int");
                    writeI(vm, out, at[1], static_cast<int64_t>(d), op == 17);
                } else {
                    writeI(vm, out, at[1], readI(a, at[0]), op == 17);
                }
            });
            return Value::null();
        }
        if (ints && (op <= 1 || op == 14 || (op >= 11 && op <= 13))) {
            const int64_t lo = static_cast<int64_t>(p), hi = static_cast<int64_t>(q);
            walk(a.shape, strides, {a.offset, out.offset}, [&](const int64_t* at) {
                const int64_t x = readI(a, at[0]);
                int64_t z = x;
                if (op == 0 || (op == 1 && x < 0)) {
                    if (x == INT64_MIN) throw vm.overflowError(Op::Sub, 0, x);
                    z = -x;
                } else if (op == 14) {
                    z = std::clamp(x, lo, hi);
                }
                writeI(vm, out, at[1], z);
            });
            return Value::null();
        }
        walk(a.shape, strides, {a.offset, out.offset}, [&](const int64_t* at) {
            const double x = readF(a, at[0]);
            double z;
            switch (op) {
                case 0: z = -x; break;
                case 1: z = std::fabs(x); break;
                case 2: z = std::sqrt(x); break;
                case 3: z = fmath::exp(x); break;
                case 4: z = fmath::log(x); break;
                case 5: z = fmath::logBase(x, p); break;
                case 6: z = fmath::log10(x); break;
                case 7: z = fmath::pow(x, p); break;
                case 8: z = fmath::sin(x); break;
                case 9: z = fmath::cos(x); break;
                case 10: z = fmath::tan(x); break;
                case 11: z = std::floor(x); break;
                case 12: z = std::ceil(x); break;
                case 13: z = std::round(x); break;
                default: z = std::clamp(x, p, q); break;
            }
            writeF(vm, out, at[1], z);
        });
        return Value::null();
    });

    // reduce(op, a..., axis, out...): 0 sum, 1 mean, 2 min, 3 max, 4 product, 5 indexOfMin,
    // 6 indexOfMax, 7 any, 8 all, 9 count; over the whole (axis -1: the answer given) or along
    // one axis, into `out`, of the shape without it.
    def("reduce", 10, 10, [](VM& vm, int, Value* args) -> Value {
        const int op = static_cast<int>(vm.toInt(args[0], "op"));
        const View a = viewOf(vm, args[1], args[2], args[3], args[4]);
        const int64_t axis = vm.toInt(args[5], "axis");
        const bool ints = !a.isFloat;
        // Folds `n` items, the i-th at item index at(i).
        auto fold = [&](const std::function<int64_t(int64_t)>& at, int64_t n) -> Value {
            if (op == 7 || op == 8 || op == 9) {
                int64_t trues = 0;
                for (int64_t i = 0; i < n; i++) trues += readI(a, at(i)) != 0;
                return op == 7 ? Value::boolean(trues > 0) : op == 8 ? Value::boolean(trues == n) : Value::integer(trues);
            }
            if (n == 0) {
                if (op == 0) return ints ? Value::integer(0) : Value::number(0);
                if (op == 4) return ints ? Value::integer(1) : Value::number(1);
                if (op == 1) return Value::number(std::nan(""));
                fail(vm, Thrown::NoSuchElementException, "an empty tensor has no least or greatest item");
            }
            if (op == 5 || op == 6) {
                int64_t best = 0;
                if (ints) {
                    int64_t v = readI(a, at(0));
                    for (int64_t i = 1; i < n; i++) {
                        const int64_t x = readI(a, at(i));
                        if (op == 5 ? x < v : x > v) v = x, best = i;
                    }
                } else {
                    double v = readF(a, at(0));
                    for (int64_t i = 1; i < n && !std::isnan(v); i++) {
                        const double x = readF(a, at(i));
                        if (std::isnan(x) || (op == 5 ? x < v : x > v)) v = x, best = i;
                    }
                }
                return Value::integer(best);
            }
            if (ints && op != 1) {
                int64_t t = readI(a, at(0));
                for (int64_t i = 1; i < n; i++) {
                    const int64_t x = readI(a, at(i));
                    int64_t z;
                    if (op == 0) {
                        if (addOverflows(t, x, z)) throw vm.overflowError(Op::Add, t, x);
                        t = z;
                    } else if (op == 4) {
                        if (mulOverflows(t, x, z)) throw vm.overflowError(Op::Mul, t, x);
                        t = z;
                    } else {
                        t = op == 2 ? std::min(t, x) : std::max(t, x);
                    }
                }
                return Value::integer(t);
            }
            double t = readF(a, at(0));
            for (int64_t i = 1; i < n; i++) {
                const double x = readF(a, at(i));
                if (op == 0 || op == 1) t += x;
                else if (op == 4) t *= x;
                else if (std::isnan(x) || std::isnan(t)) t = std::nan("");
                else t = op == 2 ? std::min(t, x) : std::max(t, x);
            }
            return Value::number(op == 1 ? t / static_cast<double>(n) : t);
        };
        if (axis < 0) {
            // The whole, its items in order (the last axis fastest): read where they are when
            // they lie one after another, else gathered first.
            bool inOrder = true;
            int64_t step = 1;
            for (size_t i = a.rank(); i-- > 0;) {
                if (a.shape[i] != 1 && a.strides[i] != step) inOrder = false;
                step *= a.shape[i];
            }
            if (inOrder) {
                const int64_t first = a.offset;
                return fold([first](int64_t i) { return first + i; }, static_cast<int64_t>(a.count));
            }
            std::vector<int64_t> items;
            items.reserve(a.count);
            walk(a.shape, {a.strides}, {a.offset}, [&](const int64_t* at) { items.push_back(at[0]); });
            return fold([&](int64_t i) { return items[static_cast<size_t>(i)]; }, static_cast<int64_t>(items.size()));
        }
        if (axis >= static_cast<int64_t>(a.rank())) fail(vm, Thrown::IllegalArgumentException, "axis " + std::to_string(axis) + " is not one of the " + std::to_string(a.rank()));
        const View out = viewOf(vm, args[6], args[7], args[8], args[9]);
        // The answer's items, and where each line along the axis starts.
        std::vector<int64_t> rest, restStrides;
        for (size_t i = 0; i < a.rank(); i++)
            if (static_cast<int64_t>(i) != axis) rest.push_back(a.shape[i]), restStrides.push_back(a.strides[i]);
        if (rest != out.shape) fail(vm, Thrown::IllegalArgumentException, "into is not the shape the answer has");
        const int64_t n = a.shape[static_cast<size_t>(axis)], step = a.strides[static_cast<size_t>(axis)];
        walk(rest, {restStrides, out.strides}, {a.offset, out.offset}, [&](const int64_t* at) {
            const int64_t start = at[0];
            const Value r = fold([&](int64_t i) { return start + i * step; }, n);
            if (r.isFloat()) writeF(vm, out, at[1], r.as.d);
            else writeI(vm, out, at[1], r.isBool() ? r.as.b : r.as.i);
        });
        return Value::null();
    });

    // running(a..., axis, out...): prefix sums along an axis, or over the whole in order into a
    // tensor of one axis (axis -1).
    def("running", 9, 9, [](VM& vm, int, Value* args) -> Value {
        const View a = viewOf(vm, args[0], args[1], args[2], args[3]);
        const int64_t axis = vm.toInt(args[4], "axis");
        const View out = viewOf(vm, args[5], args[6], args[7], args[8]);
        const bool ints = !a.isFloat && !out.isFloat;
        if (axis < 0) {
            int64_t ti = 0, k = 0;
            double tf = 0;
            walk(a.shape, {a.strides}, {a.offset}, [&](const int64_t* at) {
                const int64_t to = out.offset + k++ * out.strides[0];
                if (ints) {
                    const int64_t x = readI(a, at[0]);
                    if (addOverflows(ti, x, ti)) throw vm.overflowError(Op::Add, ti - x, x);
                    writeI(vm, out, to, ti);
                } else {
                    tf += readF(a, at[0]);
                    writeF(vm, out, to, tf);
                }
            });
            return Value::null();
        }
        std::vector<int64_t> rest, ra, ro;
        for (size_t i = 0; i < a.rank(); i++)
            if (static_cast<int64_t>(i) != axis) rest.push_back(a.shape[i]), ra.push_back(a.strides[i]), ro.push_back(out.strides[i]);
        const int64_t n = a.shape[static_cast<size_t>(axis)], sa = a.strides[static_cast<size_t>(axis)], so = out.strides[static_cast<size_t>(axis)];
        walk(rest, {ra, ro}, {a.offset, out.offset}, [&](const int64_t* at) {
            int64_t ti = 0;
            double tf = 0;
            for (int64_t i = 0; i < n; i++) {
                if (ints) {
                    const int64_t x = readI(a, at[0] + i * sa);
                    if (addOverflows(ti, x, ti)) throw vm.overflowError(Op::Add, ti - x, x);
                    writeI(vm, out, at[1] + i * so, ti);
                } else {
                    tf += readF(a, at[0] + i * sa);
                    writeF(vm, out, at[1] + i * so, tf);
                }
            }
        });
        return Value::null();
    });

    // copy(a..., out...): a tensor's items written into another of the same shape, or `a` one
    // number written into every item of `out` (fill); a narrow width's range checked.
    def("copy", 8, 8, [](VM& vm, int, Value* args) -> Value {
        const View a = operand(vm, args, 0), out = viewOf(vm, args[4], args[5], args[6], args[7]);
        if (!a.single && a.shape != out.shape) fail(vm, Thrown::IllegalArgumentException, "the tensors' shapes differ");
        const bool sameKind = !a.single && a.pack->store == out.pack->store && a.pack->width == out.pack->width && a.pack->narrow == out.pack->narrow;
        if (sameKind && (a.pack->width == 8 || a.pack->width == 1)) {
            // Items of one width copied a row at a time, without asking what they are.
            const uint8_t* from = a.pack->block.data();
            uint8_t* to = out.pack->block.data();
            const size_t width = a.pack->width;
            walkRows(out.shape, {aligned(a, out.shape), out.strides}, {a.offset, out.offset}, [&](const int64_t* at, int64_t n, const int64_t* st) {
                if (st[0] == 1 && st[1] == 1) {
                    std::memmove(to + at[1] * width, from + at[0] * width, static_cast<size_t>(n) * width);
                } else if (width == 8) {
                    for (int64_t k = 0; k < n; k++) std::memcpy(to + (at[1] + k * st[1]) * 8, from + (at[0] + k * st[0]) * 8, 8);
                } else {
                    for (int64_t k = 0; k < n; k++) to[at[1] + k * st[1]] = from[at[0] + k * st[0]];
                }
            });
            return Value::null();
        }
        if (fillQuickly(vm, a, out)) return Value::null();
        walk(out.shape, {aligned(a, out.shape), out.strides}, {a.offset, out.offset}, [&](const int64_t* at) {
            if (sameKind) {
                std::memmove(out.pack->block.data() + static_cast<size_t>(at[1]) * out.pack->width, a.pack->block.data() + static_cast<size_t>(at[0]) * a.pack->width,
                             a.pack->width);
                return;
            }
            if (a.isFloat && !a.isBool) writeF(vm, out, at[1], readF(a, at[0]));
            else writeI(vm, out, at[1], readI(a, at[0]));
        });
        return Value::null();
    });

    // where(c..., t..., f..., out...): each item `t`'s where `c` is true, else `f`'s, broadcast;
    // `t` and `f` may be numbers.
    def("where", 16, 16, [](VM& vm, int, Value* args) -> Value {
        const View c = operand(vm, args, 0), t = operand(vm, args, 4), f = operand(vm, args, 8), out = viewOf(vm, args[12], args[13], args[14], args[15]);
        walk(out.shape, {aligned(c, out.shape), aligned(t, out.shape), aligned(f, out.shape), out.strides}, {c.offset, t.offset, f.offset, out.offset},
             [&](const int64_t* at) {
                 const bool pick = readI(c, at[0]) != 0;
                 const View& from = pick ? t : f;
                 if (from.isFloat) writeF(vm, out, at[3], readF(from, pick ? at[1] : at[2]));
                 else writeI(vm, out, at[3], readI(from, pick ? at[1] : at[2]));
             });
        return Value::null();
    });

    // masked(a..., mask..., out...): the items where a tensor of bools of the same shape is
    // true, in order, into a tensor of one axis; with `out` null, how many there are. With
    // `set` given (a number), those items set to it instead.
    def("masked", 9, 10, [](VM& vm, int argc, Value* args) -> Value {
        const View a = viewOf(vm, args[0], args[1], args[2], args[3]), m = viewOf(vm, args[4], args[5], args[6], args[7]);
        if (a.shape != m.shape) fail(vm, Thrown::IllegalArgumentException, "a mask is of the tensor's shape");
        if (argc > 9) {
            const Value v = args[9].isByte() ? Value::integer(args[9].as.i) : args[9];
            walk(a.shape, {a.strides, m.strides}, {a.offset, m.offset}, [&](const int64_t* at) {
                if (readI(m, at[1]) == 0) return;
                if (v.isFloat()) writeF(vm, a, at[0], v.as.d);
                else writeI(vm, a, at[0], v.isBool() ? v.as.b : v.as.i);
            });
            return Value::null();
        }
        if (args[8].isNull()) {
            int64_t n = 0;
            walk(m.shape, {m.strides}, {m.offset}, [&](const int64_t* at) { n += readI(m, at[0]) != 0; });
            return Value::integer(n);
        }
        ObjList* out = asList(args[8]);
        PackedItems& p = *out->pack;
        size_t k = 0;
        walk(a.shape, {a.strides, m.strides}, {a.offset, m.offset}, [&](const int64_t* at) {
            if (readI(m, at[1]) == 0) return;
            std::memcpy(p.block.data() + k++ * p.width, a.pack->block.data() + static_cast<size_t>(at[0]) * a.pack->width, p.width);
        });
        return Value::null();
    });

    // dot(a..., b..., out...): the matrix product, of two vectors (out null: the answer
    // given), a matrix and a vector, or two matrices.
    def("dot", 12, 12, [](VM& vm, int, Value* args) -> Value {
        const View a = viewOf(vm, args[0], args[1], args[2], args[3]), b = viewOf(vm, args[4], args[5], args[6], args[7]);
        const bool ints = !a.isFloat && !b.isFloat;
        auto product = [&](int64_t pa, int64_t sa, int64_t pb, int64_t sb, int64_t n) -> Value {
            if (ints) {
                int64_t t = 0;
                for (int64_t i = 0; i < n; i++) {
                    int64_t x;
                    const int64_t u = readI(a, pa + i * sa), w = readI(b, pb + i * sb);
                    if (mulOverflows(u, w, x)) throw vm.overflowError(Op::Mul, u, w);
                    if (addOverflows(t, x, t)) throw vm.overflowError(Op::Add, t - x, x);
                }
                return Value::integer(t);
            }
            double t = 0;
            for (int64_t i = 0; i < n; i++) t += readF(a, pa + i * sa) * readF(b, pb + i * sb);
            return Value::number(t);
        };
        // The length of what is summed: a's last axis, b's first (or only).
        const int64_t n = a.shape.back(), nb = b.rank() == 1 ? b.shape[0] : b.shape[b.rank() - 2];
        if (n != nb) fail(vm, Thrown::IllegalArgumentException, "a dot product sums over " + std::to_string(n) + " and " + std::to_string(nb) + " items: they must be one number");
        const int64_t sa = a.strides.back(), sb = b.rank() == 1 ? b.strides[0] : b.strides[b.rank() - 2];
        if (a.rank() == 1 && b.rank() == 1) return product(a.offset, sa, b.offset, sb, n);
        const View out = viewOf(vm, args[8], args[9], args[10], args[11]);
        auto put = [&](int64_t at, Value v) {
            if (v.isFloat()) writeF(vm, out, at, v.as.d);
            else writeI(vm, out, at, v.as.i);
        };
        if (a.rank() == 2 && b.rank() == 1) {
            for (int64_t i = 0; i < a.shape[0]; i++) put(out.offset + i * out.strides[0], product(a.offset + i * a.strides[0], sa, b.offset, sb, n));
            return Value::null();
        }
        if (a.rank() == 1 && b.rank() == 2) {
            for (int64_t j = 0; j < b.shape[1]; j++) put(out.offset + j * out.strides[0], product(a.offset, sa, b.offset + j * b.strides[1], sb, n));
            return Value::null();
        }
        if (a.rank() != 2 || b.rank() != 2) fail(vm, Thrown::IllegalArgumentException, "dot is of vectors and matrices: tensors of one or two axes");
        for (int64_t i = 0; i < a.shape[0]; i++)
            for (int64_t j = 0; j < b.shape[1]; j++)
                put(out.offset + i * out.strides[0] + j * out.strides[1], product(a.offset + i * a.strides[0], sa, b.offset + j * b.strides[1], sb, n));
        return Value::null();
    });

    // linear(op, a..., b..., out...): of a square matrix of floats, by LU with the largest pivot
    // in each column: 0 its inverse into `out`, 1 x where a.dot(x) == b into `out`, 2 its
    // determinant (given). An ArithmeticException for a matrix that has no inverse.
    def("linear", 13, 13, [](VM& vm, int, Value* args) -> Value {
        const int op = static_cast<int>(vm.toInt(args[0], "op"));
        const View a = viewOf(vm, args[1], args[2], args[3], args[4]);
        if (a.rank() != 2 || a.shape[0] != a.shape[1]) fail(vm, Thrown::IllegalArgumentException, "a square matrix is a tensor of two axes of one length");
        const size_t n = static_cast<size_t>(a.shape[0]);
        std::vector<double> m(n * n);
        for (size_t i = 0; i < n; i++)
            for (size_t j = 0; j < n; j++) m[i * n + j] = readF(a, a.offset + static_cast<int64_t>(i) * a.strides[0] + static_cast<int64_t>(j) * a.strides[1]);
        std::vector<size_t> perm(n);
        std::iota(perm.begin(), perm.end(), size_t{0});
        double det = 1;
        bool singular = false;
        for (size_t c = 0; c < n; c++) {
            size_t best = c;
            for (size_t r = c + 1; r < n; r++)
                if (std::fabs(m[r * n + c]) > std::fabs(m[best * n + c])) best = r;
            if (m[best * n + c] == 0) {
                singular = true;
                det = 0;
                break;
            }
            if (best != c) {
                for (size_t j = 0; j < n; j++) std::swap(m[c * n + j], m[best * n + j]);
                std::swap(perm[c], perm[best]);
                det = -det;
            }
            det *= m[c * n + c];
            for (size_t r = c + 1; r < n; r++) {
                const double f = m[r * n + c] /= m[c * n + c];
                for (size_t j = c + 1; j < n; j++) m[r * n + j] -= f * m[c * n + j];
            }
        }
        if (op == 2) return Value::number(singular ? 0.0 : det);
        if (singular) fail(vm, Thrown::ArithmeticException, "the matrix has no inverse: its determinant is 0");
        // Solves for each column of the right-hand side: the identity's for an inverse.
        const View out = viewOf(vm, args[9], args[10], args[11], args[12]);
        View b;
        size_t columns = n;
        if (op == 1) {
            b = viewOf(vm, args[5], args[6], args[7], args[8]);
            if (b.shape[0] != static_cast<int64_t>(n)) fail(vm, Thrown::IllegalArgumentException, "b has " + std::to_string(b.shape[0]) + " rows, not " + std::to_string(n));
            columns = b.rank() == 1 ? 1 : static_cast<size_t>(b.shape[1]);
        }
        std::vector<double> x(n);
        for (size_t col = 0; col < columns; col++) {
            for (size_t i = 0; i < n; i++) {
                const size_t from = perm[i];
                x[i] = op == 0 ? (from == col ? 1.0 : 0.0)
                               : readF(b, b.offset + static_cast<int64_t>(from) * b.strides[0] + (b.rank() == 1 ? 0 : static_cast<int64_t>(col) * b.strides[1]));
            }
            for (size_t i = 0; i < n; i++)
                for (size_t k = 0; k < i; k++) x[i] -= m[i * n + k] * x[k];
            for (size_t i = n; i-- > 0;) {
                for (size_t k = i + 1; k < n; k++) x[i] -= m[i * n + k] * x[k];
                x[i] /= m[i * n + i];
            }
            for (size_t i = 0; i < n; i++) {
                const int64_t at = out.offset + static_cast<int64_t>(i) * out.strides[0] + (out.rank() == 1 ? 0 : static_cast<int64_t>(col) * out.strides[1]);
                writeF(vm, out, at, x[i]);
            }
        }
        return Value::null();
    });

    // fillWith(out..., kind, p, q): 0 numbers from p a step of q apart; 1 floats from 0 to 1 by
    // the generator seeded by p (splitmix64), the same ones for the same seed.
    def("fillWith", 7, 7, [](VM& vm, int, Value* args) -> Value {
        const View out = viewOf(vm, args[0], args[1], args[2], args[3]);
        const int kind = static_cast<int>(vm.toInt(args[4], "kind"));
        int64_t k = 0;
        uint64_t state = static_cast<uint64_t>(kind == 1 ? vm.toInt(args[5], "seed") : 0);
        const bool ints = !out.isFloat && args[5].isInt() && args[6].isInt();
        walk(out.shape, {out.strides}, {out.offset}, [&](const int64_t* at) {
            if (kind == 1) {
                uint64_t z = (state += 0x9E3779B97F4A7C15ull);
                z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
                z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
                z ^= z >> 31;
                writeF(vm, out, at[0], static_cast<double>(z >> 11) / 9007199254740992.0);
            } else if (ints) {
                writeI(vm, out, at[0], args[5].as.i + k * args[6].as.i);
            } else {
                writeF(vm, out, at[0], vm.toNumber(args[5], "from") + static_cast<double>(k) * vm.toNumber(args[6], "step"));
            }
            k++;
        });
        return Value::null();
    });

    // fromBytes(bytes, out): bytes as the items of a packed List<byte> (a picture's pixels as a
    // tensor), and toBytes(list): the other way.
    def("fromBytes", 2, 2, [](VM& vm, int, Value* args) -> Value {
        if (!args[0].isObjType(ObjType::Bytes) || !args[1].isObjType(ObjType::List) || !asList(args[1])->packed() || asList(args[1])->pack->width != 1)
            fail(vm, Thrown::ClassCastException, "bytes into a list of bytes");
        PackedItems& p = *asList(args[1])->pack;
        const std::vector<uint8_t>& data = asBytes(args[0])->data;
        if (p.count != data.size()) fail(vm, Thrown::IllegalArgumentException, "the sizes differ");
        std::memcpy(p.block.data(), data.data(), data.size());
        return Value::null();
    });

    // takeBytes(cells, data): a packed list's bytes, one after another, as the items of an
    // empty list of bytes (Grid<Color>.toTensor: the pixels copied once, never set to zero first).
    def("takeBytes", 2, 2, [](VM& vm, int, Value* args) -> Value {
        if (!args[0].isObjType(ObjType::List) || !asList(args[0])->packed() || !args[1].isObjType(ObjType::List) || !asList(args[1])->packed() ||
            asList(args[1])->pack->width != 1 || asList(args[1])->pack->count != 0 || asList(args[1])->pack->nullable)
            fail(vm, Thrown::ClassCastException, "a packed list's bytes into an empty list of bytes");
        const PackedItems& from = *asList(args[0])->pack;
        PackedItems& to = *asList(args[1])->pack;
        const size_t n = from.count * from.width;
        to.block.assign(from.block.data(), from.block.data() + n);
        to.count = n;
        vm.account(n);
        return Value::null();
    });

    vm.defineGlobal("__tensor", Value::object(lib));
}

}  // namespace ww
