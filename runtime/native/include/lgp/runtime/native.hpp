// Native execution of the original A64 code (docs/analysis/56-originalcode-nativ-machbarkeit.md,
// stage C): the rewritten modules (lgp_recomp native-rewrite) run directly on
// an AArch64 host, with guest address == host address
// (GuestMemory::Options::at_guest_address).
//
// The rewriter turns every SVC and every access to TPIDR_EL0, TPIDRRO_EL0 and
// the counter registers into a call of the gate in the patch area
// (lgp/recomp/native_gate.hpp). The gate calls the host function this
// executor installs; it switches to the thread's host stack, restores the
// host's x18 and runs the instruction on the gate frame: SVCs go to the
// active Kernel exactly as from lifted code (guest.hpp), the thread registers
// live in the thread's CpuState (tpidr_el0, tpidrro_el0), the counters come
// from read_sysreg().
//
// run() has the signature of a lifted function, so the scheduler starts
// native threads like lifted ones (install it through the indirect resolver
// for the addresses of native code). It loads the CpuState into the
// registers and branches to ctx->pc with x30 pointing at a return stub; it
// comes back when that code returns (state after the return in ctx, pc and
// x30 = the x30 of the call) or when the thread leaves guest code (an SVC
// that ends the thread or the process, an unknown gate instruction: halt set,
// ctx holds the state at the site). Calls nest: an HLE service may call guest
// code through run() while a guest thread is inside an SVC.
//
// Faults in native code end the guest thread with the halt reason lifted code
// would give; threads that run without SVCs leave guest code on exit and park
// on a pause (kick signal). Host functions at guest addresses are reached
// through host-call stubs (native_gate::kHostCallWord). Not covered yet: x18
// for hosts that use it themselves (Android's shadow call stack).
#pragma once

#include <cstdint>

#include "lgp/lifted/cpu_state.hpp"

namespace lgp::rt::native {

// True where this build can run guest code natively (AArch64 with GCC/Clang).
bool available();

// Writes the address of the gate's host function to the patch area's
// host-function slot (native_gate::kHostFnSlot), given as guest VA == host
// address. The page must be writable.
void install_gate(uint64_t host_fn_slot);

// Runs ctx natively from ctx->pc on the calling thread (see above). On a
// build without native execution it halts with kHaltUndefined.
void run(CpuState* ctx);

// Process-wide counts since the start (--time-stats line "native"): entries
// into guest code through run(), gate passes by kind, kick signals taken in
// guest code (time slices, pause, exit). Zero without native execution.
struct Stats {
    uint64_t entries = 0;
    uint64_t svcs = 0;
    uint64_t host_calls = 0;
    uint64_t sysregs = 0;
    uint64_t kicks = 0;
};
Stats stats();

}  // namespace lgp::rt::native
