# time, datetime and textwrap (the Snippy plan, part 5.3, step 3), as CPython 3.14 gives them:
# nothing here depends on the clock or the time zone of the machine.
import time
import datetime as dt
from datetime import date, datetime, timedelta, timezone, tzinfo, UTC
import textwrap

# time.
t0 = time.time()
print(type(t0).__name__, t0 > 1.7e9, type(time.time_ns()).__name__, time.perf_counter() <= time.perf_counter(), time.monotonic() > 0, time.process_time() >= 0)
g = time.gmtime(0)
print(g, g.tm_year, g[0], g.tm_wday, g.tm_yday, tuple(g), time.gmtime(1700000000))
print(time.strftime("%Y-%m-%d %H:%M:%S %a %A %b %B %j %w %y %p %I %%", time.gmtime(1700000000)), time.asctime(time.gmtime(86400 * 365)))
st = time.struct_time((2024, 2, 29, 13, 5, 9, 3, 60, 0))
print(st.tm_mon, st.tm_mday, time.strftime("%d/%m/%Y %H:%M", st), time.calendar_ns if False else 0, time.strptime("2024-03-01 10:20", "%Y-%m-%d %H:%M")[:6])
t1 = time.perf_counter()
time.sleep(0.01)
print(time.perf_counter() - t1 >= 0.009, time.gmtime(0) == time.gmtime(0))

# timedelta.
d = timedelta(days=1, hours=2, minutes=3, seconds=4, microseconds=5, milliseconds=6, weeks=1)
print(d, repr(d), d.days, d.seconds, d.microseconds, d.total_seconds(), timedelta(hours=-1), repr(timedelta(seconds=-1)), timedelta(0), timedelta(days=-1, seconds=1))
print(d * 2, d / 2, d // 3, d % timedelta(days=1), -d, abs(-d), d + d, d - timedelta(days=8), d / timedelta(hours=1), d // timedelta(hours=1), timedelta(1.5), timedelta(seconds=0.5))
print(timedelta.max, timedelta.min, timedelta.resolution, timedelta(days=2) > timedelta(hours=40), hash(timedelta(1)) == hash(timedelta(days=1)), bool(timedelta(0)))

# date.
a = date(2024, 2, 29)
print(a, repr(a), a.year, a.month, a.day, a.weekday(), a.isoweekday(), a.isocalendar(), a.isoformat(), a.toordinal(), a.timetuple()[:3], a.ctime())
print(a + timedelta(days=1), a - timedelta(days=366), date(2025, 1, 1) - a, a.replace(year=2028), date.fromordinal(738945), date.fromisoformat("2023-12-25"),
      date.fromisocalendar(2024, 1, 1), a.strftime("%A %d %B %Y, week %U %W, %G-W%V-%u"), f"{a:%Y/%m/%d}", date.min, date.max, a > date(2024, 1, 1))
print(date(2024, 12, 30).isocalendar(), date(2021, 1, 3).isocalendar().week, date.fromisoformat("20240229"), date.fromisoformat("2024-W09-4"))

# datetime and timezone.
b = datetime(2024, 2, 29, 13, 5, 9, 123456)
print(b, repr(b), b.date(), b.time(), b.hour, b.minute, b.second, b.microsecond, b.isoformat(), b.isoformat(sep=" ", timespec="minutes"), b.weekday())
print(b + timedelta(hours=11), b - datetime(2024, 1, 1), b.replace(microsecond=0), b.strftime("%Y%m%dT%H%M%S.%f %I:%M %p"), datetime.fromisoformat("2024-02-29T13:05:09.5"),
      datetime.combine(a, dt.time(8, 30)), datetime(2024, 1, 1) < b, b.timetuple()[:6], b.toordinal())
utc = timezone.utc
c = datetime(2024, 2, 29, 13, 5, tzinfo=utc)
est = timezone(timedelta(hours=-5), "EST")
print(c, repr(c), c.timestamp(), c.astimezone(est), c.utcoffset(), c.tzname(), est, repr(est), timezone(timedelta(hours=5, minutes=30)), c.isoformat())
print(datetime.fromtimestamp(1700000000, tz=utc), datetime.fromtimestamp(0, utc).isoformat(), datetime(2024, 1, 1, tzinfo=est) - datetime(2024, 1, 1, tzinfo=utc),
      datetime.fromisoformat("2024-02-29T13:05:09+02:00"), c == datetime(2024, 2, 29, 8, 5, tzinfo=est), datetime.strptime("29/02/2024 13:05", "%d/%m/%Y %H:%M"))
print(dt.time(23, 59, 59, 999999), dt.time(7, 5).isoformat(), dt.time.fromisoformat("10:20:30.25"), dt.time(1, 2, 3).replace(hour=5), dt.time(12, 0) > dt.time(9, 30),
      dt.time(14, 3).strftime("%H:%M %p"), datetime.min, datetime.max, dt.MINYEAR, dt.MAXYEAR, date(2000, 1, 1).timetuple().tm_yday)
for f in [lambda: date(2023, 2, 29), lambda: datetime(2024, 13, 1), lambda: date.fromisoformat("2024-1-1"), lambda: timedelta(days=10**10),
          lambda: datetime(2024, 1, 1) - datetime(2024, 1, 1, tzinfo=utc), lambda: datetime.strptime("x", "%Y"), lambda: date(2024, 1, 1) + 1]:
    try:
        print(f())
    except Exception as e:
        print(type(e).__name__, e)

# textwrap.
text = "The quick brown fox jumps over the lazy dog. " * 3
print(textwrap.wrap(text, width=30))
print(textwrap.fill(text, width=40, initial_indent="* ", subsequent_indent="  "))
print(textwrap.shorten("Hello  world!   How are you today?", width=20), textwrap.shorten("Hi there", width=20), textwrap.shorten("a b c d e f", 7, placeholder=".."))
print(repr(textwrap.dedent("    one\n      two\n    three\n")), repr(textwrap.indent("a\n\nb\n", "> ")), repr(textwrap.indent("a\nb", "+", lambda line: True)))
w = textwrap.TextWrapper(width=12, break_long_words=True, max_lines=3, placeholder=" [..]")
print(w.wrap("supercalifragilisticexpialidocious is a long word indeed and more"), textwrap.wrap("a-hyphenated-compound word here", 10))
print(textwrap.wrap("tab\there and\tthere", 10, expand_tabs=True, tabsize=4), textwrap.wrap("  lead and trail  ", 8, drop_whitespace=False),
      textwrap.wrap("one. Two. three", 6, fix_sentence_endings=True), textwrap.wrap("", 5), textwrap.wrap("x " * 3, 1))

# More of datetime: rounding, offsets with seconds, ISO forms, subclasses, a tzinfo of its own, errors.
print(timedelta(microseconds=-1), timedelta(days=-3, hours=5), repr(timedelta(days=-3, hours=5)), timedelta(seconds=1.0000005), timedelta(microseconds=2.5), timedelta(microseconds=3.5), timedelta(minutes=0.1))
print(timedelta(days=1) / 3, timedelta(days=1) * 0.1, timedelta(hours=5) // 2, divmod(timedelta(hours=5), timedelta(hours=2)), timedelta(hours=1) * -1.5, timedelta(1) / 7.0)
print(-timedelta(days=1, seconds=5), str(timedelta(days=-1, seconds=86399, microseconds=999999)), timedelta(weeks=-0.5), timedelta(hours=1).total_seconds(), timedelta(microseconds=1).total_seconds())
tz = timezone(timedelta(hours=5, minutes=30, seconds=15, microseconds=7))
d = datetime(2024, 3, 10, 2, 30, tzinfo=tz)
print(d, d.strftime("%z %:z %Z|%f|%%"), tz, repr(tz), timezone(timedelta(0)), repr(timezone(timedelta(0))), repr(UTC), timezone(-timedelta(hours=3)).tzname(None))
print(datetime.fromisoformat("2024-02-29"), datetime.fromisoformat("2024-02-29T13"), datetime.fromisoformat("2024-02-29 13:05"), datetime.fromisoformat("20240229T130509"), datetime.fromisoformat("2024-02-29T13:05:09.123456789"),
      datetime.fromisoformat("2024-02-29T13:05Z"), datetime.fromisoformat("2024-02-29T13:05:09-0330"), datetime.fromisoformat("2024-W09-4T10:00"), datetime.fromisoformat("2024-02-29T24:00"))
print(dt.time.fromisoformat("1020"), dt.time.fromisoformat("T10:20:30Z"), dt.time(1, tzinfo=tz), repr(dt.time(1, 2, tzinfo=timezone.utc)), repr(dt.time(0)), repr(dt.time(1, 2, 3, 4)), dt.time(5).isoformat("milliseconds"))
print(date(2024, 1, 1) == datetime(2024, 1, 1), date(2024,1,1) != datetime(2024,1,1), hash(datetime(2024,1,1,tzinfo=UTC)) == hash(datetime(2024,1,1,5,tzinfo=timezone(timedelta(hours=5)))), datetime(2024,1,1,tzinfo=UTC) == datetime(2024,1,1,5,tzinfo=timezone(timedelta(hours=5))))
print(datetime(2024,1,1,tzinfo=UTC) - datetime(2023,12,31,20,tzinfo=timezone(timedelta(hours=-5))), date(2024,3,1) - date(2023,3,1), date.max - date.min, datetime(2024,1,1) == datetime(2024,1,1,tzinfo=UTC))
print(d.astimezone(UTC), d.astimezone(timezone(timedelta(hours=-8))), d.utctimetuple()[:6], d.timetuple().tm_isdst, d.timestamp(), datetime.fromtimestamp(1.5, UTC), datetime.fromtimestamp(-1.000001, UTC), datetime.fromtimestamp(0.0000005, UTC), datetime.fromtimestamp(0.0000015, UTC))
print(date(2024,2,29).ctime(), datetime(2024,2,9,3,4,5).ctime(), f"{datetime(2024,1,2):%d.%m}", format(date(2024,1,2)), format(dt.time(1,2), "%H"), str(datetime.min.replace(tzinfo=UTC)))
class D(datetime): pass
print(repr(D(2024, 1, 2)), D(2024,1,2) + timedelta(1), type(D(2024,1,2) + timedelta(1)).__name__, D.fromisoformat("2024-01-01").__class__.__name__, repr(date(2024,1,2).isocalendar()))
class Fixed(tzinfo):
    def utcoffset(self, dt): return timedelta(hours=1)
    def dst(self, dt): return timedelta(0)
    def tzname(self, dt): return "FX"
f = datetime(2024, 6, 1, 12, tzinfo=Fixed())
print(f, f.tzname(), f.astimezone(UTC), f.strftime("%Z %z"), f.timetuple().tm_isdst, f.dst())
for g in [lambda: timedelta(days=1000000000), lambda: timezone(timedelta(hours=24)), lambda: datetime(2024, 1, 1, 24), lambda: dt.time(1, 60), lambda: date(0, 1, 1),
          lambda: datetime(2024, 1, 1) < datetime(2024, 1, 1, tzinfo=UTC), lambda: date(2024,1,1) < datetime(2024,1,1), lambda: date.max + timedelta(1), lambda: timedelta(1) / 0,
          lambda: datetime.fromisoformat("2024-02-30"), lambda: date(2024, 1, 1.0), lambda: timedelta("1"), lambda: datetime.strptime("2024 junk", "%Y"), lambda: timezone(5)]:
    try:
        print(g())
    except Exception as e:
        print(type(e).__name__, e)

# More of textwrap: hyphens, em-dashes, long words, placeholders, dedent.
for t in ["Hello--world and so--on -- here", "well-known a-b-c x--y 12-34 ab-cd-ef", "e-mail re-use self-contained foo_bar-baz", "--leading dashes--", "word, --and; this?--that 'quoted'--x"]:
    for w in [1, 3, 5, 8, 12]:
        print(w, textwrap.wrap(t, w), textwrap.wrap(t, w, break_on_hyphens=False), textwrap.wrap(t, w, break_long_words=False))
print(textwrap.fill("a b c d e f g h i j", 5, max_lines=2), textwrap.wrap("aaa bbb ccc ddd", 7, max_lines=2, placeholder="..."))
print(repr(textwrap.dedent("\t  x\n\t  y\n\n   \n\t z")), repr(textwrap.dedent("  \n  ")), repr(textwrap.dedent("x\n  y")))
try:
    textwrap.wrap("abc", 0)
except ValueError as e:
    print(e)
try:
    textwrap.wrap("abc abc", 4, max_lines=1, placeholder=" [......]")
except ValueError as e:
    print(e)
w = textwrap.TextWrapper(); w.width = 6; print(w.wrap("abc def ghi"), w.width, w.placeholder)
