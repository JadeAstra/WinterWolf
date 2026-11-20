#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <iosfwd>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "bytecode.h"
#include "host.h"
#include "value.h"

namespace ww {

// A value thrown by a WinterWolf program (or a runtime error) travelling up the host stack.
struct WwThrow {
    Value value;

    // Call stack at the throw site, innermost first. Only captured when nothing will catch it.
    std::vector<std::string> trace{};
    bool hasTrace = false;
    // A script's limit passed (VM::limitPassed): it unwinds every frame of the run that has it,
    // taken by no catch and running no finally; its value is null.
    bool limit = false;
};

// Raised by the exit() builtin.
struct WwExit {
    int code;
};

constexpr uint8_t kDropResult = 1;
constexpr uint8_t kMakeWhole = 2;
struct CallFrame {
    ObjClosure* closure;
    const uint8_t* ip;
    Value* base;  // slot 0: the receiver in methods, the callee itself otherwise
    // What its return does besides returning (any of them: the return's slow way):
    // kDropResult, the call's result is not wanted: what was under it on the stack is the result
    // instead (this is how `apply` and `also` give back the value they were called on);
    // kMakeWhole, its function's whole region is made there (VM::nativeCall, by a section's
    // core, which writes no return's word: writeRunWord).
    uint8_t dropResult;
    // How many times the function has started again, calling itself as its last step (D29).
    uint32_t repeats;
};
// Kept at 32 bytes: where in its line's places a frame's end (D149) is beside the frames, with
// their type arguments and destinations (VM::framePlacesEnd), as each frame's `ip` is written at
// every instruction and its place against the stack's is felt in every loop.
static_assert(sizeof(CallFrame) == 32, "a frame is four words");
// The type arguments a frame's generic function was called with (M6.6 part 1) are kept beside
// the frames, not in them, which stay small for the calls that have none: VM::frameTypeArgs.

// A catch block that is waiting for an error.
struct Handler {
    int frameIndex;
    Value* stackTop;
    const uint8_t* ip;
};

struct ObjFiber;

// What belongs to one line of execution: its value stack, the calls in progress and their
// catch blocks. Every thread has one, and so has every task. The VM works on the one that is
// running through members of its own (sp_, frames_ and so on); the others keep their state
// here until it is their turn.
struct Context {
    Value* stack = nullptr;
    Value* stackEnd = nullptr;
    Value* sp = nullptr;
    CallFrame* frames = nullptr;
    int frameCount = 0;
    int maxFrames = 0;   // the frames laid out, with their type arguments and destinations after them
    int frameLimit = 0;  // how many may be used now: all of them, or of a stack that grows, those committed
    // A stack that grows (a generator's, D98): its memory reserved whole and committed as it is
    // used, so that it starts small and moves nowhere; how far it may go.
    bool growable = false;
    Value* stackReserveEnd = nullptr;
    std::vector<Handler> handlers;
    ObjUpvalue* openUpvalues = nullptr;
    int nativeDepth = 0;
    Value* nativeBase = nullptr;
    // The type arguments a call about to be made was given (Op::WithTypes): a thread may give
    // way between them and the call, so they are its own.
    const ObjTypeInfo* pendingTypeArgs = nullptr;
    // And where the value the next call gives is to be made (VM::pendingDestination_), for the
    // same reason (2026-11-19: kept in the VM alone, a thread that gave way between WithDestination
    // and its Rect's making had its place taken by another thread's Duration, which a Rect then
    // wrote over: the test runner's `p.waitFor(limit.seconds)` given a Rect, once in a suite).
    Value pendingDestination = Value::null();
    // What ImplicitFirst gave the next lookup of a bare name (D58, D59): a thread may give way
    // between them (at InvokeImplicit's safe point), so they too are its own.
    Value implicitFirst[8];
    uint8_t implicitFirstCount = 0;
    bool implicitOnly = false;
    uint8_t implicitTried = 0;
    Context* parent = nullptr;  // the one waiting for this one to finish or be set aside
    // The frame the innermost run() on this line of execution started with: where it ends. Kept
    // here, not in the VM, as the VM's threads take turns with it (instructions.inc, M6.7).
    int runBase = -1;
    ObjFiber* fiber = nullptr;  // the task this is the stack of; null for a thread's own
    // Where its frames keep values in their variables (D149): made when the first needs it.
    PlaceArena* places = nullptr;
    // The host its code reaches (M6.6.2): a sandbox's, given by the code that started it or
    // entered with Sandbox.run; null for the VM's own. The ones it had before each Sandbox.run.
    std::shared_ptr<Host> host;
    std::vector<std::shared_ptr<Host>> hostsBefore;

    Context(size_t stackSize, int frameLimit, bool grows = false);
    ~Context();
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;
    void release();  // gives the memory back; the context is finished with
    // Of a stack that grows: commits enough for `top` and `frames`; false where it has no more.
    bool grow(const Value* top, int frames);
};

// A task: a function running on a stack of its own, so that it can be set aside in the
// middle of a call and taken up again later (Kotlin's coroutine). See threads.cpp.
enum class FiberState : uint8_t { Fresh, Running, Suspended, Done };

struct ObjFiber final : Obj {
    // As deep as a thread's may go (G50): reserved whole, and committed as it is used.
    static constexpr size_t kStackSize = 1 << 20;
    static constexpr int kMaxFrames = 20000;

    Context context;
    FiberState state = FiberState::Fresh;
    Value entry;                  // the function it runs
    Value* pendingSlot = nullptr; // where the call it is waiting in will leave its result
    int64_t waitToken = 0;        // the wait it is at, or was last at
    int64_t resumedToken = -1;    // the last wait an answer has been given for
    int64_t waitingAt = 0;        // the wait it is set aside at; 0 if it is set aside for something else
    bool generator = false;       // a sequence's generator (D98): set aside only where it yields
    bool hasEarly = false;        // an answer came before the task got as far as waiting
    bool earlyOk = false;
    Value early;
    // Cancelled (M6.7): it throws a CancellationException at its next wait. The wait a cancel
    // answered, whose own answer, when it comes, has nowhere to go.
    bool cancelPending = false;
    int64_t cancelledAt = 0;

    // Its stack starts small and grows as needed, so that many can be alive at once (D98, G50).
    explicit ObjFiber(Value function) : Obj(ObjType::Fiber), context(kStackSize, kMaxFrames, true), entry(function) {
        context.fiber = this;
    }
};

inline ObjFiber* asFiber(const Value& v) { return static_cast<ObjFiber*>(v.as.obj); }

// Thrown by a built-in function to set the running task aside. It passes through the
// interpreter loop untouched, leaving the task's calls as they are, to where the task was
// started or last taken up.
struct FiberSuspend {};

// Thrown at a safe point in a worker that has been told to stop. Like FiberSuspend it is not
// a WinterWolf error: no catch block sees it, and it ends whatever the worker was doing.
struct WorkerStop {};

struct WorkerLink;    // worker.cpp: what a worker and the program that started it share
struct ParallelPool;  // parallel.cpp: the cores of a VM's parallel sections
struct ParallelSection;
struct ParallelPiece;
struct SharedBlock;   // worker.cpp: frozen data shared with workers, outside every heap (D97)

// One operating-system thread that runs WinterWolf code.
struct ThreadState {
    Context base;
    Context* current;  // the innermost context running on this thread
    int nesting = 0;   // how many tasks are running inside one another on it

    // The thread's temporaries, while another thread has the lock (see VM::tempRoots).
    std::vector<Value> tempRoots;
    std::vector<const std::vector<Value>*> tempVectors;

    int64_t id = 0;
    std::string name;
    bool daemon = false;
    bool finished = false;
    Value function;  // what it runs
    Value self;      // the Thread object a program holds, if one was made for it
    std::atomic<bool> interrupted{false};
    std::vector<ThreadState*> joiners;
    // The locks it holds, the one taken last at the back: what waitUntil gives back (D86).
    std::vector<const Obj*> held;

    // park() and unpark(): how a thread waits for something another thread will do.
    std::mutex parkMutex;
    std::condition_variable parkCv;
    bool parkToken = false;
    bool notified = false;  // set by notify() for a thread in wait()

    ThreadState(size_t stackSize, int frameLimit, bool grows = false) : base(stackSize, frameLimit, grows), current(&base) {}
};

// Interned names the VM looks up by pointer.
struct Names {
    ObjString* init;
    ObjString* toString;
    ObjString* message;
    ObjString* cause;
    ObjString* length;
    ObjString* size;
    ObjString* indices;
    ObjString* lastIndex;
    ObjString* keys;
    ObjString* itemType;
    ObjString* keyType;
    ObjString* valueType;
    ObjString* values;
    ObjString* key;
    ObjString* value;
    ObjString* code;
    ObjString* character;  // `n.char`
    ObjString* fromEnd;    // an Index's
    ObjString* iterator;
    ObjString* first;
    ObjString* last;
    ObjString* absoluteValue;
    ObjString* sign;
    ObjString* start;
    ObjString* end;
    ObjString* step;
    ObjString* name;
    ObjString* ordinal;
    ObjString* compareTo;
    ObjString* hasNext;
    ObjString* next;
    ObjString* entries;
    ObjString* entryName;    // an entry's own name, kept apart from a `name` property that replaces it
    ObjString* entryOrdinal; // and its place in the list, likewise
    ObjString* enumEntries;  // where an enum's own list is kept, whatever `entries` gives
    ObjString* opAdd;
    ObjString* opSub;
    ObjString* opMul;
    ObjString* opDiv;
    ObjString* opMod;
    ObjString* opEq;
    ObjString* hashCode;
    ObjString* opLt;
    ObjString* opLe;
    ObjString* opGt;
    ObjString* opGe;
    ObjString* opNeg;
    ObjString* opIndex;
    ObjString* opIndexSet;
    ObjString* opShl;
    ObjString* opShr;
    ObjString* opBitAnd;
    ObjString* opBitOr;
    ObjString* opBitXor;
    ObjString* opBitNot;
};

enum class BuiltinType { String, List, Map, Range, Bytes, Number, Char, Byte, Index, Set, Count_ };

// The classes of what the VM throws (D35), Java's, declared in src/std/errors.ww; and
// Throwable, the root, which is all that `throw` takes.
enum class Thrown : uint8_t {
    RuntimeException, ArithmeticException, IndexOutOfBoundsException, NullPointerException, ClassCastException,
    IllegalArgumentException, NumberFormatException, IllegalStateException, UnsupportedOperationException,
    NoSuchElementException, ConcurrentModificationException, AssertionError, StackOverflowError, OutOfMemoryError,
    InternalError, IOException, Throwable, SocketTimeoutException, TimeoutException, CancellationException, SecurityException, ScriptLimitException, UnknownHostException, Count_
};

// The instructions as functions (M6.7): built with Clang, each instruction of instructions.inc is
// a function of VM that calls the next instruction's as its last step, the loop's state in
// registers from one to the next (preserve_none, musttail); built otherwise, they are the cases
// of run()'s switch.
#if defined(__clang__) && !defined(WW_OP_COUNTS)
#if __has_attribute(preserve_none)  // Clang 19 on
#define WW_TAIL_CALLS 1
#if __has_feature(address_sanitizer)
// Under AddressSanitizer (clang-cl), Windows' C++ unwinder cannot read the state of an
// instrumented preserve_none function and crashes passing one: the instructions themselves go
// unchecked, what they call is checked.
#define WW_TAIL_FN __attribute__((preserve_none, no_stack_protector, no_sanitize("address")))
#else
#define WW_TAIL_FN __attribute__((preserve_none, no_stack_protector))
#endif
#define WW_TAIL_PARAMS                                                                                              \
    [[maybe_unused]] const uint8_t *ip, [[maybe_unused]] Value *sp, [[maybe_unused]] Value *base, [[maybe_unused]] CallFrame *frame,                     \
        [[maybe_unused]] const Value *consts, [[maybe_unused]] std::atomic<uint64_t> *caches, [[maybe_unused]] const uint8_t *code
#endif
#endif
#ifndef WW_TAIL_CALLS
#define WW_TAIL_CALLS 0
#endif

class VM final : public KeyMethods {
public:
    // `worker`: this VM is a worker's, running beside the one that started it. It has no
    // windows: what belongs to the one user interface stays with the main program.
    explicit VM(bool worker = false);
    // The VM of one core of `root`'s parallel sections (parallel.cpp). It has an interpreter
    // of its own and works on root's objects.
    VM(VM& root, uint8_t coreId);
    ~VM();
    bool isWorker() const { return isWorker_; }
    VM(const VM&) = delete;
    VM& operator=(const VM&) = delete;

    // ---- Heap ----

    // Allocates a garbage-collected object. Collection only ever runs at safe points inside
    // the interpreter loop (calls and loop back-edges), never here, so a native function may
    // hold raw object pointers freely until it calls back into the VM.
#ifdef WW_OP_COUNTS
    // In the build that counts instructions: every object made, shown when the program ends.
    static std::atomic<uint64_t> objectsMade;
#define WW_COUNT_OBJECT() objectsMade.fetch_add(1, std::memory_order_relaxed)
#else
#define WW_COUNT_OBJECT() ((void)0)
#endif
    template <class T, class... A>
    T* alloc(A&&... args) {
        WW_COUNT_OBJECT();
        T* obj = new T(std::forward<A>(args)...);
        obj->core = coreId_;  // what a core makes is its own until the section is over
        obj->next = objects_;
        objects_ = obj;
        bytesAllocated_ += sizeof(T);
        return obj;
    }

    // An object of `cls`, with room for the fields the class declares, none of them given a
    // value yet (M6.5 part 4).
    ObjInstance* newInstance(ObjClass* cls, uint16_t captures = 0, bool typed = false) {
        if (!cls->fieldsLaid.load(std::memory_order_acquire)) layFields(cls);
        WW_COUNT_OBJECT();
        const uint32_t count = cls->slotCount;
        // Its payloads before its slots (M6.6 part 6), the values kept inside it after (part 5).
        const bool withValues = cls->inlineLaid.load(std::memory_order_acquire) && !cls->inlineFields.empty();
        const uint32_t inlineSlots = withValues ? cls->inlineSlots : 0;
        const uint32_t slots = count + captures + (typed ? 1u : 0u) + inlineSlots;
        ObjInstance* obj = new (slots, cls->payloadBytes) ObjInstance(cls, count, captures, typed, inlineSlots, withValues, cls->payloadBytes);
        obj->core = coreId_;
        obj->next = objects_;
        objects_ = obj;
        bytesAllocated_ += sizeof(ObjInstance) + slots * sizeof(Value) + cls->payloadBytes;
        return obj;
    }
    // Makes an object of `cls` in the slot under its `argc` arguments and starts its
    // initializer; with `captures`, an anonymous object that keeps them (Op::NewObject).
    // Returns whether a frame was pushed.
    bool construct(ObjClass* cls, int argc, bool packed, const Value* captures = nullptr, uint16_t captureCount = 0);
    // A finished class's initializer and fields' function looked at: whether each only gives
    // fields arguments and constants (ObjClass::initPlan, fieldsPlan; objects plan part 4).
    void seeHeaderInit(ObjClass* cls);
    // The fields of `plan` given to a new object as `fn`'s SET_OWNs would give them, from `args`
    // (the function's arguments), where every one fits as their caches say; false, nothing
    // written, where one does not.
    bool fieldsFromPlan(ObjInstance* instance, const FunctionProto* fn, const std::vector<ObjClass::PlannedField>& plan, const Value* args);
    // An anonymous object made with the `count` captured cells on the stack's top (NewObject).
    bool constructObject(ObjClass* cls, int count);
    // A field the class declares in place of a property an interface gives with accessors.
    void dropInterfaceAccessors(ObjClass* cls, const ObjString* name);

    // ---- Lists (M6.6 part 3): see "Lists" in vm.cpp ----
    ListStore storeFor(const ObjTypeInfo* item, uint32_t& width, ObjClass*& itemClass);
    void typeList(ObjList* list, ObjTypeInfo* type);

    // ---- Sets (M6.6 part 8), and views of a map's keys and entries (D77) ----
    // Part `index` of a value taken apart (D64).
    Value componentOf(Value whole, size_t index);
    ObjSet* newSet(ObjTypeInfo* type);
    Value mapView(ObjMap* map, bool entries);
    size_t setSize(const ObjSet* set) const { return set->view() ? set->viewOf->map.size() : set->items.size(); }
    bool setContains(const ObjSet* set, Value item);
    bool setAdd(ObjSet* set, Value item);
    bool setRemove(ObjSet* set, Value item);
    std::vector<Value> setItems(const ObjSet* set);
    // Every item of a list, a range, a set, a string, bytes or a map (its entries).
    std::vector<Value> itemsOf(Value from, const char* what);
    WwThrow viewChanged(const ObjSet* view);
    uint8_t* itemAt(ObjList* list, size_t i);
    // Item `i` of a list, `i` already checked against its size: a value type's item copied into a box.
    Value listGet(ObjList* list, size_t i) { return !list->packed() ? list->values()[i] : packedGet(list, i); }
    Value packedGet(ObjList* list, size_t i);
    void checkItem(const ObjList* list, Value v);
    void writeItem(ObjList* list, size_t i, Value v);
    void listSet(ObjList* list, size_t i, Value v);
    void listInsert(ObjList* list, size_t at, const Value* items, size_t n);
    void listAdd(ObjList* list, Value v);
    // Room made for `n` items in all, none added (ensureCapacity).
    void listReserve(ObjList* list, size_t n);
    void listErase(ObjList* list, size_t at, size_t n);
    void listClear(ObjList* list);
    std::vector<Value> listCopy(ObjList* list);
    ObjList* listLike(const ObjList* like);
    // `+` on a collection (the user, 2026-11-07, as in Kotlin): a new one with an item added, or
    // with the items of another collection; a map with a Pair's entry or another map's entries.
    ObjList* listPlus(ObjList* left, Value right);
    ObjMap* mapPlus(ObjMap* left, Value right);
    // Whether `right`, added to a collection of items of `itemType` (null: of anything), is a
    // collection whose items are added, rather than one item: a list, a set or a range that is
    // not itself an item of that type.
    bool addsMany(const ObjTypeInfo* itemType, Value right);
    // The error for an item that is not of a collection's item type.
    WwThrow notAnItem(const char* kind, const ObjTypeInfo* type, const ObjTypeInfo* itemType, Value item);
    void listCopyRange(ObjList* to, const ObjList* from, size_t start, size_t count);
    void listSwap(ObjList* list, size_t i, size_t j);
    void markItems(ObjList* list);
    bool addNew(int argc);
    bool insertNew(int argc, bool givesTrue);
    void makeFromHeader(ObjClass* cls, int argc, Value dest, const ObjTypeInfo* made = nullptr);
    // `cls` with the type arguments `args` (a kTypeArguments type), as a type: found again in
    // ObjTypeInfo::madeAs where the last one asked was of the same class.
    const ObjTypeInfo* typeMadeOf(ObjClass* cls, const ObjTypeInfo* args) {
        ObjTypeInfo* known = args->madeAs.load(std::memory_order_acquire);
        if (known != nullptr && known->cls == cls) return known;
        ObjTypeInfo* made = typeOf(kTypeInstance, false, cls, args->args);
        const_cast<ObjTypeInfo*>(args)->madeAs.store(made, std::memory_order_release);
        return made;
    }
    void layInline(ObjClass* cls);
    void layFields(ObjClass* cls);
    void fieldAdded(ObjClass* cls, size_t index);
    // What a field read gives where it is not shown to be safe to read in place: a copy of a
    // value kept inside its object (M6.6 part 5), anything else as it is.
    Value fieldRead(const Value& slot) { return slot.kind == ValueKind::Ref ? ownRef(slot) : slot; }
    void listArrange(ObjList* list, const std::vector<size_t>& order);

    // ---- References (M6.6 part 4) ----
    // A value as it is kept: a Ref replaced by a box with a copy, anything else as it is. What
    // is stored anywhere but a thread's stack goes through this.
    Value own(Value v) { return v.kind == ValueKind::Ref ? ownRef(v) : v; }

    // The fields extends add (D157), kept beside the objects that have read or set one: each
    // object's entry holds them by name. The collector keeps what an entry holds while its object
    // lives and drops the entry with the object; a lock keeps the cores of a parallel section
    // from meeting in the table.
    Value extraField(ObjInstance* instance, ObjString* name, Value init);
    void setExtraField(ObjInstance* instance, ObjString* name, Value value);
    std::unordered_map<Obj*, Table> extras_;
    std::mutex extrasLock_;
    // A value of a value type in a box of its own, whatever form it is in (M6.6 part 5): for
    // the code that works on boxes, and C++ that is given values.
    Value boxed(Value v) { return v.kind == ValueKind::Ref ? ownRef(v) : v.kind == ValueKind::Small ? boxSmall(v) : v; }
    Value boxSmall(Value v);
    void boxAll(Value* from, Value* to) {
        for (Value* at = from; at < to; at++)
            if (at->kind == ValueKind::Ref || at->kind == ValueKind::Small) *at = boxed(*at);
    }
    // A value made of a small value type's bytes, kept in the value itself, with its class with
    // its type arguments where known (G4).
    Value smallFrom(const ObjClass* cls, const uint8_t* bytes, size_t size, const ObjTypeInfo* type = nullptr) {
        uint64_t bits = 0;
        std::memcpy(&bits, bytes, size);
        Value made = Value::inSlot(cls->smallId, bits);
        if (type != nullptr && !type->args.empty()) {  // a type with no arguments says no more than the class
            const uint16_t known = type->valueNumber.load(std::memory_order_acquire);
            made.refOffset = known != 0 ? known : valueTypeNumber(type);
        }
        return made;
    }

    // ---- A generic value type's type arguments (G4) ----
    // What a value of a value type says it is: its class with its type arguments (`Pair<int,
    // string>`); null where it says nothing more than its class. A box and a frame's place keep
    // it beside the class; a small value, by a number of this VM's in its slot's `refOffset`,
    // which a small value does not otherwise use; an item of a list, or a value kept inside an
    // object, takes it from the list's or the field's declared type.
    const ObjTypeInfo* recordedType(const Value& v);
    const ObjTypeInfo* refType(const Value& r);
    // The number of `type` among this VM's (its root's), given out the first time: 0 for null,
    // and for any once all 65,535 are given out.
    uint16_t valueTypeNumber(const ObjTypeInfo* type);
    const ObjTypeInfo* smallType(const Value& v) const {
        if (v.refOffset == 0) return nullptr;
        const VM& root = root_ != nullptr ? *root_ : *this;
        return root.valueTypeTable_[v.refOffset].load(std::memory_order_acquire);
    }
    std::unique_ptr<std::atomic<const ObjTypeInfo*>[]> valueTypeTable_;
    std::unordered_map<const ObjTypeInfo*, uint16_t> valueTypeNumbers_;
    uint16_t nextValueType_ = 1;
    std::vector<ObjClass*> smallKept_;  // the small value types this VM numbered, kept alive
    // The classes of anonymous objects made with type parameters of the code around them (G66),
    // by the global that names the object and the type arguments of the frame that made it:
    // types are made once each and never freed, so their address tells them apart.
    std::unordered_map<uint64_t, ObjClass*> typedClasses_;
    static uint64_t typedClassKey(int global, const ObjTypeInfo* types) {
        return static_cast<uint64_t>(global) << 48 ^ static_cast<uint64_t>(reinterpret_cast<uintptr_t>(types));
    }
    Value ownRef(Value v);
    void ownAll(Value* from, Value* to) {
        for (Value* at = from; at < to; at++)
            if (at->kind == ValueKind::Ref) *at = ownRef(*at);
    }
    // Where what a Ref points at is, checked: an error for an item no longer in its list, and
    // for `write`, for a value that may not be changed there.
    uint8_t* refAt(Value r, bool write);
    // The bytes of a Ref of depth 0 where this core may read them (`write`: and change them);
    // null where it may not, or the item is gone, which the slow way says (M6.6 part 4).
    WW_ALWAYS_INLINE uint8_t* refQuickly(const Value& r, bool write) const {
        if (r.as.obj == bandObject_) {
            if (r.refIndex < bandFrom_ || r.refIndex >= bandTo_) return nullptr;
        } else if (r.as.obj->type == ObjType::Instance) {
            if (write ? r.as.obj->core != coreId_
                      : (r.as.obj->core != 0 && r.as.obj->core != coreId_ && (readable_ == nullptr || !readable_->test(r.as.obj->core))))
                return nullptr;
        } else if (write && (r.as.obj->type == ObjType::List ? r.as.obj->core != coreId_ : !r.as.obj->valueOpen)) {
            return nullptr;
        }
        return refBytes(r);
    }
    // Field `name` of what a Ref points at: in place, a value held in it a Ref of its own.
    bool refField(Value r, const ObjString* name, Value& out);
    void setRefField(Value r, ObjString* name, Value value);
    // The place a variable, field or item holding a value is: its value given a box of its own,
    // writable, in `slot`, and a Ref to it (M6.6 part 4).
    Value placeIn(Value& slot);

    // ---- Values in a frame's places (M6.7 step 6, D149; ObjPlace) ----
    // Where the places of frame `index` start: where those of the nearest frame under it that
    // has places end.
    uint32_t placesStart(ptrdiff_t index) const {
        for (ptrdiff_t below = index - 1; below >= 0; below--)
            if ((frames_[below].closure->proto->typeArgs & kHasPlaces) != 0) return framePlacesEnd(below) & ~PlaceArena::kNone;
        return 0;
    }
    // Place `place` of `frame`; null where the frame has no places.
    ObjPlace* placeOf(const CallFrame* frame, int place) const {
        const uint32_t end = framePlacesEnd(frame - frames_);
        if ((end & PlaceArena::kNone) != 0) return nullptr;
        return context_->places->at(end - static_cast<uint32_t>(place) - 1);
    }
    void enterPlaces(CallFrame* frame, int count);
    void setPlace(const CallFrame* frame, Value& slot, Value value, int place);
    Value withPlace(const CallFrame* frame, int place);
    // Whether `result` is where the caller of `frame` asked for it to be made.
    bool madeAsAsked(const CallFrame* frame, const Value& result) {
        if ((frame->closure->proto->typeArgs & kTakesDestination) == 0) return false;
        const Value& dest = frameDestination(frame - frames_);
        return dest.kind == ValueKind::Ref && dest.as.obj == result.as.obj && dest.refIndex == result.refIndex &&
               dest.refOffset == result.refOffset && dest.refDepth == result.refDepth;
    }
    // Whether a Ref into the places is to one of the running frame's own.
    bool ownPlace(const Value& r) const {
        const ptrdiff_t index = frameCount_ - 1;
        const uint32_t end = framePlacesEnd(index);
        if ((frames_[index].closure->proto->typeArgs & kHasPlaces) == 0 || (end & PlaceArena::kNone) != 0) return false;
        const uint32_t at = context_->places->indexOf(r.as.obj);
        return at < end && at >= placesStart(index);
    }

    // ---- Value types (M6.6 part 2) ----
    ObjClass* fieldClass(FieldDecl& field);
    const ValueLayout& layoutOf(ObjClass* cls);
    [[noreturn]] void valueTooBig(const ObjClass* cls, uint32_t size);
    ObjValue* newValue(ObjClass* cls);
    // An object's narrow field (a byte, a char, a storage width, G20), remembered by a place that
    // reads or writes it: where its payload is, its bit, whether it is a val or tracked, and what
    // it keeps, under its class's shape with the top bit set, which no shape has. So the quick ways,
    // which read 8 bytes of a payload, never take it; the rare ones read and write it at once.
    static constexpr uint32_t kNarrowShape = 0x80000000u;
    static uint64_t narrowEntry(uint32_t shape, const FieldDecl& d) {
        return static_cast<uint64_t>(shape | kNarrowShape) << 32 | static_cast<uint64_t>(sizeof(ObjInstance) + d.payloadOffset) << 16 |
               static_cast<uint64_t>(d.payloadBit) << 8 | (d.isVal ? 0x40u : 0u) | (d.tracked ? 0x20u : 0u) | static_cast<uint64_t>(d.narrow);
    }
    bool narrowRead(const Value& obj, uint64_t seen, Value& out) const {
        if ((seen >> 63) == 0 || !obj.isObjType(ObjType::Instance)) return false;
        const ObjInstance* instance = asInstance(obj);
        if (static_cast<uint32_t>(seen >> 32) != (instance->shape | kNarrowShape) || (instance->core != 0 && instance->core != coreId_)) return false;
        out = readNarrow(static_cast<Narrow>(seen & 7), reinterpret_cast<const uint8_t*>(instance) + ((seen >> 16) & 0xFFFF));
        return true;
    }
    // A val's is given only once, the general way.
    bool narrowWrite(const Value& obj, uint64_t seen, Value value) {
        if ((seen >> 63) == 0 || (seen & 0x40) != 0 || !obj.isObjType(ObjType::Instance)) return false;
        ObjInstance* instance = asInstance(obj);
        const Narrow narrow = static_cast<Narrow>(seen & 7);
        if (static_cast<uint32_t>(seen >> 32) != (instance->shape | kNarrowShape) || instance->core != coreId_ || !fitsNarrow(narrow, value)) return false;
        if (narrow == Narrow::Byte && value.isInt()) value = Value::byte(static_cast<uint8_t>(value.as.i));
        writeNarrow(narrow, reinterpret_cast<uint8_t*>(instance) + ((seen >> 16) & 0xFFFF), value);
        if ((seen & 0x20) != 0) {
            const uint32_t bit = (seen >> 8) & 0xFF;
            instance->payloadData()[bit / 8] |= static_cast<uint8_t>(1u << (bit % 8));
        }
        return true;
    }
    // RGetPath's rare way (G14): the path of fields read worked out from where it starts, kept in
    // its cache and read; false where it cannot be told so (the stack code reads it then).
    bool resolvePath(const Value& obj, const uint8_t* ip, const Value* consts, std::atomic<uint64_t>* caches, Value& out);
    // A plain field at `at`, of the kind an RGetPath's cache says, given whole.
    static WW_ALWAYS_INLINE void readPathField(const uint8_t* at, uint64_t seen, Value& out) {
        uint64_t bits = 0;
        const uint64_t kind = (seen >> 1) & 3;
        if (kind == 2) bits = *at != 0 ? 1 : 0;
        else std::memcpy(&bits, at, sizeof bits);
        out.setWhole(static_cast<uint8_t>(kind == 0 ? ValueKind::Int : kind == 1 ? ValueKind::Float : ValueKind::Bool), bits);
    }
    // A field of a value, read on its own; one of a value type is given `type` (G4), the
    // field's declared type made for the value holding it, where it is known.
    Value readValueField(const uint8_t* at, const ValueField& field, const ObjTypeInfo* type = nullptr);
    // Whether a value of value type `cls` fits `type`, written with type arguments (G4): by the
    // type arguments it keeps, and where those are wider than asked or not known, by what its
    // fields declared with them hold.
    bool valueFits(const Value& v, ObjClass* cls, const ObjTypeInfo* type);
    // A copy of a value of a value type that keeps `type` as its type arguments: what a cast to a
    // type narrower than the one it keeps gives (G4).
    Value retyped(const Value& v, const ObjTypeInfo* type);
    // An int, float or bool field read with no call, for the interpreter's quick paths.
    // An int, float or bool field, with no call: for a field of another kind, false.
    static WW_ALWAYS_INLINE bool readPlain(const uint8_t* at, const ValueField& field, Value& out) {
        switch (field.kind) {
            case ValueField::Float: {
                uint64_t bits;
                std::memcpy(&bits, at, sizeof bits);
                out.setWhole(static_cast<uint8_t>(ValueKind::Float), bits);
                return true;
            }
            case ValueField::Int: {
                uint64_t bits;
                std::memcpy(&bits, at, sizeof bits);
                out.setWhole(static_cast<uint8_t>(ValueKind::Int), bits);
                return true;
            }
            case ValueField::Bool: out.setWhole(static_cast<uint8_t>(ValueKind::Bool), *at != 0 ? 1 : 0); return true;
            // A byte, a char or another narrow width (a Color's parts): what it holds, made whole.
            case ValueField::Narrow: out = readNarrow(field.narrow, at); return true;
            default: return false;
        }
    }
    Value readPlainField(const uint8_t* at, const ValueField& field) {
        switch (field.kind) {
            case ValueField::Float: {
                double d;
                std::memcpy(&d, at, sizeof d);
                return Value::number(d);
            }
            case ValueField::Int: {
                int64_t i;
                std::memcpy(&i, at, sizeof i);
                return Value::integer(i);
            }
            case ValueField::Bool: return Value::boolean(*at != 0);
            default: return readValueField(at, field);
        }
    }
    void writeValueField(uint8_t* at, const ValueField& field, const ObjClass* owner, Value value);
    std::string storageMismatch(Narrow narrow, Value v);
    // Positions and slices (D146, D147).
    bool position(Value index, size_t size, size_t& out, const char* what);
    WwThrow outOfRange(const char* what, Value index, size_t size);
    void sliceBounds(const ObjRange* range, size_t size, size_t& begin, size_t& end);
    WwThrow rangeFromEnd(const ObjRange* range, const std::string& use);
    // An int given where a storage width it does not fit is declared: an error saying so, with
    // `where` after it. Returns normally for anything else.
    void checkRange(uint8_t type, Value v, const std::string& where);
    int64_t valueFieldIndex(const ObjValue* box, const ObjString* name) const;
    Value valueField(const ObjValue* box, size_t index);
    void setValueField(ObjValue* box, ObjString* name, Value value);
    bool valuesSame(const ObjValue* a, const ObjValue* b, int depth);
    // `type`: what the value at `at` is, where known, for one read whole (G4).
    const uint8_t* readThrough(const uint8_t* at, const ValueField* field, const uint8_t* next, const Value* consts, Value& out,
                               const ObjTypeInfo* type = nullptr);
    // Capture `k` of the object a member of an anonymous class runs on, checked.
    ObjUpvalue* capturedOf(Value receiver, int k);
    ObjUpvalue* receiverUpvalue(const CallFrame* frame, const Value* base, uint16_t entry) const;

    // Tells the collector about memory owned by an object beyond its own size.
    void account(size_t bytes) { bytesAllocated_ += bytes; }
    // A collection now, asked for by the program (Program.collectGarbage): none inside a key's
    // == or hashCode, nor on a core of a parallel section, where a safe point would not collect
    // either. It waits first for the native code it asked the builder for (made on another core
    // as it runs on): until made, a build keeps what the function had in hand when asked
    // (markWaitingBuilds), which would be counted as in use, or not, by when the build ended.
    void collectNow() {
        if (keyCalls_ != 0 || coreId_ != 0) return;
        waitForBuilds();
        collectGarbage();
    }
    // The bytes the program's objects take now, the garbage not yet collected counted with them
    // (Program.memoryUsed); what is still in use, right after a collection.
    size_t memoryUsed() const { return bytesAllocated_; }

    ObjString* intern(std::string_view s);

    // The name a class's private member is kept under (2026-11-20, o-private-shadow): text as
    // `text` is, but another object than the interned name, so that a subclass's member of that
    // name, or another class's private one, is kept apart from it. `owner` is the class's file
    // and name, as the compiler and the checker write them (its file key, a tab, `Outer.Inner`):
    // the same owner and text give the same object, in each unit and the cache.
    ObjString* privateName(std::string_view owner, std::string_view text);
    // The owner of a private name, or null for any other string (of any VM: the cache writes
    // them with no VM at hand).
    static const std::string* privateOwner(const ObjString* name);
    // The name a private name is the private form of: its text, interned.
    ObjString* publicName(const ObjString* name) {
        return privateOwner(name) != nullptr ? intern(std::string_view(name->utf8())) : const_cast<ObjString*>(name);
    }

    // Where a member `name` of `target` (an object of `cls`) is not found (o-private-shadow): the
    // other name to look for once, or null. A private name is looked for by its text: the code of
    // a class reaching, through `this` of a lambda run on another object, what that object has.
    // A name of text is looked for as a private one of the class's: code put in place from a
    // kept unit, whose bodies were not checked, reaching another object of its own class; and
    // code that may not, which is then told it is private (accessError).
    ObjString* missedName(const ObjClass* cls, ObjString* name, Value target, bool write);
    // A private name of the class's that reads as `name`, its own or one it inherits; null where
    // it has none (at once for a text no class has a private name of: ObjString::restricted 16).
    ObjString* privateLike(const ObjClass* cls, const ObjString* name) const;
    // For memberGet and memberSet, C++ asking by a text whoever may use it: the class's private name
    // of it where the object has nothing else of that text.
    ObjString* trustedName(Value obj, ObjString* name) const;

    // ---- Types when the program runs (M6.6 part 1) ----
    // The one object for a type: `id` a TypeId (0 any), for a class `cls`, with its arguments.
    ObjTypeInfo* typeOf(uint8_t id, bool nullable, ObjClass* cls, const std::vector<ObjTypeInfo*>& args);
    // The type of what a value is, as `x#` gives its class (D48): an object's class with the
    // type arguments it was made with, a list's type, `int` for an int. Null has none.
    ObjTypeInfo* typeOfValue(const Value& v);

    // ---- Reflection (D38): see reflect.cpp ----
    // What the description of `cls` gives (Op::ClassMeta), for its type `own` (the class with
    // its type arguments, or null for none): `which` 0 its fields, 1 its methods; null where it
    // has none. Made once for each type.
    Value classDescription(ObjClass* cls, const ObjTypeInfo* own, int which);
    // Whether the code of `proto` may not use the member `name` of an object of `cls`, or may
    // not assign to it; with no code, what only some code may use.
    bool memberDenied(const ObjClass* cls, const ObjString* name, const FunctionProto* proto, bool write) const;
    // The same for a member reached through `target`: an object, or a class for its statics.
    bool accessDenied(Value target, const ObjString* name, const FunctionProto* proto, bool write) const;
    // A member read, set or found by its name, whoever may use it: as reflection does once it
    // has listed it to the code asking.
    Value memberGet(Value obj, ObjString* name);
    // The global a class is kept in, or -1 (one declared inside a function): a worker runs the
    // same program, so a message names a class by it (G6). And the class in a global, or null.
    int globalSlotOf(const ObjClass* cls);
    ObjClass* classInGlobal(int slot) const;
    void bundleValues(int count);  // Op::BundleValues
    void mergeBundles();           // Op::MergeBundle
    // Whether a value is an Iterator, an object with hasNext() and next().
    bool isIteratorObject(Value v) const;
    // The map a for over a map or a set goes through: the set's own, or a view's map.
    static const ValueMap& keysOf(Value v) {
        if (v.isObjType(ObjType::Map)) return asMap(v)->map;
        const ObjSet* set = asSet(v);
        return set->view() ? set->viewOf->map : set->items;
    }
    // A for over a map or a set whose keys have changed since it began stops, as Kotlin's does:
    // going on, it could skip items or see one twice (8.2i).
    void checkUnchanged(const ObjIterator* it) {
        if (keysOf(it->source).version() != it->version) [[unlikely]] changedInLoop(it);
    }
    [[noreturn]] void changedInLoop(const ObjIterator* it);
    void memberSet(Value obj, ObjString* name, Value value);
    // An atomic field changed in one step (Op::AtomicField, D92).
    Value atomicField(Value object, ObjString* name, int kind, const Value* operands);
    // Calls `callee` with type arguments, as a generic function or class is called.
    Value callWithTypes(Value callee, const std::vector<ObjTypeInfo*>& typeArgs, int argc, const Value* args);

    // ---- ww test (D153) ----
    Value globalAt(int slot) const {
        return slot >= 0 && static_cast<size_t>(slot) < globals_.size() ? globals_[static_cast<size_t>(slot)] : Value::null();
    }
    // ---- A script's limits (script.cpp; the scripting plan, part 4) ----
    // A limit passed (from any thread): the next safe point throws what no catch or finally of
    // the script's takes (WwThrow::limit), and so does every one after it, until clearLimit() as
    // the call it limits ends; a wait (a sleep, a lock) is woken to throw it. `what` says which.
    void limitPassed(const std::string& what);
    void clearLimit();
    bool limitWasPassed() const { return limitHit_.load(); }
    std::string limitText();
    // A count of safe points (each time round a loop, each call) the call now starting may pass;
    // -1 for none. Counted, a safe point is passed at every one, so that the count is the same
    // on every machine: with native code off (nativeCodeOff), which leaves its loops only now
    // and then.
    void countSteps(int64_t steps);
    // The heap's size past which a collection finds the limit passed (0: none). Collections
    // come before it is reached.
    void limitMemory(size_t bytes);
    // Calls deeper than `frames` are a StackOverflowError (0: as deep as the stack goes).
    void limitDepth(int frames);
    // No native code is made for this VM's functions.
    void nativeCodeOff() { nativeOff_ = true; }

    // A test's time limit, `millis` long, is kept from now on: a read made meanwhile can be
    // stopped when it passes (G45).
    void armTimeLimit(int64_t millis) { timeLimitMillis_ = millis; }
    // It passed: the next safe point throws an AssertionError saying so, and a test waiting
    // (sleep, join, a lock, the event loop, a read) is woken to throw it. Called from another
    // thread; clearTimeLimit() forgets one not yet acted on.
    void timeLimitPassed(int64_t millis) {
        timeLimitMillis_ = millis;
        timeLimitPassed_ = true;
        attention_ = true;
        wakeForTimeLimit();
    }
    void clearTimeLimit() {
        timeLimitPassed_ = false;
        timeLimitMillis_ = 0;
    }
    // Whether the limit has passed, where the AssertionError may be thrown; then throws it.
    bool timeLimitDue() const { return timeLimitPassed_.load(std::memory_order_relaxed) && keyCalls_ == 0; }
    void throwIfTimeLimitPassed() {
        if (timeLimitDue()) safePoint();
    }
    void wakeForTimeLimit();
    // A type as the program writes it: `List<Point?>`.
    std::string typeInfoText(const ObjTypeInfo* type) const;
    // Whether every value of `a` is one of `b`. A list or map read through a type is read as
    // any type its items fit (a List<Circle> is a List<Shape>), as Kotlin's read-only ones are.
    bool isSubtype(const ObjTypeInfo* a, const ObjTypeInfo* b);
    // Whether a value is of a type, its type arguments included.
    bool fitsType(const Value& v, const ObjTypeInfo* type);
    // An object of `cls` whose own type is `own` (null where it was made without one), seen as
    // one of `target`, a class it extends or an interface it implements: `Base<List<int>>` for a
    // `Pair<int>` of `class Pair<A> : Base<List<A>>()`. Null where that has no type arguments.
    const ObjTypeInfo* typeAs(ObjClass* cls, const ObjTypeInfo* own, const ObjClass* target);
    // The supertypes of `cls` written with type arguments, made for `own` (ObjClass::supertypeMakers).
    const std::vector<ObjTypeInfo*>& supertypesOf(ObjClass* cls, const ObjTypeInfo* own);
    // The types of the fields of `cls` that depend on its type arguments, or hold a value of a
    // generic value type, for `own` (null: made without type arguments), by declared index;
    // null for any other field, and all of them before the class's patterns are made (G4,
    // ObjClass::fieldTypesMaker). Runs nothing of the program: only types are made.
    const std::vector<ObjTypeInfo*>& fieldTypesOf(ObjClass* cls, const ObjTypeInfo* own);
    void makeFieldPatterns(ObjClass* cls);
    // Whether a container whose declared type for its values is `declared` keeps each value's
    // own type beside it: where `declared` says too little of a phantom type parameter (G4).
    static bool keepsItemTypes(const ObjTypeInfo* declared);
    // Writes a value's type into the slot at `at` (a pointer, null for none).
    static void writeTypeSlot(uint8_t* at, const ObjTypeInfo* type) { std::memcpy(at, &type, sizeof type); }
    static const ObjTypeInfo* readTypeSlot(const uint8_t* at) {
        const ObjTypeInfo* type;
        std::memcpy(&type, at, sizeof type);
        return type;
    }
    ObjTypeInfo* substituted(ObjTypeInfo* pattern, const ObjTypeInfo* own);
    void needPatterns(ObjClass* cls) {
        if (cls->fieldTypesMaker != nullptr && cls->patternsState.load(std::memory_order_acquire) == 0) makeFieldPatterns(cls);
    }
    const ObjTypeInfo* fieldTypeOf(ObjClass* cls, const ObjTypeInfo* own, size_t declared) {
        if (cls->fieldTypesMaker == nullptr) return nullptr;
        const std::vector<ObjTypeInfo*>& types = fieldTypesOf(cls, own);
        return declared < types.size() ? types[declared] : nullptr;
    }
    // What type a value is, as the program writes it: `List<string>` for a list made to hold strings.
    std::string valueTypeText(const Value& v) const;
    // Runs one of the instructions of types (MakeType to InstanceTypeArg); gives where the next one is.
    const uint8_t* typeInstruction(Op op, const uint8_t* ip, CallFrame* frame);
    const uint8_t* placeInstruction(Op op, const uint8_t* ip, CallFrame* frame);
    void classFinished();
    Value returnedRef(Value result, ptrdiff_t frameIndex);
    // Whether the caller of `frame` keeps what it returns where it is (Op::WithRefResult).
    bool keptAsItIs(const CallFrame* frame) {
        return (frame->closure->proto->typeArgs & kTakesDestination) != 0 && frameDestination(frame - frames_).kind == ValueKind::Undefined;
    }
    // The type a MakeType shape describes, from `at`, its classes and types taken from `operand` on.
    ObjTypeInfo* typeFromShape(const std::string& shape, size_t& at, const Value*& operand);
    // The type `any`, made once.
    ObjTypeInfo* anyType() {
        if (anyType_ == nullptr) anyType_ = typeOf(0, false, nullptr, {});
        return anyType_;
    }
    // The type List<int>, made once: for natives that give lists of places, called many times.
    ObjTypeInfo* intListType() {
        if (intListType_ == nullptr) intListType_ = typeOf(kTypeList, false, nullptr, {typeOf(kTypeInt, false, nullptr, {})});
        return intListType_;
    }
    // The type arguments of frame `index`, a kTypeArguments type, null where there are none: a
    // generic function's are those WithTypes gave its call, and a lambda's those of where it was
    // made. Kept only for a function whose FunctionProto::typeArgs says it reads them. They are
    // in the block of the frames, after them (Context).
    const ObjTypeInfo*& frameTypeArgs(ptrdiff_t index) {
        return reinterpret_cast<const ObjTypeInfo**>(frames_ + maxFrames_)[index];
    }
    // Where frame `index`'s result goes (M6.6 part 4): a place, or null; kept only for a function
    // whose FunctionProto::typeArgs has kTakesDestination. In the block of the frames, after the
    // type arguments.
    Value& frameDestination(ptrdiff_t index) {
        return reinterpret_cast<Value*>(reinterpret_cast<const ObjTypeInfo**>(frames_ + maxFrames_) + maxFrames_)[index];
    }
    // Of a function with places (kHasPlaces), set by its EnterPlaces: where in its line's
    // PlaceArena the places of frame `index` end, their first just above the end of those of the
    // frames under it; with PlaceArena::kNone added where there was no room, and it has none
    // (D149). In the block of the frames, after the destinations.
    uint32_t& framePlacesEnd(ptrdiff_t index) {
        return reinterpret_cast<uint32_t*>(reinterpret_cast<Value*>(reinterpret_cast<const ObjTypeInfo**>(frames_ + maxFrames_) + maxFrames_) +
                                           maxFrames_)[index];
    }
    uint32_t framePlacesEnd(ptrdiff_t index) const { return const_cast<VM*>(this)->framePlacesEnd(index); }
    Value pendingDestination_;  // given by WithDestination to the call that follows
    // Type argument `i` of what is in `args` (a frame's, or an object's type), or `any` where
    // that is not known.
    const ObjTypeInfo* joinTypeArgs(const ObjTypeInfo* outer, int base, const ObjTypeInfo* own);
    ObjTypeInfo* typeArgument(const ObjTypeInfo* args, int i) {
        return args != nullptr && static_cast<size_t>(i) < args->args.size() ? args->args[static_cast<size_t>(i)] : anyType();
    }
    ObjString* newString(std::string s);
    ObjString* newString(Characters text);
    ObjString* joinStrings(const ObjString* first, const ObjString* second);
    Value str(std::string s) { return Value::object(newString(std::move(s))); }
    Value str(Characters text) { return Value::object(newString(std::move(text))); }
    // The string of character `c` alone.
    Value characterString(uint32_t c) {
        Characters one;
        one.width = c < 0x100 ? 1 : c < 0x10000 ? 2 : 4;
        one.units.resize(one.width);
        if (one.width == 1) one.units[0] = static_cast<char>(c);
        else if (one.width == 2) { const uint16_t unit = static_cast<uint16_t>(c); std::memcpy(one.units.data(), &unit, 2); }
        else std::memcpy(one.units.data(), &c, 4);
        return str(std::move(one));
    }

    // ---- Running programs ----

    // Makes `program` current. May be called again after the program has grown (shell).
    void load(Program& program);

    // Runs a module's top-level code and returns its result. Throws WwThrow or WwExit.
    Value runModule(int index);
    // Calls a function of the standard library's by its name, with no arguments, once the
    // library is loaded: what a worker the library starts runs (Worker("std:__name")). An
    // IllegalStateException where it has no such function.
    Value runStdFunction(const std::string& name);

    // Calls a WinterWolf value from native code. Anything the caller holds that is not on the VM
    // stack must be protected with TempRoot / TempVector across this call.
    Value call(Value callee, int argc, const Value* args);

    // The == operator: by value, and for lists, maps and ranges by what they hold.
    bool equals(Value a, Value b, int depth = 0);
    static constexpr int kMaxEqualsDepth = 200;

    // Runs a function with `receiver` as the object it runs on: inside it, a name that is not
    // one of its variables is looked up on that object first (see Op::GetImplicit).
    Value callOn(Value receiver, Value callee, int argc, const Value* args);

    // ---- Helpers for native code ----

    WwThrow runtimeError(const std::string& message);  // a RuntimeException
    WwThrow runtimeError(Thrown kind, const std::string& message);
    // An error of a class the standard library declares (IOException, say), by its name.
    WwThrow errorOf(const char* className, const std::string& message);
    // The globals the VM itself finds by name: kept in a built program whether or not its code
    // names them (M6.5 part 5). errorOf takes no other name.
    static const std::vector<std::string>& globalsFoundByName();
    // The member names the VM and the built-in functions look up themselves (the names table,
    // and a few more): an extension function of one of them is kept in a built program.
    std::vector<std::string> membersFoundByName() const;
    WwThrow outOfMemory();
    void checkRoomFor(uint64_t bytes);
    std::string stringify(Value v);  // as print() shows it
    std::string repr(Value v);       // with strings quoted
    std::string typeName(Value v) const;

    // An int, a byte taken as one; else a ClassCastException naming `what`. Inline for the common case:
    // every method written in C++ reads its arguments with it.
    int64_t toInt(Value v, const char* what) {
        if (v.isInt() || v.isByte()) [[likely]] return v.as.i;
        notAnInt(v, what);
    }
    [[noreturn]] void notAnInt(Value v, const char* what);
    // An int handed to Windows, OpenGL or the text engine, which take 32 bits: one past their
    // range is taken as their nearest end (D76), as `int.maxValue` for "no limit" is.
    int toInt32(Value v, const char* what) {
        return static_cast<int>(std::clamp<int64_t>(toInt(v, what), INT32_MIN, INT32_MAX));
    }
    double toNumber(Value v, const char* what) {
        if (v.isByte()) return static_cast<double>(v.as.i);
        if (v.isNumber()) [[likely]] return v.toDouble();
        notANumber(v, what);
    }
    [[noreturn]] void notANumber(Value v, const char* what);
    const std::string& toStr(Value v, const char* what);
    // A string, as its characters (D20); toStr gives its UTF-8 form.
    const ObjString* toText(Value v, const char* what) {
        if (!v.isString()) toStr(v, what);  // says what it should have been
        return asString(v);
    }

    void defineGlobal(const std::string& name, Value value) { builtins_[name] = value; }
    // A built-in global by its name, as defineGlobal gave it (a safe script's VM changes some,
    // script.cpp); null where there is none.
    Value* builtinNamed(const std::string& name) {
        auto found = builtins_.find(name);
        return found == builtins_.end() ? nullptr : &found->second;
    }
    bool hasBuiltin(const std::string& name) const { return builtins_.count(name) != 0; }
    // A built-in global this VM has not (a script's has no windows, as a worker's has none), and
    // what its use throws instead of "undefined variable": a safe script's VM says it is refused.
    struct Absent {
        Thrown thrown;
        std::string message;
    };
    std::unordered_map<std::string, Absent> absentGlobals;
    // What the use of the global `name`, unset, throws.
    WwThrow undefinedGlobal(const std::string& name);
    // What the C++ provides, for `external` declarations to be checked against: whether a global
    // function, or a built-in type's method, exists, and how many arguments it takes (-1: any).
    bool builtinArity(const std::string& name, int& minArgs, int& maxArgs) const;
    // A static of a class the C++ provides (`Math.sqrt`): its own static, or null.
    ObjNative* builtinStatic(const std::string& cls, const std::string& name);
    bool methodArity(BuiltinType type, const std::string& name, int& minArgs, int& maxArgs);
    void defineNative(const std::string& name, int minArgs, int maxArgs, NativeFn fn);
    void defineMethod(BuiltinType type, const std::string& name, int minArgs, int maxArgs, NativeMethodFn fn);
    // Says what the parameters of a built-in method are called, so that it takes them by name.
    // What a built-in method's or function's parameters are called, from its declaration in
    // the standard library (D116). A name with several forms keeps the longest list.
    void nameParams(BuiltinType type, const std::string& method, const std::vector<std::string>& params);
    void nameFunctionParams(const std::string& function, const std::vector<std::string>& params);
    static bool builtinTypeNamed(const std::string& name, BuiltinType& type);
    // Everything a list, map, string, range, bytes or object with an iterator() holds, in order.
    void collect(Value iterable, std::vector<Value>& out);
    // The errors set aside for an error (Throwable.suppressed, G40), in the order they came.
    std::vector<Value> setAsideOf(Value error);
    // Where an error is being made (Throwable's stackTrace): the calls under way, innermost first,
    // as an uncaught error's are shown, leaving out the error's own constructors (the innermost
    // calls whose receiver is the object being made), as Java's fillInStackTrace does.
    Value stackHere();
    // The calls under way in Snippy files (.py, .snippy), innermost first, every one: what a
    // Snippy traceback is made of (lib/Snippy/Runtime/Exc.ww).
    Value snippyFrames();

    std::ostream* output;
    std::ostream* errorOutput;  // printError's, and Program.errors'
    std::istream* input;
    std::vector<std::string> scriptArgs;
    int mainModule_ = -1;  // the program's own file's module (Program.name)
    // That file's name without its folder and .ww: "bank" for bank.ww; "program" if none.
    std::string mainFileName() const;
    // The interfaces extends fitted a built-in type to (G30), for reflection: those that hold for
    // every value of the type as written (List<int>#: List's and List<int>'s).
    std::vector<ObjClass*> builtinInterfaces(const ObjTypeInfo* type);
    std::mt19937_64 rng;
    Names names{};

    // The temporaries of the thread that is running (see TempRoot). Each thread has its own;
    // they change places with these when the lock changes hands.
    std::vector<Value> tempRoots;

    // Values that native code needs kept alive for the rest of the program.
    std::vector<Value> pinned;
    std::vector<const std::vector<Value>*> tempVectors;

    // ---- Threads, the event loop and tasks (threads.cpp) ----
    //
    // Threads share one heap, and one of them runs WinterWolf code at a time: whichever holds the
    // lock. A thread gives the lock up at a safe point when another has waited long enough,
    // and around anything that blocks.

    // Starts a thread that calls `function`. Returns its id.
    int64_t startThread(Value function, Value self, const std::string& name, bool daemon);
    ThreadState* currentThread() const { return thread_; }
    ThreadState* findThread(int64_t id) const;
    bool onLoopThread() const { return thread_ == &mainThread_; }
    // Waits for a thread to finish; false if `timeoutMs` (negative: no limit) ran out first.
    bool joinThread(int64_t id, int64_t timeoutMs);
    void interruptThread(int64_t id);

    // Gives up the lock, with this thread's state put away, and takes it back. Between the
    // two the thread must not touch the VM or any object.
    void releaseLock();
    void acquireLock(ThreadState* thread);
    // Blocks the calling thread until unpark(), an interrupt or the time limit (negative:
    // none), with the lock released meanwhile. Wake-ups can come early: callers re-check.
    void park(int64_t timeoutMs);
    void unpark(ThreadState* thread);

    // An object used as a lock (`synchronized`), with wait and notify as Java has them.
    void monitorEnter(Value lock);
    void monitorExit(Value lock);
    bool monitorTryEnter(Value lock);
    int monitorWait(Value lock, int64_t timeoutMs);  // 0 notified, 1 timed out, 2 interrupted
    void monitorNotify(Value lock, bool all);
    Value innermostMonitor() const;
    int monitorLetGo(Value lock);
    void monitorTakeBack(Value lock, int count);
    bool monitorHeld(Value lock) const;

    // The event loop: tasks posted from any thread and timers, run on the main thread.
    void post(Value task);
    int64_t addTimer(Value task, int64_t delayMs, int64_t periodMs);
    void cancelTimer(int64_t id);
    // Something to do on the loop thread, handed over by a thread that does not hold the
    // lock (a worker finishing a read, say). `pendingJobs` counts work that will post one.
    void postNative(std::function<void(VM&)> work);
    void jobStarted() { pendingJobs_++; }
    void jobFinished() { pendingJobs_--; }
    // Runs what is ready. With `wait`, goes on until there is nothing left to wait for: no
    // task, no timer, no work in progress and no thread that is not a daemon.
    // Without `wait` it returns when nothing is ready, or, given a budget in milliseconds,
    // once that long has passed and at least one thing has been run.
    // With `until`, a function of no arguments, it returns as soon as that gives true, asked
    // before each thing it runs and as it would wait (M6.7).
    void runEventLoop(bool wait, int64_t budgetMs = -1, Value until = Value::null());
    // True if threads are still running, in which case the process has to end without
    // taking the VM apart under them.
    bool threadsAlive() const { return threads_.size() > 1 || pendingJobs_ > 0 || liveWorkers_ > 0 || liveScripts_.load() > 0; }
    // How long until the next timer is due, in milliseconds: 0 if something is ready now,
    // -1 if there is nothing to wait for. For a loop of another kind (a window's) that
    // has to wake up in time to call runEventLoop(false).
    int64_t millisUntilLoopWork();
    // Called, from whatever thread, when the loop has been given something to do.
    std::atomic<void (*)()> onLoopWake{nullptr};
    // For a program with windows, whose events come through the same loop. `loopPump`
    // handles the window system's events that are waiting, calling into the program for
    // each. `loopWait` waits, with the lock released, for one to arrive or for onLoopWake,
    // for at most that many milliseconds (-1: for as long as it takes).
    void (*loopPump)(VM&) = nullptr;
    void (*loopWait)(int64_t) = nullptr;

    // ---- Parallel sections (parallel.cpp) ----
    // Several cores running WinterWolf code on the same objects at the same time. Each core has a
    // VM of its own for the interpreter's working state, and they share the heap of the VM
    // that started the section, which waits meanwhile with its lock held. What keeps them
    // apart is ownership (Obj::core): a core looks only into what is shared or its own, and
    // changes only what is its own. Every place that looks into an object asks.

    bool inParallelSection() const { return coreId_ != 0; }
    // Throws if this core may not look into, or may not change, the object.
    void guardRead(const Obj* obj) const {
        if (obj->core != coreId_ && obj->core != 0) [[unlikely]] {
            if (readable_ == nullptr || !readable_->test(obj->core)) throw otherCores(obj);
        }
    }
    void guardRead(Value v) const {
        if (v.kind == ValueKind::Obj) guardRead(v.as.obj);
    }
    void guardWrite(const Obj* obj) const {
        if (obj->core != coreId_ || obj->frozen != 0) [[unlikely]] throw notOwned(obj);
    }
    // A frozen copy of a list, a map or a set (D26), which nobody may change; the lists, maps
    // and sets inside it are frozen copies too. One already frozen is itself.
    Value frozenCopy(Value v, int depth = 0);
    // A frozen copy's type: the lists, maps and sets it holds frozen, as the copy freezes them,
    // so that it is what the checker says frozen() gives (8.2e).
    ObjTypeInfo* frozenArgs(ObjTypeInfo* type);
    ObjTypeInfo* frozenWhole(ObjTypeInfo* type);
    // In a value's bytes laid out as `layout`, each list, map or set held given a frozen copy (G27).
    void freezeInside(uint8_t* at, const ValueLayout& layout, int depth);
    // A lazy field's, static's or global's value (D37), worked out on this first read and given
    // to `store` for its place, unless `store` is null; `what` names it for the errors. In a
    // parallel section the cores may meet here at once: the first to come works it out, shared
    // (makeShared), and the others wait for it.
    Value forceLazy(Value marker, const std::function<void(Value)>& store, const std::function<std::string()>& what);
    // Makes what is kept for every core (a lazy value, a class's description) with `make`. In a
    // parallel section, what this core makes meanwhile becomes shared as it is kept, read by
    // every core and changed by none until the section is over, as what was made before the
    // section is; so does what the pieces of a section it starts meanwhile make.
    template <class F>
    Value makeShared(F&& make) {
        if (coreId_ == 0) return make();
        Obj* const before = objects_;
        struct Making {
            uint32_t& depth;
            explicit Making(uint32_t& d) : depth(d) { depth++; }
            ~Making() { depth--; }
        } making(makingShared_);
        const Value made = make();
        for (Obj* obj = objects_; obj != before; obj = obj->next)
            if (obj->core == coreId_) obj->core = 0;
        return made;
    }
    void wakeLazyWaiters(ObjLazy* lazy);
    // A global's value where a read finds none: a lazy one's, worked out; an error otherwise.
    Value globalOrLazy(size_t index);
    // Keys (D26): a class's own == and hashCode, called for a map or a set, and the error
    // for a key that can change.
    bool hashOf(const ObjClass* cls, const Value& v, size_t& out) override;
    bool equalOf(const ObjClass* cls, const Value& a, const Value& b, bool& out) override;
    [[noreturn]] void refuseKey(const Value& key) override;
    // Says which of a type's built-in methods change the value they are called on, and which
    // built-in functions are not for a parallel section (by the object they belong to).
    void markMutating(BuiltinType type, std::initializer_list<const char*> methods);
    void markTakesPlaces(BuiltinType type, std::initializer_list<const char*> methods);
    // Op::Numeric's work (builtins.cpp, beside Math's functions): the result of `which` on
    // `args`, one value or two.
    Value numeric(uint8_t which, const Value* args);
    WwThrow overflowError(Op op, int64_t a, int64_t b);
    void markNotParallel(const std::string& global, std::initializer_list<const char*> except = {});

    // Runs `function` on every item of `source` (a list, a range, anything a loop takes), the
    // items dealt out to the cores. With `keep`, gives the list of what it returned, in order.
    // With `replace`, each item of the list `source` is replaced by what the function gives for it.
    Value runParallel(Value source, Value function, bool keep, bool replace = false);
    // Runs `function` on each of `parts` ranges that together cover a `bytes`, one core to
    // a range, each allowed to write that part of it.
    void runParallelChunks(Value buffer, Value function, size_t align = 1);
    int parallelCores();
    // The core of a parallel section this VM is (1 and up), or 0 for a program's own.
    uint8_t coreId() const { return coreId_; }
    // parallel({ a }, { b }) (D96): each function at once, on whatever cores are free; what they
    // give, in order.
    Value runParallelFunctions(const std::vector<Value>& functions);
    // gather (D93): for each piece of the items, `into` makes a result and `each` adds an item
    // to it; the pieces' results, in order.
    Value runParallelGather(Value source, Value into, Value each);
    // Runs a section from this VM, the program's or a core's, and waits for its pieces.
    void runSection(ParallelSection& section);
    // One piece of a section, run on this core's VM.
    void runParallelPiece(ParallelPiece& piece);

    // ---- Workers (worker.cpp) ----
    // A worker is a second VM on a thread of its own, with a heap and a lock of its own, so
    // it runs at the same time as this one, on another core. The two share nothing and
    // exchange messages, which are copied.

    // The program as bytes, for a worker to load its own copy from. Set by whoever loaded it.
    std::function<std::string()> programImage;
    // In a worker: the link to the program that started it.
    std::shared_ptr<WorkerLink> workerLink;
    // In a worker: the object its messages are delivered to (Worker.parent).
    Value workerSelf;
    // The jobs that failed with nothing asking for their errors (8.7a): each job's error, the
    // job held weakly, so that one nothing can ask any more is reported once it is collected, and
    // the others as the program ends. `unaskedDeliver` reports one (__unaskedFailed: to
    // Program.onUncaughtError, or as an error nobody caught).
    std::unordered_map<Obj*, Value> unaskedFailures;
    std::vector<Value> freedFailures;  // their jobs collected, to be reported on the event loop
    Value unaskedDeliver = Value::null();
    // Where the error of a thread { }'s job was thrown, kept for its report while nothing has
    // asked for it, by the error.
    std::unordered_map<Obj*, std::vector<std::string>> failureTraces;
    // What the program reaches outside itself (host.h, M6.6.2): the system's, unless a sandbox's
    // was given (a worker's, started in one). The code running now may be in a sandbox of its
    // own: currentHost() is what each of its operations goes to.
    std::shared_ptr<Host> host = std::shared_ptr<Host>(&realHost(), [](Host*) {});
    Host& currentHost() { return context_ != nullptr && context_->host ? *context_->host : *host; }
    std::shared_ptr<Host> currentHostShared() { return context_ != nullptr && context_->host ? context_->host : host; }
    // Sandbox.run: the code running now reaches `entered` until leaveHost.
    void enterHost(std::shared_ptr<Host> entered) {
        context_->hostsBefore.push_back(context_->host);
        context_->host = std::move(entered);
    }
    void leaveHost() {
        if (context_->hostsBefore.empty()) return;
        context_->host = std::move(context_->hostsBefore.back());
        context_->hostsBefore.pop_back();
    }
    // A new thread's or task's: the host of the code that starts it, if it is not the VM's own.
    void inheritHost(Context& made) {
        if (context_ != nullptr) made.host = context_->host;
    }
    // Program.onUncaughtError, as the program set it: what takes the error that ends a thread,
    // or a job's nothing asked for; null while there is none.
    Value uncaughtHandler = Value::null();
    // An error nobody caught was reported, in a thread or a job, with no handler to take it: the
    // program's exit code is then 70, however the rest of it ends.
    bool uncaughtReported = false;
    bool deliveryPosted = false;
    void deliverUnasked(bool atEnd);

    // ---- Frozen data shared with workers (D97, worker.cpp) ----
    // The blocks outside every heap this VM holds, and what of its own frozen data was shared as
    // which. A collection notes the blocks it reaches; those it does not are let go.
    std::unordered_set<SharedBlock*> heldBlocks;
    std::unordered_set<SharedBlock*> reachedBlocks;
    std::unordered_map<const Obj*, SharedBlock*> sharedAs;
    void noteOutside(const Obj* obj);
    void releaseUnreachedBlocks();
    void releaseAllBlocks();
    // The Worker objects of the workers this VM has started, by id.
    std::unordered_map<int64_t, Value> workerObjects;
    // This VM's objects and functions handed to the other side of a script as handles
    // (script.cpp, the scripting plan's part 2): by id, each with the run it was handed over in,
    // and kept from the collector until that run ends; and the id each object has, by run, so
    // that the same object goes as the same handle.
    struct ScriptHandle {
        Value value;
        const void* run;
    };
    std::unordered_map<int64_t, ScriptHandle> scriptHandles;
    std::unordered_map<const void*, std::unordered_map<const Obj*, int64_t>> scriptHandleIds;
    int64_t nextScriptHandle = 1;
    // The class a global of this name holds (the last such global), or null.
    ObjClass* classNamed(const std::string& name);
    // The value of the first global of this name, or null.
    Value globalNamed(const std::string& name);
    std::unordered_map<int64_t, std::shared_ptr<WorkerLink>> workerLinks;
    int64_t nextWorkerId = 1;
    void workerStarted() { liveWorkers_++; }
    void workerEnded() { liveWorkers_--; }
    // The scripts this VM started whose threads have not ended (script.cpp): counted from those
    // threads, as each ends.
    void scriptStarted() { liveScripts_++; }
    void scriptEnded() { liveScripts_--; }
    // Asks this VM, from another thread, to stop what it is doing: every one of its threads
    // gets a WorkerStop at its next safe point.
    void requestStop();
    // For long work inside a built-in function (a regex match): throws what a safe point would
    // for a stop asked for or a time limit passed, and nothing else.
    void throwIfStopped() {
        if (stopRequested_.load(std::memory_order_relaxed)) throw WorkerStop{};
        throwIfTimeLimitPassed();
    }

    // For a built-in function that ends by calling a WinterWolf function: sets that call going
    // in its own place, so that the function runs as part of the program rather than inside
    // the built-in (where it could not be set aside). The built-in returns what this returns.
    // `receiver` becomes the object the function runs on.
    Value tailCallOn(Value receiver, Value function);

    // Tasks.
    ObjFiber* currentFiber() const { return context_->fiber; }
    // Starts or takes up a task, giving it `value` as the result of what it was waiting for
    // (or, with `ok` false, raising it there as an error). Returns when the task finishes or
    // is set aside again; an error the task does not catch comes out of this call.
    void runFiber(ObjFiber* fiber, bool ok, Value value);
    // Whether a built-in function called now could set the running task aside.
    bool canSuspend() const { return context_->fiber != nullptr && !context_->fiber->generator && canSetAside(); }
    // Whether what runs now could be set aside: no built-in function is calling back into it.
    bool canSetAside() const { return nativeDepth_ == 0 && nativeBase_ != nullptr; }
    // Sets the running task aside: does not return. The task goes on when runFiber is called.
    [[noreturn]] void suspendFiber();

    // Runs `work`, which blocks and must not touch the VM, and gives what `finish` makes of
    // it. In a task on the loop thread the task is set aside while a worker thread does the
    // work, and the loop stays live; anywhere else the calling thread waits, with the lock
    // released. A built-in function has to return what this returns straight away.
    Value blocking(std::function<void()> work, std::function<Value(VM&)> finish);

    // Called for an error that nothing caught on a thread other than the main one.
    std::function<void(VM&, WwThrow&, const std::string& threadName)> onThreadError;
    // How a job's error nothing asked for is reported where the program has no handler (8.7a).
    std::function<void(VM&, WwThrow&)> onUnaskedError;
    // Called when exit() is used on such a thread.
    std::function<void(int code)> onThreadExit;

private:
    static constexpr size_t kStackSize = 1 << 20;
    static constexpr int kMaxFrames = 20000;
    static constexpr size_t kThreadStackSize = 1 << 18;
    static constexpr int kThreadMaxFrames = 8000;
    static constexpr int kMaxNativeDepth = 200;
    static constexpr int kMaxTaskNesting = 60;
    // The heap a collection first waits for, and the least it waits for after: 16 MB, so that a
    // program building a large structure (an XML document's tree, say) is not marked again and
    // again while it grows (the user, 2026-11-08; it was 4 MB).
    static constexpr size_t kMinHeap = 16 * 1024 * 1024;

    // Execution state: that of the context that is running (see Context).
    Value* stack_;
    Value* stackEnd_;
    Value* sp_;
    CallFrame* frames_;
    int frameCount_ = 0;
    int maxFrames_ = 0;
    int frameLimit_ = 0;
    // Whether the stack has room up to `top`: it has, or it grew to have it (Context::grow).
    bool roomFor(const Value* top) { return top <= stackEnd_ || growContext(top, frameCount_); }
    bool growContext(const Value* top, int frames);
    std::vector<Handler> handlers_;
    ObjUpvalue* openUpvalues_ = nullptr;
    int nativeDepth_ = 0;
    Value* nativeBase_ = nullptr;  // under the arguments of the built-in function being called
    Context* context_ = nullptr;
    std::vector<Value> namedScratch_;  // see callNamed

    void saveContext();
    void loadContext(Context* context);
    void markContext(const Context& context);

    // Threads
    // A worker's and a script's (VM(true)) grows as it is used, reserved whole but committed a page
    // at a time, as a task's does: a script that needs little takes little (2026-11-19: 18 MB a
    // script VM before, committed whole). The program's own is made whole.
    ThreadState mainThread_;
    ThreadState* thread_ = &mainThread_;  // the one holding the lock
    std::vector<ThreadState*> threads_;   // all that are running, the main one first
    int64_t nextThreadId_ = 1;
    std::mutex lockMutex_;
    std::condition_variable lockFree_;
    std::condition_variable lockTaken_;
    bool lockHeld_ = true;
    uint64_t lockSwitches_ = 0;
    std::atomic<int> lockWaiters_{0};
    std::atomic<bool> attention_{false};  // a safe point has something to do besides collecting
    bool threadStress_ = false;           // WW_THREAD_STRESS: change threads at every safe point
    void safePoint();
    void yieldLock();
    void threadMain(ThreadState* thread);

    struct Monitor {
        ThreadState* owner = nullptr;
        int count = 0;
        std::vector<ThreadState*> entering;
        std::vector<ThreadState*> waiting;
    };
    std::unordered_map<const Obj*, Monitor> monitors_;
    Monitor& heldMonitor(Value lock, const char* what);

    // The event loop
    struct Timer {
        int64_t id;
        std::chrono::steady_clock::time_point due;
        int64_t periodMs;
        Value task;
        // One set in a sandbox (M6.6.2): due by its clock, in its nanoseconds, not by `due`.
        std::shared_ptr<Host> sandbox;
        int64_t sandboxDue = 0;
    };
    bool runSandboxTimer();
    std::deque<Value> loopTasks_;
    std::vector<Timer> timers_;
    int64_t nextTimerId_ = 1;
    int pendingJobs_ = 0;
    std::mutex loopMutex_;
    std::condition_variable loopCv_;
    bool loopWake_ = false;
    std::deque<std::function<void(VM&)>> nativeTasks_;
    void wakeLoop();
    std::vector<ObjFiber*> waitingFibers_;  // tasks set aside until work in progress is done
    bool isWorker_ = false;
    int liveWorkers_ = 0;
    std::atomic<int> liveScripts_{0};

    // Parallel sections
    VM* root_ = nullptr;           // for a core's VM: the VM whose objects it works on
    // Which piece of work this VM is running, by the number what the piece owns is marked with;
    // 0 for a VM that is not a core's. A core's runs one piece after another, and pieces inside
    // pieces (D96): each reads, besides its own and what is shared, what `readable_` holds,
    // the numbers of the pieces that started its section.
    uint8_t coreId_ = 0;
    // Above 0 while this core makes something shared (makeShared): what the pieces of a section
    // it starts make is shared when they end, not its own.
    uint32_t makingShared_ = 0;
    const std::bitset<256>* readable_ = nullptr;
    ParallelSection* section_ = nullptr;   // the section of the piece running
    uint64_t seenGeneration_ = 0;          // the outermost section it last took the program's state for
    int coreIndex_ = -1;                   // which core's VM this is
    std::shared_ptr<ParallelPool> parallelPool_;
    std::mutex internMutex_;       // cores intern strings in the root's table
    std::atomic<uint64_t> classesFinished_{0};  // the root's: classes made so far (VM::layInline)
    uint64_t extensionEpoch_ = 0;  // goes up when an extension function is added
    // In a section over the parts of a buffer, or over a list of values (M6.6 part 4): the bytes
    // or the list this core may write the part of, and that part. Of such a list it reads only
    // its own part too, which no other core is writing.
    const Obj* bandObject_ = nullptr;
    size_t bandFrom_ = 0, bandTo_ = 0;
    WwThrow outsideBand(size_t i) const;
    WwThrow wholeBand() const;
    // A built-in function or method given the list of values a section is over, which would
    // read every core's items: refused, but for one that reads none of them.
    void guardBand(Value v, const std::string& method = "") const {
        if (v.isObj() && v.as.obj == bandObject_ && bandObject_->type == ObjType::List && method != "isEmpty" && method != "isNotEmpty")
            [[unlikely]] throw wholeBand();
    }
    void refreshFromRoot();
    void adoptObjects(VM& core);
    WwThrow otherCores(const Obj* obj) const;
    WwThrow notOwned(const Obj* obj) const;
    WwThrow notOwned(const std::string& what) const;
    void guardNative(const Value* args, int argc) const {
        for (int i = 0; i < argc; i++) {
            guardRead(args[i]);
            guardBand(args[i]);
        }
    }
    friend struct ParallelPool;
    std::atomic<bool> stopRequested_{false};
    std::atomic<bool> timeLimitPassed_{false};
    std::atomic<int64_t> timeLimitMillis_{0};
    // A script's limits (limitPassed and the rest, above).
    std::atomic<bool> limitHit_{false};
    std::mutex limitMutex_;
    std::string limitText_;
    int64_t stepsLeft_ = -1;
    int64_t stepsGiven_ = 0;
    size_t memoryLimit_ = 0;
    std::string memoryLimitText() const;
    int depthLimit_ = 0;
    bool nativeOff_ = false;
    // Whether a safe point has work besides collecting, kept up while it has (a stop, a limit,
    // steps counted).
    bool wantsAttention() const { return stopRequested_.load() || timeLimitPassed_.load() || stepsLeft_ >= 0 || sampleDue_.load(); }
public:
    // ww --profile's clock asks for a sample: the next safe point hands the calls under way to
    // the profiler (profile.cpp). Called from the clock's thread.
    void sampleWanted() {
        sampleDue_ = true;
        attention_ = true;
    }
private:
    std::atomic<bool> sampleDue_{false};
    void takeSample();

    // Worker threads for work that blocks (see blocking()).
    struct Workers;
    std::shared_ptr<Workers> workers_;

    // Program state
    Program* program_ = nullptr;
    std::vector<Value> globals_;

    // Declared type of each global variable; `type` 0 means untyped.
    struct TypeConstraint {
        uint8_t type = 0;
        ObjClass* cls = nullptr;  // for kTypeInstance
    };
    std::vector<TypeConstraint> globalTypes_;
    std::vector<bool> loaded_;
    std::unordered_map<std::string, Value> builtins_;
    std::unordered_map<const ObjString*, NativeMethod> methods_[static_cast<int>(BuiltinType::Count_)];
    // The methods of methods_ found last, by name and type, so that a call in a loop does not
    // look in the table each time. Only what was found is kept: a name kept is a key of the
    // table, so it lives as long as the VM, and its place cannot be some other string's.
    struct NativeCacheEntry {
        const ObjString* name = nullptr;
        const NativeMethod* method = nullptr;
        BuiltinType type = BuiltinType::Count_;
    };
    mutable NativeCacheEntry nativeCache_[256] = {};
    const NativeMethod* cachedNative(BuiltinType type, const ObjString* name) const {
        NativeCacheEntry& entry = nativeCache_[((reinterpret_cast<uintptr_t>(name) >> 4) ^ static_cast<uintptr_t>(type) * 37) & 255];
        if (entry.name == name && entry.type == type) return entry.method;
        const auto& table = methods_[static_cast<int>(type)];
        auto it = table.find(name);
        if (it == table.end()) return nullptr;
        entry = NativeCacheEntry{name, &it->second, type};
        return entry.method;
    }
    ObjClass* errorClass_ = nullptr;
    ObjClass* thrownClasses_[static_cast<size_t>(Thrown::Count_)] = {};
    ObjClass* mapEntryClass_ = nullptr;  // MapEntry, from the standard library: what a map's entries are
    Value classOfFn_ = Value::null();    // Class, from the standard library: what a type is made into (D162)
  public:
    // A type as the program sees it (D162): the Class the standard library makes of it.
    Value classValue(ObjTypeInfo* type);
  private:
    ObjClass* viewClass_ = nullptr;      // ListView, from the standard library: what splitAt hands out (D96)
    ObjClass* imageCanvasClass_ = nullptr;  // ImageCanvas, from the standard library: what canvas.split() hands out (G64)
    ObjString* opNames_[static_cast<int>(Op::Count_)] = {};

    // Heap state
    Obj* objects_ = nullptr;
    size_t bytesAllocated_ = 0;
    size_t nextGC_ = kMinHeap;
    bool gcStress_ = false;  // WW_GC_STRESS: collect at every safe point, to expose rooting bugs
    // A class's own == or hashCode running for a map or a set (D26): the C++ around it holds
    // objects nothing roots (a map being built, its entries), so meanwhile nothing is collected
    // and the lock is not handed to another thread.
    int keyCalls_ = 0;
    friend struct KeyCall;
#if WW_TAIL_CALLS
    friend struct TailTable;
    // Instruction `op` (instructions.inc), and an R instruction's slow ways: its two operands
    // (left in slowB_ and slowC_) put where the stack would have them, and on to its stack code,
    // or into stack instruction `plain`'s code.
    template <Op op> WW_TAIL_FN int tail(WW_TAIL_PARAMS);
    template <int sAt, int slowAt> WW_TAIL_FN int slowTwo(WW_TAIL_PARAMS);
    template <Op plain> WW_TAIL_FN int plainTwo(WW_TAIL_PARAMS);
    // An instruction's rare part (COLD in instructions.inc): a function apart, so that the part
    // that runs makes no call. Built for Windows, a function that calls anything at all sets up
    // its stack frame as it starts, whichever way it then goes.
    template <Op op> WW_TAIL_FN int cold(WW_TAIL_PARAMS);
    // The way into a native code region (M9.5, src/native/regions.cpp): the code holds its
    // address before the first instruction of the loop the region was made of.
    WW_TAIL_FN int nativeEnter(WW_TAIL_PARAMS);
    WW_TAIL_FN int nativeFirst(WW_TAIL_PARAMS);
    // A whole function's region (2026-11-15): its calls counted at its start, then made at a
    // return, where the frame holds what its locals are.
    WW_TAIL_FN int nativeCall(WW_TAIL_PARAMS);
    WW_TAIL_FN int nativeReturn(WW_TAIL_PARAMS);
    // A value of a value type in a box, made for a region (RegionContext::makeBox): its bytes
    // zero; null where a collection is due (the region leaves the making to the VM).
    static void* boxForRegion(void* vm, const void* cls);
    static bool roomForRegion(void* vm, const void* list);
    static int findForRegion(const void* table, const void* key, void* out);
    static int setForRegion(void* vm, const void* table, const void* key, const void* value);
    static bool plainKeyForRegion(const Value& k);
    // Whether a bare name in function `f` is no member of `receiver`, as implicitReceiver would
    // find it, by its class alone (NativeSeen::noMember).
    static bool noMemberForRegion(const void* vm, const FunctionProto& f, const ObjString* name, const Value& receiver);
    // Native code made on another core (2026-11-17, background-plan.html): what the program holds
    // now for `f`'s regions, and their making handed on or done here (vm.cpp's Builder).
    std::unique_ptr<NativeSeen> seenForRegions(const FunctionProto& f, const Value* base, const Value* top);
    void makeRegionsFor(FunctionProto& f, uint32_t head, size_t which, std::unique_lock<std::mutex>& states, const Value* base, const Value* top);
    bool remakeCounted(FunctionProto& f, const FunctionProto::NativeRegion& region, const Value* base, const Value* top);
    const Value* slowB_ = nullptr;
    const Value* slowC_ = nullptr;
#endif
    // The builds this VM asked the builder for, not made yet: what they read kept by the collector;
    // dropped as the VM ends.
    void markWaitingBuilds();
    void waitForBuilds();  // until no build this VM asked for waits or is under way
    void dropWaitingBuilds();
public:
    // Run code's words written by a thread that may not write them (a section's core, the
    // builder), waiting for the program's own VM, the only one then running its code (vm.cpp's
    // writeRunWord): written at its next safe point, or as its parallel section ends.
    void writeWaitingWords();
    // A function's whole region made at a return of it, counted (VM::nativeCall).
    void wholeAtReturn(FunctionProto& f, const Value* base, const Value* top);
    // Whether this VM is the program's own and running no section's pieces itself: while it runs,
    // no other thread runs the program's code (its threads take turns with it; a section's cores
    // run while it waits for them).
    bool runsProgramAlone() const { return root_ == nullptr && coreId_ == 0; }
    VM& programVM() { return root_ != nullptr ? *root_ : *this; }
    // Some written: a safe point of the program's VM writes them.
    void wordsWaiting() {
        wordsWaiting_ = true;
        attention_ = true;
    }
private:
    std::atomic<bool> wordsWaiting_{false};
    size_t liveBytes_ = 0;
    std::vector<Obj*> gray_;
    // A name's hash eight bytes at a time: the library's own goes a byte at a time on Windows,
    // and a program's start interns every name it holds (a quarter of reading a kept program in,
    // 2026-11-09).
    struct NameHash {
        size_t operator()(std::string_view s) const noexcept {
            uint64_t h = 0x9E3779B97F4A7C15ull ^ s.size();
            const char* p = s.data();
            size_t n = s.size();
            for (; n >= 8; p += 8, n -= 8) {
                uint64_t w;
                std::memcpy(&w, p, 8);
                h = (h ^ w) * 0xFF51AFD7ED558CCDull;
                h ^= h >> 32;
            }
            uint64_t w = 0;
            std::memcpy(&w, p, n);
            h = (h ^ w) * 0xC4CEB9FE1A85EC53ull;
            return static_cast<size_t>(h ^ (h >> 29));
        }
    };
    std::unordered_map<std::string_view, ObjString*, NameHash> interned_;
    // Private names (privateName), by owner, a tab and text. Under internMutex_ in the root VM.
    std::unordered_map<std::string, ObjString*> privateNames_;
    std::unordered_map<std::string, ObjTypeInfo*> types_;  // by what they are made of (typeOf)
    ObjTypeInfo* anyType_ = nullptr;
    ObjTypeInfo* intListType_ = nullptr;
    // The type arguments WithTypes gave for the call that follows it, which takes them: the
    // frame it pushes, or the object it makes. Cleared by a call of C++, which has none.
    const ObjTypeInfo* pendingTypeArgs_ = nullptr;
    std::unordered_map<const ObjClass*, int> classSlots_;  // globalSlotOf's, found once
    // What MakeType made last at a place, from which types (G3): each VM's own, a core's in a
    // parallel section, so read and written with no lock.
    struct MadeTypeCache {
        static constexpr int kMost = 3;
        const uint8_t* site = nullptr;
        uint64_t age = 0;  // the collections there had been: code freed since may be at the same place
        const Obj* operands[kMost] = {};
        ObjTypeInfo* made = nullptr;
    };
    static constexpr size_t kMadeTypeCaches = 64;
    MadeTypeCache madeTypes_[kMadeTypeCaches];
    uint64_t collections_ = 0;  // how many there have been: a MadeTypeCache is good for one age

    Value execute(const FunctionProto* proto);
    void run(int baseFrame);
    void resetState();

    // `packed`: the arguments are already arranged one per parameter (see callNamed).
    bool callValue(Value callee, int argc, bool packed = false);
    void callClosure(ObjClosure* closure, int argc, bool packed = false);
    ObjClosure* chooseForm(const ObjForms* set, const Value* args, int positional, const Value* pairs, int named);
    // The operator added from outside to the type of `left` (op Number.*) that takes `*right`
    // on its right, or null.
    const Value* addedOperator(Value left, const Value* right, const ObjString* symbol);
    bool formTakes(const FunctionProto* proto, const Value* args, int positional, const Value* pairs, int named,
                   int& score, int& defaultsLeft) const;
    int paramFit(Value v, uint8_t type, const ObjString* className) const;
    ObjString* emptyName_ = nullptr;  // the name a lambda written after a call travels under
    void callTrailing(ObjClosure* closure, int argc);
    bool callValueTrailing(Value callee, int argc);
    int packVarargs(const FunctionProto* proto, int argc);

    // Bare names passed as arguments that may be enum entries (ValueKind::EnumName), not yet
    // settled. While there are none, calls need not look for them.
    int pendingEnums_ = 0;
    WwThrow unsettledName(Value pending);
    void rejectPendingNames(const Value* args, int argc);
    Value settleEnumName(Value pending, const ObjClass* type);
    bool callSpread(int named);
    bool invoke(ObjString* name, int argc, bool trailing = false);
    bool callNativeNamed(int positional, int named);
    int scopeFunction(ObjString* name, int argc);
    bool callNamed(int positional, int named);
    void invokeOperator(Value receiver, ObjString* name, int argc);
    Value callNative(const std::string& name, int minArgs, int maxArgs, int argc);
    void checkArity(const std::string& name, int minArgs, int maxArgs, int argc);

    Value callWithReceiver(Value receiver, ObjClosure* method);
    Value implicitReceiver(const CallFrame* frame, const Value* base, const ObjString* name) const;
    // What ImplicitFirst gave the next lookup of a bare name (D58, D59), for the running
    // context: saved with it when threads take turns (saveContext).
    mutable Value implicitFirst_[8];
    mutable uint8_t implicitFirstCount_ = 0;
    mutable bool implicitOnly_ = false;
    mutable uint8_t implicitTried_ = 0;  // how many of them the last lookup tried, for its error
    // Bare names found not to be members of the object a function runs on (2026-11-15: `Color`,
    // `Rect` in a method, each looked for among the object's members, extends and the functions
    // added to its type, some 25 ns): by the function, the name and the object's class, as long
    // as no Table has gained or lost a key since (tableKeysChanged).
    struct ImplicitMiss {
        const FunctionProto* proto = nullptr;
        const ObjString* name = nullptr;
        const ObjClass* cls = nullptr;
        uint64_t keys = 0;
    };
    mutable ImplicitMiss implicitMisses_[256];
    void setAccess(ObjClass* cls, Table& marks, ObjString* name, bool write, const std::vector<uint16_t>& run, int kind);
    void declareAccess(ObjClass* cls, Table& marks, ObjString* name, int id, int kind, bool write);
    void joinProtected(ObjClass* subclass);
    bool accessDeniedTo(Value target, const ObjString* name, const std::vector<uint16_t>* ids, bool write) const;
    bool memberDeniedTo(const ObjClass* cls, const ObjString* name, const std::vector<uint16_t>* ids, bool write) const;
    bool rightsAllow(Value target, const ObjString* name, const FunctionProto* proto, const uint8_t* ip, bool write) const;
    bool accessDeniedAt(Value target, const ObjString* name, const FunctionProto* proto, bool write, const uint8_t* ip) const;
    WwThrow accessError(Value target, const ObjString* name, const FunctionProto* proto);
    WwThrow undefinedName(const CallFrame* frame, const Value* base, const ObjString* name, size_t global);
    void ensureClassInit(ObjClass* cls);
    bool nestedThrough(ObjInstance* instance, ObjString* name, Value& out);

public:
    // One entry of a map, as `map.entries` gives it: an object with a key and a value.
    Value makeEntry(Value key, Value value);
    Value mapEntries(ObjMap* map);
    // The type of a list of a map's keys (0), values (1) or entries (2), from the map's own; null where it has none.
    ObjTypeInfo* mapPartType(const ObjMap* map, int part);
    Value mapKeys(ObjMap* map);
    Value mapValues(ObjMap* map);

private:
    const Value* superMethod(Value super, Value self, const ObjString* name) const;

    // Extension functions on the built-in types, by TypeId; on null; and on every value.
    Table typeExtensions_[kTypeCount_];
    Table nullExtensions_;
    Table nullExtensionOwners_;  // for each of those, the type it was declared on
    Table anyExtensions_;
    bool hasExtensions_ = false;
    void addExtensionForm(Table& table, ObjString* name, Value function);
    std::unordered_map<std::string, std::unique_ptr<FunctionProto>> extensionSets_;  // the protos of their sets, by name
    const Value* findExtension(Value receiver, const ObjString* name) const;
    template <bool InReach>
    const Value* findExtensionOf(Value receiver, const ObjString* name) const;
    // Whether an extension function, or one of a name's forms, is in reach of the function
    // running (G31, D188): of its file's folder, of its file, of a folder its file imports, or
    // the standard library's. A call that names a form whole is given it all the same.
    bool inReach(const FunctionProto* form) const {
        if (form->library <= kEverywhere || frameCount_ == 0) return true;
        const FileReach* reach = frames_[frameCount_ - 1].closure->proto->reach;
        return reach == nullptr || reach->has(form->library);
    }
    // What a table has under a name, if any: the standard library's and one form alone are
    // told here, as most are; a name's forms and others' by inReachSlow.
    bool inReach(const Value* found, const ObjString* name) const {
        if (found == nullptr) return false;
        if (!found->isObjType(ObjType::Closure)) return true;
        const FunctionProto* proto = asClosure(*found)->proto;
        if (!proto->chooser) return inReach(proto) || inReachSlow(name);
        return inReachSlow(found, name);
    }
    bool inReachSlow(const Value* found, const ObjString* name) const;
    bool inReachSlow(const ObjString* name) const;
    bool hasMember(Value candidate, const ObjString* name) const;

    // `extend string : Sized`, `extend List<int> : Summable` (D33, G30): the interfaces an
    // extend fitted a built-in type to, by the type's id. Each holds the type as written, which
    // for List<int> asks what a list holds and for List alone takes every list; the interface
    // or one above it; and the extend's getters bridging names. A built-in value has no class
    // to keep these in, so is, as, typed collections and members ask here.
    struct BuiltinFit {
        ObjTypeInfo* type = nullptr;
        ObjClass* iface = nullptr;
        Table getters;
        Table methods;  // the extend's funs (D186)
    };
    std::deque<BuiltinFit> builtinFits_[kTypeCount_];  // a deque: a fit stays where it is as more come
    bool hasBuiltinFits_ = false;
    BuiltinFit* extending_ = nullptr;  // the fit an extend being made is giving its getters to
    static uint8_t builtinIdOf(const Value& v);
    bool fitHolds(const BuiltinFit& fit, const Value& v);
    // Whether a built-in value, or a built-in type, fits an interface by an extend.
    bool builtinFitsClass(const Value& v, const ObjClass* target);
    bool builtinTypeFitsClass(const ObjTypeInfo* type, const ObjClass* target);
    // What an extend brought a built-in value: a bridging getter, the interface's own getter
    // or method; null where none did.
    const Value* builtinFitGetter(const Value& v, const ObjString* name);
    const Value* builtinFitMethod(const Value& v, const ObjString* name);
    void declareEntryFields(ObjClass* cls);
    void nameEntry(ObjInstance* entry, ObjString* name, int ordinal);
    void checkFieldAssignment(ObjInstance* instance, ObjString* name, Value value);
    void checkStaticType(ObjClass* owner, FieldDecl& field, Value value);
    void assignStatic(ObjClass* cls, ObjString* name, Value value);
    void checkRequiredFields(ObjInstance* instance, const ObjClosure* init);
    Value getProperty(Value obj, ObjString* name);
    void setProperty(Value obj, ObjString* name, Value value);
    Value getIndex(Value obj, Value index);
    void setIndex(Value obj, Value index, Value value);
    bool binarySlow(Op op);

    bool numberArithmetic(Op op);
    bool compareSlow(Op op);
    bool shift(Op op);
    bool bitwise(Op op);

    bool matchesType(const Value& v, uint8_t type, const ObjClass* cls) const;
    // A byte kept where an int or a Number is declared becomes that int (8.9c): asked only once
    // the value has failed its type's check, so that the checks that pass cost nothing more.
    // True where it was widened, and so now fits.
    static bool widenedByte(Value& v, uint8_t type);
    ObjClass* popTypeClass(uint8_t type);
    std::string declaredTypeText(uint8_t type, const ObjClass* cls) const;
    WwThrow typeMismatch(const std::string& variable, uint8_t type, const ObjClass* cls, Value v);
    const NativeMethod* findNativeMethod(Value receiver, const ObjString* name) const;
    // A method written in C++ of the string, list, map, set or bytes below `argc` arguments
    // at `args`, where it may be called at once: on this core's own value, with the arguments
    // it takes, none of them needing a box or the first an object (the standard library's form
    // in WinterWolf), and nothing waiting to be checked. Null where invoke() must see to the call.
    const NativeMethod* quickNative(Value receiver, const ObjString* name, int argc, const Value* args) const {
        if (receiver.kind != ValueKind::Obj || coreId_ != 0 || pendingEnums_ > 0) return nullptr;
        const Obj* obj = receiver.as.obj;
        BuiltinType type;
        switch (obj->type) {
            case ObjType::String: type = BuiltinType::String; break;
            case ObjType::List: type = BuiltinType::List; break;
            case ObjType::Map: type = BuiltinType::Map; break;
            case ObjType::Set: type = BuiltinType::Set; break;
            case ObjType::Bytes: type = BuiltinType::Bytes; break;
            default: return nullptr;
        }
        if (obj->core != 0) return nullptr;
        const NativeMethod* method = cachedNative(type, name);
        if (method == nullptr || argc < method->minArgs || (method->maxArgs >= 0 && argc > method->maxArgs)) return nullptr;
        if (method->mutates && obj->frozen != 0) return nullptr;
        if (argc >= 1 && args[0].isObjType(ObjType::Instance)) return nullptr;
        if (!method->takesPlaces)
            for (int i = 0; i < argc; i++)
                if (args[i].kind == ValueKind::Ref || args[i].kind == ValueKind::Small) return nullptr;
        return method;
    }
    // An RInvoke's cache (instructions.inc): the C++ method called there last, for a receiver of
    // one type, in 64 bits: the function (in the low 48, where every function of the program is),
    // the receiver's ObjType + 1 (6 bits), whether the method changes its receiver and whether it
    // takes places. Kept apart from the VM's tables, so that it means the same in any VM; 0 where
    // it cannot be kept. `argc` is the place's, which never changes.
    static uint64_t nativeCacheEntry(const NativeMethod* method, ObjType type) {
        const uint64_t fn = reinterpret_cast<uint64_t>(method->fn);
        if ((fn >> 48) != 0 || static_cast<int>(type) >= 63) return 0;
        return fn | (static_cast<uint64_t>(static_cast<int>(type) + 1) << 48) | (uint64_t{method->mutates} << 54) | (uint64_t{method->takesPlaces} << 55);
    }
    Value makeIterator(Value iterable);
    bool iteratorNext(ObjIterator* it, Value& out);

    ObjUpvalue* captureUpvalue(Value* slot);
    void closeUpvalues(Value* last);

    bool tryHandle(Value exception, int baseFrame);
    void unwindTo(int baseFrame);
    std::vector<std::string> captureTrace() const;
    std::vector<std::string> captureTraceFrom(int top) const;

    void stringifyInto(std::string& out, Value v, bool quote, int depth);

    void collectGarbage();
    void markValue(const Value& v) {
        if (v.kind == ValueKind::Obj) markObject(v.as.obj);  // a Small holds no object
        else if (v.kind == ValueKind::Ref) v.as.obj->type == ObjType::Places ? markPlace(static_cast<const ObjPlace*>(v.as.obj)) : markObject(v.as.obj);
        else if (v.kind == ValueKind::Undefined && v.as.obj != nullptr) markObject(v.as.obj);  // a lazy one's ObjLazy
    }
    void markObject(Obj* obj);
    void markPlace(const ObjPlace* place);  // what a frame's place holds (D149)
    void markTable(const Table& table);
    void blacken(Obj* obj);
    void sweep();
};

// For a built-in function that is about to block: gives up the lock for as long as this
// object lives. Nothing of the VM, and no object, may be touched meanwhile.
class LockReleased {
public:
    explicit LockReleased(VM& vm) : vm_(vm), thread_(vm.currentThread()) { vm_.releaseLock(); }
    ~LockReleased() { vm_.acquireLock(thread_); }
    LockReleased(const LockReleased&) = delete;
    LockReleased& operator=(const LockReleased&) = delete;

private:
    VM& vm_;
    ThreadState* thread_;
};

// Keeps a value alive while native code calls back into the VM.
class TempRoot {
public:
    TempRoot(VM& vm, Value v) : vm_(vm), index_(vm.tempRoots.size()) { vm.tempRoots.push_back(v); }
    ~TempRoot() { vm_.tempRoots.resize(index_); }
    TempRoot(const TempRoot&) = delete;
    TempRoot& operator=(const TempRoot&) = delete;

    void set(Value v) { vm_.tempRoots[index_] = v; }
    Value get() const { return vm_.tempRoots[index_]; }

private:
    VM& vm_;
    size_t index_;
};

// Keeps every value in a native-side vector alive while native code calls back into the VM.
class TempVector {
public:
    TempVector(VM& vm, const std::vector<Value>& values) : vm_(vm) { vm.tempVectors.push_back(&values); }
    ~TempVector() { vm_.tempVectors.pop_back(); }
    TempVector(const TempVector&) = delete;
    TempVector& operator=(const TempVector&) = delete;

private:
    VM& vm_;
};

void registerBuiltins(VM& vm);
void registerThreads(VM& vm);  // threads.cpp: threads, locks, the event loop and tasks
void registerIo(VM& vm);       // io.cpp: streams, processes and sockets
void registerSandbox(VM& vm);  // sandbox.cpp: __sandbox, what Sandbox is built on (M6.6.2)
void registerGrid(VM& vm);     // grid.cpp: what Grid<T> is built on (D155)
void registerWebp(VM& vm);     // webp.cpp: WebP's two codecs, VP8L and VP8
void registerPicture(VM& vm);  // picture.cpp: palettes and dithering over a picture's pixels
void registerNumeric(VM& vm);
void registerTensor(VM& vm);   // tensor.cpp: what Tensor<T> is built on (M9/M10, question 16)  // numeric.cpp: whole-list, grid and picture operations (M9/M10, questions 15 and 17)
void registerBits(VM& vm);     // bits_api.cpp: what BitReader, HuffmanTable, inflate and deflate are built on
void registerXml(VM& vm);      // xml_scan.cpp: what Xml.scan is built on (M9/M10)
void registerRegex(VM& vm);    // regex_api.cpp: what Regex is built on (N5, M8)
void registerForeign(VM& vm);  // foreign.cpp: Library, Memory and Foreign (M8)
void registerWorkers(VM& vm);  // worker.cpp: isolated workers
void registerSecureRandom(VM& vm);  // crypto/random.cpp: the system's secure random numbers
void registerCrypto(VM& vm);  // crypto/api.cpp: what import Crypto is built on (networking plan, part 2)
// For scripts (script.cpp): a value written out as a worker's message is, to be read by another
// VM; and such a message read back. Frozen data in it is shared, as in a worker's (D97).
struct Message;
std::shared_ptr<Message> messageFor(VM& vm, Value v);
Value readMessage(VM& vm, const Message& message);
// What a script's messages do with what cannot be copied (script.cpp): an object of an ordinary
// class or a function goes to the other side as a handle, made there into an object of an
// interface it implements, or a function, whose calls come back to it. Set on the thread for
// as long as a script's message is written or read; null for a worker's, which refuses them.
struct HandleWays {
    // The handle `v` goes as: its id, whether it is the reader's own object coming home, whether it
    // is a function, and the interfaces it implements; false where it cannot go so.
    virtual bool give(VM& vm, Value v, int64_t& id, bool& home, bool& function, std::vector<std::string>& interfaces) = 0;
    // What the reader makes of a handle.
    virtual Value take(VM& vm, int64_t id, bool home, bool function, const std::vector<std::string>& interfaces) = 0;
    // The class a message names where its global's number is another's: of the run's contract,
    // where it has one of the name (several scripts in one VM each have their own).
    virtual ObjClass* classNamed(VM& vm, const std::string& name) { return vm.classNamed(name); }
    virtual ~HandleWays() = default;
};
extern thread_local HandleWays* handleWays;
// The scripts a program compiles and runs as it runs (script.cpp, the scripting plan): set by
// ww's main, whose compiler side they are part of, and registered where that side is in the
// file (a built program keeps it only where it uses Script, D161).
extern void (*registerScripts)(VM& vm);
void registerParallel(VM& vm); // parallel.cpp: parallel sections
void registerText(VM& vm);     // text/api.cpp: fonts, text and pictures
// What started this program (see __io.selfCommand): set by main before the program runs.
void setSelfCommand(std::vector<std::string> command);

// The native layer of the Jade library (lib/Jade): windows, events and drawing. jade/api.cpp.
void registerJade(VM& vm);

}  // namespace ww
