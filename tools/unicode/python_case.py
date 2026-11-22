# What CPython's str methods on case give for every code point, folded into one number each, for
# tests/case_python.ww: WinterWolf's upper, lower, foldCase, title, swapCase, isUpper, isLower and
# isTitle must give Python's, so that Snippy can use them (its own tables are gone). Run in WSL:
#
#   python3.14 tools/unicode/python_case.py
#
# Each number: h = (h * 1000003 + x + 1) mod 1000000007 for each code point x of the answer, then
# once more with x = -1 to end it (a bool is the code point 1 or 0). Lone surrogates are left out.
# The quick numbers (the test under GC stress) are of every 401st code point only.
import sys
import unicodedata

M = 1000000007


def fold(h, text):
    for ch in text:
        h = (h * 1000003 + ord(ch) + 1) % M
    return (h * 1000003) % M


names = ["upper", "lower", "foldCase", "title", "swapCase", "isUpper", "isLower", "isTitle"]
funcs = [str.upper, str.lower, str.casefold, str.title, str.swapcase,
         lambda c: "\x01" if c.isupper() else "\x00", lambda c: "\x01" if c.islower() else "\x00",
         lambda c: "\x01" if c.istitle() else "\x00"]


def numbers(step):
    hashes = [0] * len(funcs)
    for cp in range(0, 0x110000, step):
        if 0xD800 <= cp <= 0xDFFF:
            continue
        c = chr(cp)
        for k, f in enumerate(funcs):
            hashes[k] = fold(hashes[k], f(c))
    return hashes


print("// CPython %s, Unicode %s" % (sys.version.split()[0], unicodedata.unidata_version))
print("val python = %s" % numbers(1))
print("val pythonQuick = %s" % numbers(401))
