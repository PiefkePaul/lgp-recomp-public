// Native probe (memprobe.hpp 6., docs/analysis/56 stage F): can this device
// run original code at a fixed guest address? No game code.
#include <sys/mman.h>
#include <unistd.h>

#include <csignal>

#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#if defined(__linux__)
#include <sys/syscall.h>
#endif
#if defined(__APPLE__)
#include <TargetConditionals.h>
#include <libkern/OSCacheControl.h>
#include <mach/mach.h>
#endif

#include "memprobe.hpp"

namespace {

constexpr uint64_t kPage = 16 * 1024;  // Apple arm64 pages; a multiple of the others
constexpr uint64_t kGiB = uint64_t{1} << 30;

// The code the probe maps to the guest address: alone in its own 16 KiB of
// the program's text, so the page holds nothing else and needs no relocation.
#if defined(__APPLE__)
#define LGP_PROBE_SECTION "__TEXT,__lgpprobe"
#else
#define LGP_PROBE_SECTION ".text.lgpprobe"
#endif
extern "C" __attribute__((noinline, used, aligned(16384), section(LGP_PROBE_SECTION))) int lgp_native_probe_fn(int x) {
    return x * 2 + 42;
}

std::string hex(uint64_t v) {
    char b[24];
    std::snprintf(b, sizeof b, "0x%" PRIx64, v);
    return b;
}

// Reserves [base, base + size) without replacing anything: a hint that the
// kernel either takes or moves elsewhere (then it is given back).
bool reserve_at(uint64_t base, uint64_t size) {
    void* p = mmap(reinterpret_cast<void*>(base), size, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED)
        return false;
    if (reinterpret_cast<uint64_t>(p) != base) {
        munmap(p, size);
        return false;
    }
    return true;
}

using ProbeFn = int (*)(int);

#if defined(__aarch64__) && defined(__linux__)
volatile sig_atomic_t g_signals = 0;
void on_probe_signal(int) { g_signals = g_signals + 1; }

// Guest code owns x18 between gates, while bionic may use it as the shadow
// call stack (ShadowCallStack). A signal in guest code runs the handler and
// the sigreturn path with the guest's x18: sends one to this thread with x18
// pointing at an unmapped address, straight from inline assembly so no libc
// code runs in between. A process that dies here has a shadow call stack in
// its signal path.
bool signal_with_foreign_x18() {
    struct sigaction sa {}, old {};
    sa.sa_handler = &on_probe_signal;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGUSR1, &sa, &old) != 0)
        return false;
    const long pid = getpid(), tid = syscall(SYS_gettid);
    const sig_atomic_t before = g_signals;
    asm volatile(
        "mov x9, x18\n"
        "mov x18, #0x10\n"
        "mov x0, %[pid]\n"
        "mov x1, %[tid]\n"
        "mov x2, %[sig]\n"
        "mov x8, %[nr]\n"
        "svc #0\n"
        "mov x18, x9\n"
        :
        : [pid] "r"(pid), [tid] "r"(tid), [sig] "r"(long{SIGUSR1}), [nr] "r"(long{SYS_tgkill})
        : "x0", "x1", "x2", "x8", "x9", "memory");
    sigaction(SIGUSR1, &old, nullptr);
    return g_signals != before;
}
#endif

int call_at(uint64_t va) { return reinterpret_cast<ProbeFn>(va)(7); }

}  // namespace

namespace lgp::memprobe {

int run_native_probe(const Options& options, const std::function<void(const std::string&)>& line) {
    line("native probe: " + host_description());
    const uint64_t span = (options.native_gib == 0 ? 8 : options.native_gib) * kGiB;
    // The console's addresses have 39 bits; the native start needs its base
    // below 2^39 with the whole guest space behind it (BootOptions::Native).
    static const uint64_t kBases[] = {0x70'0000'0000, 0x60'0000'0000, 0x40'0000'0000, 0x20'0000'0000,
                                      0x10'0000'0000, 0x8'0000'0000,  0x4'0000'0000,  0x2'0000'0000};
    uint64_t base = 0;
    int ok = 0;
    for (const uint64_t b : kBases) {
        if (b + span > (uint64_t{1} << 39))
            continue;
        const bool got = reserve_at(b, span);
        line("reserve " + std::to_string(span / kGiB) + " GiB at " + hex(b) + ": " + (got ? "ok" : "no"));
        if (got) {
            ++ok;
            if (base == 0)
                base = b;
            else
                munmap(reinterpret_cast<void*>(b), span);
        }
    }
    if (base == 0) {
        line("native probe: no base below 2^39 with " + std::to_string(span / kGiB) + " GiB, stop");
        return 0;
    }
    line("native probe: base " + hex(base));
    const uint64_t page = reinterpret_cast<uint64_t>(&lgp_native_probe_fn) & ~(kPage - 1);
    const uint64_t offset = reinterpret_cast<uint64_t>(&lgp_native_probe_fn) - page;
    const int expect = call_at(reinterpret_cast<uint64_t>(&lgp_native_probe_fn));
    uint64_t target = base + span / 2;

#if defined(__APPLE__)
    // iOS/tvOS (and macOS): the signed text page itself at the guest
    // address, shared with the program's own mapping (no copy, so its code
    // signature stays valid). A process the kernel kills for running
    // unsigned code ends here without another line.
    {
        vm_address_t at = static_cast<vm_address_t>(target);
        vm_prot_t cur = 0, max = 0;
        const kern_return_t kr = vm_remap(mach_task_self(), &at, kPage, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE,
                                          mach_task_self(), static_cast<vm_address_t>(page), FALSE, &cur, &max,
                                          VM_INHERIT_NONE);
        line("vm_remap of the signed text page to " + hex(target) + ": " +
             (kr == KERN_SUCCESS ? "ok, prot " + std::to_string(cur) + "/" + std::to_string(max)
                                 : "failed (" + std::to_string(kr) + ")"));
        if (kr == KERN_SUCCESS) {
            line("calling the remapped code ...");
            const int got = call_at(target + offset);
            line(std::string("remapped code: ") + (got == expect ? "runs, result ok" : "runs, WRONG result"));
            ++ok;
        }
        target += kPage;
    }
#endif

    // Android, Linux (and for comparison Apple): the page from a file,
    // mapped readable and executable at the guest address.
    {
        std::string dir = options.file_dir;
        if (dir.empty()) {
            const char* t = std::getenv("TMPDIR");
            dir = t != nullptr && *t != '\0' ? t : "/tmp";
        }
        std::string path = dir + "/lgp-native-probe-XXXXXX";
        const int fd = mkstemp(path.data());
        bool mapped = false;
        if (fd >= 0) {
            unlink(path.c_str());
            mapped = write(fd, reinterpret_cast<const void*>(page), kPage) == static_cast<ssize_t>(kPage) &&
                     mmap(reinterpret_cast<void*>(target), kPage, PROT_READ | PROT_EXEC, MAP_PRIVATE | MAP_FIXED, fd,
                          0) != MAP_FAILED;
            close(fd);
        }
        line("file page mapped executable at " + hex(target) + ": " +
             (mapped ? std::string("ok") : "failed (" + std::string(std::strerror(errno)) + ")"));
        if (mapped) {
            line("calling the file-mapped code ...");
            const int got = call_at(target + offset);
            line(std::string("file-mapped code: ") + (got == expect ? "runs, result ok" : "runs, WRONG result"));
            ++ok;
        }
        target += kPage;
    }

    // Anonymous memory made executable (what the native start does on the
    // Mac today): expected to fail where the platform has no JIT.
    {
        void* p = mmap(reinterpret_cast<void*>(target), kPage, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED,
                       -1, 0);
        const bool rw = p != MAP_FAILED;
        if (rw)
            std::memcpy(p, reinterpret_cast<const void*>(page), kPage);
        const bool rx = rw && mprotect(p, kPage, PROT_READ | PROT_EXEC) == 0;
        line("anonymous page made executable at " + hex(target) + ": " +
             (rx ? std::string("ok") : "refused (" + std::string(std::strerror(errno)) + ")"));
        if (rx) {
#if defined(__APPLE__)
            sys_icache_invalidate(p, kPage);
#else
            __builtin___clear_cache(static_cast<char*>(p), static_cast<char*>(p) + kPage);
#endif
            line("calling the copied code ...");
            const int got = call_at(target + offset);
            line(std::string("copied code: ") + (got == expect ? "runs, result ok" : "runs, WRONG result"));
            ++ok;
        }
    }
#if defined(__aarch64__) && defined(__linux__)
    line("signal with a foreign x18 ...");
    line(std::string("signal with a foreign x18: ") + (signal_with_foreign_x18() ? "handled, process alive" : "NOT handled"));
#endif
    munmap(reinterpret_cast<void*>(base), span);
    line("native probe: done");
    return ok;
}

}  // namespace lgp::memprobe
