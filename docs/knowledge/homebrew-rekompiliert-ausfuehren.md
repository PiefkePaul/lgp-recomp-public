# Running libnx homebrew recompiled and checking it against an oracle

Cross-game knowledge from P2.8 (status 2026-10-11). Implementation: `tests/homebrew/` (runner, Unicorn executor),
fixtures `tests/fixtures/{basic,threads}`. Tests: `ctest -L fixtures` (native, needs devkitPro + Unicorn) and the
CI job `fixtures-wasm` (recompiled in Node).

## How an NSO/NRO starts as a process

- **Load the image unrelocated.** libnx' `crt0` calls `__nx_dynamic` and relocates itself. devkitA64 r29 produces
  `DT_RELR`, and RELR adds the base (`*p += base`): applied twice, every pointer is wrong. The link step
  of the recompiler (rtld replacement for SDK modules) must therefore not be applied to the *runtime* image for homebrew;
  for the analysis (data pointers, `.init_array`) it must.
- **Mapping like Horizon's loader:** `.text` RX and `.rodata` R as `Code`, `.data`+`.bss` RW as `CodeData`. After relocating, libnx
  sets the RELRO part to R via `svcSetMemoryPermission`; that only works on `CodeData`.
- **Entry ABI:** `x0 = 0`, `x1 = handle of the main thread` (NSO ABI). An NRO starts the same way: `crt0` recognizes
  the NSO case by `x0 == 0` and ends with `svcExitProcess` instead of returning to the loader. The
  homebrew loader ABI (`x0` = config entries, `x1 = -1`) is not needed for this.
- **Heap:** `__libnx_initheap` takes `TotalMemorySize − UsedMemorySize − 2 MiB`, rounded down to 2 MiB; if that is 0,
  it requests **512 MiB**. So the heap region has to match `TotalMemorySize`.
- **No services:** the weak `__appInit` connects to sm, applet, hid, time and fs. Fixtures that only check the
  recompiler and kernel override `__appInit`/`__appExit` with empty functions.
- `argvSetup` queries the range behind the image via `svcQueryMemory` (`__argdata__`); if it is not mapped,
  there are no arguments. `virtmemSetup` needs `GetInfo` for the alias, heap, ASLR and stack regions.
  `virtmemFindStack` randomizes thread stacks with `randomGet64`, which is fed from `GetInfo(RandomEntropy)`:
  fixed entropy in the kernel = reproducible addresses.

Since P3.4 the boot program (`runtime/boot/`, [`sdk-prozessstart-ohne-rtld.md`](sdk-prozessstart-ohne-rtld.md)) also starts
homebrew this way: one module, image unrelocated, entry with `x0 = 0`, `x1 = handle`, plus the real HLE services. The
fixtures `basic`/`threads` produce the same debug output through it as in the P2.8 run (`ctest -L boot`), the fixture
`services` uses libnx' service clients.

## Pitfalls for the recompiler

- **`crt0` sets `x30 = exit` and jumps with `b main`.** `main` ends with `RET`, which in the lifted model returns to the
  host caller. For the outermost function of a thread there is none; there `RET` means "continue at
  x30". After the entry function returns, the runtime therefore calls the function at x30 as long as there
  is one (`scheduler.cpp`). Without that, every libnx program ends with `kHaltBadJump` on `exit`.
- **devkitA64 links without `.eh_frame_hdr`**, see [`function-discovery.md`](function-discovery.md): without
  code pointer seeds, `_EntryWrap` (thread entry of libnx) and `__libc_fini_array` are missing.
- **Data words in the function range** (`HOMEBREW` magic after `_start: b 1f`, MOD0 offset, zero words behind
  noreturn calls) decode as unallocated. On the A57 they are UNDEFINED as well; the lifter produces
  the same trap. `lgp_lift` counts them separately ("undefined on the A57") and only fails on real gaps.

## Unicorn as an oracle for whole processes

Instead of running Unicorn next to the runtime, Unicorn here is **an executor for guest threads**: a function
with the signature of a lifted function (`void(CpuState*)`), installed as resolver for every address. The
scheduler passes it the `CpuState` of each thread (pc, sp, x0/x1, TPIDRRO_EL0); it emulates on the host thread
of the guest thread, passes every SVC to the same `Kernel` as lifted code and exits with the same
halt reasons. That way oracle and recompiled code run on the same kernel, layout and scheduler, and the comparison can
be strict: process end, debug output and the content of **every** mapped block (stack, TLS, heap included).

- Memory via `uc_mem_map_ptr` onto the runtime's host memory, compared against the kernel's block list after every SVC
  (heap, stacks, TLS pages change) and remapped **only on a change**: every
  `uc_mem_unmap`/`uc_mem_map_ptr` discards Unicorn's translation cache. Always remapping made the
  `threads` run (a few thousand mutex/CondVar SVCs) 150 times slower (4:44 min instead of 1.9 s). An access to unknown memory is retried once after a fresh comparison, because another
  thread may just have mapped it.
- Unicorn runs in slices (here 10⁶ instructions); between the slices there is a scheduler point like at a
  lifted loop back edge.
- **Determinism:** with all threads on one core and core baton, the scheduler switches only at SVCs. Then
  both runs have the same interleaving, even the order in which consumers take elements from a queue.
  In "free" mode only the result would be the same, not the memory.

## WebAssembly

- The loader (`lgp_recomp_core`) uses C++ exceptions, which Emscripten only has with `-fwasm-exceptions`. Then
  **every** object with `setjmp`/`longjmp` (here `run_lifted`, `host_jmp.hpp`) must also be built that way, otherwise
  `emscripten_longjmp` is missing at link time. Until P6.1 the fixture runners had their own runtime variant for this;
  since the boot program also runs in the browser, the project builds throughout with `-fwasm-exceptions` under Emscripten
  (root `CMakeLists.txt`).
- devkitA64 without Docker Hub: on "TOOMANYREQUESTS" (anonymous pull limit) `mirror.gcr.io` delivers the same image
  by digest without a token (`https://mirror.gcr.io/v2/devkitpro/devkita64/manifests/<digest>`, blobs under `/blobs/`).
- The Wasm runs compare against the final state of the native Unicorn run stored as text (FNV-1a per block).

## devkitA64 without pacman/Docker daemon

In environments without a Docker daemon and without access to `pkg.devkitpro.org` the image
`devkitpro/devkita64` can be fetched via the registry API (anonymous token from `auth.docker.io`, manifest by digest, layers as
`tar.gz`) and only `opt/devkitpro` unpacked. With `DEVKITPRO` pointing to that path, `cmake --preset default` builds the
fixtures. Use the same digest as in CI.
