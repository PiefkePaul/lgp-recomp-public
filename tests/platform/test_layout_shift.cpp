// Moving header and footer units of layouts (lgp/platform/ui/layout_shift.hpp)
// on layouts the test builds itself.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "lgp/platform/ui/layout.hpp"
#include "lgp/platform/ui/layout_shift.hpp"
#include "lgp/platform/ui/sarc.hpp"
#include "ui_fixtures.hpp"

using namespace lgp::platform::ui;

namespace {

uint32_t get32(const std::vector<uint8_t>& v, size_t at) {
    return uint32_t(v[at]) | uint32_t(v[at + 1]) << 8 | uint32_t(v[at + 2]) << 16 | uint32_t(v[at + 3]) << 24;
}
uint16_t get16(const std::vector<uint8_t>& v, size_t at) { return uint16_t(v[at] | v[at + 1] << 8); }

}  // namespace

int main() {
    int failures = 0;
    const auto check = [&](bool ok, const char* what) {
        if (!ok) {
            std::printf("FAIL: %s\n", what);
            ++failures;
        }
    };
    // A footer group (bar and text) at the bottom, a header picture at the top
    // and a label at the side.
    lgp::test::LayoutBuilder b(1280, 720, false);
    b.materials({{"M", -1}});
    b.null("RootPane", 0, 0, 1280, 720);
    b.begin_children();
    b.null("N_foot", 0, 40, 1280, 80, uint8_t(2 << 6));
    b.begin_children();
    b.picture("P_bar", 0, 0, 0, 1280, 80);
    b.text("T_hint", 0, 560, 0, 160, 40, 24, 0);
    b.end_children();
    b.picture("P_head", 0, 0, 320, 1280, 80);
    b.picture("P_side", 0, -100, 0, 160, 100, 0, 0xFFFFFFFF);
    b.end_children();
    std::vector<uint8_t> file = b.finish();
    std::string error;
    {
        auto l = parse_bflyt(file, &error);
        check(l.has_value(), "layout parses");
        if (l) {
            BandStats bc;
            const std::vector<int> band = layout_bands(*l, 0, false, &bc);
            check(bc.groups[1] == 1 && band[size_t(l->find("N_foot"))] == 1, "footer group is the bottom band group");
            check(bc.covered[1] == 2 && bc.texts[1] == 1 && bc.texts_band[1] == 1, "it covers bar and text");
            check(bc.groups[0] == 1 && band[size_t(l->find("P_head"))] == 0, "header picture is a top band group");
            check(band[size_t(l->find("P_bar"))] < 0, "the bar moves with its group, not on its own");
        }
    }
    // Footer group down, header up, by 120 (its parent unscaled); the
    // curve that slides the footer in shifts too, the bar inside does not.
    {
        const auto anim = lgp::test::make_bflan(10, {{"N_foot", "FLPA", 1, -300}, {"P_bar", "FLPA", 1, 5}});
        std::vector<uint8_t> arc = lgp::test::make_sarc({{"blyt/a.bflyt", std::string(file.begin(), file.end())},
                                                        {"anim/a_in.bflan", std::string(anim.begin(), anim.end())}});
        const size_t size = arc.size();
        ShiftOptions o;
        o.shift = 120;
        ShiftStats sc;
        check(shift_sarc(arc, o, &sc), "the archive changes");
        check(arc.size() == size, "same size");
        check(sc.layouts == 1 && sc.groups == 2 && sc.curves == 1 && sc.keys == 2, "two groups, one curve");
        const auto shifted = Sarc::parse(arc, &error);
        const auto sl = shifted ? parse_bflyt(shifted->file("blyt/a.bflyt"), &error) : std::nullopt;
        const auto sa = shifted ? parse_bflan(shifted->file("anim/a_in.bflan"), &error) : std::nullopt;
        check(sl && sa, "shifted files parse");
        if (sl && sa) {
            check(std::abs(sl->panes[size_t(sl->find("N_foot"))].translate[1] - (40 - 120)) < 1e-3f, "footer down");
            check(std::abs(sl->panes[size_t(sl->find("P_head"))].translate[1] - (320 + 120)) < 1e-3f, "header up");
            check(sl->panes[size_t(sl->find("P_bar"))].translate[1] == 0, "bar unchanged");
            check(std::abs(sa->groups[0].curves[0].keys[1].value - (-420)) < 1e-3f, "footer curve down");
            check(sa->groups[1].curves[0].keys[1].value == 5, "bar curve unchanged");
        }
        // `a_sub_in` belongs to `a_sub`, not to `a` too: its curve shifts once.
        {
            const auto sub_anim = lgp::test::make_bflan(10, {{"N_foot", "FLPA", 1, -300}});
            std::vector<uint8_t> two = lgp::test::make_sarc(
                {{"blyt/a.bflyt", std::string(file.begin(), file.end())},
                 {"blyt/a_sub.bflyt", std::string(file.begin(), file.end())},
                 {"anim/a_sub_in.bflan", std::string(sub_anim.begin(), sub_anim.end())}});
            ShiftStats s2;
            check(shift_sarc(two, o, &s2), "the archive with a_sub changes");
            const auto sh = Sarc::parse(two, &error);
            const auto a2 = sh ? parse_bflan(sh->file("anim/a_sub_in.bflan"), &error) : std::nullopt;
            check(a2.has_value() && std::abs(a2->groups[0].curves[0].keys[1].value - (-420)) < 1e-3f,
                  "a_sub's curve shifts once, not also for a");
        }
        // Under a parent turned by 180 degrees and scaled by 2, the local shift is
        // reversed and halved; under one turned by 90, it goes along local x.
        lgp::test::LayoutBuilder t(1280, 720, false);
        t.materials({{"M", -1}});
        t.null("RootPane", 0, 0, 1280, 720);
        t.begin_children();
        t.null("N_turned", 0, 0, 1280, 720);
        t.begin_children();
        t.picture("P_low", 0, 0, 150, 200, 40);  // turned: on screen at y -300
        t.picture("P_mid", 0, 0, 0, 200, 40);    // keeps the parent out of the band
        t.end_children();
        t.null("N_quarter", 0, 0, 1280, 720);
        t.begin_children();
        t.picture("P_q", 0, -300, 0, 40, 40);  // turned by 90: on screen at y -300
        t.picture("P_qmid", 0, 0, 0, 40, 40);
        t.end_children();
        t.end_children();
        auto tl = parse_bflyt(t.finish(), &error);
        check(tl.has_value(), "turned layout parses");
        if (tl) {
            LayoutPane& turned = tl->panes[size_t(tl->find("N_turned"))];
            turned.rotate[2] = 180, turned.scale[0] = turned.scale[1] = 2;
            tl->panes[size_t(tl->find("N_quarter"))].rotate[2] = 90;
            ShiftStats tc;
            const auto sh = layout_shifts(*tl, o, &tc);
            const auto& low = sh[size_t(tl->find("P_low"))];
            const auto& q = sh[size_t(tl->find("P_q"))];
            check(tc.groups == 2 && tc.turned == 2, "two groups under turned parents");
            check(std::abs(low[0]) < 1e-3f && std::abs(low[1] - 60) < 1e-3f, "180 degrees, scale 2: +60 locally");
            check(std::abs(q[0] - (-120)) < 1e-3f && std::abs(q[1]) < 1e-3f, "90 degrees: -120 along local x");
        }
        // Units of 360: an item of icon (middle) and label (bottom band) stays
        // together and in place; a box in the top band takes its sentence along,
        // although the sentence alone is not in the band.
        lgp::test::LayoutBuilder u(1280, 720, false);
        u.materials({{"M", -1}});
        u.null("RootPane", 0, 0, 1280, 720);
        u.begin_children();
        u.null("N_item", 0, 0, 100, 100);
        u.begin_children();
        u.picture("P_icon", 0, 0, -110, 80, 80);
        u.text("T_label", 0, 0, -205, 100, 30, 20, 0);
        u.end_children();
        u.null("N_box", 0, 0, 820, 180);
        u.begin_children();
        u.picture("P_box", 0, 0, 273, 820, 180);
        u.text("T_ask", 0, 0, 178, 600, 30, 20, 0);
        u.end_children();
        u.picture("P_mid", 0, 0, 0, 400, 300);
        u.end_children();
        auto ul = parse_bflyt(u.finish(), &error);
        check(ul.has_value(), "units layout parses");
        if (ul) {
            BandStats plain;
            const auto b0 = layout_bands(*ul, 0, true, &plain);
            check(b0[size_t(ul->find("T_label"))] == 1 && b0[size_t(ul->find("P_box"))] == 0, "band groups split them");
            BandStats uc;
            const auto b1 = layout_bands(*ul, 360, true, &uc);
            check(b1[size_t(ul->find("N_item"))] < 0 && b1[size_t(ul->find("T_label"))] < 0, "the item stays whole");
            check(b1[size_t(ul->find("N_box"))] == 0 && uc.groups[0] == 1 && uc.groups[1] == 0, "the box moves whole");
            ShiftOptions io = o;
            io.units = 360;
            io.into = {"N_box"};
            ShiftStats ic;
            const auto into = layout_shifts(*ul, io, &ic);
            check(into[size_t(ul->find("N_box"))][1] == 0 && into[size_t(ul->find("P_box"))][1] == 120 &&
                      into[size_t(ul->find("T_ask"))][1] == 120 && ic.groups == 2,
                  "into moves the box's children instead");
        }
        // A part pane whose override places the part's footer: the override moves
        // with the footer of the part layout, other overrides stay.
        {
            lgp::test::LayoutBuilder host(400, 300, false);
            host.null("RootPane", 0, 0, 400, 300);
            host.begin_children();
            host.part("L_bar", 0, 0, 1280, 720, "a", {{"N_foot", -330}, {"P_side", 0}});
            host.end_children();
            const auto h = host.finish();
            std::vector<uint8_t> two = lgp::test::make_sarc({{"blyt/a.bflyt", std::string(file.begin(), file.end())},
                                                            {"blyt/host.bflyt", std::string(h.begin(), h.end())}});
            ShiftStats pc;
            check(shift_sarc(two, o, &pc) && pc.overrides == 1, "one part override moves");
            const auto sh = Sarc::parse(two, &error);
            const auto hl = sh ? parse_bflyt(sh->file("blyt/host.bflyt"), &error) : std::nullopt;
            check(hl && hl->panes.size() == 2 && hl->panes[1].overrides.size() == 2, "host parses with overrides");
            if (hl && hl->panes.size() == 2 && hl->panes[1].overrides.size() == 2) {
                check(std::abs(hl->panes[1].overrides[0].translate[1] - (-450)) < 1e-3f, "footer override down");
                check(hl->panes[1].overrides[1].translate[1] == 0, "side override unchanged");
            }
            // A screen-sized panel is no unit and holds nothing: its header
            // moves, the panel stays.
            lgp::test::LayoutBuilder pb(1280, 720, false);
            pb.materials({{"M", -1}});
            pb.null("RootPane", 0, 0, 1280, 720);
            pb.begin_children();
            pb.null("all", 0, 0, 1280, 720);
            pb.begin_children();
            pb.picture("P_bg", 0, 0, 0, 1280, 720);
            pb.picture("P_head", 0, 0, 320, 600, 60);
            pb.end_children();
            pb.end_children();
            auto pl = parse_bflyt(pb.finish(), &error);
            if (pl) {
                BandStats pc2;
                const auto bp = layout_bands(*pl, 360, true, &pc2);
                check(bp[size_t(pl->find("all"))] < 0 && bp[size_t(pl->find("P_bg"))] < 0 &&
                          bp[size_t(pl->find("P_head"))] == 0,
                      "a tall panel stays, its header moves");
            }
            // Texts on a tall window that stays, as siblings under a 0x0 group:
            // they stay with it.
            lgp::test::LayoutBuilder wb(1280, 720, false);
            wb.materials({{"M", -1}});
            wb.null("RootPane", 0, 0, 1280, 720);
            wb.begin_children();
            wb.null("N_info", -400, 170, 0, 0);
            wb.begin_children();
            wb.picture("W_window", 0, 0, 0, 480, 430);
            wb.text("T_number", 0, 0, 130, 200, 30, 20, 0);
            wb.end_children();
            wb.end_children();
            auto wl = parse_bflyt(wb.finish(), &error);
            if (wl) {
                BandStats wc;
                const auto bw = layout_bands(*wl, 360, true, &wc);
                check(bw[size_t(wl->find("T_number"))] < 0 && wc.groups[0] == 0, "a text on a tall window stays");
            }
        }
        // .gfpak: an LZ4 SARC with the layout is shifted and stored again in its place.
        {
            // "abc" + match (offset 3, length 4 + 5) + last literal "x": "abcabcabcabcx"
            const uint8_t block[] = {0x35, 'a', 'b', 'c', 0x03, 0x00, 0x10, 'x'};
            const auto dec = lz4_block_decode(block, 13);
            check(dec && std::string(dec->begin(), dec->end()) == "abcabcabcabcx", "LZ4 block with a match decodes");
            {
                std::vector<uint8_t> text;
                for (int k = 0; k < 20000; ++k)
                    text.push_back(uint8_t("layout pane "[k % 12] + (k % 997 == 0 ? 1 : 0)));
                for (int k = 0; k < 3000; ++k)
                    text.push_back(uint8_t(k * 7919 >> 3));
                const auto z = lz4_block_compress(text);
                const auto zback = lz4_block_decode(z, text.size());
                check(zback && *zback == text && z.size() < text.size() / 2, "LZ4 compression round trip");
                const std::vector<uint8_t> tiny = {1, 2, 3};
                const auto t2 = lz4_block_decode(lz4_block_compress(tiny), 3);
                check(t2 && *t2 == tiny, "LZ4 compression of 3 bytes");
            }
            std::vector<uint8_t> sarc = lgp::test::make_sarc({{"blyt/a.bflyt", std::string(file.begin(), file.end())}});
            const auto packed = lz4_block_compress(sarc);
            std::vector<uint8_t> pack(0x40 + 0x18 * 2, 0);
            std::memcpy(pack.data(), "GFLXPACK", 8);
            lgp::test::put32(pack, 0x10, 2);
            lgp::test::put64(pack, 0x18, 0x40);
            const auto entry = [&](size_t i, uint16_t comp, const std::vector<uint8_t>& raw, uint32_t size) {
                while (pack.size() % 16 != 0)
                    pack.push_back(0);
                const size_t at = pack.size();
                pack.insert(pack.end(), raw.begin(), raw.end());
                const size_t e = 0x40 + i * 0x18;
                lgp::test::put16(pack, e + 2, comp);
                lgp::test::put32(pack, e + 4, size);
                lgp::test::put32(pack, e + 8, uint32_t(raw.size()));
                lgp::test::put64(pack, e + 0x10, at);
            };
            entry(0, 0, std::vector<uint8_t>{'B', 'N', 'T', 'X'}, 4);
            entry(1, 2, packed, uint32_t(sarc.size()));
            ShiftStats gc;
            check(shift_gfpak(pack, o, &gc) && gc.gfpaks == 1 && gc.archives == 0 && gc.groups == 2,
                  "the SARC in the .gfpak is shifted");
            const size_t e = 0x40 + 0x18;
            const uint64_t off = get32(pack, e + 0x10);
            const auto out = lz4_block_decode(std::span<const uint8_t>(pack.data() + off, get32(pack, e + 8)),
                                              get32(pack, e + 4));
            const auto osarc = out ? Sarc::parse(*out, &error) : std::nullopt;
            const auto ol = osarc ? parse_bflyt(osarc->file("blyt/a.bflyt"), &error) : std::nullopt;
            check(ol && std::abs(ol->panes[size_t(ol->find("N_foot"))].translate[1] - (40 - 120)) < 1e-3f,
                  "footer down inside the .gfpak");
            check(get16(pack, 0x40 + 2) == 0 && get32(pack, 0x40 + 0x10) != off, "the other file stays");
            check(gc.in_place == 1 && gc.appended == 0, "the shifted file fits its old place");
        }
    }
    // A small part (a row of balls) whose part layout also holds far off
    // dummy panes: the part pane counts by its content (36 high, a unit at
    // the bottom) and moves; the part layout itself is no screen layout, so
    // its dummies stay where they are.
    {
        lgp::test::LayoutBuilder plate(1280, 720, false);
        plate.materials({{"M", -1}});
        plate.null("RootPane", 0, 0, 1280, 720);
        plate.begin_children();
        plate.picture("P_bg", 0, 0, 0, 126, 36);
        plate.picture("P_dummy", 0, 662, -377, 16, 16);
        plate.end_children();
        const auto pf = plate.finish();
        lgp::test::LayoutBuilder top(1280, 720, false);
        top.materials({{"M", -1}});
        top.null("RootPane", 0, 0, 1280, 720);
        top.begin_children();
        top.null("N_pos", -583, -274, 40, 40);
        top.begin_children();
        top.part("L_plate", 0, 0, 1376, 770, "plate", {});
        top.end_children();
        top.end_children();
        const auto tf = top.finish();
        std::vector<uint8_t> arc = lgp::test::make_sarc({{"blyt/plate.bflyt", std::string(pf.begin(), pf.end())},
                                                        {"blyt/top.bflyt", std::string(tf.begin(), tf.end())}});
        std::string error;
        const auto pl = parse_bflyt(pf, &error);
        const LayoutRect box = pl ? layout_content(*pl) : LayoutRect{};
        check(box.h == 36 && box.w == 126, "the content box leaves the dummy out");
        ShiftOptions o;
        o.shift = 120;
        o.units = 360;
        ShiftStats sc;
        check(shift_sarc(arc, o, &sc) && sc.layouts == 1 && sc.groups == 1, "only the host layout moves");
        const auto out = Sarc::parse(arc, &error);
        const auto ol = out ? parse_bflyt(out->file("blyt/top.bflyt"), &error) : std::nullopt;
        const auto op = out ? parse_bflyt(out->file("blyt/plate.bflyt"), &error) : std::nullopt;
        check(ol && std::abs(ol->panes[size_t(ol->find("N_pos"))].translate[1] - (-274 - 120)) < 1e-3f,
              "the ball row moves down");
        check(op && op->panes[size_t(op->find("P_dummy"))].translate[1] == -377, "the dummy stays");
    }
    // A part layout with only a footer (a button bar shown as a full-screen
    // part, and also loaded alone): its footer moves inside it, the part pane
    // stays, so the footer moves once.
    {
        lgp::test::LayoutBuilder bar(1280, 720, false);
        bar.materials({{"M", -1}});
        bar.null("RootPane", 0, 0, 1280, 720);
        bar.begin_children();
        bar.picture("P_bar", 0, 0, -330, 1280, 60);
        bar.end_children();
        const auto bf = bar.finish();
        lgp::test::LayoutBuilder host(1280, 720, false);
        host.materials({{"M", -1}});
        host.null("RootPane", 0, 0, 1280, 720);
        host.begin_children();
        host.part("L_bar", 0, 0, 1280, 720, "bar", {});
        host.end_children();
        const auto hf = host.finish();
        std::vector<uint8_t> arc = lgp::test::make_sarc({{"blyt/bar.bflyt", std::string(bf.begin(), bf.end())},
                                                        {"blyt/host.bflyt", std::string(hf.begin(), hf.end())}});
        ShiftOptions o;
        o.shift = 120;
        o.units = 360;
        ShiftStats sc;
        std::string error;
        check(shift_sarc(arc, o, &sc) && sc.layouts == 1 && sc.groups == 1, "the bar layout moves, the host not");
        const auto out = Sarc::parse(arc, &error);
        const auto ob = out ? parse_bflyt(out->file("blyt/bar.bflyt"), &error) : std::nullopt;
        const auto oh = out ? parse_bflyt(out->file("blyt/host.bflyt"), &error) : std::nullopt;
        check(ob && std::abs(ob->panes[size_t(ob->find("P_bar"))].translate[1] - (-450)) < 1e-3f, "footer down once");
        check(oh && oh->panes[size_t(oh->find("L_bar"))].translate[1] == 0, "the part pane stays");
    }
    // Edge bands: a full-width gradient ending at the bottom edge (360 high,
    // so no unit) moves down as a whole, a lower one lying on it moves with
    // it; the screen background and a side panel at the edge stay.
    {
        lgp::test::LayoutBuilder bg(1280, 720, false);
        bg.materials({{"M", -1}});
        bg.null("RootPane", 0, 0, 1280, 720);
        bg.begin_children();
        bg.null("all", 0, 0, 0, 0);
        bg.begin_children();
        bg.picture("P_bg_00", 0, 0, 0, 1280, 720);
        bg.picture("P_bg_01", 0, 0, -360, 1280, 320, 2 << 2);
        bg.picture("P_bg_02", 0, 0, -360, 1280, 360, 2 << 2);
        bg.picture("P_side", 0, -440, -360, 400, 400, 2 << 2);
        bg.end_children();
        bg.end_children();
        const auto f = bg.finish();
        std::string error;
        const auto l = parse_bflyt(f, &error);
        check(l.has_value(), "edge band layout parses");
        if (l) {
            ShiftOptions o;
            o.shift = 120;
            o.units = 360;
            ShiftStats sc;
            const auto sh = layout_shifts(*l, o, &sc);
            check(sh[size_t(l->find("P_bg_02"))][1] == -120, "the gradient at the edge moves down");
            check(sh[size_t(l->find("P_bg_01"))][1] == -120, "the band lying on it moves with it");
            check(sh[size_t(l->find("P_bg_00"))][1] == 0, "the background stays");
            check(sh[size_t(l->find("P_side"))][1] == 0, "a side panel at the edge stays");
        }
    }
    // Numbered siblings: a grid of badges whose top row lies in the top band
    // stays together; a row of footer buttons all in the band still moves,
    // though its group's name is numbered like a staying group of other size.
    {
        lgp::test::LayoutBuilder g(1280, 720, false);
        g.materials({{"M", -1}});
        g.null("RootPane", 0, 0, 1280, 720);
        g.begin_children();
        g.null("inout_00", 0, 0, 40, 40);
        g.begin_children();
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 2; ++c) {
                const std::string name = "P_badge_0" + std::to_string(r * 2 + c);
                g.picture(name.c_str(), 0, 220 + 124 * c, 274 - 112 * r, 98, 98);
            }
        g.end_children();
        g.null("inout_01", 0, 0, 40, 40);
        g.begin_children();
        g.picture("P_button_00", 0, -200, -310, 200, 80);
        g.picture("P_button_01", 0, 200, -310, 200, 80);
        g.end_children();
        g.null("inout_02", 0, 0, 40, 40);
        g.begin_children();
        g.picture("P_text_00", 0, -300, 0, 300, 200);
        g.end_children();
        g.end_children();
        const auto f = g.finish();
        std::string error;
        const auto l = parse_bflyt(f, &error);
        check(l.has_value(), "badge grid layout parses");
        if (l) {
            ShiftOptions o;
            o.shift = 120;
            o.units = 360;
            ShiftStats sc;
            const auto sh = layout_shifts(*l, o, &sc);
            check(sh[size_t(l->find("P_badge_00"))][1] == 0 && sh[size_t(l->find("P_badge_01"))][1] == 0,
                  "the top row of the grid stays with the others");
            check(sh[size_t(l->find("inout_01"))][1] == -120, "the footer row moves (its numbered sibling is unlike it)");
            o.keep = {"P_button_00", "inout_01"};
            const auto kept = layout_shifts(*l, o, &sc);
            check(kept[size_t(l->find("inout_01"))][1] == 0 && kept[size_t(l->find("P_button_00"))][1] == 0,
                  "a kept pane stays with everything under it");
            o.keep = {"P_button_01"};  // inside the moving footer row: it gets the opposite shift
            const auto inner = layout_shifts(*l, o, &sc);
            check(inner[size_t(l->find("inout_01"))][1] == -120 && inner[size_t(l->find("P_button_01"))][1] == 120 &&
                      inner[size_t(l->find("P_button_00"))][1] == 0,
                  "a kept pane inside a moving group stays on screen");
        }
    }
    // axis 0 (a wider screen): corner units go out in x, the middle stays, a
    // parent scaled by 2 halves the local shift.
    {
        lgp::test::LayoutBuilder x(1280, 720, false);
        x.materials({{"M", -1}});
        x.null("RootPane", 0, 0, 1280, 720);
        x.begin_children();
        x.null("N_plate", -540, -320, 274, 96);
        x.begin_children();
        x.picture("P_plate", 0, 0, 0, 274, 96);
        x.end_children();
        x.picture("P_enemy", 0, 525, 308, 274, 86);
        x.picture("P_middle", 0, -100, 0, 300, 200);
        x.null("N_scaled", 0, 0, 10, 10);
        x.begin_children();
        x.picture("P_bg", 0, 0, 0, 640, 360);  // 1280 wide on screen: no unit, its icon is
        x.picture("P_icon", 0, 270, -150, 50, 50);
        x.end_children();
        x.end_children();
        const auto f = x.finish();
        std::string error;
        auto l = parse_bflyt(f, &error);
        check(l.has_value(), "x layout parses");
        if (l) {
            l->panes[size_t(l->find("N_scaled"))].scale[0] = l->panes[size_t(l->find("N_scaled"))].scale[1] = 2;
            ShiftOptions o;
            o.shift = 144;
            o.units = 400;
            o.axis = 0;
            ShiftStats sc;
            const auto sh = layout_shifts(*l, o, &sc);
            check(sh[size_t(l->find("N_plate"))][0] == -144 && sh[size_t(l->find("N_plate"))][1] == 0,
                  "the bottom left plate goes left");
            check(sh[size_t(l->find("P_enemy"))][0] == 144, "the top right plate goes right");
            check(sh[size_t(l->find("P_middle"))][0] == 0, "the middle stays");
            check(std::abs(sh[size_t(l->find("P_icon"))][0] - 72) < 1e-4f, "under a parent scaled 2: half the local step");
        }
        // Curves along x: the plate's slide-in curve in x moves with it, a y curve of
        // a unit moved in x stays.
        const auto anim = lgp::test::make_bflan(10, {{"N_plate", "FLPA", 0, -700}, {"P_enemy", "FLPA", 1, 308}});
        std::vector<uint8_t> arc = lgp::test::make_sarc({{"blyt/x.bflyt", std::string(f.begin(), f.end())},
                                                        {"anim/x_in.bflan", std::string(anim.begin(), anim.end())}});
        ShiftOptions o;
        o.shift = 144;
        o.units = 400;
        o.axis = 0;
        ShiftStats sc;
        check(shift_sarc(arc, o, &sc), "the x archive changes");
        check(sc.curves == 1, "one x curve");
        const auto shifted = Sarc::parse(arc, &error);
        const auto sa = shifted ? parse_bflan(shifted->file("anim/x_in.bflan"), &error) : std::nullopt;
        check(sa.has_value(), "shifted x animation parses");
        if (sa) {
            check(std::abs(sa->groups[0].curves[0].keys[1].value - (-844)) < 1e-3f, "plate curve goes left");
            check(sa->groups[1].curves[0].keys[1].value == 308, "the y curve stays");
        }
    }
    // `move`: a corner node holding two plates (wider than the units) and a row
    // of numbered command circles (a family only partly in the band) move as
    // one group each; the nodes under them do not move a second time.
    {
        lgp::test::LayoutBuilder x(1280, 720, false);
        x.materials({{"M", -1}});
        x.null("RootPane", 0, 0, 1280, 720);
        x.begin_children();
        x.null("N_pos_LT_00", 0, 0, 40, 40);
        x.begin_children();
        x.picture("P_plate_00", 0, -510, -319, 476, 96);
        x.picture("P_plate_01", 0, -258, -319, 476, 96);
        x.end_children();
        x.null("inout", 0, 0, 0, 0);
        x.begin_children();
        x.null("N_net_00", 50, 0, 10, 10);
        x.begin_children();
        x.picture("P_target", 0, 532, -178, 100, 88);
        x.end_children();
        for (int k = 0; k < 4; ++k) {
            const std::string name = "N_command_0" + std::to_string(k);
            x.null(name.c_str(), 162 + 136 * k, -286, 30, 40);
            x.begin_children();
            x.picture(("P_circle_0" + std::to_string(k)).c_str(), 0, 0, 0, 162, 162);
            x.end_children();
        }
        x.end_children();
        x.picture("P_middle", 0, -100, 0, 300, 200);
        x.end_children();
        const auto f = x.finish();
        std::string error;
        auto l = parse_bflyt(f, &error);
        check(l.has_value(), "move layout parses");
        if (l) {
            ShiftOptions o;
            o.shift = 144;
            o.units = 300;
            o.axis = 0;
            ShiftStats sc;
            auto sh = layout_shifts(*l, o, &sc);
            check(sh[size_t(l->find("N_pos_LT_00"))][0] == 0, "without move the plates stay");
            check(sh[size_t(l->find("N_net_00"))][0] == 144, "without move the target goes alone");
            o.move = {"N_pos_LT_00", "inout"};
            sh = layout_shifts(*l, o, &sc);
            check(sh[size_t(l->find("N_pos_LT_00"))][0] == -144, "the corner node goes left");
            check(sh[size_t(l->find("inout"))][0] == 144, "the circle row goes right");
            check(sh[size_t(l->find("N_net_00"))][0] == 0 && sh[size_t(l->find("N_command_03"))][0] == 0,
                  "nothing under a moved pane moves again");
            check(sh[size_t(l->find("P_middle"))][0] == 0, "the middle still stays");
        }
    }
    // hud: the listed layout moves by its own amount per side (top 30, bottom
    // none), the other one by the whole shift.
    {
        std::vector<uint8_t> arc = lgp::test::make_sarc({{"blyt/battle.bflyt", std::string(file.begin(), file.end())},
                                                        {"blyt/menu.bflyt", std::string(file.begin(), file.end())}});
        ShiftOptions o;
        o.shift = 120;
        o.hud = {"battle"};
        o.hud_shift = {30, 0};
        ShiftStats sc;
        check(shift_sarc(arc, o, &sc), "the hud archive changes");
        const auto shifted = Sarc::parse(arc, &error);
        const auto hb = shifted ? parse_bflyt(shifted->file("blyt/battle.bflyt"), &error) : std::nullopt;
        const auto hm = shifted ? parse_bflyt(shifted->file("blyt/menu.bflyt"), &error) : std::nullopt;
        check(hb && hm, "hud layouts parse");
        if (hb && hm) {
            check(std::abs(hb->panes[size_t(hb->find("P_head"))].translate[1] - (320 + 30)) < 1e-3f, "hud header by its top bound");
            check(hb->panes[size_t(hb->find("N_foot"))].translate[1] == 40.f, "hud footer stays at bound 0");
            check(std::abs(hm->panes[size_t(hm->find("P_head"))].translate[1] - (320 + 120)) < 1e-3f, "menu header to the edge");
            check(std::abs(hm->panes[size_t(hm->find("N_foot"))].translate[1] - (40 - 120)) < 1e-3f, "menu footer to the edge");
        }
    }
    std::printf(failures ? "layout_shift: %d failures\n" : "layout_shift: ok\n", failures);
    return failures ? 1 : 0;
}
