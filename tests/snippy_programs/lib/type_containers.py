# list, tuple, dict, set, frozenset, range and slice: every method and their corners
# (the Snippy plan, part 5.3), as CPython 3.14 gives them.

# list.
a = [3, 1, 2]
a.append(5)
a.extend((7, 8))
a.extend("xy")
a.insert(0, 0)
a.insert(-1, "m")
a.insert(100, "end")
print(a, a.pop(), a.pop(0), a.pop(-2), a.index(2), a.index("x", 3), a.count(7), len(a))
a.remove(7)
a.reverse()
print(a)
b = [5, 3, 9, 1]
b.sort()
print(b, sorted(b, reverse=True), sorted(["b", "A", "c"], key=str.lower), sorted([(2, "a"), (1, "b"), (2, "A")]))
c = b.copy()
c.clear()
print(b, c, b * 2, [0] * 3, b + [10], b[1:3], b[::-1], b[::2], b[-2:], [] == [], [1, [2]] == [1, [2]], [1, 2] < [1, 3], [1, 2] < [1, 2, 0])
b[1:3] = ["a", "b", "c"]
b[::2] = [0, 0, 0]
del b[0]
del b[::2]
print(b)
l = list(range(10))
del l[2:8:3]
l[1:1] = [99]
print(l, l[100:], list("abc"), list({"k": 1}), list((1, 2)), [x * x for x in range(4)], [1, 2, 3][::-2])
for f in [lambda: [].pop(), lambda: [1].pop(5), lambda: [1].remove(2), lambda: [1].index(2), lambda: [1][3], lambda: [1, 2].__setitem__(slice(0, 2, 2), [1, 2]),
          lambda: [3, "a"].sort(), lambda: [1].insert()]:
    try:
        f()
    except Exception as e:
        print(type(e).__name__, e)
n = [[1, 2], [3, 4]]
m = n.copy()
m[0].append(5)
print(n, m, n is m, n[0] is m[0], list.__name__, repr([1, "a", None, 2.5]))
r = [1, 2]
r.append(r)
print(r, len(r))

# tuple.
t = (1, 2, 3, 2)
print(t.count(2), t.index(3), t.index(2, 2), t[1:], t[::-1], t + (4,), t * 2, (1,), (), tuple("ab"), tuple([1, 2]), len(t), 2 in t)
print((1, 2) < (1, 3), (1, 2) == (1, 2), hash((1, 2)) == hash((1, 2)), tuple.__name__, repr((1,)), repr((1, (2, 3))), max((1, 5, 3)))
for f in [lambda: t.index(9), lambda: t[10], lambda: hash(([1],)), lambda: t.__setitem__(0, 1)]:
    try:
        f()
    except Exception as e:
        print(type(e).__name__, e)

# dict.
d = {"a": 1, "b": 2}
d["c"] = 3
d.update({"d": 4}, e=5)
d.update([("f", 6)])
print(d, d.get("a"), d.get("z"), d.get("z", 0), d.setdefault("a", 9), d.setdefault("g", 7), d.pop("g"), d.pop("zz", None), len(d))
print(list(d.keys()), list(d.values()), list(d.items()), d.popitem(), "a" in d, "zz" not in d, list(reversed(d)))
print(dict.fromkeys("abc"), dict.fromkeys([1, 2], 0), dict(a=1, b=2), dict([("x", 1)]), dict({"y": 2}, z=3), dict(zip("ab", [1, 2])))
e = d.copy()
e.clear()
print(e, d | {"z": 0}, {**d, "a": 100}, d == {"a": 1, "b": 2, "c": 3, "d": 4}, {1: "a"} == {1.0: "a"})
d |= {"h": 8}
print(d, d.keys() & {"a", "h", "q"}, d.keys() - {"a"}, d.keys() | {"x"}, d.items() & {("a", 1)}, len(d.values()))
ks = d.keys()
d["new"] = 1
print(ks, "new" in ks, repr({}), repr({1: {2: [3]}}), dict.__name__)
for f in [lambda: d["zz"], lambda: {}.popitem(), lambda: d.pop("zz"), lambda: {[1]: 2}, lambda: dict([1, 2]), lambda: dict([(1, 2, 3)])]:
    try:
        f()
    except Exception as e:
        print(type(e).__name__, e)
counts = {}
for w in "the cat the hat the".split():
    counts[w] = counts.get(w, 0) + 1
print(counts, sorted(counts, key=counts.get, reverse=True), max(counts.items(), key=lambda kv: kv[1]))
try:
    for k in d:
        d["more"] = 1
except RuntimeError as err:
    print("RuntimeError", err)

# set and frozenset.
s = {3, 1, 2}
s.add(4)
s.update([5, 6], (7,))
s.discard(99)
s.remove(7)
print(sorted(s), s.pop() in {1, 2, 3, 4, 5, 6}, len(s), 3 in s)
p, q = {1, 2, 3, 4}, {3, 4, 5}
print(p | q, p & q, p - q, p ^ q, p.union(q, [9]), p.intersection(q, {4}), p.difference(q, {1}), p.symmetric_difference(q))
print(p.issubset({1, 2, 3, 4, 5}), p.issuperset({1}), p.isdisjoint({8}), {1, 2} <= {1, 2}, {1, 2} < {1, 2}, {1} < {1, 2}, p == {4, 3, 2, 1})
c = p.copy()
c.intersection_update(q)
p.difference_update({1})
q.symmetric_difference_update({5, 6})
print(c, p, q)
c.clear()
f = frozenset([1, 2, 2])
print(c, f, f | {3}, type(f | {3}).__name__, type({3} | f).__name__, hash(f) == hash(frozenset([2, 1])), f.copy() is f, frozenset())
print({frozenset({1}): "x"}, set("abc") == set("cba"), set(), repr({1}), set.__name__, frozenset.__name__, sorted({"b", "a"}))
for f2 in [lambda: set().pop(), lambda: {1}.remove(2), lambda: {[1]}, lambda: frozenset().add(1), lambda: {1} < [1]]:
    try:
        f2()
    except Exception as e:
        print(type(e).__name__, e)

# range and slice.
r = range(2, 20, 3)
print(r, r.start, r.stop, r.step, len(r), r[1], r[-1], r[1:3], r[::-1], list(r[::2]), 8 in r, 9 in r, r.index(11), r.count(5))
print(range(5) == range(0, 5), range(0) == range(5, 2), list(range(10, 0, -3)), range(3)[5:], hash(range(3)) == hash(range(0, 3)), reversed(range(3)).__class__.__name__)
print(list(reversed(range(1, 10, 4))), range(-5), range(2**70)[-1], bool(range(0)), repr(range(1, 2, 1)))
for f3 in [lambda: range(1, 2, 0), lambda: range(3).index(9), lambda: range(3)[5], lambda: range(1.5)]:
    try:
        f3()
    except Exception as e:
        print(type(e).__name__, e)
sl = slice(1, 10, 2)
print(sl, sl.start, sl.stop, sl.step, sl.indices(5), slice(None, None, -1).indices(4), slice(5).indices(3), slice(-3, None).indices(10))
print([0, 1, 2, 3, 4, 5][sl], "abcdef"[slice(None, None, -2)], slice(1, 2) == slice(1, 2), repr(slice(None)), slice.__name__)
try:
    slice(0, 5, 0).indices(3)
except ValueError as e:
    print("ValueError", e)
