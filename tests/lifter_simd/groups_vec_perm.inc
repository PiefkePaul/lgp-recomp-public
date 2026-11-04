// Test groups of the SIMD&FP lifter module vec_perm (recomp/lifter/src/simd/vec_perm.cpp).
// Included by casegen_simd.cpp inside namespace simdgen.

inline const std::vector<Group> kVecPermGroups = {
    // Advanced SIMD copy: 0 Q op 01110000 imm5 0 imm4 1 Rn Rd
    {"vperm_dup_elem", 0xBFE0FC00u, 0x0E000400u, Kind::Alu, 3, VInit::Int, FpcrInit::Zero},
    {"vperm_dup_gen", 0xBFE0FC00u, 0x0E000C00u, Kind::Alu, 3, VInit::Random, FpcrInit::Zero},
    {"vperm_ins_gen", 0xFFE0FC00u, 0x4E001C00u, Kind::Alu, 3, VInit::Random, FpcrInit::Zero},
    {"vperm_ins_elem", 0xFFE08400u, 0x6E000400u, Kind::Alu, 3, VInit::Random, FpcrInit::Zero},
    {"vperm_smov", 0xBFE0FC00u, 0x0E002C00u, Kind::Alu, 3, VInit::Int, FpcrInit::Zero},
    {"vperm_umov", 0xBFE0FC00u, 0x0E003C00u, Kind::Alu, 3, VInit::Int, FpcrInit::Zero},
    {"vperm_copy_mix", 0x9FE08400u, 0x0E000400u, Kind::Alu, 6, VInit::Random, FpcrInit::Zero},
    // Advanced SIMD scalar copy: 01 0 11110000 imm5 0 0000 1 Rn Rd
    {"vperm_scalar_dup", 0xFFE0FC00u, 0x5E000400u, Kind::Alu, 3, VInit::Random, FpcrInit::Zero},
    // Permute: 0 Q 001110 size 0 Rm 0 opc 10 Rn Rd
    {"vperm_permute", 0xBF208C00u, 0x0E000800u, Kind::Alu, 4, VInit::Random, FpcrInit::Zero},
    // Extract: 0 Q 101110 00 0 Rm 0 imm4 0 Rn Rd
    {"vperm_ext", 0xBFE08400u, 0x2E000000u, Kind::Alu, 4, VInit::Random, FpcrInit::Zero},
    // Table lookup: 0 Q 001110 00 0 Rm 0 len op 00 Rn Rd. Random index bytes are mostly out
    // of range, so also run with "interesting" integer lanes (0, 1, small values).
    {"vperm_tbl", 0xBFE08C00u, 0x0E000000u, Kind::Alu, 4, VInit::Random, FpcrInit::Zero},
    {"vperm_tbl_small", 0xBFE08C00u, 0x0E000000u, Kind::Alu, 4, VInit::Int, FpcrInit::Zero},
    // Modified immediate: 0 Q op 0111100000 abc cmode o2 1 defgh Rd
    {"vperm_modimm", 0x9FF80C00u, 0x0F000400u, Kind::Alu, 4, VInit::Random, FpcrInit::Zero},
    {"vperm_modimm_fmov", 0x9FF8FC00u, 0x0F00F400u, Kind::Alu, 2, VInit::Random, FpcrInit::Zero},
    {"vperm_modimm_bytemask", 0x9FF8FC00u, 0x2F00E400u, Kind::Alu, 2, VInit::Random, FpcrInit::Zero},
};

inline void vec_perm_templates(Rng& rng, unsigned per, std::vector<GenCase>& out) {
    (void)rng;
    (void)per;
    (void)out;
}
