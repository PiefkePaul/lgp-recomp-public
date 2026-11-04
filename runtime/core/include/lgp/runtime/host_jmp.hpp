// Guest setjmp/longjmp by the host's setjmp/longjmp (ADR-0004).
//
// Lifted code keeps the host stack in step with the guest call stack (one C++
// frame per guest frame, RET = return), so a guest longjmp can unwind the host
// stack with the host longjmp. The recompiler replaces the guest's setjmp and
// longjmp by symbol (ADR-0005):
//   - BL setjmp becomes LGP_SETJMP() inline in the calling lifted function,
//     because the host setjmp must run in the frame the guest setjmp returns
//     to. It saves the AAPCS64 callee-saved state (x19-x30, sp, d8-d15) under
//     the address of the guest jmp_buf (x0) and returns 0 in x0.
//   - longjmp's body becomes LGP_LONGJMP(): it restores that state, sets x0 to
//     the value (0 becomes 1) and jumps to the host jmp_buf.
// The guest jmp_buf in guest memory is not written; code that copies or
// inspects a jmp_buf is not supported. setjmp reached through a function
// pointer or a tail call cannot work this way and halts (kHaltBadJump).
//
// Lifted code has no objects with destructors, so the host longjmp skips
// nothing. Like the guest, it is undefined to longjmp to a setjmp whose frame
// has returned. A longjmp across run_lifted() (an HLE callback into guest
// code) would skip that run_lifted's fault-handler restore and is not
// supported either.
#pragma once

#include <csetjmp>
#include <cstdint>
#include <memory>
#include <unordered_map>

#include "lgp/lifted/cpu_state.hpp"
#include "lgp/runtime/log.hpp"
#include "lgp/runtime/system.hpp"

namespace lgp::rt {

struct HostJmp {
    std::jmp_buf env;
    uint64_t x[12];  // x19..x30
    uint64_t sp;
    uint64_t d[8];  // d8..d15
};

// Host jump buffers by guest jmp_buf address; a jump never crosses threads.
inline std::unordered_map<uint64_t, std::unique_ptr<HostJmp>>& host_jmp_map() {
    thread_local std::unordered_map<uint64_t, std::unique_ptr<HostJmp>> map;
    return map;
}

// Saves the callee-saved state under the guest jmp_buf in x0.
inline HostJmp& host_jmp_save(CpuState* ctx) {
    std::unique_ptr<HostJmp>& slot = host_jmp_map()[ctx->x[0]];
    if (!slot)
        slot = std::make_unique<HostJmp>();
    HostJmp& h = *slot;
    for (int i = 0; i < 12; ++i)
        h.x[i] = ctx->x[19 + i];
    h.sp = ctx->sp;
    for (int i = 0; i < 8; ++i)
        h.d[i] = ctx->vreg[8 + i].lo;
    return h;
}

// longjmp(x0 = guest jmp_buf, w1 = value). Returns only if the buffer is
// unknown, with kHaltBadJump set (halt_info = the guest jmp_buf address).
inline void host_longjmp(CpuState* ctx) {
    auto& map = host_jmp_map();
    auto it = map.find(ctx->x[0]);
    if (it == map.end()) {
        log(LogLevel::Error, "longjmp to a jmp_buf without setjmp: 0x%llx",
            static_cast<unsigned long long>(ctx->x[0]));
        if (ctx->halt == kHaltNone) {
            ctx->halt = kHaltBadJump;
            ctx->halt_info = ctx->x[0];
        }
        return;
    }
    HostJmp& h = *it->second;
    for (int i = 0; i < 12; ++i)
        ctx->x[19 + i] = h.x[i];
    ctx->sp = h.sp;
    for (int i = 0; i < 8; ++i)
        ctx->vreg[8 + i] = {h.d[i], 0};  // writing Dn clears the upper half, as the guest's ldp d8, d9 does
    const uint32_t value = static_cast<uint32_t>(ctx->x[1]);
    ctx->x[0] = value != 0 ? value : 1;
    std::longjmp(h.env, 1);
}

// setjmp entered through a function pointer or a tail call.
inline void host_setjmp_indirect(CpuState* ctx) {
    log(LogLevel::Error, "setjmp at 0x%llx reached without a direct call; not supported (ADR-0004)",
        static_cast<unsigned long long>(ctx->pc));
    if (ctx->halt == kHaltNone) {
        ctx->halt = kHaltBadJump;
        ctx->halt_info = ctx->pc;
    }
}

}  // namespace lgp::rt
