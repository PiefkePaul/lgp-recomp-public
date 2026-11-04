// Hypervisor.framework guest for the differential-test harness (ADR-0006, P2.7).
//
// Runs AArch64 code natively on Apple Silicon at EL0 with the MMU on, like
// game code on the Switch: the host builds stage-1 page tables (4 KiB granule,
// identity VA == IPA) and a tiny EL1 vector table whose every entry is an
// `hvc #n`, so each exception from EL0 (SVC, aborts, undefined instructions)
// comes back to the host as an HVC exit; ESR_EL1/ELR_EL1/FAR_EL1 tell what
// happened. BRK is trapped straight to the host (debug exception trap).
//
// One Machine per process (Hypervisor.framework allows one VM per process),
// used from the thread that created it (vCPU affinity).
#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "../oracle.h"

namespace lgp::difftest::hv {

// Highest guest VA + 1 the machine can map (39-bit stage 1, 36-bit IPA).
inline constexpr uint64_t kGuestVaLimit = 0xF'0000'0000;

struct Exit {
    enum class Kind {
        Brk,      // BRK at final pc; imm = BRK immediate
        Svc,      // SVC; imm = SVC immediate, pc already after the SVC
        Fault,    // any other exception from EL0; esr/far/elr describe it
        Timeout,  // the guest ran longer than the timeout
    } kind = Kind::Fault;
    uint32_t imm = 0;
    uint32_t esr = 0;  // ESR_EL1 for Svc/Fault
    uint64_t far = 0;
    uint64_t elr = 0;
    std::string detail;
};

class Machine {
public:
    Machine();
    ~Machine();
    Machine(const Machine&) = delete;
    Machine& operator=(const Machine&) = delete;

    // Maps [va, va + size) with perms (MemPerm bits; exec implies read),
    // 4 KiB granular. New memory reads zero. Remapping a mapped page only
    // changes its permissions.
    void map(uint64_t va, uint64_t size, uint32_t perms);
    // Removes every mapping (and frees the host memory).
    void unmap_all();
    bool is_mapped(uint64_t va) const;

    // Host-side access to mapped guest memory (throws outside mappings).
    void write(uint64_t va, const void* data, uint64_t size);
    void read(uint64_t va, void* out, uint64_t size) const;

    void set_state(const GuestState& s);
    GuestState state() const;

    // Runs EL0 code from the current state until an exception or the timeout.
    // After an Svc exit the guest continues behind the SVC on the next run().
    Exit run(std::chrono::microseconds timeout);

    static const char* exception_class_name(uint32_t ec);

private:
    struct Chunk {
        uint64_t ipa = 0;  // 16 KiB aligned
        uint64_t size = 0;
        uint8_t* host = nullptr;
    };
    uint8_t* host_page(uint64_t va) const;  // nullptr when no backing
    uint64_t* l3_entry(uint64_t va, bool create);
    uint64_t alloc_table();
    void reset_tables();
    void run_el1_stub();  // TLB and I-cache maintenance after table/code changes

    uint64_t vcpu_ = 0;
    void* exit_ = nullptr;  // hv_vcpu_exit_t*
    uint8_t* sys_ = nullptr;  // system area: vectors, stub, page tables
    uint64_t next_table_ = 0;
    std::map<uint64_t, Chunk> chunks_;  // by ipa
    std::map<uint64_t, uint8_t*> pages_;  // 16 KiB page ipa -> host
    bool dirty_ = true;  // tables or code changed since the last maintenance
};

// IExecutor over Machine: maps the test case's regions, runs to the sentinel
// BRK, svcExitProcess or a fault; SVCs go to TestCase::svc like in Unicorn.
// max_instructions is approximated by a wall-clock timeout (≈ 1 ns per
// instruction, at least 100 ms), because the hardware does not count.
class HvExecutor : public IExecutor {
public:
    const char* name() const override { return "hypervisor"; }
    RunResult run(const TestCase& tc) override;
    Machine& machine() { return machine_; }

private:
    Machine machine_;
};

}  // namespace lgp::difftest::hv
