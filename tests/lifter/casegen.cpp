// Generates differential test cases for the lifter: random instruction words
// per encoding group (operand bits random, opcode bits fixed), plus
// hand-built control-flow templates. Every case is lifted to C++; the test
// driver runs the same case in Unicorn and compares (ADR-0006, stage 1).
//
// Usage: lgp_lifter_casegen <out_dir> <shards> <cases_per_group> <seed>
// Writes shard_<n>.cpp and index.cpp. Deterministic for a given seed.
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "case_defs.hpp"
#include "int_groups.hpp"
#include "lgp/recomp/a64.hpp"
#include "lgp/recomp/lifter.hpp"

namespace {

using lgp::recomp::LiftBlock;
using lgp::recomp::LiftFunction;
using lgp::recomp::Lifter;
using lgp::recomp::LiftStats;
namespace a64 = lgp::recomp::a64;
using lgp_test::kCodeBase;

constexpr uint32_t kSentinel = 0xD4200000u | (0x4C47u << 5);  // BRK #0x4C47

struct Rng {
    lgp_test::SplitMix sm;
    explicit Rng(uint64_t seed) : sm(seed) {}
    uint64_t u64() { return sm.next(); }
    uint32_t u32() { return static_cast<uint32_t>(sm.next()); }
    uint64_t below(uint64_t n) { return n ? u64() % n : 0; }
    bool chance(unsigned percent) { return below(100) < percent; }
};

struct GenFunction {
    uint64_t entry;
    uint64_t end;
    std::map<uint64_t, std::vector<uint64_t>> indirect_targets;
    uint64_t start = 0;  // block start if the entry is not the lowest address
};

struct GenCase {
    std::string group;
    std::string name;
    uint64_t seed = 0;
    std::vector<uint32_t> code;
    std::vector<lgp_test::RegOverride> overrides;
    std::vector<GenFunction> functions;  // empty: one function covering all code
    uint64_t entry = kCodeBase;
};

// ---------------------------------------------------------------------------
// Encoders for the templates
// ---------------------------------------------------------------------------
uint32_t movz(bool sf, unsigned rd, uint32_t imm16, unsigned hw = 0) {
    return (sf ? 0x80000000u : 0) | 0x52800000u | (hw << 21) | ((imm16 & 0xFFFF) << 5) | rd;
}
uint32_t add_imm(bool sf, unsigned rd, unsigned rn, uint32_t imm12) {
    return (sf ? 0x80000000u : 0) | 0x11000000u | ((imm12 & 0xFFF) << 10) | (rn << 5) | rd;
}
uint32_t subs_imm(bool sf, unsigned rd, unsigned rn, uint32_t imm12) {
    return (sf ? 0x80000000u : 0) | 0x71000000u | ((imm12 & 0xFFF) << 10) | (rn << 5) | rd;
}
uint32_t add_reg(bool sf, unsigned rd, unsigned rn, unsigned rm) {
    return (sf ? 0x80000000u : 0) | 0x0B000000u | (rm << 16) | (rn << 5) | rd;
}
uint32_t b(int64_t off) { return 0x14000000u | (static_cast<uint32_t>(off / 4) & 0x3FFFFFF); }
uint32_t bl(int64_t off) { return 0x94000000u | (static_cast<uint32_t>(off / 4) & 0x3FFFFFF); }
uint32_t bcond(unsigned cond, int64_t off) { return 0x54000000u | ((static_cast<uint32_t>(off / 4) & 0x7FFFF) << 5) | cond; }
uint32_t cbz(bool sf, bool nz, unsigned rt, int64_t off) {
    return (sf ? 0x80000000u : 0) | 0x34000000u | (nz ? 0x01000000u : 0) | ((static_cast<uint32_t>(off / 4) & 0x7FFFF) << 5) | rt;
}
uint32_t tbz(bool nz, unsigned rt, unsigned bitpos, int64_t off) {
    return ((bitpos >> 5) << 31) | 0x36000000u | (nz ? 0x01000000u : 0) | ((bitpos & 31) << 19) |
           ((static_cast<uint32_t>(off / 4) & 0x3FFF) << 5) | rt;
}
uint32_t adr(unsigned rd, int64_t off) {
    const uint32_t imm = static_cast<uint32_t>(off) & 0x1FFFFF;
    return 0x10000000u | ((imm & 3) << 29) | ((imm >> 2) << 5) | rd;
}
uint32_t br(unsigned rn) { return 0xD61F0000u | (rn << 5); }
uint32_t blr(unsigned rn) { return 0xD63F0000u | (rn << 5); }
uint32_t ret(unsigned rn = 30) { return 0xD65F0000u | (rn << 5); }
uint32_t svc(uint32_t imm) { return 0xD4000001u | ((imm & 0xFFFF) << 5); }
uint32_t stp_pre(unsigned rt, unsigned rt2, unsigned rn, int imm) {  // 64-bit
    return 0xA9800000u | ((static_cast<uint32_t>(imm / 8) & 0x7F) << 15) | (rt2 << 10) | (rn << 5) | rt;
}
uint32_t ldp_post(unsigned rt, unsigned rt2, unsigned rn, int imm) {
    return 0xA8C00000u | ((static_cast<uint32_t>(imm / 8) & 0x7F) << 15) | (rt2 << 10) | (rn << 5) | rt;
}
uint32_t ldxr(unsigned size, bool acq, unsigned rt, unsigned rn) {
    return (size << 30) | 0x085F7C00u | (acq ? 0x8000u : 0) | (rn << 5) | rt;
}
uint32_t stxr(unsigned size, bool rel, unsigned rs, unsigned rt, unsigned rn) {
    return (size << 30) | 0x08007C00u | (rel ? 0x8000u : 0) | (rs << 16) | (rn << 5) | rt;
}
uint32_t ldxp(bool x, bool acq, unsigned rt, unsigned rt2, unsigned rn) {
    return (x ? 0xC0000000u : 0x80000000u) | 0x087F0000u | (acq ? 0x8000u : 0) | (rt2 << 10) | (rn << 5) | rt;
}
uint32_t stxp(bool x, bool rel, unsigned rs, unsigned rt, unsigned rt2, unsigned rn) {
    return (x ? 0xC0000000u : 0x80000000u) | 0x08200000u | (rel ? 0x8000u : 0) | (rs << 16) | (rt2 << 10) | (rn << 5) | rt;
}
uint32_t ldar(unsigned size, unsigned rt, unsigned rn) { return (size << 30) | 0x08DFFC00u | (rn << 5) | rt; }
uint32_t stlr(unsigned size, unsigned rt, unsigned rn) { return (size << 30) | 0x089FFC00u | (rn << 5) | rt; }
uint32_t clrex() { return 0xD503305Fu; }
uint32_t mrs(uint32_t sysreg, unsigned rt) { return 0xD5300000u | (sysreg << 5) | rt; }
uint32_t msr(uint32_t sysreg, unsigned rt) { return 0xD5100000u | (sysreg << 5) | rt; }
uint32_t dc_zva(unsigned rt) { return 0xD50B7420u | rt; }
uint32_t ldr_x(unsigned rt, unsigned rn) { return 0xF9400000u | (rn << 5) | rt; }
uint32_t str_x(unsigned rt, unsigned rn) { return 0xF9000000u | (rn << 5) | rt; }

// ---------------------------------------------------------------------------
// Random encoding groups
// ---------------------------------------------------------------------------
using lgp_test::intgen::Group;
using lgp_test::intgen::kBaseRegs;
using lgp_test::intgen::kGroups;
using lgp_test::intgen::Kind;
using lgp_test::intgen::memory_overrides;
using lgp_test::intgen::patch_literal;
using lgp_test::intgen::patch_memory;
using lgp_test::intgen::set_field;

GenCase random_case(Rng& rng, const Group& g, unsigned index) {
    GenCase c;
    c.group = g.name;
    c.name = std::string(g.name) + "/" + std::to_string(index);
    c.seed = rng.u64();
    const unsigned len = 1 + static_cast<unsigned>(rng.below(g.max_len));
    // Single-instruction cases first, so every encoding is also tested alone.
    const unsigned n = index % 2 == 0 ? 1 : len;
    for (unsigned k = 0; k < n; ++k) {
        const uint64_t pc = kCodeBase + 4 * k;
        for (int attempt = 0; attempt < 10000; ++attempt) {
            uint32_t w = (rng.u32() & ~g.mask) | g.value;
            // SMULH/UMULH need Ra = 31, MUL/MNEG are Ra = 31 too: make it common.
            if (std::strcmp(g.name, "dp3") == 0 && rng.chance(30)) w |= 0x1Fu << 10;
            if (a64::decode(w, pc).op == a64::Op::Invalid) continue;
            if (g.kind == Kind::Mem && !patch_memory(rng, w, pc)) continue;
            if (g.kind == Kind::Literal && !patch_literal(rng, w, pc)) continue;
            c.code.push_back(w);
            break;
        }
    }
    if (g.kind == Kind::Mem) memory_overrides(rng, c, 0);
    c.code.push_back(kSentinel);
    return c;
}

// ---------------------------------------------------------------------------
// Templates: control flow, system registers, exclusives
// ---------------------------------------------------------------------------
GenCase make(const char* group, unsigned index, Rng& rng) {
    GenCase c;
    c.group = group;
    c.name = std::string(group) + "/" + std::to_string(index);
    c.seed = rng.u64();
    return c;
}

uint64_t interesting(Rng& rng) {
    static const uint64_t kVals[] = {0, 1, 0xFFFFFFFFFFFFFFFFull, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0x100000000ull,
                                     0x8000000000000000ull, 0x7FFFFFFFFFFFFFFFull, 0xFFFFFFFF00000000ull};
    return rng.chance(50) ? kVals[rng.below(std::size(kVals))] : rng.u64();
}

// One random ALU instruction (no memory access) that does not write x28.
uint32_t random_alu(Rng& rng, uint64_t pc) {
    static const unsigned kAlu[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14};  // indices into kGroups
    for (;;) {
        const Group& g = kGroups[kAlu[rng.below(std::size(kAlu))]];
        uint32_t w = (rng.u32() & ~g.mask) | g.value;
        if (a64::decode(w, pc).op == a64::Op::Invalid) continue;
        if ((w & 0x1F) == 28) w = set_field(w, 0, 5, 27);
        if (a64::decode(w, pc).op == a64::Op::Invalid) continue;
        return w;
    }
}

// Random structured program: a counted loop around a body of ALU
// instructions and forward branches of every kind.
GenCase random_cfg(Rng& rng, unsigned index) {
    GenCase c;
    c.group = "cfg_random";
    c.name = "cfg_random/" + std::to_string(index);
    c.seed = rng.u64();
    const unsigned body = 4 + static_cast<unsigned>(rng.below(12));
    const unsigned first = 1, tail = first + body;  // slot 0: counter, tail: SUB + CBNZ
    std::vector<uint32_t> code(tail + 3, 0);
    code[0] = movz(true, 28, 1 + static_cast<uint32_t>(rng.below(4)));
    for (unsigned k = first; k < tail; ++k) {
        const uint64_t pc = kCodeBase + 4 * k;
        if (!rng.chance(30)) {
            code[k] = random_alu(rng, pc);
            continue;
        }
        // Forward targets up to the SUB of the loop tail, sometimes straight to the sentinel.
        const unsigned target = rng.chance(10) ? tail + 2 : k + 1 + static_cast<unsigned>(rng.below(tail - k));
        const int64_t off = 4 * (static_cast<int64_t>(target) - static_cast<int64_t>(k));
        const unsigned r = static_cast<unsigned>(rng.below(32));
        switch (rng.below(6)) {
        case 0: code[k] = b(off); break;
        case 1:
        case 2: code[k] = bcond(static_cast<unsigned>(rng.below(16)), off); break;
        case 3: code[k] = cbz(rng.chance(50), rng.chance(50), r, off); break;
        default: code[k] = tbz(rng.chance(50), r, static_cast<unsigned>(rng.below(64)), off); break;
        }
    }
    code[tail] = 0xD1000000u | (1u << 10) | (28u << 5) | 28u;  // sub x28, x28, #1
    code[tail + 1] = cbz(true, true, 28, -4 * static_cast<int64_t>(tail + 1 - first));
    code[tail + 2] = kSentinel;
    c.code = code;
    return c;
}

std::vector<GenCase> templates(Rng& rng, unsigned per) {
    std::vector<GenCase> out;
    for (unsigned k = 0; k < per; ++k) {
        {  // B.cond over one instruction, all 16 conditions, random NZCV
            GenCase c = make("branch_cond", k, rng);
            c.code = {bcond(k % 16, 8), movz(true, 0, 1), kSentinel};
            c.overrides.push_back({32, (rng.u64() & 0xF) << 28});
            out.push_back(c);
        }
        {  // counted backward loop
            GenCase c = make("branch_loop", k, rng);
            const uint32_t n = 1 + static_cast<uint32_t>(rng.below(20));
            c.code = {movz(true, 1, n), add_imm(true, 0, 0, 3), subs_imm(true, 1, 1, 1), bcond(1 /*NE*/, -8), kSentinel};
            out.push_back(c);
        }
        {  // CBZ/CBNZ, 32 and 64 bit
            GenCase c = make("branch_cbz", k, rng);
            const unsigned r = static_cast<unsigned>(rng.below(31));
            const bool sf = rng.chance(50);
            c.code = {cbz(sf, rng.chance(50), r, 8), movz(true, r == 0 ? 1 : 0, 1), kSentinel};
            const uint64_t v = rng.chance(30) ? 0 : (rng.chance(30) ? (rng.u64() << 32) : interesting(rng));
            c.overrides.push_back({static_cast<uint8_t>(r), v});
            out.push_back(c);
        }
        {  // TBZ/TBNZ
            GenCase c = make("branch_tbz", k, rng);
            const unsigned r = static_cast<unsigned>(rng.below(31));
            c.code = {tbz(rng.chance(50), r, static_cast<unsigned>(rng.below(64)), 8), movz(true, r == 0 ? 1 : 0, 1),
                      kSentinel};
            out.push_back(c);
        }
        {  // B forward
            GenCase c = make("branch_b", k, rng);
            c.code = {b(8), movz(true, 0, 1), kSentinel};
            out.push_back(c);
        }
        {  // BL + RET into a separate function
            GenCase c = make("call_bl", k, rng);
            c.code = {bl(12), add_imm(true, 0, 0, 1), kSentinel, add_imm(true, 1, 1, 5), ret()};
            c.functions = {{kCodeBase, kCodeBase + 12, {}}, {kCodeBase + 12, kCodeBase + 20, {}}};
            out.push_back(c);
        }
        {  // BLR through ADR
            GenCase c = make("call_blr", k, rng);
            c.code = {adr(16, 12), blr(16), kSentinel, movz(true, 2, 7), ret()};
            c.functions = {{kCodeBase, kCodeBase + 12, {}}, {kCodeBase + 12, kCodeBase + 20, {}}};
            out.push_back(c);
        }
        {  // BR with a resolved in-function target (jump-table form)
            GenCase c = make("branch_br", k, rng);
            c.code = {adr(17, 12), br(17), movz(true, 0, 1), movz(true, 3, 9), kSentinel};
            c.functions = {{kCodeBase, kCodeBase + 20, {{kCodeBase + 4, {kCodeBase + 12}}}}};
            out.push_back(c);
        }
        {  // BR without a resolved target: runtime dispatch (tail call)
            GenCase c = make("branch_br_dispatch", k, rng);
            c.code = {bl(12), add_imm(true, 0, 0, 1), kSentinel, adr(17, 8), br(17), movz(true, 4, 3), ret()};
            c.functions = {{kCodeBase, kCodeBase + 12, {}}, {kCodeBase + 12, kCodeBase + 20, {}},
                           {kCodeBase + 20, kCodeBase + 28, {}}};
            out.push_back(c);
        }
        {  // nested calls with a frame on the stack
            GenCase c = make("call_nested", k, rng);
            c.code = {bl(8),
                      kSentinel,
                      stp_pre(29, 30, 31, -16),
                      bl(12),
                      ldp_post(29, 30, 31, 16),
                      ret(),
                      add_imm(true, 0, 0, 1),
                      ret()};
            c.functions = {{kCodeBase, kCodeBase + 8, {}}, {kCodeBase + 8, kCodeBase + 24, {}},
                           {kCodeBase + 24, kCodeBase + 32, {}}};
            memory_overrides(rng, c, 4);
            out.push_back(c);
        }
        {  // tail call: B to another function's entry
            GenCase c = make("call_tail", k, rng);
            c.code = {bl(12), add_imm(true, 0, 0, 1), kSentinel, b(4), add_imm(true, 5, 5, 2), ret()};
            c.functions = {{kCodeBase, kCodeBase + 12, {}}, {kCodeBase + 12, kCodeBase + 16, {}},
                           {kCodeBase + 16, kCodeBase + 24, {}}};
            out.push_back(c);
        }
        {  // SVC (handler returns 0 in x0) and svcExitProcess
            GenCase c = make("svc", k, rng);
            uint32_t imm = static_cast<uint32_t>(rng.below(0x80));
            if (k % 4 == 3) imm = 7;
            else if (imm == 7) imm = 8;
            c.code = {svc(imm), add_reg(true, 6, 0, 1), kSentinel};
            out.push_back(c);
        }
        {  // system registers readable/writable from EL0
            GenCase c = make("sysreg", k, rng);
            const unsigned r = static_cast<unsigned>(rng.below(31));
            const unsigned r2 = (r + 1 + static_cast<unsigned>(rng.below(29))) % 31;
            switch (k % 7) {
            case 0: c.code = {msr(a64::sr::kNzcv, r), mrs(a64::sr::kNzcv, r2)}; break;
            case 1: c.code = {mrs(a64::sr::kNzcv, r)}; break;
            case 2: c.code = {msr(a64::sr::kTpidrEl0, r), mrs(a64::sr::kTpidrEl0, r2)}; break;
            case 3: c.code = {mrs(a64::sr::kTpidrroEl0, r)}; break;
            case 4: c.code = {mrs(a64::sr::kCtrEl0, r), mrs(a64::sr::kDczidEl0, r2)}; break;
            case 5: {
                const uint32_t b = kBaseRegs[rng.below(9)];
                c.code = {0xD503201Fu /*nop*/, 0xD503203Fu /*yield*/, 0xD5033BBFu /*dmb ish*/,
                          0xD5033F9Fu /*dsb sy*/, 0xD5033FDFu /*isb*/, clrex(),
                          0xD50B7A20u | b /*dc cvac*/, 0xD50B7B20u | b /*dc cvau*/, 0xD50B7E20u | b /*dc civac*/,
                          0xD50B7520u | b /*ic ivau*/};
                memory_overrides(rng, c, 0);
                break;
            }
            case 6: c.code = {msr(a64::sr::kFpcr, r), mrs(a64::sr::kFpcr, r2)}; break;
            }
            c.code.push_back(kSentinel);
            out.push_back(c);
        }
        {  // FPSR round trip, separate group: implementation-defined bits
            GenCase c = make("sysreg_fpsr", k, rng);
            const unsigned r = static_cast<unsigned>(rng.below(31));
            c.code = {msr(a64::sr::kFpsr, r), mrs(a64::sr::kFpsr, (r + 1) % 31), kSentinel};
            out.push_back(c);
        }
        {  // exclusives: LDXR/STXR success, failure without monitor, CLREX
            GenCase c = make("exclusive", k, rng);
            const unsigned size = static_cast<unsigned>(rng.below(4));
            const unsigned base = kBaseRegs[rng.below(9)];  // not sp
            const unsigned rt = static_cast<unsigned>(rng.below(8));
            const unsigned rs = 8 + static_cast<unsigned>(rng.below(4));
            const unsigned rv = 12 + static_cast<unsigned>(rng.below(4));
            const bool acq = rng.chance(50), rel = rng.chance(50);
            switch (k % 4) {
            case 0: c.code = {ldxr(size, acq, rt, base), add_imm(true, rv, rt, 1), stxr(size, rel, rs, rv, base)}; break;
            case 1: c.code = {stxr(size, rel, rs, rv, base)}; break;
            case 2: c.code = {ldxr(size, acq, rt, base), clrex(), stxr(size, rel, rs, rv, base)}; break;
            case 3: c.code = {ldar(size, rt, base), stlr(size, rv, base), ldar(size, rs, base)}; break;
            }
            c.code.push_back(kSentinel);
            memory_overrides(rng, c, 4);
            out.push_back(c);
        }
        {  // exclusive pairs
            GenCase c = make("exclusive_pair", k, rng);
            const bool x = rng.chance(50);
            const unsigned base = kBaseRegs[rng.below(9)];
            const bool acq = rng.chance(50), rel = rng.chance(50);
            if (k % 2 == 0)
                c.code = {ldxp(x, acq, 0, 1, base), add_imm(true, 2, 0, 1), add_imm(true, 3, 1, 2),
                          stxp(x, rel, 4, 2, 3, base)};
            else
                c.code = {stxp(x, rel, 4, 2, 3, base)};
            c.code.push_back(kSentinel);
            memory_overrides(rng, c, 4);
            out.push_back(c);
        }
        {  // entry is not the lowest address of its function
            GenCase c = make("entry_not_lowest", k, rng);
            c.code = {add_imm(true, 2, 2, 1), kSentinel, add_imm(true, 3, 3, 2), b(-12)};
            c.functions = {{kCodeBase + 8, kCodeBase + 16, {}, kCodeBase}};
            c.entry = kCodeBase + 8;
            out.push_back(c);
        }
        {  // undefined instructions: UDF, HLT and random unallocated words
            GenCase c = make("undefined", k, rng);
            uint32_t w = 0;
            if (k % 4 == 0) {
                w = static_cast<uint32_t>(rng.below(0x10000));  // UDF #imm16
            } else if (k % 4 == 1) {
                w = 0xD4400000u | (static_cast<uint32_t>(rng.below(0x10000)) << 5);  // HLT #imm16
            } else {
                do {
                    w = rng.u32();
                } while (a64::decode(w, kCodeBase).invalid != a64::Invalid::Unallocated);
            }
            c.code = {add_imm(true, 0, 0, 1), w, kSentinel};
            out.push_back(c);
        }
        {  // faulting access between valid stores: nothing after it may take effect
            GenCase c = make("mem_fault", k, rng);
            uint32_t bad = 0;
            bool to_rodata = false;
            switch (k % 6) {
            case 0: bad = ldr_x(5, 29); break;
            case 1: bad = str_x(5, 29); to_rodata = true; break;  // write to a read-only region
            case 2: bad = ldp_post(5, 6, 29, 16); break;          // no writeback either
            case 3: bad = rng.chance(50) ? ldxr(3, false, 5, 29) : ldar(2, 5, 29); break;
            case 4: bad = 0x4CDF73A0u; break;                     // ld1 {v0.16b}, [x29], #16 (SIMD structure)
            default: bad = 0x4C0073A0u; to_rodata = true; break;  // st1 {v0.16b}, [x29]
            }
            c.code = {str_x(0, 20), add_imm(true, 1, 1, 1), bad, add_imm(true, 2, 2, 1), str_x(3, 21), kSentinel};
            memory_overrides(rng, c, 3);
            const uint64_t addr = to_rodata ? lgp_test::kRodataBase + 8 * rng.below(0x100)
                                            : 0x300000 + 8 * rng.below(0x1000);  // unmapped
            c.overrides.push_back({29, addr});
            out.push_back(c);
        }
        {  // DC ZVA
            GenCase c = make("dc_zva", k, rng);
            c.code = {dc_zva(kBaseRegs[rng.below(9)]), kSentinel};
            memory_overrides(rng, c, 0);
            out.push_back(c);
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Output
// ---------------------------------------------------------------------------
std::string hex(uint64_t v) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "0x%" PRIx64, v);
    return buf;
}

std::string emit_case(const GenCase& c, size_t id, LiftStats& stats, std::string& table_row) {
    const std::string ns = "c" + std::to_string(id);
    std::vector<GenFunction> fns = c.functions;
    if (fns.empty()) fns.push_back({kCodeBase, kCodeBase + 4 * c.code.size(), {}});
    std::vector<uint64_t> entries;
    for (const GenFunction& f : fns) entries.push_back(f.entry);

    const std::vector<uint32_t>& code = c.code;
    Lifter lifter(
        [&code](uint64_t va, uint32_t& insn) {
            if (va < kCodeBase || va >= kCodeBase + 4 * code.size() || (va & 3)) return false;
            insn = code[(va - kCodeBase) / 4];
            return true;
        },
        entries, [] {
            lgp::recomp::LiftOptions o;
            o.precise_fault_pc = true;  // mem_fault cases compare the pc of the faulting access
            return o;
        }());

    std::string out = "namespace " + ns + " {\n";
    out += lifter.declarations();
    for (const GenFunction& f : fns) {
        LiftFunction lf;
        lf.entry = f.entry;
        lf.blocks.push_back({f.start ? f.start : f.entry, f.end});
        lf.indirect_targets = f.indirect_targets;
        out += lifter.lift(lf, stats);
    }
    out += "lgp::LiftedFn lookup(uint64_t va) {\n    switch (va) {\n";
    for (uint64_t e : entries)
        out += "    case " + hex(e) + ": return &" + lifter.function_name(e) + ";\n";
    out += "    default: return nullptr;\n    }\n}\n";
    out += "const uint32_t code[] = {";
    for (uint32_t w : c.code)
        out += hex(w) + "u, ";
    out += "};\n";
    out += "const lgp_test::RegOverride overrides[] = {";
    for (const auto& o : c.overrides)
        out += "{" + std::to_string(o.reg) + ", " + hex(o.value) + "ull}, ";
    if (c.overrides.empty()) out += "{0, 0}";
    out += "};\n";
    out += "}  // namespace " + ns + "\n\n";

    table_row = "    {\"" + c.group + "\", \"" + c.name + "\", " + hex(c.seed) + "ull, " + ns + "::code, " +
                std::to_string(c.code.size()) + ", " + ns + "::overrides, " + std::to_string(c.overrides.size()) +
                ", " + hex(c.entry) + "ull, &" + ns + "::" + lifter.function_name(c.entry) + ", &" + ns + "::lookup},\n";
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 5) {
        std::fprintf(stderr, "usage: %s <out_dir> <shards> <cases_per_group> <seed>\n", argv[0]);
        return 2;
    }
    const std::string out_dir = argv[1];
    const unsigned shards = static_cast<unsigned>(std::strtoul(argv[2], nullptr, 10));
    const unsigned per_group = static_cast<unsigned>(std::strtoul(argv[3], nullptr, 10));
    Rng rng(std::strtoull(argv[4], nullptr, 0));
    if (shards == 0) return 2;

    std::vector<GenCase> cases;
    for (const Group& g : kGroups)
        for (unsigned k = 0; k < per_group; ++k)
            cases.push_back(random_case(rng, g, k));
    for (GenCase& c : templates(rng, per_group / 4 > 16 ? per_group / 4 : 16))
        cases.push_back(std::move(c));
    for (unsigned k = 0; k < per_group; ++k)
        cases.push_back(random_cfg(rng, k));

    LiftStats stats;
    for (unsigned s = 0; s < shards; ++s) {
        std::string body =
            "// Generated by lgp_lifter_casegen. Do not edit.\n"
            "#include \"case_defs.hpp\"\n"
            "#include \"lgp/lifted/lifted.hpp\"\n\n";
        std::string table = "namespace lgp_test {\nextern const CaseDef kShard" + std::to_string(s) + "[] = {\n";
        size_t count = 0;
        for (size_t id = s; id < cases.size(); id += shards) {
            std::string row;
            body += emit_case(cases[id], id, stats, row);
            table += row;
            ++count;
        }
        if (count == 0) table += "    {nullptr, nullptr, 0, nullptr, 0, nullptr, 0, 0, nullptr, nullptr},\n";
        table += "};\nextern const size_t kShard" + std::to_string(s) + "Count = " + std::to_string(count) +
                 ";\n}  // namespace lgp_test\n";
        std::ofstream f(out_dir + "/shard_" + std::to_string(s) + ".cpp");
        f << body << table;
        if (!f) {
            std::fprintf(stderr, "cannot write shard %u\n", s);
            return 1;
        }
    }

    std::string index = "// Generated by lgp_lifter_casegen. Do not edit.\n#include \"case_defs.hpp\"\n\nnamespace lgp_test {\n";
    for (unsigned s = 0; s < shards; ++s)
        index += "extern const CaseDef kShard" + std::to_string(s) + "[];\nextern const size_t kShard" +
                 std::to_string(s) + "Count;\n";
    index += "extern const CaseShard kAllShards[] = {\n";
    for (unsigned s = 0; s < shards; ++s)
        index += "    {kShard" + std::to_string(s) + ", kShard" + std::to_string(s) + "Count},\n";
    index += "};\nextern const size_t kShardCount = " + std::to_string(shards) + ";\n";
    index += "extern const char* const kLifterStats = \"cases=" + std::to_string(cases.size()) +
             " instructions=" + std::to_string(stats.instructions) + " unsupported=" + std::to_string(stats.unsupported) +
             "\";\n}  // namespace lgp_test\n";
    std::ofstream f(out_dir + "/index.cpp");
    f << index;
    for (const auto& [op, n] : stats.by_op)
        std::printf("  %-12s %6" PRIu64 "\n", op.c_str(), n);
    // Acceptance criterion of P2.3: every operation the lifter implements is
    // covered by the differential test. Fail the build if one goes missing.
    bool missing = false;
    for (uint16_t k = 1; k < static_cast<uint16_t>(a64::Op::kCount); ++k) {
        if (static_cast<a64::Op>(k) == a64::Op::Simd) continue;  // covered by tests/lifter_simd (P2.4)
        const char* name = a64::op_name(static_cast<a64::Op>(k));
        auto it = stats.by_op.find(name);
        const uint64_t n = it == stats.by_op.end() ? 0 : it->second;
        if (n < 5) {
            std::fprintf(stderr, "lgp_lifter_casegen: operation %s is covered by only %" PRIu64 " instructions\n", name, n);
            missing = true;
        }
    }
    if (missing) return 1;
    std::printf("lgp_lifter_casegen: %zu cases, %" PRIu64 " lifted instructions, %" PRIu64 " unsupported\n", cases.size(),
                stats.instructions, stats.unsupported);
    return f ? 0 : 1;
}
