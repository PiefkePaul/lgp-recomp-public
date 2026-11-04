#include "unicorn_executor.h"

#include <unicorn/unicorn.h>

#include <cstdio>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>

namespace lgp::difftest {
namespace {

constexpr uint64_t kPage = 0x1000;

// QEMU exception numbers as Unicorn passes them to UC_HOOK_INTR (target/arm/cpu.h).
constexpr uint32_t kExcpUdef = 1;
constexpr uint32_t kExcpSwi = 2;
constexpr uint32_t kExcpBkpt = 7;

struct UcCloser {
    void operator()(uc_engine* uc) const { uc_close(uc); }
};
using UcPtr = std::unique_ptr<uc_engine, UcCloser>;

void check(uc_err err, const char* what) {
    if (err != UC_ERR_OK)
        throw std::runtime_error(std::string(what) + ": " + uc_strerror(err));
}

uint64_t round_up(uint64_t v) { return (v + kPage - 1) & ~(kPage - 1); }

uint64_t reg_read(uc_engine* uc, int reg) {
    uint64_t v = 0;
    check(uc_reg_read(uc, reg, &v), "uc_reg_read");
    return v;
}

void reg_write(uc_engine* uc, int reg, uint64_t v) { check(uc_reg_write(uc, reg, &v), "uc_reg_write"); }

// Hook callbacks run inside Unicorn's C code and must not throw.
uint64_t peek(uc_engine* uc, int reg) {
    uint64_t v = 0;
    uc_reg_read(uc, reg, &v);
    return v;
}

void poke(uc_engine* uc, int reg, uint64_t v) { uc_reg_write(uc, reg, &v); }

int x_reg(size_t i) {
    if (i == 29) return UC_ARM64_REG_X29;
    if (i == 30) return UC_ARM64_REG_X30;
    return UC_ARM64_REG_X0 + static_cast<int>(i);
}

void write_state(uc_engine* uc, const GuestState& s) {
    for (size_t i = 0; i < s.x.size(); ++i)
        reg_write(uc, x_reg(i), s.x[i]);
    reg_write(uc, UC_ARM64_REG_SP, s.sp);
    reg_write(uc, UC_ARM64_REG_PC, s.pc);
    reg_write(uc, UC_ARM64_REG_NZCV, s.nzcv);
    for (size_t i = 0; i < s.v.size(); ++i) {
        uint64_t q[2] = {s.v[i].lo, s.v[i].hi};
        check(uc_reg_write(uc, UC_ARM64_REG_Q0 + static_cast<int>(i), q), "uc_reg_write(q)");
    }
    reg_write(uc, UC_ARM64_REG_FPCR, s.fpcr);
    reg_write(uc, UC_ARM64_REG_FPSR, s.fpsr);
    reg_write(uc, UC_ARM64_REG_TPIDR_EL0, s.tpidr_el0);
    reg_write(uc, UC_ARM64_REG_TPIDRRO_EL0, s.tpidrro_el0);
}

GuestState read_state(uc_engine* uc) {
    GuestState s;
    for (size_t i = 0; i < s.x.size(); ++i)
        s.x[i] = reg_read(uc, x_reg(i));
    s.sp = reg_read(uc, UC_ARM64_REG_SP);
    s.pc = reg_read(uc, UC_ARM64_REG_PC);
    s.nzcv = static_cast<uint32_t>(reg_read(uc, UC_ARM64_REG_NZCV));
    for (size_t i = 0; i < s.v.size(); ++i) {
        uint64_t q[2] = {};
        check(uc_reg_read(uc, UC_ARM64_REG_Q0 + static_cast<int>(i), q), "uc_reg_read(q)");
        s.v[i] = {q[0], q[1]};
    }
    s.fpcr = static_cast<uint32_t>(reg_read(uc, UC_ARM64_REG_FPCR));
    s.fpsr = static_cast<uint32_t>(reg_read(uc, UC_ARM64_REG_FPSR));
    s.tpidr_el0 = reg_read(uc, UC_ARM64_REG_TPIDR_EL0);
    s.tpidrro_el0 = reg_read(uc, UC_ARM64_REG_TPIDRRO_EL0);
    return s;
}

std::string hex(uint64_t v) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "0x%llx", static_cast<unsigned long long>(v));
    return buf;
}

struct HookContext {
    const TestCase* tc = nullptr;
    RunResult* result = nullptr;
    bool stopped = false;
};

bool read_insn(uc_engine* uc, uint64_t addr, uint32_t& insn) {
    return uc_mem_read(uc, addr, &insn, sizeof(insn)) == UC_ERR_OK;
}

void stop(uc_engine* uc, HookContext& ctx, Outcome outcome, std::string detail = {}) {
    ctx.stopped = true;
    ctx.result->outcome = outcome;
    ctx.result->detail = std::move(detail);
    uc_emu_stop(uc);
}

void on_interrupt(uc_engine* uc, uint32_t intno, void* user) {
    auto& ctx = *static_cast<HookContext*>(user);
    const uint64_t pc = peek(uc, UC_ARM64_REG_PC);
    uint32_t insn = 0;

    if (intno == kExcpSwi) {
        // PC already points past the SVC (its preferred return address).
        if (!read_insn(uc, pc - 4, insn) || (insn & 0xFFE0001Fu) != 0xD4000001u)
            return stop(uc, ctx, Outcome::Fault, "SVC exception without SVC instruction at " + hex(pc - 4));
        SvcRecord rec;
        rec.imm = (insn >> 5) & 0xFFFF;
        for (size_t i = 0; i < rec.args.size(); ++i)
            rec.args[i] = peek(uc, UC_ARM64_REG_X0 + static_cast<int>(i));
        rec.results = rec.args;
        if (rec.imm == kSvcExitProcess) {
            ctx.result->svcs.push_back(rec);
            return stop(uc, ctx, Outcome::ExitProcess);
        }
        if (ctx.tc->svc) {
            try {
                ctx.tc->svc(rec.imm, rec.results);
            } catch (const std::exception& e) {
                return stop(uc, ctx, Outcome::Fault, std::string("SVC handler threw: ") + e.what());
            }
        } else {
            rec.results[0] = 0;
        }
        for (size_t i = 0; i < rec.results.size(); ++i)
            poke(uc, UC_ARM64_REG_X0 + static_cast<int>(i), rec.results[i]);
        ctx.result->svcs.push_back(rec);
        return;
    }

    if (intno == kExcpBkpt) {
        // Unicorn leaves PC on the BRK itself, which matches ELR on hardware.
        if (!read_insn(uc, pc, insn) || (insn & 0xFFE0001Fu) != 0xD4200000u)
            return stop(uc, ctx, Outcome::Fault, "BRK exception without BRK instruction at " + hex(pc));
        const uint32_t imm = (insn >> 5) & 0xFFFF;
        if (imm == kSentinelBrk)
            return stop(uc, ctx, Outcome::Sentinel);
        return stop(uc, ctx, Outcome::Fault, "BRK #" + hex(imm) + " at " + hex(pc));
    }

    if (intno == kExcpUdef)
        return stop(uc, ctx, Outcome::Fault, "undefined instruction at " + hex(pc));
    stop(uc, ctx, Outcome::Fault, "exception " + std::to_string(intno) + " at " + hex(pc));
}

}  // namespace

RunResult UnicornExecutor::run(const TestCase& tc) {
    uc_engine* raw = nullptr;
    check(uc_open(UC_ARCH_ARM64, UC_MODE_ARM, &raw), "uc_open");
    UcPtr uc(raw);
    // The Switch has Cortex-A57 cores (ARMv8.0-A); must be set before anything else.
    check(uc_ctl_set_cpu_model(uc.get(), UC_CPU_ARM64_A57), "uc_ctl_set_cpu_model");

    for (const MemoryRegion& r : tc.memory) {
        if (r.base % kPage != 0)
            throw std::invalid_argument(tc.name + ": region base " + hex(r.base) + " not 4 KiB aligned");
        uint32_t perms = 0;
        if (r.perms & kRead) perms |= UC_PROT_READ;
        if (r.perms & kWrite) perms |= UC_PROT_WRITE;
        if (r.perms & kExec) perms |= UC_PROT_EXEC;
        check(uc_mem_map(uc.get(), r.base, round_up(r.bytes.size()), perms), "uc_mem_map");
        if (!r.bytes.empty())
            check(uc_mem_write(uc.get(), r.base, r.bytes.data(), r.bytes.size()), "uc_mem_write");
    }
    write_state(uc.get(), tc.initial);

    RunResult result;
    HookContext ctx{&tc, &result};
    uc_hook hook = 0;
    check(uc_hook_add(uc.get(), &hook, UC_HOOK_INTR, reinterpret_cast<void*>(&on_interrupt), &ctx, 1, 0),
          "uc_hook_add");

    const uint64_t until = tc.stop_pc != 0 ? tc.stop_pc : std::numeric_limits<uint64_t>::max();
    const uc_err err = uc_emu_start(uc.get(), tc.initial.pc, until, 0, static_cast<size_t>(tc.max_instructions));
    if (!ctx.stopped) {
        if (err == UC_ERR_OK && tc.stop_pc != 0 && reg_read(uc.get(), UC_ARM64_REG_PC) == tc.stop_pc) {
            result.outcome = Outcome::StopAddress;
        } else if (err == UC_ERR_OK) {
            result.outcome = Outcome::InstructionLimit;
        } else {
            result.outcome = Outcome::Fault;
            result.detail = std::string(uc_strerror(err)) + " at " + hex(reg_read(uc.get(), UC_ARM64_REG_PC));
        }
    }

    result.final = read_state(uc.get());
    for (const MemoryRegion& r : tc.memory) {
        if (!(r.perms & kWrite))
            continue;
        MemoryRegion out{r.base, std::vector<uint8_t>(r.bytes.size()), r.perms};
        if (!out.bytes.empty())
            check(uc_mem_read(uc.get(), r.base, out.bytes.data(), out.bytes.size()), "uc_mem_read");
        result.memory.push_back(std::move(out));
    }
    return result;
}

}  // namespace lgp::difftest
