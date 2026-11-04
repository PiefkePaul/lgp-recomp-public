# Textures from NVN memory pools: block-linear layout, format table, upload

Cross-game knowledge from P4.3 (measured on Pokémon Let's Go, applies to NVN titles on Tegra X1). Code:
`gpu/nvn/include/lgp/gpu/nvn/texture_layout.hpp`, `gpu/nvn/src/abi.cpp` (`kFormats`, `texture_layout()`),
`gpu/webgpu/src/webgpu_backend.cpp` (`upload_from_pool`). LGP numbers: `docs/analysis/11-erstes-bild.md`.

## Why this is needed

Almost all textures of an NVN game do not come through `nvnTextureWriteTexels`, but are already sitting finished in the
memory pool: the game loads the file (BNTX or similar) into a pool and creates a texture with `nvnTextureInitialize`
at `storage = pool address + offset`. The content is stored in the GPU's **block-linear layout**. An HLE therefore
has to read, deswizzle and upload it itself; otherwise all textures are empty.

## Block-linear (Maxwell/Tegra X1)

- **GOB** (group of bytes): 64 bytes wide × 8 rows = 512 bytes. Within a GOB, byte `(x, y)` (x in bytes,
  y in rows) lies at

  ```
  ((x >> 5) & 1) << 8 | ((y >> 1) & 3) << 6 | ((x >> 4) & 1) << 5 | (y & 1) << 4 | (x & 15)
  ```

  So 16 consecutive bytes of a row stay together (sector 16 bytes × 2 rows); `deswizzle_level`
  therefore copies in 16-byte pieces.
- **Block**: 1 GOB wide, `block_height` GOBs high (1, 2, 4, 8, 16). Blocks are laid out row by row: first all blocks of a
  block row from left to right, then the next block row. The row width (pitch) is the width in bytes rounded up to
  64.
- **Block height**: without an explicit value, NVN picks for level 0 the smallest power of two of GOBs that covers the height (in
  format blocks), at most 16. Smaller mip levels reduce it as long as half the height still suffices
  (same rule as Ryujinx `GetMipGobBlockSizes`).
- **Mip chain**: each level is padded to whole blocks of its block height, the levels follow each other directly.
- **Layers** (arrays, cube faces): each layer repeats the whole mip chain; the layer stride is rounded up to a whole
  block of the level-0 block height. Cube = 6 layers, cube array = 6 × depth.
- **3D textures** additionally have a **block depth** (number of GOBs in z per block). Implemented in
  `gpu/nvn/src/texture_layout.cpp` following deko3d's rule (`dk_image.cpp`, zlib, only as a source of facts):
  - Block height 1 GOB; block depth = largest power of two ≤ 1.5 · depth, at most 16; smaller mip levels reduce
    it like the block height, as long as half the depth still suffices.
  - Within a block the GOBs lie first in y, then in z; blocks lie in x, then y, then z.
  - Size of a level = blocks in x · blocks in y · blocks in z · 512 B · block height · block depth.
  - Example (test `nvn.texture_layout`): RGBA8 32×32×32 → block height 1, block depth 16, pitch 128, 128 KiB.
  - **Not measured on the NVN driver.** Check via `nvnTextureBuilderGetStorageSize` of a 3D texture in our own
    `sdk` or in the game (LGP's 32×32×32 color LUT); if the size matches, the block depth probably matches too.
- **Compressed formats** compute in format blocks (BCn: 4×4 texels, ASTC: block size of the format); "width in bytes"
  is `blocks × bytes per block`.
- Storage size for `nvnTextureBuilderGetStorageSize`: total size rounded up to 0x200.

**Packaged layout** (`nvnTextureBuilderSetPackagedTextureLayout`, measured): the driver copies from the
8-byte `NVNpackagedTextureLayout` the lower 12 bits (three 4-bit fields) into the builder and sets a flag. Field 0 is
the block height of level 0 as log2 GOBs (for all textures of LGP, fields 1/2 are 0 there). The tools that produce the files
round differently than the default rule above: block height = power of two ≥ (block rows / 8, rounded down). Example
BC2 64×76 (19 block rows): file 2 GOBs, default rule 4. Whoever ignores the field deswizzles such textures wrongly
(in LGP: fragmented borders in the text box).

**Linear textures** (`NVN_TEXTURE_FLAGS_LINEAR` = 0x10, measured via the driver's storage alignment: 0x20 for
this bit, 0x80 for bit 0x40, linear render targets): rows lie at the distance of the builder stride (or rounded up to 32 bytes)
without swizzle. LGP uses seven 2048×1024 R8 textures of this kind as a font cache written by the CPU;
the backend therefore checks them for changes every frame.

## Format table in the driver

The `sdk` driver has a table with **0x30-byte entries**, index = `NVNformat` value (161 entries; the names
follow the public NVN enum order). Fields, as far as needed:

| Offset | Content |
|---|---|
| `+0x08` | block width / block height in texels (1 for uncompressed formats) |
| `+0x10` | bytes per block (or per texel) |
| `+0x12` | sRGB flag |
| `+0x20` | TIC-like format word (components/type for the texture header) |

The table can be found via the GOT entry that `nvnTextureBuilderGetStorageSize` loads. `abi.cpp` contains the
rows measured from it (format, block dimensions, bytes per block, sRGB) as numbers.

Examples (NVN enum): 56 RGBA8_SRGB, 59 RGB5A1, 63 R11G11B10F, 66 RGBA_DXT1 (BC1), 67 DXT3 (BC2), 68 DXT5 (BC3),
70 DXT1_SRGB, 73 RGTC1_UNORM (BC4), 75 RGTC2_UNORM (BC5), 77/78 BPTC_UNORM(_SRGB) (BC7), 121 ASTC_4x4.

## Upload to WebGPU

- Uploading happens **on first bind** (a texture that is never sampled costs nothing), after that every 60
  presentations a checksum over the pool content (games stream textures into existing memory).
- Textures written by the GPU or the API (render targets, copy targets, `WriteTexels`) are **never** loaded from the
  pool: the HLE does not write their pool memory back, it only contains old content.
- BCn needs `TextureCompressionBC`, ASTC `TextureCompressionASTC` (desktop adapters usually only have BC). Compressed
  textures are created rounded up to block multiples and have no RenderAttachment usage.
- 16-bit formats without a WebGPU counterpart (RGB565, RGB5A1, RGBA4, …) are widened to RGBA8 on upload.
- R11G11B10F as a render target needs `RG11B10UfloatRenderable`, otherwise RGBA16F.
- ASTC without `TextureCompressionASTC` is decoded to RGBA8 on the CPU, see
  [`astc-dekodierung.md`](astc-dekodierung.md).
- 3D textures: WebGPU `3d` textures (at most 2048 per side, not as a render target, not compressed), all
  slices of a level in one upload. A shader that samples a 3D texture as 2D (or vice versa) gets the
  dummy texture (reason "texture: 2D/3D does not match the shader"), instead of failing WebGPU validation.

## Channel swizzle of the texture view

NVN puts the swizzle into the texture view (`nvnTextureViewSetSwizzle`, values ZERO, ONE, R, G, B, A = 0…5), which ends up in the pool on
`RegisterTexture`. Single-channel UI and font textures are typically sampled with (ONE, ONE, ONE, R):
without swizzle they appear red instead of white with alpha.

WebGPU has the feature `texture-component-swizzle` (`WGPUTextureComponentSwizzleDescriptor`) for this, but wgpu-native 29
does not offer it on the tested adapter. The translator therefore applies the swizzle **in the shader**
(`wgsl::Options::texture_swizzle`): each texture group gets, behind the texture/sampler pairs, a uniform buffer
with one code per binding (3 bits per component), every color texel goes through `tex_swizzle_apply`. This works in
every WebGPU (desktop and browser) without an extra feature; the backend keeps a small buffer per code combination.
Depth comparisons stay without swizzle.
