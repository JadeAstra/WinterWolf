# Makes src/regex_unicode.inc: what Python's re (CPython's _sre) takes a character to be, for the
# regex engine's Python syntax (Snippy's re, RegexOption.python): whether it is a word character,
# a decimal digit or whitespace in Unicode's sense (str.isalnum or "_", str.isdecimal,
# str.isspace), a letter (str.isalpha), a character a name may start or go on with
# (str.isidentifier), its lowercase (_sre.unicode_tolower), and how it compares without case (its
# lowercase, with CPython's table of lowercase letters that share an uppercase one, re._casefix).
# From the repository's top, in WSL:
#
#   python3.14 tools/snippy/regex_unicode.py > src/regex_unicode.inc
#
# Two stages: kPyBlock[c >> 7] picks a block of 128 entries in kPyEntry, each an index into
# kPyRecord: { flags, the character's case form minus the character, its lowercase minus it }.
# The case form is the smallest of the lowercase letters a character's lowercase is one with; two
# characters are equal without case when their forms are. kPyCaseGroup lists (form, character)
# for every character whose form is not itself alone, sorted, so that a class can be asked for
# each of them.
import _sre
from re import _casefix

WORD, DECIMAL, SPACE, ALPHA, ID_START, ID_CONTINUE = 1, 2, 4, 8, 16, 32

form = [0] * 0x110000
for c in range(0x110000):
    lo = _sre.unicode_tolower(c)
    group = [lo] + list(_casefix._EXTRA_CASES.get(lo, ()))
    form[c] = min(group)

members = {}
for c in range(0x110000):
    members.setdefault(form[c], []).append(c)

records = []
record_index = {}
entries = []
for c in range(0x110000):
    ch = chr(c)
    flags = 0
    if ch.isalnum() or ch == "_":
        flags |= WORD
    if ch.isdecimal():
        flags |= DECIMAL
    if ch.isspace():
        flags |= SPACE
    if ch.isalpha():
        flags |= ALPHA
    if ch.isidentifier():
        flags |= ID_START
    if ("a" + ch).isidentifier():
        flags |= ID_CONTINUE
    key = (flags, form[c] - c, _sre.unicode_tolower(c) - c)
    if key not in record_index:
        record_index[key] = len(records)
        records.append(key)
    entries.append(record_index[key])

blocks = []
block_index = {}
stage1 = []
for b in range(0, 0x110000, 128):
    block = tuple(entries[b:b + 128])
    if block not in block_index:
        block_index[block] = len(blocks)
        blocks.append(block)
    stage1.append(block_index[block])

pairs = sorted((f, c) for f, cs in members.items() if len(cs) > 1 for c in cs)

out = []
out.append("// Made by tools/snippy/regex_unicode.py from CPython %s (Unicode %s): do not edit." % (
    __import__("sys").version.split()[0], __import__("unicodedata").unidata_version))
out.append("// What Python's re takes each character to be; see the tool for the layout.")
out.append("")
out.append("constexpr uint8_t kPyWord = %d, kPyDecimal = %d, kPySpace = %d, kPyAlpha = %d, kPyIdStart = %d, kPyIdContinue = %d;" % (
    WORD, DECIMAL, SPACE, ALPHA, ID_START, ID_CONTINUE))
out.append("struct PyRecord { uint8_t flags; int32_t form, lower; };")


def rows(values, per):
    for k in range(0, len(values), per):
        out.append("    " + ", ".join(str(v) for v in values[k:k + per]) + ",")


out.append("const uint16_t kPyBlock[%d] = {" % len(stage1))
rows(stage1, 24)
out.append("};")
flat = [e for block in blocks for e in block]
out.append("const uint16_t kPyEntry[%d] = {" % len(flat))
rows(flat, 32)
out.append("};")
out.append("const PyRecord kPyRecord[%d] = {" % len(records))
out.append("    " + ", ".join("{%d, %d, %d}" % r for r in records) + ",")
out.append("};")
out.append("const uint32_t kPyCaseGroup[%d][2] = {" % len(pairs))
for k in range(0, len(pairs), 8):
    out.append("    " + ", ".join("{%d, %d}" % p for p in pairs[k:k + 8]) + ",")
out.append("};")
print("\n".join(out))
