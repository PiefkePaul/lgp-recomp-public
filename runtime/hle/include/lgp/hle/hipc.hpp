// HIPC message layout (the kernel IPC format in the TLS message buffer) and
// the CMIF framing inside its raw data (P3.3). Interface facts from
// switchbrew/libnx; no code taken from another project.
//
//   word 0   type:16  num_x:4  num_a:4  num_b:4  num_w:4
//   word 1   num_data_words:10  recv_static_mode:4  pad:6  recv_list_offset:11  has_special_header:1
//   [special header: send_pid:1  num_copy:4  num_move:4]  [pid u64]  [copy handles]  [move handles]
//   X descriptors (2 words), A, B, W descriptors (3 words each)
//   raw data (num_data_words): padding to 16 bytes from the message start, then CMIF
//   C descriptors (recv list, 2 words each)
#pragma once

#include <cstdint>
#include <cstring>
#include <optional>
#include <vector>

namespace lgp::hle {

enum class HipcType : uint16_t {
    Invalid = 0,
    Close = 2,
    Request = 4,
    Control = 5,
    RequestWithContext = 6,
    ControlWithContext = 7,
};

struct HipcBuffer {
    uint64_t addr = 0;
    uint64_t size = 0;
    uint32_t index_or_mode = 0;  // X: static index; A/B/W: buffer mode
};

// A parsed request. All offsets are bytes from the message start.
struct HipcRequest {
    uint16_t type = 0;
    bool send_pid = false;
    uint64_t pid = 0;
    std::vector<uint32_t> copy_handles;
    std::vector<uint32_t> move_handles;
    std::vector<HipcBuffer> x, a, b, w, c;
    uint32_t data_offset = 0;  // first raw data word
    uint32_t data_size = 0;    // bytes
};

constexpr uint32_t align16(uint32_t v) { return (v + 15) & ~15u; }

// Parses a request; nullopt if it does not fit into `size` bytes.
std::optional<HipcRequest> parse_hipc(const uint8_t* msg, uint32_t size);

// Writes a response header (type 0, no buffers) with the given copy/move
// handles and room for raw_size bytes of CMIF data, zeroed. Returns the
// 16-byte aligned offset where the caller writes that data, or 0 if it does
// not fit into `size`.
uint32_t write_hipc_response(uint8_t* msg, uint32_t size, uint32_t raw_size, const std::vector<uint32_t>& copy,
                             const std::vector<uint32_t>& move);

// CMIF framing.
inline constexpr uint32_t kCmifInMagic = 0x49434653;   // "SFCI"
inline constexpr uint32_t kCmifOutMagic = 0x4F434653;  // "SFCO"

struct CmifInHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t command_id;
    uint32_t token;
};
struct CmifOutHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t result;
    uint32_t token;
};
// Domain requests put this in front of the CMIF header.
struct CmifDomainInHeader {
    uint8_t type;  // 1 = send message, 2 = close object
    uint8_t num_in_objects;
    uint16_t data_size;  // CMIF header + payload
    uint32_t object_id;
    uint32_t padding;
    uint32_t token;
};
struct CmifDomainOutHeader {
    uint32_t num_out_objects;
    uint32_t padding[3];
};
static_assert(sizeof(CmifInHeader) == 16 && sizeof(CmifOutHeader) == 16);
static_assert(sizeof(CmifDomainInHeader) == 16 && sizeof(CmifDomainOutHeader) == 16);

}  // namespace lgp::hle
