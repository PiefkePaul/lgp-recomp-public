# Horizon memory SVCs in a custom runtime (HLE kernel)

Cross-game knowledge from P2.6/P3.1 (status 2026-10-11). Implementation: `runtime/core/` (`memory.hpp`, `kernel.hpp`,
`guest_mem.hpp`, `guest.hpp`). The semantics follow the public ABI documentation (switchbrew, libnx) and the behavior of
Atmosphère/Ryujinx as far as known to us; no code was taken. Where a rule is only our reading,
it says so.

## Register ABI of the memory SVCs

Result always in `w0` (0 = success, otherwise `module 1 | description << 9`).

| SVC | No. | Input | Output |
|---|---|---|---|
| `SetHeapSize` | 0x01 | `x1` size (multiple of 2 MiB) | `x1` heap address |
| `SetMemoryPermission` | 0x02 | `x0` address, `x1` size, `w2` perm (only None/R/RW) | – |
| `SetMemoryAttribute` | 0x03 | `x0`, `x1`, `w2` mask, `w3` value (only Uncached = 8) | – |
| `MapMemory` / `UnmapMemory` | 0x04/0x05 | `x0` destination, `x1` source, `x2` size | – |
| `QueryMemory` | 0x06 | `x0` pointer to `MemoryInfo` (0x28 bytes), `x2` address | `w1` PageInfo |
| `MapSharedMemory` / `Unmap…` | 0x13/0x14 | `w0` handle, `x1` address, `x2` size, `w3` perm | – |
| `CreateTransferMemory` | 0x15 | `x1` address, `x2` size, `w3` perm | `w1` handle |
| `GetInfo` | 0x29 | `w1` Id0, `w2` handle, `x3` Id1 | `x1` value |

Pseudo-handles: `0xFFFF8000` current thread, `0xFFFF8001` current process. `GetInfo` for process values requires
`0xFFFF8001` and `Id1 = 0`; `RandomEntropy` (11) and `DebuggerAttached` (8) require handle 0.

`UserExceptionContextAddress` (20) must not be 0: the SDK writes into it right at startup
(`nn::os::SetDyingMessageRegion` stores address and size of the dying message at offset 0x1c0). Horizon gives every
process its own 0x200 area for this in thread-local pages (the "process local region"); the runtime creates it on the
first query.

## The runtime determines the layout

- Games read all regions via `GetInfo` (heap 4/5, alias 2/3, ASLR 12/13, stack 14/15) and `QueryMemory`. Retail code
  does not expect fixed addresses (see `horizon-prozessverhalten-fuer-recomp.md`). That is why a runtime may place the
  whole address space below 4 GiB; outside it `QueryMemory` reports an `Inaccessible` block.
- `MapMemory` may only map into the **stack region** (destination outside: `InvalidMemoryRegion`). The SDK's thread stacks
  are created this way: heap pages are mirrored into the stack region. So the region only needs to be as large as all
  thread stacks together (for LGP 38 stacks).
- Shared memory may be anywhere in the address space, just not in the heap or alias region (our reading of the
  `CanContain` rule). A runtime therefore needs a free zone outside the heap.
- `UsedMemorySize` only counts real memory (images, heap, main stack, TLS). `MapMemory` creates an alias and does not count.
  `SetHeapSize` fails with `LimitReached` if growth needs more than `TotalMemorySize − UsedMemorySize`.

## Alias mappings without aliasing: copy instead of mirror

`MapMemory` makes the source inaccessible (perm None, attribute Locked) as long as the destination exists. Therefore a
runtime can **copy** the content into the destination on mapping and copy it back on unmapping; for the guest this is
indistinguishable. This also works in Wasm, where there is no page aliasing. Cost: once the stack size per
thread start and end, negligible.

Transfer memory, on the other hand, needs no second mapping at all if the services run via HLE in the same address space: the
service reads and writes the lent pages directly at their guest address. The owner keeps the pages with the
permission given at creation (None/R/RW) and the attribute Locked until the last handle or the last
reference of the service is gone.

## HLE and host page protection do not get along

The obvious idea would be to mirror guest permissions via `mprotect`/`VirtualProtect` onto the host pages, so that forbidden
guest accesses are caught in hardware. This fails because of HLE: services share the same mapping and must write exactly where
the guest may not (HID shared memory is read-only for the guest, transfer memory is often lent with perm
None). A second host mapping of the same pages (memfd or similar) would work on the desktop, but not in Wasm.

Solution in this runtime: host protection only distinguishes "mapped" (RW) and "not mapped" (no access, on the
desktop via reservation). Guest permissions are checked by a software page table (1 byte per 4 KiB page, 1 MiB for
4 GiB), but only in checked mode (`LGP_GUEST_MEM_CHECKED`, default in debug builds). The same table carries the
watch/dirty bit for GPU pools (ADR-0001 point 4).

## Wasm: zero-filling without touching gigabytes

Horizon delivers new heap filled with zeros. In Wasm a `calloc` or `memset` over 3 GiB would be fatal, because the browser
then actually commits the pages. Fresh memory from `memory.grow`, however, is guaranteed to be zero. Before the
allocation the runtime remembers the old memory size (`__builtin_wasm_memory_size(0)`) and only clears the part of the block that lies below it
(remnants of earlier allocations). On unmapping, what was actually used is cleared.

## Pitfalls

- `QueryMemory` only reports the lower byte of the kernel state (`MemoryInfo.state`), e.g. 5 for heap, 0xB for
  `MapMemory` destinations, 3/4 for code/data of a module.
- `SetMemoryAttribute` only allows the Uncached bit; any other mask is `InvalidCombination`. For LGP 1,127 calls,
  probably GPU buffers. The bit is suitable as a hint as to which pages the graphics layer must watch.
- `CreateTransferMemory` with perm None and afterwards a guest access to the pages is a real bug in the guest, not a
  runtime bug. The Unicorn test `runtime.kernel_unicorn` checks exactly that.
- Per shared memory object the runtime currently supports **one** simultaneous mapping; a second one returns
  `InvalidState`. For the two calls measured for LGP this is enough.
