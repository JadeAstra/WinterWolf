// The native part of the standard library's Kotlin names: what is called often enough to
// matter, and what WinterWolf code cannot do for itself (reading a file, formatting a number,
// telling a letter from a digit). The rest is written in WinterWolf, in src/std/*.ww.

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>

#include "builtins.h"
#include "charset.h"
#include "fmath.h"
#include "host.h"
#include "unicode.h"
#include "vm.h"

namespace ww {

namespace {

namespace fs = std::filesystem;

// ---- Helpers ----

// Whether optional argument `i` was passed. Null counts as left out: it is what a parameter
// skipped on the way to a named one is given.
bool given(int argc, const Value* args, int i) { return i < argc && !args[i].isNull(); }

bool flag(int argc, const Value* args, int i) { return given(argc, args, i) && args[i].truthy(); }

std::string fold(const std::string& s) { return changeCase(s, false); }

// The byte at which character `index` of `s` starts; the end if there are not that many.
size_t offsetOfCharacter(std::string_view s, size_t index) {
    size_t i = 0;
    for (; i < s.size() && index > 0; index--) {
        i++;
        while (i < s.size() && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) i++;
    }
    return i;
}

int compareNumbers(Value a, Value b) {
    if (a.isInt() && b.isInt()) return (a.as.i > b.as.i) - (a.as.i < b.as.i);
    const double x = a.toDouble(), y = b.toDouble();
    return (x > y) - (x < y);
}

// How two values are ordered: numbers by size, strings by their characters, and null before
// everything else. Negative, zero or positive, as a comparator gives.
int compareValues(VM& vm, Value a, Value b) {
    if (a.isNull() || b.isNull()) return (b.isNull() ? 1 : 0) - (a.isNull() ? 1 : 0);
    if (a.isNumber() && b.isNumber()) return compareNumbers(a, b);
    if (a.isString() && b.isString()) {
        return compareText(asString(a), asString(b));
    }
    if (a.isBool() && b.isBool()) return static_cast<int>(a.as.b) - static_cast<int>(b.as.b);
    // Chars by their numbers; a byte as the int it holds.
    if (a.isChar() && b.isChar()) return (a.as.i > b.as.i) - (a.as.i < b.as.i);
    if (a.isByte()) a = Value::integer(a.as.i);
    if (b.isByte()) b = Value::integer(b.as.i);
    if (a.isNumber() && b.isNumber()) return compareNumbers(a, b);
    throw vm.runtimeError(Thrown::ClassCastException, "cannot compare " + vm.typeName(a) + " and " + vm.typeName(b));
}

// An int to a power of 0 or more, an error where it does not fit in an int (D27).
int64_t integerPower(VM& vm, int64_t base, int64_t exponent) {
    int64_t result = 1, b = base;
    bool bOverflowed = false;
    for (int64_t e = exponent; e != 0; e >>= 1) {
        if (e & 1) {
            if (bOverflowed || mulOverflows(result, b, result))
                throw vm.runtimeError(Thrown::ArithmeticException, std::to_string(base) + " to the power " + std::to_string(exponent) + " does not fit in an int");
        }
        if (e > 1 && !bOverflowed) bOverflowed = mulOverflows(b, b, b);
    }
    return result;
}

// ---- Strings ----

bool parseWhole(std::string_view s, int radix, int64_t& out) {
    if (!s.empty() && s[0] == '+') s.remove_prefix(1);
    if (s.empty()) return false;
    auto result = std::from_chars(s.data(), s.data() + s.size(), out, radix);
    return result.ec == std::errc() && result.ptr == s.data() + s.size();
}

// An int's 64 bits, for the unsigned helpers (D44).
uint64_t wholeOnly(VM& vm, Value v, const char* what) {
    if (!v.isInt()) throw vm.runtimeError(Thrown::ClassCastException, std::string(what) + " is for ints, not " + vm.typeName(v));
    return static_cast<uint64_t>(v.as.i);
}

int radixOf(VM& vm, int argc, const Value* args, int i) {
    if (!given(argc, args, i)) return 10;
    const int64_t radix = vm.toInt(args[i], "radix");
    if (radix < 2 || radix > 36) throw vm.runtimeError(Thrown::IllegalArgumentException, "a radix is from 2 to 36, not " + std::to_string(radix));
    return static_cast<int>(radix);
}

enum class CharacterClass { Letter, Digit, LetterOrDigit, Whitespace, Upper, Lower };

// What kind a character is, as Java's Character methods say (unicode.h): the same on every system.
bool characterIs(uint32_t c, CharacterClass which) {
    switch (which) {
        case CharacterClass::Letter: return unicode::isLetter(c);
        case CharacterClass::Digit: return unicode::isDigit(c);
        case CharacterClass::LetterOrDigit: return unicode::isLetterOrDigit(c);
        case CharacterClass::Whitespace: return unicode::isWhitespace(c);
        case CharacterClass::Upper: return unicode::isUpperCase(c);
        case CharacterClass::Lower: return unicode::isLowerCase(c);
    }
    return false;
}

// Whether every character of `s` is of the kind asked about; false for no characters at all.
bool charactersAre(const std::string& s, CharacterClass which) {
    if (s.empty()) return false;
    const Characters text = Characters::fromUtf8(s);
    for (size_t i = 0; i < text.size(); i++)
        if (!characterIs(text[i], which)) return false;
    return true;
}

// Python's isupper() and islower() (G21): whether `s` has a letter that has a case, and every such
// letter is upper case (`upper`) or lower case; digits, spaces and letters with no case do not
// count, and a titlecase letter (ǅ) is neither.
bool casedAre(const std::string& s, bool upper) {
    bool seen = false;
    const Characters text = Characters::fromUtf8(s);
    for (size_t i = 0; i < text.size(); i++) {
        const uint32_t c = text[i];
        const bool isUpper = unicode::isUpperCase(c), isLower = unicode::isLowerCase(c);
        if ((upper ? isLower : isUpper) || unicode::isTitleCase(c)) return false;
        seen = seen || (upper ? isUpper : isLower);
    }
    return seen;
}

// Thousands separators for the digits of a whole number: 1234567 -> 1,234,567.
std::string grouped(const std::string& digits) {
    std::string out;
    for (size_t i = 0; i < digits.size(); i++) {
        if (i > 0 && (digits.size() - i) % 3 == 0) out += ',';
        out += digits[i];
    }
    return out;
}

// A fraction as format writes it (f, e and g): the number's exact binary value rounded to the
// places asked for, a half to even, as C's printf and CPython do (D1 of
// docs/snippy-ww-checklist.html). So "%.1f" of 0.35 is 0.3 (0.35 is 0.34999... in binary) and
// "%.0f" of 2.5 is 2. std::to_chars with a precision rounds so, the same on every system.
// `magnitude` is not negative, and finite.
namespace {

// `magnitude` with `places` digits after the point.
std::string exactFixed(double magnitude, int places) {
    char buffer[1100];  // 309 whole digits, the point and 340 places at most
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), magnitude, std::chars_format::fixed, places);
    return std::string(buffer, result.ptr);
}

// `magnitude` as d.ddd with `places` digits after the point, and e+05: at least two digits in
// the exponent, as C and Java write it.
std::string exactScientific(double magnitude, int places, bool upper) {
    char buffer[400];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), magnitude, std::chars_format::scientific, places);
    std::string out(buffer, result.ptr);
    if (upper) out[out.find('e')] = 'E';
    return out;
}

// %g: `precision` significant digits; fixed when the number, so rounded, is at least 10^-4 and
// below 10^precision, else scientific, as C and Java decide. Trailing zeros are kept, as Java
// keeps them.
std::string exactGeneral(double magnitude, int precision, bool upper, bool& fixed) {
    if (precision == 0) precision = 1;
    const std::string scientific = exactScientific(magnitude, precision - 1, upper);
    const int exponent = std::stoi(scientific.substr(scientific.find_first_of("eE") + 1));
    fixed = exponent >= -4 && exponent < precision;
    return fixed ? exactFixed(magnitude, precision - 1 - exponent) : scientific;
}

} // namespace

// Whether `v` is the standard library's BigInt.
bool isBigInt(VM& vm, Value v) {
    if (!v.isObjType(ObjType::Instance)) return false;
    ObjClass* cls = asInstance(v)->cls;
    return cls->name->utf8() == "BigInt" && vm.classNamed("BigInt") == cls;
}

// A BigInt's digits in `radix` (its toString(radix)), a minus first where negative.
std::string bigIntText(VM& vm, Value v, int radix) {
    if (radix == 10) return vm.stringify(v);
    const Value* method = asInstance(v)->cls->methods.find(vm.intern("toString"));
    if (method == nullptr) throw vm.runtimeError(Thrown::InternalError, "BigInt has no toString");
    Value bound = Value::object(vm.alloc<ObjBoundMethod>(v, asClosure(*method)));
    TempRoot keep(vm, bound);
    const Value radixValue = Value::integer(radix);
    const Value result = vm.call(bound, 1, &radixValue);
    if (!result.isString()) throw vm.runtimeError(Thrown::InternalError, "BigInt's toString gave no text");
    return asString(result)->utf8();
}

// "%d items, %.2f each".format(3, 1.5): the patterns of Kotlin's (Java's) String.format.
//   %[index$][flags][width][.precision]conversion
// Flags: - left-justify, 0 pad with zeros, + always a sign, space for a plus sign,
// , thousands separators, and # the alternate form (0x or 0X before %x and %X, 0 before %o; a
// point always with %f, %e and %E). Conversions: d x X o (whole numbers: ints, whole floats for
// %d, and BigInts, which %x and %o give with their sign, as Java's BigInteger), f e E g G
// (fractions), s S (anything, as print shows it), c (a character), b (true or false), %% and %n.
std::string formatText(VM& vm, const std::string& pattern, int argc, const Value* args) {
    std::string out;
    int next = 0;
    const size_t size = pattern.size();
    auto digit = [&](size_t at) { return at < size && pattern[at] >= '0' && pattern[at] <= '9'; };

    for (size_t i = 0; i < size; i++) {
        if (pattern[i] != '%') {
            out += pattern[i];
            continue;
        }
        size_t at = i + 1;
        if (at < size && pattern[at] == '%') {
            out += '%';
            i = at;
            continue;
        }
        if (at < size && pattern[at] == 'n') {
            out += '\n';
            i = at;
            continue;
        }

        int index = -1;
        {
            size_t p = at;
            int number = 0;
            while (digit(p) && number < 100000) number = number * 10 + (pattern[p++] - '0');
            if (p > at && p < size && pattern[p] == '$') {
                index = number - 1;
                at = p + 1;
            }
        }
        bool left = false, zero = false, plus = false, space = false, comma = false, alternate = false;
        for (bool more = true; more && at < size;) {
            switch (pattern[at]) {
                case '-': left = true; break;
                case '0': zero = true; break;
                case '+': plus = true; break;
                case ' ': space = true; break;
                case ',': comma = true; break;
                case '#': alternate = true; break;
                default: more = false; break;
            }
            if (more) at++;
        }
        size_t width = 0;
        while (digit(at) && width < 100000) width = width * 10 + static_cast<size_t>(pattern[at++] - '0');
        int precision = -1;
        if (at < size && pattern[at] == '.') {
            at++;
            precision = 0;
            while (digit(at) && precision < 100000) precision = precision * 10 + (pattern[at++] - '0');
        }
        if (at >= size) throw vm.runtimeError(Thrown::IllegalArgumentException, "format: the pattern ends in the middle of a '%' item");
        const char conversion = pattern[at];
        i = at;

        if (index < 0) index = next++;
        if (index >= argc)
            throw vm.runtimeError(Thrown::IllegalArgumentException, "format: the pattern asks for more values than the " + std::to_string(argc) + " given");
        const Value value = args[index];

        // Java's alternate form goes with %x, %X, %o, %f, %e and %E only.
        if (alternate && conversion != 'x' && conversion != 'X' && conversion != 'o' && conversion != 'f' && conversion != 'e' && conversion != 'E')
            throw vm.runtimeError(Thrown::IllegalArgumentException, std::string("format: the flag '#' does not go with %") + conversion +
                                  "; it goes with %x, %X, %o, %f, %e and %E");
        std::string sign, body;
        bool numeric = false;
        switch (conversion) {
            case 'd': {
                if (isBigInt(vm, value)) {
                    body = bigIntText(vm, value, 10);
                    const bool negative = !body.empty() && body[0] == '-';
                    if (negative) body.erase(0, 1);
                    if (comma) body = grouped(body);
                    sign = negative ? "-" : plus ? "+" : space ? " " : "";
                    numeric = true;
                    break;
                }
                if (!value.isNumber() || (value.isFloat() && value.as.d != std::floor(value.as.d)))
                    throw vm.runtimeError(Thrown::IllegalArgumentException, "format: %d takes a whole number, not " + vm.stringify(value) +
                                          "; %f and %s take any number");
                const int64_t n = value.isInt() ? value.as.i : floatToInt(vm, value.as.d);
                const uint64_t magnitude = n < 0 ? uint64_t{0} - static_cast<uint64_t>(n) : static_cast<uint64_t>(n);
                body = std::to_string(magnitude);
                if (comma) body = grouped(body);
                sign = n < 0 ? "-" : plus ? "+" : space ? " " : "";
                numeric = true;
                break;
            }
            case 'x':
            case 'X':
            case 'o': {
                const int radix = conversion == 'o' ? 8 : 16;
                if (isBigInt(vm, value)) {
                    // A BigInt with its sign, as Java's BigInteger: -ff, +ff with the flag.
                    body = bigIntText(vm, value, radix);
                    const bool negative = !body.empty() && body[0] == '-';
                    if (negative) body.erase(0, 1);
                    sign = negative ? "-" : plus ? "+" : space ? " " : "";
                } else {
                    // An int as its 64 bits, unsigned, as Java's long.
                    if (!value.isInt()) throw vm.runtimeError(Thrown::IllegalArgumentException, std::string("format: %") + conversion + " takes a whole number, not " + vm.stringify(value));
                    char buffer[32];
                    auto result = std::to_chars(buffer, buffer + sizeof(buffer), static_cast<uint64_t>(value.as.i), radix);
                    body.assign(buffer, result.ptr);
                }
                if (conversion == 'X')
                    for (char& c : body) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                // The alternate form's 0x, 0X or 0 goes after the sign, before any zeros put in.
                if (alternate) sign += conversion == 'o' ? "0" : conversion == 'X' ? "0X" : "0x";
                numeric = true;
                break;
            }
            case 'f':
            case 'e':
            case 'E':
            case 'g':
            case 'G': {
                if (!value.isNumber()) throw vm.runtimeError(Thrown::IllegalArgumentException, std::string("format: %") + conversion + " takes a number, not " + vm.stringify(value));
                const double d = value.toDouble();
                sign = std::signbit(d) && !std::isnan(d) ? "-" : plus ? "+" : space ? " " : "";
                if (std::isnan(d) || std::isinf(d)) {
                    body = std::isnan(d) ? "NaN" : "Infinity";
                    zero = false;
                    break;
                }
                const int digits = precision < 0 ? 6 : std::min(precision, 340);
                bool fixed = conversion == 'f';
                if (conversion == 'f') body = exactFixed(std::fabs(d), digits);
                else if (conversion == 'e' || conversion == 'E') body = exactScientific(std::fabs(d), digits, conversion == 'E');
                else body = exactGeneral(std::fabs(d), digits, conversion == 'G', fixed);
                if (comma && fixed) {
                    const size_t point = body.find('.');
                    body = grouped(body.substr(0, point)) + (point == std::string::npos ? "" : body.substr(point));
                }
                // The alternate form always has a point: 3. and 3.e+00.
                if (alternate && body.find('.') == std::string::npos) {
                    const size_t e = body.find_first_of("eE");
                    body.insert(e == std::string::npos ? body.size() : e, ".");
                }
                numeric = true;
                break;
            }
            case 's':
            case 'S':
                body = vm.stringify(value);
                if (precision >= 0) body.resize(offsetOfCharacter(body, static_cast<size_t>(precision)));
                if (conversion == 'S') body = changeCase(body, true);
                break;
            case 'c':
                if (value.isChar()) {
                    appendUtf8(body, static_cast<uint32_t>(value.as.i));
                } else if (value.isInt()) {
                    if (value.as.i < 0 || value.as.i > 0x10FFFF) throw vm.runtimeError(Thrown::IllegalArgumentException, "format: %c takes a character or its code, not " + vm.stringify(value));
                    appendUtf8(body, static_cast<uint32_t>(value.as.i));
                } else if (value.isString()) {
                    body = asString(value)->utf8();
                } else {
                    throw vm.runtimeError(Thrown::IllegalArgumentException, "format: %c takes a character or its code, not " + vm.stringify(value));
                }
                break;
            case 'b':
                body = value.truthy() ? "true" : "false";
                break;
            default:
                throw vm.runtimeError(Thrown::IllegalArgumentException, std::string("format: '%") + conversion + "' is not a kind of item; use d, x, f, e, g, s, c or b, and %% for a percent sign");
        }

        const size_t length = sign.size() + countCharacters(body);
        const std::string padding(length < width ? width - length : 0, zero && numeric && !left ? '0' : ' ');
        if (left) out += sign + body + padding;
        else if (zero && numeric) out += sign + padding + body;
        else out += padding + sign + body;
    }
    return out;
}

// A char given where a string goes, as Kotlin's overloads taking a Char have it (D25): made
// the string of it alone, in its place on the stack, where the collector sees it.
void charAsText(VM& vm, Value& v) {
    if (v.isChar()) v = vm.characterString(static_cast<uint32_t>(v.as.i));
}

void registerStringMethods(VM& vm) {
    const BuiltinType T = BuiltinType::String;

    vm.defineMethod(T, "contains", 1, 2, [](VM& vm, Value self, int argc, Value* args) -> Value {
        if (args[0].isChar() && !flag(argc, args, 1))  // a char looked for where it is, no string made of it
            return Value::boolean(asString(self)->findChar(static_cast<uint32_t>(args[0].as.i), 0) >= 0);
        charAsText(vm, args[0]);
        const std::string& s = asString(self)->utf8();
        const std::string& other = vm.toStr(args[0], "search text");
        if (flag(argc, args, 1)) return Value::boolean(fold(s).find(fold(other)) != std::string::npos);
        return Value::boolean(s.find(other) != std::string::npos);
    });

    vm.defineMethod(T, "startsWith", 1, 2, [](VM& vm, Value self, int argc, Value* args) -> Value {
        charAsText(vm, args[0]);
        const std::string& prefix = vm.toStr(args[0], "prefix");
        if (flag(argc, args, 1)) {
            const std::string s = fold(asString(self)->utf8()), p = fold(prefix);
            return Value::boolean(s.compare(0, p.size(), p) == 0);
        }
        return Value::boolean(asString(self)->utf8().compare(0, prefix.size(), prefix) == 0);
    });

    vm.defineMethod(T, "endsWith", 1, 2, [](VM& vm, Value self, int argc, Value* args) -> Value {
        charAsText(vm, args[0]);
        const bool ignoreCase = flag(argc, args, 1);
        const std::string s = ignoreCase ? fold(asString(self)->utf8()) : asString(self)->utf8();
        const std::string suffix = ignoreCase ? fold(vm.toStr(args[0], "suffix")) : vm.toStr(args[0], "suffix");
        return Value::boolean(s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0);
    });

    // Where `other` first appears at or after `startIndex`, or -1.
    vm.defineMethod(T, "indexOf", 1, 3, [](VM& vm, Value self, int argc, Value* args) -> Value {
        const ObjString* text = asString(self);
        const size_t start = given(argc, args, 1) ? clampPosition(std::max<int64_t>(vm.toInt(args[1], "start index"), 0), text->length) : 0;
        // A char looked for where it is: before, a string was made of it for each call.
        if (args[0].isChar() && !flag(argc, args, 2)) return Value::integer(text->findChar(static_cast<uint32_t>(args[0].as.i), start));
        charAsText(vm, args[0]);
        const ObjString* part = vm.toText(args[0], "search text");
        if (flag(argc, args, 2)) {
            const std::string& other = part->utf8();
            // Changing case keeps the number of characters, so a position in one is a position in the other.
            const std::string s = fold(text->utf8());
            const size_t at = s.find(fold(other), offsetOfCharacter(s, start));
            return Value::integer(at == std::string::npos ? -1 : static_cast<int64_t>(countCharacters(std::string_view(s).substr(0, at))));
        }
        return Value::integer(text->find(part, start));
    });

    // Where `other` last appears, starting at or before `startIndex`, or -1.
    vm.defineMethod(T, "lastIndexOf", 1, 3, [](VM& vm, Value self, int argc, Value* args) -> Value {
        const ObjString* text = asString(self);
        int64_t start = given(argc, args, 1) ? vm.toInt(args[1], "start index") : static_cast<int64_t>(text->length);
        if (start < 0) return Value::integer(-1);
        const size_t from = std::min(static_cast<size_t>(start), text->length);
        if (args[0].isChar() && !flag(argc, args, 2)) return Value::integer(text->findChar(static_cast<uint32_t>(args[0].as.i), from, true));
        charAsText(vm, args[0]);
        const ObjString* part = vm.toText(args[0], "search text");
        if (flag(argc, args, 2)) {
            const std::string& other = part->utf8();
            const std::string s = fold(text->utf8());
            const size_t at = s.rfind(fold(other), offsetOfCharacter(s, from));
            return Value::integer(at == std::string::npos ? -1 : static_cast<int64_t>(countCharacters(std::string_view(s).substr(0, at))));
        }
        return Value::integer(text->find(part, from, true));
    });

    vm.defineMethod(T, "replace", 2, 3, [](VM& vm, Value self, int argc, Value* args) -> Value {
        charAsText(vm, args[0]);
        charAsText(vm, args[1]);
        const ObjString* text = asString(self);
        const std::string& s = text->utf8();
        const std::string& from = vm.toStr(args[0], "text to replace");
        const std::string& to = vm.toStr(args[1], "replacement");
        if (from.empty()) return self;

        std::string result;
        size_t begin = 0;
        if (flag(argc, args, 2)) {
            // Found in the text with its case folded, by character position, and cut out of the original.
            const std::string folded = fold(s), target = fold(from);
            const size_t targetLength = countCharacters(target);
            size_t searchFrom = 0, charsBefore = 0;
            for (;;) {
                const size_t at = folded.find(target, searchFrom);
                if (at == std::string::npos) break;
                charsBefore += countCharacters(std::string_view(folded).substr(searchFrom, at - searchFrom));
                const size_t cut = text->utf8Offset(charsBefore);
                result.append(s, begin, cut - begin);
                result += to;
                charsBefore += targetLength;
                begin = text->utf8Offset(charsBefore);
                searchFrom = at + target.size();
            }
        } else {
            for (;;) {
                const size_t at = s.find(from, begin);
                if (at == std::string::npos) break;
                result.append(s, begin, at - begin);
                result += to;
                begin = at + from.size();
            }
        }
        result.append(s, begin, std::string::npos);
        return vm.str(std::move(result));
    });

    vm.defineMethod(T, "equals", 1, 2, [](VM&, Value self, int argc, Value* args) -> Value {
        if (!args[0].isString()) return Value::boolean(false);
        if (flag(argc, args, 1)) return Value::boolean(fold(asString(self)->utf8()) == fold(asString(args[0])->utf8()));
        return Value::boolean(sameText(asString(self), asString(args[0])));
    });

    vm.defineMethod(T, "compareTo", 1, 2, [](VM& vm, Value self, int argc, Value* args) -> Value {
        const ObjString* other = vm.toText(args[0], "text to compare with");
        if (!flag(argc, args, 1)) return Value::integer(compareText(asString(self), other));
        const int order = fold(asString(self)->utf8()).compare(fold(other->utf8()));
        return Value::integer((order > 0) - (order < 0));
    });



    vm.defineMethod(T, "isEmpty", 0, 0, [](VM&, Value self, int, Value*) -> Value {
        return Value::boolean(asString(self)->utf8().empty());
    });
    vm.defineMethod(T, "isNotEmpty", 0, 0, [](VM&, Value self, int, Value*) -> Value {
        return Value::boolean(!asString(self)->utf8().empty());
    });
    vm.defineMethod(T, "isBlank", 0, 0, [](VM&, Value self, int, Value*) -> Value {
        return Value::boolean(trimmed(asString(self)->utf8()).empty());
    });
    vm.defineMethod(T, "isNotBlank", 0, 0, [](VM&, Value self, int, Value*) -> Value {
        return Value::boolean(!trimmed(asString(self)->utf8()).empty());
    });

    vm.defineMethod(T, "trimStart", 0, 0, [](VM& vm, Value self, int, Value*) -> Value {
        return vm.str(std::string(trimmedStart(asString(self)->utf8())));
    });
    vm.defineMethod(T, "trimEnd", 0, 0, [](VM& vm, Value self, int, Value*) -> Value {
        return vm.str(std::string(trimmedEnd(asString(self)->utf8())));
    });

    // The lines of the text, without their line ends.
    vm.defineMethod(T, "lines", 0, 0, [](VM& vm, Value self, int, Value*) -> Value {
        ObjList* list = newList(vm);
        TempRoot keep(vm, Value::object(list));
        for (std::string& line : splitLines(asString(self)->utf8())) list->values().push_back(vm.str(std::move(line)));
        vm.account(list->values().size() * sizeof(Value));
        return Value::object(list);
    });

    auto pad = [](VM& vm, Value self, int argc, Value* args, bool atStart) -> Value {
        const ObjString* text = asString(self);
        const int64_t length = vm.toInt(args[0], "padded length");
        if (given(argc, args, 1)) charAsText(vm, args[1]);  // padStart(5, ' ') (D25)
        const std::string with = given(argc, args, 1) ? vm.toStr(args[1], "padding character") : std::string(" ");
        if (countCharacters(with) != 1) throw vm.runtimeError(Thrown::IllegalArgumentException, "padding is done with a single character, not " + vm.stringify(args[1]));
        if (length <= static_cast<int64_t>(text->length)) return self;
        // As large as memory allows: the room is asked for at once, so a size too large fails at once.
        const uint64_t missing = static_cast<uint64_t>(length) - text->length;
        if (missing > SIZE_MAX / with.size()) throw vm.outOfMemory();
        vm.checkRoomFor(missing * with.size());
        std::string padding;
        padding.reserve(static_cast<size_t>(missing) * with.size());
        for (int64_t i = static_cast<int64_t>(text->length); i < length; i++) padding += with;
        return vm.str(atStart ? padding + text->utf8() : text->utf8() + padding);
    };
    static const auto padText = pad;
    vm.defineMethod(T, "padStart", 1, 2, [](VM& vm, Value self, int argc, Value* args) -> Value {
        return padText(vm, self, argc, args, true);
    });
    vm.defineMethod(T, "padEnd", 1, 2, [](VM& vm, Value self, int argc, Value* args) -> Value {
        return padText(vm, self, argc, args, false);
    });

    // The characters in the opposite order.
    vm.defineMethod(T, "reversed", 0, 0, [](VM& vm, Value self, int, Value*) -> Value {
        const ObjString* text = asString(self);
        Characters out{std::string(text->units().size(), '\0'), text->width};
        const size_t width = text->width;
        for (size_t i = 0; i < text->length; i++)
            std::memcpy(&out.units[i * width], text->units().data() + (text->length - 1 - i) * width, width);
        return vm.str(std::move(out));
    });

    // Text to number. toInt() and toDouble() are errors for text that is not one; the
    // ...OrNull forms give null instead.
    vm.defineMethod(T, "toInt", 0, 1, [](VM& vm, Value self, int argc, Value* args) -> Value {
        int64_t n;
        if (!parseWhole(asString(self)->utf8(), radixOf(vm, argc, args, 0), n))
            throw vm.runtimeError(Thrown::NumberFormatException, vm.stringify(self).insert(0, "\"") + "\" is not a whole number");
        return Value::integer(n);
    });
    vm.defineMethod(T, "toIntOrNull", 0, 1, [](VM& vm, Value self, int argc, Value* args) -> Value {
        int64_t n;
        return parseWhole(asString(self)->utf8(), radixOf(vm, argc, args, 0), n) ? Value::integer(n) : Value::null();
    });
    // Text to an int's 64 bits read as unsigned (D44): "18446744073709551615" is -1.
    vm.defineMethod(T, "parseUnsigned", 0, 1, [](VM& vm, Value self, int argc, Value* args) -> Value {
        std::string_view text = asString(self)->utf8();
        if (!text.empty() && text[0] == '+') text.remove_prefix(1);
        uint64_t n = 0;
        auto result = std::from_chars(text.data(), text.data() + text.size(), n, radixOf(vm, argc, args, 0));
        if (text.empty() || result.ec != std::errc() || result.ptr != text.data() + text.size())
            throw vm.runtimeError(Thrown::NumberFormatException, vm.stringify(self).insert(0, "\"") + "\" is not a whole number from 0 to 18446744073709551615");
        return Value::integer(static_cast<int64_t>(n));
    });
    vm.defineMethod(T, "toFloat", 0, 0, [](VM& vm, Value self, int, Value*) -> Value {
        double d;
        if (!parseFloat(asString(self)->utf8(), d)) throw vm.runtimeError(Thrown::NumberFormatException, vm.stringify(self).insert(0, "\"") + "\" is not a number");
        return Value::number(d);
    });
    vm.defineMethod(T, "toFloatOrNull", 0, 0, [](VM&, Value self, int, Value*) -> Value {
        double d;
        return parseFloat(asString(self)->utf8(), d) ? Value::number(d) : Value::null();
    });

    vm.defineMethod(T, "format", 0, -1, [](VM& vm, Value self, int argc, Value* args) -> Value {
        return vm.str(formatText(vm, asString(self)->utf8(), argc, args));
    });

#define WW_CHARACTER_TEST(NAME, WHICH)                                              \
    vm.defineMethod(T, NAME, 0, 0, [](VM&, Value self, int, Value*) -> Value {          \
        return Value::boolean(charactersAre(asString(self)->utf8(), CharacterClass::WHICH)); \
    })
    WW_CHARACTER_TEST("isAlpha", Letter);
    WW_CHARACTER_TEST("isDigit", Digit);
    WW_CHARACTER_TEST("isAlnum", LetterOrDigit);
    WW_CHARACTER_TEST("isSpace", Whitespace);
#undef WW_CHARACTER_TEST
    vm.defineMethod(T, "isUpper", 0, 0, [](VM&, Value self, int, Value*) -> Value { return Value::boolean(casedAre(asString(self)->utf8(), true)); });
    vm.defineMethod(T, "isLower", 0, 0, [](VM&, Value self, int, Value*) -> Value { return Value::boolean(casedAre(asString(self)->utf8(), false)); });
}

// ---- Numbers ----

void registerNumberMethods(VM& vm) {
    const BuiltinType T = BuiltinType::Number;

    // The number itself if it lies from `minimum` to `maximum`, and otherwise the nearer of the two.
    vm.defineMethod(T, "coerceIn", 1, 2, [](VM& vm, Value self, int argc, Value* args) -> Value {
        Value low = args[0], high = argc > 1 ? args[1] : Value::null();
        if (argc == 1 && low.isObjType(ObjType::Range)) {
            const ObjRange* range = asRange(low);
            if (range->count() == 0) throw vm.runtimeError(Thrown::IllegalArgumentException, "coerceIn: the range is empty");
            const int64_t first = range->start, last = range->start + (range->count() - 1) * range->step;
            low = Value::integer(std::min(first, last));
            high = Value::integer(std::max(first, last));
        }
        if (!low.isNumber() || !high.isNumber()) throw vm.runtimeError(Thrown::IllegalArgumentException, "coerceIn takes a minimum and a maximum, or a range");
        if (compareNumbers(low, high) > 0)
            throw vm.runtimeError(Thrown::IllegalArgumentException, "coerceIn: the maximum " + vm.stringify(high) + " is less than the minimum " + vm.stringify(low));
        if (compareNumbers(self, low) < 0) return low;
        if (compareNumbers(self, high) > 0) return high;
        return self;
    });

    vm.defineMethod(T, "coerceAtLeast", 1, 1, [](VM& vm, Value self, int, Value* args) -> Value {
        vm.toNumber(args[0], "minimum");
        return compareNumbers(self, args[0]) < 0 ? args[0] : self;
    });
    vm.defineMethod(T, "coerceAtMost", 1, 1, [](VM& vm, Value self, int, Value* args) -> Value {
        vm.toNumber(args[0], "maximum");
        return compareNumbers(self, args[0]) > 0 ? args[0] : self;
    });

    // To the nearest whole number; a half goes up, as in Kotlin.
    auto round = [](VM& vm, Value self, int, Value*) -> Value {
        if (self.isInt()) return self;
        if (std::isnan(self.as.d)) throw vm.runtimeError(Thrown::ArithmeticException, "cannot round NaN to a whole number");
        return Value::integer(floatToInt(vm, std::floor(self.as.d + 0.5)));
    };
    vm.defineMethod(T, "roundToInt", 0, 0, round);

    // The whole part: what is after the point is dropped.
    auto whole = [](VM& vm, Value self, int, Value*) -> Value {
        return self.isInt() ? self : Value::integer(floatToInt(vm, std::trunc(self.as.d)));
    };
    vm.defineMethod(T, "toInt", 0, 0, whole);

    auto fraction = [](VM&, Value self, int, Value*) -> Value { return Value::number(self.toDouble()); };
    vm.defineMethod(T, "toFloat", 0, 0, fraction);

    // An int as a byte (D19): an error past 0 to 255; toByteWrapped() keeps the low 8 bits.
    vm.defineMethod(T, "toByte", 0, 0, [](VM& vm, Value self, int, Value*) -> Value {
        const int64_t n = static_cast<int64_t>(wholeOnly(vm, self, "toByte"));
        if (n < 0 || n > 255)
            throw vm.runtimeError(Thrown::IllegalArgumentException, std::to_string(n) + " does not fit in a byte, which holds 0 to 255; toByteWrapped() keeps the low 8 bits");
        return Value::byte(static_cast<uint8_t>(n));
    });
    vm.defineMethod(T, "toByteWrapped", 0, 0, [](VM& vm, Value self, int, Value*) -> Value {
        return Value::byte(static_cast<uint8_t>(wholeOnly(vm, self, "toByteWrapped")));
    });
    // The low bits of an int, as the storage widths keep them (D148), read back as an int.
    vm.defineMethod(T, "toInt16Wrapped", 0, 0, [](VM& vm, Value self, int, Value*) -> Value {
        return Value::integer(static_cast<int16_t>(wholeOnly(vm, self, "toInt16Wrapped")));
    });
    vm.defineMethod(T, "toUint16Wrapped", 0, 0, [](VM& vm, Value self, int, Value*) -> Value {
        return Value::integer(static_cast<uint16_t>(wholeOnly(vm, self, "toUint16Wrapped")));
    });
    vm.defineMethod(T, "toInt32Wrapped", 0, 0, [](VM& vm, Value self, int, Value*) -> Value {
        return Value::integer(static_cast<int32_t>(wholeOnly(vm, self, "toInt32Wrapped")));
    });
    vm.defineMethod(T, "toUint32Wrapped", 0, 0, [](VM& vm, Value self, int, Value*) -> Value {
        return Value::integer(static_cast<uint32_t>(wholeOnly(vm, self, "toUint32Wrapped")));
    });
    // For 32-bit work ported from Kotlin (D148): the low 32 bits as a signed number, and
    // rotations of them, each giving a signed 32-bit number as Kotlin's Int does.
    vm.defineMethod(T, "rotateLeft32", 1, 1, [](VM& vm, Value self, int, Value* args) -> Value {
        const uint32_t bits = static_cast<uint32_t>(wholeOnly(vm, self, "rotateLeft32"));
        const unsigned count = static_cast<unsigned>(vm.toInt(args[0], "rotation") & 31);
        return Value::integer(static_cast<int32_t>(count == 0 ? bits : (bits << count) | (bits >> (32 - count))));
    });
    vm.defineMethod(T, "rotateRight32", 1, 1, [](VM& vm, Value self, int, Value* args) -> Value {
        const uint32_t bits = static_cast<uint32_t>(wholeOnly(vm, self, "rotateRight32"));
        const unsigned count = static_cast<unsigned>(vm.toInt(args[0], "rotation") & 31);
        return Value::integer(static_cast<int32_t>(count == 0 ? bits : (bits >> count) | (bits << (32 - count))));
    });

    // An int's 64 bits read as unsigned (D44): there are no unsigned types, but these.
    vm.defineMethod(T, "toUnsignedString", 0, 1, [](VM& vm, Value self, int argc, Value* args) -> Value {
        const uint64_t n = wholeOnly(vm, self, "toUnsignedString");
        char buffer[72];
        auto result = std::to_chars(buffer, buffer + sizeof(buffer), n, radixOf(vm, argc, args, 0));
        return vm.str(std::string(buffer, result.ptr));
    });
    vm.defineMethod(T, "compareUnsigned", 1, 1, [](VM& vm, Value self, int, Value* args) -> Value {
        const uint64_t a = wholeOnly(vm, self, "compareUnsigned"), b = wholeOnly(vm, args[0], "compareUnsigned");
        return Value::integer(a < b ? -1 : a > b ? 1 : 0);
    });
    vm.defineMethod(T, "divideUnsigned", 1, 1, [](VM& vm, Value self, int, Value* args) -> Value {
        const uint64_t a = wholeOnly(vm, self, "divideUnsigned"), b = wholeOnly(vm, args[0], "divideUnsigned");
        if (b == 0) throw vm.runtimeError(Thrown::ArithmeticException, "division by zero");
        return Value::integer(static_cast<int64_t>(a / b));
    });
    vm.defineMethod(T, "remainderUnsigned", 1, 1, [](VM& vm, Value self, int, Value* args) -> Value {
        const uint64_t a = wholeOnly(vm, self, "remainderUnsigned"), b = wholeOnly(vm, args[0], "remainderUnsigned");
        if (b == 0) throw vm.runtimeError(Thrown::ArithmeticException, "division by zero");
        return Value::integer(static_cast<int64_t>(a % b));
    });

    // toString(), or toString(radix) for a whole number in another base.
    vm.defineMethod(T, "toString", 0, 1, [](VM& vm, Value self, int argc, Value* args) -> Value {
        if (!given(argc, args, 0)) return vm.str(vm.stringify(self));
        const int radix = radixOf(vm, argc, args, 0);
        if (!self.isInt()) throw vm.runtimeError(Thrown::ClassCastException, "toString(radix) is for whole numbers, not " + vm.stringify(self));
        const uint64_t magnitude = self.as.i < 0 ? uint64_t{0} - static_cast<uint64_t>(self.as.i) : static_cast<uint64_t>(self.as.i);
        char buffer[72];
        auto result = std::to_chars(buffer, buffer + sizeof(buffer), magnitude, radix);
        return vm.str((self.as.i < 0 ? "-" : "") + std::string(buffer, result.ptr));
    });

    vm.defineMethod(T, "pow", 1, 1, [](VM& vm, Value self, int, Value* args) -> Value {
        if (self.isInt() && args[0].isInt() && args[0].as.i >= 0) return Value::integer(integerPower(vm, self.as.i, args[0].as.i));
        return Value::number(fmath::pow(self.toDouble(), vm.toNumber(args[0], "exponent")));
    });

    // The remainder that is never negative for a positive divisor: (-1).mod(5) is 4.
    // A float's 64 bits as an int, every NaN the one quiet NaN (Kotlin's toBits; A2).
    vm.defineMethod(T, "toBits", 0, 0, [](VM&, Value self, int, Value*) -> Value {
        return Value::integer(static_cast<int64_t>(fmath::canonicalBits(self.toDouble())));
    });

    // The largest whole number at or below a float, and the smallest at or above, as floats (A3):
    // NaN, infinities and -0.0 as they are, numbers past 2^63 too.
    vm.defineMethod(T, "floor", 0, 0, [](VM&, Value self, int, Value*) -> Value { return Value::number(std::floor(self.toDouble())); });
    vm.defineMethod(T, "ceil", 0, 0, [](VM&, Value self, int, Value*) -> Value { return Value::number(std::ceil(self.toDouble())); });

    vm.defineMethod(T, "mod", 1, 1, [](VM& vm, Value self, int, Value* args) -> Value {
        if (self.isInt() && args[0].isInt()) {
            const int64_t divisor = args[0].as.i;
            if (divisor == 0) throw vm.runtimeError(Thrown::ArithmeticException, "division by zero");
            if (divisor == -1) return Value::integer(0);
            const int64_t remainder = self.as.i % divisor;
            return Value::integer(remainder != 0 && ((remainder < 0) != (divisor < 0)) ? remainder + divisor : remainder);
        }
        const double divisor = vm.toNumber(args[0], "divisor");
        const double remainder = std::fmod(self.toDouble(), divisor);
        return Value::number(remainder != 0 && ((remainder < 0) != (divisor < 0)) ? remainder + divisor : remainder);
    });

    vm.defineMethod(T, "compareTo", 1, 1, [](VM& vm, Value self, int, Value* args) -> Value {
        vm.toNumber(args[0], "number to compare with");
        return Value::integer(compareNumbers(self, args[0]));
    });

    vm.defineMethod(T, "isNaN", 0, 0, [](VM&, Value self, int, Value*) -> Value {
        return Value::boolean(self.isFloat() && std::isnan(self.as.d));
    });
    vm.defineMethod(T, "isInfinite", 0, 0, [](VM&, Value self, int, Value*) -> Value {
        return Value::boolean(self.isFloat() && std::isinf(self.as.d));
    });
    vm.defineMethod(T, "isFinite", 0, 0, [](VM&, Value self, int, Value*) -> Value {
        return Value::boolean(self.isInt() || std::isfinite(self.as.d));
    });
}

// A char's methods (D24): what kind of character it is, and the same letter in the other case.
void registerCharMethods(VM& vm) {
    constexpr BuiltinType T = BuiltinType::Char;
#define CHARACTER_CLASS(NAME, WHICH)                                                                          \
    vm.defineMethod(T, NAME, 0, 0, [](VM&, Value self, int, Value*) -> Value {                                 \
        std::string text;                                                                                     \
        appendUtf8(text, static_cast<uint32_t>(self.as.i));                                                   \
        return Value::boolean(charactersAre(text, CharacterClass::WHICH));                                    \
    });
    CHARACTER_CLASS("isLetter", Letter)
    CHARACTER_CLASS("isDigit", Digit)
    CHARACTER_CLASS("isLetterOrDigit", LetterOrDigit)
    CHARACTER_CLASS("isWhitespace", Whitespace)
    CHARACTER_CLASS("isUpperCase", Upper)
    CHARACTER_CLASS("isLowerCase", Lower)
#undef CHARACTER_CLASS
    // The same letter in the other case, as Java's Character.toUpperCase and toLowerCase give it: one
    // character for one, the char itself where it has none.
    vm.defineMethod(T, "upper", 0, 0, [](VM&, Value self, int, Value*) -> Value {
        return Value::character(unicode::toUpper(static_cast<uint32_t>(self.as.i)));
    });
    vm.defineMethod(T, "lower", 0, 0, [](VM&, Value self, int, Value*) -> Value {
        return Value::character(unicode::toLower(static_cast<uint32_t>(self.as.i)));
    });
    // The number a digit stands for: '7' is 7, and with a radix past 10, 'f' is 15. As Java's
    // Character.digit (Kotlin's digitToInt): a decimal digit of any script ('٣' is 3), and the
    // letters, Latin and fullwidth, past 9 (W4 of docs/snippy-ww-checklist.html).
    vm.defineMethod(T, "digitToInt", 0, 1, [](VM& vm, Value self, int argc, Value* args) -> Value {
        const int radix = radixOf(vm, argc, args, 0);
        const int64_t c = self.as.i;
        auto digitOf = [](int64_t c) -> int64_t {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'z') return c - 'a' + 10;
            if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
            if (c >= 0xFF41 && c <= 0xFF5A) return c - 0xFF41 + 10;  // fullwidth a to z
            if (c >= 0xFF21 && c <= 0xFF3A) return c - 0xFF21 + 10;  // fullwidth A to Z
            if (c < 0x80 || c > 0x10FFFF) return 99;
            const int value = unicode::decimalValue(static_cast<uint32_t>(c));   // UnicodeData's decimal digit
            return value < 0 ? 99 : value;
        };
        const int64_t digit = digitOf(c);
        if (digit >= radix) throw vm.runtimeError(Thrown::NumberFormatException, quoteCharacter(static_cast<uint32_t>(c)) + " is not a digit" + (radix == 10 ? std::string() : " in base " + std::to_string(radix)));
        return Value::integer(digit);
    });
}

// An Index's (D146): the position it means in `size` items, ^k being size - k. An int is one
// too, a position from the start, so of() is on ints as well, for an Index that holds one.
void registerIndexMethods(VM& vm) {
    vm.defineMethod(BuiltinType::Index, "of", 1, 1, [](VM& vm, Value self, int, Value* args) -> Value {
        return Value::integer(vm.toInt(args[0], "size") - self.as.i);
    });
    vm.defineMethod(BuiltinType::Number, "of", 1, 1, [](VM& vm, Value self, int, Value*) -> Value {
        return Value::integer(vm.toInt(self, "position"));
    });
    // A range's bounds in `size` items: one with an end counted from the end made plain.
    vm.defineMethod(BuiltinType::Range, "of", 1, 1, [](VM& vm, Value self, int, Value* args) -> Value {
        const ObjRange* range = asRange(self);
        const int64_t size = vm.toInt(args[0], "size");
        return Value::object(vm.alloc<ObjRange>(range->startFromEnd ? size - range->start : range->start,
                                                range->endFromEnd ? size - range->end : range->end, range->step, range->inclusive));
    });
}

// `10 downTo 1` and `range step 2` (D72): infix functions of the standard library, which the
// compiler writes in place where it sees them, as it does a range written with `..`.
void registerRangeWords(VM& vm) {
    vm.defineMethod(BuiltinType::Number, "downTo", 1, 1, [](VM& vm, Value self, int, Value* args) -> Value {
        return Value::object(vm.alloc<ObjRange>(vm.toInt(self, "start"), vm.toInt(args[0], "end"), -1, true));
    });
    vm.defineMethod(BuiltinType::Range, "step", 1, 1, [](VM& vm, Value self, int, Value* args) -> Value {
        const ObjRange* range = asRange(self);
        const int64_t by = vm.toInt(args[0], "step");
        if (by <= 0) throw vm.runtimeError(Thrown::IllegalArgumentException, "the step of a range must be a positive int");
        return Value::object(vm.alloc<ObjRange>(range->start, range->end, range->step < 0 ? -by : by, range->inclusive));
    });
}

// A byte's methods (D19): the int it holds.
void registerByteMethods(VM& vm) {
    vm.defineMethod(BuiltinType::Byte, "toInt", 0, 0, [](VM&, Value self, int, Value*) -> Value { return Value::integer(self.as.i); });
}

// Int.maxValue and the like. WinterWolf has one whole-number type and one for fractions; the
// four names are Kotlin's, so that a limit means the same number as it does there.
void registerNumberConstants(VM&) {
    // WinterWolf's are `int.maxValue` and the like, which the compiler writes in place (D76); Kotlin's
    // holders, Int, Long, Double and Float, are errors saying so.
}

// ---- Lists ----

// A position in a list for get, set and subList: unlike list[i], it does not count from the
// end, so that a position before the start is told apart from the last item.
bool exactIndex(Value index, size_t size, size_t& out) {
    if (!index.isInt() || index.as.i < 0 || static_cast<uint64_t>(index.as.i) >= size) return false;
    out = static_cast<size_t>(index.as.i);
    return true;
}

WwThrow outOfRange(VM& vm, const char* what, Value index, size_t size) {
    return vm.runtimeError(Thrown::IndexOutOfBoundsException, std::string(what) + ": index " + vm.stringify(index) + " is out of range (size " + std::to_string(size) + ")");
}

// Sorts positions [lo, hi) by `less`, stably: merge sort, so that what `less` does cannot
// leave the sort in a state std::sort does not allow.
template <typename Less>
void sortPositions(std::vector<size_t>& order, std::vector<size_t>& scratch, size_t lo, size_t hi, const Less& less) {
    if (hi - lo < 2) return;
    const size_t mid = lo + (hi - lo) / 2;
    sortPositions(order, scratch, lo, mid, less);
    sortPositions(order, scratch, mid, hi, less);
    size_t i = lo, j = mid, k = lo;
    while (i < mid && j < hi) scratch[k++] = less(order[j], order[i]) ? order[j++] : order[i++];
    while (i < mid) scratch[k++] = order[i++];
    while (j < hi) scratch[k++] = order[j++];
    std::copy(scratch.begin() + static_cast<std::ptrdiff_t>(lo), scratch.begin() + static_cast<std::ptrdiff_t>(hi),
              order.begin() + static_cast<std::ptrdiff_t>(lo));
}

// Item `i` of a list as a function given it is handed it: an item of a list of values where it is,
// a Ref, which the function copies itself if it may change the list while it runs (M6.6 part 4).
Value itemWhereItIs(VM& vm, ObjList* list, size_t i) {
    if (list->store() == ListStore::Items && !(list->pack->nullable && (list->pack->nulls[i / 64] >> (i % 64) & 1)))
        return Value::ref(list, static_cast<uint32_t>(i), 0, 0);
    return vm.listGet(list, i);
}

// sortBy { key } and sortByDescending { key } (M6.6 part 3, value-lists.html section 9): each
// item's key is worked out once, the positions are sorted by the keys, and then each item is
// moved once, to where it goes, however large it is and however many comparisons there are.
Value sortByKey(VM& vm, Value self, Value selector, bool descending) {
    ObjList* list = asList(self);
    const size_t n = list->size();
    std::vector<Value> keys;
    keys.reserve(n);
    TempVector keepKeys(vm, keys);
    for (size_t i = 0; i < n && i < list->size(); i++) {
        Value item = itemWhereItIs(vm, list, i);  // M6.6 part 4: no copy of a value to work out its key
        TempRoot keepItem(vm, item);
        keys.push_back(vm.call(selector, 1, &item));
    }
    if (list->size() != n) throw vm.runtimeError(Thrown::ConcurrentModificationException, "the list was changed while it was sorted");
    std::vector<size_t> order(n), scratch(n);
    for (size_t i = 0; i < n; i++) order[i] = i;
    bool ints = true;
    for (const Value& key : keys) ints = ints && key.isInt();
    if (ints) {
        sortPositions(order, scratch, 0, n, [&](size_t a, size_t b) {
            return descending ? keys[b].as.i < keys[a].as.i : keys[a].as.i < keys[b].as.i;
        });
    } else {
        sortPositions(order, scratch, 0, n, [&](size_t a, size_t b) {
            return descending ? compareValues(vm, keys[b], keys[a]) < 0 : compareValues(vm, keys[a], keys[b]) < 0;
        });
    }
    vm.listArrange(list, order);
    return Value::null();
}

void registerListMethods(VM& vm) {
    const BuiltinType T = BuiltinType::List;

    vm.defineMethod(T, "sortBy", 1, 1, [](VM& vm, Value self, int, Value* args) -> Value { return sortByKey(vm, self, args[0], false); });

    // replaceAll { it.moved() } (value-lists.html section 9): every item replaced by what the
    // function gives for it. An item of a list of values is handed over where it is, and is
    // where the result goes: a value made from its header alone is built there (section 7).
    vm.defineMethod(T, "replaceAll", 1, 1, [](VM& vm, Value self, int, Value* args) -> Value {
        ObjList* list = asList(self);
        const Value transform = args[0];
        for (size_t i = 0; i < list->size(); i++) {
            Value item = itemWhereItIs(vm, list, i);
            TempRoot keepItem(vm, item);
            if (item.isRef()) vm.pendingDestination_ = item;
            const Value made = vm.call(transform, 1, &item);
            vm.pendingDestination_ = Value::null();
            TempRoot keepMade(vm, made);
            if (i >= list->size()) throw vm.runtimeError(Thrown::ConcurrentModificationException, "the list was made shorter while its items were replaced");
            vm.listSet(list, i, made);
        }
        return Value::null();
    });
    vm.defineMethod(T, "sortByDescending", 1, 1, [](VM& vm, Value self, int, Value* args) -> Value {
        return sortByKey(vm, self, args[0], true);
    });

    // add(item) at the end, or add(index, item) before the item at `index`.
    vm.defineMethod(T, "add", 1, 2, [](VM& vm, Value self, int argc, Value* args) -> Value {
        ObjList* list = asList(self);
        if (argc == 1) {
            vm.listAdd(list, args[0]);  // at the end, where nothing moves: a Ref is read as it is copied
        } else {
            args[1] = vm.boxed(args[1]);  // copied before the items after it move
            const int64_t at = vm.toInt(args[0], "add position");
            if (at < 0 || static_cast<uint64_t>(at) > list->size()) throw vm.outOfRange("add position", args[0], list->size());
            vm.listInsert(list, static_cast<size_t>(at), &args[1], 1);
        }
        return Value::boolean(true);
    });

    // ensureCapacity(capacity): room for that many items in all, so that adding up to that many
    // makes no room as it goes; nothing added (2026-11-15: a list built by adding, copied at once
    // by native code where it has room).
    vm.defineMethod(T, "ensureCapacity", 1, 1, [](VM& vm, Value self, int, Value* args) -> Value {
        const int64_t n = vm.toInt(args[0], "capacity");
        if (n < 0) throw vm.runtimeError(Thrown::IllegalArgumentException, "a capacity of " + std::to_string(n) + ": it cannot be negative");
        if (n > (int64_t{1} << 40)) throw vm.runtimeError(Thrown::IllegalArgumentException, "a capacity of " + std::to_string(n) + " items is more than a list can hold");
        ObjList* list = asList(self);
        vm.checkRoomFor(static_cast<uint64_t>(n) * (list->packed() ? list->pack->width : sizeof(Value)));
        vm.listReserve(list, static_cast<size_t>(n));
        return Value::null();
    });

    // addAll(items) at the end, or addAll(index, items). `items` is anything a for loop takes.
    vm.defineMethod(T, "addAll", 1, 2, [](VM& vm, Value self, int argc, Value* args) -> Value {
        ObjList* list = asList(self);
        size_t at = list->size();
        if (argc == 2) {
            if (!args[0].isInt() || args[0].as.i < 0 || static_cast<uint64_t>(args[0].as.i) > list->size())
                throw outOfRange(vm, "addAll", args[0], list->size());
            at = static_cast<size_t>(args[0].as.i);
        }
        std::vector<Value> added;
        vm.collect(args[argc - 1], added);
        at = std::min(at, list->size());  // an iterator() may have changed the list
        TempVector keep(vm, added);
        vm.listInsert(list, at, added.data(), added.size());
        return Value::boolean(!added.empty());
    });

    vm.defineMethod(T, "isEmpty", 0, 0, [](VM&, Value self, int, Value*) -> Value {
        return Value::boolean(asList(self)->size() == 0);
    });
    vm.defineMethod(T, "isNotEmpty", 0, 0, [](VM&, Value self, int, Value*) -> Value {
        return Value::boolean(asList(self)->size() != 0);
    });

    vm.defineMethod(T, "get", 1, 1, [](VM& vm, Value self, int, Value* args) -> Value {
        ObjList* list = asList(self);
        size_t i;
        if (!exactIndex(args[0], list->size(), i)) throw outOfRange(vm, "get", args[0], list->size());
        return vm.listGet(list, i);
    });
    vm.defineMethod(T, "getOrNull", 1, 1, [](VM& vm, Value self, int, Value* args) -> Value {
        ObjList* list = asList(self);
        size_t i;
        return exactIndex(args[0], list->size(), i) ? vm.listGet(list, i) : Value::null();
    });
    // Gives the item that was there.
    vm.defineMethod(T, "set", 2, 2, [](VM& vm, Value self, int, Value* args) -> Value {
        ObjList* list = asList(self);
        size_t i;
        if (!exactIndex(args[0], list->size(), i)) throw outOfRange(vm, "set", args[0], list->size());
        const Value before = vm.listGet(list, i);
        vm.listSet(list, i, args[1]);
        return before;
    });

    auto removeEnd = [](VM& vm, Value self, bool first, const char* name, bool orNull) -> Value {
        ObjList* list = asList(self);
        if (list->size() == 0) {
            if (orNull) return Value::null();
            throw vm.runtimeError(Thrown::NoSuchElementException, std::string(name) + "() from an empty list");
        }
        const size_t at = first ? 0 : list->size() - 1;
        const Value removed = vm.listGet(list, at);
        vm.listErase(list, at, 1);
        return removed;
    };
    static const auto removeAtEnd = removeEnd;
    vm.defineMethod(T, "removeFirst", 0, 0, [](VM& vm, Value self, int, Value*) -> Value {
        return removeAtEnd(vm, self, true, "removeFirst", false);
    });
    vm.defineMethod(T, "removeLast", 0, 0, [](VM& vm, Value self, int, Value*) -> Value {
        return removeAtEnd(vm, self, false, "removeLast", false);
    });
    vm.defineMethod(T, "removeFirstOrNull", 0, 0, [](VM& vm, Value self, int, Value*) -> Value {
        return removeAtEnd(vm, self, true, "removeFirstOrNull", true);
    });
    vm.defineMethod(T, "removeLastOrNull", 0, 0, [](VM& vm, Value self, int, Value*) -> Value {
        return removeAtEnd(vm, self, false, "removeLastOrNull", true);
    });

    // A new list holding the same items, of the same type and kept the same way.
    auto copy = [](VM& vm, Value self, int, Value*) -> Value {
        ObjList* list = asList(self);
        if (!list->packed()) return Value::object(newList(vm, list->values()));
        ObjList* made = vm.listLike(list);
        vm.listCopyRange(made, list, 0, list->size());
        return Value::object(made);
    };
    vm.defineMethod(T, "toList", 0, 0, copy);
    vm.defineMethod(T, "toOpenList", 0, 0, copy);
    // A copy nobody may change, which can be a key (D26).
    vm.defineMethod(T, "frozen", 0, 0, [](VM& vm, Value self, int, Value*) -> Value { return vm.frozenCopy(self); });


    // A new list in the opposite order; reverse() turns the list itself round.
    vm.defineMethod(T, "reversed", 0, 0, [](VM& vm, Value self, int, Value*) -> Value {
        ObjList* list = asList(self);
        ObjList* made = list->packed() ? vm.listLike(list) : newList(vm);
        vm.listCopyRange(made, list, 0, list->size());
        for (size_t i = 0, j = made->size(); i + 1 < j; i++, j--) vm.listSwap(made, i, j - 1);
        return Value::object(made);
    });

    vm.defineMethod(T, "lastIndexOf", 1, 1, [](VM& vm, Value self, int, Value* args) -> Value {
        ObjList* list = asList(self);
        for (size_t i = list->size(); i > 0; i--) {
            if (i > list->size()) continue;  // an == operator may have shortened the list
            if (vm.equals(vm.listGet(list, i - 1), args[0])) return Value::integer(static_cast<int64_t>(i - 1));
        }
        return Value::integer(-1);
    });

    // fill(item, from = 0, to = size): -1 for `to` is the end (the declaration's default).
    vm.defineMethod(T, "fill", 1, 3, [](VM& vm, Value self, int argc, Value* args) -> Value {
        ObjList* list = asList(self);
        const int64_t size = static_cast<int64_t>(list->size());
        const int64_t from = argc > 1 ? vm.toInt(args[1], "from") : 0;
        const int64_t to = argc > 2 && vm.toInt(args[2], "to") >= 0 ? vm.toInt(args[2], "to") : size;
        if (from < 0 || from > to || to > size)
            throw vm.runtimeError(Thrown::IndexOutOfBoundsException, "fill from " + std::to_string(from) + " to " + std::to_string(to) + " is not inside the " + std::to_string(size) + " items");
        if (to == from) return Value::null();
        vm.listSet(list, static_cast<size_t>(from), args[0]);
        if (list->packed() && !list->pack->nullable) {
            // The first written as the list keeps it, then copied: a picture's row filled at once.
            const size_t w = list->pack->width;
            uint8_t* first = vm.itemAt(list, static_cast<size_t>(from));
            for (int64_t i = from + 1; i < to; i++) std::memcpy(vm.itemAt(list, static_cast<size_t>(i)), first, w);
            return Value::null();
        }
        for (int64_t i = from + 1; i < to; i++) vm.listSet(list, static_cast<size_t>(i), args[0]);
        return Value::null();
    });
}

// ---- Maps ----

// A set's own (M6.6 part 8); what a set does besides is in set.ww. A view of a map's keys
// or entries (D77) takes all that only reads.
void registerSetMethods(VM& vm) {
    const BuiltinType T = BuiltinType::Set;
    vm.defineMethod(T, "frozen", 0, 0, [](VM& vm, Value self, int, Value*) -> Value { return vm.frozenCopy(self); });
    // Set(), or Set(from): the items of a list, a range, another set, a string or a map.
    vm.defineNative("Set", 0, 1, [](VM& vm, int argc, Value* args) -> Value {
        // Made from a list, a set or a map's values that know what they hold, it holds the same (G25).
        ObjTypeInfo* item = nullptr;
        if (argc > 0 && args[0].isObjType(ObjType::List) && asList(args[0])->type != nullptr && !asList(args[0])->type->args.empty())
            item = asList(args[0])->type->args[0];
        else if (argc > 0 && args[0].isObjType(ObjType::Set) && asSet(args[0])->type != nullptr && !asSet(args[0])->type->args.empty() && !asSet(args[0])->entries)
            item = asSet(args[0])->type->args[0];
        ObjSet* set = vm.newSet(item != nullptr ? vm.typeOf(kTypeSet, false, nullptr, {item}) : nullptr);
        TempRoot keep(vm, Value::object(set));
        if (argc > 0 && !args[0].isNull())
            for (const Value& value : vm.itemsOf(args[0], "Set()")) vm.setAdd(set, value);
        return Value::object(set);
    });
    vm.defineMethod(T, "add", 1, 1, [](VM& vm, Value self, int, Value* args) -> Value {
        return Value::boolean(vm.setAdd(asSet(self), args[0]));  // true if it was not there
    });
    vm.defineMethod(T, "remove", 1, 1, [](VM& vm, Value self, int, Value* args) -> Value {
        return Value::boolean(vm.setRemove(asSet(self), args[0]));  // true if it was there
    });
    vm.defineMethod(T, "contains", 1, 1, [](VM& vm, Value self, int, Value* args) -> Value {
        return Value::boolean(vm.setContains(asSet(self), args[0]));
    });
    vm.defineMethod(T, "isEmpty", 0, 0, [](VM& vm, Value self, int, Value*) -> Value { return Value::boolean(vm.setSize(asSet(self)) == 0); });
    vm.defineMethod(T, "isNotEmpty", 0, 0, [](VM& vm, Value self, int, Value*) -> Value { return Value::boolean(vm.setSize(asSet(self)) != 0); });
    vm.defineMethod(T, "clear", 0, 0, [](VM& vm, Value self, int, Value*) -> Value {
        ObjSet* set = asSet(self);
        if (set->view()) throw vm.viewChanged(set);
        set->items.clear();
        return Value::null();
    });
    vm.defineMethod(T, "addAll", 1, 1, [](VM& vm, Value self, int, Value* args) -> Value {
        ObjSet* set = asSet(self);
        if (set->view()) throw vm.viewChanged(set);
        bool changed = false;
        for (const Value& item : vm.itemsOf(args[0], "addAll()")) changed = vm.setAdd(set, item) || changed;
        return Value::boolean(changed);
    });
    // removeAll and retainAll take a collection, or a condition each item is given.
    auto removing = [](VM& vm, Value self, Value what, bool keepMatches) -> Value {
        ObjSet* set = asSet(self);
        if (set->view()) throw vm.viewChanged(set);
        const bool condition = what.isObjType(ObjType::Closure) || what.isObjType(ObjType::BoundMethod) || what.isObjType(ObjType::Native) ||
                               what.isObjType(ObjType::BoundNative);
        ObjSet* from = nullptr;
        if (!condition) {
            from = vm.newSet(nullptr);
            TempRoot keepFrom(vm, Value::object(from));
            for (const Value& item : vm.itemsOf(what, keepMatches ? "retainAll()" : "removeAll()")) vm.setAdd(from, item);
        }
        TempRoot keepFrom(vm, from != nullptr ? Value::object(from) : Value::null());
        std::vector<Value> items = vm.setItems(set);
        TempVector keep(vm, items);
        bool changed = false;
        for (Value& item : items) {
            const bool matches = condition ? vm.call(what, 1, &item).truthy() : vm.setContains(from, item);
            if (matches != keepMatches) changed = vm.setRemove(set, item) || changed;
        }
        return Value::boolean(changed);
    };
    static const auto removeBy = removing;
    vm.defineMethod(T, "removeAll", 1, 1, [](VM& vm, Value self, int, Value* args) -> Value { return removeBy(vm, self, args[0], false); });
    vm.defineMethod(T, "retainAll", 1, 1, [](VM& vm, Value self, int, Value* args) -> Value { return removeBy(vm, self, args[0], true); });
    vm.defineMethod(T, "toList", 0, 0, [](VM& vm, Value self, int, Value*) -> Value {
        ObjSet* set = asSet(self);
        ObjList* list = vm.alloc<ObjList>();
        TempRoot keep(vm, Value::object(list));
        list->values() = vm.setItems(set);
        vm.account(list->values().size() * sizeof(Value));
        if (set->type != nullptr && !set->type->args.empty()) vm.typeList(list, vm.typeOf(kTypeList, false, nullptr, {set->type->args[0]}));
        return Value::object(list);
    });
    auto end = [](VM& vm, Value self, bool last) -> Value {
        const std::vector<Value> items = vm.setItems(asSet(self));
        if (items.empty()) throw vm.runtimeError(Thrown::NoSuchElementException, std::string(last ? "last()" : "first()") + " of an empty set");
        return last ? items.back() : items.front();
    };
    static const auto endOf = end;
    vm.defineMethod(T, "first", 0, 0, [](VM& vm, Value self, int, Value*) -> Value { return endOf(vm, self, false); });
    vm.defineMethod(T, "last", 0, 0, [](VM& vm, Value self, int, Value*) -> Value { return endOf(vm, self, true); });
}

void registerMapMethods(VM& vm) {
    const BuiltinType T = BuiltinType::Map;



    // put and set store a value under a key; put gives the value that was there, or null.
    vm.defineMethod(T, "put", 2, 2, [](VM& vm, Value self, int, Value* args) -> Value {
        ObjMap* map = asMap(self);
        const Value* found = map->map.find(args[0]);
        Value before = found != nullptr ? *found : Value::null();
        if (found == nullptr) vm.account(2 * sizeof(Value));
        map->map.set(args[0], args[1]);
        return before;
    });
    vm.defineMethod(T, "set", 2, 2, [](VM& vm, Value self, int, Value* args) -> Value {
        ObjMap* map = asMap(self);
        if (map->map.find(args[0]) == nullptr) vm.account(2 * sizeof(Value));
        map->map.set(args[0], args[1]);
        return Value::null();
    });

    vm.defineMethod(T, "isEmpty", 0, 0, [](VM&, Value self, int, Value*) -> Value {
        return Value::boolean(asMap(self)->map.size() == 0);
    });
    vm.defineMethod(T, "isNotEmpty", 0, 0, [](VM&, Value self, int, Value*) -> Value {
        return Value::boolean(asMap(self)->map.size() != 0);
    });

    // The value under a key that has to be there.
    vm.defineMethod(T, "getValue", 1, 1, [](VM& vm, Value self, int, Value* args) -> Value {
        const Value* found = asMap(self)->map.find(args[0]);
        if (found == nullptr) throw vm.runtimeError(Thrown::NoSuchElementException, "getValue: the map has no key " + vm.stringify(args[0]));
        return *found;
    });
    vm.defineMethod(T, "getOrDefault", 2, 2, [](VM&, Value self, int, Value* args) -> Value {
        const Value* found = asMap(self)->map.find(args[0]);
        return found != nullptr ? *found : args[1];
    });

    // A new map with the same keys and values.
    auto copy = [](VM& vm, Value self, int, Value*) -> Value {
        ObjMap* made = vm.alloc<ObjMap>();
        for (const ValueMap::Entry& entry : asMap(self)->map.entries())
            if (entry.live) made->map.set(entry.key, entry.value);
        vm.account(made->map.size() * 2 * sizeof(Value));
        return Value::object(made);
    };
    vm.defineMethod(T, "toMap", 0, 0, copy);
    vm.defineMethod(T, "toOpenMap", 0, 0, copy);
    vm.defineMethod(T, "frozen", 0, 0, [](VM& vm, Value self, int, Value*) -> Value { return vm.frozenCopy(self); });

}

// ---- Ranges ----

void registerRangeMethods(VM& vm) {
    const BuiltinType T = BuiltinType::Range;

    vm.defineMethod(T, "first", 0, 0, [](VM& vm, Value self, int, Value*) -> Value {
        const ObjRange* range = asRange(self);
        if (range->count() == 0) throw vm.runtimeError(Thrown::NoSuchElementException, "first() of an empty range");
        return Value::integer(range->start);
    });
    vm.defineMethod(T, "last", 0, 0, [](VM& vm, Value self, int, Value*) -> Value {
        const ObjRange* range = asRange(self);
        if (range->count() == 0) throw vm.runtimeError(Thrown::NoSuchElementException, "last() of an empty range");
        return Value::integer(range->start + (range->count() - 1) * range->step);
    });
    vm.defineMethod(T, "isEmpty", 0, 0, [](VM&, Value self, int, Value*) -> Value {
        return Value::boolean(asRange(self)->count() == 0);
    });
}

// ---- Global functions ----

void registerGlobals(VM& vm) {
    vm.defineNative("compareValues", 2, 2, [](VM& vm, int, Value* args) -> Value {
        return Value::integer(compareValues(vm, args[0], args[1]));
    });

    // The largest and the smallest of the values given.
    vm.defineNative("maxOf", 1, -1, [](VM& vm, int argc, Value* args) -> Value {
        Value best = args[0];
        for (int i = 1; i < argc; i++)
            if (compareValues(vm, args[i], best) > 0) best = args[i];
        return best;
    });
    vm.defineNative("minOf", 1, -1, [](VM& vm, int argc, Value* args) -> Value {
        Value best = args[0];
        for (int i = 1; i < argc; i++)
            if (compareValues(vm, args[i], best) < 0) best = args[i];
        return best;
    });
}

// ---- Files ----
// Each goes to the host of the code running (host.h, M6.6.2): the system's, or a sandbox's.

// Whether a character separates the names of a path, in the host's style of paths.
bool isSeparator(char c, bool windows) { return c == '/' || (windows && c == '\\'); }

// A path without the separators it ends in, unless it is a root ("/", "C:\").
std::string_view withoutTrailingSeparators(std::string_view path, bool windows) {
    while (path.size() > 1 && isSeparator(path.back(), windows) && !(windows && path.size() == 3 && path[1] == ':')) path.remove_suffix(1);
    return path;
}

// Where the last name of a path starts.
size_t lastNameStart(std::string_view path, bool windows) {
    size_t at = path.size();
    while (at > 0 && !isSeparator(path[at - 1], windows) && !(windows && at == 2 && path[1] == ':')) at--;
    return at;
}

// A whole file read or written in one go. The work itself touches nothing of the VM, so it
// can be done with the lock released, or by a worker while a task is set aside (VM::blocking).
struct FileJob {
    std::shared_ptr<Host> host;
    std::string path;
    std::string data;
    std::string error;
    Charset charset = Charset::Utf8;  // what text read is in (D127)
    bool markIfNew = false;           // text appended in Charset.utf16: a byte order mark only where the file is empty (G53)
};

// A sandbox says why (M6.6.2); the system's message stays as it was, which says nothing more.
std::string because(const Host& host, const std::error_code& ec) { return host.isSandbox() ? ": " + ec.message() : ""; }

void readWhole(FileJob& job) {
    if (const std::error_code ec = job.host->readWhole(job.path, job.data)) job.error = "cannot read file '" + job.path + "'" + because(*job.host, ec);
}

void writeWhole(FileJob& job, bool append) {
    if (job.markIfNew) {
        const auto size = job.host->size(job.path);
        if (!size || *size == 0) job.data.insert(0, "\xFE\xFF", 2);
    }
    if (const std::error_code ec = job.host->writeWhole(job.path, job.data, append)) job.error = "cannot write file '" + job.path + "'" + because(*job.host, ec);
}

Value readFile(VM& vm, Value path, bool asBytes, Value charset = Value::null()) {
    auto job = std::make_shared<FileJob>();
    job->host = vm.currentHostShared();
    job->path = vm.toStr(path, "file path");
    job->charset = charsetOf(vm, charset);
    return vm.blocking([job, asBytes] {
        readWhole(*job);
        if (!asBytes && job->error.empty() && job->charset != Charset::Utf8) job->data = decodeText(job->data, job->charset);
    }, [job, asBytes](VM& vm) -> Value {
        if (!job->error.empty()) throw vm.errorOf("IOException", job->error);
        if (!asBytes) return vm.str(std::move(job->data));
        ObjBytes* made = vm.alloc<ObjBytes>();
        made->data.assign(job->data.begin(), job->data.end());
        vm.account(made->data.size());
        return Value::object(made);
    });
}

Value writeFile(VM& vm, Value path, Value content, bool bytesOnly, bool append, Value charset = Value::null()) {
    auto job = std::make_shared<FileJob>();
    job->host = vm.currentHostShared();
    job->path = vm.toStr(path, "file path");
    if (content.isObjType(ObjType::Bytes)) {
        job->data.assign(asBytes(content)->data.begin(), asBytes(content)->data.end());
    } else if (bytesOnly) {
        throw vm.runtimeError(Thrown::ClassCastException, "writeBytes takes bytes, not a value of type " + vm.typeName(content) + (content.isString() ? "; text is written with writeText" : ""));
    } else {
        const Charset cs = charsetOf(vm, charset);
        job->markIfNew = append && cs == Charset::Utf16;
        job->data = encodeText(vm.stringify(content), job->markIfNew ? Charset::Utf16Be : cs);
    }
    return vm.blocking([job, append] { writeWhole(*job, append); }, [job](VM& vm) -> Value {
        if (!job->error.empty()) throw vm.errorOf("IOException", job->error);
        return Value::null();
    });
}

// An IOException for something done to a path, with what the system said.
WwThrow pathError(VM& vm, const std::string& what, const std::string& path, const std::error_code& ec) {
    return vm.errorOf("IOException", "cannot " + what + " '" + path + "'" + (ec ? ": " + ec.message() : ""));
}

// What the FilePath class (src/std/file.ww) is built on. The name starts with two
// underscores, which only the standard library's own source can use.
void registerFiles(VM& vm) {
    ObjClass* file = vm.alloc<ObjClass>(vm.intern("FilePath"));
    auto def = [&](const char* name, int minArgs, int maxArgs, NativeFn fn) {
        file->statics.set(vm.intern(name), Value::object(vm.alloc<ObjNative>(std::string("FilePath.") + name, minArgs, maxArgs, fn)));
    };

    def("read", 1, 2, [](VM& vm, int argc, Value* args) -> Value { return readFile(vm, args[0], false, argc > 1 ? args[1] : Value::null()); });
    def("write", 2, 3, [](VM& vm, int argc, Value* args) -> Value { return writeFile(vm, args[0], args[1], false, false, argc > 2 ? args[2] : Value::null()); });
    def("append", 2, 3, [](VM& vm, int argc, Value* args) -> Value { return writeFile(vm, args[0], args[1], false, true, argc > 2 ? args[2] : Value::null()); });
    def("readBytes", 1, 1, [](VM& vm, int, Value* args) -> Value { return readFile(vm, args[0], true); });
    def("writeBytes", 2, 2, [](VM& vm, int, Value* args) -> Value { return writeFile(vm, args[0], args[1], true, false); });
    def("appendBytes", 2, 2, [](VM& vm, int, Value* args) -> Value { return writeFile(vm, args[0], args[1], true, true); });

    def("exists", 1, 1, [](VM& vm, int, Value* args) -> Value { return Value::boolean(vm.currentHost().exists(vm.toStr(args[0], "file path"))); });
    def("isFile", 1, 1, [](VM& vm, int, Value* args) -> Value { return Value::boolean(vm.currentHost().isFile(vm.toStr(args[0], "file path"))); });
    def("isDirectory", 1, 1, [](VM& vm, int, Value* args) -> Value { return Value::boolean(vm.currentHost().isDirectory(vm.toStr(args[0], "file path"))); });
    // Whether the path itself is a symbolic link (on Windows a junction too), whatever it leads to.
    def("isSymbolicLink", 1, 1, [](VM& vm, int, Value* args) -> Value { return Value::boolean(vm.currentHost().isSymbolicLink(vm.toStr(args[0], "file path"))); });
    // In bytes; 0 for a file that is not there.
    def("size", 1, 1, [](VM& vm, int, Value* args) -> Value {
        const auto size = vm.currentHost().size(vm.toStr(args[0], "file path"));
        return Value::integer(size ? static_cast<int64_t>(*size) : 0);
    });
    // When the file was last written, in nanoseconds since the start of 1970 (G54); null if it
    // is not there.
    def("lastModified", 1, 1, [](VM& vm, int, Value* args) -> Value {
        const auto time = vm.currentHost().lastModified(vm.toStr(args[0], "file path"));
        return time ? Value::integer(*time) : Value::null();
    });
    // The room on the volume a path is on, in bytes: [total, free, free to this program] (M8). A
    // sandbox has no volumes yet, and refuses.
    def("space", 1, 1, [](VM& vm, int, Value* args) -> Value {
        const std::string& path = vm.toStr(args[0], "file path");
        if (vm.currentHost().isSandbox())
            throw vm.runtimeError(Thrown::IOException, "a sandbox has no volumes to ask the space of");  // what the properties say they throw
        std::error_code ec;
        const std::filesystem::space_info room = std::filesystem::space(std::filesystem::path(std::u8string(path.begin(), path.end())), ec);
        if (ec) throw pathError(vm, "find the space of", path, ec);
        std::vector<Value> items{Value::integer(static_cast<int64_t>(room.capacity)), Value::integer(static_cast<int64_t>(room.free)),
                                 Value::integer(static_cast<int64_t>(room.available))};
        return Value::object(newList(vm, std::move(items)));
    });
    // The tops of the file system: each drive's on Windows ("C:\\"), "/" elsewhere (M8).
    def("roots", 0, 0, [](VM& vm, int, Value*) -> Value {
        if (vm.currentHost().isSandbox())
            throw vm.runtimeError(Thrown::UnsupportedOperationException, "a sandbox has no volumes to list");
        std::vector<std::string> roots;
#ifdef _WIN32
        wchar_t buffer[512];
        const DWORD length = GetLogicalDriveStringsW(511, buffer);
        for (const wchar_t* at = buffer; length > 0 && length < 512 && *at != L'\0'; at += wcslen(at) + 1) {
            std::string root;
            for (const wchar_t* c = at; *c != L'\0'; c++) root.push_back(static_cast<char>(*c));  // "C:\": ASCII
            roots.push_back(root);
        }
#else
        roots.push_back("/");
#endif
        TempRoot keep(vm, Value::object(newList(vm, {})));
        ObjList* list = asList(keep.get());
        for (const std::string& root : roots) list->values().push_back(vm.str(root));
        return keep.get();
    });
    // A file, or a directory with nothing in it. True if it was there and is gone; with
    // `mustExist`, an IOException where it was not there or could not go.
    def("delete", 2, 2, [](VM& vm, int, Value* args) -> Value {
        std::error_code ec;
        const std::string& path = vm.toStr(args[0], "file path");
        const bool removed = vm.currentHost().remove(path, ec);
        if (ec) throw pathError(vm, "delete", path, ec);
        if (!removed && args[1].truthy()) throw vm.errorOf("IOException", "cannot delete '" + path + "': there is nothing there");
        return Value::boolean(removed);
    });
    def("deleteRecursively", 1, 1, [](VM& vm, int, Value* args) -> Value {
        std::error_code ec;
        const auto removed = vm.currentHost().removeAll(vm.toStr(args[0], "file path"), ec);
        return Value::boolean(!ec && removed > 0);
    });
    // mkdir(path, parents): the directory, and with `parents` any missing above it (D126). One
    // there already is no failure; one that cannot be made is an IOException.
    def("mkdir", 2, 2, [](VM& vm, int, Value* args) -> Value {
        std::error_code ec;
        const std::string& path = vm.toStr(args[0], "file path");
        vm.currentHost().makeDirectory(path, args[1].truthy(), ec);
        if (!vm.currentHost().isDirectory(path)) throw pathError(vm, "make the directory", path, ec);
        return Value::null();
    });
    // move(from, to, overwrite) (D129): an IOException where it cannot be moved, or where
    // something is at `to` already and `overwrite` is false.
    def("move", 3, 3, [](VM& vm, int, Value* args) -> Value {
        const std::string& from = vm.toStr(args[0], "file path");
        const std::string& to = vm.toStr(args[1], "new path");
        if (!args[2].truthy() && vm.currentHost().exists(to))
            throw vm.errorOf("IOException", "cannot move '" + from + "' to '" + to + "': it is already there");
        if (const std::error_code ec = vm.currentHost().move(from, to))
            throw vm.errorOf("IOException", "cannot move '" + from + "' to '" + to + "': " + ec.message());
        return Value::null();
    });
    def("copy", 3, 3, [](VM& vm, int, Value* args) -> Value {
        std::error_code ec;
        const std::string& from = vm.toStr(args[0], "file path");
        const std::string& to = vm.toStr(args[1], "new path");
        const bool copied = vm.currentHost().copy(from, to, args[2].truthy(), ec);
        if (ec || !copied)
            throw vm.runtimeError(Thrown::IOException, "cannot copy '" + from + "' to '" + to + "'" + (ec ? ": " + ec.message() : ": it is already there"));
        return Value::null();
    });
    // The names in a directory, in order; an IOException if it is not one, or if reading it fails
    // part of the way, rather than the names read so far (8.8a).
    def("list", 1, 1, [](VM& vm, int, Value* args) -> Value {
        std::vector<std::string> names;
        const std::string& path = vm.toStr(args[0], "file path");
        if (const std::error_code ec = vm.currentHost().list(path, names)) throw pathError(vm, "list the directory", path, ec);
        std::sort(names.begin(), names.end());
        ObjList* list = newList(vm);
        TempRoot keep(vm, Value::object(list));
        for (std::string& name : names) list->values().push_back(vm.str(std::move(name)));
        vm.account(list->values().size() * sizeof(Value));
        return Value::object(list);
    });
    // What is in a directory, in order of name, as list gives the names: four values for each, its
    // name, what it is (0 a file, 1 a directory, 2 a link, 3 anything else), its size, and its
    // time in nanoseconds since 1970 or null. FilePath.entries makes them DirEntry values.
    def("entries", 1, 1, [](VM& vm, int, Value* args) -> Value {
        std::vector<Host::Entry> found;
        const std::string& path = vm.toStr(args[0], "file path");
        if (const std::error_code ec = vm.currentHost().entries(path, found)) throw pathError(vm, "list the directory", path, ec);
        std::sort(found.begin(), found.end(), [](const Host::Entry& a, const Host::Entry& b) { return a.name < b.name; });
        ObjList* list = newList(vm);
        TempRoot keep(vm, Value::object(list));
        std::vector<Value>& values = list->values();
        values.reserve(found.size() * 4);
        for (Host::Entry& e : found) {
            values.push_back(vm.str(std::move(e.name)));
            values.push_back(Value::integer(e.link ? 2 : e.directory ? 1 : e.file ? 0 : 3));
            values.push_back(Value::integer(static_cast<int64_t>(e.size)));
            values.push_back(e.modified ? Value::integer(*e.modified) : Value::null());
        }
        vm.account(values.size() * sizeof(Value));
        return Value::object(list);
    });
    // With `.`, `..` and links worked out (D135); an IOException if it is not there.
    def("canonical", 1, 1, [](VM& vm, int, Value* args) -> Value {
        std::error_code ec;
        const std::string& path = vm.toStr(args[0], "file path");
        std::string made = vm.currentHost().canonical(path, ec);
        if (ec) throw pathError(vm, "find", path, ec);
        return vm.str(std::move(made));
    });
    // What this program may do to it, as bits: 4 read, 2 write, 1 run; 0 if it is not there.
    def("permissions", 1, 1, [](VM& vm, int, Value* args) -> Value { return Value::integer(vm.currentHost().permissions(vm.toStr(args[0], "file path"))); });
    // Lets it be written, or not (on Windows, its read-only flag): elsewhere by its owner, as
    // Java's setWritable has it, and kept from being written by anyone.
    def("setWritable", 2, 2, [](VM& vm, int, Value* args) -> Value {
        const std::string& path = vm.toStr(args[0], "file path");
        if (const std::error_code ec = vm.currentHost().setWritable(path, args[1].truthy())) throw pathError(vm, "change who may write", path, ec);
        return Value::null();
    });
    // setLastModified(path, nanoseconds since the start of 1970).
    def("setLastModified", 2, 2, [](VM& vm, int, Value* args) -> Value {
        const std::string& path = vm.toStr(args[0], "file path");
        if (const std::error_code ec = vm.currentHost().setLastModified(path, vm.toInt(args[1], "time"))) throw pathError(vm, "set the time of", path, ec);
        return Value::null();
    });
    // A new empty directory, or file, in the temporary directory, its name starting with
    // `prefix` (and a file's ending with `suffix`).
    def("createTemp", 3, 3, [](VM& vm, int, Value* args) -> Value {
        std::error_code ec;
        std::string base;
        const std::string& prefix = vm.toStr(args[0], "prefix");
        const bool directory = args[2].truthy();
        const std::string suffix = directory ? "" : vm.toStr(args[1], "suffix");
        std::optional<std::string> made = vm.currentHost().createTemp(prefix, suffix, directory, base, ec);
        if (made) return vm.str(std::move(*made));
        if (ec) throw pathError(vm, "find", "the temporary directory", ec);
        throw vm.errorOf("IOException", std::string("cannot make a temporary ") + (directory ? "directory" : "file") + " in '" + base + "'");
    });
    def("absolute", 1, 1, [](VM& vm, int, Value* args) -> Value { return vm.str(vm.currentHost().absolute(vm.toStr(args[0], "file path"))); });

    // The last name of a path, the path of the folder it is in (null if the path says none),
    // and a path with a name added, in the host's style of paths.
    def("name", 1, 1, [](VM& vm, int, Value* args) -> Value {
        const bool windows = vm.currentHost().windowsPaths();
        const std::string_view path = withoutTrailingSeparators(vm.toStr(args[0], "file path"), windows);
        return vm.str(std::string(path.substr(lastNameStart(path, windows))));
    });
    def("parent", 1, 1, [](VM& vm, int, Value* args) -> Value {
        const bool windows = vm.currentHost().windowsPaths();
        const std::string_view path = withoutTrailingSeparators(vm.toStr(args[0], "file path"), windows);
        const size_t at = lastNameStart(path, windows);
        if (at == 0 || at == path.size()) return Value::null();
        return vm.str(std::string(withoutTrailingSeparators(path.substr(0, at), windows)));
    });
    def("join", 2, 2, [](VM& vm, int, Value* args) -> Value {
        const bool windows = vm.currentHost().windowsPaths();
        const std::string& base = vm.toStr(args[0], "file path");
        const std::string& child = vm.toStr(args[1], "file name");
        if (base.empty()) return args[1];
        if (isSeparator(base.back(), windows)) return vm.str(base + child);
        return vm.str(base + (windows ? '\\' : '/') + child);
    });
    // The separator, as the program reads it, follows the host too (FilePath.separator in file.ww).
    def("separator", 0, 0, [](VM& vm, int, Value*) -> Value { return vm.str(vm.currentHost().windowsPaths() ? "\\" : "/"); });

    vm.defineGlobal("__file", Value::object(file));
}

}  // namespace

void registerStdlib(VM& vm) {
    registerGlobals(vm);
    registerStringMethods(vm);
    registerNumberMethods(vm);
    registerCharMethods(vm);
    registerByteMethods(vm);
    registerIndexMethods(vm);
    registerRangeWords(vm);
    registerNumberConstants(vm);
    registerListMethods(vm);
    registerMapMethods(vm);
    registerSetMethods(vm);
    registerRangeMethods(vm);
    registerFiles(vm);
}

}  // namespace ww
