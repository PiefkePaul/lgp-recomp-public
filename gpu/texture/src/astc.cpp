// ASTC LDR decoder, see astc.hpp. Section numbers refer to the Khronos Data
// Format Specification 1.3, chapter 23.
#include "lgp/gpu/texture/astc.hpp"

#include <algorithm>
#include <array>
#include <cstring>

namespace lgp::gpu::texture {
namespace {

constexpr uint32_t kMaxTexels = 12 * 12;
constexpr uint32_t kMaxWeights = 64;
constexpr uint32_t kMaxColorValues = 18;
constexpr uint8_t kErrorColor[4] = {0xFF, 0x00, 0xFF, 0xFF};

// ---------------------------------------------------------------------------
// Bit access. Bit i of the block is bit (i % 8) of byte i / 8.

uint32_t get_bits(const uint8_t* data, uint32_t pos, uint32_t count) {
    uint32_t v = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t p = pos + i;
        if (p < 128)
            v |= uint32_t((data[p >> 3] >> (p & 7)) & 1) << i;
    }
    return v;
}

uint8_t reverse_byte(uint8_t b) {
    b = uint8_t((b & 0xF0) >> 4 | (b & 0x0F) << 4);
    b = uint8_t((b & 0xCC) >> 2 | (b & 0x33) << 2);
    b = uint8_t((b & 0xAA) >> 1 | (b & 0x55) << 1);
    return b;
}

// Sequential reader limited to [pos, end): bits past the end read as zero
// (the last trit/quint group of a sequence may be cut short, 23.12).
struct BitReader {
    const uint8_t* data;
    uint32_t pos, end;
    uint32_t read(uint32_t count) {
        uint32_t v = 0;
        for (uint32_t i = 0; i < count; ++i, ++pos)
            if (pos < end)
                v |= get_bits(data, pos, 1) << i;
        return v;
    }
};

// ---------------------------------------------------------------------------
// Integer sequence encoding (23.12).

struct Range {
    uint32_t trits = 0, quints = 0, bits = 0;
};

// A range of `levels` values: levels = 2^b, 3 * 2^b or 5 * 2^b.
Range range_of(uint32_t levels) {
    Range r;
    if (levels % 3 == 0) {
        r.trits = 1;
        levels /= 3;
    } else if (levels % 5 == 0) {
        r.quints = 1;
        levels /= 5;
    }
    while (levels > 1) {
        ++r.bits;
        levels >>= 1;
    }
    return r;
}

uint32_t ise_bits(uint32_t levels, uint32_t count) {
    const Range r = range_of(levels);
    uint32_t n = count * r.bits;
    if (r.trits)
        n += (count * 8 + 4) / 5;
    if (r.quints)
        n += (count * 7 + 2) / 3;
    return n;
}

void decode_trits(uint32_t t, uint32_t out[5]) {
    auto bit = [](uint32_t v, int i) { return (v >> i) & 1; };
    uint32_t c, t4, t3, t2, t1, t0;
    if (((t >> 2) & 7) == 7) {
        c = ((t >> 5) & 7) << 2 | (t & 3);
        t4 = t3 = 2;
    } else {
        c = t & 0x1F;
        if (((t >> 5) & 3) == 3) {
            t4 = 2;
            t3 = bit(t, 7);
        } else {
            t4 = bit(t, 7);
            t3 = (t >> 5) & 3;
        }
    }
    if ((c & 3) == 3) {
        t2 = 2;
        t1 = bit(c, 4);
        t0 = bit(c, 3) << 1 | (bit(c, 2) & ~bit(c, 3) & 1);
    } else if (((c >> 2) & 3) == 3) {
        t2 = 2;
        t1 = 2;
        t0 = c & 3;
    } else {
        t2 = bit(c, 4);
        t1 = (c >> 2) & 3;
        t0 = bit(c, 1) << 1 | (bit(c, 0) & ~bit(c, 1) & 1);
    }
    out[0] = t0;
    out[1] = t1;
    out[2] = t2;
    out[3] = t3;
    out[4] = t4;
}

void decode_quints(uint32_t q, uint32_t out[3]) {
    auto bit = [](uint32_t v, int i) { return (v >> i) & 1; };
    uint32_t q2, q1, q0;
    if (((q >> 1) & 3) == 3 && ((q >> 5) & 3) == 0) {
        q2 = bit(q, 0) << 2 | (bit(q, 4) & ~bit(q, 0) & 1) << 1 | (bit(q, 3) & ~bit(q, 0) & 1);
        q1 = q0 = 4;
    } else {
        uint32_t c;
        if (((q >> 1) & 3) == 3) {
            q2 = 4;
            c = ((q >> 3) & 3) << 3 | (~(q >> 5) & 3) << 1 | bit(q, 0);
        } else {
            q2 = (q >> 5) & 3;
            c = q & 0x1F;
        }
        if ((c & 7) == 5) {
            q1 = 4;
            q0 = (c >> 3) & 3;
        } else {
            q1 = (c >> 3) & 3;
            q0 = c & 7;
        }
    }
    out[0] = q0;
    out[1] = q1;
    out[2] = q2;
}

// Decodes `count` values; each value is (trit/quint << bits) | bits, returned
// as the pair (digit, low bits) for unquantization.
struct IseValue {
    uint32_t digit;  // trit or quint (0 for pure bit ranges)
    uint32_t low;    // the `bits` low bits
};

void decode_ise(const uint8_t* data, uint32_t start, uint32_t levels, uint32_t count, IseValue* out) {
    const Range r = range_of(levels);
    BitReader br{data, start, start + ise_bits(levels, count)};
    uint32_t i = 0;
    if (r.trits) {
        // Groups of 5: m0 T[1:0] m1 T[3:2] m2 T[4] m3 T[6:5] m4 T[7].
        static constexpr uint32_t kTBits[5] = {2, 2, 1, 2, 1};
        while (i < count) {
            uint32_t m[5], t = 0, shift = 0;
            for (int k = 0; k < 5; ++k) {
                m[k] = br.read(r.bits);
                t |= br.read(kTBits[k]) << shift;
                shift += kTBits[k];
            }
            uint32_t d[5];
            decode_trits(t, d);
            for (int k = 0; k < 5 && i < count; ++k, ++i)
                out[i] = {d[k], m[k]};
        }
    } else if (r.quints) {
        // Groups of 3: m0 Q[2:0] m1 Q[4:3] m2 Q[6:5].
        static constexpr uint32_t kQBits[3] = {3, 2, 2};
        while (i < count) {
            uint32_t m[3], q = 0, shift = 0;
            for (int k = 0; k < 3; ++k) {
                m[k] = br.read(r.bits);
                q |= br.read(kQBits[k]) << shift;
                shift += kQBits[k];
            }
            uint32_t d[3];
            decode_quints(q, d);
            for (int k = 0; k < 3 && i < count; ++k, ++i)
                out[i] = {d[k], m[k]};
        }
    } else {
        for (; i < count; ++i)
            out[i] = {0, br.read(r.bits)};
    }
}

// Bit `i` of v replicated: helper for the unquantization bit patterns.
uint32_t b(uint32_t v, int i) { return (v >> i) & 1; }

// Colour endpoint unquantization to 0..255 (23.13, table "Color
// Unquantization Parameters").
uint32_t unquantize_color(const Range& r, IseValue v) {
    if (!r.trits && !r.quints) {
        // Bit replication of `bits` bits to 8.
        uint32_t x = v.low, n = r.bits, out = 0;
        int have = 0;
        while (have < 8) {
            out = (out << n) | x;
            have += int(n);
        }
        return out >> (have - 8);
    }
    const uint32_t m = v.low;
    const uint32_t a = (m & 1) ? 0x1FF : 0;
    uint32_t bb = 0, c = 0;
    if (r.trits) {
        switch (r.bits) {
        case 1: c = 204; break;
        case 2: bb = b(m, 1) << 8 | b(m, 1) << 4 | b(m, 1) << 2 | b(m, 1) << 1; c = 93; break;   // b000b0bb0
        case 3: bb = (m >> 1 & 3) << 7 | (m >> 1 & 3) << 2 | (m >> 1 & 3); c = 44; break;  // cb000cbcb
        case 4: bb = (m >> 1 & 7) << 6 | (m >> 1 & 7); c = 22; break;                            // dcb000dcb
        case 5: bb = (m >> 1 & 0xF) << 5 | (m >> 3 & 3); c = 11; break;                          // edcb000ed
        case 6: bb = (m >> 1 & 0x1F) << 4 | (m >> 5 & 1); c = 5; break;                          // fedcb000f
        default: return 0;
        }
    } else {
        switch (r.bits) {
        case 1: c = 113; break;
        case 2: bb = b(m, 1) << 8 | b(m, 1) << 3 | b(m, 1) << 2; c = 54; break;                  // b0000bb00
        case 3: bb = (m >> 1 & 3) << 7 | (m >> 1 & 3) << 1 | (m >> 2 & 1); c = 26; break;       // cb0000cbc
        case 4: bb = (m >> 1 & 7) << 6 | (m >> 2 & 3); c = 13; break;                            // dcb0000dc
        case 5: bb = (m >> 1 & 0xF) << 5 | (m >> 4 & 1); c = 6; break;                           // edcb0000e
        default: return 0;
        }
    }
    uint32_t t = v.digit * c + bb;
    t ^= a;
    return (a & 0x80) | (t >> 2);
}

// Weight unquantization to 0..64 (23.17).
uint32_t unquantize_weight(const Range& r, IseValue v) {
    uint32_t t;
    if (!r.trits && !r.quints) {
        uint32_t x = v.low, n = r.bits, out = 0;
        int have = 0;
        while (have < 6) {
            out = (out << n) | x;
            have += int(n);
        }
        t = out >> (have - 6);
    } else if (r.bits == 0) {
        t = r.trits ? v.digit * 32 : v.digit * 16;
        return t;  // already 0..64 (0, 32, 64 / 0, 16, ..., 64)
    } else {
        const uint32_t m = v.low;
        const uint32_t a = (m & 1) ? 0x7F : 0;
        uint32_t bb = 0, c = 0;
        if (r.trits) {
            switch (r.bits) {
            case 1: c = 50; break;
            case 2: bb = b(m, 1) << 6 | b(m, 1) << 2 | b(m, 1); c = 23; break;     // b000b0b
            case 3: bb = (m >> 1 & 3) << 5 | (m >> 1 & 3); c = 11; break;         // cb000cb
            default: return 0;
            }
        } else {
            switch (r.bits) {
            case 1: c = 28; break;
            case 2: bb = b(m, 1) << 6 | b(m, 1) << 1; c = 13; break;              // b0000b0
            default: return 0;
            }
        }
        t = v.digit * c + bb;
        t ^= a;
        t = (a & 0x20) | (t >> 2);
    }
    if (t > 32)
        ++t;
    return t;
}

// ---------------------------------------------------------------------------
// Partition selection (23.19).

uint32_t hash52(uint32_t p) {
    p ^= p >> 15;
    p -= p << 17;
    p += p << 7;
    p += p << 4;
    p ^= p >> 5;
    p += p << 16;
    p ^= p >> 7;
    p ^= p >> 3;
    p ^= p << 6;
    p ^= p >> 17;
    return p;
}

uint32_t select_partition(uint32_t seed, uint32_t x, uint32_t y, uint32_t z, uint32_t count, bool small_block) {
    if (small_block) {
        x <<= 1;
        y <<= 1;
        z <<= 1;
    }
    seed += (count - 1) * 1024;
    const uint32_t rnum = hash52(seed);
    uint32_t s[13];
    s[1] = rnum & 0xF;
    s[2] = (rnum >> 4) & 0xF;
    s[3] = (rnum >> 8) & 0xF;
    s[4] = (rnum >> 12) & 0xF;
    s[5] = (rnum >> 16) & 0xF;
    s[6] = (rnum >> 20) & 0xF;
    s[7] = (rnum >> 24) & 0xF;
    s[8] = (rnum >> 28) & 0xF;
    s[9] = (rnum >> 18) & 0xF;
    s[10] = (rnum >> 22) & 0xF;
    s[11] = (rnum >> 26) & 0xF;
    s[12] = ((rnum >> 30) | (rnum << 2)) & 0xF;
    for (int i = 1; i <= 12; ++i)
        s[i] *= s[i];
    uint32_t sh1, sh2;
    if (seed & 1) {
        sh1 = (seed & 2) ? 4 : 5;
        sh2 = (count == 3) ? 6 : 5;
    } else {
        sh1 = (count == 3) ? 6 : 5;
        sh2 = (seed & 2) ? 4 : 5;
    }
    const uint32_t sh3 = (seed & 0x10) ? sh1 : sh2;
    for (int i = 1; i <= 8; ++i)
        s[i] >>= (i & 1) ? sh1 : sh2;
    for (int i = 9; i <= 12; ++i)
        s[i] >>= sh3;
    uint32_t a = s[1] * x + s[2] * y + s[11] * z + (rnum >> 14);
    uint32_t bv = s[3] * x + s[4] * y + s[12] * z + (rnum >> 10);
    uint32_t c = s[5] * x + s[6] * y + s[9] * z + (rnum >> 6);
    uint32_t d = s[7] * x + s[8] * y + s[10] * z + (rnum >> 2);
    a &= 0x3F;
    bv &= 0x3F;
    c &= 0x3F;
    d &= 0x3F;
    if (count < 4)
        d = 0;
    if (count < 3)
        c = 0;
    if (a >= bv && a >= c && a >= d)
        return 0;
    if (bv >= c && bv >= d)
        return 1;
    if (c >= d)
        return 2;
    return 3;
}

// ---------------------------------------------------------------------------
// Block mode (23.10).

struct BlockMode {
    uint32_t grid_w = 0, grid_h = 0;
    uint32_t weight_levels = 0;
    bool dual_plane = false;
};

bool decode_block_mode(uint32_t mode, BlockMode* bm) {
    static constexpr uint32_t kLevels[2][8] = {{0, 0, 2, 3, 4, 5, 6, 8}, {0, 0, 10, 12, 16, 20, 24, 32}};
    uint32_t r, w = 0, h = 0;
    uint32_t high = (mode >> 9) & 1;
    bool dual = (mode >> 10) & 1;
    const uint32_t a = (mode >> 5) & 3;
    if (mode & 3) {
        r = ((mode >> 4) & 1) | (mode & 3) << 1;
        switch ((mode >> 2) & 3) {
        case 0: w = ((mode >> 7) & 3) + 4; h = a + 2; break;
        case 1: w = ((mode >> 7) & 3) + 8; h = a + 2; break;
        case 2: w = a + 2; h = ((mode >> 7) & 3) + 8; break;
        default:
            if (mode & 0x100) {
                w = ((mode >> 7) & 1) + 2;
                h = a + 2;
            } else {
                w = a + 2;
                h = ((mode >> 7) & 1) + 6;
            }
            break;
        }
    } else {
        r = ((mode >> 4) & 1) | ((mode >> 2) & 3) << 1;
        if (((mode >> 2) & 3) == 0)
            return false;  // reserved (xxxxxxx0000)
        switch ((mode >> 7) & 3) {
        case 0: w = 12; h = a + 2; break;
        case 1: w = a + 2; h = 12; break;
        case 2:
            w = a + 6;
            h = ((mode >> 9) & 3) + 6;
            high = 0;
            dual = false;
            break;
        default:
            if (a == 0) {
                w = 6;
                h = 10;
            } else if (a == 1) {
                w = 10;
                h = 6;
            } else {
                return false;  // reserved
            }
            break;
        }
    }
    if (r < 2)
        return false;
    bm->grid_w = w;
    bm->grid_h = h;
    bm->weight_levels = kLevels[high][r];
    bm->dual_plane = dual;
    return true;
}

// ---------------------------------------------------------------------------
// Colour endpoint decoding (23.14), LDR modes. Returns false for HDR modes.

using Rgba = std::array<int, 4>;

void bit_transfer_signed(int& a, int& bb) {
    bb >>= 1;
    bb |= a & 0x80;
    a >>= 1;
    a &= 0x3F;
    if (a & 0x20)
        a -= 0x40;
}

Rgba blue_contract(int r, int g, int bl, int a) { return {(r + bl) >> 1, (g + bl) >> 1, bl, a}; }

int clamp255(int v) { return std::clamp(v, 0, 255); }

bool decode_endpoints(uint32_t cem, const uint32_t* vin, Rgba* e0, Rgba* e1) {
    int v[8];
    for (int i = 0; i < 8; ++i)
        v[i] = int(vin[i]);
    switch (cem) {
    case 0:
        *e0 = {v[0], v[0], v[0], 255};
        *e1 = {v[1], v[1], v[1], 255};
        break;
    case 1: {
        const int l0 = (v[0] >> 2) | (v[1] & 0xC0);
        const int l1 = std::min(l0 + (v[1] & 0x3F), 255);
        *e0 = {l0, l0, l0, 255};
        *e1 = {l1, l1, l1, 255};
        break;
    }
    case 4:
        *e0 = {v[0], v[0], v[0], v[2]};
        *e1 = {v[1], v[1], v[1], v[3]};
        break;
    case 5:
        bit_transfer_signed(v[1], v[0]);
        bit_transfer_signed(v[3], v[2]);
        *e0 = {v[0], v[0], v[0], v[2]};
        *e1 = {v[0] + v[1], v[0] + v[1], v[0] + v[1], v[2] + v[3]};
        break;
    case 6:
        *e0 = {(v[0] * v[3]) >> 8, (v[1] * v[3]) >> 8, (v[2] * v[3]) >> 8, 255};
        *e1 = {v[0], v[1], v[2], 255};
        break;
    case 8:
        if (v[1] + v[3] + v[5] >= v[0] + v[2] + v[4]) {
            *e0 = {v[0], v[2], v[4], 255};
            *e1 = {v[1], v[3], v[5], 255};
        } else {
            *e0 = blue_contract(v[1], v[3], v[5], 255);
            *e1 = blue_contract(v[0], v[2], v[4], 255);
        }
        break;
    case 9:
        bit_transfer_signed(v[1], v[0]);
        bit_transfer_signed(v[3], v[2]);
        bit_transfer_signed(v[5], v[4]);
        if (v[1] + v[3] + v[5] >= 0) {
            *e0 = {v[0], v[2], v[4], 255};
            *e1 = {v[0] + v[1], v[2] + v[3], v[4] + v[5], 255};
        } else {
            *e0 = blue_contract(v[0] + v[1], v[2] + v[3], v[4] + v[5], 255);
            *e1 = blue_contract(v[0], v[2], v[4], 255);
        }
        break;
    case 10:
        *e0 = {(v[0] * v[3]) >> 8, (v[1] * v[3]) >> 8, (v[2] * v[3]) >> 8, v[4]};
        *e1 = {v[0], v[1], v[2], v[5]};
        break;
    case 12:
        if (v[1] + v[3] + v[5] >= v[0] + v[2] + v[4]) {
            *e0 = {v[0], v[2], v[4], v[6]};
            *e1 = {v[1], v[3], v[5], v[7]};
        } else {
            *e0 = blue_contract(v[1], v[3], v[5], v[7]);
            *e1 = blue_contract(v[0], v[2], v[4], v[6]);
        }
        break;
    case 13:
        bit_transfer_signed(v[1], v[0]);
        bit_transfer_signed(v[3], v[2]);
        bit_transfer_signed(v[5], v[4]);
        bit_transfer_signed(v[7], v[6]);
        if (v[1] + v[3] + v[5] >= 0) {
            *e0 = {v[0], v[2], v[4], v[6]};
            *e1 = {v[0] + v[1], v[2] + v[3], v[4] + v[5], v[6] + v[7]};
        } else {
            *e0 = blue_contract(v[0] + v[1], v[2] + v[3], v[4] + v[5], v[6] + v[7]);
            *e1 = blue_contract(v[0], v[2], v[4], v[6]);
        }
        break;
    default: return false;  // 2, 3, 7, 11, 14, 15: HDR
    }
    for (int c = 0; c < 4; ++c) {
        (*e0)[c] = clamp255((*e0)[c]);
        (*e1)[c] = clamp255((*e1)[c]);
    }
    return true;
}

void fill(uint8_t* out, size_t stride, uint32_t bw, uint32_t bh, const uint8_t rgba[4]) {
    for (uint32_t y = 0; y < bh; ++y)
        for (uint32_t x = 0; x < bw; ++x)
            std::memcpy(out + y * stride + x * 4, rgba, 4);
}

bool error_block(uint8_t* out, size_t stride, uint32_t bw, uint32_t bh) {
    fill(out, stride, bw, bh, kErrorColor);
    return false;
}

// Void-extent block (23.23): one constant colour.
bool decode_void_extent(const uint8_t* blk, uint32_t bw, uint32_t bh, uint8_t* out, size_t stride) {
    if (get_bits(blk, 9, 1))
        return error_block(out, stride, bw, bh);  // HDR (FP16 colour), not in the LDR profile
    if (get_bits(blk, 10, 2) != 3)
        return error_block(out, stride, bw, bh);  // reserved bits must be 11
    const uint32_t s0 = get_bits(blk, 12, 13), s1 = get_bits(blk, 25, 13);
    const uint32_t t0 = get_bits(blk, 38, 13), t1 = get_bits(blk, 51, 13);
    const bool all_ones = s0 == 0x1FFF && s1 == 0x1FFF && t0 == 0x1FFF && t1 == 0x1FFF;
    if (!all_ones && (s0 >= s1 || t0 >= t1))
        return error_block(out, stride, bw, bh);
    uint8_t rgba[4];
    for (int c = 0; c < 4; ++c)
        rgba[c] = uint8_t(get_bits(blk, 64 + 16 * c, 16) >> 8);
    fill(out, stride, bw, bh, rgba);
    return true;
}

}  // namespace

bool astc_footprint_valid(uint32_t bw, uint32_t bh) {
    static constexpr uint32_t kFootprints[][2] = {{4, 4},  {5, 4},  {5, 5},  {6, 5},   {6, 6},   {8, 5},   {8, 6},
                                                  {8, 8},  {10, 5}, {10, 6}, {10, 8}, {10, 10}, {12, 10}, {12, 12}};
    for (const auto& f : kFootprints)
        if (f[0] == bw && f[1] == bh)
            return true;
    return false;
}

bool decode_astc_block(const uint8_t* blk, uint32_t bw, uint32_t bh, bool srgb, uint8_t* out, size_t stride) {
    if (!astc_footprint_valid(bw, bh))
        return error_block(out, stride, std::min(bw, 12u), std::min(bh, 12u));

    const uint32_t mode = get_bits(blk, 0, 11);
    if ((mode & 0x1FF) == 0x1FC)
        return decode_void_extent(blk, bw, bh, out, stride);

    BlockMode bm;
    if (!decode_block_mode(mode, &bm))
        return error_block(out, stride, bw, bh);
    const uint32_t planes = bm.dual_plane ? 2 : 1;
    const uint32_t weight_count = bm.grid_w * bm.grid_h * planes;
    if (bm.grid_w > bw || bm.grid_h > bh || weight_count > kMaxWeights)
        return error_block(out, stride, bw, bh);
    const uint32_t weight_bits = ise_bits(bm.weight_levels, weight_count);
    if (weight_bits < 24 || weight_bits > 96)
        return error_block(out, stride, bw, bh);

    const uint32_t partitions = get_bits(blk, 11, 2) + 1;
    if (partitions == 4 && bm.dual_plane)
        return error_block(out, stride, bw, bh);

    // Colour endpoint modes (23.11).
    uint32_t cem[4] = {};
    uint32_t color_start, below_weights = 128 - weight_bits;
    uint32_t partition_seed = 0;
    if (partitions == 1) {
        cem[0] = get_bits(blk, 13, 4);
        color_start = 17;
    } else {
        partition_seed = get_bits(blk, 13, 10);
        const uint32_t low = get_bits(blk, 23, 6);
        color_start = 29;
        const uint32_t selector = low & 3;
        if (selector == 0) {
            for (uint32_t p = 0; p < partitions; ++p)
                cem[p] = (low >> 2) & 0xF;
        } else {
            const uint32_t extra_bits = 3 * partitions - 4;
            below_weights -= extra_bits;
            const uint32_t encoded = get_bits(blk, below_weights, extra_bits) << 6 | low;
            uint32_t pos = 2;
            for (uint32_t p = 0; p < partitions; ++p, ++pos)
                cem[p] = ((encoded >> pos) & 1) + selector - 1;
            for (uint32_t p = 0; p < partitions; ++p, pos += 2)
                cem[p] = cem[p] << 2 | ((encoded >> pos) & 3);
        }
    }
    uint32_t ccs = 0;
    if (bm.dual_plane) {
        below_weights -= 2;
        ccs = get_bits(blk, below_weights, 2);
    }

    uint32_t color_count = 0;
    for (uint32_t p = 0; p < partitions; ++p)
        color_count += ((cem[p] >> 2) + 1) * 2;
    if (color_count > kMaxColorValues || below_weights <= color_start)
        return error_block(out, stride, bw, bh);
    const uint32_t color_bits = below_weights - color_start;
    if (color_bits < (13 * color_count + 4) / 5)
        return error_block(out, stride, bw, bh);
    // The largest colour range whose encoding fits (23.13).
    static constexpr uint32_t kColorLevels[] = {256, 192, 160, 128, 96, 80, 64, 48, 40, 32, 24,
                                                20,  16,  12,  10,  8,  6,  5,  4,  3,  2};
    uint32_t color_levels = 0;
    for (uint32_t levels : kColorLevels) {
        if (ise_bits(levels, color_count) <= color_bits) {
            color_levels = levels;
            break;
        }
    }
    if (color_levels < 6)
        return error_block(out, stride, bw, bh);

    IseValue raw[kMaxColorValues];
    decode_ise(blk, color_start, color_levels, color_count, raw);
    const Range color_range = range_of(color_levels);
    uint32_t colors[kMaxColorValues];
    for (uint32_t i = 0; i < color_count; ++i)
        colors[i] = unquantize_color(color_range, raw[i]);

    // 16-bit endpoints per partition.
    int ep[4][2][4];
    uint32_t ci = 0;
    for (uint32_t p = 0; p < partitions; ++p) {
        uint32_t vals[8] = {};
        const uint32_t n = ((cem[p] >> 2) + 1) * 2;
        std::copy(colors + ci, colors + ci + n, vals);
        ci += n;
        Rgba e0, e1;
        const bool ldr = decode_endpoints(cem[p], vals, &e0, &e1);
        for (int c = 0; c < 4; ++c) {
            const int a0 = ldr ? e0[c] : kErrorColor[c];
            const int a1 = ldr ? e1[c] : kErrorColor[c];
            ep[p][0][c] = srgb ? (a0 << 8 | 0x80) : a0 * 257;
            ep[p][1][c] = srgb ? (a1 << 8 | 0x80) : a1 * 257;
        }
    }

    // Weights: stored bit-reversed from the top of the block (23.16).
    uint8_t rev[16];
    for (int i = 0; i < 16; ++i)
        rev[i] = reverse_byte(blk[15 - i]);
    IseValue wraw[kMaxWeights];
    decode_ise(rev, 0, bm.weight_levels, weight_count, wraw);
    const Range weight_range = range_of(bm.weight_levels);
    uint32_t grid[kMaxWeights];
    for (uint32_t i = 0; i < weight_count; ++i)
        grid[i] = unquantize_weight(weight_range, wraw[i]);

    // Infill to texel weights (23.18); planes interleaved in the grid.
    uint32_t tw[2][kMaxTexels];
    const uint32_t ds = (1024 + bw / 2) / (bw - 1), dt = (1024 + bh / 2) / (bh - 1);
    const uint32_t gw = bm.grid_w, gh = bm.grid_h;
    for (uint32_t t = 0; t < bh; ++t) {
        for (uint32_t s = 0; s < bw; ++s) {
            const uint32_t gs = (ds * s * (gw - 1) + 32) >> 6, gt = (dt * t * (gh - 1) + 32) >> 6;
            const uint32_t js = gs >> 4, fs = gs & 0xF, jt = gt >> 4, ft = gt & 0xF;
            const uint32_t w11 = (fs * ft + 8) >> 4, w10 = ft - w11, w01 = fs - w11, w00 = 16 - fs - ft + w11;
            const uint32_t v0 = js + jt * gw;
            for (uint32_t pl = 0; pl < planes; ++pl) {
                auto at = [&](uint32_t idx) { return idx < gw * gh ? grid[idx * planes + pl] : 0u; };
                const uint32_t p00 = at(v0), p01 = at(v0 + 1), p10 = at(v0 + gw), p11 = at(v0 + gw + 1);
                tw[pl][t * bw + s] = (p00 * w00 + p01 * w01 + p10 * w10 + p11 * w11 + 8) >> 4;
            }
        }
    }

    const bool small = bw * bh < 31;
    for (uint32_t y = 0; y < bh; ++y) {
        for (uint32_t x = 0; x < bw; ++x) {
            const uint32_t p = partitions > 1 ? select_partition(partition_seed, x, y, 0, partitions, small) : 0;
            uint8_t* o = out + y * stride + x * 4;
            for (uint32_t c = 0; c < 4; ++c) {
                const uint32_t w = (bm.dual_plane && c == ccs) ? tw[1][y * bw + x] : tw[0][y * bw + x];
                const uint32_t v = (uint32_t(ep[p][0][c]) * (64 - w) + uint32_t(ep[p][1][c]) * w + 32) >> 6;
                o[c] = uint8_t(v >> 8);
            }
        }
    }
    return true;
}

size_t decode_astc_image(const uint8_t* data, uint32_t width, uint32_t height, uint32_t bw, uint32_t bh, bool srgb,
                         uint8_t* out) {
    if (!astc_footprint_valid(bw, bh))
        return 0;
    const uint32_t bx = (width + bw - 1) / bw, by = (height + bh - 1) / bh;
    uint8_t tile[kMaxTexels * 4];
    size_t errors = 0;
    for (uint32_t j = 0; j < by; ++j) {
        for (uint32_t i = 0; i < bx; ++i) {
            if (!decode_astc_block(data + (size_t(j) * bx + i) * kAstcBlockBytes, bw, bh, srgb, tile, bw * 4))
                ++errors;
            const uint32_t x0 = i * bw, y0 = j * bh;
            const uint32_t cw = std::min(bw, width - x0), ch = std::min(bh, height - y0);
            for (uint32_t y = 0; y < ch; ++y)
                std::memcpy(out + (size_t(y0 + y) * width + x0) * 4, tile + y * bw * 4, size_t(cw) * 4);
        }
    }
    return errors;
}

}  // namespace lgp::gpu::texture
