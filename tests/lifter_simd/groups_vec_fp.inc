// Test groups of the SIMD&FP lifter module vec_fp (recomp/lifter/src/simd/vec_fp.cpp).
// Included by casegen_simd.cpp inside namespace simdgen.
//
// Opcode bits are fixed so that only FP opcodes come out (the integer module
// shares the classes); unallocated combinations (FP16, 1D, ...) are filtered
// by the generator's valid() check.

inline const std::vector<Group> kVecFpGroups = {
    // Three same, vector: opcode 11xxx (FMAXNM, FMLA, FADD, FMULX, FCMEQ, FMAX, FRECPS, FMINNM, FMLS, FSUB,
    // FMIN, FRSQRTS, FMAXNMP, FADDP, FMUL, FCMGE, FACGE, FMAXP, FDIV, FMINNMP, FABD, FCMGT, FACGT, FMINP)
    {"vfp_three_same", 0x9F20C400u, 0x0E20C400u, Kind::Alu, 3, VInit::Fp, FpcrInit::Random},
    {"vfp_three_same_fpcr0", 0x9F20C400u, 0x0E20C400u, Kind::Alu, 2, VInit::Fp, FpcrInit::Zero},
    // FMLA/FMLS (vector)
    {"vfp_fma", 0xBF20FC00u, 0x0E20CC00u, Kind::Alu, 3, VInit::Fp, FpcrInit::Random},
    {"vfp_fma_fpcr0", 0xBF20FC00u, 0x0E20CC00u, Kind::Alu, 2, VInit::Fp, FpcrInit::Zero},
    // Pairwise (vector): FMAXNMP/FMINNMP, FADDP, FMAXP/FMINP (U = 1, opcode 11xx0)
    {"vfp_pairwise", 0xBF20C400u | 0x0800u, 0x2E20C400u, Kind::Alu, 3, VInit::Fp, FpcrInit::Random},
    // FRECPS/FRSQRTS (vector)
    {"vfp_recps", 0xBF20FC00u, 0x0E20FC00u, Kind::Alu, 2, VInit::Fp, FpcrInit::Random},
    // Three same, scalar: FMULX, FCMEQ, FRECPS, FRSQRTS, FCMGE, FACGE, FABD, FCMGT, FACGT
    {"vfp_three_same_scalar", 0xDF20C400u, 0x5E20C400u, Kind::Alu, 3, VInit::Fp, FpcrInit::Random},

    // Two-register misc, vector: opcode 11xxx (FRINT*, FCVT[NPMZA][SU], URECPE, URSQRTE, [SU]CVTF, FRECPE,
    // FRSQRTE, FSQRT)
    {"vfp_two_misc", 0x9F3F8C00u, 0x0E218800u, Kind::Alu, 3, VInit::Fp, FpcrInit::Random},
    {"vfp_two_misc_fpcr0", 0x9F3F8C00u, 0x0E218800u, Kind::Alu, 2, VInit::Fp, FpcrInit::Zero},
    // SCVTF/UCVTF (vector, integer) on integer lanes
    {"vfp_cvtf_int", 0x9FBFFC00u, 0x0E21D800u, Kind::Alu, 2, VInit::Int, FpcrInit::Random},
    // URECPE/URSQRTE on integer lanes
    {"vfp_urecpe", 0x9FFFDC00u, 0x0EA1C800u, Kind::Alu, 2, VInit::Int, FpcrInit::Zero},
    // Compare with zero, FABS, FNEG: opcode 011xx
    {"vfp_two_misc_cmp", 0x9F3FCC00u, 0x0E20C800u, Kind::Alu, 3, VInit::Fp, FpcrInit::Random},
    // FCVTN/FCVTN2, FCVTXN/FCVTXN2, FCVTL/FCVTL2: opcode 1011x
    {"vfp_fcvt_nl", 0x9F3FEC00u, 0x0E216800u, Kind::Alu, 3, VInit::Fp, FpcrInit::Random},
    // Two-register misc, scalar: FCVT[NPMZA][SU], [SU]CVTF, FRECPE, FRSQRTE, FRECPX
    {"vfp_two_misc_scalar", 0xDF3F8C00u, 0x5E218800u, Kind::Alu, 3, VInit::Fp, FpcrInit::Random},
    // Scalar compare with zero
    {"vfp_two_misc_scalar_cmp", 0xDF3FCC00u, 0x5E20C800u, Kind::Alu, 3, VInit::Fp, FpcrInit::Random},
    // FCVTXN (scalar)
    {"vfp_fcvtxn_scalar", 0xFFFFFC00u, 0x7E616800u, Kind::Alu, 2, VInit::Fp, FpcrInit::Random},

    // Across lanes: FMAXNMV/FMINNMV/FMAXV/FMINV (4S)
    {"vfp_across", 0xFF7FCC00u, 0x6E30C800u, Kind::Alu, 2, VInit::Fp, FpcrInit::Random},
    // Scalar pairwise: FMAXNMP/FMINNMP, FADDP, FMAXP/FMINP
    {"vfp_scalar_pairwise", 0xFF3FCC00u, 0x7E30C800u, Kind::Alu, 2, VInit::Fp, FpcrInit::Random},

    // Shift by immediate: SCVTF/UCVTF/FCVTZS/FCVTZU with fbits (single: immh = 01xx, double: immh = 1xxx)
    {"vfp_shift_s", 0x9FE0E400u, 0x0F20E400u, Kind::Alu, 2, VInit::Fp, FpcrInit::Random},
    {"vfp_shift_d", 0x9FC0E400u, 0x0F40E400u, Kind::Alu, 2, VInit::Fp, FpcrInit::Random},
    {"vfp_shift_int_s", 0x9FE0FC00u, 0x0F20E400u, Kind::Alu, 2, VInit::Int, FpcrInit::Random},
    {"vfp_shift_int_d", 0x9FC0FC00u, 0x0F40E400u, Kind::Alu, 2, VInit::Int, FpcrInit::Random},
    {"vfp_shift_scalar_s", 0xDFE0E400u, 0x5F20E400u, Kind::Alu, 2, VInit::Fp, FpcrInit::Random},
    {"vfp_shift_scalar_d", 0xDFC0E400u, 0x5F40E400u, Kind::Alu, 2, VInit::Fp, FpcrInit::Random},

    // By element: FMLA/FMLS (opcode 0x01) and FMUL/FMULX (opcode 1001)
    {"vfp_elem_fma", 0x9F80B400u, 0x0F801000u, Kind::Alu, 3, VInit::Fp, FpcrInit::Random},
    {"vfp_elem_fma_fpcr0", 0x9F80B400u, 0x0F801000u, Kind::Alu, 2, VInit::Fp, FpcrInit::Zero},
    {"vfp_elem_mul", 0x9F80F400u, 0x0F809000u, Kind::Alu, 3, VInit::Fp, FpcrInit::Random},
    {"vfp_elem_scalar_fma", 0xDF80B400u, 0x5F801000u, Kind::Alu, 3, VInit::Fp, FpcrInit::Random},
    {"vfp_elem_scalar_mul", 0xDF80F400u, 0x5F809000u, Kind::Alu, 3, VInit::Fp, FpcrInit::Random},
};

// Register aliasing: words of the three-same and by-element groups with Rd = Rn (= Rm for three same).
inline void vec_fp_templates(Rng& rng, unsigned per, std::vector<GenCase>& out) {
    static const char* const kAliasGroups[] = {"vfp_three_same", "vfp_fma", "vfp_pairwise", "vfp_elem_fma",
                                               "vfp_elem_mul",   "vfp_fcvt_nl", "vfp_two_misc"};
    unsigned index = 0;
    for (const char* name : kAliasGroups) {
        const Group* g = nullptr;
        for (const Group& k : kVecFpGroups)
            if (std::string(k.name) == name) g = &k;
        if (!g) continue;
        for (unsigned k = 0; k < per; ++k) {
            GenCase c = make_case("vfp_alias", index++, rng);
            for (int attempt = 0; attempt < 100000; ++attempt) {
                uint32_t w = (rng.u32() & ~g->mask) | g->value;
                const uint32_t reg = static_cast<uint32_t>(rng.below(32));
                w = set_field(w, 0, 5, reg);
                w = set_field(w, 5, 5, reg);
                const bool three_same = (g->value & 0x1F000400u) == 0x0E000400u;
                if (three_same && rng.chance(50)) w = set_field(w, 16, 5, reg);
                if (!three_same && (g->value & 0x1F000000u) == 0x0F000000u && rng.chance(50))
                    w = set_field(w, 16, 4, reg & 15);  // by element: Rm<3:0> as well
                if (!valid(w)) continue;
                c.code.push_back(w);
                break;
            }
            if (c.code.empty()) continue;
            for (unsigned r = 0; r < 32; ++r) {
                uint64_t lo, hi;
                vector_value(rng, VInit::Fp, lo, hi);
                c.overrides.push_back({static_cast<uint8_t>(lgp_test::kRegVLo + r), lo});
                c.overrides.push_back({static_cast<uint8_t>(lgp_test::kRegVHi + r), hi});
            }
            c.overrides.push_back({lgp_test::kRegFpcr, random_fpcr(rng)});
            c.code.push_back(kSentinel);
            out.push_back(std::move(c));
        }
    }
}
