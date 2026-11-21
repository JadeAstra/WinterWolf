# Makes tests/bigint_divide.ww: quotients of BigInts, and of a BigInt and a float, with the bits of
# the float nearest each exact quotient, worked out with CPython's Fraction (A1 of
# docs/snippy-ww-checklist.html). Run in WSL from the repository:
#
#   python3 tools/bigint/divide_cases.py

import random, struct
from fractions import Fraction
def hexbits(x): return '%016x' % struct.unpack('<Q', struct.pack('<d', x))[0]
def exact(fr):
    try: return float(fr)
    except OverflowError: return float('inf') if fr > 0 else float('-inf')
rng = random.Random(2026)
big = []
for _ in range(400):
    a = rng.getrandbits(rng.randint(1, 1200)) * rng.choice([1, -1])
    b = rng.getrandbits(rng.randint(1, 1200)) or 1
    b *= rng.choice([1, -1])
    big.append((a, b))
# Ties: quotients exactly halfway between two floats, both ways round.
for k in [1, 3, 5, 7]:
    big.append(((1 << 54) + k, 2))
    big.append((((1 << 54) + k) << 300, 2 << 300))
# Subnormal and overflow edges, and a quotient that is whole and huge.
big += [(1, 1 << 1074), (1, 1 << 1075), (3, 1 << 1075), (1, (1 << 1075) - 1), (10**400 + 1, 10**399), (10**400, 1), (1 << 1023, 1), ((1 << 1024) - (1 << 970), 1), ((1 << 1024) - (1 << 969), 1), (0, -5), (0, 7)]
lines = []
for a, b in big:
    q = exact(Fraction(a, b)) if a != 0 else (-0.0 if b < 0 else 0.0)   # a zero quotient keeps its sign, as CPython's int / int does
    lines.append('  Triple("%d", "%d", "%s"),' % (a, b, hexbits(q)))
floats = []
for _ in range(150):
    a = rng.getrandbits(rng.randint(54, 1100)) * rng.choice([1, -1])
    x = rng.choice([rng.uniform(-1e6, 1e6), rng.uniform(1e-300, 1e-290), rng.uniform(1e290, 1e300), 3.0, 0.1])
    floats.append('  Triple("%d", "%s", "%s"),' % (a, hexbits(x), hexbits(exact(Fraction(a) / Fraction(x)))))
    floats.append('  Triple("%d", "%s", "%s"),' % (a, hexbits(x), hexbits(exact(Fraction(x) / Fraction(a)))) + ' // x / n')
out = open('tests/bigint_divide.ww', 'w', newline='\n')
out.write('''// BigInt divided to a float, rounded once (A1 of docs/snippy-ww-checklist.html): divideToFloat,
// and / of a BigInt and a float either way, each the float nearest the exact quotient, its bits
// the same as CPython's Fraction gives (exact rational arithmetic, rounded once; past the largest
// float an infinity, where Fraction raises; a zero quotient with its sign, as CPython's int / int
// gives it). Made by tools/bigint/divide_cases.py from random
// numbers of 1 to 1200 bits, with ties, subnormal quotients and the edge of overflow.

fun bitsOf(x : float) : string => Bytes(8).putFloat64(0, x, true).toHex()

fun floatOf(hex : string) : float => hex.hexToBytes().float64(0, true)

val quotients = [
''' + '\n'.join(lines) + '''
]
var wrong = 0
for (t in quotients) {
  val got = bitsOf(t.first.toBigInt().divideToFloat(t.second.toBigInt()))
  if (got != t.third) {
    wrong += 1
    print("wrong:", t.first, "/", t.second, "gave", got, "not", t.third)
  }
}
print(quotients.size, "quotients,", wrong, "wrong")   // expect: ''' + str(len(lines)) + ''' quotients, 0 wrong

val mixed = [
''' + '\n'.join(floats) + '''
]
wrong = 0
var i = 0
for (t in mixed) {
  val n = t.first.toBigInt()
  val x = floatOf(t.second)
  val got = bitsOf(i % 2 == 0 ? n / x : x / n)
  if (got != t.third) {
    wrong += 1
    print("wrong:", i % 2 == 0 ? "n / x" : "x / n", t.first, t.second, "gave", got, "not", t.third)
  }
  i += 1
}
print(mixed.size, "with floats,", wrong, "wrong")   // expect: ''' + str(len(floats)) + ''' with floats, 0 wrong

// Readable cases: S1's, and what rounding twice gave before.
print("896756281784094569167941".toBigInt().divideToFloat("826325471441701108669".toBigInt()))   // expect: 1085.2337399445182
print((BigInt.ten.pow(400) + 1).divideToFloat(BigInt.ten.pow(399)), BigInt.ten.pow(400).divideToFloat(BigInt.of(3)))   // expect: 10.0 inf
print(BigInt.of(0).divideToFloat(BigInt.of(-5)), BigInt.of(7).divideToFloat(BigInt.of(2)), BigInt.of(-1).divideToFloat(BigInt.of(3)))   // expect: -0.0 3.5 -0.3333333333333333
try {
  BigInt.of(1).divideToFloat(BigInt.zero)
} catch (e : ArithmeticException) {
  print(e.message)   // expect: division by zero
}
''')
out.close()
print(len(lines), len(floats))
