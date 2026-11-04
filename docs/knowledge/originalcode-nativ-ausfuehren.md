# Running the original code natively (ARM64 host)

Cross-game knowledge about the second executor next to the lifter: Switch code (A64, EL0) runs directly on the CPU of an ARM64 host after a
rewrite. The plan and measurements for LGP are in
Analysis 56, the rewriter in `lgp_recomp native-rewrite`
(`recomp/core/src/native_image.cpp`), the executor in `runtime/native/` (`lgp/runtime/native.hpp`).

## What the host has to provide

- **Guest address = host address.** The guest block lies at the address where the layout begins
  (`GuestMemory::Options::at_guest_address`, `HostBlock(size, at)`). The layout stays smaller than 4 GiB, but may
  lie above 4 GiB (iOS: `__PAGEZERO` occupies the lower 4 GiB). On Linux `MAP_FIXED_NOREPLACE` enforces the
  address; on macOS it is only a hint; if the system returns a different one, the start aborts.
- **Executable text.** `HostBlock::make_executable` switches whole host pages to R+X and flushes the caches. W^X
  means: a page is either writable or executable. With 16 KiB pages (Apple silicon) the last
  page of the text must therefore not be a page of `.data`; `.rodata` lies in between and only needs to be readable.
- **SVC, TPIDR, counters** go through the gate in the patch area (contract `lgp/recomp/native_gate.hpp`). The
  gate's host function (`runtime/native/src/native.cpp`) fetches the thread context, sets `x18` and SP to the
  host's values and calls the dispatcher: SVCs go to `Kernel::svc` as from lifted code, `TPIDR_EL0`/`TPIDRRO_EL0` come from
  the thread's `CpuState`, the counters from `read_sysreg`.
- **Entry and return.** `native::run(CpuState*)` has the shape of a lifted function; the scheduler starts
  native threads via the indirect resolver, like Unicorn in the oracle. The entry saves the host's callee-saved registers,
  loads all guest registers and jumps; `x30` points to a return stub that writes the state back.
  If the thread or the process ends in an SVC, the dispatcher jumps back to `run()` via `longjmp`.
- **Signals** run on guest threads on a separate stack (`platform::ensure_signal_stack`), because the
  rewriter stores values below SP. The handler redirects faults in guest code (PC in guest memory) to the
  host stack instead of jumping out of the handler via `siglongjmp`: it rewrites PC, SP, `x0` and `x18` in the
  signal context, and after returning from the handler the thread continues in a host function that
  terminates it. This way the kernel restores the signal mask itself. Threads that compute without SVCs are pulled out at
  process exit by a kick signal (SIGURG); during a pause the handler parks them until resumption. A kick
  that hits a thread between the gate and guest code is lost, so a kicker thread repeats it until
  no thread is in guest code anymore.
- **On macOS** `ucontext_t` comes from `<sys/ucontext.h>`; `<ucontext.h>` requires `_XOPEN_SOURCE` there.
- **Addresses.** macOS 26 on arm64 rejects fixed mappings at 0x10_0000_0000–0x6f_ffff_ffff (`EPERM`, measured on the M1);
  0x80_0000_0000 is free there and on Linux. For SDK titles, however, the layout must end below 2^39: the
  address fields that the SDK writes into IPC buffer descriptors have 39 bits; above that, services see truncated
  addresses (LGP aborted at 0x80_0000_0000 with 0xE601 InvalidPointer, suspicion, to be confirmed on the Mac). The native
  start uses 0x70_0000_0000.

## Host functions at guest addresses (SDK start)

The boot program places host functions at guest addresses: boot thunks (`nn::init::Start` callbacks), NVN procs,
import shims and replaced SDK functions (`nvnBootstrapLoader`, graphics stops). Lifted code finds them in the
dispatch table; native code simply jumps there via `BL`/`BLR`. That is why every such address holds a `B` to
a host call stub (`build_native_host_calls`). The stub calls the gate with the instruction word `HLT #0x4c47`
(`native_gate::kHostCallWord`, does not occur in application code), the dispatcher runs the registered function on
the `CpuState` and the stub returns to `x30` via `RET`. The key pages of the boot thunks are mapped
R+X for this; the argument area of `nn::init::Start` gets its own unmapped page.

- **Guest code called from within the gate** (a host function or SVC calls guest code, nested `run()`) starts below
  the gate frame: the frame lies below the guest SP and would otherwise be overwritten (seen as a jump to 0).
- **FPCR:** host code behind the gate runs with the host's FPCR; the guest's waits in `guest_fpcr`.
- **Modules on 16 KiB boundaries:** the native start links with 16 KiB pages (`LinkOptions::page_size`). Within
  a module the NSO determines the segment boundaries; if `.data` shares a 16 KiB page with `.text`, the start aborts.
- **Which modules:** without lifted code nobody names the modules, so the native start takes the ExeFS. A module whose
  exports are replaced by an HLE stays out, as with the lift; otherwise the imports end up in the original code instead of the HLE.
  Seen with LGP: with `subsdk1` (multimedia behind `movie::`) the `movie::` imports went into the real module, the log
  showed no movie-HLE line, and a new game crashed in the intro (frame ≈ 300) with a null pointer on this
  path.
- **`game.cfg`** names addresses for the lift base; the native start moves them to its own base
  (`rebase_config`).

## x18

The rewriter renames `x18` in guest code or spills it to the stack. Sites it cannot rewrite
(e.g. libnx's exception entry, which only saves and reloads the caller's `x18`) run with the host's `x18`.
On Linux and macOS this is harmless (Apple: reserved, the kernel may zero it). Android uses `x18`
for the Shadow Call Stack; there this is open (stage F).

## Scheduler

Native code has no back-edge polls. Code that only switches at SVCs (the fixtures) runs deterministically with `CoreBaton`.
Code that waits for another thread without an SVC needs `Free` (checked for LGP on the PC, A1).
Exception: if the wait loop reads the counter (`MRS CNTVCT/CNTPCT`, goes through the gate), every 64th read calls
`backedge_poll()` and, under the simple baton (`--sched baton`, set by `--clock virtual`), gives up the slot,
like lifted back edges. Test `native.signals` ("baton spin-wait reading the counter"). For loops without a gate
(LGP in nn::init under `--sched baton`: call via pointer, then a flag byte, no SVC, no counter; Mac, run 5) the
kicker thread ends time slices: a thread with slots (Baton, CoreBaton) that is found in guest code without a
gate pass on two scans 2 ms apart gets a kick, and the handler does what a back-edge poll does (Baton: `yield_hint`,
CoreBaton: `preempt_poll`). Test "baton spin-wait without SVC (time slice)". This is not deterministic: runs with
`--clock virtual` are not bit-exactly repeatable natively.

Closing the window (also SIGTERM, which SDL turns into a quit event) ends the run only at the next present.
If none comes (hang before the first frame), the boot program stops the guest after 5 s (`ProcessControl::stop`, line
`app: no present within 5 s of closing, stopping the guest`). This also applies to the lifted build.

## System time

On the Mac (LGP, town, unthrottled) the guest code needs 40–50 % less time natively, but the system time is about 33 %
above the lift, even when `LGP_PARK_SPIN_US=50` eliminates sleeping and waking. To narrow it down, `--time-stats` reports
per window the system calls, Mach calls and context switches (`host cpu`) as well as the entries into guest code and the
gate passes (`native`). The entry no longer saves the signal mask (`sigsetjmp(…, 0)`, otherwise one
`sigprocmask` per entry). For comparison, `LGP_NATIVE_SAVE_MASK=1` sets the old path. Whether this explains the system time
has not been measured yet.

## Android (stage F)

The memory probe in a shell process on a OnePlus 7 Pro (Android 12) ran code mapped from a file and anonymous code
made executable, and a signal with a foreign `x18` did no harm (`docs/knowledge/speicherprobe-mobil.md`). An app runs
under a stricter SELinux domain (`untrusted_app`), and ART's signal chain (`libsigchain`) sees every fault before our
handler does, with the guest's `x18`. Both can only be checked in an app: the Android app build with
`-DLGP_ANDROID_NATIVE_APP=ON` adds `lgp_game_native.apk` (package `org.lgp.recomp.native`, label "LGP native").
It is a separate app with its own data folder, so the game is imported into it once and the saves of the lifted app
stay untouched. It uses the same app mode as `lgp_game`, and the rewrite cache lives in its cache folder.

**Result, OnePlus 7 Pro (2026-10-29):**

- **Runs:** SELinux reports no denial, so the app may make the rewritten code executable. The rewrite takes 47.6 s,
  and 16 ms from the cache. The game runs 900 frames and ends with exit 0.
- **Prerequisites:** the run needs `--save`; without it `am` `EnsureSaveData` has no storage and the guest stops at
  an unimplemented call. It also needs `systemdata` (NgWord2); the native start has no hooks, so it has no stand-in
  for it and stops after the intro.
- **Speed:** native and lifted are equally fast. In an ABAB run (`--unthrottled`, press script, 6000 frames,
  host clock) each 600-frame window is within a few fps between the two arms, with no arm ahead in every window.
  The native arm reports `nvnSyncWait` at 35–42 ms per frame in the slow scene, so the GPU sets the pace there, not
  the CPU code.
- **Memory:** the native arm needs about 200 MiB less footprint (≈1555 MiB against 1749 MiB at frame 6000).
- **Image:** with the virtual clock and the same press script (10 500 frames), `--frame-grid` of frames 900, 3000,
  6000, 9000 and 10 500 is identical between native and lifted (`frame_grid_compare.py`: max cell diff 0).
- **CPU throughput:** the same virtual-clock run takes 455 s native against 701 s lifted (35 % less). With the
  virtual clock one guest thread runs at a time and nothing waits for vsync, so this measures the guest code. It
  shows a CPU gain that the GPU hides in the normal run (single run per arm, so a suspicion).
- **Caveats:** the lifted arm was another build (main at the time).

## Precomputed code image (stage F)

iOS and tvOS let no process make memory executable itself; code must come from pages the system has signed. Android
apps probably may not map files of their own folder executable either (SELinux denies `execute` on app data files
from targetSdk 29; suspicion until an APK run, the shell probe runs under another domain); whether anonymous memory
works there, the memory probe checks (`docs/knowledge/speicherprobe-mobil.md`). So the device should not produce the executable pages,
but map them: `--native-image-out FILE` runs the boot up to the point where the start would make code executable, writes
every such page to FILE and stops (`native: image … written`). It runs no guest code, so any desktop host writes it,
also x86 Linux and Windows (`lgp_game_native` is built there for this). `--native-image FILE` does the boot as usual,
then compares those pages byte by byte with the file and maps the file's pages read + execute over them
(`HostBlock::map_code_file`) instead of `make_executable`; one byte off and the boot fails with the first differing
address.

- **Format:** `LGPNIMG1`, base, count, then per range VA, length and file offset, then the pages, each range in
  whole 16 KiB pages at a 16 KiB aligned offset (so the file maps as it is on 4 and 16 KiB hosts). Ranges: module text
  (rounded up to 16 KiB, so with the start of `.rodata`), the code of the patch area, the key pages and the host-call
  stubs. The patch area's data page (host function pointer) is not in it.
- **Host independence:** the pages hold only guest addresses and instruction words. The image of the test pair
  written on x86 Linux is bit-identical to the one written under qemu on AArch64, and the AArch64 start runs from it
  (`boot.sdk_native_image`).
- **Real game (LGP, 2026-10-28):** `lgp_recomp native-rewrite` gives bit-identical outputs on Windows (PC) and macOS
  (M1) with the same `game.cfg`. The image (`--graphics hle`, 7 ranges, 27 525 120 bytes, 1190 host calls) written
  on Windows (2026-10-29, two runs) is bit-identical to the one written on the Mac, so one desktop can write the image
  for every device that runs the same options. With `--graphics stop` it has 27 475 968 bytes and 166 host calls.
- **macOS refuses executable file mappings** (`mmap(PROT_EXEC)` of an unsigned file: `EPERM`, M1, memory probe and
  `--native-image` alike). There `--native-image` checks the pages against the file and then makes memory executable
  as without an image (warning `not mapped (…)`, line `native: code checked against the image …`). iOS/tvOS need the
  `vm_remap` route from signed pages for this (not built yet).
- **Same options:** the host-call stubs are those of the run's registered host functions. A run with another
  `--graphics` (NVN procs only with `hle`), another program build that registers other functions, another config or
  base gets other pages and refuses the image (`boot.sdk_native_image_mismatch`). Write the image with the options
  the device runs with.
- **Memory:** mapped code is clean file memory, which the system may drop and read again, unlike the rewritten
  anonymous pages (`make_executable`). Whether that lowers the footprint the system counts (jetsam on tvOS) is not
  measured.
- **Not yet:** embedding the image in an iOS/tvOS app (its pages in a `__TEXT` section, `vm_remap` to the guest
  address, the way the memory probe proves it); a file mapping alone is not signed code there. On Android the counterpart would be the image as a file in the APK's
library folder (system-owned, executable for the app), mapped from there; untried. The image only holds
  game code: local only, like the rewrite cache.

## Testing without an ARM64 machine

An AArch64 Linux build runs under qemu-user on x86:

```sh
apt install g++-aarch64-linux-gnu qemu-user-static
cmake -S . -B build/a64 -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/aarch64-linux-gnu.cmake \
  -DLGP_ENABLE_UNICORN=OFF -DLGP_ENABLE_WEBGPU=OFF -DLGP_ENABLE_SDL=OFF -DLGP_ENABLE_OPUS=OFF \
  -DLGP_ENABLE_VORBIS=OFF -DLGP_ENABLE_ZSTD=OFF -DLGP_ENABLE_VPX=OFF -DLGP_FIXTURE_NATIVE_DIR=<fixtures>
cmake --build build/a64 --target lgp_fixture_native
ctest --test-dir build/a64 -L native
```

The same tree runs `boot.sdk_native_stop`, `boot.sdk_native_hle` and `boot.sdk_native_image*` (`lgp_game_native` with the SDK-like
module pair from `tests/boot`, needs clang and ld.lld); parts that need `lgp_lift` on the build machine are missing there.

`<fixtures>` contains `basic`/`threads` as NSO/NRO and `expected/<name>_<fmt>.native.expected` from an
x86 build with Unicorn (test `fixtures.native_oracle.*`). qemu-user executes the real instructions, but is no
proof for Apple hardware: memory ordering, page size and code signing are only checked by a run on the Mac.
