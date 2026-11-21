// What the Regex class (src/std/regex.ww) is built on: compiling a pattern, finding a match,
// replacing and splitting, over the engine in regex.cpp.

#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "builtins.h"
#include "regex.h"
#include "vm.h"

namespace ww {

namespace {

using regex::Pattern;

void releasePattern(int64_t id) { delete reinterpret_cast<Pattern*>(id); }

const Pattern& patternOf(VM& vm, const Value& v) {
    if (!v.isObjType(ObjType::Resource) || std::strcmp(static_cast<ObjResource*>(v.as.obj)->kind, "regex") != 0)
        throw vm.runtimeError(Thrown::IllegalArgumentException, "not a compiled pattern");
    return *reinterpret_cast<const Pattern*>(static_cast<ObjResource*>(v.as.obj)->id);
}

const ObjString* textOf(VM& vm, const Value& v) {
    if (!v.isString()) throw vm.runtimeError(Thrown::IllegalArgumentException, "a pattern matches a string, not a value of type " + vm.typeName(v));
    return asString(v);
}

void appendRange(Characters& out, const ObjString* s, size_t begin, size_t end) {
    if (end > begin) out.append(s->units().data() + begin * s->width, end - begin, s->width);
}

void appendChar(Characters& out, uint32_t c) {
    const uint8_t width = c < 0x100 ? 1 : c < 0x10000 ? 2 : 4;
    char bytes[4];
    if (width == 1) {
        bytes[0] = static_cast<char>(c);
    } else if (width == 2) {
        const uint16_t u = static_cast<uint16_t>(c);
        std::memcpy(bytes, &u, 2);
    } else {
        std::memcpy(bytes, &c, 4);
    }
    out.append(bytes, 1, width);
}

// A List<int> of `count` numbers, kept packed: one block for them all.
Value intList(VM& vm, const int64_t* numbers, size_t count) {
    ObjList* list = newList(vm);
    TempRoot keep(vm, Value::object(list));
    vm.typeList(list, vm.intListType());
    if (count == 0) return Value::object(list);
    if (list->store() == ListStore::Ints) {
        list->pack->block.resize(count * sizeof(int64_t));
        std::memcpy(list->pack->block.data(), numbers, count * sizeof(int64_t));
        list->pack->count = count;
        vm.account(list->pack->block.size());
    } else {
        for (size_t i = 0; i < count; i++) vm.listAdd(list, Value::integer(numbers[i]));
    }
    return Value::object(list);
}

// The groups of the match being made on this thread, kept from one to the next.
std::vector<int64_t>& scratchGroups() {
    static thread_local std::vector<int64_t> groups;
    return groups;
}

Pattern::Mode modeOf(int64_t n) { return n == 1 ? Pattern::Mode::AtStart : n == 2 ? Pattern::Mode::Entire : Pattern::Mode::Search; }

// Java's appendReplacement: $n (as many digits as still name a group), ${name}, \x for x.
void appendReplacement(VM& vm, Characters& out, const ObjString* text, const ObjString* replacement, const Pattern& pattern,
                       const std::vector<int64_t>& groups) {
    const size_t n = replacement->length;
    for (size_t i = 0; i < n; i++) {
        const uint32_t c = replacement->at(i);
        if (c == '\\') {
            if (++i >= n) throw vm.runtimeError(Thrown::IllegalArgumentException, "character to be escaped is missing");
            appendChar(out, replacement->at(i));
        } else if (c == '$') {
            if (++i >= n) throw vm.runtimeError(Thrown::IllegalArgumentException, "Illegal group reference: group index is missing");
            int group;
            if (replacement->at(i) == '{') {
                std::string name;
                while (++i < n && replacement->at(i) != '}') {
                    const uint32_t ch = replacement->at(i);
                    if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9')))
                        throw vm.runtimeError(Thrown::IllegalArgumentException, "named capturing group is missing trailing '}'");
                    name.push_back(static_cast<char>(ch));
                }
                if (i >= n) throw vm.runtimeError(Thrown::IllegalArgumentException, "named capturing group is missing trailing '}'");
                if (name.empty()) throw vm.runtimeError(Thrown::IllegalArgumentException, "named capturing group has 0 length name");
                auto found = pattern.groupNames().find(name);
                if (found == pattern.groupNames().end()) throw vm.runtimeError(Thrown::IllegalArgumentException, "No group with name {" + name + "}");
                group = found->second;
            } else {
                const uint32_t d = replacement->at(i);
                if (d < '0' || d > '9') throw vm.runtimeError(Thrown::IllegalArgumentException, "Illegal group reference");
                group = static_cast<int>(d - '0');
                if (group > pattern.groupCount()) throw vm.runtimeError(Thrown::IndexOutOfBoundsException, "No group " + std::to_string(group));
                while (i + 1 < n && replacement->at(i + 1) >= '0' && replacement->at(i + 1) <= '9') {
                    const int longer = group * 10 + static_cast<int>(replacement->at(i + 1) - '0');
                    if (longer > pattern.groupCount()) break;
                    group = longer;
                    i++;
                }
            }
            const int64_t b = groups[static_cast<size_t>(2 * group)], e = groups[static_cast<size_t>(2 * group + 1)];
            if (b >= 0) appendRange(out, text, static_cast<size_t>(b), static_cast<size_t>(e));
        } else {
            appendChar(out, c);
        }
    }
}

}  // namespace

void registerRegex(VM& vm) {
    ObjClass* holder = vm.alloc<ObjClass>(vm.intern("Regex"));
    auto def = [&](const char* name, int minArgs, int maxArgs, NativeFn fn) {
        holder->statics.set(vm.intern(name), Value::object(vm.alloc<ObjNative>(std::string("Regex.") + name, minArgs, maxArgs, fn)));
    };

    // The pattern compiled, or [what is wrong, where] when it is no pattern.
    def("compile", 2, 2, [](VM& vm, int, Value* args) -> Value {
        const ObjString* source = textOf(vm, args[0]);
        std::u32string pattern;
        pattern.reserve(source->length);
        for (size_t i = 0; i < source->length; i++) pattern.push_back(static_cast<char32_t>(source->at(i)));
        try {
            auto* made = new Pattern(pattern, static_cast<uint32_t>(vm.toInt(args[1], "options")));
            return Value::object(vm.alloc<ObjResource>("regex", reinterpret_cast<int64_t>(made), &releasePattern));
        } catch (const regex::SyntaxError& e) {
            // Snippy's syntax (RegexOption.snippy): the name it is about in quotes.
            std::string description = e.description;
            std::string name = "'";
            for (char32_t c : e.arg) appendUtf8(name, static_cast<uint32_t>(c));
            name += "'";
            for (const char* mark : {WW_SNIPPY_REPR, WW_SNIPPY_ASCII})
                for (size_t at; (at = description.find(mark)) != std::string::npos;) description.replace(at, std::strlen(mark), name);
            const Value what = vm.str(description);
            TempRoot keep(vm, what);
            return Value::object(newList(vm, {what, Value::integer(e.index)}));
        }
    });
    // A pattern in Snippy's syntax (Snippy's re): `flags` CPython's (re.IGNORECASE...), `bytes`
    // for a bytes pattern given as a string of its bytes. The pattern compiled, or [what is wrong,
    // where (-1 for nowhere), what CPython raises (0 re.error, 1 OverflowError, 2 ValueError),
    // the name a WW_SNIPPY_REPR or WW_SNIPPY_ASCII in it stands for].
    def("compileSnippy", 3, 3, [](VM& vm, int, Value* args) -> Value {
        const ObjString* source = textOf(vm, args[0]);
        std::u32string pattern;
        pattern.reserve(source->length);
        for (size_t i = 0; i < source->length; i++) pattern.push_back(static_cast<char32_t>(source->at(i)));
        const int64_t flags = vm.toInt(args[1], "flags");
        uint32_t bits = regex::kSnippy | (static_cast<uint32_t>(flags & 0xFFFF) << regex::kSnippyFlagsShift);
        if (args[2].truthy()) bits |= regex::kSnippyBytes;
        try {
            auto* made = new Pattern(pattern, bits);
            return Value::object(vm.alloc<ObjResource>("regex", reinterpret_cast<int64_t>(made), &releasePattern));
        } catch (const regex::SyntaxError& e) {
            const Value what = vm.str(e.description);
            TempRoot keep(vm, what);
            std::string arg;
            for (char32_t ch : e.arg) appendUtf8(arg, static_cast<uint32_t>(ch));
            const Value argText = vm.str(arg);
            TempRoot keepArg(vm, argText);
            return Value::object(newList(vm, {what, Value::integer(e.index), Value::integer(e.kind), argText}));
        }
    });
    def("snippyFlags", 1, 1, [](VM& vm, int, Value* args) -> Value { return Value::integer(patternOf(vm, args[0]).snippyFlags()); });
    // A match of a pattern in Snippy's syntax from `from`, the text taken to end at `to`, as find
    // gives it with the number of the group closed last after it (-1 for none); one that starts at
    // `notEmptyAt` may not be empty there. Mode as find's.
    def("findSnippy", 6, 6, [](VM& vm, int, Value* args) -> Value {
        const Pattern& pattern = patternOf(vm, args[0]);
        const std::function<void()> stop = [&vm] { vm.throwIfStopped(); };
        const ObjString* text = textOf(vm, args[1]);
        const int64_t from = vm.toInt(args[2], "position");
        const int64_t to = vm.toInt(args[3], "end");
        if (from < 0 || to < from || static_cast<size_t>(to) > text->length)
            throw vm.runtimeError(Thrown::IndexOutOfBoundsException,
                                  "from " + std::to_string(from) + " to " + std::to_string(to) + " is out of bounds of a text of " +
                                      std::to_string(text->length) + " characters");
        std::vector<int64_t>& groups = scratchGroups();
        if (!pattern.match(text, static_cast<size_t>(from), modeOf(vm.toInt(args[4], "mode")), groups, &stop, static_cast<size_t>(to),
                           vm.toInt(args[5], "notEmptyAt")))
            return Value::null();
        return intList(vm, groups.data(), groups.size());
    });
    def("groupCount", 1, 1, [](VM& vm, int, Value* args) -> Value { return Value::integer(patternOf(vm, args[0]).groupCount()); });
    def("groupNames", 1, 1, [](VM& vm, int, Value* args) -> Value {
        ObjMap* made = vm.alloc<ObjMap>();
        TempRoot keep(vm, Value::object(made));
        for (const auto& [name, index] : patternOf(vm, args[0]).groupNames()) {
            const Value key = vm.str(name);
            TempRoot keepKey(vm, key);
            made->map.set(key, Value::integer(index));
        }
        return Value::object(made);
    });
    // A match from `from`: [start, end] for the whole and each group (-1 where a group took no
    // part), or null. Mode 0 looks for one, 1 wants one starting there, 2 one running to the end.
    def("find", 4, 4, [](VM& vm, int, Value* args) -> Value {
        const Pattern& pattern = patternOf(vm, args[0]);
        const std::function<void()> stop = [&vm] { vm.throwIfStopped(); };
        const ObjString* text = textOf(vm, args[1]);
        const int64_t from = vm.toInt(args[2], "position");
        if (from < 0 || static_cast<size_t>(from) > text->length)
            throw vm.runtimeError(Thrown::IndexOutOfBoundsException,
                                  "start index " + std::to_string(from) + " is out of bounds of a text of " + std::to_string(text->length) + " characters");
        std::vector<int64_t>& groups = scratchGroups();
        if (!pattern.match(text, static_cast<size_t>(from), modeOf(vm.toInt(args[3], "mode")), groups, &stop)) return Value::null();
        return intList(vm, groups.data(), groups.size());
    });
    // Every match from `from` that ends by `to`, as findAll finds them: each one's places, as
    // find gives them, one after another in one List<int>.
    def("allSpans", 4, 4, [](VM& vm, int, Value* args) -> Value {
        const Pattern& pattern = patternOf(vm, args[0]);
        const std::function<void()> stop = [&vm] { vm.throwIfStopped(); };
        const ObjString* text = textOf(vm, args[1]);
        const int64_t start = vm.toInt(args[2], "startIndex");
        const int64_t end = vm.toInt(args[3], "endIndex");
        const int64_t n = static_cast<int64_t>(text->length);
        if (start < 0 || start > n)
            throw vm.runtimeError(Thrown::IndexOutOfBoundsException,
                                  "start index " + std::to_string(start) + " is out of bounds of a text of " + std::to_string(n) + " characters");
        if (end < start || end > n)
            throw vm.runtimeError(Thrown::IndexOutOfBoundsException,
                                  "end index " + std::to_string(end) + " is out of bounds: from " + std::to_string(start) + " to " + std::to_string(n));
        static thread_local std::vector<int64_t> all;
        all.clear();
        std::vector<int64_t>& groups = scratchGroups();
        size_t from = static_cast<size_t>(start);
        while (static_cast<int64_t>(from) <= end && pattern.match(text, from, Pattern::Mode::Search, groups, &stop) && groups[1] <= end) {
            all.insert(all.end(), groups.begin(), groups.begin() + 2 * (pattern.groupCount() + 1));  // not CPython's lastindex
            const size_t b = static_cast<size_t>(groups[0]), e = static_cast<size_t>(groups[1]);
            from = e == b ? e + 1 : e;
        }
        return intList(vm, all.data(), all.size());
    });
    // Every match, or the first, replaced by `replacement` in Java's way ($1, ${name}, \$).
    def("replace", 4, 4, [](VM& vm, int, Value* args) -> Value {
        const Pattern& pattern = patternOf(vm, args[0]);
        const std::function<void()> stop = [&vm] { vm.throwIfStopped(); };
        const ObjString* text = textOf(vm, args[1]);
        const ObjString* replacement = textOf(vm, args[2]);
        const bool firstOnly = args[3].truthy();
        Characters out;
        size_t last = 0, from = 0;
        bool any = false;
        std::vector<int64_t> groups;
        while (from <= text->length && pattern.match(text, from, Pattern::Mode::Search, groups, &stop)) {
            any = true;
            const size_t b = static_cast<size_t>(groups[0]), e = static_cast<size_t>(groups[1]);
            appendRange(out, text, last, b);
            appendReplacement(vm, out, text, replacement, pattern, groups);
            last = e;
            from = e == b ? e + 1 : e;
            if (firstOnly) break;
        }
        if (!any) return args[1];
        appendRange(out, text, last, text->length);
        return vm.str(std::move(out));
    });
    // The pieces between the matches, as Kotlin's Regex.split: up to `limit` of them (0 for any
    // number), the last holding the rest; empty pieces kept, at either end too.
    def("split", 3, 3, [](VM& vm, int, Value* args) -> Value {
        const Pattern& pattern = patternOf(vm, args[0]);
        const std::function<void()> stop = [&vm] { vm.throwIfStopped(); };
        const ObjString* text = textOf(vm, args[1]);
        const int64_t limit = vm.toInt(args[2], "limit");
        if (limit < 0) throw vm.runtimeError(Thrown::IllegalArgumentException, "Limit must be non-negative, but was " + std::to_string(limit));
        std::vector<std::pair<size_t, size_t>> pieces;
        std::vector<int64_t> groups;
        size_t last = 0, from = 0;
        if (limit != 1) {
            while (from <= text->length && pattern.match(text, from, Pattern::Mode::Search, groups, &stop)) {
                const size_t b = static_cast<size_t>(groups[0]), e = static_cast<size_t>(groups[1]);
                pieces.push_back({last, b});
                last = e;
                from = e == b ? e + 1 : e;
                if (limit > 0 && static_cast<int64_t>(pieces.size()) == limit - 1) break;
            }
        }
        pieces.push_back({last, text->length});
        TempRoot keep(vm, Value::object(newList(vm, {})));
        ObjList* list = asList(keep.get());
        for (const auto& [b, e] : pieces) list->values().push_back(vm.str(text->slice(b, e)));
        return keep.get();
    });

    vm.defineGlobal("__regex", Value::object(holder));
}

}  // namespace ww
