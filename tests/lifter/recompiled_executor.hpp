// IExecutor for lifted code (ADR-0006): runs a generated case's lifted entry
// function against the TestCase's memory and initial state.
#pragma once

#include "case_defs.hpp"
#include "oracle.h"

namespace lgp_test {

class RecompiledExecutor final : public lgp::difftest::IExecutor {
public:
    const char* name() const override { return "recompiled"; }
    // The lifted code to run for the next TestCase.
    void set_case(const CaseDef* c) { case_ = c; }
    // Supports stop on the sentinel BRK, svcExitProcess and faults. stop_pc
    // and max_instructions are not supported (lifted code has no per-instruction
    // hook); cases must end in the sentinel.
    lgp::difftest::RunResult run(const lgp::difftest::TestCase& tc) override;

private:
    const CaseDef* case_ = nullptr;
};

}  // namespace lgp_test
