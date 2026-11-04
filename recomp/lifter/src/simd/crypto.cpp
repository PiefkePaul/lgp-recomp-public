// SIMD&FP lifter module: crypto (P2.4).
//
// Cryptographic Extension of ARMv8.0 (Arm ARM DDI 0487, C4.1 "Cryptographic
// AES", "Cryptographic three-register SHA", "Cryptographic two-register SHA"):
// AESE/AESD/AESMC/AESIMC, SHA1C/SHA1P/SHA1M/SHA1SU0/SHA256H/SHA256H2/
// SHA256SU1, SHA1H/SHA1SU1/SHA256SU0. SHA512, SHA3, SM3 and SM4 are later
// extensions and unallocated on the Cortex-A57.
#include "internal.hpp"

namespace lgp::recomp::a64::simd::detail {
namespace {

const char* const kNs = "lgp::lifted::simd::";

Lifted lift_aes(uint32_t raw, bool emit) {
    if (bits(raw, 23, 22) != 0) return invalid();
    static const char* const kNames[] = {"aese", "aesd", "aesmc", "aesimc"};
    const uint32_t opcode = bits(raw, 16, 12);
    if (opcode < 0b00100 || opcode > 0b00111) return invalid();
    const char* name = kNames[opcode - 0b00100];
    if (!emit) return ok(name);
    const unsigned rn = bits(raw, 9, 5), rd = bits(raw, 4, 0);
    const std::string args = opcode < 0b00110 ? vreg(rd) + ", " + vreg(rn) : vreg(rn);
    return ok(name, vreg(rd) + " = " + kNs + name + "(" + args + ");");
}

Lifted lift_sha3(uint32_t raw, bool emit) {
    if (bits(raw, 23, 22) != 0) return invalid();
    static const char* const kNames[] = {"sha1c", "sha1p", "sha1m", "sha1su0", "sha256h", "sha256h2", "sha256su1"};
    const uint32_t opcode = bits(raw, 14, 12);
    if (opcode == 0b111) return invalid();
    const char* name = kNames[opcode];
    if (!emit) return ok(name);
    const std::string d = vreg(bits(raw, 4, 0)), n = vreg(bits(raw, 9, 5)), m = vreg(bits(raw, 20, 16));
    if (opcode <= 0b010)
        return ok(name, d + " = " + kNs + "sha1_hash(" + d + ", static_cast<uint32_t>(" + n + ".lo), " + m + ", " +
                            num(opcode) + ");");
    return ok(name, d + " = " + kNs + name + "(" + d + ", " + n + ", " + m + ");");
}

Lifted lift_sha2(uint32_t raw, bool emit) {
    if (bits(raw, 23, 22) != 0) return invalid();
    static const char* const kNames[] = {"sha1h", "sha1su1", "sha256su0"};
    const uint32_t opcode = bits(raw, 16, 12);
    if (opcode > 0b00010) return invalid();
    const char* name = kNames[opcode];
    if (!emit) return ok(name);
    const std::string d = vreg(bits(raw, 4, 0)), n = vreg(bits(raw, 9, 5));
    if (opcode == 0) return ok(name, d + " = " + kNs + "sha1h(" + n + ");");
    return ok(name, d + " = " + kNs + name + "(" + d + ", " + n + ");");
}

}  // namespace

Lifted lift_crypto(Class cls, uint32_t raw, bool emit) {
    switch (cls) {
    case Class::CryptoAes: return lift_aes(raw, emit);
    case Class::CryptoSha3: return lift_sha3(raw, emit);
    case Class::CryptoSha2: return lift_sha2(raw, emit);
    default: return invalid();
    }
}

}  // namespace lgp::recomp::a64::simd::detail
