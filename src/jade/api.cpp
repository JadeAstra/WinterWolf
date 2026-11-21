// Jade's native layer as the library sees it: the built-in __jade, which lib/Jade is
// written on. Windows, the raw events they receive, pictures on the graphics card
// ("surfaces"), the drawing operations of the toolkit's canvas, the clipboard and the clock.
//
// The library works in layout units; everything below this file works in device pixels.
// The crossing is here: a position is multiplied by the scale and rounded to a whole
// pixel, so that the edges of things are sharp at any scale and neighbours still meet.
//
// Windows and drawing belong to the main thread, the one the event loop runs on. The
// window system's events are handled from that loop (VM::loopPump), so a program with a
// window open goes on until its windows are closed, with no loop of its own to write.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <memory>

#include "../builtins.h"
#include "../fmath.h"
#include "../sandbox.h"
#include "../vm.h"
#include "platform.h"
#include "portal.h"
#include "render.h"
#include "../text/shapes.h"

#include "../section_window.h"  // last: this file's own code goes in its section (D161)

namespace ww {

std::shared_ptr<text::Font> textFontOf(VM& vm, Value v);  // text/api.cpp

namespace {

using jade::Platform;
using jade::PlatformWindow;
using jade::RawEvent;
using jade::Renderer;

struct Window {
    std::unique_ptr<PlatformWindow> native;
    int surface = 0;
    int frame = 0;           // the title bar and edges the program draws, where it does (windowFrameSurface)
    bool holdsLoop = false;  // shown: the program goes on while it is
};

struct State {
    VM* vm = nullptr;
    Renderer* renderer = nullptr;
    std::map<int, Window> windows;
    int nextWindow = 1;

    bool hasDispatcher = false;
    size_t dispatcherPin = 0;
    Value dispatcher;

    // An error raised by the program while it was being told of an event. It cannot leave
    // through the window system's own code, so it is kept and raised again once we are out.
    bool hasThrow = false, hasExit = false;
    WwThrow pendingThrow;
    int exitCode = 0;
    bool pumping = false;
    bool idling = false;
    // Windows closed from inside one of their own events: the event is still on its way
    // through the window's code, so the window itself goes once that has returned.
    int delivering = 0;
    std::vector<std::unique_ptr<PlatformWindow>> closed;

    // Where the drawing calls draw: set by `target`.
    int originX = 0, originY = 0;
    double scale = 1;
    uint32_t color = 0x000000FF;
    double lineWidth = 1;  // how thick outlines and lines are, in units (M8)
    std::shared_ptr<text::Font> font;

    // For tests: a clipboard of the program's own, so that the system's is left alone.
    bool ownClipboard = false;
    std::string clipboard;
};

// Made the first time it is needed, not as the program starts: this file's code is left out
// of a built program that does not use it, start-up code included (M6.5 part 5).
State& state() {
    static State made;
    return made;
}

// The pixels of an image's grid (a Grid<Color>), four bytes each, where the grid keeps them:
// nothing is copied (8.9f). `count` is how many the picture they go to has.
const uint8_t* gridPixels(VM& vm, Value grid, size_t count) {
    Value cells;
    if (!grid.isObjType(ObjType::Instance) || !asInstance(grid)->readText(vm.intern("cells"), cells) || !cells.isObjType(ObjType::List))
        throw vm.runtimeError(Thrown::ClassCastException, "expected an image's pixels");
    ObjList* list = asList(cells);
    if (!list->packed() || list->pack->width != 4 || list->pack->count != count)
        throw vm.runtimeError(Thrown::IllegalArgumentException, "the pixels do not fit the picture");
    return list->pack->block.data();
}

Renderer& need(VM& vm) {
    if (!vm.onLoopThread()) throw vm.runtimeError(Thrown::IllegalStateException, "windows and drawing belong to the main thread; hand the work to it with EventLoop.post");
    if (state().renderer == nullptr) {
        std::string error;
        state().renderer = Renderer::get(error);
        if (state().renderer == nullptr) throw vm.runtimeError(Thrown::IllegalStateException, "no windows or drawing here: " + error);
    }
    return *state().renderer;
}

Window& windowOf(VM& vm, Value v) {
    auto found = state().windows.find(vm.toInt32(v, "window"));
    if (found == state().windows.end()) throw vm.runtimeError(Thrown::IllegalStateException, "the window has been closed");
    return found->second;
}

void release(VM& vm, Window& window) {
    if (!window.holdsLoop) return;
    window.holdsLoop = false;
    vm.jobFinished();
}

// A surface is given as the value surfaceCreate made, or as a window's own by its number.
int surfaceOf(VM& vm, Value v) {
    if (v.isObjType(ObjType::Resource)) return static_cast<int>(static_cast<const ObjResource*>(v.as.obj)->id);
    return vm.toInt32(v, "surface");
}

jade::Surface& surfaceNeeded(VM& vm, Renderer& r, Value v) {
    jade::Surface* s = r.surface(surfaceOf(vm, v));
    if (s == nullptr) throw vm.runtimeError(Thrown::IllegalStateException, "the picture has been disposed of");
    return *s;
}

// From 0xAARRGGBB, as programs write colours, to 0xRRGGBBAA.
uint32_t colorOf(VM& vm, Value v) {
    const uint64_t argb = static_cast<uint64_t>(vm.toInt(v, "colour"));
    return static_cast<uint32_t>(((argb & 0xFFFFFF) << 8) | ((argb >> 24) & 0xFF));
}

// From layout units to device pixels: a place, and a thickness.
int dx(double x) { return state().originX + static_cast<int>(std::lround(x * state().scale)); }
int dy(double y) { return state().originY + static_cast<int>(std::lround(y * state().scale)); }
int thickness() { return std::max(1, static_cast<int>(std::lround(state().scale))); }
// How thick a line of the canvas's lineWidth is, in whole pixels: at least one.
int strokePixels() { return std::max(1, static_cast<int>(std::lround(state().lineWidth * state().scale))); }
double num(VM& vm, Value v) { return vm.toNumber(v, "number"); }

// ---- Events ----

Value done(Value result = Value::null()) {
    if (state().pumping) return result;  // raised once the window system's code has been left
    if (state().hasThrow) {
        state().hasThrow = false;
        WwThrow thrown = state().pendingThrow;
        state().pendingThrow = WwThrow();
        throw thrown;
    }
    if (state().hasExit) {
        state().hasExit = false;
        throw WwExit{state().exitCode};
    }
    return result;
}

// Tells the program of an event: dispatcher(window, kind, x, y, n1, n2, f1, f2, extra).
void dispatch(int window, const char* kind, int64_t x = 0, int64_t y = 0, int64_t n1 = 0, int64_t n2 = 0, double f1 = 0, double f2 = 0,
              Value extra = Value::null()) {
    if (!state().hasDispatcher || state().hasThrow || state().hasExit) return;
    VM& vm = *state().vm;
    std::vector<Value> args = {Value::integer(window), Value::null(), Value::integer(x),  Value::integer(y), Value::integer(n1),
                               Value::integer(n2),     Value::number(f1), Value::number(f2), extra};
    TempVector keep(vm, args);
    args[1] = Value::object(vm.intern(kind));
    try {
        vm.call(state().dispatcher, static_cast<int>(args.size()), args.data());
    } catch (WwThrow& thrown) {
        state().pendingThrow = thrown;
        state().hasThrow = true;
    } catch (WwExit& exit) {
        state().exitCode = exit.code;
        state().hasExit = true;
    }
}

// One event of the window system, as it goes to the program: positions in layout units.
void deliver(int id, const RawEvent& e) {
    auto found = state().windows.find(id);
    if (found == state().windows.end()) return;
    Window& window = found->second;
    VM& vm = *state().vm;
    state().delivering++;
    struct Leave {
        ~Leave() { state().delivering--; }
    } leave;
    const double scale = window.native->scale();
    auto unit = [&](int device) { return static_cast<int64_t>(std::floor(device / scale)); };

    switch (e.kind) {
        case RawEvent::PointerMove:
            dispatch(id, "pointerMove", unit(e.x), unit(e.y), e.buttons, e.modifiers, e.dx / scale, e.dy / scale);
            break;
        case RawEvent::PointerDown:
            dispatch(id, "pointerDown", unit(e.x), unit(e.y), e.button, e.modifiers);
            break;
        case RawEvent::PointerUp:
            dispatch(id, "pointerUp", unit(e.x), unit(e.y), e.button, e.modifiers);
            break;
        case RawEvent::PointerLeave:
            dispatch(id, "pointerLeave");
            break;
        case RawEvent::Wheel:
            dispatch(id, "wheel", unit(e.x), unit(e.y), 0, e.modifiers, e.dx, e.dy);
            break;
        case RawEvent::KeyDown:
        case RawEvent::KeyUp:
            dispatch(id, e.kind == RawEvent::KeyDown ? "keyDown" : "keyUp", e.letter, 0, e.repeat ? 1 : 0, e.modifiers, 0, 0,
                     Value::object(vm.intern(jade::keyName(e.key))));
            break;
        case RawEvent::Text: {
            TempRoot text(vm, vm.str(e.text));
            dispatch(id, "text", 0, 0, 0, 0, 0, 0, text.get());
            break;
        }
        case RawEvent::Focus:
            dispatch(id, "focus", 0, 0, e.on ? 1 : 0);
            break;
        case RawEvent::Resize:
            // The window's picture follows its size, keeping what was on it: the program
            // draws it again when it hears of the new size, from inside this very call if
            // an edge is being dragged.
            if (state().renderer != nullptr && e.width > 0 && e.height > 0) state().renderer->resizeSurface(window.surface, e.width, e.height);
            dispatch(id, "resize", unit(e.width), unit(e.height), e.width, e.height, scale);
            break;
        case RawEvent::Scale:
            dispatch(id, "scale", 0, 0, 0, 0, e.scale);
            break;
        case RawEvent::Close:
            dispatch(id, "close");
            break;
        case RawEvent::FilesDropped: {
            ObjList* files = newList(vm);
            TempRoot keep(vm, Value::object(files));
            for (const std::string& path : e.files) files->values().push_back(vm.str(path));
            dispatch(id, "filesDropped", unit(e.x), unit(e.y), 0, 0, 0, 0, Value::object(files));
            break;
        }
        // Over the title bar the program draws (from the outer corner of all the window draws).
        case RawEvent::FrameMove:
            dispatch(id, "frameMove", unit(e.x), unit(e.y), e.buttons, e.modifiers);
            break;
        case RawEvent::FrameDown:
            dispatch(id, "frameDown", unit(e.x), unit(e.y), e.button, e.modifiers);
            break;
        case RawEvent::FrameUp:
            dispatch(id, "frameUp", unit(e.x), unit(e.y), e.button, e.modifiers);
            break;
        case RawEvent::FrameLeave:
            dispatch(id, "frameLeave");
            break;
        case RawEvent::State:
            dispatch(id, "state", 0, 0, e.state);
            break;
    }
}

void raisePending() {
    if (state().hasThrow) {
        state().hasThrow = false;
        WwThrow thrown = state().pendingThrow;
        state().pendingThrow = WwThrow();
        throw thrown;
    }
    if (state().hasExit) {
        state().hasExit = false;
        throw WwExit{state().exitCode};
    }
}

// The window system's events, handled from the runtime's event loop.
void pump(VM&) {
    if (state().renderer == nullptr || state().pumping) return;
    state().pumping = true;
    state().renderer->platform().pumpEvents();
    state().pumping = false;
    state().closed.clear();
    raisePending();
}

void waitForEvents(int64_t millis) {
    if (state().renderer != nullptr) state().renderer->platform().waitEvents(millis);
}

void wakeLoop() {
    std::string unused;
    if (Platform* platform = Platform::get(unused)) platform->wake();
}

// While the system runs a loop of its own (a window being dragged by its title bar or an
// edge), the program's tasks and timers are run from here, so an animation goes on
// meanwhile. Only for a moment at a time: the system's loop has to get its turn back, or
// the drag never moves and the window stops answering. An animation whose frame takes
// longer than its timer's period always has a frame due, so this cannot wait for "nothing
// left to do".
void idle() {
    VM& vm = *state().vm;
    if (state().hasThrow || state().hasExit || state().idling) return;
    state().idling = true;
    struct Reset {
        ~Reset() { state().idling = false; }
    } reset;
    try {
        vm.runEventLoop(false, 2);
    } catch (WwThrow& thrown) {
        state().pendingThrow = thrown;
        state().hasThrow = true;
    } catch (WwExit& exit) {
        state().exitCode = exit.code;
        state().hasExit = true;
    }
}

void hookLoop(VM& vm) {
    if (vm.loopPump != nullptr) return;
    vm.loopPump = pump;
    vm.loopWait = waitForEvents;
    vm.onLoopWake = wakeLoop;
    state().renderer->platform().onIdle = idle;
    // The program's Palette, where it follows the system's look, takes it again (Native.ww).
    state().renderer->platform().onAppearanceChanged = [] { dispatch(0, "appearance"); };
}

RawEvent syntheticEvent(VM& vm, Window& window, const std::string& kind, Value* args) {
    const double scale = window.native->scale();
    auto device = [&](Value v) { return static_cast<int>(std::lround(vm.toNumber(v, "position") * scale)); };
    RawEvent e;
    if (kind == "pointerMove" || kind == "pointerDown" || kind == "pointerUp" || kind == "wheel" || kind == "filesDropped") {
        e.x = device(args[0]);
        e.y = device(args[1]);
    }
    if (kind == "pointerMove") {
        e.kind = RawEvent::PointerMove;
        e.buttons = vm.toInt32(args[2], "buttons");
    } else if (kind == "pointerDown" || kind == "pointerUp") {
        e.kind = kind == "pointerDown" ? RawEvent::PointerDown : RawEvent::PointerUp;
        e.button = vm.toInt32(args[2], "button");
    } else if (kind == "pointerLeave") {
        e.kind = RawEvent::PointerLeave;
    } else if (kind == "wheel") {
        e.kind = RawEvent::Wheel;
        e.dx = vm.toNumber(args[4], "dx");
        e.dy = vm.toNumber(args[5], "dy");
    } else if (kind == "keyDown" || kind == "keyUp") {
        e.kind = kind == "keyDown" ? RawEvent::KeyDown : RawEvent::KeyUp;
        e.key = jade::keyNamed(vm.toStr(args[6], "key"));
        e.letter = static_cast<uint32_t>(vm.toInt(args[0], "letter"));
        e.repeat = args[2].truthy() && !(args[2].isInt() && args[2].as.i == 0);
    } else if (kind == "text") {
        e.kind = RawEvent::Text;
        e.text = vm.toStr(args[6], "text");
    } else if (kind == "focus") {
        e.kind = RawEvent::Focus;
        e.on = args[2].truthy() && !(args[2].isInt() && args[2].as.i == 0);
    } else if (kind == "close") {
        e.kind = RawEvent::Close;
    } else if (kind == "filesDropped") {
        e.kind = RawEvent::FilesDropped;
        if (args[6].isObjType(ObjType::List))
            for (const Value& path : vm.listCopy(asList(args[6]))) e.files.push_back(vm.toStr(path, "file path"));
    } else {
        throw vm.runtimeError(Thrown::IllegalStateException, "no such event: " + kind);
    }
    e.modifiers = vm.toInt32(args[3], "modifiers");
    return e;
}

Value pairOf(VM& vm, std::initializer_list<Value> values) {
    ObjList* list = newList(vm, std::vector<Value>(values));
    return Value::object(list);
}

}  // namespace

void registerJade(VM& vm) {
    state().vm = &vm;
    ObjClass* lib = vm.alloc<ObjClass>(vm.intern("jade"));
    auto def = [&](const char* name, int minArgs, int maxArgs, NativeFn fn) {
        lib->statics.set(vm.intern(name), Value::object(vm.alloc<ObjNative>(std::string("jade.") + name, minArgs, maxArgs, fn)));
    };

    // ---- The system ----

    // Whether there is anything to draw with here; if not, why.
    def("problem", 0, 0, [](VM& vm, int, Value*) -> Value {
        if (!vm.onLoopThread()) return vm.str("windows and drawing belong to the main thread");
        std::string error;
        if (state().renderer == nullptr) state().renderer = Renderer::get(error);
        return state().renderer != nullptr ? Value::null() : vm.str(error);
    });
    def("describe", 0, 0, [](VM& vm, int, Value*) -> Value { return vm.str(need(vm).description()); });
    // [shapes, batches] sent to the graphics card since the program began, and how many
    // surfaces there are now.
    def("counters", 0, 0, [](VM& vm, int, Value*) -> Value {
        Renderer& r = need(vm);
        r.flush();  // which also lets go of the surfaces the collector has found unused
        return pairOf(vm, {Value::integer(r.shapesDrawn()), Value::integer(r.batchesDrawn()), Value::integer(static_cast<int64_t>(r.surfaceCount()))});
    });
    def("setDispatcher", 1, 1, [](VM& vm, int, Value* args) -> Value {
        if (state().hasDispatcher) {
            vm.pinned[state().dispatcherPin] = args[0];
        } else {
            state().dispatcherPin = vm.pinned.size();
            vm.pinned.push_back(args[0]);
        }
        state().dispatcher = args[0];
        state().hasDispatcher = true;
        return Value::null();
    });
    // A clock that only goes forwards, in nanoseconds.
    def("nanos", 0, 0, [](VM&, int, Value*) -> Value {
        return Value::integer(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    });
    def("doubleClickTime", 0, 0, [](VM& vm, int, Value*) -> Value { return Value::integer(need(vm).platform().doubleClickTime()); });
    def("screenScale", 0, 0, [](VM& vm, int, Value*) -> Value { return Value::number(need(vm).platform().screenScale()); });
    // What the desktop's portal says of its look, on Linux (Jade.systemDark, Jade.systemAccent):
    // true for dark, false for light or no preference, null where it cannot be told; the accent
    // as 0xRRGGBB, or null. Null elsewhere, and in a sandbox.
    def("desktopDark", 0, 0, [](VM& vm, int, Value*) -> Value {
        if (vm.currentHost().isSandbox()) return Value::null();
        const int scheme = jade::portal::colorScheme();
        return scheme < 0 ? Value::null() : Value::boolean(scheme == 1);
    });
    def("desktopSetting", 2, 2, [](VM& vm, int, Value* args) -> Value {
        std::string value;
        if (vm.currentHost().isSandbox() || !jade::portal::textSetting(vm.toStr(args[0], "group"), vm.toStr(args[1], "key"), value)) return Value::null();
        return vm.str(std::move(value));
    });
    def("desktopAccent", 0, 0, [](VM& vm, int, Value*) -> Value {
        uint32_t rgb = 0;
        if (vm.currentHost().isSandbox() || !jade::portal::accentColor(rgb)) return Value::null();
        return Value::integer(static_cast<int64_t>(rgb));
    });

    // chooseFile(kind, title, filters, startDir, name, window): the system's dialog for a file
    // to open (0), to save to (1) or a folder (2), waited for here with the program's other
    // threads going on (it is called on a thread of its own, SystemFileDialog in Window.ww);
    // in front of the window with that number, or none for 0. The path chosen, or null.
    def("chooseFile", 6, 6, [](VM& vm, int, Value* args) -> Value {
        if (vm.currentHost().isSandbox()) throw vm.runtimeError(Thrown::UnsupportedOperationException, "a sandbox shows no file dialogs");
        const int64_t kind = vm.toInt(args[0], "kind");
        const std::string title = vm.toStr(args[1], "title");
        std::vector<std::pair<std::string, std::string>> filters;
        if (args[2].isObjType(ObjType::List)) {
            for (const Value& f : vm.listCopy(asList(args[2]))) {
                if (!f.isObjType(ObjType::List)) continue;
                const std::vector<Value> pair = vm.listCopy(asList(f));
                if (pair.size() == 2) filters.push_back({vm.toStr(pair[0], "filter"), vm.toStr(pair[1], "filter")});
            }
        }
        const std::string startDir = args[3].isNull() ? std::string() : vm.toStr(args[3], "folder");
        const std::string name = args[4].isNull() ? std::string() : vm.toStr(args[4], "name");
        PlatformWindow* owner = nullptr;
        if (!args[5].isNull()) {
            auto found = state().windows.find(static_cast<int>(vm.toInt(args[5], "window")));
            if (found != state().windows.end()) owner = found->second.native.get();
        }
        std::string error;
        Platform* platform = Platform::get(error);
        if (platform == nullptr) throw vm.errorOf("IOException", "no file dialog: " + error);
        std::string path;
        bool chosen;
        {
            LockReleased waiting(vm);
            chosen = platform->chooseFile(static_cast<int>(kind), title, filters, startDir, name, owner, path, error);
        }
        if (!error.empty()) throw vm.errorOf("IOException", error);
        return chosen ? vm.str(std::move(path)) : Value::null();
    });

    def("clipboardOwn", 1, 1, [](VM&, int, Value* args) -> Value {
        state().ownClipboard = args[0].truthy();
        return Value::null();
    });
    // In a sandbox, a clipboard of its own (M6.6.2).
    def("clipboardGet", 0, 0, [](VM& vm, int, Value*) -> Value {
        if (vm.currentHost().isSandbox()) {
            auto& box = static_cast<SandboxHost&>(vm.currentHost());
            std::lock_guard<std::mutex> hold(box.clipboardMutex);
            return vm.str(box.clipboard);
        }
        if (state().ownClipboard) return vm.str(state().clipboard);
        return vm.str(need(vm).platform().clipboardText());
    });
    def("clipboardSet", 1, 1, [](VM& vm, int, Value* args) -> Value {
        const std::string& text = vm.toStr(args[0], "text");
        if (vm.currentHost().isSandbox()) {
            auto& box = static_cast<SandboxHost&>(vm.currentHost());
            std::lock_guard<std::mutex> hold(box.clipboardMutex);
            box.clipboard = text;
        } else if (state().ownClipboard) state().clipboard = text;
        else need(vm).platform().setClipboardText(text);
        return Value::null();
    });

    // ---- Windows ----

    // windowCreate(title, width, height, resizable, dialog, owner or 0, scale or 0): a window,
    // not yet shown. The size is of what can be drawn on, in layout units.
    def("windowCreate", 7, 7, [](VM& vm, int, Value* args) -> Value {
        Renderer& r = need(vm);
        jade::WindowSettings settings;
        settings.title = vm.toStr(args[0], "title");
        settings.width = static_cast<int>(std::clamp<int64_t>(vm.toInt(args[1], "width"), 1, 16384));
        settings.height = static_cast<int>(std::clamp<int64_t>(vm.toInt(args[2], "height"), 1, 16384));
        settings.resizable = args[3].truthy();
        settings.dialog = args[4].truthy();
        settings.scale = vm.toNumber(args[6], "scale");
        PlatformWindow* owner = nullptr;
        const int64_t ownerId = vm.toInt(args[5], "owner");
        if (ownerId != 0) owner = windowOf(vm, args[5]).native.get();

        std::string error;
        const int id = state().nextWindow++;
        Window window;
        window.native = r.platform().createWindow(settings, owner, error);
        if (window.native == nullptr) throw vm.runtimeError(Thrown::IllegalStateException, "the window cannot be made: " + error);
        window.surface = r.createSurface(std::max(window.native->width(), 1), std::max(window.native->height(), 1));
        if (window.surface == 0) throw vm.runtimeError(Thrown::IllegalStateException, "the window is too large to draw");
        window.native->onEvent = [id](const RawEvent& event) { deliver(id, event); };
        window.native->onExpose = [id] {
            auto found = state().windows.find(id);
            if (found != state().windows.end() && state().renderer != nullptr && state().renderer->present(found->second.surface, *found->second.native, found->second.frame))
                found->second.native->swapBuffers();
        };
        state().windows[id] = std::move(window);
        hookLoop(vm);
        return Value::integer(id);
    });

    def("windowShow", 1, 1, [](VM& vm, int, Value* args) -> Value {
        Window& window = windowOf(vm, args[0]);
        if (!window.holdsLoop) {
            window.holdsLoop = true;
            vm.jobStarted();
        }
        window.native->show();
        return done();
    });
    def("windowHide", 1, 1, [](VM& vm, int, Value* args) -> Value {
        Window& window = windowOf(vm, args[0]);
        window.native->hide();
        release(vm, window);
        return done();
    });
    // Closes a window for good. Closing one that is closed already does nothing.
    def("windowClose", 1, 1, [](VM& vm, int, Value* args) -> Value {
        auto found = state().windows.find(vm.toInt32(args[0], "window"));
        if (found == state().windows.end()) return Value::null();
        Renderer& r = need(vm);
        release(vm, found->second);
        r.destroySurface(found->second.surface);
        // Taken out of the table first: destroying it sends it messages still.
        Window gone = std::move(found->second);
        state().windows.erase(found);
        if (state().delivering > 0 || state().pumping) {
            // Closed from inside an event: out of sight now, and gone when the event has
            // finished its way through the window's own code.
            gone.native->hide();
            state().closed.push_back(std::move(gone.native));
        } else {
            gone.native.reset();
        }
        return done();
    });
    def("windowIsOpen", 1, 1, [](VM& vm, int, Value* args) -> Value {
        return Value::boolean(state().windows.count(vm.toInt32(args[0], "window")) != 0);
    });
    // windowOwner(window, owner or 0): the window given to another owner, or to none.
    def("windowOwner", 2, 2, [](VM& vm, int, Value* args) -> Value {
        PlatformWindow* owner = nullptr;
        if (vm.toInt(args[1], "owner") != 0) owner = windowOf(vm, args[1]).native.get();
        windowOf(vm, args[0]).native->setOwner(owner);
        return Value::null();
    });
    def("windowTitle", 2, 2, [](VM& vm, int, Value* args) -> Value {
        windowOf(vm, args[0]).native->setTitle(vm.toStr(args[1], "title"));
        return Value::null();
    });
    // [width, height] in layout units, [width, height] in device pixels, the scale, and the surface.
    def("windowInfo", 1, 1, [](VM& vm, int, Value* args) -> Value {
        Window& window = windowOf(vm, args[0]);
        const double scale = window.native->scale();
        const int w = window.native->width(), h = window.native->height();
        return pairOf(vm, {Value::integer(static_cast<int64_t>(std::floor(w / scale))), Value::integer(static_cast<int64_t>(std::floor(h / scale))),
                           Value::integer(w), Value::integer(h), Value::number(scale), Value::integer(window.surface)});
    });
    def("windowSetSize", 3, 3, [](VM& vm, int, Value* args) -> Value {
        Window& window = windowOf(vm, args[0]);
        const double scale = window.native->scale();
        window.native->setSize(static_cast<int>(std::lround(num(vm, args[1]) * scale)), static_cast<int>(std::lround(num(vm, args[2]) * scale)));
        return done();
    });
    // Where the window is on the screen, in the screen's own pixels.
    def("windowPosition", 1, 1, [](VM& vm, int, Value* args) -> Value {
        int x = 0, y = 0;
        windowOf(vm, args[0]).native->position(x, y);
        return pairOf(vm, {Value::integer(x), Value::integer(y)});
    });
    def("windowSetPosition", 3, 3, [](VM& vm, int, Value* args) -> Value {
        windowOf(vm, args[0]).native->setPosition(vm.toInt32(args[1], "x"), vm.toInt32(args[2], "y"));
        return done();
    });
    // windowLimits(window, minWidth, minHeight, maxWidth, maxHeight) in layout units; 0 for none.
    def("windowLimits", 5, 5, [](VM& vm, int, Value* args) -> Value {
        Window& window = windowOf(vm, args[0]);
        const double scale = window.native->scale();
        auto device = [&](Value v) { return static_cast<int>(std::lround(num(vm, v) * scale)); };
        window.native->setLimits(device(args[1]), device(args[2]), device(args[3]), device(args[4]));
        return Value::null();
    });
    def("windowResizable", 2, 2, [](VM& vm, int, Value* args) -> Value {
        windowOf(vm, args[0]).native->setResizable(args[1].truthy());
        return done();
    });
    def("windowToFront", 1, 1, [](VM& vm, int, Value* args) -> Value {
        windowOf(vm, args[0]).native->toFront();
        return done();
    });
    def("windowAttention", 1, 1, [](VM& vm, int, Value* args) -> Value {
        windowOf(vm, args[0]).native->requestAttention();
        return Value::null();
    });
    def("windowState", 1, 1, [](VM& vm, int, Value* args) -> Value {
        Window& window = windowOf(vm, args[0]);
        return pairOf(vm, {Value::boolean(window.native->visible()), Value::boolean(window.native->focused())});
    });
    // windowIcon(window, pixels, width, height): red, green, blue, alpha, four bytes a pixel.
    def("windowIcon", 4, 4, [](VM& vm, int, Value* args) -> Value {
        Window& window = windowOf(vm, args[0]);
        const int width = vm.toInt32(args[2], "width"), height = vm.toInt32(args[3], "height");
        const size_t count = static_cast<size_t>(std::max(width, 0)) * static_cast<size_t>(std::max(height, 0));
        const uint8_t* pixels = gridPixels(vm, args[1], count);
        window.native->setIcon(std::vector<uint8_t>(pixels, pixels + count * 4), width, height);
        return Value::null();
    });
    def("windowCursor", 2, 2, [](VM& vm, int, Value* args) -> Value {
        windowOf(vm, args[0]).native->setCursor(static_cast<jade::Cursor>(std::clamp<int64_t>(vm.toInt(args[1], "cursor"), 0, 8)));
        return Value::null();
    });
    def("windowPointerMode", 2, 2, [](VM& vm, int, Value* args) -> Value {
        windowOf(vm, args[0]).native->setPointerMode(static_cast<jade::PointerMode>(std::clamp<int64_t>(vm.toInt(args[1], "pointer mode"), 0, 2)));
        return Value::null();
    });
    // Shows on the screen what has been drawn on the window's surface.
    def("windowPresent", 1, 1, [](VM& vm, int, Value* args) -> Value {
        Window& window = windowOf(vm, args[0]);
        Renderer& r = need(vm);
        if (!window.native->visible()) {
            r.flush();
            return Value::null();
        }
        if (r.present(window.surface, *window.native, window.frame)) {
            // Showing it waits for the screen's next refresh; the program's other threads
            // have that time.
            PlatformWindow* native = window.native.get();
            LockReleased released(vm);
            native->swapBuffers();
        }
        return Value::null();
    });
    // windowDarkFrame(window, dark): the system's title bar dark or light, with the program's theme.
    def("windowDarkFrame", 2, 2, [](VM& vm, int, Value* args) -> Value {
        windowOf(vm, args[0]).native->setDarkFrame(args[1].truthy());
        return Value::null();
    });

    // ---- A title bar and edges the program draws (PlatformWindow::clientFrame) ----
    // Whether it draws them: where the system does not (GNOME, WSLg).
    def("windowClientFrame", 1, 1, [](VM& vm, int, Value* args) -> Value { return Value::boolean(windowOf(vm, args[0]).native->clientFrame()); });
    // windowSetFrame(window, bar, shadow): their sizes, in layout units; 0, 0 for none.
    def("windowSetFrame", 3, 3, [](VM& vm, int, Value* args) -> Value {
        windowOf(vm, args[0]).native->setFrame(vm.toInt32(args[1], "bar"), vm.toInt32(args[2], "shadow"));
        return Value::null();
    });
    // windowFrameSurface(window, surface or null): the picture of them, shown around what can be
    // drawn on (which is left for it).
    def("windowFrameSurface", 2, 2, [](VM& vm, int, Value* args) -> Value {
        windowOf(vm, args[0]).frame = args[1].isNull() ? 0 : surfaceOf(vm, args[1]);
        return Value::null();
    });
    // windowOuter(window): [width, height] of all the window draws and [x, y] of what can be drawn
    // on in it, in device pixels, and its state (kMaximized, kFullscreen, kTiled, kActivated).
    def("windowOuter", 1, 1, [](VM& vm, int, Value* args) -> Value {
        const PlatformWindow& native = *windowOf(vm, args[0]).native;
        int ox = 0, oy = 0;
        native.contentOffset(ox, oy);
        return pairOf(vm, {Value::integer(native.outerWidth()), Value::integer(native.outerHeight()), Value::integer(ox), Value::integer(oy),
                           Value::integer(native.frameState())});
    });
    // windowFrameAction(window, action, x, y): 1 move it (a press on the bar), 2 maximise it or
    // give it back its size, 3 minimise it, 4 the system's menu for it at (x, y), layout units from
    // the outer corner.
    def("windowFrameAction", 4, 4, [](VM& vm, int, Value* args) -> Value {
        PlatformWindow& native = *windowOf(vm, args[0]).native;
        const int64_t action = vm.toInt(args[1], "action");
        if (action < 1 || action > 4) throw vm.runtimeError(Thrown::IllegalArgumentException, "a frame action is 1 to 4");
        const double scale = native.scale();
        native.frameAction(static_cast<jade::FrameAction>(action), static_cast<int>(std::lround(num(vm, args[2]) * scale)),
                           static_cast<int>(std::lround(num(vm, args[3]) * scale)));
        return Value::null();
    });

    // windowSend(window, kind, x, y, n1, n2, f1, f2, extra): an event made up by a test,
    // which then goes the way a real one does.
    def("windowSend", 9, 9, [](VM& vm, int, Value* args) -> Value {
        Window& window = windowOf(vm, args[0]);
        const int id = vm.toInt32(args[0], "window");
        const RawEvent event = syntheticEvent(vm, window, vm.toStr(args[1], "kind of event"), args + 2);
        deliver(id, event);
        if (state().delivering == 0 && !state().pumping) state().closed.clear();
        return done();
    });

    // windowPost(window, what): for tests, a message put in the window's queue as the
    // system would put it ("moveBegan", "moveEnded", "close"), handled when events are
    // next pumped and not from inside this call.
    def("windowPost", 2, 2, [](VM& vm, int, Value* args) -> Value {
        windowOf(vm, args[0]).native->postForTest(vm.toStr(args[1], "kind of message"));
        return Value::null();
    });

    // ---- Surfaces ----

    // A picture on the graphics card, every pixel clear. In device pixels.
    def("surfaceCreate", 2, 2, [](VM& vm, int, Value* args) -> Value {
        Renderer& r = need(vm);
        const int64_t w = vm.toInt(args[0], "width"), h = vm.toInt(args[1], "height");
        const int64_t most = r.maxSurfaceSize();
        if (w < 1 || h < 1 || w > most || h > most)
            throw vm.runtimeError(Thrown::IllegalArgumentException, "a picture on the graphics card is from 1 to " + std::to_string(most) + " pixels each way, not " + std::to_string(w) +
                                  " by " + std::to_string(h));
        const int id = r.createSurface(static_cast<int>(w), static_cast<int>(h));
        if (id == 0) throw vm.runtimeError(Thrown::IllegalStateException, "the graphics card cannot hold a picture of " + std::to_string(w) + " by " + std::to_string(h));
        // What it takes on the card counts towards the next collection, which is what lets
        // go of the surfaces a program has stopped using.
        vm.account(static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
        return Value::object(vm.alloc<ObjResource>("surface", id, &Renderer::releaseLater));
    });
    def("surfaceLargestSide", 0, 0, [](VM& vm, int, Value*) -> Value { return Value::integer(need(vm).maxSurfaceSize()); });
    def("surfaceDispose", 1, 1, [](VM& vm, int, Value* args) -> Value {
        need(vm).destroySurface(surfaceOf(vm, args[0]));
        return Value::null();
    });
    def("surfaceSize", 1, 1, [](VM& vm, int, Value* args) -> Value {
        jade::Surface& s = surfaceNeeded(vm, need(vm), args[0]);
        return pairOf(vm, {Value::integer(s.width), Value::integer(s.height)});
    });
    // cushions(width, height, background, tiles, paints): a treemap's cushions (van Wijk and van de
    // Wetering, 1999), the pixels of TreemapRenderer.render. `tiles` is x, y, width, height and
    // depth for each tile, parents before children: each adds a parabolic bump over its own
    // rectangle, lower the deeper it is. `paints` is a tile's index and its colour (0xRRGGBB), in
    // the order they are painted: each pixel of that tile lit from up and to the left over the
    // bumps there. Red, green, blue and alpha, four bytes each, top row first; the rest is the
    // background. In C++ as it is worked out for every pixel of the map, several times over.
    def("cushions", 5, 5, [](VM& vm, int, Value* args) -> Value {
        const int64_t w = vm.toInt(args[0], "width"), h = vm.toInt(args[1], "height");
        if (w <= 0 || h <= 0 || w > 32768 || h > 32768)
            throw vm.runtimeError(Thrown::IllegalArgumentException, "a treemap of " + std::to_string(w) + " by " + std::to_string(h) + " pixels");
        const uint32_t background = static_cast<uint32_t>(vm.toInt(args[2], "background"));
        if (!args[3].isObjType(ObjType::List) || !args[4].isObjType(ObjType::List))
            throw vm.runtimeError(Thrown::IllegalArgumentException, "cushions takes the tiles and the paints as lists of numbers");
        const std::vector<Value> tiles = vm.listCopy(asList(args[3]));
        const std::vector<Value> paints = vm.listCopy(asList(args[4]));
        const size_t width = static_cast<size_t>(w), height = static_cast<size_t>(h), n = width * height;
        // The surface, kept as the sums of each bump's derivative along x and along y: for x, the
        // slope is 2 * x1 * x + x2 at each pixel.
        std::vector<double> x1(n), x2(n), y1(n), y2(n);
        constexpr double kHeight = 0.55, kScaleDown = 0.75;
        auto tile = [&](size_t i, int64_t& tx, int64_t& ty, int64_t& tw, int64_t& th, int64_t& depth) {
            tx = vm.toInt(tiles[i * 5], "tile");
            ty = vm.toInt(tiles[i * 5 + 1], "tile");
            tw = vm.toInt(tiles[i * 5 + 2], "tile");
            th = vm.toInt(tiles[i * 5 + 3], "tile");
            depth = vm.toInt(tiles[i * 5 + 4], "tile");
        };
        auto clampTo = [](int64_t v, int64_t hi) { return static_cast<size_t>(std::clamp<int64_t>(v, 0, hi)); };
        const size_t count = tiles.size() / 5;
        for (size_t t = 0; t < count; t++) {
            int64_t tx, ty, tw, th, depth;
            tile(t, tx, ty, tw, th, depth);
            if (tw <= 0 || th <= 0) continue;
            const double bump = kHeight * fmath::pow(kScaleDown, static_cast<double>(depth));
            // d/dv of 4h (v - lo)(hi - v) / (hi - lo), in the running sums, along each axis.
            const double slopeX = 4.0 * bump / static_cast<double>(tw), slopeY = 4.0 * bump / static_cast<double>(th);
            const double ax = -slopeX, bx = slopeX * static_cast<double>(2 * tx + tw);
            const double ay = -slopeY, by = slopeY * static_cast<double>(2 * ty + th);
            const size_t c0 = clampTo(tx, w), c1 = clampTo(tx + tw, w), r0 = clampTo(ty, h), r1 = clampTo(ty + th, h);
            for (size_t y = r0; y < r1; y++) {
                for (size_t x = c0, i = y * width + c0; x < c1; x++, i++) {
                    x1[i] += ax;
                    x2[i] += bx;
                    y1[i] += ay;
                    y2[i] += by;
                }
            }
        }
        // Lit from up and to the left; mostly ambient, so that the colour (what the map shows)
        // survives the shading, and summing to 1, so that a cushion's top is the colour itself.
        constexpr double kLightX = -0.09759, kLightY = -0.19518, kLightZ = 0.97590, kAmbient = 0.58, kDiffuse = 0.42;
        ObjBytes* made = vm.alloc<ObjBytes>();
        std::vector<uint8_t>& out = made->data;
        out.resize(n * 4);
        for (size_t i = 0; i < n; i++) {
            out[i * 4] = static_cast<uint8_t>(background >> 16);
            out[i * 4 + 1] = static_cast<uint8_t>(background >> 8);
            out[i * 4 + 2] = static_cast<uint8_t>(background);
            out[i * 4 + 3] = 0xFF;
        }
        auto shade = [](double channel, double light) { return static_cast<uint8_t>(std::clamp(static_cast<int>(channel * light), 0, 255)); };
        for (size_t p = 0; p + 1 < paints.size(); p += 2) {
            const int64_t index = vm.toInt(paints[p], "tile");
            if (index < 0 || static_cast<size_t>(index) >= count) continue;
            int64_t tx, ty, tw, th, depth;
            tile(static_cast<size_t>(index), tx, ty, tw, th, depth);
            const uint32_t color = static_cast<uint32_t>(vm.toInt(paints[p + 1], "colour"));
            const double r = (color >> 16) & 0xFF, g = (color >> 8) & 0xFF, b = color & 0xFF;
            const size_t c0 = clampTo(tx, w), c1 = clampTo(tx + tw, w), r0 = clampTo(ty, h), r1 = clampTo(ty + th, h);
            for (size_t y = r0; y < r1; y++) {
                for (size_t x = c0, i = y * width + c0; x < c1; x++, i++) {
                    const double nx = -(2.0 * x1[i] * (static_cast<double>(x) + 0.5) + x2[i]);
                    const double ny = -(2.0 * y1[i] * (static_cast<double>(y) + 0.5) + y2[i]);
                    double cosine = (kLightX * nx + kLightY * ny + kLightZ) / std::sqrt(nx * nx + ny * ny + 1.0);
                    if (cosine < 0.0) cosine = 0.0;
                    const double light = kAmbient + kDiffuse * cosine;
                    out[i * 4] = shade(r, light);
                    out[i * 4 + 1] = shade(g, light);
                    out[i * 4 + 2] = shade(b, light);
                }
            }
        }
        vm.account(out.size());
        return Value::object(made);
    });
    // The pixels: red, green, blue, alpha, four bytes each, top row first.
    def("surfaceRead", 1, 1, [](VM& vm, int, Value* args) -> Value {
        Renderer& r = need(vm);
        surfaceNeeded(vm, r, args[0]);
        ObjBytes* made = vm.alloc<ObjBytes>();
        r.read(surfaceOf(vm, args[0]), made->data);
        vm.account(made->data.size());
        return Value::object(made);
    });
    // imageCard(image): the standard library's Image as a picture on the graphics card, kept in
    // a slot of the image's that only this layer uses (D124): sent whole the first time, nothing
    // while the image's change number is the one it was copied at, and only the rectangle changed
    // since when it is not. Null for an image with no pixels.
    def("imageCard", 1, 1, [](VM& vm, int, Value* args) -> Value {
        if (!args[0].isObjType(ObjType::Instance)) throw vm.runtimeError(Thrown::ClassCastException, "expected an Image");
        const Value image = args[0];
        // By name, as reflection reads them: an int field is kept as a payload, not a slot.
        auto field = [&](Value on, const char* name) -> Value { return vm.memberGet(on, vm.intern(name)); };
        auto setField = [&](const char* name, Value v) { vm.memberSet(image, vm.intern(name), v); };
        const int64_t w = field(image, "width").as.i, h = field(image, "height").as.i;
        if (w <= 0 || h <= 0) return Value::null();
        const Value grid = field(image, "cells");
        if (!grid.isObjType(ObjType::Instance)) throw vm.runtimeError(Thrown::InternalError, "an Image's pixels are a grid");
        const Value cells = field(grid, "cells");
        if (!cells.isObjType(ObjType::List) || !asList(cells)->packed()) throw vm.runtimeError(Thrown::InternalError, "an Image's pixels are packed");
        const uint8_t* rgba = asList(cells)->pack->block.data();
        const int64_t changes = field(image, "changes").as.i;
        Value card = field(image, "card");
        Renderer& r = need(vm);
        if (card.isNull()) {
            const int id = r.createSurface(static_cast<int>(w), static_cast<int>(h));
            if (id == 0) throw vm.runtimeError(Thrown::IllegalStateException, "the graphics card cannot hold a picture of " + std::to_string(w) + " by " + std::to_string(h));
            vm.account(static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
            card = Value::object(vm.alloc<ObjResource>("surface", id, &Renderer::releaseLater));
            TempRoot keep(vm, card);
            setField("card", card);
            r.upload(surfaceOf(vm, card), rgba);
        } else if (field(image, "cardAt").as.i != changes) {
            const int64_t l = std::clamp<int64_t>(field(image, "changedLeft").as.i, 0, w), t = std::clamp<int64_t>(field(image, "changedTop").as.i, 0, h);
            const int64_t rt = std::clamp<int64_t>(field(image, "changedRight").as.i, 0, w), b = std::clamp<int64_t>(field(image, "changedBottom").as.i, 0, h);
            if (rt > l && b > t) r.uploadPart(surfaceOf(vm, card), rgba, static_cast<int>(w), static_cast<int>(l), static_cast<int>(t), static_cast<int>(rt - l), static_cast<int>(b - t));
        }
        setField("cardAt", Value::integer(changes));
        // Nothing changed since this copy: empty, as Image keeps it (the largest left and top, the
        // smallest right and bottom).
        setField("changedLeft", Value::integer(INT64_MAX));
        setField("changedTop", Value::integer(INT64_MAX));
        setField("changedRight", Value::integer(INT64_MIN));
        setField("changedBottom", Value::integer(INT64_MIN));
        return card;
    });
    def("surfaceUpload", 2, 2, [](VM& vm, int, Value* args) -> Value {
        Renderer& r = need(vm);
        jade::Surface& s = surfaceNeeded(vm, r, args[0]);
        r.upload(surfaceOf(vm, args[0]), gridPixels(vm, args[1], static_cast<size_t>(s.width) * static_cast<size_t>(s.height)));
        return Value::null();
    });

    // ---- Drawing ----

    // target(surface, originX, originY, scale, clipLeft, clipTop, clipRight, clipBottom):
    // where the calls that follow draw. The origin and the clip are in device pixels; what
    // the drawing calls are given is in layout units from that origin.
    def("target", 8, 8, [](VM& vm, int, Value* args) -> Value {
        Renderer& r = need(vm);
        surfaceNeeded(vm, r, args[0]);
        state().originX = vm.toInt32(args[1], "x");
        state().originY = vm.toInt32(args[2], "y");
        state().scale = vm.toNumber(args[3], "scale");
        auto edge = [&](Value v) { return static_cast<int>(std::clamp<int64_t>(vm.toInt(v, "clip"), -(int64_t{1} << 24), int64_t{1} << 24)); };
        r.setTarget(surfaceOf(vm, args[0]), edge(args[4]), edge(args[5]), edge(args[6]), edge(args[7]));
        return Value::null();
    });
    def("color", 1, 1, [](VM& vm, int, Value* args) -> Value {
        state().color = colorOf(vm, args[0]);
        return Value::null();
    });
    // font(font, scale): a Font of the standard library's, drawn at the canvas's scale (D125):
    // the engine's font for it is the one Font keeps for that scale.
    def("font", 2, 2, [](VM& vm, int, Value* args) -> Value {
        const Value method = vm.memberGet(args[0], vm.intern("engineHandle"));
        const Value handle = vm.call(method, 1, &args[1]);
        state().font = textFontOf(vm, handle);
        return Value::null();
    });
    // lineWidth(units): how thick the outlines and lines that follow are.
    def("lineWidth", 1, 1, [](VM& vm, int, Value* args) -> Value {
        state().lineWidth = num(vm, args[0]);
        return Value::null();
    });
    def("flush", 0, 0, [](VM& vm, int, Value*) -> Value {
        need(vm).flush();
        return Value::null();
    });

    // Makes a rectangle clear again.
    def("clearRect", 4, 4, [](VM& vm, int, Value* args) -> Value {
        const double x = num(vm, args[0]), y = num(vm, args[1]);
        need(vm).clear(dx(x), dy(y), dx(x + num(vm, args[2])), dy(y + num(vm, args[3])));
        return Value::null();
    });
    def("fillRect", 4, 4, [](VM& vm, int, Value* args) -> Value {
        const double x = num(vm, args[0]), y = num(vm, args[1]);
        need(vm).fillRect(static_cast<float>(dx(x)), static_cast<float>(dy(y)), static_cast<float>(dx(x + num(vm, args[2]))),
                          static_cast<float>(dy(y + num(vm, args[3]))), state().color);
        return Value::null();
    });
    // The outline of a rectangle, one unit thick: it covers the pixels from (x, y) to
    // (x + w, y + h), both included, as Java's drawRect does.
    def("drawRect", 4, 4, [](VM& vm, int, Value* args) -> Value {
        Renderer& r = need(vm);
        const double x = num(vm, args[0]), y = num(vm, args[1]), w = num(vm, args[2]), h = num(vm, args[3]);
        if (w < 0 || h < 0) return Value::null();
        // A wider outline spreads around the one-unit one, its larger half to the right and below.
        const float t = static_cast<float>(strokePixels());
        const text::Spread spread = text::spreadOf(t, thickness());
        const float before = static_cast<float>(spread.before);
        const float left = static_cast<float>(dx(x)) - before, top = static_cast<float>(dy(y)) - before;
        const float right = static_cast<float>(dx(x + w)) - before, bottom = static_cast<float>(dy(y + h)) - before;
        r.fillRect(left, top, right + t, top + t, state().color);
        r.fillRect(left, bottom, right + t, bottom + t, state().color);
        r.fillRect(left, top + t, left + t, bottom, state().color);
        r.fillRect(right, top + t, right + t, bottom, state().color);
        return Value::null();
    });
    // roundRect(x, y, w, h, arcWidth, arcHeight, fill)
    def("roundRect", 7, 7, [](VM& vm, int, Value* args) -> Value {
        Renderer& r = need(vm);
        const double x = num(vm, args[0]), y = num(vm, args[1]), w = num(vm, args[2]), h = num(vm, args[3]);
        const float radius = static_cast<float>(std::min(num(vm, args[4]), num(vm, args[5])) / 2 * state().scale);
        const float left = static_cast<float>(dx(x)), top = static_cast<float>(dy(y)), right = static_cast<float>(dx(x + w)), bottom = static_cast<float>(dy(y + h));
        const float t = static_cast<float>(strokePixels()), unit = static_cast<float>(thickness());
        const text::Spread spread = text::spreadOf(t, unit);
        const float before = static_cast<float>(spread.before), after = static_cast<float>(spread.after);
        if (args[6].truthy()) r.roundRect(left, top, right, bottom, radius, 0, state().color);
        else r.roundRect(left - before, top - before, right + unit + after, bottom + unit + after, radius + t / 2, t, state().color);
        return Value::null();
    });
    // oval(x, y, w, h, fill)
    def("oval", 5, 5, [](VM& vm, int, Value* args) -> Value {
        Renderer& r = need(vm);
        const double x = num(vm, args[0]), y = num(vm, args[1]), w = num(vm, args[2]), h = num(vm, args[3]);
        const float left = static_cast<float>(dx(x)), top = static_cast<float>(dy(y)), right = static_cast<float>(dx(x + w)), bottom = static_cast<float>(dy(y + h));
        const float t = static_cast<float>(strokePixels()), unit = static_cast<float>(thickness());
        const text::Spread spread = text::spreadOf(t, unit);
        const float before = static_cast<float>(spread.before), after = static_cast<float>(spread.after);
        if (args[4].truthy()) r.oval(left, top, right, bottom, 0, state().color);
        else r.oval(left - before, top - before, right + unit + after, bottom + unit + after, t, state().color);
        return Value::null();
    });
    // A line one unit thick from one point to another, both covered.
    def("line", 4, 4, [](VM& vm, int, Value* args) -> Value {
        Renderer& r = need(vm);
        const double x1 = num(vm, args[0]), y1 = num(vm, args[1]), x2 = num(vm, args[2]), y2 = num(vm, args[3]);
        const int unit = thickness(), t = strokePixels();
        const text::Spread spread = text::spreadOf(t, unit);
        const int ax = dx(x1), ay = dy(y1), bx = dx(x2), by = dy(y2);
        if (ax == bx || ay == by) {
            // Straight across or straight down: whole pixels, with nothing to smooth.
            const float before = static_cast<float>(spread.before), after = static_cast<float>(spread.after);
            r.fillRect(static_cast<float>(std::min(ax, bx)) - before, static_cast<float>(std::min(ay, by)) - before,
                       static_cast<float>(std::max(ax, bx) + unit) + after, static_cast<float>(std::max(ay, by) + unit) + after, state().color);
        } else {
            const float middle = static_cast<float>(unit - 1) / 2 + static_cast<float>(spread.after - spread.before) / 2;
            r.line(static_cast<float>(ax) + middle, static_cast<float>(ay) + middle, static_cast<float>(bx) + middle, static_cast<float>(by) + middle,
                   static_cast<float>(t), state().color);
        }
        return Value::null();
    });
    // polygon(xs, ys, count, fill, closed): filled, or its outline, or (not closed) a line through the points.
    def("polygon", 5, 5, [](VM& vm, int, Value* args) -> Value {
        Renderer& r = need(vm);
        if (!args[0].isObjType(ObjType::List) || !args[1].isObjType(ObjType::List)) throw vm.runtimeError(Thrown::IllegalArgumentException, "a polygon's points are two lists of numbers");
        const std::vector<Value> xs = vm.listCopy(asList(args[0]));
        const std::vector<Value> ys = vm.listCopy(asList(args[1]));
        const size_t count = static_cast<size_t>(std::clamp<int64_t>(vm.toInt(args[2], "number of points"), 0, static_cast<int64_t>(std::min(xs.size(), ys.size()))));
        if (count < 2) return Value::null();
        if (args[3].truthy()) {
            std::vector<float> points;
            points.reserve(count * 2);
            for (size_t i = 0; i < count; i++) {
                points.push_back(static_cast<float>(state().originX + num(vm, xs[i]) * state().scale));
                points.push_back(static_cast<float>(state().originY + num(vm, ys[i]) * state().scale));
            }
            r.polygon(points, state().color);
            return Value::null();
        }
        if (strokePixels() > thickness()) {
            // Wider than a unit: one shape of bands and round joins, as the image canvas draws it.
            const text::Spread spread = text::spreadOf(strokePixels(), thickness());
            const double shift = (spread.after - spread.before) / 2;
            std::vector<double> points;
            for (size_t i = 0; i < count; i++) {
                points.push_back(state().originX + (num(vm, xs[i]) + 0.5) * state().scale + shift);
                points.push_back(state().originY + (num(vm, ys[i]) + 0.5) * state().scale + shift);
            }
            text::Path path;
            text::polylineStroke(path, points, args[4].truthy(), strokePixels());
            r.shape(path, state().color);
            return Value::null();
        }
        const int t = thickness();
        const float middle = static_cast<float>(t - 1) / 2;
        const size_t segments = args[4].truthy() ? count : count - 1;
        for (size_t i = 0; i < segments; i++) {
            const size_t next = (i + 1) % count;
            const int ax = dx(num(vm, xs[i])), ay = dy(num(vm, ys[i])), bx = dx(num(vm, xs[next])), by = dy(num(vm, ys[next]));
            if (ax == bx || ay == by)
                r.fillRect(static_cast<float>(std::min(ax, bx)), static_cast<float>(std::min(ay, by)), static_cast<float>(std::max(ax, bx) + t),
                           static_cast<float>(std::max(ay, by) + t), state().color);
            else
                r.line(static_cast<float>(ax) + middle, static_cast<float>(ay) + middle, static_cast<float>(bx) + middle, static_cast<float>(by) + middle,
                       static_cast<float>(t), state().color);
        }
        return Value::null();
    });
    // pixels(cells, width, height, left, top, clear): a picture's pixels laid over the target at
    // (left, top) in its pixels, inside the clip, or clearing what is there by their alpha (N3).
    def("pixels", 6, 6, [](VM& vm, int, Value* args) -> Value {
        Renderer& r = need(vm);
        const int64_t w = vm.toInt(args[1], "width"), h = vm.toInt(args[2], "height");
        ItemBlock* data = nullptr;
        if (args[0].isObjType(ObjType::List) && asList(args[0])->packed() && asList(args[0])->pack->width == 4) data = &asList(args[0])->pack->block;
        if (data == nullptr || w < 0 || h < 0 || static_cast<uint64_t>(w) * static_cast<uint64_t>(h) * 4 != data->size())
            throw vm.runtimeError(Thrown::IllegalArgumentException, "the picture's pixels do not fit its size");
        r.pixels(data->data(), static_cast<int>(w), static_cast<int>(h), static_cast<int>(vm.toInt(args[3], "left")), static_cast<int>(vm.toInt(args[4], "top")),
                 args[5].truthy());
        return Value::null();
    });
    // path(outline, evenOdd): a Path flattened into straight lines (Path.flatten) and filled
    // (N3), as the image canvas's canvasPath fills it. Rasterised once, and kept by its look, as
    // a polygon is.
    def("path", 2, 2, [](VM& vm, int, Value* args) -> Value {
        Renderer& r = need(vm);
        text::Path path;
        if (!text::pathOf(vm, args[0], state().originX, state().originY, state().scale, path)) return Value::null();
        r.shape(path, state().color, args[1].truthy() ? text::FillRule::EvenOdd : text::FillRule::NonZero);
        return Value::null();
    });
    // arc(x, y, w, h, start, extent, kind, fill): an arc of the oval in the rectangle (M8), filled
    // as oval fills (a wedge for a pie, the arc and its chord otherwise) or its outline drawn as
    // an oval's, the line width across. Rasterised once, and kept by its look, as a polygon is.
    def("arc", 8, 8, [](VM& vm, int, Value* args) -> Value {
        Renderer& r = need(vm);
        const double x = num(vm, args[0]), y = num(vm, args[1]), w = num(vm, args[2]), h = num(vm, args[3]);
        const double start = num(vm, args[4]), extent = num(vm, args[5]);
        const auto kind = static_cast<text::ArcKind>(std::clamp<int64_t>(vm.toInt(args[6], "kind"), 0, 2));
        const double s = state().scale;
        const double l = state().originX + x * s, t = state().originY + y * s, right = state().originX + (x + w) * s, b = state().originY + (y + h) * s;
        text::Path path;
        if (args[7].truthy()) {
            text::arcFill(path, l, t, right, b, start, extent, kind);
        } else {
            const double unit = thickness(), line = strokePixels();
            const text::Spread spread = text::spreadOf(line, unit);
            const double middle = unit / 2 + (spread.after - spread.before) / 2;
            text::arcStroke(path, l + middle, t + middle, right + middle, b + middle, start, extent, kind, line);
        }
        r.shape(path, state().color);
        return Value::null();
    });
    // text(string, x, y): at x with its baseline at y, in the font and colour set. Its width in device pixels.
    def("text", 3, 3, [](VM& vm, int, Value* args) -> Value {
        Renderer& r = need(vm);
        if (state().font == nullptr) throw vm.runtimeError(Thrown::NoSuchElementException, "no font has been set");
        return Value::integer(r.text(*state().font, vm.toStr(args[0], "text"), dx(num(vm, args[1])), dy(num(vm, args[2])), state().color));
    });
    // blit(surface, x, y, alpha): another surface as it is, pixel for pixel, its corner at (x, y).
    def("blit", 4, 4, [](VM& vm, int, Value* args) -> Value {
        Renderer& r = need(vm);
        jade::Surface& s = surfaceNeeded(vm, r, args[0]);
        const float left = static_cast<float>(dx(num(vm, args[1]))), top = static_cast<float>(dy(num(vm, args[2])));
        r.image(surfaceOf(vm, args[0]), 0, 0, static_cast<float>(s.width), static_cast<float>(s.height), left, top, left + static_cast<float>(s.width),
                top + static_cast<float>(s.height), static_cast<float>(num(vm, args[3])));
        return Value::null();
    });
    // image(surface, srcX, srcY, srcW, srcH, x, y, w, h, alpha): a part of another surface
    // (in its own pixels) scaled into a rectangle.
    def("image", 10, 10, [](VM& vm, int, Value* args) -> Value {
        Renderer& r = need(vm);
        surfaceNeeded(vm, r, args[0]);
        const float sx = static_cast<float>(num(vm, args[1])), sy = static_cast<float>(num(vm, args[2]));
        const double x = num(vm, args[5]), y = num(vm, args[6]);
        r.image(surfaceOf(vm, args[0]), sx, sy, sx + static_cast<float>(num(vm, args[3])), sy + static_cast<float>(num(vm, args[4])),
                static_cast<float>(dx(x)), static_cast<float>(dy(y)), static_cast<float>(dx(x + num(vm, args[7]))), static_cast<float>(dy(y + num(vm, args[8]))),
                static_cast<float>(num(vm, args[9])));
        return Value::null();
    });

    vm.defineGlobal("__jade", Value::object(lib));
    vm.markNotParallel("__jade", {"nanos"});
}

}  // namespace ww
