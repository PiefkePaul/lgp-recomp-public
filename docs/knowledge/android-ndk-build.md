# Android: NDK build and start via adb

Cross-game knowledge (status 2026-10-13, P7.3). Background: [ADR-0008](../decisions/0008-native-apps-auf-allen-geraeten.md),
[`speicherprobe-mobil.md`](speicherprobe-mobil.md). Measurement results for LGP belong in `docs/analysis/`.

## What builds

The whole tree (recompiler libraries, runtime, HLE, NVN HLE, WebGPU renderer, boot program, tests) builds with the
Android NDK for `arm64-v8a` without code changes (checked with NDK r30, cloud container, 2026-10-13; NDK r27c fails on `std::atomic_ref`, which its libc++ does not have):

```sh
export ANDROID_NDK_HOME=<NDK folder>      # Windows: set ANDROID_NDK_HOME=%USERPROFILE%\Tools\android-ndk-r30
cmake --preset android                     # Ninja, Release, API 29, libc++ static, without SDL3
cmake --build --preset android
```

Preset `android`: `ANDROID_ABI=arm64-v8a`, `ANDROID_PLATFORM=android-29`, `ANDROID_STL=c++_static`,
`LGP_ENABLE_SDL=OFF` (SDL3 needs the Java activity on Android; an `adb shell` program has no window).
Unicorn is off on Android (desktop test oracle only). Tests whose test programs are lifted on the build host
(`runtime.kernel_lifted`, `boot.sdk_*`, `nvn.guest_clear`) are missing in the cross build, as under Emscripten.

The game: as on the desktop with `-DLGP_GAME_LIFTED=<lgp_lift --split output>` → `lgp_game`. The lift output is
host-neutral; the same output as for the PC build fits. It stays in `$LGP_PRIVATE_DIR`, and so does the built program
(translated game code, never into the repo, never into CI).

## Tests on the device

In the preset `android`, `tools/android/adb_run.py` is the `CMAKE_CROSSCOMPILING_EMULATOR`: `ctest --preset android`
pushes each test program via `adb push` to `/data/local/tmp/lgp-run` and starts it there; arguments that name a
file or folder on the build host (test data from `tests/`) are pushed along. Environment:
`LGP_ADB` (adb program), `LGP_ADB_SERIAL` (device, like `adb -s`), `LGP_ADB_DIR`. Variables `LGP_*` are passed on to the
device. Tests that are a Python or Node script on the build host (`guard`, `tools`, `gpu.translate` etc.)
keep running on the build host.

## wgpu-native for Android

Contrary to what ADR-0008 assumed, wgpu-native for Android does **not** have to be built from source: the
release v29.0.1.1 contains `wgpu-android-aarch64-release.zip` (same `webgpu.h`/`wgpu.h` as on the desktop,
`libwgpu_native.a` and `.so`; SHA-256 in `cmake/LgpWebGpu.cmake`). On Android, `cmake/LgpWebGpu.cmake` links the
**static** library (plus `libandroid`, `liblog`, `libdl`, `libm`), so that a program via `adb` and later
the app's `libmain.so` need no additional `.so`. Backend on the device: Vulkan (Adreno, Mali);
`LGP_WEBGPU_BACKEND=vulkan` forces it in case wgpu picks the GLES adapter.

The release does not have the patch `driver-cache`; the Vulkan driver then recompiles every pipeline on every start
(on Adreno about 127 ms per pipeline). Therefore an Android build without `LGP_WGPU_NATIVE_DIR` takes
`$LGP_PRIVATE_DIR/toolchains/wgpu-native-android` by default if a build is there
(`tools/android/build_wgpu_native.sh <checkout> $LGP_PRIVATE_DIR/toolchains/wgpu-native-android`). Turn off with
`-DLGP_WGPU_NATIVE_AUTO=OFF`. The line `WebGPU: wgpu-native … from …` at configure time shows which one was
taken. Without the folder and in CI (no `LGP_PRIVATE_DIR`) it stays with the release, with a note at
configure time. The environment only counts at configure time.

The script builds with `driver-cache,lazy-naga-ir` by default: shader modules then only hold their WGSL text instead of
the naga IR; on the Shield −615 MiB native heap and hardly any page faults left in the dip window
(`docs/analysis/36-android-native-heap.md`, section 6; off at run time with `LGP_WGPU_LAZY_IR=0`). If the
patch is missing according to `BUILD-INFO` in the chosen folder, configuring warns; then rebuild the folder.

## Start as a shell program via adb

Before the APK, everything runs as a program under `/data/local/tmp` (shell user, may use Vulkan; suspicion: on
most devices, it is checked on the OnePlus 7 Pro). Without a window: `--renderer webgpu` renders headless,
`--frame-stats`/`--frame-dump` show whether frames are produced.

```sh
adb shell mkdir -p /data/local/tmp/lgp/save
adb push build/android/gpu/webgpu/lgp_webgpu_demo /data/local/tmp/lgp/
adb shell /data/local/tmp/lgp/lgp_webgpu_demo --headless --frames 120   # Vulkan smoke test without game data
```

Game data (only your own dump, stays on your own device): `exefs/` and the RomFS as one image
(`lgp_romfs_pack <romfs> romfs.bin`, one file instead of thousands) to `/data/local/tmp/lgp/game/`.

```sh
adb shell "cd /data/local/tmp/lgp && ./lgp_game --exefs game/exefs --game game --save save \
  --graphics hle --renderer webgpu --audio none --keyboard none --frames 600 --frame-stats 60,300,600"
```

Limitation as with the memory probe: a shell process is not an app, the low-memory killer rates it differently.

## App (APK) with SDL3, without Gradle

Preset `android-app` (inherits from `android`, plus SDL3, without tests): programs with a window become a
**shared library** that SDL's Java activity loads, and a signed debug APK next to it:

```sh
export ANDROID_HOME=<Android SDK>          # with platforms;android-35 and build-tools;35.0.0 (sdkmanager)
cmake --preset android-app [-DLGP_GAME_LIFTED=…]
cmake --build --preset android-app         # gpu/webgpu/lgp_webgpu_demo.apk, runtime/boot/lgp_game.apk
adb install -r build/android-app/gpu/webgpu/lgp_webgpu_demo.apk
```

Structure (checked: the APK builds in the cloud container, start on the device is pending):

- `cmake/LgpAndroidApk.cmake`: `lgp_add_sdl_program()` turns a program into `lib<target>.so`; its `main()` is then
  called `lgp_program_main` (`LGP_MAIN_NAME`), and `platform/android/src/sdl_main_android.cpp` provides the `SDL_main`
  that SDL calls. SDL3 is linked statically, `JNI_OnLoad` lives in the same `.so`.
  `lgp_add_android_apk()` packs the APK.
- `tools/android/build_apk.py`: `aapt2 compile` of the resources from `platform/android/res` (`--res`, so far only the
  TV banner) and `aapt2 link` → `javac` (SDL's Java sources from the
  SDL tarball + `platform/android/java`) → `d8` → `classes.dex` and `lib/arm64-v8a/lib<target>.so` into the APK →
  `zipalign` → `apksigner` with the debug key from `LGP_ANDROID_KEYSTORE` (see "One key for all
  builds"; if the file is missing, `keytool` creates it the first time).
- Size of the library in the APK: the NDK compiles with `-g` in every build type. The Android Gradle plugin
  strips the debug sections when packaging; `build_apk.py` did not do that until 2026-10-21. Since then it packs the
  library with `llvm-strip --strip-debug` (`LGP_ANDROID_STRIP_DEBUG`, default on, the NDK's `CMAKE_STRIP`).
  The symbol table stays, so `simpleperf` still names functions. The library in the build tree keeps its
  debug sections, for instance for `ndk-stack`. `build_apk` reports both sizes. Occasion: the Shield
  rejected a test APK with `INSTALL_FAILED_INSUFFICIENT_STORAGE`. `liblgp_game.so` was 1.39 GB
  (compressed 303 MB), with 2.7 GB free on `/data`. How much of it is debug sections has not yet been measured on the game build
  (suspicion: the bulk; check with `llvm-readelf -S`).
- `-DLGP_ANDROID_EXTRACT_NATIVE_LIBS=OFF` (default on): manifest with `extractNativeLibs="false"`, the library
  lies uncompressed and page-aligned through `zipalign -p` in the APK. Android loads it directly from there and
  creates no extracted copy under `/data/app`. In exchange the APK becomes as large as the library, the transfer
  via adb takes longer. On the device this saves the compressed copy (≈ 300 MB for the game), the extraction
  at install time is dropped. During an update, old and new version sit side by side until completion. The space
  for that is needed in both forms.
- `platform/android/java/org/lgp/recomp/LgpActivity.java`: only loads the library from the manifest entry
  `lgp.library`, sets `LGP_APP_DIR` to the app's own external folder
  (`/sdcard/Android/data/<package>/files`) and passes the intent extra `args` through as arguments.
- Window: on Android, `SdlWindow` takes the `ANativeWindow` (`SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER`) as the
  WebGPU surface, always fullscreen and landscape only (`SDL_HINT_ORIENTATIONS`; otherwise SDL also picks portrait for a
  resizable window, the manifest alone is not enough). Android takes the surface away from the activity
  when the app goes into the background. Finding on the OnePlus: the first approach via
  `SDL_EVENT_WILL_ENTER_BACKGROUND` in the event loop came too late (SDL frees the `ANativeWindow` in
  `surfaceDestroyed` on the Java thread immediately, the event loop blocks during the pause), the next
  `wgpuSurfaceConfigure` aborted with a Rust panic. Now `org.lgp.recomp.LgpSurface.surfaceDestroyed`
  calls native `SdlWindow::surface_lost()` before SDL (WebGPU surface freed, the game keeps running without a picture);
  `pump_events()` creates the new surface as soon as SDL reports an `ANativeWindow` again and
  `LgpSurface.surfaceChanged` has reported the new surface (`SdlWindow::surface_ready`). Without this lock
  the demo APK, on Home, still accessed the old `ANativeWindow` between `surfaceDestroyed` and SDL's release
  (SIGSEGV in `vkCreateAndroidSurfaceKHR`, PC test 2026-10-14). The release runs on the UI thread and must
  not wait for the GPU thread: once a cold program creation stood there for 68 s, `surfaceDestroyed` hung that long,
  and `onPause`/`nativePause` came correspondingly late (OnePlus, 2026-10-14; a blocked
  UI thread risks an ANR). `WebGpuRenderer::replace_surface` therefore frees the surface directly on the
  calling thread, under its own mutex that only a running present holds. iOS uses an
  `SDL_AddEventWatch` on `WILL_ENTER_BACKGROUND` for this. Not yet checked on the device.
- Pause in the background: SDL sends `SDL_EVENT_WILL_ENTER_BACKGROUND` from its event pump shortly before it
  blocks for the pause (`SDL_HINT_ANDROID_BLOCK_ON_PAUSE`, default on; SDL also pauses its
  audio devices). According to SDL, the handling belongs in an event filter or watch that runs when the event is queued. There,
  as under iOS, `lgp_game` halts all guest threads and lets the GPU drain
  (`SdlWindow::set_lifecycle_handler`, `boot::ProcessControl`, [`horizon-threads-sync-in-eigener-runtime.md`](horizon-threads-sync-in-eigener-runtime.md));
  `SDL_EVENT_DID_ENTER_FOREGROUND` resumes. If the app ends while it is paused (`onDestroy` or
  `SDL_EVENT_QUIT` in the background), `ProcessControl::close()` releases all pauses and makes later ones ineffective. Otherwise
  the guest would never reach the present that ends the process, and `lgp_game` would hang on exit (Codex finding, not
  reproduced on the device; test `boot.process_control`). Finding on the OnePlus 7 Pro (2026-10-14): the pause takes effect after 18 ms,
  and after 60 s in the game the process has 0.0 % CPU in the background. Directly after a cold start, however, the load was still
  at 192–214 %, suspicion: the pipeline workers that warm up the cache (`WARM_ALL`). Therefore they too
  halt since then (`WebGpuBackend::hold_prebuilds`). They then accept no new job, only the pipeline currently
  being built is finished. The log line names how many those still are. Cross-check fully cold (Home 3 s after the
  start, before the first frame): 0 pipelines open, still about one core of load for 65 s. Suspicion: the one-time
  creation of all programs from the cache on the GPU thread (`WARM_ALL` in `set_pipeline_cache`). Since then that
  too halts after the current program, and the first frame after the return creates the rest. The line
  `cached programs warmed at once … stopped, app in the background` shows this. Not yet interruptible is everything before
  the process start (loading modules, opening the RomFS); a pause only takes effect once the kernel is up.

`lgp_game` as an app (package `org.lgp.recomp.game`): without arguments the app mode starts (P7.1). If
`settings.ini` has no game folder, it takes `<app folder>/game` (containing `exefs/` and `romfs.bin` or `romfs/`);
output goes to `<app folder>/logs/lgp.log`, saves to `<app folder>/saves`. Put the dump
there via adb (the shell user may write into `Android/data`):

```sh
adb install -r lgp_game.apk
adb shell mkdir -p /sdcard/Android/data/org.lgp.recomp.game/files/game
adb push <exefs> /sdcard/Android/data/org.lgp.recomp.game/files/game/exefs
adb push romfs.bin /sdcard/Android/data/org.lgp.recomp.game/files/game/romfs.bin
adb shell am start -n org.lgp.recomp.game/org.lgp.recomp.LgpActivity        # or tap the icon
adb shell cat /sdcard/Android/data/org.lgp.recomp.game/files/logs/lgp.log
```

With arguments instead of app mode: `adb shell am start -n … -e args "--exefs game/exefs --game game --frames 600"`.
Then too (as with `--app …`) all output goes to `<app folder>/logs/lgp.log` (the run before to
`lgp.prev.log`), and relative paths apply from the app folder: the app has no console, its working directory is `/`.
Until October 2026 the pure argument mode wrote no log and looked for relative paths under `/`; a run
ended after 2 ms without a visible reason (finding on the PC). The log now starts with `app: arguments …` and
`app: working directory …` and ends with `lgp_boot: exit code N`; a usage error (e.g. a wrong `--press`)
ends without this line, but appears with its message above it. The first line is `log: descriptors before: 1 …, 2 …` (in the app both are `/dev/null`). Until October 2026 `redirect_output` used `freopen`, which closes descriptor 1 before it reopens the file; another thread's descriptor could take number 1 in that gap. On the Shield every lgp.log began with one 86-byte logd record (libc's "Access denied finding property …"), and 5 of 41 runs lost the stdout lines or all lines. Since then the file is put on 1 and 2 with `dup2` only: 20 of 20 runs complete, no record at the start (S24). `LGP_BOOT_APP_LOG_ALWAYS` turns this on for Android and
iOS apps, test on the desktop: `boot.sdk_app_hosted_args`, `boot.sdk_app_hosted_args_error`. Logcat only shows
SDL's own messages (`adb logcat -s SDL lgp`).

### One key for all builds

Android only updates an installed app if the new APK is signed with the **same** key. Otherwise
`adb install -r` aborts with `INSTALL_FAILED_UPDATE_INCOMPATIBLE`, and the only way out, uninstalling the app,
deletes its folder `/sdcard/Android/data/<package>/files` and with it the saves. A debug key
per build folder (formerly `<build>/lgp-debug.keystore`) is not enough for this: a second build tree, an
`rm -rf build` or another machine silently creates a new key.

Therefore `cmake/LgpAndroidApk.cmake` by default takes **a fixed path outside the repo**:

- `LGP_ANDROID_KEYSTORE` (cache variable), default `$LGP_PRIVATE_DIR/android/lgp-debug.keystore`; only without
  `LGP_PRIVATE_DIR` does it fall back to `<build>/lgp-debug.keystore`. The value is written into the cache at the first configure;
  a tree that was previously configured without `LGP_PRIVATE_DIR` needs
  `-DLGP_ANDROID_KEYSTORE=…` or a fresh cache.
- Configuring reports the path (`Android apps: … keystore …`) and **warns** if the file is missing: then
  the first APK build creates a new key. On a machine whose APK is already on a device with saves,
  copy the existing keystore there first. A replaced keystore (correct key copied back) re-signs the
  APK on the next build, provided the file already existed at configure time (it is then a dependency).
- After signing, `build_apk.py` prints the SHA-256 of the certificate (`build_apk: key …, certificate SHA-256 …`).
  Comparison with the installed version: get the path via `adb shell pm path <package>`, fetch the APK via `adb pull`, then
  `apksigner verify --print-certs <apk>`; both SHA-256 must be the same.
- Password and alias are the usual debug values (`android`, `androiddebugkey`). The keystore is not a secret
  in the strict sense, but like everything private it belongs in `$LGP_PRIVATE_DIR` and never into the repo (`.gitignore` excludes
  `*.keystore` and `*.jks`).
- Whoever deliberately wants a new key (e.g. a device without saves) sets `-DLGP_ANDROID_KEYSTORE` to another
  file.

A release key for a store is a separate topic and not solved here.

Profiling: the manifest declares the app profileable with `<profileable android:shell="true"/>`. This allows
`simpleperf` from `adb shell` to measure the running app without it being `debuggable` (from Android 10). Older APKs
are rejected by simpleperf with "Permission denied". Example:
`adb shell simpleperf record -p $(adb shell pidof org.lgp.recomp.game) -g --duration 10 -o /data/local/tmp/perf.data`
and then `simpleperf report`. The demo's package is `org.lgp.recomp.demo`.

### Game data in the app (split APKs) and import without adb

Main path: the game comes as split APKs alongside the program APK. A single APK with the whole game does not
work. LGP's `romfs.bin` is 4.2 GiB, and `zipalign` and `apksigner` (build-tools 35) cannot do
Zip64. A test with 4.4 GiB of dummy assets was rejected by both, a split with a 1.9 GiB uncompressed asset,
on the other hand, can be signed and verified. The tool `tools/android/build_game_splits.py` builds from it:

- `game-exefs.apk`: `exefs/` as `assets/game/exefs/` and `systemdata/` (system fonts,
  [`horizon-pl-shared-font.md`](horizon-pl-shared-font.md)) as `assets/game/systemdata/`. On the first start after
  every installation `GameBundle.java` extracts both to `<app folder>/game/`, that is only a few MiB. Without the
  standard font the tool aborts, because LGP crashes without it before frame 1 (Shield, 2026-10-20). Another
  folder works with `--systemdata DIR`. If the game folder has `mods/` (kit build, `tools/kit/build.py --target android`),
  it comes along as `assets/game/mods/` and is extracted likewise (`--mods DIR` for another folder). When
  extracting, `GameBundle` deletes the old folder first, so that for instance a dropped mod does not stay behind.
- `game-romfs-<n>.apk`: one part each of the `romfs.bin` below 2 GiB (default 1.5 GiB), uncompressed. It is
  read **in place** and not extracted. `AssetManager.openFd` provides fd, offset and length.
  `GameBundle` passes that as `LGP_APP_GAME_FILES=romfs.bin=/proc/self/fd/N@OFFSET+LENGTH,…`. The app mode
  turns that into `--game-file romfs.bin=…`, and `AddedFilesStorage` (`lgp/platform/segment_storage.hpp`, test
  `platform.segment_storage`) assembles the parts into `/romfs.bin` in the game's storage. On the device
  the game is thus present only once (≈ 4.3 GiB).

The splits need the same key and the same `versionCode` as the program APK. They contain no
code and are the game data itself: only build them on your own PC, store them under `$LGP_PRIVATE_DIR`, never commit,
upload or put them into CI.

```sh
python3 tools/android/build_game_splits.py --game $LGP_PRIVATE_DIR/<dump with exefs/, romfs.bin, systemdata/> \
    --out $LGP_PRIVATE_DIR/android-splits --sdk $ANDROID_HOME --keystore build/android-app/lgp-debug.keystore
adb install-multiple build/android-app/runtime/boot/lgp_game.apk $LGP_PRIVATE_DIR/android-splits/game-*.apk
# later only a new program, the splits stay (suspicion, to be checked on the device):
adb install-multiple -p org.lgp.recomp.game build/android-app/runtime/boot/lgp_game.apk
```

Entirely without adb this would only work via an installer app for split APKs. Android's own installer only takes
single APKs, and Play asset packs need Play or `bundletool`, which in turn uses adb.

Alternative without data splits: if the app mode finds no game, it opens the system's folder picker (Storage
Access Framework, `GameImport.java`). SDL3 has no folder picker on Android (`SDL_FILEDIALOG_OPENFOLDER`
reports "unsupported"). One picks the folder with `exefs/` and `romfs.bin` or `romfs/` (or a folder
above it). Only these parts are copied and, if present, `systemdata/` next to them, first to `<app folder>/game.import`, on success to
`<app folder>/game`. There is a progress display, the option to cancel, and beforehand a check
of free storage (requirement + 64 MiB). Error messages name what is missing in the folder. The app does not get a
general storage permission in the process. Not yet checked on the device with real data: duration of the copy via SAF with 27,818
individual files in `romfs/` (suspicion: clearly slower than a single `romfs.bin`).

## Emulator (x86_64 image)

For the Android emulator on the PC (Windows with WHPX/Hyper-V, Linux with KVM) there is a second ABI:
`-DANDROID_ABI=x86_64` added to the preset `android-app` or `tools/kit/build.py build --target android --abi x86_64`
(own build folder `build-android-x86_64`, output `android-x86_64/`; the split APKs with the game are
ABI-independent). wgpu-native then comes from the release `wgpu-android-x86_64`; a custom build under
`$LGP_PRIVATE_DIR/toolchains/wgpu-native-android` is only taken for arm64 (without the driver cache patch,
every start recompiles the pipelines). On x86_64 images from Android 11, the arm64 APK only runs via the image's
ARM translation and is too slow for that.

What the emulator can prove: build path (kit, split APKs, `adb install-multiple`), start, menus, mods,
saves, touch controls, crashes in platform-independent code. What not: performance, memory limits
and GPU peculiarities of real devices (Adreno/Mali, BCn, driver bugs). Vulkan in the emulator goes via the
host GPU (`-gpu host`); without Vulkan 1.1 in the guest the picture stays black.

## Android TV and controllers (NVIDIA Shield)

The same APK also runs on Android TV. The manifest contains `android.software.leanback` (not required),
a second launcher category `LEANBACK_LAUNCHER`, `android:isGame` and a banner
(`platform/android/res/drawable/banner.xml`, vector 320×180 dp, without game graphics). Without a banner and the
leanback category the TV launcher does not show the app. `aapt2 dump badging` reports
`leanback-launchable-activity` for the demo APK. This is checked in the cloud container, not yet on a Shield.

Input:

- **Gamepads** (Shield controller, Bluetooth pads, USB) come via SDL's `SDLControllerManager` as SDL gamepads,
  exactly like on the desktop (`SdlInput`, buttons by their label, `host-input-auf-switch-controller.md`).
  A controller's back button is `SDL_GAMEPAD_BUTTON_BACK` there, i.e. minus, and Start is plus.
  Android keeps Home for itself (Shield controller, suspicion: never arrives in the app). That is why on Android **Start,
  held for 0.7 s,** opens the app's pause (`AppMenu::set_start_hold_pause`, otherwise off), like the remote's menu button; a short press stays plus. In the
  menus, Back, Start and the button labeled B (ImGui's cancel; not in open drop-down lists
  or while adjusting a slider) go back one page, A selects (with Nintendo layout ImGui swaps this via
  `ConfigNavSwapGamepadButtons`), the D-pad and the left stick move the focus.
  Test: `platform.app_menu` (`test_pad_start_and_back`).
- **TV remote as gamepad:** SDL lists every device with source `SOURCE_DPAD` as a joystick
  (`SDL_HINT_TV_REMOTE_AS_JOYSTICK`, default on), so probably also the Android TV remotes (suspicion, not
  checked on the device). Its automatic mapping makes the center (`DPAD_CENTER`) A, Back B (the device has
  no B) and Menu Start. On Android TV and tvOS (`SdlInputOptions::tv_remotes`; D-pad pads like the
  SNES controller have shoulder buttons and keep their slot), `SdlInput` recognizes a gamepad without a right stick and without shoulder buttons
  as a remote: it gets no
  slot of its own, its buttons go to slot 0. Without that, a controller connected afterwards would have been player 2 (right
  Joy-Con) and would not have controlled the game. Logcat: `lgp: gamepad … is a TV remote: it joins slot 0`. Test: `platform.sdl_input` (`test_remote_joins_slot0`).
- **TV remote as keyboard** (if SDL does not list it as a gamepad): the D-pad comes as arrow keys
  and counts as player 1's D-pad. The center (Enter) becomes A, Back (`SDL_SCANCODE_AC_BACK`) B, Menu
  plus. On Android, Enter is A instead of plus even with a real keyboard. Holding Menu for 0.7 s opens the app's
  pause (the remote has no Home). By then the game has already seen plus pressed; a short
  press stays plain plus.
- **Back no longer quits the game.** On Android `SdlWindow` sets `SDL_HINT_ANDROID_TRAP_BACK_BUTTON`.
  Back (remote, gesture or button) then reaches the game as B or minus. Leaving works via Home. This
  also applies on phones.
- **Vibration only from Android 12:** SDL only reports rumble for an Android gamepad if `InputDevice` under API 31+
  has a `VibratorManager` with motors (`SDLControllerManager.pollInputDevices`). Below that,
  `SDL_RumbleGamepad` fails (log line `rumble failed`, once per pad). The Shield runs Android 11 (suspicion: last
  version of the Shield Experience), so the controller does not vibrate there.
- **Touch overlay:** on Android TV devices (`SDL_IsTV()`, UI mode television) it is off by default;
  `--touch on` turns it back on.

Open and not checked on the device: which labels SDL reports for the Shield controller's buttons, whether the
remote really arrives as a gamepad, and whether the Shield (Tegra X1, 3 GiB RAM) can carry the game at all. The
list of suspected cases and the test plan are kept in `docs/analysis/33-shield-testplan.md`.

## Video: MediaCodec

Boot programs on Android (shell and app) get `MediaCodecVideo` as host video (`platform/android`,
`--video host` or the default without FFmpeg): VP9 via the phone's hardware decoder, output as a ByteBuffer and
conversion to NV12. Supported output formats: `COLOR_FormatYUV420SemiPlanar` (21), Qualcomm's Venus NV12
(`0x7FA30C04`, rows aligned to 128, height to 32) and planar I420 (19); other (tiled)
vendor formats fail, then the movie HLE shows black frames. H.264 is not connected (LGP uses VP9).
For decoders whose buffer format is unknown (for instance NVIDIA's on the Shield), there is a second path, switch
`LGP_MEDIACODEC_IMAGE_READER=1` (default off, in the app via `--env`): the decoder draws into the surface of an
`AImageReader` in the format `YUV_420_888`, and each frame is copied the way its planes specify (row and
pixel stride per plane, crop rectangle; planar, NV12 and NV21 alike, `yuv420_flexible_to_nv12` in
`video_convert.hpp`). Logcat reports once `MediaCodec image reader: BxH chroma row R pixel P`. Each frame
carries the timestamp of its surface buffer, a late one does not shift the others. If a frame does not fit
or three in a row do not arrive, the decoder reports an error (then a `FallbackVideo` to libvpx takes over,
where built); if the decoder cannot be started with the surface, it uses its own buffers as without the switch. Unverified is whether this is slower than the
ByteBuffer path (one more copy in the driver, suspicion: irrelevant at 720p).
Opening a decoder (the movie HLE's `Configure` call) stops the guest. On the Shield it takes 0.2–1.3 s, with no wait
for the movie HLE's lock. logcat shows the parts for each decoder:
`MediaCodec: video/x-vnd.on2.vp9 1920x1080: create … ms, configure … ms, start … ms`. In S19–S21 (9 runs without a spare) create
took 114–1107 ms, configure 25–151 ms and start 61–362 ms.
Creating the codec ahead of time on a worker thread was tried and dropped (S20/S21, 4 valid runs per arm). Creating
it at app start took only 6–12 ms, but configure then took about 2.6 times as long (311 against 118 ms on average):
the work only moves. Decoder:2 fell by 0.13 s on average, less than the spread of the title window (2.9–5.4 s).
With libvpx in the build (`-DLGP_ENABLE_VPX=ON`, not the default on Android) libvpx then takes over the video
(`platform::FallbackVideo`): if the hardware decoder fails before its first frame or shows none at all up to the end of the video
or within 30 frames (`kMaxKeptFrames`), libvpx gets all frames delivered so far and the rest
of the video (log `video: the first decoder failed or showed nothing before its first picture`). Without libvpx it stays
black. The fallback only applies without `--video`. `--video host` checks MediaCodec alone. It also kicks in on an error
that would have been temporary, and stays in place for this video. libvpx in software can be too slow on weak CPUs
(Tegra X1, suspicion). Test of the switch-over: `platform.fallback_video`.
Test: `platform.mediacodec_video` (test video `vp9_pattern.webm`, runs on the device via adb), the same test via
the AImageReader as `platform.mediacodec_video_image_reader`; the plane variants are checked by `platform.video_convert`
even without a device. For Android this replaces
the libvpx path planned in ADR-0008 (no third-party code, hardware decoder); for iOS/tvOS the question remains
open (VideoToolbox or libvpx there).

## Textures: BCn without GPU support

Mobile GPUs (Adreno, Mali) do not have `texture-compression-bc`. Even on the iPad Pro M1, wgpu-native does not offer the
feature (258 BC textures "not supported" before the CPU path, [`ios-app-bauen.md`](ios-app-bauen.md)); desktop GPUs
have it. Without the feature the WebGPU backend decodes BC1–BC5 and BC7 on the CPU during upload (`*_SRGB` stays
sRGB; BC1–BC3 and BC7 to RGBA8, BC4/BC5 to R8/RG8, snorm as `R8Snorm`/`RG8Snorm`),
decoder `gpu/texture/bc.hpp`, as with the ASTC path. BC6H (float) is not covered and stays empty. Force it for
checking on the desktop: `LGP_WEBGPU_DEBUG=cpubc` or `WebGpuBackend::force_cpu_bc(true)`; counter `bc_cpu_decoded`.
Tests: `gpu.bc` (1024 blocks against Pillow's decoder, regenerate with `tools/texture/gen_bc_vectors.py`),
`nvn.webgpu_draw` (BC1 from pool memory). Cost: four times (BC1: eight times, BC4/BC5: twice) as much GPU memory per texture and
CPU time during upload.

Multiple threads (default 0, on tvOS 3): `LGP_WEBGPU_DECODE_THREADS=N` (0–16) distributes the block rows of large
BCn levels over N helpers plus the GPU thread (`gpu::texture::DecodePool`, `gpu/texture/parallel.hpp`; levels below
about 64 KiB of blocks stay on the GPU thread). Result bit-identical (test `gpu.bc`, random blocks of all formats).
Cloud machine (4 cores), 2048×2048: BC7 99 → 39 ms with 2 helpers, 31 ms with 3; BC1 15 → 5 ms with 3.

Apple TV 4K (A15, tvOS 27.2), ABAB with host clock, without HDMI, 3 versus 0 helpers: in the scene-change dip the
uploads fell from 767–796 to 526–534 ms (−32 %), the peak of "textures from pools" from about 665 to 425 ms (−36 %) and
deswizzle/convert by 41 %. The frame rate there rose from 23.4 to 24.6–24.8 fps. In the quiet window all arms were at
29.3 fps, so the helpers cost nothing there. That is why 3 is the default on tvOS. On iPad and Android it is not
measured and stays 0. The helpers compete there with the guest threads for the cores.

Two decoders write the same bytes: `fast` (default) and `reference` (`LGP_WEBGPU_BC_DECODER=reference`). The
reference decoder reads BC7 bit by bit and writes each texel byte by byte into an intermediate block. The fast decoder
writes a texel as a 32-bit word and whole blocks directly into the image. It reads BC7 fields with shifts, the
indices from a 64-bit window per set, and has its own instance with fixed field widths per BC7 mode. The equality
is checked by `gpu.bc` on all vectors with both decoders and on 200,000 random blocks per format, plus on images with
edge blocks. Measure: `lgp_bc_bench [--size N] [--seconds S]` (release build, ctest `bench.bc_decode` only checks the
equality). In the cloud (x86, GCC 13, `-O2`, noisy) the fast decoder was about 2.2–2.5 times as fast for BC7, about
1.7–2 times for BC1, about 1.7 times for BC3 and about 1.1–1.3 times for BC4/BC5. On ARM the gap is larger. On a
Mac M1 in the release build the following values came out (Mtexel/s, reference → fast):

| Format | Reference | Fast | Factor |
|---|---|---|---|
| BC1 | 725 | 1537 | 2.1× |
| BC2 | 773 | 1354 | 1.75× |
| BC3 | 472 | 832 | 1.76× |
| BC4 | 524 | 964 | 1.84× |
| BC5 | 380 | 700 | 1.84× |
| BC7 | 83 | 429 | 5.1× |

In the game on the Apple TV 4K (ABAB, scene-change window) the uploads fell from about 554 to 269 ms. The largest
peak of "textures from pools" fell by 54 %, deswizzle/convert by 62 %. The window fps rose from 24.4–24.9 to
26.4–26.5. Since then `fast` is the default on all platforms that decode BCn on the CPU.

NEON: for BC7 the time was not in missing vectorization, but in bitwise reading and in the branching per
texel. The fast decoder solves both without SIMD. Suitable for NEON are the palette lookups of BC1 and BC4/BC5:
16 texels with `vqtbl1q_u8` from a 16-byte palette. That is only worthwhile once the measurement on the iPad shows that
BC1/BC4 still matter there. BC4 and BC5 are also first decoded to RGBA8 and only then compacted to R8/RG8 in the backend.
Writing directly to R8/RG8 saves this detour and three quarters of the writes. That is a
separate step and not built yet.

### GPU memory

On the OnePlus 7 Pro the game aborted at frame ~5800 with "Not enough memory left" (wgpu). The line
`gpu memory` of `--time-stats` already showed around 2540 live textures with ~3 GiB from frame 1, the limit was at
~3.2 GiB. Where the memory goes is shown by `LGP_WEBGPU_TEXTURE_REPORT=N`: every N frames a summary line (never
used, used in the last N frames, GPU-written, BC-decoded including size as BC, shared
pool memory) and the 16 largest groups by host format (WebGPU enum, hex), CPU decoding and edge length.
Plus a line `cpu copies`: what the backend itself holds in CPU memory (guest bytes of the geometry cache,
upload arenas, shadows of the pitch-linear textures, upload buffer, WGSL of the programs, keys of the
draw pipelines, texts of the pipeline cache, number of cached bind groups). Allocations in wgpu/naga and in the
driver are not counted. The pipeline cache now remembers known records only as a 64-bit hash instead of as a
second copy of each WGSL text. The program texts of the cache (WGSL and bindings, ~20 MiB in LGP) are freed by the backend
as soon as all cached programs have been created; after that it only needs the recipes.
`LGP_WEBGPU_LAZY_TEXTURES=0/1` creates the GPU side of a texture only on first use (bind, draw,
copy, present), not already at `nvnTextureInitialize`; test `nvn.webgpu_draw_lazy`. Measurement on the phone (frame
300): 2538 textures with 2959 MiB, of which 2891 MiB never used; BC-decoded 2789 MiB (~543 MiB as BC). That is why
the switch is on by default on Android and iOS, off on the desktop.
`LGP_WEBGPU_TEXTURE_BUDGET_MB=M` (or `WebGpuBackend::set_texture_budget`) limits the textures with a GPU side:
if the sum at present time is above M, textures whose contents come from pool memory and that have not been used for two
frames lose their GPU side (the longest unused first, down to 90 % of M). The next
use creates them anew and uploads them again. What the GPU has written (render targets, copies, clears,
`nvnTextureWriteTexels`) stays. Counter `textures_evicted`, test in `nvn.webgpu_draw` (BC1 case). The
budget is off by default.

On Adreno 640 (OnePlus 7 Pro), in `nvn.webgpu_draw` all compute invocations wrote the value of word 0, although
the memory locations were correct. Suspicion: with a dynamic index into a `vec4` from a
uniform buffer (`cb[i][j]`, WGSL → SPIR-V `OpVectorExtractDynamic`/access chain) the driver always returns component x. Fix:
`wgsl::Options::vector_select` picks the component with `select()` (helper function `vsel`); on by default in
Android builds, off otherwise, switchable via `LGP_WGSL_VECTOR_SELECT=0/1`. It was not the naga bounds checks
(`LGP_WEBGPU_BOUNDS_CHECKS=0` changed nothing; the switch stays for diagnostics).

## Shader costs at start

On Adreno 640, creating individual compute pipelines costs a great deal: in a cold run 73 programs in 108 s, of which
4 compute programs took 107.9 s (two of them 57 s and 43 s), the 69 graphics programs together 0.6 s. A Vulkan
pipeline cache of the driver does not carry over across program starts (warm run equally slow), and wgpu-native
29 offers no pipeline cache API in C. Therefore, on Android, the backend creates all programs from its own
pipeline cache (`--pipeline-cache`) right at start and builds the compute pipelines on the worker threads
(`LGP_WEBGPU_WARM_ALL`, on by default on Android): first 300 frames 15.9 s instead of 130 s. The very first start without
a cache stays slow.

A shipped seed helps against that (shared with iOS/tvOS, `LGP_APP_PIPELINE_SEED=<file.lgppipes>`, e.g. the
cache from a longer game session): on Android `build_apk.py --asset` puts it into the APK as `assets/pipeline-seed.lgppipes`.
`LgpActivity` copies it before the start to `<app folder>/package/pipeline-seed.lgppipes`, once and
again after each app update (APK assets are not files). `platform::packaged_game_dir()` on Android is
`<app folder>/package/game` (never exists, the game is not in the APK); next to it the program finds the seed as
on Apple devices. Only records missing from the own cache come from the seed; the player's cache is never
replaced (`PipelineCacheFile::import_records`), the seed is read on every start (one file, the size of the cache).

Without `LGP_APP_PIPELINE_SEED`, a local game build (with `LGP_GAME_LIFTED`) automatically takes
`$LGP_PRIVATE_DIR/pipeline-seed.lgppipes` if the file exists at configure time (`LGP_APP_PIPELINE_SEED_AUTO`,
default on, applies to all app packages). Configuring reports `lgp_game: pipeline seed …`. The file is only created
locally from one's own cache and never goes into the repo or into CI. CI builds have neither `LGP_GAME_LIFTED` nor
`LGP_PRIVATE_DIR` and build without a seed as before. The environment only counts at configure time. In a shell without
`LGP_PRIVATE_DIR` (for instance a non-login SSH on the Mac) there is no automatic seed; then set the path
explicitly. Measured on the Apple TV, cold versus seed from the package (19.3 MB, 999 entries), ABAB: 0 instead of 165 new
draw pipelines, frames 701–800 at 23.5 instead of 20.7 fps, wall time 49 s instead of 51–57 s. The Metal shader cache was warm in
both arms.
With `-DLGP_APP_DRIVER_SEED=ON` the driver cache next to the seed (`<seed>.driver`, only with patched
wgpu-native) also comes along as `assets/pipeline-seed.lgppipes.driver` and is copied the same way (both at most 32 MiB, otherwise
discarded, log `pipeline cache seed …: larger than …`); the program starts with it as long as it has no
driver cache of its own (`webgpu-backend-draw-kosten.md`). It only fits the same GPU and the same driver.
What this brings: a freshly installed device knows the programs immediately and builds them with `LGP_WEBGPU_WARM_ALL`
at start instead of when an effect first appears. It does not remove the driver compile (Adreno still compiles
every pipeline, about 127 ms per pipeline on one thread); for that the driver cache `<cache>.driver` is needed
(#171). It is bound to GPU and driver version and cannot be mixed; as a seed it is only suitable for the same
device model with the same driver (not built yet). Measurement of the seed on the device is pending.

The APK of `lgp_game` contains translated game code: only build it on your own PC, only onto your own device, never into CI,
never upload it. The demo APK contains no game code.

## CPU cores: big cores stay unused

Finding on the OnePlus 7 Pro (Snapdragon 855: cpu0–3 small up to 1.79 GHz, cpu4–6 big up to 2.42 GHz, cpu7 up to 2.84 GHz),
shell program via adb with the screen locked, 2026-10-14: after 30 s the device reached 84.6 °C. From about 150 s on
the big cores ran permanently at idle clock (cpu4 710 MHz, cpu7 826 MHz), although the temperature fell back to
41–46 °C. The small cores meanwhile ran at full clock, and the game fell from ~20 to 4–10 fps. A
thermal throttling would slow the big cores, but not leave them idle. **Suspicion:** after the lock the process
is in a cpuset or uclamp group for the background, or is put onto the small cores by a vendor core control
(core_ctl). Unproven until `/proc/<pid>/cpuset` and a run with the screen on are available.

Tool for this (`platform/os/src/cpu_affinity.cpp`, test `platform.backends`): every guest thread (`HostThread`) reads
`LGP_CPU_AFFINITY` at start:

| Value | Effect |
|---|---|
| `all` (outside of Android also empty) | change nothing (default except on Android) |
| `big` (on Android also empty) | all cores except the slowest cluster, by `cpuinfo_max_freq` (SD855: 4–7); default on Android |
| `4-7`, `0,2,4-5` | exactly these cores |

On Android the first guest thread always writes a line `cpu affinity: … allowed <cores>, cpuset <group>`,
even without the variable. `sched_setaffinity` can only choose within the cpuset: if the group excludes the big cores,
the call fails (message "sched_setaffinity failed"), and then the only thing that helps is that the app runs in the foreground
(cpuset `top-app`).

Measurement ABAB on the OnePlus 7 Pro (shell program, warm pipeline cache, 3000 frames, every run from 34.9 °C,
2026-10-14): without pinning 405 s and 410 s, with `big` 276 s and 275 s, i.e. −32 %. The fps rise in all
sections (e.g. 14 → 24 and 3.7 → 6.1). From frame ~2400, without pinning 99.5 % of the samples run on the small
cores, with `big` 97.5 % on the middle ones. That is why `big` is the default on Android; `LGP_CPU_AFFINITY=all`
turns it off. Even when pinned, the middle cores only clock at ~1.1 of 2.42 GHz on average. **Suspicion:** the
governor (schedutil) is the next lever.

The choice is intersected with the cores the thread is currently allowed to use; a narrower mask (`taskset`, cpuset) is never
widened. `big` uses `cpu<N>/cpu_capacity` if every core has one (also separates clusters with the same maximum clock,
e.g. Helio P60), otherwise `cpufreq/policy<N>/cpuinfo_max_freq` for all `related_cpus` (also cores that are switched off).
A change of the cpuset (background → `top-app`) resets the mask of every thread on older kernels (OnePlus 7 Pro: 4.14);
therefore, when returning to the foreground, the app re-pins all
guest threads (`reapply_thread_cpu_policy`). Checked on the OnePlus (2026-10-14, app mode): in the game 12 guest threads on 4-7 (`top-app`), after Home
0-3 (`background`), 5 s and 15 s after the return 4-7 again.

Threads carry names (`set_current_thread_name`): `lgp-g<id>` for guest threads, `lgp-pipe` for the pipeline workers,
`lgp-boot` for the app's process thread. In `adb shell top -H -p <pid>` everything is otherwise called "SDLThread".

`APerformanceHint` (ADPF, `android/performance_hint.h`) only exists from API 33 (Android 13). The test device has
Android 12, the app targets API 29; the interface is missing there. On devices from Android 13 on, a hint thread
per guest thread with a target time of 33 ms would be the clean way, via `dlsym`, because the minimum API stays 29.

## Main thread (SDLThread) at 100 % of a core

Measured on the OnePlus 7 Pro with `top -H` (PC, 2026-10-14): the thread `SDLThread` is permanently at about 100 % of a
core and is thus the hottest thread of the game. On Android this is the thread on which SDL starts `main()`. In
app and window mode only the event loop runs there (`window->pump_events(10)`, then `sdl_input->service()`,
overlay and menu). The game itself runs in its own threads.

**Cause according to the SDL source code (3.4.16, read; the same on SDL's `main`; proof with simpleperf is pending, until then
suspicion):** `SDL_WaitEventTimeout` has its own branch on Android (`src/events/SDL_events.c`,
`SDL_WaitEventTimeoutNS`, `#ifdef SDL_PLATFORM_ANDROID`):

1. Every round calls `SDL_PumpEventsInternal(true)`. That puts an `SDL_EVENT_POLL_SENTINEL` into the queue.
2. Every new event triggers `SDL_SendWakeupEvent()`. On Android that is
   `Android_SendLifecycleEvent(SDL_ANDROID_LIFECYCLE_WAKE)`.
3. Then `Android_PumpEvents(delay)` waits for a lifecycle event. It immediately finds the `WAKE` just created,
   sets the wait time to 0 and returns.
4. The loop starts over until the wait time (10 ms in our case) is up.

So `SDL_WaitEventTimeout` never waits, but spins the whole time. It has nothing to do with controllers or sensors:
Android only looks for joysticks every 3 s (`ANDROID_JoystickDetect`), and sensors are delivered by a separate thread at 60 Hz.

**Fix behind a switch:** `LGP_SDL_PUMP_MS=N` (e.g. `--env LGP_SDL_PUMP_MS=4` in argument mode) replaces the
waiting in `SdlWindow::pump_events` with `SDL_PollEvent`. After that the thread sleeps at most N ms with
`SDL_DelayNS` and asks again, until events are there or the wait time is up. Inputs thus arrive at most N ms
later. On pause, `SDL_PollEvent` keeps blocking as before (`SDL_HINT_ANDROID_BLOCK_ON_PAUSE`). Without the
variable everything stays as before.

**Measurement (OnePlus 7 Pro, ABAB up to frame 3000, warm cache, start at 34.9 °C, 2026-10-14):** the `SDLThread` drops
from about 101 % (CPU 7) to 1.3–1.6 %. But the game becomes reproducibly about 29 % slower: 13.96 and
13.91 fps without the switch, 10.82 and 10.74 fps with `LGP_SDL_PUMP_MS=4`, wall time 215 s versus 278 s. The work per frame
rises, guest thread 1 from 15.6 to 21.8 ms, GPU from 37.9 to 45.7 ms. The switch therefore stays off.
**Suspicion, not yet proven:** if no core runs at full load any more, the governors lower the clocks of CPU, memory or GPU bus.
This can be checked by logging the clocks every second (`cpufreq/policy0/4/7`, `/sys/class/devfreq/*`).
`APerformanceHint` does not exist on this device (Android 12, see the ADPF section above).
Without root, `/sys/class/devfreq` and the kgsl clocks are not readable there, only the CPU policies. For an indirect
check there is therefore the diagnostic switch `LGP_CLOCK_KEEPER=CPU[:PERCENT[:PERIOD_US]]`: a thread `lgp-clock` is pinned to the CPU
and keeps it busy PERCENT (default 100) of every 10 ms, also in the background. It is only meant for measurements.
If the fps come back with `LGP_SDL_PUMP_MS=4 LGP_CLOCK_KEEPER=7`, the gain was due to a fully loaded
CPU 7 and not to SDL.

**Mind the measurement mode:** the vpress smoke test runs with `--clock virtual`. That sets `--sched baton`, i.e. a
single slot for all guest threads. Every handoff is on the critical path there. When playing (host clock),
`core-baton` applies with one slot per guest core. The following numbers therefore only apply to the measurement mode. Statements about fps when
playing come from runs with `--clock host`.

**Result of the clock keeper (ABDABD up to frame 1500, wall time, 2026-10-14):** without the switch 74.1/75.4 s. With
`PUMP_MS=4` and the keeper on CPU 7 75.4/75.2 s. With `PUMP_MS=4` and the keeper on CPU 0 (small core) 75.9/77.1 s; in that case
CPU 7 drops to about 900 MHz. `PUMP_MS=4` alone: 87.9/89.5 s. Policy 4 is at 1.0–1.2 GHz everywhere. That refutes the
hypothesis about the prime core or the DDR clock: it is enough that some core never sleeps.
**Suspicion, not proven:** otherwise clusters and LLCC drop into deep idle states, and every handoff between
guest threads or to the GPU pays the wake-up latency.

Switches to achieve this more precisely without fully occupying a core (all default off, all for measurements):
- `LGP_CLOCK_KEEPER=CPU:PERCENT:PERIOD_US` (third part of the switch above): the keeper runs at partial load and with a shorter period (default 10000 µs).
  Short sleep phases presumably let the idle governor choose shallow states, e.g. `0:20:1000`.
- `LGP_PARK_SPIN_US=N`: a guest thread that waits (baton, event, mutex, sleep with deadline) first spins for up to
  N µs without a lock. If the wake-up comes within that time, it does not sleep at all (`Scheduler::set_park_spin_ns`, test
  `runtime.threads_lifted` section `park_spin`). That costs at most N µs of CPU per wait.
- `LGP_GPU_SPIN_US=N`: `WebGpuRenderer::run` polls the device for N µs without blocking (`wgpuDevicePoll` without
  waiting) before it waits blocking. That only helps if GPU tasks wait for the device at all.
- `LGP_RENDER_RELEASE_SLOT=1`: renderer calls (`TimedBackend`, time category `gpu`) give up the slot of their guest core
  in `core-baton` mode while they run. Another guest thread of the same core then keeps running, as on
  the Switch next to the GPU. So that there is no mutual blocking, a thread waiting for the occupied NVN HLE lock
  also waits without a slot (`rt::HostWorkScope`, `lgp/runtime/host_work.hpp`, test `runtime.time_stats`). In
  `baton` mode (i.e. with `--clock virtual`) the switch has no effect, so that runs stay reproducible.
  **Risk:** the renderer reads guest memory while another guest thread keeps running. In `free` mode that
  already exists.
  **Suspicion, not measured:** without a slot, a lower-priority thread of the same core can also run while
  the higher-priority one is stuck in the renderer. When the higher-priority one returns, it must first get the slot
  back. If the other one is just spinning in a wait loop without an SVC, that can delay the return.
- `LGP_CPU_DMA_LATENCY=US`: writes US to `/dev/cpu_dma_latency` (PM QoS) and keeps the file open as long as the
  window lives. For apps the file is usually locked. The log says whether it worked (`lgp: /dev/cpu_dma_latency: …`).

On iOS, SDL's UIKit driver has no `WaitEventTimeout`. According to the source code, SDL polls there every 1 ms with
`SDL_DelayNS`; that is not spinning, but unverified.

## Android TV: output size and guest block in a file

- **Surface:** on Android there is no limit by default, not even on Android TV, as long as it has not been measured
  there. `LGP_SURFACE_MAX_HEIGHT=1440` limits the swapchain to 1440 rows as under tvOS. On a
  4K TV it then costs ≈ 14 MiB per frame instead of ≈ 32 MiB. The compositor does the upscaling (suspicion, not
  checked on the device). If the measurement on the Shield shows that this works, 1440 becomes the default there (`SDL_IsTV()`).
- **Guest block in a file:** `LgpActivity` sets `TMPDIR` to the app's internal cache (`getCacheDir()`), because
  apps get no `TMPDIR`, and `/tmp` does not exist. `LGP_HOST_BLOCK_FILE=1` then puts the guest block, as under
  tvOS, on a deleted, sparse file there. Deliberately not in the external app folder, which runs via FUSE.
  The default stays off. Whether Android's lmkd spares file-backed pages like jetsam under tvOS is open. Only
  a measurement will show that (`dumpsys meminfo` up to Route 1, with and without).

## Sound: counting underruns

SDL 3.4 plays via AAudio on Android (`SDL_aaudio.c`, OpenSL ES only as a fallback). Without a hint SDL opens the
stream in mode `AAUDIO_PERFORMANCE_MODE_LOW_LATENCY` with 20 ms per callback (`freq / 50`) and twice as much
buffer capacity (40 ms). On top of that SDL itself keeps three such buffers ready. The callback is served by the AAudio thread, the
buffers are filled by SDL's own audio thread. Neither of the two is pinned; that only applies to guest threads. An underrun at the
AAudio level only ends up in SDL's disabled `LOGI` ("Audio playback underflow"), so it does not appear in the log.

Since P7.3, `SdlAudio` counts the underrun on our side: if the device requests more than is in the queue
(`SDL_SetAudioStreamGetCallback` with `additional_amount > 0`), audout did not deliver in time, and the
device plays silence. That is the expected case when the game hangs. At most every 10 s and at the end the log says
`lgp audio: queue ran short N times (X ms silence) …` (upper bound, SDL asks somewhat generously). On opening it says
`lgp audio: device … Hz, … frames per buffer`. Pauses are not counted (audout stop and the app's pause menu).
A hang is only reported afterwards, on the next write. The lines go via `SDL_Log`, so on Android to
Logcat (tag `SDL/APP`), not into the log file. An output device with more than 48 kHz can occasionally
count wrongly (SDL converts the amount).

**Measurement (OnePlus 7 Pro, `--clock host`, up to frame 10500, 2026-10-14):** 179 underruns, all between frame ~1300 and
~3300, i.e. where the fps drop (down to 9.9 fps). From frame 3601 on the game ran continuously at 29–30 fps and without
underruns. So the sound is a consequence of the hangs, not their cause. A larger queue would only shift the gaps,
which is why there is no switch for it.

Switches for measurements (in argument mode each as `--env NAME=VALUE`):
- `SDL_AUDIO_DEVICE_SAMPLE_FRAMES` (SDL hint, default 960 = 20 ms): frames per AAudio callback, the capacity is
  twice that.
- `SDL_ANDROID_LOW_LATENCY_AUDIO=0` (SDL hint): without low-latency mode, i.e. `PERFORMANCE_MODE_NONE` with the
  normal mixer path and larger buffers. SDL does not set PowerSaving.

Underruns below SDL are shown by `adb shell dumpsys media.audio_flinger` (column "UndFrmCnt" or "Underruns" of the
track, only without the MMAP path) or `adb shell dumpsys media.aaudio`. Which core SDL's audio thread runs on is shown by
`adb shell ps -T -o tid,psr,comm -p <pid>`.
