// The parts of a program's NPDM (main.npdm in the ExeFS) the boot program
// needs to start the process like Horizon's loader would: program id, main
// thread stack size, priority and core, and the thread capabilities.
// Layout from switchbrew ("NPDM"): META header, ACI0 with the program id and
// the kernel capability descriptors. Signatures and the ACID are not checked.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace lgp::boot {

struct Npdm {
    std::string name;  // META name (e.g. "Application")
    uint64_t program_id = 0;
    uint32_t main_thread_stack_size = 0;
    uint8_t main_thread_priority = 0;
    uint8_t main_thread_core = 0;
    uint32_t system_resource_size = 0;
    bool is_64bit = false;
    uint8_t address_space_type = 0;  // META flags bits 1..3 (1 = 36 bit, 2 = 32 bit no reserved, 3 = 39 bit)
    // ThreadInfo kernel capability (0b0111): allowed cores and priorities.
    // Absent in a malformed NPDM; the caller keeps its defaults then.
    std::optional<uint64_t> core_mask;
    std::optional<uint64_t> priority_mask;
};

// Parses an NPDM image; nullopt with a reason in *error if it is not one.
std::optional<Npdm> parse_npdm(const std::vector<uint8_t>& bytes, std::string* error);

}  // namespace lgp::boot
