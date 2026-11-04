// Native-execution census (lgp_recomp census) on synthetic code.
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

#include "a64_encode.hpp"
#include "check.hpp"
#include "lgp/recomp/a64.hpp"
#include "lgp/recomp/cfg.hpp"
#include "lgp/recomp/link.hpp"
#include "lgp/recomp/loader.hpp"
#include "lgp/recomp/native_census.hpp"
#include "synthetic_module.hpp"

using namespace lgp::recomp;
using namespace ::a64;

namespace {

constexpr uint32_t mrs(uint32_t op0, uint32_t op1, uint32_t crn, uint32_t crm, uint32_t op2, uint8_t rt) {
    return 0xD5300000u | ((op0 & 1) << 19) | (op1 << 16) | (crn << 12) | (crm << 8) | (op2 << 5) | rt;
}
constexpr uint32_t msr(uint32_t op0, uint32_t op1, uint32_t crn, uint32_t crm, uint32_t op2, uint8_t rt) {
    return mrs(op0, op1, crn, crm, op2, rt) & ~(1u << 21);
}
constexpr uint32_t ldaxr_w(uint8_t rt, uint8_t rn) { return 0x885FFC00u | (rn << 5) | rt; }
constexpr uint32_t stlxr_w(uint8_t rs, uint8_t rt, uint8_t rn) { return 0x8800FC00u | (rs << 16) | (rn << 5) | rt; }
constexpr uint32_t ldrb_post(uint8_t rt, uint8_t rn, uint32_t imm9) { return 0x38400400u | (imm9 << 12) | (rn << 5) | rt; }
constexpr uint32_t dup_4s_w(uint8_t vd, uint8_t wn) { return 0x4E040C00u | (wn << 5) | vd; }
constexpr uint32_t ld1_16b(uint8_t vt, uint8_t xn) { return 0x4C407000u | (xn << 5) | vt; }
constexpr uint32_t yield = 0xD503203Fu;

// Lays out functions back to back, each with an FDE.
struct Layout {
    synth::Builder bd;
    void fn(std::initializer_list<uint32_t> words) {
        const uint64_t start = bd.text_off(bd.text_words.size());
        bd.text_words.insert(bd.text_words.end(), words);
        bd.fdes.push_back({start, bd.text_off(bd.text_words.size())});
    }
    NativeCensus run() {
        std::vector<Module> mods;
        mods.push_back(load_nso(bd.build_nso(false), "main"));
        LinkedImage img = link(std::move(mods));
        AnalysisResult res = analyze(img);
        std::vector<NativeCensus> all = native_census(img, res);
        CHECK_EQ(all.size(), 1u);
        return all.empty() ? NativeCensus{} : all[0];
    }
};

}  // namespace

TEST(gpr_use_fields) {
    // movz x0, #18: the imm16 field overlaps Rn, which must not count as x18.
    GprUse u = gpr_use(lgp::recomp::a64::decode(movz(true, 0, 18), 0x1000));
    CHECK_EQ(u.reads, 0u);
    CHECK_EQ(u.writes, 1u);
    // add x0, x1, #0x480: imm12 overlaps Rm.
    u = gpr_use(lgp::recomp::a64::decode(add_imm(true, 0, 1, 0x480), 0x1000));
    CHECK_EQ(u.reads, 1u << 1);
    // rbit x0, x1 leaves Rm = 0, which is not an operand.
    u = gpr_use(lgp::recomp::a64::decode(0xDAC00020u, 0x1000));
    CHECK_EQ(u.reads, 1u << 1);
    CHECK_EQ(u.writes, 1u);
    // sub sp, sp, x9 (extended register) reads and writes SP.
    u = gpr_use(lgp::recomp::a64::decode(add_ext(SP, SP, 9, 3, 0) | 0x40000000u, 0x1000));
    CHECK_EQ(u.reads, kGprSp | (1u << 9));
    CHECK_EQ(u.writes, kGprSp);
    // stlxr w9, w18, [x0]
    u = gpr_use(lgp::recomp::a64::decode(stlxr_w(9, 18, 0), 0x1000));
    CHECK_EQ(u.reads, (1u << 18) | 1u);
    CHECK_EQ(u.writes, 1u << 9);
    // ldrb w9, [x0], #1 writes the base back.
    u = gpr_use(lgp::recomp::a64::decode(ldrb_post(9, 0, 1), 0x1000));
    CHECK_EQ(u.reads, 1u);
    CHECK_EQ(u.writes, (1u << 9) | 1u);
    // SIMD&FP: dup v0.4s, w18 reads x18; ld1 {v0.16b}, [x18] reads its base.
    u = gpr_use(lgp::recomp::a64::decode(dup_4s_w(0, 18), 0x1000));
    CHECK_EQ(u.reads, 1u << 18);
    CHECK_EQ(u.writes, 0u);
    u = gpr_use(lgp::recomp::a64::decode(ld1_16b(0, 18), 0x1000));
    CHECK_EQ(u.reads, 1u << 18);
}

TEST(census_counts) {
    Layout l;
    // x18 as a plain register, an address base and a call target.
    l.fn({mov_reg(true, 18, 0), ldr_x_imm(1, 18, 0), blr(18), RET});
    // Thread registers, counter and SVC.
    l.fn({mrs(3, 3, 13, 0, 3, 0), svc(0x26), mrs(3, 3, 14, 0, 1, 1), msr(3, 3, 13, 0, 2, 2), mrs(3, 3, 4, 4, 0, 3), RET});
    // Exclusive window with x18 inside: ldaxr; add w18; stlxr; cbnz retry.
    l.fn({ldaxr_w(8, 0), add_imm(false, 18, 8, 1), stlxr_w(9, 18, 0), cbnz(false, 9, -12), RET});
    // Spin wait: ldr w8, [x19]; yield; cbz w8, loop.
    l.fn({ldr_imm(2, 1, 8, 19, 0), yield, cbz(false, 8, -8), RET});
    // Not a spin wait: the loop advances its pointer (strlen).
    l.fn({ldrb_post(9, 0, 1), cbnz(false, 9, -4), RET});
    // SP from a register and an unaligned immediate; no x18 despite overlapping fields.
    l.fn({add_ext(SP, SP, 9, 3, 0) | 0x40000000u, sub_imm(true, SP, SP, 8), add_imm(true, SP, SP, 8),
          sub_imm(true, SP, SP, 32), movz(true, 0, 18), add_imm(true, 0, 1, 0x480), RET});
    // SIMD&FP with x18, then a tail call through x18.
    l.fn({dup_4s_w(0, 18), ld1_16b(0, 18), br(18)});
    const NativeCensus c = l.run();

    CHECK_EQ(c.undecodable, 0u);
    CHECK_EQ(c.x18_sites, 8u);  // mov, ldr, blr, add, stlxr, dup, ld1, br
    CHECK_EQ(c.x18_writes, 2u);
    CHECK_EQ(c.x18_blr, 1u);
    CHECK_EQ(c.x18_br, 1u);
    CHECK_EQ(c.x18_br_jump_table, 0u);
    CHECK_EQ(c.x18_mem, 1u);
    CHECK_EQ(c.x18_simd, 2u);
    CHECK_EQ(c.x18_in_exclusive, 2u);
    CHECK_EQ(c.functions_with_x18, 3u);
    CHECK_EQ(c.svc, 1u);
    CHECK_EQ(c.svc_by_number.at(0x26), 1u);
    CHECK_EQ(c.mrs.at("tpidrro_el0"), 1u);
    CHECK_EQ(c.mrs.at("cntpct_el0"), 1u);
    CHECK_EQ(c.mrs.at("fpcr"), 1u);
    CHECK_EQ(c.msr.at("tpidr_el0"), 1u);
    CHECK_EQ(c.sp_from_register, 1u);
    CHECK_EQ(c.sp_imm_unaligned, 2u);
    CHECK_EQ(c.exclusive_windows, 1u);
    CHECK_EQ(c.exclusive_windows_with_site, 1u);
    CHECK_EQ(c.spin_loops, 1u);
    CHECK_EQ(c.spin_loops_with_hint, 1u);
    CHECK_EQ(c.yield_wfe, 1u);
    // 8 x18 + 1 svc + tpidrro + cntpct + tpidr + 3 sp
    CHECK_EQ(c.patch_sites(), 15u);
}

TEST(x18_liveness) {
    Layout l;
    // Renamable: x18 written before it is read, dead at every exit and call.
    l.fn({mov_reg(true, 18, 0), add_imm(true, 18, 18, 1), mov_reg(true, 0, 18), RET});
    // Live at entry: reads x18 first.
    l.fn({mov_reg(true, 0, 18), RET});
    // Live across a call: x18 set, bl, then read.
    l.fn({stp_x29_x30_pre(-2), mov_reg(true, 18, 0), bl(8), mov_reg(true, 0, 18), RET});
    // Written, then the loop reads it on the back edge only: still renamable.
    l.fn({movz(true, 18, 0), add_imm(true, 18, 18, 1), cmp_imm(true, 18, 10), bcond(1, -8), RET});
    const NativeCensus c = l.run();
    CHECK_EQ(c.functions_with_x18, 4u);
    CHECK_EQ(c.x18_live_at_entry, 1u);
    CHECK_EQ(c.x18_live_across_call, 1u);
    CHECK_EQ(c.x18_unresolved, 0u);
    CHECK_EQ(c.x18_no_free_register, 0u);
    CHECK_EQ(c.x18_renamable, 2u);
    CHECK_EQ(c.x18_renamable_sites, 6u);  // 3 + 3 (movz, add, cmp)
    // Webs: fn 1 one, fn 2 one (open), fn 3 one, fn 4 one. Fn 3 keeps x18
    // across bl, so every candidate the callee may clobber is out, but
    // x19..x28 are live at the exit and x18 is the only choice: left.
    CHECK_EQ(c.x18_webs, 4u);
    CHECK_EQ(c.x18_webs_open, 1u);
    CHECK_EQ(c.x18_webs_renamable, 2u);
    CHECK_EQ(c.x18_web_sites_renamable, 6u);
    CHECK_EQ(c.x18_web_sites_left, 3u);
    CHECK_EQ(c.x18_functions_all_renamable, 2u);
}

TEST(x18_webs) {
    Layout l;
    // Every one of x0..x17 is used somewhere, so no register is free in the
    // whole function, but x18's two live ranges each find a dead one.
    l.fn({movz(true, 18, 1), add_reg(true, 0, 18, 18),  // web 1: x1..x17 dead here
          movz(true, 1, 0), movz(true, 2, 0), movz(true, 3, 0), movz(true, 4, 0), movz(true, 5, 0),
          movz(true, 6, 0), movz(true, 7, 0), movz(true, 8, 0), movz(true, 9, 0), movz(true, 10, 0),
          movz(true, 11, 0), movz(true, 12, 0), movz(true, 13, 0), movz(true, 14, 0), movz(true, 15, 0),
          movz(true, 16, 0), movz(true, 17, 0),
          movz(true, 18, 2), add_reg(true, 0, 0, 18),  // web 2: x8..x17 dead again
          RET});
    // Read at entry: open, needs a stub.
    l.fn({add_reg(true, 0, 18, 18), RET});
    const NativeCensus c = l.run();
    CHECK_EQ(c.x18_no_free_register, 1u);
    CHECK_EQ(c.x18_renamable, 0u);
    CHECK_EQ(c.x18_webs, 3u);
    CHECK_EQ(c.x18_webs_renamable, 2u);
    CHECK_EQ(c.x18_web_sites_renamable, 4u);
    CHECK_EQ(c.x18_webs_open, 1u);
    CHECK_EQ(c.x18_web_sites_left, 1u);
    CHECK_EQ(c.x18_functions_all_renamable, 1u);
}

int main(int argc, char** argv) { return tst::run(argc, argv); }
