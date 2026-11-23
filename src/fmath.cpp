// WinterWolf's own math functions (src/fmath.h, o-complex-linux).
//
// Every function is made of +, -, *, / and sqrt, each rounded once as IEEE 754 asks, and of
// exact steps on the bits of doubles; so the same program gives the same bits on Windows,
// Linux and macOS, on x86-64 and on ARM64. A multiply and an add fused into one step (an FMA)
// rounds once where two steps round twice, so it is turned off here: the compilers may not
// fuse what is written as two.
//
// Where one double is not fine enough, a value is kept as two (DD): a double and the double
// nearest to what is left. Sums and products of these are exact or nearly (Dekker's and
// Knuth's two-sum and two-product), so a result is worked out to some 60 to 100 bits and then
// rounded once: within an ulp of the true value, almost always the nearest double.
//
// The ways in are the textbook ones, nothing taken from another library: exp by 2^(j/64)
// from a table and a short series (Tang); ln by ln(1 + j/128) from a table and the series of
// ln(1 + r); sin and cos by reducing to [-pi/4, pi/4] (pi/2 in four parts, or for large
// arguments 2/pi's bits as Payne and Hanek do) and their series; atan by atan(j/64) from a
// table and the series of what is left; the rest from these. The tables and constants are
// made exactly by tools/fmath/tables.py (src/fmath_tables.inc).
//
// Six are not ww's own: exp, expm1, log1p, sinh, cosh and tanh are glibc's (src/fmath_glibc.inc:
// Arm's exp and Sun's fdlibm, with their notices), so that they give the bits CPython gives on
// Linux. glibc fuses some of their multiplies and adds; those are written there as std::fma,
// which rounds once on every processor, so they too are the same bits everywhere.
#include "fmath.h"

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

// (CMakeLists.txt also gives this file -ffp-contract=off, for GCC and for a Clang told otherwise.)
#if defined(__clang__)
#pragma clang fp contract(off)
#elif defined(_MSC_VER)
#pragma fp_contract(off)
#endif

namespace ww::fmath {
namespace {

struct DD {
    double hi, lo;
};

#include "fmath_tables.inc"

constexpr double kInf = std::numeric_limits<double>::infinity();
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

inline uint64_t bitsOf(double x) {
    uint64_t b;
    std::memcpy(&b, &x, sizeof b);
    return b;
}

inline double fromBits(uint64_t b) {
    double x;
    std::memcpy(&x, &b, sizeof x);
    return x;
}

inline bool isNaN(double x) { return x != x; }
inline bool isInf(double x) { return x == kInf || x == -kInf; }
inline double absOf(double x) { return fromBits(bitsOf(x) & 0x7FFFFFFFFFFFFFFFull); }
inline double withSign(double magnitude, double sign) {
    return fromBits((bitsOf(magnitude) & 0x7FFFFFFFFFFFFFFFull) | (bitsOf(sign) & 0x8000000000000000ull));
}

// 2^k for -1022 <= k <= 1023.
inline double pow2(int k) { return fromBits(static_cast<uint64_t>(k + 1023) << 52); }

// x * 2^k, rounded once.
double scale2(double x, int k) {
    if (k > 1023) {
        x *= 0x1p1023;
        k -= 1023;
        if (k > 1023) {
            x *= 0x1p1023;
            k -= 1023;
            if (k > 1023) k = 1023;
        }
        return x * pow2(k);
    }
    if (k >= -1022) return x * pow2(k);
    if (k >= -1022 - 54) return (x * pow2(k + 54)) * 0x1p-54;
    if (k < -2022) return x * 0.0;
    return (x * 0x1p-1000) * pow2(k + 1000);
}

// The nearest integer to x (|x| < 2^51), halves to even, as a double.
inline double nearestInt(double x) {
    const double big = 0x1.8p52;
    return x >= 0 ? (x + big) - big : (x - big) + big;
}

// ---- Two doubles as one value ----

inline DD twoSum(double a, double b) {
    const double s = a + b;
    const double bb = s - a;
    return {s, (a - (s - bb)) + (b - bb)};
}

// For |a| >= |b| (or a == 0).
inline DD quickTwoSum(double a, double b) {
    const double s = a + b;
    return {s, b - (s - a)};
}

inline void split(double a, double& hi, double& lo) {
    const double t = 134217729.0 * a;  // 2^27 + 1
    hi = t - (t - a);
    lo = a - hi;
}

inline DD twoProd(double a, double b) {
    const double p = a * b;
    double ah, al, bh, bl;
    split(a, ah, al);
    split(b, bh, bl);
    return {p, ((ah * bh - p) + ah * bl + al * bh) + al * bl};
}

inline DD neg(DD a) { return {-a.hi, -a.lo}; }

// To about 2^-104 of |a| + |b|: none of the sums here cancel more than that leaves room for.
inline DD add(DD a, DD b) {
    DD s = twoSum(a.hi, b.hi);
    s.lo += a.lo + b.lo;
    return quickTwoSum(s.hi, s.lo);
}

inline DD addD(DD a, double b) {
    DD s = twoSum(a.hi, b);
    s.lo += a.lo;
    return quickTwoSum(s.hi, s.lo);
}

inline DD mul(DD a, DD b) {
    DD p = twoProd(a.hi, b.hi);
    p.lo += a.hi * b.lo + a.lo * b.hi;
    return quickTwoSum(p.hi, p.lo);
}

inline DD mulD(DD a, double b) {
    DD p = twoProd(a.hi, b);
    p.lo += a.lo * b;
    return quickTwoSum(p.hi, p.lo);
}

// To about 2^-104: q = a.hi / b.hi, and what a - q b leaves (a.hi - q b.hi exact: they are
// within a rounding of each other) divided again.
DD div(DD a, DD b) {
    const double q = a.hi / b.hi;
    const DD p = twoProd(q, b.hi);
    return quickTwoSum(q, ((((a.hi - p.hi) - p.lo) + a.lo) - q * b.lo) / b.hi);
}

// For a.hi > 0.
DD sqrtDD(DD a) {
    const double s = std::sqrt(a.hi);  // correctly rounded, as IEEE 754 asks of every system
    const DD p = twoProd(s, s);
    const double e = (((a.hi - p.hi) - p.lo) + a.lo) / (2.0 * s);
    return quickTwoSum(s, e);
}

// ---- e^x ----

// e^(zh + zl) as 2^m times a value from about 0.99 to 2.02, for |zh| <= 746: z = n ln2/64 + r,
// |r| <= ln2/128, so e^z = 2^(n/64) e^r, 2^(n/64) = 2^m 2^(j/64) from the table, and e^r by
// its series to r^6 (what is left is under 2^-64 of it).
DD expCore(double zh, double zl, int& m) {
    const double dn = nearestInt(zh * kSixtyFourOverLn2);
    const int n = static_cast<int>(dn);
    const int j = n & 63;
    m = (n - j) / 64;
    // z - n ln2/64, the first two parts of ln2/64 times n exact, and z - n part1 exact (the two
    // are within a factor of 2 of each other).
    const double a = zh - dn * kLn2Over64Part1;
    DD r = twoSum(a, -dn * kLn2Over64Part2);
    r.lo += zl - dn * kLn2Over64Part3;
    r = quickTwoSum(r.hi, r.lo);
    const double x = r.hi;
    const double x2 = x * x;
    const double q = x2 * ((kInvFactorial[2] + x * kInvFactorial[3]) +
                           x2 * ((kInvFactorial[4] + x * kInvFactorial[5]) + x2 * kInvFactorial[6]));
    const DD p = quickTwoSum(x, r.lo + q);  // e^r - 1
    const DD t = kExp2Table[j];
    return add(t, mul(t, p));
}

// ---- ln x ----

// ln(1 + r) for |r| <= 2^-8 + a little, r as two doubles: its series to r^10 (what is left is
// under 2^-80 of it), r and r^2 / 2 kept exact.
DD log1pSmall(double rh, double rl) {
    const DD r2 = twoProd(rh, rh);
    // (Estrin's way: pairs worked out side by side, the chain of steps short.)
    const double a2 = r2.hi, a4 = a2 * a2;
    const double tail = rh * a2 *
                        (((1.0 / 3 - rh * 0.25) + a2 * (0.2 - rh * (1.0 / 6))) +
                         a4 * ((1.0 / 7 - rh * 0.125) + a2 * (1.0 / 9 - rh * 0.1)));
    DD s = twoSum(rh, -0.5 * r2.hi);
    s.lo += ((rl - 0.5 * r2.lo) - rh * rl) + tail;
    return quickTwoSum(s.hi, s.lo);
}

// ln x as two doubles, for x > 0 finite: x = 2^k m with 1 <= m < 2, F = 1 + j/128 the nearest
// to m, so ln x = k ln2 + ln F + ln(1 + (m - F) / F).
DD logCore(double x) {
    int k = 0;
    if (x < 0x1p-1022) {
        x *= 0x1p54;
        k = -54;
    }
    const uint64_t b = bitsOf(x);
    k += static_cast<int>((b >> 52) & 0x7FF) - 1023;
    const double m = fromBits((b & 0x000FFFFFFFFFFFFFull) | 0x3FF0000000000000ull);
    const int j = static_cast<int>((m - 1.0) * 128.0 + 0.5);
    const double d = m - (1.0 + j / 128.0);  // exact
    // (m - F) / F as two doubles: d times 1/F from the table.
    const DD inverse = kLogInverseTable[j];
    DD q = twoProd(d, inverse.hi);
    q.lo += d * inverse.lo;
    q = quickTwoSum(q.hi, q.lo);
    const DD lp = log1pSmall(q.hi, q.lo);
    DD klog = twoProd(static_cast<double>(k), kLn2.hi);
    klog.lo += static_cast<double>(k) * kLn2.lo;
    return add(add(quickTwoSum(klog.hi, klog.lo), kLogTable[j]), lp);
}

// ln(1 + u) for u > -1 as two doubles.
DD log1pDD(DD u) {
    if (absOf(u.hi) <= 0x1p-8) return log1pSmall(u.hi, u.lo);
    DD v = twoSum(1.0, u.hi);
    v.lo += u.lo;
    v = quickTwoSum(v.hi, v.lo);
    return addD(logCore(v.hi), v.lo / v.hi);
}

// ---- sin, cos ----

// x = n pi/2 + y with |y| <= pi/4 or a little more: n mod 4, and y as two doubles. x finite.
int reduce(double x, DD& y) {
    const double ax = absOf(x);
    if (ax <= 0x1.921fb54442d18p-1) {
        y = {x, 0.0};
        return 0;
    }
    if (ax < 0x1p20 * 0x1.921fb54442d18p+0) {
        // pi/2 in four parts: n times each of the first three exact (n < 2^20), and
        // x - n part1 exact (the two within a factor of 2 of each other).
        const double dn = nearestInt(x * kTwoOverPi);
        const double a = x - dn * kPio2Part1;
        const DD r = twoSum(a, -dn * kPio2Part2);
        DD s = twoSum(r.hi, -dn * kPio2Part3);
        s.lo += r.lo - dn * kPio2Part4;
        y = quickTwoSum(s.hi, s.lo);
        return static_cast<int>(static_cast<int64_t>(dn) & 3);
    }
    // Payne and Hanek: |x| = M 2^e, and x 2/pi worked out from 2/pi's bits, those whose part of
    // the product is a multiple of 4 left out, 256 bits taken (what is left out after them is
    // under 2^-170, where y can be no less than about 2^-62 of pi/2).
    constexpr int K = 8;
    const uint64_t b = bitsOf(ax);
    const int e = static_cast<int>(b >> 52) - 1075;
    const uint64_t mant = (b & 0x000FFFFFFFFFFFFFull) | 0x0010000000000000ull;
    const int i0 = e >= 2 ? (e - 2) / 32 : 0;
    uint32_t p[K + 2] = {};
    const uint64_t m0 = mant & 0xFFFFFFFFu, m1 = mant >> 32;
    uint64_t carry = 0;
    for (int i = 0; i < K; i++) {
        const uint64_t c = kTwoOverPiBits[i0 + K - 1 - i];
        const uint64_t t = c * m0 + p[i] + carry;
        p[i] = static_cast<uint32_t>(t);
        carry = t >> 32;
    }
    p[K] = static_cast<uint32_t>(carry);
    carry = 0;
    for (int i = 0; i < K; i++) {
        const uint64_t c = kTwoOverPiBits[i0 + K - 1 - i];
        const uint64_t t = c * m1 + p[i + 1] + carry;
        p[i + 1] = static_cast<uint32_t>(t);
        carry = t >> 32;
    }
    p[K + 1] = static_cast<uint32_t>(carry);
    // x 2/pi = P 2^-shift: bits shift and shift + 1 are n mod 4, those below the fraction.
    const int shift = 32 * (i0 + K) - e;
    auto bit = [&](int at) -> uint32_t { return at < 0 ? 0u : (p[at / 32] >> (at % 32)) & 1u; };
    int n = static_cast<int>(bit(shift) | (bit(shift + 1) << 1));
    // The fraction alone; at a half or more, 1 less than it (n one more).
    for (int at = shift; at < 32 * (K + 2); at++) p[at / 32] &= ~(1u << (at % 32));
    double sign = 1.0;
    if (bit(shift - 1) != 0) {
        n = (n + 1) & 3;
        sign = -1.0;
        uint64_t c = 1;
        for (int i = 0; i < K + 2; i++) {
            const uint64_t t = static_cast<uint64_t>(~p[i]) + c;
            p[i] = static_cast<uint32_t>(t);
            c = t >> 32;
        }
        for (int at = shift; at < 32 * (K + 2); at++) p[at / 32] &= ~(1u << (at % 32));
    }
    int lead = shift - 1;
    while (lead >= 0 && bit(lead) == 0) lead--;
    if (lead < 0) {  // cannot be: pi is not rational
        y = {0.0, 0.0};
        return x < 0 ? (4 - n) & 3 : n;
    }
    auto bits64 = [&](int from) {  // bits from .. from + 63
        uint64_t v = 0;
        for (int i = 63; i >= 0; i--) v = (v << 1) | bit(from + i);
        return v;
    };
    const uint64_t top = bits64(lead - 63);   // its highest bit set
    const uint64_t next = bits64(lead - 127);
    const double hi = static_cast<double>(top >> 11);  // 53 bits, exact
    const double lo = static_cast<double>(((top & 0x7FF) << 53) | (next >> 11));
    const int scale = lead - shift - 52;
    const DD f = quickTwoSum(scale2(hi, scale), scale2(lo, scale - 64));
    DD r = mul(f, kPiOver2);
    r = {sign * r.hi, sign * r.lo};
    if (x < 0) {
        y = neg(r);
        return (4 - n) & 3;
    }
    y = r;
    return n;
}

// The series' coefficients: (-1)^k / (2k+1)! and (-1)^k / (2k)!.
constexpr double kS2 = kInvFactorial[5], kS3 = -kInvFactorial[7], kS4 = kInvFactorial[9],
                 kS5 = -kInvFactorial[11], kS6 = kInvFactorial[13], kS7 = -kInvFactorial[15], kS8 = kInvFactorial[17],
                 kS9 = -kInvFactorial[19];
constexpr double kC2 = -kInvFactorial[6], kC3 = kInvFactorial[8], kC4 = -kInvFactorial[10],
                 kC5 = kInvFactorial[12], kC6 = -kInvFactorial[14], kC7 = kInvFactorial[16], kC8 = -kInvFactorial[18],
                 kC9 = kInvFactorial[20];

// sin(x + y) for |x| <= pi/4 + a little, |y| tiny, as two doubles: x and what is added to it.
// sin(x + y) = x - x^3/6 + x^5 r + y (1 - x^2/2), x^3/6 kept exact (two doubles): what is
// rounded after it is under 2^-8 of the result.
DD sinKernel(double x, double y) {
    const DD zz = twoProd(x, x);
    const double z = zz.hi;
    const DD v = mulD(zz, x);
    const DD t = mul(v, kMinusOneSixth);
    const double z2 = z * z;
    const double r = ((kS2 + z * kS3) + z2 * (kS4 + z * kS5)) + (z2 * z2) * ((kS6 + z * kS7) + z2 * (kS8 + z * kS9));
    DD s = twoSum(x, t.hi);
    s.lo += t.lo + (v.hi * z * r + y * (1.0 - 0.5 * z));
    return quickTwoSum(s.hi, s.lo);
}

// cos(x + y), likewise: 1 - x^2/2 + x^4/24 + x^6 r - x y, x^2/2 and x^4/24 kept exact.
DD cosKernel(double x, double y) {
    const DD zz = twoProd(x, x);
    const double z = zz.hi;
    const DD q = mul(mul(zz, zz), kOneTwentyFourth);
    const double z2 = z * z;
    const double r = ((kC2 + z * kC3) + z2 * (kC4 + z * kC5)) + (z2 * z2) * ((kC6 + z * kC7) + z2 * (kC8 + z * kC9));
    const double hz = 0.5 * z;
    const double w = 1.0 - hz;
    // 1 - w and (1 - w) - hz are exact: what w's rounding lost.
    DD s = twoSum(w, q.hi);
    s.lo += (((1.0 - w) - hz) - 0.5 * zz.lo) + (q.lo + (z * z2 * r - x * y));
    return quickTwoSum(s.hi, s.lo);
}

// ---- atan ----

// atan t for 0 <= t <= 1, t as two doubles: c = j/64 the nearest, so atan t = atan c +
// atan((t - c) / (1 + t c)), the second by its series to u^11 (|u| <= 2^-7).
DD atanDD(DD t) {
    const int j = static_cast<int>(t.hi * 64.0 + 0.5);
    const double c = j / 64.0;
    const DD num = twoSum(t.hi - c, t.lo);  // t.hi - c exact
    DD den = twoProd(t.hi, c);
    den.lo += t.lo * c;
    den = addD(quickTwoSum(den.hi, den.lo), 1.0);
    const DD u = div(num, den);
    const double z = u.hi * u.hi;
    const double z2 = z * z;
    const double tail = u.hi * z * ((-1.0 / 3 + z * 0.2) + z2 * ((-1.0 / 7 + z * (1.0 / 9)) + z2 * (-1.0 / 11)));
    DD au = twoSum(u.hi, tail);
    au.lo += u.lo;
    return add(kAtanTable[j], quickTwoSum(au.hi, au.lo));
}

// atan(small / large) for 0 <= small <= large, large finite and not 0.
DD atanRatio(double small, double large) {
    if (small == 0.0) return {0.0, 0.0};
    if (large < 0x1p-1000) {  // either may be subnormal: both scaled up first (the ratio is the same)
        small *= 0x1p600;
        large *= 0x1p600;
    }
    const int es = static_cast<int>((bitsOf(small) >> 52) & 0x7FF), el = static_cast<int>((bitsOf(large) >> 52) & 0x7FF);
    if (el - es > 60) {
        // Under 2^-59: atan of it is itself to well within a rounding.
        return {small / large, 0.0};
    }
    // Both scaled so large is from 1 to 2: exact, and nothing overflows below. Then as atanDD
    // does, but from the two: atan(s/l) = atan c + atan((s - c l) / (l + c s)), c = j/64.
    const int k = el - 1023;
    const double s = scale2(small, -k), l = scale2(large, -k);
    const int j = static_cast<int>(s / l * 64.0 + 0.5);
    const double c = j / 64.0;
    const DD cl = twoProd(c, l);
    const DD num = twoSum(s - cl.hi, -cl.lo);  // s - cl.hi exact: within a factor of 2
    const DD cs = twoProd(c, s);
    DD den = twoSum(l, cs.hi);
    den.lo += cs.lo;
    const DD u = div(num, quickTwoSum(den.hi, den.lo));
    const double z = u.hi * u.hi;
    const double z2 = z * z;
    const double tail = u.hi * z * ((-1.0 / 3 + z * 0.2) + z2 * ((-1.0 / 7 + z * (1.0 / 9)) + z2 * (-1.0 / 11)));
    DD au = twoSum(u.hi, tail);
    au.lo += u.lo;
    return add(kAtanTable[j], quickTwoSum(au.hi, au.lo));
}

// 1 - x^2 for 0 <= x <= 1, as two doubles.
DD oneMinusSquare(double ax) {
    if (ax <= 0.5) {
        const DD p = twoProd(ax, ax);
        DD w = twoSum(1.0, -p.hi);
        w.lo -= p.lo;
        return quickTwoSum(w.hi, w.lo);
    }
    const double a = 1.0 - ax;  // exact
    const DD b = twoSum(1.0, ax);
    return mulD(b, a);
}

// x^n exactly, rounded once, where it can be: n a whole number from 1 to 128 and the odd part of
// x to the n fitting in 128 bits (every x^2 does). Where the true value is halfway between two
// doubles, only the exact one can say which way it goes. False where it cannot be done so.
bool exactPower(double x, int n, double& out) {
    const uint64_t b = bitsOf(x);
    const int biased = static_cast<int>((b >> 52) & 0x7FF);
    uint64_t mant = b & 0x000FFFFFFFFFFFFFull;
    int e = biased == 0 ? -1074 : biased - 1075;
    if (biased != 0) mant |= 0x0010000000000000ull;
    const int zeros = std::countr_zero(mant);
    mant >>= zeros;
    e += zeros;
    if ((64 - std::countl_zero(mant)) * n > 128) return false;
    // mant^n in two 64-bit halves: each step (hi, lo) times mant, lo by 32-bit halves.
    uint64_t hi = 0, lo = 1;
    const uint64_t m0 = mant & 0xFFFFFFFFu, m1 = mant >> 32;
    for (int i = 0; i < n; i++) {
        const uint64_t l0 = lo & 0xFFFFFFFFu, l1 = lo >> 32;
        const uint64_t p00 = l0 * m0, p01 = l0 * m1, p10 = l1 * m0, p11 = l1 * m1;
        const uint64_t mid = (p00 >> 32) + (p01 & 0xFFFFFFFFu) + (p10 & 0xFFFFFFFFu);
        hi = hi * mant + ((mid >> 32) + (p01 >> 32) + (p10 >> 32) + p11);
        lo = (p00 & 0xFFFFFFFFu) | (mid << 32);
    }
    // Rounded to 53 bits, halves to even.
    const int length = hi != 0 ? 128 - std::countl_zero(hi) : 64 - std::countl_zero(lo);
    if (length <= 53) {
        out = scale2(static_cast<double>(lo), e * n);
        return true;
    }
    int shift = length - 53;
    uint64_t q = shift >= 64 ? hi >> (shift - 64) : (lo >> shift) | (hi << (64 - shift));
    const int h = shift - 1;  // the bit that is a half
    const bool half = (h >= 64 ? (hi >> (h - 64)) : (lo >> h)) & 1;
    const bool below = h >= 64 ? (lo != 0 || (hi & ((uint64_t{1} << (h - 64)) - 1)) != 0)
                               : (lo & ((uint64_t{1} << h) - 1)) != 0;
    if (half && (below || (q & 1) != 0)) {
        q++;
        if (q == 0x0020000000000000ull) {
            q >>= 1;
            shift++;
        }
    }
    out = scale2(static_cast<double>(q), shift + e * n);
    return true;
}

// The parts of a double: whether it is an integer, and an odd one.
bool isInteger(double y) {
    if (absOf(y) >= 0x1p53) return true;
    return static_cast<double>(static_cast<int64_t>(y)) == y;
}

bool isOddInteger(double y) {
    if (absOf(y) >= 0x1p53) return false;
    const auto i = static_cast<int64_t>(y);
    return static_cast<double>(i) == y && (i & 1) != 0;
}

#include "fmath_glibc.inc"

}  // namespace

// ---- The functions ----

double exp(double x) { return glibcExp(x); }

double expm1(double x) { return glibcExpm1(x); }

double log(double x) {
    if (isNaN(x)) return x + x;
    if (x < 0) return kNaN;
    if (x == 0) return -kInf;
    if (x == kInf) return x;
    return logCore(x).hi;
}

double log2(double x) {
    if (isNaN(x)) return x + x;
    if (x < 0) return kNaN;
    if (x == 0) return -kInf;
    if (x == kInf) return x;
    return mul(logCore(x), kInvLn2).hi;
}

double log10(double x) {
    if (isNaN(x)) return x + x;
    if (x < 0) return kNaN;
    if (x == 0) return -kInf;
    if (x == kInf) return x;
    return mul(logCore(x), kInvLn10).hi;
}

double log1p(double x) { return glibcLog1p(x); }

double logBase(double x, double base) {
    if (isNaN(x) || isNaN(base)) return x + base;
    if (base <= 0.0 || base == 1.0) return kNaN;
    if (base == 2.0) return log2(x);
    if (base == 10.0) return log10(x);
    if (x < 0) return kNaN;
    if (base == kInf) return x == kInf || x == 0 ? kNaN : 0.0;
    if (x == 0) return base > 1.0 ? -kInf : kInf;
    if (x == kInf) return base > 1.0 ? kInf : -kInf;
    if (x == 1.0) return 0.0;
    return div(logCore(x), logCore(base)).hi;
}

double sin(double x) {
    if (isNaN(x)) return x + x;
    if (isInf(x)) return kNaN;  // not inf - inf: x86 and ARM make that NaN with other bits
    if (absOf(x) < 0x1p-27) return x;
    DD y;
    switch (reduce(x, y)) {
        case 0: return sinKernel(y.hi, y.lo).hi;
        case 1: return cosKernel(y.hi, y.lo).hi;
        case 2: return -sinKernel(y.hi, y.lo).hi;
        default: return -cosKernel(y.hi, y.lo).hi;
    }
}

double cos(double x) {
    if (isNaN(x)) return x + x;
    if (isInf(x)) return kNaN;  // not inf - inf: x86 and ARM make that NaN with other bits
    if (absOf(x) < 0x1p-27) return 1.0;
    DD y;
    switch (reduce(x, y)) {
        case 0: return cosKernel(y.hi, y.lo).hi;
        case 1: return -sinKernel(y.hi, y.lo).hi;
        case 2: return -cosKernel(y.hi, y.lo).hi;
        default: return sinKernel(y.hi, y.lo).hi;
    }
}

double tan(double x) {
    if (isNaN(x)) return x + x;
    if (isInf(x)) return kNaN;  // not inf - inf: x86 and ARM make that NaN with other bits
    if (absOf(x) < 0x1p-27) return x;
    DD y;
    const int n = reduce(x, y);
    const DD s = sinKernel(y.hi, y.lo), c = cosKernel(y.hi, y.lo);
    return (n & 1) == 0 ? div(s, c).hi : -div(c, s).hi;
}

double atan(double x) {
    if (isNaN(x)) return x + x;
    const double ax = absOf(x);
    if (ax < 0x1p-27) return x;
    if (ax > 0x1p60) return withSign(kPiOver2.hi, x);
    DD r;
    if (ax <= 1.0) {
        r = atanDD({ax, 0.0});
    } else {
        const double q = 1.0 / ax;
        const DD p = twoProd(q, ax);
        r = add(kPiOver2, neg(atanDD(quickTwoSum(q, ((1.0 - p.hi) - p.lo) / ax))));
    }
    return withSign(r.hi, x);
}

double atan2(double y, double x) {
    if (isNaN(x) || isNaN(y)) return x + y;
    const double ax = absOf(x), ay = absOf(y);
    const bool left = bitsOf(x) >> 63 != 0;  // x < 0, or -0
    if (ay == 0.0) return withSign(left ? kPi.hi : 0.0, y);
    if (ax == 0.0) return withSign(kPiOver2.hi, y);
    if (isInf(ay)) {
        if (isInf(ax)) return withSign(left ? 3.0 * kPiOver4.hi : kPiOver4.hi, y);
        return withSign(kPiOver2.hi, y);
    }
    if (isInf(ax)) return withSign(left ? kPi.hi : 0.0, y);
    DD a = ay <= ax ? atanRatio(ay, ax) : add(kPiOver2, neg(atanRatio(ax, ay)));
    if (left) a = add(kPi, neg(a));
    return withSign(a.hi, y);
}

double asin(double x) {
    if (isNaN(x)) return x + x;
    const double ax = absOf(x);
    if (ax > 1.0) return kNaN;
    if (ax < 0x1p-27) return x;
    if (ax == 1.0) return withSign(kPiOver2.hi, x);
    const DD s = sqrtDD(oneMinusSquare(ax));
    const DD r = ax <= s.hi ? atanDD(div({ax, 0.0}, s)) : add(kPiOver2, neg(atanDD(div(s, {ax, 0.0}))));
    return withSign(r.hi, x);
}

double acos(double x) {
    if (isNaN(x)) return x + x;
    const double ax = absOf(x);
    if (ax > 1.0) return kNaN;
    if (x == 1.0) return 0.0;
    if (x == -1.0) return kPi.hi;
    if (ax < 0x1p-55) return kPiOver2.hi;
    const DD s = sqrtDD(oneMinusSquare(ax));
    DD r = s.hi <= ax ? atanDD(div(s, {ax, 0.0})) : add(kPiOver2, neg(atanDD(div({ax, 0.0}, s))));
    if (x < 0) r = add(kPi, neg(r));
    return r.hi;
}

double sinh(double x) { return glibcSinh(x); }

double cosh(double x) { return glibcCosh(x); }

double tanh(double x) { return glibcTanh(x); }

double asinh(double x) {
    if (isNaN(x) || isInf(x)) return x;
    const double ax = absOf(x);
    if (ax < 0x1p-27) return x;
    if (ax > 0x1p28) return withSign(add(logCore(ax), kLn2).hi, x);
    // ln(x + sqrt(x^2 + 1)) = ln(1 + x + x^2 / (1 + sqrt(x^2 + 1))): nothing cancels.
    const DD x2 = twoProd(ax, ax);
    const DD q = sqrtDD(addD(x2, 1.0));
    const DD u = addD(div(x2, addD(q, 1.0)), ax);
    return withSign(log1pDD(u).hi, x);
}

double acosh(double x) {
    if (isNaN(x)) return x + x;
    if (x < 1.0) return kNaN;
    if (x == 1.0) return 0.0;
    if (x == kInf) return x;
    if (x > 0x1p28) return add(logCore(x), kLn2).hi;
    // ln(x + sqrt(x^2 - 1)) = ln(1 + (x - 1) + sqrt(x^2 - 1)).
    const DD t = twoSum(x, -1.0);
    const DD p = twoProd(x, x);
    DD v = twoSum(p.hi, -1.0);
    v.lo += p.lo;
    v = quickTwoSum(v.hi, v.lo);
    return log1pDD(add(t, sqrtDD(v))).hi;
}

double atanh(double x) {
    if (isNaN(x)) return x + x;
    const double ax = absOf(x);
    if (ax > 1.0) return kNaN;
    if (ax == 1.0) return withSign(kInf, x);
    if (ax < 0x1p-27) return x;
    // ln((1 + x) / (1 - x)) / 2 = ln(1 + 2x / (1 - x)) / 2.
    const DD d = twoSum(1.0, -ax);
    return withSign(log1pDD(div({2.0 * ax, 0.0}, d)).hi * 0.5, x);
}

double pow(double x, double y) {
    if (y == 0.0) return 1.0;
    if (x == 1.0) return 1.0;
    if (isNaN(x) || isNaN(y)) return x + y;
    const double ax = absOf(x);
    if (isInf(y)) {
        if (ax == 1.0) return 1.0;
        return (ax > 1.0) == (y > 0) ? kInf : 0.0;
    }
    const bool odd = isOddInteger(y);
    if (x == 0.0) {
        if (y < 0) return odd ? withSign(kInf, x) : kInf;
        return odd ? x : 0.0;
    }
    if (isInf(x)) {
        if (x > 0) return y < 0 ? 0.0 : kInf;
        if (y < 0) return odd ? -0.0 : 0.0;
        return odd ? -kInf : kInf;
    }
    double sign = 1.0;
    if (x < 0) {
        if (!isInteger(y)) return kNaN;
        if (odd) sign = -1.0;
        if (x == -1.0) return sign;
    }
    if (y > 0.0 && y <= 128.0 && isInteger(y)) {
        double exact;
        if (exactPower(ax, static_cast<int>(y), exact)) return sign * exact;
    }
    // |x|^y = e^(y ln|x|), y ln|x| as two doubles.
    const DD l = logCore(ax);
    const double zh = y * l.hi;
    if (zh > 710.0) return sign * kInf;
    if (zh < -746.0) return sign * 0.0;
    DD z = twoProd(y, l.hi);
    z.lo += y * l.lo;
    z = quickTwoSum(z.hi, z.lo);
    int m;
    const DD v = expCore(z.hi, z.lo, m);
    return sign * scale2(v.hi, m);
}

double ldexp(double x, int64_t e) {
    if (isNaN(x) || isInf(x) || x == 0.0) return x;
    if (e > 3000) e = 3000;  // past any float's reach either way: an infinity or a zero, as the rounding says
    if (e < -3000) e = -3000;
    return scale2(x, static_cast<int>(e));
}

double frexp(double x, int64_t& e) {
    e = 0;
    if (isNaN(x) || isInf(x) || x == 0.0) return x;
    int shift = 0;
    if (absOf(x) < 0x1p-1022) {  // subnormal: made normal first, exactly
        x *= 0x1p54;
        shift = -54;
    }
    const uint64_t b = bitsOf(x);
    const int biased = static_cast<int>((b >> 52) & 0x7FF);
    e = biased - 1022 + shift;
    // The same significand with the exponent of [0.5, 1).
    return fromBits((b & 0x800FFFFFFFFFFFFFull) | 0x3FE0000000000000ull);
}

double hypot(double x, double y) {
    if (isInf(x) || isInf(y)) return kInf;
    if (isNaN(x) || isNaN(y)) return x + y;
    double a = absOf(x), b = absOf(y);
    if (a < b) {
        const double t = a;
        a = b;
        b = t;
    }
    if (b == 0.0) return a;
    int k = 0;
    if (a < 0x1p-1000) {  // small enough that a and b may be subnormal: scaled up first
        a *= 0x1p600;
        b *= 0x1p600;
        k = -600;
    }
    const int ea = static_cast<int>((bitsOf(a) >> 52) & 0x7FF), eb = static_cast<int>((bitsOf(b) >> 52) & 0x7FF);
    if (ea - eb > 60) return scale2(a, k);  // b^2 under 2^-120 of a^2
    const int e = ea - 1023;
    const double as = scale2(a, -e), bs = scale2(b, -e);  // exact: as from 1 to 2
    const DD s = add(twoProd(as, as), twoProd(bs, bs));
    return scale2(sqrtDD(s).hi, e + k);
}


// ---- More functions (docs/math-plan.html): cbrt, gamma, lnGamma, erf, erfc, fma, ieeeRemainder,
// hypot of many ----

namespace {

// (v.hi + v.lo) × 2^m rounded once to a float, a subnormal one too: v.hi is already v rounded,
// and only where scaling it rounds again, at a tie of the subnormal steps, does v.lo say which way.
double roundScaled(DD v, int m) {
    const double r = scale2(v.hi, m);
    if (absOf(r) >= 0x1p-1022 || v.lo == 0.0 || isInf(r)) return r;
    const double back = scale2(r, -m);  // exact: r is a whole number of subnormal steps
    const double d = v.hi - back;       // exact: the two are within a step of each other
    const int halfStep = -1075 - m;
    if (halfStep < -1022 || halfStep > 1023) return r;
    if (absOf(d) == pow2(halfStep) && (d > 0) == (v.lo > 0)) return r + withSign(0x1p-1074, d);
    return r;
}

// e^z as two doubles times 2^m, to some 2^-100 of it (expCore's 2^-64 is not enough where erfc
// is 1 - erf): z less n ln 2, ln 2 in two doubles, then e^r by its series to past 2^-110.
DD expPrecise(DD z, int& m) {
    constexpr DD kLn2 = {0x1.62e42fefa39efp-1, 0x1.abc9e3b39803fp-56};
    const double n = nearestInt(z.hi / kLn2.hi);
    const DD r = add(z, neg(mulD(kLn2, n)));
    DD term = {1.0, 0.0};
    DD sum = term;
    for (int k = 1; k < 60; k++) {
        term = div(mul(term, r), {static_cast<double>(k), 0.0});
        sum = add(sum, term);
        if (absOf(term.hi) < 0x1p-112) break;
    }
    m = static_cast<int>(n);
    return sum;
}

// 2/sqrt(pi) and 1/sqrt(pi) as two doubles.
constexpr DD kTwoOverSqrtPi = {0x1.20dd750429b6dp+0, 0x1.1ae3a914fed80p-56};
constexpr DD kOneOverSqrtPi = {0x1.20dd750429b6dp-1, 0x1.1ae3a914fed80p-57};

// erf(x) for 0 < x < 6 as two doubles times 2^m: 2/sqrt(pi) e^(-x^2) times the sum of
// 2^n x^(2n+1) / (1·3·5···(2n+1)), whose terms are all above 0, so nothing cancels.
DD erfSeries(double x, int& m) {
    DD term = {x, 0.0};
    DD sum = term;
    const DD x2 = twoProd(x, x);
    const DD twoX2 = {2.0 * x2.hi, 2.0 * x2.lo};
    for (int n = 1; n < 2000; n++) {
        term = div(mul(term, twoX2), {2.0 * n + 1.0, 0.0});
        sum = add(sum, term);
        if (term.hi < sum.hi * 0x1p-112) break;
    }
    const DD e = expPrecise(neg(x2), m);
    return mul(mul(sum, e), kTwoOverSqrtPi);
}

// erfc(x) for x >= 4 as two doubles times 2^m: e^(-x^2) / sqrt(pi) over x + (1/2) / (x + 1 /
// (x + (3/2) / (x + ...))), the continued fraction worked from its far end.
DD erfcFraction(double x, int& m) {
    const DD xd = {x, 0.0};
    // Terms enough that the fraction is settled to past 2^-110: more where x is small.
    const int terms = static_cast<int>(4000.0 / (x * x)) + 40;
    DD f = xd;
    for (int k = terms; k >= 1; k--) f = add(xd, div({0.5 * k, 0.0}, f));
    const DD x2 = twoProd(x, x);
    const DD e = expPrecise(neg(x2), m);
    return div(mul(e, kOneOverSqrtPi), f);
}

// erf (complement false) or erfc (true) of a finite x.
double erfOrErfc(double x, bool complement) {
    if (x == 0.0) return complement ? 1.0 : x;
    const bool negative = x < 0.0;
    const double ax = absOf(x);
    if (!complement) {
        if (ax >= 6.0) return negative ? -1.0 : 1.0;
        if (ax < 0x1p-60) {
            // 2x/sqrt(pi): the rest of the series is under 2^-120 of it. Scaled up first, so a
            // subnormal x is multiplied exactly.
            const DD p = mulD(kTwoOverSqrtPi, scale2(ax, 200));
            const double r = roundScaled(quickTwoSum(p.hi, p.lo), -200);
            return negative ? -r : r;
        }
        int m;
        const DD s = erfSeries(ax, m);
        const double r = roundScaled(s, m);
        return negative ? -r : r;
    }
    if (x >= 27.3) return 0.0;
    if (x <= -6.0) return 2.0;
    if (x >= 4.0) {
        int m;
        const DD f = erfcFraction(x, m);
        return roundScaled(f, m);
    }
    if (ax < 0x1p-60) return 1.0;
    // 1 - erf(x), or 1 + erf(|x|): erfc(4) is 1.5e-8, so at most 27 bits of 106 cancel.
    int m;
    const DD s = erfSeries(ax, m);
    const DD e = {scale2(s.hi, m), scale2(s.lo, m)};  // exact: erf(x) here is above 2^-61
    const DD r = negative ? addD(e, 1.0) : addD(neg(e), 1.0);
    return r.hi;
}

// The Lanczos approximation, as CPython's m_tgamma and m_lgamma use it (Modules/mathmodule.c,
// Copyright (c) 2001 Python Software Foundation; PSF License Agreement): g, g - 1/2, and the
// sum's numerator and denominator.
constexpr double kLanczosG = 6.024680040776729583740234375;
constexpr double kLanczosGMinusHalf = 5.524680040776729583740234375;
constexpr double kLanczosNum[13] = {
    23531376880.410759688572007674451636754734846804940, 42919803642.649098768957899047001988850926355848959,
    35711959237.355668049440185451547166705960488635843, 17921034426.037209699919755754458931112671403265390,
    6039542586.3520280050642916443072979210699388420708, 1439720407.3117216736632230727949123939715485786772,
    248874557.86205415651146038641322942321632125127801, 31426415.585400194380614231628318205362874684987640,
    2876370.6289353724412254090516208496135991145378768, 186056.26539522349504029498971604569928220784236328,
    8071.6720023658162106380029022722506138218516325024, 210.82427775157934587250973392071336271166969580291,
    2.5066282746310002701649081771338373386264310793408};
constexpr double kLanczosDen[13] = {0.0, 39916800.0, 120543840.0, 150917976.0, 105258076.0, 45995730.0, 13339535.0,
                                    2637558.0, 357423.0, 32670.0, 1925.0, 66.0, 1.0};
constexpr double kGammaIntegral[23] = {
    1.0, 1.0, 2.0, 6.0, 24.0, 120.0, 720.0, 5040.0, 40320.0, 362880.0, 3628800.0, 39916800.0, 479001600.0,
    6227020800.0, 87178291200.0, 1307674368000.0, 20922789888000.0, 355687428096000.0, 6402373705728000.0,
    121645100408832000.0, 2432902008176640000.0, 51090942171709440000.0, 1124000727777607680000.0};
constexpr double kLogPi = 1.144729885849400174143427351353058711647;

double lanczosSum(double x) {
    double num = 0.0, den = 0.0;
    if (x < 5.0) {
        for (int i = 12; i >= 0; i--) {
            num = num * x + kLanczosNum[i];
            den = den * x + kLanczosDen[i];
        }
    } else {
        for (int i = 0; i < 13; i++) {
            num = num / x + kLanczosNum[i];
            den = den / x + kLanczosDen[i];
        }
    }
    return num / den;
}

// sin(pi x), exact at the whole and half numbers, as CPython's m_sinpi.
double sinPi(double x) {
    const double y = std::fmod(absOf(x), 2.0);  // exact
    const double n = std::round(2.0 * y);      // exact
    double r;
    if (n == 0.0) {
        r = sin(kPi.hi * y);
    } else if (n == 1.0) {
        r = cos(kPi.hi * (y - 0.5));
    } else if (n == 2.0) {
        r = sin(kPi.hi * (1.0 - y));
    } else if (n == 3.0) {
        r = -cos(kPi.hi * (y - 1.5));
    } else {
        r = sin(kPi.hi * (y - 2.0));
    }
    return withSign(1.0, x) * r;
}

bool isWhole(double x) { return std::floor(x) == x; }

}  // namespace

double cbrt(double x) {
    if (isNaN(x) || isInf(x) || x == 0.0) return x + x;
    double ax = absOf(x);
    int k = 0;
    if (ax < 0x1p-1000) {  // a subnormal or near one: scaled by 2^600, its root by 2^200
        ax *= 0x1p600;
        k = -200;
    } else if (ax > 0x1p1000) {
        ax *= 0x1p-600;
        k = 200;
    }
    // A guess to some 13 digits, one step of Newton's way in doubles to past 16, then what the
    // cube still lacks, worked out in two doubles, as one correction: the nearest float, and an
    // exact cube's root exactly.
    double y = pow(ax, 1.0 / 3.0);
    y = y - (y * y * y - ax) / (3.0 * y * y);
    const DD y3 = mulD(twoProd(y, y), y);
    const DD rest = addD(neg(y3), ax);
    const DD r = quickTwoSum(y, (rest.hi + rest.lo) / (3.0 * y * y));
    const double out = scale2(r.hi, k);
    return x < 0.0 ? -out : out;
}

double gamma(double x) {
    if (isNaN(x) || x == kInf) return x;
    if (x == -kInf) return kNaN;
    if (x == 0.0) return withSign(kInf, x);
    if (isWhole(x) && x < 0.0) return kNaN;
    if (isWhole(x) && x <= 23.0) return kGammaIntegral[static_cast<int>(x) - 1];
    const double absx = absOf(x);
    if (absx < 1e-20) return 1.0 / x;
    if (absx > 200.0) return x < 0.0 ? 0.0 / sinPi(x) : kInf;
    const double y = absx + kLanczosGMinusHalf;
    double z;
    if (absx > kLanczosGMinusHalf) {
        const double q = y - absx;
        z = q - kLanczosGMinusHalf;
    } else {
        const double q = y - kLanczosGMinusHalf;
        z = q - absx;
    }
    z = z * kLanczosG / y;
    double r;
    if (x < 0.0) {
        r = -kPi.hi / sinPi(absx) / absx * exp(y) / lanczosSum(absx);
        r -= z * r;
        if (absx < 140.0) {
            r /= pow(y, absx - 0.5);
        } else {
            const double sqrtpow = pow(y, absx / 2.0 - 0.25);
            r /= sqrtpow;
            r /= sqrtpow;
        }
    } else {
        r = lanczosSum(absx) / exp(y);
        r += z * r;
        if (absx < 140.0) {
            r *= pow(y, absx - 0.5);
        } else {
            const double sqrtpow = pow(y, absx / 2.0 - 0.25);
            r *= sqrtpow;
            r *= sqrtpow;
        }
    }
    return r;
}

double lnGamma(double x) {
    if (isNaN(x)) return x;
    if (isInf(x)) return kInf;
    if (isWhole(x) && x <= 2.0) return x <= 0.0 ? kInf : 0.0;
    const double absx = absOf(x);
    if (absx < 1e-20) return -log(absx);
    double r = log(lanczosSum(absx)) - kLanczosG;
    r += (absx - 0.5) * (log(absx + kLanczosG - 0.5) - 1.0);
    if (x < 0.0) r = kLogPi - log(absOf(sinPi(absx))) - log(absx) - r;
    return r;
}

double erf(double x) {
    if (isNaN(x)) return x;
    if (isInf(x)) return x > 0 ? 1.0 : -1.0;
    return erfOrErfc(x, false);
}

double erfc(double x) {
    if (isNaN(x)) return x;
    if (isInf(x)) return x > 0 ? 0.0 : 2.0;
    return erfOrErfc(x, true);
}

double fma(double a, double b, double c) { return std::fma(a, b, c); }

double ieeeRemainder(double x, double y) {
    if (isNaN(x) || isNaN(y)) return x + y;
    if (isInf(x) || y == 0.0) return kNaN;
    if (isInf(y)) return x;
    // As CPython's m_remainder: from fmod, which is exact, x - n y for the n nearest x / y, ties to even.
    const double absx = absOf(x), absy = absOf(y);
    const double m = std::fmod(absx, absy);
    const double c = absy - m;
    double r;
    if (m < c) {
        r = m;
    } else if (m > c) {
        r = -c;
    } else {
        r = m - 2.0 * std::fmod(0.5 * (absx - m), absy);
    }
    return withSign(1.0, x) * r;
}

double hypot(const double* values, size_t count) {
    bool nan = false;
    double big = 0.0;
    for (size_t i = 0; i < count; i++) {
        if (isInf(values[i])) return kInf;
        if (isNaN(values[i])) nan = true;
        if (absOf(values[i]) > big) big = absOf(values[i]);
    }
    if (nan) return kNaN;
    if (big == 0.0) return 0.0;
    int k = 0;
    if (big < 0x1p-1000) k = 600;  // subnormals scaled up first
    const int e = static_cast<int>((bitsOf(scale2(big, k)) >> 52) & 0x7FF) - 1023;
    // Each scaled so that the largest is from 1 to 2 (exact, but for a part too small to count),
    // its square exact in two doubles, the squares added in two doubles.
    DD sum = {0.0, 0.0};
    for (size_t i = 0; i < count; i++) {
        const double v = scale2(absOf(values[i]), k - e);
        sum = add(sum, twoProd(v, v));
    }
    return roundScaled(sqrtDD(sum), e - k);
}

}  // namespace ww::fmath
