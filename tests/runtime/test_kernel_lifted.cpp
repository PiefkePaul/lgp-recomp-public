// P2.6: the kernel test programs lifted to C++ by the P2.3 lifter and run
// against the real runtime (guest.hpp + lgp::rt::Kernel), checked with the same
// expectations as the Unicorn run. Built against both runtime variants.
#include "kernel_programs.hpp"
#include "lgp/lifted/cpu_state.hpp"
#include "lgp/runtime/guest.hpp"

extern const lgp::LiftedFn kLiftedPrograms[];

using namespace lgp_test;

int main() {
    const auto& programs = program_codes();
    for (size_t i = 0; i < programs.size(); ++i) {
        const ProgramCode& p = programs[i];
        std::printf("[%s]\n", p.name);
        lgp::rt::Kernel k(program_process());
        lgp::rt::set_active_kernel(&k);
        const LoadedProgram lp = load_program(k, p);

        lgp::CpuState cpu{};
        for (size_t r = 0; r < lp.x.size(); ++r)
            cpu.x[r] = lp.x[r];
        cpu.sp = lp.stack_top;
        cpu.pc = lp.entry;
        lgp::rt::run_lifted(&cpu, kLiftedPrograms[i]);

        GuestResult res;
        for (size_t r = 0; r < res.x.size(); ++r)
            res.x[r] = cpu.x[r];
        res.pc = cpu.pc;
        switch (cpu.halt) {
        case lgp::kHaltExit:
            res.stop = cpu.halt_info == lgp::rt::kSvcExitProcess ? GuestStop::ExitProcess : GuestStop::Break;
            break;
        case lgp::kHaltUndefined:
            // An svc the runtime lacks halts with the SVC's encoding.
            res.stop = (cpu.halt_info & 0xFFE0001Fu) == 0xD4000001u ? GuestStop::Unimplemented : GuestStop::Other;
            break;
        case lgp::kHaltMemFault: res.stop = GuestStop::MemFault; break;
        default:
            res.stop = GuestStop::Other;
            res.detail = "halt " + std::to_string(cpu.halt) + " info " + std::to_string(cpu.halt_info);
            break;
        }
        check_program(p, k, res, /*faults_enforced=*/LGP_GUEST_MEM_CHECKED != 0);
        lgp::rt::set_active_kernel(nullptr);
    }
    return finish();
}
