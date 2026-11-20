// The NIST prime curves P-256 and P-384 (FIPS 186-5, SEC 2): ECDH and ECDSA, one body for both,
// sized by its limbs (4 or 6 of 64 bits). Numbers mod p and mod n in Montgomery form; points in
// projective coordinates with the complete addition formulas of Renes, Costello and Batina
// (2016, algorithm 4, a = -3), which have no special cases to branch on, so adding a point to
// itself or to infinity is the same work. A scalar times a point is a double and an add for every
// bit, the sum taken or not by mask; the nonce of a signature is RFC 6979's, made from the key and
// the message, so no random number is needed to sign.

#include "crypto.h"

#include <vector>

namespace ww {
namespace crypto {

namespace {

using u128 = unsigned __int128;

template <int L>
struct N {
    uint64_t v[L];  // little-endian limbs
};

template <int L>
N<L> zeroN() {
    N<L> z;
    for (int i = 0; i < L; i++) z.v[i] = 0;
    return z;
}
template <int L>
N<L> smallN(uint64_t x) {
    N<L> z = zeroN<L>();
    z.v[0] = x;
    return z;
}

// A modulus and what Montgomery multiplication needs of it.
template <int L>
struct Modulus {
    N<L> m;
    uint64_t m0inv;  // -m^-1 mod 2^64
    N<L> r2;         // R^2 mod m, R = 2^(64L)
    N<L> one;        // R mod m: 1 in Montgomery form
};

// a + b - (m where the sum is m or more), with no branch.
template <int L>
N<L> modAdd(const N<L>& a, const N<L>& b, const N<L>& m) {
    N<L> s, t;
    u128 c = 0;
    for (int i = 0; i < L; i++) {
        c += u128(a.v[i]) + b.v[i];
        s.v[i] = uint64_t(c);
        c >>= 64;
    }
    const uint64_t carryOut = uint64_t(c);
    uint64_t borrow = 0;
    for (int i = 0; i < L; i++) {
        const u128 d = u128(s.v[i]) - m.v[i] - borrow;
        t.v[i] = uint64_t(d);
        borrow = uint64_t(d >> 64) & 1;
    }
    const uint64_t takeT = 0 - (carryOut | (borrow ^ 1));
    N<L> r;
    for (int i = 0; i < L; i++) r.v[i] = (t.v[i] & takeT) | (s.v[i] & ~takeT);
    return r;
}

template <int L>
N<L> modSub(const N<L>& a, const N<L>& b, const N<L>& m) {
    N<L> d, e;
    uint64_t borrow = 0;
    for (int i = 0; i < L; i++) {
        const u128 x = u128(a.v[i]) - b.v[i] - borrow;
        d.v[i] = uint64_t(x);
        borrow = uint64_t(x >> 64) & 1;
    }
    const uint64_t mask = 0 - borrow;
    u128 c = 0;
    for (int i = 0; i < L; i++) {
        c += u128(d.v[i]) + (m.v[i] & mask);
        e.v[i] = uint64_t(c);
        c >>= 64;
    }
    return e;
}

// a b R^-1 mod m (CIOS), both below m.
template <int L>
N<L> montMul(const N<L>& a, const N<L>& b, const Modulus<L>& md) {
    uint64_t t[L + 2] = {};
    for (int i = 0; i < L; i++) {
        u128 c = 0;
        for (int j = 0; j < L; j++) {
            c += u128(a.v[j]) * b.v[i] + t[j];
            t[j] = uint64_t(c);
            c >>= 64;
        }
        c += t[L];
        t[L] = uint64_t(c);
        t[L + 1] = uint64_t(c >> 64);
        const uint64_t q = t[0] * md.m0inv;
        c = u128(q) * md.m.v[0] + t[0];
        c >>= 64;
        for (int j = 1; j < L; j++) {
            c += u128(q) * md.m.v[j] + t[j];
            t[j - 1] = uint64_t(c);
            c >>= 64;
        }
        c += t[L];
        t[L - 1] = uint64_t(c);
        t[L] = t[L + 1] + uint64_t(c >> 64);
    }
    N<L> r, s;
    for (int i = 0; i < L; i++) r.v[i] = t[i];
    uint64_t borrow = 0;
    for (int i = 0; i < L; i++) {
        const u128 x = u128(r.v[i]) - md.m.v[i] - borrow;
        s.v[i] = uint64_t(x);
        borrow = uint64_t(x >> 64) & 1;
    }
    const uint64_t takeS = 0 - (t[L] | (borrow ^ 1));
    for (int i = 0; i < L; i++) r.v[i] = (s.v[i] & takeS) | (r.v[i] & ~takeS);
    return r;
}

template <int L>
N<L> fromBe(const uint8_t* b) {
    N<L> r;
    for (int i = 0; i < L; i++) r.v[i] = load64be(b + 8 * (L - 1 - i));
    return r;
}
template <int L>
void toBe(uint8_t* b, const N<L>& a) {
    for (int i = 0; i < L; i++) store64be(b + 8 * (L - 1 - i), a.v[i]);
}

// Whether a < b (public values only: it may branch).
template <int L>
bool lessThan(const N<L>& a, const N<L>& b) {
    for (int i = L - 1; i >= 0; i--)
        if (a.v[i] != b.v[i]) return a.v[i] < b.v[i];
    return false;
}
template <int L>
bool isZeroN(const N<L>& a) {
    uint64_t any = 0;
    for (int i = 0; i < L; i++) any |= a.v[i];
    return any == 0;
}
template <int L>
bool equalN(const N<L>& a, const N<L>& b) {
    for (int i = 0; i < L; i++)
        if (a.v[i] != b.v[i]) return false;
    return true;
}

template <int L>
N<L> fromHex(const char* hex) {
    uint8_t b[8 * L];
    for (int i = 0; i < 8 * L; i++) {
        auto digit = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
        b[i] = uint8_t(digit(hex[2 * i]) * 16 + digit(hex[2 * i + 1]));
    }
    return fromBe<L>(b);
}

template <int L>
Modulus<L> makeModulus(const char* hex) {
    Modulus<L> md;
    md.m = fromHex<L>(hex);
    uint64_t inv = 1;
    for (int i = 0; i < 6; i++) inv *= 2 - md.m.v[0] * inv;
    md.m0inv = 0 - inv;
    N<L> x = smallN<L>(1);
    for (int i = 0; i < 128 * L; i++) {
        x = modAdd(x, x, md.m);
        if (i == 64 * L - 1) md.one = x;
    }
    md.r2 = x;
    return md;
}

template <int L>
N<L> toMont(const N<L>& a, const Modulus<L>& md) {
    return montMul(a, md.r2, md);
}
template <int L>
N<L> fromMont(const N<L>& a, const Modulus<L>& md) {
    return montMul(a, smallN<L>(1), md);
}

// a^e for a public exponent e, in Montgomery form.
template <int L>
N<L> montPow(const N<L>& a, const N<L>& e, const Modulus<L>& md) {
    N<L> r = md.one;
    for (int i = 64 * L - 1; i >= 0; i--) {
        r = montMul(r, r, md);
        if ((e.v[i / 64] >> (i % 64)) & 1) r = montMul(r, a, md);
    }
    return r;
}

// 1/a mod a prime m, as a^(m - 2); the exponent is public, so the work is the same for every a.
template <int L>
N<L> montInvert(const N<L>& a, const Modulus<L>& md) {
    N<L> e = md.m;
    e.v[0] -= 2;  // m is odd and far above 2
    return montPow(a, e, md);
}

template <int L>
struct Curve {
    Modulus<L> p, n;
    N<L> b, gx, gy;  // Montgomery form, mod p
    N<L> sqrtExp;    // (p + 1) / 4
};

template <int L>
Curve<L> makeCurve(const char* p, const char* n, const char* b, const char* gx, const char* gy) {
    Curve<L> c;
    c.p = makeModulus<L>(p);
    c.n = makeModulus<L>(n);
    c.b = toMont(fromHex<L>(b), c.p);
    c.gx = toMont(fromHex<L>(gx), c.p);
    c.gy = toMont(fromHex<L>(gy), c.p);
    N<L> p1 = c.p.m;
    u128 carryOver = 1;
    for (int i = 0; i < L; i++) {
        carryOver += p1.v[i];
        p1.v[i] = uint64_t(carryOver);
        carryOver >>= 64;
    }
    for (int i = 0; i < L; i++) c.sqrtExp.v[i] = (p1.v[i] >> 2) | (i < L - 1 ? p1.v[i + 1] << 62 : 0);
    return c;
}

template <int L>
const Curve<L>& curve();

template <>
const Curve<4>& curve<4>() {
    static const Curve<4> made = makeCurve<4>("ffffffff00000001000000000000000000000000ffffffffffffffffffffffff",
                                              "ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551",
                                              "5ac635d8aa3a93e7b3ebbd55769886bc651d06b0cc53b0f63bce3c3e27d2604b",
                                              "6b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296",
                                              "4fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5");
    return made;
}

template <>
const Curve<6>& curve<6>() {
    static const Curve<6> made = makeCurve<6>(
        "fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffeffffffff0000000000000000ffffffff",
        "ffffffffffffffffffffffffffffffffffffffffffffffffc7634d81f4372ddf581a0db248b0a77aecec196accc52973",
        "b3312fa7e23ee7e4988e056be3f82d19181d9c6efe8141120314088f5013875ac656398d8a2ed19d2a85c8edd3ec2aef",
        "aa87ca22be8b05378eb1c71ef320ad746e1d3b628ba79b9859f741e082542a385502f25dbf55296c3a545e3872760ab7",
        "3617de4a96262c6f5d9e98bf9292dc29f8f41dbd289a147ce9da3113b5f0b8c00a60b1ce1d7e819d7a431d7c90ea0e5f");
    return made;
}

// A point (X : Y : Z), Montgomery form; Z = 0 is infinity.
template <int L>
struct Pt {
    N<L> x, y, z;
};

// Algorithm 4 of Renes, Costello and Batina: complete addition on y^2 = x^3 - 3x + b.
template <int L>
Pt<L> ptAdd(const Pt<L>& P, const Pt<L>& Q) {
    const Curve<L>& c = curve<L>();
    const Modulus<L>& md = c.p;
    const N<L>& m = md.m;
    auto mul = [&](const N<L>& a, const N<L>& b) { return montMul(a, b, md); };
    auto add = [&](const N<L>& a, const N<L>& b) { return modAdd(a, b, m); };
    auto sub = [&](const N<L>& a, const N<L>& b) { return modSub(a, b, m); };
    N<L> t0 = mul(P.x, Q.x), t1 = mul(P.y, Q.y), t2 = mul(P.z, Q.z);
    N<L> t3 = add(P.x, P.y), t4 = add(Q.x, Q.y);
    t3 = mul(t3, t4);
    t4 = add(t0, t1);
    t3 = sub(t3, t4);
    t4 = add(P.y, P.z);
    N<L> x3 = add(Q.y, Q.z);
    t4 = mul(t4, x3);
    x3 = add(t1, t2);
    t4 = sub(t4, x3);
    x3 = add(P.x, P.z);
    N<L> y3 = add(Q.x, Q.z);
    x3 = mul(x3, y3);
    y3 = add(t0, t2);
    y3 = sub(x3, y3);
    N<L> z3 = mul(c.b, t2);
    x3 = sub(y3, z3);
    z3 = add(x3, x3);
    x3 = add(x3, z3);
    z3 = sub(t1, x3);
    x3 = add(t1, x3);
    y3 = mul(c.b, y3);
    t1 = add(t2, t2);
    t2 = add(t1, t2);
    y3 = sub(y3, t2);
    y3 = sub(y3, t0);
    t1 = add(y3, y3);
    y3 = add(t1, y3);
    t1 = add(t0, t0);
    t0 = add(t1, t0);
    t0 = sub(t0, t2);
    t1 = mul(t4, y3);
    t2 = mul(t0, y3);
    y3 = mul(x3, z3);
    y3 = add(y3, t2);
    x3 = mul(t3, x3);
    x3 = sub(x3, t1);
    z3 = mul(t4, z3);
    t1 = mul(t3, t0);
    z3 = add(z3, t1);
    return Pt<L>{x3, y3, z3};
}

template <int L>
Pt<L> infinity() {
    return Pt<L>{zeroN<L>(), curve<L>().p.one, zeroN<L>()};
}

template <int L>
void select(Pt<L>& r, const Pt<L>& a, uint64_t take) {
    const uint64_t mask = 0 - take;
    for (int i = 0; i < L; i++) {
        r.x.v[i] ^= mask & (r.x.v[i] ^ a.x.v[i]);
        r.y.v[i] ^= mask & (r.y.v[i] ^ a.y.v[i]);
        r.z.v[i] ^= mask & (r.z.v[i] ^ a.z.v[i]);
    }
}

// [k]P for a scalar of 8L bytes, big-endian.
template <int L>
Pt<L> scalarMul(const uint8_t* k, const Pt<L>& p) {
    Pt<L> r = infinity<L>();
    for (int i = 0; i < 64 * L; i++) {
        r = ptAdd(r, r);
        const Pt<L> sum = ptAdd(r, p);
        select(r, sum, (k[i / 8] >> (7 - i % 8)) & 1);
    }
    return r;
}

template <int L>
Pt<L> generator() {
    return Pt<L>{curve<L>().gx, curve<L>().gy, curve<L>().p.one};
}

// The affine x and y, normal form; false for infinity.
template <int L>
bool affine(const Pt<L>& p, N<L>& x, N<L>& y) {
    const Modulus<L>& md = curve<L>().p;
    if (isZeroN(fromMont(p.z, md))) return false;
    const N<L> zi = montInvert(p.z, md);
    x = fromMont(montMul(p.x, zi, md), md);
    y = fromMont(montMul(p.y, zi, md), md);
    return true;
}

// y^2 - x^3 + 3x for x and y in Montgomery form, as it must equal b.
template <int L>
N<L> rhs(const N<L>& x) {
    const Curve<L>& c = curve<L>();
    const Modulus<L>& md = c.p;
    N<L> r = montMul(montMul(x, x, md), x, md);
    const N<L> x3 = modAdd(modAdd(x, x, md.m), x, md.m);
    return modAdd(modSub(r, x3, md.m), c.b, md.m);
}

template <int L>
bool onCurve(const N<L>& x, const N<L>& y) {
    return equalN(montMul(y, y, curve<L>().p), rhs(x));
}

// ---- The operations, for either curve ----

constexpr size_t kBytes(int L) { return size_t(8 * L); }

template <int L>
bool point(const uint8_t* bytes, size_t size, uint8_t* out) {
    const Curve<L>& c = curve<L>();
    const Modulus<L>& md = c.p;
    const size_t b = kBytes(L);
    if (size == 2 * b + 1 && bytes[0] == 4) {
        const N<L> x = fromBe<L>(bytes + 1), y = fromBe<L>(bytes + 1 + b);
        if (!lessThan(x, md.m) || !lessThan(y, md.m)) return false;
        if (!onCurve(toMont(x, md), toMont(y, md))) return false;
        std::memcpy(out, bytes, 2 * b + 1);
        return true;
    }
    if (size == b + 1 && (bytes[0] == 2 || bytes[0] == 3)) {
        const N<L> x = fromBe<L>(bytes + 1);
        if (!lessThan(x, md.m)) return false;
        const N<L> xm = toMont(x, md);
        const N<L> ym = montPow(rhs(xm), c.sqrtExp, md);
        if (!onCurve(xm, ym)) return false;  // no square root: x is on no point
        N<L> y = fromMont(ym, md);
        if (int(y.v[0] & 1) != (bytes[0] & 1)) y = modSub(zeroN<L>(), y, md.m);
        out[0] = 4;
        toBe(out + 1, x);
        toBe(out + 1 + b, y);
        return true;
    }
    return false;
}

template <int L>
bool validPrivate(const uint8_t* d) {
    const N<L> k = fromBe<L>(d);
    return !isZeroN(k) && lessThan(k, curve<L>().n.m);
}

template <int L>
void publicOf(uint8_t* out, const uint8_t* d) {
    N<L> x, y;
    affine(scalarMul<L>(d, generator<L>()), x, y);
    out[0] = 4;
    toBe(out + 1, x);
    toBe(out + 1 + kBytes(L), y);
}

template <int L>
bool ecdh(uint8_t* out, const uint8_t* d, const uint8_t* q65) {
    const Modulus<L>& md = curve<L>().p;
    const Pt<L> q{toMont(fromBe<L>(q65 + 1), md), toMont(fromBe<L>(q65 + 1 + kBytes(L)), md), md.one};
    N<L> x, y;
    if (!affine(scalarMul<L>(d, q), x, y)) return false;
    toBe(out, x);
    return true;
}

// bits2int of RFC 6979: the hash's first 8L bytes (zeros before a shorter one), reduced below n
// with one subtraction at most. The hash is public: a branch on it tells nothing.
template <int L>
N<L> hashToScalar(const uint8_t* hash, size_t size) {
    uint8_t b[8 * L] = {};
    if (size >= kBytes(L)) std::memcpy(b, hash, kBytes(L));
    else std::memcpy(b + kBytes(L) - size, hash, size);
    const Modulus<L>& n = curve<L>().n;
    const N<L> e = fromBe<L>(b);
    return lessThan(e, n.m) ? e : modSub(e, n.m, n.m);
}

template <int L>
void sign(uint8_t* r, uint8_t* s, const uint8_t* d, const uint8_t* hash, size_t hashSize, const Hash& prototype) {
    const Modulus<L>& n = curve<L>().n;
    const size_t b = kBytes(L);
    const N<L> e = hashToScalar<L>(hash, hashSize);
    uint8_t h1[8 * L];
    toBe(h1, e);  // bits2octets(h1)
    const size_t hlen = prototype.size();
    std::vector<uint8_t> v(hlen, 0x01), k(hlen, 0x00);
    auto hmac = [&](std::initializer_list<std::pair<const uint8_t*, size_t>> parts, std::vector<uint8_t>& out) {
        Hmac mac(prototype, k.data(), k.size());
        for (const auto& part : parts) mac.update(part.first, part.second);
        out.resize(hlen);
        mac.finish(out.data());
    };
    const uint8_t zero = 0, one = 1;
    hmac({{v.data(), hlen}, {&zero, 1}, {d, b}, {h1, b}}, k);
    hmac({{v.data(), hlen}}, v);
    hmac({{v.data(), hlen}, {&one, 1}, {d, b}, {h1, b}}, k);
    hmac({{v.data(), hlen}}, v);
    const N<L> dm = toMont(fromBe<L>(d), n);
    for (;;) {
        std::vector<uint8_t> t;
        while (t.size() < b) {
            hmac({{v.data(), hlen}}, v);
            t.insert(t.end(), v.begin(), v.end());
        }
        uint8_t candidate[8 * L];
        std::memcpy(candidate, t.data(), b);
        const N<L> kn = fromBe<L>(candidate);
        if (!isZeroN(kn) && lessThan(kn, n.m)) {
            N<L> x, y;
            affine(scalarMul<L>(candidate, generator<L>()), x, y);
            const N<L> rn = lessThan(x, n.m) ? x : modSub(x, n.m, n.m);
            if (!isZeroN(rn)) {
                const N<L> km = toMont(kn, n), rm = toMont(rn, n), em = toMont(e, n);
                const N<L> sn = fromMont(montMul(montInvert(km, n), modAdd(em, montMul(rm, dm, n), n.m), n), n);
                if (!isZeroN(sn)) {
                    toBe(r, rn);
                    toBe(s, sn);
                    wipe(candidate, sizeof(candidate));
                    wipe(k.data(), k.size());
                    return;
                }
            }
        }
        hmac({{v.data(), hlen}, {&zero, 1}}, k);
        hmac({{v.data(), hlen}}, v);
    }
}

template <int L>
bool verify(const uint8_t* q, const uint8_t* hash, size_t hashSize, const uint8_t* r8, const uint8_t* s8) {
    const Curve<L>& c = curve<L>();
    const Modulus<L>& n = c.n;
    const N<L> r = fromBe<L>(r8), s = fromBe<L>(s8);
    if (isZeroN(r) || isZeroN(s) || !lessThan(r, n.m) || !lessThan(s, n.m)) return false;
    const N<L> e = hashToScalar<L>(hash, hashSize);
    const N<L> w = montInvert(toMont(s, n), n);
    uint8_t u1[8 * L], u2[8 * L];
    toBe(u1, fromMont(montMul(toMont(e, n), w, n), n));
    toBe(u2, fromMont(montMul(toMont(r, n), w, n), n));
    const Modulus<L>& p = c.p;
    const Pt<L> qp{toMont(fromBe<L>(q + 1), p), toMont(fromBe<L>(q + 1 + kBytes(L)), p), p.one};
    N<L> x, y;
    if (!affine(ptAdd(scalarMul<L>(u1, generator<L>()), scalarMul<L>(u2, qp)), x, y)) return false;
    const N<L> v = lessThan(x, n.m) ? x : modSub(x, n.m, n.m);
    return equalN(v, r);
}

}  // namespace

// ---- What the rest of ww calls ----

bool p256Point(const uint8_t* bytes, size_t size, uint8_t out65[65]) { return point<4>(bytes, size, out65); }
bool p256ValidPrivate(const uint8_t d[32]) { return validPrivate<4>(d); }
void p256Public(uint8_t out65[65], const uint8_t d[32]) { publicOf<4>(out65, d); }
bool p256Ecdh(uint8_t out32[32], const uint8_t d[32], const uint8_t point65[65]) { return ecdh<4>(out32, d, point65); }
void p256Sign(uint8_t r32[32], uint8_t s32[32], const uint8_t d[32], const uint8_t* hash, size_t hashSize, const Hash& prototype) {
    sign<4>(r32, s32, d, hash, hashSize, prototype);
}
bool p256Verify(const uint8_t point65[65], const uint8_t* hash, size_t hashSize, const uint8_t r32[32], const uint8_t s32[32]) {
    return verify<4>(point65, hash, hashSize, r32, s32);
}

bool p384Point(const uint8_t* bytes, size_t size, uint8_t out97[97]) { return point<6>(bytes, size, out97); }
bool p384ValidPrivate(const uint8_t d[48]) { return validPrivate<6>(d); }
void p384Public(uint8_t out97[97], const uint8_t d[48]) { publicOf<6>(out97, d); }
bool p384Ecdh(uint8_t out48[48], const uint8_t d[48], const uint8_t point97[97]) { return ecdh<6>(out48, d, point97); }
void p384Sign(uint8_t r48[48], uint8_t s48[48], const uint8_t d[48], const uint8_t* hash, size_t hashSize, const Hash& prototype) {
    sign<6>(r48, s48, d, hash, hashSize, prototype);
}
bool p384Verify(const uint8_t point97[97], const uint8_t* hash, size_t hashSize, const uint8_t r48[48], const uint8_t s48[48]) {
    return verify<6>(point97, hash, hashSize, r48, s48);
}

}  // namespace crypto
}  // namespace ww
