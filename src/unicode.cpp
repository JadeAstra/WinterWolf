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
};
struct UnicodeSpecial {
    uint16_t lowerAt, lowerLength, titleAt, titleLength, upperAt, upperLength;
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
bool isAlphabetic(uint32_t c) { return has(c, kAlphabetic); }
bool isSpaceChar(uint32_t c) { return major(category(c)) == 'Z'; }
bool isWhitespace(uint32_t c) {
    if (c == 0xA0 || c == 0x2007 || c == 0x202F) return false;
    return (c >= 9 && c <= 13) || (c >= 0x1C && c <= 0x1F) || isSpaceChar(c);
}

uint32_t toUpper(uint32_t c) { return moved(c, recordOf(c).upper); }
uint32_t toLower(uint32_t c) { return moved(c, recordOf(c).lower); }
uint32_t toTitle(uint32_t c) { return moved(c, recordOf(c).title); }

std::u32string upper(const std::u32string& s) {
    std::u32string out;
    out.reserve(s.size());
    for (char32_t ch : s) {
        const uint32_t c = static_cast<uint32_t>(ch);
        const UnicodeRecord& r = recordOf(c);
        if (r.special != 0) {
            const UnicodeSpecial& sp = kUnicodeSpecial[r.special - 1];
            for (uint16_t k = 0; k < sp.upperLength; k++) out.push_back(static_cast<char32_t>(kUnicodeSpecialChars[sp.upperAt + k]));
        } else {
            out.push_back(static_cast<char32_t>(moved(c, r.upper)));
        }
    }
    return out;
}

std::u32string lower(const std::u32string& s) {
    std::u32string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        const uint32_t c = static_cast<uint32_t>(s[i]);
        const UnicodeRecord& r = recordOf(c);
        if (r.special != 0) {
            const UnicodeSpecial& sp = kUnicodeSpecial[r.special - 1];
            for (uint16_t k = 0; k < sp.lowerLength; k++) out.push_back(static_cast<char32_t>(kUnicodeSpecialChars[sp.lowerAt + k]));
        } else if (c == 0x3A3 && finalSigma(s, i)) {
            out.push_back(U'ς');
        } else {
            out.push_back(static_cast<char32_t>(moved(c, r.lower)));
        }
    }
    return out;
}

std::string upper(const std::string& s) {
    bool ascii = true;
    for (unsigned char c : s) ascii = ascii && c < 0x80;
    if (ascii) {
        std::string out = s;
        for (char& c : out)
            if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 32);
        return out;
    }
    return toUtf8(upper(fromUtf8(s)));
}

std::string lower(const std::string& s) {
    bool ascii = true;
    for (unsigned char c : s) ascii = ascii && c < 0x80;
    if (ascii) {
        std::string out = s;
        for (char& c : out)
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
        return out;
    }
    return toUtf8(lower(fromUtf8(s)));
}

}  // namespace ww::unicode
