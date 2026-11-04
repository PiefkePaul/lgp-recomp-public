# Ryujinx caches as run-time data (PTC profile, guest shader cache)

Cross-game knowledge from P1.4 (status 2026-10-11, Ryujinx Canary 1.3.265 / Ryubing, source code under
`D:\Ryujinx\Repo`). Tool: `tools/analyze/ryujinx_cache.py`.

Ryujinx creates caches per game that show, without any instrumentation, **which code ran** and **which shaders the
GPU received**. One only has to play normally (PTC and shader cache are on by default). Everything is under
`<ryujinx-root>/games/<titleid>/cache/`.

## PTC profile: which guest functions ran

File: `cpu/0/<version>-default.info` (`cpu/1/…` was identical for us). Source: `ARMeilleure/Translation/PTC/PtcProfiler.cs`.

| Part | Content |
|---|---|
| OuterHeader, 29 bytes, uncompressed | `u64` magic `"Pohd\0\0\0\0"`, `u32` InfoFileVersion (7007 for us, current source 7031, same format), `u8` endianness, `Hash128` header hash |
| Rest: raw Deflate (RFC 1951, without zlib header) | `Hash128` over the rest, `i32` count, per entry `u64` guest address, `i32` ExecutionMode, `u8` HighCq, `u8` Blacklist (14 bytes) |

- An entry is created when ARMeilleure translates a function starting at this address (`Translator.cs`, `AddEntry`), i.e.
  on first execution. Entries are function entries (call targets), not basic blocks.
- The profile is **accumulated** over all sessions (loaded, extended, saved).
- Older versions without the blacklist field have 13-byte entries; the reader rejects them instead of reading them wrong.
- Argument values are not visible: "ran" only means "was called".

Module bases are in the log (`Logs/Ryujinx_*.log`, even without trace logging):

```
|I| Loader Load: Loading rtld...            (order = image index)
|I| Loader LoadNsos: Loading image 1 at 0x0000000008504000...
```

The same in all our runs (no ASLR): `rtld` 0x8500000, `main` 0x8504000, then `subsdk*` and `sdk` directly
one after another (image + BSS, rounded to 0x1000).

Typical questions this answers: which SDK functions does the game really call? Does a module run at all
(e.g. glslc)? Which game functions are "hot" enough to check them first?

## Guest shader cache: which shaders the GPU received

Files: `shader/guest.toc` + `shader/guest.data`. Source: `Ryujinx.Graphics.Gpu/Shader/DiskCache/DiskCacheGuestStorage.cs`.

| Part | Content |
|---|---|
| `guest.toc` header (32 bytes) | `u32` magic `"TOCG"`, `u32` version (0x00010001), `u32` padding, `u32` ModificationsCount, 2× `u64` reserved |
| `guest.toc` entries (16 bytes) | `u32` offset in `guest.data`, `u32` CodeSize, `u32` Cb1DataSize, `u32` hash |
| `guest.data` from offset | Cb1 data (uncompressed), then `u8` compression (0 none, 1 Deflate, **2 Brotli** = default), then the compressed code |

- The Brotli stream has no length field; one cuts it at the start of the next entry (entries are appended
  one after another). Python: package `brotli`.
- The code is the raw Maxwell guest code **including** the 0x50-byte shader program header (SPH). Compute shaders have
  no SPH and start directly with the first control word.
- Every stage is in there once, accumulated over all sessions. Programs (combinations of stages) are stored separately in
  `shared.toc`/`shared.data`; `vulkan_nvidia.*` is the host cache (translated SPIR-V/driver binaries).

Typical questions: how many shaders does a game section use? Are they byte-identical in the game data (then
offline translation is enough) or are they created at run time? See `nx-grafikformate.md` for searching in BNSH containers.

## Notes

- The Ryujinx source code is available locally under `D:\Ryujinx\Repo` (the public repos are partly blocked by DMCA,
  `git.ryujinx.app` was not reachable from the PC). Ryujinx is MIT-licensed.
- The caches contain game code or shaders and stay in `$LGP_PRIVATE_DIR`. Only counts and names go into the repo.
