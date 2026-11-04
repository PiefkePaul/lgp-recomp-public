# 0004 – Indirect jumps, self-modifying and JIT code

Status: **Accepted** (Paul, 2026-10-10) · Date: 2026-10-10 · Updated: 2026-10-10 (P1.6, measurement data from P1.3–P1.5) · Decides: Paul · Gate: data from P1.5

## Context

AArch64 has three indirect control flows: `BLR Xn` (function pointers, virtual calls, calls via PLT/GOT into other NSOs), `BR Xn` (jump tables of `switch`, tail calls) and `RET` (return via LR). Static recompilation must know every target at build time or look it up at runtime.

Self-modifying code and JIT: retail games on the Switch have **no JIT permission** (only system applets such as the browser have it). Dynamically loaded code is still possible: NROs via the `ro` service. LGP does not use it (next section).

Proven patterns:
- N64Recomp: indirect calls become `LOOKUP_FUNC(addr)`, the runtime resolves via a table.
- XenonRecomp: function table directly behind guest memory (index = guest address); jump tables are recognized by an analysis tool and translated into real `switch` statements via a config file. The README itself says that there is no general solution for jump tables.

## Measurement data (P1.3–P1.5)

Sources: `04-code-statistik.md`, `01-inventar.md` §4/§5, `02-services.md` §3.

| Quantity (`main`, unless stated otherwise) | Value |
|---|---|
| Functions / FDEs | 60,919 Ghidra functions, 59,324 exactly on an `.eh_frame` FDE; only 3 without FDE (PLT thunks); no `BL` to an address without a function |
| `BL` / `BLR` | 177,518 / **109,287** (0.6 `BLR` per `BL`); 94 % "vtable-like" (pointer from object behind pointer), Ghidra resolves only 2 % |
| address-taken functions | **37,049 = 61 %** of all functions (6,733 pointer runs with 56,406 slots) |
| `BR` | 5,924: 1,541 jump tables, 787 PLT thunks, 211 resolved tail calls, 3,385 open (of which 3,085 "loaded pointer" = indirect tail calls, 55 jump table patterns without Ghidra resolution) |
| Jump table pattern | **all 1,541** use the same clang pattern `ldrsw` → `add` → `br` (32-bit relative); max. 115 cases |
| `RET` via a register other than `x30` | **0** (all modules) |
| `setjmp` / `longjmp` | 3 / 4 call sites in `main` (20 / 4 in `subsdk0`) |
| C++ exceptions in `main` | 1 FDE with LSDA, 2× `__cxa_begin_catch`, `__cxa_throw` **not imported**; in `sdk` 1,227 FDEs with LSDA |
| dynamic code | no code memory SVCs in the NPDM, no NROs in the RomFS, no `nn::ro` import, `ldr:ro` unused during the run |
| Function sizes | p50 104 B (26 instr.), a quarter ≤ 4 instructions |

### Critical review of the recommendation

1. **Option A is confirmed.** A native C++ return holds up (0 `RET` ≠ `x30`, no exceptions in `main`), function boundaries come almost completely from the FDE table. No SMC/JIT, no interpreter needed, not even in the debug build.
2. **The dispatch table must contain all functions,** not just "known" targets: 61 % are address-taken, 98 % of the `BLR` are statically unresolved. Because indirect calls are so frequent (109,000 sites), the lookup must be **O(1)**. Directly indexed table (index = (address − module base)/4, 4-byte entry with Wasm table index): ≈ 13 MiB for `main`, ≈ 18 MiB for `main` + `sdk` (ADR-0005), ≈ 29 MiB for all modules. This fits into the budget from ADR-0001. A sorted array (~16 comparisons per call) would be too expensive at this call density.
3. **`setjmp`/`longjmp`: option C is probably unnecessary.** Because with option A the host stack mirrors the guest call stack 1:1, guest `setjmp`/`longjmp` can be mapped via HLE onto host `setjmp`/`longjmp`: the host jump buffer lives in a side table keyed by the address of the guest `jmp_buf`; the guest registers (`x19`–`x30`, `sp`, `d8`–`d15`) are saved in addition. In Wasm, Emscripten handles this via Wasm exceptions (`-sSUPPORT_LONGJMP=wasm`), with costs only in the functions that call `setjmp`. This is much less effort than a block dispatcher for these functions. Option C remains the fallback.
4. **Exceptions:** practically none in `main`. `sdk` has 1,227 functions with LSDA; if `sdk` is recompiled too (ADR-0005), the recompiler would need unwinding. Proposal: no unwinder; `__cxa_throw` (and `_Unwind_RaiseException`) are replaced by symbol and abort loudly with a log. If this is ever triggered in the real run, it is re-evaluated.
5. **New risk: indirect tail calls and the host stack.** The 3,085 `BR` from a loaded pointer plus 15,599 direct `B` tail calls to other functions are mapped as "call + return". This makes the host stack grow where the guest stack stays constant. With chains (e.g. state machines that call each other via tail calls), the host stack can overflow. Remedy: `[[clang::musttail]]` in the generated code where signatures match (all generated functions have the same signature, so this always fits). In Wasm this becomes `return_call`/`return_call_indirect`; according to secondary sources Wasm tail calls exist in Chrome, Firefox and Safari (from 18.2), to be checked before P6.1.
6. **Jump tables:** a single recognizer for the `ldrsw`/`add`/`br` pattern covers ≥ 97 %; the rest goes into the config file. That is less maintenance than feared in the draft.

## Options

### A: Function-based like N64Recomp/XenonRecomp

- Every recognized guest function becomes a C++ function; `BL` = direct call, `RET` = C++ `return` (native stack).
- `BLR` → dispatch via a table guest address → host function (sorted array/perfect hash; in the WASM build this ends up as `call_indirect`).
- `BR` within a function → statically recognized jump table as `switch`; `BR` to another function start → tail call via dispatch.
- Unknown target → hard error with the address; the target is added to a config file and recompiled.

Pros: proven, fast, easy to debug. Cons: `RET` as a native return breaks with code that manipulates LR/SP (`longjmp`, C++ exceptions with their own unwinder, coroutines); needs special handling for that.

### B: One big dispatcher loop (basic block level)

Every block returns the next guest address, a loop dispatches.

Pros: arbitrary control flows, `longjmp`/exceptions trivial. Cons: much slower, bad for compiler optimization, expensive in the browser.

### C: Hybrid

A as the normal case, B only for marked problem functions.

### Interpreter fallback for unknown code

An ARM64 interpreter in the runtime for non-recompiled targets (e.g. NROs loaded at runtime). Helps during bring-up, but costs effort and masks analysis errors.

## Recommendation

**A** (confirmed), made concrete by the data:
- Function = C++ function with a uniform signature, `BL` = direct call, `RET` = C++ `return`.
- Function boundaries primarily from the FDE table, supplemented by `BL` targets and PLT thunks.
- `BLR` and `BR` from a loaded pointer → **directly indexed dispatch table over all function starts** of the recompiled modules (O(1)).
- Tail calls (direct and indirect) with `[[clang::musttail]]`; where that is not possible, call + return.
- `BR` with the `ldrsw`/`add`/`br` pattern → `switch`; exceptions via config file.
- `setjmp`/`longjmp` via HLE onto host `setjmp`/`longjmp`; option C (block dispatcher) only if that fails.
- No exception support: `__cxa_throw` aborts loudly.
- No interpreter, not even in the debug build (no dynamic code present).

Unknown indirect targets are an **error, not a silent fallback**: they are logged, entered into a config file (without game code, only addresses) and recompiled. Because the table contains all FDE functions, there should be very few of them (targets in the middle of functions or in non-recompiled modules).

## Open questions / data needed

- ~~`BLR`/`BR` sites, jump table patterns, unresolvable targets (P1.5)~~ → answered (table above).
- ~~`setjmp`/`longjmp`, exceptions, NROs~~ → 3/4 sites, no exceptions in `main`, no NROs.
- ~~Calls `main` ↔ `sdk`~~ → 787 `JUMP_SLOT` PLT thunks, all imports resolvable within the ExeFS; the recompiler resolves them statically at link time (ADR-0005).
- **New:** measure the runtime cost of the dispatch table and of `musttail` in Wasm (P2.8 WASM smoke test, P6.1).
- **New:** check the targets of the 55 unresolved jump tables with our own recognizer (P2.5).

## Consequences

- The recompiler needs a config file per game with additional knowledge (jump tables, function boundaries, special cases); it contains only addresses and our own names, no code.
- The runtime contains the dispatch table and the error path (P2.5); callbacks from HLE code into guest code (thread entry, NVN memory callback, allocator hooks `sdk` → `main`) go through the same table.
- `setjmp`/`longjmp` and `__cxa_throw` are on the replacement list from ADR-0005.

## Sources

- <https://github.com/N64Recomp/N64Recomp>
- <https://github.com/hedge-dev/XenonRecomp>
