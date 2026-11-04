// The guest clock: what svcGetSystemTick, CNTPCT_EL0, kernel timeouts and the
// host services that follow time (hid sampling, audout buffer release) see.
//
// Host mode (default): guest time is the host's steady clock since the
// runtime started; timeouts wait on the host clock.
//
// Virtual mode (--clock virtual): guest time does not follow the host at all.
// It only moves forward
//   - by a fixed cost per svc and per counter read of a guest thread
//     (kVirtualSvcCostNs, kVirtualCounterReadCostNs), so polling loops see
//     time pass;
//   - to the next deadline (a timed wait, a periodic host callback) when every
//     guest thread is waiting, i.e. idle time is skipped, not waited for;
//   - to the next vsync at a present (the boot program's NVN-HLE hook).
// With the single baton (SchedMode::Baton) the order of all of these is fixed
// by the guest's own execution, so equal inputs give equal runs. Periodic
// callbacks run on the guest thread that moved the clock past their deadline
// (or that found the process idle), never on a host timer thread.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

namespace lgp::rt {

enum class ClockMode : uint8_t { Host, Virtual };
const char* clock_mode_name(ClockMode mode);

// Virtual mode: guest time charged per svc and per CNTPCT/CNTVCT read. Rough
// costs of the real operations; only their being fixed matters.
inline constexpr uint64_t kVirtualSvcCostNs = 1000;
inline constexpr uint64_t kVirtualCounterReadCostNs = 100;

// Host steady clock in ns since the runtime started (host mode's guest time).
uint64_t host_clock_ns();

class GuestClock {
public:
    using Callback = std::function<void(uint64_t now_ns)>;
    static constexpr uint64_t kNever = ~uint64_t{0};

    explicit GuestClock(ClockMode mode) : mode_(mode) {}
    GuestClock(const GuestClock&) = delete;
    GuestClock& operator=(const GuestClock&) = delete;

    ClockMode mode() const { return mode_; }
    bool is_virtual() const { return mode_ == ClockMode::Virtual; }

    // Guest time in ns since the runtime started.
    uint64_t now_ns() const {
        return is_virtual() ? virtual_ns_.load(std::memory_order_acquire) : host_clock_ns();
    }
    // Virtual mode: guest time moves forward by ns (no-op in host mode).
    void charge(uint64_t ns);
    // Virtual mode: guest time becomes at least ns (no-op in host mode).
    void advance_to(uint64_t ns);

    // Virtual mode: cb(deadline) runs every period_ns of guest time, first at
    // now + period_ns. Callbacks run in deadline order, ties in registration
    // order. Register before the process runs.
    void add_periodic(uint64_t period_ns, Callback cb);
    // Earliest pending callback deadline, kNever without callbacks.
    uint64_t next_timer_ns() const { return next_timer_.load(std::memory_order_acquire); }
    // Runs every callback whose deadline has been reached, each with its own
    // deadline as the time. Callers serialise (Scheduler::run_timers).
    void run_due_timers();

private:
    struct Timer {
        uint64_t period = 0;
        uint64_t next = 0;
        Callback cb;
    };
    void update_next_locked();

    const ClockMode mode_;
    std::atomic<uint64_t> virtual_ns_{0};
    mutable std::mutex mutex_;
    std::vector<Timer> timers_;
    std::atomic<uint64_t> next_timer_{kNever};
};

}  // namespace lgp::rt
