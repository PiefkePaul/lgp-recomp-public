// Integer encoding groups and memory-operand fix-ups shared by the lifter
// case generator (casegen.cpp) and the mixed-sequence fuzzer
// (tests/lifter_fuzz). Templated on the generator's Rng and case types, which
// only need below()/chance()/u64() and an `overrides` vector of RegOverride.
#pragma once

#include <cstdint>
#include <iterator>

#include "case_defs.hpp"
#include "lgp/recomp/a64.hpp"

namespace lgp_test::intgen {

namespace a64 = lgp::recomp::a64;

enum class Kind { Alu, Mem, Literal };

struct Group {
    const char* name;
    uint32_t mask;   // fixed bits
    uint32_t value;  // their values
    Kind kind;
    unsigned max_len;  // instructions per case (1..max_len)
};

inline constexpr Group kGroups[] = {
    {"adr", 0x1F000000u, 0x10000000u, Kind::Alu, 3},
    {"addsub_imm", 0x1F800000u, 0x11000000u, Kind::Alu, 4},
    {"logic_imm", 0x1F800000u, 0x12000000u, Kind::Alu, 4},
    {"movewide", 0x1F800000u, 0x12800000u, Kind::Alu, 4},
    {"bitfield", 0x1F800000u, 0x13000000u, Kind::Alu, 3},
    {"extr", 0x7FA00000u, 0x13800000u, Kind::Alu, 3},
    {"logic_reg", 0x1F000000u, 0x0A000000u, Kind::Alu, 4},
    {"addsub_reg", 0x1F200000u, 0x0B000000u, Kind::Alu, 4},
    {"addsub_ext", 0x1F200000u, 0x0B200000u, Kind::Alu, 4},
    {"adc_sbc", 0x1FE0FC00u, 0x1A000000u, Kind::Alu, 4},
    {"ccmp", 0x3FE00410u, 0x3A400000u, Kind::Alu, 4},
    {"csel", 0x3FE00800u, 0x1A800000u, Kind::Alu, 4},
    {"dp3", 0x7F000000u, 0x1B000000u, Kind::Alu, 3},
    {"dp2", 0x7FE00000u, 0x1AC00000u, Kind::Alu, 3},
    {"dp1", 0x7FFF0000u, 0x5AC00000u, Kind::Alu, 3},
    {"ldst_uimm", 0x3B000000u, 0x39000000u, Kind::Mem, 4},
    {"ldst_imm9", 0x3B200000u, 0x38000000u, Kind::Mem, 4},
    {"ldst_regoff", 0x3B200C00u, 0x38200800u, Kind::Mem, 4},
    {"ldst_pair", 0x3A000000u, 0x28000000u, Kind::Mem, 4},
    {"ldr_literal", 0x3B000000u, 0x18000000u, Kind::Literal, 3},
};

// Registers reserved for memory cases: base registers point into the data
// window, two index registers hold small offsets. Data registers are x0..x17.
inline constexpr unsigned kBaseRegs[] = {20, 21, 22, 23, 24, 25, 26, 27, 28, 31};
inline constexpr unsigned kIndexPos = 19;  // small non-negative index
inline constexpr unsigned kIndexNeg = 18;  // small negative index
inline constexpr unsigned kDataRegMax = 17;

inline uint32_t set_field(uint32_t w, int lo, int width, uint32_t v) {
    const uint32_t m = ((1u << width) - 1) << lo;
    return (w & ~m) | ((v << lo) & m);
}

template <typename Rng, typename Case>
void memory_overrides(Rng& rng, Case& c, unsigned align_log2) {
    for (unsigned r : kBaseRegs) {
        uint64_t v = kDataWindow + rng.below(0x1000);
        if (align_log2) v &= ~((uint64_t{1} << align_log2) - 1);
        if (r == 31) v = kDataWindow + 16 * rng.below(0x100);
        c.overrides.push_back({static_cast<uint8_t>(r), v});
    }
    c.overrides.push_back({kIndexPos, rng.below(128)});
    c.overrides.push_back({kIndexNeg, static_cast<uint64_t>(-static_cast<int64_t>(1 + rng.below(128)))});
}

// Patch the register fields of a memory instruction so it hits the data
// window, then re-decode; returns false if the result is not usable.
template <typename Rng>
bool patch_memory(Rng& rng, uint32_t& w, uint64_t pc) {
    a64::Inst i = a64::decode(w, pc);
    if (i.op == a64::Op::Invalid) return false;
    const unsigned rt = i.simd ? static_cast<unsigned>(rng.below(32))
                               : (rng.chance(8) ? 31u : static_cast<unsigned>(rng.below(kDataRegMax + 1)));
    unsigned rt2 = i.simd ? static_cast<unsigned>(rng.below(32))
                          : (rng.chance(8) ? 31u : static_cast<unsigned>(rng.below(kDataRegMax + 1)));
    if (rt2 == rt) rt2 = (rt + 1) % 18;
    w = set_field(w, 0, 5, rt);
    w = set_field(w, 5, 5, kBaseRegs[rng.below(std::size(kBaseRegs))]);
    if (i.op == a64::Op::LdStPair) w = set_field(w, 10, 5, rt2);
    if (i.op == a64::Op::LdSt || i.op == a64::Op::Prfm) {
        if (i.mode == a64::AddrMode::RegOffset) {
            const bool uxtw = i.extend == a64::Extend::Uxtw;
            w = set_field(w, 16, 5, (uxtw || rng.chance(50)) ? kIndexPos : kIndexNeg);
        }
        // Unsigned offsets of 128-bit accesses would leave the data window.
        if ((w & 0x3B000000u) == 0x39000000u && i.size_log2 == 4) w = set_field(w, 10, 12, (w >> 10) & 0x7FF);
    }
    const a64::Inst j = a64::decode(w, pc);
    return j.op != a64::Op::Invalid;
}

// Point a literal load somewhere into the read-only region.
template <typename Rng>
bool patch_literal(Rng& rng, uint32_t& w, uint64_t pc) {
    // Target somewhere in the read-only region, 4-byte aligned, room for 16 bytes.
    const uint64_t target = kRodataBase + (rng.below(kRodataSize - 16) & ~3ull);
    const int64_t off = static_cast<int64_t>(target - pc) / 4;
    w = set_field(w, 5, 19, static_cast<uint32_t>(off) & 0x7FFFF);
    w = set_field(w, 0, 5, static_cast<uint32_t>(rng.below(32)) % 18);
    if (w & (1u << 26)) w = set_field(w, 0, 5, static_cast<uint32_t>(rng.below(32)));
    return a64::decode(w, pc).op != a64::Op::Invalid;
}

}  // namespace lgp_test::intgen
