# What shader modules and pipelines in wgpu hold in host memory

Applies to wgpu-native 29.0.1.1. Its `Cargo.lock` pulls in wgpu-core/wgpu-hal/naga 29.0.3; the line numbers below are
from 29.0.1, the affected places are the same in 29.0.3. The question was whether a port can release the shader modules
after building its pipelines to save memory (memory list #216, item 6). The statements about the source code have been
read; the numbers come from `lgp_wgsl_memprobe` (below). What is only inferred is marked as suspicion.

## From the source code (read)

- **Every pipeline holds on to its modules.** `wgpu-core/src/pipeline.rs`: `RenderPipeline::_shader_modules`
  (`ArrayVec<Arc<ShaderModule>>`, line 811) and `ComputePipeline::_shader_module` (line 279). A
  `wgpuShaderModuleRelease` therefore only releases the caller's handle. The module itself lives as long as the last
  pipeline that uses it.
- **Metal keeps the whole naga IR.** `wgpu-hal/src/metal/device.rs` `create_shader_module` only stores
  `ShaderModuleSource::Naga(NagaShader)`, i.e. `naga::Module` and `ModuleInfo`. MSL is only written when the pipeline is built
  (`load_shader`: `process_overrides`, `msl::write_string`, `newLibraryWithSource`), and anew for every
  pipeline, even for an already translated module with different override values.
- **Every Metal pipeline keeps its `MTLLibrary`.** `PipelineStageInfo::library` (`metal/mod.rs` line 900, with
  `#[allow(dead_code)]`) stays attached to the pipeline per stage. With N variants of a program that is N libraries with
  the same code. Whether Metal shares them internally is unverified (suspicion: no, because the source contains its own
  constants per variant).
- **Vulkan keeps the IR only with overrides.** `wgpu-hal/src/vulkan/device.rs` (line 1730): modules without
  `override` constants become SPIR-V immediately and drop the IR. Modules with overrides stay
  `ShaderModule::Intermediate` with the naga IR. The vertex and fragment WGSL of our translator always declares
  overrides (`lgp_flip_y`, `lgp_depth_minus_w`, `lgp_alpha_func`), so it keeps the IR.
- **The analysis (`ModuleInfo`) stays in the module too.** `naga::valid::ModuleInfo` contains per function a
  `FunctionInfo` with one `ExpressionInfo` per expression (`naga/src/valid/analyzer.rs` line 1245,
  `vec![ExpressionInfo::new(); fun.expressions.len()]`). That is the `Vec::from_elem` under `validate_function` that
  `malloc_history` on the Mac shows as the largest item. Metal needs this analysis when building pipelines
  (`process_overrides`, `msl::write_string`, memory sizes of the workgroup variables), which is why `NagaShader` keeps it.
- **Validation cannot be turned off.** `wgpu-core/src/device/resource.rs` `create_shader_module` always calls
  `create_validator(…, ValidationFlags::all()).validate(&module)`; the result is exactly that `ModuleInfo` which
  the Metal translator needs. `wgpuDeviceCreateShaderModuleTrusted` only changes the run-time checks in the generated
  code (`ShaderRuntimeChecks`), not the validation. The only path without naga is
  `create_shader_module_passthrough` (feature `PASSTHROUGH_SHADERS`, ready-made MSL or metallib): such modules have
  no `interface`, hence no auto layout and no override constants. For our renderer that would mean generating MSL ourselves for every
  override combination. That is not a small step.
- With `InstanceFlags::DEBUG`, wgpu-core additionally attaches the WGSL source to the module (`debug_source`). The
  boot program sets `WGPUInstanceFlag_Default`, so it does not.

## Measured (cloud, lavapipe, test shaders, 2026-10-14)

`lgp_wgsl_memprobe` with the 8 vertex and compute test shaders from `tests/gpu/shaders` (0.1 MiB WGSL), 4 variants per
program:

| Phase | Heap |
|---|---|
| Modules created | +1.2 MiB, about 20 bytes per byte of WGSL |
| 32 pipelines | +34 MiB, about 1 MiB per pipeline (lavapipe compiles with LLVM, not transferable to Metal) |
| Module handles released | ±0, as expected from the source code |
| Pipelines released | back to the start |

The test shaders are small. Only the ratio "bytes of IR per byte of WGSL" carries over to the game, and even that only as an
estimate: the IR of large shaders may grow differently.

## Measured (Mac M1, Metal, the game's WGSL, 2026-10-14)

`lgp_wgsl_memprobe` (Release, `f770802`) over the game's WGSL dump: 296 programs, 572 modules, 10.9 MiB WGSL,
592 pipelines, 0 errors.

This version of the probe built programs whose fragment shader outputs no color (only depth or `discard`) without a
fragment stage. The renderer attaches it even then. For these programs the pipeline numbers below are therefore somewhat too
low. How many of the 296 programs this affects has not been counted. The module numbers and the comparison with and without the
patch are not affected, because both runs used the same probe. The probe now follows the renderer in this respect.

| Phase | Heap | footprint |
|---|---|---|
| Start | 14.0 MiB | 19.5 MiB |
| Modules created | 277.8 MiB (+264) | 273.5 MiB |
| Pipelines built | 303.0 MiB | 305.2 MiB |
| Module handles released | unchanged | unchanged |
| everything released | 14.4 MiB | 106.7 MiB |

That is 24.1 bytes of heap per byte of WGSL for the modules and 43.7 KiB per pipeline. The modules (IR + `ModuleInfo`) are
thus the largest item; the `MTLLibrary` per pipeline is small. In the game (frame 8500) `malloc_history` showed about 175 MiB for the
naga parts, because not all programs were loaded there. The 87 MiB of footprint that remain after releasing
belong to Metal (suspicion: translator and driver caches), not to the heap.

## Patch: modules keep only their WGSL text (Metal, Vulkan)

`tools/wgpu/lazy-naga-ir.patch` (against wgpu 29.0.3) changes three places:

- Under Metal and Vulkan, wgpu-core always passes the WGSL text on to wgpu-hal as `debug_source` (otherwise only with
  `InstanceFlags::DEBUG`).
- wgpu-hal then stores only this text instead of `NagaShader`: Metal always (`ShaderModuleSource::Wgsl`), Vulkan for the
  modules it would otherwise keep as IR (`ShaderModule::Wgsl`, i.e. modules with overrides). IR and `ModuleInfo` are dropped
  when the module is created. Vulkan modules without overrides become SPIR-V immediately as before; without `DEBUG`
  the text does not end up as debug info in the SPIR-V.
- When building the pipeline, wgpu-hal re-parses and validates the text (`reparse_wgsl`: `naga::front::wgsl::parse_str`,
  validator with all capabilities; wgpu-core already checked the module with the device's capabilities) and discards
  the result afterwards.

Run-time switch: `LGP_WGPU_LAZY_IR=0` keeps the old behavior (for A/B measurements). GL and DX12 do not
change.

Measured in the cloud (lavapipe/Vulkan, test shaders as above, 4 variants, ABAB): modules +1.2 MiB (20.5 bytes per byte of
WGSL) without the patch vs. +0.1 MiB (1.9 bytes per byte of WGSL) with the patch. The time for 32 pipelines varied
(1698–2082 ms vs. 1710–1818 ms) and showed no measurable difference, because lavapipe's LLVM compilation
dominates here. With smaller shaders, re-parsing costs measurable time (below: about 0.5 ms or +30 % per pipeline).
With the patched wgpu-native, all 30 tests of `ctest -L "webgpu|nvn"` (image comparisons
included) ran green in the cloud.

Measured on the Mac (M1, Metal, the game's WGSL as above, 572 modules, 592 pipelines, ABAB with `LGP_WGPU_LAZY_IR=0`
vs. default, 2026-10-14):

| | without patch | with patch |
|---|---|---|
| Heap after the modules | 278 MiB (+264) | 28.8 MiB (+14) |
| Heap per byte of WGSL | 24.1 bytes | 1.3 bytes |
| footprint after the pipelines | 305 MiB | 71 MiB (−235) |
| Pipeline build, 592 pipelines | 814 ms | about 2.7 s (about +3.2 ms per pipeline) |

Both times with a warm Metal translator cache; the first run with a cold cache (31 s) is not counted. The memory
gain is the largest lever found so far for tvOS. The price hits every pipeline build that the
pipeline cache does not catch, i.e. mainly scene changes. The patch will therefore only become the default once `lgp_game`
on the Apple TV (footprint and fps across a scene change) shows that it is worth it.

In the game (Mac M1, `lgp_game` native, vpress up to frame 3000, `--pipeline-cache none`, Metal cache warm, ABAB; A without,
B with patch, 2026-10-14):

| Run | footprint at 3000 | Peak | Draw pipelines Σ | fps median / minimum |
|---|---|---|---|---|
| A1 | 1100.0 MiB | 1146.7 MiB | 229.7 ms | 57.7 / 28.0 |
| B1 | 980.0 MiB | 1003.3 MiB | 409.8 ms | 57.7 / 29.4 |
| A2 | 1100.0 MiB | 1112.5 MiB | 231.5 ms | 57.7 / 31.6 |
| B2 | 871.1 MiB | 882.7 MiB | 406.2 ms | 57.2 / 29.9 |

In the game the patch saves 120 to 230 MiB of footprint and costs about 175 ms more pipeline time up to frame 3000; the fps
stay the same, and the frames are bit-identical. Why B1 and B2 differ by a good 100 MiB has not been explained
(suspicion: the point in time at which Metal or malloc return memory; A1 and A2 are the same).

**Apple TV 4K (A15, tvOS 27.2, 2026-10-14), ABAB up to frame 10500 with scene changes, without HDMI (memory only, fps
not usable), all arms `LGP_WEBGPU_GEOMETRY_FP=1`:**

| Run | Lazy IR | End | footprint peak | smallest `available` |
|---|---|---|---|---|
| A1 | off | kill at ~8300 | 1881 MiB | – |
| B1 | on | 10500 | 1715 MiB | 463 MiB |
| A2 | off | kill at ~8400 | 2073 MiB | 25 MiB |
| B2 | on | 10500 | 1614 MiB | 600 MiB |

Without the patch, jetsam kills the game at the scene at ~8400 (A2 shows the limit directly: 25 MiB left). With the patch
the peak is 260–460 MiB lower and the run goes through. That is why `tools/apple/build_wgpu_native.sh tvos` builds in the
patch by default. The graphics spike at the same scene (+~450 MiB ledger graphics) remains in all arms; it lies
below our counters (suspicion: Metal command buffer memory, `docs/analysis/28-tvos-machbarkeit.md`).

Building: one script for all patches, `tools/webgpu/build_wgpu_native.py`. It checks out the pinned tag, applies
the chosen patches and stores the result like a release. The patch here is called `lazy-naga-ir`. With
`--android` it is in the script's default (`driver-cache,lazy-naga-ir`; Shield −615 MiB native heap,
`docs/analysis/36-android-native-heap.md`, section 6), on the host it is not (only `driver-cache`); for desktop and iOS
a measurement that would justify the default is missing:

```sh
python3 tools/webgpu/build_wgpu_native.py --out $OUT --patches driver-cache,lazy-naga-ir            # Host (Mac, Linux, Windows)
ANDROID_NDK_HOME=… python3 tools/webgpu/build_wgpu_native.py --out $OUT --android --patches driver-cache,lazy-naga-ir
cmake --preset release -DLGP_WGPU_NATIVE_DIR=$OUT
```

For iOS/tvOS `tools/apple/build_wgpu_native.sh ios|tvos` (built on the Mac for tvOS); under tvOS `lazy-naga-ir` is the
default (see below, `LGP_WGPU_NATIVE_LAZY_IR=0` leaves it out), under iOS only with `LGP_WGPU_NATIVE_LAZY_IR=1`. `tools/wgpu/patch_wgpu_native.sh <checkout>` only patches an existing checkout (clones wgpu 29.0.3 to
`<checkout>-wgpu` and adds `[patch.crates-io]`), `tools/android/build_wgpu_native.sh <checkout> <out>` builds for Android from an
existing checkout (patches via `LGP_WGPU_NATIVE_PATCHES`, default `driver-cache,lazy-naga-ir`); both call the
Python script. The host build uses the default features like the release archive, so that only the patches
differ. For Android, bindgen needs `--sysroot` and a target with an API level (`--target=aarch64-linux-android29`),
otherwise the NDK headers reject the target. `driver-cache` and `lazy-naga-ir` together have been built in the cloud for Linux
and run with the tests `-L "nvn|webgpu"`.

**Same SPIR-V (checked on test shaders):** a small Rust program against naga 29.0.3 compared, for the WGSL output of the
WebGPU and NVN tests (60 files, `LGP_WEBGPU_DUMP_DIR` during ctest, 4 override variants per entry point as in the
backend), the SPIR-V from the module with device-like capabilities with that from the re-parsed module with all
capabilities: 240 of 240 identical word for word. Pipeline cache and driver cache (#171) therefore still
hit. This has not been checked for the game shaders (suspicion: identical, because `ModuleInfo` does not depend on the capabilities).

**Time per pipeline (lavapipe, same WGSL output):** 35 programs, 61 modules, 0.1 MiB, 140 pipelines with 4 variants
(9 compute pipelines fail in both runs on the tool's workgroup limit), 10 runs ABAB. The heap of the modules
drops from +2.5 to +0.3 MiB. The pipelines take a median of 210 ms without vs. 277 ms with the patch, i.e. about 0.5 ms
more per pipeline (+30 %). With the 8 test shaders above, lavapipe's LLVM compilation hides this; here the
shaders are smaller and the pipelines cheaper. Conclusion for Android (suspicion): game shaders are on average about four times
as large as these, the phone is slower, under Android only one pipeline thread builds, and with a warm driver cache
(#171) what remains of the pipeline build is mainly naga. Whether the memory gain is worth the time will only be shown by an ABAB on the
device with `--time-stats` (pipeline worker time) and `dumpsys meminfo`.

## Conclusions

- Releasing modules after building the pipelines saves nothing as long as the pipelines live. The backend keeps its
  pipelines (cache per program and state), and thus also the IR of all programs.
- What would actually save memory (all suspicion; the size can only be estimated after measuring the game):
  1. **Release programs that have not drawn for a long time, together with their pipelines** (LRU by time or frames). The
     rebuild costs compilation time on next use; the on-disk pipeline cache shortens that. This
     touches `webgpu_backend.cpp` and needs to be coordinated with the WebGPU thread.
  2. **Do not keep IR and `ModuleInfo` in the module** (patch above). According to the Mac measurement the largest lever.
  3. **wgpu-hal Metal: release the `MTLLibrary` after building the pipeline.** According to the measurement at most about 44 KiB per pipeline
     (together with the rest of the pipeline), so secondary. For measuring: `tools/wgpu/metal-drop-library.patch`, enabled with
     `LGP_WGPU_METAL_DROP_LIBRARY=1`; what else a Metal pipeline holds on to is described in
     `docs/analysis/32-metal-speicher-je-pipeline.md`.
  4. **Fewer variants per program** (override combinations), because under Metal every variant gets its own MSL and its
     own `MTLLibrary`.

## Measuring on the game

```sh
# 1. Dump the game's WGSL once (translated game shaders: only to $LGP_PRIVATE_DIR)
LGP_WEBGPU_DUMP_DIR=$LGP_PRIVATE_DIR/wgsl-dump lgp_game … --frames 3000
# 2. Memory per module and pipeline (release build, otherwise the debug heap is counted too)
build/release/bench/webgpu/lgp_wgsl_memprobe $LGP_PRIVATE_DIR/wgsl-dump --variants 2
```

The output shows the heap and, under Darwin, the footprint after each phase. It contains only numbers and may be
shared. The ctest `bench.wgsl_memprobe` runs with two built-in shaders and only checks that the tool runs.
Limits of the tool: pipelines are created with auto layout, RGBA8 targets and vertex buffers in format `*x4`. A
program that does not build this way counts as "rejected" and is missing from the pipeline count and the per-pipeline value (under
wgpu-native; in the browser the answer only comes later, there every pipeline counts). If a program has a fragment shader
without color output, the pipeline gets a depth target as in the renderer and keeps the fragment stage.
