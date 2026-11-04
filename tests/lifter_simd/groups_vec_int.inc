// Test groups of the SIMD&FP lifter module vec_int (recomp/lifter/src/simd/vec_int.cpp).
// Included by casegen_simd.cpp inside namespace simdgen.
//
// One group per encoding class and opcode family (mask = fixed bits, value =
// their values, everything else random; casegen keeps only words the decoder
// accepts). FP opcodes of the shared classes are excluded by the masks or by
// a patch function so the groups only ever exercise integer instructions.

// Shift by immediate: immh = 0000 is the modified-immediate class, and opcodes
// 11100/11111 are the fixed-point conversions (vec_fp).
inline bool vint_patch_shift_imm(Rng&, uint32_t& w, GenCase&) {
    const uint32_t opc = (w >> 11) & 0x1F;
    return ((w >> 19) & 0xF) != 0 && opc != 0x1C && opc != 0x1F;
}
// Three same: opcodes >= 11000 are FP.
inline bool vint_patch_three_same(Rng&, uint32_t& w, GenCase&) { return ((w >> 11) & 0x1F) < 0x18; }
// Two-register misc: opcodes 011xx and >= 10110 are FP.
inline bool vint_patch_two_misc(Rng&, uint32_t& w, GenCase&) {
    const uint32_t opc = (w >> 12) & 0x1F;
    return !((opc >= 0x0C && opc <= 0x0F) || opc >= 0x16);
}

inline const std::vector<Group> kVecIntGroups = {
    // ---- three same (vector): 0 Q U 01110 size 1 Rm opcode 1 Rn Rd
    {"vint_v3s_op00", 0x9F200400u | 0xE000u, 0x0E200400u | (0x00u << 11), Kind::Alu, 1, VInit::Int, FpcrInit::Zero},
    {"vint_v3s_logic", 0x9F200400u | 0xF800u, 0x0E200400u | (0x03u << 11), Kind::Alu, 1, VInit::Int, FpcrInit::Zero},
    {"vint_v3s_op04", 0x9F200400u | 0xE000u, 0x0E200400u | (0x04u << 11), Kind::Alu, 1, VInit::Int, FpcrInit::Zero},
    {"vint_v3s_shl", 0x9F200400u | 0xE800u, 0x0E200400u | (0x08u << 11), Kind::Alu, 1, VInit::Int, FpcrInit::Zero},
    {"vint_v3s_qshl", 0x9F200400u | 0xE800u, 0x0E200400u | (0x09u << 11), Kind::Alu, 1, VInit::Int, FpcrInit::Zero},
    {"vint_v3s_op0c", 0x9F200400u | 0xE000u, 0x0E200400u | (0x0Cu << 11), Kind::Alu, 1, VInit::Int, FpcrInit::Zero},
    {"vint_v3s_op10", 0x9F200400u | 0xE000u, 0x0E200400u | (0x10u << 11), Kind::Alu, 1, VInit::Int, FpcrInit::Zero},
    {"vint_v3s_op14", 0x9F200400u | 0xE000u, 0x0E200400u | (0x14u << 11), Kind::Alu, 1, VInit::Int, FpcrInit::Zero},
    // ---- three same (scalar): 01 U 11110 size 1 Rm opcode 1 Rn Rd
    {"vint_s3s_op00", 0xDF200400u | 0xC000u, 0x5E200400u | (0x00u << 11), Kind::Alu, 1, VInit::Int, FpcrInit::Zero},
    {"vint_s3s_shl", 0xDF200400u | 0xE000u, 0x5E200400u | (0x08u << 11), Kind::Alu, 1, VInit::Int, FpcrInit::Zero},
    {"vint_s3s_op10", 0xDF200400u | 0xC000u, 0x5E200400u | (0x10u << 11), Kind::Alu, 1, VInit::Int, FpcrInit::Zero},

    // ---- three different (vector): 0 Q U 01110 size 1 Rm opcode 00 Rn Rd
    {"vint_v3d_op0", 0x9F200C00u | 0xC000u, 0x0E200000u | (0x0u << 12), Kind::Alu, 1, VInit::Int, FpcrInit::Zero},
    {"vint_v3d_op4", 0x9F200C00u | 0xC000u, 0x0E200000u | (0x4u << 12), Kind::Alu, 1, VInit::Int, FpcrInit::Zero},
    {"vint_v3d_op8", 0x9F200C00u | 0xC000u, 0x0E200000u | (0x8u << 12), Kind::Alu, 1, VInit::Int, FpcrInit::Zero},
    {"vint_v3d_opc", 0x9F200C00u | 0xC000u, 0x0E200000u | (0xCu << 12), Kind::Alu, 1, VInit::Int, FpcrInit::Zero},
    {"vint_v3d_pmull", 0xBF200C00u | 0xF000u, 0x0E200000u | (0xEu << 12), Kind::Alu, 1, VInit::Random,
     FpcrInit::Zero},
    // ---- three different (scalar): SQDMLAL/SQDMLSL/SQDMULL
    {"vint_s3d", 0xFF200C00u, 0x5E200000u, Kind::Alu, 1, VInit::Int, FpcrInit::Zero},

    // ---- two-register misc (vector): 0 Q U 01110 size 10000 opcode 10 Rn Rd
    {"vint_v2m_op00", 0x9F3E0C00u | 0x18000u, 0x0E200800u | (0x00u << 12), Kind::Alu, 1, VInit::Int, FpcrInit::Zero},
    {"vint_v2m_op08", 0x9F3E0C00u | 0x1C000u, 0x0E200800u | (0x08u << 12), Kind::Alu, 1, VInit::Int, FpcrInit::Zero},
    {"vint_v2m_xtn", 0x9F3E0C00u | 0x1E000u, 0x0E200800u | (0x12u << 12), Kind::Alu, 1, VInit::Int, FpcrInit::Zero},
    {"vint_v2m_qxtn", 0x9F3E0C00u | 0x1F000u, 0x0E200800u | (0x14u << 12), Kind::Alu, 1, VInit::Int, FpcrInit::Zero},
    // ---- two-register misc (scalar): 01 U 11110 size 10000 opcode 10 Rn Rd
    {"vint_s2m_op00", 0xDF3E0C00u | 0x18000u, 0x5E200800u | (0x00u << 12), Kind::Alu, 1, VInit::Int, FpcrInit::Zero},
    {"vint_s2m_op08", 0xDF3E0C00u | 0x1C000u, 0x5E200800u | (0x08u << 12), Kind::Alu, 1, VInit::Int, FpcrInit::Zero},
    {"vint_s2m_sqxtun", 0xDF3E0C00u | 0x1F000u, 0x5E200800u | (0x12u << 12), Kind::Alu, 1, VInit::Int,
     FpcrInit::Zero},
    {"vint_s2m_qxtn", 0xDF3E0C00u | 0x1F000u, 0x5E200800u | (0x14u << 12), Kind::Alu, 1, VInit::Int, FpcrInit::Zero},

    // ---- across lanes: 0 Q U 01110 size 11000 opcode 10 Rn Rd (bit 14 = 0 excludes the FP opcodes)
    {"vint_across", 0x9F3E0C00u | 0x4000u, 0x0E300800u, Kind::Alu, 1, VInit::Int, FpcrInit::Zero},
    // ---- scalar pairwise: ADDP Dd, Vn.2D
    {"vint_s_addp", 0xFFFFFC00u, 0x5EF1B800u, Kind::Alu, 1, VInit::Int, FpcrInit::Zero},

    // ---- shift by immediate (vector): 0 Q U 011110 immh immb opcode 1 Rn Rd
    {"vint_vsh_right", 0x9F800400u | 0xE000u, 0x0F000400u | (0x00u << 11), Kind::Alu, 1, VInit::Int, FpcrInit::Zero,
     vint_patch_shift_imm},
    {"vint_vsh_left", 0x9F800400u | 0xE000u, 0x0F000400u | (0x08u << 11), Kind::Alu, 1, VInit::Int, FpcrInit::Zero,
     vint_patch_shift_imm},
    {"vint_vsh_narrow", 0x9F800400u | 0xE000u, 0x0F000400u | (0x10u << 11), Kind::Alu, 1, VInit::Int,
     FpcrInit::Zero, vint_patch_shift_imm},
    {"vint_vsh_long", 0x9F800400u | 0xF800u, 0x0F000400u | (0x14u << 11), Kind::Alu, 1, VInit::Int, FpcrInit::Zero,
     vint_patch_shift_imm},
    // ---- shift by immediate (scalar): 01 U 111110 immh immb opcode 1 Rn Rd
    {"vint_ssh_right", 0xDF800400u | 0xE000u, 0x5F000400u | (0x00u << 11), Kind::Alu, 1, VInit::Int, FpcrInit::Zero,
     vint_patch_shift_imm},
    {"vint_ssh_left", 0xDF800400u | 0xE000u, 0x5F000400u | (0x08u << 11), Kind::Alu, 1, VInit::Int, FpcrInit::Zero,
     vint_patch_shift_imm},
    {"vint_ssh_narrow", 0xDF800400u | 0xE000u, 0x5F000400u | (0x10u << 11), Kind::Alu, 1, VInit::Int,
     FpcrInit::Zero, vint_patch_shift_imm},

    // ---- by element (vector): 0 Q U 01111 size L M Rm opcode H 0 Rn Rd
    {"vint_vix_x0", 0x9F000400u | 0x3000u, 0x0F000000u | (0x0u << 12), Kind::Alu, 1, VInit::Int, FpcrInit::Zero},
    {"vint_vix_x2", 0x9F000400u | 0x3000u, 0x0F000000u | (0x2u << 12), Kind::Alu, 1, VInit::Int, FpcrInit::Zero},
    {"vint_vix_x3", 0x9F000400u | 0x3000u, 0x0F000000u | (0x3u << 12), Kind::Alu, 1, VInit::Int, FpcrInit::Zero},
    {"vint_vix_sqrdmulh", 0x9F000400u | 0xF000u, 0x0F000000u | (0xDu << 12), Kind::Alu, 1, VInit::Int,
     FpcrInit::Zero},
    // ---- by element (scalar): 01 U 11111 size L M Rm opcode H 0 Rn Rd
    {"vint_six_x3", 0xDF000400u | 0x3000u, 0x5F000000u | (0x3u << 12), Kind::Alu, 1, VInit::Int, FpcrInit::Zero},
    {"vint_six_mulh", 0xDF000400u | 0xE000u, 0x5F000000u | (0xCu << 12), Kind::Alu, 1, VInit::Int, FpcrInit::Zero},

    // ---- multi-instruction sequences (results feed later instructions, QC accumulates)
    {"vint_seq_three_same", 0x9F200400u, 0x0E200400u, Kind::Alu, 4, VInit::Int, FpcrInit::Zero,
     vint_patch_three_same},
    {"vint_seq_two_misc", 0x9F3E0C00u, 0x0E200800u, Kind::Alu, 4, VInit::Int, FpcrInit::Zero, vint_patch_two_misc},
    {"vint_seq_shift_imm", 0x9F800400u, 0x0F000400u, Kind::Alu, 4, VInit::Int, FpcrInit::Zero,
     vint_patch_shift_imm},
};

// Shift by register (SSHL/USHL/SRSHL/URSHL/SQSHL/UQSHL/SQRSHL/UQRSHL, vector
// and scalar) with shift amounts at the interesting boundaries (+-esize,
// +-esize+-1, +-64, 127, -128): random encodings rarely hit them for wide lanes.
inline void vec_int_templates(Rng& rng, unsigned per, std::vector<GenCase>& out) {
    for (unsigned k = 0; k < 2 * per; ++k) {
        GenCase c = make_case("vint_tpl_shlreg", k, rng);
        const bool scalar = rng.chance(30);
        const unsigned opc = 0x08 + static_cast<unsigned>(rng.below(4));
        unsigned size = static_cast<unsigned>(rng.below(4));
        if (scalar && (opc & 1) == 0) size = 3;  // scalar SSHL/USHL/SRSHL/URSHL are 64-bit only
        const bool q = size == 3 || rng.chance(50);
        const uint32_t u = rng.chance(50) ? 1u : 0u;
        const unsigned d = static_cast<unsigned>(rng.below(32));
        const unsigned n = static_cast<unsigned>(rng.below(32));
        unsigned m = static_cast<unsigned>(rng.below(32));
        if (m == n) m = (m + 1) & 31;
        uint32_t w = (scalar ? 0x5E200400u : (0x0E200400u | (q ? 1u << 30 : 0u))) | (u << 29) | (size << 22) |
                     (m << 16) | (opc << 11) | (n << 5) | d;
        if (!valid(w)) continue;
        c.code.push_back(w);
        c.code.push_back(kSentinel);

        const int esize = 8 << size;
        const int shifts[] = {-esize - 1, -esize, -esize + 1, -1, 0, 1, esize - 1, esize, esize + 1, 127, -128,
                              -64,        64,     -65,        65, -63, 63, -33, 33, -32, 32, -31, 31, -17, 17,
                              -16,        16,     -15,        15, -9, 9, -8, 8, -7, 7, 2, -2};
        uint64_t mv[2] = {0, 0}, nv[2] = {0, 0}, dv[2] = {0, 0};
        for (unsigned h = 0; h < 2; ++h) {
            for (int bit = 0; bit < 64; bit += esize) {
                const int s = shifts[rng.below(std::size(shifts))];
                uint64_t lanev = esize == 8 ? 0 : (rng.u64() << 8);
                lanev |= static_cast<uint8_t>(static_cast<int8_t>(s));
                const uint64_t lm = esize == 64 ? ~0ull : ((1ull << esize) - 1);
                mv[h] |= (lanev & lm) << bit;
                nv[h] |= int_lane(rng, static_cast<unsigned>(esize)) << bit;
                dv[h] |= int_lane(rng, static_cast<unsigned>(esize)) << bit;
            }
        }
        c.overrides.push_back({static_cast<uint8_t>(lgp_test::kRegVLo + d), dv[0]});
        c.overrides.push_back({static_cast<uint8_t>(lgp_test::kRegVHi + d), dv[1]});
        c.overrides.push_back({static_cast<uint8_t>(lgp_test::kRegVLo + n), nv[0]});
        c.overrides.push_back({static_cast<uint8_t>(lgp_test::kRegVHi + n), nv[1]});
        c.overrides.push_back({static_cast<uint8_t>(lgp_test::kRegVLo + m), mv[0]});
        c.overrides.push_back({static_cast<uint8_t>(lgp_test::kRegVHi + m), mv[1]});
        out.push_back(std::move(c));
    }
}
