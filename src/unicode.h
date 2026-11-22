// WinterWolf's own Unicode data (Unicode 16.0.0, made by tools/unicode/make.ww from Unicode's own
// files): what a character is and its other cases, the same on every system and every processor,
// where the system's tables differed (Windows' from Linux's, and both from Unicode's latest).
// Characters, strings and regular expressions (outside Snippy's syntax) all ask here, with Java's
// meaning for each question, as Kotlin's Char and String give it.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ww::unicode {

// The general categories, in the order the table numbers them (Cn, unassigned, is 0).
enum class Category : uint8_t {
    Cn, Lu, Ll, Lt, Lm, Lo, Mn, Mc, Me, Nd, Nl, No, Pc, Pd, Ps, Pe, Pi, Pf, Po, Sm, Sc, Sk, So, Zs, Zl, Zp, Cc, Cf, Cs, Co,
};

// The properties of PropList.txt and DerivedCoreProperties.txt the table carries.
enum Property : uint16_t {
    kAlphabetic = 1, kUppercase = 2, kLowercase = 4, kWhiteSpace = 8, kCased = 16, kCaseIgnorable = 32, kHexDigit = 64,
    kJoinControl = 128, kIdeographic = 256,
};

Category category(uint32_t c);
bool has(uint32_t c, Property p);

// The category's letter: L, M, N, P, S, Z or C.
char major(Category k);

// Java's Character methods.
bool isLetter(uint32_t c);          // L*
bool isDigit(uint32_t c);           // Nd
bool isLetterOrDigit(uint32_t c);   // L* or Nd
bool isUpperCase(uint32_t c);       // Uppercase: Lu and Other_Uppercase
bool isLowerCase(uint32_t c);       // Lowercase: Ll and Other_Lowercase
bool isTitleCase(uint32_t c);       // Lt
bool isCased(uint32_t c);           // Cased: upper, lower or title case (Uppercase, Lowercase or Lt)
bool isCaseIgnorable(uint32_t c);   // Case_Ignorable: passed over when a word's case is looked for (', ., marks)
// Every code point that is Cased or has a case mapping or folding of its own, in order (about
// 3,000; found once, by the blocks of the table that have any).
const std::vector<uint32_t>& casedCodePoints();
bool isAlphabetic(uint32_t c);      // Alphabetic
bool isWhitespace(uint32_t c);      // Java's: the separators but the no-break ones, \t to \r, \x1C to \x1F
bool isSpaceChar(uint32_t c);       // Z*

// The value of a decimal digit (category Nd, UnicodeData's decimal digit): 0 to 9 ('٣' is 3), -1
// for any other character, as Java's Character.digit(c, 10) gives it for those.
int decimalValue(uint32_t c);

// The simple mappings (one character for one; the character itself where it has none), as
// Java's Character.toUpperCase, toLowerCase and toTitleCase give them.
uint32_t toUpper(uint32_t c);
uint32_t toLower(uint32_t c);
uint32_t toTitle(uint32_t c);

// A string in upper or lower case as Java's String.toUpperCase(Locale.ROOT) and toLowerCase
// give it: the full mappings (ß to SS, İ to i̇), and a final Σ as ς. UTF-8 in and out.
std::string upper(const std::string& s);
std::string lower(const std::string& s);
std::u32string upper(const std::u32string& s);
std::u32string lower(const std::u32string& s);
// Python's str methods on case, each as CPython 3.14 gives it (and as Unicode defines the parts):
// foldCase is casefold, the full case folding for comparing without case (ß and SS are both ss;
// CaseFolding.txt's F, else its C line, else the full lowercase); title puts the first cased
// letter of each run of cased letters in title case and the rest in lower case ("ǆemal" is
// "ǅemal", "ß" is "Ss"); capitalize puts the first character in title case and the rest in
// lower case; swapCase lowers what is upper case and raises what is lower case, by the full
// mappings. Lowering anywhere keeps the final sigma rule. UTF-8 in and out, or code points.
std::string foldCase(const std::string& s);
std::string title(const std::string& s);
std::string capitalize(const std::string& s);
std::string swapCase(const std::string& s);
std::u32string foldCase(const std::u32string& s);
std::u32string title(const std::u32string& s);
std::u32string capitalize(const std::u32string& s);
std::u32string swapCase(const std::u32string& s);

}  // namespace ww::unicode
