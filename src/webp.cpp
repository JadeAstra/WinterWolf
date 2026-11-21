// What WebP (src/std/webp.ww) is built on: the work for every pixel of its two codecs, VP8L
// (lossless) and VP8 (lossy), in C++ of WebP's own, as JPEG's scans are (the user, 2026-11-08).
// The RIFF container, the extended header, alpha's chunk, animation's frames and errors are WinterWolf.
// Written from the WebP specification (RFC 9649) and the VP8 one (RFC 6386).

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "builtins.h"
#include "fmath.h"
#include "vm.h"
#include "vp8_tables.h"

namespace ww {

namespace {

// ---- VP8L: lossless ----

// Bits lowest first, through 64 kept at a time; zeros past the end, which `overrun` notes.
struct LBits {
    const uint8_t* p;
    const uint8_t* end;
    uint64_t buffer = 0;
    int count = 0;
    size_t beyond = 0;

    void fill() {
        while (count <= 56) {
            if (p < end) buffer |= static_cast<uint64_t>(*p++) << count;
            else beyond++;
            count += 8;
        }
    }
    uint32_t peek(int n) {
        if (count < n) fill();
        return static_cast<uint32_t>(buffer & ((uint64_t{1} << n) - 1));
    }
    void drop(int n) {
        buffer >>= n;
        count -= n;
    }
    uint32_t get(int n) {
        if (n == 0) return 0;
        const uint32_t v = peek(n);
        drop(n);
        return v;
    }
    bool overrun() const { return static_cast<int64_t>(beyond) * 8 > count; }
};

uint32_t reverse(uint32_t code, int length) {
    uint32_t r = 0;
    for (int i = 0; i < length; i++) r |= ((code >> i) & 1) << (length - 1 - i);
    return r;
}

// A prefix code, lowest bit first: a table of the next 10 bits, and the rest a bit at a time. A
// code of one symbol takes no bits.
struct Prefix {
    static constexpr int kFast = 10;
    std::vector<uint32_t> fast;  // (symbol << 5) | length, 0xFFFFFFFF where longer
    std::vector<uint16_t> counts, sorted;
    std::vector<uint32_t> firstCode, firstIndex;
    int single = -1;

    bool build(const std::vector<int>& lengths) {
        const size_t n = lengths.size();
        counts.assign(16, 0);
        int used = 0, last = -1;
        for (size_t i = 0; i < n; i++)
            if (lengths[i] > 0) {
                counts[static_cast<size_t>(lengths[i])]++;
                used++;
                last = static_cast<int>(i);
            }
        if (used == 0) return false;
        if (used == 1) {
            single = last;
            return true;
        }
        int64_t left = 1;
        for (int l = 1; l < 16; l++) {
            left = left * 2 - counts[static_cast<size_t>(l)];
            if (left < 0) return false;
        }
        if (left != 0) return false;  // VP8L's codes are complete
        firstCode.assign(16, 0);
        firstIndex.assign(16, 0);
        std::vector<uint32_t> next(16, 0);
        uint32_t code = 0, index = 0;
        for (int l = 1; l < 16; l++) {
            code = (code + counts[static_cast<size_t>(l - 1)]) << 1;
            firstCode[static_cast<size_t>(l)] = code;
            firstIndex[static_cast<size_t>(l)] = index;
            next[static_cast<size_t>(l)] = index;
            index += counts[static_cast<size_t>(l)];
        }
        counts[0] = 0;
        sorted.assign(index, 0);
        fast.assign(size_t{1} << kFast, 0xFFFFFFFFu);
        for (size_t s = 0; s < n; s++) {
            const int l = lengths[s];
            if (l == 0) continue;
            const uint32_t at = next[static_cast<size_t>(l)]++;
            sorted[at] = static_cast<uint16_t>(s);
            if (l > kFast) continue;
            const uint32_t c = firstCode[static_cast<size_t>(l)] + (at - firstIndex[static_cast<size_t>(l)]);
            for (uint32_t k = reverse(c, l); k < (1u << kFast); k += 1u << l) fast[k] = (static_cast<uint32_t>(s) << 5) | static_cast<uint32_t>(l);
        }
        return true;
    }

    int read(LBits& b) const {
        if (single >= 0) return single;
        const uint32_t bits = b.peek(15);
        const uint32_t entry = fast[bits & ((1u << kFast) - 1)];
        if (entry != 0xFFFFFFFFu) {
            b.drop(static_cast<int>(entry & 31));
            return static_cast<int>(entry >> 5);
        }
        uint32_t c = reverse(bits & ((1u << kFast) - 1), kFast);
        for (int l = kFast + 1; l < 16; l++) {
            c = (c << 1) | ((bits >> (l - 1)) & 1);
            const uint32_t offset = c - firstCode[static_cast<size_t>(l)];
            if (offset < counts[static_cast<size_t>(l)]) {
                b.drop(l);
                return sorted[firstIndex[static_cast<size_t>(l)] + offset];
            }
        }
        return -1;
    }
};

constexpr int kCodeLengthOrder[19] = {17, 18, 0, 1, 2, 3, 4, 5, 16, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};

// The 120 shortest distances, as (across, down) from the pixel: a distance code d of 1 to 120
// means the pixel there; past 120, d - 120 pixels back.
constexpr int8_t kPlane[120][2] = {
    {0, 1},  {1, 0},  {1, 1},  {-1, 1}, {0, 2},  {2, 0},  {1, 2},  {-1, 2}, {2, 1},  {-2, 1}, {2, 2},  {-2, 2}, {0, 3},  {3, 0},  {1, 3},
    {-1, 3}, {3, 1},  {-3, 1}, {2, 3},  {-2, 3}, {3, 2},  {-3, 2}, {0, 4},  {4, 0},  {1, 4},  {-1, 4}, {4, 1},  {-4, 1}, {3, 3},  {-3, 3},
    {2, 4},  {-2, 4}, {4, 2},  {-4, 2}, {0, 5},  {3, 4},  {-3, 4}, {4, 3},  {-4, 3}, {5, 0},  {1, 5},  {-1, 5}, {5, 1},  {-5, 1}, {2, 5},
    {-2, 5}, {5, 2},  {-5, 2}, {4, 4},  {-4, 4}, {3, 5},  {-3, 5}, {5, 3},  {-5, 3}, {0, 6},  {6, 0},  {1, 6},  {-1, 6}, {6, 1},  {-6, 1},
    {2, 6},  {-2, 6}, {6, 2},  {-6, 2}, {4, 5},  {-4, 5}, {5, 4},  {-5, 4}, {3, 6},  {-3, 6}, {6, 3},  {-6, 3}, {0, 7},  {7, 0},  {1, 7},
    {-1, 7}, {5, 5},  {-5, 5}, {7, 1},  {-7, 1}, {4, 6},  {-4, 6}, {6, 4},  {-6, 4}, {2, 7},  {-2, 7}, {7, 2},  {-7, 2}, {3, 7},  {-3, 7},
    {7, 3},  {-7, 3}, {5, 6},  {-5, 6}, {6, 5},  {-6, 5}, {8, 0},  {4, 7},  {-4, 7}, {7, 4},  {-7, 4}, {8, 1},  {8, 2},  {6, 6},  {-6, 6},
    {8, 3},  {5, 7},  {-5, 7}, {7, 5},  {-7, 5}, {8, 4},  {6, 7},  {-6, 7}, {7, 6},  {-7, 6}, {8, 5},  {7, 7},  {-7, 7}, {8, 6},  {8, 7}};

struct LosslessError {
    std::string what;
};

[[noreturn]] void fail(const std::string& what) { throw LosslessError{what}; }

class Lossless {
    struct Transform;

public:
    Lossless(const uint8_t* data, size_t size) : b_{data, data + size} {}

    // The picture: its size and ARGB pixels (0xAARRGGBB). `headerless` for ALPH's data, which has
    // no signature or size of its own.
    void read(std::vector<uint32_t>& argb, int& width, int& height, bool headerless = false) {
        if (!headerless) {
            if (b_.get(8) != 0x2F) fail("its lossless data does not start with 0x2F");
            width = static_cast<int>(b_.get(14)) + 1;
            height = static_cast<int>(b_.get(14)) + 1;
            b_.get(1);  // alpha is used: a hint only
            if (b_.get(3) != 0) fail("a lossless version other than 0");
        }
        // The transforms, in the order they were applied; undone in the other order.
        std::vector<Transform> transforms;
        int xsize = width;
        bool seen[4] = {};
        while (b_.get(1)) {
            Transform t;
            t.kind = static_cast<int>(b_.get(2));
            if (seen[t.kind]) fail("a transform used twice");
            seen[t.kind] = true;
            t.width = xsize;
            t.bits = 0;
            if (t.kind == 0 || t.kind == 1) {
                t.bits = static_cast<int>(b_.get(3)) + 2;
                t.data = image(subsampled(xsize, t.bits), subsampled(height, t.bits), false);
            } else if (t.kind == 3) {
                const int colors = static_cast<int>(b_.get(8)) + 1;
                t.data = image(colors, 1, false);
                for (size_t i = 1; i < t.data.size(); i++) t.data[i] = addPixels(t.data[i], t.data[i - 1]);
                t.data.resize(256, 0);  // an index past the table: transparent black
                t.bits = colors <= 2 ? 3 : colors <= 4 ? 2 : colors <= 16 ? 1 : 0;
                xsize = subsampled(xsize, t.bits);
            }
            transforms.push_back(std::move(t));
        }
        argb = image(xsize, height, true);
        for (size_t i = transforms.size(); i-- > 0;) undo(transforms[i], argb, height);
        if (b_.overrun()) fail("its lossless data ends early");
    }

    // A predictor's guess, for the writer.
    static uint32_t predictFor(int mode, const uint32_t* px, size_t at, size_t w) { return predict(mode, px, at, w); }

private:
    struct Transform {
        int kind, bits;
        std::vector<uint32_t> data;
        int width;  // the picture's width when it was applied
    };

    LBits b_;

    static int subsampled(int size, int bits) { return (size + (1 << bits) - 1) >> bits; }

    static uint32_t addPixels(uint32_t a, uint32_t b) {
        const uint32_t ag = (a & 0xFF00FF00u) + (b & 0xFF00FF00u), rb = (a & 0x00FF00FFu) + (b & 0x00FF00FFu);
        return (ag & 0xFF00FF00u) | (rb & 0x00FF00FFu);
    }

    std::vector<int> codeLengths(int alphabet) {
        std::vector<int> lengths(static_cast<size_t>(alphabet), 0);
        if (b_.get(1)) {  // simple: one or two symbols
            const int count = static_cast<int>(b_.get(1)) + 1;
            const int first = static_cast<int>(b_.get(b_.get(1) ? 8 : 1));
            if (first >= alphabet) fail("a code's symbol past its alphabet");
            lengths[static_cast<size_t>(first)] = 1;
            if (count == 2) {
                const int second = static_cast<int>(b_.get(8));
                if (second >= alphabet) fail("a code's symbol past its alphabet");
                lengths[static_cast<size_t>(second)] = 1;
            }
            return lengths;
        }
        const int count = static_cast<int>(b_.get(4)) + 4;
        if (count > 19) fail("too many code length codes");
        std::vector<int> lengthLengths(19, 0);
        for (int i = 0; i < count; i++) lengthLengths[static_cast<size_t>(kCodeLengthOrder[i])] = static_cast<int>(b_.get(3));
        Prefix lengthCode;
        if (!lengthCode.build(lengthLengths)) fail("a code length code of impossible lengths");
        int most = alphabet;
        if (b_.get(1)) {
            const int nbits = 2 + 2 * static_cast<int>(b_.get(3));
            most = 2 + static_cast<int>(b_.get(nbits));
            if (most > alphabet) fail("more code lengths than the alphabet has");
        }
        int previous = 8;
        for (int symbol = 0; symbol < alphabet;) {
            if (most-- == 0) break;
            const int length = lengthCode.read(b_);
            if (length < 0 || b_.overrun()) fail("bits that are no code length");
            if (length < 16) {
                lengths[static_cast<size_t>(symbol++)] = length;
                if (length != 0) previous = length;
                continue;
            }
            const int extra = length == 16 ? 2 : length == 17 ? 3 : 7;
            const int offset = length == 18 ? 11 : 3;
            const int repeat = static_cast<int>(b_.get(extra)) + offset;
            if (symbol + repeat > alphabet) fail("code lengths past the alphabet");
            const int value = length == 16 ? previous : 0;
            for (int k = 0; k < repeat; k++) lengths[static_cast<size_t>(symbol++)] = value;
        }
        return lengths;
    }

    struct Group {
        Prefix codes[5];  // green with lengths and the cache, red, blue, alpha, distance
    };

    Group group(int cacheSize) {
        Group g;
        const int alphabets[5] = {256 + 24 + cacheSize, 256, 256, 256, 40};
        for (int i = 0; i < 5; i++)
            if (!g.codes[i].build(codeLengths(alphabets[i]))) fail("a prefix code of impossible lengths");
        return g;
    }

    int lengthOrDistance(int prefix) {
        if (prefix < 4) return prefix + 1;
        const int extra = (prefix - 2) >> 1;
        const int offset = (2 + (prefix & 1)) << extra;
        return offset + static_cast<int>(b_.get(extra)) + 1;
    }

    // An entropy-coded picture: the main one (with its meta prefix codes) or one of a transform's.
    std::vector<uint32_t> image(int w, int h, bool main) {
        int cacheBits = 0;
        if (b_.get(1)) {
            cacheBits = static_cast<int>(b_.get(4));
            if (cacheBits < 1 || cacheBits > 11) fail("a colour cache of " + std::to_string(cacheBits) + " bits");
        }
        const int cacheSize = cacheBits > 0 ? 1 << cacheBits : 0;
        int prefixBits = 0;
        std::vector<uint32_t> groupsImage;
        int groupsWide = 0;
        size_t groupCount = 1;
        if (main && b_.get(1)) {
            prefixBits = static_cast<int>(b_.get(3)) + 2;
            groupsWide = subsampled(w, prefixBits);
            groupsImage = image(groupsWide, subsampled(h, prefixBits), false);
            uint32_t most = 0;
            for (uint32_t& g : groupsImage) {
                g = (g >> 8) & 0xFFFF;
                most = std::max(most, g);
            }
            groupCount = most + 1;
        }
        std::vector<Group> groups;
        groups.reserve(groupCount);
        for (size_t i = 0; i < groupCount; i++) groups.push_back(group(cacheSize));
        std::vector<uint32_t> cache(static_cast<size_t>(cacheSize), 0);
        const size_t total = static_cast<size_t>(w) * static_cast<size_t>(h);
        // Room for every pixel asked for at once but filled only as pixels come: the size is the
        // file's word, and a few bytes can claim 16384 by 16384; memory never written to costs
        // nothing, so data that fails early has used next to none.
        std::vector<uint32_t> out;
        out.reserve(total);
        auto remember = [&](uint32_t argb) {
            if (cacheBits > 0) cache[(0x1E35A7BDu * argb) >> (32 - cacheBits)] = argb;
        };
        size_t at = 0;
        while (at < total) {
            const int x = static_cast<int>(at % static_cast<size_t>(w)), y = static_cast<int>(at / static_cast<size_t>(w));
            const Group& g = groupsImage.empty() ? groups[0] : groups[groupsImage[static_cast<size_t>((y >> prefixBits) * groupsWide + (x >> prefixBits))]];
            const int s = g.codes[0].read(b_);
            if (s < 0 || b_.overrun()) fail(b_.overrun() ? "its lossless data ends early" : "bits that are no code");
            if (s < 256) {
                const uint32_t red = static_cast<uint32_t>(g.codes[1].read(b_)), blue = static_cast<uint32_t>(g.codes[2].read(b_)),
                               alpha = static_cast<uint32_t>(g.codes[3].read(b_));
                const uint32_t argb = (alpha << 24) | (red << 16) | (static_cast<uint32_t>(s) << 8) | blue;
                out.push_back(argb);
                at++;
                remember(argb);
            } else if (s < 256 + 24) {
                const int length = lengthOrDistance(s - 256);
                const int code = g.codes[4].read(b_);
                if (code < 0) fail("bits that are no code");
                const int distanceCode = lengthOrDistance(code);
                int64_t distance;
                if (distanceCode > 120) {
                    distance = distanceCode - 120;
                } else {
                    distance = kPlane[distanceCode - 1][0] + static_cast<int64_t>(kPlane[distanceCode - 1][1]) * w;
                    if (distance < 1) distance = 1;
                }
                if (static_cast<size_t>(distance) > at) fail("a copy from before the picture's start");
                if (at + static_cast<size_t>(length) > total) fail("a copy past the picture's end");
                for (int k = 0; k < length; k++) {
                    const uint32_t argb = out[at - static_cast<size_t>(distance)];
                    out.push_back(argb);
                    remember(argb);
                    at++;
                }
            } else {
                const int key = s - 256 - 24;
                if (key >= cacheSize) fail("a colour cache index past the cache");
                const uint32_t argb = cache[static_cast<size_t>(key)];
                out.push_back(argb);
                remember(argb);
                at++;
            }
        }
        return out;
    }

    static uint32_t average(uint32_t a, uint32_t b) { return (((a ^ b) & 0xFEFEFEFEu) >> 1) + (a & b); }
    static int channel(uint32_t v, int shift) { return static_cast<int>((v >> shift) & 0xFF); }
    static uint32_t clampAddSubtractFull(uint32_t a, uint32_t b, uint32_t c) {
        uint32_t out = 0;
        for (int s = 0; s < 32; s += 8) out |= static_cast<uint32_t>(std::clamp(channel(a, s) + channel(b, s) - channel(c, s), 0, 255)) << s;
        return out;
    }
    static uint32_t clampAddSubtractHalf(uint32_t a, uint32_t b) {
        uint32_t out = 0;
        for (int s = 0; s < 32; s += 8) {
            const int x = channel(a, s);
            out |= static_cast<uint32_t>(std::clamp(x + (x - channel(b, s)) / 2, 0, 255)) << s;
        }
        return out;
    }
    static uint32_t select(uint32_t l, uint32_t t, uint32_t tl) {
        int pl = 0, pt = 0;
        for (int s = 0; s < 32; s += 8) {
            const int p = channel(l, s) + channel(t, s) - channel(tl, s);
            pl += std::abs(p - channel(l, s));
            pt += std::abs(p - channel(t, s));
        }
        return pl < pt ? l : t;
    }

    static uint32_t predict(int mode, const uint32_t* px, size_t at, size_t w) {
        const uint32_t l = px[at - 1], t = px[at - w], tr = px[at - w + 1], tl = px[at - w - 1];
        switch (mode) {
            case 0: return 0xFF000000u;
            case 1: return l;
            case 2: return t;
            case 3: return tr;
            case 4: return tl;
            case 5: return average(average(l, tr), t);
            case 6: return average(l, tl);
            case 7: return average(l, t);
            case 8: return average(tl, t);
            case 9: return average(t, tr);
            case 10: return average(average(l, tl), average(t, tr));
            case 11: return select(l, t, tl);
            case 12: return clampAddSubtractFull(l, t, tl);
            case 13: return clampAddSubtractHalf(average(l, t), tl);
            default: return 0xFF000000u;  // 14 and 15: as 0, as libwebp has them
        }
    }

    static int8_t delta(uint32_t transform, uint32_t color) { return static_cast<int8_t>((static_cast<int8_t>(transform) * static_cast<int8_t>(color)) >> 5); }

    void undo(const Transform& t, std::vector<uint32_t>& px, int height) {
        const int w = t.width;
        if (t.kind == 2) {  // green added back to red and blue
            for (uint32_t& p : px) {
                const uint32_t g = (p >> 8) & 0xFF;
                p = (p & 0xFF00FF00u) | ((((p >> 16) & 0xFF) + g) & 0xFF) << 16 | (((p & 0xFF) + g) & 0xFF);
            }
            return;
        }
        if (t.kind == 3) {  // indices made colours, several to a pixel where they are few
            const int perPixel = 1 << t.bits, bitsEach = 8 >> t.bits, mask = (1 << bitsEach) - 1;
            const int packedWidth = subsampled(w, t.bits);
            std::vector<uint32_t> out(static_cast<size_t>(w) * static_cast<size_t>(height));
            for (int y = 0; y < height; y++)
                for (int x = 0; x < w; x++) {
                    const uint32_t packed = px[static_cast<size_t>(y * packedWidth + (x >> t.bits))];
                    const int index = static_cast<int>((packed >> 8) >> ((x & (perPixel - 1)) * bitsEach)) & mask;
                    out[static_cast<size_t>(y * w + x)] = t.data[static_cast<size_t>(index)];
                }
            px = std::move(out);
            return;
        }
        const int blocksWide = subsampled(w, t.bits);
        const size_t sw = static_cast<size_t>(w);
        for (int y = 0; y < height; y++)
            for (int x = 0; x < w; x++) {
                const size_t at = static_cast<size_t>(y) * sw + static_cast<size_t>(x);
                const uint32_t element = t.data[static_cast<size_t>((y >> t.bits) * blocksWide + (x >> t.bits))];
                if (t.kind == 0) {  // predictor
                    uint32_t prediction;
                    if (y == 0) prediction = x == 0 ? 0xFF000000u : px[at - 1];
                    else if (x == 0) prediction = px[at - sw];
                    else prediction = predict(static_cast<int>((element >> 8) & 0xF), px.data(), at, sw);
                    px[at] = addPixels(px[at], prediction);
                } else {  // colour transform
                    const uint32_t p = px[at];
                    const int green = static_cast<int8_t>((p >> 8) & 0xFF);
                    int red = static_cast<int>((p >> 16) & 0xFF), blue = static_cast<int>(p & 0xFF);
                    red += delta(element & 0xFF, static_cast<uint32_t>(green));          // green to red
                    blue += delta((element >> 8) & 0xFF, static_cast<uint32_t>(green));  // green to blue
                    blue += delta((element >> 16) & 0xFF, static_cast<uint32_t>(red & 0xFF));  // red to blue
                    px[at] = (p & 0xFF00FF00u) | (static_cast<uint32_t>(red & 0xFF) << 16) | static_cast<uint32_t>(blue & 0xFF);
                }
            }
    }
};


// ---- VP8: lossy ----

// The boolean decoder (RFC 6386, 7): bits each with a probability of being 0, in 256ths.
struct BoolDecoder {
    const uint8_t* p = nullptr;
    const uint8_t* end = nullptr;
    uint32_t value = 0, range = 255;
    int bitCount = 0;
    size_t past = 0;  // bytes asked for past the end, read as 0

    void init(const uint8_t* data, size_t size) {
        p = data;
        end = data + size;
        past = 0;
        value = (next() << 8) | next();
        range = 255;
        bitCount = 0;
    }
    uint32_t next() {
        if (p < end) return *p++;
        past++;
        return 0;
    }
    // Read clearly past the end: more than the 2 bytes held ahead and the 2 a writer's shorter
    // flush may leave out. A stream cut short (or made up) fails here, as libwebp's does, rather
    // than going on through every macroblock its header claims with nothing but zeros.
    bool overrun() const { return past > 4; }
    int read(int prob) {
        const uint32_t split = 1 + (((range - 1) * static_cast<uint32_t>(prob)) >> 8);
        const uint32_t bigSplit = split << 8;
        int bit;
        if (value >= bigSplit) {
            bit = 1;
            range -= split;
            value -= bigSplit;
        } else {
            bit = 0;
            range = split;
        }
        while (range < 128) {
            value <<= 1;
            range <<= 1;
            if (++bitCount == 8) {
                bitCount = 0;
                value |= next();
            }
        }
        return bit;
    }
    int literal(int n) {
        int v = 0;
        while (n-- > 0) v = (v << 1) | read(128);
        return v;
    }
    // A flag, then a magnitude of n bits and its sign, where the flag is set.
    int maybeSigned(int n) {
        if (!read(128)) return 0;
        const int v = literal(n);
        return read(128) ? -v : v;
    }
    // A tree's leaf: each node's bit read with its probability; leaves are 0 or negative.
    int tree(const int8_t* t, const uint8_t* probs) {
        int i = 0;
        while ((i = t[i + read(probs[i >> 1])]) > 0) {
        }
        return -i;
    }
};

enum { kDcPred, kVPred, kHPred, kTmPred, kBPred };
enum { kBDc, kBTm, kBVe, kBHe, kBLd, kBRd, kBVr, kBVl, kBHd, kBHu };

constexpr int8_t kKeyFrameYModeTree[8] = {-kBPred, 2, 4, 6, -kDcPred, -kVPred, -kHPred, -kTmPred};
constexpr uint8_t kKeyFrameYModeProbs[4] = {145, 156, 163, 128};
constexpr int8_t kUvModeTree[6] = {-kDcPred, 2, -kVPred, 4, -kHPred, -kTmPred};
constexpr uint8_t kKeyFrameUvModeProbs[3] = {142, 114, 183};
constexpr int8_t kBModeTree[18] = {-kBDc, 2, -kBTm, 4, -kBVe, 6, 8, 12, -kBHe, 10, -kBRd, -kBVr, -kBLd, 14, -kBVl, 16, -kBHd, -kBHu};
constexpr int8_t kSegmentTree[6] = {2, 4, -0, -1, -2, -3};
constexpr uint8_t kBands[17] = {0, 1, 2, 3, 6, 4, 5, 6, 6, 6, 6, 6, 6, 6, 6, 7, 0};
constexpr uint8_t kZigzag[16] = {0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15};
constexpr uint8_t kCat3[] = {173, 148, 140}, kCat4[] = {176, 155, 140, 135}, kCat5[] = {180, 157, 141, 134, 130};
constexpr uint8_t kCat6[] = {254, 254, 243, 230, 196, 177, 153, 140, 133, 130, 129};

struct Vp8Error {
    std::string what;
};

class Lossy {
    friend class LossyWriter;  // the encoder predicts and reconstructs as the decoder does
public:
    Lossy(const uint8_t* data, size_t size) : data_(data), size_(size) {}

    // The picture, as RGBA.
    std::vector<uint8_t> read(int& width, int& height) {
        if (size_ < 10) fail("its lossy data is cut short");
        const uint32_t tag = data_[0] | (data_[1] << 8) | (data_[2] << 16);
        if ((tag & 1) != 0) fail("its lossy data is not a key frame");
        const size_t first = tag >> 5;
        if (data_[3] != 0x9D || data_[4] != 0x01 || data_[5] != 0x2A) fail("its lossy data has no start code");
        width = (data_[6] | (data_[7] << 8)) & 0x3FFF;
        height = (data_[8] | (data_[9] << 8)) & 0x3FFF;
        if (width == 0 || height == 0) fail("a size of 0");
        if (10 + first > size_) fail("its first partition is cut short");
        mbCols_ = (width + 15) / 16;
        mbRows_ = (height + 15) / 16;
        BoolDecoder d;
        d.init(data_ + 10, first);
        header(d, data_ + 10 + first, size_ - 10 - first);
        frame(d);
        loopFilter();
        return toRgba(width, height);
    }

private:
    const uint8_t* data_;
    size_t size_;
    int mbCols_ = 0, mbRows_ = 0;
    // The header's settings.
    bool segmentation_ = false, updateMap_ = false, segmentAbsolute_ = false;
    int segmentQuant_[4] = {}, segmentFilter_[4] = {};
    uint8_t segmentProbs_[3] = {255, 255, 255};
    bool simpleFilter_ = false;
    int filterLevel_ = 0, sharpness_ = 0;
    bool filterDeltas_ = false;
    int refDelta_[4] = {}, modeDelta_[4] = {};
    std::vector<BoolDecoder> partitions_;
    int quant_[4][6] = {};  // per segment: y dc, y ac, y2 dc, y2 ac, uv dc, uv ac
    uint8_t probs_[4][8][3][11];
    bool skipEnabled_ = false;
    int skipProb_ = 0;
    // The planes, with a row above and a column to the left for the edges, and 4 more columns
    // right of the luma for the last macroblock's above-right.
    std::vector<uint8_t> y_, u_, v_;
    size_t yStride_ = 0, uvStride_ = 0;
    struct MbInfo {
        uint8_t yMode, segment, hasCoefficients;
    };
    std::vector<MbInfo> info_;

    [[noreturn]] static void fail(const std::string& what) { throw Vp8Error{what}; }

    uint8_t* yAt(int x, int y) { return &y_[static_cast<size_t>(y + 1) * yStride_ + static_cast<size_t>(x + 1)]; }
    uint8_t* uvAt(std::vector<uint8_t>& plane, int x, int y) { return &plane[static_cast<size_t>(y + 1) * uvStride_ + static_cast<size_t>(x + 1)]; }

    void header(BoolDecoder& d, const uint8_t* rest, size_t restSize) {
        if (d.literal(2) != 0) {
            // The colour space and clamping bits: 0 and 0 are all VP8 defines for its pictures.
        }
        segmentation_ = d.read(128);
        if (segmentation_) {
            updateMap_ = d.read(128);
            if (d.read(128)) {  // the segments' settings
                segmentAbsolute_ = d.read(128);
                for (int& q : segmentQuant_) q = d.maybeSigned(7);
                for (int& f : segmentFilter_) f = d.maybeSigned(6);
            }
            if (updateMap_)
                for (uint8_t& p : segmentProbs_) p = static_cast<uint8_t>(d.read(128) ? d.literal(8) : 255);
        }
        simpleFilter_ = d.read(128);
        filterLevel_ = d.literal(6);
        sharpness_ = d.literal(3);
        filterDeltas_ = d.read(128);
        if (filterDeltas_ && d.read(128)) {
            for (int& r : refDelta_) r = d.maybeSigned(6);
            for (int& m : modeDelta_) m = d.maybeSigned(6);
        }
        const int count = 1 << d.literal(2);
        if (restSize < static_cast<size_t>(3 * (count - 1))) fail("its partitions' sizes are cut short");
        const uint8_t* sizes = rest;
        const uint8_t* at = rest + 3 * (count - 1);
        size_t left = restSize - 3 * static_cast<size_t>(count - 1);
        partitions_.resize(static_cast<size_t>(count));
        for (int i = 0; i < count; i++) {
            size_t size = left;
            if (i < count - 1) size = sizes[i * 3] | (sizes[i * 3 + 1] << 8) | (sizes[i * 3 + 2] << 16);
            if (size > left) fail("a partition past the end");
            partitions_[static_cast<size_t>(i)].init(at, size);
            at += size;
            left -= size;
        }
        const int q = d.literal(7);
        const int yDc = d.maybeSigned(4), y2Dc = d.maybeSigned(4), y2Ac = d.maybeSigned(4), uvDc = d.maybeSigned(4), uvAc = d.maybeSigned(4);
        for (int s = 0; s < 4; s++) {
            int base = q;
            if (segmentation_) base = segmentAbsolute_ ? segmentQuant_[s] : q + segmentQuant_[s];
            auto dc = [](int i) { return static_cast<int>(vp8::kDcQuant[std::clamp(i, 0, 127)]); };
            auto ac = [](int i) { return static_cast<int>(vp8::kAcQuant[std::clamp(i, 0, 127)]); };
            quant_[s][0] = dc(base + yDc);
            quant_[s][1] = ac(base);
            quant_[s][2] = dc(base + y2Dc) * 2;
            quant_[s][3] = std::max(ac(base + y2Ac) * 155 / 100, 8);
            quant_[s][4] = std::min(dc(base + uvDc), 132);
            quant_[s][5] = ac(base + uvAc);
        }
        d.read(128);  // refresh entropy probabilities: one frame only, so nothing to keep
        std::memcpy(probs_, vp8::kDefaultCoeffProbs, sizeof probs_);
        uint8_t* p = &probs_[0][0][0][0];
        for (int i = 0; i < 1056; i++)
            if (d.read(vp8::kCoeffUpdateProbs[i])) p[i] = static_cast<uint8_t>(d.literal(8));
        skipEnabled_ = d.read(128);
        if (skipEnabled_) skipProb_ = d.literal(8);
    }

    // A block's coefficients (dequantized, natural order) from its tokens; whether it had any.
    bool block(BoolDecoder& d, int type, int context, int first, int dcq, int acq, int16_t* out) {
        int c = first;
        const uint8_t* p = probs_[type][kBands[c]][context];
        if (!d.read(p[0])) return false;
        for (;;) {
            while (!d.read(p[1])) {  // a zero: the next has no end of block
                if (++c == 16) return true;
                p = probs_[type][kBands[c]][0];
            }
            int v, next;
            if (!d.read(p[2])) {
                v = 1;
                next = 1;
            } else {
                next = 2;
                if (!d.read(p[3])) {
                    v = !d.read(p[4]) ? 2 : 3 + d.read(p[5]);
                } else if (!d.read(p[6])) {
                    if (!d.read(p[7])) v = 5 + d.read(159);
                    else v = 7 + 2 * d.read(165) + d.read(145);
                } else {
                    const uint8_t* extra;
                    int base, bits;
                    if (!d.read(p[8])) {
                        if (!d.read(p[9])) extra = kCat3, base = 11, bits = 3;
                        else extra = kCat4, base = 19, bits = 4;
                    } else {
                        if (!d.read(p[10])) extra = kCat5, base = 35, bits = 5;
                        else extra = kCat6, base = 67, bits = 11;
                    }
                    int value = 0;
                    for (int k = 0; k < bits; k++) value = (value << 1) | d.read(extra[k]);
                    v = base + value;
                }
            }
            if (d.read(128)) v = -v;
            out[kZigzag[c]] = static_cast<int16_t>(std::clamp(v * (c > 0 ? acq : dcq), -32768, 32767));
            if (++c == 16) return true;
            p = probs_[type][kBands[c]][next];
            if (!d.read(p[0])) return true;
        }
    }

    void frame(BoolDecoder& d) {
        const size_t w16 = static_cast<size_t>(mbCols_) * 16, h16 = static_cast<size_t>(mbRows_) * 16;
        yStride_ = w16 + 1 + 4;
        uvStride_ = w16 / 2 + 1;
        y_.assign(yStride_ * (h16 + 1), 0);
        u_.assign(uvStride_ * (h16 / 2 + 1), 0);
        v_.assign(uvStride_ * (h16 / 2 + 1), 0);
        // The edges: 127 above (the corner too), 129 to the left.
        std::memset(y_.data(), 127, yStride_);
        std::memset(u_.data(), 127, uvStride_);
        std::memset(v_.data(), 127, uvStride_);
        for (size_t r = 1; r <= h16; r++) y_[r * yStride_] = 129;
        for (size_t r = 1; r <= h16 / 2; r++) u_[r * uvStride_] = v_[r * uvStride_] = 129;
        info_.assign(static_cast<size_t>(mbCols_ * mbRows_), MbInfo{});
        // Mode contexts, and token contexts: 4 luma, 2 + 2 chroma, 1 Y2 above each macroblock and left.
        std::vector<uint8_t> aboveModes(static_cast<size_t>(mbCols_) * 4, kBDc);
        std::vector<std::array<uint8_t, 9>> aboveNz(static_cast<size_t>(mbCols_));
        int16_t coefs[25][16];
        for (int my = 0; my < mbRows_; my++) {
            BoolDecoder& tokens = partitions_[static_cast<size_t>(my) % partitions_.size()];
            uint8_t leftModes[4] = {kBDc, kBDc, kBDc, kBDc};
            std::array<uint8_t, 9> leftNz{};
            for (int mx = 0; mx < mbCols_; mx++) {
                MbInfo& mb = info_[static_cast<size_t>(my * mbCols_ + mx)];
                mb.segment = updateMap_ ? static_cast<uint8_t>(d.tree(kSegmentTree, segmentProbs_)) : 0;
                const bool skip = skipEnabled_ ? d.read(skipProb_) != 0 : false;
                mb.yMode = static_cast<uint8_t>(d.tree(kKeyFrameYModeTree, kKeyFrameYModeProbs));
                uint8_t bModes[16];
                uint8_t* above = &aboveModes[static_cast<size_t>(mx) * 4];
                if (mb.yMode == kBPred) {
                    for (int i = 0; i < 16; i++) {
                        const uint8_t a = i < 4 ? above[i] : bModes[i - 4];
                        const uint8_t l = (i & 3) == 0 ? leftModes[i >> 2] : bModes[i - 1];
                        bModes[i] = static_cast<uint8_t>(d.tree(kBModeTree, &vp8::kKeyFrameBModeProbs[(a * 10 + l) * 9]));
                    }
                } else {
                    const uint8_t implied = mb.yMode == kDcPred ? kBDc : mb.yMode == kVPred ? kBVe : mb.yMode == kHPred ? kBHe : kBTm;
                    std::fill(bModes, bModes + 16, implied);
                }
                for (int i = 0; i < 4; i++) {
                    above[i] = bModes[12 + i];
                    leftModes[i] = bModes[i * 4 + 3];
                }
                const int uvMode = d.tree(kUvModeTree, kKeyFrameUvModeProbs);
                // The residue.
                std::memset(coefs, 0, sizeof coefs);
                std::array<uint8_t, 9>& an = aboveNz[static_cast<size_t>(mx)];
                const bool hasY2 = mb.yMode != kBPred;
                const int* q = quant_[mb.segment];
                bool any = false;
                if (skip) {
                    for (int i = 0; i < 8; i++) an[static_cast<size_t>(i)] = leftNz[static_cast<size_t>(i)] = 0;
                    if (hasY2) an[8] = leftNz[8] = 0;
                } else {
                    if (hasY2) {
                        const bool nz = block(tokens, 1, an[8] + leftNz[8], 0, q[2], q[3], coefs[24]);
                        an[8] = leftNz[8] = nz;
                        any |= nz;
                    }
                    for (int i = 0; i < 16; i++) {
                        const bool nz = block(tokens, hasY2 ? 0 : 3, an[static_cast<size_t>(i & 3)] + leftNz[static_cast<size_t>(i >> 2)], hasY2 ? 1 : 0, q[0], q[1], coefs[i]);
                        an[static_cast<size_t>(i & 3)] = leftNz[static_cast<size_t>(i >> 2)] = nz;
                        any |= nz;
                    }
                    for (int i = 0; i < 8; i++) {
                        const size_t ai = static_cast<size_t>(4 + (i >= 4 ? 2 : 0) + (i & 1)), li = static_cast<size_t>(4 + (i >= 4 ? 2 : 0) + ((i & 3) >> 1));
                        const bool nz = block(tokens, 2, an[ai] + leftNz[li], 0, q[4], q[5], coefs[16 + i]);
                        an[ai] = leftNz[li] = nz;
                        any |= nz;
                    }
                }
                mb.hasCoefficients = any;
                if (d.overrun() || tokens.overrun()) fail("its lossy data ends early");
                reconstruct(mx, my, mb.yMode, bModes, uvMode, coefs, hasY2 && !skip);
            }
            // The four pixels right of the last macroblock, on its bottom row: the next row's
            // last macroblock's above-right.
            const int last = my * 16 + 15;
            const uint8_t edge = *yAt(mbCols_ * 16 - 1, last);
            for (int k = 0; k < 4; k++) *yAt(mbCols_ * 16 + k, last) = edge;
        }
    }

    static uint8_t clamp255(int v) { return static_cast<uint8_t>(v < 0 ? 0 : v > 255 ? 255 : v); }

    // The inverse DCT of a 4x4 block added to the prediction in place (RFC 6386, 14.4).
    static void addIdct(const int16_t* in, uint8_t* dst, size_t stride) {
        int tmp[16];
        for (int i = 0; i < 4; i++) {
            const int a1 = in[i] + in[8 + i], b1 = in[i] - in[8 + i];
            int t1 = (in[4 + i] * 35468) >> 16, t2 = in[12 + i] + ((in[12 + i] * 20091) >> 16);
            const int c1 = t1 - t2;
            t1 = in[4 + i] + ((in[4 + i] * 20091) >> 16);
            t2 = (in[12 + i] * 35468) >> 16;
            const int d1 = t1 + t2;
            tmp[i] = a1 + d1;
            tmp[12 + i] = a1 - d1;
            tmp[4 + i] = b1 + c1;
            tmp[8 + i] = b1 - c1;
        }
        for (int i = 0; i < 4; i++) {
            const int* r = tmp + i * 4;
            const int a1 = r[0] + r[2], b1 = r[0] - r[2];
            int t1 = (r[1] * 35468) >> 16, t2 = r[3] + ((r[3] * 20091) >> 16);
            const int c1 = t1 - t2;
            t1 = r[1] + ((r[1] * 20091) >> 16);
            t2 = (r[3] * 35468) >> 16;
            const int d1 = t1 + t2;
            uint8_t* o = dst + static_cast<size_t>(i) * stride;
            o[0] = clamp255(o[0] + ((a1 + d1 + 4) >> 3));
            o[3] = clamp255(o[3] + ((a1 - d1 + 4) >> 3));
            o[1] = clamp255(o[1] + ((b1 + c1 + 4) >> 3));
            o[2] = clamp255(o[2] + ((b1 - c1 + 4) >> 3));
        }
    }

    // The inverse Walsh-Hadamard transform of Y2: each luma block's DC (RFC 6386, 14.3).
    static void walsh(const int16_t* in, int16_t coefs[25][16]) {
        int t[16];
        for (int i = 0; i < 4; i++) {
            const int a1 = in[i] + in[12 + i], b1 = in[4 + i] + in[8 + i], c1 = in[4 + i] - in[8 + i], d1 = in[i] - in[12 + i];
            t[i] = a1 + b1;
            t[4 + i] = c1 + d1;
            t[8 + i] = a1 - b1;
            t[12 + i] = d1 - c1;
        }
        for (int i = 0; i < 4; i++) {
            const int* r = t + i * 4;
            const int a1 = r[0] + r[3], b1 = r[1] + r[2], c1 = r[1] - r[2], d1 = r[0] - r[3];
            coefs[i * 4][0] = static_cast<int16_t>((a1 + b1 + 3) >> 3);
            coefs[i * 4 + 1][0] = static_cast<int16_t>((c1 + d1 + 3) >> 3);
            coefs[i * 4 + 2][0] = static_cast<int16_t>((a1 - b1 + 3) >> 3);
            coefs[i * 4 + 3][0] = static_cast<int16_t>((d1 - c1 + 3) >> 3);
        }
    }

    // A whole-block prediction (16x16 luma or 8x8 chroma) at `dst`, its edges in the plane. DC
    // averages only the edges inside the picture (128 where neither is).
    static void predictBlock(uint8_t* dst, size_t stride, int n, int mode, bool top, bool left) {
        const uint8_t* above = dst - stride;
        if (mode == kDcPred) {
            int sum = 0, shift = 0;
            if (!top) {
                for (int i = 0; i < n; i++) sum += above[i];
                shift++;
            }
            if (!left) {
                for (int i = 0; i < n; i++) sum += dst[static_cast<size_t>(i) * stride - 1];
                shift++;
            }
            const int log = n == 16 ? 4 : 3;
            const int dc = shift == 0 ? 128 : shift == 1 ? (sum + (1 << (log - 1))) >> log : (sum + (1 << log)) >> (log + 1);
            for (int y = 0; y < n; y++) std::memset(dst + static_cast<size_t>(y) * stride, dc, static_cast<size_t>(n));
            return;
        }
        for (int y = 0; y < n; y++) {
            uint8_t* row = dst + static_cast<size_t>(y) * stride;
            const int l = row[-1];
            for (int x = 0; x < n; x++) {
                if (mode == kVPred) row[x] = above[x];
                else if (mode == kHPred) row[x] = static_cast<uint8_t>(l);
                else row[x] = clamp255(l + above[x] - above[-1]);
            }
        }
    }

    // A 4x4 sub-block's prediction from its edges: A the 8 above (4 and the 4 above-right), L the
    // 4 to the left, P above-left.
    static void predictSub(uint8_t* dst, size_t stride, int mode, const uint8_t* A, const uint8_t* L, int P) {
        auto put = [&](int x, int y, int v) { dst[static_cast<size_t>(y) * stride + static_cast<size_t>(x)] = static_cast<uint8_t>(v); };
        auto avg3 = [](int a, int b, int c) { return (a + 2 * b + c + 2) >> 2; };
        auto avg2 = [](int a, int b) { return (a + b + 1) >> 1; };
        switch (mode) {
            case kBDc: {
                int sum = 4;
                for (int i = 0; i < 4; i++) sum += A[i] + L[i];
                for (int y = 0; y < 4; y++)
                    for (int x = 0; x < 4; x++) put(x, y, sum >> 3);
                break;
            }
            case kBTm:
                for (int y = 0; y < 4; y++)
                    for (int x = 0; x < 4; x++) put(x, y, clamp255(L[y] + A[x] - P));
                break;
            case kBVe:
                for (int x = 0; x < 4; x++) {
                    const int v = avg3(x == 0 ? P : A[x - 1], A[x], A[x + 1]);
                    for (int y = 0; y < 4; y++) put(x, y, v);
                }
                break;
            case kBHe: {
                const int v[4] = {avg3(P, L[0], L[1]), avg3(L[0], L[1], L[2]), avg3(L[1], L[2], L[3]), avg3(L[2], L[3], L[3])};
                for (int y = 0; y < 4; y++)
                    for (int x = 0; x < 4; x++) put(x, y, v[y]);
                break;
            }
            case kBLd:
                for (int y = 0; y < 4; y++)
                    for (int x = 0; x < 4; x++) {
                        const int i = x + y;
                        put(x, y, i < 6 ? avg3(A[i], A[i + 1], A[i + 2]) : avg3(A[6], A[7], A[7]));
                    }
                break;
            case kBRd: {
                // The edge from the bottom-left up and along: L3 L2 L1 L0 P A0 A1 A2 A3.
                const int e[9] = {L[3], L[2], L[1], L[0], P, A[0], A[1], A[2], A[3]};
                for (int y = 0; y < 4; y++)
                    for (int x = 0; x < 4; x++) {
                        const int i = 4 - y + x;
                        put(x, y, avg3(e[i - 1], e[i], e[i + 1]));
                    }
                break;
            }
            case kBVr: {
                const int e[9] = {L[3], L[2], L[1], L[0], P, A[0], A[1], A[2], A[3]};
                put(0, 3, avg3(e[1], e[2], e[3]));
                put(0, 2, avg3(e[2], e[3], e[4]));
                const int a = avg3(e[3], e[4], e[5]);
                put(0, 1, a), put(1, 3, a);
                const int b = avg2(e[4], e[5]);
                put(0, 0, b), put(1, 2, b);
                const int c = avg3(e[4], e[5], e[6]);
                put(1, 1, c), put(2, 3, c);
                const int dd = avg2(e[5], e[6]);
                put(1, 0, dd), put(2, 2, dd);
                const int f = avg3(e[5], e[6], e[7]);
                put(2, 1, f), put(3, 3, f);
                const int g = avg2(e[6], e[7]);
                put(2, 0, g), put(3, 2, g);
                put(3, 1, avg3(e[6], e[7], e[8]));
                put(3, 0, avg2(e[7], e[8]));
                break;
            }
            case kBVl:
                put(0, 0, avg2(A[0], A[1]));
                put(0, 1, avg3(A[0], A[1], A[2]));
                put(0, 2, avg2(A[1], A[2])), put(1, 0, avg2(A[1], A[2]));
                put(1, 1, avg3(A[1], A[2], A[3])), put(0, 3, avg3(A[1], A[2], A[3]));
                put(1, 2, avg2(A[2], A[3])), put(2, 0, avg2(A[2], A[3]));
                put(1, 3, avg3(A[2], A[3], A[4])), put(2, 1, avg3(A[2], A[3], A[4]));
                put(2, 2, avg2(A[3], A[4])), put(3, 0, avg2(A[3], A[4]));
                put(2, 3, avg3(A[3], A[4], A[5])), put(3, 1, avg3(A[3], A[4], A[5]));
                put(3, 2, avg3(A[4], A[5], A[6]));
                put(3, 3, avg3(A[5], A[6], A[7]));
                break;
            case kBHd: {
                const int e[9] = {L[3], L[2], L[1], L[0], P, A[0], A[1], A[2], A[3]};
                put(0, 3, avg2(e[0], e[1]));
                put(1, 3, avg3(e[0], e[1], e[2]));
                put(0, 2, avg2(e[1], e[2])), put(2, 3, avg2(e[1], e[2]));
                put(1, 2, avg3(e[1], e[2], e[3])), put(3, 3, avg3(e[1], e[2], e[3]));
                put(2, 2, avg2(e[2], e[3])), put(0, 1, avg2(e[2], e[3]));
                put(3, 2, avg3(e[2], e[3], e[4])), put(1, 1, avg3(e[2], e[3], e[4]));
                put(2, 1, avg2(e[3], e[4])), put(0, 0, avg2(e[3], e[4]));
                put(3, 1, avg3(e[3], e[4], e[5])), put(1, 0, avg3(e[3], e[4], e[5]));
                put(2, 0, avg3(e[4], e[5], e[6]));
                put(3, 0, avg3(e[5], e[6], e[7]));
                break;
            }
            case kBHu:
                put(0, 0, avg2(L[0], L[1]));
                put(1, 0, avg3(L[0], L[1], L[2]));
                put(2, 0, avg2(L[1], L[2])), put(0, 1, avg2(L[1], L[2]));
                put(3, 0, avg3(L[1], L[2], L[3])), put(1, 1, avg3(L[1], L[2], L[3]));
                put(2, 1, avg2(L[2], L[3])), put(0, 2, avg2(L[2], L[3]));
                put(3, 1, avg3(L[2], L[3], L[3])), put(1, 2, avg3(L[2], L[3], L[3]));
                put(2, 2, L[3]), put(3, 2, L[3]), put(0, 3, L[3]), put(1, 3, L[3]), put(2, 3, L[3]), put(3, 3, L[3]);
                break;
            default:
                break;
        }
    }

    void reconstruct(int mx, int my, int yMode, const uint8_t* bModes, int uvMode, int16_t coefs[25][16], bool walshDone) {
        uint8_t* yDst = yAt(mx * 16, my * 16);
        if (yMode == kBPred) {
            // The macroblock's above-right, for the sub-blocks of its right column.
            const uint8_t* aboveRight = yAt(mx * 16 + 16, my * 16 - 1);
            for (int i = 0; i < 16; i++) {
                const int bx = (i & 3) * 4, by = (i >> 2) * 4;
                uint8_t* dst = yDst + static_cast<size_t>(by) * yStride_ + static_cast<size_t>(bx);
                uint8_t A[8], L[4];
                const uint8_t* up = dst - yStride_;
                for (int k = 0; k < 4; k++) A[k] = up[k];
                for (int k = 0; k < 4; k++) A[4 + k] = (i & 3) == 3 ? aboveRight[k] : up[4 + k];
                for (int k = 0; k < 4; k++) L[k] = dst[static_cast<size_t>(k) * yStride_ - 1];
                predictSub(dst, yStride_, bModes[i], A, L, up[-1]);
                addIdct(coefs[i], dst, yStride_);
            }
        } else {
            predictBlock(yDst, yStride_, 16, yMode, my == 0, mx == 0);
            if (walshDone) walsh(coefs[24], coefs);
            for (int i = 0; i < 16; i++) addIdct(coefs[i], yDst + static_cast<size_t>((i >> 2) * 4) * yStride_ + static_cast<size_t>((i & 3) * 4), yStride_);
        }
        for (int plane = 0; plane < 2; plane++) {
            std::vector<uint8_t>& p = plane == 0 ? u_ : v_;
            uint8_t* dst = uvAt(p, mx * 8, my * 8);
            predictBlock(dst, uvStride_, 8, uvMode, my == 0, mx == 0);
            for (int i = 0; i < 4; i++) addIdct(coefs[16 + plane * 4 + i], dst + static_cast<size_t>((i >> 1) * 4) * uvStride_ + static_cast<size_t>((i & 1) * 4), uvStride_);
        }
    }

    // ---- The loop filter (RFC 6386, 15) ----

    static int s8(int x) { return x < -128 ? -128 : x > 127 ? 127 : x; }

    static void common(uint8_t* p, ptrdiff_t s, bool outer) {
        const int p1 = p[-2 * s], p0 = p[-s], q0 = p[0], q1 = p[s];
        int a = 3 * (q0 - p0);
        if (outer) a += s8(p1 - q1);
        a = s8(a);
        const int f1 = (a + 4 > 127 ? 127 : a + 4) >> 3, f2 = (a + 3 > 127 ? 127 : a + 3) >> 3;
        p[-s] = clamp255(p0 + f2);
        p[0] = clamp255(q0 - f1);
        if (!outer) {
            const int b = (f1 + 1) >> 1;
            p[-2 * s] = clamp255(p1 + b);
            p[s] = clamp255(q1 - b);
        }
    }
    static void mbEdge(uint8_t* p, ptrdiff_t s) {
        const int p2 = p[-3 * s], p1 = p[-2 * s], p0 = p[-s], q0 = p[0], q1 = p[s], q2 = p[2 * s];
        const int w = s8(s8(p1 - q1) + 3 * (q0 - p0));
        int a = (27 * w + 63) >> 7;
        p[-s] = clamp255(p0 + a);
        p[0] = clamp255(q0 - a);
        a = (18 * w + 63) >> 7;
        p[-2 * s] = clamp255(p1 + a);
        p[s] = clamp255(q1 - a);
        a = (9 * w + 63) >> 7;
        p[-3 * s] = clamp255(p2 + a);
        p[2 * s] = clamp255(q2 - a);
    }
    static bool simpleOk(const uint8_t* p, ptrdiff_t s, int limit) { return std::abs(p[-s] - p[0]) * 2 + (std::abs(p[-2 * s] - p[s]) >> 1) <= limit; }
    static bool normalOk(const uint8_t* p, ptrdiff_t s, int e, int i) {
        return simpleOk(p, s, 2 * e + i) && std::abs(p[-4 * s] - p[-3 * s]) <= i && std::abs(p[-3 * s] - p[-2 * s]) <= i && std::abs(p[-2 * s] - p[-s]) <= i &&
               std::abs(p[3 * s] - p[2 * s]) <= i && std::abs(p[2 * s] - p[s]) <= i && std::abs(p[s] - p[0]) <= i;
    }
    static bool highVariance(const uint8_t* p, ptrdiff_t s, int t) { return std::abs(p[-2 * s] - p[-s]) > t || std::abs(p[s] - p[0]) > t; }

    // An edge of `count` pixels: across it `s` apart, along it `along` apart.
    static void edge(uint8_t* p, ptrdiff_t s, ptrdiff_t along, int count, bool mb, int e, int i, int hev) {
        for (int k = 0; k < count; k++, p += along) {
            if (!normalOk(p, s, e, i)) continue;
            if (mb) {
                if (highVariance(p, s, hev)) common(p, s, true);
                else mbEdge(p, s);
            } else {
                common(p, s, highVariance(p, s, hev));
            }
        }
    }

    void loopFilter() {
        if (filterLevel_ == 0) return;
        const ptrdiff_t ys = static_cast<ptrdiff_t>(yStride_), us = static_cast<ptrdiff_t>(uvStride_);
        for (int my = 0; my < mbRows_; my++)
            for (int mx = 0; mx < mbCols_; mx++) {
                const MbInfo& mb = info_[static_cast<size_t>(my * mbCols_ + mx)];
                int level = filterLevel_;
                if (segmentation_) level = segmentAbsolute_ ? segmentFilter_[mb.segment] : level + segmentFilter_[mb.segment];
                level = std::clamp(level, 0, 63);
                if (filterDeltas_) {
                    level += refDelta_[0];  // the current frame's: every key frame macroblock is intra
                    if (mb.yMode == kBPred) level += modeDelta_[0];
                    level = std::clamp(level, 0, 63);
                }
                if (level == 0) continue;
                int interior = level;
                if (sharpness_) {
                    interior >>= sharpness_ > 4 ? 2 : 1;
                    if (interior > 9 - sharpness_) interior = 9 - sharpness_;
                }
                if (interior < 1) interior = 1;
                const int hev = level >= 40 ? 2 : level >= 15 ? 1 : 0;
                const bool inner = mb.hasCoefficients || mb.yMode == kBPred;
                uint8_t* y = yAt(mx * 16, my * 16);
                uint8_t* u = uvAt(u_, mx * 8, my * 8);
                uint8_t* v = uvAt(v_, mx * 8, my * 8);
                if (simpleFilter_) {
                    const int mbLimit = (level + 2) * 2 + interior, bLimit = level * 2 + interior;
                    auto simple = [&](uint8_t* p, ptrdiff_t s, ptrdiff_t along, int limit) {
                        for (int k = 0; k < 16; k++, p += along)
                            if (simpleOk(p, s, limit)) common(p, s, true);
                    };
                    if (mx > 0) simple(y, 1, ys, mbLimit);
                    if (inner)
                        for (int k = 4; k < 16; k += 4) simple(y + k, 1, ys, bLimit);
                    if (my > 0) simple(y, ys, 1, mbLimit);
                    if (inner)
                        for (int k = 4; k < 16; k += 4) simple(y + k * ys, ys, 1, bLimit);
                    continue;
                }
                if (mx > 0) {
                    edge(y, 1, ys, 16, true, level + 2, interior, hev);
                    edge(u, 1, us, 8, true, level + 2, interior, hev);
                    edge(v, 1, us, 8, true, level + 2, interior, hev);
                }
                if (inner) {
                    for (int k = 4; k < 16; k += 4) edge(y + k, 1, ys, 16, false, level, interior, hev);
                    edge(u + 4, 1, us, 8, false, level, interior, hev);
                    edge(v + 4, 1, us, 8, false, level, interior, hev);
                }
                if (my > 0) {
                    edge(y, ys, 1, 16, true, level + 2, interior, hev);
                    edge(u, us, 1, 8, true, level + 2, interior, hev);
                    edge(v, us, 1, 8, true, level + 2, interior, hev);
                }
                if (inner) {
                    for (int k = 4; k < 16; k += 4) edge(y + k * ys, ys, 1, 16, false, level, interior, hev);
                    edge(u + 4 * us, us, 1, 8, false, level, interior, hev);
                    edge(v + 4 * us, us, 1, 8, false, level, interior, hev);
                }
            }
    }

    // ---- To RGBA: the chroma twice the size by the triangle filter, BT.601's limited range ----

    static int clip8(int v) { return (v & ~((256 << 6) - 1)) == 0 ? v >> 6 : v < 0 ? 0 : 255; }

    std::vector<uint8_t> toRgba(int width, int height) {
        const int cw = (width + 1) / 2, ch = (height + 1) / 2;
        std::vector<uint8_t> out(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
        auto chroma = [&](std::vector<uint8_t>& plane, int x, int y) -> int {
            return *uvAt(plane, std::clamp(x, 0, cw - 1), std::clamp(y, 0, ch - 1));
        };
        for (int y = 0; y < height; y++) {
            const int sy = y >> 1, ny = (y & 1) ? sy + 1 : sy - 1;
            for (int x = 0; x < width; x++) {
                const int sx = x >> 1, nx = (x & 1) ? sx + 1 : sx - 1;
                auto up = [&](std::vector<uint8_t>& p) {
                    return (9 * chroma(p, sx, sy) + 3 * chroma(p, nx, sy) + 3 * chroma(p, sx, ny) + chroma(p, nx, ny) + 8) >> 4;
                };
                const int yy = *yAt(x, y), uu = up(u_), vv = up(v_);
                const int base = (yy * 19077) >> 8;
                uint8_t* o = &out[(static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 4];
                o[0] = static_cast<uint8_t>(clip8(base + ((vv * 26149) >> 8) - 14234));
                o[1] = static_cast<uint8_t>(clip8(base - ((uu * 6419) >> 8) - ((vv * 13320) >> 8) + 8708));
                o[2] = static_cast<uint8_t>(clip8(base + ((uu * 33050) >> 8) - 17685));
                o[3] = 255;
            }
        }
        return out;
    }
};


// ---- VP8: writing ----

// The boolean encoder (RFC 6386, 7.3).
struct BoolEncoder {
    std::vector<uint8_t> out;
    uint32_t range = 255, bottom = 0;
    int bitCount = 24;

    void carry() {
        for (size_t i = out.size(); i-- > 0;) {
            if (out[i] != 255) {
                out[i]++;
                return;
            }
            out[i] = 0;
        }
    }
    void write(int prob, int bit) {
        const uint32_t split = 1 + (((range - 1) * static_cast<uint32_t>(prob)) >> 8);
        if (bit) {
            bottom += split;
            range -= split;
        } else {
            range = split;
        }
        while (range < 128) {
            range <<= 1;
            if (bottom & (1u << 31)) carry();
            bottom <<= 1;
            if (!--bitCount) {
                out.push_back(static_cast<uint8_t>(bottom >> 24));
                bottom &= (1u << 24) - 1;
                bitCount = 8;
            }
        }
    }
    void literal(int value, int n) {
        for (int i = n - 1; i >= 0; i--) write(128, (value >> i) & 1);
    }
    void flush() {
        int c = bitCount;
        uint32_t v = bottom;
        if (v & (1u << (32 - c))) carry();
        v <<= c & 7;
        c >>= 3;
        while (--c >= 0) v <<= 8;
        for (c = 0; c < 4; c++) {
            out.push_back(static_cast<uint8_t>(v >> 24));
            v <<= 8;
        }
    }
    // A tree's leaf: the path to it found and written, each node's bit with its probability.
    void tree(const int8_t* t, const uint8_t* probs, int leaf) {
        int path[16], depth = 0;
        auto find = [&](auto&& self, int i, int d) -> bool {
            for (int b = 0; b < 2; b++) {
                const int next = t[i + b];
                path[d] = b;
                if (next <= 0 ? -next == leaf : self(self, next, d + 1)) {
                    depth = next <= 0 ? d + 1 : depth;
                    return true;
                }
            }
            return false;
        };
        find(find, 0, 0);
        int i = 0;
        for (int d = 0; d < depth; d++) {
            write(probs[i >> 1], path[d]);
            i = t[i + path[d]];
        }
    }
};

class LossyWriter {
public:
    // `rgba` width by height, at `quality` 0 to 1 (0 smallest, 1 best).
    LossyWriter(const uint8_t* rgba, int width, int height, double quality) : w_(width), h_(height) {
        mbCols_ = (width + 15) / 16;
        mbRows_ = (height + 15) / 16;
        // A quantizer index from the quality: 0 the finest, 127 the coarsest. Between these points
        // a quality gives about the size and loss libwebp gives at 100 times it.
        static const double kQuality[] = {0, 0.1, 0.5, 0.75, 0.9, 0.95, 1}, kIndex[] = {127, 78, 50, 31, 12.5, 5.7, 0};
        const double q = std::clamp(quality, 0.0, 1.0);
        int k = 0;
        while (k < 5 && q > kQuality[k + 1]) k++;
        const double t = (q - kQuality[k]) / (kQuality[k + 1] - kQuality[k]);
        qi_ = std::clamp(static_cast<int>(std::lround(kIndex[k] + t * (kIndex[k + 1] - kIndex[k]))), 0, 127);
        auto dc = [](int i) { return static_cast<int>(vp8::kDcQuant[std::clamp(i, 0, 127)]); };
        auto ac = [](int i) { return static_cast<int>(vp8::kAcQuant[std::clamp(i, 0, 127)]); };
        step_[0] = dc(qi_);
        step_[1] = ac(qi_);
        step_[2] = dc(qi_) * 2;
        step_[3] = std::max(ac(qi_) * 155 / 100, 8);
        step_[4] = std::min(dc(qi_), 132);
        step_[5] = ac(qi_);
        filterLevel_ = std::clamp(qi_ * 2 / 5, 0, 63);
        toYuv(rgba);
    }

    std::vector<uint8_t> write() {
        const size_t w16 = static_cast<size_t>(mbCols_) * 16, h16 = static_cast<size_t>(mbRows_) * 16;
        yStride_ = w16 + 1 + 4;
        uvStride_ = w16 / 2 + 1;
        y_.assign(yStride_ * (h16 + 1), 0);
        u_.assign(uvStride_ * (h16 / 2 + 1), 0);
        v_.assign(uvStride_ * (h16 / 2 + 1), 0);
        std::memset(y_.data(), 127, yStride_);
        std::memset(u_.data(), 127, uvStride_);
        std::memset(v_.data(), 127, uvStride_);
        for (size_t r = 1; r <= h16; r++) y_[r * yStride_] = 129;
        for (size_t r = 1; r <= h16 / 2; r++) u_[r * uvStride_] = v_[r * uvStride_] = 129;
        mbs_.resize(static_cast<size_t>(mbCols_ * mbRows_));
        for (int my = 0; my < mbRows_; my++) {
            for (int mx = 0; mx < mbCols_; mx++) encodeMb(mx, my, mbs_[static_cast<size_t>(my * mbCols_ + mx)]);
            const int last = my * 16 + 15;
            const uint8_t edge = *yAt(mbCols_ * 16 - 1, last);
            for (int k = 0; k < 4; k++) *yAt(mbCols_ * 16 + k, last) = edge;
        }
        return bitstream();
    }

private:
    int w_, h_, mbCols_, mbRows_, qi_ = 0, filterLevel_ = 0;
    int step_[6];  // y dc, y ac, y2 dc, y2 ac, uv dc, uv ac
    std::vector<uint8_t> srcY_, srcU_, srcV_;  // the picture as YUV, padded to whole macroblocks
    std::vector<uint8_t> y_, u_, v_;           // the reconstruction, laid out as the decoder's
    size_t yStride_ = 0, uvStride_ = 0;
    struct Mb {
        uint8_t yMode = kDcPred, uvMode = kDcPred;
        uint8_t bModes[16] = {};
        int16_t coefs[25][16] = {};  // quantized, natural order
        bool hasY2 = true;
    };
    std::vector<Mb> mbs_;

    uint8_t* yAt(int x, int y) { return &y_[static_cast<size_t>(y + 1) * yStride_ + static_cast<size_t>(x + 1)]; }
    uint8_t* uvAt(std::vector<uint8_t>& plane, int x, int y) { return &plane[static_cast<size_t>(y + 1) * uvStride_ + static_cast<size_t>(x + 1)]; }

    // RGB to YUV, BT.601's limited range, the chroma the average of each 2 by 2; edges repeated
    // out to whole macroblocks.
    void toYuv(const uint8_t* rgba) {
        const int W = mbCols_ * 16, H = mbRows_ * 16;
        srcY_.assign(static_cast<size_t>(W * H), 0);
        srcU_.assign(static_cast<size_t>(W * H / 4), 0);
        srcV_.assign(static_cast<size_t>(W * H / 4), 0);
        auto px = [&](int x, int y) { return rgba + (static_cast<size_t>(std::min(y, h_ - 1)) * static_cast<size_t>(w_) + static_cast<size_t>(std::min(x, w_ - 1))) * 4; };
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                const uint8_t* p = px(x, y);
                srcY_[static_cast<size_t>(y * W + x)] = static_cast<uint8_t>((16839 * p[0] + 33059 * p[1] + 6420 * p[2] + (16 << 16) + (1 << 15)) >> 16);
            }
        for (int y = 0; y < H / 2; y++)
            for (int x = 0; x < W / 2; x++) {
                int r = 0, g = 0, b = 0;
                for (int k = 0; k < 4; k++) {
                    const uint8_t* p = px(x * 2 + (k & 1), y * 2 + (k >> 1));
                    r += p[0];
                    g += p[1];
                    b += p[2];
                }
                srcU_[static_cast<size_t>(y * W / 2 + x)] = static_cast<uint8_t>(std::clamp((-9719 * r - 19081 * g + 28800 * b + (128 << 18) + (1 << 17)) >> 18, 0, 255));
                srcV_[static_cast<size_t>(y * W / 2 + x)] = static_cast<uint8_t>(std::clamp((28800 * r - 24116 * g - 4684 * b + (128 << 18) + (1 << 17)) >> 18, 0, 255));
            }
    }

    // The 4x4 forward DCT, the decoder's inverse undone: twice the orthonormal DCT.
    static void fdct(const int* in, double* out) {
        static const auto basis = [] {
            std::array<double, 16> c{};
            for (int k = 0; k < 4; k++)
                for (int n = 0; n < 4; n++) c[static_cast<size_t>(k * 4 + n)] = (k == 0 ? 0.5 : std::sqrt(0.5)) * fmath::cos((2 * n + 1) * k * 3.14159265358979323846 / 8);
            return c;
        }();
        double tmp[16];
        for (int y = 0; y < 4; y++)
            for (int u = 0; u < 4; u++) {
                double s = 0;
                for (int x = 0; x < 4; x++) s += basis[static_cast<size_t>(u * 4 + x)] * in[y * 4 + x];
                tmp[y * 4 + u] = s;
            }
        for (int v = 0; v < 4; v++)
            for (int u = 0; u < 4; u++) {
                double s = 0;
                for (int y = 0; y < 4; y++) s += basis[static_cast<size_t>(v * 4 + y)] * tmp[y * 4 + u];
                out[v * 4 + u] = 2 * s;
            }
    }

    // Quantized: rounded, with a dead zone for the AC coefficients so that small ones go.
    static int quantize(double v, int step, bool ac) {
        const double a = std::abs(v) / step + (ac ? 0.38 : 0.5);
        const int q = std::min(static_cast<int>(a), 2048 + 66);
        return v < 0 ? -q : q;
    }

    static int64_t sse(const uint8_t* a, size_t as, const uint8_t* b, size_t bs, int n) {
        int64_t s = 0;
        for (int y = 0; y < n; y++)
            for (int x = 0; x < n; x++) {
                const int d = a[static_cast<size_t>(y) * as + static_cast<size_t>(x)] - b[static_cast<size_t>(y) * bs + static_cast<size_t>(x)];
                s += d * d;
            }
        return s;
    }

    // A 4x4 block's residue (source less prediction) transformed and quantized into `q`, and
    // reconstructed into `dst` over the prediction; how many coefficients are not zero.
    int codeBlock(const uint8_t* src, size_t ss, uint8_t* dst, size_t ds, int dcStep, int acStep, int16_t* q) {
        int residue[16];
        for (int y = 0; y < 4; y++)
            for (int x = 0; x < 4; x++) residue[y * 4 + x] = src[static_cast<size_t>(y) * ss + static_cast<size_t>(x)] - dst[static_cast<size_t>(y) * ds + static_cast<size_t>(x)];
        double f[16];
        fdct(residue, f);
        int nz = 0;
        int16_t deq[16];
        for (int i = 0; i < 16; i++) {
            q[i] = static_cast<int16_t>(quantize(f[i], i == 0 ? dcStep : acStep, i > 0));
            deq[i] = static_cast<int16_t>(q[i] * (i == 0 ? dcStep : acStep));
            nz += q[i] != 0;
        }
        Lossy::addIdct(deq, dst, ds);
        return nz;
    }

    void encodeMb(int mx, int my, Mb& mb) {
        const size_t sw = static_cast<size_t>(mbCols_) * 16;
        const uint8_t* src = &srcY_[static_cast<size_t>(my * 16) * sw + static_cast<size_t>(mx * 16)];
        uint8_t* dst = yAt(mx * 16, my * 16);
        const double lambda = 0.25 * step_[1] * step_[1];
        // Each 16x16 mode's prediction; the best kept.
        uint8_t pred[16 * 16], best16[16 * 16];
        int mode16 = kDcPred;
        int64_t bestPrediction = INT64_MAX;
        for (int m = kDcPred; m <= kTmPred; m++) {
            // Predicted in place, then copied out: the decoder predicts from the frame's edges.
            Lossy::predictBlock(dst, yStride_, 16, m, my == 0, mx == 0);
            for (int y = 0; y < 16; y++) std::memcpy(pred + y * 16, dst + static_cast<size_t>(y) * yStride_, 16);
            const int64_t e = sse(src, sw, pred, 16, 16);
            if (e < bestPrediction) {
                bestPrediction = e;
                mode16 = m;
                std::memcpy(best16, pred, sizeof pred);
            }
        }
        // 16x16: each block's AC, and the DCs together through Y2.
        Mb i16;
        i16.yMode = static_cast<uint8_t>(mode16);
        i16.hasY2 = true;
        uint8_t recon16[16 * 16];
        std::memcpy(recon16, best16, sizeof recon16);
        double dcs[16];
        int nz16 = 0;
        for (int b = 0; b < 16; b++) {
            const int bx = (b & 3) * 4, by = (b >> 2) * 4;
            int residue[16];
            for (int y = 0; y < 4; y++)
                for (int x = 0; x < 4; x++)
                    residue[y * 4 + x] = src[static_cast<size_t>(by + y) * sw + static_cast<size_t>(bx + x)] - recon16[(by + y) * 16 + bx + x];
            double f[16];
            fdct(residue, f);
            dcs[b] = f[0];
            for (int i = 1; i < 16; i++) {
                i16.coefs[b][i] = static_cast<int16_t>(quantize(f[i], step_[1], true));
                nz16 += i16.coefs[b][i] != 0;
            }
        }
        // Y2: the DCs' Walsh-Hadamard transform, the decoder's inverse undone (M' D M / 2).
        static const int M[4][4] = {{1, 1, 1, 1}, {1, 1, -1, -1}, {1, -1, -1, 1}, {1, -1, 1, -1}};
        double t[16], wht[16];
        for (int r = 0; r < 4; r++)
            for (int c = 0; c < 4; c++) {
                double s = 0;
                for (int k = 0; k < 4; k++) s += M[k][r] * dcs[k * 4 + c];
                t[r * 4 + c] = s;
            }
        for (int r = 0; r < 4; r++)
            for (int c = 0; c < 4; c++) {
                double s = 0;
                for (int k = 0; k < 4; k++) s += t[r * 4 + k] * M[k][c];
                wht[r * 4 + c] = s / 2;
            }
        for (int i = 0; i < 16; i++) {
            i16.coefs[24][i] = static_cast<int16_t>(quantize(wht[i], i == 0 ? step_[2] : step_[3], i > 0));
            nz16 += i16.coefs[24][i] != 0;
        }
        // Reconstructed as the decoder will.
        {
            int16_t deq[25][16] = {};
            for (int i = 0; i < 16; i++) deq[24][i] = static_cast<int16_t>(i16.coefs[24][i] * (i == 0 ? step_[2] : step_[3]));
            Lossy::walsh(deq[24], deq);
            for (int b = 0; b < 16; b++) {
                for (int i = 1; i < 16; i++) deq[b][i] = static_cast<int16_t>(i16.coefs[b][i] * step_[1]);
                Lossy::addIdct(deq[b], recon16 + (b >> 2) * 4 * 16 + (b & 3) * 4, 16);
            }
        }
        const double cost16 = static_cast<double>(sse(src, sw, recon16, 16, 16)) + lambda * (nz16 + 2);
        // 4x4: each sub-block's best mode, coded and reconstructed in place before the next.
        Mb i4;
        i4.yMode = kBPred;
        i4.hasY2 = false;
        int nz4 = 0;
        const uint8_t* aboveRight = yAt(mx * 16 + 16, my * 16 - 1);
        for (int b = 0; b < 16; b++) {
            const int bx = (b & 3) * 4, by = (b >> 2) * 4;
            uint8_t* d = dst + static_cast<size_t>(by) * yStride_ + static_cast<size_t>(bx);
            const uint8_t* s = src + static_cast<size_t>(by) * sw + static_cast<size_t>(bx);
            uint8_t A[8], L[4];
            const uint8_t* up = d - yStride_;
            for (int k = 0; k < 4; k++) A[k] = up[k];
            for (int k = 0; k < 4; k++) A[4 + k] = (b & 3) == 3 ? aboveRight[k] : up[4 + k];
            for (int k = 0; k < 4; k++) L[k] = d[static_cast<size_t>(k) * yStride_ - 1];
            int bestMode = kBDc;
            int64_t bestError = INT64_MAX;
            for (int m = kBDc; m <= kBHu; m++) {
                Lossy::predictSub(d, yStride_, m, A, L, up[-1]);
                const int64_t e = sse(s, sw, d, yStride_, 4) + (m == kBDc ? 0 : static_cast<int64_t>(lambda * 0.15));
                if (e < bestError) {
                    bestError = e;
                    bestMode = m;
                }
            }
            Lossy::predictSub(d, yStride_, bestMode, A, L, up[-1]);
            i4.bModes[b] = static_cast<uint8_t>(bestMode);
            nz4 += codeBlock(s, sw, d, yStride_, step_[0], step_[1], i4.coefs[b]);
        }
        const double cost4 = static_cast<double>(sse(src, sw, dst, yStride_, 16)) + lambda * (nz4 + 6);
        if (cost16 <= cost4) {
            for (int y = 0; y < 16; y++) std::memcpy(dst + static_cast<size_t>(y) * yStride_, recon16 + y * 16, 16);
            mb = i16;
        } else {
            mb = i4;
        }
        // Chroma: the best of the four modes for U and V together.
        const size_t cs = sw / 2;
        const uint8_t* su = &srcU_[static_cast<size_t>(my * 8) * cs + static_cast<size_t>(mx * 8)];
        const uint8_t* sv = &srcV_[static_cast<size_t>(my * 8) * cs + static_cast<size_t>(mx * 8)];
        uint8_t* du = uvAt(u_, mx * 8, my * 8);
        uint8_t* dv = uvAt(v_, mx * 8, my * 8);
        int uvMode = kDcPred;
        int64_t bestUv = INT64_MAX;
        for (int m = kDcPred; m <= kTmPred; m++) {
            Lossy::predictBlock(du, uvStride_, 8, m, my == 0, mx == 0);
            Lossy::predictBlock(dv, uvStride_, 8, m, my == 0, mx == 0);
            const int64_t e = sse(su, cs, du, uvStride_, 8) + sse(sv, cs, dv, uvStride_, 8);
            if (e < bestUv) {
                bestUv = e;
                uvMode = m;
            }
        }
        mb.uvMode = static_cast<uint8_t>(uvMode);
        Lossy::predictBlock(du, uvStride_, 8, uvMode, my == 0, mx == 0);
        Lossy::predictBlock(dv, uvStride_, 8, uvMode, my == 0, mx == 0);
        for (int i = 0; i < 4; i++) {
            const size_t off = static_cast<size_t>((i >> 1) * 4), offx = static_cast<size_t>((i & 1) * 4);
            codeBlock(su + off * cs + offx, cs, du + off * uvStride_ + offx, uvStride_, step_[4], step_[5], mb.coefs[16 + i]);
            codeBlock(sv + off * cs + offx, cs, dv + off * uvStride_ + offx, uvStride_, step_[4], step_[5], mb.coefs[20 + i]);
        }
    }

    // A block's tokens, under the default probabilities; whether it had any.
    static bool tokens(BoolEncoder& e, int type, int context, int first, const int16_t* q) {
        int last = -1;
        for (int c = 15; c >= first; c--)
            if (q[kZigzag[c]] != 0) {
                last = c;
                break;
            }
        int c = first;
        const uint8_t* p = vp8::kDefaultCoeffProbs + ((type * 8 + kBands[c]) * 3 + context) * 11;
        auto probsAt = [&](int at, int ctx) { return vp8::kDefaultCoeffProbs + ((type * 8 + kBands[at]) * 3 + ctx) * 11; };
        if (last < 0) {
            e.write(p[0], 0);
            return false;
        }
        e.write(p[0], 1);
        for (;;) {
            const int v = q[kZigzag[c]];
            if (v == 0) {
                e.write(p[1], 0);
                c++;
                p = probsAt(c, 0);
                continue;
            }
            e.write(p[1], 1);
            const int a = std::abs(v);
            int next = 2;
            if (a == 1) {
                e.write(p[2], 0);
                next = 1;
            } else {
                e.write(p[2], 1);
                if (a <= 4) {
                    e.write(p[3], 0);
                    if (a == 2) {
                        e.write(p[4], 0);
                    } else {
                        e.write(p[4], 1);
                        e.write(p[5], a == 4);
                    }
                } else {
                    e.write(p[3], 1);
                    if (a <= 10) {
                        e.write(p[6], 0);
                        if (a <= 6) {
                            e.write(p[7], 0);
                            e.write(159, a - 5);
                        } else {
                            e.write(p[7], 1);
                            e.write(165, (a - 7) >> 1);
                            e.write(145, (a - 7) & 1);
                        }
                    } else {
                        e.write(p[6], 1);
                        const uint8_t* extra;
                        int base, bits;
                        if (a <= 34) {
                            e.write(p[8], 0);
                            if (a <= 18) e.write(p[9], 0), extra = kCat3, base = 11, bits = 3;
                            else e.write(p[9], 1), extra = kCat4, base = 19, bits = 4;
                        } else {
                            e.write(p[8], 1);
                            if (a <= 66) e.write(p[10], 0), extra = kCat5, base = 35, bits = 5;
                            else e.write(p[10], 1), extra = kCat6, base = 67, bits = 11;
                        }
                        for (int k = 0; k < bits; k++) e.write(extra[k], ((a - base) >> (bits - 1 - k)) & 1);
                    }
                }
            }
            e.write(128, v < 0);
            if (++c == 16) return true;
            p = probsAt(c, next);
            if (c > last) {
                e.write(p[0], 0);
                return true;
            }
            e.write(p[0], 1);
        }
    }

    std::vector<uint8_t> bitstream() {
        // How often a macroblock has no coefficients, for the probability of the skip flag.
        auto empty = [](const Mb& mb) {
            for (int b = 0; b < 25; b++) {
                if (b == 24 && !mb.hasY2) continue;
                for (int i = 0; i < 16; i++)
                    if (mb.coefs[b][i] != 0) return false;
            }
            return true;
        };
        int skipped = 0;
        for (const Mb& mb : mbs_) skipped += empty(mb);
        const int skipProb = std::clamp(255 - 255 * skipped / std::max<int>(1, static_cast<int>(mbs_.size())), 1, 254);
        BoolEncoder head, toks;
        head.literal(0, 2);  // colour space, clamping
        head.write(128, 0);  // no segmentation
        head.write(128, 0);  // the normal loop filter
        head.literal(filterLevel_, 6);
        head.literal(0, 3);  // sharpness
        head.write(128, 0);  // no filter adjustments
        head.literal(0, 2);  // one token partition
        head.literal(qi_, 7);
        for (int i = 0; i < 5; i++) head.write(128, 0);  // no quantizer deltas
        head.write(128, 0);                              // refresh entropy probabilities
        for (int i = 0; i < 1056; i++) head.write(vp8::kCoeffUpdateProbs[i], 0);
        head.write(128, 1);  // skipping macroblocks with no coefficients
        head.literal(skipProb, 8);
        std::vector<uint8_t> aboveModes(static_cast<size_t>(mbCols_) * 4, kBDc);
        std::vector<std::array<uint8_t, 9>> aboveNz(static_cast<size_t>(mbCols_));
        for (int my = 0; my < mbRows_; my++) {
            uint8_t leftModes[4] = {kBDc, kBDc, kBDc, kBDc};
            std::array<uint8_t, 9> leftNz{};
            for (int mx = 0; mx < mbCols_; mx++) {
                const Mb& mb = mbs_[static_cast<size_t>(my * mbCols_ + mx)];
                const bool skip = empty(mb);
                head.write(skipProb, skip);
                head.tree(kKeyFrameYModeTree, kKeyFrameYModeProbs, mb.yMode);
                uint8_t* above = &aboveModes[static_cast<size_t>(mx) * 4];
                uint8_t bModes[16];
                if (mb.yMode == kBPred) {
                    for (int i = 0; i < 16; i++) {
                        const uint8_t a = i < 4 ? above[i] : bModes[i - 4];
                        const uint8_t l = (i & 3) == 0 ? leftModes[i >> 2] : bModes[i - 1];
                        bModes[i] = mb.bModes[i];
                        head.tree(kBModeTree, &vp8::kKeyFrameBModeProbs[(a * 10 + l) * 9], bModes[i]);
                    }
                } else {
                    const uint8_t implied = mb.yMode == kDcPred ? kBDc : mb.yMode == kVPred ? kBVe : mb.yMode == kHPred ? kBHe : kBTm;
                    std::fill(bModes, bModes + 16, implied);
                }
                for (int i = 0; i < 4; i++) {
                    above[i] = bModes[12 + i];
                    leftModes[i] = bModes[i * 4 + 3];
                }
                head.tree(kUvModeTree, kKeyFrameUvModeProbs, mb.uvMode);
                std::array<uint8_t, 9>& an = aboveNz[static_cast<size_t>(mx)];
                if (skip) {
                    for (int i = 0; i < 8; i++) an[static_cast<size_t>(i)] = leftNz[static_cast<size_t>(i)] = 0;
                    if (mb.hasY2) an[8] = leftNz[8] = 0;
                    continue;
                }
                if (mb.hasY2) an[8] = leftNz[8] = tokens(toks, 1, an[8] + leftNz[8], 0, mb.coefs[24]);
                for (int i = 0; i < 16; i++) {
                    const bool nz = tokens(toks, mb.hasY2 ? 0 : 3, an[static_cast<size_t>(i & 3)] + leftNz[static_cast<size_t>(i >> 2)], mb.hasY2 ? 1 : 0, mb.coefs[i]);
                    an[static_cast<size_t>(i & 3)] = leftNz[static_cast<size_t>(i >> 2)] = nz;
                }
                for (int i = 0; i < 8; i++) {
                    const size_t ai = static_cast<size_t>(4 + (i >= 4 ? 2 : 0) + (i & 1)), li = static_cast<size_t>(4 + (i >= 4 ? 2 : 0) + ((i & 3) >> 1));
                    const bool nz = tokens(toks, 2, an[ai] + leftNz[li], 0, mb.coefs[16 + i]);
                    an[ai] = leftNz[li] = nz;
                }
            }
        }
        head.flush();
        toks.flush();
        // The frame: its tag (a key frame, shown, the first partition's size), start code, size.
        std::vector<uint8_t> out;
        const uint32_t tag = (static_cast<uint32_t>(head.out.size()) << 5) | (1u << 4);
        out.push_back(static_cast<uint8_t>(tag));
        out.push_back(static_cast<uint8_t>(tag >> 8));
        out.push_back(static_cast<uint8_t>(tag >> 16));
        out.insert(out.end(), {0x9D, 0x01, 0x2A, static_cast<uint8_t>(w_), static_cast<uint8_t>(w_ >> 8), static_cast<uint8_t>(h_), static_cast<uint8_t>(h_ >> 8)});
        out.insert(out.end(), head.out.begin(), head.out.end());
        out.insert(out.end(), toks.out.begin(), toks.out.end());
        return out;
    }
};
// Alpha's chunk (ALPH) put into RGBA pixels: raw or lossless, then unfiltered.
void applyAlpha(const uint8_t* data, size_t size, std::vector<uint8_t>& rgba, int width, int height) {
    if (size < 1) throw Vp8Error{"its alpha is cut short"};
    const int method = data[0] & 3, filter = (data[0] >> 2) & 3;
    const size_t n = static_cast<size_t>(width) * static_cast<size_t>(height);
    std::vector<uint8_t> alpha(n);
    if (method == 0) {
        if (size - 1 < n) throw Vp8Error{"its alpha is cut short"};
        std::memcpy(alpha.data(), data + 1, n);
    } else if (method == 1) {
        std::vector<uint32_t> argb;
        int w = width, h = height;
        try {
            Lossless(data + 1, size - 1).read(argb, w, h, true);
        } catch (const LosslessError& e) {
            throw Vp8Error{"its alpha: " + e.what};
        }
        for (size_t i = 0; i < n; i++) alpha[i] = static_cast<uint8_t>(argb[i] >> 8);
    } else {
        throw Vp8Error{"alpha compressed in a way there is none of"};
    }
    // The filters undone: each value had its prediction (left, above, or left + above - above-left) taken off.
    if (filter != 0) {
        for (int y = 0; y < height; y++)
            for (int x = 0; x < width; x++) {
                const size_t at = static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x);
                int prediction;
                if (x == 0 && y == 0) prediction = 0;
                else if (y == 0) prediction = alpha[at - 1];
                else if (x == 0) prediction = alpha[at - static_cast<size_t>(width)];
                else if (filter == 1) prediction = alpha[at - 1];
                else if (filter == 2) prediction = alpha[at - static_cast<size_t>(width)];
                else prediction = std::clamp(alpha[at - 1] + alpha[at - static_cast<size_t>(width)] - alpha[at - static_cast<size_t>(width) - 1], 0, 255);
                alpha[at] = static_cast<uint8_t>(alpha[at] + prediction);
            }
    }
    for (size_t i = 0; i < n; i++) rgba[i * 4 + 3] = alpha[i];
}

// ---- VP8L: writing ----

// Bits lowest first.
struct LBitsOut {
    std::vector<uint8_t> out;
    uint64_t buffer = 0;
    int count = 0;
    void put(uint32_t value, int n) {
        if (n == 0) return;
        buffer |= static_cast<uint64_t>(value & ((n == 32 ? 0xFFFFFFFFu : (1u << n) - 1))) << count;
        count += n;
        while (count >= 8) {
            out.push_back(static_cast<uint8_t>(buffer));
            buffer >>= 8;
            count -= 8;
        }
    }
    void finish() {
        if (count > 0) out.push_back(static_cast<uint8_t>(buffer));
        buffer = 0;
        count = 0;
    }
};

// Code lengths for counts, none longer than `most`: Huffman's, then the deepest shortened and
// others lengthened to make room, the rarest given the longest (as codec.cpp's for deflate).
std::vector<int> limitedLengths(const std::vector<uint32_t>& counts, int most) {
    const size_t n = counts.size();
    std::vector<int> lengths(n, 0);
    std::vector<std::pair<uint32_t, int>> used;
    for (size_t i = 0; i < n; i++)
        if (counts[i] != 0) used.push_back({counts[i], static_cast<int>(i)});
    if (used.empty()) return lengths;
    if (used.size() == 1) {
        lengths[static_cast<size_t>(used[0].second)] = 1;
        return lengths;
    }
    std::sort(used.begin(), used.end());
    const size_t m = used.size();
    std::vector<uint64_t> weight(2 * m);
    std::vector<int32_t> parent(2 * m, -1);
    for (size_t i = 0; i < m; i++) weight[i] = used[i].first;
    size_t leaf = 0, node = m, made = m;
    auto next = [&]() -> size_t {
        if (leaf < m && (node >= made || weight[leaf] <= weight[node])) return leaf++;
        return node++;
    };
    while (made < 2 * m - 1) {
        const size_t a = next(), b = next();
        weight[made] = weight[a] + weight[b];
        parent[a] = static_cast<int32_t>(made);
        parent[b] = static_cast<int32_t>(made);
        made++;
    }
    std::vector<int> depth(2 * m - 1, 0);
    for (size_t i = 2 * m - 2; i-- > 0;) depth[i] = depth[static_cast<size_t>(parent[i])] + 1;
    std::vector<int> perLength(64, 0);
    for (size_t i = 0; i < m; i++) perLength[static_cast<size_t>(std::min(depth[i], 63))]++;
    for (int l = most + 1; l < 64; l++) {
        perLength[static_cast<size_t>(most)] += perLength[static_cast<size_t>(l)];
        perLength[static_cast<size_t>(l)] = 0;
    }
    uint64_t total = 0;
    for (int l = 1; l <= most; l++) total += static_cast<uint64_t>(perLength[static_cast<size_t>(l)]) << (most - l);
    while (total > (uint64_t{1} << most)) {
        perLength[static_cast<size_t>(most)]--;
        for (int l = most - 1; l > 0; l--)
            if (perLength[static_cast<size_t>(l)] != 0) {
                perLength[static_cast<size_t>(l)]--;
                perLength[static_cast<size_t>(l + 1)] += 2;
                break;
            }
        total--;
    }
    size_t i = 0;
    for (int l = most; l > 0; l--)
        for (int k = 0; k < perLength[static_cast<size_t>(l)]; k++) lengths[static_cast<size_t>(used[i++].second)] = l;
    return lengths;
}

// Codes for lengths, as the decoder numbers them, each reversed, as they go out lowest bit first.
std::vector<uint32_t> codesFor(const std::vector<int>& lengths) {
    std::vector<uint32_t> codes(lengths.size(), 0);
    uint32_t perLength[16] = {}, next[16] = {};
    for (int l : lengths) perLength[l]++;
    perLength[0] = 0;
    uint32_t code = 0;
    for (int l = 1; l < 16; l++) {
        code = (code + perLength[l - 1]) << 1;
        next[l] = code;
    }
    for (size_t s = 0; s < lengths.size(); s++)
        if (lengths[s] > 0) codes[s] = reverse(next[lengths[s]]++, lengths[s]);
    return codes;
}

// A prefix code written: simply where it has one or two symbols below 256, else its lengths
// through the code length code, runs of them shortened (16 the length before, 17 and 18 zeros).
void writeCode(LBitsOut& w, const std::vector<int>& lengths) {
    std::vector<int> symbols;
    for (size_t s = 0; s < lengths.size(); s++)
        if (lengths[s] > 0) symbols.push_back(static_cast<int>(s));
    if (symbols.empty()) symbols.push_back(0);
    if (symbols.size() <= 2 && symbols.back() < 256) {
        w.put(1, 1);
        w.put(static_cast<uint32_t>(symbols.size() - 1), 1);
        if (symbols[0] < 2) {
            w.put(0, 1);
            w.put(static_cast<uint32_t>(symbols[0]), 1);
        } else {
            w.put(1, 1);
            w.put(static_cast<uint32_t>(symbols[0]), 8);
        }
        if (symbols.size() == 2) w.put(static_cast<uint32_t>(symbols[1]), 8);
        return;
    }
    w.put(0, 1);
    std::vector<std::pair<int, int>> tokens;  // a code length symbol and its extra bits
    for (size_t i = 0; i < lengths.size();) {
        const int value = lengths[i];
        size_t run = 1;
        while (i + run < lengths.size() && lengths[i + run] == value) run++;
        i += run;
        if (value == 0) {
            while (run >= 11) {
                const size_t take = std::min<size_t>(run, 138);
                tokens.push_back({18, static_cast<int>(take - 11)});
                run -= take;
            }
            if (run >= 3) {
                tokens.push_back({17, static_cast<int>(run - 3)});
                run = 0;
            }
        } else {
            tokens.push_back({value, 0});
            run--;
            while (run >= 3) {
                const size_t take = std::min<size_t>(run, 6);
                tokens.push_back({16, static_cast<int>(take - 3)});
                run -= take;
            }
        }
        while (run-- > 0) tokens.push_back({value, 0});
    }
    std::vector<uint32_t> counts(19, 0);
    for (const auto& t : tokens) counts[static_cast<size_t>(t.first)]++;
    const std::vector<int> lengthLengths = limitedLengths(counts, 7);
    int count = 19;
    while (count > 4 && lengthLengths[static_cast<size_t>(kCodeLengthOrder[count - 1])] == 0) count--;
    w.put(static_cast<uint32_t>(count - 4), 4);
    for (int i = 0; i < count; i++) w.put(static_cast<uint32_t>(lengthLengths[static_cast<size_t>(kCodeLengthOrder[i])]), 3);
    w.put(0, 1);  // all the alphabet's lengths are given
    // A code length code of one symbol takes no bits.
    int usedLengths = 0;
    for (int l : lengthLengths) usedLengths += l > 0 ? 1 : 0;
    const std::vector<uint32_t> lengthCodes = codesFor(lengthLengths);
    for (const auto& t : tokens) {
        if (usedLengths > 1) w.put(lengthCodes[static_cast<size_t>(t.first)], lengthLengths[static_cast<size_t>(t.first)]);
        if (t.first == 16) w.put(static_cast<uint32_t>(t.second), 2);
        else if (t.first == 17) w.put(static_cast<uint32_t>(t.second), 3);
        else if (t.first == 18) w.put(static_cast<uint32_t>(t.second), 7);
    }
}

// The prefix code of a length or a distance, and its extra bits.
void prefixOf(int value, int& code, int& extraBits, int& extra) {
    if (value <= 4) {
        code = value - 1;
        extraBits = 0;
        extra = 0;
        return;
    }
    const int v = value - 1;
    int highest = 31;
    while (((v >> highest) & 1) == 0) highest--;
    const int second = (v >> (highest - 1)) & 1;
    extraBits = highest - 1;
    extra = v & ((1 << extraBits) - 1);
    code = 2 * highest + second;
}

// An entropy-coded picture written: its colour cache, no meta codes (a single group), its five
// codes and its pixels, as literals, copies (LZ77 over pixels) or cache indices.
void writeImage(LBitsOut& w, const std::vector<uint32_t>& px, int width, bool main, int cacheBits) {
    if (cacheBits > 0) {
        w.put(1, 1);
        w.put(static_cast<uint32_t>(cacheBits), 4);
    } else {
        w.put(0, 1);
    }
    if (main) w.put(0, 1);  // no meta prefix codes
    // The symbols: literals, copies and cache hits, found greedily.
    struct Symbol {
        uint32_t argb;
        int kind;  // 0 literal, 1 copy, 2 cache
        int length, distance, cacheKey;
    };
    std::vector<Symbol> symbols;
    const int cacheSize = cacheBits > 0 ? 1 << cacheBits : 0;
    std::vector<uint32_t> cache(static_cast<size_t>(cacheSize), 0);
    std::vector<uint8_t> cacheSet(static_cast<size_t>(cacheSize), 0);
    auto remember = [&](uint32_t argb) {
        if (cacheBits == 0) return;
        const size_t k = (0x1E35A7BDu * argb) >> (32 - cacheBits);
        cache[k] = argb;
        cacheSet[k] = 1;
    };
    const size_t n = px.size();
    constexpr int kHashBits = 16;
    std::vector<int32_t> head(size_t{1} << kHashBits, -1), previous(n, -1);
    auto hashAt = [&](size_t i) { return static_cast<size_t>(((px[i] * 0x9E3779B1u) ^ (px[i + 1] * 0x85EBCA77u)) >> (32 - kHashBits)); };
    // The short distances of the plane code, by their offset, for the commonest small copies.
    auto distanceCode = [&](int64_t distance) -> int {
        const int64_t across = distance % width, down = distance / width;
        for (int c = 0; c < 120; c++) {
            const int64_t d = kPlane[c][0] + static_cast<int64_t>(kPlane[c][1]) * width;
            if (d == distance && (d >= 1)) return c + 1;
        }
        (void)across;
        (void)down;
        return static_cast<int>(distance) + 120;
    };
    for (size_t i = 0; i < n;) {
        int bestLength = 0;
        int64_t bestDistance = 0;
        if (i + 1 < n) {
            int chain = 64;
            for (int32_t at = head[hashAt(i)]; at >= 0 && chain-- > 0; at = previous[static_cast<size_t>(at)]) {
                const size_t from = static_cast<size_t>(at);
                if (i - from > (size_t{1} << 20) - 120) break;  // past the longest distance there is
                const size_t limit = std::min<size_t>(4096, n - i);
                size_t length = 0;
                while (length < limit && px[from + length] == px[i + length]) length++;
                if (static_cast<int>(length) > bestLength) {
                    bestLength = static_cast<int>(length);
                    bestDistance = static_cast<int64_t>(i - from);
                    if (length == limit) break;
                }
            }
        }
        if (bestLength >= 3) {
            symbols.push_back({0, 1, bestLength, static_cast<int>(distanceCode(bestDistance)), 0});
            for (int k = 0; k < bestLength; k++) {
                if (i + 1 < n) {
                    const size_t h = hashAt(i);
                    previous[i] = head[h];
                    head[h] = static_cast<int32_t>(i);
                }
                remember(px[i]);
                i++;
            }
            continue;
        }
        const uint32_t argb = px[i];
        if (cacheBits > 0) {
            const size_t k = (0x1E35A7BDu * argb) >> (32 - cacheBits);
            if (cacheSet[k] && cache[k] == argb) {
                symbols.push_back({argb, 2, 0, 0, static_cast<int>(k)});
            } else {
                symbols.push_back({argb, 0, 0, 0, 0});
            }
        } else {
            symbols.push_back({argb, 0, 0, 0, 0});
        }
        if (i + 1 < n) {
            const size_t h = hashAt(i);
            previous[i] = head[h];
            head[h] = static_cast<int32_t>(i);
        }
        remember(argb);
        i++;
    }
    // The five codes from the symbols' counts.
    std::vector<uint32_t> counts[5] = {std::vector<uint32_t>(static_cast<size_t>(256 + 24 + cacheSize), 0), std::vector<uint32_t>(256, 0),
                                       std::vector<uint32_t>(256, 0), std::vector<uint32_t>(256, 0), std::vector<uint32_t>(40, 0)};
    for (const Symbol& s : symbols) {
        if (s.kind == 0) {
            counts[0][(s.argb >> 8) & 0xFF]++;
            counts[1][(s.argb >> 16) & 0xFF]++;
            counts[2][s.argb & 0xFF]++;
            counts[3][s.argb >> 24]++;
        } else if (s.kind == 1) {
            int code, bits, extra;
            prefixOf(s.length, code, bits, extra);
            counts[0][static_cast<size_t>(256 + code)]++;
            prefixOf(s.distance, code, bits, extra);
            counts[4][static_cast<size_t>(code)]++;
        } else {
            counts[0][static_cast<size_t>(256 + 24 + s.cacheKey)]++;
        }
    }
    std::vector<int> lengths[5];
    std::vector<uint32_t> codes[5];
    for (int c = 0; c < 5; c++) {
        lengths[c] = limitedLengths(counts[c], 15);
        writeCode(w, lengths[c]);
        codes[c] = codesFor(lengths[c]);
    }
    // A code of one symbol takes no bits.
    bool single[5];
    for (int c = 0; c < 5; c++) {
        int used = 0;
        for (int l : lengths[c]) used += l > 0 ? 1 : 0;
        single[c] = used <= 1;
    }
    auto put = [&](int c, size_t symbol) {
        if (!single[c]) w.put(codes[c][symbol], lengths[c][symbol]);
    };
    for (const Symbol& s : symbols) {
        if (s.kind == 0) {
            put(0, (s.argb >> 8) & 0xFF);
            put(1, (s.argb >> 16) & 0xFF);
            put(2, s.argb & 0xFF);
            put(3, s.argb >> 24);
        } else if (s.kind == 1) {
            int code, bits, extra;
            prefixOf(s.length, code, bits, extra);
            put(0, static_cast<size_t>(256 + code));
            w.put(static_cast<uint32_t>(extra), bits);
            prefixOf(s.distance, code, bits, extra);
            put(4, static_cast<size_t>(code));
            w.put(static_cast<uint32_t>(extra), bits);
        } else {
            put(0, static_cast<size_t>(256 + 24 + s.cacheKey));
        }
    }
}

uint32_t subPixels(uint32_t a, uint32_t b) {
    const uint32_t ag = 0x00FF00FFu + (a & 0xFF00FF00u) - (b & 0xFF00FF00u), rb = 0xFF00FF00u + (a & 0x00FF00FFu) - (b & 0x00FF00FFu);
    return (ag & 0xFF00FF00u) | (rb & 0x00FF00FFu);
}

// Lossless WebP data (VP8L) of ARGB pixels: through a palette where there are 256 colours or fewer,
// else subtract-green and a predictor chosen for each 16 by 16 block.
std::vector<uint8_t> encodeLossless(const std::vector<uint32_t>& argb, int width, int height, bool headerless) {
    LBitsOut w;
    bool alpha = false;
    for (uint32_t p : argb)
        if ((p >> 24) != 0xFF) {
            alpha = true;
            break;
        }
    if (!headerless) {
        w.put(0x2F, 8);
        w.put(static_cast<uint32_t>(width - 1), 14);
        w.put(static_cast<uint32_t>(height - 1), 14);
        w.put(alpha ? 1 : 0, 1);
        w.put(0, 3);
    }
    // The colours, if there are no more than 256.
    std::vector<uint32_t> palette;
    {
        std::vector<uint32_t> sorted(argb);
        std::sort(sorted.begin(), sorted.end());
        sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
        if (sorted.size() <= 256) palette = sorted;
    }
    std::vector<uint32_t> px;
    int xsize = width;
    if (!palette.empty()) {
        w.put(1, 1);
        w.put(3, 2);
        w.put(static_cast<uint32_t>(palette.size() - 1), 8);
        std::vector<uint32_t> deltas(palette.size());
        for (size_t i = 0; i < palette.size(); i++) deltas[i] = i == 0 ? palette[0] : subPixels(palette[i], palette[i - 1]);
        writeImage(w, deltas, static_cast<int>(palette.size()), false, 0);
        const int bits = palette.size() <= 2 ? 3 : palette.size() <= 4 ? 2 : palette.size() <= 16 ? 1 : 0;
        const int perPixel = 1 << bits, bitsEach = 8 >> bits;
        xsize = (width + perPixel - 1) >> bits;
        px.assign(static_cast<size_t>(xsize) * static_cast<size_t>(height), 0xFF000000u);
        for (int y = 0; y < height; y++)
            for (int x = 0; x < width; x++) {
                const uint32_t c = argb[static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)];
                const uint32_t index = static_cast<uint32_t>(std::lower_bound(palette.begin(), palette.end(), c) - palette.begin());
                px[static_cast<size_t>(y * xsize + (x >> bits))] |= (index << ((x & (perPixel - 1)) * bitsEach)) << 8;
            }
    } else {
        // Subtract green.
        px = argb;
        for (uint32_t& p : px) {
            const uint32_t g = (p >> 8) & 0xFF;
            p = (p & 0xFF00FF00u) | ((((p >> 16) & 0xFF) - g) & 0xFF) << 16 | (((p & 0xFF) - g) & 0xFF);
        }
        w.put(1, 1);
        w.put(2, 2);
        // The predictor: for each block the mode whose leftovers are smallest, tried on every other pixel.
        const int bits = 4;
        const int bw = (width + 15) >> bits, bh = (height + 15) >> bits;
        std::vector<uint32_t> modes(static_cast<size_t>(bw) * static_cast<size_t>(bh));
        const size_t sw = static_cast<size_t>(width);
        auto cost = [](uint32_t residual) {
            int c = 0;
            for (int s = 0; s < 32; s += 8) {
                const int v = static_cast<int>((residual >> s) & 0xFF);
                c += v < 128 ? v : 256 - v;
            }
            return c;
        };
        for (int by = 0; by < bh; by++)
            for (int bx = 0; bx < bw; bx++) {
                int best = 11;
                int64_t bestCost = INT64_MAX;
                for (int mode = 0; mode < 14; mode++) {
                    int64_t total = 0;
                    for (int y = std::max(1, by * 16); y < std::min(height, by * 16 + 16); y += 2)
                        for (int x = std::max(1, bx * 16); x < std::min(width, bx * 16 + 16); x += 2) {
                            const size_t at = static_cast<size_t>(y) * sw + static_cast<size_t>(x);
                            total += cost(subPixels(px[at], Lossless::predictFor(mode, px.data(), at, sw)));
                        }
                    if (total < bestCost) {
                        bestCost = total;
                        best = mode;
                    }
                }
                modes[static_cast<size_t>(by * bw + bx)] = 0xFF000000u | (static_cast<uint32_t>(best) << 8);
            }
        std::vector<uint32_t> residuals(px.size());
        for (int y = 0; y < height; y++)
            for (int x = 0; x < width; x++) {
                const size_t at = static_cast<size_t>(y) * sw + static_cast<size_t>(x);
                uint32_t prediction;
                if (y == 0) prediction = x == 0 ? 0xFF000000u : px[at - 1];
                else if (x == 0) prediction = px[at - sw];
                else prediction = Lossless::predictFor(static_cast<int>((modes[static_cast<size_t>((y >> bits) * bw + (x >> bits))] >> 8) & 0xF), px.data(), at, sw);
                residuals[at] = subPixels(px[at], prediction);
            }
        w.put(1, 1);
        w.put(0, 2);
        w.put(bits - 2, 3);
        writeImage(w, modes, bw, false, 0);
        px = std::move(residuals);
    }
    w.put(0, 1);  // no more transforms
    writeImage(w, px, xsize, true, palette.empty() ? 10 : 0);
    w.finish();
    return std::move(w.out);
}

// ARGB pixels as RGBA bytes.
std::vector<uint8_t> rgbaOf(const std::vector<uint32_t>& argb) {
    std::vector<uint8_t> out(argb.size() * 4);
    for (size_t i = 0; i < argb.size(); i++) {
        const uint32_t p = argb[i];
        out[i * 4] = static_cast<uint8_t>(p >> 16);
        out[i * 4 + 1] = static_cast<uint8_t>(p >> 8);
        out[i * 4 + 2] = static_cast<uint8_t>(p);
        out[i * 4 + 3] = static_cast<uint8_t>(p >> 24);
    }
    return out;
}

Value bytesValue(VM& vm, std::vector<uint8_t>&& data) {
    vm.checkRoomFor(data.size());
    ObjBytes* made = vm.alloc<ObjBytes>();
    made->data = std::move(data);
    vm.account(made->data.size());
    return Value::object(made);
}

}  // namespace

void registerWebp(VM& vm) {
    ObjClass* holder = vm.alloc<ObjClass>(vm.intern("Webp"));
    auto def = [&](const char* name, int minArgs, int maxArgs, NativeFn fn) {
        holder->statics.set(vm.intern(name), Value::object(vm.alloc<ObjNative>(std::string("Webp.") + name, minArgs, maxArgs, fn)));
    };
    // decodeLossless(data, from, to, width, height): [width, height, RGBA bytes] of a VP8L
    // bitstream, or a string, what is wrong. A width and height given (more than 0) mean ALPH's
    // headerless data of that size, whose green is the alpha.
    def("decodeLossless", 5, 5, [](VM& vm, int, Value* args) -> Value {
        if (!args[0].isObjType(ObjType::Bytes)) throw vm.runtimeError(Thrown::IllegalArgumentException, "WebP's data is Bytes");
        const std::vector<uint8_t>& data = asBytes(args[0])->data;
        const int64_t from = vm.toInt(args[1], "from"), to = vm.toInt(args[2], "to");
        if (from < 0 || to < from || to > static_cast<int64_t>(data.size())) throw vm.runtimeError(Thrown::IndexOutOfBoundsException, "a part outside the data");
        const int64_t givenWidth = vm.toInt(args[3], "width"), givenHeight = vm.toInt(args[4], "height");
        if ((givenWidth != 0 || givenHeight != 0) && (givenWidth < 1 || givenHeight < 1 || givenWidth > 16384 || givenHeight > 16384))
            throw vm.runtimeError(Thrown::IllegalArgumentException, "a WebP is 1 to 16384 pixels each way, not " + std::to_string(givenWidth) + " by " + std::to_string(givenHeight));
        int width = static_cast<int>(givenWidth), height = static_cast<int>(givenHeight);
        const bool headerless = width > 0 && height > 0;
        std::vector<uint32_t> argb;
        try {
            Lossless(data.data() + from, static_cast<size_t>(to - from)).read(argb, width, height, headerless);
        } catch (const LosslessError& e) {
            return vm.str(e.what);
        }
        const Value pixels = bytesValue(vm, rgbaOf(argb));
        TempRoot keep(vm, pixels);
        return Value::object(newList(vm, {Value::integer(width), Value::integer(height), pixels}));
    });
    // decodeLossy(data, from, to, alphaFrom, alphaTo): [width, height, RGBA bytes] of a VP8 key
    // frame, its alpha from the ALPH chunk's bytes where alphaTo > alphaFrom; or a string, what is wrong.
    def("decodeLossy", 5, 5, [](VM& vm, int, Value* args) -> Value {
        if (!args[0].isObjType(ObjType::Bytes)) throw vm.runtimeError(Thrown::IllegalArgumentException, "WebP's data is Bytes");
        const std::vector<uint8_t>& data = asBytes(args[0])->data;
        const int64_t from = vm.toInt(args[1], "from"), to = vm.toInt(args[2], "to"), alphaFrom = vm.toInt(args[3], "alphaFrom"), alphaTo = vm.toInt(args[4], "alphaTo");
        if (from < 0 || to < from || to > static_cast<int64_t>(data.size()) || alphaFrom < 0 || alphaTo > static_cast<int64_t>(data.size()))
            throw vm.runtimeError(Thrown::IndexOutOfBoundsException, "a part outside the data");
        int width = 0, height = 0;
        std::vector<uint8_t> rgba;
        try {
            rgba = Lossy(data.data() + from, static_cast<size_t>(to - from)).read(width, height);
            if (alphaTo > alphaFrom) applyAlpha(data.data() + alphaFrom, static_cast<size_t>(alphaTo - alphaFrom), rgba, width, height);
        } catch (const Vp8Error& e) {
            return vm.str(e.what);
        }
        const Value pixels = bytesValue(vm, std::move(rgba));
        TempRoot keep(vm, pixels);
        return Value::object(newList(vm, {Value::integer(width), Value::integer(height), pixels}));
    });
    // encodeLossless(rgba, width, height, headerless): VP8L data of RGBA pixels; headerless for
    // ALPH's alpha, which has no signature or size of its own.
    def("encodeLossless", 4, 4, [](VM& vm, int, Value* args) -> Value {
        if (!args[0].isObjType(ObjType::Bytes)) throw vm.runtimeError(Thrown::IllegalArgumentException, "pixels are Bytes");
        const std::vector<uint8_t>& rgba = asBytes(args[0])->data;
        const int64_t width = vm.toInt(args[1], "width"), height = vm.toInt(args[2], "height");
        if (width < 1 || height < 1 || width > 16384 || height > 16384) throw vm.runtimeError(Thrown::IllegalArgumentException, "a WebP is 1 to 16384 pixels each way, not " + std::to_string(width) + " by " + std::to_string(height));
        if (rgba.size() != static_cast<size_t>(width * height * 4)) throw vm.runtimeError(Thrown::IllegalArgumentException, "the pixels are not " + std::to_string(width) + " by " + std::to_string(height));
        std::vector<uint32_t> argb(static_cast<size_t>(width * height));
        for (size_t i = 0; i < argb.size(); i++)
            argb[i] = (static_cast<uint32_t>(rgba[i * 4 + 3]) << 24) | (static_cast<uint32_t>(rgba[i * 4]) << 16) | (static_cast<uint32_t>(rgba[i * 4 + 1]) << 8) | rgba[i * 4 + 2];
        return bytesValue(vm, encodeLossless(argb, static_cast<int>(width), static_cast<int>(height), args[3].truthy()));
    });
    // encodeLossy(rgba, width, height, quality): [VP8 data, ALPH data or null] of RGBA pixels at
    // `quality` from 0 to 1; the alpha, kept losslessly, only where some pixel is see-through.
    def("encodeLossy", 4, 4, [](VM& vm, int, Value* args) -> Value {
        if (!args[0].isObjType(ObjType::Bytes)) throw vm.runtimeError(Thrown::IllegalArgumentException, "pixels are Bytes");
        const std::vector<uint8_t>& rgba = asBytes(args[0])->data;
        const int64_t width = vm.toInt(args[1], "width"), height = vm.toInt(args[2], "height");
        if (width < 1 || height < 1 || width > 16383 || height > 16383) throw vm.runtimeError(Thrown::IllegalArgumentException, "a lossy WebP is 1 to 16383 pixels each way, not " + std::to_string(width) + " by " + std::to_string(height));
        if (rgba.size() != static_cast<size_t>(width * height * 4)) throw vm.runtimeError(Thrown::IllegalArgumentException, "the pixels are not " + std::to_string(width) + " by " + std::to_string(height));
        const double quality = vm.toNumber(args[3], "quality");
        const Value picture = bytesValue(vm, LossyWriter(rgba.data(), static_cast<int>(width), static_cast<int>(height), quality).write());
        TempRoot keepPicture(vm, picture);
        const size_t n = static_cast<size_t>(width * height);
        bool seeThrough = false;
        for (size_t i = 0; i < n && !seeThrough; i++) seeThrough = rgba[i * 4 + 3] != 255;
        Value alpha = Value::null();
        if (seeThrough) {
            // ALPH: lossless, unfiltered, the alpha as green.
            std::vector<uint32_t> green(n);
            for (size_t i = 0; i < n; i++) green[i] = 0xFF000000u | (static_cast<uint32_t>(rgba[i * 4 + 3]) << 8);
            std::vector<uint8_t> data = encodeLossless(green, static_cast<int>(width), static_cast<int>(height), true);
            data.insert(data.begin(), 0x01);
            alpha = bytesValue(vm, std::move(data));
        }
        TempRoot keepAlpha(vm, alpha);
        return Value::object(newList(vm, {picture, alpha}));
    });
    vm.defineGlobal("__webp", Value::object(holder));
}

}  // namespace ww
