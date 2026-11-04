#include "lgp/runtime/clock.hpp"

#include <algorithm>
#include <chrono>

namespace lgp::rt {
namespace {

const auto g_start = std::chrono::steady_clock::now();

}  // namespace

const char* clock_mode_name(ClockMode mode) {
    switch (mode) {
    case ClockMode::Host: return "host";
    case ClockMode::Virtual: return "virtual";
    }
    return "?";
}

uint64_t host_clock_ns() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - g_start).count());
}

void GuestClock::charge(uint64_t ns) {
    if (is_virtual())
        virtual_ns_.fetch_add(ns, std::memory_order_acq_rel);
}

void GuestClock::advance_to(uint64_t ns) {
    if (!is_virtual())
        return;
    uint64_t cur = virtual_ns_.load(std::memory_order_acquire);
    while (cur < ns && !virtual_ns_.compare_exchange_weak(cur, ns, std::memory_order_acq_rel))
        ;
}

void GuestClock::add_periodic(uint64_t period_ns, Callback cb) {
    std::lock_guard lock(mutex_);
    timers_.push_back({std::max<uint64_t>(period_ns, 1), now_ns() + std::max<uint64_t>(period_ns, 1), std::move(cb)});
    update_next_locked();
}

void GuestClock::update_next_locked() {
    uint64_t next = kNever;
    for (const Timer& t : timers_)
        next = std::min(next, t.next);
    next_timer_.store(next, std::memory_order_release);
}

void GuestClock::run_due_timers() {
    for (;;) {
        const uint64_t now = now_ns();
        Callback* cb = nullptr;
        uint64_t when = 0;
        {
            std::lock_guard lock(mutex_);
            Timer* due = nullptr;
            for (Timer& t : timers_)
                if (t.next <= now && (due == nullptr || t.next < due->next))
                    due = &t;
            if (due == nullptr)
                return;
            when = due->next;
            due->next += due->period;
            cb = &due->cb;
            update_next_locked();
        }
        // timers_ does not change while the process runs (add_periodic comes
        // first), so the callback stays valid without the lock.
        (*cb)(when);
    }
}

}  // namespace lgp::rt
