// Guest CPU state shared by lifted code and the runtime.
//
// The lifter decides these field names; the runtime (runtime/core) and the
// test harness include this header. Plain aggregate, no virtuals, so it can be
// laid out identically on desktop and WebAssembly builds.
#pragma once

#include <cstdint>

namespace lgp {

struct alignas(16) V128 {
    uint64_t lo;
    uint64_t hi;
};

// Reasons for leaving lifted code early. Lifted code checks CpuState::halt
// after every call and returns when it is non-zero, so a runtime hook can stop
// the guest anywhere without longjmp.
enum HaltReason : uint32_t {
    kHaltNone = 0,
    kHaltBrk = 1,            // BRK/HLT executed (halt_info = immediate)
    kHaltExit = 2,           // runtime requested an exit (e.g. svcExitProcess)
    kHaltUndefined = 3,      // instruction the lifter does not support (halt_info = raw word)
    kHaltBadJump = 4,        // indirect branch to an address without lifted code (halt_info = target)
    kHaltMemFault = 5,       // memory access outside the guest address space (halt_info = address)
    kHaltLimit = 6,          // execution budget exhausted (LGP_BACKEDGE in a bounded run)
};

struct CpuState {
    uint64_t x[31];  // x0..x30
    uint64_t sp;
    // Only kept up to date where the runtime can observe it: at SVCs, halts and
    // indirect branches. Lifted code uses constant PCs everywhere else.
    uint64_t pc;

    // NZCV flags, one byte each (0 or 1) so lifted code can test them cheaply.
    uint8_t n, z, c, v;

    uint32_t fpcr;
    uint32_t fpsr;
    uint64_t tpidr_el0;
    uint64_t tpidrro_el0;
    V128 vreg[32];

    // Local exclusive monitor for LDXR/STXR (lowered to compare-and-swap).
    uint64_t excl_addr;
    uint64_t excl_lo;   // value seen by the load-exclusive (first register)
    uint64_t excl_hi;   // second register of LDXP
    uint8_t excl_size;  // total bytes covered by the exclusive access
    uint8_t excl_valid;

    uint32_t halt;  // HaltReason
    uint64_t halt_info;

    void* user;  // runtime-private
};

using LiftedFn = void (*)(CpuState*);

inline uint32_t pack_nzcv(const CpuState& s) {
    return (uint32_t{s.n} << 31) | (uint32_t{s.z} << 30) | (uint32_t{s.c} << 29) | (uint32_t{s.v} << 28);
}

inline void unpack_nzcv(CpuState& s, uint64_t nzcv) {
    s.n = static_cast<uint8_t>((nzcv >> 31) & 1);
    s.z = static_cast<uint8_t>((nzcv >> 30) & 1);
    s.c = static_cast<uint8_t>((nzcv >> 29) & 1);
    s.v = static_cast<uint8_t>((nzcv >> 28) & 1);
}

}  // namespace lgp
