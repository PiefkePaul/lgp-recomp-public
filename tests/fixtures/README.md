# Homebrew test fixtures

Our own Switch programs as a test basis for parser, loader and recompiler (roadmap P0.7). They replace real game data wherever cloud threads and CI work.

## Contents

| Fixture | Source | Deliberately contains |
|---|---|---|
| `basic` | `basic/main.c`, `basic/asm_ops.s` | Integer loops, CRC32, dense `switch` (jump table), function pointer table, floating point, an assembler leaf with flags/`csel`/`ubfx`, result via `svcOutputDebugString`; without `.eh_frame_hdr` (devkitA64 default) |
| `threads` | `threads/main.c` | libnx threads (`threadCreate`, stack via `svcMapMemory`), mutex/condvar via the arbiter, producer/consumer, `__thread`, `malloc`, priorities with preemption; checks itself ("checks passed"); with `-Wl,--eh-frame-hdr` like an SDK module |
| `services` | `services/main.c` | libnx's own service clients against the HLE services (P3.4): sm, applet, set (language), time, csrng, acc (nickname), fsp-srv with RomFS and save data, software keyboard; checks itself ("services: all checks passed"), without `printf` (see [`jump-table-patterns.md`](../../docs/knowledge/jump-table-patterns.md)); NSO only, runs via the boot program (`boot.fixture_services_nso`) |

`basic` and `threads` override `__appInit`/`__appExit` with empty functions: they need no services. Since P2.8 all fixtures run
recompiled against the runtime and under Unicorn with an identical final state (`tests/homebrew/`,
[`homebrew-rekompiliert-ausfuehren.md`](../../docs/knowledge/homebrew-rekompiliert-ausfuehren.md)).

Outputs per fixture (only in the build directory, **never commit**):

- `basic.elf`: ELF with all symbols, reference for function boundaries (P2.2)
- `basic.nro`: homebrew NRO (`elf2nro`, own NACP, no icon)
- `basic.nso`: the same ELF as NSO (`elf2nso`, LZ4-compressed)
- `threads.elf`, `threads.nro`, `threads.nso`: the same for `threads`
- `expected/<name>_<nso|nro>.expected`: final state of the Unicorn run (template for the WebAssembly run)
- `basic.readelf.txt`: relocations and build ID according to binutils, reference for `tests/tools/test_fixture_inventory.py`

Size: NRO approx. 96 KiB, NSO approx. 47 KiB. Most of it is libnx startup code and newlib; our own code is small.

## Building

The toolchain is devkitA64 + libnx from the Docker image `devkitpro/devkita64` (pinned by digest in CI, as of `20260219`: devkitA64 r29.2, GCC 15.2, libnx 4.12.0).

```sh
# in the container, repo mounted at /src
docker run --rm -v "$PWD":/src -w /src devkitpro/devkita64:20260219 bash -c \
  'cmake --preset ci -DLGP_BUILD_FIXTURES=ON -DLGP_ENABLE_UNICORN=OFF && cmake --build --preset ci && ctest --preset ci -L fixtures'
```

With a locally installed devkitPro (environment variable `DEVKITPRO` set), `cmake --preset default` turns the fixtures on automatically (`LGP_BUILD_FIXTURES`). Under Windows this has not been checked yet.

The main project builds `tests/fixtures/` as a separate CMake project (ExternalProject) with `$DEVKITPRO/cmake/Switch.cmake` as toolchain. Two builds of the same commit in the same image produce byte-identical NRO/NSO (checked via SHA-256).

## License

The fixture source code is our own code. Linked in are libnx (ISC), newlib (BSD-like licenses) and the GCC runtime (GPL with Runtime Library Exception). No Nintendo code is included, only what libnx brings along.

## First finding from the real fixture

devkitA64 r29 links with **`DT_RELR`** (packed relative relocations) instead of `DT_RELA`: `basic` has 13 RELR entries for 342 locations and not a single RELA. `nso_inventory.py` did not count RELR until then and was extended in this step. For the loader in P2.1 this means: RELR must be supported for the fixtures. According to `01-inventar.md` the game uses RELA; whether RELR also occurs there was not visible with the old tool and will be shown by the next PC run of `nso_inventory.py`.
