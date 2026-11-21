# json (the Snippy plan, part 5.3, step 4), as CPython 3.14 gives it: dumps and loads with their
# options, the encoder and decoder classes, and CPython's words for what is wrong.
import json
import json.decoder
from json import JSONDecodeError, JSONEncoder, JSONDecoder
from collections import OrderedDict

# Writing.
data = {"name": "Snippy", "n": 3, "f": 2.5, "big": 10**30, "neg": -0.0, "list": [1, 2.0, None, True, False], "nested": {"a": [], "b": {}}, "t": (1, 2), "é": "ü\n\t\"\\/"}
print(json.dumps(data))
print(json.dumps(data, ensure_ascii=False))
print(json.dumps(data, indent=2, sort_keys=True))
print(json.dumps([1, [2, {"x": [3]}]], indent="\t"), json.dumps([], indent=2), json.dumps({}, indent=2), json.dumps([[]], indent=0))
print(json.dumps({"a": 1, "b": 2}, separators=(",", ":")), json.dumps([1, 2], separators=(" ; ", "=")), json.dumps({1: "a", 2.5: "b", True: "c", None: "d"}), json.dumps({False: 0}))
print(json.dumps(float("inf")), json.dumps([float("nan"), -float("inf")]), json.dumps(1e100), json.dumps(1.0), json.dumps(0.1 + 0.2), json.dumps("\U0001F600"), json.dumps("\U0001F600", ensure_ascii=False))
print(json.dumps("\x00\x1f\x7f "), json.dumps({"b": 1, "a": 2}, sort_keys=True), json.dumps({(1, 2): 3}, skipkeys=True), json.dumps(OrderedDict([("z", 1), ("y", 2)])), json.dumps(1 == 1), json.dumps(None))
print(json.dumps({"s": {1, 2}}, default=sorted), json.dumps(3 + 0j, default=lambda c: [c.real, c.imag]), json.dumps(True, indent=4), json.dumps("x", indent=4))

class Point:
    def __init__(self, x, y):
        self.x, self.y = x, y

class PointEncoder(JSONEncoder):
    def default(self, o):
        if isinstance(o, Point):
            return {"x": o.x, "y": o.y}
        return super().default(o)

print(json.dumps([Point(1, 2)], cls=PointEncoder), PointEncoder(indent=1).encode({"p": Point(3, 4)}), list(JSONEncoder().iterencode({"a": [1, 2]})), JSONEncoder().item_separator, JSONEncoder(indent=2).item_separator)

# Reading.
print(json.loads('{"a": [1, 2.5, -3e2, 1E+2, 0.0, -0, true, false, null], "b": "x\\u00e9\\ud83d\\ude00\\n\\/", "c": {}}'))
print(json.loads("123"), json.loads(" -12.50 "), json.loads('"\\""'), json.loads("[]"), json.loads("12345678901234567890123"), json.loads("1e400"), json.loads("[NaN, Infinity, -Infinity]"), json.loads('"\\ud800"') == "\ud800")
print(json.loads('{"a": 1, "a": 2}'), json.loads('[1, 2]', object_hook=dict), json.loads('{"a": {"b": 1}}', object_hook=lambda d: sorted(d.items())), json.loads('{"b": 1, "a": 2}', object_pairs_hook=lambda p: p))
print(json.loads("[1.5, 2]", parse_float=lambda s: "F" + s, parse_int=lambda s: "I" + s), json.loads("[NaN]", parse_constant=lambda s: s.lower()), json.loads(b'{"x": 1}'), json.loads('{"x": 1}'.encode("utf-16")), json.loads(bytearray(b"[2]")))
print(JSONDecoder().decode(' [1] '), JSONDecoder().raw_decode('{"a": 1} tail'), JSONDecoder().raw_decode("[1]x", 0), json.loads('"a\tb"', strict=False), json.loads(" \n\t\r 7"))

# What is wrong.
for doc in ["", " ", "[", "[1", "[1,", "[1,]", "[1 2]", "{", '{"a"', '{"a":', '{"a": 1', '{"a": 1,}', '{"a" 1}', "{1: 2}", '{"a": 1 "b": 2}', '"abc', '"a\\x"', '"\\u12"', '"\\ud83dx"',
            '"a\nb"', '"\x00"', "tru", "nul", "[True]", "1 2", "01", "-", "1.", "1e", ".5", "+1", "﻿1", "[1]]", '{"a": [1, {"b": }]}', "[\n1,\n2,\n]"]:
    try:
        print(repr(json.loads(doc)))
    except JSONDecodeError as e:
        print(type(e).__name__, e, "|", e.msg, e.pos, e.lineno, e.colno, isinstance(e, ValueError))
for f in [lambda: json.dumps(object()), lambda: json.dumps({(1, 2): 3}), lambda: json.dumps([float("nan")], allow_nan=False), lambda: json.dumps({"b": 1, 2: 3}, sort_keys=True),
          lambda: json.loads(5), lambda: json.dumps([1], indnt=2), lambda: json.loads("[1]", objecthook=1)]:
    try:
        print(f())
    except Exception as e:
        print(type(e).__name__, e)
x = []
x.append(x)
try:
    json.dumps(x)
except ValueError as e:
    print(type(e).__name__, e)
print(json.decoder.JSONDecodeError is JSONDecodeError, JSONDecodeError.__module__, JSONEncoder.__module__, JSONDecoder.__module__, json.__all__, json.decoder.__name__, json.encoder.JSONEncoder is JSONEncoder)
