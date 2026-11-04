// Data model for loaded Switch modules (NSO / NRO) and the linked image.
// See docs/design/p2-interfaces.md for the contract with the lifter.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace lgp::recomp {

class FormatError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

enum class SegmentKind : uint8_t { Text, Ro, Data, Bss };

struct Segment {
    SegmentKind kind;
    uint64_t offset;   // offset inside the module image
    uint64_t size;     // in memory
    uint64_t file_offset = 0;
    uint64_t file_size = 0;
    bool compressed = false;
    bool hash_checked = false;
    bool hash_ok = true;
    std::array<uint8_t, 32> sha256{};

    bool readable() const { return true; }
    bool writable() const { return kind == SegmentKind::Data || kind == SegmentKind::Bss; }
    bool executable() const { return kind == SegmentKind::Text; }
};

// ELF symbol binding / type values (subset).
constexpr uint8_t kStbLocal = 0, kStbGlobal = 1, kStbWeak = 2;
constexpr uint8_t kSttNoType = 0, kSttObject = 1, kSttFunc = 2, kSttTls = 6;

struct Symbol {
    std::string name;
    uint64_t value = 0;   // module-relative offset (st_value); VA = module.base + value
    uint64_t size = 0;
    uint8_t bind = 0;
    uint8_t type = 0;
    uint16_t shndx = 0;
    bool defined() const { return shndx != 0; }
};

// AArch64 relocation types the loader understands.
constexpr uint32_t kRAarch64Abs64 = 257;
constexpr uint32_t kRAarch64GlobDat = 1025;
constexpr uint32_t kRAarch64JumpSlot = 1026;
constexpr uint32_t kRAarch64Relative = 1027;
constexpr uint32_t kRAarch64TlsTpRel64 = 1030;
constexpr uint32_t kRAarch64TlsDesc = 1031;

const char* relocation_type_name(uint32_t type);

struct Relocation {
    uint64_t offset;      // module-relative location of the slot
    uint32_t type;
    uint32_t sym;         // dynsym index, 0 = none
    int64_t addend;
    bool from_relr = false;
    bool from_jmprel = false;
    bool applied = false; // set by the link step
};

// Dynamic section tags we use.
constexpr int64_t kDtNull = 0, kDtNeeded = 1, kDtPltRelSz = 2, kDtHash = 4, kDtStrTab = 5, kDtSymTab = 6,
                  kDtRela = 7, kDtRelaSz = 8, kDtRelaEnt = 9, kDtStrSz = 10, kDtSymEnt = 11, kDtInit = 12,
                  kDtFini = 13, kDtSoName = 14, kDtRel = 17, kDtRelSz = 18, kDtPltRel = 20, kDtJmpRel = 23,
                  kDtInitArray = 25, kDtFiniArray = 26, kDtInitArraySz = 27, kDtFiniArraySz = 28,
                  kDtRelrSz = 35, kDtRelr = 36, kDtRelrEnt = 37;
constexpr int64_t kDtGnuHash = 0x6FFFFEF5;

struct DynamicEntry {
    int64_t tag;
    uint64_t value;
};

struct Module {
    std::string name;             // "main", "sdk", fixture file name
    std::string internal_name;    // from the .ro path header, e.g. "beluga.nss" (SDK builds only)
    std::array<uint8_t, 32> module_id{};
    bool is_nro = false;
    uint32_t nso_flags = 0;
    uint64_t base = 0;            // guest VA of image[0]; 0 until linked
    std::vector<Segment> segments;      // Text, Ro, Data, Bss in this order
    std::vector<uint8_t> image;         // decompressed; relocations applied after link(); includes BSS
    std::vector<std::string> api_info;
    std::vector<Symbol> dynsym;
    std::vector<std::string> needed;    // DT_NEEDED
    std::vector<DynamicEntry> dynamic;
    std::vector<Relocation> relocations;
    uint64_t mod0_offset = 0;           // 0 = no MOD0
    uint64_t dynamic_offset = 0;
    uint64_t eh_frame_hdr_offset = 0, eh_frame_hdr_size = 0;
    std::vector<uint64_t> fde_starts;                        // module-relative, sorted, unique
    std::vector<std::pair<uint64_t, uint64_t>> fde_ranges;   // [start, end), module-relative, sorted
    std::vector<uint64_t> lsda_fde_starts;                   // FDEs with an LSDA (C++ landing pads), sorted
    uint64_t init = 0, fini = 0;                              // module-relative, 0 = none
    std::vector<uint64_t> init_array, fini_array;             // module-relative (read after relocation)
    uint64_t init_array_offset = 0, init_array_size = 0, fini_array_offset = 0, fini_array_size = 0;
    uint64_t entry_offset = 0;    // .text start
    uint64_t header_code_exclusion = 0; // bytes after .text start that are header, not code (NRO: 0x80)

    const Segment& segment(SegmentKind k) const;
    const Segment* segment_at_offset(uint64_t off) const;
    uint64_t image_size() const { return image.size(); }
    std::optional<uint64_t> dynamic_value(int64_t tag) const;

    uint32_t read_u32(uint64_t off) const;
    uint64_t read_u64(uint64_t off) const;
    void write_u64(uint64_t off, uint64_t v);
    std::string hex_module_id() const;
};

}  // namespace lgp::recomp
