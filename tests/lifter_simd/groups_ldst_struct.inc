// Test groups of the SIMD&FP lifter module ldst_struct (recomp/lifter/src/simd/ldst_struct.cpp).
// Included by casegen_simd.cpp inside namespace simdgen.

// Points Rn at one of the data-window base registers and, for register
// post-index, Rm at the small non-negative index register (or keeps Rm = 31,
// the immediate form). Loads only write V registers, so the base registers
// stay inside the data window over a sequence: each instruction advances them
// by at most 64 (immediate) or 127 (kIndexPos) bytes.
inline bool patch_ldst_struct(Rng& rng, uint32_t& w, GenCase& c) {
    (void)c;
    w = set_field(w, 5, 5, kBaseRegs[rng.below(std::size(kBaseRegs))]);
    if (w & (1u << 23)) w = set_field(w, 16, 5, rng.chance(50) ? 31u : kIndexPos);
    return true;
}

inline const std::vector<Group> kLdStStructGroups = {
    // Multiple structures: 0 Q 0011000 L 000000 opcode size Rn Rt / 0 Q 0011001 L 0 Rm opcode size Rn Rt
    {"vldst_multi", 0xBFBF0000u, 0x0C000000u, Kind::Mem, 3, VInit::Random, FpcrInit::Zero, patch_ldst_struct},
    {"vldst_multi_post", 0xBFA00000u, 0x0C800000u, Kind::Mem, 4, VInit::Random, FpcrInit::Zero, patch_ldst_struct},
    // LD1/ST1 with 1-4 registers only (opcode x x 1 0 / 0111).
    {"vldst_ld1_multi", 0xBFBF2000u, 0x0C002000u, Kind::Mem, 3, VInit::Random, FpcrInit::Zero, patch_ldst_struct},
    // Single structures: 0 Q 0011010 L R 00000 opcode S size Rn Rt / 0 Q 0011011 L R Rm opcode S size Rn Rt
    {"vldst_single", 0xBF9F0000u, 0x0D000000u, Kind::Mem, 3, VInit::Random, FpcrInit::Zero, patch_ldst_struct},
    {"vldst_single_post", 0xBF800000u, 0x0D800000u, Kind::Mem, 4, VInit::Random, FpcrInit::Zero, patch_ldst_struct},
    // LD1R-LD4R: opcode 11x, L = 1, S = 0.
    {"vldst_rep", 0xBFDFD000u, 0x0D40C000u, Kind::Mem, 3, VInit::Random, FpcrInit::Zero, patch_ldst_struct},
    {"vldst_rep_post", 0xBFC0D000u, 0x0DC0C000u, Kind::Mem, 4, VInit::Random, FpcrInit::Zero, patch_ldst_struct},
    // Any load/store structure instruction, longer sequences (stores followed by loads of the same area).
    {"vldst_mix", 0xBE000000u, 0x0C000000u, Kind::Mem, 8, VInit::Random, FpcrInit::Zero, patch_ldst_struct},
};

inline void ldst_struct_templates(Rng& rng, unsigned per, std::vector<GenCase>& out) {
    (void)rng;
    (void)per;
    (void)out;
}
