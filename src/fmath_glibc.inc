// glibc's exp, expm1, log1p, sinh, cosh and tanh (W6 of docs/snippy-ww-checklist.html; the user:
// "2b (ww should agree with itself on every system after that)"), so that ww gives the bits
// CPython gives on Linux, and the same bits on every system and processor.
//
// glibc 2.43 on x86-64 with FMA, as CPython runs there: the FMA builds of exp, expm1, log1p and
// sinh (compiled with GCC's -ffp-contract=fast, which fuses a multiply into the add or subtract
// that takes it within one block), and cosh and tanh built without FMA but calling those. Each
// multiply-add GCC fuses there is written here as std::fma, which rounds once, exactly, on
// every processor; the rest as written, with this file's contraction turned off, so the bits
// are glibc's everywhere. Checked against CPython 3.14's math on 1.27 million inputs over every
// branch: none differ. tests/glibc_math.ww keeps 93,000 of them (tools/fmath/glibc_digest.py).
//
// exp: Arm's optimized-routines (Copyright (c) 2018-2023, Arm Limited; SPDX-License-Identifier:
// MIT OR Apache-2.0 WITH LLVM-exception), which glibc's is, with its table of 128 and its
// order-5 polynomial.
//
// expm1, log1p, sinh, cosh, tanh: Sun's fdlibm, as glibc has them:
// ====================================================
// Copyright (C) 1993 by Sun Microsystems, Inc. All rights reserved.
//
// Developed at SunPro, a Sun Microsystems, Inc. business.
// Permission to use, copy, modify, and distribute this
// software is freely granted, provided that this notice
// is preserved.
// ====================================================

inline uint32_t highWord(double x) { return static_cast<uint32_t>(bitsOf(x) >> 32); }
inline uint32_t lowWord(double x) { return static_cast<uint32_t>(bitsOf(x)); }
inline double withHighWord(double x, uint32_t h) { return fromBits((static_cast<uint64_t>(h) << 32) | lowWord(x)); }
inline double fused(double a, double b, double c) { return std::fma(a, b, c); }

// ---- exp ----

constexpr uint64_t kGlibcExpTable[256] = {
#include "fmath_glibc_exp.inc"
};
constexpr double kGexpInvLn2N = 0x1.71547652b82fep0 * 128, kGexpNegLn2HiN = -0x1.62e42fefa0000p-8,
                 kGexpNegLn2LoN = -0x1.cf79abc9e3b3ap-47, kGexpShift = 0x1.8p52, kGexpC2 = 0x1.ffffffffffdbdp-2,
                 kGexpC3 = 0x1.555555555543cp-3, kGexpC4 = 0x1.55555cf172b91p-5, kGexpC5 = 0x1.1111167a4d017p-7;

// A result that may overflow or underflow: scale * (1 + tmp) without rounding on the way.
double glibcExpSpecial(double tmp, uint64_t sbits, uint64_t ki) {
    if ((ki & 0x80000000) == 0) {
        sbits -= 1009ull << 52;
        const double scale = fromBits(sbits);
        return 0x1p1009 * fused(scale, tmp, scale);
    }
    sbits += 1022ull << 52;
    const double scale = fromBits(sbits);
    double y = scale + scale * tmp;  // not fused there
    if (y < 1.0) {
        double lo = fused(scale, tmp, scale - y);
        const double hi = 1.0 + y;
        lo = 1.0 - hi + y + lo;
        y = (hi + lo) - 1.0;
        if (y == 0.0) y = 0.0;
    }
    return 0x1p-1022 * y;
}

double glibcExp(double x) {
    uint32_t abstop = static_cast<uint32_t>(bitsOf(x) >> 52) & 0x7ff;
    const uint32_t tiny = static_cast<uint32_t>(bitsOf(0x1p-54) >> 52), big = static_cast<uint32_t>(bitsOf(512.0) >> 52);
    if (abstop - tiny >= big - tiny) {
        if (abstop - tiny >= 0x80000000) return 1.0 + x;
        if (abstop >= static_cast<uint32_t>(bitsOf(1024.0) >> 52)) {
            if (bitsOf(x) == bitsOf(-kInf)) return 0.0;
            if (abstop >= 0x7ff) return 1.0 + x;
            return (bitsOf(x) >> 63) ? 0.0 : kInf;
        }
        abstop = 0;
    }
    double kd = fused(kGexpInvLn2N, x, kGexpShift);
    const uint64_t ki = bitsOf(kd);
    kd -= kGexpShift;
    const double r = fused(kd, kGexpNegLn2LoN, fused(kd, kGexpNegLn2HiN, x));
    const uint64_t idx = 2 * (ki % 128);
    const uint64_t top = ki << (52 - 7);
    const double tail = fromBits(kGlibcExpTable[idx]);
    const uint64_t sbits = kGlibcExpTable[idx + 1] + top;
    const double r2 = r * r;
    const double tmp = fused(r2 * r2, fused(r, kGexpC5, kGexpC4), fused(r2, fused(r, kGexpC3, kGexpC2), tail + r));
    if (abstop == 0) return glibcExpSpecial(tmp, sbits, ki);
    const double scale = fromBits(sbits);
    return fused(scale, tmp, scale);
}

// ---- expm1 ----

constexpr double kGHuge = 1.0e+300, kGTiny = 1.0e-300, kGOverflow = 7.09782712893383973096e+02,
                 kGLn2Hi = 6.93147180369123816490e-01, kGLn2Lo = 1.90821492927058770002e-10,
                 kGInvLn2 = 1.44269504088896338700e+00;
constexpr double kGQ[] = {1.0, -3.33333333333331316428e-02, 1.58730158725481460165e-03, -7.93650757867487942473e-05,
                          4.00821782732936239552e-06, -2.01099218183624371326e-07};

double glibcExpm1(double x) {
    double y, hi, lo, c = 0, t, e;
    int32_t k;
    uint32_t hx = highWord(x);
    const uint32_t xsb = hx & 0x80000000;
    hx &= 0x7fffffff;
    if (hx >= 0x4043687A) {
        if (hx >= 0x40862E42) {
            if (hx >= 0x7ff00000) {
                if (((hx & 0xfffff) | lowWord(x)) != 0) return x + x;
                return xsb == 0 ? x : -1.0;
            }
            if (x > kGOverflow) return kGHuge * kGHuge;
        }
        if (xsb != 0) return kGTiny - 1.0;
    }
    if (hx > 0x3fd62e42) {
        if (hx < 0x3FF0A2B2) {
            if (xsb == 0) {
                hi = x - kGLn2Hi;
                lo = kGLn2Lo;
                k = 1;
            } else {
                hi = x + kGLn2Hi;
                lo = -kGLn2Lo;
                k = -1;
            }
            x = hi - lo;
            c = (hi - x) - lo;
        } else {
            k = static_cast<int32_t>(fused(kGInvLn2, x, xsb == 0 ? 0.5 : -0.5));
            t = k;
            hi = fused(-t, kGLn2Hi, x);
            // lo = t * ln2_lo has two uses, both subtractions in its block: GCC fuses it into each.
            x = fused(-t, kGLn2Lo, hi);
            c = fused(-t, kGLn2Lo, hi - x);
        }
    } else if (hx < 0x3c900000) {
        t = kGHuge + x;
        return x - (t - (kGHuge + x));
    } else {
        k = 0;
    }
    const double hfx = 0.5 * x;
    const double hxs = x * hfx;
    const double R1 = fused(hxs, kGQ[1], 1.0);
    const double h2 = hxs * hxs;
    const double R2 = fused(hxs, kGQ[3], kGQ[2]);
    const double h4 = h2 * h2;
    const double R3 = fused(hxs, kGQ[5], kGQ[4]);
    const double r1 = fused(h4, R3, fused(h2, R2, R1));
    t = fused(-r1, hfx, 3.0);
    e = hxs * ((r1 - t) / fused(-x, t, 6.0));
    if (k == 0) return x - fused(x, e, -hxs);
    e = fused(x, e - c, -c);
    e -= hxs;
    if (k == -1) return fused(0.5, x - e, -0.5);
    if (k == 1) {
        if (x < -0.25) return -2.0 * (e - (x + 0.5));
        return fused(2.0, x - e, 1.0);
    }
    if (k <= -2 || k > 56) {
        y = 1.0 - (e - x);
        y = withHighWord(y, highWord(y) + (static_cast<uint32_t>(k) << 20));
        return y - 1.0;
    }
    t = 1.0;
    if (k < 20) {
        t = withHighWord(t, 0x3ff00000 - (0x200000u >> k));
        y = t - (e - x);
    } else {
        t = withHighWord(t, static_cast<uint32_t>(0x3ff - k) << 20);
        y = x - (e + t);
        y += 1.0;
    }
    return withHighWord(y, highWord(y) + (static_cast<uint32_t>(k) << 20));
}

// ---- log1p ----

constexpr double kGLp[] = {0.0, 6.666666666666735130e-01, 3.999999999940941908e-01, 2.857142874366239149e-01,
                           2.222219843214978396e-01, 1.818357216161805012e-01, 1.531383769920937332e-01,
                           1.479819860511658591e-01};

double glibcLog1p(double x) {
    double f = 0, c = 0;
    int32_t k = 1, hu = 0;
    const int32_t hx = static_cast<int32_t>(highWord(x));
    const int32_t ax = hx & 0x7fffffff;
    if (hx < 0x3FDA827A) {
        if (ax >= 0x3ff00000) return x == -1.0 ? -kInf : kNaN;
        if (ax < 0x3e200000) {
            if (ax < 0x3c900000) return x;
            return fused(-(x * x), 0.5, x);
        }
        if (hx > 0 || hx <= static_cast<int32_t>(0xbfd2bec3)) {
            k = 0;
            f = x;
            hu = 1;
        }
    } else if (hx >= 0x7ff00000) {
        return x + x;
    }
    if (k != 0) {
        double u;
        if (hx < 0x43400000) {
            u = 1.0 + x;
            hu = static_cast<int32_t>(highWord(u));
            k = (hu >> 20) - 1023;
            c = (k > 0) ? 1.0 - (u - x) : x - (u - 1.0);
            c /= u;
        } else {
            u = x;
            hu = static_cast<int32_t>(highWord(u));
            k = (hu >> 20) - 1023;
            c = 0;
        }
        hu &= 0x000fffff;
        if (hu < 0x6a09e) {
            u = withHighWord(u, static_cast<uint32_t>(hu | 0x3ff00000));
        } else {
            k += 1;
            u = withHighWord(u, static_cast<uint32_t>(hu | 0x3fe00000));
            hu = (0x00100000 - hu) >> 2;
        }
        f = u - 1.0;
    }
    const double hfsq = 0.5 * f * f;
    const double dk = k;
    if (hu == 0) {
        if (f == 0.0) {
            if (k == 0) return 0.0;
            c = fused(dk, kGLn2Lo, c);
            return fused(dk, kGLn2Hi, c);
        }
        const double R = hfsq * fused(-0.66666666666666666, f, 1.0);
        if (k == 0) return f - R;
        return fused(dk, kGLn2Hi, -((R - fused(dk, kGLn2Lo, c)) - f));
    }
    const double s = f / (2.0 + f);
    const double z = s * s;
    const double z2 = z * z;
    const double R2 = fused(z, kGLp[3], kGLp[2]);
    const double z4 = z2 * z2;
    const double R3 = fused(z, kGLp[5], kGLp[4]);
    const double z6 = z4 * z2;
    const double R4 = fused(z, kGLp[7], kGLp[6]);
    // R1 + z2*R2 + z4*R3 + z6*R4, GCC fusing z*Lp[1] into the first sum and the last two products.
    const double R = fused(z6, R4, fused(z4, R3, fused(z, kGLp[1], z2 * R2)));
    if (k == 0) return f - (hfsq - s * (hfsq + R));
    return fused(dk, kGLn2Hi, -((hfsq - (s * (hfsq + R) + fused(dk, kGLn2Lo, c))) - f));
}

// ---- sinh, cosh, tanh ----

double glibcSinh(double x) {
    const int32_t jx = static_cast<int32_t>(highWord(x));
    const int32_t ix = jx & 0x7fffffff;
    if (ix >= 0x7ff00000) return x + x;
    const double h = jx < 0 ? -0.5 : 0.5;
    if (ix < 0x40360000) {
        if (ix < 0x3e300000 && 1.0e307 + x > 1.0) return x;
        const double t = glibcExpm1(absOf(x));
        if (ix < 0x3ff00000) return h * fused(2.0, t, -(t * t / (t + 1.0)));
        return h * (t + t / (t + 1.0));
    }
    if (ix < 0x40862e42) return h * glibcExp(absOf(x));
    const uint32_t lx = lowWord(x);
    if (ix < 0x408633ce || (ix == 0x408633ce && lx <= 0x8fb9f87du)) {
        const double w = glibcExp(0.5 * absOf(x));
        const double t = h * w;
        return t * w;
    }
    return x * 1.0e307;
}

double glibcCosh(double x) {
    const int32_t ix = static_cast<int32_t>(highWord(x)) & 0x7fffffff;
    if (ix < 0x40360000) {
        if (ix < 0x3fd62e43) {
            if (ix < 0x3c800000) return 1.0;
            const double t = glibcExpm1(absOf(x));
            const double w = 1.0 + t;
            return 1.0 + (t * t) / (w + w);
        }
        const double t = glibcExp(absOf(x));
        return 0.5 * t + 0.5 / t;
    }
    if (ix < 0x40862e42) return 0.5 * glibcExp(absOf(x));
    if ((bitsOf(x) & 0x7fffffffffffffffull) <= 0x408633ce8fb9f87dull) {
        const double w = glibcExp(0.5 * absOf(x));
        const double t = 0.5 * w;
        return t * w;
    }
    if (ix >= 0x7ff00000) return x * x;
    return kGHuge * kGHuge;
}

double glibcTanh(double x) {
    const int32_t jx = static_cast<int32_t>(highWord(x)), lx = static_cast<int32_t>(lowWord(x));
    const int32_t ix = jx & 0x7fffffff;
    double z;
    if (ix >= 0x7ff00000) return jx >= 0 ? 1.0 / x + 1.0 : 1.0 / x - 1.0;
    if (ix < 0x40360000) {
        if ((ix | lx) == 0) return x;
        if (ix < 0x3c800000) return x * (1.0 + x);
        if (ix >= 0x3ff00000) {
            const double t = glibcExpm1(2.0 * absOf(x));
            z = 1.0 - 2.0 / (t + 2.0);
        } else {
            const double t = glibcExpm1(-2.0 * absOf(x));
            z = -t / (t + 2.0);
        }
    } else {
        z = 1.0 - kGTiny;
    }
    return jx >= 0 ? z : -z;
}
