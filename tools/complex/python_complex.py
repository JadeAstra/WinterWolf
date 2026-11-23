# What CPython 3.14 gives for complex products and quotients, a real divided by a complex, and
# cmath.rect, over every pair of awkward values (zeros of both signs, a subnormal, the largest
# floats, infinities, NaN), folded into one number each, for tests/complex_python.ww: ww's
# Complex must give the same (C11's Annex G, as CPython does it). Run in WSL:
#
#   python3.14 tools/complex/python_complex.py
#
# Each number: h = (h * 1000003 + v + 1) mod 1000000007 for each part of each answer, v its
# float's 64 bits mod 1000000007 (NaN as 1, an error as 2), in the order the loops go.
import cmath
import itertools
import math
import struct

M = 1000000007
inf, nan = math.inf, math.nan
vals = [0.0, -0.0, 1.0, -2.5, 1e308, 1e-310, inf, -inf, nan]
angles = [0.0, -0.0, 1.0, -2.5, 3.0, 1e-310, 100.0, inf, -inf, nan]


def part(x):
    if math.isnan(x):
        return 1
    return struct.unpack("<q", struct.pack("<d", x))[0] % M


def fold(h, z):
    if z is None:
        return (h * 1000003 + 3) % M
    for x in (z.real, z.imag):
        h = (h * 1000003 + part(x) + 1) % M
    return h


products = quotients = reals = rects = 0
for a, b, c, d in itertools.product(vals, vals, vals, vals):
    products = fold(products, complex(a, b) * complex(c, d))
    if not (c == 0.0 and d == 0.0):
        quotients = fold(quotients, complex(a, b) / complex(c, d))
for a, c, d in itertools.product(vals, vals, vals):
    if not (c == 0.0 and d == 0.0):
        reals = fold(reals, a / complex(c, d))
for r, t in itertools.product(vals, angles):
    try:
        rects = fold(rects, cmath.rect(r, t))
    except ValueError:
        rects = fold(rects, None)
print("products %d\nquotients %d\nreals %d\nrects %d" % (products, quotients, reals, rects))
