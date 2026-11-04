# BNSH format (NVN shader container) in detail

Cross-game knowledge from P4.2 (status 2026-10-11), gained from all 8,401 distinct BNSH of a
NintendoSDK 5.x game (version 2.1.5, all of them readable this way without exception). Complements the overview in
[nx-grafikformate.md](nx-grafikformate.md). Parser: `tools/analyze/bnsh.py` (`list` for one file, `extract` for
RomFS + ExeFS).

All pointers are 64-bit **file offsets**, as they stand in the file (the relocation table _RLT at the end turns
them into addresses at load time). Little endian.

## File header (0x20 bytes, shared with BNTX)

| Offset | Type | Content |
|---|---|---|
| 0x00 | char[4] + u32 | `BNSH`, 0 |
| 0x08 | u32 | version, e.g. `0x00020105` = 2.1.5 |
| 0x0C | u16 | BOM `0xFEFF` |
| 0x0E | u8 | alignment as a power of two (12 = 0x1000, for the code memory pool) |
| 0x0F | u8 | target address size |
| 0x10 | u32 | offset of the file name (string pool `_STR`) |
| 0x14 | u16 | flags |
| 0x16 | u16 | offset of the first block (`grsc`, typically 0x60) |
| 0x18 | u32 | offset of the relocation table (`_RLT`) |
| 0x1C | u32 | file size |

## `grsc` (Graphics Shader Container)

| Offset | Type | Content |
|---|---|---|
| +0x00 | char[4] | `grsc` |
| +0x04 / +0x08 | u32 | offset of the next block / block size |
| +0x10 | u16, u16 | target API type (always 4 here, probably NVN), API version (0) |
| +0x18 | u32 | probably compiler version (here 0x00020102), not checked |
| +0x1C | u32 | number of variants |
| +0x20 | ptr | variant array |
| +0x28 | ptr | further data (meaning not checked; not the code range itself) |

## Variant (0x40 bytes)

| Offset | Content |
|---|---|
| +0x00 | ptr source program (0 in retail) |
| +0x08 | ptr intermediate program (0 in retail) |
| +0x10 | ptr **binary program** |
| +0x18 | ptr back to `grsc` (good plausibility check) |

A variant is a set of stages that are bound together as an NVN program. Many containers contain
material permutations as variants; the same stage then appears in several variants (and containers).

## Binary program and stages

Program: from +0x08 one pointer each to the stages in this order (0 = not present):

| +0x08 | +0x10 | +0x18 | +0x20 | +0x28 | +0x30 |
|---|---|---|---|---|---|
| Vertex | Hull (Tess Control) | Domain (Tess Eval) | Geometry | Pixel | Compute |

In this game only vertex, pixel and compute are populated; the positions of hull, domain and geometry follow from
the order and are not checked against real data. After that follow, among other things, a pointer to reflection data (uniform/sampler names); not needed for the translation.

Stage (0x20-byte header):

| Offset | Type | Content |
|---|---|---|
| +0x08 | ptr | NVN **control section** (magic `0x98761234`, driver metadata) |
| +0x10 | ptr | **code** (in the 0x1000-aligned pool) |
| +0x18 | u32 | code size incl. NVN code header |
| +0x1C | u32 | size of the control section |

Code of a **graphics stage**: 0x30-byte NVN code header (starts with magic `0x12345678`, otherwise zeros), then the
0x50-byte **SPH** and the Maxwell code. From the SPH on, this is exactly the memory the game hands to the GPU
(byte-identical in the Ryujinx guest shader cache). **Compute code has neither an NVN code header nor an SPH**. It starts at the entry offset from the
control section (`+0x708`); in LGP that is `0` for all compute stages, so the code starts directly with the first
scheduling word. According to public tools, newer glslc output uses `0x100`; parser and HLE therefore follow the
field (checklist item 11 in [nvn-hle.md](nvn-hle.md)). The SPH shader types match the stage position in the program in all cases (checked in the
parser).

The code is stored back to back in the pool; the code size includes padding words at the end. The end of the program is marked by a
`BRA` to itself (target = own bundle start) after the last `EXIT`; what comes after is padding.

## Experience

- **Real count instead of heuristics:** the search for SPH patterns (P1.4) counts hits that are not stages: all
  supposed geometry and tessellation stages lay outside any stage code (in control or
  reflection data). With the parser there were only vertex, pixel and compute stages.
- **Count uniquely by whole content:** stages that differ only at the end are different programs. A
  count over the first 0xC0 bytes underestimates by more than half.
- **VS/PS pairs** come from the variant. A vertex shader can be bound with several pixel shaders; for the
  translation to WGSL it needs the interpolation modes of the partner (see
  [maxwell-nach-wgsl.md](maxwell-nach-wgsl.md), item 8). The tool picks the most frequent partner and counts the
  alternatives (`pixel_variants`).
- Containers are often embedded in other files (archives, effect files, NSO `.ro`). Find them via `BNSH\0\0\0\0` plus
  file size at 0x1C, then parse.
