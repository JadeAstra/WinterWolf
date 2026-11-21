#!/usr/bin/env python3
# Makes src/fmath_tables.inc: the constants and tables of WinterWolf's own math functions
# (src/fmath.cpp), each worked out exactly here (Python's decimal and integers) and written as
# hexadecimal doubles, a value too fine for one double as two: its double and what is left.
#
#   python3 tools/fmath/tables.py > src/fmath_tables.inc
#
# Nothing here comes from another math library: pi by Machin's formula, logarithms and powers
# by decimal's own ln and exp (correctly rounded at the precision set), arctangents by their
# series.
from decimal import Decimal, getcontext
from fractions import Fraction
from math import factorial

getcontext().prec = 480


def atan_series(x):
    """atan(x) for |x| <= 1/2 by its series, to the context's precision."""
    x = Decimal(x)
    total, power, n, x2 = Decimal(0), x, 1, x * x
    eps = Decimal(10) ** -(getcontext().prec + 5)
    while abs(power) / n > eps:
        total += power / n if n % 4 == 1 else -power / n
        power *= x2
        n += 2
    return total


def atan(x):
    """atan(x) for 0 <= x <= 1: halved twice (atan x = 2 atan(x / (1 + sqrt(1 + x^2)))) first."""
    x = Decimal(x)
    halvings = 0
    while x > Decimal("0.25"):
        x = x / (1 + (1 + x * x).sqrt())
        halvings += 1
    return atan_series(x) * (2 ** halvings)


PI = 16 * atan_series(Decimal(1) / 5) - 4 * atan_series(Decimal(1) / 239)
LN2 = Decimal(2).ln()
LN10 = Decimal(10).ln()


def hexd(v):
    return float(v).hex() if not isinstance(v, float) else v.hex()


def dd(v):
    """A value as two doubles: its nearest double and the nearest double to what is left."""
    hi = float(v)
    lo = float(Decimal(v) - Decimal(hi))
    return hi, lo


def ddtext(v):
    hi, lo = dd(v)
    return "{%s, %s}" % (hi.hex(), lo.hex())


def rounded_bits(v, scale):
    """v rounded to a multiple of 2^-scale, exactly: the leading part of a split constant."""
    return Fraction(int((Decimal(v) * (Decimal(2) ** scale)).to_integral_value()), 2 ** scale)


def frac_to_decimal(f):
    return Decimal(f.numerator) / Decimal(f.denominator)


def main():
    out = []
    w = out.append
    w("// Made by tools/fmath/tables.py: do not edit; run it again instead.")
    w("// Constants and tables of src/fmath.cpp, exact to the last bit given.")
    w("")

    # ---- pi ----
    PIO2 = PI / 2
    w("static constexpr DD kPi = %s;" % ddtext(PI))
    w("static constexpr DD kPiOver2 = %s;" % ddtext(PIO2))
    w("static constexpr DD kPiOver4 = %s;" % ddtext(PI / 4))
    # pi/2 in four parts, the first three of 33 bits at most: n times each of them exact for n < 2^20.
    p1 = rounded_bits(PIO2, 32)
    rest = PIO2 - frac_to_decimal(p1)
    p2 = rounded_bits(rest, 65)
    rest -= frac_to_decimal(p2)
    p3 = rounded_bits(rest, 98)
    rest -= frac_to_decimal(p3)
    p4 = float(rest)
    for name, part in (("kPio2Part1", p1), ("kPio2Part2", p2), ("kPio2Part3", p3)):
        assert part.numerator.bit_length() <= 33, name
        w("static constexpr double %s = %s;" % (name, float(part).hex()))
        assert Fraction(float(part)) == part
    w("static constexpr double kPio2Part4 = %s;" % p4.hex())
    w("static constexpr double kTwoOverPi = %s;" % float(2 / PI).hex())

    # 2/pi's bits after the point, 32 at a time, for reducing large arguments (Payne and Hanek).
    CHUNKS = 48
    two_over_pi = Decimal(2) / PI
    bits = int((two_over_pi * (Decimal(2) ** (32 * CHUNKS))).to_integral_value(rounding="ROUND_FLOOR"))
    chunks = [(bits >> (32 * (CHUNKS - 1 - i))) & 0xFFFFFFFF for i in range(CHUNKS)]
    w("static constexpr int kTwoOverPiChunks = %d;" % CHUNKS)
    w("static constexpr uint32_t kTwoOverPiBits[kTwoOverPiChunks] = {")
    for i in range(0, CHUNKS, 6):
        w("    " + ", ".join("0x%08X" % c for c in chunks[i:i + 6]) + ",")
    w("};")
    w("")

    # ---- ln 2, ln 10 ----
    w("static constexpr DD kLn2 = %s;" % ddtext(LN2))
    w("static constexpr DD kLn10 = %s;" % ddtext(LN10))
    w("static constexpr DD kInvLn2 = %s;" % ddtext(1 / LN2))
    w("static constexpr DD kInvLn10 = %s;" % ddtext(1 / LN10))
    # ln2/64 in three parts, the first two of 32 bits: n times each exact for |n| < 2^21.
    step = LN2 / 64
    l1 = rounded_bits(step, 38)
    rest = step - frac_to_decimal(l1)
    l2 = rounded_bits(rest, 70)
    rest -= frac_to_decimal(l2)
    for name, part in (("kLn2Over64Part1", l1), ("kLn2Over64Part2", l2)):
        assert part.numerator.bit_length() <= 32, name
        w("static constexpr double %s = %s;" % (name, float(part).hex()))
        assert Fraction(float(part)) == part
    w("static constexpr double kLn2Over64Part3 = %s;" % float(rest).hex())
    w("static constexpr double kSixtyFourOverLn2 = %s;" % float(64 / LN2).hex())
    w("")

    # ---- 2^(j/64) ----
    w("// 2^(j/64), j = 0 .. 63.")
    w("static constexpr DD kExp2Table[64] = {")
    for j in range(64):
        w("    %s," % ddtext((LN2 * j / 64).exp()))
    w("};")
    w("")

    # ---- ln(1 + j/128) ----
    w("// ln(1 + j/128), j = 0 .. 128.")
    w("static constexpr DD kLogTable[129] = {")
    for j in range(129):
        w("    %s," % ddtext((1 + Decimal(j) / 128).ln()))
    w("};")
    w("")

    # ---- 1 / (1 + j/128) ----
    w("// 1 / (1 + j/128), j = 0 .. 128.")
    w("static constexpr DD kLogInverseTable[129] = {")
    for j in range(129):
        w("    %s," % ddtext(Decimal(128) / (128 + j)))
    w("};")
    w("")

    # ---- atan(j/64) ----
    w("// atan(j/64), j = 0 .. 64.")
    w("static constexpr DD kAtanTable[65] = {")
    for j in range(65):
        w("    %s," % ddtext(atan(Decimal(j) / 64)))
    w("};")
    w("")

    # ---- 1/n! ----
    w("static constexpr DD kMinusOneSixth = %s;" % ddtext(Decimal(-1) / 6))
    w("static constexpr DD kOneTwentyFourth = %s;" % ddtext(Decimal(1) / 24))
    w("// 1/n!, n = 0 .. 24, each its nearest double.")
    w("static constexpr double kInvFactorial[25] = {")
    for n in range(25):
        w("    %s," % float(Fraction(1, factorial(n))).hex())
    w("};")

    print("\n".join(out))


if __name__ == "__main__":
    main()
