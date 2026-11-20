#include "vm.h"
#include "native/native.h"
#include "native/layout.h"

#include "builtins.h"  // addOverflows and the like (D27)
#include "profile.h"  // ww --profile

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <new>
#include <stdexcept>

namespace ww {

namespace {

// Counts a call from a built-in function back into the interpreter, and puts back the
// record of where that built-in's own arguments are when the call returns.
struct NativeCallGuard {
    int& depth;
    Value*& base;
    Value* const saved;
    NativeCallGuard(int& d, Value*& b) : depth(d), base(b), saved(b) { depth++; }
    ~NativeCallGuard() {
        depth--;
        base = saved;
    }
};

}  // namespace

VM::VM(bool worker)
    : output(&std::cout), errorOutput(&std::cerr), input(&std::cin), rng(std::random_device{}()), mainThread_(kStackSize, kMaxFrames, worker), isWorker_(worker) {
    // The thread that makes the VM is its main thread, and holds the lock from the start.
    mainThread_.name = "main";
    mainThread_.id = nextThreadId_++;
    threads_.push_back(&mainThread_);
    loadContext(&mainThread_.base);

    if (const char* stress = std::getenv("WW_THREAD_STRESS"); stress != nullptr && *stress != '\0' && *stress != '0')
        threadStress_ = true;

    if (const char* stress = std::getenv("WW_GC_STRESS"); stress != nullptr && *stress != '\0' && *stress != '0') {
        gcStress_ = true;
        nextGC_ = 0;
    }

    // Room for the names a program of some size holds, so that the table is not made again as it grows.
    interned_.reserve(8192);
    names.init = intern("init");
    names.toString = intern("toString");
    names.message = intern("message");
    names.cause = intern("cause");
    names.length = intern("length");
    names.size = intern("size");
    names.indices = intern("indices");
    names.lastIndex = intern("lastIndex");
    names.keys = intern("keys");
    names.itemType = intern("itemType");
    names.keyType = intern("keyType");
    names.valueType = intern("valueType");
    names.values = intern("values");
    names.key = intern("key");
    names.value = intern("value");
    names.code = intern("code");
    names.character = intern("char");
    names.fromEnd = intern("fromEnd");
    names.iterator = intern("iterator");
    names.first = intern("first");
    names.last = intern("last");
    names.absoluteValue = intern("absoluteValue");
    names.sign = intern("sign");
    names.start = intern("start");
    names.end = intern("end");
    names.step = intern("step");
    names.name = intern("name");
    names.ordinal = intern("ordinal");
    names.compareTo = intern("compareTo");
    names.hasNext = intern("hasNext");
    names.next = intern("next");
    names.entries = intern("entries");
    names.enumEntries = intern("(entries)");
    names.entryName = intern("(name)");
    names.entryOrdinal = intern("(ordinal)");
    names.opAdd = intern("+");
    names.opSub = intern("-");
    names.opMul = intern("*");
    names.opDiv = intern("/");
    names.opMod = intern("%");
    names.opEq = intern("==");
    names.hashCode = intern("hashCode");
    names.opLt = intern("<");
    names.opLe = intern("<=");
    names.opGt = intern(">");
    names.opGe = intern(">=");
    names.opNeg = intern("unary-");
    names.opIndex = intern("[]");
    names.opIndexSet = intern("[]=");
    names.opShl = intern("<<");
    names.opShr = intern(">>");
    names.opBitAnd = intern("&");
    names.opBitOr = intern("|");
    names.opBitXor = intern("^");
    names.opBitNot = intern("unary~");

    opNames_[static_cast<int>(Op::Add)] = names.opAdd;
    opNames_[static_cast<int>(Op::Sub)] = names.opSub;
    opNames_[static_cast<int>(Op::Mul)] = names.opMul;
    opNames_[static_cast<int>(Op::Div)] = names.opDiv;
    opNames_[static_cast<int>(Op::Mod)] = names.opMod;
    opNames_[static_cast<int>(Op::Shl)] = names.opShl;
    opNames_[static_cast<int>(Op::Shr)] = names.opShr;
    opNames_[static_cast<int>(Op::Ushr)] = intern(">>>");
    opNames_[static_cast<int>(Op::BitAnd)] = names.opBitAnd;
    opNames_[static_cast<int>(Op::BitOr)] = names.opBitOr;
    opNames_[static_cast<int>(Op::BitXor)] = names.opBitXor;
    opNames_[static_cast<int>(Op::Lt)] = names.opLt;
    opNames_[static_cast<int>(Op::Le)] = names.opLe;
    opNames_[static_cast<int>(Op::Gt)] = names.opGt;
    opNames_[static_cast<int>(Op::Ge)] = names.opGe;

    registerBuiltins(*this);
    if (!worker) profile::attach(*this);  // the program's VM, where ww --profile is on
}

VM::~VM() {
    profile::detach(*this);
    dropWaitingBuilds();  // what this VM asked the builder for, which reads its objects
    parallelPool_.reset();  // its cores first: they work on this VM's objects
    dropWaitingBuilds();    // (and the run code words its cores left waiting for it as they ended)
    releaseAllBlocks();
    Obj* obj = objects_;
    while (obj != nullptr) {
        Obj* next = obj->next;
        delete obj;
        obj = next;
    }
}

// ---- Strings ----

// ---- Types when the program runs (M6.6 part 1) ----

ObjTypeInfo* VM::typeOf(uint8_t id, bool nullable, ObjClass* cls, const std::vector<ObjTypeInfo*>& args) {
    if (root_ != nullptr) {
        // A core's VM has no table of its own: a type is the same object for every core.
        std::lock_guard<std::mutex> hold(root_->internMutex_);
        return root_->typeOf(id, nullable, cls, args);
    }
    std::string key;
    key.reserve(10 + 8 * (args.size() + 1));
    key.push_back(static_cast<char>(id));
    key.push_back(nullable ? 1 : 0);
    auto pointer = [&](const void* p) {
        const uintptr_t bits = reinterpret_cast<uintptr_t>(p);
        key.append(reinterpret_cast<const char*>(&bits), sizeof bits);
    };
    pointer(cls);
    for (const ObjTypeInfo* arg : args) pointer(arg);
    auto found = types_.find(key);
    if (found != types_.end()) return found->second;
    ObjTypeInfo* type = alloc<ObjTypeInfo>(id, nullable, cls, args);
    type->immortal = true;
    type->core = 0;  // every core's to read
    types_.emplace(std::move(key), type);
    return type;
}

ObjTypeInfo* VM::typeOfValue(const Value& v) {
    if (ObjClass* cls = classOf(v)) {
        if (v.isObjType(ObjType::Instance))
            if (const ObjTypeInfo* own = asInstance(v)->type()) return const_cast<ObjTypeInfo*>(own);
        if (cls->isValue)
            if (const ObjTypeInfo* kept = recordedType(v)) return const_cast<ObjTypeInfo*>(kept);  // G4
        return typeOf(kTypeInstance, false, cls, {});
    }
    auto plain = [&](uint8_t id) { return typeOf(id, false, nullptr, {}); };
    switch (v.kind) {
        case ValueKind::Null: return plain(kTypeNull);  // null#, as Python's NoneType (D48)
        case ValueKind::Bool: return plain(kTypeBool);
        case ValueKind::Int: return plain(kTypeInt);
        case ValueKind::Char: return plain(kTypeChar);
        case ValueKind::Byte: return plain(kTypeByte);
        case ValueKind::FromEnd: return plain(kTypeIndex);
        case ValueKind::Float: return plain(kTypeFloat);
        case ValueKind::Obj: break;
        default: return plain(0);
    }
    ObjTypeInfo* const any = typeOf(0, true, nullptr, {});
    switch (v.as.obj->type) {
        case ObjType::String: return plain(kTypeString);
        case ObjType::List: return asList(v)->type != nullptr ? asList(v)->type : typeOf(kTypeList, false, nullptr, {any});
        case ObjType::Map: return asMap(v)->type != nullptr ? asMap(v)->type : typeOf(kTypeMap, false, nullptr, {any, any});
        case ObjType::Set:
            if (asSet(v)->type != nullptr) return asSet(v)->type;
            return typeOf(asSet(v)->values ? kTypeValues : kTypeSet, false, nullptr, {any});
        case ObjType::Range: return plain(kTypeRange);
        case ObjType::Bytes: return plain(kTypeBytes);
        case ObjType::Closure:
        case ObjType::Native:
        case ObjType::BoundMethod:
        case ObjType::BoundNative: return plain(kTypeFunction);
        case ObjType::TypeInfo: return plain(kTypeType);
        case ObjType::Class: return typeOf(kTypeInstance, false, static_cast<ObjClass*>(v.as.obj), {});
        default: return plain(0);
    }
}

Value VM::classDescription(ObjClass* cls, const ObjTypeInfo* own, int which) {
    VM& root = root_ != nullptr ? *root_ : *this;  // what every core has made, kept once
    ObjClosure* meta = nullptr;
    {
        std::lock_guard<std::mutex> hold(root.internMutex_);
        auto found = cls->described[which].find(own);
        if (found != cls->described[which].end()) return found->second;
        if (cls->metaProto[which] == nullptr) return Value::null();
        meta = cls->meta[which];
    }
    // Made outside the lock, which making a type takes, as is the function; two cores may make
    // them at once, and the first kept is kept.
    if (meta == nullptr) {
        // Read now, where the program's image left it to be (G47). Asked under the lock it is
        // read under, always: another core may be part way through reading it.
        loadDeferred(const_cast<FunctionProto&>(*cls->metaProto[which]), this);
        meta = alloc<ObjClosure>(cls->metaProto[which], 0);
        meta->core = 0;  // every core's to call: it holds nothing
        std::lock_guard<std::mutex> hold(root.internMutex_);
        if (cls->meta[which] == nullptr) cls->meta[which] = meta;
        meta = cls->meta[which];
    }
    // Kept for every core: what a core made for it is shared.
    const Value made = makeShared([&] { return callWithTypes(Value::object(meta), own != nullptr ? own->args : std::vector<ObjTypeInfo*>{}, 0, nullptr); });
    std::lock_guard<std::mutex> hold(root.internMutex_);
    return cls->described[which].emplace(own, made).first->second;
}

// A property's getter and setter run, as `obj.name` and `obj.name = value` run them.
// Op::BundleValues (D143): bundle name1 .. nameN -> name1 value1 .. nameN valueN, each value
// the bundle's for that name. Kept out of the instruction loop.
//
// A bundle's names are made from the program's constants, as the names asked are, and every
// constant string is one object (VM::intern); and they are asked in the order the bundle keeps
// them, most often. So each is looked for first where the one before it was found, by its
// address, and only then looked up (G58). The pairs are written over the names from the last
// down: the pair for name k starts at slot 2k - 1 (the bundle's slot for the first), never
// below the names not yet read.
void VM::bundleValues(int count) {
    Value* asked = sp_ - count;
    const Value bundle = asked[-1];
    if (!bundle.isObjType(ObjType::Map)) throw runtimeError(Thrown::ClassCastException, "expected a bundle of named arguments");
    const ValueMap& map = asMap(bundle)->map;
    const std::vector<ValueMap::Entry>& entries = map.entries();
    Value* const out = asked - 1;
    // Read from the last name down, so the first looked for is the bundle's last entry, and each
    // after it the entry before the one found.
    size_t at = entries.size() - 1;  // past every entry (wrapped) where there are none
    for (int k = count - 1; k >= 0; k--) {
        const Value name = asked[k];
        size_t index = at;
        if (!(at < entries.size() && entries[at].live && name.isObj() && entries[at].key.isObj() && entries[at].key.as.obj == name.as.obj)) {
            const Value* found = map.find(name);
            index = found != nullptr ? static_cast<size_t>(reinterpret_cast<const ValueMap::Entry*>(
                                                               reinterpret_cast<const char*>(found) - offsetof(ValueMap::Entry, value)) -
                                                           entries.data())
                                     : entries.size();
        }
        out[2 * k] = name;
        out[2 * k + 1] = index < entries.size() ? entries[index].value : Value::null();
        at = index - 1;
    }
    sp_ = out + 2 * count;
}

// The errors of failed jobs nothing asked about (8.7a): those whose jobs were collected, and as
// the program ends every one left. Each is handed to __unaskedFailed; an error that throws is
// reported as one nobody caught.
void VM::deliverUnasked(bool atEnd) {
    std::vector<Value> pending = std::move(freedFailures);
    freedFailures.clear();
    if (atEnd) {
        for (const auto& [job, error] : unaskedFailures) pending.push_back(error);
        unaskedFailures.clear();
    }
    if (pending.empty() || unaskedDeliver.isNull()) return;
    TempVector keep(*this, pending);
    for (Value error : pending) {
        try {
            call(unaskedDeliver, 1, &error);
        } catch (WwThrow& thrown) {
            uncaughtReported = true;
            if (onUnaskedError) onUnaskedError(*this, thrown);
        }
        if (error.isObj()) failureTraces.erase(error.as.obj);  // a handler took it, or it is reported
    }
}

// A field an extend adds (D157), read: the object's value, or its starting value, made for
// it now and kept. The table is the root's, which every core of a parallel section reads and
// only the root collects.
Value VM::extraField(ObjInstance* instance, ObjString* name, Value init) {
    VM& home = root_ != nullptr ? *root_ : *this;
    {
        std::lock_guard<std::mutex> hold(home.extrasLock_);
        if (auto found = home.extras_.find(instance); found != home.extras_.end())
            if (const Value* value = found->second.find(name)) return *value;
    }
    TempRoot keep(*this, Value::object(instance));
    const Value made = own(call(init, 0, nullptr));
    std::lock_guard<std::mutex> hold(home.extrasLock_);
    Table& record = home.extras_[instance];
    if (const Value* value = record.find(name)) return *value;  // another core made it meanwhile
    record.set(name, made);
    return made;
}

void VM::setExtraField(ObjInstance* instance, ObjString* name, Value value) {
    VM& home = root_ != nullptr ? *root_ : *this;
    std::lock_guard<std::mutex> hold(home.extrasLock_);
    home.extras_[instance].set(name, value);
}

// Op::MergeBundle: a + b of two bundles, b's value where both have a name. Every bundle is
// made here, params(...) as an empty one and its names, so each is marked as one (8.9e).
void VM::mergeBundles() {
    const Value a = sp_[-2], b = sp_[-1];
    if (!a.isObjType(ObjType::Map) || !b.isObjType(ObjType::Map)) throw runtimeError(Thrown::ClassCastException, "expected bundles of named arguments");
    ObjMap* made = alloc<ObjMap>();
    made->bundle = true;
    made->map = asMap(a)->map;
    for (const ValueMap::Entry& entry : asMap(b)->map.entries())
        if (entry.live) made->map.set(entry.key, entry.value);
    sp_[-2] = Value::object(made);
    sp_--;
}

bool VM::widenedByte(Value& v, uint8_t type) {
    if (v.kind != ValueKind::Byte) return false;
    const uint8_t id = type & kTypeMask;
    if (id != kTypeInt && id != kTypeNumber && !isIntStorage(id)) return false;
    v = Value::integer(v.as.i);
    return true;
}

int VM::globalSlotOf(const ObjClass* cls) {
    auto known = classSlots_.find(cls);
    if (known != classSlots_.end() && static_cast<size_t>(known->second) < globals_.size() && globals_[static_cast<size_t>(known->second)].isObjType(ObjType::Class) &&
        asClass(globals_[static_cast<size_t>(known->second)]) == cls)
        return known->second;
    for (size_t i = 0; i < globals_.size(); i++)
        if (globals_[i].isObjType(ObjType::Class) && asClass(globals_[i]) == cls) {
            classSlots_[cls] = static_cast<int>(i);
            return static_cast<int>(i);
        }
    return -1;
}

ObjClass* VM::classNamed(const std::string& name) {
    // The last such: a class of a script's contract comes after the standard library's of the
    // same name (Point), which a message finds by its global first anyway (worker.cpp, takeClass).
    for (size_t i = globals_.size(); i-- > 0;)
        if (globals_[i].isObjType(ObjType::Class) && asClass(globals_[i])->name->utf8() == name) return asClass(globals_[i]);
    return nullptr;
}

Value VM::globalNamed(const std::string& name) {
    for (size_t i = 0; i < globals_.size() && i < program_->globalNames.size(); i++)
        if (program_->globalNames[i] == name) return globals_[i];
    return Value::null();
}

ObjClass* VM::classInGlobal(int slot) const {
    if (slot < 0 || static_cast<size_t>(slot) >= globals_.size() || !globals_[static_cast<size_t>(slot)].isObjType(ObjType::Class)) return nullptr;
    return asClass(globals_[static_cast<size_t>(slot)]);
}

// The member of `name`'s text the C++ means, for memberGet and memberSet, whoever may use it: a
// class's private one where the object has nothing else of that text (o-private-shadow).
ObjString* VM::trustedName(Value obj, ObjString* name) const {
    if ((name->restricted & 16) == 0 || (name->restricted & 8) != 0) return name;
    const ObjClass* cls = classOf(obj);
    if (cls == nullptr || hasMember(obj, name)) return name;
    ObjString* own = privateLike(cls, name);
    return own != nullptr ? own : name;
}

Value VM::memberGet(Value obj, ObjString* name) {
    name = trustedName(obj, name);
    if (const ObjClass* cls = classOf(obj); cls != nullptr && cls->hasAccessors)
        if (const Value* getter = cls->getters.find(name)) return callWithReceiver(obj, asClosure(*getter));
    return getProperty(obj, name);
}

void VM::memberSet(Value obj, ObjString* name, Value value) {
    name = trustedName(obj, name);
    if (const ObjClass* cls = classOf(obj); cls != nullptr && cls->hasAccessors) {
        if (const Value* setter = cls->setters.find(name)) {
            const Value bound = Value::object(alloc<ObjBoundMethod>(obj, asClosure(*setter)));
            TempRoot keep(*this, bound);
            call(bound, 1, &value);
            return;
        }
    }
    setProperty(obj, name, value);
}

// ---- Atomic fields (D92) ----
// An `atomic var` field changed in one step, whoever runs the code: an int's or a float's kept
// as its payload with the processor's atomic operations, anything else under a lock shared by
// address, which reads it (kind 5) and sets it (kind 6) too (8.7f). Any piece of a parallel
// section may change one of an object it may read; what it stores has to be a value every core
// may read.
Value VM::atomicField(Value object, ObjString* name, int kind, const Value* operands) {
    if (!object.isObjType(ObjType::Instance)) throw runtimeError(Thrown::InternalError, "an atomic field belongs to an object");
    guardRead(object);
    ObjInstance* instance = asInstance(object);
    const FieldDecl* d = instance->declOf(name);
    if (d == nullptr) throw runtimeError(Thrown::InternalError, "no atomic field '" + name->utf8() + "'");
    const Value operand = kind == 5 ? Value::null() : operands[kind == 4 ? 1 : 0];
    if (coreId_ != 0 && operand.isObj() && operand.as.obj->frozen == 0 && operand.as.obj->type != ObjType::String)
        throw runtimeError(Thrown::IllegalStateException, "in a parallel section an atomic field is given values: numbers, bools, small values, text or frozen data; not this " +
                                                              typeName(operand) + ", which a core may still change");
    auto change = [&](const Value& old) -> Value {
        if (kind == 4 || kind == 6) return operand;
        if (old.isInt() && operand.isInt()) {
            int64_t r = 0;
            if (kind == 0 ? addOverflows(old.as.i, operand.as.i, r) : kind == 1 ? subOverflows(old.as.i, operand.as.i, r) : false)
                throw overflowError(kind == 0 ? Op::Add : Op::Sub, old.as.i, operand.as.i);
            if (kind == 2) r = std::max(old.as.i, operand.as.i);
            if (kind == 3) r = std::min(old.as.i, operand.as.i);
            return Value::integer(r);
        }
        if (!old.isNumber() || !operand.isNumber())
            throw runtimeError(Thrown::ClassCastException, "'" + name->utf8() + "' holds " + typeName(old) + ", which " + (kind < 2 ? "+= and -=" : "max and min") +
                                                               " do not take with " + typeName(operand));
        const double a = old.toDouble(), b = operand.toDouble();
        return Value::number(kind == 0 ? a + b : kind == 1 ? a - b : kind == 2 ? std::max(a, b) : std::min(a, b));
    };
    if (d->payload == FieldDecl::IntPayload || d->payload == FieldDecl::FloatPayload) {
        std::atomic_ref<int64_t> held(*reinterpret_cast<int64_t*>(instance->payloadData() + d->payloadOffset));
        const ValueKind valueKind = d->payload == FieldDecl::IntPayload ? ValueKind::Int : ValueKind::Float;
        auto toValue = [&](int64_t bits) {
            Value v;
            v.kind = valueKind;
            v.as.i = bits;
            return v;
        };
        auto toBits = [&](const Value& v) {
            if (valueKind == ValueKind::Float && v.isInt()) return Value::number(static_cast<double>(v.as.i)).as.i;
            return v.as.i;
        };
        int64_t old = held.load();
        if (kind == 5) return toValue(old);
        if (kind == 4) {
            const Value expected = operands[0];
            if (!expected.isNumber()) return Value::boolean(false);
            int64_t want = toBits(expected);
            return Value::boolean(held.compare_exchange_strong(want, toBits(operand)));
        }
        for (;;) {
            const Value made = change(toValue(old));
            if (valueKind == ValueKind::Int && !made.isInt())
                throw runtimeError(Thrown::ClassCastException, "'" + name->utf8() + "' holds an int, which a float does not fit");
            if (held.compare_exchange_weak(old, toBits(made))) return toValue(toBits(made));
        }
    }
    if (d->payload != FieldDecl::NoPayload) throw runtimeError(Thrown::UnsupportedOperationException, "an atomic field holds a number, a bool, a small value or frozen data");
    static std::mutex locks[64];
    std::lock_guard<std::mutex> hold(locks[(reinterpret_cast<uintptr_t>(instance) >> 4) & 63]);
    Value& slot = instance->fields()[d->slot];
    if (kind == 5) return slot;
    if (kind == 4) {
        const Value expected = operands[0];
        const bool same = slot.kind == expected.kind && (slot.isObj() ? slot.as.obj == expected.as.obj : slot.isFloat() ? slot.as.i == expected.as.i : slot.as.i == expected.as.i);
        if (!same) return Value::boolean(false);
        slot = operand;
        return Value::boolean(true);
    }
    slot = change(slot);
    return slot;
}

Value VM::callWithTypes(Value callee, const std::vector<ObjTypeInfo*>& typeArgs, int argc, const Value* args) {
    struct Clear {
        const ObjTypeInfo*& pending;
        ~Clear() { pending = nullptr; }  // not taken by a callee that has none
    } clear{pendingTypeArgs_};
    pendingTypeArgs_ = typeArgs.empty() ? nullptr : typeOf(kTypeArguments, false, nullptr, typeArgs);
    return call(callee, argc, args);
}

std::string VM::typeInfoText(const ObjTypeInfo* type) const {
    std::string text;
    if (type->id == kTypeInstance) {
        text = type->cls->name->utf8();
    } else if (type->id == 0) {
        text = "any";
    } else {
        text = declaredTypeText(type->id, nullptr);
    }
    if (!type->args.empty()) {
        text += "<";
        for (size_t i = 0; i < type->args.size(); i++) text += (i ? ", " : "") + typeInfoText(type->args[i]);
        text += ">";
    }
    if (type->nullable) text += "?";
    return text;
}

ObjTypeInfo* VM::typeFromShape(const std::string& shape, size_t& at, const Value*& operand) {
    bool nullable = false;
    if (at < shape.size() && shape[at] == '?') {
        nullable = true;
        at++;
    }
    if (at >= shape.size()) throw runtimeError(Thrown::InternalError, "invalid type shape");
    const char c = shape[at++];
    switch (c) {
        case 'a': return typeOf(0, nullable, nullptr, {});
        case 'i': return typeOf(kTypeInt, nullable, nullptr, {});
        case 'f': return typeOf(kTypeFloat, nullable, nullptr, {});
        case 'n': return typeOf(kTypeNumber, nullable, nullptr, {});
        case 's': return typeOf(kTypeString, nullable, nullptr, {});
        case 'b': return typeOf(kTypeBool, nullable, nullptr, {});
        case 'y': return typeOf(kTypeBytes, nullable, nullptr, {});
        case 'c': return typeOf(kTypeChar, nullable, nullptr, {});
        case 'u': return typeOf(kTypeByte, nullable, nullptr, {});
        case 'h': return typeOf(kTypeInt16, nullable, nullptr, {});
        case 'd': return typeOf(kTypeInt32, nullable, nullptr, {});
        case 'H': return typeOf(kTypeUint16, nullable, nullptr, {});
        case 'D': return typeOf(kTypeUint32, nullable, nullptr, {});
        case 'g': return typeOf(kTypeFloat32, nullable, nullptr, {});
        case 'x': return typeOf(kTypeIndex, nullable, nullptr, {});
        case 'r': return typeOf(kTypeRange, nullable, nullptr, {});
        case 't': return typeOf(kTypeType, nullable, nullptr, {});
        case 'F': return typeOf(kTypeFunction, nullable, nullptr, {});
        case 'L': {
            ObjTypeInfo* item = typeFromShape(shape, at, operand);
            return typeOf(kTypeList, nullable, nullptr, {item});
        }
        case 'S': {
            ObjTypeInfo* item = typeFromShape(shape, at, operand);
            return typeOf(kTypeSet, nullable, nullptr, {item});
        }
        case 'V': {
            ObjTypeInfo* item = typeFromShape(shape, at, operand);
            return typeOf(kTypeValues, nullable, nullptr, {item});
        }
        case 'M': {
            ObjTypeInfo* key = typeFromShape(shape, at, operand);
            ObjTypeInfo* value = typeFromShape(shape, at, operand);
            return typeOf(kTypeMap, nullable, nullptr, {key, value});
        }
        case 'Z': {
            // FrozenList<T> and the others (D26): the list's type, frozen.
            const ObjTypeInfo* plain = typeFromShape(shape, at, operand);
            if (frozenTypeId(plain->id) == 0) throw runtimeError(Thrown::InternalError, "invalid type shape");
            return typeOf(frozenTypeId(plain->id), nullable, nullptr, plain->args);
        }
        case 'C': {
            const int count = shape[at++] - '0';
            const Value cls = *operand++;
            if (!cls.isObjType(ObjType::Class)) throw runtimeError(Thrown::ClassCastException, "a type must be a class or a built-in type, not a value of type " + typeName(cls));
            std::vector<ObjTypeInfo*> args;
            for (int i = 0; i < count; i++) args.push_back(typeFromShape(shape, at, operand));
            return typeOf(kTypeInstance, nullable, asClass(cls), args);
        }
        case 'A': {
            const int count = shape[at++] - '0';
            std::vector<ObjTypeInfo*> args;
            for (int i = 0; i < count; i++) args.push_back(typeFromShape(shape, at, operand));
            return typeOf(kTypeArguments, false, nullptr, args);
        }
        case 'P': {
            const Value given = *operand++;
            if (!given.isObjType(ObjType::TypeInfo)) throw runtimeError(Thrown::InternalError, "invalid type");
            ObjTypeInfo* type = static_cast<ObjTypeInfo*>(given.as.obj);
            if (!nullable || type->nullable) return type;  // as it is: no need to look it up
            return typeOf(type->id, true, type->cls, type->args);
        }
        default:
            throw runtimeError(Thrown::InternalError, "invalid type shape");
    }
}

std::string VM::valueTypeText(const Value& v) const {
    if (v.isObjType(ObjType::List) && asList(v)->type != nullptr) return typeInfoText(asList(v)->type);
    if (v.isObjType(ObjType::Map) && asMap(v)->type != nullptr) return typeInfoText(asMap(v)->type);
    if (v.isObjType(ObjType::Instance) && asInstance(v)->type() != nullptr) return typeInfoText(asInstance(v)->type());
    // A value of a generic value type, with the type arguments it keeps in a box or itself (G4).
    if (v.isObjType(ObjType::Value) && asValue(v)->type != nullptr) return typeInfoText(asValue(v)->type);
    if (v.isSmall() && smallType(v) != nullptr) return typeInfoText(smallType(v));
    return typeName(v);
}

const std::vector<ObjTypeInfo*>& VM::supertypesOf(ObjClass* cls, const ObjTypeInfo* own) {
    VM& root = root_ != nullptr ? *root_ : *this;  // what every core has made, kept once
    {
        std::lock_guard<std::mutex> hold(root.internMutex_);
        auto found = cls->supertypesMade.find(own);
        if (found != cls->supertypesMade.end()) return found->second;
    }
    // Made outside the lock, which making a type takes. Two cores may make them at once, and
    // make the same types, which are interned: the first kept is kept.
    std::vector<ObjTypeInfo*> made;
    const ObjTypeInfo* args = own != nullptr ? typeOf(kTypeArguments, false, nullptr, own->args) : nullptr;
    for (ObjClosure* maker : cls->supertypeMakers) {
        pendingTypeArgs_ = args;
        const Value type = call(Value::object(maker), 0, nullptr);
        if (type.isObjType(ObjType::TypeInfo) && static_cast<ObjTypeInfo*>(type.as.obj)->id == kTypeInstance)
            made.push_back(static_cast<ObjTypeInfo*>(type.as.obj));
    }
    std::lock_guard<std::mutex> hold(root.internMutex_);
    return cls->supertypesMade.emplace(own, std::move(made)).first->second;
}

const std::vector<ObjTypeInfo*>& VM::fieldTypesOf(ObjClass* cls, const ObjTypeInfo* own) {
    static const std::vector<ObjTypeInfo*> none;
    if (cls->patternsState.load(std::memory_order_acquire) != 2) return none;
    VM& root = root_ != nullptr ? *root_ : *this;  // what every core has made, kept once
    {
        std::lock_guard<std::mutex> hold(root.internMutex_);
        auto found = cls->fieldTypesMade.find(own);
        if (found != cls->fieldTypesMade.end()) return found->second;
    }
    // Made outside the lock, which making a type takes; two cores may make the same types, which
    // are interned: the first kept is kept.
    std::vector<ObjTypeInfo*> made(cls->fieldPatterns.size(), nullptr);
    for (size_t i = 0; i < made.size(); i++)
        if (cls->fieldPatterns[i] != nullptr) made[i] = substituted(cls->fieldPatterns[i], own);
    std::lock_guard<std::mutex> hold(root.internMutex_);
    return cls->fieldTypesMade.emplace(own, std::move(made)).first->second;
}

bool VM::keepsItemTypes(const ObjTypeInfo* declared) {
    if (declared == nullptr || declared->id != kTypeInstance || declared->cls == nullptr || declared->cls->phantomParams == 0) return false;
    if (declared->args.empty()) return true;  // written without its type arguments: any
    for (size_t i = 0; i < declared->args.size() && i < 7; i++) {
        if (((declared->cls->phantomParams >> i) & 1) == 0) continue;
        const ObjTypeInfo* arg = declared->args[i];
        if (arg->id == 0 || arg->id == kTypeNumber || (arg->id == kTypeInstance && arg->cls != nullptr && (arg->cls->isInterface || arg->cls->isAbstract)))
            return true;
    }
    return false;
}

ObjTypeInfo* VM::substituted(ObjTypeInfo* pattern, const ObjTypeInfo* own) {
    if (pattern->id >= kTypeHole && pattern->id < kTypeHole + kTypeHoles) {
        ObjTypeInfo* given = typeArgument(own, pattern->id - kTypeHole);
        if (!pattern->nullable || given->nullable) return given;
        return typeOf(given->id, true, given->cls, given->args);
    }
    if (pattern->args.empty()) return pattern;
    std::vector<ObjTypeInfo*> args;
    bool changed = false;
    for (ObjTypeInfo* arg : pattern->args) {
        args.push_back(substituted(arg, own));
        changed = changed || args.back() != arg;
    }
    return changed ? typeOf(pattern->id, pattern->nullable, pattern->cls, args) : pattern;
}

// The patterns of a class's fields' types (ObjClass::fieldTypesMaker): its maker run once, given
// stand-ins for the class's type arguments, at a point where the program may run, as the first
// object or value of the class is made. A core that finds another making them goes on without.
void VM::makeFieldPatterns(ObjClass* cls) {
    uint8_t notMade = 0;
    if (!cls->patternsState.compare_exchange_strong(notMade, 1, std::memory_order_acq_rel)) return;
    std::vector<ObjTypeInfo*> holes;
    for (uint8_t i = 0; i < kTypeHoles; i++) holes.push_back(typeOf(static_cast<uint8_t>(kTypeHole + i), false, nullptr, {}));
    // What the call about to be made was given is put back for it.
    const ObjTypeInfo* const savedTypeArgs = pendingTypeArgs_;
    const Value savedDestination = pendingDestination_;
    TempRoot keepDestination(*this, savedDestination);
    pendingDestination_ = Value::null();
    pendingTypeArgs_ = typeOf(kTypeArguments, false, nullptr, holes);
    std::vector<ObjTypeInfo*> made(cls->declared.size(), nullptr);
    const Value given = call(Value::object(cls->fieldTypesMaker), 0, nullptr);
    pendingTypeArgs_ = savedTypeArgs;
    pendingDestination_ = savedDestination;
    if (given.isObjType(ObjType::List) && asList(given)->pack == nullptr) {
        const std::vector<Value>& pairs = asList(given)->values();
        for (size_t i = 0; i + 1 < pairs.size(); i += 2) {
            const Value name = pairs[i], type = pairs[i + 1];
            if (!name.isString() || !type.isObjType(ObjType::TypeInfo)) continue;
            if (const Value* at = cls->declaredIndex.find(asString(name))) made[static_cast<size_t>(at->as.i)] = static_cast<ObjTypeInfo*>(type.as.obj);
        }
    }
    cls->fieldPatterns = std::move(made);
    cls->patternsState.store(2, std::memory_order_release);
}

uint16_t VM::valueTypeNumber(const ObjTypeInfo* type) {
    if (type == nullptr) return 0;
    VM& root = root_ != nullptr ? *root_ : *this;
    std::lock_guard<std::mutex> hold(root.internMutex_);
    if (root.valueTypeTable_ == nullptr) root.valueTypeTable_.reset(new std::atomic<const ObjTypeInfo*>[65536]());
    auto found = root.valueTypeNumbers_.find(type);
    if (found != root.valueTypeNumbers_.end()) return found->second;
    if (root.nextValueType_ == 0) return 0;  // every number given out
    const uint16_t number = root.nextValueType_++;
    root.valueTypeTable_[number].store(type, std::memory_order_release);
    root.valueTypeNumbers_.emplace(type, number);
    const_cast<ObjTypeInfo*>(type)->valueNumber.store(number, std::memory_order_release);  // a type is one VM's, and the number its
    return number;
}

const ObjTypeInfo* VM::recordedType(const Value& v) {
    switch (v.kind) {
        case ValueKind::Small: return smallType(v);
        case ValueKind::Ref: return refType(v);
        case ValueKind::Obj: return v.as.obj->type == ObjType::Value ? asValue(v)->type : nullptr;
        default: return nullptr;
    }
}

const ObjTypeInfo* VM::refType(const Value& r) {
    const ObjTypeInfo* type = nullptr;
    ObjClass* cls = nullptr;
    uint32_t base = 0;
    switch (r.as.obj->type) {
        case ObjType::List: {
            // An item of a list: the type the list was made to hold, its own where it is the item's
            // class, wider or not (a Pair<int, int> in a List<Pair<Number, Number>>).
            const ObjList* list = static_cast<const ObjList*>(r.as.obj);
            if (list->pack == nullptr) return nullptr;
            cls = list->pack->itemClass;
            if (list->type != nullptr && list->type->args.size() == 1 && list->type->args[0]->cls == cls) type = list->type->args[0];
            if (list->pack->typeSlot != 0 && r.refIndex < list->pack->count)
                if (const ObjTypeInfo* kept = readTypeSlot(list->pack->block.data() + static_cast<size_t>(r.refIndex) * list->pack->width + list->pack->typeSlot))
                    type = kept;
            break;
        }
        case ObjType::Instance: {
            // A value kept inside an object: its field's declared type, made for the object's own.
            ObjInstance* instance = static_cast<ObjInstance*>(r.as.obj);
            const ObjClass::InlineField* field = instance->cls->inlineFieldAt(r.refOffset);
            cls = field->cls;
            base = field->offset;
            type = fieldTypeOf(instance->cls, instance->type(), field->declared);
            if (field->typeSlot != 0)
                if (const ObjTypeInfo* kept = readTypeSlot(instance->inlineData() + field->offset + field->typeSlot)) type = kept;
            break;
        }
        case ObjType::Places: {
            const ObjPlace* place = static_cast<const ObjPlace*>(r.as.obj);
            cls = place->cls;
            type = place->type;
            break;
        }
        default: {
            const ObjValue* box = static_cast<const ObjValue*>(r.as.obj);
            cls = box->cls;
            type = box->type;
            break;
        }
    }
    // A value held in it, `refDepth` deep: its field's declared type, made for the one holding it.
    for (int d = 0; d < r.refDepth && cls != nullptr; d++) {
        const ValueLayout* layout = cls->layout.load(std::memory_order_acquire);
        ObjClass* inner = nullptr;
        for (size_t i = 0; i < layout->fields.size(); i++) {
            const ValueField& field = layout->fields[i];
            if (field.kind != ValueField::Nested) continue;
            const uint32_t start = base + field.offset;
            if (r.refOffset >= start && r.refOffset < start + field.cls->layout.load(std::memory_order_acquire)->size) {
                type = fieldTypeOf(cls, type, i);
                if (field.typeSlot != 0) {
                    // The type this field keeps beside its value, where its bytes are.
                    if (const uint8_t* bytes = refBytes(Value::ref(r.as.obj, r.refIndex, static_cast<uint16_t>(start), 0)))
                        if (const ObjTypeInfo* kept = readTypeSlot(bytes + field.typeSlot)) type = kept;
                }
                inner = field.cls;
                base = start;
                break;
            }
        }
        cls = inner;
    }
    return type != nullptr && type->cls == cls ? type : nullptr;
}

const ObjTypeInfo* VM::typeAs(ObjClass* cls, const ObjTypeInfo* own, const ObjClass* target) {
    while (cls != target) {
        if (cls == nullptr) return nullptr;
        // A supertype written with type arguments that leads to the target, or else the
        // superclass, written without them.
        const ObjTypeInfo* next = nullptr;
        if (!cls->supertypeMakers.empty())
            for (ObjTypeInfo* made : supertypesOf(cls, own))
                if (made->cls->isSubclassOf(target)) {
                    next = made;
                    break;
                }
        if (next != nullptr) {
            cls = next->cls;
            own = next;
            continue;
        }
        if (cls->super == nullptr || !cls->super->isSubclassOf(target)) return nullptr;  // by an interface written bare
        cls = cls->super;
        own = nullptr;
    }
    return own;
}

bool VM::isSubtype(const ObjTypeInfo* a, const ObjTypeInfo* b) {
    if (a == b) return true;
    if (a->nullable && !b->nullable) return false;
    if (b->id == 0) return true;  // any
    if (a->id == 0) return false;
    if (b->id == kTypeNumber && (a->id == kTypeInt || a->id == kTypeFloat || a->id == kTypeNumber)) return true;
    // A built-in type an extend fits to an interface (G30): List<int> where a Summable goes.
    if (b->id == kTypeInstance && a->id != kTypeInstance && hasBuiltinFits_) return builtinTypeFitsClass(a, b->cls);
    // A FrozenList is a List (D26).
    if (a->id != b->id && unfrozenTypeId(a->id) != b->id) return false;
    if (a->id == kTypeInstance && !a->cls->isSubclassOf(b->cls)) return false;
    if (a->id == kTypeInstance && !b->args.empty() && a->cls != b->cls) {
        // A subclass's type, seen as the class's: `Pair<int>` as the `Base<List<int>>` it extends.
        a = typeAs(a->cls, a, b->cls);
        if (a == nullptr) return true;  // made without type arguments: takes any
    }
    if (a->id == kTypeInstance && b->args.empty()) return true;
    if (a->args.size() != b->args.size()) return b->args.empty();
    for (size_t i = 0; i < a->args.size(); i++)
        if (!isSubtype(a->args[i], b->args[i])) return false;
    return true;
}

bool VM::fitsType(const Value& v, const ObjTypeInfo* type) {
    if (v.isNull()) return type->nullable || type->id == 0 || type->id == kTypeNull;
    if (type->id == 0) return true;
    if (type->id == kTypeNull) return false;  // null alone (D48)
    if (type->id == kTypeInstance) {
        const ObjClass* const cls = classOf(v);
        if (cls == nullptr) return builtinFitsClass(v, type->cls);  // a built-in value an extend fits (G30)
        if (!cls->isSubclassOf(type->cls)) return false;
        if (type->args.empty()) return true;
        if (cls->isValue) return cls != type->cls || valueFits(v, const_cast<ObjClass*>(cls), type);
        if (!v.isObjType(ObjType::Instance)) return true;
        // An object made without its type arguments takes any; one of a subclass is seen as
        // the class it extends.
        ObjInstance* instance = asInstance(v);
        const ObjTypeInfo* made = typeAs(instance->cls, instance->type(), type->cls);
        if (made == nullptr || made->args.size() != type->args.size()) return true;
        for (size_t i = 0; i < made->args.size(); i++)
            if (!isSubtype(made->args[i], type->args[i])) return false;
        return true;
    }
    if (!matchesType(v, type->id, nullptr)) return false;
    if (type->args.empty()) return true;
    // A list or map made before its type was kept, or by code that did not say, takes anything.
    const ObjTypeInfo* made = v.isObjType(ObjType::List) ? asList(v)->type : v.isObjType(ObjType::Map) ? asMap(v)->type
                              : v.isObjType(ObjType::Set)                         ? asSet(v)->type
                                                                                  : nullptr;
    if (made == nullptr || made->args.size() != type->args.size()) return true;
    for (size_t i = 0; i < made->args.size(); i++)
        if (!isSubtype(made->args[i], type->args[i])) return false;
    return true;
}

bool VM::valueFits(const Value& v, ObjClass* cls, const ObjTypeInfo* type) {
    const ObjTypeInfo* kept = recordedType(v);
    if (kept != nullptr && kept->args.size() != type->args.size()) kept = nullptr;
    bool wider = false;
    for (size_t i = 0; i < type->args.size(); i++) {
        const ObjTypeInfo* asked = type->args[i];
        const ObjTypeInfo* has = kept != nullptr ? kept->args[i] : nullptr;
        if (has != nullptr && isSubtype(has, asked)) continue;
        // Wider than asked, or not known: what the value holds says. Neither within the other: no
        // value is of both (an Id<User> is never an Id<Order>).
        if (has != nullptr && !isSubtype(asked, has)) return false;
        wider = true;
    }
    if (!wider || cls->fieldTypesMaker == nullptr) return true;
    // The fields declared with type arguments, each against its type made for those asked.
    const Value held = v;  // a small value's bytes are in it
    const uint8_t* bytes = held.kind == ValueKind::Small ? smallBytes(held)
                           : held.kind == ValueKind::Ref ? refBytes(held)
                           : held.isObjType(ObjType::Value) ? asValue(held)->data()
                                                            : nullptr;
    if (bytes == nullptr) return false;
    const ValueLayout& layout = layoutOf(cls);
    const std::vector<ObjTypeInfo*>& fields = fieldTypesOf(cls, typeOf(kTypeInstance, false, cls, type->args));
    TempRoot keep(*this, v);
    for (size_t i = 0; i < fields.size() && i < layout.fields.size(); i++) {
        if (fields[i] == nullptr) continue;
        const Value field = readValueField(bytes + layout.fields[i].offset, layout.fields[i], fields[i]);
        TempRoot keepField(*this, field);
        if (!fitsType(field, fields[i])) return false;
    }
    return true;
}

Value VM::retyped(const Value& v, const ObjTypeInfo* type) {
    if (v.kind == ValueKind::Small) {
        Value made = v;
        made.refOffset = valueTypeNumber(type);
        return made;
    }
    const Value boxed = v.kind == ValueKind::Ref ? ownRef(v) : v;
    if (!boxed.isObjType(ObjType::Value)) return v;
    ObjValue* from = asValue(boxed);
    if (from->type == type) return boxed;
    TempRoot keep(*this, boxed);
    ObjValue* box = newValue(from->cls);
    std::memcpy(box->data(), from->data(), layoutOf(from->cls).size);
    box->type = type;
    return Value::object(box);
}

ObjString* VM::intern(std::string_view s) {
    if (root_ != nullptr) {
        // A core's VM has no table of its own: a name is the same object for every core.
        std::lock_guard<std::mutex> hold(root_->internMutex_);
        return root_->intern(s);
    }
    auto it = interned_.find(s);
    if (it != interned_.end()) return it->second;

    ObjString* str = alloc<ObjString>(std::string(s));
    str->immortal = true;
    interned_.emplace(std::string_view(str->utf8()), str);
    return str;
}

namespace {
// Each private name's owner, for every VM (a VM's private names are freed with it, and their
// places taken again: hence assigned, not added, and asked only of a name marked private).
std::mutex privateOwnersMutex;
std::unordered_map<const ObjString*, std::string> privateOwners;
}  // namespace

ObjString* VM::privateName(std::string_view owner, std::string_view text) {
    VM& root = root_ != nullptr ? *root_ : *this;
    std::lock_guard<std::mutex> hold(root.internMutex_);
    std::string key;
    key.reserve(owner.size() + 1 + text.size());
    key.append(owner).append(1, '\t').append(text);
    auto it = root.privateNames_.find(key);
    if (it != root.privateNames_.end()) return it->second;
    ObjString* str = root.alloc<ObjString>(std::string(text));
    str->immortal = true;
    str->restricted |= 8;
    // The text, interned, is marked too: a name some class has a private one of (privateLike).
    root.intern(text)->restricted |= 16;
    root.privateNames_.emplace(std::move(key), str);
    std::lock_guard<std::mutex> owners(privateOwnersMutex);
    privateOwners[str] = std::string(owner);
    return str;
}

const std::string* VM::privateOwner(const ObjString* name) {
    if ((name->restricted & 8) == 0) return nullptr;
    std::lock_guard<std::mutex> hold(privateOwnersMutex);
    auto it = privateOwners.find(name);
    return it != privateOwners.end() ? &it->second : nullptr;
}

ObjString* VM::privateLike(const ObjClass* cls, const ObjString* name) const {
    if (cls == nullptr || (name->restricted & 16) == 0) return nullptr;
    // Among its fields, methods and properties, inherited ones with them.
    ObjString* found = nullptr;
    auto scan = [&](const Table& table) {
        for (uint32_t at = 0; found == nullptr && at < table.capacity(); at++) {
            ObjString* key = table.entries()[at].key;
            if (key != nullptr && (key->restricted & 8) != 0 && sameText(key, name)) found = key;
        }
    };
    scan(cls->declaredIndex);
    scan(cls->methods);
    scan(cls->getters);
    scan(cls->setters);
    return found;
}

ObjString* VM::missedName(const ObjClass* cls, ObjString* name, Value target, bool write) {
    if (cls == nullptr) return nullptr;
    if ((name->restricted & 8) != 0) return publicName(name);
    ObjString* const found = privateLike(cls, name);
    if (found == nullptr) return nullptr;
    // What may use it says the code running: of its class, or put in place from it (whose
    // rights are noted by where it is, and are asked of the instruction last run).
    if (frameCount_ > 0) {
        const CallFrame& frame = frames_[frameCount_ - 1];
        const FunctionProto* proto = frame.closure->proto;
        if (accessDenied(target, found, proto, write) &&
            (proto->rights.empty() || frame.ip == nullptr || !rightsAllow(target, found, proto, frame.ip, write)))
            throw accessError(target, found, proto);
    }
    return found;
}

ObjString* VM::newString(std::string s) {
    account(s.size());
    return alloc<ObjString>(std::move(s));
}

ObjString* VM::joinStrings(const ObjString* first, const ObjString* second) {
    account(first->units().size() + second->units().size());
    return alloc<ObjString>(first, second);
}

ObjString* VM::newString(Characters text) {
    account(text.units.size());
    return alloc<ObjString>(std::move(text));
}

// ---- Setup ----

void VM::defineNative(const std::string& name, int minArgs, int maxArgs, NativeFn fn) {
    builtins_[name] = Value::object(alloc<ObjNative>(name, minArgs, maxArgs, fn));
}

void VM::defineMethod(BuiltinType type, const std::string& name, int minArgs, int maxArgs, NativeMethodFn fn) {
    methods_[static_cast<int>(type)][intern(name)] = NativeMethod{name, minArgs, maxArgs, fn, {}};
}

bool VM::builtinArity(const std::string& name, int& minArgs, int& maxArgs) const {
    auto found = builtins_.find(name);
    if (found == builtins_.end() || !found->second.isObjType(ObjType::Native)) return false;
    const ObjNative* native = static_cast<const ObjNative*>(found->second.as.obj);
    minArgs = native->minArgs;
    maxArgs = native->maxArgs;
    return true;
}

ObjNative* VM::builtinStatic(const std::string& cls, const std::string& name) {
    auto found = builtins_.find(cls);
    if (found == builtins_.end() || !found->second.isObjType(ObjType::Class)) return nullptr;
    const Value* member = asClass(found->second)->statics.find(intern(name));
    if (member == nullptr || !member->isObjType(ObjType::Native)) return nullptr;
    return asNative(*member);
}

bool VM::methodArity(BuiltinType type, const std::string& name, int& minArgs, int& maxArgs) {
    const auto& table = methods_[static_cast<int>(type)];
    auto found = table.find(intern(name));
    if (found == table.end()) return false;
    minArgs = found->second.minArgs;
    maxArgs = found->second.maxArgs;
    return true;
}

void VM::nameParams(BuiltinType type, const std::string& method, const std::vector<std::string>& params) {
    auto found = methods_[static_cast<int>(type)].find(intern(method));
    if (found == methods_[static_cast<int>(type)].end()) return;
    if (params.size() > found->second.params.size()) found->second.params = params;
    auto& forms = found->second.forms;
    if (std::find(forms.begin(), forms.end(), params) == forms.end()) forms.push_back(params);
}

void VM::nameFunctionParams(const std::string& function, const std::vector<std::string>& params) {
    auto found = builtins_.find(function);
    if (found == builtins_.end() || !found->second.isObjType(ObjType::Native)) return;
    ObjNative* native = asNative(found->second);
    if (params.size() > native->params.size()) native->params = params;
}

bool VM::builtinTypeNamed(const std::string& name, BuiltinType& type) {
    if (name == "string") type = BuiltinType::String;
    else if (name == "List") type = BuiltinType::List;
    else if (name == "Map") type = BuiltinType::Map;
    else if (name == "Range") type = BuiltinType::Range;
    else if (name == "Bytes") type = BuiltinType::Bytes;
    else if (name == "Number" || name == "int" || name == "float") type = BuiltinType::Number;
    else if (name == "char") type = BuiltinType::Char;
    else if (name == "byte") type = BuiltinType::Byte;
    else if (name == "Index") type = BuiltinType::Index;
    else if (name == "Set") type = BuiltinType::Set;
    else return false;
    return true;
}

std::vector<Value> VM::setAsideOf(Value error) {
    std::vector<Value> out;
    if (!error.isObjType(ObjType::Instance)) return out;
    const Value* kept = asInstance(error)->fieldText(intern("setAside"));
    if (kept != nullptr && kept->isObjType(ObjType::List)) collect(*kept, out);
    return out;
}

void VM::collect(Value iterable, std::vector<Value>& out) {
    guardRead(iterable);
    if (iterable.isObjType(ObjType::List)) {
        ObjList* list = asList(iterable);
        if (!list->packed()) {
            out.insert(out.end(), list->values().begin(), list->values().end());
            return;
        }
        for (size_t i = 0; i < list->size(); i++) out.push_back(listGet(list, i));
        return;
    }
    TempVector held(*this, out);
    TempRoot iterator(*this, makeIterator(iterable));
    ObjIterator* it = asIterator(iterator.get());
    Value item;
    while (iteratorNext(it, item)) out.push_back(item);
}

void VM::load(Program& program) {
    program_ = &program;

    size_t known = globals_.size();
    globals_.resize(program.globalNames.size(), Value::undefined());
    globalTypes_.resize(globals_.size());
    for (size_t i = known; i < globals_.size(); i++) {
        auto it = builtins_.find(program.globalNames[i]);
        if (it != builtins_.end()) globals_[i] = it->second;
    }
    loaded_.resize(program.modules.size(), false);
}

std::string VM::mainFileName() const {
    std::string file;
    if (program_ != nullptr && mainModule_ >= 0 && static_cast<size_t>(mainModule_) < program_->modules.size() &&
        program_->modules[static_cast<size_t>(mainModule_)] != nullptr)
        file = program_->modules[static_cast<size_t>(mainModule_)]->source;
    const std::string stem = std::filesystem::path(file).stem().string();
    return stem.empty() ? std::string("program") : stem;
}

Value VM::runStdFunction(const std::string& name) {
    if (!loaded_[0]) runModule(0);
    for (size_t i = 0; i < globals_.size() && i < program_->globalNames.size(); i++)
        if (program_->globalNames[i] == name && globals_[i].isObjType(ObjType::Closure)) return call(globals_[i], 0, nullptr);
    throw runtimeError(Thrown::IllegalStateException, "the standard library has no function " + name + " for a worker to run");
}

Value VM::runModule(int index) {
    if (index != 0 && mainModule_ < 0) mainModule_ = index;  // the first a program runs: its own file
    if (!loaded_[0]) {
        // The standard library defines the classes runtime errors are instances of (D35).
        loaded_[0] = true;
        execute(program_->modules[0]);
        const std::vector<std::string>& known = globalsFoundByName();
        for (size_t i = 0; i < globals_.size(); i++) {
            if (!globals_[i].isObjType(ObjType::Class)) continue;
            for (size_t k = 0; k < static_cast<size_t>(Thrown::Count_); k++)
                if (thrownClasses_[k] == nullptr && program_->globalNames[i] == known[k]) thrownClasses_[k] = asClass(globals_[i]);
            if (program_->globalNames[i] == "RuntimeException" && errorClass_ == nullptr) errorClass_ = asClass(globals_[i]);
            if (program_->globalNames[i] == "MapEntry" && globals_[i].isObjType(ObjType::Class) && mapEntryClass_ == nullptr)
                mapEntryClass_ = asClass(globals_[i]);
            if (program_->globalNames[i] == "Class" && globals_[i].isObjType(ObjType::Class) && classOfFn_.isNull()) classOfFn_ = globals_[i];
            if (program_->globalNames[i] == "ListView" && globals_[i].isObjType(ObjType::Class) && viewClass_ == nullptr)
                viewClass_ = asClass(globals_[i]);
            if (program_->globalNames[i] == "ImageCanvas" && globals_[i].isObjType(ObjType::Class) && imageCanvasClass_ == nullptr)
                imageCanvasClass_ = asClass(globals_[i]);
        }
    }
    if (index == 0) return Value::null();  // the standard library, run above once
    loaded_[static_cast<size_t>(index)] = true;
    return execute(program_->modules[static_cast<size_t>(index)]);
}

Value VM::execute(const FunctionProto* proto) {
    try {
        ObjClosure* closure = alloc<ObjClosure>(proto, proto->upvalues.size());
        int base = frameCount_;
        *sp_++ = Value::object(closure);
        callClosure(closure, 0);
        run(base);
        return *--sp_;
    } catch (...) {
        resetState();
        throw;
    }
}

void VM::resetState() {
    closeUpvalues(stack_);
    sp_ = stack_;
    frameCount_ = 0;
    handlers_.clear();
    nativeDepth_ = 0;
}

Value VM::call(Value callee, int argc, const Value* args) {
    if (nativeDepth_ >= kMaxNativeDepth || !roomFor(sp_ + argc + 1)) throw runtimeError(Thrown::StackOverflowError, "stack overflow");

    NativeCallGuard guard(nativeDepth_, nativeBase_);

    int base = frameCount_;
    *sp_++ = callee;
    for (int i = 0; i < argc; i++) *sp_++ = args[i];
    if (callValue(callee, argc)) run(base);
    return *--sp_;
}

Value VM::callOn(Value receiver, Value callee, int argc, const Value* args) {
    // Only a function written in WinterWolf can be given an object to run on.
    if (!callee.isObjType(ObjType::Closure)) return call(callee, argc, args);
    if (nativeDepth_ >= kMaxNativeDepth || !roomFor(sp_ + argc + 1)) throw runtimeError(Thrown::StackOverflowError, "stack overflow");

    NativeCallGuard guard(nativeDepth_, nativeBase_);

    int base = frameCount_;
    *sp_++ = receiver;  // slot 0, where a method has `this`
    for (int i = 0; i < argc; i++) *sp_++ = args[i];
    callClosure(asClosure(callee), argc);
    run(base);
    return *--sp_;
}

// The object a name in a lambda belongs to: the one the lambda is running on, or failing that
// the one an enclosing lambda is running on, whichever first has a member of that name.
// Undefined if none has, in which case the name is a global.
// The extension function called `name` that a value has, if any: one for its class, a class
// above it or an interface it implements; for its built-in type; or one for every value.
// An extension added to a type under a name: in place of one with the same signature (the
// receiver's type and the parameters' types, as written), or as another form beside those
// there already (D56). A name with several forms holds the set of them, and each form is also
// found under its own name, the name with its signature, for calls that know which they mean.
void VM::addExtensionForm(Table& table, ObjString* name, Value function) {
    ObjClosure* added = asClosure(function);
    // Under its own name always, whether or not the name has other forms: a call the checker
    // has told which form it means names it so (one with a receiver that takes null, which null
    // alone could not choose).
    if (!added->proto->signature.empty()) table.set(intern(name->utf8() + added->proto->signature), function);
    const Value* existing = table.find(name);
    if (existing == nullptr || !existing->isObjType(ObjType::Closure) || added->proto->signature.empty()) {
        table.set(name, function);
        return;
    }
    TempRoot keep(*this, function);
    ObjClosure* before = asClosure(*existing);
    std::vector<ObjClosure*> forms;
    if (before->proto->chooser) forms = static_cast<const ObjForms*>(before)->forms;
    else forms.push_back(before);
    bool replaced = false;
    for (ObjClosure*& form : forms) {
        if (form->proto->signature.empty() || form->proto->signature == added->proto->signature) {
            form = added;
            replaced = true;
        }
    }
    if (!replaced) forms.push_back(added);
    if (forms.size() == 1) {
        table.set(name, function);
        return;
    }
    std::unique_ptr<FunctionProto>& proto = extensionSets_[name->utf8()];
    if (proto == nullptr) {
        proto = std::make_unique<FunctionProto>();
        proto->name = name->utf8();
        proto->chooser = true;
    }
    for (ObjClosure* form : forms) table.set(intern(name->utf8() + form->proto->signature), Value::object(form));
    ObjForms* set = alloc<ObjForms>(proto.get());
    set->forms = std::move(forms);
    account(set->forms.size() * sizeof(ObjClosure*));
    table.set(name, Value::object(set));
}

bool VM::inReachSlow(const Value* found, const ObjString* name) const {
    for (const ObjClosure* form : static_cast<const ObjForms*>(asClosure(*found))->forms)
        if (inReach(form->proto)) return true;
    return inReachSlow(name);
}

// A form named whole: code copied in from an inline function names the one it means in the
// file it is written in.
bool VM::inReachSlow(const ObjString* name) const { return name->utf8().find(kLibraryMark) != std::string::npos; }

template <bool InReach>
const Value* VM::findExtensionOf(Value receiver, const ObjString* name) const {
    if (!hasExtensions_) return nullptr;
    const Value* found = nullptr;
    // What a type has under the name; with InReach, only if the running function has it in reach.
    auto reached = [&](const Value* v) { return !InReach || inReach(v, name) ? v : nullptr; };
    switch (receiver.kind) {
        case ValueKind::Null:
            found = reached(nullExtensions_.find(name));
            // Two types each have one that takes null (string?.orEmpty, list?.orEmpty), and
            // null does not say which is meant.
            if (found != nullptr && !found->isObjType(ObjType::Closure))
                throw const_cast<VM*>(this)->runtimeError(Thrown::IllegalStateException, 
                    name->utf8() + "() is called on null, and more than one type has its own " + name->utf8() +
                    "() that takes null, so there is no telling which is meant; use ?: to say what stands in for null");
            break;
        case ValueKind::Bool: found = reached(typeExtensions_[kTypeBool].find(name)); break;
        case ValueKind::Int: found = reached(typeExtensions_[kTypeInt].find(name)); break;
        case ValueKind::Float: found = reached(typeExtensions_[kTypeFloat].find(name)); break;
        case ValueKind::Char: found = reached(typeExtensions_[kTypeChar].find(name)); break;
        case ValueKind::Byte: found = reached(typeExtensions_[kTypeByte].find(name)); break;
        case ValueKind::FromEnd: found = reached(typeExtensions_[kTypeIndex].find(name)); break;
        case ValueKind::Obj:
            switch (receiver.as.obj->type) {
                case ObjType::String: found = reached(typeExtensions_[kTypeString].find(name)); break;
                case ObjType::List: found = reached(typeExtensions_[kTypeList].find(name)); break;
                case ObjType::Map: found = reached(typeExtensions_[kTypeMap].find(name)); break;
                case ObjType::Set: found = reached(typeExtensions_[asSet(receiver)->values ? kTypeValues : kTypeSet].find(name)); break;
                case ObjType::Range: found = reached(typeExtensions_[kTypeRange].find(name)); break;
                case ObjType::Bytes: found = reached(typeExtensions_[kTypeBytes].find(name)); break;
                case ObjType::Closure:
                case ObjType::Native:
                case ObjType::BoundMethod:
                case ObjType::BoundNative: found = reached(typeExtensions_[kTypeFunction].find(name)); break;
                case ObjType::Instance:
                case ObjType::Value: {
                    const ObjClass* cls = classOf(receiver);
                    for (const ObjClass* k = cls; k != nullptr && found == nullptr; k = k->super) found = reached(k->extensions.find(name));
                    for (size_t i = 0; i < cls->interfaces.size() && found == nullptr; i++)
                        found = reached(cls->interfaces[i]->extensions.find(name));
                    break;
                }
                default: break;
            }
            break;
        default: break;
    }
    if (found == nullptr && receiver.isNumber()) found = reached(typeExtensions_[kTypeNumber].find(name));
    if (found == nullptr && !receiver.isNull()) found = reached(anyExtensions_.find(name));
    return found;
}
template const Value* VM::findExtensionOf<false>(Value, const ObjString*) const;
template const Value* VM::findExtensionOf<true>(Value, const ObjString*) const;

const Value* VM::findExtension(Value receiver, const ObjString* name) const {
    const Value* found = findExtensionOf<false>(receiver, name);
    // Nearly always one in reach: only where it is not is the search made again, passing over
    // those out of reach.
    return found == nullptr || inReach(found, name) ? found : findExtensionOf<true>(receiver, name);
}

// ---- extend of a built-in type (D33, G30) ----

// The built-in type of a value with no class: its TypeId, or 0 for an object or a value of a
// class (whose extends its class keeps) and anything else.
uint8_t VM::builtinIdOf(const Value& v) {
    switch (v.kind) {
        case ValueKind::Bool: return kTypeBool;
        case ValueKind::Int: return kTypeInt;
        case ValueKind::Float: return kTypeFloat;
        case ValueKind::Char: return kTypeChar;
        case ValueKind::Byte: return kTypeByte;
        case ValueKind::Obj:
            switch (v.as.obj->type) {
                case ObjType::String: return kTypeString;
                case ObjType::List: return kTypeList;
                case ObjType::Map: return kTypeMap;
                case ObjType::Set: return asSet(v)->values ? 0 : kTypeSet;
                case ObjType::Range: return kTypeRange;
                case ObjType::Bytes: return kTypeBytes;
                default: return 0;
            }
        default: return 0;
    }
}

// Whether a fit holds for the value: for every value of the type, or, for one written with
// what it holds (List<int>), by what the collection was made to hold. One made without saying
// (a list from before types were kept) is judged by what it holds.
bool VM::fitHolds(const BuiltinFit& fit, const Value& v) {
    if (fit.type->args.empty()) return true;
    const ObjTypeInfo* made = v.isObjType(ObjType::List) ? asList(v)->type
                            : v.isObjType(ObjType::Map)  ? asMap(v)->type
                            : v.isObjType(ObjType::Set)  ? asSet(v)->type
                                                         : nullptr;
    if (made != nullptr) return isSubtype(made, fit.type);
    if (v.isObjType(ObjType::List)) {
        ObjList* list = asList(v);
        for (size_t i = 0; i < list->size(); i++)
            if (!fitsType(listGet(list, i), fit.type->args[0])) return false;
        return true;
    }
    if (v.isObjType(ObjType::Set)) {
        for (const ValueMap::Entry& e : asSet(v)->items.entries())
            if (e.live && !fitsType(e.key, fit.type->args[0])) return false;
        return true;
    }
    if (v.isObjType(ObjType::Map) && fit.type->args.size() == 2) {
        for (const ValueMap::Entry& e : asMap(v)->map.entries())
            if (e.live && (!fitsType(e.key, fit.type->args[0]) || !fitsType(e.value, fit.type->args[1]))) return false;
        return true;
    }
    return false;
}

std::vector<ObjClass*> VM::builtinInterfaces(const ObjTypeInfo* type) {
    std::vector<ObjClass*> found;
    if (type->id == 0 || type->id == kTypeInstance || type->id >= kTypeCount_) return found;
    for (const BuiltinFit& fit : builtinFits_[unfrozenTypeId(type->id)])
        if (isSubtype(type, fit.type) && std::find(found.begin(), found.end(), fit.iface) == found.end()) found.push_back(fit.iface);
    return found;
}

bool VM::builtinFitsClass(const Value& v, const ObjClass* target) {
    if (!hasBuiltinFits_ || target == nullptr || !target->isInterface) return false;
    const uint8_t id = builtinIdOf(v);
    if (id == 0) return false;
    for (const BuiltinFit& fit : builtinFits_[id])
        if (fit.iface == target && fitHolds(fit, v)) return true;
    return false;
}

bool VM::builtinTypeFitsClass(const ObjTypeInfo* type, const ObjClass* target) {
    if (!hasBuiltinFits_ || target == nullptr || !target->isInterface || type->id == kTypeInstance || type->id >= kTypeCount_) return false;
    // A FrozenList is a List (D26).
    for (const BuiltinFit& fit : builtinFits_[unfrozenTypeId(type->id)])
        if (fit.iface == target && isSubtype(type, fit.type)) return true;
    return false;
}

// The fits written with what the type holds come first: the nearer to what the value is.
const Value* VM::builtinFitGetter(const Value& v, const ObjString* name) {
    if (!hasBuiltinFits_) return nullptr;
    const uint8_t id = builtinIdOf(v);
    if (id == 0 || builtinFits_[id].empty()) return nullptr;
    for (int pass = 0; pass < 2; pass++)
        for (const BuiltinFit& fit : builtinFits_[id]) {
            if (fit.type->args.empty() != (pass == 1) || !fitHolds(fit, v)) continue;
            if (const Value* getter = fit.getters.find(name)) return getter;
            if (const Value* getter = fit.iface->getters.find(name)) return getter;
        }
    return nullptr;
}

const Value* VM::builtinFitMethod(const Value& v, const ObjString* name) {
    if (!hasBuiltinFits_) return nullptr;
    const uint8_t id = builtinIdOf(v);
    if (id == 0 || builtinFits_[id].empty()) return nullptr;
    for (int pass = 0; pass < 2; pass++)
        for (const BuiltinFit& fit : builtinFits_[id]) {
            if (fit.type->args.empty() != (pass == 1) || !fitHolds(fit, v)) continue;
            if (const Value* method = fit.methods.find(name)) return method;
            if (const Value* method = fit.iface->methods.find(name)) return method;
        }
    return nullptr;
}

// Whether a bare name written in code that runs on `candidate` means a member of it.
bool VM::hasMember(Value candidate, const ObjString* name) const {
    guardRead(candidate);
    if (candidate.isObjType(ObjType::Value) || candidate.isSmall() || candidate.isRef()) {
        const ObjClass* cls = classOf(candidate);
        if (cls->declaredIndex.find(name) != nullptr || cls->methods.find(name) != nullptr || cls->getters.find(name) != nullptr)
            return true;
        if (cls->extendMethod(name) != nullptr || cls->extendGetter(name) != nullptr) return true;  // an extend's (D33, D186)
        return findExtension(candidate, name) != nullptr;
    }
    if (candidate.isObjType(ObjType::Instance)) {
        const ObjInstance* instance = asInstance(candidate);
        const ObjClass* cls = instance->cls;
        if (instance->hasValue(name) || cls->methods.find(name) != nullptr || cls->extendField(name) != nullptr ||
            cls->declaredIndex.find(name) != nullptr ||
            (cls->hasAccessors && (cls->getters.find(name) != nullptr || cls->setters.find(name) != nullptr)))
            return true;
        if (cls->extendMethod(name) != nullptr || cls->extendGetter(name) != nullptr) return true;  // an extend's (D33, D186)
        return findExtension(candidate, name) != nullptr;
    }
    // Slot 0 of an ordinary function is the function itself, which has no members to speak of.
    if (candidate.isObjType(ObjType::Closure) || candidate.isObjType(ObjType::Native) ||
        candidate.isObjType(ObjType::BoundMethod) || candidate.isObjType(ObjType::BoundNative) ||
        candidate.isObjType(ObjType::Class))
        return false;
    // An extension function on a built-in type runs on a string, a list, a number...
    if (!hasExtensions_) return false;
    if (findNativeMethod(candidate, name) != nullptr || findExtension(candidate, name) != nullptr) return true;
    if (hasBuiltinFits_) {
        VM& self = const_cast<VM&>(*this);
        if (self.builtinFitGetter(candidate, name) != nullptr || self.builtinFitMethod(candidate, name) != nullptr) return true;
    }
    // The properties getProperty answers for the built-in types, as it answers them.
    if (candidate.isChar()) return name == names.code;
    if (candidate.isInt() || candidate.isFromEnd()) {
        if (name == names.fromEnd || name == names.value) return true;
        if (!candidate.isInt()) return false;
    }
    if (candidate.isInt() || candidate.isFloat())
        return name == names.absoluteValue || name == names.sign || (candidate.isInt() && name == names.character);
    if (!candidate.isObj()) return false;
    const ObjType type = candidate.as.obj->type;
    if (name == names.size)
        return type == ObjType::String || type == ObjType::List || type == ObjType::Map || type == ObjType::Range ||
               type == ObjType::Bytes || type == ObjType::Set;
    if (name == names.indices || name == names.lastIndex) return type == ObjType::String || type == ObjType::List;
    if (name == names.itemType) return type == ObjType::List || type == ObjType::Set;
    if (name == names.keyType || name == names.valueType || name == names.keys || name == names.values || name == names.entries)
        return type == ObjType::Map;
    return type == ObjType::Range && (name == names.start || name == names.end || name == names.step || name == names.first || name == names.last);
}

// ---- Who may use a member ----
//
// A member written `private`, `protected` or `internal` belongs to some of the program's
// code only. The compiler gives every class and every library a number, and every function
// carries the numbers of the classes it is written inside of and of its library. The name
// of such a member leads to a mark: the numbers a function has to carry one of.
//
//   private     the class that declared it, and a subclass that declared it again, private too
//   protected   the class that declared it and each of its subclasses, down to the object's own
//   internal    the library the class is in
//
// A mark has two halves of 32 bits, the lower for using the member at all and the upper for
// assigning to it (`private set`). A half is count | kind << 8 | offset << 12, the offset
// and count of a run of numbers in the class's `accessIds`; a count of 0 is open to all.

namespace {

enum AccessKind { kAccessOpen = 0, kAccessPrivate = 1, kAccessProtected = 2, kAccessInternal = 3, kAccessAsAbove = 4 };
const int kAccessOfSetter = 0x10;

inline uint32_t halfOf(const Value& mark, bool write) {
    return static_cast<uint32_t>(static_cast<uint64_t>(mark.as.i) >> (write ? 32 : 0));
}
inline uint32_t halfCount(uint32_t half) { return half & 0xFF; }
inline int halfKind(uint32_t half) { return static_cast<int>((half >> 8) & 0xF); }
inline const uint16_t* halfRun(const ObjClass* cls, uint32_t half) { return cls->accessIds.data() + (half >> 12); }

// Whether a half keeps the member from code with the rights of `ids` (FunctionProto::classIds).
inline bool halfDenies(const ObjClass* cls, uint32_t half, const std::vector<uint16_t>& ids) {
    const uint32_t count = halfCount(half);
    if (count == 0) return false;
    const uint16_t* allowed = halfRun(cls, half);
    for (uint16_t written : ids)
        for (uint32_t i = 0; i < count; i++)
            if (allowed[i] == written) return false;
    return true;
}

inline bool halfDenies(const ObjClass* cls, uint32_t half, const FunctionProto* proto) { return halfDenies(cls, half, proto->classIds); }

inline bool writtenIn(const std::vector<uint16_t>& ids, const ObjClass* cls) {
    for (uint16_t written : ids)
        if (written == cls->number) return true;
    return false;
}

}  // namespace

// Gives one half of the mark of `name` a run of numbers, or none: open.
void VM::setAccess(ObjClass* cls, Table& marks, ObjString* name, bool write, const std::vector<uint16_t>& run, int kind) {
    uint32_t half = 0;
    if (!run.empty()) {
        if (run.size() > 0xFF || cls->accessIds.size() + run.size() > 0xFFFFF)
            throw runtimeError(Thrown::InternalError, "class " + cls->name->utf8() + " has too many private, protected and internal members");
        half = static_cast<uint32_t>(run.size()) | static_cast<uint32_t>(kind) << 8 |
               static_cast<uint32_t>(cls->accessIds.size()) << 12;
        cls->accessIds.insert(cls->accessIds.end(), run.begin(), run.end());
        name->restricted |= (write ? 2 : 1) | (kind == kAccessPrivate ? 4 : 0);
        if (&marks == &cls->accessMarks) cls->accessBits |= 1ull << (name->hash() & 63);
    }
    const Value* before = marks.find(name);
    uint64_t mark = before != nullptr ? static_cast<uint64_t>(before->as.i) : 0;
    mark = write ? (mark & 0xFFFFFFFFull) | static_cast<uint64_t>(half) << 32 : (mark & ~0xFFFFFFFFull) | half;
    marks.set(name, Value::integer(static_cast<int64_t>(mark)));
}

// What a class says of a member it declares: the access word in front of it, for the code
// with the number `id`.
void VM::declareAccess(ObjClass* cls, Table& marks, ObjString* name, int id, int kind, bool write) {
    const Value* before = marks.find(name);
    const uint32_t half = before != nullptr ? halfOf(*before, write) : 0;
    if (kind == kAccessOpen || kind == kAccessAsAbove) {
        // `public override` opens it. `override` alone leaves it as it was above, except
        // that what was private above is a new, open member here.
        if (halfCount(half) != 0 && (kind == kAccessOpen || halfKind(half) == kAccessPrivate)) setAccess(cls, marks, name, write, {}, 0);
        return;
    }
    // Declared again by a subclass, it is the subclass's too.
    std::vector<uint16_t> run(halfRun(cls, half), halfRun(cls, half) + halfCount(half));
    if (std::find(run.begin(), run.end(), static_cast<uint16_t>(id)) == run.end()) run.push_back(static_cast<uint16_t>(id));
    setAccess(cls, marks, name, write, run, kind);
}

// A class that has just been given its superclass: what is protected above is its own code's
// to use as well, on its own objects, and so are the protected static members above.
void VM::joinProtected(ObjClass* subclass) {
    auto join = [&](ObjClass* owner, Table& marks) {
        std::vector<ObjString*> names;
        for (uint32_t at = 0; at < marks.capacity(); at++)
            if (marks.entries()[at].key != nullptr) names.push_back(marks.entries()[at].key);
        for (ObjString* name : names) {
            for (bool write : {false, true}) {
                const uint32_t half = halfOf(*marks.find(name), write);
                if (halfCount(half) == 0 || halfKind(half) != kAccessProtected) continue;
                std::vector<uint16_t> run(halfRun(owner, half), halfRun(owner, half) + halfCount(half));
                if (std::find(run.begin(), run.end(), static_cast<uint16_t>(subclass->number)) != run.end()) continue;
                run.push_back(static_cast<uint16_t>(subclass->number));
                // A class above is everyone's: a core of a parallel section leaves it alone.
                if (owner != subclass && coreId_ != 0)
                    throw notOwned("class " + owner->name->utf8() + " has protected static members, and a subclass of it is declared for the whole program");
                setAccess(owner, marks, name, write, run, kAccessProtected);
            }
        }
    };
    if (subclass->number == 0) return;
    join(subclass, subclass->accessMarks);
    for (ObjClass* above = subclass->super; above != nullptr; above = above->super) join(above, above->staticAccessMarks);
}

// Whether the code of `proto` may not use the member `name` of `target` (an object, or a
// class for its static members), or may not assign to it. This is only asked for a name that
// some class restricts (see CHECK_ACCESS).
bool VM::memberDenied(const ObjClass* cls, const ObjString* name, const FunctionProto* proto, bool write) const {
    return memberDeniedTo(cls, name, proto != nullptr ? &proto->classIds : nullptr, write);
}

// As memberDenied, for code with the rights of `ids`; none for null.
bool VM::memberDeniedTo(const ObjClass* cls, const ObjString* name, const std::vector<uint16_t>* ids, bool write) const {
    // Code written in the object's own class may use all that the class has, or all but what a
    // class above keeps private: then a name that is private nowhere is its to use.
    if ((cls->accessBits >> (name->hash() & 63) & 1) == 0) return false;
    if (ids != nullptr && (cls->accessAllOwn || (cls->accessSharedOwn && (name->restricted & 4) == 0)) && writtenIn(*ids, cls)) return false;
    const Value* mark = cls->accessMarks.find(name);
    if (mark == nullptr) return false;
    if (ids == nullptr) return halfCount(halfOf(*mark, false)) != 0 || (write && halfCount(halfOf(*mark, true)) != 0);
    return halfDenies(cls, halfOf(*mark, false), *ids) || (write && halfDenies(cls, halfOf(*mark, true), *ids));
}

bool VM::accessDenied(Value target, const ObjString* name, const FunctionProto* proto, bool write) const {
    return accessDeniedTo(target, name, proto != nullptr ? &proto->classIds : nullptr, write);
}

bool VM::accessDeniedTo(Value target, const ObjString* name, const std::vector<uint16_t>* ids, bool write) const {
    if (!target.isObj() && !target.isSmall() && !target.isRef()) return false;
    const ObjClass* cls;
    const Table* marks;
    if (!target.isObj() || target.as.obj->type == ObjType::Instance || target.as.obj->type == ObjType::Value) {
        return memberDeniedTo(classOf(target), name, ids, write);
    } else if (target.as.obj->type == ObjType::Class) {
        cls = asClass(target);
        marks = &cls->staticAccessMarks;
        if (marks->size() == 0 || (ids != nullptr && writtenIn(*ids, cls))) return false;
    } else {
        return false;
    }
    const Value* mark = marks->find(name);
    if (mark == nullptr) return false;
    if (ids == nullptr) return halfCount(halfOf(*mark, false)) != 0 || (write && halfCount(halfOf(*mark, true)) != 0);
    return halfDenies(cls, halfOf(*mark, false), *ids) || (write && halfDenies(cls, halfOf(*mark, true), *ids));
}

// As accessDenied, for the instruction before `ip`: code copied in from an inline function of
// another class or library may use what that one's own code may, where it is (D59, G36).
bool VM::accessDeniedAt(Value target, const ObjString* name, const FunctionProto* proto, bool write, const uint8_t* ip) const {
    return accessDenied(target, name, proto, write) && (proto->rights.empty() || !rightsAllow(target, name, proto, ip, write));
}

// Whether a range of `proto` around `ip` lets its code use the member.
bool VM::rightsAllow(Value target, const ObjString* name, const FunctionProto* proto, const uint8_t* ip, bool write) const {
    const uint32_t at = static_cast<uint32_t>(ip - proto->run.data());
    for (const FunctionProto::RunRights& r : proto->runRights)
        if (r.start < at && at <= r.end && !accessDeniedTo(target, name, &proto->rights[r.which].ids, write)) return true;
    return false;
}

// What is said of a member used by code that may not.
WwThrow VM::accessError(Value target, const ObjString* name, const FunctionProto* proto) {
    const bool isStatic = target.isObjType(ObjType::Class);
    const ObjClass* cls = isStatic ? asClass(target) : asInstance(target)->cls;
    const Value* mark = (isStatic ? cls->staticAccessMarks : cls->accessMarks).find(name);
    if (mark == nullptr) return runtimeError(Thrown::IllegalStateException, "'" + name->utf8() + "' cannot be used here");
    const bool write = !halfDenies(cls, halfOf(*mark, false), proto);
    const uint32_t half = halfOf(*mark, write);
    const int kind = halfKind(half);

    // The class that said so: the highest one that has the member marked as this one does. A
    // protected member's mark grows by a number with every subclass, so its start is compared.
    const ObjClass* declaring = cls;
    auto same = [&](const ObjClass* above) {
        const Value* other = above->accessMarks.find(name);
        const uint32_t theirs = other != nullptr ? halfOf(*other, write) : 0;
        if (halfCount(theirs) == 0 || halfKind(theirs) != kind) return false;
        if (kind == kAccessProtected) return halfRun(above, theirs)[0] == halfRun(cls, half)[0];
        return halfCount(theirs) == halfCount(half) &&
               std::equal(halfRun(cls, half), halfRun(cls, half) + halfCount(half), halfRun(above, theirs));
    };
    while (!isStatic && declaring->super != nullptr && same(declaring->super)) declaring = declaring->super;

    const char* const word = kind == kAccessProtected ? "protected" : kind == kAccessInternal ? "internal" : "private";
    const char* const who = kind == kAccessProtected ? "only code written in that class or in a subclass of it can "
                            : kind == kAccessInternal ? "only code of the library it belongs to can "
                            : "only code written in that class can ";
    if (write)
        return runtimeError(Thrown::IllegalStateException, "'" + name->utf8() + "' cannot be assigned here: its setter is " + word + " in class " +
                            declaring->name->utf8() + ", so " + who + "assign to it");
    return runtimeError(Thrown::IllegalStateException, "'" + name->utf8() + "' is " + word + " in class " + declaring->name->utf8() + ": " + who + "use it");
}

// An entry of FunctionProto::outerReceivers: an upvalue of the function, or with kCapturedReceiver
// a capture of the anonymous object a member runs on.
ObjUpvalue* VM::receiverUpvalue(const CallFrame* frame, const Value* base, uint16_t entry) const {
    if ((entry & kCapturedReceiver) == 0) return frame->closure->upvalues[entry];
    const uint16_t k = entry & static_cast<uint16_t>(~kCapturedReceiver);
    if (!base[0].isObjType(ObjType::Instance) || k >= asInstance(base[0])->captureCount) return nullptr;
    return asInstance(base[0])->captured(k);
}

Value VM::implicitReceiver(const CallFrame* frame, const Value* base, const ObjString* name) const {
    const FunctionProto* proto = frame->closure->proto;
    auto has = [&](Value candidate) {
        if (hasMember(candidate, name)) return !(name->restricted != 0 && accessDenied(candidate, name, proto, false));
        // A private member of that text the code may use: opened by an override below (o-private-shadow).
        const ObjString* own = privateLike(classOf(candidate), name);
        return own != nullptr && !accessDenied(candidate, own, proto, false);
    };
    implicitTried_ = implicitFirstCount_;
    if (implicitFirstCount_ != 0) {
        // The objects of blocks copied in place, innermost first (D58, D59).
        const int count = implicitFirstCount_;
        implicitFirstCount_ = 0;
        const bool only = std::exchange(implicitOnly_, false);
        for (int i = count; i-- > 0;)
            if (has(implicitFirst_[i])) return implicitFirst_[i];
        if (only) return Value::undefined();
    }
    // An object of a class, with no object around it to look in (no outer receivers): whether
    // the name is a member says its class alone, kept once found not to be (implicitMisses_).
    if (proto->outerReceivers.empty())
        if (const ObjClass* const cls = classOf(base[0])) {
            guardRead(base[0]);
            const size_t at = ((reinterpret_cast<uintptr_t>(name) >> 4) ^ (reinterpret_cast<uintptr_t>(cls) >> 6) ^
                               (reinterpret_cast<uintptr_t>(proto) >> 5)) & 255;
            ImplicitMiss& seen = implicitMisses_[at];
            const uint64_t keys = tableKeysChanged.load(std::memory_order_relaxed);
            if (seen.name == name && seen.cls == cls && seen.proto == proto && seen.keys == keys) return Value::undefined();
            if (has(base[0])) return base[0];
            seen = ImplicitMiss{proto, name, cls, keys};
            return Value::undefined();
        }
    if (has(base[0])) return base[0];
    for (uint16_t upvalue : proto->outerReceivers) {
        const ObjUpvalue* cell = receiverUpvalue(frame, base, upvalue);
        if (cell != nullptr && has(*cell->location)) return *cell->location;
    }
    return Value::undefined();
}

// A bare name that is neither a member the code may use nor a variable: said plainly when
// it is a member of the object the code runs on that the code may not use.
WwThrow VM::undefinedName(const CallFrame* frame, const Value* base, const ObjString* name, size_t global) {
    const FunctionProto* proto = frame->closure->proto;
    // The name the member is kept under where the code may not use it (its private one, o-private-shadow), or null.
    auto kept = [&](Value candidate) -> const ObjString* {
        if (hasMember(candidate, name)) return accessDenied(candidate, name, proto, false) ? name : nullptr;
        const ObjString* own = privateLike(classOf(candidate), name);
        return own != nullptr && accessDenied(candidate, own, proto, false) ? own : nullptr;
    };
    for (int i = std::exchange(implicitTried_, uint8_t{0}); i-- > 0;)
        if (const ObjString* k = kept(implicitFirst_[i])) return accessError(implicitFirst_[i], k, proto);
    if (const ObjString* k = kept(base[0])) return accessError(base[0], k, proto);
    for (uint16_t upvalue : proto->outerReceivers) {
        const ObjUpvalue* cell = receiverUpvalue(frame, base, upvalue);
        if (cell == nullptr) continue;
        Value outer = *cell->location;
        if (const ObjString* k = kept(outer)) return accessError(outer, k, proto);
    }
    return undefinedGlobal(program_->globalNames[global]);
}

// Runs a method that takes no arguments on `receiver` and returns its result.
Value VM::callWithReceiver(Value receiver, ObjClosure* method) {
    if (nativeDepth_ >= kMaxNativeDepth || !roomFor(sp_ + 1)) throw runtimeError(Thrown::StackOverflowError, "stack overflow");

    NativeCallGuard guard(nativeDepth_, nativeBase_);

    int base = frameCount_;
    *sp_++ = receiver;
    callClosure(method, 0);
    run(base);
    return *--sp_;
}

// ---- Errors ----

WwThrow VM::runtimeError(const std::string& message) { return runtimeError(Thrown::RuntimeException, message); }

namespace {
Value traceList(VM& vm, const std::vector<std::string>& lines);  // below, with the traces
}

WwThrow VM::runtimeError(Thrown kind, const std::string& message) {
    WwThrow t;
    ObjClass* cls = thrownClasses_[static_cast<size_t>(kind)];
    if (cls == nullptr) cls = errorClass_;
    if (cls == nullptr) {
        t.value = str(message);  // before the standard library has its classes
    } else {
        ObjInstance* error = newInstance(cls);
        error->setField(names.message, str(message));
        error->setField(names.cause, Value::null());
        error->setFieldText(intern("setAside"), Value::null());  // Throwable's errors set aside for it (G40)
        // Where it was made (Throwable.stackTrace): here, as an error made in WinterWolf code keeps
        // its maker's place. Not for running out of memory, which the strings would take more of.
        error->setFieldText(intern("frames"), kind == Thrown::OutOfMemoryError ? Value::null() : traceList(*this, captureTrace()));
        t.value = Value::object(error);
    }
    return t;
}

// The classes of Thrown, in its order, then the other globals the VM finds by name.
const std::vector<std::string>& VM::globalsFoundByName() {
    static const std::vector<std::string> names{
        "RuntimeException", "ArithmeticException", "IndexOutOfBoundsException", "NullPointerException", "ClassCastException",
        "IllegalArgumentException", "NumberFormatException", "IllegalStateException", "UnsupportedOperationException",
        "NoSuchElementException", "ConcurrentModificationException", "AssertionError", "StackOverflowError", "OutOfMemoryError",
        "InternalError", "IOException", "Throwable", "SocketTimeoutException", "TimeoutException", "CancellationException", "SecurityException", "ScriptLimitException",
        "UnknownHostException", "MapEntry"};
    return names;
}

std::vector<std::string> VM::membersFoundByName() const {
    std::vector<std::string> found{"onError"};  // a worker's handler (worker.cpp)
    // Every member of the names table is an ObjString*.
    const ObjString* const* first = reinterpret_cast<const ObjString* const*>(&names);
    for (size_t i = 0; i < sizeof names / sizeof(ObjString*); i++)
        if (first[i] != nullptr) found.push_back(first[i]->utf8());
    return found;
}

WwThrow VM::errorOf(const char* className, const std::string& message) {
    const std::vector<std::string>& known = globalsFoundByName();
    if (std::find(known.begin(), known.end(), className) == known.end())
        return runtimeError(Thrown::InternalError, std::string("internal error: ") + className + " is not among VM::globalsFoundByName()");
    for (size_t k = 0; k < static_cast<size_t>(Thrown::Count_); k++)
        if (known[k] == className) return runtimeError(static_cast<Thrown>(k), message);
    return runtimeError(message);
}

// What the program is told when memory runs out: an OutOfMemoryError, which it can catch.
WwThrow VM::outOfMemory() {
    return runtimeError(Thrown::OutOfMemoryError, "out of memory");
}

std::vector<std::string> VM::captureTrace() const { return captureTraceFrom(frameCount_ - 1); }

// A sample for ww --profile: every call under way, innermost first, calls put in place as the
// calls they were (as captureTraceFrom), on the thread running.
void VM::takeSample() {
    std::vector<profile::Call> calls;
    calls.reserve(static_cast<size_t>(frameCount_) + 4);
    for (int i = frameCount_ - 1; i >= 0; i--) {
        const CallFrame& f = frames_[static_cast<size_t>(i)];
        const FunctionProto* p = f.closure->proto;
        size_t offset = f.ip != nullptr ? static_cast<size_t>(f.ip - p->run.data()) : 0;
        if (offset > 0) offset--;
        int at = offset < p->runLines.size() ? p->runLines[offset] : 0;
        while (at >= kInlinedLine && static_cast<size_t>(at - kInlinedLine) < p->inlined.size()) {
            const FunctionProto::InlinedCall& call = p->inlined[static_cast<size_t>(at - kInlinedLine)];
            calls.push_back(profile::Call{call.name, call.source, call.line});
            at = call.callLine;
        }
        // A lambda is known by where it is written.
        std::string name = p->name;
        if (name == "<lambda>" && !p->runLines.empty() && p->runLines.front() > 0 && p->runLines.front() < kInlinedLine)
            name += ":" + std::to_string(p->runLines.front());
        calls.push_back(profile::Call{std::move(name), p->source, at});
    }
    profile::record(calls, thread_ != nullptr ? thread_->name : std::string());
}

// The calls under way from frame `top` down, innermost first: a deep stack keeps its ten
// innermost calls and its two outermost.
std::vector<std::string> VM::captureTraceFrom(int top) const {
    std::vector<std::string> trace;
    const int frames = top + 1;
    for (int i = top; i >= 0; i--) {
        if (frames > 14 && i == frames - 11) {
            trace.push_back("... " + std::to_string(i - 1) + " more calls ...");
            i = 1;
        }
        const CallFrame& f = frames_[static_cast<size_t>(i)];
        const FunctionProto* p = f.closure->proto;
        size_t offset = static_cast<size_t>(f.ip - p->run.data());
        if (offset > 0) offset--;
        int at = p->runLines[offset];
        // Calls put in place (FunctionProto::inlined), innermost first, as the calls they were.
        while (at >= kInlinedLine && static_cast<size_t>(at - kInlinedLine) < p->inlined.size()) {
            const FunctionProto::InlinedCall& call = p->inlined[static_cast<size_t>(at - kInlinedLine)];
            trace.push_back("at " + call.name + " (" + call.source + ":" + std::to_string(call.line) + ")");
            at = call.callLine;
        }
        std::string line = "at " + p->name + " (" + p->source + ":" + std::to_string(at) + ")";
        if (f.repeats > 0) {
            // In place of the frames a call of itself as its last step saved (D29).
            std::string count = std::to_string(static_cast<uint64_t>(f.repeats) + 1);
            for (int at = static_cast<int>(count.size()) - 3; at > 0; at -= 3) count.insert(static_cast<size_t>(at), ",");
            line += ", " + count + " times";
        }
        trace.push_back(std::move(line));
    }
    return trace;
}

namespace {

// Lines of a trace as a list of strings, for an error's `frames`.
Value traceList(VM& vm, const std::vector<std::string>& lines) {
    ObjList* list = newList(vm);
    std::vector<Value>& values = list->values();
    values.reserve(lines.size());
    for (const std::string& line : lines) values.push_back(vm.str(line));
    vm.account(values.size() * sizeof(Value));
    return Value::object(list);
}

}  // namespace

Value VM::stackHere() {
    int top = frameCount_ - 1;
    if (top >= 0) {
        const Value made = frames_[static_cast<size_t>(top)].base[0];
        if (made.isObjType(ObjType::Instance))
            while (top >= 0 && frames_[static_cast<size_t>(top)].base[0].isObj() && frames_[static_cast<size_t>(top)].base[0].as.obj == made.as.obj) top--;
    }
    return traceList(*this, captureTraceFrom(top));
}

Value VM::snippyFrames() {
    auto snippy = [](const std::string& source) {
        auto ends = [&](const char* tail) {
            const size_t n = std::strlen(tail);
            return source.size() >= n && source.compare(source.size() - n, n, tail) == 0;
        };
        return ends(".py") || ends(".snippy");
    };
    std::vector<std::string> trace;
    for (int i = frameCount_ - 1; i >= 0; i--) {
        const CallFrame& f = frames_[static_cast<size_t>(i)];
        const FunctionProto* p = f.closure->proto;
        size_t offset = static_cast<size_t>(f.ip - p->run.data());
        if (offset > 0) offset--;
        int at = p->runLines[offset];
        while (at >= kInlinedLine && static_cast<size_t>(at - kInlinedLine) < p->inlined.size()) {
            const FunctionProto::InlinedCall& call = p->inlined[static_cast<size_t>(at - kInlinedLine)];
            if (snippy(call.source)) trace.push_back("at " + call.name + " (" + call.source + ":" + std::to_string(call.line) + ")");
            at = call.callLine;
        }
        if (!snippy(p->source)) continue;
        trace.push_back("at " + p->name + " (" + p->source + ":" + std::to_string(at) + ")");
    }
    return traceList(*this, trace);
}

// Transfers control to the innermost catch block, if it belongs to the run() that is asking.
bool VM::tryHandle(Value exception, int baseFrame) {
    if (handlers_.empty()) return false;
    Handler h = handlers_.back();
    if (h.frameIndex < baseFrame) return false;  // belongs to an outer run()

    handlers_.pop_back();
    closeUpvalues(h.stackTop);
    pendingTypeArgs_ = nullptr;  // a call that failed before it began
    pendingDestination_ = Value::null();
    frameCount_ = h.frameIndex + 1;
    sp_ = h.stackTop;
    *sp_++ = exception;
    frames_[static_cast<size_t>(h.frameIndex)].ip = h.ip;
    return true;
}

void VM::unwindTo(int baseFrame) {
    Value* base = frames_[static_cast<size_t>(baseFrame)].base;
    closeUpvalues(base);
    sp_ = base;
    frameCount_ = baseFrame;
}

std::string VM::typeName(Value v) const {
    switch (v.kind) {
        case ValueKind::Null: return "null";
        case ValueKind::Ref: return refClass(v)->name->utf8();
        case ValueKind::Small: return smallClassOf(v)->name->utf8();
        case ValueKind::Bool: return "bool";
        case ValueKind::Int: return "int";
        case ValueKind::Char: return "char";
        case ValueKind::Byte: return "byte";
        case ValueKind::FromEnd: return "Index";
        case ValueKind::Float: return "float";
        case ValueKind::Undefined: return "undefined";
        case ValueKind::EnumName:  // the bare name it holds, a string
        case ValueKind::Obj: break;
    }
    switch (v.as.obj->type) {
        case ObjType::String: return "string";
        case ObjType::List: return "List";
        case ObjType::Map: return "Map";
        case ObjType::Set: return asSet(v)->values ? "Values" : "Set";
        case ObjType::Range: return "Range";
        case ObjType::Bytes: return "Bytes";
        case ObjType::Closure:
        case ObjType::Native:
        case ObjType::BoundMethod:
        case ObjType::BoundNative: return "Function";
        case ObjType::Class: return "Class";
        case ObjType::Instance: return asInstance(v)->cls->name->utf8();
        case ObjType::Value: return asValue(v)->cls->name->utf8();
        case ObjType::Fiber: return "task";
        case ObjType::Resource: return static_cast<const ObjResource*>(v.as.obj)->kind;
        case ObjType::Upvalue:
        case ObjType::Iterator:
        case ObjType::Lazy:
        case ObjType::Places: return "internal";
        case ObjType::TypeInfo: return "Type";
    }
    return "unknown";
}

// A byte is taken where an int or a number is (M6.6 part 8, group 9).
void VM::notAnInt(Value v, const char* what) {
    throw runtimeError(Thrown::ClassCastException, std::string(what) + " must be an int, got " + typeName(v));
}

void VM::notANumber(Value v, const char* what) {
    throw runtimeError(Thrown::ClassCastException, std::string(what) + " must be a number, got " + typeName(v));
}

const std::string& VM::toStr(Value v, const char* what) {
    if (!v.isString()) throw runtimeError(Thrown::ClassCastException, std::string(what) + " must be a string, got " + typeName(v));
    return asString(v)->utf8();
}

// ---- Calls ----

void VM::checkArity(const std::string& name, int minArgs, int maxArgs, int argc) {
    if (argc >= minArgs && (maxArgs < 0 || argc <= maxArgs)) return;
    std::string expected = maxArgs < 0 ? "at least " + std::to_string(minArgs)
                           : minArgs == maxArgs ? std::to_string(minArgs)
                           : std::to_string(minArgs) + " to " + std::to_string(maxArgs);
    throw runtimeError(Thrown::IllegalArgumentException, name + "() expects " + expected + " argument(s) but got " + std::to_string(argc));
}

WwThrow VM::undefinedGlobal(const std::string& name) {
    if (auto found = absentGlobals.find(name); found != absentGlobals.end()) return runtimeError(found->second.thrown, found->second.message);
    return runtimeError(Thrown::IllegalStateException, "undefined variable '" + name + "'");
}

// A bare name was passed where nothing says which enum it could belong to.
WwThrow VM::unsettledName(Value pending) {
    pendingEnums_--;
    return undefinedGlobal(static_cast<ObjString*>(pending.as.obj)->utf8());
}

void VM::rejectPendingNames(const Value* args, int argc) {
    for (int i = 0; i < argc; i++)
        if (args[i].kind == ValueKind::EnumName) throw unsettledName(args[i]);
}

// The name has reached a parameter declared with the class `type`: it is the entry of that
// name if the class is an enum, and an error otherwise.
Value VM::settleEnumName(Value pending, const ObjClass* type) {
    if (type != nullptr && type->staticInit != nullptr) ensureClassInit(const_cast<ObjClass*>(type));
    if (type == nullptr || !type->isEnum) throw unsettledName(pending);
    pendingEnums_--;
    const ObjString* name = static_cast<ObjString*>(pending.as.obj);
    const Value* entry = type->statics.find(name);
    if (entry == nullptr || !entry->isObjType(ObjType::Instance) || asInstance(*entry)->cls != type)
        throw runtimeError(Thrown::ClassCastException, "'" + name->utf8() + "' is not an entry of enum " + type->name->utf8());
    return *entry;
}

// For a function with a `vararg` parameter: gathers the arguments from that parameter's
// position on into a list, leaving the parameters after it to their defaults. Returns the
// new argument count.
int VM::packVarargs(const FunctionProto* proto, int argc) {
    const int at = proto->varargIndex;
    if (argc < proto->requiredArgs()) checkArity(proto->name, proto->requiredArgs(), -1, argc);
    if (!roomFor(sp_ - argc - 1 + proto->maxStack)) throw runtimeError(Thrown::StackOverflowError, "stack overflow");

    for (; argc < at; argc++) *sp_++ = Value::undefined();
    const int extra = argc - at;
    if (pendingEnums_ > 0) rejectPendingNames(sp_ - extra, extra);
    ObjList* list = alloc<ObjList>();
    list->values().assign(sp_ - extra, sp_);
    account(static_cast<size_t>(extra) * sizeof(Value));
    sp_ -= extra;
    *sp_++ = Value::object(list);
    return at + 1;
}

// How well an argument fits a parameter of a form: -1 not at all, 0 a parameter without a
// type, more the narrower the type is (a class nearer the object's own, more still).
int VM::paramFit(Value v, uint8_t type, const ObjString* className) const {
    if (type == 0) return 0;
    if ((type & kTypeMask) != kTypeInstance) {
        if (!matchesType(v, type, nullptr)) return -1;
        return (type & kTypeMask) == kTypeNumber ? 1 : 2;
    }
    if (v.isNull()) return (type & kTypeNullable) != 0 ? 1 : -1;
    if (className == nullptr) return -1;
    // The class is known by its name, as written: the last part of it.
    auto named = [&](const ObjClass* c) {
        const std::string& n = c->name->utf8();
        const std::string& want = className->utf8();
        return n == want || (n.size() > want.size() && n[n.size() - want.size() - 1] == '.' &&
                             n.compare(n.size() - want.size(), want.size(), want) == 0);
    };
    if (classOf(v) == nullptr) {
        // A built-in value an extend fitted to the interface (G30, G51): `1..3` for an Iterable,
        // as narrowly as an interface well above a class.
        const uint8_t id = hasBuiltinFits_ ? builtinIdOf(v) : 0;
        if (id != 0)
            for (const BuiltinFit& fit : builtinFits_[id])
                if (named(fit.iface) && const_cast<VM*>(this)->fitHolds(fit, v)) return 2;
        return -1;
    }
    int distance = 0;
    for (const ObjClass* c = classOf(v); c != nullptr; c = c->super, distance++) {
        if (named(c)) return 2 + std::max(0, 64 - distance);
        for (const ObjClass* i : c->interfaces)
            if (named(i)) return 2 + std::max(0, 63 - distance);
    }
    return -1;
}

// Whether a form takes a call's arguments: as many as there are, each of a type its parameter
// takes. `pairs` holds a named call's (name, value) pairs; the empty name is a lambda written
// after the call, which goes where callNamed puts it. `score` says how narrowly the types fit.
bool VM::formTakes(const FunctionProto* proto, const Value* args, int positional, const Value* pairs, int named,
                   int& score, int& defaultsLeft) const {
    const int arity = proto->arity;
    const int vararg = proto->varargIndex;
    const int required = proto->requiredArgs();
    if (vararg < 0 && positional > arity) return false;
    const bool typed = proto->paramTypes.size() == static_cast<size_t>(arity);
    auto fit = [&](int param, Value v) {
        if (!typed || param == vararg) return 0;
        const int f = paramFit(v, proto->paramTypes[static_cast<size_t>(param)], proto->paramClasses[static_cast<size_t>(param)]);
        // A function written in WinterWolf, given for a parameter of a function type: it has to
        // take as many arguments as the type gives.
        const size_t counted = proto->paramCounts.size() > static_cast<size_t>(param) ? proto->paramCounts[static_cast<size_t>(param)] : 0;
        if (f >= 0 && counted > 0) {
            const FunctionProto* given = v.isObjType(ObjType::Closure)       ? asClosure(v)->proto
                                         : v.isObjType(ObjType::BoundMethod) ? asBoundMethod(v)->method->proto
                                                                             : nullptr;
            const int wanted = static_cast<int>(counted) - 1;
            if (given != nullptr && !given->chooser && given->varargIndex < 0 && (wanted < given->requiredArgs() || wanted > given->arity))
                return -1;
        }
        return f;
    };
    uint64_t given = 0;  // the parameters given a value (the first 64)
    auto give = [&](int param) {
        if (param < 64) given |= uint64_t{1} << param;
    };
    auto isGiven = [&](int param) { return param < 64 && ((given >> param) & 1) != 0; };
    score = 0;
    const int direct = vararg >= 0 && positional > vararg ? vararg : positional;
    for (int i = 0; i < direct; i++) {
        const int f = fit(i, args[i]);
        if (f < 0) return false;
        score += f;
        give(i);
    }
    if (vararg >= 0 && positional > vararg) give(vararg);
    for (int k = 0; k < named; k++) {
        const ObjString* name = asString(pairs[2 * k]);
        const Value value = pairs[2 * k + 1];
        int index = -1;
        if (name->utf8().empty()) {
            if (arity == 0) return false;
            index = arity - 1;
        } else {
            for (int p = 0; p < arity && index < 0; p++)
                if (proto->paramNames.size() == static_cast<size_t>(arity) && proto->paramNames[static_cast<size_t>(p)] == name) index = p;
        }
        if (index < 0 || isGiven(index)) return false;
        const int f = fit(index, value);
        if (f < 0) return false;
        score += f;
        give(index);
    }
    defaultsLeft = 0;
    for (int p = 0; p < arity; p++) {
        if (isGiven(p) || p == vararg || p >= 64) continue;
        if (p < required) return false;
        defaultsLeft++;
    }
    return true;
}

// The form of a name with several (D56) that a call means, when the compiler could not tell
// (an argument whose type was not known, or code compiled on its own): of the forms that take
// the arguments, the one whose parameters' types fit them most narrowly; between two as
// narrow, one without a vararg, then one that leaves fewer parameters to their defaults.
const Value* VM::addedOperator(Value left, const Value* right, const ObjString* symbol) {
    // Whether the operator (one form, or several) takes the right side.
    auto takes = [&](const Value* added) {
        if (added == nullptr || !added->isObjType(ObjType::Closure)) return false;
        const ObjClosure* closure = asClosure(*added);
        int score = 0, defaults = 0;
        if (!closure->proto->chooser) return inReach(closure->proto) && formTakes(closure->proto, right, 1, nullptr, 0, score, defaults);
        for (const ObjClosure* form : static_cast<const ObjForms*>(closure)->forms)
            if (inReach(form->proto) && formTakes(form->proto, right, 1, nullptr, 0, score, defaults)) return true;
        return false;
    };
    const Value* added = findExtension(left, symbol);
    if (takes(added)) return added;
    // The int's own (op int.* for BigInt) not taking it, Number's or any's may (op Number.* for a
    // Tensor), as the checker chose (2026-11-19).
    if (hasExtensions_ && left.isNumber()) {
        if (const Value* broader = typeExtensions_[kTypeNumber].find(symbol); broader != added && takes(broader)) return broader;
    }
    if (hasExtensions_ && !left.isNull()) {
        if (const Value* broader = anyExtensions_.find(symbol); broader != added && takes(broader)) return broader;
    }
    return nullptr;
}

ObjClosure* VM::chooseForm(const ObjForms* set, const Value* args, int positional, const Value* pairs, int named) {
    ObjClosure* best = nullptr;
    int bestScore = -1, bestDefaults = 0;
    bool bestVararg = false, tie = false;
    for (ObjClosure* form : set->forms) {
        int score = 0, defaults = 0;
        if (!inReach(form->proto) || !formTakes(form->proto, args, positional, pairs, named, score, defaults)) continue;
        const bool vararg = form->proto->varargIndex >= 0;
        bool better = best == nullptr || score > bestScore;
        bool same = !better && score == bestScore;
        // A program's own extension comes before the standard library's with the same
        // parameters, as the checker has it.
        if (same && form->proto->library != best->proto->library &&
            (form->proto->library == kEverywhere || best->proto->library == kEverywhere) &&
            form->proto->signature.substr(0, form->proto->signature.rfind(kLibraryMark)) ==
                best->proto->signature.substr(0, best->proto->signature.rfind(kLibraryMark))) {
            better = best->proto->library == kEverywhere;
            same = false;
        }
        if (same && vararg != bestVararg) {
            better = !vararg;
            same = false;
        }
        if (same && defaults != bestDefaults) {
            better = defaults < bestDefaults;
            same = false;
        }
        if (better) {
            best = form;
            bestScore = score;
            bestDefaults = defaults;
            bestVararg = vararg;
            tie = false;
        } else if (same) {
            tie = true;
        }
    }
    const std::string& name = set->proto->name;
    auto formsText = [&](const std::vector<ObjClosure*>& forms) {
        std::string text;
        for (size_t i = 0; i < forms.size(); i++) {
            if (i > 0) text += i + 1 == forms.size() ? " and " : ", ";
            const std::string& signature = forms[i]->proto->signature;
            text += name + signature.substr(0, signature.rfind(kLibraryMark));
        }
        return text;
    };
    if (best == nullptr) {
        std::string given;
        for (int i = 0; i < positional; i++) given += (given.empty() ? "" : ", ") + typeName(args[i]);
        for (int k = 0; k < named; k++) {
            const std::string& n = asString(pairs[2 * k])->utf8();
            given += (given.empty() ? "" : ", ") + (n.empty() ? std::string() : n + " = ") + typeName(pairs[2 * k + 1]);
        }
        throw runtimeError(Thrown::IllegalArgumentException, "no form of " + name + "() takes " + (given.empty() ? std::string("no arguments") : "(" + given + ")") +
                           "; there are " + formsText(set->forms));
    }
    if (tie) {
        // Every form that fits as well as the best, in the order they were written.
        std::vector<ObjClosure*> tied;
        for (ObjClosure* form : set->forms) {
            int score = 0, defaults = 0;
            if (formTakes(form->proto, args, positional, pairs, named, score, defaults) && score == bestScore &&
                defaults == bestDefaults && (form->proto->varargIndex >= 0) == bestVararg)
                tied.push_back(form);
        }
        throw runtimeError(Thrown::IllegalArgumentException, "this call fits more than one form of " + name + "(): " + formsText(tied) +
                           "; write the types of what is passed, so that the form is chosen when compiling");
    }
    return best;
}

// The first `base` of `outer` (any where it has fewer), then all of `own`.
const ObjTypeInfo* VM::joinTypeArgs(const ObjTypeInfo* outer, int base, const ObjTypeInfo* own) {
    std::vector<ObjTypeInfo*> all;
    all.reserve(static_cast<size_t>(base) + (own != nullptr ? own->args.size() : 0));
    for (int i = 0; i < base; i++) all.push_back(typeArgument(outer, i));
    if (own != nullptr) all.insert(all.end(), own->args.begin(), own->args.end());
    return typeOf(kTypeArguments, false, nullptr, all);
}

void VM::callClosure(ObjClosure* closure, int argc, bool packed) {
    if (closure->proto->chooser) closure = chooseForm(static_cast<const ObjForms*>(closure), sp_ - argc, argc, nullptr, 0);
    const FunctionProto* proto = closure->proto;
    if (proto->varargIndex >= 0 && !packed) argc = packVarargs(proto, argc);
    if (argc != proto->arity && (argc > proto->arity || argc < proto->requiredArgs()))
        checkArity(proto->name, proto->requiredArgs(), proto->arity, argc);

    Value* base = sp_ - argc - 1;
    if ((frameCount_ == frameLimit_ || stackEnd_ - base < proto->maxStack) && !growContext(base + proto->maxStack, frameCount_ + 1))
        throw runtimeError(Thrown::StackOverflowError, "stack overflow");

    if (pendingEnums_ > 0) {
        // A bare name may only arrive at a parameter that could make an enum entry of it.
        for (int i = 0; i < argc; i++)
            if (base[i + 1].kind == ValueKind::EnumName && (i >= 64 || ((proto->classParams >> i) & 1) == 0))
                throw unsettledName(base[i + 1]);
    }

    // Arguments that were left out are marked, and the function fills in their defaults.
    for (int missing = argc; missing < proto->arity; missing++) *sp_++ = Value::undefined();

    // A generic function declared inside another (G1) has those of where it was made and then
    // its own, made before the frame is, as making them may collect.
    const ObjTypeInfo* joined = (proto->typeArgs & kTypeArgsOwn) != 0 && proto->typeArgBase != 0
                                    ? joinTypeArgs(closure->typeArgs, proto->typeArgBase, pendingTypeArgs_)
                                    : nullptr;

    CallFrame& frame = frames_[static_cast<size_t>(frameCount_++)];
    frame.dropResult = 0;
    frame.repeats = 0;
    frame.closure = closure;
    // A generic function takes those WithTypes gave; a lambda has those of where it was made.
    // Only a function that reads them keeps them, which the others are not slowed by.
    if (proto->typeArgs != 0) {
        frameTypeArgs(frameCount_ - 1) = joined != nullptr ? joined
                                         : pendingTypeArgs_ != nullptr && (proto->typeArgs & kTypeArgsOwn) != 0 ? pendingTypeArgs_
                                                                                                                 : closure->typeArgs;
        pendingTypeArgs_ = nullptr;
        if ((proto->typeArgs & kTakesDestination) != 0) frameDestination(frameCount_ - 1) = pendingDestination_;
    }
    pendingDestination_ = Value::null();
    frame.ip = proto->run.data();
    frame.base = base;
}

// A call whose last argument is a lambda written after the call, as in
// `items.joinToString(", ") { it.name }`. The lambda belongs to the last parameter, as in
// Kotlin, so the parameters between the other arguments and it are left to their defaults; a
// function that takes a lambda or not has a form for each (D56).
void VM::callTrailing(ObjClosure* closure, int argc) {
    if (closure->proto->chooser) {
        if (emptyName_ == nullptr) emptyName_ = intern("");
        const Value lambda[2] = {Value::object(emptyName_), sp_[-1]};
        closure = chooseForm(static_cast<const ObjForms*>(closure), sp_ - argc, argc - 1, lambda, 1);
    }
    const FunctionProto* proto = closure->proto;
    const int arity = proto->arity;
    const int vararg = proto->varargIndex;
    // Nothing to move: the lambda is already where the last parameter is, there are too many
    // arguments (reported as usual), or the last parameter gathers whatever is left.
    if (vararg == arity - 1 || (vararg < 0 && argc >= arity)) {
        callClosure(closure, argc);
        return;
    }
    if (!roomFor(sp_ + arity + 1)) throw runtimeError(Thrown::StackOverflowError, "stack overflow");

    int given = argc - 1;
    const int target = arity - 1;
    if (given < proto->requiredArgs()) {
        // The lambda may pass over parameters that have a default, and no others.
        if (proto->paramNames.size() != static_cast<size_t>(arity)) checkArity(proto->name, proto->requiredArgs(), arity, argc);
        throw runtimeError(Thrown::IllegalArgumentException, proto->name + "() is missing argument '" + proto->paramNames[static_cast<size_t>(given)]->utf8() + "'");
    }
    if (target == given && vararg < 0) {
        callClosure(closure, argc);  // already in its place; what follows takes its default
        return;
    }

    TempRoot lambda(*this, sp_[-1]);
    sp_--;
    bool packed = false;
    if (vararg >= 0) {
        for (; given < vararg; given++) *sp_++ = Value::undefined();
        const int extra = given - vararg;
        if (pendingEnums_ > 0) rejectPendingNames(sp_ - extra, extra);
        ObjList* list = alloc<ObjList>();
        list->values().assign(sp_ - extra, sp_);
        account(static_cast<size_t>(extra) * sizeof(Value));
        sp_ -= extra;
        *sp_++ = Value::object(list);
        given = vararg + 1;
        packed = true;
    }
    for (; given < target; given++) *sp_++ = Value::undefined();
    *sp_++ = lambda.get();
    callClosure(closure, arity, packed);
}

// callValue for such a call. Only a function written in WinterWolf has parameters to look at; to
// anything else the lambda is simply the last argument.
bool VM::callValueTrailing(Value callee, int argc) {
    if (callee.isObjType(ObjType::Closure)) {
        callTrailing(asClosure(callee), argc);
        return true;
    }
    if (callee.isObjType(ObjType::BoundMethod)) {
        ObjBoundMethod* bound = asBoundMethod(callee);
        sp_[-argc - 1] = bound->receiver;
        callTrailing(bound->method, argc);
        return true;
    }
    return callValue(callee, argc);
}

// Calls the value sitting below `argc` arguments on the stack. Returns true if a new frame
// was pushed (the caller must then keep interpreting), false if the call already completed
// and left its result on the stack.
bool VM::callValue(Value callee, int argc, bool packed) {
    if (callee.isObj()) {
        switch (callee.as.obj->type) {
            case ObjType::Closure:
                callClosure(asClosure(callee), argc, packed);
                return true;

            case ObjType::Native: {
                ObjNative* native = asNative(callee);
                checkArity(native->name, native->minArgs, native->maxArgs, argc);
                if (pendingEnums_ > 0) rejectPendingNames(sp_ - argc, argc);
                if (coreId_ != 0) {
                    if (!native->parallelSafe)
                        throw runtimeError(Thrown::IllegalStateException, native->name + "() cannot be used in a parallel section: nothing waits there, and what the " +
                                           "whole program has one of is left alone");
                    guardNative(sp_ - argc, argc);
                }
                nativeBase_ = sp_ - argc - 1;
                pendingTypeArgs_ = nullptr;
                boxAll(sp_ - argc, sp_);  // the C++ is given boxes, never a place in a list
                pendingDestination_ = Value::null();
                Value result = native->fn(*this, argc, sp_ - argc);
                if (result.kind == ValueKind::Undefined) return true;  // it set a call going in its place (tailCallOn)
                sp_ -= argc + 1;
                *sp_++ = result;
                return false;
            }

            case ObjType::Class:
                return construct(asClass(callee), argc, packed);

            case ObjType::BoundMethod: {
                ObjBoundMethod* bound = asBoundMethod(callee);
                sp_[-argc - 1] = bound->receiver;
                callClosure(bound->method, argc, packed);
                return true;
            }

            case ObjType::BoundNative: {
                ObjBoundNative* bound = asBoundNative(callee);
                const NativeMethod* method = bound->method;
                checkArity(method->name, method->minArgs, method->maxArgs, argc);
                if (pendingEnums_ > 0) rejectPendingNames(sp_ - argc, argc);
                // What changes its receiver: not one that is another core's, or frozen (D26).
                if (method->mutates && bound->receiver.isObj()) guardWrite(bound->receiver.as.obj);
                if (coreId_ != 0) {
                    guardRead(bound->receiver);
                    guardBand(bound->receiver, method->name);
                    guardNative(sp_ - argc, argc);
                }
                nativeBase_ = sp_ - argc - 1;
                pendingTypeArgs_ = nullptr;
                if (!method->takesPlaces) boxAll(sp_ - argc, sp_);
                pendingDestination_ = Value::null();  // the C++ makes its result where it likes, and calls nothing with this
                Value result = method->fn(*this, bound->receiver, argc, sp_ - argc);
                sp_ -= argc + 1;
                *sp_++ = result;
                return false;
            }

            default:
                break;
        }
    }
    throw runtimeError(Thrown::UnsupportedOperationException, "cannot call a value of type " + typeName(callee));
}

// A call with arguments passed by name. The stack holds the callee, the positional
// arguments, then a (name, value) pair for each named one. They are rearranged into the
// callee's parameter order, leaving the "not passed" mark for parameters that have a default,
// and the call then proceeds as usual.
bool VM::callNamed(int positional, int named) {
    Value* first = sp_ - positional - 2 * named;
    Value callee = first[-1];

    // A lambda written after a call to a built-in function is simply its last argument.
    if (named == 1 && sp_[-2].isString() && asString(sp_[-2])->utf8().empty() &&
        (callee.isObjType(ObjType::Native) || callee.isObjType(ObjType::BoundNative))) {
        sp_[-2] = sp_[-1];
        sp_--;
        return callValue(callee, positional + 1);
    }

    // The function whose parameter names the arguments are matched against.
    const FunctionProto* proto = nullptr;
    auto display = [&]() -> std::string {
        return callee.isObjType(ObjType::Class) ? asClass(callee)->name->utf8() : proto->name;
    };
    if (callee.isObj()) {
        switch (callee.as.obj->type) {
            case ObjType::Closure:
                if (asClosure(callee)->proto->chooser) {
                    callee = Value::object(chooseForm(static_cast<const ObjForms*>(asClosure(callee)), first, positional,
                                                      first + positional, named));
                    first[-1] = callee;
                }
                proto = asClosure(callee)->proto;
                break;
            case ObjType::BoundMethod:
                if (asBoundMethod(callee)->method->proto->chooser) {
                    // The form is called on the object as a method is: the object in slot 0.
                    first[-1] = asBoundMethod(callee)->receiver;
                    callee = Value::object(chooseForm(static_cast<const ObjForms*>(asBoundMethod(callee)->method), first,
                                                      positional, first + positional, named));
                }
                proto = callee.isObjType(ObjType::Closure) ? asClosure(callee)->proto : asBoundMethod(callee)->method->proto;
                break;
            case ObjType::Class: {
                ObjClass* cls = asClass(callee);
                if (cls->init == nullptr) throw runtimeError(Thrown::IllegalArgumentException, cls->name->utf8() + "() takes no arguments");
                proto = cls->init->proto;
                break;
            }
            case ObjType::Native:
                if (!asNative(callee)->params.empty()) return callNativeNamed(positional, named);
                throw runtimeError(Thrown::UnsupportedOperationException, "built-in function " + asNative(callee)->name + "() does not take named arguments");
            case ObjType::BoundNative:
                if (!asBoundNative(callee)->method->params.empty()) return callNativeNamed(positional, named);
                throw runtimeError(Thrown::UnsupportedOperationException, "built-in method " + asBoundNative(callee)->method->name +
                                   "() does not take named arguments");
            default:
                break;
        }
    }
    if (proto == nullptr) throw runtimeError(Thrown::UnsupportedOperationException, "cannot call a value of type " + typeName(callee));

    int arity = proto->arity;
    if (proto->paramNames.size() != static_cast<size_t>(arity))
        throw runtimeError(Thrown::UnsupportedOperationException, display() + "() does not take named arguments");
    const int vararg = proto->varargIndex;
    if (positional > arity && vararg < 0) checkArity(display(), proto->requiredArgs(), arity, positional + named);

    // The common case of a lambda after the call: every parameter before the last was given,
    // so the lambda is simply the next argument.
    if (named == 1 && vararg < 0 && positional + 1 == arity && asString(sp_[-2])->utf8().empty()) {
        sp_[-2] = sp_[-1];
        sp_--;
        return callValue(callee, positional + 1);
    }

    // Scratch space kept between calls. It is emptied back onto the stack before the callee
    // runs, so a named call made from inside the callee can use it again.
    std::vector<Value>& arranged = namedScratch_;
    arranged.resize(static_cast<size_t>(arity));
    const int direct = vararg >= 0 && positional > vararg ? vararg : positional;
    for (int i = 0; i < arity; i++) arranged[static_cast<size_t>(i)] = i < direct ? first[i] : Value::undefined();
    if (vararg >= 0 && positional > vararg) {
        // The positional arguments from the vararg parameter on are its list.
        ObjList* list = alloc<ObjList>();
        list->values().assign(first + vararg, first + positional);
        account(static_cast<size_t>(positional - vararg) * sizeof(Value));
        arranged[static_cast<size_t>(vararg)] = Value::object(list);
    }

    const Value* pair = first + positional;
    for (int i = 0; i < named; i++, pair += 2) {
        if (!pair[0].isString()) throw runtimeError(Thrown::InternalError, "invalid call");
        const ObjString* name = asString(pair[0]);  // interned, like the parameter names

        int index = -1;
        if (name->utf8().empty()) {
            // The lambda written after the call: it is the last parameter (see callTrailing).
            if (arity == 0) throw runtimeError(Thrown::UnsupportedOperationException, display() + "() takes no arguments, so no lambda can follow the call");
            index = arity - 1;
            name = proto->paramNames[static_cast<size_t>(index)];
            // To a vararg parameter it is one more item, as the checker has it: compareBy { it.x }
            // (M8: it was refused as a value given by name).
            if (index == vararg) {
                Value& gathered = arranged[static_cast<size_t>(index)];
                if (gathered.kind == ValueKind::Undefined) {
                    ObjList* list = alloc<ObjList>();
                    list->values().push_back(pair[1]);
                    account(sizeof(Value));
                    gathered = Value::object(list);
                } else if (gathered.isObjType(ObjType::List)) {
                    asList(gathered)->values().push_back(pair[1]);
                }
                continue;
            }
        }
        for (int p = 0; index < 0 && p < arity; p++) {
            if (proto->paramNames[static_cast<size_t>(p)] == name) index = p;
        }
        if (index < 0) throw runtimeError(Thrown::IllegalArgumentException, display() + "() has no parameter named '" + name->utf8() + "'");
        if (arranged[static_cast<size_t>(index)].kind != ValueKind::Undefined)
            throw runtimeError(Thrown::IllegalArgumentException, "argument '" + name->utf8() + "' of " + display() + "() is given more than once");
        arranged[static_cast<size_t>(index)] = pair[1];
    }

    if (vararg >= 0) {
        Value& gathered = arranged[static_cast<size_t>(vararg)];
        if (gathered.kind == ValueKind::Undefined) {
            gathered = Value::object(alloc<ObjList>());
        } else if (!gathered.isObjType(ObjType::List)) {
            throw runtimeError(Thrown::IllegalArgumentException, "argument '" + proto->paramNames[static_cast<size_t>(vararg)]->utf8() + "' of " + display() +
                               "() is a vararg parameter: by name it takes a list, not a value of type " + typeName(gathered));
        }
    }

    // Only parameters with a default value may be left out.
    for (int i = 0; i < proto->requiredArgs(); i++)
        if (arranged[static_cast<size_t>(i)].kind == ValueKind::Undefined)
            throw runtimeError(Thrown::IllegalArgumentException, display() + "() is missing argument '" + proto->paramNames[static_cast<size_t>(i)]->utf8() + "'");

    if (!roomFor(first + arity)) throw runtimeError(Thrown::StackOverflowError, "stack overflow");
    for (int i = 0; i < arity; i++) first[i] = arranged[static_cast<size_t>(i)];
    sp_ = first + arity;
    return callValue(callee, arity, vararg >= 0);
}

// A built-in method called with arguments by name, as in text.contains("a", ignoreCase = true).
// Only the methods that say what their parameters are called take them. A parameter that is
// passed over on the way to a named one is given null, which such a method reads as left out.
bool VM::callNativeNamed(int positional, int named) {
    Value* first = sp_ - positional - 2 * named;
    Value callee = first[-1];
    // A built-in method of a value, or a built-in function: what it is called and takes.
    struct Native {
        const std::vector<std::string>& params;
        const std::string& name;
        int minArgs, maxArgs;
    };
    const Native native = callee.isObjType(ObjType::BoundNative)
                              ? Native{asBoundNative(callee)->method->params, asBoundNative(callee)->method->name,
                                       asBoundNative(callee)->method->minArgs, asBoundNative(callee)->method->maxArgs}
                              : Native{asNative(callee)->params, asNative(callee)->name, asNative(callee)->minArgs,
                                       asNative(callee)->maxArgs};
    const std::vector<std::string>* best = nullptr;
    if (callee.isObjType(ObjType::BoundNative) && asBoundNative(callee)->method->forms.size() > 1) {
        // Declared in several forms: the smallest that has a parameter for each name given.
        for (const std::vector<std::string>& form : asBoundNative(callee)->method->forms) {
            bool fits = static_cast<int>(form.size()) >= positional;
            const Value* pair = first + positional;
            for (int i = 0; fits && i < named; i++, pair += 2) {
                const std::string& name = pair[0].isString() ? asString(pair[0])->utf8() : std::string();
                fits = name.empty() ? !form.empty() : std::find(form.begin(), form.end(), name) != form.end();
            }
            if (fits && (best == nullptr || form.size() < best->size())) best = &form;
        }
    }
    const Native chosen{best != nullptr ? *best : native.params, native.name, native.minArgs, native.maxArgs};
    const Native* method = &chosen;
    const int count = static_cast<int>(method->params.size());
    if (positional > count) checkArity(method->name, method->minArgs, method->maxArgs, positional + named);

    std::vector<Value>& arranged = namedScratch_;
    arranged.assign(static_cast<size_t>(count), Value::undefined());
    for (int i = 0; i < positional; i++) arranged[static_cast<size_t>(i)] = first[i];
    int given = positional;
    const Value* pair = first + positional;
    for (int i = 0; i < named; i++, pair += 2) {
        if (!pair[0].isString()) throw runtimeError(Thrown::InternalError, "invalid call");
        const std::string& name = asString(pair[0])->utf8();
        int index = name.empty() ? count - 1 : -1;  // a lambda after the call is the last parameter
        for (int p = 0; index < 0 && p < count; p++)
            if (method->params[static_cast<size_t>(p)] == name) index = p;
        if (index < 0) throw runtimeError(Thrown::IllegalArgumentException, method->name + "() has no parameter named '" + name + "'");
        if (arranged[static_cast<size_t>(index)].kind != ValueKind::Undefined)
            throw runtimeError(Thrown::IllegalArgumentException, "argument '" + method->params[static_cast<size_t>(index)] + "' of " + method->name +
                               "() is given more than once");
        arranged[static_cast<size_t>(index)] = pair[1];
        if (index + 1 > given) given = index + 1;
    }

    if (!roomFor(first + given)) throw runtimeError(Thrown::StackOverflowError, "stack overflow");
    for (int i = 0; i < given; i++) {
        const Value& value = arranged[static_cast<size_t>(i)];
        first[i] = value.kind == ValueKind::Undefined ? Value::null() : value;
    }
    sp_ = first + given;
    return callValue(callee, given);
}

// A call whose positional arguments were gathered into a list because some were written
// `*list`. The stack holds the callee, that list, then a (name, value) pair for each named
// argument. The list is laid out as ordinary arguments and the call proceeds as usual.
bool VM::callSpread(int named) {
    Value* slot = sp_ - 2 * named - 1;
    ObjList* list = asList(*slot);
    const int count = static_cast<int>(list->size());
    if (!roomFor(sp_ + count + 1)) throw runtimeError(Thrown::StackOverflowError, "stack overflow");

    std::copy_backward(slot + 1, sp_, sp_ + count - 1);
    for (int i = 0; i < count; i++) slot[i] = listGet(list, static_cast<size_t>(i));
    sp_ += count - 1;
    if (named > 0) return callNamed(count, named);
    return callValue(sp_[-count - 1], count);
}

const NativeMethod* VM::findNativeMethod(Value receiver, const ObjString* name) const {
    BuiltinType type;
    if (receiver.isNumber()) {
        type = BuiltinType::Number;
    } else if (receiver.isChar()) {
        type = BuiltinType::Char;
    } else if (receiver.isByte()) {
        type = BuiltinType::Byte;
    } else if (receiver.isFromEnd()) {
        type = BuiltinType::Index;
    } else if (!receiver.isObj()) {
        return nullptr;
    } else {
        switch (receiver.as.obj->type) {
            case ObjType::String: type = BuiltinType::String; break;
            case ObjType::List: type = BuiltinType::List; break;
            case ObjType::Map: type = BuiltinType::Map; break;
            case ObjType::Set: type = BuiltinType::Set; break;
            case ObjType::Range: type = BuiltinType::Range; break;
            case ObjType::Bytes: type = BuiltinType::Bytes; break;
            default: return nullptr;
        }
    }
    return cachedNative(type, name);
}

// Calls method `name` on the receiver sitting below `argc` arguments. Same result as callValue.
bool VM::invoke(ObjString* name, int argc, bool trailing) {
    Value receiver = sp_[-argc - 1];
    if (receiver.isSmall()) {
        // A method run on a value kept in the value itself (M6.6 part 5): `this` is the value.
        ObjClass* cls = smallClassOf(receiver);
        if (const Value* method = cls->methods.find(name)) {
            if (trailing) callTrailing(asClosure(*method), argc);
            else callClosure(asClosure(*method), argc);
            return true;
        }
        receiver = sp_[-argc - 1] = boxSmall(receiver);  // a field, a property or an extension: on a box
    }
    if (receiver.isRef()) {
        // A method run on a value where it is kept (M6.6 part 4): `this` is the place.
        ObjClass* cls = refClass(receiver);
        if (const Value* method = cls->methods.find(name)) {
            if (trailing) callTrailing(asClosure(*method), argc);
            else callClosure(asClosure(*method), argc);
            return true;
        }
        Value field;
        if (refField(receiver, name, field)) {
            sp_[-argc - 1] = field;
            return trailing ? callValueTrailing(field, argc) : callValue(field, argc);
        }
        if (const Value* getter = cls->getters.find(name)) {
            Value callee = callWithReceiver(receiver, asClosure(*getter));
            sp_[-argc - 1] = callee;
            return trailing ? callValueTrailing(callee, argc) : callValue(callee, argc);
        }
        receiver = sp_[-argc - 1] = ownRef(receiver);  // an extension or the like: on a copy
    }
    guardRead(receiver);

    if (receiver.isObj()) {
        if (receiver.as.obj->type == ObjType::Instance) {
            ObjInstance* instance = asInstance(receiver);
            // A call goes to the method of that name. A field may share the name: reading it
            // gives the field, calling it runs the method.
            if (const Value* method = instance->cls->methods.find(name)) {
                if (trailing) callTrailing(asClosure(*method), argc);
                else callClosure(asClosure(*method), argc);
                return true;
            }
            if (Value field; instance->read(name, field)) {
                sp_[-argc - 1] = field;
                return trailing ? callValueTrailing(field, argc) : callValue(field, argc);
            }
            if (instance->cls->hasAccessors) {
                // obj.property(args): call whatever the getter of the property returns.
                if (const Value* getter = instance->cls->getters.find(name)) {
                    Value callee = callWithReceiver(receiver, asClosure(*getter));
                    sp_[-argc - 1] = callee;
                    return trailing ? callValueTrailing(callee, argc) : callValue(callee, argc);
                }
            }
            // What an extend brought to the class (D33).
            if (const Value* method = instance->cls->extendMethod(name)) {
                if (trailing) callTrailing(asClosure(*method), argc);
                else callClosure(asClosure(*method), argc);
                return true;
            }
            if (const Value* getter = instance->cls->extendGetter(name)) {
                Value callee = callWithReceiver(receiver, asClosure(*getter));
                sp_[-argc - 1] = callee;
                return trailing ? callValueTrailing(callee, argc) : callValue(callee, argc);
            }
            if (const Value* init = instance->cls->extendField(name)) {
                // A function kept in a field an extend adds (D157), called.
                Value callee = extraField(instance, name, *init);
                sp_[-argc - 1] = callee;
                return trailing ? callValueTrailing(callee, argc) : callValue(callee, argc);
            }
            if (const Value* extension = findExtension(receiver, name)) {
                if (trailing) callTrailing(asClosure(*extension), argc);
                else callClosure(asClosure(*extension), argc);
                return true;
            }
            if (const int done = scopeFunction(name, argc)) return done == 2;
            if (Value inner; nestedThrough(instance, name, inner)) {
                // `Store.Box(1)`: a class declared inside the object's, made (o-m8-nested).
                sp_[-argc - 1] = inner;
                return trailing ? callValueTrailing(inner, argc) : callValue(inner, argc);
            }
            if (ObjString* other = missedName(instance->cls, name, receiver, false)) {  // o-private-shadow
                return invoke(other, argc, trailing);
            }
            throw runtimeError(Thrown::UnsupportedOperationException, "undefined method '" + name->utf8() + "' on " + instance->cls->name->utf8());
        }

        if (receiver.as.obj->type == ObjType::Value) {
            // A method of a value's type (M6.6 part 2), run on the value; or what a field or a
            // property holds, called.
            ObjValue* box = asValue(receiver);
            if (const Value* method = box->cls->methods.find(name)) {
                if (trailing) callTrailing(asClosure(*method), argc);
                else callClosure(asClosure(*method), argc);
                return true;
            }
            if (const int64_t at = valueFieldIndex(box, name); at >= 0) {
                const Value field = valueField(box, static_cast<size_t>(at));
                sp_[-argc - 1] = field;
                return trailing ? callValueTrailing(field, argc) : callValue(field, argc);
            }
            if (const Value* getter = box->cls->getters.find(name)) {
                Value callee = callWithReceiver(receiver, asClosure(*getter));
                sp_[-argc - 1] = callee;
                return trailing ? callValueTrailing(callee, argc) : callValue(callee, argc);
            }
            // What an extend brought the value's type (D33, D186).
            if (const Value* method = box->cls->extendMethod(name)) {
                if (trailing) callTrailing(asClosure(*method), argc);
                else callClosure(asClosure(*method), argc);
                return true;
            }
            if (const Value* getter = box->cls->extendGetter(name)) {
                Value callee = callWithReceiver(receiver, asClosure(*getter));
                sp_[-argc - 1] = callee;
                return trailing ? callValueTrailing(callee, argc) : callValue(callee, argc);
            }
            if (const Value* extension = findExtension(receiver, name)) {
                if (trailing) callTrailing(asClosure(*extension), argc);
                else callClosure(asClosure(*extension), argc);
                return true;
            }
            if (const int done = scopeFunction(name, argc)) return done == 2;
            throw runtimeError(Thrown::UnsupportedOperationException, "undefined method '" + name->utf8() + "' on " + box->cls->name->utf8());
        }

        if (receiver.as.obj->type == ObjType::Class) {
            ObjClass* cls = asClass(receiver);
            if (cls->staticInit != nullptr || (cls->super != nullptr && cls->super->staticInit != nullptr)) ensureClassInit(cls);
            if (const Value* member = cls->findStatic(name)) {
                sp_[-argc - 1] = *member;
                return trailing ? callValueTrailing(*member, argc) : callValue(*member, argc);
            }
            throw runtimeError(Thrown::UnsupportedOperationException, "undefined static method '" + name->utf8() + "' on class " + cls->name->utf8());
        }
    }

    const NativeMethod* method = findNativeMethod(receiver, name);
    // An object of a class as the first argument: the standard library's form in WinterWolf beside
    // the built-in one ("text".replace(regex, ...), M8), as the built-in methods take built-in
    // values only.
    if (method != nullptr && argc >= 1 && sp_[-argc].isObjType(ObjType::Instance)) {
        if (const Value* extension = findExtension(receiver, name)) {
            if (trailing) callTrailing(asClosure(*extension), argc);
            else callClosure(asClosure(*extension), argc);
            return true;
        }
    }
    if (method == nullptr) {
        if (const Value* extension = findExtension(receiver, name)) {
            // An int's own forms not taking the argument, Number's may: `2 * tensor` (op
            // Number.*) beside op int.* for BigInt, called so by the compiler (2026-11-19).
            if (argc == 1 && !trailing && receiver.isNumber())
                if (const Value* added = addedOperator(receiver, &sp_[-1], name)) extension = added;
            if (trailing) callTrailing(asClosure(*extension), argc);
            else callClosure(asClosure(*extension), argc);
            return true;
        }
        // What an extend brought the built-in type (G30): the interface's method, or what a
        // getter gives, called.
        if (const Value* fitted = builtinFitMethod(receiver, name)) {
            if (trailing) callTrailing(asClosure(*fitted), argc);
            else callClosure(asClosure(*fitted), argc);
            return true;
        }
        if (const Value* getter = builtinFitGetter(receiver, name)) {
            Value callee = callWithReceiver(receiver, asClosure(*getter));
            sp_[-argc - 1] = callee;
            return trailing ? callValueTrailing(callee, argc) : callValue(callee, argc);
        }
        if (const int done = scopeFunction(name, argc)) return done == 2;
        throw runtimeError(Thrown::UnsupportedOperationException, "undefined method '" + name->utf8() + "' on a value of type " + typeName(receiver));
    }
    if (receiver.isObjType(ObjType::Range) && asRange(receiver)->fromEnd() && method->name != "of")
        throw rangeFromEnd(asRange(receiver), method->name + "()");
    checkArity(method->name, method->minArgs, method->maxArgs, argc);
    if (pendingEnums_ > 0) rejectPendingNames(sp_ - argc, argc);
    if (method->mutates && receiver.isObj()) guardWrite(receiver.as.obj);  // nor frozen (D26)
    if (coreId_ != 0) {
        guardBand(receiver, method->name);
        guardNative(sp_ - argc, argc);
    }
    nativeBase_ = sp_ - argc - 1;
    pendingTypeArgs_ = nullptr;
    if (!method->takesPlaces) boxAll(sp_ - argc, sp_);
    receiver = boxed(receiver);
    pendingDestination_ = Value::null();  // the C++ makes its result where it likes, and calls nothing with this
    Value result = method->fn(*this, receiver, argc, sp_ - argc);
    sp_ -= argc + 1;
    *sp_++ = result;
    return false;
}

Value VM::tailCallOn(Value receiver, Value function) {
    if (!function.isObjType(ObjType::Closure)) throw runtimeError(Thrown::ClassCastException, "expected a function written in WinterWolf, got a value of type " + typeName(function));
    // The built-in's own call goes: the receiver takes the place it was called at, and the
    // function is called there with nothing else.
    TempRoot keep(*this, function);
    sp_ = nativeBase_;
    *sp_++ = receiver;
    callClosure(asClosure(function), 0);
    return Value::undefined();
}

// x.let { }, x.also { }, x.apply { } and x.run { } on any value that has no method of its own
// by that name. `let` and `also` hand the value to the block as its argument; `apply` and
// `run` run the block on the value. `also` and `apply` give the value back, the others the
// block's result. (Written with a lambda they are the standard library's inline functions, copied in
// by the compiler, and never get here, D59.)
//
// The block is called as part of the program, not from inside this function, so that a task
// can be set aside in it. Returns 0 if `name` is not one of these, 1 if the result is on the
// stack, and 2 if a call has been set going.
int VM::scopeFunction(ObjString* name, int argc) {
    if (argc != 1) return 0;
    const std::string& n = name->utf8();
    const bool onReceiver = n == "apply" || n == "run";
    if (!onReceiver && n != "let" && n != "also") return 0;

    const Value receiver = sp_[-2], block = sp_[-1];
    if (n == "let") {
        // block(receiver), in the place of the two.
        sp_[-2] = block;
        sp_[-1] = receiver;
        return callValue(block, 1) ? 2 : 1;
    }
    if (!block.isObjType(ObjType::Closure)) {
        // Something that is not a function written in WinterWolf is simply called.
        Value result = n == "also" ? call(block, 1, &receiver) : call(block, 0, nullptr);
        if (n != "run") result = receiver;
        sp_ -= 2;
        *sp_++ = result;
        return 1;
    }
    if (n == "run") {
        // The block runs on the receiver, and its result takes the receiver's place.
        sp_--;
        callClosure(asClosure(block), 0);
        return 2;
    }
    if (n == "apply") {
        // The same, on a second copy of the receiver, and the block's result is dropped.
        sp_[-1] = receiver;
        callClosure(asClosure(block), 0);
    } else {
        // also: block(receiver), and the result is dropped.
        if (!roomFor(sp_ + 1)) throw runtimeError(Thrown::StackOverflowError, "stack overflow");
        *sp_++ = receiver;
        callClosure(asClosure(block), 1);
    }
    frames_[static_cast<size_t>(frameCount_ - 1)].dropResult |= kDropResult;
    return 2;
}

// What == means: numbers and strings by value; lists, maps and ranges by what they hold,
// compared the same way all the way down; an object by its own `==` operator if its class
// has one, and otherwise only to itself. A structure nested deeper than a comparison can
// follow (one that contains itself, for instance) is equal only to itself from there on.
bool VM::equals(Value a, Value b, int depth) {
    if (a.isSmall() && b.isSmall() && a.refIndex == b.refIndex && smallClassOf(a)->methods.find(names.opEq) == nullptr)
        return valuesEqual(a, b);  // field by field, with no box (M6.6 part 5)
    if (a.isRef() || b.isRef() || a.isSmall() || b.isSmall()) {
        TempRoot keepA(*this, a = boxed(a));
        TempRoot keepB(*this, b = boxed(b));
        return equals(a, b, depth);
    }
    // A number, a string or another plain value against an object whose class says what ==
    // means: the object's == decides, either way round (2026-11-19: `5 == BigInt.of(5)` as
    // `BigInt.of(5) == 5`; a plain value has no == of its own that knows the object).
    if (!a.isObj() && b.isObj() &&
        ((b.isObjType(ObjType::Instance) && asInstance(b)->cls->methods.find(names.opEq) != nullptr) ||
         (b.isObjType(ObjType::Value) && asValue(b)->cls->methods.find(names.opEq) != nullptr)))
        return equals(b, a, depth);
    if (a.isObj() && !b.isObj() &&
        ((a.isObjType(ObjType::Instance) && asInstance(a)->cls->methods.find(names.opEq) != nullptr) ||
         (a.isObjType(ObjType::Value) && asValue(a)->cls->methods.find(names.opEq) != nullptr))) {
        const Value* method = a.isObjType(ObjType::Instance) ? asInstance(a)->cls->methods.find(names.opEq) : asValue(a)->cls->methods.find(names.opEq);
        TempRoot bound(*this, Value::object(alloc<ObjBoundMethod>(a, asClosure(*method))));
        return call(bound.get(), 1, &b).truthy();
    }
    if (!a.isObj() || !b.isObj()) return valuesEqual(a, b);
    if (a.as.obj == b.as.obj) return true;

    // In a parallel section another core's item is not looked into. Telling two objects apart
    // by which they are needs no looking, so only the cases below that do look ask.
    if (a.isObjType(ObjType::Value)) {
        // Two values of one value type are equal when their fields are (M6.6 part 2), unless
        // the type says what == means itself.
        if (const Value* method = asValue(a)->cls->methods.find(names.opEq)) {
            TempRoot bound(*this, Value::object(alloc<ObjBoundMethod>(a, asClosure(*method))));
            return call(bound.get(), 1, &b).truthy();
        }
        if (!b.isObjType(ObjType::Value) || depth >= kMaxEqualsDepth) return false;
        return valuesSame(asValue(a), asValue(b), depth);
    }
    if (a.isObjType(ObjType::Instance)) {
        const Value* method = asInstance(a)->cls->methods.find(names.opEq);
        if (method == nullptr) {
            // Two objects of one data class are equal when their header fields are.
            ObjInstance* x = asInstance(a);
            if (!x->cls->isData || !b.isObjType(ObjType::Instance) || asInstance(b)->cls != x->cls) return false;
            if (depth >= kMaxEqualsDepth) return false;
            guardRead(a.as.obj);
            guardRead(b.as.obj);
            ObjInstance* y = asInstance(b);
            for (ObjString* part : x->cls->components) {
                Value left = Value::null(), right = Value::null();
                x->read(part, left);
                y->read(part, right);
                if (!equals(left, right, depth + 1)) return false;
            }
            return true;
        }
        TempRoot bound(*this, Value::object(alloc<ObjBoundMethod>(a, asClosure(*method))));
        return call(bound.get(), 1, &b).truthy();
    }
    if (a.as.obj->type != b.as.obj->type) return false;
    if (depth >= kMaxEqualsDepth) return false;
    guardRead(a.as.obj);
    guardRead(b.as.obj);

    switch (a.as.obj->type) {
        case ObjType::Bytes:
            return asBytes(a)->data == asBytes(b)->data;
        case ObjType::List: {
            ObjList* x = asList(a);
            ObjList* y = asList(b);
            if (x->size() != y->size()) return false;
            const bool sameNarrow = x->store() == ListStore::Narrow && x->pack->narrow == y->pack->narrow && x->pack->narrow != Narrow::Float32;
            if (x->packed() && y->packed() && x->store() == y->store() && (x->store() == ListStore::Ints || x->store() == ListStore::Bools || sameNarrow) &&
                !x->pack->nullable && !y->pack->nullable)
                return x->pack->block == y->pack->block;  // ints and bools, equal where their bytes are
            // An element's own == may change either list, so the sizes are looked at again.
            for (size_t i = 0; i < x->size() && i < y->size(); i++) {
                const Value left = listGet(x, i);
                TempRoot keep(*this, left);
                const Value right = listGet(y, i);
                TempRoot keepToo(*this, right);
                if (!equals(left, right, depth + 1)) return false;
            }
            return x->size() == y->size();
        }
        case ObjType::Map: {
            const ValueMap& x = asMap(a)->map;
            const ValueMap& y = asMap(b)->map;
            if (x.size() != y.size()) return false;
            for (size_t i = 0; i < x.entries().size(); i++) {
                if (!x.entries()[i].live) continue;
                const Value key = x.entries()[i].key, value = x.entries()[i].value;
                const Value* other = y.find(key);
                if (other == nullptr) return false;
                const Value otherValue = *other;
                if (!equals(value, otherValue, depth + 1)) return false;
            }
            return true;
        }
        case ObjType::Range: {
            const ObjRange* x = asRange(a);
            const ObjRange* y = asRange(b);
            return x->start == y->start && x->end == y->end && x->step == y->step && x->inclusive == y->inclusive &&
                   x->startFromEnd == y->startFromEnd && x->endFromEnd == y->endFromEnd;
        }
        case ObjType::Set: {
            const ObjSet* x = asSet(a);
            const ObjSet* y = asSet(b);
            if (x->values != y->values) return false;
            if (x->values) {
                // A map's values (G22): the same values in the same order, as for lists.
                const std::vector<Value> xs = setItems(x);
                TempVector keepX(*this, const_cast<std::vector<Value>&>(xs));
                const std::vector<Value> ys = setItems(y);
                TempVector keepY(*this, const_cast<std::vector<Value>&>(ys));
                if (xs.size() != ys.size()) return false;
                for (size_t i = 0; i < xs.size(); i++)
                    if (!equals(xs[i], ys[i])) return false;
                return true;
            }
            // The same items, whatever their order.
            if (setSize(x) != setSize(y)) return false;
            const std::vector<Value> items = setItems(x);
            TempVector keep(*this, const_cast<std::vector<Value>&>(items));
            for (const Value& item : items)
                if (!setContains(y, item)) return false;
            return true;
        }
        default:
            return valuesEqual(a, b);
    }
}

// Calls an overloaded operator; the receiver and its operands are already on the stack in
// call layout, so this always pushes a frame.
void VM::invokeOperator(Value receiver, ObjString* name, int argc) {
    const ObjClass* cls = classOf(receiver);
    const Value* method = cls->methods.find(name);
    if (method == nullptr) {
        const std::string symbol = name == names.opNeg ? "-" : name == names.opBitNot ? "~" : name->utf8();
        throw runtimeError(Thrown::UnsupportedOperationException, cls->name->utf8() + " does not define operator '" + symbol + "'");
    }
    callClosure(asClosure(*method), argc);
}

// ---- Properties and indexing ----

Value VM::getProperty(Value obj, ObjString* name) {
    if (obj.isSmall()) {
        // A field of a value kept in the value itself (M6.6 part 5); anything else, on a box.
        const ObjClass* cls = smallClassOf(obj);
        if (const Value* at = cls->declaredIndex.find(name)) {
            const ValueField& field = cls->layout.load(std::memory_order_relaxed)->fields[static_cast<size_t>(at->as.i)];
            return readValueField(smallBytes(obj) + field.offset, field);
        }
        obj = boxSmall(obj);
    }
    if (obj.isRef()) {
        Value read;
        if (refField(obj, name, read)) return read;
        obj = ownRef(obj);  // a method or property of it: on a copy that may be kept
    }
    if (obj.isObj()) {
        guardRead(obj.as.obj);
        switch (obj.as.obj->type) {
            case ObjType::Instance: {
                ObjInstance* instance = asInstance(obj);
                if (Value field; instance->read(name, field)) return fieldRead(field);
                if (const Value* method = instance->cls->methods.find(name))
                    return Value::object(alloc<ObjBoundMethod>(obj, asClosure(*method)));
                if (instance->cls->setters.find(name) != nullptr)
                    throw runtimeError(Thrown::IllegalStateException, "property '" + name->utf8() + "' of " + instance->cls->name->utf8() +
                                       " cannot be read: it has no getter");
                if (const Value* extension = findExtension(obj, name))
                    return Value::object(alloc<ObjBoundMethod>(obj, asClosure(*extension)));
                if (const Value* getter = instance->cls->extendGetter(name)) return callWithReceiver(obj, asClosure(*getter));  // D33
                if (const Value* method = instance->cls->extendMethod(name))
                    return Value::object(alloc<ObjBoundMethod>(obj, asClosure(*method)));
                if (const Value* init = instance->cls->extendField(name)) return extraField(instance, name, *init);  // D157
                if (const FieldDecl* decl = instance->declOf(name)) {
                    if (decl->payload == FieldDecl::NoPayload && instance->fields()[decl->slot].isLazy())
                        // An object shared in a parallel section keeps its marker: another core may
                        // read the field as it is written.
                        return forceLazy(instance->fields()[decl->slot],
                                         instance->core == coreId_ ? std::function<void(Value)>([&](Value v) { instance->fields()[decl->slot] = v; })
                                                                   : std::function<void(Value)>(),
                                         [&] { return "'" + name->utf8() + "' of " + instance->cls->name->utf8(); });
                    throw runtimeError(Thrown::IllegalStateException, "'" + name->utf8() + "' of " + instance->cls->name->utf8() + " is read before it is given a value " +
                                       "(a lateinit field is read only once it has one)");
                }
                if (Value inner; nestedThrough(instance, name, inner)) return inner;  // `Store.Inner` (o-m8-nested)
                if (ObjString* other = missedName(instance->cls, name, obj, false)) {  // o-private-shadow
                    return getProperty(obj, other);
                }
                throw runtimeError(Thrown::UnsupportedOperationException, "undefined property '" + name->utf8() + "' on " + instance->cls->name->utf8());
            }
            case ObjType::Value: {
                ObjValue* box = asValue(obj);
                if (const int64_t at = valueFieldIndex(box, name); at >= 0) return valueField(box, static_cast<size_t>(at));
                if (const Value* method = box->cls->methods.find(name))
                    return Value::object(alloc<ObjBoundMethod>(obj, asClosure(*method)));
                if (const Value* getter = box->cls->getters.find(name)) return callWithReceiver(obj, asClosure(*getter));
                if (const Value* extension = findExtension(obj, name))
                    return Value::object(alloc<ObjBoundMethod>(obj, asClosure(*extension)));
                if (const Value* getter = box->cls->extendGetter(name)) return callWithReceiver(obj, asClosure(*getter));  // D33
                if (const Value* method = box->cls->extendMethod(name))
                    return Value::object(alloc<ObjBoundMethod>(obj, asClosure(*method)));
                throw runtimeError(Thrown::UnsupportedOperationException, "undefined property '" + name->utf8() + "' on " + box->cls->name->utf8());
            }
            case ObjType::Class: {
                ObjClass* cls = asClass(obj);
                if (cls->staticInit != nullptr || (cls->super != nullptr && cls->super->staticInit != nullptr)) ensureClassInit(cls);
                if (const Value* member = cls->findStatic(name)) {
                    if (!member->isLazy()) return *member;
                    for (ObjClass* k = cls; k != nullptr; k = k->super)
                        if (k->statics.find(name) != nullptr)
                            // In a parallel section the class, every core's, keeps its marker.
                            return forceLazy(*member, coreId_ == 0 ? std::function<void(Value)>([&](Value v) { k->statics.set(name, v); }) : std::function<void(Value)>(),
                                             [&] { return cls->name->utf8() + "." + name->utf8(); });
                }
                for (const ObjClass* k = cls; k != nullptr; k = k->super) {
                    if (const Value* getter = k->staticGetters.find(name)) return call(*getter, 0, nullptr);
                    if (k->staticSetters.find(name) != nullptr)
                        throw runtimeError(Thrown::IllegalStateException, "static property '" + name->utf8() + "' of " + k->name->utf8() +
                                           " cannot be read: it has no getter");
                }
                throw runtimeError(Thrown::UnsupportedOperationException, "undefined static member '" + name->utf8() + "' on class " + cls->name->utf8());
            }
            case ObjType::String:
                if (name == names.size) return Value::integer(static_cast<int64_t>(asString(obj)->length));
                if (name == names.indices)
                    return Value::object(alloc<ObjRange>(int64_t{0}, static_cast<int64_t>(asString(obj)->length), int64_t{1}, false));
                if (name == names.lastIndex) return Value::integer(static_cast<int64_t>(asString(obj)->length) - 1);
                break;
            case ObjType::List:
                if (name == names.size) return Value::integer(static_cast<int64_t>(asList(obj)->size()));
                if (name == names.indices)
                    return Value::object(alloc<ObjRange>(int64_t{0}, static_cast<int64_t>(asList(obj)->size()), int64_t{1}, false));
                if (name == names.lastIndex) return Value::integer(static_cast<int64_t>(asList(obj)->size()) - 1);
                if (name == names.itemType) return classValue(typeArgument(asList(obj)->type, 0));  // M6.6 part 1, D162
                break;
            case ObjType::Map:
                if (name == names.size) return Value::integer(static_cast<int64_t>(asMap(obj)->map.size()));
                if (name == names.keyType) return classValue(typeArgument(asMap(obj)->type, 0));
                if (name == names.valueType) return classValue(typeArgument(asMap(obj)->type, 1));
                if (name == names.keys) return mapView(asMap(obj), false);  // live views (D77)
                if (name == names.values) return mapValues(asMap(obj));  // a live view (G22)
                if (name == names.entries) return mapView(asMap(obj), true);
                break;
            case ObjType::Set:
                if (name == names.size) return Value::integer(static_cast<int64_t>(setSize(asSet(obj))));
                if (name == names.itemType) return classValue(typeArgument(asSet(obj)->type, 0));
                break;
            case ObjType::Range: {
                ObjRange* range = asRange(obj);
                if (name == names.start) return range->startFromEnd ? Value::fromEnd(range->start) : Value::integer(range->start);
                if (name == names.end) return range->endFromEnd ? Value::fromEnd(range->end) : Value::integer(range->end);
                if (range->fromEnd()) throw rangeFromEnd(range, name->utf8());
                if (name == names.step) return Value::integer(range->step);
                if (name == names.size) return Value::integer(range->count());
                // The numbers it starts and finishes on, when it holds any.
                if (name == names.first && range->count() > 0) return Value::integer(range->start);
                if (name == names.last && range->count() > 0)
                    return Value::integer(range->start + (range->count() - 1) * range->step);
                break;
            }
            case ObjType::Bytes:
                if (name == names.size) return Value::integer(static_cast<int64_t>(asBytes(obj)->data.size()));
                break;
            default:
                break;
        }
    }

    if (obj.isChar() && name == names.code) return Value::integer(obj.as.i);  // the character's number (D24)
    // An Index (D146): ^k, or an int from the start.
    if ((obj.isFromEnd() || obj.isInt()) && name == names.fromEnd) return Value::boolean(obj.isFromEnd());
    if ((obj.isFromEnd() || obj.isInt()) && name == names.value) return Value::integer(obj.as.i);
    if (obj.isInt() && name == names.character) {
        // The character numbered n (D142).
        if (!isCharacter(obj.as.i))
            throw runtimeError(Thrown::IllegalArgumentException, std::to_string(obj.as.i) + " is not a character's number: characters are numbered 0 to 1114111, leaving out 55296 to 57343");
        return Value::character(static_cast<uint32_t>(obj.as.i));
    }
    if (obj.isInt()) {
        if (name == names.absoluteValue) return Value::integer(obj.as.i < 0 ? -obj.as.i : obj.as.i);
        if (name == names.sign) return Value::integer((obj.as.i > 0) - (obj.as.i < 0));
    } else if (obj.isFloat()) {
        if (name == names.absoluteValue) return Value::number(std::fabs(obj.as.d));
        if (name == names.sign) return Value::number(obj.as.d > 0 ? 1.0 : obj.as.d < 0 ? -1.0 : obj.as.d);
    }

    if (const NativeMethod* method = findNativeMethod(obj, name))
        return Value::object(alloc<ObjBoundNative>(obj, method));
    if (const Value* extension = findExtension(obj, name))
        return Value::object(alloc<ObjBoundMethod>(obj, asClosure(*extension)));
    // What an extend brought the built-in type (G30).
    if (const Value* getter = builtinFitGetter(obj, name)) return callWithReceiver(obj, asClosure(*getter));
    if (const Value* method = builtinFitMethod(obj, name)) return Value::object(alloc<ObjBoundMethod>(obj, asClosure(*method)));
    throw runtimeError(Thrown::UnsupportedOperationException, "undefined property '" + name->utf8() + "' on a value of type " + typeName(obj));
}

// ---- Value types (M6.6 part 2, value-lists.html) ----

// The class a class-typed field is declared with, found the first time it is needed (the
// class may be declared later in the program than the field).
ObjClass* VM::fieldClass(FieldDecl& field) {
    if ((field.type & kTypeMask) != kTypeInstance) return nullptr;
    if (field.typeClass == nullptr) {
        Value resolved = call(Value::object(field.typeThunk), 0, nullptr);
        if (!resolved.isObjType(ObjType::Class))
            throw runtimeError(Thrown::ClassCastException, "a type must be a class or a built-in type, not a value of type " + typeName(resolved));
        // (In a parallel section several cores may find this out at once: they all find the same class.)
        field.typeClass = asClass(resolved);
    }
    return field.typeClass;
}

// A value type's fixed layout: each field at an offset aligned to its width, in the order the
// class declares them, a value type held by another in place. Worked out when the first value
// of the type is made, when every class its fields name exists; then never again.
const ValueLayout& VM::layoutOf(ObjClass* cls) {
    if (const ValueLayout* known = cls->layout.load(std::memory_order_acquire)) return *known;
    // The value types being laid out, to refuse one that holds itself.
    static thread_local std::vector<const ObjClass*> laying;
    if (std::find(laying.begin(), laying.end(), cls) != laying.end())
        throw runtimeError(Thrown::IllegalStateException, "value " + cls->name->utf8() + " holds itself, and would have no end: make the field nullable, " +
                           cls->name->utf8() + "?, which keeps it in a box");
    laying.push_back(cls);
    struct Done {
        std::vector<const ObjClass*>& list;
        ~Done() { list.pop_back(); }
    } done{laying};

    auto made = std::make_unique<ValueLayout>();
    uint32_t at = 0;
    auto place = [&](uint32_t size, uint32_t align) {
        at = (at + align - 1) / align * align;
        made->align = std::max(made->align, align);
        const uint32_t offset = at;
        at += size;
        return offset;
    };
    for (FieldDecl& declared : cls->declared) {
        ValueField field;
        field.name = declared.name;
        field.type = static_cast<uint8_t>(declared.type);
        const uint8_t id = declared.type & kTypeMask;
        const bool nullable = (declared.type & kTypeNullable) != 0;
        if (declared.type == 0) {
            field.kind = ValueField::Any;
        } else if (!nullable && id == kTypeInt) {
            field.kind = ValueField::Int;
        } else if (!nullable && id == kTypeFloat) {
            field.kind = ValueField::Float;
        } else if (!nullable && id == kTypeBool) {
            field.kind = ValueField::Bool;
        } else if (!nullable && narrowOf(id) != Narrow::None) {
            field.kind = ValueField::Narrow;  // a byte, a char or a storage width (D148)
            field.narrow = narrowOf(id);
        } else if (id == kTypeInstance) {
            field.cls = fieldClass(declared);
            field.kind = !nullable && field.cls->isValue ? ValueField::Nested : ValueField::Object;
            // Declared wider than its phantom type parameters can be: its type kept after it (G4).
            if (field.kind == ValueField::Nested && cls->typeSlottedAt(static_cast<size_t>(&declared - cls->declared.data())))
                field.typeSlot = (layoutOf(field.cls).size + 7) / 8 * 8;
        } else if (id == kTypeString || id == kTypeList || id == kTypeMap || id == kTypeRange || id == kTypeBytes ||
                   id == kTypeFunction || id == kTypeType || id == kTypeSet || id == kTypeFrozenList || id == kTypeFrozenMap || id == kTypeFrozenSet ||
                   id == kTypeValues) {
            field.kind = ValueField::Object;  // null is a null pointer, where the type takes it
        } else {
            field.kind = ValueField::Any;  // Number, and an int, float or bool that may be null
        }
        switch (field.kind) {
            case ValueField::Int:
            case ValueField::Float: field.offset = place(8, 8); break;
            case ValueField::Bool: field.offset = place(1, 1); break;
            case ValueField::Narrow: field.offset = place(narrowSize(field.narrow), narrowSize(field.narrow)); break;
            case ValueField::Object:
                field.offset = place(8, 8);
                made->objects.push_back(field.offset);
                break;
            case ValueField::Any:
                field.offset = place(sizeof(Value), 8);
                made->values.push_back(field.offset);
                break;
            case ValueField::Nested: {
                const ValueLayout& inner = layoutOf(field.cls);
                field.offset = field.typeSlot != 0 ? place(field.typeSlot + 8, std::max<uint32_t>(inner.align, 8)) : place(inner.size, inner.align);
                for (uint32_t offset : inner.objects) made->objects.push_back(field.offset + offset);
                for (uint32_t offset : inner.values) made->values.push_back(field.offset + offset);
                break;
            }
        }
        made->fields.push_back(field);
    }
    made->size = (at + made->align - 1) / made->align * made->align;
    for (const ObjString* component : cls->components) {
        uint32_t index = UINT32_MAX;
        for (size_t f = 0; f < made->fields.size(); f++)
            if (made->fields[f].name == component) index = static_cast<uint32_t>(f);
        made->components.push_back(index);
    }
    // The checker refuses a bigger one; this holds for one it did not see (Size, 2026-11-03).
    if (made->size > kMaxValueBytes) valueTooBig(cls, made->size);

    // Two cores may lay a type out at once; the first kept is kept, and the same.
    VM& root = root_ != nullptr ? *root_ : *this;
    std::lock_guard<std::mutex> hold(root.internMutex_);
    if (const ValueLayout* known = cls->layout.load(std::memory_order_acquire)) return *known;
    // At most 8 bytes, holding no object: kept in the value itself, with no box (M6.6 part 5).
    if (made->size <= 8 && made->objects.empty() && made->values.empty()) {
        static std::atomic<uint32_t> nextSmall{1};
        const uint32_t id = nextSmall.fetch_add(1, std::memory_order_relaxed);
        if (id < kMaxSmallClasses) {
            cls->smallId = id;
            smallClasses[id].store(cls, std::memory_order_release);
            root.smallKept_.push_back(cls);
        }
    }
    cls->layoutOwned = std::move(made);
    cls->layout.store(cls->layoutOwned.get(), std::memory_order_release);
    return *cls->layoutOwned;
}

#ifdef WW_OP_COUNTS
// In the build that counts instructions: how many values were put in boxes of their own (M6.6
// part 4), shown when the program ends.
namespace {
struct BoxCount {
    std::atomic<uint64_t> made{0};
    ~BoxCount() {
        std::fprintf(stderr, "value boxes made: %llu\n", static_cast<unsigned long long>(made.load()));
        std::fprintf(stderr, "objects made, boxes included: %llu\n", static_cast<unsigned long long>(VM::objectsMade.load() + made.load()));
    }
} boxCount;
}  // namespace
std::atomic<uint64_t> VM::objectsMade{0};
#endif

// A box for a value of `cls`, its fields zero: 0, 0.0, false and null.
ObjValue* VM::newValue(ObjClass* cls) {
#ifdef WW_OP_COUNTS
    boxCount.made.fetch_add(1, std::memory_order_relaxed);
#endif
    const ValueLayout& layout = layoutOf(cls);
    ObjValue* box = new (layout.size) ObjValue(cls);
    std::memset(box->data(), 0, layout.size);
    box->core = 0;  // every core's to read: it never changes once made
    box->next = objects_;
    objects_ = box;
    bytesAllocated_ += sizeof(ObjValue) + layout.size;
    return box;
}

// The value of a field kept at `at` in a value's layout.
// The cache an RGetPath keeps: the class's shape (bits 32 to 63), where the last field's bytes are
// (16 to 31: in the object's values, or from where the Ref points), where the object keeps its
// first field's place (3 to 15, in 8-byte steps), the last field's kind (1 and 2: int, float,
// bool), and whether it starts from an object (0).
bool VM::resolvePath(const Value& obj, const uint8_t* ip, const Value* consts, std::atomic<uint64_t>* caches, Value& out) {
    const int hops = ip[6];
    auto u16At = [&](int at) { return static_cast<size_t>(ip[at] | ip[at + 1] << 8); };
    auto nameAt = [&](int k) { return asString(consts[u16At(7 + 2 * k)]); };
    for (int k = 0; k < hops; k++)
        if ((nameAt(k)->restricted & 1) != 0) return false;  // a name with rules on who reads it: the stack code's way
    ObjClass* cls = nullptr;
    const uint8_t* bytes = nullptr;
    uint32_t offset = 0, slotOffset = 0, shape = 0;
    uint64_t form = 0;
    int k = 0;
    if (obj.isObjType(ObjType::Instance)) {
        ObjInstance* instance = asInstance(obj);
        if (instance->core != 0 && instance->core != coreId_) return false;
        const FieldDecl* decl = instance->declOf(nameAt(0));
        if (decl == nullptr || decl->inlineOffset < 0 || decl->payload != FieldDecl::NoPayload || decl->slot < 0) return false;
        const Value& slot = instance->fields()[decl->slot];
        if (slot.kind != ValueKind::Ref || slot.as.obj != instance) return false;  // null, or a box: not inside it
        cls = instance->cls->inlineFieldAt(static_cast<uint32_t>(decl->inlineOffset))->cls;
        bytes = instance->inlineData();
        offset = static_cast<uint32_t>(decl->inlineOffset);
        slotOffset = static_cast<uint32_t>(reinterpret_cast<const uint8_t*>(&slot) - reinterpret_cast<const uint8_t*>(instance));
        shape = instance->shape;
        form = 1;
        k = 1;
    } else if (obj.isRef() && obj.refDepth == 0) {
        cls = refHolderClass(obj);
        if (cls == nullptr) return false;
        bytes = refQuickly(obj, false);
        if (bytes == nullptr) return false;
        shape = cls->shape;
    } else {
        return false;
    }
    for (; k < hops; k++) {
        const Value* index = cls->declaredIndex.find(nameAt(k));
        if (index == nullptr) return false;
        const ValueField& field = layoutOf(cls).fields[static_cast<size_t>(index->as.i)];
        offset += field.offset;
        if (k < hops - 1) {
            if (field.kind != ValueField::Nested) return false;
            cls = field.cls;
            continue;
        }
        const uint64_t kind = field.kind == ValueField::Int ? 0 : field.kind == ValueField::Float ? 1 : field.kind == ValueField::Bool ? 2 : 3;
        if (kind == 3 || offset > 0xFFFF || slotOffset % 8 != 0 || slotOffset / 8 > 0x1FFF) return false;
        const uint64_t seen = static_cast<uint64_t>(shape) << 32 | static_cast<uint64_t>(offset) << 16 | static_cast<uint64_t>(slotOffset / 8) << 3 | kind << 1 | form;
        caches[u16At(4)].store(seen, std::memory_order_relaxed);
        readPathField(bytes + offset, seen, out);
        return true;
    }
    return false;
}

Value VM::readValueField(const uint8_t* at, const ValueField& field, const ObjTypeInfo* type) {
    switch (field.kind) {
        case ValueField::Int: {
            int64_t i;
            std::memcpy(&i, at, sizeof i);
            return Value::integer(i);
        }
        case ValueField::Float: {
            double d;
            std::memcpy(&d, at, sizeof d);
            return Value::number(d);
        }
        case ValueField::Bool: return Value::boolean(*at != 0);
        case ValueField::Narrow: return readNarrow(field.narrow, at);
        case ValueField::Object: {
            Obj* obj;
            std::memcpy(&obj, at, sizeof obj);
            return obj != nullptr ? Value::object(obj) : Value::null();
        }
        case ValueField::Any: {
            Value v;
            std::memcpy(&v, at, sizeof v);
            return v;
        }
        case ValueField::Nested: {
            if (field.typeSlot != 0)
                if (const ObjTypeInfo* kept = readTypeSlot(at + field.typeSlot)) type = kept;  // G4
            if (type != nullptr && type->cls != field.cls) type = nullptr;
            if (field.cls->smallId != 0) return smallFrom(field.cls, at, field.cls->layout.load(std::memory_order_relaxed)->size, type);
            // A value held in another, read on its own: a box of its own, a copy (M6.6 part 2).
            ObjValue* box = newValue(field.cls);
            std::memcpy(box->data(), at, layoutOf(field.cls).size);
            box->type = type;
            return Value::object(box);
        }
    }
    return Value::null();
}

// What an int that does not fit a storage width is told (D148).
std::string VM::storageMismatch(Narrow narrow, Value v) {
    int64_t low, high;
    narrowRange(narrow, low, high);
    static const char* const kinds[] = {"", "byte", "char", "int16", "int32", "uint16", "uint32", "float32"};
    static const char* const wrapped[] = {"", "toByteWrapped()", "", "toInt16Wrapped()", "toInt32Wrapped()", "toUint16Wrapped()", "toUint32Wrapped()", ""};
    const size_t at = static_cast<size_t>(narrow);
    return std::to_string(v.as.i) + " does not fit in " + (narrow == Narrow::Int16 || narrow == Narrow::Int32 ? "an " : "a ") + kinds[at] +
           ", which holds " + std::to_string(low) + " to " + std::to_string(high) + "; " + wrapped[at] + " keeps the low " +
           std::to_string(narrowSize(narrow) * 8) + " bits";
}

void VM::checkRange(uint8_t type, Value v, const std::string& where) {
    if (isIntStorage(type & kTypeMask) && v.isInt()) throw runtimeError(Thrown::ClassCastException, storageMismatch(narrowOf(type & kTypeMask), v) + " (" + where + ")");
}

// Writes `value` into a field kept at `at`, checked against the field's declared type as an
// object's field is.
void VM::writeValueField(uint8_t* at, const ValueField& field, const ObjClass* owner, Value value) {
    // The commonest: an int, a float, a bool or a byte into a field of just that.
    switch (field.kind) {
        case ValueField::Int:
            if (value.isInt()) {
                std::memcpy(at, &value.as.i, sizeof value.as.i);
                return;
            }
            break;
        case ValueField::Float:
            if (value.isFloat()) {
                std::memcpy(at, &value.as.d, sizeof value.as.d);
                return;
            }
            break;
        case ValueField::Bool:
            if (value.isBool()) {
                *at = value.as.b ? 1 : 0;
                return;
            }
            break;
        case ValueField::Narrow:
            if (field.narrow == Narrow::Byte && value.isByte()) {
                *at = static_cast<uint8_t>(value.as.i);
                return;
            }
            break;
        default: break;
    }
    if (field.kind == ValueField::Narrow && value.isInt() && field.narrow != Narrow::Byte && field.narrow != Narrow::Char &&
        !fitsNarrow(field.narrow, value))
        throw runtimeError(Thrown::ClassCastException, storageMismatch(field.narrow, value) + " (field '" + field.name->utf8() + "' of " + owner->name->utf8() + ")");
    if (field.type != 0 && !(value.isNull() && (field.type & kTypeNullable) != 0) && (!matchesType(value, field.type, field.cls) && !widenedByte(value, field.type)))
        throw runtimeError(Thrown::ClassCastException, "cannot assign a value of type " + typeName(value) + " to field '" + field.name->utf8() + "' of " +
                           owner->name->utf8() + ", which is declared as " + declaredTypeText(field.type, field.cls));
    if (field.kind == ValueField::Nested && field.typeSlot != 0) writeTypeSlot(at + field.typeSlot, recordedType(value));  // G4
    if (value.isRef() && field.kind == ValueField::Nested) {
        std::memmove(at, refAt(value, false), layoutOf(field.cls).size);  // the same bytes, perhaps
        return;
    }
    value = own(value);  // a value kept in another is kept, as in a box
    switch (field.kind) {
        case ValueField::Int: std::memcpy(at, &value.as.i, sizeof value.as.i); break;
        case ValueField::Float: std::memcpy(at, &value.as.d, sizeof value.as.d); break;
        case ValueField::Bool: *at = value.as.b ? 1 : 0; break;
        case ValueField::Narrow:
            if (field.narrow == Narrow::Byte && value.isInt()) value = Value::byte(static_cast<uint8_t>(value.as.i));
            writeNarrow(field.narrow, at, value);
            break;
        case ValueField::Object: {
            // A value type's value that may be null is kept by pointer: one kept in a Value of
            // its own (a small one, of one field) is boxed first, or it would be kept as null.
            if (value.isSmall()) value = boxSmall(value);
            Obj* obj = value.isObj() ? value.as.obj : nullptr;
            std::memcpy(at, &obj, sizeof obj);
            break;
        }
        case ValueField::Any: std::memcpy(at, &value, sizeof value); break;
        case ValueField::Nested:
            std::memcpy(at, value.isSmall() ? smallBytes(value) : asValue(value)->data(), layoutOf(field.cls).size);
            break;
    }
}

inline int operandU16(const uint8_t* at);  // below, with the interpreter

// A value held in another, read with one of its own fields at once, as in `s.pref.width`: the
// instruction after (a GetProp or GetOwn starting at `next`) is done here as well, and the field read
// where it is, with no copy made of the value that holds it. `at` is where `field` is, a
// Nested one. Gives where the next instruction to run is, and the value read in `out`; a copy
// of the held value where the instruction after is not such a read.
const uint8_t* VM::readThrough(const uint8_t* at, const ValueField* field, const uint8_t* next, const Value* consts, Value& out,
                                const ObjTypeInfo* type) {
    for (;;) {
        const Op op = opAt(next + kThreaded);
        if (op != Op::GetProp && op != Op::GetOwn && op != Op::GetPropRef && op != Op::GetOwnRef) break;
        const ObjString* name = asString(consts[operandU16(next + kThreaded + kOpBytes)]);
        // A name some class keeps from being read, and this value's class among them: whether it
        // may be read is told by the instruction itself. (One only kept from being assigned, as a
        // `private set` width elsewhere is, or kept by other classes only, is read here.)
        if ((name->restricted & 1) != 0 && (field->cls->accessBits >> (name->hash() & 63) & 1) != 0) break;
        const Value* index = field->cls->declaredIndex.find(name);
        if (index == nullptr) break;
        const ValueField& inner = layoutOf(field->cls).fields[static_cast<size_t>(index->as.i)];
        next += kThreaded + kOpBytes + 4;  // the instruction and its two operands
        if (inner.kind != ValueField::Nested) {
            out = readValueField(at + inner.offset, inner);
            return next;
        }
        at += inner.offset;
        field = &inner;
        type = nullptr;  // a value held in another: the one given was the other's
    }
    out = readValueField(at, *field, type);
    return next;
}

// Field `name` of a value, or -1 if its type has no such field.
int64_t VM::valueFieldIndex(const ObjValue* box, const ObjString* name) const {
    const Value* at = box->cls->declaredIndex.find(name);
    return at != nullptr ? at->as.i : -1;
}

Value VM::valueField(const ObjValue* box, size_t index) {
    const ValueField& field = layoutOf(box->cls).fields[index];
    return readValueField(box->data() + field.offset, field);
}

// Gives a field of a value being made its value; a value once made never changes.
void VM::setValueField(ObjValue* box, ObjString* name, Value value) {
    const int64_t index = valueFieldIndex(box, name);
    if (index < 0)
        throw runtimeError(Thrown::UnsupportedOperationException, box->cls->name->utf8() + " has no field '" + name->utf8() + "'; fields must be declared in it with val or var");
    if (!box->valueOpen)
        throw runtimeError(Thrown::IllegalStateException, "'" + name->utf8() + "' of value " + box->cls->name->utf8() +
                           " cannot be given a new value: a value never changes once it is made");
    const ValueField& field = layoutOf(box->cls).fields[static_cast<size_t>(index)];
    writeValueField(box->data() + field.offset, field, box->cls, value);
}

// Whether two values of one value type hold the same, field by field, with == for each.
bool VM::valuesSame(const ObjValue* a, const ObjValue* b, int depth) {
    if (a->cls != b->cls) return false;
    const ValueLayout& layout = layoutOf(a->cls);
    for (size_t i = 0; i < layout.fields.size(); i++) {
        const ValueField& field = layout.fields[i];
        switch (field.kind) {
            case ValueField::Int:
            case ValueField::Bool:
                if (std::memcmp(a->data() + field.offset, b->data() + field.offset, field.kind == ValueField::Int ? 8 : 1) != 0)
                    return false;
                break;
            case ValueField::Float: {
                double x, y;
                std::memcpy(&x, a->data() + field.offset, sizeof x);
                std::memcpy(&y, b->data() + field.offset, sizeof y);
                if (!(x == y)) return false;  // -0.0 is 0.0, as == says
                break;
            }
            default: {
                const Value x = readValueField(a->data() + field.offset, field);
                TempRoot keep(*this, x);
                const Value y = readValueField(b->data() + field.offset, field);
                TempRoot keepToo(*this, y);
                if (!equals(x, y, depth + 1)) return false;
            }
        }
    }
    return true;
}

// ---- Lists (M6.6 part 3, value-lists.html) ----
//
// A list keeps its items as 16-byte values, or, when it was made to hold values (List<int>,
// List<float>, List<bool>, a value type's, and the nullable ones of those), packed one after
// another at their own widths, a bit per item for null beside them where the type takes null.
// Everything that reads or changes a list's items goes through these, or works on `block`
// itself where it knows the store.

namespace {
bool nullAt(const ObjList* list, size_t i) { return (list->pack->nulls[i / 64] >> (i % 64) & 1) != 0; }
void setNullAt(ObjList* list, size_t i, bool null) {
    const uint64_t bit = uint64_t{1} << (i % 64);
    if (null) list->pack->nulls[i / 64] |= bit;
    else list->pack->nulls[i / 64] &= ~bit;
}
}  // namespace

// How a list made to hold `item` keeps its items, and how wide each is.
ListStore VM::storeFor(const ObjTypeInfo* item, uint32_t& width, ObjClass*& itemClass) {
    itemClass = nullptr;
    switch (item->id) {
        case kTypeInt: width = 8; return ListStore::Ints;
        case kTypeFloat: width = 8; return ListStore::Floats;
        case kTypeBool: width = 1; return ListStore::Bools;
        case kTypeByte:
        case kTypeChar:
        case kTypeInt16:
        case kTypeInt32:
        case kTypeUint16:
        case kTypeUint32:
        case kTypeFloat32: width = narrowSize(narrowOf(item->id)); return ListStore::Narrow;
        case kTypeInstance:
            if (item->cls != nullptr && item->cls->isValue) {
                itemClass = item->cls;
                width = layoutOf(item->cls).size;
                return ListStore::Items;
            }
            break;
        default: break;
    }
    width = sizeof(Value);
    return ListStore::Values;
}

// Gives a list the type it was made to hold, where it has none yet, and from then on keeps its
// items as that type says: packed, for a list of values.
void VM::typeList(ObjList* list, ObjTypeInfo* type) {
    if (list->type != nullptr || type == nullptr || type->id != kTypeList || type->args.size() != 1) return;
    list->type = type;
    if (type->packs.load(std::memory_order_relaxed) == 0) return;  // as most lists: 16-byte values
    uint32_t width = 0;
    ObjClass* itemClass = nullptr;
    const ListStore store = storeFor(type->args[0], width, itemClass);
    type->packs.store(store == ListStore::Values ? 0 : 1, std::memory_order_relaxed);
    if (store == ListStore::Values) return;
    std::vector<Value> items = std::move(list->values());
    list->values().clear();
    list->values().shrink_to_fit();
    list->pack = std::make_unique<PackedItems>();
    list->pack->store = store;
    if (store == ListStore::Items && keepsItemTypes(type->args[0])) {
        // Declared wider than its items' phantom type parameters can be: each item's type kept
        // after it (G4).
        list->pack->typeSlot = (width + 7) / 8 * 8;
        width = list->pack->typeSlot + 8;
    }
    list->pack->width = width;
    list->pack->itemClass = itemClass;
    if (store == ListStore::Items && !type->args[0]->args.empty() && type->args[0]->cls == itemClass) list->pack->itemType = type->args[0];
    list->pack->narrow = store == ListStore::Narrow ? narrowOf(type->args[0]->id) : Narrow::None;
    list->pack->nullable = type->args[0]->nullable;
    list->pack->count = 0;
    // The items it was made with, checked and packed: a list that cannot hold one of them is
    // an error where it is made.
    TempRoot keep(*this, Value::object(list));
    listInsert(list, 0, items.data(), items.size());
}

uint8_t* VM::itemAt(ObjList* list, size_t i) { return list->pack->block.data() + i * list->pack->width; }

// Item `i` of a packed list (listGet takes the others itself). A value type's item is copied into a
// box of its own.
Value VM::packedGet(ObjList* list, size_t i) {
    if (list == bandObject_ && (i < bandFrom_ || i >= bandTo_)) [[unlikely]] throw outsideBand(i);
    if (list->pack->nullable && nullAt(list, i)) return Value::null();
    const uint8_t* at = itemAt(list, i);
    switch (list->pack->store) {
        case ListStore::Ints: {
            int64_t v;
            std::memcpy(&v, at, sizeof v);
            return Value::integer(v);
        }
        case ListStore::Floats: {
            double v;
            std::memcpy(&v, at, sizeof v);
            return Value::number(v);
        }
        case ListStore::Bools: return Value::boolean(*at != 0);
        case ListStore::Narrow: return readNarrow(list->pack->narrow, at);
        case ListStore::Items: {
            const ObjTypeInfo* type = list->type != nullptr && list->type->args.size() == 1 && list->type->args[0]->cls == list->pack->itemClass
                                          ? list->type->args[0]
                                          : nullptr;
            if (list->pack->typeSlot != 0)
                if (const ObjTypeInfo* kept = readTypeSlot(at + list->pack->typeSlot)) type = kept;
            const uint32_t size = list->pack->typeSlot != 0 ? list->pack->typeSlot : list->pack->width;
            if (list->pack->itemClass->smallId != 0) return smallFrom(list->pack->itemClass, at, layoutOf(list->pack->itemClass).size, type);
            ObjValue* box = newValue(list->pack->itemClass);
            std::memcpy(box->data(), at, std::min<uint32_t>(size, layoutOf(list->pack->itemClass).size));
            box->type = type;
            return Value::object(box);
        }
        default: return Value::null();
    }
}

// Whether `v` may be an item of a packed list; an error saying why not, where it may not.
void VM::checkItem(const ObjList* list, Value v) {
    if (v.isNull() && list->pack->nullable) return;
    // A byte goes where an int does: kept as its number.
    if (v.isByte() && (list->pack->store == ListStore::Ints ||
                       (list->pack->store == ListStore::Narrow && list->pack->narrow != Narrow::Byte && list->pack->narrow != Narrow::Char)))
        v = Value::integer(v.as.i);
    bool fits = false;
    switch (list->pack->store) {
        case ListStore::Ints: fits = v.isInt(); break;
        case ListStore::Floats: fits = v.isFloat(); break;
        case ListStore::Bools: fits = v.isBool(); break;
        case ListStore::Narrow:
            if (v.isInt() && list->pack->narrow != Narrow::Byte && list->pack->narrow != Narrow::Char && !fitsNarrow(list->pack->narrow, v))
                throw runtimeError(Thrown::ClassCastException, storageMismatch(list->pack->narrow, v));
            fits = fitsNarrow(list->pack->narrow, v);
            break;
        case ListStore::Items:
            fits = (v.isObjType(ObjType::Value) && asValue(v)->cls == list->pack->itemClass) || (v.isRef() && refClass(v) == list->pack->itemClass) ||
                   (v.isSmall() && smallClassOf(v) == list->pack->itemClass);
            break;
        default: fits = true; break;
    }
    if (!fits)
        throw runtimeError(Thrown::ClassCastException, "cannot put a value of type " + typeName(v) + " in a " + typeInfoText(list->type) +
                           ", which keeps only what it was made to hold");
}

// Writes `v`, already checked, as item `i` of a packed list.
void VM::writeItem(ObjList* list, size_t i, Value v) {
    uint8_t* at = itemAt(list, i);
    if (list->pack->nullable) setNullAt(list, i, v.isNull());
    if (v.isNull()) {
        std::memset(at, 0, list->pack->width);
        return;
    }
    switch (list->pack->store) {
        case ListStore::Ints: std::memcpy(at, &v.as.i, 8); break;
        case ListStore::Floats: std::memcpy(at, &v.as.d, 8); break;
        case ListStore::Bools: *at = v.as.b ? 1 : 0; break;
        case ListStore::Narrow: writeNarrow(list->pack->narrow, at, v); break;
        case ListStore::Items:
            if (list->pack->typeSlot != 0) {
                const ObjTypeInfo* type = recordedType(v);
                std::memmove(at, v.isRef() ? refAt(v, false) : v.isSmall() ? smallBytes(v) : asValue(v)->data(), layoutOf(list->pack->itemClass).size);
                writeTypeSlot(at + list->pack->typeSlot, type);
                break;
            }
            std::memmove(at, v.isRef() ? refAt(v, false) : v.isSmall() ? smallBytes(v) : asValue(v)->data(), list->pack->width);
            break;
        default: break;
    }
}

void VM::listSet(ObjList* list, size_t i, Value v) {
    if (!list->packed()) {
        list->values()[i] = own(v);
        return;
    }
    checkItem(list, v);
    writeItem(list, i, v);
}

// Puts `n` items at position `at`, moving those from there on along.
void VM::listInsert(ObjList* list, size_t at, const Value* items, size_t n) {
    if (!list->packed()) {
        std::vector<Value>& values = list->values();
        values.insert(values.begin() + static_cast<std::ptrdiff_t>(at), items, items + n);
        for (size_t k = at; k < at + n; k++) values[k] = own(values[k]);
        account(n * sizeof(Value));
        return;
    }
    for (size_t k = 0; k < n; k++) checkItem(list, items[k]);  // all of them, before anything changes
    const size_t width = list->pack->width;
    const size_t old = list->pack->count;
    const size_t before = list->pack->block.capacity();
    list->pack->block.resize((old + n) * width);
    uint8_t* base = list->pack->block.data();
    std::memmove(base + (at + n) * width, base + at * width, (old - at) * width);
    if (list->pack->nullable) {
        coverNulls(*list->pack, old + n);
        for (size_t i = old; i-- > at;) setNullAt(list, i + n, nullAt(list, i));
    }
    list->pack->count = old + n;
    for (size_t k = 0; k < n; k++) writeItem(list, at + k, items[k]);
    if (list->pack->block.capacity() > before) account(list->pack->block.capacity() - before);
}

void VM::listAdd(ObjList* list, Value v) { listInsert(list, list->size(), &v, 1); }

void VM::listReserve(ObjList* list, size_t n) {
    if (!list->packed()) {
        std::vector<Value>& values = list->values();
        const size_t before = values.capacity();
        values.reserve(n);
        if (values.capacity() > before) account((values.capacity() - before) * sizeof(Value));
        return;
    }
    PackedItems& pack = *list->pack;
    const size_t before = pack.block.capacity();
    pack.block.reserve(n * pack.width);
    if (pack.nullable) coverNulls(pack, n);
    if (pack.block.capacity() > before) account(pack.block.capacity() - before);
}

// Takes `n` items away from position `at`, moving those after it back.
void VM::listErase(ObjList* list, size_t at, size_t n) {
    if (!list->packed()) {
        std::vector<Value>& values = list->values();
        values.erase(values.begin() + static_cast<std::ptrdiff_t>(at), values.begin() + static_cast<std::ptrdiff_t>(at + n));
        return;
    }
    const size_t width = list->pack->width;
    uint8_t* base = list->pack->block.data();
    std::memmove(base + at * width, base + (at + n) * width, (list->pack->count - at - n) * width);
    if (list->pack->nullable)
        for (size_t i = at; i + n < list->pack->count; i++) setNullAt(list, i, nullAt(list, i + n));
    list->pack->count -= n;
    list->pack->block.resize(list->pack->count * width);
}

void VM::listClear(ObjList* list) {
    if (!list->packed()) {
        list->values().clear();
        return;
    }
    list->pack->count = 0;
    list->pack->block.clear();  // its room kept, and the bits for it (coverNulls)
}

// The items as 16-byte values, for what reads them so; the list itself is as it was.
std::vector<Value> VM::listCopy(ObjList* list) {
    if (!list->packed()) return list->values();
    std::vector<Value> out;
    out.reserve(list->pack->count);
    for (size_t i = 0; i < list->pack->count; i++) out.push_back(listGet(list, i));
    return out;
}

// A new list of the same type and store as `like`, with nothing in it.
ObjList* VM::listLike(const ObjList* like) {
    ObjList* list = alloc<ObjList>();
    list->type = like->type;
    if (like->packed()) {
        list->pack = std::make_unique<PackedItems>();
        list->pack->store = like->pack->store;
        list->pack->width = like->pack->width;
        list->pack->typeSlot = like->pack->typeSlot;
        list->pack->itemType = like->pack->itemType;
        list->pack->itemClass = like->pack->itemClass;
        list->pack->narrow = like->pack->narrow;   // what each item of a narrow list is
        list->pack->nullable = like->pack->nullable;
    }
    return list;
}

// `count` items of `from`, from position `start`, put at the end of `to`, both of one store:
// packed ones as one block of memory.
void VM::listCopyRange(ObjList* to, const ObjList* from, size_t start, size_t count) {
    if (!from->packed()) {
        const std::vector<Value>& values = from->values();
        to->values().insert(to->values().end(), values.begin() + static_cast<std::ptrdiff_t>(start),
                            values.begin() + static_cast<std::ptrdiff_t>(start + count));
        account(count * sizeof(Value));
        return;
    }
    const size_t old = to->pack->count;
    if (to != from) {
        // Copied onto the end at once (2026-11-13): a resize first set the new bytes to zero,
        // writing them twice.
        const uint8_t* const src = from->pack->block.data() + start * from->pack->width;
        to->pack->block.resize(old * to->pack->width);
        to->pack->block.insert(to->pack->block.end(), src, src + count * from->pack->width);
    } else {
        to->pack->block.resize((old + count) * to->pack->width);
        std::memcpy(to->pack->block.data() + old * to->pack->width, from->pack->block.data() + start * from->pack->width, count * from->pack->width);
    }
    if (to->pack->nullable) {
        coverNulls(*to->pack, old + count);
        for (size_t i = 0; i < count; i++) setNullAt(to, old + i, nullAt(from, start + i));
    }
    to->pack->count = old + count;
    account(count * to->pack->width);
}

// Two items of one list change places.
void VM::listSwap(ObjList* list, size_t i, size_t j) {
    if (!list->packed()) {
        std::swap(list->values()[i], list->values()[j]);
        return;
    }
    uint8_t tmp[64];
    std::vector<uint8_t> big;
    uint8_t* t = list->pack->width <= sizeof tmp ? tmp : (big.resize(list->pack->width), big.data());
    std::memcpy(t, itemAt(list, i), list->pack->width);
    std::memcpy(itemAt(list, i), itemAt(list, j), list->pack->width);
    std::memcpy(itemAt(list, j), t, list->pack->width);
    if (list->pack->nullable) {
        const bool a = nullAt(list, i);
        setNullAt(list, i, nullAt(list, j));
        setNullAt(list, j, a);
    }
}

// A value made from its header alone, every argument given (M6.6 part 4): its fields written
// straight from the arguments, with no initializer run. Where the result goes, if `dest` is such
// a place and still there (value-lists.html section 7), every argument worked out before any of
// the old value is written over; otherwise in a box of its own. The result in place of the call.
// Header field `k` of a value type: where its layout found it, or by its name.
static const ValueField& headerField(ObjClass* cls, const ValueLayout& layout, int k) {
    if (static_cast<size_t>(k) < layout.components.size() && layout.components[static_cast<size_t>(k)] != UINT32_MAX)
        return layout.fields[layout.components[static_cast<size_t>(k)]];
    return layout.fields[static_cast<size_t>(cls->declaredIndex.find(cls->components[static_cast<size_t>(k)])->as.i)];
}

void VM::makeFromHeader(ObjClass* cls, int argc, Value dest, const ObjTypeInfo* made) {
    const ValueLayout& layout = layoutOf(cls);
    Value* const args = sp_ - argc;
    if (dest.isRef() && dest.as.obj->type == ObjType::Places && dest.refDepth == 0 && layout.size <= ObjPlace::kBytes) {
        // A frame's place (D149), empty or holding a value of this type: it takes this one, and
        // keeps its type arguments (G4). A list's item, or a value inside an object, has its
        // container's.
        ObjPlace* place = static_cast<ObjPlace*>(dest.as.obj);
        if (place->cls == nullptr) place->cls = cls;
        if (place->cls == cls) place->type = made;
    }
    if (dest.isRef() && refClass(dest) == cls && refBytes(dest) != nullptr) {
        uint8_t small[256];
        std::vector<uint8_t> large;
        uint8_t* item = layout.size <= sizeof small ? small : (large.resize(layout.size), large.data());
        uint8_t* const at = refAt(dest, true);
        std::memcpy(item, at, layout.size);
        for (int k = 0; k < argc; k++) {
            const ValueField& field = headerField(cls, layout, k);
            writeValueField(item + field.offset, field, cls, args[k]);
        }
        std::memcpy(at, item, layout.size);
        // Where the place keeps its value's type beside it: this one's (G4).
        if (dest.refDepth == 0 && dest.as.obj->type == ObjType::List && static_cast<ObjList*>(dest.as.obj)->pack->typeSlot != 0)
            writeTypeSlot(at + static_cast<ObjList*>(dest.as.obj)->pack->typeSlot, made);
        else if (dest.refDepth == 0 && dest.as.obj->type == ObjType::Instance)
            if (const ObjClass::InlineField* inside = static_cast<ObjInstance*>(dest.as.obj)->cls->inlineFieldAt(dest.refOffset); inside->typeSlot != 0)
                writeTypeSlot(at + inside->typeSlot, made);
        sp_ = args;
        sp_[-1] = dest;
        return;
    }
    if (cls->smallId != 0) {
        // Small: in the value itself, with no box (M6.6 part 5).
        uint8_t bytes[8] = {};
        for (int k = 0; k < argc; k++) {
            const ValueField& field = headerField(cls, layout, k);
            writeValueField(bytes + field.offset, field, cls, args[k]);
        }
        sp_ = args;
        sp_[-1] = smallFrom(cls, bytes, sizeof bytes, made);
        return;
    }
    ObjValue* box = newValue(cls);
    box->type = made;
    sp_[-argc - 1] = Value::object(box);  // kept where the collector sees it
    for (int k = 0; k < argc; k++) {
        const ValueField& field = headerField(cls, layout, k);
        writeValueField(box->data() + field.offset, field, cls, args[k]);
    }
    sp_ = args;
}

// A class finished: one more for VM::layInline to wait for. Kept out of run().
#ifdef _MSC_VER
__declspec(noinline)
#else
__attribute__((noinline))
#endif
void VM::classFinished() {
    (root_ != nullptr ? *root_ : *this).classesFinished_.fetch_add(1, std::memory_order_acq_rel);
}

// Which of a class's fields are kept inside its objects (M6.6 part 5, value-lists.html section
// 4): those whose type is a value type kept in a box elsewhere, not small, laid out one after
// another after the object's other slots. One that may be null is kept there too (G16): its slot
// holds a Ref to it, or null. Worked out when the first object of the class is made where every
// class those fields name exists; until then, objects are made with no room for any of them and
// keep their values in boxes, and the layout is tried again once another class has been made.
void VM::layInline(ObjClass* cls) {
    if (!cls->fieldsLaid.load(std::memory_order_acquire)) layFields(cls);
    VM& root = root_ != nullptr ? *root_ : *this;
    const uint64_t finished = root.classesFinished_.load(std::memory_order_acquire);
    if (cls->inlineTriedAt.load(std::memory_order_acquire) == finished) return;  // still waiting for a class
    std::vector<ObjClass::InlineField> fields;
    std::vector<int32_t> offsets(cls->declared.size(), -1);
    uint32_t at = 0;
    bool waiting = false;
    for (size_t i = 0; i < cls->declared.size(); i++) {
        FieldDecl& declared = cls->declared[i];
        if ((declared.type & kTypeMask) != kTypeInstance) continue;
        ObjClass* type = declared.typeClass;
        const ValueLayout* layout = nullptr;
        try {
            if (type == nullptr) type = fieldClass(declared);
            if (type->isValue) layout = &layoutOf(type);
        } catch (WwThrow&) {
            waiting = true;  // not there yet
            continue;
        }
        if (layout == nullptr || type->smallId != 0) continue;  // not a value type, or kept in the value itself
        // Past 64 KB of values inside the object, a place in one could not be told by its offset:
        // the rest stay in boxes of their own, as a value does before its class is laid out.
        if ((at + 7) / 8 * 8 + layout->size > kMaxValueBytes) continue;
        at = (at + 7) / 8 * 8;
        offsets[i] = static_cast<int32_t>(at);
        // Declared wider than its phantom type parameters can be: its type kept after it (G4).
        const uint32_t typeSlot = cls->typeSlottedAt(i) ? (layout->size + 7) / 8 * 8 : 0;
        fields.push_back({at, layout->size, type, static_cast<uint32_t>(i), typeSlot});
        at += typeSlot != 0 ? typeSlot + 8 : layout->size;
    }
    std::lock_guard<std::mutex> hold(root.internMutex_);
    if (cls->inlineLaid.load(std::memory_order_acquire)) return;
    if (waiting) {
        cls->inlineTriedAt.store(finished, std::memory_order_release);
        return;
    }
    for (size_t i = 0; i < offsets.size(); i++) cls->declared[i].inlineOffset = offsets[i];
    cls->inlineFields = std::move(fields);
    cls->inlineSlots = (at + sizeof(Value) - 1) / sizeof(Value);
    cls->inlineLaid.store(true, std::memory_order_release);
}

// Where the objects of a class keep each field (M6.6 part 6, FieldDecl::slot), worked out when
// the first one is made: an int, a float or a bool whose type is written and is not null as its
// payload alone, the first 256 of them, after the bits that say which have a value; any other
// field in a slot of its own.
void VM::layFields(ObjClass* cls) {
    VM& root = root_ != nullptr ? *root_ : *this;
    std::lock_guard<std::mutex> hold(root.internMutex_);
    if (cls->fieldsLaid.load(std::memory_order_acquire)) return;
    auto kindOf = [](const FieldDecl& d) {
        if ((d.type & kTypeNullable) != 0) return FieldDecl::NoPayload;
        switch (d.type & kTypeMask) {
            case kTypeInt: return FieldDecl::IntPayload;
            case kTypeFloat: return FieldDecl::FloatPayload;
            case kTypeBool: return FieldDecl::BoolPayload;
            default: return narrowOf(d.type & kTypeMask) != Narrow::None ? FieldDecl::NarrowPayload : FieldDecl::NoPayload;
        }
    };
    // A field its init must set, here or in a class above.
    auto required = [&](const ObjString* name) {
        for (const ObjClass* k = cls; k != nullptr; k = k->super)
            for (const ObjString* r : k->ownRequired)
                if (r == name) return true;
        return false;
    };
    // A bit each only for the vals and the fields init must set, up to 256 of those.
    uint32_t tracked = 0;
    for (const FieldDecl& d : cls->declared)
        if (kindOf(d) != FieldDecl::NoPayload && (d.isVal || required(d.name))) tracked++;
    tracked = std::min(tracked, 256u);
    uint32_t at = (tracked + 63) / 64 * 8, slot = 0, bit = 0;
    // The 8-byte payloads first, then the narrow ones from the widest down, each in line.
    std::vector<FieldDecl*> narrow[5];
    for (FieldDecl& d : cls->declared) {
        d.inlineOffset = -1;  // (part 5 lays those out later, from what is here)
        const FieldDecl::PayloadKind kind = kindOf(d);
        const bool needsBit = d.isVal || required(d.name);
        if (kind != FieldDecl::NoPayload && (!needsBit || bit < tracked)) {
            d.payload = kind;
            d.tracked = needsBit;
            d.payloadBit = static_cast<uint16_t>(needsBit ? bit++ : 0);
            d.slot = -1;
            if (kind == FieldDecl::NarrowPayload) {
                d.narrow = narrowOf(d.type & kTypeMask);
                narrow[narrowSize(d.narrow)].push_back(&d);
                continue;
            }
            d.payloadOffset = static_cast<int32_t>(at);
            at += 8;
        } else {
            d.payload = FieldDecl::NoPayload;
            d.payloadOffset = -1;
            d.slot = static_cast<int32_t>(slot++);
        }
    }
    for (size_t size : {4, 2, 1})
        for (FieldDecl* d : narrow[size]) {
            d->payloadOffset = static_cast<int32_t>(at);
            at += static_cast<uint32_t>(size);
        }
    at = (at + 7) / 8 * 8;  // the slots after them are 8-byte aligned
    cls->slotCount = slot;
    cls->payloadBytes = at;
    cls->fieldsLaid.store(true, std::memory_order_release);
}

// A field declared after the class's objects were laid out: a slot of its own, which the
// objects made before do not have.
void VM::fieldAdded(ObjClass* cls, size_t index) {
    FieldDecl& d = cls->declared[index];
    d.payload = FieldDecl::NoPayload;
    d.payloadOffset = -1;
    d.slot = cls->fieldsLaid.load(std::memory_order_acquire) ? static_cast<int32_t>(cls->slotCount++) : -1;
}

// `pts.add(Point(1, 2))` (Op::AddNew): the value written straight into a new item at the end of
// a list that keeps its items packed, its fields from the arguments, as Point(1, 2) would give
// them; false, with nothing done, where that is not so and the call and the add follow.
bool VM::addNew(int argc) {
    Value* const args = sp_ - argc;
    if (!args[-2].isObjType(ObjType::List) || !args[-1].isObjType(ObjType::Class)) return false;
    ObjList* list = asList(args[-2]);
    ObjClass* cls = asClass(args[-1]);
    if (list->store() != ListStore::Items || list->pack->itemClass != cls || !cls->madeFromHeader ||
        static_cast<size_t>(argc) != cls->components.size())
        return false;
    needPatterns(cls);  // the first of its class: its fields' types (G4)
    guardWrite(list);
    if (cls->staticInit != nullptr) ensureClassInit(cls);
    const ValueLayout& layout = layoutOf(cls);
    // Made apart first, so that an argument of the wrong type leaves the list as it was.
    uint8_t small[256];
    std::vector<uint8_t> large;
    uint8_t* item = layout.size <= sizeof small ? small : (large.resize(layout.size), large.data());
    std::memset(item, 0, layout.size);
    for (int k = 0; k < argc; k++) {
        const ValueField& field = headerField(cls, layout, k);
        writeValueField(item + field.offset, field, cls, args[k]);
    }
    PackedItems& pack = *list->pack;
    const size_t before = pack.block.capacity();
    pack.block.insert(pack.block.end(), item, item + layout.size);
    if (pack.nullable) {
        coverNulls(pack, pack.count + 1);
        setNullAt(list, pack.count, false);
    }
    pack.count++;
    if (pack.block.capacity() > before) account(pack.block.capacity() - before);
    sp_ = args - 2;
    *sp_++ = Value::boolean(true);
    return true;
}

// Puts a list's items in the order `order` gives, the position each comes from: each item is
// moved once, a packed one as its bytes.
void VM::listArrange(ObjList* list, const std::vector<size_t>& order) {
    if (!list->packed()) {
        std::vector<Value> arranged;
        arranged.reserve(order.size());
        for (size_t from : order) arranged.push_back(list->values()[from]);
        list->values() = std::move(arranged);
        return;
    }
    PackedItems& pack = *list->pack;
    ItemBlock block(pack.block.size());
    for (size_t i = 0; i < order.size(); i++)
        std::memcpy(block.data() + i * pack.width, pack.block.data() + order[i] * pack.width, pack.width);
    if (pack.nullable) {
        std::vector<bool> nulls(order.size());
        for (size_t i = 0; i < order.size(); i++) nulls[i] = nullAt(list, order[i]);
        for (size_t i = 0; i < order.size(); i++) setNullAt(list, i, nulls[i]);
    }
    pack.block = std::move(block);
}


// The collector's view of a packed list of a value type that holds objects.
void VM::markItems(ObjList* list) {
    if (!list->packed() || list->pack->store != ListStore::Items) return;
    const ValueLayout* layout = list->pack->itemClass->layout.load(std::memory_order_acquire);
    markObject(list->pack->itemClass);
    if (layout == nullptr || (layout->objects.empty() && layout->values.empty())) return;
    for (size_t i = 0; i < list->pack->count; i++) {
        const uint8_t* item = itemAt(list, i);
        for (uint32_t offset : layout->objects) {
            Obj* held;
            std::memcpy(&held, item + offset, sizeof held);
            markObject(held);
        }
        for (uint32_t offset : layout->values) {
            Value held;
            std::memcpy(&held, item + offset, sizeof held);
            markValue(held);
        }
    }
}

// ---- References (M6.6 part 4, value-lists.html section 6) ----

Value VM::ownRef(Value r) {
    const uint8_t* at = refAt(r, false);
    ObjClass* cls = refClass(r);
    const ObjTypeInfo* type = refType(r);
    ObjValue* box = newValue(cls);
    std::memcpy(box->data(), at, layoutOf(cls).size);
    box->type = type;
    return Value::object(box);
}

Value VM::boxSmall(Value v) {
    ObjClass* cls = smallClassOf(v);
    ObjValue* box = newValue(cls);
    std::memcpy(box->data(), smallBytes(v), layoutOf(cls).size);
    box->type = smallType(v);
    return Value::object(box);
}

uint8_t* VM::refAt(Value r, bool write) {
    uint8_t* at = refBytes(r);
    if (at == nullptr)
        throw runtimeError(Thrown::IndexOutOfBoundsException, "item " + std::to_string(r.refIndex) + " of the list is no longer there: the list was made shorter "
                           "while the item was in use");
    if (r.as.obj == bandObject_) [[unlikely]] {
        // In a parallel section over this list: this core's own part of it, and no other.
        if (r.refIndex < bandFrom_ || r.refIndex >= bandTo_) throw outsideBand(r.refIndex);
    } else if (r.as.obj->type == ObjType::Instance) {
        if (write) guardWrite(r.as.obj);
        else guardRead(r.as.obj);
    } else if (write) {
        if (r.as.obj->type == ObjType::List) guardWrite(r.as.obj);
        else if (!r.as.obj->valueOpen)
            throw runtimeError(Thrown::UnsupportedOperationException, "a value of " + refClass(r)->name->utf8() + " cannot be changed there: a value never changes "
                               "where others may see it; change it through the place that holds it");
    }
    return at;
}

bool VM::refField(Value r, const ObjString* name, Value& out) {
    ObjClass* cls = refClass(r);
    const Value* index = cls->declaredIndex.find(name);
    if (index == nullptr) return false;
    const ValueField& field = layoutOf(cls).fields[static_cast<size_t>(index->as.i)];
    if (field.kind == ValueField::Nested) {
        refAt(r, false);  // checked: still there
        out = Value::ref(r.as.obj, r.refIndex, static_cast<uint16_t>(r.refOffset + field.offset), static_cast<uint8_t>(r.refDepth + 1));
    } else {
        out = readValueField(refAt(r, false) + field.offset, field);
    }
    return true;
}

// `place.x = v` where the place is a Ref (M6.6 part 4, value-lists.html section 8): the field
// written where the value is. Only a `var` field; a value never changes where others see it.
void VM::setRefField(Value r, ObjString* name, Value value) {
    ObjClass* cls = refClass(r);
    const Value* index = cls->declaredIndex.find(name);
    if (index == nullptr)
        throw runtimeError(Thrown::UnsupportedOperationException, cls->name->utf8() + " has no field '" + name->utf8() + "'; fields must be declared in it with val or var");
    const size_t at = static_cast<size_t>(index->as.i);
    if (cls->declared[at].isVal) throw runtimeError(Thrown::UnsupportedOperationException, "cannot assign to val field '" + name->utf8() + "' of " + cls->name->utf8());
    const ValueField& field = layoutOf(cls).fields[at];
    writeValueField(refAt(r, true) + field.offset, field, cls, value);
}

Value VM::placeIn(Value& slot) {
    // Where it is already: in a list, an object or one of this frame's places. A place of a frame
    // under it, reached through a variable a lambda shares, is copied as a box is: it may be read
    // where it is by that frame's calls in progress, which see it as it was.
    if (slot.isRef() && slot.as.obj->type != ObjType::Value && (slot.as.obj->type != ObjType::Places || ownPlace(slot))) return slot;
    const Value kept = boxed(slot);
    // Null, for `p?.move()` to pass by: what is changed through it reports a change to null.
    if (kept.isNull()) return kept;
    if (!kept.isObjType(ObjType::Value))
        throw runtimeError(Thrown::NullPointerException, "cannot change a field of " + (kept.isNull() ? std::string("null") : "a value of type " + typeName(kept)) +
                           " through its place");
    // A box of its own, which nothing else holds: a change made in it is seen through this
    // variable or field alone (`val q = p; p.x = 5` leaves q as it was).
    ObjValue* fresh = newValue(asValue(kept)->cls);
    std::memcpy(fresh->data(), asValue(kept)->data(), layoutOf(fresh->cls).size);
    fresh->type = asValue(kept)->type;
    fresh->valueOpen = true;  // may be written, through the place
    slot = Value::object(fresh);
    return Value::ref(fresh, 0, 0, 0);
}

// ---- Values in a frame's places (M6.7 step 6, D149) ----

// EnterPlaces: `count` places for the frame, above those of the frames under it; none where
// there is no room left, and its variables keep boxes, as before.
void VM::enterPlaces(CallFrame* frame, int count) {
    if (context_->places == nullptr) context_->places = new PlaceArena();
    const uint32_t start = placesStart(frame - frames_);
    const uint32_t end = start + static_cast<uint32_t>(count);
    framePlacesEnd(frame - frames_) = end <= PlaceArena::kCount ? end : start | PlaceArena::kNone;
}

// SetPlace: a variable with place `place` given `value`. A value of a value type, in a box or
// where it is kept, is copied into the place, and the variable holds a Ref to it; anything else,
// and a value too large for a place, is held as SetLocal holds it.
void VM::setPlace(const CallFrame* frame, Value& slot, Value value, int place) {
    ObjPlace* const at = placeOf(frame, place);
    ObjClass* cls = nullptr;
    const uint8_t* from = nullptr;
    if (at != nullptr) {
        if (value.isRef()) {
            cls = refClass(value);
            if (cls != nullptr) from = refAt(value, false);
        } else if (value.isObjType(ObjType::Value)) {
            cls = asValue(value)->cls;
            from = asValue(value)->data();
        }
    }
    if (cls == nullptr || layoutOf(cls).size > ObjPlace::kBytes) {
        slot = value;
        return;
    }
    const ObjTypeInfo* type = recordedType(value);
    uint8_t* to = at->data();
    if (to != from) std::memmove(to, from, layoutOf(cls).size);
    at->cls = cls;
    at->type = type;
    slot = Value::ref(at, 0, 0, 0);
}

// WithPlace: place `place`, emptied, for the call about to be made to make its result in; null
// where the frame has no places, and the call makes a box.
Value VM::withPlace(const CallFrame* frame, int place) {
    ObjPlace* const at = placeOf(frame, place);
    if (at == nullptr) return Value::null();
    at->cls = nullptr;
    at->type = nullptr;
    return Value::ref(at, 0, 0, 0);
}

// A field of the class's own stands for a property an interface gives with a getter and a
// setter of its own: the field is the property, as Kotlin's `override var dir : String? = null`
// is, so the interface's accessors, taken in when the class took the interface, go (M8).
void VM::dropInterfaceAccessors(ObjClass* cls, const ObjString* name) {
    for (ObjClass* iface : cls->interfaces) {
        const Value* getter = iface->getters.find(name);
        const Value* mine = cls->getters.find(name);
        if (getter != nullptr && mine != nullptr && mine->as.obj == getter->as.obj) cls->getters.remove(name);
        const Value* setter = iface->setters.find(name);
        const Value* mineSet = cls->setters.find(name);
        if (setter != nullptr && mineSet != nullptr && mineSet->as.obj == setter->as.obj) cls->setters.remove(name);
    }
}

// The captures are kept aside until the object has them: making it may run code that uses the
// stack above its top (its fields' types, worked out for the first object of its class), which
// would overwrite them there, and the collector looks at the stack only below its top.
bool VM::constructObject(ObjClass* cls, int count) {
    std::vector<Value> captured(sp_ - count, sp_);
    TempVector keep(*this, captured);
    sp_ -= count;
    return construct(cls, 0, false, captured.data(), static_cast<uint16_t>(count));
}

bool VM::construct(ObjClass* cls, int argc, bool packed, const Value* captures, uint16_t captureCount) {
    if (cls->staticInit != nullptr) ensureClassInit(cls);  // an enum has its entries by now
    if (cls->isAbstract)
        throw runtimeError(Thrown::UnsupportedOperationException, cls->name->utf8() + " is abstract and cannot be created; create a class that extends it");
    if (cls->isInterface)
        throw runtimeError(Thrown::UnsupportedOperationException, cls->name->utf8() + " is an interface and cannot be created; create a class that implements it");
    if (cls->enumSealed)
        throw runtimeError(Thrown::UnsupportedOperationException, "enum " + cls->name->utf8() + " has only the entries it declares; " + cls->name->utf8() +
                           "() cannot make another");
    // A generic class's type arguments, given by WithTypes: the object keeps its type.
    const ObjTypeInfo* typeArgs = std::exchange(pendingTypeArgs_, nullptr);
    needPatterns(cls);  // the first of its class: its fields' types (G4)
    if (cls->isValue) {
        const Value dest = std::exchange(pendingDestination_, Value::null());
        // Its class with the type arguments it was made with (`Pair<int, string>(1, "s")`, or
        // worked out by the checker), which it keeps (G4).
        const ObjTypeInfo* made = typeArgs != nullptr && !typeArgs->args.empty() ? typeMadeOf(cls, typeArgs) : nullptr;
        // Every argument given: none left out by a call by name (`Insets(top = 1, right = 2)`),
        // which the initializer gives its default.
        bool allGiven = cls->madeFromHeader && static_cast<size_t>(argc) == cls->components.size() && !packed;
        for (int k = 0; allGiven && k < argc; k++) allGiven = sp_[-argc + k].kind != ValueKind::Undefined;
        if (allGiven) {
            makeFromHeader(cls, argc, dest, made);
            return false;
        }
        // A value, in a box (M6.6 part 2): its fields are given their values, and then it never
        // changes. (A generic value type's type arguments are not kept yet.)
        ObjValue* box = newValue(cls);
        box->type = made;
        sp_[-argc - 1] = Value::object(box);
        box->valueOpen = true;
        for (size_t i = 0; i < cls->fieldInits.size(); i++) callWithReceiver(Value::object(box), cls->fieldInits[i]);
        if (cls->init != nullptr) {
            callClosure(cls->init, argc, packed);
            return true;
        }
        box->valueOpen = false;
        if (argc != 0) throw runtimeError(Thrown::IllegalArgumentException, cls->name->utf8() + "() takes no arguments but got " + std::to_string(argc));
        return false;
    }
    if (!cls->inlineLaid.load(std::memory_order_acquire)) layInline(cls);
    ObjInstance* instance = newInstance(cls, captureCount, typeArgs != nullptr);
    for (uint16_t i = 0; i < captureCount; i++) instance->fields()[instance->fieldCount + i] = captures[i];
    if (typeArgs != nullptr) instance->setType(typeOf(kTypeInstance, false, cls, typeArgs->args));
    sp_[-argc - 1] = Value::object(instance);
    instance->constructing = true;
    if (cls->nextEntry != nullptr) {
        // An enum entry: it knows its name before its init runs.
        nameEntry(instance, cls->nextEntry, cls->nextOrdinal);
        cls->nextEntry = nullptr;
    }
    // Declared fields get their initial values before the initializer runs: here, with no call,
    // where the fields' function only gives them constants and each fits (objects plan part 4).
    if (!(cls->fieldsPlanned && fieldsFromPlan(instance, cls->fieldInits[0]->proto, cls->fieldsPlan, nullptr)))
        for (size_t i = 0; i < cls->fieldInits.size(); i++) callWithReceiver(Value::object(instance), cls->fieldInits[i]);
    // An initializer that only gives fields its arguments and constants: given here, with no
    // frame for it, where each fits.
    if (cls->initPlanned && !packed && pendingEnums_ == 0 && argc == cls->init->proto->arity && fieldsFromPlan(instance, cls->init->proto, cls->initPlan, sp_ - argc)) {
        instance->constructing = false;
        sp_ -= argc;
        return false;
    }
    if (cls->init != nullptr) {
        callClosure(cls->init, argc, packed);
        return true;
    }
    instance->constructing = false;
    if (argc != 0) throw runtimeError(Thrown::IllegalArgumentException, cls->name->utf8() + "() takes no arguments but got " + std::to_string(argc));
    return false;
}

// Field values given with no frame (objects plan part 4, 2026-11-20). A function that gives a new
// object's fields their values, as the compiler makes it, is a run of GET_LOCAL 0, a value,
// SET_OWN, POP for each field, the value an argument (GET_LOCAL k) or a constant (CONST, TRUE,
// FALSE, NULL); an initializer ends GET_LOCAL 0, RETURN_INIT and takes each argument once, a
// fields' function ends NULL, RETURN and takes none. Such a function's work is done by construct
// itself, through the caches of those SET_OWNs; any other code in it (a default, an init block,
// a value worked out, a check) and it is called as before. False, nothing planned, where not.
static bool planFieldsOf(VM& vm, const ObjClass* cls, const FunctionProto* p, bool isInit, std::vector<ObjClass::PlannedField>& out) {
    out.clear();
    // (Its caches may not be made yet: a function of a kept unit has them made as it first runs.
    // Until then construct calls it, as it does where a cache has seen nothing.)
    if (p->quickArity != p->arity || p->quickFewest != p->arity || (!isInit && p->arity != 0)) return false;
    const std::vector<uint8_t>& c = p->code;
    auto opAtCode = [&](size_t i) { return i < c.size() ? static_cast<Op>(c[i]) : Op::Pop; };
    auto u16At = [&](size_t i) { return i + 1 < c.size() ? static_cast<int>(c[i] | (c[i + 1] << 8)) : -1; };
    std::vector<bool> taken(static_cast<size_t>(p->arity) + 1, false);
    size_t at = 0;
    for (;;) {
        if (!isInit && opAtCode(at) == Op::Null && opAtCode(at + 1) == Op::Return && at + 2 == c.size()) break;
        if (opAtCode(at) != Op::GetLocal || u16At(at + 1) != 0) return false;
        at += 3;
        if (isInit && opAtCode(at) == Op::ReturnInit && at + 1 == c.size()) break;
        ObjClass::PlannedField planned;
        const Op valueOp = opAtCode(at);
        if (valueOp == Op::GetLocal) {
            const int k = u16At(at + 1);
            if (!isInit || k < 1 || k > p->arity || taken[static_cast<size_t>(k)]) return false;
            taken[static_cast<size_t>(k)] = true;
            planned.argument = k;
            at += 3;
        } else if (valueOp == Op::Const) {
            const int k = u16At(at + 1);
            if (k < 0 || static_cast<size_t>(k) >= p->constants.size()) return false;
            const Value v = p->constants[static_cast<size_t>(k)];
            // A constant the collector need not see: a number, a char, a byte, or a string kept for good.
            if (!(v.isInt() || v.isFloat() || v.isChar() || v.isByte() || (v.isObjType(ObjType::String) && v.as.obj->immortal))) return false;
            planned.constant = v;
            at += 3;
        } else if (valueOp == Op::True || valueOp == Op::False || valueOp == Op::Null) {
            planned.constant = valueOp == Op::True ? Value::boolean(true) : valueOp == Op::False ? Value::boolean(false) : Value::null();
            at += 1;
        } else {
            return false;
        }
        if (opAtCode(at) != Op::SetOwn) return false;
        const int name = u16At(at + 1);
        const int cache = u16At(at + 3);
        if (cache < 0 || cache > 0xFFFF || name < 0 || static_cast<size_t>(name) >= p->constants.size() ||
            !p->constants[static_cast<size_t>(name)].isObjType(ObjType::String))
            return false;
        const Value* const declared = cls->declaredIndex.find(asString(p->constants[static_cast<size_t>(name)]));
        if (declared == nullptr) return false;
        at += 5;
        if (opAtCode(at) != Op::Pop) return false;
        at += 1;
        planned.cache = static_cast<uint16_t>(cache);
        planned.field = static_cast<uint32_t>(declared->as.i);
        out.push_back(planned);
    }
    for (int k = 1; k <= p->arity; k++)
        if (!taken[static_cast<size_t>(k)]) return false;  // an argument given to no field: used otherwise
    (void)vm;
    return true;
}

void VM::seeHeaderInit(ObjClass* cls) {
    cls->initPlanned = cls->fieldsPlanned = false;
    cls->initPlan.clear();
    cls->fieldsPlan.clear();
    if (cls->isValue || cls->isInterface || cls->isAbstract) return;
    // The fields' function, where there is one alone (a subclass's inherited ones as well: called as before).
    if (cls->fieldInits.size() == 1 && planFieldsOf(*this, cls, cls->fieldInits[0]->proto, false, cls->fieldsPlan)) cls->fieldsPlanned = true;
    // The initializer, where it has no required fields to check at its end.
    if (cls->init != nullptr && !cls->anyRequired && planFieldsOf(*this, cls, cls->init->proto, true, cls->initPlan)) cls->initPlanned = true;
}

bool VM::fieldsFromPlan(ObjInstance* instance, const FunctionProto* fn, const std::vector<ObjClass::PlannedField>& plan, const Value* args) {
    if (instance->core != coreId_) return false;
    const std::atomic<uint64_t>* const caches = fn->caches.get();
    if (caches == nullptr) return false;  // not run yet: it makes its caches as it first runs
    const ObjClass* const cls = instance->cls;
    for (const ObjClass::PlannedField& planned : plan)
        if (planned.cache >= fn->cacheCount) return false;
    // Every value first: one that does not fit as its SET_OWN's cache says (no cache yet, an
    // object of another layout, a value of another kind, a type not this one, a Ref or a value
    // kept in the value itself) and the function gives them all instead.
    for (const ObjClass::PlannedField& planned : plan) {
        const uint64_t seen = caches[planned.cache].load(std::memory_order_relaxed);
        const Value& value = planned.argument > 0 ? args[planned.argument - 1] : planned.constant;
        if (seen == 0 || static_cast<uint32_t>(seen >> 32) != instance->shape || value.kind == ValueKind::Ref ||
            value.kind == ValueKind::Small || value.kind == ValueKind::Undefined)
            return false;
        if ((seen & 0x80) != 0) {
            if (value.kind != static_cast<ValueKind>(seen & 3)) return false;  // a payload of its kind
        } else {
            if ((seen & 2) != 0) return false;  // a value kept inside the object: its own way
            // Its declared type, as SET_OWN's rare way checks it (checkFieldAssignment): a class's
            // found by the function the first time, before which the function does it.
            const FieldDecl& d = cls->declared[planned.field];
            const uint8_t type = d.type;
            if (type != 0 && !(value.isNull() && (type & kTypeNullable) != 0)) {
                if ((type & kTypeMask) == kTypeInstance && d.typeClass == nullptr) return false;
                if (!matchesType(value, type, d.typeClass)) return false;
            }
        }
    }
    // Then each written as SET_OWN's quick way writes it: a new object's vals have no value yet.
    uint8_t* const data = instance->payloadData();
    for (const ObjClass::PlannedField& planned : plan) {
        const uint64_t seen = caches[planned.cache].load(std::memory_order_relaxed);
        const Value& value = planned.argument > 0 ? args[planned.argument - 1] : planned.constant;
        uint8_t* const field = reinterpret_cast<uint8_t*>(instance) + ((seen >> 16) & 0xFFFF);
        if ((seen & 0x80) != 0) {
            const int64_t bits = value.kind == ValueKind::Bool ? (value.as.b ? 1 : 0) : value.as.i;
            std::memcpy(field, &bits, 8);
            if ((seen & 0x20) != 0) {
                const uint32_t bit = (seen >> 8) & 0xFF;
                data[bit / 8] |= static_cast<uint8_t>(1u << (bit % 8));
            }
        } else {
            std::memcpy(field, &value, sizeof(Value));
        }
    }
    return true;
}


ObjUpvalue* VM::capturedOf(Value receiver, int k) {
    if (!receiver.isObjType(ObjType::Instance) || k >= asInstance(receiver)->captureCount)
        throw runtimeError(Thrown::InternalError, "invalid bytecode: no such captured variable");
    return asInstance(receiver)->captured(static_cast<uint32_t>(k));
}

// Called when initializer `init` returns: the class it belongs to may have declared fields
// with no initial value, which the initializer was obliged to assign.
void VM::checkRequiredFields(ObjInstance* instance, const ObjClosure* init) {
    // The owner is the highest class in the chain using this initializer; subclasses that
    // merely inherit it cannot have required fields of their own.
    const ObjClass* owner = nullptr;
    for (const ObjClass* k = instance->cls; k != nullptr; k = k->super)
        if (k->init == init) owner = k;
    if (owner == nullptr) return;

    for (ObjString* name : owner->ownRequired)
        if (!instance->hasValue(name))
            throw runtimeError(Thrown::IllegalStateException, owner->name->utf8() + ".init() must set field '" + name->utf8() + "'");
}

// Enforces the field declarations of a class that has any (its own or inherited): only
// declared fields exist, a val can be assigned only once, and a typed field only takes
// values of its type.
void VM::checkFieldAssignment(ObjInstance* instance, ObjString* stored, Value value) {
    ObjClass* cls = instance->cls;
    // A property with accessors keeps its value under "field:" and its name; messages say the name.
    struct Shown {
        std::string text;
        const std::string& utf8() const { return text; }
    };
    const Shown shown{stored->utf8().rfind("field:", 0) == 0 ? stored->utf8().substr(6) : stored->utf8()};
    const Shown* const name = &shown;
    const Value* index = cls->declaredIndex.find(stored);
    if (index == nullptr)
        throw runtimeError(Thrown::UnsupportedOperationException, cls->name->utf8() + " has no field '" + name->utf8() +
                           "'; fields must be declared in the class with val or var");
    size_t at = static_cast<size_t>(index->as.i);

    if (cls->declared[at].isVal && instance->hasValue(stored) &&
        !(cls->declared[at].overrides && instance->constructing))
        throw runtimeError(Thrown::UnsupportedOperationException, "cannot assign to val field '" + name->utf8() + "' of " + cls->name->utf8());

    uint8_t type = cls->declared[at].type;
    if (type == 0) return;
    if (value.isNull() && (type & kTypeNullable) != 0) return;

    if ((type & kTypeMask) == kTypeInstance && cls->declared[at].typeClass == nullptr) {
        // First use of a class-typed field: find out which class its type names.
        TempRoot keepAlive(*this, value);
        Value resolved = call(Value::object(cls->declared[at].typeThunk), 0, nullptr);
        if (!resolved.isObjType(ObjType::Class))
            throw runtimeError(Thrown::ClassCastException, "a type must be a class or a built-in type, not a value of type " + typeName(resolved));
        // (In a parallel section several cores may find this out at once: they all find the same class.)
        cls->declared[at].typeClass = asClass(resolved);
    }

    const ObjClass* typeClass = cls->declared[at].typeClass;
    if ((!matchesType(value, type, typeClass) && !widenedByte(value, type))) checkRange(type, value, "field '" + name->utf8() + "' of " + cls->name->utf8());
    if (!matchesType(value, type, typeClass))
        throw runtimeError(Thrown::ClassCastException, "cannot assign a value of type " + typeName(value) + " to field '" + name->utf8() + "' of " +
                           cls->name->utf8() + ", which is declared as " + declaredTypeText(type, typeClass));
}

// `super.name(...)`: the method of the superclass, or failing that the one that comes with an
// interface the object's class implements (a class with only interfaces has no superclass).
const Value* VM::superMethod(Value super, Value self, const ObjString* name) const {
    if (super.isObjType(ObjType::Class)) {
        if (const Value* method = asClass(super)->methods.find(name)) return method;
    }
    if (const ObjClass* cls = classOf(self)) {
        for (const ObjClass* implemented : cls->interfaces)
            if (const Value* method = implemented->methods.find(name)) return method;
    }
    return nullptr;
}

// Gives an enum entry its name and its place in the list. A getter the enum wrote for `name`
// or `ordinal` stands in for the field; the real ones are kept under names of their own.
// An enum's entries keep their name and their place in the list in fields the class has
// besides those it declares, and in `name` and `ordinal` unless properties of those names
// stand in for them. They are added to the class before its first entry is made.
void VM::declareEntryFields(ObjClass* cls) {
    if (cls->declaredIndex.find(names.entryName) != nullptr) return;
    auto add = [&](ObjString* name) {
        if (cls->declaredIndex.find(name) != nullptr) return;
        FieldDecl field;
        field.name = name;
        field.isVal = true;
        cls->declaredIndex.set(name, Value::integer(static_cast<int64_t>(cls->declared.size())));
        cls->declared.push_back(field);
        fieldAdded(cls, cls->declared.size() - 1);
    };
    add(names.entryName);
    add(names.entryOrdinal);
    if (cls->getters.find(names.name) == nullptr) add(names.name);
    if (cls->getters.find(names.ordinal) == nullptr) add(names.ordinal);
    cls->shape = newShape();
}

void VM::nameEntry(ObjInstance* entry, ObjString* name, int ordinal) {
    const ObjClass* cls = entry->cls;
    if (entry->declOf(names.entryName) == nullptr) throw runtimeError(Thrown::InternalError, "invalid enum declaration");
    entry->setField(names.entryName, Value::object(name));
    entry->setField(names.entryOrdinal, Value::integer(ordinal));
    if (cls->getters.find(names.name) == nullptr) entry->setField(names.name, Value::object(name));
    if (cls->getters.find(names.ordinal) == nullptr) entry->setField(names.ordinal, Value::integer(ordinal));
}

Value VM::classValue(ObjTypeInfo* type) {
    if (classOfFn_.isNull()) return Value::object(type);  // the standard library not loaded yet
    Value args[2] = {Value::object(type), Value::null()};
    return call(classOfFn_, 2, args);
}

Value VM::makeEntry(Value key, Value value) {
    if (mapEntryClass_ == nullptr) {
        ObjList* pair = alloc<ObjList>();
        pair->values() = {key, value};
        return Value::object(pair);
    }
    // A value (D73): a box holding the key and the value, in MapEntry's layout.
    TempRoot keepKey(*this, key), keepValue(*this, value);
    ObjValue* entry = newValue(mapEntryClass_);
    const ValueLayout& layout = layoutOf(mapEntryClass_);
    writeValueField(entry->data() + layout.fields[0].offset, layout.fields[0], mapEntryClass_, key);
    writeValueField(entry->data() + layout.fields[1].offset, layout.fields[1], mapEntryClass_, value);
    return Value::object(entry);
}

// What a list made of a map's keys, values or entries holds, as the map says (M6.6 part 1):
// 0 its keys, 1 its values, 2 its entries; null for a map made without its type.
ObjTypeInfo* VM::mapPartType(const ObjMap* map, int part) {
    if (map->type == nullptr || map->type->args.size() != 2) return nullptr;
    ObjTypeInfo* item = part < 2 ? map->type->args[static_cast<size_t>(part)]
                                 : mapEntryClass_ != nullptr ? typeOf(kTypeInstance, false, mapEntryClass_, map->type->args) : nullptr;
    return item != nullptr ? typeOf(kTypeList, false, nullptr, {item}) : nullptr;
}

Value VM::mapEntries(ObjMap* map) {
    ObjTypeInfo* const type = mapPartType(map, 2);
    ObjList* list = alloc<ObjList>();
    TempRoot keep(*this, Value::object(list));
    for (const ValueMap::Entry& entry : map->map.entries())
        if (entry.live) list->values().push_back(makeEntry(entry.key, entry.value));
    account(list->values().size() * sizeof(Value));
    typeList(list, type);
    return Value::object(list);
}

Value VM::mapKeys(ObjMap* map) {
    ObjTypeInfo* const type = mapPartType(map, 0);
    ObjList* list = alloc<ObjList>();
    for (const ValueMap::Entry& entry : map->map.entries())
        if (entry.live) list->values().push_back(entry.key);
    account(list->values().size() * sizeof(Value));
    typeList(list, type);
    return Value::object(list);
}

// A map's values (G22): a Values, a live view of them as a keys view is of its keys.
Value VM::mapValues(ObjMap* map) {
    ObjTypeInfo* item = mapPartType(map, 1);
    ObjSet* view = newSet(typeOf(kTypeValues, false, nullptr, {item != nullptr && !item->args.empty() ? item->args[0] : typeOf(0, true, nullptr, {})}));
    view->viewOf = map;
    view->values = true;
    return Value::object(view);
}

// A class or an object declared inside the class of `instance`, named through it: `Store.Box`
// and `Store.Inner` where Store is an object (o-m8-nested), as `Outer.Box` is through a class.
// Only a static marked so (kFieldNested), never the class's other statics.
bool VM::nestedThrough(ObjInstance* instance, ObjString* name, Value& out) {
    ObjClass* cls = instance->cls;
    if (cls->staticInit != nullptr || (cls->super != nullptr && cls->super->staticInit != nullptr)) ensureClassInit(cls);
    const Value* at = cls->staticIndex.find(name);
    if (at == nullptr || !cls->staticDeclared[static_cast<size_t>(at->as.i)].nested) return false;
    out = getProperty(Value::object(cls), name);
    return true;
}

// Runs what works out a class's static values, if it has not run yet, superclasses first.
void VM::ensureClassInit(ObjClass* cls) {
    if (cls->super != nullptr && cls->super->staticInit != nullptr) ensureClassInit(cls->super);
    ObjClosure* init = cls->staticInit;
    if (init == nullptr) return;
    // Working out a class's static values changes the class, which every core sees.
    if (cls->core != coreId_)
        throw notOwned("class " + cls->name->utf8() + " is used for the first time here, and its static values are worked out on " +
                       "first use; use it once before the section");
    cls->staticInit = nullptr;  // before running it: what it works out may be asked for meanwhile
    call(Value::object(init), 0, nullptr);
}

// A value for a declared static field has to fit the field's type.
void VM::checkStaticType(ObjClass* owner, FieldDecl& field, Value value) {
    const uint8_t type = field.type;
    if (type == 0) return;
    if (value.isNull() && (type & kTypeNullable) != 0) return;

    if ((type & kTypeMask) == kTypeInstance && field.typeClass == nullptr) {
        TempRoot keepAlive(*this, value);
        Value resolved = call(Value::object(field.typeThunk), 0, nullptr);
        if (!resolved.isObjType(ObjType::Class))
            throw runtimeError(Thrown::ClassCastException, "a type must be a class or a built-in type, not a value of type " + typeName(resolved));
        field.typeClass = asClass(resolved);
    }
    if ((!matchesType(value, type, field.typeClass) && !widenedByte(value, type))) checkRange(type, value, "static field '" + field.name->utf8() + "' of " + owner->name->utf8());
    if (!matchesType(value, type, field.typeClass))
        throw runtimeError(Thrown::ClassCastException, "cannot assign a value of type " + typeName(value) + " to static field '" + field.name->utf8() +
                           "' of " + owner->name->utf8() + ", which is declared as " + declaredTypeText(type, field.typeClass));
}

// `Class.name = value`. A declared static field keeps its rules (a val stays as it is, a type
// is enforced), a static property runs its setter, and any other name is simply stored.
void VM::assignStatic(ObjClass* cls, ObjString* name, Value value) {
    guardWrite(cls);
    if (cls->staticInit != nullptr || (cls->super != nullptr && cls->super->staticInit != nullptr)) {
        TempRoot keepAlive(*this, value);
        ensureClassInit(cls);
    }
    for (ObjClass* k = cls; k != nullptr; k = k->super) {
        if (const Value* index = k->staticIndex.find(name)) {
            FieldDecl& field = k->staticDeclared[static_cast<size_t>(index->as.i)];
            if (field.isVal) throw runtimeError(Thrown::UnsupportedOperationException, "cannot assign to static val '" + name->utf8() + "' of " + k->name->utf8());
            checkStaticType(k, field, value);
            k->statics.set(name, value);
            return;
        }
        if (const Value* setter = k->staticSetters.find(name)) {
            call(*setter, 1, &value);
            return;
        }
        if (k->staticGetters.find(name) != nullptr)
            throw runtimeError(Thrown::IllegalStateException, "static property '" + name->utf8() + "' of " + k->name->utf8() +
                               " cannot be assigned: it has no setter");
    }
    cls->statics.set(name, value);
}

void VM::setProperty(Value obj, ObjString* name, Value value) {
    if (obj.isRef()) {
        setRefField(obj, name, value);  // in place, where the value is
        return;
    }
    // A Ref is copied where the value is kept as it is; one kept inside an object is copied from
    // where it is (M6.6 part 5).
    if (obj.isObjType(ObjType::Value)) {
        setValueField(asValue(obj), name, own(value));  // only while it is being made
        return;
    }
    if (obj.isObjType(ObjType::Instance)) {
        ObjInstance* instance = asInstance(obj);
        guardWrite(instance);
        if (instance->cls->isEnum && (name == names.name || name == names.ordinal))
            throw runtimeError(Thrown::UnsupportedOperationException, "cannot assign to '" + name->utf8() + "' of an entry of enum " + instance->cls->name->utf8());
        if (instance->cls->declaredIndex.find(name) == nullptr && instance->cls->extendField(name) != nullptr) {
            setExtraField(instance, name, own(value));  // a field an extend adds (D157)
            return;
        }
        if (instance->cls->declaredIndex.find(name) == nullptr && instance->cls->setters.find(name) == nullptr)
            if (ObjString* other = missedName(instance->cls, name, obj, true)) {  // o-private-shadow
                setProperty(obj, other, value);
                return;
            }
        checkFieldAssignment(instance, name, value);  // every field is declared (R24)
        if (instance->inlined) {
            const FieldDecl* decl = instance->declOf(name);
            if (decl != nullptr && decl->inlineOffset >= 0) {
                // A value kept inside the object: its bytes copied in (M6.6 part 5).
                const int64_t at = decl->slot;
                const uint32_t offset = static_cast<uint32_t>(decl->inlineOffset);
                if (value.isNull()) {
                    // One that may be null (G16), given null: the slot says so, and the bytes are
                    // cleared, holding nothing for the collector.
                    std::memset(instance->inlineData() + offset, 0, instance->cls->inlineFieldAt(offset)->size);
                    instance->fields()[at] = Value::null();
                    return;
                }
                ValueField whole;
                whole.kind = ValueField::Nested;
                whole.cls = instance->cls->inlineFieldAt(offset)->cls;
                whole.typeSlot = instance->cls->inlineFieldAt(offset)->typeSlot;  // its type kept beside it, where it is (G4)
                writeValueField(instance->inlineData() + offset, whole, instance->cls, value);
                instance->fields()[at] = Value::ref(instance, 0, static_cast<uint16_t>(offset), 0);
                return;
            }
        }
        if (!instance->setField(name, own(value)))
            throw runtimeError(Thrown::UnsupportedOperationException, instance->cls->name->utf8() + " has no field '" + name->utf8() + "'");
    } else if (obj.isObjType(ObjType::Class)) {
        assignStatic(asClass(obj), name, own(value));
    } else {
        throw runtimeError(Thrown::UnsupportedOperationException, "cannot set property '" + name->utf8() + "' on a value of type " + typeName(obj));
    }
}

namespace {

// Resolves a possibly negative index against a length; returns false if out of range.
// A position from the start: a negative one is never one counted from the end (D146).
bool resolveIndex(int64_t index, size_t length, size_t& out) {
    if (index < 0 || static_cast<uint64_t>(index) >= length) return false;
    out = static_cast<size_t>(index);
    return true;
}

}  // namespace

// Where `index` is in `size` items: an int from the start, or ^k from the end (D146); false
// past either end. A negative int is no position, never one counted from the end.
bool VM::position(Value index, size_t size, size_t& out, const char* what) {
    if (index.isFromEnd()) {
        if (static_cast<uint64_t>(index.as.i) > size) return false;
        out = size - static_cast<size_t>(index.as.i);
        return true;
    }
    return resolveIndex(toInt(index, what), size, out);
}

WwThrow VM::outOfRange(const char* what, Value index, size_t size) {
    std::string message = std::string(what) + " " + stringify(index) + " is out of range (size " + std::to_string(size) + ")";
    if (index.isInt() && index.as.i < 0) message += ": a negative number is no position; the last is [^1]";
    return runtimeError(Thrown::IndexOutOfBoundsException, message);
}

// The positions a range in an index takes in `size` items (D147): from `begin` up to `end`,
// left out. Inclusive as `..` is; an end counted from the end resolved; an error past either
// end or running backwards.
void VM::sliceBounds(const ObjRange* range, size_t size, size_t& begin, size_t& end) {
    if (range->step != 1) throw runtimeError(Thrown::IllegalArgumentException, "a slice takes a range that counts up by one, not " + stringify(Value::object(const_cast<ObjRange*>(range))));
    const int64_t n = static_cast<int64_t>(size);
    const int64_t first = range->startFromEnd ? n - range->start : range->start;
    const int64_t last = (range->endFromEnd ? n - range->end : range->end) + (range->inclusive ? 1 : 0);
    if (first < 0 || first > n || last < 0 || last > n || last < first) {   // the range shown only for an error: it costs more than the slice
        const std::string shown = stringify(Value::object(const_cast<ObjRange*>(range)));
        if (first < 0 || first > n || last < 0 || last > n)
            throw runtimeError(Thrown::IndexOutOfBoundsException, "the slice " + shown + " is out of range (size " + std::to_string(size) + ")");
        throw runtimeError(Thrown::IllegalArgumentException, "the slice " + shown + " runs backwards: from " + std::to_string(first) + " to " + std::to_string(last - 1));
    }
    begin = static_cast<size_t>(first);
    end = static_cast<size_t>(last);
}

// A range with an end counted from the end, used where its own numbers are wanted.
WwThrow VM::rangeFromEnd(const ObjRange* range, const std::string& use) {
    const std::string shown = stringify(Value::object(const_cast<ObjRange*>(range)));
    return runtimeError(Thrown::IllegalArgumentException, "the range " + shown + " has an end counted from the end (^), which " + use +
                        " cannot work with: slice with it, items[" + shown + "], or make it plain with of(size)");
}

Value VM::getIndex(Value obj, Value index) {
    if (obj.isObj()) {
        guardRead(obj.as.obj);
        // A range in the index: that part, a copy (D147).
        if (index.isObjType(ObjType::Range) && (obj.as.obj->type == ObjType::List || obj.as.obj->type == ObjType::String ||
                                                obj.as.obj->type == ObjType::Bytes)) {
            size_t begin, end;
            if (obj.isObjType(ObjType::List)) {
                ObjList* list = asList(obj);
                sliceBounds(asRange(index), list->size(), begin, end);
                ObjList* part = listLike(list);
                TempRoot keep(*this, Value::object(part));
                listCopyRange(part, list, begin, end - begin);
                return Value::object(part);
            }
            if (obj.isString()) {
                const ObjString* text = asString(obj);
                sliceBounds(asRange(index), text->length, begin, end);
                return str(text->slice(begin, end));
            }
            const std::vector<uint8_t>& data = asBytes(obj)->data;
            sliceBounds(asRange(index), data.size(), begin, end);
            ObjBytes* part = alloc<ObjBytes>();
            part->data.assign(data.begin() + static_cast<std::ptrdiff_t>(begin), data.begin() + static_cast<std::ptrdiff_t>(end));
            account(part->data.size());
            return Value::object(part);
        }
        switch (obj.as.obj->type) {
            case ObjType::List: {
                ObjList* list = asList(obj);
                size_t i;
                if (!position(index, list->size(), i, "list index")) throw outOfRange("list index", index, list->size());
                return listGet(list, i);
            }
            case ObjType::String: {
                // One character, wherever it is: positions count characters, not bytes.
                const ObjString* text = asString(obj);
                size_t i;
                if (!position(index, text->length, i, "string index")) throw outOfRange("string index", index, text->length);
                return Value::character(text->at(i));  // a char (D24)
            }
            case ObjType::Map: {
                const Value* found = asMap(obj)->map.find(own(index));
                return found != nullptr ? *found : Value::null();
            }
            case ObjType::Bytes: {
                const std::vector<uint8_t>& data = asBytes(obj)->data;
                size_t i;
                if (!position(index, data.size(), i, "bytes index")) throw outOfRange("bytes index", index, data.size());
                // In a parallel section over the parts of a buffer, a core reads its own part only,
                // as for a list: another's would be read while that core writes it (8.7h).
                if (obj.as.obj == bandObject_ && (i < bandFrom_ || i >= bandTo_)) [[unlikely]] throw outsideBand(i);
                return Value::byte(data[i]);  // a byte (D19)
            }
            default:
                break;
        }
    }
    throw runtimeError(Thrown::UnsupportedOperationException, "cannot index a value of type " + typeName(obj));
}

void VM::setIndex(Value obj, Value index, Value value) {
    if (obj.isObjType(ObjType::List)) {
        ObjList* list = asList(obj);
        size_t i;
        if (obj.as.obj != bandObject_) guardWrite(obj.as.obj);
        if (!position(index, list->size(), i, "list index")) throw outOfRange("list index", index, list->size());
        // In a parallel section over the parts of a list, a piece writes its own part of it (D96).
        if (obj.as.obj == bandObject_ && (i < bandFrom_ || i >= bandTo_ || obj.as.obj->frozen != 0)) [[unlikely]] {
            if (obj.as.obj->frozen != 0) guardWrite(obj.as.obj);
            throw outsideBand(i);
        }
        listSet(list, i, value);
    } else if (obj.isObjType(ObjType::Map)) {
        guardWrite(obj.as.obj);
        asMap(obj)->map.set(own(index), own(value));
        account(64);
    } else if (obj.isObjType(ObjType::Bytes)) {
        std::vector<uint8_t>& data = asBytes(obj)->data;
        size_t i;
        if (!position(index, data.size(), i, "bytes index")) throw outOfRange("bytes index", index, data.size());
        if (!value.isByte() && (!value.isInt() || value.as.i < 0 || value.as.i > 255))
            throw runtimeError(Thrown::IllegalArgumentException, "a byte is a whole number from 0 to 255, not " + stringify(value));
        // In a parallel section over the parts of a buffer, a core writes its own part of it.
        if (obj.as.obj->core != coreId_ && !(obj.as.obj == bandObject_ && i >= bandFrom_ && i < bandTo_)) {
            if (obj.as.obj == bandObject_)
                throw notOwned("position " + std::to_string(i) + " is outside this core's part of the buffer, which is " +
                               std::to_string(bandFrom_) + " to " + std::to_string(bandTo_ - 1));
            throw notOwned(obj.as.obj);
        }
        data[i] = static_cast<uint8_t>(value.as.i);
    } else {
        throw runtimeError(Thrown::UnsupportedOperationException, "cannot assign to an index of a value of type " + typeName(obj));
    }
}

// ---- Operators (the cases the interpreter loop does not handle inline) ----

namespace {

// The item type of a collection's type (`int` of `List<int>`), or null where its items may be
// anything: no type, or `any`.
const ObjTypeInfo* itemTypeOf(const ObjTypeInfo* collection) {
    if (collection == nullptr || collection->args.empty() || collection->args[0] == nullptr || collection->args[0]->id == 0) return nullptr;
    return collection->args[0];
}

}  // namespace

bool VM::addsMany(const ObjTypeInfo* itemType, Value right) {
    const bool collection = right.isObjType(ObjType::List) || right.isObjType(ObjType::Set) || right.isObjType(ObjType::Range);
    return collection && !(itemType != nullptr && fitsType(right, itemType));
}

WwThrow VM::notAnItem(const char* kind, const ObjTypeInfo* type, const ObjTypeInfo* itemType, Value item) {
    return runtimeError(Thrown::ClassCastException, std::string("'+' adds to a ") + (type != nullptr ? typeInfoText(type) : kind) + " an item of type " +
                                                        typeInfoText(itemType) + ", or the items of a collection of them; not " + typeName(item));
}

ObjList* VM::listPlus(ObjList* left, Value right) {
    const ObjTypeInfo* itemType = itemTypeOf(left->type);
    if (!addsMany(itemType, right)) {
        if (itemType != nullptr && !fitsType(right, itemType)) throw notAnItem("List", left->type, itemType, right);
        ObjList* made = listLike(left);
        TempRoot keep(*this, Value::object(made));
        listCopyRange(made, left, 0, left->size());
        listAdd(made, right);
        return made;
    }
    if (right.isObjType(ObjType::List)) {
        ObjList* other = asList(right);
        ObjList* joined;
        if (!left->packed() && !other->packed()) {
            // Two lists of 16-byte values, as most are: their items one after the other.
            joined = alloc<ObjList>();
            std::vector<Value>& items = joined->values();
            items.reserve(left->size() + other->size());
            items.insert(items.end(), left->values().begin(), left->values().end());
            items.insert(items.end(), other->values().begin(), other->values().end());
            account(items.size() * sizeof(Value));
            if (left->type == other->type) joined->type = left->type;
        } else if (left->type != nullptr && left->type == other->type) {
            // Two lists of one type: one of that type, packed as they are.
            joined = listLike(left);
            listCopyRange(joined, left, 0, left->size());
            listCopyRange(joined, other, 0, other->size());
        } else {
            joined = alloc<ObjList>();
            std::vector<Value>& items = joined->values();
            items = listCopy(left);
            const std::vector<Value> more = listCopy(other);
            items.insert(items.end(), more.begin(), more.end());
            account(items.size() * sizeof(Value));
        }
        return joined;
    }
    // A set's items or a range's numbers, after the list's own.
    const std::vector<Value> more = itemsOf(right, "list +");
    TempVector keepMore(*this, more);
    for (const Value& item : more)
        if (itemType != nullptr && !fitsType(item, itemType)) throw notAnItem("List", left->type, itemType, item);
    ObjList* made = listLike(left);
    TempRoot keep(*this, Value::object(made));
    listCopyRange(made, left, 0, left->size());
    for (const Value& item : more) listAdd(made, item);
    return made;
}

ObjMap* VM::mapPlus(ObjMap* left, Value right) {
    std::vector<Value> entries;  // key, value, key, value...
    TempVector keepEntries(*this, entries);
    const ObjClass* cls = classOf(right);
    if (right.isObjType(ObjType::Map)) {
        for (const ValueMap::Entry& e : asMap(right)->map.entries())
            if (e.live) {
                entries.push_back(e.key);
                entries.push_back(e.value);
            }
    } else if (cls != nullptr && cls->name->utf8() == "Pair" && cls->origin != nullptr && cls->origin->utf8() == "ww") {
        entries.push_back(componentOf(right, 0));
        entries.push_back(componentOf(right, 1));
    } else {
        throw runtimeError(Thrown::UnsupportedOperationException, "'+' adds to a map a Pair (key to value) or the entries of another map; not " + typeName(right));
    }
    if (left->type != nullptr && left->type->args.size() == 2) {
        for (size_t i = 0; i < entries.size(); i++) {
            const ObjTypeInfo* wanted = left->type->args[i % 2];
            if (wanted != nullptr && wanted->id != 0 && !fitsType(entries[i], wanted))
                throw runtimeError(Thrown::ClassCastException, "'+' adds to a " + typeInfoText(left->type) + (i % 2 == 0 ? " keys" : " values") + " of type " +
                                                                   typeInfoText(wanted) + "; not " + typeName(entries[i]));
        }
    }
    ObjMap* made = alloc<ObjMap>();
    made->type = left->type;
    made->map = left->map;
    TempRoot keep(*this, Value::object(made));
    for (size_t i = 0; i + 1 < entries.size(); i += 2) made->map.set(own(entries[i]), own(entries[i + 1]));
    account(64 * made->map.size());
    return made;
}

bool VM::binarySlow(Op op) {
    Value a = sp_[-2], b = sp_[-1];
    guardRead(a);
    guardRead(b);
    ObjString* symbol = opNames_[static_cast<int>(op)];

    // A collection + an item is a new collection of the same kind with the item added, and + a
    // collection, with its items added, as in Kotlin (the user, 2026-11-07). Which the right side
    // is goes by the left one's item type, as Kotlin's overloads go by it: a List<List<int>> + a
    // list adds the list as one item. A collection on the left comes before text, so
    // ["a"] + "text" is ["a", "text"]; "text" + a list is still text.
    if (op == Op::Add && a.isObjType(ObjType::List)) {
        sp_[-2] = Value::object(listPlus(asList(a), b));
        sp_--;
        return false;
    }
    if (op == Op::Add && a.isObjType(ObjType::Map)) {
        sp_[-2] = Value::object(mapPlus(asMap(a), b));
        sp_--;
        return false;
    }
    // set + x and set - x: a new set, with x or without it, or with or without what a list, a
    // range or another set holds.
    if (a.isObjType(ObjType::Set) && (op == Op::Add || op == Op::Sub)) {
        const ObjSet* from = asSet(a);
        const ObjTypeInfo* itemType = itemTypeOf(from->type);
        if (op == Op::Add && !addsMany(itemType, b) && itemType != nullptr && !fitsType(b, itemType))
            throw notAnItem("Set", from->type, itemType, b);
        ObjSet* made = newSet(from->type);
        TempRoot keep(*this, Value::object(made));
        for (const Value& item : setItems(from)) setAdd(made, item);
        const std::vector<Value> others = addsMany(itemType, b) ? itemsOf(b, "set + and -") : std::vector<Value>{b};
        TempVector keepOthers(*this, others);
        for (const Value& item : others) {
            if (op == Op::Add && itemType != nullptr && !fitsType(item, itemType)) throw notAnItem("Set", from->type, itemType, item);
            if (op == Op::Add) setAdd(made, item);
            else setRemove(made, item);
        }
        sp_[-2] = Value::object(made);
        sp_--;
        return false;
    }

    // A built-in value on the left of an object's: the operator added to its type (op Number.*).
    if (classOf(a) == nullptr && classOf(b) != nullptr) {
        if (const Value* added = addedOperator(a, &sp_[-1], symbol)) {
            callClosure(asClosure(*added), 1);
            return true;
        }
    }

    // A byte in arithmetic is the int it holds, so byte + byte is an int (D19).
    if ((a.isByte() && (b.isNumber() || b.isByte())) || (b.isByte() && a.isNumber())) {
        if (a.isByte()) sp_[-2] = a = Value::integer(a.as.i);
        if (b.isByte()) sp_[-1] = b = Value::integer(b.as.i);
        return numberArithmetic(op);
    }
    // Arithmetic with characters (D25): char + char is a string, char +- int a char,
    // char - char an int, int +- char an int.
    if ((a.isChar() || b.isChar()) && !a.isString() && !b.isString()) {
        const bool plus = op == Op::Add, minus = op == Op::Sub;
        Value result;
        if (plus && a.isChar() && b.isChar()) {
            std::string joined;
            appendUtf8(joined, static_cast<uint32_t>(a.as.i));
            appendUtf8(joined, static_cast<uint32_t>(b.as.i));
            result = str(std::move(joined));
        } else if ((plus || minus) && a.isChar() && b.isInt()) {
            int64_t c;
            if ((plus ? addOverflows(a.as.i, b.as.i, c) : subOverflows(a.as.i, b.as.i, c)) || !isCharacter(c))
                throw runtimeError(Thrown::IllegalArgumentException, quoteCharacter(static_cast<uint32_t>(a.as.i)) + (plus ? " + " : " - ") + std::to_string(b.as.i) +
                                   " is not a character: characters are numbered 0 to 1114111, leaving out 55296 to 57343");
            result = Value::character(static_cast<uint32_t>(c));
        } else if (minus && a.isChar() && b.isChar()) {
            result = Value::integer(a.as.i - b.as.i);
        } else if ((plus || minus) && a.isInt() && b.isChar()) {
            int64_t n;
            if (plus ? addOverflows(a.as.i, b.as.i, n) : subOverflows(a.as.i, b.as.i, n)) throw overflowError(op, a.as.i, b.as.i);
            result = Value::integer(n);
        } else {
            throw runtimeError(Thrown::UnsupportedOperationException, "unsupported operand types for '" + symbol->utf8() + "': " + typeName(a) + " and " + typeName(b));
        }
        sp_[-2] = result;
        sp_--;
        return false;
    }

    if (classOf(a) != nullptr) {
        invokeOperator(a, symbol, 1);
        return true;
    }

    if (op == Op::Add) {
        if (a.isString() || b.isString()) {
            // Joined at the width of the wider part (D20).
            if (a.isString() && b.isString()) {
                sp_[-2] = Value::object(joinStrings(asString(a), asString(b)));
                sp_--;
                return false;
            }
            Characters joined;
            joined.units.reserve((a.isString() ? asString(a)->units().size() : 16) + (b.isString() ? asString(b)->units().size() : 16));
            for (const Value& part : {a, b}) {
                if (part.isString()) {
                    appendText(joined, asString(part));
                } else {
                    std::string shown;
                    stringifyInto(shown, part, false, 0);
                    joined.appendUtf8Text(shown);
                }
            }
            sp_[-2] = str(std::move(joined));
            sp_--;
            return false;
        }
        if (a.isObjType(ObjType::Bytes) && b.isObjType(ObjType::Bytes)) {
            ObjBytes* joined = alloc<ObjBytes>();
            joined->data = asBytes(a)->data;
            joined->data.insert(joined->data.end(), asBytes(b)->data.begin(), asBytes(b)->data.end());
            account(joined->data.size());
            sp_[-2] = Value::object(joined);
            sp_--;
            return false;
        }
    }

    throw runtimeError(Thrown::UnsupportedOperationException, "unsupported operand types for '" + symbol->utf8() + "': " + typeName(a) + " and " + typeName(b));
}

bool VM::compareSlow(Op op) {
    Value a = sp_[-2], b = sp_[-1];
    guardRead(a);
    guardRead(b);
    ObjString* symbol = opNames_[static_cast<int>(op)];

    if (classOf(a) != nullptr) {
        invokeOperator(a, symbol, 1);
        return true;
    }
    // A built-in value on the left of an object: the operator added to its type (op Number.<).
    if (classOf(b) != nullptr) {
        if (const Value* added = addedOperator(a, &sp_[-1], symbol)) {
            callClosure(asClosure(*added), 1);
            return true;
        }
    }

    int order = 0;
    const bool numbers = (a.isNumber() || a.isByte()) && (b.isNumber() || b.isByte());
    if (a.isString() && b.isString()) {
        order = compareText(asString(a), asString(b));
    } else if (a.isChar() && b.isChar()) {
        order = a.as.i < b.as.i ? -1 : a.as.i > b.as.i;
    } else if (numbers) {
        // A byte compared is the number it holds.
        const bool whole = !a.isFloat() && !b.isFloat();
        if (whole) order = a.as.i < b.as.i ? -1 : a.as.i > b.as.i;
        else {
            const double x = a.isFloat() ? a.as.d : static_cast<double>(a.as.i), y = b.isFloat() ? b.as.d : static_cast<double>(b.as.i);
            const bool result = op == Op::Lt ? x < y : op == Op::Le ? x <= y : op == Op::Gt ? x > y : x >= y;
            sp_[-2] = Value::boolean(result);
            sp_--;
            return false;
        }
    }
    if ((a.isString() && b.isString()) || (a.isChar() && b.isChar()) || numbers) {
        bool result = op == Op::Lt ? order < 0 : op == Op::Le ? order <= 0 : op == Op::Gt ? order > 0 : order >= 0;
        sp_[-2] = Value::boolean(result);
        sp_--;
        return false;
    }

    throw runtimeError(Thrown::ClassCastException, "cannot compare " + typeName(a) + " and " + typeName(b) + " with '" + symbol->utf8() + "'");
}

// ---- Declared variable types ----

bool VM::matchesType(const Value& v, uint8_t type, const ObjClass* cls) const {
    if (v.isNull()) return (type & kTypeNullable) != 0 || (type & kTypeMask) == kTypeNull;

    switch (type & kTypeMask) {
        case kTypeNull: return false;  // anything but null
        case kTypeInt: return v.isInt();
        case kTypeFloat: return v.isFloat();
        case kTypeNumber: return v.isNumber();
        case kTypeBool: return v.isBool();
        case kTypeString: return v.isString();
        case kTypeList: return v.isObjType(ObjType::List);
        case kTypeMap: return v.isObjType(ObjType::Map);
        case kTypeSet: return v.isObjType(ObjType::Set) && !asSet(v)->values;
        case kTypeValues: return v.isObjType(ObjType::Set) && asSet(v)->values;
        case kTypeFrozenList: return v.isObjType(ObjType::List) && v.as.obj->frozen != 0;
        case kTypeFrozenMap: return v.isObjType(ObjType::Map) && v.as.obj->frozen != 0;
        case kTypeFrozenSet: return v.isObjType(ObjType::Set) && !asSet(v)->values && v.as.obj->frozen != 0;
        case kTypeRange: return v.isObjType(ObjType::Range);
        case kTypeBytes: return v.isObjType(ObjType::Bytes);
        case kTypeChar: return v.isChar();
        case kTypeByte: return v.isByte();
        case kTypeInt16:
        case kTypeInt32:
        case kTypeUint16:
        case kTypeUint32: {
            if (!v.isInt()) return false;
            int64_t low, high;
            intStorageRange(type & kTypeMask, low, high);
            return v.as.i >= low && v.as.i <= high;
        }
        case kTypeFloat32: return v.isFloat();
        case kTypeIndex: return v.isInt() || v.isFromEnd();
        case kTypeType: return v.isObjType(ObjType::TypeInfo);
        case kTypeFunction:
            return v.isObjType(ObjType::Closure) || v.isObjType(ObjType::Native) ||
                   v.isObjType(ObjType::BoundMethod) || v.isObjType(ObjType::BoundNative);
        case kTypeInstance: {
            const ObjClass* own = classOf(v);
            return own != nullptr ? own->isSubclassOf(cls) : const_cast<VM*>(this)->builtinFitsClass(v, cls);  // G30
        }
        default: return false;
    }
}

// For a class type, takes the class off the stack (the compiler pushed it above the value).
ObjClass* VM::popTypeClass(uint8_t type) {
    if ((type & kTypeMask) != kTypeInstance) return nullptr;
    Value cls = *--sp_;
    if (!cls.isObjType(ObjType::Class))
        throw runtimeError(Thrown::ClassCastException, "a type must be a class or a built-in type, not a value of type " + typeName(cls));
    return asClass(cls);
}

std::string VM::declaredTypeText(uint8_t type, const ObjClass* cls) const {
    return typeText(type, cls != nullptr ? cls->name->utf8() : std::string());
}

WwThrow VM::typeMismatch(const std::string& variable, uint8_t type, const ObjClass* cls, Value v) {
    checkRange(type, v, "'" + variable + "'");
    return runtimeError(Thrown::ClassCastException, "cannot assign a value of type " + typeName(v) + " to '" + variable + "', which is declared as " +
                        declaredTypeText(type, cls));
}

// An int sum, difference, product or quotient whose result does not fit in an int (D27).
WwThrow VM::overflowError(Op op, int64_t a, int64_t b) {
    const char* symbol = op == Op::Add ? "+" : op == Op::Sub ? "-" : op == Op::Mul ? "*" : "/";
    const std::string sum = op == Op::Sub && a == 0 ? "-(" + std::to_string(b) + ")" : std::to_string(a) + " " + symbol + " " + std::to_string(b);
    std::string message = sum + " does not fit in an int, which holds -9223372036854775808 to 9223372036854775807";
    if (op != Op::Div && !(op == Op::Sub && a == 0)) message += std::string("; write &") + symbol + " for an answer that wraps around";
    return runtimeError(Thrown::ArithmeticException, message);
}

// Arithmetic on two numbers, ints or floats, for an operator the interpreter's quick path did
// not take (a byte among them, made an int).
bool VM::numberArithmetic(Op op) {
    const Value a = sp_[-2], b = sp_[-1];
    switch (op) {
        case Op::Shl:
        case Op::Shr:
        case Op::Ushr: return shift(op);
        case Op::BitAnd:
        case Op::BitOr:
        case Op::BitXor: return bitwise(op);
        default: break;
    }
    Value result;
    if (a.isInt() && b.isInt()) {
        const int64_t x = a.as.i, y = b.as.i;
        int64_t n = 0;
        switch (op) {
            case Op::Add: if (addOverflows(x, y, n)) throw overflowError(op, x, y); break;
            case Op::Sub: if (subOverflows(x, y, n)) throw overflowError(op, x, y); break;
            case Op::Mul: if (mulOverflows(x, y, n)) throw overflowError(op, x, y); break;
            case Op::AddWrap: n = static_cast<int64_t>(static_cast<uint64_t>(x) + static_cast<uint64_t>(y)); break;
            case Op::SubWrap: n = static_cast<int64_t>(static_cast<uint64_t>(x) - static_cast<uint64_t>(y)); break;
            case Op::MulWrap: n = static_cast<int64_t>(static_cast<uint64_t>(x) * static_cast<uint64_t>(y)); break;
            case Op::Div:
            case Op::Mod:
                if (y == 0) throw runtimeError(Thrown::ArithmeticException, "division by zero");
                if (op == Op::Div && y == -1 && x == INT64_MIN) throw overflowError(op, x, y);
                n = y == -1 ? (op == Op::Div ? -x : 0) : op == Op::Div ? x / y : x % y;
                break;
            default: throw runtimeError(Thrown::UnsupportedOperationException, "unsupported operand types for '" + opNames_[static_cast<int>(op)]->utf8() + "': int and int");
        }
        result = Value::integer(n);
    } else {
        const double x = a.toDouble(), y = b.toDouble();
        switch (op) {
            case Op::Add: result = Value::number(x + y); break;
            case Op::Sub: result = Value::number(x - y); break;
            case Op::Mul: result = Value::number(x * y); break;
            case Op::Div: result = Value::number(x / y); break;
            case Op::Mod: result = Value::number(std::fmod(x, y)); break;
            default: throw runtimeError(Thrown::UnsupportedOperationException, "unsupported operand types for '" + opNames_[static_cast<int>(op)]->utf8() + "': " + typeName(a) + " and " + typeName(b));
        }
    }
    sp_[-2] = result;
    sp_--;
    return false;
}

// a << b and a >> b on ints. Returns true if an overloaded operator was called instead.
bool VM::shift(Op op) {
    Value a = sp_[-2], b = sp_[-1];
    if (!a.isWhole() || !b.isWhole()) return binarySlow(op);

    int64_t count = b.as.i;
    if (count < 0) throw runtimeError(Thrown::ArithmeticException, "cannot shift by a negative amount (" + std::to_string(count) + ")");

    int64_t result;
    if (op == Op::Shl) {
        // Bits shifted past the top are lost; a count of 64 or more leaves nothing.
        result = count >= 64 ? 0 : static_cast<int64_t>(static_cast<uint64_t>(a.as.i) << count);
    } else if (op == Op::Ushr) {
        // `>>>` brings in zeros, the sign bit among the bits shifted (D44).
        result = count >= 64 ? 0 : static_cast<int64_t>(static_cast<uint64_t>(a.as.i) >> count);
    } else {
        // The sign is kept: negative numbers stay negative, ending at -1.
        if (count >= 64) result = a.as.i < 0 ? -1 : 0;
        else result = a.as.i < 0 ? ~(~a.as.i >> count) : a.as.i >> count;
    }
    sp_[-2] = Value::integer(result);
    sp_--;
    return false;
}

// a & b, a | b and a ^ b on ints. Returns true if an overloaded operator was called instead.
bool VM::bitwise(Op op) {
    Value a = sp_[-2], b = sp_[-1];
    if (!a.isWhole() || !b.isWhole()) return binarySlow(op);

    int64_t result = op == Op::BitAnd ? a.as.i & b.as.i : op == Op::BitOr ? a.as.i | b.as.i : a.as.i ^ b.as.i;
    sp_[-2] = Value::integer(result);
    sp_--;
    return false;
}

// ---- Iteration ----

Value VM::makeIterator(Value iterable) {
    guardRead(iterable);
    if (iterable.isObj()) {
        switch (iterable.as.obj->type) {
            case ObjType::List:
            case ObjType::String:
            case ObjType::Bytes:
                return Value::object(alloc<ObjIterator>(iterable));
            case ObjType::Map:
            case ObjType::Set: {
                // The map's change count as the for begins: a key added or taken out meanwhile
                // stops the for (8.2i), where it would skip items or see one twice.
                ObjIterator* it = alloc<ObjIterator>(iterable);
                it->version = keysOf(iterable).version();
                return Value::object(it);
            }
            case ObjType::Range: {
                if (asRange(iterable)->fromEnd()) throw rangeFromEnd(asRange(iterable), "a for loop");
                ObjIterator* it = alloc<ObjIterator>(iterable);
                it->nextValue = asRange(iterable)->start;
                it->remaining = asRange(iterable)->count();
                return Value::object(it);
            }
            case ObjType::Instance:
            case ObjType::Value: {
                // An Iterator itself (hasNext() and next()), or an object with an iterator() that
                // gives one, or gives something else that can be looped over: a list, say.
                if (isIteratorObject(iterable)) return Value::object(alloc<ObjIterator>(iterable));
                const Value* method = classOf(iterable)->methods.find(names.iterator);
                if (method == nullptr) break;
                TempRoot bound(*this, Value::object(alloc<ObjBoundMethod>(iterable, asClosure(*method))));
                Value inner = call(bound.get(), 0, nullptr);
                if (classOf(inner) != nullptr && !isIteratorObject(inner))
                    throw runtimeError(Thrown::IllegalStateException, "iterator() of " + typeName(iterable) + " has to give an Iterator, a list or another built-in collection");
                TempRoot keep(*this, inner);
                return makeIterator(inner);
            }
            default:
                break;
        }
    }
    throw runtimeError(Thrown::UnsupportedOperationException, "a value of type " + typeName(iterable) + " is not iterable");
}

// Part `index` of a value taken apart (D64): a list's item, or a data object's or a value's
// field, in the order of its header.
Value VM::componentOf(Value whole, size_t index) {
    guardRead(whole);
    static const char* const kOrdinals[] = {"first", "second", "third", "fourth", "fifth"};
    const std::string which = index < 5 ? kOrdinals[index] : std::to_string(index + 1) + "th";
    if (whole.isObjType(ObjType::List)) {
        ObjList* list = asList(whole);
        if (index >= list->size()) throw runtimeError(Thrown::IndexOutOfBoundsException, "cannot take a " + which + " part from a list of " + std::to_string(list->size()));
        return listGet(list, index);
    }
    if (classOf(whole) != nullptr && classOf(whole)->isData) {
        const ObjClass* cls = classOf(whole);
        if (index >= cls->components.size())
            throw runtimeError(Thrown::IndexOutOfBoundsException, "cannot take a " + which + " part from " + cls->name->utf8() + ", which has " +
                               std::to_string(cls->components.size()));
        return getProperty(whole, cls->components[index]);
    }
    throw runtimeError(Thrown::UnsupportedOperationException, "cannot take apart a value of type " + typeName(whole) + ": only a list, a data object or a value has parts");
}

// ---- Sets (M6.6 part 8), and views of a map's keys and entries (D77) ----

ObjSet* VM::newSet(ObjTypeInfo* type) {
    ObjSet* set = alloc<ObjSet>();
    set->type = type;
    return set;
}

// A map's keys, or its entries, as a set that shows the map as it is and cannot be changed.
Value VM::mapView(ObjMap* map, bool entries) {
    // The keys' view is made once and kept (a live view, it is the same whatever it is asked
    // for); not one a core of a parallel section makes, which would be the core's.
    if (!entries && map->keysView != nullptr) return Value::object(map->keysView);
    ObjTypeInfo* item = mapPartType(map, entries ? 2 : 0);
    ObjSet* view = newSet(item != nullptr && !item->args.empty() ? typeOf(kTypeSet, false, nullptr, {item->args[0]}) : nullptr);
    view->viewOf = map;
    view->entries = entries;
    if (!entries && coreId_ == 0) map->keysView = view;
    return Value::object(view);
}

bool VM::setContains(const ObjSet* set, Value item) {
    if (!set->view()) return set->items.find(item) != nullptr;
    if (set->values) {
        // A value is there when some key has it: each looked at in turn.
        for (const ValueMap::Entry& entry : set->viewOf->map.entries())
            if (entry.live && equals(entry.value, item)) return true;
        return false;
    }
    if (!set->entries) return set->viewOf->map.find(item) != nullptr;
    // An entry, a MapEntry or a pair, is there when its key is, with its value.
    if (!item.isObjType(ObjType::Value) && !item.isSmall() && !item.isRef()) return false;
    const ObjClass* cls = classOf(item);
    if (cls == nullptr || cls->components.size() != 2) return false;
    const Value key = componentOf(item, 0), value = componentOf(item, 1);
    const Value* held = set->viewOf->map.find(key);
    return held != nullptr && equals(*held, value);
}

WwThrow VM::viewChanged(const ObjSet* view) {
    return runtimeError(Thrown::UnsupportedOperationException, std::string("a map's ") + (view->values ? "values" : view->entries ? "entries" : "keys") +
                        " show the map and cannot be changed through it: change the map, with remove, removeAll { (k, v) -> } or retainAll");
}

bool VM::setAdd(ObjSet* set, Value item) {
    if (set->view()) throw viewChanged(set);
    const uint32_t hash = ValueMap::hashOf(item);  // once, for the lookup and the insert (G28)
    if (set->items.find(item, hash) != nullptr) return false;
    set->items.addNew(own(item), Value::null(), hash);
    account(64);
    return true;
}

bool VM::setRemove(ObjSet* set, Value item) {
    if (set->view()) throw viewChanged(set);
    return set->items.remove(item);
}

// ---- Keys and frozen collections (D26) ----

// While a key's == or hashCode runs, nothing is collected (VM::keyCalls_).
struct KeyCall {
    VM& vm;
    explicit KeyCall(VM& v) : vm(v) { vm.keyCalls_++; }
    ~KeyCall() { vm.keyCalls_--; }
};

// Whether the class has an == or a hashCode of its own, asked once.
static bool hasOwnKeys(const ObjClass* cls, const Names& names) {
    if (cls->ownKeys == 0)
        const_cast<ObjClass*>(cls)->ownKeys = cls->methods.find(names.opEq) != nullptr || cls->methods.find(names.hashCode) != nullptr ? 2 : 1;
    return cls->ownKeys == 2;
}

bool VM::hashOf(const ObjClass* cls, const Value& v, size_t& out) {
    if (!hasOwnKeys(cls, names)) return false;
    const Value* method = cls->methods.find(names.hashCode);
    if (method == nullptr || !method->isObjType(ObjType::Closure)) return false;
    TempRoot receiver(*this, boxed(v));
    TempRoot bound(*this, Value::object(alloc<ObjBoundMethod>(receiver.get(), asClosure(*method))));
    KeyCall inKey(*this);
    const Value result = call(bound.get(), 0, nullptr);
    if (!result.isInt()) throw runtimeError(Thrown::ClassCastException, cls->name->utf8() + ".hashCode() gave " + typeName(result) + ", not an int");
    out = static_cast<size_t>(result.as.i);
    return true;
}

bool VM::equalOf(const ObjClass* cls, const Value& a, const Value& b, bool& out) {
    if (!hasOwnKeys(cls, names)) return false;
    const Value* method = cls->methods.find(names.opEq);
    if (method == nullptr || !method->isObjType(ObjType::Closure)) return false;
    TempRoot receiver(*this, boxed(a));
    TempRoot other(*this, boxed(b));
    TempRoot bound(*this, Value::object(alloc<ObjBoundMethod>(receiver.get(), asClosure(*method))));
    const Value given = other.get();
    KeyCall inKey(*this);
    out = call(bound.get(), 1, &given).truthy();
    return true;
}

void VM::refuseKey(const Value& key) {
    if (key.isObj() && key.as.obj->frozen == 2)
        throw runtimeError(Thrown::IllegalArgumentException, "this frozen " + typeName(key) + " holds something that can change, so it cannot be a key");
    if (key.isObjType(ObjType::Bytes))
        throw runtimeError(Thrown::IllegalArgumentException, "bytes can change, so they cannot be a key: a frozen list of them can, key.toList().frozen()");
    if (key.isObjType(ObjType::List) || key.isObjType(ObjType::Map) || key.isObjType(ObjType::Set))
        throw runtimeError(Thrown::IllegalArgumentException, "a " + typeName(key) + " can change, so it cannot be a key: a frozen copy can, key.frozen()");
    const std::string name = typeName(key);
    const bool vowel = !name.empty() && std::string("AEIOUaeiou").find(name[0]) != std::string::npos;
    throw runtimeError(Thrown::IllegalArgumentException, (vowel ? "an " : "a ") + name + " can change in what its == looks at, so it cannot be a key");
}

// A frozen collection's values hold frozen copies of the lists, maps and sets they hold, as the
// collection itself does (G27): what it holds shares nothing with what it was made from. Objects
// of classes are left as they are, by identity (D26).
void VM::freezeInside(uint8_t* at, const ValueLayout& layout, int depth) {
    for (uint32_t offset : layout.objects) {
        Obj* held;
        std::memcpy(&held, at + offset, sizeof held);
        if (held == nullptr || (held->type != ObjType::List && held->type != ObjType::Map && held->type != ObjType::Set && held->type != ObjType::Value)) continue;
        const Value made = frozenCopy(Value::object(held), depth + 1);
        std::memcpy(at + offset, &made.as.obj, sizeof made.as.obj);
    }
    for (uint32_t offset : layout.values) {
        Value held;
        std::memcpy(&held, at + offset, sizeof held);
        if (!held.isObj()) continue;
        const Value made = frozenCopy(held, depth + 1);
        std::memcpy(at + offset, &made, sizeof made);
    }
}

ObjTypeInfo* VM::frozenArgs(ObjTypeInfo* type) {
    if (type == nullptr || type->args.empty() || frozenTypeId(unfrozenTypeId(type->id)) == 0) return type;
    std::vector<ObjTypeInfo*> args;
    bool changed = false;
    for (ObjTypeInfo* arg : type->args) {
        args.push_back(frozenWhole(arg));
        changed = changed || args.back() != arg;
    }
    return changed ? typeOf(type->id, type->nullable, type->cls, args) : type;
}

ObjTypeInfo* VM::frozenWhole(ObjTypeInfo* type) {
    if (type == nullptr) return type;
    const uint8_t frozen = frozenTypeId(type->id);
    if (frozen == 0) return frozenArgs(type);  // a frozen one's inside, or anything else as it is
    return typeOf(frozen, type->nullable, type->cls, frozenArgs(type)->args);
}

Value VM::frozenCopy(Value v, int depth) {
    if (!v.isObj() || v.as.obj->frozen != 0) return v;
    if (v.isObjType(ObjType::Value)) {
        // A value in a box of its own: a copy of it, what it holds frozen (G27).
        const ObjValue* box = asValue(v);
        const ValueLayout& layout = layoutOf(box->cls);
        if (layout.objects.empty() && layout.values.empty()) return v;
        if (depth >= kMaxEqualsDepth) throw runtimeError(Thrown::IllegalArgumentException, "frozen(): the " + typeName(v) + " holds itself, or is nested too deeply to copy");
        ObjValue* made = newValue(box->cls);
        TempRoot keep(*this, Value::object(made));
        std::memcpy(made->data(), box->data(), layout.size);
        freezeInside(made->data(), layout, depth);
        return keep.get();
    }
    if (v.isObjType(ObjType::Set) && asSet(v)->values) {
        // A map's values (G22), frozen: a list of them as they are.
        ObjList* list = newList(*this);
        TempRoot keep(*this, Value::object(list));
        list->values() = setItems(asSet(v));
        account(list->values().size() * sizeof(Value));
        return frozenCopy(Value::object(list), depth);
    }
    const ObjType type = v.as.obj->type;
    if (type != ObjType::List && type != ObjType::Map && type != ObjType::Set) return v;
    if (depth >= kMaxEqualsDepth) throw runtimeError(Thrown::IllegalArgumentException, "frozen(): the " + typeName(v) + " holds itself, or is nested too deeply to copy");
    guardRead(v);
    // Whether all it holds could be a key as well.
    bool stable = true;
    auto inner = [&](Value item) {
        const Value made = frozenCopy(item, depth + 1);
        stable = stable && stableKey(made);
        return made;
    };
    if (type == ObjType::List) {
        ObjList* list = asList(v);
        ObjList* made = listLike(list);
        TempRoot keep(*this, Value::object(made));
        made->type = frozenArgs(list->type);
        if (list->packed()) {
            listCopyRange(made, list, 0, list->size());
            if (list->store() == ListStore::Items) {
                if (list->pack->itemClass->unstableKey) stable = false;
                // Each item's lists, maps and sets frozen too (G27), the items as they were copied.
                const ValueLayout& layout = layoutOf(list->pack->itemClass);
                if (!layout.objects.empty() || !layout.values.empty())
                    for (size_t i = 0; i < made->size(); i++) freezeInside(itemAt(made, i), layout, depth);
            }
        } else {
            for (size_t i = 0; i < list->size(); i++) {
                const Value item = inner(list->values()[i]);
                made->values().push_back(item);
            }
        }
        made->frozen = stable ? 1 : 2;
        return keep.get();
    }
    if (type == ObjType::Map) {
        ObjMap* map = asMap(v);
        ObjMap* made = alloc<ObjMap>();
        made->type = map->type;
        made->bundle = map->bundle;
        TempRoot keep(*this, Value::object(made));
        made->type = frozenArgs(map->type);
        // By position: a key's hashCode, run as it goes in, may change the map.
        for (size_t i = 0; i < map->map.entries().size(); i++) {
            const ValueMap::Entry entry = map->map.entries()[i];
            if (!entry.live) continue;
            const Value value = inner(entry.value);
            made->map.set(entry.key, value);
        }
        account(made->map.size() * 2 * sizeof(Value));
        made->frozen = stable ? 1 : 2;
        return keep.get();
    }
    const ObjSet* set = asSet(v);
    ObjSet* made = newSet(set->type);
    TempRoot keep(*this, Value::object(made));
    for (const Value& item : setItems(set)) made->items.set(item, Value::null());
    made->frozen = 1;
    return keep.get();
}

// ---- Lazy values (D37) ----

// Tells the threads waiting for a lazy value that it is there, or that working it out failed.
void VM::wakeLazyWaiters(ObjLazy* lazy) {
    if (lazy->waiters == 0) return;
    const Value key = Value::object(lazy);
    monitorEnter(key);
    monitorNotify(key, true);
    monitorExit(key);
}

Value VM::forceLazy(Value marker, const std::function<void(Value)>& store, const std::function<std::string()>& what) {
    static thread_local char here;
    const uintptr_t me = reinterpret_cast<uintptr_t>(&here);
    ObjLazy* lazy = static_cast<ObjLazy*>(marker.as.obj);
    TempRoot keep(*this, Value::object(lazy));
    // The cores of a parallel section run at once, not in turns: the state is claimed and
    // published as one, the value written before it says 2.
    std::atomic_ref<uint8_t> state(lazy->state);
    std::atomic_ref<uintptr_t> owner(lazy->owner);
    for (;;) {
        const uint8_t now = state.load(std::memory_order_acquire);
        if (now == 2) {
            if (store) store(lazy->value);
            return lazy->value;
        }
        if (now == 1) {
            if (owner.load(std::memory_order_relaxed) == me)
                throw runtimeError(Thrown::IllegalStateException, what() + " is read while its value is being worked out: its starting value needs itself");
            if (inParallelSection()) {
                yieldLock();
                continue;
            }
            // Another thread is working it out: wait until it is told the value is there, or
            // that working it out failed.
            const Value key = Value::object(lazy);
            lazy->waiters++;
            bool interrupted = false;
            monitorEnter(key);
            while (state.load(std::memory_order_acquire) == 1)
                if (monitorWait(key, -1) == 2) interrupted = true;
            monitorExit(key);
            lazy->waiters--;
            if (interrupted) thread_->interrupted = true;  // for whoever asks next
            continue;
        }
        uint8_t unclaimed = 0;
        if (!state.compare_exchange_strong(unclaimed, 1, std::memory_order_acq_rel)) continue;
        owner.store(me, std::memory_order_relaxed);
        Value result;
        try {
            // Made in a parallel section for what every core reads (a global, a static, a shared
            // object's field), it is shared, as made before the section; for this core's own
            // object, its own.
            auto make = [&] { return own(call(Value::object(lazy->init), 0, nullptr)); };
            result = lazy->core != coreId_ ? makeShared(make) : make();
        } catch (...) {
            // A starting value that fails leaves it unset, for the next read to try again.
            owner.store(0, std::memory_order_relaxed);
            state.store(0, std::memory_order_release);
            wakeLazyWaiters(lazy);
            throw;
        }
        lazy->value = result;
        state.store(2, std::memory_order_release);
        wakeLazyWaiters(lazy);
        if (store) store(lazy->value);
        return lazy->value;
    }
}

Value VM::globalOrLazy(size_t index) {
    const Value value = globals_[index];
    if (!value.isLazy()) throw undefinedGlobal(program_->globalNames[index]);
    // A core's table of globals is its own copy: written, as the program's is.
    return forceLazy(value, [&](Value v) { globals_[index] = v; }, [&] { return "'" + program_->globalNames[index] + "'"; });
}

std::vector<Value> VM::setItems(const ObjSet* set) {
    std::vector<Value> out;
    const ValueMap& from = set->view() ? set->viewOf->map : set->items;
    out.reserve(from.size());
    TempVector keep(*this, out);
    for (size_t i = 0; i < from.entries().size(); i++) {
        const ValueMap::Entry& entry = from.entries()[i];
        if (!entry.live) continue;
        out.push_back(set->entries ? makeEntry(entry.key, entry.value) : set->values ? entry.value : entry.key);
    }
    return out;
}

std::vector<Value> VM::itemsOf(Value from, const char* what) {
    if (from.isObjType(ObjType::List)) return listCopy(asList(from));
    if (from.isObjType(ObjType::Set)) return setItems(asSet(from));
    const bool iterable = from.isObjType(ObjType::Range) || from.isString() || from.isObjType(ObjType::Bytes) || from.isObjType(ObjType::Map);
    if (!iterable) throw runtimeError(Thrown::ClassCastException, std::string(what) + " takes a list, a set, a range, a string, bytes or a map, not a value of type " + typeName(from));
    std::vector<Value> out;
    TempVector keep(*this, out);
    const Value iterator = makeIterator(from);
    TempRoot keepIterator(*this, iterator);
    Value item;
    while (iteratorNext(asIterator(iterator), item)) out.push_back(item);
    return out;
}

bool VM::iteratorNext(ObjIterator* it, Value& out) {
    Value source = it->source;
    switch (source.as.obj->type) {
        case ObjType::List: {
            ObjList* list = asList(source);
            if (it->index >= list->size()) return false;
            out = listGet(list, it->index++);
            return true;
        }
        case ObjType::Bytes: {
            const std::vector<uint8_t>& data = asBytes(source)->data;
            if (it->index >= data.size()) return false;
            out = Value::byte(data[it->index++]);
            return true;
        }
        case ObjType::Range: {
            if (it->remaining <= 0) return false;
            it->remaining--;
            out = Value::integer(it->nextValue);
            it->nextValue = static_cast<int64_t>(static_cast<uint64_t>(it->nextValue) + static_cast<uint64_t>(asRange(source)->step));
            return true;
        }
        case ObjType::String: {
            const ObjString* text = asString(source);
            if (it->index >= text->length) return false;
            out = Value::character(text->at(it->index++));
            return true;
        }
        case ObjType::Set: {
            // A set's items; a view's, the map's keys or entries as they are (D77).
            const ObjSet* set = asSet(source);
            checkUnchanged(it);
            const std::vector<ValueMap::Entry>& entries = set->view() ? set->viewOf->map.entries() : set->items.entries();
            while (it->index < entries.size()) {
                const ValueMap::Entry& entry = entries[it->index++];
                if (!entry.live) continue;
                out = set->entries ? makeEntry(entry.key, entry.value) : set->values ? entry.value : entry.key;
                return true;
            }
            return false;
        }
        case ObjType::Map: {
            checkUnchanged(it);
            const std::vector<ValueMap::Entry>& entries = asMap(source)->map.entries();
            while (it->index < entries.size()) {
                const ValueMap::Entry& entry = entries[it->index++];
                if (entry.live) {
                    out = makeEntry(entry.key, entry.value);  // a map gives its entries (D64)
                    return true;
                }
            }
            return false;
        }
        case ObjType::Instance:
        case ObjType::Value: {
            // An Iterator: hasNext(), then next().
            const Value hasNext = memberGet(source, names.hasNext);
            TempRoot keepHas(*this, hasNext);
            if (!call(hasNext, 0, nullptr).truthy()) return false;
            const Value next = memberGet(source, names.next);
            TempRoot keepNext(*this, next);
            out = call(next, 0, nullptr);
            return true;
        }
        default:
            return false;
    }
}

// The error checkUnchanged throws, out of the loops' way.
#ifdef _MSC_VER
__declspec(noinline)
#else
__attribute__((noinline))
#endif
void VM::changedInLoop(const ObjIterator* it) {
    const bool map = it->source.isObjType(ObjType::Map) || asSet(it->source)->view();
    throw runtimeError(Thrown::ConcurrentModificationException,
                       std::string(map ? "the map" : "the set") + " was changed while a for went through it: removeAll { } takes items out as it goes, " +
                           "or go through a copy, toList()");
}

bool VM::isIteratorObject(Value v) const {
    const ObjClass* cls = classOf(v);
    return cls != nullptr && cls->methods.find(names.hasNext) != nullptr && cls->methods.find(names.next) != nullptr;
}

// ---- Upvalues ----

ObjUpvalue* VM::captureUpvalue(Value* slot) {
    // The open list is sorted by stack slot, highest first.
    ObjUpvalue* previous = nullptr;
    ObjUpvalue* upvalue = openUpvalues_;
    while (upvalue != nullptr && upvalue->location > slot) {
        previous = upvalue;
        upvalue = upvalue->nextOpen;
    }
    if (upvalue != nullptr && upvalue->location == slot) return upvalue;

    ObjUpvalue* created = alloc<ObjUpvalue>(slot);
    created->owner = context_->fiber;
    created->nextOpen = upvalue;
    if (previous == nullptr) openUpvalues_ = created;
    else previous->nextOpen = created;
    return created;
}

// Moves every captured variable at or above `last` off the stack and into its upvalue.
void VM::closeUpvalues(Value* last) {
    while (openUpvalues_ != nullptr && openUpvalues_->location >= last) {
        ObjUpvalue* upvalue = openUpvalues_;
        upvalue->closed = own(*upvalue->location);  // a variable kept on, past its frame
        upvalue->location = &upvalue->closed;
        upvalue->owner = nullptr;
        openUpvalues_ = upvalue->nextOpen;
        upvalue->nextOpen = nullptr;
    }
}

// ---- Interpreter loop ----

// What a GetProp place remembers where it read the size of a string, list, map or bytes: no
// class's shape is 0, so no object, value or place of one is taken for it.
constexpr uint64_t kSeenSize = 1;

// The size of a string, list, map or bytes, read at once; else -1.
static inline int64_t quickSize(const Value& v) {
    if (v.kind != ValueKind::Obj) return -1;
    const Obj* obj = v.as.obj;
    switch (obj->type) {
        case ObjType::String: return static_cast<int64_t>(static_cast<const ObjString*>(obj)->length);
        case ObjType::List: return static_cast<int64_t>(static_cast<const ObjList*>(obj)->size());
        case ObjType::Map: return static_cast<int64_t>(static_cast<const ObjMap*>(obj)->map.size());
        case ObjType::Bytes: return static_cast<int64_t>(static_cast<const ObjBytes*>(obj)->data.size());
        default: return -1;
    }
}

#ifdef WW_OP_COUNTS
#include <intrin.h>
// A build for finding where a program's instructions go (M6.5 part 4): how often each runs,
// and each pair one after the other, shown when the program ends.
namespace {
struct OpCounts {
    uint64_t single[kOpCount] = {};
    uint64_t pairs[kOpCount][kOpCount] = {};
    uint64_t cycles[kOpCount] = {};  // the processor's clock from each instruction to the next, by the first
    uint64_t lastTime = 0;
    int last = static_cast<int>(kOpCount);
    ~OpCounts() {
        uint64_t allCycles = 0;
        std::vector<std::pair<uint64_t, std::string>> slow;
        for (int i = 0; i < static_cast<int>(kOpCount); i++) {
            allCycles += cycles[i];
            if (cycles[i] != 0) slow.emplace_back(cycles[i], opName(static_cast<Op>(i)));
        }
        std::sort(slow.rbegin(), slow.rend());
        std::fprintf(stderr, "time, by instruction (with what it calls):\n");
        for (size_t i = 0; i < slow.size() && i < 25; i++) {
            int op = 0;
            while (op < static_cast<int>(kOpCount) - 1 && slow[i].second != opName(static_cast<Op>(op))) op++;
            std::fprintf(stderr, "  %-22s %5.1f%%  %8.1f cycles each\n", slow[i].second.c_str(),
                         100.0 * static_cast<double>(slow[i].first) / static_cast<double>(allCycles),
                         static_cast<double>(slow[i].first) / static_cast<double>(single[op] == 0 ? 1 : single[op]));
        }
        uint64_t total = 0;
        std::vector<std::pair<uint64_t, std::string>> rows;
        for (int i = 0; i < static_cast<int>(kOpCount); i++) {
            total += single[i];
            if (single[i] != 0) rows.emplace_back(single[i], opName(static_cast<Op>(i)));
        }
        std::sort(rows.rbegin(), rows.rend());
        std::fprintf(stderr, "instructions run: %llu\n", static_cast<unsigned long long>(total));
        for (size_t i = 0; i < rows.size() && i < 25; i++)
            std::fprintf(stderr, "  %-22s %12llu  %5.1f%%\n", rows[i].second.c_str(), static_cast<unsigned long long>(rows[i].first),
                         100.0 * static_cast<double>(rows[i].first) / static_cast<double>(total));
        std::vector<std::pair<uint64_t, std::string>> twos;
        for (int a = 0; a < static_cast<int>(kOpCount); a++)
            for (int b = 0; b < static_cast<int>(kOpCount); b++)
                if (pairs[a][b] != 0)
                    twos.emplace_back(pairs[a][b], std::string(opName(static_cast<Op>(a))) + " " + opName(static_cast<Op>(b)));
        std::sort(twos.rbegin(), twos.rend());
        std::fprintf(stderr, "pairs:\n");
        for (size_t i = 0; i < twos.size() && i < 25; i++)
            std::fprintf(stderr, "  %-40s %12llu  %5.1f%%\n", twos[i].second.c_str(), static_cast<unsigned long long>(twos[i].first),
                         100.0 * static_cast<double>(twos[i].first) / static_cast<double>(total));
    }
};
OpCounts opCounts;
inline void countOp(Op which) {
    const size_t op = static_cast<size_t>(which);
    const uint64_t now = __builtin_readcyclecounter();
    if (opCounts.last != static_cast<int>(kOpCount)) opCounts.cycles[opCounts.last] += now - opCounts.lastTime;
    opCounts.lastTime = now;
    opCounts.single[op]++;
    if (opCounts.last != static_cast<int>(kOpCount)) opCounts.pairs[opCounts.last][op]++;
    opCounts.last = op;
}
}  // namespace
#endif

// An instruction's operands, written low byte first, read with one load each where the
// machine keeps its numbers that way (M6.5 part 4).
inline int operandU16(const uint8_t* at) {
    uint16_t v;
    std::memcpy(&v, at, sizeof v);
    if constexpr (std::endian::native == std::endian::big) v = static_cast<uint16_t>((v >> 8) | (v << 8));
    return v;
}
inline int32_t operandI32(const uint8_t* at) {
    uint32_t v;
    std::memcpy(&v, at, sizeof v);
    if constexpr (std::endian::native == std::endian::big)
        v = (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24);
    return static_cast<int32_t>(v);
}

#if WW_TAIL_CALLS
// An instruction's function (VM::tail), whose address the code holds before each instruction.
using TailFn = WW_TAIL_FN int (VM::*)(WW_TAIL_PARAMS);
// The code keeps a function's address, kThreaded bytes. Microsoft's C++ has a member function's
// pointer be that alone; the Itanium C++ of Linux and macOS, that and how far to move `this`,
// which for these, functions of VM itself and none virtual, is always 0 (TailTable::get checks).
static_assert(sizeof(TailFn) == kThreaded || sizeof(TailFn) == 2 * kThreaded, "an instruction's function is one address, or two words");
WW_ALWAYS_INLINE TailFn tailAt(const uint8_t* at) {
    TailFn f;
    if constexpr (sizeof(TailFn) == kThreaded) {
        std::memcpy(&f, at, sizeof f);
    } else {
        uintptr_t parts[2] = {0, 0};
        std::memcpy(&parts[0], at, sizeof(uintptr_t));
        std::memcpy(&f, parts, sizeof f);
    }
    return f;
}
struct TailTable {
    static const TailFn* get();
    static TailFn nativeEntry() { return &VM::nativeEnter; }
    static TailFn nativeFirstEntry() { return &VM::nativeFirst; }
    static TailFn nativeCallEntry() { return &VM::nativeCall; }
    static TailFn nativeReturnEntry() { return &VM::nativeReturn; }
};
namespace {
// Writes into run code each instruction's function (D160), as the code is made: the functions,
// unlike run()'s labels, are known before anything runs, so nothing is left to the loop.
void addressCode(FunctionProto& f) {
    const TailFn* const table = TailTable::get();
    for (size_t at = 0; at < f.run.size();) {
        const char* name = nullptr;
        const size_t size = runSize(f.run, at + kThreaded, name);
        std::memcpy(f.run.data() + at, &table[static_cast<size_t>(opAt(f.run.data() + at + kThreaded))], kThreaded);
        at += kThreaded + size;
    }
    // A loop whose native code regions are not made yet starts with VM::nativeFirst, which makes
    // them; a region's ways in are VM::nativeEnter's (M9.5).
    const TailFn first = TailTable::nativeFirstEntry();
    for (size_t k = 0; k < f.nativeHeads.size(); k++)
        if (f.nativeArrivals[k] < 2 || f.nativeArrivals[k] == 3) std::memcpy(f.run.data() + f.nativeHeads[k], &first, kThreaded);
    // A whole function's start, its calls counted until its region is made (2026-11-15).
    if (f.nativeCallHead != UINT32_MAX) {
        const TailFn call = TailTable::nativeCallEntry();
        std::memcpy(f.run.data() + f.nativeCallHead, &call, kThreaded);
    }
    const TailFn enter = TailTable::nativeEntry();
    if (const auto* regions = f.regionsNow.load(std::memory_order_acquire))
        for (const FunctionProto::NativeRegion& r : *regions) std::memcpy(f.run.data() + r.head, &enter, kThreaded);
    f.addressed.done.store(true, std::memory_order_release);
}
const bool addressesWritten = (writeRunAddresses = &addressCode, true);
}  // namespace


#define ADDRESS_CODE(proto)
#elif defined(__clang__) || defined(__GNUC__)
namespace {
// Where each instruction's code is (D160): its label in run(), or the switch for any without
// one. Filled the first time any thread runs the loop, as a label's address is had only there.
const void* dispatch[kOpCount];
std::mutex addressing;

// Writes into the run code of a function the address of each instruction's code (D160), the
// first time the loop runs it; a thread that finds another doing it waits for it to be done.
void addressCode(const FunctionProto* proto) {
    std::lock_guard<std::mutex> hold(addressing);
    if (proto->addressed.done.load(std::memory_order_relaxed)) return;
    uint8_t* const run = const_cast<uint8_t*>(proto->run.data());
    for (size_t at = 0; at < proto->run.size();) {
        const char* name = nullptr;
        const size_t size = runSize(proto->run, at + kThreaded, name);
        const void* const to = dispatch[static_cast<size_t>(opAt(run + at + kThreaded))];
        std::memcpy(run + at, &to, sizeof to);
        at += kThreaded + size;
    }
    proto->addressed.done.store(true, std::memory_order_release);
}
}  // namespace
#define ADDRESS_CODE(proto) \
    if (!(proto)->addressed.done.load(std::memory_order_acquire)) [[unlikely]] addressCode(proto)
#else
#define ADDRESS_CODE(proto)
#endif

// Its start is kept on a 64-byte line in the Clang build, which the benchmarks are timed with:
// how fast it runs depends on where its jumps fall, and code growing elsewhere in the program
// moved it otherwise (numeric 14% slower for an unrelated change, G42).
#if defined(__clang__)
__attribute__((aligned(64)))
#endif
void VM::run(int baseFrame) {
    // Maps and sets on this thread call this VM for a class's own == and hashCode (D26).
    struct KeysHere {
        KeyMethods* const saved = keyMethods;
        explicit KeysHere(KeyMethods* here) { keyMethods = here; }
        ~KeysHere() { keyMethods = saved; }
    } keysHere(this);
    CallFrame* frame = nullptr;
    const uint8_t* ip = nullptr;
    const uint8_t* code = nullptr;
    Value* base = nullptr;
    const Value* consts = nullptr;
    std::atomic<uint64_t>* caches = nullptr;
#if WW_TAIL_CALLS
    // The instructions, functions of their own, know where this run() started: in the line of
    // execution it runs on, which another thread's turn leaves as it was.
    struct RunBase {
        Context* const context;
        const int saved;
        RunBase(Context* c, int here) : context(c), saved(c->runBase) { context->runBase = here; }
        ~RunBase() { context->runBase = saved; }
    } runBase(context_, baseFrame);
#endif
#define RUN_BASE baseFrame

#define LOAD_FRAME()                                         \
    do {                                                     \
        frame = &frames_[static_cast<size_t>(frameCount_ - 1)]; \
        ip = frame->ip;                                      \
        base = frame->base;                                  \
        code = frame->closure->proto->run.data();            \
        ADDRESS_CODE(frame->closure->proto);                 \
        consts = frame->closure->proto->constants.data();    \
        caches = frame->closure->proto->caches.get();        \
    } while (0)

// Must precede anything that can push a frame or re-enter the VM, so the frame can be
// resumed and shows the right line in a stack trace.
#define SAVE_IP() (frame->ip = ip)

// Before a member is reached by a name written after an object (or a class, for a static
// one): the code may not be allowed to use it, or to assign to it. `bits` is what about the
// name matters here: 1 that some class restricts its use, 2 that some class restricts
// assigning to it. A name no class restricts costs the one test. Of the rest, three cases
// are passed without a call: the instructions the compiler writes where it knows the code
// may (GetOwn and the like, for which `checked` is false); an object whose class restricts
// no name like this one (ObjClass::accessBits); and code written in the class of the object
// it works on, when that class's own code may use all its objects have.
#define CHECK_ACCESS(target, bits, write, checked)                                               \
    do {                                                                                         \
        if (name->restricted & (bits)) [[unlikely]] {                                            \
            const Value target_ = (target);                                                      \
            const FunctionProto* const proto_ = frame->closure->proto;                           \
            const bool own_ = !(checked) ||                                                      \
                              (classOf(target_) != nullptr &&                                    \
                               ((classOf(target_)->accessBits >> (name->hash() & 63) & 1) == 0 ||  \
                                (classOf(target_)->number == proto_->ownClass &&                 \
                                 (classOf(target_)->accessAllOwn ||                              \
                                  (classOf(target_)->accessSharedOwn && (name->restricted & 4) == 0))))); \
            if (!own_ && accessDeniedAt(target_, name, proto_, (write), ip)) {                   \
                SAVE_IP();                                                                       \
                throw accessError(target_, name, proto_);                                        \
            }                                                                                    \
        }                                                                                        \
    } while (0)

#define READ_U8() (*ip++)
#define READ_U16() (ip += 2, operandU16(ip - 2))
#define READ_I32() (ip += 4, operandI32(ip - 4))

// Where the collector may run, and where a thread gives way to another.
#define GC_SAFE_POINT()                                                                    \
    do {                                                                                   \
        if (bytesAllocated_ > nextGC_ || attention_.load(std::memory_order_relaxed)) {         \
            SAVE_IP(); /* where it is, for a sample (ww --profile) */                      \
            safePoint();                                                                   \
        }                                                                                  \
    } while (0)

// The fast path's operands, read where the instruction is, before ip moves past it.
#define FAST_U16(at) operandU16(ip + (at))
#define FAST_I32(at) operandI32(ip + (at))

// A call of a closure that needs nothing done before it starts (M6.5 part 4): as many arguments
// as it has parameters, none gathered into a list, no collection or other thread waiting, and
// room for its frame. Its frame is pushed here and the loop goes on in it; anything else falls
// through to the instruction's own code. `size` is the length of the instruction's operands.
#define QUICK_CALL(callee, argc, size)                                                                        \
    {                                                                                                         \
        ObjClosure* const quick_ = (callee);                                                                  \
        const FunctionProto* const proto_ = quick_->proto;                                                    \
        Value* const base_ = sp - (argc) - 1;                                                                 \
        if ((argc) <= proto_->quickArity && (argc) >= proto_->quickFewest && pendingEnums_ == 0 &&            \
            frameCount_ < frameLimit_ && stackEnd_ - base_ >= proto_->maxStack && bytesAllocated_ <= nextGC_ &&\
            !attention_.load(std::memory_order_relaxed)) {                                                    \
            /* Arguments left out are marked, and the function fills in their defaults. */                   \
            for (int left_ = (argc); left_ < proto_->quickArity; left_++) *sp++ = Value::undefined();         \
            frame->ip = ip + (size);                                                                          \
            CallFrame& next_ = frames_[static_cast<size_t>(frameCount_++)];                                   \
            next_.dropResult = 0; next_.repeats = 0;                                                                             \
            next_.closure = quick_;                                                                           \
            next_.ip = proto_->run.data();                                                                    \
            next_.base = base_;                                                                               \
            /* Where its result goes, for a function that takes it; for any other, none (M6.6 part 4). */  \
            if (proto_->typeArgs != 0) frameDestination(frameCount_ - 1) = pendingDestination_;               \
            pendingDestination_ = Value::null();                                                              \
            /* The new frame's registers from what is at hand, not read back from it (LOAD_FRAME). */      \
            frame = &next_;                                                                                   \
            ip = proto_->run.data();                                                                          \
            base = base_;                                                                                     \
            code = ip;                                                                                        \
            ADDRESS_CODE(proto_);                                                                             \
            consts = proto_->constants.data();                                                                \
            caches = proto_->caches.get();                                                                    \
            NEXT();                                                                                           \
        }                                                                                                     \
    }

// Each instruction's case in the loop's switch, and with Clang or GCC its label as well, which
// the dispatch table points to (D160). MSVC, which has no computed goto, goes by the switch alone.
#if !WW_TAIL_CALLS && (defined(__clang__) || defined(__GNUC__))
#define WW_GOTO_DISPATCH 1
#define CASE(name) \
    case Op::name: \
    op_##name:
#else
#define WW_GOTO_DISPATCH 0
#define CASE(name) case Op::name:
#endif

// Nothing, but different at each place it is written, so that the compiler does not merge the
// ends of instructions into one: each instruction's own jump to the next is predicted apart from
// the others' (D160), which is what makes going on from each instruction pay. (With the address
// read from the code, clang merged them, and numeric was 10% slower than with the table.)
#define WW_APART() asm volatile("" ::"i"(__COUNTER__))

// An instruction's ways out in instructions.inc, as the switch's cases have them (see there).
#define DONE() break
#define EXIT_RUN() return
#define FALL_INTO(op) [[fallthrough]]
#define HAS_COLD(op)
#define GO_COLD(op) goto cold_##op
#define COLD(op) cold_##op:
#define JUMP_LABEL(l) goto l
#define LABEL(l) l:
// CASES(a, b, ...): CASE(a) CASE(b) ..., up to 16 (WW_EXPAND for MSVC's preprocessor).
#define WW_EXPAND(x) x
#define WW_CAT_(a, b) a##b
#define WW_CAT(a, b) WW_CAT_(a, b)
#define WW_COUNT_(_1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11, _12, _13, _14, _15, _16, n, ...) n
#define WW_COUNT(...) WW_EXPAND(WW_COUNT_(__VA_ARGS__, 16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1))
#define WW_EACH_1(m, a) m(a)
#define WW_EACH_2(m, a, ...) m(a) WW_EXPAND(WW_EACH_1(m, __VA_ARGS__))
#define WW_EACH_3(m, a, ...) m(a) WW_EXPAND(WW_EACH_2(m, __VA_ARGS__))
#define WW_EACH_4(m, a, ...) m(a) WW_EXPAND(WW_EACH_3(m, __VA_ARGS__))
#define WW_EACH_5(m, a, ...) m(a) WW_EXPAND(WW_EACH_4(m, __VA_ARGS__))
#define WW_EACH_6(m, a, ...) m(a) WW_EXPAND(WW_EACH_5(m, __VA_ARGS__))
#define WW_EACH_7(m, a, ...) m(a) WW_EXPAND(WW_EACH_6(m, __VA_ARGS__))
#define WW_EACH_8(m, a, ...) m(a) WW_EXPAND(WW_EACH_7(m, __VA_ARGS__))
#define WW_EACH_9(m, a, ...) m(a) WW_EXPAND(WW_EACH_8(m, __VA_ARGS__))
#define WW_EACH_10(m, a, ...) m(a) WW_EXPAND(WW_EACH_9(m, __VA_ARGS__))
#define WW_EACH_11(m, a, ...) m(a) WW_EXPAND(WW_EACH_10(m, __VA_ARGS__))
#define WW_EACH_12(m, a, ...) m(a) WW_EXPAND(WW_EACH_11(m, __VA_ARGS__))
#define WW_EACH_13(m, a, ...) m(a) WW_EXPAND(WW_EACH_12(m, __VA_ARGS__))
#define WW_EACH_14(m, a, ...) m(a) WW_EXPAND(WW_EACH_13(m, __VA_ARGS__))
#define WW_EACH_15(m, a, ...) m(a) WW_EXPAND(WW_EACH_14(m, __VA_ARGS__))
#define WW_EACH_16(m, a, ...) m(a) WW_EXPAND(WW_EACH_15(m, __VA_ARGS__))
#define WW_EACH(m, ...) WW_EXPAND(WW_CAT(WW_EACH_, WW_COUNT(__VA_ARGS__))(m, __VA_ARGS__))
#define CASES(...) WW_EACH(CASE, __VA_ARGS__)

// The end of an instruction that goes on to the next at once: with Clang or GCC, straight to the
// next one's code from here, as the loop's head would; with MSVC, round the loop.
#if WW_GOTO_DISPATCH && !defined(WW_OP_COUNTS)
#define NEXT()                                \
    do {                                      \
        sp_ = sp;                             \
        const void* to_;                      \
        std::memcpy(&to_, ip, sizeof to_);    \
        frame->ip = ip += kThreaded + kOpBytes;      \
        WW_APART();                        \
        goto* to_;                            \
    } while (0)
#else
#define NEXT() continue
#endif

// ---- R instructions (M6.7, registers.cpp) ----
// After the opcode: their operands, a byte each, and the start of the slow way: where the quick way
// does not do, the operands are put where the stack would have them, from slot S, the top after
// them, and the stack code the instruction was made from runs, put after the function's own code,
// which then comes back. An operand is a slot, or a constant where the instruction's name says so.
// They are read where they are, a field at a time, not copied whole: a value just written in two
// stores is handed on by the processor to a read no wider than each, and waited for by one wider.
#define R_S(i) base[ip[i]]
#define R_K(i) consts[ip[i]]
// u8 A, u8 B, u8 C, u8 T, u8 S, i32 slow.
#define R_DONE()            \
    {                       \
        sp = base + ip[3];  \
        ip += 9;            \
        NEXT();             \
    }
#define R_SLOW2(b, c, sAt, slowAt)               \
    {                                            \
        const Value b_ = (b), c_ = (c);          \
        Value* const at_ = base + ip[sAt];       \
        at_[0] = b_;                             \
        at_[1] = c_;                             \
        sp = at_ + 2;                            \
        ip = code + operandI32(ip + (slowAt));   \
        NEXT();                                  \
    }
// An R instruction whose result goes where the stack would have had it: its operands put there,
// the instruction passed over, and the stack instruction's own way of doing it run, here.
#define R_PLAIN2(b, c, plain)                    \
    if (ip[0] == ip[4]) {                        \
        const Value b_ = (b), c_ = (c);          \
        Value* const at_ = base + ip[4];         \
        at_[0] = b_;                             \
        at_[1] = c_;                             \
        sp = sp_ = at_ + 2;                      \
        ip += 9;                                 \
        JUMP_LABEL(plain);                       \
    }
// Two floats or two ints worked out at once.
#define R_ARITHMETIC(name, LB, LC, op, checked, plain)       \
    CASE(name) {                                             \
        const Value &b = LB(1), &c = LC(2);                  \
        if (b.isFloat() && c.isFloat()) {                    \
            base[ip[0]].setFloat(b.as.d op c.as.d);          \
            R_DONE()                                         \
        }                                                    \
        if (b.isWhole() && c.isWhole()) {                    \
            int64_t r_;                                      \
            if (!checked(b.as.i, c.as.i, r_)) {              \
                base[ip[0]].setInt(r_);                      \
                R_DONE()                                     \
            }                                                \
        } else if (b.isNumber() && c.isNumber()) {           \
            /* an int and a float: a float, as plainly */     \
            base[ip[0]].setFloat(b.toDouble() op c.toDouble()); \
            R_DONE()                                         \
        }                                                    \
        R_PLAIN2(b, c, plain)                                \
        R_SLOW2(b, c, 4, 5)                                  \
    }
// A division or a remainder of two floats, or of two whole numbers, worked out at once; a whole
// number divided by 0 or by -1 the plain way, which has the rules for them.
#define R_DIVIDE(name, LB, LC, floatExpr, intExpr, plain)    \
    CASE(name) {                                             \
        const Value &b = LB(1), &c = LC(2);                  \
        if (b.isFloat() && c.isFloat()) {                    \
            const double x = b.as.d, y = c.as.d;             \
            base[ip[0]].setFloat(floatExpr);                 \
            R_DONE()                                         \
        }                                                    \
        if (b.isWhole() && c.isWhole() && static_cast<uint64_t>(c.as.i) + 1 > 1) { \
            const int64_t x = b.as.i, y = c.as.i;            \
            base[ip[0]].setInt(intExpr);                     \
            R_DONE()                                         \
        }                                                    \
        R_PLAIN2(b, c, plain)                                \
        R_SLOW2(b, c, 4, 5)                                  \
    }
// Whether `b cmp c` is told at once, into `holds`: two floats, two ints, two characters by their
// numbers; and for ==, two booleans, strings and characters, or null and anything.
#define R_COMPARED(b, c, cmp, equality, holds)                                                           \
    ((b).isFloat() && (c).isFloat()   ? ((holds) = (b).as.d cmp (c).as.d, true)                          \
     : ((b).isWhole() && (c).isWhole()) || ((b).isChar() && (c).isChar()) ? ((holds) = (b).as.i cmp (c).as.i, true) \
     : !(equality)                    ? false                                                            \
     : (b).isBool() && (c).isBool()   ? ((holds) = (b).as.b == (c).as.b, true)                           \
     : ((b).isString() || (b).isChar()) && ((c).isString() || (c).isChar()) ? ((holds) = valuesEqual((b), (c)), true) \
     : (b).isNull()                   ? ((holds) = (c).isNull(), true)                                   \
                                      : false)
#define R_COMPARISON(name, LB, LC, cmp, plain, equality)     \
    CASE(name) {                                             \
        const Value &b = LB(1), &c = LC(2);                  \
        bool holds_ = false;                                 \
        if (R_COMPARED(b, c, cmp, equality, holds_)) {       \
            base[ip[0]].setBool(holds_);                     \
            R_DONE()                                         \
        }                                                    \
        R_PLAIN2(b, c, plain)                                \
        R_SLOW2(b, c, 4, 5)                                  \
    }
// Two ints worked out at once, `ok` saying where (a shift's count in range); anything else the
// slow way, whose stack instruction reads which it is from its own opcode.
#define R_INTS(name, LB, LC, ok, expr)                       \
    CASE(name) {                                             \
        const Value &b = LB(1), &c = LC(2);                  \
        if (b.isWhole() && c.isWhole() && (ok)) {            \
            const int64_t x = b.as.i, y = c.as.i;            \
            base[ip[0]].setInt(expr);                        \
            R_DONE()                                         \
        }                                                    \
        R_SLOW2(b, c, 4, 5)                                  \
    }
// u8 B, u8 C, u8 T, i32 target, i32 slow: compares and jumps where the comparison does not hold;
// the stack would have had B at T.
#define R_JUMP(name, LB, LC, cmp, equality)                  \
    CASE(name) {                                             \
        const Value &b = LB(0), &c = LC(1);                  \
        bool holds_ = false;                                 \
        if (!R_COMPARED(b, c, cmp, equality, holds_)) R_SLOW2(b, c, 2, 7) \
        sp = base + ip[2];                                   \
        if (holds_) ip += 11;                                \
        else ip = code + operandI32(ip + 3);                 \
        NEXT();                                              \
    }

// Whether `v` is certainly of `type`, told at once for the types written most; false means only
// that the full test (matchesType) has to be made.
// On operands the checker knows (M6.7 step 5), testing nothing of them: two ints, whose
// overflow alone goes the slow way (which says so); two floats; a comparison of either, and one
// with the JumpIfFalse after it.
#define R_INT_ARITHMETIC(name, LB, LC, op, checked, plain)   \
    CASE(name) {                                             \
        const Value &b = LB(1), &c = LC(2);                  \
        int64_t r_;                                          \
        if (!checked(b.as.i, c.as.i, r_)) {                  \
            base[ip[0]].setInt(r_);                          \
            R_DONE()                                         \
        }                                                    \
        R_PLAIN2(b, c, plain)                                \
        R_SLOW2(b, c, 4, 5)                                  \
    }
#define R_FLOAT_ARITHMETIC(name, LB, LC, op)                 \
    CASE(name) {                                             \
        base[ip[0]].setFloat(LB(1).as.d op LC(2).as.d);      \
        R_DONE()                                             \
    }
#define R_TYPED_COMPARISON(name, LB, LC, part, cmp)          \
    CASE(name) {                                             \
        base[ip[0]].setBool(LB(1).as.part cmp LC(2).as.part); \
        R_DONE()                                             \
    }
#define R_TYPED_JUMP(name, LB, LC, part, cmp)                \
    CASE(name) {                                             \
        const bool holds_ = LB(0).as.part cmp LC(1).as.part; \
        sp = base + ip[2];                                   \
        if (holds_) ip += 11;                                \
        else ip = code + operandI32(ip + 3);                 \
        NEXT();                                              \
    }

#define FITS_AT_ONCE(v, type) ((type) == kTypeFloat ? (v).isFloat() : (type) == kTypeInt ? (v).isInt() : (type) == kTypeBool && (v).isBool())

// Two operands, `a` kept where it is, on the stack's top held in a register: two floats or two
// ints are worked out at once, and `done` ends the instruction; anything else falls through to
// the instruction's own code, the stack as it was (M6.5 part 4).
// Two ints whose result does not fit (D27) fall through as well, to be told so.
#define FAST_ARITHMETIC(a, b, op, checked, done)                              \
    if ((a).isFloat() && (b).isFloat()) {                                     \
        (a).setFloat((a).as.d op (b).as.d);                                   \
        done;                                                                 \
    } else if ((a).isWhole() && (b).isWhole()) {                              \
        int64_t sum_;                                                         \
        if (!checked((a).as.i, (b).as.i, sum_)) {                             \
            (a).setInt(sum_);                                                 \
            done;                                                             \
        }                                                                     \
    }
#define FAST_FLOAT(a, b, op, done)                                            \
    if ((a).isFloat() && (b).isFloat()) {                                     \
        (a).setFloat((a).as.d op (b).as.d);                                   \
        done;                                                                 \
    }
#define FAST_COMPARISON(a, b, cmp, done)                                      \
    if ((a).isFloat() && (b).isFloat()) {                                     \
        (a).setBool((a).as.d cmp (b).as.d);                                   \
        done;                                                                 \
    } else if ((a).isWhole() && (b).isWhole()) {                              \
        (a).setBool((a).as.i cmp (b).as.i);                                   \
        done;                                                                 \
    }

#define ARITHMETIC(op, checked, floatExpr)                                 \
    {                                                                     \
        Value b = sp_[-1], a = sp_[-2];                                   \
        if (a.isWhole() && b.isWhole()) {                                 \
            int64_t sum_;                                                 \
            if (checked(a.as.i, b.as.i, sum_)) throw overflowError(op, a.as.i, b.as.i); \
            sp_[-2].setInt(sum_);                                         \
            sp_--;                                                        \
        } else if (a.isNumber() && b.isNumber()) {                        \
            double x = a.toDouble(), y = b.toDouble();                    \
            sp_[-2].setFloat(floatExpr);                                  \
            sp_--;                                                        \
        } else {                                                          \
            SAVE_IP();                                                    \
            if (binarySlow(op)) LOAD_FRAME();                             \
        }                                                                 \
        DONE();                                                          \
    }

#define COMPARISON(op, cmp)                                               \
    {                                                                     \
        Value b = sp_[-1], a = sp_[-2];                                   \
        if (a.isWhole() && b.isWhole()) {                                 \
            sp_[-2].setBool(a.as.i cmp b.as.i);                           \
            sp_--;                                                        \
        } else if (a.isNumber() && b.isNumber()) {                        \
            sp_[-2].setBool(a.toDouble() cmp b.toDouble());               \
            sp_--;                                                        \
        } else {                                                          \
            SAVE_IP();                                                    \
            if (compareSlow(op)) LOAD_FRAME();                            \
        }                                                                 \
        DONE();                                                          \
    }

    // An error the program may catch: the innermost handler that takes it gets it, or, with
    // none in this run, it goes on to whoever called. Returns false in that case.
    // The frame that was running knows where it was: each instruction, as it starts, writes down
    // where it is (M6.5 part 4), so that the loop's own `ip` can stay in a register.
    auto handled = [&](WwThrow& thrown) {
        if (thrown.limit) {  // a script's limit: no catch or finally of this run takes it
            while (!handlers_.empty() && handlers_.back().frameIndex >= baseFrame) handlers_.pop_back();
            unwindTo(baseFrame);
            return false;
        }
        if (!thrown.hasTrace && handlers_.empty()) {
            thrown.trace = captureTrace();
            thrown.hasTrace = true;
        }
        if (tryHandle(thrown.value, baseFrame)) return true;
        unwindTo(baseFrame);
        return false;
    };

#if WW_GOTO_DISPATCH
    // The table of where each instruction's code is, above, filled the first time.
    static std::atomic<bool> dispatchMade{false};
    if (!dispatchMade.load(std::memory_order_acquire)) {
        static std::mutex filling;
        std::lock_guard<std::mutex> hold(filling);
        if (!dispatchMade.load(std::memory_order_relaxed)) {
            for (const void*& at : dispatch) at = &&viaSwitch;
#define WW_DISPATCH_TO(name) dispatch[static_cast<size_t>(Op::name)] = &&op_##name;
            WW_DISPATCH_TO(Const)
            WW_DISPATCH_TO(Null)
            WW_DISPATCH_TO(True)
            WW_DISPATCH_TO(False)
            WW_DISPATCH_TO(Pop)
            WW_DISPATCH_TO(Dup)
            WW_DISPATCH_TO(Dup2)
            WW_DISPATCH_TO(GetLocal)
            WW_DISPATCH_TO(SetLocal)
            WW_DISPATCH_TO(GetGlobal)
            WW_DISPATCH_TO(SetGlobal)
            WW_DISPATCH_TO(DefineGlobal)
            WW_DISPATCH_TO(GetImplicit)
            WW_DISPATCH_TO(TryImplicit)
            WW_DISPATCH_TO(InvokeImplicit)
            WW_DISPATCH_TO(SetImplicit)
            WW_DISPATCH_TO(DefineTypedGlobal)
            WW_DISPATCH_TO(IndexN)
            WW_DISPATCH_TO(SetIndexN)
            WW_DISPATCH_TO(BundleValues)
            WW_DISPATCH_TO(MergeBundle)
            WW_DISPATCH_TO(DupN)
            WW_DISPATCH_TO(SettleEnum)
            WW_DISPATCH_TO(CheckParam)
            WW_DISPATCH_TO(CheckReturn)
            WW_DISPATCH_TO(CheckType)
            WW_DISPATCH_TO(GetUpvalue)
            WW_DISPATCH_TO(GetUpvalueChecked)
            WW_DISPATCH_TO(MakeType)
            WW_DISPATCH_TO(CachedType)
            WW_DISPATCH_TO(CacheType)
            WW_DISPATCH_TO(TypeCollection)
            WW_DISPATCH_TO(IsFull)
            WW_DISPATCH_TO(CastFull)
            WW_DISPATCH_TO(WithTypes)
            WW_DISPATCH_TO(FrameTypes)
            WW_DISPATCH_TO(TypeArg)
            WW_DISPATCH_TO(InstanceTypeArg)
            WW_DISPATCH_TO(SupertypeMaker)
            WW_DISPATCH_TO(CheckResult)
            WW_DISPATCH_TO(ClassOf)
            WW_DISPATCH_TO(ClassMeta)
            WW_DISPATCH_TO(AtomicField)
            WW_DISPATCH_TO(GetCaptured)
            WW_DISPATCH_TO(SetCaptured)
            WW_DISPATCH_TO(ObjectClass)
            WW_DISPATCH_TO(StoreClass)
            WW_DISPATCH_TO(ObjectClassTyped)
            WW_DISPATCH_TO(StoreClassTyped)
            WW_DISPATCH_TO(Capture)
            WW_DISPATCH_TO(NewObject)
            WW_DISPATCH_TO(Unset)
            WW_DISPATCH_TO(SetUpvalue)
            WW_DISPATCH_TO(GetOwn)
            WW_DISPATCH_TO(GetProp)
            WW_DISPATCH_TO(GetOwnRef)
            WW_DISPATCH_TO(GetPropRef)
            WW_DISPATCH_TO(SetOwn)
            WW_DISPATCH_TO(SetProp)
            WW_DISPATCH_TO(GetIndexRef)
            WW_DISPATCH_TO(OwnLocal)
            WW_DISPATCH_TO(GetIndex)
            WW_DISPATCH_TO(SetIndex)
            WW_DISPATCH_TO(SetSuper)
            WW_DISPATCH_TO(GetSuper)
            WW_DISPATCH_TO(Add)
            WW_DISPATCH_TO(Sub)
            WW_DISPATCH_TO(Mul)
            WW_DISPATCH_TO(StoreLocal)
            WW_DISPATCH_TO(CopyLocal)
            WW_DISPATCH_TO(CheckLocal)
            WW_DISPATCH_TO(Div)
            WW_DISPATCH_TO(Mod)
            WW_DISPATCH_TO(AddWrap)
            WW_DISPATCH_TO(SubWrap)
            WW_DISPATCH_TO(MulWrap)
            WW_DISPATCH_TO(Shl)
            WW_DISPATCH_TO(Shr)
            WW_DISPATCH_TO(Ushr)
            WW_DISPATCH_TO(BitAnd)
            WW_DISPATCH_TO(BitOr)
            WW_DISPATCH_TO(BitXor)
            WW_DISPATCH_TO(BitNot)
            WW_DISPATCH_TO(Neg)
            WW_DISPATCH_TO(Not)
            WW_DISPATCH_TO(Eq)
            WW_DISPATCH_TO(Identical)
            WW_DISPATCH_TO(Lt)
            WW_DISPATCH_TO(Le)
            WW_DISPATCH_TO(Gt)
            WW_DISPATCH_TO(Ge)
            WW_DISPATCH_TO(Is)
            WW_DISPATCH_TO(Jump)
            WW_DISPATCH_TO(Loop)
            WW_DISPATCH_TO(JumpIfFalse)
            WW_DISPATCH_TO(And)
            WW_DISPATCH_TO(Or)
            WW_DISPATCH_TO(JumpIfNull)
            WW_DISPATCH_TO(JumpIfNotNull)
            WW_DISPATCH_TO(AssertNotNull)
            WW_DISPATCH_TO(Swap)
            WW_DISPATCH_TO(PopUnder)
            WW_DISPATCH_TO(IsType)
            WW_DISPATCH_TO(Cast)
            WW_DISPATCH_TO(CastOrNull)
            WW_DISPATCH_TO(Settle)
            WW_DISPATCH_TO(Call)
            WW_DISPATCH_TO(CallNamed)
            WW_DISPATCH_TO(Spread)
            WW_DISPATCH_TO(CallSpread)
            WW_DISPATCH_TO(InvokeOwn)
            WW_DISPATCH_TO(Invoke)
            WW_DISPATCH_TO(InvokeOwnTrailing)
            WW_DISPATCH_TO(InvokeTrailing)
            WW_DISPATCH_TO(SuperInvoke)
            WW_DISPATCH_TO(SuperInvokeTrailing)
            WW_DISPATCH_TO(SuperInit)
            WW_DISPATCH_TO(Closure)
            WW_DISPATCH_TO(MakeForms)
            WW_DISPATCH_TO(MethodForms)
            WW_DISPATCH_TO(StaticForms)
            WW_DISPATCH_TO(GetIndexOr)
            WW_DISPATCH_TO(CloseUpvalue)
            WW_DISPATCH_TO(CallOn)
            WW_DISPATCH_TO(ImplicitFirst)
            WW_DISPATCH_TO(TailLoop)
            WW_DISPATCH_TO(ReturnInit)
            WW_DISPATCH_TO(Return)
            WW_DISPATCH_TO(Class)
            WW_DISPATCH_TO(Inherit)
            WW_DISPATCH_TO(ClassInit)
            WW_DISPATCH_TO(EnsureClassInit)
            WW_DISPATCH_TO(MissingArg)
            WW_DISPATCH_TO(DataField)
            WW_DISPATCH_TO(Extend)
            WW_DISPATCH_TO(Bridge)
            WW_DISPATCH_TO(BridgeMethod)
            WW_DISPATCH_TO(Lazy)
            WW_DISPATCH_TO(UnstableKey)
            WW_DISPATCH_TO(Component)
            WW_DISPATCH_TO(IterNextPair)
            WW_DISPATCH_TO(MarkInterface)
            WW_DISPATCH_TO(Extension)
            WW_DISPATCH_TO(MarkAbstract)
            WW_DISPATCH_TO(ClassOrigin)
            WW_DISPATCH_TO(MarkValue)
            WW_DISPATCH_TO(Numeric)
            WW_DISPATCH_TO(WithRefResult)
            WW_DISPATCH_TO(FieldDestination)
            WW_DISPATCH_TO(PlaceLocal)
            WW_DISPATCH_TO(PlaceUpvalue)
            WW_DISPATCH_TO(PlaceGlobal)
            WW_DISPATCH_TO(PlaceField)
            WW_DISPATCH_TO(PlaceIndex)
            WW_DISPATCH_TO(InsertNew)
            WW_DISPATCH_TO(AddNew)
            WW_DISPATCH_TO(WithDestinationAt)
            WW_DISPATCH_TO(WithFrameDestination)
            WW_DISPATCH_TO(CheckOverride)
            WW_DISPATCH_TO(Require)
            WW_DISPATCH_TO(ClassNumber)
            WW_DISPATCH_TO(MemberAccess)
            WW_DISPATCH_TO(StaticAccess)
            WW_DISPATCH_TO(FinishClass)
            WW_DISPATCH_TO(Getter)
            WW_DISPATCH_TO(Setter)
            WW_DISPATCH_TO(PendingEnum)
            WW_DISPATCH_TO(EnumNext)
            WW_DISPATCH_TO(EnumEntry)
            WW_DISPATCH_TO(EnumSeal)
            WW_DISPATCH_TO(StaticGetter)
            WW_DISPATCH_TO(StaticSetter)
            WW_DISPATCH_TO(StaticField)
            WW_DISPATCH_TO(DeclareField)
            WW_DISPATCH_TO(FieldInit)
            WW_DISPATCH_TO(Method)
            WW_DISPATCH_TO(StaticMethod)
            WW_DISPATCH_TO(BuildList)
            WW_DISPATCH_TO(TagCollection)
            WW_DISPATCH_TO(BuildMap)
            WW_DISPATCH_TO(BuildString)
            WW_DISPATCH_TO(Range)
            WW_DISPATCH_TO(FromEnd)
            WW_DISPATCH_TO(IterInit)
            WW_DISPATCH_TO(RangeLoop)
            WW_DISPATCH_TO(RangeNext)
            WW_DISPATCH_TO(DefaultArg)
            WW_DISPATCH_TO(IterNextRef)
            WW_DISPATCH_TO(IterNext)
            WW_DISPATCH_TO(TryBegin)
            WW_DISPATCH_TO(TryEnd)
            WW_DISPATCH_TO(Throw)
            WW_DISPATCH_TO(Import)
            WW_DISPATCH_TO(Count_)
            WW_DISPATCH_TO(MoveS)
            WW_DISPATCH_TO(MoveK)
            WW_DISPATCH_TO(SetTop)
            WW_DISPATCH_TO(RAddSS)
            WW_DISPATCH_TO(RAddSK)
            WW_DISPATCH_TO(RAddKS)
            WW_DISPATCH_TO(RSubSS)
            WW_DISPATCH_TO(RSubSK)
            WW_DISPATCH_TO(RMulSS)
            WW_DISPATCH_TO(RMulSK)
            WW_DISPATCH_TO(RMulKS)
            WW_DISPATCH_TO(RDivSS)
            WW_DISPATCH_TO(RDivSK)
            WW_DISPATCH_TO(RLtSS)
            WW_DISPATCH_TO(RLtSK)
            WW_DISPATCH_TO(RJumpLtSS)
            WW_DISPATCH_TO(RJumpLtSK)
            WW_DISPATCH_TO(RLeSS)
            WW_DISPATCH_TO(RLeSK)
            WW_DISPATCH_TO(RJumpLeSS)
            WW_DISPATCH_TO(RJumpLeSK)
            WW_DISPATCH_TO(RGtSS)
            WW_DISPATCH_TO(RGtSK)
            WW_DISPATCH_TO(RJumpGtSS)
            WW_DISPATCH_TO(RJumpGtSK)
            WW_DISPATCH_TO(RGeSS)
            WW_DISPATCH_TO(RGeSK)
            WW_DISPATCH_TO(RJumpGeSS)
            WW_DISPATCH_TO(RJumpGeSK)
            WW_DISPATCH_TO(REqSS)
            WW_DISPATCH_TO(REqSK)
            WW_DISPATCH_TO(RJumpEqSS)
            WW_DISPATCH_TO(RJumpEqSK)
            WW_DISPATCH_TO(RGetProp)
            WW_DISPATCH_TO(RModSS)
            WW_DISPATCH_TO(RModSK)
            WW_DISPATCH_TO(RSubKS)
            WW_DISPATCH_TO(RDivKS)
            WW_DISPATCH_TO(RSetPropSS)
            WW_DISPATCH_TO(RSetPropSK)
            WW_DISPATCH_TO(RGetGlobal)
            WW_DISPATCH_TO(RGetUpvalue)
            WW_DISPATCH_TO(RGetIndexSS)
            WW_DISPATCH_TO(RGetIndexSK)
            WW_DISPATCH_TO(RNeg)
            WW_DISPATCH_TO(RNot)
            WW_DISPATCH_TO(RJumpIfFalse)
            WW_DISPATCH_TO(RShlSK)
            WW_DISPATCH_TO(RShrSK)
            WW_DISPATCH_TO(RBitAndSS)
            WW_DISPATCH_TO(RBitAndSK)
            WW_DISPATCH_TO(RBitOrSS)
            WW_DISPATCH_TO(RBitOrSK)
            WW_DISPATCH_TO(RShlSS)
            WW_DISPATCH_TO(RShrSS)
            WW_DISPATCH_TO(RBitXorSS)
            WW_DISPATCH_TO(RBitXorSK)
            WW_DISPATCH_TO(RUshrSK)
            WW_DISPATCH_TO(RUshrSS)
            WW_DISPATCH_TO(RAddWrapSS)
            WW_DISPATCH_TO(RAddWrapSK)
            WW_DISPATCH_TO(RSubWrapSS)
            WW_DISPATCH_TO(RSubWrapSK)
            WW_DISPATCH_TO(RMulWrapSS)
            WW_DISPATCH_TO(RMulWrapSK)
            // Since M6.7 step 5: typed arithmetic, places, type slots, RGetPath (G14).
            WW_DISPATCH_TO(AddI)
            WW_DISPATCH_TO(SubI)
            WW_DISPATCH_TO(MulI)
            WW_DISPATCH_TO(DivF)
            WW_DISPATCH_TO(AddF)
            WW_DISPATCH_TO(SubF)
            WW_DISPATCH_TO(MulF)
            WW_DISPATCH_TO(LtI)
            WW_DISPATCH_TO(LeI)
            WW_DISPATCH_TO(GtI)
            WW_DISPATCH_TO(GeI)
            WW_DISPATCH_TO(EqI)
            WW_DISPATCH_TO(LtF)
            WW_DISPATCH_TO(LeF)
            WW_DISPATCH_TO(GtF)
            WW_DISPATCH_TO(GeF)
            WW_DISPATCH_TO(EqF)
            WW_DISPATCH_TO(EnterPlaces)
            WW_DISPATCH_TO(SetPlace)
            WW_DISPATCH_TO(WithPlace)
            WW_DISPATCH_TO(KeepLocal)
            WW_DISPATCH_TO(KeepTop)
            WW_DISPATCH_TO(FieldTypesMaker)
            WW_DISPATCH_TO(TypeSlots)
            WW_DISPATCH_TO(JumpIfNotClass)
            WW_DISPATCH_TO(RGetPath)
            WW_DISPATCH_TO(Translate)
            WW_DISPATCH_TO(RAddISS)
            WW_DISPATCH_TO(RAddISK)
            WW_DISPATCH_TO(RAddIKS)
            WW_DISPATCH_TO(RSubISS)
            WW_DISPATCH_TO(RSubISK)
            WW_DISPATCH_TO(RSubIKS)
            WW_DISPATCH_TO(RMulISS)
            WW_DISPATCH_TO(RMulISK)
            WW_DISPATCH_TO(RMulIKS)
            WW_DISPATCH_TO(RAddFSS)
            WW_DISPATCH_TO(RAddFSK)
            WW_DISPATCH_TO(RAddFKS)
            WW_DISPATCH_TO(RSubFSS)
            WW_DISPATCH_TO(RSubFSK)
            WW_DISPATCH_TO(RSubFKS)
            WW_DISPATCH_TO(RMulFSS)
            WW_DISPATCH_TO(RMulFSK)
            WW_DISPATCH_TO(RMulFKS)
            WW_DISPATCH_TO(RDivFSS)
            WW_DISPATCH_TO(RDivFSK)
            WW_DISPATCH_TO(RDivFKS)
            WW_DISPATCH_TO(RLtISS)
            WW_DISPATCH_TO(RLtISK)
            WW_DISPATCH_TO(RLeISS)
            WW_DISPATCH_TO(RLeISK)
            WW_DISPATCH_TO(RGtISS)
            WW_DISPATCH_TO(RGtISK)
            WW_DISPATCH_TO(RGeISS)
            WW_DISPATCH_TO(RGeISK)
            WW_DISPATCH_TO(REqISS)
            WW_DISPATCH_TO(REqISK)
            WW_DISPATCH_TO(RLtFSS)
            WW_DISPATCH_TO(RLtFSK)
            WW_DISPATCH_TO(RLeFSS)
            WW_DISPATCH_TO(RLeFSK)
            WW_DISPATCH_TO(RGtFSS)
            WW_DISPATCH_TO(RGtFSK)
            WW_DISPATCH_TO(RGeFSS)
            WW_DISPATCH_TO(RGeFSK)
            WW_DISPATCH_TO(REqFSS)
            WW_DISPATCH_TO(REqFSK)
            WW_DISPATCH_TO(RJumpLtISS)
            WW_DISPATCH_TO(RJumpLtISK)
            WW_DISPATCH_TO(RJumpLeISS)
            WW_DISPATCH_TO(RJumpLeISK)
            WW_DISPATCH_TO(RJumpGtISS)
            WW_DISPATCH_TO(RJumpGtISK)
            WW_DISPATCH_TO(RJumpGeISS)
            WW_DISPATCH_TO(RJumpGeISK)
            WW_DISPATCH_TO(RJumpEqISS)
            WW_DISPATCH_TO(RJumpEqISK)
            WW_DISPATCH_TO(RJumpLtFSS)
            WW_DISPATCH_TO(RJumpLtFSK)
            WW_DISPATCH_TO(RJumpLeFSS)
            WW_DISPATCH_TO(RJumpLeFSK)
            WW_DISPATCH_TO(RJumpGtFSS)
            WW_DISPATCH_TO(RJumpGtFSK)
            WW_DISPATCH_TO(RJumpGeFSS)
            WW_DISPATCH_TO(RJumpGeFSK)
            WW_DISPATCH_TO(RJumpEqFSS)
            WW_DISPATCH_TO(RJumpEqFSK)
            WW_DISPATCH_TO(RInvoke)
            WW_DISPATCH_TO(RCall)
            WW_DISPATCH_TO(RGetIndexPropSS)
            WW_DISPATCH_TO(RGetIndexPropSK)
#undef WW_DISPATCH_TO
            dispatchMade.store(true, std::memory_order_release);
        }
    }
#endif

    for (;;) {
        try {
            LOAD_FRAME();

#if WW_TAIL_CALLS
            {
                // The first instruction's function, which calls the next one's, and so on until
                // one returns: where run() ends. An error thrown in one comes here, as below.
                Value* sp = sp_;
                const TailFn to_ = tailAt(ip);
                frame->ip = ip += kThreaded + kOpBytes;
                (this->*to_)(ip, sp, base, frame, consts, caches, code);
            }
            return;
#else
            // The stack's top, held in a register while the loop runs (M6.5 part 4). It is written
            // back to sp_ before each instruction, a store nothing waits on, so that every
            // instruction not given a quick way below works on sp_ as it always has; after one
            // of those, it is read back. The quick ones work on it here and go straight on.
            Value* sp = sp_;
            for (;;) {
#ifdef WW_OP_COUNTS
                countOp(opAt(ip + kThreaded));
#endif
                sp_ = sp;
#if WW_GOTO_DISPATCH
                {
                    // Straight to the instruction's code, its address in the code (D160); the
                    // compiler copies the jump to the end of each instruction, where each is
                    // predicted apart.
                    const void* to_;
                    std::memcpy(&to_, ip, sizeof to_);
                    frame->ip = ip += kThreaded + kOpBytes;
                    goto* to_;
                }
            viaSwitch:
#else
                frame->ip = ip += kThreaded + kOpBytes;
#endif
                switch (opAt(ip - kOpBytes)) {
#include "instructions.inc"
                    default:
                        // The stack code's own instructions (AddLocals and the rest), which the run
                        // code never holds (registers.cpp).
                        throw runtimeError(Thrown::InternalError, "invalid instruction");
                }
                sp = sp_;
            }
#endif
        } catch (WwThrow& thrown) {
            if (!handled(thrown)) throw;
        } catch (const std::bad_alloc&) {
            // Asking for memory failed. That is almost always one large request (a list, a
            // string, a buffer), so what is left is plenty for the error itself; if even that
            // fails, the program ends as before.
            WwThrow thrown = outOfMemory();
            if (!handled(thrown)) throw thrown;
        } catch (const std::length_error&) {
            WwThrow thrown = outOfMemory();  // more than a list or a string can ever hold
            if (!handled(thrown)) throw thrown;
        }
    }

}

#if WW_TAIL_CALLS
// ---- The instructions as functions (M6.7) ----
// instructions.inc again, each instruction a function of its own: VM::tail<op>. Its ways out:
// NEXT calls the next instruction's function as its last step, and EXIT_RUN returns, to run().

#undef CASE
#undef CASES
#undef NEXT
#undef DONE
#undef EXIT_RUN
#undef FALL_INTO
#undef JUMP_LABEL
#undef LABEL
#undef RUN_BASE
#undef R_SLOW2
#undef R_PLAIN2
#define WW_TAIL_ARGS ip, sp, base, frame, consts, caches, code
#define CASE(name) template <> WW_TAIL_FN int VM::tail<Op::name>(WW_TAIL_PARAMS)
#define WW_DECLARE(name) template <> WW_TAIL_FN int VM::tail<Op::name>(WW_TAIL_PARAMS);
#define WW_FORWARD(to, name) \
    template <> WW_TAIL_FN int VM::tail<Op::name>(WW_TAIL_PARAMS) { [[clang::musttail]] return tail<Op::to>(WW_TAIL_ARGS); }
// CASES(a, b, ...): the code is a's; b and the rest go to it.
#define WW_CASES_1(a0) CASE(a0)
#define WW_CASES_2(a0, a1) WW_DECLARE(a0) WW_FORWARD(a0, a1) CASE(a0)
#define WW_CASES_3(a0, a1, a2) WW_DECLARE(a0) WW_FORWARD(a0, a1) WW_FORWARD(a0, a2) CASE(a0)
#define WW_CASES_4(a0, a1, a2, a3) WW_DECLARE(a0) WW_FORWARD(a0, a1) WW_FORWARD(a0, a2) WW_FORWARD(a0, a3) CASE(a0)
#define WW_CASES_5(a0, a1, a2, a3, a4) WW_DECLARE(a0) WW_FORWARD(a0, a1) WW_FORWARD(a0, a2) WW_FORWARD(a0, a3) WW_FORWARD(a0, a4) CASE(a0)
#define WW_CASES_6(a0, a1, a2, a3, a4, a5) WW_DECLARE(a0) WW_FORWARD(a0, a1) WW_FORWARD(a0, a2) WW_FORWARD(a0, a3) WW_FORWARD(a0, a4) WW_FORWARD(a0, a5) CASE(a0)
#define WW_CASES_7(a0, a1, a2, a3, a4, a5, a6) WW_DECLARE(a0) WW_FORWARD(a0, a1) WW_FORWARD(a0, a2) WW_FORWARD(a0, a3) WW_FORWARD(a0, a4) WW_FORWARD(a0, a5) WW_FORWARD(a0, a6) CASE(a0)
#define WW_CASES_8(a0, a1, a2, a3, a4, a5, a6, a7) WW_DECLARE(a0) WW_FORWARD(a0, a1) WW_FORWARD(a0, a2) WW_FORWARD(a0, a3) WW_FORWARD(a0, a4) WW_FORWARD(a0, a5) WW_FORWARD(a0, a6) WW_FORWARD(a0, a7) CASE(a0)
#define WW_CASES_9(a0, a1, a2, a3, a4, a5, a6, a7, a8) WW_DECLARE(a0) WW_FORWARD(a0, a1) WW_FORWARD(a0, a2) WW_FORWARD(a0, a3) WW_FORWARD(a0, a4) WW_FORWARD(a0, a5) WW_FORWARD(a0, a6) WW_FORWARD(a0, a7) WW_FORWARD(a0, a8) CASE(a0)
#define WW_CASES_10(a0, a1, a2, a3, a4, a5, a6, a7, a8, a9) WW_DECLARE(a0) WW_FORWARD(a0, a1) WW_FORWARD(a0, a2) WW_FORWARD(a0, a3) WW_FORWARD(a0, a4) WW_FORWARD(a0, a5) WW_FORWARD(a0, a6) WW_FORWARD(a0, a7) WW_FORWARD(a0, a8) WW_FORWARD(a0, a9) CASE(a0)
#define WW_CASES_11(a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10) WW_DECLARE(a0) WW_FORWARD(a0, a1) WW_FORWARD(a0, a2) WW_FORWARD(a0, a3) WW_FORWARD(a0, a4) WW_FORWARD(a0, a5) WW_FORWARD(a0, a6) WW_FORWARD(a0, a7) WW_FORWARD(a0, a8) WW_FORWARD(a0, a9) WW_FORWARD(a0, a10) CASE(a0)
#define WW_CASES_12(a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11) WW_DECLARE(a0) WW_FORWARD(a0, a1) WW_FORWARD(a0, a2) WW_FORWARD(a0, a3) WW_FORWARD(a0, a4) WW_FORWARD(a0, a5) WW_FORWARD(a0, a6) WW_FORWARD(a0, a7) WW_FORWARD(a0, a8) WW_FORWARD(a0, a9) WW_FORWARD(a0, a10) WW_FORWARD(a0, a11) CASE(a0)
#define WW_CASES_13(a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12) WW_DECLARE(a0) WW_FORWARD(a0, a1) WW_FORWARD(a0, a2) WW_FORWARD(a0, a3) WW_FORWARD(a0, a4) WW_FORWARD(a0, a5) WW_FORWARD(a0, a6) WW_FORWARD(a0, a7) WW_FORWARD(a0, a8) WW_FORWARD(a0, a9) WW_FORWARD(a0, a10) WW_FORWARD(a0, a11) WW_FORWARD(a0, a12) CASE(a0)
#define WW_CASES_14(a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13) WW_DECLARE(a0) WW_FORWARD(a0, a1) WW_FORWARD(a0, a2) WW_FORWARD(a0, a3) WW_FORWARD(a0, a4) WW_FORWARD(a0, a5) WW_FORWARD(a0, a6) WW_FORWARD(a0, a7) WW_FORWARD(a0, a8) WW_FORWARD(a0, a9) WW_FORWARD(a0, a10) WW_FORWARD(a0, a11) WW_FORWARD(a0, a12) WW_FORWARD(a0, a13) CASE(a0)
#define WW_CASES_15(a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14) WW_DECLARE(a0) WW_FORWARD(a0, a1) WW_FORWARD(a0, a2) WW_FORWARD(a0, a3) WW_FORWARD(a0, a4) WW_FORWARD(a0, a5) WW_FORWARD(a0, a6) WW_FORWARD(a0, a7) WW_FORWARD(a0, a8) WW_FORWARD(a0, a9) WW_FORWARD(a0, a10) WW_FORWARD(a0, a11) WW_FORWARD(a0, a12) WW_FORWARD(a0, a13) WW_FORWARD(a0, a14) CASE(a0)
#define WW_CASES_16(a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15) WW_DECLARE(a0) WW_FORWARD(a0, a1) WW_FORWARD(a0, a2) WW_FORWARD(a0, a3) WW_FORWARD(a0, a4) WW_FORWARD(a0, a5) WW_FORWARD(a0, a6) WW_FORWARD(a0, a7) WW_FORWARD(a0, a8) WW_FORWARD(a0, a9) WW_FORWARD(a0, a10) WW_FORWARD(a0, a11) WW_FORWARD(a0, a12) WW_FORWARD(a0, a13) WW_FORWARD(a0, a14) WW_FORWARD(a0, a15) CASE(a0)
#define CASES(...) WW_EXPAND(WW_CAT(WW_CASES_, WW_COUNT(__VA_ARGS__))(__VA_ARGS__))
#define NEXT()                                                            \
    do {                                                                  \
        const TailFn to_ = tailAt(ip);                                    \
        sp_ = sp;                                                         \
        frame->ip = ip += kThreaded + kOpBytes;                           \
        [[clang::musttail]] return (this->*to_)(WW_TAIL_ARGS);         \
    } while (0)
#define DONE()     \
    do {           \
        sp = sp_;  \
        NEXT();    \
    } while (0)
#define EXIT_RUN() return 0
#define FALL_INTO(op) [[clang::musttail]] return tail<Op::op>(WW_TAIL_ARGS)
#undef HAS_COLD
#undef GO_COLD
#undef COLD
#define HAS_COLD(op) template <> WW_TAIL_FN int VM::cold<Op::op>(WW_TAIL_PARAMS);
#define GO_COLD(op) [[clang::musttail]] return cold<Op::op>(WW_TAIL_ARGS)
#define COLD(op) template <> __attribute__((noinline)) WW_TAIL_FN int VM::cold<Op::op>(WW_TAIL_PARAMS)
#define RUN_BASE context_->runBase
#define LABEL(l)
// The instruction whose code a label of instructions.inc is in, or at the start of.
#define WW_OP_OF_getIndex GetIndex
#define WW_OP_OF_iterNext IterNext
#define WW_OP_OF_plainAdd Add
#define WW_OP_OF_plainDiv Div
#define WW_OP_OF_plainEq Eq
#define WW_OP_OF_plainGe Ge
#define WW_OP_OF_plainGt Gt
#define WW_OP_OF_plainLe Le
#define WW_OP_OF_plainLt Lt
#define WW_OP_OF_plainMod Mod
#define WW_OP_OF_plainMul Mul
#define WW_OP_OF_plainNeg Neg
#define WW_OP_OF_plainNot Not
#define WW_OP_OF_plainSub Sub
#define JUMP_LABEL(l) [[clang::musttail]] return tail<Op::WW_OP_OF_##l>(WW_TAIL_ARGS)
#define R_SLOW2(b, c, sAt, slowAt)                                                   \
    {                                                                                \
        slowB_ = &(b);                                                               \
        slowC_ = &(c);                                                               \
        [[clang::musttail]] return slowTwo<sAt, slowAt>(WW_TAIL_ARGS);            \
    }
#define R_PLAIN2(b, c, plain)                                                        \
    if (ip[0] == ip[4]) {                                                            \
        slowB_ = &(b);                                                               \
        slowC_ = &(c);                                                               \
        [[clang::musttail]] return plainTwo<Op::WW_OP_OF_##plain>(WW_TAIL_ARGS); \
    }

// What is jumped into before its own code comes.
WW_DECLARE(Return)
WW_DECLARE(Add)
WW_DECLARE(Div)
WW_DECLARE(Eq)
WW_DECLARE(Ge)
WW_DECLARE(GetIndex)
WW_DECLARE(Gt)
WW_DECLARE(IterNext)
WW_DECLARE(Le)
WW_DECLARE(Lt)
WW_DECLARE(Mod)
WW_DECLARE(Mul)
WW_DECLARE(Neg)
WW_DECLARE(Not)
WW_DECLARE(Sub)

template <int sAt, int slowAt>
__attribute__((noinline)) WW_TAIL_FN int VM::slowTwo(WW_TAIL_PARAMS) {
    const Value b_ = *slowB_, c_ = *slowC_;
    Value* const at = base + ip[sAt];
    at[0] = b_;
    at[1] = c_;
    sp = at + 2;
    ip = code + operandI32(ip + slowAt);
    NEXT();
}

template <Op plain>
__attribute__((noinline)) WW_TAIL_FN int VM::plainTwo(WW_TAIL_PARAMS) {
    const Value b_ = *slowB_, c_ = *slowC_;
    Value* const at = base + ip[4];
    at[0] = b_;
    at[1] = c_;
    sp = sp_ = at + 2;
    ip += 9;
    [[clang::musttail]] return tail<plain>(WW_TAIL_ARGS);
}

#include "instructions.inc"

// An instruction run code never has.
template <Op op>
WW_TAIL_FN int VM::tail(WW_TAIL_PARAMS) {
    throw runtimeError(Thrown::InternalError, "invalid instruction");
}

const TailFn* TailTable::get() {
    static const std::array<TailFn, kOpCount> table = []<size_t... I>(std::index_sequence<I...>) {
        std::array<TailFn, kOpCount> made{{&VM::tail<static_cast<Op>(I)>...}};
        if constexpr (sizeof(TailFn) == 2 * kThreaded) {
            for (const TailFn& f : made) {
                uintptr_t parts[2];
                std::memcpy(parts, &f, sizeof parts);
                if (parts[1] != 0 || (parts[0] & 1) != 0) throw std::logic_error("an instruction's function is not one address");
            }
        }
        return made;
    }(std::make_index_sequence<kOpCount>{});
    return table.data();
}

// How often a region's quick ways may not do what they do before the VM stops going in.
constexpr uint32_t kNativeMissLimit = 256;

// ---- Native code made on another core (2026-11-17, background-plan.html) ----
// Where a region is made (a loop's second time round, a whole function's return after its
// calls are counted, one dropped made again), what the program holds is taken there
// (takeNativeSeen) and the making handed to one builder thread; the program goes on in the VM
// meanwhile, and the builder puts the regions in as they are made, as a core would. Where
// WW_NATIVE_BACKGROUND is 0, made in place as before; `wait`, by the builder while the
// program waits (made from what was taken either way, so the code is the same).
namespace {
enum class MakeWay { inPlace, background, wait };
MakeWay makeWay() {
    static const MakeWay way = [] {
        const char* asked = std::getenv("WW_NATIVE_BACKGROUND");
        if (asked == nullptr) return MakeWay::background;
        const std::string a(asked);
        return a == "0" ? MakeWay::inPlace : a == "wait" ? MakeWay::wait : MakeWay::background;
    }();
    return way;
}
// One region-making at a time, in place or the builder's (the executable memory and the
// making's counters are shared); and, apart, the loops' states (nativeArrivals) and the
// publishing of regions, held only briefly, so that a core handing a build on never waits for
// one being made.
std::mutex nativeMaking;
std::mutex nativeStates;

// ---- Run code's words, written while no other thread runs them (2026-11-18) ----
// Each instruction's word (its function's address, kThreaded bytes) starts where the instruction
// before it ends, at any byte: so one may lie across two cache lines, and a core reading it as
// another writes it may take half of each address (o-pstress: parallel.ww under thread stress,
// a core jumped to tail<Return>'s address with nativeReturn's low byte, one run in some 500).
// So a word is written only by the program's own VM (VM::runsProgramAlone), under nativeStates:
// while it runs no other thread runs the program's code. One written by a section's core or the
// builder waits for it (VM::writeWaitingWords: at its next safe point, or as the section ends),
// the latest for each word; meanwhile each VM::native* way in does rightly whatever its word says.
struct WaitingWord {
    const VM* program;
    FunctionProto* f;
    uint32_t at;
    uint64_t word;
};
std::vector<WaitingWord> waitingWords;  // under nativeStates

uint64_t wordOf(const TailFn& to) {
    uint64_t word;
    std::memcpy(&word, &to, kThreaded);
    return word;
}
// Instruction `at`'s word in f's run code made `to`'s: here, by `by` running the program alone,
// or waiting for the program's VM (`by` null: the builder, for `asker`). Under nativeStates.
void writeRunWord(VM* by, const VM* asker, FunctionProto& f, uint32_t at, const TailFn& to) {
    const uint64_t word = wordOf(to);
    if (by != nullptr && by->runsProgramAlone()) {
        // (One waiting for this word is older: dropped.)
        if (!waitingWords.empty()) std::erase_if(waitingWords, [&](const WaitingWord& w) { return w.f == &f && w.at == at; });
        std::memcpy(f.run.data() + at, &word, kThreaded);
        return;
    }
    VM& program = const_cast<VM*>(by != nullptr ? by : asker)->programVM();
    for (WaitingWord& w : waitingWords)
        if (w.f == &f && w.at == at) {
            w.word = word;
            program.wordsWaiting();
            return;
        }
    waitingWords.push_back({&program, &f, at, word});
    program.wordsWaiting();
}
// The instruction's own function, as its word holds it with no native code.
TailFn ownOf(const uint8_t* run, uint32_t at) { return TailTable::get()[static_cast<size_t>(opAt(run + at + kThreaded))]; }

// A loop's regions published (under nativeStates): the function's list replaced whole (less
// those dropped: one made again has the same ways in, found first otherwise), its head its own
// instruction, each way in the VM's way into its region.
void publishLoop(VM* by, const VM* asker, FunctionProto& f, size_t which, const std::vector<FunctionProto::NativeRegion>& made) {
    auto list = std::make_unique<std::vector<FunctionProto::NativeRegion>>();
    if (const auto* now = f.regionsNow.load(std::memory_order_acquire))
        for (const FunctionProto::NativeRegion& r : *now)
            if (*r.misses < kNativeMissLimit) list->push_back(r);
    list->insert(list->end(), made.begin(), made.end());
    f.regionsNow.store(list.get(), std::memory_order_release);
    f.regionLists.push_back(std::move(list));
    const uint32_t head = f.nativeHeads[which];
    writeRunWord(by, asker, f, head, ownOf(f.run.data(), head));
    const TailFn enter = TailTable::nativeEntry();
    for (const FunctionProto::NativeRegion& r : made) writeRunWord(by, asker, f, r.head, enter);
    const uint8_t was = f.nativeArrivals[which];
    f.nativeArrivals[which] = was == 3 || was == 6 ? 4 : 2;
}
// A whole function's region published (its returns already their own instructions again).
void publishFunction(VM* by, const VM* asker, FunctionProto& f, const std::vector<FunctionProto::NativeRegion>& made) {
    if (made.empty()) return;
    auto list = std::make_unique<std::vector<FunctionProto::NativeRegion>>();
    if (const auto* now = f.regionsNow.load(std::memory_order_acquire))  // (less those dropped)
        for (const FunctionProto::NativeRegion& r : *now)
            if (*r.misses < kNativeMissLimit) list->push_back(r);
    list->insert(list->end(), made.begin(), made.end());
    f.regionsNow.store(list.get(), std::memory_order_release);
    f.regionLists.push_back(std::move(list));
    const TailFn enter = TailTable::nativeEntry();
    for (const FunctionProto::NativeRegion& r : made) writeRunWord(by, asker, f, r.head, enter);
}

constexpr size_t kWholeFunction = SIZE_MAX;
struct Build {
    const VM* asker;
    FunctionProto* f;
    uint32_t head;
    size_t which;  // the loop's (in nativeHeads), or kWholeFunction
    std::unique_ptr<NativeSeen> seen;
    bool done = false;
    RegionCounts* counted = nullptr;  // made again from these (the region made first's)
};
// A loop's regions made again from what its region made first counted, published (under
// nativeStates): they take the place of that one's (those of its counts) in the function's
// list; its ways in that are not theirs the VM's own instructions again, theirs the VM's way
// into them; its times round set one short of kCountedRounds, so that where it is running yet
// (on a long loop) it leaves at its next, and the VM goes on in these. Made none (refused),
// that one stays.
void publishCounted(VM* by, const VM* asker, FunctionProto& f, RegionCounts* counted, const std::vector<FunctionProto::NativeRegion>& made) {
    if (made.empty()) return;
    auto list = std::make_unique<std::vector<FunctionProto::NativeRegion>>();
    std::vector<uint32_t> old;
    if (const auto* now = f.regionsNow.load(std::memory_order_acquire))
        for (const FunctionProto::NativeRegion& r : *now) {
            if (r.counts == counted) old.push_back(r.head);
            else if (*r.misses < kNativeMissLimit) list->push_back(r);
        }
    list->insert(list->end(), made.begin(), made.end());
    f.regionsNow.store(list.get(), std::memory_order_release);
    f.regionLists.push_back(std::move(list));
    for (uint32_t at : old) writeRunWord(by, asker, f, at, ownOf(f.run.data(), at));
    const TailFn enter = TailTable::nativeEntry();
    for (const FunctionProto::NativeRegion& r : made) writeRunWord(by, asker, f, r.head, enter);
    counted->runs[counted->at.size() + 1] = 0;  // (where it runs on, it leaves at its next time round)
}
void makeAndPublish(Build& b) {
    std::vector<FunctionProto::NativeRegion> made;
    {
        std::lock_guard<std::mutex> making(nativeMaking);
        if (makeNativeRegions != nullptr && native::enabled()) makeNativeRegions(*b.f, *b.seen, b.head, made);
    }
    std::lock_guard<std::mutex> hold(nativeStates);
    if (b.counted != nullptr) publishCounted(nullptr, b.asker, *b.f, b.counted, made);
    else if (b.which == kWholeFunction) publishFunction(nullptr, b.asker, *b.f, made);
    else publishLoop(nullptr, b.asker, *b.f, b.which, made);
}
// The builder: one thread, started with the first build, taking them in turn. Never destroyed
// (its thread is let go): as the program ends (stop) it finishes the build under way and drops
// the rest.
class Builder {
public:
    void add(const std::shared_ptr<Build>& b) {
        std::lock_guard<std::mutex> hold(lock_);
        if (stopping_) return;
        if (!started_) {
            started_ = true;
            std::thread([this] { work(); }).detach();
            std::atexit([] { builder().stop(); });
        }
        queue_.push_back(b);
        more_.notify_one();
    }
    void waitFor(const Build& b) {
        std::unique_lock<std::mutex> hold(lock_);
        finished_.wait(hold, [&] { return b.done || stopping_; });
    }
    // Until no build `asker` asked for waits or is under way.
    void waitForAll(const VM* asker) {
        std::unique_lock<std::mutex> hold(lock_);
        finished_.wait(hold, [&] {
            if (stopping_) return true;
            if (current_ != nullptr && current_->asker == asker) return false;
            return std::none_of(queue_.begin(), queue_.end(), [&](const std::shared_ptr<Build>& b) { return b->asker == asker; });
        });
    }
    // Each value a build `asker` asked for keeps (marked by its collector).
    template <class Mark>
    void roots(const VM* asker, Mark mark) {
        std::lock_guard<std::mutex> hold(lock_);
        auto each = [&](const std::shared_ptr<Build>& b) {
            if (b != nullptr && b->asker == asker)
                for (const Value& v : b->seen->roots) mark(v);
        };
        for (const auto& b : queue_) each(b);
        each(current_);
    }
    // The builds `asker` asked for dropped, the one under way finished (a VM ending).
    // A loop's build dropped is asked for again by the next core to start the loop (its head
    // VM::nativeFirst's again): cores share functions, and one ending drops what it asked for.
    // (A whole function's stays unmade.)
    void dropFor(const VM* asker) {
        std::vector<std::shared_ptr<Build>> dropped;
        {
            std::unique_lock<std::mutex> hold(lock_);
            for (auto it = queue_.begin(); it != queue_.end();)
                if ((*it)->asker == asker) {
                    dropped.push_back(*it);
                    it = queue_.erase(it);
                } else {
                    ++it;
                }
            finished_.wait(hold, [&] { return current_ == nullptr || current_->asker != asker; });
        }
        std::lock_guard<std::mutex> states(nativeStates);
        const TailFn first = TailTable::nativeFirstEntry();
        for (const auto& b : dropped) {
            if (b->counted != nullptr) b->counted->asked = false, b->counted->runs[b->counted->at.size() + 1] = 0;  // (asked for again as it next leaves)
            if (b->which == kWholeFunction || b->counted != nullptr) continue;
            uint8_t& arrived = b->f->nativeArrivals[b->which];
            if (arrived != 5 && arrived != 6) continue;
            arrived = arrived == 6 ? 3 : 1;
            writeRunWord(const_cast<VM*>(asker), asker, *b->f, b->head, first);
        }
    }
    // The builds of functions a program owns, as it ends (the program goes before its VM).
    void dropFunctions(const std::vector<std::unique_ptr<FunctionProto>>& owned) {
        std::unordered_set<const FunctionProto*> mine;
        {
            std::unique_lock<std::mutex> hold(lock_);
            if (!queue_.empty() || current_ != nullptr) {
                for (const auto& p : owned) mine.insert(p.get());
                std::erase_if(queue_, [&](const std::shared_ptr<Build>& b) { return mine.count(b->f) != 0; });
                finished_.wait(hold, [&] { return current_ == nullptr || mine.count(current_->f) == 0; });
            }
        }
        // And their words waiting to be written (writeRunWord).
        std::lock_guard<std::mutex> states(nativeStates);
        if (waitingWords.empty()) return;
        if (mine.empty())
            for (const auto& p : owned) mine.insert(p.get());
        std::erase_if(waitingWords, [&](const WaitingWord& w) { return mine.count(w.f) != 0; });
    }
    void stop() {
        std::unique_lock<std::mutex> hold(lock_);
        stopping_ = true;
        queue_.clear();
        finished_.notify_all();
        finished_.wait(hold, [&] { return current_ == nullptr; });
    }
    static Builder& builder() {
        static Builder* const one = new Builder;
        return *one;
    }

private:
    void work() {
        std::unique_lock<std::mutex> hold(lock_);
        for (;;) {
            more_.wait(hold, [&] { return !queue_.empty(); });
            current_ = queue_.front();
            queue_.pop_front();
            hold.unlock();
            makeAndPublish(*current_);
            hold.lock();
            current_->done = true;
            current_->seen.reset();
            current_ = nullptr;
            finished_.notify_all();
        }
    }
    std::mutex lock_;
    std::condition_variable more_, finished_;
    std::deque<std::shared_ptr<Build>> queue_;
    std::shared_ptr<Build> current_;
    bool started_ = false;
    bool stopping_ = false;
};
}  // namespace

// What the program holds now, for `f`'s regions (on this core).
std::unique_ptr<NativeSeen> VM::seenForRegions(const FunctionProto& f, const Value* base, const Value* top) {
    return takeNativeSeen(f, base, static_cast<int>(top - base), globals_.data(), globals_.size(), &VM::noMemberForRegion, this);
}

// Each build this VM asked for that is not made yet keeps what it read (collectGarbage).
void VM::markWaitingBuilds() {
    Builder::builder().roots(this, [&](const Value& v) { markValue(v); });
}

void VM::waitForBuilds() { Builder::builder().waitForAll(this); }

// A build of `f`'s regions handed on (the builder's), or made here (in place), by makeWay();
// `which` its loop, or kWholeFunction. Under nativeStates (`states`): let go here before any waiting.
void VM::makeRegionsFor(FunctionProto& f, uint32_t head, size_t which, std::unique_lock<std::mutex>& states, const Value* base, const Value* top) {
    auto b = std::make_shared<Build>();
    b->asker = this;
    b->f = &f;
    b->head = head;
    b->which = which;
    if (takeNativeSeen == nullptr || makeNativeRegions == nullptr || !native::enabled() || nativeOff_) {  // none made: as made, of no regions
        if (which == kWholeFunction) publishFunction(this, this, f, {});
        else publishLoop(this, this, f, which, {});
        return;
    }
    b->seen = seenForRegions(f, base, top);
    // WW_TRACE_BUILDS: each build said as it is asked for (the function, its loop's head, or a
    // whole function's; how far the loop had got: 1 first, 3 made again; the core asking).
    static const bool trace = std::getenv("WW_TRACE_BUILDS") != nullptr;
    if (trace)
        std::fprintf(stderr, "build %s %p at %u, %s %d, core %p\n", f.name.c_str(), static_cast<void*>(&f), head, which == kWholeFunction ? "whole" : "loop",
                     which == kWholeFunction ? 0 : static_cast<int>(f.nativeArrivals[which]), static_cast<const void*>(this));
    if (makeWay() == MakeWay::inPlace) {
        std::vector<FunctionProto::NativeRegion> made;
        {
            std::lock_guard<std::mutex> making(nativeMaking);
            makeNativeRegions(f, *b->seen, head, made);
        }
        if (which == kWholeFunction) publishFunction(this, this, f, made);
        else publishLoop(this, this, f, which, made);
        return;
    }
    // Meanwhile the loop's head is its own instruction again (its returns, a function's), and
    // the loop is marked as being made (5; 6 made again).
    if (which != kWholeFunction) {
        writeRunWord(this, this, f, head, ownOf(f.run.data(), head));
        f.nativeArrivals[which] = f.nativeArrivals[which] == 3 ? 6 : 5;
    }
    states.unlock();
    Builder::builder().add(b);
    if (makeWay() == MakeWay::wait) Builder::builder().waitFor(*b);
}

// A region made first, at its kCountedRounds-th time round (registers-plan.html part B): made
// again from what it counted, in place or by the builder (makeWay()), from the frame here; asked
// for once. True where it was made again here (the VM goes into the new one at once).
bool VM::remakeCounted(FunctionProto& f, const FunctionProto::NativeRegion& region, const Value* base, const Value* top) {
    std::unique_lock<std::mutex> states(nativeStates);
    RegionCounts* const counted = region.counts;
    if (counted == nullptr || counted->asked || takeNativeSeen == nullptr || makeNativeRegions == nullptr || !native::enabled() || nativeOff_) return false;
    // (Made from its start: the loop's head, its first way in, whichever way it left by.)
    uint32_t head = UINT32_MAX;
    if (const auto* now = f.regionsNow.load(std::memory_order_acquire))
        for (const FunctionProto::NativeRegion& r : *now)
            if (r.counts == counted && r.first) head = r.head;
    if (head == UINT32_MAX) return false;
    counted->asked = true;
    auto b = std::make_shared<Build>();
    b->asker = this;
    b->f = &f;
    b->head = head;
    b->which = 0;
    b->counted = counted;
    b->seen = seenForRegions(f, base, top);
    b->seen->rounds = counted->runs[counted->at.size()];
    for (size_t i = 0; i < counted->at.size(); i++) b->seen->runs[counted->at[i]] = counted->runs[i];
    static const bool trace = std::getenv("WW_TRACE_BUILDS") != nullptr;
    if (trace) std::fprintf(stderr, "build %s %p at %u, made again from %llu times round, core %p\n", f.name.c_str(), static_cast<void*>(&f), head,
                            static_cast<unsigned long long>(b->seen->rounds), static_cast<const void*>(this));
    if (trace) {  // (and what each instruction counted)
        for (size_t i = 0; i < counted->at.size(); i++) std::fprintf(stderr, " %zu:%llu", i, static_cast<unsigned long long>(counted->runs[i]));
        std::fprintf(stderr, "\n");
    }
    if (makeWay() == MakeWay::inPlace) {
        std::vector<FunctionProto::NativeRegion> made;
        {
            std::lock_guard<std::mutex> making(nativeMaking);
            makeNativeRegions(f, *b->seen, head, made);
        }
        publishCounted(this, this, f, counted, made);
        return !made.empty();
    }
    states.unlock();
    Builder::builder().add(b);
    if (makeWay() == MakeWay::wait) {
        Builder::builder().waitFor(*b);
        return true;
    }
    return false;
}

void VM::dropWaitingBuilds() {
    Builder::builder().dropFor(this);
    // The words waiting for this VM (as the program's), none to be written now.
    std::lock_guard<std::mutex> states(nativeStates);
    std::erase_if(waitingWords, [&](const WaitingWord& w) { return w.program == this; });
}

// The run code's words written elsewhere meanwhile (writeRunWord), where this VM runs the program
// alone: at a safe point, or as a parallel section ends.
void VM::writeWaitingWords() {
    if (!runsProgramAlone()) return;
    std::lock_guard<std::mutex> states(nativeStates);
    wordsWaiting_ = false;
    std::erase_if(waitingWords, [&](const WaitingWord& w) {
        if (w.program != this) return false;
        std::memcpy(w.f->run.data() + w.at, &w.word, kThreaded);
        return true;
    });
}
namespace {
const bool buildsDropped = (dropNativeBuildsOf = [](const std::vector<std::unique_ptr<FunctionProto>>& owned) { Builder::builder().dropFunctions(owned); }, true);
}  // namespace

// The second time a loop starts (once the VM has run it a time round, its places' caches have
// seen what they read, and its lists are in the frame): its regions made (by one thread or core
// at a time), published, and addressed: its first instruction a region's way in, or the
// instruction's own function again: written by the program's VM, or waiting for it where a
// section's core made them (writeRunWord); meanwhile the head's word says nativeFirst still,
// which goes into the region. A region is made whole before any way into it is published.
WW_TAIL_FN int VM::nativeFirst(WW_TAIL_PARAMS) {
    FunctionProto& f = *const_cast<FunctionProto*>(frame->closure->proto);
    const uint32_t head = static_cast<uint32_t>(ip - code) - static_cast<uint32_t>(kThreaded + kOpBytes);
    size_t which = 0;
    while (which < f.nativeHeads.size() && f.nativeHeads[which] != head) which++;
    if (which < f.nativeHeads.size() && f.nativeArrivals[which] < 1) {
        f.nativeArrivals[which] = 1;  // racing cores may each set it: as well
        ip -= kThreaded + kOpBytes;
        const TailFn own = TailTable::get()[static_cast<size_t>(opAt(ip + kThreaded))];
        sp_ = sp;
        frame->ip = ip += kThreaded + kOpBytes;
        [[clang::musttail]] return (this->*own)(WW_TAIL_ARGS);
    }
    // 1: made the first time; 3: made again, its regions dropped past their misses' limit
    // (2026-11-16). (5 and 6: being made by the builder, the head its own instruction; 2 and 4
    // made, the head's word not written yet, writeRunWord: on without the lock, as the word would.)
    if (which < f.nativeHeads.size() && (f.nativeArrivals[which] == 1 || f.nativeArrivals[which] == 3)) {
        std::unique_lock<std::mutex> states(nativeStates);
        if (f.nativeArrivals[which] == 1 || f.nativeArrivals[which] == 3) makeRegionsFor(f, head, which, states, base, sp);
    }
    // On at this instruction: into its region where one is published (nativeEnter, which does
    // its own instruction where none is), as its word may not say yet (writeRunWord).
    [[clang::musttail]] return nativeEnter(WW_TAIL_ARGS);
}

bool VM::noMemberForRegion(const void* vm, const FunctionProto& f, const ObjString* name, const Value& receiver) {
    const VM& self = *static_cast<const VM*>(vm);
    if (!f.outerReceivers.empty() || !(receiver.isObjType(ObjType::Instance) || receiver.isObjType(ObjType::Value))) return false;
    try {
        if (self.hasMember(receiver, name)) return name->restricted != 0 && self.accessDenied(receiver, name, &f, false);
        const ObjString* own = self.privateLike(classOf(receiver), name);  // o-private-shadow
        return own == nullptr || self.accessDenied(receiver, own, &f, false);
    } catch (...) {
        return false;
    }
}

void* VM::boxForRegion(void* vm, const void* cls) {
    VM& self = *static_cast<VM*>(vm);
    if (self.bytesAllocated_ > self.nextGC_) return nullptr;
    return self.newValue(const_cast<ObjClass*>(static_cast<const ObjClass*>(cls)));
}

// Room made in a packed list a region adds to, which has none left (2026-11-16): twice its
// items, at least 8, its bits for null too (listReserve, coverNulls), as the VM's add would
// grow it; false where a collection is due (the region leaves the add to the VM, which collects
// first) or the list is not packed. Only a region that has looked at no list once asks (its
// steps keep no list's block address).
bool VM::roomForRegion(void* vm, const void* list) {
    VM& self = *static_cast<VM*>(vm);
    if (self.bytesAllocated_ > self.nextGC_) return false;
    ObjList* const l = const_cast<ObjList*>(static_cast<const ObjList*>(list));
    if (!l->packed()) return false;
    const size_t want = l->pack->count * 2 > 8 ? l->pack->count * 2 : 8;
    self.listReserve(l, want);
    return true;
}

// A key a region gives the VM's way for maps and sets: one with no == or hashCode of its own,
// whose lookup runs no WinterWolf code (a number, a bool, a char, null or a string).
bool VM::plainKeyForRegion(const Value& k) {
    switch (k.kind) {
        case ValueKind::Int:
        case ValueKind::Byte:
        case ValueKind::Float:
        case ValueKind::Bool:
        case ValueKind::Char:
        case ValueKind::Null: return true;
        case ValueKind::Obj: return k.as.obj->type == ObjType::String;
        default: return false;
    }
}

// A key looked up in a map's keys or a set for a region (2026-11-16, sets-maps-plan part 1: map[k],
// set.contains(k), k in set, k in map.keys): 1 where it is there (a map's value copied to `out`,
// where given), 0 where it is not, -1 where the VM looks it up instead: the table no map, set or
// view of a map's keys, the key not a plain one (plainKeyForRegion), or a stored key met that may
// have an == of its own (ValueMap::findPlain runs no WinterWolf code).
int VM::findForRegion(const void* table, const void* key, void* out) {
    const Obj* const o = static_cast<const Obj*>(table);
    const Value& k = *static_cast<const Value*>(key);
    if (!plainKeyForRegion(k)) return -1;
    const ValueMap* m = nullptr;
    if (o->type == ObjType::Map) {
        m = &static_cast<const ObjMap*>(o)->map;
    } else if (o->type == ObjType::Set) {
        const ObjSet* const set = static_cast<const ObjSet*>(o);
        if (!set->view()) m = &set->items;
        else if (!set->entries && !set->values && out == nullptr) m = &set->viewOf->map;  // a map's keys
        else return -1;
    } else {
        return -1;
    }
    const Value* found = nullptr;
    const int r = m->findPlain(k, found);
    if (r == 1 && out != nullptr) std::memcpy(out, found, sizeof(Value));
    return r;
}

// A key set in a map, or added to a set, for a region (2026-11-16, sets-maps-plan part 2: map[k]
// = v, set.add(k)): 1 done (a set: added), 0 a set that had it, -1 where the VM does it instead:
// the table no map or set of this core's, frozen, or a view; the key not a number, a bool, a
// char, null or a string; a map's value a Ref (the VM copies it); a stored key met that may have
// an == of its own; or a collection due (the VM collects first).
int VM::setForRegion(void* vm, const void* table, const void* key, const void* value) {
    VM& self = *static_cast<VM*>(vm);
    Obj* const o = const_cast<Obj*>(static_cast<const Obj*>(table));
    const Value& k = *static_cast<const Value*>(key);
    if (!plainKeyForRegion(k) || o->core != self.coreId_ || o->frozen != 0 || self.bytesAllocated_ > self.nextGC_) return -1;
    ValueMap* m = nullptr;
    const bool isMap = o->type == ObjType::Map;
    if (isMap) {
        if (value == nullptr || static_cast<const Value*>(value)->kind == ValueKind::Ref) return -1;
        m = &static_cast<ObjMap*>(o)->map;
    } else if (o->type == ObjType::Set && !static_cast<ObjSet*>(o)->view()) {
        m = &static_cast<ObjSet*>(o)->items;
    } else {
        return -1;
    }
    const Value* found = nullptr;
    const int r = m->findPlain(k, found);
    if (r < 0) return -1;
    if (r == 1) {
        if (!isMap) return 0;
        std::memcpy(const_cast<Value*>(found), value, sizeof(Value));
        return 1;
    }
    m->addNew(k, isMap ? *static_cast<const Value*>(value) : Value::null(), ValueMap::hashOf(k));
    self.account(64);
    return 1;
}

// How many times a function is called before its whole region is made (2026-11-15): enough that
// a function called a few times, as most are, costs nothing to make; few enough that one called
// in a loop is made at once.
constexpr uint32_t kNativeCallsFirst = 64;

// A whole function's start, until its region is made: the calls counted, and at the count the
// function's returns made VM::nativeReturn's, which makes the region (as the frame then says
// what the function's locals are); the start the instruction's own function again.
WW_TAIL_FN int VM::nativeCall(WW_TAIL_PARAMS) {
    FunctionProto& f = *const_cast<FunctionProto*>(frame->closure->proto);
    uint8_t* const run = const_cast<uint8_t*>(code);
    const uint32_t head = static_cast<uint32_t>(ip - code) - static_cast<uint32_t>(kThreaded + kOpBytes);
    if (head == f.nativeCallHead) {
        // (Counted no further once counted: its word may stay this one a while, writeRunWord.)
        if (f.nativeCalls < kNativeCallsFirst) ++f.nativeCalls;  // racing cores may count one call twice: as well
        // Its region made (its word not yet the way into it): into it.
        if (f.nativeCalls == UINT32_MAX) [[clang::musttail]] return nativeEnter(WW_TAIL_ARGS);
        if (f.nativeCalls >= kNativeCallsFirst) {
            if (runsProgramAlone()) {  // (counted here, or on a section's core before)
                std::lock_guard<std::mutex> states(nativeStates);
                const TailFn back = TailTable::nativeReturnEntry();
                for (uint32_t at : f.nativeReturns) writeRunWord(this, this, f, at, back);
                writeRunWord(this, this, f, head, ownOf(run, head));
            } else {
                // On a section's core, which writes no return's word: made at this call's return,
                // or another's that gets there first.
                frame->dropResult |= kMakeWhole;
            }
        }
    }
    ip -= kThreaded + kOpBytes;
    const TailFn to_ = TailTable::get()[static_cast<size_t>(opAt(ip + kThreaded))];
    sp_ = sp;
    frame->ip = ip += kThreaded + kOpBytes;
    [[clang::musttail]] return (this->*to_)(WW_TAIL_ARGS);
}

// Its whole region made at a return of `f`, a function called often (nativeCall), from the frame
// there, its slots saying what its locals hold; once, its returns their own instructions again.
// At a return made VM::nativeReturn's, or one whose frame asks (kMakeWhole, Return's slow way).
void VM::wholeAtReturn(FunctionProto& f, const Value* base, const Value* top) {
    std::unique_lock<std::mutex> states(nativeStates);
    if (f.nativeCallHead != UINT32_MAX && f.nativeCalls >= kNativeCallsFirst && f.nativeCalls != UINT32_MAX) {
        f.nativeCalls = UINT32_MAX;  // made, or refused: never again
        for (uint32_t at : f.nativeReturns) writeRunWord(this, this, f, at, ownOf(f.run.data(), at));
        makeRegionsFor(f, f.nativeCallHead, kWholeFunction, states, base, top);
    }
}

// A return of a function called often (nativeCall): its whole region made; its start then the
// region's way in, and its returns their own.
WW_TAIL_FN int VM::nativeReturn(WW_TAIL_PARAMS) {
    wholeAtReturn(*const_cast<FunctionProto*>(frame->closure->proto), base, sp);
    ip -= kThreaded + kOpBytes;
    const TailFn to_ = TailTable::get()[static_cast<size_t>(opAt(ip + kThreaded))];
    sp_ = sp;
    frame->ip = ip += kThreaded + kOpBytes;
    [[clang::musttail]] return (this->*to_)(WW_TAIL_ARGS);
}

// Into a native code region (M9.5): its machine code run on this frame's slots from the
// instruction here, one of its ways in, and on where it gives back, with the stack's top it
// gives. `ip` is just past the instruction's opcode, where the instruction before put it.
WW_TAIL_FN int VM::nativeEnter(WW_TAIL_PARAMS) {
    const uint32_t head = static_cast<uint32_t>(ip - code) - static_cast<uint32_t>(kThreaded + kOpBytes);
    const FunctionProto::NativeRegion* region = nullptr;
    const std::vector<FunctionProto::NativeRegion>* regions = frame->closure->proto->regionsNow.load(std::memory_order_acquire);
    if (regions != nullptr)
        for (const FunctionProto::NativeRegion& r : *regions)
            if (r.head == head && *r.misses < kNativeMissLimit) {  // (past its limit: its ways in are to be the VM's again)
                region = &r;
                break;
            }
    // The region's context is laid in the frame past the function's own slots (nativeContextSlot,
    // 2026-11-19), which a frame made before the function was translated may have no room for:
    // the VM's way there.
    const int64_t contextSlot = frame->closure->proto->nativeContextSlot;
    if (contextSlot <= 0 || stackEnd_ - base < contextSlot + kNativeContextSlots) region = nullptr;
    uint32_t at = head;
    for (int chained = 0; region != nullptr; chained++) {
        // The context, its lists and held fields set to nothing only at the region's start: they
        // are made by its prologue, which no other way in runs, and read only in a region with no
        // other way in (its lists and fields looked at once where nothing of the VM's is in it but
        // on the way to a return or throw). A way in after an island, as after each call a loop
        // makes, is spared setting some 600 bytes (2026-11-13); one that runs the prologue again
        // (2026-11-17: a loop that calls out and comes back looks at its lists and fields again)
        // sets each of those it reads, as the start does. A loop template's words are never set to
        // nothing: each that its shape reads is set by the steps just before it runs.
        native::RegionContext& context = native::contextAt(base, contextSlot);
        context.attention = reinterpret_cast<const volatile uint8_t*>(&attention_);
        context.core = coreId_;
        context.band = bandObject_;
        context.globals = globals_.data();
        context.upvalues = frame->closure->upvalues.data();
        context.units = native::unitTable();
        context.vm = this;
        context.makeBox = &VM::boxForRegion;
        context.makeRoom = &VM::roomForRegion;
        context.findKey = &VM::findForRegion;
        context.setKey = &VM::setForRegion;
        context.places = nullptr;
        context.keys = reinterpret_cast<const uint64_t*>(&tableKeysChanged);
        if ((frame->closure->proto->typeArgs & kHasPlaces) != 0 && context_->places != nullptr && (framePlacesEnd(frame - frames_) & ~PlaceArena::kNone) != 0)
            if (ObjPlace* const first = placeOf(frame, 0)) context.places = reinterpret_cast<uint8_t*>(first);
        if (region->looks) {
            if (region->first) {
                std::memset(context.lists, 0, sizeof context.lists);
                std::memset(context.held, 0, sizeof context.held);
            }
            std::memset(context.heldAt, 0, sizeof context.heldAt);
        }
        const int64_t out = native::runRegion(region->code, base);
        // WW_TRACE_EXITS=n: the first n ways in and out (at least 30).
        static const int traceExits = std::getenv("WW_TRACE_EXITS") == nullptr ? 0 : std::max(std::atoi(std::getenv("WW_TRACE_EXITS")), 30);
        static int traced = 0;
        if (traced < traceExits) {
            traced++;
            std::fprintf(stderr, "region %s: in at %u, out at %u%s\n", frame->closure->proto->name.c_str(), at, static_cast<uint32_t>(out),
                         ((static_cast<uint64_t>(out) >> 48) & 1) != 0 ? " (could not)" : "");
        }
        sp = base + ((static_cast<uint64_t>(out) >> 32) & 0xFFFF);
        ip = code + static_cast<uint32_t>(out);
        // A quick way that could not: past a limit, the region's ways in are the VM's own
        // instructions again (as writeRunWord writes them).
        const bool missed = ((static_cast<uint64_t>(out) >> 48) & 1) != 0;
        if (missed && ++*region->misses == kNativeMissLimit) {
            std::lock_guard<std::mutex> states(nativeStates);
            FunctionProto& fp = *const_cast<FunctionProto*>(frame->closure->proto);
            for (const FunctionProto::NativeRegion& r : *regions)
                if (r.misses == region->misses) writeRunWord(this, this, fp, r.head, ownOf(code, r.head));
            // A loop's regions made again, once, from the frame the next time the loop starts
            // (2026-11-16: made for a list holding no null, their steps miss on one that may hold
            // some; made from a frame showing such a list, they read both kinds).
            for (const FunctionProto::NativeRegion& r : *regions) {
                if (r.misses != region->misses || !r.first) continue;
                // A whole function's (2026-11-16): made again at its next return, from the frame
                // there, once (nativeReturn).
                if (r.head == fp.nativeCallHead && !fp.nativeCallsAgain && fp.nativeCalls == UINT32_MAX) {
                    fp.nativeCallsAgain = true;
                    fp.nativeCalls = kNativeCallsFirst;
                    const TailFn back = TailTable::nativeReturnEntry();
                    for (uint32_t at : fp.nativeReturns) writeRunWord(this, this, fp, at, back);
                }
                for (size_t k = 0; k < fp.nativeHeads.size(); k++)
                    if (fp.nativeHeads[k] == r.head && fp.nativeArrivals[k] == 2) {
                        fp.nativeArrivals[k] = 3;
                        writeRunWord(this, this, fp, r.head, TailTable::nativeFirstEntry());
                    }
            }
        }
        // Counted kCountedRounds times round (registers-plan.html part B): made again from its
        // counts. Made here, the VM goes on into the new one (not this one, chained).
        if (RegionCounts* const counted = region->counts; counted != nullptr && counted->runs[counted->at.size() + 1] == 2) {
            counted->runs[counted->at.size() + 1] = 1;  // (left once: not again till it is made again, or that dropped)
            if (!counted->asked && remakeCounted(*const_cast<FunctionProto*>(frame->closure->proto), *region, base, sp)) break;
        }
        // Left at another region's way in (a loop's region where its function's goes on after
        // the loop, and the function's at the start of a loop left to its own, 2026-11-15):
        // straight into that one, as the VM would at once. Not where this one could not do what
        // it left at (the VM does it), and not round and round.
        const uint32_t next = static_cast<uint32_t>(out);
        if (missed || chained == 3 || next == at) break;
        const TailFn enter = TailTable::nativeEntry();
        if (std::memcmp(code + next, &enter, kThreaded) != 0) break;
        const FunctionProto::NativeRegion* other = nullptr;
        for (const FunctionProto::NativeRegion& r : *regions)
            if (r.head == next && *r.misses < kNativeMissLimit) {
                other = &r;
                break;
            }
        if (other == nullptr || other->misses == region->misses) break;
        region = other;
        at = next;
    }
    if (region == nullptr) ip = code + head;
    // The VM does the instruction the region left at: its own function, never this one again,
    // where the instruction is another way into the region.
    const TailFn to_ = TailTable::get()[static_cast<size_t>(opAt(ip + kThreaded))];
    sp_ = sp;
    frame->ip = ip += kThreaded + kOpBytes;
    [[clang::musttail]] return (this->*to_)(WW_TAIL_ARGS);
}

#undef CASE
#undef WW_DECLARE
#undef WW_FORWARD
#undef NEXT
#undef DONE
#undef EXIT_RUN
#undef RUN_BASE
#undef LABEL
#undef JUMP_LABEL
#endif
#if !WW_TAIL_CALLS
void VM::markWaitingBuilds() {}
void VM::waitForBuilds() {}
void VM::dropWaitingBuilds() {}
void VM::writeWaitingWords() {}
void VM::wholeAtReturn(FunctionProto&, const Value*, const Value*) {}
#endif

#undef LOAD_FRAME
#undef SAVE_IP
#undef READ_U8
#undef READ_U16
#undef READ_I32
#undef GC_SAFE_POINT
#undef ARITHMETIC
#undef COMPARISON

// What a function returns that is a Ref (M6.6 part 4): one into a list, or into an object
// (part 5), goes no further than the function that made it, unless it is where the caller asked
// for the result to go.
#ifdef _MSC_VER
__declspec(noinline)
#else
__attribute__((noinline))
#endif
Value VM::returnedRef(Value result, ptrdiff_t frameIndex) {
    // Given a place of the caller's for its result (D149): a value where it is, in a list, an
    // object or a place, copied into it, with no box made.
    if ((frames_[frameIndex].closure->proto->typeArgs & kTakesDestination) != 0) {
        const Value dest = frameDestination(frameIndex);
        if (dest.isRef() && dest.as.obj->type == ObjType::Places && dest.refDepth == 0) {
            ObjClass* cls = refClass(result);
            if (cls != nullptr && layoutOf(cls).size <= ObjPlace::kBytes) {
                ObjPlace* place = static_cast<ObjPlace*>(dest.as.obj);
                const ObjTypeInfo* type = recordedType(result);
                uint8_t* to = refBytes(dest);
                const uint8_t* from = refAt(result, false);
                if (to != from) std::memmove(to, from, layoutOf(cls).size);
                place->cls = cls;
                place->type = type;
                return dest;
            }
        }
    }
    // A value in a place given back with nowhere said for it to go: a box, as the place may be the
    // frame's own, given back as it returns.
    if (result.as.obj->type == ObjType::Places) return ownRef(result);
    if ((frames_[frameIndex].closure->proto->typeArgs & kTakesDestination) != 0) {
        const Value& dest = frameDestination(frameIndex);
        if (dest.kind == ValueKind::Undefined) return result;  // the caller keeps it as it is (Op::WithRefResult)
        if (dest.isRef() && dest.as.obj == result.as.obj && dest.refIndex == result.refIndex && dest.refOffset == result.refOffset &&
            dest.refDepth == result.refDepth)
            return result;
    }
    if (result.as.obj->type != ObjType::List && result.as.obj->type != ObjType::Instance) return result;
    return ownRef(result);
}

// The instructions that give a place (M6.6 part 4), and InsertNew, which makes an item in its
// place (G7), kept out of run() so that what they need does not weigh on the instructions run
// most. Gives where the next instruction is.
#ifdef _MSC_VER
__declspec(noinline)
#else
__attribute__((noinline))
#endif
const uint8_t* VM::placeInstruction(Op op, const uint8_t* ip, CallFrame* frame) {
    const Value* const consts = frame->closure->proto->constants.data();
    switch (op) {
        case Op::PlaceLocal: {
            Value& slot = frame->base[(ip += 2, operandU16(ip - 2))];
            const Value place = placeIn(slot);
            *sp_++ = place;
            break;
        }
        case Op::PlaceUpvalue: {
            ObjUpvalue* upvalue = frame->closure->upvalues[static_cast<size_t>((ip += 2, operandU16(ip - 2)))];
            if (upvalue->core != coreId_) throw notOwned("a variable from outside the section is shared");
            const Value place = placeIn(*upvalue->location);
            *sp_++ = place;
            break;
        }
        case Op::PlaceGlobal: {
            const int index = (ip += 2, operandU16(ip - 2));
            if (coreId_ != 0) throw notOwned("the variable '" + program_->globalNames[static_cast<size_t>(index)] + "' is shared");
            if (globals_[static_cast<size_t>(index)].isLazy()) globalOrLazy(static_cast<size_t>(index));
            const Value place = placeIn(globals_[static_cast<size_t>(index)]);
            *sp_++ = place;
            break;
        }
        case Op::PlaceField: {
            ObjString* name = asString(consts[(ip += 2, operandU16(ip - 2))]);
            const Value obj = sp_[-1];
            if (obj.isRef() || obj.isObjType(ObjType::Value)) {
                // A field of a value that is itself a place: where it is in it.
                Value read;
                if (obj.isRef() ? refField(obj, name, read) : false) {
                    sp_[-1] = read;
                    break;
                }
            }
            if (!obj.isObjType(ObjType::Instance)) throw runtimeError(Thrown::UnsupportedOperationException, "cannot change a field of a value of type " + typeName(obj));
            ObjInstance* instance = asInstance(obj);
            guardWrite(instance);
            const FieldDecl* decl = instance->declOf(name);
            if (decl == nullptr) throw runtimeError(Thrown::UnsupportedOperationException, "undefined property '" + name->utf8() + "' on " + instance->cls->name->utf8());
            if (decl->isVal) throw runtimeError(Thrown::UnsupportedOperationException, "cannot assign to val field '" + name->utf8() + "' of " + instance->cls->name->utf8());
            if (decl->payload != FieldDecl::NoPayload) {
                Value kept = Value::null();
                instance->read(name, kept);
                throw runtimeError(Thrown::NullPointerException, "cannot change a field of " + (kept.isNull() ? std::string("null") : "a value of type " + typeName(kept)) +
                                   " through its place");
            }
            sp_[-1] = placeIn(instance->fields()[decl->slot]);
            break;
        }
        case Op::PlaceIndex: {
            const Value index = sp_[-1];
            const Value obj = sp_[-2];
            if (!obj.isObjType(ObjType::List)) throw runtimeError(Thrown::UnsupportedOperationException, "cannot change an item of a value of type " + typeName(obj));
            ObjList* list = asList(obj);
            guardWrite(list);
            size_t i;
            if (!position(index, list->size(), i, "list index")) throw outOfRange("list index", index, list->size());
            sp_--;
            if (list->store() == ListStore::Items) {
                if (list->pack->nullable && (list->pack->nulls[i / 64] >> (i % 64) & 1)) sp_[-1] = Value::null();  // for `pts[i]?.move()` to pass by
                else sp_[-1] = Value::ref(list, static_cast<uint32_t>(i), 0, 0);  // the item, where it is
            } else {
                sp_[-1] = placeIn(list->values()[i]);
            }
            break;
        }
        case Op::InsertNew: {
            const int argc = *ip;
            const int target = operandI32(ip + 1);
            ip += 5;
            if (insertNew(argc & 0x7F, (argc & 0x80) != 0)) ip = frame->closure->proto->run.data() + target;
            break;
        }
        default:
            break;
    }
    return ip;
}

// `pts.insert(i, Point(1, 2))` and `pts.add(i, Point(1, 2))` (Op::InsertNew, G7): as addNew, the
// position under the class, an int from 0 to the list's size; the item is made at the end and
// moved to its place, as an insert moves the items after it. Its result is null, as insert
// gives, or true, as add(i, item) gives. False, with the stack as it was, where addNew does
// nothing or the position is not one, and the call and the insert or add follow and say why.
// Kept after run(), as addNew is before it, so that run() stays where it was (G7: numeric was 14%
// slower with run() moved).
bool VM::insertNew(int argc, bool givesTrue) {
    Value* const args = sp_ - argc;
    if (!args[-3].isObjType(ObjType::List)) return false;
    ObjList* list = asList(args[-3]);
    const Value position = args[-2];
    if (list->store() != ListStore::Items || !position.isInt() || position.as.i < 0 || static_cast<uint64_t>(position.as.i) > list->pack->count)
        return false;
    const size_t at = static_cast<size_t>(position.as.i);
    // The position taken from under the class for addNew, and put back if it does nothing.
    std::memmove(args - 2, args - 1, static_cast<size_t>(argc + 1) * sizeof(Value));
    sp_--;
    if (!addNew(argc)) {
        std::memmove(args - 1, args - 2, static_cast<size_t>(argc + 1) * sizeof(Value));
        args[-2] = position;
        sp_++;
        return false;
    }
    PackedItems& pack = *list->pack;
    const size_t width = pack.width;
    // The new item kept aside, the items after `at` moved on by one in one move, and the item put
    // in the gap (std::rotate swaps byte by byte: twice as slow at the front of a long list).
    uint8_t* const base = pack.block.data();
    const size_t last = (pack.count - 1) * width;
    uint8_t small[256];
    std::vector<uint8_t> large;
    uint8_t* const made = width <= sizeof small ? small : (large.resize(width), large.data());
    std::memcpy(made, base + last, width);
    std::memmove(base + (at + 1) * width, base + at * width, last - at * width);
    std::memcpy(base + at * width, made, width);
    if (pack.nullable) {
        for (size_t i = pack.count - 1; i > at; i--) setNullAt(list, i, nullAt(list, i - 1));
        setNullAt(list, at, false);
    }
    if (!givesTrue) sp_[-1] = Value::null();
    return true;
}

// The instructions of types when the program runs (M6.6 part 1), kept out of run()'s loop: they
// are seldom run, and code in the loop costs every other instruction there. Gives where the
// next instruction is.
#ifdef _MSC_VER
__declspec(noinline)
#else
__attribute__((noinline))
#endif
const uint8_t* VM::typeInstruction(Op op, const uint8_t* ip, CallFrame* frame) {
    const uint8_t* const code = frame->closure->proto->run.data();
    const Value* const consts = frame->closure->proto->constants.data();
    std::atomic<uint64_t>* const caches = frame->closure->proto->caches.get();
    switch (op) {
        case Op::MakeType: {
            const uint8_t* site = ip - 1;
            const std::string& shape = asString(consts[(ip += 2, operandU16(ip - 2))])->utf8();
            const int count = shapeOperands(shape);
            const Value* operand = sp_ - count;
            // The same types as the last time here: the same type, with no look-up in the table
            // of types, which a core takes a lock for (G3). Only types are keys: they are never
            // freed, where a class may be and its address taken again.
            MadeTypeCache* kept = nullptr;
            if (count >= 1 && count <= MadeTypeCache::kMost) {
                bool types = true;
                for (int k = 0; k < count; k++) types = types && operand[k].isObjType(ObjType::TypeInfo);
                if (types) {
                    kept = &madeTypes_[(reinterpret_cast<uintptr_t>(site) >> 2) % kMadeTypeCaches];
                    const uint64_t age = (root_ != nullptr ? root_ : this)->collections_;
                    bool same = kept->site == site && kept->age == age;
                    for (int k = 0; same && k < count; k++) same = kept->operands[k] == operand[k].as.obj;
                    if (same) {
                        sp_ -= count;
                        *sp_++ = Value::object(kept->made);
                        break;
                    }
                }
            }
            size_t at = 0;
            const Value* read = operand;  // typeFromShape moves it past what it reads
            ObjTypeInfo* made = typeFromShape(shape, at, read);
            if (kept != nullptr) {
                kept->site = site;
                kept->age = (root_ != nullptr ? root_ : this)->collections_;
                for (int k = 0; k < count; k++) kept->operands[k] = operand[k].as.obj;
                kept->made = made;
            }
            sp_ -= count;
            *sp_++ = Value::object(made);
            break;
        }
        case Op::FrameTypes:
            pendingTypeArgs_ = frameTypeArgs(frame - frames_);
            break;
        case Op::ObjectClassTyped: {
            // An anonymous object's class made with the type arguments the frame has, made
            // before (G66). A core of a parallel section makes its own each time, as StoreClass.
            const int global = (ip += 2, operandU16(ip - 2));
            const int target = (ip += 4, operandI32(ip - 4));
            if (coreId_ == 0) {
                const auto found = typedClasses_.find(typedClassKey(global, frameTypeArgs(frame - frames_)));
                if (found != typedClasses_.end()) {
                    *sp_++ = Value::object(found->second);
                    ip = code + target;
                }
            }
            break;
        }
        case Op::StoreClassTyped: {
            const int global = (ip += 2, operandU16(ip - 2));
            if (coreId_ == 0 && sp_[-1].isObjType(ObjType::Class))
                typedClasses_[typedClassKey(global, frameTypeArgs(frame - frames_))] = asClass(sp_[-1]);
            break;
        }
        case Op::CachedType: {
            const int cache = (ip += 2, operandU16(ip - 2));
            const int target = (ip += 4, operandI32(ip - 4));
            const uint64_t kept = caches != nullptr ? caches[cache].load(std::memory_order_acquire) : 0;
            if (kept != 0) {
                *sp_++ = Value::object(reinterpret_cast<ObjTypeInfo*>(static_cast<uintptr_t>(kept)));
                ip = code + target;
            }
            break;
        }
        case Op::CacheType: {
            const int cache = (ip += 2, operandU16(ip - 2));
            if (caches != nullptr && sp_[-1].isObjType(ObjType::TypeInfo))
                caches[cache].store(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(sp_[-1].as.obj)), std::memory_order_release);
            break;
        }
        case Op::TypeCollection: {
            if (!sp_[-1].isObjType(ObjType::TypeInfo)) throw runtimeError(Thrown::InternalError, "invalid type");
            ObjTypeInfo* type = static_cast<ObjTypeInfo*>(sp_[-1].as.obj);
            const Value made = sp_[-2];
            // What it was made to hold, said once: a list a C++ function gave with a type of
            // its own keeps it.
            if (made.isObjType(ObjType::List)) typeList(asList(made), type);
            else if (made.isObjType(ObjType::Map) && type->id == kTypeMap && asMap(made)->type == nullptr) asMap(made)->type = type;
                        else if (made.isObjType(ObjType::Set) && type->id == kTypeSet && asSet(made)->type == nullptr) asSet(made)->type = type;
            sp_--;
            break;
        }
        case Op::IsFull: {
            if (!sp_[-1].isObjType(ObjType::TypeInfo)) throw runtimeError(Thrown::InternalError, "invalid type");
            const bool fits = fitsType(sp_[-2], static_cast<const ObjTypeInfo*>(sp_[-1].as.obj));
            sp_[-2].setBool(fits);
            sp_--;
            break;
        }
        case Op::WithTypes: {
            if (!sp_[-1].isObjType(ObjType::TypeInfo)) throw runtimeError(Thrown::InternalError, "invalid type");
            pendingTypeArgs_ = static_cast<const ObjTypeInfo*>(sp_[-1].as.obj);
            sp_--;
            break;
        }
        case Op::TypeArg: {
            const int index = (*ip++);
            *sp_++ = Value::object(typeArgument(frameTypeArgs(frame - frames_), index));
            break;
        }
        case Op::InstanceTypeArg: {
            // The type argument of the class that declares the code, which the object may extend.
            const int index = (*ip++);
            const ObjTypeInfo* type = nullptr;
            if (sp_[-2].isObjType(ObjType::Instance) && sp_[-1].isObjType(ObjType::Class)) {
                ObjInstance* instance = asInstance(sp_[-2]);
                type = instance->cls == asClass(sp_[-1]) ? instance->type() : typeAs(instance->cls, instance->type(), asClass(sp_[-1]));
            }
            sp_--;
            sp_[-1] = Value::object(typeArgument(type, index));
            break;
        }
        case Op::CheckResult: {
            const std::string& name = asString(consts[(ip += 2, operandU16(ip - 2))])->utf8();
            if (!sp_[-1].isObjType(ObjType::TypeInfo)) throw runtimeError(Thrown::InternalError, "invalid type");
            const ObjTypeInfo* type = static_cast<const ObjTypeInfo*>(sp_[-1].as.obj);
            if (!fitsType(sp_[-2], type))
                throw runtimeError(Thrown::InternalError, "the C++ function " + name + "() gave a value of type " + valueTypeText(sp_[-2]) + ", where its declaration says " +
                                   typeInfoText(type));
            sp_--;
            break;
        }
        case Op::ClassOf: {
            const int how = *ip++;
            Value& top = sp_[-1];
            ObjTypeInfo* type = nullptr;
            if (how == 0) {
                if (!top.isObjType(ObjType::TypeInfo)) throw runtimeError(Thrown::InternalError, "invalid type");
                type = static_cast<ObjTypeInfo*>(top.as.obj);
            } else {
                frame->ip = ip;
                type = typeOfValue(top);
            }
            top = Value::object(type);
            *sp_++ = Value::object(frame->closure);
            break;
        }
        case Op::AtomicField: {
            ObjString* name = asString(frame->closure->proto->constants[static_cast<size_t>(operandU16(ip))]);
            ip += 2;
            const int kind = *ip++;
            frame->ip = ip;
            const int operands = kind == 4 ? 2 : kind == 5 ? 0 : 1;
            const Value result = atomicField(sp_[-operands - 1], name, kind, sp_ - operands);
            sp_ -= operands;
            sp_[-1] = result;
            break;
        }
        case Op::ClassMeta: {
            const FunctionProto* proto = frame->closure->proto->protos[operandU16(ip)];
            ip += 2;
            const int which = *ip++;
            if (!sp_[-1].isObjType(ObjType::Class) || which > 1) throw runtimeError(Thrown::InternalError, "invalid class declaration");
            ObjClass* cls = asClass(sp_[-1]);
            cls->metaProto[which] = proto;
            cls->meta[which] = nullptr;
            break;
        }
        case Op::SupertypeMaker: {
            if (!sp_[-2].isObjType(ObjType::Class) || !sp_[-1].isObjType(ObjType::Closure)) throw runtimeError(Thrown::InternalError, "invalid class declaration");
            asClass(sp_[-2])->supertypeMakers.push_back(asClosure(sp_[-1]));
            sp_--;
            break;
        }
        case Op::TypeSlots: {
            const std::string& slotted = asString(consts[(ip += 2, operandU16(ip - 2))])->utf8();
            if (!sp_[-1].isObjType(ObjType::Class)) throw runtimeError(Thrown::InternalError, "invalid class declaration");
            ObjClass* cls = asClass(sp_[-1]);
            for (size_t from = 0; from < slotted.size();) {
                const size_t end = slotted.find('\n', from);
                if (end == std::string::npos) break;
                // A private field's line: , its owner, a tab and its text (o-private-shadow).
                const std::string_view line = std::string_view(slotted).substr(from, end - from);
                const size_t tab = line.rfind('	');
                ObjString* const name = !line.empty() && line[0] == '' && tab != std::string_view::npos
                                            ? privateName(line.substr(1, tab - 1), line.substr(tab + 1))
                                            : intern(line);
                if (const Value* at = cls->declaredIndex.find(name))
                    cls->typeSlotted.push_back(static_cast<uint32_t>(at->as.i));
                from = end + 1;
            }
            break;
        }
        case Op::FieldTypesMaker: {
            if (!sp_[-2].isObjType(ObjType::Class) || !sp_[-1].isObjType(ObjType::Closure)) throw runtimeError(Thrown::InternalError, "invalid class declaration");
            asClass(sp_[-2])->fieldTypesMaker = asClosure(sp_[-1]);
            sp_--;
            break;
        }
        case Op::CastFull: {
            const bool safe = (*ip++) != 0;
            if (!sp_[-1].isObjType(ObjType::TypeInfo)) throw runtimeError(Thrown::InternalError, "invalid type");
            const ObjTypeInfo* type = static_cast<const ObjTypeInfo*>(sp_[-1].as.obj);
            if (!fitsType(sp_[-2], type)) {
                if (!safe) {
                    frame->ip = ip;
                    throw runtimeError(Thrown::ClassCastException, "cannot cast a value of type " + valueTypeText(sp_[-2]) + " to " + typeInfoText(type));
                }
                sp_[-2] = Value::null();
            } else if (type->id == kTypeInstance && !type->args.empty() && type->cls != nullptr && type->cls->isValue && classOf(sp_[-2]) == type->cls) {
                // A value seen as of narrower type arguments than it keeps is a copy that keeps
                // those, which nothing can undo behind it (G4).
                const ObjTypeInfo* kept = recordedType(sp_[-2]);
                const ObjTypeInfo* as = type->nullable ? typeOf(kTypeInstance, false, type->cls, type->args) : type;
                if (kept != as && !(kept != nullptr && isSubtype(kept, as))) {
                    frame->ip = ip;
                    sp_[-2] = retyped(sp_[-2], as);
                }
            }
            sp_--;
            break;
        }
        default:
            break;
    }
    return ip;
}

// ---- Converting values to text ----

std::string VM::stringify(Value v) {
    TempRoot root(*this, v);
    std::string text;
    stringifyInto(text, v, false, 0);
    return text;
}

std::string VM::repr(Value v) {
    TempRoot root(*this, v);
    std::string text;
    stringifyInto(text, v, true, 0);
    return text;
}

// `v` must be reachable by the collector: a toString() method may run and trigger it.
void VM::stringifyInto(std::string& out, Value v, bool quote, int depth) {
    if (v.isRef() || v.isSmall()) v = boxed(v);  // printed as the value it is
    TempRoot keepValue(*this, v);
    switch (v.kind) {
        case ValueKind::Null: out += "null"; return;
        case ValueKind::Bool: out += v.as.b ? "true" : "false"; return;
        case ValueKind::Int:
        case ValueKind::Byte: out += std::to_string(v.as.i); return;
        case ValueKind::Float: out += formatFloat(v.as.d); return;
        case ValueKind::Char:
            if (quote) out += quoteCharacter(static_cast<uint32_t>(v.as.i));
            else appendUtf8(out, static_cast<uint32_t>(v.as.i));
            return;
        case ValueKind::FromEnd: out += "^" + std::to_string(v.as.i); return;
        case ValueKind::Undefined: out += "undefined"; return;
        case ValueKind::Ref:
        case ValueKind::Small:     // made boxes above
        case ValueKind::EnumName:  // the bare name it holds, a string
        case ValueKind::Obj: break;
    }

    guardRead(v.as.obj);
    switch (v.as.obj->type) {
        case ObjType::String:
            if (quote) out += quoteString(asString(v)->utf8());
            else out += asString(v)->utf8();
            return;

        case ObjType::List: {
            if (depth > 16) {
                out += "[...]";
                return;
            }
            ObjList* list = asList(v);
            out += '[';
            // Indexed and re-checked each time round: a toString() may change the list.
            for (size_t i = 0; i < list->size(); i++) {
                if (i > 0) out += ", ";
                const Value item = listGet(list, i);
                TempRoot keep(*this, item);
                stringifyInto(out, item, true, depth + 1);
            }
            out += ']';
            return;
        }

        case ObjType::Map: {
            if (depth > 16) {
                out += "{...}";
                return;
            }
            ObjMap* map = asMap(v);
            out += map->bundle ? "params(" : "{";
            bool first = true;
            for (size_t i = 0; i < map->map.entries().size(); i++) {
                ValueMap::Entry entry = map->map.entries()[i];
                if (!entry.live) continue;
                if (!first) out += ", ";
                first = false;
                stringifyInto(out, entry.key, !map->bundle, depth + 1);  // a bundle's names as written
                out += map->bundle ? " = " : ": ";
                stringifyInto(out, entry.value, true, depth + 1);
            }
            out += map->bundle ? ')' : '}';
            return;
        }

        case ObjType::Set: {
            // A map's values (G22) are written as a list is: they may repeat, and have an order.
            const bool values = asSet(v)->values;
            if (depth > 16) {
                out += values ? "[...]" : "{...}";
                return;
            }
            out += values ? '[' : '{';
            const std::vector<Value> items = setItems(asSet(v));
            TempVector keep(*this, const_cast<std::vector<Value>&>(items));
            for (size_t i = 0; i < items.size(); i++) {
                if (i > 0) out += ", ";
                stringifyInto(out, items[i], true, depth + 1);
            }
            out += values ? ']' : '}';
            return;
        }

        case ObjType::Bytes: {
            const std::vector<uint8_t>& data = asBytes(v)->data;
            out += "Bytes[";
            for (size_t i = 0; i < data.size() && i < 32; i++) {
                if (i > 0) out += ", ";
                out += std::to_string(data[i]);
            }
            if (data.size() > 32) out += ", ... " + std::to_string(data.size() - 32) + " more";
            out += ']';
            return;
        }

        case ObjType::Range: {
            ObjRange* range = asRange(v);
            out += (range->startFromEnd ? "^" : "") + std::to_string(range->start);
            out += range->inclusive ? ".." : "..<";
            out += (range->endFromEnd ? "^" : "") + std::to_string(range->end);
            if (range->step != 1) out += " step " + std::to_string(range->step);
            return;
        }

        case ObjType::Closure:
            out += "<fun " + asClosure(v)->proto->name + ">";
            return;
        case ObjType::Native:
            out += "<native fun " + asNative(v)->name + ">";
            return;
        case ObjType::BoundMethod:
            out += "<fun " + asBoundMethod(v)->method->proto->name + ">";
            return;
        case ObjType::BoundNative:
            out += "<native fun " + asBoundNative(v)->method->name + ">";
            return;
        case ObjType::Class:
            out += "<class " + asClass(v)->name->utf8() + ">";
            return;

        case ObjType::Value: {
            // A value prints as a data object does, Point(x=1, y=2), unless it says otherwise.
            ObjValue* box = asValue(v);
            const Value* method = box->cls->methods.find(names.toString);
            if (method != nullptr && asClosure(*method)->proto->arity == 0) {
                Value result = callWithReceiver(v, asClosure(*method));
                if (result.isString()) {
                    out += asString(result)->utf8();
                } else {
                    TempRoot root(*this, result);
                    stringifyInto(out, result, false, depth + 1);
                }
                return;
            }
            out += box->cls->name->utf8() + "(";
            if (depth > 20) {
                out += "...)";
                return;
            }
            bool first = true;
            for (ObjString* part : box->cls->components) {
                if (!first) out += ", ";
                first = false;
                out += part->utf8() + "=";
                const int64_t at = valueFieldIndex(box, part);
                const Value field = at >= 0 ? valueField(box, static_cast<size_t>(at)) : Value::null();
                TempRoot keep(*this, field);
                stringifyInto(out, field, true, depth + 1);
            }
            out += ")";
            return;
        }
        case ObjType::Instance: {
            ObjInstance* instance = asInstance(v);
            const Value* method = instance->cls->methods.find(names.toString);
            if (method != nullptr && asClosure(*method)->proto->arity == 0) {
                Value bound = Value::object(alloc<ObjBoundMethod>(v, asClosure(*method)));
                Value result = call(bound, 0, nullptr);
                if (result.isString()) {
                    out += asString(result)->utf8();
                } else {
                    TempRoot root(*this, result);
                    stringifyInto(out, result, false, depth + 1);
                }
                return;
            }
            if (instance->cls->isData) {
                // A data object prints as its class and header fields: Point(x=1, y=2).
                out += instance->cls->name->utf8() + "(";
                if (depth > 20) {
                    out += "...)";
                    return;
                }
                bool first = true;
                for (ObjString* part : instance->cls->components) {
                    if (!first) out += ", ";
                    first = false;
                    out += part->utf8() + "=";
                    Value field = Value::null();
                    instance->read(part, field);
                    stringifyInto(out, field, true, depth + 1);
                }
                out += ")";
                return;
            }
            if (instance->cls->isEnum) {
                // An enum entry prints as its name.
                Value entryName = Value::null();
                instance->read(names.entryName, entryName);
                if (entryName.isString()) {
                    out += asString(entryName)->utf8();
                    return;
                }
            }
            out += "<" + instance->cls->name->utf8() + " instance>";
            return;
        }

        case ObjType::Fiber:
            out += "<task>";
            return;

        case ObjType::Resource:
            out += std::string("<") + static_cast<const ObjResource*>(v.as.obj)->kind + ">";
            return;

        case ObjType::Upvalue:
        case ObjType::Iterator:
        case ObjType::Lazy:
        case ObjType::Places:
            out += "<internal>";
            return;

        case ObjType::TypeInfo:
            out += typeInfoText(static_cast<const ObjTypeInfo*>(v.as.obj));
            return;
    }
}

// ---- Garbage collector (mark and sweep) ----

void VM::collectGarbage() {
    liveBytes_ = 0;
    collections_++;  // what MadeType kept may name code freed now

    // The context that is running, whose state is in the VM's own members...
    for (Value* slot = stack_; slot < sp_; slot++) markValue(*slot);
    for (int i = 0; i < frameCount_; i++) {
        markObject(frames_[static_cast<size_t>(i)].closure);
        if ((frames_[static_cast<size_t>(i)].closure->proto->typeArgs & kTakesDestination) != 0) markValue(frameDestination(i));
    }
    for (ObjUpvalue* upvalue = openUpvalues_; upvalue != nullptr; upvalue = upvalue->nextOpen) markObject(upvalue);
    markObject(context_->fiber);
    markValue(pendingDestination_);
    for (ObjClass* cls : smallKept_) markObject(cls);
    for (const auto& [key, cls] : typedClasses_) markObject(cls);
    // ...and every other one that a thread is in the middle of: the contexts under the
    // running one, and those of the threads that are waiting for the lock or blocked.
    for (const ThreadState* thread : threads_) {
        for (const Context* context = thread->current; context != nullptr; context = context->parent)
            if (context != context_) markContext(*context);
        markValue(thread->function);
        markValue(thread->self);
        if (thread == thread_) continue;  // its temporaries are the VM's, marked below
        for (const Value& root : thread->tempRoots) markValue(root);
        for (const std::vector<Value>* values : thread->tempVectors)
            for (const Value& value : *values) markValue(value);
    }
    for (ObjFiber* fiber : waitingFibers_) markObject(fiber);
    markValue(workerSelf);
    markValue(unaskedDeliver);
    markValue(uncaughtHandler);
    for (const Value& error : freedFailures) markValue(error);
    for (const auto& [job, error] : unaskedFailures) markValue(error);  // the job is held weakly
    for (const auto& worker : workerObjects) markValue(worker.second);
    for (const auto& handed : scriptHandles) markValue(handed.second.value);
    for (const Value& task : loopTasks_) markValue(task);
    for (const Timer& timer : timers_) markValue(timer.task);
    for (const Value& global : globals_) markValue(global);
    markWaitingBuilds();
    for (const Table& table : typeExtensions_) markTable(table);
    for (const std::deque<BuiltinFit>& fits : builtinFits_)
        for (const BuiltinFit& fit : fits) {
            markObject(fit.type);
            markObject(fit.iface);
            markTable(fit.getters);
            markTable(fit.methods);
        }
    for (const auto& [key, type] : types_) markObject(type);  // what the types name, their classes
    markTable(nullExtensions_);
    markTable(nullExtensionOwners_);
    markTable(anyExtensions_);
    for (const auto& builtin : builtins_) markValue(builtin.second);
    for (const Value& root : tempRoots) markValue(root);
    for (const Value& root : pinned) markValue(root);
    for (const std::vector<Value>* values : tempVectors)
        for (const Value& value : *values) markValue(value);
    markObject(errorClass_);
    for (ObjClass* cls : thrownClasses_) markObject(cls);
    markObject(mapEntryClass_);
    markObject(viewClass_);
    markObject(imageCanvasClass_);
    for (const TypeConstraint& declared : globalTypes_) markObject(declared.cls);

    while (!gray_.empty()) {
        Obj* obj = gray_.back();
        gray_.pop_back();
        blacken(obj);
    }
    // The fields extends add (D157): what an object's entry holds lives while the object does,
    // and may reach objects with entries of their own; an entry goes with its object.
    if (!extras_.empty()) {
        for (;;) {
            for (const auto& [obj, record] : extras_)
                if (obj->marked || obj->immortal) markTable(record);
            if (gray_.empty()) break;
            while (!gray_.empty()) {
                Obj* obj = gray_.back();
                gray_.pop_back();
                blacken(obj);
            }
        }
        for (auto it = extras_.begin(); it != extras_.end();)
            it = it->first->marked || it->first->immortal ? std::next(it) : extras_.erase(it);
    }
    // A failed job nothing asked about, now nothing can (8.7a): its error is reported on the
    // event loop, where WinterWolf code may run.
    for (auto it = unaskedFailures.begin(); it != unaskedFailures.end();) {
        if (it->first->marked || it->first->immortal) {
            ++it;
            continue;
        }
        freedFailures.push_back(it->second);
        it = unaskedFailures.erase(it);
    }
    if (!freedFailures.empty() && !deliveryPosted) {
        deliveryPosted = true;
        postNative([](VM& vm) {
            vm.deliveryPosted = false;
            vm.deliverUnasked(false);
        });
    }

    releaseUnreachedBlocks();
    sweep();
    bytesAllocated_ = liveBytes_;
    nextGC_ = gcStress_ ? 0 : std::max(liveBytes_ * 2, kMinHeap);
    // A script's heap limit (limitMemory): passed where what is live is past it; else the next
    // collection comes before the heap could be far past it.
    if (memoryLimit_ > 0) {
        if (liveBytes_ > memoryLimit_) limitPassed(memoryLimitText());
        nextGC_ = std::min(nextGC_, std::max(memoryLimit_, liveBytes_ + memoryLimit_ / 4));
    }
}

void VM::markContext(const Context& context) {
    for (const Value* slot = context.stack; slot < context.sp; slot++) markValue(*slot);
    for (int i = 0; i < context.frameCount; i++) {
        markObject(context.frames[i].closure);
        if ((context.frames[i].closure->proto->typeArgs & kTakesDestination) != 0)
            markValue(reinterpret_cast<const Value*>(reinterpret_cast<const ObjTypeInfo* const*>(context.frames + context.maxFrames) +
                                                     context.maxFrames)[i]);
    }
    for (ObjUpvalue* upvalue = context.openUpvalues; upvalue != nullptr; upvalue = upvalue->nextOpen) markObject(upvalue);
    markObject(context.fiber);
    markValue(context.pendingDestination);
}

// What the value in a frame's place holds of other objects (D149), where its type's layout says:
// marked through each Ref to it, as the places themselves are no collected object.
void VM::markPlace(const ObjPlace* place) {
    ObjClass* cls = place->cls;
    if (cls == nullptr) return;
    markObject(cls);
    const ValueLayout* layout = cls->layout.load(std::memory_order_acquire);
    if (layout == nullptr) return;
    const uint8_t* data = place->data();
    for (uint32_t offset : layout->objects) {
        Obj* held;
        std::memcpy(&held, data + offset, sizeof held);
        markObject(held);
    }
    for (uint32_t offset : layout->values) {
        Value held;
        std::memcpy(&held, data + offset, sizeof held);
        markValue(held);
    }
}

void VM::markObject(Obj* obj) {
    if (obj == nullptr || obj->marked) return;
    if (obj->outside) [[unlikely]] {
        noteOutside(obj);  // another heap's to free: only noted as reached (D97)
        return;
    }
    obj->marked = true;
    if (obj->type == ObjType::String) {
        // Strings have no references to trace. Interned ones are not counted as collectable heap.
        if (!obj->immortal) liveBytes_ += sizeof(ObjString) + static_cast<ObjString*>(obj)->heldBytes();
        return;
    }
    gray_.push_back(obj);
}

void VM::markTable(const Table& table) {
    const Table::Entry* entries = table.entries();
    for (uint32_t i = 0; i < table.capacity(); i++)
        if (entries[i].key != nullptr) markValue(entries[i].value);
    liveBytes_ += table.capacity() * sizeof(Table::Entry);
}

// Marks everything `obj` refers to, and adds its footprint to the live-heap estimate.
void VM::blacken(Obj* obj) {
    switch (obj->type) {
        case ObjType::String:
            break;
        case ObjType::List: {
            ObjList* list = static_cast<ObjList*>(obj);
            liveBytes_ += sizeof(ObjList) + list->values().capacity() * sizeof(Value);
            if (list->packed())
                liveBytes_ += sizeof(PackedItems) + list->pack->block.capacity() + list->pack->nulls.capacity() * sizeof(uint64_t);
            for (const Value& item : list->values()) markValue(item);
            markItems(list);
            break;
        }
        case ObjType::Places:
            // Kept in its line's arena, not the heap: markValue reaches its values through a
            // reference into it, and nothing else marks one, but what it holds is marked here too.
            markPlace(static_cast<const ObjPlace*>(obj));
            break;
        case ObjType::Lazy: {
            ObjLazy* lazy = static_cast<ObjLazy*>(obj);
            liveBytes_ += sizeof(ObjLazy);
            markObject(lazy->init);
            markValue(lazy->value);
            break;
        }
        case ObjType::Set: {
            ObjSet* set = static_cast<ObjSet*>(obj);
            liveBytes_ += sizeof(ObjSet) + set->items.entries().size() * 64;
            for (const ValueMap::Entry& entry : set->items.entries())
                if (entry.live) markValue(entry.key);
            markObject(set->viewOf);
            markObject(set->type);
            break;
        }
        case ObjType::Map: {
            ObjMap* map = static_cast<ObjMap*>(obj);
            liveBytes_ += sizeof(ObjMap) + map->map.entries().size() * 64;
            for (const ValueMap::Entry& entry : map->map.entries()) {
                if (!entry.live) continue;
                markValue(entry.key);
                markValue(entry.value);
            }
            if (map->keysView != nullptr) markObject(map->keysView);
            break;
        }
        case ObjType::Range:
            liveBytes_ += sizeof(ObjRange);
            break;
        case ObjType::Bytes:
            liveBytes_ += sizeof(ObjBytes) + static_cast<ObjBytes*>(obj)->data.capacity();
            break;
        case ObjType::Closure: {
            ObjClosure* closure = static_cast<ObjClosure*>(obj);
            liveBytes_ += sizeof(ObjClosure) + closure->upvalues.size() * sizeof(ObjUpvalue*);
            for (ObjUpvalue* upvalue : closure->upvalues) markObject(upvalue);
            if (closure->proto->chooser) {
                const ObjForms* set = static_cast<const ObjForms*>(closure);
                liveBytes_ += sizeof(ObjForms) - sizeof(ObjClosure) + set->forms.size() * sizeof(ObjClosure*);
                for (ObjClosure* form : set->forms) markObject(form);
            }
            break;
        }
        case ObjType::Upvalue: {
            ObjUpvalue* upvalue = static_cast<ObjUpvalue*>(obj);
            liveBytes_ += sizeof(ObjUpvalue);
            // While it is open its variable lives on a stack. If that is the stack of a task,
            // the task is kept, and marks the stack itself if it has been set aside.
            if (upvalue->owner != nullptr) markObject(upvalue->owner);
            else markValue(*upvalue->location);
            break;
        }
        case ObjType::Resource:
            liveBytes_ += sizeof(ObjResource);
            break;
        case ObjType::TypeInfo: {
            ObjTypeInfo* type = static_cast<ObjTypeInfo*>(obj);
            markObject(type->cls);
            for (ObjTypeInfo* arg : type->args) markObject(arg);
            break;
        }
        case ObjType::Fiber: {
            ObjFiber* fiber = static_cast<ObjFiber*>(obj);
            liveBytes_ += sizeof(ObjFiber) + static_cast<size_t>(fiber->context.stackEnd - fiber->context.stack) * sizeof(Value) +
                          static_cast<size_t>(fiber->context.frameLimit) * sizeof(CallFrame);
            markValue(fiber->entry);
            markValue(fiber->early);
            // One that is running is marked as a context of the thread it is running on;
            // what this object holds of its stack is out of date until it is set aside.
            if (fiber->state == FiberState::Suspended) markContext(fiber->context);
            break;
        }
        case ObjType::Native:
            liveBytes_ += sizeof(ObjNative);
            break;
        case ObjType::Class: {
            ObjClass* cls = static_cast<ObjClass*>(obj);
            liveBytes_ += sizeof(ObjClass);
            markObject(cls->name);
            if (cls->origin != nullptr) markObject(cls->origin);
            markObject(cls->super);
            markObject(cls->init);
            for (ObjClosure* maker : cls->supertypeMakers) markObject(maker);
            markObject(cls->fieldTypesMaker);  // G4
            markObject(cls->meta[0]);
            markObject(cls->meta[1]);
            for (const auto& described : cls->described)
                for (const auto& [type, made] : described) markValue(made);
            markTable(cls->methods);
            markTable(cls->statics);
            markTable(cls->getters);
            markTable(cls->setters);
            for (const FieldDecl& field : cls->declared) {
                markObject(field.typeThunk);
                markObject(field.typeClass);
            }
            for (ObjClosure* fieldInit : cls->fieldInits) markObject(fieldInit);
            markTable(cls->staticGetters);
            markTable(cls->staticSetters);
            markObject(cls->staticInit);
            markTable(cls->extensions);
            markObject(cls->nextEntry);
            for (ObjClass* implemented : cls->interfaces) markObject(implemented);
            for (ObjClass* fitted : cls->extendedTo) markObject(fitted);
            markTable(cls->extendMethods);
            markTable(cls->extendGetters);
            markTable(cls->extendFields);
            for (ObjString* part : cls->components) markObject(part);
            for (const auto& [name, interface] : cls->required) {
                markObject(name);
                markObject(interface);
            }
            for (const FieldDecl& field : cls->staticDeclared) {
                markObject(field.typeThunk);
                markObject(field.typeClass);
            }
            liveBytes_ += cls->staticDeclared.capacity() * sizeof(FieldDecl);
            liveBytes_ += cls->declared.capacity() * sizeof(FieldDecl) + cls->fieldInits.capacity() * sizeof(ObjClosure*);
            break;
        }
        case ObjType::Value: {
            // What a value holds of other objects, where its type's layout says.
            ObjValue* box = static_cast<ObjValue*>(obj);
            markObject(box->cls);
            const ValueLayout* layout = box->cls->layout.load(std::memory_order_acquire);
            liveBytes_ += sizeof(ObjValue) + (layout != nullptr ? layout->size : 0);
            if (layout == nullptr) break;
            for (uint32_t offset : layout->objects) {
                Obj* held;
                std::memcpy(&held, box->data() + offset, sizeof held);
                markObject(held);
            }
            for (uint32_t offset : layout->values) {
                Value held;
                std::memcpy(&held, box->data() + offset, sizeof held);
                markValue(held);
            }
            break;
        }
        case ObjType::Instance: {
            ObjInstance* instance = static_cast<ObjInstance*>(obj);
            liveBytes_ += instance->slotsStart + (instance->fieldCount + instance->captureCount) * sizeof(Value);  // its payloads too (M6.6 part 6)
            markObject(instance->cls);
            const Value* fields = instance->fields();
            for (uint32_t i = 0; i < instance->fieldCount + instance->captureCount; i++) markValue(fields[i]);
            if (instance->inlined) {
                // The values kept inside it (M6.6 part 5): what they hold, where their layouts say.
                liveBytes_ += instance->cls->inlineSlots * sizeof(Value);
                for (const ObjClass::InlineField& inside : instance->cls->inlineFields) {
                    const ValueLayout* layout = inside.cls->layout.load(std::memory_order_acquire);
                    const uint8_t* at = instance->inlineData() + inside.offset;
                    for (uint32_t offset : layout->objects) {
                        Obj* held;
                        std::memcpy(&held, at + offset, sizeof held);
                        if (held != nullptr) markObject(held);
                    }
                    for (uint32_t offset : layout->values) {
                        Value held;
                        std::memcpy(&held, at + offset, sizeof held);
                        markValue(held);
                    }
                }
            }
            break;
        }
        case ObjType::BoundMethod: {
            ObjBoundMethod* bound = static_cast<ObjBoundMethod*>(obj);
            liveBytes_ += sizeof(ObjBoundMethod);
            markValue(bound->receiver);
            markObject(bound->method);
            break;
        }
        case ObjType::BoundNative: {
            ObjBoundNative* bound = static_cast<ObjBoundNative*>(obj);
            liveBytes_ += sizeof(ObjBoundNative);
            markValue(bound->receiver);
            break;
        }
        case ObjType::Iterator: {
            ObjIterator* iterator = static_cast<ObjIterator*>(obj);
            liveBytes_ += sizeof(ObjIterator);
            markValue(iterator->source);
            break;
        }
    }
}

void VM::sweep() {
    Obj** link = &objects_;
    while (*link != nullptr) {
        Obj* obj = *link;
        if (obj->marked) {
            obj->marked = false;
            link = &obj->next;
        } else if (obj->immortal) {
            link = &obj->next;
        } else {
            *link = obj->next;
            if (!monitors_.empty()) monitors_.erase(obj);  // it was locked and then forgotten
            if (!sharedAs.empty()) [[unlikely]] {
                // Frozen data shared with workers: what it was shared as is let go of with it.
                if (auto shared = sharedAs.find(obj); shared != sharedAs.end()) {
                    reachedBlocks.erase(shared->second);
                    sharedAs.erase(shared);
                }
            }
            // An object of a class has nothing to let go of but its memory (objects plan part 4):
            // given back at once, with no call of a destructor through its table.
            if (obj->type == ObjType::Instance) ObjInstance::operator delete(obj);
            else delete obj;
        }
    }
}

// A value type over 64 KB, which the checker did not see (layoutOf). Kept here, after run(), and
// out of line: an error nobody meets, which should move nothing of the interpreter's.
#ifdef _MSC_VER
__declspec(noinline)
#else
__attribute__((noinline))
#endif
void VM::valueTooBig(const ObjClass* cls, uint32_t size) {
    throw runtimeError(Thrown::IllegalStateException, "value " + cls->name->utf8() + " is " + std::to_string(size) +
                                                          " bytes, and a value type is at most 64 KB (65,536 bytes), as it is copied whole: make it a class");
}

}  // namespace ww
