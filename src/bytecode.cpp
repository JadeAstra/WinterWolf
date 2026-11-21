#include "bytecode.h"

#include <algorithm>
#include <cstring>
#include <iomanip>
#include <map>
#include <mutex>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <set>
#include <unordered_set>

#include "vm.h"

namespace ww {

namespace {

enum class Operand : uint8_t { None, U8, U16, I32, U16U8, U16I32, U16U16, U8U8, U16U16U8, U8I32 };

struct OpInfo {
    const char* name;
    Operand operand;
};

// Same order as the Op enum.
const OpInfo kOps[] = {
    {"CONST", Operand::U16},
    {"NULL", Operand::None},
    {"TRUE", Operand::None},
    {"FALSE", Operand::None},
    {"POP", Operand::None},
    {"DUP", Operand::None},
    {"DUP2", Operand::None},
    {"GET_LOCAL", Operand::U16},
    {"SET_LOCAL", Operand::U16},
    {"GET_GLOBAL", Operand::U16},
    {"SET_GLOBAL", Operand::U16},
    {"DEFINE_GLOBAL", Operand::U16},
    {"GET_UPVALUE", Operand::U16},
    {"SET_UPVALUE", Operand::U16},
    {"GET_PROP", Operand::U16U16},
    {"SET_PROP", Operand::U16U16},
    {"GET_INDEX", Operand::None},
    {"SET_INDEX", Operand::None},
    {"GET_SUPER", Operand::U16},
    {"SET_SUPER", Operand::U16},
    {"ADD", Operand::None},
    {"SUB", Operand::None},
    {"MUL", Operand::None},
    {"DIV", Operand::None},
    {"MOD", Operand::None},
    {"NEG", Operand::None},
    {"NOT", Operand::None},
    {"EQ", Operand::None},
    {"LT", Operand::None},
    {"LE", Operand::None},
    {"GT", Operand::None},
    {"GE", Operand::None},
    {"IS", Operand::None},
    {"JUMP", Operand::I32},
    {"LOOP", Operand::I32},
    {"JUMP_IF_FALSE", Operand::I32},
    {"AND", Operand::I32},
    {"OR", Operand::I32},
    {"CALL", Operand::U8},
    {"INVOKE", Operand::U16U8},
    {"SUPER_INVOKE", Operand::U16U8},
    {"CLOSURE", Operand::U16},
    {"CLOSE_UPVALUE", Operand::None},
    {"RETURN", Operand::None},
    {"CLASS", Operand::U16},
    {"INHERIT", Operand::U8},
    {"METHOD", Operand::U16},
    {"STATIC_METHOD", Operand::U16},
    {"BUILD_LIST", Operand::U16},
    {"BUILD_MAP", Operand::U16},
    {"BUILD_STRING", Operand::U16},
    {"RANGE", Operand::U8},
    {"ITER_INIT", Operand::None},
    {"ITER_NEXT", Operand::U16I32},
    {"TRY_BEGIN", Operand::I32},
    {"TRY_END", Operand::None},
    {"THROW", Operand::None},
    {"IMPORT", Operand::U16},
    {"CHECK_TYPE", Operand::U16U8},
    {"DEFINE_TYPED_GLOBAL", Operand::U16U8},
    {"CHECK_PARAM", Operand::U16U8},
    {"CHECK_RETURN", Operand::U8},
    {"DECLARE_FIELD", Operand::U16U16},
    {"FIELD_INIT", Operand::None},
    {"RETURN_INIT", Operand::None},
    {"SUPER_INIT", Operand::U8},
    {"DEFAULT_ARG", Operand::U16I32},
    {"CALL_NAMED", Operand::U8U8},
    {"GETTER", Operand::U16},
    {"SETTER", Operand::U16},
    {"UNSET", Operand::None},
    {"GET_UPVALUE_CHECKED", Operand::U16U16},
    {"SHL", Operand::None},
    {"SHR", Operand::None},
    {"BIT_AND", Operand::None},
    {"BIT_OR", Operand::None},
    {"BIT_XOR", Operand::None},
    {"BIT_NOT", Operand::None},
    {"GET_IMPLICIT", Operand::U16U16},
    {"SET_IMPLICIT", Operand::U16U16},
    {"IDENTICAL", Operand::None},
    {"JUMP_IF_NULL", Operand::I32},
    {"JUMP_IF_NOT_NULL", Operand::I32},
    {"ASSERT_NOT_NULL", Operand::None},
    {"SETTLE", Operand::U16},
    {"IS_TYPE", Operand::U8},
    {"CAST", Operand::U8},
    {"CAST_OR_NULL", Operand::U8},
    {"SWAP", Operand::None},
    {"POP_UNDER", Operand::U16},
    {"SPREAD", Operand::None},
    {"CALL_SPREAD", Operand::U8},
    {"STATIC_FIELD", Operand::U16U16},
    {"STATIC_GETTER", Operand::U16},
    {"STATIC_SETTER", Operand::U16},
    {"TRY_IMPLICIT", Operand::U16I32},
    {"INVOKE_IMPLICIT", Operand::U16U16U8},
    {"ENUM_ENTRY", Operand::U16U16},
    {"ENUM_SEAL", Operand::None},
    {"ENUM_NEXT", Operand::U16U16},
    {"PENDING_ENUM", Operand::U16},
    {"MARK_INTERFACE", Operand::None},
    {"REQUIRE", Operand::U16},
    {"FINISH_CLASS", Operand::None},
    {"DATA_FIELD", Operand::U16},
    {"UNSTABLE_KEY", Operand::None},
    {"LAZY", Operand::None},
    {"EXTEND", Operand::None},
    {"BRIDGE", Operand::U16},
    {"BRIDGE_METHOD", Operand::U16},
    {"COMPONENT", Operand::U8},
    {"MISSING_ARG", Operand::U16},
    {"TAIL_LOOP", Operand::U8},
    {"IMPLICIT_FIRST", Operand::U16U8},
    {"CALL_ON", Operand::U8},
    {"CLASS_INIT", Operand::None},
    {"ENSURE_CLASS_INIT", Operand::None},
    {"EXTENSION", Operand::U16U8},
    {"MARK_ABSTRACT", Operand::None},
    {"MARK_VALUE", Operand::U8},
    {"CHECK_OVERRIDE", Operand::U16U8},
    {"INVOKE_TRAILING", Operand::U16U8},
    {"SUPER_INVOKE_TRAILING", Operand::U16U8},
    {"CLASS_NUMBER", Operand::U16U16},
    {"MEMBER_ACCESS", Operand::U16U16U8},
    {"STATIC_ACCESS", Operand::U16U16U8},
    {"GET_OWN", Operand::U16U16},
    {"SET_OWN", Operand::U16U16},
    {"INVOKE_OWN", Operand::U16U8},
    {"INVOKE_OWN_TRAILING", Operand::U16U8},
    {"MAKE_FORMS", Operand::U16U8},
    {"METHOD_FORMS", Operand::U16U8},
    {"STATIC_FORMS", Operand::U16U8},
    {"GET_INDEX_OR", Operand::None},
    {"ADD_LOCAL", Operand::U16}, {"SUB_LOCAL", Operand::U16}, {"MUL_LOCAL", Operand::U16}, {"DIV_LOCAL", Operand::U16},
    {"MOD_LOCAL", Operand::U16}, {"LT_LOCAL", Operand::U16}, {"LE_LOCAL", Operand::U16}, {"GT_LOCAL", Operand::U16},
    {"GE_LOCAL", Operand::U16}, {"EQ_LOCAL", Operand::U16},
    {"ADD_CONST", Operand::U16}, {"SUB_CONST", Operand::U16}, {"MUL_CONST", Operand::U16}, {"DIV_CONST", Operand::U16},
    {"MOD_CONST", Operand::U16}, {"LT_CONST", Operand::U16}, {"LE_CONST", Operand::U16}, {"GT_CONST", Operand::U16},
    {"GE_CONST", Operand::U16}, {"EQ_CONST", Operand::U16},
    {"STORE_LOCAL", Operand::U16},
    {"CHECK_LOCAL", Operand::U16U16U8},
    {"RANGE_LOOP", Operand::U8},
    {"RANGE_NEXT", Operand::U16I32},
    {"ADD_LOCALS", Operand::U16U16},
    {"SUB_LOCALS", Operand::U16U16},
    {"MUL_LOCALS", Operand::U16U16},
    {"DIV_LOCALS", Operand::U16U16},
    {"LT_LOCALS", Operand::U16U16},
    {"LE_LOCALS", Operand::U16U16},
    {"GT_LOCALS", Operand::U16U16},
    {"GE_LOCALS", Operand::U16U16},
    {"EQ_LOCALS", Operand::U16U16},
    {"COPY_LOCAL", Operand::U16U16},
    {"OBJECT_CLASS", Operand::U16I32},
    {"STORE_CLASS", Operand::U16},
    {"OBJECT_CLASS_TYPED", Operand::U16I32},
    {"STORE_CLASS_TYPED", Operand::U16},
    {"CAPTURE", Operand::U16U8},
    {"NEW_OBJECT", Operand::U8},
    {"GET_CAPTURED", Operand::U16U16},
    {"SET_CAPTURED", Operand::U16},
    {"MAKE_TYPE", Operand::U16},
    {"CACHED_TYPE", Operand::U16I32},
    {"CACHE_TYPE", Operand::U16},
    {"TAG_COLLECTION", Operand::U16I32},
    {"TYPE_COLLECTION", Operand::None},
    {"IS_FULL", Operand::None},
    {"CAST_FULL", Operand::U8},
    {"WITH_TYPES", Operand::None},
    {"TYPE_ARG", Operand::U8},
    {"INSTANCE_TYPE_ARG", Operand::U8},
    {"SUPERTYPE_MAKER", Operand::None},
    {"CHECK_RESULT", Operand::U16},
    {"ADD_NEW", Operand::U8I32},
    {"PLACE_LOCAL", Operand::U16},
    {"PLACE_UPVALUE", Operand::U16},
    {"PLACE_GLOBAL", Operand::U16},
    {"PLACE_FIELD", Operand::U16},
    {"PLACE_INDEX", Operand::None},
    {"GET_INDEX_REF", Operand::None},
    {"ITER_NEXT_REF", Operand::U16I32},
    {"GET_PROP_REF", Operand::U16U16},
    {"GET_OWN_REF", Operand::U16U16},
    {"FIELD_DESTINATION", Operand::U16},
    {"WITH_REF_RESULT", Operand::None},
    {"NUMERIC", Operand::U8},
    {"USHR", Operand::None},
    {"ADD_WRAP", Operand::None},
    {"SUB_WRAP", Operand::None},
    {"MUL_WRAP", Operand::None},
    {"OWN_LOCAL", Operand::U16},
    {"WITH_DESTINATION_AT", Operand::U8},
    {"WITH_FRAME_DESTINATION", Operand::None},
    {"FROM_END", Operand::None},
    {"ITER_NEXT_PAIR", Operand::U16I32},
    {"CLASS_OF", Operand::U8},
    {"CLASS_META", Operand::U16U8},
    {"ATOMIC_FIELD", Operand::U16U8},
    {"SETTLE_ENUM", Operand::None},
    {"INDEX_N", Operand::U8},
    {"SET_INDEX_N", Operand::U8},
    {"DUP_N", Operand::U8},
    {"BUNDLE_VALUES", Operand::U8},
    {"MERGE_BUNDLE", Operand::None},
    {"FRAME_TYPES", Operand::None},
    {"INSERT_NEW", Operand::U8I32},
    {"CLASS_ORIGIN", Operand::U16},
    {"ADD_I", Operand::None},
    {"SUB_I", Operand::None},
    {"MUL_I", Operand::None},
    {"LT_I", Operand::None},
    {"LE_I", Operand::None},
    {"GT_I", Operand::None},
    {"GE_I", Operand::None},
    {"EQ_I", Operand::None},
    {"ADD_F", Operand::None},
    {"SUB_F", Operand::None},
    {"MUL_F", Operand::None},
    {"DIV_F", Operand::None},
    {"LT_F", Operand::None},
    {"LE_F", Operand::None},
    {"GT_F", Operand::None},
    {"GE_F", Operand::None},
    {"EQ_F", Operand::None},
    {"ENTER_PLACES", Operand::U8},
    {"SET_PLACE", Operand::U16U8},
    {"WITH_PLACE", Operand::U8},
    {"KEEP_LOCAL", Operand::U16},
    {"KEEP_TOP", Operand::None},
    {"FIELD_TYPES_MAKER", Operand::None},
    {"TYPE_SLOTS", Operand::U16},
    {"JUMP_IF_NOT_CLASS", Operand::U16I32},
};

const char* const kTypeNames[] = {"", "int", "float", "Number", "string", "bool", "List", "Map", "Range", "Function", "Bytes", "Type", "",
                                   "char", "byte", "int16", "int32", "uint16", "uint32", "float32", "Index", "Set",
                                   "FrozenList", "FrozenMap", "FrozenSet", "Values", "null"};
static_assert(sizeof(kTypeNames) / sizeof(kTypeNames[0]) == kTypeCount_, "kTypeNames must match TypeId");

static_assert(sizeof(kOps) / sizeof(kOps[0]) == static_cast<size_t>(Op::Count_), "kOps must match the Op enum");

size_t operandSize(Operand o) {
    switch (o) {
        case Operand::None: return 0;
        case Operand::U8: return 1;
        case Operand::U16: return 2;
        case Operand::I32: return 4;
        case Operand::U16U8: return 3;
        case Operand::U16I32: return 6;
        case Operand::U8I32: return 5;
        case Operand::U16U16: return 4;
        case Operand::U8U8: return 2;
        case Operand::U16U16U8: return 5;
    }
    return 0;
}

int readU16(const std::vector<uint8_t>& code, size_t at) { return code[at] | (code[at + 1] << 8); }

int32_t readI32(const std::vector<uint8_t>& code, size_t at) {
    uint32_t v = static_cast<uint32_t>(code[at]) | (static_cast<uint32_t>(code[at + 1]) << 8) |
                 (static_cast<uint32_t>(code[at + 2]) << 16) | (static_cast<uint32_t>(code[at + 3]) << 24);
    return static_cast<int32_t>(v);
}

// Decodes the operands of the instruction at `ip`: `a` is the first, `b` the second.
void decodeOperands(const std::vector<uint8_t>& code, size_t ip, Operand operand, int& a, int& b) {
    a = b = 0;
    switch (operand) {
        case Operand::None: break;
        case Operand::U8: a = code[ip + 1]; break;
        case Operand::U16: a = readU16(code, ip + 1); break;
        case Operand::I32: a = readI32(code, ip + 1); break;
        case Operand::U16U8:
            a = readU16(code, ip + 1);
            b = code[ip + 3];
            break;
        case Operand::U8I32:
            a = code[ip + 1];
            b = readI32(code, ip + 2);
            break;
        case Operand::U16I32:
            a = readU16(code, ip + 1);
            b = readI32(code, ip + 3);
            break;
        case Operand::U16U16:
        case Operand::U16U16U8:  // the third operand is read where it is needed
            a = readU16(code, ip + 1);
            b = readU16(code, ip + 3);
            break;
        case Operand::U8U8:
            a = code[ip + 1];
            b = code[ip + 2];
            break;
    }
}

}  // namespace

std::string builtinSpelling(const std::string& name) {
    static const std::string prefix = "ww.";
    if (name.rfind(prefix, 0) != 0) return name;
    const std::string rest = name.substr(prefix.size());
    for (uint8_t id = kTypeInt; id < kTypeCount_; id++)
        if (id != kTypeInstance && rest == kTypeNames[id]) return rest;
    return rest == "Unit" || rest == "Nothing" ? rest : name;
}

uint8_t builtinTypeId(const std::string& name) {
    for (uint8_t id = kTypeInt; id < kTypeCount_; id++)
        if (id != kTypeInstance && name == kTypeNames[id]) return id;
    return 0;
}

std::string typeText(uint8_t type, const std::string& className) {
    uint8_t id = type & kTypeMask;
    std::string text = id == kTypeInstance ? className : isBuiltinTypeId(id) ? kTypeNames[id] : "?";
    if (type & kTypeNullable) text += '?';
    return text;
}

Program::~Program() {
    if (dropNativeBuildsOf != nullptr) dropNativeBuildsOf(owned_);
}

FunctionProto* Program::newProto() {
    owned_.push_back(std::make_unique<FunctionProto>());
    return owned_.back().get();
}

// ---- Verification ----

// Follows every path through a function, checking each instruction and working out the
// stack depth it runs at. With `partial` the function is still being compiled: jumps that
// have no target yet are passed over, and the answer is the depth at which the code written
// so far ends, or -1 if nothing reaches that point.
static int analyzeStack(FunctionProto& f, const Program& program, bool partial, std::vector<int>* depths = nullptr, StackSteps* steps = nullptr) {
    auto fail = [&](const std::string& why) {
        throw std::runtime_error("invalid bytecode in " + f.name + ": " + why);
    };

    if (f.chooser) {
        // A set of forms runs none of its own code.
        if (!f.code.empty() || f.arity != 0) fail("a set of forms has code of its own");
        return 1;
    }
    const std::vector<uint8_t>& code = f.code;
    const size_t n = code.size();
    if (n == 0 && partial) return 1 + f.arity;
    if (n == 0) fail("empty function");
    if (f.lines.size() != n) fail("line table does not match the code");
    for (size_t i = 0; i < f.inlined.size(); i++) {
        const int call = f.inlined[i].callLine;
        if (call < 0 || (call >= kInlinedLine && static_cast<size_t>(call - kInlinedLine) >= i)) fail("bad call put in place");
    }
    for (int line : f.lines)
        if (line < 0 || (line >= kInlinedLine && static_cast<size_t>(line - kInlinedLine) >= f.inlined.size())) fail("bad line");
    if (f.arity < 0 || f.arity > 255) fail("bad arity");
    if (f.minArity < -1 || f.minArity > f.arity) fail("bad arity");
    if (f.varargIndex < -1 || f.varargIndex >= f.arity) fail("bad vararg parameter");
    if (!f.paramNames.empty() && f.paramNames.size() != static_cast<size_t>(f.arity)) fail("bad parameter names");
    for (uint16_t upvalue : f.outerReceivers)
        if ((upvalue & kCapturedReceiver) == 0 && upvalue >= f.upvalues.size()) fail("receiver upvalue out of range");

    // Stack depth on entry to each instruction, counted from the frame base (slot 0 is the
    // callee or receiver, then the parameters). -1 means not reached yet.
    std::vector<int> depthAt(n, -1);
    std::vector<size_t> work;
    int maxDepth = 1 + f.arity;

    int endDepth = -1;
    auto reachEnd = [&](int depth) {
        if (endDepth != -1 && endDepth != depth) fail("inconsistent stack depth");
        endDepth = depth;
    };

    size_t current = 0;  // the instruction being looked at, for `steps`
    if (steps != nullptr) {
        steps->pops.assign(n, 0);
        steps->pushes.assign(n, 0);
        steps->jumps.clear();
    }
    auto flow = [&](int64_t target, int depth) {
        if (steps != nullptr) steps->jumps.emplace_back(static_cast<uint32_t>(current), static_cast<uint32_t>(target));
        if (partial && target == 0) return;  // a forward jump that has not been given its target
        if (partial && static_cast<size_t>(target) == n) return reachEnd(depth);
        if (target < 0 || static_cast<size_t>(target) >= n) fail("jump out of range");
        int& d = depthAt[static_cast<size_t>(target)];
        if (d == -1) {
            d = depth;
            work.push_back(static_cast<size_t>(target));
        } else if (d != depth) {
            fail("inconsistent stack depth");
        }
    };

    int caches = 0;  // one more than the highest cache a GetProp or the like names

    auto requireName = [&](int index) {
        if (static_cast<size_t>(index) >= f.constants.size() || !f.constants[index].isString())
            fail("name operand is not a string constant");
    };

    // The places a function's frame has (D149): given by its first instruction, where it has any.
    int places = 0;
    if (n >= 2 && code[0] == static_cast<uint8_t>(Op::EnterPlaces)) places = code[1];
    if ((places != 0) != ((f.typeArgs & kHasPlaces) != 0)) fail("places that do not match the function's");

    depthAt[0] = 1 + f.arity;
    work.push_back(0);

    while (!work.empty()) {
        size_t ip = work.back();
        work.pop_back();
        int depth = depthAt[ip];

        for (;;) {
            uint8_t raw = code[ip];
            if (raw >= static_cast<uint8_t>(Op::Count_)) fail("unknown opcode");
            Op op = static_cast<Op>(raw);
            size_t size = 1 + operandSize(kOps[raw].operand);
            if (ip + size > n) fail("truncated instruction");

            int a, b;
            decodeOperands(code, ip, kOps[raw].operand, a, b);

            int pops = 0, pushes = 0;
            bool terminal = false;
            current = ip;

            switch (op) {
                case Op::Const:
                    if (static_cast<size_t>(a) >= f.constants.size()) fail("constant out of range");
                    pushes = 1;
                    break;
                case Op::Null:
                case Op::True:
                case Op::False:
                case Op::Unset:
                    pushes = 1;
                    break;
                case Op::GetUpvalueChecked:
                    if (static_cast<size_t>(a) >= f.upvalues.size()) fail("upvalue out of range");
                    requireName(b);
                    pushes = 1;
                    break;
                case Op::Pop:
                case Op::CloseUpvalue:
                    pops = 1;
                    break;
                case Op::Dup:
                    pops = 1;
                    pushes = 2;
                    break;
                case Op::Dup2:
                    pops = 2;
                    pushes = 4;
                    break;
                case Op::GetLocal:
                    if (a >= depth) fail("local slot out of range");
                    pushes = 1;
                    break;
                case Op::AddLocal:
                case Op::SubLocal:
                case Op::MulLocal:
                case Op::DivLocal:
                case Op::ModLocal:
                case Op::LtLocal:
                case Op::LeLocal:
                case Op::GtLocal:
                case Op::GeLocal:
                case Op::EqLocal:
                    if (a >= depth) fail("local slot out of range");
                    pops = pushes = 1;
                    maxDepth = std::max(maxDepth, depth + 1);  // the local is pushed where they are not worked out at once
                    break;
                case Op::AddLocals:
                case Op::SubLocals:
                case Op::MulLocals:
                case Op::DivLocals:
                case Op::LtLocals:
                case Op::LeLocals:
                case Op::GtLocals:
                case Op::GeLocals:
                case Op::EqLocals:
                    if (a >= depth || b >= depth) fail("local slot out of range");
                    pushes = 1;
                    maxDepth = std::max(maxDepth, depth + 2);  // both are pushed where they are not worked out at once
                    break;
                case Op::CopyLocal:
                    if (a >= depth || b >= depth) fail("local slot out of range");
                    break;
                case Op::AddConst:
                case Op::SubConst:
                case Op::MulConst:
                case Op::DivConst:
                case Op::ModConst:
                case Op::LtConst:
                case Op::LeConst:
                case Op::GtConst:
                case Op::GeConst:
                case Op::EqConst:
                    if (static_cast<size_t>(a) >= f.constants.size()) fail("constant out of range");
                    pops = pushes = 1;
                    maxDepth = std::max(maxDepth, depth + 1);
                    break;
                case Op::StoreLocal:
                    if (a >= depth - 1) fail("local slot out of range");
                    pops = 1;
                    break;
                case Op::CheckLocal:
                    if (a >= depth) fail("local slot out of range");
                    requireName(b);
                    break;
                case Op::SetLocal:
                    if (a >= depth) fail("local slot out of range");
                    pops = pushes = 1;
                    break;
                case Op::GetGlobal:
                case Op::SetGlobal:
                case Op::DefineGlobal:
                    if (static_cast<size_t>(a) >= program.globalNames.size()) fail("global out of range");
                    if (op == Op::GetGlobal) pushes = 1;
                    else if (op == Op::DefineGlobal) pops = 1;
                    else pops = pushes = 1;
                    break;
                case Op::GetUpvalue:
                case Op::SetUpvalue:
                    if (static_cast<size_t>(a) >= f.upvalues.size()) fail("upvalue out of range");
                    if (op == Op::GetUpvalue) pushes = 1;
                    else pops = pushes = 1;
                    break;
                case Op::GetImplicit:
                case Op::SetImplicit:
                    requireName(a);
                    if (static_cast<size_t>(b) >= program.globalNames.size()) fail("global out of range");
                    if (op == Op::GetImplicit) {
                        pushes = 1;
                    } else {
                        pops = pushes = 1;
                        // On the way, the object is slid in under the value.
                        maxDepth = std::max(maxDepth, depth + 1);
                    }
                    break;
                case Op::GetProp:
                case Op::GetOwn:
                case Op::GetPropRef:
                case Op::GetOwnRef:
                    requireName(a);
                    caches = std::max(caches, b + 1);
                    pops = pushes = 1;
                    break;
                case Op::FieldDestination:
                    requireName(a);
                    pops = pushes = 1;
                    break;
                case Op::WithRefResult:
                    break;
                case Op::Ushr:
                case Op::AddWrap:
                case Op::SubWrap:
                case Op::MulWrap:
                    pops = 2;
                    pushes = 1;
                    break;
                case Op::Numeric:
                    if (a >= static_cast<int>(NumericOp::Count_)) fail("unknown numeric operation");
                    pops = numericArity(static_cast<uint8_t>(a));
                    pushes = 1;
                    break;
                case Op::SetProp:
                case Op::SetOwn:
                    requireName(a);
                    caches = std::max(caches, b + 1);
                    pops = 2;
                    pushes = 1;
                    break;
                case Op::GetSuper:
                case Op::Method:
                case Op::StaticMethod:
                case Op::Getter:
                case Op::Setter:
                case Op::StaticGetter:
                case Op::StaticSetter:
                    requireName(a);
                    pops = 2;
                    pushes = 1;
                    break;
                case Op::SetSuper:
                    requireName(a);
                    pops = 3;
                    pushes = 1;
                    break;
                case Op::Extend:
                    pops = 2;
                    pushes = 1;
                    break;
                case Op::Bridge:
                case Op::BridgeMethod:
                    requireName(a);
                    pops = 2;
                    pushes = 1;
                    break;
                case Op::GetIndex:
                case Op::AddI:
                case Op::SubI:
                case Op::MulI:
                case Op::LtI:
                case Op::LeI:
                case Op::GtI:
                case Op::GeI:
                case Op::EqI:
                case Op::AddF:
                case Op::SubF:
                case Op::MulF:
                case Op::DivF:
                case Op::LtF:
                case Op::LeF:
                case Op::GtF:
                case Op::GeF:
                case Op::EqF:
                case Op::Add:
                case Op::Sub:
                case Op::Mul:
                case Op::Div:
                case Op::Mod:
                case Op::Identical:
                case Op::Eq:
                case Op::Lt:
                case Op::Le:
                case Op::Gt:
                case Op::Ge:
                case Op::Is:
                case Op::Shl:
                case Op::Shr:
                case Op::BitAnd:
                case Op::BitOr:
                case Op::BitXor:
                    pops = 2;
                    pushes = 1;
                    break;
                case Op::SetIndex:
                    pops = 3;
                    pushes = 1;
                    break;
                case Op::Neg:
                case Op::Not:
                case Op::BitNot:
                case Op::FromEnd:
                case Op::IterInit:
                    pops = pushes = 1;
                    break;
                case Op::Jump:
                case Op::Loop:
                    flow(a, depth);
                    terminal = true;
                    break;
                case Op::JumpIfFalse:
                    if (depth < 2) fail("stack underflow");
                    flow(a, depth - 1);
                    pops = 1;
                    break;
                case Op::JumpIfNotClass:
                    if (depth < 2) fail("stack underflow");
                    if (static_cast<size_t>(a) >= program.globalNames.size()) fail("global out of range");
                    flow(b, depth - 1);
                    pops = 1;
                    break;
                case Op::And:
                case Op::Or:
                    if (depth < 2) fail("stack underflow");
                    flow(a, depth);
                    pops = 1;
                    break;
                case Op::JumpIfNull:
                case Op::JumpIfNotNull:
                    if (depth < 2) fail("stack underflow");
                    flow(a, depth);
                    break;
                case Op::AssertNotNull:
                    pops = pushes = 1;
                    break;
                case Op::Swap:
                    pops = pushes = 2;
                    break;
                case Op::PopUnder:
                    pops = a + 1;
                    pushes = 1;
                    break;
                case Op::Spread:
                    pops = 2;
                    pushes = 1;
                    break;
                case Op::EnumEntry:
                    requireName(a);
                    pops = 2;
                    pushes = 1;
                    break;
                case Op::EnumNext:
                case Op::MemberAccess:
                case Op::StaticAccess:
                    requireName(a);
                    pops = pushes = 1;
                    break;
                case Op::ClassNumber:
                    pops = pushes = 1;
                    break;
                case Op::EnumSeal:
                case Op::UnstableKey:
                case Op::MarkInterface:
                case Op::FinishClass:
                    pops = pushes = 1;
                    break;
                case Op::Require:
                case Op::DataField:
                    requireName(a);
                    pops = pushes = 1;
                    break;
                case Op::Component:
                case Op::Lazy:
                    pops = pushes = 1;
                    break;
                case Op::ObjectClass:
                case Op::ObjectClassTyped:
                    if (a >= static_cast<int>(program.globalNames.size()) && a != 0xFFFF) fail("global out of range");
                    flow(b, depth + 1);
                    maxDepth = std::max(maxDepth, depth + 1);
                    break;
                case Op::StoreClass:
                case Op::StoreClassTyped:
                    if (a >= static_cast<int>(program.globalNames.size()) && a != 0xFFFF) fail("global out of range");
                    pops = pushes = 1;
                    break;
                case Op::Capture:
                    if (b > 3 || (b == 0 && a >= depth) || (b == 3 && a >= depth)) fail("bad capture");
                    pushes = 1;
                    break;
                case Op::NewObject:
                    pops = a + 1;
                    pushes = 1;
                    break;
                case Op::GetCaptured:
                    requireName(b);
                    pushes = 1;
                    break;
                case Op::MakeType: {
                    requireName(a);
                    const int operands = shapeOperands(asString(f.constants[static_cast<size_t>(a)])->utf8());
                    if (operands < 0) fail("bad type shape");
                    pops = operands;
                    pushes = 1;
                    break;
                }
                case Op::CachedType:
                    caches = std::max(caches, a + 1);
                    flow(b, depth + 1);
                    maxDepth = std::max(maxDepth, depth + 1);
                    break;
                case Op::CacheType:
                    caches = std::max(caches, a + 1);
                    pops = pushes = 1;
                    break;
                case Op::TagCollection:
                    caches = std::max(caches, a + 1);
                    flow(b, depth);
                    break;
                case Op::TypeCollection:
                case Op::IsFull:
                case Op::CastFull:
                    pops = 2;
                    pushes = 1;
                    break;
                case Op::CheckResult:
                    requireName(a);
                    pops = 2;
                    pushes = 1;
                    break;
                case Op::PlaceLocal:
                    if (a >= depth) fail("local slot out of range");
                    pushes = 1;
                    break;
                case Op::PlaceUpvalue:
                    if (static_cast<size_t>(a) >= f.upvalues.size()) fail("captured variable out of range");
                    pushes = 1;
                    break;
                case Op::PlaceGlobal:
                    if (static_cast<size_t>(a) >= program.globalNames.size()) fail("global out of range");
                    pushes = 1;
                    break;
                case Op::PlaceField:
                    requireName(a);
                    pops = pushes = 1;
                    break;
                case Op::PlaceIndex:
                case Op::GetIndexRef:
                    pops = 2;
                    pushes = 1;
                    break;
                case Op::OwnLocal:
                case Op::KeepLocal:
                    if (a >= depth) fail("local slot out of range");
                    break;
                case Op::EnterPlaces:
                    if (ip != 0 || a != places) fail("places given anywhere but where a function starts");
                    break;
                case Op::SetPlace:
                    if (a >= depth) fail("local slot out of range");
                    if (b >= places) fail("place out of range");
                    pops = pushes = 1;
                    break;
                case Op::WithPlace:
                    if (a >= places) fail("place out of range");
                    break;
                case Op::KeepTop:
                    pops = pushes = 1;
                    break;
                case Op::WithDestinationAt:
                    if (depth < a + 1) fail("stack underflow");
                    break;
                case Op::WithFrameDestination:
                    break;
                case Op::AddNew:
                    // On at the target with the add's result, or on to the call, all as they were.
                    if (depth < a + 2) fail("stack underflow");
                    flow(b, depth - a - 1);
                    break;
                case Op::InsertNew:
                    // As AddNew, with the position under the class; the high bit of argc is the result's.
                    if (depth < (a & 0x7F) + 3) fail("stack underflow");
                    flow(b, depth - (a & 0x7F) - 2);
                    break;
                case Op::WithTypes:
                    pops = 1;
                    break;
                case Op::TypeArg:
                    if ((f.typeArgs & (kTypeArgsOwn | kTypeArgsKept)) == 0) fail("type arguments out of reach");
                    pushes = 1;
                    break;
                case Op::InstanceTypeArg:
                case Op::SupertypeMaker:
                case Op::FieldTypesMaker:
                    pops = 2;
                    pushes = 1;
                    break;
                case Op::SetCaptured:
                    pops = pushes = 1;
                    break;
                case Op::Range:
                    if ((a & ~7) != 0) fail("bad range");
                    pops = (a & 4) != 0 ? 3 : 2;
                    pushes = 1;
                    break;
                case Op::RangeLoop:
                    if ((a & ~7) != 0) fail("bad range");
                    pops = (a & 4) != 0 ? 3 : 2;
                    pushes = 3;
                    break;
                case Op::RangeNext:
                    if (a + 2 >= depth) fail("local slot out of range");
                    flow(b, depth);
                    pushes = 1;
                    break;
                case Op::MissingArg:
                    requireName(a);
                    terminal = true;
                    break;
                case Op::TailLoop:
                    pops = a;
                    terminal = true;
                    break;
                case Op::CallOn:
                    pops = a + 2;
                    pushes = 1;
                    break;
                case Op::ImplicitFirst:
                    if (a >= depth) fail("local slot out of range");
                    if ((b & 0xFE) != 0) fail("bad flags");
                    break;
                case Op::ClassInit:
                    pops = 2;
                    pushes = 1;
                    break;
                case Op::EnsureClassInit:
                case Op::MarkAbstract:
                case Op::MarkValue:
                    pops = pushes = 1;
                    break;
                case Op::TypeSlots:
                    requireName(a);
                    pops = pushes = 1;
                    break;
                case Op::ClassOrigin:
                    requireName(a);
                    pops = pushes = 1;
                    break;
                case Op::CheckOverride:
                    requireName(a);
                    pops = pushes = 1;
                    break;
                case Op::Extension:
                    requireName(a);
                    if ((b & kTypeMask) >= kTypeCount_) fail("unknown type");
                    pops = (b & kTypeMask) == kTypeInstance ? 2 : 1;
                    break;
                case Op::Inherit:
                    if (a < 1) fail("bad supertype count");
                    pops = a + 1;
                    pushes = 1;
                    break;
                case Op::PendingEnum:
                    requireName(a);
                    pushes = 1;
                    break;
                case Op::TryImplicit:
                    requireName(a);
                    flow(b, depth + 1);
                    maxDepth = std::max(maxDepth, depth + 1);
                    break;
                case Op::InvokeImplicit:
                    requireName(a);
                    if (static_cast<size_t>(b) >= program.globalNames.size()) fail("global out of range");
                    pops = code[ip + 5] + 1;
                    pushes = 1;
                    break;
                case Op::StaticField: {
                    requireName(a);
                    int id = b & kTypeMask;
                    if (id >= kTypeCount_ || (b & ~(0xFF | kFieldVal | kFieldNested)) != 0) fail("bad field declaration");
                    pops = id == kTypeInstance ? 3 : 2;
                    pushes = 1;
                    break;
                }
                case Op::CallSpread:
                    pops = 2 + 2 * a;
                    pushes = 1;
                    break;
                case Op::IsType:
                case Op::Cast:
                case Op::CastOrNull: {
                    int id = a & kTypeMask;
                    if (id < kTypeInt || id >= kTypeCount_) fail("unknown type");
                    pops = id == kTypeInstance ? 2 : 1;
                    pushes = 1;
                    break;
                }
                case Op::Settle:
                    // At most one slot is added: that of a variable whose value was being worked out.
                    if (depth < 2 || a < 1 || a > depth) fail("bad stack size");
                    depth = a + 1;
                    break;
                case Op::Call:
                    pops = a + 1;
                    pushes = 1;
                    break;
                case Op::Invoke:
                case Op::InvokeOwn:
                    requireName(a);
                    pops = b + 1;
                    pushes = 1;
                    break;
                case Op::InvokeTrailing:
                case Op::InvokeOwnTrailing:
                    requireName(a);
                    if (b < 1) fail("a call with a lambda after it has no arguments");
                    pops = b + 1;
                    pushes = 1;
                    break;
                case Op::SuperInvoke:
                    requireName(a);
                    pops = b + 2;
                    pushes = 1;
                    break;
                case Op::SuperInvokeTrailing:
                    requireName(a);
                    if (b < 1) fail("a call with a lambda after it has no arguments");
                    pops = b + 2;
                    pushes = 1;
                    break;
                case Op::SuperInit:
                    pops = a + 2;
                    pushes = 1;
                    break;
                case Op::CallNamed:
                    pops = 1 + a + 2 * b;
                    pushes = 1;
                    break;
                case Op::MakeForms:
                    if (static_cast<size_t>(a) >= f.protos.size() || !f.protos[a]->chooser) fail("bad set of forms");
                    if (b < 2) fail("a set of forms has fewer than two");
                    pops = b;
                    pushes = 1;
                    break;
                case Op::GetIndexOr:
                    pops = 3;
                    pushes = 1;
                    break;
                case Op::ClassOf:
                    if (a > 2) fail("bad operand of a class");
                    pops = 1;
                    pushes = 2;
                    break;
                case Op::AtomicField:
                    requireName(a);
                    if (b > 6) fail("bad atomic operation");
                    pops = b == 4 ? 3 : b == 5 ? 1 : 2;
                    pushes = 1;
                    break;
                case Op::SettleEnum:
                    pops = 2;
                    pushes = 1;
                    break;
                case Op::IndexN:
                    if (a < 1) fail("bad index count");
                    pops = a + 1;
                    pushes = 1;
                    break;
                case Op::SetIndexN:
                    if (a < 1) fail("bad index count");
                    pops = a + 2;
                    pushes = 1;
                    break;
                case Op::DupN:
                    if (a < 1) fail("bad count");
                    pops = a;
                    pushes = 2 * a;
                    break;
                case Op::BundleValues:
                    pops = a + 1;
                    pushes = 2 * a;
                    break;
                case Op::MergeBundle:
                    pops = 2;
                    pushes = 1;
                    break;
                case Op::FrameTypes:
                    if ((f.typeArgs & (kTypeArgsOwn | kTypeArgsKept)) == 0) fail("type arguments out of reach");
                    break;
                case Op::ClassMeta:
                    if (static_cast<size_t>(a) >= f.protos.size()) fail("nested function out of range");
                    if (b > 1) fail("bad description of a class");
                    pops = pushes = 1;
                    break;
                case Op::MethodForms:
                case Op::StaticForms:
                    if (static_cast<size_t>(a) >= f.protos.size() || !f.protos[a]->chooser) fail("bad set of forms");
                    if (b < 1) fail("a set of forms has none");
                    pops = b + 1;
                    pushes = 1;
                    break;
                case Op::Closure: {
                    if (static_cast<size_t>(a) >= f.protos.size()) fail("nested function out of range");
                    // A closure keeps the type arguments of the frame it is made in, which has them.
                    if ((f.protos[a]->typeArgs & kTypeArgsKept) != 0 && (f.typeArgs & (kTypeArgsOwn | kTypeArgsKept)) == 0) fail("type arguments out of reach");
                    // A local function may capture its own slot, which is the one being pushed.
                    for (const UpvalueDesc& uv : f.protos[a]->upvalues) {
                        if (uv.fromCapture) continue;  // checked when the closure is made
                        if (uv.isLocal ? uv.index > depth : uv.index >= f.upvalues.size())
                            fail("captured variable out of range");
                    }
                    pushes = 1;
                    break;
                }
                case Op::Return:
                case Op::ReturnInit:
                    terminal = true;
                    break;
                case Op::DeclareField: {
                    requireName(a);
                    int id = b & kTypeMask;
                    if (id >= kTypeCount_ || (b & ~(0xFF | kFieldVal | kFieldRequired | kFieldOverride)) != 0) fail("bad field declaration");
                    pops = id == kTypeInstance ? 2 : 1;
                    pushes = 1;
                    break;
                }
                case Op::FieldInit:
                    pops = 2;
                    pushes = 1;
                    break;
                case Op::Class:
                    requireName(a);
                    pushes = 1;
                    break;
                case Op::BuildList:
                case Op::BuildString:
                    pops = a;
                    pushes = 1;
                    break;
                case Op::BuildMap:
                    pops = 2 * a;
                    pushes = 1;
                    break;
                case Op::IterNext:
                case Op::IterNextRef:
                    if (a >= depth) fail("local slot out of range");
                    flow(b, depth);
                    pushes = 1;
                    break;
                case Op::IterNextPair:
                    if (a >= depth) fail("local slot out of range");
                    flow(b, depth);
                    pushes = 2;
                    break;
                case Op::DefaultArg:
                    if (a >= depth) fail("local slot out of range");
                    flow(b, depth);
                    break;
                case Op::TryBegin:
                    // The handler starts with the thrown value on top of the current stack.
                    flow(a, depth + 1);
                    maxDepth = std::max(maxDepth, depth + 1);
                    break;
                case Op::TryEnd:
                    break;
                case Op::Throw:
                    pops = 1;
                    terminal = true;
                    break;
                case Op::Import:
                    if (static_cast<size_t>(a) >= program.modules.size()) fail("module out of range");
                    pushes = 1;
                    break;
                case Op::CheckType:
                case Op::CheckParam:
                case Op::CheckReturn:
                case Op::DefineTypedGlobal: {
                    int id = (op == Op::CheckReturn ? a : b) & kTypeMask;
                    if (id < kTypeInt || id >= kTypeCount_) fail("unknown type");
                    if (op == Op::DefineTypedGlobal) {
                        if (static_cast<size_t>(a) >= program.globalNames.size()) fail("global out of range");
                    } else {
                        if (op != Op::CheckReturn) requireName(a);
                        pushes = 1;
                    }
                    pops = id == kTypeInstance ? 2 : 1;
                    break;
                }
                default:
                    // Count_, and the run code's instructions, which stack code never holds; and
                    // so, refused, a stack code instruction this list has been left without.
                    fail("unknown opcode");
            }

            // Slot 0 belongs to the frame and is never popped.
            if (depth - pops < 1) fail("stack underflow");
            if (steps != nullptr) {
                steps->pops[ip] = static_cast<uint16_t>(pops);
                steps->pushes[ip] = static_cast<uint16_t>(pushes);
            }
            depth = depth - pops + pushes;
            maxDepth = std::max(maxDepth, depth);
            if (terminal) break;

            size_t nextIp = ip + size;
            if (nextIp >= n && partial) {
                reachEnd(depth);
                break;
            }
            if (nextIp >= n) fail("control runs off the end of the function");
            int& d = depthAt[nextIp];
            if (d != -1) {
                if (d != depth) fail("inconsistent stack depth");
                break;
            }
            d = depth;
            ip = nextIp;
        }
    }

    if (depths != nullptr) *depths = depthAt;
    if (!partial) {
        f.maxStack = maxDepth;
        if (static_cast<uint32_t>(caches) > f.cacheCount) {
            f.caches.reset(new std::atomic<uint64_t>[static_cast<size_t>(caches)]());
            f.cacheCount = static_cast<uint32_t>(caches);
        }
    }
    return endDepth;
}

// What a function's body is checked for besides its stack (analyzeStack): its rights and notes
// in range, and whether its whole work is reading a field. Done as it is read, or as its body is.
void verifyBody(FunctionProto& f, const Program& program) {
    for (const FunctionProto::Rights& r : f.rights)
        if (r.start > r.end || r.end > f.code.size()) throw std::runtime_error("invalid bytecode: rights of copied code out of range");
    if (f.reflection != nullptr) {
        for (const FunctionProto::ReflectNote& note : f.reflection->notes) {
            if (note.at >= f.code.size()) throw std::runtime_error("invalid bytecode: a note on reflection out of range");
            for (uint16_t global : note.globals)
                if (global >= program.globalNames.size()) throw std::runtime_error("invalid bytecode: a note on reflection names no global");
        }
        for (const std::vector<uint16_t>& entry : f.reflection->supertypes)
            for (size_t i = 0; i < entry.size(); i++)
                if (entry[i] >= program.globalNames.size() && !(i == 0 && entry[i] == kNoGlobal))
                    throw std::runtime_error("invalid bytecode: a class's supertypes name no global");
    }
    // A function whose whole work is `this.name`: GetLocal 0, GetProp or GetOwn, Return, after
    // the places it gives its frame, which are none (D149).
    const std::vector<uint8_t>& whole = f.code;
    f.readsField = nullptr;
    const size_t from = whole.size() >= 2 && whole[0] == static_cast<uint8_t>(Op::EnterPlaces) && whole[1] == 0 ? 2 : 0;
    const uint8_t* c = whole.data() + from;
    const size_t size = whole.size() - from;
    auto u16 = [&](size_t at) { return static_cast<size_t>(c[at] | (c[at + 1] << 8)); };
    if (f.arity == 0 && f.upvalues.empty() && f.rights.empty() && size >= 9 && c[0] == static_cast<uint8_t>(Op::GetLocal) && u16(1) == 0 &&
        (c[3] == static_cast<uint8_t>(Op::GetProp) || c[3] == static_cast<uint8_t>(Op::GetOwn)) && c[8] == static_cast<uint8_t>(Op::Return) &&
        u16(4) < f.constants.size() && f.constants[u16(4)].isString())
        f.readsField = asString(f.constants[u16(4)]);
}

void verifyFunction(FunctionProto& f, const Program& program, bool now, bool trusted) {
    // One read without its body: what needs the body is done as it is read (loadBody), and it
    // is translated at its first call.
    const bool bodyLater = f.laterBody != nullptr;
    if (!bodyLater) verifyBody(f, program);
    f.quickArity = !f.chooser && f.varargIndex < 0 && (f.typeArgs & ~(kTakesDestination | kHasPlaces)) == 0 ? f.arity : -1;
    f.quickFewest = f.quickArity >= 0 ? f.requiredArgs() : 0;
    std::vector<int> depths;
    StackSteps steps;
    // Code of ww's own making translated at its first call is checked then (translateNow):
    // a program's start reads many functions it never calls (a quarter of a kept program's
    // start went to checking them, 2026-11-09).
    const bool later = !now && !f.chooser && (bodyLater || !f.code.empty());
    f.unverified = trusted && later;
    if (!f.unverified) analyzeStack(f, program, false, &depths, later ? nullptr : &steps);
    if (!later) {
        f.untranslated = nullptr;
        translate(f, depths, f.unverified ? nullptr : &steps);
    } else {
        // Its run code made at its first call: a program's start reads the whole standard
        // library, and translating it all took 11 of its 14 ms (M9/M10).
        f.untranslated = &program;
        runLater(f);
    }
}

int stackDepthAtEnd(FunctionProto& f, const Program& program) { return analyzeStack(f, program, true); }

void translateNow(FunctionProto& f) {
    // One at a time: threads of one VM take turns, but a function read late (loadDeferred) or
    // a program of its own may be reached by more than one.
    static std::mutex translating;
    std::lock_guard<std::mutex> hold(translating);
    if (f.untranslated == nullptr) return;
    loadBody(f);
    // The standard library's own top-level code runs once, as a program starts, and only makes
    // its classes and functions: its stack code is run as it is, threaded, which takes a third
    // of the time of translating its 30 KB into R instructions that would pay for themselves
    // only in a loop.
    const bool once = f.name == "<script>" && f.source == "<std>";
    std::vector<int> depths;
    StackSteps steps;
    // As verified, only the depths (and what each instruction does to the stack) are wanted
    // again; one read unverified is verified now.
    if (!once || f.unverified) {
        std::vector<int> found;
        analyzeStack(f, *f.untranslated, false, &found, &steps);
        if (!once) depths = std::move(found);
        f.unverified = false;
    }
    f.addressed.done.store(false, std::memory_order_relaxed);
    translate(f, depths, depths.empty() ? nullptr : &steps);
    f.untranslated = nullptr;
}

void Program::translateAll() {
    // By index: a function whose body is read only now (loadBody) adds its nested functions to
    // owned_ as it is translated, which may move the list, and they are translated too.
    for (size_t i = 0; i < owned_.size(); i++) {
        FunctionProto* f = owned_[i].get();
        if (f->untranslated != nullptr) translateNow(*f);
    }
}

// ---- Disassembly ----

namespace {

std::string constantText(const Value& v) {
    switch (v.kind) {
        case ValueKind::Int: return std::to_string(v.as.i);
        case ValueKind::Float: return formatFloat(v.as.d);
        case ValueKind::Obj:
            if (v.isString()) return quoteString(asString(v)->utf8());
            return "<object>";
        default: return "?";
    }
}

void disassembleFunction(const FunctionProto& f, const Program& program, std::ostream& out) {
    // A function of a kept program has its body read, and is checked and its stack's size found,
    // when first asked for (at its first call, translateNow): here, listing it.
    FunctionProto& own = const_cast<FunctionProto&>(f);
    if (f.laterBody != nullptr) loadBody(own);
    if (f.deferred != nullptr) loadDeferred(own);
    if (f.unverified) {
        analyzeStack(own, program, false);
        own.unverified = false;
    }
    std::string arity = std::to_string(f.arity);
    if (f.requiredArgs() != f.arity) arity = std::to_string(f.requiredArgs()) + ".." + arity;
    out << "== " << f.name << " (" << f.source << ")  arity=" << arity << " upvalues=" << f.upvalues.size()
        << " stack=" << f.maxStack << " ==\n";

    const std::vector<uint8_t>& code = f.code;
    size_t ip = 0;
    int lastLine = -1;
    while (ip < code.size()) {
        uint8_t raw = code[ip];
        if (raw >= static_cast<uint8_t>(Op::Count_)) {
            out << std::setw(5) << ip << "  ??? " << static_cast<int>(raw) << "\n";
            break;
        }
        const OpInfo& info = kOps[raw];
        size_t size = 1 + operandSize(info.operand);
        if (ip + size > code.size()) break;

        int a, b;
        decodeOperands(code, ip, info.operand, a, b);

        out << std::setw(5) << ip << " ";
        int line = f.sourceLine(f.lines[ip]);
        if (line == lastLine) out << "    | ";
        else out << std::setw(5) << line << " ";
        lastLine = line;
        out << std::left << std::setw(20) << info.name << std::right;

        switch (static_cast<Op>(raw)) {
            case Op::Const:
            case Op::AddConst:
            case Op::SubConst:
            case Op::MulConst:
            case Op::DivConst:
            case Op::ModConst:
            case Op::LtConst:
            case Op::LeConst:
            case Op::GtConst:
            case Op::GeConst:
            case Op::EqConst:
                out << " " << a << "  ; " << constantText(f.constants[a]);
                break;
            case Op::GetGlobal:
            case Op::SetGlobal:
            case Op::DefineGlobal:
                out << " " << a << "  ; " << (static_cast<size_t>(a) < program.globalNames.size() ? program.globalNames[a] : std::string("?"));
                break;
            case Op::ClassOrigin:
            case Op::GetProp:
            case Op::SetProp:
            case Op::GetOwn:
            case Op::SetOwn:
            case Op::GetPropRef:
            case Op::GetOwnRef:
            case Op::FieldDestination:
            case Op::GetSuper:
            case Op::SetSuper:
            case Op::Class:
            case Op::Method:
            case Op::StaticMethod:
            case Op::Getter:
            case Op::Setter:
            case Op::StaticGetter:
            case Op::StaticSetter:
                out << " " << a << "  ; " << asString(f.constants[a])->utf8();
                break;
            case Op::Invoke:
            case Op::InvokeTrailing:
            case Op::InvokeOwn:
            case Op::InvokeOwnTrailing:
            case Op::SuperInvoke:
            case Op::SuperInvokeTrailing:
                out << " " << a << " argc=" << b << "  ; " << asString(f.constants[a])->utf8();
                break;
            case Op::GetImplicit:
            case Op::SetImplicit:
                out << " " << a << " " << b << "  ; " << asString(f.constants[a])->utf8();
                break;
            case Op::CallNamed:
                out << " positional=" << a << " named=" << b;
                break;
            case Op::TryImplicit:
                out << " " << a << " -> " << b << "  ; " << asString(f.constants[a])->utf8();
                break;
            case Op::InvokeImplicit:
                out << " " << a << " " << b << " argc=" << static_cast<int>(f.code[ip + 5]) << "  ; " << asString(f.constants[a])->utf8();
                break;
            case Op::Closure:
                out << " " << a << "  ; " << f.protos[a]->name;
                break;
            case Op::MakeForms:
            case Op::MethodForms:
            case Op::StaticForms:
                out << " " << a << " count=" << b << "  ; " << f.protos[a]->name;
                break;
            case Op::ClassMeta:
                out << " " << a << (b == 0 ? " fields" : " methods") << "  ; " << f.protos[a]->name;
                break;
            case Op::AtomicField:
                out << " " << a << " " << (b == 0 ? "+=" : b == 1 ? "-=" : b == 2 ? "max" : b == 3 ? "min" : "compareAndSet") << "  ; " << asString(f.constants[a])->utf8();
                break;
            case Op::DefaultArg:
            case Op::IterNext:
            case Op::IterNextRef:
            case Op::IterNextPair:
            case Op::RangeNext:
                out << " slot=" << a << " -> " << b;
                break;
            case Op::JumpIfNotClass:
                out << " " << a << " -> " << b << "  ; " << program.globalNames[static_cast<size_t>(a)];
                break;
            case Op::CachedType:
            case Op::TagCollection:
                out << " cache=" << a << " -> " << b;
                break;
            case Op::AddNew:
                out << " argc=" << a << " -> " << b;
                break;
            case Op::InsertNew:
                out << " argc=" << (a & 0x7F) << ((a & 0x80) != 0 ? " gives true" : "") << " -> " << b;
                break;
            case Op::ObjectClass:
            case Op::ObjectClassTyped:
                out << " " << a << " -> " << b;
                break;
            case Op::Capture:
                out << " " << a << " kind=" << b;
                break;
            case Op::Jump:
            case Op::Loop:
            case Op::JumpIfFalse:
            case Op::And:
            case Op::Or:
            case Op::JumpIfNull:
            case Op::JumpIfNotNull:
            case Op::TryBegin:
                out << " -> " << a;
                break;
            case Op::Import:
                out << " " << a << "  ; " << program.modules[a]->source;
                break;
            case Op::CheckType:
            case Op::CheckParam:
                out << " " << a << "  ; " << asString(f.constants[a])->utf8() << " : " << typeText(static_cast<uint8_t>(b), "<class>");
                break;
            case Op::CheckReturn:
            case Op::IsType:
            case Op::Cast:
            case Op::CastOrNull:
                out << "  ; " << typeText(static_cast<uint8_t>(a), "<class>");
                break;
            case Op::StaticField:
            case Op::DeclareField:
                out << " " << a << "  ; " << ((b & kFieldVal) ? "val " : "var ") << asString(f.constants[a])->utf8();
                if ((b & kTypeMask) != 0) out << " : " << typeText(static_cast<uint8_t>(b & 0xFF), "<class>");
                if (b & kFieldRequired) out << " (set by init)";
                break;
            case Op::DefineTypedGlobal:
                out << " " << a << "  ; " << (static_cast<size_t>(a) < program.globalNames.size() ? program.globalNames[a] : std::string("?")) << " : " << typeText(static_cast<uint8_t>(b), "<class>");
                break;
            default:
                if (info.operand != Operand::None) out << " " << a;
                break;
        }
        out << "\n";
        ip += size;
    }
    out << "\n";

    for (const FunctionProto* nested : f.protos) disassembleFunction(*nested, program, out);
}

}  // namespace

size_t operandBytes(Op op) {
    return op < Op::Count_ ? operandSize(kOps[static_cast<size_t>(op)].operand) : 0;
}

size_t instructionSize(const std::vector<uint8_t>& code, size_t at) {
    const uint8_t raw = code[at];
    return raw < static_cast<uint8_t>(Op::Count_) ? 1 + operandSize(kOps[raw].operand) : 1;
}

int shapeOperands(const std::string& shape) {
    int operands = 0;
    for (size_t at = 0; at < shape.size(); at++) {
        const char c = shape[at];
        if (c == 'C') {
            if (at + 1 >= shape.size() || shape[at + 1] < '0' || shape[at + 1] > '9') return -1;
            operands++;
            at++;
        } else if (c == 'A') {
            if (at + 1 >= shape.size() || shape[at + 1] < '0' || shape[at + 1] > '9') return -1;
            at++;
        } else if (c == 'P') {
            operands++;
        } else if (std::string_view("?aifnsbyrtFLMcuhdHDgxSZV").find(c) == std::string_view::npos) {
            return -1;
        }
    }
    return operands;
}

const char* opName(Op op) {
    const size_t i = static_cast<size_t>(op);
    if (i < static_cast<size_t>(Op::Count_)) return kOps[i].name;
    // The run code's own (M6.7), in the order of the Op enum after Count_.
    static const char* const run[] = {"MOVE_S",       "MOVE_K",       "SET_TOP",      "R_ADD_SS",     "R_ADD_SK",     "R_ADD_KS",
                                      "R_SUB_SS",     "R_SUB_SK",     "R_MUL_SS",     "R_MUL_SK",     "R_MUL_KS",     "R_DIV_SS",
                                      "R_DIV_SK",     "R_LT_SS",      "R_LT_SK",      "R_LE_SS",      "R_LE_SK",      "R_GT_SS",
                                      "R_GT_SK",      "R_GE_SS",      "R_GE_SK",      "R_EQ_SS",      "R_EQ_SK",      "R_JUMP_LT_SS",
                                      "R_JUMP_LT_SK", "R_JUMP_LE_SS", "R_JUMP_LE_SK", "R_JUMP_GT_SS", "R_JUMP_GT_SK", "R_JUMP_GE_SS",
                                      "R_JUMP_GE_SK", "R_JUMP_EQ_SS", "R_JUMP_EQ_SK", "R_GET_PROP",   "R_MOD_SS",     "R_MOD_SK",
                                      "R_SUB_KS",     "R_DIV_KS",     "R_SET_PROP_SS", "R_SET_PROP_SK", "R_GET_GLOBAL", "R_GET_UPVALUE",
                                      "R_GET_INDEX_SS", "R_GET_INDEX_SK", "R_NEG",     "R_NOT",        "R_JUMP_IF_FALSE", "R_SHL_SK",
                                      "R_SHR_SK",     "R_BIT_AND_SS", "R_BIT_AND_SK", "R_BIT_OR_SS",  "R_BIT_OR_SK", "R_SHL_SS", "R_SHR_SS", "R_BIT_XOR_SS", "R_BIT_XOR_SK", "R_USHR_SK", "R_USHR_SS", "R_ADD_WRAP_SS", "R_ADD_WRAP_SK", "R_SUB_WRAP_SS", "R_SUB_WRAP_SK", "R_MUL_WRAP_SS", "R_MUL_WRAP_SK",
                                      "R_ADD_I_SS", "R_ADD_I_SK", "R_ADD_I_KS", "R_SUB_I_SS", "R_SUB_I_SK",
                                      "R_SUB_I_KS", "R_MUL_I_SS", "R_MUL_I_SK", "R_MUL_I_KS", "R_ADD_F_SS",
                                      "R_ADD_F_SK", "R_ADD_F_KS", "R_SUB_F_SS", "R_SUB_F_SK", "R_SUB_F_KS",
                                      "R_MUL_F_SS", "R_MUL_F_SK", "R_MUL_F_KS", "R_DIV_F_SS", "R_DIV_F_SK",
                                      "R_DIV_F_KS", "R_LT_I_SS", "R_LT_I_SK", "R_LE_I_SS", "R_LE_I_SK",
                                      "R_GT_I_SS", "R_GT_I_SK", "R_GE_I_SS", "R_GE_I_SK", "R_EQ_I_SS",
                                      "R_EQ_I_SK", "R_LT_F_SS", "R_LT_F_SK", "R_LE_F_SS", "R_LE_F_SK",
                                      "R_GT_F_SS", "R_GT_F_SK", "R_GE_F_SS", "R_GE_F_SK", "R_EQ_F_SS",
                                      "R_EQ_F_SK", "R_JUMP_LT_I_SS", "R_JUMP_LT_I_SK", "R_JUMP_LE_I_SS", "R_JUMP_LE_I_SK",
                                      "R_JUMP_GT_I_SS", "R_JUMP_GT_I_SK", "R_JUMP_GE_I_SS", "R_JUMP_GE_I_SK", "R_JUMP_EQ_I_SS",
                                      "R_JUMP_EQ_I_SK", "R_JUMP_LT_F_SS", "R_JUMP_LT_F_SK", "R_JUMP_LE_F_SS", "R_JUMP_LE_F_SK",
                                      "R_JUMP_GT_F_SS", "R_JUMP_GT_F_SK", "R_JUMP_GE_F_SS", "R_JUMP_GE_F_SK", "R_JUMP_EQ_F_SS",
                                      "R_JUMP_EQ_F_SK", "R_GET_PATH",   "R_INVOKE", "R_CALL",     "R_GET_INDEX_PROP_SS", "R_GET_INDEX_PROP_SK", "TRANSLATE"};
    static_assert(sizeof(run) / sizeof(run[0]) == kOpCount - static_cast<size_t>(Op::MoveS), "a name for each");
    const size_t r = i - static_cast<size_t>(Op::MoveS);
    return i >= static_cast<size_t>(Op::MoveS) && r < sizeof(run) / sizeof(run[0]) ? run[r] : "?";
}

void disassemble(const Program& program, std::ostream& out) {
    // Module 0 is the standard library, the same in every program, and is left out.
    for (size_t i = 1; i < program.modules.size(); i++)
        if (program.modules[i] != nullptr) disassembleFunction(*program.modules[i], program, out);
}

// ---- Bytecode files ----

// The library keys given numbers so far, for every VM of the process: a program's are known
// before it runs, while its workers, each with a VM of its own, run its functions.
namespace {
std::mutex libraryMutex;
// With libraryMutex held.
uint16_t libraryNumber(const std::string& key) {
    static std::unordered_map<std::string, uint16_t> numbers{{"p:<builtin>", kEverywhere}};
    auto [at, added] = numbers.emplace(key, static_cast<uint16_t>(numbers.size() + 1));
    if (added && numbers.size() >= 0xFFFF) throw std::runtime_error("a process cannot load more than 65534 libraries");
    return at->second;
}
}  // namespace

uint16_t extensionLibrary(const std::string& key) {
    std::lock_guard<std::mutex> lock(libraryMutex);
    return libraryNumber(key);
}

const FileReach* makeReach(std::vector<std::string> keys) {
    std::lock_guard<std::mutex> lock(libraryMutex);
    static std::map<std::vector<std::string>, std::unique_ptr<FileReach>> made;
    std::unique_ptr<FileReach>& reach = made[keys];
    if (reach == nullptr) {
        reach = std::make_unique<FileReach>();
        for (const std::string& key : keys) {
            const uint16_t library = libraryNumber(key);
            const size_t word = library >> 6;
            if (reach->bits.size() <= word) reach->bits.resize(word + 1, 0);
            reach->bits[word] |= uint64_t{1} << (library & 63);
        }
        reach->keys = std::move(keys);
    }
    return reach.get();
}

void FunctionProto::noteLibrary() {
    const size_t mark = signature.rfind(kLibraryMark);
    library = mark == std::string::npos ? 0 : extensionLibrary(signature.substr(mark + 1));
}

namespace {

const char kMagic[4] = {'A', 'S', 'T', 'R'};
const uint8_t kFormatVersion = 35;  // 35: a program's sources for Compiler.compile (2026-11-21); 34: private names (2026-11-20); 33: JumpIfNotClass (2026-11-13)
const int kMaxProtoNesting = 200;

enum ConstantTag : uint8_t { TagInt = 0, TagFloat = 1, TagString = 2, TagChar = 3, TagByte = 4, TagPrivateName = 5 };

struct Writer {
    std::ostream& out;
    std::unordered_map<const FileReach*, uint32_t> reaches{};  // those written already, by their place

    void u8(uint8_t v) { out.put(static_cast<char>(v)); }
    void u16(uint16_t v) {
        u8(static_cast<uint8_t>(v & 0xFF));
        u8(static_cast<uint8_t>(v >> 8));
    }
    void u32(uint32_t v) {
        for (int i = 0; i < 4; i++) u8(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
    }
    void u64(uint64_t v) {
        for (int i = 0; i < 8; i++) u8(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
    }
    void str(const std::string& s) {
        u32(static_cast<uint32_t>(s.size()));
        out.write(s.data(), static_cast<std::streamsize>(s.size()));
    }

    void proto(const FunctionProto& f) {
        str(f.name);
        str(f.source);
        u32(static_cast<uint32_t>(f.arity));
        u32(static_cast<uint32_t>(f.requiredArgs()));
        u32(static_cast<uint32_t>(f.varargIndex + 1));
        u64(f.classParams);
        u8(f.typeArgs);
        u8(f.typeArgBase);
        u32(static_cast<uint32_t>(f.paramNames.size()));
        for (const ObjString* param : f.paramNames) str(param->utf8());

        u32(static_cast<uint32_t>(f.upvalues.size()));
        for (const UpvalueDesc& uv : f.upvalues) {
            u8(uv.fromCapture ? 2 : uv.isLocal ? 1 : 0);
            u16(uv.index);
        }
        u32(static_cast<uint32_t>(f.outerReceivers.size()));
        for (uint16_t upvalue : f.outerReceivers) u16(upvalue);
        u32(static_cast<uint32_t>(f.classIds.size()));
        for (uint16_t id : f.classIds) u16(id);
        u16(f.ownClass);
        u8(f.chooser ? 1 : 0);
        str(f.signature);
        // A reach: 0 for none, the place of one written before, or all ones and the keys.
        if (f.reach == nullptr) {
            u32(0);
        } else if (auto known = reaches.find(f.reach); known != reaches.end()) {
            u32(known->second);
        } else {
            u32(0xFFFFFFFF);
            u32(static_cast<uint32_t>(f.reach->keys.size()));
            for (const std::string& key : f.reach->keys) str(key);
            const uint32_t place = static_cast<uint32_t>(reaches.size()) + 1;
            reaches.emplace(f.reach, place);
        }
        u32(static_cast<uint32_t>(f.rights.size()));
        for (const FunctionProto::Rights& r : f.rights) {
            u32(r.start);
            u32(r.end);
            u8(static_cast<uint8_t>(r.ids.size()));
            for (uint16_t id : r.ids) u16(id);
        }
        u32(static_cast<uint32_t>(f.paramTypes.size()));
        for (size_t i = 0; i < f.paramTypes.size(); i++) {
            u8(f.paramTypes[i]);
            str(i < f.paramClasses.size() && f.paramClasses[i] != nullptr ? f.paramClasses[i]->utf8() : std::string());
            u8(i < f.paramCounts.size() ? f.paramCounts[i] : 0);
        }

        // The body: its length, then the body written with what it needs of its own (its
        // reaches in full), so that it can be read by itself when first asked for. One read
        // without its body is written as it was read.
        if (f.laterBody != nullptr) {
            str(f.laterBody->bytes);
            return;
        }
        std::ostringstream apart;
        Writer own{apart};
        own.body(f);
        str(apart.str());
    }

    void body(const FunctionProto& f) {
        u32(static_cast<uint32_t>(f.code.size()));
        out.write(reinterpret_cast<const char*>(f.code.data()), static_cast<std::streamsize>(f.code.size()));
        u32(static_cast<uint32_t>(f.spans.size()));
        for (const DeclarationSpan& span : f.spans) {
            u32(span.start);
            u32(span.end);
            u16(span.key);
            u8(span.kind);
        }
        // Notes on reflection, which only a program not yet built has: a byte saying which.
        static const FunctionProto::Reflection none;
        const FunctionProto::Reflection& noted = f.reflection != nullptr ? *f.reflection : none;
        u8(static_cast<uint8_t>((noted.notes.empty() ? 0 : 1) | (noted.supertypes.empty() ? 0 : 2)));
        if (!noted.notes.empty()) u32(static_cast<uint32_t>(noted.notes.size()));
        for (const FunctionProto::ReflectNote& note : noted.notes) {
            u32(note.at);
            u8(note.flags);
            u16(static_cast<uint16_t>(note.globals.size()));
            for (uint16_t global : note.globals) u16(global);
        }
        if (!noted.supertypes.empty()) u32(static_cast<uint32_t>(noted.supertypes.size()));
        for (const std::vector<uint16_t>& entry : noted.supertypes) {
            u16(static_cast<uint16_t>(entry.size()));
            for (uint16_t global : entry) u16(global);
        }

        // Line table, run-length encoded.
        std::vector<std::pair<uint32_t, uint32_t>> runs;
        for (int line : f.lines) {
            if (!runs.empty() && runs.back().first == static_cast<uint32_t>(line)) runs.back().second++;
            else runs.emplace_back(static_cast<uint32_t>(line), 1u);
        }
        u32(static_cast<uint32_t>(runs.size()));
        for (const auto& run : runs) {
            u32(run.first);
            u32(run.second);
        }
        u32(static_cast<uint32_t>(f.inlined.size()));
        for (const FunctionProto::InlinedCall& call : f.inlined) {
            str(call.name);
            str(call.source);
            u32(static_cast<uint32_t>(call.line));
            u32(static_cast<uint32_t>(call.callLine));
        }

        u32(static_cast<uint32_t>(f.constants.size()));
        for (const Value& c : f.constants) {
            if (c.isInt()) {
                u8(TagInt);
                u64(static_cast<uint64_t>(c.as.i));
            } else if (c.isFloat()) {
                uint64_t bits;
                std::memcpy(&bits, &c.as.d, sizeof bits);
                u8(TagFloat);
                u64(bits);
            } else if (c.isChar() || c.isByte()) {
                u8(c.isChar() ? TagChar : TagByte);
                u64(static_cast<uint64_t>(c.as.i));
            } else if (const std::string* owner = VM::privateOwner(asString(c))) {
                u8(TagPrivateName);  // a class's private name (o-private-shadow): its owner and text
                str(*owner);
                str(asString(c)->utf8());
            } else {
                u8(TagString);
                str(asString(c)->utf8());
            }
        }

        // Each nested function: 0 and the function, or 1 for a class's description, kept apart:
        // its typeArgs, then its length and the function, written with what it needs of its own
        // (its reaches in full) so that it can be read by itself (G47).
        u32(static_cast<uint32_t>(f.protos.size()));
        for (const FunctionProto* nested : f.protos) {
            if (!nested->describes && nested->deferred == nullptr) {
                u8(0);
                proto(*nested);
                continue;
            }
            std::string bytes;
            if (nested->deferred != nullptr) {
                bytes = nested->deferred->bytes;
            } else {
                std::ostringstream apart;
                Writer own{apart};
                own.proto(*nested);
                bytes = apart.str();
            }
            u8(1);
            u8(nested->typeArgs);
            str(bytes);
        }
    }
};

struct Reader {
    const std::string& data;
    Program& program;
    VM& vm;
    size_t pos = 0;
    std::vector<const FileReach*> reaches{};  // as they were written
    bool defer = false;                     // a class's description left to be read when asked for
    bool lazyBodies = false;                // a function's body left to be read when asked for (loadBody)

    [[noreturn]] void corrupt() { throw std::runtime_error("corrupt or truncated bytecode file"); }

    void need(size_t n) {
        if (n > data.size() - pos) corrupt();
    }
    uint8_t u8() {
        need(1);
        return static_cast<uint8_t>(data[pos++]);
    }
    // Little-endian numbers, each read with one look at what is left: a kept program's start reads
    // hundreds of thousands of them (2026-11-09).
    uint16_t u16() {
        need(2);
        const auto* b = reinterpret_cast<const uint8_t*>(data.data() + pos);
        pos += 2;
        return static_cast<uint16_t>(b[0] | (b[1] << 8));
    }
    uint32_t u32() {
        need(4);
        const auto* b = reinterpret_cast<const uint8_t*>(data.data() + pos);
        pos += 4;
        return static_cast<uint32_t>(b[0]) | static_cast<uint32_t>(b[1]) << 8 | static_cast<uint32_t>(b[2]) << 16 |
               static_cast<uint32_t>(b[3]) << 24;
    }
    uint64_t u64() {
        const uint64_t lo = u32();
        return lo | static_cast<uint64_t>(u32()) << 32;
    }
    // A count of items that each take at least `itemSize` bytes in the file.
    uint32_t count(size_t itemSize) {
        uint32_t n = u32();
        if (n > (data.size() - pos) / itemSize) corrupt();
        return n;
    }
    std::string str() {
        uint32_t n = count(1);
        std::string s = data.substr(pos, n);
        pos += n;
        return s;
    }
    // The same where it is only looked at (interned, say): no copy made.
    std::string_view view() {
        uint32_t n = count(1);
        std::string_view s(data.data() + pos, n);
        pos += n;
        return s;
    }

    FunctionProto* proto(int nesting) {
        if (nesting > kMaxProtoNesting) corrupt();
        FunctionProto* f = program.newProto();
        f->name = str();
        f->source = str();
        f->arity = static_cast<int>(u32());
        f->minArity = static_cast<int>(u32());
        if (f->minArity < 0 || f->minArity > f->arity) corrupt();
        uint32_t vararg = u32();
        if (vararg > static_cast<uint32_t>(f->arity)) corrupt();
        f->varargIndex = static_cast<int>(vararg) - 1;
        f->classParams = u64();
        f->typeArgs = u8();
        if ((f->typeArgs & ~(kTypeArgsOwn | kTypeArgsKept | kTakesDestination | kHasPlaces)) != 0) corrupt();
        f->typeArgBase = u8();
        uint32_t paramCount = count(4);
        for (uint32_t i = 0; i < paramCount; i++) f->paramNames.push_back(vm.intern(view()));

        uint32_t upvalueCount = count(3);
        for (uint32_t i = 0; i < upvalueCount; i++) {
            const uint8_t kind = u8();
            if (kind > 2) corrupt();
            f->upvalues.push_back(UpvalueDesc{kind == 1, u16(), kind == 2});
        }
        uint32_t receiverCount = count(2);
        for (uint32_t i = 0; i < receiverCount; i++) {
            uint16_t upvalue = u16();
            if ((upvalue & kCapturedReceiver) == 0 && upvalue >= f->upvalues.size()) corrupt();
            f->outerReceivers.push_back(upvalue);
        }
        uint32_t classCount = count(2);
        for (uint32_t i = 0; i < classCount; i++) f->classIds.push_back(u16());
        f->ownClass = u16();
        f->chooser = u8() != 0;
        f->signature = str();
        f->noteLibrary();
        if (const uint32_t reach = u32(); reach == 0xFFFFFFFF) {
            const uint32_t keyCount = count(4);
            std::vector<std::string> keys;
            for (uint32_t i = 0; i < keyCount; i++) keys.push_back(str());
            reaches.push_back(makeReach(std::move(keys)));
            f->reach = reaches.back();
        } else if (reach != 0) {
            if (reach > reaches.size()) corrupt();
            f->reach = reaches[reach - 1];
        }
        const uint32_t rightsCount = count(9);
        for (uint32_t i = 0; i < rightsCount; i++) {
            FunctionProto::Rights r;
            r.start = u32();
            r.end = u32();
            const uint8_t ids = u8();
            for (uint8_t k = 0; k < ids; k++) r.ids.push_back(u16());
            f->rights.push_back(std::move(r));
        }
        uint32_t typed = count(2);
        if (typed != 0 && typed != static_cast<uint32_t>(f->arity)) corrupt();
        for (uint32_t i = 0; i < typed; i++) {
            f->paramTypes.push_back(u8());
            const std::string cls = str();
            f->paramClasses.push_back(cls.empty() ? nullptr : vm.intern(cls));
            f->paramCounts.push_back(u8());
        }

        // The body: kept as its bytes where it may be read later, else read now, in place.
        const uint32_t bodySize = count(1);
        if (lazyBodies && !f->chooser) {
            f->laterBody = std::make_unique<FunctionProto::LaterBody>(
                FunctionProto::LaterBody{data.substr(pos, bodySize), &vm, &program, nesting});
        } else {
            Reader own{data, program, vm, pos};
            own.defer = defer;
            own.lazyBodies = lazyBodies;
            own.body(f, nesting);
            if (own.pos != pos + bodySize) corrupt();
        }
        pos += bodySize;
        return f;
    }

    void body(FunctionProto* f, int nesting) {
        uint32_t codeSize = count(1);
        f->code.assign(data.begin() + static_cast<std::ptrdiff_t>(pos),
                       data.begin() + static_cast<std::ptrdiff_t>(pos + codeSize));
        pos += codeSize;
        uint32_t spanCount = count(11);
        for (uint32_t i = 0; i < spanCount; i++) {
            DeclarationSpan span;
            span.start = u32();
            span.end = u32();
            span.key = u16();
            const uint8_t kind = u8();
            if (kind > DeclarationSpan::ReflectMethods || span.start > span.end || span.end > codeSize) corrupt();
            span.kind = static_cast<DeclarationSpan::Kind>(kind);
            f->spans.push_back(span);
        }
        const uint8_t noted = u8();
        if (noted > 3) corrupt();
        const uint32_t noteCount = (noted & 1) != 0 ? count(7) : 0;
        for (uint32_t i = 0; i < noteCount; i++) {
            FunctionProto::ReflectNote note;
            note.at = u32();
            note.flags = u8();
            const uint16_t globals = u16();
            for (uint16_t k = 0; k < globals; k++) note.globals.push_back(u16());
            if (note.at >= codeSize) corrupt();
            f->noted().notes.push_back(std::move(note));
        }
        const uint32_t supertypeCount = (noted & 2) != 0 ? count(2) : 0;
        for (uint32_t i = 0; i < supertypeCount; i++) {
            std::vector<uint16_t> entry;
            const uint16_t globals = u16();
            for (uint16_t k = 0; k < globals; k++) entry.push_back(u16());
            f->noted().supertypes.push_back(std::move(entry));
        }

        uint32_t runs = count(8);
        f->lines.reserve(codeSize);  // a line for each byte of the code
        for (uint32_t i = 0; i < runs; i++) {
            uint32_t line = u32();
            uint32_t length = u32();
            if (length > codeSize - f->lines.size()) corrupt();
            f->lines.insert(f->lines.end(), length, static_cast<int>(line));
        }
        const uint32_t inlinedCount = count(16);
        for (uint32_t i = 0; i < inlinedCount; i++) {
            FunctionProto::InlinedCall call;
            call.name = str();
            call.source = str();
            call.line = static_cast<int>(u32());
            call.callLine = static_cast<int>(u32());
            f->inlined.push_back(std::move(call));
        }

        uint32_t constantCount = count(1);
        for (uint32_t i = 0; i < constantCount; i++) {
            switch (u8()) {
                case TagInt:
                    f->constants.push_back(Value::integer(static_cast<int64_t>(u64())));
                    break;
                case TagFloat: {
                    uint64_t bits = u64();
                    double d;
                    std::memcpy(&d, &bits, sizeof d);
                    f->constants.push_back(Value::number(d));
                    break;
                }
                case TagString:
                    f->constants.push_back(Value::object(vm.intern(view())));
                    break;
                case TagPrivateName: {
                    const std::string owner(view());
                    f->constants.push_back(Value::object(vm.privateName(owner, view())));
                    break;
                }
                case TagChar: {
                    const uint64_t c = u64();
                    if (!isCharacter(static_cast<int64_t>(c))) corrupt();
                    f->constants.push_back(Value::character(static_cast<uint32_t>(c)));
                    break;
                }
                case TagByte: {
                    const uint64_t b = u64();
                    if (b > 255) corrupt();
                    f->constants.push_back(Value::byte(static_cast<uint8_t>(b)));
                    break;
                }
                default:
                    corrupt();
            }
        }

        uint32_t protoCount = count(1);
        for (uint32_t i = 0; i < protoCount; i++) {
            const uint8_t kind = u8();
            if (kind == 0) {
                f->protos.push_back(proto(nesting + 1));
                continue;
            }
            if (kind != 1) corrupt();
            // A class's description, kept apart: read now, or when it is asked for.
            const uint8_t typeArgs = u8();
            const uint32_t size = count(1);
            FunctionProto* described;
            if (defer) {
                described = program.newProto();
                described->deferred = std::make_unique<FunctionProto::Deferred>(FunctionProto::Deferred{data.substr(pos, size), &vm, &program});
            } else {
                Reader apart{data, program, vm, pos};
                described = apart.proto(nesting + 1);
                if (apart.pos != pos + size) corrupt();
            }
            described->describes = true;
            described->typeArgs = typeArgs;
            pos += size;
            f->protos.push_back(described);
        }
    }
};

void verifyTree(FunctionProto& f, const Program& program, bool now = false, bool trusted = false);

}  // namespace

void verifyBody(FunctionProto& f, const Program& program);

void loadBody(FunctionProto& f) {
    static std::mutex loading;
    std::lock_guard<std::mutex> hold(loading);
    if (f.laterBody == nullptr) return;
    const std::unique_ptr<FunctionProto::LaterBody> kept = std::move(f.laterBody);
    Reader r{kept->bytes, *kept->program, *kept->vm, 0};
    r.lazyBodies = true;
    r.body(&f, kept->nesting);
    if (r.pos != kept->bytes.size()) r.corrupt();
    // What its header's checks left for the body, and its nested functions' headers, as a
    // program's are as it is read: theirs verified at their first calls.
    verifyBody(f, *kept->program);
    for (FunctionProto* nested : f.protos) verifyTree(*nested, *kept->program, false, true);
}

void loadDeferred(FunctionProto& f, VM* reader) {
    static std::mutex loading;
    std::lock_guard<std::mutex> hold(loading);
    if (f.deferred == nullptr) return;
    const std::unique_ptr<FunctionProto::Deferred> kept = std::move(f.deferred);
    // Read with no `defer` and no `lazyBodies`, so the VM makes only names (intern), and keeps
    // no pointer to it.
    Reader r{kept->bytes, *kept->program, reader != nullptr ? *reader : *kept->vm, 0};
    FunctionProto* read = r.proto(1);
    if (r.pos != kept->bytes.size()) r.corrupt();
    verifyTree(*read, *kept->program, true);  // read when asked for, perhaps in a parallel section: made now
    f = std::move(*read);
    f.describes = true;
}

namespace {

void verifyTree(FunctionProto& f, const Program& program, bool now, bool trusted) {
    if (f.deferred != nullptr) return;  // verified when it is read
    verifyFunction(f, program, now, trusted);
    for (FunctionProto* nested : f.protos) verifyTree(*nested, program, now, trusted);
}

void writeU16(std::vector<uint8_t>& code, size_t at, int v) {
    code[at] = static_cast<uint8_t>(v & 0xFF);
    code[at + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
}

}  // namespace

void renumber(FunctionProto& f, const Renumbering& r, VM& vm) {
    if (f.deferred != nullptr) loadDeferred(f);
    std::vector<uint8_t>& code = f.code;
    for (DeclarationSpan& span : f.spans)
        if (span.kind != DeclarationSpan::Extension) span.key = static_cast<uint16_t>(r.global(span.key));
    for (size_t ip = 0; ip < code.size();) {
        const uint8_t raw = code[ip];
        if (raw >= static_cast<uint8_t>(Op::Count_)) break;  // malformed: verifying will say so
        const size_t size = 1 + operandSize(kOps[raw].operand);
        if (ip + size > code.size()) break;
        auto first = [&](const std::function<int(int)>& map) { writeU16(code, ip + 1, map(readU16(code, ip + 1))); };
        auto second = [&](const std::function<int(int)>& map) { writeU16(code, ip + 3, map(readU16(code, ip + 3))); };
        switch (static_cast<Op>(raw)) {
            case Op::GetGlobal:
            case Op::SetGlobal:
            case Op::DefineGlobal:
            case Op::DefineTypedGlobal:
            case Op::PlaceGlobal:     // a global reached by a body put in place of its call
            case Op::JumpIfNotClass:  // the class a body put in place was made for
                first(r.global);
                break;
            case Op::ObjectClass:
            case Op::StoreClass:
            case Op::ObjectClassTyped:
            case Op::StoreClassTyped:
                if (readU16(code, ip + 1) != 0xFFFF) first(r.global);  // 0xFFFF: kept nowhere
                break;
            case Op::GetImplicit:
            case Op::SetImplicit:
            case Op::InvokeImplicit: second(r.global); break;
            case Op::Import: first(r.module); break;
            case Op::ClassNumber:
                first(r.number);
                second(r.number);
                break;
            case Op::MemberAccess:
            case Op::StaticAccess: second(r.number); break;
            default: break;
        }
        ip += size;
    }
    for (uint16_t& id : f.classIds) id = static_cast<uint16_t>(r.number(id));
    for (FunctionProto::Rights& rights : f.rights)
        for (uint16_t& id : rights.ids) id = static_cast<uint16_t>(r.number(id));
    if (f.reflection != nullptr) {
        for (FunctionProto::ReflectNote& note : f.reflection->notes)
            for (uint16_t& global : note.globals) global = static_cast<uint16_t>(r.global(global));
        for (std::vector<uint16_t>& entry : f.reflection->supertypes)
            for (uint16_t& global : entry)
                if (global != kNoGlobal) global = static_cast<uint16_t>(r.global(global));
    }
    if (f.ownClass != 0) f.ownClass = static_cast<uint16_t>(r.number(f.ownClass));
    // The script a worker runs is named by a string the compiler made: "\x01<module>\x01<path>".
    for (Value& c : f.constants) {
        if (!c.isObjType(ObjType::String)) continue;
        const std::string& text = asString(c)->utf8();
        if (text.size() < 3 || text[0] != '\x01') continue;
        const size_t end = text.find('\x01', 1);
        if (end == std::string::npos) continue;
        const int module = r.module(std::stoi(text.substr(1, end - 1)));
        c = Value::object(vm.intern('\x01' + std::to_string(module) + text.substr(end)));
    }
    for (FunctionProto* nested : f.protos) renumber(*nested, r, vm);
}

void writeModules(const std::vector<const FunctionProto*>& modules, std::ostream& out) {
    Writer w{out};
    w.u8(kFormatVersion);
    w.u32(static_cast<uint32_t>(modules.size()));
    for (const FunctionProto* module : modules) w.proto(*module);
}

std::vector<FunctionProto*> readModules(Program& program, VM& vm, const std::string& data) {
    Reader r{data, program, vm, 0};
    if (r.u8() != kFormatVersion) throw std::runtime_error("kept from another version of the bytecode");
    std::vector<FunctionProto*> modules;
    const uint32_t count = r.count(1);
    for (uint32_t i = 0; i < count; i++) modules.push_back(r.proto(0));
    if (r.pos != data.size()) r.corrupt();
    return modules;
}

void verifyModule(FunctionProto& module, const Program& program, bool trusted) {
    if (module.arity != 0 || !module.upvalues.empty())
        throw std::runtime_error("invalid bytecode: a module cannot take arguments or capture variables");
    verifyTree(module, program, false, trusted);
}

void writeProgram(const Program& program, std::ostream& out) {
    Writer w{out};
    out.write(kMagic, sizeof kMagic);
    w.u8(kFormatVersion);

    w.u32(static_cast<uint32_t>(program.globalNames.size()));
    for (const std::string& name : program.globalNames) w.str(name);

    w.u32(static_cast<uint32_t>(program.modules.size()));
    for (const FunctionProto* module : program.modules) w.proto(*module);
    w.u32(static_cast<uint32_t>(program.entry));

    w.u32(static_cast<uint32_t>(program.compiledFrom.size()));
    for (const Program::Source& source : program.compiledFrom) {
        w.str(source.key);
        w.str(source.text);
    }
    w.str(program.compiledMain);
    w.u32(static_cast<uint32_t>(program.compiledLibraries.size()));
    for (const std::string& library : program.compiledLibraries) w.str(library);
}

bool looksLikeBytecode(const std::string& data) {
    return data.size() >= sizeof kMagic && std::memcmp(data.data(), kMagic, sizeof kMagic) == 0;
}

void readProgram(Program& program, VM& vm, const std::string& data, bool trusted) {
    if (!looksLikeBytecode(data)) throw std::runtime_error("not a WinterWolf bytecode file");
    Reader r{data, program, vm, sizeof kMagic};
    r.defer = true;
    r.lazyBodies = trusted;  // ww's own making: each function's body read when it is first asked for

    uint8_t version = r.u8();
    if (version != kFormatVersion)
        throw std::runtime_error("unsupported bytecode version " + std::to_string(version) +
                                 " (this build reads version " + std::to_string(kFormatVersion) + ")");

    uint32_t globalCount = r.count(4);
    if (globalCount > 65536) r.corrupt();
    for (uint32_t i = 0; i < globalCount; i++) program.globalNames.push_back(r.str());

    uint32_t moduleCount = r.count(1);
    if (moduleCount == 0 || moduleCount > 65536) r.corrupt();
    for (uint32_t i = 0; i < moduleCount; i++) program.modules.push_back(r.proto(0));

    uint32_t entry = r.u32();
    if (entry >= moduleCount) r.corrupt();
    program.entry = static_cast<int>(entry);

    const uint32_t sources = r.count(8);
    for (uint32_t i = 0; i < sources; i++) {
        Program::Source source;
        source.key = r.str();
        source.text = r.str();
        program.compiledFrom.push_back(std::move(source));
    }
    program.compiledMain = r.str();
    const uint32_t libraries = r.count(4);
    for (uint32_t i = 0; i < libraries; i++) program.compiledLibraries.push_back(r.str());

    for (FunctionProto* module : program.modules) {
        if (module->arity != 0 || !module->upvalues.empty())
            throw std::runtime_error("invalid bytecode: a module cannot take arguments or capture variables");
        verifyTree(*module, program, false, trusted);
    }
}


// ---- What a built program can never reach, left out (M6.5 part 5) ----
//
// The code of every module runs, but what lies inside a top-level declaration's span only
// makes that declaration (a function, or a class with its methods). Starting from the modules,
// everything their code can reach is followed: the globals it names, and for each global the
// spans that make it; the functions the reached code makes. The spans of the globals never
// reached are taken out of their modules' code, with the functions only they made.

namespace {

// Which operand of an instruction is a global's slot: 1 the first, 2 the second, 0 none.
int globalOperand(Op op, int first) {
    switch (op) {
        case Op::GetGlobal:
        case Op::SetGlobal:
        case Op::DefineGlobal:
        case Op::DefineTypedGlobal:
        case Op::PlaceGlobal:
        case Op::JumpIfNotClass: return 1;
        case Op::ObjectClass:
        case Op::StoreClass:
        case Op::ObjectClassTyped:
        case Op::StoreClassTyped: return first == 0xFFFF ? 0 : 1;
        case Op::GetImplicit:
        case Op::SetImplicit:
        case Op::InvokeImplicit: return 2;
        default: return 0;
    }
}

}  // namespace

int targetOperand(Op op) {
    switch (op) {
        case Op::Jump:
        case Op::Loop:
        case Op::JumpIfFalse:
        case Op::And:
        case Op::Or:
        case Op::JumpIfNull:
        case Op::JumpIfNotNull:
        case Op::TryBegin: return 1;
        case Op::AddNew:
        case Op::InsertNew: return 2;
        case Op::ObjectClass:
        case Op::ObjectClassTyped:
        case Op::CachedType:
        case Op::TagCollection:
        case Op::RangeNext:
        case Op::TryImplicit:
        case Op::IterNext:
        case Op::IterNextRef:
        case Op::IterNextPair:
        case Op::JumpIfNotClass:
        case Op::DefaultArg: return 3;
        default: return 0;
    }
}

namespace {

// An instruction whose first operand is the name of a member it uses.
bool usesMemberName(Op op) {
    switch (op) {
        case Op::GetProp:
        case Op::SetProp:
        case Op::GetOwn:
        case Op::SetOwn:
        case Op::GetPropRef:
        case Op::GetOwnRef:
        case Op::FieldDestination:
        case Op::Invoke:
        case Op::InvokeOwn:
        case Op::InvokeTrailing:
        case Op::InvokeOwnTrailing:
        case Op::SuperInvoke:
        case Op::SuperInvokeTrailing:
        case Op::GetSuper:
        case Op::SetSuper:
        case Op::GetImplicit:
        case Op::SetImplicit:
        case Op::TryImplicit:
        case Op::InvokeImplicit: return true;
        default: return false;
    }
}

// An instruction that makes a function of the code's own nested ones: its first operand.
bool makesFunction(Op op) {
    return op == Op::Closure || op == Op::MakeForms || op == Op::MethodForms || op == Op::StaticForms || op == Op::ClassMeta;
}

struct Reach {
    Program& program;
    std::vector<char> reachedGlobal;
    std::unordered_set<std::string> reachedName;
    std::unordered_set<const FunctionProto*> reachedProto;
    // An extension's name, and where the spans that make extensions of that name are.
    std::unordered_map<std::string, std::vector<std::pair<FunctionProto*, size_t>>> extensionsNamed;
    // A global, and where the spans that make it are: the module, and the span's place in it.
    std::unordered_map<int, std::vector<std::pair<FunctionProto*, size_t>>> spansOf;
    std::vector<std::pair<FunctionProto*, size_t>> spanWork;  // spans to look through
    std::vector<FunctionProto*> protoWork;                   // whole functions to look through

    // Reflection (D50, G42). The `#`s reached, each with the classes the Class it gives may be
    // and what is asked of it (FunctionProto::ReflectNote), and the classes the descriptions
    // reached name, given on; whether code reached asks for the methods of a Class given on.
    // What each class is besides, both ways: a class by the global of its top-level
    // declaration, one declared inside a function after the globals. The classes whose
    // fields', and whose methods', descriptions are kept, with what is above them (1), and
    // with what is below them too (2); or all. The spans describing classes (Op::ClassMeta),
    // which of them are kept, and the functions they make, which are the descriptions.
    struct Asked {
        std::vector<uint16_t> globals;
        uint8_t flags = 0;
    };
    std::vector<Asked> asked;
    size_t settled = 0;
    bool methodsAsked = false, settledMethods = false;
    std::vector<std::vector<int>> above, below;
    std::vector<char> fieldsKept, methodsKept;
    bool allFields = false, allMethods = false;
    std::vector<std::pair<FunctionProto*, size_t>> metaSpans;
    std::set<std::pair<const FunctionProto*, size_t>> metaKept;
    std::unordered_set<const FunctionProto*> descriptions;
    std::vector<uint16_t>* named = nullptr;  // where the globals a description names go

    explicit Reach(Program& p) : program(p), reachedGlobal(p.globalNames.size(), 0), above(p.globalNames.size()), below(p.globalNames.size()) {}

    // A class's entry of FunctionProto::supertypes.
    void relate(const std::vector<uint16_t>& entry) {
        const size_t globals = reachedGlobal.size();
        if (entry.size() < 2 || (entry[0] >= globals && entry[0] != kNoGlobal)) return;
        size_t self = entry[0];
        if (entry[0] == kNoGlobal) {
            self = above.size();
            above.emplace_back();
            below.emplace_back();
        }
        for (size_t i = 1; i < entry.size(); i++) {
            if (entry[i] >= globals || entry[i] == self) continue;
            above[self].push_back(entry[i]);
            below[entry[i]].push_back(static_cast<int>(self));
        }
    }

    // The note on the instruction at `at`: one saying it asks nothing of methods, or that of a `#`.
    static const FunctionProto::ReflectNote* noteAt(const FunctionProto& f, size_t at, bool notMethods) {
        if (f.reflection == nullptr) return nullptr;
        const std::vector<FunctionProto::ReflectNote>& notes = f.reflection->notes;
        auto first = std::lower_bound(notes.begin(), notes.end(), at,
                                      [](const FunctionProto::ReflectNote& note, size_t place) { return note.at < place; });
        for (; first != notes.end() && first->at == at; ++first)
            if (((first->flags & kReflectNotMethods) != 0) == notMethods && (first->flags & kReflectDescribes) == 0) return &*first;
        return nullptr;
    }

    // The note of the Op::ClassMeta at `at` of a class no global holds (G43), if any.
    static const FunctionProto::ReflectNote* describesAt(const FunctionProto& f, size_t at) {
        if (f.reflection == nullptr) return nullptr;
        for (const FunctionProto::ReflectNote& note : f.reflection->notes)
            if (note.at == at && (note.flags & kReflectDescribes) != 0) return &note;
        return nullptr;
    }

    // The descriptions of the classes no global holds (G43), each the class's node in what is
    // above and below and whether it is of its fields (0) or methods (1): reached only where
    // that is asked for, and emptied where it never is.
    struct Unheld {
        FunctionProto* description;
        int node;
        int which;
        bool reached = false;
    };
    std::vector<Unheld> unheld;

    // A node for such a class, below the classes `supertypes` names; marked kept at once where
    // what is below one of them is kept already.
    int nodeBelow(const std::vector<uint16_t>& supertypes) {
        const int self = static_cast<int>(above.size());
        above.emplace_back();
        below.emplace_back();
        for (uint16_t cls : supertypes) {
            if (cls >= reachedGlobal.size()) continue;
            above[static_cast<size_t>(self)].push_back(cls);
            below[cls].push_back(self);
        }
        if (!fieldsKept.empty()) {
            fieldsKept.push_back(0);
            methodsKept.push_back(0);
            for (std::vector<char>* kept : {&fieldsKept, &methodsKept})
                for (uint16_t cls : supertypes)
                    if (cls < reachedGlobal.size() && (*kept)[cls] == 2) keep(*kept, self, false);
        }
        return self;
    }

    // A class, and what is above it, marked kept in `kept`; what is below it too unless
    // `exact`. A class marked has what is above it marked (1), and with 2 what is below too.
    void keep(std::vector<char>& kept, int cls, bool exact) {
        std::vector<int> up;
        if (exact) {
            if (kept[static_cast<size_t>(cls)] == 0) {
                kept[static_cast<size_t>(cls)] = 1;
                up.push_back(cls);
            }
        } else {
            std::vector<int> down{cls};
            while (!down.empty()) {
                const int k = down.back();
                down.pop_back();
                if (kept[static_cast<size_t>(k)] == 2) continue;
                if (kept[static_cast<size_t>(k)] == 0) up.push_back(k);
                kept[static_cast<size_t>(k)] = 2;
                for (int b : below[static_cast<size_t>(k)]) down.push_back(b);
            }
        }
        while (!up.empty()) {
            const int k = up.back();
            up.pop_back();
            for (int a : above[static_cast<size_t>(k)])
                if (kept[static_cast<size_t>(a)] == 0) {
                    kept[static_cast<size_t>(a)] = 1;
                    up.push_back(a);
                }
        }
    }

    // What the `#`s reached since last time ask, and what those given on ask now that methods
    // are asked for.
    void settle() {
        if (fieldsKept.empty()) {
            fieldsKept.assign(above.size(), 0);
            methodsKept.assign(above.size(), 0);
        }
        const bool again = methodsAsked && !settledMethods;
        settledMethods = methodsAsked;
        for (size_t i = again ? 0 : settled; i < asked.size(); i++) {
            const Asked& a = asked[i];
            const bool fields = (a.flags & kReflectFields) != 0;
            const bool givenOn = fields && (a.flags & kReflectMethods) == 0;
            if (i < settled && !givenOn) continue;
            const bool methods = (a.flags & kReflectMethods) != 0 || (givenOn && methodsAsked);
            if ((a.flags & kReflectAll) != 0) {
                allFields = allFields || fields;
                allMethods = allMethods || methods;
                continue;
            }
            for (uint16_t cls : a.globals) {
                if (fields) keep(fieldsKept, cls, (a.flags & kReflectExact) != 0);
                if (methods) keep(methodsKept, cls, (a.flags & kReflectExact) != 0);
            }
        }
        settled = asked.size();
    }

    void global(int slot) {
        if (slot < 0 || static_cast<size_t>(slot) >= reachedGlobal.size() || reachedGlobal[static_cast<size_t>(slot)]) return;
        reachedGlobal[static_cast<size_t>(slot)] = 1;
        auto found = spansOf.find(slot);
        if (found != spansOf.end())
            for (const auto& span : found->second) spanWork.push_back(span);
    }

    // A member name, without the signature a form of a name with several has after it
    // (`lastOrNull[List<T>]()`): extensions are matched by the name alone.
    static std::string bare(const std::string& member) { return member.substr(0, member.find('[')); }

    void name(const std::string& used) {
        const std::string member = bare(used);
        if (!reachedName.insert(member).second) return;
        auto found = extensionsNamed.find(member);
        if (found != extensionsNamed.end())
            for (const auto& span : found->second) spanWork.push_back(span);
    }

    // Whether the span was reached: its global, or for an extension its name.
    bool reached(const FunctionProto& module, const DeclarationSpan& span) const {
        if (span.kind == DeclarationSpan::Global) return reachedGlobal[span.key] != 0;
        if (span.kind == DeclarationSpan::Reflect || span.kind == DeclarationSpan::ReflectMethods)
            return metaKept.count({&module, static_cast<size_t>(&span - module.spans.data())}) != 0;
        return span.key < module.constants.size() && module.constants[span.key].isString() &&
               reachedName.count(bare(asString(module.constants[span.key])->utf8())) != 0;
    }

    void function(FunctionProto* f) {
        if (reachedProto.insert(f).second) protoWork.push_back(f);
    }

    // What the instructions of `f` from `start` to `end` reach; those inside one of its spans
    // only where `skipSpans` says so.
    void look(FunctionProto& f, size_t start, size_t end, bool skipSpans) {
        const std::vector<uint8_t>& code = f.code;
        for (size_t at = start; at < end && at < code.size();) {
            if (skipSpans) {
                bool skipped = false;
                for (const DeclarationSpan& span : f.spans)
                    if (span.start == at && span.end > at) {
                        at = span.end;
                        skipped = true;
                        break;
                    }
                if (skipped) continue;
            }
            const uint8_t raw = code[at];
            if (raw >= static_cast<uint8_t>(Op::Count_)) return;
            const Op op = static_cast<Op>(raw);
            int a = 0, b = 0;
            decodeOperands(code, at, kOps[raw].operand, a, b);
            const int which = globalOperand(op, a);
            if (which == 1) global(a);
            if (which == 2) global(b);
            if (named != nullptr && op == Op::GetGlobal && static_cast<size_t>(a) < reachedGlobal.size()) named->push_back(static_cast<uint16_t>(a));
            const FunctionProto::ReflectNote* unheldClass = op == Op::ClassMeta ? describesAt(f, at) : nullptr;
            if (unheldClass != nullptr && static_cast<size_t>(a) < f.protos.size()) {
                // Reached only where it is asked for (G43).
                unheld.push_back(Unheld{f.protos[static_cast<size_t>(a)], nodeBelow(unheldClass->globals), b});
            } else {
                if (op == Op::ClassMeta && static_cast<size_t>(a) < f.protos.size()) descriptions.insert(f.protos[static_cast<size_t>(a)]);
                if (makesFunction(op) && static_cast<size_t>(a) < f.protos.size()) function(f.protos[static_cast<size_t>(a)]);
            }
            if (usesMemberName(op) && static_cast<size_t>(a) < f.constants.size() && f.constants[static_cast<size_t>(a)].isString()) {
                const std::string& used = asString(f.constants[static_cast<size_t>(a)])->utf8();
                name(used);
                const std::string member = bare(used);
                if (!methodsAsked && (member == "methods" || member == "method" || member == "constructors" || member == "staticMethods" || member == "staticMethod") &&
                    noteAt(f, at, true) == nullptr)
                    methodsAsked = true;
            }
            if (op == Op::ClassOf) {
                // What is asked of the Class: as noted, or of every class, anything.
                const FunctionProto::ReflectNote* note = noteAt(f, at, false);
                if (note != nullptr) asked.push_back(Asked{note->globals, note->flags});
                else asked.push_back(Asked{{}, static_cast<uint8_t>(kReflectAll | kReflectFields)});
            }
            at += 1 + operandSize(kOps[raw].operand);
        }
    }

    void run() {
        for (;;) {
            while (!spanWork.empty() || !protoWork.empty()) {
                if (!spanWork.empty()) {
                    auto [module, index] = spanWork.back();
                    spanWork.pop_back();
                    const DeclarationSpan span = module->spans[index];
                    look(*module, span.start, span.end, false);
                } else {
                    FunctionProto* f = protoWork.back();
                    protoWork.pop_back();
                    if (descriptions.count(f) != 0) {
                        // A description: the classes it names (its fields' and parameters'
                        // types, its annotations) may be given on as Classes.
                        Asked given;
                        given.flags = kReflectExact | kReflectFields;
                        named = &given.globals;
                        look(*f, 0, f->code.size(), !f->spans.empty());
                        named = nullptr;
                        if (!given.globals.empty()) asked.push_back(std::move(given));
                    } else {
                        look(*f, 0, f->code.size(), !f->spans.empty());
                    }
                }
            }
            // What describes the classes asked of, with those above them and, for a value's
            // class, below: that may reach more.
            settle();
            bool more = false;
            for (const auto& [module, index] : metaSpans) {
                const DeclarationSpan& span = module->spans[index];
                const bool methods = span.kind == DeclarationSpan::ReflectMethods;
                if (!(methods ? allMethods : allFields) && (span.key >= fieldsKept.size() || (methods ? methodsKept : fieldsKept)[span.key] == 0)) continue;
                if (span.key >= reachedGlobal.size() || !reachedGlobal[span.key] || !metaKept.insert({module, index}).second) continue;
                spanWork.emplace_back(module, index);
                more = true;
            }
            for (Unheld& u : unheld) {
                if (u.reached || !(u.which == 0 ? allFields || fieldsKept[static_cast<size_t>(u.node)] != 0
                                                : allMethods || methodsKept[static_cast<size_t>(u.node)] != 0))
                    continue;
                u.reached = true;
                descriptions.insert(u.description);
                function(u.description);
                more = true;
            }
            if (!more) return;
        }
    }
};

// Takes the spans not reached out of a module's code, and the functions only they made.
// Leaves the module as it was if its spans do not fall on whole instructions.
void leaveOut(FunctionProto& module, const Reach& reach, size_t& before, size_t& after) {
    std::vector<uint8_t>& code = module.code;
    before += code.size();
    std::vector<char> starts(code.size() + 1, 0);
    for (size_t at = 0; at < code.size(); at += instructionSize(code, at)) starts[at] = 1;
    starts[code.size()] = 1;
    std::vector<char> gone(code.size(), 0);
    bool any = false;
    for (const FunctionProto::Rights& r : module.rights)
        if (r.end > code.size() || !starts[r.start] || !starts[r.end]) {
            after += code.size();
            return;
        }
    for (const DeclarationSpan& span : module.spans) {
        if (!starts[span.start] || !starts[span.end]) {
            after += code.size();
            return;
        }
        if (reach.reached(module, span)) continue;
        for (uint32_t at = span.start; at < span.end; at++) gone[at] = 1;
        any = true;
    }
    if (!any) {
        after += code.size();
        return;
    }

    // Where each instruction kept goes, and which of the module's own functions are still made.
    std::vector<int64_t> moved(code.size() + 1, -1);
    std::vector<char> made(module.protos.size(), 0);
    size_t length = 0;
    for (size_t at = 0; at < code.size(); at += instructionSize(code, at)) {
        moved[at] = static_cast<int64_t>(length);  // for one left out: where what follows goes
        if (gone[at]) continue;
        length += instructionSize(code, at);
        const Op op = static_cast<Op>(code[at]);
        if (makesFunction(op)) {
            const size_t index = static_cast<size_t>(code[at + 1] | (code[at + 2] << 8));
            if (index < made.size()) made[index] = 1;
        }
    }
    moved[code.size()] = static_cast<int64_t>(length);
    std::vector<int> renumbered(module.protos.size(), -1);
    std::vector<FunctionProto*> protos;
    for (size_t i = 0; i < module.protos.size(); i++) {
        if (!made[i]) continue;
        renumbered[i] = static_cast<int>(protos.size());
        protos.push_back(module.protos[i]);
    }

    std::vector<uint8_t> kept;
    std::vector<int> lines;
    kept.reserve(length);
    lines.reserve(length);
    for (size_t at = 0; at < code.size();) {
        const size_t size = instructionSize(code, at);
        if (gone[at]) {
            at += size;
            continue;
        }
        const size_t to = kept.size();
        kept.insert(kept.end(), code.begin() + static_cast<std::ptrdiff_t>(at), code.begin() + static_cast<std::ptrdiff_t>(at + size));
        lines.insert(lines.end(), module.lines.begin() + static_cast<std::ptrdiff_t>(at),
                     module.lines.begin() + static_cast<std::ptrdiff_t>(at + size));
        const Op op = static_cast<Op>(code[at]);
        const int target = targetOperand(op);
        if (target != 0) {
            const size_t place = to + static_cast<size_t>(target);
            const int32_t old = static_cast<int32_t>(static_cast<uint32_t>(kept[place]) | (static_cast<uint32_t>(kept[place + 1]) << 8) |
                                                     (static_cast<uint32_t>(kept[place + 2]) << 16) | (static_cast<uint32_t>(kept[place + 3]) << 24));
            if (old < 0 || static_cast<size_t>(old) > code.size() || moved[static_cast<size_t>(old)] < 0 ||
                (static_cast<size_t>(old) < code.size() && gone[static_cast<size_t>(old)])) {
                after += code.size();  // goes into what would be left out: keep it all
                return;
            }
            const uint32_t now = static_cast<uint32_t>(moved[static_cast<size_t>(old)]);
            for (int i = 0; i < 4; i++) kept[place + static_cast<size_t>(i)] = static_cast<uint8_t>((now >> (8 * i)) & 0xFF);
        }
        if (makesFunction(op)) {
            const size_t index = static_cast<size_t>(kept[to + 1] | (kept[to + 2] << 8));
            const int now = renumbered[index];
            kept[to + 1] = static_cast<uint8_t>(now & 0xFF);
            kept[to + 2] = static_cast<uint8_t>((now >> 8) & 0xFF);
        }
        at += size;
    }

    std::vector<DeclarationSpan> spans;
    for (const DeclarationSpan& span : module.spans)
        if (reach.reached(module, span))
            spans.push_back(DeclarationSpan{static_cast<uint32_t>(moved[span.start]), static_cast<uint32_t>(moved[span.end]), span.key, span.kind});
    std::vector<FunctionProto::Rights> rights;
    for (const FunctionProto::Rights& r : module.rights) {
        const int64_t start = moved[r.start], end = moved[r.end];
        if (start >= 0 && end > start) rights.push_back(FunctionProto::Rights{static_cast<uint32_t>(start), static_cast<uint32_t>(end), r.ids});
    }
    module.rights = std::move(rights);
    code = std::move(kept);
    module.lines = std::move(lines);
    module.protos = std::move(protos);
    module.spans = std::move(spans);
    module.caches.reset();
    module.cacheCount = 0;
    after += code.size();
}

}  // namespace

const std::string* memberNameAt(const FunctionProto& f, size_t at) {
    if (at >= f.code.size() || f.code[at] >= static_cast<uint8_t>(Op::Count_)) return nullptr;
    const uint8_t raw = f.code[at];
    if (!usesMemberName(static_cast<Op>(raw))) return nullptr;
    int a = 0, b = 0;
    decodeOperands(f.code, at, kOps[raw].operand, a, b);
    if (static_cast<size_t>(a) >= f.constants.size() || !f.constants[static_cast<size_t>(a)].isString()) return nullptr;
    return &asString(f.constants[static_cast<size_t>(a)])->utf8();
}

// With outsideStd, only the program's own modules are looked at, not the standard library's.
bool namesGlobal(const Program& program, const std::vector<std::string>& names, bool outsideStd) {
    std::vector<char> wanted(program.globalNames.size(), 0);
    bool any = false;
    for (size_t slot = 0; slot < program.globalNames.size(); slot++)
        for (const std::string& name : names)
            if (program.globalNames[slot] == name) wanted[slot] = any = 1;
    if (!any) return false;
    std::vector<const FunctionProto*> work;
    for (const FunctionProto* m : program.modules)
        if (!outsideStd || (m != nullptr && m->source != "<std>")) work.push_back(m);
    while (!work.empty()) {
        const FunctionProto* f = work.back();
        work.pop_back();
        if (f == nullptr) continue;
        if (f->deferred != nullptr) loadDeferred(const_cast<FunctionProto&>(*f));
        for (const FunctionProto* nested : f->protos) work.push_back(nested);
        for (size_t at = 0; at < f->code.size();) {
            const uint8_t raw = f->code[at];
            if (raw >= static_cast<uint8_t>(Op::Count_)) break;
            int a = 0, b = 0;
            decodeOperands(f->code, at, kOps[raw].operand, a, b);
            const int which = globalOperand(static_cast<Op>(raw), a);
            const int slot = which == 1 ? a : which == 2 ? b : -1;
            if (slot >= 0 && static_cast<size_t>(slot) < wanted.size() && wanted[static_cast<size_t>(slot)]) return true;
            at += 1 + operandSize(kOps[raw].operand);
        }
    }
    return false;
}

LeftOut leaveOutUnreachable(Program& program, const std::vector<std::string>& globalsByName,
                            const std::vector<std::string>& membersByName) {
    Reach reach(program);
    // Every function's notes on reflection, in order, and what its classes are besides; taken
    // out once done with, a built program having no more use for them.
    std::vector<FunctionProto*> every;
    for (FunctionProto* module : program.modules)
        if (module != nullptr) every.push_back(module);
    for (size_t i = 0; i < every.size(); i++) {
        FunctionProto& f = *every[i];
        if (f.deferred != nullptr) loadDeferred(f);
        if (f.reflection != nullptr) {
            std::stable_sort(f.reflection->notes.begin(), f.reflection->notes.end(),
                             [](const FunctionProto::ReflectNote& a, const FunctionProto::ReflectNote& b) { return a.at < b.at; });
            for (const std::vector<uint16_t>& entry : f.reflection->supertypes) reach.relate(entry);
        }
        for (FunctionProto* nested : f.protos)
            if (nested != nullptr) every.push_back(nested);
    }
    for (FunctionProto* module : program.modules) {
        if (module == nullptr) continue;
        for (size_t i = 0; i < module->spans.size(); i++) {
            const DeclarationSpan& span = module->spans[i];
            if (span.kind == DeclarationSpan::Global) {
                reach.spansOf[span.key].emplace_back(module, i);
                // Any other global it defines needs it as well: the classes declared inside a
                // class are made in its span (M7: an enum inside a class nothing used was left
                // out with it, and the code that sets the enum up was not).
                const std::vector<uint8_t>& code = module->code;
                for (size_t at = span.start; at < span.end && at < code.size(); at += instructionSize(code, at)) {
                    const Op op = static_cast<Op>(code[at]);
                    if (op != Op::DefineGlobal && op != Op::DefineTypedGlobal) continue;
                    const int slot = code[at + 1] | (code[at + 2] << 8);
                    if (slot != span.key) reach.spansOf[slot].emplace_back(module, i);
                }
            } else if (span.kind == DeclarationSpan::Reflect || span.kind == DeclarationSpan::ReflectMethods) {
                reach.metaSpans.emplace_back(module, i);
            } else if (span.key < module->constants.size() && module->constants[span.key].isString()) {
                reach.extensionsNamed[Reach::bare(asString(module->constants[span.key])->utf8())].emplace_back(module, i);
            }
        }
    }
    for (size_t slot = 0; slot < program.globalNames.size(); slot++)
        for (const std::string& name : globalsByName)
            if (program.globalNames[slot] == name) reach.global(static_cast<int>(slot));
    // A script's proxies (script.cpp): found by their names when a handle arrives.
    for (size_t slot = 0; slot < program.globalNames.size(); slot++)
        if (program.globalNames[slot].rfind("__ScriptProxy_", 0) == 0) reach.global(static_cast<int>(slot));
    for (const std::string& name : membersByName) reach.name(name);
    for (FunctionProto* module : program.modules)
        if (module != nullptr) reach.function(module);
    reach.run();
    // The descriptions of classes no global holds that nothing asks for: emptied, describing
    // nothing (G43).
    for (const Reach::Unheld& u : reach.unheld) {
        if (u.reached) continue;
        FunctionProto& f = *u.description;
        const int line = f.lines.empty() ? 0 : f.lines.front();
        f.code = {static_cast<uint8_t>(Op::Null), static_cast<uint8_t>(Op::Return)};
        f.lines.assign(2, line);
        f.constants.clear();
        f.protos.clear();
        f.rights.clear();
        f.caches.reset();
        f.cacheCount = 0;
    }

    LeftOut result;
    for (FunctionProto* module : program.modules) {
        if (module == nullptr) continue;
        for (const DeclarationSpan& span : module->spans)
            if (!reach.reached(*module, span)) result.declarations++;
        leaveOut(*module, reach, result.codeBefore, result.codeAfter);
    }
    for (FunctionProto* f : every) {
        f->reflection.reset();
    }
    for (FunctionProto* module : program.modules)
        if (module != nullptr) verifyModule(*module, program);
    return result;
}

}  // namespace ww
