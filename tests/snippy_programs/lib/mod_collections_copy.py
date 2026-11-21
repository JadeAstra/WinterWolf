# collections and copy (the Snippy plan, part 5.3, step 2), as CPython 3.14 gives them.
import collections
from collections import Counter, OrderedDict, defaultdict, deque, namedtuple, ChainMap, UserDict, UserList, UserString
import copy

# Counter.
c = Counter("abracadabra")
print(c, c["a"], c["z"], c.most_common(2), sorted(c.elements())[:6], c.total(), len(c), isinstance(c, dict))
c.update("aaz")
c.subtract({"a": 1})
print(c, Counter(a=3, b=1) + Counter(a=1, b=2), Counter(a=3, b=1) - Counter(a=1, b=2), Counter(a=3) & Counter(a=1), Counter(a=1) | Counter(a=2, b=1))
print(Counter([1, 1, 2]) == Counter({1: 2, 2: 1}), -Counter(a=-2, b=1), +Counter(a=-2, b=1), Counter().most_common(), repr(Counter()))
d = Counter(x=1)
del d["x"]
print(d, Counter("hello").most_common()[-1], dict(Counter("aab")))

# OrderedDict.
od = OrderedDict([("a", 1), ("b", 2), ("c", 3)])
od.move_to_end("a")
od.move_to_end("c", last=False)
print(od, od.popitem(), od.popitem(last=False), od, list(reversed(OrderedDict(x=1, y=2))), OrderedDict(a=1) == OrderedDict(a=1))

# defaultdict.
dd = defaultdict(list)
dd["x"].append(1)
dd["y"]
counts = defaultdict(int)
for w in "a b a".split():
    counts[w] += 1
print(dd, counts, dd.default_factory, defaultdict(None)["k"] if False else dict(counts), isinstance(dd, dict))
try:
    defaultdict()["missing"]
except KeyError as e:
    print("KeyError", e)

# deque.
q = deque([1, 2, 3], maxlen=5)
q.append(4)
q.appendleft(0)
q.append(5)
q.extendleft([-1, -2])
print(q, q.maxlen, q.popleft(), q.pop(), q, q[0], q[-1], len(q), 2 in q, q.count(2), q.index(2))
q.rotate(2)
print(q)
q.rotate(-1)
q.remove(2)
q.insert(1, 9)
print(q, list(reversed(q)), deque("ab") + deque("c"), deque([1]) * 3, deque() == deque([]), q.copy())
q.clear()
print(q, repr(deque()), deque(range(3), maxlen=2))
for f in [lambda: deque().pop(), lambda: deque([1]).remove(2), lambda: deque([1], maxlen=1).insert(0, 2)]:
    try:
        f()
    except Exception as e:
        print(type(e).__name__, e)

# namedtuple.
Point = namedtuple("Point", ["x", "y"])
pt = Point(1, y=2)
print(pt, pt.x, pt[1], pt._asdict(), pt._replace(x=5), Point._fields, Point._make([3, 4]), pt == (1, 2), isinstance(pt, tuple), hash(pt) == hash((1, 2)))
P3 = namedtuple("P3", "a b c", defaults=[0, 9])
print(P3(1), P3._field_defaults, P3.__name__, repr(P3(1, 2, 3)), list(P3(7)), P3(1, 2, 3)[1:])
x, y = pt
print(x, y, Point(x=1, y=2).y)
for f in [lambda: Point(1), lambda: Point(1, 2, 3), lambda: namedtuple("Bad", ["x", "x"]), lambda: namedtuple("Bad", ["1a"])]:
    try:
        f()
    except Exception as e:
        print(type(e).__name__, e)
Renamed = namedtuple("Renamed", ["ok", "def", "ok"], rename=True)
print(Renamed._fields)

# ChainMap, UserDict, UserList, UserString.
cm = ChainMap({"a": 1}, {"a": 2, "b": 3})
print(cm["a"], cm["b"], len(cm), sorted(cm), cm.maps, cm.new_child({"z": 0})["z"], dict(cm.parents))
cm["c"] = 4
print(cm.maps[0], "c" in cm)
class Upper(UserDict):
    def __setitem__(self, key, value):
        super().__setitem__(key.upper(), value)
u = Upper()
u["a"] = 1
print(u, u.data, len(u), UserList([1, 2]) + [3], UserString("ab").upper(), UserString("x") + "y")

# copy.
orig = {"list": [1, [2, 3]], "n": 1}
shallow = copy.copy(orig)
deep = copy.deepcopy(orig)
orig["list"][1].append(4)
print(shallow["list"], deep["list"], shallow is orig, deep["list"] is orig["list"])
class Node:
    def __init__(self, v):
        self.v = v
        self.next = self
n = Node(5)
n2 = copy.deepcopy(n)
print(n2.v, n2.next is n2, n2 is n)
class Custom:
    def __init__(self):
        self.calls = 0
    def __copy__(self):
        r = Custom()
        r.calls = self.calls + 1
        return r
    def __deepcopy__(self, memo):
        r = Custom()
        r.calls = self.calls + 10
        return r
print(copy.copy(Custom()).calls, copy.deepcopy(Custom()).calls, copy.copy((1, 2)), copy.deepcopy([1, (2, [3])]), copy.replace(Point(1, 2), y=5))
