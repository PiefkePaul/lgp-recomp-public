// Function discovery and CFG tests on synthetic code (P2.2).
#include <cstdlib>

#include "a64_encode.hpp"
#include "check.hpp"
#include "lgp/recomp/a64_cf.hpp"
#include "lgp/recomp/cfg.hpp"
#include "lgp/recomp/link.hpp"
#include "lgp/recomp/loader.hpp"
#include "synthetic_module.hpp"

using namespace lgp::recomp;
using namespace a64;

namespace {

struct Built;
void dump(const Built& t);

struct Built {
    LinkedImage img;
    AnalysisResult res;
    uint64_t base;
    uint64_t va(uint64_t module_off) const { return base + module_off; }
};

Built analyze_builder(synth::Builder& b, const AnalysisConfig& cfg = {}, bool as_nro = false) {
    std::vector<Module> mods;
    mods.push_back(as_nro ? load_nro(b.build_nro(), "fixture.nro") : load_nso(b.build_nso(false), "main"));
    Built out{link(std::move(mods)), {}, 0};
    out.base = out.img.modules[0].base;
    out.res = analyze(out.img, cfg);
    dump(out);
    return out;
}

const BasicBlock* block_at(const Function& f, uint64_t start) {
    for (const auto& b : f.blocks) if (b.start == start) return &b;
    return nullptr;
}

void dump(const Built& t) {
    if (!std::getenv("LGP_TEST_DEBUG")) return;
    for (const auto& f : t.res.functions) {
        std::printf("  fn 0x%llx-0x%llx %s %s%s\n", (unsigned long long)f.entry, (unsigned long long)f.end,
                    function_source_name(f.source), f.name.c_str(), f.noreturn ? " noreturn" : "");
        for (const auto& b : f.blocks) {
            std::printf("    blk 0x%llx-0x%llx %s succ:", (unsigned long long)b.start, (unsigned long long)b.end, terminator_name(b.term));
            for (auto s : b.successors) std::printf(" 0x%llx", (unsigned long long)s);
            for (const auto& c : b.calls) std::printf(" call(0x%llx%s%s)", (unsigned long long)c.target, c.tail ? ",tail" : "", c.indirect ? ",ind" : "");
            std::printf("\n");
        }
    }
    for (const auto& d : t.res.diagnostics)
        std::printf("  diag %s 0x%llx %s\n", diagnostic_kind_name(d.kind), (unsigned long long)d.pc, d.text.c_str());
}

bool has_diag(const AnalysisResult& r, Diagnostic::Kind k) {
    for (const auto& d : r.diagnostics) if (d.kind == k) return true;
    return false;
}

}  // namespace

TEST(classify_control_flow) {
    CHECK(classify(b(0x100), 0x1000).kind == CfKind::Branch);
    CHECK_EQ(classify(b(0x100), 0x1000).target, 0x1100u);
    CHECK_EQ(classify(b(-8), 0x1000).target, 0xFF8u);
    CHECK(classify(bl(0x200), 0x1000).kind == CfKind::Call);
    CHECK_EQ(classify(bl(0x200), 0x1000).target, 0x1200u);
    CfInfo c = classify(bcond(8, 0x20), 0x1000);
    CHECK(c.kind == CfKind::CondBranch);
    CHECK(c.cond_kind == CondKind::BCond);
    CHECK_EQ(c.cond, 8);
    CHECK_EQ(c.target, 0x1020u);
    c = classify(cbz(false, 3, 0x10), 0x1000);
    CHECK(c.kind == CfKind::CondBranch && c.cond_kind == CondKind::Cbz && c.reg == 3 && c.target == 0x1010);
    c = classify(cbnz(true, 4, -4), 0x1000);
    CHECK(c.cond_kind == CondKind::Cbnz && c.target == 0xFFC);
    c = classify(tbz(0, 3, 16), 0x1000);
    CHECK(c.kind == CfKind::CondBranch && c.cond_kind == CondKind::Tbz && c.target == 0x1010);
    c = classify(tbnz(1, 40, 8), 0x1000);
    CHECK(c.cond_kind == CondKind::Tbnz && c.target == 0x1008);
    c = classify(br(16), 0);
    CHECK(c.kind == CfKind::IndirectBranch && c.reg == 16);
    c = classify(blr(8), 0);
    CHECK(c.kind == CfKind::IndirectCall && c.reg == 8);
    CHECK(classify(RET, 0).kind == CfKind::Return);
    CHECK(classify(0xD69F03E0, 0).kind == CfKind::Return);  // eret
    c = classify(svc(0x1f), 0);
    CHECK(c.kind == CfKind::Svc && c.imm == 0x1f);
    c = classify(brk(0xf000), 0);
    CHECK(c.kind == CfKind::Trap && c.imm == 0xf000);
    CHECK(classify(0xD4400000, 0).kind == CfKind::Trap);  // hlt #0
    CHECK(classify(UDF0, 0).kind == CfKind::Trap);
    CHECK(classify(NOP, 0).kind == CfKind::None);
    CHECK(classify(add_imm(true, 0, 1, 2), 0).kind == CfKind::None);
    CHECK(classify(ldr_x_imm(0, 1, 8), 0).kind == CfKind::None);
    CHECK(classify(stp_x29_x30_pre(-2), 0).kind == CfKind::None);
    CHECK(classify(0xD5033FDF, 0).kind == CfKind::None);  // isb
    CHECK(classify(0xD53BD040, 0).kind == CfKind::None);  // mrs x0, tpidr_el0
    CHECK(classify(0x02000000, 0).kind == CfKind::Unknown);  // unallocated class 0001
    CHECK(classify(0x00010000, 0).kind == CfKind::Unknown);  // class 0000 but not UDF
    CHECK(is_padding(NOP));
    CHECK(is_padding(0));
    CHECK(!is_padding(RET));
}

TEST(functions_from_fde_symbols_calls_and_tail_calls) {
    synth::Builder bd;
    // f0 (FDE): cbz w0, L1 ; bl f1 ; L1: ret
    // f1 (no FDE, found as BL target): b f2       (tail call across f3)
    // f3 (symbol, no FDE): ret
    // f2 (found as tail target): ret
    bd.text_words = {
        cbz(false, 0, 8),          // 0: f0
        bl(8),                     // 1: -> f1 at word 3
        RET,                       // 2
        b(3 * 4),                  // 3: f1 -> f2 at word 6
        RET,                       // 4: f3
        NOP,                       // 5: padding
        RET,                       // 6: f2
        NOP, NOP,
    };
    uint64_t f0 = bd.text_off(0), f1 = bd.text_off(3), f3 = bd.text_off(4), f2 = bd.text_off(6);
    bd.fdes = {{f0, f0 + 12}};
    bd.symbols.push_back({"f3", f3, 4, synth::STT_FUNC, synth::STB_GLOBAL, true});
    Built t = analyze_builder(bd);
    const AnalysisResult& r = t.res;
    CHECK_EQ(r.functions.size(), 5u);  // entry stub at 0 + f0..f3
    const Function* F0 = r.function_at(t.va(f0));
    const Function* F1 = r.function_at(t.va(f1));
    const Function* F2 = r.function_at(t.va(f2));
    const Function* F3 = r.function_at(t.va(f3));
    CHECK(F0 && F1 && F2 && F3);
    if (!(F0 && F1 && F2 && F3)) return;
    CHECK(F0->source == FunctionSource::Fde);
    CHECK(F1->source == FunctionSource::CallTarget);
    CHECK(F2->source == FunctionSource::TailTarget);
    CHECK(F3->source == FunctionSource::Symbol);
    CHECK_STR_EQ(F3->name, "f3");
    CHECK_STR_EQ(F0->name, "fn_8000010");
    CHECK_EQ(F0->blocks.size(), 3u);
    CHECK_EQ(F0->end, t.va(f0) + 12);
    const BasicBlock* b0 = block_at(*F0, t.va(f0));
    CHECK(b0 && b0->term == Terminator::CondBranch && b0->successors.size() == 2);
    CHECK(b0 && b0->successors[0] == t.va(f0) + 4 && b0->successors[1] == t.va(f0) + 8);
    const BasicBlock* b1 = block_at(*F0, t.va(f0) + 4);
    CHECK(b1 && b1->term == Terminator::Fallthrough && b1->calls.size() == 1 && b1->calls[0].target == t.va(f1));
    CHECK(b1 && !b1->calls[0].tail);
    const BasicBlock* b2 = block_at(*F0, t.va(f0) + 8);
    CHECK(b2 && b2->term == Terminator::Return);
    CHECK_EQ(F1->blocks.size(), 1u);
    CHECK(F1->blocks[0].term == Terminator::Branch);
    CHECK(F1->blocks[0].successors.empty());
    CHECK(F1->blocks[0].calls.size() == 1 && F1->blocks[0].calls[0].tail && F1->blocks[0].calls[0].target == t.va(f2));
    CHECK_EQ(F1->end, t.va(f1) + 4);
    CHECK(r.is_entry(t.va(f2)));
    // Entry stub: `b code` at offset 0 is a one-instruction tail call; the MOD0 word at +4 is not code.
    const Function* E = r.function_at(t.va(0));
    CHECK(E && E->source == FunctionSource::Entry && E->blocks.size() == 1 && E->blocks[0].calls.size() == 1);
    CHECK(!r.is_entry(t.va(4)));
    CHECK_EQ(r.stats.direct_calls, 1u);
    CHECK_EQ(r.stats.tail_calls, 2u);
    CHECK(r.diagnostics.empty());
}

TEST(noreturn_from_fde_tail_and_config_name) {
    synth::Builder bd;
    // f0 (FDE): bl abort_fn ; <end of FDE>   g (no FDE): ret
    // f1 (FDE): cmp; b.ne L; bl abort_fn; L: ret    -> block after bl must not fall into anything
    // abort_fn: brk
    bd.text_words = {
        bl(5 * 4),            // 0: f0 -> abort_fn (word 5)
        RET,                  // 1: g, directly behind f0 without FDE
        cbz(false, 0, 8),     // 2: f1
        bl(2 * 4),            // 3: -> abort_fn
        RET,                  // 4
        brk(0),               // 5: abort_fn
        NOP, NOP,
    };
    uint64_t f0 = bd.text_off(0), g = bd.text_off(1), f1 = bd.text_off(2), ab = bd.text_off(5);
    bd.fdes = {{f0, f0 + 4}, {f1, f1 + 12}};
    Built t = analyze_builder(bd);
    const Function* F0 = t.res.function_at(t.va(f0));
    const Function* G = t.res.function_at(t.va(g));
    const Function* F1 = t.res.function_at(t.va(f1));
    const Function* AB = t.res.function_at(t.va(ab));
    CHECK(F0 && G && F1 && AB);
    if (!(F0 && G && F1 && AB)) return;
    CHECK(F0->blocks.size() == 1 && F0->blocks[0].term == Terminator::NoReturnCall);
    CHECK_EQ(F0->end, t.va(f0) + 4);
    CHECK(G->source == FunctionSource::GapScan);
    CHECK(AB->noreturn);
    CHECK(AB->blocks[0].term == Terminator::Trap);
    const BasicBlock* bb = block_at(*F1, t.va(f1) + 4);
    CHECK(bb && bb->term == Terminator::NoReturnCall && bb->successors.empty());
    CHECK_EQ(t.res.stats.noreturn_functions, 3u);  // abort_fn, f0 (ends in bl abort), entry stub (b f0)
    CHECK(!has_diag(t.res, Diagnostic::Kind::FallsIntoNextFunction));

    // Same code, but abort_fn only known by configured name (no FDE tail).
    synth::Builder bd2;
    bd2.text_words = bd.text_words;
    bd2.symbols.push_back({"my_abort", bd2.text_off(5), 4, synth::STT_FUNC, synth::STB_GLOBAL, true});
    bd2.fdes = {{bd2.text_off(2), bd2.text_off(2) + 12}};
    AnalysisConfig cfg = parse_config("noreturn my_abort\n");
    Built t2 = analyze_builder(bd2, cfg);
    const Function* F = t2.res.function_at(t2.va(bd2.text_off(0)));
    CHECK(F && F->blocks.size() == 1 && F->blocks[0].term == Terminator::NoReturnCall);
}

TEST(fde_tail_bl_does_not_make_a_returning_callee_noreturn) {
    // A cleanup path can end its FDE with a BL to an ordinary function (sdk: an EH
    // landing pad ends in `bl` to a mutex-unlock thunk). The FDE-tail rule guesses
    // noreturn; the callee's own code shows a return path and must win, or every
    // caller loses the code after its call.
    synth::Builder bd;
    // f0 (FDE): movz; bl stub          <end of FDE>
    // f2 (FDE): bl stub; movz; ret      -> must continue after the call
    // stub (no FDE, call target): b unlock
    // unlock (FDE): ret
    bd.text_words = {
        movz(false, 0, 1),    // 0: f0
        bl(4 * 4),            // 1: -> stub (word 5)
        bl(3 * 4),            // 2: f2 -> stub (word 5)
        movz(false, 0, 2),    // 3
        RET,                  // 4
        b(4),                 // 5: stub -> unlock (word 6)
        RET,                  // 6: unlock
        NOP,
    };
    uint64_t f0 = bd.text_off(0), f2 = bd.text_off(2), stub = bd.text_off(5), unlock = bd.text_off(6);
    bd.fdes = {{f0, f0 + 8}, {f2, f2 + 12}, {unlock, unlock + 4}};
    Built t = analyze_builder(bd);
    const Function* S = t.res.function_at(t.va(stub));
    const Function* U = t.res.function_at(t.va(unlock));
    const Function* F2 = t.res.function_at(t.va(f2));
    CHECK(S && U && F2);
    if (!(S && U && F2)) return;
    CHECK(!S->noreturn);
    CHECK(!U->noreturn);
    const BasicBlock* call = block_at(*F2, t.va(f2));
    CHECK(call && call->term != Terminator::NoReturnCall);
    bool f2_returns = false;
    for (const auto& blk : F2->blocks) f2_returns |= blk.term == Terminator::Return;
    CHECK(f2_returns);
}

TEST(jump_table_clang_byte_table) {
    // clang -Oz style:  cmp w0,#3; b.hi default; adr x9, tbl; adr x10, base; ldrb w11,[x9,w0,uxtw];
    //                   add x10,x10,x11,lsl #2; br x10 ; cases...
    synth::Builder bd;
    uint64_t f = bd.text_off(0);
    // layout (word index): 0 cmp 1 b.hi 2 adr tbl 3 adr base 4 ldrb 5 add 6 br 7 c0 8 c1 9 c2 10 c3 11 default
    std::vector<uint32_t> w(12, RET);
    uint64_t tbl = bd.alloc_ro(4, 4);
    int64_t pc2 = static_cast<int64_t>(bd.text_off(2)), pc3 = static_cast<int64_t>(bd.text_off(3));
    uint64_t base = bd.text_off(7);
    w[0] = cmp_imm(false, 0, 3);
    w[1] = bcond(8, static_cast<int64_t>(bd.text_off(11)) - static_cast<int64_t>(bd.text_off(1)));  // b.hi default
    w[2] = adr(9, static_cast<int64_t>(tbl) - pc2);
    w[3] = adr(10, static_cast<int64_t>(base) - pc3);
    w[4] = ldrb_uxtw(11, 9, 0);
    w[5] = add_reg(true, 10, 10, 11, 2);
    w[6] = br(10);
    w[7] = movz(false, 0, 10); w[8] = movz(false, 0, 11); w[9] = movz(false, 0, 12); w[10] = movz(false, 0, 13);
    w[11] = RET;
    // case blocks fall through to the ret at 11; entries are (target - base) / 4 in words
    bd.write(tbl, {0, 1, 2, 3});
    bd.text_words = w;
    bd.fdes = {{f, f + 12 * 4}};
    Built t = analyze_builder(bd);
    const Function* F = t.res.function_at(t.va(f));
    CHECK(F);
    if (!F) return;
    CHECK_EQ(F->jump_tables.size(), 1u);
    if (F->jump_tables.empty()) return;
    const JumpTable& jt = F->jump_tables[0];
    CHECK_EQ(jt.br_pc, t.va(bd.text_off(6)));
    CHECK_EQ(jt.table_va, t.va(tbl));
    CHECK_EQ(jt.entry_size, 1);
    CHECK_EQ(jt.count, 4u);
    CHECK(jt.bound_known);
    CHECK(jt.confidence == Confidence::High);
    CHECK_EQ(jt.targets.size(), 4u);
    CHECK_EQ(jt.targets[3], t.va(bd.text_off(10)));
    const BasicBlock* brb = block_at(*F, t.va(bd.text_off(2)));
    CHECK(brb && brb->term == Terminator::JumpTable && brb->successors.size() == 4);
    CHECK(!F->has_unresolved_indirect);
    CHECK_EQ(t.res.stats.jump_tables_bounded, 1u);
    CHECK(!has_diag(t.res, Diagnostic::Kind::UnresolvedIndirectJump));
}

TEST(jump_table_gcc_sxth_and_adrp_add) {
    // gcc style: sub w1, w0, #2; cmp w1, #2; b.hi def; adrp x2, tbl@PAGE; add x2, x2, tbl@PAGEOFF;
    //            ldrh w1, [x2, w1, uxtw #1]; adr x2, base; add x1, x2, w1, sxth #2; br x1
    synth::Builder bd;
    uint64_t f = bd.text_off(0);
    std::vector<uint32_t> w(14, RET);
    uint64_t tbl = bd.alloc_ro(8, 8);
    uint64_t base = bd.text_off(9);
    int64_t pc3 = static_cast<int64_t>(bd.text_off(3)), pc6 = static_cast<int64_t>(bd.text_off(6));
    int64_t page_delta = (static_cast<int64_t>(tbl) & ~0xFFF) - (pc3 & ~0xFFF);
    w[0] = sub_imm(false, 1, 0, 2);
    w[1] = cmp_imm(false, 1, 2);
    w[2] = bcond(8, static_cast<int64_t>(bd.text_off(13)) - static_cast<int64_t>(bd.text_off(2)));
    w[3] = adrp(2, page_delta >> 12);
    w[4] = add_imm(true, 2, 2, static_cast<uint16_t>(tbl & 0xFFF));
    w[5] = ldrh_uxtw_s(1, 2, 1);
    w[6] = adr(2, static_cast<int64_t>(base) - pc6);
    w[7] = add_ext(1, 2, 1, 5, 2);  // sxth #2
    w[8] = br(1);
    w[9] = movz(false, 0, 1); w[10] = movz(false, 0, 2); w[11] = movz(false, 0, 3); w[12] = RET; w[13] = RET;
    // entries: signed 16-bit (target-base)/4; use a negative one to check sign handling: case2 -> word 9 again
    bd.write(tbl, {0, 0, 1, 0, 2, 0, 0, 0});
    bd.text_words = w;
    bd.fdes = {{f, f + 14 * 4}};
    Built t = analyze_builder(bd);
    const Function* F = t.res.function_at(t.va(f));
    CHECK(F && F->jump_tables.size() == 1);
    if (!F || F->jump_tables.empty()) return;
    const JumpTable& jt = F->jump_tables[0];
    CHECK_EQ(jt.entry_size, 2);
    CHECK(jt.entries_signed);
    CHECK_EQ(jt.count, 3u);
    CHECK(jt.bound_known);
    CHECK_EQ(jt.targets[2], t.va(bd.text_off(11)));
    CHECK_EQ(jt.base_va, t.va(base));
}

TEST(jump_table_absolute_64bit_with_relocations) {
    // cmp x0, #2; b.cs def; adrp x8, tbl; add x8, x8, lo12; ldr x9, [x8, x0, lsl #3]; br x9
    synth::Builder bd;
    uint64_t f = bd.text_off(0);
    std::vector<uint32_t> w(10, RET);
    uint64_t tbl = bd.alloc_ro(16, 8);
    int64_t pc2 = static_cast<int64_t>(bd.text_off(2));
    w[0] = cmp_imm(true, 0, 2);
    w[1] = bcond(2, static_cast<int64_t>(bd.text_off(9)) - static_cast<int64_t>(bd.text_off(1)));  // b.cs default
    w[2] = adrp(8, ((static_cast<int64_t>(tbl) & ~0xFFF) - (pc2 & ~0xFFF)) >> 12);
    w[3] = add_imm(true, 8, 8, static_cast<uint16_t>(tbl & 0xFFF));
    w[4] = ldr_x_lsl3(9, 8, 0);
    w[5] = br(9);
    w[6] = movz(false, 0, 1); w[7] = RET; w[8] = movz(false, 0, 2); w[9] = RET;
    bd.relas.push_back({tbl, synth::R_RELATIVE, "", static_cast<int64_t>(bd.text_off(6))});
    bd.relas.push_back({tbl + 8, synth::R_RELATIVE, "", static_cast<int64_t>(bd.text_off(8))});
    bd.text_words = w;
    bd.fdes = {{f, f + 10 * 4}};
    Built t = analyze_builder(bd);
    const Function* F = t.res.function_at(t.va(f));
    CHECK(F && F->jump_tables.size() == 1);
    if (!F || F->jump_tables.empty()) return;
    const JumpTable& jt = F->jump_tables[0];
    CHECK_EQ(jt.entry_size, 8);
    CHECK_EQ(jt.count, 2u);
    CHECK(jt.bound_known);
    CHECK_EQ(jt.targets[0], t.va(bd.text_off(6)));
    CHECK_EQ(jt.targets[1], t.va(bd.text_off(8)));
    // The table pointers were data pointers into .text: they must not have become functions.
    CHECK(!t.res.is_entry(t.va(bd.text_off(6))));
    CHECK(has_diag(t.res, Diagnostic::Kind::DataPointerIgnored));
}

TEST(jump_table_without_bound_is_low_confidence) {
    // No cmp: adr x9, tbl; ldrsw x10, [x9, w0, uxtw #2]; add x9, x9, x10; br x9  (clang -fPIC 32-bit table, base == table)
    synth::Builder bd;
    uint64_t f = bd.text_off(0);
    std::vector<uint32_t> w(8, RET);
    uint64_t tbl = bd.alloc_ro(16, 4);
    int64_t pc0 = static_cast<int64_t>(bd.text_off(0));
    w[0] = adr(9, static_cast<int64_t>(tbl) - pc0);
    w[1] = ldrsw_uxtw_s(10, 9, 0);
    w[2] = add_reg(true, 9, 9, 10, 0);
    w[3] = br(9);
    w[4] = movz(false, 0, 1); w[5] = RET; w[6] = movz(false, 0, 2); w[7] = RET;
    auto rel = [&](uint64_t target) { return static_cast<uint32_t>(static_cast<int32_t>(static_cast<int64_t>(target) - static_cast<int64_t>(tbl))); };
    std::vector<uint8_t> entries;
    for (uint32_t v : {rel(bd.text_off(4)), rel(bd.text_off(6)), 0x12345678u, 0u}) synth::app32(entries, v);
    bd.write(tbl, entries);
    bd.text_words = w;
    bd.fdes = {{f, f + 8 * 4}};
    Built t = analyze_builder(bd);
    const Function* F = t.res.function_at(t.va(f));
    CHECK(F && F->jump_tables.size() == 1);
    if (!F || F->jump_tables.empty()) return;
    const JumpTable& jt = F->jump_tables[0];
    CHECK(!jt.bound_known);
    CHECK(jt.confidence == Confidence::Low);
    CHECK_EQ(jt.count, 2u);  // third entry points outside the function -> scan stops
    CHECK_EQ(jt.entry_size, 4);
    CHECK(has_diag(t.res, Diagnostic::Kind::JumpTableUnbounded));
}

TEST(jump_table_override_from_config) {
    synth::Builder bd;
    uint64_t f = bd.text_off(0);
    // Unrecognisable sequence (table address comes from a register we don't model): mul; ldrb; add; br
    std::vector<uint32_t> w(8, RET);
    uint64_t tbl = bd.alloc_ro(4, 4);
    w[0] = mul_w(9, 1, 2);
    w[1] = ldrb_uxtw(11, 9, 0);
    w[2] = adr(10, static_cast<int64_t>(bd.text_off(4)) - static_cast<int64_t>(bd.text_off(2)));
    w[3] = add_reg(true, 10, 10, 11, 2);
    w[4] = br(10);
    w[5] = RET; w[6] = movz(false, 0, 1); w[7] = RET;
    bd.write(tbl, {1, 2, 3, 3});
    bd.text_words = w;
    bd.fdes = {{f, f + 8 * 4}};
    Built plain = analyze_builder(bd);
    const Function* F = plain.res.function_at(plain.va(f));
    CHECK(F && F->has_unresolved_indirect && F->jump_tables.empty());
    CHECK(has_diag(plain.res, Diagnostic::Kind::UnresolvedIndirectJump));
    char cfgtext[256];
    std::snprintf(cfgtext, sizeof cfgtext, "jumptable br=0x%llx table=0x%llx entry=1 count=3 base=0x%llx unsigned\n",
                  (unsigned long long)plain.va(bd.text_off(4)), (unsigned long long)plain.va(tbl),
                  (unsigned long long)plain.va(bd.text_off(4)));
    synth::Builder bd2 = bd;
    Built fixed = analyze_builder(bd2, parse_config(cfgtext));
    const Function* G = fixed.res.function_at(fixed.va(f));
    CHECK(G && !G->has_unresolved_indirect && G->jump_tables.size() == 1);
    if (G && !G->jump_tables.empty()) {
        CHECK_EQ(G->jump_tables[0].count, 3u);
        CHECK_EQ(G->jump_tables[0].targets[2], fixed.va(bd.text_off(7)));
    }
    AnalysisConfig unused = parse_config("jumptable br=0x1234 table=0x5678 entry=4 count=2\n");
    synth::Builder bd3 = bd;
    Built u = analyze_builder(bd3, unused);
    CHECK(has_diag(u.res, Diagnostic::Kind::JumpTableOverrideUnused));
}

TEST(plt_stub_via_got_is_tail_call_with_slot) {
    // main: stub: adrp x16, got; ldr x17, [x16, #lo12]; add x16, x16, #lo12; br x17   -> ext in sdk
    synth::Builder mb;
    uint64_t got = mb.alloc_data(8, 8);
    uint64_t stub = mb.text_off(0);
    int64_t pc0 = static_cast<int64_t>(stub);
    mb.text_words = {
        adrp(16, ((static_cast<int64_t>(got) & ~0xFFF) - (pc0 & ~0xFFF)) >> 12),
        ldr_x_imm(17, 16, static_cast<uint16_t>(got & 0xFFF)),
        add_imm(true, 16, 16, static_cast<uint16_t>(got & 0xFFF)),
        br(17),
        RET,
    };
    mb.symbols.push_back({"ext", 0, 0, synth::STT_FUNC, synth::STB_GLOBAL, false});
    mb.relas.push_back({got, synth::R_JUMP_SLOT, "ext", 0});
    mb.fdes = {{stub, stub + 16}};
    synth::Builder sb;
    sb.text_words = {RET};
    sb.symbols.push_back({"ext", sb.text_off(0), 4, synth::STT_FUNC, synth::STB_GLOBAL, true});
    std::vector<Module> mods;
    mods.push_back(load_nso(mb.build_nso(false), "main"));
    mods.push_back(load_nso(sb.build_nso(false), "sdk"));
    LinkedImage img = link(std::move(mods));
    AnalysisResult r = analyze(img);
    const Function* S = r.function_at(img.modules[0].base + stub);
    CHECK(S);
    if (!S) return;
    CHECK(S->blocks.size() == 1 && S->blocks[0].term == Terminator::Branch);
    CHECK_EQ(S->blocks[0].calls.size(), 1u);
    if (!S->blocks[0].calls.empty()) {
        const Call& c = S->blocks[0].calls[0];
        CHECK(c.tail);
        CHECK_EQ(c.target, img.modules[1].base + sb.text_off(0));
        CHECK_EQ(c.via_slot, img.modules[0].base + got);
    }
    CHECK(!S->has_unresolved_indirect);
    // The sdk function is found through its symbol and the import.
    CHECK(r.is_entry(img.modules[1].base + sb.text_off(0)));
}

TEST(weak_seed_demotion_and_gap_scan) {
    // f0 (FDE covers 4 words): cbz w0, +8 ; ret ; <data pointer into here> ; ret
    // A RELATIVE pointer into the middle of f0's FDE range is ignored (label), and one into
    // a gap function (no FDE) that is jumped into gets demoted.
    synth::Builder bd;
    bd.text_words = {
        cbz(false, 0, 12),     // 0: f0 -> word 3
        movz(false, 0, 1),     // 1
        RET,                   // 2
        RET,                   // 3  (pointer target inside FDE -> label)
        // g (no FDE): cbz -> word 7 ; nop ; nop ; ret
        cbz(false, 1, 12),     // 4: g
        NOP,                   // 5
        RET,                   // 6
        RET,                   // 7  <- data pointer here, but reached by cond branch from g: demote
        NOP, NOP,
    };
    uint64_t f0 = bd.text_off(0), g = bd.text_off(4);
    bd.fdes = {{f0, f0 + 16}};
    uint64_t p1 = bd.alloc_data(8), p2 = bd.alloc_data(8);
    bd.relas.push_back({p1, synth::R_RELATIVE, "", static_cast<int64_t>(bd.text_off(3))});
    bd.relas.push_back({p2, synth::R_RELATIVE, "", static_cast<int64_t>(bd.text_off(7))});
    Built t = analyze_builder(bd);
    CHECK(!t.res.is_entry(t.va(bd.text_off(3))));
    CHECK(!t.res.is_entry(t.va(bd.text_off(7))));
    const Function* G = t.res.function_at(t.va(g));
    CHECK(G && G->source == FunctionSource::GapScan);
    if (G) CHECK_EQ(G->end, t.va(bd.text_off(8)));
    CHECK(has_diag(t.res, Diagnostic::Kind::SeedDemoted));
    CHECK(has_diag(t.res, Diagnostic::Kind::DataPointerIgnored));
    CHECK(!has_diag(t.res, Diagnostic::Kind::CrossFunctionJump));
    // Padding after g is not a function.
    CHECK_EQ(t.res.functions.size(), 3u);  // entry stub, f0, g
    CHECK_EQ(t.res.stats.padding_bytes, 0x1000u - 8u - 7u * 4u);  // everything but the 7 real instructions (header words excluded)
}

TEST(nro_header_is_not_scanned_as_code) {
    synth::Builder bd;
    bd.nro = true;
    bd.text_words = {RET, NOP, NOP, NOP};
    bd.fdes = {{bd.text_off(0), bd.text_off(0) + 4}};
    Built t = analyze_builder(bd, {}, true);
    CHECK_EQ(t.res.functions.size(), 2u);  // entry `b` at 0 and the ret at 0x80
    for (const auto& f : t.res.functions) CHECK(f.entry == t.va(0) || f.entry == t.va(0x80));
    CHECK(!has_diag(t.res, Diagnostic::Kind::UndecodableInstruction));
}

TEST(falls_into_next_function_and_cross_jump_diagnostics) {
    synth::Builder bd;
    // f0 (FDE, 2 words): movz; movz   -> no terminator, falls into f1 (FDE)
    // f1 (FDE): b into the middle of f2 (cross jump)
    // f2 (FDE): nop; ret
    bd.text_words = {
        movz(false, 0, 1), movz(false, 0, 2),      // f0
        b(4 + 4),                                   // f1 -> word 4 (middle of f2)
        NOP, RET,                                   // f2
    };
    uint64_t f0 = bd.text_off(0), f1 = bd.text_off(2), f2 = bd.text_off(3);
    bd.fdes = {{f0, f0 + 8}, {f1, f1 + 4}, {f2, f2 + 8}};
    Built t = analyze_builder(bd);
    CHECK(has_diag(t.res, Diagnostic::Kind::FallsIntoNextFunction));
    CHECK(has_diag(t.res, Diagnostic::Kind::CrossFunctionJump));
    const Function* F0 = t.res.function_at(t.va(f0));
    CHECK(F0 && F0->blocks.size() == 1 && F0->blocks[0].term == Terminator::Branch && F0->blocks[0].calls.size() == 1);
    CHECK(F0 && F0->blocks[0].calls[0].tail && F0->blocks[0].calls[0].target == t.va(f1));
    CHECK(!t.res.is_entry(t.va(bd.text_off(4))));
}

TEST(noreturn_inferred_from_bl_before_strong_neighbour) {
    synth::Builder bd;
    // f0 (symbol, no FDE): movz; bl die     -> no terminator before f1: die must be noreturn
    // f1 (symbol, no FDE): ret
    // die (symbol, no FDE): b .            (never returns)
    bd.text_words = {
        movz(false, 0, 1), bl(3 * 4),              // 0,1: f0, bl -> word 4
        RET,                                       // 2: f1
        NOP,                                       // 3: padding
        b(0),                                      // 4: die: b .
    };
    uint64_t f0 = bd.text_off(0), f1 = bd.text_off(2), die = bd.text_off(4);
    bd.symbols.push_back({"f0", f0, 8, synth::STT_FUNC, synth::STB_GLOBAL, true});
    bd.symbols.push_back({"f1", f1, 4, synth::STT_FUNC, synth::STB_GLOBAL, true});
    bd.symbols.push_back({"die", die, 4, synth::STT_FUNC, synth::STB_GLOBAL, true});
    Built t = analyze_builder(bd);
    CHECK(has_diag(t.res, Diagnostic::Kind::NoReturnInferred));
    CHECK(!has_diag(t.res, Diagnostic::Kind::FallsIntoNextFunction));
    const Function* F0 = t.res.function_at(t.va(f0));
    const Function* D = t.res.function_at(t.va(die));
    CHECK(F0 && F0->blocks.size() == 1 && F0->blocks[0].term == Terminator::NoReturnCall);
    CHECK(F0 && F0->end == t.va(f1));
    CHECK(D && D->noreturn);
    CHECK(t.res.function_at(t.va(f1)) != nullptr);
}

TEST(gap_scan_keeps_leading_nop_when_not_alignment_padding) {
    synth::Builder bd;
    // Word 0 is 16-aligned (NSO code offset 0x10), so even words are 8-aligned, odd words only 4-aligned.
    // f0 (FDE, words 0-1): nop; ret
    // words 2-3: nop; ret   -> the NOP at an 8-aligned address leads to a 4-aligned word: code, not padding
    //                          (linker-patched `bl <undefined weak>` becomes NOP, libnx exception return entry)
    // f1 (FDE, words 4-5): ret; ret
    // words 6-7: nop; nop   -> padding up to the 16-aligned word 8
    // words 8..: ret
    bd.text_words = { NOP, RET, NOP, RET, RET, RET, NOP, NOP, RET };
    uint64_t f0 = bd.text_off(0), f1 = bd.text_off(4);
    bd.fdes = {{f0, f0 + 8}, {f1, f1 + 8}};
    Built t = analyze_builder(bd);
    CHECK(t.res.is_entry(t.va(bd.text_off(2))));
    CHECK(!t.res.is_entry(t.va(bd.text_off(3))));
    CHECK(!t.res.is_entry(t.va(bd.text_off(6))));
    CHECK(t.res.is_entry(t.va(bd.text_off(8))));
    const Function* G = t.res.function_at(t.va(bd.text_off(2)));
    CHECK(G && G->source == FunctionSource::GapScan && G->end == t.va(bd.text_off(4)));
}

TEST(diagnostics_are_not_repeated_by_re_exploration) {
    synth::Builder bd;
    // f0 (FDE): br x9 (unresolved); trap function afterwards makes noreturn propagation re-explore everything.
    bd.text_words = { br(9), NOP, UDF0, NOP };
    uint64_t f0 = bd.text_off(0), t0 = bd.text_off(2);
    bd.fdes = {{f0, f0 + 4}, {t0, t0 + 4}};
    Built t = analyze_builder(bd);
    size_t n = 0;
    for (const auto& d : t.res.diagnostics)
        if (d.kind == Diagnostic::Kind::UnresolvedIndirectJump && d.pc == t.va(f0)) ++n;
    CHECK_EQ(n, 1u);
}

TEST(entry_stub_and_code_behind_nro_header_are_one_function) {
    synth::Builder bd;
    bd.nro = true;
    // `b 0x80` at 0 (builder), header, then plain code at 0x80 with no FDE or symbol (libnx/clang crt0 `_start: b 1f`).
    bd.text_words = { movz(false, 0, 1), RET, NOP, NOP };
    Built t = analyze_builder(bd, {}, true);
    CHECK_EQ(t.res.functions.size(), 1u);
    const Function* E = t.res.function_at(t.va(0));
    CHECK(E && E->blocks.size() == 2 && E->end == t.va(bd.text_off(2)));
    CHECK(!t.res.is_entry(t.va(0x80)));
    CHECK(!has_diag(t.res, Diagnostic::Kind::UndecodableInstruction));
    CHECK(!has_diag(t.res, Diagnostic::Kind::FallsIntoNextFunction));
}

TEST(call_into_guessed_body_splits_function) {
    // g (gap scan, no FDE): nop; nop; ret ; h: ret   where a later function calls h.
    synth::Builder bd;
    bd.text_words = {
        NOP, NOP, RET,          // 0..2 g
        RET,                    // 3 h
        bl(-4),                 // 4 f (FDE): bl h ; ret
        RET,
    };
    uint64_t f = bd.text_off(4);
    bd.fdes = {{f, f + 8}};
    Built t = analyze_builder(bd);
    const Function* H = t.res.function_at(t.va(bd.text_off(3)));
    CHECK(H && H->source == FunctionSource::CallTarget);
    const Function* G = t.res.function_at(t.va(bd.text_off(0)));
    // g starts at the entry stub? No: the entry `b` at 0 jumps to text_off(0) == code start, so the code
    // start is itself the tail target; it ends before h.
    CHECK(G != nullptr);
    if (G) CHECK_EQ(G->end, t.va(bd.text_off(3)));
}

TEST(config_parser) {
    AnalysisConfig c = parse_config(
        "# comment\n"
        "function 0x8000100 0x8000200\n"
        "noreturn abort 0x8000300\n"
        "jumptable br=0x8000400 table=0x8001000 entry=4 count=5 signed shift=2\n"
        "scan_gaps off\n"
        "data_pointers off\n");
    CHECK_EQ(c.extra_functions.size(), 2u);
    CHECK_EQ(c.extra_functions[1], 0x8000200u);
    CHECK(std::find(c.noreturn_symbols.begin(), c.noreturn_symbols.end(), "abort") != c.noreturn_symbols.end());
    CHECK_EQ(c.noreturn_addresses.size(), 1u);
    CHECK_EQ(c.jump_table_overrides.size(), 1u);
    CHECK_EQ(c.jump_table_overrides[0].count, 5u);
    CHECK(c.jump_table_overrides[0].entries_signed);
    CHECK(!c.scan_gaps);
    CHECK(!c.seed_data_pointers);
    CHECK_THROWS(parse_config("bogus 1\n"), FormatError);
    CHECK_THROWS(parse_config("jumptable br=0x10\n"), FormatError);
    CHECK_THROWS(parse_config("function zz\n"), FormatError);
    // shift default by entry size: 2 for 1/2-byte entries, 0 for 4-byte byte offsets
    AnalysisConfig d = parse_config("jumptable br=1 table=2 entry=1 count=1\n"
                                    "jumptable br=1 table=2 entry=2 count=1\n"
                                    "jumptable br=1 table=2 entry=4 count=1\n"
                                    "jumptable br=1 table=2 entry=4 count=1 shift=2\n");
    CHECK_EQ(d.jump_table_overrides[0].shift, 2);
    CHECK_EQ(d.jump_table_overrides[1].shift, 2);
    CHECK_EQ(d.jump_table_overrides[2].shift, 0);
    CHECK_EQ(d.jump_table_overrides[3].shift, 2);
    CHECK_THROWS(parse_config("jumptable br=1 table=2 entry=4 count=1 shift=4\n"), FormatError);
}

TEST(disable_gap_scan_leaves_gaps_unclaimed) {
    synth::Builder bd;
    bd.text_words = {RET, RET, NOP, NOP};
    bd.fdes = {{bd.text_off(0), bd.text_off(0) + 4}};
    AnalysisConfig cfg;
    cfg.scan_gaps = false;
    Built t = analyze_builder(bd, cfg);
    CHECK(!t.res.is_entry(t.va(bd.text_off(1))));
    AnalysisConfig on;
    synth::Builder bd2 = bd;
    Built t2 = analyze_builder(bd2, on);
    CHECK(t2.res.is_entry(t2.va(bd.text_off(1))));
}

TEST(jump_table_index_from_unmodelled_instruction) {
    // The index comes from an instruction the slicer does not model (here mul; in real code
    // e.g. a load through a computed address). It must stay an opaque value instead of
    // hiding the constant table around it:
    //   mul w8,w1,w2; cmp w8,#3; b.hi def; adrp x9,tbl; add x9,x9,lo12; ldrsw x8,[x9,x8,lsl #2];
    //   add x8,x8,x9; br x8; 4 cases; def
    synth::Builder bd;
    uint64_t f = bd.text_off(0);
    std::vector<uint32_t> w(13, RET);
    uint64_t tbl = bd.alloc_ro(16, 4);
    int64_t pc3 = static_cast<int64_t>(bd.text_off(3));
    w[0] = mul_w(8, 1, 2);
    w[1] = cmp_imm(false, 8, 3);
    w[2] = bcond(8, static_cast<int64_t>(bd.text_off(12)) - static_cast<int64_t>(bd.text_off(2)));  // b.hi default
    w[3] = adrp(9, ((static_cast<int64_t>(tbl) & ~0xFFF) - (pc3 & ~0xFFF)) >> 12);
    w[4] = add_imm(true, 9, 9, static_cast<uint16_t>(tbl & 0xFFF));
    w[5] = ldr_reg(2, 2, 8, 9, 8, 3, true);  // ldrsw x8, [x9, x8, lsl #2]
    w[6] = add_reg(true, 8, 8, 9, 0);
    w[7] = br(8);
    w[8] = movz(false, 0, 1); w[9] = movz(false, 0, 2); w[10] = movz(false, 0, 3); w[11] = movz(false, 0, 4);
    std::vector<uint8_t> entries;
    for (int k = 8; k < 12; ++k)
        synth::app32(entries, static_cast<uint32_t>(static_cast<int32_t>(static_cast<int64_t>(bd.text_off(k)) - static_cast<int64_t>(tbl))));
    bd.write(tbl, entries);
    bd.text_words = w;
    bd.fdes = {{f, f + 13 * 4}};
    Built t = analyze_builder(bd);
    const Function* F = t.res.function_at(t.va(f));
    CHECK(F && F->jump_tables.size() == 1);
    if (!F || F->jump_tables.empty()) return;
    const JumpTable& jt = F->jump_tables[0];
    CHECK(jt.bound_known);
    CHECK_EQ(jt.count, 4u);
    CHECK_EQ(jt.targets[3], t.va(bd.text_off(11)));
    CHECK(!F->has_unresolved_indirect);
}

TEST(jump_table_across_fp_and_unrelated_instructions) {
    // Seen in the game (Route 1): FP code and smaddl between the bound check and the table load.
    // FP/SIMD instructions write V registers, not the general registers with the same number,
    // and stores write no register; the slicer used to treat both as writes of Xn and lost x8/x9:
    //   0 sub w8,w0,#3; 1 cmp w8,#3; 2 b.hi def; 3 adrp x9,tbl; 4 add x9,x9,lo12;
    //   5 fmul d8,d0,d1; 6 ldr s9,[x2]; 7 smaddl x10,w1,w2,x3; 8 fmov s9,w1; 9 str d8,[sp,#16];
    //   10 str x8,[sp,#8]; 11 ld1 {v8.4s},[x2],#16; 12 fcmp s0,s1;
    //   13 ldrsw x8,[x9,x8,lsl #2]; 14 add x8,x8,x9; 15 br x8; 16-19 cases; 20 def: ret
    synth::Builder bd;
    uint64_t f = bd.text_off(0);
    std::vector<uint32_t> w(21, RET);
    uint64_t tbl = bd.alloc_ro(16, 4);
    auto rel = [&](int from, int to) { return static_cast<int64_t>(bd.text_off(to)) - static_cast<int64_t>(bd.text_off(from)); };
    int64_t pc3 = static_cast<int64_t>(bd.text_off(3));
    w[0] = sub_imm(false, 8, 0, 3);
    w[1] = cmp_imm(false, 8, 3);
    w[2] = bcond(8, rel(2, 20));  // b.hi default
    w[3] = adrp(9, ((static_cast<int64_t>(tbl) & ~0xFFF) - (pc3 & ~0xFFF)) >> 12);
    w[4] = add_imm(true, 9, 9, static_cast<uint16_t>(tbl & 0xFFF));
    w[5] = 0x1E610808;   // fmul d8, d0, d1
    w[6] = 0xBD400049;   // ldr s9, [x2]
    w[7] = 0x9B220C2A;   // smaddl x10, w1, w2, x3
    w[8] = 0x1E270029;   // fmov s9, w1
    w[9] = 0xFD000BE8;   // str d8, [sp, #16]
    w[10] = 0xF90007E8;  // str x8, [sp, #8]
    w[11] = 0x4CDF7848;  // ld1 {v8.4s}, [x2], #16
    w[12] = 0x1E212000;  // fcmp s0, s1
    w[13] = ldr_reg(2, 2, 8, 9, 8, 3, true);  // ldrsw x8, [x9, x8, lsl #2]
    w[14] = add_reg(true, 8, 8, 9, 0);
    w[15] = br(8);
    for (int k = 16; k < 20; ++k) w[k] = movz(false, 0, static_cast<uint16_t>(k));
    std::vector<uint8_t> entries;
    for (int k = 16; k < 20; ++k)
        synth::app32(entries, static_cast<uint32_t>(static_cast<int32_t>(static_cast<int64_t>(bd.text_off(k)) - static_cast<int64_t>(tbl))));
    bd.write(tbl, entries);
    bd.text_words = w;
    bd.fdes = {{f, f + 21 * 4}};
    Built t = analyze_builder(bd);
    const Function* F = t.res.function_at(t.va(f));
    CHECK(F && F->jump_tables.size() == 1);
    if (!F || F->jump_tables.empty()) return;
    const JumpTable& jt = F->jump_tables[0];
    CHECK(jt.bound_known);
    CHECK_EQ(jt.count, 4u);
    CHECK(jt.confidence == Confidence::High);
    CHECK_EQ(jt.targets[0], t.va(bd.text_off(16)));
    CHECK_EQ(jt.targets[3], t.va(bd.text_off(19)));
    CHECK(!F->has_unresolved_indirect);
}

TEST(jump_table_bound_on_the_value_before_the_offset) {
    // The bound is checked on the switch value, the table is indexed with value - 3:
    //   0 cmp w0,#6; 1 b.hi def; 2 sub w8,w0,#3; 3 adrp x9,tbl; 4 add x9,x9,lo12;
    //   5 ldrsw x8,[x9,x8,lsl #2]; 6 add x8,x8,x9; 7 br x8; 8-11 cases; 12 def: ret
    // Only values 3..6 reach the table (the compiler handled 0..2 before), so it has 7 - 3 = 4
    // entries; without that the scan ran on into the next table.
    synth::Builder bd;
    uint64_t f = bd.text_off(0);
    std::vector<uint32_t> w(13, RET);
    uint64_t tbl = bd.alloc_ro(32, 4);
    auto rel = [&](int from, int to) { return static_cast<int64_t>(bd.text_off(to)) - static_cast<int64_t>(bd.text_off(from)); };
    int64_t pc3 = static_cast<int64_t>(bd.text_off(3));
    w[0] = cmp_imm(false, 0, 6);
    w[1] = bcond(8, rel(1, 12));  // b.hi default
    w[2] = sub_imm(false, 8, 0, 3);
    w[3] = adrp(9, ((static_cast<int64_t>(tbl) & ~0xFFF) - (pc3 & ~0xFFF)) >> 12);
    w[4] = add_imm(true, 9, 9, static_cast<uint16_t>(tbl & 0xFFF));
    w[5] = ldr_reg(2, 2, 8, 9, 8, 3, true);  // ldrsw x8, [x9, x8, lsl #2]
    w[6] = add_reg(true, 8, 8, 9, 0);
    w[7] = br(8);
    for (int k = 8; k < 12; ++k) w[k] = movz(false, 0, static_cast<uint16_t>(k));
    std::vector<uint8_t> entries;
    for (int k = 8; k < 12; ++k)
        synth::app32(entries, static_cast<uint32_t>(static_cast<int32_t>(static_cast<int64_t>(bd.text_off(k)) - static_cast<int64_t>(tbl))));
    // Behind the table: more valid-looking entries (the next table) that must not be counted.
    for (int k = 8; k < 12; ++k)
        synth::app32(entries, static_cast<uint32_t>(static_cast<int32_t>(static_cast<int64_t>(bd.text_off(8)) - static_cast<int64_t>(tbl))));
    bd.write(tbl, entries);
    bd.text_words = w;
    bd.fdes = {{f, f + 13 * 4}};
    Built t = analyze_builder(bd);
    const Function* F = t.res.function_at(t.va(f));
    CHECK(F && F->jump_tables.size() == 1);
    if (!F || F->jump_tables.empty()) return;
    const JumpTable& jt = F->jump_tables[0];
    CHECK(jt.bound_known);
    CHECK_EQ(jt.count, 4u);
    CHECK_EQ(jt.table_va, t.va(tbl));
    CHECK_EQ(jt.targets[3], t.va(bd.text_off(11)));
}

TEST(jump_table_offset_folded_into_the_table_address) {
    // The bound is checked on value - 3, but the load uses the value itself and the compiler
    // moved the table address down by 3 entries instead:
    //   0 sub w8,w0,#3; 1 cmp w8,#3; 2 b.hi def; 3 adrp x9,tbl-12; 4 add x9,x9,lo12;
    //   5 ldrsw x8,[x9,w0,uxtw #2]; 6 add x8,x8,x9; 7 br x8; 8-11 cases; 12 def: ret
    // Entries 3..6 from x9 are the table, i.e. 4 entries starting at tbl.
    synth::Builder bd;
    uint64_t f = bd.text_off(0);
    std::vector<uint32_t> w(13, RET);
    uint64_t tbl = bd.alloc_ro(32, 4) + 16;
    uint64_t base = tbl - 12;
    auto rel = [&](int from, int to) { return static_cast<int64_t>(bd.text_off(to)) - static_cast<int64_t>(bd.text_off(from)); };
    int64_t pc3 = static_cast<int64_t>(bd.text_off(3));
    w[0] = sub_imm(false, 8, 0, 3);
    w[1] = cmp_imm(false, 8, 3);
    w[2] = bcond(8, rel(2, 12));  // b.hi default
    w[3] = adrp(9, ((static_cast<int64_t>(base) & ~0xFFF) - (pc3 & ~0xFFF)) >> 12);
    w[4] = add_imm(true, 9, 9, static_cast<uint16_t>(base & 0xFFF));
    w[5] = 0xB8A05928;  // ldrsw x8, [x9, w0, uxtw #2]
    w[6] = add_reg(true, 8, 8, 9, 0);
    w[7] = br(8);
    for (int k = 8; k < 12; ++k) w[k] = movz(false, 0, static_cast<uint16_t>(k));
    std::vector<uint8_t> entries;
    for (int k = 8; k < 12; ++k)
        synth::app32(entries, static_cast<uint32_t>(static_cast<int32_t>(static_cast<int64_t>(bd.text_off(k)) - static_cast<int64_t>(base))));
    bd.write(tbl, entries);
    bd.text_words = w;
    bd.fdes = {{f, f + 13 * 4}};
    Built t = analyze_builder(bd);
    const Function* F = t.res.function_at(t.va(f));
    CHECK(F && F->jump_tables.size() == 1);
    if (!F || F->jump_tables.empty()) return;
    const JumpTable& jt = F->jump_tables[0];
    CHECK(jt.bound_known);
    CHECK_EQ(jt.count, 4u);
    CHECK_EQ(jt.table_va, t.va(tbl));
    CHECK_EQ(jt.base_va, t.va(base));
    CHECK_EQ(jt.targets[0], t.va(bd.text_off(8)));
    CHECK_EQ(jt.targets[3], t.va(bd.text_off(11)));
}

TEST(jump_table_base_survives_literal_load_in_the_loop) {
    // sdk (PC check of the FP fix): the base lives in x25 from before the loop, the index comes
    // from an instruction the slicer does not model. The jt decoder leaves LDR (literal) to
    // invalidate_other, whose fallback for that class also killed the "Rt2"/"Rs" fields; in a
    // literal load those bits are part of imm19 and here spell 25, so x25 lost its constant:
    //   0 adrp x25,tbl; 1 add x25,x25,lo12; 2 loop: ldr w24,lit (imm19 bits 20:16 = 25);
    //   3 cmp w24,#12; 4 b.hi next; 5 mov w9,w24; 6 ldrsw x10,[x25,x9,lsl #2]; 7 ldr x9,[x20];
    //   8 add x10,x10,x25; 9 br x10; 10-22 cases; 23 next: b loop
    synth::Builder bd;
    uint64_t f = bd.text_off(0);
    std::vector<uint32_t> w(24, RET);
    uint64_t tbl = bd.alloc_ro(13 * 4, 4);
    auto rel = [&](int from, int to) { return static_cast<int64_t>(bd.text_off(to)) - static_cast<int64_t>(bd.text_off(from)); };
    int64_t pc0 = static_cast<int64_t>(bd.text_off(0));
    w[0] = adrp(25, ((static_cast<int64_t>(tbl) & ~0xFFF) - (pc0 & ~0xFFF)) >> 12);
    w[1] = add_imm(true, 25, 25, static_cast<uint16_t>(tbl & 0xFFF));
    w[2] = 0x18190018;  // ldr w24, #0x32000 (imm19 = 25 << 11)
    w[3] = cmp_imm(false, 24, 12);
    w[4] = bcond(8, rel(4, 23));  // b.hi next
    w[5] = mov_reg(false, 9, 24);
    w[6] = ldr_reg(2, 2, 10, 25, 9, 3, true);  // ldrsw x10, [x25, x9, lsl #2]
    w[7] = ldr_x_imm(9, 20, 0);
    w[8] = add_reg(true, 10, 10, 25, 0);
    w[9] = br(10);
    for (int k = 10; k < 23; ++k) w[k] = movz(false, 0, static_cast<uint16_t>(k));
    w[23] = b(rel(23, 2));
    std::vector<uint8_t> entries;
    for (int k = 10; k < 23; ++k)
        synth::app32(entries, static_cast<uint32_t>(static_cast<int32_t>(static_cast<int64_t>(bd.text_off(k)) - static_cast<int64_t>(tbl))));
    bd.write(tbl, entries);
    bd.text_words = w;
    bd.fdes = {{f, f + 24 * 4}};
    Built t = analyze_builder(bd);
    const Function* F = t.res.function_at(t.va(f));
    CHECK(F && F->jump_tables.size() == 1 && !F->has_unresolved_indirect);
    if (!F || F->jump_tables.empty()) return;
    CHECK(F->jump_tables[0].bound_known);
    CHECK_EQ(F->jump_tables[0].count, 13u);
    CHECK_EQ(F->jump_tables[0].targets[12], t.va(bd.text_off(22)));
}

TEST(jump_table_state_machine_with_constant_initial_state) {
    // sdk (PC trace of 0xa388190): a switch inside a loop over a state variable. Before the
    // table is resolved only the path from the function entry is known, and there the state is
    // a constant; the cases that set other states and jump back are reached only through the
    // table. Folding that constant into the index turned the table load into one fixed entry:
    //   0 movz w24,#2; 1 loop: cmp w24,#12; 2 b.hi exit; 3 adrp x25,tbl; 4 add x25,x25,lo12;
    //   5 mov w9,w24; 6 ldrsw x10,[x25,x9,lsl #2]; 7 ldr x9,[x20]; 8 add x10,x10,x25; 9 br x10;
    //   10+2k case k: movz w24,#k+1; b loop  (k = 0..12); 36 exit: ret
    synth::Builder bd;
    uint64_t f = bd.text_off(0);
    std::vector<uint32_t> w(37, RET);
    uint64_t tbl = bd.alloc_ro(13 * 4, 4);
    auto rel = [&](int from, int to) { return static_cast<int64_t>(bd.text_off(to)) - static_cast<int64_t>(bd.text_off(from)); };
    int64_t pc3 = static_cast<int64_t>(bd.text_off(3));
    w[0] = movz(false, 24, 2);
    w[1] = cmp_imm(false, 24, 12);
    w[2] = bcond(8, rel(2, 36));  // b.hi exit
    w[3] = adrp(25, ((static_cast<int64_t>(tbl) & ~0xFFF) - (pc3 & ~0xFFF)) >> 12);
    w[4] = add_imm(true, 25, 25, static_cast<uint16_t>(tbl & 0xFFF));
    w[5] = mov_reg(false, 9, 24);
    w[6] = ldr_reg(2, 2, 10, 25, 9, 3, true);  // ldrsw x10, [x25, x9, lsl #2]
    w[7] = ldr_x_imm(9, 20, 0);
    w[8] = add_reg(true, 10, 10, 25, 0);
    w[9] = br(10);
    std::vector<uint8_t> entries;
    for (int k = 0; k < 13; ++k) {
        w[10 + 2 * k] = movz(false, 24, static_cast<uint16_t>(k + 1));
        w[11 + 2 * k] = b(rel(11 + 2 * k, 1));
        synth::app32(entries, static_cast<uint32_t>(static_cast<int32_t>(static_cast<int64_t>(bd.text_off(10 + 2 * k)) - static_cast<int64_t>(tbl))));
    }
    bd.write(tbl, entries);
    bd.text_words = w;
    bd.fdes = {{f, f + 37 * 4}};
    Built t = analyze_builder(bd);
    const Function* F = t.res.function_at(t.va(f));
    CHECK(F && F->jump_tables.size() == 1 && !F->has_unresolved_indirect);
    if (!F || F->jump_tables.empty()) return;
    CHECK(F->jump_tables[0].bound_known);
    CHECK_EQ(F->jump_tables[0].count, 13u);
    CHECK_EQ(F->jump_tables[0].targets[12], t.va(bd.text_off(34)));
    CHECK(block_at(*F, t.va(bd.text_off(34))) != nullptr);
}

TEST(br_through_literal_pool_is_a_direct_tail_call) {
    // ldr x16, lit; br x16 with the literal holding another function's address. The jt decoder
    // never matched LDR (literal) (its condition tested bit 28 against itself), so such a BR
    // stayed unresolved:
    //   0 f: ldr x16, lit; 1 br x16; 2 g: ret; 3 pad; 4-5 lit: .quad g
    synth::Builder bd;
    uint64_t f = bd.text_off(0), g = bd.text_off(2);
    std::vector<uint32_t> w = {ldr_lit(1, 16, static_cast<int64_t>(bd.text_off(4)) - static_cast<int64_t>(bd.text_off(0))),
                               br(16), RET, NOP, 0, 0};
    bd.text_words = w;
    bd.fdes = {{f, f + 8}, {g, g + 4}};
    Built probe = analyze_builder(bd);
    synth::Builder bd2 = bd;
    std::vector<uint32_t> w2 = w;
    const uint64_t gva = probe.va(g);
    w2[4] = static_cast<uint32_t>(gva);
    w2[5] = static_cast<uint32_t>(gva >> 32);
    bd2.text_words = w2;
    Built t = analyze_builder(bd2);
    const Function* F = t.res.function_at(t.va(f));
    CHECK(F && !F->has_unresolved_indirect);
    if (!F) return;
    bool tail = false;
    for (const auto& blk : F->blocks)
        for (const auto& c : blk.calls) tail |= c.tail && c.target == t.va(g);
    CHECK(tail);
    CHECK(!has_diag(t.res, Diagnostic::Kind::UnresolvedIndirectJump));
}

TEST(jump_table_override_with_wrong_shift_is_rejected) {
    // ldrsw + add table (byte offsets): the override without shift= uses 0 and is applied;
    // shift=2 multiplies the offsets by 4, the targets leave .text and the override is
    // rejected with a diagnostic (the heuristic still resolves the BR).
    //   0 cmp w0,#3; 1 b.hi def; 2 adrp x9,tbl; 3 add x9,x9,lo12; 4 ldrsw x8,[x9,x0,lsl #2];
    //   5 add x8,x8,x9; 6 br x8; 7-10 cases; 11 def: ret
    synth::Builder bd;
    uint64_t f = bd.text_off(0);
    std::vector<uint32_t> w(12, RET);
    uint64_t tbl = bd.alloc_ro(16, 4);
    int64_t pc2 = static_cast<int64_t>(bd.text_off(2));
    w[0] = cmp_imm(false, 0, 3);
    w[1] = bcond(8, static_cast<int64_t>(bd.text_off(11)) - static_cast<int64_t>(bd.text_off(1)));
    w[2] = adrp(9, ((static_cast<int64_t>(tbl) & ~0xFFF) - (pc2 & ~0xFFF)) >> 12);
    w[3] = add_imm(true, 9, 9, static_cast<uint16_t>(tbl & 0xFFF));
    w[4] = ldr_reg(2, 2, 8, 9, 0, 3, true);  // ldrsw x8, [x9, x0, lsl #2]
    w[5] = add_reg(true, 8, 8, 9, 0);
    w[6] = br(8);
    for (int k = 7; k < 11; ++k) w[k] = movz(false, 0, static_cast<uint16_t>(k));
    std::vector<uint8_t> entries;
    for (int k = 7; k < 11; ++k)
        synth::app32(entries, static_cast<uint32_t>(static_cast<int32_t>(static_cast<int64_t>(bd.text_off(k)) - static_cast<int64_t>(tbl))));
    bd.write(tbl, entries);
    bd.text_words = w;
    bd.fdes = {{f, f + 12 * 4}};
    synth::Builder probe = bd;
    Built p = analyze_builder(probe);
    char line[200];
    auto run = [&](const char* extra) {
        std::snprintf(line, sizeof line, "jumptable br=0x%llx table=0x%llx entry=4 count=4 signed%s\n",
                      (unsigned long long)p.va(bd.text_off(6)), (unsigned long long)p.va(tbl), extra);
        synth::Builder copy = bd;
        return analyze_builder(copy, parse_config(line));
    };
    Built good = run("");
    const Function* G = good.res.function_at(good.va(f));
    CHECK(G && G->jump_tables.size() == 1 && G->jump_tables[0].count == 4);
    if (G && !G->jump_tables.empty()) CHECK_EQ(G->jump_tables[0].targets[3], good.va(bd.text_off(10)));
    CHECK(!has_diag(good.res, Diagnostic::Kind::JumpTableOverrideRejected));
    CHECK(!has_diag(good.res, Diagnostic::Kind::JumpTableOverrideOutsideFunction));
    Built bad = run(" shift=2");
    CHECK(has_diag(bad.res, Diagnostic::Kind::JumpTableOverrideRejected));
    CHECK(is_config_diagnostic(Diagnostic::Kind::JumpTableOverrideRejected));
    const Function* B = bad.res.function_at(bad.va(f));
    CHECK(B && B->jump_tables.size() == 1 && !B->has_unresolved_indirect);
    if (B && !B->jump_tables.empty()) CHECK_EQ(B->jump_tables[0].targets[3], bad.va(bd.text_off(10)));
}

TEST(jump_table_base_set_before_the_loop_head) {
    // The table address is loaded once in front of a loop and the BR sits inside it (sdk:
    // strzcmp16), so the unique-predecessor chain stops at the loop head before the ADRP/ADD.
    // A register written only by that ADRP/ADD pair in the whole function is a constant:
    //   0 adrp x9,tbl; 1 add x9,x9,lo12; 2 loop: mul w8,w1,w2; 3 cmp w8,#3; 4 b.hi next;
    //   5 ldrsw x8,[x9,x8,lsl #2]; 6 add x8,x8,x9; 7 br x8; 8-11 cases: ret;
    //   12 next: movz w0,#5; 13 b loop
    synth::Builder bd;
    uint64_t f = bd.text_off(0);
    std::vector<uint32_t> w(14, RET);
    uint64_t tbl = bd.alloc_ro(16, 4);
    int64_t pc0 = static_cast<int64_t>(bd.text_off(0));
    auto rel = [&](int from, int to) { return static_cast<int64_t>(bd.text_off(to)) - static_cast<int64_t>(bd.text_off(from)); };
    w[0] = adrp(9, ((static_cast<int64_t>(tbl) & ~0xFFF) - (pc0 & ~0xFFF)) >> 12);
    w[1] = add_imm(true, 9, 9, static_cast<uint16_t>(tbl & 0xFFF));
    w[2] = mul_w(8, 1, 2);
    w[3] = cmp_imm(false, 8, 3);
    w[4] = bcond(8, rel(4, 12));  // b.hi next
    w[5] = ldr_reg(2, 2, 8, 9, 8, 3, true);  // ldrsw x8, [x9, x8, lsl #2]
    w[6] = add_reg(true, 8, 8, 9, 0);
    w[7] = br(8);
    w[12] = movz(false, 0, 5);
    w[13] = b(rel(13, 2));
    std::vector<uint8_t> entries;
    for (int k = 8; k < 12; ++k)
        synth::app32(entries, static_cast<uint32_t>(static_cast<int32_t>(static_cast<int64_t>(bd.text_off(k)) - static_cast<int64_t>(tbl))));
    bd.write(tbl, entries);
    bd.text_words = w;
    bd.fdes = {{f, f + 14 * 4}};
    Built t = analyze_builder(bd);
    const Function* F = t.res.function_at(t.va(f));
    CHECK(F && F->jump_tables.size() == 1);
    if (!F || F->jump_tables.empty()) return;
    const JumpTable& jt = F->jump_tables[0];
    CHECK(jt.bound_known);
    CHECK_EQ(jt.count, 4u);
    CHECK_EQ(jt.targets[0], t.va(bd.text_off(8)));
    CHECK_EQ(jt.targets[3], t.va(bd.text_off(11)));
    CHECK(!F->has_unresolved_indirect);
}

TEST(jump_table_base_in_callee_saved_register_across_calls) {
    // main: the base lives in x23, saved in the prologue, kept across a call inside the loop and
    // restored in an epilogue. Neither the STP nor the SP-based LDP counts as a write.
    //   0 stp x24,x23,[sp,#-16]!; 1 adrp x23,tbl; 2 add x23,x23,lo12; 3 loop: bl helper;
    //   4 cmp w0,#3; 5 b.hi next; 6 ldrsw x8,[x23,x0,lsl #2]; 7 add x8,x8,x23; 8 br x8;
    //   9-11 cases: ret; 12 case 3: ldp x24,x23,[sp],#16; 13 ret; 14 next: b loop; 15 helper: ret
    synth::Builder bd;
    uint64_t f = bd.text_off(0), helper = bd.text_off(15);
    std::vector<uint32_t> w(16, RET);
    uint64_t tbl = bd.alloc_ro(16, 4);
    int64_t pc1 = static_cast<int64_t>(bd.text_off(1));
    auto rel = [&](int from, int to) { return static_cast<int64_t>(bd.text_off(to)) - static_cast<int64_t>(bd.text_off(from)); };
    w[0] = 0xA9800000u | (0x7Eu << 15) | (23u << 10) | (31u << 5) | 24u;  // stp x24, x23, [sp, #-16]!
    w[1] = adrp(23, ((static_cast<int64_t>(tbl) & ~0xFFF) - (pc1 & ~0xFFF)) >> 12);
    w[2] = add_imm(true, 23, 23, static_cast<uint16_t>(tbl & 0xFFF));
    w[3] = bl(rel(3, 15));
    w[4] = cmp_imm(false, 0, 3);
    w[5] = bcond(8, rel(5, 14));
    w[6] = ldr_reg(2, 2, 8, 23, 0, 3, true);  // ldrsw x8, [x23, x0, lsl #2]
    w[7] = add_reg(true, 8, 8, 23, 0);
    w[8] = br(8);
    w[12] = 0xA8C00000u | (0x2u << 15) | (23u << 10) | (31u << 5) | 24u;  // ldp x24, x23, [sp], #16
    w[14] = b(rel(14, 3));
    std::vector<uint8_t> entries;
    for (int k = 9; k < 13; ++k)
        synth::app32(entries, static_cast<uint32_t>(static_cast<int32_t>(static_cast<int64_t>(bd.text_off(k)) - static_cast<int64_t>(tbl))));
    bd.write(tbl, entries);
    bd.text_words = w;
    bd.fdes = {{f, f + 15 * 4}, {helper, helper + 4}};
    Built t = analyze_builder(bd);
    const Function* F = t.res.function_at(t.va(f));
    CHECK(F && F->jump_tables.size() == 1);
    if (!F || F->jump_tables.empty()) return;
    CHECK(F->jump_tables[0].bound_known);
    CHECK_EQ(F->jump_tables[0].targets[3], t.va(bd.text_off(12)));
}

// Caller-saved base register (x9) defined after a call and used in a loop: the call in front of
// the definition does not clobber it (sdk get_abbreviated_name keeps its table base in x18);
// with the call inside the loop, behind the definition, the base is not a constant.
Built table_after_call(bool call_in_loop) {
    //   0 bl helper (or nop); 1 adrp x9,tbl; 2 add x9,x9,lo12; 3 loop: mul w8,w1,w2 (or bl helper);
    //   4 cmp w8,#3; 5 b.hi next; 6 ldrsw x8,[x9,x8,lsl #2]; 7 add x8,x8,x9; 8 br x8;
    //   9-12 cases: ret; 13 next: b loop; 14 helper: ret
    synth::Builder bd;
    uint64_t f = bd.text_off(0), helper = bd.text_off(14);
    std::vector<uint32_t> w(15, RET);
    uint64_t tbl = bd.alloc_ro(16, 4);
    int64_t pc1 = static_cast<int64_t>(bd.text_off(1));
    auto rel = [&](int from, int to) { return static_cast<int64_t>(bd.text_off(to)) - static_cast<int64_t>(bd.text_off(from)); };
    w[0] = call_in_loop ? NOP : bl(rel(0, 14));
    w[1] = adrp(9, ((static_cast<int64_t>(tbl) & ~0xFFF) - (pc1 & ~0xFFF)) >> 12);
    w[2] = add_imm(true, 9, 9, static_cast<uint16_t>(tbl & 0xFFF));
    w[3] = call_in_loop ? bl(rel(3, 14)) : mul_w(8, 1, 2);
    w[4] = cmp_imm(false, 8, 3);
    w[5] = bcond(8, rel(5, 13));
    w[6] = ldr_reg(2, 2, 8, 9, 8, 3, true);
    w[7] = add_reg(true, 8, 8, 9, 0);
    w[8] = br(8);
    w[13] = b(rel(13, 3));
    std::vector<uint8_t> entries;
    for (int k = 9; k < 13; ++k)
        synth::app32(entries, static_cast<uint32_t>(static_cast<int32_t>(static_cast<int64_t>(bd.text_off(k)) - static_cast<int64_t>(tbl))));
    bd.write(tbl, entries);
    bd.text_words = w;
    bd.fdes = {{f, f + 14 * 4}, {helper, helper + 4}};
    return analyze_builder(bd);
}

TEST(jump_table_base_defined_after_a_call) {
    auto has_table = [](const Built& b) {
        for (const auto& fn : b.res.functions)
            if (!fn.jump_tables.empty()) return true;
        return false;
    };
    CHECK(has_table(table_after_call(false)));
    CHECK(!has_table(table_after_call(true)));
}

TEST(jump_table_two_loops_reuse_the_base_register) {
    // Two table loops in one function, each loading its own base into x9 in front of it
    // (sdk get_abbreviated_name): every BR takes the nearest definition before it.
    //   0 adrp x9,A; 1 add; 2 loopA: mul w8,w1,w2; 3 cmp w8,#3; 4 b.hi 13; 5 ldrsw; 6 add; 7 br x8;
    //   8-11 cases A; 12 nop; 13 cbz w3, loopA;
    //   14 adrp x9,B; 15 add; 16 loopB: mul; 17 cmp; 18 b.hi 27; 19 ldrsw; 20 add; 21 br x8;
    //   22-25 cases B; 26 nop; 27 cbz w3, loopB; 28 ret
    synth::Builder bd;
    uint64_t f = bd.text_off(0);
    std::vector<uint32_t> w(29, RET);
    const uint64_t tbl[2] = {bd.alloc_ro(16, 4), bd.alloc_ro(16, 4)};
    auto rel = [&](int from, int to) { return static_cast<int64_t>(bd.text_off(to)) - static_cast<int64_t>(bd.text_off(from)); };
    for (int t = 0; t < 2; ++t) {
        const int o = 14 * t;
        const int64_t pc = static_cast<int64_t>(bd.text_off(o));
        w[o + 0] = adrp(9, ((static_cast<int64_t>(tbl[t]) & ~0xFFF) - (pc & ~0xFFF)) >> 12);
        w[o + 1] = add_imm(true, 9, 9, static_cast<uint16_t>(tbl[t] & 0xFFF));
        w[o + 2] = mul_w(8, 1, 2);
        w[o + 3] = cmp_imm(false, 8, 3);
        w[o + 4] = bcond(8, rel(o + 4, o + 13));
        w[o + 5] = ldr_reg(2, 2, 8, 9, 8, 3, true);
        w[o + 6] = add_reg(true, 8, 8, 9, 0);
        w[o + 7] = br(8);
        w[o + 12] = NOP;
        w[o + 13] = cbz(false, 3, rel(o + 13, o + 2));
        std::vector<uint8_t> entries;
        for (int k = o + 8; k < o + 12; ++k)
            synth::app32(entries, static_cast<uint32_t>(static_cast<int32_t>(static_cast<int64_t>(bd.text_off(k)) - static_cast<int64_t>(tbl[t]))));
        bd.write(tbl[t], entries);
    }
    bd.text_words = w;
    bd.fdes = {{f, f + 29 * 4}};
    Built t = analyze_builder(bd);
    const Function* F = t.res.function_at(t.va(f));
    CHECK(F && F->jump_tables.size() == 2 && !F->has_unresolved_indirect);
    if (!F || F->jump_tables.size() != 2) return;
    for (const JumpTable& jt : F->jump_tables) {
        const int o = jt.br_pc == t.va(bd.text_off(7)) ? 0 : 14;
        CHECK_EQ(jt.targets[0], t.va(bd.text_off(o + 8)));
        CHECK_EQ(jt.targets[3], t.va(bd.text_off(o + 11)));
    }
}

TEST(jump_table_base_reused_after_the_loop) {
    // The base register gets another value only after the loop (sdk make_graymap); no branch
    // behind that write leads back into the loop, so the base stays a constant for the BR.
    //   0 adrp x9,tbl; 1 add; 2 loop: mul w8,w1,w2; 3 cmp w8,#3; 4 b.hi 13; 5 ldrsw; 6 add; 7 br x8;
    //   8-11 cases: ret; 12 nop; 13 cbz w3, loop; 14 mov x9, x1; 15 bl helper; 16 ret; 17 helper: ret
    //   (the call clobbers x9 too, but also only behind the loop)
    synth::Builder bd;
    uint64_t f = bd.text_off(0);
    std::vector<uint32_t> w(18, RET);
    uint64_t tbl = bd.alloc_ro(16, 4);
    int64_t pc0 = static_cast<int64_t>(bd.text_off(0));
    auto rel = [&](int from, int to) { return static_cast<int64_t>(bd.text_off(to)) - static_cast<int64_t>(bd.text_off(from)); };
    w[0] = adrp(9, ((static_cast<int64_t>(tbl) & ~0xFFF) - (pc0 & ~0xFFF)) >> 12);
    w[1] = add_imm(true, 9, 9, static_cast<uint16_t>(tbl & 0xFFF));
    w[2] = mul_w(8, 1, 2);
    w[3] = cmp_imm(false, 8, 3);
    w[4] = bcond(8, rel(4, 13));
    w[5] = ldr_reg(2, 2, 8, 9, 8, 3, true);
    w[6] = add_reg(true, 8, 8, 9, 0);
    w[7] = br(8);
    w[12] = NOP;
    w[13] = cbz(false, 3, rel(13, 2));
    w[14] = mov_reg(true, 9, 1);
    w[15] = bl(rel(15, 17));
    std::vector<uint8_t> entries;
    for (int k = 8; k < 12; ++k)
        synth::app32(entries, static_cast<uint32_t>(static_cast<int32_t>(static_cast<int64_t>(bd.text_off(k)) - static_cast<int64_t>(tbl))));
    bd.write(tbl, entries);
    bd.text_words = w;
    bd.fdes = {{f, f + 17 * 4}, {bd.text_off(17), bd.text_off(18)}};
    Built t = analyze_builder(bd);
    const Function* F = t.res.function_at(t.va(f));
    CHECK(F && F->jump_tables.size() == 1 && !F->has_unresolved_indirect);
}

TEST(jump_table_base_written_twice_stays_unresolved) {
    // Same shape, but the out-of-range path overwrites x9 before looping (12 add x9,x9,#4;
    // 13 b loop): not a constant.
    synth::Builder bd;
    uint64_t f = bd.text_off(0);
    std::vector<uint32_t> w(14, RET);
    uint64_t tbl = bd.alloc_ro(16, 4);
    int64_t pc0 = static_cast<int64_t>(bd.text_off(0));
    auto rel = [&](int from, int to) { return static_cast<int64_t>(bd.text_off(to)) - static_cast<int64_t>(bd.text_off(from)); };
    w[0] = adrp(9, ((static_cast<int64_t>(tbl) & ~0xFFF) - (pc0 & ~0xFFF)) >> 12);
    w[1] = add_imm(true, 9, 9, static_cast<uint16_t>(tbl & 0xFFF));
    w[2] = mul_w(8, 1, 2);
    w[3] = cmp_imm(false, 8, 3);
    w[4] = bcond(8, rel(4, 12));
    w[5] = ldr_reg(2, 2, 8, 9, 8, 3, true);
    w[6] = add_reg(true, 8, 8, 9, 0);
    w[7] = br(8);
    w[12] = add_imm(true, 9, 9, 4);
    w[13] = b(rel(13, 2));
    std::vector<uint8_t> entries;
    for (int k = 8; k < 12; ++k)
        synth::app32(entries, static_cast<uint32_t>(static_cast<int32_t>(static_cast<int64_t>(bd.text_off(k)) - static_cast<int64_t>(tbl))));
    bd.write(tbl, entries);
    bd.text_words = w;
    bd.fdes = {{f, f + 14 * 4}};
    Built t = analyze_builder(bd);
    const Function* F = t.res.function_at(t.va(f));
    CHECK(F && F->jump_tables.empty() && F->has_unresolved_indirect);
}

TEST(jump_table_resolved_when_last_work_item_is_known_block) {
    // The BR block is the taken side of the bound check (explored first); the fall-through
    // side loops back to the entry, so the last work item is an existing block. The pending
    // BR must still be resolved when the worklist drains.
    //   0 cmp w0,#1; 1 b.ls 3; 2 b 0; 3 adr x9,tbl; 4 ldrsw x10,[x9,w0,uxtw #2]; 5 add x9,x9,x10;
    //   6 br x9; 7 ret (case 0); 8 ret (case 1)
    synth::Builder bd;
    uint64_t f = bd.text_off(0);
    std::vector<uint32_t> w(9, RET);
    uint64_t tbl = bd.alloc_ro(8, 4);
    w[0] = cmp_imm(false, 0, 1);
    w[1] = bcond(9, static_cast<int64_t>(bd.text_off(3)) - static_cast<int64_t>(bd.text_off(1)));  // b.ls table
    w[2] = b(static_cast<int64_t>(bd.text_off(0)) - static_cast<int64_t>(bd.text_off(2)));
    w[3] = adr(9, static_cast<int64_t>(tbl) - static_cast<int64_t>(bd.text_off(3)));
    w[4] = ldrsw_uxtw_s(10, 9, 0);
    w[5] = add_reg(true, 9, 9, 10, 0);
    w[6] = br(9);
    std::vector<uint8_t> entries;
    for (int k : {7, 8})
        synth::app32(entries, static_cast<uint32_t>(static_cast<int32_t>(static_cast<int64_t>(bd.text_off(k)) - static_cast<int64_t>(tbl))));
    bd.write(tbl, entries);
    bd.text_words = w;
    bd.fdes = {{f, f + 9 * 4}};
    Built t = analyze_builder(bd);
    const Function* F = t.res.function_at(t.va(f));
    CHECK(F && F->jump_tables.size() == 1);
    if (!F) return;
    CHECK(!F->has_unresolved_indirect);
    CHECK(block_at(*F, t.va(bd.text_off(8))) != nullptr);
    CHECK(!has_diag(t.res, Diagnostic::Kind::UnresolvedIndirectJump));
}

int main(int argc, char** argv) { return tst::run(argc, argv); }
