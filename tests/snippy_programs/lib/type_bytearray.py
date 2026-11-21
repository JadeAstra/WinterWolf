# bytearray: bytes' methods, giving bytearrays, and its own changes (the Snippy plan, part 5.3).

a = bytearray(b"Hello world")
print(a, len(a), a[0], a[-1], a[1:4], type(a[1:4]).__name__, a.upper(), type(a.upper()).__name__)
print(a.find(b"o"), a.count(b"l"), a.split(), type(a.split()[0]).__name__, a.partition(b" "), a.replace(b"l", b"L"))
print(a.center(15, b"*"), a.strip(b"Hd"), a.title(), a.hex(), a.decode(), a.isalpha(), a.startswith(b"He"))
print(bytearray(b"-").join([b"a", bytearray(b"b")]), type(bytearray(b"-").join([b"a"])).__name__)
print(bytearray.fromhex("41 42"), type(bytearray.fromhex("41")).__name__, bytearray.maketrans(b"a", b"b")[97])

# Changes.
a.append(33)
a.extend(b" ok")
a.extend([65, 66])
a.insert(0, 62)
a.insert(-1, 95)
print(a)
print(a.pop(), a.pop(0), a)
a.remove(111)
print(a)
a.reverse()
print(a)
a.reverse()
c = a.copy()
c[0] = 104
print(a, c, a is c)
a[0:5] = b"HELLO"
print(a)
a[0:5] = b"hi"
print(a)
a[::2] = b"_" * len(a[::2])
print(a)
del a[0]
del a[0:3]
del a[::2]
print(a)
a += b"!!"
print(a)
a *= 2
print(a)
a.clear()
print(a, len(a))
r = bytearray(b"abc")
r.resize(5)
print(r)
r.resize(2)
print(r)

# Errors, as CPython says them.
for bad in [lambda: bytearray(b"x").append(256), lambda: bytearray(b"x").pop(5), lambda: bytearray().pop(),
            lambda: bytearray(b"x").remove(1), lambda: bytearray(b"x").__setitem__(0, 300),
            lambda: bytearray(b"x")[3], lambda: hash(bytearray(b"x")), lambda: bytearray(b"abc").__setitem__(slice(0, 3, 2), b"xyz")]:
    try:
        bad()
    except Exception as e:
        print(type(e).__name__, e)

# Constructors, comparisons, the rest.
print(bytearray(), bytearray(3), bytearray([1, 2]), bytearray("hé", "utf-8"), bytearray(b"ab") == b"ab", b"ab" == bytearray(b"ab"))
print(bytearray(b"ab") < bytearray(b"b"), bytearray(b"ab") + b"cd", b"cd" + bytearray(b"ab"), type(b"cd" + bytearray(b"ab")).__name__)
print(repr(bytearray(b"a'b")), list(bytearray(b"hi")), 104 in bytearray(b"hi"), bytearray(b"xy") * 2)
print(b"%s and %s" % (bytearray(b"ba"), b"by"), bytearray(b"%d") % 5)
m = bytearray(b"abc")
print(isinstance(m, bytearray), bytearray.__name__, bytes(m), m.isascii(), m.zfill(5), m.expandtabs())
