# What CPython 3.14 on Linux (glibc 2.43's libm) gives for exp, expm1, log1p, sinh, cosh and tanh
# over many inputs through every branch, folded into one number each, for tests/glibc_math.ww:
# ww's own versions of these six are glibc's, bit for bit (src/fmath_glibc.inc). Run in WSL:
#
#   python3.14 tools/fmath/glibc_digest.py
#
# Inputs: for range (lo, hi) the i-th is lo + (hi - lo) * frac(i * g), g = 0.6180339887498949;
# for an exponent range (e0, e1) and sign s it is s * ldexp(0.5 + frac(i * g), e0 + (i * 7919) mod
# (e1 - e0)); then some awkward values. Each number: h = (h * 1000003 + v + 1) mod 1000000007 for
# each answer, v its float's 64 bits mod 1000000007 (NaN as 1, an error as 2). "inputs" folds the
# inputs themselves, so that a difference there shows apart.
import math
import struct

M = 1000000007
G = 0.6180339887498949
N = 3000


def part(x):
    if math.isnan(x):
        return 1
    return struct.unpack("<q", struct.pack("<d", x))[0] % M


def frac(i):
    return math.fmod(i * G, 1.0)


def spread(lo, hi):
    return [lo + (hi - lo) * frac(i) for i in range(N)]


def logspread(e0, e1, s):
    return [s * math.ldexp(0.5 + frac(i), e0 + (i * 7919) % (e1 - e0)) for i in range(N)]


special = [0.0, -0.0, math.inf, -math.inf, math.nan, 1e-320, -1e-320, 5e-324, 1e-300, 1e-17, -1e-17, 0.5, -0.5,
           1.0, -1.0, 22.0, -22.0, 709.78, 709.79, -745.13, -745.14, 710.5, 711.0, -708.0, 0.34657359027997264,
           1.0397207708399179, -1.0, -0.9999999999999999, 1e300, 1.7976931348623157e308]
sets = [
    ("exp", spread(-745.5, 710) + spread(-1, 1) + logspread(-60, 0, 1) + logspread(-60, 0, -1) + spread(-745.5, -700) + spread(700, 709.8)),
    ("expm1", spread(-40, 710) + spread(-1.1, 1.1) + logspread(-60, 0, 1) + logspread(-60, 0, -1) + spread(-0.6, 0.6) + spread(-38, -1)),
    ("log1p", spread(-0.999999, 1) + logspread(-60, 0, 1) + logspread(-60, -1, -1) + logspread(0, 1023, 1) + spread(-0.3, 0.42)),
    ("sinh", spread(-30, 30) + spread(-1, 1) + logspread(-60, 0, 1) + spread(-711, 711) + spread(709, 711)),
    ("cosh", spread(-30, 30) + spread(-0.4, 0.4) + logspread(-60, 0, 1) + spread(-711, 711) + spread(709, 711)),
    ("tanh", spread(-25, 25) + spread(-1.2, 1.2) + logspread(-60, 0, 1) + logspread(-60, 0, -1)),
]
inputs = 0
for name, xs in sets:
    f = getattr(math, name)
    h = 0
    for x in xs + special:
        inputs = (inputs * 1000003 + part(x) + 1) % M
        try:
            v = part(f(x))
        except (OverflowError, ValueError):
            v = 2
        h = (h * 1000003 + v + 1) % M
    print(name, h)
print("inputs", inputs)
