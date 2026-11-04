// Contract between the native patch area (native_image.hpp) and the runtime
// that runs original A64 code natively (docs/analysis/56-originalcode-nativ-machbarkeit.md,
// stage C). Header only, no dependencies, so the runtime can include it as is.
//
// A gate site (SVC, MRS/MSR of TPIDR_EL0/TPIDRRO_EL0, MRS of CNTPCT/CNTVCT/CNTFRQ_EL0)
// is a `B` into a stub. The stub reserves a GateFrame below the guest SP,
// saves x16, x17 and x30 there, loads the site address and the instruction
// word and calls the shared gate entry. The gate entry stores the rest of the
// guest state into the frame and calls the host function whose address the
// runtime wrote to the data page of the patch area (kHostFnSlot):
//
//     void host(GateFrame* frame);      // AAPCS64, x0 = frame
//
// The host function performs the instruction (`word`) on the frame: an SVC
// reads its arguments from x[] and writes its results there, an MRS writes
// x[rt], an MSR reads x[rt] (rt 31 is XZR). Everything it leaves in x[],
// nzcv, fpsr and q[] becomes the guest state after the site; pc, sp and word
// are read only. Execution always continues after the site.
//
// Registers the host function must keep: SP (it returns with SP at the frame,
// via x30). All other registers may be clobbered, the gate restores the guest
// state from the frame (x0..x30, NZCV, FPSR, q0..q31). FPCR must be left as
// it was. The host function runs on the guest stack, directly below the frame;
// switching to a host stack and setting up x18 for the host platform (Apple:
// reserved, Android: shadow call stack) is the runtime's job (stage C).
#pragma once

#include <cstddef>
#include <cstdint>

namespace lgp::recomp::native_gate {

inline constexpr uint32_t kX = 0;          // x0..x30, 8 bytes each
inline constexpr uint32_t kSp = 248;       // guest SP at the site (read only)
inline constexpr uint32_t kPc = 256;       // VA of the site (read only)
inline constexpr uint32_t kWord = 264;     // instruction word of the site, 32 bits (read only)
inline constexpr uint32_t kNzcv = 272;     // NZCV as the NZCV system register (bits 31..28)
inline constexpr uint32_t kFpsr = 280;
inline constexpr uint32_t kQ = 288;        // q0..q31, 16 bytes each
inline constexpr uint32_t kRet = 800;      // return address of the gate entry (private to the gate)
inline constexpr uint32_t kSize = 816;     // frame size, a multiple of 16

// Data page of the patch area (the page after the code).
inline constexpr uint32_t kHostFnSlot = 0;  // u64: address of the host function

// Host call (native_image.hpp: build_native_host_calls): the runtime's host
// functions at guest addresses (boot thunks, NVN procs, import shims, replaced
// SDK functions) are reached by BL/BLR like guest functions. A `B` at such an
// address leads to a stub that calls the gate with this word and pc = the
// address; the host function runs the call on the frame, and the stub returns
// to x30 of the frame instead of continuing after the site. HLT never occurs
// in user code (it halts the CPU in debug state), so it names no real site.
inline constexpr uint32_t kHostCallWord = 0xD44988E0;  // hlt #0x4c47

struct alignas(16) GateFrame {
    uint64_t x[31];
    uint64_t sp;
    uint64_t pc;
    uint32_t word;
    uint32_t reserved0;
    uint64_t nzcv;
    uint64_t fpsr;
    uint64_t q[32][2];  // lo, hi
    uint64_t ret;
    uint64_t reserved1;
};
static_assert(offsetof(GateFrame, sp) == kSp && offsetof(GateFrame, pc) == kPc && offsetof(GateFrame, word) == kWord);
static_assert(offsetof(GateFrame, nzcv) == kNzcv && offsetof(GateFrame, fpsr) == kFpsr && offsetof(GateFrame, q) == kQ);
static_assert(offsetof(GateFrame, ret) == kRet && sizeof(GateFrame) == kSize && kSize % 16 == 0);

}  // namespace lgp::recomp::native_gate
