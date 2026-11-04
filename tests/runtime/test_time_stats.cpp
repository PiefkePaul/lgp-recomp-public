// P6 time accounting (lgp/runtime/time_stats.hpp): categories, nesting,
// external sources, and the scheduler's Ready/Wait bookkeeping for guest
// threads under the single baton and in the free mode.
#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

#include "check.hpp"
#include "lgp/lifted/lifted.hpp"
#include "lgp/runtime/host_work.hpp"
#include "lgp/runtime/kernel.hpp"
#include "lgp/runtime/time_stats.hpp"

using namespace lgp::rt;

namespace {

constexpr uint64_t kBase = 0x0800'0000;
constexpr uint64_t kStacks = kBase + 0x20000;
constexpr uint64_t kFnBase = kBase + 0x1000;
constexpr uint64_t kMs = 1'000'000;

#define SVC(n)            \
    do {                  \
        ctx->pc = 0;      \
        LGP_SVC(n);       \
        LGP_HALT_CHECK(); \
    } while (0)

void set_args(lgp::CpuState* ctx, std::initializer_list<uint64_t> args) {
    size_t i = 0;
    for (uint64_t a : args)
        ctx->x[i++] = a;
}

lgp::LiftedFn g_fns[4];
lgp::LiftedFn resolve(uint64_t va) {
    if (va < kFnBase || (va - kFnBase) % 0x10 != 0 || (va - kFnBase) / 0x10 >= 4)
        return nullptr;
    return g_fns[(va - kFnBase) / 0x10];
}
uint64_t fn_va(int index) { return kFnBase + 0x10 * static_cast<uint64_t>(index); }

void spin(std::chrono::milliseconds d) {
    const auto end = std::chrono::steady_clock::now() + d;
    while (std::chrono::steady_clock::now() < end) {
    }
}

uint64_t ns_of(const ThreadTimes& t, TimeCat c) { return t.ns[size_t(c)]; }

// --- host thread, no scheduler ------------------------------------------------------

void test_categories() {
    CHECK(time_stats_switch(TimeCat::Gpu) == TimeCat::Guest);  // not attached: no-op
    enable_time_stats(true);
    std::atomic<bool> in_gpu{false}, seen{false};
    std::thread th([&] {
        time_stats_attach(7, 44, 1);
        {
            TimeScope nvn(TimeCat::Nvn);
            {
                TimeScope gpu(TimeCat::Gpu);
                in_gpu = true;
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                while (!seen.load())
                    std::this_thread::yield();
            }
            TimeScope gpu(TimeCat::Gpu);
        }
        time_stats_detach();
    });
    uint64_t busy = 0, calls = 0;
    time_stats_add_source("fake gpu", [&](uint64_t* b, uint64_t* c) {
        *b = busy;
        *c = calls;
    });
    // Hang reports: what a live thread is inside right now.
    while (!in_gpu.load())
        std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    const std::vector<std::string> now = time_stats_current();
    seen = true;
    CHECK_EQ(now.size(), size_t{1});
    if (!now.empty())
        CHECK(now[0].rfind("thread 7: gpu for ", 0) == 0);
    th.join();
    CHECK(time_stats_current().empty());  // ended threads are not listed
    busy = 5 * kMs;
    calls = 3;
    const TimeWindow w = time_stats_take();
    const ThreadTimes* t = nullptr;
    for (const ThreadTimes& x : w.threads)
        if (x.id == 7)
            t = &x;
    CHECK(t != nullptr);
    if (t != nullptr) {
        CHECK(!t->alive);
        CHECK_EQ(t->priority, 44);
        CHECK_EQ(t->core, 1);
        CHECK(ns_of(*t, TimeCat::Gpu) >= 15 * kMs);
        CHECK_EQ(t->entries[size_t(TimeCat::Gpu)], uint64_t{2});
        CHECK_EQ(t->entries[size_t(TimeCat::Nvn)], uint64_t{3});  // entered once, returned to twice
        CHECK(ns_of(*t, TimeCat::Nvn) < ns_of(*t, TimeCat::Gpu));
    }
    CHECK_EQ(w.sources.size(), size_t{1});
    if (!w.sources.empty()) {
        CHECK_EQ(w.sources[0].busy_ns, 5 * kMs);
        CHECK_EQ(w.sources[0].calls, uint64_t{3});
    }
    // The ended thread is reported once, then dropped.
    const TimeWindow again = time_stats_take();
    for (const ThreadTimes& x : again.threads)
        CHECK(x.id != 7);
    CHECK(!time_stats_report(1, 10).empty());
    time_stats_clear_sources();
    enable_time_stats(false);
}

// --- guest threads ------------------------------------------------------------------

void worker(lgp::CpuState* ctx) {
    spin(std::chrono::milliseconds(5));
    SVC(0x0a);  // ExitThread
}

// Starts a worker of the same priority and core, runs 80 ms without an svc
// (the worker waits for the baton meanwhile), then sleeps 30 ms.
void main_fn(lgp::CpuState* ctx) {
    set_args(ctx, {0, fn_va(1), 0, kStacks + 0x4000, 44, 0});
    SVC(0x08);  // CreateThread
    set_args(ctx, {ctx->x[1]});
    SVC(0x09);  // StartThread
    // The worker is ready now, but its host thread only starts counting once
    // it runs (slow on loaded CI runners): give it time to get there.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    spin(std::chrono::milliseconds(30));
    set_args(ctx, {30 * kMs});
    SVC(0x0b);  // SleepThread
    SVC(0x0a);
}

void run_process(SchedMode mode) {
    ProcessConfig c;
    c.custom_layout = true;
    c.layout = Layout::packed(kBase, 0x10'0000, 0x10'0000, 0x10'0000, 0x10'0000, 16 * 0x10'0000);
    c.sched_mode = mode;
    Kernel k(c);
    CHECK(k.map_owned(kStacks, 0x10000, MemState::Stack, kPermRW).ok());
    g_fns[0] = &main_fn;
    g_fns[1] = &worker;
    set_indirect_resolver(&resolve);
    const ProcessExit e = k.run({fn_va(0), kStacks + 0x10000, 44, 0});
    set_indirect_resolver(nullptr);
    set_active_kernel(nullptr);
    CHECK(e.reason == ProcessExit::Reason::AllThreadsExited);
}

void test_scheduler(SchedMode mode) {
    enable_time_stats(true);
    time_stats_take();
    uint64_t wake_ns0 = 0, wakes0 = 0;
    scheduler_wake_latency(&wake_ns0, &wakes0);
    run_process(mode);
    const TimeWindow w = time_stats_take();
    uint64_t wake_ns1 = 0, wakes1 = 0;
    scheduler_wake_latency(&wake_ns1, &wakes1);
    enable_time_stats(false);
    // The worker got the baton from the sleeping main thread (and back), or
    // in free mode was let run when started: each counts as a handover.
    CHECK(wakes1 > wakes0);
    CHECK(wake_ns1 >= wake_ns0);
    CHECK_EQ(w.threads.size(), size_t{2});
    if (w.threads.size() != 2)
        return;
    // Threads are registered when they start: the main thread first.
    const ThreadTimes& main_t = w.threads[0];
    const ThreadTimes& work_t = w.threads[1];
    CHECK(ns_of(main_t, TimeCat::Guest) >= 25 * kMs);
    CHECK(ns_of(main_t, TimeCat::Wait) >= 25 * kMs);  // SleepThread
    CHECK(ns_of(work_t, TimeCat::Guest) >= 4 * kMs);
    if (mode == SchedMode::Baton) {
        CHECK(ns_of(work_t, TimeCat::Ready) >= 25 * kMs);  // waited for the baton
    } else {
        CHECK(ns_of(work_t, TimeCat::Ready) < 15 * kMs);   // ran beside the main thread
    }
}

// HostWorkScope (host_work.hpp): in the core baton mode a worker of the
// same core runs while the main thread does host work.
std::atomic<int64_t> g_worker_ran_ns{0};
std::atomic<int64_t> g_host_work_end_ns{0};
bool g_force = false;  // HostWorkScope(true): an HLE request waiting for a server (ldn)

int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void marking_worker(lgp::CpuState* ctx) {
    g_worker_ran_ns = now_ns();
    SVC(0x0a);
}

void host_work_main(lgp::CpuState* ctx) {
    set_args(ctx, {0, fn_va(2), 0, kStacks + 0x4000, 44, 0});
    SVC(0x08);
    set_args(ctx, {ctx->x[1]});
    SVC(0x09);
    {
        HostWorkScope away(g_force);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        g_host_work_end_ns = now_ns();
    }
    set_args(ctx, {10 * kMs});
    SVC(0x0b);
    SVC(0x0a);
}

bool worker_ran_during_host_work(bool release, bool force = false) {
    enable_host_work_release(release);
    g_force = force;
    g_worker_ran_ns = 0;
    g_fns[0] = &host_work_main;
    g_fns[2] = &marking_worker;
    ProcessConfig c;
    c.custom_layout = true;
    c.layout = Layout::packed(kBase, 0x10'0000, 0x10'0000, 0x10'0000, 0x10'0000, 16 * 0x10'0000);
    c.sched_mode = SchedMode::CoreBaton;
    Kernel k(c);
    CHECK(k.map_owned(kStacks, 0x10000, MemState::Stack, kPermRW).ok());
    set_indirect_resolver(&resolve);
    const ProcessExit e = k.run({fn_va(0), kStacks + 0x10000, 44, 0});
    set_indirect_resolver(nullptr);
    set_active_kernel(nullptr);
    enable_host_work_release(false);
    CHECK(e.reason == ProcessExit::Reason::AllThreadsExited);
    CHECK(g_worker_ran_ns.load() != 0);
    return g_worker_ran_ns.load() < g_host_work_end_ns.load();
}

}  // namespace

void test_host_work_release() {
    CHECK(!worker_ran_during_host_work(false));  // the slot stays taken
    CHECK(worker_ran_during_host_work(true));
    CHECK(worker_ran_during_host_work(false, true));  // force: whatever the switch says
}

void test_scheduler_baton() { test_scheduler(SchedMode::Baton); }
void test_scheduler_free() { test_scheduler(SchedMode::Free); }

int main() {
    RUN(test_categories);
    RUN(test_scheduler_baton);
    RUN(test_scheduler_free);
    RUN(test_host_work_release);
    return finish();
}
