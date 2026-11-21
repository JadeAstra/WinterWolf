// Regular expressions (N5, M8): Java's syntax, as Kotlin's Regex takes it, matched by code point
// over WinterWolf's strings. A pattern is compiled once into a small program; matching runs it with
// backtracking on a stack of its own, so a long text cannot run the C++ stack out.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace ww {

struct ObjString;

namespace regex {

// The options, as Java's Pattern flags (Kotlin's RegexOption).
enum Flags : uint32_t {
    kCaseInsensitive = 1,
    kMultiline = 2,
    kDotAll = 4,
    kComments = 8,
    kLiteral = 16,
    kUnixLines = 32,
    kUnicodeCase = 64,  // with kCaseInsensitive, every letter; without it, ASCII ones only
    // Python's syntax and meaning, as CPython's re reads a pattern (re/_parser.py) and matches it
    // (_sre): Snippy's re, and RegexOption.python. Its own flags (re.IGNORECASE and the rest, as
    // CPython numbers them) go in the bits from kPyFlagsShift up; the ones above are not used.
    kPython = 128,
    kPyBytes = 256,  // with kPython, a bytes pattern (each character a byte): ASCII's classes, no \u, \U or \N
};
constexpr int kPyFlagsShift = 16;
// CPython's numbers for its flags.
enum PyFlags : uint32_t {
    kPyIgnoreCase = 2, kPyLocale = 4, kPyMultiline = 8, kPyDotAll = 16, kPyUnicode = 32, kPyVerbose = 64, kPyDebug = 128, kPyAscii = 256,
};

// In the description of a SyntaxError of Python's syntax, where the name it is about goes, as
// repr() or ascii() writes it: U+FDD0 and U+FDD1, characters Unicode keeps from ever being text.
#define WW_PY_REPR "\xEF\xB7\x90"
#define WW_PY_ASCII "\xEF\xB7\x91"

// What is wrong with a pattern, with where (as Java's PatternSyntaxException says it).
struct SyntaxError : std::runtime_error {
    std::string description;
    int index;
    // With kPython: what CPython raises (0 re.error, 1 OverflowError, 2 ValueError), index -1 for
    // an error without a position; and the name a WW_PY_REPR in the description stands for, to be
    // written as Python's repr() writes it (WW_PY_ASCII: as its ascii() does).
    int kind = 0;
    std::u32string arg;
    SyntaxError(std::string d, int at, const std::string& pattern);
};

struct Program;

// A compiled pattern.
class Pattern {
public:
    Pattern(const std::u32string& pattern, uint32_t flags);  // throws SyntaxError
    ~Pattern();
    Pattern(const Pattern&) = delete;
    Pattern& operator=(const Pattern&) = delete;

    int groupCount() const;  // capturing groups, not counting the whole match
    const std::map<std::string, int>& groupNames() const;

    // How a match is looked for.
    enum class Mode { Search, AtStart, Entire };
    // A match in `text` from `from` (Search: the first at or after it; AtStart: one starting
    // there; Entire: one starting there and running to the end). On success `groups` holds
    // start and end for the whole match and each group, -1 for a group that took no part.
    // `stop`, where given, is called now and then as a long match goes on, and throws to end it.
    // The text is taken to end at `end` where that is before its end (Python's endpos); a match
    // that starts at `notEmptyAt` must not be empty (Python's next search after an empty match).
    // With kPython `groups` holds one more: the number of the group closed last (lastindex), or -1.
    bool match(const ObjString* text, size_t from, Mode mode, std::vector<int64_t>& groups, const std::function<void()>* stop = nullptr,
               size_t end = SIZE_MAX, int64_t notEmptyAt = -1) const;
    // With kPython: the pattern's flags, with those it sets at its start ((?i)), as CPython's
    // Pattern.flags has them.
    int pythonFlags() const;

private:
    std::unique_ptr<Program> program_;
};

}  // namespace regex
}  // namespace ww
