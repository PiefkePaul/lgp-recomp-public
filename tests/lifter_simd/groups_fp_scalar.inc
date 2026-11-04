// Test groups of the SIMD&FP lifter module fp_scalar (recomp/lifter/src/simd/fp_scalar.cpp).
// Included by casegen_simd.cpp inside namespace simdgen.

inline const std::vector<Group> kFpScalarGroups = {
    // FMADD/FMSUB/FNMADD/FNMSUB, single and double
    {"fp_dp3", 0xFF800000u, 0x1F000000u, Kind::Alu, 3, VInit::Fp, FpcrInit::Random},
    // FMUL/FDIV/FADD/FSUB/FMAX/FMIN/FMAXNM/FMINNM/FNMUL
    {"fp_dp2", 0xFFA00C00u, 0x1E200800u, Kind::Alu, 3, VInit::Fp, FpcrInit::Random},
    {"fp_dp2_fpcr0", 0xFFA00C00u, 0x1E200800u, Kind::Alu, 1, VInit::Fp, FpcrInit::Zero},
    // FMOV/FABS/FNEG/FSQRT (opcode 0000xx)
    {"fp_dp1", 0xFFBC7C00u, 0x1E204000u, Kind::Alu, 3, VInit::Fp, FpcrInit::Random},
    // FCVT between half, single and double
    {"fp_fcvt", 0xFF3E7C00u, 0x1E224000u, Kind::Alu, 2, VInit::Fp, FpcrInit::Random},
    // FRINTN/P/M/Z/A/X/I
    {"fp_frint", 0xFFBC7C00u, 0x1E244000u, Kind::Alu, 2, VInit::Fp, FpcrInit::Random},
    // FCMP/FCMPE (register and #0.0)
    {"fp_cmp", 0xFFA0FC07u, 0x1E202000u, Kind::Alu, 2, VInit::Fp, FpcrInit::Random},
    // FMOV (scalar, immediate)
    {"fp_imm", 0xFFA01FE0u, 0x1E201000u, Kind::Alu, 2, VInit::Fp, FpcrInit::Random},
    // FCCMP/FCCMPE
    {"fp_ccmp", 0xFFA00C00u, 0x1E200400u, Kind::Alu, 2, VInit::Fp, FpcrInit::Random},
    // FCSEL
    {"fp_csel", 0xFFA00C00u, 0x1E200C00u, Kind::Alu, 2, VInit::Fp, FpcrInit::Random},
    // SCVTF/UCVTF/FCVTZS/FCVTZU with fbits
    {"fp_fixed_conv", 0x7FA00000u, 0x1E000000u, Kind::Alu, 2, VInit::Fp, FpcrInit::Random},
    // FCVT[NPMZA][SU], [SU]CVTF, FMOV general <-> S/D
    {"fp_int_conv", 0x7FA0FC00u, 0x1E200000u, Kind::Alu, 2, VInit::Fp, FpcrInit::Random},
    // FMOV Xd, Vn.D[1] and FMOV Vd.D[1], Xn
    {"fp_fmov_top", 0xFFFEFC00u, 0x9EAE0000u, Kind::Alu, 2, VInit::Fp, FpcrInit::Zero},
};

inline void fp_scalar_templates(Rng& rng, unsigned per, std::vector<GenCase>& out) {
    (void)rng;
    (void)per;
    (void)out;
}
