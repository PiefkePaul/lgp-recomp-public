#include "lgp/platform/ui/layout.hpp"

#include <algorithm>
#include <cstring>

namespace lgp::platform::ui {

namespace {

// Little-endian reads with bounds (Switch layouts are little endian).
struct Reader {
    std::span<const uint8_t> d;
    bool has(size_t at, size_t n) const { return at <= d.size() && n <= d.size() - at; }
    uint8_t u8(size_t at) const { return has(at, 1) ? d[at] : 0; }
    uint16_t u16(size_t at) const { return has(at, 2) ? uint16_t(d[at] | d[at + 1] << 8) : 0; }
    uint32_t u32(size_t at) const {
        return has(at, 4) ? uint32_t(d[at]) | uint32_t(d[at + 1]) << 8 | uint32_t(d[at + 2]) << 16 |
                                uint32_t(d[at + 3]) << 24
                          : 0;
    }
    float f32(size_t at) const {
        const uint32_t v = u32(at);
        float f;
        std::memcpy(&f, &v, 4);
        return f;
    }
    LayoutColor color(size_t at) const { return {u8(at), u8(at + 1), u8(at + 2), u8(at + 3)}; }
    std::string str(size_t at, size_t max) const {
        std::string s;
        for (size_t i = 0; i < max && has(at + i, 1) && d[at + i] != 0; ++i)
            s.push_back(char(d[at + i]));
        return s;
    }
};

// Material headers: up to version 7 black, white, flags; later versions
// (Switch) are documented both ways, so each layout's materials are read
// the way that makes sense of all of them.
struct MaterialHead {
    size_t black, white, flags, maps;
};
constexpr MaterialHead kHeads[2] = {{0x1C, 0x20, 0x24, 0x28}, {0x24, 0x28, 0x1C, 0x2C}};

// 0 if the material makes no sense read this way, 1 if it does, 3 if it
// also names textures (read the wrong way, a black colour reads as flags
// without textures).
int plausible(const Reader& r, size_t at, size_t end, const MaterialHead& h, size_t textures) {
    const uint32_t flags = r.u32(at + h.flags);
    const uint32_t maps = flags & 3, srts = (flags >> 2) & 3;
    if (at + h.maps + maps * 4 + srts * 20 > end)
        return 0;
    for (uint32_t i = 0; i < maps; ++i)
        if (r.u16(at + h.maps + i * 4) >= textures)
            return 0;
    return maps > 0 ? 3 : 1;
}

void read_pane_head(const Reader& r, size_t body, LayoutPane* p) {
    p->flags = r.u8(body);
    p->origin = r.u8(body + 1);
    p->alpha = r.u8(body + 2);
    p->name = r.str(body + 4, 0x18);
    for (int i = 0; i < 3; ++i) {
        p->translate[i] = r.f32(body + 0x24 + size_t(i) * 4);
        p->rotate[i] = r.f32(body + 0x30 + size_t(i) * 4);
    }
    p->scale[0] = r.f32(body + 0x3C);
    p->scale[1] = r.f32(body + 0x40);
    p->width = r.f32(body + 0x44);
    p->height = r.f32(body + 0x48);
}

// Colours, material and UV sets as in pic1 and a window's content; the
// size in bytes.
size_t read_quad(const Reader& r, size_t at, LayoutPane* p) {
    for (int i = 0; i < 4; ++i)
        p->colors[i] = r.color(at + size_t(i) * 4);
    p->material = r.u16(at + 0x10);
    const uint8_t sets = r.u8(at + 0x12);
    size_t uv = at + 0x14;
    for (uint8_t s = 0; s < sets && r.has(uv, 32); ++s, uv += 32) {
        LayoutQuadUv q;
        for (int c = 0; c < 4; ++c)
            q[size_t(c)] = {r.f32(uv + size_t(c) * 8), r.f32(uv + size_t(c) * 8 + 4)};
        p->uvs.push_back(q);
    }
    return uv - at;
}

std::vector<std::string> name_table(const Reader& r, size_t body) {
    // count u16, pad u16, offsets relative to the table's start
    std::vector<std::string> out;
    const uint16_t n = r.u16(body);
    for (uint16_t i = 0; i < n; ++i)
        out.push_back(r.str(body + 4 + r.u32(body + 4 + size_t(i) * 4), 256));
    return out;
}

}  // namespace

int Layout::find(std::string_view pane) const {
    for (size_t i = 0; i < panes.size(); ++i)
        if (panes[i].name == pane)
            return int(i);
    return -1;
}

std::optional<Layout> parse_bflyt(std::span<const uint8_t> file, std::string* error) {
    const Reader r{file};
    if (!r.has(0, 0x14) || std::memcmp(file.data(), "FLYT", 4) != 0) {
        *error = "no FLYT header";
        return std::nullopt;
    }
    if (r.u16(4) != 0xFEFF) {
        *error = "not little endian";
        return std::nullopt;
    }
    Layout l;
    size_t at = r.u16(6);
    const uint16_t sections = r.u16(0x10);
    int last = -1;    // the pane read last
    int parent = -1;  // the pane new ones hang under
    for (uint16_t i = 0; i < sections && r.has(at, 8); ++i) {
        const std::string magic = r.str(at, 4);
        const uint32_t size = r.u32(at + 4);
        if (size < 8 || !r.has(at, size)) {
            l.warnings.push_back("section " + magic + " runs past the file");
            break;
        }
        const size_t body = at + 8;
        if (magic == "lyt1") {
            l.width = r.f32(body + 4);
            l.height = r.f32(body + 8);
        } else if (magic == "txl1") {
            l.textures = name_table(r, body);
        } else if (magic == "fnl1") {
            l.fonts = name_table(r, body);
        } else if (magic == "mat1") {
            const uint16_t n = r.u16(body);
            std::vector<std::pair<size_t, size_t>> spans;  // start, end of each material
            for (uint16_t k = 0; k < n; ++k) {
                const size_t m = at + r.u32(body + 4 + size_t(k) * 4);
                const size_t end = k + 1 < n ? at + r.u32(body + 4 + size_t(k + 1) * 4) : at + size;
                spans.emplace_back(m, end);
            }
            int score[2] = {0, 0};
            for (int h = 0; h < 2; ++h)
                for (const auto& [m, end] : spans)
                    score[h] += plausible(r, m, end, kHeads[h], l.textures.size());
            const MaterialHead& head = score[1] > score[0] ? kHeads[1] : kHeads[0];
            if (score[0] == score[1] && score[0] < 3 * n)
                l.warnings.push_back("material header order unclear");
            for (const auto& [m, end] : spans) {
                LayoutMaterial mat;
                mat.name = r.str(m, 0x1C);
                mat.black = r.color(m + head.black);
                mat.white = r.color(m + head.white);
                mat.flags = r.u32(m + head.flags);
                size_t p = m + head.maps;
                for (uint32_t t = 0; t < (mat.flags & 3) && p + 4 <= end; ++t, p += 4)
                    mat.maps.push_back({r.u16(p), uint8_t(r.u8(p + 2) & 3), uint8_t(r.u8(p + 3) & 3)});
                for (uint32_t t = 0; t < ((mat.flags >> 2) & 3) && p + 20 <= end; ++t, p += 20)
                    mat.srts.push_back({r.f32(p), r.f32(p + 4), r.f32(p + 8), r.f32(p + 12), r.f32(p + 16)});
                // Texture coordinate generators (their size differs between
                // versions: the one that leaves a sensible blend mode),
                // TEV stages, alpha compare, then the blend mode.
                if (mat.flags & 0x400) {
                    const uint32_t gens = (mat.flags >> 4) & 3, tevs = (mat.flags >> 6) & 7;
                    const size_t rest = size_t(tevs) * 4 + ((mat.flags >> 9) & 1) * 8;
                    for (const size_t gen_size : {size_t(0x10), size_t(0x0C), size_t(4)}) {
                        const size_t b = p + gens * gen_size + rest;
                        if (b + 4 > end)
                            continue;
                        const uint8_t op = r.u8(b), src = r.u8(b + 1), dst = r.u8(b + 2), logic = r.u8(b + 3);
                        if (op >= 1 && op <= 5 && src <= 9 && dst <= 9 && logic <= 15) {
                            mat.blend_op = op;
                            mat.blend_src = src;
                            mat.blend_dst = dst;
                            break;
                        }
                    }
                }
                for (LayoutTexMap& map : mat.maps)
                    if (map.texture >= int(l.textures.size()))
                        map.texture = -1;
                l.materials.push_back(std::move(mat));
            }
        } else if (magic == "pas1") {
            if (last >= 0)
                parent = last;
        } else if (magic == "pae1") {
            if (parent >= 0)
                parent = l.panes[size_t(parent)].parent;
        } else if (magic == "pan1" || magic == "pic1" || magic == "txt1" || magic == "wnd1" || magic == "bnd1" ||
                   magic == "prt1") {
            LayoutPane p;
            read_pane_head(r, body, &p);
            const size_t ext = body + 0x4C;  // after the common pane fields
            if (magic == "pic1") {
                p.kind = PaneKind::Picture;
                read_quad(r, ext, &p);
            } else if (magic == "txt1") {
                p.kind = PaneKind::Text;
                p.material = r.u16(ext + 4);
                p.font = r.u16(ext + 6);
                p.text_align = r.u8(ext + 8);
                p.line_align = r.u8(ext + 9);
                p.text_top = r.color(ext + 0x14);
                p.text_bottom = r.color(ext + 0x18);
                p.font_size[0] = r.f32(ext + 0x1C);
                p.font_size[1] = r.f32(ext + 0x20);
            } else if (magic == "wnd1") {
                p.kind = PaneKind::Window;
                for (int k = 0; k < 4; ++k) {
                    p.inflation[k] = float(int16_t(r.u16(ext + size_t(k) * 2)));
                    p.frame_size[k] = float(r.u16(ext + 8 + size_t(k) * 2));
                }
                const uint8_t frames = r.u8(ext + 0x10);
                p.window_flags = r.u8(ext + 0x11);
                const size_t content = at + r.u32(ext + 0x14);
                const size_t table = at + r.u32(ext + 0x18);
                read_quad(r, content, &p);
                for (uint8_t k = 0; k < frames; ++k) {
                    const size_t f = at + r.u32(table + size_t(k) * 4);
                    p.frames.push_back({r.u16(f), r.u8(f + 2)});
                }
            } else if (magic == "bnd1") {
                p.kind = PaneKind::Bounding;
            } else if (magic == "prt1") {
                p.kind = PaneKind::Part;
                const uint32_t props = r.u32(ext);
                p.magnify[0] = r.f32(ext + 4);
                p.magnify[1] = r.f32(ext + 8);
                for (float& v : p.magnify)
                    if (!(v > 0.0f && v < 100.0f))
                        v = 1.0f;
                p.part = r.str(ext + 12 + size_t(props) * 0x28, 64);
                // Properties (0x28 each): name[24], usage flags (pane,
                // basic info, material), offsets from the section's start to
                // a pane block, user data and basic info (user data[8],
                // translate xyz, rotate xyz, scale xy, size xy, alpha). Basic
                // info bits: 0 visibility set, 1 its value, 3 translate,
                // 4 size, 5 scale, 6 alpha. The material comes from the pane
                // block where the material flags say so.
                for (uint32_t k = 0; k < props && k < 64; ++k) {
                    const size_t e = ext + 12 + size_t(k) * 0x28;
                    LayoutPartOverride o;
                    o.name = r.str(e, 0x18);
                    for (int u = 0; u < 3; ++u)
                        o.usage[u] = r.u8(e + 0x18 + size_t(u));
                    const uint32_t pane_at = r.u32(e + 0x1C), basic_at = r.u32(e + 0x24);
                    if (pane_at != 0) {
                        const size_t block = at + pane_at;
                        const std::string kind = r.str(block, 4);
                        if (kind == "pan1" || kind == "pic1" || kind == "txt1" || kind == "wnd1" || kind == "bnd1" ||
                            kind == "prt1") {
                            LayoutPane head;
                            read_pane_head(r, block + 8, &head);
                            if (o.usage[0] != 0) {
                                o.set |= LayoutPartOverride::kPane | LayoutPartOverride::kTranslate |
                                         LayoutPartOverride::kScale | LayoutPartOverride::kSize |
                                         LayoutPartOverride::kAlpha;
                                o.flags = head.flags;
                                o.origin = head.origin;
                                o.alpha = head.alpha;
                                std::copy(head.translate, head.translate + 3, o.translate);
                                std::copy(head.rotate, head.rotate + 3, o.rotate);
                                std::copy(head.scale, head.scale + 2, o.scale);
                                o.width = head.width;
                                o.height = head.height;
                            }
                            if (kind == "pic1" && o.usage[2] != 0) {
                                read_quad(r, block + 8 + 0x4C, &head);
                                o.set |= LayoutPartOverride::kMaterial;
                                o.material = head.material;
                            }
                        } else {
                            l.warnings.push_back("part " + p.name + ": property " + o.name + " not read");
                        }
                    }
                    const uint8_t basic = o.usage[1];
                    if (basic_at != 0 && basic != 0 && r.has(at + basic_at, 0x34)) {
                        const size_t b = at + basic_at;
                        if (basic & 1) {
                            o.set |= LayoutPartOverride::kVisible;
                            o.flags = uint8_t((basic & 2) ? (o.flags | 1) : (o.flags & ~1));
                        }
                        if (basic & 8) {
                            o.set |= LayoutPartOverride::kTranslate;
                            for (int c = 0; c < 3; ++c)
                                o.translate[c] = r.f32(b + 8 + size_t(c) * 4);
                        }
                        if (basic & 16) {
                            o.set |= LayoutPartOverride::kSize;
                            o.width = r.f32(b + 40);
                            o.height = r.f32(b + 44);
                        }
                        if (basic & 32) {
                            o.set |= LayoutPartOverride::kScale;
                            o.scale[0] = r.f32(b + 32);
                            o.scale[1] = r.f32(b + 36);
                        }
                        if (basic & 64) {
                            o.set |= LayoutPartOverride::kAlpha;
                            o.alpha = r.u8(b + 48);
                        }
                    }
                    if (o.set != 0)
                        p.overrides.push_back(std::move(o));
                }
            }
            if (p.material >= int(l.materials.size()) && !l.materials.empty())
                p.material = -1;
            p.parent = parent;
            last = int(l.panes.size());
            if (parent >= 0)
                l.panes[size_t(parent)].children.push_back(last);
            else if (last > 0)
                l.warnings.push_back("pane " + p.name + " outside the root");
            l.panes.push_back(std::move(p));
        }
        at += size;
    }
    if (l.panes.empty()) {
        *error = "no panes";
        return std::nullopt;
    }
    return l;
}

float LayoutCurve::at(float frame) const {
    if (keys.empty())
        return 0;
    if (frame <= keys.front().frame)
        return keys.front().value;
    if (frame >= keys.back().frame)
        return keys.back().value;
    size_t i = 1;
    while (i < keys.size() && keys[i].frame < frame)
        ++i;
    const Key& a = keys[i - 1];
    const Key& b = keys[i];
    if (kind != 2)
        return a.value;
    const float span = b.frame - a.frame;
    if (span <= 0)
        return b.value;
    const float t = (frame - a.frame) / span, t2 = t * t, t3 = t2 * t;
    return (2 * t3 - 3 * t2 + 1) * a.value + (t3 - 2 * t2 + t) * a.slope * span + (-2 * t3 + 3 * t2) * b.value +
           (t3 - t2) * b.slope * span;
}

std::optional<LayoutAnimation> parse_bflan(std::span<const uint8_t> file, std::string* error) {
    const Reader r{file};
    if (!r.has(0, 0x14) || std::memcmp(file.data(), "FLAN", 4) != 0 || r.u16(4) != 0xFEFF) {
        *error = "no little-endian FLAN header";
        return std::nullopt;
    }
    LayoutAnimation a;
    size_t at = r.u16(6);
    const uint16_t sections = r.u16(0x10);
    for (uint16_t i = 0; i < sections && r.has(at, 8); ++i) {
        const std::string magic = r.str(at, 4);
        const uint32_t size = r.u32(at + 4);
        if (size < 8 || !r.has(at, size))
            break;
        if (magic == "pai1") {
            // frames u16, loop u8, pad, textures u16, groups u16, group table offset u32
            a.frames = r.u16(at + 8);
            a.loop = r.u8(at + 10) != 0;
            const uint16_t groups = r.u16(at + 14);
            const size_t table = at + r.u32(at + 16);
            for (uint16_t g = 0; g < groups; ++g) {
                const size_t e = at + r.u32(table + size_t(g) * 4);
                LayoutAnimGroup group;
                group.name = r.str(e, 0x1C);
                const uint8_t tags = r.u8(e + 0x1C);
                group.material = r.u8(e + 0x1D) == 1;
                for (uint8_t t = 0; t < tags; ++t) {
                    const size_t tag = e + r.u32(e + 0x20 + size_t(t) * 4);
                    const std::string tag_magic = r.str(tag, 4);
                    const uint8_t entries = r.u8(tag + 4);
                    for (uint8_t k = 0; k < entries; ++k) {
                        const size_t ent = tag + r.u32(tag + 8 + size_t(k) * 4);
                        LayoutCurve c;
                        c.tag = tag_magic;
                        c.target = r.u8(ent + 1);
                        c.kind = r.u8(ent + 2);
                        const uint16_t keys = r.u16(ent + 4);
                        const size_t key = ent + r.u32(ent + 8);
                        const size_t stride = c.kind == 2 ? 12 : 8;
                        for (uint16_t n = 0; n < keys && r.has(key + n * stride, stride); ++n) {
                            const size_t kp = key + n * stride;
                            if (c.kind == 2)
                                c.keys.push_back({r.f32(kp), r.f32(kp + 4), r.f32(kp + 8)});
                            else  // step: frame, u16 value
                                c.keys.push_back({r.f32(kp), float(r.u16(kp + 4)), 0});
                        }
                        group.curves.push_back(std::move(c));
                    }
                }
                a.groups.push_back(std::move(group));
            }
        }
        at += size;
    }
    return a;
}

Layout animated(const Layout& layout, const LayoutAnimation& anim, float frame) {
    Layout l = layout;
    const float f = frame < 0 ? anim.frames : frame;
    const auto byte = [](float v) { return uint8_t(std::clamp(v, 0.0f, 255.0f) + 0.5f); };
    for (const LayoutAnimGroup& g : anim.groups) {
        if (g.material) {
            for (LayoutMaterial& m : l.materials) {
                if (m.name != g.name)
                    continue;
                for (const LayoutCurve& c : g.curves) {
                    if (c.tag != "FLMC" || c.target >= 8)
                        continue;
                    LayoutColor& col = c.target < 4 ? m.black : m.white;
                    uint8_t* ch[4] = {&col.r, &col.g, &col.b, &col.a};
                    *ch[c.target & 3] = byte(c.at(f));
                }
            }
            continue;
        }
        const int index = l.find(g.name);
        if (index < 0)
            continue;
        LayoutPane& p = l.panes[size_t(index)];
        for (const LayoutCurve& c : g.curves) {
            const float v = c.at(f);
            if (c.tag == "FLPA") {
                if (c.target < 3)
                    p.translate[c.target] = v;
                else if (c.target < 6)
                    p.rotate[c.target - 3] = v;
                else if (c.target < 8)
                    p.scale[c.target - 6] = v;
                else if (c.target == 8)
                    p.width = v;
                else if (c.target == 9)
                    p.height = v;
            } else if (c.tag == "FLVI" && c.target == 0) {
                p.flags = uint8_t(v >= 0.5f ? (p.flags | 1) : (p.flags & ~1));
            } else if (c.tag == "FLVC") {
                if (c.target < 16) {
                    uint8_t* ch[4] = {&p.colors[c.target / 4].r, &p.colors[c.target / 4].g, &p.colors[c.target / 4].b,
                                      &p.colors[c.target / 4].a};
                    *ch[c.target & 3] = byte(v);
                } else if (c.target == 16) {
                    p.alpha = byte(v);
                }
            }
        }
    }
    return l;
}

}  // namespace lgp::platform::ui
