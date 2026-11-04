#include "memprobe.hpp"

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <memory>
#include <thread>
#include <vector>

#include "lgp/platform/host_block.hpp"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#include <mach/mach.h>
#include <sys/sysctl.h>
#if TARGET_OS_IPHONE
#include <os/proc.h>
#endif
#endif
#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/utsname.h>
#include <unistd.h>
#endif

namespace lgp::memprobe {

namespace {

constexpr uint64_t kMiB = 1024 * 1024;

#if defined(__GNUC__)
__attribute__((format(printf, 1, 2)))
#endif
std::string format(const char* fmt, ...);
std::string format(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return buf;
}

#if defined(__linux__)
// "Key:   123 kB" from a /proc file, in bytes.
uint64_t proc_kb(const char* file, const char* key) {
    std::ifstream in(file);
    const size_t n = std::strlen(key);
    for (std::string l; std::getline(in, l);)
        if (l.compare(0, n, key) == 0)
            return std::strtoull(l.c_str() + n, nullptr, 10) * 1024;
    return 0;
}
#endif

// xorshift64: cheap, incompressible enough for the memory compressor.
void fill(uint8_t* p, uint64_t len, uint64_t* state, bool zero) {
    if (zero) {
        std::memset(p, 0, len);
        return;
    }
    uint64_t x = *state;
    auto* w = reinterpret_cast<uint64_t*>(p);
    for (uint64_t i = 0; i < len / 8; ++i) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        w[i] = x;
    }
    *state = x;
}

// A plain reservation of `bytes` without access (address space only).
bool try_reserve(uint64_t bytes) {
#if defined(_WIN32)
    (void)bytes;
    return false;
#else
#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0
#endif
    void* p = mmap(nullptr, bytes, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED)
        return false;
    munmap(p, bytes);
    return true;
#endif
}

// The guest block as a shared mapping of a sparse file (Options::file_dir).
// The file is unlinked right away: nothing is left behind when the system
// kills the probe, and the mapping keeps the storage until the process ends.
class FileBlock {
public:
    FileBlock(const std::string& dir, uint64_t size, std::string* error) : size_(size) {
#if defined(_WIN32)
        (void)dir;
        *error = "not supported on Windows";
#else
        const std::string path = dir + "/memprobe-guest.bin";
        fd_ = open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0600);
        if (fd_ < 0) {
            *error = "cannot create " + path + ": " + std::strerror(errno);
            return;
        }
        unlink(path.c_str());
        if (ftruncate(fd_, static_cast<off_t>(size)) != 0) {
            *error = std::string("ftruncate: ") + std::strerror(errno);
            return;
        }
        void* p = mmap(nullptr, static_cast<size_t>(size), PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
        if (p == MAP_FAILED) {
            *error = std::string("mmap(MAP_SHARED): ") + std::strerror(errno);
            return;
        }
        base_ = static_cast<uint8_t*>(p);
#endif
    }
    ~FileBlock() {
#if !defined(_WIN32)
        if (base_ != nullptr)
            munmap(base_, static_cast<size_t>(size_));
        if (fd_ >= 0)
            close(fd_);
#endif
    }
    FileBlock(const FileBlock&) = delete;
    FileBlock& operator=(const FileBlock&) = delete;
    uint8_t* base() const { return base_; }
    void sync(uint64_t off, uint64_t len) {
#if !defined(_WIN32)
        msync(base_ + off, static_cast<size_t>(len), MS_ASYNC);
#else
        (void)off;
        (void)len;
#endif
    }

private:
    uint8_t* base_ = nullptr;
    uint64_t size_ = 0;
    int fd_ = -1;
};

double seconds_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

// One variant of the first-touch timing: maps `bytes` (anonymous when fd < 0,
// else MAP_SHARED on fd), clears it chunk by chunk and reports the rate, the
// slowest chunk and a second pass over the same pages.
void touch_variant(const char* name, int fd, uint64_t bytes, uint64_t chunk, double prepare_s,
                   const std::function<void(const std::string&)>& line) {
#if defined(_WIN32)
    (void)name, (void)fd, (void)bytes, (void)chunk, (void)prepare_s, (void)line;
#else
    void* p = fd < 0 ? mmap(nullptr, static_cast<size_t>(bytes), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0)
                     : mmap(nullptr, static_cast<size_t>(bytes), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        line(format("touch %s: mmap FAILED: %s", name, std::strerror(errno)));
        return;
    }
    auto* base = static_cast<uint8_t*>(p);
    double slowest = 0;
    const auto t0 = std::chrono::steady_clock::now();
    for (uint64_t off = 0; off < bytes; off += chunk) {
        const auto c0 = std::chrono::steady_clock::now();
        std::memset(base + off, 0, static_cast<size_t>(std::min(chunk, bytes - off)));
        slowest = std::max(slowest, seconds_since(c0));
    }
    const double first = seconds_since(t0);
    const auto t1 = std::chrono::steady_clock::now();
    std::memset(base, 1, static_cast<size_t>(bytes));
    const double second = seconds_since(t1);
    const double mib = double(bytes) / double(kMiB);
    const HostMemory m = host_memory();
    line(format("touch %s: prepare %.2f s, first write %.2f s (%.0f MiB/s, slowest %" PRIu64 " MiB chunk %.2f s), "
                "second write %.2f s, footprint %" PRIu64 " MiB, available %" PRIu64 " MiB",
                name, prepare_s, first, mib / std::max(first, 1e-6), chunk / kMiB, slowest, second,
                m.footprint / kMiB, m.available / kMiB));
    munmap(p, static_cast<size_t>(bytes));
#endif
}

#if !defined(_WIN32)
// A fresh unlinked file of `bytes` in `dir` (sparse after ftruncate); -1 on failure.
int touch_file(const std::string& dir, uint64_t bytes, const std::function<void(const std::string&)>& line) {
    const std::string path = dir + "/memprobe-touch.bin";
    const int fd = open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        line("touch: cannot create " + path + ": " + std::strerror(errno));
        return -1;
    }
    unlink(path.c_str());
    if (ftruncate(fd, static_cast<off_t>(bytes)) != 0) {
        line(std::string("touch: ftruncate: ") + std::strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}
#endif

}  // namespace

int run_touch(const Options& o, const std::function<void(const std::string&)>& line) {
    line("memprobe touch: " + host_description());
#if defined(_WIN32)
    (void)o;
    line("memprobe touch: not supported on Windows");
    return 0;
#else
    const uint64_t bytes = o.touch_mib * kMiB;
    const uint64_t chunk = std::max<uint64_t>(o.chunk_mib, 1) * kMiB;
    std::string dir = o.file_dir;
    if (dir.empty()) {
        const char* t = std::getenv("TMPDIR");
        dir = t != nullptr && *t != 0 ? t : "/tmp";
    }
    while (dir.size() > 1 && dir.back() == '/')
        dir.pop_back();
    line(format("memprobe touch: %" PRIu64 " MiB per variant in chunks of %" PRIu64 " MiB, files in %s, page %ld",
                o.touch_mib, chunk / kMiB, dir.c_str(), long(getpagesize())));
    int measured = 0;
    touch_variant("anonymous", -1, bytes, chunk, 0, line);
    ++measured;
    if (const int fd = touch_file(dir, bytes, line); fd >= 0) {
        touch_variant("sparse file", fd, bytes, chunk, 0, line);
        close(fd);
        ++measured;
    }
    if (const int fd = touch_file(dir, bytes, line); fd >= 0) {
        const auto t0 = std::chrono::steady_clock::now();
        std::vector<uint8_t> zeros(static_cast<size_t>(std::min<uint64_t>(chunk, 8 * kMiB)));
        bool ok = true;
        for (uint64_t off = 0; ok && off < bytes; off += zeros.size()) {
            const size_t n = static_cast<size_t>(std::min<uint64_t>(zeros.size(), bytes - off));
            ok = pwrite(fd, zeros.data(), n, static_cast<off_t>(off)) == static_cast<ssize_t>(n);
        }
        if (ok)
            ok = fsync(fd) == 0;
        if (ok)
            touch_variant("file written first", fd, bytes, chunk, seconds_since(t0), line);
        else
            line(std::string("touch file written first: pwrite/fsync FAILED: ") + std::strerror(errno));
        close(fd);
        ++measured;
    }
#if defined(__APPLE__) && defined(F_PREALLOCATE)
    if (const int fd = touch_file(dir, bytes, line); fd >= 0) {
        const auto t0 = std::chrono::steady_clock::now();
        fstore_t store{};
        store.fst_flags = F_ALLOCATEALL;
        store.fst_posmode = F_PEOFPOSMODE;
        store.fst_offset = 0;
        store.fst_length = static_cast<off_t>(bytes);
        if (fcntl(fd, F_PREALLOCATE, &store) == 0)
            touch_variant("file preallocated", fd, bytes, chunk, seconds_since(t0), line);
        else
            line(std::string("touch file preallocated: F_PREALLOCATE FAILED: ") + std::strerror(errno));
        close(fd);
        ++measured;
    }
#elif defined(__linux__)
    if (const int fd = touch_file(dir, bytes, line); fd >= 0) {
        const auto t0 = std::chrono::steady_clock::now();
        if (posix_fallocate(fd, 0, static_cast<off_t>(bytes)) == 0)
            touch_variant("file preallocated", fd, bytes, chunk, seconds_since(t0), line);
        else
            line("touch file preallocated: posix_fallocate FAILED");
        close(fd);
        ++measured;
    }
#endif
    return measured;
#endif
}

HostMemory host_memory() {
    HostMemory m;
#if defined(__APPLE__)
    task_vm_info_data_t info{};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS)
        m.footprint = info.phys_footprint;
    uint64_t mem = 0;
    size_t len = sizeof(mem);
    if (sysctlbyname("hw.memsize", &mem, &len, nullptr, 0) == 0)
        m.physical = mem;
#if TARGET_OS_IPHONE
    m.available = os_proc_available_memory();
#endif
#elif defined(__linux__)
    m.footprint = proc_kb("/proc/self/status", "VmRSS:");
    m.available = proc_kb("/proc/meminfo", "MemAvailable:");
    m.physical = proc_kb("/proc/meminfo", "MemTotal:");
#endif
    return m;
}

std::string host_description() {
    std::string d;
#if !defined(_WIN32)
    utsname u{};
    if (uname(&u) == 0)
        d = std::string(u.sysname) + " " + u.release + " " + u.machine;
#endif
#if defined(__APPLE__)
    char model[128] = {};
    size_t len = sizeof(model) - 1;
    if (sysctlbyname("hw.model", model, &len, nullptr, 0) == 0)
        d += std::string(", model ") + model;
#endif
#if defined(__ANDROID__)
    d += ", Android";
#endif
    return d;
}

uint64_t run(const Options& o, const std::function<void(const std::string&)>& line) {
    auto mem_line = [&](const char* what, uint64_t written_mib) {
        const HostMemory m = host_memory();
        line(format("%s: written %" PRIu64 " MiB, footprint %" PRIu64 " MiB, available %" PRIu64 " MiB", what,
                    written_mib, m.footprint / kMiB, m.available / kMiB));
    };
    const HostMemory start = host_memory();
    line("memprobe: " + host_description());
    line(format("memprobe: physical %" PRIu64 " MiB, available at start %" PRIu64 " MiB, footprint %" PRIu64
                " MiB, page %ld",
                start.physical / kMiB, start.available / kMiB, start.footprint / kMiB,
#if defined(_WIN32)
                4096L
#else
                long(getpagesize())
#endif
                ));
    line(format("memprobe: guest %" PRIu64 " MiB, chunks of %" PRIu64 " MiB, up to %" PRIu64 " MiB, %s fill, %s",
                o.guest_mib, o.chunk_mib, o.max_mib, o.zero_fill ? "zero" : "random",
                o.file_dir.empty() ? "anonymous memory" : (o.file_sync ? "file-backed (msync per chunk)" : "file-backed")));
    for (uint64_t gib : {8, 16, 32, 64})
        line(format("reserve %" PRIu64 " GiB address space: %s", gib, try_reserve(gib << 30) ? "ok" : "FAILED"));

    const uint64_t guest_bytes = o.guest_mib * kMiB;
    std::unique_ptr<platform::HostBlock> block;
    std::unique_ptr<FileBlock> file;
    if (o.file_dir.empty()) {
        try {
            block = std::make_unique<platform::HostBlock>(guest_bytes);
        } catch (const std::exception& e) {
            line(std::string("guest block: reservation FAILED: ") + e.what());
            return 0;
        }
        line(format("guest block: reserved %" PRIu64 " MiB (HostBlock, as the runtime)", o.guest_mib));
    } else {
        std::string error;
        file = std::make_unique<FileBlock>(o.file_dir, guest_bytes, &error);
        if (file->base() == nullptr) {
            line("guest block: file mapping FAILED: " + error);
            return 0;
        }
        line(format("guest block: %" PRIu64 " MiB mapped from a sparse file in %s", o.guest_mib, o.file_dir.c_str()));
    }
    uint8_t* const base = block ? block->base() : file->base();

    const uint64_t chunk = o.chunk_mib * kMiB;
    uint64_t state = 0x9E3779B97F4A7C15ull;
    uint64_t written = 0;
    for (uint64_t off = 0; off < guest_bytes && written < o.max_mib * kMiB; off += chunk) {
        const uint64_t len = std::min({chunk, guest_bytes - off, o.max_mib * kMiB - written});
        if (block) {
            try {
                block->commit_zeroed(off, len);
            } catch (const std::exception& e) {
                line(std::string("guest block: commit FAILED: ") + e.what());
                return written / kMiB;
            }
        }
        fill(base + off, len, &state, o.zero_fill);
        if (file && o.file_sync)
            file->sync(off, len);
        written += len;
        mem_line("guest", written / kMiB);
        std::this_thread::sleep_for(std::chrono::milliseconds(o.pause_ms));
    }
    if (written >= guest_bytes)
        line(format("guest block: all %" PRIu64 " MiB written", o.guest_mib));

    // Beyond the guest block: room left for the renderer, the HLE and the rest.
    std::vector<std::unique_ptr<uint8_t[]>> extra;
    while (written < o.max_mib * kMiB) {
        const uint64_t len = std::min(chunk, o.max_mib * kMiB - written);
        std::unique_ptr<uint8_t[]> p(new (std::nothrow) uint8_t[len]);
        if (!p) {
            mem_line("extra: allocation FAILED", written / kMiB);
            return written / kMiB;
        }
        fill(p.get(), len, &state, o.zero_fill);
        extra.push_back(std::move(p));
        written += len;
        mem_line("extra", written / kMiB);
        std::this_thread::sleep_for(std::chrono::milliseconds(o.pause_ms));
    }
    line(format("memprobe: reached the limit of %" PRIu64 " MiB without being stopped", o.max_mib));
    return written / kMiB;
}

}  // namespace lgp::memprobe
