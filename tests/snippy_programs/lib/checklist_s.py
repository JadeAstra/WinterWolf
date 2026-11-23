# Checklist S1, S2, S3, S5, S6 and S7 (docs/snippy-ww-checklist.html): places where Snippy's own
# working gave other answers than CPython's.
import math
import random
import statistics
import time

# S1: int / int rounded once, as long_true_divide; no overflow before the division.
print(896756281784094569167941 / 826325471441701108669)
print((10**400 + 1) / 10**399, -1 / 10**400, 10**30 / 3, (2**1100 + 1) / 2**1000)
try:
    print(10**400 / 3)
except OverflowError as e:
    print("OverflowError:", e)

# S2: mktime reads the year, month and day (each carried past its range), not tm_yday.
def day(t):
    return time.localtime(time.mktime(t))[:6]

print(day((2024, 3, 1, 0, 0, 0, 0, 0, -1)), day((2024, 3, 1, 0, 0, 0, 3, 61, -1)))
print(day((2024, 13, 1, 12, 0, 0, 0, 0, -1)), day((2024, 0, 1, 12, 0, 0, 0, 0, -1)), day((2024, 3, 0, 12, 0, 0, 0, 0, -1)))
print(day((2024, 1, 61, 12, 0, 0, 0, 0, -1)), day((2024, 2, 28, 12, 61, -1, 0, 0, -1)), day((2024, -1, 1, 12, 0, 0, 0, 0, -1)))
print(time.mktime((2024, 3, 1, 0, 0, 0, 0, 0, -1)) == time.mktime((2024, 2, 30, 0, 0, 0, 6, 200, -1)))
for bad in [(2024, 3, 1, 0, 0, 0, 0, 0), [2024, 3, 1, 0, 0, 0, 0, 0, -1], 5]:
    try:
        time.mktime(bad)
    except TypeError as e:
        print("TypeError:", e)

# S3: isidentifier by XID_Start and XID_Continue.
print("℘".isidentifier(), "a·".isidentifier(), "·a".isidentifier(), "℮x".isidentifier(), "_".isidentifier(), "1a".isidentifier())
print("x̀".isidentifier(), "̀x".isidentifier(), "ﬁ".isidentifier(), "a‌".isidentifier(), "Ⅻ".isidentifier())

# S7: isdecimal, isdigit and isnumeric by Numeric_Type; isalnum with them.
for c in ["7", "٣", "፩", "²", "①", "½", "五", "Ⅳ", "〇", "𝟗", "a"]:
    print(repr(c), c.isdecimal(), c.isdigit(), c.isnumeric(), c.isalnum())

# All the code points, every 7th: how many each says yes to.
counts = [0, 0, 0, 0, 0, 0]
for cp in range(0, 0x110000, 7):
    c = chr(cp)
    counts[0] += c.isdecimal()
    counts[1] += c.isdigit()
    counts[2] += c.isnumeric()
    counts[3] += c.isidentifier()
    counts[4] += ("a" + c).isidentifier()
    counts[5] += c.isalnum()
print(counts)

# S5: NormalDist.samples without a seed draws from the random module.
random.seed(1)
print(statistics.NormalDist(10, 2).samples(3))
random.seed(1)
first = random.random()
random.seed(1)
print(statistics.NormalDist().samples(1)[0] == statistics.NormalDist().inv_cdf(first))
print(statistics.NormalDist(10, 2).samples(2, seed=5))

# S6: statistics adds up with math.fsum.
for data in [[math.nan, math.inf, -math.inf], [-0.0, -0.0], [1e308, 1e308, -1e308]]:
    try:
        print(statistics.fmean(data))
    except (ValueError, OverflowError) as e:
        print(type(e).__name__ + ":", e)
print(statistics.covariance([1.5, math.inf, 3.0], [2.0, 1.0, 4.0]), statistics.fmean([0.1] * 10), statistics.correlation([1.0, 2.0, 3.0], [1.0, 2.5, 2.9]))

# math.fsum as CPython's own steps: 3000 lists with cancellation, every size of number, and sums
# near the largest float, folded into one number.
random.seed(2026)
check = 0
kinds = {"value": 0, "overflow": 0, "zero": 0}
for k in range(3000):
    values = []
    for _ in range(random.randrange(1, 12)):
        e = random.choice([random.randrange(-1074, 1024), random.randrange(-60, 60), 1023, 1022])
        v = math.ldexp(random.random() + 0.5, e) if e < 1023 else random.choice([1.7976931348623157e308, 1e308, 8.98846567431158e307])
        values.append(v if random.random() < 0.5 else -v)
        if random.random() < 0.3:
            values.append(-values[random.randrange(len(values))])
    try:
        r = math.fsum(values)
        kinds["zero" if r == 0.0 else "value"] += 1
        m, e = math.frexp(r)
        check = (check * 1000003 + int(m * 2**53) * 2048 + e + 1074) % (2**61 - 1)
    except OverflowError:
        kinds["overflow"] += 1
        check = (check * 1000003 + 7) % (2**61 - 1)
print(check, kinds, repr(math.fsum([-0.0])), repr(math.fsum([])), repr(math.fsum([0.1] * 10)))
# gc stress: leave out (every seventh code point's classes, and 3000 sums of random floats: past the limit under GC stress, and nothing more for the collector than a short program)
