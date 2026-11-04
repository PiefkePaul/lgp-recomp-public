// Watches a native wait loop that polls the device (wgpuDevicePoll) until a
// condition holds: frames in flight, compute write-back. Such a loop has no
// time limit, and wgpuDevicePoll(device, true, ...) itself blocks until the
// submitted work is done; if the device stops completing work (lost device,
// driver hang) the thread would wait for ever without a word. A watchdog
// thread therefore checks every active watch, also while its owner is
// blocked inside the poll, and only logs: a warning once the wait has lasted
// 5 s, again at 10 s, 20 s, 40 s and so on, and one line when a warned wait
// ends. It never breaks the loop or changes what the loop waits for.
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

#include "lgp/runtime/log.hpp"

namespace lgp::gpu::webgpu {

class PollWatch {
public:
    static constexpr uint64_t kFirstWarnMs = 5000;

    // first_warn_ms: tests only.
    explicit PollWatch(const char* what, uint64_t first_warn_ms = kFirstWarnMs)
        : what_(what), start_(std::chrono::steady_clock::now()), next_ms_(first_warn_ms) {
        Watchdog& w = watchdog();
        {
            std::lock_guard lock(w.mutex);
            w.active.push_back(this);
            w.tick_ms = std::clamp<uint64_t>(first_warn_ms / 4, 1, 250);
            if (!w.started) {
                w.started = true;
                std::thread([&w] { w.run(); }).detach();  // lives as long as the process
            }
        }
        w.cv.notify_all();
    }
    ~PollWatch() {
        Watchdog& w = watchdog();
        {
            std::lock_guard lock(w.mutex);  // the watchdog does not look at this watch any more
            w.active.erase(std::find(w.active.begin(), w.active.end(), this));
        }
        if (warned_.load(std::memory_order_relaxed) != 0)
            rt::log(rt::LogLevel::Warn, "nvn webgpu: %s: device poll loop ended after %.1f s", what_,
                    double(elapsed_ms()) / 1000.0);
    }
    PollWatch(const PollWatch&) = delete;
    PollWatch& operator=(const PollWatch&) = delete;

    // Once per loop iteration (the watchdog covers the time inside a poll).
    void check() {
        Watchdog& w = watchdog();
        std::lock_guard lock(w.mutex);
        report();
    }

    // The schedule: whether a warning is due after `ms` milliseconds of
    // waiting when the next one is due at *next_ms (first 5000); moves
    // *next_ms on by doublings past ms when it is.
    static bool due(uint64_t* next_ms, uint64_t ms) {
        if (ms < *next_ms)
            return false;
        while (*next_ms <= ms) *next_ms *= 2;
        return true;
    }
    uint32_t warnings() const { return warned_.load(std::memory_order_relaxed); }

private:
    struct Watchdog {
        std::mutex mutex;
        std::condition_variable cv;
        std::vector<PollWatch*> active;
        bool started = false;
        uint64_t tick_ms = 250;
        void run() {
            std::unique_lock lock(mutex);
            for (;;) {
                cv.wait(lock, [this] { return !active.empty(); });
                cv.wait_for(lock, std::chrono::milliseconds(tick_ms));
                for (PollWatch* p : active) p->report();
            }
        }
    };
    // Never destroyed: the detached thread may still run at process exit.
    static Watchdog& watchdog() {
        static Watchdog* w = new Watchdog;
        return *w;
    }

    // With the watchdog's mutex held.
    void report() {
        const uint64_t ms = elapsed_ms();
        if (!due(&next_ms_, ms))
            return;
        warned_.fetch_add(1, std::memory_order_relaxed);
        rt::log(rt::LogLevel::Warn,
                    "nvn webgpu: %s: still polling the device after %.1f s (GPU work does not complete; device "
                    "lost or driver hang?)",
                    what_, double(ms) / 1000.0);
    }
    uint64_t elapsed_ms() const {
        return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                              start_)
                            .count());
    }

    const char* what_;
    std::chrono::steady_clock::time_point start_;
    uint64_t next_ms_;
    std::atomic<uint32_t> warned_{0};
};

}  // namespace lgp::gpu::webgpu
