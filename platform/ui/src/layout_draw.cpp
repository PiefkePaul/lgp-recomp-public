#include "layout_draw.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace lgp::platform::ui {

namespace {

// 2D affine map of the layout's plane (y up): x' = a x + c y + tx,
// y' = b x + d y + ty.
struct Aff {
    float a = 1, b = 0, c = 0, d = 1, tx = 0, ty = 0;
    ImVec2 apply(float x, float y) const { return {a * x + c * y + tx, b * x + d * y + ty}; }
    Aff then(const Aff& m) const {  // this * m: m first
        return {a * m.a + c * m.b, b * m.a + d * m.b, a * m.c + c * m.d, b * m.c + d * m.d,
                a * m.tx + c * m.ty + tx, b * m.tx + d * m.ty + ty};
    }
};

struct Rect {
    float x0, y0, x1, y1;  // y0 bottom, y1 top
};

// Inside a part pane, translations and sizes of the part's panes are
// multiplied by its magnification (nested parts: the product).
struct Mag {
    float x = 1, y = 1;
};

// The pane's rectangle around its own position, by its origin.
Rect pane_rect(const LayoutPane& p, Mag g = {}) {
    const float w = p.width * g.x, h = p.height * g.y;
    const int ox = p.origin & 3, oy = (p.origin >> 2) & 3;
    const float x0 = ox == 1 ? 0.0f : ox == 2 ? -w : -w * 0.5f;
    const float y0 = oy == 1 ? -h : oy == 2 ? 0.0f : -h * 0.5f;
    return {x0, y0, x0 + w, y0 + h};
}

// Where in the parent's rectangle the pane's translation starts (the
// parent's rectangle given for a part's top panes: the part pane's).
ImVec2 anchor(const Layout& l, const LayoutPane& p, Mag g, const Rect* parent_rect) {
    if (p.parent < 0 && parent_rect == nullptr)
        return {0, 0};
    const Rect r = parent_rect != nullptr ? *parent_rect : pane_rect(l.panes[size_t(p.parent)], g);
    const int px = (p.origin >> 4) & 3, py = (p.origin >> 6) & 3;
    return {px == 1 ? r.x0 : px == 2 ? r.x1 : (r.x0 + r.x1) * 0.5f,
            py == 1 ? r.y1 : py == 2 ? r.y0 : (r.y0 + r.y1) * 0.5f};
}

Aff local(const Layout& l, const LayoutPane& p, Mag g = {}, const Rect* parent_rect = nullptr) {
    const ImVec2 at = anchor(l, p, g, parent_rect);
    const float rad = p.rotate[2] * 3.14159265f / 180.0f;
    const float cs = std::cos(rad), sn = std::sin(rad);
    return {cs * p.scale[0], sn * p.scale[0], -sn * p.scale[1], cs * p.scale[1], at.x + p.translate[0] * g.x,
            at.y + p.translate[1] * g.y};
}

using Uv = std::array<ImVec2, 4>;  // top left, top right, bottom left, bottom right

Uv flip(Uv q, uint8_t f) {
    switch (f) {
    case 1: return {q[1], q[0], q[3], q[2]};
    case 2: return {q[2], q[3], q[0], q[1]};
    case 3: return {q[2], q[0], q[3], q[1]};
    case 4: return {q[3], q[2], q[1], q[0]};
    case 5: return {q[1], q[3], q[0], q[2]};
    default: return q;
    }
}

Uv srt(Uv q, const LayoutMaterial& m) {
    if (m.srts.empty())
        return q;
    const LayoutTexSrt& s = m.srts[0];
    const float rad = s.rotate * 3.14159265f / 180.0f, cs = std::cos(rad), sn = std::sin(rad);
    // Scaled and rotated about the middle, then moved; both axes alike (a
    // quarter mirrored into a whole box takes translate 0.5, scale 2).
    for (ImVec2& uv : q) {
        const float u = (uv.x - 0.5f) * s.sx, v = (uv.y - 0.5f) * s.sy;
        uv = {u * cs - v * sn + 0.5f + s.tx, u * sn + v * cs + 0.5f + s.ty};
    }
    return q;
}

ImU32 mul(const LayoutColor& c, const ImVec4& tint, float alpha) {
    return ImGui::ColorConvertFloat4ToU32(ImVec4(c.r / 255.0f * tint.x, c.g / 255.0f * tint.y, c.b / 255.0f * tint.z,
                                                 c.a / 255.0f * tint.w * alpha));
}

struct Painter {
    const LayoutDraw& d;
    ImVec2 center;
    float scale;
    float scale_y;  // vertical (a band stretched over the screen), else scale

    ImVec2 screen(const Aff& m, float x, float y) const {
        const ImVec2 p = m.apply(x, y);
        return {center.x + p.x * scale, center.y - p.y * scale_y};
    }

    // A quad of rectangle r with uvs and corner colours, with the material's
    // texture (or its colour alone).
    void quad(const Layout& l, const Aff& m, Rect r, int material, Uv uv, const LayoutColor* colors, float alpha,
              const LayoutPane* picture = nullptr) {
        ImTextureID id = ImTextureID_Invalid;
        ImVec4 tint(1, 1, 1, 1);
        const LayoutMaterial* mat = material >= 0 && material < int(l.materials.size())
                                        ? &l.materials[size_t(material)]
                                        : nullptr;
        bool textured = picture != nullptr && d.picture && d.picture(l, *picture, &id, &tint);
        if (textured) {
            mat = nullptr;
            uv = {ImVec2(0, 0), ImVec2(1, 0), ImVec2(0, 1), ImVec2(1, 1)};
        }
        if (mat != nullptr) {
            textured = d.texture && d.texture(l, *mat, &id, &tint);
            if (!textured)
                tint = ImVec4(mat->white.r / 255.0f, mat->white.g / 255.0f, mat->white.b / 255.0f,
                              mat->white.a / 255.0f);
            else
                uv = srt(uv, *mat);
        }
        // A material whose texture is missing (not decoded) is left out
        // rather than drawn as a white sheet.
        if (!textured && mat != nullptr && !mat->maps.empty())
            return;
        // Coordinates beyond the texture with repeat or mirror wrap (a
        // quarter texture mirrored over a box): one quad per copy, the
        // sampler clamps.
        if (textured && mat != nullptr && !mat->maps.empty() && (mat->maps[0].wrap_s != 0 || mat->maps[0].wrap_t != 0) &&
            uv[0].y == uv[1].y && uv[2].y == uv[3].y && uv[0].x == uv[2].x && uv[1].x == uv[3].x) {
            const float u0 = uv[0].x, u1 = uv[1].x, v0 = uv[0].y, v1 = uv[2].y;
            const auto beyond = [](float a, float b, uint8_t wrap) {
                return wrap != 0 && (std::min(a, b) < -1e-3f || std::max(a, b) > 1.001f);
            };
            const bool split_u = beyond(u0, u1, mat->maps[0].wrap_s), split_v = beyond(v0, v1, mat->maps[0].wrap_t);
            if ((split_u || split_v) && std::fabs(u1 - u0) < 16 && std::fabs(v1 - v0) < 16) {
                // Cuts of [a, b] at whole numbers (where it splits), as
                // fractions t of the span with the texture span of each piece.
                struct Piece {
                    float t0, t1, c0, c1;
                };
                const auto pieces = [](float a, float b, bool split, uint8_t wrap) {
                    std::vector<Piece> out;
                    if (!split || a == b) {
                        out.push_back({0, 1, a, b});
                        return out;
                    }
                    const float lo = std::min(a, b), hi = std::max(a, b);
                    for (float k = std::floor(lo); k < hi; k += 1) {
                        const float p0 = std::max(lo, k), p1 = std::min(hi, k + 1);
                        if (p1 - p0 < 1e-5f)
                            continue;
                        float c0 = p0 - k, c1 = p1 - k;
                        if (wrap == 2 && (int64_t(k) & 1)) {
                            c0 = 1 - c0;
                            c1 = 1 - c1;
                        }
                        float t0 = (p0 - a) / (b - a), t1 = (p1 - a) / (b - a);
                        if (t0 > t1) {
                            std::swap(t0, t1);
                            std::swap(c0, c1);
                        }
                        out.push_back({t0, t1, c0, c1});
                    }
                    return out;
                };
                const auto us = pieces(u0, u1, split_u, mat->maps[0].wrap_s);
                const auto vs = pieces(v0, v1, split_v, mat->maps[0].wrap_t);
                // Corner colours over the pieces: bilinear.
                const LayoutColor white;
                const LayoutColor* cc = colors ? colors : nullptr;
                const auto lerp_color = [&](float tx, float ty) {
                    if (cc == nullptr)
                        return white;
                    const auto ch = [&](uint8_t LayoutColor::*m) {
                        const float top = cc[0].*m + (cc[1].*m - cc[0].*m) * tx;
                        const float bottom = cc[2].*m + (cc[3].*m - cc[2].*m) * tx;
                        return uint8_t(std::clamp(top + (bottom - top) * ty, 0.0f, 255.0f) + 0.5f);
                    };
                    return LayoutColor{ch(&LayoutColor::r), ch(&LayoutColor::g), ch(&LayoutColor::b),
                                       ch(&LayoutColor::a)};
                };
                const float w = r.x1 - r.x0, h = r.y1 - r.y0;
                ImDrawList* dl = d.list;
                dl->PushTexture(ImTextureRef(id));
                for (const Piece& pu : us)
                    for (const Piece& pv : vs) {
                        // t runs top to bottom in v, the rectangle's y up.
                        const float x0 = r.x0 + w * pu.t0, x1 = r.x0 + w * pu.t1;
                        const float y1 = r.y1 - h * pv.t0, y0 = r.y1 - h * pv.t1;
                        const ImVec2 p[4] = {screen(m, x0, y1), screen(m, x1, y1), screen(m, x0, y0), screen(m, x1, y0)};
                        const ImVec2 q[4] = {ImVec2(pu.c0, pv.c0), ImVec2(pu.c1, pv.c0), ImVec2(pu.c0, pv.c1),
                                             ImVec2(pu.c1, pv.c1)};
                        const LayoutColor c[4] = {lerp_color(pu.t0, pv.t0), lerp_color(pu.t1, pv.t0),
                                                  lerp_color(pu.t0, pv.t1), lerp_color(pu.t1, pv.t1)};
                        dl->PrimReserve(6, 4);
                        const ImDrawIdx base = ImDrawIdx(dl->_VtxCurrentIdx);
                        for (int i = 0; i < 4; ++i)
                            dl->PrimWriteVtx(p[i], q[i], mul(c[i], tint, alpha));
                        dl->PrimWriteIdx(base);
                        dl->PrimWriteIdx(ImDrawIdx(base + 1));
                        dl->PrimWriteIdx(ImDrawIdx(base + 3));
                        dl->PrimWriteIdx(base);
                        dl->PrimWriteIdx(ImDrawIdx(base + 3));
                        dl->PrimWriteIdx(ImDrawIdx(base + 2));
                    }
                dl->PopTexture();
                return;
            }
        }
        ImDrawList* dl = d.list;
        if (!textured) {
            const ImVec2 w = ImGui::GetFontTexUvWhitePixel();
            uv = {w, w, w, w};
        } else {
            dl->PushTexture(ImTextureRef(id));
        }
        const LayoutColor white;
        const ImVec2 p[4] = {screen(m, r.x0, r.y1), screen(m, r.x1, r.y1), screen(m, r.x0, r.y0),
                             screen(m, r.x1, r.y0)};
        dl->PrimReserve(6, 4);
        const ImDrawIdx base = ImDrawIdx(dl->_VtxCurrentIdx);
        for (int i = 0; i < 4; ++i)
            dl->PrimWriteVtx(p[i], uv[size_t(i)], mul(colors ? colors[i] : white, tint, alpha));
        dl->PrimWriteIdx(base);
        dl->PrimWriteIdx(ImDrawIdx(base + 1));
        dl->PrimWriteIdx(ImDrawIdx(base + 3));
        dl->PrimWriteIdx(base);
        dl->PrimWriteIdx(ImDrawIdx(base + 3));
        dl->PrimWriteIdx(ImDrawIdx(base + 2));
        if (textured)
            dl->PopTexture();
    }

    void window(const Layout& l, const LayoutPane& p, const Aff& m, Rect r, float alpha) {
        const Uv unit = {ImVec2(0, 0), ImVec2(1, 0), ImVec2(0, 1), ImVec2(1, 1)};
        const int kind = (p.window_flags >> 2) & 3;
        const float L = p.frame_size[0], R = p.frame_size[1], T = p.frame_size[2], B = p.frame_size[3];
        // The content's vertex colours tint the frames as well where the
        // window says so (the game's buttons colour their states that way).
        const LayoutColor* frame_colors = (p.window_flags & 2) ? p.colors : nullptr;
        const Uv content_uv = p.uvs.empty() ? unit : Uv{ImVec2(p.uvs[0][0][0], p.uvs[0][0][1]),
                                                        ImVec2(p.uvs[0][1][0], p.uvs[0][1][1]),
                                                        ImVec2(p.uvs[0][2][0], p.uvs[0][2][1]),
                                                        ImVec2(p.uvs[0][3][0], p.uvs[0][3][1])};
        if (kind == 1 || kind == 2) {
            // Horizontal: a cap at each end, full height, and the content
            // between them (or the caps' inner texels stretched).
            const LayoutWindowFrame& left = p.frames.empty() ? LayoutWindowFrame{} : p.frames[0];
            const LayoutWindowFrame& right = p.frames.size() >= 2 ? p.frames[1] : left;
            const Uv luv = flip(unit, p.frames.empty() ? 0 : left.flip);
            const Uv ruv = flip(unit, p.frames.size() >= 2 ? right.flip : 1);
            const Rect middle = {r.x0 + L, r.y0, r.x1 - R, r.y1};
            if (kind == 1 && !(p.window_flags & 16))
                quad(l, m, {middle.x0 - p.inflation[0], r.y0 - p.inflation[3], middle.x1 + p.inflation[1],
                            r.y1 + p.inflation[2]},
                     p.material, content_uv, p.colors, alpha);
            else if (kind == 2 && !p.frames.empty() && middle.x1 > middle.x0)
                quad(l, m, middle, left.material, Uv{luv[1], luv[1], luv[3], luv[3]}, frame_colors, alpha);
            if (!p.frames.empty()) {
                quad(l, m, {r.x0, r.y0, r.x0 + L, r.y1}, left.material, luv, frame_colors, alpha);
                quad(l, m, {r.x1 - R, r.y0, r.x1, r.y1}, right.material, ruv, frame_colors, alpha);
            }
            return;
        }
        const Rect content = {r.x0 - p.inflation[0], r.y0 - p.inflation[3], r.x1 + p.inflation[1],
                              r.y1 + p.inflation[2]};
        if (!(p.window_flags & 16))
            quad(l, m, content, p.material, content_uv, p.colors, alpha);
        if (p.frames.empty())
            return;
        // Corners: one texture flipped into all four, or one each.
        static constexpr uint8_t kImplied[4] = {0, 1, 2, 4};
        int mat[4];
        Uv uv[4];
        for (int i = 0; i < 4; ++i) {
            const LayoutWindowFrame& f = p.frames[p.frames.size() >= 4 ? size_t(i) : 0];
            mat[i] = f.material;
            uv[i] = p.frames.size() >= 4 ? flip(unit, f.flip) : flip(unit, kImplied[i]);
        }
        const Rect corner[4] = {{r.x0, r.y1 - T, r.x0 + L, r.y1},
                                {r.x1 - R, r.y1 - T, r.x1, r.y1},
                                {r.x0, r.y0, r.x0 + L, r.y0 + B},
                                {r.x1 - R, r.y0, r.x1, r.y0 + B}};
        for (int i = 0; i < 4; ++i)
            quad(l, m, corner[i], mat[i], uv[i], frame_colors, alpha);
        // Edges: their own textures (8 frames: left, right, top, bottom) or
        // the inner texels of the neighbouring corner stretched.
        const Rect edge[4] = {{r.x0, r.y0 + B, r.x0 + L, r.y1 - T},
                              {r.x1 - R, r.y0 + B, r.x1, r.y1 - T},
                              {r.x0 + L, r.y1 - T, r.x1 - R, r.y1},
                              {r.x0 + L, r.y0, r.x1 - R, r.y0 + B}};
        if (p.frames.size() >= 8) {
            for (int i = 0; i < 4; ++i)
                quad(l, m, edge[i], p.frames[size_t(4 + i)].material, flip(unit, p.frames[size_t(4 + i)].flip),
                     frame_colors, alpha);
            return;
        }
        const Uv& tl = uv[0];
        const Uv& tr = uv[1];
        const Uv& bl = uv[2];
        const Uv& br = uv[3];
        quad(l, m, edge[0], mat[2], Uv{bl[0], bl[1], bl[0], bl[1]}, frame_colors, alpha);  // left: bottom-left's top row
        quad(l, m, edge[1], mat[1], Uv{tr[2], tr[3], tr[2], tr[3]}, frame_colors, alpha);  // right: top-right's bottom row
        quad(l, m, edge[2], mat[0], Uv{tl[1], tl[1], tl[3], tl[3]}, frame_colors, alpha);  // top: top-left's right column
        quad(l, m, edge[3], mat[3], Uv{br[0], br[0], br[2], br[2]}, frame_colors, alpha);  // bottom: bottom-right's left column
    }

    void text(const Layout& l, const LayoutPane& p, const Aff& m, Rect r, float alpha, Mag g) {
        const std::string* s = d.text ? d.text(l, p) : nullptr;
        if (s == nullptr || s->empty())
            return;
        ImFont* font = d.font != nullptr ? d.font : ImGui::GetFont();
        // Pane scale applies to the glyphs as well.
        const float sy = std::sqrt(m.c * m.c + m.d * m.d);
        float size = std::max(1.0f, p.font_size[1] * g.y * sy * scale * d.text_scale);
        ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0.0f, s->c_str());
        const ImVec2 a = screen(m, r.x0, r.y1), b = screen(m, r.x1, r.y0);
        // Longer than its share of the box (the app's texts are not the
        // game's): smaller, down to two thirds; beyond that it runs over.
        if (const float box = std::fabs(b.x - a.x) * (d.text_fit ? d.text_fit(l, p) : 0.0f);
            box > 1.0f && ts.x > box) {
            size *= std::max(2.0f / 3.0f, box / ts.x);
            ts = font->CalcTextSizeA(size, FLT_MAX, 0.0f, s->c_str());
        }
        const int ax = p.text_align & 3, ay = (p.text_align >> 2) & 3;
        const float x = ax == 1 ? a.x : ax == 2 ? b.x - ts.x : (a.x + b.x - ts.x) * 0.5f;
        const float y = ay == 1 ? a.y : ay == 2 ? b.y - ts.y : (a.y + b.y - ts.y) * 0.5f;
        d.list->AddText(font, size, ImVec2(std::round(x), std::round(y)), mul(p.text_top, ImVec4(1, 1, 1, 1), alpha),
                        s->c_str());
    }

    void pane(const Layout& l, int index, const Aff& parent, float parent_alpha, int depth, Mag g = {},
              const Rect* parent_rect = nullptr) {
        const LayoutPane& p = l.panes[size_t(index)];
        if (!p.visible() || (d.hidden && d.hidden(l, p)) || depth > 64)
            return;
        const Aff m = parent.then(local(l, p, g, parent_rect));
        const float alpha = parent_alpha * (p.alpha / 255.0f);
        const Rect r = pane_rect(p, g);
        switch (p.kind) {
        case PaneKind::Picture: {
            Uv uv = {ImVec2(0, 0), ImVec2(1, 0), ImVec2(0, 1), ImVec2(1, 1)};
            if (!p.uvs.empty())
                for (size_t i = 0; i < 4; ++i)
                    uv[i] = ImVec2(p.uvs[0][i][0], p.uvs[0][i][1]);
            quad(l, m, r, p.material, uv, p.colors, alpha, &p);
            break;
        }
        case PaneKind::Window:
            window(l, p, m, r, alpha);
            break;
        case PaneKind::Text:
            text(l, p, m, r, alpha, g);
            break;
        case PaneKind::Part:
            // The part pane stands in for the part's root: the root's
            // children hang off its rectangle, magnified.
            if (const Layout* part = d.part ? d.part(p.part) : nullptr; part != nullptr && part != &l) {
                // With the panes the part pane replaces.
                Layout replaced;
                if (!p.overrides.empty()) {
                    replaced = *part;
                    for (const LayoutPartOverride& o : p.overrides) {
                        const int i = replaced.find(o.name);
                        if (i < 0)
                            continue;
                        LayoutPane& q = replaced.panes[size_t(i)];
                        using O = LayoutPartOverride;
                        if (o.set & O::kPane) {
                            q.origin = o.origin;
                            std::copy(o.rotate, o.rotate + 3, q.rotate);
                        }
                        if (o.set & (O::kPane | O::kVisible))
                            q.flags = uint8_t((q.flags & ~1) | (o.flags & 1));
                        if (o.set & O::kTranslate)
                            std::copy(o.translate, o.translate + 3, q.translate);
                        if (o.set & O::kScale)
                            std::copy(o.scale, o.scale + 2, q.scale);
                        if (o.set & O::kSize) {
                            q.width = o.width;
                            q.height = o.height;
                        }
                        if (o.set & O::kAlpha)
                            q.alpha = o.alpha;
                        // The material is the outer layout's: copied in with
                        // the textures it names.
                        if ((o.set & O::kMaterial) && o.material >= 0 && o.material < int(l.materials.size())) {
                            LayoutMaterial mat = l.materials[size_t(o.material)];
                            for (LayoutTexMap& map : mat.maps) {
                                if (map.texture < 0 || map.texture >= int(l.textures.size()))
                                    continue;
                                const std::string& name = l.textures[size_t(map.texture)];
                                auto it = std::find(replaced.textures.begin(), replaced.textures.end(), name);
                                map.texture = int(it - replaced.textures.begin());
                                if (it == replaced.textures.end())
                                    replaced.textures.push_back(name);
                            }
                            q.material = int(replaced.materials.size());
                            replaced.materials.push_back(std::move(mat));
                        }
                    }
                    part = &replaced;
                }
                const Mag pg{g.x * p.magnify[0], g.y * p.magnify[1]};
                const LayoutPane& root = part->panes[0];
                const float root_alpha = (root.flags & 2) ? alpha * (root.alpha / 255.0f) : alpha;
                for (int c : root.children)
                    pane(*part, c, m, root_alpha, depth + 1, pg, &r);
            }
            break;
        default:
            break;
        }
        // Children take on this pane's alpha only where it says so.
        const float child_alpha = (p.flags & 2) ? alpha : parent_alpha;
        for (int c : p.children)
            pane(l, c, m, child_alpha, depth + 1, g);
    }
};

// The pane's map from its own plane to the layout's.
bool pane_map(const Layout& l, int index, Aff* out) {
    if (index < 0 || index >= int(l.panes.size()))
        return false;
    Aff m;
    for (int i = index, guard = 0; i >= 0 && guard < 64; i = l.panes[size_t(i)].parent, ++guard)
        m = local(l, l.panes[size_t(i)]).then(m);
    *out = m;
    return true;
}

}  // namespace

void draw_layout(const LayoutDraw& d, const Layout& l, ImVec2 center, float scale, float alpha, float scale_y) {
    if (l.panes.empty() || d.list == nullptr)
        return;
    Painter{d, center, scale, scale_y > 0 ? scale_y : scale}.pane(l, 0, Aff{}, alpha, 0);
}

Layout widened(const Layout& l, float dx, float min_width) {
    Layout w = l;
    if (dx == 0.0f)
        return w;
    // Each pane's x offset, x scale and x extent in the root's plane, before
    // (panes come parent first). Rotation is left out: the menu rows have none.
    const size_t n = l.panes.size();
    std::vector<float> old_tx(n), old_sx(n), x0s(n), x1s(n), new_tx(n);
    const auto left_of = [](const LayoutPane& p) {  // the rectangle's left edge from the pane's position
        const int ox = p.origin & 3;
        return ox == 1 ? 0.0f : ox == 2 ? -p.width : -p.width * 0.5f;
    };
    const auto parent_of = [&](size_t i) {
        const int p = l.panes[i].parent;
        return p < 0 || size_t(p) >= i ? -1 : p;
    };
    for (size_t i = 0; i < n; ++i) {
        const LayoutPane& p = l.panes[i];
        const int par = parent_of(i);
        const float ptx = par < 0 ? 0.0f : old_tx[size_t(par)], psx = par < 0 ? 1.0f : old_sx[size_t(par)];
        old_tx[i] = ptx + psx * (anchor(l, p, {}, nullptr).x + p.translate[0]);
        old_sx[i] = psx * p.scale[0];
        x0s[i] = old_tx[i] + old_sx[i] * left_of(p);
        x1s[i] = x0s[i] + old_sx[i] * p.width;
        if (x0s[i] > x1s[i])
            std::swap(x0s[i], x1s[i]);
    }
    const auto wide = [&](size_t i) { return x1s[i] - x0s[i] >= min_width && x0s[i] < 0 && x1s[i] > 0; };
    const auto side = [&](size_t i) {  // -1 left, 1 right, 0 middle
        const float c = (x0s[i] + x1s[i]) * 0.5f;
        return c > 0.5f ? 1 : c < -0.5f ? -1 : 0;
    };
    for (size_t i = 0; i < n; ++i) {
        const LayoutPane& p = l.panes[i];
        LayoutPane& q = w.panes[i];
        const int par = parent_of(i);
        const float psx = par < 0 ? 1.0f : old_sx[size_t(par)];
        const float ntx = par < 0 ? 0.0f : new_tx[size_t(par)];
        const float sx = old_sx[i];
        if (std::fabs(sx) < 1e-4f || std::fabs(psx) < 1e-4f) {
            new_tx[i] = ntx + psx * (anchor(w, q, {}, nullptr).x + q.translate[0]);
            continue;
        }
        const float x0 = x0s[i], x1 = x1s[i];
        float nx0, nx1;
        if (wide(i)) {  // across the middle: wider (narrower down to a fifth)
            const float grow = std::max(dx, -(x1 - x0) * 0.8f);
            nx0 = x0 - grow * 0.5f;
            nx1 = x1 + grow * 0.5f;
            q.width = p.width + grow / std::fabs(sx);
        } else {
            // Moved outwards with its side; a narrow pane across the middle
            // (a row's left arrow at its value) with the sibling it overlaps
            // most.
            int s = side(i);
            if (x0 < 0 && x1 > 0) {
                float best = 0;
                for (size_t k = 0; k < n; ++k) {
                    if (k == i || parent_of(k) != par || wide(k) || (x0s[k] < 0 && x1s[k] > 0))
                        continue;
                    const float overlap = std::min(x1, x1s[k]) - std::max(x0, x0s[k]);
                    if (overlap > best) {
                        best = overlap;
                        s = side(k);
                    }
                }
            }
            nx0 = x0 + s * dx * 0.5f;
            nx1 = x1 + s * dx * 0.5f;
        }
        // The translation that puts the new rectangle there.
        const float at = ntx + psx * anchor(w, q, {}, nullptr).x;
        const float edge = sx > 0 ? nx0 - sx * left_of(q) : nx1 - sx * left_of(q);
        q.translate[0] = (edge - at) / psx;
        new_tx[i] = at + psx * q.translate[0];
    }
    return w;
}

bool layout_pane_rect(const Layout& l, int index, ImVec2 center, float scale, ImVec2* min, ImVec2* max) {
    Aff m;
    if (!pane_map(l, index, &m))
        return false;
    const Rect r = pane_rect(l.panes[size_t(index)]);
    const Painter p{LayoutDraw{}, center, scale, scale};
    const ImVec2 c[4] = {p.screen(m, r.x0, r.y0), p.screen(m, r.x1, r.y0), p.screen(m, r.x0, r.y1),
                         p.screen(m, r.x1, r.y1)};
    *min = *max = c[0];
    for (const ImVec2& v : c) {
        min->x = std::min(min->x, v.x);
        min->y = std::min(min->y, v.y);
        max->x = std::max(max->x, v.x);
        max->y = std::max(max->y, v.y);
    }
    return true;
}

}  // namespace lgp::platform::ui
