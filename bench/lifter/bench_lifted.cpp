// Lifted-code benchmark (P6): runs every kernel of guest/kernels.cpp once as
// recompiled guest code (lgp_lift output on the runtime core) and once as the
// same C++ compiled natively for the host, checks that both return the same
// checksum and prints the times and their ratio.
//
// usage: lgp_bench_lifted <program-stem> [--kernel N] [--scale S] [--repeat R] [--csv]
//   <program-stem>.img/.meta come from lgp_bench_image.
// Exit code 1 if a checksum differs (the ctest smoke run relies on that).
#include <algorithm>
#include <cmath>
#include <memory>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "lgp/lifted/lifted.hpp"
#include "lgp/runtime/dispatch.hpp"
#include "lgp/runtime/kernel.hpp"

extern const lgp::LiftedEntry lgp_lifted_entries[];
extern const size_t lgp_lifted_entries_count;

namespace lgp_bench_native {
uint64_t bench_main(uint64_t kernel, uint64_t iters);
}

namespace {

using namespace lgp::rt;
constexpr uint64_t kMiB = 0x10'0000;
constexpr uint64_t kStackSize = 512 * 1024;

struct KernelInfo {
    const char* name;
    uint64_t iters;  // at scale 1: roughly 10-30 ms natively on a desktop CPU
};
const KernelInfo kKernels[] = {
    {"crc32", 200}, {"quicksort", 40}, {"list_walk", 400},  {"interp", 3000}, {"vcall", 30000},
    {"fib", 400},   {"mat4", 4000},    {"particles", 1000}, {"memops", 2000}, {"strings", 1000},
};

struct Segment {
    uint64_t offset, size;
    uint8_t perm;
};
struct Program {
    std::vector<uint8_t> image;
    uint64_t base = 0, entry = 0;
    std::vector<Segment> segments;
};

bool load_program(const std::string& stem, Program& p) {
    std::ifstream img(stem + ".img", std::ios::binary);
    p.image.assign(std::istreambuf_iterator<char>(img), {});
    std::ifstream meta(stem + ".meta");
    std::string key;
    uint64_t size = 0;
    while (meta >> key) {
        if (key == "base") meta >> p.base;
        else if (key == "entry") meta >> p.entry;
        else if (key == "size") meta >> size;
        else if (key == "segment") {
            Segment s;
            std::string perm;
            meta >> s.offset >> s.size >> perm;
            s.perm = perm == "rx" ? kPermRX : perm == "r" ? kPermR : kPermRW;
            p.segments.push_back(s);
        }
    }
    return !p.image.empty() && p.image.size() == size && p.entry != 0;
}

class Guest {
public:
    explicit Guest(const Program& p) : p_(p) {
        ProcessConfig cfg;
        cfg.total_memory_size = 32 * kMiB;
        cfg.custom_layout = true;
        cfg.layout = Layout::packed(p.base, 4 * kMiB, 2 * kMiB, 1 * kMiB, 1 * kMiB, 8 * kMiB);
        kernel_ = std::make_unique<Kernel>(cfg);
        set_active_kernel(kernel_.get());
        const Layout& l = kernel_->memory().layout();
        stack_top_ = l.stack.end();
        ok_ = p.image.size() <= l.code.size && kernel_->map_owned(l.code.base, p.image.size(), MemState::Code, kPermRW).ok() &&
              kernel_->map_owned(l.stack.end() - kStackSize, kStackSize, MemState::Stack, kPermRW).ok();
        if (!ok_) return;
        std::memcpy(kernel_->memory().host_ptr(l.code.base, p.image.size()), p.image.data(), p.image.size());
        for (const Segment& s : p.segments) {
            const uint64_t lo = s.offset & ~uint64_t{0xFFF};
            const uint64_t hi = (s.offset + s.size + 0xFFF) & ~uint64_t{0xFFF};
            kernel_->memory().set_perm(l.code.base + lo, hi - lo, s.perm);
        }
        entry_ = lookup_function(p.entry);
        ok_ = entry_ != nullptr;
    }
    ~Guest() { set_active_kernel(nullptr); }
    bool ok() const { return ok_; }

    // Returns false if the guest did not reach the sentinel BRK.
    bool run(uint64_t kernel, uint64_t iters, uint64_t& result) {
        lgp::CpuState cpu{};
        cpu.x[0] = kernel;
        cpu.x[1] = iters;
        cpu.sp = stack_top_;
        cpu.pc = p_.entry;
        run_lifted(&cpu, entry_);
        result = cpu.x[0];
        if (cpu.halt == lgp::kHaltBrk && cpu.halt_info == 0x4C47) return true;
        std::fprintf(stderr, "guest stopped: halt %u info 0x%" PRIx64 " pc 0x%" PRIx64 "\n", cpu.halt, cpu.halt_info, cpu.pc);
        return false;
    }

private:
    const Program& p_;
    std::unique_ptr<Kernel> kernel_;
    uint64_t stack_top_ = 0;
    lgp::LiftedFn entry_ = nullptr;
    bool ok_ = false;
};

template <typename F>
double best_ms(unsigned repeat, F&& f) {
    double best = 1e300;
    for (unsigned r = 0; r < repeat; ++r) {
        const auto t0 = std::chrono::steady_clock::now();
        f();
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        best = std::min(best, ms);
    }
    return best;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <program-stem> [--kernel N] [--scale S] [--repeat R] [--csv]\n", argv[0]);
        return 2;
    }
    int only = -1;
    double scale = 1.0;
    unsigned repeat = 3;
    bool csv = false;
    for (int i = 2; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--kernel") && i + 1 < argc) only = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--scale") && i + 1 < argc) scale = std::atof(argv[++i]);
        else if (!std::strcmp(argv[i], "--repeat") && i + 1 < argc) repeat = static_cast<unsigned>(std::atoi(argv[++i]));
        else if (!std::strcmp(argv[i], "--csv")) csv = true;
    }
    Program prog;
    if (!load_program(argv[1], prog)) {
        std::fprintf(stderr, "cannot read %s.img/.meta\n", argv[1]);
        return 2;
    }
    register_functions(lgp_lifted_entries, lgp_lifted_entries_count);
    Guest guest(prog);
    if (!guest.ok()) {
        std::fprintf(stderr, "guest setup failed\n");
        return 2;
    }

    if (csv) std::printf("kernel,iterations,native_ms,lifted_ms,ratio\n");
    else std::printf("%-10s %8s %11s %11s %8s\n", "kernel", "iters", "native ms", "lifted ms", "ratio");
    int failures = 0;
    double log_sum = 0;
    unsigned count = 0;
    for (int k = 0; k < static_cast<int>(std::size(kKernels)); ++k) {
        if (only >= 0 && k != only) continue;
        const uint64_t iters = std::max<uint64_t>(1, static_cast<uint64_t>(static_cast<double>(kKernels[k].iters) * scale));
        uint64_t expected = 0, actual = 0;
        bool ran = true;
        const double native = best_ms(repeat, [&] { expected = lgp_bench_native::bench_main(static_cast<uint64_t>(k), iters); });
        const double lifted = best_ms(repeat, [&] { ran &= guest.run(static_cast<uint64_t>(k), iters, actual); });
        const bool ok = ran && actual == expected;
        if (!ok) ++failures;
        const double ratio = lifted / std::max(native, 1e-6);
        log_sum += std::log(ratio);
        ++count;
        if (csv)
            std::printf("%s,%" PRIu64 ",%.3f,%.3f,%.2f%s\n", kKernels[k].name, iters, native, lifted, ratio, ok ? "" : ",MISMATCH");
        else
            std::printf("%-10s %8" PRIu64 " %11.2f %11.2f %7.1fx%s\n", kKernels[k].name, iters, native, lifted, ratio,
                        ok ? "" : "  MISMATCH");
        if (!ok)
            std::printf("  checksum native 0x%016" PRIx64 ", lifted 0x%016" PRIx64 "\n", expected, actual);
    }
    if (count > 0 && !csv) std::printf("geometric mean ratio: %.2fx\n", std::exp(log_sum / count));
    return failures == 0 ? 0 : 1;
}
