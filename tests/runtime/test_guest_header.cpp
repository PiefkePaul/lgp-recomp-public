// guest.hpp (P2.6) against the lifter's CpuState: hand-written functions in
// the style of lifted code use the macros exactly as the lifter emits them.
// Built only when recomp/lifter (cpu_state.hpp) is present.
#include "lgp/lifted/cpu_state.hpp"
#include "lgp/runtime/guest.hpp"

#include "check.hpp"

using namespace lgp::rt;

namespace {

constexpr uint64_t kMiB = 0x10'0000;

#define LGP_HALT_CHECK() \
    do {                 \
        if (ctx->halt)   \
            return;      \
    } while (0)

// x0 = *(u64*)x1 + 1; *(u64*)x1 = x0; then SetHeapSize(x2) and return.
void fn_load_store_svc(lgp::CpuState* ctx) {
    ctx->x[0] = lgp_mem_read64(ctx->x[1]) + 1;
    lgp_mem_write64(ctx->x[1], ctx->x[0]);
    ctx->x[1] = ctx->x[2];
    ctx->pc = 0x0800'0010;
    LGP_SVC(1);
    LGP_HALT_CHECK();
}

void fn_exit(lgp::CpuState* ctx) {
    ctx->pc = 0x0800'0020;
    LGP_SVC(7);
    LGP_HALT_CHECK();
    ctx->x[5] = 99;  // not reached
}

void fn_target(lgp::CpuState* ctx) { ctx->x[7] = 0x7777; }

lgp::LiftedFn resolve(uint64_t va) { return va == 0x0800'0100 ? &fn_target : nullptr; }

void fn_indirect(lgp::CpuState* ctx) {
    LGP_CALL_INDIRECT(ctx->x[16]);
    LGP_HALT_CHECK();
}

void fn_sysregs(lgp::CpuState* ctx) {
    ctx->x[0] = static_cast<uint64_t>(LGP_READ_SYSREG((3u << 14) | (3u << 11) | (14u << 7) | 0u));  // CNTFRQ_EL0
    ctx->x[1] = static_cast<uint64_t>(LGP_READ_SYSREG((3u << 14) | (3u << 11) | (14u << 7) | 1u));  // CNTPCT_EL0
    LGP_BARRIER();
    LGP_YIELD();
    ctx->x[2] = static_cast<uint64_t>(LGP_READ_SYSREG((3u << 14) | (3u << 11) | (14u << 7) | 1u));
}

void fn_cas(lgp::CpuState* ctx) {
    ctx->x[0] = lgp_mem_cas32(ctx->x[1], 0, 5) ? 0 : 1;
    ctx->x[2] = lgp_mem_cas64(ctx->x[1] + 8, 0, 6) ? 0 : 1;
    ctx->x[3] = lgp_mem_cas128(ctx->x[1] + 16, 0, 0, 7, 8) ? 0 : 1;
}

void fn_brk(lgp::CpuState* ctx) {
    LGP_BRK(0x4C47);
    LGP_HALT_CHECK();
}

void test_lifted_style_calls() {
    ProcessConfig cfg;
    cfg.total_memory_size = 8 * kMiB;
    cfg.custom_layout = true;
    cfg.layout = Layout::packed(0x0800'0000, 1 * kMiB, 1 * kMiB, 1 * kMiB, 1 * kMiB, 8 * kMiB);
    Kernel k(cfg);
    set_active_kernel(&k);
    const uint64_t data = cfg.layout.misc.base;
    CHECK(k.map_owned(data, kPageSize, MemState::CodeData, kPermRW).ok());

    lgp::CpuState cpu{};
    cpu.x[1] = data;
    cpu.x[2] = 2 * kMiB;
    lgp_mem_write64(data, 41);
    run_lifted(&cpu, &fn_load_store_svc);
    CHECK_EQ(cpu.halt, lgp::kHaltNone);
    CHECK_EQ(lgp_mem_read64(data), 42u);
    CHECK_EQ(cpu.x[0], 0u);  // svc result
    CHECK_EQ(cpu.x[1], cfg.layout.heap.base);
    CHECK_EQ(k.heap_size(), 2 * kMiB);

    cpu.x[1] = data + 0x100;
    run_lifted(&cpu, &fn_cas);
    CHECK_EQ(cpu.x[0] | cpu.x[2] | cpu.x[3], 0u);
    CHECK_EQ(lgp_mem_read32(data + 0x100), 5u);
    CHECK_EQ(lgp_mem_read64(data + 0x118), 8u);

    run_lifted(&cpu, &fn_sysregs);
    CHECK_EQ(cpu.x[0], 19'200'000u);
    CHECK(cpu.x[2] >= cpu.x[1]);

    set_indirect_resolver(&resolve);
    cpu.x[16] = 0x0800'0100;
    run_lifted(&cpu, &fn_indirect);
    CHECK_EQ(cpu.x[7], 0x7777u);
    cpu.x[16] = 0x0800'0200;
    run_lifted(&cpu, &fn_indirect);
    CHECK_EQ(cpu.halt, lgp::kHaltBadJump);
    CHECK_EQ(cpu.halt_info, 0x0800'0200u);
    set_indirect_resolver(nullptr);

    cpu.halt = lgp::kHaltNone;
    run_lifted(&cpu, &fn_brk);
    CHECK_EQ(cpu.halt, lgp::kHaltBrk);
    CHECK_EQ(cpu.halt_info, 0x4C47u);

    cpu.halt = lgp::kHaltNone;
    cpu.x[5] = 0;
    run_lifted(&cpu, &fn_exit);
    CHECK_EQ(cpu.halt, lgp::kHaltExit);
    CHECK_EQ(cpu.halt_info, 7u);
    CHECK_EQ(cpu.x[5], 0u);

#if LGP_GUEST_MEM_CHECKED
    cpu.halt = lgp::kHaltNone;
    cpu.x[1] = cfg.layout.stack.base;  // unmapped
    cpu.x[2] = 0;
    cpu.x[0] = 0x1234;
    run_lifted(&cpu, &fn_load_store_svc);
    CHECK_EQ(cpu.halt, lgp::kHaltMemFault);
    CHECK_EQ(cpu.halt_info, cfg.layout.stack.base);
    CHECK_EQ(cpu.x[0], 0x1234u);  // the faulting load did not complete
#endif
    set_active_kernel(nullptr);
}

}  // namespace

int main() {
    RUN(test_lifted_style_calls);
    return finish();
}
