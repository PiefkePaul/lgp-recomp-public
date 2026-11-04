// lgp_fn_difftest: function-level differential test (ADR-0006 stage 3).
//
//   <runner> <module...> --candidates FILE [--report FILE] [--cases N] [--seed S]
//            [--only ADDR]... [--case K] [--timeout-ms T] [--no-fork] [--verbose] [--min-ok N]
//            [--fpcr-mix] [--known FILE]
//
// For every function in the candidates file (written by lgp_fn_select from
// the same modules), N register/memory states are generated from a seeded
// generator. Each state runs once natively on the hypervisor (EL0, MMU on)
// and once through the lifted code linked into this program; the function
// returns to a sentinel BRK. Compared: outcome, x0..x30, sp, NZCV, v0..v31,
// FPCR/FPSR (without the IEEE exception flags, which the lifter does not
// model) and every writable byte (module data/BSS, argument buffers,
// stack). Where the hypervisor faults, the recompiled run must fault too
// (registers are not compared then: lifted code keeps no precise PC).
//
// Runs in a child process that the parent restarts behind a function that
// crashes or hangs the recompiled side (reported as "crash"); the hypervisor
// side has its own timeout ("hv-timeout", recompiled run skipped).
//
// Output lines (also the report file): one per function,
//   <entry> <module>+<offset> <verdict> agree=<n> fault=<n> hv-timeout=<n> mismatch=<n>
//   fault-va-differs=<n> [| first difference] [| first differing fault address]
// For a game this is analysis output: keep the report in $LGP_PRIVATE_DIR.
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "hv/hv_machine.h"
#include "lgp/lifted/lifted.hpp"
#include "lgp/recomp/link.hpp"
#include "lgp/recomp/loader.hpp"
#include "lgp/runtime/dispatch.hpp"
#include "lgp/runtime/kernel.hpp"
#include "lgp/runtime/system.hpp"

extern const lgp::LiftedEntry lgp_lifted_entries[];
extern const size_t lgp_lifted_entries_count;

namespace rc = lgp::recomp;
namespace dt = lgp::difftest;
using namespace lgp::rt;

namespace {

constexpr uint64_t kPage = 0x1000;
constexpr uint64_t kMiB = 1ull << 20;
constexpr uint64_t kStackSize = 1 * kMiB;
constexpr uint64_t kBufSize = 0x10000;  // per argument buffer
constexpr int kBuffers = 8;
constexpr uint32_t kSentinelInsn = 0xD4200000u | (uint32_t{dt::kSentinelBrk} << 5);

constexpr uint32_t kFpsrIeeeFlags = 0x9F;

uint64_t round_up(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }

std::string hex(uint64_t v) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "0x%" PRIx64, v);
    return buf;
}

struct Args {
    std::vector<std::string> modules;
    std::string candidates, report;
    unsigned cases = 16;
    uint64_t seed = 1;
    std::vector<uint64_t> only;
    int only_case = -1;
    unsigned timeout_ms = 200;
    bool fork = true, verbose = false;
    bool fpcr_mix = false;  // vary FPCR (FZ, DN, rounding modes) over the cases
    std::string known;      // file of "ADDR reason" lines: mismatches explained by M1 != A57
    size_t first = 0;  // internal: child starts at this candidate index
    size_t min_ok = 0;  // fail unless this many functions agreed on at least one normal return
};

[[noreturn]] void usage() {
    std::fprintf(stderr,
                 "usage: lgp_fn_difftest <module...> --candidates FILE [--report FILE] [--cases N] [--seed S]\n"
                 "                       [--only ADDR]... [--case K] [--timeout-ms T] [--no-fork] [--verbose]\n"
                 "                       [--min-ok N] [--fpcr-mix] [--known FILE]\n");
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
        if (s == "--candidates") a.candidates = next();
        else if (s == "--report") a.report = next();
        else if (s == "--cases") a.cases = static_cast<unsigned>(std::stoul(next()));
        else if (s == "--seed") a.seed = std::stoull(next(), nullptr, 0);
        else if (s == "--only") a.only.push_back(std::stoull(next(), nullptr, 0));
        else if (s == "--case") a.only_case = std::stoi(next());
        else if (s == "--timeout-ms") a.timeout_ms = static_cast<unsigned>(std::stoul(next()));
        else if (s == "--no-fork") a.fork = false;
        else if (s == "--fpcr-mix") a.fpcr_mix = true;
        else if (s == "--known") a.known = next();
        else if (s == "--verbose") a.verbose = true;
        else if (s == "--first") a.first = std::stoull(next());
        else if (s == "--min-ok") a.min_ok = std::stoull(next());
        else if (!s.empty() && s[0] == '-') usage();
        else a.modules.push_back(s);
    }
    if (a.modules.empty() || a.candidates.empty() || a.cases == 0) usage();
    return a;
}

struct Candidate {
    uint64_t entry;
    std::string where;  // module+offset
    std::string known;  // reason from --known: a mismatch is reported as "known"
};

// --known: functions whose mismatches are hardware differences between the
// M1 and the Switch's A57 (see tests/difftest/README.md), one "ADDR reason"
// per line, '#' comments.
std::map<uint64_t, std::string> read_known(const std::string& path) {
    std::map<uint64_t, std::string> out;
    if (path.empty()) return out;
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot read " + path);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ls(line);
        std::string addr, reason;
        ls >> addr;
        std::getline(ls, reason);
        out[std::stoull(addr, nullptr, 0)] = reason.empty() ? "known" : reason.substr(reason.find_first_not_of(' '));
    }
    return out;
}

std::vector<Candidate> read_candidates(const Args& a) {
    std::ifstream in(a.candidates);
    if (!in) throw std::runtime_error("cannot read " + a.candidates);
    const std::map<uint64_t, std::string> known = read_known(a.known);
    std::vector<Candidate> out;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ls(line);
        std::string entry, module, offset;
        ls >> entry >> module >> offset;
        Candidate c{std::stoull(entry, nullptr, 0), module + "+" + offset, {}};
        if (auto it = known.find(c.entry); it != known.end()) c.known = it->second;
        if (a.only.empty() || std::find(a.only.begin(), a.only.end(), c.entry) != a.only.end())
            out.push_back(c);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Guest memory plan, identical on both sides.

struct Area {
    uint64_t va = 0, size = 0;
    uint32_t perms = 0;           // dt::MemPerm
    std::vector<uint8_t> pristine;  // initial bytes of writable module areas
};

struct Plan {
    rc::LinkedImage image;
    std::vector<Area> module_areas;  // per segment, page-rounded
    Layout layout;
    uint64_t sentinel = 0;
    uint64_t stack_base = 0, buf_base = 0;
};

uint32_t seg_perms(rc::SegmentKind k) {
    switch (k) {
    case rc::SegmentKind::Text: return dt::kRead | dt::kExec;
    case rc::SegmentKind::Ro: return dt::kRead;
    default: return dt::kRead | dt::kWrite;
    }
}

Plan make_plan(const Args& a) {
    Plan p;
    std::vector<rc::Module> mods;
    for (const std::string& path : a.modules) mods.push_back(rc::load_module_file(path));
    p.image = rc::link(std::move(mods));
    const uint64_t start = p.image.modules.front().base;
    // Areas are page-rounded; Data and Bss may share a page (both RW).
    std::map<uint64_t, uint32_t> page_perms;
    for (const rc::Module& m : p.image.modules)
        for (const rc::Segment& s : m.segments) {
            if (s.size == 0) continue;
            const uint64_t lo = (m.base + s.offset) & ~(kPage - 1);
            const uint64_t hi = round_up(m.base + s.offset + s.size, kPage);
            for (uint64_t q = lo; q < hi; q += kPage) page_perms[q] |= seg_perms(s.kind);
        }
    for (auto it = page_perms.begin(); it != page_perms.end();) {
        Area ar{it->first, kPage, it->second, {}};
        auto j = std::next(it);
        while (j != page_perms.end() && j->first == ar.va + ar.size && j->second == ar.perms) {
            ar.size += kPage;
            ++j;
        }
        if (ar.perms & dt::kWrite) {
            ar.pristine.assign(ar.size, 0);
            for (const rc::Module& m : p.image.modules) {
                const uint64_t lo = std::max(ar.va, m.base), hi = std::min(ar.va + ar.size, m.base + m.image.size());
                if (lo < hi) std::memcpy(ar.pristine.data() + (lo - ar.va), m.image.data() + (lo - m.base), hi - lo);
            }
        }
        p.module_areas.push_back(std::move(ar));
        it = j;
    }
    p.sentinel = round_up(p.image.end_va(), kPage);
    const uint64_t code = round_up(p.sentinel + kPage - start, 2 * kMiB);
    p.layout = Layout::packed(start, code, round_up(kStackSize, 2 * kMiB), 2 * kMiB, 2 * kMiB,
                              round_up(kBuffers * kBufSize, 2 * kMiB));
    p.stack_base = p.layout.stack.base;
    p.buf_base = p.layout.heap.base;
    return p;
}

// ---------------------------------------------------------------------------
// Inputs.

struct Rng {
    uint64_t s;
    uint64_t next() {
        uint64_t z = (s += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
};

// Special FP values: NaNs (quiet and signalling, with payload), infinities,
// signed zeros, denormals, extremes.
uint64_t special_double(Rng& r) {
    static constexpr uint64_t kSpecial[] = {
        0x7FF8000000000000ull, 0xFFF8000000000123ull, 0x7FF0000000000001ull, 0x7FF4000000000abcull,
        0x7FF0000000000000ull, 0xFFF0000000000000ull, 0x8000000000000000ull, 0x0000000000000001ull,
        0x800FFFFFFFFFFFFFull, 0x0010000000000000ull, 0x7FEFFFFFFFFFFFFFull, 0x3FF0000000000000ull,
    };
    return kSpecial[r.next() % (sizeof(kSpecial) / sizeof(kSpecial[0]))];
}
uint32_t special_float(Rng& r) {
    static constexpr uint32_t kSpecial[] = {
        0x7FC00000u, 0xFFC00123u, 0x7F800001u, 0x7FA00abcu, 0x7F800000u, 0xFF800000u,
        0x80000000u, 0x00000001u, 0x807FFFFFu, 0x00800000u, 0x7F7FFFFFu, 0x3F800000u,
    };
    return kSpecial[r.next() % (sizeof(kSpecial) / sizeof(kSpecial[0]))];
}

uint64_t nice_double(Rng& r) {
    double d;
    switch (r.next() % 6) {
    case 0: d = static_cast<double>(static_cast<int64_t>(r.next() % 200) - 100); break;
    case 1: d = (static_cast<double>(r.next() % 2000001) - 1e6) / 997.0; break;
    case 2: d = static_cast<double>(r.next() % 1000) / 1000.0; break;
    case 3: d = 0.0; break;
    case 4: return r.next() % 8 == 0 ? special_double(r) : nice_double(r);
    default: d = (static_cast<double>(r.next() >> 11) / 9007199254740992.0) * 2.0 - 1.0; break;
    }
    uint64_t bits;
    std::memcpy(&bits, &d, 8);
    return bits;
}
uint32_t nice_float(Rng& r) {
    if (r.next() % 16 == 0) return special_float(r);
    uint64_t db = nice_double(r);
    double d;
    std::memcpy(&d, &db, 8);
    const float f = static_cast<float>(d);
    uint32_t bits;
    std::memcpy(&bits, &f, 4);
    return bits;
}

struct Case {
    dt::GuestState state;
    std::vector<uint8_t> buffers;  // kBuffers * kBufSize at buf_base
    std::vector<uint8_t> stack;    // kStackSize at stack_base
};

// Profiles: 0 pointer arguments into random buffers, 1 small integers,
// 2 random 64-bit values, 3 pointer-rich buffers (structures of pointers).
// FPCR values for --fpcr-mix: default, FZ, DN, FZ+DN, and the three other
// rounding modes (RP, RM, RZ). 7 is coprime to the 4 profiles.
constexpr uint32_t kFpcrMix[] = {0, 1u << 24, 1u << 25, 3u << 24, 1u << 22, 2u << 22, 3u << 22};

Case make_case(const Plan& p, uint64_t seed, uint64_t entry, unsigned k, bool fpcr_mix) {
    Rng r{seed * 0x100000001B3ull ^ entry * 0x9E3779B97F4A7C15ull ^ (uint64_t{k} << 48)};
    const unsigned profile = k % 4;
    Case c;
    auto buf_ptr = [&](Rng& rr) {
        const uint64_t b = rr.next() % kBuffers;
        return p.buf_base + b * kBufSize + 0x4000 + (rr.next() % 0x1000) * 8;
    };
    auto fill = [&](std::vector<uint8_t>& v, size_t n) {
        v.resize(n);
        for (size_t i = 0; i < n; i += 8) {
            uint64_t w;
            if (profile == 3) {
                switch (r.next() % 4) {
                case 0: w = buf_ptr(r); break;
                case 1: w = r.next() % 16; break;
                case 2: w = nice_double(r); break;
                default: w = r.next(); break;
                }
            } else if (profile == 1) {
                w = r.next() & 0x0F0F0F0F0F0F0F0Full;  // small bytes
            } else {
                w = r.next();
            }
            std::memcpy(v.data() + i, &w, 8);
        }
    };
    fill(c.buffers, kBuffers * kBufSize);
    fill(c.stack, kStackSize);

    dt::GuestState& s = c.state;
    for (int i = 0; i < 31; ++i) s.x[i] = r.next();
    for (int i = 0; i < 8; ++i) {
        switch (profile) {
        case 0: s.x[i] = p.buf_base + i * kBufSize + 0x8000; break;
        case 1: s.x[i] = r.next() % 64; break;
        case 2: break;
        default:
            switch (r.next() % 3) {
            case 0: s.x[i] = p.buf_base + i * kBufSize + 0x8000; break;
            case 1: s.x[i] = r.next() % 16; break;
            default: s.x[i] = buf_ptr(r); break;
            }
        }
    }
    if (profile != 2) s.x[8] = p.buf_base + 7 * kBufSize + 0x100;  // indirect result location
    s.x[30] = p.sentinel;
    s.sp = p.stack_base + kStackSize - 0x10000;
    s.pc = entry;
    s.nzcv = static_cast<uint32_t>(r.next() & 0xF) << 28;
    if (fpcr_mix) s.fpcr = kFpcrMix[k % (sizeof(kFpcrMix) / sizeof(kFpcrMix[0]))];
    for (int i = 0; i < 32; ++i) {
        if (i < 8 && profile != 2) {
            auto half = [&] {
                return (r.next() & 1) ? nice_double(r) : (uint64_t{nice_float(r)} | (uint64_t{nice_float(r)} << 32));
            };
            s.v[i] = {half(), half()};
        } else {
            s.v[i] = {r.next(), r.next()};
        }
    }
    return c;
}

// ---------------------------------------------------------------------------
// Results.

struct RunOut {
    enum class Kind { Returned, Fault, Timeout, Other } kind = Kind::Other;
    std::string detail;
    uint64_t fault_va = 0;
    dt::GuestState state;
    std::vector<std::vector<uint8_t>> memory;  // writable module areas, buffers, stack
};

const char* kind_name(RunOut::Kind k) {
    switch (k) {
    case RunOut::Kind::Returned: return "returned";
    case RunOut::Kind::Fault: return "fault";
    case RunOut::Kind::Timeout: return "timeout";
    default: return "other";
    }
}

std::vector<std::string> compare(const Plan& p, const RunOut& hv, const RunOut& re) {
    std::vector<std::string> d;
    if (hv.kind != re.kind) {
        d.push_back(std::string("outcome hv=") + kind_name(hv.kind) + " (" + hv.detail + ") rec=" +
                    kind_name(re.kind) + " (" + re.detail + ")");
        return d;
    }
    if (hv.kind != RunOut::Kind::Returned) return d;
    dt::RunResult a, b;
    a.outcome = b.outcome = dt::Outcome::Sentinel;
    a.final = hv.state;
    b.final = re.state;
    // The lifter does not model the cumulative IEEE exception flags (IOC, DZC,
    // OFC, UFC, IXC, IDC; docs/knowledge/a64-fp-simd-semantik.md); QC is kept.
    a.final.fpsr &= ~kFpsrIeeeFlags;
    b.final.fpsr &= ~kFpsrIeeeFlags;
    for (std::string& s : dt::diff(a, b)) d.push_back(std::move(s));
    std::vector<uint64_t> bases;
    for (const Area& ar : p.module_areas)
        if (ar.perms & dt::kWrite) bases.push_back(ar.va);
    bases.push_back(p.buf_base);
    bases.push_back(p.stack_base);
    for (size_t i = 0; i < hv.memory.size() && i < re.memory.size(); ++i) {
        const auto& x = hv.memory[i];
        const auto& y = re.memory[i];
        if (x == y) continue;
        for (size_t j = 0; j < x.size(); ++j)
            if (x[j] != y[j]) {
                char buf[128];
                std::snprintf(buf, sizeof(buf), "memory %s: hv %02x rec %02x", hex(bases[i] + j).c_str(), x[j], y[j]);
                d.push_back(buf);
                break;
            }
    }
    return d;
}

// ---------------------------------------------------------------------------
// The two sides.

class HvSide {
public:
    explicit HvSide(const Plan& p) : p_(p) {
        for (const Area& ar : p.module_areas) m_.map(ar.va, ar.size, ar.perms);
        for (const rc::Module& m : p.image.modules) m_.write(m.base, m.image.data(), m.image.size());
        m_.map(p.sentinel, kPage, dt::kRead | dt::kExec);
        m_.write(p.sentinel, &kSentinelInsn, 4);
        m_.map(p.stack_base, kStackSize, dt::kRead | dt::kWrite);
        m_.map(p.buf_base, kBuffers * kBufSize, dt::kRead | dt::kWrite);
    }

    RunOut run(const Case& c, unsigned timeout_ms) {
        for (const Area& ar : p_.module_areas)
            if (ar.perms & dt::kWrite) m_.write(ar.va, ar.pristine.data(), ar.size);
        m_.write(p_.buf_base, c.buffers.data(), c.buffers.size());
        m_.write(p_.stack_base, c.stack.data(), c.stack.size());
        m_.set_state(c.state);
        const dt::hv::Exit e = m_.run(std::chrono::milliseconds(timeout_ms));
        RunOut r;
        r.state = m_.state();
        if (e.kind == dt::hv::Exit::Kind::Brk && e.imm == dt::kSentinelBrk && r.state.pc == p_.sentinel) {
            r.kind = RunOut::Kind::Returned;
        } else if (e.kind == dt::hv::Exit::Kind::Timeout) {
            r.kind = RunOut::Kind::Timeout;
            r.detail = "timeout";
        } else if (e.kind == dt::hv::Exit::Kind::Fault &&
                   ((e.esr >> 26) == 0x24 || (e.esr >> 26) == 0x20 || (e.esr >> 26) == 0x22)) {
            // Data abort, instruction abort, or PC alignment (RET to a bad x30).
            r.kind = RunOut::Kind::Fault;
            r.fault_va = e.far;
            r.detail = e.detail;
        } else {
            r.kind = RunOut::Kind::Other;
            r.detail = e.kind == dt::hv::Exit::Kind::Brk ? "BRK #" + hex(e.imm) + " at " + hex(r.state.pc) : e.detail;
            if (e.kind == dt::hv::Exit::Kind::Svc) r.detail = "SVC #" + hex(e.imm);
        }
        if (r.kind == RunOut::Kind::Returned) {
            for (const Area& ar : p_.module_areas)
                if (ar.perms & dt::kWrite) {
                    r.memory.emplace_back(ar.size);
                    m_.read(ar.va, r.memory.back().data(), ar.size);
                }
            r.memory.emplace_back(c.buffers.size());
            m_.read(p_.buf_base, r.memory.back().data(), c.buffers.size());
            r.memory.emplace_back(c.stack.size());
            m_.read(p_.stack_base, r.memory.back().data(), c.stack.size());
        }
        return r;
    }

private:
    const Plan& p_;
    dt::hv::Machine m_;
};

uint8_t rt_perm(uint32_t perms) {
    uint8_t out = 0;
    if (perms & dt::kRead) out |= kPermR;
    if (perms & dt::kWrite) out |= kPermW;
    if (perms & dt::kExec) out |= kPermX;
    return out;
}

class RecSide {
public:
    explicit RecSide(const Plan& p) : p_(p), k_(config(p)) {
        set_active_kernel(&k_);
        GuestMemory& mem = k_.memory();
        const uint64_t code_size = p.sentinel + kPage - p.layout.code.base;
        check(k_.map_owned(p.layout.code.base, code_size, MemState::Code, kPermRW), "map code");
        for (const rc::Module& m : p.image.modules)
            std::memcpy(mem.host_ptr(m.base, m.image.size()), m.image.data(), m.image.size());
        std::memcpy(mem.host_ptr(p.sentinel, 4), &kSentinelInsn, 4);
        for (const Area& ar : p.module_areas) mem.set_perm(ar.va, ar.size, rt_perm(ar.perms));
        mem.set_perm(p.sentinel, kPage, kPermRX);
        check(k_.map_owned(p.stack_base, kStackSize, MemState::Stack, kPermRW), "map stack");
        check(k_.map_owned(p.buf_base, kBuffers * kBufSize, MemState::Normal, kPermRW), "map buffers");
        register_functions(lgp_lifted_entries, lgp_lifted_entries_count);
    }
    ~RecSide() { set_active_kernel(nullptr); }

    RunOut run(uint64_t entry, const Case& c) {
        GuestMemory& mem = k_.memory();
        for (const Area& ar : p_.module_areas)
            if (ar.perms & dt::kWrite) std::memcpy(mem.host_ptr(ar.va, ar.size), ar.pristine.data(), ar.size);
        std::memcpy(mem.host_ptr(p_.buf_base, c.buffers.size()), c.buffers.data(), c.buffers.size());
        std::memcpy(mem.host_ptr(p_.stack_base, c.stack.size()), c.stack.data(), c.stack.size());

        lgp::CpuState cpu{};
        const dt::GuestState& s = c.state;
        for (int i = 0; i < 31; ++i) cpu.x[i] = s.x[i];
        cpu.sp = s.sp;
        cpu.pc = s.pc;
        lgp::unpack_nzcv(cpu, s.nzcv);
        cpu.fpcr = s.fpcr;
        cpu.fpsr = s.fpsr;
        cpu.tpidr_el0 = s.tpidr_el0;
        cpu.tpidrro_el0 = s.tpidrro_el0;
        for (int i = 0; i < 32; ++i) cpu.vreg[i] = {s.v[i].lo, s.v[i].hi};

        RunOut r;
        const lgp::LiftedFn fn = lookup_function(entry);
        if (fn == nullptr) {
            r.kind = RunOut::Kind::Other;
            r.detail = "not lifted";
            return r;
        }
        run_lifted(&cpu, fn);

        dt::GuestState& o = r.state;
        for (int i = 0; i < 31; ++i) o.x[i] = cpu.x[i];
        o.sp = cpu.sp;
        o.pc = cpu.halt == lgp::kHaltNone ? cpu.x[30] : cpu.pc;  // RET leaves to x30
        o.nzcv = lgp::pack_nzcv(cpu);
        o.fpcr = cpu.fpcr;
        o.fpsr = cpu.fpsr;
        o.tpidr_el0 = cpu.tpidr_el0;
        o.tpidrro_el0 = cpu.tpidrro_el0;
        for (int i = 0; i < 32; ++i) o.v[i] = {cpu.vreg[i].lo, cpu.vreg[i].hi};

        if (cpu.halt == lgp::kHaltNone && o.pc == p_.sentinel) {
            r.kind = RunOut::Kind::Returned;
        } else if (cpu.halt == lgp::kHaltNone) {
            // RET to an x30 the function loaded from memory: the lifted code
            // just returns; the hardware fetches from there and aborts.
            r.kind = RunOut::Kind::Fault;
            r.fault_va = o.pc;
            r.detail = "returned to " + hex(o.pc);
        } else if (cpu.halt == lgp::kHaltMemFault) {
            r.kind = RunOut::Kind::Fault;
            r.fault_va = cpu.halt_info;
            r.detail = "memory fault at " + hex(cpu.halt_info);
        } else {
            r.kind = RunOut::Kind::Other;
            r.detail = "halt " + std::to_string(cpu.halt) + " info " + hex(cpu.halt_info) + " pc " + hex(o.pc);
        }
        if (r.kind == RunOut::Kind::Returned) {
            for (const Area& ar : p_.module_areas)
                if (ar.perms & dt::kWrite) {
                    const uint8_t* q = mem.host_ptr(ar.va, ar.size);
                    r.memory.emplace_back(q, q + ar.size);
                }
            const uint8_t* b = mem.host_ptr(p_.buf_base, c.buffers.size());
            r.memory.emplace_back(b, b + c.buffers.size());
            const uint8_t* st = mem.host_ptr(p_.stack_base, c.stack.size());
            r.memory.emplace_back(st, st + c.stack.size());
        }
        return r;
    }

private:
    static ProcessConfig config(const Plan& p) {
        ProcessConfig cfg;
        cfg.custom_layout = true;
        cfg.layout = p.layout;
        cfg.total_memory_size = p.layout.heap.size;
        return cfg;
    }
    static void check(const lgp::rt::Result& r, const char* what) {
        if (!r.ok()) throw std::runtime_error(std::string("runtime: ") + what + " failed");
    }

    const Plan& p_;
    Kernel k_;
};

// ---------------------------------------------------------------------------

struct FnSummary {
    unsigned agree = 0, fault = 0, hv_timeout = 0, mismatch = 0, fault_va = 0;
    std::string first, first_fault_va;
};

std::string verdict_line(const Candidate& c, const FnSummary& s) {
    const char* verdict = s.mismatch ? (c.known.empty() ? "MISMATCH" : "known")
                                     : (s.agree + s.fault == 0 ? "untested" : "ok");
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%s %s %s agree=%u fault=%u hv-timeout=%u mismatch=%u fault-va-differs=%u",
                  hex(c.entry).c_str(), c.where.c_str(), verdict, s.agree, s.fault, s.hv_timeout, s.mismatch,
                  s.fault_va);
    std::string out = buf;
    if (s.mismatch && !c.known.empty()) out += " | known: " + c.known;
    if (!s.first.empty()) out += " | " + s.first;
    if (!s.first_fault_va.empty()) out += " | fault address " + s.first_fault_va;
    return out;
}

// Runs candidates [first, end) and writes "S <i>" before and "R <line>"
// after each one to fd (the parent's pipe; stdout with --no-fork).
int child(const Args& a, const std::vector<Candidate>& cands, int fd) {
    const Plan plan = make_plan(a);
    HvSide hv(plan);
    RecSide rec(plan);
    auto say = [&](const std::string& s) {
        const std::string line = s + "\n";
        if (write(fd, line.data(), line.size()) < 0) std::_Exit(5);
    };
    for (size_t i = a.first; i < cands.size(); ++i) {
        const Candidate& c = cands[i];
        say("S " + std::to_string(i));
        FnSummary sum;
        for (unsigned k = 0; k < a.cases; ++k) {
            if (a.only_case >= 0 && static_cast<unsigned>(a.only_case) != k) continue;
            const Case cs = make_case(plan, a.seed, c.entry, k, a.fpcr_mix);
            const RunOut h = hv.run(cs, a.timeout_ms);
            if (h.kind == RunOut::Kind::Timeout) {
                ++sum.hv_timeout;
                continue;
            }
            alarm(10);  // a recompiled hang kills the child; the parent reports it
            const RunOut r = rec.run(c.entry, cs);
            alarm(0);
            const std::vector<std::string> d = compare(plan, h, r);
            if (d.empty()) {
                if (h.kind == RunOut::Kind::Returned) {
                    ++sum.agree;
                } else {
                    ++sum.fault;
                    // Same fault, different address: worth a look (an access
                    // straddling into an unmapped page reports the page on
                    // hardware, the access start in lifted code).
                    if (h.kind == RunOut::Kind::Fault && h.fault_va != r.fault_va) {
                        ++sum.fault_va;
                        if (sum.first_fault_va.empty())
                            sum.first_fault_va = "case " + std::to_string(k) + ": hv " + hex(h.fault_va) + " rec " +
                                                 hex(r.fault_va);
                    }
                }
            } else {
                ++sum.mismatch;
                if (sum.first.empty()) {
                    sum.first = "case " + std::to_string(k) + ":";
                    for (size_t j = 0; j < d.size() && j < 6; ++j) sum.first += " " + d[j] + ";";
                    if (d.size() > 6) sum.first += " (+" + std::to_string(d.size() - 6) + " more)";
                }
                if (a.verbose)
                    for (const std::string& line : d) std::fprintf(stderr, "  case %u: %s\n", k, line.c_str());
            }
        }
        say("R " + verdict_line(c, sum));
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    Args a = parse_args(argc, argv);
    try {
        const std::vector<Candidate> cands = read_candidates(a);
        std::FILE* report = a.report.empty() ? nullptr : std::fopen(a.report.c_str(), "w");
        if (!a.report.empty() && report == nullptr) throw std::runtime_error("cannot write " + a.report);
        std::map<std::string, unsigned> totals;
        size_t returned_ok = 0;
        auto emit = [&](const std::string& line) {
            std::printf("%s\n", line.c_str());
            if (report) std::fprintf(report, "%s\n", line.c_str());
            const size_t sp1 = line.find(' '), sp2 = line.find(' ', sp1 + 1), sp3 = line.find(' ', sp2 + 1);
            const std::string verdict = line.substr(sp2 + 1, sp3 - sp2 - 1);
            ++totals[verdict];
            if (verdict == "ok" && line.find(" agree=0 ") == std::string::npos) ++returned_ok;
        };

        if (!a.fork) {
            // Lines go to stdout as they are; no restart after a crash.
            return child(a, cands, STDOUT_FILENO);
        }
        size_t next = a.first;
        while (next < cands.size()) {
            int fds[2];
            if (pipe(fds) != 0) throw std::runtime_error("pipe failed");
            std::fflush(stdout);
            if (report) std::fflush(report);
            const pid_t pid = fork();
            if (pid < 0) throw std::runtime_error("fork failed");
            if (pid == 0) {
                close(fds[0]);
                a.first = next;
                int rc = 1;
                try {
                    rc = child(a, cands, fds[1]);
                } catch (const std::exception& e) {
                    std::fprintf(stderr, "child: %s\n", e.what());
                    rc = 3;
                }
                std::_Exit(rc);
            }
            close(fds[1]);
            std::FILE* in = fdopen(fds[0], "r");
            char buf[4096];
            long started = -1;
            while (std::fgets(buf, sizeof(buf), in) != nullptr) {
                std::string line(buf);
                if (!line.empty() && line.back() == '\n') line.pop_back();
                if (line.rfind("S ", 0) == 0) {
                    started = std::stol(line.substr(2));
                } else if (line.rfind("R ", 0) == 0) {
                    emit(line.substr(2));
                    next = static_cast<size_t>(started) + 1;
                    started = -1;
                }
            }
            std::fclose(in);
            int status = 0;
            waitpid(pid, &status, 0);
            if (started >= 0) {
                const Candidate& c = cands[static_cast<size_t>(started)];
                std::string why = WIFSIGNALED(status) ? "signal " + std::to_string(WTERMSIG(status))
                                                       : "exit " + std::to_string(WEXITSTATUS(status));
                if (WIFSIGNALED(status) && WTERMSIG(status) == SIGALRM) why = "recompiled run hung (> 10 s)";
                emit(hex(c.entry) + " " + c.where + " crash " + why);
                next = static_cast<size_t>(started) + 1;
            } else if (!(WIFEXITED(status) && WEXITSTATUS(status) == 0)) {
                std::fprintf(stderr, "lgp_fn_difftest: child failed before the first function (status %d)\n", status);
                return 1;
            }
        }
        std::printf("summary:");
        for (const auto& [k, v] : totals) std::printf(" %s=%u", k.c_str(), v);
        std::printf(" (functions %zu, cases per function %u, seed %" PRIu64 ")\n", cands.size(), a.cases, a.seed);
        if (report) std::fclose(report);
        std::printf("functions that agreed on a normal return: %zu\n", returned_ok);
        if (returned_ok < a.min_ok) {
            std::printf("FAIL: fewer than --min-ok %zu\n", a.min_ok);
            return 1;
        }
        return totals.count("MISMATCH") || totals.count("crash") ? 1 : 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "lgp_fn_difftest: %s\n", e.what());
        return 1;
    }
}
