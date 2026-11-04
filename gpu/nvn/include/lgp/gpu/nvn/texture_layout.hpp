// Tegra X1 (Maxwell) block-linear texture layout, as NVN lays out texture
// storage in a memory pool, and the conversion of one level to linear rows.
//
// A GOB is 64 bytes x 8 rows (512 bytes, a fixed swizzle inside). A block is
// one GOB wide and 1..16 GOBs high (block height); blocks follow each other
// row by row. NVN picks the block height from the level-0 height (the
// smallest power of two of GOBs that covers it, at most 16) and lets it
// shrink for smaller mip levels. Each level is padded to whole blocks and
// the levels follow each other; array layers repeat the whole mip chain.
// Compressed formats count in blocks of texels (4x4 for BCn).
// 3D textures add a block depth: a block is then 1 GOB wide, gob_height GOBs
// high and gob_depth slices deep (GOBs inside it y first, then z); blocks
// follow x, then y, then z. For 3D textures the block height is one GOB and
// the block depth the largest power of two <= 1.5 * depth (at most 16),
// shrinking with the levels like the block height: that is what deko3d
// (devkitPro's open NVN-like API, dk_image.cpp) does following "official
// software", UNVERIFIED against LGP's NVN driver.
// Sources: Ryujinx (SizeCalculator, BlockLinearLayout), Tegra X1 TRM, deko3d.
// Details: docs/knowledge/tegra-block-linear-texturen.md.
#pragma once

#include <cstdint>
#include <vector>

namespace lgp::gpu::nvn {

struct LevelLayout {
    uint64_t offset = 0;          // from the start of the layer
    uint64_t size = 0;            // padded bytes of this level (one layer)
    uint32_t width = 0, height = 0, depth = 1;  // texels
    uint32_t blocks_x = 0, blocks_y = 0;        // format blocks (texels for 1x1 formats)
    uint32_t row_bytes = 0;       // blocks_x * bytes per block (linear row)
    uint32_t pitch = 0;           // padded row width in bytes (multiple of 64)
    uint32_t gob_height = 1;      // block height in GOBs for this level
    uint32_t gob_depth = 1;       // block depth in slices (3D textures)
};

struct TextureLayout {
    std::vector<LevelLayout> levels;
    uint32_t layers = 1;
    uint64_t layer_size = 0;      // stride between array layers / cube faces
    uint64_t total_size = 0;      // all layers
};

// bytes_per_block and block_width/height from FormatInfo; depth is the
// number of slices of a 3D texture (volume = true), layers the array size
// (6 for cube maps).
// gob_height_log2 >= 0: level-0 block height given by the texture file
// (NVNpackagedTextureLayout); its tools round down (rows / 8 rounded up to a
// power of two), unlike NVN's own default rule. Levels shrink from it alike.
// volume: 3D texture, blocks also span GOBs in z (block depth).
TextureLayout block_linear_layout(uint32_t bytes_per_block, uint32_t block_width, uint32_t block_height,
                                  uint32_t width, uint32_t height, uint32_t depth, uint32_t layers, uint32_t levels,
                                  int32_t gob_height_log2 = -1, bool volume = false);

// Byte offset of (x_bytes, y_rows) inside one 2D slice of a block-linear
// surface with the given padded pitch and block height.
uint64_t block_linear_offset(uint32_t x_bytes, uint32_t y, uint32_t pitch, uint32_t gob_height);

// The same for slice z of a level (3D textures; equals the 2D form for z 0
// and gob_depth 1).
uint64_t block_linear_offset(const LevelLayout& level, uint32_t x_bytes, uint32_t y, uint32_t z);

// One level: src points at the level (layer offset + level offset applied),
// out receives depth slices of blocks_y rows of row_bytes each. False if src
// is too short.
bool deswizzle_level(const LevelLayout& level, const uint8_t* src, uint64_t src_size, std::vector<uint8_t>* out);

}  // namespace lgp::gpu::nvn
