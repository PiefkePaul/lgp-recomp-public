// BC1-BC5 and BC7 (S3TC/DXT, RGTC, BPTC unorm) software decoder, for GPUs
// without texture-compression-bc (mobile GPUs such as Adreno and Mali, P7.3).
//
// Written from the block format descriptions (Khronos Data Format
// Specification 1.3, sections 20-22); no code taken from other decoders. The
// BC7 partition tables are spec data; the tests compare against Pillow's
// decoder (tools/texture/gen_bc_vectors.py). Output is RGBA8 per texel:
//   BC1  colour + 1-bit alpha (3-colour blocks: index 3 is transparent black)
//   BC2  colour + explicit 4-bit alpha, BC3 colour + interpolated alpha
//   BC4  (r, 0, 0, 255), BC5 (r, g, 0, 255); snorm variants as signed bytes
//        (r, g, 0, 127) for an RGBA8Snorm texture
//   BC7  RGBA, all eight modes (invalid mode byte 0: transparent black)
// sRGB variants decode the same bytes; the texel stays an sRGB value for an
// *_SRGB host texture. BC6H (float) is not covered.
#pragma once

#include <cstddef>
#include <cstdint>

namespace lgp::gpu::texture {

enum class BcFormat : uint8_t { BC1, BC2, BC3, BC4, BC4Snorm, BC5, BC5Snorm, BC7 };

// Bytes per 4x4 block: 8 for BC1 and BC4, 16 for the others.
size_t bc_block_bytes(BcFormat format);

// Two decoders that write the same bytes: Reference (the one the vectors were
// checked with, one bit and one byte at a time) and Fast (32-bit texel stores,
// BC7 fields read with shifts, whole blocks straight into the image). The
// process-wide choice is Fast (Apple TV, scene change, ABAB: uploads -51 %,
// fps +7 %); LGP_WEBGPU_BC_DECODER=reference selects Reference in the WebGPU
// backend.
enum class BcDecoder : uint8_t { Reference, Fast };
void set_bc_decoder(BcDecoder decoder);
BcDecoder bc_decoder();

// Decodes one block into 4x4 RGBA8 texels at out + y * out_stride + x * 4.
void decode_bc_block(BcFormat format, const uint8_t* block, uint8_t* out, size_t out_stride);
void decode_bc_block_with(BcDecoder decoder, BcFormat format, const uint8_t* block, uint8_t* out, size_t out_stride);

// Decodes a linear array of blocks (row-major, ceil(width / 4) per row) into a
// tightly packed width x height RGBA8 image.
void decode_bc_image(BcFormat format, const uint8_t* data, uint32_t width, uint32_t height, uint8_t* out);
void decode_bc_image_with(BcDecoder decoder, BcFormat format, const uint8_t* data, uint32_t width, uint32_t height,
                          uint8_t* out);

}  // namespace lgp::gpu::texture
