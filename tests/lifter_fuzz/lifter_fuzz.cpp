// Long-running lifter fuzzer: generates batches of random mixed sequences,
// lifts them, compiles each batch with the host compiler into a shared
// library, loads it and compares every case against Unicorn. Divergent cases
// are minimised (instructions replaced by NOP, then removed; overrides
// dropped) and printed as lines for regressions.txt.
//
// Usage: lgp_lifter_fuzz [--seed S] [--cases N | --seconds T] [--batch B]
//                        [--max-len L] [--work DIR] [--cxx PATH] [--opt FLAG] [--jobs J] [--ubsan]
//                        [--release-shape] [--case-seed X] [--max-failures N] [--no-minimize]
//                        [--replay regressions.txt]
// Exit code: 0 no divergence, 1 divergences found, 2 usage or tool error.
#include <dlfcn.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "fuzz_gen.hpp"
#include "fuzz_run.hpp"

#ifndef LGP_FUZZ_CXX
#error "LGP_FUZZ_CXX (host C++ compiler) must be defined by the build"
#endif
#ifndef LGP_FUZZ_INCLUDES
#error "LGP_FUZZ_INCLUDES (-I flags for lifted code) must be defined by the build"
#endif

namespace {

using lgp_fuzz::FuzzCase;
using lgp_fuzz::Verdict;

struct Options {
    uint64_t seed = 1;
    uint64_t cases = 2000;
    double seconds = 0;  // > 0: run until the time is up instead of a case count
    unsigned batch = 800;
    lgp_fuzz::GenOptions gen;
    std::string work;
    std::string opt = "-O2";
    std::string cxx = LGP_FUZZ_CXX;
    unsigned jobs = std::max(1u, std::thread::hardware_concurrency());
    bool ubsan = false;
    bool precise = true;
    bool minimize = true;
    unsigned max_failures = 5;
    bool have_case_seed = false;
    uint64_t case_seed = 0;
    std::string replay;
};

// A compiled batch: one shared library per part (parts compile in parallel),
// loaded with their case tables.
class Batch {
public:
    Batch() = default;
    Batch(const Batch&) = delete;
    Batch& operator=(const Batch&) = delete;
    ~Batch() {
        for (void* h : handles_)
            dlclose(h);
    }

    bool build(const std::vector<FuzzCase>& cases, const Options& o, std::string& error) {
        static unsigned counter = 0;
        const size_t parts = std::max<size_t>(1, std::min<size_t>(o.jobs, (cases.size() + 15) / 16));
        std::vector<std::string> stems(parts), errors(parts);
        std::vector<std::thread> threads;
        for (size_t p = 0; p < parts; ++p) {
            stems[p] = o.work + "/batch_" + std::to_string(getpid()) + "_" + std::to_string(counter++);
            std::vector<FuzzCase> part;
            for (size_t k = p; k < cases.size(); k += parts)
                part.push_back(cases[k]);
            lgp::recomp::LiftStats stats;
            std::ofstream f(stems[p] + ".cpp", std::ios::binary);
            f << lgp_fuzz::emit_cases(part, "lgp_fuzz_table", "fuzz", stats, o.precise);
            if (!f) return error = "cannot write " + stems[p] + ".cpp", false;
        }
        for (size_t p = 0; p < parts; ++p)
            threads.emplace_back([&, p] { errors[p] = compile(stems[p], o); });
        for (auto& t : threads)
            t.join();
        for (size_t p = 0; p < parts; ++p)
            if (!errors[p].empty()) return error = errors[p], false;

        std::vector<std::vector<const lgp_test::CaseDef*>> tables(parts);
        for (size_t p = 0; p < parts; ++p) {
            void* h = dlopen((stems[p] + ".so").c_str(), RTLD_NOW | RTLD_LOCAL);
            std::filesystem::remove(stems[p] + ".so");
            if (!h) return error = std::string("dlopen: ") + dlerror(), false;
            handles_.push_back(h);
            const auto* table = static_cast<const lgp_test::CaseDef*>(dlsym(h, "lgp_fuzz_table"));
            const auto* count = static_cast<const size_t*>(dlsym(h, "lgp_fuzz_table_count"));
            if (!table || !count) return error = "case table missing in compiled batch", false;
            for (size_t k = 0; k < *count; ++k)
                tables[p].push_back(&table[k]);
        }
        // Back to the order of `cases` (round-robin over the parts).
        for (size_t k = 0; k < cases.size(); ++k)
            cases_.push_back(tables[k % parts][k / parts]);
        return true;
    }

    size_t size() const { return cases_.size(); }
    const lgp_test::CaseDef& operator[](size_t k) const { return *cases_[k]; }

private:
    // clang links its UBSan runtime statically into executables only, so a
    // dlopen()ed batch would miss __ubsan_handle_*: link the shared runtime
    // and find it through an rpath (GCC links libubsan shared by itself; for
    // GCC no runtime library answers and nothing is added).
    static std::string ubsan_runtime([[maybe_unused]] const Options& o) {
#if defined(__APPLE__) || defined(_WIN32)
        return {};
#else
        static const std::string flags = [&] {
            // One line of the compiler's output for `option`.
            const auto ask = [&](const std::string& option) {
                const std::string out = (std::filesystem::temp_directory_path() / "lgp_fuzz_ask.txt").string();
                const std::string cmd = "\"" + o.cxx + "\" " + option + " > \"" + out + "\" 2>/dev/null";
                std::string line;
                if (std::system(cmd.c_str()) == 0) {
                    std::ifstream in(out);
                    std::getline(in, line);
                }
                std::filesystem::remove(out);
                return line;
            };
#if defined(__x86_64__)
            const std::string arch = "x86_64";
#elif defined(__aarch64__)
            const std::string arch = "aarch64";
#else
            const std::string arch;
#endif
            // The runtime's folder: newer layouts name the library without the
            // architecture (-print-runtime-dir), Debian/Ubuntu packages keep
            // lib/linux/libclang_rt.ubsan_standalone-<arch>.so.
            std::error_code ec;
            for (const std::string name : {std::string("libclang_rt.ubsan_standalone.so"),
                                           "libclang_rt.ubsan_standalone-" + arch + ".so"}) {
                const std::filesystem::path lib = ask("-print-file-name=" + name);
                if (lib.is_absolute() && std::filesystem::exists(lib, ec))
                    return " -shared-libsan -Wl,-rpath,\"" + lib.parent_path().string() + "\"";
            }
            const std::filesystem::path dir = ask("-print-runtime-dir");
            if (!dir.empty() && std::filesystem::is_directory(dir, ec))
                return " -shared-libsan -Wl,-rpath,\"" + dir.string() + "\"";
            return std::string();
        }();
        return flags;
#endif
    }

    static std::string compile(const std::string& stem, const Options& o) {
        std::string cmd = "\"" + o.cxx + "\" -std=c++20 -fPIC -shared -w " + o.opt +
                          " -ffp-contract=off -fno-fast-math -DLGP_RUNTIME_HEADER='\"guest_stub.hpp\"' " +
                          LGP_FUZZ_INCLUDES;
        if (o.ubsan) cmd += " -fsanitize=undefined -fno-sanitize-recover=undefined" + ubsan_runtime(o);
#ifdef __APPLE__
        cmd += " -undefined dynamic_lookup";
#endif
        cmd += " -o \"" + stem + ".so\" \"" + stem + ".cpp\" 2> \"" + stem + ".log\"";
        const int rc = std::system(cmd.c_str());
        std::ifstream log(stem + ".log");
        std::stringstream s;
        s << log.rdbuf();
        log.close();
        std::filesystem::remove(stem + ".log");
        if (rc != 0) return "compiling " + stem + ".cpp failed (kept):\n" + s.str().substr(0, 4000);
        std::filesystem::remove(stem + ".cpp");
        return {};
    }

    std::vector<void*> handles_;
    std::vector<const lgp_test::CaseDef*> cases_;
};

struct Runner {
    lgp::difftest::UnicornExecutor unicorn;
    lgp_test::RecompiledExecutor lifted;
    bool precise = true;
    Verdict check(const lgp_test::CaseDef& c) { return lgp_fuzz::check(c, unicorn, lifted, precise); }
};

// A CaseDef of a compiled case with other overrides (no recompilation needed:
// overrides only change the initial state).
Verdict check_with_overrides(Runner& r, const lgp_test::CaseDef& c, const std::vector<lgp_test::RegOverride>& ov) {
    lgp_test::CaseDef d = c;
    d.overrides = ov.data();
    d.override_count = static_cast<uint32_t>(ov.size());
    return r.check(d);
}

bool is_branch(uint32_t w) {
    const auto i = lgp::recomp::a64::decode(w, lgp_test::kCodeBase);
    return i.flow != lgp::recomp::a64::Flow::Normal;
}

// A reduction must still diverge with a valid oracle run: replacing the loop
// counter setup by a NOP, say, makes Unicorn hit its instruction limit, which
// is a different (generator) failure.
bool still_diverges(const Verdict& v) {
    using lgp::difftest::Outcome;
    return !v.diffs.empty() && (v.outcome == Outcome::Sentinel || v.outcome == Outcome::Fault);
}

// Words the minimiser keeps: the call structure (BL, the caller's sentinel,
// RET) and the flag setters after a BL and at the callee's entry. Without them
// the lifted code may legitimately differ (AAPCS64 flags) or the call breaks.
bool structural(const std::vector<uint32_t>& code, size_t k) {
    const size_t split = lgp_fuzz::callee_split(code);
    if (split == SIZE_MAX) return false;
    auto is_bl = [&](size_t i) { return (code[i] & 0xFC000000u) == 0x94000000u; };
    return k == 0 || k == split || k == split + 1 || code[k] == lgp_fuzz::kRet || is_bl(k) || (k > 0 && is_bl(k - 1));
}

// Greedy minimisation. Each round compiles all one-step reductions of the
// current case in one batch and keeps the first that still diverges.
bool minimize(FuzzCase& c, Runner& r, const Options& o, std::string& error) {
    for (;;) {
        std::vector<FuzzCase> cand;
        for (size_t k = 0; k < c.code.size(); ++k) {
            if (c.code[k] == lgp_fuzz::kNop || structural(c.code, k)) continue;
            FuzzCase n = c;
            n.code[k] = lgp_fuzz::kNop;
            cand.push_back(std::move(n));
        }
        // Removing NOPs shifts later code: only without branches and literal loads in between.
        bool pc_relative = false;
        for (uint32_t w : c.code)
            pc_relative |= is_branch(w) || (w & 0x3B000000u) == 0x18000000u || (w & 0x1F000000u) == 0x10000000u;
        if (!pc_relative) {
            FuzzCase n = c;
            std::erase(n.code, lgp_fuzz::kNop);
            if (!n.code.empty() && n.code.size() != c.code.size()) cand.push_back(std::move(n));
        }
        if (cand.empty()) break;
        Batch b;
        if (!b.build(cand, o, error)) return false;
        size_t pick = cand.size();
        for (size_t k = 0; k < b.size() && pick == cand.size(); ++k)
            if (still_diverges(r.check(b[k]))) pick = k;
        if (pick == cand.size()) break;
        c.code = cand[pick].code;
    }

    // Overrides: drop those that do not matter (base registers of remaining
    // memory accesses stay: without them the case faults in both executors).
    std::vector<FuzzCase> one{c};
    Batch b;
    if (!b.build(one, o, error)) return false;
    std::vector<lgp_test::RegOverride> ov = c.overrides;
    for (size_t k = ov.size(); k-- > 0;) {
        std::vector<lgp_test::RegOverride> n = ov;
        n.erase(n.begin() + static_cast<std::ptrdiff_t>(k));
        if (still_diverges(check_with_overrides(r, b[0], n))) ov = std::move(n);
    }
    c.overrides = std::move(ov);
    return true;
}

bool parse_args(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
        const char* v = nullptr;
        if (a == "--seed" && (v = next()))
            o.seed = std::strtoull(v, nullptr, 0);
        else if (a == "--cases" && (v = next()))
            o.cases = std::strtoull(v, nullptr, 0);
        else if (a == "--seconds" && (v = next()))
            o.seconds = std::strtod(v, nullptr);
        else if (a == "--batch" && (v = next()))
            o.batch = static_cast<unsigned>(std::strtoul(v, nullptr, 0));
        else if (a == "--max-len" && (v = next()))
            o.gen.max_len = static_cast<unsigned>(std::strtoul(v, nullptr, 0));
        else if (a == "--work" && (v = next()))
            o.work = v;
        else if (a == "--opt" && (v = next()))
            o.opt = v;
        else if (a == "--cxx" && (v = next()))
            o.cxx = v;
        else if (a == "--jobs" && (v = next()))
            o.jobs = std::max(1u, static_cast<unsigned>(std::strtoul(v, nullptr, 0)));
        else if (a == "--ubsan")
            o.ubsan = true;
        else if (a == "--release-shape")
            o.precise = false;
        else if (a == "--no-minimize")
            o.minimize = false;
        else if (a == "--max-failures" && (v = next()))
            o.max_failures = static_cast<unsigned>(std::strtoul(v, nullptr, 0));
        else if (a == "--case-seed" && (v = next()))
            o.have_case_seed = true, o.case_seed = std::strtoull(v, nullptr, 0);
        else if (a == "--replay" && (v = next()))
            o.replay = v;
        else
            return false;
    }
    return o.batch > 0;
}

}  // namespace

int main(int argc, char** argv) {
    Options o;
    if (!parse_args(argc, argv, o)) {
        std::fprintf(stderr,
                     "usage: %s [--seed S] [--cases N | --seconds T] [--batch B] [--max-len L] [--work DIR]\n"
                     "          [--cxx PATH] [--opt FLAG] [--jobs J] [--ubsan] [--release-shape] [--case-seed X] [--max-failures N]\n"
                     "          [--no-minimize]\n"
                     "          [--replay regressions.txt]\n",
                     argv[0]);
        return 2;
    }
    if (o.work.empty()) o.work = (std::filesystem::temp_directory_path() / "lgp_lifter_fuzz").string();
    std::filesystem::create_directories(o.work);

    // Case source: one regenerated case, a regression file or seeded batches.
    std::vector<FuzzCase> fixed;
    if (o.have_case_seed) {
        fixed.push_back(lgp_fuzz::generate(o.case_seed, o.gen));
    } else if (!o.replay.empty()) {
        std::ifstream f(o.replay, std::ios::binary);
        std::stringstream s;
        s << f.rdbuf();
        std::string error;
        if (!f || !lgp_fuzz::parse_regressions(s.str(), fixed, error)) {
            std::fprintf(stderr, "lgp_lifter_fuzz: %s: %s\n", o.replay.c_str(), f ? error.c_str() : "cannot read");
            return 2;
        }
    }

    Runner runner;
    runner.precise = o.precise;
    const auto start = std::chrono::steady_clock::now();
    auto elapsed = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(); };
    uint64_t index = 0, total = 0, faults = 0;
    std::vector<FuzzCase> failures;
    for (;;) {
        std::vector<FuzzCase> batch;
        if (!fixed.empty()) {
            if (index > 0) break;
            batch = fixed;
            index = fixed.size();
        } else {
            const bool done = o.seconds > 0 ? elapsed() >= o.seconds : index >= o.cases;
            if (done) break;
            const uint64_t n = o.seconds > 0 ? o.batch : std::min<uint64_t>(o.batch, o.cases - index);
            for (uint64_t k = 0; k < n; ++k)
                batch.push_back(lgp_fuzz::generate(lgp_fuzz::case_seed(o.seed, index + k), o.gen));
            index += n;
        }
        Batch b;
        std::string error;
        if (!b.build(batch, o, error)) {
            std::fprintf(stderr, "lgp_lifter_fuzz: %s\n", error.c_str());
            return 2;
        }
        for (size_t k = 0; k < b.size(); ++k) {
            const Verdict v = runner.check(b[k]);
            ++total;
            if (v.outcome == lgp::difftest::Outcome::Fault) ++faults;
            if (v.diffs.empty()) continue;
            std::fputs(lgp_fuzz::report(b[k], v).c_str(), stdout);
            failures.push_back(batch[k]);
        }
        std::printf("lgp_lifter_fuzz: %" PRIu64 " cases, %zu divergent, %.0f s\n", total, failures.size(), elapsed());
        std::fflush(stdout);
        if (failures.size() >= o.max_failures) break;
    }

    if (o.minimize && !failures.empty()) {
        std::printf("\nMinimised cases for tests/lifter_fuzz/regressions.txt:\n");
        for (FuzzCase& c : failures) {
            std::string error;
            if (!minimize(c, runner, o, error)) {
                std::fprintf(stderr, "lgp_lifter_fuzz: minimising %s: %s\n", c.name.c_str(), error.c_str());
                return 2;
            }
            Batch b;
            if (!b.build({c}, o, error)) {
                std::fprintf(stderr, "lgp_lifter_fuzz: %s\n", error.c_str());
                return 2;
            }
            std::fputs(lgp_fuzz::report(b[0], runner.check(b[0])).c_str(), stdout);
            std::printf("%s\n\n", lgp_fuzz::format_regression(c).c_str());
        }
    }
    std::printf("lgp_lifter_fuzz: seed=0x%" PRIx64 " %" PRIu64 " cases (%" PRIu64 " ending in a fault), %zu divergent\n",
                o.seed, total, faults, failures.size());
    return failures.empty() ? 0 : 1;
}
