# int, bool, float and complex: their methods and attributes, and the builtins on numbers
# (the Snippy plan, part 5.3), as CPython 3.14 gives them.

# int.
for n in [0, 7, -7, 255, 2**70, -(2**70)]:
    print(n, n.bit_length(), n.bit_count(), n.real, n.imag, n.numerator, n.denominator, n.conjugate(), n.as_integer_ratio(), n.is_integer())
print((1024).to_bytes(2, "big"), (1024).to_bytes(2, "little"), (-1).to_bytes(2, "big", signed=True), (255).to_bytes(1), (0).to_bytes(0, "big"))
print(int.from_bytes(b"\x04\x00", "big"), int.from_bytes(b"\x04\x00", "little"), int.from_bytes(b"\xff\xff", "big", signed=True),
      int.from_bytes([1, 2]), int.from_bytes(b""))
for f in [lambda: (256).to_bytes(1, "big"), lambda: (-1).to_bytes(1, "big"), lambda: (1).to_bytes(1, "middle")]:
    try:
        f()
    except Exception as e:
        print(type(e).__name__, e)
print(int("42"), int(" -42 "), int("1_000"), int("ff", 16), int("0xff", 0), int("0o17", 0), int("0b101", 0), int("z", 36), int(3.9),
      int(-3.9), int(True), int("١٢"), int(b"12"), int(bytearray(b"7")))
for f in [lambda: int("4.5"), lambda: int("0x", 16), lambda: int("12", 1), lambda: int(float("inf")), lambda: int(float("nan")),
          lambda: int(None), lambda: int("")]:
    try:
        f()
    except Exception as e:
        print(type(e).__name__, e)
print(divmod(7, 2), divmod(-7, 2), divmod(7, -2), divmod(7.5, 2), pow(2, 10), pow(2, -1), pow(3, 4, 5), pow(3, -1, 7), pow(-2, 3, 5))
print(abs(-5), abs(-2**70), round(2.5), round(3.5), round(-2.5), round(1234, -2), round(1250, -2), round(15, -1), round(25, -1))
print(7 // 2, -7 // 2, 7 % -2, -7 % 2, 2 ** 0.5, 10 / 4, 1 << 70, -1 >> 3, ~5, 6 & 3, 6 | 3, 6 ^ 3, 7 .__index__())
print(int.__name__, bool.__mro__ if False else bool.__bases__, isinstance(True, int), True + True, True * 3, -True)
print(hex(255), oct(8), bin(5), hex(-255), bin(2**70), format(255, "x"), format(255, "#010b"), format(-5, "+05d"), format(1234567, ","))

# bool.
print(bool(), bool(0), bool(""), bool([]), bool([0]), bool(0.0), bool(-1), True.real, False.imag, True.numerator, True.as_integer_ratio())
print(True & False, True | False, True ^ True, str(True), repr(False), int(True), float(False), True == 1, hash(True), True is not False)

# float.
for x in [0.0, -0.0, 1.5, -2.25, 1e300, 1e-300, 0.1, 3.0]:
    print(repr(x), x.is_integer(), x.as_integer_ratio(), x.hex(), x.real, x.imag, x.conjugate())
print(float.fromhex("0x1.8p1"), float.fromhex("-0x1p-1074"), float.fromhex("inf"), float("1e5"), float(" -1.5 "), float("nan"),
      float("-inf"), float("1_000.5"), float(7), float(True), float(2**70))
for f in [lambda: (float("inf")).as_integer_ratio(), lambda: float("nan").as_integer_ratio(), lambda: float("x"), lambda: float(""),
          lambda: float.fromhex("zz"), lambda: 1.0 / 0, lambda: 1 // 0.0, lambda: 2.0 ** 10000]:
    try:
        f()
    except Exception as e:
        print(type(e).__name__, e)
print(round(2.675, 2), round(0.5), round(1.5), round(-0.5), round(2.5), round(3.14159, 3), round(1e20), round(123.456, -1), round(5.0, 0))
print(1.0 // 3, -1.0 // 3, 7.5 % 2, -7.5 % 2, 2.0 ** -1, abs(-1.5), 1e16, 1e15, 1.23e-5, 123456789.0, 0.1 + 0.2, 1 / 3)
print(float.__name__, float.from_number(5), float.from_number(2.5), format(1.5, ".3e"), format(0.000123, "g"), format(1e100, ".2g"),
      format(2.5, "%"), format(-0.0, "+"), format(1234.5678, ",.2f"), "%.3g|%10.4e|%-8.2f|" % (0.0001234, 12345.678, 3.14159))
print(int(1e20), 1e20 == 10**20, 0.5 == 1/2, hash(1.0) == hash(1), hash(0.5), hash(-1.0), hash(float("inf")), 1e308 * 10, -1e308 * 10)

# complex.
for z in [complex(1, 2), complex(-1.5, 0), 3j, complex("1+2j"), complex("-j"), complex(2), complex(0, -0.0)]:
    print(z, repr(z), z.real, z.imag, z.conjugate(), abs(z))
print(complex(1, 2) * complex(3, 4), (1 + 2j) / (3 - 4j), (1 + 1j) ** 2, 2 ** 1j == pow(2, 1j), (1j) ** 0, complex(1, 2) == complex(1, 2))
print(complex.__name__, complex.from_number(5), format(1 + 2j, ".2f"), format(3j, ""), hash(1 + 0j) == hash(1), -(1 + 1j), +(1 - 1j))
for f in [lambda: complex("x"), lambda: (1 + 1j) < (2 + 2j), lambda: 1j // 1, lambda: int(1j), lambda: complex(1, 2) % 2]:
    try:
        f()
    except Exception as e:
        print(type(e).__name__, e)

# min, max, sum over numbers.
print(sum([1, 2, 3]), sum([0.1] * 10), sum([1, 2], 10), sum(range(101)), sum([[1], [2]], []), sum([1.5, 2]), sum([], 0.0))
print(min(3, 1, 2), max([4, 9, 2]), min([], default=0), max("abc", key=ord), min([(2, "b"), (1, "z")]), max(1, 2.0), min(-0.0, 0.0))
for f in [lambda: min([]), lambda: max(), lambda: sum(["a", "b"], ""), lambda: sum([1], start="x")]:
    try:
        f()
    except Exception as e:
        print(type(e).__name__, e)
