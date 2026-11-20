// What `import Crypto` (lib/Crypto) is built on: the built-in __crypto, over the primitives of
// src/crypto. A hash, a MAC or a cipher's key is a resource the program holds; collected, its
// state is wiped. The work that takes long (PBKDF2, Argon2id) runs through VM::blocking on copies
// of its inputs, so the program's other threads go on meanwhile.

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincrypt.h>
#endif

#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "../builtins.h"
#include "../vm.h"
#include "crypto.h"

namespace ww {

namespace {

using namespace crypto;

struct HashBox {
    std::unique_ptr<Hash> hash;
};
struct MacBox {
    std::unique_ptr<Hmac> mac;
};
struct AeadBox {
    std::unique_ptr<Aead> aead;
};

void releaseHash(int64_t id) { delete reinterpret_cast<HashBox*>(id); }
void releaseMac(int64_t id) { delete reinterpret_cast<MacBox*>(id); }
void releaseAead(int64_t id) { delete reinterpret_cast<AeadBox*>(id); }

template <typename Box>
Box& boxOf(VM& vm, const Value& v, const char* kind) {
    if (!v.isObjType(ObjType::Resource) || std::strcmp(static_cast<ObjResource*>(v.as.obj)->kind, kind) != 0)
        throw vm.runtimeError(Thrown::IllegalArgumentException, std::string("not a ") + kind);
    return *reinterpret_cast<Box*>(static_cast<ObjResource*>(v.as.obj)->id);
}

ObjBytes* bytesOf(VM& vm, const Value& v, const char* what) {
    if (!v.isObjType(ObjType::Bytes)) throw vm.runtimeError(Thrown::ClassCastException, std::string(what) + " is Bytes, not " + vm.typeName(v));
    return asBytes(v);
}

// A run of bytes: `count` of them from `offset`, checked to fit.
struct Span {
    uint8_t* data;
    size_t size;
};
Span spanOf(VM& vm, const Value& v, const Value& offsetValue, const Value& countValue, const char* what) {
    ObjBytes* b = bytesOf(vm, v, what);
    const int64_t size = static_cast<int64_t>(b->data.size());
    const int64_t offset = vm.toInt(offsetValue, "offset");
    const int64_t count = vm.toInt(countValue, "count");
    if (offset < 0 || count < 0 || offset > size || count > size - offset)
        throw vm.runtimeError(Thrown::IndexOutOfBoundsException,
                              std::to_string(count) + " bytes from " + std::to_string(offset) + " do not fit in bytes of size " + std::to_string(size));
    return Span{b->data.data() + offset, static_cast<size_t>(count)};
}
// The whole of the bytes, or nothing for null.
Span wholeOf(VM& vm, const Value& v, const char* what) {
    if (v.isNull()) return Span{nullptr, 0};
    ObjBytes* b = bytesOf(vm, v, what);
    return Span{b->data.data(), b->data.size()};
}

Value newBytes(VM& vm, const uint8_t* data, size_t size) {
    ObjBytes* made = vm.alloc<ObjBytes>();
    made->data.assign(data, data + size);
    vm.account(made->data.size());
    return Value::object(made);
}

HashKind kindOf(VM& vm, const Value& v) {
    const int64_t k = vm.toInt(v, "hash");
    if (k < 1 || k > 11) throw vm.runtimeError(Thrown::IllegalArgumentException, "no hash numbered " + std::to_string(k));
    return static_cast<HashKind>(k);
}

std::unique_ptr<Hash> hashFor(VM& vm, const Value& kindValue, const Value& sizeValue) {
    const HashKind kind = kindOf(vm, kindValue);
    const int64_t size = sizeValue.isNull() ? 0 : vm.toInt(sizeValue, "size");
    if ((kind == HashKind::Shake128 || kind == HashKind::Shake256) && size < 1)
        throw vm.runtimeError(Thrown::IllegalArgumentException, "a SHAKE's size is 1 byte or more, not " + std::to_string(size));
    return newHash(kind, static_cast<size_t>(size));
}

// Whether a curve's size names P-384 (true) or P-256 (false); an IllegalArgumentException else.
bool curveOf(VM& vm, const Value& v) {
    const int64_t bits = vm.toInt(v, "curve");
    if (bits != 256 && bits != 384) throw vm.runtimeError(Thrown::IllegalArgumentException, "no curve P-" + std::to_string(bits));
    return bits == 384;
}

// Bytes of exactly `size`: a key or point of a fixed size.
Span exactly(VM& vm, const Value& v, size_t size, const char* what) {
    const Span s = wholeOf(vm, v, what);
    if (s.data == nullptr || s.size != size)
        throw vm.runtimeError(Thrown::IllegalArgumentException, std::string(what) + " is " + std::to_string(size) + " bytes, not " + std::to_string(s.size));
    return s;
}

// Where `count` bytes go: `into` from `at`, checked to fit, and not frozen.
uint8_t* destination(VM& vm, const Value& into, const Value& atValue, size_t count) {
    ObjBytes* b = bytesOf(vm, into, "where it goes");
    vm.guardWrite(b);
    const int64_t at = vm.toInt(atValue, "at");
    const int64_t size = static_cast<int64_t>(b->data.size());
    if (at < 0 || at > size || static_cast<int64_t>(count) > size - at)
        throw vm.runtimeError(Thrown::IndexOutOfBoundsException,
                              std::to_string(count) + " bytes at " + std::to_string(at) + " do not fit in bytes of size " + std::to_string(size));
    return b->data.data() + at;
}

}  // namespace

void registerCrypto(VM& vm) {
    ObjClass* holder = vm.alloc<ObjClass>(vm.intern("crypto"));
    auto def = [&](const char* name, int minArgs, int maxArgs, NativeFn fn) {
        holder->statics.set(vm.intern(name), Value::object(vm.alloc<ObjNative>(std::string("crypto.") + name, minArgs, maxArgs, fn)));
    };

    // ---- Hashes ----

    // hashNew(kind, size): a hash in its starting state; `size` is a SHAKE's digest size.
    def("hashNew", 2, 2, [](VM& vm, int, Value* args) -> Value {
        auto* box = new HashBox{hashFor(vm, args[0], args[1])};
        return Value::object(vm.alloc<ObjResource>("hash", reinterpret_cast<int64_t>(box), &releaseHash));
    });
    // hashUpdate(hash, bytes, offset, count)
    def("hashUpdate", 4, 4, [](VM& vm, int, Value* args) -> Value {
        HashBox& box = boxOf<HashBox>(vm, args[0], "hash");
        const Span s = spanOf(vm, args[1], args[2], args[3], "what is hashed");
        box.hash->update(s.data, s.size);
        return Value::null();
    });
    // hashFinish(hash): the digest as new bytes; the hash starts again.
    def("hashFinish", 1, 1, [](VM& vm, int, Value* args) -> Value {
        HashBox& box = boxOf<HashBox>(vm, args[0], "hash");
        std::vector<uint8_t> out(box.hash->size());
        box.hash->finish(out.data());
        return newBytes(vm, out.data(), out.size());
    });
    // hashFinishInto(hash, into, at): the digest written into bytes.
    def("hashFinishInto", 3, 3, [](VM& vm, int, Value* args) -> Value {
        HashBox& box = boxOf<HashBox>(vm, args[0], "hash");
        uint8_t* to = destination(vm, args[1], args[2], box.hash->size());
        box.hash->finish(to);
        return Value::null();
    });
    // hashSqueeze(hash, count): a SHAKE's next `count` bytes of output.
    def("hashSqueeze", 2, 2, [](VM& vm, int, Value* args) -> Value {
        HashBox& box = boxOf<HashBox>(vm, args[0], "hash");
        const int64_t count = vm.toInt(args[1], "count");
        if (count < 0) throw vm.runtimeError(Thrown::IllegalArgumentException, "a count is 0 or more, not " + std::to_string(count));
        std::vector<uint8_t> out(static_cast<size_t>(count));
        if (!box.hash->squeeze(out.data(), out.size())) throw vm.runtimeError(Thrown::UnsupportedOperationException, "only a SHAKE gives more output");
        return newBytes(vm, out.data(), out.size());
    });
    def("hashCopy", 1, 1, [](VM& vm, int, Value* args) -> Value {
        HashBox& box = boxOf<HashBox>(vm, args[0], "hash");
        auto* copy = new HashBox{box.hash->clone()};
        return Value::object(vm.alloc<ObjResource>("hash", reinterpret_cast<int64_t>(copy), &releaseHash));
    });
    def("hashReset", 1, 1, [](VM& vm, int, Value* args) -> Value {
        boxOf<HashBox>(vm, args[0], "hash").hash->reset();
        return Value::null();
    });
    // hashSize(kind, size), hashBlockSize(kind): a kind's sizes, with no hash made.
    def("hashSize", 2, 2, [](VM& vm, int, Value* args) -> Value { return Value::integer(static_cast<int64_t>(hashFor(vm, args[0], args[1])->size())); });
    def("hashBlockSize", 1, 1, [](VM& vm, int, Value* args) -> Value {
        return Value::integer(static_cast<int64_t>(hashFor(vm, args[0], Value::integer(32))->blockSize()));
    });
    // digest(kind, size, bytes, offset, count): the digest of the bytes, at once.
    def("digest", 5, 5, [](VM& vm, int, Value* args) -> Value {
        std::unique_ptr<Hash> h = hashFor(vm, args[0], args[1]);
        const Span s = spanOf(vm, args[2], args[3], args[4], "what is hashed");
        h->update(s.data, s.size);
        std::vector<uint8_t> out(h->size());
        h->finish(out.data());
        return newBytes(vm, out.data(), out.size());
    });

    // ---- HMAC ----

    // macNew(kind, key): HMAC with the key over a hash of the kind (not a SHAKE).
    def("macNew", 2, 2, [](VM& vm, int, Value* args) -> Value {
        const HashKind kind = kindOf(vm, args[0]);
        if (kind == HashKind::Shake128 || kind == HashKind::Shake256) throw vm.runtimeError(Thrown::IllegalArgumentException, "HMAC is not made over a SHAKE");
        const Span key = wholeOf(vm, args[1], "a key");
        std::unique_ptr<Hash> h = newHash(kind);
        auto* box = new MacBox{std::make_unique<Hmac>(*h, key.data, key.size)};
        return Value::object(vm.alloc<ObjResource>("mac", reinterpret_cast<int64_t>(box), &releaseMac));
    });
    def("macUpdate", 4, 4, [](VM& vm, int, Value* args) -> Value {
        MacBox& box = boxOf<MacBox>(vm, args[0], "mac");
        const Span s = spanOf(vm, args[1], args[2], args[3], "what is authenticated");
        box.mac->update(s.data, s.size);
        return Value::null();
    });
    def("macFinish", 1, 1, [](VM& vm, int, Value* args) -> Value {
        MacBox& box = boxOf<MacBox>(vm, args[0], "mac");
        std::vector<uint8_t> out(box.mac->size());
        box.mac->finish(out.data());
        return newBytes(vm, out.data(), out.size());
    });
    def("macCopy", 1, 1, [](VM& vm, int, Value* args) -> Value {
        MacBox& box = boxOf<MacBox>(vm, args[0], "mac");
        auto* copy = new MacBox{box.mac->clone()};
        return Value::object(vm.alloc<ObjResource>("mac", reinterpret_cast<int64_t>(copy), &releaseMac));
    });
    def("macReset", 1, 1, [](VM& vm, int, Value* args) -> Value {
        boxOf<MacBox>(vm, args[0], "mac").mac->reset();
        return Value::null();
    });

    // ---- Key derivation ----

    // pbkdf2(kind, password, salt, iterations, size)
    def("pbkdf2", 5, 5, [](VM& vm, int, Value* args) -> Value {
        const HashKind kind = kindOf(vm, args[0]);
        if (kind == HashKind::Shake128 || kind == HashKind::Shake256) throw vm.runtimeError(Thrown::IllegalArgumentException, "PBKDF2 is not made over a SHAKE");
        const Span password = wholeOf(vm, args[1], "a password");
        const Span salt = wholeOf(vm, args[2], "a salt");
        const int64_t iterations = vm.toInt(args[3], "iterations");
        const int64_t size = vm.toInt(args[4], "size");
        if (iterations < 1) throw vm.runtimeError(Thrown::IllegalArgumentException, "PBKDF2's iterations are 1 or more, not " + std::to_string(iterations));
        if (size < 1 || size > (int64_t(1) << 32)) throw vm.runtimeError(Thrown::IllegalArgumentException, "PBKDF2 gives 1 byte or more, not " + std::to_string(size));
        struct Work {
            std::vector<uint8_t> password, salt, out;
            ~Work() { wipe(password.data(), password.size()), wipe(out.data(), out.size()); }
        };
        auto work = std::make_shared<Work>();
        work->password.assign(password.data, password.data + password.size);
        work->salt.assign(salt.data, salt.data + salt.size);
        work->out.resize(static_cast<size_t>(size));
        return vm.blocking(
            [work, kind, iterations] {
                std::unique_ptr<Hash> h = newHash(kind);
                pbkdf2(*h, work->password.data(), work->password.size(), work->salt.data(), work->salt.size(), static_cast<uint64_t>(iterations), work->out.data(),
                       work->out.size());
            },
            [work](VM& vm) -> Value { return newBytes(vm, work->out.data(), work->out.size()); });
    });

    // argon2id(password, salt, secret, data, iterations, memoryKiB, lanes, size)
    def("argon2id", 8, 8, [](VM& vm, int, Value* args) -> Value {
        struct Work {
            std::vector<uint8_t> password, salt, secret, data, out;
            uint32_t iterations = 0, memory = 0, lanes = 0;
            std::string why;
            bool done = false;
            ~Work() { wipe(password.data(), password.size()), wipe(secret.data(), secret.size()), wipe(out.data(), out.size()); }
        };
        auto work = std::make_shared<Work>();
        const Span password = wholeOf(vm, args[0], "a password"), salt = wholeOf(vm, args[1], "a salt"), secret = wholeOf(vm, args[2], "a secret"),
                   data = wholeOf(vm, args[3], "associated data");
        work->password.assign(password.data, password.data + password.size);
        work->salt.assign(salt.data, salt.data + salt.size);
        work->secret.assign(secret.data, secret.data + secret.size);
        work->data.assign(data.data, data.data + data.size);
        const int64_t iterations = vm.toInt(args[4], "iterations"), memory = vm.toInt(args[5], "memory"), lanes = vm.toInt(args[6], "parallelism"),
                      size = vm.toInt(args[7], "size");
        auto inRange = [&](int64_t v, int64_t low, int64_t high, const char* what) {
            if (v < low || v > high)
                throw vm.runtimeError(Thrown::IllegalArgumentException,
                                      std::string("Argon2's ") + what + " is from " + std::to_string(low) + " to " + std::to_string(high) + ", not " + std::to_string(v));
        };
        inRange(iterations, 1, 0xFFFFFFFFLL, "iterations");
        inRange(lanes, 1, 0xFFFFFF, "parallelism");
        inRange(memory, 8 * lanes, 0xFFFFFFFFLL, "memory in KiB");
        inRange(size, 4, 1 << 20, "size");
        work->iterations = static_cast<uint32_t>(iterations);
        work->memory = static_cast<uint32_t>(memory);
        work->lanes = static_cast<uint32_t>(lanes);
        work->out.resize(static_cast<size_t>(size));
        return vm.blocking(
            [work] {
                work->done = argon2id(work->password.data(), work->password.size(), work->salt.data(), work->salt.size(), work->secret.data(), work->secret.size(),
                                      work->data.data(), work->data.size(), work->iterations, work->memory, work->lanes, work->out.data(), work->out.size(), work->why);
            },
            [work](VM& vm) -> Value {
                if (!work->done) throw vm.runtimeError(Thrown::IllegalArgumentException, work->why);
                return newBytes(vm, work->out.data(), work->out.size());
            });
    });

    // blake2b(bytes, size, key): BLAKE2b's digest of 1 to 64 bytes, keyed by up to 64 bytes or not.
    def("blake2b", 3, 3, [](VM& vm, int, Value* args) -> Value {
        const Span in = wholeOf(vm, args[0], "what is hashed");
        const int64_t size = vm.toInt(args[1], "size");
        const Span key = wholeOf(vm, args[2], "a key");
        if (size < 1 || size > 64) throw vm.runtimeError(Thrown::IllegalArgumentException, "a BLAKE2b digest is 1 to 64 bytes, not " + std::to_string(size));
        if (key.size > 64) throw vm.runtimeError(Thrown::IllegalArgumentException, "a BLAKE2b key is at most 64 bytes, not " + std::to_string(key.size));
        uint8_t out[64];
        blake2b(out, static_cast<size_t>(size), in.data, in.size, key.data, key.size);
        return newBytes(vm, out, static_cast<size_t>(size));
    });

    // ---- Authenticated ciphers ----

    // aeadNew(kind, key): 1 AES-GCM (a key of 16, 24 or 32 bytes), 2 ChaCha20-Poly1305 (32).
    def("aeadNew", 2, 2, [](VM& vm, int, Value* args) -> Value {
        const int64_t kind = vm.toInt(args[0], "cipher");
        const Span key = wholeOf(vm, args[1], "a key");
        std::unique_ptr<Aead> made = kind == 1 ? newAesGcm(key.data, key.size) : kind == 2 ? newChaCha20Poly1305(key.data, key.size) : nullptr;
        if (made == nullptr)
            throw vm.runtimeError(Thrown::IllegalArgumentException, kind == 1 ? "an AES key is 16, 24 or 32 bytes, not " + std::to_string(key.size)
                                                                              : "a ChaCha20-Poly1305 key is 32 bytes, not " + std::to_string(key.size));
        auto* box = new AeadBox{std::move(made)};
        return Value::object(vm.alloc<ObjResource>("cipher", reinterpret_cast<int64_t>(box), &releaseAead));
    });
    // aeadSeal(cipher, nonce, aad, from, offset, count, into, at): `count` bytes encrypted to
    // `into` at `at`, the 16-byte tag after them; gives count + 16. In place is allowed.
    def("aeadSeal", 8, 8, [](VM& vm, int, Value* args) -> Value {
        AeadBox& box = boxOf<AeadBox>(vm, args[0], "cipher");
        const Span nonce = wholeOf(vm, args[1], "a nonce");
        if (!box.aead->nonceFits(nonce.size)) throw vm.runtimeError(Thrown::IllegalArgumentException, "a nonce of " + std::to_string(nonce.size) + " bytes does not fit this cipher");
        const Span aad = wholeOf(vm, args[2], "associated data");
        const Span in = spanOf(vm, args[3], args[4], args[5], "what is sealed");
        uint8_t* out = destination(vm, args[6], args[7], in.size + Aead::kTag);
        if (out != in.data && out < in.data + in.size && in.data < out + in.size)
            throw vm.runtimeError(Thrown::IllegalArgumentException, "sealed bytes go in the same place as the plain ones, or apart from them, not overlapping");
        box.aead->seal(nonce.data, nonce.size, aad.data, aad.size, in.data, in.size, out, out + in.size);
        return Value::integer(static_cast<int64_t>(in.size + Aead::kTag));
    });
    // aeadOpen(cipher, nonce, aad, from, offset, count, into, at): the `count` sealed bytes (the
    // tag last) checked and decrypted to `into` at `at`; gives count - 16, or -1 where they are
    // not authentic, with nothing written.
    def("aeadOpen", 8, 8, [](VM& vm, int, Value* args) -> Value {
        AeadBox& box = boxOf<AeadBox>(vm, args[0], "cipher");
        const Span nonce = wholeOf(vm, args[1], "a nonce");
        if (!box.aead->nonceFits(nonce.size)) throw vm.runtimeError(Thrown::IllegalArgumentException, "a nonce of " + std::to_string(nonce.size) + " bytes does not fit this cipher");
        const Span aad = wholeOf(vm, args[2], "associated data");
        const Span in = spanOf(vm, args[3], args[4], args[5], "what is opened");
        if (in.size < Aead::kTag) return Value::integer(-1);
        const size_t textSize = in.size - Aead::kTag;
        uint8_t* out = destination(vm, args[6], args[7], textSize);
        if (out != in.data && out < in.data + in.size && in.data < out + textSize)
            throw vm.runtimeError(Thrown::IllegalArgumentException, "opened bytes go in the same place as the sealed ones, or apart from them, not overlapping");
        const bool authentic = box.aead->open(nonce.data, nonce.size, aad.data, aad.size, in.data, textSize, in.data + textSize, out);
        return Value::integer(authentic ? static_cast<int64_t>(textSize) : -1);
    });

    // ---- Public keys (networking plan, part 3) ----

    // x25519(scalar, u): the shared secret, or null where it is all zeros.
    def("x25519", 2, 2, [](VM& vm, int, Value* args) -> Value {
        const Span k = exactly(vm, args[0], 32, "an X25519 private key"), u = exactly(vm, args[1], 32, "an X25519 public key");
        uint8_t out[32];
        if (!x25519(out, k.data, u.data)) return Value::null();
        const Value made = newBytes(vm, out, 32);
        wipe(out, 32);
        return made;
    });
    def("x25519Public", 1, 1, [](VM& vm, int, Value* args) -> Value {
        const Span k = exactly(vm, args[0], 32, "an X25519 private key");
        uint8_t out[32];
        x25519Public(out, k.data);
        return newBytes(vm, out, 32);
    });
    def("ed25519Public", 1, 1, [](VM& vm, int, Value* args) -> Value {
        const Span seed = exactly(vm, args[0], 32, "an Ed25519 private key");
        uint8_t out[32];
        ed25519Public(out, seed.data);
        return newBytes(vm, out, 32);
    });
    def("ed25519Sign", 2, 2, [](VM& vm, int, Value* args) -> Value {
        const Span seed = exactly(vm, args[0], 32, "an Ed25519 private key");
        const Span m = wholeOf(vm, args[1], "a message");
        uint8_t sig[64];
        ed25519Sign(sig, seed.data, m.data, m.size);
        return newBytes(vm, sig, 64);
    });
    // ed25519Verify(public, message, signature): false for any size but 32 and 64 too.
    def("ed25519Verify", 3, 3, [](VM& vm, int, Value* args) -> Value {
        const Span key = wholeOf(vm, args[0], "a public key"), m = wholeOf(vm, args[1], "a message"), sig = wholeOf(vm, args[2], "a signature");
        if (key.size != 32 || sig.size != 64) return Value::boolean(false);
        return Value::boolean(ed25519Verify(key.data, m.data, m.size, sig.data));
    });
    // The prime curves, P-256 or P-384 by `curve` (256 or 384): ecPoint(curve, bytes), the point
    // as 04 X Y, checked on the curve, or null; ecValidPrivate(curve, bytes); ecPublic(curve, d);
    // ecEcdh(curve, d, point), the shared x or null for infinity; ecSign(curve, d, hash kind,
    // message), r and s; ecVerify(curve, point, hash kind, message, r and s).
    def("ecPoint", 2, 2, [](VM& vm, int, Value* args) -> Value {
        const bool big = curveOf(vm, args[0]);
        const Span b = wholeOf(vm, args[1], "a point");
        uint8_t out[97];
        if (!(big ? p384Point(b.data, b.size, out) : p256Point(b.data, b.size, out))) return Value::null();
        return newBytes(vm, out, big ? 97 : 65);
    });
    def("ecValidPrivate", 2, 2, [](VM& vm, int, Value* args) -> Value {
        const bool big = curveOf(vm, args[0]);
        const Span d = wholeOf(vm, args[1], "a private key");
        return Value::boolean(d.size == (big ? 48u : 32u) && (big ? p384ValidPrivate(d.data) : p256ValidPrivate(d.data)));
    });
    def("ecPublic", 2, 2, [](VM& vm, int, Value* args) -> Value {
        const bool big = curveOf(vm, args[0]);
        const Span d = exactly(vm, args[1], big ? 48 : 32, "a private key");
        uint8_t out[97];
        if (big) p384Public(out, d.data);
        else p256Public(out, d.data);
        return newBytes(vm, out, big ? 97 : 65);
    });
    def("ecEcdh", 3, 3, [](VM& vm, int, Value* args) -> Value {
        const bool big = curveOf(vm, args[0]);
        const Span d = exactly(vm, args[1], big ? 48 : 32, "a private key"), q = exactly(vm, args[2], big ? 97 : 65, "a point");
        uint8_t out[48];
        if (!(big ? p384Ecdh(out, d.data, q.data) : p256Ecdh(out, d.data, q.data))) return Value::null();
        const Value made = newBytes(vm, out, big ? 48 : 32);
        wipe(out, sizeof(out));
        return made;
    });
    def("ecSign", 4, 4, [](VM& vm, int, Value* args) -> Value {
        const bool big = curveOf(vm, args[0]);
        const Span d = exactly(vm, args[1], big ? 48 : 32, "a private key");
        std::unique_ptr<Hash> h = hashFor(vm, args[2], Value::integer(32));
        const Span m = wholeOf(vm, args[3], "a message");
        h->update(m.data, m.size);
        std::vector<uint8_t> digest(h->size());
        h->finish(digest.data());
        uint8_t sig[96];
        const size_t half = big ? 48 : 32;
        if (big) p384Sign(sig, sig + half, d.data, digest.data(), digest.size(), *h);
        else p256Sign(sig, sig + half, d.data, digest.data(), digest.size(), *h);
        return newBytes(vm, sig, 2 * half);
    });
    def("ecVerify", 5, 5, [](VM& vm, int, Value* args) -> Value {
        const bool big = curveOf(vm, args[0]);
        const Span q = wholeOf(vm, args[1], "a point"), m = wholeOf(vm, args[3], "a message"), sig = wholeOf(vm, args[4], "a signature");
        std::unique_ptr<Hash> h = hashFor(vm, args[2], Value::integer(32));
        const size_t half = big ? 48 : 32;
        if (q.size != 2 * half + 1 || sig.size != 2 * half) return Value::boolean(false);
        h->update(m.data, m.size);
        std::vector<uint8_t> digest(h->size());
        h->finish(digest.data());
        return Value::boolean(big ? p384Verify(q.data, digest.data(), digest.size(), sig.data, sig.data + half)
                                  : p256Verify(q.data, digest.data(), digest.size(), sig.data, sig.data + half));
    });
    // rsaGenerate(bits, e): [n, e, d, p, q, dp, dq, qinv], waiting on its thread as a read does.
    def("rsaGenerate", 2, 2, [](VM& vm, int, Value* args) -> Value {
        const int64_t bits = vm.toInt(args[0], "bits"), e = vm.toInt(args[1], "public exponent");
        struct Work {
            RsaKey key;
            std::string why;
            bool done = false;
            ~Work() { wipe(key.d.data(), key.d.size()), wipe(key.p.data(), key.p.size()), wipe(key.q.data(), key.q.size()); }
        };
        auto work = std::make_shared<Work>();
        if (bits < 0 || e < 0) throw vm.runtimeError(Thrown::IllegalArgumentException, "an RSA key's bits and exponent are positive");
        return vm.blocking([work, bits, e] { work->done = rsaGenerate(static_cast<size_t>(bits), static_cast<uint64_t>(e), work->key, work->why); },
                           [work](VM& vm) -> Value {
                               if (!work->done) throw vm.runtimeError(Thrown::IllegalArgumentException, work->why);
                               const RsaKey& k = work->key;
                               ObjList* list = newList(vm);
                               TempRoot keep(vm, Value::object(list));
                               for (const auto* part : {&k.n, &k.e, &k.d, &k.p, &k.q, &k.dp, &k.dq, &k.qinv}) list->values().push_back(newBytes(vm, part->data(), part->size()));
                               return Value::object(list);
                           });
    });
    // rsaSign([n, e, d, p, q, dp, dq, qinv], hash kind, message, pss, salt length, mgf hash kind)
    def("rsaSign", 6, 6, [](VM& vm, int, Value* args) -> Value {
        if (!args[0].isObjType(ObjType::List) || asList(args[0])->size() != 8) throw vm.runtimeError(Thrown::IllegalArgumentException, "not an RSA private key");
        RsaKey key;
        std::vector<uint8_t>* parts[8] = {&key.n, &key.e, &key.d, &key.p, &key.q, &key.dp, &key.dq, &key.qinv};
        for (size_t i = 0; i < 8; i++) {
            const Value v = vm.listGet(asList(args[0]), i);
            if (v.isNull()) continue;
            const Span s = wholeOf(vm, v, "a key's number");
            parts[i]->assign(s.data, s.data + s.size);
        }
        const HashKind kind = kindOf(vm, args[1]);
        const Span m = wholeOf(vm, args[2], "a message");
        const bool pss = args[3].truthy();
        const int64_t saltLen = vm.toInt(args[4], "salt length");
        const HashKind mgf = kindOf(vm, args[5]);
        if (saltLen < 0) throw vm.runtimeError(Thrown::IllegalArgumentException, "a salt is 0 bytes or more");
        std::vector<uint8_t> signature;
        std::string why;
        const bool done = rsaSign(key, kind, m.data, m.size, pss, static_cast<size_t>(saltLen), mgf, signature, why);
        for (auto* part : parts) wipe(part->data(), part->size());
        if (!done) throw vm.runtimeError(Thrown::IllegalArgumentException, why);
        return newBytes(vm, signature.data(), signature.size());
    });
    // rsaVerify(n, e, hash kind, message, signature, pss, salt length, mgf hash kind)
    def("rsaVerify", 8, 8, [](VM& vm, int, Value* args) -> Value {
        const Span n = wholeOf(vm, args[0], "a modulus"), e = wholeOf(vm, args[1], "an exponent"), m = wholeOf(vm, args[3], "a message"),
                   sig = wholeOf(vm, args[4], "a signature");
        const HashKind kind = kindOf(vm, args[2]), mgf = kindOf(vm, args[7]);
        const int64_t saltLen = vm.toInt(args[6], "salt length");
        if (saltLen < 0 || n.data == nullptr || e.data == nullptr || sig.data == nullptr) return Value::boolean(false);
        return Value::boolean(rsaVerify(std::vector<uint8_t>(n.data, n.data + n.size), std::vector<uint8_t>(e.data, e.data + e.size), kind, m.data, m.size,
                                        std::vector<uint8_t>(sig.data, sig.data + sig.size), args[5].truthy(), static_cast<size_t>(saltLen), mgf));
    });

    // ---- Certificates (networking plan, part 4) ----

    // aesCbc(key, iv, data, encrypt): AES-CBC with PKCS#7 padding, for encrypted private keys and
    // PKCS#12 files; decrypting, null where the padding is wrong (a wrong password, mostly).
    def("aesCbc", 4, 4, [](VM& vm, int, Value* args) -> Value {
        const Span key = wholeOf(vm, args[0], "a key"), iv = exactly(vm, args[1], 16, "an IV"), data = wholeOf(vm, args[2], "data");
        if (key.size != 16 && key.size != 24 && key.size != 32) throw vm.runtimeError(Thrown::IllegalArgumentException, "an AES key is 16, 24 or 32 bytes");
        std::vector<uint8_t> out;
        if (!aesCbc(key.data, key.size, iv.data, data.data, data.size, args[3].truthy(), out)) return Value::null();
        const Value made = newBytes(vm, out.data(), out.size());
        wipe(out.data(), out.size());
        return made;
    });

    // systemRoots(): the certificates the system trusts as roots, as DER; on Windows from its
    // store of roots (what the "Trusted Root Certification Authorities" show), elsewhere null:
    // lib/Crypto reads Linux's files itself.
    def("systemRoots", 0, 0, [](VM& vm, int, Value*) -> Value {
#ifdef _WIN32
        HCERTSTORE store = CertOpenSystemStoreW(0, L"ROOT");
        if (store == nullptr) throw vm.errorOf("IOException", "cannot open the system's store of root certificates");
        ObjList* list = newList(vm);
        TempRoot keep(vm, Value::object(list));
        for (PCCERT_CONTEXT cert = CertEnumCertificatesInStore(store, nullptr); cert != nullptr; cert = CertEnumCertificatesInStore(store, cert)) {
            if (cert->dwCertEncodingType != X509_ASN_ENCODING) continue;
            list->values().push_back(newBytes(vm, cert->pbCertEncoded, cert->cbCertEncoded));
        }
        CertCloseStore(store, 0);
        return Value::object(list);
#else
        (void)vm;
        return Value::null();
#endif
    });

    // systemRootFor(chain): the chain Windows builds to a root it trusts for a chain (the
    // certificates' DER one after another, the leaf first), as the same, the root last; null
    // where it finds none. Windows keeps only some of its trusted roots in the store
    // systemRoots() reads and fetches the others as a chain needs them (and intermediates its
    // certificates name); building the chain fetches them. What it gives is only a candidate:
    // lib/Crypto checks the chain itself. Elsewhere null.
    def("systemRootFor", 1, 1, [](VM& vm, int, Value* args) -> Value {
#ifdef _WIN32
        const Span all = wholeOf(vm, args[0], "chain");
        std::vector<std::pair<const uint8_t*, DWORD>> certs;
        size_t at = 0;
        while (at < all.size) {
            if (all.size - at < 2 || all.data[at] != 0x30) return Value::null();
            size_t length = all.data[at + 1], header = 2;
            if (length & 0x80) {
                const size_t count = length & 0x7F;
                if (count < 1 || count > 4 || all.size - at < 2 + count) return Value::null();
                length = 0;
                for (size_t i = 0; i < count; i++) length = (length << 8) | all.data[at + 2 + i];
                header = 2 + count;
            }
            if (length > all.size - at - header) return Value::null();
            certs.emplace_back(all.data + at, static_cast<DWORD>(header + length));
            at += header + length;
        }
        if (certs.empty()) return Value::null();
        PCCERT_CONTEXT leaf = CertCreateCertificateContext(X509_ASN_ENCODING, certs[0].first, certs[0].second);
        if (leaf == nullptr) return Value::null();
        HCERTSTORE given = CertOpenStore(CERT_STORE_PROV_MEMORY, 0, 0, 0, nullptr);
        for (size_t i = 1; i < certs.size() && given != nullptr; i++)
            CertAddEncodedCertificateToStore(given, X509_ASN_ENCODING, certs[i].first, certs[i].second, CERT_STORE_ADD_ALWAYS, nullptr);
        CERT_CHAIN_PARA para{};
        para.cbSize = sizeof(para);
        PCCERT_CHAIN_CONTEXT chain = nullptr;
        Value result = Value::null();
        if (CertGetCertificateChain(nullptr, leaf, nullptr, given, &para, 0, nullptr, &chain) && chain != nullptr && chain->cChain > 0) {
            const CERT_SIMPLE_CHAIN* simple = chain->rgpChain[0];
            const DWORD refused = CERT_TRUST_IS_UNTRUSTED_ROOT | CERT_TRUST_IS_PARTIAL_CHAIN | CERT_TRUST_IS_EXPLICIT_DISTRUST;
            if (simple->cElement > 0 && (simple->TrustStatus.dwErrorStatus & refused) == 0) {
                std::vector<uint8_t> joined;
                for (DWORD i = 0; i < simple->cElement; i++) {
                    PCCERT_CONTEXT c = simple->rgpElement[i]->pCertContext;
                    joined.insert(joined.end(), c->pbCertEncoded, c->pbCertEncoded + c->cbCertEncoded);
                }
                result = newBytes(vm, joined.data(), joined.size());
            }
        }
        if (chain != nullptr) CertFreeCertificateChain(chain);
        if (given != nullptr) CertCloseStore(given, 0);
        CertFreeCertificateContext(leaf);
        return result;
#else
        (void)vm;
        (void)args;
        return Value::null();
#endif
    });

    // sha1(data): SHA-1, for what X.509 names with it alone (key identifiers, the fingerprints
    // people quote); never for a signature or a MAC here.
    def("sha1", 1, 1, [](VM& vm, int, Value* args) -> Value {
        const Span d = wholeOf(vm, args[0], "data");
        uint8_t out[20];
        sha1(out, d.data, d.size);
        return newBytes(vm, out, 20);
    });

    // ---- What every secret needs ----

    // equal(a, b): whether the bytes are the same, in a time that depends on their size alone.
    def("equal", 2, 2, [](VM& vm, int, Value* args) -> Value {
        const Span a = wholeOf(vm, args[0], "bytes"), b = wholeOf(vm, args[1], "bytes");
        if (a.size != b.size) return Value::boolean(false);
        return Value::boolean(equal(a.data, b.data, a.size));
    });
    // wipe(bytes): zeros, written so the compiler keeps them.
    def("wipe", 1, 1, [](VM& vm, int, Value* args) -> Value {
        ObjBytes* b = bytesOf(vm, args[0], "what is wiped");
        vm.guardWrite(b);
        wipe(b->data.data(), b->data.size());
        return Value::null();
    });
    // instructions(): the processor's instructions in use, as names.
    def("instructions", 0, 0, [](VM& vm, int, Value*) -> Value {
        const Cpu& c = cpu();
        ObjList* list = newList(vm);
        TempRoot keep(vm, Value::object(list));
        if (c.aes) list->values().push_back(vm.str("aes"));
        if (c.clmul) list->values().push_back(vm.str("clmul"));
        if (c.sha) list->values().push_back(vm.str("sha"));
        return Value::object(list);
    });
    // setPortable(on): the portable code everywhere, or the processor's instructions again.
    def("setPortable", 1, 1, [](VM&, int, Value* args) -> Value {
        setPortable(args[0].truthy());
        return Value::null();
    });
    def("portable", 0, 0, [](VM&, int, Value*) -> Value { return Value::boolean(portable()); });

    vm.defineGlobal("__crypto", Value::object(holder));
}

}  // namespace ww
