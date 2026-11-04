// Stage-0 spike for the Hypervisor.framework oracle (ADR-0006).
//
// Proves on Apple Silicon that we can:
//   1. create a VM and map guest memory,
//   2. run a few hand-assembled AArch64 instructions natively in the guest,
//   3. get control back on BRK (debug exception trapped to the host),
//   4. read the guest register state afterwards.
// It also measures the round-trip cost of a guest exit (HVC), which sets the
// batch size for stage-1 instruction tests.
//
// The guest runs at EL1 with the MMU off. That is enough for this spike; the
// real harness runs test code at EL0 with page tables (see ADR-0006).

#include <Hypervisor/Hypervisor.h>
#include <sys/mman.h>

#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

constexpr uint64_t kGuestBase = 0x10000;   // IPA of guest code
constexpr size_t kGuestSize = 0x10000;     // 64 KiB, multiple of the 16 KiB host page
constexpr uint64_t kCpsrEl1h = 0x3c5;      // EL1h, DAIF masked

// Exception classes (ESR_ELx.EC).
constexpr uint32_t kEcHvc64 = 0x16;
constexpr uint32_t kEcBrk64 = 0x3c;

// Hand-assembled A64 encodings.
constexpr uint32_t kAddX0X0X1 = 0x8b010000;  // add x0, x0, x1
constexpr uint32_t kSubsX2X0X1 = 0xeb010002; // subs x2, x0, x1
constexpr uint32_t kBrk0 = 0xd4200000;       // brk #0
constexpr uint32_t kHvc0 = 0xd4000002;       // hvc #0
constexpr uint32_t kBMinus4 = 0x17ffffff;    // b .-4

int failures = 0;

#define CHECK_HV(expr)                                                        \
    do {                                                                      \
        hv_return_t r_ = (expr);                                              \
        if (r_ != HV_SUCCESS) {                                               \
            std::fprintf(stderr, "FAIL %s -> 0x%x\n", #expr, (unsigned)r_);   \
            std::exit(2);                                                     \
        }                                                                     \
    } while (0)

void expect_eq(const char* what, uint64_t got, uint64_t want) {
    if (got != want) {
        std::fprintf(stderr, "FAIL %s: got 0x%" PRIx64 ", want 0x%" PRIx64 "\n",
                     what, got, want);
        ++failures;
    } else {
        std::printf("ok   %s = 0x%" PRIx64 "\n", what, got);
    }
}

uint32_t exit_ec(const hv_vcpu_exit_t* ex) {
    return static_cast<uint32_t>(ex->exception.syndrome >> 26) & 0x3f;
}

uint64_t get_reg(hv_vcpu_t vcpu, hv_reg_t reg) {
    uint64_t v = 0;
    CHECK_HV(hv_vcpu_get_reg(vcpu, reg, &v));
    return v;
}

}  // namespace

int main() {
    hv_return_t vm = hv_vm_create(nullptr);
    if (vm == HV_DENIED) {
        std::fprintf(stderr,
                     "FAIL hv_vm_create: HV_DENIED (binary lacks the "
                     "com.apple.security.hypervisor entitlement?)\n");
        return 2;
    }
    CHECK_HV(vm);

    void* mem = mmap(nullptr, kGuestSize, PROT_READ | PROT_WRITE,
                     MAP_ANON | MAP_PRIVATE, -1, 0);
    if (mem == MAP_FAILED) {
        std::perror("mmap");
        return 2;
    }
    auto* code = static_cast<uint32_t*>(mem);
    // Block 1 at +0x000: add, subs, brk.
    code[0] = kAddX0X0X1;
    code[1] = kSubsX2X0X1;
    code[2] = kBrk0;
    // Block 2 at +0x100: endless HVC loop for the exit-cost measurement.
    code[0x40] = kHvc0;
    code[0x41] = kBMinus4;

    CHECK_HV(hv_vm_map(mem, kGuestBase, kGuestSize,
                       HV_MEMORY_READ | HV_MEMORY_WRITE | HV_MEMORY_EXEC));

    hv_vcpu_t vcpu;
    hv_vcpu_exit_t* ex = nullptr;
    CHECK_HV(hv_vcpu_create(&vcpu, &ex, nullptr));
    CHECK_HV(hv_vcpu_set_trap_debug_exceptions(vcpu, true));

    // --- Test 1: ADD/SUBS run natively, BRK returns control ---------------
    CHECK_HV(hv_vcpu_set_reg(vcpu, HV_REG_PC, kGuestBase));
    CHECK_HV(hv_vcpu_set_reg(vcpu, HV_REG_CPSR, kCpsrEl1h));
    CHECK_HV(hv_vcpu_set_reg(vcpu, HV_REG_X0, 0x1234));
    CHECK_HV(hv_vcpu_set_reg(vcpu, HV_REG_X1, 0x1234));
    CHECK_HV(hv_vcpu_run(vcpu));

    expect_eq("exit reason (exception)", ex->reason, HV_EXIT_REASON_EXCEPTION);
    expect_eq("ESR.EC (BRK64)", exit_ec(ex), kEcBrk64);
    expect_eq("PC at BRK", get_reg(vcpu, HV_REG_PC), kGuestBase + 8);
    expect_eq("X0 = 0x1234 + 0x1234", get_reg(vcpu, HV_REG_X0), 0x2468);
    expect_eq("X2 = X0 - X1", get_reg(vcpu, HV_REG_X2), 0x1234);
    // SUBS 0x2468 - 0x1234: positive, non-zero, no borrow -> N=0 Z=0 C=1 V=0.
    expect_eq("NZCV", (get_reg(vcpu, HV_REG_CPSR) >> 28) & 0xf, 0x2);

    // --- Test 2: exit round-trip cost --------------------------------------
    constexpr int kExits = 20000;
    CHECK_HV(hv_vcpu_set_reg(vcpu, HV_REG_PC, kGuestBase + 0x100));
    CHECK_HV(hv_vcpu_set_reg(vcpu, HV_REG_CPSR, kCpsrEl1h));
    int hvc_exits = 0;
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kExits; ++i) {
        CHECK_HV(hv_vcpu_run(vcpu));
        if (ex->reason == HV_EXIT_REASON_EXCEPTION && exit_ec(ex) == kEcHvc64) {
            ++hvc_exits;
        }
    }
    auto t1 = std::chrono::steady_clock::now();
    expect_eq("HVC exits", hvc_exits, kExits);
    double us = std::chrono::duration<double, std::micro>(t1 - t0).count() / kExits;
    std::printf("info exit round trip: %.2f us (%d HVC exits)\n", us, kExits);

    CHECK_HV(hv_vcpu_destroy(vcpu));
    CHECK_HV(hv_vm_unmap(kGuestBase, kGuestSize));
    CHECK_HV(hv_vm_destroy());
    munmap(mem, kGuestSize);

    if (failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("hv_spike: all checks passed\n");
    return 0;
}
