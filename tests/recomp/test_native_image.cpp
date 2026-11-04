// Native rewrite, second part (native_image.hpp): stub code, patch area, NSO
// output, on synthetic code only.
//
// Without Unicorn: the encoder round trip (a64_emit.hpp), planning counters,
// branch reach and the patched NSO. `--dump-forms` prints every encoder form
// with its assembly text for test_emit_vs_llvm.py.
//
// With LGP_NATIVE_REWRITE_UNICORN every stub form runs under Unicorn, original
// against rewritten: x0..x7, x19..x29, SP, PC, NZCV, the vector registers,
// FPSR, TPIDR*, the SVCs made, the data region and the stack above the initial
// SP must agree. Gate sites call a small A64 host function (host_function())
// that performs the SVC through Unicorn's SVC hook and reads and writes TPIDR*
// for real. A deliberately broken stub must show up as a difference.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <random>
#include <stdexcept>
#include <string>

#include "a64_encode.hpp"
#include "check.hpp"
#include "lgp/recomp/a64.hpp"
#include "lgp/recomp/a64_emit.hpp"
#include "lgp/recomp/cfg.hpp"
#include "lgp/recomp/link.hpp"
#include "lgp/recomp/loader.hpp"
#include "lgp/recomp/native_cache.hpp"
#include "lgp/recomp/native_gate.hpp"
#include "lgp/recomp/native_image.hpp"
#include "lgp/recomp/native_rewrite.hpp"
#include "synthetic_module.hpp"

#ifdef LGP_NATIVE_REWRITE_UNICORN
#include "unicorn/unicorn_executor.h"
#endif

using namespace lgp::recomp;
namespace e = lgp::recomp::emit;
namespace g = lgp::recomp::native_gate;
namespace sr = lgp::recomp::a64::sr;
using ::a64::add_imm;
using ::a64::add_reg;
using ::a64::RET;

namespace {

// ---- encoder forms ------------------------------------------------------------

struct Form {
    uint32_t word;
    uint64_t pc;
    std::string text;  // LLVM assembly, branch targets relative to the instruction (`#off`)
};

std::vector<Form> encoder_forms() {
    const uint64_t pc = 0x1000;
    auto rel = [](int64_t d) { return "#" + std::to_string(d); };
    std::vector<Form> f;
    for (int64_t d : {4ll, -8ll, 0x7FFFFFCll, -0x8000000ll}) {
        f.push_back({e::b(pc, pc + d), pc, "b " + rel(d)});
        f.push_back({e::bl(pc, pc + d), pc, "bl " + rel(d)});
    }
    for (int64_t d : {64ll, -32ll, 0xFFFFCll}) {
        f.push_back({e::cbz(true, 9, pc, pc + d), pc, "cbz x9, " + rel(d)});
        f.push_back({e::cbz(false, 16, pc, pc + d, true), pc, "cbnz w16, " + rel(d)});
        f.push_back({e::ldr_lit(3, false, 16, pc, pc + d), pc, "ldr x16, " + rel(d)});
        f.push_back({e::ldr_lit(2, false, 17, pc, pc + d), pc, "ldr w17, " + rel(d)});
        f.push_back({e::ldr_lit(2, true, 3, pc, pc + d), pc, "ldrsw x3, " + rel(d)});
    }
    for (int64_t d : {32ll, -0x8000ll, 0x7FFCll}) {
        f.push_back({e::tbz(7, 3, pc, pc + d), pc, "tbz w7, #3, " + rel(d)});
        f.push_back({e::tbz(7, 40, pc, pc + d, true), pc, "tbnz x7, #40, " + rel(d)});
    }
    f.push_back({e::br(9), pc, "br x9"});
    f.push_back({e::blr(16), pc, "blr x16"});
    f.push_back({e::kRet, pc, "ret"});
    f.push_back({e::kNop, pc, "nop"});
    for (int64_t d : {12ll, -12ll, 0xFFFFFll, -0x100000ll}) f.push_back({e::adr(1, pc, pc + d), pc, "adr x1, " + rel(d)});
    for (int64_t pages : {1ll, -3ll, 0x7FFFFll}) {
        const uint64_t to = (pc & ~0xFFFull) + static_cast<uint64_t>(pages * 4096) + 0x123;
        f.push_back({e::adrp(2, pc, to), pc, "adrp x2, " + rel(pages * 4096)});
    }
    f.push_back({e::add_imm(16, 31, g::kSize), pc, "add x16, sp, #" + std::to_string(g::kSize)});
    f.push_back({e::add_imm(0, 31, 0), pc, "add x0, sp, #0"});
    f.push_back({e::sub_imm(31, 31, g::kSize), pc, "sub sp, sp, #" + std::to_string(g::kSize)});
    f.push_back({e::add_imm(9, 9, 0xFFF), pc, "add x9, x9, #4095"});
    f.push_back({e::ldr_x(30, 31, g::kRet), pc, "ldr x30, [sp, #800]"});
    f.push_back({e::str_x(16, 31, g::kPc), pc, "str x16, [sp, #256]"});
    f.push_back({e::ldr_w(4, 5, 4092), pc, "ldr w4, [x5, #4092]"});
    f.push_back({e::ldst_uimm(2, 0, 17, 31, g::kWord), pc, "str w17, [sp, #264]"});
    f.push_back({e::ldst_uimm(2, 2, 9, 9, 0), pc, "ldrsw x9, [x9]"});
    f.push_back({e::ldst_uimm(0, 1, 9, 9, 0), pc, "ldrb w9, [x9]"});
    f.push_back({e::ldst_uimm(1, 1, 9, 9, 0), pc, "ldrh w9, [x9]"});
    for (int64_t off : {-256ll, -16ll, 8ll, 255ll}) {
        f.push_back({e::ldur_x(9, 31, off), pc, "ldur x9, [sp, #" + std::to_string(off) + "]"});
        f.push_back({e::stur_x(17, 31, off), pc, "stur x17, [sp, #" + std::to_string(off) + "]"});
    }
    for (int64_t off : {0ll, 224ll, -512ll, 504ll}) {
        f.push_back({e::stp_x(28, 29, 31, off), pc, "stp x28, x29, [sp, #" + std::to_string(off) + "]"});
        f.push_back({e::ldp_x(0, 1, 31, off), pc, "ldp x0, x1, [sp, #" + std::to_string(off) + "]"});
    }
    for (int64_t off : {0ll, 480ll, -1024ll, 1008ll}) {
        f.push_back({e::stp_q(30, 31, 16, off), pc, "stp q30, q31, [x16, #" + std::to_string(off) + "]"});
        f.push_back({e::ldp_q(0, 1, 16, off), pc, "ldp q0, q1, [x16, #" + std::to_string(off) + "]"});
    }
    f.push_back({e::mrs(16, sr::kNzcv), pc, "mrs x16, NZCV"});
    f.push_back({e::msr(sr::kNzcv, 16), pc, "msr NZCV, x16"});
    f.push_back({e::mrs(16, sr::kFpsr), pc, "mrs x16, FPSR"});
    f.push_back({e::msr(sr::kFpsr, 16), pc, "msr FPSR, x16"});
    f.push_back({e::mrs(3, sr::kTpidrroEl0), pc, "mrs x3, TPIDRRO_EL0"});
    f.push_back({e::msr(sr::kTpidrEl0, 30), pc, "msr TPIDR_EL0, x30"});
    return f;
}

// ---- synthetic module -----------------------------------------------------------

constexpr uint32_t kSentinel = 0xD4200000u | (0x4C47u << 5);  // brk #0x4c47
uint32_t str_x(uint8_t xt, uint8_t xn, uint32_t off = 0) { return e::str_x(xt, xn, off); }
uint32_t ldr_x(uint8_t xt, uint8_t xn, uint32_t off = 0) { return e::ldr_x(xt, xn, off); }
uint32_t mrs(uint8_t rt, uint32_t r) { return e::mrs(rt, r); }
uint32_t msr(uint32_t r, uint8_t rt) { return e::msr(r, rt); }
uint32_t mov_x(uint8_t rd, uint8_t rm) { return ::a64::mov_reg(true, rd, rm); }

struct Layout {
    synth::Builder bd;
    std::vector<uint64_t> starts;  // module-relative
    size_t fn(std::initializer_list<uint32_t> words, bool lsda = false) {
        const uint64_t start = bd.text_off(bd.text_words.size());
        bd.text_words.insert(bd.text_words.end(), words);
        bd.fdes.push_back({start, bd.text_off(bd.text_words.size())});
        if (lsda) bd.lsda_fdes.push_back(start);
        starts.push_back(start);
        return starts.size() - 1;
    }
    uint64_t at(size_t f, size_t k) const { return starts[f] + 4 * k; }  // module offset of word k
    void set(size_t f, size_t k, uint32_t w) { bd.text_words[(at(f, k) - bd.code_offset()) / 4] = w; }
};

struct Planned {
    LinkedImage img;
    std::vector<NativeRewrite> plan;
    std::vector<uint8_t> nso;
};

Planned plan(Layout& l, const NativeRewriteOptions& opt = {}) {
    Planned p;
    p.nso = l.bd.build_nso(true);
    std::vector<Module> mods;
    mods.push_back(load_nso(p.nso, "main"));
    p.img = link(std::move(mods));
    const AnalysisResult res = analyze(p.img);
    p.plan = plan_native_rewrite(p.img, res, opt);
    return p;
}

// Functions of the stub test module. Each returns through x30 to the sentinel.
struct StubModule {
    Layout l;
    size_t plain, prepost, pcrel, branch, saved, gate, counters, x30_gate, br_far, sentinel;
    uint64_t ro_lit = 0;
    static constexpr uint16_t kSvcs[3] = {0x26, 0x01, 0x7F};
};

StubModule stub_module() {
    StubModule m;
    Layout& l = m.l;
    // x18 in plain instructions, SP moving while it lives in its slot.
    m.plain = l.fn({::a64::movz(true, 18, 0x1234), add_reg(true, 18, 18, 0), ::a64::sub_imm(true, 31, 31, 32),
                    str_x(18, 31, 8), ldr_x(9, 31, 8), add_imm(true, 31, 31, 32), str_x(18, 1), ldr_x(18, 0),
                    add_reg(true, 0, 0, 18), ::a64::movz(false, 18, 7), add_reg(false, 0, 0, 18), RET});
    // Sites that move SP themselves (pre- and post-index).
    m.prepost = l.fn({::a64::movz(true, 18, 77), 0xF81F0FF2u /* str x18, [sp, #-16]! */,
                      0xF84107F2u /* ldr x18, [sp], #16 */, add_reg(true, 0, 0, 18), RET});
    // PC-relative forms; literals in .ro.
    m.ro_lit = l.bd.alloc_ro(16, 8);
    l.bd.write64(m.ro_lit, 0x8877665544332211ull);
    l.bd.write64(m.ro_lit + 8, 0xFFFFFFF0DEADBEEFull);
    m.pcrel = l.fn({0, ldr_x(2, 18), add_reg(true, 0, 0, 2), 0, add_reg(true, 0, 0, 18), 0, add_reg(true, 0, 0, 18), 0,
                    add_reg(true, 0, 0, 18), 0, add_reg(true, 0, 0, 18), RET});
    // Conditional branches on x18, both directions.
    m.branch = l.fn({mov_x(18, 0), ::a64::cbz(true, 18, 8), add_imm(true, 0, 0, 1), ::a64::cbnz(false, 18, 8),
                     add_imm(true, 0, 0, 2), ::a64::tbz(18, 3, 8), add_imm(true, 0, 0, 4), ::a64::tbnz(18, 40, 8),
                     add_imm(true, 0, 0, 8), mov_x(18, 1), ::a64::cbnz(true, 18, 8), add_imm(true, 0, 0, 16), RET});
    // x0..x17 live across the web and an LSDA: no register, the helper is saved.
    m.saved = l.fn({0xF81F0FF3u /* str x19, [sp, #-16]! */, ::a64::movz(true, 18, 7), add_reg(true, 18, 18, 18),
                    add_reg(true, 0, 0, 18), add_reg(true, 0, 0, 1), add_reg(true, 0, 0, 2), add_reg(true, 0, 0, 3),
                    add_reg(true, 0, 0, 4), add_reg(true, 0, 0, 5), add_reg(true, 0, 0, 6), add_reg(true, 0, 0, 7),
                    add_reg(true, 0, 0, 8), add_reg(true, 0, 0, 9), add_reg(true, 0, 0, 10), add_reg(true, 0, 0, 11),
                    add_reg(true, 0, 0, 12), add_reg(true, 0, 0, 13), add_reg(true, 0, 0, 14), add_reg(true, 0, 0, 15),
                    add_reg(true, 0, 0, 16), add_reg(true, 0, 0, 17), 0xF84107F3u /* ldr x19, [sp], #16 */, RET},
                   true);
    // Gate sites: SVCs, TPIDR* with x16/x17 (the stub's own scratch), flags live across.
    m.gate = l.fn({::a64::cmp_reg(true, 0, 1), ::a64::svc(StubModule::kSvcs[0]), mrs(16, sr::kTpidrEl0),
                   add_reg(true, 0, 0, 16), mrs(17, sr::kTpidrroEl0), add_reg(true, 1, 1, 17), msr(sr::kTpidrEl0, 4),
                   mrs(2, sr::kTpidrEl0), ::a64::svc(StubModule::kSvcs[1]), msr(sr::kTpidrEl0, 31),
                   ::a64::svc(StubModule::kSvcs[2]), mrs(31, sr::kTpidrroEl0), RET});
    m.counters = l.fn({mrs(0, sr::kCntvctEl0), mrs(1, sr::kCntpctEl0), mrs(2, sr::kCntfrqEl0), RET});
    // MRS into x30 and back.
    m.x30_gate = l.fn({mov_x(9, 30), mrs(30, sr::kTpidrroEl0), add_reg(true, 0, 0, 30), msr(sr::kTpidrEl0, 30),
                       mov_x(30, 9), RET});
    // BR x18 to a constant target inside the function.
    m.br_far = l.fn({0, 0, 0xD4200120u /* brk #9 */, add_imm(true, 0, 0, 5), RET});
    m.sentinel = l.fn({kSentinel});

    auto off = [&](size_t f, size_t k, uint64_t to) { return static_cast<int64_t>(to) - static_cast<int64_t>(l.at(f, k)); };
    l.set(m.pcrel, 0, ::a64::adr(18, off(m.pcrel, 0, m.ro_lit)));
    l.set(m.pcrel, 3, ::a64::adrp(18, static_cast<int64_t>(m.ro_lit >> 12) - static_cast<int64_t>(l.at(m.pcrel, 3) >> 12)));
    l.set(m.pcrel, 5, ::a64::ldr_lit(1, 18, off(m.pcrel, 5, m.ro_lit)));          // ldr x18, lit
    l.set(m.pcrel, 7, ::a64::ldr_lit(0, 18, off(m.pcrel, 7, m.ro_lit + 8)));      // ldr w18, lit
    l.set(m.pcrel, 9, ::a64::ldr_lit(2, 18, off(m.pcrel, 9, m.ro_lit + 8)));      // ldrsw x18, lit
    l.set(m.br_far, 0, ::a64::adr(18, 12));
    l.set(m.br_far, 1, ::a64::br(18));
    return m;
}

const X18Spill* spill_at(const NativeRewrite& r, uint64_t va) {
    for (const X18Spill& s : r.x18_spills)
        if (s.va == va) return &s;
    return nullptr;
}

#ifdef LGP_NATIVE_REWRITE_UNICORN
using namespace lgp::difftest;

constexpr uint64_t kHost = 0x6000'0000, kData = 0x7000'0000, kStackTop = 0x7100'0000, kStackSize = 0x10000;
constexpr uint64_t kCntvct = 0x1111, kCntpct = 0x2222, kCntfrq = 19'200'000;

// Host function for the gate (native_gate.hpp), A64, at kHost. x20 = frame.
std::vector<uint32_t> host_function() {
    using namespace ::a64;
    std::vector<uint32_t> c;
    std::vector<std::pair<size_t, std::string>> fix;  // b.eq / b to a label
    std::vector<std::pair<std::string, size_t>> labels;
    auto beq = [&](const std::string& to) { fix.push_back({c.size(), "eq:" + to}); c.push_back(0); };
    auto jmp = [&](const std::string& to) { fix.push_back({c.size(), to}); c.push_back(0); };
    auto label = [&](const std::string& n) { labels.push_back({n, c.size()}); };
    auto ldp = [](uint8_t a, uint8_t b2, uint8_t n, int64_t o) { return e::ldp_x(a, b2, n, o); };
    auto stp = [](uint8_t a, uint8_t b2, uint8_t n, int64_t o) { return e::stp_x(a, b2, n, o); };

    c.push_back(add_imm(true, 20, 0, 0));                               // mov x20, x0
    c.push_back(e::ldr_w(9, 20, g::kWord));
    for (uint8_t q : {0, 8, 31}) c.push_back(0x3DC00000u | ((g::kQ + 16) / 16) << 10 | (20u << 5) | q);  // ldr qN, [x20, #q1]
    c.push_back(ubfm(false, 10, 9, 20, 31));                            // lsr w10, w9, #20
    c.push_back(movz(false, 11, 0xD40)); c.push_back(cmp_reg(false, 10, 11)); beq("svc");
    c.push_back(movz(false, 11, 0xD53)); c.push_back(cmp_reg(false, 10, 11)); beq("mrs");
    c.push_back(movz(false, 11, 0xD51)); c.push_back(cmp_reg(false, 10, 11)); beq("msr");
    c.push_back(brk(1));

    label("svc");
    c.push_back(ubfm(false, 10, 9, 5, 20));                             // ubfx w10, w9, #5, #16
    for (uint16_t imm : StubModule::kSvcs) {
        c.push_back(movz(false, 11, imm)); c.push_back(cmp_reg(false, 10, 11)); beq("svc" + std::to_string(imm));
    }
    c.push_back(brk(2));
    for (uint16_t imm : StubModule::kSvcs) {
        label("svc" + std::to_string(imm));
        for (uint8_t r = 0; r < 8; r += 2) c.push_back(ldp(r, r + 1, 20, 8 * r));
        c.push_back(svc(imm));
        for (uint8_t r = 0; r < 8; r += 2) c.push_back(stp(r, r + 1, 20, 8 * r));
        c.push_back(RET);
    }

    label("mrs");
    c.push_back(ubfm(false, 12, 9, 5, 19));                             // ubfx w12, w9, #5, #15: system register
    c.push_back(ubfm(false, 13, 9, 0, 4));                              // rt
    const std::pair<uint32_t, const char*> regs[] = {{sr::kTpidrEl0, "tpidr"}, {sr::kTpidrroEl0, "tpidrro"},
                                                     {sr::kCntvctEl0, "cntvct"}, {sr::kCntpctEl0, "cntpct"},
                                                     {sr::kCntfrqEl0, "cntfrq"}};
    for (const auto& [r, name] : regs) {
        c.push_back(movz(false, 11, static_cast<uint16_t>(r & 0x7FFF))); c.push_back(cmp_reg(false, 12, 11)); beq(name);
    }
    c.push_back(brk(3));
    label("tpidr"); c.push_back(e::mrs(14, sr::kTpidrEl0)); jmp("put");
    label("tpidrro"); c.push_back(e::mrs(14, sr::kTpidrroEl0)); jmp("put");
    label("cntvct"); c.push_back(movz(true, 14, static_cast<uint16_t>(kCntvct))); jmp("put");
    label("cntpct"); c.push_back(movz(true, 14, static_cast<uint16_t>(kCntpct))); jmp("put");
    label("cntfrq"); c.push_back(movz(true, 14, static_cast<uint16_t>(kCntfrq & 0xFFFF))); c.push_back(movk(true, 14, static_cast<uint16_t>(kCntfrq >> 16), 1)); jmp("put");
    label("put");
    c.push_back(cmp_imm(false, 13, 31)); beq("ret");
    c.push_back(ldr_reg(3, 0, 14, 20, 13, 3, true));                    // str x14, [x20, x13, lsl #3]
    label("ret");
    c.push_back(RET);

    label("msr");
    c.push_back(ubfm(false, 13, 9, 0, 4));
    c.push_back(movz(true, 14, 0));
    c.push_back(cmp_imm(false, 13, 31)); beq("msr_set");
    c.push_back(ldr_reg(3, 1, 14, 20, 13, 3, true));                    // ldr x14, [x20, x13, lsl #3]
    label("msr_set");
    c.push_back(e::msr(sr::kTpidrEl0, 14));
    c.push_back(RET);

    for (const auto& [at, target] : fix) {
        const bool cond = target.rfind("eq:", 0) == 0;
        const std::string name = cond ? target.substr(3) : target;
        const auto it = std::find_if(labels.begin(), labels.end(), [&](const auto& x) { return x.first == name; });
        if (it == labels.end()) throw std::logic_error("host_function: no label " + name);
        const int64_t d = 4 * (static_cast<int64_t>(it->second) - static_cast<int64_t>(at));
        c[at] = cond ? bcond(0, d) : b(d);
    }
    return c;
}

std::vector<uint8_t> to_bytes(const std::vector<uint32_t>& w) {
    std::vector<uint8_t> b(w.size() * 4);
    std::memcpy(b.data(), w.data(), b.size());
    return b;
}

// Image of the module with the patch applied, and the patch area with the
// host function pointer in place.
struct Applied {
    std::vector<uint8_t> module;
    std::vector<uint8_t> area;
    uint64_t area_base = 0;
};

Applied apply(const Planned& p, const NativePatch& patch) {
    const Module& mod = p.img.modules[0];
    Applied a{mod.image, patch.area, patch.area_base};
    for (const auto& [va, w] : patch.modules[0].words) std::memcpy(a.module.data() + (va - mod.base), &w, 4);
    const uint64_t host = kHost;
    std::memcpy(a.area.data() + (patch.host_fn_slot - patch.area_base), &host, 8);
    return a;
}

TestCase test_case(const Planned& p, const Applied* patched, uint64_t entry, std::mt19937_64& rng) {
    const Module& mod = p.img.modules[0];
    TestCase tc;
    tc.name = "native_image";
    tc.memory.push_back({mod.base, patched ? patched->module : mod.image, kRead | kWrite | kExec});
    if (patched) tc.memory.push_back({patched->area_base, patched->area, kRead | kWrite | kExec});
    static const std::vector<uint8_t> host = to_bytes(host_function());
    tc.memory.push_back({kHost, host, kRead | kExec});
    MemoryRegion data{kData, std::vector<uint8_t>(4096), kRead | kWrite};
    MemoryRegion stack{kStackTop - kStackSize, std::vector<uint8_t>(kStackSize), kRead | kWrite};
    for (auto& b : data.bytes) b = static_cast<uint8_t>(rng());
    for (auto& b : stack.bytes) b = static_cast<uint8_t>(rng());
    tc.memory.push_back(std::move(data));
    tc.memory.push_back(std::move(stack));
    for (uint64_t& x : tc.initial.x) x = rng();
    for (Vec128& v : tc.initial.v) v = {rng(), rng()};
    tc.initial.nzcv = static_cast<uint32_t>(rng()) & 0xF0000000u;
    tc.initial.tpidr_el0 = rng();
    tc.initial.tpidrro_el0 = rng();
    tc.initial.x[0] = kData + 8 * (rng() % 64);
    tc.initial.x[1] = kData + 8 * (rng() % 64);
    tc.initial.sp = kStackTop - 0x400;
    tc.initial.pc = entry;
    tc.svc = [](uint32_t imm, SvcRegs& r) {
        r[0] = r[0] * 3 + imm;
        r[1] ^= r[2];
        r[7] = imm;
    };
    return tc;
}

const MemoryRegion* region(const RunResult& r, uint64_t base) {
    for (const MemoryRegion& m : r.memory)
        if (m.base == base) return &m;
    return nullptr;
}

// Differences that matter to a caller (empty: the runs agree).
std::vector<std::string> differences(const RunResult& a, const RunResult& b, uint64_t initial_sp) {
    std::vector<std::string> d;
    if (a.outcome != Outcome::Sentinel || b.outcome != Outcome::Sentinel)
        d.push_back(std::string("outcome ") + to_string(a.outcome) + " / " + to_string(b.outcome) + " " + b.detail);
    for (unsigned k : {0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 19u, 20u, 21u, 22u, 23u, 24u, 25u, 26u, 27u, 28u, 29u})
        if (a.final.x[k] != b.final.x[k]) d.push_back("x" + std::to_string(k));
    if (a.final.sp != b.final.sp) d.push_back("sp");
    if (a.final.pc != b.final.pc) d.push_back("pc");
    if (a.final.nzcv != b.final.nzcv) d.push_back("nzcv");
    if (a.final.v != b.final.v) d.push_back("v");
    if (a.final.fpsr != b.final.fpsr || a.final.fpcr != b.final.fpcr) d.push_back("fpsr/fpcr");
    if (a.final.tpidr_el0 != b.final.tpidr_el0 || a.final.tpidrro_el0 != b.final.tpidrro_el0) d.push_back("tpidr");
    if (a.svcs != b.svcs) d.push_back("svcs");
    const MemoryRegion *da = region(a, kData), *db = region(b, kData);
    if (!da || !db || da->bytes != db->bytes) d.push_back("data");
    const MemoryRegion *sa = region(a, kStackTop - kStackSize), *sb = region(b, kStackTop - kStackSize);
    const size_t from = initial_sp - (kStackTop - kStackSize);
    if (!sa || !sb || !std::equal(sa->bytes.begin() + from, sa->bytes.end(), sb->bytes.begin() + from))
        d.push_back("stack above SP");
    return d;
}

using Setup = std::function<void(TestCase&, int)>;

// Runs `entry` original and patched; returns the number of rounds that differ.
int compare(const Planned& p, const Applied& patched, uint64_t entry, int rounds, uint64_t seed, bool report,
            const Setup& setup = {}) {
    UnicornExecutor ex;
    std::mt19937_64 rng(seed);
    int bad = 0;
    for (int round = 0; round < rounds; ++round) {
        const uint64_t s = rng();
        std::mt19937_64 r1(s), r2(s);
        TestCase a = test_case(p, nullptr, entry, r1);
        TestCase b = test_case(p, &patched, entry, r2);
        if (setup) {
            setup(a, round);
            setup(b, round);
        }
        const RunResult ra = ex.run(a), rb = ex.run(b);
        const auto d = differences(ra, rb, a.initial.sp);
        if (!d.empty()) {
            ++bad;
            if (report) {
                std::string all;
                for (const auto& x : d) all += " " + x;
                std::fprintf(stderr, "  entry 0x%llx round %d differs:%s\n", static_cast<unsigned long long>(entry), round,
                             all.c_str());
            }
        }
    }
    return bad;
}
#endif

}  // namespace

TEST(encoder_round_trip) {
    for (const Form& f : encoder_forms()) {
        const lgp::recomp::a64::Inst i = lgp::recomp::a64::decode(f.word, f.pc);
        if (i.op == lgp::recomp::a64::Op::Invalid) {
            std::fprintf(stderr, "  invalid: %08x %s\n", f.word, f.text.c_str());
            CHECK(false);
        }
    }
    // Fields of the forms the stubs depend on.
    using lgp::recomp::a64::decode;
    using lgp::recomp::a64::Op;
    auto i = decode(e::b(0x1000, 0x2000), 0x1000);
    CHECK(i.op == Op::B && i.target == 0x2000);
    i = decode(e::cbz(false, 16, 0x1000, 0x0F00, true), 0x1000);
    CHECK(i.op == Op::Cbnz && !i.sf && i.rt == 16 && i.target == 0x0F00);
    i = decode(e::tbz(7, 40, 0x1000, 0x1020), 0x1000);
    CHECK(i.op == Op::Tbz && i.bit == 40 && i.rt == 7 && i.target == 0x1020);
    i = decode(e::adrp(2, 0x1000, 0x5123), 0x1000);
    CHECK(i.op == Op::Adrp && i.rd == 2 && i.target == 0x5000);
    i = decode(e::adr(1, 0x1000, 0x0FF4), 0x1000);
    CHECK(i.op == Op::Adr && i.target == 0x0FF4);
    i = decode(e::ldr_lit(2, true, 3, 0x1000, 0x1010), 0x1000);
    CHECK(i.op == Op::LdrLit && i.sign_extend && i.size_log2 == 2 && i.target == 0x1010);
    i = decode(e::ldur_x(9, 31, -16), 0);
    CHECK(i.op == Op::LdSt && i.load && i.rn == 31 && i.offset == -16 && i.size_log2 == 3);
    i = decode(e::stp_q(30, 31, 16, -1024), 0);
    CHECK(i.op == Op::LdStPair && i.simd && i.size_log2 == 4 && i.offset == -1024 && i.rt == 30 && i.rt2 == 31);
    i = decode(e::mrs(3, sr::kTpidrroEl0), 0);
    CHECK(i.op == Op::Mrs && i.rt == 3 && i.sysreg == sr::kTpidrroEl0);
    i = decode(e::msr(sr::kNzcv, 16), 0);
    CHECK(i.op == Op::Msr && i.rt == 16 && i.sysreg == sr::kNzcv);
    bool threw = false;
    try { (void)e::b(0, 0x8000000); } catch (const std::out_of_range&) { threw = true; }
    CHECK(threw);
    CHECK(e::b_reaches(0, 0x7FFFFFC) && !e::b_reaches(0, 0x8000000) && e::b_reaches(0x8000000, 0));
}

TEST(plan_counters) {
    StubModule m = stub_module();
    NativeRewriteOptions opt;
    opt.rename = false;
    Planned p = plan(m.l, opt);
    const NativeRewrite& r = p.plan[0];
    const uint64_t base = p.img.modules[0].base;
    // plain: two webs (x18 = 0x1234 + x0, then a load), w18 a third; prepost two; pcrel five; branch two; saved one.
    CHECK_EQ(r.webs_renamed, 0u);
    // plus br_far: adr x18 / br x18.
    CHECK_EQ(r.webs_spilled, 3u + 2u + 5u + 2u + 1u + 1u);
    CHECK_EQ(r.webs_open, 0u);
    const X18Spill* br = spill_at(r, base + m.l.at(m.br_far, 1));
    CHECK(br && br->helper_dead);
    CHECK_EQ(r.webs_form + r.webs_gate + r.webs_sp_unknown + r.webs_slot_far + r.webs_exclusive, 0u);
    // SVCs, TPIDR*, counters and the x30 forms are gate sites.
    CHECK_EQ(r.gate_sites.size(), 3u + 6u + 3u + 2u);
    CHECK_EQ(r.gate_x18 + r.gate_exclusive + r.sp_sites, 0u);
    // The site that moves SP: slot offsets before and after differ by 16.
    const X18Spill* pre = spill_at(r, base + m.l.at(m.prepost, 1));
    CHECK(pre && pre->slot_after == pre->slot_before + 16);
    const X18Spill* post = spill_at(r, base + m.l.at(m.prepost, 2));
    CHECK(post && post->slot_after == post->slot_before - 16);
    // No free register in `saved`: the helper is saved.
    const X18Spill* s = spill_at(r, base + m.l.at(m.saved, 1));
    CHECK(s && !s->helper_dead && s->helper < 18);
    // With renaming, `saved` is still the only web left (no register).
    Planned q = plan(m.l);
    CHECK_EQ(q.plan[0].webs_spilled, 1u);

    // A gate site inside a web: the frame would cover the slot.
    Layout l2;
    l2.fn({::a64::movz(true, 18, 1), ::a64::svc(0x26), add_reg(true, 0, 0, 18), RET});
    Planned p2 = plan(l2, opt);
    CHECK_EQ(p2.plan[0].webs_gate, 1u);
    CHECK_EQ(p2.plan[0].webs_spilled, 0u);
    CHECK_EQ(p2.plan[0].x18_unpatched.size(), 2u);
}

TEST(patch_area_and_nso) {
    StubModule m = stub_module();
    NativeRewriteOptions opt;
    opt.rename = false;
    Planned p = plan(m.l, opt);
    const NativePatch patch = build_native_patch(p.img, p.plan);
    const Module& mod = p.img.modules[0];
    CHECK_EQ(patch.area_base % kNativePageSize, 0u);
    CHECK(patch.area_base >= mod.base + mod.image.size());
    CHECK_EQ(patch.gate_entry, patch.area_base);
    CHECK_EQ(patch.data_base % kNativePageSize, 0u);
    CHECK(patch.data_base >= patch.area_base + patch.code_size);
    CHECK_EQ(patch.area.size(), patch.data_base - patch.area_base + kNativePageSize);
    CHECK_EQ(patch.spill_stubs, p.plan[0].x18_spills.size());
    CHECK_EQ(patch.gate_stubs, p.plan[0].gate_sites.size());
    CHECK_EQ(patch.stubs.size(), patch.spill_stubs + patch.gate_stubs);
    // Every site is a B to its stub.
    for (const auto& [site, stub] : patch.stubs) {
        const auto it = std::find_if(patch.modules[0].words.begin(), patch.modules[0].words.end(),
                                     [&](const auto& w) { return w.first == site; });
        CHECK(it != patch.modules[0].words.end() && it->second == e::b(site, stub));
    }

    // The patched NSO: .text carries the words, .ro and .data unchanged, uncompressed, fresh hashes.
    const std::vector<uint8_t> out = patch_nso(p.nso, mod, patch.modules[0]);
    const Module re = load_nso(out, "main");
    CHECK_EQ(re.nso_flags & 7u, 0u);
    CHECK_EQ(re.nso_flags & 0x38u, mod.nso_flags & 0x38u);
    for (const Segment& s : re.segments)
        if (s.kind != SegmentKind::Bss) CHECK(s.hash_ok && !s.compressed);
    const Module orig = load_nso(p.nso, "main");
    for (const auto& [va, w] : patch.modules[0].words) CHECK_EQ(re.read_u32(va - mod.base), w);
    const Segment& t = re.segment(SegmentKind::Text);
    size_t changed = 0;
    for (uint64_t off = t.offset; off < t.offset + t.size; off += 4) changed += re.read_u32(off) != orig.read_u32(off);
    CHECK_EQ(changed, patch.modules[0].words.size());
    for (SegmentKind k : {SegmentKind::Ro, SegmentKind::Data}) {
        const Segment& a = orig.segment(k);
        CHECK(std::equal(orig.image.begin() + a.offset, orig.image.begin() + a.offset + a.size, re.image.begin() + a.offset));
    }

    // A stub beyond 128 MiB of its site stops the build.
    NativePatchOptions far;
    far.area_base = (mod.base + 0x0800'0000 + kNativePageSize) & ~(kNativePageSize - 1);
    bool threw = false;
    try { (void)build_native_patch(p.img, p.plan, far); } catch (const std::runtime_error&) { threw = true; }
    CHECK(threw);
}

#ifdef LGP_NATIVE_REWRITE_UNICORN
TEST(unicorn_stub_forms) {
    StubModule m = stub_module();
    NativeRewriteOptions opt;
    opt.rename = false;
    Planned p = plan(m.l, opt);
    const uint64_t base = p.img.modules[0].base;
    const uint64_t sentinel = base + m.l.starts[m.sentinel];
    const Setup ret_to_sentinel = [&](TestCase& tc, int) { tc.initial.x[30] = sentinel; };
    const Setup branchy = [&](TestCase& tc, int round) {
        tc.initial.x[30] = sentinel;
        // Rounds alternate zero, small and random values so every branch goes both ways.
        if (round % 3 == 0) tc.initial.x[0] = 0;
        if (round % 3 == 1) tc.initial.x[0] = static_cast<uint64_t>(round) << 1;
    };
    struct Case {
        const char* name;
        size_t fn;
        bool branches;
    };
    const Case cases[] = {{"plain", m.plain, false},   {"prepost", m.prepost, false}, {"pcrel", m.pcrel, false},
                          {"branch", m.branch, true},  {"saved", m.saved, false},     {"gate", m.gate, false},
                          {"x30_gate", m.x30_gate, false}, {"br", m.br_far, false}};

    // A: as planned (dead helpers where there are any).
    const NativePatch patch = build_native_patch(p.img, p.plan);
    const Applied a = apply(p, patch);
    for (const Case& c : cases) {
        const int bad = compare(p, a, base + m.l.starts[c.fn], 12, 56, true, c.branches ? branchy : ret_to_sentinel);
        if (bad) std::fprintf(stderr, "  %s: %d rounds differ\n", c.name, bad);
        CHECK_EQ(bad, 0);
    }

    // B: every helper saved and restored (x7 or x6, live at the exit).
    std::vector<NativeRewrite> saved = p.plan;
    for (X18Spill& s : saved[0].x18_spills) {
        const auto i = lgp::recomp::a64::decode(s.word, s.va);
        if (i.op == lgp::recomp::a64::Op::Br) continue;  // needs a dead helper
        const auto u = gpr_use(i);
        s.helper = ((u.reads | u.writes) & (1u << 7)) ? 6 : 7;
        s.helper_dead = false;
    }
    const Applied b = apply(p, build_native_patch(p.img, saved));
    for (const Case& c : cases)
        CHECK_EQ(compare(p, b, base + m.l.starts[c.fn], 12, 57, true, c.branches ? branchy : ret_to_sentinel), 0);

    // Counters: the host function's values arrive in the guest registers.
    {
        UnicornExecutor ex;
        std::mt19937_64 rng(59);
        TestCase tc = test_case(p, &a, base + m.l.starts[m.counters], rng);
        tc.initial.x[30] = sentinel;
        const RunResult r = ex.run(tc);
        CHECK(r.outcome == Outcome::Sentinel);
        CHECK_EQ(r.final.x[0], kCntvct);
        CHECK_EQ(r.final.x[1], kCntpct);
        CHECK_EQ(r.final.x[2], kCntfrq);
        // The gate function really went through the host function: three SVCs, TPIDR_EL0 cleared by msr xzr.
        std::mt19937_64 rng2(63);
        TestCase tg = test_case(p, &a, base + m.l.starts[m.gate], rng2);
        tg.initial.x[30] = sentinel;
        const RunResult rg = ex.run(tg);
        CHECK(rg.outcome == Outcome::Sentinel);
        CHECK_EQ(rg.svcs.size(), 3u);
        CHECK_EQ(rg.final.tpidr_el0, 0u);
        CHECK_EQ(rg.final.x[2], tg.initial.x[4]);  // msr tpidr_el0, x4; mrs x2, tpidr_el0
    }
    std::printf("unicorn: %zu stubs, original and rewritten agree\n", patch.stubs.size());

    // Negative controls: a broken stub must show up.
    // (1) A spill stub stores x18 to the wrong slot.
    std::vector<NativeRewrite> wrong = p.plan;
    for (X18Spill& s : wrong[0].x18_spills)
        if (s.va == base + m.l.at(m.plain, 0)) s.slot_after = static_cast<int16_t>(s.slot_after - 8);
    const Applied w1 = apply(p, build_native_patch(p.img, wrong));
    CHECK(compare(p, w1, base + m.l.starts[m.plain], 4, 60, false, ret_to_sentinel) > 0);
    // (2) The gate entry does not restore NZCV (the host function changes the flags).
    Applied w2 = a;
    const uint32_t restore_nzcv = e::msr(sr::kNzcv, 16);
    bool found = false;
    for (size_t k = 0; k + 4 <= patch.code_size; k += 4) {
        uint32_t w;
        std::memcpy(&w, w2.area.data() + k, 4);
        if (w == restore_nzcv) {
            std::memcpy(w2.area.data() + k, &e::kNop, 4);
            found = true;
            break;
        }
    }
    CHECK(found);
    CHECK(compare(p, w2, base + m.l.starts[m.gate], 8, 61, false, ret_to_sentinel) > 0);
    // (3) A gate stub that forgets to restore x30.
    Applied w3 = a;
    const uint32_t restore_x30 = e::ldr_x(30, 31, 8 * 30);
    for (size_t k = 0; k + 4 <= patch.code_size; k += 4) {
        uint32_t w;
        std::memcpy(&w, w3.area.data() + k, 4);
        if (w == restore_x30) std::memcpy(w3.area.data() + k, &e::kNop, 4);
    }
    CHECK(compare(p, w3, base + m.l.starts[m.gate], 2, 62, false, ret_to_sentinel) > 0);
    std::printf("unicorn: broken stubs are caught\n");
}
#endif

// The rewrite cache (native_cache.hpp): a record comes back as it went in,
// only under its own key, and a damaged or cut file is refused.
TEST(native_cache_round_trip) {
    NativeRewriteRecord r;
    r.patch.area_base = 0x70'0001'0000;
    r.patch.gate_entry = r.patch.area_base;
    r.patch.data_base = r.patch.area_base + 0x4000;
    r.patch.host_fn_slot = r.patch.data_base + 8;
    r.patch.area = std::vector<uint8_t>(0x8000, 0xA5);
    r.patch.code_size = 0x40;
    r.patch.modules = {{"main", 0x70'0000'0000, {{0x70'0000'0010, 0x14000004u}, {0x70'0000'0020, 0xAA0203E1u}}},
                       {"sdk", 0x70'0000'8000, {}}};
    r.patch.stubs = {{0x70'0000'0010, 0x70'0001'0020}};
    r.patch.sites_renamed = 1;
    r.patch.gate_stubs = 1;
    r.x18_unpatched = 3;
    r.gate_unpatched = 2;
    const std::vector<uint8_t> bytes = encode_native_cache(r, 42);
    NativeRewriteRecord back;
    std::string why;
    CHECK(decode_native_cache(bytes, 42, &back, &why));
    CHECK(back.patch.area == r.patch.area);
    CHECK_EQ(back.patch.code_size, r.patch.code_size);
    CHECK_EQ(back.patch.host_fn_slot, r.patch.host_fn_slot);
    CHECK_EQ(back.patch.modules.size(), size_t{2});
    CHECK(back.patch.modules[0].words == r.patch.modules[0].words);
    CHECK_STR_EQ(back.patch.modules[1].module, std::string("sdk"));
    CHECK(back.patch.stubs == r.patch.stubs);
    CHECK_EQ(back.x18_unpatched, uint64_t{3});
    CHECK_EQ(back.gate_unpatched, uint64_t{2});
    CHECK(!decode_native_cache(bytes, 43, &back, &why));
    std::vector<uint8_t> bad = bytes;
    bad[100] ^= 1;
    CHECK(!decode_native_cache(bad, 42, &back, &why));
    CHECK_STR_EQ(why, std::string("damaged"));
    bad.assign(bytes.begin(), bytes.begin() + bytes.size() / 2);
    CHECK(!decode_native_cache(bad, 42, &back, &why));
    CHECK(!decode_native_cache({}, 42, &back, &why));
}

// The key follows every input it names.
TEST(native_cache_key) {
    LinkedImage image;
    image.modules.resize(1);
    image.modules[0].name = "main";
    image.modules[0].base = 0x70'0000'0000;
    image.modules[0].image = std::vector<uint8_t>(0x1003, 7);
    const uint64_t k = native_cache_key(image, "cfg", 0x1000, "salt");
    CHECK_EQ(native_cache_key(image, "cfg", 0x1000, "salt"), k);
    CHECK(native_cache_key(image, "cfg2", 0x1000, "salt") != k);
    CHECK(native_cache_key(image, "cfg", 0x2000, "salt") != k);
    CHECK(native_cache_key(image, "cfg", 0x1000, "other") != k);
    image.modules[0].image[0x1002] = 8;  // the tail after the last full word
    CHECK(native_cache_key(image, "cfg", 0x1000, "salt") != k);
}

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--dump-forms") == 0) {
        for (const Form& f : encoder_forms()) std::printf("%08x\t%s\n", f.word, f.text.c_str());
        return 0;
    }
    return tst::run(argc, argv);
}
