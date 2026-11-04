# Maxwell shader ISA (SM50/SM52, Tegra X1) for translators

Cross-game knowledge from the P4 shader spike (status 2026-10-11). Applies to all Switch games: the GPU of the
Tegra X1 is a GM20B (Maxwell 2nd generation, SM52); precompiled shaders in BNSH containers
([nx-grafikformate.md](nx-grafikformate.md)) contain exactly this machine code. Implementation:
`gpu/shader/` (decoder `maxwell.cpp`, SPH `sph.cpp`), tool `maxwell2wgsl`.

**Reliability:** everything under "Encodings" is checked against `envydis` (envytools): 1,776 random words over all
supported forms match field by field (`tests/gpu/maxwell_vectors.txt`). The semantics (what an
instruction *does*) are derived from Mesa NAK and envytools and only checked via our own test shaders on a
software GPU, **not on real hardware**. Where this is explicitly uncertain, it says so.

## Sources

| Source | License | Used for |
|---|---|---|
| envytools `envydis/gm107.c`, tools `envydis`/`envyas` (github.com/envytools/envytools) | MIT | bit fields of all instructions, independent oracle for tests, assembler for our own test shaders |
| Mesa NAK (`src/nouveau/compiler/nak/sm50.rs`, `nak_private.h`, `from_nir.rs`) | MIT | encoder view, attribute addresses, conventions for FS outputs |
| NVIDIA open-gpu-doc `cla097sph.h` (github.com/NVIDIA/open-gpu-doc) | MIT | SPH layout |
| Ryujinx `Shader/` (MIT) | MIT | allowed as a reference, was **not reachable** from the sandbox; TEXS details come from memory of it (see below) |

envytools builds with CMake and needs `flex`, `bison`, `libxml2-dev`; only `envydis`/`envyas` are needed.
Invocation: `envydis -m gm107 -n -w` reads hex words (32-bit pairs, low first) from stdin.

## Code layout

- Code starts directly after the 0x50-byte SPH. Addresses in the code (branch targets) are relative to this start.
- Instructions are 64 bits. Every 32 bytes form a **bundle**: one scheduling word (stall counter, barriers,
  yield) plus three instructions. Scheduling words are at offset `% 32 == 0` and have no meaning for the semantics;
  a translator skips them. So the first instruction is at 0x8.
- A valid filler scheduling word for our own test shaders: `0x001f8000fc0007e0`. NOP: `0x50b0000000070f00`.

## Encodings (checked against envydis)

Common fields of almost all instructions:

| Bits | Field |
|---|---|
| 0–7 | Rd (destination), 255 = RZ (reads 0, writes discarded) |
| 8–15 | Ra |
| 16–18, 19 | guard predicate, negation. 7 = PT (always). **PT with negation (0xf) = never execute** |
| 48–63 | opcode (4 to 16 significant bits depending on class) |

Form of the B operand (recognizable by the opcode variant, e.g. FADD `0x5c58` reg, `0x3858` imm, `0x4c58` cbuf):

| Form | Bits | Note |
|---|---|---|
| Register | 20–27 | |
| Constant buffer `c[bank][off]` | bank 34–38, offset 20–33 (×4 bytes) | Hardware: offset **unsigned**. envydis shows it as signed 14 bits (a display question, no encoding difference) |
| Imm20 | 19 bits from 20, top bit in **56** | Integer ops: sign-extended. Float ops: the upper 20 bits of an f32 (`raw << 12`) |
| Imm32 (`*32I` opcodes) | 20–51 | |
| FFMA RegC (`0x5180`) | B = register 39–46, C = cbuf | otherwise C is register 39–46 |

Branches and stack ops (BRA, SSY, PBK): target = address + 8 + signed 24 bits from 20–43. The form with a target address
requires bit 5 = 0. Bits 0–4 are a condition code test, 0xf = always (everything else is CC-predicated and occurs in
game code when FSETP/ISETP with `.CC` precede it).

Supported opcode masks (excerpt, complete in `maxwell.cpp`):

| Group | Instructions |
|---|---|
| Float | FADD, FADD32I, FMUL, FMUL32I, FFMA (4 forms), FMNMX, FSETP, FSET, MUFU, RRO, F2F, F2I, I2F |
| Integer | IADD, IADD32I, ISCADD, SHL, SHR, LOP, LOP32I, ISETP, IMNMX, SEL, MOV, MOV32I, PSETP |
| Memory | ALD, AST (attributes), IPA (interpolation), LDC (constant buffer indexed), TEXS, TEX, TLDS, TXQ, TMML, TLD4 |
| Control flow | EXIT, BRA, SSY/SYNC, PBK/BRK, KIL, NOP, DEPBAR |

Pitfalls when decoding:
- **DSET overlaps FSET**: `0x49xx` is DSET (f64) and lies within the FSET c[] mask; filter it out as invalid beforehand.
- **IPA**: two variants. Without an index Ra must be RZ; bit 38 switches to indexed addressing.
- **TEXS** needs bit 59 in the mask, otherwise it collides with other `0xd...` ops.
- envydis quirks that matter for comparison: RZ is printed as `0x0`, PT as `0x1`; negative offsets without
  a base register as 64-bit hex; branch targets 64-bit; `"keeprefcount"` is in quotes; RRO shows a stray `C` flag
  for some words (envydis artifact, excluded in the vector generator).

## Semantics a translator must know

- **Predicates** P0–P6 plus PT. FSETP/ISETP write two predicates (result and result ∧/∨/⊕ negated
  combination predicate), PSETP combines predicates.
- **IMNMX/FMNMX**: a predicate operand selects minimum (true) or maximum (false); there is no separate MIN/MAX opcode.
- **SHL/SHR**: a shift ≥ 32 yields 0 (or the sign for arithmetic SHR), except with `.W` (wrap).
- **RRO** (range reduction) before MUFU sin/cos/ex2: can be treated as a copy like Ryujinx does, if MUFU computes the full
  function.
- **MUFU** rcp, rsq, sqrt, sin, cos, ex2, lg2. Results on real hardware are inexact (a few ULP).
- **F2F with rounding** (round/floor/ceil/trunc) is the usual form for `round()`/`floor()` in shaders; round =
  half-to-even.
- **ftz/fmz** (flush-to-zero, "0 × ∞ = 0") are common in game code; WebGPU cannot control them. Deviations
  only show up in edge cases (denormals, NaN/∞).

### Control flow: SSY/SYNC, PBK/BRK

Maxwell has no structured if/else in the ISA. Divergent code is controlled via a hardware stack:
`SSY target` pushes a reconvergence point, `SYNC` jumps there (for the executing lanes); `PBK`/`BRK`
likewise for loop breaks. For a translator that works per invocation (WGSL), these are simply jumps with
a small stack. Structuring into if/loop is possible but not necessary (see
[maxwell-nach-wgsl.md](maxwell-nach-wgsl.md)).

### Attribute addresses (ALD/AST/IPA)

| Address | Meaning |
|---|---|
| 0x6c | point size |
| 0x70–0x7c | position xyzw (in the FS: fragment coordinates, see IPA) |
| 0x80 + 16·i + 4·c | generic attribute i (0–31), component c |
| 0x2f8 | instance_id |
| 0x2fc | vertex_id |
| 0x3fc | front_face (FS) |

ALD/AST transfer up to four components (`b32`…`b128`) from an address into Ra+k. More system values in Mesa
`nak_private.h`.

### IPA (interpolation in the fragment shader) — derived, not checked on hardware

- `IPA.PASS a[0x7c]` returns 1/w of the fragment. Shaders compute w from it with `MUFU.RCP`.
- A perspective attribute returns, with PASS, the **screen-linearly** interpolated value attr/w; only the
  form `IPA.MUL attr, Rw` (multiplication with the w register) yields the perspective-correct attr. That is the
  pattern of practically all NVN fragment shaders: `ipa pass r a[0x7c]; mufu rcp r r; ipa r a[0x80] r`.
- The modes "constant" (flat) and "screen-linear" come from the SPH (below), not from the instruction.
- In WebGPU this corresponds to: perspective-interpolated attr × `position.w` (WebGPU `position.w` is also
  1/w) for PASS, followed by the multiplication as in the code.

### TEXS — from memory of Ryujinx, confirmed in our own test

TEXS is the compact texture form with a fixed handle (bits 36–48, ×4 = offset in the bindless constant buffer) and two
destination registers. 2D: coordinates x = Ra, y = Rb. `LL` (explicit LOD): x = Ra, y = Ra+1, LOD = Rb. Result: the
first two active components go to Rd0, Rd0+1, the rest to Rd1, Rd1+1; the component mask is a
table that depends on whether Rd1 = RZ. The test shader `fs_textured` (rgba to r0–r3) yields the expected pixels on a real
WebGPU implementation; other masks are untested.

Bindless: according to P1.4 the games' texture handles are in uniform buffers; the handle in the opcode is an offset into
the constant buffer the driver set as the texture buffer. Which buffer that is under NVN is **open**
(P4.2/P4.1 will clarify; Ryujinx, as far as remembered, takes it from a register of the 3D engine).

### TLD4 (texture gather) — encoding from envytools, registers from Mesa NAK

`TLD4` (opcode mask `0xc838`/`0xfc38`) returns the four texels of the bilinear 2x2 footprint, one
component from each (like GLSL `textureGather`). Fields (envytools `gm107.c`, tables `c838_0`/`c838_1`):

| Bits | Field |
|---|---|
| 0–7, 8–15, 20–27 | Rd, Ra, Rb |
| 28 | `array` |
| 29–30 | dimension (1D/2D/3D/cube) |
| 31–34 | write mask (result components into consecutive registers from Rd) |
| 35 | `ndv` |
| 36–48 | texture handle (as TEX) |
| 49 | `nodep` |
| 50 | `dc` (depth compare, gather of the comparison results) |
| 54–55 | offsets: 0 none, 1 `aoffi`, 2 `ptp` (one per texel), 3 invalid |
| 56–57 | gathered component r/g/b/a |

envytools does not decode bits 51–53; in the first real find (game pixel shader, `c8ba0087a1270c04` =
`tld4 r ptp nodep $r4 $r12 $r18 0x8 t2d 0xf`) they are set and were ignored.

Source registers (Mesa NAK `nak_nir_lower_tex.c`, SM50, MIT): Ra.. = [array layer as integer], u, v; Rb.. =
[offsets], [depth reference]. Offsets for gather have **8 bits per component** (x bits 0–7, y 8–15; NVIDIA allows
−32..31), other texture instructions only 4. `ptp` needs two registers: texels 0 and 1 in Rb (16 bits each), texels 2 and 3 in
Rb+1. Result order as in GL/Vulkan: (i0,j1), (i1,j1), (i1,j0), (i0,j0). Confirmed in our own test (`fs_gather`, lavapipe);
unverified on real hardware.

Not decoded (status of this thread): `TLD4S` (`0xdf00`), `TLD` (`0xdc38`), `TXD` (`0xde38`) and the bindless forms
`TEX.B`/`TLD4.B`/`TXQ.B`/`TMML.B`. None of them occurred in the opcode census of LGP's BNSH shaders (docs/analysis/07),
only `tld4` (1 shader).

## SPH (Shader Program Header, 0x50 bytes = 20 words)

Word 0 see [nx-grafikformate.md](nx-grafikformate.md). Bit positions counted over the whole header
(`cla097sph.h`):

| Bits | Content |
|---|---|
| 0–4 | SphType: 1 = VTG, 2 = PS |
| 5–9 | version (3) |
| 10–13 | shader type: 1 vertex … 5 pixel |
| 15 | kills_pixels (PS contains KIL) |
| 32–55 | size of local memory |
| VTG 192–319 | input mask generic attributes, 1 bit per component (bit 192 + 4i + c) |
| VTG 432–559 | output mask generic attributes (432 + 4i + c) |
| PS 192–447 | input mode per component, **2 bits**: 0 unused, 1 constant, 2 perspective, 3 screen-linear |
| PS 576–607 | output mask color targets (576 + 4t + c) |
| PS 608 / 609 | writes sample mask / depth |

**Convention for FS outputs** (Mesa NAK, matches our own tests): at EXIT the colors are in registers.
Each target active in the mask occupies four consecutive registers from R0 (inactive targets occupy none).
After that follows the sample mask register; the depth is one register after it.

The interpolation mode exists **only in the fragment shader's SPH**. For APIs that require it on both sides
(WebGPU, Vulkan with flat), the vertex shader must be translated to match the fragment shader.

## Building our own test shaders

`tools/shader/assemble_test_shader.py --envyas <envyas> in.s out.hex` assembles an `.s` file in envydis syntax
with small directives (`.stage`, `.in`, `.out`, `.target`, `.depth`, labels as `@name`) and generates SPH,
scheduling words and the `.hex` format that `maxwell2wgsl` reads. envyas rejects negative branch targets; the script
therefore assembles with target 0x8 and patches the 24-bit offset afterwards.
