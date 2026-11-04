// Single include for generated (lifted) C++ code.
//
// Generated code depends on three things only:
//   1. CpuState (cpu_state.hpp), passed as `ctx` to every lifted function;
//   2. the pure helpers in helpers.hpp;
//   3. a runtime header that supplies memory access, dispatch and runtime
//      calls. It is selected with LGP_RUNTIME_HEADER so the test harness can
//      plug in its own stub; the real one is runtime/core (P2.6, ADR-0001/0004).
//
// Contract of the runtime header (all macros may use the variable `ctx`, a
// lgp::CpuState*, which is in scope in every lifted function):
//
//   uint8_t  lgp_mem_read8 (uint64_t va);   ... read16/read32/read64
//   void     lgp_mem_write8(uint64_t va, uint8_t v); ... write16/write32/write64
//   bool     lgp_mem_cas32 (uint64_t va, uint32_t expected, uint32_t desired);
//   bool     lgp_mem_cas64 (uint64_t va, uint64_t expected, uint64_t desired);
//   bool     lgp_mem_cas128(uint64_t va, uint64_t exp_lo, uint64_t exp_hi, uint64_t new_lo, uint64_t new_hi);
//                           On an unmapped or protected address these must NOT return normally
//                           (trap/abort, C++ exception or longjmp): generated code does not check
//                           ctx->halt after memory accesses, so a returning hook would let the
//                           instruction and its successors run on with a made-up value. ctx->pc
//                           is exact at that point only with LiftOptions::precise_fault_pc.
//   LGP_CALL_INDIRECT(va)   BLR, or BL to an address without a known function; x30 is already set,
//                           ctx->pc holds the branch. An unknown target must set ctx->halt.
//   LGP_JUMP_INDIRECT(va)   BR without a resolved target, or B out of the function to a non-entry:
//                           an indirect tail call. May return from the lifted function itself
//                           (the real runtime does, with LGP_MUSTTAIL); the lifter emits `return;`
//                           after it for runtimes that only call.
//   LGP_SETJMP()            BL to the guest's setjmp (LiftOptions::setjmp_entries), x0 = jmp_buf;
//                           ADR-0004 host variant, must expand to a host setjmp in this frame
//   LGP_LONGJMP()           body of the replaced guest longjmp (code generator, not the lifter)
//   LGP_SETJMP_INDIRECT()   body of the replaced guest setjmp: reached only by BLR/tail call, an error
//   LGP_SVC(imm)            SVC; ctx->pc holds the address after the SVC
//   LGP_BRK(imm)            BRK; ctx->pc holds the BRK's address; must set ctx->halt
//   LGP_UNDEFINED(raw)      unsupported/undefined instruction at ctx->pc; must set ctx->halt
//   LGP_YIELD()             YIELD/WFE/WFI hint (spin-wait loops; scheduling point in baton mode)
//   LGP_BARRIER()           DMB/DSB and the ordering of LDAR/STLR/LDAXR/STLXR
//   LGP_READ_SYSREG(enc)    MRS of CNTFRQ_EL0/CNTPCT_EL0/CNTVCT_EL0 (enc = a64::sysreg packing)
//   LGP_TRACE_BLOCK(pc)     only emitted with LiftOptions::block_trace (ADR-0006 block trace)
//   LGP_BACKEDGE()          on every loop back-edge (LiftOptions::backedge_hook); may set ctx->halt
//                           and return from the lifted function. Preemption/yield point for
//                           spin-waits without SVC (ADR-0002) and a runaway-loop guard in tests.
//                           Optional: defaults to nothing.
//   LGP_BACKEDGE_SYNC(out, in)  what is actually emitted on a back-edge, see below. Optional.
//   LGP_BACKEDGE_ENTRY()    at the entry of every function with back-edges, see below. Optional.
//   LGP_PATCH_ACTIVE(va)    at a patch site (LiftOptions::patch_sites): true while a host function
//                           replaces the instruction at va (game mods). Optional: defaults to false.
//   LGP_PATCH(va)           the host function at va; ctx holds every register, ctx->pc = va + 4.
//                           Yields true if it replaced the instruction, false if the instruction
//                           still runs (a patch that only observes). The lifted code continues
//                           unless it set ctx->halt. Optional with LGP_PATCH_ACTIVE.
//   LGP_HOOKED(va, orig)    whole body of a hook door (code generator, `lgp_lift --hook`): the
//                           function at va was lifted as `orig`; the runtime may run a host hook
//                           instead. Optional: defaults to a tail call of orig.
//   LGP_HOOKABLE(entries, n) at file scope, once per module: the va of every hook door. Optional.
#pragma once

#include <cstdint>

#include "lgp/lifted/cpu_state.hpp"
#include "lgp/lifted/helpers.hpp"
#include "lgp/lifted/simd.hpp"

#ifndef LGP_RUNTIME_HEADER
#define LGP_RUNTIME_HEADER "lgp/runtime/guest.hpp"
#endif
#include LGP_RUNTIME_HEADER

// Needs the memory functions of the runtime header.
#include "lgp/lifted/simd_ldst.hpp"

#ifndef LGP_BACKEDGE
#define LGP_BACKEDGE() ((void)0)
#endif
// What the lifter emits on a back-edge: `out` writes the registers held in
// locals back to ctx, `in` reloads them (LiftOptions::register_locals; both
// empty otherwise). This fallback syncs on every back-edge; a runtime whose
// LGP_BACKEDGE only rarely does anything defines its own version that syncs
// only on that rare path. Functions with back-edges declare
// `uint32_t lgp_backedges = 0` for it: a counter in a local stays in a host
// register, a thread_local one costs a load and a store per iteration.
// LGP_BACKEDGE_ENTRY() runs once at the entry of such a function, so a runtime
// can also count invocations: endless execution through many short calls or
// tail calls never reaches the per-invocation threshold.
#ifndef LGP_BACKEDGE_ENTRY
#define LGP_BACKEDGE_ENTRY() ((void)0)
#endif
#ifndef LGP_BACKEDGE_SYNC
#define LGP_BACKEDGE_SYNC(out, in) \
    do {                           \
        out;                       \
        LGP_BACKEDGE();            \
        in;                        \
    } while (0)
#endif

#ifndef LGP_PATCH_ACTIVE
#define LGP_PATCH_ACTIVE(va) (static_cast<void>(va), false)
#define LGP_PATCH(va) (static_cast<void>(va), true)
#endif

// Direct tail calls (B to another function's entry). ADR-0004: guaranteed
// tail calls where the compiler can do them, otherwise call + return. Wasm
// needs the tail-call feature (-mtail-call) for musttail.
// LGP_HAS_GUARANTEED_TAIL_CALLS tells whether chains of tail calls run in
// constant host stack; without it they only do where the optimiser happens
// to turn them into jumps (GCC < 15 and MSVC, and every compiler at -O0).
#if defined(__clang__) && defined(__has_cpp_attribute)
#if __has_cpp_attribute(clang::musttail) && (!defined(__wasm__) || defined(__wasm_tail_call__))
#define LGP_MUSTTAIL [[clang::musttail]]
#endif
#elif defined(__GNUC__) && __GNUC__ >= 15 && defined(__has_cpp_attribute)
#if __has_cpp_attribute(gnu::musttail)
#define LGP_MUSTTAIL [[gnu::musttail]]
#endif
#endif
#ifdef LGP_MUSTTAIL
#define LGP_HAS_GUARANTEED_TAIL_CALLS 1
#else
#define LGP_MUSTTAIL
#define LGP_HAS_GUARANTEED_TAIL_CALLS 0
#endif
#define LGP_TAIL_CALL(fn)          \
    do {                           \
        LGP_MUSTTAIL return fn(ctx); \
    } while (0)

#ifndef LGP_HOOKED
#define LGP_HOOKED(va, orig) LGP_TAIL_CALL(orig)
#endif
#ifndef LGP_HOOKABLE
#define LGP_HOOKABLE(entries, n)
#endif

// Lifted code returns to its caller as soon as a runtime hook has set halt.
#define LGP_HALT_CHECK() \
    do {                 \
        if (ctx->halt)   \
            return;      \
    } while (0)
