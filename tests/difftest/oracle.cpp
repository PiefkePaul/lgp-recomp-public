#include "oracle.h"

#include <cstdio>

namespace lgp::difftest {

const char* to_string(Outcome o) {
    switch (o) {
    case Outcome::Sentinel: return "sentinel";
    case Outcome::ExitProcess: return "exit-process";
    case Outcome::StopAddress: return "stop-address";
    case Outcome::InstructionLimit: return "instruction-limit";
    case Outcome::Fault: return "fault";
    }
    return "?";
}

namespace {

std::string hex(uint64_t v) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "0x%llx", static_cast<unsigned long long>(v));
    return buf;
}

void cmp(std::vector<std::string>& out, const std::string& what, uint64_t a, uint64_t b) {
    if (a != b)
        out.push_back(what + ": " + hex(a) + " != " + hex(b));
}

}  // namespace

std::vector<std::string> diff(const RunResult& a, const RunResult& b) {
    std::vector<std::string> out;
    if (a.outcome != b.outcome)
        out.push_back(std::string("outcome: ") + to_string(a.outcome) + " != " + to_string(b.outcome));

    const GuestState& sa = a.final;
    const GuestState& sb = b.final;
    for (size_t i = 0; i < sa.x.size(); ++i)
        cmp(out, "x" + std::to_string(i), sa.x[i], sb.x[i]);
    cmp(out, "sp", sa.sp, sb.sp);
    cmp(out, "pc", sa.pc, sb.pc);
    cmp(out, "nzcv", sa.nzcv, sb.nzcv);
    for (size_t i = 0; i < sa.v.size(); ++i) {
        cmp(out, "v" + std::to_string(i) + ".lo", sa.v[i].lo, sb.v[i].lo);
        cmp(out, "v" + std::to_string(i) + ".hi", sa.v[i].hi, sb.v[i].hi);
    }
    cmp(out, "fpcr", sa.fpcr, sb.fpcr);
    cmp(out, "fpsr", sa.fpsr, sb.fpsr);
    cmp(out, "tpidr_el0", sa.tpidr_el0, sb.tpidr_el0);
    cmp(out, "tpidrro_el0", sa.tpidrro_el0, sb.tpidrro_el0);

    if (a.memory.size() != b.memory.size()) {
        out.push_back("memory: " + std::to_string(a.memory.size()) + " regions != " +
                      std::to_string(b.memory.size()));
    } else {
        for (size_t r = 0; r < a.memory.size(); ++r) {
            const MemoryRegion& ma = a.memory[r];
            const MemoryRegion& mb = b.memory[r];
            if (ma.base != mb.base || ma.bytes.size() != mb.bytes.size()) {
                out.push_back("memory region " + std::to_string(r) + ": layout differs");
                continue;
            }
            for (size_t i = 0; i < ma.bytes.size(); ++i) {
                if (ma.bytes[i] != mb.bytes[i]) {
                    out.push_back("memory " + hex(ma.base + i) + ": " + hex(ma.bytes[i]) + " != " +
                                  hex(mb.bytes[i]) + " (first difference in region)");
                    break;
                }
            }
        }
    }

    if (a.svcs.size() != b.svcs.size())
        out.push_back("svc count: " + std::to_string(a.svcs.size()) + " != " + std::to_string(b.svcs.size()));
    for (size_t i = 0; i < a.svcs.size() && i < b.svcs.size(); ++i) {
        if (!(a.svcs[i] == b.svcs[i]))
            out.push_back("svc #" + std::to_string(i) + " (imm " + hex(a.svcs[i].imm) + " vs " +
                          hex(b.svcs[i].imm) + ") differs");
    }
    return out;
}

}  // namespace lgp::difftest
