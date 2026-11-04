// pl:u (shared fonts). The system fonts are system data archives of the
// console's firmware, not part of a game; the runtime reads them from the
// user's own dump through the game storage, like fsp-srv's system data:
//
//   /systemdata/<data id as %016llx>/<file>.bfttf
//
//   0100000000000811  nintendo_udsg-r_std_003.bfttf        Standard (JP/US/EU)
//   0100000000000814  nintendo_udsg-r_org_zh-cn_003.bfttf  Chinese simplified
//   0100000000000814  nintendo_udsg-r_ext_zh-cn_003.bfttf  Chinese simplified extension
//   0100000000000813  nintendo_udjxh-db_zh-tw_003.bfttf    Chinese traditional
//   0100000000000812  nintendo_udsg-r_ko_003.bfttf         Korean
//   0100000000000810  nintendo_ext_003.bfttf               Nintendo extension
//
// All six go into one 0x1100000-byte shared memory, each as an 8-byte header
// (magic and big-endian size, both XORed with a key) followed by the
// decrypted TrueType data; GetSharedMemoryAddressOffset points at the data.
// A missing file leaves its font empty (size 0) and is logged; a missing
// standard font is logged as an error, because LGP crashes without it.
// Without the firmware's standard font a stand-in TrueType (by default the
// free font of cmake/LgpFreeFont.cmake) serves types 0-4 at one offset; the
// Nintendo extension (type 5, button icons) comes from the firmware or stays
// empty.
// Command ids from switchbrew "Shared Font services", layout as Ryujinx
// (SharedFontManager). Details: docs/knowledge/horizon-pl-shared-font.md.
#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "lgp/hle/service.hpp"
#include "lgp/platform/storage.hpp"

namespace lgp::hle {

inline constexpr int kSharedFontCount = 6;
inline constexpr uint64_t kSharedFontMemorySize = 0x1100000;

// BFTTF (as stored in the system data archive) -> TrueType: every 32-bit word
// after the 8-byte header XORed with the key. Empty on a bad magic.
std::vector<uint8_t> decrypt_bfttf(const std::vector<uint8_t>& bfttf);

// The stand-in for the standard font this build has: the embedded free font
// (LGP_ENABLE_FREE_FONT), else empty.
std::span<const uint8_t> default_stand_in_font();

// Registers pl:u. game: storage with /systemdata (nullptr: no fonts); must
// outlive every session. stand_in: TrueType used when the standard font is
// missing (empty: none); must outlive every session. Fonts are read on the
// first command.
void register_pl(ServiceManager& sm, platform::IStorage* game,
                 std::span<const uint8_t> stand_in = default_stand_in_font());

}  // namespace lgp::hle
