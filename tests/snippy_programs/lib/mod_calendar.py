# calendar, and the rest of S8-S10, U3, U17 (docs/snippy-ww-checklist.html), as CPython 3.14 gives them.
import calendar
import cmath
import json
import math
import time

# The module's functions.
print([calendar.isleap(y) for y in (1900, 2000, 2023, 2024, -4, 2024.0)], calendar.leapdays(1900, 2024), calendar.leapdays(2024, 1900))
print(calendar.weekday(2024, 2, 29), calendar.weekday(10000, 1, 1), calendar.weekday(-1, 3, 1), calendar.monthrange(2024, 2), calendar.monthrange(2023, 12))
print(calendar.monthcalendar(2024, 3), calendar.timegm((2024, 2, 29, 12, 30, 15)), calendar.timegm((1969, 12, 31, 23, 59, 59.5, 0, 0, 0)))
print(calendar.firstweekday(), calendar.weekheader(2), "|" + calendar.weekheader(3) + "|", calendar.weekheader(9))
print(list(calendar.day_name), list(calendar.day_abbr), calendar.month_name[1:4], calendar.month_abbr[0] == "", len(calendar.month_name), len(calendar.day_abbr))
print(calendar.formatstring(["a", "bb"], 5, 2) + "|", calendar.EPOCH, calendar.mdays, calendar.error)
for bad in [lambda: calendar.monthrange(2024, 13), lambda: calendar.monthrange(2024, 0), lambda: calendar.setfirstweekday(7), lambda: calendar.month(2024, 13)]:
    try:
        bad()
    except ValueError as e:
        print(type(e).__name__, e, isinstance(e, IndexError), repr(e))

# Day and Month, as IntEnums.
print(calendar.MONDAY, repr(calendar.SUNDAY), calendar.Day(3), repr(calendar.Day(3)), calendar.Day["FRIDAY"].name, calendar.JANUARY.value, calendar.Month(12) is calendar.DECEMBER)
print(list(calendar.Day), len(calendar.Month), 3 in calendar.Day, 9 in calendar.Day, repr(calendar.Day), calendar.MONDAY + 1, calendar.FRIDAY == 4, "%d %s %r" % (calendar.MAY, calendar.MAY, calendar.MAY), f"{calendar.MAY:>3}")
try:
    calendar.Day(9)
except ValueError as e:
    print("ValueError", e)

# The text calendars.
calendar.prmonth(2024, 2)
print(calendar.month(2024, 1, 3, 2), end="")
print(calendar.calendar(2024))
calendar.setfirstweekday(calendar.SUNDAY)
print(calendar.month(2025, 6), calendar.firstweekday(), calendar.monthcalendar(2025, 6)[0])
calendar.setfirstweekday(0)
t = calendar.TextCalendar(6)
print(t.formatmonth(2024, 9), t.firstweekday, list(t.iterweekdays()))
t.firstweekday = 9
print(t.firstweekday, t.getfirstweekday(), t.formatweekheader(3))
print(t.formatyear(2023, 2, 1, 2, 4))
class Marked(calendar.TextCalendar):
    def formatday(self, day, weekday, width):
        return ("*" if day == 13 else " ") + super().formatday(day, weekday, width - 1)
print(Marked().formatmonth(2024, 9))
c = calendar.Calendar()
print(list(c.itermonthdates(2024, 2))[:3], list(c.itermonthdays2(2024, 2))[:3], list(c.itermonthdays3(2023, 12))[-2:], list(c.itermonthdays4(2024, 2))[0])
print(c.monthdays2calendar(2024, 2)[0], len(c.yeardays2calendar(2024, 2)), len(c.yeardayscalendar(2024, 4)[0]), c.yeardatescalendar(2024)[0][0][0][0])
lt = calendar.LocaleTextCalendar(locale="C")
print(lt.locale, lt.formatmonthname(2024, 3, 20))

# The HTML calendars.
h = calendar.HTMLCalendar()
print(h.formatmonth(2024, 2))
page = h.formatyearpage(2024, css=None)
print(type(page).__name__, len(page), page[:120])
class Red(calendar.HTMLCalendar):
    cssclasses = ["red"] * 7
    cssclass_noday = "none"
print(Red(6).formatweek([(0, 6), (1, 0), (2, 1)]), Red().formatweekday(2), Red().formatmonthname(2024, 5, withyear=False))

# S8: real and complex together, as CPython 3.14 (C11's Annex G).
print(-1 - 0j, 2 - 0j, 0.0 * complex(math.inf, 0.0), complex(1, math.inf) / 2, 2.0 / complex(-0.0, 2.0), cmath.phase(-1 - 0j), (1 + 0j) * complex(math.inf, math.nan))
for f in [lambda: complex(math.inf, 0) ** 2.0, lambda: 2.0 ** complex(1, math.inf), lambda: 0j ** -1, lambda: (1e308 + 1e308j) ** 2]:
    try:
        print(f())
    except (OverflowError, ZeroDivisionError) as e:
        print(type(e).__name__, e)

# S9: rect's special values.
print(cmath.rect(math.inf, 0.0), cmath.rect(-math.inf, -0.0), cmath.rect(math.nan, 0.0), cmath.rect(0.0, math.inf), cmath.rect(math.inf, math.nan), cmath.rect(-math.inf, 1.0))
try:
    cmath.rect(1.0, math.inf)
except ValueError as e:
    print("ValueError", e)

# U3, U17: names and padding; JSON's line and column.
print(time.strftime("%a %A %b %B %Y|%C|%d|%e", (2024, 9, 1, 0, 0, 0, 6, 245, 0)), time.strptime("Sun Sep 1 2024", "%a %b %d %Y")[:3])
for doc in ['{"a": 1,\n  "b": ]', '[1, 2', '\n\n  x']:
    try:
        json.loads(doc)
    except json.JSONDecodeError as e:
        print(e.lineno, e.colno, e.pos, e)
for pos in [3, 0, 10, -2]:
    e = json.JSONDecodeError("m", "ab\ncd", pos)
    print(pos, e.lineno, e.colno)

# A list's slices taken out.
l = list(range(10))
del l[2:5]
del l[-2:]
l[1:3] = []
del l[::3]
print(l)
