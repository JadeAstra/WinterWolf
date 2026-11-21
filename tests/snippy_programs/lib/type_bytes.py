# bytes: every method, as CPython 3.14 has them (the Snippy plan, part 5.3).

b = b"Hello, World! hello"
print(b.count(b"l"), b.count(b"l", 5), b.count(b"l", 5, 12), b.count(108), b.count(b""))
print(b.find(b"o"), b.find(b"o", 5), b.find(b"zz"), b.find(111), b.rfind(b"o"), b.rfind(b"l", 0, 5))
print(b.index(b"W"), b.rindex(b"l"))
try:
    b.index(b"zz")
except ValueError as e:
    print("ValueError", e)
try:
    b.rindex(b"zz")
except ValueError as e:
    print("ValueError", e)
print(b.startswith(b"Hell"), b.startswith((b"x", b"He")), b.endswith(b"llo"), b.endswith(b"World", 0, 12))

# Case and classes: ASCII only.
x = b"hello WORLD \xe9t\xc9 42"
print(x.upper(), x.lower(), x.swapcase(), x.capitalize(), x.title())
for s in [b"abc", b"ABC", b"Abc Def", b"abc1", b"123", b" \t\n", b"", b"\xe9", b"ab cd", b"Ab Cd", b"AB CD", b"aB"]:
    print(s, s.isalnum(), s.isalpha(), s.isascii(), s.isdigit(), s.islower(), s.isupper(), s.isspace(), s.istitle())
print(b"they're bill's friends".title(), b"hello\xe9world".capitalize())

# Padding.
print(b"ab".center(7), b"ab".center(7, b"*"), b"ab".ljust(5, b"-"), b"ab".rjust(5, b"-"), b"ab".center(1))
print(b"42".zfill(5), b"-42".zfill(5), b"+7".zfill(4), b"abc".zfill(2))
print(b"a\tbc\td".expandtabs(), b"a\tbc\td".expandtabs(4), b"x\ty\nab\tc".expandtabs(3))
try:
    b"ab".center(5, b"**")
except TypeError as e:
    print("TypeError", e)

# Stripping and splitting: ASCII whitespace alone.
w = b" \t\x0b\x0c spaced \xa0\x85 \n"
print(w.strip(), w.lstrip(), w.rstrip(), b"xxhixx".strip(b"x"), b"abcba".strip(b"ab"))
print(b"a b  c".split(), b"  a b c  ".split(None, 1), b"a,b,,c".split(b","), b"a,b,c".split(b",", 1))
print(b"a b c".rsplit(), b"a,b,c".rsplit(b",", 1), b"  a  b  ".rsplit(None, 1))
print(b"\xa0a\x85b".split(), b"x\x1cy".split())
print(b"one\ntwo\r\nthree\rfour\x0bfive\x0c".splitlines(), b"a\nb\n".splitlines(True), b"".splitlines())
print(b"a-b-c".partition(b"-"), b"a-b-c".rpartition(b"-"), b"abc".partition(b"x"), b"abc".rpartition(b"x"))
print(b"prefix-body".removeprefix(b"prefix-"), b"body.txt".removesuffix(b".txt"), b"x".removeprefix(b"y"))
print(b"aaa".replace(b"a", b"b"), b"aaa".replace(b"a", b"b", 2), b"abc".replace(b"", b"-"), b"abc".replace(b"b", b""))
print(b", ".join([b"a", b"b", bytearray(b"c"), memoryview(b"d")]), b"".join([]))
try:
    b"-".join([b"a", "b"])
except TypeError as e:
    print("TypeError", e)

# Translation.
t = bytes.maketrans(b"abc", b"xyz")
print(len(t), t[97:100], b"aabbcc".translate(t), b"hello".translate(None, b"l"), b"abc".translate(t, b"b"))
try:
    bytes.maketrans(b"ab", b"x")
except ValueError as e:
    print("ValueError", e)

# hex and fromhex.
print(b"\x00\xff\x10".hex(), b"\x01\x02\x03\x04".hex(":"), b"\x01\x02\x03\x04".hex("-", 2), b"\x01\x02\x03\x04\x05".hex(" ", -2))
print(bytes.fromhex("de ad be ef"), bytes.fromhex(""), bytes.fromhex("0A0b"))
try:
    bytes.fromhex("abc")
except ValueError as e:
    print("ValueError", e)
try:
    bytes.fromhex("zz")
except ValueError as e:
    print("ValueError", e)

# decode.
print(b"caf\xc3\xa9".decode(), b"caf\xe9".decode("latin-1"), b"abc".decode("ascii"), b"caf\xff".decode("utf-8", "replace"),
      b"caf\xff".decode("utf-8", "ignore"), b"caf\xff".decode("utf-8", errors="backslashreplace"))
try:
    b"caf\xff".decode()
except UnicodeDecodeError as e:
    print("UnicodeDecodeError", e)

# Constructors and the rest.
print(bytes(), bytes(3), bytes([65, 66, 67]), bytes("abc", "ascii"), bytes(range(5)), bytes(bytearray(b"xy")), bytes(memoryview(b"mv")))
try:
    bytes([256])
except ValueError as e:
    print("ValueError", e)
try:
    bytes("abc")
except TypeError as e:
    print("TypeError", e)
print(b[0], b[-1], b[1:5], b[::-1], b[::3], len(b), b"ab" * 3, 2 * b"x", b"a" + b"b", b"ab" + bytearray(b"c"))
print(b"abc" < b"abd", b"abc" == b"abc", b"a" in b"cat", 97 in b"cat", hash(b"abc") == hash(b"abc"), list(b"hi"))
print(repr(b"a'b"), repr(b'a"b'), repr(b"a'\"b"), repr(b"\t\n\r\x00\x7f\\"), str(b"x"))
print(b"%d items cost %.2f: %s %r %x %c %c" % (3, 1.5, b"ok", b"r", 255, 65, b"Z"))
print(bytes.__name__, type(b"").__name__, isinstance(b"", bytes))
