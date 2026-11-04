# 0003 – Graphics: NVN HLE vs. Maxwell LLE; shaders offline vs. at runtime

Status: **Accepted** (Paul, 2026-10-10) · Date: 2026-10-10 · Updated: 2026-10-10 (P1.6, measurement data from P1.3–P1.5) · Decides: Paul · Gate: data from P1.4

## Context

Switch games render via **NVN**, NVIDIA's proprietary graphics API. The NVN implementation sits in the game's SDK NSOs and produces Maxwell commands (GPFIFO) that go to the GPU via `nvdrv` ioctls. NVN is not publicly documented. Known from the modding scene: games fetch NVN function pointers via `nvnBootstrapLoader`/`nvnDeviceGetProcAddress` **by name**; overlays hook in exactly there. P1.4 confirmed that LGP does it this way (next section).

In Switch games, shaders are precompiled as Maxwell machine code (typically in the RomFS). Independently of HLE/LLE, a translator **Maxwell shader → IR → WGSL** (or SPIR-V/MSL for later platforms) is therefore needed.

References: Ryujinx (MIT, LLE at the Maxwell level, shader translator to GLSL/SPIR-V, by now also Metal) and the yuzu heritage (GPLv2+, shader recompiler; adopting it would be copyleft and needs Paul's approval). Both emulators work **LLE**; to my knowledge no open project has an NVN HLE.

## Measurement data (P1.3–P1.5)

Sources: `03-grafik.md`, `02-services.md` §1, `01-inventar.md` §3/§6.

| Gate criterion / question | Result | Met? |
|---|---|---|
| NVN via bootstrap loader by name | Yes: `main` imports only `nvnBootstrapLoader`, a proc loader fills 498 function pointer slots once, afterwards only calls via the slots | **yes** |
| Number of NVN functions used < ~300 | **230 of 498** (static ∪ runtime), 204 ran up to Route 1; focus 65 `CommandBuffer`, 10 `Queue`, 7 `Window` | **yes** |
| Central structures roughly understood | **Not yet.** But: the NVN driver sits entirely in `sdk`, with a name table and a parallel implementation table. For every function the implementation is known; many getters/setters directly show field offsets. Objects belong to the driver, the game only provides memory of fixed size. | **partly** – reverse engineering is well prepared, but not done |
| Runtime shader compilation (glslc) | **no** up to Route 1: none of the `glslc*` functions ran, the only collecting path in `main` never ran | – |
| Shader inventory | ≈ 2,450 unique graphics stages in ≈ 8,400 BNSH (plus compute, not counted); used up to Route 1: 527 (212 V, 307 P, 8 compute, 0 geometry/tessellation) | – |
| Origin of the runtime shaders | **500 of 527 (95 %) byte-identical in RomFS/`main`**; 27 small shaders (boot/title) of unknown origin | – |
| Textures | 20,675 in 8,689 BNTX: 94.5 % BCn, 1.3 % ASTC 4×4 (271), 4.2 % uncompressed; all block linear | – |
| GPU load at runtime | `nvdrv` with 3 channels, ≈ 30 `SubmitGpfifo`/s (30 fps), presentation via `vi`/binder | – |

### Critical review of the recommendation

1. **The data clearly supports NVN HLE.** The interface is narrow (function pointer slots, no inline accesses to object contents), the scope of 230 functions is below the guideline value, and every implementation can be found in `sdk`. LLE, by contrast, would mean: recompiling the NVN driver from `sdk` too, and emulating `nvdrv`, GPFIFO, the GPU MMU and the Maxwell classes below it.
2. **The third gate criterion is not met.** To be honest: the structures are not "roughly understood", only easily accessible. The recommendation is still A, because the alternative B is not cheaper (see point 4) and accessibility via the driver table significantly lowers the main risk of A (missing documentation). The criterion is rephrased into an **abort criterion in P4.1** (see recommendation).
3. **Offline shaders are confirmed, the runtime fallback remains necessary.** 95 % of the shaders used come from the data. For the 27 without a known origin there are two ways: clarify the origin (Ghidra, addresses at `ProgramSetShaders`) or translate at runtime. GLSL→WGSL is **not** needed.
4. **The biggest graphics risk is independent of HLE/LLE.** The game writes vertex, index and uniform data directly into mapped GPU memory, without reporting each write via the API. The browser lacks page protection (see ADR-0001, point 4). LLE would have exactly the same problem plus a GPU MMU. This is another argument for A: with HLE we at least know every draw call and its bound buffer ranges.
5. **WebGPU gaps are known and limited:** alpha test (→ `discard` in the shader), depth bounds, conservative raster, wide lines, rasterizer discard, bindless handles (→ own handle scheme between HLE and shader translator), no geometry/tessellation stages (16 stages in the data, not used up to Route 1). Whether the fixed-function features are actually switched on will be shown by argument logging in our own HLE.
6. **Textures:** desktop WebGPU can use BCn directly with `texture-compression-bc`. The 271 ASTC textures are converted offline; deswizzling (block linear) belongs in the same offline step. For P7 (mobile) this is reversed.

## Options

### A: HLE at the NVN API level

We replace the NVN functions (via the bootstrap loader) with our own implementations on `IRenderer` (WebGPU).

- Pros: significantly fewer translation layers, resource lifecycles are explicit (no guessing via memory observation, no GPU MMU re-creation), NVN, like WebGPU, is a modern explicit API → better browser performance.
- Cons: **semantics must be reverse-engineered** (no documentation, parameter structures opaque); scope depends on how many of the several hundred NVN functions LGP uses; more game-specific and not pre-tested by emulators.

### B: LLE at the Maxwell command level (like Ryujinx/yuzu)

We recompile the NVN implementation too and emulate `nvdrv`, GPFIFO and the Maxwell 3D/compute/DMA classes.

- Pros: well understood thanks to two emulators, Ryujinx (MIT) as reference; game-independent; NVN semantics need not be understood.
- Cons: much more runtime work (command parsing, texture cache over guest memory, synchronization via syncpoints), hard to make performant in the browser; a lot of code.

### Shaders: offline vs. runtime

- **Offline** (a tool translates all shaders from the local RomFS in advance, the result stays local): no stutter, errors visible early, WGSL validation in advance.
- **Runtime**: robust against unknown/dynamic shaders, but translation + pipeline creation in the browser is expensive.

## Recommendation

**A (NVN HLE)** plus **offline shader translation with a runtime fallback and a local cache**. Two of three gate criteria are met (bootstrap loader, 230 < 300); the third becomes an abort criterion:

- **HLE boundary:** `nvnBootstrapLoader` or `nvnDeviceGetProcAddress` is replaced and returns our own implementations for all 498 names; the 268 unused ones return a stub that aborts loudly with the function name. That way the NVN driver from `sdk` never runs, and `nvdrv`/`vi` binder are not needed for graphics (confirmation in P4.1).
- **Abort criterion P4.1:** if, up to "clear color visible", re-creating `Device`, `Queue`, `Window`, `MemoryPool`, `Texture`/`TextureBuilder` and `CommandBuffer` recording does not succeed (in particular object sizes and handle values), B is re-evaluated with Ryujinx as reference before investing in P4.2 ff.
- **Shaders:** offline from the local RomFS (≈ 2,450 stages plus compute), the result stays local (OPFS/`$LGP_PRIVATE_DIR`); runtime translation for unknown shaders with a cache. No GLSL path.
- **Textures:** deswizzle offline, ASTC → BC7 (desktop); BCn directly via `texture-compression-bc`.
- **GPU memory changes** (risk 4): the method is chosen in P4.1; the starting point is "upload ranges referenced per draw, with hash comparison", because it needs no recompiler intervention.

Shader translator: own IR, structure modeled on Ryujinx (MIT), WGSL via our own emitter or SPIR-V → WGSL (Tint/Naga). yuzu code only after approval.

Honest note: next to runtime/HLE, graphics is the largest single item (P4: several XL steps). This applies to both options.

## Open questions / data needed

- ~~List of NVN functions, number of shaders, storage location (P1.4)~~ → answered (`03-grafik.md`, appendix A).
- ~~Runtime shaders?~~ → no glslc; 27 shaders of unknown origin (P4.2 clarifies).
- ~~Texture formats~~ → 94.5 % BCn, 271 ASTC.
- **New:** which fixed-function features (depth bounds, conservative raster, wide lines, alpha test) are actually enabled? (argument logging in P4.1)
- **New:** method for detecting CPU writes to GPU memory in the browser (P4.1, with ADR-0001).
- **New:** later parts of the game (not measured) may bring geometry/tessellation stages and further NVN functions.

## Consequences

- `gpu/` gets two parts: front end (NVN HLE or nvdrv LLE) and shader translator; both depend only on `IRenderer`.
- Translated shaders and converted textures are derived game data and stay local.
- `nvdrv` and the binder transactions of `vi` will probably only need stubs for graphics (`vi` itself remains necessary for `OpenDisplay`/`OpenLayer`/`GetNativeHandle`, because the game gets the native window for `nvnWindowBuilderSetNativeWindow` from it); video (`subsdk1`) is replaced anyway according to ADR-0005.

## Sources

- Ryujinx (MIT): <https://emulation.gametechwiki.com/index.php/Ryujinx>
- WebGPU status: <https://web.dev/blog/webgpu-supported-major-browsers>, Firefox 147 Apple Silicon: <https://www.firefox.com/firefox/147.0/releasenotes/>
- Switchbrew `nvdrv`: <https://switchbrew.org/wiki/NV_services>
