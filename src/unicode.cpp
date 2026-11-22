// WinterWolf's own Unicode data: see unicode.h.

#include "unicode.h"

#include "builtins.h"
#include "value.h"

namespace ww::unicode {

namespace {

struct UnicodeRecord {
    uint8_t category;
    uint16_t properties;
    int32_t upper, lower, title;
    uint16_t special;  // 1 + the special casing's number, 0 for none
    int8_t digit;      // the decimal digit's value, -1 for none
    int32_t fold;      // the case folding where it is one character
};
struct UnicodeSpecial {
    uint16_t lowerAt, lowerLength, titleAt, titleLength, upperAt, upperLength, foldAt, foldLength;
};

#include "unicode_data.inc"

const UnicodeRecord& recordOf(uint32_t c) {
    if (c > 0x10FFFF) c = 0x10FFFF;  // nothing past Unicode is assigned (U+10FFFF is not)
    return kUnicodeRecord[kUnicodeEntry[static_cast<size_t>(kUnicodeBlock[c >> 7]) * 128 + (c & 127)]];
}

uint32_t moved(uint32_t c, int32_t by) { return c > 0x10FFFF ? c : static_cast<uint32_t>(static_cast<int64_t>(c) + by); }

// Whether the Σ at `i` ends a word: a cased letter before it, past any case-ignorable ones, and
// none after it (Unicode's Final_Sigma, as Java's toLowerCase takes it).
bool finalSigma(const std::u32string& s, size_t i) {
    bool before = false;
    for (size_t k = i; k > 0; k--) {
        const uint32_t c = static_cast<uint32_t>(s[k - 1]);
        if (has(c, kCaseIgnorable)) continue;
        before = has(c, kCased);
        break;
    }
    if (!before) return false;
    for (size_t k = i + 1; k < s.size(); k++) {
        const uint32_t c = static_cast<uint32_t>(s[k]);
        if (has(c, kCaseIgnorable)) continue;
        return !has(c, kCased);
    }
    return true;
}

// The character's full mappings, put on the end of `out`.
void appendSpecial(std::u32string& out, uint16_t at, uint16_t length) {
    for (uint16_t k = 0; k < length; k++) out.push_back(static_cast<char32_t>(kUnicodeSpecialChars[at + k]));
}
void appendUpper(std::u32string& out, uint32_t c) {
    const UnicodeRecord& r = recordOf(c);
    if (r.special != 0) appendSpecial(out, kUnicodeSpecial[r.special - 1].upperAt, kUnicodeSpecial[r.special - 1].upperLength);
    else out.push_back(static_cast<char32_t>(moved(c, r.upper)));
}
void appendTitle(std::u32string& out, uint32_t c) {
    const UnicodeRecord& r = recordOf(c);
    if (r.special != 0) appendSpecial(out, kUnicodeSpecial[r.special - 1].titleAt, kUnicodeSpecial[r.special - 1].titleLength);
    else out.push_back(static_cast<char32_t>(moved(c, r.title)));
}
void appendFold(std::u32string& out, uint32_t c) {
    const UnicodeRecord& r = recordOf(c);
    if (r.special != 0 && kUnicodeSpecial[r.special - 1].foldLength != 0) appendSpecial(out, kUnicodeSpecial[r.special - 1].foldAt, kUnicodeSpecial[r.special - 1].foldLength);
    else out.push_back(static_cast<char32_t>(moved(c, r.fold)));
}
// The character at `i` of `s` in lower case: a capital sigma that ends a word as ς.
void appendLower(std::u32string& out, const std::u32string& s, size_t i) {
    const uint32_t c = static_cast<uint32_t>(s[i]);
    const UnicodeRecord& r = recordOf(c);
    if (r.special != 0) appendSpecial(out, kUnicodeSpecial[r.special - 1].lowerAt, kUnicodeSpecial[r.special - 1].lowerLength);
    else if (c == 0x3A3 && finalSigma(s, i)) out.push_back(U'ς');
    else out.push_back(static_cast<char32_t>(moved(c, r.lower)));
}

bool isAscii(const std::string& s) {
    for (unsigned char c : s)
        if (c >= 0x80) return false;
    return true;
}

std::u32string fromUtf8(const std::string& s) {
    const Characters text = Characters::fromUtf8(s);
    std::u32string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); i++) out.push_back(static_cast<char32_t>(text[i]));
    return out;
}
std::string toUtf8(const std::u32string& s) {
    std::string out;
    out.reserve(s.size());
    for (char32_t c : s) appendUtf8(out, static_cast<uint32_t>(c));
    return out;
}

}  // namespace

Category category(uint32_t c) { return static_cast<Category>(recordOf(c).category); }
bool has(uint32_t c, Property p) { return (recordOf(c).properties & p) != 0; }

char major(Category k) {
    switch (k) {
        case Category::Lu: case Category::Ll: case Category::Lt: case Category::Lm: case Category::Lo: return 'L';
        case Category::Mn: case Category::Mc: case Category::Me: return 'M';
        case Category::Nd: case Category::Nl: case Category::No: return 'N';
        case Category::Pc: case Category::Pd: case Category::Ps: case Category::Pe: case Category::Pi: case Category::Pf: case Category::Po: return 'P';
        case Category::Sm: case Category::Sc: case Category::Sk: case Category::So: return 'S';
        case Category::Zs: case Category::Zl: case Category::Zp: return 'Z';
        default: return 'C';
    }
}

bool isLetter(uint32_t c) { return major(category(c)) == 'L'; }
bool isDigit(uint32_t c) { return category(c) == Category::Nd; }
bool isLetterOrDigit(uint32_t c) { return isLetter(c) || isDigit(c); }
bool isUpperCase(uint32_t c) { return has(c, kUppercase); }
bool isLowerCase(uint32_t c) { return has(c, kLowercase); }
bool isTitleCase(uint32_t c) { return category(c) == Category::Lt; }
bool isCased(uint32_t c) { return has(c, kCased); }
bool isCaseIgnorable(uint32_t c) { return has(c, kCaseIgnorable); }

const std::vector<uint32_t>& casedCodePoints() {
    static const std::vector<uint32_t> found = [] {
        const auto cased = [](const UnicodeRecord& r) {
            return (r.properties & kCased) != 0 || r.upper != 0 || r.lower != 0 || r.title != 0 || r.special != 0 || r.fold != 0;
        };
        // Which of the table's distinct blocks of 128 have any: most share one block with none.
        constexpr size_t kBlocks = sizeof(kUnicodeEntry) / sizeof(kUnicodeEntry[0]) / 128;
        std::vector<int8_t> blockHas(kBlocks, -1);
        std::vector<uint32_t> out;
        for (uint32_t b = 0; b < 0x110000 / 128; b++) {
            const size_t block = kUnicodeBlock[b];
            if (blockHas[block] < 0) {
                blockHas[block] = 0;
                for (size_t k = 0; k < 128; k++)
                    if (cased(kUnicodeRecord[kUnicodeEntry[block * 128 + k]])) blockHas[block] = 1;
            }
            if (blockHas[block] == 0) continue;
            for (uint32_t k = 0; k < 128; k++)
                if (cased(kUnicodeRecord[kUnicodeEntry[block * 128 + k]])) out.push_back(b * 128 + k);
        }
        return out;
    }();
    return found;
}
bool isAlphabetic(uint32_t c) { return has(c, kAlphabetic); }
bool isSpaceChar(uint32_t c) { return major(category(c)) == 'Z'; }
bool isWhitespace(uint32_t c) {
    if (c == 0xA0 || c == 0x2007 || c == 0x202F) return false;
    return (c >= 9 && c <= 13) || (c >= 0x1C && c <= 0x1F) || isSpaceChar(c);
}

int decimalValue(uint32_t c) { return recordOf(c).digit; }

uint32_t toUpper(uint32_t c) { return moved(c, recordOf(c).upper); }
uint32_t toLower(uint32_t c) { return moved(c, recordOf(c).lower); }
uint32_t toTitle(uint32_t c) { return moved(c, recordOf(c).title); }

std::u32string upper(const std::u32string& s) {
    std::u32string out;
    out.reserve(s.size());
    for (char32_t c : s) appendUpper(out, static_cast<uint32_t>(c));
    return out;
}

std::u32string lower(const std::u32string& s) {
    std::u32string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) appendLower(out, s, i);
    return out;
}

std::u32string foldCase(const std::u32string& s) {
    std::u32string out;
    out.reserve(s.size());
    for (char32_t c : s) appendFold(out, static_cast<uint32_t>(c));
    return out;
}

// CPython's do_title: a character after a cased one lowered, any other put in title case.
std::u32string title(const std::u32string& s) {
    std::u32string out;
    out.reserve(s.size());
    bool previousCased = false;
    for (size_t i = 0; i < s.size(); i++) {
        const uint32_t c = static_cast<uint32_t>(s[i]);
        if (previousCased) appendLower(out, s, i);
        else appendTitle(out, c);
        previousCased = isCased(c);
    }
    return out;
}

// CPython's do_capitalize (since 3.8): the first character in title case, the rest lowered.
std::u32string capitalize(const std::u32string& s) {
    std::u32string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (i == 0) appendTitle(out, static_cast<uint32_t>(s[i]));
        else appendLower(out, s, i);
    }
    return out;
}

// CPython's do_swapcase: upper case lowered, lower case raised, anything else as it is.
std::u32string swapCase(const std::u32string& s) {
    std::u32string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        const uint32_t c = static_cast<uint32_t>(s[i]);
        if (isUpperCase(c)) appendLower(out, s, i);
        else if (isLowerCase(c)) appendUpper(out, c);
        else out.push_back(static_cast<char32_t>(c));
    }
    return out;
}

std::string upper(const std::string& s) {
    if (isAscii(s)) {
        std::string out = s;
        for (char& c : out)
            if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 32);
        return out;
    }
    return toUtf8(upper(fromUtf8(s)));
}

std::string lower(const std::string& s) {
    if (isAscii(s)) {
        std::string out = s;
        for (char& c : out)
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
        return out;
    }
    return toUtf8(lower(fromUtf8(s)));
}

std::string foldCase(const std::string& s) { return isAscii(s) ? lower(s) : toUtf8(foldCase(fromUtf8(s))); }
std::string title(const std::string& s) { return toUtf8(title(fromUtf8(s))); }
std::string capitalize(const std::string& s) { return toUtf8(capitalize(fromUtf8(s))); }
std::string swapCase(const std::string& s) { return toUtf8(swapCase(fromUtf8(s))); }

}  // namespace ww::unicode
