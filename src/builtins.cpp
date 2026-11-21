// The standard library: global functions, the Math and File classes, and the methods of
// the built-in types.

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#ifdef __linux__
#include <sys/sysinfo.h>
#endif
#ifndef _WIN32
#include <unistd.h>
extern char** environ;
#endif
#if defined(__SSE2__) || defined(_M_X64)
#include <emmintrin.h>
#endif

#include <algorithm>
#include <bit>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>

#include "builtins.h"
#include "charset.h"
#include "fmath.h"
#include "run.h"
#include "sandbox.h"
#include "unicode.h"
#include "vm.h"

namespace ww {

void (*registerScripts)(VM& vm) = nullptr;

// A request for more memory than the machine could ever give, its memory and page file together,
// is refused at once, with the same error that running out gives; below that the allocator
// decides. (Asking anyway would also stop a program built with the address sanitizer, whose
// allocator does not fail in the ordinary way.) In a script's VM, one for more than its heap's
// limit is refused before anything is taken, as the limit (limitMemory): its call ends.
void VM::checkRoomFor(uint64_t bytes) {
    if (memoryLimit_ > 0 && bytes > memoryLimit_) {
        limitPassed(memoryLimitText());
        WwThrow thrown{Value::null()};
        thrown.limit = true;
        throw thrown;
    }
    static const uint64_t limit = [] {
#ifdef _WIN32
        MEMORYSTATUSEX status{};
        status.dwLength = sizeof status;
        return GlobalMemoryStatusEx(&status) ? static_cast<uint64_t>(status.ullTotalPageFile) : UINT64_MAX;
#elif defined(__linux__)
        struct sysinfo info{};
        return sysinfo(&info) == 0 ? (static_cast<uint64_t>(info.totalram) + info.totalswap) * info.mem_unit : UINT64_MAX;
#else
        return UINT64_MAX;
#endif
    }();
    if (bytes > limit) throw outOfMemory();
}

#ifdef _WIN32
// UTF-16 from Windows as UTF-8.
static std::string wideToUtf8(const std::wstring& wide) {
    if (wide.empty()) return std::string();
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string utf8(static_cast<size_t>(size > 0 ? size : 0), '\0');
    if (size > 0) WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), utf8.data(), size, nullptr, nullptr);
    return utf8;
}
#endif

// ---- Helpers (declared in builtins.h; stdlib.cpp uses them too) ----

// Clamps a possibly negative position into [0, length], as slice() bounds.
size_t clampPosition(int64_t position, size_t length) {
    return static_cast<size_t>(std::clamp<int64_t>(position, 0, static_cast<int64_t>(length)));
}

// Upper or lower case for all letters, as Kotlin's uppercase() and lowercase() give it (Java's,
// with Locale.ROOT): full mappings from WinterWolf's own Unicode data (unicode.h), the same on
// every system.
std::string changeCase(const std::string& s, bool upper) { return upper ? unicode::upper(s) : unicode::lower(s); }

std::string_view trimmed(std::string_view s) {
    const char* whitespace = " \t\r\n\f\v";
    size_t begin = s.find_first_not_of(whitespace);
    if (begin == std::string_view::npos) return {};
    size_t end = s.find_last_not_of(whitespace);
    return s.substr(begin, end - begin + 1);
}

bool parseInt(std::string_view s, int64_t& out) {
    if (!s.empty() && s[0] == '+') s.remove_prefix(1);
    auto res = std::from_chars(s.data(), s.data() + s.size(), out);
    return !s.empty() && res.ec == std::errc() && res.ptr == s.data() + s.size();
}

bool parseFloat(std::string_view s, double& out) {
    if (!s.empty() && s[0] == '+') s.remove_prefix(1);
    auto res = std::from_chars(s.data(), s.data() + s.size(), out);
    if (s.empty() || res.ptr != s.data() + s.size()) return false;
    if (res.ec == std::errc()) return true;
    if (res.ec != std::errc::result_out_of_range) return false;
    // Too large or too small for a float, as Kotlin's, Java's and Python's give it: infinity
    // past the largest, zero below the smallest, with the sign written. Which of the two is
    // where the first digit that is not 0 stands, its power of ten, moved by the exponent.
    const bool negative = s[0] == '-';
    size_t i = negative ? 1 : 0;
    long long before = 0;  // digits before the point, from the first that is not 0
    long long zeros = 0;   // where none is before it: the 0s after the point before the first that is not
    bool seen = false, afterPoint = false;
    for (; i < s.size() && s[i] != 'e' && s[i] != 'E'; i++) {
        if (s[i] == '.') {
            afterPoint = true;
        } else if (!afterPoint) {
            if (s[i] != '0') seen = true;
            if (seen) before++;
        } else if (!seen) {
            if (s[i] != '0') seen = true;
            else zeros++;
        }
    }
    const long long place = before > 0 ? before - 1 : -(zeros + 1);
    long long exponent = 0;
    if (i < s.size()) {
        int64_t e = 0;
        std::string_view digits = s.substr(i + 1);
        if (!digits.empty() && digits[0] == '+') digits.remove_prefix(1);
        auto er = std::from_chars(digits.data(), digits.data() + digits.size(), e);
        exponent = er.ec == std::errc() ? e : (!digits.empty() && digits[0] == '-' ? -1000000 : 1000000);
    }
    const double magnitude = place + exponent > 0 ? std::numeric_limits<double>::infinity() : 0.0;  // out of range: past one end or the other
    out = negative ? -magnitude : magnitude;
    return true;
}

int64_t floatToInt(VM& vm, double d) {
    if (!(d >= -9223372036854775808.0 && d < 9223372036854775808.0))
        throw vm.runtimeError(Thrown::ArithmeticException, "number " + formatFloat(d) + " does not fit in an int");
    return static_cast<int64_t>(d);
}

std::vector<std::string> splitLines(const std::string& text) {
    std::vector<std::string> lines;
    size_t start = 0;
    for (;;) {
        size_t end = text.find('\n', start);
        std::string line = text.substr(start, end == std::string::npos ? end : end - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(std::move(line));
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return lines;
}

ObjList* newList(VM& vm, std::vector<Value> items) {
    ObjList* list = vm.alloc<ObjList>();
    list->values() = std::move(items);
    vm.account(list->values().size() * sizeof(Value));
    return list;
}

// Ordering used by sort() when no comparator is given.
bool defaultLess(VM& vm, const Value& a, const Value& b) {
    if (a.isInt() && b.isInt()) return a.as.i < b.as.i;
    if (a.isNumber() && b.isNumber()) return a.toDouble() < b.toDouble();
    if (a.isString() && b.isString()) return compareText(asString(a), asString(b)) < 0;
    // Chars by their numbers; a byte as the int it holds.
    if (a.isChar() && b.isChar()) return a.as.i < b.as.i;
    if ((a.isByte() || a.isInt()) && (b.isByte() || b.isInt())) return a.as.i < b.as.i;
    if ((a.isByte() || a.isNumber()) && (b.isByte() || b.isNumber()))
        return (a.isFloat() ? a.as.d : static_cast<double>(a.as.i)) < (b.isFloat() ? b.as.d : static_cast<double>(b.as.i));
    // An object whose class says how it compares, with compareTo (Kotlin's Comparable): a Duration.
    if (const ObjClass* cls = classOf(a); cls != nullptr && cls->methods.find(vm.names.compareTo) != nullptr) {
        const Value method = vm.memberGet(a, vm.names.compareTo);
        TempRoot keep(vm, method);
        const Value order = vm.call(method, 1, &b);
        if (order.isInt()) return order.as.i < 0;
    }
    throw vm.runtimeError(Thrown::ClassCastException, "cannot compare " + vm.typeName(a) + " and " + vm.typeName(b) + " while sorting");
}

// Stable merge sort of items[lo, hi). Hand-written because the comparator is user code:
// it may throw or be inconsistent, which std::sort does not tolerate.
void mergeSort(std::vector<Value>& items, std::vector<Value>& scratch, size_t lo, size_t hi,
               const std::function<bool(const Value&, const Value&)>& less) {
    if (hi - lo < 2) return;
    size_t mid = lo + (hi - lo) / 2;
    mergeSort(items, scratch, lo, mid, less);
    mergeSort(items, scratch, mid, hi, less);

    size_t i = lo, j = mid, k = lo;
    while (i < mid && j < hi) scratch[k++] = less(items[j], items[i]) ? items[j++] : items[i++];
    while (i < mid) scratch[k++] = items[i++];
    while (j < hi) scratch[k++] = items[j++];
    std::copy(scratch.begin() + static_cast<std::ptrdiff_t>(lo), scratch.begin() + static_cast<std::ptrdiff_t>(hi),
              items.begin() + static_cast<std::ptrdiff_t>(lo));
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

namespace {

// How many of data[start, end) are `value`, and with `places` given, where each is, in order
// (Bytes.indicesOf: counted first, so that the list is made the size it ends at). With SSE2,
// which every x64 processor has, 64 bytes are compared at a time, as four of 16, and the
// matches read off a 64-bit mask, rather than a memchr call for each. The first four of a
// block's matches are written whether there are four or not, and the count moved on by how
// many there are, so that no branch depends on how many there are (as simdjson does): for
// line ends, a match every 60 bytes or so, a branch on it would guess wrong half the time.
// `places` has room for 4 more than are found.
size_t bytePlaces(const uint8_t* data, int64_t start, int64_t end, uint8_t value, int64_t* places) {
    size_t found = 0;
    int64_t at = start;
#if defined(__SSE2__) || defined(_M_X64)
    const __m128i wanted = _mm_set1_epi8(static_cast<char>(value));
    auto mask16 = [&](int64_t from) {
        const __m128i chunk = _mm_loadu_si128(reinterpret_cast<const __m128i*>(data + from));
        return static_cast<uint64_t>(static_cast<unsigned>(_mm_movemask_epi8(_mm_cmpeq_epi8(chunk, wanted))));
    };
    for (; at + 64 <= end; at += 64) {
        uint64_t mask = mask16(at) | mask16(at + 16) << 16 | mask16(at + 32) << 32 | mask16(at + 48) << 48;
        const int count = std::popcount(mask);
        if (places != nullptr) {
            int64_t* out = places + found;
            for (int i = 0; i < 4; i++) {
                out[i] = at + std::countr_zero(mask);  // 64 past the last: written over or left beyond
                mask &= mask - 1;
            }
            for (int i = 4; mask != 0; i++) {
                out[i] = at + std::countr_zero(mask);
                mask &= mask - 1;
            }
        }
        found += static_cast<size_t>(count);
    }
#endif
    for (; at < end; at++)
        if (data[at] == value) {
            if (places != nullptr) places[found] = at;
            found++;
        }
    return found;
}

// ---- Global functions ----

void registerGlobals(VM& vm) {
    // __nameParams(type, name, params): what a built-in's parameters are called, from its
    // declaration in src/std/builtins.ww. The standard library calls it as it loads; the
    // type is "" for a function.
    vm.defineNative("__nameParams", 3, 3, [](VM& vm, int, Value* args) -> Value {
        if (!args[0].isString() || !args[1].isString() || !args[2].isObjType(ObjType::List)) throw vm.runtimeError(Thrown::InternalError, "__nameParams: bad arguments");
        std::vector<std::string> params;
        for (const Value& p : vm.listCopy(asList(args[2])))
            if (p.isString()) params.push_back(asString(p)->utf8());
        const std::string& type = asString(args[0])->utf8();
        BuiltinType builtin;
        if (type.empty()) {
            vm.nameFunctionParams(asString(args[1])->utf8(), params);
        } else if (VM::builtinTypeNamed(type, builtin)) {
            vm.nameParams(builtin, asString(args[1])->utf8(), params);
        } else if (ObjNative* member = vm.builtinStatic(type, asString(args[1])->utf8())) {
            // A function of an object the C++ provides (`Math.pow`).
            if (params.size() > member->params.size()) member->params = params;
        }
        return Value::null();
    });
    vm.defineNative("print", 0, -1, [](VM& vm, int argc, Value* args) -> Value {
        for (int i = 0; i < argc; i++) {
            if (i > 0) *vm.output << ' ';
            *vm.output << vm.stringify(args[i]);
        }
        *vm.output << '\n';
        return Value::null();
    });

    // As print, to standard error; what print wrote before goes out first.
    vm.defineNative("printError", 0, -1, [](VM& vm, int argc, Value* args) -> Value {
        vm.output->flush();
        for (int i = 0; i < argc; i++) {
            if (i > 0) *vm.errorOutput << ' ';
            *vm.errorOutput << vm.stringify(args[i]);
        }
        *vm.errorOutput << '\n';
        vm.errorOutput->flush();
        return Value::null();
    });

    vm.defineNative("write", 0, -1, [](VM& vm, int argc, Value* args) -> Value {
        for (int i = 0; i < argc; i++) *vm.output << vm.stringify(args[i]);
        return Value::null();
    });

    // Sends on whatever print and write have gathered. Output to a file or to another program
    // is otherwise sent in pieces, and at the latest when the program ends.
    vm.defineNative("flush", 0, 0, [](VM& vm, int, Value*) -> Value {
        vm.output->flush();
        return Value::null();
    });

    vm.defineNative("input", 0, 1, [](VM& vm, int argc, Value* args) -> Value {
        if (argc > 0) *vm.output << vm.stringify(args[0]);
        vm.output->flush();
        std::string line;
        bool got;
        {
            LockReleased released(vm);  // other threads go on while this one waits for a line
            got = static_cast<bool>(std::getline(*vm.input, line));
        }
        if (!got) return Value::null();
        if (!line.empty() && line.back() == '\r') line.pop_back();
        // Text piped in on Windows often starts with a UTF-8 byte order mark.
        if (line.compare(0, 3, "\xEF\xBB\xBF") == 0) line.erase(0, 3);
        return vm.str(std::move(line));
    });





    // In a sandbox, its own clock, which moves only when the program waits (M6.6.2).
    vm.defineNative("clock", 0, 0, [](VM& vm, int, Value*) -> Value {
        if (Host& host = vm.currentHost(); host.isSandbox()) return Value::number(static_cast<double>(static_cast<SandboxHost&>(host).now()) / 1e9);
        auto now = std::chrono::steady_clock::now().time_since_epoch();
        return Value::number(std::chrono::duration<double>(now).count());
    });
    // What the system's clock says, in nanoseconds since the start of 1970 (Instant.now, G54); in
    // a sandbox, its own, from 2000-01-01.
    vm.defineNative("__epochNanoseconds", 0, 0, [](VM& vm, int, Value*) -> Value {
        if (Host& host = vm.currentHost(); host.isSandbox()) return Value::integer(SandboxHost::kWallStart + static_cast<SandboxHost&>(host).now());
        const auto now = std::chrono::system_clock::now().time_since_epoch();
        return Value::integer(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
    });

    // How many seconds the system's time zone is ahead of UTC at a moment, given in seconds
    // since the start of 1970 (LocalDateTime, M8); summer time included. A sandbox is in UTC.
    vm.defineNative("__localOffset", 1, 1, [](VM& vm, int, Value* args) -> Value {
        if (vm.currentHost().isSandbox()) return Value::integer(0);
        const time_t t = static_cast<time_t>(vm.toInt(args[0], "seconds"));
        std::tm local{};
#ifdef _WIN32
        if (localtime_s(&local, &t) != 0) return Value::integer(0);  // before 1970 or past 3000: taken as UTC
#else
        if (localtime_r(&t, &local) == nullptr) return Value::integer(0);
#endif
        // The local date and time as if they were UTC, less the moment itself (days_from_civil).
        const int64_t year = local.tm_year + 1900 - (local.tm_mon < 2 ? 1 : 0);
        const int64_t era = (year >= 0 ? year : year - 399) / 400;
        const int64_t yoe = year - era * 400;
        const int64_t month = local.tm_mon + 1;
        const int64_t doy = (153 * (month > 2 ? month - 3 : month + 9) + 2) / 5 + local.tm_mday - 1;
        const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
        const int64_t days = era * 146097 + doe - 719468;
        const int64_t civil = days * 86400 + local.tm_hour * 3600 + local.tm_min * 60 + local.tm_sec;
        return Value::integer(civil - static_cast<int64_t>(t));
    });

    vm.defineNative("Range", 1, 3, [](VM& vm, int argc, Value* args) -> Value {
        int64_t start = 0, end, step = 1;
        if (argc == 1) {
            end = vm.toInt(args[0], "range end");
        } else {
            start = vm.toInt(args[0], "range start");
            end = vm.toInt(args[1], "range end");
            if (argc == 3) step = vm.toInt(args[2], "range step");
        }
        if (step == 0) throw vm.runtimeError(Thrown::IllegalArgumentException, "range step cannot be zero");
        return Value::object(vm.alloc<ObjRange>(start, end, step, false));
    });

    // Throws what it is given as it is: the standard library's, for a failure caught where it was
    // thrown and given again later, whose class the compiler cannot know (D35).
    vm.defineNative("__rethrow", 1, 1, [](VM&, int, Value* args) -> Value {
        WwThrow thrown;
        thrown.value = args[0];
        throw thrown;
    });
    vm.defineNative("__callThrowing", 1, 1, [](VM& vm, int, Value* args) -> Value { return vm.call(args[0], 0, nullptr); });
    // `extend Button { var tag : string? = null }` (D157): the class, the field's name, and what
    // makes its starting value. A second extend giving the same name keeps the first.
    vm.defineNative("__addExtendField", 3, 3, [](VM&, int, Value* args) -> Value {
        ObjClass* cls = asClass(args[0]);
        ObjString* name = asString(args[1]);
        if (cls->extendFields.find(name) == nullptr) cls->extendFields.set(name, args[2]);
        return Value::null();
    });

    // The program's name, by default: its own file's, without .ww (Program.name, D184).
    // Where an error is being made, for Throwable's frames (VM::stackHere).
    vm.defineNative("__stackHere", 0, 0, [](VM& vm, int, Value*) -> Value { return vm.stackHere(); });
    // The calls under way in Snippy files, for Snippy's tracebacks (VM::snippyFrames).
    vm.defineNative("__snippyFrames", 0, 0, [](VM& vm, int, Value*) -> Value { return vm.snippyFrames(); });
    vm.defineNative("__programName", 0, 0, [](VM& vm, int, Value*) -> Value {
        return vm.str(vm.mainFileName());
    });

    // Program.collectGarbage() and Program.memoryUsed: a collection now, and the bytes the objects
    // take; the collection also gives back the large blocks kept for lists to reuse (value.h,
    // ItemAllocator), which Program.memoryKept counts.
    vm.defineNative("__collectGarbage", 0, 0, [](VM& vm, int, Value*) -> Value {
        vm.collectNow();
        releaseItemBlocks();
        return Value::null();
    });
    vm.defineNative("__memoryUsed", 0, 0, [](VM& vm, int, Value*) -> Value {
        return Value::integer(static_cast<int64_t>(vm.memoryUsed()));
    });
    vm.defineNative("__memoryKept", 0, 0, [](VM&, int, Value*) -> Value {
        return Value::integer(static_cast<int64_t>(keptItemBytes()));
    });

    vm.defineNative("__arguments", 0, 0, [](VM& vm, int, Value*) -> Value {
        std::vector<Value> items;
        for (const std::string& arg : vm.scriptArgs) items.push_back(vm.str(arg));
        return Value::object(newList(vm, std::move(items)));
    });

    // The environment the program was started with, as a map from each variable's name to its
    // value (Program.environment, M8); in a sandbox, none, so that a test sees the same
    // everywhere.
    vm.defineNative("__environment", 0, 0, [](VM& vm, int, Value*) -> Value {
        ObjMap* made = vm.alloc<ObjMap>();
        TempRoot keep(vm, Value::object(made));
        if (vm.currentHost().isSandbox()) return Value::object(made);
#ifdef _WIN32
        wchar_t* block = GetEnvironmentStringsW();
        if (block != nullptr) {
            for (const wchar_t* at = block; *at != L'\0'; at += wcslen(at) + 1) {
                const std::wstring entry(at);
                const size_t eq = entry.find(L'=', 1);  // "=C:=C:\\": a drive's folder, its name starting with '='
                if (eq == std::wstring::npos || entry[0] == L'=') continue;
                const Value name = vm.str(wideToUtf8(entry.substr(0, eq)));
                TempRoot keepName(vm, name);
                made->map.set(name, vm.str(wideToUtf8(entry.substr(eq + 1))));
            }
            FreeEnvironmentStringsW(block);
        }
#else
        for (char** at = environ; *at != nullptr; at++) {
            const std::string entry(*at);
            const size_t eq = entry.find('=');
            if (eq == std::string::npos || eq == 0) continue;
            const Value name = vm.str(entry.substr(0, eq));
            TempRoot keepName(vm, name);
            made->map.set(name, vm.str(entry.substr(eq + 1)));
        }
#endif
        vm.account(made->map.size() * 2 * sizeof(Value));
        return Value::object(made);
    });
    // The program's own process id (Program.processId); 1 in a sandbox.
    vm.defineNative("__processId", 0, 0, [](VM& vm, int, Value*) -> Value {
        if (vm.currentHost().isSandbox()) return Value::integer(1);
#ifdef _WIN32
        return Value::integer(static_cast<int64_t>(GetCurrentProcessId()));
#else
        return Value::integer(static_cast<int64_t>(getpid()));
#endif
    });
    // The computer's name on the network (Program.hostName); "sandbox" in a sandbox.
    vm.defineNative("__hostName", 0, 0, [](VM& vm, int, Value*) -> Value {
        if (vm.currentHost().isSandbox()) return vm.str("sandbox");
#ifdef _WIN32
        wchar_t name[256];
        DWORD size = 256;
        if (GetComputerNameExW(ComputerNameDnsHostname, name, &size)) return vm.str(wideToUtf8(std::wstring(name, size)));
        size = 256;
        if (GetComputerNameW(name, &size)) return vm.str(wideToUtf8(std::wstring(name, size)));
        return vm.str("localhost");
#else
        char name[256] = {};
        if (gethostname(name, sizeof name - 1) == 0) return vm.str(name);
        return vm.str("localhost");
#endif
    });

    vm.defineNative("exit", 0, 1, [](VM& vm, int argc, Value* args) -> Value {
        throw WwExit{argc > 0 ? static_cast<int>(vm.toInt(args[0], "exit code")) : 0};
    });

    // A value's own hash, as a map takes it, at 32 bits: what hashCode() gives for a value
    // whose class has none of its own (D26).
    vm.defineNative("__hashCode", 1, 1, [](VM&, int, Value* args) -> Value {
        const size_t one = args[0].isNull() ? 0 : keyHash(args[0]);
        return Value::integer(static_cast<int32_t>(static_cast<uint32_t>(one ^ (one >> 32))));
    });

    // The hash of several values together, as Java's Objects.hash works it out, wrapping at 32
    // bits inside, so a class's hash needs no `31 * h + ...` of its own (D148).
    vm.defineNative("hash", 0, -1, [](VM&, int argc, Value* args) -> Value {
        uint32_t result = 1;
        for (int i = 0; i < argc; i++) {
            const size_t one = args[i].isNull() ? 0 : keyHash(args[i]);
            result = 31 * result + static_cast<uint32_t>(one ^ (one >> 32));
        }
        return Value::integer(static_cast<int32_t>(result));
    });
}

// ---- Math ----

// A byte as the int it is, for the functions that keep an int an int; anything else as it is.
static Value asWideInt(Value v) { return v.isByte() ? Value::integer(v.as.i) : v; }

void registerMath(VM& vm) {
    ObjClass* math = vm.alloc<ObjClass>(vm.intern("Math"));
    auto def = [&](const char* name, int minArgs, int maxArgs, NativeFn fn) {
        math->statics.set(vm.intern(name),
                          Value::object(vm.alloc<ObjNative>(std::string("Math.") + name, minArgs, maxArgs, fn)));
    };

    math->statics.set(vm.intern("pi"), Value::number(3.14159265358979323846));
    math->statics.set(vm.intern("e"), Value::number(2.71828182845904523536));

#define WW_MATH_UNARY(NAME, FN)                                           \
    def(NAME, 1, 1, [](VM& vm, int, Value* args) -> Value {                 \
        return Value::number(FN(vm.toNumber(args[0], "Math." NAME "() argument"))); \
    })
    WW_MATH_UNARY("sqrt", std::sqrt);
    WW_MATH_UNARY("sin", fmath::sin);
    WW_MATH_UNARY("cos", fmath::cos);
    WW_MATH_UNARY("tan", fmath::tan);
    WW_MATH_UNARY("ln", fmath::log);
    WW_MATH_UNARY("exp", fmath::exp);
    WW_MATH_UNARY("log10", fmath::log10);
    WW_MATH_UNARY("log2", fmath::log2);
    WW_MATH_UNARY("asin", fmath::asin);
    WW_MATH_UNARY("acos", fmath::acos);
    WW_MATH_UNARY("atan", fmath::atan);
    WW_MATH_UNARY("trunc", std::trunc);
    WW_MATH_UNARY("sinh", fmath::sinh);
    WW_MATH_UNARY("cosh", fmath::cosh);
    WW_MATH_UNARY("tanh", fmath::tanh);
    WW_MATH_UNARY("asinh", fmath::asinh);
    WW_MATH_UNARY("acosh", fmath::acosh);
    WW_MATH_UNARY("atanh", fmath::atanh);
    WW_MATH_UNARY("expm1", fmath::expm1);
    WW_MATH_UNARY("log1p", fmath::log1p);
#undef WW_MATH_UNARY

    // The logarithm in a base given, as Kotlin's log(x, base): NaN for a base of 1 or not above 0.
    // Bases 2 and 10 by their own functions, which are exact at their powers: log(1000, 10) is 3.0.
    def("log", 2, 2, [](VM& vm, int, Value* args) -> Value {
        const double x = vm.toNumber(args[0], "Math.log() argument");
        return Value::number(fmath::logBase(x, vm.toNumber(args[1], "Math.log() base")));
    });

    def("abs", 1, 1, [](VM& vm, int, Value* args) -> Value {
        args[0] = asWideInt(args[0]);   // a byte is an int here
        Value v = args[0];
        if (v.isInt()) {
            if (v.as.i == INT64_MIN) throw vm.overflowError(Op::Sub, 0, v.as.i);  // (D27)
            return Value::integer(v.as.i < 0 ? -v.as.i : v.as.i);
        }
        return Value::number(std::fabs(vm.toNumber(v, "Math.abs() argument")));
    });

    def("floor", 1, 1, [](VM& vm, int, Value* args) -> Value {
        args[0] = asWideInt(args[0]);   // a byte is an int here
        if (args[0].isInt()) return args[0];
        return Value::integer(floatToInt(vm, std::floor(vm.toNumber(args[0], "Math.floor() argument"))));
    });

    def("ceil", 1, 1, [](VM& vm, int, Value* args) -> Value {
        args[0] = asWideInt(args[0]);   // a byte is an int here
        if (args[0].isInt()) return args[0];
        return Value::integer(floatToInt(vm, std::ceil(vm.toNumber(args[0], "Math.ceil() argument"))));
    });

    def("round", 1, 1, [](VM& vm, int, Value* args) -> Value {
        args[0] = asWideInt(args[0]);   // a byte is an int here
        if (args[0].isInt()) return args[0];
        return Value::integer(floatToInt(vm, std::round(vm.toNumber(args[0], "Math.round() argument"))));
    });

    // (Op::Numeric does min and max of two, and the rest, as these do: VM::numeric below.)
    def("min", 1, -1, [](VM& vm, int argc, Value* args) -> Value {
        Value best = args[0];
        vm.toNumber(best, "Math.min() argument");
        for (int i = 1; i < argc; i++)
            if (vm.toNumber(args[i], "Math.min() argument") < vm.toNumber(best, "Math.min() argument")) best = args[i];
        return best.isByte() ? Value::integer(best.as.i) : best;
    });

    def("max", 1, -1, [](VM& vm, int argc, Value* args) -> Value {
        Value best = args[0];
        vm.toNumber(best, "Math.max() argument");
        for (int i = 1; i < argc; i++)
            if (vm.toNumber(args[i], "Math.max() argument") > vm.toNumber(best, "Math.max() argument")) best = args[i];
        return best.isByte() ? Value::integer(best.as.i) : best;
    });

    def("pow", 2, 2, [](VM& vm, int, Value* args) -> Value {
        args[0] = asWideInt(args[0]);   // a byte is an int here
        args[1] = asWideInt(args[1]);
        if (args[0].isInt() && args[1].isInt() && args[1].as.i >= 0)
            return Value::integer(integerPower(vm, args[0].as.i, args[1].as.i));
        return Value::number(fmath::pow(vm.toNumber(args[0], "Math.pow() base"), vm.toNumber(args[1], "Math.pow() exponent")));
    });

    // In a sandbox, its own randomness, from a fixed seed (M6.6.2).
    def("random", 0, 0, [](VM& vm, int, Value*) -> Value {
        if (Host& host = vm.currentHost(); host.isSandbox()) {
            SandboxHost& box = static_cast<SandboxHost&>(host);
            std::lock_guard<std::mutex> hold(box.randomMutex);
            return Value::number(std::uniform_real_distribution<double>(0.0, 1.0)(box.random));
        }
        return Value::number(std::uniform_real_distribution<double>(0.0, 1.0)(vm.rng));
    });

    def("randomInt", 2, 2, [](VM& vm, int, Value* args) -> Value {
        int64_t lo = vm.toInt(args[0], "Math.randomInt() lower bound");
        int64_t hi = vm.toInt(args[1], "Math.randomInt() upper bound");
        if (lo > hi) throw vm.runtimeError(Thrown::IllegalArgumentException, "Math.randomInt() lower bound is greater than the upper bound");
        if (Host& host = vm.currentHost(); host.isSandbox()) {
            SandboxHost& box = static_cast<SandboxHost&>(host);
            std::lock_guard<std::mutex> hold(box.randomMutex);
            return Value::integer(std::uniform_int_distribution<int64_t>(lo, hi)(box.random));
        }
        return Value::integer(std::uniform_int_distribution<int64_t>(lo, hi)(vm.rng));
    });

    def("seed", 1, 1, [](VM& vm, int, Value* args) -> Value {
        const uint64_t seed = static_cast<uint64_t>(vm.toInt(args[0], "Math.seed() argument"));
        if (Host& host = vm.currentHost(); host.isSandbox()) {
            SandboxHost& box = static_cast<SandboxHost&>(host);
            std::lock_guard<std::mutex> hold(box.randomMutex);
            box.random.seed(seed);
            return Value::null();
        }
        vm.rng.seed(seed);
        return Value::null();
    });

    def("atan2", 2, 2, [](VM& vm, int, Value* args) -> Value {
        return Value::number(fmath::atan2(vm.toNumber(args[0], "Math.atan2() argument"), vm.toNumber(args[1], "Math.atan2() argument")));
    });
    def("hypot", 2, 2, [](VM& vm, int, Value* args) -> Value {
        return Value::number(fmath::hypot(vm.toNumber(args[0], "Math.hypot() argument"), vm.toNumber(args[1], "Math.hypot() argument")));
    });
    def("sign", 1, 1, [](VM& vm, int, Value* args) -> Value {
        args[0] = asWideInt(args[0]);   // a byte is an int here
        if (args[0].isInt()) return Value::integer((args[0].as.i > 0) - (args[0].as.i < 0));
        const double d = vm.toNumber(args[0], "Math.sign() argument");
        return Value::number(d > 0 ? 1.0 : d < 0 ? -1.0 : d);
    });

    vm.defineGlobal("Math", Value::object(math));
}

// ---- File ----

// ---- String methods ----

void registerStringMethods(VM& vm) {
    const BuiltinType T = BuiltinType::String;

    vm.defineMethod(T, "upper", 0, 0, [](VM& vm, Value self, int, Value*) -> Value {
        return vm.str(changeCase(asString(self)->utf8(), true));
    });

    vm.defineMethod(T, "lower", 0, 0, [](VM& vm, Value self, int, Value*) -> Value {
        return vm.str(changeCase(asString(self)->utf8(), false));
    });

    vm.defineMethod(T, "trim", 0, 0, [](VM& vm, Value self, int, Value*) -> Value {
        return vm.str(std::string(trimmed(asString(self)->utf8())));
    });

    // For text written over several lines in the source: trimIndent takes away the indentation
    // the lines share, and trimMargin everything up to and including a marker ("|" unless
    // another is given). Both drop a first and a last line that are blank.
    vm.defineMethod(T, "trimIndent", 0, 0, [](VM& vm, Value self, int, Value*) -> Value {
        std::vector<std::string> lines = splitLines(asString(self)->utf8());
        size_t indent = std::string::npos;
        for (const std::string& line : lines) {
            size_t first = line.find_first_not_of(" \t");
            if (first != std::string::npos) indent = std::min(indent, first);
        }
        std::string out;
        for (size_t i = 0; i < lines.size(); i++) {
            const bool blank = lines[i].find_first_not_of(" \t") == std::string::npos;
            if (blank && (i == 0 || i + 1 == lines.size())) continue;
            if (!out.empty() || i > 1 || (i == 1 && lines[0].find_first_not_of(" \t") != std::string::npos)) out += '\n';
            if (!blank) out += lines[i].substr(indent == std::string::npos ? 0 : indent);
        }
        return vm.str(std::move(out));
    });

    vm.defineMethod(T, "trimMargin", 0, 1, [](VM& vm, Value self, int argc, Value* args) -> Value {
        const std::string marker = argc > 0 ? vm.toStr(args[0], "margin marker") : std::string("|");
        std::vector<std::string> lines = splitLines(asString(self)->utf8());
        std::string out;
        bool any = false;
        for (size_t i = 0; i < lines.size(); i++) {
            const size_t first = lines[i].find_first_not_of(" \t");
            const bool blank = first == std::string::npos;
            if (blank && (i == 0 || i + 1 == lines.size())) continue;
            if (any) out += '\n';
            any = true;
            if (!blank && !marker.empty() && lines[i].compare(first, marker.size(), marker) == 0) out += lines[i].substr(first + marker.size());
            else out += lines[i];
        }
        return vm.str(std::move(out));
    });

    vm.defineMethod(T, "split", 0, 1, [](VM& vm, Value self, int argc, Value* args) -> Value {
        if (argc > 0 && args[0].isChar()) args[0] = vm.characterString(static_cast<uint32_t>(args[0].as.i));  // split(',') (D25)
        // On the characters themselves, each part at the width it needs (D20).
        const ObjString* text = asString(self);
        const size_t length = text->length;
        std::vector<Value> parts;

        if (argc == 0) {
            // No separator: split on runs of whitespace.
            auto space = [text](size_t i) {
                const uint32_t c = text->at(i);
                return c < 0x80 && std::isspace(static_cast<int>(c));
            };
            size_t i = 0;
            while (i < length) {
                while (i < length && space(i)) i++;
                size_t begin = i;
                while (i < length && !space(i)) i++;
                if (i > begin) parts.push_back(vm.str(text->slice(begin, i)));
            }
        } else {
            const ObjString* separator = vm.toText(args[0], "separator");
            if (separator->length == 0) {
                for (size_t i = 0; i < length; i++) parts.push_back(vm.characterString(text->at(i)));
            } else {
                size_t begin = 0;
                for (;;) {
                    const int64_t at = text->find(separator, begin);
                    if (at < 0) break;
                    parts.push_back(vm.str(text->slice(begin, static_cast<size_t>(at))));
                    begin = static_cast<size_t>(at) + separator->length;
                }
                parts.push_back(vm.str(text->slice(begin, length)));
            }
        }
        return Value::object(newList(vm, std::move(parts)));
    });


    vm.defineMethod(T, "repeat", 1, 1, [](VM& vm, Value self, int, Value* args) -> Value {
        const std::string& s = asString(self)->utf8();
        int64_t times = vm.toInt(args[0], "repeat count");
        if (times < 0) throw vm.runtimeError(Thrown::IllegalArgumentException, "repeat count cannot be negative");
        // As large as memory allows; a size past what can be counted cannot be asked for at all.
        if (!s.empty() && static_cast<uint64_t>(times) > SIZE_MAX / s.size()) throw vm.outOfMemory();
        vm.checkRoomFor(static_cast<uint64_t>(times) * s.size());
        std::string result;
        result.reserve(s.size() * static_cast<size_t>(times));
        for (int64_t i = 0; i < times; i++) result += s;
        return vm.str(std::move(result));
    });
}

// ---- List methods ----

void registerListMethods(VM& vm) {
    const BuiltinType T = BuiltinType::List;

    vm.defineMethod(T, "removeAt", 1, 1, [](VM& vm, Value self, int, Value* args) -> Value {
        ObjList* list = asList(self);
        size_t index;
        if (!vm.position(args[0], list->size(), index, "list index")) throw vm.outOfRange("list index", args[0], list->size());
        const Value removed = vm.listGet(list, index);
        vm.listErase(list, index, 1);
        return removed;
    });

    vm.defineMethod(T, "remove", 1, 1, [](VM& vm, Value self, int, Value* args) -> Value {
        ObjList* list = asList(self);
        for (size_t i = 0; i < list->size(); i++) {
            if (vm.equals(vm.listGet(list, i), args[0])) {
                vm.listErase(list, i, 1);
                return Value::boolean(true);
            }
        }
        return Value::boolean(false);
    });

    vm.defineMethod(T, "clear", 0, 0, [](VM& vm, Value self, int, Value*) -> Value {
        vm.listClear(asList(self));
        return Value::null();
    });

    vm.defineMethod(T, "contains", 1, 1, [](VM& vm, Value self, int, Value* args) -> Value {
        // By index: comparing may run an object's own == operator, which may change the list.
        ObjList* list = asList(self);
        for (size_t i = 0; i < list->size(); i++)
            if (vm.equals(vm.listGet(list, i), args[0])) return Value::boolean(true);
        return Value::boolean(false);
    });

    vm.defineMethod(T, "indexOf", 1, 1, [](VM& vm, Value self, int, Value* args) -> Value {
        ObjList* list = asList(self);
        for (size_t i = 0; i < list->size(); i++)
            if (vm.equals(vm.listGet(list, i), args[0])) return Value::integer(static_cast<int64_t>(i));
        return Value::integer(-1);
    });


    // The items as text (Kotlin's joinToString, D74): a separator between them, a prefix and a
    // postfix around them, at most `limit` of them and then `truncated`, each as `transform`
    // makes it or as print shows it.
    vm.defineMethod(T, "join", 0, 6, [](VM& vm, Value self, int argc, Value* args) -> Value {
        ObjList* list = asList(self);
        auto given = [&](int i) { return i < argc && !args[i].isNull(); };
        // Copied before any call: running the transform may move the stack the arguments are on.
        const std::string separator = given(0) ? vm.toStr(args[0], "separator") : ", ";
        const std::string prefix = given(1) ? vm.toStr(args[1], "prefix") : "";
        const std::string postfix = given(2) ? vm.toStr(args[2], "postfix") : "";
        const int64_t limit = given(3) ? vm.toInt(args[3], "limit") : -1;
        const std::string truncated = given(4) ? vm.toStr(args[4], "truncated") : "...";
        const Value transform = given(5) ? args[5] : Value::null();
        std::string result = prefix;
        // Indexed and re-checked each time round: a toString() or the transform may change the list.
        for (size_t i = 0; i < list->size(); i++) {
            if (i > 0) result += separator;
            if (limit >= 0 && static_cast<int64_t>(i) >= limit) {
                result += truncated;
                break;
            }
            Value item = vm.listGet(list, i);
            TempRoot keep(vm, item);
            if (!transform.isNull()) item = vm.call(transform, 1, &item);
            TempRoot keepShown(vm, item);
            result += vm.stringify(item);
        }
        return vm.str(result + postfix);
    });

    vm.defineMethod(T, "reverse", 0, 0, [](VM& vm, Value self, int, Value*) -> Value {
        ObjList* list = asList(self);
        for (size_t i = 0, j = list->size(); i + 1 < j; i++, j--) vm.listSwap(list, i, j - 1);
        return self;
    });

    vm.defineMethod(T, "sort", 0, 1, [](VM& vm, Value self, int argc, Value* args) -> Value {
        ObjList* list = asList(self);
        // Sorts a copy, so a comparator that modifies the list cannot corrupt the sort. A packed
        // list's items are taken out once, sorted, and each written back once: the items move
        // once, however many comparisons there are (value-lists.html, section 9).
        std::vector<Value> items = vm.listCopy(list);
        TempVector keepAlive(vm, items);
        std::vector<Value> scratch(items.size());

        std::function<bool(const Value&, const Value&)> less;
        if (argc == 0 || args[0].isNull()) {
            less = [&vm](const Value& a, const Value& b) { return defaultLess(vm, a, b); };
        } else {
            Value comparator = args[0];
            less = [&vm, comparator](const Value& a, const Value& b) {
                Value pair[2] = {a, b};
                Value order = vm.call(comparator, 2, pair);
                if (!order.isNumber()) throw vm.runtimeError(Thrown::ClassCastException, "sort comparator must return a number, got " + vm.typeName(order));
                return order.toDouble() < 0;
            };
        }

        mergeSort(items, scratch, 0, items.size(), less);
        if (!list->packed()) {
            list->values() = std::move(items);
        } else {
            if (list->size() != items.size()) throw vm.runtimeError(Thrown::ConcurrentModificationException, "the list was changed while it was sorted");
            for (size_t i = 0; i < items.size(); i++) vm.listSet(list, i, items[i]);
        }
        return self;
    });

    // map, filter, reduce, each and find are in src/std/list.ww: a function given to a
    // method written in WinterWolf runs as part of the program, where a task can be set aside.
}

// ---- Map methods ----

void registerMapMethods(VM& vm) {
    const BuiltinType T = BuiltinType::Map;





    // The same under the name that `key in map` looks for.
    // A whole entry, `"a" to 1` or a MapEntry, with its key and its value (D140); a key is
    // asked of the keys.
    vm.defineMethod(T, "contains", 1, 1, [](VM& vm, Value self, int, Value* args) -> Value {
        const Value entry = args[0];
        const bool whole = entry.isObjType(ObjType::Value) || entry.isSmall() || entry.isRef();
        if (!whole)
            throw vm.runtimeError(Thrown::IllegalArgumentException, "map.contains(...) asks for a whole entry, (key to value); for a key write map.keys.contains(" +
                                  vm.repr(entry) + "), or " + vm.repr(entry) + " in map.keys");
        const Value key = vm.componentOf(entry, 0);
        TempRoot keepKey(vm, key);
        const Value value = vm.componentOf(entry, 1);
        const Value* held = asMap(self)->map.find(key);
        return Value::boolean(held != nullptr && vm.equals(*held, value));
    });

    vm.defineMethod(T, "get", 1, 2, [](VM&, Value self, int argc, Value* args) -> Value {
        const Value* found = asMap(self)->map.find(args[0]);
        if (found != nullptr) return *found;
        return argc > 1 ? args[1] : Value::null();
    });

    // Gives the value that was there, or null.
    vm.defineMethod(T, "remove", 1, 1, [](VM&, Value self, int, Value* args) -> Value {
        Value removed = Value::null();
        asMap(self)->map.remove(args[0], &removed);  // one lookup, one hashCode (G28)
        return removed;
    });

    vm.defineMethod(T, "clear", 0, 0, [](VM&, Value self, int, Value*) -> Value {
        asMap(self)->map.clear();
        return Value::null();
    });
}

// ---- Range methods ----

void registerRangeMethods(VM& vm) {
    const BuiltinType T = BuiltinType::Range;

    vm.defineMethod(T, "toList", 0, 0, [](VM& vm, Value self, int, Value*) -> Value {
        ObjRange* range = asRange(self);
        int64_t count = range->count();
        if (static_cast<uint64_t>(count) > SIZE_MAX / sizeof(Value)) throw vm.outOfMemory();
        vm.checkRoomFor(static_cast<uint64_t>(count) * sizeof(Value));
        std::vector<Value> items;
        items.reserve(static_cast<size_t>(count));
        uint64_t next = static_cast<uint64_t>(range->start);
        for (int64_t i = 0; i < count; i++) {
            items.push_back(Value::integer(static_cast<int64_t>(next)));
            next += static_cast<uint64_t>(range->step);
        }
        return Value::object(newList(vm, std::move(items)));
    });

    vm.defineMethod(T, "contains", 1, 1, [](VM&, Value self, int, Value* args) -> Value {
        // A byte is the number it holds (M7: `color.alpha in 1..254` was always false).
        return Value::boolean((args[0].isInt() || args[0].isByte()) && asRange(self)->contains(args[0].as.i));
    });
}

}  // namespace

// ---- bytes ----

// A byte given as one, or as an int from 0 to 255 by code that does not say its types.
uint8_t byteArgument(VM& vm, Value v) {
    if (v.isByte()) return static_cast<uint8_t>(v.as.i);
    if (!v.isInt() || v.as.i < 0 || v.as.i > 255) throw vm.runtimeError(Thrown::IllegalArgumentException, "a byte is a whole number from 0 to 255, not " + vm.stringify(v));
    return static_cast<uint8_t>(v.as.i);
}

// Where a number of `size` bytes starts in `data`, at `at`: an error past the end.
[[noreturn]] void binaryPastEnd(VM& vm, const std::vector<uint8_t>& data, int64_t i, size_t size) {
    throw vm.runtimeError(Thrown::IndexOutOfBoundsException, "a " + std::to_string(size * 8) + "-bit number at " + std::to_string(i) + " runs past the end of " +
                          std::to_string(data.size()) + " bytes");
}

inline size_t binaryAt(VM& vm, const std::vector<uint8_t>& data, Value at, size_t size) {
    const int64_t i = vm.toInt(at, "position");
    if (i < 0 || static_cast<uint64_t>(i) > data.size() || data.size() - static_cast<size_t>(i) < size) [[unlikely]]
        binaryPastEnd(vm, data, i, size);
    return static_cast<size_t>(i);
}

// `size` bytes from `data` at `at`, as one number, least significant byte first unless `big`.
uint64_t readBinary(const std::vector<uint8_t>& data, size_t at, size_t size, bool big) {
    uint64_t n = 0;
    for (size_t k = 0; k < size; k++) n |= static_cast<uint64_t>(data[at + (big ? size - 1 - k : k)]) << (8 * k);
    return n;
}

void writeBinary(std::vector<uint8_t>& data, size_t at, size_t size, bool big, uint64_t n) {
    for (size_t k = 0; k < size; k++) data[at + (big ? size - 1 - k : k)] = static_cast<uint8_t>(n >> (8 * k));
}

enum class Kind { Signed, Unsigned, Float };

template <size_t Size, Kind kind>
Value readNumber(VM& vm, Value self, int argc, Value* args) {
    const std::vector<uint8_t>& data = asBytes(self)->data;
    const uint64_t bits = readBinary(data, binaryAt(vm, data, args[0], Size), Size, argc > 1 && args[1].truthy());
    if constexpr (kind == Kind::Float && Size == 8) {
        double d;
        std::memcpy(&d, &bits, sizeof d);
        return Value::number(d);
    } else if constexpr (kind == Kind::Float) {
        float f;
        const uint32_t word = static_cast<uint32_t>(bits);
        std::memcpy(&f, &word, sizeof f);
        return Value::number(f);
    } else if constexpr (kind == Kind::Signed) {
        return Value::integer(Size == 2 ? static_cast<int16_t>(bits) : Size == 4 ? static_cast<int32_t>(bits) : static_cast<int64_t>(bits));
    } else {
        return Value::integer(static_cast<int64_t>(bits));
    }
}

template <size_t Size, Kind kind>
Value writeNumber(VM& vm, Value self, int argc, Value* args) {
    std::vector<uint8_t>& data = asBytes(self)->data;
    const size_t at = binaryAt(vm, data, args[0], Size);
    uint64_t bits;
    if constexpr (kind == Kind::Float && Size == 8) {
        bits = fmath::canonicalBits(vm.toNumber(args[1], "number"));  // every NaN the one NaN
    } else if constexpr (kind == Kind::Signed && Size == 8) {
        bits = static_cast<uint64_t>(vm.toInt(args[1], "number"));  // every int fits
    } else if constexpr (kind == Kind::Float) {
        bits = fmath::canonicalBits(static_cast<float>(vm.toNumber(args[1], "number")));
    } else {
        const int64_t n = vm.toInt(args[1], "number");
        const int64_t low = kind == Kind::Unsigned ? 0 : Size == 2 ? -32768 : -2147483648LL;
        const int64_t high = kind == Kind::Unsigned ? (Size == 2 ? 65535 : 4294967295LL) : Size == 2 ? 32767 : 2147483647;
        if (n < low || n > high)
            throw vm.runtimeError(Thrown::ArithmeticException, std::to_string(n) + " does not fit in " + std::to_string(Size * 8) + " bits " +
                                  (kind == Kind::Unsigned ? "unsigned" : "signed") + ", which hold " + std::to_string(low) + " to " +
                                  std::to_string(high));
        bits = static_cast<uint64_t>(n);
    }
    writeBinary(data, at, Size, argc > 2 && args[2].truthy(), bits);
    return self;
}

void registerBytes(VM& vm) {
    // bytes(16): sixteen zeros. bytes([1, 2, 255]) or bytes(other): a copy.
    vm.defineNative("Bytes", 1, 1, [](VM& vm, int, Value* args) -> Value {
        ObjBytes* made = vm.alloc<ObjBytes>();
        Value from = args[0];
        if (from.isInt()) {
            if (from.as.i < 0) throw vm.runtimeError(Thrown::IllegalArgumentException, "bytes() cannot make a negative number of bytes");
            vm.checkRoomFor(static_cast<uint64_t>(from.as.i));
            made->data.assign(static_cast<size_t>(from.as.i), 0);
        } else if (from.isObjType(ObjType::List)) {
            for (Value item : vm.listCopy(asList(from))) made->data.push_back(byteArgument(vm, item));
        } else if (from.isObjType(ObjType::Bytes)) {
            made->data = asBytes(from)->data;
        } else {
            throw vm.runtimeError(Thrown::IllegalArgumentException, "bytes() takes a size, a list of numbers or other bytes, not a value of type " + vm.typeName(from) +
                                  (from.isString() ? "; text becomes bytes with toBytes()" : ""));
        }
        vm.account(made->data.size());
        return Value::object(made);
    });

    const BuiltinType T = BuiltinType::Bytes;


    vm.defineMethod(T, "toList", 0, 0, [](VM& vm, Value self, int, Value*) -> Value {
        std::vector<Value> items;
        for (uint8_t b : asBytes(self)->data) items.push_back(Value::byte(b));
        return Value::object(newList(vm, std::move(items)));
    });

    // The text these bytes spell, in UTF-8 or the character set given (D127).
    vm.defineMethod(T, "decodeToString", 0, 1, [](VM& vm, Value self, int argc, Value* args) -> Value {
        const std::vector<uint8_t>& data = asBytes(self)->data;
        const Charset cs = charsetOf(vm, argc > 0 ? args[0] : Value::null());
        const std::string_view raw(reinterpret_cast<const char*>(data.data()), data.size());
        return vm.str(cs == Charset::Utf8 ? std::string(raw) : decodeText(raw, cs));
    });

    vm.defineMethod(T, "fill", 1, 1, [](VM& vm, Value self, int, Value* args) -> Value {
        const uint8_t value = byteArgument(vm, args[0]);
        std::vector<uint8_t>& data = asBytes(self)->data;
        std::fill(data.begin(), data.end(), value);
        return self;
    });

    // The first place of a byte at or after `startIndex` (a negative one taken as 0), by the
    // system's memchr: the native byte search line ends are found with (LineIndex).
    vm.defineMethod(T, "indexOf", 1, 2, [](VM& vm, Value self, int argc, Value* args) -> Value {
        const std::vector<uint8_t>& data = asBytes(self)->data;
        const int64_t start = argc > 1 && !args[1].isNull() ? std::max<int64_t>(0, vm.toInt(args[1], "startIndex")) : 0;
        if ((!args[0].isInt() && !args[0].isByte()) || args[0].as.i < 0 || args[0].as.i > 255 || start >= static_cast<int64_t>(data.size()))
            return Value::integer(-1);
        const void* found = std::memchr(data.data() + start, static_cast<int>(args[0].as.i), data.size() - static_cast<size_t>(start));
        return Value::integer(found == nullptr ? -1 : static_cast<const uint8_t*>(found) - data.data());
    });

    // Every place of a byte from `startIndex` up to `endIndex` (not included), in order, as a
    // List<int>: a buffer's line ends found in one call (LineIndex), where a loop of indexOf
    // pays for a call at each. Places outside the bytes are taken as their ends.
    vm.defineMethod(T, "indicesOf", 1, 3, [](VM& vm, Value self, int argc, Value* args) -> Value {
        const int64_t size = static_cast<int64_t>(asBytes(self)->data.size());
        const int64_t start = argc > 1 && !args[1].isNull() ? std::clamp<int64_t>(vm.toInt(args[1], "startIndex"), 0, size) : 0;
        const int64_t end = argc > 2 && !args[2].isNull() ? std::clamp<int64_t>(vm.toInt(args[2], "endIndex"), start, size) : size;
        const bool searched = (args[0].isInt() || args[0].isByte()) && args[0].as.i >= 0 && args[0].as.i <= 255;
        const uint8_t value = static_cast<uint8_t>(args[0].as.i);
        const size_t count = searched ? bytePlaces(asBytes(self)->data.data(), start, end, value, nullptr) : 0;
        ObjList* list = newList(vm);
        TempRoot keep(vm, Value::object(list));
        vm.typeList(list, vm.intListType());
        if (count == 0) return Value::object(list);
        if (list->store() == ListStore::Ints) {
            list->pack->block.resize((count + 4) * sizeof(int64_t));  // room for what bytePlaces writes past the last
            bytePlaces(asBytes(self)->data.data(), start, end, value, reinterpret_cast<int64_t*>(list->pack->block.data()));
            list->pack->block.resize(count * sizeof(int64_t));
            list->pack->count = count;
            vm.account(list->pack->block.size());
        } else {
            std::vector<int64_t> places(count + 4);
            bytePlaces(asBytes(self)->data.data(), start, end, value, places.data());
            for (int64_t place : places) vm.listAdd(list, Value::integer(place));
        }
        return Value::object(list);
    });

    vm.defineMethod(T, "contains", 1, 1, [](VM&, Value self, int, Value* args) -> Value {
        const std::vector<uint8_t>& data = asBytes(self)->data;
        return Value::boolean((args[0].isInt() || args[0].isByte()) && std::find(data.begin(), data.end(), args[0].as.i) != data.end());
    });

    // Each byte b made table[b], into new bytes: a table of 256 that turns bytes into letters
    // (Base64), letters into their values, or one character set's codes into another's. With a
    // `width`, each byte made `width` bytes, table[b * width] on: a palette's index made its colour.
    vm.defineMethod(T, "mapped", 1, 2, [](VM& vm, Value self, int argc, Value* args) -> Value {
        const int64_t width = argc > 1 && !args[1].isNull() ? vm.toInt(args[1], "width") : 1;
        if (width < 1 || width > 64) throw vm.runtimeError(Thrown::IllegalArgumentException, "mapped() takes a width from 1 to 64, not " + std::to_string(width));
        if (!args[0].isObjType(ObjType::Bytes) || asBytes(args[0])->data.size() != static_cast<size_t>(256 * width))
            throw vm.runtimeError(Thrown::IllegalArgumentException, width == 1 ? std::string("mapped() takes a table of 256 bytes, one for each value a byte has")
                                                                               : "mapped() with a width of " + std::to_string(width) + " takes a table of " +
                                                                                     std::to_string(256 * width) + " bytes, " + std::to_string(width) + " for each value a byte has");
        const size_t n = asBytes(self)->data.size();
        const size_t w = static_cast<size_t>(width);
        vm.checkRoomFor(n * w);
        ObjBytes* made = vm.alloc<ObjBytes>();
        TempRoot keep(vm, Value::object(made));
        made->data.resize(n * w);
        const uint8_t* table = asBytes(args[0])->data.data();
        const uint8_t* from = asBytes(self)->data.data();
        uint8_t* to = made->data.data();
        if (w == 1) {
            for (size_t i = 0; i < n; i++) to[i] = table[from[i]];
        } else if (w == 4) {
            for (size_t i = 0; i < n; i++) std::memcpy(to + i * 4, table + from[i] * 4, 4);
        } else {
            for (size_t i = 0; i < n; i++) std::memcpy(to + i * w, table + from[i] * w, w);
        }
        vm.account(n * w);
        return Value::object(made);
    });

    // `count` bytes of `source` copied in, from `sourceIndex` on a step of `sourceStep` (0, the
    // same byte each time) to `index` on a step of `step`: one channel of interleaved pixels into
    // another's place, a plane into pixels, a byte into every fourth place. Gives the bytes
    // themselves. An IndexOutOfBoundsException where either side would go outside its bytes.
    vm.defineMethod(T, "copyFrom", 6, 6, [](VM& vm, Value self, int, Value* args) -> Value {
        if (!args[0].isObjType(ObjType::Bytes)) throw vm.runtimeError(Thrown::IllegalArgumentException, "copyFrom() copies from Bytes, not a value of type " + vm.typeName(args[0]));
        const int64_t sourceIndex = vm.toInt(args[1], "sourceIndex"), sourceStep = vm.toInt(args[2], "sourceStep");
        const int64_t index = vm.toInt(args[3], "index"), step = vm.toInt(args[4], "step"), count = vm.toInt(args[5], "count");
        const std::vector<uint8_t>& from = asBytes(args[0])->data;
        std::vector<uint8_t>& to = asBytes(self)->data;
        if (count < 0 || sourceStep < 0 || step < 1) throw vm.runtimeError(Thrown::IllegalArgumentException, "copyFrom() takes a count and a source step of 0 or more and a step of 1 or more");
        if (count == 0) return self;
        const int64_t lastFrom = sourceIndex + sourceStep * (count - 1), lastTo = index + step * (count - 1);
        if (sourceIndex < 0 || lastFrom >= static_cast<int64_t>(from.size()))
            throw vm.runtimeError(Thrown::IndexOutOfBoundsException, "copyFrom() reads " + std::to_string(sourceIndex) + " to " + std::to_string(lastFrom) + " of " + std::to_string(from.size()) + " bytes");
        if (index < 0 || lastTo >= static_cast<int64_t>(to.size()))
            throw vm.runtimeError(Thrown::IndexOutOfBoundsException, "copyFrom() writes " + std::to_string(index) + " to " + std::to_string(lastTo) + " of " + std::to_string(to.size()) + " bytes");
        const uint8_t* s = from.data() + sourceIndex;
        uint8_t* d = to.data() + index;
        const size_t n = static_cast<size_t>(count), ss = static_cast<size_t>(sourceStep), ds = static_cast<size_t>(step);
        if (ss == 1 && ds == 1) {
            std::memmove(d, s, n);
        } else if (ss == 0) {
            const uint8_t value = *s;
            for (size_t i = 0; i < n; i++) d[i * ds] = value;
        } else {
            for (size_t i = 0; i < n; i++) d[i * ds] = s[i * ss];
        }
        return self;
    });

    // Each byte b made table[b] where it is, and the bytes themselves given back: mapped without
    // new bytes made, for bytes that are not wanted as they were.
    vm.defineMethod(T, "mapInPlace", 1, 1, [](VM& vm, Value self, int, Value* args) -> Value {
        if (!args[0].isObjType(ObjType::Bytes) || asBytes(args[0])->data.size() != 256)
            throw vm.runtimeError(Thrown::IllegalArgumentException, "mapInPlace() takes a table of 256 bytes, one for each value a byte has");
        const uint8_t* table = asBytes(args[0])->data.data();
        std::vector<uint8_t>& data = asBytes(self)->data;
        uint8_t* at = data.data();
        const size_t n = data.size();
        for (size_t i = 0; i < n; i++) at[i] = table[at[i]];
        return self;
    });

    // The bits in groups of `width` (1 to 8), the most significant first, each group a byte of
    // its own: 6 for Base64's letters, 1, 2 or 4 for the pixels of a picture of few colours. A
    // last group the bits do not fill is filled with zeros.
    vm.defineMethod(T, "unpackBits", 1, 1, [](VM& vm, Value self, int, Value* args) -> Value {
        const int64_t width = vm.toInt(args[0], "width");
        if (width < 1 || width > 8) throw vm.runtimeError(Thrown::IllegalArgumentException, "unpackBits() takes a width from 1 to 8, not " + std::to_string(width));
        const size_t n = asBytes(self)->data.size();
        const size_t count = (n * 8 + static_cast<size_t>(width) - 1) / static_cast<size_t>(width);
        vm.checkRoomFor(count);
        ObjBytes* made = vm.alloc<ObjBytes>();
        TempRoot keep(vm, Value::object(made));
        made->data.resize(count);
        const uint8_t* from = asBytes(self)->data.data();
        uint8_t* to = made->data.data();
        size_t i = 0, o = 0;
        if (width == 8) {
            std::memcpy(to, from, n);
            o = n;
            i = n;
        } else if (width == 6) {   // three bytes to four groups at a time
            for (; i + 3 <= n; i += 3, o += 4) {
                const uint32_t word = (uint32_t{from[i]} << 16) | (uint32_t{from[i + 1]} << 8) | from[i + 2];
                to[o] = static_cast<uint8_t>(word >> 18);
                to[o + 1] = static_cast<uint8_t>((word >> 12) & 63);
                to[o + 2] = static_cast<uint8_t>((word >> 6) & 63);
                to[o + 3] = static_cast<uint8_t>(word & 63);
            }
        } else if (8 % width == 0) {   // 1, 2, 4: whole groups in each byte
            const int per = 8 / static_cast<int>(width);
            const uint8_t mask = static_cast<uint8_t>((1u << width) - 1);
            for (; i < n; i++)
                for (int k = per - 1; k >= 0; k--) to[o++] = static_cast<uint8_t>((from[i] >> (k * width)) & mask);
        }
        // What is left, a bit at a time through a store of bits.
        uint32_t store = 0;
        int have = 0;
        const uint32_t mask = (1u << width) - 1;
        for (; i < n; i++) {
            store = (store << 8) | from[i];
            have += 8;
            while (have >= width) {
                have -= static_cast<int>(width);
                to[o++] = static_cast<uint8_t>((store >> have) & mask);
            }
            store &= (1u << have) - 1;
        }
        if (have > 0) to[o++] = static_cast<uint8_t>((store << (width - have)) & mask);
        vm.account(count);
        return Value::object(made);
    });

    // The reverse of unpackBits: the low `width` bits (1 to 8) of each byte from `startIndex` up
    // to `endIndex`, the first the most significant, joined into whole bytes; bits left over that
    // make no whole byte are dropped.
    vm.defineMethod(T, "packBits", 1, 3, [](VM& vm, Value self, int argc, Value* args) -> Value {
        const int64_t width = vm.toInt(args[0], "width");
        if (width < 1 || width > 8) throw vm.runtimeError(Thrown::IllegalArgumentException, "packBits() takes a width from 1 to 8, not " + std::to_string(width));
        const int64_t size = static_cast<int64_t>(asBytes(self)->data.size());
        const int64_t first = argc > 1 && !args[1].isNull() ? vm.toInt(args[1], "startIndex") : 0;
        const int64_t last = argc > 2 && !args[2].isNull() ? vm.toInt(args[2], "endIndex") : size;
        if (first < 0 || last > size || first > last)
            throw vm.runtimeError(Thrown::IndexOutOfBoundsException, "packBits() from " + std::to_string(first) + " to " + std::to_string(last) +
                                  " is out of bounds of " + std::to_string(size) + " bytes");
        const size_t n = static_cast<size_t>(last - first);
        const size_t count = n * static_cast<size_t>(width) / 8;
        vm.checkRoomFor(count);
        ObjBytes* made = vm.alloc<ObjBytes>();
        TempRoot keep(vm, Value::object(made));
        made->data.resize(count);
        const uint8_t* from = asBytes(self)->data.data() + first;
        uint8_t* to = made->data.data();
        size_t i = 0, o = 0;
        const uint32_t mask = (1u << width) - 1;
        if (width == 8) {
            std::memcpy(to, from, n);
            i = n;
            o = n;
        } else if (width == 6) {   // four groups to three bytes at a time
            for (; i + 4 <= n; i += 4, o += 3) {
                const uint32_t word = ((from[i] & 63u) << 18) | ((from[i + 1] & 63u) << 12) | ((from[i + 2] & 63u) << 6) | (from[i + 3] & 63u);
                to[o] = static_cast<uint8_t>(word >> 16);
                to[o + 1] = static_cast<uint8_t>(word >> 8);
                to[o + 2] = static_cast<uint8_t>(word);
            }
        }
        uint32_t store = 0;
        int have = 0;
        for (; i < n && o < count; i++) {
            store = (store << width) | (from[i] & mask);
            have += static_cast<int>(width);
            if (have >= 8) {
                have -= 8;
                to[o++] = static_cast<uint8_t>(store >> have);
                store &= (1u << have) - 1;
            }
        }
        vm.account(count);
        return Value::object(made);
    });

    // New bytes with every byte equal to `value` taken out: the line breaks of text, say.
    vm.defineMethod(T, "without", 1, 1, [](VM& vm, Value self, int, Value* args) -> Value {
        const uint8_t value = byteArgument(vm, args[0]);
        const size_t n = asBytes(self)->data.size();
        vm.checkRoomFor(n);
        ObjBytes* made = vm.alloc<ObjBytes>();
        TempRoot keep(vm, Value::object(made));
        const std::vector<uint8_t>& from = asBytes(self)->data;
        made->data.resize(n);
        const uint8_t* source = from.data();
        uint8_t* to = made->data.data();
        size_t kept = 0, at = 0;
        while (at < n) {
            // A stretch with none of `value` found at once and copied whole (the line breaks of
            // text, far apart)...
            const void* hit = std::memchr(source + at, value, n - at);
            const size_t end = hit == nullptr ? n : static_cast<size_t>(static_cast<const uint8_t*>(hit) - source);
            std::memcpy(to + kept, source + at, end - at);
            kept += end - at;
            if (end == n) break;
            // ...and after one, a stretch where they may be close together gone through a byte at a
            // time with no branch: every alpha 255 of an opaque picture took a call each before
            // (2026-11-17: 30 ms for 12 million).
            const size_t stop = std::min(n, end + 256);
            for (size_t i = end; i < stop; i++) {
                to[kept] = source[i];
                kept += source[i] != value ? 1 : 0;
            }
            at = stop;
        }
        made->data.resize(kept);
        made->data.shrink_to_fit();
        vm.account(made->data.size());
        return Value::object(made);
    });


    // 16-, 32- and 64-bit numbers in the bytes (D19, L0; 64-bit since M8): read as an int (a float for float32) and
    // written, each at a position, least significant byte first unless `bigEndian`.
    vm.defineMethod(T, "int16", 1, 2, readNumber<2, Kind::Signed>);
    vm.defineMethod(T, "uint16", 1, 2, readNumber<2, Kind::Unsigned>);
    vm.defineMethod(T, "int32", 1, 2, readNumber<4, Kind::Signed>);
    vm.defineMethod(T, "uint32", 1, 2, readNumber<4, Kind::Unsigned>);
    vm.defineMethod(T, "float32", 1, 2, readNumber<4, Kind::Float>);
    vm.defineMethod(T, "int64", 1, 2, readNumber<8, Kind::Signed>);
    vm.defineMethod(T, "float64", 1, 2, readNumber<8, Kind::Float>);
    vm.defineMethod(T, "putInt16", 2, 3, writeNumber<2, Kind::Signed>);
    vm.defineMethod(T, "putUint16", 2, 3, writeNumber<2, Kind::Unsigned>);
    vm.defineMethod(T, "putInt32", 2, 3, writeNumber<4, Kind::Signed>);
    vm.defineMethod(T, "putUint32", 2, 3, writeNumber<4, Kind::Unsigned>);
    vm.defineMethod(T, "putFloat32", 2, 3, writeNumber<4, Kind::Float>);
    vm.defineMethod(T, "putInt64", 2, 3, writeNumber<8, Kind::Signed>);
    vm.defineMethod(T, "putFloat64", 2, 3, writeNumber<8, Kind::Float>);

    // Text as bytes, in UTF-8 or the character set given (D127).
    vm.defineMethod(BuiltinType::String, "toBytes", 0, 1, [](VM& vm, Value self, int argc, Value* args) -> Value {
        const Charset cs = charsetOf(vm, argc > 0 ? args[0] : Value::null());
        const std::string& utf8 = asString(self)->utf8();
        ObjBytes* made = vm.alloc<ObjBytes>();
        if (cs == Charset::Utf8) {
            made->data.assign(utf8.begin(), utf8.end());
        } else {
            const std::string s = encodeText(utf8, cs);
            made->data.assign(s.begin(), s.end());
        }
        vm.account(made->data.size());
        return Value::object(made);
    });
}

void registerBuiltins(VM& vm) {
    registerGlobals(vm);
    registerMath(vm);
    registerStringMethods(vm);
    registerListMethods(vm);
    registerMapMethods(vm);
    registerRangeMethods(vm);
    registerBytes(vm);
    registerStdlib(vm);
    registerReflection(vm);
    registerThreads(vm);
    registerIo(vm);
    registerSecureRandom(vm);
    registerCrypto(vm);
    registerSandbox(vm);
    registerGrid(vm);
    registerRegex(vm);
    registerXml(vm);
    registerBits(vm);
    registerPicture(vm);
    registerNumeric(vm);
    registerTensor(vm);
    registerWebp(vm);
    registerForeign(vm);
    registerWorkers(vm);
    registerParallel(vm);
    // Scripts compiled as the program runs: where the compiler side is in the file (D161).
    if (compilerCodePresent()) registerScripts(vm);
    // A built program that reaches neither Jade nor the text engine has their code left out of
    // its file (M6.5 part 5): their names are then not there either.
    if (windowCodePresent()) {
        registerText(vm);
        // Windows belong to the main program: a worker has none.
        if (!vm.isWorker()) registerJade(vm);
    }

    // For parallel sections: which methods change what they are called on, and which
    // built-in functions wait, or work on something the whole program has one of.
    vm.markMutating(BuiltinType::List, {"removeAt", "remove", "clear", "reverse", "sort", "add", "addAll", "set", "ensureCapacity",
                                        "removeFirst", "removeLast", "removeFirstOrNull", "removeLastOrNull", "fill", "sortBy",
                                        "sortByDescending", "replaceAll"});
    vm.markMutating(BuiltinType::Map, {"remove", "clear", "put", "set"});
    vm.markMutating(BuiltinType::Set, {"add", "remove", "clear", "addAll", "removeAll", "retainAll"});
    // Given an item of a list of values where it is, `add` copies it in at once (M6.6 part 5).
    vm.markTakesPlaces(BuiltinType::List, {"add"});
    vm.markMutating(BuiltinType::Bytes, {"fill", "mapInPlace", "copyFrom", "putInt16", "putUint16", "putInt32", "putUint32", "putFloat32"});
    for (const char* global : {"__thread", "__loop", "__io", "__file", "__worker", "input", "exit"})
        vm.markNotParallel(global);
    vm.markNotParallel("__task", {"arity"});      // how many parameters a function has: map.forEach asks
}

// Op::Numeric (M6.6 part 6): a number's conversion or one of Math's functions, giving what the
// standard library's own gives, errors included; the compiler chose it for the arguments' types.
Value VM::numeric(uint8_t which, const Value* args) {
    const Value a = args[0].isByte() ? Value::integer(args[0].as.i) : args[0];   // a byte is an int here
    switch (static_cast<NumericOp>(which)) {
        case NumericOp::ToByte:
        case NumericOp::ToByteWrapped: {
            const bool wrapped = which == static_cast<uint8_t>(NumericOp::ToByteWrapped);
            if (!args[0].isInt())
                throw runtimeError(Thrown::ClassCastException, std::string(wrapped ? "toByteWrapped" : "toByte") + " is for ints, not " + typeName(args[0]));
            const int64_t n = args[0].as.i;
            if (!wrapped && (n < 0 || n > 255))
                throw runtimeError(Thrown::IllegalArgumentException, std::to_string(n) + " does not fit in a byte, which holds 0 to 255; toByteWrapped() keeps the low 8 bits");
            return Value::byte(static_cast<uint8_t>(n));
        }
        case NumericOp::ToInt: return a.isInt() ? a : Value::integer(floatToInt(*this, std::trunc(toNumber(a, "the number"))));
        case NumericOp::ToFloat: return Value::number(toNumber(a, "the number"));
        case NumericOp::RoundToInt:
            if (a.isInt()) return a;
            if (std::isnan(toNumber(a, "the number"))) throw runtimeError(Thrown::ArithmeticException, "cannot round NaN to a whole number");
            return Value::integer(floatToInt(*this, std::floor(a.as.d + 0.5)));
        case NumericOp::IsNaN: return Value::boolean(a.isFloat() && std::isnan(a.as.d));
        case NumericOp::IsInfinite: return Value::boolean(a.isFloat() && std::isinf(a.as.d));
        case NumericOp::IsFinite: return Value::boolean(a.isInt() || std::isfinite(toNumber(a, "the number")));
        case NumericOp::Sqrt: return Value::number(std::sqrt(toNumber(a, "Math.sqrt() argument")));
        case NumericOp::Floor:
            return a.isInt() ? a : Value::integer(floatToInt(*this, std::floor(toNumber(a, "Math.floor() argument"))));
        case NumericOp::Ceil:
            return a.isInt() ? a : Value::integer(floatToInt(*this, std::ceil(toNumber(a, "Math.ceil() argument"))));
        case NumericOp::Round:
            return a.isInt() ? a : Value::integer(floatToInt(*this, std::round(toNumber(a, "Math.round() argument"))));
        case NumericOp::AbsInt:
        case NumericOp::AbsFloat:
            if (a.isInt()) {
                if (a.as.i == INT64_MIN) throw overflowError(Op::Sub, 0, a.as.i);  // (D27)
                return Value::integer(a.as.i < 0 ? -a.as.i : a.as.i);
            }
            return Value::number(std::fabs(toNumber(a, "Math.abs() argument")));
        case NumericOp::MinInt:
        case NumericOp::MinFloat: {
            const Value b = args[1].isByte() ? Value::integer(args[1].as.i) : args[1];
            const double x = toNumber(a, "Math.min() argument"), y = toNumber(b, "Math.min() argument");
            return y < x ? b : a;
        }
        case NumericOp::MaxInt:
        case NumericOp::MaxFloat: {
            const Value b = args[1].isByte() ? Value::integer(args[1].as.i) : args[1];
            const double x = toNumber(a, "Math.max() argument"), y = toNumber(b, "Math.max() argument");
            return y > x ? b : a;
        }
        default: throw runtimeError(Thrown::InternalError, "invalid bytecode: unknown numeric operation");
    }
}

}  // namespace ww
