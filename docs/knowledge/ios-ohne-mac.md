# Building for iOS/iPadOS without a Mac (Linux, WSL)

As of 2026-10-25. **Draft**: The build with the real iPhoneOS SDK 27.1 runs through in WSL (section "What is
verified"); nothing has been installed or started on a device yet. The Mac way is described in
[`ios-app-bauen.md`](ios-app-bauen.md).

## Decision: everyone builds and signs themselves

Paul, 2026-10-25: Every friend builds the app from their own dump and signs it with their own Apple ID.
Finished builds are not distributed, not even via a private "store" (ad-hoc distribution with a paid
account remains just an idea). If the free account is not enough (memory entitlements, see below), iOS needs
a paid Apple developer account (99 €/year), and the guide says so; it is not worked around.

## Goal and approach

Friends without a Mac should be able to build the game for iPhone/iPad from their own dump (build tool
`tools/kit/build.py`, target `ios`). On Windows this runs in WSL (Ubuntu), because the SDK and linker only work on Linux.

1. **SDK:** Each user downloads `Xcode.xip` themselves with their Apple ID from developer.apple.com and unpacks it with
   [xtool](https://xtool.sh): `xtool sdk install Xcode.xip`. The SDK then lies in
   `~/.swiftpm/swift-sdks/darwin.artifactbundle`. The toolchain finds it there, or one sets `LGP_IOS_SDK`.
2. **Building:** Preset `ios-cross` (`cmake/toolchains/ios-cross.cmake`) with Ninja and host LLVM 19 or newer (the libc++ of
   SDK 27 uses `__builtin_clzg`, `__is_convertible` and `__GCC_DESTRUCTIVE_SIZE`; clang 18 failed on these, verified
   is clang 21): clang with `--target arm64-apple-ios16.0` and the SDK as sysroot, linked with `ld64.lld` (lld for
   Mach-O). clang passes `-platform_version` on to the linker only with `-fuse-ld=lld`, so both
   switches are in the linker flags (`-fuse-ld=lld --ld-path=<ld64.lld>`). On Debian/Ubuntu the tools are called
   `clang-21`, `ld64.lld-21` etc.; the toolchain takes the newest version from 19 on (`LGP_LLVM_SUFFIX=-21` pins one)
   and stops with a hint if clang is older. It makes three adjustments to the SDK itself:
   - **`.tbd` without arm64:** The text stubs of SDK 27 name almost only `arm64e` and `arm64e.x1` (778 of 817 entries
     without `arm64`). ld64 allows that, lld does not ("unknown architecture", and it only takes symbols of its own
     architecture). `tools/ios/patch_sdk.py ensure <sdk>` creates a copy once (`~/.cache/lgp-recomp/ios-sdk`,
     `LGP_IOS_SDK_CACHE`; files via hard link, only the `.tbd` new), in which every `arm64e`/`arm64e.x1` target has an
     `arm64` counterpart and `arm64e.x1` is dropped; this copy is then the sysroot. It contains Apple's SDK and
     stays on the machine. On the PC a hand-made copy with `arm64e.x1` → `arm64` ran; the automatic version
     additionally adds `arm64` next to `arm64e` (as ld64 allows) and has not yet run with the real SDK.
   - **`INFINITY`/`NAN`:** In module mode, the SDK's `math.h` gets them from Apple's `float.h` via
     `__need_infinity_nan`; upstream clang's `float.h` does not know that, and the Metal headers fail. Every
     C/C++/ObjC file therefore gets `-include cmake/toolchains/ios-cross-prelude.h` (defines both if they
     are missing; a `-D` with parentheses does not survive the shell of the sub-builds).
   - **libvpx:** Its Darwin targets call `xcrun`, hence target `arm64-linux-gcc` with the iOS clang including the SDK and
     `ld64.lld` in `CC`/`CXX`/`LD` (`cmake/LgpVpx.cmake`). The first PC run still took host `g++`/`ld` and stopped with
     "unable to link executables"; the fix has not run yet (way out `-DLGP_ENABLE_VPX=OFF`; then
     the videos stay black, iOS has no other decoder).
3. **App bundle:** Ninja creates a flat `.app`. There is no `actool`, so the icons go into the bundle as single
   PNGs (`tools/ios/loose_icons.py`, `CFBundleIcons` in the Info.plist). The keys that Xcode enters itself
   (`MinimumOSVersion`, `UIDeviceFamily`, `CFBundleSupportedPlatforms`, `DTPlatformName`,
   `UIRequiredDeviceCapabilities`) are set by `cmake/LgpIos.cmake` in cross mode. The build's entitlements are in
   `<build>/lgp_game.entitlements`.
4. **IPA:** `tools/ios/make_ipa.py` signs the program ad hoc with the entitlements and packs
   `Payload/lgp_game.app` into `work/ios/lgp_game.ipa`. This is necessary: xtool reads the entitlements **only from the
   program's signature** (`DeveloperServicesAddAppOperation`, `signer.analyze(executable:)`, xtool 7f7d5b7); without a
   signature it signs without entitlements, i.e. without `increased-memory-limit`. The signer is `rcodesign`
   (apple-codesign 0.29.0, MPL-2.0, static Linux build with a fixed SHA-256 in `build.py`,
   `build.py fetch-rcodesign` downloads it into the tool cache; `ldid` works too). Verified with a self-linked
   iOS program: `rcodesign print-signature-info` shows the entitlement as an XML and a DER blob.
5. **Signing and installing:** `xtool install work/ios/lgp_game.ipa`. xtool logs in with the Apple ID, fetches
   certificate and profile and installs via USB over usbmuxd. In WSL this needs usbipd-win so that the iPhone is visible
   in WSL. With a free Apple ID the signature is valid for 7 days, and at most 3 apps work at the same time.

   Since 2026-10-28 this is only `--installer xtool`. The default is `--installer sideload`: the user installs the
   `.ipa` with Impactor, SideStore or AltStore, without xtool's login and without usbipd
   ([`ios-sideloading.md`](ios-sideloading.md)).

On Windows the assistant does it (`lgp-bauen.cmd`, choice 3): It unpacks a dump while still on Windows (Node
from the ready-made package), offers to install WSL with Ubuntu if it is missing, and then starts itself
in WSL with the unpacked game (`LGP_KIT_TARGET`, `LGP_KIT_KIT` via `WSLENV`, path translated); there it checks
the Linux tools, offers `apt` packages, builds into `~/lgp-kit-build` and offers `xtool install`. If the
repo is on a Windows drive (`/mnt/…`), `build.py` first mirrors it to `~/lgp-recomp-kit` (without `.git`,
`build/`, `bauset/`; only changed files, `LGP_KIT_WSL_REPO`, off with `LGP_KIT_WSL_MIRROR=0`) and runs from there:
Via `/mnt` (9P) every file access is considerably slower; on the PC configuring there took over 40 min
instead of 73 s in the WSL-native clone, although slow downloads (14–85 KB/s) played a large part back then. Older
build folders from the `/mnt` repo are created anew by `build.py` (CMake rejects a different source folder). Paths that
one drags from Windows Explorer into the WSL window become `/mnt/<laufwerk>/…`. On the PC (2026-10-25) it recognized
keys, firmware and kit server, translated the paths (since 69e7f8f with `wsl.exe --exec`) and found the tools without
`LGP_LLVM_SUFFIX`.

`python3 tools/kit/build.py build --kit <dump.zip> --target ios` does steps 2–4 (lift as for the other targets),
`doctor --target ios` checks SDK, clang ≥ 19, `ld64.lld`, `llvm-ar`, `llvm-ranlib` and `rcodesign`; a missing xtool
is only a hint, because it is only needed for installing;
with `apt` it offers the newest `clang-N`/`lld-N`/`llvm-N` from 19 on that the package sources have.
On Windows itself the tool says that it has to run in WSL.

## Memory entitlements: the switch point

LGP needs a lot of memory on iOS (guest block 3456 MiB reserved, 1.3–2.6 GiB written). The Mac build signs
`increased-memory-limit` (higher jetsam limit) and `extended-virtual-addressing` (more address space).

**Mac test 2026-10-25 with a free Apple ID (Personal Team) in Xcode:**
- Both together fail: "Personal development teams … do not support the Extended Virtual Addressing capability."
- `increased-memory-limit` alone can be signed, the profile is valid for 7 days.
- A Personal Team needs a registered device; Xcode registered the iPad with
  `-allowProvisioningDeviceRegistration`. The profile has to be trusted once on the device (Settings → General →
  VPN & Device Management).
- **Provisionally confirmed (iPad, 2026-10-26, Mac build with a free Apple ID):** A game build with only
  `increased-memory-limit` (plus `team-identifier`, without iCloud) starts up to the title screen, 30 fps, footprint
  at most 1.7 GB, guest block 3.3 GB reserved without `extended-virtual-addressing`, no crash in 1 min. With
  a save: field loaded, 22 inputs arrived, ~30 fps, footprint peak 2.5 GB, no crash; field and walking
  only confirmed by log, not checked visually. Scenes with the highest demand (battles, long runs) and the iPhone
  not tested yet. After deleting the app,
  iPadOS asks for "Trust" again (Settings → General → VPN & Device Management).

Hence three levels, CMake `LGP_IOS_MEMORY_ENTITLEMENTS` or `build.py --memory-entitlements`:

| Level | Entitlements | When | Guest block |
|---|---|---|---|
| `both` | both | paid team (default on the Mac and with `--ios-team`) | anonymous |
| `limit` | only `increased-memory-limit` | free Apple ID (default without `--ios-team`, `FREE_TEAM_MEMORY_ENTITLEMENTS` in `build.py`) | anonymous |
| `none` | none | in case that fails too | from 768 MiB on a file as on the Apple TV (`LGP_HOST_BLOCK_FILE_DEFAULT`, `LGP_HOST_BLOCK_FILE_RANGE_DEFAULT=768:`) |

Why `limit` without a file could be enough (reasoned estimate, not measured): The address space without
`extended-virtual-addressing` is enough for the guest block. The iPad probe without entitlements reserved 3456 MiB in
one piece (8 GiB not, Analysis 25), and on the Apple TV the game runs without this entitlement. The jetsam limit, on
the other hand, is raised by `increased-memory-limit`, and that stays.

**Test outcomes:**
- **Measurement with `limit` is enough** (game runs into the scenes with the highest demand): `FREE_TEAM_MEMORY_ENTITLEMENTS
  = "limit"` stays, the free-team way is the default.
- **Measurement is not enough:** first check `none` with the guest block on a file (on the Apple TV that carries up to Route 1). If
  that is not enough either, iOS without a Mac needs a paid team: set `FREE_TEAM_MEMORY_ENTITLEMENTS` to `"both"`, and
  `doctor` should then stop without a paid team (not implemented yet).

## Gap in xtool and the patch (not compiled, not tried)

xtool reads the entitlements from the app, creates the matching capability on the App ID for known entitlements
and then fetches the profile (`DeveloperServicesAddAppOperation.swift`). It does not know `increased-memory-limit`: It
keeps the entitlement in the signature but creates no capability. **Suspicion:** The profile then does not contain it,
and iOS rejects the installation ("signed with invalid entitlements").

The Mac read the capability from Xcode's portal cache (`DVTPortalCachedPortalCapabilities.json`, 2026-10-25):
`INCREASED_MEMORY_LIMIT`, profile key `com.apple.developer.kernel.increased-memory-limit`, team types
`XCODE_FREE_PROGRAM`, `APPLE_DEVELOPER_PROGRAM`, `ENTERPRISE`, `UNIVERSITY`, SDKs iOS and visionOS,
`canRequestFromPortal: false`. `EXTENDED_VIRTUAL_ADDRESSING` exists only for `APPLE_DEVELOPER_PROGRAM` and
`ENTERPRISE`. The probe's free-team profile contained `increased-memory-limit = true`.

`tools/ios/xtool-increased-memory-limit.patch` (against xtool main 7f7d5b7, MIT) enters the entitlement as a known type
and maps it to `INCREASED_MEMORY_LIMIT`, also for free teams. Apply it in the xtool checkout with
`git apply <repo>/tools/ios/xtool-increased-memory-limit.patch`, then build xtool yourself (`swift build -c release`).
The patch is not compiled (no Swift here) and not tried. Above all, `canRequestFromPortal: false` is open:
Whether the Developer Services interface that xtool uses lets one create the capability,
I do not know; Xcode got it for the probe. If Apple rejects it, `none` remains.

## License and legal matters (honestly)

- **Xcode license:** Apple's license for Xcode and the SDK allows use only on Apple hardware. Using the SDK on
  Linux/WSL violates it. xtool changes nothing about that, it only makes it technically possible. Paul has
  decided to implement it anyway for this private project.
- **No passing on:** The SDK, `Xcode.xip` and everything derived from it never go into the repo, the ready-made package, CI or onto
  a server. Everyone downloads Xcode themselves with their own Apple ID and agrees to the license in doing so.
- **Login:** xtool logs in with a free Apple ID by password via non-public Apple interfaces
  (like Xcode itself). This is not documented by Apple and can break at any time; Apple could lock the account
  (suspicion, no known case checked). With the paid program it goes via an API key.
- **Game:** As with all targets, the finished package contains game data. It stays with the user and is never
  passed on.

## What is verified

- First run with the real SDK (PC Ghidra, WSL with Ubuntu 26.04, iPhoneOS 27.1, 2026-10-25, state 662518e) with
  stopgaps (clang 21, hand-made copy of the SDK with `arm64` `.tbd`, INFINITY/NAN header, without libvpx, xtool dummy):
  `build.py build --target ios` ran through in 8.5 min (tools 2.2 min, game folder 33 s, lift 23 s, game build
  5.3 min), `.app` 2.3 GB, `.ipa` 2206 MiB. Linked without undefined symbols; `LC_BUILD_VERSION` platform ios,
  sdk 27.1, minos 16.0; wgpu-native static; dylibs only libSystem, libobjc, libc++ and system frameworks (CloudKit,
  CoreHaptics weak). Entitlements file only with `increased-memory-limit`, but for lack of a signer **not** in the
  program (hence now `rcodesign`). The adjustments in step 2 come from this run.
- Run without stopgaps (PC, state 497852c): original SDK (copy via `patch_sdk.py`, 675 `.tbd`), libvpx on, only
  `LGP_LLVM_SUFFIX=-21` set. 9.7 min, without undefined symbols, `.ipa` 2207 MiB, `make_ipa` with `rcodesign`;
  `rcodesign print-signature-info` shows only `increased-memory-limit`. The PC's toolchain:
  `00-toolchain-pc-wsl.md`.
- Toolchain: clang 18 + `ld64.lld-18` link a Mach-O arm64 program for iOS with a self-built mini SDK;
  version check and SDK copy with a staged SDK (configuring).
- `patch_sdk.py`: YAML (v4) and JSON `.tbd` (v5), copy with symlinks, reuse (test `tools.make_ipa`).
- `LgpIos.cmake` in cross mode: a small test project yields a flat `.app` with icons, `Settings.bundle` and
  a valid Info.plist; entitlements for `both`, `limit`, `none`.
- `make_ipa.py`: structure of the IPA, file permissions, `ldid` call (test `tools.make_ipa`).
- `build.py`: plans for iOS without a Mac and the levels, clang check and apt offer (test `tools.kit_build`, dry run
  only).

## Unverified

- Linking the whole game with the real SDK, especially against wgpu-native's prebuilt iOS library
  (release asset `wgpu-ios-aarch64-release.zip`, without our patches; lazy IR is only optional on iOS anyway),
  and whether lld finds all symbols with the added `arm64` targets.
- libvpx with the corrected `CC`/`CXX`/`LD`.
- Whether iOS accepts the flat icons and the Info.plist without `actool`.
- xtool: login, signature with `increased-memory-limit` (see gap above), installation via usbipd in WSL.
- Other sideload tools (research 2026-10-25, not checked ourselves): only AltStore from 2.2 and LiveContainer keep
  `increased-memory-limit`, Sideloadly loses it.
- Precompiled Metal libraries do not exist without a Mac (no Metal toolchain on Linux; the cross build
  takes the official wgpu-native without the patch `metal-precompiled` anyway). The device compiles MSL itself at the first
  start, like every build without a metallib. On the Mac M1 this cost about 48 s of CPU cold on the pipeline threads, the
  first 1200 frames took 23.2 s instead of 13.6 s; from the second start on, Metal's own cache holds the results
  (11.3 s), Analysis 46. Not measured on the iPhone, probably slower.
- iCloud saves are off in the cross build (`LGP_IOS_CLOUD_SAVES=OFF` in the preset, a free Apple ID has no
  CloudKit). CloudKit is linked anyway (`platform/CMakeLists.txt` for all Apple mobile builds), but without
  `LGP_CLOUD_CONTAINER` the program never calls it (`runtime/boot/src/main.cpp`). Loading the framework needs
  no entitlement, so a crash because of it is not to be expected (read from the code, unverified on the device).

## Sources

- xtool: [xtool.sh](https://xtool.sh), source code github.com/xtool-org/xtool (MIT), `Documentation/Installation-Linux.md`,
  `Sources/XKit/DeveloperServices/App IDs/`.
- Apple: [Increased Memory Limit](https://developer.apple.com/documentation/bundleresources/entitlements/com.apple.developer.kernel.increased-memory-limit),
  [Extended Virtual Addressing](https://developer.apple.com/documentation/bundleresources/entitlements/com.apple.developer.kernel.extended-virtual-addressing).
- Measurements: Analysis 25, [`darwin-footprint-und-jetsam.md`](darwin-footprint-und-jetsam.md),
  [`tvos-als-zielplattform.md`](tvos-als-zielplattform.md).
