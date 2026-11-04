# Translating Maxwell shaders to WGSL: mapping, pitfalls, tools

Cross-game knowledge from the P4 shader spike (status 2026-10-11). ISA basics in
[maxwell-shader-isa.md](maxwell-shader-isa.md). Code: `gpu/shader/` (lifter `lift.cpp`, backend `wgsl.cpp`),
command line `maxwell2wgsl`, tests `tests/gpu/` (`ctest -L gpu`).

## Pipeline

```
SPH + code ──decode──► Maxwell instr ──lift──► IR (one inst per machine instruction) ──emit──► WGSL module
```

- **Lifter**: follows the control flow from 0x8 (worklist over branch and SSY/PBK targets); only reachable code
  is translated. Every unsupported variant (modifier, form, opcode) aborts with address and word; nothing is
  ever silently translated wrong. List of aborts: `grep unsupported gpu/shader/src/lift.cpp`.
- **IR**: register-based like the hardware (R0–R254 as `u32`, P0–P6 as `bool`), operands with neg/abs/inv,
  guard predicate per instruction. Deliberately no SSA: for the spike 1:1 is enough; an SSA/optimization stage belongs in
  P4.2 if output size or compile time in the browser become a problem.
- **Backend**: registers become `var<private> rN: u32`, floats via `bitcast<f32>`. The bit pattern is thus preserved
  exactly (Maxwell registers are untyped).

## Resource mapping (convention of `maxwell2wgsl`)

| Maxwell | WGSL |
|---|---|
| `c[b][off]` | `@group(0) @binding(b) var<uniform> cbB: array<vec4<u32>, 4096>` (64 KiB, index `off/16`, component `off/4 % 4`) |
| texture handle h (k-th distinct) | `@group(1) @binding(2k)` `texture_2d<f32>`, `@binding(2k+1)` `sampler` |
| VS generic input i | `@location(i) vec4<f32>` |
| VS generic output i / FS input i | `@location(i)`, interpolation from the FS SPH |
| FS color target t | `@location(t)` in the output struct, values from the registers by SPH convention |
| FS depth | `@builtin(frag_depth)` |
| vertex_id / instance_id | `@builtin(vertex_index)` / `@builtin(instance_index)` |

The groups are configurable via `wgsl::Options::cbuf_group`/`texture_group` (default 0/1). Maxwell has
constant buffers per shader stage; in a WebGPU pipeline, however, vertex and fragment shader share the bind groups.
The WebGPU backend (P4.4) therefore translates vertex with 0/2 and fragment with 1/3.

Uniform arrays need a stride of 16 bytes in WGSL, hence `vec4<u32>` instead of `array<u32>`. Dynamic
LDC offsets are split into index and component.

## Control flow: dispatcher instead of structuring

Without branches, code is emitted straight-line. With branches, basic blocks are created at all targets and after every
terminator, embedded in

```wgsl
var pc: u32 = 0u;
loop {
    if (pc >= N) { break; }
    switch pc {
        case 0u: { ...; pc = 1u; }
        case 1u: { ... }
        default: {}
    }
}
return finish_fragment();
```

SSY/PBK push targets onto small `array<u32, 16>` stacks, SYNC/BRK pop them. This is correct for arbitrary (even
unstructured) branches, but harder to optimize than real if/loop. For P4.2 measure whether Tint/naga
digest this well enough; otherwise retrofit structuring (e.g. following the approach of Ryujinx/Mesa).

## WGSL pitfalls (all encountered in the spike)

1. **Constant bitcasts to ∞/NaN are a compile error.** `bitcast<f32>(0x7f800000u)` is evaluated at compile time
   and rejected. Solution: route the immediate through a function with a private variable
   (`nonconst_u32(v) = v | nonconst_zero`), so that the value is not constant.
2. **Constant overflow** in literal arithmetic (e.g. `i32` literals) is also an error; always generate immediates
   as `u32` and reinterpret them only at run time.
3. **Shift ≥ 32** is undefined in WGSL or taken modulo 32; on Maxwell it yields 0. Handle explicitly
   with `select`.
4. **Uniformity analysis**: `textureSample` (implicit LOD, derivatives) is only allowed in uniform control flow.
   The dispatcher loop depends on `pc` and always counts as non-uniform. Maxwell computes derivatives per
   quad regardless of that; hence for FS with implicit LOD and branches `diagnostic(off, derivative_uniformity);`.
   In the vertex shader there is no implicit LOD: `textureSampleLevel(..., 0.0)`.
5. **naga and Tint disagree on returns.** naga requires a `return` at the end of the function, even after a
   loop without `break`; Tint then warns "code is unreachable". Solution above: leave the loop via `break` and
   return afterwards. Generated code should be warning-free in both (the tests treat warnings as errors).
6. **Multiple assignment**: instructions like `ld b128` or TEXS write several registers whose sources may overlap
   with the destinations. First put all values into `let` temporaries, then assign, in a separate `{}` block
   (otherwise the `let` names collide).
7. **NaN**: WGSL allows implementations to assume NaN away or optimize it out; `min`/`max`/comparisons with NaN
   are not guaranteed to behave as on Maxwell. Games that use NaN deliberately may deviate.
8. **Interpolation must match on both sides.** WebGPU rejects the pipeline if VS output and
   FS input of a location have different `@interpolate` attributes. Even `@interpolate(perspective)` without
   sampling against a VS output without an attribute failed in wgpu; therefore always emit fully
   (`perspective, center`, `linear, center`, `flat`). Maxwell knows the mode only in the FS SPH. Consequence: a vertex shader must be
   translated per fragment shader partner (`maxwell2wgsl --link-fs fs.bin vs.bin`,
   API `wgsl::fragment_input_interpolation()`); the runtime (P4.4) must cache VS variants per pipeline.
9. **flat** uses the first vertex in WebGPU (`flat, first`); which provoking vertex NVN games expect has
   not been checked.
10. **Limits**: WebGPU guarantees 16 inter-stage locations (Maxwell: 32 generic attributes), 8 color targets and
    a limited number of uniform buffers per stage (12). Shaders that use more need redistribution.

## What WebGPU lacks (from P1.4) and how to emulate it

- **Alpha test**: via `discard` in the fragment shader, reference value and comparison from the pipeline state as a
  uniform. Not implemented yet (the spike only covers the shader's own KIL).
- **Depth bounds test, conservative rasterization**: no replacement in WebGPU; approximate in the FS if needed or
  leave out. Uncritical for Let's Go according to P1.4, but keep an eye on it in P4.5.

## What the translator can and cannot do (status P4.2)

Supported: VS, FS and **compute** with the opcode set from [maxwell-shader-isa.md](maxwell-shader-isa.md) plus (P4.2, from real
game shaders) **XMAD, IADD3, ISET, I2I (incl. `.CC`), CSETP, R2P, BFE, PSET**, LOP with predicate output, FMUL with
result scaling (`.D2` … `.M8`), **FMUL.FTZ.RZ** and **I2F.RZ** (exact, see below), 2D textures via TEXS (2D, LZ,
LL, **DC** = depth compare) and TEX (2D, LZ, LL, **LB**, **AOFFI**), **TLDS** (2D LZ/LL, AOFFI), **TXQ** (DIMENSION),
**TMML** (2D), **3D textures** (TEXS type 10/11 = `t3d`/`t3d lz`, TEX with dimension 2, TLDS type 7 = 3D LZ, TXQ
returns the depth; WGSL `texture_3d<f32>`, coordinates from Ra, Ra+1, Rb; TLDS 3D is UNVERIFIED, because none of our
game shaders uses it), constant buffers direct and indexed, KIL, loops and branches via SSY/SYNC/PBK/BRK/BRA, color
and depth output. Compute (P4.2 gaps, see section below): S2R (thread/CTA ID, LaneId etc.), LDG/STG (8 to
128 bits), LDS/STS, LDL/STL, ATOM/RED (global), ATOMS (shared), BAR.SYNC 0, MEMBAR (only directly before BAR or EXIT, otherwise rejected: WGSL has no standalone fence), LEA/LEA.HI, SHF, IADD with `.CC`/`.X`.

Not supported (aborts with a message): geometry/tessellation, `.CC` except for I2I/IADD/LEA, half-precision ops,
FADD/FFMA/F2F with `.RM/.RP/.RZ`, indexed attributes, cube/array textures, 3D with AOFFI/DC, TLD4, TXQ except DIMENSION, IPA with
centroid/sample offset, barriers other than `BAR.SYNC 0` without thread count, 64-bit/float atomics, shuffle/vote.
ftz/fmz are ignored except for FMUL.RZ; vertex inputs are assumed to be float. Coverage of real game shaders:
`docs/analysis/07-shader-abdeckung.md` (LGP: VS 99.9 %, PS 93.7 % before this extension; re-measurement open).

Input is the SPH+code piece of one stage; `tools/analyze/bnsh.py` extracts it from BNSH containers
(format: [bnsh-format.md](bnsh-format.md)); `tools/analyze/shader_coverage.py` does the measurement over all stages.

## Findings from real shaders (P4.2)

1. **Branch targets at bundle starts.** Compiler code often branches (BRA, SSY, PBK) to the start of a 32-byte bundle,
   i.e. to the scheduling word. What is meant is the first instruction after it (+8). A translator that treats such targets as
   errors loses a third of the shaders.
2. **XMAD triple chain = 32-bit multiplication.** `XMAD d, a, b, RZ` / `XMAD.MRG t, a, b.H1, RZ` /
   `XMAD.PSL.CBCC d, a.H1, t.H1, d`. Semantics per XMAD: 16-bit halves (`.H1` = upper, otherwise lower; sign-extended
   for `s16`), product (32 bits, wrapping), `.PSL` shifts it left by 16; C mode `.CLO` (C & 0xffff), `.CHI`
   (C >> 16), `.CBCC` (C + (B << 16)), result `+ C`; `.MRG` replaces the upper 16 bits with the lower ones of B. For
   `.CBCC` and `.MRG` the **raw 32-bit B** counts, not the selected half.
3. **CC chains are short in practice.** `I2I.U32.U32.CC RZ, Rx` followed by `CSETP.NEU P, PT, CC, PT` is an
   "Rx ≠ 0". It is enough to keep the zero and sign flags as `bool` as long as only I2I writes the flags (all other
   `.CC` writers are rejected).
4. **Rebuild round-toward-zero exactly, not with float tricks.** FMUL.FTZ.RZ (before F2I in skinning shaders) does not exist in
   WGSL. A Dekker split with an error term computes exactly on paper, but on a real GPU (Intel,
   Vulkan) the round-to-nearest result came out anyway; the driver apparently rewrote it. Robust:
   compute mantissas as a 24×24-bit product in `u32` partial products, truncate, assemble the exponent
   (FTZ: denormals → 0, overflow → largest finite value). I2F.RZ: truncate to 24 significant bits, then
   `f32()`, which is then exact.
5. **Depth compare (TEXS.DC)**: `textureSampleCompare` with `texture_depth_2d` + `sampler_comparison`; the
   comparison operator comes from the sampler state, the runtime must set it. The same texture slot must not also be sampled
   normally in the same shader (the WGSL type is fixed per binding).
6. **End of program**: after the last `EXIT` there is a `BRA` to itself, followed by filler words. When counting
   opcodes over the whole code size, garbage gets counted too otherwise.

## Compute: resources, memory, barriers

Compute code has **no SPH** (and in BNSH no NVN code header); it starts at address 0 with the first
scheduling word (`ir::lift_compute`, `maxwell2wgsl --compute`). What the SPH otherwise provides is in the NVN control area
and must come from outside: workgroup size (`wgsl::Options::workgroup_size`, in WGSL as `override wg_size_x/y/z`,
the pipeline can override it), shared memory size (`shared_memory_bytes`, 0 = 16 KiB, WebGPU's
default limit) and local memory per thread.

| Maxwell | WGSL |
|---|---|
| `S2R` TID.X/Y/Z, CTAID.X/Y/Z | `@builtin(local_invocation_id)` / `@builtin(workgroup_id)` in private variables |
| global memory (64-bit addresses) | 8 storage slots: `@group(2) @binding(k) var<storage, read_write> gmK: array<atomic<u32>>`, plus a slot table `@binding(8)` with (address low, high, size) |
| shared memory | `var<workgroup> smem: array<atomic<u32>, N>` |
| local memory (LDL/STL) | `var<private> lmem: array<u32, N>` |

- **Global addresses** are computed by the shader itself from 64-bit base addresses in the driver constant buffer
  (with NVN one block per stage: vertex `c0[0x110 + 16 k]`, compute `c0[0x310 + 16 k]`, checked on LGP; compute also reads
  constant buffers this way, via `c0[0x210 + 16 · uniform block]`) plus offset, with IADD.CC/IADD.X or LEA/LEA.HI.X chains. The translator
  assigns an address at run time to the slot whose range contains it (`gmem_addr`); reading outside all slots
  returns 0, writing is dropped. That way the translator does not need to know from which constant buffer word the
  base comes. Limit: 8 slots (WebGPU's default `maxStorageBuffersPerShaderStage`).
- **Everything is `atomic<u32>`**, including normal loads/stores (`atomicLoad`/`atomicStore`). Otherwise
  ATOM/RED on the same memory would not be possible in WGSL (type fixed per binding). Sub-word stores (STG.U8/S16) go as
  `atomicAnd` + `atomicOr`, so that neighboring bytes of other threads are preserved. Cost: every access is atomic,
  barely noticeable on real GPUs, but measure it (P4.5).
- **Atomics without a WGSL counterpart** (INC/DEC with a limit, signed MIN/MAX on `u32` memory) run as an
  `atomicCompareExchangeWeak` loop.
- **Carry chains**: `.CC` sets zero/sign/carry/overflow as `bool`s, `.X` adds the carry; IADD with `neg` is
  `a + ~b + 1` (carry as on the hardware). `cc_test` for signed comparisons uses S≠O.
- **Shared memory is undefined at the start on the hardware**; games initialize it themselves. Test shaders must
  do so too: lavapipe+wgpu handed out workgroup memory **reused between workgroups** (the second
  workgroup saw the counter of the first), although WebGPU promises zero initialization. Therefore explicitly reset own helper variables
  (barrier counters) instead of relying on zero initialization.

### Barriers in divergent code (lockstep emulation)

`BAR.SYNC` becomes `workgroupBarrier()` (plus `storageBarrier()` for global memory), but only if the shader is
straight-line and the barrier is not behind a conditional EXIT. WGSL requires barriers in uniform
control flow; the dispatcher loop (`pc`) is never uniform, and Maxwell code lets threads exit before a barrier
(the hardware then only counts living threads). In that case the translator emulates the barrier:

- Each thread has a state (running, waiting at barrier, finished); workgroup counters `ls_running`/`ls_waiting`.
- A barrier ends the basic block; the thread sets `pc` behind the barrier and waits.
- The outer loop runs equally often for all threads: per round each running thread executes **one** block,
  then `storageBarrier(); workgroupBarrier();`, thread 0 decides (all finished → end; all living ones waiting →
  release), and all read the decision via `workgroupUniformLoad`, which makes the control flow uniform for WGSL.

Also correct for barriers in loops and after divergent EXIT (test `cs_lockstep`). Cost: one real barrier per
basic block step; only shaders with barriers in divergent code pay for that.

## Texture queries and offsets (TLDS, TXQ, TMML, AOFFI, LB)

- **TLDS** (texel fetch): `textureLoad` with integer coordinates; LZ = level 0, LL = level from register; AOFFI
  adds the offset to the coordinates (exact).
- **TEX.AOFFI**: offsets are packed in a register (4 bits signed each). If the register is unconditionally loaded with a
  constant in the same block, it becomes a constant WGSL offset `vec2<i32>(x, y)` (WGSL requires
  a constant expression there). Otherwise fallback: `uv + off / textureDimensions(t, 0)`, exact only for level 0.
- **TEX.LB** (LOD bias): `textureSampleBias` in the fragment shader, otherwise LOD = bias.
- **TXQ DIMENSION**: width, height from `textureDimensions`, depth 1 (UNVERIFIED for 2D), level count from
  `textureNumLevels`.
- **TMML** does not exist in WGSL. Rebuild: `dpdx`/`dpdy` of the coordinates times texture size, LOD =
  `0.5 * log2(max(|dx|², |dy|²))`; component 0 = relative LOD as a signed 8.8 fixed-point number, component 1
  = LOD clamped to the existing levels (8.8). Outside fragment shaders 0. Accuracy against the hardware
  unverified (the hardware rounds and filters anisotropically differently); test `fs_tmml` only checks the formula.

## Texture gather (TLD4)

- Without offset: `textureGather(c, t, s, uv[, layer])`; with `.DC`: `textureGatherCompare` on
  `texture_depth_2d(_array)`. Gather needs no derivatives, so it is also allowed in non-uniform control flow and in
  vertex shaders.
- **Offsets**: WGSL requires a constant expression in the range **−8..7**, NVIDIA allows −32..31. Constant offsets
  in this range become a WGSL offset; otherwise (larger or only known at run time) the coordinates are shifted by
  `offset / textureDimensions(t, 0)`. Gather always reads level 0, so this is exact except for float rounding at
  exact texel boundaries.
- **PTP** (one offset per texel) does not exist in WGSL. Rebuild following the GL/SPIR-V semantics of `textureGatherOffsets`:
  texel j is the (i0,j0) texel of the footprint at `uv + offset_j`, i.e. component `w` of four separate gathers.
- **View swizzle** (NVN path, `Options::texture_swizzle`): the gathered component must be a constant; the
  translator selects it via a `switch` over the swizzle word (ZERO/ONE return constant vectors).

## Integer vertex formats

Maxwell vertex shaders read attributes with `ALD` as raw bits. Whether an attribute arrives as float (normalized, float formats)
or as an integer (UINT/SINT, e.g. bone indices RGBA8UI) is decided only by the vertex format in the
pipeline state. WebGPU, on the other hand, requires the type of the shader input to match the format (`f32` for float/norm,
`u32` for `uint*`, `i32` for `sint*`). Solution:

- The translator stores inputs as `vec4<u32>` (`vinN = bitcast<vec4<u32>>(aN)`); the shader body reads the
  bits directly. For float inputs nothing changes.
- `wgsl::retype_vertex_inputs(code, {loc: Uint|Sint})` turns the module into a variant with `@location(N) aN:
  vec4<u32>`/`vec4<i32>` (for u32 without bitcast). The WebGPU backend creates it per program and set of integer
  inputs when building the pipeline (the pipeline depends on the vertex layout anyway) and caches it.
- WebGPU fills missing components of integer formats with (0, 0, 0, 1), like the hardware.
- Formats without a WebGPU equivalent are fetched like a matching integer format and converted in the variant
  (`VertexInputType`): "scaled" (`*_UI2F`/`*_I2F`, integer in memory, float value in the shader) as `uint*`/`sint*`
  with `vec4<f32>(aN)`; packed 10:10:10:2 formats (`RGB10A2UI`, `RGB10A2SN`, `RGB10A2I`, `RGB10A2_UI2F`/`_I2F`) as
  one `uint32` and unpacked in the shader (x bits 0–9, y 10–19, z 20–29, w 30–31; sign via `i32(p << k) >> 22u`,
  SNORM as `max(v / (511, 511, 511, 1), -1)`). Packed formats have no missing components; all four
  are used. Details and NVN values in [`nvn-hle.md`](nvn-hle.md) (section vertex formats).
- No solution: 3-component formats with 8/16 bits (NVN 25–33, 93–96); reading as x4 could access beyond the end of the buffer.
  These attributes are still dropped (input reads 0).

## Test tools and their quirks

| Tool | Role | Experience |
|---|---|---|
| naga-cli 30.0.1 (`cargo install --locked naga-cli@30.0.1`) | WGSL validation (front end of wgpu/Firefox) | fast, strict return analysis (point 5) |
| Python `wgpu` 0.32.0 (wgpu-native) | actually runs shaders, pixel comparison | runs headless on Mesa **lavapipe** (`mesa-vulkan-drivers`), adapter "llvmpipe" |
| npm `webgpu` 0.6.2 (Dawn/Tint, Chrome implementation) | compile and pipeline check with Tint | cause of the random crashes (`ProcessEvents` throws `system_error`, segfaults): the instance returned by `webgpu.create([])` was collected by the GC while adapter/device lived on. Keep the instance referenced globally (`globalThis.lgpGpuInstance = gpu`); then render and compute pipelines run stably |
| envydis/envyas | oracle for the decoder, assembler for test shaders | see ISA document |

Note for the pixel test: `rgba32float` is not filterable in WebGPU (a sampler with linear fails); for
test textures use `rgba16float` with exactly representable values. For integer results interpolate flat and
choose values that are exact as f32.
