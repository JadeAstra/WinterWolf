# open() and io as CPython 3.14 gives them on Linux: text and binary files, every mode, reading
# by size, by line and by iteration, seek and tell, append, exclusive creation, newline
# translation both ways, encodings with their byte order marks and error handlers, the file
# objects' types, reprs and attributes, StringIO and BytesIO, and the errors in CPython's words.
# Its files are *.tmp, which the runners (tests/snippy_lib.ww, tools/snippy/expect.sh and check.sh)
# delete before and after each program.
import io

T = "mod_io_a.tmp"
B = "mod_io_b.tmp"


def err(f, *a, **k):
    try:
        r = f(*a, **k)
        print("ok", repr(r))
    except Exception as e:
        print(type(e).__name__, e)


# ---- Text ----
with open(T, "w") as f:
    print(type(f).__name__, f.name, f.mode, f.encoding, f.errors, f.closed, f.readable(), f.writable(), f.seekable())
    print(f.write("first line\n"), f.write("second\n"), f.write("third, no newline"))
    print(f.tell())
    f.writelines(["\n", "x\n", "y\n"])
    print("printed", 1, 2.5, file=f)
print(f.closed)
with open(T) as f:
    print(repr(f.read(5)), repr(f.readline()), f.tell(), repr(f.readline()), repr(f.read()), repr(f.read()), repr(f.readline()))
with open(T, "r") as f:
    print(f.readlines())
with open(T) as f:
    print([line for line in f])
with open(T) as f:
    print(next(f), end="")
    print(f.readline(3), f.readline(0), repr(f.readline(-1)))
    err(f.tell)
    f.seek(0)
    f.readline()
    pos = f.tell()
    rest = f.read()
    f.seek(pos)
    print(f.read() == rest, f.seek(0), repr(f.read(4)), f.seek(0, 2), repr(f.read()))
with open(T, "a") as f:
    print(f.tell(), f.write("appended\n"), f.tell())
with open(T) as f:
    print(f.read().splitlines()[-2:])
with open(T, "r+") as f:
    print(repr(f.read(5)), f.seek(0), f.write("FIRST"), f.seek(0), repr(f.readline()))
with open(T, "w+") as f:
    f.write("one\ntwo\n")
    print(f.tell(), repr(f.read()), f.seek(0), repr(f.read()), f.truncate(4), f.seek(0), repr(f.read()))
with open(T, "a+") as f:
    print(f.tell(), repr(f.read()), f.seek(0), repr(f.read()))

# ---- Newlines ----
with open(T, "w", newline="") as f:
    f.write("a\nb\r\nc\rd\r")
with open(T, "rb") as f:
    print(f.read())
with open(T) as f:
    print(repr(f.read()), f.newlines)
with open(T, newline="") as f:
    print(f.readlines(), f.newlines)
with open(T, newline="\r") as f:
    print(f.readlines())
with open(T, newline="\r\n") as f:
    print(f.readlines())
with open(T, newline="\n") as f:
    print(f.readlines())
with open(T) as f:
    print(f.readlines())
with open(T, "w", newline="\r\n") as f:
    f.write("p\nq\n")
with open(T, "rb") as f:
    print(f.read())
with open(T, "w", newline="\r") as f:
    f.write("p\nq\n")
with open(T, "rb") as f:
    print(f.read())
with open(T, "w") as f:
    f.write("one\r\ntwo\n")
with open(T) as f:
    print(repr(f.readline()), f.tell(), repr(f.readline()), f.tell(), f.newlines)

# ---- Encodings ----
for enc in ["utf-8", "latin-1", "utf-16", "utf-16-le", "utf-8-sig", "utf-32", "ascii"]:
    with open(T, "w", encoding=enc) as f:
        f.write("héllo\n" if enc != "ascii" else "hello\n")
        f.write("z")
    with open(T, "rb") as f:
        raw = f.read()
    with open(T, encoding=enc) as f:
        print(enc, raw, repr(f.read()), f.encoding)
with open(T, "w", encoding="utf-8") as f:
    f.write("aé€😀b")
with open(T, encoding="utf-8") as f:
    print(repr(f.read(1)), f.tell(), repr(f.read(1)), f.tell(), repr(f.read(2)), f.tell(), repr(f.read()), f.tell())
with open(T, "wb") as f:
    f.write(b"ok\xffbad\n")
err(lambda: open(T, encoding="utf-8").read())
with open(T, encoding="utf-8", errors="replace") as f:
    print(repr(f.read()), f.errors)
with open(T, encoding="utf-8", errors="surrogateescape") as f:
    print(repr(f.read()))
with open(T, encoding="latin-1") as f:
    print(repr(f.read()))
with open(T, "w", encoding="ascii", errors="xmlcharrefreplace") as f:
    f.write("é")
with open(T, "rb") as f:
    print(f.read())
err(lambda: open(T, "w", encoding="ascii").write("é"))
err(open, T, encoding="nope")

# ---- Binary ----
with open(B, "wb") as f:
    print(type(f).__name__, f.name, f.mode, f.readable(), f.writable(), f.seekable(), f.closed)
    print(f.write(b"0123456789"), f.write(bytearray(b"\nab\n")), f.write(memoryview(b"cd")), f.tell())
with open(B, "rb") as f:
    print(type(f).__name__, f.mode, f.read(3), f.tell(), f.read1(2), f.seek(-2, 2), f.read(), f.seek(1), f.seek(2, 1), f.read(2), f.read(0))
    f.seek(0)
    print(f.readline(), f.readline(), f.readline(), f.readline(), f.tell())
    f.seek(0)
    print(list(f), f.readlines())
    f.seek(0)
    buf = bytearray(4)
    print(f.readinto(buf), buf, f.peek(1)[:1], f.read(2))
with open(B, "r+b") as f:
    print(type(f).__name__, f.mode, f.read(2), f.write(b"XY"), f.tell(), f.seek(0), f.read(5), f.truncate(), f.tell())
with open(B, "rb") as f:
    print(f.read())
with open(B, "ab") as f:
    print(f.tell(), f.write(b"++"), f.tell())
with open(B, "w+b") as f:
    print(type(f).__name__, f.write(b"abc"), f.seek(0), f.read(), f.truncate(1), f.tell(), f.seek(0, 2))
with open(B, "rb", buffering=0) as f:
    print(type(f).__name__, f.mode, f.read(), f.read(), f.closefd)
with open(B, "rb") as f:
    print(type(f.raw).__name__, f.raw.mode, f.raw.name)
with open(T) as f:
    print(type(f.buffer).__name__, f.buffer.mode, type(f.buffer.raw).__name__, f.buffer.raw.mode)
with open(B, "r+b") as f:
    print(type(f.raw).__name__, f.raw.mode)
with open(B, "ab") as f:
    print(type(f.raw).__name__, f.raw.mode)

# ---- Exclusive creation ----
X = "mod_io_x.tmp"
with open(X, "x") as f:
    f.write("new")
err(open, X, "x")
with open(X, "xb" if False else "rb") as f:
    print(f.read())

# ---- Reprs, isinstance ----
with open(T, "w", encoding="utf-8") as f:
    print(repr(f).replace(T, "T"))
    print(repr(f.buffer).replace(T, "T"), repr(f.buffer.raw).replace(T, "T"))
    print(isinstance(f, io.TextIOWrapper), isinstance(f, io.TextIOBase), isinstance(f, io.IOBase), isinstance(f, io.BufferedIOBase))
    print(isinstance(f.buffer, io.BufferedWriter), isinstance(f.buffer, io.BufferedIOBase), isinstance(f.buffer.raw, io.RawIOBase), isinstance(f.buffer.raw, io.FileIO))
with open(B, "rb") as f:
    print(repr(f).replace(B, "B"))
with open(B, "r+b") as f:
    print(repr(f).replace(B, "B"))
print(repr(f).replace(B, "B"))
print(io.SEEK_SET, io.SEEK_CUR, io.SEEK_END, io.open is open, issubclass(io.UnsupportedOperation, OSError), issubclass(io.UnsupportedOperation, ValueError))

# ---- Errors ----
err(open, "mod_io_missing.tmp")
try:
    open("mod_io_missing.tmp", "rb")
except FileNotFoundError as e:
    print(e.errno, e.strerror, e.filename, e.args, repr(e))
except OSError as e:
    print("other", e)
err(open, ".")
err(open, ".", "w")
err(open, "mod_io_no_dir/x.tmp", "w")
err(open, T, "rw")
err(open, T, "rt+b")
err(open, T, "wa")
err(open, T, "")
err(open, T, "rb", encoding="utf-8")
err(open, T, "rb", errors="strict")
err(open, T, "rb", newline="")
err(open, T, "r", buffering=0)
err(open, T, newline="x")
err(open, T, "q")
err(open, 1.5)
err(open, T, 5)
f = open(T)
f.close()
f.close()
err(f.read)
err(f.write, "x")
err(f.readline)
err(f.tell)
err(f.seek, 0)
err(lambda: next(f))
err(lambda: f.closed)
f = open(T)
err(f.write, "x")
err(f.seek, 1, 1)
err(f.seek, -1, 2)
err(f.seek, -1)
err(f.seek, 0, 3)
f.close()
f = open(T, "w")
err(f.read)
err(f.readline)
err(f.write, b"x")
err(f.write, 5)
f.close()
f = open(B, "wb")
err(f.read)
err(f.write, "x")
f.close()
f = open(B, "rb")
err(f.write, b"x")
err(f.seek, -1)
err(f.read, "1")
f.close()
with open(T, "w") as f:
    f.write("line one\nline two\n")
with open(T) as f:
    for i, line in enumerate(f):
        print(i, repr(line))

# ---- StringIO, BytesIO ----
s = io.StringIO("hello\nworld\n")
print(s.read(3), repr(s.readline()), s.tell(), s.readlines(), s.getvalue() == "hello\nworld\n")
s = io.StringIO()
print(s.write("abc"), s.write("\ndef"), s.tell(), s.getvalue(), s.seek(1), s.read(), s.seek(0), s.truncate(2), repr(s.getvalue()))
print("x", 1, file=s, end="!")
print(repr(s.getvalue()), type(s).__name__, isinstance(s, io.TextIOBase), s.readable(), s.writable(), s.seekable(), s.closed)
s.seek(0)
print([l for l in io.StringIO("a\nb\nc")], list(io.StringIO("p\r\nq", newline=None)), io.StringIO("p\r\nq", newline="").readlines())
s.close()
err(s.getvalue)
b = io.BytesIO(b"abc\ndef")
print(b.read(2), b.readline(), b.tell(), b.read(), b.getvalue(), b.seek(0, 2), b.write(b"!"), b.getvalue(), type(b).__name__)
b = io.BytesIO()
print(b.write(b"12345"), b.seek(2), b.write(b"x"), b.getvalue(), b.truncate(3), b.getvalue(), b.tell(), b.seek(10), b.write(b"z"), b.getvalue())
print(list(io.BytesIO(b"a\nb")), isinstance(b, io.BufferedIOBase), bytes(b.getbuffer())[:2])
