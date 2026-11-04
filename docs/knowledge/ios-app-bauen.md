# Building the recompiled game as an iOS/iPadOS app (P7.2b)

Cross-game knowledge and guide, as of 2026-10-14. Background:
[`ios-als-zielplattform-fuer-switch-recomp.md`](ios-als-zielplattform-fuer-switch-recomp.md),
memory limits: [`speicherprobe-mobil.md`](speicherprobe-mobil.md). Results for a specific game belong
in `docs/analysis/`.

Building only works on a Mac: by its license, Xcode's iOS SDK may only run on Apple hardware. Built apps
with lifted game code stay local, never from CI.

## Structure

The same boot program as on the desktop (`lgp_add_boot_executable(… APP)`), on iOS as an app bundle:

| Part | iOS |
|---|---|
| Renderer | WebGPU renderer via wgpu-native on Metal. The wgpu-native release contains a ready-made static library for devices (`wgpu-ios-aarch64-release.zip`). An own Rust build is not needed. It is linked statically so that no framework has to be embedded and signed. |
| Window, input, sound | SDL3 (static). SDL owns `main()` (`platform/desktop/src/sdl_main.cpp`): it starts `UIApplicationMain` with its scene delegate `SDLUIKitSceneDelegate` and calls `lgp_boot_main` as soon as the scene is connected. The Metal surface is created as under macOS with `SDL_Metal_CreateView`. |
| Scene life cycle | Required with current SDKs. SDL 3.4 brings it along, `platform/ios/Info.plist.in` carries the scene manifest. |
| Memory | Entitlements `increased-memory-limit` and `extended-virtual-addressing` (`platform/ios/lgp_app.entitlements`) |
| Folders | `Documents/` of the app container for `settings.ini`, `saves/`, `logs/lgp.log` and `game/` (game dump). It is visible in the Files app (`UIFileSharingEnabled`). The pipeline cache is in `Library/Application Support/cache/` (until October 2026 in `Library/Caches/`, which iOS may empty when storage is low; after the update it rebuilds once, the old folder stays until iOS empties it). The cache folder is excluded from iCloud and computer backups (`platform::exclude_from_backup`). At build time `-DLGP_APP_PIPELINE_SEED=<datei.lgppipes>` (without the option automatically `$LGP_PRIVATE_DIR/pipeline-seed.lgppipes`, if present, see `android-ndk-build.md`) puts a seed into the package (`pipeline-seed.lgppipes` next to `game/`); the app takes missing entries from it, so that a fresh installation also creates the pipelines before first use. If the option is cleared again and no automatic seed applies any more (`-DLGP_APP_PIPELINE_SEED_AUTO=OFF` or no file in `$LGP_PRIVATE_DIR`), the next build removes the previously copied seed (or the previously copied `game/` folder) from the package; what the build did not copy itself stays untouched. The seed comes from the game and, like the package, stays local. |
| Game folder | There is no folder selection. Without `game =` in `settings.ini` the app first takes `game/` in the app package (copied in at build time with `-DLGP_APP_GAME_DIR=<Spielordner>`; the package then contains game data and stays local), otherwise `Documents/game` (`exefs/` and `romfs.bin` or `romfs/`, plus `systemdata/` if the game reads system fonts or other system data). Symbolic links in the game folder (e.g. `romfs.bin` on another drive) are copied into the package as files, because a link points nowhere on the device and `codesign` rejects it. Suspicion (not measured): Xcode re-signs the whole package with the roughly 4 GiB of game data on every build, even if only code has changed, and takes noticeably longer for it. The package route has not been checked on the device yet. |
| Full screen | `platform/ios/Info.plist.in` requires full screen (`UIRequiresFullScreen`, no Split View), landscape only, and hides the status bar at start (`UIStatusBarHidden`). After that SDL's view controller decides (`prefersStatusBarHidden`, `UIViewControllerBasedStatusBarAppearance` stays at the default): it hides the status bar only for windows with `SDL_WINDOW_FULLSCREEN` or `BORDERLESS`. That is why `SdlWindow::create` always creates the window under iOS/iPadOS with `SDL_WINDOW_FULLSCREEN` (as under Android); without it the status bar stayed visible on the iPad in landscape (inferred from SDL 3.4.16 `SDL_uikitviewcontroller.m`, unverified on the device). The home indicator: `SDL_HINT_IOS_HIDE_HOME_INDICATOR=2` (dimmed, the system's edge gestures are deferred: the first swipe up shows it, only the second goes to the home screen). `1` would have hidden it only until the next touch, so practically never in a touch game, and left Home at one swipe; a stick drag at the bottom edge would have left the game. An environment variable `SDL_IOS_HIDE_HOME_INDICATOR` wins. The SDL view and the Metal surface (`SDL_WINDOW_HIGH_PIXEL_DENSITY`, `contentsScale = nativeScale`) cover the whole screen including the area around the notch and corners, at full pixel size. From iPadOS 26 on, `UIRequiresFullScreen` is deprecated: apps can run in resizable windows (multitasking "Fenster-Apps" (windowed apps), Stage Manager); the user then chooses full screen via the window controls or in the settings. A public API that forces full screen or hides the window buttons ("Ampel", traffic lights) is offered neither by UIKit as far as we know nor by SDL 3.4.16 (there `UIKit_SetWindowSize` uses `requestGeometryUpdate` only under visionOS, `SDL_SetWindowFullscreen` only adjusts the status bar and view frame). The closest is the scene's size constraint: on the iPad `SdlWindow::create` sets `UIWindowScene.sizeRestrictions.minimumSize` to the whole screen size in landscape (`platform/desktop/src/ios_scene.mm`, log `lgp: asked for a window of the whole screen`, switch off with `LGP_IOS_FULL_SIZE=0`). Whether iPadOS 26 respects this in window mode is unverified on the device. If the window size does change, the renderer (`SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED` → new swapchain), touch controls (aspect ratio, safe area) and menus (`SDL_EVENT_WINDOW_RESIZED`) follow; SDL reports the size from `viewDidLayoutSubviews`, also for windows with `SDL_WINDOW_FULLSCREEN`. Aspect (`aspect = fit/fill/stretch`) and safe area for menu and touch controls: `desktop-app-huelle.md`. |
| System settings | Since 2026-10-22 the settings that need a restart are in Settings > Apps > LGP: "Controller", "Lichteffekte" (light effects), "Auflösung" (resolution), "Grafik-Thread" (graphics thread), "WLAN" (Wi-Fi), "Online", "Server", "Raum" (room) (`platform/ios/Settings.bundle/Root.plist`, only `lgp_game`, not tvOS: as far as we know there is no app page in the settings there). The app menu leaves these rows out and shows on "Steuerung" (controls, before the game), "Bild" (image) and "Spiel" (game) the button "iOS-Einstellungen öffnen" (open iOS settings) (`app-settings:`). The source stays `settings.ini` (iCloud backs it up). At start `sync_system_settings` (`runtime/boot/src/main.cpp`, rule in `lgp/platform/system_settings.hpp`, NSUserDefaults in `platform/apple/src/system_settings.mm`) takes over every value that differs in the settings from what was written there at the last start (`lgp.mirror.<Schlüssel>`), and then writes all values back. Invalid values (e.g. a server address that `parse_ryuldn_server` rejects) are not taken over and are reset in the settings. Not yet checked on the device, nor whether CMake puts the bundle at the right place in the app package via `MACOSX_PACKAGE_LOCATION Resources/Settings.bundle`. |
| Log | In app mode always in `Documents/logs/lgp.log`, also when started with `--app` |
| Videos | libvpx (VP9 in software, `--video vpx`, default without FFmpeg): `configure --target=arm64-darwin-gcc` fetches the compiler and iPhoneOS SDK itself via `xcrun`. Before (without a decoder) in-game videos stayed black, the sound played (libvorbis). Checked on Paul's iPad (2026-10-14). Not for the simulator. |
| Off | Unicorn, hypervisor oracle, tests (`BUILD_TESTING=OFF`). Tests run in the desktop tree. |

## Building (Mac)

The native tree generates the lifted sources as before (`lgp_lift … --split N`). They are platform-neutral C++
and are only recompiled for iOS.

```sh
cmake --preset ios -DLGP_IOS_TEAM=<Team-ID> -DLGP_GAME_LIFTED=$LGP_PRIVATE_DIR/<lift-Ausgabe>
cmake --build --preset ios --target lgp_game
# App: build/ios/runtime/boot/Release-iphoneos/lgp_game.app
```

- With ccache in `PATH`, all compilations go through ccache. Because the Xcode generator does not know `CMAKE_CXX_COMPILER_LAUNCHER`,
  Xcode's `CC`/`CXX` point to wrapper scripts in the build folder. Checked: a deleted lift object came
  back as a ccache hit in 3 s. The cache is small (0.1 GiB for all 574 objects) because ccache compresses. Switch off with `-DLGP_IOS_CCACHE=OFF`.
- `-allowProvisioningUpdates` (set in the build preset) lets Xcode create the provisioning profile for the new bundle ID
  `<Präfix>.game` itself, including the entitlements.
- `-jobs 2` (also in the build preset) limits the parallel compilations. The lifted code needs a lot of memory per translation unit.
- Arguments after `--` (`cmake --build --preset ios … -- <xcodebuild-Optionen>`) replace the preset's options
  instead of adding to them. Without `-allowProvisioningUpdates` Xcode then takes the wildcard profile, and signing
  fails with "Provisioning Profile … doesn't include Increased Memory Limit". Whoever passes their own options has to
  include `-allowProvisioningUpdates` as well.

### Game Mode (switch, on by default)

`LGP_APPLE_GAME_MODE` (default `ON`, `-DLGP_APPLE_GAME_MODE=OFF` builds without) marks the apps as a game. It then sets `LSApplicationCategoryType` =
`public.app-category.games`, `GCSupportsGameMode` (according to Apple's docs iOS/iPadOS 18.0+, according to the forum also macOS 14+) and
`LSSupportsGameMode` (according to Apple's docs iOS/iPadOS 18.6+ and macOS 26+; under macOS 26 Xcode warns that it will become required). Apple forum threads 782343 and
764575 recommend setting both. The keys are in the template as `@LGP_PLIST_GAME_MODE@` and are filled in per target in
`<build>/…/<ziel>.Info.plist.in`.

According to Apple (support article 105118), in Game Mode the game gets the highest priority for CPU and GPU,
background work is throttled, and Bluetooth is sampled twice as often. This shortens the latency of
controllers and AirPods. Two limitations apply:

- The keys only make the app eligible. Whether Game Mode really kicks in is decided by the system at runtime.
  Developers report in the Apple forums that it triggers for apps from Xcode or TestFlight.
- tvOS has no Game Mode: Apple's docs list both keys only for iOS, iPadOS and macOS. On the Apple TV 4K
  (tvOS 27.2, build with the switch, keys checked in the Info.plist) the performance overlay shows "Game Mode = Off",
  no notification appears (2026-10-20). The keys do no harm there, but have no effect.

On macOS (Sonoma, M1 Max, one game) eclecticlight.co observed:

- The E-cores belonged to the game, and low-QoS background threads stood still.
- The GPU ran at a clearly higher clock.

This applies to the Mac and only to full screen; for iPad and Apple TV it is not established. Our `lgp_game` under macOS
is not an app bundle; the switch affects nothing there.

On 2026-10-21 Paul decided to switch it on by default, without a prior measurement. A gain is
therefore not established. It could be measured with ABAB from two builds (off/on), `--clock host`, fps and frame time.
Checking whether it kicks in: according to reports, iOS shows a "Game Mode" notification at start (not checked).

Slim app (build tool `--game-separate`, `tools/kit/README.md`): the package has no `game/`, the game folder goes
into `Documents/game` once (Finder, Apple Devices app, Files app or `devicectl` as below). Before using a folder the
app compares the build IDs of `exefs/` with the lifted code and, with `-DLGP_APP_GAME_STAMP=<stamp>`, the folder's
`lgp-build-stamp.txt` with the build's stamp (`lgp::boot::game_fits_build`); an empty `Documents/game` counts as no
game. Checked on the iPad (2026-10-28): the slim app is 114 MB instead of 4.3 GB with the game inside, `devicectl device copy
to` takes the whole folder in one call (2.2 GB in 73 s), and the Finder accepts the whole folder `game` dragged onto
the app. The Apple Devices app on Windows is not checked yet.

## Onto the device (without tapping, via devicectl)

```sh
xcrun devicectl list devices                                   # device ID
xcrun devicectl device install app --device <ID> build/ios/runtime/boot/Release-iphoneos/lgp_game.app
# Game dump into the container (once; exefs/ one by one, romfs as one image):
xcrun devicectl device copy to --device <ID> --domain-type appDataContainer \
      --domain-identifier de.piefkepaul.lgp.game --source <dump>/romfs.bin --destination Documents/game/romfs.bin
xcrun devicectl device copy to … --source <dump>/exefs/main --destination Documents/game/exefs/main   # per module + main.npdm
xcrun devicectl device copy to … --source <dump>/systemdata/<id>/<datei> --destination Documents/game/systemdata/<id>/<datei>
# Start (options after --app as on the desktop; "--" separates them from devicectl's options,
# lgp_game itself discards a leading "--" in case devicectl passes it on):
xcrun devicectl device process launch --device <ID> --terminate-existing de.piefkepaul.lgp.game \
      -- --app --frames 600 --frame-stats 60
# Fetch the log:
xcrun devicectl device copy from --device <ID> --domain-type appDataContainer \
      --domain-identifier de.piefkepaul.lgp.game --source Documents/logs/lgp.log --destination ./lgp.log
```

`copy to` also takes whole folders (checked with Xcode 27.1; a 4.2 GiB `romfs.bin` took 146 s over USB). For
`process launch` the device must be unlocked and awake, otherwise the app does not come to the foreground. Pack the RomFS as `romfs.bin` (`lgp_romfs_pack` from the desktop tree): one large file goes
faster than thousands of small ones.

### Script runs (measurements without tapping)

For reproducible runs with an input script, `--no-movie-audio --audio none
--nvn-unimplemented continue` belong on the device (this way the script runs on the iPad Pro M1 went the same as on the Mac up to Route 1;
which of the three options alone is needed has not been checked individually). An example with the virtual clock:

```sh
xcrun devicectl device process launch --device <ID> --terminate-existing --console de.piefkepaul.lgp.game \
      -- --app --clock virtual --press "$(cat <skript>)" --frames 10500 --frame-stats 500 --time-stats 300 \
         --keyboard none --no-movie-audio --audio none --nvn-unimplemented continue --save run-saves-empty
```

Give `--save` as a relative path: it then applies below `Documents/`. The absolute container path
(`/private/var/mobile/Containers/Data/Application/<UUID>/…`) changes with every reinstallation; an old
absolute path makes the start fail with return value 3. Empty saves: empty the folder before the run or
take a new name.

ABAB measurements of an environment variable (host clock, each run with a fresh copy of a fixed save in
`Documents/abab-saves/…`, logs to `$LGP_PRIVATE_DIR`): `tools/apple/measure_device_abab.sh` with `VAR`, `ORDER` and
`PLATFORM=ios|tvos`, ready-configured `tools/apple/measure_ipad_decode_threads.sh` (BCn helpers 0/2/3) and
`measure_tvos_fif.sh`, for the iPad also `measure_ipad_fif.sh` (frames in flight 16 against 3, virtual clock,
empty save, Analysis 32) and `measure_ipad_host_block_file.sh` (`LGP_HOST_BLOCK_FILE` 0 against 1). The fill area of the
main target (#312, Analysis 41) is measured by `measure_device_fill.sh` on iPad and (with `PLATFORM=tvos`) Apple TV: A without the switch,
B `LGP_WEBGPU_FILL_SCALE=0.75` on 1280×720. A value `-` in `ORDER` passes the variable empty (label `off`).
Both variables go along via `--env` after `--app`; `settings.ini` stays unchanged. The script fetches it read-only
to `<OUT>/settings.ini` and aborts if its `args` already name the variable. On the iPad the script aborts before each run if `lgp_game` is already running (someone
is playing), and never ends a running game. It backs up the `lgp.log` from before to `<OUT>/before/`.

The whole iPad block is run by `APP=<lgp_game.app> tools/apple/run_ipad_block.sh <Gerät> <Spielstand> <leerer Ordner>
<vpress> [<press>]`. The order is BCn helpers, then frames in flight, then guest memory on a file, finally the
XCUITest proof from #193 (`UI_REPO`). `STEPS=fill` runs the fill area measurement (only with #312). Between the steps there are cooling pauses (`COOL`, default 900 s). Before each step
the script waits until `lgp_game` is no longer running (`WAIT_IDLE`, default 1800 s); after that it stops. It checks that the
repo contains #215, #271 and #272 and that the app carries a `pipeline-seed.lgppipes`. It installs the app beforehand and again after the
UI test as an update. It fetches the saves `Documents/saves`, `saves-2` … before and after and compares their MD5.
If they change, it ends with 4 and leaves the copy from before in `<OUT>/saves-before/`. `DRY_RUN=1` only checks and reads
only the process list from the device. Without `UI_REPO` it leaves out the UI step with a notice. An app in the background
also appears in the process list of `devicectl device info processes`; devicectl cannot tell the foreground apart.
Therefore `lgp_game` must be swiped away on the iPad before the block. Otherwise the script waits and then stops.
Checked on the Apple TV (read-only, 2026-10-15): `devicectl device copy from` takes a folder as `--source` and copies
it recursively, including the hidden `.lgp-*`. A missing source fails with error 7000 and creates nothing.

### Taps and screenshots (XCUITest)

The app draws the menu and the game image itself (Dear ImGui, WebGPU). So there are no controls that
XCUITest could find via accessibility. The steps hit the screen via normalized coordinates (0..1,
top left, the app runs in landscape). With `-DLGP_IOS_UI_TESTS=ON`, `lgp_ios_ui_tests()` (`cmake/LgpIos.cmake`) creates next to
`lgp_game` the bundle `lgp_game_uitests` (`platform/ios/uitests/LgpUITests.m`) and switches on the
Xcode schemes for both. It is started from the Mac:

```sh
cmake --preset ios -DLGP_IOS_TEAM=… -DLGP_GAME_LIFTED=… -DLGP_IOS_UI_TESTS=ON
tools/ios/uitest.sh build/ios <ID> $LGP_PRIVATE_DIR/uitest/<lauf> \
    'wait:20;shot:start;tap:0.5,0.42;wait:5;shot:after-tap;type:Red\n;wait:2;shot:name' '--app --menu on'
```

The steps: `wait:SEC`, `tap:X,Y`, `hold:X,Y,SEC`, `drag:X1,Y1,X2,Y2[,SEC]`, `type:TEXT` (`\n` = Return, needs
an open text input), `shot:NAME`, `home`, `activate`. Under tvOS (`cmake --preset tvos … -DLGP_IOS_UI_TESTS=ON`) there is no
tapping; there `remote:TASTE[,SEK]` presses a button of the Siri Remote (`up`, `down`, `left`, `right`, `select`, `menu`,
`playpause`, `home`) via `XCUIRemote`. These presses reach neither the GameController framework nor SDL (Apple TV,
tvOS 27.2); they are good for the system, not as input into the game. At the end `shot:end` always follows. `xcodebuild` passes
`TEST_RUNNER_LGP_UITEST_SCRIPT` and `TEST_RUNNER_LGP_UITEST_ARGS` on to the test as `LGP_UITEST_*`. The test splits the
start arguments at spaces, without quotes; paths with spaces do not work this way. The script exports the
images with `xcrun xcresulttool export attachments` together with `manifest.json` (name to file), even if the test fails,
and then ends with the code of `xcodebuild`. Images from the
game are game images and belong in `$LGP_PRIVATE_DIR`.

Checked on the Mac (2026-10-14): CMake's Xcode generator builds the bundle with `XCODE_PRODUCT_TYPE`
`com.apple.product-type.bundle.ui-testing` as a UI test including the runner app, and `xcodebuild test` leaves `Documents/` with
saves and `settings.ini` in place. Two pitfalls: `enable_language(OBJC)` only applies in the directory where it
stands, so the function sets `XCODE_EXPLICIT_FILE_TYPE sourcecode.c.objc` for `LgpUITests.m`, otherwise
Xcode compiles the file as C. And CMake does not enter an XCTest bundle into its own scheme, but under "Test" in the
scheme of its `XCTEST_TESTEE`. `uitest.sh` therefore starts the scheme `lgp_game` with `-only-testing lgp_game_uitests`
(suspicion, not yet checked on the device). Also open: whether `typeText` reaches SDL's hidden text input.
If the device reports "Timed out while enabling automation mode", it is usually locked (suspicion); under
Settings > Developer, "UI-Automatisierung aktivieren" (Enable UI Automation) must be on. `xcresulttool export attachments` exists from Xcode 16 on.
Paul keeps starting his own tap tests himself via the icon, not via this route.

If `platform/ios/Info.plist.in` changes, the next build reconfigures the project (`CMAKE_CONFIGURE_DEPENDS`).
Whether a key arrives in the app: `plutil -p build/ios/runtime/boot/Release-iphoneos/lgp_game.app/Info.plist`.

## Experience from the first device start (iPad Pro M1, iPadOS 27.2)

- First full build with 64 lifted translation units on an M1 with 8 GiB: 748 s wall time with `-jobs 2`; an
  incremental build after a change in the renderer 22 s.
- The signature carries both memory entitlements when `-allowProvisioningUpdates` runs along (set in the preset).
- wgpu-native first created the device with 16 KiB as the largest uniform binding: whoever requests no limits apparently gets
  wgpu's downlevel values there, not the WebGPU default values (inferred from the error message).
  Constant buffers of 64 KiB failed in `wgpuDeviceCreateBindGroup`, after which `wgpuQueueSubmit` aborted with a
  panic. The renderer therefore requests every limit at least up to the WebGPU default value, as far as the adapter
  has it (`platform/webgpu/src/webgpu_renderer.cpp`).
- wgpu-native does not offer `texture-compression-bc` on the iPad Pro M1: 258 BC textures (BC4, one BC5) were
  discarded as "not supported" at the first start. Since P7.3 the BCn CPU decoder (`bc_cpu`,
  `gpu/texture/src/bc.cpp`) decodes them on upload.
  Whether ASTC comes natively only shows once the game creates ASTC textures (Apple GPUs can do it).

## Open points and risks (suspicion, not checked)


- **Background:** iOS terminates apps that submit Metal work in the background. Since P7, `lgp_game` pauses the game when
  switching to the background: SDL's event watch on `SDL_EVENT_WILL_ENTER_BACKGROUND` (still runs in the
  app delegate) calls `SdlWindow::lifecycle_event(true)`, the boot program stops all guest threads
  (`boot::ProcessControl::pause`, at most 1 s, `Scheduler::pause`) and waits until the GPU is done
  (`WebGpuRenderer::wait_idle`). Under iOS/tvOS the surface is released only at `SDL_EVENT_DID_ENTER_BACKGROUND`: SDL reports
  `WILL_ENTER_BACKGROUND` already at `sceneWillResignActive` (a system dialog such as the "Lokales Netzwerk" (local network) prompt,
  Control Center, call banner), and according to Apple's docs iOS locks the GPU only in the background. Under Android it is
  still released already at `WILL_ENTER_BACKGROUND`. Under iOS/tvOS SDL reports every switch twice (scene delegate and
  `UIApplication` notifications). On the iPhone and iPad (test build 88c0513) the surface went away once more after the last
  `DID_ENTER_FOREGROUND`, and the image froze while the game kept running (log: endless
  `webgpu: no surface`). Suspicion: a late background notification. That is why `SdlWindow::pump_events` gets the surface back
  under iOS/tvOS on every pass as long as `UIApplication.applicationState` is not background
  (`uikit_app_in_background`), and logs this with `lgp: WebGPU surface back in the foreground`. The pipeline workers accept
  no job until the return (`WebGpuBackend::hold_prebuilds`), so that no CPU load remains in the background. `SDL_EVENT_DID_ENTER_FOREGROUND` resumes.
  Pauses are counted: every `pause()` needs its `resume()`, only the last one lets the game continue. This way the
  return to the foreground does not wake a game whose pause menu is still open.
  Log lines `app: background, game paused in N ms, K pipeline(s) still compiling` and `app: foreground, game continues`. Not yet
  checked on the device.
- **Guest memory on a file:** On the iPad the footprint up to frame 3000 is at 2505 MiB, on the Apple TV
  with a file-backed guest block at 1333 MiB (Analysis 28). The difference comes from the guest block being anonymous on iOS
  and Darwin not counting file-backed pages towards `phys_footprint`, which jetsam compares against the limit.
  `-DLGP_HOST_BLOCK_FILE_DEFAULT=ON` switches the file backing on by default on iOS as well (`LGP_HOST_BLOCK_FILE=0`
  switches it off for one run, `host_block.hpp`). For the measurement both variants are built from the same state; the
  distinction is in the log as `host block: guest memory backed by a file in …`. Footprint and
  fps are measured in ABAB alternation. The option becomes the default only with a measured gain. Also watch the write load: under
  memory pressure the kernel writes dirty pages into the file in the app's `tmp/` instead of terminating the app. That costs
  flash writes and time (suspicion, not measured). It helps mainly on devices with little RAM,
  where the jetsam limit is closer than on the iPad Pro M1.
- **Start time:** SDL calls the game only after the scene is connected, i.e. after `didFinishLaunching`. This
  should not trigger iOS's launch watchdog.
- **Resolution:** The surface has the full Retina resolution, on the iPad Pro 12.9″ that is 2732 × 2048. Whether
  upscaling to it costs frame rate is measured by P7.2d.
- **Bluetooth:** SDL's HIDAPI can touch CoreBluetooth. `NSBluetoothAlwaysUsageDescription` is therefore in the
  `Info.plist` so that the app does not crash when that happens.
