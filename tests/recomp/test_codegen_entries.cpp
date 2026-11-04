// Which functions the code generator treats as setjmp (codegen.cpp,
// entries_for_symbols): the named symbol, PLT stubs of a named import, and
// stubs whose slot is bound inside the same module (the sdk calls its own
// interposable setjmp that way, seen with LGP's font engine in P4).
#include "a64_encode.hpp"
#include "check.hpp"
#include "lgp/recomp/cfg.hpp"
#include "lgp/recomp/codegen.hpp"
#include "lgp/recomp/link.hpp"
#include "lgp/recomp/loader.hpp"
#include "synthetic_module.hpp"

using namespace ::a64;
using lgp::recomp::AnalysisResult;
using lgp::recomp::LinkedImage;
using lgp::recomp::Module;

TEST(setjmp_stubs_bound_inside_the_module) {
    synth::Builder bd;
    // 0 f0: bl plt; 1 bl thunk; 2 bl bad; 3 ret
    // 4-7 plt: adrp x16, slot; ldr x17, [x16, #lo]; add x16, x16, #lo; br x17   (slot holds &setjmp)
    // 8 thunk: b setjmp
    // 9-10 bad: mov x0, x1; b setjmp     (changes x0: not setjmp)
    // 11 setjmp: ret
    std::vector<uint32_t> w(12, RET);
    const uint64_t slot = bd.alloc_ro(8, 8);
    auto rel = [&](int from, int to) { return static_cast<int64_t>(bd.text_off(to)) - static_cast<int64_t>(bd.text_off(from)); };
    const int64_t pc4 = static_cast<int64_t>(bd.text_off(4));
    w[0] = bl(rel(0, 4));
    w[1] = bl(rel(1, 8));
    w[2] = bl(rel(2, 9));
    w[4] = adrp(16, ((static_cast<int64_t>(slot) & ~0xFFF) - (pc4 & ~0xFFF)) >> 12);
    w[5] = ldr_x_imm(17, 16, static_cast<uint16_t>(slot & 0xFFF));
    w[6] = add_imm(true, 16, 16, static_cast<uint16_t>(slot & 0xFFF));
    w[7] = br(17);
    w[8] = b(rel(8, 11));
    w[9] = mov_reg(true, 0, 1);
    w[10] = b(rel(10, 11));
    bd.text_words = w;
    const uint64_t base = 0x0800'0000;  // LinkOptions default
    std::vector<uint8_t> ptr;
    synth::app64(ptr, base + bd.text_off(11));
    bd.write(slot, ptr);
    bd.symbols.push_back({"setjmp", bd.text_off(11), 4, synth::STT_FUNC, synth::STB_GLOBAL, true});
    bd.fdes = {{bd.text_off(0), bd.text_off(4)}, {bd.text_off(11), bd.text_off(12)}};
    std::vector<Module> mods;
    mods.push_back(lgp::recomp::load_nso(bd.build_nso(false), "main"));
    const LinkedImage img = lgp::recomp::link(std::move(mods));
    CHECK_EQ(img.modules[0].base, base);
    const AnalysisResult res = lgp::recomp::analyze(img, {});
    const std::set<uint64_t> e = lgp::recomp::entries_for_symbols(img, res, {"setjmp"});
    CHECK(e.count(base + bd.text_off(11)));  // the symbol
    CHECK(e.count(base + bd.text_off(4)));   // PLT-shaped stub, slot inside the module
    CHECK(e.count(base + bd.text_off(8)));   // plain b
    CHECK(!e.count(base + bd.text_off(9)));  // changes x0 first
}

int main(int argc, char** argv) { return tst::run(argc, argv); }
