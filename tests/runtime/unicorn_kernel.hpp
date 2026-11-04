// Runs own A64 code under Unicorn with lgp::rt::Kernel as the kernel: guest
// memory is the runtime's GuestMemory (mapped into Unicorn by pointer), every
// SVC goes to Kernel::svc. Test-only (Unicorn is GPL-2.0, ADR-0006).
#pragma once

#include <array>
#include <cstdint>
#include <string>

#include "lgp/runtime/kernel.hpp"

namespace lgp_test {

enum class KernelRunOutcome { ExitProcess, Break, Unimplemented, Fault, InstructionLimit };
const char* to_string(KernelRunOutcome o);

struct KernelRun {
    KernelRunOutcome outcome = KernelRunOutcome::Fault;
    std::string detail;
    std::array<uint64_t, 31> x{};
    uint64_t sp = 0;
    uint64_t pc = 0;
    unsigned svc_count = 0;
};

KernelRun run_under_unicorn(lgp::rt::Kernel& kernel, uint64_t entry, uint64_t sp,
                            const std::array<uint64_t, 31>& initial_x = {}, uint64_t max_instructions = 100000);

}  // namespace lgp_test
