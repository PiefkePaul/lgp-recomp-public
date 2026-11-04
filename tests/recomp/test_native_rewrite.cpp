// Native rewrite plan (x18 renaming, native_rewrite.hpp) on synthetic code.
// With LGP_NATIVE_REWRITE_UNICORN the planned words are also applied and the
// original and the rewritten functions run under Unicorn: every register the
// caller may rely on (AAPCS64: x0..x7 results, x19..x30, sp) and memory must
// agree.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <random>

#include "a64_encode.hpp"
#include "check.hpp"
#include "lgp/recomp/a64.hpp"
#include "lgp/recomp/cfg.hpp"
#include "lgp/recomp/link.hpp"
#include "lgp/recomp/loader.hpp"
#include "lgp/recomp/native_rewrite.hpp"
#include "synthetic_module.hpp"

#ifdef LGP_NATIVE_REWRITE_UNICORN
#include "unicorn/unicorn_executor.h"
#endif

using namespace lgp::recomp;
using namespace ::a64;

namespace {

constexpr uint32_t ldr_x(uint8_t xt, uint8_t xn) { return 0xF9400000u | (xn << 5) | xt; }
constexpr uint32_t str_x(uint8_t xt, uint8_t xn) { return 0xF9000000u | (xn << 5) | xt; }
constexpr uint32_t ldp_x29_x30_post16 = 0xA8C17BFDu;  // ldp x29, x30, [sp], #16
constexpr uint32_t ldaxr_w(uint8_t rt, uint8_t rn) { return 0x885FFC00u | (rn << 5) | rt; }
constexpr uint32_t sentinel = 0xD4200000u | (0x4C47u << 5);  // brk #0x4c47

struct Layout {
    synth::Builder bd;
    std::vector<uint64_t> starts;  // module-relative
    size_t fn(std::initializer_list<uint32_t> words) {
        const uint64_t start = bd.text_off(bd.text_words.size());
        bd.text_words.insert(bd.text_words.end(), words);
        bd.fdes.push_back({start, bd.text_off(bd.text_words.size())});
        starts.push_back(start);
        return starts.size() - 1;
    }
    int64_t to(size_t from_fn, size_t word_in_fn, size_t target_fn) const {
        return static_cast<int64_t>(starts[target_fn]) - static_cast<int64_t>(starts[from_fn] + 4 * word_in_fn);
    }
};

struct Planned {
    LinkedImage img;
    NativeRewrite plan;
};

Planned plan(Layout& l) {
    std::vector<Module> mods;
    mods.push_back(load_nso(l.bd.build_nso(false), "main"));
    Planned p{link(std::move(mods)), {}};
    AnalysisResult res = analyze(p.img);
    std::vector<NativeRewrite> all = plan_native_rewrite(p.img, res);
    CHECK_EQ(all.size(), 1u);
    if (!all.empty()) p.plan = all[0];
    return p;
}

uint32_t word_at(const Planned& p, uint64_t va) {
    for (const auto& [a, w] : p.plan.words)
        if (a == va) return w;
    return p.img.read_u32(va);
}

}  // namespace

TEST(rename_gpr_forms) {
    // add x0, x18, x18 -> add x0, x9, x9
    CHECK_EQ(*rename_gpr(add_reg(true, 0, 18, 18), 0x1000, 18, 9), add_reg(true, 0, 9, 9));
    // movz x18, #18: only Rd, never the immediate.
    CHECK_EQ(*rename_gpr(movz(true, 18, 18), 0x1000, 18, 3), movz(true, 3, 18));
    // ldr x18, [x18]
    CHECK_EQ(*rename_gpr(ldr_x(18, 18), 0x1000, 18, 5), ldr_x(5, 5));
    // ldaxr w18, [x0]
    CHECK_EQ(*rename_gpr(ldaxr_w(18, 0), 0x1000, 18, 7), ldaxr_w(7, 0));
    // ubfm x18, x0, #18, #20: immr holds 18 at bit 16, must stay.
    CHECK_EQ(*rename_gpr(ubfm(true, 18, 0, 18, 20), 0x1000, 18, 4), ubfm(true, 4, 0, 18, 20));
    // Already names the target, or does not name x18.
    CHECK(!rename_gpr(add_reg(true, 9, 18, 0), 0x1000, 18, 9));
    CHECK(!rename_gpr(add_reg(true, 0, 1, 2), 0x1000, 18, 9));
}

TEST(plan_and_run) {
    Layout l;
    // f0: sum of eight words at [x0] with x18 as cursor.
    const size_t f0 = l.fn({mov_reg(true, 18, 0), movz(true, 9, 0), movz(true, 10, 8),
                            ldr_x(11, 18), add_reg(true, 9, 9, 11), add_imm(true, 18, 18, 8),
                            subs_imm(true, 10, 10, 1), bcond(1, -16), mov_reg(true, 0, 9), RET});
    // f1: clobbers x8..x17 (a callee).
    const size_t f1 = l.fn({movz(true, 8, 1), movz(true, 9, 2), movz(true, 10, 3), movz(true, 11, 4),
                            movz(true, 12, 5), movz(true, 13, 6), movz(true, 14, 7), movz(true, 15, 8),
                            movz(true, 16, 9), movz(true, 17, 10), RET});
    // f2: two webs around a call; the second also stores through x18.
    const size_t f2 = l.fn({stp_x29_x30_pre(-2), add_imm(true, 18, 0, 1), add_reg(true, 0, 18, 18), 0,
                            add_imm(true, 18, 0, 5), add_reg(true, 0, 0, 18), mov_reg(true, 18, 1),
                            str_x(0, 18), ldp_x29_x30_post16, RET});
    l.bd.text_words[l.starts[f2] / 4 - l.bd.code_offset() / 4 + 3] = bl(l.to(f2, 3, f1));
    // f3: reads x18 at entry: open, left for a stub. f4: svc.
    l.fn({add_reg(true, 0, 18, 18), RET});
    l.fn({svc(0x26), RET});
    // f5: x0..x17 all live across the web, x19 saved and restored by the
    // function itself (no LSDA in the synthetic FDEs): the web takes x19.
    const size_t f5 = l.fn({0xF81F0FF3u /* str x19, [sp, #-16]! */, movz(true, 18, 7), add_reg(true, 18, 18, 18),
                            add_reg(true, 0, 0, 18), add_reg(true, 0, 0, 1), add_reg(true, 0, 0, 2),
                            add_reg(true, 0, 0, 3), add_reg(true, 0, 0, 4), add_reg(true, 0, 0, 5),
                            add_reg(true, 0, 0, 6), add_reg(true, 0, 0, 7), add_reg(true, 0, 0, 8),
                            add_reg(true, 0, 0, 9), add_reg(true, 0, 0, 10), add_reg(true, 0, 0, 11),
                            add_reg(true, 0, 0, 12), add_reg(true, 0, 0, 13), add_reg(true, 0, 0, 14),
                            add_reg(true, 0, 0, 15), add_reg(true, 0, 0, 16), add_reg(true, 0, 0, 17),
                            0xF84107F3u /* ldr x19, [sp], #16 */, RET});
    const size_t fs = l.fn({sentinel});

    Planned p = plan(l);
    const NativeRewrite& r = p.plan;
    CHECK_EQ(r.functions_with_x18, 4u);
    CHECK_EQ(r.webs, 6u);  // f0 one, f2 three, f3 one, f5 one
    CHECK_EQ(r.webs_renamed, 5u);
    CHECK_EQ(r.functions_callee_saved, 4u);
    CHECK_EQ(r.webs_open, 1u);
    CHECK_EQ(r.sites_renamed, 3u + 6u + 3u);
    CHECK_EQ(r.x18_stub_sites.size(), 1u);
    CHECK_EQ(r.other_stub_sites.size(), 1u);
    const uint64_t base = p.img.modules[0].base;
    // f0: x1..x7 stay live to the exit (AAPCS results), x8 is the first free one.
    CHECK_EQ(word_at(p, base + l.starts[f0]), mov_reg(true, 8, 0));
    CHECK_EQ(word_at(p, base + l.starts[f5] + 4), movz(true, 19, 7));

#ifdef LGP_NATIVE_REWRITE_UNICORN
    using namespace lgp::difftest;
    const Module& mod = p.img.modules[0];
    std::vector<uint8_t> patched = mod.image;
    for (const auto& [va, w] : r.words) std::memcpy(patched.data() + (va - mod.base), &w, 4);
    CHECK(patched != mod.image);
    if (std::getenv("LGP_NATIVE_REWRITE_BREAK")) {  // negative control: rename f0's x8 to x1 (live to the exit)
        const uint32_t bad = *rename_gpr(word_at(p, mod.base + l.starts[f0]), 0, 8, 1);
        std::memcpy(patched.data() + l.starts[f0], &bad, 4);
    }
    constexpr uint64_t kData = 0x7000'0000, kStackTop = 0x7100'0000;
    std::mt19937_64 rng(56);
    for (size_t f : {f0, f2, f5}) {
        for (int round = 0; round < 8; ++round) {
            TestCase tc;
            tc.name = "native_rewrite";
            MemoryRegion code{mod.base, mod.image, kRead | kWrite | kExec};
            MemoryRegion data{kData, std::vector<uint8_t>(4096), kRead | kWrite};
            for (auto& b : data.bytes) b = static_cast<uint8_t>(rng());
            MemoryRegion stack{kStackTop - 0x10000, std::vector<uint8_t>(0x10000), kRead | kWrite};
            for (uint64_t& x : tc.initial.x) x = rng();
            tc.initial.x[0] = kData + 8 * (rng() % 64);
            tc.initial.x[1] = kData + 8 * (rng() % 64);
            tc.initial.x[30] = mod.base + l.starts[fs];
            tc.initial.sp = kStackTop - 0x100;
            tc.initial.pc = mod.base + l.starts[f];
            tc.memory = {code, data, stack};
            UnicornExecutor ex;
            const RunResult a = ex.run(tc);
            tc.memory[0].bytes = patched;
            const RunResult b = ex.run(tc);
            CHECK(a.outcome == Outcome::Sentinel);
            CHECK(b.outcome == Outcome::Sentinel);
            for (unsigned k : {0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 19u, 20u, 21u, 22u, 23u, 24u, 25u, 26u, 27u, 28u, 29u})
                CHECK_EQ(a.final.x[k], b.final.x[k]);
            CHECK_EQ(a.final.sp, b.final.sp);
            CHECK_EQ(a.final.pc, b.final.pc);
            CHECK(a.memory.size() == b.memory.size());
            for (size_t m = 1; m < a.memory.size() && m < b.memory.size(); ++m)
                CHECK(a.memory[m].bytes == b.memory[m].bytes);
        }
    }
    std::printf("unicorn: original and rewritten agree\n");
#endif
}

TEST(lsda_keeps_callee_saved_out) {
    // Same shape as f5 above, but the FDE has an LSDA: x19 is not allowed, and
    // with x0..x17 all live the web is left for a stub.
    Layout l;
    const size_t f = l.fn({0xF81F0FF3u, movz(true, 18, 7), add_reg(true, 18, 18, 18), add_reg(true, 0, 0, 18),
                           add_reg(true, 0, 0, 1), add_reg(true, 0, 0, 2), add_reg(true, 0, 0, 3),
                           add_reg(true, 0, 0, 4), add_reg(true, 0, 0, 5), add_reg(true, 0, 0, 6),
                           add_reg(true, 0, 0, 7), add_reg(true, 0, 0, 8), add_reg(true, 0, 0, 9),
                           add_reg(true, 0, 0, 10), add_reg(true, 0, 0, 11), add_reg(true, 0, 0, 12),
                           add_reg(true, 0, 0, 13), add_reg(true, 0, 0, 14), add_reg(true, 0, 0, 15),
                           add_reg(true, 0, 0, 16), add_reg(true, 0, 0, 17), 0xF84107F3u, RET});
    l.bd.lsda_fdes.push_back(l.starts[f]);
    Planned p = plan(l);
    CHECK_EQ(p.img.modules[0].lsda_fde_starts.size(), 1u);
    CHECK_EQ(p.plan.functions_callee_saved, 0u);
    CHECK_EQ(p.plan.webs, 1u);
    CHECK_EQ(p.plan.webs_no_register, 1u);
    CHECK_EQ(p.plan.x18_stub_sites.size(), 3u);
}

int main(int argc, char** argv) { return tst::run(argc, argv); }
