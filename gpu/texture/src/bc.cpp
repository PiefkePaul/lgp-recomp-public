// BCn software decoder (bc.hpp).
#include "lgp/gpu/texture/bc.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstring>

namespace lgp::gpu::texture {
namespace {

// The fast decoder stores a texel as one little-endian 32-bit word.
static_assert(std::endian::native == std::endian::little);

uint32_t load32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
uint64_t load64(const uint8_t* p) { return uint64_t(load32(p)) | uint64_t(load32(p + 4)) << 32; }

void put(uint8_t* out, size_t stride, uint32_t i, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    uint8_t* t = out + (i / 4) * stride + (i % 4) * 4;
    t[0] = r;
    t[1] = g;
    t[2] = b;
    t[3] = a;
}

// The reference decoder: straight from the format descriptions, one bit and
// one byte at a time (LGP_WEBGPU_BC_DECODER=reference; tests compare with it).
namespace ref {

// --- BC1-BC3 colour ---------------------------------------------------------

void rgb565(uint16_t c, uint8_t rgb[3]) {
    const uint32_t r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    rgb[0] = uint8_t(r << 3 | r >> 2);
    rgb[1] = uint8_t(g << 2 | g >> 4);
    rgb[2] = uint8_t(b << 3 | b >> 2);
}

// Writes colour (and alpha 255, or 0 for BC1's transparent index) of 16 texels.
void decode_colour(const uint8_t* block, bool allow_three_colour, uint8_t* out, size_t stride) {
    const uint16_t c0 = uint16_t(block[0] | block[1] << 8), c1 = uint16_t(block[2] | block[3] << 8);
    uint8_t p[4][4];
    rgb565(c0, p[0]);
    rgb565(c1, p[1]);
    p[0][3] = p[1][3] = p[2][3] = p[3][3] = 255;
    if (c0 > c1 || !allow_three_colour) {
        for (int k = 0; k < 3; ++k) {
            p[2][k] = uint8_t((2 * p[0][k] + p[1][k]) / 3);
            p[3][k] = uint8_t((p[0][k] + 2 * p[1][k]) / 3);
        }
    } else {
        for (int k = 0; k < 3; ++k) {
            p[2][k] = uint8_t((p[0][k] + p[1][k]) / 2);
            p[3][k] = 0;
        }
        p[3][3] = 0;
    }
    const uint32_t idx = load32(block + 4);
    for (uint32_t i = 0; i < 16; ++i) {
        const uint8_t* c = p[(idx >> (2 * i)) & 3];
        put(out, stride, i, c[0], c[1], c[2], c[3]);
    }
}

// --- BC3 alpha / BC4 / BC5 (one 8-byte channel block) -------------------------

void channel_unorm(const uint8_t* block, uint8_t v[16]) {
    const uint32_t a0 = block[0], a1 = block[1];
    uint32_t pal[8] = {a0, a1};
    if (a0 > a1) {
        for (uint32_t i = 1; i < 7; ++i)
            pal[i + 1] = ((7 - i) * a0 + i * a1) / 7;
    } else {
        for (uint32_t i = 1; i < 5; ++i)
            pal[i + 1] = ((5 - i) * a0 + i * a1) / 5;
        pal[6] = 0;
        pal[7] = 255;
    }
    const uint64_t idx = load64(block) >> 16;
    for (uint32_t i = 0; i < 16; ++i)
        v[i] = uint8_t(pal[(idx >> (3 * i)) & 7]);
}

void channel_snorm(const uint8_t* block, uint8_t v[16]) {
    const int32_t a0 = std::max<int32_t>(int8_t(block[0]), -127), a1 = std::max<int32_t>(int8_t(block[1]), -127);
    int32_t pal[8] = {a0, a1};
    if (a0 > a1) {
        for (int32_t i = 1; i < 7; ++i)
            pal[i + 1] = ((7 - i) * a0 + i * a1) / 7;
    } else {
        for (int32_t i = 1; i < 5; ++i)
            pal[i + 1] = ((5 - i) * a0 + i * a1) / 5;
        pal[6] = -127;
        pal[7] = 127;
    }
    const uint64_t idx = load64(block) >> 16;
    for (uint32_t i = 0; i < 16; ++i)
        v[i] = uint8_t(int8_t(pal[(idx >> (3 * i)) & 7]));
}

// --- BC7 ----------------------------------------------------------------------

struct Bc7Mode {
    uint8_t subsets, partition_bits, rotation_bits, selector_bits, colour_bits, alpha_bits, endpoint_pbits,
        shared_pbits, index_bits, index2_bits;
};
constexpr Bc7Mode kBc7Modes[8] = {
    {3, 4, 0, 0, 4, 0, 1, 0, 3, 0}, {2, 6, 0, 0, 6, 0, 0, 1, 3, 0}, {3, 6, 0, 0, 5, 0, 0, 0, 2, 0},
    {2, 6, 0, 0, 7, 0, 1, 0, 2, 0}, {1, 0, 2, 1, 5, 6, 0, 0, 2, 3}, {1, 0, 2, 0, 7, 8, 0, 0, 2, 2},
    {1, 0, 0, 0, 7, 7, 1, 0, 4, 0}, {2, 6, 0, 0, 5, 5, 1, 0, 2, 0},
};

// Subset of texel i (bit i) for the 64 two-subset partitions.
constexpr uint16_t kPartition2[64] = {
    0xcccc, 0x8888, 0xeeee, 0xecc8, 0xc880, 0xfeec, 0xfec8, 0xec80, 0xc800, 0xffec, 0xfe80, 0xe800, 0xffe8,
    0xff00, 0xfff0, 0xf000, 0xf710, 0x008e, 0x7100, 0x08ce, 0x008c, 0x7310, 0x3100, 0x8cce, 0x088c, 0x3110,
    0x6666, 0x366c, 0x17e8, 0x0ff0, 0x718e, 0x399c, 0xaaaa, 0xf0f0, 0x5a5a, 0x33cc, 0x3c3c, 0x55aa, 0x9696,
    0xa55a, 0x73ce, 0x13c8, 0x324c, 0x3bdc, 0x6996, 0xc33c, 0x9966, 0x0660, 0x0272, 0x04e4, 0x4e40, 0x2720,
    0xc936, 0x936c, 0x39c6, 0x639c, 0x9336, 0x9cc6, 0x817e, 0xe718, 0xccf0, 0x0fcc, 0x7744, 0xee22,
};
// Subset of texel i (bits 2i..2i+1) for the 64 three-subset partitions.
constexpr uint32_t kPartition3[64] = {
    0xaa685050, 0x6a5a5040, 0x5a5a4200, 0x5450a0a8, 0xa5a50000, 0xa0a05050, 0x5555a0a0, 0x5a5a5050,
    0xaa550000, 0xaa555500, 0xaaaa5500, 0x90909090, 0x94949494, 0xa4a4a4a4, 0xa9a59450, 0x2a0a4250,
    0xa5945040, 0x0a425054, 0xa5a5a500, 0x55a0a0a0, 0xa8a85454, 0x6a6a4040, 0xa4a45000, 0x1a1a0500,
    0x0050a4a4, 0xaaa59090, 0x14696914, 0x69691400, 0xa08585a0, 0xaa821414, 0x50a4a450, 0x6a5a0200,
    0xa9a58000, 0x5090a0a8, 0xa8a09050, 0x24242424, 0x00aa5500, 0x24924924, 0x24499224, 0x50a50a50,
    0x500aa550, 0xaaaa4444, 0x66660000, 0xa5a0a5a0, 0x50a050a0, 0x69286928, 0x44aaaa44, 0x66666600,
    0xaa444444, 0x54a854a8, 0x95809580, 0x96969600, 0xa85454a8, 0x80959580, 0xaa141414, 0x96960000,
    0xaaaa1414, 0xa05050a0, 0xa0a5a5a0, 0x96000000, 0x40804080, 0xa9a8a9a8, 0xaaaaaa44, 0x2a4a5254,
};
// Anchor texel of subset 1 (two subsets), of subsets 1 and 2 (three subsets).
constexpr uint8_t kAnchor2[64] = {
    15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 2,  8,  2,  2,  8,
    8,  15, 2,  8,  2,  2,  8,  8,  2,  2,  15, 15, 6,  8,  2,  8,  15, 15, 2,  8,  2,  2,
    2,  15, 15, 6,  6,  2,  6,  8,  15, 15, 2,  2,  15, 15, 15, 15, 15, 2,  2,  15,
};
constexpr uint8_t kAnchor3a[64] = {
    3,  3,  15, 15, 8, 3, 15, 15, 8, 8,  6,  6,  6, 5,  3,  3,  3, 3,  8,  15, 3,  3,
    6,  10, 5,  8,  8, 6, 8,  5,  15, 15, 8,  15, 3, 5,  6,  10, 8, 15, 15, 3,  15, 5,
    15, 15, 15, 15, 3, 15, 5, 5,  5,  8,  5,  10, 5, 10, 8,  13, 15, 12, 3,  3,
};
constexpr uint8_t kAnchor3b[64] = {
    15, 8,  8,  3,  15, 15, 3,  8,  15, 15, 15, 15, 15, 15, 15, 8,  15, 8,  15, 3,  15, 8,
    15, 8,  3,  15, 6,  10, 15, 15, 10, 8,  15, 3,  15, 10, 10, 8,  9,  10, 6,  15, 8,  15,
    3,  6,  6,  8,  15, 3,  15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 3,  15, 15, 8,
};
constexpr uint8_t kWeights2[4] = {0, 21, 43, 64};
constexpr uint8_t kWeights3[8] = {0, 9, 18, 27, 37, 46, 55, 64};
constexpr uint8_t kWeights4[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};

const uint8_t* weights(uint32_t bits) { return bits == 2 ? kWeights2 : bits == 3 ? kWeights3 : kWeights4; }

struct Bits {
    uint64_t lo, hi;
    uint32_t pos = 0;
    uint32_t take(uint32_t n) {
        uint32_t v = 0;
        for (uint32_t i = 0; i < n; ++i, ++pos) {
            const uint64_t bit = pos < 64 ? (lo >> pos) & 1 : (hi >> (pos - 64)) & 1;
            v |= uint32_t(bit) << i;
        }
        return v;
    }
};

uint8_t expand(uint32_t v, uint32_t bits) {
    v <<= 8 - bits;
    return uint8_t(v | v >> bits);
}

uint8_t interpolate(uint8_t e0, uint8_t e1, uint32_t w) { return uint8_t(((64 - w) * e0 + w * e1 + 32) >> 6); }

void decode_bc7(const uint8_t* block, uint8_t* out, size_t stride) {
    Bits bits{load64(block), load64(block + 8)};
    uint32_t mode = 0;
    while (mode < 8 && bits.take(1) == 0)
        ++mode;
    if (mode == 8) {
        for (uint32_t i = 0; i < 16; ++i)
            put(out, stride, i, 0, 0, 0, 0);
        return;
    }
    const Bc7Mode& m = kBc7Modes[mode];
    const uint32_t partition = bits.take(m.partition_bits);
    const uint32_t rotation = bits.take(m.rotation_bits);
    const uint32_t selector = bits.take(m.selector_bits);
    const uint32_t ns = m.subsets;
    uint32_t ep[6][4] = {};  // endpoint (subset * 2 + k), channel RGBA, raw bits
    for (uint32_t c = 0; c < 3; ++c)
        for (uint32_t e = 0; e < ns * 2; ++e)
            ep[e][c] = bits.take(m.colour_bits);
    for (uint32_t e = 0; e < ns * 2; ++e)
        ep[e][3] = m.alpha_bits ? bits.take(m.alpha_bits) : 0;
    uint32_t cbits = m.colour_bits, abits = m.alpha_bits;
    if (m.endpoint_pbits || m.shared_pbits) {
        uint32_t p[6];
        if (m.endpoint_pbits) {
            for (uint32_t e = 0; e < ns * 2; ++e)
                p[e] = bits.take(1);
        } else {
            for (uint32_t s = 0; s < ns; ++s)
                p[2 * s] = p[2 * s + 1] = bits.take(1);
        }
        for (uint32_t e = 0; e < ns * 2; ++e)
            for (uint32_t c = 0; c < 4; ++c)
                ep[e][c] = ep[e][c] << 1 | p[e];
        ++cbits;
        if (abits)
            ++abits;
    }
    uint8_t endpoints[6][4];
    for (uint32_t e = 0; e < ns * 2; ++e) {
        for (uint32_t c = 0; c < 3; ++c)
            endpoints[e][c] = expand(ep[e][c], cbits);
        endpoints[e][3] = abits ? expand(ep[e][3], abits) : 255;
    }
    auto subset_of = [&](uint32_t i) -> uint32_t {
        if (ns == 2)
            return (kPartition2[partition] >> i) & 1;
        if (ns == 3)
            return (kPartition3[partition] >> (2 * i)) & 3;
        return 0;
    };
    auto is_anchor = [&](uint32_t i) {
        if (i == 0)
            return true;
        if (ns == 2)
            return i == kAnchor2[partition];
        if (ns == 3)
            return i == kAnchor3a[partition] || i == kAnchor3b[partition];
        return false;
    };
    uint32_t index[16], index2[16] = {};
    for (uint32_t i = 0; i < 16; ++i)
        index[i] = bits.take(is_anchor(i) ? m.index_bits - 1 : m.index_bits);
    if (m.index2_bits)
        for (uint32_t i = 0; i < 16; ++i)
            index2[i] = bits.take(i == 0 ? m.index2_bits - 1 : m.index2_bits);

    for (uint32_t i = 0; i < 16; ++i) {
        const uint32_t s = subset_of(i);
        const uint8_t* e0 = endpoints[2 * s];
        const uint8_t* e1 = endpoints[2 * s + 1];
        uint8_t px[4];
        if (m.index2_bits) {
            // Modes 4 and 5: colour and alpha with separate index sets; the
            // selector bit swaps which set is which (mode 4).
            const uint32_t ci = selector ? index2[i] : index[i];
            const uint32_t ai = selector ? index[i] : index2[i];
            const uint32_t cb = selector ? m.index2_bits : m.index_bits;
            const uint32_t ab = selector ? m.index_bits : m.index2_bits;
            for (uint32_t c = 0; c < 3; ++c)
                px[c] = interpolate(e0[c], e1[c], weights(cb)[ci]);
            px[3] = interpolate(e0[3], e1[3], weights(ab)[ai]);
        } else {
            const uint8_t w = weights(m.index_bits)[index[i]];
            for (uint32_t c = 0; c < 4; ++c)
                px[c] = interpolate(e0[c], e1[c], w);
        }
        if (rotation != 0)
            std::swap(px[3], px[rotation - 1]);
        put(out, stride, i, px[0], px[1], px[2], px[3]);
    }
}

void decode_block(BcFormat format, const uint8_t* block, uint8_t* out, size_t stride) {
    uint8_t a[16], b[16];
    switch (format) {
    case BcFormat::BC1:
        decode_colour(block, true, out, stride);
        return;
    case BcFormat::BC2: {
        decode_colour(block + 8, false, out, stride);
        const uint64_t alpha = load64(block);
        for (uint32_t i = 0; i < 16; ++i)
            out[(i / 4) * stride + (i % 4) * 4 + 3] = uint8_t(((alpha >> (4 * i)) & 15) * 17);
        return;
    }
    case BcFormat::BC3:
        decode_colour(block + 8, false, out, stride);
        channel_unorm(block, a);
        for (uint32_t i = 0; i < 16; ++i)
            out[(i / 4) * stride + (i % 4) * 4 + 3] = a[i];
        return;
    case BcFormat::BC4:
        channel_unorm(block, a);
        for (uint32_t i = 0; i < 16; ++i)
            put(out, stride, i, a[i], 0, 0, 255);
        return;
    case BcFormat::BC4Snorm:
        channel_snorm(block, a);
        for (uint32_t i = 0; i < 16; ++i)
            put(out, stride, i, a[i], 0, 0, 127);
        return;
    case BcFormat::BC5:
        channel_unorm(block, a);
        channel_unorm(block + 8, b);
        for (uint32_t i = 0; i < 16; ++i)
            put(out, stride, i, a[i], b[i], 0, 255);
        return;
    case BcFormat::BC5Snorm:
        channel_snorm(block, a);
        channel_snorm(block + 8, b);
        for (uint32_t i = 0; i < 16; ++i)
            put(out, stride, i, a[i], b[i], 0, 127);
        return;
    case BcFormat::BC7:
        decode_bc7(block, out, stride);
        return;
    }
}

}  // namespace ref

// The fast decoder (the default): the same arithmetic, so the
// same bytes (tests/gpu/texture/test_bc.cpp compares them), but a texel is
// one 32-bit store, BC7 reads its fields with shifts instead of bit by bit,
// and whole blocks go straight into the image.
namespace fast {

uint32_t pack(uint32_t r, uint32_t g, uint32_t b, uint32_t a) { return r | g << 8 | b << 16 | a << 24; }

void store_row(uint8_t* out, const uint32_t row[4]) { std::memcpy(out, row, 16); }

void decode_colour(const uint8_t* block, bool allow_three_colour, uint8_t* out, size_t stride) {
    const uint32_t c0 = uint32_t(block[0] | block[1] << 8), c1 = uint32_t(block[2] | block[3] << 8);
    uint32_t e[2][3];
    for (int k = 0; k < 2; ++k) {
        const uint32_t c = k == 0 ? c0 : c1;
        const uint32_t r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
        e[k][0] = r << 3 | r >> 2;
        e[k][1] = g << 2 | g >> 4;
        e[k][2] = b << 3 | b >> 2;
    }
    uint32_t pal[4];
    pal[0] = pack(e[0][0], e[0][1], e[0][2], 255);
    pal[1] = pack(e[1][0], e[1][1], e[1][2], 255);
    if (c0 > c1 || !allow_three_colour) {
        pal[2] = pack((2 * e[0][0] + e[1][0]) / 3, (2 * e[0][1] + e[1][1]) / 3, (2 * e[0][2] + e[1][2]) / 3, 255);
        pal[3] = pack((e[0][0] + 2 * e[1][0]) / 3, (e[0][1] + 2 * e[1][1]) / 3, (e[0][2] + 2 * e[1][2]) / 3, 255);
    } else {
        pal[2] = pack((e[0][0] + e[1][0]) / 2, (e[0][1] + e[1][1]) / 2, (e[0][2] + e[1][2]) / 2, 255);
        pal[3] = 0;
    }
    const uint32_t idx = load32(block + 4);
    for (uint32_t y = 0; y < 4; ++y) {
        const uint32_t bits = idx >> (8 * y);
        const uint32_t row[4] = {pal[bits & 3], pal[(bits >> 2) & 3], pal[(bits >> 4) & 3], pal[(bits >> 6) & 3]};
        store_row(out + y * stride, row);
    }
}

// Palette of one 8-byte channel block (BC3 alpha, BC4, BC5) and its 48 index bits.
void channel_palette_unorm(const uint8_t* block, uint32_t pal[8]) {
    const uint32_t a0 = block[0], a1 = block[1];
    pal[0] = a0;
    pal[1] = a1;
    if (a0 > a1) {
        for (uint32_t i = 1; i < 7; ++i)
            pal[i + 1] = ((7 - i) * a0 + i * a1) / 7;
    } else {
        for (uint32_t i = 1; i < 5; ++i)
            pal[i + 1] = ((5 - i) * a0 + i * a1) / 5;
        pal[6] = 0;
        pal[7] = 255;
    }
}

void channel_palette_snorm(const uint8_t* block, uint32_t pal[8]) {
    const int32_t a0 = std::max<int32_t>(int8_t(block[0]), -127), a1 = std::max<int32_t>(int8_t(block[1]), -127);
    int32_t p[8] = {a0, a1};
    if (a0 > a1) {
        for (int32_t i = 1; i < 7; ++i)
            p[i + 1] = ((7 - i) * a0 + i * a1) / 7;
    } else {
        for (int32_t i = 1; i < 5; ++i)
            p[i + 1] = ((5 - i) * a0 + i * a1) / 5;
        p[6] = -127;
        p[7] = 127;
    }
    for (int i = 0; i < 8; ++i)
        pal[i] = uint8_t(int8_t(p[i]));
}

void channel_palette(const uint8_t* block, bool snorm, uint32_t pal[8]) {
    if (snorm)
        channel_palette_snorm(block, pal);
    else
        channel_palette_unorm(block, pal);
}

// BC4/BC5: (r, g, 0, a) with r from `block`, g from `block2` (or 0).
void decode_channels(const uint8_t* block, const uint8_t* block2, bool snorm, uint8_t* out, size_t stride) {
    uint32_t pr[8], pg[8] = {};
    channel_palette(block, snorm, pr);
    if (block2 != nullptr)
        channel_palette(block2, snorm, pg);
    const uint64_t ir = load64(block) >> 16, ig = block2 != nullptr ? load64(block2) >> 16 : 0;
    const uint32_t a = snorm ? 127u << 24 : 255u << 24;
    for (uint32_t y = 0; y < 4; ++y) {
        uint32_t row[4];
        for (uint32_t x = 0; x < 4; ++x) {
            const uint32_t sh = 3 * (4 * y + x);
            row[x] = pr[(ir >> sh) & 7] | pg[(ig >> sh) & 7] << 8 | a;
        }
        store_row(out + y * stride, row);
    }
}

// BC3 alpha into byte 3 of texels already written.
void decode_alpha(const uint8_t* block, uint8_t* out, size_t stride) {
    uint32_t pal[8];
    channel_palette_unorm(block, pal);
    const uint64_t idx = load64(block) >> 16;
    for (uint32_t i = 0; i < 16; ++i)
        out[(i / 4) * stride + (i % 4) * 4 + 3] = uint8_t(pal[(idx >> (3 * i)) & 7]);
}

struct Bits {
    uint64_t lo, hi;
    uint32_t pos = 0;
    // The 64 bits from pos on (fewer past bit 127), least significant first.
    uint64_t window() const {
        if (pos >= 64)
            return hi >> (pos - 64);
        if (pos == 0)
            return lo;
        return lo >> pos | hi << (64 - pos);
    }
    // n <= 8 bits from pos on (the reference's take()).
    uint32_t take(uint32_t n) {
        const uint64_t v = window();
        pos += n;
        return uint32_t(v & ((uint64_t(1) << n) - 1));
    }
};

// One instance per mode, so the field widths are constants and the loops unroll.
template <uint32_t Mode>
void decode_bc7_mode(Bits bits, uint8_t* out, size_t stride) {
    using namespace ref;
    constexpr Bc7Mode m = kBc7Modes[Mode];
    bits.pos = Mode + 1;
    const uint32_t partition = bits.take(m.partition_bits);
    const uint32_t rotation = bits.take(m.rotation_bits);
    const uint32_t selector = bits.take(m.selector_bits);
    constexpr uint32_t ns = m.subsets;
    uint32_t ep[6][4] = {};
    for (uint32_t c = 0; c < 3; ++c)
        for (uint32_t e = 0; e < ns * 2; ++e)
            ep[e][c] = bits.take(m.colour_bits);
    if constexpr (m.alpha_bits != 0)
        for (uint32_t e = 0; e < ns * 2; ++e)
            ep[e][3] = bits.take(m.alpha_bits);
    constexpr uint32_t pbit = m.endpoint_pbits || m.shared_pbits ? 1 : 0;
    constexpr uint32_t cbits = m.colour_bits + pbit, abits = m.alpha_bits ? m.alpha_bits + pbit : 0;
    if constexpr (pbit != 0) {
        uint32_t p[6];
        if constexpr (m.endpoint_pbits != 0) {
            for (uint32_t e = 0; e < ns * 2; ++e)
                p[e] = bits.take(1);
        } else {
            for (uint32_t s = 0; s < ns; ++s)
                p[2 * s] = p[2 * s + 1] = bits.take(1);
        }
        for (uint32_t e = 0; e < ns * 2; ++e)
            for (uint32_t c = 0; c < 4; ++c)
                ep[e][c] = ep[e][c] << 1 | p[e];
    }
    uint32_t endpoints[6][4];
    for (uint32_t e = 0; e < ns * 2; ++e) {
        for (uint32_t c = 0; c < 3; ++c)
            endpoints[e][c] = expand(ep[e][c], cbits);
        if constexpr (abits != 0)
            endpoints[e][3] = expand(ep[e][3], abits);
        else
            endpoints[e][3] = 255;
    }
    // Subset of each texel and the anchors (one index bit less).
    const uint32_t part = ns == 2 ? kPartition2[partition] : ns == 3 ? kPartition3[partition] : 0;
    const uint32_t subset_shift = ns == 3 ? 2 : 1;
    uint32_t anchors = 1;
    if constexpr (ns == 2)
        anchors |= 1u << kAnchor2[partition];
    else if constexpr (ns == 3)
        anchors |= 1u << kAnchor3a[partition] | 1u << kAnchor3b[partition];
    // The index sets are at most 64 bits each: one window per set.
    uint32_t index[16], index2[16] = {};
    uint64_t iw = bits.window();
    for (uint32_t i = 0; i < 16; ++i) {
        const uint32_t n = m.index_bits - ((anchors >> i) & 1);
        index[i] = uint32_t(iw & ((uint64_t(1) << n) - 1));
        iw >>= n;
    }
    if constexpr (m.index2_bits != 0) {
        bits.pos += 16 * m.index_bits - ns;
        uint64_t iw2 = bits.window();
        for (uint32_t i = 0; i < 16; ++i) {
            const uint32_t n = i == 0 ? m.index2_bits - 1 : m.index2_bits;
            index2[i] = uint32_t(iw2 & ((uint64_t(1) << n) - 1));
            iw2 >>= n;
        }
    }

    const uint8_t* wc = weights(m.index_bits);
    const uint8_t* wa = wc;
    const uint32_t* ic = index;
    const uint32_t* ia = index;
    if constexpr (m.index2_bits != 0) {
        // Modes 4 and 5: the selector bit swaps the colour and alpha sets.
        wc = weights(selector ? m.index2_bits : m.index_bits);
        wa = weights(selector ? m.index_bits : m.index2_bits);
        ic = selector ? index2 : index;
        ia = selector ? index : index2;
    }
    for (uint32_t y = 0; y < 4; ++y) {
        uint32_t row[4];
        for (uint32_t x = 0; x < 4; ++x) {
            const uint32_t i = 4 * y + x;
            const uint32_t s = ns == 1 ? 0 : (part >> (subset_shift * i)) & (ns == 3 ? 3 : 1);
            const uint32_t* e0 = endpoints[2 * s];
            const uint32_t* e1 = endpoints[2 * s + 1];
            const uint32_t w = wc[ic[i]], w2 = wa[ia[i]];
            uint32_t px[4];
            for (uint32_t c = 0; c < 3; ++c)
                px[c] = ((64 - w) * e0[c] + w * e1[c] + 32) >> 6;
            px[3] = ((64 - w2) * e0[3] + w2 * e1[3] + 32) >> 6;
            if (rotation != 0)
                std::swap(px[3], px[rotation - 1]);
            row[x] = pack(px[0], px[1], px[2], px[3]);
        }
        store_row(out + y * stride, row);
    }
}

void decode_bc7(const uint8_t* block, uint8_t* out, size_t stride) {
    const Bits bits{load64(block), load64(block + 8)};
    switch (std::countr_zero(uint32_t(bits.lo & 0xff))) {
    case 0: decode_bc7_mode<0>(bits, out, stride); return;
    case 1: decode_bc7_mode<1>(bits, out, stride); return;
    case 2: decode_bc7_mode<2>(bits, out, stride); return;
    case 3: decode_bc7_mode<3>(bits, out, stride); return;
    case 4: decode_bc7_mode<4>(bits, out, stride); return;
    case 5: decode_bc7_mode<5>(bits, out, stride); return;
    case 6: decode_bc7_mode<6>(bits, out, stride); return;
    case 7: decode_bc7_mode<7>(bits, out, stride); return;
    default: {  // mode byte 0: invalid, transparent black
        const uint32_t row[4] = {};
        for (uint32_t y = 0; y < 4; ++y)
            store_row(out + y * stride, row);
        return;
    }
    }
}

void decode_block(BcFormat format, const uint8_t* block, uint8_t* out, size_t stride) {
    switch (format) {
    case BcFormat::BC1: decode_colour(block, true, out, stride); return;
    case BcFormat::BC2: {
        decode_colour(block + 8, false, out, stride);
        const uint64_t alpha = load64(block);
        for (uint32_t i = 0; i < 16; ++i)
            out[(i / 4) * stride + (i % 4) * 4 + 3] = uint8_t(((alpha >> (4 * i)) & 15) * 17);
        return;
    }
    case BcFormat::BC3:
        decode_colour(block + 8, false, out, stride);
        decode_alpha(block, out, stride);
        return;
    case BcFormat::BC4: decode_channels(block, nullptr, false, out, stride); return;
    case BcFormat::BC4Snorm: decode_channels(block, nullptr, true, out, stride); return;
    case BcFormat::BC5: decode_channels(block, block + 8, false, out, stride); return;
    case BcFormat::BC5Snorm: decode_channels(block, block + 8, true, out, stride); return;
    case BcFormat::BC7: decode_bc7(block, out, stride); return;
    }
}

}  // namespace fast

std::atomic<BcDecoder> g_decoder{BcDecoder::Fast};
}  // namespace

size_t bc_block_bytes(BcFormat format) {
    return format == BcFormat::BC1 || format == BcFormat::BC4 || format == BcFormat::BC4Snorm ? 8 : 16;
}

void set_bc_decoder(BcDecoder decoder) { g_decoder.store(decoder, std::memory_order_relaxed); }
BcDecoder bc_decoder() { return g_decoder.load(std::memory_order_relaxed); }

void decode_bc_block_with(BcDecoder decoder, BcFormat format, const uint8_t* block, uint8_t* out, size_t stride) {
    if (decoder == BcDecoder::Fast)
        fast::decode_block(format, block, out, stride);
    else
        ref::decode_block(format, block, out, stride);
}

void decode_bc_block(BcFormat format, const uint8_t* block, uint8_t* out, size_t stride) {
    decode_bc_block_with(bc_decoder(), format, block, out, stride);
}

void decode_bc_image_with(BcDecoder decoder, BcFormat format, const uint8_t* data, uint32_t width, uint32_t height,
                          uint8_t* out) {
    const size_t bytes = bc_block_bytes(format);
    const uint32_t bx = (width + 3) / 4, by = (height + 3) / 4;
    const size_t stride = size_t(width) * 4;
    uint8_t tmp[4 * 4 * 4];
    for (uint32_t y = 0; y < by; ++y)
        for (uint32_t x = 0; x < bx; ++x) {
            const uint8_t* block = data + (size_t(y) * bx + x) * bytes;
            uint8_t* dst = out + size_t(y) * 4 * stride + size_t(x) * 16;
            const uint32_t w = std::min(4u, width - x * 4), h = std::min(4u, height - y * 4);
            if (decoder == BcDecoder::Fast && w == 4 && h == 4) {
                fast::decode_block(format, block, dst, stride);  // whole block: straight into the image
                continue;
            }
            decode_bc_block_with(decoder, format, block, tmp, 16);
            for (uint32_t r = 0; r < h; ++r)
                std::memcpy(dst + r * stride, tmp + r * 16, size_t(w) * 4);
        }
}

void decode_bc_image(BcFormat format, const uint8_t* data, uint32_t width, uint32_t height, uint8_t* out) {
    decode_bc_image_with(bc_decoder(), format, data, width, height, out);
}

}  // namespace lgp::gpu::texture
