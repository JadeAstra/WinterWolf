# Makes lib/Snippy/Text/Case.ww from CPython (the Snippy plan, part 5.3): Python's full case
# mappings (lower, upper, title, casefold) and the two properties its final sigma rule and title()
# read, Cased and Case_Ignorable, which Python does not show but its own methods give away. Run in WSL:
#   python3.14 tools/snippy/case.py > lib/Snippy/Text/Case.ww
import sys

MAX = 0x110000
def chars():
    for cp in range(MAX):
        if 0xD800 <= cp <= 0xDFFF:
            continue
        yield cp, chr(cp)

# One code point to one: runs of (start, count, stride, delta); to more than one, or any title or
# casefold mapping that differs from upper or lower, a list of (code point, its mapping).
def runs_of(pairs):
    out = []
    i = 0
    while i < len(pairs):
        cp, to = pairs[i]
        delta = to - cp
        stride = 0
        n = 1
        if i + 1 < len(pairs):
            nxt, nto = pairs[i + 1]
            if nto - nxt == delta and nxt - cp in (1, 2):
                stride = nxt - cp
                n = 2
                while i + n < len(pairs):
                    c2, t2 = pairs[i + n]
                    if c2 - pairs[i + n - 1][0] == stride and t2 - c2 == delta:
                        n += 1
                    else:
                        break
        out.append((cp, n, stride if n > 1 else 1, delta))
        i += n
    return out

single = {"lower": [], "upper": []}
special = {"lower": [], "upper": [], "title": [], "casefold": []}
cased = []
ignorable = []
for cp, c in chars():
    lo, up, ti, cf = c.lower(), c.upper(), c.title(), c.casefold()
    for name, m in (("lower", lo), ("upper", up)):
        if m != c:
            if len(m) == 1:
                single[name].append((cp, ord(m)))
            else:
                special[name].append((cp, m))
    if ti != up:
        special["title"].append((cp, ti))
    if cf != lo:
        special["casefold"].append((cp, cf))
    # Cased: a letter after it stays lower in title().
    t = (c + "a").title()
    is_cased = t[-1] == "a"
    if is_cased:
        cased.append(cp)
    # Case_Ignorable: passed over when the final sigma rule looks for a cased letter.
    if is_cased:
        ign = ("AΣ" + c).lower()[1] == "ς"
    else:
        ign = ("A" + c + "Σ").lower()[-1] == "ς"
    if ign:
        ignorable.append(cp)

def ranges(cps):
    out = []
    for cp in cps:
        if out and out[-1][1] == cp - 1:
            out[-1][1] = cp
        else:
            out.append([cp, cp])
    return out

def ints(xs, per=16):
    lines = []
    for i in range(0, len(xs), per):
        lines.append("  " + ", ".join(str(x) for x in xs[i:i + per]) + ",")
    return "\n".join(lines)

print("// Made by tools/snippy/case.py from CPython 3.14: do not change by hand.")
print("// Python's full case mappings and the Cased and Case_Ignorable properties (the Snippy plan, part 5.3).")
for name in ("lower", "upper"):
    flat = []
    for r in runs_of(single[name]):
        flat.extend(r)
    print("internal fun __pyCaseRuns_%s() : List<int> => [\n%s\n]" % (name, ints(flat)))
for name in ("lower", "upper", "title", "casefold"):
    flat = []
    for cp, m in special[name]:
        flat.append(cp)
        flat.append(len(m))
        flat.extend(ord(x) for x in m)
    print("internal fun __pyCaseSpecial_%s() : List<int> => [\n%s\n]" % (name, ints(flat)))
for name, cps in (("cased", cased), ("ignorable", ignorable)):
    flat = []
    for a, b in ranges(cps):
        flat.extend((a, b))
    print("internal fun __pyCaseRanges_%s() : List<int> => [\n%s\n]" % (name, ints(flat)))
print("# runs lower %d upper %d; special %s; cased %d ignorable %d" % (len(runs_of(single["lower"])), len(runs_of(single["upper"])),
      {k: len(v) for k, v in special.items()}, len(ranges(cased)), len(ranges(ignorable))), file=sys.stderr)
