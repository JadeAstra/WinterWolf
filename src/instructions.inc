// The VM's instructions (M6.7): each the code that does it, written once and compiled twice.
// With Clang each is a function of its own that calls the next instruction's as its last step
// (VM::tail), the loop's state in registers; with MSVC and GCC they are the cases of run()'s
// switch. Their ways out, which mean the same in both:
//   NEXT()            straight on to the next instruction
//   DONE()            on to the next, the stack's top in sp_ (the switch's break)
//   EXIT_RUN()        out of run(), where the frame it started with returns
//   JUMP_LABEL(l)     on in another instruction's code, at its LABEL(l)
//   FALL_INTO(op)     on in the code of instruction op, which comes next
//   GO_COLD(op)       on in op's rare part, COLD(op) { ... } after it (declared by HAS_COLD(op)
//                     before the instruction): with tail calls a function of its own, so that the
//                     part that runs makes no call and keeps no stack frame
//   CASES(a, b, ...)  one code for several instructions, which tell which they are by opAt
// RUN_BASE is run()'s baseFrame.
// Included by vm.cpp only.

CASE(Const) {
    *sp++ = consts[READ_U16()];
    NEXT();
}
CASE(Null) {
    *sp++ = Value::null();
    NEXT();
}
CASE(True) {
    (sp++)->setBool(true);
    NEXT();
}
CASE(False) {
    (sp++)->setBool(false);
    NEXT();
}
CASE(Pop) {
    sp--;
    NEXT();
}
CASE(Dup) {
    sp[0] = sp[-1];
    sp++;
    NEXT();
}
CASE(Dup2) {
    sp_[0] = sp_[-2];
    sp_[1] = sp_[-1];
    sp_ += 2;
    DONE();
}

CASE(GetLocal) {
    *sp++ = base[READ_U16()];
    NEXT();
}
CASE(SetLocal) {
    base[READ_U16()] = sp[-1];
    NEXT();
}

CASE(GetGlobal) {
    {
    if (const Value global = globals_[static_cast<size_t>(FAST_U16(0))]; global.kind != ValueKind::Undefined) {
        *sp++ = global;
        ip += 2;
        NEXT();
    }
    int index = READ_U16();
    Value value = globals_[static_cast<size_t>(index)];
    if (value.kind == ValueKind::Undefined) {
        SAVE_IP();
        value = globalOrLazy(static_cast<size_t>(index));  // a lazy one's (D37)
    }
    *sp_++ = value;
    DONE();
}
}
CASE(SetGlobal) {
    {
    int index = READ_U16();
    if (coreId_ != 0) throw notOwned("the variable '" + program_->globalNames[static_cast<size_t>(index)] + "' is shared");
    Value& slot = globals_[static_cast<size_t>(index)];
    if (slot.kind == ValueKind::Undefined && !slot.isLazy())  // a lazy var may be given one first (D37)
        throw runtimeError(Thrown::IllegalStateException, "undefined variable '" + program_->globalNames[static_cast<size_t>(index)] + "'");
    const TypeConstraint& declared = globalTypes_[static_cast<size_t>(index)];
    if (declared.type != 0 && (!matchesType(sp_[-1], declared.type, declared.cls) && !widenedByte(sp_[-1], declared.type)))
        throw typeMismatch(program_->globalNames[static_cast<size_t>(index)], declared.type,
                           declared.cls, sp_[-1]);
    slot = own(sp_[-1]);
    DONE();
}
}
CASE(DefineGlobal) {
    {
    size_t index = static_cast<size_t>(READ_U16());
    if (coreId_ != 0) throw notOwned("'" + program_->globalNames[index] + "' is declared for the whole program");
    globals_[index] = own(*--sp_);
    globalTypes_[index] = TypeConstraint{};
    DONE();
}
}

CASE(GetImplicit) {
    {
    ObjString* name = asString(consts[READ_U16()]);
    size_t index = static_cast<size_t>(READ_U16());
    Value receiver = implicitReceiver(frame, base, name);
    if (receiver.kind == ValueKind::Undefined) {
        Value value = globals_[index];
        if (value.isLazy()) {
            SAVE_IP();
            value = globalOrLazy(index);
        }
        if (value.kind == ValueKind::Undefined) throw undefinedName(frame, base, name, index);
        *sp_++ = value;
        DONE();
    }
    if (!receiver.isObjType(ObjType::Instance)) {
        // In an extension function on a built-in type: a member of the value.
        SAVE_IP();
        *sp_++ = getProperty(receiver, name);
        DONE();
    }
    ObjInstance* instance = asInstance(receiver);
    if (Value field; instance->read(name, field)) {
        *sp_++ = fieldRead(field);
        DONE();
    }
    if (instance->cls->hasAccessors) {
        if (const Value* getter = instance->cls->getters.find(name)) {
            *sp_++ = receiver;
            SAVE_IP();
            callClosure(asClosure(*getter), 0);
            LOAD_FRAME();
            DONE();
        }
    }
    SAVE_IP();
    *sp_++ = getProperty(receiver, name);  // a method, bound to the object
    DONE();
}
}
CASE(TryImplicit) {
    {
    ObjString* name = asString(consts[READ_U16()]);
    int target = READ_I32();
    Value receiver = implicitReceiver(frame, base, name);
    if (receiver.kind == ValueKind::Undefined) DONE();
    ip = code + target;
    if (!receiver.isObjType(ObjType::Instance)) {
        SAVE_IP();
        *sp_++ = getProperty(receiver, name);
        DONE();
    }
    ObjInstance* instance = asInstance(receiver);
    if (Value field; instance->read(name, field)) {
        *sp_++ = fieldRead(field);
        DONE();
    }
    if (instance->cls->hasAccessors) {
        if (const Value* getter = instance->cls->getters.find(name)) {
            *sp_++ = receiver;
            SAVE_IP();
            callClosure(asClosure(*getter), 0);
            LOAD_FRAME();
            DONE();
        }
    }
    SAVE_IP();
    *sp_++ = getProperty(receiver, name);
    DONE();
}
}
CASE(InvokeImplicit) {
    {
    ObjString* name = asString(consts[READ_U16()]);
    size_t index = static_cast<size_t>(READ_U16());
    int argc = READ_U8();
    GC_SAFE_POINT();
    SAVE_IP();
    Value receiver = implicitReceiver(frame, base, name);
    if (receiver.kind == ValueKind::Undefined) {
        Value callee = globals_[index];
        if (callee.isLazy()) callee = globalOrLazy(index);
        if (callee.kind == ValueKind::Undefined) throw undefinedName(frame, base, name, index);
        sp_[-argc - 1] = callee;
        if (callValue(callee, argc)) LOAD_FRAME();
    } else {
        sp_[-argc - 1] = receiver;
        if (invoke(name, argc)) LOAD_FRAME();
    }
    DONE();
}
}
CASE(SetImplicit) {
    {
    ObjString* name = asString(consts[READ_U16()]);
    size_t index = static_cast<size_t>(READ_U16());
    Value receiver = implicitReceiver(frame, base, name);
    if (receiver.kind == ValueKind::Undefined) {
        if (coreId_ != 0) throw notOwned("the variable '" + program_->globalNames[index] + "' is shared");
        Value& slot = globals_[index];
        if (slot.kind == ValueKind::Undefined && !slot.isLazy()) throw undefinedName(frame, base, name, index);
        const TypeConstraint& declared = globalTypes_[index];
        if (declared.type != 0 && (!matchesType(sp_[-1], declared.type, declared.cls) && !widenedByte(sp_[-1], declared.type)))
            throw typeMismatch(program_->globalNames[index], declared.type, declared.cls, sp_[-1]);
        slot = own(sp_[-1]);  // as SetGlobal: an item where it is is copied as it is kept
        DONE();
    }
    CHECK_ACCESS(receiver, 2, true, true);  // found, but its setter may be kept back
    // Lay the object under the value, as an assignment to obj.name has them.
    Value value = sp_[-1];
    sp_[-1] = receiver;
    *sp_++ = value;
    SAVE_IP();
    if (!receiver.isObjType(ObjType::Instance)) {
        setProperty(receiver, name, value);  // reports that it cannot be set
        DONE();
    }
    ObjClass* cls = asInstance(receiver)->cls;
    if (cls->hasAccessors) {
        if (const Value* setter = cls->setters.find(name)) {
            callClosure(asClosure(*setter), 1);
            LOAD_FRAME();
            DONE();
        }
        if (cls->getters.find(name) != nullptr)
            throw runtimeError(Thrown::IllegalStateException, "property '" + name->utf8() + "' of " + cls->name->utf8() +
                               " cannot be assigned: it has no setter");
    }
    setProperty(receiver, name, value);
    sp_[-2] = value;
    sp_--;
    DONE();
}
}
CASE(DefineTypedGlobal) {
    {
    size_t index = static_cast<size_t>(READ_U16());
    uint8_t type = READ_U8();
    ObjClass* cls = popTypeClass(type);
    Value value = *--sp_;
    if ((!matchesType(value, type, cls) && !widenedByte(value, type)))
        throw typeMismatch(program_->globalNames[index], type, cls, value);
    if (coreId_ != 0) throw notOwned("'" + program_->globalNames[index] + "' is declared for the whole program");
    globals_[index] = own(value);
    globalTypes_[index] = TypeConstraint{type, cls};
    DONE();
}
}
CASES(IndexN, SetIndexN) {
    {
    // `grid[x, y]` and `grid[x, y] = v` (D133): only a class's op [] takes several.
    const bool set = opAt(ip - kOpBytes) == Op::SetIndexN;
    const int count = READ_U8();
    const Value obj = sp_[-count - 1 - (set ? 1 : 0)];
    if (classOf(obj) == nullptr)
        throw runtimeError(Thrown::UnsupportedOperationException,
                           "a value of type " + typeName(obj) + " takes one index; [x, y] is for a class with op [](x, y)");
    SAVE_IP();
    invokeOperator(obj, set ? names.opIndexSet : names.opIndex, count + (set ? 1 : 0));
    LOAD_FRAME();
    DONE();
}
}
CASE(BundleValues) {
    bundleValues(READ_U8());
    DONE();
}
CASE(MergeBundle) {
    mergeBundles();
    DONE();
}
CASE(DupN) {
    {
    const int count = READ_U8();
    for (int k = 0; k < count; k++) sp_[k] = sp_[k - count];
    sp_ += count;
    DONE();
}
}
CASE(SettleEnum) {
    {
    ObjClass* cls = popTypeClass(kTypeInstance);
    if (sp_[-1].kind == ValueKind::EnumName) sp_[-1] = settleEnumName(sp_[-1], cls);
    DONE();
}
}
CASE(CheckParam) {
    {
    ObjString* parameter = asString(consts[READ_U16()]);
    uint8_t type = READ_U8();
    ObjClass* cls = popTypeClass(type);
    if (sp_[-1].kind == ValueKind::EnumName) sp_[-1] = settleEnumName(sp_[-1], cls);
    if ((!matchesType(sp_[-1], type, cls) && !widenedByte(sp_[-1], type))) {
        checkRange(type, sp_[-1], "argument '" + parameter->utf8() + "' of " + frame->closure->proto->name + "()");
        throw runtimeError(Thrown::IllegalArgumentException, "argument '" + parameter->utf8() + "' of " + frame->closure->proto->name +
                           "() must be " + declaredTypeText(type, cls) + ", got " + typeName(sp_[-1]));
    }
    DONE();
}
}
CASE(CheckReturn) {
    {
    if (FITS_AT_ONCE(sp[-1], ip[0])) {
        ip++;
        NEXT();
    }
    uint8_t type = READ_U8();
    ObjClass* cls = popTypeClass(type);
    if ((!matchesType(sp_[-1], type, cls) && !widenedByte(sp_[-1], type)))
        throw runtimeError(Thrown::ClassCastException, frame->closure->proto->name + "() must return " + declaredTypeText(type, cls) +
                           ", but returned " + typeName(sp_[-1]));
    DONE();
}
}
CASE(CheckType) {
    {
    ObjString* variable = asString(consts[READ_U16()]);
    uint8_t type = READ_U8();
    ObjClass* cls = popTypeClass(type);
    if ((!matchesType(sp_[-1], type, cls) && !widenedByte(sp_[-1], type))) throw typeMismatch(variable->utf8(), type, cls, sp_[-1]);
    DONE();
}
}

CASE(GetUpvalue) {
    *sp_++ = *frame->closure->upvalues[static_cast<size_t>(READ_U16())]->location;
    DONE();
}
CASE(GetUpvalueChecked) {
    {
    int index = READ_U16();
    ObjString* name = asString(consts[READ_U16()]);
    Value value = *frame->closure->upvalues[static_cast<size_t>(index)]->location;
    if (value.kind == ValueKind::Undefined)
        throw runtimeError(Thrown::IllegalStateException, "'" + name->utf8() + "' is used before it has a value");
    *sp_++ = value;
    DONE();
}
}
CASES(MakeType, CachedType, CacheType, TypeCollection, IsFull, CastFull, WithTypes, FrameTypes, TypeArg, InstanceTypeArg, SupertypeMaker, CheckResult, ClassOf, ClassMeta, AtomicField, FieldTypesMaker) {
    ip = typeInstruction(opAt(ip - kOpBytes), ip, frame);
    DONE();
}

CASE(GetCaptured) {
    {
    const ObjUpvalue* cell = capturedOf(base[0], READ_U16());
    ObjString* name = asString(consts[READ_U16()]);
    const Value value = *cell->location;
    if (value.kind == ValueKind::Undefined)
        throw runtimeError(Thrown::IllegalStateException, "'" + name->utf8() + "' is used before it has a value");
    *sp_++ = value;
    DONE();
}
}
CASE(SetCaptured) {
    {
    ObjUpvalue* cell = capturedOf(base[0], READ_U16());
    // As SetUpvalue: a variable of the code around a section is nobody's to change there.
    if (cell->core != coreId_) throw notOwned("a variable from outside the section is shared");
    *cell->location = sp_[-1];
    DONE();
}
}

CASE(ObjectClass) {
    {
    const int global = READ_U16();
    const int target = READ_I32();
    if (global != 0xFFFF) {
        const Value kept = globals_[static_cast<size_t>(global)];
        if (kept.isObjType(ObjType::Class)) {
            *sp_++ = kept;
            ip = code + target;
        }
    }
    DONE();
}
}
CASE(StoreClass) {
    {
    const int global = READ_U16();
    // A core of a parallel section makes its own each time: the program's
    // globals are not its to change.
    if (global != 0xFFFF && coreId_ == 0) globals_[static_cast<size_t>(global)] = sp_[-1];
    DONE();
}
}
CASE(Capture) {
    {
    const int index = READ_U16();
    const int kind = READ_U8();
    ObjUpvalue* cell = nullptr;
    if (kind == 0) {
        cell = captureUpvalue(base + index);
    } else if (kind == 1) {
        if (static_cast<size_t>(index) >= frame->closure->upvalues.size()) throw runtimeError(Thrown::InternalError, "invalid bytecode: no such upvalue");
        cell = frame->closure->upvalues[static_cast<size_t>(index)];
    } else if (kind == 2) {
        cell = capturedOf(base[0], index);
    } else {
        // `super`: the class's superclass (null if it has only interfaces), in a cell of its own.
        const Value made = sp_[-1 - index];
        if (!made.isObjType(ObjType::Class)) throw runtimeError(Thrown::InternalError, "invalid anonymous object");
        ObjClass* superclass = asClass(made)->super;
        cell = alloc<ObjUpvalue>(nullptr);
        cell->closed = superclass != nullptr ? Value::object(superclass) : Value::null();
        cell->location = &cell->closed;
    }
    *sp_++ = Value::object(cell);
    DONE();
}
}
CASE(NewObject) {
    {
    const int count = READ_U8();
    const Value made = sp_[-count - 1];
    if (!made.isObjType(ObjType::Class)) throw runtimeError(Thrown::InternalError, "invalid anonymous object");
    ObjClass* cls = asClass(made);
    SAVE_IP();
    if (cls->staticInit != nullptr) ensureClassInit(cls);  // with the captures still on the stack
    if (constructObject(cls, count)) LOAD_FRAME();
    DONE();
}
}

CASE(Unset) {
    *sp_++ = Value::undefined();
    DONE();
}
CASE(SetUpvalue) {
    {
    ObjUpvalue* upvalue = frame->closure->upvalues[static_cast<size_t>(READ_U16())];
    // A variable of the code around the section is every core's to read, and nobody's to change.
    if (upvalue->core != coreId_) throw notOwned("a variable from outside the section is shared");
    // An item of a list where it is is copied as it is kept: in a variable of
    // the code around, it would outlive what the checker judged of the lambda.
    *upvalue->location = own(sp_[-1]);
    DONE();
}
}

HAS_COLD(GetOwn)
CASES(GetOwn, GetProp, GetOwnRef, GetPropRef) {
    // A field of an object, where this place found it before (M6.5 part 4), kept as its payload
    // alone or as a value (a value kept inside the object as itself only by GetPropRef and
    // GetOwnRef), here; anything else, the quick ways included, in the rare part, below.
    if (sp[-1].isObjType(ObjType::Instance) && caches != nullptr) {
        const ObjInstance* instance = asInstance(sp[-1]);
        const uint64_t seen = caches[FAST_U16(2)].load(std::memory_order_relaxed);
        if (static_cast<uint32_t>(seen >> 32) == instance->shape && (instance->core == 0 || instance->core == coreId_)) {
            const uint8_t* const at = reinterpret_cast<const uint8_t*>(instance) + ((seen >> 16) & 0xFFFF);
            if ((seen & 0x80) != 0) {
                uint64_t bits;
                std::memcpy(&bits, at, 8);
                sp[-1].setWhole(seen & 3, bits);
                ip += 4;
                NEXT();
            }
            if (const ValueKind kind = reinterpret_cast<const Value*>(at)->kind;
                kind != ValueKind::Undefined && (kind != ValueKind::Ref || opAt(ip - kOpBytes) >= Op::GetPropRef)) {
                std::memcpy(static_cast<void*>(&sp[-1]), at, sizeof(Value));
                ip += 4;
                NEXT();
            }
        }
    }
    GO_COLD(GetOwn);
}
COLD(GetOwn) {
    {
    // An object's narrow field found before (G20).
    if (caches != nullptr && narrowRead(sp[-1], caches[FAST_U16(2)].load(std::memory_order_relaxed), sp[-1])) {
        ip += 4;
        NEXT();
    }
    // Where this place has found the field before, in an object of the same
    // layout, it is there again (M6.5 part 4): nothing else about it has
    // changed, the right to use it included. A value kept inside the object
    // (M6.6 part 5) is read where it is only by GetPropRef and GetOwnRef.
    if (sp[-1].isObjType(ObjType::Instance) && caches != nullptr) {
        const ObjInstance* instance = asInstance(sp[-1]);
        const uint64_t seen = caches[FAST_U16(2)].load(std::memory_order_relaxed);
        if (static_cast<uint32_t>(seen >> 32) == instance->shape &&
            (instance->core == 0 || instance->core == coreId_)) {
            if ((seen & 0x80) != 0) {
                // A field kept as its payload alone (M6.6 part 6).
                {
                    // A value's 8 bytes of contents, and its kind as remembered.
                    sp[-1].kind = static_cast<ValueKind>(seen & 3);
                    std::memcpy(&sp[-1].as.i, reinterpret_cast<const uint8_t*>(instance) + ((seen >> 16) & 0xFFFF), 8);
                    ip += 4;
                    NEXT();
                }
            }
            const Value field = *reinterpret_cast<const Value*>(reinterpret_cast<const uint8_t*>(instance) + ((seen >> 16) & 0xFFFF));
            if (field.kind == ValueKind::Ref && opAt(ip - kOpBytes) < Op::GetPropRef) {
                // A value kept inside the object, read through at once with
                // the fields read from it next (`comp.bounds.width`); also
                // past a `?.`, the value being there (`comp.damage?.width`, G16).
                const uint8_t* next = opAt(ip + 4 + kThreaded) == Op::JumpIfNull ? ip + 4 + kThreaded + kOpBytes + 4 : ip + 4;
                if (const Op after = opAt(next + kThreaded); after == Op::GetProp || after == Op::GetOwn) {
                    ValueField whole;
                    whole.kind = ValueField::Nested;
                    whole.cls = refClass(field);
                    Value read;
                    ip = readThrough(refBytes(field), &whole, next, consts, read);
                    sp[-1] = read;
                    NEXT();
                }
            } else if (field.kind != ValueKind::Undefined) {
                sp[-1] = field;
                ip += 4;
                NEXT();
            }
        }
    } else if (sp[-1].isObjType(ObjType::Value) && caches != nullptr) {
        // A field of a value, where this place found it before (M6.6 part 2).
        const ObjValue* box = asValue(sp[-1]);
        const uint64_t seen = caches[FAST_U16(2)].load(std::memory_order_relaxed);
        if (static_cast<uint32_t>(seen >> 32) == box->cls->shape) {
            const ValueField& field = box->cls->layout.load(std::memory_order_relaxed)->fields[(seen >> 16) & 0xFFFF];
            if (field.kind == ValueField::Nested) {
                // A value held in this one: read through at once with the
                // fields read from it next (`sizes.pref.width`), or where it
                // is in the box, which never changes.
                if (const Op after = opAt(ip + 4 + kThreaded); after == Op::GetProp || after == Op::GetOwn) {
                    Value read;
                    ip = readThrough(box->data() + field.offset, &field, ip + 4, consts, read);
                    sp[-1] = read;
                    NEXT();
                }
                sp[-1] = Value::ref(sp[-1].as.obj, 0, static_cast<uint16_t>(field.offset), 1);
            } else {
                sp[-1] = readPlainField(box->data() + field.offset, field);
            }
            ip += 4;
            NEXT();
        }
    } else if (sp[-1].isSmall() && caches != nullptr) {
        // A field of a value kept in the value itself (M6.6 part 5).
        const ObjClass* cls = smallClassOf(sp[-1]);
        const uint64_t seen = caches[FAST_U16(2)].load(std::memory_order_relaxed);
        if (static_cast<uint32_t>(seen >> 32) == cls->shape) {
            const ValueField& field = cls->layout.load(std::memory_order_relaxed)->fields[(seen >> 16) & 0xFFFF];
            const Value read = readPlainField(smallBytes(sp[-1]) + field.offset, field);
            sp[-1] = read;
            ip += 4;
            NEXT();
        }
    } else if (sp[-1].isRef() && caches != nullptr) {
        // A field of a value where it is kept, an item of a list say (M6.6 part 4),
        // or a value held in one.
        const Value r = sp[-1];
        const ObjClass* cls = refClassOrNull(r);
        const uint64_t seen = caches[FAST_U16(2)].load(std::memory_order_relaxed);
        if (cls != nullptr && static_cast<uint32_t>(seen >> 32) == cls->shape) {
            if (const uint8_t* at = refQuickly(r, false)) {
                const ValueField& field = cls->layout.load(std::memory_order_relaxed)->fields[(seen >> 16) & 0xFFFF];
                if (field.kind == ValueField::Nested) {
                    if (const Op after = opAt(ip + 4 + kThreaded); after == Op::GetProp || after == Op::GetOwn) {
                        Value read;
                        ip = readThrough(at + field.offset, &field, ip + 4, consts, read);
                        sp[-1] = read;
                        NEXT();
                    }
                    sp[-1] = Value::ref(r.as.obj, r.refIndex, static_cast<uint16_t>(r.refOffset + field.offset), static_cast<uint8_t>(r.refDepth + 1));
                } else {
                    sp[-1] = readPlainField(at + field.offset, field);
                }
                ip += 4;
                NEXT();
            }
        }
    } else if (caches != nullptr && caches[FAST_U16(2)].load(std::memory_order_relaxed) == kSeenSize) {
        // The size of a string, list, map or bytes, where this place read one before.
        if (const int64_t size = quickSize(sp[-1]); size >= 0) {
            sp[-1].setInt(size);
            ip += 4;
            NEXT();
        }
    }
    const Op getOp = opAt(ip - kOpBytes);
    const bool own = getOp == Op::GetOwn || getOp == Op::GetOwnRef;
    ObjString* name = asString(consts[READ_U16()]);
    const int cache = READ_U16();
    Value obj = sp_[-1];
    CHECK_ACCESS(obj, 1, false, !own);
    if (obj.isObjType(ObjType::Instance)) {
        ObjInstance* instance = asInstance(obj);
        guardRead(instance);
        const int64_t at = instance->fieldIndex(name);
        if (at >= 0 && instance->fields()[at].kind != ValueKind::Undefined) {
            const Value slot = instance->fields()[at];
            if (slot.isRef() && getOp < Op::GetPropRef) {
                // A value kept inside the object, read on a copy; or read
                // through at once with the fields read from it next.
                if (const Op after = opAt(ip + kThreaded); after == Op::GetProp || after == Op::GetOwn ||
                                                             after == Op::GetPropRef || after == Op::GetOwnRef) {
                    ValueField whole;
                    whole.kind = ValueField::Nested;
                    whole.cls = refClass(slot);
                    Value read;
                    ip = readThrough(refBytes(slot), &whole, ip, consts, read);
                    sp_[-1] = read;
                    DONE();
                }
                sp_[-1] = ownRef(slot);
            } else {
                sp_[-1] = slot;
            }
            const uint64_t from = instance->slotsStart + static_cast<uint64_t>(at) * sizeof(Value);
            if (caches != nullptr && from <= 0xFFFF)
                caches[cache].store(static_cast<uint64_t>(instance->shape) << 32 | from << 16, std::memory_order_relaxed);
            DONE();
        }
        if (const FieldDecl* decl = instance->declOf(name); decl != nullptr && decl->payload != FieldDecl::NoPayload) {
            // A field kept as its payload alone (M6.6 part 6), remembered so;
            // a narrow one (D148) is read here each time.
            sp_[-1] = instance->payloadOf(*decl);
            if (caches != nullptr && decl->payload != FieldDecl::NarrowPayload)
                caches[cache].store(static_cast<uint64_t>(instance->shape) << 32 |
                                        static_cast<uint64_t>(sizeof(ObjInstance) + decl->payloadOffset) << 16 |
                                        static_cast<uint64_t>(decl->payloadBit) << 8 | 0x80u | decl->payloadValueKind(),
                                    std::memory_order_relaxed);
            else if (caches != nullptr && sizeof(ObjInstance) + decl->payloadOffset <= 0xFFFF)
                caches[cache].store(narrowEntry(instance->shape, *decl), std::memory_order_relaxed);  // G20
            DONE();
        }
        if (instance->cls->hasAccessors) {
            // A property: its getter runs with the object as the receiver.
            if (const Value* getter = instance->cls->getters.find(name)) {
                // One that only reads its stored field (`get() => field`, as a
                // property with a setter alone has): read as that field and
                // remembered so, as above, where it holds a value of its own.
                // An object's class, and so its getters, is told by its shape.
                if (const FieldDecl* decl = asClosure(*getter)->proto->readsField != nullptr ? instance->declOf(asClosure(*getter)->proto->readsField) : nullptr) {
                    if (decl->payload == FieldDecl::NoPayload) {
                        const Value slot = instance->fields()[decl->slot];
                        const uint64_t from = instance->slotsStart + static_cast<uint64_t>(decl->slot) * sizeof(Value);
                        if (slot.kind != ValueKind::Undefined && !slot.isLazy()) {
                            // A value kept inside the object: where it is, to GetPropRef and
                            // GetOwnRef, as a field is read; a copy to the others, as the
                            // getter would give.
                            sp_[-1] = slot.isRef() && getOp < Op::GetPropRef ? ownRef(slot) : slot;
                            if (caches != nullptr && from <= 0xFFFF)
                                caches[cache].store(static_cast<uint64_t>(instance->shape) << 32 | from << 16, std::memory_order_relaxed);
                            DONE();
                        }
                    } else if (decl->payload != FieldDecl::NarrowPayload && !(decl->tracked && !instance->payloadSet(*decl))) {
                        sp_[-1] = instance->payloadOf(*decl);
                        if (caches != nullptr)
                            caches[cache].store(static_cast<uint64_t>(instance->shape) << 32 |
                                                    static_cast<uint64_t>(sizeof(ObjInstance) + decl->payloadOffset) << 16 |
                                                    static_cast<uint64_t>(decl->payloadBit) << 8 | 0x80u | decl->payloadValueKind(),
                                                std::memory_order_relaxed);
                        DONE();
                    }
                }
                SAVE_IP();
                callClosure(asClosure(*getter), 0);
                LOAD_FRAME();
                DONE();
            }
        }
    } else if (obj.isObjType(ObjType::Value)) {
        ObjValue* box = asValue(obj);
        const int64_t at = valueFieldIndex(box, name);
        if (at >= 0) {
            const ValueField& field = layoutOf(box->cls).fields[static_cast<size_t>(at)];
            sp_[-1] = field.kind == ValueField::Nested ? Value::ref(obj.as.obj, 0, static_cast<uint16_t>(field.offset), 1)
                                                       : readValueField(box->data() + field.offset, field);
            if (caches != nullptr && at <= 0xFFFF)
                caches[cache].store(static_cast<uint64_t>(box->cls->shape) << 32 | static_cast<uint64_t>(at) << 16,
                                    std::memory_order_relaxed);
            DONE();
        }
        if (const Value* getter = box->cls->getters.find(name)) {
            SAVE_IP();
            callClosure(asClosure(*getter), 0);
            LOAD_FRAME();
            DONE();
        }
    } else if (obj.isSmall()) {
        const ObjClass* cls = smallClassOf(obj);
        if (const Value* at = cls->declaredIndex.find(name)) {
            const ValueField& field = cls->layout.load(std::memory_order_relaxed)->fields[static_cast<size_t>(at->as.i)];
            sp_[-1] = readValueField(smallBytes(obj) + field.offset, field);
            if (caches != nullptr && at->as.i <= 0xFFFF)
                caches[cache].store(static_cast<uint64_t>(cls->shape) << 32 | static_cast<uint64_t>(at->as.i) << 16,
                                    std::memory_order_relaxed);
            DONE();
        }
    } else if (obj.isRef()) {
        // A field of a value where it is kept (M6.6 part 4).
        Value read;
        if (refField(obj, name, read)) {
            sp_[-1] = read;
            if (caches != nullptr) {
                const ObjClass* cls = refClassOrNull(obj);
                const Value* found = cls != nullptr ? cls->declaredIndex.find(name) : nullptr;
                const int64_t at = found != nullptr ? found->as.i : -1;
                if (at >= 0 && at <= 0xFFFF)
                    caches[cache].store(static_cast<uint64_t>(cls->shape) << 32 | static_cast<uint64_t>(at) << 16, std::memory_order_relaxed);
            }
            DONE();
        }
        if (const Value* getter = refClass(obj)->getters.find(name)) {
            SAVE_IP();
            callClosure(asClosure(*getter), 0);
            LOAD_FRAME();
            DONE();
        }
    }
    // (Whether some class keeps a `size` of its own private does not matter: these have none.)
    if (caches != nullptr && name == names.size && quickSize(obj) >= 0)
        caches[cache].store(kSeenSize, std::memory_order_relaxed);
    sp_[-1] = getProperty(obj, name);
    DONE();
}
}
HAS_COLD(SetOwn)
CASES(SetOwn, SetProp) {
    // A field of an object found before, kept as its payload, or a value of no declared type, an
    // int or a float, here; anything else, the quick ways included, in the rare part, below.
    if (sp[-2].isObjType(ObjType::Instance) && caches != nullptr) {
        ObjInstance* instance = asInstance(sp[-2]);
        const uint64_t seen = caches[FAST_U16(2)].load(std::memory_order_relaxed);
        if (static_cast<uint32_t>(seen >> 32) == instance->shape && instance->core == coreId_) {
            Value* const field = reinterpret_cast<Value*>(reinterpret_cast<uint8_t*>(instance) + ((seen >> 16) & 0xFFFF));
            const Value& value = sp[-1];
            if ((seen & 0x80) != 0) {
                const uint32_t bit = (seen >> 8) & 0xFF;
                const ValueKind kind = static_cast<ValueKind>(seen & 3);
                uint8_t* const data = instance->payloadData();
                const bool given = (seen & 0x20) != 0 && (data[bit / 8] >> (bit % 8) & 1) != 0;
                if (value.kind == kind && ((seen & 0x40) == 0 || !given)) {
                    const int64_t bits = kind == ValueKind::Bool ? (value.as.b ? 1 : 0) : value.as.i;
                    std::memcpy(static_cast<void*>(field), &bits, 8);  // its payload alone
                    if ((seen & 0x20) != 0) data[bit / 8] |= static_cast<uint8_t>(1u << (bit % 8));
                    std::memcpy(static_cast<void*>(&sp[-2]), &value, sizeof(Value));
                    sp--;
                    ip += 4;
                    NEXT();
                }
            } else if ((seen & 2) == 0 && ((seen & 1) == 0 || field->kind == ValueKind::Undefined)) {
                const uint8_t type = static_cast<uint8_t>(seen >> 8);
                if (type == 0 || (type == kTypeFloat && value.isFloat()) || (type == kTypeInt && value.isInt())) {
                    std::memcpy(static_cast<void*>(field), &value, sizeof(Value));
                    std::memcpy(static_cast<void*>(&sp[-2]), &value, sizeof(Value));
                    sp--;
                    ip += 4;
                    NEXT();
                }
            }
        }
    }
    GO_COLD(SetOwn);
}
COLD(SetOwn) {
    {
    // An object's narrow field found before, given a value that fits it (G20).
    if (caches != nullptr && narrowWrite(sp[-2], caches[FAST_U16(2)].load(std::memory_order_relaxed), sp[-1])) {
        sp[-2] = sp[-1];
        sp--;
        ip += 4;
        NEXT();
    }
    // As for GetProp: a field found before, which takes any value or one of
    // a type that is quickly told (never one of a class type). A val (the
    // low bit) is given its value here only while it has none, as its
    // object's init does.
    if (sp[-2].isObjType(ObjType::Instance) && caches != nullptr) {
        ObjInstance* instance = asInstance(sp[-2]);
        const uint64_t seen = caches[FAST_U16(2)].load(std::memory_order_relaxed);
        Value* const field = reinterpret_cast<Value*>(reinterpret_cast<uint8_t*>(instance) + ((seen >> 16) & 0xFFFF));
        if ((seen & 0x80) != 0) {
            // A field kept as its payload alone (M6.6 part 6): written there,
            // where the value is of its kind (and a val has none yet).
            if (static_cast<uint32_t>(seen >> 32) == instance->shape && instance->core == coreId_) {
                const Value value = sp[-1];
                const uint32_t bit = (seen >> 8) & 0xFF;
                const ValueKind kind = static_cast<ValueKind>(seen & 3);
                uint8_t* const data = instance->payloadData();
                const bool given = (seen & 0x20) != 0 && (data[bit / 8] >> (bit % 8) & 1) != 0;
                if (value.kind == kind && ((seen & 0x40) == 0 || !given)) {
                    const int64_t bits = kind == ValueKind::Bool ? (value.as.b ? 1 : 0) : value.as.i;
                    std::memcpy(reinterpret_cast<uint8_t*>(instance) + ((seen >> 16) & 0xFFFF), &bits, 8);
                    if ((seen & 0x20) != 0) data[bit / 8] |= static_cast<uint8_t>(1u << (bit % 8));
                    sp[-2] = value;
                    sp--;
                    ip += 4;
                    NEXT();
                }
            }
        } else if ((seen & 2) != 0) {
            // A value kept inside the object, given another of its type (M6.6
            // part 5): its bytes copied in, from a box or from where it is.
            if (static_cast<uint32_t>(seen >> 32) == instance->shape && instance->core == coreId_ && field->isRef() && field->as.obj == instance) {
                const Value value = sp[-1];
                const ObjClass::InlineField* inside = instance->cls->inlineFieldAt(field->refOffset);
                const uint8_t* from = value.isObjType(ObjType::Value) && asValue(value)->cls == inside->cls ? asValue(value)->data()
                                      : value.isRef() && refClass(value) == inside->cls          ? refQuickly(value, false)
                                                                                                 : nullptr;
                if (from != nullptr) {
                    std::memmove(instance->inlineData() + inside->offset, from, inside->size);
                    sp[-2] = value;
                    sp--;
                    ip += 4;
                    NEXT();
                }
            }
        } else if (static_cast<uint32_t>(seen >> 32) == instance->shape && instance->core == coreId_ &&
            ((seen & 1) == 0 || field->kind == ValueKind::Undefined)) {
            Value value = sp[-1];
            const uint8_t type = static_cast<uint8_t>(seen >> 8);
            if (type == 0 || (type == kTypeFloat ? value.isFloat()
                              : type == kTypeInt ? value.isInt()
                                                 : (matchesType(value, type, nullptr) || widenedByte(value, type)))) {
                *field = value;
                sp[-2] = value;
                sp--;
                ip += 4;
                NEXT();
            }
        }
    } else if (sp[-2].isObjType(ObjType::Value) && caches != nullptr) {
        // A field of a value being made, in its box (its init's `this`): found
        // as one of a value where it is kept, below.
        ObjValue* const box = asValue(sp[-2]);
        const uint64_t seen = caches[FAST_U16(2)].load(std::memory_order_relaxed);
        if (box->valueOpen && static_cast<uint32_t>(seen >> 32) == box->cls->shape) {
            const ValueField& field = box->cls->layout.load(std::memory_order_relaxed)->fields[(seen >> 16) & 0xFFFF];
            const Value value = sp[-1];
            if (field.kind == ValueField::Float ? value.isFloat() : field.kind == ValueField::Int ? value.isInt() : value.isBool()) {
                if (field.kind == ValueField::Bool) box->data()[field.offset] = value.as.b ? 1 : 0;
                else std::memcpy(box->data() + field.offset, &value.as, 8);
                sp[-2] = value;
                sp--;
                ip += 4;
                NEXT();
            }
        }
    } else if (sp[-2].isRef() && sp[-2].refDepth == 0 && caches != nullptr) {
        // A var field of a value where it is kept (M6.6 part 4): only an int, a
        // float or a bool field is remembered, which takes its own kind only.
        const Value r = sp[-2];
        const ObjClass* cls = refHolderClass(r);
        const uint64_t seen = caches[FAST_U16(2)].load(std::memory_order_relaxed);
        if (cls != nullptr && static_cast<uint32_t>(seen >> 32) == cls->shape) {
            const ValueField& field = cls->layout.load(std::memory_order_relaxed)->fields[(seen >> 16) & 0xFFFF];
            const Value value = sp[-1];
            uint8_t* at = (field.kind == ValueField::Float ? value.isFloat()
                           : field.kind == ValueField::Int ? value.isInt()
                                                           : value.isBool())
                              ? refQuickly(r, true)
                              : nullptr;
            if (at != nullptr) {
                // The kind matched: written as it is, with no call.
                if (field.kind == ValueField::Bool) at[field.offset] = value.as.b ? 1 : 0;
                else std::memcpy(at + field.offset, &value.as, 8);
                sp[-2] = value;
                sp--;
                ip += 4;
                NEXT();
            }
        }
    }
    const bool own = opAt(ip - kOpBytes) == Op::SetOwn;
    ObjString* name = asString(consts[READ_U16()]);
    const int cache = READ_U16();
    Value value = sp_[-1];
    SAVE_IP();
    CHECK_ACCESS(sp_[-2], 3, true, !own);
    if (sp_[-2].isObjType(ObjType::Instance) && asInstance(sp_[-2])->cls->hasAccessors) {
        // A property: the object and the value are already laid out as a call
        // to the setter, which returns the value it was given.
        ObjClass* cls = asInstance(sp_[-2])->cls;
        if (const Value* setter = cls->setters.find(name)) {
            callClosure(asClosure(*setter), 1);
            LOAD_FRAME();
            DONE();
        }
        if (cls->getters.find(name) != nullptr)
            throw runtimeError(Thrown::IllegalStateException, "property '" + name->utf8() + "' of " + cls->name->utf8() +
                               " cannot be assigned: it has no setter");
    }
    setProperty(sp_[-2], name, value);
    if (caches != nullptr && ((sp_[-2].isRef() && sp_[-2].refDepth == 0) || sp_[-2].isObjType(ObjType::Value))) {
        const ObjClass* cls = sp_[-2].isRef() ? refHolderClass(sp_[-2]) : asValue(sp_[-2])->cls;
        const Value* found = cls != nullptr ? cls->declaredIndex.find(name) : nullptr;
        const int64_t at = found != nullptr ? found->as.i : -1;
        const ValueField::Kind kind = at >= 0 ? cls->layout.load(std::memory_order_relaxed)->fields[static_cast<size_t>(at)].kind : ValueField::Any;
        if (at >= 0 && at <= 0xFFFF && (kind == ValueField::Int || kind == ValueField::Float || kind == ValueField::Bool))
            caches[cache].store(static_cast<uint64_t>(cls->shape) << 32 | static_cast<uint64_t>(at) << 16, std::memory_order_relaxed);
    } else if (caches != nullptr && sp_[-2].isObjType(ObjType::Instance)) {
        // Remembered for next time, where the next time can be done quickly.
        const ObjInstance* instance = asInstance(sp_[-2]);
        const FieldDecl* decl = instance->declOf(name);
        const int64_t at = decl != nullptr && decl->payload == FieldDecl::NoPayload ? decl->slot : -1;
        if (decl != nullptr && decl->payload == FieldDecl::NarrowPayload && !decl->isVal &&
            sizeof(ObjInstance) + decl->payloadOffset <= 0xFFFF) {
            caches[cache].store(narrowEntry(instance->shape, *decl), std::memory_order_relaxed);  // G20
        } else if (decl != nullptr && decl->payload != FieldDecl::NoPayload && decl->payload != FieldDecl::NarrowPayload &&
                   !(decl->isVal && decl->overrides)) {
            // A payload (M6.6 part 6): its offset, its bit and its kind.
            caches[cache].store(static_cast<uint64_t>(instance->shape) << 32 |
                                    static_cast<uint64_t>(sizeof(ObjInstance) + decl->payloadOffset) << 16 |
                                    static_cast<uint64_t>(decl->payloadBit) << 8 | 0x80u | (decl->isVal ? 0x40u : 0u) |
                                    (decl->tracked ? 0x20u : 0u) | decl->payloadValueKind(),
                                std::memory_order_relaxed);
        } else if (at >= 0 && instance->slotsStart + static_cast<uint64_t>(at) * sizeof(Value) <= 0xFFFF) {
            const FieldDecl& field = *decl;
            const uint64_t from = instance->slotsStart + static_cast<uint64_t>(at) * sizeof(Value);
            if (!(field.isVal && field.overrides) && (field.type & kTypeMask) != kTypeInstance)
                caches[cache].store(static_cast<uint64_t>(instance->shape) << 32 | from << 16 |
                                        static_cast<uint64_t>(field.type) << 8 | (field.isVal ? 1u : 0u),
                                    std::memory_order_relaxed);
            else if (field.inlineOffset >= 0 && instance->inlined && !field.isVal &&
                     instance->cls->inlineFieldAt(static_cast<uint32_t>(field.inlineOffset))->typeSlot == 0)  // one keeping its type goes this way (G4)
                caches[cache].store(static_cast<uint64_t>(instance->shape) << 32 | from << 16 | 2u,
                                    std::memory_order_relaxed);  // kept inside the object (the second bit)
        }
    }
    sp_[-2] = value;
    sp_--;
    DONE();
}
}

CASE(GetIndexRef) {
    {
    if (sp_[-2].isObjType(ObjType::List) && sp_[-1].isInt()) {
        ObjList* list = asList(sp_[-2]);
        size_t i;
        if (list->store() == ListStore::Items && resolveIndex(sp_[-1].as.i, list->size(), i) &&
            !(list->pack->nullable && list->pack->nulls[i / 64] >> (i % 64) & 1)) {
            guardRead(list);
            sp_[-2] = Value::ref(list, static_cast<uint32_t>(i), 0, 0);  // the item, where it is
            sp_--;
            DONE();
        }
    }
    JUMP_LABEL(getIndex);
}
}
CASE(OwnLocal) {
    {
    // A Ref to a frame's place is the variable's own value already (D149), and stays. A value
    // kept in the value itself (a Color) is a copy there, with no box (B2: `grid[x, y]` put in place).
    Value& slot = base[READ_U16()];
    if (slot.isRef() && slot.as.obj->type != ObjType::Places) {
        ObjClass* cls = refClass(slot);
        slot = cls->smallId != 0 ? smallFrom(cls, refAt(slot, false), layoutOf(cls).size, refType(slot)) : ownRef(slot);
    }
    DONE();
}
}
CASE(KeepLocal) {
    {
    // A variable with no place of its own keeps a copy of a value in one of the frame's (D149).
    Value& slot = base[READ_U16()];
    if (slot.isRef() && slot.as.obj->type == ObjType::Places) slot = ownRef(slot);
    DONE();
}
}
CASES(ObjectClassTyped, StoreClassTyped) {  // G66
    ip = typeInstruction(opAt(ip - kOpBytes), ip, frame);
    DONE();
}
CASE(TypeSlots) {
    ip = typeInstruction(opAt(ip - kOpBytes), ip, frame);
    DONE();
}
CASE(JumpIfNotClass) {
    {
    const size_t global = static_cast<size_t>(READ_U16());
    const int target = READ_I32();
    const Value v = *--sp_;
    const Value& cls = globals_[global];
    if (!(v.isObjType(ObjType::Instance) && cls.isObjType(ObjType::Class) && asInstance(v)->cls == asClass(cls))) ip = code + target;
    DONE();
}
}
CASE(KeepTop) {
    if (sp_[-1].isRef() && sp_[-1].as.obj->type == ObjType::Places) sp_[-1] = ownRef(sp_[-1]);
    DONE();
}
CASE(EnterPlaces) {
    {
    const int count = READ_U8();
    if (count != 0) enterPlaces(frame, count);
    DONE();
}
}
CASE(SetPlace) {
    {
    const int slot = READ_U16();
    const int place = READ_U8();
    // Made in its place already (WithPlace), as a call given one usually does: only held.
    const Value value = sp_[-1];
    if (value.kind == ValueKind::Ref && value.as.obj == placeOf(frame, place) && value.refDepth == 0 && value.refOffset == 0)
        base[slot] = value;
    else
        setPlace(frame, base[slot], value, place);
    DONE();
}
}
CASE(WithPlace) {
    pendingDestination_ = withPlace(frame, READ_U8());
    DONE();
}
CASE(GetIndex) {
LABEL(getIndex) {
    Value index = sp_[-1], obj = sp_[-2];
    if (classOf(obj) != nullptr) {
        SAVE_IP();
        invokeOperator(obj, names.opIndex, 1);
        LOAD_FRAME();
        DONE();
    }
    if (obj.isObjType(ObjType::List) && index.isInt()) {
        // A list's item by its position, in range, read at once where nothing
        // stands in the way: as getIndex reads it (G25).
        const ObjList* list = asList(obj);
        const uint64_t i = static_cast<uint64_t>(index.as.i);
        if (i < list->size() && obj.as.obj != bandObject_ && (obj.as.obj->core == coreId_ || obj.as.obj->core == 0)) {
            if (!list->packed()) {
                sp_[-2] = list->values()[i];
                sp_--;
                DONE();
            }
            if (const PackedItems& pack = *list->pack; !pack.nullable && (pack.store == ListStore::Ints || pack.store == ListStore::Floats)) {
                // 8 bytes an item: an int's or a float's bits, its kind the list's.
                uint64_t bits;
                std::memcpy(&bits, pack.block.data() + i * pack.width, 8);
                sp_[-2].setWhole(static_cast<uint64_t>(pack.store == ListStore::Ints ? ValueKind::Int : ValueKind::Float), bits);
                sp_--;
                DONE();
            }
            if (list->pack->store != ListStore::Items) {
                sp_[-2] = packedPlain(*list->pack, i);
                sp_--;
                DONE();
            }
        }
    }
    if (obj.isObjType(ObjType::Bytes) && index.isInt()) {
        // A buffer's byte by its position, in range, read at once where nothing stands in the
        // way, as getIndex reads it.
        const std::vector<uint8_t>& data = asBytes(obj)->data;
        const uint64_t i = static_cast<uint64_t>(index.as.i);
        if (i < data.size() && obj.as.obj != bandObject_ && (obj.as.obj->core == coreId_ || obj.as.obj->core == 0)) {
            sp_[-2] = Value::byte(data[i]);
            sp_--;
            DONE();
        }
    }
    if (obj.isObjType(ObjType::List) && asList(obj)->store() == ListStore::Items && index.isInt()) {
        // An item of a list of values (M6.6 part 3): read with one of its fields
        // at once, `pts[i].x`, the field is read where it is; otherwise a copy.
        ObjList* list = asList(obj);
        size_t i;
        if (resolveIndex(index.as.i, list->size(), i) && !(list->pack->nullable && list->pack->nulls[i / 64] >> (i % 64) & 1)) {
            guardRead(obj.as.obj);
            if (obj.as.obj == bandObject_ && (i < bandFrom_ || i >= bandTo_)) [[unlikely]] throw outsideBand(i);
            ValueField whole;
            whole.kind = ValueField::Nested;
            whole.cls = list->pack->itemClass;
            whole.typeSlot = list->pack->typeSlot;  // the type it keeps beside it, or the list's (G4)
            Value read;
            ip = readThrough(itemAt(list, i), &whole, ip, consts, read, list->pack->itemType);
            sp_[-2] = read;
            sp_--;
            DONE();
        }
    }
    sp_[-2] = getIndex(obj, index);
    sp_--;
    DONE();
}
}
CASE(SetIndex) {
    {
    Value value = sp_[-1], index = sp_[-2], obj = sp_[-3];
    if (obj.isObjType(ObjType::Bytes) && index.isInt() && obj.as.obj->core == coreId_ && (value.isByte() || value.isInt()) &&
        static_cast<uint64_t>(value.as.i) <= 255 && static_cast<uint64_t>(index.as.i) < asBytes(obj)->data.size()) {
        // A byte, or an int from 0 to 255, into a buffer this core owns: as setIndex writes it.
        asBytes(obj)->data[static_cast<size_t>(index.as.i)] = static_cast<uint8_t>(value.as.i);
        sp_[-3] = value;
        sp_ -= 2;
        DONE();
    }
    if (obj.isObjType(ObjType::List) && index.isInt() && obj.as.obj->core == coreId_ && obj.as.obj->frozen == 0 &&
        static_cast<uint64_t>(index.as.i) < asList(obj)->size()) {
        // An int, a float or a bool put into a packed list of its kind (G25): written
        // into the block, its null bit cleared; anything else as setIndex does it.
        ObjList* list = asList(obj);
        const size_t i = static_cast<size_t>(index.as.i);
        PackedItems* pack = list->pack.get();
        if (pack != nullptr && ((pack->store == ListStore::Ints && value.isInt()) || (pack->store == ListStore::Floats && value.isFloat()) ||
                                (pack->store == ListStore::Bools && value.isBool()))) {
            uint8_t* at = pack->block.data() + i * pack->width;
            if (pack->store == ListStore::Bools) *at = value.as.b ? 1 : 0;
            else std::memcpy(at, &value.as.i, 8);
            if (pack->nullable) pack->nulls[i / 64] &= ~(uint64_t{1} << (i % 64));
            sp_[-3] = value;
            sp_ -= 2;
            DONE();
        }
        if (pack != nullptr && pack->store == ListStore::Items && value.isSmall() && pack->itemType == nullptr && pack->typeSlot == 0 &&
            smallClassOf(value) == pack->itemClass) {
            // A value kept in the value itself (a Color) put into a list of its type: its bytes copied.
            std::memcpy(pack->block.data() + i * pack->width, smallBytes(value), pack->itemClass->layout.load(std::memory_order_relaxed)->size);
            if (pack->nullable) pack->nulls[i / 64] &= ~(uint64_t{1} << (i % 64));
            sp_[-3] = value;
            sp_ -= 2;
            DONE();
        }
    }
    if (classOf(obj) != nullptr) {
        SAVE_IP();
        invokeOperator(obj, names.opIndexSet, 2);
        LOAD_FRAME();
        DONE();
    }
    setIndex(obj, index, value);
    sp_[-3] = value;
    sp_ -= 2;
    DONE();
}
}

CASE(SetSuper) {
    {
    ObjString* name = asString(consts[READ_U16()]);
    const Value super = sp_[-2];
    CHECK_ACCESS(sp_[-3], 1, false, true);
    if (!super.isObjType(ObjType::Class))
        throw runtimeError(Thrown::UnsupportedOperationException, "there is no '" + name->utf8() + "' for super to mean: this class has no superclass");
    if (const Value* setter = asClass(super)->setters.find(name)) {
        // The setter, run on this object with the value.
        sp_[-2] = sp_[-1];
        sp_--;
        SAVE_IP();
        callClosure(asClosure(*setter), 1);
        LOAD_FRAME();
        DONE();
    }
    // A field the superclass declares, which the object has: written as it is.
    if (asClass(super)->declaredIndex.find(name) == nullptr)
        throw runtimeError(Thrown::UnsupportedOperationException, "undefined property '" + name->utf8() + "' in superclass " + asClass(super)->name->utf8());
    SAVE_IP();
    const Value value = sp_[-1];
    setProperty(sp_[-3], name, value);
    sp_ -= 2;
    sp_[-1] = value;
    DONE();
}
}
CASE(GetSuper) {
    {
    ObjString* name = asString(consts[READ_U16()]);
    Value super = *--sp_;
    CHECK_ACCESS(sp_[-1], 1, false, true);
    const Value* method = superMethod(super, sp_[-1], name);
    if (method == nullptr && super.isObjType(ObjType::Class)) {
        SAVE_IP();
        if (ObjString* other = missedName(asClass(super), name, sp_[-1], false)) method = superMethod(super, sp_[-1], other);  // o-private-shadow
    }
    if (method == nullptr && super.isNull())
        throw runtimeError(Thrown::UnsupportedOperationException, "there is no '" + name->utf8() + "' for super to mean: this class has no superclass, " +
                           std::string("and its interfaces come with none"));
    if (method == nullptr && !super.isObjType(ObjType::Class)) throw runtimeError(Thrown::ClassCastException, "superclass must be a class");
    if (method == nullptr) {
        // super.name: the superclass's property, run on this object, or a
        // field it declares, which the object simply has.
        if (const Value* getter = asClass(super)->getters.find(name)) {
            SAVE_IP();
            callClosure(asClosure(*getter), 0);
            LOAD_FRAME();
            DONE();
        }
        guardRead(sp_[-1]);
        Value field;
        const bool found = sp_[-1].isObjType(ObjType::Instance) && asInstance(sp_[-1])->read(name, field);
        if (found && asClass(super)->declaredIndex.find(name) != nullptr) {
            sp_[-1] = fieldRead(field);
            DONE();
        }
        throw runtimeError(Thrown::UnsupportedOperationException, "undefined method or property '" + name->utf8() + "' in superclass " +
                           asClass(super)->name->utf8());
    }
    sp_[-1] = Value::object(alloc<ObjBoundMethod>(sp_[-1], asClosure(*method)));
    DONE();
}
}

CASES(Add, AddI, AddF) {
    FAST_ARITHMETIC(sp[-2], sp[-1], +, addOverflows, sp--; NEXT())
LABEL(plainAdd)
    ARITHMETIC(Op::Add, addOverflows, x + y)
}
CASES(Sub, SubI, SubF) {
    FAST_ARITHMETIC(sp[-2], sp[-1], -, subOverflows, sp--; NEXT())
LABEL(plainSub)
    ARITHMETIC(Op::Sub, subOverflows, x - y)
}
CASES(Mul, MulI, MulF) {
    FAST_ARITHMETIC(sp[-2], sp[-1], *, mulOverflows, sp--; NEXT())
LABEL(plainMul)
    ARITHMETIC(Op::Mul, mulOverflows, x * y)
}
CASE(StoreLocal) {
    base[READ_U16()] = *--sp;
    NEXT();
}
CASE(CopyLocal) {
    base[FAST_U16(2)] = base[FAST_U16(0)];
    ip += 4;
    NEXT();
}

HAS_COLD(CheckLocal)
CASE(CheckLocal) {
    // A parameter of a type quickly told, here; any other in the rare part, below.
    {
        const Value& v = base[FAST_U16(0)];
        const uint8_t type = ip[4];
        if (type == kTypeInt ? v.isInt() : type == kTypeFloat ? v.isFloat() : type == kTypeBool ? v.isBool() : type == kTypeString && v.isString()) {
            ip += 5;
            NEXT();
        }
    }
    GO_COLD(CheckLocal);
}
COLD(CheckLocal) {
    {
    if ((matchesType(base[FAST_U16(0)], ip[4], nullptr) || widenedByte(base[FAST_U16(0)], ip[4]))) {
        ip += 5;
        NEXT();
    }
    const int slot = READ_U16();
    ObjString* parameter = asString(consts[READ_U16()]);
    const uint8_t type = READ_U8();
    if (!matchesType(base[slot], type, nullptr)) {
        checkRange(type, base[slot], "argument '" + parameter->utf8() + "' of " + frame->closure->proto->name + "()");
        throw runtimeError(Thrown::IllegalArgumentException, "argument '" + parameter->utf8() + "' of " + frame->closure->proto->name +
                           "() must be " + declaredTypeText(type, nullptr) + ", got " + typeName(base[slot]));
    }
    DONE();
}
}
CASES(Div, DivF) {
LABEL(plainDiv) {
    Value b = sp_[-1], a = sp_[-2];
    if (a.isWhole() && b.isWhole()) {
        if (b.as.i == 0) throw runtimeError(Thrown::ArithmeticException, "division by zero");
        if (b.as.i == -1 && a.as.i == INT64_MIN) throw overflowError(Op::Div, a.as.i, b.as.i);  // (D27)
        int64_t result = b.as.i == -1 ? -a.as.i : a.as.i / b.as.i;
        sp_[-2] = Value::integer(result);
        sp_--;
    } else if (a.isNumber() && b.isNumber()) {
        sp_[-2] = Value::number(a.toDouble() / b.toDouble());
        sp_--;
    } else {
        SAVE_IP();
        if (binarySlow(Op::Div)) LOAD_FRAME();
    }
    DONE();
}
}
CASE(Mod) {
LABEL(plainMod) {
    Value b = sp_[-1], a = sp_[-2];
    if (a.isWhole() && b.isWhole()) {
        if (b.as.i == 0) throw runtimeError(Thrown::ArithmeticException, "division by zero");
        sp_[-2] = Value::integer(b.as.i == -1 ? 0 : a.as.i % b.as.i);
        sp_--;
    } else if (a.isNumber() && b.isNumber()) {
        sp_[-2] = Value::number(std::fmod(a.toDouble(), b.toDouble()));
        sp_--;
    } else {
        SAVE_IP();
        if (binarySlow(Op::Mod)) LOAD_FRAME();
    }
    DONE();
}
}

CASES(AddWrap, SubWrap, MulWrap) {
    {
    // `&+`, `&-` and `&*`: an int sum, difference or product that wraps around (D27).
    const Op op = opAt(ip - kOpBytes);
    const Value b = sp_[-1], a = sp_[-2];
    if (!a.isInt() || !b.isInt())
        throw runtimeError(Thrown::ArithmeticException, std::string(op == Op::AddWrap ? "&+" : op == Op::SubWrap ? "&-" : "&*") +
                           " works on ints, not on " + typeName(a) + " and " + typeName(b));
    const uint64_t x = static_cast<uint64_t>(a.as.i), y = static_cast<uint64_t>(b.as.i);
    sp_[-2] = Value::integer(static_cast<int64_t>(op == Op::AddWrap ? x + y : op == Op::SubWrap ? x - y : x * y));
    sp_--;
    DONE();
}
}
CASES(Shl, Shr, Ushr) {
    SAVE_IP();
    if (shift(opAt(ip - kOpBytes))) LOAD_FRAME();
    DONE();
}

CASES(BitAnd, BitOr, BitXor) {
    SAVE_IP();
    if (bitwise(opAt(ip - kOpBytes))) LOAD_FRAME();
    DONE();
}

CASE(BitNot) {
    {
    Value a = sp_[-1];
    if (a.isInt() || a.isByte()) {
        sp_[-1] = Value::integer(~a.as.i);
    } else if (classOf(a) != nullptr) {
        SAVE_IP();
        invokeOperator(a, names.opBitNot, 0);
        LOAD_FRAME();
    } else {
        throw runtimeError(Thrown::UnsupportedOperationException, "cannot apply '~' to a value of type " + typeName(a));
    }
    DONE();
}
}

CASE(Neg) {
LABEL(plainNeg) {
    Value a = sp_[-1];
    if (a.isInt()) {
        if (a.as.i == INT64_MIN) throw overflowError(Op::Sub, 0, a.as.i);  // (D27)
        sp_[-1] = Value::integer(-a.as.i);
    } else if (a.isFloat()) {
        sp_[-1] = Value::number(-a.as.d);
    } else if (a.isByte()) {
        sp_[-1] = Value::integer(-a.as.i);  // a byte negated is an int (D19)
    } else if (classOf(a) != nullptr) {
        SAVE_IP();
        invokeOperator(a, names.opNeg, 0);
        LOAD_FRAME();
    } else {
        throw runtimeError(Thrown::UnsupportedOperationException, "cannot negate a value of type " + typeName(a));
    }
    DONE();
}
}
CASE(Not) {
LABEL(plainNot)
    sp[-1].setBool(!sp[-1].truthy());
    NEXT();
}

CASES(Eq, EqI, EqF) {
LABEL(plainEq) {
    Value b = sp_[-1], a = sp_[-2];
    if (const ObjClass* k = classOf(a); k != nullptr && k->methods.find(names.opEq) != nullptr) {
        SAVE_IP();
        invokeOperator(a, names.opEq, 1);
        LOAD_FRAME();
        DONE();
    }
    // A plain value (a number, a string) against an object whose class says what == means: its
    // == decides, as it does the other way round (2026-11-19: `5 == BigInt.of(5)`).
    if (classOf(a) == nullptr)
        if (const ObjClass* k = classOf(b); k != nullptr && k->methods.find(names.opEq) != nullptr) {
            sp_[-2] = b;
            sp_[-1] = a;
            SAVE_IP();
            invokeOperator(b, names.opEq, 1);
            LOAD_FRAME();
            DONE();
        }
    if ((a.isRef() || b.isRef() || a.isSmall() || b.isSmall()) ||
        (a.isObj() && b.isObj() && a.as.obj != b.as.obj && a.as.obj->type != ObjType::String)) {
        // Two lists, maps or ranges: equal if what they hold is equal.
        SAVE_IP();
        const bool same = equals(a, b);
        sp_[-2] = Value::boolean(same);
        sp_--;
        DONE();
    }
    sp_[-2] = Value::boolean(valuesEqual(a, b));
    sp_--;
    DONE();
}
}
CASE(Identical) {
    {
    // The very same object. Numbers, booleans and strings have no identity
    // apart from their value.
    if (sp_[-1].isRef() || sp_[-2].isRef() || sp_[-1].isSmall() || sp_[-2].isSmall()) {
        SAVE_IP();
        sp_[-1] = boxed(sp_[-1]);
        sp_[-2] = boxed(sp_[-2]);
    }
    Value b = sp_[-1], a = sp_[-2];
    const bool objects = a.isObj() && b.isObj() && !a.isString();
    // Nor has a value (M6.6 part 2): two of the same fields are the same.
    if (objects && a.isObjType(ObjType::Value) && b.isObjType(ObjType::Value)) {
        SAVE_IP();
        sp_[-2] = Value::boolean(a.as.obj == b.as.obj || valuesSame(asValue(a), asValue(b), 0));
    } else {
        sp_[-2] = Value::boolean(objects ? a.as.obj == b.as.obj : valuesEqual(a, b));
    }
    sp_--;
    DONE();
}
}
CASES(Lt, LtI, LtF) {
    FAST_COMPARISON(sp[-2], sp[-1], <, sp--; NEXT())
LABEL(plainLt)
    COMPARISON(Op::Lt, <)
}
CASES(Le, LeI, LeF) {
    FAST_COMPARISON(sp[-2], sp[-1], <=, sp--; NEXT())
LABEL(plainLe)
    COMPARISON(Op::Le, <=)
}
CASES(Gt, GtI, GtF) {
    FAST_COMPARISON(sp[-2], sp[-1], >, sp--; NEXT())
LABEL(plainGt)
    COMPARISON(Op::Gt, >)
}
CASES(Ge, GeI, GeF) {
    FAST_COMPARISON(sp[-2], sp[-1], >=, sp--; NEXT())
LABEL(plainGe)
    COMPARISON(Op::Ge, >=)
}

CASE(Is) {
    {
    Value type = *--sp_;
    if (!type.isObjType(ObjType::Class)) throw runtimeError(Thrown::ClassCastException, "right operand of 'is' must be a class");
    Value value = sp_[-1];
    const ObjClass* cls = classOf(value);
    sp_[-1] = Value::boolean(cls != nullptr ? cls->isSubclassOf(asClass(type)) : builtinFitsClass(value, asClass(type)));
    DONE();
}
}

CASE(Jump) {
    ip = code + READ_I32();
    DONE();
}
HAS_COLD(Loop)
CASE(Loop) {
    ip = code + READ_I32();
    if (bytesAllocated_ > nextGC_ || attention_.load(std::memory_order_relaxed)) GO_COLD(Loop);
    DONE();
}
COLD(Loop) {
    SAVE_IP();  // where it is, for a sample (ww --profile)
    safePoint();
    DONE();
}
CASE(JumpIfFalse) {
    {
    int target = READ_I32();
    if (!(*--sp).truthy()) ip = code + target;
    NEXT();
}
}
CASE(And) {
    {
    int target = READ_I32();
    if (!sp[-1].truthy()) ip = code + target;
    else sp--;
    NEXT();
}
}
CASE(Or) {
    {
    int target = READ_I32();
    if (sp[-1].truthy()) ip = code + target;
    else sp--;
    NEXT();
}
}

CASE(JumpIfNull) {
    {
    int target = READ_I32();
    if (sp_[-1].isNull()) ip = code + target;
    DONE();
}
}
CASE(JumpIfNotNull) {
    {
    int target = READ_I32();
    if (!sp_[-1].isNull()) ip = code + target;
    DONE();
}
}
CASE(AssertNotNull) {
    if (sp_[-1].isNull()) throw runtimeError(Thrown::NullPointerException, "'!!' was used on a value that is null");
    DONE();
}
CASE(Swap) {
    swapValues(sp[-1], sp[-2]);
    NEXT();
}
CASE(PopUnder) {
    {
    int count = READ_U16();
    Value* first = sp_ - 1 - count;
    closeUpvalues(first);
    *first = sp_[-1];
    sp_ = first + 1;
    DONE();
}
}
CASE(IsType) {
    {
    uint8_t type = READ_U8();
    ObjClass* cls = popTypeClass(type);
    sp_[-1] = Value::boolean(matchesType(sp_[-1], type, cls));
    DONE();
}
}
CASE(Cast) {
    {
    uint8_t type = READ_U8();
    ObjClass* cls = popTypeClass(type);
    if (!matchesType(sp_[-1], type, cls))
        throw runtimeError(Thrown::ClassCastException, "cannot cast a value of type " + typeName(sp_[-1]) + " to " +
                           declaredTypeText(type, cls));
    DONE();
}
}
CASE(CastOrNull) {
    {
    uint8_t type = READ_U8();
    ObjClass* cls = popTypeClass(type);
    if (!matchesType(sp_[-1], type, cls)) sp_[-1] = Value::null();
    DONE();
}
}
CASE(Settle) {
    {
    Value top = sp_[-1];
    Value* target = base + READ_U16();
    for (Value* p = sp_ - 1; p < target; p++) *p = Value::null();
    *target = top;
    sp_ = target + 1;
    DONE();
}
}

HAS_COLD(Call)
CASE(Call) {
    // A call of a closure that needs nothing done first, here; any other in the rare part, below.
    if (sp[-ip[0] - 1].isObjType(ObjType::Closure)) QUICK_CALL(asClosure(sp[-ip[0] - 1]), ip[0], 1)
    GO_COLD(Call);
}
COLD(Call) {
    {
    int argc = READ_U8();
    GC_SAFE_POINT();
    SAVE_IP();
    if (callValue(sp_[-argc - 1], argc)) LOAD_FRAME();
    DONE();
}
}
CASE(CallNamed) {
    {
    int positional = READ_U8();
    int named = READ_U8();
    GC_SAFE_POINT();
    SAVE_IP();
    if (callNamed(positional, named)) LOAD_FRAME();
    DONE();
}
}
CASE(Spread) {
    // Its own values gone before it goes on (a call as the last step leaves none behind).
    {
        {
        Value added = *--sp_;
        if (!added.isObjType(ObjType::List))
            throw runtimeError(Thrown::IllegalArgumentException, "'*' in a call needs a list to spread, not a value of type " + typeName(added));
        ObjList* list = asList(sp_[-1]);
        guardRead(added);
        const std::vector<Value> items = listCopy(asList(added));
        listInsert(list, list->size(), items.data(), items.size());
    }
    DONE();
}
}
CASE(CallSpread) {
    {
    int named = READ_U8();
    GC_SAFE_POINT();
    SAVE_IP();
    if (callSpread(named)) LOAD_FRAME();
    DONE();
}
}
HAS_COLD(InvokeOwn)
CASES(InvokeOwn, Invoke) {
    // A method of an object's class, open to this code, called with nothing to do first, here;
    // anything else in the rare part, below.
    if (sp[-ip[2] - 1].isObjType(ObjType::Instance)) {
        const ObjInstance* instance = asInstance(sp[-ip[2] - 1]);
        const ObjString* name = asString(consts[FAST_U16(0)]);
        if (((name->restricted & 1) == 0 || opAt(ip - kOpBytes) == Op::InvokeOwn) &&
            (instance->core == 0 || instance->core == coreId_)) {
            if (const Value* method = instance->cls->methods.find(name)) QUICK_CALL(asClosure(*method), ip[2], 3)
        }
    }
    GO_COLD(InvokeOwn);
}
COLD(InvokeOwn) {
    {
    // A method of the object's class, open to this code.
    if (sp[-ip[2] - 1].isObjType(ObjType::Instance)) {
        const ObjInstance* instance = asInstance(sp[-ip[2] - 1]);
        const ObjString* name = asString(consts[FAST_U16(0)]);
        if (((name->restricted & 1) == 0 || opAt(ip - kOpBytes) == Op::InvokeOwn) &&
            (instance->core == 0 || instance->core == coreId_)) {
            if (const Value* method = instance->cls->methods.find(name)) QUICK_CALL(asClosure(*method), ip[2], 3)
        }
    } else if (sp[-ip[2] - 1].isSmall()) {
        // A method of a small value's type, run on the value itself (M6.6 part 5).
        const ObjClass* cls = smallClassOf(sp[-ip[2] - 1]);
        const ObjString* name = asString(consts[FAST_U16(0)]);
        if ((name->restricted & 1) == 0 || opAt(ip - kOpBytes) == Op::InvokeOwn)
            if (const Value* method = cls->methods.find(name)) QUICK_CALL(asClosure(*method), ip[2], 3)
    } else if (sp[-ip[2] - 1].isObjType(ObjType::Value)) {
        // A method of a value's type (M6.6 part 2): a value never changes, so
        // any core may run one on it.
        const ObjValue* box = asValue(sp[-ip[2] - 1]);
        const ObjString* name = asString(consts[FAST_U16(0)]);
        if ((name->restricted & 1) == 0 || opAt(ip - kOpBytes) == Op::InvokeOwn)
            if (const Value* method = box->cls->methods.find(name)) QUICK_CALL(asClosure(*method), ip[2], 3)
    } else if (sp[-ip[2] - 1].isRef()) {
        // A method of the type of a value where it is kept, an item of a list say, run on it
        // there (G9): a mutating one changes it in its place.
        const ObjClass* cls = refClassOrNull(sp[-ip[2] - 1]);
        const ObjString* name = asString(consts[FAST_U16(0)]);
        if (cls != nullptr && ((name->restricted & 1) == 0 || opAt(ip - kOpBytes) == Op::InvokeOwn))
            if (const Value* method = cls->methods.find(name)) QUICK_CALL(asClosure(*method), ip[2], 3)
    }
    ObjString* name = asString(consts[READ_U16()]);
    int argc = READ_U8();
    GC_SAFE_POINT();
    SAVE_IP();
    CHECK_ACCESS(sp_[-argc - 1], 1, false, opAt(ip - 3 - kOpBytes) != Op::InvokeOwn);
    // A method of a string, list, map, set or bytes written in C++, with nothing to see to
    // first, called here (text.indexOf(c), bytes.indexOf(10, at) in a loop); the rest in invoke().
    if (const NativeMethod* native = quickNative(sp_[-argc - 1], name, argc, sp_ - argc)) {
        nativeBase_ = sp_ - argc - 1;
        pendingTypeArgs_ = nullptr;
        pendingDestination_ = Value::null();
        const Value result = native->fn(*this, sp_[-argc - 1], argc, sp_ - argc);
        sp_ -= argc + 1;
        *sp_++ = result;
        DONE();
    }
    if (invoke(name, argc)) LOAD_FRAME();
    DONE();
}
}
CASES(InvokeOwnTrailing, InvokeTrailing) {
    {
    ObjString* name = asString(consts[READ_U16()]);
    int argc = READ_U8();
    GC_SAFE_POINT();
    SAVE_IP();
    CHECK_ACCESS(sp_[-argc - 1], 1, false, opAt(ip - 3 - kOpBytes) != Op::InvokeOwnTrailing);
    if (invoke(name, argc, true)) LOAD_FRAME();
    DONE();
}
}
CASES(SuperInvoke, SuperInvokeTrailing) {
    {
    const bool trailing = opAt(ip - kOpBytes) == Op::SuperInvokeTrailing;
    ObjString* name = asString(consts[READ_U16()]);
    int argc = READ_U8();
    GC_SAFE_POINT();
    Value super = *--sp_;
    CHECK_ACCESS(sp_[-argc - 1], 1, false, true);
    const Value* method = superMethod(super, sp_[-argc - 1], name);
    if (method == nullptr && super.isObjType(ObjType::Class)) {
        SAVE_IP();
        if (ObjString* other = missedName(asClass(super), name, sp_[-argc - 1], false)) method = superMethod(super, sp_[-argc - 1], other);  // o-private-shadow
    }
    if (method == nullptr && super.isNull())
        throw runtimeError(Thrown::UnsupportedOperationException, "there is no '" + name->utf8() + "' for super to mean: this class has no superclass, " +
                           std::string("and its interfaces come with none"));
    if (method == nullptr && !super.isObjType(ObjType::Class)) throw runtimeError(Thrown::ClassCastException, "superclass must be a class");
    if (method == nullptr)
        throw runtimeError(Thrown::UnsupportedOperationException, "undefined method '" + name->utf8() + "' in superclass " +
                           asClass(super)->name->utf8());
    SAVE_IP();
    if (trailing) callTrailing(asClosure(*method), argc);
    else callClosure(asClosure(*method), argc);
    LOAD_FRAME();
    DONE();
}
}
CASE(SuperInit) {
    {
    // From a generated initializer: run the superclass's init, if it has one.
    int argc = READ_U8();
    GC_SAFE_POINT();
    Value super = *--sp_;
    if (super.isNull()) {
        // Everything the class lists after its ':' turned out to be an interface.
        if (argc != 0) throw runtimeError(Thrown::UnsupportedOperationException, "there is no superclass to pass arguments to: only a class takes them");
        DONE();
    }
    if (!super.isObjType(ObjType::Class)) throw runtimeError(Thrown::ClassCastException, "superclass must be a class");
    ObjClosure* init = asClass(super)->init;
    if (init == nullptr) {
        if (argc != 0)
            throw runtimeError(Thrown::IllegalArgumentException, asClass(super)->name->utf8() + "() takes no arguments but got " +
                               std::to_string(argc));
        DONE();  // nothing to run; the receiver stays as the result
    }
    SAVE_IP();
    callClosure(init, argc);
    LOAD_FRAME();
    DONE();
}
}

CASE(Closure) {
    {
    const FunctionProto* proto = frame->closure->proto->protos[static_cast<size_t>(READ_U16())];
    ObjClosure* closure = alloc<ObjClosure>(proto, proto->upvalues.size());
    if ((proto->typeArgs & kTypeArgsKept) != 0) closure->typeArgs = frameTypeArgs(frame - frames_);
    account(proto->upvalues.size() * sizeof(ObjUpvalue*));
    for (size_t i = 0; i < proto->upvalues.size(); i++) {
        const UpvalueDesc& uv = proto->upvalues[i];
        closure->upvalues[i] = uv.isLocal       ? captureUpvalue(base + uv.index)
                               : uv.fromCapture ? capturedOf(base[0], uv.index)
                                                : frame->closure->upvalues[uv.index];
    }
    *sp_++ = Value::object(closure);
    DONE();
}
}
CASE(MakeForms) {
    {
    const FunctionProto* chooser = frame->closure->proto->protos[static_cast<size_t>(READ_U16())];
    const int count = READ_U8();
    ObjForms* set = alloc<ObjForms>(chooser);
    account(static_cast<size_t>(count) * sizeof(ObjClosure*));
    for (int i = count; i > 0; i--) {
        if (!sp_[-i].isObjType(ObjType::Closure)) throw runtimeError(Thrown::InternalError, "invalid set of forms");
        set->forms.push_back(asClosure(sp_[-i]));
    }
    sp_ -= count;
    *sp_++ = Value::object(set);
    DONE();
}
}
CASE(MethodForms) {
    // Its own values gone before it goes on (a call as the last step leaves none behind).
    {
        {
        const FunctionProto* chooser = frame->closure->proto->protos[static_cast<size_t>(READ_U16())];
        const int count = READ_U8();
        if (!sp_[-count - 1].isObjType(ObjType::Class)) throw runtimeError(Thrown::InternalError, "invalid class declaration");
        ObjClass* cls = asClass(sp_[-count - 1]);
        ObjString* methodName = intern(chooser->name);
        std::vector<ObjClosure*> forms;
        for (int i = count; i > 0; i--) {
            ObjString* name = asString(sp_[-i]);
            const Value* method = cls->methods.find(name);
            if (method == nullptr) {
                // A form under the method's own name: compiled where the name had one form.
                const Value* plain = cls->methods.find(methodName);
                if (plain != nullptr && plain->isObjType(ObjType::Closure) && !asClosure(*plain)->proto->chooser &&
                    asClosure(*plain)->proto->signature.empty())
                    method = plain;
            }
            if (method == nullptr || !method->isObjType(ObjType::Closure)) {
                if (cls->isAbstract) continue;  // one left to the classes below
                throw runtimeError(Thrown::UnsupportedOperationException, "class " + cls->name->utf8() + " has no form " + name->utf8());
            }
            forms.push_back(asClosure(*method));
            cls->methods.set(name, *method);
        }
        sp_ -= count;
        if (forms.size() == 1) {
            cls->methods.set(methodName, Value::object(forms[0]));
        } else if (!forms.empty()) {
            ObjForms* set = alloc<ObjForms>(chooser);
            set->forms = std::move(forms);
            account(set->forms.size() * sizeof(ObjClosure*));
            cls->methods.set(methodName, Value::object(set));
        }
    }
    DONE();
}
}
CASE(StaticForms) {
    {
    const FunctionProto* chooser = frame->closure->proto->protos[static_cast<size_t>(READ_U16())];
    const int count = READ_U8();
    if (!sp_[-count - 1].isObjType(ObjType::Class)) throw runtimeError(Thrown::InternalError, "invalid class declaration");
    ObjClass* cls = asClass(sp_[-count - 1]);
    ObjForms* set = alloc<ObjForms>(chooser);
    cls->statics.set(intern(chooser->name), Value::object(set));
    for (int i = count; i > 0; i--) {
        const Value* method = cls->statics.find(asString(sp_[-i]));
        if (method == nullptr || !method->isObjType(ObjType::Closure)) throw runtimeError(Thrown::InternalError, "invalid class declaration");
        set->forms.push_back(asClosure(*method));
    }
    account(set->forms.size() * sizeof(ObjClosure*));
    sp_ -= count;
    DONE();
}
}
CASE(GetIndexOr) {
    {
    const Value start = sp_[-1], key = sp_[-2], map = sp_[-3];
    if (!map.isObjType(ObjType::Map))
        throw runtimeError(Thrown::IllegalArgumentException, "only a map's index takes a default, not that of a value of type " + typeName(map));
    guardRead(map);
    const Value* found = asMap(map)->map.find(own(key));
    sp_ -= 2;
    sp_[-1] = found != nullptr ? *found : start;
    DONE();
}
}
CASE(CloseUpvalue) {
    closeUpvalues(sp_ - 1);
    sp_--;
    DONE();
}
CASE(CallOn) {
    {
    const int argc = READ_U8();
    Value* const at = sp_ - argc - 2;
    const Value callee = at[0];
    SAVE_IP();
    if (!callee.isObjType(ObjType::Closure)) {
        // Only a function written in WinterWolf runs on an object: any other is given the arguments alone.
        for (int i = 0; i < argc; i++) at[1 + i] = at[2 + i];
        sp_--;
        if (callValue(callee, argc)) LOAD_FRAME();
        DONE();
    }
    for (int i = 0; i <= argc; i++) at[i] = at[1 + i];
    sp_--;
    callClosure(asClosure(callee), argc);
    LOAD_FRAME();
    DONE();
}
}
CASE(ImplicitFirst) {
    {
    const int slot = READ_U16();
    const int flags = READ_U8();
    if (implicitFirstCount_ < 8) implicitFirst_[implicitFirstCount_++] = base[slot];
    if ((flags & 1) != 0) implicitOnly_ = true;
    DONE();
}
}
CASE(TailLoop) {
    {
    // The function calls itself as its last step (D29): what a closure kept of
    // this round keeps its values, the arguments become the parameters, and it
    // starts again in this frame.
    const int count = READ_U8();
    Value* const given = sp_ - count;
    closeUpvalues(base + 1);
    for (int i = 0; i < count; i++) base[1 + i] = given[i];
    sp_ = base + 1 + count;
    frame->repeats++;
    ip = code;
    GC_SAFE_POINT();
    DONE();
}
}

CASE(ReturnInit) {
    // The object is made once its own class's initializer has returned.
    if (base[0].isObjType(ObjType::Instance) && asInstance(base[0])->cls->init == frame->closure)
        asInstance(base[0])->constructing = false;
    else if (base[0].isObjType(ObjType::Value) && asValue(base[0])->cls->init == frame->closure) {
        ObjValue* box = asValue(base[0]);
        box->valueOpen = false;  // and from now on never changes
        if (box->cls->smallId != 0) {
            // Small: given back in the value itself (M6.6 part 5).
            const Value made = smallFrom(box->cls, box->data(), box->cls->layout.load(std::memory_order_relaxed)->size, box->type);
            if (sp[-1].kind == ValueKind::Obj && sp[-1].as.obj == box) sp[-1] = made;
            base[0] = made;
        }
    }
    // Slot 0 is the new instance: its required fields must be set by now.
    if (base[0].isObjType(ObjType::Instance) && asInstance(base[0])->cls->anyRequired) {
        SAVE_IP();
        checkRequiredFields(asInstance(base[0]), frame->closure);
    }
    FALL_INTO(Return);
}
HAS_COLD(Return)
CASE(Return) {
    // Back to the caller, where there is nothing else to see to: no variable of the frame
    // captured, no catch block of its own left, its result wanted and not a place (M6.5 part 4).
    if ((openUpvalues_ == nullptr || openUpvalues_->location < base) && !frame->dropResult && frameCount_ - 1 != RUN_BASE &&
        (handlers_.empty() || handlers_.back().frameIndex < frameCount_ - 1) && sp[-1].kind != ValueKind::Ref) {
        std::memcpy(static_cast<void*>(base), &sp[-1], sizeof(Value));
        sp = base + 1;
        frameCount_--;
        LOAD_FRAME();
        NEXT();
    }
    GO_COLD(Return);
}
COLD(Return) {
    {
    // Its function's whole region made here, where a section's core counted its calls (nativeCall).
    if ((frame->dropResult & kMakeWhole) != 0) {
        frame->dropResult &= static_cast<uint8_t>(~kMakeWhole);
        wholeAtReturn(*const_cast<FunctionProto*>(frame->closure->proto), base, sp);
    }
    // As above, a place too, where it is kept as it is.
    if ((openUpvalues_ == nullptr || openUpvalues_->location < base) && !frame->dropResult &&
        frameCount_ - 1 != RUN_BASE && (handlers_.empty() || handlers_.back().frameIndex < frameCount_ - 1) &&
        (sp[-1].kind != ValueKind::Ref || (keptAsItIs(frame) && sp[-1].as.obj->type != ObjType::Places) || madeAsAsked(frame, sp[-1]))) {
        Value* const top = base;
        *top = sp[-1];
        sp = top + 1;
        frameCount_--;
        LOAD_FRAME();
        NEXT();
    }
    Value result = sp_[-1];
    if (result.isRef()) [[unlikely]] result = returnedRef(result, frameCount_ - 1);
    closeUpvalues(base);
    frameCount_--;
    // Drop the catch blocks of a function that returned from inside a try.
    while (!handlers_.empty() && handlers_.back().frameIndex >= frameCount_) handlers_.pop_back();
    sp_ = base;
    if ((frame->dropResult & kDropResult) == 0) *sp_++ = result;
    if (frameCount_ == RUN_BASE) EXIT_RUN();
    LOAD_FRAME();
    DONE();
}
}

CASE(Class) {
    {
    ObjString* name = asString(consts[READ_U16()]);
    *sp_++ = Value::object(alloc<ObjClass>(name));
    account(256);  // rough cost of the method tables
    DONE();
}
}
CASE(Inherit) {
    {
    const int count = READ_U8();
    Value sub = sp_[-1];
    Value* types = sp_ - 1 - count;
    if (!sub.isObjType(ObjType::Class)) throw runtimeError(Thrown::InternalError, "invalid class declaration");
    ObjClass* subclass = asClass(sub);

    // Of the supertypes, at most one is a class: the superclass.
    ObjClass* superclass = nullptr;
    for (int i = 0; i < count; i++) {
        if (!types[i].isObjType(ObjType::Class))
            throw runtimeError(Thrown::ClassCastException, "superclass must be a class, got " + typeName(types[i]));
        ObjClass* type = asClass(types[i]);
        if (type->isInterface) continue;
        if (type->isEnum) throw runtimeError(Thrown::IllegalStateException, "cannot inherit from enum " + type->name->utf8());
        if (type->isValue)
            throw runtimeError(Thrown::IllegalStateException, "'" + subclass->name->utf8() + "' cannot extend value '" + type->name->utf8() +
                               "': a value type has no subclasses");
        if (subclass->isValue)
            throw runtimeError(Thrown::IllegalStateException, "value '" + subclass->name->utf8() + "' cannot extend class '" + type->name->utf8() +
                               "': a value type may only implement interfaces");
        if (subclass->isInterface)
            throw runtimeError(Thrown::IllegalStateException, "interface " + subclass->name->utf8() + " cannot extend class " + type->name->utf8());
        if (superclass != nullptr)
            throw runtimeError(Thrown::IllegalStateException, "class " + subclass->name->utf8() + " can have only one superclass, but " +
                               superclass->name->utf8() + " and " + type->name->utf8() + " are both classes");
        superclass = type;
    }
    if (superclass != nullptr) {
        subclass->super = superclass;
        subclass->methods.addAll(superclass->methods);
        subclass->init = superclass->init;
        subclass->declared = superclass->declared;  // laid out again for the subclass's objects
        subclass->declaredIndex.addAll(superclass->declaredIndex);
        subclass->shape = newShape();
        subclass->fieldInits = superclass->fieldInits;
        subclass->anyRequired = superclass->anyRequired;
        subclass->getters.addAll(superclass->getters);
        subclass->setters.addAll(superclass->setters);
        subclass->hasAccessors = superclass->hasAccessors;
        subclass->accessMarks.addAll(superclass->accessMarks);
        subclass->accessIds = superclass->accessIds;
        subclass->accessBits = superclass->accessBits;
        subclass->interfaces = superclass->interfaces;
        subclass->required = superclass->required;  // what an abstract class left unwritten
    }

    // An interface gives the methods and properties it has a body for, where
    // the superclass has none of that name, and asks for the rest.
    auto implement = [&](ObjClass* type) {
        for (ObjClass* known : subclass->interfaces)
            if (known == type) return;
        subclass->interfaces.push_back(type);
    };
    for (int i = 0; i < count; i++) {
        ObjClass* type = asClass(types[i]);
        if (!type->isInterface) continue;
        implement(type);
        for (ObjClass* extended : type->interfaces) implement(extended);
        subclass->methods.addMissing(type->methods);
        subclass->getters.addMissing(type->getters);
        subclass->setters.addMissing(type->setters);
        subclass->hasAccessors = subclass->hasAccessors || type->hasAccessors;
        for (uint32_t at = 0; at < type->accessMarks.capacity(); at++) {
            // What the interface keeps to itself, the class keeps so too.
            const Table::Entry& entry = type->accessMarks.entries()[at];
            if (entry.key == nullptr || subclass->accessMarks.find(entry.key) != nullptr) continue;
            for (bool write : {false, true}) {
                const uint32_t half = halfOf(entry.value, write);
                if (halfCount(half) == 0) continue;
                setAccess(subclass, subclass->accessMarks, entry.key, write,
                          std::vector<uint16_t>(halfRun(type, half), halfRun(type, half) + halfCount(half)), halfKind(half));
            }
        }
        subclass->required.insert(subclass->required.end(), type->required.begin(), type->required.end());
    }
    joinProtected(subclass);
    types[0] = superclass != nullptr ? Value::object(superclass) : Value::null();
    sp_ = types + 1;
    DONE();
}
}
CASE(ClassInit) {
    {
    Value init = sp_[-1], cls = sp_[-2];
    if (!cls.isObjType(ObjType::Class) || !init.isObjType(ObjType::Closure) || asClosure(init)->proto->arity != 0)
        throw runtimeError(Thrown::InternalError, "invalid class declaration");
    asClass(cls)->staticInit = asClosure(init);
    sp_--;
    DONE();
}
}
CASE(EnsureClassInit) {
    if (!sp_[-1].isObjType(ObjType::Class)) throw runtimeError(Thrown::InternalError, "invalid class declaration");
    SAVE_IP();
    ensureClassInit(asClass(sp_[-1]));
    DONE();
}
CASE(MissingArg) {
    {
    ObjString* parameter = asString(consts[READ_U16()]);
    throw runtimeError(Thrown::IllegalArgumentException, frame->closure->proto->name + "() is missing argument '" + parameter->utf8() + "'");
}
}
CASE(DataField) {
    {
    ObjString* name = asString(consts[READ_U16()]);
    if (!sp_[-1].isObjType(ObjType::Class)) throw runtimeError(Thrown::InternalError, "invalid class declaration");
    asClass(sp_[-1])->isData = true;
    if (name->length > 0) asClass(sp_[-1])->components.push_back(name);  // "": a data class of no values
    DONE();
}
}
CASE(Extend) {
    {
    // extend Person : Weighed (D33): the class fits the interface, and the
    // ones above it, and has what they bring that it has not.
    if (!sp_[-1].isObjType(ObjType::Class) || !asClass(sp_[-1])->isInterface)
        throw runtimeError(Thrown::IllegalArgumentException, "an extend fits a class to an interface");
    if (sp_[-2].isObjType(ObjType::TypeInfo)) {
        // A built-in type (G30): kept by its id, with the interfaces above too.
        ObjTypeInfo* type = static_cast<ObjTypeInfo*>(sp_[-2].as.obj);
        ObjClass* iface = asClass(sp_[-1]);
        const uint8_t id = unfrozenTypeId(type->id);
        if (id == 0 || id == kTypeInstance || id >= kTypeCount_)
            throw runtimeError(Thrown::IllegalArgumentException, "an extend fits a class or a built-in type to an interface");
        if (coreId_ != 0) throw notOwned("an extend is made for the whole program");
        std::deque<BuiltinFit>& fits = builtinFits_[id];
        auto fit = [&](ObjClass* to) -> BuiltinFit* {
            for (BuiltinFit& f : fits)
                if (f.type == type && f.iface == to) return &f;
            fits.emplace_back();
            fits.back().type = type;
            fits.back().iface = to;
            return &fits.back();
        };
        for (ObjClass* above : iface->interfaces) fit(above);
        extending_ = fit(iface);  // last, so that no later one moves it before its getters come
        hasBuiltinFits_ = true;
        extensionEpoch_++;
        sp_--;
        DONE();
    }
    if (!sp_[-2].isObjType(ObjType::Class)) throw runtimeError(Thrown::IllegalArgumentException, "an extend fits a class to an interface");
    ObjClass* iface = asClass(sp_[-1]);
    ObjClass* cls = asClass(sp_[-2]);
    auto fit = [&](ObjClass* to) {
        if (std::find(cls->extendedTo.begin(), cls->extendedTo.end(), to) == cls->extendedTo.end())
            cls->extendedTo.push_back(to);
    };
    fit(iface);
    for (ObjClass* above : iface->interfaces) fit(above);
    for (uint32_t i = 0; i < iface->methods.capacity(); i++) {
        const Table::Entry& e = iface->methods.entries()[i];
        if (e.key != nullptr && cls->methods.find(e.key) == nullptr) cls->extendMethods.set(e.key, e.value);
    }
    for (uint32_t i = 0; i < iface->getters.capacity(); i++) {
        const Table::Entry& e = iface->getters.entries()[i];
        if (e.key != nullptr && cls->getters.find(e.key) == nullptr) cls->extendGetters.set(e.key, e.value);
    }
    sp_--;
    DONE();
}
}
CASE(Bridge) {
    {
    ObjString* name = asString(consts[READ_U16()]);
    if (sp_[-2].isObjType(ObjType::TypeInfo) && extending_ != nullptr) {
        extending_->getters.set(name, sp_[-1]);  // of a built-in type (G30)
        sp_--;
        DONE();
    }
    if (!sp_[-2].isObjType(ObjType::Class)) throw runtimeError(Thrown::InternalError, "invalid extend");
    asClass(sp_[-2])->extendGetters.set(name, sp_[-1]);
    sp_--;
    DONE();
}
}
CASE(BridgeMethod) {
    {
    // A fun in an extend (D186): the interface's method, for a type that has none.
    ObjString* name = asString(consts[READ_U16()]);
    if (sp_[-2].isObjType(ObjType::TypeInfo) && extending_ != nullptr) {
        extending_->methods.set(name, sp_[-1]);
    } else {
        if (!sp_[-2].isObjType(ObjType::Class)) throw runtimeError(Thrown::InternalError, "invalid extend");
        asClass(sp_[-2])->extendMethods.set(name, sp_[-1]);
    }
    sp_--;
    DONE();
}
}
CASE(Lazy) {
    {
    if (!sp_[-1].isObjType(ObjType::Closure)) throw runtimeError(Thrown::InternalError, "invalid lazy value");
    ObjLazy* lazy = alloc<ObjLazy>();
    lazy->init = asClosure(sp_[-1]);
    sp_[-1] = Value::lazy(lazy);
    DONE();
}
}
CASE(UnstableKey) {
    if (!sp_[-1].isObjType(ObjType::Class)) throw runtimeError(Thrown::InternalError, "invalid class declaration");
    asClass(sp_[-1])->unstableKey = true;
    DONE();
}
CASE(Component) {
    {
    const size_t index = READ_U8();
    SAVE_IP();
    sp_[-1] = componentOf(sp_[-1], index);
    DONE();
}
}
CASE(IterNextPair) {
    {
    const int slot = READ_U16();
    const int target = READ_I32();
    const Value iterator = base[slot];
    if (!iterator.isObjType(ObjType::Iterator)) throw runtimeError(Thrown::InternalError, "invalid loop state");
    ObjIterator* it = asIterator(iterator);
    SAVE_IP();
    if (it->source.isObjType(ObjType::Map)) {
        // A map's key and value, as they are: no entry made (D73).
        checkUnchanged(it);
        const std::vector<ValueMap::Entry>& entries = asMap(it->source)->map.entries();
        while (it->index < entries.size() && !entries[it->index].live) it->index++;
        if (it->index >= entries.size()) {
            ip = code + target;
            DONE();
        }
        const ValueMap::Entry& entry = entries[it->index++];
        sp_[0] = entry.key;
        sp_[1] = entry.value;
        sp_ += 2;
        DONE();
    }
    Value item;
    if (!iteratorNext(it, item)) {
        ip = code + target;
        DONE();
    }
    // Its parts read with the item where the collector sees it.
    sp_[0] = item;
    sp_[1] = item;
    sp_ += 2;
    sp_[-2] = componentOf(item, 0);
    sp_[-1] = componentOf(item, 1);
    DONE();
}
}
CASE(MarkInterface) {
    if (!sp_[-1].isObjType(ObjType::Class)) throw runtimeError(Thrown::InternalError, "invalid class declaration");
    asClass(sp_[-1])->isInterface = true;
    DONE();
}
CASE(Extension) {
    {
    ObjString* name = asString(consts[READ_U16()]);
    const int type = READ_U8();
    Value function = *--sp_;
    if (!function.isObjType(ObjType::Closure)) throw runtimeError(Thrown::InternalError, "invalid extension function");
    if (coreId_ != 0) throw notOwned("an extension function is added for the whole program");
    const int id = type & kTypeMask;
    if (id == kTypeInstance) {
        Value target = *--sp_;
        if (!target.isObjType(ObjType::Class))
            throw runtimeError(Thrown::IllegalArgumentException, "an extension function is added to a class or a built-in type, not to a value of type " +
                               typeName(target));
        addExtensionForm(asClass(target)->extensions, name, function);
    } else if (id == 0) {
        addExtensionForm(anyExtensions_, name, function);
    } else {
        addExtensionForm(typeExtensions_[id], name, function);
    }
    extensionEpoch_++;
    if (type & kTypeNullable) {
        // Remembered with the type it is for: a second type declaring the
        // same name leaves a call on null with no answer (see findExtension).
        Value owner = id == kTypeInstance ? Value::object(asClass(sp_[0])) : Value::integer(id);
        const Value* before = nullExtensionOwners_.find(name);
        const bool same = before == nullptr || (before->isInt() && owner.isInt() ? before->as.i == owner.as.i
                                                : before->isObj() && owner.isObj() && before->as.obj == owner.as.obj);
        nullExtensions_.set(name, same ? function : Value::boolean(false));
        if (before == nullptr) nullExtensionOwners_.set(name, owner);
        // Under its own name as well, which a call the checker could tell names.
        if (const std::string& signature = asClosure(function)->proto->signature; !signature.empty())
            nullExtensions_.set(intern(name->utf8() + signature), function);
    }
    hasExtensions_ = true;
    DONE();
}
}
CASE(MarkAbstract) {
    if (!sp_[-1].isObjType(ObjType::Class)) throw runtimeError(Thrown::InternalError, "invalid class declaration");
    asClass(sp_[-1])->isAbstract = true;
    DONE();
}
CASE(ClassOrigin) {
    {
    ObjString* origin = asString(consts[READ_U16()]);
    if (!sp_[-1].isObjType(ObjType::Class)) throw runtimeError(Thrown::InternalError, "invalid class declaration");
    asClass(sp_[-1])->origin = origin;
    DONE();
}
}
CASE(MarkValue) {
    {
    const int flags = READ_U8();
    if (!sp_[-1].isObjType(ObjType::Class)) throw runtimeError(Thrown::InternalError, "invalid class declaration");
    asClass(sp_[-1])->isValue = true;
    asClass(sp_[-1])->madeFromHeader = (flags & 1) != 0;
    asClass(sp_[-1])->phantomParams = static_cast<uint8_t>(flags >> 1);  // G4
    DONE();
}
}
HAS_COLD(Numeric)
CASE(Numeric) {
    {
    // The commonest at once: a float's toInt() and an int's toByte(). The handler is kept as
    // small as that, as a bigger one costs every call of it more; the other quick cases are
    // in the cold part, a jump away.
    Value* const top = sp_ - 1;
    if (ip[0] == static_cast<uint8_t>(NumericOp::ToInt) && top->isFloat() && top->as.d > -9.2e18 && top->as.d < 9.2e18) {
        top->setInt(static_cast<int64_t>(top->as.d));  // the whole part, as toInt() gives
        ip += 1;
        DONE();
    }
    if (ip[0] == static_cast<uint8_t>(NumericOp::ToByte) && top->isInt() && static_cast<uint64_t>(top->as.i) <= 255) {
        top->setHeader(ValueKind::Byte);  // the number stays as it is
        ip += 1;
        DONE();
    }
    GO_COLD(Numeric);
    }
}
COLD(Numeric) {
    {
    // The other common cases, a conversion of the kind expected or Math's functions on ints or
    // on floats; anything else (a NaN, a number too big, the wrong kind) as the method does it,
    // with its error.
    // DONE() is not used inside the switch: built with MSVC it is a `break`, which would leave
    // only this switch, and the instruction would go on to the cold way with its operand read.
    Value* const top = sp_ - 1;
    bool done = false;
    switch (static_cast<NumericOp>(ip[0])) {
        case NumericOp::ToInt:
            if (top->isByte()) {
                top->setInt(top->as.i);
                done = true;
                break;
            }
            break;
        case NumericOp::ToFloat:
            if (top->isInt()) {
                top->setFloat(static_cast<double>(top->as.i));
                done = true;
                break;
            }
            if (top->isFloat()) {
                done = true;
                break;
            }
            break;
        case NumericOp::ToByteWrapped:
            if (top->isInt()) {
                const int64_t low = top->as.i & 255;
                top->setHeader(ValueKind::Byte);
                top->as.i = low;
                done = true;
                break;
            }
            break;
        case NumericOp::Floor:
        case NumericOp::Ceil:
        case NumericOp::Round:
            if (top->isFloat() && top->as.d > -9.2e18 && top->as.d < 9.2e18) {
                const double d = top->as.d;
                const NumericOp which = static_cast<NumericOp>(ip[0]);
                top->setInt(static_cast<int64_t>(which == NumericOp::Floor ? std::floor(d) : which == NumericOp::Ceil ? std::ceil(d) : std::round(d)));
                done = true;
                break;
            }
            break;
        case NumericOp::Sqrt:
            if (top->isFloat()) {
                top->as.d = std::sqrt(top->as.d);
                done = true;
                break;
            }
            break;
        case NumericOp::AbsInt:
        case NumericOp::AbsFloat:
            if (top->isInt() && top->as.i != INT64_MIN) {
                if (top->as.i < 0) top->as.i = -top->as.i;
                done = true;
                break;
            }
            if (top->isFloat()) {
                top->as.d = std::fabs(top->as.d);
                done = true;
                break;
            }
            break;
        case NumericOp::MinInt:
        case NumericOp::MaxInt:
        case NumericOp::MinFloat:
        case NumericOp::MaxFloat: {
            // The first is kept unless the second is less (min) or more (max): as the cold way.
            Value* const first = top - 1;
            const bool min = ip[0] == static_cast<uint8_t>(NumericOp::MinInt) || ip[0] == static_cast<uint8_t>(NumericOp::MinFloat);
            if (first->isInt() && top->isInt()) {
                if (min ? top->as.i < first->as.i : top->as.i > first->as.i) first->as.i = top->as.i;
                sp_--;
                done = true;
                break;
            }
            if (first->isFloat() && top->isFloat()) {
                if (min ? top->as.d < first->as.d : top->as.d > first->as.d) first->as.d = top->as.d;
                sp_--;
                done = true;
                break;
            }
            break;
        }
        default:
            break;
    }
    if (done) {
        ip += 1;
        DONE();
    }
    const uint8_t which = READ_U8();
    SAVE_IP();
    const int arity = numericArity(which);
    const Value result = numeric(which, sp_ - arity);
    sp_ -= arity - 1;
    sp_[-1] = result;
    DONE();
}
}
CASE(WithRefResult) {
    pendingDestination_ = Value::undefined();  // taken by a function that gives a value
    DONE();
}
CASE(FieldDestination) {
    {
    ObjString* name = asString(consts[READ_U16()]);
    const Value obj = sp_[-1];
    Value place = Value::null();
    if (obj.isObjType(ObjType::Instance) && asInstance(obj)->inlined) {
        ObjInstance* instance = asInstance(obj);
        const int64_t at = instance->fieldIndex(name);
        if (at >= 0 && instance->fields()[at].isRef() && instance->fields()[at].as.obj == instance) place = instance->fields()[at];
    }
    sp_[-1] = place;
    DONE();
}
}
HAS_COLD(PlaceLocal)
CASE(PlaceLocal) {
    {
        // A variable holding an item of a list, or a value inside an object, where it is (a for
        // loop's over a list of values): its place is that, here (G9); anything else below.
        const Value& slot = base[FAST_U16(0)];
        if (slot.kind == ValueKind::Ref && (slot.as.obj->type == ObjType::List || slot.as.obj->type == ObjType::Instance)) {
            *sp++ = slot;
            ip += 2;
            NEXT();
        }
    }
    GO_COLD(PlaceLocal);
}
COLD(PlaceLocal) {
    ip = placeInstruction(opAt(ip - kOpBytes), ip, frame);
    DONE();
}
CASES(PlaceUpvalue, PlaceGlobal, PlaceField, PlaceIndex, InsertNew) {
    ip = placeInstruction(opAt(ip - kOpBytes), ip, frame);
    DONE();
}
CASE(AddNew) {
    {
    const int argc = READ_U8();
    const int target = READ_I32();
    if (addNew(argc)) ip = code + target;
    DONE();
}
}
CASE(WithDestinationAt) {
    pendingDestination_ = sp_[-READ_U8() - 1];
    DONE();
}
CASE(WithFrameDestination) {
    pendingDestination_ = (frame->closure->proto->typeArgs & kTakesDestination) != 0 ? frameDestination(frame - frames_)
                                                                                   : Value::null();
    DONE();
}

CASE(CheckOverride) {
    {
    ObjString* name = asString(consts[READ_U16()]);
    if (!sp_[-1].isObjType(ObjType::Class)) throw runtimeError(Thrown::InternalError, "invalid class declaration");
    const int flags = READ_U8();
    const bool marked = (flags & 1) != 0, property = (flags & 2) != 0;
    const ObjClass* cls = asClass(sp_[-1]);

    // What the class gets from above under this name: a method for a method,
    // a field or property for a field or property; or something an interface
    // or an abstract class asks for, which either kind supplies.
    auto has = [&](const ObjClass* from) {
        if (!property && from->methods.find(name) == nullptr && name->utf8().find('(') != std::string::npos) {
            // A form of a name with several (D56): the one above may have been
            // compiled where the name had one form, under the name alone.
            const Value* plain = from->methods.find(intern(name->utf8().substr(0, name->utf8().find('('))));
            return plain != nullptr && plain->isObjType(ObjType::Closure) && !asClosure(*plain)->proto->chooser &&
                   asClosure(*plain)->proto->signature.empty();
        }
        if (!property) return from->methods.find(name) != nullptr;
        return from->getters.find(name) != nullptr || from->setters.find(name) != nullptr ||
               from->declaredIndex.find(name) != nullptr;
    };
    bool inherited = cls->super != nullptr && has(cls->super);
    for (const ObjClass* implemented : cls->interfaces) inherited = inherited || has(implemented);
    const ObjClass* askedBy = nullptr;
    for (const auto& [wanted, by] : cls->required)
        if (wanted == name && by != cls) askedBy = by;

    if (marked) {
        // toString and == are there for every object to replace.
        if (!inherited && askedBy == nullptr && name != names.toString && name != names.opEq)
            throw runtimeError(Thrown::IllegalStateException, "'" + name->utf8() + "' in class " + cls->name->utf8() +
                               " is marked override, but there is nothing of that name for it to replace");
    } else if (askedBy != nullptr) {
        throw runtimeError(Thrown::IllegalStateException, "'" + name->utf8() + "' in class " + cls->name->utf8() + " is what " +
                           (askedBy->isInterface ? "interface " : "abstract class ") + askedBy->name->utf8() +
                           " asks for; write override in front of it");
    } else if (inherited) {
        throw runtimeError(Thrown::IllegalStateException, "'" + name->utf8() + "' in class " + cls->name->utf8() +
                           " takes the place of the one it inherits; write override in front of it");
    }
    DONE();
}
}
CASE(Require) {
    {
    ObjString* name = asString(consts[READ_U16()]);
    if (!sp_[-1].isObjType(ObjType::Class)) throw runtimeError(Thrown::InternalError, "invalid class declaration");
    asClass(sp_[-1])->required.emplace_back(name, asClass(sp_[-1]));
    DONE();
}
}
CASE(ClassNumber) {
    {
    const int number = READ_U16();
    const int library = READ_U16();
    if (!sp_[-1].isObjType(ObjType::Class)) throw runtimeError(Thrown::InternalError, "invalid class declaration");
    asClass(sp_[-1])->number = number;
    asClass(sp_[-1])->library = library;
    DONE();
}
}
CASE(MemberAccess) {
    {
    ObjString* name = asString(consts[READ_U16()]);
    const int id = READ_U16();
    const int flags = READ_U8();
    const int kind = flags & 0xF;
    const bool write = (flags & kAccessOfSetter) != 0;
    if (!sp_[-1].isObjType(ObjType::Class) || kind > kAccessAsAbove) throw runtimeError(Thrown::InternalError, "invalid class declaration");
    ObjClass* cls = asClass(sp_[-1]);
    const Value* before = cls->accessMarks.find(name);
    const bool restricts = kind == kAccessPrivate || kind == kAccessProtected || kind == kAccessInternal;
    if (restricts && !write && (before == nullptr || halfCount(halfOf(*before, false)) == 0)) {
        // Restricted here and not above: then there must be nothing above
        // for it to replace, since what is open stays open.
        auto has = [&](const ObjClass* from) {
            return from->methods.find(name) != nullptr || from->declaredIndex.find(name) != nullptr ||
                   from->getters.find(name) != nullptr || from->setters.find(name) != nullptr;
        };
        const ObjClass* open = cls->super != nullptr && has(cls->super) ? cls->super : nullptr;
        for (const ObjClass* implemented : cls->interfaces)
            if (open == nullptr && has(implemented)) open = implemented;
        for (const auto& [wanted, by] : cls->required)
            if (open == nullptr && wanted == name && by != cls) open = by;
        if (open != nullptr)
            throw runtimeError(Thrown::IllegalStateException, "'" + name->utf8() + "' in class " + cls->name->utf8() + " cannot be " +
                               (kind == kAccessPrivate ? "private" : kind == kAccessProtected ? "protected" : "internal") +
                               ": it is open in " + (open->isInterface ? "interface " : "class ") + open->name->utf8());
    }
    declareAccess(cls, cls->accessMarks, name, id, kind, write);
    DONE();
}
}
CASE(StaticAccess) {
    {
    ObjString* name = asString(consts[READ_U16()]);
    const int id = READ_U16();
    const int flags = READ_U8();
    const int kind = flags & 0xF;
    if (!sp_[-1].isObjType(ObjType::Class) || id == 0 || kind < kAccessPrivate || kind > kAccessInternal)
        throw runtimeError(Thrown::InternalError, "invalid class declaration");
    declareAccess(asClass(sp_[-1]), asClass(sp_[-1])->staticAccessMarks, name, id, kind, (flags & kAccessOfSetter) != 0);
    DONE();
}
}
CASE(FinishClass) {
    {
    if (!sp_[-1].isObjType(ObjType::Class)) throw runtimeError(Thrown::InternalError, "invalid class declaration");
    ObjClass* cls = asClass(sp_[-1]);
    // Whether the class's own code may use every member its objects have:
    // then nothing has to be looked up for it (see CHECK_ACCESS).
    classFinished();
    cls->accessAllOwn = cls->accessSharedOwn = true;
    for (uint32_t at = 0; at < cls->accessMarks.capacity(); at++) {
        const Table::Entry& entry = cls->accessMarks.entries()[at];
        if (entry.key == nullptr) continue;
        for (bool write : {false, true}) {
            const uint32_t half = halfOf(entry.value, write);
            const uint16_t* run = halfRun(cls, half);
            bool own = halfCount(half) == 0;
            for (uint32_t i = 0; i < halfCount(half); i++) own = own || run[i] == cls->number || run[i] == cls->library;
            cls->accessAllOwn = cls->accessAllOwn && own;
            if (halfKind(half) != kAccessPrivate) cls->accessSharedOwn = cls->accessSharedOwn && own;
        }
    }
    if (cls->isInterface || cls->isAbstract) DONE();  // left for the classes below
    for (const auto& [name, interface] : cls->required) {
        if (cls->methods.find(name) != nullptr || cls->declaredIndex.find(name) != nullptr ||
            cls->getters.find(name) != nullptr)
            continue;
        throw runtimeError(Thrown::IllegalStateException, "class " + cls->name->utf8() + " must have '" + name->utf8() + "', which " +
                           (interface->isInterface ? "interface " : "abstract class ") + interface->name->utf8() +
                           " requires");
    }
    cls->required.clear();
    seeHeaderInit(cls);  // objects made with no frame for their initializer (objects plan part 4)
    DONE();
}
}
CASES(Getter, Setter) {
    {
    bool isGetter = opAt(ip - kOpBytes) == Op::Getter;
    ObjString* name = asString(consts[READ_U16()]);
    Value accessor = sp_[-1], cls = sp_[-2];
    if (!cls.isObjType(ObjType::Class) || !accessor.isObjType(ObjType::Closure) ||
        asClosure(accessor)->proto->arity != (isGetter ? 0 : 1))
        throw runtimeError(Thrown::InternalError, "invalid class declaration");
    (isGetter ? asClass(cls)->getters : asClass(cls)->setters).set(name, accessor);
    asClass(cls)->hasAccessors = true;
    sp_--;
    DONE();
}
}
CASE(PendingEnum) {
    *sp_++ = Value::enumName(asString(consts[READ_U16()]));
    pendingEnums_++;
    DONE();
}
CASE(EnumNext) {
    {
    ObjString* name = asString(consts[READ_U16()]);
    const int ordinal = READ_U16();
    if (!sp_[-1].isObjType(ObjType::Class)) throw runtimeError(Thrown::InternalError, "invalid enum declaration");
    declareEntryFields(asClass(sp_[-1]));
    asClass(sp_[-1])->nextEntry = name;
    asClass(sp_[-1])->nextOrdinal = ordinal;
    DONE();
}
}
CASES(EnumEntry, EnumSeal) {
    {
    const bool isEntry = opAt(ip - kOpBytes) == Op::EnumEntry;
    ObjString* name = isEntry ? asString(consts[READ_U16()]) : nullptr;
    const int ordinal = isEntry ? READ_U16() : 0;
    Value target = sp_[isEntry ? -2 : -1];
    if (!target.isObjType(ObjType::Class)) throw runtimeError(Thrown::InternalError, "invalid enum declaration");
    ObjClass* cls = asClass(target);
    cls->isEnum = true;

    // A static val, so that neither an entry nor the list can be replaced.
    auto declare = [&](ObjString* member, Value value) {
        FieldDecl field;
        field.name = member;
        field.isVal = true;
        cls->staticIndex.set(member, Value::integer(static_cast<int64_t>(cls->staticDeclared.size())));
        cls->staticDeclared.push_back(field);
        cls->statics.set(member, value);
    };
    // The list is kept under a name of its own, and is what `entries` gives
    // unless the enum has a property of that name in its place.
    const Value* list = cls->statics.find(names.enumEntries);
    if (list == nullptr) {
        Value fresh = Value::object(alloc<ObjList>());
        cls->statics.set(names.enumEntries, fresh);
        if (cls->staticGetters.find(names.entries) == nullptr) declare(names.entries, fresh);
        list = cls->statics.find(names.enumEntries);
    }
    if (!isEntry) {
        cls->enumSealed = true;
        DONE();
    }

    Value entry = sp_[-1];
    if (!entry.isObjType(ObjType::Instance) || asInstance(entry)->cls != cls || !list->isObjType(ObjType::List))
        throw runtimeError(Thrown::InternalError, "invalid enum declaration");
    nameEntry(asInstance(entry), name, ordinal);
    listAdd(asList(*list), entry);
    declare(name, entry);
    sp_--;
    DONE();
}
}
CASES(StaticGetter, StaticSetter) {
    {
    bool isGetter = opAt(ip - kOpBytes) == Op::StaticGetter;
    ObjString* name = asString(consts[READ_U16()]);
    Value accessor = sp_[-1], cls = sp_[-2];
    if (!cls.isObjType(ObjType::Class) || !accessor.isObjType(ObjType::Closure) ||
        asClosure(accessor)->proto->arity != (isGetter ? 0 : 1))
        throw runtimeError(Thrown::InternalError, "invalid class declaration");
    (isGetter ? asClass(cls)->staticGetters : asClass(cls)->staticSetters).set(name, accessor);
    sp_--;
    DONE();
}
}
CASE(StaticField) {
    {
    ObjString* name = asString(consts[READ_U16()]);
    int flags = READ_U16();
    FieldDecl field;
    field.name = name;
    field.type = static_cast<uint8_t>(flags & 0xFF);
    field.isVal = (flags & kFieldVal) != 0;
    field.nested = (flags & kFieldNested) != 0;
    const bool classTyped = (field.type & kTypeMask) == kTypeInstance;
    Value value = sp_[-1];
    Value target = sp_[classTyped ? -3 : -2];
    if (classTyped) {
        if (!sp_[-2].isObjType(ObjType::Closure)) throw runtimeError(Thrown::InternalError, "invalid class declaration");
        field.typeThunk = asClosure(sp_[-2]);
    }
    if (!target.isObjType(ObjType::Class)) throw runtimeError(Thrown::InternalError, "invalid class declaration");
    ObjClass* cls = asClass(target);
    SAVE_IP();
    checkStaticType(cls, field, value);
    cls->staticIndex.set(name, Value::integer(static_cast<int64_t>(cls->staticDeclared.size())));
    cls->staticDeclared.push_back(field);
    cls->statics.set(name, value);
    sp_ -= classTyped ? 2 : 1;
    DONE();
}
}
CASE(DeclareField) {
    {
    ObjString* name = asString(consts[READ_U16()]);
    int flags = READ_U16();
    FieldDecl field;
    field.name = name;
    field.type = static_cast<uint8_t>(flags & 0xFF);
    field.isVal = (flags & kFieldVal) != 0;
    if ((field.type & kTypeMask) == kTypeInstance) {
        Value thunk = *--sp_;
        if (!thunk.isObjType(ObjType::Closure)) throw runtimeError(Thrown::InternalError, "invalid class declaration");
        field.typeThunk = asClosure(thunk);
    }
    if (!sp_[-1].isObjType(ObjType::Class)) throw runtimeError(Thrown::InternalError, "invalid class declaration");
    ObjClass* cls = asClass(sp_[-1]);
    if (const Value* inherited = cls->declaredIndex.find(name)) {
        if ((flags & kFieldOverride) == 0)
            throw runtimeError(Thrown::IllegalStateException, "field '" + name->utf8() + "' of class " + cls->name->utf8() +
                               " is already declared by a superclass; write override to declare it again");
        field.overrides = true;
        {
            // Kept where the field it stands in for was (M6.6 part 6).
            const FieldDecl& was = cls->declared[static_cast<size_t>(inherited->as.i)];
            field.slot = was.slot;
            field.payload = was.payload;
            field.payloadOffset = was.payloadOffset;
            field.payloadBit = was.payloadBit;
            field.inlineOffset = was.inlineOffset;
        }
        cls->declared[static_cast<size_t>(inherited->as.i)] = field;
        cls->shape = newShape();
        DONE();
    }
    if (cls->hasAccessors) dropInterfaceAccessors(cls, name);
    cls->declaredIndex.set(name, Value::integer(static_cast<int64_t>(cls->declared.size())));
    cls->declared.push_back(field);
    fieldAdded(cls, cls->declared.size() - 1);
    cls->shape = newShape();
    if (flags & kFieldRequired) {
        cls->ownRequired.push_back(name);
        cls->anyRequired = true;
    }
    DONE();
}
}
CASE(FieldInit) {
    {
    Value closure = sp_[-1], cls = sp_[-2];
    if (!cls.isObjType(ObjType::Class) || !closure.isObjType(ObjType::Closure) ||
        asClosure(closure)->proto->arity != 0)
        throw runtimeError(Thrown::InternalError, "invalid class declaration");
    asClass(cls)->fieldInits.push_back(asClosure(closure));
    sp_--;
    DONE();
}
}
CASES(Method, StaticMethod) {
    {
    bool isStatic = opAt(ip - kOpBytes) == Op::StaticMethod;
    ObjString* name = asString(consts[READ_U16()]);
    Value method = sp_[-1], cls = sp_[-2];
    if (!cls.isObjType(ObjType::Class) || !method.isObjType(ObjType::Closure))
        throw runtimeError(Thrown::InternalError, "invalid class declaration");
    if (isStatic) {
        asClass(cls)->statics.set(name, method);
    } else {
        asClass(cls)->methods.set(name, method);
        if (name == names.init) asClass(cls)->init = asClosure(method);
    }
    sp_--;
    DONE();
}
}

CASE(BuildList) {
    {
    int count = READ_U16();
    ObjList* list = alloc<ObjList>();
    ownAll(sp_ - count, sp_);
    list->values().assign(sp_ - count, sp_);
    account(static_cast<size_t>(count) * sizeof(Value));
    sp_ -= count;
    *sp_++ = Value::object(list);
    DONE();
}
}
CASE(TagCollection) {
    {
    // A list or map literal of a type that is made once (M6.6 part 1): given
    // it, and on past the code that makes it, the first time only.
    const uint64_t kept = caches != nullptr ? caches[FAST_U16(0)].load(std::memory_order_acquire) : 0;
    if (kept == 0) {
        ip += 6;
        DONE();
    }
    ObjTypeInfo* const type = reinterpret_cast<ObjTypeInfo*>(static_cast<uintptr_t>(kept));
    const Value made = sp_[-1];
    if (made.isObjType(ObjType::List)) {
        ObjList* const list = asList(made);
        if (list->type == nullptr && type->packs.load(std::memory_order_relaxed) == 0) list->type = type;
        else typeList(list, type);
    }
    else if (made.isObjType(ObjType::Map) && type->id == kTypeMap && asMap(made)->type == nullptr) asMap(made)->type = type;
    else if (made.isObjType(ObjType::Set) && type->id == kTypeSet && asSet(made)->type == nullptr) asSet(made)->type = type;
    ip = code + FAST_I32(2);
    DONE();
}
}
CASE(BuildMap) {
    {
    int count = READ_U16();
    ObjMap* map = alloc<ObjMap>();
    ownAll(sp_ - 2 * count, sp_);
    Value* entry = sp_ - 2 * count;
    for (int i = 0; i < count; i++, entry += 2) map->map.set(entry[0], entry[1]);
    account(static_cast<size_t>(count) * 64);
    sp_ -= 2 * count;
    *sp_++ = Value::object(map);
    DONE();
}
}
CASE(BuildString) {
    // Its own values gone before it goes on (a call as the last step leaves none behind).
    {
        {
        int count = READ_U16();
        SAVE_IP();
        // At the width of the widest part (D20), room made for all of it at once.
        Characters text;
        size_t characters = 0;
        for (int i = 0; i < count; i++) {
            const Value part = sp_[i - count];
            if (part.isString()) {
                characters += asString(part)->length;
                text.width = std::max(text.width, asString(part)->width);
            } else {
                characters += 20;
            }
        }
        text.units.reserve(characters * text.width);
        std::string shown;
        for (int i = 0; i < count; i++) {
            Value part = sp_[i - count];
            if (part.isString()) {
                appendText(text, asString(part));
            } else {
                shown.clear();
                stringifyInto(shown, part, false, 0);
                text.appendUtf8Text(shown);
            }
        }
        sp_ -= count;
        *sp_++ = str(std::move(text));
    }
    DONE();
}
}
CASE(Range) {
    {
    const int flags = READ_U8();
    int64_t step = 1;
    if (flags & 4) {
        Value given = *--sp_;
        if (!given.isInt() || given.as.i <= 0)
            throw runtimeError(Thrown::IllegalArgumentException, "the step of a range must be a positive int");
        step = given.as.i;
    }
    Value end = sp_[-1], start = sp_[-2];
    // An end may count from the end, `2..^1`, for a slice (D146, D147).
    if (!(start.isInt() || start.isFromEnd()) || !(end.isInt() || end.isFromEnd()))
        throw runtimeError(Thrown::ClassCastException, "range bounds must be ints, got " + typeName(start) + " and " + typeName(end));
    if ((start.isFromEnd() || end.isFromEnd()) && (flags & 6) != 0)
        throw runtimeError(Thrown::IllegalStateException, "a range with an end counted from the end (^) has no step and does not count down");
    // A slice, s[a ..< b]: where the next instruction takes the range as an index of a string or
    // of bytes, and the part lies inside them, the part is made here and the range never is.
    // Anything else (a list, an operator of a class's own, a part outside) goes the usual way.
    if ((flags & 6) == 0 && opAt(ip + kThreaded) == Op::GetIndex) {
        const Value obj = sp_[-3];
        if ((obj.isString() || obj.isObjType(ObjType::Bytes)) && classOf(obj) == nullptr) {
            const int64_t n = obj.isString() ? static_cast<int64_t>(asString(obj)->length) : static_cast<int64_t>(asBytes(obj)->data.size());
            const int64_t first = start.isFromEnd() ? n - start.as.i : start.as.i;
            const int64_t last = (end.isFromEnd() ? n - end.as.i : end.as.i) + ((flags & 1) != 0 ? 1 : 0);
            if (first >= 0 && first <= last && last <= n) {
                guardRead(obj.as.obj);
                Value part;
                if (obj.isString()) {
                    part = str(asString(obj)->slice(static_cast<size_t>(first), static_cast<size_t>(last)));
                } else {
                    const std::vector<uint8_t>& data = asBytes(obj)->data;
                    ObjBytes* made = alloc<ObjBytes>();
                    made->data.assign(data.begin() + first, data.begin() + last);
                    account(made->data.size());
                    part = Value::object(made);
                }
                sp_[-3] = part;
                sp_ -= 2;
                ip += kThreaded + kOpBytes;  // past the GET_INDEX, whose work is done
                DONE();
            }
        }
    }
    sp_[-2] = Value::object(alloc<ObjRange>(start.as.i, end.as.i, (flags & 2) ? -step : step, (flags & 1) != 0,
                                            start.isFromEnd(), end.isFromEnd()));
    sp_--;
    DONE();
}
}
CASE(FromEnd) {
    {
    // `^k`: k from the end; ^1 is the last (D146).
    const Value k = sp_[-1];
    if (!k.isInt()) throw runtimeError(Thrown::ClassCastException, "^ takes an int, not " + typeName(k));
    if (k.as.i < 1)
        throw runtimeError(Thrown::IndexOutOfBoundsException, "^" + std::to_string(k.as.i) + " is no position: ^1 is the last, ^2 the one before it");
    sp_[-1] = Value::fromEnd(k.as.i);
    DONE();
}
}

CASE(IterInit) {
    SAVE_IP();
    sp_[-1] = makeIterator(sp_[-1]);
    DONE();
}
CASE(RangeLoop) {
    {
    const int flags = READ_U8();
    int64_t step = 1;
    if (flags & 4) {
        Value given = *--sp_;
        if (!given.isInt() || given.as.i <= 0)
            throw runtimeError(Thrown::IllegalArgumentException, "the step of a range must be a positive int");
        step = given.as.i;
    }
    Value end = sp_[-1], start = sp_[-2];
    if (!start.isInt() || !end.isInt())
        throw runtimeError(Thrown::ClassCastException, "range bounds must be ints, got " + typeName(start) + " and " + typeName(end));
    if (flags & 2) step = -step;
    sp_[-1] = Value::integer(rangeCount(start.as.i, end.as.i, step, (flags & 1) != 0));
    *sp_++ = Value::integer(step);
    DONE();
}
}
CASE(RangeNext) {
    {
    Value* const counted = base + READ_U16();  // the next value, how many are left, the step
    const int target = READ_I32();
    if (counted[1].as.i > 0) {
        counted[1].as.i--;
        (sp_++)->setInt(counted[0].as.i);  // one store: what reads it whole at once is handed it
        counted[0].as.i = static_cast<int64_t>(static_cast<uint64_t>(counted[0].as.i) + static_cast<uint64_t>(counted[2].as.i));
    } else {
        ip = code + target;
    }
    DONE();
}
}
CASE(DefaultArg) {
    {
    int slot = READ_U16();
    int target = READ_I32();
    if (base[slot].kind != ValueKind::Undefined) ip = code + target;
    DONE();
}
}
CASE(IterNextRef) {
    {
    // As IterNext, but an item of a list of values is the item itself,
    // where it is (M6.6 part 4).
    const Value iterator = base[FAST_U16(0)];
    if (iterator.isObjType(ObjType::Iterator) && asIterator(iterator)->source.isObjType(ObjType::List) &&
        asList(asIterator(iterator)->source)->store() == ListStore::Items) {
        ObjIterator* it = asIterator(iterator);
        ObjList* list = asList(it->source);
        const int target = FAST_I32(2);
        ip += 6;
        if (it->index < list->size()) {
            const size_t i = it->index++;
            if (list->pack->nullable && (list->pack->nulls[i / 64] >> (i % 64) & 1)) *sp_++ = Value::null();
            else *sp_++ = Value::ref(list, static_cast<uint32_t>(i), 0, 0);
        } else {
            ip = code + target;
        }
        DONE();
    }
    JUMP_LABEL(iterNext);
}
}
CASE(IterNext) {
LABEL(iterNext) {
    // The next item of a list or a range.
    if (base[FAST_U16(0)].isObjType(ObjType::Iterator)) {
        ObjIterator* it = asIterator(base[FAST_U16(0)]);
        const Obj* source = it->source.as.obj;
        if (source->type == ObjType::Range) {
            if (it->remaining > 0) {
                it->remaining--;
                (sp++)->setInt(it->nextValue);
                it->nextValue = static_cast<int64_t>(static_cast<uint64_t>(it->nextValue) +
                                                     static_cast<uint64_t>(static_cast<const ObjRange*>(source)->step));
                ip += 6;
            } else {
                ip = code + FAST_I32(2);
            }
            NEXT();
        }
        if (source->type == ObjType::List) {
            const ObjList* list = static_cast<const ObjList*>(source);
            if (!list->packed()) {
                const std::vector<Value>& items = list->values();
                if (it->index < items.size()) {
                    *sp++ = items[it->index++];
                    ip += 6;
                } else {
                    ip = code + FAST_I32(2);
                }
                NEXT();
            }
            // Packed ints, floats, bools or narrow items, null or not (G69): read from
            // the block, but where a parallel section's band says what may be read.
            if (list->pack->store != ListStore::Items && source != bandObject_) {
                if (it->index < list->pack->count) {
                    *sp++ = packedPlain(*list->pack, it->index++);
                    ip += 6;
                } else {
                    ip = code + FAST_I32(2);
                }
                NEXT();
            }
            // A value type's items kept in the value itself (a Color), none null and none with a
            // type of its own (M9.5 B1): each a copy of its bytes, as RGetIndex gives it.
            if (const PackedItems& pack = *list->pack; source != bandObject_ && !pack.nullable && pack.itemType == nullptr && pack.typeSlot == 0 &&
                                                       pack.itemClass->smallId != 0) {
                if (it->index < pack.count) {
                    *sp++ = smallFrom(pack.itemClass, pack.block.data() + it->index * pack.width, pack.itemClass->layout.load(std::memory_order_relaxed)->size);
                    it->index++;
                    ip += 6;
                } else {
                    ip = code + FAST_I32(2);
                }
                NEXT();
            }
        }
    }
    int slot = READ_U16();
    int target = READ_I32();
    Value iterator = base[slot];
    if (!iterator.isObjType(ObjType::Iterator)) throw runtimeError(Thrown::InternalError, "invalid loop state");
    Value item;
    if (iteratorNext(asIterator(iterator), item)) *sp_++ = item;
    else ip = code + target;
    DONE();
}
}

CASE(TryBegin) {
    {
    int target = READ_I32();
    handlers_.push_back(Handler{frameCount_ - 1, sp_, code + target});
    DONE();
}
}
CASE(TryEnd) {
    if (!handlers_.empty() && handlers_.back().frameIndex == frameCount_ - 1) handlers_.pop_back();
    DONE();
}
CASE(Throw) {
    {
    // Only a Throwable is thrown (D35).
    const Value value = sp_[-1];
    const ObjClass* throwable = thrownClasses_[static_cast<size_t>(Thrown::Throwable)];
    if (throwable != nullptr && (classOf(value) == nullptr || !classOf(value)->isSubclassOf(throwable)))
        throw runtimeError(Thrown::ClassCastException, "only a Throwable can be thrown, not a value of type " + typeName(value));
    WwThrow thrown;
    thrown.value = *--sp_;
    throw thrown;
}
}

CASE(Import) {
    {
    size_t index = static_cast<size_t>(READ_U16());
    if (loaded_[index]) {
        *sp_++ = Value::null();
        DONE();
    }
    loaded_[index] = true;
    const FunctionProto* proto = program_->modules[index];
    if (proto == nullptr) throw runtimeError(Thrown::InternalError, "imported module failed to compile");
    ObjClosure* closure = alloc<ObjClosure>(proto, proto->upvalues.size());
    *sp_++ = Value::object(closure);
    SAVE_IP();
    callClosure(closure, 0);
    LOAD_FRAME();
    DONE();
}
}

// ---- The code the VM runs (M6.7, registers.cpp) ----
CASE(MoveS) {
    {
    base[ip[0]] = base[ip[1]];
    ip += 2;
    NEXT();
}
}
CASE(MoveK) {
    {
    base[ip[0]] = consts[ip[1]];
    ip += 2;
    NEXT();
}
}
CASE(SetTop) {
    {
    sp = base + ip[0];
    ip += 1;
    NEXT();
}
}
CASE(Translate) {
    {
    // A function's first call (M9/M10): its run code made now from its stack code, rather than
    // as the program was read, and run from its start, where this instruction stood.
    FunctionProto* const proto = const_cast<FunctionProto*>(frame->closure->proto);
    translateNow(*proto);
    frame->ip = proto->run.data();  // the code this instruction stood in is gone
    // One read unverified had its stack's size found just now: room made for it, as a call does.
    if (stackEnd_ - frame->base < proto->maxStack && !growContext(frame->base + proto->maxStack, frameCount_))
        throw runtimeError(Thrown::StackOverflowError, "stack overflow");
    LOAD_FRAME();
    NEXT();
}
}
R_ARITHMETIC(RAddSS, R_S, R_S, +, addOverflows, plainAdd)
R_ARITHMETIC(RAddSK, R_S, R_K, +, addOverflows, plainAdd)
R_ARITHMETIC(RAddKS, R_K, R_S, +, addOverflows, plainAdd)
R_ARITHMETIC(RSubSS, R_S, R_S, -, subOverflows, plainSub)
R_ARITHMETIC(RSubSK, R_S, R_K, -, subOverflows, plainSub)
R_ARITHMETIC(RMulSS, R_S, R_S, *, mulOverflows, plainMul)
R_ARITHMETIC(RMulSK, R_S, R_K, *, mulOverflows, plainMul)
R_ARITHMETIC(RMulKS, R_K, R_S, *, mulOverflows, plainMul)
R_DIVIDE(RDivSS, R_S, R_S, x / y, x / y, plainDiv)
R_DIVIDE(RDivSK, R_S, R_K, x / y, x / y, plainDiv)
R_DIVIDE(RModSS, R_S, R_S, std::fmod(x, y), x % y, plainMod)
R_DIVIDE(RModSK, R_S, R_K, std::fmod(x, y), x % y, plainMod)
R_ARITHMETIC(RSubKS, R_K, R_S, -, subOverflows, plainSub)
R_INTS(RShlSK, R_S, R_K, c.as.i >= 0 && c.as.i < 64, static_cast<int64_t>(static_cast<uint64_t>(x) << y))
R_INTS(RShrSK, R_S, R_K, c.as.i >= 0 && c.as.i < 64, x >> y)
R_INTS(RBitAndSS, R_S, R_S, true, x & y)
R_INTS(RBitAndSK, R_S, R_K, true, x & y)
R_INTS(RBitOrSS, R_S, R_S, true, x | y)
R_INTS(RBitOrSK, R_S, R_K, true, x | y)
R_INTS(RShlSS, R_S, R_S, c.as.i >= 0 && c.as.i < 64, static_cast<int64_t>(static_cast<uint64_t>(x) << y))
R_INTS(RShrSS, R_S, R_S, c.as.i >= 0 && c.as.i < 64, x >> y)
R_INTS(RBitXorSS, R_S, R_S, true, x ^ y)
R_INTS(RBitXorSK, R_S, R_K, true, x ^ y)
R_INTS(RUshrSK, R_S, R_K, c.as.i >= 0 && c.as.i < 64, static_cast<int64_t>(static_cast<uint64_t>(x) >> y))
R_INTS(RUshrSS, R_S, R_S, c.as.i >= 0 && c.as.i < 64, static_cast<int64_t>(static_cast<uint64_t>(x) >> y))
R_INTS(RAddWrapSS, R_S, R_S, b.isInt() && c.isInt(), static_cast<int64_t>(static_cast<uint64_t>(x) + static_cast<uint64_t>(y)))
R_INTS(RAddWrapSK, R_S, R_K, b.isInt() && c.isInt(), static_cast<int64_t>(static_cast<uint64_t>(x) + static_cast<uint64_t>(y)))
R_INTS(RSubWrapSS, R_S, R_S, b.isInt() && c.isInt(), static_cast<int64_t>(static_cast<uint64_t>(x) - static_cast<uint64_t>(y)))
R_INTS(RSubWrapSK, R_S, R_K, b.isInt() && c.isInt(), static_cast<int64_t>(static_cast<uint64_t>(x) - static_cast<uint64_t>(y)))
R_INTS(RMulWrapSS, R_S, R_S, b.isInt() && c.isInt(), static_cast<int64_t>(static_cast<uint64_t>(x) * static_cast<uint64_t>(y)))
R_INTS(RMulWrapSK, R_S, R_K, b.isInt() && c.isInt(), static_cast<int64_t>(static_cast<uint64_t>(x) * static_cast<uint64_t>(y)))
R_DIVIDE(RDivKS, R_K, R_S, x / y, x / y, plainDiv)
HAS_COLD(RSetPropSS)
CASES(RSetPropSS, RSetPropSK) {
    // A field of an object found before, as SetProp's own part, here; anything else in the rare
    // part, below. u8 A, u8 B, u8 C, u8 T, u8 S, u16 cache, i32 slow.
    {
        const Value& obj = R_S(1);
        const Value& given = opAt(ip - kOpBytes) == Op::RSetPropSK ? R_K(2) : R_S(2);
        if (obj.isObjType(ObjType::Instance) && caches != nullptr) {
            ObjInstance* instance = asInstance(obj);
            const uint64_t seen = caches[FAST_U16(5)].load(std::memory_order_relaxed);
            if (static_cast<uint32_t>(seen >> 32) == instance->shape && instance->core == coreId_) {
                Value* const field = reinterpret_cast<Value*>(reinterpret_cast<uint8_t*>(instance) + ((seen >> 16) & 0xFFFF));
                if ((seen & 0x80) != 0) {
                    const uint32_t bit = (seen >> 8) & 0xFF;
                    const ValueKind kind = static_cast<ValueKind>(seen & 3);
                    uint8_t* const data = instance->payloadData();
                    const bool hasOne = (seen & 0x20) != 0 && (data[bit / 8] >> (bit % 8) & 1) != 0;
                    if (given.kind == kind && ((seen & 0x40) == 0 || !hasOne)) {
                        const int64_t bits = kind == ValueKind::Bool ? (given.as.b ? 1 : 0) : given.as.i;
                        std::memcpy(static_cast<void*>(field), &bits, 8);  // its payload alone
                        if ((seen & 0x20) != 0) data[bit / 8] |= static_cast<uint8_t>(1u << (bit % 8));
                        std::memcpy(static_cast<void*>(&base[ip[0]]), &given, sizeof(Value));
                        sp = base + ip[3];
                        ip += 11;
                        NEXT();
                    }
                } else if ((seen & 2) == 0 && ((seen & 1) == 0 || field->kind == ValueKind::Undefined)) {
                    const uint8_t type = static_cast<uint8_t>(seen >> 8);
                    if (type == 0 || (type == kTypeFloat && given.isFloat()) || (type == kTypeInt && given.isInt())) {
                        std::memcpy(static_cast<void*>(field), &given, sizeof(Value));
                        std::memcpy(static_cast<void*>(&base[ip[0]]), &given, sizeof(Value));
                        sp = base + ip[3];
                        ip += 11;
                        NEXT();
                    }
                }
            }
        } else if (obj.isRef() && obj.refDepth == 0 && caches != nullptr) {
            // A var field of a value where it is kept (M6.6 part 4), an item of a list say: an
            // int, a float or a bool field given its own kind, here too, as a method changing
            // the items of a list writes them (G9).
            const ObjClass* cls = refHolderClass(obj);
            const uint64_t seen = caches[FAST_U16(5)].load(std::memory_order_relaxed);
            if (cls != nullptr && static_cast<uint32_t>(seen >> 32) == cls->shape) {
                const ValueField& field = cls->layout.load(std::memory_order_relaxed)->fields[(seen >> 16) & 0xFFFF];
                uint8_t* const to = (field.kind == ValueField::Float ? given.isFloat()
                                     : field.kind == ValueField::Int ? given.isInt()
                                                                     : field.kind == ValueField::Bool && given.isBool())
                                        ? refQuickly(obj, true)
                                        : nullptr;
                if (to != nullptr) {
                    if (field.kind == ValueField::Bool) to[field.offset] = given.as.b ? 1 : 0;
                    else std::memcpy(to + field.offset, &given.as, 8);
                    base[ip[0]] = given;
                    sp = base + ip[3];
                    ip += 11;
                    NEXT();
                }
            }
        }
    }
    GO_COLD(RSetPropSS);
}
COLD(RSetPropSS) {
    {
    // SetProp's quick ways on an object of a class, as GetProp's: a field found
    // before, kept as its payload or as a value of a type quickly told. u8 A,
    // u8 B, u8 C, u8 T, u8 S, u16 cache, i32 slow.
    const Value& obj = R_S(1);
    const Value& given = opAt(ip - kOpBytes) == Op::RSetPropSK ? R_K(2) : R_S(2);
    if (caches != nullptr && narrowWrite(obj, caches[FAST_U16(5)].load(std::memory_order_relaxed), given)) {  // G20
        base[ip[0]] = given;
        sp = base + ip[3];
        ip += 11;
        NEXT();
    }
    if (obj.isObjType(ObjType::Instance) && caches != nullptr) {
        ObjInstance* instance = asInstance(obj);
        const uint64_t seen = caches[FAST_U16(5)].load(std::memory_order_relaxed);
        if (static_cast<uint32_t>(seen >> 32) == instance->shape && instance->core == coreId_) {
            Value* const field = reinterpret_cast<Value*>(reinterpret_cast<uint8_t*>(instance) + ((seen >> 16) & 0xFFFF));
            if ((seen & 0x80) != 0) {
                const uint32_t bit = (seen >> 8) & 0xFF;
                const ValueKind kind = static_cast<ValueKind>(seen & 3);
                uint8_t* const data = instance->payloadData();
                const bool hasOne = (seen & 0x20) != 0 && (data[bit / 8] >> (bit % 8) & 1) != 0;
                if (given.kind == kind && ((seen & 0x40) == 0 || !hasOne)) {
                    const int64_t bits = kind == ValueKind::Bool ? (given.as.b ? 1 : 0) : given.as.i;
                    std::memcpy(static_cast<void*>(field), &bits, 8);  // its payload alone
                    if ((seen & 0x20) != 0) data[bit / 8] |= static_cast<uint8_t>(1u << (bit % 8));
                    base[ip[0]] = given;
                    sp = base + ip[3];
                    ip += 11;
                    NEXT();
                }
            } else if ((seen & 2) != 0) {
                // A value kept inside the object, given another of its type.
                if (field->isRef() && field->as.obj == instance) {
                    const ObjClass::InlineField* inside = instance->cls->inlineFieldAt(field->refOffset);
                    const uint8_t* from = given.isObjType(ObjType::Value) && asValue(given)->cls == inside->cls ? asValue(given)->data()
                                          : given.isRef() && refClass(given) == inside->cls           ? refQuickly(given, false)
                                                                                                      : nullptr;
                    if (from != nullptr) {
                        std::memmove(instance->inlineData() + inside->offset, from, inside->size);
                        base[ip[0]] = given;
                        sp = base + ip[3];
                        ip += 11;
                        NEXT();
                    }
                }
            } else if (((seen & 1) == 0 || field->kind == ValueKind::Undefined)) {
                Value value = given;  // a byte may be widened
                const uint8_t type = static_cast<uint8_t>(seen >> 8);
                if (type == 0 || (type == kTypeFloat ? value.isFloat()
                                  : type == kTypeInt ? value.isInt()
                                                     : (matchesType(value, type, nullptr) || widenedByte(value, type)))) {
                    *field = value;
                    base[ip[0]] = value;
                    sp = base + ip[3];
                    ip += 11;
                    NEXT();
                }
            }
        }
    }
    if (obj.isObjType(ObjType::Value) && caches != nullptr) {
        // A field of a value being made, in its box, as SetProp writes one.
        ObjValue* const box = asValue(obj);
        const uint64_t seen = caches[FAST_U16(5)].load(std::memory_order_relaxed);
        if (box->valueOpen && static_cast<uint32_t>(seen >> 32) == box->cls->shape) {
            const ValueField& field = box->cls->layout.load(std::memory_order_relaxed)->fields[(seen >> 16) & 0xFFFF];
            if (field.kind == ValueField::Float ? given.isFloat() : field.kind == ValueField::Int ? given.isInt() : given.isBool()) {
                if (field.kind == ValueField::Bool) box->data()[field.offset] = given.as.b ? 1 : 0;
                else std::memcpy(box->data() + field.offset, &given.as, 8);
                base[ip[0]] = given;
                sp = base + ip[3];
                ip += 11;
                NEXT();
            }
        }
    }
    const Value objHere = obj, givenHere = given;
    Value* const at = base + ip[4];
    at[0] = objHere;
    at[1] = givenHere;
    sp = at + 2;
    ip = code + operandI32(ip + 7);
    NEXT();
}
}
CASE(RGetGlobal) {
    {
    // u8 A, u8 T, u8 S, u16 global, i32 slow: a global not yet worked out (D37)
    // goes the slow way, to GetGlobal.
    if (const Value& global = globals_[static_cast<size_t>(FAST_U16(3))]; global.kind != ValueKind::Undefined) {
        std::memcpy(static_cast<void*>(&base[ip[0]]), &global, sizeof(Value));
        sp = base + ip[1];
        ip += 9;
        NEXT();
    }
    sp = base + ip[2];
    ip = code + operandI32(ip + 5);
    NEXT();
}
}
CASE(RGetUpvalue) {
    {
    base[ip[0]] = *frame->closure->upvalues[static_cast<size_t>(FAST_U16(2))]->location;
    sp = base + ip[1];
    ip += 4;
    NEXT();
}
}
CASES(RGetIndexSS, RGetIndexSK) {
    {
    // A list's item by its position, in range, as GetIndex reads it at once.
    // u8 A, u8 B, u8 C, u8 T, u8 S, i32 slow.
    const Value& obj = R_S(1);
    const Value& index = opAt(ip - kOpBytes) == Op::RGetIndexSK ? R_K(2) : R_S(2);
    if (obj.isString() && index.isInt()) {
        base[ip[0]] = getIndex(obj, index);
        sp = base + ip[3];
        ip += 9;
        NEXT();
    }
    if (obj.isObjType(ObjType::Bytes) && index.isInt()) {
        const std::vector<uint8_t>& data = asBytes(obj)->data;
        const uint64_t i = static_cast<uint64_t>(index.as.i);
        if (i < data.size() && obj.as.obj != bandObject_ && (obj.as.obj->core == coreId_ || obj.as.obj->core == 0)) {
            base[ip[0]] = Value::byte(data[i]);
            sp = base + ip[3];
            ip += 9;
            NEXT();
        }
    }
    if (obj.isObjType(ObjType::List) && index.isInt()) {
        const ObjList* list = asList(obj);
        const uint64_t i = static_cast<uint64_t>(index.as.i);
        if (i < list->size() && obj.as.obj != bandObject_ && (obj.as.obj->core == coreId_ || obj.as.obj->core == 0)) {
            if (!list->packed()) {
                base[ip[0]] = list->values()[i];
                sp = base + ip[3];
                ip += 9;
                NEXT();
            }
            if (const PackedItems& pack = *list->pack; !pack.nullable && (pack.store == ListStore::Ints || pack.store == ListStore::Floats)) {
                uint64_t bits;
                std::memcpy(&bits, pack.block.data() + i * pack.width, 8);
                base[ip[0]].setWhole(static_cast<uint64_t>(pack.store == ListStore::Ints ? ValueKind::Int : ValueKind::Float), bits);
                sp = base + ip[3];
                ip += 9;
                NEXT();
            }
            if (list->pack->store != ListStore::Items) {
                base[ip[0]] = packedPlain(*list->pack, i);
                sp = base + ip[3];
                ip += 9;
                NEXT();
            }
            if (const PackedItems& pack = *list->pack; !pack.nullable && pack.itemType == nullptr && pack.typeSlot == 0 && pack.itemClass->smallId != 0) {
                // A value kept in the value itself (a Color): its bytes, read at once.
                base[ip[0]] = smallFrom(pack.itemClass, pack.block.data() + i * pack.width, pack.itemClass->layout.load(std::memory_order_relaxed)->size);
                sp = base + ip[3];
                ip += 9;
                NEXT();
            }
        }
    }
    const Value objHere = obj, indexHere = index;
    Value* const at = base + ip[4];
    at[0] = objHere;
    at[1] = indexHere;
    sp = sp_ = at + 2;
    if (ip[0] == ip[4]) {
        ip += 9;
        JUMP_LABEL(getIndex);
    }
    ip = code + operandI32(ip + 5);
    NEXT();
}
}
CASE(RNeg) {
    {
    // u8 A, u8 B, u8 T, u8 S, i32 slow.
    const Value& b = R_S(1);
    if (b.isFloat()) {
        base[ip[0]].setFloat(-b.as.d);
        sp = base + ip[2];
        ip += 8;
        NEXT();
    }
    if (b.isInt() && b.as.i != INT64_MIN) {
        base[ip[0]].setInt(-b.as.i);
        sp = base + ip[2];
        ip += 8;
        NEXT();
    }
    base[ip[3]] = Value(b);
    sp = sp_ = base + ip[3] + 1;
    if (ip[0] == ip[3]) {
        ip += 8;
        JUMP_LABEL(plainNeg);
    }
    ip = code + operandI32(ip + 4);
    NEXT();
}
}
CASE(RNot) {
    {
    const Value& b = R_S(1);
    if (b.isBool()) {
        base[ip[0]].setBool(!b.as.b);
        sp = base + ip[2];
        ip += 8;
        NEXT();
    }
    base[ip[3]] = Value(b);
    sp = sp_ = base + ip[3] + 1;
    if (ip[0] == ip[3]) {
        ip += 8;
        JUMP_LABEL(plainNot);
    }
    ip = code + operandI32(ip + 4);
    NEXT();
}
}
CASE(RJumpIfFalse) {
    {
    // u8 B, u8 T, i32 target, i32 slow: the stack would have had B at T.
    const Value& b = R_S(0);
    if (b.isBool()) {
        sp = base + ip[1];
        if (b.as.b) ip += 10;
        else ip = code + operandI32(ip + 2);
        NEXT();
    }
    base[ip[1]] = Value(b);
    sp = base + ip[1] + 1;
    ip = code + operandI32(ip + 6);
    NEXT();
}
}
R_COMPARISON(RLtSS, R_S, R_S, <, plainLt, false)
R_JUMP(RJumpLtSS, R_S, R_S, <, false)
R_COMPARISON(RLtSK, R_S, R_K, <, plainLt, false)
R_JUMP(RJumpLtSK, R_S, R_K, <, false)
R_COMPARISON(RLeSS, R_S, R_S, <=, plainLe, false)
R_JUMP(RJumpLeSS, R_S, R_S, <=, false)
R_COMPARISON(RLeSK, R_S, R_K, <=, plainLe, false)
R_JUMP(RJumpLeSK, R_S, R_K, <=, false)
R_COMPARISON(RGtSS, R_S, R_S, >, plainGt, false)
R_JUMP(RJumpGtSS, R_S, R_S, >, false)
R_COMPARISON(RGtSK, R_S, R_K, >, plainGt, false)
R_JUMP(RJumpGtSK, R_S, R_K, >, false)
R_COMPARISON(RGeSS, R_S, R_S, >=, plainGe, false)
R_JUMP(RJumpGeSS, R_S, R_S, >=, false)
R_COMPARISON(RGeSK, R_S, R_K, >=, plainGe, false)
R_JUMP(RJumpGeSK, R_S, R_K, >=, false)
R_COMPARISON(REqSS, R_S, R_S, ==, plainEq, true)
R_JUMP(RJumpEqSS, R_S, R_S, ==, true)
R_COMPARISON(REqSK, R_S, R_K, ==, plainEq, true)
R_JUMP(RJumpEqSK, R_S, R_K, ==, true)
// On operands the checker knows (M6.7 step 5): nothing of them tested.
R_INT_ARITHMETIC(RAddISS, R_S, R_S, +, addOverflows, plainAdd)
R_INT_ARITHMETIC(RAddISK, R_S, R_K, +, addOverflows, plainAdd)
R_INT_ARITHMETIC(RAddIKS, R_K, R_S, +, addOverflows, plainAdd)
R_INT_ARITHMETIC(RSubISS, R_S, R_S, -, subOverflows, plainSub)
R_INT_ARITHMETIC(RSubISK, R_S, R_K, -, subOverflows, plainSub)
R_INT_ARITHMETIC(RSubIKS, R_K, R_S, -, subOverflows, plainSub)
R_INT_ARITHMETIC(RMulISS, R_S, R_S, *, mulOverflows, plainMul)
R_INT_ARITHMETIC(RMulISK, R_S, R_K, *, mulOverflows, plainMul)
R_INT_ARITHMETIC(RMulIKS, R_K, R_S, *, mulOverflows, plainMul)
R_FLOAT_ARITHMETIC(RAddFSS, R_S, R_S, +)
R_FLOAT_ARITHMETIC(RAddFSK, R_S, R_K, +)
R_FLOAT_ARITHMETIC(RAddFKS, R_K, R_S, +)
R_FLOAT_ARITHMETIC(RSubFSS, R_S, R_S, -)
R_FLOAT_ARITHMETIC(RSubFSK, R_S, R_K, -)
R_FLOAT_ARITHMETIC(RSubFKS, R_K, R_S, -)
R_FLOAT_ARITHMETIC(RMulFSS, R_S, R_S, *)
R_FLOAT_ARITHMETIC(RMulFSK, R_S, R_K, *)
R_FLOAT_ARITHMETIC(RMulFKS, R_K, R_S, *)
R_FLOAT_ARITHMETIC(RDivFSS, R_S, R_S, /)
R_FLOAT_ARITHMETIC(RDivFSK, R_S, R_K, /)
R_FLOAT_ARITHMETIC(RDivFKS, R_K, R_S, /)
R_TYPED_COMPARISON(RLtISS, R_S, R_S, i, <)
R_TYPED_JUMP(RJumpLtISS, R_S, R_S, i, <)
R_TYPED_COMPARISON(RLtISK, R_S, R_K, i, <)
R_TYPED_JUMP(RJumpLtISK, R_S, R_K, i, <)
R_TYPED_COMPARISON(RLeISS, R_S, R_S, i, <=)
R_TYPED_JUMP(RJumpLeISS, R_S, R_S, i, <=)
R_TYPED_COMPARISON(RLeISK, R_S, R_K, i, <=)
R_TYPED_JUMP(RJumpLeISK, R_S, R_K, i, <=)
R_TYPED_COMPARISON(RGtISS, R_S, R_S, i, >)
R_TYPED_JUMP(RJumpGtISS, R_S, R_S, i, >)
R_TYPED_COMPARISON(RGtISK, R_S, R_K, i, >)
R_TYPED_JUMP(RJumpGtISK, R_S, R_K, i, >)
R_TYPED_COMPARISON(RGeISS, R_S, R_S, i, >=)
R_TYPED_JUMP(RJumpGeISS, R_S, R_S, i, >=)
R_TYPED_COMPARISON(RGeISK, R_S, R_K, i, >=)
R_TYPED_JUMP(RJumpGeISK, R_S, R_K, i, >=)
R_TYPED_COMPARISON(REqISS, R_S, R_S, i, ==)
R_TYPED_JUMP(RJumpEqISS, R_S, R_S, i, ==)
R_TYPED_COMPARISON(REqISK, R_S, R_K, i, ==)
R_TYPED_JUMP(RJumpEqISK, R_S, R_K, i, ==)
R_TYPED_COMPARISON(RLtFSS, R_S, R_S, d, <)
R_TYPED_JUMP(RJumpLtFSS, R_S, R_S, d, <)
R_TYPED_COMPARISON(RLtFSK, R_S, R_K, d, <)
R_TYPED_JUMP(RJumpLtFSK, R_S, R_K, d, <)
R_TYPED_COMPARISON(RLeFSS, R_S, R_S, d, <=)
R_TYPED_JUMP(RJumpLeFSS, R_S, R_S, d, <=)
R_TYPED_COMPARISON(RLeFSK, R_S, R_K, d, <=)
R_TYPED_JUMP(RJumpLeFSK, R_S, R_K, d, <=)
R_TYPED_COMPARISON(RGtFSS, R_S, R_S, d, >)
R_TYPED_JUMP(RJumpGtFSS, R_S, R_S, d, >)
R_TYPED_COMPARISON(RGtFSK, R_S, R_K, d, >)
R_TYPED_JUMP(RJumpGtFSK, R_S, R_K, d, >)
R_TYPED_COMPARISON(RGeFSS, R_S, R_S, d, >=)
R_TYPED_JUMP(RJumpGeFSS, R_S, R_S, d, >=)
R_TYPED_COMPARISON(RGeFSK, R_S, R_K, d, >=)
R_TYPED_JUMP(RJumpGeFSK, R_S, R_K, d, >=)
R_TYPED_COMPARISON(REqFSS, R_S, R_S, d, ==)
R_TYPED_JUMP(RJumpEqFSS, R_S, R_S, d, ==)
R_TYPED_COMPARISON(REqFSK, R_S, R_K, d, ==)
R_TYPED_JUMP(RJumpEqFSK, R_S, R_K, d, ==)
HAS_COLD(RGetProp)
CASE(RGetProp) {
    // GetProp's quick ways, on the object in slot B, where this place found the field before, in
    // an object of the same layout (M6.5 part 4). u8 A, u8 B, u8 T, u8 S, u16 cache, i32 slow.
    // An object of a class, and a value where it is kept (a list's item), here; a value in its box,
    // kept in the value itself or held in another, a string's or a list's size, and the slow way,
    // in its rare part.
    const Value& obj = R_S(1);
    if (caches != nullptr && obj.isObjType(ObjType::Instance)) {
        const uint64_t seen = caches[FAST_U16(4)].load(std::memory_order_relaxed);
        const ObjInstance* instance = asInstance(obj);
        if (static_cast<uint32_t>(seen >> 32) == instance->shape && (instance->core == 0 || instance->core == coreId_)) {
            Value& out = base[ip[0]];
            const uint8_t* const at = reinterpret_cast<const uint8_t*>(instance) + ((seen >> 16) & 0xFFFF);
            if ((seen & 0x80) != 0) {
                // A field kept as its payload alone (M6.6 part 6).
                uint64_t bits;
                std::memcpy(&bits, at, 8);
                out.setWhole(seen & 3, bits);
                sp = base + ip[2];
                ip += 10;
                NEXT();
            }
            // Copied whole, in one store: see setWhole.
            if (const ValueKind kind = reinterpret_cast<const Value*>(at)->kind; kind != ValueKind::Ref && kind != ValueKind::Undefined) {
                std::memcpy(static_cast<void*>(&out), at, sizeof(Value));
                sp = base + ip[2];
                ip += 10;
                NEXT();
            }
        }
    } else if (caches != nullptr && obj.isRef() && obj.refDepth == 0) {
        // A list's item, or a value kept inside an object, itself.
        const uint64_t seen = caches[FAST_U16(4)].load(std::memory_order_relaxed);
        const ObjClass* cls = refHolderClass(obj);
        if (cls != nullptr && static_cast<uint32_t>(seen >> 32) == cls->shape) {
            const ValueField& field = cls->layout.load(std::memory_order_relaxed)->fields[(seen >> 16) & 0xFFFF];
            if (const uint8_t* at = refQuickly(obj, false)) {
                if (readPlain(at + field.offset, field, base[ip[0]])) {
                    sp = base + ip[2];
                    ip += 10;
                    NEXT();
                }
            }
        }
    }
    GO_COLD(RGetProp);
}
COLD(RGetProp) {
    const Value& obj = R_S(1);
    if (caches != nullptr) {
        const uint64_t seen = caches[FAST_U16(4)].load(std::memory_order_relaxed);
        Value& out = base[ip[0]];
        // A value kept in the value itself (a Color in a val) first: the commonest here.
        if (obj.isSmall()) {
            const ObjClass* cls = smallClassOf(obj);
            if (static_cast<uint32_t>(seen >> 32) == cls->shape) {
                const ValueField& field = cls->layout.load(std::memory_order_relaxed)->fields[(seen >> 16) & 0xFFFF];
                if (readPlain(smallBytes(obj) + field.offset, field, out)) {
                    sp = base + ip[2];
                    ip += 10;
                    NEXT();
                }
            }
        }
        if (seen == kSeenSize) {
            if (const int64_t size = quickSize(obj); size >= 0) {
                out.setInt(size);
                sp = base + ip[2];
                ip += 10;
                NEXT();
            }
        }
        if (narrowRead(obj, seen, out)) {  // an object's narrow field (G20)
            sp = base + ip[2];
            ip += 10;
            NEXT();
        }
        if (obj.isObjType(ObjType::Value)) {
            const ObjValue* box = asValue(obj);
            if (static_cast<uint32_t>(seen >> 32) == box->cls->shape) {
                const ValueField& field = box->cls->layout.load(std::memory_order_relaxed)->fields[(seen >> 16) & 0xFFFF];
                if (readPlain(box->data() + field.offset, field, out)) {
                    sp = base + ip[2];
                    ip += 10;
                    NEXT();
                }
            }
        } else if (obj.isSmall()) {
            const ObjClass* cls = smallClassOf(obj);
            if (static_cast<uint32_t>(seen >> 32) == cls->shape) {
                const ValueField& field = cls->layout.load(std::memory_order_relaxed)->fields[(seen >> 16) & 0xFFFF];
                if (readPlain(smallBytes(obj) + field.offset, field, out)) {
                    sp = base + ip[2];
                    ip += 10;
                    NEXT();
                }
            }
        } else if (obj.isRef()) {
            const ObjClass* cls = refClassOrNull(obj);
            if (cls != nullptr && static_cast<uint32_t>(seen >> 32) == cls->shape) {
                const ValueField& field = cls->layout.load(std::memory_order_relaxed)->fields[(seen >> 16) & 0xFFFF];
                if (const uint8_t* at = refQuickly(obj, false)) {
                    if (readPlain(at + field.offset, field, out)) {
                        sp = base + ip[2];
                        ip += 10;
                        NEXT();
                    }
                }
            }
        }
    }
    Value* const at = base + ip[3];
    at[0] = Value(obj);
    sp = at + 1;
    ip = code + operandI32(ip + 6);
    NEXT();
}
// RCall and RInvoke begin alike: u8 A, u8 F (the function) or R (the receiver), u8 T, u8 S,
// u8 argc, u8 flags, u8 X1, X2, X3, X4. The function or receiver and the arguments go where
// the stack would have them, from slot S, for every way: the arguments first, the last first,
// then the function or receiver (an argument's slot may be where that goes, never the other
// way). Each is copied whole (Value::copyWhole), and one already where it goes (worked out
// there) is left. Bit k-1 of flags says Xk is a constant.
#define R_CALL_ARGUMENT(k)                                                              \
    if ((ip[5] & (1 << ((k) - 1))) != 0) Value::copyWhole(at + (k), &consts[ip[5 + (k)]]); \
    else if (ip[5 + (k)] != ip[3] + (k)) Value::copyWhole(at + (k), &base[ip[5 + (k)]]);
#define R_CALL_PUT(argc)                                    \
    if ((argc) > 0) {                                       \
        if ((argc) > 1) {                                   \
            if ((argc) > 2) {                               \
                if ((argc) > 3) { R_CALL_ARGUMENT(4) }      \
                R_CALL_ARGUMENT(3)                          \
            }                                               \
            R_CALL_ARGUMENT(2)                              \
        }                                                   \
        R_CALL_ARGUMENT(1)                                  \
    }                                                       \
    if (ip[1] != ip[3]) Value::copyWhole(at, &base[ip[1]]);
// Where a function written in WinterWolf, called by an RCall or RInvoke of `size` bytes, its resume
// place at ip + `resume`, returns to: the instruction after it, where its result is to stay
// where the call leaves it (slot S, A being S); else, into its slow way just after the call,
// which stores it where it goes and comes back.
#define R_CALL_RETURN(size, resume) (ip[0] == ip[3] ? std::ptrdiff_t{size} : (code + operandI32(ip + (resume))) - ip)
HAS_COLD(RCall)
CASE(RCall) {
    {
    // ... i32 slow, i32 resume: `f(x1, x2)`, up to four arguments. A closure that needs nothing
    // done first is entered here; a value type made from its header in the rare part; any other
    // call goes the slow way, the stack's Call.
    const int argc = ip[4];
    Value* const at = base + ip[3];
    R_CALL_PUT(argc)
    sp = at + argc + 1;
    if (at[0].isObjType(ObjType::Closure)) QUICK_CALL(asClosure(at[0]), argc, R_CALL_RETURN(18, 14))
    GO_COLD(RCall);
    }
}
COLD(RCall) {
    {
    // A value type given every field of its header (`Point(x, y)`), its static values worked
    // out and nothing of its type to work out first: made here, as construct makes it, and
    // left where the call leaves its result.
    const int argc = ip[4];
    Value* const at = base + ip[3];
    if (at[0].isObjType(ObjType::Class) && pendingTypeArgs_ == nullptr && pendingEnums_ == 0) {
        ObjClass* const cls = asClass(at[0]);
        if (cls->isValue && cls->madeFromHeader && cls->staticInit == nullptr && cls->fieldTypesMaker == nullptr &&
            static_cast<size_t>(argc) == cls->components.size()) {
            bool given = true;
            for (int k = 1; k <= argc; k++) given = given && at[k].kind != ValueKind::Undefined;
            if (given) {
                SAVE_IP();
                sp_ = at + argc + 1;
                makeFromHeader(cls, argc, std::exchange(pendingDestination_, Value::null()), nullptr);
                sp = sp_;
                ip += R_CALL_RETURN(18, 14);
                NEXT();
            }
        }
    }
    ip = code + operandI32(ip + 10);
    NEXT();
    }
}
HAS_COLD(RInvoke)
CASE(RInvoke) {
    {
    // ... u16 name, u16 cache, i32 slow, i32 resume: `receiver.name(x1, x2)`, up to four
    // arguments; flags 16 for an InvokeOwn. A method of an object's class here; the rest in the
    // rare part, which calls C++ (the call would have this part keep a frame of its own, every way).
    const int argc = ip[4];
    Value* const at = base + ip[3];
    R_CALL_PUT(argc)
    const Value& receiver = at[0];
    sp = at + argc + 1;
    if (receiver.isObjType(ObjType::Instance)) {
        // A method of the object's class, open to this code, entered at once.
        const ObjInstance* instance = asInstance(receiver);
        const ObjString* name = asString(consts[FAST_U16(10)]);
        if (((name->restricted & 1) == 0 || (ip[5] & 16) != 0) && (instance->core == 0 || instance->core == coreId_))
            if (const Value* method = instance->cls->methods.find(name)) QUICK_CALL(asClosure(*method), argc, R_CALL_RETURN(22, 18))
    }
    GO_COLD(RInvoke);
    }
}
COLD(RInvoke) {
    {
    const int argc = ip[4];
    Value* const at = base + ip[3];
    const Value receiver = at[0];
    if (receiver.kind == ValueKind::Obj && receiver.as.obj->type != ObjType::Instance) {
        // The C++ method this place called last, on a value of the same type: called again at
        // once, its result straight to slot A, where nothing is to be checked first (as quickNative).
        const Obj* obj = receiver.as.obj;
        const uint64_t seen = caches[FAST_U16(12)].load(std::memory_order_relaxed);
        if (static_cast<int>((seen >> 48) & 63) == static_cast<int>(obj->type) + 1 && obj->core == 0 && coreId_ == 0 && pendingEnums_ == 0 &&
            bytesAllocated_ <= nextGC_ && !attention_.load(std::memory_order_relaxed) && ((seen >> 54) & 1 & obj->frozen) == 0 &&
            (argc == 0 || !at[1].isObjType(ObjType::Instance))) {
            bool plain = true;
            if (((seen >> 55) & 1) == 0)
                for (int i = 1; i <= argc; i++)
                    if (at[i].kind == ValueKind::Ref || at[i].kind == ValueKind::Small) plain = false;
            if (plain) {
                sp_ = sp;
                frame->ip = code + operandI32(ip + 18);  // an error's line, or where a task set aside goes on
                nativeBase_ = at;
                pendingTypeArgs_ = nullptr;
                pendingDestination_ = Value::null();
                const NativeMethodFn fn = reinterpret_cast<NativeMethodFn>(seen & 0xFFFFFFFFFFFFull);
                base[ip[0]] = fn(*this, receiver, argc, at + 1);
                sp = base + ip[2];
                ip += 22;
                NEXT();
            }
        }
    }
    const ObjString* name = asString(consts[FAST_U16(10)]);
    const bool open = (name->restricted & 1) == 0 || (ip[5] & 16) != 0;
    if (receiver.isSmall()) {
        // A method of a small value's type, run on the value itself (M6.6 part 5).
        if (open)
            if (const Value* method = smallClassOf(receiver)->methods.find(name)) QUICK_CALL(asClosure(*method), argc, R_CALL_RETURN(22, 18))
    } else if (receiver.isObjType(ObjType::Value)) {
        // A method of a value's type (M6.6 part 2), which any core may run.
        if (open)
            if (const Value* method = asValue(receiver)->cls->methods.find(name)) QUICK_CALL(asClosure(*method), argc, R_CALL_RETURN(22, 18))
    } else if (receiver.isRef()) {
        // A method of the type of a value where it is kept, run on it there (G9).
        const ObjClass* cls = refClassOrNull(receiver);
        if (cls != nullptr && open)
            if (const Value* method = cls->methods.find(name)) QUICK_CALL(asClosure(*method), argc, R_CALL_RETURN(22, 18))
    } else if (receiver.kind == ValueKind::Obj && receiver.as.obj->type != ObjType::Instance &&
               bytesAllocated_ <= nextGC_ && !attention_.load(std::memory_order_relaxed)) {
        // A method in C++ of a string, list, map, set or bytes: kept in the place's cache, and called.
        // Their methods are open to all, whatever a class keeps private under the same name.
        if (const NativeMethod* native = quickNative(receiver, name, argc, at + 1)) {
            caches[FAST_U16(12)].store(nativeCacheEntry(native, receiver.as.obj->type), std::memory_order_relaxed);
            sp_ = sp;
            frame->ip = code + operandI32(ip + 18);
            nativeBase_ = at;
            pendingTypeArgs_ = nullptr;
            pendingDestination_ = Value::null();
            base[ip[0]] = native->fn(*this, receiver, argc, at + 1);
            sp = base + ip[2];
            ip += 22;
            NEXT();
        }
    }
    ip = code + operandI32(ip + 14);
    NEXT();
    }
}
#undef R_CALL_ARGUMENT
#undef R_CALL_PUT
#undef R_CALL_RETURN
HAS_COLD(RGetPath)
CASE(RGetPath) {
    // `node.sizes.pref.width` (G14): from an object of the class last seen, or a value where it
    // is kept, the last field read where its cache says, here; anything else in the rare part.
    // u8 A, u8 B, u8 T, u8 S, u16 cache, u8 hops, u16 name x3, i32 slow.
    {
        const Value& obj = base[ip[1]];
        const uint64_t seen = caches[FAST_U16(4)].load(std::memory_order_relaxed);
        if (obj.isObjType(ObjType::Instance)) {
            ObjInstance* const instance = asInstance(obj);
            if ((seen & 1) != 0 && static_cast<uint32_t>(seen >> 32) == instance->shape && (instance->core == 0 || instance->core == coreId_)) {
                const Value* const slot = reinterpret_cast<const Value*>(reinterpret_cast<const uint8_t*>(instance) + ((seen >> 3) & 0x1FFF) * 8);
                if (slot->kind == ValueKind::Ref && slot->as.obj == instance) {
                    readPathField(instance->inlineData() + ((seen >> 16) & 0xFFFF), seen, base[ip[0]]);
                    sp = base + ip[2];
                    ip += 17;
                    NEXT();
                }
            }
        } else if (obj.isRef() && obj.refDepth == 0 && (seen & 1) == 0) {
            const ObjClass* const cls = refHolderClass(obj);
            if (cls != nullptr && static_cast<uint32_t>(seen >> 32) == cls->shape) {
                if (const uint8_t* at = refQuickly(obj, false)) {
                    readPathField(at + ((seen >> 16) & 0xFFFF), seen, base[ip[0]]);
                    sp = base + ip[2];
                    ip += 17;
                    NEXT();
                }
            }
        }
    }
    GO_COLD(RGetPath);
}
COLD(RGetPath) {
    {
    const Value obj = base[ip[1]];
    Value read;
    if (resolvePath(obj, ip, consts, caches, read)) {
        base[ip[0]] = read;
        sp = base + ip[2];
        ip += 17;
        NEXT();
    }
    Value* const at = base + ip[3];
    at[0] = obj;
    sp = at + 1;
    ip = code + operandI32(ip + 13);
    NEXT();
}
}
CASES(RGetIndexPropSS, RGetIndexPropSK) {
    {
    // `items[i].x` (2026-11-10): item i of a list of a value type's items, in range and not null,
    // its field read where it is when GetProp's cache knows the class; anything else the slow
    // way, GetIndexRef and GetProp. u8 A, u8 B, u8 C, u8 T, u8 S, u16 cache, i32 slow.
    const Value& obj = R_S(1);
    const Value& index = opAt(ip - kOpBytes) == Op::RGetIndexPropSK ? R_K(2) : R_S(2);
    if (caches != nullptr && obj.isObjType(ObjType::List) && index.isInt()) {
        const ObjList* list = asList(obj);
        const uint64_t i = static_cast<uint64_t>(index.as.i);
        if (i < list->size() && list->store() == ListStore::Items && obj.as.obj != bandObject_ &&
            (obj.as.obj->core == coreId_ || obj.as.obj->core == 0)) {
            const PackedItems& pack = *list->pack;
            const uint64_t seen = caches[FAST_U16(5)].load(std::memory_order_relaxed);
            if (static_cast<uint32_t>(seen >> 32) == pack.itemClass->shape && !(pack.nullable && (pack.nulls[i / 64] >> (i % 64) & 1))) {
                const ValueField& field = pack.itemClass->layout.load(std::memory_order_relaxed)->fields[(seen >> 16) & 0xFFFF];
                if (readPlain(pack.block.data() + i * pack.width + field.offset, field, base[ip[0]])) {
                    sp = base + ip[3];
                    ip += 11;
                    NEXT();
                }
            }
        }
    }
    const Value objHere = obj, indexHere = index;
    Value* const at = base + ip[4];
    at[0] = objHere;
    at[1] = indexHere;
    sp = sp_ = at + 2;
    ip = code + operandI32(ip + 7);
    NEXT();
}
}
CASE(Count_) {
    throw runtimeError(Thrown::InternalError, "invalid instruction");
}
