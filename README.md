<div align="center">

# lgp-recomp

**Pokémon Let's Go Pikachu as a native app: recompiled, not emulated.**
Windows, macOS, iPhone/iPad, Android, Apple TV and browser from the same translation, with touch controls,
our own menu, local trading and link battles over Wi-Fi, and optional real battles against wild Pokémon.

![Status](https://img.shields.io/badge/status-private%20learning%20project-orange)
![Platforms](https://img.shields.io/badge/platforms-Windows%20%7C%20macOS%20%7C%20iOS%20%7C%20Android%20%7C%20tvOS%20%7C%20Browser-lightgrey)
![Game version](https://img.shields.io/badge/game-1.0.0-green)
![Graphics](https://img.shields.io/badge/graphics-WebGPU%20(Vulkan%20%7C%20Metal%20%7C%20D3D12)-informational)

### [What runs](#what-runs) &nbsp;|&nbsp; [Extras](#what-has-been-added) &nbsp;|&nbsp; [Build it yourself](#build-it-yourself) &nbsp;|&nbsp; [FAQ](#faq)

</div>

> **This repository contains no game code, no game data, no keys and no firmware, and it never will.**
> You need your own, self-dumped copy of the game (version 1.0.0). Everything produced from it
> (translated code, shader caches, apps) is generated locally on your own computer and stays there. A
> pre-commit guard and CI check this on every commit.

This page as of: 2026-11-04.

## What is this about?

lgp-recomp translates the AArch64 machine code of *Pokémon: Let's Go, Pikachu!* (Nintendo Switch) into C++ **ahead of time**
and compiles it for the target device (**static recompilation**). While playing there is no CPU interpreter and
no JIT. What the console otherwise provides (memory, threads, file system, input, sound, videos, saves,
local communication) is re-created by the project's own runtime. The image is produced via **WebGPU**: natively via
wgpu-native on Vulkan, Metal or Direct3D 12, directly in the browser.

The result starts like a normal app: choose the game folder once, after that the game starts directly with the last
save (#496; a start screen with saves can be switched on), with settings and a pause menu.

## What it does differently

- **Native code instead of emulation.** The game code is already machine code of the device at startup. That is why
  iPhone, iPad and Apple TV work too, where apps are not allowed to use a JIT.
- **One translation for all devices.** The recompiler produces platform-neutral C++ that is only recompiled for
  each platform. Platform code sits behind interfaces (`platform/`); the runtime knows no operating system.
- **Graphics at the API level.** The console's graphics API (NVN) is mapped onto WebGPU, the GPU shaders (Maxwell)
  are translated to WGSL. A general-purpose emulator that re-creates the GPU in detail is more accurate in special cases.
  This route is smaller and can also be tuned for mobile devices and the browser.
- **Mods in the translated code.** Hooks on functions and patch sites on instructions allow changes to
  game behavior as normal C++ code, without binary-patching the game.
- **Every claim measured.** The numbers on this page come from measurement runs on our own devices.
  What is only suspected is marked as a suspicion.

## What runs

Legend: **playable** = played or checked via a playthrough sample (start, first gym, save/load);
**runs** = starts and can be played, but not smooth throughout or little tested; **experiment** = runs
on the device, but is not a full target. The original runs at 30 frames/s; more is not the goal here.

| Platform | Status | Frame rate (measured) | Notes |
|---|---|---|---|
| **Windows** | playable | 30 fps (game clock), unthrottled above that | Main development device, desktop playthrough test passed. An installation package is still missing. |
| **Browser** (Chrome, Safari on the Mac) | playable | 30 fps stable in Chrome on the PC | Milestone "playable in the browser" passed. Game data only from a locally chosen folder, never from a server. Needs WebGPU. |
| **iPhone / iPad** | playable | iPad Pro M1: 33–35 fps in game | Native app ([ADR-0007](docs/decisions/0007-ios-nativ-statt-wasm.md)), touch, device gyro, controllers, saves in iCloud. Played by Paul on iPhone 17 Pro Max and iPad Pro M1. Open: the gyro drifts when turning back and forth (correction built, #373, device test pending). |
| **macOS** (Apple Silicon) | runs | ≈ 30 fps, M1 CPU-bound | No dedicated playthrough test on the Mac. |
| **Android** (OnePlus 7 Pro) | runs, slow in heavy scenes | Mt. Moon ≈ 12 fps, gym ≈ 10 fps (same input route, 2026-10-22); in the intro and in calm scenes up to 30 fps | Limited on the GPU side, suspicion: waiting for write-backs from compute shaders. Several levers are built in; a fix for the cave and the gym is still missing. Earlier values around 20 fps mostly come from intro scenes. Image slightly darker than on the PC (open). Since #513 clears are folded and passes on the same target kept by default: gym 8.87 → 9.33 fps (+5 %, ABAB ×3). The pause of 2026-10-22 was lifted on 2026-10-25 so that friends with newer Android devices can test. |
| **Apple TV 4K** (A15, 4 GB) | runs (development only) | calm scene ≈ 29.5 fps, outdoors mostly 22–28 fps; long runs up to Route 1 at a median of 28.8 fps | Guest memory partly in a file, saves via iCloud. The loading stall of 7–24 s (page faults in the kernel) has had a remedy since #418 that is the default there: no stall in 3 runs instead of 5 out of 6 before (small sample). Since #317 the menu works with the Siri Remote and all controllers, without touch clicks; Paul confirmed the pause on the Apple TV. Since #518 texture writes go to the GPU in 64 MiB parts there: in the scene build the peak drops from 2061 to about 1790 MiB against a limit of about 2098 MiB. |
| **NVIDIA Shield** (Android TV) | experiment, paused | ≈ 15–22 fps depending on the run (intro/game start and the following scene, 2026-10-22); the Apple TV manages ≈ 29 fps on the same route | 3 GB RAM is tight. Paused since 2026-10-22, focus on Windows, macOS, iPhone and iPad. The 2019 "tube" model has 32-bit Android and cannot start the build. |

### Game features

What the game itself brings and how far it has been checked here. **checked** = played by Paul or passed in the
playthrough test; **works** = shown in test runs, but not played extensively; **not specifically tested**
= no known bug, but no evidence either.

| Feature | Description | Status |
|---|---|---|
| Game start | Title video with sound, language selection, controller selection, intro with Professor Oak | checked; since #520 the title video no longer waits 5 s before it starts (stall measured from the start: PC 6.65 → 1.8 s, Shield 9–11 → about 4.8 s). On the Shield about 4 s remain, partly opening the video decoder; #525 (reading the video file only as far as it plays) is merged; on the Shield it saves about 1 s when opening the file with a cold cache (title window 3.6 s vs 5.2 s on average over 5 runs, noisy because of the decoder setup) |
| Name entry | Names for player and rival via on-screen or system keyboard | checked |
| Overworld | Walking through cities, routes and Viridian Forest, conversations, cutscenes, light and shadow | checked up to after the first gym |
| Partner Pikachu | Pikachu on the shoulder, reactions in scenes | works |
| Catching wild Pokémon | Encounter on the map, catch minigame with berries and balls, throw via motion controls, stick or touch | checked |
| Trainer battles | Battles against trainers with moves, effects, experience and level-ups | checked |
| First gym | Gym battle against Brock in Pewter City, scene after the victory | checked |
| Pokédex | New entries after a catch, viewing entries and size data | checked |
| Party and Pokémon Box | Assembling the party, sending Pokémon to the box | checked |
| Saving and loading | Save in the game menu, restart, continue playing; a save from a real Switch also loads | checked |
| Music and sound | Music, effects, Pokémon cries | checked |
| Vibration | Rumble on controllers | works; manual check open |
| Motion controls | Aiming the throw with Joy-Con, controller gyro or device gyro | works; still drifts on the iPhone |
| Control styles | Handheld, single Joy-Con left or right; every controller is mapped onto them | checked |
| TV and handheld mode | the game renders in 1080p or 720p | works |
| Link battle | Battle against a second player via the game's communication feature | works between iPhone and iPad and between two instances on one PC; other device pairs untested |
| Trading | Pokémon trading via the communication feature | works between iPhone and iPad (Paul's test) and on the PC (two instances, same save); between two different trainers not yet shown |
| "Mit Pikachu spielen" (Play with Pikachu, X menu) | Petting and dressing up the partner | no longer crashes; whether one image in it is correct is open |
| Game progress after the first gym | further cities, gyms, Elite Four | only played in samples |
| Co-op with a second player | second player joins via Joy-Con, two trainers in the field | works on the PC (test run with input script, also with wild battles); untested on devices |
| Riding, surfing, flying | | not specifically tested |
| Shiny Pokémon | colored variants | open: once seen with normal coloring, cause unclear |
| Online, GO Park, Poké Ball Plus | Internet features, transfer from Pokémon GO, accessories | not supported (the game sees a console without Internet) |

## What has been added

Originally only the unmodified game was supposed to run in the browser. These features were added afterwards. Everything can be
switched off.

| Feature | Description | Status |
|---|---|---|
| Wild battles | Touching a wild Pokémon is followed by a real battle as in the main games, then the normal catch scene. The battle gives experience points like a trainer battle, with level-ups, new moves and evolution; the catch scene afterwards gives no second EXP. If you lose, you go to the Pokémon Center as in the game. Switchable instantly in the menu, off by default, link battles stay untouched. Needs a private mod file with game addresses, which the repo deliberately does not contain, and a lift with `--mods` | done (#382); defeat (#412), battle EXP and evolution (#429) as well as the fix for very early saves (#426) shown on the PC; on the iPad Paul confirmed the defeat with Pokémon Center and healing. Since #479 a defeat in our wild battles no longer costs money, and a defeated wild Pokémon plays its faint animation; Paul confirmed both on the iPad. The catch result view cosmetically shows "0 Erfahrungspunkte" (0 experience points) |
| Trading and link battles over Wi-Fi | between two devices with this port on the same network; not possible with a real Switch, which communicates by direct radio | done (#375); link battle and trading shown iPhone ↔ iPad and PC ↔ PC (two instances; link battle also between different trainers); other device pairs untested |
| Trading and link battles via your own server | the same feature over the Internet via a self-hosted RyuLDN server, either instead of or alongside Wi-Fi, also encrypted (`wss://`, #443); since #500 always on without a switch of its own as soon as the private mod file and a server are there (another server and the room can still be set); with that mod file the game's online button ("Weit entfernt", far away) goes through this server | built (#420, #427, #428, #455), online button shown on the PC, checked against the server without the game; not yet tested with the game over the Internet, #500 not yet tested on a device |
| Throw by swiping | Direction steers, speed determines the distance; off with a controller, can be switched on | done (#353, #380) |
| Touch controls | On-screen overlay, finger on the game's touchscreen, device gyro; overlay can be hidden, hides itself in "Mit Pikachu spielen" (Play with Pikachu) mode, L+R resets the gyro orientation, also in Joy-Con style, and is not passed on to the game | done (#329, #397, #413, #408); hiding not yet fully tuned on the device, L+R in Joy-Con style unchecked on the device; since #485 smaller buttons with labels and no drawn stick, look approved by Paul; since #535 the "Mit Pikachu spielen" scene is recognised by the game's own shader code, so shader translator changes no longer break the hiding (checked in the Android emulator, not yet on a device) |
| App menu | Start screen, saves, settings, pause in the look of the game; graphics and font from your own RomFS; own app icon on all platforms; on iPhone/iPad, settings that need a restart are in the iOS Settings app; English base text with German localization | done (#354, #414, #334, #428, #483); iOS Settings unchecked on the device; languages checked in the Android emulator and in Mac builds for macOS/iOS. Since #496 the start screen is off by default and the game starts with the last save; with a private mod file also without the controller screen (checked on the PC) |
| Switch TV/handheld | 1080p or 720p, also while the game is running | done (#352) |
| Graphics options | Light effects at half resolution, image format fit/fill/stretch, asynchronous execution | done (#370, #330, #366) |
| Graphics menu | Smoothing (off/soft/FXAA), light effects full/half, resolution 100/75/50 %, graphics thread; default smoothing off, 100 % | done (#386); on the OnePlus 75 % gave no measurable gain in game |
| Real widescreen | the image follows the screen: wider than 16:9 (iPhone, 21:9) more game world at the sides, narrower (iPad 4:3, 16:10) more at the top and bottom; menus, Pokédex and battle displays undistorted, backgrounds up to the edge. Handheld and TV mode, needs a private mod file | done (#364, #501, #542); since #542 the menu row "Picture format" offers Widescreen next to 16:9 with borders, fill and stretched, and widescreen also runs in TV mode (a switch between the modes applies from the next start); Paul's hand test on the iPad in TV mode passed (2026-11-02). Measured on the PC at 21:9, 16:10 and 4:3, confirmed by Paul on the iPhone. Since #501 header and footer groups of the game's layouts move to the new edges (iPad 4:3 more at the top and bottom, iPhone wider), with adjustable HUD bounds for the battle on a screen of their own (16:9 is the default and the smallest setting); Paul accepted this build in the game on the iPad and the iPhone (2026-10-30). Whether the two earlier iPhone gaps (text box in battle, shadow when petting) are closed was not checked separately. Known limit: the rounded ends of the bag's frame are stretched a little at 4:3 and on the iPhone (left as is for now) |
| Render scaling | Render targets smaller or larger than the original; the levels 75/50 % can be chosen in the graphics menu | prototype, default 100 % (#371) |
| Memory-saving shaders | Lazy IR: shaders only keep their text; ≈ 605 MiB (OnePlus) or 615 MiB (Shield) less Android memory (native heap), same image, fps unchanged | done (#383) |
| Fast first start | precompiled Metal shaders, pipeline cache also as a seed from another device; screen "Preparing graphics" before the game (#486), on by default on phones, tablets and Android TV | done (#331, #486); on the PC the preparation took ≈ 6 s and the game then built no more pipelines; not yet measured on the devices where it is on |
| Safe saves | Journal against data loss, export/import in the browser, iCloud shared between iPhone, iPad and Apple TV | done (#348); "iCloud-Stand laden" (load iCloud save) confirmed by Paul, later fixes still unchecked on the device |
| EXP as in Yellow | Setting in the "Spiel" (game) menu: experience points according to the Pokémon Yellow formula throughout the game, catching then gives no EXP; off by default. Like the wild battles, needs a private mod file | done (#440); wild battle, catch and trainer battle shown on the PC (#444), unchecked on the device |
| Random encounters | as in Red/Blue/Yellow: one switch "Pokémon Gelb" (Pokémon Yellow) hides all field Pokémon, rolls a battle per step only where Pokémon may appear, at Yellow's rate for the area, and starts it directly with a species and level from Yellow's table for that area. Off by default, needs a private mod file | done (#431, #457, #536, #546); the earlier version with hidden attackers was shown on the PC in long runs and confirmed by Paul on the iPad. Since #546 Yellow mode replaces the rows "Sichtbare Pokémon" and frequency; PC dry runs: no battles on paths, Route 3 7.5 per 100 steps (Yellow 7.81), Mt. Moon 4.0 (Yellow 3.91), three battles in a row without hangs; Paul's hand test on the iPhone passed (2026-11-02). Since #557 the hidden field Pokémon no longer show for about a second as a Yellow battle begins (PC: 3 runs, 5 battles). Not yet checked: the catch scene, room changes after a battle and switching it live. Since #553 a second row "Feld-Pokémon ausblenden" (hide field Pokémon) hides them without Yellow mode: no random battles, and running into a hidden one starts its battle as in Let's Go (checked on the PC in three steps with the menu, not yet on a device) |
| Data mods | replace individual RomFS files without a new lift; first mod "Begegnungen wie in Gelb" (encounters as in Yellow: species and frequency of wild Pokémon as in Pokémon Yellow) | interface done (#421); the Yellow tables have not yet been checked in the game, replacement files stay private |
| Game in the app package | a locally built app starts without file selection (except Android/browser) | done |
| Mod interface | Hooks and patch sites in the translated code, mods as C++; game addresses only in private files ([knowledge doc](docs/knowledge/spiel-mods-hooks.md)) | done (#356, #350) |
| Original code natively (experiment) | instead of translated C++, the rewritten ARM64 original code runs directly on Apple Silicon | experiment (#446, #456, #461): on the Mac a new game with virtual clock up to Route 1, frame-identical to the translated code at four sample frames; on the iPad Pro M1 a native test build (#521, not merged) with the same wgpu-native as the app ran faster than the translation: field 33.9 → 42.4 fps, battle 43.1 → 51.8 fps, memory about the same; on the iPhone about the same fps (~42) (one comparison, not yet repeated); on Android (OnePlus 7 Pro, test app, #480) the same image as the translation, same fps (GPU-bound), about 200 MiB less memory; no game build, the translation stays the default |
| Tools for tests | virtual guest clock, input scripts, frame checksums, time breakdown per thread | done |

## How it works

```
own dump --> tools/extract --> ExeFS (main, sdk, …) + RomFS
                      |
                      v
              recomp/  (lgp_lift)          AArch64 -> C++, function discovery, jump tables,
                      |                    indirect jumps via a dispatch table;
                      |                    checked against Unicorn, fuzzer and (Mac) Hypervisor.framework
                      v
              clang --> lgp_game (recompiled per platform)
                      |
                      v
        runtime/core   guest memory, threads, synchronization, SVCs
        runtime/hle    Horizon services: sm, fsp-srv (RomFS, saves), hid, audout/hwopus,
                       time, set, am/applets, vi, ldn (over LAN), offline console …
        runtime/boot   boot program, app mode, menus, in-game videos (movie::)
        gpu/nvn        NVN API -> command recording -> WebGPU
        gpu/shader     Maxwell shaders -> WGSL
        platform/      SDL3 (window, input, sound), browser (WebGPU, WebAudio, OPFS), iOS, Android, tvOS
```

- **Game code** is translated mechanically: completely and exactly, but not as readable source code.
- **System calls** are re-created, the way Wine re-creates Windows calls.
- **Graphics** is translated at the API level; shaders are translated at runtime and cached.

Fundamental decisions are recorded as ADRs in [`docs/decisions/`](docs/decisions/), knowledge about Switch internals in
[`docs/knowledge/`](docs/knowledge/).

## Build it yourself

> **Step by step for Windows and macOS, with screenshots: [`tools/kit/GUIDE.md`](tools/kit/GUIDE.md).**
> The ready-made packages are on the releases page.

You need **your own unpacked copy of the base game (either edition, version 1.0.0 or 1.0.2)**: a folder with
`exefs/` and `romfs/` (or `romfs.bin`). There is no one-click installer; building is done with CMake as below, or with
the build assistant `python3 tools/kit/build.py` (`lgp-bauen.cmd` or `lgp-bauen.command`), which takes that folder plus
a folder with `game.cfg` and the mods (`build.py build --kit <game folder> --kit-files <folder>`).

> **Decryption is not included.** For legal reasons this repository contains no tool and no instructions to decrypt
> or unpack game dumps. Everything here starts from a game folder that is already unpacked.

### 1. Prerequisites

CMake ≥ 3.25, Ninja, clang/LLVM, Python ≥ 3.9, git. Under Windows, **clang** instead of MSVC for the game: only
clang guarantees the tail calls of the indirect jumps. In addition a private folder **outside** the repo, e.g.
`export LGP_PRIVATE_DIR=$HOME/lgp-private`, with the unpacked game in `$LGP_PRIVATE_DIR/extracted/` (`exefs/`, `romfs/`).

### 2. Build and test the repo (without game data)

```sh
git clone https://github.com/PiefkePaul/lgp-recomp.git && cd lgp-recomp
cmake --preset default          # also activates the guard hook
cmake --build --preset default
ctest --preset default
```

This builds the recompiler, runtime, renderer and all tests with our own homebrew test programs.

### 3. Translate and build the game

```sh
cmake --preset release -B build/game -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build/game --target lgp_lift
mkdir -p $LGP_PRIVATE_DIR/build/lifted
build/game/recomp/codegen/lgp_lift $LGP_PRIVATE_DIR/extracted/exefs/main $LGP_PRIVATE_DIR/extracted/exefs/sdk \
    -o $LGP_PRIVATE_DIR/build/lifted/lifted.cpp --split 64 --summary
cmake --preset release -B build/game -DLGP_GAME_LIFTED=$LGP_PRIVATE_DIR/build/lifted
cmake --build build/game --target lgp_game
```

Compiling needs a lot of RAM; if short on it, use fewer jobs (`-j`) or more parts (`--split 128`).

### 4. Starting

`lgp_game` without arguments (double-click) asks for the game folder on first start (with `exefs/` and `romfs/` or `romfs.bin`) and
then shows the app menu. Settings and saves live in `%APPDATA%\lgp-recomp`,
`~/Library/Application Support/lgp-recomp` or, under Linux, `~/.config/lgp-recomp` (settings) and
`~/.local/share/lgp-recomp` (saves). All options and `settings.ini`:
[`docs/knowledge/desktop-app-huelle.md`](docs/knowledge/desktop-app-huelle.md).

### 5. Other platforms

All use the same translation from step 3.

| Target | Built on | Instructions |
|---|---|---|
| Browser | Windows/Linux/Mac with Emscripten | Analysis 13, `browser-frontend.md`; serve locally only |
| iPhone / iPad | Mac with Xcode and your own Apple developer team | [`ios-app-bauen.md`](docs/knowledge/ios-app-bauen.md) |
| Android | PC or Mac with Android NDK and SDK | [`android-ndk-build.md`](docs/knowledge/android-ndk-build.md) |
| Apple TV | Mac, build wgpu-native yourself first | [`tvos-als-zielplattform.md`](docs/knowledge/tvos-als-zielplattform.md), `tvos.md` |

## Roadmap

| # | Goal | Status |
|---|---|---|
| 1 | Polish Windows, macOS, iPhone and iPad | in progress (focus since 2026-10-22) |
| 1b | Android and Shield stable outdoors at 30 fps | paused |
| 2 | Finish real widescreen (frames, edge elements) | almost done: merged and accepted on the iPad and the iPhone (#364, #501); two earlier iPhone edge elements not checked separately |
| 3 | Encounters as in Yellow, invisible attackers in grass and caves | in progress: invisible attackers built (#431, #457), Yellow tables still unchecked |
| 4 | Installation packages for Windows and macOS | partly: build assistant and Windows build set without game data (#465, #471); ready-made packages with the game deliberately do not exist |
| 5 | Play through the whole game and collect bugs | planned |
| 6 | Ball throw in battle for wild battles | largely done: throwing a ball from the battle bag with the game's catch formula, all ball types, Pokédex and box (#432); repeated catches, also after a restart, run clean on the PC; on the iPad (2026-10-25) "already better", but opening/absorbing and breaking out still show the Let's Go sphere instead of the ball animation; since #502 a Pokémon caught in battle keeps the ball it was caught with, accepted by Paul on the iPad (2026-10-29). Since #558 two menu rows: "Fangen im Kampf" (catch in battle, from the next start) and "Fangart im Kampf" (catch style in battle: Let's Go or Klassisch/classic, only with the first row on). With Let's Go a ball chosen in the battle menu ends the battle and the Let's Go catch scene follows; on the PC (Pikachu 1.0.0) the catch worked, the ball was not used up and the classic style ran as before; the battle ends with the normal flight text, and whether the field Pokémon goes away afterwards without Yellow mode is not checked yet |
| 7 | Test trading and link battles with the game via your own server | planned |

The complete roadmap with all phases: [`docs/roadmap.md`](docs/roadmap.md).

## FAQ

**Is this legal?** The repository contains no game code, no data, keys or firmware and does not help
obtain them. Building is done from a copy you own yourself and have dumped yourself. Finished builds contain
translated game code and are therefore only built locally, not distributed and not hosted. This is not
legal advice.

**Is the game included?** No. Neither game nor keys nor firmware, and no links to them either.

**Do I need a Switch?** For your own dump, yes: your own console and your own copy of the game. The project does not
take this step off anyone's hands.

**Which game version?** Only the base game 1.0.0; do not install an update NSP. With update 1.0.2 the
translation, the game configuration and the addresses of the mods and patches do not fit. Offline, 1.0.2 brings hardly anything visible
. Since #545 a tool carries the addresses over to Pikachu 1.0.2 and to Eevee 1.0.0 and 1.0.2, and the build tool accepts these builds; on the PC Eevee 1.0.0 has run, and hiding and showing the field Pokémon was checked there (#549–#551); Pikachu 1.0.2 and Eevee 1.0.2 have so far only started up to the first room. Widescreen ran on Eevee 1.0.0 in a test run (handheld and TV), and the kit now has widescreen files for these builds too, carried over by the tool; for 1.0.2 a script run on the device found them active in the field (Eevee on the iPad and the iPhone, Pikachu on the iPhone), Paul's hand test is still to come; Yellow mode is not yet confirmed there. Pikachu 1.0.0 therefore remains the supported version.

**Which device do I need?** For desktop a GPU with Vulkan, Metal or D3D12; checked are a Windows PC with an
NVIDIA GPU and a MacBook Air M1. On mobile, checked are an iPad Pro M1, an iPhone 17 Pro Max and a
OnePlus 7 Pro (Snapdragon 855; runs, but not smoothly everywhere). Older devices are untested.

**Can I trade or battle with a real Switch?** No. For local communication the Switch uses direct radio,
not a Wi-Fi network. It works between two devices with this port; shown are
iPhone ↔ iPad and two instances on one PC.

**Can I use my save from the Switch?** Yes, a save backed up from your own console with a backup homebrew
loads (checked with a save from a second Switch). It belongs in the app's save folder.

**Is this faster than an emulator?** There is no comparison measurement for that. Only what is in the table has been measured.

## How it came about

The project is a private learning and technology project and is being created **with AI assistance**: a large part of the code
is written with Claude coding agents. Paul decides what is tried; he tests on his own devices and
approves results. Two rules keep it honest: every claim comes from a test or a measurement, and nothing
that comes from the game goes into the repository. It is not a professional reverse engineering project; rough edges are
to be expected.

## Thanks

Unicorn, Ryujinx and the yuzu successors (as reference only),
N64Recomp and XenonRecomp, envytools and Mesa NAK (Maxwell shaders), deko3d and uam-nvn, ARM astcenc, wgpu and
wgpu-native, SDL3, Dear ImGui, libopus, libvpx, libvorbis and zstd, among others, served as a model, reference or building block. Adopted third-party code and all dependencies with
licenses: [`THIRD_PARTY.md`](THIRD_PARTY.md).

## Legal

lgp-recomp is not affiliated with Nintendo, Game Freak, Creatures Inc. or The Pokémon Company and is
neither endorsed nor sponsored by them. *Pokémon* and *Nintendo Switch* are trademarks of their owners and are only
mentioned to say what the software works with. No logos or graphics of the game are used. No license has been chosen for the
repository yet.

Project rules and target picture: `docs/project-setup/`.
