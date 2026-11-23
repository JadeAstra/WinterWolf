# What CPython 3.14 on Linux gives for the functions docs/math-plan.html gave ww (erf, erfc, fma,
# remainder, hypot and dist of many, fsum, sumprod, factorial, comb, perm, lcm), and the nearest
# float to each cube root (worked out exactly here: glibc's cbrt is not always the nearest, and
# ww's is), each over many inputs, folded into one number, for tests/math_more.ww. Run in WSL:
#
#   python3.14 tools/fmath/python_more.py
#
# Inputs: the i-th from frac(i * g) = fmod(i * g, 1.0), g = 0.6180339887498949, as below. Each
# number: h = (h * 1000003 + v + 1) mod 1000000007 for each answer, v its float's 64 bits (or the
# int) mod 1000000007, NaN as 1 and an error as 2. Printed for 4000 inputs each, then for the
# first 200, which the test takes under GC stress (WW_TEST_QUICK=1).
import math
import struct
from fractions import Fraction

M = 1000000007
G = 0.6180339887498949
N = 4000


def frac(i):
    return math.fmod(i * G, 1.0)


def part(x):
    if isinstance(x, int):
        return x % M
    if math.isnan(x):
        return 1
    return struct.unpack("<q", struct.pack("<d", x))[0] % M


def fold(h, v):
    return (h * 1000003 + v + 1) % M


def ldexp_input(i, lo, hi):
    return math.ldexp(0.5 + frac(i), lo + (i * 7919) % (hi - lo))


def run(f):
    h = 0
    for i in range(N_NOW):
        try:
            h = fold(h, part(f(i)))
        except (ValueError, OverflowError):
            h = fold(h, 2)
    return h


def cbrt_nearest(x):
    # The float nearest x's real cube root: bisection on the floats' bits, the cubes compared exactly.
    if x == 0 or not math.isfinite(x):
        return x
    ax = Fraction(abs(x))
    lo, hi = 0, 0x7FEFFFFFFFFFFFFF
    while hi - lo > 1:
        mid = (lo + hi) // 2
        if Fraction(struct.unpack("<d", struct.pack("<q", mid))[0]) ** 3 <= ax:
            lo = mid
        else:
            hi = mid
    a = struct.unpack("<d", struct.pack("<q", lo))[0]
    b = struct.unpack("<d", struct.pack("<q", hi))[0]
    mid = (Fraction(a) + Fraction(b)) / 2
    r = a if mid ** 3 > ax else b if mid ** 3 < ax else (a if lo % 2 == 0 else b)
    return math.copysign(r, x)


for N_NOW in (N, 200):
  results = {
      "erf": run(lambda i: math.erf(-7 + 14 * frac(i))),
      "erfc": run(lambda i: math.erfc(-7 + 35 * frac(i))),
      "erfSmall": run(lambda i: math.erf((-1 if i % 2 else 1) * ldexp_input(i, -1074, 0))),
      "fma": run(lambda i: math.fma(-10 + 20 * frac(i), -10 + 20 * frac(i + 1), -100 + 200 * frac(i + 2))),
      "remainder": run(lambda i: math.remainder((-1 if i % 2 else 1) * ldexp_input(i, -1074, 1024), ldexp_input(i + 5, -1074, 1024))),
      "hypot": run(lambda i: math.hypot(ldexp_input(i, -1074, 1024), ldexp_input(i + 1, -200, 200), -ldexp_input(i + 2, -200, 200))),
      "dist": run(lambda i: math.dist([frac(i), 3 * frac(i + 1)], [-frac(i + 2), frac(i + 3)])),
      "fsum": run(lambda i: math.fsum([(-1 if (i + k) % 3 == 0 else 1) * ldexp_input(i + k, -60, 60) for k in range(1 + i % 9)])),
      "sumprod": run(lambda i: math.sumprod([frac(i + k) for k in range(1 + i % 7)], [-frac(i + k + 3) for k in range(1 + i % 7)])),
      "cbrt": run(lambda i: cbrt_nearest((-1 if i % 2 else 1) * ldexp_input(i, -1074, 1024))),
      "integers": run(lambda i: math.comb(i % 200, (i * 7) % 60) + math.perm(i % 40, i % 12) + math.factorial(i % 30) + math.lcm(i, 360)),
  }
  print("quick" if N_NOW == 200 else "whole", [v for v in results.values()])
