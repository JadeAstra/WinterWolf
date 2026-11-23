# cmath as CPython 3.14 gives it: every function over awkward values (zeros of both signs,
# infinities, NaN, values past exp's reach and past DBL_MAX / 4), its errors too; log with a base,
# polar, rect, isclose. (W6 of docs/snippy-ww-checklist.html: Snippy's cmath is ww's Complex.)
import cmath
import math

inf, nan = math.inf, math.nan
vals = [0.0, -0.0, 0.5, -2.0, 1.0, 800.0, 1e300, inf, -inf, nan]
names = ["sqrt", "exp", "log", "log10", "sin", "cos", "tan", "sinh", "cosh", "tanh",
         "asin", "acos", "atan", "asinh", "acosh", "atanh"]


def tried(f, *args):
    try:
        return repr(f(*args))
    except (ValueError, OverflowError, ZeroDivisionError) as e:
        return "%s(%s)" % (type(e).__name__, e)


for name in names:
    f = getattr(cmath, name)
    print(name)
    for a in vals:
        print("  ", " ".join(tried(f, complex(a, b)) for b in vals))

print([tried(cmath.log, x, b) for x, b in [(0, 2), (2, 0), (2, 1), (0, 1), (8, 2), (1j, 1j), (-1, 10), (inf, 2), (nan, 2)]])
print([tried(cmath.polar, z) for z in [1e308 + 1e308j, complex(inf, nan), complex(nan, 1), 3 + 4j, -0.0j]])
print([tried(cmath.rect, r, t) for r, t in [(2, inf), (0, inf), (nan, inf), (inf, 1), (-inf, -0.0), (1, nan), (2, 0.5)]])
print(cmath.isclose(1e300 + 1e300j, 1e300 + 1.0000000001e300j), cmath.isclose(complex(inf, 1), complex(inf, 1)), cmath.isclose(1 + 1j, 1 + 1.1j, abs_tol=0.2))
print([abs(z) for z in [3 + 4j, complex(inf, nan), complex(nan, 2), 1e-310 + 1e-310j, 0.3 + 0.5j]])
print(tried(abs, 1e308 + 1e308j), tried(abs, 1.7e308 + 1.7e308j), (0.3 + 0.5j) ** 1.5)
