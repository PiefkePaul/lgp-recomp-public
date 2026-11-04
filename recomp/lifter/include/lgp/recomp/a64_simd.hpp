// SIMD&FP part of the A64 decoder and lifter (roadmap P2.4).
//
// Covers the ARMv8.0-A "Data Processing -- Scalar Floating-Point and Advanced
// SIMD" encoding space (op0 = x111) plus the Advanced SIMD load/store
// structure instructions (LD1..LD4, ST1..ST4, LDnR), as implemented by the
// Switch's Cortex-A57: FP16 arithmetic, RDM, dot product, SHA512/SHA3/SM3/SM4
// and FCMA/JSCVT are later extensions and decode as unallocated.
//
// The integer decoder (a64.hpp) hands these words to lift() and reports them
// as Op::Simd with Flow::Normal: no SIMD&FP instruction changes control flow.
// Decoding and lifting share one pass per instruction class, so the validity
// the CFG sees and the code the lifter emits can never disagree.
//
// Emitted code is a single C++ statement block that uses `ctx` (lgp::CpuState*)
// and the helpers in lgp/lifted/fp.hpp and lgp/lifted/simd.hpp.
#pragma once

#include <cstdint>
#include <string>

#include "lgp/recomp/a64.hpp"

namespace lgp::recomp::a64::simd {

struct Lifted {
    Invalid invalid = Invalid::Unallocated;  // None: valid instruction
    const char* name = "invalid";            // lower-case mnemonic of the base form, for statistics
    std::string code;                        // C++ statements; only filled with emit = true
};

// SIMD&FP data processing (op0 = x111).
bool is_data_processing(uint32_t raw);
// Advanced SIMD load/store multiple/single structures (op0 = 0x00 1100 ...).
bool is_ldst_structure(uint32_t raw);

// Decodes (and with emit = true lifts) one instruction word of either class.
// Words of other classes come back as Invalid::Unallocated.
Lifted lift(uint32_t raw, uint64_t pc, bool emit);

// Name of the encoding class of a word (e.g. "three_same", "fp_dp2"), "none"
// outside the SIMD&FP spaces. For statistics and test reports.
const char* class_name(uint32_t raw);

}  // namespace lgp::recomp::a64::simd
