// The entitlements an iOS app was signed with, read back from its own bundle
// (docs/knowledge/ios-ohne-mac.md, sideloading): whether a sideloader kept
// increased-memory-limit shows in the log instead of only as an early jetsam
// kill. The kernel grants an entitlement when the executable's code signature
// carries it and the provisioning profile (embedded.mobileprovision) allows it.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace lgp::platform {

// The entitlements plist (XML) in the code signature of a Mach-O file (thin
// 64-bit or fat; in a fat file the arm64 slice, else the first): the blob of
// slot 5 (magic 0xfade7171) of the SuperBlob LC_CODE_SIGNATURE points at.
// Empty when the file has no signature or the signature no entitlements.
std::string macho_entitlements(const std::vector<uint8_t>& file);

// The same for a file on disk, reading only its headers and the signature
// (not the whole executable). readable: whether the file could be opened.
std::string macho_file_entitlements(const std::filesystem::path& path, bool* readable = nullptr);

// The plist inside a provisioning profile (CMS-signed): the text from
// "<?xml" to the last "</plist>". Empty when there is none.
std::string profile_plist(const std::vector<uint8_t>& profile);

// Whether plist text has <key>key</key> followed by <true/> (a profile
// lists its entitlements in such a dict too, so the first <key>key</key> of
// the text counts).
bool entitlement_true(std::string_view plist, std::string_view key);

// This process: the entitlements of its executable's signature and the plist
// of embedded.mobileprovision beside it. false off Apple or when the
// executable cannot be read; profile stays empty without a profile.
struct OwnSignature {
    std::string entitlements;
    std::string profile;
};
bool own_signature(OwnSignature* out);

}  // namespace lgp::platform
