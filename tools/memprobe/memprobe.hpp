// Memory probe for mobile targets (P7.2a iOS/iPadOS, P7.3a Android): can a
// device hold the game's guest memory? Contains no game code.
//
// 1. Reserves the guest block exactly as the runtime does (lgp::platform::
//    HostBlock, a 4 GiB PROT_NONE reservation) and, as a test of the virtual
//    address space, a few larger reservations (8 to 64 GiB).
// 2. Commits the guest block chunk by chunk and fills it with incompressible
//    data (iOS compresses memory, a zero or constant fill would not count),
//    then keeps allocating beyond it up to max_mib.
// 3. After every chunk one log line: MiB written, the process footprint and
//    what the system says is left (iOS os_proc_available_memory, Linux and
//    Android MemAvailable). The log is flushed to disk per line, so after the
//    system kills the process the last line shows how far it got.
// 4. With file_dir set (P7.4, tvOS): the guest block is a shared mapping of a
//    sparse, already unlinked file in that folder instead of anonymous memory.
//    Darwin counts such file-backed pages as "external", not in
//    phys_footprint (docs/knowledge/darwin-footprint-und-jetsam.md); the probe
//    measures whether jetsam lets more than the anonymous limit through.
// 5. With touch_mib set (tvOS load stall, docs/analysis/42-praesent-stillstand.md):
//    instead of 1.-4., times the first write (memset, as a game clearing new
//    heap) of touch_mib MiB in four ways: anonymous memory, a sparse file
//    mapping, a file whose blocks were first written with pwrite, and one
//    first preallocated (F_PREALLOCATE where the platform has it). The file
//    variants use file_dir, or the temporary folder.
// 6. run_native_probe (docs/analysis/56 stage F): can original code run at a
//    fixed guest address? Reserves native_gib GiB at bases below 2^39, then
//    puts a page of the probe's own code there and calls it: on Apple the
//    signed text page itself (vm_remap, the iOS/tvOS route), from a file
//    (the Android route) and as anonymous memory made executable (what the
//    native start does on the Mac). A process the system kills for running
//    unsigned code stops after the line "calling ...".
#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace lgp::memprobe {

struct Options {
    uint64_t guest_mib = 3456;   // ADR-0001: guest block for TotalMemorySize 3 GiB
    uint64_t chunk_mib = 64;
    uint64_t max_mib = 16384;    // stop here if the system has not stopped us
    bool zero_fill = false;      // true: touch with zeros (compressible), for comparison
    unsigned pause_ms = 50;      // between chunks: lets jetsam/lmkd see the growth
    std::string file_dir;        // non-empty: guest block backed by a file there (see 4.)
    bool file_sync = false;      // file mode: msync(MS_ASYNC) after every chunk
    uint64_t touch_mib = 0;      // non-zero: first-touch timing instead (see 5.)
    uint64_t native_gib = 8;     // run_native_probe: guest space to reserve (see 6.)
};

// Numbers the host reports about this process (0 if unknown).
struct HostMemory {
    uint64_t footprint = 0;  // Apple phys_footprint, else resident set size
    uint64_t available = 0;  // iOS os_proc_available_memory, else MemAvailable
    uint64_t physical = 0;   // installed RAM
};
HostMemory host_memory();
// A short description of the host (OS, device model where known).
std::string host_description();

// Runs the probe; `line` gets every log line (without newline). Returns the
// MiB written when it stopped by itself (max_mib reached or an allocation
// failed); a process killed by the system never returns.
uint64_t run(const Options& options, const std::function<void(const std::string&)>& line);
// The first-touch timing of 5. on its own; returns the number of variants measured.
int run_touch(const Options& options, const std::function<void(const std::string&)>& line);
// The native probe of 6.; returns how many steps worked (reservation, each code route).
int run_native_probe(const Options& options, const std::function<void(const std::string&)>& line);

}  // namespace lgp::memprobe
