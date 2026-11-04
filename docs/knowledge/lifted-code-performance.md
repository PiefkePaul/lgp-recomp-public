# Performance of lifted code (native and WebAssembly)

Game-independent. Applies to any static A64 recompiler that emits guest code as C++ and compiles it natively as well as to
WebAssembly (Emscripten, V8). As of P6 Part 3 (2026-10-12). Code: `bench/lifter/`, lifter
`recomp/lifter/src/lifter.cpp`, runtime hooks `runtime/core/include/lgp/runtime/guest.hpp`. Measurement guide for a
real game: `../analysis/12-p6-leistung-pc-messung.md`.

## Summary

| | before P6 | P6 Part 1 (PR #51) | P6 Part 2 (calls, poll) | P6 Part 3 (V registers) | Part 3 + `LGP_FP_HOST_NAN` |
|---|---:|---:|---:|---:|---:|
| Native, host instructions relative to native (geom. mean, callgrind) | 5.51× | 3.81× | 3.43× | 3.14× | 2.96× |
| Native, run time relative to native (geom. mean, clang -O3) | 3.46× | 2.67× | 2.77× (noise) | – | – |
| WebAssembly (Node 22/V8), run time relative to native Wasm (geom. mean) | 2.67× | 2.40× | 1.89× (1.86×) | 1.72× | 1.40× |
| Code: `.text` native / C++ source | 116 / 181 KB | 122 / 246 KB | 120 / 244 KB | 144 / 250 KB | 104 / 250 KB |

(The Wasm row up to Part 2 comes from a joint measurement of these states; the old measurement from Part 1 gave 2.42× and
2.06×. Part 3 was re-measured together with Part 2, and Part 2 came out at 1.86× in that run. The `.wasm` file of the whole
benchmark: Part 2 245 KB, Part 3 238 KB, with `LGP_FP_HOST_NAN` 229 KB.)

**Part 3** puts the vector registers (`v0`–`v31`) into locals like the X registers and lets `RET` leave out the registers
that are volatile under AAPCS64. In Wasm: memops −43 %, mat4 −19 %, particles −18 %, but **interp +15 %** and strings +9 % (see
"Vector registers in local variables" below). The bigger lever is the option `LGP_FP_HOST_NAN` (default for `lgp_game` since the PC check): the exact
A64 NaN handling in the FP fast path costs up to a factor of 4.5 in Wasm (mat4 137 → 30 ms, only 1.1× native now;
particles 158 → 94 ms). NaN sign and payload then depend on the host; the check on the game (Analysis 18) found no effect.

Parts 1 and 2 in retrospect: Part 1 (registers in locals) mainly helped loops without calls; call-heavy code got slower (virtual
calls native +12 %, Wasm +23 %), CRC32 in Wasm +56 %. Part 2 fixes that: compared with the state before P6, **all
kernels in Wasm are now equally fast or faster** (CRC32 ±0, vcall −18 %, fib −16 %, interpreter −68 %) except mat4
(+6 %, vector FP, see Limits), and natively vcall needs 9 % fewer instructions than before P6. The measurement on the real game
(Analysis 12) is still pending.

## Measurement setup

`bench/lifter/guest/kernels.cpp`: ten kernels with typical patterns from game code, freestanding C++ without a C library.
The same file is built twice: for AArch64 (`clang --target=aarch64-none-elf -O2`, as an NRO, recompiled with `lgp_lift`;
this is the measured code) and for the host (reference for time and checksum). FP code on both sides with
`-ffp-contract=off` so that the checksums are bit-identical.

| Kernel | Pattern |
|---|---|
| crc32 | table access in a tight loop |
| quicksort | recursion, comparisons, swapping |
| list_walk | pointer chasing, dependent loads |
| interp | stack interpreter with `switch` (jump table) |
| vcall | virtual calls of small methods in a loop |
| fib | recursive calls without a loop |
| mat4 | 4×4 matrix multiplication (scalar and vector FP) |
| particles | particle update, float vectors |
| memops | `memcpy`/`memset` (guest implementation) |
| strings | FNV hash, string comparison (byte loads, branches) |

Build and measure:

```sh
cmake --preset release && cmake --build --preset release --target lgp_bench_lifted lgp_bench_lifted_ctx lgp_bench_lifted_hostnan
ctest --preset release -L bench                                          # checksums lifted == native
python3 bench/lifter/measure.py build/release/bench/lifter --repeat 7 --callgrind
# WebAssembly: the native tree supplies the lifted program
cmake --preset wasm -B build/wasm-bench -DLGP_BENCH_IMPORT_DIR=$PWD/build/release/bench/lifter/generated
cmake --build build/wasm-bench --target lgp_bench_lifted lgp_bench_lifted_ctx lgp_bench_lifted_hostnan
python3 bench/lifter/measure.py build/wasm-bench/bench/lifter --node \
    --stem build/release/bench/lifter/generated/program --repeat 9
```

`lgp_bench_lifted_ctx` is the same program with `lgp_lift --no-register-locals` (register accesses directly via
`CpuState`), to compare the code form with otherwise identical helper functions.

Lessons about measuring:

- **Cloud timings vary by ±30 %** between process runs, even with "best of R runs". Only instruction counts are
  reliable (`--callgrind`, valgrind, counted from `lgp_fn_<entry>` or `bench_main` including callees). For
  Wasm there is no counter; take the minimum of several process starts there (table below: minimum of 6 starts
  × 5 repetitions) and treat differences below 10 % as noise.
- **Every kernel must be its own function** (`noinline`). In a first version the guest compiler had inlined all
  kernels into one 60 KB function; V8 allocates registers badly there, and locals looked worse in Wasm
  than `CpuState` accesses. Games mostly have small to medium-sized functions.
- Instruction count and time do not always move together: CRC32 needs 46 % fewer instructions natively but is not faster,
  because the loop is bound by the load latency of the table.
- "Before" is the lifter before P6 (commit `d908a08`), measured with the same kernels in a separate worktree.

## Results

Native, executed host instructions (millions, callgrind, `--cg-scale 0.1`), ratio to native in parentheses:

| Kernel | native | before P6 | Part 1 (#51) | Part 2 | Part 3 | Part 3 + host NaN | Part 3 vs. before P6 |
|---|---:|---:|---:|---:|---:|---:|---:|
| crc32 | 2.2 | 11.4 (5.2×) | 6.2 (2.8×) | 4.9 (2.2×) | 4.5 (2.0×) | 4.5 (2.0×) | −61 % |
| quicksort | 2.9 | 16.0 (5.5×) | 11.0 (3.8×) | 9.7 (3.4×) | 9.7 (3.4×) | 9.7 (3.4×) | −39 % |
| list_walk | 5.5 | 15.0 (2.7×) | 11.0 (2.0×) | 10.9 (2.0×) | 11.2 (2.0×) | 11.2 (2.0×) | −25 % |
| interp | 21.2 | 104.5 (4.9×) | 46.0 (2.2×) | 42.8 (2.0×) | 37.5 (1.8×) | 37.5 (1.8×) | −64 % |
| vcall | 10.4 | 72.7 (7.0×) | 81.1 (7.8×) | 66.5 (6.4×) | 64.6 (6.2×) | 63.8 (6.1×) | −11 % |
| fib | 11.2 | 66.0 (5.9×) | 67.4 (6.0×) | 52.3 (4.7×) | 52.3 (4.7×) | 52.3 (4.7×) | −21 % |
| mat4 | 3.1 | 51.9 (16.7×) | 40.2 (13.0×) | 40.1 (13.1×) | 29.0 (9.5×) | 19.2 (6.3×) | −44 % |
| particles | 6.3 | 106.5 (16.9×) | 63.8 (10.1×) | 62.6 (9.9×) | 53.3 (8.4×) | 44.7 (7.1×) | −50 % |
| memops | 13.2 | 26.7 (2.0×) | 17.7 (1.3×) | 17.2 (1.3×) | 14.3 (1.1×) | 14.3 (1.1×) | −46 % |
| strings | 6.6 | 18.7 (2.8×) | 11.1 (1.7×) | 9.9 (1.5×) | 10.1 (1.5×) | 10.1 (1.5×) | −46 % |
| **geom. mean** | | **5.51×** | **3.81×** | **3.43×** | **3.14×** | **2.96×** | **−43 %** |

Native, run time in ms (x86-64, clang 18 `-O3`, best of 7; state of Part 1, Part 2 lies within the noise of the cloud measurement,
the instruction counts above are more reliable):

| Kernel | native | before | after |
|---|---:|---:|---:|
| crc32 | 8.2 | 8.4 (1.0×) | 11.3 (1.4×) |
| quicksort | 9.7 | 19.1 (2.0×) | 17.8 (1.8×) |
| list_walk | 15.2 | 19.2 (1.3×) | 15.9 (1.1×) |
| interp | 19.7 | 87.8 (4.4×) | 35.1 (1.8×) |
| vcall | 10.9 | 48.2 (4.4×) | 58.0 (5.3×) |
| fib | 9.6 | 47.4 (5.0×) | 47.1 (4.9×) |
| mat4 | 9.4 | 47.2 (5.0×) | 35.3 (3.7×) |
| particles | 6.4 | 91.3 (14.2×) | 44.6 (6.9×) |
| memops | 3.2 | 19.6 (6.0×) | 13.8 (4.2×) |
| strings | 8.7 | 19.8 (2.3×) | 11.8 (1.4×) |
| **geom. mean** | | **3.46×** | **2.67×** |

WebAssembly, run time in ms (Emscripten 6.0.12, `-O3 -pthread`, Node 22.22 / V8, `--scale 2`, minimum of 5
process starts × 3 repetitions, all states in the same session; "native" = the kernels themselves compiled to Wasm).
Measurement of Part 2 (before P6, Part 1, Part 2):

| Kernel | native | before P6 | Part 1 (#51) | Part 2 | Part 2 vs. before P6 |
|---|---:|---:|---:|---:|---:|
| crc32 | 22.1 | 21.1 (0.95×) | 32.3 (1.46×) | 21.9 (0.99×) | +4 % |
| quicksort | 24.0 | 54.1 (2.25×) | 44.6 (1.86×) | 39.0 (1.62×) | −28 % |
| list_walk | 39.4 | 45.4 (1.15×) | 45.3 (1.15×) | 34.0 (0.86×) | −25 % |
| interp | 106.3 | 266.5 (2.51×) | 129.1 (1.21×) | 84.7 (0.80×) | −68 % |
| vcall | 92.9 | 268.8 (2.89×) | 280.5 (3.02×) | 219.6 (2.36×) | −18 % |
| fib | 37.6 | 145.9 (3.88×) | 161.2 (4.29×) | 122.0 (3.24×) | −16 % |
| mat4 | 27.6 | 163.0 (5.91×) | 188.4 (6.83×) | 172.7 (6.26×) | +6 % |
| particles | 62.5 | 264.3 (4.23×) | 220.8 (3.53×) | 198.3 (3.17×) | −25 % |
| memops | 6.3 | 58.9 (9.35×) | 37.6 (5.97×) | 34.5 (5.48×) | −41 % |
| strings | 47.9 | 53.2 (1.11×) | 43.7 (0.91×) | 30.0 (0.63×) | −44 % |
| **geom. mean** | | **2.67×** | **2.40×** | **1.89×** | **−29 %** |

Measurement of Part 3 (minimum of 6 process starts × 3 repetitions, Part 2 = `main` before Part 3, re-measured; the script
interleaves the variants so that load fluctuations hit all of them equally):

| Kernel | native | Part 2 | Part 3 | Part 3 + `LGP_FP_HOST_NAN` | Part 3 vs. Part 2 |
|---|---:|---:|---:|---:|---:|
| crc32 | 22.0 | 22.1 (1.00×) | 22.2 (1.01×) | 22.0 (1.00×) | ±0 |
| quicksort | 23.9 | 39.2 (1.64×) | 39.1 (1.64×) | 39.0 (1.63×) | ±0 |
| list_walk | 39.2 | 33.6 (0.86×) | 32.7 (0.84×) | 33.0 (0.84×) | −3 % |
| interp | 102.8 | 84.4 (0.82×) | 96.9 (0.94×) | 96.0 (0.93×) | **+15 %** |
| vcall | 100.0 | 220.4 (2.20×) | 217.7 (2.18×) | 210.3 (2.10×) | −1 % |
| fib | 37.6 | 120.0 (3.19×) | 119.8 (3.18×) | 121.0 (3.22×) | ±0 |
| mat4 | 27.0 | 170.6 (6.31×) | 137.4 (5.08×) | 30.1 (1.11×) | −19 % (host NaN −82 %) |
| particles | 68.4 | 192.2 (2.81×) | 158.4 (2.32×) | 94.2 (1.38×) | −18 % (host NaN −51 %) |
| memops | 6.2 | 34.5 (5.55×) | 19.6 (3.16×) | 19.4 (3.12×) | −43 % |
| strings | 48.9 | 30.5 (0.62×) | 33.2 (0.68×) | 33.1 (0.68×) | +9 % |
| **geom. mean** | | **1.86×** | **1.72×** | **1.40×** | **−8 %** (host NaN −25 %) |

Differences below 10 % are noise (repeated measurements of the same binary were up to 7 % apart);
interp was at 96–99 ms in all four measurements of Part 3 versus 84 ms, which is real. Measurement script for interleaved
minima: call `measure.py --node` repeatedly per variant, or start the variants alternately as above.

Values below 1× (lifted code faster than natively compiled code): the guest compiler optimized differently for AArch64
than Emscripten did for Wasm; this says nothing about the lifter.

## What the lifter does now

### Registers in local variables (`LiftOptions::register_locals`, on by default)

Previously every instruction read and wrote `ctx->x[n]`. Because every guest memory access goes through a pointer into linear
memory, the host compiler has to assume that it modifies `CpuState`: after every store all registers are
reloaded. Now each function declares the registers, SP, NZCV (`lgp::lifted::Nzcv`, four `bool`s) and FPCR that it
uses as locals (`lgp_x5`, `lgp_sp`, `lgp_f`, `lgp_fpcr`) and writes them back only at places where someone
else can see them:

| Place | write back (`out`) | reload (`in`) |
|---|---|---|
| `BL`, `BLR`, `SVC`, `setjmp` | yes | yes, all used ones |
| `RET`, tail call, `BR` without known targets, `BRK`, undefined instruction | yes | – |
| memory access with `precise_fault_pc` (tests) | yes | – |
| backedge | only on the rare poll path (see below) | – |

The emitter writes markers (`/*@so*/`, `/*@si*/` …) into the text; `Lifter::lift` replaces them at the end, once it is known
what the function uses. Without the option the declarations are references (`uint64_t& lgp_x5 = ctx->x[5]`), the old
code form with the same emitter.

**Write back only what is dirty.** A forward data-flow analysis over the instructions determines which registers may have been
written since the last full sync (union over all paths; instructions not reached count
as completely dirty). Without it every call writes back all used registers; calls became twice as
expensive.

**Loops with calls.** In a loop that contains a call, almost everything would be dirty at the loop header
(union with the path before the loop). The edges **into** such a natural loop therefore write back in advance
everything that the loop itself does not make dirty, and the analysis skips these edges. Applied to all
forward edges, this cost the interpreter time; it only pays off for loops with calls.

**Faults in release mode.** Without `precise_fault_pc`, stale registers are in `CpuState` at a memory fault.
Not critical for a game (a fault ends it); for debugging use the mode of the tests. The fuzzer checks both
forms (`--release-shape`: if both sides fault, only the result counts).

### Backedges without synchronization

`LGP_BACKEDGE_SYNC(out, in)` is the poll on every backedge (process exit, baton scheduling for spin-waits without
SVC). The runtime only polls every 16,384th edge. Three findings:

1. **Even sync code that never runs cost up to 2× in V8.** The cold path `out; poll; in` contained about
   40 memory operations per edge. Natively, clang moves it out of the loop; V8 still allocated the registers of the hot loop
   worse (strings 31 → 20 ms, CRC32, interpreter). The poll reads no guest registers, so the
   sync is dropped entirely (`guest.hpp`). If a future poll needs the registers (debugger, `GetThreadContext3`), it has to come back.
2. **Counter in a local instead of `thread_local`.** Every function with backedges declares `lgp_backedges`; counting is
   per call instead of per thread. A `thread_local` costs a load and a store per iteration (in Wasm additionally via
   `__tls_base`) and a dependency through memory. Interpreter in Wasm 48 → 36 ms, strings 21 → 17 ms. So that
   endless execution across many short calls or tail-call cycles does not stay below the threshold,
   `LGP_BACKEDGE_ENTRY()` additionally counts per thread at the entry of every function with a loop (one `thread_local` per call,
   not per iteration; costs fib +2 % instructions).

3. **No call in the loop (Part 2).** In V8 all registers are caller-saved. If a loop contains a
   call across which values live, V8 allocates their registers worse, even if the call never runs
   (`if (rare) { if (backedge_poll(ctx)) return; }`). Manual experiments on CRC32 (Wasm, `--scale 3`, ms):

   | Variant of the poll | CRC32 |
   |---|---:|
   | call `backedge_poll` without sync (Part 1) | 43.5 |
   | down-counter instead of `& 0x3FFF` | 45.5 |
   | check a global flag, call only then | 44 |
   | no poll | 33 |
   | check a global flag, then `return` (no call) | 33.5 |
   | sync of all registers, call, `return` | 34.5 |
   | sync, call, reload, continue (as before P6) | 30.5, but interpreter +47 %, strings +97 % |

   So what matters is whether values are still needed after a call in the loop. The poll therefore only
   checks `g_backedge_stop` (set at process exit) and then returns with `kHaltExit`. Passing on the baton in
   baton mode does need a call, though; `LGP_BACKEDGE_YIELD=1` (for targets with lifted test programs that wait on a variable in
   baton mode, `tests/runtime`) keeps it. Without the option, a spin-wait without SVC and without a call hangs in baton mode;
   the game default core-baton never yielded on backedges.
   Function entries (`LGP_BACKEDGE_ENTRY`) still call `backedge_poll`; nothing is live there yet.
   Re-entering the middle of the function after the poll (tail call back to the loop header) would be the clean solution
   for baton, but it needs guaranteed tail calls; the Wasm build does not have `-mtail-call` on.

### Vector FP on the host

`fp2v`/`fp2v_elem` (add, sub, mul, div on `float`/`double` lanes) compute all lanes in one pass with host FP
when FPCR allows the fast path (`fp::fast_path`: round to nearest, FZ = 0); if a lane yields NaN,
the whole operation is computed via the lane path with A64 NaN rules. Native mat4 −23 %, particles −40 % (together with the
locals). In Wasm no measurable gain for mat4.

`-DLGP_FP_STATS=ON` counts how many FP operations take the software path and which FPCR values occurred; the
boot program prints it at the end (`fp: soft path N of M operations (fpcr seen: 0x...)`). If a game runs with FZ = 1,
**every** FP operation goes through the software path (7 to 35 times slower); then an FZ fast path is the biggest
lever.

### Memory accesses

In the release build the runtime does not check guest accesses (`LGP_GUEST_MEM_CHECKED` only in Debug); that was already the case before P6.
The remaining check in the lifter (`precise_fault_pc`, sync before every access) is off in the game build.

**Address on 32-bit hosts as `bias + va` (Part 2).** The translation `base + (uint32_t)(va - guest_base)` is, in
wasm32 modulo 2³², the same as `(base - guest_base) + va`. In the first form, for addresses with a
constant part (`adrp` + offset, now visible as a constant thanks to the locals), LLVM kept the subtraction and two additions
per access, right in the dependency chain of a table lookup. `GuestView::bias` is computed once at publication;
the constant moves into a loop-invariant value. CRC32 in Wasm 39 → 33 ms, without any other change.
Natively (64 bit) the truncation to 32 bits stays (ADR-0001: the 4 GiB block catches wild addresses).

### NZCV at function boundaries per AAPCS64 (Part 2)

Under the procedure call standard the flags are undefined at function entry and after a call; compiled
code never reads them there. `LiftOptions::flags_across_calls = false` (default of `lgp_lift`, `--exact-flags` turns
it off) therefore does not write them back at `BL`, `BLR`, `RET` and tail calls, does not reload them after calls, and
starts every function with `Nzcv{}`. Per call this drops four byte stores and loads on each side and the
computation of N/C/V that was otherwise only needed for the store. SVC, BRK, indirect branches without a known target and
`setjmp` still sync the flags exactly. Native vcall 81 → 71 million instructions, fib 67 → 52.

Measured and rejected in the same round: **not** reloading callee-saved registers (x19–x29, SP) after calls
saves nothing; fib even got slower (56.8 instead of 51.9 million instructions): a value that has to live across the call in a
host register costs a save and restore in the host prologue; reloading from `CpuState` is
cheaper. Leaving out dead reloads after calls does not help in Wasm either; LLVM moves them out of the
loop by itself.

Risk: hand-written assembly that passes flags across a call would be translated incorrectly. In the SDK this
is not known to happen; tests that compare the flags against an oracle after a `RET` lift with `--exact-flags`. The
fuzzer checks the option with real calls ([`lifter-fuzzing.md`](lifter-fuzzing.md)).

### Vector registers in local variables (Part 3)

Like the X registers, `v0`–`v31` now also live in locals (`lgp::V128 lgp_v5 = ctx->vreg[5];`), with the same
data-flow analysis (state mask 66 bits, `std::bitset`). Counted as a write are assignments to the register or
its halves (`.lo`, `.hi`, also compound ones like `|=`), `&lgp_vN` as an argument and the first argument of
`set_lane<…>(`. The structure loads and stores (`LD1`–`LD4`, `ST1`–`ST4`, single lane, replicate) get a list of pointers or values
instead of a register number and `ctx->vreg` (`VRefs`/`VVals` in `simd_ldst.hpp`); previously
every such instruction forced a full sync.

Effect: native mat4 −28 %, particles −15 %, memops −17 % instructions (memops: the guest `memcpy` copies with `LDP`/`STP` of
Q registers). In Wasm memops −43 %, mat4 −19 %, particles −18 %.

**Counter-effect in the interpreter (Wasm +15 %).** The interpreter kernel uses `v0` only at the edge (return value),
but the local lives until `RET` and competes in V8 with the hot values of the dispatch loop for registers. Natively
it does not matter (with `--exact-registers` 42.7 versus 42.8 million instructions in Part 2). Not fixed; an
attempt to write such registers back right when they are written fixed the interpreter and made everything else
worse (see "Tried and rejected").

### Volatile registers at `RET` (Part 3, `LiftOptions::registers_across_calls`)

As with the flags (Part 2), AAPCS64 says: x9–x17, v4–v7 and v16–v31 are undefined after a call, and a
caller does not read them there. `RET` therefore does not write them back (default of `lgp_lift`;
`--exact-registers` turns it off, `--exact-calls` both, flags and registers). At `BL`/`BLR` everything
dirty is still written back and everything used is reloaded: the callee may read callee-saved registers
(frame records, stack arguments via SP). Native interp −12 % instructions (after Part 3 without this option 42.7 million),
otherwise ±2 %. Risk as with the flags: hand-written assembly that returns values in volatile registers.

### FP NaN on the host (Part 3, `LGP_FP_HOST_NAN`, default for `lgp_game`)

The FP fast path (`fp::add/sub/mul/div/sqrt`, vector form `fp2v_host`) computes with host FP and then checks every
result for NaN in order to apply the A64 rules (operand priority, quieting, positive default NaN; x86 yields
a negative default NaN, Wasm leaves sign and payload open). This one check per operation costs more in V8
than the computation: it branches per lane, prevents merging and lengthens live ranges. mat4 in Wasm
137 → 30 ms, particles 158 → 94 ms, native mat4 −34 % instructions.

`LGP_FP_HOST_NAN` (CMake, on by default, applies only to `lgp_game`; `-DLGP_FP_HOST_NAN=OFF` turns it off; bench variant
`lgp_bench_lifted_hostnan`) leaves the check out. What stays exact: whether a result is NaN, comparisons, conversions,
rounding, FZ (still goes through the software path), FPCR.DN = 1 (with the option it also goes through the
software path). Only sign and payload of a NaN result can differ. Game code that evaluates NaN bits
(NaN boxing, bit comparison with a NaN constant) would be affected; with normal graphics and physics code
this is not to be expected. The lifter tests and the fuzzer compare NaN bits against Unicorn and therefore run
without the option, as do the test boot programs (fixtures, `tests/boot`).

**On the real game (PC, Analysis 18, 2026-10-12):** with the option, frames do not differ more than between two
runs without it, no NaN artifacts, log identical. Natively equally fast (the game is bound to the frame rate), in
Chromium median 15 % faster, minimum 4 %. That is why it has been the default for `lgp_game` since P6. **Open:** compare saves
once the game saves (Analysis 18, section 2); if they differ, `-DLGP_FP_HOST_NAN=OFF`.

### FRSQRTE/URSQRTE without a counting loop (2026-10-20)

The Arm ARM describes `RecipSqrtEstimate()` as a loop: `b` counts up from 512 as long as `a·(b+1)² < 2²⁸`. Depending
on the input that is up to ≈ 500 steps per lane. `fp::rsqrt_estimate_int` had taken this over literally. In an
Instruments recording of the Apple TV (frames 7900–8250, host clock), `simd::fp1v<FpUn::RsqrtE, uint32_t>` was the biggest leaf function with 12.7 % of the
samples. Now the function computes the smallest integer `c` with `a·c² ≥ 2²⁸` from the
host square root and corrects it with integer arithmetic to the exact value. After that `b = max(512, c−1)`. The result is
bit-identical to the loop for every input (`lifter.fp_estimate` checks all of them, plus FRSQRTE in `lifter_simd` against
Unicorn). On x86 the time drops from ≈ 157 to ≈ 10 ns per call, measured in the unit test, not in the game. What this
gains in frame time on the Apple TV has not been measured.

## Tried and rejected

- **Keeping callee-saved registers in locals across calls (Part 3).** Not reloading x19–x29 and SP after `BL`, because the
  callee restores them. The first version also did not write them back before the call; within
  a few hundred cases the fuzzer found that a callee may read them (regression line in
  `tests/lifter_fuzz/regressions.txt`). Correct with write-back, but: Wasm ±0 (1.73× versus 1.73×), native fib
  +9 %, vcall +2 % instructions. This confirms the finding from Part 2 – a value that lives across a call in a
  host register costs a save in the host prologue; reloading is cheaper. Rejected.
- **Making the NaN check cheaper instead of leaving it out (Part 3):** branch-free selection per lane (mat4 in Wasm 298 ms instead of 137,
  too many ALU instructions and no more inlining), cold call outside the function (183 ms), repairing
  only the NaN lanes afterwards (more instructions natively). Only leaving it out helps.
- **`-msimd128`** for the lifted code: no measurable gain.
- **Writing vector registers back right when they are written (follow-up to Part 3).** For V registers that are not live at any
  loop header (backward liveness over the instructions), a `ctx->vreg[N] = lgp_vN;` directly after every write,
  and in exchange no write-back at `RET` and sync points. The goal was the interpreter regression. Wasm: interp 97 → 89 ms
  (Part 2: 84), but mat4 138 → 175, memops 19.5 → 30, vcall 218 → 245, geom. mean 1.72× → 1.86×. Restricted to rarely
  used registers (at most every 16th instruction of the function uses it): interp likewise better, mat4 +7 %,
  memops +33 %, vcall +12 %, geom. mean 1.79×. The extra stores cost more in V8 than the long live range;
  without a runtime profile it is impossible to tell which case applies. Rejected.
- **strings +9 % in Wasm due to missing stores at `RET`.** The kernel uses no vector registers; the only
  difference from Part 2 is the nine left-out stores of x9–x17 at the two `RET`s. With them (manual experiment on the
  generated code) 33.1 → 29.9 ms, i.e. back at the Part 2 level. Less code is slower here, presumably due to a different
  register allocation in V8. With `--exact-registers` versus the default, the geometric mean in Wasm stays the same
  (1.72× versus 1.73×), and natively the default saves 12 % in the interpreter; that is why it stays.

- **Reloading only live registers after calls** (backward liveness): no measurable gain, because loops carry their
  constants in registers across the call and the conservative liveness at instruction level keeps almost everything
  live. Instructive bug along the way: "dirty" is a union over paths; a write-back after a union
  also stores registers that are clean on the current path but not reloaded. Write-back sites therefore count
  as reads. The fuzzer found this immediately (regression line in `tests/lifter_fuzz/regressions.txt`).
- **Locals only in functions with a loop or without calls:** changed nothing measurable in the benchmark.

## Limits and next levers

1. **Calls** (native vcall 6.4×, fib 4.7×): per call, dirty registers written back, used ones reloaded, table lookup
   for `BLR`, plus prologue and epilogue of the host function. Open paths: arguments x0–x7 and return value x0 as
   C++ parameters between lifted functions (the exact mapping of `CpuState` then only remains at SVCs and
   exits), inlining of small leaves by the recompiler, cheaper dispatch lookup.
2. **FP NaN check:** after the PC check
  , `LGP_FP_HOST_NAN` is the default for `lgp_game`;
   the save comparison is pending until the game saves.
   Fix the interpreter regression caused by the `v0` local (Part 3).
3. **FZ fast path**, if `LGP_FP_STATS` shows FZ = 1 on the real game.
4. **FMA** has no host fast path (x86 baseline and Wasm without FMA).
5. **Wasm tail calls** (`-mtail-call`): would allow a poll with baton hand-off without a call in the loop and
   guaranteed tail calls (ADR-0004); check browser support first.
6. **Code size:** +36 % C++ (declarations and sync code) since before P6; Part 3 native `.text` +19 % (120 → 144 KB, cause
   not investigated), with `LGP_FP_HOST_NAN` 104 KB, the Wasm file gets smaller. For a whole game this costs translation time; measure on the
   real `main` (Analysis 12).

## Profile-guided optimisation (PGO)

`-DLGP_GAME_PGO=generate` builds the lifted translation units of `lgp_game` with clang's instrumentation.
A run that ends normally (e.g. `--frames N`) writes the profile, `llvm-profdata merge -o game.profdata *.profraw`
combines runs, and `-DLGP_GAME_PGO=<game.profdata>` optimises with it. Runtime and HLE stay without a profile; the
guest time is in the lifted code.

- Only clang (Apple clang under Xcode included). On Linux the instrumentation needs the profile runtime
  (`libclang_rt.profile`, Ubuntu `libclang-rt-<N>-dev`); the Android NDK ships it.
- The instrumented `lgp_game` writes the profile itself after the run (`__llvm_profile_write_file`, line
  `pgo: profile written to …, rc 0`): to `LLVM_PROFILE_FILE` if set (also via `--env`), in app mode otherwise to
  `<log dir>/game-%p.profraw`, else the runtime's default. Apps never run the runtime's exit handler, and the
  runtime reads `LLVM_PROFILE_FILE` before `--env` sets it (Shield, 2026-10-28: an empty folder, with the variable
  a 0-byte file).
- A profile only fits the same lift output and the same clang version. Functions whose code changed get no data;
  clang warns with `-Wprofile-instr-out-of-date`/`-Wprofile-instr-unprofiled`, the warnings stay on. A wrong
  profile only makes the build slower, not wrong.
- Never automatic: measurement builds must not pick up a profile unnoticed. Profiles come from game code and stay in
  `$LGP_PRIVATE_DIR`.
- Mac M1 (2026-10-20, Route 1 with `--clock virtual` as the profile, ABAB with a warm-up run, frames 7901–8250,
  offscreen): frame hashes equal, guest time per thread −15 to −20 % (T1 0.75 → 0.61 ms per frame), median fps
  unchanged (≈ 250, the guest no longer limits there), p10 +6 %.
- NVIDIA Shield (2026-10-28/29, NDK clang, profile from a `--clock virtual` run of Route 1, new game, ABAB 2 + 2
  runs with `--clock host`): +13 % fps over frames 701–1500. Images with `--clock virtual --frame-grid` over 1600
  frames: a PGO run equals a run without PGO in every compared frame (300–1600). Frame 900 has two possible images
  in both builds (also between two runs without PGO), so one comparison per build is not enough there.

## Real game

First measurement on the PC (i7-13700K, clang 23, 2026-10-12), details in
`../analysis/12-p6-leistung-pc-messung.md` section 6:

- In the game up to the language selection, the new code form is only 3–5 % faster than the old one, below the 5 % noise threshold:
  a measurable gain in the game is not proven. There the
  lifted code is not the limit (over 100 frames/s without V-Sync). Scenes in the game itself are still missing.
- `--exact-flags` costs nothing measurable in the game, but makes `lgp_game.exe` 9 % larger and the build 18 % longer.
- New code form: 25 % more C++ (1,110 versus 888 MB), build time almost the same (150 versus 140 s with `-j 8`).
- FZ and rounding mode have never been set in LGP so far; the FP software path never runs (`LGP_FP_STATS` does not count other FPCR bits
  such as DN).
- On real hardware `crc32` is slower with the new code form than with the old one (1.4× versus 1.1× native).

## Correctness

Every change ran against the lifter fuzzer ([`lifter-fuzzing.md`](lifter-fuzzing.md)), at least 15 minutes with GCC and
Clang, with and without `precise_fault_pc`, without a mismatch. Since P6 the fuzzer also generates `SVC`s whose handler changes x0–x7;
only with that does it check the reloading after calls. Planted bugs (forgetting flags on write-back; reloading
nothing after an `SVC`) it found within a few hundred cases.

Part 2: the fuzzer now also generates real calls (`BL` into a second lifted function, `RET`), in the release shape
with AAPCS64 flags. 15 minutes each of GCC (precise) and clang (release shape): 51,200 cases, no mismatch. A
planted bug (not reloading `x0` after calls) showed up in 8 of 800 cases.

Part 3: the fuzzer overwrites the volatile registers after every `BL` (the caller must not read them), and
`lifter_fuzz.driver_release` also replays the regression lines in the release shape. 15 minutes each of GCC and clang in the
release shape: 63,200 cases, no mismatch. The rejected attempt with callee-saved registers showed up after 800 cases
(see above).
