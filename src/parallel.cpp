// Parallel sections: several cores running WinterWolf code on the same objects at once. The
// built-in __parallel, which src/std/parallel.ww is written on.
//
// Threads take turns and workers copy; this does neither. A section is one call: the work is
// cut into pieces (a share of a list's items, one of the functions given to parallel(...)),
// which the cores take and run, and the call returns when every piece is done. Meanwhile the
// program's own VM, which started it, waits with its lock held, so nothing else of the program
// runs. A piece may start a section of its own (D96): its core then works on that section's
// pieces too, and the other cores take them from it, until they are done.
//
// Each core has a VM of its own for the interpreter's working state (stack, calls in
// progress), made once and kept. These VMs share the heap of the one that started the
// section, which is safe because of one rule, checked wherever an object is looked into
// (VM::guardRead, VM::guardWrite):
//
//   - an object is either shared (Obj::core is 0) or owned by one piece of work, by its number;
//   - a piece changes only what is its own: the items it was dealt, and what it makes;
//   - it reads what is shared and its own, and what the pieces that started its section own,
//     which nothing changes while it runs (they wait for it).
//
// So what every piece can reach is never changed while the section runs, and what is changed
// is reached by one piece only. A function that breaks the rule gets an error at the first
// attempt, not a damaged object. What a piece made becomes the starter's when the piece ends;
// what the cores made joins the program's heap when the outermost section ends, and until then
// nothing is collected.

#include <algorithm>
#include <atomic>
#include <bitset>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

#include "builtins.h"
#include "fmath.h"
#include "vm.h"

namespace ww {

// One piece of a section: what to run it on, the number its objects are owned by, and how it
// turned out.
struct ParallelPiece {
    ParallelSection* section = nullptr;
    size_t index = 0;
    size_t from = 0, to = 0;   // its share of the items, or of a buffer's or a list's positions
    uint8_t owner = 0;
    std::vector<Value> results;
    Value result;
    bool failed = false;
    WwThrow error;
};

// A section: the pieces, what they do, and who started it.
struct ParallelSection {
    enum Kind { Items, Values, Chunks, Functions, Gather };
    Kind kind = Items;
    ParallelSection* parent = nullptr;  // the section of the piece that started this one; null for the program's
    uint8_t starter = 0;                // the owner of the code that started it, whose what the pieces make becomes
    std::bitset<256> readable;          // what its pieces may read besides their own and what is shared
    uint64_t generation = 0;            // the outermost section's number: a core takes the program's state once for it
    std::shared_ptr<Host> host;         // that of the code that started it: its pieces' (M6.6.2)

    Value function;                              // what each piece runs
    const std::vector<Value>* items = nullptr;   // Items, Gather: the function is called on each of these
    ObjList* values = nullptr;                   // Values: on each item of this list of values, where it is
    const Obj* buffer = nullptr;                 // Chunks: once a piece, with its range of this buffer or list
    const std::vector<Value>* functions = nullptr;  // Functions: one of these a piece
    Value each;                                  // Gather: `function` makes a piece's result, this adds an item to it
    bool keep = false;                           // what the function returns is wanted
    bool replace = false;                        // Values: each item replaced by what the function gives for it

    std::vector<ParallelPiece> pieces;
    std::atomic<int> left{0};             // pieces not yet done
    std::atomic<bool> cancelled{false};   // a piece has failed: the others stop at their next item

    bool within(const ParallelSection* other) const {
        for (const ParallelSection* s = this; s != nullptr; s = s->parent)
            if (s == other) return true;
        return false;
    }
};

// The cores: a thread and a VM each, and the pieces waiting to be run, a queue for each core.
// A core takes from the back of its own queue, and an idle one from the front of another's.
struct ParallelPool {
    VM& root;
    const int count;
    std::vector<std::thread> threads;
    std::vector<VM*> vms;
    std::vector<std::deque<ParallelPiece*>> queues;

    std::mutex mutex;
    std::condition_variable work, done;
    int ready = 0;
    bool quitting = false;
    uint64_t generation = 0;
    std::bitset<256> owners;   // the numbers in use by pieces of the sections running
    size_t nextQueue = 0;

    ParallelPool(VM& owner, int cores) : root(owner), count(cores), vms(static_cast<size_t>(cores), nullptr), queues(static_cast<size_t>(cores)) {
        owners.set(0);
        owners.set(kViewedInParts);  // never a piece's: the marks of what pieces write in parts
        owners.set(kDrawnInParts);
        for (int i = 0; i < count; i++) threads.emplace_back([this, i] { coreMain(i); });
        // Their VMs are made from the root's tables, which is safe now: the root is here, waiting.
        std::unique_lock<std::mutex> hold(mutex);
        done.wait(hold, [&] { return ready == count; });
    }

    ~ParallelPool() {
        {
            std::lock_guard<std::mutex> hold(mutex);
            quitting = true;
        }
        work.notify_all();
        for (std::thread& thread : threads) thread.join();
    }

    // Numbers for a section's pieces: false where too few are free, and the section runs in
    // the piece that starts it instead.
    bool takeOwners(ParallelSection& section) {
        std::lock_guard<std::mutex> hold(mutex);
        if (owners.count() + section.pieces.size() > 256) return false;
        uint8_t next = 1;
        for (ParallelPiece& piece : section.pieces) {
            while (owners.test(next)) next++;
            owners.set(next);
            piece.owner = next;
        }
        return true;
    }

    void giveOwners(const ParallelSection& section) {
        std::lock_guard<std::mutex> hold(mutex);
        for (const ParallelPiece& piece : section.pieces) owners.reset(piece.owner);
    }

    // Hands the pieces out: to the core starting the section, if it is one, else round the cores.
    void post(ParallelSection& section, int core) {
        {
            std::lock_guard<std::mutex> hold(mutex);
            for (ParallelPiece& piece : section.pieces) {
                const size_t to = core >= 0 ? static_cast<size_t>(core) : nextQueue++ % queues.size();
                queues[to].push_back(&piece);
            }
        }
        work.notify_all();
    }

    // A piece for core `core` to run: of its own queue, the newest; of another's, the oldest. With
    // `of`, only one of that section or of a section inside it.
    ParallelPiece* take(int core, const ParallelSection* of) {
        auto fits = [&](const ParallelPiece* piece) { return of == nullptr || piece->section->within(of); };
        std::deque<ParallelPiece*>& mine = queues[static_cast<size_t>(core)];
        for (auto it = mine.rbegin(); it != mine.rend(); ++it)
            if (fits(*it)) {
                ParallelPiece* piece = *it;
                mine.erase(std::next(it).base());
                return piece;
            }
        for (size_t k = 1; k < queues.size(); k++) {
            std::deque<ParallelPiece*>& theirs = queues[(static_cast<size_t>(core) + k) % queues.size()];
            for (auto it = theirs.begin(); it != theirs.end(); ++it)
                if (fits(*it)) {
                    ParallelPiece* piece = *it;
                    theirs.erase(it);
                    return piece;
                }
        }
        return nullptr;
    }

    void finished(ParallelPiece& piece) {
        if (piece.section->left.fetch_sub(1) == 1) {
            std::lock_guard<std::mutex> hold(mutex);
            done.notify_all();
        }
    }

    // The program's own VM waiting for its section.
    void await(ParallelSection& section) {
        std::unique_lock<std::mutex> hold(mutex);
        done.wait(hold, [&] { return section.left.load() == 0; });
    }

    // A core waiting for a section its piece started: it runs that section's pieces meanwhile.
    void help(int core, ParallelSection& section) {
        VM& vm = *vms[static_cast<size_t>(core)];
        while (section.left.load() > 0) {
            ParallelPiece* piece = nullptr;
            {
                std::unique_lock<std::mutex> hold(mutex);
                piece = take(core, &section);
                if (piece == nullptr) {
                    done.wait_for(hold, std::chrono::milliseconds(1), [&] { return section.left.load() == 0; });
                    continue;
                }
            }
            vm.runParallelPiece(*piece);
            finished(*piece);
        }
    }

    void coreMain(int index) {
        VM* vm = new VM(root, static_cast<uint8_t>(index + 1));
        vm->coreIndex_ = index;
        {
            std::lock_guard<std::mutex> hold(mutex);
            vms[static_cast<size_t>(index)] = vm;
            ready++;
        }
        done.notify_all();

        for (;;) {
            ParallelPiece* piece = nullptr;
            {
                std::unique_lock<std::mutex> hold(mutex);
                work.wait(hold, [&] {
                    if (quitting) return true;
                    piece = take(index, nullptr);
                    return piece != nullptr;
                });
                if (quitting) break;
            }
            vm->runParallelPiece(*piece);
            finished(*piece);
        }
        delete vm;
    }
};

// ---- A core's VM ----

VM::VM(VM& root, uint8_t coreId)
    : output(root.output), errorOutput(root.errorOutput), input(root.input), rng(std::random_device{}()), mainThread_(kThreadStackSize, kThreadMaxFrames),
      isWorker_(root.isWorker_), root_(&root), coreId_(coreId) {
    host = root.host;  // the same files and the rest as the program it works for
    mainThread_.name = "core-" + std::to_string(coreId);
    mainThread_.id = nextThreadId_++;
    threads_.push_back(&mainThread_);
    loadContext(&mainThread_.base);

    // The same names and built-in methods as the root; no table of strings of its own (see
    // intern); and nothing is collected: what it makes is handed to the root when a section ends.
    names = root.names;
    for (size_t i = 0; i < static_cast<size_t>(Op::Count_); i++) opNames_[i] = root.opNames_[i];
    for (size_t i = 0; i < static_cast<size_t>(BuiltinType::Count_); i++) methods_[i] = root.methods_[i];
    for (NativeCacheEntry& entry : nativeCache_) entry = NativeCacheEntry{};  // it pointed into the tables just replaced
    scriptArgs = root.scriptArgs;
    mainModule_ = root.mainModule_;
    nextGC_ = static_cast<size_t>(-1);
    extensionEpoch_ = static_cast<uint64_t>(-1);
}

// Before each section: the program's variables and functions as they are now. A core has a
// copy of the table of variables, which it reads and never writes.
void VM::refreshFromRoot() {
    program_ = root_->program_;
    globals_ = root_->globals_;
    globalTypes_ = root_->globalTypes_;
    loaded_ = root_->loaded_;
    errorClass_ = root_->errorClass_;
    for (size_t k = 0; k < static_cast<size_t>(Thrown::Count_); k++) thrownClasses_[k] = root_->thrownClasses_[k];
    mapEntryClass_ = root_->mapEntryClass_;
    classOfFn_ = root_->classOfFn_;
    viewClass_ = root_->viewClass_;
    imageCanvasClass_ = root_->imageCanvasClass_;
    hasExtensions_ = root_->hasExtensions_;
    hasBuiltinFits_ = root_->hasBuiltinFits_;
    if (extensionEpoch_ != root_->extensionEpoch_) {
        for (size_t i = 0; i < kTypeCount_; i++) {
            typeExtensions_[i].clear();
            typeExtensions_[i].addAll(root_->typeExtensions_[i]);
            builtinFits_[i].clear();
            for (const BuiltinFit& fit : root_->builtinFits_[i]) {
                builtinFits_[i].emplace_back();
                builtinFits_[i].back().type = fit.type;
                builtinFits_[i].back().iface = fit.iface;
                builtinFits_[i].back().getters.addAll(fit.getters);
                builtinFits_[i].back().methods.addAll(fit.methods);
            }
        }
        nullExtensions_.clear();
        nullExtensions_.addAll(root_->nullExtensions_);
        anyExtensions_.clear();
        anyExtensions_.addAll(root_->anyExtensions_);
        extensionEpoch_ = root_->extensionEpoch_;
    }
    pendingEnums_ = 0;
    nextGC_ = static_cast<size_t>(-1);
}

// After a section: what a core created becomes the root's, and nobody's in particular.
void VM::adoptObjects(VM& core) {
    Obj* head = core.objects_;
    if (head == nullptr) return;
    Obj* tail = head;
    for (Obj* obj = head; obj != nullptr; obj = obj->next) {
        obj->core = 0;
        tail = obj;
    }
    tail->next = objects_;
    objects_ = head;
    bytesAllocated_ += core.bytesAllocated_;
    core.objects_ = nullptr;
    core.bytesAllocated_ = 0;
}

// One piece, run on this core's VM: with its number, what it may read, and its part of a list
// or a buffer, all put back afterwards for the piece this core was running before, if any.
void VM::runParallelPiece(ParallelPiece& piece) {
    ParallelSection& section = *piece.section;
    if (section.cancelled.load(std::memory_order_relaxed)) return;
    if (seenGeneration_ != section.generation) {
        refreshFromRoot();
        seenGeneration_ = section.generation;
    }
    const uint8_t savedOwner = coreId_;
    const std::bitset<256>* const savedReadable = readable_;
    ParallelSection* const savedSection = section_;
    const Obj* const savedBand = bandObject_;
    const size_t savedFrom = bandFrom_, savedTo = bandTo_;
    const Value savedDestination = pendingDestination_;
    coreId_ = piece.owner;
    currentCore = piece.owner;
    readable_ = &section.readable;
    const std::shared_ptr<Host> savedHost = context_->host;
    context_->host = section.host;
    currentReadable = readable_;
    section_ = &section;
    bandObject_ = nullptr;
    pendingDestination_ = Value::null();
    Obj* const mark = objects_;
    try {
        switch (section.kind) {
            case ParallelSection::Chunks: {
                // One call, with the part of the buffer or the list that is this piece's to write.
                bandObject_ = section.buffer;
                bandFrom_ = piece.from;
                bandTo_ = piece.to;
                Value range = Value::object(alloc<ObjRange>(static_cast<int64_t>(piece.from), static_cast<int64_t>(piece.to), int64_t{1}, false));
                call(section.function, 1, &range);
                break;
            }
            case ParallelSection::Values: {
                // The items of a list of values, each where it is; this piece reads and writes its
                // own part of the list and no other (refAt, packedGet). Replaced, an item is where
                // the result goes, so a value made from its header alone is built there.
                ObjList* list = section.values;
                bandObject_ = list;
                bandFrom_ = piece.from;
                bandTo_ = piece.to;
                if (section.keep) piece.results.reserve(piece.to - piece.from);
                for (size_t i = piece.from; i < piece.to && !section.cancelled.load(std::memory_order_relaxed); i++) {
                    const bool null = list->pack->nullable && (list->pack->nulls[i / 64] >> (i % 64) & 1);
                    Value item = null ? Value::null() : Value::ref(list, static_cast<uint32_t>(i), 0, 0);
                    if (section.replace && !null) pendingDestination_ = item;  // a null item is replaced by setting it
                    Value result = call(section.function, 1, &item);
                    pendingDestination_ = Value::null();
                    if (section.replace) {
                        const bool there = result.isRef() && result.as.obj == list && result.refIndex == i && result.refOffset == 0 && result.refDepth == 0;
                        if (!there) listSet(list, i, result);
                    } else if (section.keep) {
                        piece.results.push_back(result);
                    }
                }
                break;
            }
            case ParallelSection::Functions:
                piece.result = call((*section.functions)[piece.index], 0, nullptr);
                break;
            case ParallelSection::Gather: {
                // A result of its own, made empty, and each of its items added to it.
                piece.result = call(section.function, 0, nullptr);
                for (size_t i = piece.from; i < piece.to && !section.cancelled.load(std::memory_order_relaxed); i++) {
                    Value args[2] = {piece.result, (*section.items)[i]};
                    call(section.each, 2, args);
                }
                break;
            }
            case ParallelSection::Items: {
                if (section.keep) piece.results.reserve(piece.to - piece.from);
                for (size_t i = piece.from; i < piece.to && !section.cancelled.load(std::memory_order_relaxed); i++) {
                    Value item = (*section.items)[i];
                    Value result = call(section.function, 1, &item);
                    if (section.keep) piece.results.push_back(result);
                }
                break;
            }
        }
    } catch (WwThrow& thrown) {
        piece.error = thrown;
        piece.failed = true;
        section.cancelled = true;
    } catch (const WwExit&) {
        piece.error = runtimeError(Thrown::IllegalStateException, "exit() was called in a parallel section");
        piece.failed = true;
        section.cancelled = true;
    } catch (const FiberSuspend&) {
        piece.error = runtimeError(Thrown::IllegalStateException, "a parallel section does not wait");
        piece.failed = true;
        section.cancelled = true;
    }
    // What the piece made becomes its starter's: the code that waits for it. What it made shared
    // (a box, a class's description) stays shared.
    for (Obj* obj = objects_; obj != mark; obj = obj->next)
        if (obj->core != 0) obj->core = section.starter;
    coreId_ = savedOwner;
    currentCore = savedOwner;
    readable_ = savedReadable;
    currentReadable = savedReadable;
    section_ = savedSection;
    bandObject_ = savedBand;
    bandFrom_ = savedFrom;
    bandTo_ = savedTo;
    pendingDestination_ = savedDestination;
    context_->host = savedHost;
}

// ---- Starting a section ----

namespace {

int coreCount() {
    int count = static_cast<int>(std::thread::hardware_concurrency());
    // WW_CORES: how many cores a section uses, whatever the machine has. For tests.
    if (const char* wanted = std::getenv("WW_CORES"); wanted != nullptr && *wanted != '\0') count = std::atoi(wanted);
    return std::clamp(count, 1, 64);
}

bool ownable(const Obj* obj) {
    return obj->type == ObjType::Instance || obj->type == ObjType::List || obj->type == ObjType::Map || obj->type == ObjType::Bytes ||
           obj->type == ObjType::Set;
}

// Gives back, when the section is over however it ends, the items that were dealt out.
struct Dealt {
    std::vector<Obj*> objects;
    uint8_t to = 0;
    ~Dealt() {
        for (Obj* obj : objects) obj->core = to;
    }
};

// Whether the object is a part of a split canvas (G64): an ImageCanvas with a split's number.
bool isCanvasPart(VM& vm, ObjClass* canvasClass, const Obj* obj) {
    if (canvasClass == nullptr || obj->type != ObjType::Instance || static_cast<const ObjInstance*>(obj)->cls != canvasClass) return false;
    Value part;
    return static_cast<const ObjInstance*>(obj)->readText(vm.intern("part"), part) && part.isInt() && part.as.i != 0;
}

// A part of a split canvas handed to a piece (G64). It has to be of its image's latest split,
// whose parts never overlap. While the section lasts the image's pixels are drawn on through
// the parts alone, each by the piece it was handed to, and no piece reads them (kDrawnInParts).
// The image is marked changed over the part here, as Image.changed does, since a piece cannot
// change an image it may only read: done for each part of an image whose pixels this dealing
// marks, `marked` keeping which. An image this code may only read is left alone, and its parts
// draw nothing (mayDraw says why). In a section inside a section, the part was dealt already.
void dealCanvasPart(VM& vm, ObjInstance* canvas, Dealt& dealt, std::vector<const Obj*>& marked) {
    Value part, image, pixels, splits;
    if (!canvas->readText(vm.intern("part"), part) || !canvas->readText(vm.intern("image"), image) || !canvas->readText(vm.intern("pixels"), pixels) ||
        !image.isObjType(ObjType::Instance) || !pixels.isObj())
        return;
    ObjInstance* img = asInstance(image);
    if (!img->readText(vm.intern("splits"), splits) || !splits.isInt()) return;
    if (splits.as.i != part.as.i)
        throw vm.runtimeError(Thrown::IllegalStateException, "a part of an earlier split of an image's canvas is handed out: only the parts of its latest split draw on it together");
    Obj* list = pixels.as.obj;
    if (list->core == vm.coreId() && img->core == vm.coreId() && list->frozen == 0) {
        list->core = kDrawnInParts;
        dealt.objects.push_back(list);
        marked.push_back(list);
    } else if (std::find(marked.begin(), marked.end(), list) == marked.end()) {
        return;
    }
    auto get = [&](ObjInstance* from, const char* name) {
        Value v;
        return from->readText(vm.intern(name), v) && v.isInt() ? v.as.i : 0;
    };
    const int64_t l = std::max<int64_t>(0, get(canvas, "clipLeft")), t = std::max<int64_t>(0, get(canvas, "clipTop"));
    const int64_t r = std::min(get(img, "width"), get(canvas, "clipRight")), b = std::min(get(img, "height"), get(canvas, "clipBottom"));
    if (r <= l || b <= t) return;
    const int64_t cl = get(img, "changedLeft"), ct = get(img, "changedTop"), cr = get(img, "changedRight"), cb = get(img, "changedBottom");
    const bool none = cr <= cl || cb <= ct;
    img->setFieldText(vm.intern("changedLeft"), Value::integer(none ? l : std::min(cl, l)));
    img->setFieldText(vm.intern("changedTop"), Value::integer(none ? t : std::min(ct, t)));
    img->setFieldText(vm.intern("changedRight"), Value::integer(none ? r : std::max(cr, r)));
    img->setFieldText(vm.intern("changedBottom"), Value::integer(none ? b : std::max(cb, b)));
    img->setFieldText(vm.intern("changes"), Value::integer(get(img, "changes") + 1));
}

}  // namespace

// What a piece is told when it looks into a list that pieces write through its views.
const char* const kViewedMessage =
    "this list is written through its views in this parallel section: until it ends each piece reads and writes it through the views given to it, and nothing else looks into it";

namespace {

// A view of a list handed to a piece (D96): one that is not split meanwhile, from a splitAt
// whose block is still running, so that the views handed out never overlap. While the section
// lasts the list is written through its views alone, each by the piece it was handed to, and
// no piece reads it otherwise (kViewedInParts). In a section inside a section, the list was
// marked by the section outside.
void dealView(VM& vm, const ParallelSection& section, ObjInstance* view, Dealt& dealt) {
    Value lent, ended, list;
    if (view->readText(vm.intern("lent"), lent) && lent.truthy())
        throw vm.runtimeError(Thrown::IllegalStateException, "this view has been split: its halves are given out in its place until splitAt's block ends");
    if (view->readText(vm.intern("ended"), ended) && ended.truthy())
        throw vm.runtimeError(Thrown::IllegalStateException, "this view's splitAt has ended: a view is given out inside the block it was handed to");
    if (!view->readText(vm.intern("list"), list) || !list.isObj()) return;
    Obj* items = list.as.obj;
    if (items->core == kViewedInParts) return;
    if (items->core == vm.coreId() && items->frozen == 0) {
        items->core = kViewedInParts;
        dealt.objects.push_back(items);
        return;
    }
    for (const ParallelPiece& piece : section.pieces)
        if (piece.owner == items->core)
            throw vm.runtimeError(Thrown::IllegalStateException, "a list and a view of it are both given out: each piece writes the list through its views alone");
    throw vm.runtimeError(Thrown::IllegalStateException, "this view's list is not this code's to change, so its views are not given out");
}

std::string describe(const VM& vm, const Obj* obj) {
    switch (obj->type) {
        case ObjType::Instance: return "this " + static_cast<const ObjInstance*>(obj)->cls->name->utf8();
        case ObjType::Class: return "class " + static_cast<const ObjClass*>(obj)->name->utf8();
        case ObjType::Upvalue: return "this variable";
        default: return "this " + vm.typeName(Value::object(const_cast<Obj*>(obj)));
    }
}

// `count` positions in `parts` parts as even as can be.
// `count` positions in `parts` pieces, each starting at a multiple of `align`: a grid's rows
// kept whole (D155).
void cut(ParallelSection& section, size_t count, size_t parts, size_t align = 1) {
    const size_t units = count / align;
    section.pieces.resize(parts);
    for (size_t k = 0; k < parts; k++) {
        section.pieces[k].section = &section;
        section.pieces[k].index = k;
        section.pieces[k].from = units * k / parts * align;
        section.pieces[k].to = k + 1 == parts ? count : units * (k + 1) / parts * align;
    }
}

}  // namespace

int VM::parallelCores() {
    if (root_ != nullptr) return root_->parallelCores();
    if (parallelPool_ == nullptr) parallelPool_ = std::make_shared<ParallelPool>(*this, coreCount());
    return parallelPool_->count;
}

void VM::runSection(ParallelSection& section) {
    parallelCores();  // the pool, made the first time
    ParallelPool& pool = *(root_ != nullptr ? root_ : this)->parallelPool_;
    section.starter = makingShared_ != 0 ? 0 : coreId_;  // what is made for every core is shared
    section.parent = section_;
    if (readable_ != nullptr) section.readable = *readable_;
    if (coreId_ != 0) section.readable.set(coreId_);
    if (section_ != nullptr) {
        section.generation = section_->generation;
    } else {
        // The cores share the program's functions: every one's run code made before they start,
        // so that none is made while another core runs it.
        if (program_ != nullptr) program_->translateAll();
        std::lock_guard<std::mutex> hold(pool.mutex);
        section.generation = ++pool.generation;
    }
    section.left = static_cast<int>(section.pieces.size());

    // A section inside a section, with no numbers left for its pieces, runs here, one piece
    // after another, as the piece that starts it.
    const bool alone = coreId_ != 0 && !pool.takeOwners(section);
    if (alone) {
        for (ParallelPiece& piece : section.pieces) piece.owner = coreId_;
    } else if (coreId_ == 0 && !pool.takeOwners(section)) {
        throw runtimeError(Thrown::InternalError, "a parallel section has too many pieces");
    }

    {
        // The pieces' numbers are given back however the section ends, an error in handing out
        // its items among the ways, once the items themselves are given back.
        struct Owners {
            ParallelPool& pool;
            const ParallelSection& section;
            const bool taken;
            ~Owners() {
                if (taken) pool.giveOwners(section);
            }
        } owners{pool, section, !alone};
        // Each piece owns its items for as long as the section lasts: those the starter owns.
        Dealt dealt;
        dealt.to = coreId_;
        std::vector<const Obj*> marked;  // images' pixels this section draws on in parts
        // The views of a list each function of a parallel(...) captures are its own (D96), and so
        // are the parts of a split canvas (G64).
        if (!alone && section.kind == ParallelSection::Functions) {
            for (ParallelPiece& piece : section.pieces) {
                const Value& function = (*section.functions)[piece.index];
                if (!function.isObjType(ObjType::Closure)) continue;
                const ObjClosure* closure = asClosure(function);
                for (size_t u = 0; u < closure->upvalues.size(); u++) {
                    const Value held = *closure->upvalues[u]->location;
                    if (!held.isObjType(ObjType::Instance)) continue;
                    const bool isView = viewClass_ != nullptr && asInstance(held)->cls == viewClass_;
                    const bool isPart = !isView && isCanvasPart(*this, imageCanvasClass_, held.as.obj);
                    if (!isView && !isPart) continue;
                    ObjInstance* view = asInstance(held);
                    if (view->core == piece.owner) continue;  // captured twice by the one function
                    bool ours = false;
                    for (const ParallelPiece& other : section.pieces) ours = ours || other.owner == view->core;
                    if (ours)
                        throw runtimeError(Thrown::IllegalStateException, isView ? "the same view is given to two of the functions: each view is one function's own"
                                                                                 : "the same part of a canvas is given to two of the functions: each part is one function's own");
                    if (view->core != coreId_) continue;  // not the starter's to give: read, not written
                    view->core = piece.owner;
                    dealt.objects.push_back(view);
                    if (isPart) dealCanvasPart(*this, view, dealt, marked);
                    else dealView(*this, section, view, dealt);
                }
            }
        }
        if (!alone && (section.kind == ParallelSection::Items || section.kind == ParallelSection::Gather)) {
            for (ParallelPiece& piece : section.pieces) {
                for (size_t i = piece.from; i < piece.to; i++) {
                    const Value& item = (*section.items)[i];
                    if (!item.isObj() || !ownable(item.as.obj)) continue;
                    Obj* obj = item.as.obj;
                    if (obj->core != coreId_) {
                        bool ours = false;
                        for (const ParallelPiece& other : section.pieces) ours = ours || other.owner == obj->core;
                        if (ours)
                            throw runtimeError(Thrown::IllegalStateException, "the same " + typeName(item) + " is in the stream more than once (it is item " + std::to_string(i) +
                                                                                  " as well): each core has to have its items to itself");
                        continue;  // shared with the piece's starter's own readers: read, not changed
                    }
                    obj->core = piece.owner;
                    dealt.objects.push_back(obj);
                    if (isCanvasPart(*this, imageCanvasClass_, obj)) dealCanvasPart(*this, static_cast<ObjInstance*>(obj), dealt, marked);
                    else if (viewClass_ != nullptr && obj->type == ObjType::Instance && static_cast<ObjInstance*>(obj)->cls == viewClass_)
                        dealView(*this, section, static_cast<ObjInstance*>(obj), dealt);
                }
            }
        }
        if (alone) {
            for (ParallelPiece& piece : section.pieces) {
                if (!section.cancelled.load()) runParallelPiece(piece);
            }
            section.left = 0;
        } else {
            pool.post(section, coreId_ != 0 ? coreIndex_ : -1);
            if (coreId_ == 0) pool.await(section);
            else pool.help(coreIndex_, section);
        }
    }
    // The outermost section over: what the cores made is the program's, and nobody's in particular.
    if (section.parent == nullptr && coreId_ == 0) {
        for (VM* core : pool.vms) adoptObjects(*core);
        // And the run code's words its cores wrote, which waited for them to stop (writeRunWord).
        if (wordsWaiting_.load()) writeWaitingWords();
    }
    for (const ParallelPiece& piece : section.pieces)
        if (piece.failed) throw piece.error;
}

Value VM::runParallel(Value source, Value function, bool keep, bool replace) {
    if (replace && !source.isObjType(ObjType::List)) throw runtimeError(Thrown::ClassCastException, "replaceAll is for a list, not a value of type " + typeName(source));
    // A list of values (M6.6 part 4): each piece is given its own part of the list, and the items
    // where they are; one that may hold null, null for each that is (G11).
    ObjList* values = source.isObjType(ObjType::List) && asList(source)->store() == ListStore::Items
                          ? asList(source)
                          : nullptr;
    if (values != nullptr && values->core != coreId_ && (replace || coreId_ != 0)) values = nullptr;  // not this piece's to write in place
    std::vector<Value> items;
    TempVector holdItems(*this, items);
    if (values == nullptr) collect(source, items);
    const size_t count = values != nullptr ? values->size() : items.size();
    // Replacing the items of any other list, what the function gives is kept, and put in once
    // every piece is done: a list of numbers or objects is not written by several cores at once.
    const bool keepAll = keep || (replace && values == nullptr);
    ObjList* out = keepAll ? newList(*this) : nullptr;
    TempRoot holdOut(*this, keepAll ? Value::object(out) : Value::null());
    if (count == 0) return keep ? Value::object(out) : Value::null();

    ParallelSection section;
    section.kind = values != nullptr ? ParallelSection::Values : ParallelSection::Items;
    section.host = currentHostShared();
    section.function = function;
    section.items = &items;
    section.values = values;
    section.keep = keepAll;
    section.replace = replace && values != nullptr;
    // Which items are null is kept in bits, 64 to a word: a list that may hold null is cut at whole
    // words, so that no two cores set bits of the same word as they replace items.
    const size_t align = values != nullptr && values->pack->nullable ? 64 : 1;
    cut(section, count, std::min(static_cast<size_t>(parallelCores()), (count + align - 1) / align), align);
    runSection(section);

    if (replace && values == nullptr) {
        ObjList* list = asList(source);
        if (list->size() != count) throw runtimeError(Thrown::ConcurrentModificationException, "the list was changed while its items were replaced");
        size_t i = 0;
        for (const ParallelPiece& piece : section.pieces)
            for (const Value& made : piece.results) listSet(list, i++, made);
        return Value::null();
    }
    if (!keep) return Value::null();
    out->values().reserve(count);
    for (const ParallelPiece& piece : section.pieces) out->values().insert(out->values().end(), piece.results.begin(), piece.results.end());
    account(out->values().size() * sizeof(Value));
    return Value::object(out);
}

void VM::runParallelChunks(Value buffer, Value function, size_t align) {
    // Bytes, or a list (D96): its positions in parts, each piece reading and writing its own.
    const bool bytes = buffer.isObjType(ObjType::Bytes);
    if (!bytes && !buffer.isObjType(ObjType::List))
        throw runtimeError(Thrown::ClassCastException, "parallelChunks is for bytes and lists, not a value of type " + typeName(buffer));
    if (buffer.as.obj->core != coreId_) throw notOwned(buffer.as.obj);
    const size_t size = bytes ? asBytes(buffer)->data.size() : asList(buffer)->size();
    if (size == 0) return;

    ParallelSection section;
    section.kind = ParallelSection::Chunks;
    section.host = currentHostShared();
    section.function = function;
    section.buffer = buffer.as.obj;
    if (align == 0) align = 1;
    cut(section, size, std::max<size_t>(1, std::min(static_cast<size_t>(parallelCores()), size / align)), align);
    runSection(section);
}

Value VM::runParallelFunctions(const std::vector<Value>& functions) {
    ObjList* out = newList(*this);
    TempRoot holdOut(*this, Value::object(out));
    if (functions.empty()) return Value::object(out);
    ParallelSection section;
    section.kind = ParallelSection::Functions;
    section.host = currentHostShared();
    section.functions = &functions;
    section.pieces.resize(functions.size());
    for (size_t k = 0; k < functions.size(); k++) {
        section.pieces[k].section = &section;
        section.pieces[k].index = k;
    }
    runSection(section);
    for (const ParallelPiece& piece : section.pieces) out->values().push_back(piece.result);
    account(out->values().size() * sizeof(Value));
    return Value::object(out);
}

Value VM::runParallelGather(Value source, Value into, Value each) {
    std::vector<Value> items;
    TempVector holdItems(*this, items);
    collect(source, items);
    ObjList* out = newList(*this);
    TempRoot holdOut(*this, Value::object(out));
    if (items.empty()) {
        out->values().push_back(call(into, 0, nullptr));
        return Value::object(out);
    }
    ParallelSection section;
    section.kind = ParallelSection::Gather;
    section.host = currentHostShared();
    section.function = into;
    section.each = each;
    section.items = &items;
    cut(section, items.size(), std::min(static_cast<size_t>(parallelCores()), items.size()));
    runSection(section);
    for (const ParallelPiece& piece : section.pieces) out->values().push_back(piece.result);
    account(out->values().size() * sizeof(Value));
    return Value::object(out);
}

// ---- What a core is told when it oversteps ----

WwThrow VM::wholeBand() const {
    return const_cast<VM*>(this)->runtimeError(Thrown::IllegalStateException, "this List is the one the section is over and is read here an item at a time: a core looks only "
                                               "into its own items, " + std::to_string(bandFrom_) + " to " + std::to_string(bandTo_ - 1));
}

WwThrow VM::outsideBand(size_t i) const {
    if (bandObject_->type == ObjType::List)
        return const_cast<VM*>(this)->runtimeError(Thrown::IllegalStateException, "item " + std::to_string(i) + " is another core's in this parallel section: a core looks only into its "
                                                   "own items, " + std::to_string(bandFrom_) + " to " + std::to_string(bandTo_ - 1));
    return const_cast<VM*>(this)->runtimeError(Thrown::IllegalStateException, "position " + std::to_string(i) + " of the buffer is another core's in this " +
                                               "parallel section: a core looks only into its own part, " + std::to_string(bandFrom_) + " to " +
                                               std::to_string(bandTo_ - 1) + " (8.7h)");
}

WwThrow VM::notOwned(const std::string& what) const {
    return const_cast<VM*>(this)->runtimeError(Thrown::IllegalStateException, "a parallel section changes only the item it was given and what it makes itself: " + what);
}

WwThrow VM::notOwned(const Obj* obj) const {
    if (obj->frozen != 0) {
        const char* const kind = obj->type == ObjType::List ? "list" : obj->type == ObjType::Map ? "map" : obj->type == ObjType::Set ? "set" : "value";
        const char* const copy = obj->type == ObjType::List ? "toOpenList()" : obj->type == ObjType::Map ? "toOpenMap()" : "toOpenSet()";
        return const_cast<VM*>(this)->runtimeError(Thrown::UnsupportedOperationException, std::string("a frozen ") + kind + " cannot be changed: " + copy + " makes a copy that can be");
    }
    if (obj->core != 0) return otherCores(obj);
    return notOwned(describe(*this, obj) + " is shared with the other cores, so here it can be read and not changed");
}

WwThrow VM::otherCores(const Obj* obj) const {
    if (obj->core == kDrawnInParts)
        return const_cast<VM*>(this)->runtimeError(Thrown::IllegalStateException, "the pixels of an image are drawn on in parts in this parallel section: until it ends they are "
                                                                                   "drawn on through the parts of its canvas alone, and read by nothing");
    if (obj->core == kViewedInParts)
        return const_cast<VM*>(this)->runtimeError(Thrown::IllegalStateException, kViewedMessage);
    // To read every item, the section goes over the positions instead, which leaves the list
    // shared and every item readable (D95).
    return const_cast<VM*>(this)->runtimeError(Thrown::IllegalStateException, describe(*this, obj) + " is another core's item in this parallel section: a core looks only into its own items " +
                                               "and into what is shared. To read every item, go over the positions instead: items.indices.parallelStream()");
}

void VM::markMutating(BuiltinType type, std::initializer_list<const char*> methods) {
    for (const char* name : methods) methods_[static_cast<int>(type)].at(intern(name)).mutates = true;
}

void VM::markTakesPlaces(BuiltinType type, std::initializer_list<const char*> methods) {
    for (const char* name : methods) methods_[static_cast<int>(type)].at(intern(name)).takesPlaces = true;
}

void VM::markNotParallel(const std::string& global, std::initializer_list<const char*> except) {
    auto found = builtins_.find(global);
    if (found == builtins_.end()) return;
    if (found->second.isObjType(ObjType::Native)) {
        asNative(found->second)->parallelSafe = false;
    } else if (found->second.isObjType(ObjType::Class)) {
        const Table& members = asClass(found->second)->statics;
        for (uint32_t i = 0; i < members.capacity(); i++) {
            const Table::Entry& entry = members.entries()[i];
            if (entry.key == nullptr || !entry.value.isObjType(ObjType::Native)) continue;
            bool excepted = false;
            for (const char* name : except) excepted = excepted || entry.key->utf8() == name;
            asNative(entry.value)->parallelSafe = excepted;
        }
    }
}

// ---- Atomic values (D85) ----
//
// An Atomic keeps its value in a list of two items, its cell: the value, which any thread and any
// core of a section may change through these functions, each in one step, and how it is changed.
// That is chosen once, from the Atomic's type, when it is made (8.7f): an Atomic<int>'s or an
// Atomic<float>'s eight bytes with the processor's atomic operations (std::atomic_ref), anything
// else's under a lock of its own, one of a few shared by cells by their address. Chosen by what
// the cell held at each change, a change without the lock and one under it could meet in an
// Atomic<any>, and leave an int's bits taken for an object.

namespace {

std::mutex atomicLocks[64];
std::mutex& atomicLock(const Value* cell) { return atomicLocks[(reinterpret_cast<uintptr_t>(cell) >> 4) & 63]; }

enum class AtomicMode : int64_t { Locked = 0, Int = 1, Float = 2 };

struct AtomicCell {
    Value* value;
    AtomicMode mode;
};

AtomicCell atomicCell(VM& vm, const Value& v) {
    if (!v.isObjType(ObjType::List) || asList(v)->packed() || asList(v)->size() != 2)
        throw vm.runtimeError(Thrown::InternalError, "an Atomic's cell is a list of two items");
    std::vector<Value>& items = asList(v)->values();
    return {&items[0], static_cast<AtomicMode>(items[1].as.i)};
}

// What a core of a parallel section may store where every core reads it: a value that cannot
// change. Anywhere else, anything.
void storable(VM& vm, const Value& v) {
    if (vm.coreId() == 0 || !v.isObj() || v.as.obj->frozen != 0 || v.as.obj->type == ObjType::String) return;
    throw vm.runtimeError(Thrown::IllegalStateException, "in a parallel section an Atomic is given values: numbers, bools, small values, text or frozen data; not this " +
                                                             vm.typeName(v) + ", which a core may still change");
}

// Whether both are numbers or bools of one kind (by their bits), or the very same object.
bool sameValue(const Value& a, const Value& b) {
    if (a.kind != b.kind) return false;
    switch (a.kind) {
        case ValueKind::Null: return true;
        case ValueKind::Bool: return a.as.b == b.as.b;
        case ValueKind::Obj: return a.as.obj == b.as.obj;
        case ValueKind::Float: return std::memcmp(&a.as, &b.as, sizeof a.as) == 0;
        case ValueKind::Small:
        case ValueKind::Ref: return std::memcmp(&a, &b, sizeof(Value)) == 0;
        default: return a.as.i == b.as.i;
    }
}

std::atomic_ref<int64_t> payload(Value* cell) { return std::atomic_ref<int64_t>(cell->as.i); }

Value withBits(AtomicMode mode, int64_t bits) {
    if (mode == AtomicMode::Int) return Value::integer(bits);
    double d;
    std::memcpy(&d, &bits, sizeof d);
    return Value::number(d);
}

// The bits an Atomic<int> or Atomic<float> holds for a value: an int given to an Atomic<float>
// as the float it is. Its type lets in nothing else; one cast past it is an error, before
// anything is changed.
bool bitsFor(AtomicMode mode, const Value& v, int64_t& bits) {
    if (mode == AtomicMode::Int) {
        if (!v.isInt()) return false;
        bits = v.as.i;
        return true;
    }
    if (!v.isNumber()) return false;
    bits = static_cast<int64_t>(fmath::canonicalBits(v.toDouble()));  // every NaN the one NaN
    return true;
}

int64_t bitsOf(VM& vm, AtomicMode mode, const Value& v) {
    int64_t bits = 0;
    if (!bitsFor(mode, v, bits))
        throw vm.runtimeError(Thrown::ClassCastException, std::string("an Atomic<") + (mode == AtomicMode::Int ? "int" : "float") + "> holds " +
                                                              (mode == AtomicMode::Int ? "ints" : "numbers") + ", not a " + vm.typeName(v));
    return bits;
}

}  // namespace

void registerAtomics(VM& vm) {
    ObjClass* atomic = vm.alloc<ObjClass>(vm.intern("atomic"));
    auto def = [&](const char* name, int argc, NativeFn fn) {
        atomic->statics.set(vm.intern(name), Value::object(vm.alloc<ObjNative>(std::string("atomic.") + name, argc, argc, fn)));
    };
    // The value to begin with, and how it is changed: 1 for an Atomic<int>, 2 for an
    // Atomic<float>, 0 under the lock for anything else (Atomic's own code works it out from T).
    def("make", 2, [](VM& vm, int, Value* args) -> Value {
        storable(vm, args[0]);
        AtomicMode mode = static_cast<AtomicMode>(args[1].isInt() && args[1].as.i >= 0 && args[1].as.i <= 2 ? args[1].as.i : 0);
        Value initial = args[0];
        int64_t bits = 0;
        if (mode != AtomicMode::Locked) {
            if (!bitsFor(mode, initial, bits)) mode = AtomicMode::Locked;  // a T it cannot tell: under the lock, which takes anything
            else initial = withBits(mode, bits);
        }
        return Value::object(newList(vm, {initial, Value::integer(static_cast<int64_t>(mode))}));
    });
    def("get", 1, [](VM& vm, int, Value* args) -> Value {
        const AtomicCell cell = atomicCell(vm, args[0]);
        if (cell.mode != AtomicMode::Locked) return withBits(cell.mode, payload(cell.value).load());
        std::lock_guard<std::mutex> hold(atomicLock(cell.value));
        return *cell.value;
    });
    def("set", 2, [](VM& vm, int, Value* args) -> Value {
        storable(vm, args[1]);
        const AtomicCell cell = atomicCell(vm, args[0]);
        if (cell.mode != AtomicMode::Locked) {
            payload(cell.value).store(bitsOf(vm, cell.mode, args[1]));
        } else {
            std::lock_guard<std::mutex> hold(atomicLock(cell.value));
            *cell.value = args[1];
        }
        return Value::null();
    });
    def("exchange", 2, [](VM& vm, int, Value* args) -> Value {
        storable(vm, args[1]);
        const AtomicCell cell = atomicCell(vm, args[0]);
        if (cell.mode != AtomicMode::Locked) return withBits(cell.mode, payload(cell.value).exchange(bitsOf(vm, cell.mode, args[1])));
        std::lock_guard<std::mutex> hold(atomicLock(cell.value));
        const Value old = *cell.value;
        *cell.value = args[1];
        return old;
    });
    def("compareAndSet", 3, [](VM& vm, int, Value* args) -> Value {
        storable(vm, args[2]);
        const AtomicCell cell = atomicCell(vm, args[0]);
        if (cell.mode != AtomicMode::Locked) {
            const int64_t value = bitsOf(vm, cell.mode, args[2]);
            int64_t expected = 0;
            if (!bitsFor(cell.mode, args[1], expected)) return Value::boolean(false);  // never held
            return Value::boolean(payload(cell.value).compare_exchange_strong(expected, value));
        }
        std::lock_guard<std::mutex> hold(atomicLock(cell.value));
        if (!sameValue(*cell.value, args[1])) return Value::boolean(false);
        *cell.value = args[2];
        return Value::boolean(true);
    });
    // An int added to, in one step, giving the new value; one that does not fit is an error (D27).
    def("add", 2, [](VM& vm, int, Value* args) -> Value {
        const AtomicCell cell = atomicCell(vm, args[0]);
        const int64_t amount = vm.toInt(args[1], "the amount");
        auto overflow = [&](int64_t old) {
            return vm.runtimeError(Thrown::ArithmeticException, std::to_string(old) + " + " + std::to_string(amount) +
                                                                    " does not fit in an int; update { it &+ n } wraps around (D27)");
        };
        int64_t next = 0;
        if (cell.mode == AtomicMode::Locked) {
            // An Atomic<int> made where its type was not known: under the lock, as its other changes are.
            std::lock_guard<std::mutex> hold(atomicLock(cell.value));
            if (!cell.value->isInt()) throw vm.runtimeError(Thrown::ClassCastException, "an Atomic that is added to holds an int, not a " + vm.typeName(*cell.value));
            if (addOverflows(cell.value->as.i, amount, next)) throw overflow(cell.value->as.i);
            *cell.value = Value::integer(next);
            return *cell.value;
        }
        if (cell.mode != AtomicMode::Int) throw vm.runtimeError(Thrown::ClassCastException, "an Atomic that is added to holds an int, not a float");
        std::atomic_ref<int64_t> held = payload(cell.value);
        int64_t old = held.load();
        do {
            if (addOverflows(old, amount, next)) throw overflow(old);
        } while (!held.compare_exchange_weak(old, next));
        return Value::integer(next);
    });
    vm.defineGlobal("__atomic", Value::object(atomic));
}

// ---- Views of a list (D96) ----
//
// splitAt hands out two views of a list that cannot overlap, the first `mid` items and the rest;
// a view given to one of the functions of a parallel(...) is that piece's own, and writes the
// list's items in its range, whoever owns the list. A view that has been split is lent to its
// halves until splitAt's block ends: nothing writes through it meanwhile.

namespace {

struct ViewParts {
    ObjInstance* view = nullptr;
    ObjList* list = nullptr;
    size_t start = 0, size = 0;
    bool lent = false;
    bool ended = false;  // its splitAt's block is over
};

ViewParts viewOf(VM& vm, const Value& v) {
    if (!v.isObjType(ObjType::Instance)) throw vm.runtimeError(Thrown::InternalError, "a ListView is needed");
    ViewParts parts;
    parts.view = asInstance(v);
    Value list, start, size, lent;
    if (!parts.view->readText(vm.intern("list"), list) || !parts.view->readText(vm.intern("start"), start) || !parts.view->readText(vm.intern("size"), size) ||
        !parts.view->readText(vm.intern("lent"), lent) || !list.isObjType(ObjType::List))
        throw vm.runtimeError(Thrown::InternalError, "a ListView is needed");
    parts.list = asList(list);
    parts.start = static_cast<size_t>(start.as.i);
    parts.size = static_cast<size_t>(size.as.i);
    parts.lent = lent.truthy();
    Value ended;
    parts.ended = parts.view->readText(vm.intern("ended"), ended) && ended.truthy();
    if (parts.start + parts.size > parts.list->size())
        throw vm.runtimeError(Thrown::ConcurrentModificationException, "the list a view is of has become shorter than the view");
    return parts;
}

size_t viewIndex(VM& vm, const ViewParts& parts, const Value& i) {
    const int64_t at = vm.toInt(i, "a position");
    if (at < 0 || static_cast<size_t>(at) >= parts.size)
        throw vm.runtimeError(Thrown::IndexOutOfBoundsException, "position " + std::to_string(at) + " is not in a view of " + std::to_string(parts.size) + " items");
    return parts.start + static_cast<size_t>(at);
}

// A view the code running may write through: its own, and not split.
ViewParts writableView(VM& vm, const Value& v) {
    ViewParts parts = viewOf(vm, v);
    if (parts.view->core != vm.coreId())
        throw vm.runtimeError(Thrown::IllegalStateException, "a parallel section writes only through the views given to it: this one is another piece's, or shared");
    if (parts.lent) throw vm.runtimeError(Thrown::IllegalStateException, "this view has been split: its halves are written through until splitAt's block ends");
    if (parts.ended) throw vm.runtimeError(Thrown::IllegalStateException, "this view's splitAt has ended: a view is written through inside the block it was handed to");
    return parts;
}

}  // namespace

// ---- Shared collections (D94) ----
//
// A SharedMap keeps its entries in 16 maps by the key's hash (G49), any of which any piece of a
// section may change, each change whole: a part's lock, one of the few shared by address, is
// held for an instant inside, and never while the program's code runs, so cores changing
// different keys seldom take turns. What goes in has to be shareable: a value, text, frozen
// data, or an object nothing in the section changes.

namespace {

constexpr size_t kSharedParts = 16;

std::mutex& sharedLock(const Obj* map) { return atomicLocks[(reinterpret_cast<uintptr_t>(map) >> 4) & 63]; }

std::vector<Value>& sharedParts(VM& vm, const Value& v) {
    if (!v.isObjType(ObjType::List) || asList(v)->values().size() != kSharedParts)
        throw vm.runtimeError(Thrown::InternalError, "a SharedMap's entries are its parts");
    return asList(v)->values();
}

// The part the key is kept in, and the key's hash, worked out before any lock is taken.
ObjMap* sharedPart(VM& vm, const Value& v, const Value& key, uint32_t& hash) {
    hash = ValueMap::hashOf(key);
    return asMap(sharedParts(vm, v)[(hash * 0x9E3779B1u) >> 28]);
}

void shareable(VM& vm, const Value& v, const char* what) {
    if (!v.isObj() || v.as.obj->frozen != 0 || v.as.obj->type == ObjType::String) return;
    if (vm.coreId() != 0 && v.as.obj->core == vm.coreId())
        throw vm.runtimeError(Thrown::IllegalStateException, std::string("a SharedMap or SharedSet in a parallel section takes ") + what +
                                                                 " that every core may read: values, text, frozen data, or objects not changed here; not this " +
                                                                 vm.typeName(v) + ", which this core made and may still change");
}

}  // namespace

void registerShared(VM& vm, ObjClass* parallel) {
    auto def = [&](const char* name, int argc, NativeFn fn) {
        parallel->statics.set(vm.intern(name), Value::object(vm.alloc<ObjNative>(std::string("parallel.") + name, argc, argc, fn)));
    };

    def("viewGet", 2, [](VM& vm, int, Value* args) -> Value {
        vm.guardRead(args[0]);
        const ViewParts parts = viewOf(vm, args[0]);
        // While views of the list are written by the pieces they were given to, each reads
        // through its own and nothing else reads the list.
        if (parts.list->core == kViewedInParts && parts.view->core != vm.coreId()) throw vm.runtimeError(Thrown::IllegalStateException, kViewedMessage);
        return vm.listGet(parts.list, viewIndex(vm, parts, args[1]));
    });
    def("viewSet", 3, [](VM& vm, int, Value* args) -> Value {
        const ViewParts parts = writableView(vm, args[0]);
        vm.listSet(parts.list, viewIndex(vm, parts, args[1]), args[2]);
        return Value::null();
    });
    // Whether the code running may change this list: what splitAt asks of a list it splits.
    def("owns", 1, [](VM& vm, int, Value* args) -> Value {
        return Value::boolean(args[0].isObj() && args[0].as.obj->core == vm.coreId() && args[0].as.obj->frozen == 0);
    });
    // The list lent to the views of a splitAt or a parallelSort until it is given back: split
    // again meanwhile, it would give views that overlap those (D96).
    def("lend", 1, [](VM& vm, int, Value* args) -> Value {
        if (!args[0].isObjType(ObjType::List)) throw vm.runtimeError(Thrown::InternalError, "a list is lent");
        if (args[0].as.obj->lent)
            throw vm.runtimeError(Thrown::IllegalStateException, "this list is split already: until that splitAt's block ends, split its views instead (left.splitAt(...))");
        args[0].as.obj->lent = true;
        return Value::null();
    });
    def("giveBack", 1, [](VM&, int, Value* args) -> Value {
        if (args[0].isObj()) args[0].as.obj->lent = false;
        return Value::null();
    });
    // The view's items sorted in place, in their natural order; with `mid`, its two halves, each
    // sorted, merged.
    def("viewSort", 2, [](VM& vm, int, Value* args) -> Value {
        const ViewParts parts = writableView(vm, args[0]);
        std::vector<Value> items;
        items.reserve(parts.size);
        for (size_t i = 0; i < parts.size; i++) items.push_back(vm.listGet(parts.list, parts.start + i));
        TempVector hold(vm, items);
        auto less = [&](const Value& a, const Value& b) { return defaultLess(vm, a, b); };
        if (args[1].isNull()) {
            std::stable_sort(items.begin(), items.end(), less);
        } else {
            const size_t mid = std::min(static_cast<size_t>(vm.toInt(args[1], "the middle")), items.size());
            std::inplace_merge(items.begin(), items.begin() + static_cast<std::ptrdiff_t>(mid), items.end(), less);
        }
        for (size_t i = 0; i < parts.size; i++) vm.listSet(parts.list, parts.start + i, items[i]);
        return Value::null();
    });

    def("sharedMap", 0, [](VM& vm, int, Value*) -> Value {
        ObjList* parts = newList(vm);
        TempRoot keep(vm, Value::object(parts));
        for (size_t i = 0; i < kSharedParts; i++) parts->values().push_back(Value::object(vm.alloc<ObjMap>()));
        return Value::object(parts);
    });
    def("sharedGet", 2, [](VM& vm, int, Value* args) -> Value {
        uint32_t hash = 0;
        ObjMap* map = sharedPart(vm, args[0], args[1], hash);
        std::lock_guard<std::mutex> hold(sharedLock(map));
        const Value* found = map->map.find(args[1], hash);
        return found != nullptr ? *found : Value::null();
    });
    def("sharedHas", 2, [](VM& vm, int, Value* args) -> Value {
        uint32_t hash = 0;
        ObjMap* map = sharedPart(vm, args[0], args[1], hash);
        std::lock_guard<std::mutex> hold(sharedLock(map));
        return Value::boolean(map->map.find(args[1], hash) != nullptr);
    });
    def("sharedPut", 3, [](VM& vm, int, Value* args) -> Value {
        shareable(vm, args[1], "keys");
        shareable(vm, args[2], "values");
        uint32_t hash = 0;
        ObjMap* map = sharedPart(vm, args[0], args[1], hash);
        std::lock_guard<std::mutex> hold(sharedLock(map));
        Value* found = const_cast<Value*>(map->map.find(args[1], hash));
        const Value before = found != nullptr ? *found : Value::null();
        if (found != nullptr) *found = args[2];
        else map->map.addNew(args[1], args[2], hash);
        return before;
    });
    // Adds the key with the value if it is not there: whether it was added.
    def("sharedAdd", 3, [](VM& vm, int, Value* args) -> Value {
        shareable(vm, args[1], "keys");
        shareable(vm, args[2], "values");
        uint32_t hash = 0;
        ObjMap* map = sharedPart(vm, args[0], args[1], hash);
        std::lock_guard<std::mutex> hold(sharedLock(map));
        if (map->map.find(args[1], hash) != nullptr) return Value::boolean(false);
        map->map.addNew(args[1], args[2], hash);
        return Value::boolean(true);
    });
    def("sharedRemove", 2, [](VM& vm, int, Value* args) -> Value {
        uint32_t hash = 0;
        ObjMap* map = sharedPart(vm, args[0], args[1], hash);
        std::lock_guard<std::mutex> hold(sharedLock(map));
        const Value* found = map->map.find(args[1], hash);
        if (found == nullptr) return Value::null();
        const Value before = *found;
        map->map.remove(args[1]);
        return before;
    });
    // How many keys it holds: each part's count as it is when it is looked at.
    def("sharedSize", 1, [](VM& vm, int, Value* args) -> Value {
        int64_t size = 0;
        for (const Value& part : sharedParts(vm, args[0])) {
            std::lock_guard<std::mutex> hold(sharedLock(part.as.obj));
            size += static_cast<int64_t>(asMap(part)->map.size());
        }
        return Value::integer(size);
    });
    // Sets the key's value to `value` if it holds `expected` (the same value, or the very same
    // object; null for none): whether it did. What putIfAbsent and update are built on.
    def("sharedSwap", 4, [](VM& vm, int, Value* args) -> Value {
        shareable(vm, args[3], "values");
        uint32_t hash = 0;
        ObjMap* map = sharedPart(vm, args[0], args[1], hash);
        std::lock_guard<std::mutex> hold(sharedLock(map));
        Value* found = const_cast<Value*>(map->map.find(args[1], hash));
        const Value now = found != nullptr ? *found : Value::null();
        if (found == nullptr ? !args[2].isNull() : !sameValue(now, args[2])) return Value::boolean(false);
        shareable(vm, args[1], "keys");
        if (found != nullptr) *found = args[3];
        else map->map.addNew(args[1], args[3], hash);
        return Value::boolean(true);
    });
    // A copy of the entries as they are, an ordinary map, each part's as it is when it is
    // looked at: [keys, values].
    def("sharedCopy", 1, [](VM& vm, int, Value* args) -> Value {
        std::vector<Value> keys, values;
        for (const Value& part : sharedParts(vm, args[0])) {
            std::lock_guard<std::mutex> hold(sharedLock(part.as.obj));
            for (const ValueMap::Entry& entry : asMap(part)->map.entries())
                if (entry.live) {
                    keys.push_back(entry.key);
                    values.push_back(entry.value);
                }
        }
        TempVector holdKeys(vm, keys);
        TempVector holdValues(vm, values);
        ObjMap* copy = vm.alloc<ObjMap>();
        TempRoot keep(vm, Value::object(copy));
        for (size_t i = 0; i < keys.size(); i++) copy->map.set(keys[i], values[i]);
        return Value::object(copy);
    });
}

void registerParallel(VM& vm) {
    registerAtomics(vm);
    ObjClass* parallel = vm.alloc<ObjClass>(vm.intern("parallel"));
    auto def = [&](const char* name, int minArgs, int maxArgs, NativeFn fn) {
        parallel->statics.set(vm.intern(name), Value::object(vm.alloc<ObjNative>(std::string("parallel.") + name, minArgs, maxArgs, fn)));
    };

    // run(source, function, keep): the function on every item, the items dealt out to the
    // cores. With `keep`, the list of what it gave, in the order of the items.
    def("run", 3, 3, [](VM& vm, int, Value* args) -> Value { return vm.runParallel(args[0], args[1], args[2].truthy()); });
    // replace(list, function): each item of the list replaced by what the function gives for it.
    def("replace", 2, 2, [](VM& vm, int, Value* args) -> Value { return vm.runParallel(args[0], args[1], false, true); });

    // chunks(buffer, function): the function once on each core, with the range of positions
    // of the buffer that the core may write.
    // chunks(buffer, function, align): parts starting at multiples of `align` (a grid's rows).
    def("chunks", 2, 3, [](VM& vm, int argc, Value* args) -> Value {
        vm.runParallelChunks(args[0], args[1], argc > 2 ? static_cast<size_t>(std::max<int64_t>(1, vm.toInt(args[2], "alignment"))) : 1);
        return Value::null();
    });

    // functions(list): each function of the list at once (D96); what they gave, in order.
    def("functions", 1, 1, [](VM& vm, int, Value* args) -> Value {
        if (!args[0].isObjType(ObjType::List)) throw vm.runtimeError(Thrown::ClassCastException, "parallel takes functions");
        std::vector<Value> functions;
        for (size_t i = 0; i < asList(args[0])->size(); i++) functions.push_back(vm.listGet(asList(args[0]), i));
        TempVector hold(vm, functions);
        return vm.runParallelFunctions(functions);
    });
    // gather(source, into, each): each piece's result, made by `into` and each of its items
    // added by `each` (D93), in order.
    def("gather", 3, 3, [](VM& vm, int, Value* args) -> Value { return vm.runParallelGather(args[0], args[1], args[2]); });

    def("cores", 0, 0, [](VM& vm, int, Value*) -> Value { return Value::integer(vm.parallelCores()); });
    registerShared(vm, parallel);

    vm.defineGlobal("__parallel", Value::object(parallel));
}

}  // namespace ww
