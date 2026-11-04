// P3.2 acceptance: the multi-thread programs (thread_programs.s), lifted by the
// P2.3 lifter, run as a real process on the scheduler in all three modes.
// Baton runs must be deterministic (same log twice, and the expected order);
// the parallel modes must be correct.
// P3.3 acceptance (argument "ipc"): ipc_main opens "lgp:test" through sm: and
// calls a command of it, against the HLE service layer (runtime/hle).
#include <algorithm>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "check.hpp"
#include "ipc_test_service.hpp"
#include "kernel_program_code.hpp"
#include "lgp/runtime/guest.hpp"
#include "lgp/runtime/kernel.hpp"
#include "lifted_table.hpp"
#include "thread_program_code.hpp"

extern const lgp_test::LiftedEntry kLiftedThreadFunctions[];
extern const size_t kLiftedThreadFunctionCount;

using namespace lgp::rt;
using namespace lgp_test;

namespace {

constexpr uint64_t kData = kProgramBase + 0x10000;
constexpr uint64_t kDataSize = 0x10000;

lgp::LiftedFn resolve(uint64_t va) {
    for (size_t i = 0; i < kLiftedThreadFunctionCount; ++i) {
        if (kLiftedThreadFunctions[i].va == va)
            return kLiftedThreadFunctions[i].fn;
    }
    return nullptr;
}

uint64_t function_va(const char* name) {
    for (const ThreadFunction& f : kThreadFunctions) {
        if (std::strcmp(f.name, name) == 0)
            return kProgramBase + f.offset;
    }
    return 0;
}

struct Run {
    ProcessExit exit;
    uint32_t done = 0;
    uint32_t fail = 0;
    uint32_t sum = 0;
    uint32_t ipc_step = 0;
    uint32_t ipc_result = 0;
    std::vector<uint32_t> log;
};

// Second argument "virtual": every run on the virtual guest clock (clock.hpp).
ClockMode g_clock = ClockMode::Host;

Run run_program(const char* main_fn, SchedMode mode, const std::function<void(Kernel&)>& setup = {}) {
    ProcessConfig c;
    c.clock_mode = g_clock;
    c.total_memory_size = 16 * 0x10'0000ull;
    c.custom_layout = true;
    c.layout = Layout::packed(kProgramBase, 0x10'0000, 0x10'0000, 0x10'0000, 0x10'0000, 16 * 0x10'0000);
    c.sched_mode = mode;
    Kernel k(c);
    const Layout& l = k.memory().layout();
    CHECK(k.map_owned(l.code.base, kPageSize, MemState::Code, kPermRW).ok());
    std::memcpy(k.memory().host_ptr(l.code.base, sizeof(kThreadText)), kThreadText, sizeof(kThreadText));
    k.memory().set_perm(l.code.base, kPageSize, kPermRX);
    CHECK(k.map_owned(kData, kDataSize, MemState::CodeData, kPermRW).ok());
    CHECK(k.map_owned(l.stack.end() - 4 * kPageSize, 4 * kPageSize, MemState::Stack, kPermRW).ok());

    if (setup)
        setup(k);
    set_indirect_resolver(&resolve);
    Run r;
    r.exit = k.run({function_va(main_fn), l.stack.end(), 44, 0});
    set_indirect_resolver(nullptr);

    const uint8_t* d = k.memory().host_ptr(kData, kDataSize);
    std::memcpy(&r.done, d + 0x20, 4);
    std::memcpy(&r.fail, d + 0x24, 4);
    std::memcpy(&r.sum, d + 0x18, 4);
    std::memcpy(&r.ipc_step, d + 0x2c, 4);
    std::memcpy(&r.ipc_result, d + 0x30, 4);
    uint32_t n = 0;
    std::memcpy(&n, d + 0x1c, 4);
    r.log.resize(std::min<uint32_t>(n, 0x800));
    std::memcpy(r.log.data(), d + 0x1000, r.log.size() * 4);
    set_active_kernel(nullptr);
    return r;
}

void check_exit(const Run& r) {
    if (r.exit.reason != ProcessExit::Reason::ExitProcess)
        std::printf("  exit: %s, thread %llu, halt %u, info 0x%llx, pc 0x%llx\n", exit_reason_name(r.exit.reason),
                    static_cast<unsigned long long>(r.exit.thread_id), r.exit.halt,
                    static_cast<unsigned long long>(r.exit.halt_info), static_cast<unsigned long long>(r.exit.pc));
    CHECK(r.exit.reason == ProcessExit::Reason::ExitProcess);
    CHECK_EQ(r.fail, 0u);
    CHECK_EQ(r.done, 1u);
}

void check_producer_consumer(const Run& r) {
    check_exit(r);
    CHECK_EQ(r.sum, 200u * 201u / 2u);
    CHECK_EQ(r.log.size(), 400u);
    // Each side in order: produced 1..200, consumed 1..200 (FIFO queue), and
    // nothing consumed before it was produced.
    uint32_t produced = 0;
    uint32_t consumed = 0;
    for (uint32_t e : r.log) {
        if ((e & 0x80000000u) == 0) {
            CHECK_EQ(e, produced + 1);
            produced = e;
        } else {
            const uint32_t v = e & 0x7FFFFFFFu;
            CHECK_EQ(v, consumed + 1);
            CHECK(v <= produced);
            consumed = v;
        }
    }
}

void test_producer_consumer() {
    std::printf("[producer_consumer]\n");
    for (SchedMode mode : {SchedMode::Baton, SchedMode::CoreBaton, SchedMode::Free}) {
        std::printf("  mode %s\n", sched_mode_name(mode));
        const int runs = mode == SchedMode::Free ? 10 : 2;
        std::vector<uint32_t> first;
        for (int i = 0; i < runs; ++i) {
            const Run r = run_program("pc_main", mode);
            check_producer_consumer(r);
            if (i == 0)
                first = r.log;
            else if (mode != SchedMode::Free)
                CHECK(r.log == first);  // one slot per core, one core used: deterministic
        }
    }
}

void test_priorities() {
    std::printf("[priorities]\n");
    const std::vector<uint32_t> expected = {40, 1, 45, 50};
    for (SchedMode mode : {SchedMode::Baton, SchedMode::CoreBaton, SchedMode::Free}) {
        std::printf("  mode %s\n", sched_mode_name(mode));
        for (int i = 0; i < 2; ++i) {
            const Run r = run_program("prio_main", mode);
            check_exit(r);
            if (mode == SchedMode::Free) {
                std::vector<uint32_t> sorted = r.log;
                std::sort(sorted.begin(), sorted.end());
                CHECK((sorted == std::vector<uint32_t>{1, 40, 45, 50}));
            } else {
                CHECK(r.log == expected);
                if (r.log != expected) {
                    for (uint32_t e : r.log)
                        std::printf("    log %u\n", e);
                }
            }
        }
    }
}

void test_spin_wait() {
    std::printf("[spin_wait]\n");
    for (SchedMode mode : {SchedMode::Baton, SchedMode::CoreBaton, SchedMode::Free}) {
        std::printf("  mode %s\n", sched_mode_name(mode));
        check_exit(run_program("spin_main", mode));
    }
}

// Spinning before a wait (Scheduler::set_park_spin_ns, LGP_PARK_SPIN_US)
// changes when a thread sleeps, never what it computes: the baton modes give
// the same log as without, and timed waits still time out.
void test_park_spin() {
    std::printf("[park_spin]\n");
    const auto spin = [](Kernel& k) { k.scheduler().set_park_spin_ns(200'000); };
    for (SchedMode mode : {SchedMode::Baton, SchedMode::CoreBaton, SchedMode::Free}) {
        std::printf("  mode %s\n", sched_mode_name(mode));
        const Run plain = run_program("pc_main", mode);
        const Run spun = run_program("pc_main", mode, spin);
        check_producer_consumer(spun);
        if (mode != SchedMode::Free)
            CHECK(spun.log == plain.log);
        check_exit(run_program("prio_main", mode, spin));
        check_exit(run_program("spin_main", mode, spin));
    }
}

void test_ipc() {
    std::printf("[ipc]\n");
    for (SchedMode mode : {SchedMode::Baton, SchedMode::CoreBaton}) {
        std::printf("  mode %s\n", sched_mode_name(mode));
        lgp::hle::ServiceManager sm;
        sm.register_service("lgp:test", [] { return std::make_shared<TestService>(); });
        const Run r = run_program("ipc_main", mode, [&](Kernel& k) { sm.install(k); });
        if (r.fail != 0)
            std::printf("  failed at step %u with 0x%x\n", r.ipc_step, r.fail);
        check_exit(r);
        CHECK_EQ(r.ipc_result, 5u);
    }
    // Service not registered: GetServiceHandle (step 3) answers sm NotRegistered.
    lgp::hle::ServiceManager empty;
    const Run r = run_program("ipc_main", SchedMode::Baton, [&](Kernel& k) { empty.install(k); });
    CHECK(r.exit.reason == ProcessExit::Reason::ExitProcess);
    CHECK_EQ(r.done, 0u);
    CHECK_EQ(r.ipc_step, 3u);
    CHECK_EQ(r.fail, lgp::hle::sm_result::kNotRegistered.raw);
    // No sm: port at all: svcConnectToNamedPort (step 1) fails with NotFound.
    const Run none = run_program("ipc_main", SchedMode::Baton);
    CHECK_EQ(none.ipc_step, 1u);
    CHECK_EQ(none.fail, kernel_result::kNotFound.raw);
}

}  // namespace

int main(int argc, char** argv) {
    const std::string only = argc > 1 ? argv[1] : "";
    if (argc > 2 && std::string(argv[2]) == "virtual")
        g_clock = ClockMode::Virtual;
    if (only.empty() || only == "threads") {
        test_producer_consumer();
        test_priorities();
        test_spin_wait();
        test_park_spin();
    }
    if (only.empty() || only == "ipc")
        test_ipc();
    return finish();
}
