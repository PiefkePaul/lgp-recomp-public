// Virtual guest clock (clock.hpp): with SchedMode::Baton, a process with
// sleeps, timeouts, a polling loop on the counter and a periodic host
// callback that signals an event runs identically every time, guest time
// only moves with the guest's work and idle time is skipped instead of waited.
// Same style as test_threads_svc.cpp: hand-written guest functions in the
// form of lifted code, results recorded on the host.
#include <chrono>
#include <cstring>
#include <mutex>
#include <utility>
#include <vector>

#include "check.hpp"
#include "lgp/lifted/lifted.hpp"
#include "lgp/runtime/kernel.hpp"
#include "lgp/runtime/system.hpp"

using namespace lgp::rt;
namespace kr = lgp::rt::kernel_result;

namespace {

constexpr uint64_t kBase = 0x0800'0000;
constexpr uint64_t kData = kBase + 0x10000;  // RW page for svc pointer arguments
constexpr uint64_t kStacks = kBase + 0x20000;
constexpr uint64_t kFnBase = kBase + 0x1000;
constexpr uint64_t kMs = 1'000'000;
constexpr uint64_t kTimerPeriod = 5 * kMs;

constexpr int kWorkers = 3;
constexpr uint64_t kSleep[kWorkers] = {100 * kMs, 250 * kMs, 400 * kMs};
constexpr int32_t kPriority[kWorkers] = {40, 44, 50};
constexpr int kSleeps = 20;
constexpr int kEventWaits = 30;

// One record per observation: who (worker 0..2, 9 = event waiter, 10 = main,
// 100 = host callback) and the guest tick (callback: its deadline in ns).
using Trace = std::vector<std::pair<int, uint64_t>>;
std::mutex g_trace_mutex;
Trace g_trace;
uint64_t g_spins = 0;
uint64_t g_cv_result = 0;
Handle g_event_readable = kInvalidHandle;

void record(int who, uint64_t value) {
    std::lock_guard lock(g_trace_mutex);
    g_trace.emplace_back(who, value);
}

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

uint64_t tick_ns(uint64_t tick) { return tick / 12 * 625 + tick % 12 * 625 / 12; }

// x0 = worker index: kSleeps sleeps of kSleep[index], the tick after each.
void sleeper(lgp::CpuState* ctx) {
    const int index = static_cast<int>(ctx->x[0]);
    for (int i = 0; i < kSleeps; ++i) {
        set_args(ctx, {kSleep[index]});
        SVC(0x0b);  // SleepThread
        SVC(0x1e);  // GetSystemTick
        record(index, ctx->x[0]);
    }
    SVC(0x0a);
}

// x0 = readable event: kEventWaits waits for the host callback's signal.
void event_waiter(lgp::CpuState* ctx) {
    const uint64_t event = ctx->x[0];
    for (int i = 0; i < kEventWaits; ++i) {
        lgp_mem_write32(kData + 0x100, static_cast<uint32_t>(event));
        set_args(ctx, {0, kData + 0x100, 1, ~uint64_t{0}});
        SVC(0x18);  // WaitSynchronization, forever
        set_args(ctx, {event});
        SVC(0x17);  // ResetSignal
        SVC(0x1e);
        record(9, ctx->x[0]);
    }
    SVC(0x0a);
}

void vc_main(lgp::CpuState* ctx) {
    SVC(0x1e);
    record(10, ctx->x[0]);
    for (int w = 0; w <= kWorkers; ++w) {
        const bool waiter = w == kWorkers;
        set_args(ctx, {0, fn_va(waiter ? 2 : 1), waiter ? g_event_readable : static_cast<uint64_t>(w),
                       kStacks + 0x2000 * static_cast<uint64_t>(w + 1), waiter ? 30u : uint64_t(kPriority[w]),
                       static_cast<uint64_t>(-2)});
        SVC(0x08);  // CreateThread
        const uint64_t handle = ctx->x[1];
        lgp_mem_write32(kData + 4 * static_cast<uint64_t>(w), static_cast<uint32_t>(handle));
        set_args(ctx, {handle});
        SVC(0x09);  // StartThread
    }
    for (int w = 0; w <= kWorkers; ++w) {
        set_args(ctx, {0, kData + 4 * static_cast<uint64_t>(w), 1, ~uint64_t{0}});
        SVC(0x18);  // wait for the thread to end
    }
    // Polling the counter: every read costs guest time, so this ends.
    SVC(0x1e);
    const uint64_t until = ctx->x[0] + 2 * kCounterFrequency / 1000;  // + 2 ms
    uint64_t spins = 0;
    do {
        SVC(0x1e);
        ++spins;
    } while (ctx->x[0] < until);
    g_spins = spins;
    record(10, ctx->x[0]);
    // A condition variable nobody signals: times out after 1.5 ms of guest time.
    lgp_mem_write32(kData + 0x200, 0);
    set_args(ctx, {kData + 0x200, kData + 0x204, 0x1234, 1'500'000});
    SVC(0x1c);  // WaitProcessWideKeyAtomic
    g_cv_result = ctx->x[0];
    SVC(0x1e);
    record(10, ctx->x[0]);
    SVC(0x07);
}

struct RunResult {
    ProcessExit exit;
    Trace trace;
    uint64_t end_ns = 0;   // guest time at the end
    double wall_s = 0;     // host time the run took
};

RunResult run(SchedMode mode) {
    {
        std::lock_guard lock(g_trace_mutex);
        g_trace.clear();
    }
    g_spins = 0;
    g_cv_result = 0;
    ProcessConfig c;
    c.custom_layout = true;
    c.layout = Layout::packed(kBase, 0x10'0000, 0x10'0000, 0x10'0000, 0x10'0000, 16 * 0x10'0000);
    c.sched_mode = mode;
    c.clock_mode = ClockMode::Virtual;
    Kernel k(c);
    CHECK(k.map_owned(kData, kPageSize, MemState::CodeData, kPermRW).ok());
    CHECK(k.map_owned(kStacks, 0x10000, MemState::Stack, kPermRW).ok());
    Handle writable = kInvalidHandle;
    CHECK(k.scheduler().create_event(&writable, &g_event_readable).ok());
    const auto event = k.handles().get_as<KReadableEvent>(g_event_readable);
    CHECK(event != nullptr);
    // What boot.cpp does for hid/audout: a periodic host callback that
    // signals the guest.
    k.clock().add_periodic(kTimerPeriod, [&](uint64_t now) {
        record(100, now);
        k.scheduler().signal_readable(*event);
    });
    g_fns[0] = &vc_main;
    g_fns[1] = &sleeper;
    g_fns[2] = &event_waiter;
    set_indirect_resolver(&resolve);
    const auto t0 = std::chrono::steady_clock::now();
    RunResult r;
    r.exit = k.run({fn_va(0), kStacks + 0x10000, 44, 0});
    r.wall_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    r.end_ns = k.clock().now_ns();
    set_indirect_resolver(nullptr);
    set_active_kernel(nullptr);
    std::lock_guard lock(g_trace_mutex);
    r.trace = g_trace;
    return r;
}

size_t count(const Trace& t, int who) {
    size_t n = 0;
    for (const auto& [w, v] : t)
        n += w == who;
    return n;
}

void test_reproducible() {
    const RunResult first = run(SchedMode::Baton);
    CHECK(first.exit.reason == ProcessExit::Reason::ExitProcess);
    for (int w = 0; w < kWorkers; ++w)
        CHECK_EQ(count(first.trace, w), static_cast<size_t>(kSleeps));
    CHECK_EQ(count(first.trace, 9), static_cast<size_t>(kEventWaits));
    CHECK_EQ(count(first.trace, 10), 3u);
    CHECK_EQ(g_cv_result, kr::kTimedOut.raw);
    const uint64_t spins = g_spins;
    // 2 ms at 1 us per GetSystemTick (kVirtualSvcCostNs).
    CHECK(spins >= 1900 && spins <= 2001);

    // Guest time: the longest sleeper, 20 x 400 ms, plus small svc costs.
    const uint64_t longest = kSleeps * kSleep[kWorkers - 1];
    CHECK(first.end_ns >= longest && first.end_ns < longest + 50 * kMs);
    // ... of which the host waited for none.
    std::printf("  guest %.3f s in %.3f s host time\n", double(first.end_ns) / 1e9, first.wall_s);
    CHECK(first.wall_s < 4.0);

    // Every sleep lasts its time plus the svc costs around it, never less.
    for (int w = 0; w < kWorkers; ++w) {
        uint64_t last = 0;
        for (const auto& [who, tick] : first.trace) {
            if (who != w)
                continue;
            const uint64_t ns = tick_ns(tick);
            if (last != 0) {
                CHECK(ns - last >= kSleep[w]);
                CHECK(ns - last < kSleep[w] + kMs);
            }
            last = ns;
        }
    }
    // The host callback runs exactly at its deadlines.
    uint64_t callbacks = 0, expected = kTimerPeriod;
    for (const auto& [who, when] : first.trace)
        if (who == 100) {
            CHECK_EQ(when, expected);
            expected += kTimerPeriod;
            ++callbacks;
        }
    CHECK(callbacks >= longest / kTimerPeriod);

    // The same run again: the same trace, tick for tick.
    for (int i = 0; i < 3; ++i) {
        const RunResult again = run(SchedMode::Baton);
        CHECK(again.exit.reason == ProcessExit::Reason::ExitProcess);
        CHECK(again.trace == first.trace);
        CHECK_EQ(again.end_ns, first.end_ns);
        CHECK_EQ(g_spins, spins);
    }
}

// CoreBaton runs threads of different cores in parallel: not reproducible,
// but the virtual clock must work there too.
void test_core_baton() {
    const RunResult r = run(SchedMode::CoreBaton);
    CHECK(r.exit.reason == ProcessExit::Reason::ExitProcess);
    for (int w = 0; w < kWorkers; ++w)
        CHECK_EQ(count(r.trace, w), static_cast<size_t>(kSleeps));
    CHECK_EQ(count(r.trace, 9), static_cast<size_t>(kEventWaits));
    CHECK_EQ(g_cv_result, kr::kTimedOut.raw);
}

// Without a kernel, and in host mode, system_tick() follows the host clock.
void test_host_mode() {
    const uint64_t a = system_tick();
    ProcessConfig c;
    c.custom_layout = true;
    c.layout = Layout::packed(kBase, 0x10'0000, 0x10'0000, 0x10'0000, 0x10'0000, 16 * 0x10'0000);
    Kernel k(c);
    CHECK(!k.clock().is_virtual());
    k.clock().charge(1'000'000'000);  // no effect in host mode
    CHECK(k.clock().now_ns() < host_clock_ns() + 1);
    CHECK(system_tick() >= a);
}

}  // namespace

int main() {
    RUN(test_reproducible);
    RUN(test_core_baton);
    RUN(test_host_mode);
    return finish();
}
