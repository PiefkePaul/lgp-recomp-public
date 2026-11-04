# tvOS (Apple TV) as a target for a Switch recompilation

As of 2026-10-14. Applies across games. What follows from it for LGP is in
`docs/analysis/28-tvos-machbarkeit.md`. Verified here means: looked up in source code or
Apple documentation; memory, bundle size and the wgpu build are measured on the Apple TV 4K (3rd gen).

## Building

| Part | Status | Source |
|---|---|---|
| CMake | `CMAKE_SYSTEM_NAME=tvOS` is supported (`Platform/tvOS.cmake`). CMake does **not** set `IOS` with it; anyone who wants to reuse iOS branches needs their own variable (here `LGP_TVOS`, `LGP_APPLE_MOBILE`). | CMake 3.28, `Modules/Platform/` |
| Rust | `aarch64-apple-tvos` and `aarch64-apple-tvos-sim` are **Tier 2 without host tools**: `rustup target add` delivers the `std`, no nightly and no `build-std` needed. `TVOS_DEPLOYMENT_TARGET` sets the minimum version. Older statements of "Tier 3" are outdated. | rustc book, platform page `apple-tvos` |
| wgpu | The Metal backend (`wgpu-hal`) knows tvOS: capabilities are queried with `available!(…, tvos = …)`. wgpu-native has **no** tvOS release; building from source needs `BINDGEN_EXTRA_CLANG_ARGS="-isysroot <AppleTVOS-SDK> --target=arm64-apple-tvos"` for bindgen, because `build.rs` sets the SDK itself only for iOS and macOS. In addition, `rust-toolchain.toml` of v29.0.1.1 pins Rust 1.93, which has no prebuilt tvOS `std` yet (E0463); with `RUSTUP_TOOLCHAIN=stable` it builds (verified on the Mac, 2026-10-14: `libwgpu_native.a` 32.9 MB). Moreover, wgpu-native enables the Metal surface only with `any(target_os = "ios", target_os = "macos")`; on tvOS `wgpuInstanceCreateSurface` otherwise aborts with "Unsupported Surface" (Apple TV, 2026-10-14). The script `tools/apple/build_wgpu_native.sh tvos` adds `tvos` to these cfgs (upstream proposal open). | wgpu-native v29.0.1.1 `build.rs`, `wgpu-hal/src/metal/adapter.rs` |
| SDL3 | tvOS is a platform of its own (`SDL_PLATFORM_TVOS`). **Caution:** On tvOS `SDL_PLATFORM_IOS` is set as well (both depend on `TARGET_OS_IPHONE`), so always check the tvOS branch first. | SDL3 `SDL_platform_defines.h` |
| Entitlements | `com.apple.developer.kernel.extended-virtual-addressing` exists from tvOS 14. Apple names `increased-memory-limit` only for iOS, iPadOS and visionOS. A profile with an entitlement the platform does not know cannot be signed (suspicion, not tried). | Apple, Bundle Resources › Entitlements |
| Device family | `TARGETED_DEVICE_FAMILY=3`. Info.plist without orientations, without `UIFileSharingEnabled`. | Xcode templates |

## Memory

- Apple publishes no memory limit for tvOS apps. Measured on the Apple TV 4K (3rd gen, AppleTV14,1, 4 GB,
  2026-10-14, `tools/memprobe` without entitlements): 3976 MiB physical, 2092 MiB available at start, jetsam at
  ≈ 2057 MiB footprint. That is ≈ 52 % of the RAM (iPad Pro M1: ≈ 64 %, `speicherprobe-mobil.md`).
- CPU and GPU share the memory. Textures and buffers count toward the same footprint.
- Without an entitlement, 3456 MiB could be reserved virtually in one piece; 8 GiB and more fail.
- With `extended-virtual-addressing` the memory probe was terminated after ≈ 30 s by signal 9, without a log, crash or
  jetsam report (twice). Cause open (suspicion: entitlement rejected on tvOS, or a watchdog). Until that is cleared
  up, build tvOS apps without this entitlement (LGP: default, `-DLGP_TVOS_EXTENDED_VA=ON` only for testing).
- Consequence: A recompilation that needs more than ≈ 1.9 GiB of footprint does not run on the Apple TV.
- Memory warnings (`didReceiveMemoryWarning`, in SDL `SDL_EVENT_LOW_MEMORY`) come before jetsam, but without any
  promise of how much time is left. LGP counts them (`lgp::platform::note_low_memory`, line `host: low memory warning N:
  footprint …, available …` on stderr) and at the next command list empties the geometry cache and the free
  buffers of the buffer pool (line `nvn webgpu: low memory warning …`, test in `nvn.webgpu_draw`). Both fill up
  again. Whether this prevents a jetsam on the Apple TV is not measured. On Android, `SDLActivity.java`
  (SDL 3.4) calls `nativeLowMemory` on every `onTrimMemory`. SDL 3, however, never puts `SDL_EVENT_LOW_MEMORY` into the
  event queue (`SDL_SendAppEvent`); only event watches see it, so LGP counts it in the window's watch.
  Before that, no warning arrived on the Shield (2026-10-28/29: `onTrimMemory` in logcat, no line in the game).
  The system rejects `am send-trim-memory` as soon as the app already has a higher level ("Unable to set a higher trim
  level than current level"), and in the foreground also the background levels.
- `increased-memory-limit` cannot be signed for tvOS: Xcode silently leaves it out (missing in `.xcent`
  and the profile), the limit stays (Apple TV 4K, tvOS 27.2). Zero pages count just like random data.
- Way out, confirmed with the memory probe (tvOS 27.2): Darwin counts file-backed pages (`MAP_SHARED` on a file)
  as "external", not in the footprint. 3456 MiB of random data in such a block kept the footprint at
  7–15 MiB; after that ≈ 2 GiB more went anonymously. The guest block can be put on a deleted, sparse file with `LGP_HOST_BLOCK_FILE=1|<Ordner>`
  (`host_block.hpp`; default on tvOS, `LGP_HOST_BLOCK_FILE=0` switches it off; probe for it: memory probe `file`).
  With LGP the Apple TV 4K ran up to Route 1 this way (footprint peak 1813 MiB instead of jetsam at ≈ 1655 frames, Analysis 28).
- Price of the file backing: what the game writes into its memory, the system writes back to flash.
  Reported as `diskwrites_resource`: 1074 MB in 122 s, 4295 MB in 46 min (Analysis 28). `host_block.cpp` does not call
  `msync`; another folder or `F_NOCACHE` change nothing about `mmap`. The file is deleted immediately and lies, with
  `LGP_HOST_BLOCK_FILE=1` (default on tvOS), in `$TMPDIR`, otherwise in the given folder; it is not backed up. Releases punch holes (`F_PUNCHHOLE`) instead of zeroing pages. Whether that works on
  the device is told by the start line `host block: …, released pages punched|zeroed (written back)`; with
  "zeroed" every release writes the pages once more first.
  Writing less is only possible if part stays anonymous. For that one sets
  `LGP_HOST_BLOCK_FILE_RANGE=A:B` (MiB offsets in the guest block, open ends allowed, e.g. `1024:`): then only
  this window is file-backed, the rest is anonymous and counts toward the footprint again. Which window pays off is shown by
  `LGP_HOST_BLOCK_REGIONS=<MiB>` with `--time-stats` (only with lifecycle control, i.e. `--renderer window`/app).
  The line `guest regions` shows offset@guest address=resident/dirty in MiB per section. Dirty means: on Darwin
  `MINCORE_MODIFIED` according to `mincore`, i.e. not yet written back. That only exists in the file window. Anonymous
  pages always count as modified after the first write and do not have to go anywhere, which is why outside the
  window it says 0. So to choose the window, first run with the whole file (`LGP_HOST_BLOCK_FILE_RANGE=0:`, necessary on tvOS,
  because otherwise the default `768:` applies there).
  Sections that are permanently dirty there are rewritten often by the game and are candidates for anonymous; large,
  clean sections stay on the file. This is a snapshot, not a write rate (suspicion). A value that
  cannot be read leaves the whole file in place and reports that on stderr.
  On tvOS `768:` is the default (`host_block.cpp`, also in the memory probe; CMake `LGP_HOST_BLOCK_FILE_RANGE_DEFAULT`
  overrides, the environment wins, `0:` puts
  the whole block into the file). With the whole block in the file, frames stood still for 13–28 s in loading phases, because the
  system created and wrote back the file's pages written for the first time. With the first GiB anonymous (`1024:`)
  there was no stall, but in one of two runs only 90 MiB `available` were left. With `768:` at least
  438 MiB remained; in one of two runs there was a stall of 12.7 s at the first load (Analysis 42, 2026-10-21).
  An earlier regions run (Apple TV, `REGIONS=128`, whole block in the file) found only
  ≈ 100–200 MiB dirty at any point in time. The cost of the whole file only showed up as stalls in the loading phases (Analysis 42),
  not in the amount.
- The Apple TV 4K gives the window 3840×2160 pixels. A swapchain of that size costs three images of ≈ 32 MiB each,
  although the game (handheld) draws 720p; therefore the SDL window limits the surface on tvOS to 1440
  lines (`LGP_SURFACE_MAX_HEIGHT`), and the layer scales to the TV. 1440 is exactly 2 × 720, so the
  nearest scaling of the blit stays even; at 1080 (factor 1.5) every second line would be doubled. tvOS limits the geometry cache to
  64 MiB (−100 MiB footprint measured).
- Measurement runs with `--clock virtual` run with `--sched baton`: only one guest thread at a time, so the frame rate is far
  below the real one (Apple TV, quiet scene: 22 instead of ≈ 29.5 fps; outdoors with the host clock 22–28 fps).

## Storage

According to reports from 2026, the rules from Apple's archived tvOS guide still apply (suspicion until verified
on the device; a third-party source measured it on an Apple TV 4K with tvOS 26.5):

- `Documents` exists but accepts no files. `Library/Application Support` does not exist and cannot be
  created. Only `Library/Caches` and `tmp` are writable.
- The system may empty `Library/Caches` when space gets short and the app is not running.
- Only `NSUserDefaults` (about 500 KB) and iCloud are permanent: key-value store (1 MB in total) or CloudKit
  (records and assets in the user's private container, needs the iCloud capability in the profile).
- The tvOS simulator allows write accesses that the device rejects. Check storage only on the device.
- There is no Files app. Transfer from the Mac works with
  `xcrun devicectl device copy to --domain-type appDataContainer --domain-identifier <bundle-id> --destination Library/Caches/…`
  (verified on iOS, suspicion for tvOS), otherwise only over the network.
- The system does not delete the app bundle itself. Apple names 4 GB as the maximum size of a tvOS bundle; this does not
  apply to local installations: a test app with 4.5 GiB could be installed via `devicectl` (143 s, a
  reinstallation after a small change 15 s, i.e. only the delta; Apple TV 4K, 2026-10-14).

Consequence for every recompilation: game data belongs either in the bundle (permanent, but every installation brings it
along and the 4 GB limit can apply) or in the cache (transferred again after an emptying). Saves must
not lie only in the cache if they are not to be lost.
- Permanent storage on tvOS (research 2026-10-14): iCloud Drive/ubiquity containers do not exist
  (`ubiquityIdentityToken` is always nil, QA1935), `NSUbiquitousKeyValueStore` has 1 MB in total, `NSUserDefaults`
  warns from 512 KB and terminates the app from 1 MB. For larger saves CloudKit remains (private container, record up to
  1 MB without assets, the data as `CKAsset`); it needs a paid team, and without an iCloud account there is no access
  (`accountStatus`). Development builds write into the container's development environment.
  The first write access creates the record type only in Development; before TestFlight or the App Store the
  schema has to be deployed to Production in the CloudKit Dashboard ("Deploy Schema Changes"), otherwise every save
  fails there. LGP: type `SaveBlob` with `seq` (Int64) and `data` (Asset).
  LGP signs `lgp_game` for tvOS with iCloud by default; a team without the iCloud capability builds with `-DLGP_TVOS_CLOUD_SAVES=OFF`.

## Input

- **Siri Remote:** SDL presents it as a gamepad (`SDL_HINT_TV_REMOTE_AS_JOYSTICK`, on by default). According to the SDL sources
  (`SDL_mfijoystick.m`) the touch or click surface is the left stick, click or center A, Play/Pause X and
  Back/Menu B. Back only comes as a short pulse via `controllerPausedHandler` (250 ms), so it cannot be
  held. It has no Start, no right stick and no shoulder buttons. That is enough for menus, not for a game with a
  stick and many buttons. The remote's motion sensors are no replacement for a Joy-Con's gyro.
- **Remote and controller at the same time:** The remote is already connected at start and would get slot 0 without a
  special case; a controller would then be player 2. `SdlInput` (`SdlInputOptions::tv_remotes`, on for tvOS)
  gives a gamepad without a right stick and without shoulder buttons no slot of its own (SL/SR of a single
  Joy-Con, which SDL reports as paddles, count as shoulder buttons). Its buttons and its stick go to slot 0 (test `platform.sdl_input`, `test_siri_remote_and_dpad_pad`).
- **Controllers:** MFi, Xbox, PlayStation and, since tvOS 16, Nintendo Joy-Con and Switch Pro Controller via the
  GameController framework; SDL3 maps them as normal gamepads. For Nintendo controllers SDL swaps A/B and
  X/Y according to the labels (`has_nintendo_buttons`). `SdlInput` maps by label, and the app menus then swap
  confirm and cancel (`ConfigNavSwapGamepadButtons`). Info.plist: `GCSupportedGameControllers`
  (ExtendedGamepad, MicroGamepad) and `GCSupportsControllerUserInteraction`.
- **Pause without Home:** A controller's Home button stays with the system (suspicion). On tvOS, therefore,
  Start held for 0.7 s opens the app's pause, and for a gamepad without Start (Siri Remote) X (Play/Pause)
  (`AppMenu::set_start_hold_pause`, test `platform.app_menu`). A short press stays Plus or X for the game.
  In the menus, Back, Start and the button labeled B go back one page.
- **No touch screen:** Touch overlays must be off.
- **Focus engine:** UIKit interfaces on tvOS are operated via focus, not by tapping. An own
  interface in the game image (via controller events) does not need the focus engine.
- **Text input:** Whether SDL's `UITextField` path shows the system keyboard on tvOS is not verified.

## Graphics

- Apple TV 4K (3rd gen): A15, Metal GPU family Apple 8 (suspicion from the chip, not queried). ASTC in hardware,
  BC not (BC exists on Apple GPUs from M1, or with wgpu 30 on iOS from 16.4, see `wgpu-native-upstream-stand.md`).
  BC textures therefore need a CPU decoder as on the iPad.
- No MetalFX on Apple TV (Apple's feature set tables).
- Frames in flight: without a limit the driver stacks submitted command buffers as its own memory (IOAccelerator,
  not in `metal allocated`), hence default 3 on the Apple TV (`LGP_FRAMES_IN_FLIGHT_APPLE_TV`). To measure which
  limit gives the most fps: `tools/apple/measure_tvos_fif.sh <Gerät> <Spielstand> [<Eingabeskript>]` (host clock,
  order 2 3 4 4 3 2, each run with a fresh copy of the same save, only comparable with HDMI), evaluation
  `tools/analyze/perf_run_report.py --group …`; logs to `$LGP_PRIVATE_DIR`.

## Distribution

As with iOS: sign only with one's own Apple team, install only on one's own devices. The Apple TV must be paired
in Xcode (Devices and Simulators; on the Apple TV "Fernbedienungen und Geräte › Remote App and Devices" (Remotes and
Devices › Remote App and Devices)) and have developer mode switched on. `devicectl` then starts apps as with the iPad. A new Apple TV has to be registered in the team once;
`xcodebuild` with `-destination id=<Apple-TV-ID> -allowProvisioningUpdates -allowProvisioningDeviceRegistration`
does that (verified on the Apple TV 4K AppleTV14,1, 2026-10-14). UIKit on tvOS lacks some iOS properties
(`UIColor.systemBackgroundColor`, `UITextView.editable`); such code needs `#if TARGET_OS_TV`.

## Sources

- Rust: [platform page apple-tvos](https://doc.rust-lang.org/rustc/platform-support/apple-tvos.html)
- Apple: [Extended Virtual Addressing Entitlement](https://developer.apple.com/documentation/bundleresources/entitlements/com.apple.developer.kernel.extended-virtual-addressing),
  [Increased Memory Limit](https://developer.apple.com/documentation/bundleresources/entitlements/com.apple.developer.kernel.increased-memory-limit)
- Storage measured on the device (third-party source): [path_provider_tvos](https://pub.dev/documentation/path_provider_tvos/latest/)
- Storage in general: [Apple Developer Forums, "tvOS: Temporary storage?"](https://developer.apple.com/forums/thread/19002)
- wgpu-native [`build.rs`](https://github.com/gfx-rs/wgpu-native/blob/v29.0.1.1/build.rs)
