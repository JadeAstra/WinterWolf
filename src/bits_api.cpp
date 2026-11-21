// What BitReader, HuffmanTable and Bytes.inflate and deflate (src/std/bits.ww) are built on:
// bits read from any place in bytes, either order; a Huffman code decoded at a place; and
// codec.cpp's deflate both ways.

#include <algorithm>
#include <bit>
#include <array>
#include <cstring>
#include <string>
#include <vector>

#if (defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)) && !defined(WW_NO_SSE2)
#define WW_SSE2_SCAN 1
#include <emmintrin.h>
#endif

#include "builtins.h"
#include "codec.h"
#include "vm.h"

namespace ww {

namespace {

// `n` bits (0 to 32) from bit `position` of the bytes: the first bit in a byte its lowest
// (lsb first, as deflate, GIF and WebP's lossless form have them) or its highest (msb first,
// as JPEG has them), the first bit read the value's lowest or highest the same way. Bits past
// the end are zeros.
uint64_t bitsAt(const std::vector<uint8_t>& data, int64_t position, int n, bool msbFirst) {
    if (n == 0) return 0;
    const size_t size = data.size();
    size_t byte = static_cast<size_t>(position >> 3);
    const int shift = static_cast<int>(position & 7);
    // Eight bytes from `byte`, zeros past the end.
    uint64_t window = 0;
    if (byte + 8 <= size) {
        std::memcpy(&window, data.data() + byte, 8);
    } else {
        for (int k = 0; k < 8 && byte + static_cast<size_t>(k) < size; k++) window |= static_cast<uint64_t>(data[byte + static_cast<size_t>(k)]) << (8 * k);
    }
    if (!msbFirst) return (window >> shift) & ((uint64_t{1} << n) - 1);
    // Most significant first: the bytes in the other order, as one big-endian number.
    uint64_t big = 0;
    for (int k = 0; k < 8; k++) big = (big << 8) | ((window >> (8 * k)) & 0xFF);
    return (big << shift) >> (64 - n);
}

// A Huffman code: its symbols in the order of their codes, by length, and a table of the next
// 10 bits each way for the codes no longer than that.
struct HuffmanCode {
    static constexpr int kFast = 10;
    static constexpr int kLongest = 16;
    uint16_t counts[kLongest + 1] = {};
    uint32_t firstCode[kLongest + 1] = {};
    uint16_t firstIndex[kLongest + 1] = {};
    std::vector<int32_t> symbols;          // in the order of their codes
    std::vector<uint32_t> fastMsb, fastLsb;  // (symbol << 5) | length, 0 where none
};

void releaseHuffman(int64_t id) { delete reinterpret_cast<HuffmanCode*>(id); }

const HuffmanCode& huffmanOf(VM& vm, const Value& v) {
    if (!v.isObjType(ObjType::Resource) || std::strcmp(static_cast<ObjResource*>(v.as.obj)->kind, "huffman") != 0)
        throw vm.runtimeError(Thrown::IllegalArgumentException, "not a Huffman table");
    return *reinterpret_cast<const HuffmanCode*>(static_cast<ObjResource*>(v.as.obj)->id);
}

uint32_t reversed(uint32_t code, int length) {
    uint32_t r = 0;
    for (int i = 0; i < length; i++) r |= ((code >> i) & 1) << (length - 1 - i);
    return r;
}

const std::vector<uint8_t>& bytesOf(VM& vm, const Value& v) {
    if (!v.isObjType(ObjType::Bytes)) throw vm.runtimeError(Thrown::IllegalArgumentException, "bits are read from Bytes, not a value of type " + vm.typeName(v));
    return asBytes(v)->data;
}

Value bytesValue(VM& vm, std::vector<uint8_t>&& data) {
    vm.checkRoomFor(data.size());
    ObjBytes* made = vm.alloc<ObjBytes>();
    made->data = std::move(data);
    vm.account(made->data.size());
    return Value::object(made);
}


// ---- JPEG's scans (the only part of JPEG in C++: its Huffman-coded runs and sizes) ----

constexpr int kNatural[64] = {0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48,
                              41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23,
                              30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

// A scan's coded bits, highest first, an FF followed by 00 read as FF; at a marker, zeros.
struct JpegBits {
    const uint8_t* d;
    size_t size;
    size_t pos;
    uint64_t buffer = 0;
    int count = 0;
    bool atMarker = false;

    void fill() {
        while (count <= 56) {
            uint32_t byte = 0;
            if (!atMarker && pos < size) {
                byte = d[pos];
                if (byte == 0xFF) {
                    const uint8_t next = pos + 1 < size ? d[pos + 1] : 0xD9;
                    if (next == 0x00) {
                        pos += 2;
                    } else {
                        atMarker = true;
                        byte = 0;
                    }
                } else {
                    pos++;
                }
            }
            buffer |= static_cast<uint64_t>(byte) << (56 - count);
            count += 8;
        }
    }
    uint32_t get(int n) {
        if (n == 0) return 0;
        if (count < n) fill();
        const uint32_t v = static_cast<uint32_t>(buffer >> (64 - n));
        buffer <<= n;
        count -= n;
        return v;
    }
    // Past a restart marker: the bits left of the byte dropped, the marker passed (looked for
    // further on where it is not straight after).
    void restart() {
        buffer = 0;
        count = 0;
        atMarker = false;
        while (pos + 1 < size) {
            if (d[pos] == 0xFF && d[pos + 1] >= 0xD0 && d[pos + 1] <= 0xD7) {
                pos += 2;
                return;
            }
            if (d[pos] == 0xFF && d[pos + 1] != 0x00 && d[pos + 1] != 0xFF) return;  // another marker: left to the end
            pos++;
        }
    }
};

int decodeMsb(const HuffmanCode& code, JpegBits& b) {
    if (b.count < 16) b.fill();
    const uint32_t bits = static_cast<uint32_t>(b.buffer >> 48);
    const uint32_t entry = code.fastMsb[bits >> (HuffmanCode::kLongest - HuffmanCode::kFast)];
    if (entry != 0) {
        b.buffer <<= (entry & 31);
        b.count -= static_cast<int>(entry & 31);
        return static_cast<int>(entry >> 5);
    }
    uint32_t c = bits >> (HuffmanCode::kLongest - HuffmanCode::kFast);
    for (int length = HuffmanCode::kFast + 1; length <= HuffmanCode::kLongest; length++) {
        c = (c << 1) | ((bits >> (HuffmanCode::kLongest - length)) & 1);
        const uint32_t offset = c - code.firstCode[length];
        if (offset < code.counts[length]) {
            b.buffer <<= length;
            b.count -= length;
            return code.symbols[code.firstIndex[length] + offset];
        }
    }
    return -1;
}

inline int extendJpeg(uint32_t v, int s) { return s == 0 ? 0 : v < (1u << (s - 1)) ? static_cast<int>(v) - (1 << s) + 1 : static_cast<int>(v); }

struct ScanComponent {
    uint8_t* coefs;  // 16 bits a coefficient, 64 a block, natural order
    int64_t blocksW, h, v, usedW, usedH;
    const HuffmanCode* dc;
    const HuffmanCode* ac;
    int pred = 0;
};

inline int16_t coefAt(const uint8_t* c, int i) {
    int16_t v;
    std::memcpy(&v, c + i * 2, 2);
    return v;
}
inline void setCoef(uint8_t* c, int i, int v) {
    const int16_t s = static_cast<int16_t>(std::clamp(v, -32768, 32767));
    std::memcpy(c + i * 2, &s, 2);
}

// One block of a scan; false for bits that are no code.
bool scanBlock(ScanComponent& c, JpegBits& b, int64_t bx, int64_t by, bool progressive, int first, int last, int high, int low, int& eobrun) {
    uint8_t* blk = c.coefs + (by * c.blocksW + bx) * 128;
    if (!progressive || (first == 0 && high == 0)) {
        if (!progressive || first == 0) {
            const int t = decodeMsb(*c.dc, b);
            if (t < 0 || t > 16) return false;
            c.pred += extendJpeg(b.get(t), t);
            setCoef(blk, 0, progressive ? c.pred * (1 << low) : c.pred);
        }
        if (progressive) return true;
        for (int k = 1; k < 64;) {
            const int rs = decodeMsb(*c.ac, b);
            if (rs < 0) return false;
            const int s = rs & 15, run = rs >> 4;
            if (s == 0) {
                if (run != 15) break;
                k += 16;
                continue;
            }
            k += run;
            if (k > 63) return false;
            setCoef(blk, kNatural[k], extendJpeg(b.get(s), s));
            k++;
        }
        return true;
    }
    if (first == 0) {  // a DC refined by a bit
        if (b.get(1)) setCoef(blk, 0, coefAt(blk, 0) | (1 << low));
        return true;
    }
    if (high == 0) {  // a band's first pass
        if (eobrun > 0) {
            eobrun--;
            return true;
        }
        for (int k = first; k <= last;) {
            const int rs = decodeMsb(*c.ac, b);
            if (rs < 0) return false;
            const int s = rs & 15, run = rs >> 4;
            if (s == 0) {
                if (run < 15) {
                    eobrun = (1 << run) - 1;
                    if (run > 0) eobrun += static_cast<int>(b.get(run));
                    return true;
                }
                k += 16;
                continue;
            }
            k += run;
            if (k > 63) return false;
            setCoef(blk, kNatural[k], extendJpeg(b.get(s), s) * (1 << low));
            k++;
        }
        return true;
    }
    // A refining pass: a bit more of each coefficient already there, new ones of the least size.
    const int plus = 1 << low, minus = -1 * (1 << low);
    int k = first;
    auto refine = [&](int at) {
        const int v = coefAt(blk, at);
        if (b.get(1) && (v & plus) == 0) setCoef(blk, at, v + (v >= 0 ? plus : minus));
    };
    if (eobrun == 0) {
        for (; k <= last; k++) {
            const int rs = decodeMsb(*c.ac, b);
            if (rs < 0) return false;
            const int s = rs & 15;
            int run = rs >> 4;
            int value = 0;
            if (s != 0) {
                value = b.get(1) ? plus : minus;
            } else if (run != 15) {
                eobrun = 1 << run;
                if (run > 0) eobrun += static_cast<int>(b.get(run));
                break;
            }
            while (k <= last) {
                const int at = kNatural[k];
                if (coefAt(blk, at) != 0) {
                    refine(at);
                } else {
                    if (--run < 0) break;
                }
                k++;
            }
            if (value != 0 && k <= last) setCoef(blk, kNatural[k], value);
        }
    }
    if (eobrun > 0) {
        for (; k <= last; k++)
            if (coefAt(blk, kNatural[k]) != 0) refine(kNatural[k]);
        eobrun--;
    }
    return true;
}
}  // namespace

void registerBits(VM& vm) {
    ObjClass* holder = vm.alloc<ObjClass>(vm.intern("Bits"));
    auto def = [&](const char* name, int minArgs, int maxArgs, NativeFn fn) {
        holder->statics.set(vm.intern(name), Value::object(vm.alloc<ObjNative>(std::string("Bits.") + name, minArgs, maxArgs, fn)));
    };

    // n bits at a bit position: peek(bytes, position, n, msbFirst).
    def("peek", 4, 4, [](VM& vm, int, Value* args) -> Value {
        const std::vector<uint8_t>& data = bytesOf(vm, args[0]);
        const int64_t position = vm.toInt(args[1], "position");
        const int64_t n = vm.toInt(args[2], "count");
        if (n < 0 || n > 32) throw vm.runtimeError(Thrown::IllegalArgumentException, "bits are read 0 to 32 at a time, not " + std::to_string(n));
        if (position < 0) throw vm.runtimeError(Thrown::IndexOutOfBoundsException, "bit position " + std::to_string(position) + " is before the start");
        return Value::integer(static_cast<int64_t>(bitsAt(data, position, static_cast<int>(n), args[3].truthy())));
    });

    // A Huffman table from each symbol's code length, the codes as deflate numbers them; or, with
    // `symbols`, from the lengths in the order the codes go, each code's symbol given, as JPEG
    // has them. A string, what is wrong, where the lengths make no code.
    def("huffman", 2, 2, [](VM& vm, int, Value* args) -> Value {
        if (!args[0].isObjType(ObjType::List)) throw vm.runtimeError(Thrown::IllegalArgumentException, "a Huffman table is made from a list of lengths");
        ObjList* lengthList = asList(args[0]);
        ObjList* symbolList = args[1].isObjType(ObjType::List) ? asList(args[1]) : nullptr;
        const size_t n = lengthList->size();
        if (symbolList != nullptr && symbolList->size() != n)
            return vm.str("there are " + std::to_string(n) + " lengths but " + std::to_string(symbolList->size()) + " symbols");
        auto code = std::make_unique<HuffmanCode>();
        std::vector<int> lengths(n);
        std::vector<int32_t> symbolOf(n);
        for (size_t i = 0; i < n; i++) {
            const Value l = vm.listGet(lengthList, i);
            if (!l.isInt() || l.as.i < 0 || l.as.i > HuffmanCode::kLongest) return vm.str("a code length is 0 to 16, not " + vm.stringify(l));
            lengths[i] = static_cast<int>(l.as.i);
            if (symbolList != nullptr) {
                const Value s = vm.listGet(symbolList, i);
                if (!s.isInt() || s.as.i < 0 || s.as.i >= (int64_t{1} << 26)) return vm.str("a symbol is an int from 0 to 67108863, not " + vm.stringify(s));
                symbolOf[i] = static_cast<int32_t>(s.as.i);
            } else {
                symbolOf[i] = static_cast<int32_t>(i);
            }
            code->counts[lengths[i]]++;
        }
        code->counts[0] = 0;
        int64_t left = 1;
        for (int length = 1; length <= HuffmanCode::kLongest; length++) {
            left = left * 2 - code->counts[length];
            if (left < 0) return vm.str("more codes than the lengths have room for");
        }
        // Symbols by length, then in the order given (by symbol, for lengths alone).
        uint16_t next[HuffmanCode::kLongest + 1] = {};
        uint16_t index = 0;
        uint32_t first = 0;
        for (int length = 1; length <= HuffmanCode::kLongest; length++) {
            first = (first + code->counts[length - 1]) << 1;
            code->firstCode[length] = first;
            code->firstIndex[length] = index;
            next[length] = index;
            index = static_cast<uint16_t>(index + code->counts[length]);
        }
        code->symbols.assign(index, 0);
        code->fastMsb.assign(size_t{1} << HuffmanCode::kFast, 0);
        code->fastLsb.assign(size_t{1} << HuffmanCode::kFast, 0);
        for (size_t i = 0; i < n; i++) {
            const int length = lengths[i];
            if (length == 0) continue;
            const uint16_t at = next[length]++;
            code->symbols[at] = symbolOf[i];
            if (length > HuffmanCode::kFast) continue;
            const uint32_t c = code->firstCode[length] + (at - code->firstIndex[length]);
            const uint32_t entry = (static_cast<uint32_t>(symbolOf[i]) << 5) | static_cast<uint32_t>(length);
            const int spare = HuffmanCode::kFast - length;
            for (uint32_t k = 0; k < (1u << spare); k++) {
                code->fastMsb[(c << spare) | k] = entry;
                code->fastLsb[reversed(c, length) | (k << length)] = entry;
            }
        }
        HuffmanCode* kept = code.release();
        return Value::object(vm.alloc<ObjResource>("huffman", reinterpret_cast<int64_t>(kept), &releaseHuffman));
    });

    // The next code at a bit position: (symbol << 5) | its length, or -1 where the bits are none.
    def("decode", 4, 4, [](VM& vm, int, Value* args) -> Value {
        const HuffmanCode& code = huffmanOf(vm, args[0]);
        const std::vector<uint8_t>& data = bytesOf(vm, args[1]);
        const int64_t position = vm.toInt(args[2], "position");
        if (position < 0) throw vm.runtimeError(Thrown::IndexOutOfBoundsException, "bit position " + std::to_string(position) + " is before the start");
        const bool msbFirst = args[3].truthy();
        const uint32_t bits = static_cast<uint32_t>(bitsAt(data, position, HuffmanCode::kLongest, msbFirst));
        // The first 10 bits, in the order the code's bits go.
        const uint32_t head = msbFirst ? bits >> (HuffmanCode::kLongest - HuffmanCode::kFast) : bits & ((1u << HuffmanCode::kFast) - 1);
        const uint32_t entry = msbFirst ? code.fastMsb[head] : code.fastLsb[head];
        if (entry != 0) return Value::integer(static_cast<int64_t>(entry));
        uint32_t c = msbFirst ? head : reversed(head, HuffmanCode::kFast);
        for (int length = HuffmanCode::kFast + 1; length <= HuffmanCode::kLongest; length++) {
            const uint32_t bit = msbFirst ? (bits >> (HuffmanCode::kLongest - length)) & 1 : (bits >> (length - 1)) & 1;
            c = (c << 1) | bit;
            const uint32_t offset = c - code.firstCode[length];
            if (offset < code.counts[length])
                return Value::integer((static_cast<int64_t>(code.symbols[code.firstIndex[length] + offset]) << 5) | length);
        }
        return Value::integer(-1);
    });

    // Deflate both ways: inflate(bytes, raw, limit) gives Bytes, or a string saying what is wrong.
    def("inflate", 3, 3, [](VM& vm, int, Value* args) -> Value {
        const std::vector<uint8_t>& data = bytesOf(vm, args[0]);
        int64_t limit = vm.toInt(args[2], "limit");  // -1: none
        if (limit < -1) throw vm.runtimeError(Thrown::IllegalArgumentException, "the limit of inflate() is 0 or more, not " + std::to_string(limit));
        if (limit == -1) limit = INT64_MAX;
        std::vector<uint8_t> out;
        std::string error;
        if (!codec::inflate(data.data(), data.size(), out, static_cast<size_t>(limit), error, args[1].truthy())) return vm.str(error);
        return bytesValue(vm, std::move(out));
    });
    // inflateCounting(bytes, offset, limit): raw deflate data from `offset` expanded, and where
    // it ended, as [Bytes, end]; or the error as text. For gzip, whose members follow one another.
    def("inflateCounting", 3, 3, [](VM& vm, int, Value* args) -> Value {
        const std::vector<uint8_t>& data = bytesOf(vm, args[0]);
        const int64_t offset = vm.toInt(args[1], "offset");
        int64_t limit = vm.toInt(args[2], "limit");
        if (offset < 0 || offset > static_cast<int64_t>(data.size())) throw vm.runtimeError(Thrown::IndexOutOfBoundsException, "offset " + std::to_string(offset) + " out of the bytes");
        if (limit < 0) limit = INT64_MAX;
        std::vector<uint8_t> out;
        std::string error;
        size_t used = 0;
        if (!codec::inflate(data.data() + offset, data.size() - static_cast<size_t>(offset), out, static_cast<size_t>(limit), error, true, &used)) return vm.str(error);
        Value made = bytesValue(vm, std::move(out));
        TempRoot keep(vm, made);
        return Value::object(newList(vm, {made, Value::integer(offset + static_cast<int64_t>(used))}));
    });
    def("deflate", 3, 3, [](VM& vm, int, Value* args) -> Value {
        const std::vector<uint8_t>& data = bytesOf(vm, args[0]);
        const int64_t level = vm.toInt(args[1], "level");
        if (level < 0 || level > 9) throw vm.runtimeError(Thrown::IllegalArgumentException, "deflate() takes a level from 0 to 9, not " + std::to_string(level));
        return bytesValue(vm, codec::deflate(data.data(), data.size(), static_cast<int>(level), args[2].truthy()));
    });
    def("crc32", 2, 2, [](VM& vm, int, Value* args) -> Value {
        const std::vector<uint8_t>& data = bytesOf(vm, args[0]);
        return Value::integer(codec::crc32(data.data(), data.size(), static_cast<uint32_t>(vm.toInt(args[1], "crc"))));
    });
    def("adler32", 2, 2, [](VM& vm, int, Value* args) -> Value {
        const std::vector<uint8_t>& data = bytesOf(vm, args[0]);
        return Value::integer(codec::adler32(data.data(), data.size(), static_cast<uint32_t>(vm.toInt(args[1], "adler"))));
    });

    vm.defineGlobal("__bits", Value::object(holder));

    // ---- JPEG's scans ----
    {
        ObjClass* jpeg = vm.alloc<ObjClass>(vm.intern("Jpeg"));
        auto jdef = [&](const char* name, int minArgs, int maxArgs, NativeFn fn) {
            jpeg->statics.set(vm.intern(name), Value::object(vm.alloc<ObjNative>(std::string("Jpeg.") + name, minArgs, maxArgs, fn)));
        };
        // decodeScan(data, at, components, mcusX, mcusY, restartInterval, progressive, first, last,
        // high, low): the scan's coefficients into each component's; gives where the next marker
        // is, or a string, what is wrong. Each component: [coefficients, blocksW, h, v, usedW,
        // usedH, dc table, ac table].
        jdef("decodeScan", 11, 11, [](VM& vm, int, Value* args) -> Value {
            const std::vector<uint8_t>& data = bytesOf(vm, args[0]);
            const int64_t at = vm.toInt(args[1], "at");
            if (!args[2].isObjType(ObjType::List)) throw vm.runtimeError(Thrown::IllegalArgumentException, "a scan's components are a list");
            ObjList* list = asList(args[2]);
            std::vector<ScanComponent> comps;
            for (size_t i = 0; i < list->size(); i++) {
                const Value cv = vm.listGet(list, i);
                if (!cv.isObjType(ObjType::List) || asList(cv)->size() != 8) throw vm.runtimeError(Thrown::IllegalArgumentException, "a scan's component is a list of 8");
                ObjList* c = asList(cv);
                ScanComponent sc{};
                const Value coefs = vm.listGet(c, 0);
                if (!coefs.isObjType(ObjType::Bytes)) throw vm.runtimeError(Thrown::IllegalArgumentException, "a component's coefficients are Bytes");
                sc.coefs = asBytes(coefs)->data.data();
                sc.blocksW = vm.toInt(vm.listGet(c, 1), "blocksW");
                sc.h = vm.toInt(vm.listGet(c, 2), "h");
                sc.v = vm.toInt(vm.listGet(c, 3), "v");
                sc.usedW = vm.toInt(vm.listGet(c, 4), "usedW");
                sc.usedH = vm.toInt(vm.listGet(c, 5), "usedH");
                const Value dc = vm.listGet(c, 6), ac = vm.listGet(c, 7);
                sc.dc = dc.isNull() ? nullptr : &huffmanOf(vm, dc);
                sc.ac = ac.isNull() ? nullptr : &huffmanOf(vm, ac);
                const size_t needed = static_cast<size_t>(sc.blocksW * std::max<int64_t>(sc.usedH, sc.v) * 128);
                if (asBytes(coefs)->data.size() < needed) throw vm.runtimeError(Thrown::IllegalArgumentException, "a component's coefficients are fewer than its blocks");
                comps.push_back(sc);
            }
            const int64_t mcusX = vm.toInt(args[3], "mcusX"), mcusY = vm.toInt(args[4], "mcusY"), interval = vm.toInt(args[5], "restartInterval");
            const bool progressive = args[6].truthy();
            const int first = static_cast<int>(vm.toInt(args[7], "first")), last = static_cast<int>(vm.toInt(args[8], "last"));
            const int high = static_cast<int>(vm.toInt(args[9], "high")), low = static_cast<int>(vm.toInt(args[10], "low"));
            for (const ScanComponent& c : comps) {
                if ((!progressive || (first == 0 && high == 0)) && c.dc == nullptr) return vm.str("a scan uses a DC table not given");
                if ((!progressive || first > 0) && c.ac == nullptr) return vm.str("a scan uses an AC table not given");
            }
            JpegBits b{data.data(), data.size(), static_cast<size_t>(at)};
            int eobrun = 0;
            int64_t left = interval;
            const bool single = comps.size() == 1;
            const int64_t unitsX = single ? comps[0].usedW : mcusX, unitsY = single ? comps[0].usedH : mcusY;
            for (int64_t uy = 0; uy < unitsY; uy++)
                for (int64_t ux = 0; ux < unitsX; ux++) {
                    if (interval > 0) {
                        if (left == 0) {
                            b.restart();
                            for (ScanComponent& c : comps) c.pred = 0;
                            eobrun = 0;
                            left = interval;
                        }
                        left--;
                    }
                    if (single) {
                        if (!scanBlock(comps[0], b, ux, uy, progressive, first, last, high, low, eobrun)) return vm.str("damaged data: bits that are no code");
                    } else {
                        for (ScanComponent& c : comps)
                            for (int64_t by = 0; by < c.v; by++)
                                for (int64_t bx = 0; bx < c.h; bx++)
                                    if (!scanBlock(c, b, ux * c.h + bx, uy * c.v + by, progressive, first, last, high, low, eobrun))
                                        return vm.str("damaged data: bits that are no code");
                    }
                }
            // The next marker: where the reading stopped, or further on.
            size_t p = b.pos;
            while (p + 1 < data.size() && !(data[p] == 0xFF && data[p + 1] != 0x00 && data[p + 1] != 0xFF && (data[p + 1] < 0xD0 || data[p + 1] > 0xD7))) p++;
            return Value::integer(static_cast<int64_t>(p + 1 < data.size() ? p : data.size()));
        });
        // encodeScan(components, mcusX, mcusY): a baseline scan of the components' coefficients,
        // interleaved, as coded bytes (FF followed by 00), padded with ones. Each component:
        // [coefficients, blocksW, h, v, dc codes, dc lengths, ac codes, ac lengths].
        jdef("encodeScan", 3, 3, [](VM& vm, int, Value* args) -> Value {
            if (!args[0].isObjType(ObjType::List)) throw vm.runtimeError(Thrown::IllegalArgumentException, "a scan's components are a list");
            ObjList* list = asList(args[0]);
            struct Comp {
                const uint8_t* coefs;
                int64_t blocksW, h, v;
                std::array<uint32_t, 256> dcCode, dcLength, acCode, acLength;
                int pred = 0;
            };
            std::vector<Comp> comps(list->size());
            const int64_t mcusX = vm.toInt(args[1], "mcusX"), mcusY = vm.toInt(args[2], "mcusY");
            for (size_t i = 0; i < list->size(); i++) {
                ObjList* c = asList(vm.listGet(list, i));
                Comp& k = comps[i];
                const Value coefs = vm.listGet(c, 0);
                k.coefs = asBytes(coefs)->data.data();
                k.blocksW = vm.toInt(vm.listGet(c, 1), "blocksW");
                k.h = vm.toInt(vm.listGet(c, 2), "h");
                k.v = vm.toInt(vm.listGet(c, 3), "v");
                if (asBytes(coefs)->data.size() < static_cast<size_t>(k.blocksW * mcusY * k.v * 128)) throw vm.runtimeError(Thrown::IllegalArgumentException, "a component's coefficients are fewer than its blocks");
                auto table = [&](int index, std::array<uint32_t, 256>& into) {
                    ObjList* t = asList(vm.listGet(c, static_cast<size_t>(index)));
                    for (size_t s = 0; s < 256; s++) into[s] = static_cast<uint32_t>(vm.listGet(t, s).as.i);
                };
                table(4, k.dcCode);
                table(5, k.dcLength);
                table(6, k.acCode);
                table(7, k.acLength);
            }
            // The bits go 64 at a time through `acc`, out 32 at a time, an FF followed by 00; the
            // output kept 512 bytes ahead of a block, more than a block can take.
            std::vector<uint8_t> out(1 << 16);
            size_t o = 0;
            uint64_t acc = 0;
            int count = 0;
            auto room = [&] {
                if (out.size() - o < 512) out.resize(out.size() * 2);
            };
            auto flush32 = [&] {
                count -= 32;
                const uint32_t w = static_cast<uint32_t>(acc >> count);
                const uint32_t t = ~w;
                uint8_t* d = out.data() + o;
                if (((t - 0x01010101u) & ~t & 0x80808080u) == 0) {  // no byte of FF
                    d[0] = static_cast<uint8_t>(w >> 24), d[1] = static_cast<uint8_t>(w >> 16), d[2] = static_cast<uint8_t>(w >> 8), d[3] = static_cast<uint8_t>(w);
                    o += 4;
                } else {
                    for (int k = 24; k >= 0; k -= 8) {
                        const uint8_t byte = static_cast<uint8_t>(w >> k);
                        out[o++] = byte;
                        if (byte == 0xFF) out[o++] = 0;
                    }
                }
            };
            // At most 27 bits: a code of 16 and 11 more.
            auto put = [&](uint32_t value, uint32_t length) {
                acc = (acc << length) | value;
                count += static_cast<int>(length);
                if (count >= 32) flush32();
            };
            // The bits a value takes, and the bits written for it (one less, below 0).
            auto magnitude = [](int v) { return static_cast<uint32_t>(std::bit_width(static_cast<uint32_t>(v < 0 ? -v : v))); };
            auto valueBits = [](int v, uint32_t s) { return static_cast<uint32_t>(v < 0 ? v - 1 : v) & ((1u << s) - 1); };
            for (int64_t my = 0; my < mcusY; my++)
                for (int64_t mx = 0; mx < mcusX; mx++)
                    for (Comp& c : comps)
                        for (int64_t by = 0; by < c.v; by++)
                            for (int64_t bx = 0; bx < c.h; bx++) {
                                room();
                                const uint8_t* blk = c.coefs + ((my * c.v + by) * c.blocksW + mx * c.h + bx) * 128;
                                int16_t zz[64];
                                for (int k = 0; k < 64; k++) zz[k] = coefAt(blk, kNatural[k]);
                                const int dc = zz[0];
                                const int diff = dc - c.pred;
                                c.pred = dc;
                                const uint32_t ds = magnitude(diff);
                                put((c.dcCode[ds] << ds) | valueBits(diff, ds), c.dcLength[ds] + ds);
                                // Which of the 63 others are not 0, a bit each.
                                uint64_t nonzero = 0;
#ifdef WW_SSE2_SCAN
                                const __m128i zero = _mm_setzero_si128();
                                for (int k = 0; k < 64; k += 16) {
                                    const __m128i a = _mm_cmpeq_epi16(_mm_loadu_si128(reinterpret_cast<const __m128i*>(zz + k)), zero);
                                    const __m128i b = _mm_cmpeq_epi16(_mm_loadu_si128(reinterpret_cast<const __m128i*>(zz + k + 8)), zero);
                                    nonzero |= static_cast<uint64_t>(~_mm_movemask_epi8(_mm_packs_epi16(a, b)) & 0xFFFF) << k;
                                }
#else
                                for (int k = 0; k < 64; k++) nonzero |= static_cast<uint64_t>(zz[k] != 0) << k;
#endif
                                nonzero &= ~uint64_t{1};
                                int last = 0;
                                while (nonzero != 0) {
                                    const int k = std::countr_zero(nonzero);
                                    nonzero &= nonzero - 1;
                                    int run = k - last - 1;
                                    last = k;
                                    while (run > 15) {
                                        put(c.acCode[0xF0], c.acLength[0xF0]);
                                        run -= 16;
                                    }
                                    const int v = zz[k];
                                    const uint32_t sz = magnitude(v);
                                    const uint32_t symbol = (static_cast<uint32_t>(run) << 4) | sz;
                                    put((c.acCode[symbol] << sz) | valueBits(v, sz), c.acLength[symbol] + sz);
                                }
                                if (last < 63) put(c.acCode[0], c.acLength[0]);
                            }
            // The last bits, padded with ones to a byte.
            room();
            while (count >= 8) {
                count -= 8;
                const uint8_t byte = static_cast<uint8_t>(acc >> count);
                out[o++] = byte;
                if (byte == 0xFF) out[o++] = 0;
            }
            if (count > 0) {
                const uint8_t byte = static_cast<uint8_t>(((acc << (8 - count)) | ((1u << (8 - count)) - 1)) & 0xFF);
                out[o++] = byte;
                if (byte == 0xFF) out[o++] = 0;
            }
            out.resize(o);
            return bytesValue(vm, std::move(out));
        });
        vm.defineGlobal("__jpeg", Value::object(jpeg));
    }

    // ---- GIF's LZW (the only part of GIF in C++, the user's choice of 2026-11-08) ----
    {
        ObjClass* gif = vm.alloc<ObjClass>(vm.intern("Gif"));
        // decodeLzw(data, at, minSize, out): a picture's codes, in the blocks starting at `at` (each
        // its length, then that many bytes, to an empty one), from minSize + 1 bits up to 12, into
        // the indices of `out`; how many came, or a string, what is wrong. The data ending before
        // its end code keeps what came; codes past `out` are dropped; codes after the table is
        // full go on with it as it is.
        gif->statics.set(vm.intern("decodeLzw"), Value::object(vm.alloc<ObjNative>("Gif.decodeLzw", 4, 4, [](VM& vm, int, Value* args) -> Value {
            const std::vector<uint8_t>& data = bytesOf(vm, args[0]);
            const int64_t at = vm.toInt(args[1], "at"), minSize = vm.toInt(args[2], "minSize");
            if (!args[3].isObjType(ObjType::Bytes)) throw vm.runtimeError(Thrown::IllegalArgumentException, "indices are written into Bytes");
            std::vector<uint8_t>& outBytes = asBytes(args[3])->data;
            if (at < 0 || at > static_cast<int64_t>(data.size()) || minSize < 1 || minSize > 11) throw vm.runtimeError(Thrown::IllegalArgumentException, "no LZW data there");
            const uint8_t* d = data.data();
            const size_t size = data.size();
            size_t pos = static_cast<size_t>(at), blockLeft = 0;
            bool ended = false;
            uint64_t buffer = 0;
            int bits = 0;
            // The next byte of the blocks, or false at their end.
            auto nextByte = [&](uint8_t& byte) {
                while (blockLeft == 0) {
                    if (ended || pos >= size || d[pos] == 0) {
                        ended = true;
                        return false;
                    }
                    blockLeft = d[pos++];
                }
                if (pos >= size) {
                    ended = true;
                    return false;
                }
                byte = d[pos++];
                blockLeft--;
                return true;
            };
            const int clear = 1 << minSize, end = clear + 1;
            // Each entry is a string already written: where in `out` it starts, and its length.
            uint32_t start[4096], length[4096];
            for (int i = 0; i < clear; i++) length[i] = 1;
            int codeSize = static_cast<int>(minSize) + 1, next = end + 1, old = -1;
            uint32_t mask = (1u << codeSize) - 1, oldAt = 0;
            uint8_t* out = outBytes.data();
            const size_t total = outBytes.size();
            size_t o = 0;
            for (;;) {
                while (bits < codeSize) {
                    uint8_t byte;
                    if (!nextByte(byte)) return Value::integer(static_cast<int64_t>(o));
                    buffer |= static_cast<uint64_t>(byte) << bits;
                    bits += 8;
                }
                const int code = static_cast<int>(buffer & mask);
                buffer >>= codeSize;
                bits -= codeSize;
                if (code == clear) {
                    codeSize = static_cast<int>(minSize) + 1;
                    mask = (1u << codeSize) - 1;
                    next = end + 1;
                    old = -1;
                    continue;
                }
                if (code == end) return Value::integer(static_cast<int64_t>(o));
                if (old < 0 && code > clear) return vm.str("its first code, " + std::to_string(code) + ", is not a colour");
                if (code > next || (code == next && next >= 4096)) return vm.str("an LZW code, " + std::to_string(code) + ", past those made");
                const bool made = code == next;
                if (old >= 0 && next < 4096) {
                    start[next] = oldAt;
                    length[next] = length[old] + 1;
                    next++;
                    if (static_cast<uint32_t>(next) == mask + 1 && codeSize < 12) {
                        codeSize++;
                        mask = (1u << codeSize) - 1;
                    }
                }
                if (o >= total) return Value::integer(static_cast<int64_t>(total));
                if (code < clear) {
                    out[o] = static_cast<uint8_t>(code);
                    oldAt = static_cast<uint32_t>(o);
                    o++;
                    old = code;
                    continue;
                }
                const size_t n = length[code], from = start[code];
                if (o + n > total) {
                    std::memmove(out + o, out + from, total - o);
                    return Value::integer(static_cast<int64_t>(total));
                }
                if (made) {
                    std::memcpy(out + o, out + from, n - 1);
                    out[o + n - 1] = out[from];
                } else {
                    std::memcpy(out + o, out + from, n);
                }
                oldAt = static_cast<uint32_t>(o);
                o += n;
                old = code;
            }
        })));
        vm.defineGlobal("__gif", Value::object(gif));
    }

}

}  // namespace ww
