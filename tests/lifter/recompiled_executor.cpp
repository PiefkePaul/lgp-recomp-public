#include "recompiled_executor.hpp"

#include <cstdio>
#include <stdexcept>
#include <string>

#include "guest_stub.hpp"

namespace lgp_test {

thread_local Guest* g_guest = nullptr;

namespace {

using lgp::difftest::GuestState;
using lgp::difftest::Outcome;
using lgp::difftest::RunResult;
using lgp::difftest::SvcRecord;
using lgp::difftest::TestCase;

struct RunContext {
    const TestCase* tc = nullptr;
    const CaseDef* def = nullptr;
    RunResult* result = nullptr;
};
thread_local RunContext* g_run = nullptr;

std::string hex(uint64_t v) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "0x%llx", static_cast<unsigned long long>(v));
    return buf;
}

void load_state(lgp::CpuState& s, const GuestState& g) {
    s = {};
    for (size_t i = 0; i < 31; ++i)
        s.x[i] = g.x[i];
    s.sp = g.sp;
    s.pc = g.pc;
    lgp::unpack_nzcv(s, g.nzcv);
    for (size_t i = 0; i < 32; ++i)
        s.vreg[i] = {g.v[i].lo, g.v[i].hi};
    s.fpcr = g.fpcr;
    s.fpsr = g.fpsr;
    s.tpidr_el0 = g.tpidr_el0;
    s.tpidrro_el0 = g.tpidrro_el0;
}

GuestState save_state(const lgp::CpuState& s) {
    GuestState g;
    for (size_t i = 0; i < 31; ++i)
        g.x[i] = s.x[i];
    g.sp = s.sp;
    g.pc = s.pc;
    g.nzcv = lgp::pack_nzcv(s);
    for (size_t i = 0; i < 32; ++i)
        g.v[i] = {s.vreg[i].lo, s.vreg[i].hi};
    g.fpcr = s.fpcr;
    g.fpsr = s.fpsr;
    g.tpidr_el0 = s.tpidr_el0;
    g.tpidrro_el0 = s.tpidrro_el0;
    return g;
}

}  // namespace

void svc(lgp::CpuState* ctx, uint32_t imm) {
    SvcRecord rec;
    rec.imm = imm;
    for (size_t i = 0; i < rec.args.size(); ++i)
        rec.args[i] = ctx->x[i];
    rec.results = rec.args;
    if (imm == lgp::difftest::kSvcExitProcess) {
        g_run->result->svcs.push_back(rec);
        ctx->halt = lgp::kHaltExit;
        return;
    }
    if (g_run->tc->svc)
        g_run->tc->svc(imm, rec.results);
    else
        rec.results[0] = 0;
    for (size_t i = 0; i < rec.results.size(); ++i)
        ctx->x[i] = rec.results[i];
    g_run->result->svcs.push_back(rec);
}

void call_indirect(lgp::CpuState* ctx, uint64_t va) {
    lgp::LiftedFn fn = g_run->def->lookup(va);
    if (!fn) {
        ctx->halt = lgp::kHaltBadJump;
        ctx->halt_info = va;
        return;
    }
    fn(ctx);
}

RunResult RecompiledExecutor::run(const TestCase& tc) {
    if (!case_) throw std::logic_error("RecompiledExecutor: no case set");

    RunResult result;
    std::vector<lgp::difftest::MemoryRegion> mem = tc.memory;  // private, mutable copy
    Guest guest;
    if (mem.size() > std::size(guest.regions)) throw std::invalid_argument(tc.name + ": too many memory regions");
    for (auto& r : mem) {
        guest.regions[guest.region_count++] = {r.base, r.bytes.size(), r.bytes.data(),
                                               (r.perms & lgp::difftest::kWrite) != 0};
    }

    lgp::CpuState cpu;
    load_state(cpu, tc.initial);
    guest.cpu = &cpu;

    RunContext run{&tc, case_, &result};
    g_guest = &guest;
    g_run = &run;
    try {
        case_->entry(&cpu);
    } catch (const MemFault& f) {
        cpu.halt = lgp::kHaltMemFault;
        cpu.halt_info = f.va;
    }
    g_guest = nullptr;
    g_run = nullptr;

    switch (cpu.halt) {
    case lgp::kHaltBrk:
        if (cpu.halt_info == lgp::difftest::kSentinelBrk) {
            result.outcome = Outcome::Sentinel;
        } else {
            result.outcome = Outcome::Fault;
            result.detail = "BRK #" + hex(cpu.halt_info) + " at " + hex(cpu.pc);
        }
        break;
    case lgp::kHaltExit: result.outcome = Outcome::ExitProcess; break;
    case lgp::kHaltUndefined:
        result.outcome = Outcome::Fault;
        result.detail = "undefined instruction " + hex(cpu.halt_info) + " at " + hex(cpu.pc);
        break;
    case lgp::kHaltBadJump:
        result.outcome = Outcome::Fault;
        result.detail = "indirect branch to unknown address " + hex(cpu.halt_info);
        break;
    case lgp::kHaltLimit:
        result.outcome = Outcome::InstructionLimit;
        result.detail = "loop budget exhausted";
        break;
    case lgp::kHaltMemFault:
        result.outcome = Outcome::Fault;
        result.detail = "memory fault at " + hex(cpu.halt_info) + ", pc " + hex(cpu.pc);
        break;
    default:
        result.outcome = Outcome::Fault;
        result.detail = "lifted code returned to the harness without a stop";
        break;
    }
    result.final = save_state(cpu);
    for (auto& r : mem)
        if (r.perms & lgp::difftest::kWrite) result.memory.push_back(std::move(r));
    return result;
}

}  // namespace lgp_test
