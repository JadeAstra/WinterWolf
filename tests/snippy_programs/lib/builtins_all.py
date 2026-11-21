# Python's built-in functions, each with its forms (the Snippy plan, part 5.3), and the
# builtins module, as CPython 3.14 gives them.
import builtins

print(builtins.len("abc"), builtins.print is print, type(builtins).__name__, builtins.__name__, "open" in dir(builtins), hasattr(builtins, "zip"))
builtins.my_global_value = 41
print(my_global_value + 1)

print(abs(-3), all([]), all([1, 0]), any([]), any([0, 2]), ascii("é\n"), bin(10), bool(3), callable(len), callable(3), chr(97), ord("a"))
print(divmod(17, 5), enumerate.__name__, list(enumerate("ab", 1)), list(filter(None, [0, 1, "", "x"])), list(filter(lambda v: v > 1, [1, 2, 3])))
print(format(3.14159, ".2f"), format(42), format("s", ">3"), hex(31), id(None) == id(None), isinstance(1, (str, int)), issubclass(bool, int))
it = iter([1, 2])
print(next(it), next(it), next(it, "done"), len([1, 2]), list(map(str, [1, 2])), list(map(lambda x, y: x + y, [1, 2], [10, 20, 30])))
print(max(1, 3), min("ab", "a"), oct(9), pow(2, 5), repr("x"), list(reversed([1, 2])), round(2.567, 2), sorted([3, 1]), sum([1, 2]))
print(list(zip("ab", [1, 2, 3])), list(zip()), list(zip("a", "b", "c")), hash(1) == 1, type(3), type("x").__name__, vars(type("T", (), {"a": 1}))["a"])
for f in [lambda: list(zip("ab", [1], strict=True)), lambda: next(iter([])), lambda: chr(-1), lambda: ord("ab"), lambda: len(5),
          lambda: iter(5), lambda: abs("x"), lambda: divmod(1, 0), lambda: isinstance(1, 2), lambda: getattr(1, "nope")]:
    try:
        f()
    except Exception as e:
        print(type(e).__name__, e)

class Point:
    """A point."""
    def __init__(self, x, y):
        self.x = x
        self.y = y
    def __repr__(self):
        return f"Point({self.x}, {self.y})"

p = Point(1, 2)
print(getattr(p, "x"), getattr(p, "z", 0), hasattr(p, "y"), setattr(p, "z", 9), p.z, delattr(p, "z"), hasattr(p, "z"), vars(p), sorted(vars(p)))
print([n for n in dir(p) if not n.startswith("__")], "x" in dir(p), Point.__doc__, Point.__name__, Point.__qualname__, type(p) is Point)
print(isinstance(p, object), issubclass(Point, object), callable(Point), callable(p), repr(p), str(p), format(p, ""), id(p) == id(p))
it = iter(lambda: next(gen), None)
gen = iter([1, 2, None, 4])
print(list(it))
print(list(iter([3, 4].__iter__())), next(reversed("abc")), list(range(3)).__len__(), type(iter("")).__name__)

# globals, locals, and what a function sees.
counter = 5
def look():
    local_value = 1
    return sorted(locals().keys()), "counter" in globals()
print(look(), globals()["counter"])

# input, from what standard input gives (nothing: the end of the file).
try:
    input()
except EOFError as e:
    print("EOFError", e)

# print's keywords.
print("a", "b", sep="-", end="!\n")
print("no newline", end="")
print()
print(*[1, 2, 3], sep=", ")
import sys
print("to stdout", file=sys.stdout, flush=True)

# classmethod, staticmethod, property, super, object, type with three arguments.
class Base:
    count = 0
    @classmethod
    def make(cls):
        cls.count += 1
        return cls()
    @staticmethod
    def twice(v):
        return v * 2
    @property
    def name(self):
        return type(self).__name__
class Child(Base):
    def __init__(self):
        super().__init__()
print(Child.make().name, Base.twice(4), Child.count, Base.count, type("Made", (Base,), {"x": 1})().name, object().__class__.__name__)
print(sorted(set(dir(object)) - {"__getstate__"})[:8])
print(slice(1, 2), frozenset("aa"), memoryview(b"a").tolist(), bytearray(2), complex(1, 1), float("1.5"), int("7"), str(8), tuple("q"))
print(aiter.__name__, anext.__name__, breakpoint.__name__, __import__("math").__name__ if False else __import__.__name__, compile.__name__,
      eval.__name__, exec.__name__, open.__name__, help.__class__.__name__ if False else "help", copyright.__class__.__name__)
