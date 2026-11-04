// Runs a homebrew fixture module (NSO or NRO, devkitA64/libnx) as a guest
// process on lgp::rt::Kernel and records its end state (P2.8).
//
// The module is loaded the way Horizon's loader maps a homebrew NSO: image
// unrelocated at a fixed base (libnx's crt0 relocates itself), text RX, ro R,
// data+bss RW, one main-thread stack. The process starts with the Horizon
// entry ABI (x0 = 0, x1 = main thread handle), so an NRO runs the same way.
//
// Who executes the guest code is decided by the indirect resolver/dispatch
// table the caller installs: the recompiled functions, or an oracle (Unicorn)
// that is handed each thread's CpuState.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "lgp/recomp/module.hpp"
#include "lgp/runtime/kernel.hpp"

namespace lgp_fixture {

// lgp_lift's default --base; the generated code and the loaded image must agree.
inline constexpr uint64_t kModuleBase = 0x0800'0000;

struct RunOptions {
    lgp::rt::SchedMode sched_mode = lgp::rt::SchedMode::CoreBaton;
    bool trace_svcs = false;
    // Native execution (test_fixture_native.cpp): where the module and the
    // address space start, guest address == host address, room in the code
    // region behind the image (patch area), and a step after the image is
    // loaded, before the process starts.
    uint64_t base = kModuleBase;
    bool at_guest_address = false;
    uint64_t extra_code = 0;
    std::function<void(lgp::rt::Kernel&)> after_load;
};

// Comparable result of one run. Everything a guest can observe or leave
// behind: how the process ended, what it printed, and the contents of every
// mapped block (hashed per block; the module's data+bss also verbatim so a
// difference can be located).
struct EndState {
    std::string exit;  // exit reason (+ halt details for a crash)
    std::vector<std::string> debug_output;
    struct Block {
        uint64_t addr = 0;
        uint64_t size = 0;
        uint32_t state = 0;
        uint32_t perm = 0;
        uint64_t hash = 0;  // FNV-1a 64 of the contents
    };
    std::vector<Block> blocks;
    uint64_t data_addr = 0;
    std::vector<uint8_t> data;  // module .data + .bss
};

lgp::recomp::Module load_fixture(const std::string& path);

EndState run_fixture(const lgp::recomp::Module& module, const RunOptions& options);

// Text form (one fact per line) for the expected-state files the native run
// writes and the WebAssembly run compares against.
std::string to_text(const EndState& s);

// Prints the differences; true if equal.
bool compare(const EndState& expected, const EndState& actual, const char* expected_name, const char* actual_name);

}  // namespace lgp_fixture
