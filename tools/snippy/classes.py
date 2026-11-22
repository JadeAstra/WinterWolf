# Makes lib/Snippy/Text/Classes.ww: for every code point, the classes Python's str methods ask
# about that the general category does not settle, as the CPython that runs this answers them
# (CPython 3.14: Unicode 16.0). From the repository's top, in WSL:
#
#   python3.14 tools/snippy/classes.py > lib/Snippy/Text/Classes.ww
#
# Each code point gets five bits, from CPython's own methods, not worked out again here:
#   1  decimal      c.isdecimal()                  Numeric_Type=Decimal
#   2  digit        c.isdigit()                    Numeric_Type=Decimal or Digit
#   4  numeric      c.isnumeric()                  any Numeric_Type
#   8  id start     c.isidentifier()               XID_Start, or '_'
#   16 id continue  ("a" + c).isidentifier()       XID_Continue
# The table is the runs of one value: a letter for the value, then how far past the last run's
# start this one starts, in base 32 (digits 0-9 and a-v), as tools/snippy/unicode.py writes its own.
import sys

LETTERS = "ABCDEFGHIJKLMNOPQRSTUVWXYZ!#%&*+"   # no $, which a WinterWolf string reads as a value
DIGITS = "0123456789abcdefghijklmnopqrstuv"


def base32(n):
    if n == 0:
        return "0"
    out = ""
    while n:
        out = DIGITS[n % 32] + out
        n //= 32
    return out


def classes(cp):
    c = chr(cp)
    return ((1 if c.isdecimal() else 0) | (2 if c.isdigit() else 0) | (4 if c.isnumeric() else 0) |
            (8 if c.isidentifier() else 0) | (16 if ("a" + c).isidentifier() else 0))


text = []
prev_value = None
prev_start = 0
runs = 0
for cp in range(0x110000):
    v = classes(cp)
    if v != prev_value:
        text.append(LETTERS[v] + base32(cp - prev_start))
        prev_value = v
        prev_start = cp
        runs += 1
table = "".join(text)

out = sys.stdout
out.write("// The classes of every code point that Python's str methods ask about and the general category\n")
out.write("// does not settle: isdecimal, isdigit and isnumeric (Numeric_Type), and isidentifier's XID_Start\n")
out.write("// and XID_Continue. Unicode %s, made by tools/snippy/classes.py from the CPython it ran on\n" %
          __import__("unicodedata").unidata_version)
out.write("// (%d.%d), asking its own methods. Do not edit. Read by pyCharClasses in Text/Repr.ww.\n\n" %
          sys.version_info[:2])
out.write("// The %d runs, as tools/snippy/classes.py writes them.\n" % runs)
out.write("internal val __pyClassesTable : string =\n")
width = 96
pieces = [table[i:i + width] for i in range(0, len(table), width)]
for i, p in enumerate(pieces):
    out.write('  "' + p + '"' + (" +\n" if i + 1 < len(pieces) else "\n"))
