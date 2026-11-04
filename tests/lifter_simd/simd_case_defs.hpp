// Extended register numbers of lgp_test::RegOverride for SIMD&FP cases (P2.4).
// 0..30 = x0..x30, 31 = sp, 32 = nzcv as in tests/lifter/case_defs.hpp.
#pragma once

#include <cstdint>

namespace lgp_test {

inline constexpr uint8_t kRegFpcr = 33;
inline constexpr uint8_t kRegFpsr = 34;
inline constexpr uint8_t kRegVLo = 64;  // 64 + n: low 64 bits of Vn
inline constexpr uint8_t kRegVHi = 96;  // 96 + n: high 64 bits of Vn

// FPSR bits the recompiled code does not track: the cumulative IEEE exception
// flags IOC, DZC, OFC, UFC, IXC and IDC (docs/knowledge/a64-fp-simd-semantik.md).
// QC (saturation, bit 27) and the AArch32 NZCV view are compared.
inline constexpr uint32_t kFpsrUntracked = 0x9Fu;

}  // namespace lgp_test
