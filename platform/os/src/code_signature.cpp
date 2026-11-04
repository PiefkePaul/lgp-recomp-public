// Entitlements from the own code signature (code_signature.hpp).
#include "lgp/platform/code_signature.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <fstream>
#include <iterator>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace lgp::platform {

namespace {

uint32_t le32(const std::vector<uint8_t>& b, size_t at) {
    return uint32_t(b[at]) | uint32_t(b[at + 1]) << 8 | uint32_t(b[at + 2]) << 16 | uint32_t(b[at + 3]) << 24;
}

uint32_t be32(const std::vector<uint8_t>& b, size_t at) {
    return uint32_t(b[at]) << 24 | uint32_t(b[at + 1]) << 16 | uint32_t(b[at + 2]) << 8 | uint32_t(b[at + 3]);
}

constexpr uint32_t kFatMagic = 0xcafebabe;          // big-endian
constexpr uint32_t kMachO64Magic = 0xfeedfacf;      // little-endian
constexpr uint32_t kLcCodeSignature = 0x1d;
constexpr uint32_t kSuperBlobMagic = 0xfade0cc0;    // CSMAGIC_EMBEDDED_SIGNATURE
constexpr uint32_t kEntitlementsMagic = 0xfade7171; // CSMAGIC_EMBEDDED_ENTITLEMENTS
constexpr uint32_t kEntitlementsSlot = 5;           // CSSLOT_ENTITLEMENTS
constexpr uint32_t kCpuArm64 = 0x0100000c;
constexpr size_t kMaxRead = 64u << 20;               // more than any header or signature

// Bytes [off, off + len) of the file, empty when they are not all there. Only the
// headers and the signature are read, never the whole (large, lifted) executable.
using ReadAt = std::function<std::vector<uint8_t>(size_t off, size_t len)>;

// The entitlements blob in a code signature SuperBlob.
std::string superblob_entitlements(const std::vector<uint8_t>& b) {
    if (b.size() < 12 || be32(b, 0) != kSuperBlobMagic)
        return {};
    const uint32_t count = be32(b, 8);
    for (uint32_t k = 0; k < count && 12 + 8 * size_t(k + 1) <= b.size(); ++k) {
        const size_t index = 12 + 8 * size_t(k);
        if (be32(b, index) != kEntitlementsSlot)
            continue;
        const size_t blob = be32(b, index + 4);
        if (blob + 8 > b.size() || be32(b, blob) != kEntitlementsMagic)
            return {};
        const size_t blob_len = be32(b, blob + 4);
        if (blob_len < 8 || blob + blob_len > b.size())
            return {};
        return std::string(b.begin() + long(blob + 8), b.begin() + long(blob + blob_len));
    }
    return {};
}

// One thin 64-bit Mach-O at [base, base + size) of the file.
std::string thin_entitlements(const ReadAt& read, size_t base, size_t size) {
    const std::vector<uint8_t> head = size >= 32 ? read(base, 32) : std::vector<uint8_t>{};
    if (head.empty() || le32(head, 0) != kMachO64Magic)
        return {};
    const uint32_t ncmds = le32(head, 16), sizeofcmds = le32(head, 20);
    if (32 + size_t(sizeofcmds) > size || sizeofcmds > kMaxRead)
        return {};
    const std::vector<uint8_t> cmds = read(base + 32, sizeofcmds);
    if (cmds.size() != sizeofcmds)
        return {};
    size_t at = 0;
    for (uint32_t i = 0; i < ncmds && at + 8 <= cmds.size(); ++i) {
        const uint32_t cmd = le32(cmds, at), cmdsize = le32(cmds, at + 4);
        if (cmdsize < 8 || at + cmdsize > cmds.size())
            return {};
        if (cmd == kLcCodeSignature && cmdsize >= 16) {
            const size_t off = le32(cmds, at + 8), len = le32(cmds, at + 12);
            if (len < 12 || len > kMaxRead || off + len > size)
                return {};
            return superblob_entitlements(read(base + off, len));
        }
        at += cmdsize;
    }
    return {};
}

std::string entitlements(const ReadAt& read) {
    const std::vector<uint8_t> magic = read(0, 8);
    if (magic.empty())
        return {};
    if (be32(magic, 0) != kFatMagic)
        return thin_entitlements(read, 0, SIZE_MAX);
    const uint32_t n = be32(magic, 4);
    const std::vector<uint8_t> archs = n <= 64 ? read(8, 20 * size_t(n)) : std::vector<uint8_t>{};
    size_t first_off = 0, first_size = 0;
    for (uint32_t i = 0; i < n && 20 * size_t(i + 1) <= archs.size(); ++i) {
        const size_t arch = 20 * size_t(i);
        const size_t off = be32(archs, arch + 8), size = be32(archs, arch + 12);
        if (be32(archs, arch) == kCpuArm64)
            return thin_entitlements(read, off, size);
        if (i == 0)
            first_off = off, first_size = size;
    }
    return first_size ? thin_entitlements(read, first_off, first_size) : std::string{};
}

#if defined(__APPLE__)
// Small files only (the provisioning profile).
std::vector<uint8_t> read_file(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    return f ? std::vector<uint8_t>(std::istreambuf_iterator<char>(f), {}) : std::vector<uint8_t>{};
}
#endif

}  // namespace

std::string macho_entitlements(const std::vector<uint8_t>& b) {
    return entitlements([&b](size_t off, size_t len) {
        return off <= b.size() && len <= b.size() - off
                   ? std::vector<uint8_t>(b.begin() + long(off), b.begin() + long(off + len))
                   : std::vector<uint8_t>{};
    });
}

std::string macho_file_entitlements(const std::filesystem::path& path, bool* readable) {
    std::ifstream f(path, std::ios::binary);
    if (readable)
        *readable = bool(f);
    if (!f)
        return {};
    return entitlements([&f](size_t off, size_t len) {
        std::vector<uint8_t> out(len);
        f.clear();
        if (!f.seekg(std::streamoff(off)) || !f.read(reinterpret_cast<char*>(out.data()), std::streamsize(len)))
            return std::vector<uint8_t>{};
        return out;
    });
}

std::string profile_plist(const std::vector<uint8_t>& profile) {
    const std::string_view text(reinterpret_cast<const char*>(profile.data()), profile.size());
    const size_t start = text.find("<?xml");
    const size_t end = text.rfind("</plist>");
    if (start == std::string_view::npos || end == std::string_view::npos || end < start)
        return {};
    return std::string(text.substr(start, end + 8 - start));
}

bool entitlement_true(std::string_view plist, std::string_view key) {
    const std::string tag = "<key>" + std::string(key) + "</key>";
    const size_t at = plist.find(tag);
    if (at == std::string_view::npos)
        return false;
    size_t v = at + tag.size();
    while (v < plist.size() && (plist[v] == ' ' || plist[v] == '\t' || plist[v] == '\n' || plist[v] == '\r'))
        ++v;
    return plist.substr(v, 7) == "<true/>";
}

bool own_signature(OwnSignature* out) {
    *out = {};
#if defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string path(size, '\0');
    if (_NSGetExecutablePath(path.data(), &size) != 0)
        return false;
    path.resize(path.find('\0') == std::string::npos ? path.size() : path.find('\0'));
    const std::filesystem::path exe(path);
    bool readable = false;
    out->entitlements = macho_file_entitlements(exe, &readable);
    if (!readable)
        return false;
    out->profile = profile_plist(read_file(exe.parent_path() / "embedded.mobileprovision"));
    return true;
#else
    return false;
#endif
}

}  // namespace lgp::platform
