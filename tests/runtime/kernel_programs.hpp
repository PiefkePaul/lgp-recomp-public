// Setup and expected results of the kernel test programs
// (kernel_program_code.hpp), independent of the CPU that runs them: Unicorn
// (test_kernel_unicorn) or lifted C++ (test_kernel_lifted) must both pass.
#pragma once

#include <array>
#include <cstring>
#include <string>

#include "check.hpp"
#include "kernel_program_code.hpp"
#include "lgp/runtime/kernel.hpp"

namespace lgp_test {

enum class GuestStop { ExitProcess, Break, Unimplemented, MemFault, Other };

struct GuestResult {
    GuestStop stop = GuestStop::Other;
    std::array<uint64_t, 31> x{};
    uint64_t pc = 0;  // where it stopped (the faulting instruction for MemFault)
    std::string detail;
};

inline lgp::rt::ProcessConfig program_process() {
    constexpr uint64_t kMiB = 0x10'0000;
    lgp::rt::ProcessConfig c;
    c.total_memory_size = 16 * kMiB;
    c.custom_layout = true;
    c.layout = lgp::rt::Layout::packed(kProgramBase, 1 * kMiB, 1 * kMiB, 1 * kMiB, 1 * kMiB, 16 * kMiB);
    return c;
}

struct LoadedProgram {
    uint64_t entry = 0;
    uint64_t stack_top = 0;
    std::array<uint64_t, 31> x{};  // initial registers
};

// Code into the code region (RX), a main stack at the top of the stack region.
inline LoadedProgram load_program(lgp::rt::Kernel& k, const ProgramCode& p) {
    using namespace lgp::rt;
    const Layout& l = k.memory().layout();
    CHECK(k.map_owned(l.code.base, kPageSize, MemState::Code, kPermRW).ok());
    uint8_t* mem = k.memory().host_ptr(l.code.base, kPageSize);
    std::memcpy(mem, p.words.data(), p.words.size() * 4);
    if (!p.data.empty())
        std::memcpy(mem + p.words.size() * 4, p.data.data(), p.data.size());
    k.memory().set_perm(l.code.base, kPageSize, kPermRX);
    CHECK(k.map_owned(l.stack.end() - 4 * kPageSize, 4 * kPageSize, MemState::Stack, kPermRW).ok());
    LoadedProgram lp{l.code.base, l.stack.end(), {}};
    lp.x[24] = l.stack.base;  // MapMemory destination for "memory_svcs"
    return lp;
}

// faults_enforced: whether the executor stops on accesses the guest lacks
// permission for (Unicorn always; lifted code only with checked accessors).
inline void check_program(const ProgramCode& p, lgp::rt::Kernel& k, const GuestResult& r, bool faults_enforced) {
    using namespace lgp::rt;
    if (!r.detail.empty())
        std::printf("  detail: %s\n", r.detail.c_str());
    const std::string name = p.name;
    const Layout& l = k.memory().layout();
    if (name == "memory_svcs") {
        CHECK(r.stop == GuestStop::ExitProcess);
        CHECK_EQ(r.x[19], 0u);  // every svc succeeded
        CHECK_EQ(r.x[20], l.heap.base);
        CHECK_EQ(r.x[21], l.heap.base);
        CHECK_EQ(r.x[22], static_cast<uint64_t>(MemState::Normal));
        CHECK_EQ(r.x[23], static_cast<uint64_t>(kPermRW));
        CHECK_EQ(r.x[25], 0x56781234u);
        CHECK_EQ(r.x[26], 0xabcdu);
        const BlockInfo heap = k.memory().query(l.heap.base);
        CHECK(heap.state == MemState::Normal);
        CHECK_EQ(heap.attr, kAttrUncached);
        CHECK_EQ(heap.size, kPageSize);  // the uncached page split off the rest of the heap
        CHECK(k.memory().query(l.stack.base).state == MemState::Free);
        const auto out = k.debug_output();
        CHECK(out.size() == 1 && out[0] == "ok");
    } else if (name == "transfer_memory") {
        CHECK_EQ(r.x[19], 0u);
        CHECK_EQ(r.x[20], kernel_result::kInvalidMemoryRegion.raw);
        if (faults_enforced) {
            CHECK(r.stop == GuestStop::MemFault);
            CHECK_EQ(r.pc, kProgramBase + 22 * 4);  // the load from lent memory
        } else {
            CHECK(r.stop == GuestStop::ExitProcess);  // unchecked accessors do not see guest permissions
        }
        const BlockInfo lent = k.memory().query(r.x[21]);
        CHECK_EQ(lent.perm, kPermNone);
        CHECK_EQ(lent.attr, kAttrLocked);
    } else if (name == "unimplemented_svc") {
        CHECK(r.stop == GuestStop::Unimplemented);
        CHECK_EQ(r.pc, kProgramBase + 4);  // after the SVC
    } else {
        CHECK(!"unknown program");
    }
}

}  // namespace lgp_test
