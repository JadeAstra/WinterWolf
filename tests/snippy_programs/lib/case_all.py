# Case in Snippy is WinterWolf's own (str's methods and re's IGNORECASE), which must give CPython's
# answers: every character that has another case, each way it can be asked, folded into numbers.
import re

M = 1000000007


def fold(h, text):
    for ch in text:
        h = (h * 1000003 + ord(ch) + 1) % M
    return (h * 1000003) % M


cased = [chr(cp) for cp in range(0x110000) if not 0xD800 <= cp <= 0xDFFF and (chr(cp).upper() != chr(cp) or chr(cp).lower() != chr(cp))]
print(len(cased))
everything = "".join(cased)

# str's case methods, and its case tests, on each of them and on words of them.
h = [0] * 9
for c in cased:
    for k, text in enumerate([c.upper(), c.lower(), c.casefold(), c.title(), c.swapcase(), c.capitalize(),
                              "1" if c.isupper() else "0", "1" if c.islower() else "0", "1" if c.istitle() else "0"]):
        h[k] = fold(h[k], text)
print(h)
words = [everything[i:i + 7] for i in range(0, len(everything), 7)]
w = [0] * 4
for word in words:
    for k, text in enumerate([word.title(), word.capitalize(), word.swapcase(), "1" if word.istitle() else "0"]):
        w[k] = fold(w[k], text)
print(w)

# re without case: each as itself, in a class, and the characters of everything it matches.
one = 0
inclass = 0
total = 0
for c in cased:
    found = re.findall("(?i)" + re.escape(c), everything)
    one = fold(one, "".join(found))
    total += len(found)
    inclass = fold(inclass, "".join(re.findall("(?i)[" + re.escape(c) + "]", everything)))
print(total, one, inclass)

# Ranges, and a group taken again (each pair of a character and what it matches).
for pattern in ["[a-z]", "[Ͱ-Ͽ]", "[Ā-ſ]", "[Ḁ-῿]", "[Ⰰ-ⱟ]", "[\u0000-￿]", "[^a-z]", "\\w"]:
    found = re.findall("(?i)" + pattern, everything)
    print(repr(pattern), len(found), fold(0, "".join(found)))
pairs = 0
back = 0
for c in cased:
    for d in {c.upper(), c.lower(), c.casefold(), c.title(), c.swapcase()}:
        if len(d) == 1:
            m = re.fullmatch("(?i)(.)\\1", c + d)
            pairs += 1
            back = fold(back, "1" if m else "0")
print(pairs, back)
print([re.findall("(?i)ı", "Iiıİ"), re.findall("(?i)s", "SsſẞßΣ"), re.findall("(?i)k", "KkK"), re.findall("(?i)[ᲀ]", "вВᲀ"), re.findall("(?i)ﬅ", "ﬅﬆst")])
# gc stress: leave out (every character with another case, each way it can be asked: past the limit under GC stress, and nothing more for the collector than a short program)
