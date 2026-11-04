// Unpacks the textures of one layout archive of the game into PNG files, to
// see which picture is which before the menus use them:
//
//   lgp_ui_dump ARCHIVE.arc --out DIR
//
// Every BNTX inside (timg/__Combined.bntx) is decoded (level 0, RGBA8) and
// written as DIR/<texture>.png; a line per texture names size and format.
// Every layout (blyt/*.bflyt) is listed as its tree of panes (type, name,
// position, size, alpha, material) with the materials (colours, textures).
// Animations (anim/*.bflan) are listed with their curves.
// The pictures are the game's: DIR belongs in $LGP_PRIVATE_DIR.
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <vector>

#include "lgp/platform/ui/bntx.hpp"
#include "lgp/platform/ui/layout.hpp"
#include "lgp/platform/ui/sarc.hpp"

namespace fs = std::filesystem;
using namespace lgp::platform::ui;

namespace {

uint32_t crc32(const uint8_t* p, size_t n, uint32_t crc = 0) {
    crc = ~crc;
    for (size_t i = 0; i < n; ++i) {
        crc ^= p[i];
        for (int k = 0; k < 8; ++k)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

void be32(std::vector<uint8_t>& v, uint32_t x) {
    for (int i = 3; i >= 0; --i)
        v.push_back(uint8_t(x >> (8 * i)));
}

void chunk(std::vector<uint8_t>& png, const char* type, const std::vector<uint8_t>& body) {
    be32(png, uint32_t(body.size()));
    std::vector<uint8_t> crc_part(type, type + 4);
    crc_part.insert(crc_part.end(), body.begin(), body.end());
    png.insert(png.end(), crc_part.begin(), crc_part.end());
    be32(png, crc32(crc_part.data(), crc_part.size()));
}

// RGBA8 PNG with stored (uncompressed) deflate blocks: large but simple.
std::vector<uint8_t> png_rgba(const UiImage& img) {
    std::vector<uint8_t> raw;
    for (uint32_t y = 0; y < img.height; ++y) {
        raw.push_back(0);  // filter: none
        raw.insert(raw.end(), img.rgba.begin() + std::ptrdiff_t(size_t(y) * img.width * 4),
                   img.rgba.begin() + std::ptrdiff_t(size_t(y + 1) * img.width * 4));
    }
    std::vector<uint8_t> z = {0x78, 0x01};
    for (size_t at = 0; at < raw.size() || at == 0;) {
        const size_t n = std::min<size_t>(65535, raw.size() - at);
        const bool last = at + n >= raw.size();
        z.push_back(last ? 1 : 0);
        z.push_back(uint8_t(n)), z.push_back(uint8_t(n >> 8));
        z.push_back(uint8_t(~n)), z.push_back(uint8_t(~n >> 8));
        z.insert(z.end(), raw.begin() + std::ptrdiff_t(at), raw.begin() + std::ptrdiff_t(at + n));
        at += n;
        if (last)
            break;
    }
    uint32_t a = 1, b = 0;
    for (const uint8_t c : raw)
        a = (a + c) % 65521, b = (b + a) % 65521;
    be32(z, b << 16 | a);
    std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    std::vector<uint8_t> ihdr;
    be32(ihdr, img.width);
    be32(ihdr, img.height);
    ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});  // 8 bit, RGBA
    chunk(png, "IHDR", ihdr);
    chunk(png, "IDAT", z);
    chunk(png, "IEND", {});
    return png;
}

struct Le {
    std::span<const uint8_t> d;
    bool has(size_t at, size_t n) const { return at <= d.size() && n <= d.size() - at; }
    uint16_t u16(size_t at) const { return uint16_t(d[at] | d[at + 1] << 8); }
    uint32_t u32(size_t at) const {
        return uint32_t(d[at]) | uint32_t(d[at + 1]) << 8 | uint32_t(d[at + 2]) << 16 | uint32_t(d[at + 3]) << 24;
    }
    float f32(size_t at) const {
        const uint32_t x = u32(at);
        float f;
        std::memcpy(&f, &x, 4);
        return f;
    }
    std::string str(size_t at, size_t max) const {
        std::string s;
        for (size_t i = 0; i < max && at + i < d.size() && d[at + i] != 0; ++i)
            s.push_back(char(d[at + i]));
        return s;
    }
};

// Pane tree and materials of a BFLYT (little endian, version 8 and later
// as on the Switch). Sections: txl1 texture names, mat1 materials, pan1/pic1/
// txt1/wnd1/bnd1/prt1 panes (common header: flags, origin, alpha, scale,
// name[24], user data[8], translate xyz, rotate xyz, scale xy, size xy),
// pas1/pae1 open and close a pane's children.
void list_layout(const std::string& name, std::span<const uint8_t> data) {
    const Le b{data};
    if (!b.has(0, 0x14) || std::memcmp(data.data(), "FLYT", 4) != 0)
        return;
    const uint32_t version = b.u32(8);
    std::printf("layout %s (version 0x%08X)\n", name.c_str(), version);
    std::vector<std::string> textures, materials;
    size_t at = b.u16(6);
    const uint16_t sections = b.u16(0x10);
    int depth = 0;
    for (uint16_t i = 0; i < sections && b.has(at, 8); ++i) {
        const std::string magic(reinterpret_cast<const char*>(&data[at]), 4);
        const uint32_t size = b.u32(at + 4);
        if (size < 8 || !b.has(at, size))
            break;
        const size_t body = at + 8;
        if (magic == "lyt1") {
            std::printf("  screen %.0fx%.0f\n", b.f32(body + 4), b.f32(body + 8));
        } else if (magic == "txl1") {
            const uint16_t n = b.u16(body);
            for (uint16_t k = 0; k < n; ++k)
                textures.push_back(b.str(body + 4 + b.u32(body + 4 + size_t(k) * 4), 128));
        } else if (magic == "mat1") {
            const uint16_t n = b.u16(body);
            for (uint16_t k = 0; k < n; ++k) {
                const size_t m = at + b.u32(body + 4 + size_t(k) * 4);
                if (!b.has(m, 0x28))
                    break;
                const std::string mname = b.str(m, 0x1C);
                materials.push_back(mname);
                const uint32_t black = b.u32(m + 0x1C), white = b.u32(m + 0x20), flags = b.u32(m + 0x24);
                std::printf("  material %u %s black #%08X white #%08X flags 0x%08X textures", k, mname.c_str(),
                            black, white, flags);
                for (uint32_t t = 0; t < (flags & 3); ++t) {
                    const uint16_t idx = b.u16(m + 0x28 + t * 4);
                    std::printf(" %s", idx < textures.size() ? textures[idx].c_str() : "?");
                }
                std::printf("\n");
            }
        } else if (magic == "pas1") {
            ++depth;
        } else if (magic == "pae1") {
            --depth;
        } else if (magic == "pan1" || magic == "pic1" || magic == "txt1" || magic == "wnd1" || magic == "bnd1" ||
                   magic == "prt1") {
            const uint8_t flags = data[body], origin = data[body + 1], alpha = data[body + 2];
            const std::string pname = b.str(body + 4, 24);
            std::printf("  %*s%s %s at %.1f,%.1f size %.1fx%.1f scale %.2f,%.2f alpha %u origin %u%s", depth * 2, "",
                        magic.c_str(), pname.c_str(), b.f32(body + 0x24), b.f32(body + 0x28), b.f32(body + 0x44),
                        b.f32(body + 0x48), b.f32(body + 0x3C), b.f32(body + 0x40), alpha, origin,
                        (flags & 1) ? "" : " hidden");
            if (magic == "pic1" && b.has(body + 0x4C, 0x14)) {
                const uint16_t mat = b.u16(body + 0x4C + 0x10);
                std::printf(" material %s colours #%08X #%08X #%08X #%08X",
                            mat < materials.size() ? materials[mat].c_str() : "?", b.u32(body + 0x4C),
                            b.u32(body + 0x50), b.u32(body + 0x54), b.u32(body + 0x58));
            }
            if (magic == "wnd1" && b.has(body + 0x4C, 0x1C))
                std::printf(" inflation %d,%d,%d,%d frames %u,%u,%u,%u count %u window flags 0x%02X",
                            int16_t(b.u16(body + 0x4C)), int16_t(b.u16(body + 0x4E)), int16_t(b.u16(body + 0x50)),
                            int16_t(b.u16(body + 0x52)), b.u16(body + 0x54), b.u16(body + 0x56), b.u16(body + 0x58),
                            b.u16(body + 0x5A), data[body + 0x5C], data[body + 0x5D]);
            if (magic == "txt1" && b.has(body + 0x4C, 4)) {
                const uint16_t mat = b.u16(body + 0x4C + 4);
                std::printf(" material %s", mat < materials.size() ? materials[mat].c_str() : "?");
            }
            if (magic == "prt1" && b.has(body + 0x4C, 0x20)) {
                const uint32_t props = b.u32(body + 0x4C);
                const size_t name_at = body + 0x4C + 0x0C + size_t(props) * 0x28;
                std::printf(" part %s props %u magnify %.2f,%.2f",
                            b.has(name_at, 24) ? b.str(name_at, 24).c_str() : "?", props, b.f32(body + 0x4C + 4),
                            b.f32(body + 0x4C + 8));
                // Each property: the pane it replaces and, with a pane block,
                // that block's placement and size; usage flags and the basic
                // info offset as numbers.
                for (uint32_t k = 0; k < props && k < 64; ++k) {
                    const size_t e = body + 0x4C + 0x0C + size_t(k) * 0x28;
                    if (!b.has(e, 0x28))
                        break;
                    std::printf("\n  %*s  property %s usage %u/%u/%u", depth * 2, "", b.str(e, 24).c_str(),
                                data[e + 0x18], data[e + 0x19], data[e + 0x1A]);
                    const uint32_t pane_at = b.u32(e + 0x1C), basic_at = b.u32(e + 0x24);
                    const size_t blk = at + pane_at;
                    if (pane_at != 0 && b.has(blk, 0x54))
                        std::printf(" pane %s %s at %.1f,%.1f rotate %.1f size %.1fx%.1f scale %.2f,%.2f origin %u",
                                    std::string(reinterpret_cast<const char*>(&data[blk]), 4).c_str(),
                                    b.str(blk + 8 + 4, 24).c_str(), b.f32(blk + 8 + 0x24), b.f32(blk + 8 + 0x28),
                                    b.f32(blk + 8 + 0x38), b.f32(blk + 8 + 0x44), b.f32(blk + 8 + 0x48),
                                    b.f32(blk + 8 + 0x3C), b.f32(blk + 8 + 0x40), data[blk + 8 + 1]);
                    if (basic_at != 0 && b.has(at + basic_at, 0x34)) {
                        const size_t bi = at + basic_at;
                        std::printf(" basic at %.1f,%.1f scale %.2f,%.2f size %.1fx%.1f alpha %u", b.f32(bi + 8),
                                    b.f32(bi + 12), b.f32(bi + 32), b.f32(bi + 36), b.f32(bi + 40), b.f32(bi + 44),
                                    data[bi + 48]);
                    }
                }
            }
            std::printf("\n");
        }
        at += size;
    }
}

std::string safe_name(std::string s) {
    for (char& c : s)
        if (c == '/' || c == '\\' || c == ':' || c == '^' || c == '*' || c == '?' || c == '"' || c == '<' ||
            c == '>' || c == '|')
            c = '_';
    return s.empty() ? std::string("unnamed") : s;
}

}  // namespace

int main(int argc, char** argv) {
    std::string archive, out;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--out" && i + 1 < argc)
            out = argv[++i];
        else
            archive = a;
    }
    if (archive.empty() || out.empty()) {
        std::fprintf(stderr, "usage: lgp_ui_dump ARCHIVE --out DIR\n");
        return 2;
    }
    std::ifstream in(archive, std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::string error;
    auto sarc = Sarc::parse(std::move(bytes), &error);
    if (!sarc) {
        std::fprintf(stderr, "%s: %s\n", archive.c_str(), error.c_str());
        return 1;
    }
    fs::create_directories(out);
    int written = 0;
    for (const auto& e : sarc->entries()) {
        const auto f = sarc->file(e);
        if (f.size() >= 4 && std::memcmp(f.data(), "FLYT", 4) == 0)
            list_layout(e.name, f);
        if (f.size() >= 4 && std::memcmp(f.data(), "FLYT", 4) == 0) {
            // As the app reads it (parse_bflyt): materials, and the parts'
            // replaced panes with their texture coordinates.
            if (const auto l = parse_bflyt(f, &error)) {
                for (const std::string& w : l->warnings)
                    std::printf("  app: warning %s\n", w.c_str());
                for (const LayoutMaterial& m : l->materials) {
                    std::printf("  app: material %s black %u,%u,%u,%u white %u,%u,%u,%u flags 0x%X maps", m.name.c_str(),
                                m.black.r, m.black.g, m.black.b, m.black.a, m.white.r, m.white.g, m.white.b, m.white.a,
                                m.flags);
                    for (const LayoutTexMap& t : m.maps)
                        std::printf(" %s wrap %u/%u", t.texture >= 0 ? l->textures[size_t(t.texture)].c_str() : "-",
                                    t.wrap_s, t.wrap_t);
                    for (const LayoutTexSrt& t : m.srts)
                        std::printf(" srt %.2f,%.2f rot %.1f scale %.2f,%.2f", t.tx, t.ty, t.rotate, t.sx, t.sy);
                    if (m.blend_op >= 0)
                        std::printf(" blend %d %d %d", m.blend_op, m.blend_src, m.blend_dst);
                    std::printf("\n");
                }
                for (const LayoutPane& p : l->panes)
                    for (const LayoutPartOverride& o : p.overrides) {
                        std::printf("  app: part %s property %s usage %u/%u/%u sets 0x%X size %.1fx%.1f scale %.2f,%.2f "
                                    "material %s",
                                    p.name.c_str(), o.name.c_str(), o.usage[0], o.usage[1], o.usage[2], o.set, o.width,
                                    o.height, o.scale[0], o.scale[1],
                                    o.material >= 0 && o.material < int(l->materials.size())
                                        ? l->materials[size_t(o.material)].name.c_str()
                                        : "-");
                        std::printf("\n");
                    }
            }
        }
        if (f.size() >= 4 && std::memcmp(f.data(), "FLAN", 4) == 0) {
            // Animations: per pane or material the curves with their first
            // and last value.
            if (const auto a = parse_bflan(f, &error)) {
                std::printf("animation %s frames %.0f%s\n", e.name.c_str(), a->frames, a->loop ? " loop" : "");
                for (const LayoutAnimGroup& g : a->groups)
                    for (const LayoutCurve& c : g.curves)
                        std::printf("  %s %s %u: %zu keys %.2f .. %.2f\n", g.name.c_str(), c.tag.c_str(), c.target,
                                    c.keys.size(), c.keys.empty() ? 0.0f : c.keys.front().value,
                                    c.keys.empty() ? 0.0f : c.keys.back().value);
            }
        }
        if (f.size() < 4 || std::memcmp(f.data(), "BNTX", 4) != 0)
            continue;
        std::vector<std::string> skipped;
        const auto images = decode_bntx(f, &error, &skipped);
        for (const auto& img : images) {
            const auto png = png_rgba(img);
            const fs::path path = fs::path(out) / (safe_name(img.name) + ".png");
            std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(png.data()), std::streamsize(png.size()));
            std::printf("%s\t%s\t%ux%u\tformat 0x%04X\n", e.name.c_str(), img.name.c_str(), img.width, img.height,
                        img.format);
            ++written;
        }
        for (const auto& s : skipped)
            std::printf("%s\t%s\tskipped (format)\n", e.name.c_str(), s.c_str());
    }
    std::printf("%d textures written to %s\n", written, out.c_str());
    return 0;
}
