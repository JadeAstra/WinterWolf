# memoryview over bytes and bytearray (the Snippy plan, part 5.3): views that share their
# memory, slices, casts between byte formats, and what they say of themselves.

data = bytearray(b"abcdefgh")
m = memoryview(data)
print(m.nbytes, m.itemsize, m.format, m.ndim, m.shape, m.strides, m.readonly, m.obj is data, len(m))
print(m[0], m[-1], bytes(m[2:5]), m[2:5].tobytes(), m.tolist()[:3], m.hex(), m[1:3].hex(":"))
m[0] = 65
m[1:3] = b"BC"
print(data, m.tobytes())
s = m[2:6]
s[0] = 122
print(data, s.nbytes, s.obj is data, s.c_contiguous, s.contiguous, s.f_contiguous)
r = memoryview(b"read only")
print(r.readonly, bytes(r[:4]), r.toreadonly().readonly, r.count(ord("o")), r.index(ord("y")))
try:
    r[0] = 1
except TypeError as e:
    print("TypeError", e)
c = m.cast("B")
print(c.format, c.tolist())
words = memoryview(bytearray(b"\x01\x00\x02\x00\x03\x00\x04\x00")).cast("H")
print(words.format, words.itemsize, words.tolist(), words[2], len(words), words.nbytes)
ints = memoryview(bytes(range(8))).cast("i")
print(ints.tolist(), ints.shape)
print(memoryview(b"ab") == b"ab", memoryview(b"ab") == memoryview(b"ab"), b"ab" == memoryview(b"ab"), memoryview(b"ab") == b"ac")
with memoryview(b"xyz") as v:
    print(bytes(v), v.tolist())
try:
    v.tolist()
except ValueError as e:
    print("ValueError", e)
v2 = memoryview(b"q")
v2.release()
print(repr(v2).startswith("<released memory"), repr(memoryview(b"")).startswith("<memory at"))
print(list(memoryview(b"hey")), [x for x in memoryview(b"ok")], bytearray(memoryview(b"copy")), b"-".join([memoryview(b"a"), b"b"]))
print(memoryview(b"abc")[::-1].tobytes(), memoryview(b"abcdef")[::2].tolist(), type(memoryview(b"a")).__name__)
