# Patterns for a static AArch64→C++ lifter

Game-independent design patterns and testing technique from P2.3 (`recomp/lifter/`, `tests/lifter/`). Useful for any project
that translates machine code into a high-level language (recompilers à la N64Recomp/XenonRecomp).

## Output form

- **One C++ function per guest function**, uniform signature `void f(CpuState* ctx)`. Basic blocks are labels,
  jumps within the function are `goto`. Each instruction sits in its own `{ … }` block so that `goto` never jumps over
  a variable declaration (C++ forbids that).
- Only generate labels for actual jump targets (pre-pass), otherwise the compiler warns about unused labels.
- If the entry is not at the lowest address of the function, the function begins with `goto L_entry;`.
- `BL` to a known function → direct call, `RET` → `return` (assumes the code does not manipulate LR;
  check statistically beforehand, for LGP: `RET ≠ x30` does not occur). `B` to another function's start →
  tail call; guaranteed with `[[clang::musttail]]` so that tail call chains do not grow the host stack.
  WebAssembly needs the tail call feature for this (`-mtail-call`), otherwise a normal call + `return`.
- Compute constants at lift time (bit masks, shifted immediates, jump targets, PC values) instead of computing them in
  the generated code. The PC is only written into `CpuState` where someone can see it
  (runtime calls, halt, indirect jumps).

## One decoder for CFG and lifter

Function detection and lifter must see the same instructions the same way; otherwise a block ends in the CFG at a
different place than in the lifter, or a jump target differs. The CFG therefore classifies via a narrow
interface (`classify()` → control flow kind, target, register) on top of the bit-exact lifter decoder. The previous,
independent implementation remains as a cross-check test: comparing two implementations on millions of random words
found no difference for valid words during the switch, but showed how much more the bit-exact
decoder recognizes as invalid (around half of all random words that the coarse classifier let through as "no jump").
For the CFG this means: a block ends earlier (`Unknown`) when data lies in the code.

## Abstraction to the runtime

The generated code knows only a small list of names (`lgp/lifted/lifted.hpp`): `lgp_mem_read*/write*`,
`lgp_mem_cas32/64/128`, `LGP_CALL_INDIRECT`, `LGP_JUMP_INDIRECT`, `LGP_SVC`, `LGP_BRK`, `LGP_UNDEFINED`,
`LGP_YIELD`, `LGP_BARRIER`, `LGP_READ_SYSREG`, `LGP_TRACE_BLOCK`, `LGP_BACKEDGE`, plus `LGP_SETJMP`/`LGP_LONGJMP` for the
host variant of setjmp/longjmp (see [indirekte-spruenge-im-recompiler.md](indirekte-spruenge-im-recompiler.md)). Which runtime is behind it
is decided by an include (`LGP_RUNTIME_HEADER`). This lets memory model (32-bit offset vs. memory64),
dispatch and threads change without touching the lifter, and the test harness brings its own mini runtime.

## Aborting without longjmp

Runtime hooks (SVC, fault, exit) set `ctx->halt`. The generated code checks the field after each call and returns
immediately (`LGP_HALT_CHECK`). That costs one comparison per call, but works everywhere, including in
WebAssembly without exceptions, and lets the host stack unwind cleanly.

**Exception memory faults:** a halt check after every memory access would be too expensive (around 38 % of all instructions
are loads/stores). Therefore the `lgp_mem_*` hooks must **not return** on a fault (trap/abort,
C++ exception or longjmp). A hook that only sets a flag and returns 0 lets the destination register, writeback and
following instructions keep running; the code review of P2.3 found that, the test had no fault case. Since then the
group `mem_fault` checks that nothing takes effect after a fault. The hook sees the exact PC only with
`LiftOptions::precise_fault_pc` (one store per access, for tests and debug builds); on the real system a
memory fault in the game is fatal anyway.

## Loop hook

Every backward jump within a function gets `LGP_BACKEDGE()`. In the test it limits the run time (a bug
in the lifter would otherwise produce an endless loop instead of a red test); in the real runtime it is the point where
a baton scheduler can interrupt spin waits without SVC (ADR-0002). Without a definition it is empty.

## Exclusive accesses → CAS

See [a64-semantik-flags.md](a64-semantik-flags.md#exclusive-accesses). Important for the runtime interface: CAS on
32, 64 and 128 bits suffices; 8/16-bit exclusives go through the surrounding 32-bit word.

## Testing technique: random words per encoding group

Instead of assembling each instruction individually:

1. Per encoding group (e.g. "Add/Sub immediate") a mask of the fixed opcode bits; the remaining bits random.
2. Discard words the decoder rejects. What remains hits all operand combinations including
   edge cases (shift amounts, register widths, register 31).
3. For memory accesses, rewrite the register fields after rolling: base registers from a reserved set
   pointing to a valid data area; data registers from another set so that no load destroys a base;
   index registers with small positive or negative values matching the extend kind.
4. Random initial state, with a share of "interesting" values (0, ±1, `INT_MIN/MAX` in 32 and 64 bits,
   `0xFFFFFFFF`, `0x100000000`), so that flag edge cases occur often enough.
5. Run each case in the oracle (Unicorn) and as recompiled code, compare the entire state
   (registers, NZCV, vector registers, writable memory, SVC sequence, abort kind).
6. In addition templates for control flow (all conditions, loops, calls, tail calls, indirect jumps, SVC) and
   random structured programs (counted loop around a body with forward jumps of all kinds).
7. The generator aborts if an implemented operation is missing from the cases: this keeps the acceptance criterion
   "all implemented opcodes tested" machine-checked.

**Mutation testing to check the tests:** deliberately introduce a bug (carry computation, a condition,
an extend kind) and check that cases turn red. In P2.3 every checked mutation triggered in 10–26 cases.

## What generator and oracle do not cover

- Memory ordering between threads (not deterministically testable, ADR-0006).
- Implementation-defined behavior that Unicorn does differently from the A57 (exception list, see
  [unicorn-als-orakel.md](unicorn-als-orakel.md)); the hypervisor harness on the Mac is there for that (P2.7).
- Performance of the generated code: the first lifter works directly on `ctx->x[n]`; because guest memory accesses
  via `memcpy` can alias with `CpuState`, the compiler does not keep registers in host registers across memory
  accesses. Local register copies per function are the obvious optimization (P6.3).
