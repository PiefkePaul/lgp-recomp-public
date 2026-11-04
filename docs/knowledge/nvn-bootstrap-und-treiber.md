# NVN in Switch games: bootstrap, proc slots, driver table

Cross-game knowledge from P1.4 (as of 2026-10-11, NintendoSDK 5.4.150 / `NintendoSDK_NVN-5_4_150`). Game-specific
numbers are in `docs/analysis/03-grafik.md`, not here. Tool: `tools/analyze/nvn_inventory.py`.

## How a game gets to NVN

1. The game's `main` imports only **`nvnBootstrapLoader`** from `sdk` (plus `nv::InitializeGraphics`,
   `nv::SetGraphicsAllocator`, `nv::FinalizeGraphics`).
2. `nvnBootstrapLoader("nvnDeviceGetProcAddress")` returns the pointer to `nvnDeviceGetProcAddress`.
3. A proc loader (the inlined `nvnLoadCProcs` from the NVN headers) fetches **every** NVN function by name and stores it
   in its own global function pointer (`pfnc_nvn…`). In SDK 5.4 that is 498 functions; the loader always fetches
   all of them, whether the game uses them or not. So the name list in `main` only says "NVN 5.4", not "used".
4. After that the game calls NVN only through these pointers. There are no inline accesses to object contents.

Code pattern of the loader (clang, AArch64), per function:

```
adrp x1, name@PAGE ; add x1, x1, name@PAGEOFF   ; name as C string
mov  x0, x20 ; blr x19                          ; getProcAddress(device, name)
adrp x8, got@PAGE ; ldr x8, [x8, got@PAGEOFF]   ; GOT entry -> address of the slot
str  x0, [x8]                                   ; slot = function pointer
```

Detection: `ADD` reference to an `nvn*` string, then within 0x20 bytes an `LDR` from the GOT, immediately
followed by `STR X0, [Xn]`. Caution: the very first name reference (`nvnDeviceGetProcAddress` in the bootstrap) comes
before it without a store; without the `STR` condition one wrongly assigns it the slot of the first loader function.

Counting usage: every `ADRP`+`LDR` on the same GOT entry outside the loader is a call site
(pattern `ldr x8, [GOT]` → `ldr x8, [x8]` → `blr x8`). In practice the slots are only read via the GOT
(no direct `.bss` accesses, no further relocations on the slots).

## The driver in `sdk`

- `sdk` contains the complete NVN driver. It exports only `nvnBootstrapLoader`, `nvnBootstrapLoaderInternal`,
  `nvnDevtoolsBootstrap` and `nvndetailUseMiddleWare`.
- For `nvnDeviceGetProcAddress` there is a **sorted name table without the `nvn` prefix** (`BlendStateGetAdvancedMode`
  … `WindowSetPresentInterval`), NUL-separated, followed by a table of u32 offsets.
- In parallel there is a table of **implementation pointers** in `.data` (long run of `R_AARCH64_RELATIVE` at
  8-byte spacing, in SDK 5.4: 501 entries). Entry *i* is the implementation of the *i*-th name (sample:
  getters like `BlendStateGetAdvancedMode` = `ldr w8,[x0]; ubfx w0,w8,#8,#6; ret`).
- Use: for every NVN function the implementation is known. Getters/setters show field offsets and bit fields of the
  objects, `…GetDebugLabel`/`SetDebugLabel` are often just `ret`. Driver-internal calls (e.g. `QueueGetError`,
  `CommandBufferGetCommandMemoryUsed`) can be recognized by `BL` within `sdk` to the implementation.

## Runtime usage without instrumentation

Ryujinx's PTC profile contains all translated guest functions (see `ryujinx-caches-als-laufzeitdaten.md`). With the
`sdk` base address from the log: implementation *i* executed ⇔ `sdk_base + impl_i` in the profile. This yields the
NVN functions actually used, including those called only by the driver itself.

## glslc (`subsdk0`)

Games that ship glslc often bind it only optionally: `main` has a thunk per `glslc*` import that only returns the
address (`adrp x0, GOT; ldr x0, [x0, …]; ret`), and one function collects these into a table. Whether glslc
actually runs is shown by the PTC profile: if `subsdk0` only has `_init` and `.init_array` constructors, nothing was
ever compiled.

## Consequences for an NVN HLE

- It suffices to serve the function pointer slots (or `nvnDeviceGetProcAddress`) with our own functions.
- NVN objects are opaque fixed-size memory to the game. An HLE can store its own data in them; what matters
  are the **sizes** (derivable from the driver accesses) and the **values the game passes on** (GPU addresses
  from `BufferGetAddress`/`MemoryPoolGetBufferAddress`, texture/sampler handles from `DeviceGetTextureHandle` that end up in
  uniform buffers).
- Not visible via the API: CPU writes into mapped pools (`MemoryPoolMap`, `BufferMap`). A backend must
  synchronize these ranges itself.
