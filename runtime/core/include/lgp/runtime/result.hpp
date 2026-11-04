// Horizon result codes as the guest sees them (svc results in w0).
//
// A result packs a module (bits 0..8) and a description (bits 9..21). The
// kernel uses module 1; the values below are the ones svc handlers return.
#pragma once

#include <cstdint>

namespace lgp::rt {

struct Result {
    uint32_t raw = 0;

    constexpr bool ok() const { return raw == 0; }
    constexpr bool failed() const { return raw != 0; }
    constexpr uint32_t module() const { return raw & 0x1FF; }
    constexpr uint32_t description() const { return (raw >> 9) & 0x1FFF; }
    constexpr bool operator==(const Result&) const = default;
};

constexpr Result make_result(uint32_t module, uint32_t description) { return Result{module | (description << 9)}; }

inline constexpr Result kSuccess{0};

// Kernel results (module 1).
namespace kernel_result {
inline constexpr uint32_t kModule = 1;
inline constexpr Result kOutOfSessions = make_result(kModule, 7);
inline constexpr Result kInvalidArgument = make_result(kModule, 14);
inline constexpr Result kNotImplemented = make_result(kModule, 33);
inline constexpr Result kTerminationRequested = make_result(kModule, 59);
inline constexpr Result kInvalidSize = make_result(kModule, 101);
inline constexpr Result kInvalidAddress = make_result(kModule, 102);
inline constexpr Result kOutOfResource = make_result(kModule, 103);
inline constexpr Result kOutOfMemory = make_result(kModule, 104);
inline constexpr Result kOutOfHandles = make_result(kModule, 105);
inline constexpr Result kInvalidCurrentMemory = make_result(kModule, 106);
inline constexpr Result kInvalidNewMemoryPermission = make_result(kModule, 108);
inline constexpr Result kInvalidMemoryRegion = make_result(kModule, 110);
inline constexpr Result kInvalidPriority = make_result(kModule, 112);
inline constexpr Result kInvalidCoreId = make_result(kModule, 113);
inline constexpr Result kInvalidHandle = make_result(kModule, 114);
inline constexpr Result kInvalidPointer = make_result(kModule, 115);
inline constexpr Result kInvalidCombination = make_result(kModule, 116);
inline constexpr Result kTimedOut = make_result(kModule, 117);
inline constexpr Result kCancelled = make_result(kModule, 118);
inline constexpr Result kOutOfRange = make_result(kModule, 119);
inline constexpr Result kInvalidEnumValue = make_result(kModule, 120);
inline constexpr Result kNotFound = make_result(kModule, 121);
inline constexpr Result kInvalidState = make_result(kModule, 125);
inline constexpr Result kLimitReached = make_result(kModule, 132);
}  // namespace kernel_result

}  // namespace lgp::rt
