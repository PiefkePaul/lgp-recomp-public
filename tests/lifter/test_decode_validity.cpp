// Decoder vs. Unicorn: which instruction words are UNDEFINED?
//
// Executes random instruction words one at a time in Unicorn (Cortex-A57
// model) and compares "undefined instruction" with the decoder's Op::Invalid.
//   - Decoder accepts a word that Unicorn rejects as UNDEFINED: failure (the
//     lifter would give meaning to an instruction the CPU traps on).
//   - Decoder rejects a word that Unicorn executes: reported per reason
//     (a64::Invalid). NotImplemented (SIMD&FP, P2.4), Unpredictable and
//     Privileged (Unicorn runs in EL1, Switch code in EL0) are expected;
//     "Unallocated" means the decoder misses a valid instruction and fails.
//
// Usage: lgp_test_decode_validity [count] [seed]
#include <unicorn/unicorn.h>

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>

#include "case_defs.hpp"
#include "lgp/recomp/a64.hpp"

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

const char* reason_name(a64::Invalid r) {
    switch (r) {
    case a64::Invalid::None: return "none";
    case a64::Invalid::Unallocated: return "unallocated";
    case a64::Invalid::Unpredictable: return "unpredictable";
    case a64::Invalid::Privileged: return "privileged (Unicorn runs EL1)";
    case a64::Invalid::NotImplemented: return "not implemented (P2.4)";
    }
    return "?";
}

}  // namespace

int main(int argc, char** argv) {
    const uint64_t count = argc > 1 ? std::strtoull(argv[1], nullptr, 0) : 200000;
    lgp_test::SplitMix rng(argc > 2 ? std::strtoull(argv[2], nullptr, 0) : 0x56414C4944ull);

    uc_engine* uc = nullptr;
    if (uc_open(UC_ARCH_ARM64, UC_MODE_ARM, &uc) != UC_ERR_OK) return 2;
    uc_ctl_set_cpu_model(uc, UC_CPU_ARM64_A57);
    uc_mem_map(uc, kCode, 0x1000, UC_PROT_READ | UC_PROT_EXEC);
    uc_mem_map(uc, kData, kDataSize, UC_PROT_READ | UC_PROT_WRITE);
    Probe probe;
    uc_hook hook;
    uc_hook_add(uc, &hook, UC_HOOK_INTR, reinterpret_cast<void*>(&on_intr), &probe, 1, 0);

    uint64_t accepted_undef = 0, rejected_valid = 0, unexplained = 0, agree = 0;
    std::map<std::string, uint64_t> rejected_by_category;
    unsigned printed = 0;

    for (uint64_t n = 0; n < count; ++n) {
        const uint32_t w = static_cast<uint32_t>(rng.next());
        uc_mem_write(uc, kCode, &w, sizeof(w));
        const uint64_t base = kData + kDataSize / 2;
        for (int r = UC_ARM64_REG_X0; r <= UC_ARM64_REG_X28; ++r)
            uc_reg_write(uc, r, &base);
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
            if (printed++ < 30)
                std::printf("FAIL decoder accepts UNDEFINED word: %s\n", a64::describe(a64::decode(w, kCode)).c_str());
        } else if (!decoder_ok && !unicorn_undef) {
            ++rejected_valid;
            ++rejected_by_category[reason_name(inst.invalid)];
            if (inst.invalid == a64::Invalid::Unallocated) {
                ++unexplained;
                if (printed++ < 30) std::printf("GAP  decoder rejects word Unicorn executes: %08x\n", w);
            }
        } else {
            ++agree;
        }
    }
    uc_close(uc);

    std::printf("\nwords: %" PRIu64 ", agree: %" PRIu64 "\n", count, agree);
    std::printf("decoder accepts, Unicorn UNDEFINED: %" PRIu64 "\n", accepted_undef);
    std::printf("decoder rejects, Unicorn executes:  %" PRIu64 "\n", rejected_valid);
    for (const auto& [cat, k] : rejected_by_category)
        std::printf("    %-26s %" PRIu64 "\n", cat.c_str(), k);
    std::printf("    of which unexplained (decoder says unallocated): %" PRIu64 "\n", unexplained);
    return (accepted_undef == 0 && unexplained == 0) ? 0 : 1;
}
