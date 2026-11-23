# What CPython 3.14's cmath gives on Linux for each of its sixteen functions over every pair of
# awkward values (zeros of both signs, a subnormal, values about 1, past exp's reach, past
# DBL_MAX / 4, infinities, NaN), errors too; and abs() of a complex over many pairs; each folded
# into one number, for tests/cmath_python.ww: ww's Complex must give the same. Run in WSL:
#
#   python3.14 tools/complex/python_cmath.py
#
# Each number: h = (h * 1000003 + v + 1) mod 1000000007 for each part of each answer, v its
# float's 64 bits mod 1000000007 (NaN as 1, whatever its sign: ww keeps one NaN), ValueError as 2
# and OverflowError as 3 in place of an answer. The pairs for abs(): for i below 20000,
# (s * ldexp(0.5 + frac(i g), e1), ldexp(0.5 + frac(i h), e2)), g = 0.6180339887498949,
# h = 0.7548776662466927, the exponents from i as below, s -1 for odd i.
import cmath
import math
import struct

M = 1000000007
inf, nan = math.inf, math.nan
base = [0.0, 1e-310, 1e-200, 1e-8, 0.3, 0.5, 0.9, 1.0, 1.1, 2.0, 10.0, 100.0, 708.0, 710.0, 1e150, 1e300, 1.7e308]
vals = base + [-x for x in base] + [inf, -inf, nan]
funcs = ["sqrt", "exp", "log", "log10", "sin", "cos", "tan", "sinh", "cosh", "tanh", "asin", "acos", "atan", "asinh", "acosh", "atanh"]


def part(x):
    if math.isnan(x):
        return 1
    return struct.unpack("<q", struct.pack("<d", x))[0] % M


def fold(h, z):
    if isinstance(z, int):
        return (h * 1000003 + z) % M
    for x in (z.real, z.imag):
        h = (h * 1000003 + part(x) + 1) % M
    return h


for name in funcs:
    f = getattr(cmath, name)
    h = 0
    for a in vals:
        for b in vals:
            try:
                h = fold(h, f(complex(a, b)))
            except ValueError:
                h = fold(h, 2)
            except OverflowError:
                h = fold(h, 3)
    print(name, h)

G, H = 0.6180339887498949, 0.7548776662466927
h = 0
for i in range(20000):
    e1 = [0, -3, 40, -600, 515, -1030, 900][i % 7] + (i * 7919) % 40 - 20
    e2 = e1 + (i * 104729) % 120 - 60
    a = (-1.0 if i % 2 else 1.0) * math.ldexp(0.5 + math.fmod(i * G, 1.0), e1)
    b = math.ldexp(0.5 + math.fmod(i * H, 1.0), e2)
    try:
        r = abs(complex(a, b))
        h = (h * 1000003 + part(r) + 1) % M
    except OverflowError:
        h = (h * 1000003 + 3) % M
print("abs", h)
