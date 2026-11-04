// ASTC (Adaptive Scalable Texture Compression) software decoder, LDR profile.
//
// Written from the Khronos Data Format Specification 1.3, section 23
// ("ASTC Compressed Texture Image Formats"); no code taken from other
// decoders. Output follows the spec's decode_unorm8 behaviour, which is what
// the ARM reference codec (astcenc) produces for 8-bit images and what the
// tests compare against:
// - linear: 8-bit endpoints are bit-replicated to 16 bits (e * 257),
// - sRGB: endpoints become (e << 8) | 0x80 (all four channels, as astcenc),
// - texel = top 8 bits of the 16-bit interpolation result.
// The sRGB variant does not convert to linear; the texel stays an sRGB value,
// so it belongs into an *_SRGB host texture.
//
// HDR endpoint modes and HDR void-extent blocks are not part of the LDR
// profile and decode to the error colour (magenta, 0xFF00FFFF) like every
// other invalid block, as the spec demands for an LDR decoder. Only 2D block
// footprints (4x4 ... 12x12), which is all NVN offers.
// Details and pitfalls: docs/knowledge/astc-dekodierung.md.
#pragma once

#include <cstddef>
#include <cstdint>

namespace lgp::gpu::texture {

inline constexpr size_t kAstcBlockBytes = 16;

// True for the 14 2D footprints the spec defines (4x4, 5x4, ..., 12x12).
bool astc_footprint_valid(uint32_t block_width, uint32_t block_height);

// Decodes one 16-byte block into block_width * block_height RGBA8 texels.
// Texel (x, y) goes to out + y * out_stride + x * 4. Returns false (and
// writes the error colour) if the block is invalid or uses HDR.
bool decode_astc_block(const uint8_t* block, uint32_t block_width, uint32_t block_height, bool srgb, uint8_t* out,
                       size_t out_stride);

// Decodes a linear array of blocks (row-major, ceil(width / bw) blocks per
// row) into a tightly packed width x height RGBA8 image (width * 4 bytes per
// row). `data` must hold blocks_x * blocks_y * 16 bytes. Returns the number
// of blocks that decoded to the error colour.
size_t decode_astc_image(const uint8_t* data, uint32_t width, uint32_t height, uint32_t block_width,
                         uint32_t block_height, bool srgb, uint8_t* out);

}  // namespace lgp::gpu::texture
