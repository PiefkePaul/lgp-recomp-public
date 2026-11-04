#include "lgp/platform/host_block.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(__EMSCRIPTEN__)
// plain allocation, see below
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <cstdio>
#include <csignal>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/statvfs.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <TargetConditionals.h>
#include <libkern/OSCacheControl.h>
#endif
#endif

namespace lgp::platform {

namespace {
// HostBlock::op_totals(): process-wide, relaxed (statistics only).
struct OpCounter {
    std::atomic<uint64_t> calls{0}, ns{0}, bytes{0};
};
OpCounter g_ops[size_t(HostBlock::Op::Count)];

[[maybe_unused]] uint64_t now_ns() {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count());
}

// Adds one call of `op` over `bytes` that started at `start` (now_ns()).
[[maybe_unused]] void count_op(HostBlock::Op op, uint64_t start, uint64_t bytes) {
    OpCounter& c = g_ops[size_t(op)];
    c.calls.fetch_add(1, std::memory_order_relaxed);
    c.ns.fetch_add(now_ns() - start, std::memory_order_relaxed);
    c.bytes.fetch_add(bytes, std::memory_order_relaxed);
}
}  // namespace

HostBlock::OpTotals HostBlock::op_totals(Op op) {
    const OpCounter& c = g_ops[size_t(op)];
    return {c.calls.load(std::memory_order_relaxed), c.ns.load(std::memory_order_relaxed),
            c.bytes.load(std::memory_order_relaxed)};
}

const char* HostBlock::op_name(Op op) {
    switch (op) {
    case Op::Commit: return "commit";
    case Op::Release: return "release";
    case Op::Punch: return "punch";
    case Op::Zero: return "zero";
    case Op::Count: break;
    }
    return "?";
}

#if !defined(__EMSCRIPTEN__)
namespace {
// Reserve the whole 32-bit offset range (see header).
constexpr uint64_t kReserve64 = uint64_t{1} << 32;
}  // namespace
#endif

#if defined(__EMSCRIPTEN__)

HostBlock::HostBlock(uint64_t size, uint64_t at) : size_(size), reserved_(size) {
    if (at != 0)
        throw std::runtime_error("guest memory at a fixed host address is not available in the browser");
    // Memory that the Wasm instance has never handed out is zero (memory.grow
    // zero-fills). Only the part of the allocation below the old top of linear
    // memory can hold stale bytes, so only that part is cleared: clearing
    // gigabytes would commit them.
    const uintptr_t old_top = static_cast<uintptr_t>(__builtin_wasm_memory_size(0)) * 65536u;
    void* p = std::aligned_alloc(4096, static_cast<size_t>(size));
    if (p == nullptr)
        throw std::bad_alloc();
    base_ = static_cast<uint8_t*>(p);
    const uintptr_t start = reinterpret_cast<uintptr_t>(p);
    if (start < old_top)
        std::memset(p, 0, static_cast<size_t>(std::min<uint64_t>(size, old_top - start)));
}

HostBlock::~HostBlock() { std::free(base_); }

void HostBlock::commit_zeroed(uint64_t, uint64_t) {}

void HostBlock::decommit(uint64_t offset, uint64_t len) { std::memset(base_ + offset, 0, static_cast<size_t>(len)); }

void HostBlock::make_executable(uint64_t, uint64_t) {
    throw std::runtime_error("no native code execution in the browser");
}

void HostBlock::map_code_file(uint64_t, uint64_t, const std::string&, uint64_t) {
    throw std::runtime_error("no native code execution in the browser");
}

bool HostBlock::resident(uint64_t*) const { return false; }

bool HostBlock::region_stats(uint64_t, std::vector<Region>*) const { return false; }

bool HostBlock::dirty_known() { return false; }

bool HostBlock::extend_file_window(uint64_t, MoveStats*, std::string* error) {
    *error = "no file-backed guest memory on this platform";
    return false;
}

#elif defined(_WIN32)

HostBlock::HostBlock(uint64_t size, uint64_t at) : size_(size), reserved_(size < kReserve64 ? kReserve64 : size) {
    void* p = VirtualAlloc(reinterpret_cast<void*>(static_cast<uintptr_t>(at)), static_cast<SIZE_T>(reserved_),
                           MEM_RESERVE, PAGE_NOACCESS);
    if (p == nullptr)
        throw std::runtime_error("VirtualAlloc(MEM_RESERVE) failed: " + std::to_string(GetLastError()));
    base_ = static_cast<uint8_t*>(p);
}

HostBlock::~HostBlock() { VirtualFree(base_, 0, MEM_RELEASE); }

void HostBlock::commit_zeroed(uint64_t offset, uint64_t len) {
    if (VirtualAlloc(base_ + offset, static_cast<SIZE_T>(len), MEM_COMMIT, PAGE_READWRITE) == nullptr)
        throw std::runtime_error("VirtualAlloc(MEM_COMMIT) failed: " + std::to_string(GetLastError()));
}

void HostBlock::decommit(uint64_t offset, uint64_t len) {
    VirtualFree(base_ + offset, static_cast<SIZE_T>(len), MEM_DECOMMIT);
}

void HostBlock::make_executable(uint64_t offset, uint64_t len) {
    DWORD old = 0;
    if (!VirtualProtect(base_ + offset, static_cast<SIZE_T>(len), PAGE_EXECUTE_READ, &old))
        throw std::runtime_error("VirtualProtect(PAGE_EXECUTE_READ) failed: " + std::to_string(GetLastError()));
    FlushInstructionCache(GetCurrentProcess(), base_ + offset, static_cast<SIZE_T>(len));
}

void HostBlock::map_code_file(uint64_t, uint64_t, const std::string&, uint64_t) {
    // The block is one reservation; a file view cannot replace part of it.
    throw std::runtime_error("no code image mapping on Windows");
}

bool HostBlock::resident(uint64_t*) const { return false; }

bool HostBlock::region_stats(uint64_t, std::vector<Region>*) const { return false; }

bool HostBlock::dirty_known() { return false; }

bool HostBlock::extend_file_window(uint64_t, MoveStats*, std::string* error) {
    *error = "no file-backed guest memory on this platform";
    return false;
}

#else  // POSIX

#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0
#endif

namespace {
// LGP_HOST_BLOCK_FILE: "" = off, "1" = the temporary folder, else a folder.
std::string backing_dir() {
    const char* v = std::getenv("LGP_HOST_BLOCK_FILE");
#if (defined(__APPLE__) && TARGET_OS_TV) || defined(LGP_HOST_BLOCK_FILE_DEFAULT)
    // tvOS: on unless set to "0". Without it LGP dies by jetsam at ≈ 2 GiB
    // footprint on reaching the first in-game scene; with it the Apple TV 4K
    // ran the Pikachu route to frame 10500 (peak 1813 MiB, Analyse 28).
    // Elsewhere only with -DLGP_HOST_BLOCK_FILE_DEFAULT=ON (iOS: under test).
    if (v == nullptr || *v == '\0')
        v = "1";
#endif
#if defined(__ANDROID__)
    // Android devices with less than 3.5 GiB RAM (NVIDIA SHIELD: 2.9 GiB):
    // on unless set to "0". There the anonymous guest block (~965 MiB
    // resident) got the app killed before the Pikachu route's first scene;
    // in the file the kernel can write it back and drop it, and the run
    // went on to frame 1500 with 132 MiB resident (Analyse 43, arm C).
    // Phones with 6 GiB and more keep the anonymous block (no measured gain).
    constexpr uint64_t kLowMemory = uint64_t{7} << 29;  // 3.5 GiB
    if (v == nullptr || *v == '\0') {
        const long pages = sysconf(_SC_PHYS_PAGES);
        const long page = sysconf(_SC_PAGESIZE);
        const uint64_t ram = pages > 0 && page > 0 ? static_cast<uint64_t>(pages) * static_cast<uint64_t>(page) : 0;
        if (ram != 0 && ram < kLowMemory) {
            v = "1";
            std::fprintf(stderr,
                         "host block: %llu MiB RAM (below 3584): guest memory on a file by default, "
                         "LGP_HOST_BLOCK_FILE=0 turns it off\n",
                         (unsigned long long)(ram >> 20));
        }
    }
#endif
    if (v == nullptr || *v == '\0' || std::strcmp(v, "0") == 0)
        return {};
    if (std::strcmp(v, "1") != 0)
        return v;
    const char* tmp = std::getenv("TMPDIR");
    return tmp != nullptr && *tmp != '\0' ? tmp : "/tmp";
}

// LGP_HOST_BLOCK_FILE_RANGE=A:B, MiB offsets into the block; a missing end
// means the start or the end of the block. Clamped to the block and aligned
// to host pages; [0, size) without the variable or when it does not parse.
// Without the variable the build's default applies (CMake
// LGP_HOST_BLOCK_FILE_RANGE_DEFAULT; on tvOS "768:" unless that says
// otherwise, docs/analysis/42); "0:" puts the whole block in the file.
#if !defined(LGP_HOST_BLOCK_FILE_RANGE_DEFAULT) && defined(__APPLE__) && TARGET_OS_TV
#define LGP_HOST_BLOCK_FILE_RANGE_DEFAULT "768:"
#endif
void file_window(uint64_t size, uint64_t page, uint64_t* begin, uint64_t* end) {
    *begin = 0;
    // Rounded up, as the mapping of the whole block was before the window.
    *end = (size + page - 1) & ~(page - 1);
    const char* v = std::getenv("LGP_HOST_BLOCK_FILE_RANGE");
#if defined(LGP_HOST_BLOCK_FILE_RANGE_DEFAULT)
    if (v == nullptr || *v == '\0')
        v = LGP_HOST_BLOCK_FILE_RANGE_DEFAULT;
#endif
    if (v == nullptr || *v == '\0')
        return;
    // A:B in MiB, either end may be left out. Anything else keeps the whole
    // block in the file and says so.
    const auto mib = [](const char* text, const char* stop_at, uint64_t* out) {
        if (*text < '0' || *text > '9')
            return false;
        char* stop = nullptr;
        errno = 0;
        const unsigned long long n = std::strtoull(text, &stop, 10);
        if (errno != 0 || stop != stop_at || n > (UINT64_MAX >> 20))
            return false;
        *out = n << 20;
        return true;
    };
    const char* colon = std::strchr(v, ':');
    uint64_t a = 0, b = *end;
    if (colon == nullptr || (colon != v && !mib(v, colon, &a)) ||
        (colon[1] != '\0' && !mib(colon + 1, colon + std::strlen(colon), &b))) {
        std::fprintf(stderr, "host block: LGP_HOST_BLOCK_FILE_RANGE=%s is not A:B in MiB, whole block in the file\n", v);
        return;
    }
    *begin = std::min<uint64_t>(a, *end) & ~(page - 1);
    *end = std::min<uint64_t>(b, *end) & ~(page - 1);
    if (*end < *begin)
        *end = *begin;
}

// An unlinked sparse file of `size` bytes in `dir`; -1 and *error on failure.
int open_backing_file(const std::string& dir, uint64_t size, std::string* error) {
    std::string path = dir;
    if (!path.empty() && path.back() != '/')
        path += '/';
    path += "lgp-guest-XXXXXX";
    int fd = mkstemp(path.data());
    if (fd < 0) {
        *error = "cannot create " + path + ": " + std::strerror(errno);
        return -1;
    }
    unlink(path.c_str());  // nothing stays behind, whichever way the process ends
    if (ftruncate(fd, static_cast<off_t>(size)) != 0) {
        *error = std::string("ftruncate: ") + std::strerror(errno);
        close(fd);
        return -1;
    }
    return fd;
}

// Releases the file's storage for [offset, offset+len) (4 KiB aligned);
// false where the file system or platform cannot.
bool punch_hole(int fd, uint64_t offset, uint64_t len) {
#if defined(__linux__) && defined(FALLOC_FL_PUNCH_HOLE)
    return fallocate(fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, static_cast<off_t>(offset),
                     static_cast<off_t>(len)) == 0;
#elif defined(__APPLE__) && defined(F_PUNCHHOLE)
    fpunchhole_t hole{};
    hole.fp_offset = static_cast<off_t>(offset);
    hole.fp_length = static_cast<off_t>(len);
    return fcntl(fd, F_PUNCHHOLE, &hole) == 0;
#else
    (void)fd, (void)offset, (void)len;
    return false;
#endif
}

// Whether a hole punched under a written MAP_SHARED page makes the page read
// zero (Linux drops it from the page cache; checked rather than assumed for
// Darwin). Then decommit() need not write every page it releases. Uses the
// host page at `offset` (the start of the file window), which is zero and
// PROT_NONE again afterwards.
bool punch_zeroes_mapped(int fd, uint8_t* base, uint64_t offset) {
    const uint64_t page = static_cast<uint64_t>(getpagesize());
    if (mprotect(base + offset, page, PROT_READ | PROT_WRITE) != 0)
        return false;
    volatile uint8_t* p = base + offset;
    p[0] = 0xA5;
    p[page - 1] = 0x5A;
    const bool zero = punch_hole(fd, offset, page) && p[0] == 0 && p[page - 1] == 0;
    if (!zero)
        std::memset(base + offset, 0, static_cast<size_t>(page));
    mprotect(base + offset, page, PROT_NONE);
    return zero;
}

// Below this much free storage the file is not used: the first write to a
// page that has no block yet would end the app with SIGBUS.
constexpr uint64_t kMinFreeStorage = uint64_t{256} << 20;

// Free bytes for an unprivileged writer on the file system of `dir`; false
// where statvfs fails.
bool free_storage(const std::string& dir, uint64_t* bytes) {
    struct statvfs st{};
    if (statvfs(dir.c_str(), &st) != 0)
        return false;
    *bytes = uint64_t(st.f_bavail) * uint64_t(st.f_frsize != 0 ? st.f_frsize : st.f_bsize);
    return true;
}

// SIGBUS inside the file window: the storage behind the guest memory file is
// full (a write to a hole found no free block). Says so on stderr and lets
// the signal end the process as before (async-signal-safe: write(2) only).
uintptr_t g_window_begin = 0, g_window_end = 0;
struct sigaction g_previous_bus{};

void on_bus(int sig, siginfo_t* info, void* context) {
    const uintptr_t at = reinterpret_cast<uintptr_t>(info->si_addr);
    if (at >= g_window_begin && at < g_window_end) {
        static const char kMessage[] =
            "host block: SIGBUS in the guest memory file: its storage is full; free space or set "
            "LGP_HOST_BLOCK_FILE=0\n";
        ssize_t ignored = write(2, kMessage, sizeof(kMessage) - 1);
        (void)ignored;
    }
    // Hand over to whoever was there before, else the default action.
    if ((g_previous_bus.sa_flags & SA_SIGINFO) != 0 && g_previous_bus.sa_sigaction != nullptr) {
        g_previous_bus.sa_sigaction(sig, info, context);
        return;
    }
    if (g_previous_bus.sa_handler != SIG_DFL && g_previous_bus.sa_handler != SIG_IGN &&
        g_previous_bus.sa_handler != nullptr) {
        g_previous_bus.sa_handler(sig);
        return;
    }
    signal(sig, SIG_DFL);  // returning re-runs the access and ends the process
}

void watch_bus(uint8_t* begin, uint64_t len) {
    static bool installed = false;
    g_window_begin = reinterpret_cast<uintptr_t>(begin);
    g_window_end = g_window_begin + len;
    if (installed)
        return;
    struct sigaction sa{};
    sa.sa_sigaction = on_bus;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    installed = sigaction(SIGBUS, &sa, &g_previous_bus) == 0;
}
}  // namespace

HostBlock::HostBlock(uint64_t size, uint64_t at) : size_(size), reserved_(size < kReserve64 ? kReserve64 : size) {
    int flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE;
#if defined(MAP_FIXED_NOREPLACE)
    if (at != 0)
        flags |= MAP_FIXED_NOREPLACE;
#endif
    void* p = mmap(reinterpret_cast<void*>(static_cast<uintptr_t>(at)), reserved_, PROT_NONE, flags, -1, 0);
    if (p == MAP_FAILED)
        throw std::runtime_error("mmap reservation of the guest address space failed");
    if (at != 0 && reinterpret_cast<uintptr_t>(p) != at) {
        // Without MAP_FIXED_NOREPLACE the address is only a hint.
        munmap(p, reserved_);
        throw std::runtime_error("the guest address space cannot be placed at the requested host address");
    }
    base_ = static_cast<uint8_t*>(p);
    const uint64_t pages = reserved_ / static_cast<uint64_t>(getpagesize());
    committed_.assign(static_cast<size_t>((pages + 63) / 64), 0);
    if (const std::string dir = backing_dir(); !dir.empty()) {
        std::string error;
        file_window(size_, static_cast<uint64_t>(getpagesize()), &file_begin_, &file_end_);
        const uint64_t window = file_end_ - file_begin_;
        // The file takes storage as the guest first writes its pages; with
        // the storage full that write ends the app (SIGBUS). Nearly full:
        // anonymous memory; less free than the window: a warning.
        uint64_t free = 0;
        const bool free_known = window > 0 && free_storage(dir, &free);
        const bool too_full = free_known && free < kMinFreeStorage;
        const int fd = window > 0 && !too_full ? open_backing_file(dir, size_, &error) : -1;
        if (window == 0)
            error = "LGP_HOST_BLOCK_FILE_RANGE is empty";
        else if (too_full)
            error = "only " + std::to_string(free >> 20) + " MiB free in " + dir;
        else if (free_known && free < window)
            std::fprintf(stderr,
                         "host block: %llu MiB free in %s for a %llu MiB window; if the guest writes more, "
                         "the app ends (SIGBUS)\n",
                         (unsigned long long)(free >> 20), dir.c_str(), (unsigned long long)(window >> 20));
        // The file replaces the window of the reservation, at the same offset
        // in the file; the rest stays PROT_NONE as before.
        if (fd >= 0 && mmap(base_ + file_begin_, window, PROT_NONE, MAP_SHARED | MAP_FIXED, fd,
                            static_cast<off_t>(file_begin_)) == MAP_FAILED) {
            error = std::string("mmap(MAP_SHARED): ") + std::strerror(errno);
            close(fd);
            if (mmap(base_ + file_begin_, window, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED,
                     -1, 0) == MAP_FAILED) {
                munmap(base_, reserved_);
                throw std::runtime_error("mmap reservation of the guest address space failed");
            }
        } else if (fd >= 0) {
            fd_ = fd;
            // Before the probe below writes the first page of the file.
            watch_bus(base_ + file_begin_, window);
            // LGP_HOST_BLOCK_PUNCH=0: zero released pages and keep the file's
            // storage (diagnosis of the load stall, docs/analysis/42).
            const char* punch = std::getenv("LGP_HOST_BLOCK_PUNCH");
            punch_ = punch == nullptr || std::strcmp(punch, "0") != 0;
            punch_zeroes_ = punch_ && punch_zeroes_mapped(fd_, base_, file_begin_);
        }
        if (fd_ >= 0)
            std::fprintf(stderr,
                         "host block: guest memory backed by a file in %s, MiB %llu-%llu of %llu, "
                         "released pages %s\n",
                         dir.c_str(), (unsigned long long)(file_begin_ >> 20), (unsigned long long)(file_end_ >> 20),
                         (unsigned long long)(size_ >> 20),
                         punch_zeroes_ ? "punched"
                         : punch_      ? "zeroed (written back)"
                                       : "zeroed, not punched (LGP_HOST_BLOCK_PUNCH=0)");
        else
            std::fprintf(stderr, "host block: LGP_HOST_BLOCK_FILE ignored (%s), anonymous memory\n", error.c_str());
    }
}

HostBlock::~HostBlock() {
    munmap(base_, reserved_);
    if (fd_ >= 0)
        close(fd_);
}

namespace {
// Guest blocks are 4 KiB granular, host pages may be larger (16 KiB on macOS
// arm64), and mprotect/mmap need host-page-aligned ranges.
uint64_t host_page() {
    static const uint64_t page = static_cast<uint64_t>(getpagesize());
    return page;
}

// Sets or clears the bits of host pages [first, last) in `bits`.
void mark_pages(std::vector<uint64_t>& bits, uint64_t first, uint64_t last, bool on) {
    for (uint64_t i = first; i < last; ++i) {
        const uint64_t bit = uint64_t{1} << (i & 63);
        if (on)
            bits[size_t(i >> 6)] |= bit;
        else
            bits[size_t(i >> 6)] &= ~bit;
    }
}

bool page_marked(const std::vector<uint64_t>& bits, uint64_t i) {
    return (bits[size_t(i >> 6)] >> (i & 63)) & 1;
}

bool page_is_zero(const uint8_t* p, uint64_t len) {
    const uint64_t* w = reinterpret_cast<const uint64_t*>(p);
    for (uint64_t i = 0; i < len / 8; ++i)
        if (w[i] != 0)
            return false;
    return true;
}

// Writes all of [data, data+len) at `offset`; false and errno on failure.
bool pwrite_all(int fd, const uint8_t* data, uint64_t len, uint64_t offset) {
    while (len > 0) {
        const ssize_t n = pwrite(fd, data, static_cast<size_t>(len), static_cast<off_t>(offset));
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return false;
        data += n, len -= uint64_t(n), offset += uint64_t(n);
    }
    return true;
}
}  // namespace

void HostBlock::commit_zeroed(uint64_t offset, uint64_t len) {
    // Pages of the reservation are zero until written, and decommit() zeroes
    // or replaces written pages, so committing is only a protection change. A
    // host page shared with a neighbouring block may already be committed;
    // widening the range to whole host pages keeps its contents.
    const uint64_t t0 = now_ns();
    const uint64_t page = host_page();
    const uint64_t start = offset & ~(page - 1);
    const uint64_t end = (offset + len + page - 1) & ~(page - 1);
    if (mprotect(base_ + start, end - start, PROT_READ | PROT_WRITE) != 0)
        throw std::runtime_error("mprotect(commit) failed");
    mark_pages(committed_, start / page, end / page, true);
    count_op(Op::Commit, t0, len);
}

void HostBlock::make_executable(uint64_t offset, uint64_t len) {
    const uint64_t page = host_page();
    const uint64_t start = offset & ~(page - 1);
    const uint64_t end = (offset + len + page - 1) & ~(page - 1);
    if (mprotect(base_ + start, end - start, PROT_READ | PROT_EXEC) != 0)
        throw std::runtime_error(std::string("mprotect(PROT_EXEC) of guest code failed: ") + std::strerror(errno));
#if defined(__APPLE__)
    // iOS and tvOS do not export __clear_cache (the builtin's fallback).
    sys_icache_invalidate(base_ + start, end - start);
#else
    __builtin___clear_cache(reinterpret_cast<char*>(base_ + start), reinterpret_cast<char*>(base_ + end));
#endif
}

void HostBlock::map_code_file(uint64_t offset, uint64_t len, const std::string& path, uint64_t file_offset) {
    const uint64_t page = host_page();
    if (offset % page != 0 || len % page != 0 || file_offset % page != 0 || offset + len > size_)
        throw std::runtime_error("code image range not aligned to host pages or outside the block");
    if (fd_ >= 0 && offset < file_end_ && offset + len > file_begin_)
        throw std::runtime_error("code image range inside the file-backed window of guest memory");
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        throw std::runtime_error("open " + path + ": " + std::strerror(errno));
    void* p = mmap(base_ + offset, static_cast<size_t>(len), PROT_READ | PROT_EXEC, MAP_PRIVATE | MAP_FIXED, fd,
                   static_cast<off_t>(file_offset));
    const int err = errno;
    close(fd);
    if (p == MAP_FAILED)
        throw std::runtime_error(std::string("mmap(PROT_EXEC) of the code image failed: ") + std::strerror(err));
#if defined(__APPLE__)
    sys_icache_invalidate(base_ + offset, len);
#else
    __builtin___clear_cache(reinterpret_cast<char*>(base_ + offset), reinterpret_cast<char*>(base_ + offset + len));
#endif
}

void HostBlock::decommit(uint64_t offset, uint64_t len) {
    // Host pages wholly inside the range: a fresh anonymous mapping drops the
    // old pages (zero on next use) on Linux and macOS alike; MADV_DONTNEED
    // would only zero on Linux. Partial host pages at the edges may still back
    // a neighbouring block, so they stay committed and are zeroed instead.
    const uint64_t t0 = now_ns();
    struct Done {
        uint64_t t0, len;
        ~Done() { count_op(Op::Release, t0, len); }
    } done{t0, len};
    const uint64_t page = host_page();
    const uint64_t end = offset + len;
    const uint64_t inner_start = (offset + page - 1) & ~(page - 1);
    const uint64_t inner_end = end & ~(page - 1);
    if (inner_start >= inner_end) {
        std::memset(base_ + offset, 0, static_cast<size_t>(len));
        return;
    }
    std::memset(base_ + offset, 0, static_cast<size_t>(inner_start - offset));
    std::memset(base_ + inner_end, 0, static_cast<size_t>(end - inner_end));
    mark_pages(committed_, inner_start / page, inner_end / page, false);
    const auto release_anonymous = [&](uint64_t a, uint64_t b) {
        if (a >= b)
            return;
        void* p = mmap(base_ + a, b - a, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1, 0);
        if (p == MAP_FAILED)
            throw std::runtime_error("mmap(decommit) failed");
    };
    if (fd_ < 0) {
        release_anonymous(inner_start, inner_end);
        return;
    }
    // File mode: a fresh anonymous mapping would cut the file out of the
    // range. Punching a hole releases the storage and, where it zeroes the
    // mapped pages (punch_zeroes_), spares writing each page, which would
    // read it in and dirty it (the write-back this mode avoids). Elsewhere
    // zeroing keeps it correct. Outside the file window as without a file.
    const uint64_t fa = std::clamp(file_begin_, inner_start, inner_end);
    const uint64_t fb = std::clamp(file_end_, inner_start, inner_end);
    release_anonymous(inner_start, fa);
    if (fa < fb) {
        const uint64_t inner = fb - fa;
        bool punched = false;
        if (punch_zeroes_) {
            const uint64_t tp = now_ns();
            punched = punch_hole(fd_, fa, inner);
            count_op(Op::Punch, tp, inner);
        }
        if (!punched) {
            const uint64_t tz = now_ns();
            std::memset(base_ + fa, 0, static_cast<size_t>(inner));
            count_op(Op::Zero, tz, inner);
            if (punch_) {
                const uint64_t tp = now_ns();
                punch_hole(fd_, fa, inner);  // only a saving
                count_op(Op::Punch, tp, inner);
            }
        }
        // Like the anonymous path: a guest access after unmap faults.
        if (mprotect(base_ + fa, inner, PROT_NONE) != 0)
            throw std::runtime_error("mprotect(decommit) failed");
    }
    release_anonymous(fb, inner_end);
}

bool HostBlock::extend_file_window(uint64_t begin, MoveStats* stats, std::string* error) {
    const uint64_t t0 = now_ns();
    *stats = {};
    const uint64_t page = host_page();
    begin &= ~(page - 1);
    if (fd_ < 0) {
        *error = "guest memory is not file-backed";
        return false;
    }
    if (begin >= file_begin_) {
        *error = "the file window already starts at MiB " + std::to_string(file_begin_ >> 20);
        return false;
    }
    // In chunks from the window down, the window growing with each: reading
    // a committed page the guest never wrote can give it a real zero page
    // (Darwin), which the mapping of its chunk drops again at once. A failure
    // leaves a smaller, still contiguous window.
    constexpr uint64_t kChunk = uint64_t{16} << 20;
    bool ok = true;
    while (file_begin_ > begin) {
        const uint64_t lo = file_begin_ - std::min(kChunk, file_begin_ - begin);
        const uint64_t first = lo / page, last = file_begin_ / page;
        // 1. Copy the chunk into the file, at the same offsets. Zero pages
        //    stay holes (they read zero from the file too); pages that are not
        //    committed are PROT_NONE and zero by contract.
        for (uint64_t i = first; i < last && ok;) {
            if (!page_marked(committed_, i)) {
                ++i;
                continue;
            }
            stats->committed += page;
            if (page_is_zero(base_ + i * page, page)) {
                ++i;
                continue;
            }
            // One write per run of non-zero committed pages.
            uint64_t j = i + 1;
            while (j < last && page_marked(committed_, j) && !page_is_zero(base_ + j * page, page)) {
                stats->committed += page;
                ++j;
            }
            if (!pwrite_all(fd_, base_ + i * page, (j - i) * page, i * page)) {
                *error = std::string("pwrite: ") + std::strerror(errno);
                punch_hole(fd_, lo, file_begin_ - lo);  // only a saving: the chunk stays anonymous
                ok = false;
                break;
            }
            stats->written += (j - i) * page;
            i = j;
        }
        if (!ok)
            break;
        // 2. The file replaces the chunk's anonymous pages; the committed ones
        //    read and write again. A failed mapping loses the chunk: no way back.
        if (mmap(base_ + lo, file_begin_ - lo, PROT_NONE, MAP_SHARED | MAP_FIXED, fd_, static_cast<off_t>(lo)) ==
            MAP_FAILED)
            throw std::runtime_error(std::string("mmap(MAP_SHARED) of moved guest memory failed: ") +
                                     std::strerror(errno));
        for (uint64_t i = first; i < last;) {
            if (!page_marked(committed_, i)) {
                ++i;
                continue;
            }
            uint64_t j = i + 1;
            while (j < last && page_marked(committed_, j))
                ++j;
            if (mprotect(base_ + i * page, (j - i) * page, PROT_READ | PROT_WRITE) != 0)
                throw std::runtime_error("mprotect(commit) of moved guest memory failed");
            i = j;
        }
        file_begin_ = lo;
    }
    watch_bus(base_ + file_begin_, file_end_ - file_begin_);
    stats->ns = now_ns() - t0;
    return ok;
}

bool HostBlock::resident(uint64_t* bytes) const {
    const uint64_t page = host_page();
#if defined(__APPLE__)
    using Vec = char;  // mincore(caddr_t, size_t, char*)
#else
    using Vec = unsigned char;
#endif
    std::vector<Vec> vec(static_cast<size_t>((reserved_ + page - 1) / page));
    if (mincore(reinterpret_cast<decltype(base_)>(base_), static_cast<size_t>(reserved_), vec.data()) != 0)
        return false;
    uint64_t pages = 0;
    for (const Vec v : vec)
        pages += (v & 1) != 0;  // MINCORE_INCORE / bit 0 on Linux
    *bytes = pages * page;
    return true;
}

bool HostBlock::region_stats(uint64_t bucket, std::vector<Region>* out) const {
    const uint64_t page = host_page();
    if (bucket < page)
        bucket = page;
#if defined(__APPLE__)
    using Vec = char;
#else
    using Vec = unsigned char;
#endif
    std::vector<Vec> vec(static_cast<size_t>((size_ + page - 1) / page));
    if (mincore(reinterpret_cast<decltype(base_)>(base_), static_cast<size_t>(size_), vec.data()) != 0)
        return false;
    out->assign(static_cast<size_t>((size_ + bucket - 1) / bucket), Region{});
#if defined(__APPLE__) && defined(MINCORE_MODIFIED)
#if defined(MINCORE_MODIFIED_OTHER)
    constexpr int kModified = MINCORE_MODIFIED | MINCORE_MODIFIED_OTHER;
#else
    constexpr int kModified = MINCORE_MODIFIED;
#endif
#endif
    for (size_t i = 0; i < vec.size(); ++i) {
        Region& r = (*out)[static_cast<size_t>(uint64_t(i) * page / bucket)];
        if ((vec[i] & 1) != 0)
            r.resident += page;
#if defined(__APPLE__) && defined(MINCORE_MODIFIED)
        // Only inside the file window: anonymous pages count as modified
        // once written and have nothing to write back.
        const uint64_t at = uint64_t(i) * page;
        if (fd_ >= 0 && at >= file_begin_ && at < file_end_ && (vec[i] & kModified) != 0)
            r.dirty += page;
#endif
    }
    return true;
}

bool HostBlock::dirty_known() {
#if defined(__APPLE__) && defined(MINCORE_MODIFIED)
    return true;
#else
    return false;
#endif
}

#endif

}  // namespace lgp::platform
