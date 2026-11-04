// Tegra block-linear layout and deswizzling (texture_layout.hpp), against a
// byte-by-byte reference written from the GOB description independently of
// the 16-byte-chunk implementation, plus fixed values.
#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

#include "../runtime/check.hpp"
#include "lgp/gpu/nvn/abi.hpp"
#include "lgp/gpu/nvn/texture_layout.hpp"

using namespace lgp::gpu::nvn;

namespace {

// GOB: 64 bytes x 8 rows. Byte (x, y) inside it, spelled out bit by bit:
// x[5] -> bit 8, y[2:1] -> bits 7:6, x[4] -> bit 5, y[0] -> bit 4, x[3:0] -> bits 3:0.
uint32_t gob_byte(uint32_t x, uint32_t y) {
    return ((x >> 5) & 1) * 256 + ((y >> 1) & 3) * 64 + ((x >> 4) & 1) * 32 + (y & 1) * 16 + (x & 15);
}

uint64_t reference_offset(uint32_t x, uint32_t y, uint32_t pitch, uint32_t gob_height) {
    const uint32_t gobs_per_row = pitch / 64;
    const uint32_t block_row = y / (8 * gob_height);
    const uint32_t gob_in_block = (y / 8) % gob_height;
    return (uint64_t(block_row) * gobs_per_row + x / 64) * 512ull * gob_height + gob_in_block * 512ull +
           gob_byte(x % 64, y % 8);
}

void test_gob_offsets() {
    CHECK_EQ(block_linear_offset(0, 0, 64, 1), 0u);
    CHECK_EQ(block_linear_offset(16, 0, 64, 1), 32u);
    CHECK_EQ(block_linear_offset(32, 0, 64, 1), 256u);
    CHECK_EQ(block_linear_offset(0, 1, 64, 1), 16u);
    CHECK_EQ(block_linear_offset(0, 2, 64, 1), 64u);
    CHECK_EQ(block_linear_offset(63, 7, 64, 1), 511u);
    CHECK_EQ(block_linear_offset(64, 0, 128, 1), 512u);   // next GOB to the right
    CHECK_EQ(block_linear_offset(0, 8, 128, 2), 512u);    // second GOB of a 2-GOB block
    CHECK_EQ(block_linear_offset(64, 0, 128, 2), 1024u);  // next block to the right
    for (uint32_t gh : {1u, 2u, 4u, 8u, 16u})
        for (uint32_t y = 0; y < 300; y += 7)
            for (uint32_t x = 0; x < 256; x += 5)
                CHECK_EQ(block_linear_offset(x, y, 256, gh), reference_offset(x, y, 256, gh));
}

void test_layouts() {
    // 64x64 RGBA8: pitch 256, block height 8 GOBs (64 rows), one level 16 KiB.
    TextureLayout t = block_linear_layout(4, 1, 1, 64, 64, 1, 1, 1);
    CHECK_EQ(t.levels.size(), 1u);
    CHECK_EQ(t.levels[0].pitch, 256u);
    CHECK_EQ(t.levels[0].gob_height, 8u);
    CHECK_EQ(t.total_size, 16384u);
    // BC1 256x256 with mips: blocks 64x64 at level 0 (8 bytes each -> pitch 512),
    // block height shrinks with the level.
    t = block_linear_layout(8, 4, 4, 256, 256, 1, 1, 9);
    CHECK_EQ(t.levels.size(), 9u);
    CHECK_EQ(t.levels[0].blocks_x, 64u);
    CHECK_EQ(t.levels[0].pitch, 512u);
    CHECK_EQ(t.levels[0].gob_height, 8u);
    CHECK_EQ(t.levels[1].gob_height, 4u);  // 32 block rows
    CHECK_EQ(t.levels[8].blocks_x, 1u);    // 1x1 texels still one block
    CHECK_EQ(t.levels[1].offset, t.levels[0].size);
    // Odd sizes pad to whole GOBs and blocks.
    t = block_linear_layout(4, 1, 1, 100, 20, 1, 1, 1);
    CHECK_EQ(t.levels[0].pitch, 448u);      // 400 bytes -> 7 GOBs
    CHECK_EQ(t.levels[0].gob_height, 4u);   // 20 rows: 2 GOBs would not do, 4 do
    CHECK_EQ(t.total_size, 448u * 32u);
    // Arrays repeat the chain, layers aligned to a level-0 block.
    t = block_linear_layout(4, 1, 1, 16, 16, 1, 3, 2);
    CHECK_EQ(t.layers, 3u);
    CHECK_EQ(t.layer_size % (512u * t.levels[0].gob_height), 0u);
    CHECK_EQ(t.total_size, t.layer_size * 3);
    // texture_layout reads the format table: RGBA_DXT1 is 8 bytes per 4x4 block.
    t = texture_layout(uint32_t(Format::RGBA_DXT1), uint32_t(TextureTarget::Target2D), 256, 256, 1, 9);
    CHECK_EQ(t.levels[0].row_bytes, 512u);
    CHECK_EQ(texture_storage_size(uint32_t(Format::RGBA8), uint32_t(TextureTarget::Target2D), 64, 64, 1, 1), 16384u);
    // A packaged layout gives the level-0 block height: a 76-texel-high BC2
    // texture (19 block rows) with log2 1 uses 2 GOBs, the default rule 4.
    t = texture_layout(uint32_t(Format::RGBA_DXT3), uint32_t(TextureTarget::Target2D), 64, 76, 1, 1);
    CHECK_EQ(t.levels[0].gob_height, 4u);
    t = texture_layout(uint32_t(Format::RGBA_DXT3), uint32_t(TextureTarget::Target2D), 64, 76, 1, 2, 1);
    CHECK_EQ(t.levels[0].gob_height, 2u);
    CHECK_EQ(t.levels[0].size, 256u * 32u);  // 19 rows padded to 2 blocks of 16 rows
    CHECK_EQ(t.levels[1].gob_height, 2u);   // 10 rows still need 2 GOBs
}

void test_deswizzle_roundtrip() {
    // Bytes per block x size: rows that end inside a GOB and inside a 16-byte
    // sector, several GOBs per row, block heights from 1 to 16 GOBs.
    for (const auto& c : {std::array<uint32_t, 3>{4, 100, 50}, std::array<uint32_t, 3>{1, 37, 9},
                          std::array<uint32_t, 3>{2, 300, 130}, std::array<uint32_t, 3>{16, 70, 260},
                          std::array<uint32_t, 3>{8, 513, 3}})
    for (const LevelLayout& lv : block_linear_layout(c[0], 1, 1, c[1], c[2], 1, 1, 4).levels) {
        std::vector<uint8_t> swizzled(size_t(lv.size), 0);
        uint64_t needed = 0;  // one past the last byte read
        for (uint32_t y = 0; y < lv.blocks_y; ++y)
            for (uint32_t x = 0; x < lv.row_bytes; ++x) {
                const uint64_t at = reference_offset(x, y, lv.pitch, lv.gob_height);
                swizzled[at] = uint8_t(x * 7 + y * 13);
                needed = std::max(needed, at + 1);
            }
        std::vector<uint8_t> linear;
        CHECK(deswizzle_level(lv, swizzled.data(), swizzled.size(), &linear));
        bool same = linear.size() == size_t(lv.row_bytes) * lv.blocks_y;
        for (uint32_t y = 0; same && y < lv.blocks_y; ++y)
            for (uint32_t x = 0; same && x < lv.row_bytes; ++x)
                same = linear[size_t(y) * lv.row_bytes + x] == uint8_t(x * 7 + y * 13);
        CHECK(same);
        std::vector<uint8_t> out;
        CHECK(!deswizzle_level(lv, swizzled.data(), needed - 1, &out));  // one byte short
    }
}

// 3D: blocks of 1 GOB x gob_height GOBs x gob_depth slices; inside a block the
// GOBs of one slice come first (y), then the next slice (z).
uint64_t reference_offset_3d(uint32_t x, uint32_t y, uint32_t z, uint32_t pitch, uint32_t rows, uint32_t gh,
                             uint32_t gd) {
    const uint32_t gobs_x = pitch / 64, blocks_y = (rows + 8 * gh - 1) / (8 * gh);
    const uint64_t block = (uint64_t(z / gd) * blocks_y + y / (8 * gh)) * gobs_x + x / 64;
    const uint64_t gob = uint64_t(z % gd) * gh + (y / 8) % gh;
    return block * 512 * gh * gd + gob * 512 + gob_byte(x % 64, y % 8);
}

void test_layout_3d() {
    // 32x32x32 RGBA8 (LGP's 3D texture): one GOB high, 16 slices deep (largest
    // power of two <= 48), 4 GOBs per slice row block -> 128 KiB.
    TextureLayout t = texture_layout(uint32_t(Format::RGBA8), uint32_t(TextureTarget::Target3D), 32, 32, 32, 1);
    CHECK_EQ(t.levels.size(), 1u);
    CHECK_EQ(t.layers, 1u);
    CHECK_EQ(t.levels[0].depth, 32u);
    CHECK_EQ(t.levels[0].gob_height, 1u);
    CHECK_EQ(t.levels[0].gob_depth, 16u);
    CHECK_EQ(t.levels[0].pitch, 128u);
    CHECK_EQ(t.total_size, 128u * 32u * 32u);
    // Mips shrink the depth and the block depth: 16x16x16 -> 8 (<= 8, half of 16)...
    t = block_linear_layout(4, 1, 1, 64, 64, 20, 1, 4, -1, true);
    CHECK_EQ(t.levels[0].gob_depth, 16u);  // 30 -> 16
    CHECK_EQ(t.levels[1].depth, 10u);
    CHECK_EQ(t.levels[1].gob_depth, 16u);  // 10 > 8: stays 16
    CHECK_EQ(t.levels[2].gob_depth, 8u);   // depth 5
    CHECK_EQ(t.levels[3].gob_depth, 2u);   // depth 2
    CHECK_EQ(t.levels[0].size, 256u * 64u * 32u);  // depth 20 padded to 32
    // A 2D texture ignores depth.
    t = block_linear_layout(4, 1, 1, 64, 64, 20, 1, 1);
    CHECK_EQ(t.levels[0].depth, 1u);
    CHECK_EQ(t.levels[0].gob_depth, 1u);

    // Offsets and deswizzle against the reference, odd sizes included.
    for (const auto& dims : {std::array<uint32_t, 3>{32, 32, 32}, std::array<uint32_t, 3>{40, 20, 7},
                             std::array<uint32_t, 3>{70, 9, 3}}) {
        const TextureLayout v = block_linear_layout(4, 1, 1, dims[0], dims[1], dims[2], 1, 2, -1, true);
        for (const LevelLayout& lv : v.levels) {
            std::vector<uint8_t> swizzled(size_t(lv.size), 0);
            for (uint32_t z = 0; z < lv.depth; ++z)
                for (uint32_t y = 0; y < lv.blocks_y; ++y)
                    for (uint32_t x = 0; x < lv.row_bytes; ++x) {
                        const uint64_t at =
                            reference_offset_3d(x, y, z, lv.pitch, lv.blocks_y, lv.gob_height, lv.gob_depth);
                        CHECK(at < swizzled.size());
                        CHECK_EQ(block_linear_offset(lv, x, y, z), at);
                        swizzled[at] = uint8_t(x * 7 + y * 13 + z * 29);
                    }
            std::vector<uint8_t> linear;
            CHECK(deswizzle_level(lv, swizzled.data(), swizzled.size(), &linear));
            CHECK_EQ(linear.size(), size_t(lv.row_bytes) * lv.blocks_y * lv.depth);
            bool same = true;
            for (uint32_t z = 0; same && z < lv.depth; ++z)
                for (uint32_t y = 0; same && y < lv.blocks_y; ++y)
                    for (uint32_t x = 0; same && x < lv.row_bytes; ++x)
                        same = linear[(size_t(z) * lv.blocks_y + y) * lv.row_bytes + x] == uint8_t(x * 7 + y * 13 + z * 29);
            CHECK(same);
        }
    }
}

void test_format_table() {
    const auto dxt5 = format_info(uint32_t(Format::RGBA_DXT5));
    CHECK(dxt5 && dxt5->bytes_per_pixel == 16 && dxt5->block_width == 4 && !dxt5->srgb);
    const auto bc7s = format_info(uint32_t(Format::BPTC_UNORM_SRGB));
    CHECK(bc7s && bc7s->srgb && bc7s->block_height == 4);
    const auto astc = format_info(uint32_t(Format::ASTC_12x12_SRGB));
    CHECK(astc && astc->block_width == 12 && astc->block_height == 12 && astc->srgb);
    const auto d24s8 = format_info(uint32_t(Format::Depth24Stencil8));
    CHECK(d24s8 && d24s8->kind == FormatKind::DepthStencil && d24s8->bytes_per_pixel == 4);
    CHECK(!format_info(90));   // vertex-only format
    CHECK(!format_info(161));  // past the table
}

}  // namespace

int main() {
    RUN(test_gob_offsets);
    RUN(test_layouts);
    RUN(test_deswizzle_roundtrip);
    RUN(test_layout_3d);
    RUN(test_format_table);
    return finish();
}
