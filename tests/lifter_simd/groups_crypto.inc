// Test groups of the SIMD&FP lifter module crypto (recomp/lifter/src/simd/crypto.cpp).
// Included by casegen_simd.cpp inside namespace simdgen.

inline const std::vector<Group> kCryptoGroups = {
    // AES: 01001110 00 10100 001xx 10 Rn Rd
    {"crypto_aes", 0xFFFFCC00u, 0x4E284800u, Kind::Alu, 6, VInit::Random, FpcrInit::Zero},
    // Three-register SHA: 01011110 00 0 Rm 0 opcode 00 Rn Rd
    {"crypto_sha3", 0xFFE08C00u, 0x5E000000u, Kind::Alu, 6, VInit::Random, FpcrInit::Zero},
    // Two-register SHA: 01011110 00 10100 000xx 10 Rn Rd
    {"crypto_sha2", 0xFFFFCC00u, 0x5E280800u, Kind::Alu, 6, VInit::Random, FpcrInit::Zero},
    // AES and two-register SHA mixed (bit 28 selects the class).
    {"crypto_mix", 0xEFFF8C00u, 0x4E280800u, Kind::Alu, 8, VInit::Random, FpcrInit::Zero},
};

inline void crypto_templates(Rng& rng, unsigned per, std::vector<GenCase>& out) {
    (void)rng;
    (void)per;
    (void)out;
}
