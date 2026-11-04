#include "lgp/platform/ui/sarc.hpp"

#include <algorithm>
#include <cstring>

namespace lgp::platform::ui {
namespace {

uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

struct Reader {
    std::span<const uint8_t> d;
    bool little = true;
    bool ok(size_t at, size_t n) const { return at <= d.size() && n <= d.size() - at; }
    uint16_t u16(size_t at) const {
        return little ? uint16_t(d[at] | d[at + 1] << 8) : uint16_t(d[at] << 8 | d[at + 1]);
    }
    uint32_t u32(size_t at) const {
        return little ? uint32_t(d[at]) | uint32_t(d[at + 1]) << 8 | uint32_t(d[at + 2]) << 16 | uint32_t(d[at + 3]) << 24
                      : be32(&d[at]);
    }
};

}  // namespace

std::optional<std::vector<uint8_t>> yaz0_decompress(std::span<const uint8_t> in) {
    if (in.size() < 16 || std::memcmp(in.data(), "Yaz0", 4) != 0)
        return std::nullopt;
    const uint32_t size = be32(&in[4]);
    std::vector<uint8_t> out;
    out.reserve(size);
    size_t src = 16;
    while (out.size() < size) {
        if (src >= in.size())
            return std::nullopt;
        const uint8_t code = in[src++];
        for (int bit = 7; bit >= 0 && out.size() < size; --bit) {
            if (code & (1u << bit)) {
                if (src >= in.size())
                    return std::nullopt;
                out.push_back(in[src++]);
                continue;
            }
            if (src + 2 > in.size())
                return std::nullopt;
            const uint8_t b1 = in[src++], b2 = in[src++];
            const size_t dist = ((size_t(b1 & 0x0F) << 8) | b2) + 1;
            size_t n = b1 >> 4;
            if (n == 0) {
                if (src >= in.size())
                    return std::nullopt;
                n = size_t(in[src++]) + 0x12;
            } else {
                n += 2;
            }
            if (dist > out.size())
                return std::nullopt;
            for (size_t i = 0; i < n && out.size() < size; ++i)
                out.push_back(out[out.size() - dist]);  // may overlap: byte by byte
        }
    }
    return out;
}

uint32_t sarc_name_hash(const std::string& name, uint32_t key) {
    uint32_t h = 0;
    for (const char c : name)
        h = h * key + uint32_t(static_cast<int8_t>(c));  // signed chars, as the format's tools do
    return h;
}

std::optional<Sarc> Sarc::parse(std::vector<uint8_t> bytes, std::string* error) {
    const auto fail = [&](const char* why) -> std::optional<Sarc> {
        if (error != nullptr)
            *error = why;
        return std::nullopt;
    };
    if (bytes.size() >= 4 && std::memcmp(bytes.data(), "Yaz0", 4) == 0) {
        auto raw = yaz0_decompress(bytes);
        if (!raw)
            return fail("broken Yaz0 data");
        bytes = std::move(*raw);
    }
    Reader r{bytes};
    if (!r.ok(0, 0x14) || std::memcmp(bytes.data(), "SARC", 4) != 0)
        return fail("not a SARC archive");
    r.little = bytes[6] == 0xFF && bytes[7] == 0xFE;
    const uint16_t header = r.u16(4);
    const uint32_t data_offset = r.u32(0x0C);
    const size_t sfat = header;
    if (!r.ok(sfat, 0x0C) || std::memcmp(&bytes[sfat], "SFAT", 4) != 0)
        return fail("no SFAT block");
    const uint16_t count = r.u16(sfat + 6);
    const size_t nodes = sfat + r.u16(sfat + 4);
    const size_t sfnt = nodes + size_t(count) * 16;
    if (!r.ok(sfnt, 8) || std::memcmp(&bytes[sfnt], "SFNT", 4) != 0)
        return fail("no SFNT block");
    const size_t names = sfnt + r.u16(sfnt + 4);
    if (data_offset > bytes.size())
        return fail("data offset past the end");
    Sarc sarc;
    for (uint16_t i = 0; i < count; ++i) {
        const size_t n = nodes + size_t(i) * 16;
        Entry e;
        e.hash = r.u32(n);
        const uint32_t attrs = r.u32(n + 4);
        e.begin = data_offset + r.u32(n + 8);
        e.end = data_offset + r.u32(n + 12);
        if (e.end < e.begin || e.end > bytes.size())
            return fail("entry past the end");
        if (attrs >> 24) {
            size_t at = names + size_t(attrs & 0xFFFF) * 4;
            if (at >= bytes.size())
                return fail("name past the end");
            const size_t end = std::min(bytes.size(), size_t(data_offset));
            while (at < end && bytes[at] != 0)
                e.name.push_back(char(bytes[at++]));
        }
        sarc.entries_.push_back(std::move(e));
    }
    sarc.bytes_ = std::move(bytes);
    return sarc;
}

std::span<const uint8_t> Sarc::file(const Entry& e) const {
    return std::span<const uint8_t>(bytes_).subspan(e.begin, e.end - e.begin);
}

std::span<const uint8_t> Sarc::file(const std::string& name) const {
    for (const Entry& e : entries_)
        if (e.name == name)
            return file(e);
    return {};
}

}  // namespace lgp::platform::ui
