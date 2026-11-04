// Dispatch table of ADR-0004 (dispatch.hpp) and the error path for unknown
// indirect targets, without lifted code: hand-written functions in the style
// the code generator registers. Runs natively and in Node.
#include "lgp/lifted/cpu_state.hpp"
#include "lgp/runtime/guest.hpp"

#include "check.hpp"

using namespace lgp::rt;

namespace {

void fn_a(lgp::CpuState* ctx) { ctx->x[0] = 0xA; }
void fn_b(lgp::CpuState* ctx) { ctx->x[0] = 0xB; }
void fn_c(lgp::CpuState* ctx) { ctx->x[0] = 0xC; }
void fn_resolved(lgp::CpuState* ctx) { ctx->x[0] = 0x5E; }

lgp::LiftedFn resolver(uint64_t va) { return va == 0x0900'0000 ? &fn_resolved : nullptr; }

void caller(lgp::CpuState* ctx) {
    ctx->pc = 0x0800'0040;
    LGP_CALL_INDIRECT(ctx->x[16]);
    if (ctx->halt) return;
    ctx->x[1] = 1;  // reached only after a successful call
}

void test_table() {
    reset_dispatch();
    CHECK(table_lookup(0x0800'0000) == nullptr);
    const lgp::LiftedEntry first[] = {{0x0800'1000, &fn_a}, {0x0800'1010, &fn_b}};
    register_functions(first, 2);
    CHECK_EQ(dispatch_info().functions, 2u);
    CHECK_EQ(dispatch_info().base, 0x0800'1000u);
    CHECK(table_lookup(0x0800'1000) == &fn_a);
    CHECK(table_lookup(0x0800'1010) == &fn_b);
    CHECK(table_lookup(0x0800'1004) == nullptr);  // inside the range, no function
    CHECK(table_lookup(0x0800'1002) == nullptr);  // misaligned
    CHECK(table_lookup(0x0800'0ffc) == nullptr);  // below
    CHECK(table_lookup(0x0800'1014) == nullptr);  // above
    CHECK(table_lookup(0) == nullptr);
    CHECK(table_lookup(~uint64_t{0} & ~uint64_t{3}) == nullptr);

    // A second module below and above: the range grows, old entries stay.
    const lgp::LiftedEntry second[] = {{0x0700'0000, &fn_c}, {0x0800'2000, &fn_c}, {0x0800'1010, &fn_a}};
    register_functions(second, 3);
    CHECK_EQ(dispatch_info().functions, 4u);  // 0x08001010 replaced, not added
    CHECK_EQ(dispatch_info().base, 0x0700'0000u);
    CHECK(table_lookup(0x0800'1000) == &fn_a);
    CHECK(table_lookup(0x0800'1010) == &fn_a);
    CHECK(table_lookup(0x0700'0000) == &fn_c);
    CHECK(table_lookup(0x0800'2000) == &fn_c);
}

void test_resolver_and_error_path() {
    reset_dispatch();
    const lgp::LiftedEntry entries[] = {{0x0800'1000, &fn_a}};
    register_functions(entries, 1);
    set_indirect_resolver(&resolver);
    CHECK(lookup_function(0x0800'1000) == &fn_a);
    CHECK(lookup_function(0x0900'0000) == &fn_resolved);
    CHECK(lookup_function(0x0900'0004) == nullptr);

    lgp::CpuState cpu{};
    cpu.x[16] = 0x0900'0000;
    caller(&cpu);
    CHECK_EQ(cpu.halt, 0u);
    CHECK_EQ(cpu.x[0], 0x5Eu);
    CHECK_EQ(cpu.x[1], 1u);
    CHECK(unresolved_targets().empty());

    // Unknown target: halt with the target, recorded once per address.
    for (int i = 0; i < 3; ++i) {
        lgp::CpuState bad{};
        bad.x[16] = 0x0800'1004;
        caller(&bad);
        CHECK_EQ(bad.halt, static_cast<uint32_t>(lgp::kHaltBadJump));
        CHECK_EQ(bad.halt_info, 0x0800'1004u);
        CHECK_EQ(bad.pc, 0x0800'1004u);
        CHECK_EQ(bad.x[1], 0u);
    }
    const auto un = unresolved_targets();
    CHECK_EQ(un.size(), 1u);
    if (!un.empty()) {
        CHECK_EQ(un[0].target, 0x0800'1004u);
        CHECK_EQ(un[0].from_pc, 0x0800'0040u);
        CHECK_EQ(un[0].count, 3u);
    }
    const std::string cfg = unresolved_config();
    CHECK(cfg.rfind("function 0x08001004 ", 0) == 0);
    set_indirect_resolver(nullptr);
    reset_dispatch();
    CHECK(unresolved_targets().empty());
}

}  // namespace

int main() {
    RUN(test_table);
    RUN(test_resolver_and_error_path);
    return finish();
}
