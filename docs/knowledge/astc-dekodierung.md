# Decoding ASTC (LDR, software)

Cross-game knowledge about the ASTC decoder in `gpu/texture/` (P4.3). Switch games use ASTC via NVN formats
121–148 (`ASTC_4x4` … `ASTC_12x12`, then the same 14 block sizes as `_SRGB`). WebGPU can only do ASTC with the feature
`texture-compression-astc`, which desktop adapters (Windows/Linux, NVIDIA/AMD/Intel, also lavapipe) practically never
have; Apple Silicon and mobile devices have it. That is why a port needs a CPU decoder as the normal case on desktop.

## Source and verification

- Written from the **Khronos Data Format Specification 1.3, chapter 23** ("ASTC Compressed Texture Image
  Formats"). No code taken over from other decoders.
- Oracle: **ARM astcenc 5.3.0** (Apache-2.0), only as a tool. `tools/texture/astc_vectors_gen.cpp` uses
  astcenc to produce, from our own synthetic images (gradients, edges, noise, alpha), blocks for all 14 block sizes and
  three quality levels, plus random blocks (mostly invalid) and void-extent blocks. Stored per block are the
  block contents and an FNV-1a-64 hash of the RGBA8 texels decoded by astcenc (`tests/gpu/texture/astc_vectors.txt`,
  5,376 vectors). Regenerate: `python3 tools/texture/gen_astc_vectors.py --astcenc <astcenc-checkout>`.
- Test `gpu.astc` (labels `gpu`, `texture`): all vectors must match bit-exactly, plus void extent, reserved
  block mode and image edges. Cross-check: deliberately injected bugs (blue contraction, weight unquantization)
  make hundreds of vectors fail.

## Output semantics (the real trap)

The specification allows two variants for 8-bit targets; hardware and astcenc with `U8` output use
**decode_unorm8**:

- Linear: 8-bit endpoints are **replicated** to 16 bits (`e * 257`).
- sRGB: endpoints become `(e << 8) | 0x80`, and on **all four** channels (astcenc also does it for alpha).
- Texel = **upper 8 bits** of the 16-bit interpolation result (`(c0 * (64 - w) + c1 * w + 32) >> 6`), not
  rounded to 8 bits.
- sRGB textures are **not** linearized: the result stays an sRGB value and belongs in a `*_SRGB`
  host texture; the hardware converts when sampling.

Anyone who instead decodes via float and then rounds is off by 1 in many texels; a hash comparison with
astcenc reveals that immediately. The difference between `0x80` and replication for sRGB is not visible after
truncating to 8 bits (so a test cannot distinguish it).

## What an LDR decoder must handle

- **Block mode** (11 bits) → weight grid, weight range, dual plane; reserved combinations and grids larger
  than the block are error blocks.
- **Integer Sequence Encoding**: trits (5 values in 8 bits) and quints (3 values in 7 bits) with the tables from the
  specification; weights lie **bit-reversed** at the end of the block.
- **Partitions** 1–4 with the partition hash (10-bit seed; for small blocks, under 31 texels, the
  coordinate is doubled).
- **Color endpoint modes** LDR: 0, 1, 4, 5, 6, 8, 9, 10, 12, 13 (incl. blue contraction and `bit_transfer_signed`).
  HDR modes (2, 3, 7, 11, 14, 15) are errors in the LDR profile.
- **Color unquantization** with trit/quint tables (values A, B, C, D); this is where our only own bug was
  (wrong shift in the trit case b = 3), which the vectors would not have caught before the first run, but did
  afterwards.
- **Weight infill** (bilinear interpolation of the weight grid onto the block texels) and dual plane (second
  weight plane for one channel, channel from 2 bits).
- **Void extent** (block mode `0x1FC`): one color for the whole block, 16 bits per channel; HDR flag set = error
  in the LDR profile. The coordinates are only checked, not used.
- **Error color**: magenta `(255, 0, 255, 255)` for every invalid block.

## Integration in the WebGPU backend

- With `texture-compression-astc`: native `ASTC*Unorm(Srgb)` formats, upload of the deswizzled blocks.
- Without (or with `WebGpuBackend::force_cpu_astc(true)`, `LGP_WEBGPU_DEBUG=cpuastc`): host texture `RGBA8Unorm` or
  `RGBA8UnormSrgb` at level size (not rounded to blocks); each level is decoded from the pool on upload.
  `Stats::astc_cpu_decoded` counts decoded levels, `Stats::astc_error_blocks` error blocks.
- Memory cost: RGBA8 instead of 1 byte/texel is 4× for 4×4, 16× for 8×8. Irrelevant for LGP (20 textures,
  28×28); for games with many large ASTC textures, transcoding to BC7 or a decoder in the shader would be worth
  considering.
- `CopyBufferToTexture` and `TextureWriteTexels` with compressed data (BCn as well as ASTC) are still skipped
  (reason "copy to texture: unsupported texture or region" or a warning in the log).
- The decoder is platform-neutral (no SIMD, no OS headers) and runs unchanged in the WASM build.
