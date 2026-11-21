# random (the Snippy plan, part 5.3, step 3): with a seed, the same numbers as CPython 3.14's,
# its Mersenne Twister and every way it turns numbers into choices.
import random

random.seed(42)
print([random.random() for _ in range(3)])
print(random.getrandbits(1), random.getrandbits(32), random.getrandbits(100), random.randbytes(5))
print([random.randrange(10) for _ in range(8)], random.randrange(5, 50, 5), random.randint(1, 6), random.randrange(-10, 10))
print(random.choice("abcdef"), random.choice([1, 2, 3]), random.choices("abc", k=5), random.choices([1, 2, 3], weights=[5, 1, 1], k=6))
print(random.choices(range(4), cum_weights=[1, 2, 10, 11], k=4))
items = list(range(10))
random.shuffle(items)
print(items, random.sample(range(100), 5), random.sample("abcdefgh", 3), random.sample(range(10**6), 3), random.sample(["x", "y"], 2, counts=[2, 1]))
print(random.uniform(1, 10), random.triangular(0, 10, 3), random.gauss(0, 1), random.gauss(5, 2), random.normalvariate(0, 1))
print(random.expovariate(2), random.betavariate(2, 3), random.gammavariate(2, 1), random.gammavariate(0.5, 1), random.lognormvariate(0, 1))
print(random.vonmisesvariate(0, 1), random.paretovariate(3), random.weibullvariate(1, 2), random.binomialvariate(10, 0.3), random.binomialvariate(100, 0.5))

for seed in [0, 1, -5, 2**70, 3.5, "hello", b"bytes", True]:
    random.seed(seed)
    print(repr(seed), random.random(), random.randrange(1000))

r = random.Random(7)
state = r.getstate()
a = [r.random() for _ in range(3)]
r.setstate(state)
print(a == [r.random() for _ in range(3)], r.randint(0, 100), random.Random(7).random() == random.Random(7).random(), type(r).__name__)

class Rigged(random.Random):
    def random(self):
        return 0.5
print(Rigged(1).random(), Rigged(1).choice("abc"), isinstance(Rigged(), random.Random))

for f in [lambda: random.randrange(0), lambda: random.randrange(1, 1), lambda: random.choice([]), lambda: random.sample([1, 2], 3),
          lambda: random.randint(5, 1), lambda: random.getrandbits(-1), lambda: random.choices([1], weights=[1, 2])]:
    try:
        print(f())
    except Exception as e:
        print(type(e).__name__, e)
print(random.SystemRandom().random() < 1.0, 0 <= random.SystemRandom().randrange(10) < 10)
