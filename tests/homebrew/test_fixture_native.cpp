// Stage C of docs/analysis/56: a homebrew fixture runs as rewritten original
// code natively on an AArch64 host (lgp::rt::native), with the same end state
// as the original code under Unicorn.
//
// usage: lgp_fixture_native --signals [--base HEX]
//        lgp_fixture_native <module> [--oracle] [--native] [--write-expected FILE] [--expected FILE]
//                           [--base HEX] [--sched free|core-baton|baton] [--trace]
//
//   --signals         self-test of faults, pause and the exit kick on a synthetic module (signals_self_test)
//   --oracle          run the original module under Unicorn (build with LGP_FIXTURE_ORACLE)
//   --native          run the rewritten module natively (AArch64 hosts)
//   --write-expected  write the oracle's end state (for a native run on another machine)
//   --expected        compare the native run with such a file
//   --sched           scheduler of the native run (default core-baton, see main())
//
// Both runs use the same guest addresses: the address space starts at --base
// (default 0x80'0000'0000), natively with guest address == host address. The
// module is rewritten in memory by the recompiler's native rewrite (plan +
// patch area, stage B); the oracle maps the same patch area but runs the
// original text. The comparison leaves out what the rewrite changes on
// purpose: the hashes of the text block and of the patch area, and the
// hashes of stack and heap blocks (gate frames and stub slots live below the
// guest SP, Unicorn leaves those bytes as they were). Exit, debug output,
// the module's .data/.bss and every other block are compared.
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "../recomp/synthetic_module.hpp"
#include "fixture_process.hpp"
#include "lgp/recomp/cfg.hpp"
#include "lgp/recomp/link.hpp"
#include "lgp/recomp/loader.hpp"
#include "lgp/recomp/native_image.hpp"
#include "lgp/recomp/native_rewrite.hpp"
#include "lgp/runtime/memory.hpp"
#include "lgp/runtime/native.hpp"
#include "lgp/runtime/system.hpp"
#include "lgp/runtime/thread.hpp"

#if LGP_FIXTURE_ORACLE
#include "unicorn_guest.hpp"
#endif

namespace {

using namespace lgp_fixture;
namespace rc = lgp::recomp;
namespace rt = lgp::rt;

// macOS 26 on arm64 refuses fixed mappings at 0x10_0000_0000..0x6f_ffff_ffff
// (Mac M1, 23.10.); 0x80_0000_0000 is free there and on Linux.
constexpr uint64_t kDefaultBase = 0x80'0000'0000;

uint64_t segment_offset(const rc::Module& m, rc::SegmentKind kind) {
    for (const rc::Segment& s : m.segments)
        if (s.kind == kind) return s.offset;
    throw std::runtime_error("module without segment kind " + std::to_string(static_cast<int>(kind)));
}

uint64_t fnv1a(const uint8_t* p, uint64_t n) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint64_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

// The rewrite of one module at `base`.
struct Rewrite {
    rc::NativePatch patch;
    uint64_t text_end = 0;     // VA
    uint64_t image_end = 0;    // VA, page aligned
    uint64_t unpatched = 0;    // gate sites left as they are: would run the real SVC/MRS/MSR
};

Rewrite rewrite(const rc::Module& original, uint64_t base) {
    rc::Module m = original;
    m.base = base;
    rc::LinkOptions lo;
    lo.first_base = base;
    std::vector<rc::Module> mods;
    mods.push_back(std::move(m));
    // Linked only for the analysis (pointers in the data become CFG seeds);
    // the process loads the unrelocated image, libnx relocates itself.
    rc::LinkedImage img = rc::link(std::move(mods), lo);
    const rc::AnalysisResult res = rc::analyze(img);
    const std::vector<rc::NativeRewrite> plan = rc::plan_native_rewrite(img, res);
    Rewrite r;
    r.patch = rc::build_native_patch(img, plan);
    const rc::Module& lm = img.modules.at(0);
    r.text_end = base + segment_offset(lm, rc::SegmentKind::Ro);
    r.image_end = base + ((lm.image.size() + rt::kPageSize - 1) & ~(rt::kPageSize - 1));
    const rc::NativeRewrite& p = plan.at(0);
    r.unpatched = p.gate_exclusive + p.gate_x18;
    // x18 sites the rewrite leaves (for example libnx's exception entry,
    // which saves and restores the caller's x18) run on the host's x18. On
    // Linux and macOS user code does not depend on it (Apple: reserved, the
    // kernel may zero it); the gate restores the host's value before host code.
    for (const uint64_t va : p.x18_unpatched)
        std::printf("  x18 site left as is +0x%" PRIx64 ": %08x\n", va - base, img.read_u32(va));
    std::printf("rewrite: %zu words, %llu renamed, %llu stack-slot stubs, %llu gate stubs; unpatched: %zu x18 sites, "
                "%llu gate sites in exclusive windows, %llu gate sites naming x18; %llu SP-from-register sites (counted)\n",
                r.patch.modules.at(0).words.size(), (unsigned long long)r.patch.sites_renamed,
                (unsigned long long)r.patch.spill_stubs, (unsigned long long)r.patch.gate_stubs, p.x18_unpatched.size(),
                (unsigned long long)p.gate_exclusive, (unsigned long long)p.gate_x18, (unsigned long long)p.sp_sites);
    return r;
}

// The patch area as guest memory (both runs; only the native run uses it).
void map_area(rt::Kernel& k, const rc::NativePatch& p) {
    if (k.map_owned(p.area_base, p.area.size(), rt::MemState::Code, rt::kPermRX).failed())
        throw std::runtime_error("cannot map the patch area");
    std::memcpy(k.memory().host_ptr(p.area_base, p.area.size()), p.area.data(), p.area.size());
}

// End state as text, without what the rewrite changes on purpose (see top).
std::string native_view(const EndState& s, const Rewrite& r, uint64_t base) {
    std::ostringstream o;
    o << "exit " << s.exit << "\n";
    for (const std::string& line : s.debug_output) o << "debug " << line << "\n";
    char buf[160];
    std::snprintf(buf, sizeof buf, "data +0x%" PRIx64 " size 0x%zx fnv 0x%016" PRIx64 "\n", s.data_addr - base,
                  s.data.size(), fnv1a(s.data.data(), s.data.size()));
    o << buf;
    for (const EndState::Block& b : s.blocks) {
        const auto st = static_cast<rt::MemState>(b.state);
        const bool text = b.addr < r.text_end && b.addr + b.size > base;
        const bool area = b.addr >= r.patch.area_base && b.addr < r.patch.area_base + r.patch.area.size();
        const bool scratch = st == rt::MemState::Stack || st == rt::MemState::Normal;
        if (text || area || scratch)
            std::snprintf(buf, sizeof buf, "block +0x%" PRIx64 " size 0x%" PRIx64 " state 0x%x perm %u fnv -\n",
                          b.addr - base, b.size, b.state, b.perm);
        else
            std::snprintf(buf, sizeof buf, "block +0x%" PRIx64 " size 0x%" PRIx64 " state 0x%x perm %u fnv 0x%016" PRIx64 "\n",
                          b.addr - base, b.size, b.state, b.perm, b.hash);
        o << buf;
    }
    return o.str();
}

bool compare_text(const std::string& expected, const std::string& actual) {
    if (expected == actual) return true;
    std::istringstream e(expected), a(actual);
    std::string el, al;
    for (int line = 1;; ++line) {
        const bool he = static_cast<bool>(std::getline(e, el));
        const bool ha = static_cast<bool>(std::getline(a, al));
        if (!he && !ha) break;
        if (!he) el = "<none>";
        if (!ha) al = "<none>";
        if (el != al) std::printf("  line %d: expected '%s'\n          actual   '%s'\n", line, el.c_str(), al.c_str());
    }
    return false;
}

// Native code: the module's text and the patch area's code.
uint64_t g_text_begin = 0, g_text_end = 0, g_area_begin = 0, g_area_end = 0;
lgp::LiftedFn resolve_native(uint64_t va) {
    if ((va >= g_text_begin && va < g_text_end) || (va >= g_area_begin && va < g_area_end)) return &rt::native::run;
    return nullptr;
}

#if LGP_FIXTURE_ORACLE
lgp::LiftedFn resolve_oracle(uint64_t) { return &run_under_unicorn; }
#endif

// Runs `module` rewritten and natively (the native half of main()).
EndState run_native(const rc::Module& module, const Rewrite& r, uint64_t base, RunOptions o) {
    o.after_load = [&, then = o.after_load](rt::Kernel& k) {
        rt::GuestMemory& mem = k.memory();
        for (const auto& [va, word] : r.patch.modules.at(0).words)
            std::memcpy(mem.host_ptr(va, 4), &word, 4);
        map_area(k, r.patch);
        rt::native::install_gate(r.patch.host_fn_slot);
        const uint64_t start = mem.layout().start();
        mem.host_block().make_executable(base - start, r.text_end - base);
        mem.host_block().make_executable(r.patch.area_base - start, r.patch.code_size);
        if (then)
            then(k);
    };
    g_text_begin = base;
    g_text_end = r.text_end;
    g_area_begin = r.patch.area_base;
    g_area_end = r.patch.area_base + r.patch.code_size;
    rt::set_indirect_resolver(&resolve_native);
    EndState s = run_fixture(module, o);
    rt::set_indirect_resolver(nullptr);
    return s;
}

// --signals: faults in native code end the guest thread with the halt reason
// lifted code would give, a thread spinning without SVCs leaves guest code
// when the process ends and stops while it is paused (kick signal).
// Synthetic module, own code.
int signals_self_test(uint64_t base) {
    synth::Builder b;
    b.text_size = 0x4000;  // .data a 16 KiB page away from .text
    b.ro_size = 0x4000;
    std::vector<uint64_t> fn;  // module offsets
    auto add = [&](std::initializer_list<uint32_t> words) {
        const uint64_t start = b.text_off(b.text_words.size());
        b.text_words.insert(b.text_words.end(), words);
        b.fdes.push_back({start, b.text_off(b.text_words.size())});
        fn.push_back(start);
        return fn.size() - 1;
    };
    constexpr uint32_t kRet = 0xD65F03C0;
    const size_t segv = add({0xD2800000 /* movz x0, #0 */, 0xF9400001 /* ldr x1, [x0] */, kRet});
    const size_t brk = add({0xD4202460 /* brk #0x123 */, kRet});
    const size_t udf = add({0x00000042 /* udf #0x42 */, kRet});
    const size_t spin = add({0x14000000 /* b . */});
    // The counters through the gate (MRS, not SVC): CNTFRQ is the console's.
    const size_t counters = add({0xD53BE001 /* mrs x1, cntfrq_el0 */, 0xD53BE042 /* mrs x2, cntvct_el0 */,
                                 0xD53BE023 /* mrs x3, cntpct_el0 */, 0xD29F0004 /* movz x4, #0xf800 */,
                                 0xF2A02484 /* movk x4, #0x124, lsl #16 */, 0xEB04003F /* cmp x1, x4 */,
                                 0x54000041 /* b.ne .+8 */, 0xD40000E1 /* svc #0x7 ExitProcess */,
                                 0xD4200020 /* brk #1 */});
    // The guest FPCR (round toward minus) survives an SVC; the gate runs
    // the host side with the host FPCR.
    const size_t fpcr = add({0xD2A01001 /* movz x1, #0x80, lsl #16 */, 0xD51B4401 /* msr fpcr, x1 */,
                             0xD40003C1 /* svc #0x1e GetSystemTick */, 0xD53B4402 /* mrs x2, fpcr */,
                             0xEB01005F /* cmp x2, x1 */, 0x54000041 /* b.ne .+8 */,
                             0xD40000E1 /* svc #0x7 ExitProcess */, 0xD4200020 /* brk #1 */});
    const size_t jump_out = add({0xD2800200 /* movz x0, #0x10 */, 0xD61F0000 /* br x0 */});
    // Starts a thread at `spin`, then ends the process while it spins.
    // `then`: what the starting thread does afterwards.
    auto start_spinner = [&](uint32_t then) {
        const uint64_t adr_at = b.text_off(b.text_words.size());
        const int64_t rel = static_cast<int64_t>(fn[spin]) - static_cast<int64_t>(adr_at);
        const uint32_t adr = 0x10000000u | ((static_cast<uint32_t>(rel) & 3) << 29) |
                             (((static_cast<uint32_t>(rel) >> 2) & 0x7FFFF) << 5) | 1;  // adr x1, spin
        return add({adr, 0xD2800002 /* movz x2, #0 */, 0xD14043E3 /* sub x3, sp, #0x10, lsl #12 */,
                    0x52800584 /* movz w4, #44 */, 0x12800025 /* movn w5, #1 (core -2) */,
                    0xD4000101 /* svc #0x8 CreateThread */, 0x2A0103E0 /* mov w0, w1 */,
                    0xD4000121 /* svc #0x9 StartThread */, then});
    };
    const size_t spin_exit = start_spinner(0xD40000E1 /* svc #0x7 ExitProcess */);
    // Both threads spin; a host thread pauses, resumes, pauses again and
    // stops the process.
    const size_t spin_pause = start_spinner(0x14000000 /* b . */);
    // Single baton: main spins on a flag until a thread of lower priority
    // sets it, without SVC in the loop. Reading the counter there hands the
    // baton over at the gate; without, the kicker's time slice does.
    const size_t flag_worker = add({0x52800021 /* mov w1, #1 */, 0xB9000001 /* str w1, [x0] */,
                                    0xD4000141 /* svc #0xa ExitThread */});
    auto flag_spinner = [&](std::initializer_list<uint32_t> loop) {
        const uint64_t adr_at = b.text_off(b.text_words.size() + 2);
        const int64_t rel = static_cast<int64_t>(fn[flag_worker]) - static_cast<int64_t>(adr_at);
        const uint32_t adr = 0x10000000u | ((static_cast<uint32_t>(rel) & 3) << 29) |
                             (((static_cast<uint32_t>(rel) >> 2) & 0x7FFFF) << 5) | 1;  // adr x1, flag_worker
        std::vector<uint32_t> w = {0xD10083E2 /* sub x2, sp, #0x20 (the flag) */, 0xB900005F /* str wzr, [x2] */,
                                   adr, 0xD14043E3 /* sub x3, sp, #0x10, lsl #12 */, 0x52800644 /* movz w4, #50 */,
                                   0x12800025 /* movn w5, #1 (core -2) */, 0xD4000101 /* svc #0x8 CreateThread */,
                                   0x2A0103E0 /* mov w0, w1 */, 0xD4000121 /* svc #0x9 StartThread */,
                                   0xD10083E2 /* sub x2, sp, #0x20 */};
        w.insert(w.end(), loop);
        w.push_back(0xD40000E1 /* svc #0x7 ExitProcess */);
        const uint64_t start = b.text_off(b.text_words.size());
        b.text_words.insert(b.text_words.end(), w.begin(), w.end());
        b.fdes.push_back({start, b.text_off(b.text_words.size())});
        fn.push_back(start);
        return fn.size() - 1;
    };
    const size_t counter_spin = flag_spinner({0xB9400046 /* ldr w6, [x2] */, 0xD53BE047 /* mrs x7, cntvct_el0 */,
                                              0x34FFFFC6 /* cbz w6, .-8 */});
    const size_t plain_spin = flag_spinner({0xB9400046 /* ldr w6, [x2] */, 0x34FFFFE6 /* cbz w6, .-4 */});
    rc::Module module = rc::load_nso(b.build_nso(false), "main");
    const Rewrite r = rewrite(module, base);
    RunOptions o;
    o.base = base;
    o.at_guest_address = true;
    o.extra_code = r.patch.area_base + r.patch.area.size() - r.image_end;
    o.sched_mode = rt::SchedMode::Free;
    struct Case {
        const char* name;
        size_t fn;
        std::string exit;  // expected start of EndState::exit
    };
    char buf[96];
    auto crash = [&](uint32_t halt, uint64_t info, size_t f, uint32_t word) {
        std::snprintf(buf, sizeof buf, "Crash halt %u info 0x%" PRIx64 " pc 0x%" PRIx64, halt, info,
                      base + fn[f] + 4 * word);
        return std::string(buf);
    };
    const Case cases[] = {
        {"load from address 0", segv, crash(lgp::kHaltMemFault, 0, segv, 1)},
        {"brk #0x123", brk, crash(lgp::kHaltBrk, 0x123, brk, 0)},
        {"udf #0x42", udf, crash(lgp::kHaltUndefined, 0x42, udf, 0)},
        {"counters by MRS", counters, "ExitProcess"},
        {"guest FPCR across an SVC", fpcr, "ExitProcess"},
        {"branch out of guest memory", jump_out, "Crash halt 4 info 0x10 pc 0x10"},
        {"exit while a thread spins", spin_exit, "ExitProcess"},
    };
    int failed = 0;
    for (const Case& c : cases) {
        module.entry_offset = fn[c.fn];
        const EndState s = run_native(module, r, base, o);
        const bool ok = s.exit == c.exit;
        std::printf("%s: %s: exit '%s'%s%s\n", ok ? "PASS" : "FAIL", c.name, s.exit.c_str(), ok ? "" : ", expected ",
                    ok ? "" : c.exit.c_str());
        failed += ok ? 0 : 1;
    }
    for (const auto& [name, f] : {std::pair{"baton spin-wait reading the counter", counter_spin},
                                  std::pair{"baton spin-wait without SVC (time slice)", plain_spin}}) {
        module.entry_offset = fn[f];
        std::thread guard;
        std::atomic<bool> finished{false};
        RunOptions bo = o;
        bo.sched_mode = rt::SchedMode::Baton;
        bo.after_load = [&](rt::Kernel& k) {
            // Stops a hung run: exit Stopped instead of ExitProcess.
            guard = std::thread([&k, &finished] {
                for (int i = 0; i < 500 && !finished.load(); ++i)
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                if (!finished.load())
                    k.scheduler().stop();
            });
        };
        const EndState s = run_native(module, r, base, bo);
        finished = true;
        guard.join();
        const bool ok = s.exit == "ExitProcess";
        std::printf("%s: %s: exit '%s'%s\n", ok ? "PASS" : "FAIL", name, s.exit.c_str(),
                    ok ? "" : ", expected 'ExitProcess'");
        failed += ok ? 0 : 1;
    }
    {
        // Two threads in guest code without SVCs: pause() must find both at
        // rest (parked by the kick), twice, with a resume in between.
        module.entry_offset = fn[spin_pause];
        std::thread host;
        bool paused[2] = {false, false};
        int active[2] = {-1, -1};
        RunOptions po = o;
        po.after_load = [&](rt::Kernel& k) {
            host = std::thread([&k, &paused, &active] {
                rt::Scheduler& s = k.scheduler();
                for (int i = 0; i < 2; ++i) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                    paused[i] = s.pause(3000);
                    active[i] = s.active_threads();
                    s.resume();
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                s.stop();
            });
        };
        const EndState s = run_native(module, r, base, po);
        host.join();
        const bool ok = s.exit == "Stopped" && paused[0] && paused[1] && active[0] == 0 && active[1] == 0;
        std::printf("%s: pause while two threads spin: pause %d/%d, active %d/%d, exit '%s'\n", ok ? "PASS" : "FAIL",
                    paused[0], paused[1], active[0], active[1], s.exit.c_str());
        failed += ok ? 0 : 1;
    }
    return failed == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);  // lines before a hang reach the log
    std::string module_path, write_expected, expected_path;
    bool oracle = false, native = false, signals = false;
    uint64_t base = kDefaultBase;
    // The fixtures switch threads only at SVCs (tests/fixtures/threads), so
    // the core baton works natively too and the run stays deterministic.
    // Code that spins without an SVC needs --sched free (no back-edge polls
    // in native code, Analyse 56 §2).
    rt::SchedMode native_sched = rt::SchedMode::CoreBaton;
    RunOptions options;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--signals") signals = true;
        else if (a == "--oracle") oracle = true;
        else if (a == "--native") native = true;
        else if (a == "--trace") options.trace_svcs = true;
        else if (a == "--write-expected" && i + 1 < argc) write_expected = argv[++i];
        else if (a == "--expected" && i + 1 < argc) expected_path = argv[++i];
        else if (a == "--base" && i + 1 < argc) base = std::strtoull(argv[++i], nullptr, 16);
        else if (a == "--sched" && i + 1 < argc) {
            const std::string m = argv[++i];
            if (m == "free") native_sched = rt::SchedMode::Free;
            else if (m == "core-baton") native_sched = rt::SchedMode::CoreBaton;
            else if (m == "baton") native_sched = rt::SchedMode::Baton;
            else {
                std::fprintf(stderr, "--sched free|core-baton|baton\n");
                return 2;
            }
        }
        else if (module_path.empty() && a[0] != '-') module_path = a;
        else {
            std::fprintf(stderr, "usage: %s <module> [--oracle] [--native] [--write-expected FILE] [--expected FILE] "
                                 "[--base HEX] [--sched MODE] [--trace]\n", argv[0]);
            return 2;
        }
    }
    if (signals) {
        if (!rt::native::available()) {
            std::fprintf(stderr, "native execution needs an AArch64 host\n");
            return 2;
        }
        try {
            return signals_self_test(base);
        } catch (const std::exception& e) {
            std::printf("FAIL: %s\n", e.what());
            return 1;
        }
    }
    if (module_path.empty() || (!oracle && !native)) {
        std::fprintf(stderr, "need a module and --oracle and/or --native\n");
        return 2;
    }

    try {
        const rc::Module module = load_fixture(module_path);
        const Rewrite r = rewrite(module, base);
        if (r.unpatched != 0) {
            // Natively such a site would run the real instruction (a host
            // system call, the host's thread register).
            std::printf("FAIL: %llu sites cannot be patched\n", (unsigned long long)r.unpatched);
            return 1;
        }
        // W^X: the text's last page must not be a page of .data, also with
        // 16 KiB host pages (Apple silicon). basic and threads leave room by
        // their layout (.rodata in between), LGP by megabytes (Analyse 56 §2).
        const uint64_t data_start = base + segment_offset(module, rc::SegmentKind::Data);
        if (((r.text_end + 0x3FFF) & ~uint64_t{0x3FFF}) > data_start || (base & 0x3FFF) != 0) {
            std::printf("FAIL: .text and .data share a 16 KiB page (or --base is not 16 KiB aligned)\n");
            return 1;
        }
        options.base = base;
        // Both runs: the same addresses and the same kind of address space.
        options.at_guest_address = true;
        options.extra_code = r.patch.area_base + r.patch.area.size() - r.image_end;
        std::printf("module %s: base 0x%" PRIx64 ", text 0x%" PRIx64 " bytes, patch area +0x%" PRIx64 " (0x%zx bytes)\n",
                    module_path.c_str(), base, r.text_end - base, r.patch.area_base - base, r.patch.area.size());

        std::string expected_text;
        if (oracle) {
#if LGP_FIXTURE_ORACLE
            RunOptions o = options;
            o.after_load = [&](rt::Kernel& k) { map_area(k, r.patch); };
            rt::set_indirect_resolver(&resolve_oracle);
            const EndState s = run_fixture(module, o);
            rt::set_indirect_resolver(nullptr);
            std::printf("oracle (Unicorn, original code): exit %s, %zu debug line(s)\n", s.exit.c_str(),
                        s.debug_output.size());
            for (const std::string& line : s.debug_output) std::printf("    debug: %s\n", line.c_str());
            expected_text = native_view(s, r, base);
            if (!write_expected.empty()) {
                std::ofstream f(write_expected, std::ios::binary);
                f << expected_text;
                if (!f) throw std::runtime_error("cannot write " + write_expected);
            }
#else
            std::fprintf(stderr, "--oracle needs a build with Unicorn\n");
            return 2;
#endif
        }
        if (!native) return 0;
        if (!rt::native::available()) {
            std::fprintf(stderr, "native execution needs an AArch64 host\n");
            return 2;
        }
        if (!oracle) {
            if (expected_path.empty()) {
                std::fprintf(stderr, "--native without --oracle needs --expected\n");
                return 2;
            }
            std::ifstream f(expected_path, std::ios::binary);
            std::ostringstream ss;
            ss << f.rdbuf();
            if (!f) throw std::runtime_error("cannot read " + expected_path);
            expected_text = ss.str();
        }

        RunOptions o = options;
        o.sched_mode = native_sched;
        const EndState s = run_native(module, r, base, o);
        std::printf("native (rewritten code): exit %s, %zu debug line(s)\n", s.exit.c_str(), s.debug_output.size());
        for (const std::string& line : s.debug_output) std::printf("    debug: %s\n", line.c_str());
        const std::string actual = native_view(s, r, base);
        if (!compare_text(expected_text, actual)) {
            std::printf("FAIL: native end state differs from the oracle\n");
            return 1;
        }
        std::printf("PASS: native end state equals the oracle\n");
        return 0;
    } catch (const std::exception& e) {
        std::printf("FAIL: %s\n", e.what());
        return 1;
    }
}
