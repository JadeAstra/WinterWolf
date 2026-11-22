# Expectations for tests/random_same_everywhere.ww (mt19937_64 written out, checked against
# std::mt19937_64's 10000th number) and tests/format_exact.ww (CPython's %, C's rounding).
M = (1 << 64) - 1


class MT64:
    def __init__(self, seed):
        self.mt = [0] * 312
        self.mt[0] = seed & M
        for i in range(1, 312):
            self.mt[i] = (6364136223846793005 * (self.mt[i - 1] ^ (self.mt[i - 1] >> 62)) + i) & M
        self.i = 312

    def next(self):
        if self.i >= 312:
            for k in range(312):
                x = (self.mt[k] & 0xFFFFFFFF80000000) | (self.mt[(k + 1) % 312] & 0x7FFFFFFF)
                xa = x >> 1
                if x & 1:
                    xa ^= 0xB5026F5AA96619E9
                self.mt[k] = self.mt[(k + 156) % 312] ^ xa
            self.i = 0
        y = self.mt[self.i]
        self.i += 1
        y ^= (y >> 29) & 0x5555555555555555
        y ^= (y << 17) & 0x71D67FFFEDA60000
        y ^= (y << 37) & 0xFFF7EEE000000000
        y ^= y >> 43
        return y & M


# std::mt19937_64 default-seeded: 10000th output is 9981545732273789042
g = MT64(5489)
for _ in range(9999):
    g.next()
assert g.next() == 9981545732273789042


def between(g, lo, hi):
    n = (hi - lo + 1) & M
    if n == 0:
        v = g.next()
        return v - (1 << 64) if v >> 63 else v
    skip = ((1 << 64) - n) % n
    while True:
        x = g.next()
        if x >= skip:
            return lo + x % n


g = MT64(42)
print([repr((g.next() >> 11) * 2.0 ** -53) for _ in range(3)])
print([between(g, 1, 6) for _ in range(10)])
print([between(g, -1000000000000, 1000000000000) for _ in range(3)])
print(between(g, -(1 << 63), (1 << 63) - 1))
g = MT64((-7) & M)
print(repr((g.next() >> 11) * 2.0 ** -53))

for f, v in [("%.1f", 0.35), ("%.0f", 2.5), ("%.0f", 3.5), ("%.2f", 1.005), ("%.4e", 12345.0),
             ("%.2e", 999.5), ("%.1f", 0.25), ("%.2f", 1234567.125), ("%.20f", 0.1), ("%.3e", 1.0005),
             ("%.0e", 25.0), ("%.15f", 1e-10), ("%.2f", 99.995), ("%.1f", -0.05)]:
    print(f, repr(v), f % v)
for f, v in [("%.4g", 12345.0), ("%.3g", 999.5), ("%g", 0.0001), ("%g", 0.00009999995), ("%.2g", 0.000125),
             ("%.1g", 0.5), ("%g", 123456.5), ("%g", 1234565.0)]:
    print(f, repr(v), f % v, ("%#" + f[1:]) % v)
