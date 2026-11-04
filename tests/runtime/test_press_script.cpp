// --press scripts of the boot program (lgp/boot/press_script.hpp): parsing,
// frame/time/event/stable-image triggers, holds and latching between hid
// samples, and the image hash watch.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "lgp/boot/press_script.hpp"
#include "lgp/platform/pad_input.hpp"

using namespace lgp;
using boot::Press;
using boot::PressClock;
using boot::PressEvent;

namespace {

int g_failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAILED: %s\n", what);
        ++g_failures;
    }
}

boot::PressScript script(const std::string& spec) {
    std::vector<Press> presses;
    std::string error;
    if (!boot::parse_press_script(spec, &presses, &error)) {
        std::fprintf(stderr, "parse %s: %s\n", spec.c_str(), error.c_str());
        std::exit(1);
    }
    return boot::PressScript(std::move(presses));
}

PressClock at(uint64_t frame, double seconds, uint32_t starts = 0, uint32_t ends = 0, double changed = -1) {
    PressClock c;
    c.image_changed = changed;
    c.frame = frame;
    c.seconds = seconds;
    c.events[size_t(PressEvent::MovieStart)] = starts;
    c.events[size_t(PressEvent::MovieEnd)] = ends;
    return c;
}

uint64_t buttons(boot::PressScript& s, const PressClock& c) { return s.sample(c, {}).buttons; }

void test_parse() {
    std::vector<Press> p;
    std::string error;
    check(boot::parse_press_script("300:a,600:plus+b:4", &p, &error) && p.size() == 2, "frame entries");
    check(p[0].trigger == Press::Trigger::Frame && p[0].frame == 300 && !p[0].hold_seconds && p[0].hold == 10,
          "frame trigger with default hold 10 frames");
    check(p[1].buttons == (platform::kButtonPlus | platform::kButtonB) && p[1].hold == 4, "keys and hold");
    check(boot::parse_press_script("12.5s:a,movie-end+1.5s:x:0.3s,movie-start#2+30:lup", &p, &error) &&
              p.size() == 3,
          "time and event entries");
    check(p[0].trigger == Press::Trigger::Seconds && p[0].seconds == 12.5 && p[0].hold_seconds && p[0].hold == 0.2,
          "seconds trigger with default hold 0.2 s");
    check(p[1].trigger == Press::Trigger::Event && p[1].event == PressEvent::MovieEnd && p[1].occurrence == 1 &&
              p[1].offset_seconds && p[1].offset == 1.5 && p[1].hold_seconds && p[1].hold == 0.3,
          "movie-end+1.5s with hold 0.3s");
    check(p[2].event == PressEvent::MovieStart && p[2].occurrence == 2 && !p[2].offset_seconds &&
              p[2].offset == 30 && p[2].left.y == 32767,
          "movie-start#2+30 frames, stick");
    check(boot::parse_press_script("stable:500ms:a,stable:1.5s:b:300ms,movie-end+after-stable:x,"
                                   "40s+after-stable=250ms:y,movie-end+1s+after-stable:a,700ms:b",
                                   &p, &error) &&
              p.size() == 6,
          "stable entries");
    check(p[0].trigger == Press::Trigger::Stable && p[0].stable == 0.5 && p[0].hold_seconds && p[0].hold == 0.2,
          "stable:500ms with default hold 0.2 s");
    check(p[1].stable == 1.5 && p[1].hold_seconds && p[1].hold == 0.3, "stable:1.5s with hold 300ms");
    check(p[2].trigger == Press::Trigger::Event && p[2].after_stable && p[2].stable == 0.5, "movie-end+after-stable");
    check(p[3].trigger == Press::Trigger::Seconds && p[3].seconds == 40 && p[3].after_stable && p[3].stable == 0.25,
          "40s+after-stable=250ms");
    check(p[4].offset_seconds && p[4].offset == 1 && p[4].after_stable, "offset before +after-stable");
    check(p[5].trigger == Press::Trigger::Seconds && p[5].seconds == 0.7, "700ms trigger");
    check(boot::uses_image(p), "uses_image");
    check(boot::parse_press_script("1:a,2s:b", &p, &error) && !boot::uses_image(p), "no image without stable");
    for (const char* bad : {"a", "300", "300:q", "x:a", "-1s:a", "movie-end+:a", "movie-end#0:a", "movie-ends:a",
                            "10:a:1:2", "10:a:zz", "stable:a", "stable:5:a", "stable::a", "stable:1s:a:1:2",
                            "+after-stable:a", "1s+after-stable=3:a", "1s+after-stablex:a", "1ms5:a"})
        check(!boot::parse_press_script(bad, &p, &error), bad);
}

void test_frames() {
    auto s = script("100:a:5");
    check(buttons(s, at(99, 1.0)) == 0, "before the frame");
    check(buttons(s, at(100, 1.1)) == platform::kButtonA, "at the frame");
    check(buttons(s, at(104, 1.2)) == platform::kButtonA, "within hold");
    check(buttons(s, at(105, 1.3)) == 0, "hold counts from the trigger frame");
    // Latching: the whole window lies between two samples.
    auto t = script("100:b:5");
    check(buttons(t, at(50, 0.5)) == 0, "sample before");
    check(buttons(t, at(400, 0.6)) == platform::kButtonB, "missed window still reported once");
    check(buttons(t, at(405, 0.7)) == 0, "then released");
    check(t.fired() == 1, "fired count");
}

void test_seconds() {
    auto s = script("2s:a");
    check(buttons(s, at(1000, 1.99)) == 0, "before 2 s");
    check(buttons(s, at(1001, 2.0)) == platform::kButtonA, "at 2 s");
    check(buttons(s, at(1300, 2.19)) == platform::kButtonA, "held 0.2 s regardless of frames");
    check(buttons(s, at(1301, 2.2)) == 0, "released after 0.2 s");
    auto t = script("2s:a:3");  // frame hold on a time trigger counts from the first sample after it
    check(buttons(t, at(10, 2.5)) == platform::kButtonA, "late sample");
    check(buttons(t, at(12, 2.6)) == platform::kButtonA, "within 3 frames");
    check(buttons(t, at(13, 2.7)) == 0, "after 3 frames");
}

void test_events() {
    auto s = script("movie-end+1s:a,movie-end#2:x:2,movie-start:b");
    check(buttons(s, at(10, 5.0)) == 0, "no event yet");
    check(buttons(s, at(20, 6.0, 1)) == platform::kButtonB, "movie-start fires at once");
    check(buttons(s, at(30, 6.3, 1)) == 0, "movie-start released after 0.2 s");
    check(buttons(s, at(40, 30.0, 1, 1)) == 0, "end seen, offset not yet");
    check(buttons(s, at(50, 30.9, 1, 1)) == 0, "still inside the offset");
    check(buttons(s, at(60, 31.0, 1, 1)) == platform::kButtonA, "1 s after the first end");
    check(buttons(s, at(70, 31.25, 1, 1)) == 0, "released");
    check(buttons(s, at(80, 40.0, 2, 2)) == platform::kButtonX, "second end");
    check(buttons(s, at(81, 40.1, 2, 2)) == platform::kButtonX, "second end held 2 frames");
    check(buttons(s, at(82, 40.2, 2, 2)) == 0, "released after 2 frames");
    check(s.fired() == 3, "all fired");
    auto u = script("movie-end:a");
    size_t fired_index = 99;
    uint64_t fired_frame = 0;
    u.on_fire = [&](size_t i, uint64_t frame, double) {
        fired_index = i;
        fired_frame = frame;
    };
    buttons(u, at(5, 1.0, 0, 0));
    check(fired_index == 99, "no callback before the event");
    buttons(u, at(7, 1.1, 0, 1));
    check(fired_index == 0 && fired_frame == 7, "callback with frame");

    // battle-start / battle-end count the wild-battles mod's battles.
    auto b = script("battle-start:b,battle-end#2+1s:a");
    PressClock c = at(10, 5.0);
    check(buttons(b, c) == 0, "no battle yet");
    c = at(11, 6.0);
    c.events[size_t(boot::PressEvent::BattleStart)] = 1;
    check(buttons(b, c) == platform::kButtonB, "battle-start");
    c = at(20, 9.0);
    c.events[size_t(boot::PressEvent::BattleStart)] = 1;
    c.events[size_t(boot::PressEvent::BattleEnd)] = 1;
    check(buttons(b, c) == 0, "first end is not #2");
    c.frame = 30;
    c.seconds = 20.0;
    c.events[size_t(boot::PressEvent::BattleEnd)] = 2;
    check(buttons(b, c) == 0, "second end seen");
    c.frame = 40;
    c.seconds = 21.0;
    check(buttons(b, c) == platform::kButtonA, "1 s after the second end");
}

void test_stable() {
    // Two pages of a dialogue: text scrolls in until 1.0 s, A at 1.5 s; the
    // next page scrolls until 2.4 s, A at 2.9 s.
    auto s = script("stable:500ms:a,stable:500ms:a");
    check(buttons(s, at(1, 0.0)) == 0, "no image yet");
    check(buttons(s, at(2, 0.6, 0, 0, 0.3)) == 0, "armed at the first sample, image changed at 0.3 s");
    check(buttons(s, at(30, 1.2, 0, 0, 1.0)) == 0, "text still scrolling");
    check(buttons(s, at(40, 1.49, 0, 0, 1.0)) == 0, "0.49 s still");
    check(buttons(s, at(41, 1.5, 0, 0, 1.0)) == platform::kButtonA, "0.5 s still: first A");
    check(s.fired() == 1, "only the first fires");
    check(buttons(s, at(45, 1.6, 0, 0, 1.0)) == platform::kButtonA, "held");
    check(buttons(s, at(50, 1.7, 0, 0, 1.0)) == 0, "released: arms the second at 1.7 s");
    check(buttons(s, at(60, 2.1, 0, 0, 1.0)) == 0, "second: 0.4 s since arming");
    check(buttons(s, at(70, 2.3, 0, 0, 2.25)) == 0, "next page scrolls in");
    check(buttons(s, at(80, 2.74, 0, 0, 2.4)) == 0, "0.34 s still");
    check(buttons(s, at(90, 2.9, 0, 0, 2.4)) == platform::kButtonA, "second A");
    check(s.fired() == 2, "both fired");

    // Without any change after arming, stability counts from arming.
    auto t = script("1:b,stable:300ms:a");
    check(buttons(t, at(1, 0.0, 0, 0, 0.0)) == platform::kButtonB, "b");
    check(buttons(t, at(11, 0.2, 0, 0, 0.0)) == 0, "b released, stable armed at 0.2 s");
    check(buttons(t, at(20, 0.49, 0, 0, 0.0)) == 0, "0.29 s since arming");
    check(buttons(t, at(21, 0.5, 0, 0, 0.0)) == platform::kButtonA, "0.3 s since arming");

    // +after-stable: the base trigger arms, then the image has to stand still.
    auto u = script("movie-end+after-stable:x,2s+after-stable=100ms:y");
    check(buttons(u, at(10, 1.0, 1, 0, 0.9)) == 0, "video running");
    check(buttons(u, at(20, 1.5, 1, 1, 1.5)) == 0, "video ended at 1.5 s");
    check(buttons(u, at(30, 1.9, 1, 1, 1.8)) == 0, "fade-out");
    check(buttons(u, at(40, 2.05, 1, 1, 1.8)) == 0, "2 s armed y at 2.05 s");
    check(buttons(u, at(50, 2.16, 1, 1, 1.8)) == platform::kButtonY, "y 0.11 s after arming");
    check((buttons(u, at(60, 2.25, 1, 1, 1.8)) & platform::kButtonX) == 0, "x: 0.45 s after the last change");
    check((buttons(u, at(70, 2.35, 1, 1, 1.8)) & platform::kButtonX) != 0, "x: 0.55 s after the last change");
    check(u.fired() == 2, "both");
}

void test_image_watch() {
    boot::ImageWatch w;
    check(w.last_change() < 0, "nothing seen");
    std::vector<uint8_t> a(1280 * 720 * 4, 7), b = a;
    b[1234567] ^= 1;
    check(boot::hash_image(a.data(), a.size()) != boot::hash_image(b.data(), b.size()), "one bit changes the hash");
    check(boot::hash_image(a.data(), 5) != boot::hash_image(a.data(), 6), "tail bytes count");
    w.observe(boot::hash_image(a.data(), a.size()), 1.0);
    check(w.last_change() == 1.0 && w.changes() == 1, "first observation");
    w.observe(boot::hash_image(a.data(), a.size()), 2.0);
    check(w.last_change() == 1.0 && w.changes() == 1, "same image");
    w.observe(boot::hash_image(b.data(), b.size()), 3.0);
    check(w.last_change() == 3.0 && w.changes() == 2, "changed image");
}

void test_sticks_and_idle() {
    auto s = script("1:lright+a,1:rdown");
    platform::ControllerState idle;
    idle.style = platform::ControllerStyle::Handheld;
    const platform::ControllerState st = s.sample(at(1, 0), idle);
    check(st.style == platform::ControllerStyle::Handheld, "idle style kept");
    check(st.buttons == platform::kButtonA && st.left.x == 32767 && st.right.y == -32767, "sticks combined");
}

void test_scripted_input() {
    std::vector<Press> p;
    std::string error;
    boot::parse_press_script("3s:a", &p, &error);
    double now = 0;
    boot::ScriptedInput in(boot::PressScript(std::move(p)), {}, [&] { return at(0, now); });
    in.poll();
    check(in.controller(0).buttons == 0 && in.controller_count() == 1, "input before");
    now = 3.05;
    in.poll();
    check(in.controller(0).buttons == platform::kButtonA && in.fired() == 1, "input fires on poll");
}

// KEYS "throw": no button reaches the game, the motion source sees it held
// (here the real ControllerMapper of a right Joy-Con: rest, swing, rest).
void test_scripted_throw() {
    std::vector<Press> p;
    std::string error;
    check(boot::parse_press_script("1s:throw:0.1s,2s:a+throw", &p, &error), "throw parses");
    check(p.size() == 2 && p[0].buttons == boot::kPressThrow && p[1].buttons == (platform::kButtonA | boot::kPressThrow),
          "throw is its own key");
    platform::MapperConfig config;
    config.style = platform::ControllerStyle::JoyconRight;
    platform::ControllerMapper mapper(config);
    std::vector<bool> seen;
    double now = 0;
    boot::ScriptedInput in(
        boot::PressScript(std::move(p)), {}, [&] { return at(0, now); },
        [&](bool throwing, double seconds) {
            seen.push_back(throwing);
            platform::HostPad pad;
            pad.connected = true;
            pad.throw_gesture = throwing;
            return mapper.map(pad, uint64_t(seconds * 1e9) + 1).motion;
        });
    in.poll();
    const platform::MotionState rest = in.controller(0).motion;
    check(rest.valid && seen.back() == false, "at rest before the throw");
    now = 1.0;
    in.poll();
    check(in.controller(0).buttons == 0 && seen.back(), "throw held, no button");
    float peak = 0;
    for (now = 1.0; now < 1.5; now += 0.01) {
        in.poll();
        for (int a = 0; a < 3; ++a)
            peak = std::max(peak, std::fabs(in.controller(0).motion.accel[a] - rest.accel[a]));
    }
    check(peak > 3.0f, "the Joy-Con swings");
    now = 2.0;
    in.poll();
    check(in.controller(0).buttons == platform::kButtonA && seen.back(), "a with a throw");
}

// p2: entries press on player 2's controller; player 1 stays as it is.
void test_player2() {
    std::vector<Press> p;
    std::string error;
    check(boot::parse_press_script("1s:a,p2:2s:b+lup,p1:3s:x", &p, &error) && p.size() == 3, "p1:/p2: parse");
    check(p[0].player == 0 && p[1].player == 1 && p[2].player == 0, "players");
    check(boot::press_players(p) == 2, "two players");
    check(boot::parse_press_script("p2:1s:throw", &p, &error) && p[0].player == 1 && p[0].buttons == boot::kPressThrow,
          "p2 throw parses");
    check(!boot::parse_press_script("p3:1s:a", &p, &error), "no player 3");
    check(boot::parse_press_script("1:a", &p, &error) && boot::press_players(p) == 1, "one player");

    boot::parse_press_script("1s:a,p2:2s:b+lup", &p, &error);
    double now = 0;
    platform::ControllerState idle, idle2;
    idle.style = platform::ControllerStyle::Handheld;
    idle2.style = platform::ControllerStyle::JoyconRight;
    boot::ScriptedInput in(boot::PressScript(std::move(p)), idle, [&] { return at(0, now); }, {}, 2, idle2);
    in.poll();
    check(in.controller_count() == 2, "two controllers");
    check(in.controller(1).style == platform::ControllerStyle::JoyconRight && in.controller(1).buttons == 0,
          "player 2 connected, idle");
    now = 1.05;
    in.poll();
    check(in.controller(0).buttons == platform::kButtonA && in.controller(1).buttons == 0, "player 1 presses");
    now = 2.05;
    in.poll();
    check(in.controller(0).buttons == 0 && in.controller(1).buttons == platform::kButtonB &&
              in.controller(1).left.y == 32767 && in.controller(0).left.y == 0,
          "player 2 presses");
    check(in.fired() == 2, "both fired");

    // p2:…:throw drives player 2's motion only.
    boot::parse_press_script("1s:throw,p2:2s:throw", &p, &error);
    std::vector<int> seen1, seen2;
    now = 0;
    const auto source = [](std::vector<int>* seen) {
        return [seen](bool throwing, double) {
            seen->push_back(throwing);
            platform::MotionState m;
            m.valid = true;
            m.accel[0] = throwing ? 5.0f : 0.0f;
            return m;
        };
    };
    boot::ScriptedInput shake(boot::PressScript(std::move(p)), idle, [&] { return at(0, now); }, source(&seen1), 2,
                              idle2, source(&seen2));
    shake.poll();
    check(seen1.back() == 0 && seen2.back() == 0 && shake.controller(1).motion.valid, "both at rest");
    now = 1.05;
    shake.poll();
    check(seen1.back() == 1 && seen2.back() == 0 && shake.controller(0).buttons == 0, "player 1 throws");
    now = 2.05;
    shake.poll();
    check(seen1.back() == 0 && seen2.back() == 1 && shake.controller(1).buttons == 0 &&
              shake.controller(1).motion.accel[0] == 5.0f && shake.controller(0).motion.accel[0] == 0.0f,
          "player 2 shakes");

    // The one-player sample leaves p2: entries waiting.
    auto s = script("p2:1:a");
    check(buttons(s, at(5, 0)) == 0 && s.fired() == 0, "p2 ignored with one player");
}

// touch@X/Y: a finger while the press holds, through the mapper.
void test_touch() {
    std::vector<Press> p;
    std::string error;
    check(boot::parse_press_script("1s:touch@640/850:0.3s,2s:a+touch@10.5/20", &p, &error) && p.size() == 2 &&
              p[0].touch && p[0].touch_x == 640 && p[0].touch_y == 850 && p[0].buttons == 0 &&
              p[1].touch && p[1].touch_x == 10.5f && p[1].buttons == platform::kButtonA,
          "touch entries");
    check(!boot::parse_press_script("1s:touch@640", &p, &error), "touch needs X/Y");
    check(!boot::parse_press_script("1s:touch@-1/5", &p, &error), "touch not negative");
    boot::parse_press_script("1s:touch@640/850:0.3s,2s:touch@100/900", &p, &error);
    double now = 0;
    boot::ScriptedInput in(boot::PressScript(std::move(p)), {}, [&] { return at(0, now); });
    in.set_touch_mapper([](float x, float y) {
        platform::TouchPoint t;
        t.x = x / 1280 * platform::kTouchScreenWidth;
        t.y = y / 960 * platform::kTouchScreenHeight;
        return t;
    });
    in.poll();
    check(in.touch_screen().count == 0, "no finger before the press");
    now = 1.1;
    in.poll();
    check(in.touch_screen().count == 1 && in.touch_screen().points[0].x == 640 &&
              in.touch_screen().points[0].y == 850.f / 960 * 720,
          "finger through the mapper");
    now = 1.5;
    in.poll();
    check(in.touch_screen().count == 0, "lifted after the hold");
    now = 2.1;
    in.poll();
    check(in.touch_screen().count == 1 && in.touch_screen().points[0].x == 100, "second touch");
}

}  // namespace

int main() {
    test_parse();
    test_frames();
    test_seconds();
    test_events();
    test_stable();
    test_image_watch();
    test_sticks_and_idle();
    test_scripted_input();
    test_scripted_throw();
    test_player2();
    test_touch();
    if (g_failures != 0) {
        std::fprintf(stderr, "%d checks failed\n", g_failures);
        return 1;
    }
    std::printf("all checks passed\n");
    return 0;
}
