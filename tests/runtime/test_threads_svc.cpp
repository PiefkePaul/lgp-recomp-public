// P3.2: semantics of the thread and synchronization svcs, driven by
// hand-written functions in the style of lifted code (LGP_SVC with x0..x7 in
// CpuState), run as guest threads by Kernel::run. Results are recorded in
// g_out and checked after the process has ended (the host threads are joined
// by then, so plain variables are fine).
#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <thread>

#include "check.hpp"
#include "lgp/lifted/lifted.hpp"
#include "lgp/runtime/kernel.hpp"

using namespace lgp::rt;
namespace kr = lgp::rt::kernel_result;

namespace {

constexpr uint64_t kBase = 0x0800'0000;
constexpr uint64_t kData = kBase + 0x10000;  // RW page for svc pointer arguments
constexpr uint64_t kStacks = kBase + 0x20000;
constexpr uint64_t kFnBase = kBase + 0x1000;  // fake code addresses of the functions below
constexpr uint32_t kSelf = kPseudoHandleCurrentThread;

uint64_t g_out[64];
// "virtual" on the command line: the whole suite on the virtual guest clock.
ClockMode g_clock = ClockMode::Host;

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

// Function table: guest address kFnBase + 0x10 * index.
lgp::LiftedFn g_fns[16];
lgp::LiftedFn resolve(uint64_t va) {
    if (va < kFnBase || (va - kFnBase) % 0x10 != 0 || (va - kFnBase) / 0x10 >= 16)
        return nullptr;
    return g_fns[(va - kFnBase) / 0x10];
}
uint64_t fn_va(int index) { return kFnBase + 0x10 * static_cast<uint64_t>(index); }

// CreateThread(entry, arg, own stack, priority, core) + StartThread; returns the handle in x1.
#define SPAWN(index, arg, stack_slot, prio, core)                                                            \
    do {                                                                                                     \
        set_args(ctx, {0, fn_va(index), (arg), kStacks + 0x2000 * ((stack_slot) + 1), (prio),                 \
                       static_cast<uint64_t>(static_cast<int64_t>(core))});                                   \
        SVC(0x08);                                                                                           \
        g_out[60] = ctx->x[0];                                                                               \
        ctx->x[19] = ctx->x[1];                                                                              \
        set_args(ctx, {ctx->x[19]});                                                                         \
        SVC(0x09);                                                                                           \
    } while (0)

// host(k) runs on a host thread of its own while the process runs.
ProcessExit run(int main_index, SchedMode mode, const std::function<void(Kernel&)>& host = {}) {
    std::memset(g_out, 0, sizeof(g_out));
    ProcessConfig c;
    c.custom_layout = true;
    c.layout = Layout::packed(kBase, 0x10'0000, 0x10'0000, 0x10'0000, 0x10'0000, 16 * 0x10'0000);
    c.sched_mode = mode;
    c.clock_mode = g_clock;
    Kernel k(c);
    CHECK(k.map_owned(kData, kPageSize, MemState::CodeData, kPermRW).ok());
    CHECK(k.map_owned(kStacks, 0x10000, MemState::Stack, kPermRW).ok());
    set_indirect_resolver(&resolve);
    std::thread controller;
    if (host)
        controller = std::thread([&] { host(k); });
    const ProcessExit e = k.run({fn_va(main_index), kStacks + 0x10000, 44, 0});
    if (controller.joinable())
        controller.join();
    set_indirect_resolver(nullptr);
    set_active_kernel(nullptr);
    return e;
}

// --- events, WaitSynchronization --------------------------------------------------

void ev_worker(lgp::CpuState* ctx) {  // x0 = writable event
    set_args(ctx, {ctx->x[0]});
    SVC(0x11);  // SignalEvent
    g_out[10] = ctx->x[0];
    SVC(0x0a);
}

void ev_main(lgp::CpuState* ctx) {
    SVC(0x45);  // CreateEvent
    g_out[0] = ctx->x[0];
    const uint64_t w = ctx->x[1];
    const uint64_t r = ctx->x[2];
    lgp_mem_write32(kData, static_cast<uint32_t>(r));
    set_args(ctx, {0, kData, 1, 0});
    SVC(0x18);  // WaitSynchronization(r, timeout 0)
    g_out[1] = ctx->x[0];
    SPAWN(1, w, 0, 44, -2);
    const uint64_t worker = ctx->x[19];
    set_args(ctx, {0, kData, 1, ~uint64_t{0}});
    SVC(0x18);  // wait forever: the worker signals
    g_out[2] = ctx->x[0];
    g_out[3] = ctx->x[1];
    set_args(ctx, {r});
    SVC(0x17);  // ResetSignal
    g_out[4] = ctx->x[0];
    set_args(ctx, {r});
    SVC(0x17);  // again: not signaled
    g_out[5] = ctx->x[0];
    set_args(ctx, {w});
    SVC(0x12);  // ClearEvent via the writable handle
    g_out[6] = ctx->x[0];
    lgp_mem_write32(kData, static_cast<uint32_t>(r));
    lgp_mem_write32(kData + 4, static_cast<uint32_t>(worker));
    set_args(ctx, {0, kData, 2, ~uint64_t{0}});
    SVC(0x18);  // [event, worker]: the terminated worker is signaled
    g_out[7] = ctx->x[0];
    g_out[8] = ctx->x[1];
    set_args(ctx, {0, kData, 1, 1'000'000});
    SVC(0x18);  // 1 ms on the cleared event
    g_out[9] = ctx->x[0];
    set_args(ctx, {0, kData, 0x41, 0});
    SVC(0x18);  // too many handles
    g_out[11] = ctx->x[0];
    lgp_mem_write32(kData, 0x1234);
    set_args(ctx, {0, kData, 1, 0});
    SVC(0x18);  // bad handle
    g_out[12] = ctx->x[0];
    SVC(0x07);
}

void test_events() {
    g_fns[0] = &ev_main;
    g_fns[1] = &ev_worker;
    for (SchedMode mode : {SchedMode::Baton, SchedMode::Free}) {
        const ProcessExit e = run(0, mode);
        CHECK(e.reason == ProcessExit::Reason::ExitProcess);
        CHECK_EQ(g_out[0], 0u);
        CHECK_EQ(g_out[1], kr::kTimedOut.raw);
        CHECK_EQ(g_out[2], 0u);
        CHECK_EQ(g_out[3], 0u);  // index of the event
        CHECK_EQ(g_out[4], 0u);
        CHECK_EQ(g_out[5], kr::kInvalidState.raw);
        CHECK_EQ(g_out[6], 0u);
        CHECK_EQ(g_out[7], 0u);
        CHECK_EQ(g_out[8], 1u);  // index of the thread
        CHECK_EQ(g_out[9], kr::kTimedOut.raw);
        CHECK_EQ(g_out[10], 0u);
        CHECK_EQ(g_out[11], kr::kOutOfRange.raw);
        CHECK_EQ(g_out[12], kr::kInvalidHandle.raw);
    }
}

// --- CancelSynchronization ----------------------------------------------------------

void cancel_worker(lgp::CpuState* ctx) {  // waits forever on the readable event in kData
    set_args(ctx, {0, kData, 1, ~uint64_t{0}});
    SVC(0x18);
    g_out[1] = ctx->x[0];
    SVC(0x0a);
}

void cancel_main(lgp::CpuState* ctx) {
    SVC(0x45);
    lgp_mem_write32(kData, static_cast<uint32_t>(ctx->x[2]));
    SPAWN(1, 0, 0, 44, -2);
    const uint64_t worker = ctx->x[19];
    set_args(ctx, {0});
    SVC(0x0b);  // SleepThread(0): Baton hands over, the worker blocks
    set_args(ctx, {worker});
    SVC(0x19);  // CancelSynchronization(worker)
    g_out[0] = ctx->x[0];
    // Cancel before waiting: the next wait returns Cancelled at once.
    set_args(ctx, {kSelf});
    SVC(0x19);
    set_args(ctx, {0, kData, 1, ~uint64_t{0}});
    SVC(0x18);
    g_out[2] = ctx->x[0];
    lgp_mem_write32(kData + 4, static_cast<uint32_t>(worker));
    set_args(ctx, {0, kData + 4, 1, ~uint64_t{0}});
    SVC(0x18);  // join the worker
    g_out[3] = ctx->x[0];
    SVC(0x07);
}

void test_cancel() {
    g_fns[0] = &cancel_main;
    g_fns[1] = &cancel_worker;
    const ProcessExit e = run(0, SchedMode::Baton);
    CHECK(e.reason == ProcessExit::Reason::ExitProcess);
    CHECK_EQ(g_out[0], 0u);
    CHECK_EQ(g_out[1], kr::kCancelled.raw);
    CHECK_EQ(g_out[2], kr::kCancelled.raw);
    CHECK_EQ(g_out[3], 0u);
}

// --- thread queries -----------------------------------------------------------------

void info_main(lgp::CpuState* ctx) {
    set_args(ctx, {0, kSelf});
    SVC(0x0c);  // GetThreadPriority
    g_out[0] = ctx->x[0];
    g_out[1] = ctx->x[1];
    set_args(ctx, {kSelf, 45});
    SVC(0x0d);  // SetThreadPriority
    g_out[2] = ctx->x[0];
    set_args(ctx, {0, kSelf});
    SVC(0x0c);
    g_out[3] = ctx->x[1];
    set_args(ctx, {kSelf, 64});
    SVC(0x0d);
    g_out[4] = ctx->x[0];
    set_args(ctx, {0, 0, kSelf});
    SVC(0x0e);  // GetThreadCoreMask
    g_out[5] = ctx->x[0];
    g_out[6] = ctx->x[1];
    g_out[7] = ctx->x[2];
    set_args(ctx, {kSelf, 1, 0b10});
    SVC(0x0f);  // SetThreadCoreMask(core 1)
    g_out[8] = ctx->x[0];
    SVC(0x10);  // GetCurrentProcessorNumber
    g_out[9] = ctx->x[0];
    set_args(ctx, {kSelf, 0, 0b10});
    SVC(0x0f);  // core 0 not in the mask
    g_out[10] = ctx->x[0];
    set_args(ctx, {0, fn_va(1), 0, kStacks + 0x2000, 44, 3});
    SVC(0x08);  // core 3 is not in the process core mask
    g_out[11] = ctx->x[0];
    set_args(ctx, {0, fn_va(1), 0, kStacks + 0x2000, 44, 0});
    SVC(0x08);
    g_out[12] = ctx->x[0];
    set_args(ctx, {ctx->x[1]});
    SVC(0x09);  // StartThread
    g_out[13] = ctx->x[0];
    set_args(ctx, {0, kSelf});
    SVC(0x25);  // GetThreadId
    g_out[14] = ctx->x[0];
    g_out[15] = ctx->x[1];
    set_args(ctx, {0, 0x1234});
    SVC(0x25);
    g_out[16] = ctx->x[0];
    g_out[17] = ctx->tpidrro_el0;
    SVC(0x1e);  // GetSystemTick
    g_out[18] = ctx->x[0];
    set_args(ctx, {2'000'000});
    SVC(0x0b);  // SleepThread(2 ms)
    SVC(0x1e);
    g_out[19] = ctx->x[0];
    SVC(0x0a);  // ExitThread; the worker below ends the process
}

void info_worker(lgp::CpuState* ctx) {
    g_out[20] = ctx->tpidrro_el0;
    g_out[21] = lgp_mem_read64(ctx->tpidrro_el0);  // TLS is zeroed
    SVC(0x0a);
}

void test_thread_info() {
    g_fns[0] = &info_main;
    g_fns[1] = &info_worker;
    const ProcessExit e = run(0, SchedMode::CoreBaton);
    CHECK(e.reason == ProcessExit::Reason::AllThreadsExited);
    CHECK_EQ(g_out[0], 0u);
    CHECK_EQ(g_out[1], 44u);
    CHECK_EQ(g_out[2], 0u);
    CHECK_EQ(g_out[3], 45u);
    CHECK_EQ(g_out[4], kr::kInvalidPriority.raw);
    CHECK_EQ(g_out[5], 0u);
    CHECK_EQ(g_out[6], 0u);
    CHECK_EQ(g_out[7], 1u);
    CHECK_EQ(g_out[8], 0u);
    CHECK_EQ(g_out[9], 1u);
    CHECK_EQ(g_out[10], kr::kInvalidCombination.raw);
    CHECK_EQ(g_out[11], kr::kInvalidCoreId.raw);
    CHECK_EQ(g_out[12], 0u);
    CHECK_EQ(g_out[13], 0u);
    CHECK_EQ(g_out[14], 0u);
    CHECK(g_out[15] != 0);
    CHECK_EQ(g_out[16], kr::kInvalidHandle.raw);
    CHECK(g_out[17] != 0 && g_out[17] % 0x200 == 0);
    CHECK(g_out[20] != 0 && g_out[20] != g_out[17]);  // own TLS region
    CHECK_EQ(g_out[21], 0u);
    CHECK(g_out[19] - g_out[18] >= kCounterFrequency / 500);  // slept at least 2 ms
}

// --- condition variable timeout -----------------------------------------------------

void cv_main(lgp::CpuState* ctx) {
    // Own the mutex at kData (tag = some handle value; only compared, never resolved).
    lgp_mem_write32(kData, 0x1234);
    set_args(ctx, {kData, kData + 4, 0x1234, 1'000'000});
    SVC(0x1c);  // WaitProcessWideKeyAtomic, 1 ms
    g_out[0] = ctx->x[0];
    g_out[1] = lgp_mem_read32(kData);      // released by the kernel
    g_out[2] = lgp_mem_read32(kData + 4);  // key marks waiters
    set_args(ctx, {kData + 4, 1});
    SVC(0x1d);  // signal without waiters clears the key
    g_out[3] = lgp_mem_read32(kData + 4);
    set_args(ctx, {kData + 1, kData + 4, 0x1234, 0});
    SVC(0x1c);  // misaligned mutex
    g_out[4] = ctx->x[0];
    set_args(ctx, {kData + 2});
    SVC(0x1b);  // ArbitrateUnlock, misaligned
    g_out[5] = ctx->x[0];
    // ArbitrateLock on a mutex that is no longer contended returns at once.
    lgp_mem_write32(kData, 0);
    set_args(ctx, {0x1234, kData, 0x5678});
    SVC(0x1a);
    g_out[6] = ctx->x[0];
    SVC(0x07);
}

void test_condvar_timeout() {
    g_fns[0] = &cv_main;
    const ProcessExit e = run(0, SchedMode::Baton);
    CHECK(e.reason == ProcessExit::Reason::ExitProcess);
    CHECK_EQ(g_out[0], kr::kTimedOut.raw);
    CHECK_EQ(g_out[1], 0u);
    CHECK_EQ(g_out[2], 1u);
    CHECK_EQ(g_out[3], 0u);
    CHECK_EQ(g_out[4], kr::kInvalidAddress.raw);
    CHECK_EQ(g_out[5], kr::kInvalidAddress.raw);
    CHECK_EQ(g_out[6], 0u);
}

// --- WaitForAddress / SignalToAddress ----------------------------------------------

constexpr uint64_t kArb = kData + 0x20;

void arb_worker(lgp::CpuState* ctx) {  // x0 = index
    const uint64_t i = ctx->x[0];
    set_args(ctx, {kArb, 2, 0, ~uint64_t{0}});
    SVC(0x34);  // WaitForAddress(WaitIfEqual 0, forever)
    g_out[30 + i] = ctx->x[0];
    g_out[40 + i] = 1;
    SVC(0x0a);
}

void arb_main(lgp::CpuState* ctx) {
    lgp_mem_write32(kArb, 5);
    set_args(ctx, {kArb + 2, 0, 9, 0});
    SVC(0x34);
    g_out[0] = ctx->x[0];  // misaligned
    set_args(ctx, {kArb, 3, 9, 0});
    SVC(0x34);
    g_out[1] = ctx->x[0];  // bad type
    set_args(ctx, {kArb, 0, 5, 0});
    SVC(0x34);
    g_out[2] = ctx->x[0];  // 5 < 5 fails
    set_args(ctx, {kArb, 0, 6, 0});
    SVC(0x34);
    g_out[3] = ctx->x[0];  // 5 < 6, timeout 0
    set_args(ctx, {kArb, 1, 6, 0});
    SVC(0x34);
    g_out[4] = ctx->x[0];  // decrement: 5 -> 4, timeout 0
    g_out[5] = lgp_mem_read32(kArb);
    set_args(ctx, {kArb, 1, 4, 0});
    SVC(0x34);
    g_out[6] = ctx->x[0];  // 4 < 4 fails, no decrement
    g_out[7] = lgp_mem_read32(kArb);
    set_args(ctx, {kArb, 2, 3, 0});
    SVC(0x34);
    g_out[8] = ctx->x[0];  // 4 != 3
    set_args(ctx, {kArb, 2, 4, 1'000'000});
    SVC(0x34);
    g_out[9] = ctx->x[0];  // equal, 1 ms: times out

    // No waiters: modify-by-count adds one, increment needs the value.
    lgp_mem_write32(kArb, 7);
    set_args(ctx, {kArb, 2, 7, 1});
    SVC(0x35);
    g_out[10] = ctx->x[0];
    g_out[11] = lgp_mem_read32(kArb);
    set_args(ctx, {kArb, 1, 7, 1});
    SVC(0x35);
    g_out[12] = ctx->x[0];  // 8 != 7

    // Two waiters on 0: a plain signal wakes one, increment-if-equal the rest.
    lgp_mem_write32(kArb, 0);
    SPAWN(1, 0, 0, 44, -2);
    SPAWN(1, 1, 1, 44, -2);
    set_args(ctx, {5'000'000});
    SVC(0x0b);  // both block
    set_args(ctx, {kArb, 0, 0, 1});
    SVC(0x35);
    g_out[13] = ctx->x[0];
    set_args(ctx, {5'000'000});
    SVC(0x0b);
    g_out[14] = g_out[40] + g_out[41];
    set_args(ctx, {kArb, 2, 0, 1});
    SVC(0x35);  // one waiter left, count 1: value - 1
    g_out[15] = ctx->x[0];
    g_out[16] = lgp_mem_read32(kArb);
    set_args(ctx, {5'000'000});
    SVC(0x0b);
    g_out[17] = g_out[40] + g_out[41];
    SVC(0x07);
}

void test_address_arbiter() {
    g_fns[0] = &arb_main;
    g_fns[1] = &arb_worker;
    const ProcessExit e = run(0, SchedMode::Baton);
    CHECK(e.reason == ProcessExit::Reason::ExitProcess);
    CHECK_EQ(g_out[0], kr::kInvalidAddress.raw);
    CHECK_EQ(g_out[1], kr::kInvalidEnumValue.raw);
    CHECK_EQ(g_out[2], kr::kInvalidState.raw);
    CHECK_EQ(g_out[3], kr::kTimedOut.raw);
    CHECK_EQ(g_out[4], kr::kTimedOut.raw);
    CHECK_EQ(g_out[5], 4u);
    CHECK_EQ(g_out[6], kr::kInvalidState.raw);
    CHECK_EQ(g_out[7], 4u);
    CHECK_EQ(g_out[8], kr::kInvalidState.raw);
    CHECK_EQ(g_out[9], kr::kTimedOut.raw);
    CHECK_EQ(g_out[10], 0u);
    CHECK_EQ(g_out[11], 8u);
    CHECK_EQ(g_out[12], kr::kInvalidState.raw);
    CHECK_EQ(g_out[13], 0u);
    CHECK_EQ(g_out[14], 1u);  // one woke
    CHECK_EQ(g_out[15], 0u);
    CHECK_EQ(g_out[16], static_cast<uint32_t>(-1));
    CHECK_EQ(g_out[17], 2u);
    CHECK_EQ(g_out[30], 0u);
    CHECK_EQ(g_out[31], 0u);
}

// --- process end with blocked or crashing threads -----------------------------------

void blocked_worker(lgp::CpuState* ctx) {
    set_args(ctx, {0, kData, 1, ~uint64_t{0}});
    SVC(0x18);  // never signaled
    g_out[5] = 1;  // not reached: the svc ends the thread
    SVC(0x0a);
}

void exit_main(lgp::CpuState* ctx) {
    SVC(0x45);
    lgp_mem_write32(kData, static_cast<uint32_t>(ctx->x[2]));
    SPAWN(1, 0, 0, 44, -2);
    SPAWN(1, 0, 1, 44, 1);
    set_args(ctx, {0});
    SVC(0x0b);
    SVC(0x07);
}

void crash_worker(lgp::CpuState* ctx) {
    ctx->pc = 0x0800'4444;
    LGP_BRK(0x77);
    LGP_HALT_CHECK();
}

void crash_main(lgp::CpuState* ctx) {
    SVC(0x45);
    lgp_mem_write32(kData, static_cast<uint32_t>(ctx->x[2]));
    SPAWN(2, 0, 0, 30, -2);  // higher priority: runs at once and crashes
    set_args(ctx, {0, kData, 1, ~uint64_t{0}});
    SVC(0x18);
    g_out[6] = 1;  // not reached
}

void return_main(lgp::CpuState* ctx) { ctx->x[30] = 0; }

void test_process_end() {
    g_fns[0] = &exit_main;
    g_fns[1] = &blocked_worker;
    g_fns[2] = &crash_worker;
    g_fns[3] = &crash_main;
    g_fns[4] = &return_main;
    for (SchedMode mode : {SchedMode::Baton, SchedMode::CoreBaton, SchedMode::Free}) {
        ProcessExit e = run(0, mode);
        CHECK(e.reason == ProcessExit::Reason::ExitProcess);
        CHECK_EQ(g_out[5], 0u);

        e = run(3, mode);
        CHECK(e.reason == ProcessExit::Reason::Crash);
        CHECK_EQ(e.halt, static_cast<uint32_t>(lgp::kHaltBrk));
        CHECK_EQ(e.halt_info, 0x77u);
        CHECK_EQ(e.pc, 0x0800'4444u);
        CHECK_EQ(g_out[6], 0u);

        e = run(4, mode);
        CHECK(e.reason == ProcessExit::Reason::Crash);
        CHECK_EQ(e.halt, static_cast<uint32_t>(lgp::kHaltBadJump));

        e = run(9, mode);  // no function at the entry
        CHECK(e.reason == ProcessExit::Reason::Crash);
        CHECK_EQ(e.halt_info, fn_va(9));
    }
}

// --- Scheduler::pause / resume (app in the background, P7) ----------------------------

std::atomic<uint64_t> g_ticks[3];
std::atomic<bool> g_stop{false};

void pause_worker(lgp::CpuState* ctx) {  // x0 = counter index; counts, sleeps 1 ms, until g_stop
    const uint64_t i = ctx->x[0];
    while (!g_stop.load()) {
        g_ticks[i].fetch_add(1);
        set_args(ctx, {1'000'000});
        SVC(0x0b);
    }
    SVC(0x0a);
}

void pause_blocked(lgp::CpuState* ctx) {  // waits forever on the never signaled event
    set_args(ctx, {0, kData, 1, ~uint64_t{0}});
    SVC(0x18);
    SVC(0x0a);
}

void pause_main(lgp::CpuState* ctx) {
    SVC(0x45);
    lgp_mem_write32(kData, static_cast<uint32_t>(ctx->x[2]));
    SPAWN(1, 0, 0, 44, -2);
    SPAWN(1, 1, 1, 44, 1);
    SPAWN(2, 0, 2, 44, -2);
    while (!g_stop.load()) {
        g_ticks[2].fetch_add(1);
        set_args(ctx, {1'000'000});
        SVC(0x0b);
    }
    SVC(0x07);
}

uint64_t total_ticks() { return g_ticks[0].load() + g_ticks[1].load() + g_ticks[2].load(); }

bool wait_for(const std::function<bool()>& cond) {
    for (int i = 0; i < 5000; ++i) {
        if (cond())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

void test_pause() {
    g_fns[0] = &pause_main;
    g_fns[1] = &pause_worker;
    g_fns[2] = &pause_blocked;
    for (SchedMode mode : {SchedMode::Baton, SchedMode::CoreBaton, SchedMode::Free}) {
        for (auto& t : g_ticks)
            t = 0;
        g_stop = false;
        bool quiet = false, still = false, again = false, before = false, after = false;
        const ProcessExit e = run(0, mode, [&](Kernel& k) {
            Scheduler& s = k.scheduler();
            before = wait_for([] { return g_ticks[0] > 2 && g_ticks[1] > 2 && g_ticks[2] > 2; });
            quiet = s.pause(5000) && s.active_threads() == 0 && s.paused();
            const uint64_t stopped = total_ticks();
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            still = total_ticks() == stopped;
            s.resume();
            again = wait_for([&] { return total_ticks() > stopped + 6; }) && !s.paused();
            // Ending the process releases paused threads.
            after = s.pause(5000);
            s.stop();
        });
        CHECK(before);
        CHECK(quiet);
        CHECK(still);
        CHECK(again);
        CHECK(after);
        CHECK(e.reason == ProcessExit::Reason::Stopped);
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "virtual") == 0)
        g_clock = ClockMode::Virtual;
    RUN(test_events);
    RUN(test_cancel);
    RUN(test_thread_info);
    RUN(test_condvar_timeout);
    RUN(test_address_arbiter);
    RUN(test_process_end);
    RUN(test_pause);
    return finish();
}
