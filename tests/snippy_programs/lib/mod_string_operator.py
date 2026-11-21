# string and operator (the Snippy plan, part 5.3, step 2), as CPython 3.14 gives them.
import string
import operator

print(string.ascii_letters, string.ascii_lowercase, string.ascii_uppercase, string.digits, string.hexdigits, string.octdigits)
print(repr(string.punctuation), repr(string.whitespace), len(string.printable), string.printable[:20])
print(string.capwords("hello   wide world"), string.capwords("a-b-c", "-"), string.capwords(" x y "))

t = string.Template("$who likes $what, ${who}s and $$ cash")
print(t.substitute(who="tim", what="kung pao"), t.substitute({"who": "ann", "what": "tea"}), t.template)
print(t.safe_substitute(who="bo"), string.Template("$a $b").safe_substitute({"a": 1}), t.get_identifiers(), t.is_valid())
for f in [lambda: t.substitute(who="x"), lambda: string.Template("$").substitute(), lambda: string.Template("${bad").substitute(bad=1)]:
    try:
        print(f())
    except Exception as e:
        print(type(e).__name__, e)
class Dollar(string.Template):
    delimiter = "%"
print(Dollar("%x and %%").substitute(x=5))

fm = string.Formatter()
print(fm.format("{0} and {name}", 1, name="two"), list(fm.parse("a{0:>3}b{x!r}c")), fm.vformat("{} {}", (1, 2), {}))

# operator.
print(operator.add(1, 2), operator.sub(5, 3), operator.mul("ab", 2), operator.truediv(7, 2), operator.floordiv(7, 2), operator.mod(7, 3),
      operator.pow(2, 10), operator.neg(5), operator.pos(-5), operator.abs(-3), operator.invert(5), operator.lshift(1, 4), operator.rshift(16, 2))
print(operator.and_(6, 3), operator.or_(6, 3), operator.xor(6, 3), operator.not_(0), operator.truth([1]), operator.is_(None, None), operator.is_not(1, None),
      operator.is_none(None), operator.is_not_none(0), operator.matmul.__name__)
print(operator.eq(1, 1.0), operator.ne(1, 2), operator.lt(1, 2), operator.le(2, 2), operator.gt(3, 2), operator.ge(1, 2), operator.contains([1, 2], 2),
      operator.countOf([1, 2, 1], 1), operator.indexOf("abc", "c"), operator.concat([1], [2]), operator.index(7), operator.length_hint([1, 2, 3]),
      operator.length_hint(iter([]), 5))
lst = [1, 2, 3]
operator.setitem(lst, 0, 10)
operator.delitem(lst, 1)
print(lst, operator.getitem(lst, 1), operator.getitem("hello", slice(1, 3)))
x = [1]
print(operator.iadd(x, [2]), x, operator.imul([0], 3), operator.iconcat([1], (2,)), operator.ior({1}, {2}))
g1 = operator.itemgetter(1)
g2 = operator.itemgetter(0, 2)
print(g1("abc"), g2("abc"), g1({1: "one"}), repr(g1), sorted([(1, "b"), (2, "a")], key=operator.itemgetter(1)))
class P:
    def __init__(self):
        self.name = "p"
        self.inner = self
    def greet(self, who, punct="!"):
        return f"hi {who}{punct}"
a1 = operator.attrgetter("name")
a2 = operator.attrgetter("inner.inner.name", "name")
m = operator.methodcaller("greet", "bob", punct="?")
print(a1(P()), a2(P()), m(P()), repr(a1), repr(m), operator.methodcaller("upper")("x"))
print(operator.call(len, "abc"), operator.__name__, operator.__add__(1, 1))
for f in [lambda: operator.itemgetter(), lambda: operator.attrgetter(1), lambda: g1(5), lambda: operator.index(1.5)]:
    try:
        print(f())
    except Exception as e:
        print(type(e).__name__, e)
print(sorted(n for n in dir(string) if not n.startswith("_")))
