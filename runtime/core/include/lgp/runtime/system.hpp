// Process-wide runtime state that lifted code reaches without a context
// pointer: the active kernel, the guest clock and read-only system registers.
#pragma once

#include <cstdint>

namespace lgp {
struct CpuState;                      // recomp/lifter: lgp/lifted/cpu_state.hpp
using LiftedFn = void (*)(CpuState*);  // same alias as cpu_state.hpp

// One recompiled function: guest entry address and its host function. The
// recompiler emits an array of these per module (dispatch.hpp).
struct LiftedEntry {
    uint64_t va;
    LiftedFn fn;
};

// One module of the linked image the code was lifted for (the recompiler
// emits an array of these next to the entries). Lifted code bakes in the
// addresses, so the runtime must load exactly this layout.
struct LiftedModule {
    const char* name;
    uint64_t base;
    uint64_t size;           // image size including bss
    const char* module_id;   // hex build id (NSO/NRO header), may be empty
};
}  // namespace lgp

namespace lgp::rt {

class Kernel;

// The kernel that LGP_SVC talks to. Exactly one guest process per runtime.
void set_active_kernel(Kernel* kernel);
Kernel& active_kernel();

// Horizon's system counter runs at 19.2 MHz (CNTFRQ_EL0).
inline constexpr uint64_t kCounterFrequency = 19'200'000;
// Ticks since the runtime started: the active kernel's guest clock
// (clock.hpp), the host's steady clock without a kernel. Reading it costs no
// guest time (host-side readers: HLE services, input scripts).
uint64_t system_tick();
// A guest thread reads the counter (svcGetSystemTick, CNTPCT_EL0): in
// virtual clock mode this charges kVirtualCounterReadCostNs first.
uint64_t guest_counter_read();

// MRS of a read-only system register the lifter does not inline (CNTFRQ_EL0,
// CNTPCT_EL0, CNTVCT_EL0, CTR_EL0, DCZID_EL0). Packing: a64::sysreg() of the
// lifter, (op0 << 14) | (op1 << 11) | (CRn << 7) | (CRm << 3) | op2.
// An unknown register is a fatal runtime error.
uint64_t read_sysreg(uint32_t encoding);

// Indirect branch targets (ADR-0004). The direct-indexed dispatch table is in
// dispatch.hpp; a resolver installed here is asked only for addresses the
// table does not know (tests, bring-up, hand-written HLE functions).
using IndirectResolver = LiftedFn (*)(uint64_t va);
void set_indirect_resolver(IndirectResolver resolver);
IndirectResolver indirect_resolver();

// Called (on the thread that asked) each time the active process is asked to
// end (after g_backedge_stop is set), to pause (after Scheduler::paused()
// turns true) or to resume: native execution (runtime/native) interrupts its
// threads that run without SVCs. nullptr: nothing.
enum class GuestInterrupt : uint8_t { Exit, Pause, Resume };
using InterruptListener = void (*)(GuestInterrupt what);
void set_interrupt_listener(InterruptListener listener);

// Scheduling point for YIELD/WFE/WFI (Scheduler::yield_hint).
void guest_yield();

}  // namespace lgp::rt
