// SIMD&FP decoder vs. Unicorn (P2.4): which instruction words are UNDEFINED?
//
// Executes random words from the SIMD&FP data-processing space and the
// Advanced SIMD load/store structure space one at a time in Unicorn
// (Cortex-A57 model) and compares with a64::decode():
//   - decoder accepts a word Unicorn rejects as UNDEFINED: failure;
//   - decoder says "unallocated" for a word Unicorn executes: failure (gap);
//   - decoder says "not implemented": counted per encoding class, a failure
//     only with --strict.
//
// Usage: lgp_test_simd_validity [count] [seed] [--strict]
#include <unicorn/unicorn.h>

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

#include "case_defs.hpp"
#include "lgp/recomp/a64.hpp"
#include "lgp/recomp/a64_simd.hpp"

namespace {

namespace a64 = lgp::recomp::a64;

constexpr uint64_t kCode = 0x100000;
constexpr uint64_t kData = 0x200000;
constexpr uint64_t kDataSize = 0x200000;
constexpr uint32_t kExcpUdef = 1;

struct Probe {
    bool interrupted = false;
    uint32_t intno = 0;
};

void on_intr(uc_engine* uc, uint32_t intno, void* user) {
    auto* p = static_cast<Probe*>(user);
    p->interrupted = true;
    p->intno = intno;
    uc_emu_stop(uc);
}

// Random word inside the SIMD&FP spaces.
uint32_t random_simd_word(lgp_test::SplitMix& rng) {
    uint32_t w = static_cast<uint32_t>(rng.next());
    switch (rng.next() % 8) {
    case 0:  // load/store structures: 0 Q 0011 0 0/1 ...
        return (w & ~0xBF000000u) | 0x0C000000u | ((w & 1u) << 24);
    case 1:  // scalar FP (bit 30 = 0, bits 28:24 = 1111x)
        return (w & ~0x5E000000u) | 0x1E000000u;
    case 2:  // Advanced SIMD scalar (bits 31:30 = 01, bits 28:24 = 1111x)
        return (w & ~0xDE000000u) | 0x5E000000u;
    case 3:  // Advanced SIMD vector (bit 31 = 0, bits 28:24 = 0111x)
    case 4:
    case 5: return (w & ~0x9E000000u) | 0x0E000000u;
    default:  // anything with op0 = x111
        return w | 0x0E000000u;
    }
}

}  // namespace

int main(int argc, char** argv) {
    uint64_t count = 200000;
    uint64_t seed = 0x53494D44ull;
    bool strict = false;
    int pos = 0;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--strict")) {
            strict = true;
            continue;
        }
        if (pos == 0) count = std::strtoull(argv[i], nullptr, 0);
        if (pos == 1) seed = std::strtoull(argv[i], nullptr, 0);
        ++pos;
    }
    lgp_test::SplitMix rng(seed);

    uc_engine* uc = nullptr;
    if (uc_open(UC_ARCH_ARM64, UC_MODE_ARM, &uc) != UC_ERR_OK) return 2;
    uc_ctl_set_cpu_model(uc, UC_CPU_ARM64_A57);
    uc_mem_map(uc, kCode, 0x1000, UC_PROT_READ | UC_PROT_EXEC);
    uc_mem_map(uc, kData, kDataSize, UC_PROT_READ | UC_PROT_WRITE);
    Probe probe;
    uc_hook hook;
    uc_hook_add(uc, &hook, UC_HOOK_INTR, reinterpret_cast<void*>(&on_intr), &probe, 1, 0);

    uint64_t accepted_undef = 0, gaps = 0, not_impl = 0, agree = 0, valid = 0;
    std::map<std::string, uint64_t> not_impl_by_class, valid_by_name;
    unsigned printed = 0;

    for (uint64_t n = 0; n < count; ++n) {
        const uint32_t w = random_simd_word(rng);
        uc_mem_write(uc, kCode, &w, sizeof(w));
        const uint64_t base = kData + kDataSize / 2;
        const uint64_t small = 16;
        for (int r = UC_ARM64_REG_X0; r <= UC_ARM64_REG_X28; ++r)
            uc_reg_write(uc, r, (r - UC_ARM64_REG_X0) % 2 ? &small : &base);
        uc_reg_write(uc, UC_ARM64_REG_X29, &base);
        uc_reg_write(uc, UC_ARM64_REG_X30, &base);
        uc_reg_write(uc, UC_ARM64_REG_SP, &base);
        probe = {};
        uc_emu_start(uc, kCode, kCode + 4, 0, 1);
        const bool unicorn_undef = probe.interrupted && probe.intno == kExcpUdef;
        const a64::Inst inst = a64::decode(w, kCode);
        const bool decoder_ok = inst.op != a64::Op::Invalid;

        if (decoder_ok && unicorn_undef) {
            ++accepted_undef;
            if (printed++ < 40)
                std::printf("FAIL decoder accepts UNDEFINED word %08x (%s)\n", w, a64::simd::lift(w, kCode, false).name);
        } else if (!decoder_ok && !unicorn_undef) {
            if (inst.invalid == a64::Invalid::NotImplemented) {
                ++not_impl;
                ++not_impl_by_class[a64::simd::class_name(w)];
            } else if (inst.invalid == a64::Invalid::Unallocated) {
                ++gaps;
                if (printed++ < 40) std::printf("GAP  decoder rejects word Unicorn executes: %08x\n", w);
            } else {
                ++agree;  // unpredictable: Unicorn may execute some of these
            }
        } else {
            ++agree;
            if (decoder_ok) {
                ++valid;
                ++valid_by_name[a64::simd::lift(w, kCode, false).name];
            }
        }
    }
    uc_close(uc);

    std::printf("\nwords: %" PRIu64 ", agree: %" PRIu64 " (valid and lifted: %" PRIu64 ")\n", count, agree, valid);
    std::printf("decoder accepts, Unicorn UNDEFINED: %" PRIu64 "\n", accepted_undef);
    std::printf("decoder unallocated, Unicorn executes: %" PRIu64 "\n", gaps);
    std::printf("not implemented yet: %" PRIu64 "\n", not_impl);
    for (const auto& [cls, k] : not_impl_by_class)
        std::printf("    %-22s %" PRIu64 "\n", cls.c_str(), k);
    std::printf("lifted mnemonics: %zu\n", valid_by_name.size());
    return (accepted_undef == 0 && gaps == 0 && (!strict || not_impl == 0)) ? 0 : 1;
}
