// Moving header and footer units of the game's layouts (layout_shift.hpp).
#include "lgp/platform/ui/layout_shift.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <set>

#include "lgp/platform/ui/sarc.hpp"

namespace lgp::platform::ui {

namespace {

uint32_t rd32(const std::vector<uint8_t>& v, size_t at) {
    return uint32_t(v[at]) | uint32_t(v[at + 1]) << 8 | uint32_t(v[at + 2]) << 16 | uint32_t(v[at + 3]) << 24;
}
uint16_t rd16(const std::vector<uint8_t>& v, size_t at) { return uint16_t(v[at] | v[at + 1] << 8); }
void addf(std::vector<uint8_t>& v, size_t at, float d) {
    float f;
    uint32_t u = rd32(v, at);
    std::memcpy(&f, &u, 4);
    f += d;
    std::memcpy(&u, &f, 4);
    for (int i = 0; i < 4; ++i)
        v[at + size_t(i)] = uint8_t(u >> (8 * i));
}
bool has(const std::vector<uint8_t>& v, size_t begin, size_t end, size_t at, size_t len) {
    return at >= begin && at + len <= end && at + len <= v.size();
}

bool listed(const std::vector<std::string>& list, const std::string& name) {
    return std::find(list.begin(), list.end(), name) != list.end();
}

}  // namespace

// Where each pane sits on the screen, from the root down: its bounding box
// (turned panes: the box around the turned rectangle). A turn about z turns,
// one about x or y folds that axis by its cosine.
std::vector<LayoutRect> layout_place(const Layout& l, const PartBoxes* parts) {
    constexpr float kDeg = 3.14159265358979f / 180.0f;
    const size_t n = l.panes.size();
    std::vector<LayoutRect> out(n);
    std::vector<std::array<float, 4>> m(n);  // local -> screen (2x2, row major)
    std::vector<std::array<float, 2>> at(n);  // screen position of the pane's translation point
    for (size_t i = 0; i < n; ++i) {
        const LayoutPane& p = l.panes[i];
        std::array<float, 4> pm = {1, 0, 0, 1};
        std::array<float, 2> pp = {0, 0};
        float pw = l.width, ph = l.height, pcx = 0, pcy = 0;  // parent size and centre (local)
        if (p.parent >= 0 && size_t(p.parent) < i) {
            const LayoutPane& q = l.panes[size_t(p.parent)];
            pm = m[size_t(p.parent)], pp = at[size_t(p.parent)];
            pw = q.width, ph = q.height;
            const int qx = q.origin & 3, qy = (q.origin >> 2) & 3;
            pcx = qx == 1 ? pw / 2 : qx == 2 ? -pw / 2 : 0;
            pcy = qy == 1 ? -ph / 2 : qy == 2 ? ph / 2 : 0;
        }
        const int ax = (p.origin >> 4) & 3, ay = (p.origin >> 6) & 3;
        const float lx = pcx + (ax == 1 ? -pw / 2 : ax == 2 ? pw / 2 : 0) + p.translate[0];
        const float ly = pcy + (ay == 1 ? ph / 2 : ay == 2 ? -ph / 2 : 0) + p.translate[1];
        at[i] = {pp[0] + pm[0] * lx + pm[1] * ly, pp[1] + pm[2] * lx + pm[3] * ly};
        const float sx = p.scale[0] * std::cos(p.rotate[1] * kDeg), sy = p.scale[1] * std::cos(p.rotate[0] * kDeg);
        const float a = p.rotate[2] * kDeg, ca = std::cos(a), sa = std::sin(a);
        const float r[4] = {ca * sx, -sa * sy, sa * sx, ca * sy};
        m[i] = {pm[0] * r[0] + pm[1] * r[2], pm[0] * r[1] + pm[1] * r[3], pm[2] * r[0] + pm[3] * r[2],
                pm[2] * r[1] + pm[3] * r[3]};
        const int ox = p.origin & 3, oy = (p.origin >> 2) & 3;
        const float cx = ox == 1 ? p.width / 2 : ox == 2 ? -p.width / 2 : 0;
        const float cy = oy == 1 ? -p.height / 2 : oy == 2 ? p.height / 2 : 0;
        // The rectangle in local units around (bx, by): the pane's own, or its part's content.
        float bx = cx, by = cy, bw = p.width, bh = p.height;
        if (p.kind == PaneKind::Part && parts != nullptr) {
            const auto it = parts->find(p.part);
            if (it != parts->end() && it->second.w > 0 && it->second.h > 0) {
                bx = cx + it->second.cx * p.magnify[0], by = cy + it->second.cy * p.magnify[1];
                bw = it->second.w * std::abs(p.magnify[0]), bh = it->second.h * std::abs(p.magnify[1]);
            }
        }
        float lo[2] = {1e30f, 1e30f}, hi[2] = {-1e30f, -1e30f};
        for (int c = 0; c < 4; ++c) {
            const float x = bx + (c & 1 ? bw / 2 : -bw / 2), y = by + (c & 2 ? bh / 2 : -bh / 2);
            const float sxp = at[i][0] + m[i][0] * x + m[i][1] * y, syp = at[i][1] + m[i][2] * x + m[i][3] * y;
            lo[0] = std::min(lo[0], sxp), hi[0] = std::max(hi[0], sxp);
            lo[1] = std::min(lo[1], syp), hi[1] = std::max(hi[1], syp);
        }
        out[i] = {(lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2, hi[0] - lo[0], hi[1] - lo[1]};
    }
    return out;
}

bool drawn(const LayoutPane& p) {
    return p.kind == PaneKind::Picture || p.kind == PaneKind::Text || p.kind == PaneKind::Window ||
           p.kind == PaneKind::Part;
}

LayoutRect layout_content(const Layout& l) {
    const std::vector<LayoutRect> at = layout_place(l);
    float lo[2] = {1e30f, 1e30f}, hi[2] = {-1e30f, -1e30f};
    const float hw = l.width / 2, hh = l.height / 2;
    for (size_t i = 1; i < l.panes.size(); ++i) {
        const LayoutRect& r = at[i];
        const bool drawn = l.panes[i].kind == PaneKind::Picture || l.panes[i].kind == PaneKind::Text ||
                           l.panes[i].kind == PaneKind::Window || l.panes[i].kind == PaneKind::Part;
        if (!drawn || std::abs(r.cx) - std::abs(r.w) / 2 >= hw || std::abs(r.cy) - std::abs(r.h) / 2 >= hh)
            continue;
        lo[0] = std::min(lo[0], r.cx - std::abs(r.w) / 2), hi[0] = std::max(hi[0], r.cx + std::abs(r.w) / 2);
        lo[1] = std::min(lo[1], r.cy - std::abs(r.h) / 2), hi[1] = std::max(hi[1], r.cy + std::abs(r.h) / 2);
    }
    if (lo[0] > hi[0])
        return {};
    return {(lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2, hi[0] - lo[0], hi[1] - lo[1]};
}

bool layout_drawn(const LayoutPane& p) {
    return p.kind == PaneKind::Picture || p.kind == PaneKind::Text || p.kind == PaneKind::Window ||
           p.kind == PaneKind::Part;
}

std::vector<int> layout_bands(const Layout& l, float units, bool all_panes, BandStats* c, const PartBoxes* parts,
                              int axis) {
    const size_t n = l.panes.size();
    std::vector<int> band(n, -1);
    if (std::abs(l.width - 1280) > 0.5f || std::abs(l.height - 720) > 0.5f)
        return band;
    const std::vector<LayoutRect> at = layout_place(l, parts);
    // Along the axis the picture grew (y: 1, x: 0) and across it.
    const bool y = axis != 0;
    const auto pos = [&](size_t k) { return y ? at[k].cy : at[k].cx; };
    const auto len = [&](size_t k) { return std::abs(y ? at[k].h : at[k].w); };
    const auto cross_pos = [&](size_t k) { return y ? at[k].cx : at[k].cy; };
    const auto cross_len = [&](size_t k) { return std::abs(y ? at[k].w : at[k].h); };
    const float half = y ? 360.0f : 640.0f, band_at = half / 2;  // bands: the outer halves of each side
    const float span = y ? 1270.0f : 710.0f;                     // across the whole screen
    const float wide = 2 * half - 80;                             // 640 high: a screen-sized background
    std::vector<bool> shown(n, true);
    for (size_t i = 0; i < n; ++i) {
        const int up = l.panes[i].parent;
        shown[i] = all_panes || (l.panes[i].visible() && (up < 0 || size_t(up) >= i || shown[size_t(up)]));
    }
    // Extent of each pane's drawn subtree along the axis (children come after their parent).
    std::vector<float> lo(n, 1e30f), hi(n, -1e30f);
    for (size_t k = n; k-- > 0;) {
        if (layout_drawn(l.panes[k]) && shown[k]) {
            const float h = len(k);
            lo[k] = std::min(lo[k], pos(k) - h / 2);
            hi[k] = std::max(hi[k], pos(k) + h / 2);
        }
        const int up = l.panes[k].parent;
        if (up >= 0 && size_t(up) < k)
            lo[size_t(up)] = std::min(lo[size_t(up)], lo[k]), hi[size_t(up)] = std::max(hi[size_t(up)], hi[k]);
    }
    const auto side = [&](size_t k) {
        if (lo[k] > hi[k])
            return -1;
        if (lo[k] >= band_at)
            return 0;
        if (hi[k] <= -band_at)
            return 1;
        return -1;
    };
    if (units > 0) {
        // Edge bands: a drawn pane at least H but less than 640 high that
        // spans the screen's width and ends exactly at its top or bottom edge
        // (a gradient behind a footer) moves to the new edge as a whole.
        std::vector<int> edge(n, -1);
        for (size_t j = 1; j < n; ++j) {
            const float h = len(j), w = cross_len(j);
            if (!layout_drawn(l.panes[j]) || h < units || h >= wide || w < span)
                continue;
            if (std::abs(pos(j) + h / 2 - half) <= 2)
                edge[j] = 0;
            else if (std::abs(pos(j) - h / 2 + half) <= 2)
                edge[j] = 1;
            else
                continue;
            band[j] = edge[j];
            ++c->groups[edge[j]];
        }
        const auto under_edge = [&](size_t i) {
            for (int k = l.panes[i].parent; k > 0 && size_t(k) < i; k = l.panes[size_t(k)].parent)
                if (edge[size_t(k)] >= 0)
                    return true;
            return false;
        };
        std::vector<bool> unit(n, false);
        // Units instead. A unit is a pane less than H high
        // (its drawn subtree) whose parents are all at least H high, so a box
        // keeps its texts and icons and a screen-sized panel is never one; it
        // moves when its middle lies in a band.
        for (size_t i = 1; i < n; ++i) {
            if (lo[i] > hi[i] || hi[i] - lo[i] >= units || edge[i] >= 0 || under_edge(i))
                continue;
            const int up = l.panes[i].parent;
            if (up > 0 && size_t(up) < i && !(lo[size_t(up)] > hi[size_t(up)]) && hi[size_t(up)] - lo[size_t(up)] < units)
                continue;  // inside a smaller unit
            const float mid = (lo[i] + hi[i]) / 2;
            int s = mid >= band_at ? 0 : mid <= -band_at ? 1 : -1;
            // Lying on a drawn pane at least H but less than 640 high (a window
            // behind texts, not its own parent or child; taller ones are
            // backgrounds): it stays with that pane, and moves with it if
            // that pane is an edge band.
            bool on_big = false;
            int with = -1;
            for (size_t j = 1; j < n && !on_big; ++j) {
                if (j == i || !layout_drawn(l.panes[j]) || len(j) < units || len(j) >= wide)
                    continue;
                bool related = false;
                for (int k = l.panes[i].parent; k >= 0 && !related; k = k < int(i) ? l.panes[size_t(k)].parent : -1)
                    related = size_t(k) == j;
                for (int k = l.panes[j].parent; k >= 0 && !related; k = k < int(j) ? l.panes[size_t(k)].parent : -1)
                    related = size_t(k) == i;
                on_big = !related && std::abs(cross_pos(i) - cross_pos(j)) <= cross_len(j) / 2 &&
                         std::abs(mid - pos(j)) <= len(j) / 2;
                if (on_big)
                    with = edge[j];
            }
            if (on_big)
                s = with;
            unit[i] = true;
            if (s < 0)
                continue;
            band[i] = s;
            ++c->groups[s];
        }
        // Numbered siblings (P_badge_00 to _07 in a grid, the labels around a
        // chart, the rows of a list) belong together: when some of them would
        // move and others stay, none moves, so the set keeps its shape.
        std::map<std::pair<int, std::string>, std::vector<size_t>> families;
        for (size_t i = 1; i < n; ++i) {
            if (!unit[i] || edge[i] >= 0)
                continue;
            const std::string& name = l.panes[i].name;
            size_t end = name.size();
            while (end > 0 && name[end - 1] >= '0' && name[end - 1] <= '9')
                --end;
            if (end == name.size() || end == 0)
                continue;
            families[{l.panes[i].parent, name.substr(0, end)}].push_back(i);
        }
        // Only alike siblings count: the same kind of pane, and their own
        // size and their drawn subtree's within 30 % of each other (inout_00
        // to _03 holding badges, texts and buttons are not one set).
        std::vector<float> left(n, 1e30f), right(n, -1e30f);
        for (size_t k = n; k-- > 0;) {
            if (layout_drawn(l.panes[k]) && shown[k]) {
                const float w = cross_len(k);
                left[k] = std::min(left[k], cross_pos(k) - w / 2);
                right[k] = std::max(right[k], cross_pos(k) + w / 2);
            }
            const int up = l.panes[k].parent;
            if (up >= 0 && size_t(up) < k)
                left[size_t(up)] = std::min(left[size_t(up)], left[k]),
                right[size_t(up)] = std::max(right[size_t(up)], right[k]);
        }
        const auto near = [](float a, float b) {
            a = std::abs(a), b = std::abs(b);
            return std::abs(a - b) <= 0.3f * std::max(a, b) + 0.5f;
        };
        const auto alike = [&](size_t a, size_t b) {
            return l.panes[a].kind == l.panes[b].kind && near(at[a].w, at[b].w) && near(at[a].h, at[b].h) &&
                   near(right[a] - left[a], right[b] - left[b]) && near(hi[a] - lo[a], hi[b] - lo[b]);
        };
        for (const auto& [key, members] : families) {
            bool moves = false, stays = false, same = true;
            for (const size_t i : members) {
                (band[i] >= 0 ? moves : stays) = true;
                same = same && alike(i, members[0]);
            }
            if (!same)
                continue;
            if (!moves || !stays)
                continue;
            for (const size_t i : members)
                if (band[i] >= 0)
                    --c->groups[band[i]], band[i] = -1;
        }
    } else {
        for (size_t i = 1; i < n; ++i) {
            const int s = side(i);
            if (s < 0)
                continue;
            const int up = l.panes[i].parent;
            if (up > 0 && size_t(up) < i && side(size_t(up)) == s)
                continue;  // its parent moves it already
            band[i] = s;
            ++c->groups[s];
        }
    }
    // Drawn panes under a band group (or the group itself), and texts.
    for (size_t i = 1; i < n; ++i) {
        if (!layout_drawn(l.panes[i]) || !shown[i])
            continue;
        int k = int(i);
        while (k > 0 && band[size_t(k)] < 0 && l.panes[size_t(k)].parent < k)
            k = l.panes[size_t(k)].parent;
        const bool is_text = l.panes[i].kind == PaneKind::Text;
        const int s = side(i);
        if (s >= 0 && is_text)
            ++c->texts_band[s];
        if (k > 0 && band[size_t(k)] >= 0) {
            ++c->covered[band[size_t(k)]];
            if (is_text)
                ++c->texts[band[size_t(k)]];
        }
    }
    return band;
}

// The file name without folder and extension.
std::string layout_stem(const std::string& name) {
    const size_t slash = name.find_last_of('/');
    std::string base = slash == std::string::npos ? name : name.substr(slash + 1);
    const size_t dot = base.find_last_of('.');
    return dot == std::string::npos ? base : base.substr(0, dot);
}

// The layout an animation `<layout>_<name>` belongs to: the longest layout
// stem of the archive it starts with, so `a_sub_in` goes to `a_sub`, not `a`.
template <typename Plan>
std::string animation_owner(const Plan& plan, const std::string& anim) {
    std::string best;
    for (const auto& p : plan) {
        const std::string stem = layout_stem(p.entry->name);
        if (anim.size() > stem.size() && anim.compare(0, stem.size(), stem) == 0 && anim[stem.size()] == '_' &&
            stem.size() > best.size())
            best = stem;
    }
    return best;
}

// The local shift (x, y) of each band group (0 for the rest) of a 1280x720
// layout: the screen shift through the inverse of the parents' scale and
// rotation (z turns; x and y turns fold that axis by their cosine). A parent
// scaled to 0 (it grows in by animation) counts as 1.
std::vector<std::array<float, 2>> layout_shifts(const Layout& l, const ShiftOptions& o, ShiftStats* sc,
                                                const PartBoxes* parts) {
    std::vector<int> band;
    if (std::abs(l.width - 1280) <= 0.5f && std::abs(l.height - 720) <= 0.5f) {
        BandStats unused;
        band = layout_bands(l, o.units, true, &unused, parts, o.axis);
    } else {
        band.assign(l.panes.size(), -1);
    }
    // o.move: these panes move as one group whatever their size, to the side
    // their drawn subtree's middle lies on (two status plates of a double
    // battle in one corner node); the groups under them move with them.
    if (!o.move.empty()) {
        const std::vector<LayoutRect> at = layout_place(l, parts);
        const size_t n = l.panes.size();
        std::vector<float> lo(n, 1e30f), hi(n, -1e30f);
        for (size_t k = n; k-- > 0;) {
            if (layout_drawn(l.panes[k])) {
                const float c = o.axis != 0 ? at[k].cy : at[k].cx, h = std::abs(o.axis != 0 ? at[k].h : at[k].w);
                lo[k] = std::min(lo[k], c - h / 2);
                hi[k] = std::max(hi[k], c + h / 2);
            }
            const int up = l.panes[k].parent;
            if (up >= 0 && size_t(up) < k)
                lo[size_t(up)] = std::min(lo[size_t(up)], lo[k]), hi[size_t(up)] = std::max(hi[size_t(up)], hi[k]);
        }
        const auto under = [&](size_t j, size_t i) {
            for (int k = l.panes[j].parent; k >= 0 && size_t(k) < j; k = l.panes[size_t(k)].parent)
                if (size_t(k) == i)
                    return true;
            return false;
        };
        for (size_t i = 1; i < n; ++i) {
            if (!listed(o.move, l.panes[i].name) || lo[i] > hi[i])
                continue;
            const float mid = (lo[i] + hi[i]) / 2;
            if (std::abs(mid) < 1)
                continue;  // no side
            bool carried = false;  // an ancestor moves already
            for (int k = l.panes[i].parent; k > 0 && size_t(k) < i && !carried; k = l.panes[size_t(k)].parent)
                carried = band[size_t(k)] >= 0;
            if (carried)
                continue;
            band[i] = mid > 0 ? 0 : 1;
            for (size_t j = i + 1; j < n; ++j)
                if (under(j, i))
                    band[j] = -1;
        }
    }
    // o.into: a group the game places itself at run time passes the shift on to its children.
    for (size_t i = 1; i < l.panes.size(); ++i) {
        if (band[i] < 0 || !listed(o.into, l.panes[i].name))
            continue;
        for (size_t j = i + 1; j < l.panes.size(); ++j)
            if (l.panes[j].parent == int(i))
                band[j] = band[i];
        band[i] = -1;
    }
    // o.keep: these panes and all under them stay. A kept pane inside a group
    // that moves gets the opposite shift (band 2 + the group's band), so it
    // stays on screen while the rest of the group moves.
    const std::vector<int> moving = band;
    const auto parent_of = [&](int k) { return l.panes[size_t(k)].parent < k ? l.panes[size_t(k)].parent : -1; };
    for (size_t i = 1; i < l.panes.size(); ++i) {
        if (!listed(o.keep, l.panes[i].name))
            continue;
        bool under_kept = false;  // an ancestor is kept itself: it carries the counter shift
        int group = -1;
        for (int k = parent_of(int(i)); k > 0; k = parent_of(k)) {
            under_kept = under_kept || listed(o.keep, l.panes[size_t(k)].name);
            if (group < 0 && moving[size_t(k)] >= 0)
                group = moving[size_t(k)];
        }
        band[i] = !under_kept && group >= 0 ? 2 + group : -1;
        for (size_t j = i + 1; j < l.panes.size(); ++j)  // all under it stay with it
            for (int k = parent_of(int(j)); k > 0; k = parent_of(k))
                if (size_t(k) == i) {
                    band[j] = -1;
                    break;
                }
    }
    std::vector<std::array<float, 2>> out(l.panes.size(), {0.0f, 0.0f});
    constexpr float kDeg = 3.14159265358979f / 180.0f;
    for (size_t i = 1; i < l.panes.size(); ++i) {
        if (band[i] < 0)
            continue;
        std::vector<int> chain;  // parents, nearest first
        for (int k = l.panes[i].parent; k >= 0 && size_t(k) < i; k = l.panes[size_t(k)].parent)
            chain.push_back(k);
        // m: local of the band group's parent -> screen, built from the root down.
        float m[4] = {1, 0, 0, 1};
        bool turned = false, flat = false;
        for (size_t c = chain.size(); c-- > 0;) {
            const LayoutPane& p = l.panes[size_t(chain[c])];
            float sx = p.scale[0] * std::cos(p.rotate[1] * kDeg), sy = p.scale[1] * std::cos(p.rotate[0] * kDeg);
            if (std::abs(sx) < 1e-6f)
                sx = 1, flat = true;
            if (std::abs(sy) < 1e-6f)
                sy = 1, flat = true;
            const float a = p.rotate[2] * kDeg, ca = std::cos(a), sa = std::sin(a);
            turned |= p.rotate[0] != 0 || p.rotate[1] != 0 || p.rotate[2] != 0;
            const float r[4] = {ca * sx, -sa * sy, sa * sx, ca * sy};  // R * S
            const float n[4] = {m[0] * r[0] + m[1] * r[2], m[0] * r[1] + m[1] * r[3], m[2] * r[0] + m[3] * r[2],
                                m[2] * r[1] + m[3] * r[3]};
            std::copy(n, n + 4, m);
        }
        const float det = m[0] * m[3] - m[1] * m[2];
        if (std::abs(det) < 1e-6f) {
            ++sc->skipped;
            continue;
        }
        const int side = band[i] & 1;  // 2, 3: the counter of 0, 1
        const float by = o.band_shift[size_t(side)] >= 0 ? o.band_shift[size_t(side)] : o.shift;
        if (by == 0)
            continue;
        const float d = band[i] == 0 || band[i] == 3 ? by : -by;
        // The screen step (0, d) or (d, 0) through the inverse of m.
        out[i] = o.axis != 0 ? std::array<float, 2>{-m[1] * d / det, m[0] * d / det}
                             : std::array<float, 2>{m[3] * d / det, -m[2] * d / det};
        ++sc->groups;
        sc->turned += turned;
        sc->flat += flat;
    }
    return out;
}

namespace {

using PaneShifts = std::map<std::string, std::array<float, 2>>;  // pane name -> local shift

// Adds shift[i] to the x and y translation of pane i in the BFLYT bytes [begin, end),
// and, in part panes, to the overrides of the part layout's panes that move
// (parts: layout name -> its moved panes): an override replaces the part's own
// placement, so it has to move the same way. Returns the overrides changed.
size_t shift_flyt(std::vector<uint8_t>& v, size_t begin, size_t end, const std::vector<std::array<float, 2>>& shift,
                  const std::map<std::string, PaneShifts>& parts) {
    size_t at = begin + rd16(v, begin + 6);
    const uint16_t sections = rd16(v, begin + 0x10);
    size_t pane = 0, overrides = 0;
    const auto add2 = [&](size_t where, const std::array<float, 2>& d) {
        for (int c = 0; c < 2; ++c)
            if (d[size_t(c)] != 0)
                addf(v, where + size_t(c) * 4, d[size_t(c)]);
    };
    const auto str = [&](size_t where, size_t max) {
        std::string out;
        for (size_t k = 0; k < max && where + k < end && v[where + k] != 0; ++k)
            out.push_back(char(v[where + k]));
        return out;
    };
    for (uint16_t i = 0; i < sections && has(v, begin, end, at, 8); ++i) {
        const std::string magic(reinterpret_cast<const char*>(&v[at]), 4);
        const uint32_t size = rd32(v, at + 4);
        if (size < 8 || !has(v, begin, end, at, size))
            break;
        if (magic == "pan1" || magic == "pic1" || magic == "txt1" || magic == "wnd1" || magic == "bnd1" ||
            magic == "prt1") {
            if (pane < shift.size() && has(v, begin, end, at + 8 + 0x24, 8))
                add2(at + 8 + 0x24, shift[pane]);
            ++pane;
        }
        if (magic == "prt1" && has(v, begin, end, at + 8 + 0x4C, 12)) {
            const size_t ext = at + 8 + 0x4C;
            const uint32_t props = rd32(v, ext);
            const auto part = parts.find(str(ext + 12 + size_t(props) * 0x28, 64));
            for (uint32_t k = 0; part != parts.end() && k < props && k < 64; ++k) {
                const size_t e = ext + 12 + size_t(k) * 0x28;
                if (!has(v, begin, end, e, 0x28))
                    break;
                const auto moved = part->second.find(str(e, 0x18));
                if (moved == part->second.end())
                    continue;
                const uint32_t pane_at = rd32(v, e + 0x1C), basic_at = rd32(v, e + 0x24);
                bool any = false;
                if (pane_at != 0 && v[e + 0x18] != 0 && has(v, begin, end, at + pane_at + 8 + 0x24, 8))
                    add2(at + pane_at + 8 + 0x24, moved->second), any = true;
                if (basic_at != 0 && (v[e + 0x19] & 8) && has(v, begin, end, at + basic_at + 8, 8))
                    add2(at + basic_at + 8, moved->second), any = true;
                overrides += any;
            }
        }
        at += size;
    }
    return overrides;
}

// Shifts the x and y translation keys of the curves for the band groups in the BFLAN bytes [begin, end).
void shift_flan(std::vector<uint8_t>& v, size_t begin, size_t end, const Layout& l,
                const std::vector<std::array<float, 2>>& shift,
                ShiftStats* sc) {
    size_t at = begin + rd16(v, begin + 6);
    const uint16_t sections = rd16(v, begin + 0x10);
    for (uint16_t i = 0; i < sections && has(v, begin, end, at, 8); ++i) {
        const uint32_t size = rd32(v, at + 4);
        if (size < 8 || !has(v, begin, end, at, size))
            break;
        if (std::memcmp(&v[at], "pai1", 4) == 0 && has(v, begin, end, at, 0x14)) {
            const uint16_t groups = rd16(v, at + 14);
            const size_t table = at + rd32(v, at + 16);
            for (uint16_t g = 0; g < groups && has(v, begin, end, table + g * 4u, 4); ++g) {
                const size_t e = at + rd32(v, table + g * 4u);
                if (!has(v, begin, end, e, 0x24) || v[e + 0x1D] == 1)
                    continue;  // material curves
                size_t len = 0;
                while (len < 0x1C && v[e + len] != 0)
                    ++len;
                const int p = l.find(std::string(reinterpret_cast<const char*>(&v[e]), len));
                if (p <= 0 || (shift[size_t(p)][0] == 0 && shift[size_t(p)][1] == 0))
                    continue;
                const uint8_t tags = v[e + 0x1C];
                for (uint8_t t = 0; t < tags && has(v, begin, end, e + 0x20 + t * 4u, 4); ++t) {
                    const size_t tag = e + rd32(v, e + 0x20 + t * 4u);
                    if (!has(v, begin, end, tag, 8) || std::memcmp(&v[tag], "FLPA", 4) != 0)
                        continue;
                    const uint8_t entries = v[tag + 4];
                    for (uint8_t k = 0; k < entries && has(v, begin, end, tag + 8 + k * 4u, 4); ++k) {
                        const size_t ent = tag + rd32(v, tag + 8 + k * 4u);
                        if (!has(v, begin, end, ent, 12) || v[ent + 1] > 1 || v[ent + 2] != 2 ||
                            shift[size_t(p)][v[ent + 1]] == 0)
                            continue;  // only hermite curves of translate x and y
                        const uint16_t keys = rd16(v, ent + 4);
                        const size_t key = ent + rd32(v, ent + 8);
                        ++sc->curves;
                        for (uint16_t n = 0; n < keys && has(v, begin, end, key + n * 12u, 12); ++n, ++sc->keys)
                            addf(v, key + n * 12u + 4, shift[size_t(p)][v[ent + 1]]);
                    }
                }
            }
        }
        at += size;
    }
}

}  // namespace

// Shifts the band groups of every screen layout in one SARC; true if anything changed.
namespace {

// The options for one layout: LAYOUT:PANE entries of `keep` for it become PANE,
// those for other layouts go.
ShiftOptions for_layout(const ShiftOptions& o, const std::string& stem) {
    ShiftOptions out = o;
    const auto pick = [&](const std::vector<std::string>& in, std::vector<std::string>* to) {
        to->clear();
        for (const std::string& k : in) {
            const size_t colon = k.find(':');
            if (colon == std::string::npos)
                to->push_back(k);
            else if (k.compare(0, colon, stem) == 0 && colon == stem.size())
                to->push_back(k.substr(colon + 1));
        }
    };
    pick(o.keep, &out.keep);
    pick(o.move, &out.move);
    if (std::find(o.hud.begin(), o.hud.end(), stem) != o.hud.end())
        out.band_shift = o.hud_shift;
    return out;
}

}  // namespace

bool shift_sarc(std::vector<uint8_t>& bytes, const ShiftOptions& o, ShiftStats* sc, const ShiftListener& listener) {
    std::string error;
    auto sarc = Sarc::parse(bytes, &error);
    if (!sarc || std::memcmp(bytes.data(), "SARC", 4) != 0)
        return false;  // Yaz0 would change size; none in LGP
    // First the shifts of every screen layout, so part panes can follow them.
    struct Planned {
        const Sarc::Entry* entry;
        Layout layout;
        std::vector<std::array<float, 2>> shift;  // empty: not a screen layout
    };
    std::vector<Planned> plan;
    std::map<std::string, PaneShifts> parts;
    for (const auto& e : sarc->entries()) {
        const auto f = sarc->file(e);
        if (f.size() < 4 || std::memcmp(f.data(), "FLYT", 4) != 0)
            continue;
        std::string why;
        auto l = parse_bflyt(f, &why);
        if (!l)
            continue;
        plan.push_back({&e, std::move(*l), {}});
    }
    // Layouts shown as parts here, with their content boxes.
    std::set<std::string> shown_as_part;
    for (const Planned& p : plan)
        for (const LayoutPane& pane : p.layout.panes)
            if (pane.kind == PaneKind::Part && !pane.part.empty())
                shown_as_part.insert(pane.part);
    // A part layout that would move nothing on its canvas (at most panes
    // wholly outside it, such as dummy targets) is no screen layout: it stays
    // as it is and its part panes count by its content box, so they can be
    // units (the battle's ball row). One that moves something on its canvas
    // (a footer inside a full-screen part) keeps its own units and its part
    // panes their own size, as before, so nothing moves twice.
    const auto screen = [&](const Planned& p) {
        return std::abs(p.layout.width - 1280) <= 0.5f && std::abs(p.layout.height - 720) <= 0.5f &&
               !listed(o.skip, layout_stem(p.entry->name));
    };
    PartBoxes boxes;
    std::set<std::string> still;  // part layouts left as they are
    for (const Planned& p : plan) {
        const std::string stem = layout_stem(p.entry->name);
        if (shown_as_part.count(stem) == 0)
            continue;
        bool moves = false;
        if (screen(p)) {
            ShiftStats unused;
            const auto shift = layout_shifts(p.layout, for_layout(o, stem), &unused);
            const std::vector<LayoutRect> at = layout_place(p.layout);
            const size_t n = p.layout.panes.size();
            for (size_t j = 1; j < n && !moves; ++j) {
                const LayoutRect& r = at[j];
                if (!layout_drawn(p.layout.panes[j]) || std::abs(r.cx) - std::abs(r.w) / 2 >= 640 ||
                    std::abs(r.cy) - std::abs(r.h) / 2 >= 360)
                    continue;
                for (int k = int(j); k > 0 && !moves; k = p.layout.panes[size_t(k)].parent < k
                                                               ? p.layout.panes[size_t(k)].parent
                                                               : -1)
                    moves = shift[size_t(k)][0] != 0 || shift[size_t(k)][1] != 0;
            }
        }
        if (moves)
            continue;
        still.insert(stem);
        boxes[stem] = layout_content(p.layout);
    }
    for (Planned& p : plan) {
        const Layout& lay = p.layout;
        const std::string stem = layout_stem(p.entry->name);
        const bool small_part = still.count(stem) != 0;
        if (screen(p) && !small_part) {
            ShiftStats here;
            p.shift = layout_shifts(lay, for_layout(o, stem), &here, &boxes);
            sc->turned += here.turned, sc->flat += here.flat, sc->skipped += here.skipped;
            sc->groups += here.groups;
            if (listener)
                listener(p.entry->name, lay, p.shift);
            PaneShifts& moved = parts[stem];
            for (size_t i = 0; i < p.shift.size(); ++i)
                if (p.shift[i][0] != 0 || p.shift[i][1] != 0)
                    moved[lay.panes[i].name] = p.shift[i];
            if (moved.empty())
                parts.erase(stem);
        }
    }
    bool changed = false;
    for (const Planned& p : plan) {
        const bool own = !p.shift.empty() && parts.count(layout_stem(p.entry->name)) != 0;
        const size_t overrides = shift_flyt(bytes, p.entry->begin, p.entry->end, p.shift, parts);
        sc->overrides += overrides;
        if (!own && overrides == 0)
            continue;
        changed = true;
        if (own)
            ++sc->layouts;
        if (!own)
            continue;
        const std::string layout = layout_stem(p.entry->name);
        for (const auto& a : sarc->entries()) {
            const auto af = sarc->file(a);
            if (af.size() < 4 || std::memcmp(af.data(), "FLAN", 4) != 0 ||
                animation_owner(plan, layout_stem(a.name)) != layout)
                continue;
            shift_flan(bytes, a.begin, a.end, p.layout, p.shift, sc);
        }
    }
    if (changed)
        ++sc->archives;
    return changed;
}

// LZ4 block format (public): decode; nullopt if it does not come out at `size` bytes.
std::optional<std::vector<uint8_t>> lz4_block_decode(std::span<const uint8_t> in, size_t size) {
    std::vector<uint8_t> out;
    out.reserve(size);
    size_t i = 0;
    while (i < in.size()) {
        const uint8_t token = in[i++];
        size_t lit = token >> 4;
        if (lit == 15)
            for (uint8_t b = 255; b == 255 && i < in.size();)
                lit += (b = in[i++]);
        if (i + lit > in.size())
            return std::nullopt;
        out.insert(out.end(), in.begin() + std::ptrdiff_t(i), in.begin() + std::ptrdiff_t(i + lit));
        i += lit;
        if (i >= in.size())
            break;  // the last sequence has literals only
        if (i + 2 > in.size())
            return std::nullopt;
        const size_t offset = size_t(in[i]) | size_t(in[i + 1]) << 8;
        i += 2;
        size_t match = token & 15;
        if (match == 15)
            for (uint8_t b = 255; b == 255 && i < in.size();)
                match += (b = in[i++]);
        match += 4;
        if (offset == 0 || offset > out.size())
            return std::nullopt;
        for (size_t k = 0; k < match; ++k)
            out.push_back(out[out.size() - offset]);
    }
    if (out.size() != size)
        return std::nullopt;
    return out;
}

// LZ4 block compression (greedy, hash of 4 bytes, 64 KiB window); the
// format's end rules: the last 5 bytes are literals, no match starts in the
// last 12 bytes.
std::vector<uint8_t> lz4_block_compress(std::span<const uint8_t> in) {
    std::vector<uint8_t> out;
    out.reserve(in.size() / 2 + 16);
    const size_t n = in.size();
    std::vector<int64_t> table(1 << 16, -1);
    const auto hash = [&](size_t p) {
        uint32_t v;
        std::memcpy(&v, in.data() + p, 4);
        return (v * 2654435761u) >> 16;
    };
    const auto length = [&](size_t v) {
        for (; v >= 255; v -= 255)
            out.push_back(255);
        out.push_back(uint8_t(v));
    };
    size_t anchor = 0, p = 0;
    const size_t match_limit = n >= 12 ? n - 12 : 0;
    while (p < match_limit) {
        const uint32_t h = hash(p);
        const int64_t cand = table[h];
        table[h] = int64_t(p);
        if (cand < 0 || p - size_t(cand) > 65535 || std::memcmp(in.data() + cand, in.data() + p, 4) != 0) {
            ++p;
            continue;
        }
        size_t len = 4;
        while (p + len < n - 5 && in[size_t(cand) + len] == in[p + len])
            ++len;
        const size_t lit = p - anchor;
        out.push_back(uint8_t((std::min<size_t>(lit, 15) << 4) | std::min<size_t>(len - 4, 15)));
        if (lit >= 15)
            length(lit - 15);
        out.insert(out.end(), in.begin() + std::ptrdiff_t(anchor), in.begin() + std::ptrdiff_t(p));
        const size_t off = p - size_t(cand);
        out.push_back(uint8_t(off)), out.push_back(uint8_t(off >> 8));
        if (len - 4 >= 15)
            length(len - 4 - 15);
        p += len;
        anchor = p;
    }
    const size_t lit = n - anchor;
    out.push_back(uint8_t(std::min<size_t>(lit, 15) << 4));
    if (lit >= 15)
        length(lit - 15);
    out.insert(out.end(), in.begin() + std::ptrdiff_t(anchor), in.end());
    return out;
}

// A GFLXPACK archive (.gfpak; header: magic, u32 version, u32 relocated, u32
// file count, u32 folder count, u64 file table; entry 0x18: u16 level, u16
// compression (0 none, 2 LZ4 block), u32 size, u32 packed size, u32 pad, u64
// offset): shifts the SARC files in it. LZ4 files are compressed again; a
// file that fits its old place stays there, else it goes to the end of the
// archive (16-byte aligned) and its entry points there. The archive grows as
// little as possible: the game may load it into a buffer of about its size.
// True if anything changed.
bool shift_gfpak(std::vector<uint8_t>& bytes, const ShiftOptions& o, ShiftStats* sc, const ShiftListener& listener) {
    if (bytes.size() < 0x20 || std::memcmp(bytes.data(), "GFLXPACK", 8) != 0)
        return false;
    const size_t archives_before = sc->archives;
    const uint32_t count = rd32(bytes, 0x10);
    const uint64_t table = uint64_t(rd32(bytes, 0x18)) | uint64_t(rd32(bytes, 0x1C)) << 32;
    if (table + uint64_t(count) * 0x18 > bytes.size())
        return false;
    bool changed = false;
    for (uint32_t i = 0; i < count; ++i) {
        const size_t e = size_t(table) + size_t(i) * 0x18;
        const uint16_t comp = rd16(bytes, e + 2);
        const uint32_t size = rd32(bytes, e + 4), packed = rd32(bytes, e + 8);
        const uint64_t off = uint64_t(rd32(bytes, e + 0x10)) | uint64_t(rd32(bytes, e + 0x14)) << 32;
        if ((comp != 0 && comp != 2) || off + packed > bytes.size())
            continue;
        const std::span<const uint8_t> raw(bytes.data() + off, packed);
        std::optional<std::vector<uint8_t>> file;
        if (comp == 2)
            file = lz4_block_decode(raw, size);
        else if (size <= packed)
            file = std::vector<uint8_t>(raw.begin(), raw.begin() + size);
        if (!file || file->size() < 4 || std::memcmp(file->data(), "SARC", 4) != 0)
            continue;
        if (!shift_sarc(*file, o, sc, listener))
            continue;
        const std::vector<uint8_t> stored = comp == 2 ? lz4_block_compress(*file) : *file;
        uint64_t at = off;
        if (stored.size() <= packed) {
            std::copy(stored.begin(), stored.end(), bytes.begin() + std::ptrdiff_t(off));
            ++sc->in_place;
        } else {
            while (bytes.size() % 16 != 0)
                bytes.push_back(0);
            at = bytes.size();
            bytes.insert(bytes.end(), stored.begin(), stored.end());
            ++sc->appended;
        }
        const auto put = [&](size_t where32, uint32_t x) {
            for (int k = 0; k < 4; ++k)
                bytes[where32 + size_t(k)] = uint8_t(x >> (8 * k));
        };
        put(e + 8, uint32_t(stored.size()));
        put(e + 0x10, uint32_t(at));
        put(e + 0x14, uint32_t(at >> 32));
        changed = true;
    }
    sc->archives = archives_before;  // the SARC files inside are not written on their own
    sc->gfpaks += changed;
    return changed;
}

}  // namespace lgp::platform::ui
