// Process environment from the command line (lgp_boot --env KEY=VALUE): in the
// browser the page passes arguments (?args=) but no environment, so switches
// read with getenv (LGP_WEBGPU_*) are set this way there.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace lgp::platform {

// Sets `name` to `value` for this process (overwrites); false on failure.
bool set_env(const std::string& name, const std::string& value);

// Sends stdout and stderr to `file` (created or truncated), for an app
// started without a console (P7.1); stdout is flushed per line where the C
// library allows it. false and *error on failure (output unchanged).
bool redirect_output(const std::filesystem::path& file, std::string* error);

// What the host counts against this process (--time-stats, P7.4): Apple the
// phys_footprint jetsam compares with its limit, Linux/Android the resident
// set, Windows the private commit. available: bytes until the limit where
// the OS tells (iOS/tvOS os_proc_available_memory), otherwise 0.
struct ProcessMemory {
    uint64_t footprint = 0;
    uint64_t peak = 0;  // 0 where the OS does not keep one
    uint64_t available = 0;
    // Apple: purgeable memory (Metal resources with a purgeable state are such
    // VM objects); nonvolatile counts in the footprint, volatile the system
    // may drop (both with their compressed share). has_purgeable false where
    // the kernel does not report it.
    bool has_purgeable = false;
    uint64_t purgeable_nonvolatile = 0;
    uint64_t purgeable_volatile = 0;
};
// false where there is nothing to read (the browser).
bool process_memory(ProcessMemory* out);

// The OS warned that memory runs short (iOS/tvOS memory warning, Android
// onTrimMemory/onLowMemory; SDL_EVENT_LOW_MEMORY). note_low_memory() counts
// the warning and logs footprint and available; caches that can be rebuilt
// (the WebGPU backend's geometry cache and buffer pool) compare
// low_memory_warnings() with the count they last saw and drop what they hold.
// Any thread.
void note_low_memory();
uint64_t low_memory_warnings();

// Where that footprint sits (Darwin and Linux/Android, P7.4; docs/analysis/32-metal-speicher-je-pipeline.md,
// Linux/Android: docs/analysis/36-android-native-heap.md):
// the kernel's footprint ledgers by owner tag ("ledger graphics" is memory
// the GPU driver charges to the process without a mapping, "ledger media",
// "ledger neural"), then per VM region tag ("MALLOC_SMALL", "IOAccelerator",
// "tag 0" for plain mmap, ...) the pages dirtied or compressed (shared objects too), as
// vmmap's DIRTY + SWAPPED columns sum them, largest first. File-backed
// regions are summed separately as "file-backed (not in footprint)". The
// region sums are an estimate: pages of an object mapped twice count twice,
// so they need not add up to the footprint exactly. Linux/Android: "pss" first,
// "swap pss" (only if any) next, as dumpsys shows them apart; then PSS plus
// swapped PSS of the mappings grouped like dumpsys meminfo ("native heap",
// "gpu driver", "anon (unnamed)" with the guest block, "[anon:NAME]", ...), last
// "malloc in use" (what the allocator hands out; on Bionic below "native heap"
// by what it keeps free, under glibc big blocks, thread arenas and thread
// stacks are unnamed mappings and count as "anon (unnamed)").
struct MemoryPart {
    std::string name;
    uint64_t bytes = 0;
};
// false where the OS gives no breakdown.
bool memory_breakdown(std::vector<MemoryPart>* out);

// The machine's RAM beside this process (--time-stats, P7.4: whether a kill
// without a report came from system-wide shortage rather than the process
// limit). free: unused pages; file_cache: pages backed by files (Apple
// "external", Linux Cached), which the system can drop or must write back;
// compressed: physical memory the compressor occupies (Apple; 0 elsewhere). level: the share of RAM
// the system counts as available, percent, -1 where unknown.
struct SystemMemory {
    uint64_t free = 0;
    uint64_t file_cache = 0;
    uint64_t compressed = 0;
    int level = -1;
};
// false where there is nothing to read (the browser, Windows for now).
bool system_memory(SystemMemory* out);

// CPU time and page faults of the whole process so far (--time-stats "host
// cpu", the load stall in docs/analysis/42): kernel time against user time
// tells whether a thread that "computes" in guest code spends it in page
// faults. faults: all faults (Linux/Android minor ones); pageins: faults that
// read from storage (Linux major faults); cow: copy-on-write faults (Apple
// only). syscalls: Unix system calls, mach: Mach traps and messages (Apple
// only); csw: context switches (Apple: the task's, Linux: voluntary plus
// involuntary), to tell where kernel time goes. false where there is nothing
// to read (the browser).
struct ProcessCpu {
    uint64_t user_ns = 0;
    uint64_t system_ns = 0;
    uint64_t faults = 0;
    uint64_t pageins = 0;
    uint64_t cow = 0;
    uint64_t syscalls = 0;
    uint64_t mach = 0;
    uint64_t csw = 0;
};
bool process_cpu(ProcessCpu* out);

// What the system's Metal device holds for this process (P7.4), to tell GPU
// resources apart from other graphics memory in the footprint: allocated =
// MTLDevice.currentAllocatedSize (buffers, textures and heaps, wgpu's own
// staging and the driver's allocations through the device included),
// working_set = recommendedMaxWorkingSetSize. The first call creates the
// default Metal device and keeps it. false off Apple.
struct GpuMemory {
    uint64_t allocated = 0;
    uint64_t working_set = 0;
};
bool gpu_memory(GpuMemory* out);

}  // namespace lgp::platform
