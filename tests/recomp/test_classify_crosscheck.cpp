// classify() (on the lifter's decoder) against the CFG's original classifier.
// Wherever the decoder accepts a word, both must agree on kind, target and
// operands; where it rejects one (unallocated, UNPREDICTABLE, EL1-only), the
// new classifier must say Unknown (or keep the architectural class for the
// privileged branch/exception encodings). Random words plus every encoding in
// the branch/exception/system space with random low bits.
#include <cstdio>
#include <map>
#include <random>
#include <string>

#include "classify_reference.hpp"
#include "lgp/recomp/a64.hpp"
#include "lgp/recomp/a64_cf.hpp"

using lgp::recomp::CfInfo;
using lgp::recomp::CfKind;
using lgp::recomp::CondKind;
namespace a64 = lgp::recomp::a64;

namespace {

int g_failures = 0;
std::map<std::string, unsigned> g_rejected;  // old kind → count where the decoder rejected the word

const char* kind_name(CfKind k) {
    static const char* n[] = {"None", "Branch", "CondBranch", "Call", "IndirectBranch",
                              "IndirectCall", "Return", "Trap", "Svc", "Unknown"};
    return n[static_cast<unsigned>(k)];
}

void check(uint32_t insn, uint64_t pc) {
    const CfInfo now = lgp::recomp::classify(insn, pc);
    const CfInfo old = lgp::recomp::ref::classify_reference(insn, pc);
    const a64::Inst d = a64::decode(insn, pc);
    const bool rejected = (insn & 0xFFFF0000u) != 0 && d.op == a64::Op::Invalid &&
                          d.invalid != a64::Invalid::NotImplemented;
    std::string why;
    if (rejected) {
        ++g_rejected[kind_name(old.kind)];
        // Privileged branch/exception encodings keep their class, everything else is Unknown.
        if (now.kind != CfKind::Unknown && !(d.invalid == a64::Invalid::Privileged && now.kind == old.kind))
            why = "rejected word classified as " + std::string(kind_name(now.kind));
    } else if (now.kind != old.kind) {
        why = std::string("kind ") + kind_name(now.kind) + " != " + kind_name(old.kind);
    } else {
        switch (now.kind) {
            case CfKind::Branch:
            case CfKind::Call:
                if (now.target != old.target) why = "target";
                break;
            case CfKind::CondBranch:
                if (now.target != old.target || now.cond_kind != old.cond_kind) why = "target/cond_kind";
                else if (now.cond_kind == CondKind::BCond && now.cond != old.cond) why = "cond";
                else if (now.cond_kind != CondKind::BCond && now.reg != old.reg) why = "reg";
                break;
            case CfKind::IndirectBranch:
            case CfKind::IndirectCall:
            case CfKind::Return:
                if (now.reg != old.reg) why = "reg";
                break;
            case CfKind::Svc:
            case CfKind::Trap:
                if (now.imm != old.imm) why = "imm";
                break;
            default: break;
        }
    }
    if (!why.empty() && ++g_failures <= 20)
        std::printf("FAIL %08x @%llx: %s (%s)\n", insn, static_cast<unsigned long long>(pc), why.c_str(),
                    a64::describe(d).c_str());
}

}  // namespace

int main() {
    std::mt19937_64 rng(0x4C47'4346);
    const uint64_t pc = 0x0800'1000;
    unsigned n = 0;
    for (int k = 0; k < 1'000'000; ++k, ++n) check(static_cast<uint32_t>(rng()), pc);
    // Branches, exception generation and system (op0 = 101x), denser: every top 11 bits.
    for (uint32_t top = 0; top < 0x800; ++top) {
        if (((top >> 7) & 0xE) != 0xA) continue;
        for (int k = 0; k < 256; ++k, ++n) check((top << 21) | (static_cast<uint32_t>(rng()) & 0x1FFFFF), pc);
    }
    // Targets near the bottom and top of the address space (wrap-around).
    for (int k = 0; k < 10000; ++k, ++n) check(0x14000000u | (static_cast<uint32_t>(rng()) & 0x03FFFFFF), 4 * (rng() & 0xFFFF));

    std::printf("%u words, %d mismatches; words the decoder rejects, by old class:", n, g_failures);
    for (const auto& [k, c] : g_rejected) std::printf(" %s=%u", k.c_str(), c);
    std::printf("\n");
    return g_failures == 0 ? 0 : 1;
}
