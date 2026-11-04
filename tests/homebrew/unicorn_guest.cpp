#include "unicorn_guest.hpp"

#include <unicorn/unicorn.h>

#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "lgp/runtime/guest.hpp"
#include "lgp/runtime/kernel.hpp"
#include "lgp/runtime/log.hpp"

namespace lgp_fixture {
namespace {

using lgp::CpuState;
namespace rt = lgp::rt;

// QEMU exception numbers as Unicorn reports them to UC_HOOK_INTR.
constexpr uint32_t kExcpUdef = 1;
constexpr uint32_t kExcpSwi = 2;
constexpr uint32_t kExcpBkpt = 7;

// Instructions per uc_emu_start; between slices the thread polls the
// scheduler like lifted code does on loop back-edges (LGP_BACKEDGE).
constexpr uint64_t kSlice = 1'000'000;

struct UcCloser {
    void operator()(uc_engine* uc) const { uc_close(uc); }
};

void check(uc_err err, const char* what) {
    if (err != UC_ERR_OK) throw std::runtime_error(std::string(what) + ": " + uc_strerror(err));
}

int x_reg(int i) {
    if (i == 29) return UC_ARM64_REG_X29;
    if (i == 30) return UC_ARM64_REG_X30;
    return UC_ARM64_REG_X0 + i;
}

uint64_t rd(uc_engine* uc, int reg) {
    uint64_t v = 0;
    check(uc_reg_read(uc, reg, &v), "uc_reg_read");
    return v;
}

void wr(uc_engine* uc, int reg, uint64_t v) { check(uc_reg_write(uc, reg, &v), "uc_reg_write"); }

void load_state(uc_engine* uc, const CpuState& s) {
    for (int i = 0; i < 31; ++i) wr(uc, x_reg(i), s.x[i]);
    wr(uc, UC_ARM64_REG_SP, s.sp);
    wr(uc, UC_ARM64_REG_PC, s.pc);
    wr(uc, UC_ARM64_REG_NZCV, lgp::pack_nzcv(s));
    for (int i = 0; i < 32; ++i) {
        uint64_t q[2] = {s.vreg[i].lo, s.vreg[i].hi};
        check(uc_reg_write(uc, UC_ARM64_REG_Q0 + i, q), "uc_reg_write(q)");
    }
    wr(uc, UC_ARM64_REG_FPCR, s.fpcr);
    wr(uc, UC_ARM64_REG_FPSR, s.fpsr);
    wr(uc, UC_ARM64_REG_TPIDR_EL0, s.tpidr_el0);
    wr(uc, UC_ARM64_REG_TPIDRRO_EL0, s.tpidrro_el0);
}

void save_state(uc_engine* uc, CpuState& s) {
    for (int i = 0; i < 31; ++i) s.x[i] = rd(uc, x_reg(i));
    s.sp = rd(uc, UC_ARM64_REG_SP);
    s.pc = rd(uc, UC_ARM64_REG_PC);
    lgp::unpack_nzcv(s, rd(uc, UC_ARM64_REG_NZCV));
    for (int i = 0; i < 32; ++i) {
        uint64_t q[2] = {};
        check(uc_reg_read(uc, UC_ARM64_REG_Q0 + i, q), "uc_reg_read(q)");
        s.vreg[i] = {q[0], q[1]};
    }
    s.fpcr = static_cast<uint32_t>(rd(uc, UC_ARM64_REG_FPCR));
    s.fpsr = static_cast<uint32_t>(rd(uc, UC_ARM64_REG_FPSR));
    s.tpidr_el0 = rd(uc, UC_ARM64_REG_TPIDR_EL0);
}

struct Stop {
    bool svc = false;
    uint32_t svc_imm = 0;
    bool exception = false;
    uint32_t intno = 0;
    bool fault = false;
    uint64_t fault_addr = 0;
};

void on_interrupt(uc_engine* uc, uint32_t intno, void* user) {
    auto& stop = *static_cast<Stop*>(user);
    if (intno == kExcpSwi) {
        uint64_t pc = 0;
        uc_reg_read(uc, UC_ARM64_REG_PC, &pc);  // already past the SVC
        uint32_t insn = 0;
        uc_mem_read(uc, pc - 4, &insn, sizeof(insn));
        stop.svc = true;
        stop.svc_imm = (insn >> 5) & 0xFFFF;
    } else {
        stop.exception = true;
        stop.intno = intno;
    }
    uc_emu_stop(uc);
}

bool on_invalid(uc_engine*, uc_mem_type, uint64_t address, int, int64_t, void* user) {
    auto& stop = *static_cast<Stop*>(user);
    stop.fault = true;
    stop.fault_addr = address;
    return false;
}

// Mirrors the kernel's address space into this engine: every block with
// access becomes a region backed by the same host memory, so all threads (and
// the recompiled code in the other run) see one guest memory.
struct Region {
    uint64_t base, size;
    uint32_t prot;
    bool operator==(const Region&) const = default;
};
using Map = std::vector<Region>;

// Remapping flushes Unicorn's translation cache, so it only happens when the
// address space really changed (most svcs, e.g. mutex/condvar, leave it alone).
Map sync_maps(uc_engine* uc, const Map& old) {
    rt::Kernel& k = rt::active_kernel();
    std::lock_guard lock(k.memory_mutex());
    Map now;
    rt::GuestMemory& mem = k.memory();
    for (uint64_t addr = mem.layout().start(); addr < mem.layout().end();) {
        const rt::BlockInfo b = mem.query(addr);
        addr = b.addr + b.size;
        if (b.state == rt::MemState::Free || b.perm == rt::kPermNone) continue;
        uint32_t prot = 0;
        if (b.perm & rt::kPermR) prot |= UC_PROT_READ;
        if (b.perm & rt::kPermW) prot |= UC_PROT_WRITE;
        if (b.perm & rt::kPermX) prot |= UC_PROT_EXEC;
        now.push_back({b.addr, b.size, prot});
    }
    if (now == old) return now;
    for (const Region& r : old) check(uc_mem_unmap(uc, r.base, r.size), "uc_mem_unmap");
    for (const Region& r : now)
        check(uc_mem_map_ptr(uc, r.base, r.size, r.prot, mem.host_ptr(r.base, r.size)), "uc_mem_map_ptr");
    return now;
}

void set_halt(CpuState* ctx, uint32_t reason, uint64_t info) {
    if (ctx->halt == lgp::kHaltNone) {
        ctx->halt = reason;
        ctx->halt_info = info;
    }
}

}  // namespace

void run_under_unicorn(CpuState* ctx) {
    uc_engine* raw = nullptr;
    check(uc_open(UC_ARCH_ARM64, UC_MODE_ARM, &raw), "uc_open");
    std::unique_ptr<uc_engine, UcCloser> uc(raw);
    check(uc_ctl_set_cpu_model(uc.get(), UC_CPU_ARM64_A57), "uc_ctl_set_cpu_model");

    Stop stop;
    uc_hook intr = 0, invalid = 0;
    check(uc_hook_add(uc.get(), &intr, UC_HOOK_INTR, reinterpret_cast<void*>(&on_interrupt), &stop, 1, 0),
          "uc_hook_add(intr)");
    check(uc_hook_add(uc.get(), &invalid, UC_HOOK_MEM_INVALID, reinterpret_cast<void*>(&on_invalid), &stop, 1, 0),
          "uc_hook_add(invalid)");
    load_state(uc.get(), *ctx);
    Map maps = sync_maps(uc.get(), {});
    bool retried_fault = false;

    for (;;) {
        stop = Stop{};
        const uint64_t pc = rd(uc.get(), UC_ARM64_REG_PC);
        const uc_err err = uc_emu_start(uc.get(), pc, ~uint64_t{0}, 0, kSlice);
        save_state(uc.get(), *ctx);
        if (stop.svc) {
            retried_fault = false;
            const rt::SvcOutcome outcome = rt::active_kernel().svc(stop.svc_imm, ctx->x);
            for (int i = 0; i < 8; ++i) wr(uc.get(), x_reg(i), ctx->x[i]);
            if (outcome == rt::SvcOutcome::Continue) {
                maps = sync_maps(uc.get(), maps);  // the svc may have changed the address space
                continue;
            }
            if (outcome == rt::SvcOutcome::Unimplemented)
                set_halt(ctx, lgp::kHaltUndefined, 0xD4000001u | (stop.svc_imm << 5));
            else
                set_halt(ctx, lgp::kHaltExit, stop.svc_imm);
            return;
        }
        if (stop.exception) {
            uint32_t insn = 0;
            uc_mem_read(uc.get(), ctx->pc, &insn, sizeof(insn));
            if (stop.intno == kExcpBkpt)
                set_halt(ctx, lgp::kHaltBrk, (insn >> 5) & 0xFFFF);
            else if (stop.intno == kExcpUdef)
                set_halt(ctx, lgp::kHaltUndefined, insn);
            else
                set_halt(ctx, lgp::kHaltUndefined, 0x8000'0000u | stop.intno);
            return;
        }
        if (stop.fault || err != UC_ERR_OK) {
            // Another thread may have mapped memory since our last svc: look
            // at the address space once more before calling it a fault.
            if (!retried_fault) {
                retried_fault = true;
                maps = sync_maps(uc.get(), maps);
                continue;
            }
            rt::log(rt::LogLevel::Error, "unicorn: %s at pc 0x%llx, address 0x%llx", uc_strerror(err),
                    static_cast<unsigned long long>(ctx->pc), static_cast<unsigned long long>(stop.fault_addr));
            set_halt(ctx, lgp::kHaltMemFault, stop.fault ? stop.fault_addr : ctx->pc);
            return;
        }
        // Slice used up: a scheduling point like a lifted loop back-edge.
        retried_fault = false;
        if (rt::backedge_poll(ctx)) return;
    }
}

}  // namespace lgp_fixture
