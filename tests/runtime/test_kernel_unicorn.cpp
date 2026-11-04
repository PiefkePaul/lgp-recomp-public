// P3.1 kernel test programs under Unicorn: every SVC goes to lgp::rt::Kernel,
// guest memory is the runtime's own (mapped into Unicorn by pointer), so the
// permissions the kernel sets are enforced on every guest access.
#include "kernel_programs.hpp"
#include "unicorn_kernel.hpp"

using namespace lgp_test;

namespace {

GuestStop to_stop(KernelRunOutcome o) {
    switch (o) {
    case KernelRunOutcome::ExitProcess: return GuestStop::ExitProcess;
    case KernelRunOutcome::Break: return GuestStop::Break;
    case KernelRunOutcome::Unimplemented: return GuestStop::Unimplemented;
    case KernelRunOutcome::Fault: return GuestStop::MemFault;
    case KernelRunOutcome::InstructionLimit: return GuestStop::Other;
    }
    return GuestStop::Other;
}

}  // namespace

int main() {
    for (const ProgramCode& p : program_codes()) {
        std::printf("[%s]\n", p.name);
        lgp::rt::Kernel k(program_process());
        const LoadedProgram lp = load_program(k, p);
        const KernelRun run = run_under_unicorn(k, lp.entry, lp.stack_top, lp.x);
        GuestResult r{to_stop(run.outcome), run.x, run.pc, run.detail};
        check_program(p, k, r, /*faults_enforced=*/true);
    }
    return finish();
}
