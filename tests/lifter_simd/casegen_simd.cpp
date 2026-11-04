// Generates differential test cases for the SIMD&FP lifter (P2.4): random
// instruction words per encoding group (operand bits random, opcode bits
// fixed) with interesting FP/integer register contents and random FPCR
// settings, plus hand-built templates per family. Every case is lifted to
// C++; lgp_test_simd_difftest runs it in Unicorn and as lifted code.
//
// Usage: lgp_simd_casegen <out_dir> <shards> <cases_per_group> <seed>
// Writes shard_<n>.cpp and index.cpp. Deterministic for a given seed.
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "casegen_common.hpp"
#include "lgp/recomp/lifter.hpp"

namespace simdgen {
// Family files: each defines k<Family>Groups and <family>_templates().
#include "groups_fp_scalar.inc"
#include "groups_vec_int.inc"
#include "groups_vec_fp.inc"
#include "groups_vec_perm.inc"
#include "groups_ldst_struct.inc"
#include "groups_crypto.inc"
}  // namespace simdgen

namespace {

using namespace simdgen;
using lgp::recomp::LiftFunction;
using lgp::recomp::Lifter;
using lgp::recomp::LiftStats;

const std::span<const Group> kFamilies[] = {kFpScalarGroups, kVecIntGroups,     kVecFpGroups,
                                            kVecPermGroups,  kLdStStructGroups, kCryptoGroups};

using TemplateFn = void (*)(Rng&, unsigned, std::vector<GenCase>&);
const TemplateFn kTemplates[] = {fp_scalar_templates, vec_int_templates,     vec_fp_templates,
                                 vec_perm_templates,  ldst_struct_templates, crypto_templates};

void init_state(Rng& rng, const Group& g, GenCase& c) {
    if (g.vinit != VInit::Random) {
        for (unsigned r = 0; r < 32; ++r) {
            uint64_t lo, hi;
            vector_value(rng, g.vinit, lo, hi);
            c.overrides.push_back({static_cast<uint8_t>(lgp_test::kRegVLo + r), lo});
            c.overrides.push_back({static_cast<uint8_t>(lgp_test::kRegVHi + r), hi});
        }
    }
    if (g.fpcr == FpcrInit::Random) c.overrides.push_back({lgp_test::kRegFpcr, random_fpcr(rng)});
}

GenCase random_case(Rng& rng, const Group& g, unsigned index) {
    GenCase c;
    c.group = g.name;
    c.name = std::string(g.name) + "/" + std::to_string(index);
    c.seed = rng.u64();
    const unsigned len = 1 + static_cast<unsigned>(rng.below(g.max_len));
    // Single-instruction cases first, so every encoding is also tested alone.
    const unsigned n = index % 2 == 0 ? 1 : len;
    for (unsigned k = 0; k < n; ++k) {
        for (int attempt = 0; attempt < 100000; ++attempt) {
            uint32_t w = (rng.u32() & ~g.mask) | g.value;
            if (!valid(w)) continue;
            if (g.patch && !g.patch(rng, w, c)) continue;
            if (!valid(w)) continue;
            c.code.push_back(w);
            break;
        }
    }
    if (g.kind == Kind::Mem) memory_overrides(rng, c, 0);
    init_state(rng, g, c);
    c.code.push_back(kSentinel);
    return c;
}

std::string hex(uint64_t v) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "0x%" PRIx64, v);
    return buf;
}

std::string emit_case(const GenCase& c, size_t id, LiftStats& stats, std::string& table_row) {
    const std::string ns = "c" + std::to_string(id);
    const std::vector<uint32_t>& code = c.code;
    Lifter lifter(
        [&code](uint64_t va, uint32_t& insn) {
            if (va < kCodeBase || va >= kCodeBase + 4 * code.size() || (va & 3)) return false;
            insn = code[(va - kCodeBase) / 4];
            return true;
        },
        {kCodeBase});

    std::string out = "namespace " + ns + " {\n";
    out += lifter.declarations();
    LiftFunction lf;
    lf.entry = kCodeBase;
    lf.blocks.push_back({kCodeBase, kCodeBase + 4 * c.code.size()});
    out += lifter.lift(lf, stats);
    out += "lgp::LiftedFn lookup(uint64_t va) { return va == " + hex(kCodeBase) + " ? &" +
           lifter.function_name(kCodeBase) + " : nullptr; }\n";
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
                ", " + hex(kCodeBase) + "ull, &" + ns + "::" + lifter.function_name(kCodeBase) + ", &" + ns +
                "::lookup},\n";
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
    std::set<std::string> names;
    for (const auto& family : kFamilies) {
        for (const Group& g : family) {
            if (!names.insert(g.name).second) {
                std::fprintf(stderr, "duplicate group name %s\n", g.name);
                return 1;
            }
            for (unsigned k = 0; k < per_group; ++k) {
                GenCase c = random_case(rng, g, k);
                if (c.code.size() < 2) {
                    std::fprintf(stderr, "group %s: no valid encoding found\n", g.name);
                    return 1;
                }
                cases.push_back(std::move(c));
            }
        }
    }
    for (TemplateFn t : kTemplates)
        t(rng, per_group / 4 > 16 ? per_group / 4 : 16, cases);

    LiftStats stats;
    for (unsigned s = 0; s < shards; ++s) {
        std::string body =
            "// Generated by lgp_simd_casegen. Do not edit.\n"
            "#include \"case_defs.hpp\"\n"
            "#include \"lgp/lifted/lifted.hpp\"\n\n";
        std::string table = "namespace lgp_test {\nextern const CaseDef kSimdShard" + std::to_string(s) + "[] = {\n";
        size_t count = 0;
        for (size_t id = s; id < cases.size(); id += shards) {
            std::string row;
            body += emit_case(cases[id], id, stats, row);
            table += row;
            ++count;
        }
        if (count == 0) table += "    {nullptr, nullptr, 0, nullptr, 0, nullptr, 0, 0, nullptr, nullptr},\n";
        table += "};\nextern const size_t kSimdShard" + std::to_string(s) + "Count = " + std::to_string(count) +
                 ";\n}  // namespace lgp_test\n";
        std::ofstream f(out_dir + "/shard_" + std::to_string(s) + ".cpp");
        f << body << table;
        if (!f) {
            std::fprintf(stderr, "cannot write shard %u\n", s);
            return 1;
        }
    }

    std::string index =
        "// Generated by lgp_simd_casegen. Do not edit.\n#include \"case_defs.hpp\"\n\nnamespace lgp_test {\n";
    for (unsigned s = 0; s < shards; ++s)
        index += "extern const CaseDef kSimdShard" + std::to_string(s) + "[];\nextern const size_t kSimdShard" +
                 std::to_string(s) + "Count;\n";
    index += "extern const CaseShard kSimdShards[] = {\n";
    for (unsigned s = 0; s < shards; ++s)
        index += "    {kSimdShard" + std::to_string(s) + ", kSimdShard" + std::to_string(s) + "Count},\n";
    index += "};\nextern const size_t kSimdShardCount = " + std::to_string(shards) + ";\n";
    index += "extern const char* const kSimdLifterStats = \"cases=" + std::to_string(cases.size()) +
             " instructions=" + std::to_string(stats.instructions) + " unsupported=" +
             std::to_string(stats.unsupported) + "\";\n}  // namespace lgp_test\n";
    std::ofstream f(out_dir + "/index.cpp");
    f << index;
    for (const auto& [op, n] : stats.by_op)
        std::printf("  %-24s %6" PRIu64 "\n", op.c_str(), n);
    std::printf("lgp_simd_casegen: %zu cases, %" PRIu64 " lifted instructions, %" PRIu64 " unsupported\n",
                cases.size(), stats.instructions, stats.unsupported);
    return f ? 0 : 1;
}
