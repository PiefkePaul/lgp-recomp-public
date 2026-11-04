# NX graphics formats: BNSH, BNTX, Maxwell SPH, GFLXPACK

Cross-game knowledge from P1.4 (as of 2026-10-11). Applies to games using NintendoSDK/NVN; GFLXPACK is
Game Freak's archive format (Let's Go, probably also later titles with different compression). Tool:
`tools/analyze/shader_inventory.py`.

## Common NX binary header (BNSH, BNTX, …)

| Offset | Content |
|---|---|
| 0x00 | Magic, 4 characters + 4 zero bytes (`BNSH\0\0\0\0`, `BNTX\0\0\0\0`) |
| 0x1C | `u32` total size of the container |

This makes it possible to find and cut out containers in arbitrary files (also embedded in archives or NSO `.ro`).

## BNSH (NVN shader container)

- Contains one or more shader stages as Maxwell machine code, plus reflection data.
- Full structure (header, `grsc`, variants, programs, stages): [bnsh-format.md](bnsh-format.md). The SPH search
  below is only an approximation and counts false hits (e.g. apparent geometry stages in control data).
- Code regions start 0x30 bytes past a 0x100 boundary (relative to the container start); right there sits the
  **SPH** (Shader Program Header, 0x50 bytes), followed by the instructions. Exactly this SPH+code piece is what the game passes
  to the GPU (found byte-identical in the Ryujinx shader cache).
- A game often spreads BNSH over many places: loose files (`.bnsh`, `.bnsh_vsh`, `.bnsh_fsh`), effect files
  (NintendoWare Vfx `.ptcl`), model archives, SARC (`.arc`) and embedded in `main`. Many containers are copies.

### SPH word 0 (for recognizing stages)

| Bits | Field | Typical values |
|---|---|---|
| 0–4 | SphType | 1 = VTG (Vertex/Tess/Geometry), 2 = PS (Pixel) |
| 5–9 | Version | 3 |
| 10–13 | ShaderType | 1 Vertex, 2 Tess Control, 3 Tess Eval, 4 Geometry, 5 Pixel |
| 14 | MrtEnable | |
| 15 | KillsPixels | |

Quick search filter: byte 0 ∈ {0x61, 0x62}, byte 1 = `ShaderType << 2` plus arbitrary bits 6–7. **Compute shaders have
no SPH**; their code starts with a control word (often `f0 07 20 e2`), they can only be found by their content.

## BNTX (texture container)

- One `BRTI` block per texture. Relative to the `BRTI`: 0x11 `u8` dimension, 0x16 `u16` mip count, **0x1C `u32` format**,
  0x24/0x28 `u32` width/height.
- Format = `(Type << 8) | Variant`, e.g. 0x1A01 BC1_UNORM, 0x1A06 BC1_SRGB, 0x1C01 BC3, 0x1D01 BC4, 0x1E01 BC5,
  0x2001/0x2006 BC7, 0x2D01 ASTC 4×4, 0x0B01 RGBA8, 0x0501 R5G5B5A1. The table in the tool follows the public
  tool documentation (Switch Toolbox convention), not checked against Ryujinx.
- Data is in block-linear layout (GOB swizzle) and must be deswizzled for PC/web.

## GFLXPACK (`.gfpak`, Game Freak)

| Offset | Content |
|---|---|
| 0x00 | Magic `GFLXPACK` |
| 0x08 | `u32` version (0x1000), `u32` relocated, `u32` file count, `u32` folder count |
| 0x18 | `u64` offset file table, `u64` offset hash table, `u64` offset folder table |

File entry (0x18 bytes): `u16` level, `u16` compression, `u32` uncompressed size, `u32` compressed size, `u32` padding,
`u64` offset. In Let's Go the compression is **2 = LZ4 block** throughout (without frame header). File names are only
stored as hashes. Python: package `lz4` (`lz4.block.decompress(raw, uncompressed_size=…)`), the fallback is the LZ4 decoder
from `nso_inventory.py`.
