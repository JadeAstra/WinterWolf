# Makes src/regex_unicode.inc: what Python's re (CPython's _sre) takes a character to be, for the
# regex engine's Snippy syntax (Snippy's re, RegexOption.snippy): whether it is a word character,
# a decimal digit or whitespace in Unicode's sense (str.isalnum or "_", str.isdecimal,
# str.isspace), a letter (str.isalpha), or a character a name may start or go on with
# (str.isidentifier). How it compares without case comes from WinterWolf's own case data
# (src/regex.cpp, snippyCases), which gives CPython's.
# From the repository's top, in WSL:
#
#   python3.14 tools/snippy/regex_unicode.py > src/regex_unicode.inc
#
# Two stages: kSnippyBlock[c >> 7] picks a block of 128 entries in kSnippyEntry, each an index into
# kSnippyRecord: { flags }.

WORD, DECIMAL, SPACE, ALPHA, ID_START, ID_CONTINUE = 1, 2, 4, 8, 16, 32

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
    key = (flags,)
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

out = []
out.append("// Made by tools/snippy/regex_unicode.py from CPython %s (Unicode %s): do not edit." % (
    __import__("sys").version.split()[0], __import__("unicodedata").unidata_version))
out.append("// What CPython's re takes each character to be; see the tool for the layout.")
out.append("")
out.append("constexpr uint8_t kSnippyWord = %d, kSnippyDecimal = %d, kSnippySpace = %d, kSnippyAlpha = %d, kSnippyIdStart = %d, kSnippyIdContinue = %d;" % (
    WORD, DECIMAL, SPACE, ALPHA, ID_START, ID_CONTINUE))
out.append("struct SnippyRecord { uint8_t flags; };")


def rows(values, per):
    for k in range(0, len(values), per):
        out.append("    " + ", ".join(str(v) for v in values[k:k + per]) + ",")


out.append("const uint16_t kSnippyBlock[%d] = {" % len(stage1))
rows(stage1, 24)
out.append("};")
flat = [e for block in blocks for e in block]
out.append("const uint16_t kSnippyEntry[%d] = {" % len(flat))
rows(flat, 32)
out.append("};")
out.append("const SnippyRecord kSnippyRecord[%d] = {" % len(records))
out.append("    " + ", ".join("{%d}" % r for r in records) + ",")
out.append("};")
print("\n".join(out))
