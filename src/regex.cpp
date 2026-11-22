// Regular expressions (N5, M8): see regex.h. Java's syntax and meaning, as Kotlin's Regex has
// them on the JVM (where Kotlin always adds UNICODE_CASE to IGNORE_CASE): the pattern is parsed
// into a tree, the tree compiled into a program of a few kinds of instruction, and the program
// run by backtracking, with the alternatives kept on a stack of its own.
//
// What a character is (a letter, a digit, upper or lower case, its category and properties) and
// its other case come from WinterWolf's own Unicode data (unicode.h, Unicode 16.0), as they do for
// char.isLetter() and the rest, each class as Java's Pattern defines it.

#include "regex.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "builtins.h"
#include "unicode.h"
#include "value.h"

namespace ww::regex {

// ---- What a character is ----

namespace {

// Java's simple case mappings, and one form of a character for comparing without case, as
// Java's UNICODE_CASE does.
uint32_t upperOf(uint32_t c) { return unicode::toUpper(c); }
uint32_t lowerOf(uint32_t c) { return unicode::toLower(c); }
uint32_t fold(uint32_t c) { return lowerOf(upperOf(c)); }

bool isLineTerminator(uint32_t c, bool unixLines) {
    if (unixLines) return c == '\n';
    return c == '\n' || c == '\r' || c == 0x85 || c == 0x2028 || c == 0x2029;
}
bool isAsciiWord(uint32_t c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }

// ---- What a character is to CPython's re ----
// From CPython's own tables (tools/snippy/regex_unicode.py), the same on every system.

#include "regex_unicode.inc"

const SnippyRecord& snippyRecord(uint32_t c) {
    if (c > 0x10FFFF) c = 0xFFFF;  // nothing past Unicode is a word, a digit or a space
    return kSnippyRecord[kSnippyEntry[static_cast<size_t>(kSnippyBlock[c >> 7]) * 128 + (c & 127)]];
}
bool snippyIs(uint32_t c, uint8_t flag) { return (snippyRecord(c).flags & flag) != 0; }

// ---- How CPython's re compares without case, from WinterWolf's own case data ----
// _sre's lowercase is the simple one (unicode::toLower gives CPython's for every code point), and
// re._casefix makes one of the characters that are their own lowercase and have the same full
// uppercase (i and ı are both I; s and ſ are both S): exactly that, 50 of them in Unicode 16, as
// CPython's Tools/build/generate_re_casefix.py makes it. A character's form is the smallest of its
// lowercase and those; two characters are the same without case when their forms are.
struct SnippyCases {
    std::unordered_map<uint32_t, uint32_t> shared;      // a lowercase letter -> its group's form, where it has one
    std::vector<std::array<uint32_t, 2>> groups;        // (form, character), sorted, where a form has more than one
};

uint32_t snippyLower(uint32_t c) { return c > 0x10FFFF ? c : unicode::toLower(c); }

const SnippyCases& snippyCases();

uint32_t snippyForm(uint32_t c) {
    const uint32_t lower = snippyLower(c);
    const auto& shared = snippyCases().shared;
    const auto it = shared.find(lower);
    return it == shared.end() ? lower : it->second;
}

const SnippyCases& snippyCases() {
    static const SnippyCases cases = [] {
        SnippyCases out;
        std::map<std::u32string, std::vector<uint32_t>> byUpper;
        for (uint32_t c : unicode::casedCodePoints()) {
            if (unicode::toLower(c) != c) continue;
            const std::u32string up = unicode::upper(std::u32string(1, static_cast<char32_t>(c)));
            if (up.size() == 1 && static_cast<uint32_t>(up[0]) == c) continue;
            byUpper[up].push_back(c);
        }
        for (const auto& [up, members] : byUpper)
            if (members.size() > 1)
                for (uint32_t m : members) out.shared[m] = members.front();  // the smallest: found in order
        std::set<uint32_t> forms;
        for (uint32_t c : unicode::casedCodePoints()) {  // any other is its own form, alone
            const uint32_t lower = unicode::toLower(c);
            const auto it = out.shared.find(lower);
            const uint32_t form = it == out.shared.end() ? lower : it->second;
            if (form == c) continue;
            out.groups.push_back({form, c});
            forms.insert(form);
        }
        for (uint32_t form : forms) out.groups.push_back({form, form});
        std::sort(out.groups.begin(), out.groups.end());
        return out;
    }();
    return cases;
}

// The characters of the form `form` where they are more than one: [first, last).
std::pair<const std::array<uint32_t, 2>*, const std::array<uint32_t, 2>*> snippyCaseGroup(uint32_t form) {
    const auto& groups = snippyCases().groups;
    const auto* begin = groups.data();
    const auto* end = begin + groups.size();
    const auto* lo = std::lower_bound(begin, end, form, [](const std::array<uint32_t, 2>& e, uint32_t f) { return e[0] < f; });
    const auto* hi = lo;
    while (hi != end && (*hi)[0] == form) hi++;
    return {lo, hi};
}
// Whether a character has another case to CPython's re.
bool snippyCased(uint32_t c) {
    const auto [lo, hi] = snippyCaseGroup(snippyForm(c));
    return lo != hi;
}

// The classes a pattern names: \d, \p{Lu} and the like, each a test of one character.
enum class Pred : uint8_t {
    AsciiDigit, AsciiWord, AsciiSpace, HSpace, VSpace,
    PosixLower, PosixUpper, Ascii, PosixAlpha, PosixAlnum, PosixPunct, PosixGraph, PosixPrint, Blank, PosixCntrl, XDigit,
    Any, Whitespace, LetterOrDigit, Alphabetic, UnicodeWord, Uppercase, Lowercase, WhiteSpaceProperty, Assigned, HexDigit,
    Ideographic, JoinControl, CasedLetter,
    MajorL, MajorM, MajorN, MajorP, MajorS, MajorZ, MajorC,
    // One general category each, in unicode::Category's order.
    CatCn, CatLu, CatLl, CatLt, CatLm, CatLo, CatMn, CatMc, CatMe, CatNd, CatNl, CatNo, CatPc, CatPd, CatPs, CatPe, CatPi, CatPf,
    CatPo, CatSm, CatSc, CatSk, CatSo, CatZs, CatZl, CatZp, CatCc, CatCf, CatCs, CatCo,
    SnippyWord, SnippyDecimal, SnippySpace,  // CPython's \w, \d and \s on str: str.isalnum() or _, str.isdecimal(), str.isspace()
};

bool test(Pred p, uint32_t c) {
    using unicode::Category;
    const auto posixPunct = [](uint32_t ch) { return ch < 0x80 && std::strchr("!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~", static_cast<int>(ch)) != nullptr && ch != 0; };
    if (p >= Pred::CatCn && p <= Pred::CatCo) return unicode::category(c) == static_cast<Category>(static_cast<int>(p) - static_cast<int>(Pred::CatCn));
    switch (p) {
        case Pred::AsciiDigit: return c >= '0' && c <= '9';
        case Pred::AsciiWord: return isAsciiWord(c);
        case Pred::AsciiSpace: return c == ' ' || c == '\t' || c == '\n' || c == 0x0B || c == '\f' || c == '\r';
        case Pred::HSpace:
            return c == ' ' || c == '\t' || c == 0xA0 || c == 0x1680 || c == 0x180E || (c >= 0x2000 && c <= 0x200A) || c == 0x202F || c == 0x205F ||
                   c == 0x3000;
        case Pred::VSpace: return c == '\n' || c == 0x0B || c == '\f' || c == '\r' || c == 0x85 || c == 0x2028 || c == 0x2029;
        case Pred::PosixLower: return c >= 'a' && c <= 'z';
        case Pred::PosixUpper: return c >= 'A' && c <= 'Z';
        case Pred::Ascii: return c < 0x80;
        case Pred::PosixAlpha: return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        case Pred::PosixAlnum: return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        case Pred::PosixPunct: return posixPunct(c);
        case Pred::PosixGraph: return c > 0x20 && c < 0x7F;
        case Pred::PosixPrint: return c >= 0x20 && c < 0x7F;
        case Pred::Blank: return c == ' ' || c == '\t';
        case Pred::PosixCntrl: return c < 0x20 || c == 0x7F;
        case Pred::XDigit: return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        case Pred::Any: return true;
        case Pred::Whitespace: return unicode::isWhitespace(c);  // Java's Character.isWhitespace
        case Pred::LetterOrDigit: return unicode::isLetterOrDigit(c);
        case Pred::Alphabetic: return unicode::isAlphabetic(c);
        case Pred::UnicodeWord: {
            // Java's \p{IsWord}: alphabetic, a mark, a decimal digit, connector punctuation or a joiner.
            const Category k = unicode::category(c);
            return unicode::isAlphabetic(c) || k == Category::Mn || k == Category::Me || k == Category::Mc || k == Category::Nd || k == Category::Pc ||
                   unicode::has(c, unicode::kJoinControl);
        }
        case Pred::Uppercase: return unicode::isUpperCase(c);
        case Pred::Lowercase: return unicode::isLowerCase(c);
        case Pred::WhiteSpaceProperty: return unicode::has(c, unicode::kWhiteSpace);
        case Pred::Assigned: return unicode::category(c) != Category::Cn;
        case Pred::HexDigit:
            // Java's \p{IsHex_Digit}: a decimal digit, or 0-9, A-F and a-f, at half or full width.
            return unicode::isDigit(c) || unicode::has(c, unicode::kHexDigit);
        case Pred::Ideographic: return unicode::has(c, unicode::kIdeographic);
        case Pred::JoinControl: return unicode::has(c, unicode::kJoinControl);
        case Pred::CasedLetter: {
            const Category k = unicode::category(c);
            return k == Category::Lu || k == Category::Ll || k == Category::Lt;
        }
        case Pred::MajorL: return unicode::major(unicode::category(c)) == 'L';
        case Pred::MajorM: return unicode::major(unicode::category(c)) == 'M';
        case Pred::MajorN: return unicode::major(unicode::category(c)) == 'N';
        case Pred::MajorP: return unicode::major(unicode::category(c)) == 'P';
        case Pred::MajorS: return unicode::major(unicode::category(c)) == 'S';
        case Pred::MajorZ: return unicode::major(unicode::category(c)) == 'Z';
        case Pred::MajorC: return unicode::major(unicode::category(c)) == 'C';
        case Pred::SnippyWord: return snippyIs(c, kSnippyWord);
        case Pred::SnippyDecimal: return snippyIs(c, kSnippyDecimal);
        case Pred::SnippySpace: return snippyIs(c, kSnippySpace);
        default: return false;
    }
}

}  // namespace

SyntaxError::SyntaxError(std::string d, int at, const std::string& pattern)
    : std::runtime_error([&] {
          std::string m = d;
          if (at >= 0) {
              m += " near index " + std::to_string(at) + "\n" + pattern;
              if (at <= static_cast<int>(pattern.size())) m += "\n" + std::string(static_cast<size_t>(at), ' ') + "^";
          }
          return m;
      }()),
      description(std::move(d)),
      index(at) {}

// ---- The program ----

namespace {

// A class of characters: what is in it, and how it is made of others.
struct ClassNode {
    bool negated = false;
    bool intersection = false;  // a and b, both of which a character must be in
    int a = -1, b = -1;
    std::vector<std::pair<uint32_t, uint32_t>> ranges;
    std::vector<std::pair<Pred, bool>> preds;  // each, or each's opposite
    std::vector<int> kids;                     // classes inside it ([a-z[0-9]])
};

enum class Op : uint8_t {
    Char, CharFold, CharFoldAscii, AnyAll, AnyLine, Class, Split, Jmp, Save, Match,
    Begin, LineStart, End, EndFinal, LineEnd, WordBoundary, NotWordBoundary, PreviousEnd,
    BackRef, Look, Atomic, LoopSet, LoopCheck, AtTarget,
    // X* (greedy) of one character, `test` saying which kind: the longest run taken at once,
    // and one entry left to give it back a character at a time, where a loop of Split, X and
    // Jmp would take three instructions and an entry for each character ([^"]* over a long
    // attribute's value, say).
    Run,
    // CPython's: a character the same as `c` without case (c its form, snippyForm); and on to the next
    // instruction where group `a` has matched, else to `b` ((?(1)yes|no)).
    CharFoldSnippy, IfGroup,
};

struct Inst {
    explicit Inst(Op o) : op(o) {}
    Op op;
    bool flag = false;  // Class: without case; BackRef: without case; Look: negative; LineStart/LineEnd/EndFinal: Unix lines
    bool behind = false;
    uint32_t c = 0;
    int a = 0, b = 0;   // Split: preferred, other; Jmp: target; Save/BackRef/Loop*: slot; Class: class; Look/Atomic: sub-program start
    int minLen = 0, maxLen = 0;
    Op test = Op::Char;  // Run: the one character's instruction, Char to Class, with the fields above
};

}  // namespace

struct Program {
    std::vector<Inst> code;
    std::vector<ClassNode> classes;
    std::vector<std::array<uint64_t, 2>> asciiBits;  // for each class: which ASCII characters are in it
    std::vector<uint8_t> classFold;  // 0 case counts, 1 ASCII letters either case, 2 every letter, 3 as CPython's re (IN_UNI_IGNORE)
    int start = 0;
    int groups = 0;  // capturing groups
    int loops = 0;   // registers for loops that may match nothing
    std::map<std::string, int> names;
    // Branch points of the main program, numbered, for remembering where they failed: where a
    // match from a branch point at a position depends on nothing else (no back references, no
    // loops that may match nothing), one that failed there fails there again, and is not tried
    // twice. That keeps patterns such as (a+)+$ from taking exponential time, as Java's do.
    std::vector<int> splitNumber;  // for each instruction of the main program, -1 if no branch point
    int splits = 0;
    bool memo = false;
    // Where a match can start: only at the start of the input, or only at a given character.
    bool anchored = false;
    bool hasFirst = false;
    uint32_t first = 0;
    // Snippy's syntax: its flags as CPython gives them, and the slot after the groups' that holds
    // the number of the group closed last.
    bool snippy = false;
    int snippyFlags = 0;
    int lastSlot = -1;
};

namespace {

bool inClassRaw(const Program& p, int index, uint32_t c) {
    const ClassNode& n = p.classes[static_cast<size_t>(index)];
    bool in;
    if (n.intersection) {
        in = inClassRaw(p, n.a, c) && inClassRaw(p, n.b, c);
    } else {
        in = false;
        for (const auto& [lo, hi] : n.ranges)
            if (c >= lo && c <= hi) {
                in = true;
                break;
            }
        if (!in)
            for (const auto& [pred, opposite] : n.preds)
                if (test(pred, c) != opposite) {
                    in = true;
                    break;
                }
        if (!in)
            for (int kid : n.kids)
                if (inClassRaw(p, kid, c)) {
                    in = true;
                    break;
                }
    }
    return n.negated ? !in : in;
}

// A class of CPython's without case, as CPython's IN_UNI_IGNORE tests it: its tests on the
// character's lowercase, its characters and ranges holding one of those the same as it without case.
bool inClassSnippy(const Program& p, int index, uint32_t c) {
    const ClassNode& n = p.classes[static_cast<size_t>(index)];
    const auto holds = [&](uint32_t x) {
        for (const auto& [lo, hi] : n.ranges)
            if (x >= lo && x <= hi) return true;
        return false;
    };
    const uint32_t lower = snippyLower(c);
    bool in = holds(c) || holds(lower);
    if (!in) {
        const auto [first, last] = snippyCaseGroup(snippyForm(c));
        for (auto e = first; e != last && !in; e++) in = holds((*e)[1]);
    }
    if (!in)
        for (const auto& [pred, opposite] : n.preds)
            if (test(pred, lower) != opposite) {
                in = true;
                break;
            }
    return n.negated ? !in : in;
}

bool inClass(const Program& p, int index, uint32_t c) {
    if (c < 128) {
        const auto& bits = p.asciiBits[static_cast<size_t>(index)];
        return (bits[c >> 6] >> (c & 63)) & 1;
    }
    if (p.classFold[static_cast<size_t>(index)] == 3) return inClassSnippy(p, index, c);
    if (inClassRaw(p, index, c)) return true;
    if (p.classFold[static_cast<size_t>(index)] != 2) return false;  // past ASCII only Unicode case folds
    const uint32_t u = upperOf(c), l = lowerOf(c);
    return (u != c && inClassRaw(p, index, u)) || (l != c && inClassRaw(p, index, l));
}

// ---- Parsing ----

struct Node {
    // Cond: CPython's (?(group)yes|no), kids {yes, no}, no -1 where there is none.
    enum Kind { Empty, Char, Class, Dot, Assert, Group, Concat, Alt, Repeat, BackRef, Look, Atomic, Cond };
    explicit Node(Kind k) : kind(k) {}
    Kind kind;
    uint32_t c = 0;
    bool fold = false;       // Char: compared by its Unicode fold
    bool asciiFold = false;  // Char: an ASCII letter in either case
    bool snippyFold = false;     // Char: compared as CPython's re does without case (c is its form)
    int foldMode = 0;        // BackRef: 0 case counts, 1 ASCII, 2 Unicode, 3 CPython's lowercase
    int variant = 0;         // Assert: 1 for CPython's (LineStart: after any \n, the end too; boundaries: Unicode's words)
    int64_t behindWidth = -1;  // Look behind, CPython's: how far back it starts, its fixed width
    bool dotAll = false, unixLines = false;
    Op assertOp = Op::Begin;
    int group = -1;            // Group: its number, or -1; BackRef: the group
    std::string groupName;     // BackRef by name, settled once every group is known
    int nameAt = -1;           // where that name ends in the pattern
    int cls = -1;
    std::vector<int> kids;
    int min = 0, max = 0;      // Repeat; max -1 for no end
    enum Greed { Greedy, Lazy, Possessive } greed = Greedy;
    bool negative = false, behind = false;  // Look
};

class Parser {
public:
    Parser(const std::u32string& pattern, uint32_t flags, Program& out) : s_(pattern), flags_(flags), p_(out) {
        for (char32_t ch : pattern) appendUtf8(utf8_, static_cast<uint32_t>(ch));
    }

    int parse() {
        if (flags_ & kSnippy) return snippyParseAll();
        if (flags_ & kLiteral) {
            const int seq = add(Node{Node::Concat});
            for (char32_t ch : s_) {
                const int k = charNode(static_cast<uint32_t>(ch));
                nodes_[static_cast<size_t>(seq)].kids.push_back(k);
            }
            return seq;
        }
        const int root = alternation();
        if (at_ < s_.size()) {
            if (s_[at_] == ')') fail("Unmatched closing ')'", static_cast<int>(at_) - 1 < 0 ? 0 : static_cast<int>(at_) - 1);
            fail("Unexpected character", static_cast<int>(at_));
        }
        for (Node& n : nodes_)
            if (n.kind == Node::BackRef && !n.groupName.empty()) {
                auto found = p_.names.find(n.groupName);
                if (found == p_.names.end()) fail("named capturing group <" + n.groupName + "> does not exist", n.nameAt);
                n.group = found->second;
            }
        return root;
    }

    std::vector<Node> nodes_;

private:
    const std::u32string& s_;
    std::string utf8_;
    uint32_t flags_;
    Program& p_;
    size_t at_ = 0;
    int depth_ = 0;

    [[noreturn]] void fail(const std::string& what, int at) { throw SyntaxError(what, at, utf8_); }

    // Groups and classes (and a class's &&s) inside each other, at most kMaxNesting deep: every
    // pass over the pattern (parsing, compiling, the lengths, matching a look-around) goes down
    // them by recursion, and a pattern of some thousands ran the stack out and ended the process.
    // The language's own parser stops at the same depth.
    static constexpr int kMaxNesting = 256;
    void deeper(size_t at) {
        if (++depth_ > kMaxNesting) fail("Nested too deeply: more than 256 groups or classes inside each other", static_cast<int>(at));
    }
    struct Nest {
        Parser& p;
        Nest(Parser& parser, size_t at) : p(parser) { p.deeper(at); }
        ~Nest() { p.depth_--; }
    };

    int add(Node n) {
        nodes_.push_back(std::move(n));
        return static_cast<int>(nodes_.size()) - 1;
    }
    bool more() const { return at_ < s_.size(); }
    char32_t peek() const { return at_ < s_.size() ? s_[at_] : 0; }

    // Whitespace and # comments, in COMMENTS mode.
    void skipComments() {
        if (!(flags_ & kComments)) return;
        while (at_ < s_.size()) {
            const char32_t ch = s_[at_];
            if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f' || ch == 0x0B) {
                at_++;
            } else if (ch == '#') {
                while (at_ < s_.size() && !isLineTerminator(s_[at_], (flags_ & kUnixLines) != 0)) at_++;
            } else {
                break;
            }
        }
    }

    int alternation() {
        std::vector<int> branches{sequence()};
        while (more() && peek() == '|') {
            at_++;
            branches.push_back(sequence());
        }
        if (branches.size() == 1) return branches[0];
        Node alt{Node::Alt};
        alt.kids = branches;
        return add(alt);
    }

    int sequence() {
        Node seq{Node::Concat};
        for (;;) {
            skipComments();
            if (!more() || peek() == '|' || peek() == ')') break;
            int atom = this->atom();
            if (atom == -2) continue;  // a flag group such as (?i), which changes what follows
            atom = quantified(atom);
            seq.kids.push_back(atom);
        }
        if (seq.kids.size() == 1) return seq.kids[0];
        return add(seq);
    }

    bool parseNumber(int& out) {
        const size_t begin = at_;
        int64_t n = 0;
        while (more() && peek() >= '0' && peek() <= '9') {
            n = n * 10 + (peek() - '0');
            if (n > 1000000000) fail("Illegal repetition range", static_cast<int>(begin));
            at_++;
        }
        out = static_cast<int>(n);
        return at_ > begin;
    }

    int quantified(int atom) {
        for (bool once = false;; once = true) {
            skipComments();
            if (!more()) return atom;
            const char32_t ch = peek();
            int min, max;
            const size_t qAt = at_;
            if (ch == '*') {
                min = 0;
                max = -1;
                at_++;
            } else if (ch == '+') {
                min = 1;
                max = -1;
                at_++;
            } else if (ch == '?') {
                min = 0;
                max = 1;
                at_++;
            } else if (ch == '{') {
                at_++;
                if (!parseNumber(min)) fail("Illegal repetition", static_cast<int>(qAt) + 1);
                max = min;
                if (more() && peek() == ',') {
                    at_++;
                    if (!parseNumber(max)) max = -1;
                }
                if (!more() || peek() != '}') fail("Unclosed counted closure", static_cast<int>(at_));
                at_++;
                if (max >= 0 && max < min) fail("Illegal repetition range", static_cast<int>(at_) - 1);
            } else {
                return atom;
            }
            // One quantifier to an atom, as in Java: a** is an error, not a repeat of a repeat.
            if (once) fail(std::string("Dangling meta character '") + static_cast<char>(ch) + "'", static_cast<int>(qAt));
            Node rep{Node::Repeat};
            rep.kids = {atom};
            rep.min = min;
            rep.max = max;
            if (more() && peek() == '?') {
                rep.greed = Node::Lazy;
                at_++;
            } else if (more() && peek() == '+') {
                rep.greed = Node::Possessive;
                at_++;
            }
            atom = add(rep);
        }
    }

    int charNode(uint32_t ch) {
        Node n{Node::Char};
        // Without case only where the character has another case at all: any letter with
        // Unicode case, an ASCII one without.
        if ((flags_ & kCaseInsensitive) && (flags_ & kUnicodeCase)) {
            n.fold = upperOf(ch) != ch || lowerOf(ch) != ch;
            n.c = n.fold ? regex::fold(ch) : ch;
        } else if ((flags_ & kCaseInsensitive) && ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z'))) {
            n.asciiFold = true;
            n.c = ch | 0x20;
        } else {
            n.c = ch;
        }
        return add(n);
    }

    int classOf(std::vector<std::pair<Pred, bool>> preds, std::vector<std::pair<uint32_t, uint32_t>> ranges = {}) {
        ClassNode c;
        c.preds = std::move(preds);
        c.ranges = std::move(ranges);
        p_.classes.push_back(std::move(c));
        p_.classFold.push_back(static_cast<uint8_t>((flags_ & kCaseInsensitive) ? ((flags_ & kUnicodeCase) ? 2 : 1) : 0));
        Node n{Node::Class};
        n.cls = static_cast<int>(p_.classes.size()) - 1;
        return add(n);
    }

    int assertNode(Op op) {
        Node n{Node::Assert};
        n.assertOp = op;
        n.unixLines = (flags_ & kUnixLines) != 0;
        return add(n);
    }

    int atom() {
        const size_t begin = at_;
        const char32_t ch = s_[at_++];
        switch (ch) {
            case '(':
                return group(begin);
            case '[': {
                const int cls = charClass(begin);
                Node n{Node::Class};
                n.cls = cls;
                return add(n);
            }
            case '.': {
                Node n{Node::Dot};
                n.dotAll = (flags_ & kDotAll) != 0;
                n.unixLines = (flags_ & kUnixLines) != 0;
                return add(n);
            }
            case '^':
                return assertNode((flags_ & kMultiline) ? Op::LineStart : Op::Begin);
            case '$':
                return assertNode((flags_ & kMultiline) ? Op::LineEnd : Op::EndFinal);
            case '\\':
                return escape(begin);
            case '*':
            case '+':
            case '?':
                fail(std::string("Dangling meta character '") + static_cast<char>(ch) + "'", static_cast<int>(begin));
            case '{':
                fail("Illegal repetition", static_cast<int>(begin));
            default:
                return charNode(static_cast<uint32_t>(ch));
        }
    }

    // After "(".
    int group(size_t begin) {
        const Nest nest(*this, begin);
        int index = -1;
        std::string name;
        Node::Kind kind = Node::Group;
        bool negative = false, behind = false;
        const uint32_t saved = flags_;
        if (more() && peek() == '?') {
            at_++;
            if (!more()) fail("Unknown inline modifier", static_cast<int>(at_));
            const char32_t ch = s_[at_++];
            if (ch == ':') {
                // (?:...)
            } else if (ch == '=' || ch == '!') {
                kind = Node::Look;
                negative = ch == '!';
            } else if (ch == '>') {
                kind = Node::Atomic;
            } else if (ch == '<') {
                if (more() && (peek() == '=' || peek() == '!')) {
                    kind = Node::Look;
                    behind = true;
                    negative = s_[at_++] == '!';
                } else {
                    name = groupName();
                    if (p_.names.count(name) != 0) fail("Named capturing group <" + name + "> is already defined", static_cast<int>(at_) - 1);
                    index = ++p_.groups;
                    p_.names[name] = index;
                }
            } else {
                // Flags: (?i) for the rest of the group around, or (?i:...) for this group alone.
                at_--;
                bool off = false;
                for (;;) {
                    if (!more()) fail("Unknown inline modifier", static_cast<int>(at_));
                    const char32_t f = s_[at_++];
                    uint32_t bit = 0;
                    switch (f) {
                        case 'i': bit = kCaseInsensitive; break;
                        case 'm': bit = kMultiline; break;
                        case 's': bit = kDotAll; break;
                        case 'x': bit = kComments; break;
                        case 'd': bit = kUnixLines; break;
                        case 'u': bit = kUnicodeCase; break;
                        case 'U': case 'c': bit = 0; break;  // no effect here
                        case '-':
                            if (off) fail("Unknown inline modifier", static_cast<int>(at_) - 1);
                            off = true;
                            continue;
                        case ')':
                            return -2;  // flags for what follows, in this group
                        case ':': {
                            const int body = alternation();
                            if (!more() || peek() != ')') fail("Unclosed group", static_cast<int>(s_.size()));
                            at_++;
                            flags_ = saved;
                            Node g{Node::Group};
                            g.kids = {body};
                            return add(g);
                        }
                        default:
                            fail("Unknown inline modifier", static_cast<int>(at_) - 1);
                    }
                    flags_ = off ? (flags_ & ~bit) : (flags_ | bit);
                }
            }
        } else {
            index = ++p_.groups;
        }
        const int body = alternation();
        if (!more() || peek() != ')') fail("Unclosed group", static_cast<int>(s_.size()));
        at_++;
        flags_ = saved;
        Node g{kind};
        g.group = index;
        g.kids = {body};
        g.negative = negative;
        g.behind = behind;
        (void)begin;
        return add(g);
    }

    std::string groupName() {
        std::string name;
        const size_t begin = at_;
        while (more() && peek() != '>') {
            const char32_t ch = s_[at_++];
            const bool letter = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
            const bool digit = ch >= '0' && ch <= '9';
            if (!letter && !(digit && at_ - 1 > begin)) fail("named capturing group is missing trailing '>'", static_cast<int>(at_) - 1);
            name.push_back(static_cast<char>(ch));
        }
        if (!more()) fail("named capturing group is missing trailing '>'", static_cast<int>(at_));
        if (name.empty()) fail("capturing group name does not start with a Latin letter", static_cast<int>(at_));
        at_++;
        return name;
    }

    uint32_t hexDigits(size_t count, size_t begin) {
        uint32_t n = 0;
        for (size_t k = 0; k < count; k++) {
            if (!more()) fail("Illegal hexadecimal escape sequence", static_cast<int>(begin));
            const char32_t ch = s_[at_++];
            int d;
            if (ch >= '0' && ch <= '9') d = static_cast<int>(ch - '0');
            else if (ch >= 'a' && ch <= 'f') d = static_cast<int>(ch - 'a' + 10);
            else if (ch >= 'A' && ch <= 'F') d = static_cast<int>(ch - 'A' + 10);
            else fail(count == 4 ? "Illegal Unicode escape sequence" : "Illegal hexadecimal escape sequence", static_cast<int>(at_) - 1);
            n = n * 16 + static_cast<uint32_t>(d);
        }
        return n;
    }

    // A character an escape stands for, where it stands for one: true and `out` set.
    bool charEscape(char32_t ch, size_t begin, uint32_t& out) {
        switch (ch) {
            case 't': out = '\t'; return true;
            case 'n': out = '\n'; return true;
            case 'r': out = '\r'; return true;
            case 'f': out = '\f'; return true;
            case 'a': out = 7; return true;
            case 'e': out = 27; return true;
            case '0': {
                // \0n, \0nn, \0mnn: octal
                int n = 0, digits = 0;
                while (more() && peek() >= '0' && peek() <= '7' && digits < 3) {
                    const int next = n * 8 + static_cast<int>(peek() - '0');
                    if (next > 0377) break;
                    n = next;
                    at_++;
                    digits++;
                }
                if (digits == 0) fail("Illegal octal escape sequence", static_cast<int>(at_));
                out = static_cast<uint32_t>(n);
                return true;
            }
            case 'x':
                if (more() && peek() == '{') {
                    at_++;
                    uint32_t n = 0;
                    size_t digits = 0;
                    while (more() && peek() != '}') {
                        const char32_t d = s_[at_++];
                        int v;
                        if (d >= '0' && d <= '9') v = static_cast<int>(d - '0');
                        else if (d >= 'a' && d <= 'f') v = static_cast<int>(d - 'a' + 10);
                        else if (d >= 'A' && d <= 'F') v = static_cast<int>(d - 'A' + 10);
                        else fail("Illegal hexadecimal escape sequence", static_cast<int>(at_) - 1);
                        n = n * 16 + static_cast<uint32_t>(v);
                        if (n > 0x10FFFF) fail("Hexadecimal codepoint is too big", static_cast<int>(at_) - 1);
                        digits++;
                    }
                    if (!more() || digits == 0) fail("Unclosed hexadecimal escape sequence", static_cast<int>(at_));
                    at_++;
                    out = n;
                    return true;
                }
                out = hexDigits(2, begin);
                return true;
            case 'u': {
                uint32_t n = hexDigits(4, begin);
                // A surrogate pair written as two escapes is the one character.
                if (n >= 0xD800 && n < 0xDC00 && at_ + 6 <= s_.size() && s_[at_] == '\\' && s_[at_ + 1] == 'u') {
                    const size_t back = at_;
                    at_ += 2;
                    const uint32_t low = hexDigits(4, begin);
                    if (low >= 0xDC00 && low < 0xE000) n = 0x10000 + ((n - 0xD800) << 10) + (low - 0xDC00);
                    else at_ = back;
                }
                out = n;
                return true;
            }
            case 'c':
                if (!more()) fail("Illegal control escape sequence", static_cast<int>(at_));
                out = static_cast<uint32_t>(s_[at_++]) ^ 64;
                return true;
            default:
                break;
        }
        // Anything not a letter or digit stands for itself.
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9'))) {
            out = static_cast<uint32_t>(ch);
            return true;
        }
        return false;
    }

    // \d, \w, \p{..} and the rest: the class they name, as tests; false where it names none.
    bool classEscape(char32_t ch, std::vector<std::pair<Pred, bool>>& preds) {
        switch (ch) {
            case 'd': preds.push_back({Pred::AsciiDigit, false}); return true;
            case 'D': preds.push_back({Pred::AsciiDigit, true}); return true;
            case 'w': preds.push_back({Pred::AsciiWord, false}); return true;
            case 'W': preds.push_back({Pred::AsciiWord, true}); return true;
            case 's': preds.push_back({Pred::AsciiSpace, false}); return true;
            case 'S': preds.push_back({Pred::AsciiSpace, true}); return true;
            case 'h': preds.push_back({Pred::HSpace, false}); return true;
            case 'H': preds.push_back({Pred::HSpace, true}); return true;
            case 'v': preds.push_back({Pred::VSpace, false}); return true;
            case 'V': preds.push_back({Pred::VSpace, true}); return true;
            case 'p':
            case 'P': {
                const bool opposite = ch == 'P';
                std::string name;
                const size_t begin = at_;
                if (more() && peek() == '{') {
                    at_++;
                    while (more() && peek() != '}') name.push_back(static_cast<char>(s_[at_++]));
                    if (!more()) fail("Unclosed character family", static_cast<int>(at_));
                    at_++;
                } else if (more()) {
                    name.push_back(static_cast<char>(s_[at_++]));
                } else {
                    fail("Illegal character family escape", static_cast<int>(at_));
                }
                bool negate = false;
                const Pred p = property(name, begin, negate);
                preds.push_back({p, opposite != negate});
                return true;
            }
            default:
                return false;
        }
    }

    Pred property(std::string name, size_t at, bool& negate) {
        static const std::map<std::string, Pred> posix = {
            {"Lower", Pred::PosixLower}, {"Upper", Pred::PosixUpper}, {"ASCII", Pred::Ascii}, {"Alpha", Pred::PosixAlpha},
            {"Digit", Pred::AsciiDigit}, {"Alnum", Pred::PosixAlnum}, {"Punct", Pred::PosixPunct}, {"Graph", Pred::PosixGraph},
            {"Print", Pred::PosixPrint}, {"Blank", Pred::Blank}, {"Cntrl", Pred::PosixCntrl}, {"XDigit", Pred::XDigit},
            {"Space", Pred::AsciiSpace},
            // Java's Character methods.
            {"javaLowerCase", Pred::Lowercase}, {"javaUpperCase", Pred::Uppercase}, {"javaTitleCase", Pred::CatLt},
            {"javaWhitespace", Pred::Whitespace}, {"javaLetter", Pred::MajorL}, {"javaDigit", Pred::CatNd},
            {"javaLetterOrDigit", Pred::LetterOrDigit}, {"javaAlphabetic", Pred::Alphabetic}, {"javaSpaceChar", Pred::MajorZ},
            {"javaIdeographic", Pred::Ideographic}, {"javaDefined", Pred::Assigned},
        };
        static const std::map<std::string, Pred> categories = {
            {"L", Pred::MajorL}, {"LC", Pred::CasedLetter}, {"Lu", Pred::CatLu}, {"Ll", Pred::CatLl}, {"Lt", Pred::CatLt}, {"Lm", Pred::CatLm},
            {"Lo", Pred::CatLo}, {"M", Pred::MajorM}, {"Mn", Pred::CatMn}, {"Mc", Pred::CatMc}, {"Me", Pred::CatMe},
            {"N", Pred::MajorN}, {"Nd", Pred::CatNd}, {"Nl", Pred::CatNl}, {"No", Pred::CatNo},
            {"P", Pred::MajorP}, {"Pc", Pred::CatPc}, {"Pd", Pred::CatPd}, {"Ps", Pred::CatPs}, {"Pe", Pred::CatPe},
            {"Pi", Pred::CatPi}, {"Pf", Pred::CatPf}, {"Po", Pred::CatPo},
            {"S", Pred::MajorS}, {"Sm", Pred::CatSm}, {"Sc", Pred::CatSc}, {"Sk", Pred::CatSk}, {"So", Pred::CatSo},
            {"Z", Pred::MajorZ}, {"Zs", Pred::CatZs}, {"Zl", Pred::CatZl}, {"Zp", Pred::CatZp},
            {"C", Pred::MajorC}, {"Cc", Pred::CatCc}, {"Cf", Pred::CatCf}, {"Cs", Pred::CatCs}, {"Co", Pred::CatCo},
            {"Cn", Pred::CatCn},
        };
        static const std::map<std::string, Pred> binary = {
            {"ALPHABETIC", Pred::Alphabetic}, {"LETTER", Pred::MajorL}, {"LOWERCASE", Pred::Lowercase}, {"UPPERCASE", Pred::Uppercase},
            {"TITLECASE", Pred::CatLt}, {"PUNCTUATION", Pred::MajorP}, {"CONTROL", Pred::CatCc}, {"WHITE_SPACE", Pred::WhiteSpaceProperty},
            {"WHITESPACE", Pred::WhiteSpaceProperty}, {"DIGIT", Pred::CatNd}, {"HEX_DIGIT", Pred::HexDigit}, {"HEXDIGIT", Pred::HexDigit},
            {"ASSIGNED", Pred::Assigned}, {"WORD", Pred::UnicodeWord}, {"IDEOGRAPHIC", Pred::Ideographic},
            {"JOIN_CONTROL", Pred::JoinControl}, {"JOINCONTROL", Pred::JoinControl},
        };
        negate = false;
        if (auto f = posix.find(name); f != posix.end()) return f->second;
        std::string rest = name;
        for (const char* prefix : {"general_category=", "gc="})
            if (rest.rfind(prefix, 0) == 0) rest = rest.substr(std::strlen(prefix));
        if (rest.rfind("Is", 0) == 0 && rest.size() > 2) {
            const std::string after = rest.substr(2);
            if (auto f = categories.find(after); f != categories.end()) return f->second;
            std::string upper;
            for (char c : after) upper.push_back(static_cast<char>(c >= 'a' && c <= 'z' ? c - 32 : c));
            if (auto f = binary.find(upper); f != binary.end()) return f->second;
        }
        if (auto f = categories.find(rest); f != categories.end()) return f->second;
        if (name == "all") return Pred::Any;
        fail("Unknown character property name {" + name + "}: WinterWolf knows Java's POSIX and java classes, the general categories, and IsAlphabetic, "
             "IsLetter and the like; scripts and blocks (IsLatin, InGreek) wait for the text engine's Unicode data",
             static_cast<int>(at));
    }

    int escape(size_t begin) {
        if (!more()) fail("Unexpected internal error", static_cast<int>(at_));
        const char32_t ch = s_[at_++];
        switch (ch) {
            case 'b':
                if (more() && peek() == '{') fail("\\b{g} is not supported", static_cast<int>(at_));
                return assertNode(Op::WordBoundary);
            case 'B': return assertNode(Op::NotWordBoundary);
            case 'A': return assertNode(Op::Begin);
            case 'G': return assertNode(Op::PreviousEnd);
            case 'Z': return assertNode(Op::EndFinal);
            case 'z': return assertNode(Op::End);
            case 'R': {
                // (?>\r\n|[\n\x0B\f\r\x85  ])
                Node crlf{Node::Concat};
                Node cr{Node::Char};
                cr.c = '\r';
                Node lf{Node::Char};
                lf.c = '\n';
                crlf.kids = {add(cr), add(lf)};
                const int pair = add(crlf);
                const int one = classOf({{Pred::VSpace, false}});
                Node alt{Node::Alt};
                alt.kids = {pair, one};
                Node atomic{Node::Atomic};
                atomic.kids = {add(alt)};
                return add(atomic);
            }
            case 'X': fail("\\X (a grapheme cluster) is not supported", static_cast<int>(begin));
            case 'N': fail("\\N{name} is not supported", static_cast<int>(begin));
            case 'Q': {
                Node seq{Node::Concat};
                while (more()) {
                    if (peek() == '\\' && at_ + 1 < s_.size() && s_[at_ + 1] == 'E') {
                        at_ += 2;
                        break;
                    }
                    seq.kids.push_back(charNode(static_cast<uint32_t>(s_[at_++])));
                }
                return add(seq);
            }
            case 'E': return add(Node{Node::Empty});
            case 'k': {
                if (!more() || peek() != '<') fail("\\k is not followed by '<' for named capturing group", static_cast<int>(at_));
                at_++;
                Node ref{Node::BackRef};
                ref.groupName = groupName();
                ref.foldMode = (flags_ & kCaseInsensitive) ? ((flags_ & kUnicodeCase) ? 2 : 1) : 0;
                ref.nameAt = static_cast<int>(at_) - 1;
                return add(ref);
            }
            default:
                break;
        }
        if (ch >= '1' && ch <= '9') {
            // A back reference: as many digits as still name a group that exists by now.
            int n = static_cast<int>(ch - '0');
            while (more() && peek() >= '0' && peek() <= '9') {
                const int longer = n * 10 + static_cast<int>(peek() - '0');
                if (longer > p_.groups) break;
                n = longer;
                at_++;
            }
            Node ref{Node::BackRef};
            ref.group = n;
            ref.foldMode = (flags_ & kCaseInsensitive) ? ((flags_ & kUnicodeCase) ? 2 : 1) : 0;
            return add(ref);
        }
        std::vector<std::pair<Pred, bool>> preds;
        if (classEscape(ch, preds)) return classOf(std::move(preds));
        uint32_t c;
        if (charEscape(ch, begin, c)) return charNode(c);
        fail("Illegal/unsupported escape sequence", static_cast<int>(at_) - 1);
    }

    // After "[": the class, as an index into p_.classes.
    int charClass(size_t begin) {
        const Nest nest(*this, begin);
        const int depthBefore = depth_;  // each && below makes the class one deeper
        ClassNode node;
        if (more() && peek() == '^') {
            node.negated = true;
            at_++;
        }
        const int self = static_cast<int>(p_.classes.size());
        p_.classes.push_back(ClassNode{});
        p_.classFold.push_back(static_cast<uint8_t>((flags_ & kCaseInsensitive) ? ((flags_ & kUnicodeCase) ? 2 : 1) : 0));
        bool firstItem = true;
        int left = -1;  // what came before && in this class
        auto finishOperand = [&](ClassNode& part) -> int {
            const int index = static_cast<int>(p_.classes.size());
            p_.classes.push_back(std::move(part));
            p_.classFold.push_back(static_cast<uint8_t>((flags_ & kCaseInsensitive) ? ((flags_ & kUnicodeCase) ? 2 : 1) : 0));
            part = ClassNode{};
            return index;
        };
        ClassNode current;
        for (;;) {
            if (flags_ & kComments) skipComments();
            if (!more()) fail("Unclosed character class", static_cast<int>(s_.size()) - 1);
            const char32_t ch = s_[at_];
            if (ch == ']' && !firstItem) {
                at_++;
                break;
            }
            if (ch == '[') {
                at_++;
                current.kids.push_back(charClass(at_ - 1));
                firstItem = false;
                continue;
            }
            if (ch == '&' && at_ + 1 < s_.size() && s_[at_ + 1] == '&') {
                deeper(at_);
                at_ += 2;
                const int operand = finishOperand(current);
                if (left < 0) {
                    left = operand;
                } else {
                    ClassNode both;
                    both.intersection = true;
                    both.a = left;
                    both.b = operand;
                    left = finishOperand(both);
                }
                firstItem = false;
                continue;
            }
            uint32_t lo;
            at_++;
            if (ch == '\\') {
                if (!more()) fail("Unclosed character class", static_cast<int>(at_));
                const char32_t e = s_[at_++];
                std::vector<std::pair<Pred, bool>> preds;
                if (classEscape(e, preds)) {
                    for (auto& p : preds) current.preds.push_back(p);
                    firstItem = false;
                    continue;
                }
                if (e == 'Q') {
                    while (more()) {
                        if (peek() == '\\' && at_ + 1 < s_.size() && s_[at_ + 1] == 'E') {
                            at_ += 2;
                            break;
                        }
                        const uint32_t q = s_[at_++];
                        current.ranges.push_back({q, q});
                    }
                    firstItem = false;
                    continue;
                }
                if (!charEscape(e, at_ - 2, lo)) fail("Illegal/unsupported escape sequence", static_cast<int>(at_) - 1);
            } else {
                lo = static_cast<uint32_t>(ch);
            }
            firstItem = false;
            // A range, unless the - ends the class or is followed by [.
            if (more() && peek() == '-' && at_ + 1 < s_.size() && s_[at_ + 1] != ']' && s_[at_ + 1] != '[') {
                at_++;
                uint32_t hi;
                const char32_t h = s_[at_++];
                if (h == '\\') {
                    if (!more()) fail("Unclosed character class", static_cast<int>(at_));
                    const char32_t e = s_[at_++];
                    if (!charEscape(e, at_ - 2, hi)) fail("Illegal character range", static_cast<int>(at_) - 1);
                } else {
                    hi = static_cast<uint32_t>(h);
                }
                if (hi < lo) fail("Illegal character range", static_cast<int>(at_) - 1);
                current.ranges.push_back({lo, hi});
            } else {
                current.ranges.push_back({lo, lo});
            }
        }
        (void)begin;
        if (left >= 0) {
            const int operand = finishOperand(current);
            node.intersection = true;
            node.a = left;
            node.b = operand;
        } else {
            node.ranges = std::move(current.ranges);
            node.preds = std::move(current.preds);
            node.kids = std::move(current.kids);
        }
        p_.classes[static_cast<size_t>(self)] = std::move(node);
        depth_ = depthBefore;
        return self;
    }

    // ---- Snippy's syntax, after CPython's re/_parser.py ----
    // Read as CPython reads a pattern, step for step, so that a pattern that is none fails with
    // CPython's words at CPython's place, the first of its mistakes CPython finds. Its tokens are
    // a character, or a backslash and the character after it.

    struct SnippyTok {
        char32_t a = 0, b = 0;
        int len = 0;  // 0: the end of the pattern
        bool is(char32_t c) const { return len == 1 && a == c; }
        bool none() const { return len == 0; }
    };
    SnippyTok tn_;          // the next token
    size_t ti_ = 0;     // where the one after it starts
    int snippyFlags_ = 0;   // CPython's flags where the parser is
    bool bytes_ = false;
    int lookbehindGroups_ = -1;                                 // state.lookbehindgroups, -1 for None
    std::vector<std::pair<uint64_t, uint64_t>> groupWidths_{{0, 0}};  // each closed group's width
    std::vector<bool> groupClosed_{false};
    std::vector<std::pair<int64_t, int>> groupRefPos_;          // (?(n)...): n, and where it was written

    static std::string utf8Of(const std::u32string& s) {
        std::string out;
        for (char32_t ch : s) appendUtf8(out, static_cast<uint32_t>(ch));
        return out;
    }
    static std::string tokText(const SnippyTok& t) {
        std::u32string s;
        if (t.len >= 1) s.push_back(t.a);
        if (t.len == 2) s.push_back(t.b);
        return utf8Of(s);
    }

    void tAdvance() {
        size_t index = ti_;
        if (index >= s_.size()) {
            tn_ = SnippyTok{};
            return;
        }
        SnippyTok t;
        t.a = s_[index];
        t.len = 1;
        if (t.a == '\\') {
            index++;
            if (index >= s_.size()) snippyFailAt("bad escape (end of pattern)", static_cast<int>(s_.size()) - 1);
            t.b = s_[index];
            t.len = 2;
        }
        ti_ = index + 1;
        tn_ = t;
    }
    int tTell() const { return static_cast<int>(ti_) - tn_.len; }
    bool tMatch(char32_t c) {
        if (!tn_.is(c)) return false;
        tAdvance();
        return true;
    }
    SnippyTok tGet() {
        const SnippyTok t = tn_;
        tAdvance();
        return t;
    }
    void tSeek(int index) {
        ti_ = static_cast<size_t>(index);
        tAdvance();
    }
    std::u32string tGetWhile(int n, bool (*in)(char32_t)) {
        std::u32string out;
        for (int k = 0; k < n; k++) {
            if (tn_.len != 1 || !in(tn_.a)) break;
            out.push_back(tn_.a);
            tAdvance();
        }
        return out;
    }
    std::u32string tGetUntil(char32_t terminator, const std::string& name) {
        std::u32string out;
        for (;;) {
            const SnippyTok c = tGet();
            if (c.none()) {
                if (out.empty()) snippyErr("missing " + name);
                snippyErr("missing " + utf8Of(std::u32string(1, terminator)) + ", unterminated name", static_cast<int>(out.size()));
            }
            if (c.is(terminator)) {
                if (out.empty()) snippyErr("missing " + name, 1);
                break;
            }
            out.push_back(c.a);
            if (c.len == 2) out.push_back(c.b);
        }
        return out;
    }

    [[noreturn]] void snippyFailAt(const std::string& msg, int at, int kind = 0, std::u32string arg = {}) {
        SyntaxError e(msg, at, utf8_);
        e.kind = kind;
        e.arg = std::move(arg);
        throw e;
    }
    [[noreturn]] void snippyErr(const std::string& msg, int offset = 0, std::u32string arg = {}) { snippyFailAt(msg, tTell() - offset, 0, std::move(arg)); }

    static bool isDigit(char32_t c) { return c >= '0' && c <= '9'; }
    static bool isOctal(char32_t c) { return c >= '0' && c <= '7'; }
    static bool isHex(char32_t c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
    static bool isAsciiLetter(char32_t c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
    static bool isAscii(const std::u32string& s) {
        for (char32_t c : s)
            if (c >= 0x80) return false;
        return true;
    }
    static bool isIdentifier(const std::u32string& s) {
        if (s.empty() || !snippyIs(static_cast<uint32_t>(s[0]), kSnippyIdStart)) return false;
        for (size_t k = 1; k < s.size(); k++)
            if (!snippyIs(static_cast<uint32_t>(s[k]), kSnippyIdContinue)) return false;
        return true;
    }
    static bool isAsciiDecimal(const std::u32string& s) {
        if (s.empty()) return false;
        for (char32_t c : s)
            if (!isDigit(c)) return false;
        return true;
    }
    // A number of digits as CPython's int() and %d give it: its digits without leading zeros.
    static std::string digitsOf(const std::u32string& s) {
        size_t k = 0;
        while (k + 1 < s.size() && s[k] == '0') k++;
        return utf8Of(s.substr(k));
    }
    // Its value, or past `most` where it is more.
    static uint64_t valueOf(const std::u32string& s, uint64_t most) {
        uint64_t n = 0;
        for (char32_t c : s) {
            n = n * 10 + static_cast<uint64_t>(c - '0');
            if (n > most) return most + 1;
        }
        return n;
    }

    void checkGroupName(const std::u32string& name, int offset) {
        if (bytes_ && !isAscii(name)) snippyErr("bad character in group name " WW_SNIPPY_ASCII, static_cast<int>(name.size()) + offset, name);
        if (!isIdentifier(name)) snippyErr("bad character in group name " WW_SNIPPY_REPR, static_cast<int>(name.size()) + offset, name);
    }
    bool checkGroup(int64_t gid) const {
        return gid < static_cast<int64_t>(groupClosed_.size()) && groupClosed_[static_cast<size_t>(gid)];
    }
    void checkLookbehindGroup(int64_t gid) {
        if (lookbehindGroups_ < 0) return;
        if (!checkGroup(gid)) snippyErr("cannot refer to an open group");
        if (gid >= lookbehindGroups_) snippyErr("cannot refer to group defined in the same lookbehind subpattern");
    }

    // CPython's flags turned into the parser's.
    void snippySetFlags(int f) {
        snippyFlags_ = f;
        uint32_t out = kSnippy | kUnixLines | (bytes_ ? kSnippyBytes : 0);
        if (f & kSnippyIgnoreCase) out |= kCaseInsensitive;
        if (f & kSnippyMultiline) out |= kMultiline;
        if (f & kSnippyDotAll) out |= kDotAll;
        if (f & kSnippyVerbose) out |= kComments;
        flags_ = out;
    }
    // Whether classes and case are Unicode's: a str pattern without re.ASCII or re.LOCALE.
    bool snippyUnicode() const { return !bytes_ && !(snippyFlags_ & (kSnippyAscii | kSnippyLocale)); }

    int snippyChar(uint32_t c) {
        Node n{Node::Char};
        n.c = c;
        if (flags_ & kCaseInsensitive) {
            if (snippyUnicode()) {
                if (snippyCased(c)) {
                    n.snippyFold = true;
                    n.c = snippyForm(c);
                }
            } else if (isAsciiLetter(static_cast<char32_t>(c))) {
                n.asciiFold = true;
                n.c = c | 0x20;
            }
        }
        return add(n);
    }
    int snippyAssert(Op op, int variant = 0) {
        Node n{Node::Assert};
        n.assertOp = op;
        n.unixLines = true;
        n.variant = variant;
        return add(n);
    }
    // \d, \s, \w and their opposites as tests.
    std::pair<Pred, bool> snippyCategory(char32_t c) {
        const bool u = snippyUnicode();
        switch (c) {
            case 'd': return {u ? Pred::SnippyDecimal : Pred::AsciiDigit, false};
            case 'D': return {u ? Pred::SnippyDecimal : Pred::AsciiDigit, true};
            case 's': return {u ? Pred::SnippySpace : Pred::AsciiSpace, false};
            case 'S': return {u ? Pred::SnippySpace : Pred::AsciiSpace, true};
            case 'w': return {u ? Pred::SnippyWord : Pred::AsciiWord, false};
            default: return {u ? Pred::SnippyWord : Pred::AsciiWord, true};
        }
    }
    int snippyClassNode(ClassNode c, bool cased) {
        p_.classes.push_back(std::move(c));
        uint8_t fold = 0;
        if (flags_ & kCaseInsensitive) fold = snippyUnicode() ? (cased ? 3 : 0) : 1;
        p_.classFold.push_back(fold);
        Node n{Node::Class};
        n.cls = static_cast<int>(p_.classes.size()) - 1;
        return add(n);
    }

    // What an item of a class is: a character, or a category (\d...).
    struct SnippyItem {
        bool category = false;
        uint32_t c = 0;
        std::pair<Pred, bool> pred{Pred::Any, false};
    };

    // \x, \u, \U, \N and octal escapes, the same in and out of a class: true where `escape` was one.
    bool snippyNumberEscape(std::u32string& escape, uint32_t& out) {
        const char32_t c = escape[1];
        auto hex = [&](int digits) {
            escape += tGetWhile(digits, isHex);
            if (static_cast<int>(escape.size()) != digits + 2) snippyErr("incomplete escape " + utf8Of(escape), static_cast<int>(escape.size()));
            uint64_t n = 0;
            for (size_t k = 2; k < escape.size(); k++) {
                const char32_t d = escape[k];
                n = n * 16 + static_cast<uint64_t>(d <= '9' ? d - '0' : (d | 0x20) - 'a' + 10);
            }
            if (n > 0x10FFFF) snippyErr("bad escape " + utf8Of(escape), static_cast<int>(escape.size()));  // chr() refuses it
            out = static_cast<uint32_t>(n);
        };
        if (c == 'x') {
            hex(2);
            return true;
        }
        if (c == 'u' && !bytes_) {
            hex(4);
            return true;
        }
        if (c == 'U' && !bytes_) {
            hex(8);
            return true;
        }
        if (c == 'N' && !bytes_) {
            if (!tMatch('{')) snippyErr("missing {");
            const std::u32string name = tGetUntil('}', "character name");
            // The names of characters come with unicodedata's (Snippy 5.3, step 5).
            snippyErr("undefined character name " WW_SNIPPY_REPR,static_cast<int>(name.size()) + 4, name);
        }
        return false;
    }

    SnippyItem snippyClassEscape(const SnippyTok& t) {
        std::u32string escape{t.a, t.b};
        const char32_t c = t.b;
        SnippyItem item;
        switch (c) {
            case 'a': item.c = 7; return item;
            case 'b': item.c = 8; return item;
            case 'f': item.c = '\f'; return item;
            case 'n': item.c = '\n'; return item;
            case 'r': item.c = '\r'; return item;
            case 't': item.c = '\t'; return item;
            case 'v': item.c = 0x0B; return item;
            case '\\': item.c = '\\'; return item;
            case 'd': case 'D': case 's': case 'S': case 'w': case 'W':
                item.category = true;
                item.pred = snippyCategory(c);
                return item;
            default: break;
        }
        uint32_t value;
        if (snippyNumberEscape(escape, value)) {
            item.c = value;
            return item;
        }
        if (isOctal(c)) {
            escape += tGetWhile(2, isOctal);
            uint32_t n = 0;
            for (size_t k = 1; k < escape.size(); k++) n = n * 8 + static_cast<uint32_t>(escape[k] - '0');
            if (n > 0377) snippyErr("octal escape value " + utf8Of(escape) + " outside of range 0-0o377", static_cast<int>(escape.size()));
            item.c = n;
            return item;
        }
        if (isDigit(c) || isAsciiLetter(c)) snippyErr("bad escape " + utf8Of(escape), static_cast<int>(escape.size()));
        item.c = static_cast<uint32_t>(c);
        return item;
    }

    int snippyEscape(const SnippyTok& t) {
        std::u32string escape{t.a, t.b};
        const char32_t c = t.b;
        switch (c) {
            case 'A': return snippyAssert(Op::Begin);
            case 'b': return snippyAssert(Op::WordBoundary, snippyUnicode() ? 1 : 0);
            case 'B': return snippyAssert(Op::NotWordBoundary, snippyUnicode() ? 1 : 0);
            case 'z': case 'Z': return snippyAssert(Op::End);
            case 'd': case 'D': case 's': case 'S': case 'w': case 'W': {
                ClassNode cls;
                cls.preds.push_back(snippyCategory(c));
                return snippyClassNode(std::move(cls), false);
            }
            case 'a': return snippyChar(7);
            case 'f': return snippyChar('\f');
            case 'n': return snippyChar('\n');
            case 'r': return snippyChar('\r');
            case 't': return snippyChar('\t');
            case 'v': return snippyChar(0x0B);
            case '\\': return snippyChar('\\');
            default: break;
        }
        uint32_t value;
        if (snippyNumberEscape(escape, value)) return snippyChar(value);
        if (c == '0') {
            escape += tGetWhile(2, isOctal);
            uint32_t n = 0;
            for (size_t k = 1; k < escape.size(); k++) n = n * 8 + static_cast<uint32_t>(escape[k] - '0');
            return snippyChar(n);
        }
        if (isDigit(c)) {
            // An octal escape, or the number of a group.
            if (tn_.len == 1 && isDigit(tn_.a)) {
                escape.push_back(tGet().a);
                if (isOctal(escape[1]) && isOctal(escape[2]) && tn_.len == 1 && isOctal(tn_.a)) {
                    escape.push_back(tGet().a);
                    const uint32_t n = static_cast<uint32_t>((escape[1] - '0') * 64 + (escape[2] - '0') * 8 + (escape[3] - '0'));
                    if (n > 0377) snippyErr("octal escape value " + utf8Of(escape) + " outside of range 0-0o377", static_cast<int>(escape.size()));
                    return snippyChar(n);
                }
            }
            const int64_t group = static_cast<int64_t>(valueOf(escape.substr(1), 1000));
            if (group < p_.groups + 1) {
                if (!checkGroup(group)) snippyErr("cannot refer to an open group", static_cast<int>(escape.size()));
                checkLookbehindGroup(group);
                return snippyBackRef(static_cast<int>(group));
            }
            snippyErr("invalid group reference " + digitsOf(escape.substr(1)), static_cast<int>(escape.size()) - 1);
        }
        if (isAsciiLetter(c)) snippyErr("bad escape " + utf8Of(escape), static_cast<int>(escape.size()));
        return snippyChar(static_cast<uint32_t>(c));
    }

    int snippyBackRef(int group) {
        Node ref{Node::BackRef};
        ref.group = group;
        if (flags_ & kCaseInsensitive) ref.foldMode = snippyUnicode() ? 3 : 1;
        return add(ref);
    }

    // _parse_sub: a|b|c.
    int snippySub(bool verbose, int nested) {
        const Nest nest(*this, static_cast<size_t>(tTell()));
        std::vector<int> items;
        for (;;) {
            items.push_back(snippyParse(verbose, nested + 1, nested == 0 && items.empty()));
            if (!tMatch('|')) break;
            if (nested == 0) verbose = (snippyFlags_ & kSnippyVerbose) != 0;
        }
        if (items.size() == 1) return items[0];
        Node alt{Node::Alt};
        alt.kids = items;
        return add(alt);
    }

    // _parse: the items of one branch.
    int snippyParse(bool verbose, int nested, bool first) {
        std::vector<int> items;
        for (;;) {
            if (tn_.none() || tn_.is('|') || tn_.is(')')) break;
            const SnippyTok t = tGet();
            if (verbose) {
                if (t.len == 1 && (t.a == ' ' || t.a == '\t' || t.a == '\n' || t.a == '\r' || t.a == 0x0B || t.a == '\f')) continue;
                if (t.is('#')) {
                    for (;;) {
                        const SnippyTok x = tGet();
                        if (x.none() || x.is('\n')) break;
                    }
                    continue;
                }
            }
            if (t.len == 2) {
                items.push_back(snippyEscape(t));
                continue;
            }
            const char32_t c = t.a;
            switch (c) {
                case '[': items.push_back(snippyClass()); continue;
                case '*': case '+': case '?': case '{': {
                    const int here = tTell();
                    int64_t min = 0, max = -1;  // -1: no end (MAXREPEAT)
                    if (c == '?') {
                        max = 1;
                    } else if (c == '+') {
                        min = 1;
                    } else if (c == '{') {
                        if (tn_.is('}')) {
                            items.push_back(snippyChar('{'));
                            continue;
                        }
                        std::u32string lo, hi;
                        while (tn_.len == 1 && isDigit(tn_.a)) lo.push_back(tGet().a);
                        if (tMatch(',')) {
                            while (tn_.len == 1 && isDigit(tn_.a)) hi.push_back(tGet().a);
                        } else {
                            hi = lo;
                        }
                        if (!tMatch('}')) {
                            items.push_back(snippyChar('{'));
                            tSeek(here);
                            continue;
                        }
                        constexpr uint64_t kMaxRepeat = 4294967295u;
                        if (!lo.empty()) {
                            const uint64_t v = valueOf(lo, kMaxRepeat);
                            if (v >= kMaxRepeat) snippyFailAt("the repetition number is too large", -1, 1);
                            min = static_cast<int64_t>(v);
                        }
                        if (!hi.empty()) {
                            const uint64_t v = valueOf(hi, kMaxRepeat);
                            if (v >= kMaxRepeat) snippyFailAt("the repetition number is too large", -1, 1);
                            max = static_cast<int64_t>(v);
                            if (max < min) snippyErr("min repeat greater than max repeat", tTell() - here);
                        }
                    }
                    if (items.empty() || nodes_[static_cast<size_t>(items.back())].kind == Node::Assert)
                        snippyErr("nothing to repeat", tTell() - here + 1);
                    if (nodes_[static_cast<size_t>(items.back())].kind == Node::Repeat) snippyErr("multiple repeat", tTell() - here + 1);
                    Node rep{Node::Repeat};
                    rep.kids = {items.back()};
                    // Past what the engine can count, the pattern is too large to compile anyway.
                    rep.min = static_cast<int>(std::min<int64_t>(min, INT32_MAX));
                    rep.max = max < 0 ? -1 : static_cast<int>(std::min<int64_t>(max, INT32_MAX));
                    if (tMatch('?')) rep.greed = Node::Lazy;
                    else if (tMatch('+')) rep.greed = Node::Possessive;
                    items.back() = add(rep);
                    continue;
                }
                case '.': {
                    Node n{Node::Dot};
                    n.dotAll = (flags_ & kDotAll) != 0;
                    n.unixLines = true;
                    items.push_back(add(n));
                    continue;
                }
                case '(': {
                    const int g = snippyGroup(verbose, nested, first, items.empty());
                    if (g == -2) {
                        verbose = (snippyFlags_ & kSnippyVerbose) != 0;  // global flags: (?x) at the start
                        continue;
                    }
                    if (g >= 0) items.push_back(g);
                    continue;
                }
                case '^': items.push_back(snippyAssert((flags_ & kMultiline) ? Op::LineStart : Op::Begin, 1)); continue;
                case '$': items.push_back(snippyAssert((flags_ & kMultiline) ? Op::LineEnd : Op::EndFinal)); continue;
                default: items.push_back(snippyChar(static_cast<uint32_t>(c))); continue;
            }
        }
        Node seq{Node::Concat};
        seq.kids = std::move(items);
        return add(seq);
    }

    // After "[": the class, or the one character it holds.
    int snippyClass() {
        const int here = tTell() - 1;
        std::vector<SnippyItem> items;
        std::vector<std::pair<uint32_t, uint32_t>> ranges;
        const bool negate = tMatch('^');
        size_t count = 0;  // items and ranges
        for (;;) {
            const SnippyTok t = tGet();
            if (t.none()) snippyErr("unterminated character set", tTell() - here);
            if (t.is(']') && count > 0) break;
            const SnippyItem one = t.len == 2 ? snippyClassEscape(t) : SnippyItem{false, static_cast<uint32_t>(t.a), {Pred::Any, false}};
            if (tMatch('-')) {
                const SnippyTok that = tGet();
                if (that.none()) snippyErr("unterminated character set", tTell() - here);
                if (that.is(']')) {
                    items.push_back(one);
                    items.push_back(SnippyItem{false, '-', {Pred::Any, false}});
                    count += 2;
                    break;
                }
                const SnippyItem two = that.len == 2 ? snippyClassEscape(that) : SnippyItem{false, static_cast<uint32_t>(that.a), {Pred::Any, false}};
                const std::string range = "bad character range " + tokText(t) + "-" + tokText(that);
                if (one.category || two.category || two.c < one.c) snippyErr(range, t.len + 1 + that.len);
                ranges.push_back({one.c, two.c});
                count++;
            } else {
                items.push_back(one);
                count++;
            }
        }
        if (!negate && ranges.empty() && items.size() >= 1 && !items[0].category) {
            bool same = true;  // one character, written once or more
            for (const SnippyItem& i : items) same = same && !i.category && i.c == items[0].c;
            if (same) return snippyChar(items[0].c);
        }
        ClassNode cls;
        cls.negated = negate;
        bool cased = false;
        for (const SnippyItem& i : items) {
            if (i.category) {
                cls.preds.push_back(i.pred);
            } else {
                cls.ranges.push_back({i.c, i.c});
                cased = cased || snippyCased(i.c);
            }
        }
        for (const auto& r : ranges) {
            cls.ranges.push_back(r);
            if (!cased && (flags_ & kCaseInsensitive) && snippyUnicode()) {
                if (r.second - r.first < 0x400) {
                    for (uint32_t x = r.first; x <= r.second && !cased; x++) cased = snippyCased(x);
                } else {
                    for (const auto& e : snippyCases().groups) cased = cased || (e[1] >= r.first && e[1] <= r.second);
                }
            }
        }
        return snippyClassNode(std::move(cls), cased);
    }

    // _parse_flags, after "(?" and its first character: true for flags of the whole pattern ((?i)),
    // else the flags turned on and off for a group's part ((?i:...), (?-i:...)).
    bool snippyParseFlags(SnippyTok ch, int& add, int& del) {
        auto flagOf = [](const SnippyTok& t) -> int {
            if (t.len != 1) return 0;
            switch (t.a) {
                case 'i': return kSnippyIgnoreCase;
                case 'L': return kSnippyLocale;
                case 'm': return kSnippyMultiline;
                case 's': return kSnippyDotAll;
                case 'x': return kSnippyVerbose;
                case 'a': return kSnippyAscii;
                case 'u': return kSnippyUnicode;
                default: return 0;
            }
        };
        auto alpha = [](const SnippyTok& t) { return t.len == 1 && snippyIs(static_cast<uint32_t>(t.a), kSnippyAlpha); };
        constexpr int kType = kSnippyAscii | kSnippyLocale | kSnippyUnicode;
        add = del = 0;
        if (!ch.is('-')) {
            for (;;) {
                const int flag = flagOf(ch);
                if (!bytes_) {
                    if (ch.is('L')) snippyErr("bad inline flags: cannot use 'L' flag with a str pattern");
                } else {
                    if (ch.is('u')) snippyErr("bad inline flags: cannot use 'u' flag with a bytes pattern");
                }
                add |= flag;
                if ((flag & kType) && (add & kType) != flag) snippyErr("bad inline flags: flags 'a', 'u' and 'L' are incompatible");
                ch = tGet();
                if (ch.none()) snippyErr("missing -, : or )");
                if (ch.is(')') || ch.is('-') || ch.is(':')) break;
                if (!flagOf(ch)) snippyErr(alpha(ch) ? "unknown flag" : "missing -, : or )", ch.len);
            }
        }
        if (ch.is(')')) {
            snippySetFlags(snippyFlags_ | add);
            return true;
        }
        if (ch.is('-')) {
            ch = tGet();
            if (ch.none()) snippyErr("missing flag");
            if (!flagOf(ch)) snippyErr(alpha(ch) ? "unknown flag" : "missing flag", ch.len);
            for (;;) {
                const int flag = flagOf(ch);
                if (flag & kType) snippyErr("bad inline flags: cannot turn off flags 'a', 'u' and 'L'");
                del |= flag;
                ch = tGet();
                if (ch.none()) snippyErr("missing :");
                if (ch.is(':')) break;
                if (!flagOf(ch)) snippyErr(alpha(ch) ? "unknown flag" : "missing :", ch.len);
            }
        }
        if (add & del) snippyErr("bad inline flags: flag turned on and off", 1);
        return false;
    }

    // After "(": the group's node, -1 for nothing ((?#...)), -2 for flags of the whole pattern.
    int snippyGroup(bool verbose, int nested, bool first, bool nothingYet) {
        const int start = tTell() - 1;
        bool capture = true, atomic = false;
        bool named = false;
        std::u32string name;
        int addFlags = 0, delFlags = 0;
        if (tMatch('?')) {
            const SnippyTok ch = tGet();
            if (ch.none()) snippyErr("unexpected end of pattern");
            if (ch.is('P')) {
                if (tMatch('<')) {
                    name = tGetUntil('>', "group name");
                    checkGroupName(name, 1);
                    named = true;
                } else if (tMatch('=')) {
                    name = tGetUntil(')', "group name");
                    checkGroupName(name, 1);
                    const auto found = p_.names.find(utf8Of(name));
                    if (found == p_.names.end()) snippyErr("unknown group name " WW_SNIPPY_REPR, static_cast<int>(name.size()) + 1, name);
                    if (!checkGroup(found->second)) snippyErr("cannot refer to an open group", static_cast<int>(name.size()) + 1);
                    checkLookbehindGroup(found->second);
                    return snippyBackRef(found->second);
                } else {
                    const SnippyTok c = tGet();
                    if (c.none()) snippyErr("unexpected end of pattern");
                    snippyErr("unknown extension ?P" + tokText(c), c.len + 2);
                }
            } else if (ch.is(':')) {
                capture = false;
            } else if (ch.is('#')) {
                for (;;) {
                    if (tn_.none()) snippyErr("missing ), unterminated comment", tTell() - start);
                    if (tGet().is(')')) break;
                }
                return -1;
            } else if (ch.is('=') || ch.is('!') || ch.is('<')) {
                SnippyTok kind = ch;
                bool behind = false;
                const int before = lookbehindGroups_;
                if (kind.is('<')) {
                    kind = tGet();
                    if (kind.none()) snippyErr("unexpected end of pattern");
                    if (!kind.is('=') && !kind.is('!')) snippyErr("unknown extension ?<" + tokText(kind), kind.len + 2);
                    behind = true;
                    if (lookbehindGroups_ < 0) lookbehindGroups_ = p_.groups + 1;
                }
                const int body = snippySub(verbose, nested + 1);
                if (behind && before < 0) lookbehindGroups_ = -1;
                if (!tMatch(')')) snippyErr("missing ), unterminated subpattern", tTell() - start);
                Node look{Node::Look};
                look.kids = {body};
                look.negative = kind.is('!');
                look.behind = behind;
                return add(look);
            } else if (ch.is('(')) {
                const std::u32string cond = tGetUntil(')', "group name");
                int64_t group;
                if (!isAsciiDecimal(cond)) {
                    checkGroupName(cond, 1);
                    const auto found = p_.names.find(utf8Of(cond));
                    if (found == p_.names.end()) snippyErr("unknown group name " WW_SNIPPY_REPR, static_cast<int>(cond.size()) + 1, cond);
                    group = found->second;
                } else {
                    constexpr uint64_t kMaxGroups = 1073741823;
                    const uint64_t v = valueOf(cond, kMaxGroups);
                    if (v == 0) snippyErr("bad group number", static_cast<int>(cond.size()) + 1);
                    if (v >= kMaxGroups) snippyErr("invalid group reference " + digitsOf(cond), static_cast<int>(cond.size()) + 1);
                    group = static_cast<int64_t>(v);
                    bool seen = false;
                    for (const auto& r : groupRefPos_) seen = seen || r.first == group;
                    if (!seen) groupRefPos_.push_back({group, tTell() - static_cast<int>(cond.size()) - 1});
                }
                checkLookbehindGroup(group);
                const int yes = snippyParse(verbose, nested + 1, false);
                int no = -1;
                if (tMatch('|')) {
                    no = snippyParse(verbose, nested + 1, false);
                    if (tn_.is('|')) snippyErr("conditional backref with more than two branches");
                }
                if (!tMatch(')')) snippyErr("missing ), unterminated subpattern", tTell() - start);
                Node c{Node::Cond};
                c.group = static_cast<int>(group);
                c.kids = {yes, no};
                return add(c);
            } else if (ch.is('>')) {
                capture = false;
                atomic = true;
            } else if (ch.len == 1 && std::u32string(U"iLmsxau-").find(ch.a) != std::u32string::npos) {
                if (snippyParseFlags(ch, addFlags, delFlags)) {
                    if (!first || !nothingYet) snippyErr("global flags not at the start of the expression", tTell() - start);
                    return -2;
                }
                capture = false;
            } else {
                snippyErr("unknown extension ?" + tokText(ch), ch.len + 1);
            }
        }
        int group = -1;
        if (capture) {
            group = p_.groups + 1;
            if (named) {
                const std::string key = utf8Of(name);
                const auto had = p_.names.find(key);
                if (had != p_.names.end())
                    snippyErr("redefinition of group name " WW_SNIPPY_REPR " as group " + std::to_string(group) + "; was group " + std::to_string(had->second),
                          static_cast<int>(name.size()) + 1, name);
                p_.names[key] = group;
            }
            p_.groups = group;
            groupWidths_.push_back({0, 0});
            groupClosed_.push_back(false);
        }
        const bool subVerbose = (verbose || (addFlags & kSnippyVerbose)) && !(delFlags & kSnippyVerbose);
        const int saved = snippyFlags_;
        int inner = snippyFlags_;
        if (addFlags & (kSnippyAscii | kSnippyLocale | kSnippyUnicode)) inner &= ~(kSnippyAscii | kSnippyLocale | kSnippyUnicode);
        snippySetFlags((inner | addFlags) & ~delFlags);
        const int body = snippySub(subVerbose, nested + 1);
        if (!tMatch(')')) snippyErr("missing ), unterminated subpattern", tTell() - start);
        snippySetFlags(saved);
        if (group >= 0) {
            groupWidths_[static_cast<size_t>(group)] = snippyWidth(body);
            groupClosed_[static_cast<size_t>(group)] = true;
        }
        Node g{atomic ? Node::Atomic : Node::Group};
        g.group = group;
        g.kids = {body};
        return add(g);
    }

    // SubPattern.getwidth: the fewest and most characters a part matches, as CPython counts them
    // (its most 2**64, here UINT64_MAX, where there is no end).
    std::pair<uint64_t, uint64_t> snippyWidth(int i) const {
        constexpr uint64_t kMost = UINT64_MAX;
        auto plus = [](uint64_t a, uint64_t b) { return a > kMost - b ? kMost : a + b; };
        auto times = [](uint64_t a, uint64_t b) { return a == 0 || b == 0 ? 0 : a > kMost / b ? kMost : a * b; };
        const Node& n = nodes_[static_cast<size_t>(i)];
        switch (n.kind) {
            case Node::Char: case Node::Class: case Node::Dot: return {1, 1};
            case Node::Empty: case Node::Assert: case Node::Look: return {0, 0};
            case Node::BackRef: return groupWidths_[static_cast<size_t>(n.group)];
            case Node::Group: case Node::Atomic: return snippyWidth(n.kids[0]);
            case Node::Concat: {
                uint64_t lo = 0, hi = 0;
                for (int k : n.kids) {
                    const auto [a, b] = snippyWidth(k);
                    lo = plus(lo, a);
                    hi = plus(hi, b);
                }
                return {lo, hi};
            }
            case Node::Alt: {
                uint64_t lo = kMost, hi = 0;
                for (int k : n.kids) {
                    const auto [a, b] = snippyWidth(k);
                    lo = std::min(lo, a);
                    hi = std::max(hi, b);
                }
                return {lo, hi};
            }
            case Node::Repeat: {
                const auto [a, b] = snippyWidth(n.kids[0]);
                const uint64_t lo = times(a, static_cast<uint64_t>(n.min));
                const uint64_t hi = n.max < 0 ? (b ? kMost : 0) : times(b, static_cast<uint64_t>(n.max));
                return {lo, hi};
            }
            case Node::Cond: {
                auto [a, b] = snippyWidth(n.kids[0]);
                if (n.kids[1] >= 0) {
                    const auto [c, d] = snippyWidth(n.kids[1]);
                    a = std::min(a, c);
                    b = std::max(b, d);
                } else {
                    a = 0;
                }
                return {a, b};
            }
        }
        return {0, 0};
    }

    // What CPython's compiler refuses: a look-behind that is not of one width.
    void snippyCheckBehind(int i) {
        Node& n = nodes_[static_cast<size_t>(i)];
        if (n.kind == Node::Look && n.behind) {
            const auto [lo, hi] = snippyWidth(n.kids[0]);
            if (lo > 0xFFFFFFFFu) snippyFailAt("looks too much behind", -1);
            if (lo != hi) snippyFailAt("look-behind requires fixed-width pattern", -1);
            nodes_[static_cast<size_t>(i)].behindWidth = static_cast<int64_t>(lo);
        }
        const std::vector<int> kids = nodes_[static_cast<size_t>(i)].kids;
        for (int k : kids)
            if (k >= 0) snippyCheckBehind(k);
    }

    int snippyParseAll() {
        bytes_ = (flags_ & kSnippyBytes) != 0;
        snippySetFlags(static_cast<int>(flags_ >> kSnippyFlagsShift));
        tAdvance();
        const int root = snippySub((snippyFlags_ & kSnippyVerbose) != 0, 0);
        // fix_flags
        int f = snippyFlags_;
        if (!bytes_) {
            if (f & kSnippyLocale) snippyFailAt("cannot use LOCALE flag with a str pattern", -1, 2);
            if (!(f & kSnippyAscii)) f |= kSnippyUnicode;
            else if (f & kSnippyUnicode) snippyFailAt("ASCII and UNICODE flags are incompatible", -1, 2);
        } else {
            if (f & kSnippyUnicode) snippyFailAt("cannot use UNICODE flag with a bytes pattern", -1, 2);
            if ((f & kSnippyLocale) && (f & kSnippyAscii)) snippyFailAt("ASCII and LOCALE flags are incompatible", -1, 2);
        }
        if (!tn_.none()) snippyErr("unbalanced parenthesis");
        for (const auto& [g, at] : groupRefPos_)
            if (g >= p_.groups + 1) snippyFailAt("invalid group reference " + std::to_string(g), at);
        snippyCheckBehind(root);
        p_.snippy = true;
        p_.snippyFlags = f;
        p_.lastSlot = 2 * (p_.groups + 1);
        return root;
    }
};

// ---- Compiling ----

class Compiler {
public:
    Compiler(std::vector<Node>& nodes, Program& p) : n_(nodes), p_(p) {}

    bool canBeEmpty(int i) const {
        const Node& n = n_[static_cast<size_t>(i)];
        switch (n.kind) {
            case Node::Char: case Node::Class: case Node::Dot: return false;
            case Node::Empty: case Node::Assert: case Node::Look: case Node::BackRef: return true;
            case Node::Group: case Node::Atomic: return canBeEmpty(n.kids[0]);
            case Node::Cond: return canBeEmpty(n.kids[0]) || n.kids[1] < 0 || canBeEmpty(n.kids[1]);
            case Node::Concat:
                for (int k : n.kids)
                    if (!canBeEmpty(k)) return false;
                return true;
            case Node::Alt:
                for (int k : n.kids)
                    if (canBeEmpty(k)) return true;
                return false;
            case Node::Repeat: return n.min == 0 || canBeEmpty(n.kids[0]);
        }
        return true;
    }

    // The fewest and most characters it can match; most -1 for no end.
    void lengths(int i, int64_t& lo, int64_t& hi) const {
        const Node& n = n_[static_cast<size_t>(i)];
        switch (n.kind) {
            case Node::Char: case Node::Class: case Node::Dot: lo = hi = 1; return;
            case Node::Empty: case Node::Assert: case Node::Look: lo = hi = 0; return;
            case Node::BackRef: lo = 0; hi = -1; return;
            case Node::Group: case Node::Atomic: lengths(n.kids[0], lo, hi); return;
            case Node::Cond: {
                lengths(n.kids[0], lo, hi);
                int64_t a = 0, b = 0;
                if (n.kids[1] >= 0) lengths(n.kids[1], a, b);
                lo = std::min(lo, a);
                hi = (hi < 0 || b < 0) ? -1 : std::max(hi, b);
                return;
            }
            case Node::Concat: {
                lo = hi = 0;
                for (int k : n.kids) {
                    int64_t a, b;
                    lengths(k, a, b);
                    lo += a;
                    hi = (hi < 0 || b < 0) ? -1 : hi + b;
                }
                return;
            }
            case Node::Alt: {
                lo = INT64_MAX;
                hi = 0;
                for (int k : n.kids) {
                    int64_t a, b;
                    lengths(k, a, b);
                    lo = std::min(lo, a);
                    hi = (hi < 0 || b < 0) ? -1 : std::max(hi, b);
                }
                return;
            }
            case Node::Repeat: {
                int64_t a, b;
                lengths(n.kids[0], a, b);
                lo = a * n.min;
                hi = (n.max < 0 || b < 0) ? (b == 0 ? 0 : -1) : b * n.max;
                return;
            }
        }
    }

    int emit(Inst inst) {
        if (p_.code.size() > 2000000) throw SyntaxError("The pattern is too large to compile", -1, "");
        p_.code.push_back(inst);
        return static_cast<int>(p_.code.size()) - 1;
    }
    Inst& at(int pc) { return p_.code[static_cast<size_t>(pc)]; }
    int here() const { return static_cast<int>(p_.code.size()); }

    // A part compiled as a program of its own (a look-around or an atomic group): its start.
    int sub(int node, bool behind) {
        std::vector<Inst> saved;
        saved.swap(p_.code);
        compile(node);
        if (behind) emit(Inst{Op::AtTarget});
        emit(Inst{Op::Match});
        std::vector<Inst> body;
        body.swap(p_.code);
        p_.code.swap(saved);
        // Placed after everything else; its jumps shift by where it lands.
        pending_.push_back(std::move(body));
        return -static_cast<int>(pending_.size());  // settled in finish()
    }

    void compile(int i) {
        const Node& n = n_[static_cast<size_t>(i)];
        switch (n.kind) {
            case Node::Empty: return;
            case Node::Char: {
                Inst inst{n.snippyFold ? Op::CharFoldSnippy : n.fold ? Op::CharFold : n.asciiFold ? Op::CharFoldAscii : Op::Char};
                inst.c = n.c;
                emit(inst);
                return;
            }
            case Node::Class: {
                Inst inst{Op::Class};
                inst.a = n.cls;
                emit(inst);
                return;
            }
            case Node::Dot: {
                Inst inst{n.dotAll ? Op::AnyAll : Op::AnyLine};
                inst.flag = n.unixLines;
                emit(inst);
                return;
            }
            case Node::Assert: {
                Inst inst{n.assertOp};
                inst.flag = n.unixLines;
                inst.c = static_cast<uint32_t>(n.variant);
                emit(inst);
                return;
            }
            case Node::Group: {
                if (n.group >= 0) {
                    Inst open{Op::Save};
                    open.a = 2 * n.group;
                    emit(open);
                }
                compile(n.kids[0]);
                if (n.group >= 0) {
                    Inst close{Op::Save};
                    close.a = 2 * n.group + 1;
                    emit(close);
                }
                return;
            }
            case Node::Concat:
                for (int k : n.kids) compile(k);
                return;
            case Node::Alt: {
                std::vector<int> jumps;
                for (size_t k = 0; k < n.kids.size(); k++) {
                    if (k + 1 < n.kids.size()) {
                        const int split = emit(Inst{Op::Split});
                        at(split).a = here();
                        compile(n.kids[k]);
                        jumps.push_back(emit(Inst{Op::Jmp}));
                        at(split).b = here();
                    } else {
                        compile(n.kids[k]);
                    }
                }
                for (int j : jumps) at(j).a = here();
                return;
            }
            case Node::BackRef: {
                if (n.group > p_.groups || n.group < 1) {
                    // A reference to a group that never takes part matches nothing, as in Java.
                    Inst inst{Op::BackRef};
                    inst.a = -1;
                    emit(inst);
                    return;
                }
                Inst inst{Op::BackRef};
                inst.a = n.group;
                inst.c = static_cast<uint32_t>(n.foldMode);
                emit(inst);
                return;
            }
            case Node::Look: {
                Inst inst{Op::Look};
                inst.flag = n.negative;
                inst.behind = n.behind;
                if (n.behind && n.behindWidth >= 0) {
                    // CPython's: of one width, worked out as CPython does.
                    inst.minLen = inst.maxLen = static_cast<int>(std::min<int64_t>(n.behindWidth, INT32_MAX));
                } else if (n.behind) {
                    int64_t lo, hi;
                    lengths(n.kids[0], lo, hi);
                    inst.minLen = static_cast<int>(lo);
                    inst.maxLen = hi < 0 || hi > INT32_MAX ? INT32_MAX : static_cast<int>(hi);
                }
                inst.a = sub(n.kids[0], n.behind);
                emit(inst);
                return;
            }
            case Node::Atomic: {
                Inst inst{Op::Atomic};
                inst.a = sub(n.kids[0], false);
                emit(inst);
                return;
            }
            case Node::Cond: {
                // IfGroup g, no; yes; jmp out; no: no; out:
                Inst test{Op::IfGroup};
                test.a = n.group;
                const int at = emit(test);
                compile(n.kids[0]);
                if (n.kids[1] >= 0) {
                    const int jump = emit(Inst{Op::Jmp});
                    this->at(at).b = here();
                    compile(n.kids[1]);
                    this->at(jump).a = here();
                } else {
                    this->at(at).b = here();
                }
                return;
            }
            case Node::Repeat: {
                if (n.greed == Node::Possessive && i != greedyOnce_) {
                    // X*+ is (?>X*): this node again, compiled as a greedy repeat, in a program of its own.
                    const int saved = greedyOnce_;
                    greedyOnce_ = i;
                    Inst inst{Op::Atomic};
                    inst.a = sub(i, false);
                    greedyOnce_ = saved;
                    emit(inst);
                    return;
                }
                const int child = n.kids[0];
                const int min = n.min, max = n.max;
                const bool lazy = n.greed == Node::Lazy && i != greedyOnce_;
                // A part that compiles to nothing (an empty group) is compiled once, not min times:
                // (?:(?:){1000000000}){1000000000} took for ever, adding nothing.
                const int before = here();
                for (int k = 0; k < min; k++) {
                    compile(child);
                    if (here() == before) break;
                }
                const Node::Kind childKind = n_[static_cast<size_t>(child)].kind;
                if (max < 0 && !lazy && (childKind == Node::Char || childKind == Node::Class || childKind == Node::Dot)) {
                    // X* of one character: one Run.
                    compile(child);
                    Inst& one = at(here() - 1);
                    one.test = one.op;
                    one.op = Op::Run;
                    return;
                }
                if (max < 0) {
                    // X*: L: split body, out; body: [mark] X [check] jmp L
                    const bool empty = canBeEmpty(child);
                    const int reg = empty ? p_.loops++ : -1;
                    const int loop = emit(Inst{Op::Split});
                    const int body = here();
                    if (empty) {
                        Inst mark{Op::LoopSet};
                        mark.a = reg;
                        emit(mark);
                    }
                    compile(child);
                    int checkAt = -1;
                    if (empty) {
                        Inst check{Op::LoopCheck};
                        check.a = reg;
                        checkAt = emit(check);
                    }
                    Inst back{Op::Jmp};
                    back.a = loop;
                    emit(back);
                    const int out = here();
                    if (checkAt >= 0) at(checkAt).b = out;
                    at(loop).a = lazy ? out : body;
                    at(loop).b = lazy ? body : out;
                } else {
                    // X{0,k} after the required ones: k optional copies, each inside the last.
                    std::vector<int> splits;
                    for (int k = min; k < max; k++) {
                        const int split = emit(Inst{Op::Split});
                        splits.push_back(split);
                        at(split).a = here();
                        compile(child);
                        if (here() == split + 1) {  // nothing to make optional
                            p_.code.pop_back();
                            splits.pop_back();
                            break;
                        }
                    }
                    const int out = here();
                    for (int split : splits) {
                        const int body = at(split).a;
                        at(split).a = lazy ? out : body;
                        at(split).b = lazy ? body : out;
                    }
                }
                return;
            }
        }
    }

    // Places the programs of look-arounds and atomic groups after the main one, settling where
    // each starts, and their own jumps.
    void finish() {
        const int mainEnd = here();
        p_.splitNumber.assign(static_cast<size_t>(mainEnd), -1);
        bool memo = p_.loops == 0;
        for (int pc = 0; pc < mainEnd; pc++) {
            const Op op = p_.code[static_cast<size_t>(pc)].op;
            if (op == Op::Split) p_.splitNumber[static_cast<size_t>(pc)] = p_.splits++;
            if (op == Op::Run) {
                // Two: one for coming to the run at a position, one for going on after it from one.
                p_.splitNumber[static_cast<size_t>(pc)] = p_.splits;
                p_.splits += 2;
            }
            if (op == Op::BackRef || op == Op::IfGroup) memo = false;
        }
        // A back reference in a part's program counts too.
        for (const auto& body : pending_)
            for (const Inst& inst : body)
                if (inst.op == Op::BackRef || inst.op == Op::LoopSet || inst.op == Op::IfGroup) memo = false;
        p_.memo = memo && p_.splits > 0;
        for (size_t k = 0; k < pending_.size(); k++) {
            const int base = here();
            starts_.push_back(base);
            for (Inst inst : pending_[k]) {
                if (inst.op == Op::Split) {
                    inst.a += base;
                    inst.b += base;
                } else if (inst.op == Op::LoopCheck || inst.op == Op::IfGroup) {
                    inst.b += base;
                } else if (inst.op == Op::Jmp) {
                    inst.a += base;
                }
                p_.code.push_back(inst);
            }
        }
        // Each Look/Atomic names its program by -(k + 1).
        for (Inst& inst : p_.code)
            if ((inst.op == Op::Look || inst.op == Op::Atomic) && inst.a < 0) inst.a = starts_[static_cast<size_t>(-inst.a - 1)];
    }

private:
    std::vector<Node>& n_;
    Program& p_;
    std::vector<std::vector<Inst>> pending_;
    std::vector<int> starts_;
    int greedyOnce_ = -1;  // a possessive repeat being compiled as the greedy one inside it
};

// Where a match must start: the start of the input, or at a given character.
void firstOf(const std::vector<Node>& nodes, int i, Program& p) {
    const Node& n = nodes[static_cast<size_t>(i)];
    switch (n.kind) {
        case Node::Char:
            if (!n.fold && !n.asciiFold && !n.snippyFold) {
                p.hasFirst = true;
                p.first = n.c;
            }
            return;
        case Node::Assert:
            if (n.assertOp == Op::Begin) p.anchored = true;
            return;
        case Node::Group:
            firstOf(nodes, n.kids[0], p);
            return;
        case Node::Concat:
            for (int k : n.kids) {
                const Node& kid = nodes[static_cast<size_t>(k)];
                if (kid.kind == Node::Empty) continue;
                firstOf(nodes, k, p);
                return;
            }
            return;
        case Node::Repeat:
            if (n.min > 0) firstOf(nodes, n.kids[0], p);
            return;
        default:
            return;
    }
}

}  // namespace

// ---- Matching ----

namespace {

struct Entry {
    enum Kind : uint8_t { Alt, Capture, Register, Run } kind;
    int pc;
    int64_t value;  // Alt: the position; Capture, Register: the value to put back; Run: the position to go on from next
    int64_t slot;   // Capture, Register: which; Run: where the run began, the shortest it may be given back to
};

// The entries of the match being made on this thread, kept from one match to the next: a
// match is made at a time on a thread, and most are short, where a vector of their own would
// cost them an allocation each.
std::vector<Entry>& scratchStack() {
    static thread_local std::vector<Entry> stack;
    stack.clear();
    return stack;
}

class Runner {
public:
    Runner(const Program& p, const ObjString* s, std::vector<int64_t>& caps, size_t searchStart, const std::function<void()>* stop, size_t end = SIZE_MAX,
           int64_t notEmptyAt = -1)
        : p_(p), s_(s), n_(std::min(s->length, end)), notEmptyAt_(notEmptyAt), caps_(caps), regs_(static_cast<size_t>(p.loops + p.groups + 1), -1), stack_(scratchStack()), searchStart_(searchStart), stop_(stop) {}

    // Every 65,536 times a match backs out or starts a part, the caller is asked whether to stop
    // (it throws to stop): a pattern that backs out exponentially, such as (a*)*b, runs on until
    // the program's own limits stop it (a worker's terminate(), a test's time limit), as code in
    // the VM does at its safe points.
    void poll() {
        if (stop_ != nullptr && ++steps_ == 65536) {
            steps_ = 0;
            (*stop_)();
        }
    }

    // Runs the program from `pc` at `pos`; on success `end` is where the match ended. With a
    // target, a look-behind's program, it must end there; with requireEnd, at the end of the input.
    bool run(int pc, int64_t pos, int64_t& end, int64_t target = -1, bool requireEnd = false) {
        poll();
        if (depth_ == 0) start_ = pos;
        const size_t base = stack_.size();
        for (;;) {
            const Inst& inst = p_.code[static_cast<size_t>(pc)];
            bool ok = true;
            switch (inst.op) {
                case Op::Char:
                    ok = pos < static_cast<int64_t>(n_) && s_->at(static_cast<size_t>(pos)) == inst.c;
                    if (ok) pos++, pc++;
                    break;
                case Op::CharFold:
                    ok = pos < static_cast<int64_t>(n_) && fold(s_->at(static_cast<size_t>(pos))) == inst.c;
                    if (ok) pos++, pc++;
                    break;
                case Op::CharFoldSnippy:
                    ok = pos < static_cast<int64_t>(n_) && snippyForm(s_->at(static_cast<size_t>(pos))) == inst.c;
                    if (ok) pos++, pc++;
                    break;
                case Op::IfGroup: {
                    const int64_t from = caps_[static_cast<size_t>(2 * inst.a)], to = caps_[static_cast<size_t>(2 * inst.a + 1)];
                    pc = from >= 0 && to >= from ? pc + 1 : inst.b;
                    break;
                }
                case Op::CharFoldAscii: {
                    const uint32_t c = pos < static_cast<int64_t>(n_) ? s_->at(static_cast<size_t>(pos)) : 0;
                    ok = pos < static_cast<int64_t>(n_) && c < 0x80 && (c | 0x20) == inst.c;
                    if (ok) pos++, pc++;
                    break;
                }
                case Op::AnyAll:
                    ok = pos < static_cast<int64_t>(n_);
                    if (ok) pos++, pc++;
                    break;
                case Op::AnyLine:
                    ok = pos < static_cast<int64_t>(n_) && !isLineTerminator(s_->at(static_cast<size_t>(pos)), inst.flag);
                    if (ok) pos++, pc++;
                    break;
                case Op::Class:
                    ok = pos < static_cast<int64_t>(n_) && inClass(p_, inst.a, s_->at(static_cast<size_t>(pos)));
                    if (ok) pos++, pc++;
                    break;
                case Op::Split:
                    if (!firstTry(pc, pos, target)) {
                        ok = false;  // tried from here before, and it failed
                        break;
                    }
                    stack_.push_back(Entry{Entry::Alt, inst.b, pos, 0});
                    pc = inst.a;
                    break;
                case Op::Jmp:
                    pc = inst.a;
                    break;
                case Op::Save:
                    // A group's start is held aside as it opens and written with its end as it
                    // closes, as Java's are: a back reference inside the group, or a later look at
                    // it, sees its last whole match, never a new start with the old end
                    // ((ab|c\1)+$ matched "abc").
                    if (inst.a % 2 == 0) {
                        const size_t held = static_cast<size_t>(p_.loops + inst.a / 2);
                        stack_.push_back(Entry{Entry::Register, 0, regs_[held], static_cast<int>(held)});
                        regs_[held] = pos;
                    } else {
                        const int start = inst.a - 1;
                        stack_.push_back(Entry{Entry::Capture, 0, caps_[static_cast<size_t>(start)], start});
                        stack_.push_back(Entry{Entry::Capture, 0, caps_[static_cast<size_t>(inst.a)], inst.a});
                        caps_[static_cast<size_t>(start)] = regs_[static_cast<size_t>(p_.loops + start / 2)];
                        caps_[static_cast<size_t>(inst.a)] = pos;
                        if (p_.lastSlot >= 0) {
                            // CPython's lastindex: the group closed last.
                            stack_.push_back(Entry{Entry::Capture, 0, caps_[static_cast<size_t>(p_.lastSlot)], p_.lastSlot});
                            caps_[static_cast<size_t>(p_.lastSlot)] = inst.a / 2;
                        }
                    }
                    pc++;
                    break;
                case Op::LoopSet:
                    stack_.push_back(Entry{Entry::Register, 0, regs_[static_cast<size_t>(inst.a)], inst.a});
                    regs_[static_cast<size_t>(inst.a)] = pos;
                    pc++;
                    break;
                case Op::LoopCheck:
                    // An iteration that took nothing ends the loop, what it captured kept, as in Java.
                    pc = regs_[static_cast<size_t>(inst.a)] == pos ? inst.b : pc + 1;
                    break;
                case Op::Match:
                    if ((target >= 0 && pos != target) || (requireEnd && pos != static_cast<int64_t>(n_)) ||
                        (depth_ == 0 && pos == notEmptyAt_ && start_ == notEmptyAt_)) {
                        ok = false;
                        break;
                    }
                    end = pos;
                    return true;
                case Op::AtTarget:
                    ok = pos == target;
                    if (ok) pc++;
                    break;
                case Op::Run: {
                    if (!firstTry(pc, pos, target)) {
                        ok = false;  // come to from here before, and it failed
                        break;
                    }
                    const int64_t n = static_cast<int64_t>(n_);
                    int64_t p = pos;
                    if (memoed(pc, target)) {
                        // Coming to the run at a position it passes over would go on from fewer of
                        // the same places: each noted as tried, as a loop of Splits notes each one.
                        // Where the run was come to before, all after it has been tried: it stops.
                        while (p < n && (inst.test == Op::Class ? inClass(p_, inst.a, s_->at(static_cast<size_t>(p))) : oneOf(inst, s_->at(static_cast<size_t>(p)))) &&
                               !wasTried(pc, p + 1)) {
                            p++;
                            noteTried(pc, p, target);
                        }
                    } else if (inst.test == Op::Class && s_->width == 1) {
                        // Text of one byte a character, ASCII ones tested by the class's bits alone.
                        const uint8_t* text = reinterpret_cast<const uint8_t*>(s_->units().data());
                        const auto& bits = p_.asciiBits[static_cast<size_t>(inst.a)];
                        while (p < n) {
                            const uint8_t c = text[p];
                            if (c < 128 ? ((bits[c >> 6] >> (c & 63)) & 1) == 0 : !inClass(p_, inst.a, c)) break;
                            p++;
                        }
                    } else if (inst.test == Op::Class) {
                        while (p < n && inClass(p_, inst.a, s_->at(static_cast<size_t>(p)))) p++;
                    } else {
                        while (p < n && oneOf(inst, s_->at(static_cast<size_t>(p)))) p++;
                    }
                    const int64_t from = runFrom(pc, p, pos, target);
                    if (from < 0) {
                        ok = false;
                        break;
                    }
                    if (from > pos) stack_.push_back(Entry{Entry::Run, pc, from - 1, pos});
                    pos = from;
                    pc++;
                    break;
                }
                case Op::Begin:
                    ok = pos == 0;
                    if (ok) pc++;
                    break;
                case Op::End:
                    ok = pos == static_cast<int64_t>(n_);
                    if (ok) pc++;
                    break;
                case Op::LineStart:
                    // CPython's ^ in MULTILINE: at the start and after every \n, the end of the text too.
                    ok = inst.c == 1 ? pos == 0 || ch(pos - 1) == '\n' : lineStart(pos, inst.flag);
                    if (ok) pc++;
                    break;
                case Op::EndFinal:
                    ok = dollar(pos, false, inst.flag);
                    if (ok) pc++;
                    break;
                case Op::LineEnd:
                    ok = dollar(pos, true, inst.flag);
                    if (ok) pc++;
                    break;
                case Op::WordBoundary:
                case Op::NotWordBoundary: {
                    const auto word = [&](uint32_t c) { return inst.c == 1 ? snippyIs(c, kSnippyWord) : isAsciiWord(c); };
                    const bool before = pos > 0 && word(s_->at(static_cast<size_t>(pos - 1)));
                    const bool after = pos < static_cast<int64_t>(n_) && word(s_->at(static_cast<size_t>(pos)));
                    ok = (before != after) == (inst.op == Op::WordBoundary);
                    if (ok) pc++;
                    break;
                }
                case Op::PreviousEnd:
                    ok = pos == static_cast<int64_t>(searchStart_);
                    if (ok) pc++;
                    break;
                case Op::BackRef: {
                    ok = inst.a >= 0;
                    if (!ok) break;
                    const int64_t from = caps_[static_cast<size_t>(2 * inst.a)], to = caps_[static_cast<size_t>(2 * inst.a + 1)];
                    ok = from >= 0 && to >= from;
                    if (!ok) break;
                    const int64_t length = to - from;
                    ok = pos + length <= static_cast<int64_t>(n_);
                    for (int64_t k = 0; ok && k < length; k++) {
                        const uint32_t a = s_->at(static_cast<size_t>(from + k)), b = s_->at(static_cast<size_t>(pos + k));
                        if (inst.c == 3) ok = snippyLower(a) == snippyLower(b);
                        else if (inst.c == 2) ok = fold(a) == fold(b);
                        else if (inst.c == 1) ok = a == b || (a < 0x80 && b < 0x80 && ((a | 0x20) == (b | 0x20)) && (a | 0x20) >= 'a' && (a | 0x20) <= 'z');
                        else ok = a == b;
                    }
                    if (ok) pos += length, pc++;
                    break;
                }
                case Op::Look: {
                    const size_t mark = stack_.size();
                    bool found = false;
                    depth_++;
                    if (!inst.behind) {
                        int64_t e;
                        found = run(inst.a, pos, e);
                    } else {
                        for (int64_t k = inst.minLen; k <= inst.maxLen && !found; k++) {
                            if (pos - k < 0) break;
                            int64_t e;
                            found = run(inst.a, pos - k, e, pos);
                        }
                    }
                    depth_--;
                    if (inst.flag) {
                        // Negative: what the part did is undone either way.
                        if (found) unwindTo(mark);
                        ok = !found;
                    } else {
                        ok = found;
                        if (found) keepOnlyRestores(mark);
                    }
                    if (ok) pc++;
                    break;
                }
                case Op::Atomic: {
                    const size_t mark = stack_.size();
                    int64_t e;
                    depth_++;
                    ok = run(inst.a, pos, e);
                    depth_--;
                    if (ok) {
                        keepOnlyRestores(mark);
                        pos = e;
                        pc++;
                    }
                    break;
                }
            }
            if (ok) continue;
            // Back to the last alternative, putting back what was changed since.
            poll();
            bool resumed = false;
            while (stack_.size() > base) {
                const Entry e = stack_.back();
                stack_.pop_back();
                if (e.kind == Entry::Capture) {
                    caps_[static_cast<size_t>(e.slot)] = e.value;
                } else if (e.kind == Entry::Register) {
                    regs_[static_cast<size_t>(e.slot)] = e.value;
                } else {
                    if (e.kind == Entry::Run) {
                        // A run given back to the longest it has not gone on from before, the rest
                        // of it kept for later; one with nothing left is passed by.
                        const int64_t from = runFrom(e.pc, e.value, e.slot, target);
                        if (from < 0) continue;
                        if (from > e.slot) stack_.push_back(Entry{Entry::Run, e.pc, from - 1, e.slot});
                        pc = e.pc + 1;
                        pos = from;
                    } else {
                        pc = e.pc;
                        pos = e.value;
                    }
                    resumed = true;
                    break;
                }
            }
            if (!resumed) return false;
        }
    }

    // Leaves only the entries that put captures back, above `mark`: the part's alternatives
    // are forgotten, and what it captured is still undone should the match back out past it.
    void keepOnlyRestores(size_t mark) {
        size_t out = mark;
        for (size_t k = mark; k < stack_.size(); k++)
            if (stack_[k].kind == Entry::Capture || stack_[k].kind == Entry::Register) stack_[out++] = stack_[k];
        stack_.resize(out);
    }
    // Whether the program goes on from branch point `pc` at `pos` for the first time, noting it:
    // false where it went on from there before and failed (the memo, once it pays). A Split is a
    // branch point, and so is a Run at each position it may go on from.
    // A Run has two numbers: `part` 0 for coming to it at `pos`, 1 for going on after it from `pos`.
    bool firstTry(int pc, int64_t pos, int64_t target, int part = 0) {
        if (!p_.memo || target >= 0 || static_cast<size_t>(pc) >= p_.splitNumber.size()) return true;
        if (++splitsTaken_ > kMemoAfter && memo_.empty() && !memoRefused_) startMemo();
        if (memo_.empty()) return true;
        const size_t bit = static_cast<size_t>(p_.splitNumber[static_cast<size_t>(pc)] + part) * (n_ + 1) + static_cast<size_t>(pos);
        if ((memo_[bit >> 6] >> (bit & 63)) & 1) return false;
        memo_[bit >> 6] |= uint64_t{1} << (bit & 63);
        return true;
    }
    // Whether the memo is kept for branch point `pc` now.
    bool memoed(int pc, int64_t target) const {
        return p_.memo && target < 0 && static_cast<size_t>(pc) < p_.splitNumber.size() && !memo_.empty();
    }
    // Whether branch point `pc` was come to at `pos` before (the memo kept).
    bool wasTried(int pc, int64_t pos) const {
        const size_t bit = static_cast<size_t>(p_.splitNumber[static_cast<size_t>(pc)]) * (n_ + 1) + static_cast<size_t>(pos);
        return (memo_[bit >> 6] >> (bit & 63)) & 1;
    }
    // Notes branch point `pc` as gone on from at `pos`, without asking.
    void noteTried(int pc, int64_t pos, int64_t target) {
        if (!p_.memo || target >= 0 || static_cast<size_t>(pc) >= p_.splitNumber.size() || memo_.empty()) return;
        const size_t bit = static_cast<size_t>(p_.splitNumber[static_cast<size_t>(pc)]) * (n_ + 1) + static_cast<size_t>(pos);
        memo_[bit >> 6] |= uint64_t{1} << (bit & 63);
    }
    // Where the Run at `pc`, which took the text from `low` up to `high`, goes on from: the
    // longest it has not gone on from before, or -1 when it has from every one.
    int64_t runFrom(int pc, int64_t high, int64_t low, int64_t target) {
        for (int64_t at = high; at >= low; at--)
            if (firstTry(pc, at, target, 1)) return at;
        return -1;
    }
    // Whether `c` is the one character a Run's instruction matches (any but Class).
    bool oneOf(const Inst& inst, uint32_t c) const {
        switch (inst.test) {
            case Op::Char: return c == inst.c;
            case Op::CharFold: return fold(c) == inst.c;
            case Op::CharFoldAscii: return c < 0x80 && (c | 0x20) == inst.c;
            case Op::CharFoldSnippy: return snippyForm(c) == inst.c;
            case Op::AnyAll: return true;
            case Op::AnyLine: return !isLineTerminator(c, inst.flag);
            default: return inClass(p_, inst.a, c);
        }
    }
    void unwindTo(size_t mark) {
        while (stack_.size() > mark) {
            const Entry e = stack_.back();
            stack_.pop_back();
            if (e.kind == Entry::Capture) caps_[static_cast<size_t>(e.slot)] = e.value;
            else if (e.kind == Entry::Register) regs_[static_cast<size_t>(e.slot)] = e.value;
        }
    }
    void reset() {
        stack_.clear();
        std::fill(regs_.begin(), regs_.end(), -1);
        // What failed from a branch point at a position fails whatever the start: the table is kept.
    }

private:
    const Program& p_;
    const ObjString* s_;
    const size_t n_;
    const int64_t notEmptyAt_;  // a match that starts here may not be empty
    int64_t start_ = 0;         // where the match being tried starts
    int depth_ = 0;             // inside how many look-arounds and atomic groups
    std::vector<int64_t>& caps_;
    std::vector<int64_t> regs_;
    std::vector<Entry>& stack_;
    const size_t searchStart_;
    const std::function<void()>* stop_;
    uint32_t steps_ = 0;
    // Which branch points failed at which positions, once the search has taken enough branches
    // for it to pay; refused where the table would be too large.
    static constexpr uint64_t kMemoAfter = 20000;
    static constexpr uint64_t kMemoMostBits = uint64_t{1} << 31;  // 256 MB
    uint64_t splitsTaken_ = 0;
    std::vector<uint64_t> memo_;
    bool memoRefused_ = false;
    void startMemo() {
        const uint64_t bits = static_cast<uint64_t>(p_.splits) * (n_ + 1);
        if (bits > kMemoMostBits) {
            memoRefused_ = true;
            return;
        }
        memo_.assign(static_cast<size_t>((bits + 63) / 64), 0);
    }

    uint32_t ch(int64_t i) const { return s_->at(static_cast<size_t>(i)); }

    // ^ in MULTILINE, as Java has it: at the start, or after a line terminator but not between
    // \r and \n, and never at the end of the input.
    bool lineStart(int64_t i, bool unix) const {
        const int64_t end = static_cast<int64_t>(n_);
        if (i > 0) {
            const uint32_t before = ch(i - 1);
            if (unix) {
                if (before != '\n') return false;
            } else {
                if (!isLineTerminator(before, false)) return false;
                if (before == '\r' && i < end && ch(i) == '\n') return false;
            }
        }
        return i != end;
    }

    // $ (and \Z), as Java's Dollar: at the end, or before a line terminator; without
    // MULTILINE only one that ends the input.
    bool dollar(int64_t i, bool multiline, bool unix) const {
        const int64_t end = static_cast<int64_t>(n_);
        if (unix) {
            if (i < end) {
                if (ch(i) != '\n') return false;
                return multiline || i == end - 1;
            }
            return true;
        }
        if (!multiline) {
            if (i < end - 2) return false;
            if (i == end - 2) {
                if (ch(i) != '\r' || ch(i + 1) != '\n') return false;
            }
        }
        if (i < end) {
            const uint32_t c = ch(i);
            if (c == '\n') {
                if (i > 0 && ch(i - 1) == '\r') return false;
                return true;
            }
            if (c == '\r' || c == 0x85 || c == 0x2028 || c == 0x2029) return true;
            return false;
        }
        return true;
    }
};

}  // namespace

// ---- The pattern ----

Pattern::Pattern(const std::u32string& pattern, uint32_t flags) : program_(std::make_unique<Program>()) {
    Program& p = *program_;
    std::string utf8;
    for (char32_t ch : pattern) appendUtf8(utf8, static_cast<uint32_t>(ch));
    Parser parser(pattern, flags, p);
    int root;
    try {
        root = parser.parse();
    } catch (const SyntaxError&) {
        throw;
    }
    std::vector<Node>& nodes = parser.nodes_;
    Compiler compiler(nodes, p);
    try {
        compiler.compile(root);
        compiler.emit(Inst{Op::Match});
        compiler.finish();
    } catch (const SyntaxError& e) {
        throw SyntaxError(e.description, -1, utf8);
    }
    p.start = 0;
    firstOf(nodes, root, p);
    // Which ASCII characters each class holds, worked out once: for the classes the program tests
    // only, not the parts of one ([a[b]], [a&&b]), so that each part is looked at under one class
    // alone (classes nested thousands deep took time as the square of their depth).
    p.asciiBits.assign(p.classes.size(), std::array<uint64_t, 2>{0, 0});
    std::vector<bool> tested(p.classes.size(), false);
    for (const Node& node : nodes)
        if (node.kind == Node::Class) tested[static_cast<size_t>(node.cls)] = true;
    for (size_t k = 0; k < p.classes.size(); k++) {
        if (!tested[k]) continue;
        std::array<uint64_t, 2> bits{0, 0};
        for (uint32_t c = 0; c < 128; c++) {
            if (p.classFold[k] == 3) {
                if (inClassSnippy(p, static_cast<int>(k), c)) bits[c >> 6] |= uint64_t{1} << (c & 63);
                continue;
            }
            bool in = inClassRaw(p, static_cast<int>(k), c);
            const bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
            if (!in && p.classFold[k] != 0 && letter) {
                const uint32_t other = c ^ 0x20;
                in = inClassRaw(p, static_cast<int>(k), other);
                // A letter past ASCII whose other case is this one: the Kelvin sign is K, the long s is S.
                if (!in && p.classFold[k] == 2) {
                    if (c == 'k' || c == 'K') in = inClassRaw(p, static_cast<int>(k), 0x212A);
                    if (c == 's' || c == 'S') in = in || inClassRaw(p, static_cast<int>(k), 0x017F);
                }
            }
            if (in) bits[c >> 6] |= uint64_t{1} << (c & 63);
        }
        p.asciiBits[k] = bits;
    }
}

Pattern::~Pattern() = default;

int Pattern::groupCount() const { return program_->groups; }
int Pattern::snippyFlags() const { return program_->snippyFlags; }
const std::map<std::string, int>& Pattern::groupNames() const { return program_->names; }

bool Pattern::match(const ObjString* text, size_t from, Mode mode, std::vector<int64_t>& groups, const std::function<void()>* stop, size_t end,
                    int64_t notEmptyAt) const {
    const Program& p = *program_;
    groups.assign(static_cast<size_t>(2 * (p.groups + 1) + (p.snippy ? 1 : 0)), -1);
    const size_t n = std::min(text->length, end);
    if (from > n) return false;
    Runner runner(p, text, groups, from, stop, end, notEmptyAt);
    if (mode != Mode::Search) {
        int64_t end;
        if (!runner.run(p.start, static_cast<int64_t>(from), end, -1, mode == Mode::Entire)) return false;
        groups[0] = static_cast<int64_t>(from);
        groups[1] = end;
        return true;
    }
    for (size_t at = from; at <= n; at++) {
        if (p.anchored && at > 0) return false;
        if (p.hasFirst) {
            while (at < n && text->at(at) != p.first) at++;
            if (at >= n) return false;
        }
        runner.reset();
        int64_t end;
        if (runner.run(p.start, static_cast<int64_t>(at), end)) {
            groups[0] = static_cast<int64_t>(at);
            groups[1] = end;
            return true;
        }
        std::fill(groups.begin(), groups.end(), -1);
    }
    return false;
}

}  // namespace ww::regex
