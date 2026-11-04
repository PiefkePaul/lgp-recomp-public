# Darwin: what counts toward the footprint and how to stay under the jetsam limit

Status 2026-10-14. Cross-game, applies to iOS, iPadOS, tvOS and visionOS. What follows from it for LGP on the Apple TV
is in `docs/analysis/31-apple-tv-speicher-recherche.md`.
The basis is the XNU source code (apple-oss-distributions/xnu, `main`, read 2026-10-14) and Apple's documentation.
What was only read from the source code and not measured on a device is marked as "from the source code".

## The limit is a ledger limit on `phys_footprint`

jetsam terminates a foreground app as soon as its ledger entry `phys_footprint` exceeds the limit
(`task_set_phys_footprint_limit_internal`, `ledger_get_limit(…, task_ledgers.phys_footprint, …)` in
`osfmk/kern/task.c`; `bsd/kern/kern_memorystatus.c` reads `get_task_phys_footprint`). The limit depends on the device
model and is not documented. Measured: Apple TV 4K (3rd gen) ≈ 2098 MiB, iPad Pro M1 ≈ 5.1 GiB, 4 GB iPhones according
to the Apple forum "ActiveHard 2098 MB" (so the same order of magnitude as the Apple TV).

The formula is a comment in `osfmk/arm/pmap/pmap.c`:

```
phys_footprint = (internal - alternate_accounting)
               + (internal_compressed - alternate_accounting_compressed)
               + iokit_mapped
               + purgeable_nonvolatile + purgeable_nonvolatile_compressed
               + page_table
```

Consequences, all from the source code:

| Kind of memory | counts? | Evidence |
|---|---|---|
| anonymous memory (`MAP_ANON`, `malloc`), written | yes ("internal") | formula |
| anonymous memory, packed by the compressor | **yes, at full page size**. The compressor does not lower the footprint. | `pmap.c`: when paging out to the compressor `internal_compressed += page size` with the comment "it mustn't affect total task footprint" |
| file-backed pages (`mmap` of a file with `MAP_SHARED`), even written | **no** ("external") | `pmap.c`: "external page: no impact on ledgers"; external is not in the formula |
| file with `MAP_PRIVATE`, written | yes (copy-on-write creates anonymous pages) | general VM behavior |
| purgeable, marked volatile (`VM_PURGABLE_VOLATILE`, `MTLPurgeableState.volatile`) | no, but may be discarded at any time | formula (only `purgeable_nonvolatile`) |
| `MADV_FREE_REUSABLE` | no, as long as it is not used again | ledger `reusable` |
| Metal/IOKit memory (textures, buffers) | yes (`iokit_mapped` or the graphics ledgers) | formula |
| page tables | yes, but small (≈ 2 MiB for 4 GiB with 16 KiB pages) | formula |

Important: older notes in this repo assumed that compressed pages count only with their compressed size and
zero pages hardly at all. According to the source code that is **not** true for the footprint. Compression helps the
system as a whole, not against the per-app limit. It can be confirmed cheaply: the memory probe with fill `zero` must
abort at the same number of written MiB as with random data.

## Levers, sorted by benefit for a large guest memory

1. **File-backed guest memory (`MAP_SHARED` on a file in `Library/Caches` or `tmp`).** Written pages
   are "external" and do not count. The system writes them back to the file when needed and reloads them on the
   next access, so it is a kind of paging that iOS does not have for anonymous memory. A third-party source
   (mo4tech, "MMap on iOS") says the same, without measurement. Open points (suspicion, unmeasured):
   - Performance: the first access to a page is a file fault (for a sparse file from `ftruncate`
     without read access to the memory). If more is written than is free, write and read accesses
     to the flash occur, i.e. stutters.
   - Flash write load: iOS reports high write volumes as an `EXC_RESOURCE` report (Apple: "Reducing disk writes").
     Whether that is fatal on tvOS has not been checked; suspicion: only a report.
   - Apple does not document the writeback policy (DTS in the forum: "implementation detail").
   - The address space without `extended-virtual-addressing` is limited: a developer measured on an iPhone 13 (4 GB)
     a limit for mapped memory of ≈ 6.5 GiB (ENOMEM instead of jetsam), with the entitlement over 46 GiB.
     A 4 GiB guest block fits below that.
2. **Do not write pages that are never written.** A page counts from the first write access, even if only
   zeros are written. If the game zeroes large regions, it is worth checking how much of the footprint
   consists of zero pages. Such pages can be replaced at a safe point (all guest threads paused) by a
   new anonymous mapping (`mmap(MAP_FIXED)`); then they no longer count.
3. **Keep caches purgeable** (Metal `setPurgeableState`, `vm_purgable_control`): e.g. evicted textures
   or pipeline data that can be recreated. Does not count, but can be gone at any time.
4. **Keep GPU memory small**: texture budget, smaller upload arenas, render targets as `memoryless` where they
   only live within one pass. Typically brings tens to a few hundred MiB.

What does not help: switching from wgpu to native Metal or Apple's Game Porting Toolkit only lowers the GPU share
and the abstraction overhead, not the CPU-side guest memory. The Game Porting Toolkit is a
macOS test environment for Windows games (Wine + D3DMetal); the Metal Shader Converter translates DXIL. Neither fits
a recompilation with its own Maxwell→WGSL shaders.

## Entitlements

| Entitlement | iOS/iPadOS | tvOS | Effect |
|---|---|---|---|
| `com.apple.developer.kernel.increased-memory-limit` | from 15.0 | **not listed** (docs name iOS, iPadOS, visionOS 2.5) | higher limit, only on some models |
| `com.apple.developer.kernel.extended-virtual-addressing` | from 14.0 | from 14.0 | more address space, **not** more footprint |

## Measuring

- `phys_footprint` via `task_info(TASK_VM_INFO)` and `os_proc_available_memory()` (both already in `--time-stats`,
  line `host memory`).
- Breakdown by region: Xcode memory graph (`.memgraph`) or Instruments "VM Tracker"/"Allocations" on the
  device; on the Mac `footprint -p <pid>` and `vmmap --summary` on a `.memgraph` file.
- jetsam reports (`JetsamEvent-*.ips`) under Settings › Privacy › Analytics or via
  `xcrun devicectl device info files … --domain-type systemCrashLogs`. Reason `per-process-limit` means: own limit
  exceeded; `vm-pageshortage`: the whole device was short.

## Sources

- XNU: [`osfmk/arm/pmap/pmap.c`](https://github.com/apple-oss-distributions/xnu/blob/main/osfmk/arm/pmap/pmap.c)
  (footprint formula, compressor accounting, external pages), [`osfmk/kern/task.c`](https://github.com/apple-oss-distributions/xnu/blob/main/osfmk/kern/task.c)
  (ledger and limit), [`bsd/kern/kern_memorystatus.c`](https://github.com/apple-oss-distributions/xnu/blob/main/bsd/kern/kern_memorystatus.c)
- Apple: [Increased Memory Limit](https://developer.apple.com/documentation/bundleresources/entitlements/com.apple.developer.kernel.increased-memory-limit),
  [Extended Virtual Addressing](https://developer.apple.com/documentation/bundleresources/entitlements/com.apple.developer.kernel.extended-virtual-addressing),
  [Games: Get started](https://developer.apple.com/games/get-started/), [WWDC23 "Bring your game to Mac" (Metal Shader Converter)](https://developer.apple.com/wwdc23/10124)
- Apple forum: [iOS writeback behavior for mmap(MAP_SHARED) dirty pages](https://developer.apple.com/forums/thread/763058),
  [jetsam per-process-limit on 4 GB iPhones (2098 MB)](https://developer.apple.com/forums/thread/688973),
  [Entitlement does not work as expected](https://developer.apple.com/forums/thread/770868)
- Third-party measurement of the address space limit: [cactus-compute/cactus#793](https://github.com/cactus-compute/cactus/issues/793)
- Third-party source without measurement: [MMap on iOS (mo4tech)](https://www.mo4tech.com/client-basics-mmap-on-ios.html)
