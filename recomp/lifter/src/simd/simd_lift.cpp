// Top-level classification of SIMD&FP instruction words (Arm ARM DDI 0487,
// C4.1 encoding tables) and dispatch to the family modules.
#include "internal.hpp"

namespace lgp::recomp::a64::simd {
namespace detail {

Class classify(uint32_t r) {
    // Advanced SIMD load/store structures.
    if ((r & 0xBFBF0000u) == 0x0C000000u || (r & 0xBFA00000u) == 0x0C800000u) return Class::LdStMultiple;
    if ((r & 0xBF9F0000u) == 0x0D000000u || (r & 0xBF800000u) == 0x0D800000u) return Class::LdStSingle;

    if ((bits(r, 28, 25) & 0b0111) != 0b0111) return Class::None;

    // Cryptographic extension.
    if ((r & 0xFF3E0C00u) == 0x4E280800u) return Class::CryptoAes;
    if ((r & 0xFF208C00u) == 0x5E000000u) return Class::CryptoSha3;
    if ((r & 0xFF3E0C00u) == 0x5E280800u) return Class::CryptoSha2;

    // Scalar floating point (bit 30 = 0, bit 28 = 1).
    if ((r & 0x5F000000u) == 0x1F000000u) return Class::FpDp3;
    if ((r & 0x5F200000u) == 0x1E000000u) return Class::FpFixedConv;
    if ((r & 0x5F200000u) == 0x1E200000u) {
        if ((r & 0xFC00u) == 0x0000u) return Class::FpIntConv;
        if ((r & 0x7C00u) == 0x4000u) return Class::FpDp1;
        if ((r & 0x3C00u) == 0x2000u) return Class::FpCompare;
        if ((r & 0x1C00u) == 0x1000u) return Class::FpImm;
        switch (bits(r, 11, 10)) {
        case 0b01: return Class::FpCondCompare;
        case 0b10: return Class::FpDp2;
        case 0b11: return Class::FpCondSelect;
        default: return Class::None;
        }
    }

    // Advanced SIMD scalar (bits 31:30 = 01, bit 28 = 1).
    if ((r & 0xDF000000u) == 0x5E000000u) {
        if ((r & 0xDFE08400u) == 0x5E000400u) return Class::ScalarCopy;
        if ((r & 0xDF3E0C00u) == 0x5E200800u) return Class::ScalarTwoMisc;
        if ((r & 0xDF3E0C00u) == 0x5E300800u) return Class::ScalarPairwise;
        if ((r & 0xDF200C00u) == 0x5E200000u) return Class::ScalarThreeDiff;
        if ((r & 0xDF200400u) == 0x5E200400u) return Class::ScalarThreeSame;
        return Class::None;
    }
    if ((r & 0xDF000000u) == 0x5F000000u) {
        if (bit(r, 10)) return (bit(r, 23) == 0 && bits(r, 22, 19) != 0) ? Class::ScalarShiftImm : Class::None;
        return Class::ScalarIndexed;
    }

    // Advanced SIMD vector (bit 31 = 0, bit 28 = 0).
    if ((r & 0x9F000000u) == 0x0E000000u) {
        if (bit(r, 21)) {
            if ((r & 0x9F3E0C00u) == 0x0E200800u) return Class::TwoMisc;
            if ((r & 0x9F3E0C00u) == 0x0E300800u) return Class::AcrossLanes;
            if (bit(r, 10)) return Class::ThreeSame;
            if (bits(r, 11, 10) == 0b00) return Class::ThreeDiff;
            return Class::None;
        }
        if ((r & 0x9FE08400u) == 0x0E000400u) return Class::Copy;
        if (bit(r, 29) == 0 && bits(r, 11, 10) == 0b00 && bit(r, 15) == 0) return Class::Table;
        if (bit(r, 29) == 0 && bits(r, 11, 10) == 0b10 && bit(r, 15) == 0) return Class::Permute;
        if (bit(r, 29) == 1 && bits(r, 23, 22) == 0b00 && bit(r, 15) == 0 && bit(r, 10) == 0) return Class::Extract;
        return Class::None;
    }
    if ((r & 0x9F000000u) == 0x0F000000u) {
        if (bit(r, 10)) {
            if (bit(r, 23)) return Class::None;
            return bits(r, 22, 19) == 0 ? Class::ModImm : Class::ShiftImm;
        }
        return Class::Indexed;
    }
    return Class::None;
}

bool is_fp_op(Class cls, uint32_t r) {
    switch (cls) {
    case Class::ThreeSame:
    case Class::ScalarThreeSame: return bits(r, 15, 11) >= 0b11000;
    case Class::TwoMisc:
    case Class::ScalarTwoMisc: {
        const uint32_t op = bits(r, 16, 12);
        return (op >= 0x0C && op <= 0x0F) || op >= 0x16;
    }
    case Class::AcrossLanes: {
        const uint32_t op = bits(r, 16, 12);
        return op == 0x0C || op == 0x0F;
    }
    case Class::ScalarPairwise: return bits(r, 16, 12) != 0x1B;
    case Class::ShiftImm:
    case Class::ScalarShiftImm: {
        const uint32_t op = bits(r, 15, 11);
        return op == 0x1C || op == 0x1F;
    }
    case Class::Indexed:
    case Class::ScalarIndexed: {
        const uint32_t op = bits(r, 15, 12);
        return op == 0x1 || op == 0x5 || op == 0x9;
    }
    default: return false;
    }
}

}  // namespace detail

bool is_data_processing(uint32_t raw) { return (detail::bits(raw, 28, 25) & 0b0111) == 0b0111; }

bool is_ldst_structure(uint32_t raw) {
    const detail::Class c = detail::classify(raw);
    return c == detail::Class::LdStMultiple || c == detail::Class::LdStSingle;
}

const char* class_name(uint32_t raw) {
    using detail::Class;
    switch (detail::classify(raw)) {
    case Class::CryptoAes: return "crypto_aes";
    case Class::CryptoSha3: return "crypto_sha3";
    case Class::CryptoSha2: return "crypto_sha2";
    case Class::ScalarCopy: return "scalar_copy";
    case Class::ScalarThreeSame: return "scalar_three_same";
    case Class::ScalarThreeDiff: return "scalar_three_diff";
    case Class::ScalarTwoMisc: return "scalar_two_misc";
    case Class::ScalarPairwise: return "scalar_pairwise";
    case Class::ScalarShiftImm: return "scalar_shift_imm";
    case Class::ScalarIndexed: return "scalar_indexed";
    case Class::Table: return "table";
    case Class::Permute: return "permute";
    case Class::Extract: return "extract";
    case Class::Copy: return "copy";
    case Class::ThreeSame: return "three_same";
    case Class::ThreeDiff: return "three_diff";
    case Class::TwoMisc: return "two_misc";
    case Class::AcrossLanes: return "across_lanes";
    case Class::ModImm: return "mod_imm";
    case Class::ShiftImm: return "shift_imm";
    case Class::Indexed: return "indexed";
    case Class::FpFixedConv: return "fp_fixed_conv";
    case Class::FpIntConv: return "fp_int_conv";
    case Class::FpDp1: return "fp_dp1";
    case Class::FpCompare: return "fp_compare";
    case Class::FpImm: return "fp_imm";
    case Class::FpCondCompare: return "fp_cond_compare";
    case Class::FpDp2: return "fp_dp2";
    case Class::FpCondSelect: return "fp_cond_select";
    case Class::FpDp3: return "fp_dp3";
    case Class::LdStMultiple: return "ldst_multiple";
    case Class::LdStSingle: return "ldst_single";
    case Class::None: break;
    }
    return "none";
}

Lifted lift(uint32_t raw, uint64_t pc, bool emit) {
    using detail::Class;
    (void)pc;  // no SIMD&FP instruction is PC-relative
    const Class cls = detail::classify(raw);
    Lifted l;
    switch (cls) {
    case Class::CryptoAes:
    case Class::CryptoSha3:
    case Class::CryptoSha2: l = detail::lift_crypto(cls, raw, emit); break;

    case Class::FpFixedConv:
    case Class::FpIntConv:
    case Class::FpDp1:
    case Class::FpCompare:
    case Class::FpImm:
    case Class::FpCondCompare:
    case Class::FpDp2:
    case Class::FpCondSelect:
    case Class::FpDp3: l = detail::lift_fp_scalar(cls, raw, emit); break;

    case Class::ScalarCopy:
    case Class::Table:
    case Class::Permute:
    case Class::Extract:
    case Class::Copy:
    case Class::ModImm: l = detail::lift_vec_perm(cls, raw, emit); break;

    case Class::ScalarThreeSame:
    case Class::ScalarThreeDiff:
    case Class::ScalarTwoMisc:
    case Class::ScalarPairwise:
    case Class::ScalarShiftImm:
    case Class::ScalarIndexed:
    case Class::ThreeSame:
    case Class::ThreeDiff:
    case Class::TwoMisc:
    case Class::AcrossLanes:
    case Class::ShiftImm:
    case Class::Indexed:
        l = detail::is_fp_op(cls, raw) ? detail::lift_vec_fp(cls, raw, emit) : detail::lift_vec_int(cls, raw, emit);
        break;

    case Class::LdStMultiple:
    case Class::LdStSingle: l = detail::lift_ldst_struct(cls, raw, emit); break;

    case Class::None: return detail::invalid();
    }
    if (l.invalid != Invalid::None || !emit) {
        l.code.clear();
    } else if (l.code.empty()) {
        l.code = ";";
    }
    return l;
}

}  // namespace lgp::recomp::a64::simd
