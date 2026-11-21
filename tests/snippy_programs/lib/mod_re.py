# re (the Snippy plan, part 5.3, step 4), as CPython 3.14 gives it: patterns read and matched by
# the engine's Python syntax, and the module's Pattern and Match objects around it.
import re
import copy

# Searching and matching.
p = re.compile(r"(?P<key>\w+)=(?P<value>\d+)")
m = p.search("x: alpha=12, beta=345")
print(m, m.group(), m.group(0), m.group(1), m.group("value"), m.group(1, "value"), m[0], m["key"], m.groups(), m.groupdict())
print(m.start(), m.end(), m.span(), m.span(2), m.start("value"), m.end(1), m.pos, m.endpos, m.lastindex, m.lastgroup, m.re is p, m.string, m.regs)
print(p.match("alpha=1"), p.match("x alpha=1"), p.fullmatch("a=1"), p.fullmatch("a=1 "), p.search("a=1", 1), p.search("ab=12", 0, 4), p.groups, p.groupindex, p.pattern, p.flags)
print(re.search(r"\d+", "ab 42 7").group(), re.match("ab", "abc").span(), re.fullmatch(r"a.c", "abc").group(), re.search("z", "abc"), re.match(r"(a)(b)?", "ac").groups(), re.match(r"(a)(b)?", "ac").groups("-"))
print(re.findall(r"\d+", "a1b22c333"), re.findall(r"(\w)=(\d)", "a=1 b=2"), re.findall(r"(\w)=\d", "a=1 b=2"), re.findall("", "ab"), re.findall(r"a*?", "aa"), re.findall(r"\b", "a b"))
print([m.span() for m in re.finditer(r"\w+", "one two  three")], [m.group() for m in re.finditer(r"x*", "axxb")], list(re.finditer("(a)|b", "ab"))[1].groups())

# Replacing and splitting.
print(re.sub(r"\d", "#", "a1b2c3"), re.sub(r"(\w+) (\w+)", r"\2 \1", "hello world"), re.sub(r"(?P<a>\w)", r"[\g<a>\g<0>\g<1>]", "xy"), re.subn("a", "b", "aaa"), re.sub("a", "b", "aaa", count=2))
print(re.sub("x*", "-", "abxd"), re.sub("", "-", "abc"), re.sub(r"\w+", lambda m: m.group().upper(), "go to it"), re.sub("a", r"\n\t\\", "xa"), re.sub("(a)|b", r"[\1]", "ab"), re.sub("a", "-", "b"))
print(re.split(r"\W+", "Words, words, words."), re.split(r"(\W+)", "Words, words."), re.split(r"\W+", "Words, words, words.", maxsplit=1), re.split("x*", "axbc"), re.split(r"\b", "a b"), re.split("(a)|(b)", "1a2b3"))
print(re.escape("a.b*c?(d)[e]{f} g\t-#&~"), re.escape(b"a.b"), re.escape("é_1"))

# Flags.
print(re.I, re.IGNORECASE, re.M | re.I, re.I | re.M | re.S, re.A, re.X, re.NOFLAG, re.I & re.M, int(re.I), re.I == 2, type(re.I).__name__, re.U | re.L, repr(re.DOTALL))
print(re.findall("(?i)straße", "STRASSE straße Straße"), re.findall("ſ", "sſS", re.I), re.findall("k", "kK\u212a", re.I), re.findall(r"[a-z]+", "ÀbC", re.I), re.findall(r"\w+", "naïve café", re.A), re.findall(r"\w+", "naïve café"))
print(re.findall("^a", "a\na\n", re.M), re.findall("a$", "a\na\n", re.M), re.findall("a$", "a\na\n"), re.findall("^", "a\n", re.M), re.findall(".", "a\nb"), re.findall(".", "a\nb", re.S), re.findall(r"\Aa|b\Z", "ab"))
print(re.compile(r"""
    (\d+)   # digits
    \s* ,   # a comma
""", re.X).findall("12 , 3,"), re.compile("a", re.I | re.M), re.compile(b"a", re.I), re.compile("a"), re.compile("(?x) a b").pattern, re.compile("(?i)a").flags, re.compile(b"a").flags, re.compile("a", re.A).flags)

# More of the syntax.
print(re.match(r"(a)?(?(1)b|c)", "ab").group(), re.match(r"(a)?(?(1)b|c)", "c").group(), re.match(r"(?P<q>['\"])?x(?(q)(?P=q))", "'x'").group(), re.search(r"(?<=ab)c", "abc").span(), re.search(r"(?<!a)c", "ac bc").span(), re.search(r"\d+(?=px)", "10em 20px").group())
print(re.match(r"a{2,3}", "aaaa").group(), re.match(r"a{,2}", "aaa").group(), re.match(r"a{2}", "aaa").group(), re.match("a{", "a{").group(), re.match("a{1,x}", "a{1,x}").group(), re.match(r"x*+x", "xxx"), re.match(r"(?>a+)b", "aab").group(), re.match(r"a+?", "aaa").group())
print(re.match(r"(a|b)*", "abab").group(1), re.match(r"(a)|(b)", "b").lastindex, re.match(r"(a(b))", "ab").lastindex, re.match(r"((a)b)", "ab").lastgroup, re.match(r"(?P<x>a)(?P<y>b)?", "a").lastgroup, re.match("a", "a").lastindex)
print(re.match(r"\x41\u00e9\U0001F600\101\0", "Aé\U0001F600A\0").group(), re.findall(r"[\d\s]", "1 a2"), re.findall(r"[^\W\d]", "a1_b"), re.findall(r"[a\-z]", "a-z"), re.findall(r"[]a]", "]a"), re.findall(r"(?#comment)a", "a"), re.findall(r"\d", "١٢3"), re.findall(r"\d", "١٢3", re.A))
print(re.match(r"(?i:a)b", "Ab"), re.match(r"(?i:a)b", "AB"), re.match(r"(?-i:a)b", "aB", re.I), re.match(r"(?s:.)", "\n"), re.findall(r"(?a:\w)\w", "éé aé"), bool(re.match(r"\bé", "é")), bool(re.match(r"(?a)\bé", "é")))

# Match objects and patterns as objects.
m = re.match(r"(\w+) (\w+)", "Isaac Newton, physicist")
print(m.expand(r"\2, \1"), m.expand(r"\g<2>!"), repr(m), m[1], len(m.groups()), copy.copy(m) is m, copy.deepcopy(p) is p, re.compile(r"(\w+) (\w+)") is re.compile(r"(\w+) (\w+)"), p == re.compile(p.pattern), re.compile(p) is p)
print(repr(re.match(".*", "x" * 80)), re.match(b"(a)(b)", b"abc"), re.match(b"(a)(b)", b"abc").groups(), re.findall(rb"\w", b"a\xe9b"), re.sub(b"a", b"b", b"aa"), re.split(b",", b"a,b"))
print(re.compile("a" * 300), [x for x in dir(re.match("a", "a")) if not x.startswith("_")])

# What is wrong.
for f in [lambda: re.compile("("), lambda: re.compile("a**"), lambda: re.compile("[z-a]"), lambda: re.compile("(?P<1>x)"), lambda: re.compile("\\q"), lambda: re.compile("(?<=a|bc)d"),
          lambda: re.compile("a)"), lambda: re.compile("(?P=n)"), lambda: re.compile(r"(a)\2"), lambda: re.compile("*"), lambda: re.compile("[a"), lambda: re.compile("a\n(b"), lambda: re.compile("(?z)"),
          lambda: re.compile("a{3,2}"), lambda: re.compile(r"\x4"), lambda: re.compile("a", re.L), lambda: re.compile("x(?i)"), lambda: re.compile("(?P<a>x)(?P<a>y)"), lambda: re.compile("(?(2)a)"),
          lambda: re.compile("\\"), lambda: re.compile("a{4294967295}"), lambda: re.sub("(a)", r"\2", "a"), lambda: re.sub("(a)", r"\g<x>", "a"), lambda: re.sub("a", r"\q", "a"), lambda: re.match("a", 5),
          lambda: re.match(b"a", "a"), lambda: re.match("a", b"a"), lambda: re.compile(re.compile("a"), re.I), lambda: re.compile(5), lambda: re.match("(a)", "a").group(2), lambda: re.match("(a)", "a").group("x"),
          lambda: re.sub("a", lambda m: 5, "a")]:
    try:
        print(f())
    except Exception as e:
        print(type(e).__name__, e, getattr(e, "pos", "-"), getattr(e, "lineno", "-"), getattr(e, "colno", "-"))
e = None
try:
    re.compile("ab\n(cd")
except re.error as err:
    e = err
print(type(e).__name__, e.msg, e.pattern == "ab\n(cd", e.pos, e.lineno, e.colno, re.error is re.PatternError, isinstance(e, Exception))
