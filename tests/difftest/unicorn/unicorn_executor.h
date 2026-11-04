// Unicorn-based executor: the CI oracle of ADR-0006. Runs everywhere, but it
// is an emulator itself; disagreements with the Hypervisor oracle go to an
// exception list instead of being "fixed" in the lifter.
#pragma once

#include "../oracle.h"

namespace lgp::difftest {

class UnicornExecutor final : public IExecutor {
public:
    const char* name() const override { return "unicorn"; }
    RunResult run(const TestCase& tc) override;
};

}  // namespace lgp::difftest
