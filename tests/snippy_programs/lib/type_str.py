# str: every method, its corners as CPython 3.14 has them (the Snippy plan, part 5.3).

s = "Hello, World! hello"
print(s.count("l"), s.count("l", 5), s.count("l", 5, 12), s.count(""), s.count("", 3, 5), "aaaa".count("aa"))
print(s.find("o"), s.find("o", 5), s.find("zz"), s.rfind("o"), s.rfind("l", 0, 5), s.find("", 30), s.rfind(""))
print(s.index("W"), s.rindex("l"))
for f in [lambda: s.index("zz"), lambda: s.rindex("zz"), lambda: s.count(1)]:
    try:
        f()
    except Exception as e:
        print(type(e).__name__, e)
print(s.startswith("Hell"), s.startswith(("x", "He")), s.endswith("llo"), s.endswith("World", 0, 12), s.startswith("", 50))

# Case, Unicode's own.
x = "hello WORLD ßtraße ǅ éclair ΣΑΣ"
print(x.upper(), "|", x.lower(), "|", x.swapcase(), "|", x.capitalize(), "|", x.title(), "|", x.casefold())
print("they're bill's friends from the UK".title(), "ﬁne".upper(), "İ".lower(), len("İ".lower()), "ΣΑΣ σ".lower())
for t in ["abc", "ABC", "Abc Def", "abc1", "123", "½", "²", "٣", " \t\n\x1c\x85\xa0", "", "é", "ǅ", "Ab Cd", "aB", "a_b", "1a", "_x", "x́"]:
    print(repr(t), t.isalnum(), t.isalpha(), t.isascii(), t.isdecimal(), t.isdigit(), t.isnumeric(), t.islower(),
          t.isupper(), t.isspace(), t.istitle(), t.isidentifier(), t.isprintable())

# Padding.
print("ab".center(7), "ab".center(7, "*"), "ab".ljust(5, "-"), "ab".rjust(5, "-"), "ab".center(1), "abc".center(6, "é"))
print("42".zfill(5), "-42".zfill(5), "+7".zfill(4), "abc".zfill(2), "".zfill(3))
print("a\tbc\td".expandtabs(), "a\tbc\td".expandtabs(4), "x\ty\nab\tc".expandtabs(3), "\t".expandtabs(0))
try:
    "ab".center(5, "**")
except TypeError as e:
    print("TypeError", e)

# Stripping and splitting.
w = " \t\x0b\x0c\x1c spaced \xa0  \n"
print(repr(w.strip()), repr(w.lstrip()), repr(w.rstrip()), "xxhixx".strip("x"), "abcba".strip("ab"), "abc".strip(""))
print("a b  c".split(), "  a b c  ".split(None, 1), "a,b,,c".split(","), "a,b,c".split(",", 1), "".split(), "".split(","))
print("a b c".rsplit(), "a,b,c".rsplit(",", 1), "  a  b  ".rsplit(None, 1), "a b c".split(maxsplit=1), "a-b-c".rsplit(sep="-", maxsplit=1))
print("one\ntwo\r\nthree\rfour\x0bfive\x0csix\x1cseven\x85eight nine".splitlines(), "a\nb\n".splitlines(True), "".splitlines())
print("a-b-c".partition("-"), "a-b-c".rpartition("-"), "abc".partition("x"), "abc".rpartition("x"))
for f in [lambda: "abc".partition(""), lambda: "abc".split("")]:
    try:
        f()
    except ValueError as e:
        print("ValueError", e)
print("prefix-body".removeprefix("prefix-"), "body.txt".removesuffix(".txt"), "x".removeprefix("y"), "abc".removesuffix(""))
print("aaa".replace("a", "b"), "aaa".replace("a", "b", 2), "abc".replace("", "-"), "abc".replace("", "-", 2), "abc".replace("b", ""),
      "aaa".replace("a", "bb", count=1))
print(", ".join(["a", "b", "c"]), "".join([]), "-".join("xyz"), "+".join(map(str, range(4))))
try:
    "-".join(["a", 1])
except TypeError as e:
    print("TypeError", e)

# Translation.
t = str.maketrans("abc", "xyz", "d")
print(sorted(t.items()), "aabbccdd".translate(t), "hello".translate({ord("l"): None, ord("h"): "J"}), str.maketrans({"a": "1", "b": None}))
for f in [lambda: str.maketrans("ab", "x"), lambda: str.maketrans({"ab": 1}), lambda: str.maketrans(1)]:
    try:
        f()
    except Exception as e:
        print(type(e).__name__, e)

# Formatting.
print("{} {}".format(1, 2), "{1} {0}".format("a", "b"), "{x}-{y}".format(x=1, y=2), "{0[1]} {1.real}".format([5, 6], 3),
      "{:>6}|{:<6}|{:^6}|{:*^7}".format("a", "b", "c", "d"), "{:,} {:_x} {:08.3f} {:+d} {:e} {:%}".format(1234567, 255, 3.14159, 5, 1234.5, 0.25))
print("{!r} {!s} {!a}".format("é", "é", "é"), "{{}} {}".format(7), "{:{w}.{p}f}".format(3.14159, w=8, p=2), "{0:b} {0:o} {0:#x}".format(10))
print("%(a)s and %(b)d" % {"a": "x", "b": 3}, "%5.1f|%-5d|%05d|%x|%r" % (2.345, 42, 7, 255, "q"), format(3.5, "08.2f"), format("x", "^5"))
print("{name} is {age}".format_map({"name": "Ann", "age": 30}))
for f in [lambda: "{}".format(), lambda: "{0} {}".format(1, 2), lambda: "{:d}".format("x"), lambda: "{".format(1), lambda: "%d" % "x",
          lambda: "%s %s" % ("a",), lambda: "%s" % ("a", "b")]:
    try:
        f()
    except Exception as e:
        print(type(e).__name__, e)

# encode.
print("café".encode(), "café".encode("latin-1"), "abc".encode("ascii"), "café".encode("ascii", "replace"), "café".encode("ascii", "ignore"),
      "café".encode("ascii", "backslashreplace"), "café ☃".encode("ascii", "xmlcharrefreplace"), "€".encode("utf-16-le"), "€".encode("utf-32-be"))
try:
    "café".encode("ascii")
except UnicodeEncodeError as e:
    print("UnicodeEncodeError", e)

# The rest.
print(s[0], s[-1], s[1:5], s[::-1], s[::3], s[100:], len(s), "ab" * 3, 2 * "x", "a" + "b", "x" in "xyz", "" in "abc")
print("abc" < "abd", "abc" < "ab", "Z" < "a", "é" > "z", "abc" == "abc", hash("abc") == hash("abc"), list("hi"), sorted("bca"))
print(repr("a'b"), repr('a"b'), repr("a'\"b"), repr("\t\n\r\x00\x7f\\​\U0001f600é"), ascii("é☃\U0001f600"), str(5), str(b"x"), str(b"\xc3\xa9", "utf-8"))
print(str.__name__, type("").__name__, isinstance("", str), str(), str(None), str([1, "a"]), "%s" % None)
print(min("hello"), max("hello"), "".join(reversed("abc")), "a,b".split(",")[1], ord("é"), chr(233), chr(0x1F600))
