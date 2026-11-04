// lgp_fn_select: picks self-contained functions for the function-level
// differential test (ADR-0006 stage 3) and lifts only them.
//
//   lgp_fn_select <module...> --out DIR [--module NAME] [--max-closure N] [--max-insns N]
//                 [--limit N] [--split N]
//
// A function qualifies when it and everything it calls directly (its call
// closure) touch nothing but registers and memory: no SVC, no BRK, no
// indirect call or unresolved indirect jump, no system register other than
// NZCV/FPCR/FPSR, no WFE-type hint, no call into a non-function address.
// Such a function can run from any register state on the hypervisor and
// recompiled, and both must end in the same state.
//
// Writes DIR/candidates.tsv (one function per line) and DIR/lifted.cpp
// (+ lifted.<n>.cpp with --split): the lifted closures with dispatch entries.
// For a game the output is derived game code: DIR belongs in $LGP_PRIVATE_DIR.
#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "lgp/recomp/a64.hpp"
#include "lgp/recomp/a64_simd.hpp"
#include "lgp/recomp/codegen.hpp"
#include "lgp/recomp/loader.hpp"

using namespace lgp::recomp;

namespace {

struct Args {
    std::vector<std::string> inputs;
    std::string out, module;
    size_t max_closure = 64, max_insns = 20000, limit = 0, split = 0;
};

[[noreturn]] void usage() {
    std::fprintf(stderr,
                 "usage: lgp_fn_select <module...> --out DIR [--module NAME] [--max-closure N] [--max-insns N]\n"
                 "                     [--limit N] [--split N]\n");
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
        if (s == "--out") a.out = next();
        else if (s == "--module") a.module = next();
        else if (s == "--max-closure") a.max_closure = std::stoull(next());
        else if (s == "--max-insns") a.max_insns = std::stoull(next());
        else if (s == "--limit") a.limit = std::stoull(next());
        else if (s == "--split") a.split = std::stoull(next());
        else if (!s.empty() && s[0] == '-') usage();
        else a.inputs.push_back(s);
    }
    if (a.inputs.empty() || a.out.empty()) usage();
    return a;
}

// Why a function cannot run in isolation (first reason found).
enum class Reject : uint8_t {
    None, Svc, Brk, Undefined, IndirectCall, IndirectJump, SysReg, Hint, NonEntryCall, Terminator,
    Replaced, ClosureTooBig, CalleeRejected, kCount
};
const char* reject_name(Reject r) {
    switch (r) {
    case Reject::None: return "ok";
    case Reject::Svc: return "svc";
    case Reject::Brk: return "brk";
    case Reject::Undefined: return "undefined";
    case Reject::IndirectCall: return "indirect-call";
    case Reject::IndirectJump: return "indirect-jump";
    case Reject::SysReg: return "sysreg";
    case Reject::Hint: return "hint";
    case Reject::NonEntryCall: return "call-into-non-entry";
    case Reject::Terminator: return "noreturn/trap/unknown";
    case Reject::Replaced: return "setjmp/longjmp";
    case Reject::ClosureTooBig: return "closure-too-big";
    case Reject::CalleeRejected: return "callee-rejected";
    default: return "?";
    }
}

struct Local {
    Reject reject = Reject::None;
    bool simd = false;
    bool loop = false;
    std::vector<uint64_t> callees;
};

Local inspect(const LinkedImage& image, const AnalysisResult& analysis, const Function& f) {
    Local l;
    for (const BasicBlock& b : f.blocks) {
        switch (b.term) {
        case Terminator::NoReturnCall:
        case Terminator::Trap:
        case Terminator::Unknown:
            l.reject = Reject::Terminator;
            return l;
        case Terminator::IndirectJump:
            l.reject = Reject::IndirectJump;
            return l;
        default:
            break;
        }
        for (uint64_t s : b.successors)
            if (s <= b.start)
                l.loop = true;
        for (uint64_t pc = b.start; pc < b.end; pc += 4) {
            const a64::Inst i = a64::decode(image.read_u32(pc), pc);
            switch (i.op) {
            case a64::Op::Svc: l.reject = Reject::Svc; return l;
            case a64::Op::Brk: l.reject = Reject::Brk; return l;
            case a64::Op::Invalid: l.reject = Reject::Undefined; return l;
            case a64::Op::Blr: l.reject = Reject::IndirectCall; return l;
            case a64::Op::Br:
                if (b.term != Terminator::JumpTable && b.term != Terminator::Branch) {
                    l.reject = Reject::IndirectJump;
                    return l;
                }
                break;
            case a64::Op::Mrs:
            case a64::Op::Msr:
                if (i.sysreg != a64::sr::kNzcv && i.sysreg != a64::sr::kFpcr && i.sysreg != a64::sr::kFpsr) {
                    l.reject = Reject::SysReg;
                    return l;
                }
                break;
            case a64::Op::Hint:
                if (i.raw != 0xD503201Fu) {  // only NOP: WFE/WFI/YIELD/SEV depend on other cores
                    l.reject = Reject::Hint;
                    return l;
                }
                break;
            case a64::Op::Simd:
                if (a64::simd::lift(i.raw, pc, false).invalid != a64::Invalid::None) {
                    l.reject = Reject::Undefined;
                    return l;
                }
                l.simd = true;
                break;
            default:
                break;
            }
        }
        for (const Call& c : b.calls) {
            if (c.indirect || c.target == 0) {
                l.reject = Reject::IndirectCall;
                return l;
            }
            if (!analysis.is_entry(c.target)) {
                l.reject = Reject::NonEntryCall;
                return l;
            }
            l.callees.push_back(c.target);
        }
    }
    return l;
}

}  // namespace

int main(int argc, char** argv) {
    const Args a = parse_args(argc, argv);
    try {
        std::vector<Module> mods;
        for (const std::string& p : a.inputs) mods.push_back(load_module_file(p));
        const LinkedImage image = link(std::move(mods));
        const AnalysisResult analysis = analyze(image);

        CodegenOptions co;
        const std::set<uint64_t> replaced_set = [&] {
            std::set<uint64_t> s = entries_for_symbols(image, analysis, co.setjmp_symbols);
            for (uint64_t e : entries_for_symbols(image, analysis, co.longjmp_symbols)) s.insert(e);
            return s;
        }();

        std::map<uint64_t, Local> local;
        for (const Function& f : analysis.functions) {
            Local l = inspect(image, analysis, f);
            if (l.reject == Reject::None && replaced_set.count(f.entry)) l.reject = Reject::Replaced;
            local.emplace(f.entry, std::move(l));
        }

        struct Candidate {
            uint64_t entry;
            std::vector<uint64_t> closure;
            uint64_t insns = 0;
            bool simd = false, loop = false;
        };
        std::vector<Candidate> candidates;
        uint64_t counts[static_cast<size_t>(Reject::kCount)] = {};
        for (const Function& f : analysis.functions) {
            const Module* m = image.module_at(f.entry);
            if (!a.module.empty() && (m == nullptr || m->name != a.module))
                continue;
            Candidate c{f.entry, {}, 0, false, false};
            Reject why = Reject::None;
            std::set<uint64_t> seen{f.entry};
            std::vector<uint64_t> work{f.entry};
            while (!work.empty() && why == Reject::None) {
                const uint64_t e = work.back();
                work.pop_back();
                const Local& l = local.at(e);
                if (l.reject != Reject::None) {
                    why = e == f.entry ? l.reject : Reject::CalleeRejected;
                    break;
                }
                c.closure.push_back(e);
                c.insns += analysis.function_at(e)->instruction_count();
                c.simd = c.simd || l.simd;
                c.loop = c.loop || l.loop;
                if (c.closure.size() > a.max_closure || c.insns > a.max_insns) {
                    why = Reject::ClosureTooBig;
                    break;
                }
                for (uint64_t t : l.callees)
                    if (seen.insert(t).second) work.push_back(t);
            }
            ++counts[static_cast<size_t>(why)];
            if (why == Reject::None) candidates.push_back(std::move(c));
        }
        if (a.limit != 0 && candidates.size() > a.limit) {
            // Spread the sample over the whole address range.
            std::vector<Candidate> sample;
            for (size_t i = 0; i < a.limit; ++i) sample.push_back(candidates[i * candidates.size() / a.limit]);
            candidates = std::move(sample);
        }

        std::filesystem::create_directories(a.out);
        std::set<uint64_t> lift_set;
        uint64_t lift_insns = 0;
        {
            std::ofstream tsv(a.out + "/candidates.tsv");
            tsv << "# entry\tmodule\toffset\tclosure_functions\tclosure_insns\tsimd\tloop\n";
            for (const Candidate& c : candidates) {
                const Module* m = image.module_at(c.entry);
                char line[160];
                std::snprintf(line, sizeof(line), "0x%" PRIx64 "\t%s\t0x%" PRIx64 "\t%zu\t%" PRIu64 "\t%d\t%d\n", c.entry,
                              m ? m->name.c_str() : "?", m ? c.entry - m->base : 0, c.closure.size(), c.insns,
                              c.simd ? 1 : 0, c.loop ? 1 : 0);
                tsv << line;
                lift_set.insert(c.closure.begin(), c.closure.end());
            }
            if (!tsv.good()) throw FormatError("cannot write candidates.tsv");
        }

        AnalysisResult subset;
        for (const Function& f : analysis.functions)
            if (lift_set.count(f.entry)) {
                subset.functions.push_back(f);
                subset.function_entries.push_back(f.entry);
                lift_insns += f.instruction_count();
            }
        co.parts = a.split != 0 ? a.split : std::max<size_t>(1, static_cast<size_t>(lift_insns / 40000));
        const ModuleCode mc = generate_module(image, subset, co);
        for (size_t i = 0; i < mc.sources.size(); ++i) {
            const std::string path = a.out + (i == 0 ? "/lifted.cpp" : "/lifted." + std::to_string(i) + ".cpp");
            std::ofstream f(path, std::ios::binary);
            f << mc.sources[i];
            if (!f.good()) throw FormatError("cannot write " + path);
        }

        std::printf("functions analysed   %zu\n", analysis.functions.size());
        for (size_t i = 0; i < static_cast<size_t>(Reject::kCount); ++i)
            if (counts[i] != 0) std::printf("  %-22s %" PRIu64 "\n", reject_name(static_cast<Reject>(i)), counts[i]);
        std::printf("candidates written   %zu\n", candidates.size());
        std::printf("lifted functions     %zu (%" PRIu64 " instructions, %zu translation units, %" PRIu64
                    " unsupported)\n",
                    subset.functions.size(), lift_insns, mc.sources.size(), mc.stats.lift.unsupported);
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "lgp_fn_select: %s\n", e.what());
        return 1;
    }
}
