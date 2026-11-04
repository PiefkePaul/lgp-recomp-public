// A request that waits with its core slot given up (rt::HostWorkScope, as ldn
// does while it waits for a server) in the core baton mode: a second guest
// thread of the same core that sends a request on the same session meanwhile
// must wait for the session without the slot, else neither thread gets on.
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <thread>

#include "check.hpp"
#include "ipc_client.hpp"
#include "lgp/lifted/lifted.hpp"
#include "lgp/runtime/host_work.hpp"

namespace {

using namespace lgp_test;

constexpr uint64_t kStacks = kBase + 0x20000;
constexpr uint64_t kFnBase = kBase + 0x1000;
constexpr uint64_t kMsgMain = kBase + 0x40000;
constexpr uint64_t kMsgWorker = kBase + 0x41000;
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

lgp::LiftedFn g_fns[3];
lgp::LiftedFn resolve(uint64_t va) {
    if (va < kFnBase || (va - kFnBase) % 0x10 != 0 || (va - kFnBase) / 0x10 >= 3)
        return nullptr;
    return g_fns[(va - kFnBase) / 0x10];
}
uint64_t fn_va(int index) { return kFnBase + 0x10 * static_cast<uint64_t>(index); }

Kernel* g_kernel = nullptr;
std::shared_ptr<Session> g_session;
std::atomic<bool> g_worker_sending{false};
std::atomic<uint32_t> g_main_result{0xFFFFFFFF}, g_worker_result{0xFFFFFFFF};

// Command 0 waits without its slot until the worker is about to send (at most
// 2 s), then a little longer so its request meets the taken session; command 1
// answers at once.
struct Slow final : Service {
    Slow() : Service("lgp:slow") {
        on(0, [](Request&, Response&) {
            HostWorkScope away(true);
            const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (!g_worker_sending.load() && std::chrono::steady_clock::now() < end)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        });
        on(1, [](Request&, Response&) {});
    }
};

void write_request(uint64_t va, uint32_t command) {
    Msg m;
    m.command = command;
    const std::vector<uint8_t> bytes = build(m);
    std::memcpy(g_kernel->memory().host_ptr(va, bytes.size()), bytes.data(), bytes.size());
}

uint32_t send(uint64_t va) {
    Result rc = kSuccess;
    g_session->handle_request(*g_kernel, va, 0x100, &rc);
    if (rc.failed())
        return rc.raw;
    return parse_reply(g_kernel->memory().host_ptr(va, 0x100), false).result;
}

void worker(lgp::CpuState* ctx) {
    g_worker_sending = true;
    g_worker_result = send(kMsgWorker);
    SVC(0x0a);  // ExitThread
}

void main_fn(lgp::CpuState* ctx) {
    set_args(ctx, {0, fn_va(1), 0, kStacks + 0x4000, 44, 0});  // same core and priority
    SVC(0x08);  // CreateThread
    set_args(ctx, {ctx->x[1]});
    SVC(0x09);  // StartThread
    g_main_result = send(kMsgMain);
    set_args(ctx, {10 * kMs});
    SVC(0x0b);  // SleepThread: the worker finishes
    SVC(0x0a);
}

void test_session_wait_without_slot() {
    g_fns[0] = &main_fn;
    g_fns[1] = &worker;
    ProcessConfig c;
    c.custom_layout = true;
    c.layout = Layout::packed(kBase, 0x10'0000, 0x10'0000, 0x10'0000, 0x10'0000, 16 * 0x10'0000);
    c.sched_mode = SchedMode::CoreBaton;
    Kernel k(c);
    g_kernel = &k;
    CHECK(k.map_owned(kStacks, 0x10000, MemState::Stack, kPermRW).ok());
    CHECK(k.map_owned(kMsgMain, 0x2000, MemState::Stack, kPermRW).ok());
    write_request(kMsgMain, 0);
    write_request(kMsgWorker, 1);
    g_session = std::make_shared<Session>(std::make_shared<Slow>());
    set_indirect_resolver(&resolve);
    const ProcessExit e = k.run({fn_va(0), kStacks + 0x10000, 44, 0});  // hangs without the fix
    set_indirect_resolver(nullptr);
    set_active_kernel(nullptr);
    g_session.reset();
    g_kernel = nullptr;
    CHECK(e.reason == ProcessExit::Reason::AllThreadsExited);
    CHECK(g_worker_sending.load());
    CHECK_EQ(g_main_result.load(), 0u);
    CHECK_EQ(g_worker_result.load(), 0u);
}

}  // namespace

int main() {
    RUN(test_session_wait_without_slot);
    return finish();
}
