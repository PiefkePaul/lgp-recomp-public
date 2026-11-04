#include "fuzz_gen.hpp"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <sstream>

#include "casegen_common.hpp"
#include "int_groups.hpp"
#include "lgp/recomp/a64_simd.hpp"

namespace simdgen {
#include "groups_fp_scalar.inc"
#include "groups_vec_int.inc"
#include "groups_vec_fp.inc"
#include "groups_vec_perm.inc"
#include "groups_ldst_struct.inc"
#include "groups_crypto.inc"
}  // namespace simdgen

namespace lgp_fuzz {

namespace {

namespace a64 = lgp::recomp::a64;
namespace intgen = lgp_test::intgen;
using lgp_test::kCodeBase;
using lgp_test::RegOverride;
using simdgen::Rng;

std::vector<const simdgen::Group*> simd_groups(simdgen::Kind kind) {
    // Built on first use, not as a namespace-scope array: spans over the inline
    // group vectors taken during static initialisation came out empty in
    // optimised clang builds.
    const std::span<const simdgen::Group> families[] = {simdgen::kFpScalarGroups,   simdgen::kVecIntGroups,
                                                        simdgen::kVecFpGroups,      simdgen::kVecPermGroups,
                                                        simdgen::kLdStStructGroups, simdgen::kCryptoGroups};
    std::vector<const simdgen::Group*> out;
    for (const auto& family : families)
        for (const simdgen::Group& g : family)
            if (g.kind == kind) out.push_back(&g);
    return out;
}

const std::vector<const simdgen::Group*>& simd_alu() {
    static const auto v = simd_groups(simdgen::Kind::Alu);
    return v;
}
const std::vector<const simdgen::Group*>& simd_mem() {
    static const auto v = simd_groups(simdgen::Kind::Mem);
    return v;
}

bool valid(uint32_t w, uint64_t pc) { return a64::decode(w, pc).op != a64::Op::Invalid; }

// Bias the register fields of an ALU word towards a few hot registers, so
// that results feed later instructions (flags and values flow through the
// sequence instead of every instruction reading fresh random inputs).
uint32_t hot_registers(Rng& rng, uint32_t w) {
    if (!rng.chance(60)) return w;
    w = intgen::set_field(w, 0, 5, static_cast<uint32_t>(rng.below(6)));
    if (rng.chance(70)) w = intgen::set_field(w, 5, 5, static_cast<uint32_t>(rng.below(6)));
    if (rng.chance(50)) w = intgen::set_field(w, 16, 5, static_cast<uint32_t>(rng.below(6)));
    return w;
}

// Loop counter of looping cases; like the base registers never a destination.
constexpr unsigned kLoopReg = 29;

// Highest destination register a generated instruction may write: x29 is the
// loop counter, and a callee must not change its return address in x30.
unsigned max_dest(bool callee) { return callee ? 30 : kLoopReg; }

// Keep the base and index registers of memory instructions and the loop
// counter (in a callee also x30) intact: integer destinations x18 and up and SP
// move to x0..x17.
bool protect_int(Rng& rng, uint32_t& w, uint64_t pc, bool callee) {
    const a64::Inst i = a64::decode(w, pc);
    if (i.op == a64::Op::Invalid) return false;
    const bool writes_base = (i.rd >= 18 && i.rd <= max_dest(callee)) || (i.rd == 31 && i.rd_sp);
    if (writes_base) w = intgen::set_field(w, 0, 5, static_cast<uint32_t>(rng.below(18)));
    return valid(w, pc);
}

// SIMD&FP words write X registers only through bits 4:0 (FMOV, UMOV, FCVTZS,
// ...): move 18..29 out of the way for every word except the FP compares,
// whose bits 4:0 are opcode/flags.
bool protect_simd(Rng& rng, uint32_t& w, const simdgen::Group& g, uint64_t pc, bool callee) {
    const bool flags_field = !std::strcmp(g.name, "fp_cmp") || !std::strcmp(g.name, "fp_ccmp");
    const uint32_t rd = w & 31;
    if (!flags_field && rd >= 18 && rd <= max_dest(callee))
        w = intgen::set_field(w, 0, 5, static_cast<uint32_t>(rng.below(18)));
    return valid(w, pc);
}

// Picks a group first and retries inside it, so groups with few valid
// encodings (dp1, dp2, ...) are as frequent as the others.
const intgen::Group& pick_int_group(Rng& rng, bool alu) {
    for (;;) {
        const intgen::Group& g = intgen::kGroups[rng.below(std::size(intgen::kGroups))];
        if ((g.kind == intgen::Kind::Alu) == alu) return g;
    }
}

uint32_t random_int_alu(Rng& rng, uint64_t pc, bool callee) {
    const intgen::Group& g = pick_int_group(rng, true);
    for (;;) {
        uint32_t w = (rng.u32() & ~g.mask) | g.value;
        if (!std::strcmp(g.name, "dp3") && rng.chance(30)) w |= 0x1Fu << 10;  // MUL/SMULH/UMULH forms
        w = hot_registers(rng, w);
        if (!valid(w, pc) || !protect_int(rng, w, pc, callee)) continue;
        return w;
    }
}

uint32_t random_int_mem(Rng& rng, uint64_t pc) {
    const intgen::Group& g = pick_int_group(rng, false);
    for (;;) {
        uint32_t w = (rng.u32() & ~g.mask) | g.value;
        if (!valid(w, pc)) continue;
        if (g.kind == intgen::Kind::Mem && !intgen::patch_memory(rng, w, pc)) continue;
        if (g.kind == intgen::Kind::Literal && !intgen::patch_literal(rng, w, pc)) continue;
        return w;
    }
}

uint32_t random_simd(Rng& rng, const std::vector<const simdgen::Group*>& groups, simdgen::GenCase& scratch,
                     uint64_t pc, bool callee) {
    for (;;) {
        const simdgen::Group& g = *groups[rng.below(groups.size())];
        for (int attempt = 0; attempt < 1000; ++attempt) {
            uint32_t w = (rng.u32() & ~g.mask) | g.value;
            if (g.kind == simdgen::Kind::Alu) w = hot_registers(rng, w);
            if (!valid(w, pc)) continue;
            if (g.patch && !g.patch(rng, w, scratch)) continue;
            if (!valid(w, pc)) continue;
            if (g.kind == simdgen::Kind::Alu && !protect_simd(rng, w, g, pc, callee)) continue;
            return w;
        }
    }
}

uint32_t movz_hw1(unsigned rd, uint32_t imm16) { return 0xD2A00000u | ((imm16 & 0xFFFF) << 5) | rd; }
uint32_t msr(uint32_t sysreg, unsigned rt) { return 0xD5100000u | (sysreg << 5) | rt; }
uint32_t mrs(uint32_t sysreg, unsigned rt) { return 0xD5300000u | (sysreg << 5) | rt; }

// Sets all four flags: MSR NZCV, xN or CMP xN, #imm. Follows every generated
// BL and starts every callee, so no code reads the flags where AAPCS64 leaves
// them undefined (LiftOptions::flags_across_calls).
uint32_t set_flags(Rng& rng) {
    const unsigned r = static_cast<unsigned>(rng.below(6));
    if (rng.chance(50)) return msr(a64::sr::kNzcv, r);
    return 0xF100001Fu | (static_cast<uint32_t>(rng.below(4096)) << 10) | (r << 5);  // subs xzr, xN, #imm
}

// After every generated BL: overwrite the registers AAPCS64 leaves corruptible
// and the release shape does not write back at RET (x9-x17, v4-v7, v16-v31,
// LiftOptions::registers_across_calls), so no caller reads a callee's values
// there. The flags follow (set_flags()).
void clobber_after_call(Rng& rng, std::vector<uint32_t>& code) {
    for (unsigned r = 9; r <= 17; ++r)
        code.push_back(0xD2800000u | (static_cast<uint32_t>(rng.below(0x10000)) << 5) | r);  // movz xR, #imm
    for (unsigned r = 4; r < 32; ++r)
        if (r < 8 || r >= 16) code.push_back(0x6F00E400u | r);  // movi vR.2d, #0
}

uint32_t forward_branch(Rng& rng, int64_t off) {
    const unsigned r = rng.chance(70) ? static_cast<unsigned>(rng.below(6)) : static_cast<unsigned>(rng.below(32));
    switch (rng.below(6)) {
    case 0: return 0x14000000u | (static_cast<uint32_t>(off / 4) & 0x3FFFFFF);  // B
    case 1:
    case 2: return 0x54000000u | ((static_cast<uint32_t>(off / 4) & 0x7FFFF) << 5) | static_cast<uint32_t>(rng.below(16));
    case 3:  // CBZ/CBNZ, 32 or 64 bit
        return (rng.chance(50) ? 0x80000000u : 0) | 0x34000000u | (rng.chance(50) ? 0x01000000u : 0) |
               ((static_cast<uint32_t>(off / 4) & 0x7FFFF) << 5) | r;
    default: {  // TBZ/TBNZ
        const unsigned bit = static_cast<unsigned>(rng.below(64));
        return ((bit >> 5) << 31) | 0x36000000u | (rng.chance(50) ? 0x01000000u : 0) | ((bit & 31) << 19) |
               ((static_cast<uint32_t>(off / 4) & 0x3FFF) << 5) | r;
    }
    }
}

std::string hex(uint64_t v) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "0x%" PRIx64, v);
    return buf;
}

std::string reg_name(uint8_t reg) {
    if (reg < 31) return "x" + std::to_string(reg);
    if (reg == 31) return "sp";
    if (reg == 32) return "nzcv";
    if (reg == lgp_test::kRegFpcr) return "fpcr";
    if (reg == lgp_test::kRegFpsr) return "fpsr";
    if (reg >= lgp_test::kRegVLo && reg < lgp_test::kRegVLo + 32) return "v" + std::to_string(reg - lgp_test::kRegVLo) + ".lo";
    if (reg >= lgp_test::kRegVHi && reg < lgp_test::kRegVHi + 32) return "v" + std::to_string(reg - lgp_test::kRegVHi) + ".hi";
    return "r" + std::to_string(reg);
}

bool parse_reg(const std::string& s, uint8_t& reg) {
    if (s == "sp") return reg = 31, true;
    if (s == "nzcv") return reg = 32, true;
    if (s == "fpcr") return reg = lgp_test::kRegFpcr, true;
    if (s == "fpsr") return reg = lgp_test::kRegFpsr, true;
    char* end = nullptr;
    if (s.size() > 1 && s[0] == 'x') {
        const unsigned long n = std::strtoul(s.c_str() + 1, &end, 10);
        if (*end || n > 30) return false;
        return reg = static_cast<uint8_t>(n), true;
    }
    if (s.size() > 4 && s[0] == 'v') {
        const unsigned long n = std::strtoul(s.c_str() + 1, &end, 10);
        if (n > 31) return false;
        if (!std::strcmp(end, ".lo")) return reg = static_cast<uint8_t>(lgp_test::kRegVLo + n), true;
        if (!std::strcmp(end, ".hi")) return reg = static_cast<uint8_t>(lgp_test::kRegVHi + n), true;
    }
    return false;
}

bool parse_u64(const std::string& s, uint64_t& v) {
    if (s.empty()) return false;
    char* end = nullptr;
    v = std::strtoull(s.c_str(), &end, 0);
    return *end == 0;
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (char ch : s) {
        if (ch == sep) {
            out.push_back(cur);
            cur.clear();
        } else {
            cur += ch;
        }
    }
    out.push_back(cur);
    return out;
}

}  // namespace

uint64_t case_seed(uint64_t run_seed, uint64_t index) {
    lgp_test::SplitMix sm(run_seed ^ (index * 0xD1B54A32D192ED03ull));
    return sm.next();
}

FuzzCase generate(uint64_t seed, const GenOptions& options) {
    Rng rng(seed);
    FuzzCase c;
    c.name = "fuzz/" + hex(seed);
    c.seed = rng.u64();

    // Some cases run their body 1..3 times in a counted loop (x29), so values,
    // flags and FPCR changes are carried around a back edge.
    const bool loop = rng.chance(20);
    // Some cases call a second function (BL ... RET) placed after the sentinel,
    // so registers held in locals cross real calls and returns.
    const bool with_callee = rng.chance(25);
    // Lifted with AAPCS64 flags (release shape) such a case's caller starts
    // with undefined flags, too: it sets them first.
    if (with_callee) c.code.push_back(set_flags(rng));
    if (loop) c.code.push_back(0xD2800000u | ((1 + static_cast<uint32_t>(rng.below(3))) << 5) | kLoopReg);  // movz

    // Branch slots are filled after the length is known (forward targets only,
    // so every case terminates). Call slots once the callee's address is known.
    std::vector<size_t> branch_slots;
    std::vector<size_t> call_slots;
    simdgen::GenCase scratch;  // overrides some SIMD patch functions add (none today)
    // Appends `len` random instructions; a callee contains no calls.
    auto body = [&](unsigned len, bool callee) {
        const size_t first = c.code.size();
        while (c.code.size() < first + len) {
            const uint64_t pc = kCodeBase + 4 * c.code.size();
            const uint64_t pick = rng.below(100);
            if (pick < 30) {
                c.code.push_back(random_int_alu(rng, pc, callee));
            } else if (pick < 44) {
                c.code.push_back(random_int_mem(rng, pc));
            } else if (pick < 80) {
                c.code.push_back(random_simd(rng, simd_alu(), scratch, pc, callee));
            } else if (pick < 88) {
                c.code.push_back(random_simd(rng, simd_mem(), scratch, pc, callee));
            } else if (pick < 95) {
                branch_slots.push_back(c.code.size());
                c.code.push_back(0);
            } else {
                const unsigned rt = static_cast<unsigned>(rng.below(18));
                switch (rng.below(with_callee && !callee ? 6 : 5)) {
                case 0:  // FPCR changes mid-sequence: rounding mode, FZ, DN, AHP
                    c.code.push_back(movz_hw1(17, simdgen::random_fpcr(rng) >> 16));
                    c.code.push_back(msr(a64::sr::kFpcr, 17));
                    break;
                case 1: c.code.push_back(msr(a64::sr::kNzcv, rt)); break;
                case 2: c.code.push_back(mrs(a64::sr::kNzcv, rt)); break;
                case 3:  // SVC: registers go back to CpuState and x0..x7 change (fuzz_run.cpp)
                    c.code.push_back(0xD4000001u | ((0x10u + static_cast<uint32_t>(rng.below(16))) << 5));
                    break;
                case 5:
                    call_slots.push_back(c.code.size());
                    c.code.push_back(0);
                    clobber_after_call(rng, c.code);
                    c.code.push_back(set_flags(rng));
                    break;
                default: c.code.push_back(mrs(a64::sr::kFpcr, rt)); break;
                }
            }
        }
    };
    // Forward branches of [from, slots) end anywhere after themselves up to
    // `last` (inclusive).
    auto fill_branches = [&](size_t from, size_t last, size_t tail, bool loop_tail) {
        for (size_t n = from; n < branch_slots.size(); ++n) {
            const size_t k = branch_slots[n];
            size_t target = k + 1 + static_cast<size_t>(rng.below(tail - k));
            if (target == tail && (!loop_tail || rng.chance(30))) target = last;
            c.code[k] = forward_branch(rng, 4 * static_cast<int64_t>(target - k));
        }
    };

    // Caller and callee together stay well inside the code page (kCodeSize).
    const unsigned max_len = std::clamp(options.max_len, 1u, 300u);
    const size_t body_start = c.code.size();
    const unsigned len = 1 + static_cast<unsigned>(rng.below(max_len));
    body(len, false);
    if (with_callee && call_slots.empty()) {  // at least one call
        call_slots.push_back(c.code.size());
        c.code.push_back(0);
        clobber_after_call(rng, c.code);
        c.code.push_back(set_flags(rng));
    }
    // Forward targets end at the loop tail (SUB) or at the sentinel; never at
    // the CBNZ, which would skip the decrement.
    const size_t tail = c.code.size();
    if (loop) {
        c.code.push_back(0xD1000400u | (kLoopReg << 5) | kLoopReg);  // sub x29, x29, #1
        const int64_t back = -4 * static_cast<int64_t>(c.code.size() - body_start);
        c.code.push_back(0xB5000000u | ((static_cast<uint32_t>(back / 4) & 0x7FFFF) << 5) | kLoopReg);  // cbnz x29
    }
    const size_t end = c.code.size();  // index of the sentinel
    fill_branches(0, end, tail, loop);

    if (with_callee) {
        // The caller's sentinel, then the callee: flags first (undefined at
        // entry), a body, RET. Its branches end at the RET.
        c.code.push_back(kSentinel);
        const size_t callee = c.code.size();
        c.code.push_back(set_flags(rng));
        const size_t callee_branches = branch_slots.size();
        body(1 + static_cast<unsigned>(rng.below(max_len)), true);
        const size_t ret = c.code.size();
        c.code.push_back(kRet);
        fill_branches(callee_branches, ret, ret, false);
        for (size_t k : call_slots)
            c.code[k] = 0x94000000u | (static_cast<uint32_t>(callee - k) & 0x3FFFFFF);  // BL callee
    }

    // Initial state on top of the seed's random registers.
    intgen::memory_overrides(rng, c, 0);
    for (uint8_t r = 0; r < 32; ++r) {
        const simdgen::VInit mode = static_cast<simdgen::VInit>(rng.below(3));
        if (mode == simdgen::VInit::Random) continue;  // random bits come from the seed
        uint64_t lo, hi;
        simdgen::vector_value(rng, mode, lo, hi);
        c.overrides.push_back({static_cast<uint8_t>(lgp_test::kRegVLo + r), lo});
        c.overrides.push_back({static_cast<uint8_t>(lgp_test::kRegVHi + r), hi});
    }
    const uint32_t fpcr = simdgen::random_fpcr(rng);
    if (fpcr) c.overrides.push_back({lgp_test::kRegFpcr, fpcr});
    if (rng.chance(20)) c.overrides.push_back({lgp_test::kRegFpsr, 1u << 27});  // QC already set
    for (const auto& o : scratch.overrides)
        c.overrides.push_back(o);
    return c;
}

std::string emit_cases(const std::vector<FuzzCase>& cases, const std::string& table, const std::string& group,
                       lgp::recomp::LiftStats& stats, bool precise) {
    std::string out =
        "// Generated by the lifter fuzzer (tests/lifter_fuzz). Do not edit.\n"
        "#include \"case_defs.hpp\"\n"
        "#include \"lgp/lifted/lifted.hpp\"\n\n";
    std::string rows;
    for (size_t id = 0; id < cases.size(); ++id) {
        const FuzzCase& c = cases[id];
        std::vector<uint32_t> code = c.code;
        code.push_back(kSentinel);
        lgp::recomp::LiftOptions opts;
        opts.precise_fault_pc = precise;  // faults compare the pc of the faulting access
        // Caller [kCodeBase, inner sentinel], callee after it (callee_split()).
        const size_t split = callee_split(c.code);
        // The release shape is the game's: flags and registers follow AAPCS64 at
        // calls. Cases without a callee read the initial flags, so only those
        // with one.
        opts.flags_across_calls = precise || split == SIZE_MAX;
        opts.registers_across_calls = opts.flags_across_calls;
        const uint64_t callee = split == SIZE_MAX ? 0 : kCodeBase + 4 * (split + 1);
        std::vector<uint64_t> entries{kCodeBase};
        if (callee) entries.push_back(callee);
        lgp::recomp::Lifter lifter(
            [&code](uint64_t va, uint32_t& insn) {
                if (va < kCodeBase || va >= kCodeBase + 4 * code.size() || (va & 3)) return false;
                insn = code[(va - kCodeBase) / 4];
                return true;
            },
            entries, opts);
        const std::string ns = table + "_" + std::to_string(id);
        out += "namespace " + ns + " {\n" + lifter.declarations();
        const uint64_t code_end = kCodeBase + 4 * code.size();
        lgp::recomp::LiftFunction lf;
        lf.entry = kCodeBase;
        lf.blocks.push_back({kCodeBase, callee ? callee : code_end});
        out += lifter.lift(lf, stats);
        const std::string fn = lifter.function_name(kCodeBase);
        std::string lookup = "va == " + hex(kCodeBase) + " ? &" + fn;
        if (callee) {
            lgp::recomp::LiftFunction cf;
            cf.entry = callee;
            cf.blocks.push_back({callee, code_end});
            out += lifter.lift(cf, stats);
            lookup += " : va == " + hex(callee) + " ? &" + lifter.function_name(callee);
        }
        out += "lgp::LiftedFn lookup(uint64_t va) { return " + lookup + " : nullptr; }\n";
        out += "const uint32_t code[] = {";
        for (uint32_t w : code)
            out += hex(w) + "u, ";
        out += "};\nconst lgp_test::RegOverride overrides[] = {";
        for (const auto& o : c.overrides)
            out += "{" + std::to_string(o.reg) + ", " + hex(o.value) + "ull}, ";
        if (c.overrides.empty()) out += "{0, 0}";
        out += "};\n}  // namespace " + ns + "\n\n";
        rows += "    {\"" + group + "\", \"" + c.name + "\", " + hex(c.seed) + "ull, " + ns + "::code, " +
                std::to_string(code.size()) + ", " + ns + "::overrides, " + std::to_string(c.overrides.size()) + ", " +
                hex(kCodeBase) + "ull, &" + ns + "::" + fn + ", &" + ns + "::lookup},\n";
    }
    if (cases.empty()) rows = "    {nullptr, nullptr, 0, nullptr, 0, nullptr, 0, 0, nullptr, nullptr},\n";
    out += "extern \"C\" const lgp_test::CaseDef " + table + "[] = {\n" + rows + "};\n";
    out += "extern \"C\" const size_t " + table + "_count = " + std::to_string(cases.size()) + ";\n";
    return out;
}

size_t callee_split(const std::vector<uint32_t>& code) {
    for (size_t k = 0; k + 1 < code.size(); ++k)
        if (code[k] == kSentinel) return k;
    return SIZE_MAX;
}

bool parse_regressions(const std::string& text, std::vector<FuzzCase>& out, std::string& error) {
    std::istringstream in(text);
    std::string line;
    unsigned lineno = 0;
    while (std::getline(in, line)) {
        ++lineno;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t first = line.find_first_not_of(" \t");
        if (first == std::string::npos || line[first] == '#') continue;
        std::istringstream words(line.substr(first));
        FuzzCase c;
        bool have_seed = false;
        std::string word;
        words >> c.name;
        auto fail = [&](const std::string& what) {
            error = "line " + std::to_string(lineno) + ": " + what;
            return false;
        };
        while (words >> word) {
            const size_t eq = word.find('=');
            if (eq == std::string::npos) return fail("expected key=value, got '" + word + "'");
            const std::string key = word.substr(0, eq), value = word.substr(eq + 1);
            if (key == "seed") {
                if (!parse_u64(value, c.seed)) return fail("bad seed");
                have_seed = true;
            } else if (key == "code") {
                for (const std::string& w : split(value, ',')) {
                    uint64_t v;
                    if (!parse_u64(w, v) || v > 0xFFFFFFFFu) return fail("bad code word '" + w + "'");
                    c.code.push_back(static_cast<uint32_t>(v));
                }
            } else if (key == "ov") {
                for (const std::string& item : split(value, ',')) {
                    const size_t colon = item.find(':');
                    lgp_test::RegOverride o{};
                    if (colon == std::string::npos || !parse_reg(item.substr(0, colon), o.reg) ||
                        !parse_u64(item.substr(colon + 1), o.value))
                        return fail("bad override '" + item + "'");
                    c.overrides.push_back(o);
                }
            } else {
                return fail("unknown key '" + key + "'");
            }
        }
        if (!have_seed || c.code.empty()) return fail("case needs seed= and code=");
        out.push_back(std::move(c));
    }
    return true;
}

std::string format_regression(const FuzzCase& c) {
    std::string s = c.name + " seed=" + hex(c.seed) + " code=";
    for (size_t k = 0; k < c.code.size(); ++k)
        s += (k ? "," : "") + hex(c.code[k]);
    if (!c.overrides.empty()) {
        s += " ov=";
        for (size_t k = 0; k < c.overrides.size(); ++k)
            s += (k ? "," : "") + reg_name(c.overrides[k].reg) + ":" + hex(c.overrides[k].value);
    }
    return s;
}

std::string listing(const std::vector<uint32_t>& code) {
    std::string out;
    for (size_t k = 0; k < code.size(); ++k) {
        const uint64_t pc = kCodeBase + 4 * k;
        const uint32_t w = code[k];
        char buf[48];
        std::snprintf(buf, sizeof(buf), "      %06" PRIx64 "  %08x  ", pc, w);
        out += buf;
        const a64::Inst i = a64::decode(w, pc);
        out += i.op == a64::Op::Simd ? std::string(a64::simd::lift(w, pc, false).name) : a64::describe(i);
        out += "\n";
    }
    return out;
}

}  // namespace lgp_fuzz
