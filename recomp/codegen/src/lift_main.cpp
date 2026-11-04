// lgp_lift: recompiles linked NSO/NRO modules to one C++ file (roadmap P2.5).
//
//   lgp_lift <module...> -o out.cpp [--config game.cfg] [--function ADDR|SYMBOL[+OFF]]...
//            [--elf module.elf] [--base 0xADDR] [--entries-name NAME] [--prefix PREFIX]
//            [--split N] [--precise-fault-pc] [--no-register-locals] [--exact-flags]
//            [--exact-registers] [--exact-calls] [--block-trace] [--summary]
//            [--hook ADDR|SYMBOL[+OFF]]... [--mods FILE|DIR] [--patch-site ADDR|SYMBOL[+OFF]]...
//            [--no-sdk-hooks] [--bad-mods-out FILE]
//
// --split N writes N translation units: out.cpp (with the dispatch and module
// tables) and out.1.cpp … out.<N-1>.cpp. A game module needs this to compile.
// --function adds a function entry like `function` in the config. A symbol is
// looked up in the modules' dynsym, or with --elf in that ELF's .symtab (own
// test programs only; the ELF belongs to the first module).
// --patch-site makes one instruction a patch site of a game mod
// (LiftOptions::patch_sites, lgp/runtime/patch.hpp): it must fall through to
// the next instruction without branching, otherwise lgp_lift fails (exit 1).
// The output is derived from the input code: for the game it goes to build/ or
// $LGP_PRIVATE_DIR, never into the repository. --summary prints counters only.
// --hook gives the function at that entry a hook door (lgp/runtime/hooks.hpp);
// --mods does so for every `hook` line of a mod file or of the *.cfg files in
// a directory (lgp/mods/mod_file.hpp, game mods: <game>/mods). A hook address
// becomes a function entry like --function. The SDK functions of
// lgp/recomp/sdk_hooks.hpp (profanity filter) get a door as well when a module
// exports them, so the boot program can stand in for firmware data;
// --no-sdk-hooks leaves them out.
// --bad-mods-out FILE: when a patch site of a mod file is rejected, lgp_lift
// still fails (exit 1) but writes the file names of those mod files to FILE,
// one per line, so a caller (tools/kit/build.py) can leave out just those
// mods and lift again.
//
// NZCV follows AAPCS64 by default: undefined at function entry and after calls,
// so it is not written back or reloaded there (LiftOptions::flags_across_calls).
// --exact-flags keeps the flags across calls and returns for code that does not
// follow the procedure call standard, and for tests that compare the flags
// after a return. Likewise RET does not write back the corruptible registers
// (LiftOptions::registers_across_calls); --exact-registers writes them all,
// --exact-calls means both options.
//
// Exit code 3: the code contains valid ARMv8.0 instructions the lifter does not
// implement. Words that are UNDEFINED on the A57 anyway (data or padding inside
// a function's range, e.g. the "HOMEBREW" magic in libnx's crt0, or the zero
// word after a noreturn call) are lifted as the same trap and do not count.
#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "lgp/recomp/a64.hpp"
#include "lgp/recomp/a64_simd.hpp"
#include "lgp/recomp/codegen.hpp"
#include "lgp/recomp/elf_symbols.hpp"
#include "lgp/recomp/loader.hpp"
#include "lgp/recomp/sdk_hooks.hpp"
#include "lgp/mods/mod_file.hpp"

using namespace lgp::recomp;

namespace {

struct Args {
    std::vector<std::string> inputs;
    std::string out, config, elf, entries_name = "lgp_lifted_entries", prefix = "lgp_fn_";
    std::vector<std::string> functions, hooks, patch_sites;
    std::string mods, bad_mods_out;
    uint64_t base = 0x0800'0000;
    size_t split = 1;
    bool precise_fault_pc = false, block_trace = false, summary = false, register_locals = true;
    bool exact_flags = false;
    bool exact_registers = false;
    bool sdk_hooks = true;
};

[[noreturn]] void usage() {
    std::fprintf(stderr,
                 "usage: lgp_lift <module...> -o out.cpp [--config game.cfg] [--function ADDR|SYMBOL[+OFF]]...\n"
                 "                [--elf module.elf] [--base 0xADDR] [--entries-name NAME] [--prefix PREFIX]\n"
                 "                [--split N] [--precise-fault-pc] [--no-register-locals] [--exact-flags]\n"
                 "                [--exact-registers] [--exact-calls] [--block-trace]\n"
                 "                [--summary] [--hook ADDR|SYMBOL[+OFF]]... [--mods FILE|DIR] [--no-sdk-hooks]\n"
                 "                [--patch-site ADDR|SYMBOL[+OFF]]... [--bad-mods-out FILE]\n");
    std::exit(2);
}

Args parse_args(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        const std::string s = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) usage();
            return argv[++i];
        };
        if (s == "-o") a.out = next();
        else if (s == "--config") a.config = next();
        else if (s == "--function") a.functions.push_back(next());
        else if (s == "--hook") a.hooks.push_back(next());
        else if (s == "--mods") a.mods = next();
        else if (s == "--bad-mods-out") a.bad_mods_out = next();
        else if (s == "--elf") a.elf = next();
        else if (s == "--base") a.base = std::stoull(next(), nullptr, 0);
        else if (s == "--entries-name") a.entries_name = next();
        else if (s == "--prefix") a.prefix = next();
        else if (s == "--split") {
            a.split = std::stoull(next());
            if (a.split == 0 || a.split > 4096) usage();
        }
        else if (s == "--precise-fault-pc") a.precise_fault_pc = true;
        else if (s == "--no-register-locals") a.register_locals = false;
        else if (s == "--exact-flags") a.exact_flags = true;
        else if (s == "--exact-registers") a.exact_registers = true;
        else if (s == "--exact-calls") a.exact_flags = a.exact_registers = true;
        else if (s == "--block-trace") a.block_trace = true;
        else if (s == "--patch-site") a.patch_sites.push_back(next());
        else if (s == "--summary") a.summary = true;
        else if (s == "--no-sdk-hooks") a.sdk_hooks = false;
        else if (!s.empty() && s[0] == '-') usage();
        else a.inputs.push_back(s);
    }
    if (a.inputs.empty() || a.out.empty()) usage();
    return a;
}

uint64_t resolve_address(const char* option, const std::string& spec, const LinkedImage& image,
                         const std::optional<ElfSymbols>& elf) {
    if (!spec.empty() && spec[0] >= '0' && spec[0] <= '9') return std::stoull(spec, nullptr, 0);
    std::string name = spec;
    uint64_t off = 0;
    if (auto plus = spec.find('+'); plus != std::string::npos) {
        name = spec.substr(0, plus);
        off = std::stoull(spec.substr(plus + 1), nullptr, 0);
    }
    if (elf) {
        for (const Symbol& s : elf->symbols)
            if (s.name == name && s.defined()) return image.modules.front().base + s.value + off;
    } else if (std::optional<uint64_t> va = image.symbol_va(name)) {
        return *va + off;
    }
    throw FormatError(std::string(option) + ": unknown symbol " + name);
}

}  // namespace

int main(int argc, char** argv) {
    const Args a = parse_args(argc, argv);
    try {
        std::vector<Module> mods;
        for (const std::string& p : a.inputs) mods.push_back(load_module_file(p));
        LinkOptions lo;
        lo.first_base = a.base;
        const LinkedImage image = link(std::move(mods), lo);

        AnalysisConfig cfg = a.config.empty() ? AnalysisConfig{} : load_config_file(a.config);
        std::optional<ElfSymbols> elf;
        if (!a.elf.empty()) elf = read_elf_symbols_file(a.elf);
        for (const std::string& f : a.functions) cfg.extra_functions.push_back(resolve_address("--function", f, image, elf));
        std::set<uint64_t> hooks, mod_patch_sites;
        std::map<uint64_t, std::set<std::string>> patch_owners;  // patch site -> mod file names
        for (const std::string& h : a.hooks) hooks.insert(resolve_address("--hook", h, image, elf));
        if (a.sdk_hooks)
            for (const SdkHookSymbol& s : kSdkHookSymbols)
                if (std::optional<uint64_t> va = image.symbol_va(std::string(s.name))) hooks.insert(*va);
        if (!a.mods.empty()) {
            std::vector<lgp::mods::ModFile> files;
            std::string error;
            if (!lgp::mods::load_mod_files(a.mods, &files, &error)) throw FormatError("--mods: " + error);
            if (files.empty()) throw FormatError("--mods: no mod files in " + a.mods);
            for (const lgp::mods::ModFile& f : files)
                for (uint64_t va : f.hook_entries()) hooks.insert(va);
            for (const lgp::mods::ModFile& f : files)
                for (uint64_t va : f.patch_entries()) {
                    mod_patch_sites.insert(va);
                    patch_owners[va].insert(f.path.filename().string());
                }
        }
        for (uint64_t va : hooks) cfg.extra_functions.push_back(va);

        const AnalysisResult analysis = analyze(image, cfg);
        for (const auto& d : analysis.diagnostics)
            if (is_config_diagnostic(d.kind))
                std::fprintf(stderr, "lgp_lift: warning: %s 0x%llx %s\n", diagnostic_kind_name(d.kind),
                             (unsigned long long)d.pc, d.text.c_str());
        CodegenOptions co;
        co.entries_name = a.entries_name;
        co.lift.name_prefix = a.prefix;
        co.lift.precise_fault_pc = a.precise_fault_pc;
        co.lift.block_trace = a.block_trace;
        co.lift.register_locals = a.register_locals;
        co.lift.flags_across_calls = a.exact_flags;
        co.lift.registers_across_calls = a.exact_registers;
        for (const std::string& p : a.patch_sites) co.lift.patch_sites.insert(resolve_address("--patch-site", p, image, elf));
        co.lift.patch_sites.insert(mod_patch_sites.begin(), mod_patch_sites.end());
        co.parts = a.split;
        co.hook_entries = hooks;
        const ModuleCode mc = generate_module(image, analysis, co);
        std::set<std::string> bad_mods;
        auto owners = [&](uint64_t pc) {
            std::string s;
            if (auto it = patch_owners.find(pc); it != patch_owners.end())
                for (const std::string& name : it->second) {
                    s += (s.empty() ? " (mod file " : ", ") + name;
                    bad_mods.insert(name);
                }
            return s.empty() ? s : s + ")";
        };
        bool bad_site = false;
        for (uint64_t pc : mc.stats.lift.rejected_patch_sites) {
            std::fprintf(stderr, "lgp_lift: patch site 0x%llx branches or traps (only plain instructions)%s\n",
                         (unsigned long long)pc, owners(pc).c_str());
            bad_site = true;
        }
        for (uint64_t pc : co.lift.patch_sites)
            if (!mc.stats.lift.patch_sites.count(pc) &&
                std::find(mc.stats.lift.rejected_patch_sites.begin(), mc.stats.lift.rejected_patch_sites.end(), pc) ==
                    mc.stats.lift.rejected_patch_sites.end()) {
                std::fprintf(stderr, "lgp_lift: patch site 0x%llx is not inside a lifted function%s\n",
                             (unsigned long long)pc, owners(pc).c_str());
                bad_site = true;
            }
        if (bad_site) {
            if (!a.bad_mods_out.empty()) {
                std::ofstream f(a.bad_mods_out, std::ios::binary);
                for (const std::string& name : bad_mods) f << name << '\n';
            }
            return 1;
        }

        for (size_t i = 0; i < mc.sources.size(); ++i) {
            std::string path = a.out;
            if (i != 0) {
                const size_t dot = path.rfind('.');
                const size_t slash = path.find_last_of("/\\");
                const std::string suffix = "." + std::to_string(i);
                if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
                    path += suffix;
                else
                    path.insert(dot, suffix);
            }
            std::ofstream f(path, std::ios::binary);
            f << mc.sources[i];
            if (!f.good()) throw FormatError("cannot write " + path);
        }
        uint64_t undefined = 0;  // unallocated or privileged: the A57 traps too
        for (uint64_t pc : mc.stats.lift.unsupported_pcs) {
            const a64::Inst i = a64::decode(image.read_u32(pc), pc);
            a64::Invalid why = i.invalid;
            if (i.op == a64::Op::Simd) why = a64::simd::lift(i.raw, pc, false).invalid;
            if (why == a64::Invalid::Unallocated || why == a64::Invalid::Privileged) ++undefined;
        }
        const uint64_t not_implemented = mc.stats.lift.unsupported - undefined;
        if (a.summary) {
            const CodegenStats& s = mc.stats;
            std::printf("functions        %" PRIu64 "\n", s.functions);
            std::printf("replaced         %" PRIu64 " (setjmp %zu, longjmp %zu)\n", s.replaced,
                        mc.setjmp_entries.size(), mc.longjmp_entries.size());
            std::printf("instructions     %" PRIu64 "\n", s.lift.instructions);
            std::printf("unsupported      %" PRIu64 " (undefined on the A57 %" PRIu64 ", not implemented %" PRIu64 ")\n",
                        s.lift.unsupported, undefined, not_implemented);
            std::printf("jump tables      %" PRIu64 "\n", s.jump_tables);
            std::printf("indirect BR      %" PRIu64 " (dispatch table)\n", s.indirect_jumps);
            if (!s.lift.patch_sites.empty()) std::printf("patch sites      %zu\n", s.lift.patch_sites.size());
            std::printf("hook doors       %" PRIu64 "\n", s.hooked);
        }
        return not_implemented == 0 ? 0 : 3;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "lgp_lift: %s\n", e.what());
        return 1;
    }
}
