// P2.5 acceptance: the test program (guest/program.cpp, built for AArch64 with
// the host clang and packed as NRO) runs under Unicorn and recompiled
// (lgp_lift -> lifted C++ -> runtime dispatch table) with the same result.
//
// usage: lgp_test_indirect_<variant> <program.nro> <program.elf>
//
// Built once per generated file. LGP_INDIRECT_CONFIG=1: the generator got the
// second entry of two_entries as `--function`, so mode 1 must also match
// Unicorn; =0: mode 1 must halt with kHaltBadJump at exactly that address and
// report it as a config line (ADR-0004 error path). LGP_INDIRECT_HOOKS=1: the
// generator also gave helper and op_add hook doors (lgp/runtime/hooks.hpp):
// without hooks, with counting hooks and with a hook that replaces helper by
// the same arithmetic the run matches Unicorn; a different replacement shows
// in the result. LGP_INDIRECT_HV=1 (macOS
// on Apple Silicon): the same runs also against the hypervisor oracle (P2.7).
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../runtime/check.hpp"
#include "lgp/lifted/lifted.hpp"
#include "lgp/recomp/cfg.hpp"
#include "lgp/recomp/elf_symbols.hpp"
#include "lgp/recomp/loader.hpp"
#include "lgp/runtime/dispatch.hpp"
#include "lgp/runtime/hooks.hpp"
#include "oracle.h"
#include "unicorn/unicorn_executor.h"
#if LGP_INDIRECT_HV
#include "hv/hv_machine.h"
#endif

extern const lgp::LiftedEntry lgp_lifted_entries[];
extern const size_t lgp_lifted_entries_count;

namespace {

using namespace lgp::rt;
namespace dt = lgp::difftest;
namespace rc = lgp::recomp;

constexpr uint64_t kMiB = 0x10'0000;
constexpr uint64_t kBase = 0x0800'0000;  // lgp_lift's default --base
constexpr uint64_t kStackSize = 256 * 1024;

struct Image {
    rc::LinkedImage linked;
    uint64_t entry = 0;
    uint64_t size = 0;        // whole image incl. bss
    uint64_t data_va = 0;     // first writable byte (.data); .data + .bss are compared
    uint64_t second_entry = 0;  // two_entries + 4
    uint64_t helper = 0, op_add = 0;  // hook doors with LGP_INDIRECT_HOOKS
};

Image load(const char* nro, const char* elf) {
    Image im;
    std::vector<rc::Module> mods;
    mods.push_back(rc::load_module_file(nro));
    rc::LinkOptions lo;
    lo.first_base = kBase;
    im.linked = rc::link(std::move(mods), lo);
    const rc::Module& m = im.linked.modules.front();
    im.entry = m.base + m.entry_offset;
    im.size = (m.image.size() + 0xFFF) & ~uint64_t{0xFFF};
    for (const rc::Segment& s : m.segments)
        if (s.kind == rc::SegmentKind::Data) im.data_va = m.base + s.offset;
    for (const rc::Symbol& s : rc::read_elf_symbols_file(elf).symbols)
        if (s.name == "two_entries") im.second_entry = m.base + s.value + 4;
        else if (s.name == "_Z6helperi") im.helper = m.base + s.value;
        else if (s.name == "_Z6op_addii") im.op_add = m.base + s.value;
    CHECK(im.data_va != 0);
    CHECK(im.second_entry != 0);
    return im;
}

uint8_t seg_perm(rc::SegmentKind k) {
    switch (k) {
    case rc::SegmentKind::Text: return kPermRX;
    case rc::SegmentKind::Ro: return kPermR;
    default: return kPermRW;
    }
}

struct Outcome {
    bool sentinel = false;
    uint32_t halt = 0;
    uint64_t halt_info = 0;
    uint64_t pc = 0, sp = 0;
    uint64_t x[31] = {};
    std::vector<uint8_t> data;  // .data + .bss after the run
};

// The oracle side: Unicorn everywhere, the hypervisor (real ARM64) on Apple
// Silicon (P2.7, LGP_INDIRECT_HV).
Outcome run_oracle(dt::IExecutor& ex, const Image& im, uint64_t mode) {
    const rc::Module& m = im.linked.modules.front();
    dt::TestCase tc;
    tc.name = "mode" + std::to_string(mode);
    for (const rc::Segment& s : m.segments) {
        if (s.kind == rc::SegmentKind::Bss) continue;  // part of the data region below
        dt::MemoryRegion r;
        r.base = m.base + s.offset;
        const uint64_t end = s.kind == rc::SegmentKind::Data ? m.image.size() : s.offset + s.size;
        r.bytes.assign(m.image.begin() + static_cast<std::ptrdiff_t>(s.offset),
                       m.image.begin() + static_cast<std::ptrdiff_t>(end));
        if (s.kind == rc::SegmentKind::Data) r.bytes.resize(im.size - s.offset);  // up to the page end, as compared
        r.perms = s.kind == rc::SegmentKind::Text ? (dt::kRead | dt::kExec)
                  : s.kind == rc::SegmentKind::Ro ? dt::kRead
                                                  : (dt::kRead | dt::kWrite);
        tc.memory.push_back(std::move(r));
    }
    const uint64_t stack_base = kBase + 0x1000'0000;
    tc.memory.push_back({stack_base, std::vector<uint8_t>(kStackSize), dt::kRead | dt::kWrite});
    tc.initial.x[0] = mode;
    tc.initial.sp = stack_base + kStackSize;
    tc.initial.pc = im.entry;
    tc.max_instructions = 200'000'000;

    const dt::RunResult rr = ex.run(tc);
    Outcome o;
    o.sentinel = rr.outcome == dt::Outcome::Sentinel;
    if (!o.sentinel) std::printf("  %s: %s %s\n", ex.name(), dt::to_string(rr.outcome), rr.detail.c_str());
    o.pc = rr.final.pc;
    o.sp = rr.final.sp;
    for (int i = 0; i < 31; ++i) o.x[i] = rr.final.x[i];
    for (const dt::MemoryRegion& r : rr.memory)
        if (r.base == im.data_va) o.data = r.bytes;
    // The oracle's sp: the guest stack is not part of the comparison, only sp.
    o.sp = o.sp - (stack_base + kStackSize);
    return o;
}

Outcome run_unicorn(const Image& im, uint64_t mode) {
    dt::UnicornExecutor uc;
    return run_oracle(uc, im, mode);
}

#if LGP_INDIRECT_HV
Outcome run_hv(const Image& im, uint64_t mode) {
    static dt::hv::HvExecutor hv;  // one VM per process
    return run_oracle(hv, im, mode);
}
#endif

Outcome run_recompiled(const Image& im, uint64_t mode) {
    const rc::Module& m = im.linked.modules.front();
    ProcessConfig cfg;
    cfg.total_memory_size = 16 * kMiB;
    cfg.custom_layout = true;
    cfg.layout = Layout::packed(kBase, 1 * kMiB, 1 * kMiB, 1 * kMiB, 1 * kMiB, 8 * kMiB);
    Kernel k(cfg);
    set_active_kernel(&k);
    const Layout& l = k.memory().layout();
    CHECK(im.size <= l.code.size);
    CHECK(k.map_owned(l.code.base, im.size, MemState::Code, kPermRW).ok());
    std::memcpy(k.memory().host_ptr(l.code.base, im.size), m.image.data(), m.image.size());
    for (const rc::Segment& s : m.segments) {
        if (s.size == 0) continue;
        const uint64_t lo = s.offset & ~uint64_t{0xFFF};
        const uint64_t hi = (s.offset + s.size + 0xFFF) & ~uint64_t{0xFFF};
        k.memory().set_perm(l.code.base + lo, hi - lo, seg_perm(s.kind));
    }
    CHECK(k.map_owned(l.stack.end() - kStackSize, kStackSize, MemState::Stack, kPermRW).ok());

    lgp::CpuState cpu{};
    cpu.x[0] = mode;
    cpu.sp = l.stack.end();
    cpu.pc = im.entry;
    const lgp::LiftedFn entry = lookup_function(im.entry);
    CHECK(entry != nullptr);
    if (entry != nullptr) run_lifted(&cpu, entry);

    Outcome o;
    o.sentinel = cpu.halt == lgp::kHaltBrk && cpu.halt_info == dt::kSentinelBrk;
    o.halt = cpu.halt;
    o.halt_info = cpu.halt_info;
    o.pc = cpu.pc;
    o.sp = cpu.sp - l.stack.end();
    for (int i = 0; i < 31; ++i) o.x[i] = cpu.x[i];
    const uint64_t n = im.size - (im.data_va - kBase);
    const uint8_t* p = k.memory().host_ptr(im.data_va, n);
    o.data.assign(p, p + n);
    set_active_kernel(nullptr);
    return o;
}

// Same end state: sentinel, pc, sp, x0 and the callee-saved x19-x29. The
// caller-saved registers may legitimately differ after the host longjmp
// (it restores the callee-saved state only, like any setjmp contract).
void compare(const Outcome& u, const Outcome& r) {
    CHECK(u.sentinel);
    CHECK(r.sentinel);
    if (!r.sentinel) std::printf("  recompiled: halt %u info 0x%" PRIx64 " pc 0x%" PRIx64 "\n", r.halt, r.halt_info, r.pc);
    CHECK_EQ(r.pc, u.pc);
    CHECK_EQ(r.sp, u.sp);
    CHECK_EQ(r.x[0], u.x[0]);
    for (int i = 19; i <= 29; ++i) CHECK_EQ(r.x[i], u.x[i]);
    CHECK(r.data == u.data);
    if (r.data != u.data) {
        for (size_t i = 0; i < r.data.size() && i < u.data.size(); ++i)
            if (r.data[i] != u.data[i]) {
                std::printf("  first data difference at 0x%" PRIx64 ": recompiled %02x, unicorn %02x (sizes %zu/%zu)\n",
                            static_cast<uint64_t>(i), r.data[i], u.data[i], r.data.size(), u.data.size());
                break;
            }
    }
    std::printf("  x0 = 0x%016" PRIx64 "\n", r.x[0]);
}

#if LGP_INDIRECT_HOOKS
struct Count {
    uint64_t calls = 0;
};

void count_hook(lgp::CpuState* ctx, lgp::LiftedFn original, void* user) {
    ++static_cast<Count*>(user)->calls;
    original(ctx);
}

// helper(int x) { return x * 5 + 3; } without the original: a replacing hook
// does what the function's RET would leave behind (the result in w0).
void helper_same(lgp::CpuState* ctx, lgp::LiftedFn, void* user) {
    ++static_cast<Count*>(user)->calls;
    ctx->x[0] = static_cast<uint32_t>(static_cast<int32_t>(ctx->x[0]) * 5 + 3);
}

void helper_other(lgp::CpuState* ctx, lgp::LiftedFn, void*) {
    ctx->x[0] = static_cast<uint32_t>(static_cast<int32_t>(ctx->x[0]) * 5 + 4);
}

void hooks(const Image& im, const Outcome& u0) {
    std::printf("[hooks on helper 0x%" PRIx64 " (BL) and op_add 0x%" PRIx64 " (BLR)]\n", im.helper, im.op_add);
    CHECK(im.helper != 0 && im.op_add != 0);
    CHECK(is_hookable(im.helper));
    CHECK(is_hookable(im.op_add));
    CHECK_EQ(hookable_entries().size(), 2u);
    CHECK(!is_hookable(im.entry));
    CHECK(!set_hook(im.entry, &count_hook));  // no door: refused
    CHECK_EQ(hook_count(), 0u);

    Count helper_calls, add_calls;
    CHECK(set_hook(im.helper, &count_hook, &helper_calls));
    CHECK(set_hook(im.op_add, &count_hook, &add_calls));
    compare(u0, run_recompiled(im, 0));
    CHECK_EQ(helper_calls.calls, 10u);  // the even cases 0-18 of dispatch()
    CHECK_EQ(add_calls.calls, 4u);      // three in fold(), one apply()

    Count same_calls;
    CHECK(set_hook(im.helper, &helper_same, &same_calls));  // replaces the counting hook
    CHECK(set_hook(im.op_add, nullptr));
    CHECK_EQ(hook_count(), 1u);
    compare(u0, run_recompiled(im, 0));
    CHECK_EQ(same_calls.calls, 10u);

    CHECK(set_hook(im.helper, &helper_other));
    const Outcome changed = run_recompiled(im, 0);
    CHECK(changed.sentinel);
    CHECK(changed.x[0] != u0.x[0]);

    clear_hooks();
    CHECK_EQ(hook_count(), 0u);
    compare(u0, run_recompiled(im, 0));
}
#endif

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s <program.nro> <program.elf>\n", argv[0]);
        return 2;
    }
    std::printf("guaranteed tail calls: %d, state chain depth %d\n", LGP_HAS_GUARANTEED_TAIL_CALLS, LGP_STATE_DEPTH);
    const Image im = load(argv[1], argv[2]);
    register_functions(lgp_lifted_entries, lgp_lifted_entries_count);
    const DispatchInfo info = dispatch_info();
    std::printf("dispatch table: %zu functions, 0x%" PRIx64 "..0x%" PRIx64 "\n", info.functions, info.base,
                info.base + info.size);
    CHECK_EQ(info.functions, lgp_lifted_entries_count);

    std::printf("[mode 0: function pointers, virtual calls, switch, tail calls, setjmp/longjmp]\n");
    const Outcome u0 = run_unicorn(im, 0);
    compare(u0, run_recompiled(im, 0));
    CHECK(unresolved_targets().empty());
#if LGP_INDIRECT_HOOKS
    hooks(im, u0);
#endif
#if LGP_INDIRECT_HV
    std::printf("[mode 0 on the hypervisor (EL0, real ARM64) vs. recompiled]\n");
    compare(run_hv(im, 0), run_recompiled(im, 0));
#endif

    std::printf("[mode 1: second entry 0x%" PRIx64 " of two_entries]\n", im.second_entry);
    const Outcome u1 = run_unicorn(im, 1);
    CHECK(u1.sentinel);
    CHECK_EQ(u1.x[0], 5u + 7u + 1u);  // the second entry skips the first add
    const Outcome r1 = run_recompiled(im, 1);
#if LGP_INDIRECT_CONFIG
    compare(u1, r1);
    CHECK(unresolved_targets().empty());
#if LGP_INDIRECT_HV
    std::printf("[mode 1 on the hypervisor vs. recompiled]\n");
    compare(run_hv(im, 1), run_recompiled(im, 1));
#endif
#else
    CHECK_EQ(r1.halt, static_cast<uint32_t>(lgp::kHaltBadJump));
    CHECK_EQ(r1.halt_info, im.second_entry);
    const std::vector<UnresolvedTarget> un = unresolved_targets();
    CHECK_EQ(un.size(), 1u);
    if (!un.empty()) {
        CHECK_EQ(un[0].target, im.second_entry);
        CHECK_EQ(un[0].count, 1u);
        CHECK(un[0].from_pc != 0);
    }
    // The error report is a valid recompiler config naming exactly that entry.
    const std::string config = unresolved_config();
    std::printf("  config: %s", config.c_str());
    const rc::AnalysisConfig parsed = rc::parse_config(config);
    CHECK_EQ(parsed.extra_functions.size(), 1u);
    if (!parsed.extra_functions.empty()) CHECK_EQ(parsed.extra_functions[0], im.second_entry);
#endif
    return finish();
}
