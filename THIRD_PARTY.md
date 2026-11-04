# Third-party code and references

Every piece of code taken over from another project (copied, adapted or
closely ported) gets a row here **in the same PR** that adds it. Pure
inspiration from reading a project does not need a row, but should be named
in the relevant ADR or `docs/analysis/` note.

Rules (see `docs/project-setup/02-projekt-anweisungen.md`):
- Keep the original license header in copied files.
- Copyleft code (GPL/LGPL/AGPL/MPL) needs Paul's approval before it lands in
  any area that might be shared later. Note the approval (PR link) in the table.
- Never list or include game code, SDK binaries or anything derived from them.

| Component | Upstream (URL, commit/tag) | License | Used in (path) | What was taken | Approval |
|---|---|---|---|---|---|
| envytools `envydis/gm107.c` | https://github.com/envytools/envytools (master, 2026-10) | MIT | `gpu/shader/src/maxwell.cpp`, `tests/gpu/maxwell_vectors.txt` | Opcode values/masks and bit positions of the Maxwell instructions (facts, re-typed, no code copied); disassembly syntax mirrored so envydis can serve as test oracle; the test vectors are envydis output (`tools/shader/gen_maxwell_vectors.py`) | not needed (MIT) |
| Mesa NAK (`src/nouveau/compiler/nak/sm50.rs`, `nak_private.h`) | https://gitlab.freedesktop.org/mesa/mesa (main, 2026-10) | MIT | `gpu/shader/src/lift.cpp`, `wgsl.cpp` | Attribute addresses, fragment-output register convention, encoding cross-check (facts, no code copied) | not needed (MIT) |
| NVIDIA open-gpu-doc `classes/3d/cla097sph.h` | https://github.com/NVIDIA/open-gpu-doc | MIT | `gpu/shader/src/sph.cpp` | SPH bit layout (facts, no code copied) | not needed (MIT) |
| ARM astcenc 5.3.0 | https://github.com/ARM-software/astc-encoder (tag 5.3.0) | Apache-2.0 | `tools/texture/astc_vectors_gen.cpp` (links against a local checkout, not part of the build), `tests/gpu/texture/astc_vectors.txt` | Test oracle only: the vectors are astcenc output (encoded own synthetic images, decoded hashes). The decoder in `gpu/texture/` is written from the Khronos Data Format Spec 1.3, no astcenc code | not needed (Apache-2.0) |
| uam-nvn `source/nvn_control.h` | https://github.com/nvnprogram/uam-nvn (f4470ae) | zlib | `gpu/nvn/include/lgp/gpu/nvn/abi.hpp` (`kNvnControl*`), `tools/analyze/bnsh.py` | Offsets of the NVN control section fields (entry offset, stage, compute workgroup/shared/local memory), facts only, no code copied | not needed (zlib) |
| KillzXGaming ShaderLibrary `ControlShader.cs` | https://github.com/cobra838/ShaderLibrary (fbb055c) | MIT | same as above | Cross-check of the same control section offsets (facts, no code copied) | not needed (MIT) |
| bnsh-decoder `BNSH.ksy` | https://github.com/maierfelix/bnsh-decoder (master, 2026-10) | MIT | `tools/analyze/bnsh.py` (`compute` command) | Position of the compute workgroup size in BNSH reflection data (facts, UNVERIFIED), used only to cross-check the control section | not needed (MIT) |
| Dear ImGui 1.92.9 (`imgui*.cpp/.h`, `imstb_*.h`, `backends/imgui_impl_sdl3.*`) | https://github.com/ocornut/imgui (tag v1.92.9) | MIT | `platform/ui/third_party/imgui/` (vendored unchanged, with `LICENSE.txt`) | The UI library of the app's menus (P7); the WebGPU drawing is our own (`webgpu_renderer.cpp`, `draw_ui`) | not needed (MIT) |
| deko3d `source/dk_image.cpp` | https://github.com/devkitPro/deko3d (master, 2026-10) | zlib | `gpu/nvn/src/texture_layout.cpp` | Block-linear rule for 3D textures (block depth, GOB order), facts only, no code copied; not yet verified against NVN | not needed (zlib) |

Ryujinx (MIT) was allowed as reference for the P4 shader spike but was not reachable from the cloud sandbox; the TEXS
register/mask layout follows recollection of its decoder and is marked as such in `docs/knowledge/maxwell-shader-isa.md`.

## Build-time dependencies (not vendored)

| Tool | Purpose | License |
|---|---|---|
| CMake, Ninja | Build system | BSD-3-Clause, Apache-2.0 |
| Python 3 (stdlib only) | Pre-commit guard, test runner | PSF-2.0 |
| Unicorn Engine 2.1.4 (sdist from PyPI, SHA-256 pinned in `tests/difftest/unicorn/CMakeLists.txt`) | ARM64 oracle, linked into test executables only (ADR-0006, P0.8) | GPL-2.0 |
| devkitA64 + libnx (Docker image `devkitpro/devkita64`, digest pinned in CI) | Builds the homebrew fixtures in `tests/fixtures/` (P0.7); libnx, newlib and libgcc end up in the fixture binaries, which are never committed or shipped | libnx ISC, newlib BSD-style, GCC GPL-3.0 with runtime exception |
| envytools `envydis`/`envyas` (built locally, not in CI) | Regenerates the decoder test vectors and assembles the own test shaders in `tests/gpu/shaders/` | MIT |
| M PLUS Rounded 1c Medium (`MPLUSRounded1c-Medium.ttf`, google/fonts commit `ddf386ba`, SHA-256 pinned in `cmake/LgpFreeFont.cmake`) | Downloaded at configure time and embedded unchanged in `lgp_free_font`: stand-in for the console's standard system font when the game folder has no firmware fonts (`docs/knowledge/horizon-pl-shared-font.md`). Copyright and license stay in the font's name table | SIL OFL 1.1 |
| naga-cli 30.0.1 (`cargo install`, cached in CI) | Validates generated WGSL (`gpu.naga_validate`) | MIT / Apache-2.0 |
| Python `wgpu` 0.32.0 (wgpu-native) | Executes generated WGSL and compares pixels (`gpu.render`) | BSD-2-Clause (wgpu-native MIT / Apache-2.0) |
| npm `webgpu` 0.6.2 (Dawn/Tint), Node 22 | Compiles generated WGSL with Tint (`gpu.tint_compile`) | MIT (Dawn BSD-3-Clause) |
| Mesa lavapipe (`mesa-vulkan-drivers`) | Software Vulkan driver for the WebGPU tests in CI | MIT |
| wgpu-native 29.0.1.1 (prebuilt release zips from GitHub, SHA-256 pinned in `cmake/LgpWebGpu.cmake`; optionally built from source by `tools/webgpu/build_wgpu_native.py` with our patch `tools/webgpu/wgpu-native-pipeline-cache.patch`) | `webgpu.h` implementation of the desktop WebGPU renderer (P4.4), linked as shared library (desktop) or static library (iOS, P7.2b) | MIT / Apache-2.0 |
| wgpu 29.0.3 (`wgpu-core`, `wgpu-hal`; only as patches, `tools/wgpu/lazy-naga-ir.patch`, `tools/wgpu/metal-drop-library.patch`) | Metal shader modules keep their WGSL text instead of the naga IR; Metal pipelines can drop their `MTLLibrary` (`LGP_WGPU_METAL_DROP_LIBRARY=1`); applied to a local wgpu checkout by `tools/webgpu/build_wgpu_native.py --patches lazy-naga-ir` (or `tools/wgpu/patch_wgpu_native.sh`) | MIT / Apache-2.0 |
| emdawnwebgpu (Emscripten port `--use-port=emdawnwebgpu`, version pinned by Emscripten 6.0.12) | `webgpu.h` on top of the browser's WebGPU (P4.4) | BSD-3-Clause (Dawn) |
| libopus 1.5.2 (release tarball from github.com/xiph/opus or downloads.xiph.org, SHA-256 pinned in `cmake/LgpOpus.cmake`, built statically) | Opus decoding in `hwopus` (P5.2), linked into `lgp_runtime_hle`; the encoder is used only by the `runtime.hle_hwopus` test | BSD-3-Clause |
| libvpx 1.17.0 (release tarball from storage.googleapis.com/downloads.webmproject.org, SHA-256 pinned in `cmake/LgpVpx.cmake`, VP9 decoder only, built statically with its own configure) | VP9 decoding of in-game videos (`lgp_platform_vpx`, `--video vpx`), default on iOS only (optional on macOS, Linux, Android) | BSD-3-Clause (plus the WebM patent grant in `PATENTS`) |
| zstd 1.5.7 (release tarball from github.com/facebook/zstd, SHA-256 pinned in `cmake/LgpZstd.cmake`, library sources built statically without assembly) | Compressed RomFS images (`romfs.bin` with block index): decompression in `lgp_runtime_hle` (fsp-srv, movie:: HLE), compression in `lgp_romfs_pack --compress` | BSD-3-Clause (dual-licensed BSD/GPL-2.0; used under BSD) |
| Mbed TLS 3.6.7 LTS (release tarball `mbedtls-3.6.7.tar.bz2` from github.com/Mbed-TLS/mbedtls, SHA-256 pinned in `cmake/LgpMbedTls.cmake`, library built statically without programs/tests; not under Emscripten) | TLS for `wss://` to the RyuLDN server (`platform/os/src/lan_tls.cpp`, `ILan::connect_tls`), linked into `lgp_platform` | Apache-2.0 (dual-licensed Apache-2.0/GPL-2.0-or-later; used under Apache-2.0) |
| libogg 1.3.6 and libvorbis 1.3.7 (release tarballs from github.com/xiph or downloads.xiph.org, SHA-256 pinned in `cmake/LgpVorbis.cmake`, decoder sources built statically) | Vorbis sound tracks of in-game videos (movie:: HLE, `--movie-audio`, P5), linked into `lgp_runtime_hle` | BSD-3-Clause |
| FFmpeg libavcodec/libavutil (optional, local only: a "shared" development build named by `LGP_FFMPEG_DIR`, or the system's via pkg-config; not vendored; on Windows the build kit's `build.py fetch-ffmpeg` downloads BtbN's pinned LGPL "shared" build, SHA-256 checked, into the user's tool cache) | VP9/H.264 decoding for in-game videos (`platform/ffmpeg`, movie:: HLE, P5), linked dynamically; the DLLs are copied next to local programs and never committed or shipped by this repo. Chosen by Paul (option A "real playback" of the title video, 2026-10-11) | LGPL-2.1+ (use an LGPL build, not a GPL one) |
| SDL 3.4.16 (release tarball, SHA-256 pinned in `cmake/LgpSdl.cmake`, built statically: video, audio, gamepad/HIDAPI, haptic, sensor) | Desktop window for the WebGPU surface (P4.4), gamepads/keyboard (P5.1), sound output (P5.2) | Zlib |
| Playwright + Chromium (preinstalled, optional) | Browser test `browser.webgpu_demo` (`tests/web/`), not shipped | Apache-2.0 / BSD-3-Clause |
