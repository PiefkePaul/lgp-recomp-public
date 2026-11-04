// Cache of the native rewrite (native_image.hpp): the analysis of a game's
// modules takes seconds (LGP on a Mac M1: 18 s), its result only depends on
// the linked images, the analysis config, where the patch area goes and the
// code that computed it. The native start (BootOptions::native.cache) keeps
// the patch in a file and takes it from there while all of these match.
//
// The file holds patched words and stub code derived from game code: it
// stays on the device (app cache folder, $LGP_PRIVATE_DIR), never in the repo.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "lgp/recomp/link.hpp"
#include "lgp/recomp/native_image.hpp"

namespace lgp::recomp {

// Bump when the analysis, the plan or the patch change their output for the
// same input. The boot program adds a stamp of its own executable to the key
// as well (any relink invalidates), so this matters for builds that keep it.
inline constexpr uint32_t kNativeCacheVersion = 1;

// What a cached rewrite restores: the patch and the plan's counts of sites it
// could not patch.
struct NativeRewriteRecord {
    NativePatch patch;
    uint64_t x18_unpatched = 0;
    uint64_t gate_unpatched = 0;
};

// Key of a rewrite: module names, bases and images, the config text, the
// patch area base, kNativeCacheVersion and `salt` (the caller's build stamp).
uint64_t native_cache_key(const LinkedImage& image, std::string_view config_text, uint64_t area_base,
                          std::string_view salt);

std::vector<uint8_t> encode_native_cache(const NativeRewriteRecord& r, uint64_t key);

// False (with the reason in *why) for another key, another format or a
// damaged file; *out is then unchanged.
bool decode_native_cache(std::span<const uint8_t> bytes, uint64_t key, NativeRewriteRecord* out, std::string* why);

}  // namespace lgp::recomp
