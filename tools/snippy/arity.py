# Makes lib/Snippy/Runtime/Arity.ww from CPython (the Snippy plan, part 5.3): for each method of
# Python's builtin types, how many positional arguments it takes, and the TypeError CPython gives
# for too few and for too many, so that Snippy's builtin methods say the same. Run in WSL:
#   python3.14 tools/snippy/arity.py > lib/Snippy/Runtime/Arity.ww
import re

samples = {
    "str": "abc", "bytes": b"abc", "bytearray": bytearray(b"abc"), "list": [1, 2], "tuple": (1, 2), "dict": {1: 2},
    "set": {1}, "frozenset": frozenset({1}), "int": 5, "float": 1.5, "complex": 1j, "range": range(3),
}
# A count's messages, with the count given written as {n}.
def probe(value, name, n):
    method = getattr(value, name)
    args = [object()] * n
    try:
        method(*args)
    except TypeError as e:
        text = str(e)
        if "takes no positional arguments" in text:
            return text
        # Only an error about the number of arguments.
        if re.search(r"argument|given|got \d", text) and re.search(r"\b%d\b" % n, text) and (
                "expected" in text or "takes" in text or "argument" in text and "given" in text):
            if re.search(r"must be|not '|object|cannot|unhashable|unsupported", text) and "takes" not in text and "expected" not in text:
                return None
            return re.sub(r"\b%d\b" % n, "{n}", text, count=1) if ("got %d" % n in text or "(%d given)" % n in text) else text
        return None
    except Exception:
        return None
    return None

rows = []
for tname, value in samples.items():
    for name in sorted(dir(value)):
        if name.startswith("__") and name not in ("__contains__", "__getitem__", "__len__", "__iter__", "__reversed__"):
            continue
        if not callable(getattr(value, name)):
            continue
        if name in ("fromkeys", "fromhex", "maketrans", "from_bytes", "from_number", "mro"):
            continue
        # Copies, so that a probe that changes the value changes nothing of the next.
        bad = {}
        for n in range(0, 7):
            v = value.copy() if hasattr(value, "copy") and tname not in ("frozenset",) else value
            m = probe(v, name, n)
            if m is not None:
                bad[n] = m
        ok = [n for n in range(0, 7) if n not in bad]
        if not ok:
            continue
        low, high = min(ok), max(ok)
        few = bad.get(low - 1) if low > 0 else None
        many = bad.get(high + 1)
        if high == 6:
            high = -1
            many = None
        rows.append((tname, name, low, high, few or "", many or ""))

def ww(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"').replace("$", "\\$") + '"'

print("// Made by tools/snippy/arity.py from CPython 3.14: do not change by hand.")
print("// How many positional arguments each method of Python's builtin types takes, and what CPython")
print("// says where a call gives fewer or more ({n} the count given); -1 for no most.")
print("internal fun __pyArityRows() : List<List<any>> => [")
for r in rows:
    print("  [%s, %s, %d, %d, %s, %s]," % (ww(r[0]), ww(r[1]), r[2], r[3], ww(r[4]), ww(r[5])))
print("]")
