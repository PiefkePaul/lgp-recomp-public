// Unicorn as the executor of guest threads on the runtime's scheduler (P2.8
// oracle, test-only: Unicorn is GPL-2.0, ADR-0006).
//
// run_under_unicorn has the signature of a lifted function. Installed as the
// indirect resolver for every address, the scheduler hands it each guest
// thread's CpuState (entry pc, sp, x0/x1, TLS pointer); it emulates the thread
// on its host thread, passes every SVC to the active Kernel exactly as lifted
// code does (guest.hpp), and leaves with the same halt reasons. So the
// oracle and the recompiled code run on the same kernel, the same memory
// layout and the same scheduling points.
#pragma once

#include "lgp/lifted/cpu_state.hpp"

namespace lgp_fixture {

void run_under_unicorn(lgp::CpuState* ctx);

}  // namespace lgp_fixture
