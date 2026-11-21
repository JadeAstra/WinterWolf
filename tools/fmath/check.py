#!/usr/bin/env python3
# Measures WinterWolf's own math functions (src/fmath.cpp) against the true values, worked out
# with Python's decimal: for each function, inputs drawn at random over its whole range (and
# its edges), each result's distance from the true value in ulps; the largest, and how many
# are not the nearest double. Also a hash of every result's bits, which must be the same on
# every system (o-complex-linux).
#
#   python3 tools/fmath/check.py <fmath_check program> [count per function] [seed]
#
# (tools/fmath/check.cpp says how to build the program.)
import random
import struct
import subprocess
import sys
import hashlib
from decimal import Decimal, getcontext, localcontext

sys.path.insert(0, __file__.rsplit("/", 1)[0])
import tables  # noqa: E402  (pi and atan to many digits)

PREC = 70
PI = +tables.PI


def bits(x):
    return struct.unpack("<Q", struct.pack("<d", x))[0]


def from_bits(b):
    return struct.unpack("<d", struct.pack("<Q", b))[0]


def ulp_of(v):
    """The ulp of the double nearest v (v finite, not 0)."""
    d = abs(float(v))
    if d == 0.0:
        return Decimal(2) ** -1074
    b = bits(d)
    e = (b >> 52) & 0x7FF
    if e == 0:
        return Decimal(2) ** -1074
    return Decimal(2) ** (e - 1075)


# ---- The true values ----

def reduce_pi2(x):
    """x mod pi/2: (n mod 4, y) with |y| <= pi/4, x exact."""
    with localcontext() as c:
        c.prec = 800
        pio2 = tables.PI / 2
        n = (x / pio2).to_integral_value()
        y = x - n * pio2
    return int(n) % 4, +y


def series_sin(y):
    with localcontext() as c:
        c.prec = PREC + 10
        total, term, k = Decimal(0), y, 1
        y2 = y * y
        while term != 0 and abs(term) > Decimal(10) ** -(PREC + 20) * max(abs(total), Decimal(10) ** -400):
            total += term
            term = -term * y2 / ((2 * k) * (2 * k + 1))
            k += 1
        return total


def series_cos(y):
    with localcontext() as c:
        c.prec = PREC + 10
        total, term, k = Decimal(0), Decimal(1), 1
        y2 = y * y
        while term != 0 and abs(term) > Decimal(10) ** -(PREC + 20):
            total += term
            term = -term * y2 / ((2 * k - 1) * (2 * k))
            k += 1
        return total


def t_sin(x):
    n, y = reduce_pi2(x)
    return [series_sin(y), series_cos(y), -series_sin(y), -series_cos(y)][n]


def t_cos(x):
    n, y = reduce_pi2(x)
    return [series_cos(y), -series_sin(y), -series_cos(y), series_sin(y)][n]


def t_tan(x):
    return t_sin(x) / t_cos(x)


def t_atan(x):
    if x < 0:
        return -t_atan(-x)
    if x > 1:
        return PI / 2 - tables.atan(1 / x)
    return tables.atan(x)


def t_atan2(y, x):
    if x > 0:
        return t_atan(y / x)
    a = t_atan(abs(y) / -x) if x < 0 else PI / 2
    r = PI - a if x < 0 else a
    return r if y > 0 else -r


def t_asin(x):
    if abs(x) == 1:
        return x * PI / 2
    return t_atan(x / (1 - x * x).sqrt())


def t_acos(x):
    with localcontext() as c:
        c.prec = PREC + 30
        return PI / 2 - t_asin(x)


def t_exp(x):
    return x.exp()


def t_expm1(x):
    if abs(x) < Decimal("1e-8"):
        return x + x * x / 2 + x ** 3 / 6 + x ** 4 / 24
    with localcontext() as c:
        c.prec = PREC + 30
        return x.exp() - 1


def t_log(x):
    return x.ln()


def t_log2(x):
    return x.ln() / Decimal(2).ln()


def t_log10(x):
    return x.log10()


def t_log1p(x):
    if abs(x) < Decimal("1e-20"):
        return x - x * x / 2 + x ** 3 / 3
    with localcontext() as c:
        c.prec = 1200
        return (1 + x).ln()


def t_sinh(x):
    if abs(x) < Decimal("1e-8"):
        return x + x ** 3 / 6 + x ** 5 / 120
    with localcontext() as c:
        c.prec = PREC + 20
        return (x.exp() - (-x).exp()) / 2


def t_cosh(x):
    return (x.exp() + (-x).exp()) / 2


def t_tanh(x):
    if abs(x) < Decimal("1e-8"):
        return x - x ** 3 / 3
    with localcontext() as c:
        c.prec = PREC + 20
        e = (2 * x).exp()
        return (e - 1) / (e + 1)


def t_asinh(x):
    if abs(x) < Decimal("1e-8"):
        return x - x ** 3 / 6
    with localcontext() as c:
        c.prec = PREC + 20
        a = abs(x)
        r = (a + (a * a + 1).sqrt()).ln()
        return r if x > 0 else -r


def t_acosh(x):
    with localcontext() as c:
        c.prec = PREC + 40
        return (x + (x * x - 1).sqrt()).ln()


def t_atanh(x):
    if abs(x) < Decimal("1e-8"):
        return x + x ** 3 / 3
    with localcontext() as c:
        c.prec = PREC + 20
        return ((1 + x) / (1 - x)).ln() / 2


def t_pow(x, y):
    if x < 0:
        r = t_pow(-x, y)
        return -r if int(y) % 2 == 1 else r
    if y == y.to_integral_value() and abs(y) <= 64:
        with localcontext() as c:
            c.prec = 2000
            return x ** int(y)
    with localcontext() as c:
        c.prec = PREC + 30
        return (y * x.ln()).exp()


def t_hypot(x, y):
    with localcontext() as c:
        c.prec = 1500
        s = x * x + y * y
    return s.sqrt()


def t_logbase(x, b):
    with localcontext() as c:
        c.prec = PREC + 20
        return x.ln() / b.ln()


# ---- Inputs ----

def wide(rng, lo_exp, hi_exp, signed=True):
    """A double of random significand and an exponent from lo_exp to hi_exp."""
    m = 1 + rng.random()
    x = m * 2.0 ** rng.randint(lo_exp, hi_exp)
    return -x if signed and rng.random() < 0.5 else x


def uniform(rng, a, b):
    return rng.uniform(a, b)


FUNCS = {
    # name: (true value, input maker, number of arguments, where the true value is defined)
    "sin": (t_sin, lambda r: r.choice([uniform(r, -10, 10), wide(r, -30, 1023), uniform(r, -1e6, 1e6)]), 1),
    "cos": (t_cos, lambda r: r.choice([uniform(r, -10, 10), wide(r, -30, 1023), uniform(r, -1e6, 1e6)]), 1),
    "tan": (t_tan, lambda r: r.choice([uniform(r, -10, 10), wide(r, -30, 1023), uniform(r, -1e6, 1e6)]), 1),
    "asin": (t_asin, lambda r: r.choice([uniform(r, -1, 1), wide(r, -60, -1), 1 - wide(r, -53, -1, False)]), 1),
    "acos": (t_acos, lambda r: r.choice([uniform(r, -1, 1), wide(r, -60, -1), 1 - wide(r, -53, -1, False), -1 + wide(r, -53, -1, False)]), 1),
    "atan": (t_atan, lambda r: r.choice([uniform(r, -4, 4), wide(r, -40, 70)]), 1),
    "atan2": (t_atan2, lambda r: (r.choice([uniform(r, -4, 4), wide(r, -100, 100)]), r.choice([uniform(r, -4, 4), wide(r, -100, 100)])), 2),
    "exp": (t_exp, lambda r: r.choice([uniform(r, -745, 709.7), uniform(r, -2, 2), wide(r, -60, 9)]), 1),
    "expm1": (t_expm1, lambda r: r.choice([uniform(r, -40, 709.7), uniform(r, -1, 1), wide(r, -60, 5), wide(r, -54, -40)]), 1),
    "log": (t_log, lambda r: r.choice([wide(r, -1074, 1023, False), uniform(r, 0.5, 2), 1 + wide(r, -52, -2)]), 1),
    "log2": (t_log2, lambda r: r.choice([wide(r, -1074, 1023, False), uniform(r, 0.5, 2), 1 + wide(r, -52, -2)]), 1),
    "log10": (t_log10, lambda r: r.choice([wide(r, -1074, 1023, False), uniform(r, 0.5, 2), 1 + wide(r, -52, -2)]), 1),
    "log1p": (t_log1p, lambda r: r.choice([wide(r, -60, 1023, False), uniform(r, -0.9999, 2), wide(r, -60, -1)]), 1),
    "logbase": (t_logbase, lambda r: (wide(r, -100, 100, False), r.choice([uniform(r, 1.01, 40), uniform(r, 0.01, 0.99), 3.0, 16.0])), 2),
    "sinh": (t_sinh, lambda r: r.choice([uniform(r, -710, 710), uniform(r, -2, 2), wide(r, -40, 4)]), 1),
    "cosh": (t_cosh, lambda r: r.choice([uniform(r, -710, 710), uniform(r, -2, 2), wide(r, -40, 4)]), 1),
    "tanh": (t_tanh, lambda r: r.choice([uniform(r, -25, 25), uniform(r, -1, 1), wide(r, -40, 2)]), 1),
    "asinh": (t_asinh, lambda r: r.choice([uniform(r, -4, 4), wide(r, -40, 1023)]), 1),
    "acosh": (t_acosh, lambda r: r.choice([uniform(r, 1, 4), 1 + wide(r, -52, 0, False), wide(r, 1, 1023, False)]), 1),
    "atanh": (t_atanh, lambda r: r.choice([uniform(r, -1, 1), wide(r, -40, -1), 1 - wide(r, -53, -1, False)]), 1),
    "pow": (t_pow, lambda r: r.choice([
        (uniform(r, 0, 10), uniform(r, -10, 10)),
        (wide(r, -100, 100, False), uniform(r, -6, 6)),
        (1 + wide(r, -40, -1), uniform(r, -1e9, 1e9)),
        (float(r.randint(-20, 20)), float(r.randint(-30, 30))),
        (uniform(r, -10, 10), float(r.randint(1, 8))),
        (float(r.randint(2, 100000)), float(r.randint(2, 12))),
        (uniform(r, 0.5, 2), uniform(r, -700, 700))]), 2),
    "hypot": (t_hypot, lambda r: r.choice([(uniform(r, -10, 10), uniform(r, -10, 10)), (wide(r, -1074, 1023), wide(r, -1074, 1023)), (wide(r, -500, 500), wide(r, -500, 500))]), 2),
}


def main():
    program = sys.argv[1]
    count = int(sys.argv[2]) if len(sys.argv) > 2 else 3000
    seed = int(sys.argv[3]) if len(sys.argv) > 3 else 1
    only = sys.argv[4].split(",") if len(sys.argv) > 4 else None
    getcontext().prec = PREC
    rng = random.Random(seed)
    cases = []
    for name, (_, make, arity) in FUNCS.items():
        if only and name not in only:
            continue
        for _ in range(count):
            a = make(rng)
            args = a if isinstance(a, tuple) else (a,)
            cases.append((name, args))
    text = "".join("%s %016x %016x\n" % (n, bits(a[0]), bits(a[1] if len(a) > 1 else 0.0)) for n, a in cases)
    out = subprocess.run([program], input=text, capture_output=True, text=True, check=True).stdout.split()
    assert len(out) == len(cases), (len(out), len(cases))
    digest = hashlib.sha256("".join(out).encode()).hexdigest()[:16]
    stats = {}
    for (name, args), got_hex in zip(cases, out):
        got = from_bits(int(got_hex, 16))
        s = stats.setdefault(name, {"n": 0, "worst": 0.0, "worst_at": None, "not_nearest": 0, "skipped": 0})
        true_fn = FUNCS[name][0]
        dargs = [Decimal(a) for a in args]
        try:
            truth = true_fn(*dargs)
        except Exception:
            s["skipped"] += 1
            continue
        nearest = float(truth) if abs(truth) < Decimal("1.797693134862315708e308") else float("inf")
        if nearest in (float("inf"), float("-inf")) or got in (float("inf"), float("-inf")) or got != got:
            if got != nearest:
                s["not_nearest"] += 1
                s["worst"] = float("inf")
                s["worst_at"] = (args, got, nearest)
            s["n"] += 1
            continue
        err = abs((Decimal(got) - truth) / ulp_of(truth))
        s["n"] += 1
        if got != nearest:
            s["not_nearest"] += 1
        if float(err) > s["worst"]:
            s["worst"] = float(err)
            s["worst_at"] = (args, got, nearest)
    print("results' hash %s (%d results)" % (digest, len(out)))
    print("%-8s %7s %10s %12s   worst at" % ("function", "inputs", "worst ulp", "not nearest"))
    bad = False
    for name, s in stats.items():
        print("%-8s %7d %10.3f %12d   %s" % (name, s["n"], s["worst"], s["not_nearest"], s["worst_at"]))
        bad = bad or s["worst"] >= 1.0
    bad = specials(program) > 0 or bad
    sys.exit(1 if bad else 0)


# ---- Special values, against the C library of the system this runs on (glibc in WSL), which
# keeps C's Annex F: every pair of these, each result's kind (NaN, an infinity, a zero and its
# sign) the same, and a finite one within an ulp of the C library's. ----

SPECIALS = [0.0, -0.0, float("inf"), float("-inf"), float("nan"), 1.0, -1.0, 0.5, -0.5, 2.0, -2.0, 3.0, -3.0,
            0.1, -0.1, 1e-310, -1e-310, 5e-324, -5e-324, 2.2250738585072014e-308, 1.7976931348623157e308,
            -1.7976931348623157e308, 1.5707963267948966, 3.141592653589793, 1e22, 1e300, 0.9999999999999999,
            1.0000000000000002, 710.0, -746.0, 22.0, 1e-20, 2.0 ** 53, 2.0 ** 53 + 2, -(2.0 ** 53) - 2, 0.5000000000000001]


def specials(program):
    import ctypes
    import math
    libm = ctypes.CDLL("libm.so.6")
    one = {}
    for name in ["sin", "cos", "tan", "asin", "acos", "atan", "exp", "expm1", "log", "log2", "log10", "log1p",
                 "sinh", "cosh", "tanh", "asinh", "acosh", "atanh"]:
        f = getattr(libm, name)
        f.restype = ctypes.c_double
        f.argtypes = [ctypes.c_double]
        one[name] = f
    two = {}
    for name in ["atan2", "pow", "hypot"]:
        f = getattr(libm, name)
        f.restype = ctypes.c_double
        f.argtypes = [ctypes.c_double, ctypes.c_double]
        two[name] = f
    cases = [(n, (x,)) for n in one for x in SPECIALS] + [(n, (x, y)) for n in two for x in SPECIALS for y in SPECIALS]
    text = "".join("%s %016x %016x\n" % (n, bits(a[0]), bits(a[1] if len(a) > 1 else 0.0)) for n, a in cases)
    out = subprocess.run([program], input=text, capture_output=True, text=True, check=True).stdout.split()
    problems = 0
    for (name, args), got_hex in zip(cases, out):
        got = from_bits(int(got_hex, 16))
        want = one[name](*args) if name in one else two[name](*args)
        def kind(v):
            if v != v:
                return "nan"
            if math.isinf(v) or v == 0:
                return repr(v)
            return "finite"
        same = kind(got) == kind(want)
        if same and kind(got) == "finite" and got != want:
            same = abs(got - want) <= math.ulp(want)
        if not same:
            problems += 1
            print("special: %s%r gives %r, the C library %r" % (name, args, got, want))
    print("special values: %d cases, %d differ" % (len(cases), problems))
    return problems


if __name__ == "__main__":
    main()
