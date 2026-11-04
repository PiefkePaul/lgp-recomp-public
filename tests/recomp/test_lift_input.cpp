// Analysis → lifter adapter (docs/design/p2-interfaces.md section 4a) on a
// synthetic module: a clang-style jump-table function and a caller that calls
// it and then tail-calls it.
#include <cstdio>
#include <cstdlib>
#include <string>

#include "a64_encode.hpp"
#include "check.hpp"
#include "lgp/recomp/cfg.hpp"
#include "lgp/recomp/lift_input.hpp"
#include "lgp/recomp/link.hpp"
#include "lgp/recomp/loader.hpp"
#include "synthetic_module.hpp"

namespace enc = ::a64;
namespace rc = lgp::recomp;

namespace {

bool contains(const std::string& s, const std::string& what) { return s.find(what) != std::string::npos; }

std::string hex(uint64_t v) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%llx", static_cast<unsigned long long>(v));
    return buf;
}

}  // namespace

TEST(adapter_copies_blocks_and_jump_tables_and_lifts) {
    synth::Builder bd;
    const uint64_t f = bd.text_off(0), g = bd.text_off(12);
    std::vector<uint32_t> w(15, enc::RET);
    const uint64_t tbl = bd.alloc_ro(4, 4);
    const int64_t pc2 = static_cast<int64_t>(bd.text_off(2)), pc3 = static_cast<int64_t>(bd.text_off(3));
    // f: cmp w0,#3; b.hi default; adr x9,tbl; adr x10,base; ldrb w11,[x9,w0,uxtw]; add x10,x10,x11,lsl #2; br x10
    w[0] = enc::cmp_imm(false, 0, 3);
    w[1] = enc::bcond(8, static_cast<int64_t>(bd.text_off(11)) - static_cast<int64_t>(bd.text_off(1)));
    w[2] = enc::adr(9, static_cast<int64_t>(tbl) - pc2);
    w[3] = enc::adr(10, static_cast<int64_t>(bd.text_off(7)) - pc3);
    w[4] = enc::ldrb_uxtw(11, 9, 0);
    w[5] = enc::add_reg(true, 10, 10, 11, 2);
    w[6] = enc::br(10);
    w[7] = enc::movz(false, 0, 10);
    w[8] = enc::movz(false, 0, 11);
    w[9] = enc::movz(false, 0, 12);
    w[10] = enc::movz(false, 0, 13);
    w[11] = enc::RET;
    // g: bl f; movz w1,#1; b f (tail call)
    w[12] = enc::bl(static_cast<int64_t>(f) - static_cast<int64_t>(g));
    w[13] = enc::movz(false, 1, 1);
    w[14] = enc::b(static_cast<int64_t>(f) - static_cast<int64_t>(bd.text_off(14)));
    bd.write(tbl, {0, 1, 2, 3});
    bd.text_words = w;
    bd.fdes = {{f, f + 12 * 4}, {g, g + 3 * 4}};

    std::vector<rc::Module> mods;
    mods.push_back(rc::load_nso(bd.build_nso(false), "main"));
    const rc::LinkedImage img = rc::link(std::move(mods));
    const uint64_t base = img.modules[0].base;
    const rc::AnalysisResult res = rc::analyze(img, {});
    const rc::Function* F = res.function_at(base + f);
    const rc::Function* G = res.function_at(base + g);
    CHECK(F && G);
    if (!F || !G) return;

    const rc::LiftFunction lf = rc::to_lift_function(*F);
    CHECK_EQ(lf.entry, F->entry);
    CHECK_EQ(lf.blocks.size(), F->blocks.size());
    for (size_t k = 0; k < lf.blocks.size() && k < F->blocks.size(); ++k) {
        CHECK_EQ(lf.blocks[k].start, F->blocks[k].start);
        CHECK_EQ(lf.blocks[k].end, F->blocks[k].end);
    }
    CHECK_EQ(lf.indirect_targets.size(), 1u);
    const uint64_t br_pc = base + bd.text_off(6);
    CHECK(lf.indirect_targets.count(br_pc) == 1);
    if (lf.indirect_targets.count(br_pc)) CHECK(lf.indirect_targets.at(br_pc) == F->jump_tables[0].targets);
    CHECK_EQ(rc::to_lift_functions(res).size(), res.functions.size());

    // The reader sees code only.
    const rc::CodeReader read = rc::code_reader(img);
    uint32_t insn = 0;
    CHECK(read(base + f, insn) && insn == w[0]);
    CHECK(!read(base + tbl, insn));
    CHECK(!read(base + f + 2, insn));

    const rc::Lifter lifter = rc::make_lifter(img, res);
    rc::LiftStats stats;
    const std::string fcode = lifter.lift(lf, stats);
    const std::string gcode = lifter.lift(rc::to_lift_function(*G), stats);
    CHECK_EQ(stats.unsupported, 0u);
    CHECK_EQ(stats.instructions, 15u);
    CHECK(contains(fcode, "void " + lifter.function_name(base + f) + "(lgp::CpuState* ctx)"));
    CHECK(contains(fcode, "switch"));                             // jump table → switch over the targets
    CHECK(contains(gcode, lifter.function_name(base + f) + "(ctx)"));  // BL → direct call
    CHECK(contains(gcode, "LGP_TAIL_CALL(" + lifter.function_name(base + f) + ")"));  // B → tail call
    CHECK(contains(lifter.declarations(), hex(base + g)));
    if (std::getenv("LGP_DUMP")) std::printf("%s\n%s\n", fcode.c_str(), gcode.c_str());
}

int main(int argc, char** argv) { return tst::run(argc, argv); }
