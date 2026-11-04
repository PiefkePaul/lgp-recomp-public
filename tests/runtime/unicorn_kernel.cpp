#include "unicorn_kernel.hpp"

#include <unicorn/unicorn.h>

#include <memory>
#include <stdexcept>
#include <vector>

namespace lgp_test {
namespace {

using lgp::rt::BlockInfo;
using lgp::rt::MemState;

constexpr uint32_t kExcpSwi = 2;  // QEMU exception number for SVC

struct UcCloser {
    void operator()(uc_engine* uc) const { uc_close(uc); }
};

void check(uc_err err, const char* what) {
    if (err != UC_ERR_OK)
        throw std::runtime_error(std::string(what) + ": " + uc_strerror(err));
}

int x_reg(size_t i) {
    if (i == 29) return UC_ARM64_REG_X29;
    if (i == 30) return UC_ARM64_REG_X30;
    return UC_ARM64_REG_X0 + static_cast<int>(i);
}

uint64_t rd(uc_engine* uc, int reg) {
    uint64_t v = 0;
    check(uc_reg_read(uc, reg, &v), "uc_reg_read");
    return v;
}

void wr(uc_engine* uc, int reg, uint64_t v) { check(uc_reg_write(uc, reg, &v), "uc_reg_write"); }

struct SvcStop {
    bool pending = false;
    uint32_t imm = 0;
    bool other_exception = false;
    uint32_t exception = 0;
};

void on_interrupt(uc_engine* uc, uint32_t intno, void* user) {
    auto& stop = *static_cast<SvcStop*>(user);
    if (intno == kExcpSwi) {
        uint64_t pc = 0;
        uc_reg_read(uc, UC_ARM64_REG_PC, &pc);  // already past the SVC
        uint32_t insn = 0;
        uc_mem_read(uc, pc - 4, &insn, sizeof(insn));
        stop.pending = true;
        stop.imm = (insn >> 5) & 0xFFFF;
    } else {
        stop.other_exception = true;
        stop.exception = intno;
    }
    uc_emu_stop(uc);
}

// Mirrors the kernel's view of the address space into Unicorn: every mapped
// block the guest may access becomes a Unicorn region backed by the same host
// memory, with the guest permissions.
void sync_maps(uc_engine* uc, lgp::rt::Kernel& kernel) {
    uc_mem_region* regions = nullptr;
    uint32_t count = 0;
    check(uc_mem_regions(uc, &regions, &count), "uc_mem_regions");
    std::vector<std::pair<uint64_t, uint64_t>> old;
    for (uint32_t i = 0; i < count; ++i)
        old.emplace_back(regions[i].begin, regions[i].end - regions[i].begin + 1);
    uc_free(regions);
    for (auto [base, size] : old)
        check(uc_mem_unmap(uc, base, size), "uc_mem_unmap");

    lgp::rt::GuestMemory& mem = kernel.memory();
    const uint64_t end = mem.layout().end();
    for (uint64_t addr = mem.layout().start(); addr < end;) {
        const BlockInfo b = mem.query(addr);
        addr = b.addr + b.size;
        if (b.state == MemState::Free || b.perm == lgp::rt::kPermNone)
            continue;
        uint32_t prot = 0;
        if (b.perm & lgp::rt::kPermR) prot |= UC_PROT_READ;
        if (b.perm & lgp::rt::kPermW) prot |= UC_PROT_WRITE;
        if (b.perm & lgp::rt::kPermX) prot |= UC_PROT_EXEC;
        check(uc_mem_map_ptr(uc, b.addr, b.size, prot, mem.host_ptr(b.addr, b.size)), "uc_mem_map_ptr");
    }
}

}  // namespace

const char* to_string(KernelRunOutcome o) {
    switch (o) {
    case KernelRunOutcome::ExitProcess: return "ExitProcess";
    case KernelRunOutcome::Break: return "Break";
    case KernelRunOutcome::Unimplemented: return "Unimplemented";
    case KernelRunOutcome::Fault: return "Fault";
    case KernelRunOutcome::InstructionLimit: return "InstructionLimit";
    }
    return "?";
}

KernelRun run_under_unicorn(lgp::rt::Kernel& kernel, uint64_t entry, uint64_t sp,
                            const std::array<uint64_t, 31>& initial_x, uint64_t max_instructions) {
    uc_engine* raw = nullptr;
    check(uc_open(UC_ARCH_ARM64, UC_MODE_ARM, &raw), "uc_open");
    std::unique_ptr<uc_engine, UcCloser> uc(raw);
    check(uc_ctl_set_cpu_model(uc.get(), UC_CPU_ARM64_A57), "uc_ctl_set_cpu_model");

    SvcStop stop;
    uc_hook hook = 0;
    check(uc_hook_add(uc.get(), &hook, UC_HOOK_INTR, reinterpret_cast<void*>(&on_interrupt), &stop, 1, 0),
          "uc_hook_add");
    for (size_t i = 0; i < initial_x.size(); ++i)
        wr(uc.get(), x_reg(i), initial_x[i]);
    wr(uc.get(), UC_ARM64_REG_SP, sp);
    sync_maps(uc.get(), kernel);

    KernelRun run;
    uint64_t pc = entry;
    for (;;) {
        stop = SvcStop{};
        const uc_err err = uc_emu_start(uc.get(), pc, ~uint64_t{0}, 0, static_cast<size_t>(max_instructions));
        pc = rd(uc.get(), UC_ARM64_REG_PC);
        if (stop.pending) {
            ++run.svc_count;
            uint64_t regs[8];
            for (int i = 0; i < 8; ++i)
                regs[i] = rd(uc.get(), UC_ARM64_REG_X0 + i);
            const lgp::rt::SvcOutcome outcome = kernel.svc(stop.imm, regs);
            for (int i = 0; i < 8; ++i)
                wr(uc.get(), UC_ARM64_REG_X0 + i, regs[i]);
            if (outcome == lgp::rt::SvcOutcome::Continue) {
                sync_maps(uc.get(), kernel);
                continue;
            }
            run.outcome = outcome == lgp::rt::SvcOutcome::ExitProcess ? KernelRunOutcome::ExitProcess
                          : outcome == lgp::rt::SvcOutcome::Break     ? KernelRunOutcome::Break
                                                                      : KernelRunOutcome::Unimplemented;
        } else if (stop.other_exception) {
            run.outcome = KernelRunOutcome::Fault;
            run.detail = "exception " + std::to_string(stop.exception);
        } else if (err != UC_ERR_OK) {
            run.outcome = KernelRunOutcome::Fault;
            run.detail = uc_strerror(err);
        } else {
            run.outcome = KernelRunOutcome::InstructionLimit;
        }
        break;
    }
    for (size_t i = 0; i < run.x.size(); ++i)
        run.x[i] = rd(uc.get(), x_reg(i));
    run.sp = rd(uc.get(), UC_ARM64_REG_SP);
    run.pc = pc;
    return run;
}

}  // namespace lgp_test
