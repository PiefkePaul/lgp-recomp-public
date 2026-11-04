// Runtime header for lifted (generated) code: the P2.6 contract.
//
// recomp/lifter's lifted.hpp includes this after lgp/lifted/cpu_state.hpp
// (selected via LGP_RUNTIME_HEADER), so lgp::CpuState is complete here and the
// macros below may use the variable `ctx` (a lgp::CpuState*) that is in scope
// in every lifted function. The macro list and their meaning are defined in
// lifted.hpp; this file only implements them.
//
// Memory accesses go through lgp::rt::load/store (guest_mem.hpp, ADR-0001).
// SVCs go straight into the active Kernel as a function call, no trap.
#pragma once

#include <atomic>
#include <csetjmp>
#include <cstdint>

#include "lgp/runtime/dispatch.hpp"
#include "lgp/runtime/guest_mem.hpp"
#include "lgp/runtime/hooks.hpp"
#include "lgp/runtime/host_jmp.hpp"
#include "lgp/runtime/kernel.hpp"
#include "lgp/runtime/patch.hpp"
#include "lgp/runtime/system.hpp"

inline uint8_t lgp_mem_read8(uint64_t va) { return lgp::rt::load<uint8_t>(va); }
inline uint16_t lgp_mem_read16(uint64_t va) { return lgp::rt::load<uint16_t>(va); }
inline uint32_t lgp_mem_read32(uint64_t va) { return lgp::rt::load<uint32_t>(va); }
inline uint64_t lgp_mem_read64(uint64_t va) { return lgp::rt::load<uint64_t>(va); }
inline void lgp_mem_write8(uint64_t va, uint8_t v) { lgp::rt::store<uint8_t>(va, v); }
inline void lgp_mem_write16(uint64_t va, uint16_t v) { lgp::rt::store<uint16_t>(va, v); }
inline void lgp_mem_write32(uint64_t va, uint32_t v) { lgp::rt::store<uint32_t>(va, v); }
inline void lgp_mem_write64(uint64_t va, uint64_t v) { lgp::rt::store<uint64_t>(va, v); }

inline bool lgp_mem_cas32(uint64_t va, uint32_t expected, uint32_t desired) {
    return lgp::rt::compare_and_swap<uint32_t>(va, expected, desired);
}
inline bool lgp_mem_cas64(uint64_t va, uint64_t expected, uint64_t desired) {
    return lgp::rt::compare_and_swap<uint64_t>(va, expected, desired);
}
inline bool lgp_mem_cas128(uint64_t va, uint64_t exp_lo, uint64_t exp_hi, uint64_t new_lo, uint64_t new_hi) {
    return lgp::rt::compare_and_swap_128(va, exp_lo, exp_hi, new_lo, new_hi);
}

namespace lgp::rt {

// Unknown indirect target: the halt is set, the caller's halt check unwinds.
inline void halted_target(CpuState*) {}

// Target of BLR/BR (ADR-0004): the dispatch table, then the bring-up
// resolver. A miss is recorded (dispatch.hpp: unresolved_config()) and halts
// the guest with kHaltBadJump, halt_info = ctx->pc = va. Never nullptr.
inline LiftedFn resolve_indirect(CpuState* ctx, uint64_t va) {
    if (LiftedFn fn = table_lookup(va)) [[likely]]
        return fn;
    if (LiftedFn fn = detail::resolve_slow(va, ctx->pc))
        return fn;
    ctx->pc = va;
    if (ctx->halt == kHaltNone) {
        ctx->halt = kHaltBadJump;
        ctx->halt_info = va;
    }
    return &halted_target;
}

inline void call_indirect(CpuState* ctx, uint64_t va) { resolve_indirect(ctx, va)(ctx); }

// SVC from lifted code. halt_info on an exit names the svc that ended the
// guest (0x07 ExitProcess, 0x0A ExitThread, 0x26 Break, or the svc during
// which the process started ending); an svc the runtime lacks halts as an
// undefined instruction with the SVC's encoding as halt_info.
inline void svc_from_lifted(CpuState* ctx, uint32_t imm) {
    switch (active_kernel().svc(imm, ctx->x)) {
    case SvcOutcome::Continue:
        return;
    case SvcOutcome::ExitProcess:
    case SvcOutcome::Break:
    case SvcOutcome::ExitThread:
        if (ctx->halt == kHaltNone) {
            ctx->halt = kHaltExit;
            ctx->halt_info = imm;
        }
        return;
    case SvcOutcome::Unimplemented:
        if (ctx->halt == kHaltNone) {
            ctx->halt = kHaltUndefined;
            ctx->halt_info = 0xD4000001u | (imm << 5);
        }
        return;
    }
}

inline thread_local uint32_t t_backedges = 0;
// Defined in scheduler.cpp. True: the thread must leave guest code (halt set).
bool backedge_poll(CpuState* ctx);
// Non-zero while the active process is exiting (Scheduler::request_exit,
// cleared by every new Scheduler). Read by the call-free back-edge poll.
extern std::atomic<uint32_t> g_backedge_stop;

// Runs a lifted function with guest memory faults turned into a halt: a
// faulting access sets ctx->halt = kHaltMemFault (halt_info = address) and
// unwinds straight back here via longjmp, as lifted.hpp requires (accessors
// never return on a fault). Lifted code has no objects with destructors, so
// the longjmp skips nothing. Nests: the previous handler is restored.
// The CpuState of the guest code running on this thread (innermost
// run_lifted), for diagnostics such as write watchpoints; nullptr on host
// threads.
inline thread_local CpuState* t_running_ctx = nullptr;

inline void run_lifted(CpuState* ctx, LiftedFn fn) {
    struct Guard {
        std::jmp_buf env;
        CpuState* ctx;
    };
    Guard guard;
    guard.ctx = ctx;
    const FaultHandler previous = set_fault_handler({[](uint64_t va, unsigned, bool, void* user) {
                                                         auto* g = static_cast<Guard*>(user);
                                                         if (g->ctx->halt == kHaltNone) {
                                                             g->ctx->halt = kHaltMemFault;
                                                             g->ctx->halt_info = va;
                                                         }
                                                         std::longjmp(g->env, 1);
                                                     },
                                                     &guard});
    CpuState* const outer = t_running_ctx;
    t_running_ctx = ctx;
    if (setjmp(guard.env) == 0)
        fn(ctx);
    t_running_ctx = outer;
    set_fault_handler(previous);
}

}  // namespace lgp::rt

#define LGP_CALL_INDIRECT(va) ::lgp::rt::resolve_indirect(ctx, (va))(ctx)
// Indirect tail call: returns from the lifted function. LGP_MUSTTAIL comes
// from lifted.hpp (guaranteed tail call where the compiler supports it).
#define LGP_JUMP_INDIRECT(va)                                         \
    do {                                                              \
        LGP_MUSTTAIL return ::lgp::rt::resolve_indirect(ctx, (va))(ctx); \
    } while (0)
// setjmp/longjmp by the host variant (ADR-0004), see host_jmp.hpp.
#define LGP_SETJMP()                                                  \
    do {                                                              \
        ::lgp::rt::HostJmp& lgp_hj_ = ::lgp::rt::host_jmp_save(ctx); \
        if (setjmp(lgp_hj_.env) == 0)                                 \
            ctx->x[0] = 0;                                            \
    } while (0)
// Hook door (hooks.hpp): without a hook a tail call of the lifted original.
#define LGP_HOOKED(va, orig)                                                  \
    do {                                                                      \
        if (const ::lgp::rt::detail::HookSlot* lgp_hook_ = ::lgp::rt::find_hook(va)) [[unlikely]] { \
            lgp_hook_->fn(ctx, &orig, lgp_hook_->user);                       \
            return;                                                           \
        }                                                                     \
        LGP_MUSTTAIL return orig(ctx);                                        \
    } while (0)
#define LGP_HOOKABLE(entries, n)                                                              \
    [[maybe_unused]] static const bool lgp_hookable_declared_ =                              \
        (::lgp::rt::declare_hookable((entries), (n)), true);
#define LGP_LONGJMP() ::lgp::rt::host_longjmp(ctx)
#define LGP_SETJMP_INDIRECT() ::lgp::rt::host_setjmp_indirect(ctx)
#define LGP_SVC(imm) ::lgp::rt::svc_from_lifted(ctx, (imm))
// Patch sites of game mods (patch.hpp).
#define LGP_PATCH_ACTIVE(va) ::lgp::rt::patch_active(va)
#define LGP_PATCH(va) ::lgp::rt::run_patch(ctx, (va))
#define LGP_BRK(imm)                         \
    do {                                     \
        if (ctx->halt == ::lgp::kHaltNone) { \
            ctx->halt = ::lgp::kHaltBrk;     \
            ctx->halt_info = (imm);          \
        }                                    \
    } while (0)
#define LGP_UNDEFINED(raw)                     \
    do {                                       \
        if (ctx->halt == ::lgp::kHaltNone) {   \
            ctx->halt = ::lgp::kHaltUndefined; \
            ctx->halt_info = (raw);            \
        }                                      \
    } while (0)
#define LGP_YIELD() ::lgp::rt::guest_yield()
// Every 16384th loop back-edge of a thread: leave guest code when the process
// ends, and in Baton mode let other threads run (spin-waits without svc).
#define LGP_BACKEDGE()                                                                                 \
    do {                                                                                               \
        if ((++::lgp::rt::t_backedges & 0x3FFFu) == 0 && ::lgp::rt::backedge_poll(ctx)) [[unlikely]] \
            return;                                                                                    \
    } while (0)
// What the lifter actually emits on a back-edge (lifted.hpp). It counts in
// the function's own lgp_backedges (lifted.hpp): every 16384th back-edge of
// one invocation. LGP_BACKEDGE_ENTRY adds every invocation of a function with
// loops to the per-thread count, so endless execution spread over many short
// invocations (calls, tail-call cycles) is polled as well.
//
// LGP_BACKEDGE_YIELD selects what a back-edge may do
// (docs/knowledge/lifted-code-performance.md):
//   0 (default)  leave guest code when the process exits: a load of
//                g_backedge_stop and a return, no call. V8 keeps the values of
//                a loop in registers only if the loop contains no call at all,
//                even one that never runs: the call version costs tight loops
//                20-30 % in WebAssembly (CRC32 +30 %, interpreter +27 %).
//   1            call backedge_poll(): additionally hands the baton over in
//                Baton mode, so spin-waits without svc and without calls do
//                not hang there (tests/runtime spin test). Function entries
//                (LGP_BACKEDGE_ENTRY) call it in both variants.
// Neither variant syncs the registers held in locals (`out`/`in`): the poll
// reads none, and the sync code costs WebAssembly up to 2x even unexecuted.
#ifndef LGP_BACKEDGE_YIELD
#define LGP_BACKEDGE_YIELD 0
#endif
#define LGP_BACKEDGE_ENTRY()                                                                           \
    do {                                                                                               \
        if ((++::lgp::rt::t_backedges & 0x3FFFu) == 0 && ::lgp::rt::backedge_poll(ctx)) [[unlikely]] \
            return;                                                                                    \
    } while (0)
#if LGP_BACKEDGE_YIELD
#define LGP_BACKEDGE_SYNC(out, in)                                        \
    do {                                                                  \
        if ((++lgp_backedges & 0x3FFFu) == 0) [[unlikely]] {              \
            if (::lgp::rt::backedge_poll(ctx)) return;                    \
        }                                                                 \
    } while (0)
#else
#define LGP_BACKEDGE_SYNC(out, in)                                                                  \
    do {                                                                                            \
        if ((++lgp_backedges & 0x3FFFu) == 0 &&                                                     \
            ::lgp::rt::g_backedge_stop.load(std::memory_order_relaxed) != 0) [[unlikely]] {         \
            if (ctx->halt == ::lgp::kHaltNone) {                                                    \
                ctx->halt = ::lgp::kHaltExit;                                                       \
                ctx->halt_info = 0;                                                                 \
            }                                                                                       \
            return;                                                                                 \
        }                                                                                           \
    } while (0)
#endif
#define LGP_BARRIER() std::atomic_thread_fence(std::memory_order_seq_cst)
#define LGP_READ_SYSREG(enc) ::lgp::rt::read_sysreg(enc)
#define LGP_TRACE_BLOCK(pc) ((void)(pc))
