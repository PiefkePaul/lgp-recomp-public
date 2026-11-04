// Where guest threads spend their wall time (P6 measurement aid,
// docs/knowledge/zeitaufteilung-gast-threads.md).
//
// Off by default. When enabled, every guest thread keeps one "current
// category" and adds the host wall time between two switches to it: guest
// code, waiting for its scheduler slot (baton), blocked in the kernel, inside
// an HLE service request, inside an NVN proc, waiting for the NVN-HLE lock,
// inside a renderer call (blocked until the GPU thread recorded the work)
// and inside the movie:: HLE. Switches happen in the scheduler, the IPC svc,
// the NVN-HLE and the movie:: HLE;
// lifted code is never instrumented. time_stats_report() prints the time per
// category since the previous report, so a run can be cut into windows of N
// presented frames (lgp_boot --time-stats N).
//
// Readings of other threads are taken without stopping them, as a snapshot
// between two of their switches; the running segment of each thread is added
// from its start time. Good enough for windows of seconds, not for single
// frames.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace lgp::rt {

enum class TimeCat : uint8_t {
    Guest,    // running: lifted code, svcs without their own category
    Ready,    // wants to run, waits for its scheduler slot (baton modes)
    Wait,     // blocked in the kernel (waits, sleeps, frame pacing)
    Ipc,      // svcSendSyncRequest: the HLE service handling the request
    Nvn,      // inside an NVN proc (HLE recording), without Gpu and NvnLock
    NvnLock,  // waiting for the NVN-HLE lock (another thread is inside a proc)
    Gpu,      // inside a renderer call (blocked until the GPU thread is done)
    Movie,    // inside the movie:: HLE (extractor and decoder methods)
    Count,
};
const char* time_cat_name(TimeCat cat);

namespace detail {
extern std::atomic<bool> g_time_stats_enabled;
}  // namespace detail

void enable_time_stats(bool on);
// Clock of the accounting: 0 reads the host clock at every switch (default),
// N > 0 a value a helper thread refreshes every N microseconds. Sums over
// many switches stay right on average, single segments are off by up to N.
// For hosts where reading the clock is costly or leaks (Safari workers:
// performance.now() per switch, docs/knowledge/zeitaufteilung-gast-threads.md).
// Call before enable_time_stats; the helper thread runs until the process ends.
void set_time_stats_tick_us(uint32_t us);
inline bool time_stats_enabled() { return detail::g_time_stats_enabled.load(std::memory_order_relaxed); }

// Registers the calling host thread as guest thread `id` (category Guest from
// now on) / ends its accounting. Without enable_time_stats both do nothing.
void time_stats_attach(uint64_t id, int32_t priority, int32_t core);
void time_stats_detach();
// Switches the calling thread to `to` and returns the previous category.
// Returns Guest and does nothing if disabled or the thread is not attached.
TimeCat time_stats_switch(TimeCat to);

class TimeScope {
public:
    explicit TimeScope(TimeCat cat) : prev_(time_stats_enabled() ? time_stats_switch(cat) : TimeCat::Count) {}
    ~TimeScope() {
        if (prev_ != TimeCat::Count)
            time_stats_switch(prev_);
    }
    TimeScope(const TimeScope&) = delete;
    TimeScope& operator=(const TimeScope&) = delete;

private:
    TimeCat prev_;
};

// A host component that reports its own busy time (the renderer's GPU
// thread): totals since start, read at every report.
using TimeSource = std::function<void(uint64_t* busy_ns, uint64_t* calls)>;
void time_stats_add_source(std::string name, TimeSource source);
void time_stats_clear_sources();
// Scheduler handovers so far: a thread got its slot from another one, and
// how long it took from then until it ran (host wakeup latency plus the
// scheduler lock). Counted only while time stats are on; process-wide.
void scheduler_wake_latency(uint64_t* total_ns, uint64_t* count);

// Numbers of one thread over a report window.
struct ThreadTimes {
    uint64_t id = 0;
    int32_t priority = 0, core = 0;
    bool alive = false;
    uint64_t ns[size_t(TimeCat::Count)] = {};
    uint64_t entries[size_t(TimeCat::Count)] = {};  // switches into the category
};
struct SourceTimes {
    std::string name;
    uint64_t busy_ns = 0, calls = 0;
};
struct TimeWindow {
    uint64_t wall_ns = 0;
    std::vector<ThreadTimes> threads;  // threads with any time in the window
    std::vector<SourceTimes> sources;
};
// Everything since the previous call (or since enable_time_stats).
TimeWindow time_stats_take();
// time_stats_take() as text lines ("time: ..."), `frames` presented in the
// window (0: unknown). Threads that only waited are summed up in one line.
std::vector<std::string> time_stats_report(uint64_t first_frame, uint64_t frames);
// Each live thread's current category and how long it has been in it
// ("thread 12: nvn for 9.81 s"), longest first, without taking a window.
// Empty while time stats are off. For hang reports (lgp_boot --stall-report).
std::vector<std::string> time_stats_current();

}  // namespace lgp::rt
