// SHA-2 (FIPS 180-4): SHA-224 and SHA-256, on x86-64's SHA instructions where the processor has
// them; SHA-384, SHA-512 and SHA-512/256. None of it depends on a secret's value but through
// arithmetic, so it is constant-time as written.

#include "crypto.h"

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#define WW_X86 1
#endif

namespace ww {
namespace crypto {

namespace {

alignas(16) const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74,
    0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d,
    0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e,
    0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
    0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

const uint64_t K512[80] = {
    0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL, 0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL,
    0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL, 0xd807aa98a3030242ULL, 0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL, 0xc19bf174cf692694ULL, 0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL,
    0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL, 0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL, 0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL, 0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL,
    0x06ca6351e003826fULL, 0x142929670a0e6e70ULL, 0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
    0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL, 0x92722c851482353bULL, 0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL,
    0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL, 0xd192e819d6ef5218ULL, 0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL, 0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL, 0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL,
    0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL, 0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL, 0xca273eceea26619cULL, 0xd186b8c721c0c207ULL,
    0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL, 0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
    0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL, 0x431d67c49c100d4cULL, 0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL,
    0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL};

void sha256Portable(uint32_t state[8], const uint8_t* data, size_t blocks) {
    uint32_t w[64];
    while (blocks-- > 0) {
        for (int i = 0; i < 16; i++) w[i] = load32be(data + 4 * i);
        for (int i = 16; i < 64; i++) {
            const uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4], f = state[5], g = state[6], h = state[7];
        for (int i = 0; i < 64; i++) {
            const uint32_t t1 = h + (rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
            const uint32_t t2 = (rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            h = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        state[0] += a;
        state[1] += b;
        state[2] += c;
        state[3] += d;
        state[4] += e;
        state[5] += f;
        state[6] += g;
        state[7] += h;
        data += 64;
    }
    wipe(w, sizeof(w));
}

#ifdef WW_X86
// The SHA instructions take the state as ABEF and CDGH and four rounds' message words at a
// time; each group of four after the first four is made from the four before (msg1, msg2).
__attribute__((target("sha,sse4.1,ssse3"))) void sha256Ni(uint32_t state[8], const uint8_t* data, size_t blocks) {
    const __m128i order = _mm_set_epi64x(0x0c0d0e0f08090a0bLL, 0x0405060700010203LL);  // each word big-endian
    __m128i tmp = _mm_loadu_si128(reinterpret_cast<const __m128i*>(&state[0]));
    __m128i state1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(&state[4]));
    tmp = _mm_shuffle_epi32(tmp, 0xB1);                // CDAB
    state1 = _mm_shuffle_epi32(state1, 0x1B);          // EFGH
    __m128i state0 = _mm_alignr_epi8(tmp, state1, 8);  // ABEF
    state1 = _mm_blend_epi16(state1, tmp, 0xF0);       // CDGH
    while (blocks-- > 0) {
        const __m128i abef = state0, cdgh = state1;
        __m128i w[16];
        for (int g = 0; g < 16; g++) {
            if (g < 4) w[g] = _mm_shuffle_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(data + 16 * g)), order);
            else w[g] = _mm_sha256msg2_epu32(_mm_add_epi32(_mm_sha256msg1_epu32(w[g - 4], w[g - 3]), _mm_alignr_epi8(w[g - 1], w[g - 2], 4)), w[g - 1]);
            __m128i m = _mm_add_epi32(w[g], _mm_load_si128(reinterpret_cast<const __m128i*>(&K256[4 * g])));
            state1 = _mm_sha256rnds2_epu32(state1, state0, m);
            m = _mm_shuffle_epi32(m, 0x0E);
            state0 = _mm_sha256rnds2_epu32(state0, state1, m);
        }
        state0 = _mm_add_epi32(state0, abef);
        state1 = _mm_add_epi32(state1, cdgh);
        data += 64;
    }
    tmp = _mm_shuffle_epi32(state0, 0x1B);        // FEBA
    state1 = _mm_shuffle_epi32(state1, 0xB1);     // DCHG
    state0 = _mm_blend_epi16(tmp, state1, 0xF0);  // DCBA
    state1 = _mm_alignr_epi8(state1, tmp, 8);     // HGFE
    _mm_storeu_si128(reinterpret_cast<__m128i*>(&state[0]), state0);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(&state[4]), state1);
}
#endif

void sha256Blocks(uint32_t state[8], const uint8_t* data, size_t blocks) {
#ifdef WW_X86
    if (cpu().sha) return sha256Ni(state, data, blocks);
#endif
    sha256Portable(state, data, blocks);
}

void sha512Blocks(uint64_t state[8], const uint8_t* data, size_t blocks) {
    uint64_t w[80];
    while (blocks-- > 0) {
        for (int i = 0; i < 16; i++) w[i] = load64be(data + 8 * i);
        for (int i = 16; i < 80; i++) {
            const uint64_t s0 = rotr64(w[i - 15], 1) ^ rotr64(w[i - 15], 8) ^ (w[i - 15] >> 7);
            const uint64_t s1 = rotr64(w[i - 2], 19) ^ rotr64(w[i - 2], 61) ^ (w[i - 2] >> 6);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint64_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4], f = state[5], g = state[6], h = state[7];
        for (int i = 0; i < 80; i++) {
            const uint64_t t1 = h + (rotr64(e, 14) ^ rotr64(e, 18) ^ rotr64(e, 41)) + ((e & f) ^ (~e & g)) + K512[i] + w[i];
            const uint64_t t2 = (rotr64(a, 28) ^ rotr64(a, 34) ^ rotr64(a, 39)) + ((a & b) ^ (a & c) ^ (b & c));
            h = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        state[0] += a;
        state[1] += b;
        state[2] += c;
        state[3] += d;
        state[4] += e;
        state[5] += f;
        state[6] += g;
        state[7] += h;
        data += 128;
    }
    wipe(w, sizeof(w));
}

// The bytes not yet a whole block, and the count of all taken in; Block is 64 or 128.
template <size_t Block>
struct Buffered {
    uint8_t buffer[Block];
    size_t held = 0;
    uint64_t total = 0;  // bytes; 2^61 of them is past anything a program will hash

    template <typename Compress>
    void take(const uint8_t* data, size_t size, Compress compress) {
        total += size;
        if (held > 0) {
            const size_t room = Block - held;
            const size_t now = size < room ? size : room;
            std::memcpy(buffer + held, data, now);
            held += now;
            data += now;
            size -= now;
            if (held < Block) return;
            compress(buffer, 1);
            held = 0;
        }
        if (size >= Block) {
            compress(data, size / Block);
            data += size - size % Block;
            size %= Block;
        }
        if (size > 0) {
            std::memcpy(buffer, data, size);
            held = size;
        }
    }
    // The padding: 0x80, zeros, and the length in bits in the last `lengthBytes` (8 or 16).
    template <typename Compress>
    void pad(size_t lengthBytes, Compress compress) {
        const uint64_t bits = total * 8;
        buffer[held++] = 0x80;
        if (held > Block - lengthBytes) {
            std::memset(buffer + held, 0, Block - held);
            compress(buffer, 1);
            held = 0;
        }
        std::memset(buffer + held, 0, Block - held);
        store64be(buffer + Block - 8, bits);
        if (lengthBytes == 16) store64be(buffer + Block - 16, total >> 61);
        compress(buffer, 1);
        held = 0;
    }
};

class Sha256 final : public Hash {
public:
    explicit Sha256(bool is224) : is224_(is224) { reset(); }
    ~Sha256() override {
        wipe(state_, sizeof(state_));
        wipe(&in_, sizeof(in_));
    }
    void update(const uint8_t* data, size_t size) override {
        in_.take(data, size, [this](const uint8_t* p, size_t n) { sha256Blocks(state_, p, n); });
    }
    void finish(uint8_t* out) override {
        in_.pad(8, [this](const uint8_t* p, size_t n) { sha256Blocks(state_, p, n); });
        uint8_t all[32];
        for (int i = 0; i < 8; i++) store32be(all + 4 * i, state_[i]);
        std::memcpy(out, all, size());
        wipe(all, sizeof(all));
        reset();
    }
    void reset() override {
        static const uint32_t start256[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
        static const uint32_t start224[8] = {0xc1059ed8, 0x367cd507, 0x3070dd17, 0xf70e5939, 0xffc00b31, 0x68581511, 0x64f98fa7, 0xbefa4fa4};
        std::memcpy(state_, is224_ ? start224 : start256, sizeof(state_));
        in_.held = 0;
        in_.total = 0;
    }
    std::unique_ptr<Hash> clone() const override { return std::make_unique<Sha256>(*this); }
    size_t size() const override { return is224_ ? 28 : 32; }
    size_t blockSize() const override { return 64; }
    HashKind kind() const override { return is224_ ? HashKind::Sha224 : HashKind::Sha256; }

private:
    bool is224_;
    uint32_t state_[8];
    Buffered<64> in_;
};

class Sha512 final : public Hash {
public:
    explicit Sha512(HashKind kind) : kind_(kind) { reset(); }
    ~Sha512() override {
        wipe(state_, sizeof(state_));
        wipe(&in_, sizeof(in_));
    }
    void update(const uint8_t* data, size_t size) override {
        in_.take(data, size, [this](const uint8_t* p, size_t n) { sha512Blocks(state_, p, n); });
    }
    void finish(uint8_t* out) override {
        in_.pad(16, [this](const uint8_t* p, size_t n) { sha512Blocks(state_, p, n); });
        uint8_t all[64];
        for (int i = 0; i < 8; i++) store64be(all + 8 * i, state_[i]);
        std::memcpy(out, all, size());
        wipe(all, sizeof(all));
        reset();
    }
    void reset() override {
        static const uint64_t start512[8] = {0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL, 0x3c6ef372fe94f82bULL, 0xa54ff53a5f1d36f1ULL,
                                             0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL, 0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL};
        static const uint64_t start384[8] = {0xcbbb9d5dc1059ed8ULL, 0x629a292a367cd507ULL, 0x9159015a3070dd17ULL, 0x152fecd8f70e5939ULL,
                                             0x67332667ffc00b31ULL, 0x8eb44a8768581511ULL, 0xdb0c2e0d64f98fa7ULL, 0x47b5481dbefa4fa4ULL};
        static const uint64_t start512_256[8] = {0x22312194fc2bf72cULL, 0x9f555fa3c84c64c2ULL, 0x2393b86b6f53b151ULL, 0x963877195940eabdULL,
                                                 0x96283ee2a88effe3ULL, 0xbe5e1e2553863992ULL, 0x2b0199fc2c85b8aaULL, 0x0eb72ddc81c52ca2ULL};
        std::memcpy(state_, kind_ == HashKind::Sha384 ? start384 : kind_ == HashKind::Sha512_256 ? start512_256 : start512, sizeof(state_));
        in_.held = 0;
        in_.total = 0;
    }
    std::unique_ptr<Hash> clone() const override { return std::make_unique<Sha512>(*this); }
    size_t size() const override { return kind_ == HashKind::Sha384 ? 48 : kind_ == HashKind::Sha512_256 ? 32 : 64; }
    size_t blockSize() const override { return 128; }
    HashKind kind() const override { return kind_; }

private:
    HashKind kind_;
    uint64_t state_[8];
    Buffered<128> in_;
};

}  // namespace

std::unique_ptr<Hash> newSha3(HashKind kind, size_t shakeSize);  // sha3.cpp

std::unique_ptr<Hash> newHash(HashKind kind, size_t shakeSize) {
    switch (kind) {
        case HashKind::Sha256: return std::make_unique<Sha256>(false);
        case HashKind::Sha224: return std::make_unique<Sha256>(true);
        case HashKind::Sha384:
        case HashKind::Sha512:
        case HashKind::Sha512_256: return std::make_unique<Sha512>(kind);
        case HashKind::Sha3_224:
        case HashKind::Sha3_256:
        case HashKind::Sha3_384:
        case HashKind::Sha3_512:
        case HashKind::Shake128:
        case HashKind::Shake256: return newSha3(kind, shakeSize);
    }
    return nullptr;
}

}  // namespace crypto
}  // namespace ww

namespace ww {
namespace crypto {

// SHA-1 (FIPS 180-4), whole messages only: for what X.509 names with it (key identifiers, the
// fingerprints people quote). Broken for signatures, and used for none here.
void sha1(uint8_t out[20], const uint8_t* data, size_t size) {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    auto block = [&h](const uint8_t* p) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++) w[i] = load32be(p + 4 * i);
        for (int i = 16; i < 80; i++) w[i] = rotl32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            const uint32_t f = i < 20 ? (b & c) | (~b & d) : i < 40 ? b ^ c ^ d : i < 60 ? (b & c) | (b & d) | (c & d) : b ^ c ^ d;
            const uint32_t k = i < 20 ? 0x5A827999 : i < 40 ? 0x6ED9EBA1 : i < 60 ? 0x8F1BBCDC : 0xCA62C1D6;
            const uint32_t t = rotl32(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rotl32(b, 30);
            b = a;
            a = t;
        }
        h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e;
    };
    size_t at = 0;
    for (; at + 64 <= size; at += 64) block(data + at);
    uint8_t last[128] = {};
    const size_t rest = size - at;
    std::memcpy(last, data + at, rest);
    last[rest] = 0x80;
    const size_t total = rest + 9 <= 64 ? 64 : 128;
    store64be(last + total - 8, uint64_t(size) * 8);
    block(last);
    if (total == 128) block(last + 64);
    for (int i = 0; i < 5; i++) store32be(out + 4 * i, h[i]);
}

}  // namespace crypto
}  // namespace ww
