// Host signals for guest code that runs natively on an AArch64 host
// (lgp/runtime/native.hpp, docs/analysis/56 stage C).
//
// A fault in native guest code (bad access, undefined instruction, BRK)
// arrives as a host signal on the faulting thread. The runtime's filter
// decides inside the handler whether the signal belongs to guest code; if so
// it rewrites the interrupted register state, and the thread continues there
// when the handler returns (the runtime sends it to a host function that ends
// the guest thread). Every other signal goes to the handler installed before
// (by default the process ends as before).
//
// The kick signal interrupts a thread that runs guest code without ever
// reaching an SVC (spin loops), so it can leave guest code when the process
// ends.
//
// The handlers run on the thread's alternate signal stack
// (host_thread.hpp ensure_signal_stack()), never on the guest stack.
#pragma once

#include <cstdint>

namespace lgp::platform {

// AArch64 user registers of the interrupted thread.
struct SignalContext {
    uint64_t x[31];
    uint64_t sp;
    uint64_t pc;
    uint64_t pstate;  // NZCV in bits 31..28
};

enum class SignalKind : uint8_t { Segv, Bus, Ill, Trap, Fpe, Kick };

// Runs inside the signal handler (async-signal context: no locks, no
// allocation). May change ctx; true: handled, the thread resumes with ctx.
// false: the signal goes on to the previous handler. A Kick is dropped when
// not handled.
using SignalFilter = bool (*)(SignalKind kind, uint64_t fault_addr, SignalContext& ctx);

// Installs the handlers once per process (later calls only replace the
// filter). false where the host has no such signals (Windows, the browser)
// or is not AArch64.
bool install_signal_filter(SignalFilter filter);

// Identifies a host thread for kick_thread(). 0 where unsupported.
uint64_t current_thread_token();
// Sends the kick signal to that thread (no-op where unsupported).
void kick_thread(uint64_t token);

}  // namespace lgp::platform
