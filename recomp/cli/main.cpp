// lgp_recomp: command line for the analysis part of the recompiler.
//
//   lgp_recomp info <module...> [--json out.json] [--base 0x...]
//   lgp_recomp functions <module...> [--config game.cfg] [--json out.json] [--summary] [--diagnostics]
//   lgp_recomp check-functions <module> --elf <module.elf> [--config game.cfg] [--verbose]
//   lgp_recomp lift <module...> --out lifted.cpp [--config game.cfg] [--trace]
//   lgp_recomp census <module...> [--config game.cfg] [--json out.json]
//   lgp_recomp native-plan <module...> [--config game.cfg] [--out plan.tsv]
//   lgp_recomp native-rewrite <module...> [--config game.cfg] --out-dir DIR
//
// Output contains addresses, counts and names only, never code bytes, so a
// `--summary` run on real game data can be pasted into docs/analysis/.
// Exception: `lift` writes C++ translated from the module's code. Lifted game
// code belongs in $LGP_PRIVATE_DIR and never in the repository; only the
// statistics it prints to stderr may be shared.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "lgp/recomp/a64_cf.hpp"
#include "lgp/recomp/cfg.hpp"
#include "lgp/recomp/elf_symbols.hpp"
#include "lgp/recomp/json.hpp"
#include "lgp/recomp/lift_input.hpp"
#include "lgp/recomp/link.hpp"
#include "lgp/recomp/loader.hpp"
#include "lgp/recomp/native_census.hpp"
#include "lgp/recomp/native_image.hpp"
#include "lgp/recomp/native_rewrite.hpp"

using namespace lgp::recomp;

namespace {

const char* cf_kind_name(CfKind k) {
    static const char* kinds[] = {"none", "branch", "cond_branch", "call", "indirect_branch", "indirect_call",
                                  "return", "trap", "svc", "unknown"};
    return kinds[static_cast<int>(k)];
}


struct Args {
    std::string command;
    std::vector<std::string> inputs;
    std::string json_out, config, elf, md_out, out, out_dir;
    uint64_t base = 0x0800'0000;
    bool summary = false, diagnostics = false, verbose = false, no_hash = false, strict = false, trace = false;
};

void usage() {
    std::fprintf(stderr,
        "usage:\n"
        "  lgp_recomp info <module...> [--json out.json] [--base 0xADDR] [--no-hash]\n"
        "  lgp_recomp functions <module...> [--config game.cfg] [--json out.json] [--summary] [--diagnostics]\n"
        "  lgp_recomp check-functions <module> --elf <module.elf> [--config game.cfg] [--verbose]\n"
        "  lgp_recomp lift <module...> --out lifted.cpp [--config game.cfg] [--trace]\n"
        "      (C++ from the module's code: output from game data goes to $LGP_PRIVATE_DIR, never into the repo)\n"
        "  lgp_recomp census <module...> [--config game.cfg] [--json out.json]\n"
        "      (counts of the sites native execution would have to patch: x18, svc, tpidr*, counters, sp)\n"
        "  lgp_recomp native-plan <module...> [--config game.cfg] [--out plan.tsv]\n"
        "      (x18 renaming and stub sites; plan.tsv lists addresses and words of the module: $LGP_PRIVATE_DIR only)\n"
        "  lgp_recomp native-rewrite <module...> [--config game.cfg] --out-dir DIR\n"
        "      (patched NSOs, patch area and manifest for native execution: derived game code, $LGP_PRIVATE_DIR only;\n"
        "       stdout: counts)\n"
        "  lgp_recomp classify <hex-insn> [pc]      (debug: control-flow class of one instruction)\n");
}

bool parse_args(int argc, char** argv, Args& a) {
    if (argc < 2) return false;
    a.command = argv[1];
    for (int i = 2; i < argc; ++i) {
        std::string s = argv[i];
        auto next = [&](std::string& out) {
            if (i + 1 >= argc) throw FormatError("missing value for " + s);
            out = argv[++i];
        };
        if (s == "--json") next(a.json_out);
        else if (s == "--config") next(a.config);
        else if (s == "--elf") next(a.elf);
        else if (s == "--markdown") next(a.md_out);
        else if (s == "--out") next(a.out);
        else if (s == "--out-dir") next(a.out_dir);
        else if (s == "--trace") a.trace = true;
        else if (s == "--base") { std::string v; next(v); a.base = std::stoull(v, nullptr, 0); }
        else if (s == "--summary") a.summary = true;
        else if (s == "--diagnostics") a.diagnostics = true;
        else if (s == "--verbose") a.verbose = true;
        else if (s == "--no-hash") a.no_hash = true;
        else if (s == "--strict") a.strict = true;
        else if (!s.empty() && s[0] == '-') throw FormatError("unknown option " + s);
        else a.inputs.push_back(s);
    }
    return true;
}

LinkedImage load_all(const Args& a) {
    LoadOptions lo;
    lo.verify_hashes = !a.no_hash;
    std::vector<Module> mods;
    for (const auto& p : a.inputs) mods.push_back(load_module_file(p, lo));
    LinkOptions l;
    l.first_base = a.base;
    l.strict = a.strict;
    return link(std::move(mods), l);
}

void write_out(const std::string& path, const std::string& text) {
    if (path.empty() || path == "-") { std::cout << text << '\n'; return; }
    std::ofstream f(path, std::ios::binary);
    if (!f) throw FormatError("cannot write " + path);
    f << text << '\n';
}

const char* seg_name(SegmentKind k) {
    switch (k) {
        case SegmentKind::Text: return "text";
        case SegmentKind::Ro: return "ro";
        case SegmentKind::Data: return "data";
        case SegmentKind::Bss: return "bss";
    }
    return "?";
}

void info_json(const LinkedImage& img, JsonWriter& j) {
    j.begin_object();
    j.key("modules").begin_array();
    for (uint32_t mi = 0; mi < img.modules.size(); ++mi) {
        const Module& m = img.modules[mi];
        j.begin_object();
        j.kv("name", m.name).kv("internal_name", m.internal_name).kv("module_id", m.hex_module_id());
        j.kv("format", std::string(m.is_nro ? "nro" : "nso")).khex("base", m.base).khex("image_size", m.image.size());
        j.khex("entry", m.base + m.entry_offset);
        j.key("segments").begin_array();
        for (const auto& s : m.segments) {
            j.begin_object().kv("kind", seg_name(s.kind)).khex("va", m.base + s.offset).khex("offset", s.offset)
                .khex("size", s.size).kv("compressed", s.compressed).kv("hash_checked", s.hash_checked)
                .kv("hash_ok", s.hash_ok).end_object();
        }
        j.end_array();
        j.key("api_info").begin_array();
        for (const auto& s : m.api_info) j.value(s);
        j.end_array();
        j.key("needed").begin_array();
        for (const auto& s : m.needed) j.value(s);
        j.end_array();
        uint64_t defined = 0, undefined = 0, funcs = 0;
        for (const auto& s : m.dynsym) {
            if (s.name.empty()) continue;
            if (s.defined()) { ++defined; if (s.type == kSttFunc) ++funcs; }
            else ++undefined;
        }
        j.key("dynsym").begin_object().kv("total", static_cast<uint64_t>(m.dynsym.size())).kv("defined", defined)
            .kv("defined_functions", funcs).kv("undefined", undefined).end_object();
        std::map<std::string, uint64_t> rc;
        uint64_t relr = 0;
        for (const auto& r : m.relocations) {
            if (r.from_relr) ++relr;
            else rc[std::string(relocation_type_name(r.type)) + (r.type == 0 ? "" : "")]++;
        }
        j.key("relocations").begin_object();
        for (const auto& [k, v] : rc) j.kv(k, v);
        j.kv("RELR_RELATIVE", relr).kv("total", static_cast<uint64_t>(m.relocations.size()));
        j.end_object();
        j.khex("mod0", m.mod0_offset).khex("dynamic", m.dynamic_offset);
        j.kv("eh_frame_hdr_size", m.eh_frame_hdr_size).kv("fde_count", static_cast<uint64_t>(m.fde_starts.size()))
            .kv("fde_ranges", static_cast<uint64_t>(m.fde_ranges.size()));
        j.kv("init_array", static_cast<uint64_t>(m.init_array.size())).kv("fini_array", static_cast<uint64_t>(m.fini_array.size()));
        j.key("exports").begin_array();
        for (const auto& s : m.dynsym)
            if (s.defined() && !s.name.empty() && s.bind != kStbLocal)
                j.begin_object().kv("name", s.name).khex("va", m.base + s.value).kv("type", static_cast<uint64_t>(s.type)).end_object();
        j.end_array();
        j.end_object();
    }
    j.end_array();
    j.key("imports").begin_array();
    for (const auto& i : img.imports) {
        j.begin_object().kv("name", i.name).kv("from", img.modules[i.from_module].name)
            .kv("to", i.to_module >= 0 ? img.modules[static_cast<size_t>(i.to_module)].name : std::string(""))
            .khex("slot", i.slot_va).khex("target", i.target_va).kv("weak", i.weak).end_object();
    }
    j.end_array();
    j.key("stats").begin_object().kv("relocations_applied", img.stats.relocations_applied)
        .kv("relocations_unsupported", img.stats.relocations_unsupported).kv("imports_resolved", img.stats.imports_resolved)
        .kv("imports_unresolved_weak", img.stats.imports_unresolved_weak)
        .kv("imports_unresolved_strong", img.stats.imports_unresolved_strong).end_object();
    j.key("diagnostics").begin_array();
    for (const auto& d : img.diagnostics) j.begin_object().kv("module", d.module).kv("text", d.text).end_object();
    j.end_array();
    j.end_object();
}

void print_info_text(const LinkedImage& img) {
    for (const auto& m : img.modules) {
        std::printf("%s (%s) %s base=0x%llx image=0x%llx entry=0x%llx\n", m.name.c_str(), m.is_nro ? "NRO" : "NSO",
                    m.internal_name.c_str(), (unsigned long long)m.base, (unsigned long long)m.image.size(),
                    (unsigned long long)(m.base + m.entry_offset));
        for (const auto& s : m.segments)
            std::printf("  .%-4s va=0x%llx size=0x%llx%s%s\n", seg_name(s.kind), (unsigned long long)(m.base + s.offset),
                        (unsigned long long)s.size, s.compressed ? " lz4" : "", s.hash_checked ? (s.hash_ok ? " hash-ok" : " HASH-MISMATCH") : "");
        std::map<std::string, uint64_t> rc;
        for (const auto& r : m.relocations) rc[r.from_relr ? "RELR" : relocation_type_name(r.type)]++;
        std::printf("  relocations:");
        for (const auto& [k, v] : rc) std::printf(" %s=%llu", k.c_str(), (unsigned long long)v);
        std::printf("\n  dynsym=%zu fde=%zu (ranges %zu) init_array=%zu needed=%zu\n", m.dynsym.size(), m.fde_starts.size(),
                    m.fde_ranges.size(), m.init_array.size(), m.needed.size());
    }
    std::printf("imports: %llu resolved, %llu weak unresolved, %llu STRONG unresolved; relocations applied %llu, unsupported %llu\n",
                (unsigned long long)img.stats.imports_resolved, (unsigned long long)img.stats.imports_unresolved_weak,
                (unsigned long long)img.stats.imports_unresolved_strong, (unsigned long long)img.stats.relocations_applied,
                (unsigned long long)img.stats.relocations_unsupported);
    for (const auto& d : img.diagnostics) std::printf("  ! %s: %s\n", d.module.c_str(), d.text.c_str());
}

void functions_json(const LinkedImage& img, const AnalysisResult& res, JsonWriter& j) {
    j.begin_object();
    j.key("modules").begin_array();
    for (const auto& m : img.modules) j.begin_object().kv("name", m.name).khex("base", m.base).end_object();
    j.end_array();
    j.key("functions").begin_array();
    for (const auto& f : res.functions) {
        j.begin_object().khex("entry", f.entry).khex("end", f.end).kv("name", f.name)
            .kv("source", function_source_name(f.source)).kv("noreturn", f.noreturn)
            .kv("has_fde_range", f.has_fde_range).kv("unresolved_indirect", f.has_unresolved_indirect);
        j.key("blocks").begin_array();
        for (const auto& b : f.blocks) {
            j.begin_object().khex("start", b.start).khex("end", b.end).kv("term", terminator_name(b.term));
            j.key("succ").begin_array();
            for (uint64_t s : b.successors) j.hex(s);
            j.end_array();
            if (!b.calls.empty()) {
                j.key("calls").begin_array();
                for (const auto& c : b.calls) {
                    j.begin_object().khex("pc", c.pc).khex("target", c.target).kv("indirect", c.indirect).kv("tail", c.tail);
                    if (c.via_slot) j.khex("via_slot", c.via_slot);
                    j.end_object();
                }
                j.end_array();
            }
            if (b.jump_table) j.kv("jump_table", static_cast<uint64_t>(*b.jump_table));
            j.end_object();
        }
        j.end_array();
        if (!f.jump_tables.empty()) {
            j.key("jump_tables").begin_array();
            for (const auto& t : f.jump_tables) {
                j.begin_object().khex("br", t.br_pc).khex("table", t.table_va).kv("entry_size", static_cast<uint64_t>(t.entry_size))
                    .kv("signed", t.entries_signed).khex("base", t.base_va).kv("shift", static_cast<uint64_t>(t.shift))
                    .kv("count", static_cast<uint64_t>(t.count)).kv("bound_known", t.bound_known)
                    .kv("confidence", confidence_name(t.confidence));
                j.key("targets").begin_array();
                for (uint64_t tg : t.targets) j.hex(tg);
                j.end_array().end_object();
            }
            j.end_array();
        }
        j.end_object();
    }
    j.end_array();
    j.key("diagnostics").begin_array();
    for (const auto& d : res.diagnostics)
        j.begin_object().kv("kind", diagnostic_kind_name(d.kind)).khex("pc", d.pc).kv("text", d.text).end_object();
    j.end_array();
    const AnalysisStats& s = res.stats;
    j.key("stats").begin_object().kv("functions", s.functions).kv("blocks", s.blocks).kv("instructions", s.instructions);
    j.key("by_source").begin_object();
    for (int i = 0; i < 9; ++i) j.kv(function_source_name(static_cast<FunctionSource>(i)), s.by_source[i]);
    j.end_object();
    j.kv("jump_tables", s.jump_tables).kv("jump_tables_bounded", s.jump_tables_bounded)
        .kv("indirect_jumps_unresolved", s.indirect_jumps_unresolved).kv("indirect_calls", s.indirect_calls)
        .kv("direct_calls", s.direct_calls).kv("tail_calls", s.tail_calls).kv("noreturn_functions", s.noreturn_functions)
        .kv("text_bytes", s.text_bytes).kv("covered_bytes", s.covered_bytes).kv("padding_bytes", s.padding_bytes)
        .kv("diagnostics", static_cast<uint64_t>(res.diagnostics.size()));
    j.end_object();
    j.end_object();
}

void print_summary(const LinkedImage& img, const AnalysisResult& res) {
    const AnalysisStats& s = res.stats;
    std::printf("== lgp_recomp functions: summary (no code bytes) ==\n");
    for (const auto& m : img.modules)
        std::printf("module %-10s base=0x%llx text=0x%llx fde=%zu\n", m.name.c_str(), (unsigned long long)m.base,
                    (unsigned long long)m.segment(SegmentKind::Text).size, m.fde_starts.size());
    std::printf("functions           %llu\n", (unsigned long long)s.functions);
    for (int i = 0; i < 9; ++i)
        if (s.by_source[i]) std::printf("  %-17s %llu\n", function_source_name(static_cast<FunctionSource>(i)), (unsigned long long)s.by_source[i]);
    std::printf("basic blocks        %llu\ninstructions        %llu\n", (unsigned long long)s.blocks, (unsigned long long)s.instructions);
    std::printf("text bytes          %llu (covered %llu = %.1f%%, padding %llu)\n", (unsigned long long)s.text_bytes,
                (unsigned long long)s.covered_bytes, s.text_bytes ? 100.0 * (double)s.covered_bytes / (double)s.text_bytes : 0.0,
                (unsigned long long)s.padding_bytes);
    std::printf("direct calls        %llu\ntail calls          %llu\nindirect calls      %llu\n", (unsigned long long)s.direct_calls,
                (unsigned long long)s.tail_calls, (unsigned long long)s.indirect_calls);
    std::printf("jump tables         %llu (bounded %llu)\nunresolved BR       %llu\nnoreturn functions  %llu\n",
                (unsigned long long)s.jump_tables, (unsigned long long)s.jump_tables_bounded,
                (unsigned long long)s.indirect_jumps_unresolved, (unsigned long long)s.noreturn_functions);
    std::map<std::string, uint64_t> dk;
    for (const auto& d : res.diagnostics) dk[diagnostic_kind_name(d.kind)]++;
    std::printf("diagnostics         %zu\n", res.diagnostics.size());
    for (const auto& [k, v] : dk) std::printf("  %-28s %llu\n", k.c_str(), (unsigned long long)v);
}

int cmd_info(const Args& a) {
    LinkedImage img = load_all(a);
    if (!a.json_out.empty()) {
        JsonWriter j;
        info_json(img, j);
        write_out(a.json_out, j.str());
    }
    if (a.json_out.empty() || a.json_out != "-") print_info_text(img);
    return (a.strict && (img.stats.imports_unresolved_strong || img.stats.relocations_unsupported)) ? 2 : 0;
}

// Problems with the config file are shown even without --diagnostics.
void warn_config(const AnalysisResult& res) {
    for (const auto& d : res.diagnostics)
        if (is_config_diagnostic(d.kind))
            std::fprintf(stderr, "lgp_recomp: warning: %s 0x%llx %s\n", diagnostic_kind_name(d.kind),
                         (unsigned long long)d.pc, d.text.c_str());
}

int cmd_functions(const Args& a) {
    LinkedImage img = load_all(a);
    AnalysisConfig cfg;
    if (!a.config.empty()) cfg = load_config_file(a.config);
    AnalysisResult res = analyze(img, cfg);
    warn_config(res);
    if (!a.json_out.empty()) {
        JsonWriter j;
        functions_json(img, res, j);
        write_out(a.json_out, j.str());
    }
    if (a.summary || a.json_out.empty()) print_summary(img, res);
    if (a.diagnostics)
        for (const auto& d : res.diagnostics)
            std::printf("  %-28s 0x%llx %s\n", diagnostic_kind_name(d.kind), (unsigned long long)d.pc, d.text.c_str());
    return 0;
}

int cmd_check_functions(const Args& a) {
    if (a.inputs.size() != 1 || a.elf.empty()) throw FormatError("check-functions needs exactly one module and --elf");
    LinkedImage img = load_all(a);
    AnalysisConfig cfg;
    if (!a.config.empty()) cfg = load_config_file(a.config);
    AnalysisResult res = analyze(img, cfg);
    warn_config(res);
    ElfSymbols elf = read_elf_symbols_file(a.elf);
    const Module& m = img.modules[0];
    const Segment& text = m.segment(SegmentKind::Text);
    // Expected: STT_FUNC symbols with a size, inside .text (ELF link address 0 == module offset).
    std::map<uint64_t, std::pair<std::string, uint64_t>> expected;  // va -> (name, size)
    for (const auto& s : elf.symbols) {
        if (s.type != kSttFunc || s.name.empty() || s.shndx == 0) continue;
        if (s.value < text.offset || s.value >= text.offset + text.size) continue;
        uint64_t va = m.base + s.value;
        auto it = expected.find(va);
        if (it == expected.end() || it->second.second < s.size) expected[va] = {s.name, s.size};
    }
    std::set<uint64_t> found(res.function_entries.begin(), res.function_entries.end());
    std::vector<std::pair<uint64_t, std::string>> missing, wrong;
    for (const auto& [va, ns] : expected)
        if (!found.count(va)) missing.emplace_back(va, ns.first);
    std::vector<uint64_t> extra;
    for (uint64_t va : found) {
        if (expected.count(va)) continue;
        auto it = expected.upper_bound(va);
        bool inside = false;
        if (it != expected.begin()) {
            --it;
            inside = va > it->first && va < it->first + it->second.second;
            if (inside) wrong.emplace_back(va, it->second.first);
        }
        if (!inside) extra.push_back(va);
    }
    std::printf("check-functions %s vs %s (%s)\n", m.name.c_str(), a.elf.c_str(), elf.from_symtab ? ".symtab" : ".dynsym only");
    std::printf("  expected %zu function symbols, found %zu function entries\n", expected.size(), found.size());
    std::printf("  missing (symbol without entry): %zu\n", missing.size());
    std::printf("  wrong (entry inside a symbol's body): %zu\n", wrong.size());
    std::printf("  unnamed extra entries (no symbol, outside every symbol body): %zu\n", extra.size());
    if (a.verbose || !missing.empty())
        for (const auto& [va, n] : missing) {
            std::printf("    missing 0x%llx %s\n", (unsigned long long)va, n.c_str());
            if (!a.verbose) continue;
            // Context for debugging discovery: who covers the address, and what the loader knew.
            bool fde_start = std::binary_search(m.fde_starts.begin(), m.fde_starts.end(), va - m.base);
            const Function* owner = res.function_containing(va);
            CfInfo cf = classify(img.read_u32(va), va);
            std::printf("            fde_start=%s classify=%s", fde_start ? "yes" : "no", cf_kind_name(cf.kind));
            if (owner)
                std::printf(" inside %s 0x%llx..0x%llx (%s%s)", owner->name.c_str(), (unsigned long long)owner->entry,
                            (unsigned long long)owner->end, function_source_name(owner->source),
                            owner->has_fde_range ? ", fde" : "");
            else
                std::printf(" not covered by any function");
            std::printf("\n");
            for (const auto& fr : m.fde_ranges)
                if (va - m.base > fr.first && va - m.base < fr.second)
                    std::printf("            inside FDE range 0x%llx..0x%llx\n", (unsigned long long)(fr.first + m.base),
                                (unsigned long long)(fr.second + m.base));
        }
    if (a.verbose || !wrong.empty())
        for (const auto& [va, n] : wrong) std::printf("    wrong   0x%llx inside %s\n", (unsigned long long)va, n.c_str());
    if (a.verbose)
        for (uint64_t va : extra) {
            const Function* f = res.function_at(va);
            std::printf("    extra   0x%llx (%s)\n", (unsigned long long)va, f ? function_source_name(f->source) : "?");
        }
    if (a.verbose)
        for (const auto& d : res.diagnostics)
            std::printf("    diag    %-28s 0x%llx %s\n", diagnostic_kind_name(d.kind), (unsigned long long)d.pc, d.text.c_str());
    return (missing.empty() && wrong.empty()) ? 0 : 1;
}

// Whole-program lift: analysis (P2.1/P2.2) → adapter (p2-interfaces.md 4a) → lifter.
int cmd_lift(const Args& a) {
    if (a.inputs.empty() || a.out.empty()) throw FormatError("lift needs at least one module and --out");
    LinkedImage img = load_all(a);
    AnalysisConfig cfg;
    if (!a.config.empty()) cfg = load_config_file(a.config);
    AnalysisResult res = analyze(img, cfg);
    warn_config(res);
    LiftOptions opt;
    opt.block_trace = a.trace;
    const Lifter lifter = make_lifter(img, res, opt);
    LiftStats stats;
    std::ofstream f(a.out, std::ios::binary);
    if (!f) throw FormatError("cannot write " + a.out);
    f << "// Generated by lgp_recomp lift. Translated guest code: keep output from game data in\n"
         "// $LGP_PRIVATE_DIR, never commit it.\n"
         "#include \"lgp/lifted/lifted.hpp\"\n\n";
    f << lifter.declarations() << '\n';
    for (const Function& fn : res.functions) f << lifter.lift(to_lift_function(fn), stats) << '\n';
    if (!f) throw FormatError("cannot write " + a.out);

    std::fprintf(stderr, "lifted %zu functions, %llu instructions, %llu unsupported (%.3f %%)\n",
                 res.functions.size(), (unsigned long long)stats.instructions,
                 (unsigned long long)stats.unsupported,
                 stats.instructions ? 100.0 * double(stats.unsupported) / double(stats.instructions) : 0.0);
    if (stats.unsupported) {
        // Unsupported words are counted under the Invalid op; break them down by reason.
        std::map<std::string, uint64_t> reasons;
        for (uint64_t pc : stats.unsupported_pcs) {
            static const char* names[] = {"none", "unallocated", "unpredictable", "privileged", "not_implemented"};
            const a64::Inst i = a64::decode(img.read_u32(pc), pc);
            ++reasons[names[static_cast<int>(i.invalid)]];
        }
        for (const auto& [r, n] : reasons) std::fprintf(stderr, "  unsupported %-16s %llu\n", r.c_str(), (unsigned long long)n);
    }
    return 0;
}

int cmd_classify(const Args& a) {
    if (a.inputs.empty()) throw FormatError("classify needs a hex instruction word");
    uint32_t insn = static_cast<uint32_t>(std::stoul(a.inputs[0], nullptr, 16));
    uint64_t pc = a.inputs.size() > 1 ? std::stoull(a.inputs[1], nullptr, 0) : 0;
    CfInfo cf = classify(insn, pc);
    std::printf("%08x @0x%llx: %s target=0x%llx reg=%u imm=%u\n", insn, (unsigned long long)pc,
                cf_kind_name(cf.kind), (unsigned long long)cf.target, cf.reg, cf.imm);
    return 0;
}

}  // namespace

void census_json(const std::vector<NativeCensus>& all, JsonWriter& j) {
    j.begin_object();
    j.key("modules").begin_array();
    for (const auto& c : all) {
        j.begin_object().kv("name", c.module).kv("functions", c.functions).kv("instructions", c.instructions)
            .kv("undecodable", c.undecodable).kv("patch_sites", c.patch_sites());
        j.key("x18").begin_object().kv("sites", c.x18_sites).kv("reads", c.x18_reads).kv("writes", c.x18_writes)
            .kv("br", c.x18_br).kv("br_jump_table", c.x18_br_jump_table).kv("blr", c.x18_blr).kv("mem", c.x18_mem)
            .kv("sp", c.x18_sp).kv("simd", c.x18_simd).kv("in_exclusive", c.x18_in_exclusive)
            .kv("functions", c.functions_with_x18).kv("live_at_entry", c.x18_live_at_entry)
            .kv("live_across_call", c.x18_live_across_call).kv("unresolved", c.x18_unresolved)
            .kv("no_free_register", c.x18_no_free_register).kv("renamable", c.x18_renamable)
            .kv("renamable_sites", c.x18_renamable_sites).kv("webs", c.x18_webs)
            .kv("webs_renamable", c.x18_webs_renamable).kv("webs_open", c.x18_webs_open)
            .kv("web_sites_renamable", c.x18_web_sites_renamable).kv("web_sites_left", c.x18_web_sites_left)
            .kv("functions_all_renamable", c.x18_functions_all_renamable)
            .kv("webs_renamable_caller_saved", c.x18_webs_renamable_caller_saved)
            .kv("web_sites_renamable_caller_saved", c.x18_web_sites_renamable_caller_saved).end_object();
        j.kv("svc", c.svc);
        j.key("svc_by_number").begin_object();
        for (const auto& [n, v] : c.svc_by_number) {
            char k[16];
            std::snprintf(k, sizeof(k), "0x%02x", n);
            j.kv(k, v);
        }
        j.end_object();
        j.key("mrs").begin_object();
        for (const auto& [n, v] : c.mrs) j.kv(n, v);
        j.end_object();
        j.key("msr").begin_object();
        for (const auto& [n, v] : c.msr) j.kv(n, v);
        j.end_object();
        j.kv("dc_zva", c.dc_zva).kv("cache_maint", c.cache_maint).kv("yield_wfe", c.yield_wfe)
            .kv("sp_from_register", c.sp_from_register).kv("sp_imm_unaligned", c.sp_imm_unaligned)
            .kv("exclusive_windows", c.exclusive_windows).kv("exclusive_windows_with_site", c.exclusive_windows_with_site)
            .kv("spin_loops", c.spin_loops).kv("spin_loops_with_hint", c.spin_loops_with_hint);
        j.end_object();
    }
    j.end_array();
    j.end_object();
}

void print_census(const std::vector<NativeCensus>& all) {
    std::printf("== lgp_recomp census: native-execution patch sites (counts only) ==\n");
    for (const auto& c : all) {
        auto u = [](uint64_t v) { return static_cast<unsigned long long>(v); };
        std::printf("%s: %llu functions, %llu instructions (%llu undecodable), %llu patch sites\n", c.module.c_str(),
                    u(c.functions), u(c.instructions), u(c.undecodable), u(c.patch_sites()));
        std::printf("  x18: %llu sites (%llu reads, %llu writes) in %llu functions; br %llu (jump table %llu), blr %llu, "
                    "load/store %llu, with sp %llu, simd %llu, in exclusive windows %llu\n",
                    u(c.x18_sites), u(c.x18_reads), u(c.x18_writes), u(c.functions_with_x18), u(c.x18_br),
                    u(c.x18_br_jump_table), u(c.x18_blr), u(c.x18_mem), u(c.x18_sp), u(c.x18_simd), u(c.x18_in_exclusive));
        std::printf("  x18 per function: live at entry %llu, live across a call %llu, unresolved indirect jump %llu, "
                    "no free x9-x17 %llu; renamable %llu functions with %llu sites\n",
                    u(c.x18_live_at_entry), u(c.x18_live_across_call), u(c.x18_unresolved), u(c.x18_no_free_register),
                    u(c.x18_renamable), u(c.x18_renamable_sites));
        std::printf("  x18 per live range: %llu webs, %llu renamable (%llu sites), %llu open, %llu sites left; "
                    "%llu functions fully renamable\n",
                    u(c.x18_webs), u(c.x18_webs_renamable), u(c.x18_web_sites_renamable), u(c.x18_webs_open),
                    u(c.x18_web_sites_left), u(c.x18_functions_all_renamable));
        std::printf("  x18 per live range, x0-x17 only: %llu webs renamable (%llu sites)\n",
                    u(c.x18_webs_renamable_caller_saved), u(c.x18_web_sites_renamable_caller_saved));
        std::printf("  svc: %llu", u(c.svc));
        for (const auto& [n, v] : c.svc_by_number) std::printf(" 0x%02x=%llu", n, u(v));
        std::printf("\n  mrs:");
        for (const auto& [n, v] : c.mrs) std::printf(" %s=%llu", n.c_str(), u(v));
        std::printf("\n  msr:");
        for (const auto& [n, v] : c.msr) std::printf(" %s=%llu", n.c_str(), u(v));
        std::printf("\n  dc zva %llu, cache maintenance %llu, yield/wfe/wfi %llu\n", u(c.dc_zva), u(c.cache_maint),
                    u(c.yield_wfe));
        std::printf("  sp from register %llu, sp immediate not 16-aligned %llu\n", u(c.sp_from_register),
                    u(c.sp_imm_unaligned));
        std::printf("  exclusive windows %llu (%llu with a patch site), spin-wait candidates %llu (%llu with yield/wfe)\n",
                    u(c.exclusive_windows), u(c.exclusive_windows_with_site), u(c.spin_loops), u(c.spin_loops_with_hint));
    }
}

int cmd_census(const Args& a) {
    LinkedImage img = load_all(a);
    AnalysisConfig cfg;
    if (!a.config.empty()) cfg = load_config_file(a.config);
    AnalysisResult res = analyze(img, cfg);
    warn_config(res);
    const std::vector<NativeCensus> all = native_census(img, res);
    if (!a.json_out.empty()) {
        JsonWriter j;
        census_json(all, j);
        write_out(a.json_out, j.str());
    }
    if (a.json_out.empty() || a.json_out != "-") print_census(all);
    return 0;
}

int cmd_native_plan(const Args& a) {
    LinkedImage img = load_all(a);
    AnalysisConfig cfg;
    if (!a.config.empty()) cfg = load_config_file(a.config);
    AnalysisResult res = analyze(img, cfg);
    warn_config(res);
    const std::vector<NativeRewrite> all = plan_native_rewrite(img, res);
    using u = unsigned long long;
    std::printf("== lgp_recomp native-plan (counts only) ==\n");
    std::string tsv = "# module\tkind\tva\told\tnew\n";
    char line[128];
    for (const NativeRewrite& r : all) {
        std::printf("%s: x18 in %llu functions, %llu webs: %llu renamed (%llu sites), left for stubs: %llu open, "
                    "%llu without register, %llu shared with another function, %llu not encodable "
                    "(%llu sites); other stub sites %llu; x19-x28 allowed in %llu functions (no LSDA)\n",
                    r.module.c_str(), u(r.functions_with_x18), u(r.webs), u(r.webs_renamed), u(r.sites_renamed),
                    u(r.webs_open), u(r.webs_no_register), u(r.webs_shared), u(r.webs_encoding),
                    u(r.x18_stub_sites.size()), u(r.other_stub_sites.size()), u(r.functions_callee_saved));
        for (const auto& [va, w] : r.words) {
            std::snprintf(line, sizeof line, "\trename\t0x%llx\t%08x\t%08x\n", u(va), img.read_u32(va), w);
            tsv += r.module + line;
        }
        for (uint64_t va : r.x18_stub_sites) {
            std::snprintf(line, sizeof line, "\tstub-x18\t0x%llx\t%08x\t-\n", u(va), img.read_u32(va));
            tsv += r.module + line;
        }
        for (uint64_t va : r.other_stub_sites) {
            std::snprintf(line, sizeof line, "\tstub\t0x%llx\t%08x\t-\n", u(va), img.read_u32(va));
            tsv += r.module + line;
        }
    }
    if (!a.out.empty()) write_out(a.out, tsv);
    return 0;
}

void write_bytes(const std::filesystem::path& path, const std::vector<uint8_t>& bytes) {
    std::ofstream f(path, std::ios::binary);
    if (!f || !f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
        throw FormatError("cannot write " + path.string());
}

std::vector<uint8_t> read_bytes(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw FormatError("cannot read " + path);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), {});
}

int cmd_native_rewrite(const Args& a) {
    if (a.out_dir.empty()) throw FormatError("native-rewrite needs --out-dir");
    LinkedImage img = load_all(a);
    AnalysisConfig cfg;
    if (!a.config.empty()) cfg = load_config_file(a.config);
    AnalysisResult res = analyze(img, cfg);
    warn_config(res);
    const std::vector<NativeRewrite> plan = plan_native_rewrite(img, res);
    const NativePatch patch = build_native_patch(img, plan);
    if (img.modules.size() != a.inputs.size()) throw FormatError("native-rewrite: module count changed while linking");

    using u = unsigned long long;
    std::printf("== lgp_recomp native-rewrite (counts only) ==\n");
    JsonWriter j;
    j.begin_object();
    j.khex("area_base", patch.area_base).khex("gate_entry", patch.gate_entry).khex("data_base", patch.data_base);
    j.khex("host_fn_slot", patch.host_fn_slot).kv("area_size", uint64_t(patch.area.size())).kv("code_size", uint64_t(patch.code_size));
    j.kv("area_file", "native-patch.bin");
    j.key("modules").begin_array();
    const std::filesystem::path dir(a.out_dir);
    std::filesystem::create_directories(dir);
    for (size_t m = 0; m < img.modules.size(); ++m) {
        const Module& mod = img.modules[m];
        const NativeRewrite& r = plan[m];
        uint64_t spills = 0, gates = 0;
        for (const auto& [site, stub] : patch.stubs) {
            (void)stub;
            if (site < mod.base || site - mod.base >= mod.image.size()) continue;
            const bool spill = std::any_of(r.x18_spills.begin(), r.x18_spills.end(),
                                           [&](const X18Spill& s) { return s.va == site; });
            ++(spill ? spills : gates);
        }
        std::printf("%s: x18 webs %llu: renamed %llu, stack slot %llu (%llu sites); not patched: open %llu, shared %llu, "
                    "SP unknown %llu, slot out of range %llu, exclusive window %llu, gate in range %llu, no stub form %llu "
                    "(%llu x18 sites unpatched)\n",
                    r.module.c_str(), u(r.webs), u(r.webs_renamed), u(r.webs_spilled), u(r.sites_spilled), u(r.webs_open),
                    u(r.webs_shared), u(r.webs_sp_unknown), u(r.webs_slot_far), u(r.webs_exclusive), u(r.webs_gate),
                    u(r.webs_form), u(r.x18_unpatched.size()));
        std::printf("%s: gate stubs %llu; not patched: in exclusive window %llu, naming x18 %llu; SP from register %llu "
                    "(counted only); words written %llu\n",
                    r.module.c_str(), u(gates), u(r.gate_exclusive), u(r.gate_x18), u(r.sp_sites),
                    u(patch.modules[m].words.size()));
        const std::string file = std::filesystem::path(a.inputs[m]).filename().string();
        j.begin_object().kv("name", mod.name).kv("file", file).khex("base", mod.base).kv("size", uint64_t(mod.image.size()));
        j.kv("words", uint64_t(patch.modules[m].words.size())).kv("spill_stubs", uint64_t(spills)).kv("gate_stubs", uint64_t(gates));
        j.kv("webs", uint64_t(r.webs)).kv("webs_renamed", uint64_t(r.webs_renamed)).kv("webs_spilled", uint64_t(r.webs_spilled));
        j.kv("webs_open", uint64_t(r.webs_open)).kv("webs_shared", uint64_t(r.webs_shared)).kv("webs_sp_unknown", uint64_t(r.webs_sp_unknown));
        j.kv("webs_slot_far", uint64_t(r.webs_slot_far)).kv("webs_exclusive", uint64_t(r.webs_exclusive)).kv("webs_gate", uint64_t(r.webs_gate));
        j.kv("webs_form", uint64_t(r.webs_form)).kv("x18_sites_unpatched", uint64_t(r.x18_unpatched.size()));
        j.kv("gate_exclusive", uint64_t(r.gate_exclusive)).kv("gate_x18", uint64_t(r.gate_x18)).kv("sp_sites", uint64_t(r.sp_sites));
        if (mod.is_nro) {
            std::printf("%s: NRO, not written (only NSOs are patched)\n", r.module.c_str());
            j.kv("written", false);
        } else {
            write_bytes(dir / file, patch_nso(read_bytes(a.inputs[m]), mod, patch.modules[m]));
            j.kv("written", true);
        }
        j.end_object();
    }
    j.end_array();
    j.kv("spill_stubs", uint64_t(patch.spill_stubs)).kv("gate_stubs", uint64_t(patch.gate_stubs)).kv("sites_renamed", uint64_t(patch.sites_renamed));
    j.end_object();
    write_bytes(dir / "native-patch.bin", patch.area);
    write_out((dir / "manifest.json").string(), j.str());
    std::printf("patch area: %llu KiB code, %llu stubs (%llu stack slot, %llu gate), behind the last module\n",
                u(patch.code_size / 1024), u(patch.stubs.size()), u(patch.spill_stubs), u(patch.gate_stubs));
    return 0;
}

int main(int argc, char** argv) {
    Args a;
    try {
        if (!parse_args(argc, argv, a)) { usage(); return 64; }
        if (a.command == "info") return cmd_info(a);
        if (a.command == "functions") return cmd_functions(a);
        if (a.command == "check-functions") return cmd_check_functions(a);
        if (a.command == "classify") return cmd_classify(a);
        if (a.command == "lift") return cmd_lift(a);
        if (a.command == "census") return cmd_census(a);
        if (a.command == "native-plan") return cmd_native_plan(a);
        if (a.command == "native-rewrite") return cmd_native_rewrite(a);
        usage();
        return 64;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "lgp_recomp: %s\n", e.what());
        return 3;
    }
}
