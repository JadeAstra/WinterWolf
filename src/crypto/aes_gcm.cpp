// AES (FIPS 197) and GCM (NIST SP 800-38D).
//
// AES: one key schedule for both ways of running it; on AES-NI the rounds are the processor's
// instructions, elsewhere portable code whose S-box is worked out (an inverse in GF(2^8) and the
// affine map) for 8 bytes at once with shifts, masks and XORs, never looked up in a table, so no
// cache line it touches depends on the key or the data.
//
// GHASH: a block is a 128-bit number read big-endian, its bits reflected as GCM numbers them; a
// product is four 64x64 carry-less multiplies (PCLMULQDQ, or portable ones from integer multiplies
// on bits spaced apart, as BearSSL's ghash_ctmul64 does), shifted one bit and reduced by
// x^128 + x^7 + x^2 + x + 1.

#include "crypto.h"

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#define WW_X86 1
#endif

namespace ww {
namespace crypto {

namespace {

// ---- The S-box, 8 bytes at once ----

constexpr uint64_t kOnes = 0x0101010101010101ULL;

inline uint64_t xtime8(uint64_t a) { return ((a & 0x7f7f7f7f7f7f7f7fULL) << 1) ^ (((a >> 7) & kOnes) * 0x1b); }

inline uint64_t gfMul8(uint64_t a, uint64_t b) {
    uint64_t r = 0;
    for (int i = 0; i < 8; i++) {
        r ^= a & (((b >> i) & kOnes) * 0xff);
        a = xtime8(a);
    }
    return r;
}

inline uint64_t rotl8(uint64_t x, int n) {
    const uint64_t high = kOnes * ((0xffu << n) & 0xffu);
    const uint64_t low = kOnes * ((1u << n) - 1);
    return ((x << n) & high) | ((x >> (8 - n)) & low);
}

// Each byte b made S(b): b^254 (its inverse, 0 for 0), then the affine map.
uint64_t sbox8(uint64_t x) {
    const uint64_t x2 = gfMul8(x, x);
    const uint64_t x3 = gfMul8(x2, x);
    const uint64_t x6 = gfMul8(x3, x3);
    const uint64_t x12 = gfMul8(x6, x6);
    const uint64_t x15 = gfMul8(x12, x3);
    const uint64_t x30 = gfMul8(x15, x15);
    const uint64_t x60 = gfMul8(x30, x30);
    const uint64_t x120 = gfMul8(x60, x60);
    const uint64_t x240 = gfMul8(x120, x120);
    const uint64_t x252 = gfMul8(x240, x12);
    const uint64_t inv = gfMul8(x252, x2);
    return inv ^ rotl8(inv, 1) ^ rotl8(inv, 2) ^ rotl8(inv, 3) ^ rotl8(inv, 4) ^ (kOnes * 0x63);
}

inline uint8_t xtime(uint8_t b) { return uint8_t((b << 1) ^ (((b >> 7) & 1) * 0x1b)); }

// One block, portable: the state in column order, s[row + 4 * column].
void encryptPortable(const uint8_t* rk, int rounds, const uint8_t in[16], uint8_t out[16]) {
    uint8_t s[16];
    for (int i = 0; i < 16; i++) s[i] = in[i] ^ rk[i];
    for (int round = 1; round <= rounds; round++) {
        store64le(s, sbox8(load64le(s)));
        store64le(s + 8, sbox8(load64le(s + 8)));
        // ShiftRows: row r turns left by r.
        uint8_t t[16];
        for (int c = 0; c < 4; c++)
            for (int r = 0; r < 4; r++) t[r + 4 * c] = s[r + 4 * ((c + r) % 4)];
        if (round != rounds) {
            for (int c = 0; c < 4; c++) {
                uint8_t* col = t + 4 * c;
                const uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
                const uint8_t all = a0 ^ a1 ^ a2 ^ a3;
                col[0] = a0 ^ all ^ xtime(a0 ^ a1);
                col[1] = a1 ^ all ^ xtime(a1 ^ a2);
                col[2] = a2 ^ all ^ xtime(a2 ^ a3);
                col[3] = a3 ^ all ^ xtime(a3 ^ a0);
            }
        }
        for (int i = 0; i < 16; i++) s[i] = t[i] ^ rk[16 * round + i];
        wipe(t, sizeof(t));
    }
    std::memcpy(out, s, 16);
    wipe(s, sizeof(s));
}

#ifdef WW_X86
__attribute__((target("aes,sse2"))) void encryptNi(const uint8_t* rk, int rounds, const uint8_t in[16], uint8_t out[16]) {
    __m128i b = _mm_xor_si128(_mm_loadu_si128(reinterpret_cast<const __m128i*>(in)), _mm_load_si128(reinterpret_cast<const __m128i*>(rk)));
    for (int r = 1; r < rounds; r++) b = _mm_aesenc_si128(b, _mm_load_si128(reinterpret_cast<const __m128i*>(rk + 16 * r)));
    b = _mm_aesenclast_si128(b, _mm_load_si128(reinterpret_cast<const __m128i*>(rk + 16 * rounds)));
    _mm_storeu_si128(reinterpret_cast<__m128i*>(out), b);
}

// Counter mode, four blocks at a time so the AES units overlap.
__attribute__((target("aes,sse2"))) void ctrNi(const uint8_t* rk, int rounds, uint8_t counter[16], const uint8_t* in, uint8_t* out, size_t size) {
    __m128i k[15];
    for (int r = 0; r <= rounds; r++) k[r] = _mm_load_si128(reinterpret_cast<const __m128i*>(rk + 16 * r));
    uint32_t n = load32be(counter + 12);
    alignas(16) uint8_t blocks[64];
    for (int i = 0; i < 4; i++) std::memcpy(blocks + 16 * i, counter, 12);
    while (size > 0) {
        for (int i = 0; i < 4; i++) store32be(blocks + 16 * i + 12, n + uint32_t(i));
        __m128i b0 = _mm_xor_si128(_mm_load_si128(reinterpret_cast<const __m128i*>(blocks)), k[0]);
        __m128i b1 = _mm_xor_si128(_mm_load_si128(reinterpret_cast<const __m128i*>(blocks + 16)), k[0]);
        __m128i b2 = _mm_xor_si128(_mm_load_si128(reinterpret_cast<const __m128i*>(blocks + 32)), k[0]);
        __m128i b3 = _mm_xor_si128(_mm_load_si128(reinterpret_cast<const __m128i*>(blocks + 48)), k[0]);
        for (int r = 1; r < rounds; r++) {
            b0 = _mm_aesenc_si128(b0, k[r]);
            b1 = _mm_aesenc_si128(b1, k[r]);
            b2 = _mm_aesenc_si128(b2, k[r]);
            b3 = _mm_aesenc_si128(b3, k[r]);
        }
        b0 = _mm_aesenclast_si128(b0, k[rounds]);
        b1 = _mm_aesenclast_si128(b1, k[rounds]);
        b2 = _mm_aesenclast_si128(b2, k[rounds]);
        b3 = _mm_aesenclast_si128(b3, k[rounds]);
        if (size >= 64) {
            const __m128i* src = reinterpret_cast<const __m128i*>(in);
            __m128i* dst = reinterpret_cast<__m128i*>(out);
            _mm_storeu_si128(dst, _mm_xor_si128(_mm_loadu_si128(src), b0));
            _mm_storeu_si128(dst + 1, _mm_xor_si128(_mm_loadu_si128(src + 1), b1));
            _mm_storeu_si128(dst + 2, _mm_xor_si128(_mm_loadu_si128(src + 2), b2));
            _mm_storeu_si128(dst + 3, _mm_xor_si128(_mm_loadu_si128(src + 3), b3));
            in += 64;
            out += 64;
            size -= 64;
            n += 4;
            continue;
        }
        alignas(16) uint8_t stream[64];
        _mm_store_si128(reinterpret_cast<__m128i*>(stream), b0);
        _mm_store_si128(reinterpret_cast<__m128i*>(stream + 16), b1);
        _mm_store_si128(reinterpret_cast<__m128i*>(stream + 32), b2);
        _mm_store_si128(reinterpret_cast<__m128i*>(stream + 48), b3);
        for (size_t i = 0; i < size; i++) out[i] = in[i] ^ stream[i];
        n += uint32_t((size + 15) / 16);
        wipe(stream, sizeof(stream));
        size = 0;
    }
    store32be(counter + 12, n);
}
#endif

// ---- GHASH ----

struct U128 {
    uint64_t hi, lo;
};

// The low 64 bits of the carry-less product: integer products of bits 4 apart, whose carries
// fall in the gaps and are masked away.
inline uint64_t bmul64(uint64_t x, uint64_t y) {
    const uint64_t m0 = 0x1111111111111111ULL, m1 = 0x2222222222222222ULL, m2 = 0x4444444444444444ULL, m3 = 0x8888888888888888ULL;
    const uint64_t x0 = x & m0, x1 = x & m1, x2 = x & m2, x3 = x & m3;
    const uint64_t y0 = y & m0, y1 = y & m1, y2 = y & m2, y3 = y & m3;
    uint64_t z0 = (x0 * y0) ^ (x1 * y3) ^ (x2 * y2) ^ (x3 * y1);
    uint64_t z1 = (x0 * y1) ^ (x1 * y0) ^ (x2 * y3) ^ (x3 * y2);
    uint64_t z2 = (x0 * y2) ^ (x1 * y1) ^ (x2 * y0) ^ (x3 * y3);
    uint64_t z3 = (x0 * y3) ^ (x1 * y2) ^ (x2 * y1) ^ (x3 * y0);
    return (z0 & m0) | (z1 & m1) | (z2 & m2) | (z3 & m3);
}

inline uint64_t rev64(uint64_t x) {
    x = ((x & 0x5555555555555555ULL) << 1) | ((x >> 1) & 0x5555555555555555ULL);
    x = ((x & 0x3333333333333333ULL) << 2) | ((x >> 2) & 0x3333333333333333ULL);
    x = ((x & 0x0F0F0F0F0F0F0F0FULL) << 4) | ((x >> 4) & 0x0F0F0F0F0F0F0F0FULL);
    x = ((x & 0x00FF00FF00FF00FFULL) << 8) | ((x >> 8) & 0x00FF00FF00FF00FFULL);
    x = ((x & 0x0000FFFF0000FFFFULL) << 16) | ((x >> 16) & 0x0000FFFF0000FFFFULL);
    return (x << 32) | (x >> 32);
}

inline void clmul64(uint64_t x, uint64_t y, uint64_t& hi, uint64_t& lo) {
    lo = bmul64(x, y);
    hi = rev64(bmul64(rev64(x), rev64(y))) >> 1;
}

// The 256-bit product's four words, highest first, made one bit longer and reduced.
inline U128 reduce(uint64_t p3, uint64_t p2, uint64_t p1, uint64_t p0) {
    p3 = (p3 << 1) | (p2 >> 63);
    p2 = (p2 << 1) | (p1 >> 63);
    p1 = (p1 << 1) | (p0 >> 63);
    p0 <<= 1;
    // The low half's terms of x^128 and above fold onto the high half; what that shifts past
    // its end folds once more.
    const uint64_t th = p1 ^ (p0 << 63) ^ (p0 << 62) ^ (p0 << 57), tl = p0;
    U128 r;
    r.hi = p3 ^ th ^ (th >> 1) ^ (th >> 2) ^ (th >> 7);
    r.lo = p2 ^ tl ^ ((tl >> 1) | (th << 63)) ^ ((tl >> 2) | (th << 62)) ^ ((tl >> 7) | (th << 57));
    return r;
}

U128 gfMulPortable(U128 a, U128 b) {
    uint64_t hh, hl, lh, ll, m1h, m1l, m2h, m2l;
    clmul64(a.hi, b.hi, hh, hl);
    clmul64(a.lo, b.lo, lh, ll);
    clmul64(a.hi, b.lo, m1h, m1l);
    clmul64(a.lo, b.hi, m2h, m2l);
    return reduce(hh, hl ^ m1h ^ m2h, lh ^ m1l ^ m2l, ll);
}

#ifdef WW_X86
__attribute__((target("pclmul,sse2"))) U128 gfMulClmul(U128 a, U128 b) {
    const __m128i x = _mm_set_epi64x(static_cast<long long>(a.hi), static_cast<long long>(a.lo));
    const __m128i y = _mm_set_epi64x(static_cast<long long>(b.hi), static_cast<long long>(b.lo));
    const __m128i low = _mm_clmulepi64_si128(x, y, 0x00);
    const __m128i high = _mm_clmulepi64_si128(x, y, 0x11);
    const __m128i mid = _mm_xor_si128(_mm_clmulepi64_si128(x, y, 0x10), _mm_clmulepi64_si128(x, y, 0x01));
    alignas(16) uint64_t l[2], h[2], m[2];
    _mm_store_si128(reinterpret_cast<__m128i*>(l), low);
    _mm_store_si128(reinterpret_cast<__m128i*>(h), high);
    _mm_store_si128(reinterpret_cast<__m128i*>(m), mid);
    return reduce(h[1], h[0] ^ m[1], l[1] ^ m[0], l[0]);
}
#endif

#ifdef WW_X86
// Four blocks at once: Y = (Y + X1)H^4 + X2 H^3 + X3 H^2 + X4 H, the four products added
// unreduced and reduced once. `powers` is H, H^2, H^3, H^4. Gives the blocks taken (a multiple
// of 64 bytes).
__attribute__((target("pclmul,sse2,ssse3"))) size_t ghash4Clmul(U128& y, const U128 powers[4], const uint8_t* data, size_t size) {
    const __m128i swap = _mm_set_epi8(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
    __m128i h[4];
    for (int i = 0; i < 4; i++) h[i] = _mm_set_epi64x(static_cast<long long>(powers[i].hi), static_cast<long long>(powers[i].lo));
    size_t taken = 0;
    while (size - taken >= 64) {
        __m128i low = _mm_setzero_si128(), high = _mm_setzero_si128(), mid = _mm_setzero_si128();
        for (int i = 0; i < 4; i++) {
            __m128i x = _mm_shuffle_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(data + taken + 16 * i)), swap);
            if (i == 0) x = _mm_xor_si128(x, _mm_set_epi64x(static_cast<long long>(y.hi), static_cast<long long>(y.lo)));
            const __m128i k = h[3 - i];
            low = _mm_xor_si128(low, _mm_clmulepi64_si128(x, k, 0x00));
            high = _mm_xor_si128(high, _mm_clmulepi64_si128(x, k, 0x11));
            mid = _mm_xor_si128(mid, _mm_xor_si128(_mm_clmulepi64_si128(x, k, 0x10), _mm_clmulepi64_si128(x, k, 0x01)));
        }
        alignas(16) uint64_t l[2], hh[2], m[2];
        _mm_store_si128(reinterpret_cast<__m128i*>(l), low);
        _mm_store_si128(reinterpret_cast<__m128i*>(hh), high);
        _mm_store_si128(reinterpret_cast<__m128i*>(m), mid);
        y = reduce(hh[1], hh[0] ^ m[1], l[1] ^ m[0], l[0]);
        taken += 64;
    }
    return taken;
}
#endif

class Ghash {
public:
    // `powers`: H, H^2, H^3, H^4, for taking four blocks at once.
    explicit Ghash(const U128 powers[4]) : h_(powers[0]), powers_(powers), clmul_(cpu().clmul) {}
    // Takes `size` bytes, the last block padded with zeros.
    void update(const uint8_t* data, size_t size) {
#ifdef WW_X86
        if (clmul_ && size >= 64) {
            const size_t taken = ghash4Clmul(x_, powers_, data, size);
            data += taken;
            size -= taken;
        }
#endif
        while (size > 0) {
            uint8_t block[16] = {};
            const size_t now = size < 16 ? size : 16;
            std::memcpy(block, data, now);
            x_.hi ^= load64be(block);
            x_.lo ^= load64be(block + 8);
            x_ = mul(x_, h_);
            data += now;
            size -= now;
        }
    }
    void lengths(uint64_t aadBytes, uint64_t textBytes) {
        x_.hi ^= aadBytes * 8;
        x_.lo ^= textBytes * 8;
        x_ = mul(x_, h_);
    }
    void result(uint8_t out[16]) const {
        store64be(out, x_.hi);
        store64be(out + 8, x_.lo);
    }
    ~Ghash() { wipe(&x_, sizeof(x_)); }

private:
    U128 mul(U128 a, U128 b) const {
#ifdef WW_X86
        if (clmul_) return gfMulClmul(a, b);
#endif
        return gfMulPortable(a, b);
    }
    U128 h_;
    const U128* powers_;
    U128 x_{0, 0};
    bool clmul_;
};

// The key schedule (FIPS 197, 5.2), for 16, 24 or 32 bytes of key, as round keys in order.
void expandKey(const uint8_t* key, size_t keySize, uint8_t* schedule, int& rounds) {
    const int nk = static_cast<int>(keySize / 4);
    rounds = nk + 6;
    const int words = 4 * (rounds + 1);
    uint32_t w[60];
    for (int i = 0; i < nk; i++) w[i] = load32be(key + 4 * i);
    uint8_t rcon = 1;
    for (int i = nk; i < words; i++) {
        uint32_t t = w[i - 1];
        const bool rotate = i % nk == 0;
        if (rotate || (nk > 6 && i % nk == 4)) {
            if (rotate) t = rotl32(t, 8);
            t = static_cast<uint32_t>(sbox8(t));  // the four bytes through the S-box, high ones unused
            if (rotate) {
                t ^= uint32_t(rcon) << 24;
                rcon = xtime(rcon);
            }
        }
        w[i] = w[i - nk] ^ t;
    }
    for (int i = 0; i < words; i++) store32be(schedule + 4 * i, w[i]);
    wipe(w, sizeof(w));
}

class AesGcm final : public Aead {
public:
    AesGcm(const uint8_t* key, size_t keySize) : aes_(key, keySize) {
        uint8_t zero[16] = {}, h[16];
        aes_.encrypt(zero, h);
        powers_[0] = U128{load64be(h), load64be(h + 8)};
        for (int i = 1; i < 4; i++) powers_[i] = gfMulPortable(powers_[i - 1], powers_[0]);
        wipe(h, sizeof(h));
    }
    ~AesGcm() override { wipe(powers_, sizeof(powers_)); }

    bool nonceFits(size_t nonceSize) const override { return nonceSize >= 1; }

    void seal(const uint8_t* nonce, size_t nonceSize, const uint8_t* aad, size_t aadSize, const uint8_t* in, size_t size, uint8_t* out,
              uint8_t* tag) override {
        uint8_t j0[16], counter[16];
        start(nonce, nonceSize, j0);
        std::memcpy(counter, j0, 16);
        increment(counter);
        aes_.ctr32(counter, in, out, size);
        Ghash g(powers_);
        g.update(aad, aadSize);
        g.update(out, size);
        g.lengths(aadSize, size);
        finishTag(g, j0, tag);
    }

    bool open(const uint8_t* nonce, size_t nonceSize, const uint8_t* aad, size_t aadSize, const uint8_t* in, size_t size, const uint8_t* tag,
              uint8_t* out) override {
        uint8_t j0[16], counter[16], expected[16];
        start(nonce, nonceSize, j0);
        Ghash g(powers_);
        g.update(aad, aadSize);
        g.update(in, size);
        g.lengths(aadSize, size);
        finishTag(g, j0, expected);
        const bool authentic = equal(expected, tag, 16);
        wipe(expected, sizeof(expected));
        if (!authentic) return false;
        std::memcpy(counter, j0, 16);
        increment(counter);
        aes_.ctr32(counter, in, out, size);
        return true;
    }

private:
    // J0: the 12-byte nonce and a counter of 1, or GHASH of a nonce of another size.
    void start(const uint8_t* nonce, size_t nonceSize, uint8_t j0[16]) const {
        if (nonceSize == 12) {
            std::memcpy(j0, nonce, 12);
            store32be(j0 + 12, 1);
            return;
        }
        Ghash g(powers_);
        g.update(nonce, nonceSize);
        g.lengths(0, nonceSize);
        g.result(j0);
    }
    static void increment(uint8_t block[16]) { store32be(block + 12, load32be(block + 12) + 1); }
    void finishTag(const Ghash& g, const uint8_t j0[16], uint8_t tag[16]) const {
        uint8_t s[16], mask[16];
        g.result(s);
        aes_.encrypt(j0, mask);
        for (int i = 0; i < 16; i++) tag[i] = s[i] ^ mask[i];
        wipe(mask, sizeof(mask));
    }

    Aes aes_;
    U128 powers_[4];  // H, H^2, H^3, H^4
};

}  // namespace

Aes::Aes(const uint8_t* key, size_t keySize) { expandKey(key, keySize, schedule_, rounds_); }
Aes::~Aes() { wipe(schedule_, sizeof(schedule_)); }

void Aes::encrypt(const uint8_t in[16], uint8_t out[16]) const {
#ifdef WW_X86
    if (cpu().aes) return encryptNi(schedule_, rounds_, in, out);
#endif
    encryptPortable(schedule_, rounds_, in, out);
}

void Aes::ctr32(uint8_t counter[16], const uint8_t* in, uint8_t* out, size_t size) const {
#ifdef WW_X86
    if (cpu().aes) return ctrNi(schedule_, rounds_, counter, in, out, size);
#endif
    uint8_t stream[16];
    while (size > 0) {
        encryptPortable(schedule_, rounds_, counter, stream);
        store32be(counter + 12, load32be(counter + 12) + 1);
        const size_t now = size < 16 ? size : 16;
        for (size_t i = 0; i < now; i++) out[i] = in[i] ^ stream[i];
        in += now;
        out += now;
        size -= now;
    }
    wipe(stream, sizeof(stream));
}

namespace {

// The inverse S-box, 8 bytes at once: the inverse affine map, then b^254.
uint64_t invSbox8(uint64_t x) {
    const uint64_t y = rotl8(x, 1) ^ rotl8(x, 3) ^ rotl8(x, 6) ^ (kOnes * 0x05);
    const uint64_t y2 = gfMul8(y, y);
    const uint64_t y3 = gfMul8(y2, y);
    const uint64_t y6 = gfMul8(y3, y3);
    const uint64_t y12 = gfMul8(y6, y6);
    const uint64_t y15 = gfMul8(y12, y3);
    const uint64_t y30 = gfMul8(y15, y15);
    const uint64_t y60 = gfMul8(y30, y30);
    const uint64_t y120 = gfMul8(y60, y60);
    const uint64_t y240 = gfMul8(y120, y120);
    const uint64_t y252 = gfMul8(y240, y12);
    return gfMul8(y252, y2);
}

// The inverse cipher (FIPS 197, 5.3), portable: the state in column order, s[row + 4 * column].
void decryptPortable(const uint8_t* rk, int rounds, const uint8_t in[16], uint8_t out[16]) {
    uint8_t s[16], t[16];
    for (int i = 0; i < 16; i++) s[i] = in[i] ^ rk[16 * rounds + i];
    for (int round = rounds - 1; round >= 0; round--) {
        // InvShiftRows: row r turns right by r.
        for (int c = 0; c < 4; c++)
            for (int r = 0; r < 4; r++) t[r + 4 * ((c + r) % 4)] = s[r + 4 * c];
        store64le(t, invSbox8(load64le(t)));
        store64le(t + 8, invSbox8(load64le(t + 8)));
        for (int i = 0; i < 16; i++) t[i] ^= rk[16 * round + i];
        if (round > 0) {
            // InvMixColumns: 14 11 13 9 around the column.
            for (int c = 0; c < 4; c++) {
                uint8_t* col = t + 4 * c;
                uint8_t m[4][4];
                for (int i = 0; i < 4; i++) {
                    const uint8_t a = col[i], a2 = xtime(a), a4 = xtime(a2), a8 = xtime(a4);
                    m[i][0] = a8 ^ a4 ^ a2;  // 14
                    m[i][1] = a8 ^ a2 ^ a;   // 11
                    m[i][2] = a8 ^ a4 ^ a;   // 13
                    m[i][3] = a8 ^ a;        // 9
                }
                for (int r = 0; r < 4; r++) col[r] = m[r][0] ^ m[(r + 1) % 4][1] ^ m[(r + 2) % 4][2] ^ m[(r + 3) % 4][3];
            }
        }
        std::memcpy(s, t, 16);
    }
    std::memcpy(out, s, 16);
    wipe(s, sizeof(s));
    wipe(t, sizeof(t));
}

#ifdef WW_X86
__attribute__((target("aes,sse2"))) void decryptNi(const uint8_t* rk, int rounds, const uint8_t in[16], uint8_t out[16]) {
    __m128i b = _mm_xor_si128(_mm_loadu_si128(reinterpret_cast<const __m128i*>(in)), _mm_load_si128(reinterpret_cast<const __m128i*>(rk + 16 * rounds)));
    for (int r = rounds - 1; r > 0; r--) b = _mm_aesdec_si128(b, _mm_aesimc_si128(_mm_load_si128(reinterpret_cast<const __m128i*>(rk + 16 * r))));
    b = _mm_aesdeclast_si128(b, _mm_load_si128(reinterpret_cast<const __m128i*>(rk)));
    _mm_storeu_si128(reinterpret_cast<__m128i*>(out), b);
}
#endif

}  // namespace

void Aes::decrypt(const uint8_t in[16], uint8_t out[16]) const {
#ifdef WW_X86
    if (cpu().aes) return decryptNi(schedule_, rounds_, in, out);
#endif
    decryptPortable(schedule_, rounds_, in, out);
}

bool aesCbc(const uint8_t* key, size_t keySize, const uint8_t iv[16], const uint8_t* in, size_t size, bool encrypt, std::vector<uint8_t>& out) {
    if (keySize != 16 && keySize != 24 && keySize != 32) return false;
    const Aes aes(key, keySize);
    uint8_t chain[16], block[16];
    std::memcpy(chain, iv, 16);
    out.clear();
    if (encrypt) {
        const size_t pad = 16 - size % 16;
        std::vector<uint8_t> padded(in, in + size);
        padded.insert(padded.end(), pad, uint8_t(pad));
        out.resize(padded.size());
        for (size_t at = 0; at < padded.size(); at += 16) {
            for (int i = 0; i < 16; i++) block[i] = padded[at + size_t(i)] ^ chain[i];
            aes.encrypt(block, chain);
            std::memcpy(out.data() + at, chain, 16);
        }
        wipe(padded.data(), padded.size());
        return true;
    }
    if (size == 0 || size % 16 != 0) return false;
    out.resize(size);
    for (size_t at = 0; at < size; at += 16) {
        aes.decrypt(in + at, block);
        for (int i = 0; i < 16; i++) out[at + size_t(i)] = block[i] ^ chain[i];
        std::memcpy(chain, in + at, 16);
    }
    const uint8_t pad = out.back();
    bool good = pad >= 1 && pad <= 16;
    for (size_t i = 0; good && i < pad; i++) good = out[size - 1 - i] == pad;
    if (!good) {
        wipe(out.data(), out.size());
        out.clear();
        return false;
    }
    out.resize(size - pad);
    return true;
}

std::unique_ptr<Aead> newAesGcm(const uint8_t* key, size_t keySize) {
    if (keySize != 16 && keySize != 24 && keySize != 32) return nullptr;
    return std::make_unique<AesGcm>(key, keySize);
}

}  // namespace crypto
}  // namespace ww
