#include "lgp/gpu/nvn/texture_layout.hpp"

#include <algorithm>
#include <cstring>

namespace lgp::gpu::nvn {
namespace {

constexpr uint32_t kGobWidth = 64;   // bytes
constexpr uint32_t kGobHeight = 8;   // rows
constexpr uint32_t kGobSize = kGobWidth * kGobHeight;
constexpr uint32_t kMaxGobHeight = 16;

uint32_t div_up(uint32_t v, uint32_t d) { return (v + d - 1) / d; }
uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) / a * a; }

// Block height in GOBs for `rows` rows: shrink from `start` while half of it
// still covers the rows (Ryujinx GetMipGobBlockSizes).
uint32_t fit_gob_height(uint32_t rows, uint32_t start) {
    uint32_t g = start;
    while (g > 1 && rows <= (g / 2) * kGobHeight)
        g /= 2;
    return g;
}

// Block depth of a 3D texture: largest power of two <= 1.5 * depth, at most
// 16 (deko3d pickTileSize).
uint32_t pick_gob_depth(uint32_t depth) {
    const uint32_t d = depth + depth / 2;
    uint32_t g = 1;
    while (g < kMaxGobHeight && g * 2 <= d)
        g *= 2;
    return g;
}

// Shrinks a block depth for a smaller level (deko3d adjustTileSize, unit 1).
uint32_t fit_gob_depth(uint32_t depth, uint32_t start) {
    uint32_t g = start;
    while (g > 1 && depth <= g / 2)
        g /= 2;
    return g;
}

}  // namespace

TextureLayout block_linear_layout(uint32_t bytes_per_block, uint32_t block_width, uint32_t block_height,
                                  uint32_t width, uint32_t height, uint32_t depth, uint32_t layers, uint32_t levels,
                                  int32_t gob_height_log2, bool volume) {
    TextureLayout t;
    bytes_per_block = std::max(bytes_per_block, 1u);
    block_width = std::max(block_width, 1u);
    block_height = std::max(block_height, 1u);
    width = std::max(width, 1u);
    height = std::max(height, 1u);
    depth = std::max(depth, 1u);
    t.layers = std::max(layers, 1u);
    levels = std::max(levels, 1u);
    // Level-0 block height; NVN's default (no explicit block height set).
    // 3D textures: one GOB high (unless the texture file says otherwise), deep blocks.
    const uint32_t base_gob_height =
        gob_height_log2 >= 0
            ? fit_gob_height(div_up(height, block_height), 1u << std::min<int32_t>(gob_height_log2, 4))
        : volume ? 1
                 : fit_gob_height(div_up(height, block_height), kMaxGobHeight);
    const uint32_t base_gob_depth = volume ? pick_gob_depth(depth) : 1;
    if (!volume)
        depth = 1;
    uint64_t offset = 0;
    for (uint32_t l = 0; l < levels; ++l) {
        LevelLayout lv;
        lv.width = std::max(width >> l, 1u);
        lv.height = std::max(height >> l, 1u);
        lv.depth = std::max(depth >> l, 1u);
        lv.blocks_x = div_up(lv.width, block_width);
        lv.blocks_y = div_up(lv.height, block_height);
        lv.row_bytes = lv.blocks_x * bytes_per_block;
        lv.pitch = uint32_t(align_up(lv.row_bytes, kGobWidth));
        lv.gob_height = fit_gob_height(lv.blocks_y, base_gob_height);
        lv.gob_depth = fit_gob_depth(lv.depth, base_gob_depth);
        const uint64_t rows = align_up(lv.blocks_y, uint64_t(kGobHeight) * lv.gob_height);
        lv.offset = offset;
        lv.size = uint64_t(lv.pitch) * rows * align_up(lv.depth, lv.gob_depth);
        offset += lv.size;
        t.levels.push_back(lv);
    }
    // Layers start on a whole block of the level-0 block height.
    t.layer_size = align_up(offset, uint64_t(kGobSize) * base_gob_height);
    t.total_size = t.layer_size * t.layers;
    if (t.layers == 1)
        t.total_size = offset;  // a single layer needs no trailing padding
    return t;
}

uint64_t block_linear_offset(uint32_t x, uint32_t y, uint32_t pitch, uint32_t gob_height) {
    const uint32_t gobs_per_row = pitch / kGobWidth;
    const uint32_t gob_x = x / kGobWidth, gob_y = y / kGobHeight;
    const uint64_t block = uint64_t(gob_y / gob_height) * gobs_per_row + gob_x;
    const uint64_t base = block * (uint64_t(kGobSize) * gob_height) + uint64_t(gob_y % gob_height) * kGobSize;
    // Inside a GOB: 2 halves of 32 bytes x 8 rows, each made of 16-byte x 2-row sectors.
    const uint32_t inner = ((x & 0x3F) >> 5) << 8 | ((y & 7) >> 1) << 6 | ((x & 0x1F) >> 4) << 5 | (y & 1) << 4 |
                           (x & 0xF);
    return base + inner;
}

uint64_t block_linear_offset(const LevelLayout& lv, uint32_t x, uint32_t y, uint32_t z) {
    const uint32_t gobs_per_row = lv.pitch / kGobWidth;
    const uint32_t blocks_high = div_up(div_up(lv.blocks_y, kGobHeight), lv.gob_height);
    const uint32_t gob_x = x / kGobWidth, gob_y = y / kGobHeight;
    const uint64_t block =
        (uint64_t(z / lv.gob_depth) * blocks_high + gob_y / lv.gob_height) * gobs_per_row + gob_x;
    const uint64_t gob_in_block = uint64_t(z % lv.gob_depth) * lv.gob_height + gob_y % lv.gob_height;
    const uint64_t base = block * (uint64_t(kGobSize) * lv.gob_height * lv.gob_depth) + gob_in_block * kGobSize;
    return base + block_linear_offset(x & 0x3F, y & 7, kGobWidth, 1);
}

bool deswizzle_level(const LevelLayout& lv, const uint8_t* src, uint64_t src_size, std::vector<uint8_t>* out) {
    const uint32_t depth = std::max(lv.depth, 1u);
    out->resize(size_t(lv.row_bytes) * lv.blocks_y * depth);
    // Per row: where its first GOB starts, then one block (gob_height x
    // gob_depth GOBs) further for each next GOB of the row. Inside a GOB a
    // row is four 16-byte sectors at fixed offsets (block_linear_offset).
    const uint64_t block_size = uint64_t(kGobSize) * lv.gob_height * lv.gob_depth;
    for (uint32_t i = 0; i < lv.blocks_y * depth; ++i) {
        const uint32_t y = i % lv.blocks_y, z = i / lv.blocks_y;
        uint8_t* dst = out->data() + size_t(i) * lv.row_bytes;
        const uint64_t row_base = block_linear_offset(lv, 0, y, z);
        for (uint32_t gx = 0; gx < lv.row_bytes; gx += kGobWidth) {
            const uint64_t gob = row_base + uint64_t(gx / kGobWidth) * block_size;
            const uint32_t end = std::min<uint32_t>(lv.row_bytes - gx, kGobWidth);
            for (uint32_t x = 0; x < end; x += 16) {
                const uint64_t at = gob + ((x >> 5) << 8 | ((x >> 4) & 1) << 5);
                const uint32_t n = std::min<uint32_t>(16, end - x);
                if (at + n > src_size)
                    return false;
                std::memcpy(dst + gx + x, src + at, n);
            }
        }
    }
    return true;
}

}  // namespace lgp::gpu::nvn
