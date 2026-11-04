#include "lgp/platform/ui/bntx.hpp"

#include <cstring>

#include "lgp/gpu/nvn/texture_layout.hpp"
#include "lgp/gpu/texture/astc.hpp"
#include "lgp/gpu/texture/bc.hpp"

namespace lgp::platform::ui {
namespace {

namespace tx = lgp::gpu::texture;

struct Bytes {
    std::span<const uint8_t> d;
    bool has(uint64_t at, uint64_t n) const { return at <= d.size() && n <= d.size() - at; }
    uint16_t u16(uint64_t at) const { return uint16_t(d[at] | d[at + 1] << 8); }
    uint32_t u32(uint64_t at) const {
        return uint32_t(d[at]) | uint32_t(d[at + 1]) << 8 | uint32_t(d[at + 2]) << 16 | uint32_t(d[at + 3]) << 24;
    }
    uint64_t u64(uint64_t at) const { return uint64_t(u32(at)) | uint64_t(u32(at + 4)) << 32; }
};

// Block size and bytes per block of a format type (high byte of the format
// word); 0 bytes: not handled.
struct FormatInfo {
    uint32_t bw = 1, bh = 1, bytes = 0;
};

FormatInfo format_info(uint32_t type) {
    switch (type) {
    case 0x02: return {1, 1, 1};   // R8
    case 0x09: return {1, 1, 2};   // R8G8
    case 0x0B: return {1, 1, 4};   // R8G8B8A8
    case 0x0C: return {1, 1, 4};   // B8G8R8A8
    case 0x1A: case 0x1D: return {4, 4, 8};             // BC1, BC4
    case 0x1B: case 0x1C: case 0x1E: case 0x20: return {4, 4, 16};  // BC2, BC3, BC5, BC7
    default: break;
    }
    // ASTC 4x4 .. 12x12 (0x2D .. 0x3A).
    static constexpr uint8_t kAstc[][2] = {{4, 4},  {5, 4},  {5, 5},  {6, 5},   {6, 6},   {8, 5},   {8, 6},
                                           {8, 8},  {10, 5}, {10, 6}, {10, 8},  {10, 10}, {12, 10}, {12, 12}};
    if (type >= 0x2D && type <= 0x3A)
        return {kAstc[type - 0x2D][0], kAstc[type - 0x2D][1], 16};
    return {};
}

// Linear blocks of one level to RGBA8.
bool to_rgba(uint32_t type, uint32_t kind, const FormatInfo& f, const std::vector<uint8_t>& lin, uint32_t w,
             uint32_t h, std::vector<uint8_t>* out) {
    out->assign(size_t(w) * h * 4, 0);
    const auto bc = [&](tx::BcFormat bf) {
        tx::decode_bc_image(bf, lin.data(), w, h, out->data());
        return true;
    };
    switch (type) {
    case 0x1A: return bc(tx::BcFormat::BC1);
    case 0x1B: return bc(tx::BcFormat::BC2);
    case 0x1C: return bc(tx::BcFormat::BC3);
    case 0x1D: return bc(kind == 2 ? tx::BcFormat::BC4Snorm : tx::BcFormat::BC4);
    case 0x1E: return bc(kind == 2 ? tx::BcFormat::BC5Snorm : tx::BcFormat::BC5);
    case 0x20: return bc(tx::BcFormat::BC7);
    default: break;
    }
    if (type >= 0x2D && type <= 0x3A) {
        tx::decode_astc_image(lin.data(), w, h, f.bw, f.bh, kind == 6, out->data());
        return true;
    }
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            const uint8_t* s = &lin[(size_t(y) * w + x) * f.bytes];
            uint8_t* d = &(*out)[(size_t(y) * w + x) * 4];
            switch (type) {
            case 0x02: d[0] = s[0], d[1] = d[2] = 0, d[3] = 255; break;
            case 0x09: d[0] = s[0], d[1] = s[1], d[2] = 0, d[3] = 255; break;
            case 0x0B: std::memcpy(d, s, 4); break;
            case 0x0C: d[0] = s[2], d[1] = s[1], d[2] = s[0], d[3] = s[3]; break;
            default: return false;
            }
        }
    return true;
}

// The texture's channel selector (one byte per output channel R, G, B, A:
// 0 zero, 1 one, 2 R, 3 G, 4 B, 5 A), as layouts use BC4/BC5 for masks.
void apply_selector(uint32_t selector, std::vector<uint8_t>* rgba) {
    const uint8_t sel[4] = {uint8_t(selector), uint8_t(selector >> 8), uint8_t(selector >> 16), uint8_t(selector >> 24)};
    if (sel[0] == 2 && sel[1] == 3 && sel[2] == 4 && sel[3] == 5)
        return;  // identity
    for (size_t i = 0; i + 3 < rgba->size(); i += 4) {
        const uint8_t src[4] = {(*rgba)[i], (*rgba)[i + 1], (*rgba)[i + 2], (*rgba)[i + 3]};
        for (int c = 0; c < 4; ++c) {
            const uint8_t s = sel[c];
            (*rgba)[i + size_t(c)] = s == 0 ? 0 : s == 1 ? 255 : s >= 2 && s <= 5 ? src[s - 2] : src[c];
        }
    }
}

}  // namespace

std::vector<UiImage> decode_bntx(std::span<const uint8_t> data, std::string* error, std::vector<std::string>* skipped) {
    std::vector<UiImage> out;
    const Bytes b{data};
    if (!b.has(0, 0x48) || std::memcmp(data.data(), "BNTX", 4) != 0 || std::memcmp(&data[0x20], "NX  ", 4) != 0) {
        *error = "not a BNTX";
        return out;
    }
    const uint32_t count = b.u32(0x24);
    const uint64_t table = b.u64(0x28);
    for (uint32_t i = 0; i < count; ++i) {
        if (!b.has(table + uint64_t(i) * 8, 8))
            break;
        const uint64_t brti = b.u64(table + uint64_t(i) * 8);
        if (!b.has(brti, 0x90) || std::memcmp(&data[brti], "BRTI", 4) != 0)
            continue;
        UiImage img;
        const uint64_t name_at = b.u64(brti + 0x60);
        if (b.has(name_at, 2)) {
            const uint16_t len = b.u16(name_at);
            if (b.has(name_at + 2, len))
                img.name.assign(reinterpret_cast<const char*>(&data[name_at + 2]), len);
        }
        img.format = b.u32(brti + 0x1C);
        img.width = b.u32(brti + 0x24);
        img.height = b.u32(brti + 0x28);
        const uint32_t layout = b.u32(brti + 0x34);
        const uint32_t selector = b.u32(brti + 0x58);
        const uint16_t tile_mode = b.u16(brti + 0x12);
        const uint64_t mips = b.u64(brti + 0x70);
        const uint32_t type = img.format >> 8, kind = img.format & 0xFF;
        const FormatInfo f = format_info(type);
        if (f.bytes == 0 || img.width == 0 || img.height == 0 || img.width > 8192 || img.height > 8192 ||
            !b.has(mips, 8)) {
            if (skipped != nullptr)
                skipped->push_back(img.name);
            continue;
        }
        const uint64_t level0 = b.u64(mips);
        std::vector<uint8_t> lin;
        if (tile_mode == 1) {  // linear (pitch) storage, rare
            const uint64_t row = uint64_t((img.width + f.bw - 1) / f.bw) * f.bytes;
            const uint64_t rows = (img.height + f.bh - 1) / f.bh;
            if (!b.has(level0, row * rows))
                continue;
            lin.assign(&data[level0], &data[level0] + row * rows);
        } else {
            const auto tl = gpu::nvn::block_linear_layout(f.bytes, f.bw, f.bh, img.width, img.height, 1, 1, 1,
                                                          int32_t(layout & 7));
            if (tl.levels.empty() || !b.has(level0, 0) ||
                !gpu::nvn::deswizzle_level(tl.levels[0], &data[level0], data.size() - level0, &lin)) {
                if (skipped != nullptr)
                    skipped->push_back(img.name);
                continue;
            }
        }
        if (!to_rgba(type, kind, f, lin, img.width, img.height, &img.rgba)) {
            if (skipped != nullptr)
                skipped->push_back(img.name);
            continue;
        }
        apply_selector(selector, &img.rgba);
        out.push_back(std::move(img));
    }
    if (out.empty() && error != nullptr && error->empty())
        *error = "no texture decoded";
    return out;
}

}  // namespace lgp::platform::ui
