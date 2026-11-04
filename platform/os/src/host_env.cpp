// Process environment (host_env.hpp).
#include "lgp/platform/host_env.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#include <mach/mach.h>
#include <sys/resource.h>
#include <sys/sysctl.h>
#include <mach/vm_statistics.h>
#include <unistd.h>
#if TARGET_OS_IPHONE
#include <os/proc.h>
#endif
#elif defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define PSAPI_VERSION 2  // K32GetProcessMemoryInfo from kernel32, no psapi.lib
#include <windows.h>
#include <psapi.h>
#elif defined(__linux__)
#include <malloc.h>
#include <sys/resource.h>
#include <cstring>
#endif
#if !defined(_WIN32)
#include <fcntl.h>   // open
#include <unistd.h>  // dup2, readlink
#endif

namespace lgp::platform {

bool set_env(const std::string& name, const std::string& value) {
    if (name.empty() || name.find('=') != std::string::npos)
        return false;
#if defined(_WIN32)
    return _putenv_s(name.c_str(), value.c_str()) == 0;
#else
    return setenv(name.c_str(), value.c_str(), 1) == 0;
#endif
}

bool redirect_output(const std::filesystem::path& file, std::string* error) {
    std::fflush(stdout);
    std::fflush(stderr);
#if defined(_WIN32)
    // Truncate first, then both streams append: each write lands at the end
    // whichever stream it comes from.
    if (!std::ofstream(file, std::ios::binary | std::ios::trunc)) {
        *error = "cannot write " + file.string();
        return false;
    }
    FILE* out = _wfreopen(file.c_str(), L"a", stdout);
    FILE* err = out != nullptr ? _wfreopen(file.c_str(), L"a", stderr) : nullptr;
    if (out == nullptr || err == nullptr) {
        *error = "cannot write " + file.string();
        return false;
    }
    // The MSVC runtime has no line buffering (_IOLBF means full buffering):
    // stdout would reach the file only at exit, after everything stderr wrote.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
#else
#if defined(__linux__)
    // What descriptors 1 and 2 were before, for the line below.
    std::string before[2];
    for (int i = 0; i < 2; ++i) {
        char link[256];
        const ssize_t n = readlink(i == 0 ? "/proc/self/fd/1" : "/proc/self/fd/2", link, sizeof link - 1);
        before[i] = n > 0 ? std::string(link, size_t(n)) : std::string("closed");
    }
#endif
    // One open file on descriptors 1 and 2, set with dup2 only. freopen first
    // closes descriptor 1 and opens the file again; another thread that opens
    // a descriptor in between (on Android, the libc logger's socket to logd)
    // gets number 1, and its write and close then hit lgp.log. On the Shield
    // every lgp.log began with one logd record (86 bytes, libc's "Access
    // denied finding property ..."), and in 3 of 21 runs and 2 of 20 with a
    // single descriptor (S19-S22) the stdout lines or all lines were missing
    // (suspicion: that close). dup2 replaces 1 and 2 without such a gap.
    const int fd = open(file.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0 || dup2(fd, STDOUT_FILENO) < 0 || dup2(fd, STDERR_FILENO) < 0) {
        if (fd >= 0)
            close(fd);
        *error = "cannot write " + file.string();
        return false;
    }
    // A process started with 1 or 2 closed gets one of them from open().
    if (fd != STDOUT_FILENO && fd != STDERR_FILENO)
        close(fd);
    std::clearerr(stdout);
    std::clearerr(stderr);
    // A buffer of our own: stdout has been used already, and without one
    // glibc keeps its earlier full buffering in effect.
    static char out_buffer[4096];
    std::setvbuf(stdout, out_buffer, _IOLBF, sizeof out_buffer);
#endif
    std::setvbuf(stderr, nullptr, _IONBF, 0);
#if defined(__linux__)
    std::fprintf(stderr, "log: descriptors before: 1 %s, 2 %s\n", before[0].c_str(), before[1].c_str());
#endif
    return true;
}

namespace {
std::atomic<uint64_t> g_low_memory_warnings{0};
}  // namespace

void note_low_memory() {
    const uint64_t n = g_low_memory_warnings.fetch_add(1, std::memory_order_relaxed) + 1;
    ProcessMemory m;
    if (process_memory(&m))
        std::fprintf(stderr, "host: low memory warning %llu: footprint %llu MiB, available %llu MiB\n",
                     (unsigned long long)n, (unsigned long long)(m.footprint >> 20),
                     (unsigned long long)(m.available >> 20));
    else
        std::fprintf(stderr, "host: low memory warning %llu\n", (unsigned long long)n);
}

uint64_t low_memory_warnings() { return g_low_memory_warnings.load(std::memory_order_relaxed); }

bool process_memory(ProcessMemory* out) {
    *out = {};
#if defined(__APPLE__)
    task_vm_info_data_t info{};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS)
        return false;
    out->footprint = info.phys_footprint;
    if (count >= TASK_VM_INFO_REV2_COUNT)
        out->peak = info.ledger_phys_footprint_peak;
    // count is in natural_t units; read the purgeable ledgers only where the
    // kernel filled them. Each includes its compressed share (the nonvolatile
    // one counts into phys_footprint compressed as well).
    if (size_t(count) * sizeof(natural_t) >=
        offsetof(task_vm_info_data_t, ledger_purgeable_volatile_compressed) + sizeof(int64_t)) {
        out->has_purgeable = true;
        out->purgeable_nonvolatile =
            uint64_t(info.ledger_purgeable_nonvolatile + info.ledger_purgeable_novolatile_compressed);
        out->purgeable_volatile = uint64_t(info.ledger_purgeable_volatile + info.ledger_purgeable_volatile_compressed);
    }
#if TARGET_OS_IPHONE
    out->available = os_proc_available_memory();
#endif
    return true;
#elif defined(_WIN32)
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    if (!GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc)))
        return false;
    out->footprint = pmc.PrivateUsage;
    out->peak = pmc.PeakPagefileUsage;
    return true;
#elif defined(__linux__)
    std::FILE* f = std::fopen("/proc/self/status", "r");
    if (f == nullptr)
        return false;
    char line[256];
    bool found = false;
    while (std::fgets(line, sizeof(line), f) != nullptr) {
        unsigned long long kib = 0;
        if (std::sscanf(line, "VmRSS: %llu kB", &kib) == 1) {
            out->footprint = uint64_t(kib) * 1024;
            found = true;
        } else if (std::sscanf(line, "VmHWM: %llu kB", &kib) == 1) {
            out->peak = uint64_t(kib) * 1024;
        }
    }
    std::fclose(f);
    return found;
#else
    return false;
#endif
}

bool process_cpu(ProcessCpu* out) {
    *out = {};
#if defined(__EMSCRIPTEN__)
    return false;
#elif defined(_WIN32)
    FILETIME created, exited, kernel, user;
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user))
        return false;
    const auto ns = [](const FILETIME& t) {
        return ((uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime) * 100;  // 100 ns units
    };
    out->user_ns = ns(user);
    out->system_ns = ns(kernel);
    PROCESS_MEMORY_COUNTERS pmc{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
        out->faults = pmc.PageFaultCount;
    return true;
#elif defined(__APPLE__) || defined(__linux__)
    rusage ru{};
    if (getrusage(RUSAGE_SELF, &ru) != 0)
        return false;
    const auto ns = [](const timeval& t) { return uint64_t(t.tv_sec) * 1000000000u + uint64_t(t.tv_usec) * 1000u; };
    out->user_ns = ns(ru.ru_utime);
    out->system_ns = ns(ru.ru_stime);
#if defined(__APPLE__)
    // The task's own counters: every fault, the ones that read a page in,
    // and copy-on-write (rusage's minflt/majflt are not kept per process).
    task_events_info_data_t ev{};
    mach_msg_type_number_t count = TASK_EVENTS_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_EVENTS_INFO, reinterpret_cast<task_info_t>(&ev), &count) == KERN_SUCCESS) {
        out->faults = uint64_t(ev.faults);
        out->pageins = uint64_t(ev.pageins);
        out->cow = uint64_t(ev.cow_faults);
        out->syscalls = uint64_t(ev.syscalls_unix);
        out->mach = uint64_t(ev.syscalls_mach);
        out->csw = uint64_t(ev.csw);
    }
#else
    out->faults = uint64_t(ru.ru_minflt) + uint64_t(ru.ru_majflt);
    out->pageins = uint64_t(ru.ru_majflt);
    out->csw = uint64_t(ru.ru_nvcsw) + uint64_t(ru.ru_nivcsw);
#endif
    return true;
#else
    return false;
#endif
}

bool system_memory(SystemMemory* out) {
    *out = {};
#if defined(__APPLE__)
    // mach_host_self() hands out a send right per call; take it once.
    static const mach_port_t host = mach_host_self();
    vm_statistics64_data_t vm{};
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    if (host_statistics64(host, HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&vm), &count) !=
        KERN_SUCCESS)
        return false;
    vm_size_t page = 0;
    if (host_page_size(host, &page) != KERN_SUCCESS || page == 0)
        page = 16384;
    out->free = (uint64_t(vm.free_count) + vm.speculative_count) * page;
    out->file_cache = uint64_t(vm.external_page_count) * page;
    out->compressed = uint64_t(vm.compressor_page_count) * page;
    // Not readable in every sandbox; then unknown.
    int level = 0;
    size_t size = sizeof(level);
    if (sysctlbyname("kern.memorystatus_level", &level, &size, nullptr, 0) == 0)
        out->level = level;
    return true;
#elif defined(__linux__)
    std::FILE* f = std::fopen("/proc/meminfo", "r");
    if (f == nullptr)
        return false;
    char line[256];
    unsigned long long total = 0, free_kib = 0, cached = 0;
    long long avail = -1;  // MemAvailable is missing before Linux 3.14
    while (std::fgets(line, sizeof(line), f) != nullptr) {
        unsigned long long kib = 0;
        if (std::sscanf(line, "MemTotal: %llu kB", &kib) == 1)
            total = kib;
        else if (std::sscanf(line, "MemFree: %llu kB", &kib) == 1)
            free_kib = kib;
        else if (std::sscanf(line, "MemAvailable: %llu kB", &kib) == 1)
            avail = (long long)kib;
        else if (std::sscanf(line, "Cached: %llu kB", &kib) == 1)
            cached = kib;
    }
    std::fclose(f);
    if (total == 0)
        return false;
    out->free = uint64_t(free_kib) * 1024;
    out->file_cache = uint64_t(cached) * 1024;
    if (avail >= 0)
        out->level = int((unsigned long long)avail * 100 / total);
    return true;
#else
    return false;
#endif
}

#if !defined(__APPLE__)
bool gpu_memory(GpuMemory*) {
    return false;  // Apple only (platform/apple/src/metal_memory.mm)
}
#endif

#if defined(__APPLE__)
namespace {

// Names of the VM region tags worth telling apart (mach/vm_statistics.h);
// each only where the SDK defines it.
std::string tag_name(unsigned tag) {
    switch (tag) {
#define LGP_TAG(macro, name) case macro: return name;
#ifdef VM_MEMORY_MALLOC
        LGP_TAG(VM_MEMORY_MALLOC, "MALLOC")
#endif
#ifdef VM_MEMORY_MALLOC_SMALL
        LGP_TAG(VM_MEMORY_MALLOC_SMALL, "MALLOC_SMALL")
#endif
#ifdef VM_MEMORY_MALLOC_LARGE
        LGP_TAG(VM_MEMORY_MALLOC_LARGE, "MALLOC_LARGE")
#endif
#ifdef VM_MEMORY_MALLOC_HUGE
        LGP_TAG(VM_MEMORY_MALLOC_HUGE, "MALLOC_HUGE")
#endif
#ifdef VM_MEMORY_MALLOC_TINY
        LGP_TAG(VM_MEMORY_MALLOC_TINY, "MALLOC_TINY")
#endif
#ifdef VM_MEMORY_MALLOC_LARGE_REUSABLE
        LGP_TAG(VM_MEMORY_MALLOC_LARGE_REUSABLE, "MALLOC_LARGE_REUSABLE")
#endif
#ifdef VM_MEMORY_MALLOC_LARGE_REUSED
        LGP_TAG(VM_MEMORY_MALLOC_LARGE_REUSED, "MALLOC_LARGE_REUSED")
#endif
#ifdef VM_MEMORY_MALLOC_NANO
        LGP_TAG(VM_MEMORY_MALLOC_NANO, "MALLOC_NANO")
#endif
#ifdef VM_MEMORY_MALLOC_MEDIUM
        LGP_TAG(VM_MEMORY_MALLOC_MEDIUM, "MALLOC_MEDIUM")
#endif
#ifdef VM_MEMORY_IOKIT
        LGP_TAG(VM_MEMORY_IOKIT, "IOKit")
#endif
#ifdef VM_MEMORY_STACK
        LGP_TAG(VM_MEMORY_STACK, "Stack")
#endif
#ifdef VM_MEMORY_DYLIB
        LGP_TAG(VM_MEMORY_DYLIB, "dylib")
#endif
#ifdef VM_MEMORY_FOUNDATION
        LGP_TAG(VM_MEMORY_FOUNDATION, "Foundation")
#endif
#ifdef VM_MEMORY_COREGRAPHICS
        LGP_TAG(VM_MEMORY_COREGRAPHICS, "CoreGraphics")
#endif
#ifdef VM_MEMORY_LAYERKIT
        LGP_TAG(VM_MEMORY_LAYERKIT, "CoreAnimation")
#endif
#ifdef VM_MEMORY_DYLD
        LGP_TAG(VM_MEMORY_DYLD, "dyld")
#endif
#ifdef VM_MEMORY_DYLD_MALLOC
        LGP_TAG(VM_MEMORY_DYLD_MALLOC, "dyld malloc")
#endif
#ifdef VM_MEMORY_GLSL
        LGP_TAG(VM_MEMORY_GLSL, "GLSL")
#endif
#ifdef VM_MEMORY_OS_ALLOC_ONCE
        LGP_TAG(VM_MEMORY_OS_ALLOC_ONCE, "OS alloc once")
#endif
#ifdef VM_MEMORY_LIBDISPATCH
        LGP_TAG(VM_MEMORY_LIBDISPATCH, "libdispatch")
#endif
#ifdef VM_MEMORY_ACCELERATE
        LGP_TAG(VM_MEMORY_ACCELERATE, "Accelerate")
#endif
#ifdef VM_MEMORY_IOSURFACE
        LGP_TAG(VM_MEMORY_IOSURFACE, "IOSurface")
#endif
#ifdef VM_MEMORY_AUDIO
        LGP_TAG(VM_MEMORY_AUDIO, "Audio")
#endif
#ifdef VM_MEMORY_VIDEOBITSTREAM
        LGP_TAG(VM_MEMORY_VIDEOBITSTREAM, "video bitstream")
#endif
#ifdef VM_MEMORY_IOACCELERATOR
        LGP_TAG(VM_MEMORY_IOACCELERATOR, "IOAccelerator")
#endif
#undef LGP_TAG
    default:
        return "tag " + std::to_string(tag);
    }
}

}  // namespace
#endif

bool memory_breakdown(std::vector<MemoryPart>* out) {
    out->clear();
#if defined(__APPLE__)
    // Footprint ledgers by owner tag (fields of newer task_vm_info revisions; the
    // fields are read only where the kernel filled them).
    task_vm_info_data_t info{};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS)
        return false;
    const size_t filled = size_t(count) * sizeof(natural_t);
    const auto ledger = [&](const char* name, size_t end, int64_t value, int64_t compressed) {
        if (filled >= end && value + compressed > 0)
            out->push_back({name, uint64_t(value + compressed)});
    };
    out->push_back({"footprint", info.phys_footprint});
    ledger("ledger graphics", offsetof(task_vm_info_data_t, ledger_tag_graphics_footprint_compressed) + sizeof(int64_t),
           info.ledger_tag_graphics_footprint, info.ledger_tag_graphics_footprint_compressed);
    ledger("ledger media", offsetof(task_vm_info_data_t, ledger_tag_media_footprint_compressed) + sizeof(int64_t),
           info.ledger_tag_media_footprint, info.ledger_tag_media_footprint_compressed);
    ledger("ledger neural", offsetof(task_vm_info_data_t, ledger_tag_neural_footprint_compressed) + sizeof(int64_t),
           info.ledger_tag_neural_footprint, info.ledger_tag_neural_footprint_compressed);

    // Regions, descending into submaps (the shared cache). vm_region_recurse_64
    // rather than mach_vm_region_recurse: the iOS SDK marks mach_vm.h
    // unsupported, and vm_address_t is 64 bits on every Apple target we build.
    const uint64_t page = uint64_t(sysconf(_SC_PAGESIZE));
    std::map<std::string, uint64_t> by_tag;
    uint64_t file_backed = 0;
    vm_address_t addr = 0;
    natural_t depth = 0;
    for (;;) {
        vm_size_t size = 0;
        vm_region_submap_info_data_64_t r{};
        mach_msg_type_number_t n = VM_REGION_SUBMAP_INFO_COUNT_64;
        if (vm_region_recurse_64(mach_task_self(), &addr, &size, &depth, reinterpret_cast<vm_region_recurse_info_t>(&r),
                                 &n) != KERN_SUCCESS)
            break;
        if (r.is_submap) {
            ++depth;
            continue;
        }
        const uint64_t bytes = (uint64_t(r.pages_dirtied) + r.pages_swapped_out) * page;
        if (r.external_pager)
            file_backed += bytes;
        else if (bytes != 0 && r.share_mode != SM_EMPTY)
            by_tag[tag_name(r.user_tag)] += bytes;
        addr += size;
    }
    std::vector<MemoryPart> regions;
    for (const auto& [name, bytes] : by_tag) regions.push_back({name, bytes});
    std::sort(regions.begin(), regions.end(), [](const MemoryPart& a, const MemoryPart& b) { return a.bytes > b.bytes; });
    out->insert(out->end(), regions.begin(), regions.end());
    if (file_backed != 0)
        out->push_back({"file-backed (not in footprint)", file_backed});
    return true;
#elif defined(__linux__)
    // Linux and Android: PSS of the process and its swapped PSS (zram) apart,
    // then PSS plus swapped PSS of every mapping in /proc/self/smaps, grouped as Android's dumpsys meminfo groups them: the
    // allocator's regions are "native heap", [anon:NAME] keeps its name, GPU
    // driver devices count as "gpu driver", plain anonymous mappings (the
    // guest block among them) as "anon (unnamed)". Then what malloc itself
    // says is in use; on Bionic heap pages held but free show up as the
    // difference to "native heap". glibc maps big blocks and thread arenas
    // unnamed (and thread stacks too), so there they land in "anon (unnamed)".
    std::FILE* f = std::fopen("/proc/self/smaps", "r");
    if (f == nullptr)
        return false;
    std::map<std::string, uint64_t> by_name;
    std::string group;
    uint64_t total = 0, swapped = 0;
    char line[512];
    const auto group_of = [](const char* path) -> std::string {
        if (*path == '\0')
            return "anon (unnamed)";
        if (std::strcmp(path, "[heap]") == 0 || std::strncmp(path, "[anon:libc_malloc", 17) == 0 ||
            std::strncmp(path, "[anon:scudo:", 12) == 0 || std::strncmp(path, "[anon:jemalloc", 14) == 0 ||
            std::strncmp(path, "[anon:GWP-ASan", 14) == 0)
            return "native heap";
        if (std::strncmp(path, "[anon:stack_and_tls:", 20) == 0 || std::strncmp(path, "[stack", 6) == 0 ||
            std::strncmp(path, "[anon:thread signal stack", 25) == 0)
            return "stacks";
        if (std::strncmp(path, "[anon:", 6) == 0) {
            const char* end = std::strchr(path, ']');
            return std::string(path, end != nullptr ? size_t(end - path + 1) : std::strlen(path));
        }
        if (std::strncmp(path, "/dev/kgsl", 9) == 0 || std::strncmp(path, "/dev/mali", 9) == 0 ||
            std::strncmp(path, "/dev/dri", 8) == 0 || std::strncmp(path, "/dev/nvidia", 11) == 0 ||
            std::strncmp(path, "/dmabuf", 7) == 0 || std::strncmp(path, "/dev/dma_heap", 13) == 0)
            return "gpu driver";
        if (std::strstr(path, ".so") != nullptr)
            return "code and data (.so)";
        if (*path == '[')
            return path;
        return "other files";
    };
    bool line_start = true;
    while (std::fgets(line, sizeof(line), f) != nullptr) {
        // A path longer than the buffer arrives in pieces; only a line's
        // first piece is parsed.
        const bool first_piece = line_start;
        line_start = std::strchr(line, '\n') != nullptr;
        if (!first_piece)
            continue;
        unsigned long long kib = 0;
        if (std::sscanf(line, "Pss: %llu kB", &kib) == 1) {
            by_name[group] += uint64_t(kib) * 1024;
            total += uint64_t(kib) * 1024;
            continue;
        }
        if (std::sscanf(line, "SwapPss: %llu kB", &kib) == 1) {
            by_name[group] += uint64_t(kib) * 1024;
            swapped += uint64_t(kib) * 1024;
            continue;
        }
        // A mapping header: "start-end perms offset dev inode [path]".
        unsigned long long start = 0, end = 0;
        int name_at = 0;
        char perms[8];
        if (std::sscanf(line, "%llx-%llx %7s %*s %*s %*s %n", &start, &end, perms, &name_at) == 3) {
            char* path = line + name_at;
            path[std::strcspn(path, "\n")] = '\0';
            group = group_of(path);
        }
    }
    std::fclose(f);
    if (total == 0)
        return false;
    // dumpsys shows Pss Total and SwapPss Dirty as separate columns.
    out->push_back({"pss", total});
    if (swapped != 0)
        out->push_back({"swap pss", swapped});
    std::vector<MemoryPart> regions;
    for (const auto& [name, bytes] : by_name) regions.push_back({name, bytes});
    std::sort(regions.begin(), regions.end(), [](const MemoryPart& a, const MemoryPart& b) { return a.bytes > b.bytes; });
    out->insert(out->end(), regions.begin(), regions.end());
#if defined(__BIONIC__)
    const struct mallinfo mi = mallinfo();  // size_t fields on Bionic
    out->push_back({"malloc in use", uint64_t(mi.uordblks)});
#elif defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 33))
    const struct mallinfo2 mi = mallinfo2();
    out->push_back({"malloc in use", uint64_t(mi.uordblks) + uint64_t(mi.hblkhd)});
#endif
    return true;
#else
    return false;
#endif
}

}  // namespace lgp::platform
