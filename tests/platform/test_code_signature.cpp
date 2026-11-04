// Entitlements read back from a code signature (lgp/platform/code_signature.hpp),
// on made-up Mach-O files: thin, fat with an arm64 slice, without signature
// and with a truncated one.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "../runtime/check.hpp"
#include "lgp/platform/code_signature.hpp"

using namespace lgp::platform;

namespace {

const char* kXml =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<plist version=\"1.0\"><dict>\n"
    "\t<key>com.apple.developer.kernel.increased-memory-limit</key>\n\t<true/>\n"
    "\t<key>com.apple.developer.kernel.extended-virtual-addressing</key>\n\t<false/>\n"
    "</dict></plist>\n";

void put_le(std::vector<uint8_t>& b, uint32_t v) {
    for (int i = 0; i < 4; ++i)
        b.push_back(uint8_t(v >> (8 * i)));
}
void put_be(std::vector<uint8_t>& b, uint32_t v) {
    for (int i = 3; i >= 0; --i)
        b.push_back(uint8_t(v >> (8 * i)));
}

// mach_header_64, one LC_SEGMENT-like filler command and LC_CODE_SIGNATURE,
// then the SuperBlob with a code directory stand-in (slot 0) and the
// entitlements (slot 5).
std::vector<uint8_t> thin(const std::string& xml, bool sign = true) {
    std::vector<uint8_t> b;
    const uint32_t filler = 24, sig_cmd = 16, header = 32;
    put_le(b, 0xfeedfacf);
    put_le(b, 0x0100000c);
    put_le(b, 0);
    put_le(b, 2);  // MH_EXECUTE
    put_le(b, sign ? 2 : 1);
    put_le(b, filler + (sign ? sig_cmd : 0));
    put_le(b, 0);
    put_le(b, 0);
    put_le(b, 0x19);  // a command the reader must skip
    put_le(b, filler);
    b.resize(b.size() + filler - 8);
    if (!sign)
        return b;
    const uint32_t sig_off = header + filler + sig_cmd;
    const uint32_t cd_len = 12, ent_len = uint32_t(8 + xml.size());
    const uint32_t sb_len = 12 + 2 * 8 + cd_len + ent_len;
    put_le(b, 0x1d);
    put_le(b, sig_cmd);
    put_le(b, sig_off);
    put_le(b, sb_len);
    put_be(b, 0xfade0cc0);
    put_be(b, sb_len);
    put_be(b, 2);
    put_be(b, 0);  // slot 0, code directory
    put_be(b, 12 + 16);
    put_be(b, 5);  // slot 5, entitlements
    put_be(b, 12 + 16 + cd_len);
    put_be(b, 0xfade0c02);
    put_be(b, cd_len);
    put_be(b, 0);
    put_be(b, 0xfade7171);
    put_be(b, ent_len);
    b.insert(b.end(), xml.begin(), xml.end());
    return b;
}

void test_thin() {
    const std::string got = macho_entitlements(thin(kXml));
    CHECK(got == kXml);
    CHECK(entitlement_true(got, "com.apple.developer.kernel.increased-memory-limit"));
    CHECK(!entitlement_true(got, "com.apple.developer.kernel.extended-virtual-addressing"));
    CHECK(!entitlement_true(got, "com.apple.developer.icloud-services"));
    CHECK(macho_entitlements(thin(kXml, false)).empty());
    std::vector<uint8_t> cut = thin(kXml);
    cut.resize(cut.size() - 20);  // the blob runs past the end
    CHECK(macho_entitlements(cut).empty());
    CHECK(macho_entitlements({}).empty());
    CHECK(macho_entitlements(std::vector<uint8_t>(64, 0)).empty());
}

void test_fat() {
    const std::vector<uint8_t> other = thin("<plist>x86</plist>"), arm = thin(kXml);
    std::vector<uint8_t> b;
    put_be(b, 0xcafebabe);
    put_be(b, 2);
    const uint32_t first = 0x1000, second = first + 0x1000;
    put_be(b, 0x01000007);  // x86_64 first: the reader must pick arm64
    put_be(b, 3);
    put_be(b, first);
    put_be(b, uint32_t(other.size()));
    put_be(b, 12);
    put_be(b, 0x0100000c);
    put_be(b, 0);
    put_be(b, second);
    put_be(b, uint32_t(arm.size()));
    put_be(b, 12);
    b.resize(first);
    b.insert(b.end(), other.begin(), other.end());
    b.resize(second);
    b.insert(b.end(), arm.begin(), arm.end());
    CHECK(macho_entitlements(b) == kXml);
}

// From disk, reading only headers and signature: same result as from memory.
void test_file() {
    const auto dir = std::filesystem::temp_directory_path();
    const auto path = dir / "lgp_test_code_signature.bin";
    const std::vector<uint8_t> b = thin(kXml);
    {
        std::ofstream f(path, std::ios::binary);
        f.write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
    }
    bool readable = false;
    CHECK(macho_file_entitlements(path, &readable) == kXml && readable);
    {
        std::ofstream f(path, std::ios::binary);  // truncated signature
        f.write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size() - 20));
    }
    CHECK(macho_file_entitlements(path, &readable).empty() && readable);
    std::filesystem::remove(path);
    CHECK(macho_file_entitlements(path, &readable).empty() && !readable);
}

void test_profile() {
    std::string cms = std::string("\x30\x82\x10\x00junk", 8) + "<?xml version=\"1.0\"?><plist><dict>"
                      "<key>Entitlements</key><dict><key>com.apple.developer.kernel.increased-memory-limit</key>"
                      "<true/></dict></dict></plist>" + std::string("\x00\xa0signature", 11);
    const std::string plist = profile_plist(std::vector<uint8_t>(cms.begin(), cms.end()));
    CHECK(plist.rfind("<?xml", 0) == 0 && plist.size() > 8 && plist.substr(plist.size() - 8) == "</plist>");
    CHECK(entitlement_true(plist, "com.apple.developer.kernel.increased-memory-limit"));
    CHECK(profile_plist(std::vector<uint8_t>{'a', 'b'}).empty());
}

}  // namespace

int main() {
    RUN(test_thin);
    RUN(test_fat);
    RUN(test_file);
    RUN(test_profile);
    return finish();
}
