// P5.1: host pads -> Switch controllers (platform/common/pad_input). Runs
// natively and in Node, without devices.
#include <array>
#include <cmath>
#include <cstdio>
#include <vector>

#include "../runtime/check.hpp"
#include "lgp/platform/pad_input.hpp"

using namespace lgp::platform;

namespace {

constexpr uint64_t kMs = 1'000'000;

HostPad connected() {
    HostPad p;
    p.connected = true;
    return p;
}

void test_keyboard_defaults() {
    const KeyboardMap keys = KeyboardMap::defaults();
    std::array<bool, 256> down{};
    down[hid_usage::letter('W')] = true;
    down[hid_usage::letter('D')] = true;
    down[hid_usage::letter('Z')] = true;
    down[hid_usage::kUp] = true;
    down[hid_usage::kEnter] = true;
    down[hid_usage::letter('L')] = true;
    HostPad p;
    keys.apply(down.data(), &p);
    CHECK(p.buttons == (kButtonA | kButtonUp | kButtonPlus));
    CHECK(p.left_x == 1.0f && p.left_y == 1.0f);
    CHECK(p.right_x == 1.0f && p.right_y == 0.0f);
    CHECK(!p.throw_gesture);
    CHECK(!p.connected);  // the backend decides

    // Opposite keys cancel; idle keys leave a gamepad stick alone.
    std::array<bool, 256> lr{};
    lr[hid_usage::letter('A')] = true;
    lr[hid_usage::letter('D')] = true;
    lr[hid_usage::kSpace] = true;
    HostPad q;
    q.left_x = 0.5f;
    q.right_y = -0.25f;
    keys.apply(lr.data(), &q);
    CHECK(q.left_x == 0.5f);
    CHECK(q.right_y == -0.25f);
    CHECK(q.throw_gesture);

    KeyboardMap custom;
    custom.bind({hid_usage::kSpace, KeyTarget::Button, kButtonB});
    HostPad r;
    custom.apply(lr.data(), &r);
    CHECK(r.buttons == kButtonB && !r.throw_gesture);
}

void test_dom_codes() {
    CHECK(hid_usage_from_dom_code("KeyA") == hid_usage::letter('A'));
    CHECK(hid_usage_from_dom_code("KeyZ") == hid_usage::letter('Z'));
    CHECK(hid_usage_from_dom_code("Digit1") == hid_usage::k1);
    CHECK(hid_usage_from_dom_code("Digit0") == 0x27);
    CHECK(hid_usage_from_dom_code("Numpad1") == 0x59);
    CHECK(hid_usage_from_dom_code("Numpad0") == 0x62);
    CHECK(hid_usage_from_dom_code("ArrowUp") == hid_usage::kUp);
    CHECK(hid_usage_from_dom_code("Space") == hid_usage::kSpace);
    CHECK(hid_usage_from_dom_code("NumpadAdd") == hid_usage::kKeypadPlus);
    CHECK(hid_usage_from_dom_code("Keya") == 0);
    CHECK(hid_usage_from_dom_code("F13") == 0);
    CHECK(hid_usage_from_dom_code("") == 0);
    CHECK(hid_usage_from_dom_code(nullptr) == 0);
}

void test_handheld() {
    ControllerMapper m;  // handheld
    CHECK(m.map(HostPad{}, 0).style == ControllerStyle::None);

    HostPad p = connected();
    p.buttons = kButtonA | kButtonLeft | kButtonRightSL;  // SL/SR do not exist on a handheld
    p.left_x = 1.0f;
    p.left_y = 0.1f;   // inside the dead zone on its own, not with x
    p.right_x = 0.1f;  // dead zone
    p.zl = 0.6f;
    p.zr = 0.4f;
    const ControllerState s = m.map(p, 0);
    CHECK(s.style == ControllerStyle::Handheld);
    CHECK(s.buttons == (kButtonA | kButtonLeft | kButtonZL));
    CHECK(s.left.x > 32000 && s.left.y > 0 && s.left.y < 4000);
    CHECK(s.right.x == 0 && s.right.y == 0);
    CHECK(!s.motion.valid);  // no sensor, nothing invented for handheld

    // Full diagonal deflection is clamped to the unit circle.
    p.left_x = 1.0f;
    p.left_y = 1.0f;
    const ControllerState d = m.map(p, 0);
    const double len = std::sqrt(double(d.left.x) * d.left.x + double(d.left.y) * d.left.y);
    CHECK(len <= 32768.0 && len > 32000.0);

    // Real motion data passes through.
    p.motion.valid = true;
    p.motion.gyro[1] = 0.5f;
    const ControllerState mo = m.map(p, 0);
    CHECK(mo.motion.valid && mo.motion.gyro[1] == 0.5f);
}

void test_joycon_right() {
    MapperConfig c;
    c.style = ControllerStyle::JoyconRight;
    ControllerMapper m(c);
    HostPad p = connected();
    p.buttons = kButtonA | kButtonY | kButtonL | kButtonMinus | kButtonUp | kButtonStickL;
    p.zr = 1.0f;
    p.left_y = 1.0f;
    const ControllerState s = m.map(p, 0);
    CHECK(s.style == ControllerStyle::JoyconRight);
    CHECK(s.buttons == (kButtonA | kButtonY | kButtonRightSL | kButtonRightSR | kButtonPlus | kButtonStickR));
    CHECK(s.right.y == 32767 && s.left.y == 0);  // the Joy-Con's stick is its right stick
    CHECK(s.motion.valid);                       // synthesized resting pose
    CHECK(s.motion.accel[2] == -1.0f && s.motion.gyro[0] == 0.0f);
    // Sticks idle: the host d-pad steers (a right Joy-Con has none).
    p.left_y = 0.0f;
    p.zr = 0.0f;
    p.buttons = kButtonUp | kButtonRight;
    const ControllerState d = m.map(p, 0);
    CHECK(d.right.x == 23170 && d.right.y == 23170 && d.buttons == 0);
}

void test_joycon_left() {
    MapperConfig c;
    c.style = ControllerStyle::JoyconLeft;
    c.synthesize_motion = false;
    ControllerMapper m(c);
    HostPad p = connected();
    p.buttons = kButtonA | kButtonX | kButtonPlus | kButtonR;
    p.right_x = -1.0f;  // second stick used when the first is idle
    const ControllerState s = m.map(p, 0);
    CHECK(s.buttons == (kButtonRight | kButtonUp | kButtonMinus | kButtonLeftSR));
    CHECK(s.left.x == -32767);
    CHECK(!s.motion.valid);
}

// A stick at its stop that wobbles a little below 1.0 (Pro Controller via
// SDL) still reads as fully deflected; in between the range is rescaled.
void test_stick_outer() {
    ControllerMapper m;
    HostPad p = connected();
    for (float v : {0.9f, 0.93f, 0.97f, 1.0f}) {
        p.left_x = v;
        p.left_y = 0.0f;
        CHECK(m.map(p, 0).left.x == 32767);
    }
    p.left_x = 0.525f;  // halfway between dead zone (0.15) and outer (0.9)
    const int32_t half = m.map(p, 0).left.x;
    CHECK(half > 16000 && half < 16800);
    p.left_x = 0.1f;
    CHECK(m.map(p, 0).left.x == 0);

    MapperConfig c;
    CHECK(parse_stick_range("0.1,0.8", c) && c.stick_deadzone == 0.1f && c.stick_outer == 0.8f);
    CHECK(!parse_stick_range("0.5,0.4", c));
    CHECK(!parse_stick_range("0.1,1.2", c));
    CHECK(!parse_stick_range("0.1", c));
    CHECK(!parse_stick_range("a,0.9", c));
    CHECK(!parse_stick_range("nan,0.9", c));
    CHECK(c.stick_deadzone == 0.1f && c.stick_outer == 0.8f);  // a bad spec changes nothing
    ControllerMapper m2(c);
    p.left_x = 0.8f;
    CHECK(m2.map(p, 0).left.x == 32767);
    CHECK(c.stick_hold == 0.0f);
    CHECK(parse_stick_range("0.15,0.9,0.05", c) && c.stick_hold == 0.05f);
    CHECK(!parse_stick_range("0.15,0.9,1.0", c));
    CHECK(!parse_stick_range("0.15,0.9,0.05,1", c));
    CHECK(!parse_stick_range("0.15,0.9,", c));
}

void test_stick_hold() {
    MapperConfig c;
    c.stick_hold = 0.05f;  // about 1638
    ControllerMapper m(c);
    HostPad p = connected();
    p.left_x = 0.04f;
    p.left_y = 1.0f;
    const StickState first = m.map(p, 0).left;
    p.left_x = 0.07f;  // jitter: the reported stick stays
    CHECK(m.map(p, 0).left.x == first.x && m.map(p, 0).left.y == first.y);
    p.left_x = 0.5f;  // a real move goes through
    CHECK(m.map(p, 0).left.x > first.x + 10000);
    p.left_x = p.left_y = 0.0f;  // release always centers
    CHECK(m.map(p, 0).left.x == 0 && m.map(p, 0).left.y == 0);
    ControllerMapper off;  // default: every change goes through
    p.left_x = 0.04f;
    p.left_y = 1.0f;
    const int32_t a = off.map(p, 0).left.x;
    p.left_x = 0.07f;
    CHECK(off.map(p, 0).left.x != a);
}

void test_motion_frame() {
    MapperConfig c;
    c.style = ControllerStyle::JoyconRight;
    CHECK(parse_motion_frame("-y,x,z", c));
    CHECK(c.motion_axis[0] == 1 && c.motion_axis[1] == 0 && c.motion_axis[2] == 2);
    CHECK(c.motion_sign[0] == -1.0f && c.motion_sign[1] == 1.0f && c.motion_sign[2] == 1.0f);
    CHECK(!parse_motion_frame("x,x,z", c));    // each axis once
    CHECK(!parse_motion_frame("x,y", c));      // three items
    CHECK(!parse_motion_frame("x,y,z,x", c));
    CHECK(!parse_motion_frame("x,y,w", c));
    CHECK(c.motion_axis[0] == 1);  // a bad spec changes nothing
    ControllerMapper m(c);
    HostPad p = connected();
    p.motion.valid = true;
    p.motion.accel[0] = 0.25f;
    p.motion.accel[1] = 0.5f;
    p.motion.gyro[2] = 2.0f;
    const ControllerState s = m.map(p, 1000 * kMs);
    CHECK(s.motion.valid);
    CHECK(s.motion.accel[0] == -0.5f && s.motion.accel[1] == 0.25f && s.motion.accel[2] == 0.0f);
    CHECK(s.motion.gyro[2] == 2.0f);
    CHECK(parse_motion_frame("+x,y,z", c) && c.motion_axis[0] == 0 && c.motion_sign[0] == 1.0f);
    c.gyro_gain = 3.0f;  // gyro scales, gravity stays
    ControllerMapper g(c);
    const ControllerState sg = g.map(p, 1000 * kMs);
    CHECK(sg.motion.gyro[2] == 6.0f && sg.motion.accel[1] == 0.5f);
}

void test_throw_spec() {
    MapperConfig c;
    c.style = ControllerStyle::JoyconRight;
    CHECK(parse_throw_spec("ms=500,g=8,rps=2,accel=z,gyro=y,rest=-y", c));
    CHECK(c.throw_duration_ms == 500 && c.throw_peak_accel_g == 8.0f && c.throw_peak_gyro_rps == 2.0f);
    CHECK(c.throw_accel_axis == 2 && c.throw_gyro_axis == 1);
    CHECK(c.rest_gravity_axis == 1 && c.rest_gravity_sign == -1.0f);
    CHECK(!parse_throw_spec("g=fast", c));
    CHECK(!parse_throw_spec("accel=w", c));
    CHECK(!parse_throw_spec("ms=1e30", c));  // beyond uint32_t
    CHECK(!parse_throw_spec("g=nan", c));
    CHECK(!parse_throw_spec("rps=inf", c));
    CHECK(c.throw_peak_accel_g == 8.0f);  // a bad spec changes nothing
    ControllerMapper m(c);
    HostPad p = connected();
    ControllerState s = m.map(p, 1000 * kMs);
    CHECK(s.motion.accel[1] == -1.0f && s.motion.accel[2] == 0.0f);  // resting upright
    p.throw_gesture = true;
    m.map(p, 1000 * kMs);
    s = m.map(p, 1125 * kMs);  // a quarter in: swing near its peak
    CHECK(s.motion.accel[2] > 7.0f && s.motion.gyro[1] > 1.0f);
}

void test_throw_gesture() {
    MapperConfig c;
    c.style = ControllerStyle::JoyconRight;
    c.throw_duration_ms = 300;
    ControllerMapper m(c);
    HostPad p = connected();
    p.throw_gesture = true;
    const uint64_t t0 = 1000 * kMs;
    ControllerState s = m.map(p, t0);
    CHECK(m.throwing());
    float peak_accel = 0, peak_gyro = 0;
    for (uint64_t t = t0; t < t0 + 300 * kMs; t += 5 * kMs) {
        s = m.map(p, t);  // holding the button does not restart the gesture
        peak_accel = std::fmax(peak_accel, std::fabs(s.motion.accel[1]));
        peak_gyro = std::fmax(peak_gyro, s.motion.gyro[0]);
    }
    CHECK(peak_accel > 5.5f && peak_accel <= 6.0f);
    CHECK(peak_gyro > 4.9f && peak_gyro <= 5.0f);
    s = m.map(p, t0 + 301 * kMs);
    CHECK(!m.throwing());
    CHECK(s.motion.accel[1] == 0.0f && s.motion.accel[2] == -1.0f);
    s = m.map(p, t0 + 400 * kMs);  // still held: no second throw
    CHECK(!m.throwing());
    p.throw_gesture = false;
    m.map(p, t0 + 410 * kMs);
    p.throw_gesture = true;
    m.map(p, t0 + 420 * kMs);  // pressed again: next throw
    CHECK(m.throwing());

    // Handheld ignores the gesture (it throws with a button).
    ControllerMapper h;
    const ControllerState hs = h.map(p, t0);
    CHECK(!h.throwing() && !hs.motion.valid);
}

void test_throw_swipe() {
    // Defaults from the PC run in LGP: the side only turns about z, the
    // swipe's tempo is not used.
    {
        MapperConfig d;
        d.style = ControllerStyle::JoyconRight;
        ControllerMapper dm(d);
        HostPad dp = connected();
        dp.throw_gesture = true;
        dp.throw_side = 1;
        dp.throw_strength = 2;
        dm.map(dp, 1000 * kMs);
        const ControllerState ds = dm.map(dp, 1100 * kMs);
        // Right: a turn about y with gain 1 (5 rev/s at the peak, here sin 45°).
        CHECK(ds.motion.accel[0] == 0.0f && ds.motion.gyro[2] == 0.0f);
        CHECK(std::fabs(ds.motion.gyro[1] - 5.0f * 0.7071f) < 0.01f);
        CHECK(std::fabs(ds.motion.accel[1] - 6.0f) < 0.01f);  // strength 1
        // Left: the same turn the other way.
        ControllerMapper lm(d);
        dp.throw_side = -1;
        lm.map(dp, 1000 * kMs);
        const ControllerState ls = lm.map(dp, 1100 * kMs);
        CHECK(ls.motion.gyro[2] == 0.0f && std::fabs(ls.motion.gyro[1] + 5.0f * 0.7071f) < 0.01f);
    }
    // Side and strength of a swipe (TouchPad) shape the single Joy-Con's swing
    // (with the sideways acceleration and the tempo switched on).
    MapperConfig c;
    c.style = ControllerStyle::JoyconRight;
    CHECK(parse_throw_spec("side=x,yaw=z,sidegain=0.5,tempo=2", c));
    CHECK(c.throw_side_left_gyro_axis == 2 && c.throw_side_left_gain == 0.5f);  // yaw/sidegain: both sides
    CHECK(c.throw_side_accel_axis == 0 && c.throw_strength_max == 2.0f);
    CHECK(!parse_throw_spec("tempo=3", c));
    {
        MapperConfig l;
        CHECK(parse_throw_spec("yaw=y,leftyaw=-x,leftgain=0.25", l));
        CHECK(l.throw_side_gyro_axis == 1 && l.throw_side_left_gyro_axis == 0 &&
              l.throw_side_left_gyro_sign == -1.0f && l.throw_side_left_gain == 0.25f && l.throw_side_gain == 1.0f);
        CHECK(parse_throw_spec("yaw=none", l) && l.throw_side_left_gyro_axis == -1);
    }
    const uint64_t t0 = 1000 * kMs, quarter = 100 * kMs;  // 400 ms gesture
    const auto at_quarter = [&](float side, float strength) {
        ControllerMapper m(c);
        HostPad p = connected();
        p.throw_gesture = true;
        p.throw_side = side;
        p.throw_strength = strength;
        m.map(p, t0);
        p.throw_side = 0;  // read only when the gesture starts
        p.throw_strength = 1;
        return m.map(p, t0 + quarter);
    };
    const ControllerState straight = at_quarter(0, 1);
    CHECK(std::fabs(straight.motion.accel[1] - 6.0f) < 0.01f && std::fabs(straight.motion.gyro[0] - 5.0f * 0.7071f) < 0.01f);
    CHECK(straight.motion.accel[0] == 0.0f && straight.motion.gyro[2] == 0.0f);
    const ControllerState strong = at_quarter(0, 2);
    CHECK(std::fabs(strong.motion.accel[1] - 12.0f) < 0.02f);
    const ControllerState right = at_quarter(1, 1);  // full side: half the peaks sideways
    CHECK(std::fabs(right.motion.accel[0] - 3.0f) < 0.01f && right.motion.gyro[2] > 1.7f);
    CHECK(std::fabs(right.motion.accel[1] - 6.0f) < 0.01f);  // the forward swing stays
    const ControllerState left = at_quarter(-0.5f, 1);
    CHECK(std::fabs(left.motion.accel[0] + 1.5f) < 0.01f && left.motion.gyro[2] < 0.0f);
    const ControllerState clamped = at_quarter(5, 9);  // out of range: clamped
    CHECK(std::fabs(clamped.motion.accel[1] - 12.0f) < 0.02f && std::fabs(clamped.motion.accel[0] - 6.0f) < 0.02f);

    // Axes and signs from the spec; none switches a part off.
    CHECK(parse_throw_spec("side=-z,yaw=none,sidegain=1", c));
    CHECK(c.throw_side_accel_axis == 2 && c.throw_side_accel_sign == -1.0f && c.throw_side_gyro_axis == -1);
    CHECK(c.throw_side_gain == 1.0f);
    CHECK(!parse_throw_spec("side=all", c));
    CHECK(!parse_throw_spec("sidegain=-1", c));
    CHECK(!parse_throw_spec("dir=2", c) && !parse_throw_spec("strength=0.5", c));
    const ControllerState spec = at_quarter(1, 1);
    CHECK(std::fabs(spec.motion.accel[2] - (-1.0f + 3.0f * 0.7071f - 6.0f)) < 0.02f);  // rest + lift - side
    CHECK(spec.motion.gyro[2] == 0.0f && spec.motion.accel[0] == 0.0f);

    // dir/strength force every throw (tests with the Space key).
    {
        MapperConfig f = c;
        CHECK(parse_throw_spec("side=x,yaw=z,sidegain=0.5,dir=-1,strength=2", f));
        ControllerMapper fm(f);
        HostPad fp = connected();
        fp.throw_gesture = true;  // side 0, strength 1 from the pad
        fm.map(fp, t0);
        const ControllerState fs = fm.map(fp, t0 + quarter);
        CHECK(std::fabs(fs.motion.accel[0] + 6.0f) < 0.02f && std::fabs(fs.motion.accel[1] - 12.0f) < 0.02f);
    }

    // sideaccel/leftaccel: the sideways acceleration's own signed gain, not
    // mirrored to the left; the turn keeps sidegain.
    {
        MapperConfig a;
        a.style = ControllerStyle::JoyconRight;
        CHECK(parse_throw_spec("side=x,yaw=z,sideaccel=0.5", a));
        CHECK(a.throw_side_accel_gain == 0.5f && a.throw_side_left_accel_gain == 0.5f && a.throw_side_gain == 1.0f);
        CHECK(!parse_throw_spec("sideaccel=11", a));
        const auto side_at = [&](const MapperConfig& cfg, float side) {
            ControllerMapper m(cfg);
            HostPad p = connected();
            p.throw_gesture = true;
            p.throw_side = side;
            m.map(p, t0);
            p.throw_side = 0;
            return m.map(p, t0 + quarter);
        };
        const ControllerState ar = side_at(a, 1), al = side_at(a, -1);
        CHECK(std::fabs(ar.motion.accel[0] - 3.0f) < 0.01f && ar.motion.gyro[2] > 3.5f);
        CHECK(std::fabs(al.motion.accel[0] - 3.0f) < 0.01f && al.motion.gyro[2] < -3.5f);  // not mirrored
        CHECK(parse_throw_spec("leftaccel=-1", a) && a.throw_side_accel_gain == 0.5f);
        CHECK(std::fabs(side_at(a, -1).motion.accel[0] + 6.0f) < 0.02f);
        CHECK(std::fabs(side_at(a, -0.5f).motion.accel[0] + 3.0f) < 0.02f);  // scales with the side
        // Without them the acceleration follows sidegain on both sides,
        // mirrored to the left; leftgain is the turn's alone.
        MapperConfig b;
        b.style = ControllerStyle::JoyconRight;
        CHECK(parse_throw_spec("side=x,yaw=z,sidegain=0.5,leftgain=1", b));
        const ControllerState bl = side_at(b, -1);
        CHECK(std::fabs(bl.motion.accel[0] + 3.0f) < 0.01f && bl.motion.gyro[2] < -3.5f);
    }

    // Reach: the swing lasts 300 ms at -1, 400 at 0, 550 at 1.
    {
        MapperConfig d;
        d.style = ControllerStyle::JoyconRight;
        const auto running_at = [&](float reach, uint64_t after_ms) {
            ControllerMapper m(d);
            HostPad p = connected();
            p.throw_gesture = true;
            p.throw_reach = reach;
            m.map(p, t0);
            m.map(p, t0 + after_ms * kMs);
            return m.throwing();
        };
        CHECK(running_at(-1, 290) && !running_at(-1, 310));
        CHECK(running_at(0, 390) && !running_at(0, 410));
        CHECK(running_at(1, 540) && !running_at(1, 560));
        CHECK(running_at(-0.5f, 340) && !running_at(-0.5f, 360));  // 350 ms
        MapperConfig f = d;
        CHECK(parse_throw_spec("reach=1,long=600,short=250", f));
        CHECK(f.throw_force_reach == 1.0f && f.throw_reach_long_ms == 600 && f.throw_reach_short_ms == 250);
        CHECK(!parse_throw_spec("reach=2", f));
    }

    // Real sensors on the slot (a phone's gyro): the swing adds to them.
    MapperConfig r;
    r.style = ControllerStyle::JoyconRight;
    ControllerMapper rm(r);
    HostPad rp = connected();
    rp.motion.valid = true;
    rp.motion.accel[1] = -1.0f;  // held upright
    rp.motion.gyro[2] = 0.25f;
    ControllerState rs = rm.map(rp, t0);
    CHECK(rs.motion.accel[1] == -1.0f && rs.motion.gyro[2] == 0.25f && !rm.throwing());
    rp.throw_gesture = true;
    rm.map(rp, t0);
    CHECK(rm.throwing());
    rs = rm.map(rp, t0 + quarter);
    CHECK(std::fabs(rs.motion.accel[1] - (-1.0f + 6.0f)) < 0.01f);  // sensor + swing
    CHECK(std::fabs(rs.motion.accel[2] - 3.0f * 0.7071f) < 0.01f);    // the lift, without a second gravity
    CHECK(std::fabs(rs.motion.gyro[0] - 5.0f * 0.7071f) < 0.01f && rs.motion.gyro[2] == 0.25f);
    rs = rm.map(rp, t0 + 401 * kMs);
    CHECK(!rm.throwing() && rs.motion.accel[1] == -1.0f && rs.motion.accel[2] == 0.0f);
    // sensors=replace: during the swing only the swing, with its own resting
    // gravity; afterwards the sensors again.
    CHECK(parse_throw_spec("sensors=replace", r) && r.throw_replace_sensors);
    CHECK(!parse_throw_spec("sensors=mix", r));
    ControllerMapper xm(r);
    rp.throw_gesture = false;
    xm.map(rp, t0);
    rp.throw_gesture = true;
    xm.map(rp, t0);
    rs = xm.map(rp, t0 + quarter);
    CHECK(std::fabs(rs.motion.accel[1] - 6.0f) < 0.01f && rs.motion.gyro[2] == 0.0f);
    CHECK(std::fabs(rs.motion.accel[2] - (-1.0f + 3.0f * 0.7071f)) < 0.01f);
    rs = xm.map(rp, t0 + 401 * kMs);
    CHECK(!xm.throwing() && rs.motion.accel[1] == -1.0f && rs.motion.gyro[2] == 0.25f);
}

// PlayerPick: the first new press after start or repick() chooses player 1.
void test_player_pick() {
    using Order = std::vector<size_t>;
    PlayerPick pick;
    CHECK(pick.picking() && pick.player1() == 0);
    CHECK((pick.update({7, 9}, {0, 0}) == Order{0, 1}));
    // A button held when a pad shows up is no press.
    CHECK((pick.update({7, 9, 4}, {0, 0, kButtonA}) == Order{0, 1, 2}));
    CHECK(pick.picking());
    // The second pad presses: it moves to slot 0, the others keep their order.
    CHECK((pick.update({7, 9, 4}, {0, kButtonB, kButtonA}) == Order{1, 0, 2}));
    CHECK(!pick.picking() && pick.player1() == 9);
    // Later presses change nothing.
    CHECK((pick.update({7, 9, 4}, {kButtonA, 0, 0}) == Order{1, 0, 2}));
    // repick(): the slots stay until the next press; held buttons do not count.
    pick.repick();
    CHECK((pick.update({7, 9, 4}, {kButtonA, 0, 0}) == Order{1, 0, 2}));
    CHECK((pick.update({7, 9, 4}, {kButtonA, 0, kButtonX}) == Order{2, 0, 1}));
    CHECK(pick.player1() == 4);
    // Two pads pressing in the same sample: the first in backend order wins.
    pick.repick();
    CHECK((pick.update({7, 9, 4}, {kButtonA | kButtonY, kButtonY, kButtonX}) == Order{0, 1, 2}));
    CHECK(pick.player1() == 7);
    // Player 1 goes away: backend order, the next press chooses.
    CHECK((pick.update({9, 4}, {0, 0}) == Order{0, 1}));
    CHECK(pick.picking() && pick.player1() == 0);
    CHECK((pick.update({9, 4}, {0, kButtonPlus}) == Order{1, 0}));
    // Player 1 goes away in the same sample as another pad's press: that press chooses.
    CHECK((pick.update({9, 4, 6}, {0, 0, 0}) == Order{1, 0, 2}));
    CHECK((pick.update({9, 6}, {0, kButtonA}) == Order{1, 0}));
    CHECK(pick.player1() == 6);
    // Order mode: never reorders.
    pick.set_mode(PlayerPick::Mode::Order);
    CHECK((pick.update({9, 4}, {0, 0}) == Order{0, 1}));
    pick.repick();
    CHECK((pick.update({9, 4}, {0, kButtonA}) == Order{0, 1}));
    PlayerPick order(PlayerPick::Mode::Order);
    CHECK(!order.picking());
    CHECK((order.update({3, 5}, {0, 0}) == Order{0, 1}));
    CHECK((order.update({3, 5}, {0, kButtonA}) == Order{0, 1}));
}

class FakeBackend final : public PadInput {
public:
    FakeBackend() : PadInput(default_slots()) {}
    std::vector<HostPad> next;
    float low = -1, high = -1;
    size_t rumble_slot = 99;
    uint64_t clock = 0;

protected:
    uint64_t now_ns() const override { return clock; }
    void sample(std::vector<HostPad>& pads) override {
        CHECK(pads.size() == 2);
        for (size_t i = 0; i < pads.size() && i < next.size(); ++i)
            pads[i] = next[i];
    }
    void rumble(size_t slot, float l, float h) override {
        rumble_slot = slot;
        low = l;
        high = h;
    }
};

void test_pad_input() {
    FakeBackend in;
    CHECK(in.controller_count() == 2);
    in.poll();
    CHECK(in.controller(0).style == ControllerStyle::None);
    CHECK(in.controller(5).style == ControllerStyle::None);

    HostPad a = connected();
    a.buttons = kButtonB;
    HostPad b = connected();
    b.buttons = kButtonX;
    in.next = {a, b};
    CHECK(in.controller(0).style == ControllerStyle::None);  // until the next poll
    in.poll();
    CHECK(in.controller(0).style == ControllerStyle::Handheld && in.controller(0).buttons == kButtonB);
    CHECK(in.controller(1).style == ControllerStyle::JoyconRight && in.controller(1).buttons == kButtonX);

    in.set_vibration(1, 2.0f, -1.0f);
    CHECK(in.rumble_slot == 1 && in.low == 1.0f && in.high == 0.0f);
    in.rumble_slot = 99;
    in.set_vibration(7, 0.5f, 0.5f);
    CHECK(in.rumble_slot == 99);

    // The app's controller setting while the game runs: the next poll maps
    // slot 0 with the new style.
    MapperConfig joycon = default_slots()[0];
    joycon.style = ControllerStyle::JoyconLeft;
    in.set_slot(0, joycon);
    CHECK(in.slot(0).style == ControllerStyle::JoyconLeft);
    CHECK(in.controller(0).style == ControllerStyle::Handheld);  // until the next poll
    in.poll();
    CHECK(in.controller(0).style == ControllerStyle::JoyconLeft);
    in.set_slot(9, joycon);  // no such slot: nothing happens
    CHECK(in.controller_count() == 2);
}

}  // namespace

int main() {
    RUN(test_keyboard_defaults);
    RUN(test_dom_codes);
    RUN(test_handheld);
    RUN(test_joycon_right);
    RUN(test_joycon_left);
    RUN(test_throw_gesture);
    RUN(test_throw_spec);
    RUN(test_throw_swipe);
    RUN(test_motion_frame);
    RUN(test_stick_outer);
    RUN(test_stick_hold);
    RUN(test_pad_input);
    RUN(test_player_pick);
    return finish();
}
