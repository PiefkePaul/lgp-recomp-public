# NVN HLE in a recompilation runtime

Knowledge across games from P4.1 (as of 2026-10-11). Code: `gpu/nvn/`, tests: `tests/nvn/` (`ctest -L nvn`).
Basis: [`nvn-bootstrap-und-treiber.md`](nvn-bootstrap-und-treiber.md) (how games get to NVN),
decision: [ADR-0003](../decisions/0003-grafik-hle-lle.md). LGP-specific status: `06-nvn-hle-abdeckung.md`.

## Boundary: function pointers instead of a driver

Games fetch every NVN function once by name (`nvnBootstrapLoader` → `nvnDeviceGetProcAddress` → their own slots) and
from then on call only through the slots. An HLE therefore only has to answer these two lookups. The NVN driver in the
`sdk` module never runs; `nvdrv`, GPFIFO and syncpoints are not needed for graphics.

Implementation in a static recompilation:

- The HLE reserves a block of **thunk addresses** in the guest address space (4 bytes per function, here 1024 places) and
  enters them into the dispatch table of the indirect branches (ADR-0004). A `BLR` from lifted code to a slot thus lands
  directly in a host function. The addresses need no mapped memory; they are only keys.
- The table is directly indexed and spans all registered addresses. So put the thunk block **directly behind the
  last module**, not at the end of the code region (otherwise the table grows by dozens of MiB).
- The table passes no context to the target function. So that a function knows which name was called, there is
  a separate host function per thunk index (template instance `thunk<I>`).
- The export `nvnBootstrapLoader` in the `sdk` is overwritten by address in the dispatch table (the later
  registration wins). This is the "replacement by symbol" from ADR-0005 without touching the recompiler.
- Unknown `nvn…` names still get a thunk (without the driver's name list the HLE cannot know
  which names exist). If the game calls one that is not implemented, the guest thread stops with the name in the log
  (default), or the HLE returns 0 and counts the call (bring-up mode `UnimplementedMode::Continue`).
  Unimplemented `CommandBuffer` commands are recorded as placeholders, so that the backend can count
  what a frame skipped.

## Calling convention (AAPCS64) that is easy to get wrong

- Integer/pointer arguments in `x0…x7`, **floating point counted separately** in `s0…s7`. Example
  `ClearDepthStencil(cmd, float depth, NVNboolean mask, int stencil, int stencilMask)`: `x0=cmd, s0=depth, x1=mask,
  x2=stencil, x3=stencilMask`. The supplied guest test checks this with real lifted code.
- Narrow arguments (`NVNboolean` = 8 bits, `int`) only define their low bits; the upper bits of `x` are garbage.
  Always mask (`x & 0xFF`, `(int32_t)x`).
- Return in `x0` (also `NVNboolean`), `float` in `s0`.

## Object model

NVN objects are memory of fixed size that is opaque to the game and that the game provides itself. Two kinds:

| Kind | Examples | Where the HLE keeps its state |
|---|---|---|
| "real" objects | Device, Queue, MemoryPool, Buffer, Texture, Window, Sync, CommandBuffer | Host tables, key = guest address of the object. Nothing is written into guest memory, so no wrong object size can overwrite neighboring data. A second `Initialize` on the same address replaces the old object. |
| Builders and value objects | `*Builder`, TextureView | **inline** in guest memory. Games put them on the stack, copy them with `memcpy` and pass them by pointer; host tables would never be cleaned up there and would miss copies. |

For the inline states the **minimum size** of the guest memory counts: if the real object is smaller than our state,
we overwrite game data. That is why every assumption sits centrally in `abi.hpp`, every state has a `static_assert`
against it, and a magic tag distinguishes "`SetDefaults` ran" from uninitialized memory.

### Small state objects: 4-byte reference instead of an inline structure

The `*State` objects (Blend, Color, ChannelMask, DepthStencil, Multisample, Polygon, VertexAttrib, VertexStream) and the
`SamplerBuilder` are only a few bytes large on hardware; their real size is unknown without one's own `sdk`. Instead
of guessing a size, the HLE writes only a **32-bit reference** into the guest object (`states.hpp`): bits 24–31 a
type tag (`0xA1` Blend … `0xA9` SamplerBuilder), below that index + 1 into a host table. Every setter reads the value
behind the reference, changes it and **interns** the result (same values → same index), and writes the new
reference back.

- Only assumption: every object is at least 4 bytes large (`kStateReferenceSize`). This is safe because each of these
  objects must store at least one enum value.
- `memcpy` copies by the game stay valid (the reference points to an immutable value), stack objects
  need no cleanup, and the tables only grow with the number of **distinct** states.
- An invalid reference (object without `SetDefaults`, foreign memory) gives a one-time warning and the
  default values instead of crashing.
- Still guessed: the **stride** between array elements in `BindVertexAttribState(n, states)` and
  `BindVertexStreamState(n, states)` (`kVertexAttribStateSize` = 4, `kVertexStreamStateSize` = 8, checklist item 7)
  and the default values of `SetDefaults` (GL-like, item 8).

## Shader programs

`ProgramSetShaders(program, count, NVNshaderData*)` reads per stage `{GPU address of the code, control data}` (16 bytes).
The code lies in a MemoryPool; known from BNSH containers (P4.2) is a **0x30-byte NVN code header** with magic
`0x12345678` before the 0x50-byte SPH. The HLE accepts code with and without this header (magic comparison, only if the
address lies in a pool), determines the stage from the SPH type (1 VS, 2 TessCtl, 3 TessEval, 4 Geom, **5 Fragment**)
and translates at runtime with `gpu/shader` to WGSL (`HleOptions::program_translator`, cache over the code bytes of both
stages). Graphics: VS + FS; tessellation/geometry warn.

**Constant buffer banks** (measured on LGP's driver, P4.3): compiled NVN shaders address

| Bank | Content | Who binds |
|---|---|---|
| `c[0]` | Driver buffer (bound texture handles from byte 0x20, storage descriptors) | Driver |
| `c[1]` | **The compiler's constant pool** (literals that the code reads from memory) | Driver, from the code blob |
| `c[3 + i]` | Uniform block `i` | `BindUniformBuffer(stage, i, …)` |

`nvnCommandBufferBindUniformBuffer` writes `CB_BIND` with slot `index + 3` (`kUniformBufferBankBase`); the HLE
therefore records bank `3 + index`. The HLE reads the pool for `c[1]` at `ProgramSetShaders` from the
**control section** of the graphics stage (CPU memory, magic `0x98761234`): `+0x6FC` size, `+0x700` offset from the start of the
NVN code header, `+0x704` total size of the code blob (`kNvnControlConstants*`). Measured on all of LGP's BNSH shaders:
the offset is the end of the code, offset + size the end of the blob, and the literals from the assembler text in the
control section sit exactly there. `BindProgram` then binds the pool per stage as `c[1]`. Without it, e.g. an
alpha test `x <= c1[0]` reads against 0 and discards everything, and vertex shaders compute with wrong masks: that was the
cause of the completely black frames after P4 (`docs/analysis/11-erstes-bild.md`). The driver getters that read these
fields are not traced (virtual readers per control version); version fields of the section: `+0x0C` 0x120,
`+0x10` 0xB in all LGP shaders.

**Compute**: a program with exactly one stage counts as compute if it has no NVN code header or its
control section names stage 5. The **control section** (`NVNshaderData::control`, CPU memory, magic `0x98761234`)
contains everything the driver builds the QMD from. Offsets from three public tools that agree (origin
in `THIRD_PARTY.md`: uam-nvn `nvn_control.h`, ShaderLibrary `ControlShader.cs`, masagrator's glslc output dumper);
what they say about `+0x6FC`/`+0x700`/`+0x704` matches the measurement on LGP above:

| Offset | Field |
|---|---|
| `+0x6F8` | Code length (graphics from the SPH, compute from the entry point) |
| `+0x704` | Size of the code blob from `NVNshaderData::data` |
| `+0x708` | **Entry offset** of the code from `NVNshaderData::data`: graphics `0x30` (behind the NVN code header, at the SPH), compute `0` in LGP, `0x100` in newer glslc output |
| `+0x70C` / `+0x710` | Register count / scratch per warp |
| `+0x714` | `NVNshaderStage` (0 vertex, 1 fragment, 2 geometry, 3 tess control, 4 tess eval, 5 compute) |
| `+0x748` | Compute: workgroup x, y, z (u32 each) |
| `+0x754` | Compute: shared memory in bytes (rounded up to 256) |
| `+0x758` / `+0x75C` | Compute: local memory per thread, positive ("low") and negative ("high") part |
| `+0x760` / `+0x764` | Compute: CRS stack in bytes / number of barriers |

The compute fields follow the same order as deko3d's DKSH (`block_dims`, `shared_mem_sz`, `local_pos_mem_sz`,
`local_neg_mem_sz`, `crs_sz`, `num_barriers`). The HLE reads them with `parse_compute_control` (built in when
`HleOptions::compute_info` is empty), rejects implausible values (0 or more than 1024 threads, z > 64, more than
48 KiB of shared memory) and starts the code at the entry offset. **Checked on LGP** (checklist item 11):
entry `0`, workgroup 32×1×1 in all stages, and the code of all 24 extracted compute stages computes the index
as `ctaid.x · 32 + tid.x` (`ISCADD … 0x5`); shared memory, local memory and barriers are 0, and no
stage uses `LDS/STS/ATOMS`, `LDL/STL`, `BAR` or `tid.y/z`. These three offsets are thus consistent, but not
proven by a nonzero value (check with another game). The translation does not map the negative local memory
(it was 0 in all public examples). The WebGPU device requests the compute limits up to
1024 threads and 48 KiB of workgroup memory, as far as the adapter has them (the defaults would be 256 and 16 KiB).
`BindProgram` binds compute and graphics separately (`CmdBindProgram::compute`), stage bit compute `0x20` (UNVERIFIED).
`BindStorageBuffer(s)(stage, index, address, size)` and `DispatchCompute(x, y, z)` or `…Indirect(address)` are
recorded; `SetShaderScratchMemory` is accepted and ignored (local memory lives in WGSL variables).
The backend writes the storage descriptors (address, size) into the driver constant buffer `c0`, over whatever
the game has bound there. Each stage has its own 0x100-byte block there with stride 16 (`storage_desc_offset`):
vertex `0x110`, compute `0x310`, fragment `0x510` (UNVERIFIED), the way Ryujinx's shader translator lays out the buffer
(from memory, not checked against the source). Compute shaders also read their constant buffers via global
memory: `c0[0x210 + 16 · s]` holds address and size of NVN uniform block `s` (bank `3 + s`, s < 16; the block
ends at `0x310`, where the storage descriptors begin). The backend writes these descriptors for the bound
compute buffers and puts the ranges into their own entries of the slot table (up to four, as uniform bindings, counter
`compute_uniform_slots`). This keeps all eight storage slots free: LGP's program 68 binds all eight and still reads
uniform block 7.
Storage `0x310` and uniform blocks 6–8 are confirmed on LGP (item 12). Global loads know no bound: LGP's shaders also read
beyond the bound size (program 77: 288 of 1000 loads, within 64 KiB). A table entry of a
constant buffer therefore holds a window of 64 KiB from the base, as far as the memory is mapped; the descriptor in
`c0` keeps the bound size. If an address provably comes from exactly one storage descriptor, the
translated shader accesses its slot directly instead of searching the table (`wgsl::Options::storage_desc_offset`,
much shorter translation time, details in `webgpu-backend-draw-kosten.md`). The backend writes the results
back into guest memory after execution (limitations: `webgpu-backend-desktop-und-browser.md`, pitfall 12).
The write-back only comes after the submit of the whole list. A vertex stream whose range lies entirely in the storage buffer
of an earlier dispatch of the same list is therefore bound directly from this GPU buffer (counter
`compute_vertex_streams`, in `--frame-stats` "vertex streams from dispatch output"). LGP needs this: on the PC, up to
frame 10,500 about 93,000 vertex uploads read exactly the ranges that a dispatch wrote shortly before in the same list (skinning
or particles, 32×1×1). Index and constant buffers, textures and partially overlapping ranges still read from
guest memory and only see the result in the next list. What of this a game needs is counted by
`LGP_WEBGPU_DEBUG=computeoverlap`. The counters appear as `diag N: …` in `--frame-stats`:
- `same list: <Art> reads a range an earlier dispatch writes`: a reader in the same list that sees old bytes.
  The first 32 cases are logged with addresses.
- `later list: …` and `later list, writeback not arrived: …`: a later list reads the result, in the second case
  before the write-back has arrived.
- `writeback: compute left the buffer as uploaded`, `… equals guest memory already` and `… changes guest memory`:
  what the write-back changes in guest memory.
- `writeback: guest memory changed the compute output before a later list` or `… unchanged for 8 lists`: whether the CPU
  or copies overwrite the result before the next list.

What a single dispatch does with global memory is counted by `LGP_WEBGPU_DEBUG=computeprobe`
(`wgsl::Options::gmem_probe`). The translator then counts in the shader the started invocations as well as loads, stores and
atomics, split into "in a bound range" and "outside all ranges" (a load reads 0, a store is dropped).
It also remembers one address outside. The counters are a ninth storage buffer; the device is therefore requested with
`maxStorageBuffersPerShaderStage` ≥ 9, as far as the adapter can do it (WebGPU guarantees 8). Otherwise
the probe programs fail validation. `--frame-stats` shows sums as `diag N: probe: …`: dispatches without an invocation, with stores into a
range, only with dropped stores, without stores, with loads outside, with or without a bound c1. The first 48
dispatches and the first two per program are logged with counters, the address outside and the bound ranges.
The HLE logs per compute program the pool fields of the control section and whether c1 is bound. The
log lines contain guest addresses and stay local.

Bound textures in the shader (`TEX`/`TEXS` with handle word `w` from the driver buffer) are read as slot
`(w − 8) / 2`: the driver writes the 64-bit handle of slot `i` with `CB_POS`/`CB_DATA` to byte
`0x20 + 8 i` (measured, `kBoundTextureHandleBase` = 8, `kBoundTextureHandleStride` = 2). The translator rejects
bindless accesses.

**Separable programs**: `ProgramSetShaders` with only one graphics stage (LGP: hundreds of pure vertex or
fragment programs) succeeds; the stage is stored and, at `BindProgram(stage bits)`, combined with the other bound
stage into one backend program (cache over both codes). Rejected translations are remembered too,
and `BindProgram` reports once per reason when a program without shaders is actually bound for drawing.
`BindProgram(NULL, bits)` unbinds the named stages (no warning).

**Programs with only a vertex stage** (depth and shadow passes): LGP calls `BindProgram(NULL, FRAGMENT)` and then
`BindProgram(vs, VERTEX)` and draws without a fragment shader (on the PC 32 draws per frame in the room). If only
a vertex stage is bound at draw time, the HLE translates at the first draw a program from the vertex code and an empty
fragment stage (`translate_maxwell_program(vs, {})`, cached like pairs) and binds it; only at the draw, because at binding time
the fragment stage often still follows. `ProgramDesc::fragment_wgsl` stays empty. The WebGPU backend builds from it a
pipeline without a fragment stage if the pass has no color targets (allowed in WebGPU), otherwise with an empty
stub fragment shader and write mask 0 for all targets: an empty color format set would have to match the color targets of the
pass, and targets with mask 0 need no shader output. Result as on the GPU without a fragment shader:
depth/stencil are written, color is not. Open (check item 15): whether the console writes color in this case
(undefined content) – LGP probably uses such passes with masked color targets or without any.

**Global memory in graphics stages**: LGP's text vertex shaders read glyph data via `LDG` from storage buffers that the
game binds with `BindStorageBuffer(vertex, …)`. The translator allows read-only global memory in vertex and
fragment shaders (`wgsl::Options::graphics_global_memory`); the slots lie in the stage's texture group from binding 64,
the backend writes the descriptors into the stage's block (vertex `c0[0x110 + 16 k]`, fragment `c0[0x510 + 16 k]`).

## Textures, samplers, pools

- `TexturePoolInitialize`/`SamplerPoolInitialize(pool, memPool, offset, n)` only create host tables;
  `RegisterTexture(pool, id, texture, view)` / `RegisterSampler(pool, id, sampler)` fill them; the descriptors in
  guest memory are not written.
- **Texture rebuilt on the same object:** after a catch LGP calls `TextureFinalize` and `TextureInitialize` on
  the same `NVNtexture` (a larger version is loaded later) without registering the pool slot again
  (found by letsgo-recomp, commit 10812f1). The HLE therefore remembers the registered object per slot
  (`Hle::Impl::texture_slots`). `TextureInitialize` attaches the new texture ID to all slots that still point to this object.
  Before, the slot pointed to a destroyed ID, and the backend left the texture out (skip reason "texture:
  unknown texture id"). Test `nvn.hle` (`test_texture_rebuilt_in_place`).
- `BindTexture(stage, slot, handle)` stores the handle; it is resolved at **submit** against the pools that the
  queue knows at that time (texture ID bits 0–19, sampler ID bits 20–31), like the GPU when drawing. LGP sets the
  pools in the primary command buffer and draws in secondary ones (`CallCommands`); resolving at recording time found
  no pools there.
- The **texture view** in `RegisterTexture` supplies the channel swizzle (`TextureViewSetSwizzle`, NVN values ZERO, ONE,
  R, G, B, A = 0…5); it travels to the backend as `CmdBindTexture::swizzle`. Besides the identity (R, G, B, A), LGP uses
  e.g. (ONE, ONE, ONE, R) for single-channel UI textures and (R, R, R, G) for luminance-alpha. Mip/layer ranges and
  format views of the view are not applied yet. If a view sets more than the swizzle, the HLE reports it (info line
  "RegisterTexture view (not applied)", the first 32 variants, total count at powers of two).
- Texture content from the memory pool: format table, block-linear layout and upload are in
  [`tegra-block-linear-texturen.md`](tegra-block-linear-texturen.md).
- `TextureWriteTexels(Strided)` uploads directly into the backend, `CopyBufferToTexture` is recorded;
  `SetCopyRowStride` and `SetCopyImageStride` (distance of the layers on the linear side, 0 = dense) apply to
  following copies. Level and base layer come from the view, `z`/`depth` of the region select 3D slices or
  array layers; the WebGPU backend repacks each layer to 256-byte rows and copies all of them in one step.
- `CopyTextureToTexture(cmd, src, srcView, srcRegion, dst, dstView, dstRegion, flags)` (argument order and
  flag values UNVERIFIED, item 13) is recorded as `CmdCopyTextureToTexture`; level and layer come from the
  views. The WebGPU backend copies directly (`copyTextureToTexture`) when size, format and direction fit,
  copies within the same subresource via an intermediate texture; otherwise (scaling, mirroring, format change) it
  draws a blit (nearest, or linear with `kCopyFlagLinearFilter`). Ranges outside a texture are
  skipped (reasons "copy texture: …" in `Stats::skip_reasons`).
- State (program, bindings, bound state objects, viewport, scissor) **persists across command
  lists** like GPU state (LGP binds the textures of its last composition pass in an earlier
  command buffer); the WebGPU and soft backends carry it forward. Tests that execute several lists on one backend
  must therefore set what they rely on.

## State directly on the command buffer, counters, instancing, LOD bias

Besides the state objects, NVN sets some raster values directly on the command buffer. LGP calls them often per frame (mostly with
default values). Mapping to WebGPU (signatures and enum values UNVERIFIED, checklist item 14):

| NVN | WebGPU / HLE |
|---|---|
| `SetPolygonOffsetClamp(factor, units, clamp)` + `PolygonStateSetPolygonOffsetEnables` | `depthBias = round(units)`, `depthBiasSlopeScale = factor`, `depthBiasClamp = clamp`, only if the FILL bit is set. WebGPU allows depth bias only for triangles; points/lines: logged once |
| `SetDepthClamp(enable)` | `primitive.unclippedDepth` (feature `depth-clip-control`, requested with the device); WebGPU then clamps the fragment depth to the viewport's depth range like the GPU. Without the feature: logged once, clipping happens |
| `SetRasterizerDiscard(enable)` | Draws are dropped in the backend (`Stats::discarded_draws`), but they count for the primitive counters. Clears stay untouched (UNVERIFIED whether NVN clears follow the discard) |
| `SetSampleMask(mask)` | `multisample.mask`, only with multisampling (like OpenGL) |
| `SetDepthBounds`, `SetConservativeRasterEnable`, `SetLineWidth` | No WebGPU counterpart: accepted, logged once as soon as the game turns them on or sets a width ≠ 1 |
| `ColorStateSetAlphaTest(func)` (via `BindColorState`) + `SetAlphaRef(ref)` | Fixed-function alpha test in the translated fragment shader: `discard` if `alpha(target 0) <func> ref` does not hold; overrides `lgp_alpha_func`/`lgp_alpha_ref` per pipeline. LGP uses `GREATER` (func 5, PC log play-99) |
| `ReportCounter(type, address)`, `ResetCounter(type)` | The HLE writes `NVNcounterData` (64-bit counter value, 64-bit timestamp) itself at submit, after the list went to the backend |

**Counters** (`submit_counters` in `procs.cpp`, per queue): there are no hardware counters; the HLE estimates from the draws in
command order. `TIMESTAMP`/`TIMESTAMP_TOP`: counter value 0, timestamp = host time at submit in GPU units
(ns · 384 / 625, = system tick · 32); GPU time measured by the game is therefore ≈ 0. Vertex and primitive counters come
exactly from count and topology (× instances). `SAMPLES_PASSED` and fragment invocations: without occlusion data, every
rasterized draw adds its primitive count, so a pass with a draw counts as visible, one without as 0. Tessellation,
geometry and transform feedback stay 0, `ZCULL_STATS` is not written. The HLE logs which types the game reads
at the first report per type.

**Instancing**: `VertexStreamStateSetDivisor` ends up as `CmdSetVertexLayout::divisors`. Divisor 1 is WebGPU's
`stepMode: "instance"`. WebGPU does not know larger divisors; the backend binds such streams with stride 0 and
splits the draw into runs of `g` instances (`g` = gcd of the divisors > 1), in which each of these streams reads the same
element, and sets the buffer per run to `(base instance + instance / divisor) · stride` (OpenGL semantics: the
base instance is not divided). Cost: `instances / g` WebGPU draws; from 65,536 runs on it is skipped. Streams
that would be read beyond their bound range skip the draw instead of invalidating the whole WebGPU command
buffer.

**Vertex buffer slots**: NVN has 16 vertex streams, WebGPU only guarantees 8 vertex buffers and counts every slot up
to the last one against `maxVertexBuffers`, unused ones too (wgpu-native even rejects gaps entirely). LGP binds more
stream states than it reads (on the PC: "number of vertex buffers 9 exceeds the limit 8"). The backend therefore gives a slot only to
streams that an attribute reads and numbers them densely (`vertex_slots`); the null buffer for
inputs without an attribute gets the slot after them. The renderer requests the adapter's `maxVertexBuffers` (desktop
mostly 16 or more). Chrome/Dawn, however, allows a fixed 8, and LGP has draws that really read 9 streams (in the browser
on the PC: 92 skipped draws up to Route 1, 5176 in battle, "pipeline missing or invalid"). If a draw reads more
streams than allowed, the backend therefore merges streams (`vertex_merge`): it picks as many streams with
the same step as needed, the narrowest first: per-instance streams first (divisor 1, as in LGP: one
per-vertex stream and eight instance streams with stride 16), otherwise per-vertex streams (divisor 0). At recording time they are written interleaved into one buffer of the
geometry arena (stride = sum of the read widths rounded to 4 bytes, at most 2048). The attributes of these streams
then point with a shifted offset into the one slot. The element count is the minimum over the streams; the result
is cached per list by address, size and stride. Cost: one CPU copy of the merged streams
per list. Streams with a divisor above 1 (rebound per instance run) are never picked, nor are streams that read a
compute output of the same list (particles in LGP's battles: three of the eight instance streams). Their data lies only
on the GPU, and merging copies on the CPU. Which ones these are follows from the commands (`DispatchWrites`:
storage bindings of the compute stage at each dispatch), the same in the pipeline pre-pass of `execute()` and at recording,
and is part of the pipeline key (`merge_exclude`, recipe version 3). An unbound picked stream
reads zeros. If recording still runs into a compute output, the draw is skipped with its own reason. Only when even that is not enough is it skipped (warning up to eight times with the stride/divisor list).
`LGP_WEBGPU_MAX_VERTEX_BUFFERS=N` lowers the limit natively for reproducing this (test `nvn.webgpu_vertex_streams` with 16, 8
and 4, each per vertex and per instance from base instance 1, plus two instance streams from a dispatch; in the browser `browser.webgpu_vertex_streams`).

**Sampler LOD bias**: WebGPU samplers have no bias. It sits in bits 16–31 of the swizzle word of the texture binding
(signed, 8 fractional bits, like the hardware), and the translated shader (`wgsl::Options::sampler_lod_bias`)
adds it: implicit LOD via `textureSampleBias`, explicit LOD and `LZ` as `level + bias`, shader bias as a sum.
Depth compare lookups (WGSL has no bias form) and texel fetches ignore it. The backend's sampler cache
ignores the bias; equal samplers with different bias share one `WGPUSampler`.

## GPU addresses: identity

The GPU address of a pool (`MemoryPoolGetBufferAddress`, `BufferGetAddress`) is, in the HLE, the **guest address of its
memory**; `MemoryPoolMap` returns the same address as a CPU pointer. Advantages: a backend reads vertex, index and
uniform data directly via the normal guest accesses, there is no GPU MMU, the game's address arithmetic (base + offset)
stays valid, alignment is preserved. Risk: a game that expects GPU addresses in certain ranges (e.g.
above 4 GiB) or compares them with CPU addresses. Not observed so far; check as soon as real calls are logged.

## Command recording and execution

- `BeginRecording` … `EndRecording` records on the host; the game's command and control memory
  (`AddCommandMemory`/`AddControlMemory`) is accepted but not written. The game's memory callback therefore
  never fires (on hardware it fires when the memory is full; the game does not have to experience that).
- `EndRecording` returns its own handle value (never 0). It is released at `DeviceFinalizeCommandHandle`,
  `CommandBufferFinalize`, or when the memory it was recorded into is given to the command buffer again
  (`AddCommandMemory`/`AddControlMemory` overlaps a range that was active during the recording): on hardware
  the list is then overwritten. Games rarely finalize handles and reuse command memory every frame;
  without this release the table grew (LGP on the PC: three times "4096 command handles alive"). If a game never gives new
  memory (the HLE never triggers the memory callback), the growth remains. The HLE reports every new peak
  from 4096 handles on, after that at every doubling (info, warning from 16384), with the number of handles
  released so far. A table that stops at one size is bounded; a leak keeps doubling. LGP after #93
  (intro, ~59 s): 4096 was reached four times, so the table fell below it again in between.
- `QueueSubmitCommands` records immediately and sends off. Fences (`QueueFenceSync`) are therefore signaled immediately,
  `SyncWait` reports `ALREADY_SIGNALED`. Only compute write-backs into guest memory can still be pending;
  `QueueFenceSync` remembers `Backend::fence_mark()`; `SyncWait` waits via `finish_until(mark)` only for write-backs
  of lists before the fence, `QueueFinish` via `finish()` for all of them. LGP calls `SyncWait` three times per frame (PC,
  battle). Tried and dropped (2026-10-29): letting both wait without the HLE lock while the graphics thread runs.
  Native on the Mac M1 (town scene, 9001–9600), the unlocked arm was no faster than main (rotated ABAB, 62–70 fps
  in both), and the image was the same. The runs had two states, ~500 or ~750 wakeups per frame, for reasons not
  found; so the lock contention seen in the kernel profile did not cost fps there. (WebGPU: pitfall 12 in
  `webgpu-backend-desktop-und-browser.md`), `WindowAcquireTexture` hands out the window textures in turn.
- `QueuePresentTexture` passes the texture with the crop rectangle (`WindowSetCrop`) to the backend. The rectangle is in
  **window coordinates**: with a lower origin the HLE converts `y` into memory rows (`h − (y + height)`). In handheld mode LGP
  draws 1280×720 with viewport and crop (0, 0) into a 1920×1080 window texture; the visible part
  thus lies in the lower 720 memory rows.

## Frame pacing: present interval

On the console, `nvnQueuePresentTexture` shows a frame only at the next matching vertical retrace of the
60 Hz display. `nvnWindowSetPresentInterval` (or `WindowBuilderSetPresentInterval`) sets how many retraces
a frame stays: 1 = 60 fps, 2 = 30 fps, 0 = without waiting. Games that advance their logic once per frame rely
on this. The HLE presents immediately and synchronously; without pacing such a game runs as fast as the host draws.

- The HLE remembers the interval of the last presented window (`Hle::last_present_interval()`) and logs
  every change (`nvn: present interval N`, info).
- The caller does the pacing, not the HLE: `lgp_boot` (`NvnOptions::pace_hz`, `lgp/boot/frame_pacer.hpp`) computes after
  every present when the next frame is due (interval × 1/60 s after the last due time), and
  waits the difference **as `svcSleepThread`** of the presenting thread. That way the other guest threads keep running in the
  baton scheduler; a host `sleep` in the thunk would hold on to the core slot.
- The pacer catches up short delays (the next frame waits correspondingly less). If a frame is more than one
  period behind, the pacing starts over instead of pushing frames out unthrottled afterwards.
- At the end comes `present: waited N ms for the display`: the total wait time. Close to 0 means the host does not manage the
  interval, and the game runs slower than on the console.
- Default: paced with a window (`--renderer window`) and in the browser (`--canvas`), otherwise unthrottled, so that measurement runs
  and tests stay fast. Force with `--paced` or `--unthrottled`.
- In the browser the same mechanism paces (sleeping in the guest pthread), not `requestAnimationFrame`. The canvas with
  `Fifo` shows only at the display rate anyway; on 120 Hz displays the game still stays at its 60 Hz pace.
- The runtime still never signals the `vi` vsync event (`GetDisplayVsyncEvent`). If a game waits for it instead of
  the present interval, it needs a host timer for that.

Measured on LGP (PC, 2026-10-12, main ca47295, script up to the Pikachu catch from
`../analysis/17-intro-und-erste-ingame-szene.md`): the game sets
**interval 2 once at the start** (`nvn: present interval 2`, right after the first frame) and never changes it afterwards, not
in the video, intro, house, Pallet Town or on Route 1. The target is therefore **30 frames/s**. Without a window, with
`--unthrottled` and `--clock virtual` (guest time jumps while waiting, the frame rate shows only the drawing effort), over
host time, two runs:

| Frames | Scene | Frames/s |
|---|---|---:|
| 3000–5000 | Intro, house | 159 / 153 |
| 5000–7000 | House → Pallet Town | 45 / 42 |
| 7000–9000 | Pallet Town, edge of town | 47 / 43 |
| 9000–10,500 | Route 1 | 32 / 32 |

With the host clock and `--unthrottled`, LGP outside ran at 30–34 frames/s (boot test thread). With the virtual clock the same
stretch in Pallet Town is clearly faster: so the limit at about 30 does not come from FIFO or the window but from the
game itself, which waits for its 30 Hz pace. That matches the console. On Route 1 the drawing effort alone is
already close to 30 frames/s; there is no reserve there.

## Depth mode (clip-space z)

Like OpenGL, NVN starts with `NVN_DEPTH_MODE_NEAR_IS_MINUS_W`: clip `z` lies in [-w, w], the window depth is
`near + (far - near) * (z / w + 1) / 2`. WebGPU (like Vulkan and D3D) clips at `z < 0` and takes `z / w` directly as
depth. Without conversion WebGPU thus cuts away the front half of the NDC range. With perspective that is everything
closer than about twice the near distance, visible as a hole in close-ups and in models close to the camera. With
orthographic shadow maps half of the depth is missing, and the reference (computed by the game for [0, 1]) and the stored
depth do not match. The HLE passes the device's mode along per command list (`CommandList::depth_minus_w`;
`nvnDeviceSetDepthMode` is logged with an info line). The vertex shader then computes `z = (z + w) / 2`
(WGSL override `lgp_depth_minus_w`, per pipeline). Since this was missing until P4.4, all rendered frames were affected by it.
Test: `nvn.webgpu_draw` (quad at z = -0.5).

## Depth compare: filter and PCF in the shader

On Maxwell the sampler's min/mag filter (TSC) decides what a depth compare (`TEX*.DC`, compare mode
R_TO_TEXTURE) returns. With LINEAR the hardware filters 2×2 and returns intermediate values (hardware PCF), with NEAREST only
0 or 1. The backend takes the filter over unchanged into the WebGPU comparison sampler, so the behavior
matches. Games with NEAREST smooth in the shader themselves: N lookups with offsets from the constant buffer, averaged. That
gives at most N + 1 gray levels; hard steps at shadow edges are then faithful to the original and not a translation error.
This can be checked with the info log "depth compare on format … filter min … mag …" (format, size, filter of the
shadow map) and with the viewport and depth bias of the shadow pass.

LGP (2026-10-12, PC): the shadow map has format 53 (Depth24Stencil8), 2048², sampler NEAREST/NEAREST. 452 shaders
each contain exactly 10× `TEXS 2D.DC` on handle 0, without AOFFI/PTP, `TEX.DC` or `TLD4.DC`. The shadow pass renders
with full viewport and scissor (2048², no atlas) and without hardware bias (flags, units, factor and clamp are 0).
The test `nvn.webgpu_draw` (shader `tests/gpu/shaders/fs_shadow_pcf.s`) rebuilds the pattern: the depth map is
0.25 in one half and 0.75 in the other, 7 of 10 taps pass, the result is 0.7. No error is visible
on our side. The "pixelated" shadows from the game test are probably faithful to the original; an image comparison with Ryujinx
or real hardware is still pending.

## Window origin and scissor

Like OpenGL, NVN starts with `WINDOW_ORIGIN_MODE_LOWER_LEFT`: scissor and viewport `y` count from the bottom. The HLE's model
(since P4.3, confirmed on the real game): **window row `y` is memory row `y`** in both modes; with a lower origin memory row 0 is
the *bottom* image row, as in OpenGL (framebuffers and textures "bottom to top"). It follows:

- Scissor, viewport, clears and `WindowSetCrop` apply unchanged as memory rows.
- Clip `y` is negated in the vertex shader (WGSL override `lgp_flip_y`, per pipeline) so that NDC +1 lands in the last
  row; this flips the front face (CW ↔ CCW).
- With a lower origin the display shows the window texture **mirrored** (`PresentInfo::flip_y`).
- Textures from files and CPU-written textures likewise lie bottom to top in memory.

Evidence in LGP: the font cache (linear R8 texture) fills from the last row on and contains the glyphs upside down;
with the old model (row `h-1-y`, unmirrored display) text and control symbols appeared upside down, and the
crop rectangle (0, 0) lay in the lower memory rows. With the new model text, symbols and crop are right without
a special case. Because every drawing stage contains both mirrorings (rasterization and sampling), the error only showed up with
CPU-written textures.

## Error handling without `longjmp` through the HLE

The runtime turns memory faults in lifted code into a halt via `longjmp`. HLE functions, however, hold a
mutex and host objects with destructors; a `longjmp` through them leaves a locked mutex behind. That is why every
guest memory access of the HLE checks the page table itself (`guest_access.hpp`): reading yields 0, writing is dropped, and the
thunk sets `kHaltMemFault` with the address after the function returns. The test `nvn.hle` checks that the HLE
is still usable afterwards.

## Vertex attribute formats

`nvnVertexAttribStateSetFormat` takes a value from the shared format enum. The pure vertex formats lie at
81–108 (after that 109–120 RGBX, 121 ASTC 4x4); mapping by public enum order, **UNVERIFIED** (checklist 16):

| NVN | Name | Backend (`VertexFormat`) | WebGPU fetch, conversion in the VS |
|---|---|---|---|
| 62 | RGB10A2UI | `Uint10_10_10_2` | `uint32`, fields split |
| 81–92, 97–104 | R/RG/RGB32/RGBA × 8/16/32 `_UI2F`/`_I2F` | `UScaled*`/`SScaled*` | `uint*`/`sint*`, `vec4<f32>(a)` |
| 93–96 | RGB8/RGB16 `_UI2F`/`_I2F` | – | not supported (no 3×8/16-bit format) |
| 105 | RGB10A2SN | `Snorm10_10_10_2` | `uint32`, SNORM fields |
| 106 | RGB10A2I | `Sint10_10_10_2` | `uint32`, signed fields |
| 107/108 | RGB10A2_UI2F/_I2F | `UScaled/SScaled10_10_10_2` | `uint32`, fields → float |

The conversion is done by `wgsl::retype_vertex_inputs` (type from `nvn::vertex_format_input`); the WebGPU backend caches one
vertex shader variant per program and input layout; details in
[`maxwell-nach-wgsl.md`](maxwell-nach-wgsl.md#integer-vertex-formats). Unmapped formats report a
warning once, the attribute is dropped (reads 0). Reason: LGP uses 105 on Route 1. Tests: `nvn.hle` (format 105 → `Snorm10_10_10_2`),
`nvn.webgpu_draw` (draw with `RGB10A2_I2F`), `gpu.render` (`case_converted_input`).

## Checklist for a game's real NVN version (local, with one's own `sdk`)

Everything in `abi.hpp` and the signatures in `procs.cpp` are written from the public form of the C API and from memory of
public homebrew/mod code and marked **UNVERIFIED**. With the driver table from
`nvn_inventory.py` (name → implementation in the `sdk`), every assumption can be checked on one's own dump without taking code
into the repo (only numbers and names):

1. **Builder/value object sizes**: `…SetDefaults` resets the whole builder (memset/stores up to the end); the
   highest written address + width is the minimum size. Compare with `kDeviceBuilderSize` … `kTextureViewSize`.
2. **`nvnDeviceGetInteger`** (done for LGP, `10-nvn-pruefliste-und-renderschleife.md`):
   the implementation is a `switch` over the key; the constant cases are now in
   `default_device_integers()` as measured (API 53.209). Cases that read fields of the initialized device are missing.
3. **Format enum** (done for LGP, P4.3): the driver's format table (0x30-byte entries, 161 formats) is in
   `abi.cpp` (`kFormats`) as measured, structure in [`tegra-block-linear-texturen.md`](tegra-block-linear-texturen.md).
4. **Signatures**: for every implemented function, check the registers that the implementation reads (e.g. whether
   `TextureViewGetLevels` takes two out pointers, whether `WindowAcquireTexture` takes a sync pointer).
5. **Window origin**: compare a scissor clear in the game (or one's own homebrew with NVN, if available) against Ryujinx.
6. **Handle format** (confirmed for LGP, P4.3): texture ID bits 0–19, sampler ID bits 20–31; with this decoding
   all of the game's bound handles find registered pool entries.
7. **Array strides of the state objects**: `CommandBufferBindVertexAttribState`/`…StreamState` loop over an array;
   the loop step in the implementation is the real object size (`kVertexAttribStateSize`,
   `kVertexStreamStateSize`). While at it, note the sizes of all `*State` objects (`…SetDefaults`).
8. **Draw and state enums**: `DrawPrimitive`, `IndexType`, `ShaderStage`, `CompareFunc`, `StencilOp`, `Face`,
   `FrontFace`, `PolygonMode`, `BlendFunc`, `BlendEquation`, `LogicOp`, `Min/MagFilter`, `WrapMode` in `abi.hpp` are
   guessed GL-like. The translation tables in the `…Set…` implementations, or the method values they write into the
   command stream, show the real values. Plus the default values of every `…SetDefaults` (blend off, depth test
   off, cull off, CCW front?).
9. **Base of the bound texture handles** (done for LGP, P4.3): base 8, stride 2 words per slot, measured from
   `nvnCommandBufferBindTexture` (`CB_POS` = `0x20 + 8 · slot`).
10. **Open semantics**: does bound state persist across command buffers? Does LGP use
    `DeviceSetDepthMode(NEAR_IS_MINUS_W)` (then the depth range must be converted)? Does LGP re-register textures in the
    pool after it has recorded commands (then resolving at recording time is not enough)? Does it use TextureViews
    in `RegisterTexture` (formats/mip ranges)? All of this can be answered via `trace` on the real game.

11. **Control section of compute stages** (layout from public sources, to be confirmed on LGP):
    `python -I tools/analyze/bnsh.py compute <romfs> --exefs <exefs>` prints only counts (no data, may go into
    the thread). Expected: `control_readable` = number of compute stages (24), `workgroup_plausible` likewise,
    `reflection_match` = all (workgroup size in the control section = workgroup size in the BNSH reflection data; its
    position comes from bnsh-decoder's `BNSH.ksy` and is itself UNVERIFIED; `reflection_missing` only means that it
    was not readable). `entry_offset` shows whether LGP uses `0x100`; `prefix_zero` counts stages whose bytes before the
    entry point are zero. Second, independent probe: Ryujinx's guest shader cache stores per compute shader the local size
    and shared memory size from the QMD; they must match `workgroup` and `shared_bytes`. Then in the real game:
    the message "compute control section" must no longer appear, and `--frame-stats` should no longer count skipped
    dispatches ("dispatch: no compute program"). If something is off, only adjust the offsets in `abi.hpp`
    (`kNvnControl*`) and `bnsh.py` (`CTL_*`). Measure coverage again: `bnsh.py extract` writes the
    compute values into the manifest, `shader_coverage.py` passes them to `maxwell2wgsl` (`--workgroup/--shared/--local`).
    **Measured on LGP 1.0.0 (PC, 2026-10-12):** 39 unique compute stages (not 24), `control_readable` 39,
    `workgroup_plausible` 39, `reflection_missing` 39 (the reflection position from `BNSH.ksy` reads nothing
    plausible in LGP, so no comparison is possible). `entry_offset` is `0x0` for all 39, not `0x100`; the first 0x100
    bytes of the code are zero in no stage, and the value at control +0x700 (and +0x704) equals the
    code length in all of them. So the compute code starts directly at the beginning of the code block. Strikingly uniform: `workgroup`
    32×1×1, `shared_bytes` 0, `local_bytes` 0 and `barriers` 0 in all 39 stages; whether the `CTL_CS_*` offsets
    are right is therefore not proven and needs the second probe (Ryujinx cache).
    **Cross-check on the code (2026-10-12, instruction count over the 24 stages in `shaders/stages`, numbers only):** all 24
    read only `tid.x` and `ctaid.x` and form `ctaid.x · 32 + tid.x` via `ISCADD … 0x5`; none uses shared
    memory, local memory, barriers or `tid.y/z`. This confirms workgroup 32×1×1 at offset `+0x748` and matches
    the zeros at `+0x754/+0x758/+0x764`. The reflection probe is dropped; the Ryujinx probe is no longer needed.
12. **Storage buffer descriptors** (checked on LGP, 2026-10-12, counts only): with `0x110` for all stages,
    LGP's compute programs ran but wrote nothing. Their stores were silently dropped because no address lay in a
    bound range (`computeoverlap`: "writeback: compute output changes guest memory" 0). The 1365
    LDG/STG accesses of the 24 stages read their addresses from `c0[0x310 + 16 k]` for k = 0…7 (storage buffers, upper
    word at +4) and from `c0[0x270]`, `[0x280]` and `[0x290]` (`0x294` in 22 of 24 stages). From this follows compute
    `0x310`. The probe (`computeprobe`, PC up to frame 2000, 5191 dispatches) then showed: all dispatches loaded only
    outside all ranges (address < 0x100, so base 0) and stored nothing. Bound for compute were exactly
    uniform blocks 6, 7 and 8 (banks 9–11). Three bound blocks and three read words 16 apart give
    `c0[0x210 + 16 · block]`. The first approach `0x260 + 16 · bank` (Ryujinx's "UBE", from memory) hit none
    of them. A compiler constant pool does not exist in LGP's compute programs (control +0x6FC is 0 in all 24
    programs); c1 stays unbound for compute. Still open is the stage bit of `BindProgram` for compute
    (`kShaderStageBitCompute`).

13. **`nvnCommandBufferCopyTextureToTexture`** (checked for LGP): the implementation in the `sdk` reads x0–x7 as cmd,
    src, srcView, srcRegion, dst, dstView, dstRegion, flags; views may be NULL (then the texture's format applies,
    otherwise the view's, if its format bit is set). With a 1D array target (3) the layer is in the y field of the
    region. Tested flag bits: 0x1 (linear filter), 0x4/0x8/0x10 (mirroring X/Y/Z); this function does not read 0x2.
    LGP: 29 copies in the controller screen, all as a plain copy (no scaling, no blit).

14. **State commands on the command buffer** (UNVERIFIED, public header form): `SetPolygonOffsetClamp(cmd, s0
    factor, s1 units, s2 clamp)`, `SetDepthClamp/SetRasterizerDiscard/SetConservativeRasterEnable(cmd, bool)`,
    `SetDepthBounds(cmd, bool, s0, s1)`, `SetSampleMask(cmd, int)`, `SetLineWidth(cmd, s0)`,
    `SetCopyImageStride(cmd, ptrdiff_t)`, `ReportCounter(cmd, type, address)`, `ResetCounter(cmd, type)`;
    `NVNcounterType` 0 = TIMESTAMP … 16 = TIMESTAMP_TOP (`CounterTypeNvn`), polygon offset bits POINT 1, LINE 2,
    FILL 4. Check on the `sdk`: registers of the implementations, whether `ReportCounter` writes 16 bytes and which type arrives
    from the game (log "first report of counter type"), whether rasterizer discard also suppresses clears, and whether the
    polygon offset `units` value goes into the register 1:1 or doubled (other emulators divide the register value by 2 for
    OpenGL).
15. **Draws without a fragment program**: do LGP's depth/shadow passes have color targets bound, and if so, with which
    channel mask? (Log or frame dump; the HLE writes no color in this case.)

16. **Vertex formats 81–108** (`procs_draw.cpp`, `vertex_format`): the values come from the order of the
    public enum names, not from the format table (the `kFormats` measurement only covers texture formats). Check in
    the conversion NVN format → Maxwell vertex attribute (size/type field) in the `sdk`: 105 must give SNORM 10_10_10_2,
    81–92/97–104 USCALED/SSCALED, 106–108 INT/USCALED/SSCALED 10_10_10_2.

## Bring-up with a real game

Experience with LGP (P4): games draw almost everything via prerecorded lists (`CallCommands`); an
`nvnProgramSetShaders` that returns `false` because of a gap in the translator is fatal for the game, hence
`HleOptions::accept_untranslated_programs` (the program's draws are dropped, reasons counted). For measuring without passing
frames on: `lgp_game --frame-stats N` (share of non-black pixels, backend counters, reasons for skipped commands).

`HleOptions::unimplemented = Continue` plus `trace = true` logs every NVN call with name, `x0…x4`, `s0/s1` and lets
the game keep running. This yields (a) the order of the next functions, (b) the argument values for the
open fixed-function questions from ADR-0003 (alpha test, depth bounds, conservative raster, wide lines). `Hle::stats()`
and `Hle::unimplemented_calls()` summarize at the end what was called.
