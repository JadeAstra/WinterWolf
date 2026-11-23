# Complex's hypot (src/std/complex.ww, __cmHypot: Borges's corrected algorithm, "An Improved
# Algorithm for hypot(a,b)", 2019, without fused steps) written out in Python, against the C
# library's hypot, which CPython's cmath and abs() of a complex call. Run in WSL, on glibc:
#
#   python3.14 tools/complex/glibc_hypot.py
#
# It prints how many of 1,000,000 pairs (ordinary, all exponents, parts far apart, near the
# scaling thresholds 2^511 and 2^-500, subnormal) differ: 0 on glibc 2.43.
import ctypes
import math
import random

libm = ctypes.CDLL("libm.so.6")
libm.hypot.restype = ctypes.c_double
libm.hypot.argtypes = [ctypes.c_double] * 2


def kernel(x, y):
    h = math.sqrt(x * x + y * y)
    if h <= 2.0 * y:
        d = h - y
        t1 = x * (2.0 * d - x)
        t2 = (d - 2.0 * (x - y)) * d
    else:
        d = h - x
        t1 = 2.0 * d * (x - 2.0 * y)
        t2 = (4.0 * d - y) * y + d * d
    return h - (t1 + t2) / (2.0 * h)


def hypot(a, b):
    x, y = abs(a), abs(b)
    if math.isinf(x) or math.isinf(y):
        return math.inf
    if math.isnan(x) or math.isnan(y):
        return x + y
    if x < y:
        x, y = y, x
    if y <= x * 2.0 ** -54:
        return x + y
    if x > 2.0 ** 511:
        return kernel(x * 2.0 ** -600, y * 2.0 ** -600) * 2.0 ** 600
    if y < 2.0 ** -500:
        return kernel(x * 2.0 ** 600, y * 2.0 ** 600) * 2.0 ** -600
    return kernel(x, y)


def pair(i):
    k = i % 8
    r = random.random
    if k == 0:
        return random.uniform(-20, 20), random.uniform(-20, 20)
    if k == 1:
        e = random.randrange(-1074, 964)
        return math.ldexp(r(), e), math.ldexp(r(), e + random.randrange(-60, 60))
    if k == 2:
        return math.ldexp(r(), random.randrange(-1074, 1024)), math.ldexp(r(), random.randrange(-1074, 1024))
    if k == 3:
        a = random.uniform(0.5, 2)
        return a, a * r()
    if k == 4:
        return math.ldexp(r(), random.randrange(495, 530)), math.ldexp(r(), random.randrange(440, 530))
    if k == 5:
        return math.ldexp(r(), random.randrange(-530, -380)), math.ldexp(r(), random.randrange(-1074, -380))
    if k == 6:
        return math.ldexp(r(), random.randrange(-512, -495)), math.ldexp(r(), random.randrange(-512, -495))
    return math.ldexp(r(), random.randrange(1000, 1024)), math.ldexp(r(), random.randrange(960, 1024))


random.seed(2026)
differ = 0
for i in range(1000000):
    a, b = pair(i)
    if hypot(a, b) != libm.hypot(a, b):
        differ += 1
        if differ <= 5:
            print("differs:", a.hex(), b.hex(), hypot(a, b).hex(), libm.hypot(a, b).hex())
print("differ", differ, "of 1000000")
