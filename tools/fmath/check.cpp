// The functions of src/fmath.cpp on the inputs given, for tools/fmath/check.py: each line
// "name xbits [ybits]" (bits as 16 hex digits) answered with the result's bits. With "speed" as
// its argument it times each against the system's own function instead.
//
//   Windows: clang-cl /O2 /std:c++20 /EHsc /I src tools\fmath\check.cpp src\fmath.cpp /Fe:fmath_check.exe
//   Linux:   clang++ -O2 -std=c++20 -I src tools/fmath/check.cpp src/fmath.cpp -o fmath_check
#include "fmath.h"

#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

using namespace ww;

static double fromBits(uint64_t b) {
    double x;
    std::memcpy(&x, &b, 8);
    return x;
}

static uint64_t bitsOf(double x) {
    uint64_t b;
    std::memcpy(&b, &x, 8);
    return b;
}

static double call(const std::string& name, double x, double y, bool own) {
    if (name == "sin") return own ? fmath::sin(x) : std::sin(x);
    if (name == "cos") return own ? fmath::cos(x) : std::cos(x);
    if (name == "tan") return own ? fmath::tan(x) : std::tan(x);
    if (name == "asin") return own ? fmath::asin(x) : std::asin(x);
    if (name == "acos") return own ? fmath::acos(x) : std::acos(x);
    if (name == "atan") return own ? fmath::atan(x) : std::atan(x);
    if (name == "atan2") return own ? fmath::atan2(x, y) : std::atan2(x, y);
    if (name == "exp") return own ? fmath::exp(x) : std::exp(x);
    if (name == "expm1") return own ? fmath::expm1(x) : std::expm1(x);
    if (name == "log") return own ? fmath::log(x) : std::log(x);
    if (name == "log2") return own ? fmath::log2(x) : std::log2(x);
    if (name == "log10") return own ? fmath::log10(x) : std::log10(x);
    if (name == "log1p") return own ? fmath::log1p(x) : std::log1p(x);
    if (name == "logbase") return own ? fmath::logBase(x, y) : std::log(x) / std::log(y);
    if (name == "sinh") return own ? fmath::sinh(x) : std::sinh(x);
    if (name == "cosh") return own ? fmath::cosh(x) : std::cosh(x);
    if (name == "tanh") return own ? fmath::tanh(x) : std::tanh(x);
    if (name == "asinh") return own ? fmath::asinh(x) : std::asinh(x);
    if (name == "acosh") return own ? fmath::acosh(x) : std::acosh(x);
    if (name == "atanh") return own ? fmath::atanh(x) : std::atanh(x);
    if (name == "pow") return own ? fmath::pow(x, y) : std::pow(x, y);
    if (name == "hypot") return own ? fmath::hypot(x, y) : std::hypot(x, y);
    std::fprintf(stderr, "unknown function %s\n", name.c_str());
    return 0;
}

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "speed") {
        using Fn = double (*)(double, double);
        struct Pair {
            const char* name;
            Fn system, own;
        };
#define WW_PAIR(NAME) {#NAME, [](double x, double) { return std::NAME(x); }, [](double x, double) { return fmath::NAME(x); }}
#define WW_PAIR2(NAME) {#NAME, [](double x, double y) { return std::NAME(x, y); }, [](double x, double y) { return fmath::NAME(x, y); }}
        const Pair pairs[] = {WW_PAIR(sin),   WW_PAIR(cos),   WW_PAIR(tan),   WW_PAIR(asin),  WW_PAIR(acos),   WW_PAIR(atan),
                              WW_PAIR2(atan2), WW_PAIR(exp),  WW_PAIR(expm1), WW_PAIR(log),   WW_PAIR(log2),   WW_PAIR(log10),
                              WW_PAIR(log1p), WW_PAIR(sinh),  WW_PAIR(cosh),  WW_PAIR(tanh),  WW_PAIR(asinh),  WW_PAIR(acosh),
                              WW_PAIR(atanh), WW_PAIR2(pow),  WW_PAIR2(hypot)};
        constexpr int N = 4000000;
        for (const Pair& pair : pairs) {
            double times[2];
            for (int own = 0; own < 2; own++) {
                const Fn fn = own == 1 ? pair.own : pair.system;
                const bool acoshLike = std::string(pair.name) == "acosh";
                const bool unit = std::string(pair.name) == "asin" || std::string(pair.name) == "acos" || std::string(pair.name) == "atanh";
                volatile double sink = 0;
                const auto start = std::chrono::steady_clock::now();
                double x = acoshLike ? 1.1 : unit ? -0.9 : 0.1, acc = 0;
                const double step = unit ? 1.8 / N : 0.37e-5;
                for (int i = 0; i < N; i++) {
                    acc += fn(x, 1.3);
                    x += step;
                }
                sink = acc;
                (void)sink;
                times[own] = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() / N;
            }
            std::printf("%-6s system %6.1f ns  own %6.1f ns\n", pair.name, times[0], times[1]);
        }
        return 0;
    }
    char line[256];
    while (std::fgets(line, sizeof line, stdin) != nullptr) {
        char name[32];
        uint64_t xb = 0, yb = 0;
        const int got = std::sscanf(line, "%31s %" SCNx64 " %" SCNx64, name, &xb, &yb);
        if (got < 2) continue;
        const double r = call(name, fromBits(xb), fromBits(yb), true);
        std::printf("%016" PRIx64 "\n", bitsOf(r));
    }
    return 0;
}
