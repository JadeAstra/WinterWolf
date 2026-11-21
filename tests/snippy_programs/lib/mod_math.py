# math and cmath (the Snippy plan, part 5.3, step 2), as CPython 3.14 gives them.
import math
import cmath

print(math.pi, math.e, math.tau, math.inf, -math.inf, math.nan, type(math.inf).__name__)
print(math.sqrt(2), math.sqrt(16), math.isqrt(17), math.isqrt(10**30), math.cbrt(27), math.cbrt(-8), math.exp(1), math.exp2(10), math.expm1(1e-10))
print(math.log(100, 10), math.log(8, 2), math.log(math.e), math.log10(1000), math.log2(1024), math.log1p(1e-10), math.log(2**1000), math.log(10**400, 10))
print(math.pow(2, 0.5), math.pow(-8, 1/3) if False else math.pow(4, 0.5), math.fabs(-3), math.fmod(7, 3), math.fmod(-7, 3), math.remainder(7, 3), math.remainder(5, 2))
print(math.floor(2.7), math.floor(-2.7), math.ceil(2.1), math.ceil(-2.1), math.trunc(-2.7), math.floor(5), type(math.floor(2.5)).__name__)
print(math.sin(math.pi / 6), math.cos(0), math.tan(math.pi / 4), math.asin(1), math.acos(0), math.atan(1), math.atan2(1, -1), math.atan2(-0.0, -1))
print(math.sinh(1), math.cosh(1), math.tanh(0.5), math.asinh(1), math.acosh(3), math.atanh(0.5), math.degrees(math.pi), math.radians(180))
print(math.hypot(3, 4), math.hypot(1, 2, 2), math.hypot(), math.dist((0, 0), (3, 4)), math.dist([1, 1, 1], [2, 2, 2]))
print(math.factorial(10), math.factorial(25), math.comb(10, 3), math.comb(100, 50), math.perm(5, 2), math.perm(5), math.comb(3, 5))
print(math.gcd(12, 18), math.gcd(0, 0), math.gcd(12, 18, 27), math.gcd(), math.lcm(4, 6), math.lcm(3, 5, 7), math.lcm(), math.gcd(-12, 18))
print(math.prod([1, 2, 3, 4]), math.prod([]), math.prod([2, 3], start=10), math.prod([1.5, 2]), math.fsum([0.1] * 10), sum([0.1] * 10), math.fsum([1e100, 1, -1e100]))
print(math.sumprod([1, 2, 3], [4, 5, 6]), math.sumprod([0.1] * 3, [0.2] * 3))
print(math.isfinite(1.0), math.isfinite(math.inf), math.isinf(-math.inf), math.isnan(math.nan), math.isclose(0.1 + 0.2, 0.3), math.isclose(1, 1.1, rel_tol=0.2),
      math.isclose(0, 1e-10, abs_tol=1e-9), math.copysign(3, -0.0), math.frexp(8), math.frexp(0.0), math.ldexp(0.5, 4), math.modf(3.75), math.modf(-2.5))
print(math.erf(1), math.erfc(1), math.gamma(5), math.gamma(0.5), math.lgamma(10), math.nextafter(1, 2), math.nextafter(1, 0, steps=2), math.ulp(1.0), math.ulp(0.0))
print(math.fma(2, 3, 4), math.floor(1e20), math.ceil(-1e20), math.trunc(1e300) > 0, math.isqrt(0), math.factorial(0))
for f in [lambda: math.sqrt(-1), lambda: math.log(0), lambda: math.log(-1), lambda: math.factorial(-1), lambda: math.factorial(2.5), lambda: math.acos(2),
          lambda: math.isqrt(-1), lambda: math.comb(-1, 2), lambda: math.exp(1000), lambda: math.fmod(1, 0), lambda: math.pow(0, -1), lambda: math.floor(math.inf),
          lambda: math.floor(math.nan), lambda: math.gcd(1.5), lambda: math.sqrt("x"), lambda: math.dist((1,), (1, 2)), lambda: math.ldexp(1, 10000)]:
    try:
        print(f())
    except Exception as e:
        print(type(e).__name__, e)
class Ceil:
    def __ceil__(self):
        return 42
    def __floor__(self):
        return -42
    def __trunc__(self):
        return 7
print(math.ceil(Ceil()), math.floor(Ceil()), math.trunc(Ceil()), math.floor(True), math.sqrt(True))

# cmath.
z = complex(3, 4)
print(cmath.phase(z), cmath.polar(z), cmath.rect(2, math.pi / 2), abs(z), cmath.sqrt(-1), cmath.sqrt(z), cmath.exp(1j * math.pi))
print(cmath.log(z), cmath.log10(100), cmath.log(8, 2), cmath.sin(1j), cmath.cos(1 + 1j), cmath.tan(0.5j), cmath.asin(2), cmath.acos(2), cmath.atan(1j / 2))
print(cmath.sinh(1j), cmath.cosh(1), cmath.tanh(1 + 1j), cmath.asinh(1j), cmath.acosh(1j), cmath.atanh(0.5), cmath.pi, cmath.e, cmath.tau, cmath.inf, cmath.infj, cmath.nan, cmath.nanj)
print(cmath.isfinite(z), cmath.isinf(complex(math.inf, 0)), cmath.isnan(complex(0, math.nan)), cmath.isclose(1 + 1j, 1.0000000001 + 1j), cmath.phase(-1), cmath.phase(complex(-1, -0.0)))
for f in [lambda: cmath.log(0), lambda: cmath.sqrt("x"), lambda: cmath.atanh(1)]:
    try:
        print(f())
    except Exception as e:
        print(type(e).__name__, e)
print(sorted(n for n in dir(math) if not n.startswith("_")))
print(sorted(n for n in dir(cmath) if not n.startswith("_")))
