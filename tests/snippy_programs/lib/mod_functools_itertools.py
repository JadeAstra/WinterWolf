# functools and itertools (the Snippy plan, part 5.3, step 2), as CPython 3.14 gives them.
import functools
import itertools
import operator

print(functools.reduce(operator.add, [1, 2, 3, 4]), functools.reduce(lambda a, b: a * b, [1, 2, 3], 10), functools.reduce(operator.add, [], 0))
try:
    functools.reduce(operator.add, [])
except TypeError as e:
    print("TypeError", e)

p = functools.partial(int, base=2)
print(p("101"), p.func is int, p.args, p.keywords, repr(functools.partial(max, 1)), functools.partial(pow, 2)(5), functools.partial(functools.partial(pow, 2), 3)())

@functools.lru_cache(maxsize=None)
def fib(n):
    return n if n < 2 else fib(n - 1) + fib(n - 2)
print(fib(80), fib.cache_info())
fib.cache_clear()
print(fib.cache_info())

@functools.lru_cache(maxsize=2)
def sq(n):
    return n * n
for v in [1, 2, 1, 3, 2]:
    sq(v)
print(sq.cache_info(), sq.__name__, sq.__wrapped__.__name__)

@functools.cache
def double(n):
    return 2 * n
print(double(4), double(4), double.cache_info().hits)

def deco(f):
    @functools.wraps(f)
    def inner(*a, **k):
        return f(*a, **k)
    return inner
@deco
def documented(x):
    """It doubles."""
    return x * 2
print(documented(3), documented.__name__, documented.__doc__, documented.__wrapped__.__name__)

@functools.total_ordering
class Ver:
    def __init__(self, n):
        self.n = n
    def __eq__(self, o):
        return self.n == o.n
    def __lt__(self, o):
        return self.n < o.n
print(Ver(1) < Ver(2), Ver(2) >= Ver(1), Ver(3) <= Ver(2), Ver(3) > Ver(1))

print(sorted([3, 1, 2], key=functools.cmp_to_key(lambda a, b: b - a)))

@functools.singledispatch
def show(x):
    return f"thing {x!r}"
@show.register
def _(x: int):
    return f"int {x}"
@show.register(list)
def _(x):
    return f"list of {len(x)}"
print(show("s"), show(5), show([1, 2]), show(True))

class Lazy:
    @functools.cached_property
    def value(self):
        print("computing")
        return 42
lz = Lazy()
print(lz.value, lz.value, "value" in lz.__dict__)

# itertools.
print(list(itertools.count(5)[:0]) if False else list(itertools.islice(itertools.count(5, 2), 4)), list(itertools.islice(itertools.cycle("ab"), 5)),
      list(itertools.repeat("x", 3)), list(itertools.islice(itertools.repeat(0), 2)))
print(list(itertools.accumulate([1, 2, 3, 4])), list(itertools.accumulate([1, 2, 3], operator.mul)), list(itertools.accumulate([1, 2], initial=10)),
      list(itertools.chain("ab", [1])), list(itertools.chain.from_iterable([[1], [2, 3]])))
print(list(itertools.compress("abcd", [1, 0, 1, 0])), list(itertools.dropwhile(lambda v: v < 3, [1, 4, 1])), list(itertools.takewhile(lambda v: v < 3, [1, 2, 5, 1])),
      list(itertools.filterfalse(None, [0, 1, "", "x"])))
print([(k, list(g)) for k, g in itertools.groupby("aabbbc")], [(k, len(list(g))) for k, g in itertools.groupby([1, 3, 2, 4], key=lambda v: v % 2)])
print(list(itertools.islice("abcdefg", 2, None, 2)), list(itertools.islice(range(10), 1, 8, 3)), list(itertools.pairwise("abcd")),
      list(itertools.starmap(pow, [(2, 3), (3, 2)])), list(itertools.zip_longest("ab", [1], fillvalue="-")))
a, b = itertools.tee([1, 2, 3])
print(list(a), list(b), list(itertools.batched("abcdefg", 3)), list(itertools.batched(range(4), 2)))
print(list(itertools.product("ab", repeat=2)), list(itertools.product([1, 2], "x")), list(itertools.permutations("abc", 2)),
      len(list(itertools.permutations(range(4)))))
print(list(itertools.combinations("abcd", 2)), list(itertools.combinations_with_replacement("ab", 2)), list(itertools.combinations(range(3), 0)))
for f in [lambda: list(itertools.batched("ab", 0)), lambda: list(itertools.islice("ab", -1)), lambda: list(itertools.batched("abc", 2, strict=True))]:
    try:
        print(f())
    except Exception as e:
        print(type(e).__name__, e)
c = itertools.count()
print(next(c), next(c), repr(c), repr(itertools.repeat(1, 2)), type(itertools.chain()).__name__)

# callable(), isinstance, and a cached method bound to its object.
import collections
print(callable(functools.partial(max)), callable(operator.itemgetter(1)), callable(collections.deque()))
print(isinstance(collections.deque(), collections.deque), type(collections.deque()).__name__)
class C:
    def __init__(self, k):
        self.k = k
    @functools.lru_cache(maxsize=None)
    def f(self, x):
        return x * self.k
    @functools.cache
    def g(self):
        return self.k + 1
c = C(3)
print(c.f(2), c.f(2), c.g(), C.f.cache_info().hits)
