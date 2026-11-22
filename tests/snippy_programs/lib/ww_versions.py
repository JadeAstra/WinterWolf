# Snippy on ww's own versions (U1-U26 of docs/snippy-ww-checklist.html): dates, text, bytes,
# numbers, sorting, deques and ranges, each as CPython 3.14 gives them.
import asyncio
import cmath
import collections
import datetime
import math
import random
import string
import textwrap
import time
from fractions import Fraction

# U1, U2, U4: the calendar.
print([(datetime.date(y, 3, 1) - datetime.date(y, 2, 1)).days for y in (1900, 2000, 2023, 2024, 4)], [datetime.date(y, 12, 31).timetuple().tm_yday for y in (1900, 2000, 2024)])
d = datetime.date(2024, 2, 29)
print(d.toordinal(), datetime.date.fromordinal(1), datetime.date.fromordinal(3652059), d.weekday(), d.isoweekday(), d.isocalendar(), d.timetuple().tm_yday)
print(datetime.date(1, 1, 1).isoformat(), datetime.date(9999, 12, 31).isoformat(), datetime.date(2024, 3, 1) - datetime.date(1999, 12, 31), d + datetime.timedelta(days=366))
utc = datetime.timezone.utc
print(datetime.datetime(2024, 2, 29, tzinfo=utc).timestamp(), datetime.datetime(1969, 12, 31, 23, 59, 59, tzinfo=utc).timestamp(), datetime.datetime.fromtimestamp(0, utc).date())
print(time.mktime((2024, 13, 1, 12, 0, 0, 0, 0, -1)) == time.mktime((2025, 1, 1, 12, 0, 0, 0, 0, -1)), time.mktime((2024, 2, 30, 12, 0, 0, 0, 0, -1)) == time.mktime((2024, 3, 1, 12, 0, 0, 0, 0, -1)))
t = (2024, 6, 15, 12, 30, 0, 0, 0, -1)
print(time.localtime(time.mktime(t))[:6], datetime.datetime(2024, 6, 15, 12, 30).timestamp() == time.mktime(t))

# U6, U7, U8, U9: str and int.
print("abc".ljust(6, "*"), "abc".rjust(6, "é"), "abcdef".ljust(3), "ünï".rjust(5), "xxab".removeprefix("xx"), "abyy".removesuffix("yy"), "ab".removesuffix(""))
print("a,b,,c".split(","), "a,b,,c".split(",", 1), "a,b,,c".split(",", 0), "a,b,,c".split(",", 99), "a,b,,c".split(",", -1), "".split(","), "a--b--".split("--"))
print((0).bit_length(), (1).bit_length(), (-1).bit_length(), (255).bit_length(), (-256).bit_length(), (2**200 + 1).bit_length(), (-2**63).bit_length())

# U10 to U14: bytes.
b = bytes(range(0, 256, 15))
print(b.hex(), b"".hex(), b.upper(), bytes([0xe9, 0x41, 0x7a]).swapcase(), b"MiXeD 1".lower())
table = bytes.maketrans(b"abc", b"xyz")
print(b"aabbcc".translate(table), b"aabbcc".translate(table, b"b"), b"hello".translate(None, b"l"), b"hello".translate(None))
print(b[2:5], b[5:2], b[::-3], bytearray(b"abcdef")[1:4], 200 in bytes([200]), 15 in b, b"" in b)
try:
    print(256 in b)
except ValueError as e:
    print("ValueError", e)
l = list(range(8))
l[2:4] = ["a", "b", "c"]
l[0:0] = [9]
print(l, l[1:6], l[6:1], l[::2])
print(b"caf\xe9".decode("latin-1"), "café".encode("latin-1"), b"%s %d \xff" % (b"x", 7))

# U15: textwrap's patterns, and a subclass of its own.
w = textwrap.TextWrapper(width=12)
print(w.wordsep_re.pattern == textwrap.TextWrapper.wordsep_re.pattern, w.sentence_end_re.flags, w.wordsep_simple_re.pattern)
print(w.wrap("A well-known self-contained word--used here. Then more--and more."))
print(textwrap.wrap("Hello there -- you goof-ball, use the -b option!", 10, break_on_hyphens=1))
print(textwrap.fill("End.  Next one!  Last? Yes.", 40, fix_sentence_endings=True))
class Commas(textwrap.TextWrapper):
    wordsep_simple_re = __import__("re").compile(r"(,)")
print(Commas(width=6, break_on_hyphens=False).wrap("aa,bb,cc,dd"))

# U16: string.Template by its pattern, and subclasses that change it.
T = string.Template
print(T("$who likes ${what}; $$5").substitute(who="tim", what="tea"), T("$a $b").safe_substitute(a=1), T.pattern.pattern == string.Template.pattern.pattern)
for bad in ["$", "ab $", "x\ny $1", "a\rb $!", "$$${x"]:
    try:
        T(bad).substitute(x=1)
    except ValueError as e:
        print(repr(bad), e)
class Percent(string.Template):
    delimiter = "%"
    idpattern = r"[a-z]+"
    braceidpattern = r"[A-Z]+"
print(Percent("%who and %{ID} and %% and %{who}").safe_substitute(who="x", ID="y"), Percent("%a %{B}").get_identifiers(), Percent("%1").is_valid())
class Plain(string.Template):
    flags = 0
print(Plain("$Who $who").safe_substitute(who=1), T("$Who").substitute(Who=2), sorted(T("$a ${b} $a $$c").get_identifiers()))
class Own(string.Template):
    pattern = r"\[(?:(?P<escaped>\[)|(?P<named>\w+)\]|(?P<braced>)(?!)|(?P<invalid>))"
print(Own("[a] [[ [b]").substitute(a=1, b=2), Own.pattern.pattern[:4])

# U18, U19, U20, U21, U22: numbers.
print(math.fmod(-0.0, 1), math.fmod(5.0, math.inf), math.fmod(-7.5, 2), math.fmod(1e300, 3.0), math.fmod(-1e-300, 1e-310))
for x, y in [(math.inf, 1.0), (1.0, 0.0)]:
    try:
        print(math.fmod(x, y))
    except ValueError as e:
        print("ValueError", e)
print(hash(Fraction(1, 3)), hash(Fraction(-7, 3)), hash(Fraction(1, 2**61 - 1)), hash(Fraction(5, 7)))
print(cmath.phase(-1 + 0j), cmath.phase(complex(-1, -0.0)), cmath.polar(3 + 4j), cmath.rect(1, -0.0), cmath.rect(-0.0, 0.0), cmath.rect(2, math.pi / 3), cmath.rect(math.inf, 1.0))
print((1 + 2j) * (3 - 4j), (1 + 2j) / (3 - 4j), (1 + 2j) ** -3, (1 + 2j) ** 10, 0j ** 5, (1e300 + 1e300j) / (1e300 + 1e-300j))
try:
    print((1 + 2j) / 0j)
except ZeroDivisionError as e:
    print("ZeroDivisionError", e)
print(-7 % 3, 7 % -3, -2**63 % -1, (-2**63) % 7, 7 % 2**70, -7 % -2)

# U23: SystemRandom, and seeding from text and bytes.
r = random.SystemRandom()
print(0.0 <= r.random() < 1.0, r.getrandbits(0), r.getrandbits(70) < 2**70)
random.seed("abc")
print(random.random(), random.getrandbits(80))
random.seed(b"xyz")
print(random.randrange(10**30))

# U24: sorting, stable, reversed, by key, with only __lt__.
pairs = [(2, "a"), (1, "b"), (2, "c"), (1, "d"), (3, "e"), (2, "f")]
print(sorted(pairs, key=lambda p: p[0]), sorted(pairs, key=lambda p: p[0], reverse=True))
words = "the quick brown fox jumps over the lazy dog".split()
words.sort(key=len)
print(words, collections.Counter("abracadabra").most_common())
class Lt:
    def __init__(self, v): self.v = v
    def __lt__(self, o): return self.v < o.v
    def __repr__(self): return "Lt(%d)" % self.v
print(sorted([Lt(3), Lt(1), Lt(2), Lt(1)]), max(sorted([5, 3, 9], reverse=True)))

# U25: deque, and asyncio's queues.
q = collections.deque([1, 2, 3, 4, 5], maxlen=5)
q.append(6); q.appendleft(0); q.rotate(2); print(q)
q.rotate(-3); q.insert(2, "x") if len(q) < 5 else None; print(q, q.index(5), q.count(6))
q2 = collections.deque("abcde")
q2.rotate(7); q2.reverse(); q2.remove("c"); q2.insert(1, "z"); q2.extendleft("12"); print(q2, len(q2), q2[0], q2[-1], q2 * 2, "z" in q2)
async def main():
    queue = asyncio.Queue(maxsize=2)
    out = []
    async def consumer(name):
        while True:
            item = await queue.get()
            out.append((name, item))
            queue.task_done()
    tasks = [asyncio.create_task(consumer(n)) for n in "AB"]
    for i in range(6):
        await queue.put(i)
    await queue.join()
    for t in tasks:
        t.cancel()
    print(sorted(i for _, i in out), queue.empty(), queue.qsize())
asyncio.run(main())

# U26: ranges.
for rg in [range(10), range(10, 0, -3), range(-5, 5, 2), range(0), range(5, 5), range(-2**61, 2**61, 2**59)]:
    print(len(rg), 4 in rg, -5 in rg, True in rg, 2**60 in rg, rg[-1] if len(rg) else None)
print({1, 2, 3} | {4}, len(set(range(1000))))
