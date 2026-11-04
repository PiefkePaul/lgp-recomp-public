// Differential-test harness: common interface of all executors (ADR-0006).
//
// A TestCase is own code bytes (never game code in the repo) plus an initial
// guest state; every executor (Unicorn, Hypervisor.framework, recompiled code)
// runs it until a stop condition and returns a RunResult. Two RunResults are
// compared with diff().
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace lgp::difftest {

// BRK immediate that ends a test case ("LG"). Any other BRK is a fault.
inline constexpr uint16_t kSentinelBrk = 0x4C47;
// svcExitProcess also ends a test case.
inline constexpr uint32_t kSvcExitProcess = 0x07;

struct Vec128 {
    uint64_t lo = 0;
    uint64_t hi = 0;
    bool operator==(const Vec128&) const = default;
};

struct GuestState {
    std::array<uint64_t, 31> x{};  // x0..x30
    uint64_t sp = 0;
    uint64_t pc = 0;
    uint32_t nzcv = 0;  // flags in bits 31..28, as in the NZCV system register
    std::array<Vec128, 32> v{};
    uint32_t fpcr = 0;
    uint32_t fpsr = 0;
    uint64_t tpidr_el0 = 0;
    uint64_t tpidrro_el0 = 0;
    bool operator==(const GuestState&) const = default;
};

enum MemPerm : uint32_t { kRead = 1, kWrite = 2, kExec = 4 };

struct MemoryRegion {
    uint64_t base = 0;           // 4 KiB aligned
    std::vector<uint8_t> bytes;  // size is rounded up to 4 KiB when mapped
    uint32_t perms = kRead | kWrite;
};

// x0..x7 in, x0..x7 out: the same handler serves every executor, so only the
// CPU part differs between them (later: the runtime's SVC dispatcher, P2.6/P3).
using SvcRegs = std::array<uint64_t, 8>;
using SvcHandler = std::function<void(uint32_t imm, SvcRegs& regs)>;

struct TestCase {
    std::string name;
    std::vector<MemoryRegion> memory;
    GuestState initial;
    uint64_t max_instructions = 1'000'000;
    uint64_t stop_pc = 0;  // non-zero: also stop when execution reaches this address (not executed)
    SvcHandler svc;  // empty: every SVC returns 0 in x0
};

struct SvcRecord {
    uint32_t imm = 0;
    SvcRegs args{};
    SvcRegs results{};
    bool operator==(const SvcRecord&) const = default;
};

enum class Outcome { Sentinel, ExitProcess, StopAddress, InstructionLimit, Fault };
const char* to_string(Outcome o);

struct RunResult {
    Outcome outcome = Outcome::Fault;
    std::string detail;  // human-readable reason for a Fault; not compared
    // pc is the architectural return address of the stop: the BRK itself for
    // the sentinel, the instruction after the SVC for svcExitProcess.
    GuestState final;
    std::vector<MemoryRegion> memory;  // writable regions after the run
    std::vector<SvcRecord> svcs;
};

class IExecutor {
public:
    virtual ~IExecutor() = default;
    virtual const char* name() const = 0;
    virtual RunResult run(const TestCase& tc) = 0;
};

// Human-readable differences (empty when both results agree). Memory differences
// name the first differing byte of each region.
std::vector<std::string> diff(const RunResult& a, const RunResult& b);

}  // namespace lgp::difftest
