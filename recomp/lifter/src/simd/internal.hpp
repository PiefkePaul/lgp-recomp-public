// Shared definitions of the SIMD&FP decoder/lifter modules (P2.4).
//
// The dispatcher (simd_lift.cpp) classifies a word into one encoding class of
// the Arm ARM (DDI 0487, C4.1 "Data Processing -- Scalar Floating-Point and
// Advanced SIMD" and "Advanced SIMD load/store structures") and hands it to
// the module that owns the class. Each module decodes the remaining fields,
// rejects unallocated combinations and, with emit = true, returns C++ code.
#pragma once

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <string>

#include "lgp/recomp/a64_simd.hpp"

namespace lgp::recomp::a64::simd::detail {

enum class Class : uint8_t {
    // Cryptographic extension (crypto.cpp)
    CryptoAes,
    CryptoSha3,  // three-register SHA (SHA1C/P/M/SU0, SHA256H/H2/SU1)
    CryptoSha2,  // two-register SHA (SHA1H, SHA1SU1, SHA256SU0)

    // Advanced SIMD, scalar forms
    ScalarCopy,       // DUP (element) scalar
    ScalarThreeSame,
    ScalarThreeDiff,  // SQDMLAL/SQDMLSL/SQDMULL
    ScalarTwoMisc,
    ScalarPairwise,
    ScalarShiftImm,
    ScalarIndexed,

    // Advanced SIMD, vector forms
    Table,     // TBL/TBX
    Permute,   // UZP/TRN/ZIP
    Extract,   // EXT
    Copy,      // DUP/INS/UMOV/SMOV
    ThreeSame,
    ThreeDiff,
    TwoMisc,
    AcrossLanes,
    ModImm,    // MOVI/MVNI/ORR/BIC/FMOV (vector, immediate)
    ShiftImm,
    Indexed,

    // Scalar floating point
    FpFixedConv,  // SCVTF/UCVTF/FCVTZS/FCVTZU with fbits
    FpIntConv,    // FCVT[NPMZA][SU], [SU]CVTF, FMOV general
    FpDp1,
    FpCompare,
    FpImm,
    FpCondCompare,
    FpDp2,
    FpCondSelect,
    FpDp3,

    // Advanced SIMD load/store structures (ldst_struct.cpp)
    LdStMultiple,  // LD1-LD4/ST1-ST4 (multiple structures), with and without post-index
    LdStSingle,    // LD1-LD4/ST1-ST4 (single structure) and LD1R-LD4R

    None,
};

constexpr uint32_t bits(uint32_t v, int hi, int lo) { return (v >> lo) & ((1u << (hi - lo + 1)) - 1u); }
constexpr uint32_t bit(uint32_t v, int b) { return (v >> b) & 1u; }

inline Lifted invalid(Invalid why = Invalid::Unallocated) {
    Lifted l;
    l.invalid = why;
    return l;
}
inline Lifted unpredictable() { return invalid(Invalid::Unpredictable); }
inline Lifted not_implemented(const char* name = "invalid") {
    Lifted l;
    l.invalid = Invalid::NotImplemented;
    l.name = name;
    return l;
}
// A valid instruction; `code` is only evaluated by callers when emitting.
inline Lifted ok(const char* name, std::string code = {}) {
    Lifted l;
    l.invalid = Invalid::None;
    l.name = name;
    l.code = std::move(code);
    return l;
}

// ---- code building helpers ------------------------------------------------
inline std::string num(uint64_t v) { return std::to_string(v); }
inline std::string hex64(uint64_t v) {
    char buf[40];
    std::snprintf(buf, sizeof(buf), "UINT64_C(0x%" PRIx64 ")", v);
    return buf;
}
inline std::string hex32(uint32_t v) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "0x%" PRIx32 "u", v);
    return buf;
}
// V register (whole 128-bit lgp::V128): a local of the lifted function or a
// reference into CpuState::vreg, like the X registers (Lifter::lift).
inline std::string vreg(unsigned r) { return "lgp_v" + num(r); }
// General-purpose register read (31 = XZR unless sp is set).
inline std::string xreg(unsigned r, bool sp = false) {
    if (r == 31) return sp ? std::string("lgp_sp") : std::string("UINT64_C(0)");
    return "lgp_x" + num(r);
}
// General-purpose register write of a 64-bit expression (31 = XZR discards).
inline std::string set_x(unsigned r, const std::string& expr) {
    if (r == 31) return "(void)(" + expr + ");";
    return "lgp_x" + num(r) + " = " + expr + ";";
}

// ConditionHolds() over the NZCV flags (lgp_f, see Lifter::lift).
inline std::string cond_expr(uint32_t cond) {
    std::string base;
    switch ((cond >> 1) & 7) {
    case 0: base = "lgp_f.z"; break;
    case 1: base = "lgp_f.c"; break;
    case 2: base = "lgp_f.n"; break;
    case 3: base = "lgp_f.v"; break;
    case 4: base = "(lgp_f.c && !lgp_f.z)"; break;
    case 5: base = "(lgp_f.n == lgp_f.v)"; break;
    case 6: base = "(lgp_f.n == lgp_f.v && !lgp_f.z)"; break;
    default: return "true";  // AL and NV both mean "always" in A64
    }
    return (cond & 1) ? "!" + base : base;
}

// Scalar SIMD&FP register reads by element size in bytes (1, 2, 4, 8) and
// writes that zero the rest of the register.
inline std::string vscalar(unsigned r, unsigned bytes) {
    switch (bytes) {
    case 1: return "static_cast<uint8_t>(" + vreg(r) + ".lo)";
    case 2: return "static_cast<uint16_t>(" + vreg(r) + ".lo)";
    case 4: return "static_cast<uint32_t>(" + vreg(r) + ".lo)";
    default: return vreg(r) + ".lo";
    }
}
inline std::string set_vscalar(unsigned r, const std::string& expr) {
    return vreg(r) + " = lgp::V128{static_cast<uint64_t>(" + expr + "), 0};";
}

// Module entry points: one per family, `cls` is one of the classes the module owns.
Lifted lift_crypto(Class cls, uint32_t raw, bool emit);
Lifted lift_fp_scalar(Class cls, uint32_t raw, bool emit);
Lifted lift_vec_int(Class cls, uint32_t raw, bool emit);
Lifted lift_vec_fp(Class cls, uint32_t raw, bool emit);
Lifted lift_vec_perm(Class cls, uint32_t raw, bool emit);
Lifted lift_ldst_struct(Class cls, uint32_t raw, bool emit);

// Classification of a word (exposed for tests and statistics).
Class classify(uint32_t raw);
// Whether an instruction of an Advanced SIMD class with an FP/integer mix
// (three same, two-register misc, across lanes, pairwise, shift by immediate,
// by element) belongs to the floating-point module.
bool is_fp_op(Class cls, uint32_t raw);

}  // namespace lgp::recomp::a64::simd::detail
