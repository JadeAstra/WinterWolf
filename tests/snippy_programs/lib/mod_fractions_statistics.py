# fractions and statistics (the Snippy plan, part 5.3, step 3), as CPython 3.14 gives them.
from fractions import Fraction
import statistics
import math

F = Fraction
print(F(3, 4), F(6, -8), F(5), F("3/7"), F(" -1.25 "), F("2e-3"), F(0.75), F(1.1), F(2.5).limit_denominator(), F(math.pi).limit_denominator(1000))
print(repr(F(1, 3)), F(1, 3).numerator, F(1, 3).denominator, F(-3, 9), F(7, 1), F("1_000/3"), F(F(1, 2), F(3, 4)), F(4, 6).as_integer_ratio())
a, b = F(1, 3), F(1, 6)
print(a + b, a - b, a * b, a / b, a // b, a % b, -a, +a, abs(F(-1, 2)), a ** 2, a ** -2, F(4, 9) ** F(1, 2), 2 ** F(1, 2), divmod(F(7, 2), F(1, 3)))
print(a + 1, 1 + a, a + 0.5, 0.5 + a, a * 3, a == F(2, 6), a < b, a > 0.3, a == 1 / 3, F(1, 2) == 0.5, F(3) == 3, hash(F(1, 2)) == hash(0.5), hash(F(3)) == hash(3))
print(F(7, 2).__floor__(), math.floor(F(-7, 2)), math.ceil(F(7, 2)), math.trunc(F(-7, 2)), round(F(5, 2)), round(F(7, 2)), round(F(22, 7), 2), round(F(-1, 2)))
print(F(10, 4).is_integer(), F(8, 4).is_integer(), float(F(1, 3)), int(F(7, 2)), complex(F(1, 2)), bool(F(0)), F.from_float(0.1), F.from_decimal if False else 0)
print(format(F(1, 3), ".5f"), format(F(1, 3), ".3e"), format(F(2, 3), "%"), format(F(1, 3), ""), f"{F(22, 7):>10}", format(F(-5, 2), "+.2f"), format(F(1, 8), "g"))
print(F(1, 3) + 1j, sum([F(1, 2), F(1, 3), F(1, 6)]), max(F(1, 2), F(2, 3)), sorted([F(1, 2), F(1, 3), 0.4]), F(3, 4).limit_denominator(1))
for f in [lambda: F(1, 0), lambda: F("x"), lambda: F(1.5, 2), lambda: F(math.inf), lambda: F(math.nan), lambda: F(1, 2) ** 0.5 if False else F(1) / 0]:
    try:
        print(f())
    except Exception as e:
        print(type(e).__name__, e)

# statistics.
data = [2.75, 1.75, 1.25, 0.25, 0.5, 1.25, 3.5]
ints = [1, 3, 4, 4, 7, 9, 2, 4]
print(statistics.mean(data), statistics.mean(ints), statistics.mean([F(1, 2), F(1, 3)]), statistics.fmean(ints), statistics.fmean([1, 2, 3], weights=[3, 2, 1]))
print(statistics.median(ints), statistics.median([1, 3, 5]), statistics.median_low(ints), statistics.median_high(ints), statistics.median_grouped([52, 52, 53, 54]))
print(statistics.mode(ints), statistics.mode("aabbbc"), statistics.multimode("aabbcd"), statistics.multimode([]), statistics.geometric_mean([54, 24, 36]))
print(statistics.harmonic_mean([40, 60]), statistics.harmonic_mean([40, 60], weights=[5, 30]), statistics.pvariance(ints), statistics.variance(ints))
print(statistics.pstdev(ints), statistics.stdev(ints), statistics.variance(data), statistics.stdev([F(1), F(2), F(4)]), statistics.pvariance([1.5, 2.5], mu=2))
print(statistics.quantiles(range(1, 11), n=4), statistics.quantiles([1, 2, 3, 4, 5], n=4, method="inclusive"), statistics.quantiles(ints, n=10))
x = [1, 2, 3, 4, 5]
y = [2, 4, 5, 4, 5]
print(statistics.covariance(x, y), statistics.correlation(x, y), statistics.correlation(x, y, method="ranked"), statistics.linear_regression(x, y),
      statistics.linear_regression(x, y, proportional=True))
nd = statistics.NormalDist(100, 15)
print(nd, nd.mean, nd.stdev, nd.variance, nd.cdf(130), nd.pdf(100), nd.inv_cdf(0.95), nd.overlap(statistics.NormalDist(110, 10)), nd.zscore(115),
      nd.quantiles(4), statistics.NormalDist.from_samples([1, 2, 3, 4]), nd + 5, nd * 2, nd == statistics.NormalDist(100, 15))
for f in [lambda: statistics.mean([]), lambda: statistics.variance([1]), lambda: statistics.stdev([]), lambda: statistics.median([]),
          lambda: statistics.harmonic_mean([1, -1]), lambda: statistics.geometric_mean([0, 1]), lambda: statistics.correlation([1, 1], [1, 2]),
          lambda: statistics.quantiles([1], n=4), lambda: statistics.NormalDist(0, -1)]:
    try:
        print(f())
    except Exception as e:
        print(type(e).__name__, e)
print(issubclass(statistics.StatisticsError, ValueError))
