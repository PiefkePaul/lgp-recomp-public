# Loading NSO and NRO: format, MOD0, `.dynamic`, relocations

Cross-game knowledge from P2.1 (status 2026-10-11). Applies to every ExeFS module (`rtld`, `main`, `subsdk*`, `sdk`)
and to homebrew NROs (libnx). Game-specific numbers are in `docs/analysis/01-inventar.md`, not here.
Implementation: `recomp/core/src/loader.cpp`, `link.cpp`; Python reference `tools/analyze/nso_inventory.py`.

## NSO0 (header 0x100 bytes)

| Offset | Content |
|---|---|
| 0x00 | magic `NSO0`, 0x04 version, 0x08 reserved |
| 0x0C | flags: bits 0–2 `.text`/`.ro`/`.data` LZ4-compressed, bits 3–5 segment has SHA-256 check |
| 0x10 / 0x20 / 0x30 | segment headers `.text`/`.ro`/`.data`: `file_offset`, `memory_offset`, `size` (u32 each), then one u32 (for `.text`: `module_name_offset`, for `.ro`/`.data` unused or BSS) |
| 0x3C | BSS size (behind `.data`) |
| 0x40 | module ID (32 bytes; build ID from `.note.gnu.build-id`, padded with zeros) |
| 0x60 | compressed file sizes of the three segments |
| 0x88 | `api_info`, `dynstr`, `dynsym` as (offset, size) **relative to `.ro`** |
| 0xA0 | three SHA-256 over the **decompressed** segments |
| 0x100 | segment data in file order |

- Segments are **raw LZ4 blocks** (no frame header, no block sizes). The decoder knows the target size from the
  header; a custom implementation is 40 lines (token, literal length with 255 continuation, offset u16 LE, match
  length + 4, copy overlapping matches byte by byte). Bugs one can introduce: the last sequence has no match,
  offset 0 is invalid, never fill the target buffer beyond `size`.
- The SHA-256 is the only proof of integrity; `--no-hash` saves time during development, in the pipeline the
  check should stay on, because otherwise a twisted offset silently delivers wrong bytes.
- For SDK modules `.ro` starts with a small header: u32 0, u32 path length, then the build path (`…\beluga.nss`).
  The internal module name comes from that. NROs have nothing comparable there, the name is the file name.
- `api_info` is a string block `SDK MW+<vendor>+<library>-<version>` per line and reveals SDK version and
  middleware without any analysis.

## NRO0 (header 0x80 bytes, lies **inside** `.text`)

| Offset | Content |
|---|---|
| 0x00 | `b start` (code!), 0x04 offset to MOD0, 0x08 reserved |
| 0x10 | magic `NRO0`, 0x14 version, 0x18 file size, 0x1C flags |
| 0x20 / 0x28 / 0x30 | segment headers `.text`/`.ro`/`.data`: `memory_offset`, `size` (file offset = memory offset) |
| 0x38 | BSS size, 0x40 module ID (32 bytes) |
| 0x68 | `api_info`, `dynstr`, `dynsym` as (offset, size) relative to `.ro` |
| 0x80 | first real instruction (libnx: `.org _start+0x80`) |

Pitfalls:

- The NRO header is part of `.text`. Bytes 0x04–0x7F are **not code**, the entry at 0 on the other hand is. Whoever
  disassembles that range too finds "functions" made of `NRO0` magic and size fields there. For NSO the same applies to
  the MOD0 pointer word at `.text+4` (if MOD0 is present and `.text` is at 0): entry `b start` at 0 is code,
  the 4 bytes after it are not (`Module::header_code_exclusion`).
- Behind the NRO there is usually an ASET block (icon, NACP, RomFS); it does not belong to the image.
- Homebrew NROs are placed at a random base by the loader; everything position-independent works via the
  relocations, see below.

## MOD0 (shared by NSO and NRO)

The u32 at `.text+4` is the offset of MOD0 (relative to the module start). MOD0: magic, then **signed**
s32 offsets **relative to the MOD0 start**: `.dynamic`, BSS start, BSS end, `.eh_frame_hdr` start, `.eh_frame_hdr` end,
pointer to the runtime module object (unused). libnx appends extensions `LNY0` (GOT range), `LNY1`, `LNY2`.
Negative offsets occur (`.eh_frame_hdr` lies in `.ro`, i.e. **before** MOD0 in `.data`) – a parser that only
accepts `eh_s >= 0` then counts 0 FDEs (a bug that was in the Python reference parser and was fixed in P2.1).

## `.dynamic`: what you need

| Tag | Purpose |
|---|---|
| `DT_SYMTAB`/`DT_STRTAB`/`DT_STRSZ` | symbol table; the number of symbols does **not** come from `.dynamic` |
| `DT_HASH` | `nchain` = symbol count (SDK modules usually have it) |
| `DT_GNU_HASH` | symbol count only by walking the buckets/chains up to the last entry with bit 0 set |
| otherwise | divide the gap between `DT_SYMTAB` and the next section (`DT_STRTAB`) by 24 |
| `DT_RELA`/`DT_RELASZ` | Rela entries (24 bytes: offset, info, addend) |
| `DT_JMPREL`/`DT_PLTRELSZ` | `R_AARCH64_JUMP_SLOT` for the PLT |
| `DT_RELR`(36)/`DT_RELRSZ`(35)/`DT_RELRENT`(37) | packed relative relocations (see below) |
| `DT_INIT`/`DT_FINI`, `DT_INIT_ARRAY(SZ)`/`DT_FINI_ARRAY(SZ)` | constructors, **read only after the relocations** |
| `DT_NEEDED` | module names (`nnSdk`, `nnrtld`); cut off the extension when comparing with internal names (`nnSdk.nss`) |
| `DT_REL` | does not occur on AArch64; a parser may hard-abort |

## Relocations on AArch64

| Type | Number | Computation |
|---|---|---|
| `R_AARCH64_RELATIVE` | 1027 | `*P = B + A` |
| `R_AARCH64_ABS64` | 257 | `*P = S + A` |
| `R_AARCH64_GLOB_DAT` | 1025 | `*P = S + A` (GOT) |
| `R_AARCH64_JUMP_SLOT` | 1026 | `*P = S` (PLT GOT) |
| `R_AARCH64_TLSDESC` | 1031 | not supported, only count and report |

RELR (since devkitA64 r29 the **only** form of relative relocations in libnx NROs, not yet seen in SDK games):
entries are u64. Even values are an address `P` (relocate there, `where = P + 8`); odd values
are a bitmap over the next 63 words from `where` (bit i set → relocate `where + 8·(i−1)`), then
`where += 63·8`. Computation is always `*P = B + *P` (the addend is in the target word). A loader without RELR loads newer
homebrew NROs seemingly correctly and crashes at the first data pointer.

Symbol resolution like `rtld`: modules in load order `rtld, main, subsdk0…9, sdk` (everything else behind); for each
import the **first** definition in this order wins, `STB_GLOBAL` beats `STB_WEAK`. Unresolved
strong imports are an error, unresolved weak ones become 0. Practically all imports of a game can be resolved within
the ExeFS (`main` → `sdk`, `sdk` → `rtld`), a `--strict` run shows the rest.

## Minimal set of checks against a second parser

Two independent implementations (C++ and Python) were compared on synthetic modules and on the libnx fixture:
segment sizes/offsets, BSS, image size, module ID, `api_info`, symbol count, import/export count, FDE count,
`.eh_frame_hdr` size, relocation counters per type (incl. RELR). Exactly this list found three bugs that a
single parser would never have reported (see `tests/recomp/test_vs_reference.py`).
