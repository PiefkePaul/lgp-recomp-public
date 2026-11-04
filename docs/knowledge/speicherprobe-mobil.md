# Memory probe for mobile devices (iOS/iPadOS, Android)

Cross-game knowledge and instructions (status 2026-10-13, P7.2a/P7.3a). Background:
[`ios-als-zielplattform-fuer-switch-recomp.md`](ios-als-zielplattform-fuer-switch-recomp.md),
[`native-apps-fuer-switch-recomp.md`](native-apps-fuer-switch-recomp.md). Measurement results for LGP belong in
`docs/analysis/`.

## What the probe measures

`tools/memprobe/` (no game code, may be built anywhere):

1. Reserves the guest block with exactly the runtime's code (`lgp::platform::HostBlock`, 4 GiB `PROT_NONE`) and
   tentatively 8, 16, 32 and 64 GiB of address space. Without `extended-virtual-addressing` the address space of an iOS app
   is limited (suspicion: that is why even the 4 GiB reservation can fail, not checked).
2. Writes the block in 64 MiB steps with random data, then further memory outside the block up to
   16 GiB. Random data because iOS compresses memory: zeros or a fixed pattern would hardly count. For
   comparison there is the fill "zero".
3. Writes a line after every step: written MiB, footprint of the process (Apple `phys_footprint`, otherwise
   RSS) and the remainder according to the system (iOS `os_proc_available_memory()`, Linux/Android `MemAvailable`). The line is
   written to disk immediately; after the system kills the process, the last line shows how far it got.

Assessment for a game: the guest block must be reservable, and the game's demand (for LGP 1.3–2.6 GiB of
actually written guest memory, plus GPU buffers and textures in the same memory) must stay below the last line,
with headroom.

## iOS/iPadOS (Mac with Xcode)

```sh
cmake -S tools/memprobe -B build/memprobe-ios -G Xcode -DCMAKE_SYSTEM_NAME=iOS \
      -DCMAKE_OSX_DEPLOYMENT_TARGET=16.0 -DLGP_IOS_TEAM=<Team-ID>
open build/memprobe-ios/lgp_memprobe.xcodeproj
```

Two apps: `lgp_memprobe_entitled` (with `increased-memory-limit` and `extended-virtual-addressing`) and
`lgp_memprobe_plain` (without). In Xcode choose the device, run the target, tap "Start" in the app. After the kill, open
the app again: it shows the log of the previous run (also in the Files app under "On My iPad" →
app folder → `memprobe.log`, or via Xcode → Devices → download container).

Without tapping (runs from the Mac):
`xcrun devicectl device process launch --device <Geräte-ID> de.piefkepaul.lgp.memprobe.entitled -- --autostart [--zero]`
(`--` separates the app's arguments from those of devicectl. With Xcode 27.1 it also worked without `--`; devicectl passed `--autostart` after the bundle ID on to the app. The probe finds its switches at any position),
alternatively the environment variables `LGP_MEMPROBE_AUTOSTART=1` or `LGP_MEMPROBE_ZERO=1`. The screen must
be unlocked and the app must stay in the foreground.

If the app starts and quits immediately with "UIScene life cycle is required for apps built with this SDK",
the scene life cycle is missing: with current SDKs every UIKit app needs a `UIWindowSceneDelegate` that creates the window in
`scene:willConnectToSession:options:`, and `UIApplicationSceneManifest` in the `Info.plist`. An
AppDelegate with its own `UIWindow` is no longer enough (also applies to the later game app).

Team ID: Xcode → Settings → Accounts → Team; with a free Apple account it is called "Personal Team".

**Entitlements and account:** Apple requires the provisioning profile to allow every entitlement; the entry
in the `.entitlements` file alone is not enough ([Apple forum](https://developer.apple.com/forums/thread/704945)).
With a paid Apple Developer account Xcode signs both entitlements without further ado (checked on the iPad Pro M1,
2026-10). Whether a free "Personal Team" gets `increased-memory-limit` is contradictorily documented: one
SideStore report says yes ([SideStore #1616](https://github.com/SideStore/SideStore/issues/1616)), other
developer notes say no; for `extended-virtual-addressing` there is no evidence at all. It can be checked with
`codesign -d --entitlements - <App>.app`. Free profiles expire after 7 days (commonly known, not checked).

**What the entitlements do (measured on the iPad Pro M1, iPadOS 27.2, 8 GB class):**

| | with both entitlements | without |
|---|---|---|
| `os_proc_available_memory()` at start | 8,182 MiB | 5,110 MiB |
| Reserving 8–64 GiB address space | works | fails already at 8 GiB |
| Reserving 4 GiB (guest block) | works | works |
| jetsam at footprint | ≈ 7.8 GiB | ≈ 5.1 GiB |

Conclusion for recompilations with a large guest block: without `extended-virtual-addressing` only little address space
beyond 4 GiB remains; a block with guard zones or a second large block does not fit. Without `increased-memory-limit` the
limit drops by about 3 GiB. Both argue for always setting the entitlements (assessment; the measurements of the
respective game decide). Results per game and device belong in `docs/analysis/`.

## tvOS (Mac with Xcode)

```sh
cmake -S tools/memprobe -B build/memprobe-tvos -G Xcode -DCMAKE_SYSTEM_NAME=tvOS \
      -DCMAKE_OSX_DEPLOYMENT_TARGET=17.0 -DLGP_IOS_TEAM=<Team-ID>
```

Three apps. `lgp_memprobe_entitled` here only has `extended-virtual-addressing`, because Apple does not offer
`increased-memory-limit` for tvOS (`ios/memprobe-tvos.entitlements`); `lgp_memprobe_imlimit` tries
it alone anyway (question: does the profile sign it, does the limit move?). The log is in
`Library/Caches/memprobe.log`, because `Documents` accepts nothing on the Apple TV. Fetch from the Mac (suspicion, not
tried on tvOS):
`xcrun devicectl device copy from --device <Geräte-ID> --domain-type appDataContainer --domain-identifier de.piefkepaul.lgp.memprobe.entitled --source Library/Caches/memprobe.log --destination memprobe.log`.
Start without the remote as on iOS with `--autostart`. The TV must be on and the app must stay in the
foreground. Measured on the Apple TV 4K (3rd gen): jetsam at ≈ 2057 MiB footprint (`plain`); the `entitled` app dies after
≈ 30 s without a log.

**Fill kinds** (all apps, iOS and tvOS; in the CLI `--zero`, `--file DIR`): `random` (default), `zero` (zeros; according to XNU
compressed pages also count fully in the footprint, both should die equally early) and `file`: the guest block is
a shared mapping (`MAP_SHARED`) of a sparse, immediately deleted file in `Library/Caches`, filled
with random data. Darwin counts such pages as "external", not in `phys_footprint`. If `file` gets clearly beyond
the anonymous limit, a file as backing for the guest block is worthwhile. Start via
`-- --autostart --file [--file-sync]` (`--file-sync`: `msync(MS_ASYNC)` after every block). The file needs as much
free flash as is written (up to 3.4 GiB per run) and disappears with the process.

**First touch** (`--touch MIB`, in the apps `-- --touch 512` or `LGP_MEMPROBE_TOUCH=512`, both start
the run without tapping): instead of the fill, measures
how long the first write (`memset`, like a game clearing new heap) of MIB MiB
takes, in four kinds: anonymous, sparse file (`MAP_SHARED`, like the guest block on tvOS), file written beforehand via
`pwrite`, and file allocated beforehand with `F_PREALLOCATE` (Linux `posix_fallocate`). Per kind one line
`touch …: prepare … s, first write … s (… MiB/s, slowest … chunk …), second write …`. The files are in
`Library/Caches` (CLI: `--file DIR`, otherwise `$TMPDIR`). The reason is the loading stall on the Apple TV
(`docs/analysis/42-praesent-stillstand.md`). Caution: with "written first" the pages are usually still in the page cache
after the `pwrite`. So the line measures pre-writing plus cache, not only the allocated blocks.
Runs started without tapping (`--autostart`, `--touch`) quit the app by themselves at the end, so that
`devicectl … --console` returns.

**Native probe** (`--native [--native-gib GIB]`, in the apps `-- --native` or `LGP_MEMPROBE_NATIVE=1`, starts without
tapping): the question of `docs/analysis/56` stage F, whether original code can run at a fixed guest address on the
device. Reserves GIB GiB (default 8) at bases below 2^39 (`0x70_0000_0000` down to `0x2_0000_0000`, one line each), then
puts a 16 KiB page of the probe's own code at the first base that worked and calls it, in three ways:
- **vm_remap** (Apple only): the signed text page itself, shared, not copied. This is the iOS/tvOS route, because
  there an app may not make its own memory executable. Line `remapped code: runs, result ok`.
- **from a file** (`mmap` with `PROT_EXEC`, the Android route; the file is in `Library/Caches` or `--file DIR`):
  line `file-mapped code: …`.
- **anonymous memory made executable** (what `--native` does on the Mac today): on iOS/tvOS expected `refused`.

On Linux/Android arm64 one more line: `signal with a foreign x18`. Guest code owns `x18` between gates, while Android
may use it as the shadow call stack (ShadowCallStack) of bionic. The probe sends itself a signal straight from
assembly with `x18` pointing at an unmapped address. `handled, process alive` means the handler and `sigreturn` do
not touch `x18`; a process that dies after `signal with a foreign x18 ...` does (then the native start needs `x18`
restored in its signal path first).

If the system kills the app for unsigned code, the log ends after a line `calling …`; that route is then
closed. Without `extended-virtual-addressing` the reservations above 4 GiB probably fail (suspicion from the
table above, not checked for fixed addresses). Checked in CI on Linux x86 and under qemu-aarch64 (route
"from a file" and "anonymous"), not yet on an Apple device.

Measured on 2026-10-28, OnePlus 7 Pro (GM1913, Android 12, kernel 4.14), as a shell process (`adb shell`):
- 8 GiB were reserved at every base except `0x60_0000_0000`, and the probe used `0x70_0000_0000`.
- The file and anonymous routes both run, `result ok`.
- `signal with a foreign x18: handled, process alive`.

So the shell domain allows both routes. An app runs under a stricter SELinux domain (`untrusted_app`), so whether
an app may do the same is still open; only an APK run checks it.

## Android (PC with NDK and adb)

```sh
cmake -S tools/memprobe -B build/memprobe-android -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake \
      -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 -DANDROID_STL=c++_static
cmake --build build/memprobe-android
adb push build/memprobe-android/lgp_memprobe /data/local/tmp/
adb shell /data/local/tmp/lgp_memprobe --log /data/local/tmp/memprobe.log
adb shell cat /data/local/tmp/memprobe.log
```

Limitation: this is a shell process, not an app. The low-memory killer rates foreground apps differently
(`oom_score_adj`), so the limit of a real app may differ. The number is a first orientation; the
APK shell (P7.3b) will measure later as an app.
