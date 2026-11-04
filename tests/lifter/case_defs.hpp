// Shared definitions between the lifter test-case generator (casegen.cpp), the
// generated shards and the differential test driver.
#pragma once

#include <cstddef>
#include <cstdint>

#include "lgp/lifted/cpu_state.hpp"

namespace lgp_test {

// Guest memory layout of every generated case.
inline constexpr uint64_t kRodataBase = 0x0F0000;  // read-only, random bytes (literal loads)
inline constexpr uint64_t kRodataSize = 0x10000;
inline constexpr uint64_t kCodeBase = 0x100000;  // read + execute
inline constexpr uint64_t kCodeSize = 0x1000;
inline constexpr uint64_t kDataBase = 0x200000;  // read/write, random bytes
inline constexpr uint64_t kDataSize = 0x10000;
// Base registers of memory cases point into [kDataWindow, kDataWindow + 0x1000).
inline constexpr uint64_t kDataWindow = 0x204000;
inline constexpr uint64_t kStackTop = 0x20C000;

struct RegOverride {
    uint8_t reg;  // 0..30 = x0..x30, 31 = sp, 32 = nzcv (bits 31..28)
    uint64_t value;
};

using LookupFn = lgp::LiftedFn (*)(uint64_t va);

struct CaseDef {
    const char* group;
    const char* name;
    uint64_t seed;  // initial registers and memory contents
    const uint32_t* code;
    uint32_t code_words;
    const RegOverride* overrides;
    uint32_t override_count;
    uint64_t entry_pc;    // initial pc
    lgp::LiftedFn entry;  // lifted function at entry_pc
    LookupFn lookup;      // every function of the case, for indirect calls
};

struct CaseShard {
    const CaseDef* cases;
    size_t count;
};

// SplitMix64: deterministic stream for registers and memory.
struct SplitMix {
    uint64_t s;
    explicit SplitMix(uint64_t seed) : s(seed) {}
    uint64_t next() {
        uint64_t z = (s += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
};

}  // namespace lgp_test
