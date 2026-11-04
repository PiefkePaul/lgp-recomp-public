#include "lgp/boot/npdm.hpp"

#include <cstring>

namespace lgp::boot {
namespace {

uint32_t u32_at(const std::vector<uint8_t>& b, size_t at) {
    uint32_t v = 0;
    std::memcpy(&v, b.data() + at, 4);
    return v;
}

uint64_t u64_at(const std::vector<uint8_t>& b, size_t at) {
    uint64_t v = 0;
    std::memcpy(&v, b.data() + at, 8);
    return v;
}

uint64_t bit_range(uint32_t lo, uint32_t hi) {
    uint64_t m = 0;
    for (uint32_t i = lo; i <= hi && i < 64; ++i)
        m |= uint64_t{1} << i;
    return m;
}

bool fail(std::string* error, const char* what) {
    if (error != nullptr)
        *error = what;
    return false;
}

}  // namespace

std::optional<Npdm> parse_npdm(const std::vector<uint8_t>& b, std::string* error) {
    // META: magic, flags 0x0C, priority 0x0E, core 0x0F, system resource
    // size 0x14, stack size 0x1C, name 0x20, ACI0 offset/size 0x70/0x74.
    if (b.size() < 0x80 || std::memcmp(b.data(), "META", 4) != 0) {
        fail(error, "no META header");
        return std::nullopt;
    }
    Npdm n;
    const uint8_t flags = b[0x0C];
    n.is_64bit = (flags & 1) != 0;
    n.address_space_type = (flags >> 1) & 7;
    n.main_thread_priority = b[0x0E];
    n.main_thread_core = b[0x0F];
    n.system_resource_size = u32_at(b, 0x14);
    n.main_thread_stack_size = u32_at(b, 0x1C);
    for (size_t i = 0x20; i < 0x30 && b[i] != 0; ++i)
        n.name.push_back(static_cast<char>(b[i]));

    // ACI0: program id 0x10, kernel capabilities offset/size 0x30/0x34
    // (relative to the ACI0).
    const uint32_t aci = u32_at(b, 0x70), aci_size = u32_at(b, 0x74);
    if (aci_size < 0x40 || aci > b.size() || b.size() - aci < aci_size || std::memcmp(b.data() + aci, "ACI0", 4) != 0) {
        fail(error, "no ACI0 section");
        return std::nullopt;
    }
    n.program_id = u64_at(b, aci + 0x10);
    const uint32_t kc = u32_at(b, aci + 0x30), kc_size = u32_at(b, aci + 0x34);
    if (kc > aci_size || aci_size - kc < kc_size) {
        fail(error, "kernel capabilities outside the ACI0");
        return std::nullopt;
    }
    for (uint32_t i = 0; i + 4 <= kc_size; i += 4) {
        const uint32_t cap = u32_at(b, aci + kc + i);
        if ((cap & 0xF) != 0x7)  // ThreadInfo: id 3, pattern 0b0111
            continue;
        const uint32_t lowest_priority = (cap >> 4) & 0x3F;  // numerically largest
        const uint32_t highest_priority = (cap >> 10) & 0x3F;
        const uint32_t min_core = (cap >> 16) & 0xFF;
        const uint32_t max_core = (cap >> 24) & 0xFF;
        if (highest_priority <= lowest_priority)
            n.priority_mask = bit_range(highest_priority, lowest_priority);
        if (min_core <= max_core)
            n.core_mask = bit_range(min_core, max_core);
        break;
    }
    return n;
}

}  // namespace lgp::boot
