// Wall time per guest thread and category (time_stats.hpp).
#include "lgp/runtime/time_stats.hpp"

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <memory>
#include <mutex>
#include <thread>

namespace lgp::rt {

namespace detail {
std::atomic<bool> g_time_stats_enabled{false};
}  // namespace detail

namespace {

constexpr size_t kCats = size_t(TimeCat::Count);

uint64_t clock_ns() {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count());
}

// set_time_stats_tick_us: the ticking clock's period (0: off) and value.
std::atomic<uint32_t> g_tick_us{0};
std::atomic<uint64_t> g_tick_now{0};

uint64_t now_ns() {
    if (g_tick_us.load(std::memory_order_relaxed) != 0)
        return g_tick_now.load(std::memory_order_relaxed);
    return clock_ns();
}

struct Record {
    uint64_t id = 0;
    int32_t priority = 0, core = 0;
    std::atomic<bool> alive{true};
    // Written by the owning thread only; read by reports. `seq` is odd while
    // the owner switches, so a report can take a consistent snapshot.
    std::atomic<uint32_t> seq{0};
    std::atomic<uint8_t> cur{uint8_t(TimeCat::Guest)};
    std::atomic<uint64_t> since{0};
    std::atomic<uint64_t> ns[kCats] = {};
    std::atomic<uint64_t> entries[kCats] = {};
    // Values at the previous report (registry mutex).
    uint64_t ns_base[kCats] = {};
    uint64_t entries_base[kCats] = {};
};

struct Source {
    std::string name;
    TimeSource read;
    uint64_t busy_base = 0, calls_base = 0;
};

struct Registry {
    std::mutex mutex;
    std::vector<std::shared_ptr<Record>> records;
    std::vector<Source> sources;
    uint64_t last_take = 0;
};

Registry& registry() {
    static Registry r;
    return r;
}

thread_local std::shared_ptr<Record> t_record;

}  // namespace

const char* time_cat_name(TimeCat cat) {
    switch (cat) {
    case TimeCat::Guest: return "guest";
    case TimeCat::Ready: return "ready";
    case TimeCat::Wait: return "wait";
    case TimeCat::Ipc: return "ipc";
    case TimeCat::Nvn: return "nvn";
    case TimeCat::NvnLock: return "nvn-lock";
    case TimeCat::Gpu: return "gpu";
    case TimeCat::Movie: return "movie";
    case TimeCat::Count: break;
    }
    return "?";
}

void set_time_stats_tick_us(uint32_t us) {
    static std::once_flag started;
    if (us == 0)
        return;
    g_tick_now.store(clock_ns(), std::memory_order_relaxed);
    g_tick_us.store(us, std::memory_order_relaxed);
    std::call_once(started, [] {
        std::thread([] {
            for (;;) {
                std::this_thread::sleep_for(std::chrono::microseconds(g_tick_us.load(std::memory_order_relaxed)));
                g_tick_now.store(clock_ns(), std::memory_order_relaxed);
            }
        }).detach();
    });
}

void enable_time_stats(bool on) {
    Registry& r = registry();
    std::lock_guard lock(r.mutex);
    r.last_take = now_ns();
    detail::g_time_stats_enabled.store(on, std::memory_order_relaxed);
}

void time_stats_attach(uint64_t id, int32_t priority, int32_t core) {
    if (!time_stats_enabled())
        return;
    auto rec = std::make_shared<Record>();
    rec->id = id;
    rec->priority = priority;
    rec->core = core;
    rec->since.store(now_ns(), std::memory_order_relaxed);
    Registry& r = registry();
    std::lock_guard lock(r.mutex);
    r.records.push_back(rec);
    t_record = std::move(rec);
}

void time_stats_detach() {
    if (!t_record)
        return;
    time_stats_switch(TimeCat::Wait);  // closes the running segment
    t_record->alive.store(false, std::memory_order_release);
    t_record.reset();
}

TimeCat time_stats_switch(TimeCat to) {
    Record* rec = t_record.get();
    if (rec == nullptr)
        return TimeCat::Guest;
    const uint64_t now = now_ns();
    const auto prev = TimeCat(rec->cur.load(std::memory_order_relaxed));
    const uint64_t since = rec->since.load(std::memory_order_relaxed);
    const uint32_t seq = rec->seq.load(std::memory_order_relaxed);
    rec->seq.store(seq + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    rec->ns[size_t(prev)].fetch_add(now - since, std::memory_order_relaxed);
    if (to != prev)
        rec->entries[size_t(to)].fetch_add(1, std::memory_order_relaxed);
    rec->since.store(now, std::memory_order_relaxed);
    rec->cur.store(uint8_t(to), std::memory_order_relaxed);
    rec->seq.store(seq + 2, std::memory_order_release);
    return prev;
}

void time_stats_add_source(std::string name, TimeSource source) {
    Registry& r = registry();
    std::lock_guard lock(r.mutex);
    Source s{std::move(name), std::move(source)};
    s.read(&s.busy_base, &s.calls_base);
    r.sources.push_back(std::move(s));
}

void time_stats_clear_sources() {
    Registry& r = registry();
    std::lock_guard lock(r.mutex);
    r.sources.clear();
}

TimeWindow time_stats_take() {
    Registry& r = registry();
    std::lock_guard lock(r.mutex);
    TimeWindow w;
    const uint64_t now = now_ns();
    w.wall_ns = now - r.last_take;
    r.last_take = now;
    std::vector<std::shared_ptr<Record>> keep;
    for (const auto& rec : r.records) {
        ThreadTimes t;
        t.id = rec->id;
        t.priority = rec->priority;
        t.core = rec->core;
        t.alive = rec->alive.load(std::memory_order_acquire);
        // The running segment counts up to now. Snapshot the counters
        // between two switches of the owner (seqlock, retried while it
        // switches), else a segment could count twice.
        size_t cur = 0;
        uint64_t since = 0, snap_ns[kCats], snap_entries[kCats];
        for (;;) {
            const uint32_t s1 = rec->seq.load(std::memory_order_acquire);
            if ((s1 & 1) == 0) {
                cur = rec->cur.load(std::memory_order_relaxed);
                since = rec->since.load(std::memory_order_relaxed);
                for (size_t c = 0; c < kCats; ++c) {
                    snap_ns[c] = rec->ns[c].load(std::memory_order_relaxed);
                    snap_entries[c] = rec->entries[c].load(std::memory_order_relaxed);
                }
                std::atomic_thread_fence(std::memory_order_acquire);
                if (rec->seq.load(std::memory_order_relaxed) == s1)
                    break;
            }
            std::this_thread::yield();
        }
        bool any = false;
        for (size_t c = 0; c < kCats; ++c) {
            uint64_t ns = snap_ns[c];
            if (t.alive && c == cur && now > since)
                ns += now - since;
            const uint64_t entries = snap_entries[c];
            t.ns[c] = ns > rec->ns_base[c] ? ns - rec->ns_base[c] : 0;
            t.entries[c] = entries - rec->entries_base[c];
            rec->ns_base[c] = std::max(ns, rec->ns_base[c]);
            rec->entries_base[c] = entries;
            any |= t.ns[c] != 0 || t.entries[c] != 0;
        }
        if (any)
            w.threads.push_back(t);
        if (t.alive || any)
            keep.push_back(rec);
    }
    r.records = std::move(keep);
    for (Source& s : r.sources) {
        uint64_t busy = 0, calls = 0;
        s.read(&busy, &calls);
        w.sources.push_back({s.name, busy - s.busy_base, calls - s.calls_base});
        s.busy_base = busy;
        s.calls_base = calls;
    }
    return w;
}

std::vector<std::string> time_stats_report(uint64_t first_frame, uint64_t frames) {
    const TimeWindow w = time_stats_take();
    std::vector<std::string> out;
    char line[512];
    const double wall_ms = double(w.wall_ns) / 1e6;
    // Per frame if frames are known, else per window.
    const double div = frames != 0 ? double(frames) : 1.0;
    const char* unit = frames != 0 ? "ms/frame" : "ms";
    if (frames != 0)
        std::snprintf(line, sizeof line,
                      "time: frames %" PRIu64 "-%" PRIu64 ": %.2f s wall, %.1f fps (%.1f ms per frame)", first_frame,
                      first_frame + frames - 1, wall_ms / 1000.0, double(frames) * 1000.0 / wall_ms, wall_ms / div);
    else
        std::snprintf(line, sizeof line, "time: %.2f s wall", wall_ms / 1000.0);
    out.emplace_back(line);
    int idle = 0;
    for (const ThreadTimes& t : w.threads) {
        uint64_t active = 0;
        for (size_t c = 0; c < kCats; ++c)
            if (TimeCat(c) != TimeCat::Wait)
                active += t.ns[c];
        if (active * 200 < w.wall_ns) {
            ++idle;
            continue;
        }
        std::string s;
        std::snprintf(line, sizeof line, "time:   thread %" PRIu64 " (prio %d, core %d%s):", t.id, t.priority, t.core,
                      t.alive ? "" : ", ended");
        s = line;
        for (size_t c = 0; c < kCats; ++c) {
            std::snprintf(line, sizeof line, "%s %s %.2f", c == 0 ? "" : ",", time_cat_name(TimeCat(c)),
                          double(t.ns[c]) / 1e6 / div);
            s += line;
        }
        s += " ";
        s += unit;
        // How often the thread waited: many short readies point at handover
        // costs, few long ones at another thread holding the slot.
        if (const uint64_t ready = t.entries[size_t(TimeCat::Ready)], waits = t.entries[size_t(TimeCat::Wait)];
            ready != 0 || waits != 0) {
            std::snprintf(line, sizeof line, "; readies %.1f (%.0f us each), waits %.1f (%.0f us each)",
                          double(ready) / div,
                          ready != 0 ? double(t.ns[size_t(TimeCat::Ready)]) / 1e3 / double(ready) : 0.0,
                          double(waits) / div,
                          waits != 0 ? double(t.ns[size_t(TimeCat::Wait)]) / 1e3 / double(waits) : 0.0);
            s += line;
        }
        const uint64_t gpu = t.entries[size_t(TimeCat::Gpu)], ipc = t.entries[size_t(TimeCat::Ipc)];
        if (gpu != 0 || ipc != 0) {
            std::snprintf(line, sizeof line, "; gpu calls %.1f (%.0f us each), ipc requests %.1f (%.0f us each)",
                          double(gpu) / div, gpu != 0 ? double(t.ns[size_t(TimeCat::Gpu)]) / 1e3 / double(gpu) : 0.0,
                          double(ipc) / div, ipc != 0 ? double(t.ns[size_t(TimeCat::Ipc)]) / 1e3 / double(ipc) : 0.0);
            s += line;
        }
        out.push_back(std::move(s));
    }
    if (idle != 0) {
        std::snprintf(line, sizeof line, "time:   %d more threads (almost) only waited", idle);
        out.emplace_back(line);
    }
    for (const SourceTimes& s : w.sources) {
        std::snprintf(line, sizeof line, "time:   %s: busy %.2f %s (%.0f %%), %.1f calls (%.0f us each)",
                      s.name.c_str(), double(s.busy_ns) / 1e6 / div, unit,
                      w.wall_ns != 0 ? 100.0 * double(s.busy_ns) / double(w.wall_ns) : 0.0, double(s.calls) / div,
                      s.calls != 0 ? double(s.busy_ns) / 1e3 / double(s.calls) : 0.0);
        out.emplace_back(line);
    }
    return out;
}

std::vector<std::string> time_stats_current() {
    std::vector<std::pair<uint64_t, std::string>> rows;
    {
        Registry& r = registry();
        std::lock_guard lock(r.mutex);
        const uint64_t now = now_ns();
        for (const auto& rec : r.records) {
            if (!rec->alive.load(std::memory_order_acquire))
                continue;
            size_t cur = 0;
            uint64_t since = 0;
            for (;;) {  // consistent (cur, since) between two switches, as in time_stats_take
                const uint32_t s1 = rec->seq.load(std::memory_order_acquire);
                if ((s1 & 1) == 0) {
                    cur = rec->cur.load(std::memory_order_relaxed);
                    since = rec->since.load(std::memory_order_relaxed);
                    std::atomic_thread_fence(std::memory_order_acquire);
                    if (rec->seq.load(std::memory_order_relaxed) == s1)
                        break;
                }
                std::this_thread::yield();
            }
            const uint64_t age = now > since ? now - since : 0;
            char line[96];
            std::snprintf(line, sizeof(line), "thread %llu: %s for %.2f s", (unsigned long long)rec->id,
                          time_cat_name(TimeCat(cur)), double(age) / 1e9);
            rows.emplace_back(age, line);
        }
    }
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::vector<std::string> out;
    for (auto& row : rows)
        out.push_back(std::move(row.second));
    return out;
}

}  // namespace lgp::rt
