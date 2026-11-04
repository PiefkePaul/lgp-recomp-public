// Host controllers -> Switch controllers, see lgp/platform/pad_input.hpp.
#include "lgp/platform/pad_input.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <string_view>

namespace lgp::platform {

namespace {

constexpr float kPi = 3.14159265358979f;

// Radial dead zone and outer saturation, rescaled in between so the stick
// reaches the full range at `outer`.
StickState to_stick(float x, float y, float deadzone, float outer) {
    const float len = std::sqrt(x * x + y * y);
    if (len <= deadzone || len == 0.0f)
        return {};
    const float scaled = std::min(1.0f, (len - deadzone) / (outer - deadzone));
    const float k = scaled / len * 32767.0f;
    return {static_cast<int32_t>(std::lround(std::clamp(x * k, -32767.0f, 32767.0f))),
            static_cast<int32_t>(std::lround(std::clamp(y * k, -32767.0f, 32767.0f)))};
}

constexpr uint64_t kFaceButtons = kButtonA | kButtonB | kButtonX | kButtonY;
constexpr uint64_t kDpad = kButtonLeft | kButtonUp | kButtonRight | kButtonDown;

// D-pad as a full stick deflection (diagonals on the unit circle).
StickState dpad_stick(uint64_t buttons) {
    const int x = ((buttons & kButtonRight) ? 1 : 0) - ((buttons & kButtonLeft) ? 1 : 0);
    const int y = ((buttons & kButtonUp) ? 1 : 0) - ((buttons & kButtonDown) ? 1 : 0);
    const float k = x != 0 && y != 0 ? 32767.0f / std::sqrt(2.0f) : 32767.0f;
    return {static_cast<int32_t>(std::lround(x * k)), static_cast<int32_t>(std::lround(y * k))};
}

constexpr uint64_t bit_if(uint64_t test, uint64_t bit) { return test != 0 ? bit : 0; }

}  // namespace

uint8_t hid_usage_from_dom_code(const char* code) {
    using namespace hid_usage;
    const std::string_view c = code != nullptr ? code : "";
    if (c.size() == 4 && c.substr(0, 3) == "Key" && c[3] >= 'A' && c[3] <= 'Z')
        return letter(c[3]);
    if (c.size() == 6 && c.substr(0, 5) == "Digit" && c[5] >= '0' && c[5] <= '9')
        return c[5] == '0' ? 0x27 : static_cast<uint8_t>(k1 + (c[5] - '1'));
    if (c.size() == 7 && c.substr(0, 6) == "Numpad" && c[6] >= '0' && c[6] <= '9')
        return c[6] == '0' ? 0x62 : static_cast<uint8_t>(0x59 + (c[6] - '1'));
    static constexpr struct {
        std::string_view name;
        uint8_t usage;
    } kNamed[] = {
        {"Enter", kEnter}, {"Escape", kEscape}, {"Backspace", kBackspace}, {"Tab", kTab}, {"Space", kSpace},
        {"Minus", kMinus}, {"Equal", kEqual}, {"BracketLeft", 0x2F}, {"BracketRight", 0x30},
        {"Backslash", 0x31}, {"Semicolon", 0x33}, {"Quote", 0x34}, {"Backquote", 0x35}, {"Comma", 0x36},
        {"Period", 0x37}, {"Slash", 0x38}, {"CapsLock", 0x39}, {"ArrowRight", kRight}, {"ArrowLeft", kLeft},
        {"ArrowDown", kDown}, {"ArrowUp", kUp}, {"NumpadDivide", 0x54}, {"NumpadMultiply", 0x55},
        {"NumpadSubtract", kKeypadMinus}, {"NumpadAdd", kKeypadPlus}, {"NumpadEnter", 0x58},
        {"NumpadDecimal", 0x63}, {"IntlBackslash", 0x64}, {"ControlLeft", 0xE0}, {"ShiftLeft", kLeftShift},
        {"AltLeft", 0xE2}, {"ControlRight", 0xE4}, {"ShiftRight", kRightShift}, {"AltRight", 0xE6},
    };
    for (const auto& n : kNamed)
        if (n.name == c)
            return n.usage;
    return 0;
}

KeyboardMap KeyboardMap::defaults() {
    using namespace hid_usage;
    KeyboardMap m;
    const auto button = [&m](uint8_t usage, uint64_t b) { m.bind({usage, KeyTarget::Button, b}); };
    const auto target = [&m](uint8_t usage, KeyTarget t) { m.bind({usage, t, 0}); };
    target(letter('W'), KeyTarget::LeftUp);
    target(letter('S'), KeyTarget::LeftDown);
    target(letter('A'), KeyTarget::LeftLeft);
    target(letter('D'), KeyTarget::LeftRight);
    target(letter('I'), KeyTarget::RightUp);
    target(letter('K'), KeyTarget::RightDown);
    target(letter('J'), KeyTarget::RightLeft);
    target(letter('L'), KeyTarget::RightRight);
    button(kUp, kButtonUp);
    button(kDown, kButtonDown);
    button(kLeft, kButtonLeft);
    button(kRight, kButtonRight);
    button(letter('Z'), kButtonA);
    button(letter('X'), kButtonB);
    button(letter('C'), kButtonX);
    button(letter('V'), kButtonY);
    button(letter('Q'), kButtonZL);
    button(letter('E'), kButtonL);
    button(letter('U'), kButtonR);
    button(letter('O'), kButtonZR);
    button(letter('F'), kButtonStickL);
    button(letter('H'), kButtonStickR);
    button(kMinus, kButtonMinus);
    button(kKeypadMinus, kButtonMinus);
    button(kEqual, kButtonPlus);
    button(kKeypadPlus, kButtonPlus);
    button(kEnter, kButtonPlus);
    target(kSpace, KeyTarget::Throw);
    return m;
}

void KeyboardMap::apply(const bool* down, HostPad* pad) const {
    float lx = 0, ly = 0, rx = 0, ry = 0;
    for (const KeyBinding& b : bindings_) {
        if (!down[b.usage])
            continue;
        switch (b.target) {
        case KeyTarget::Button: pad->buttons |= b.button; break;
        case KeyTarget::LeftUp: ly += 1; break;
        case KeyTarget::LeftDown: ly -= 1; break;
        case KeyTarget::LeftLeft: lx -= 1; break;
        case KeyTarget::LeftRight: lx += 1; break;
        case KeyTarget::RightUp: ry += 1; break;
        case KeyTarget::RightDown: ry -= 1; break;
        case KeyTarget::RightLeft: rx -= 1; break;
        case KeyTarget::RightRight: rx += 1; break;
        case KeyTarget::Throw: pad->throw_gesture = true; break;
        }
    }
    // Keys win over an idle gamepad stick; diagonals keep full deflection
    // per axis like a digital stick (the mapper clamps the length).
    const auto merge = [](float* axis, float keys) {
        if (keys != 0)
            *axis = std::clamp(keys, -1.0f, 1.0f);
    };
    merge(&pad->left_x, lx);
    merge(&pad->left_y, ly);
    merge(&pad->right_x, rx);
    merge(&pad->right_y, ry);
}

MotionState ControllerMapper::synthetic_motion(bool throw_started, const HostPad& pad, uint64_t now_ns) {
    if (throw_started && throw_start_ns_ == 0) {
        throw_start_ns_ = now_ns != 0 ? now_ns : 1;
        const float side = std::isfinite(config_.throw_force_side) ? config_.throw_force_side : pad.throw_side;
        const bool forced = std::isfinite(config_.throw_force_strength);
        const float strength = forced ? config_.throw_force_strength : pad.throw_strength;
        const float most = forced ? 2.0f : std::clamp(config_.throw_strength_max, 1.0f, 2.0f);
        throw_side_ = std::isfinite(side) ? std::clamp(side, -1.0f, 1.0f) : 0.0f;
        throw_strength_ = std::isfinite(strength) ? std::clamp(strength, 1.0f, most) : 1.0f;
        const float reach_in = std::isfinite(config_.throw_force_reach) ? config_.throw_force_reach : pad.throw_reach;
        const float reach = std::isfinite(reach_in) ? std::clamp(reach_in, -1.0f, 1.0f) : 0.0f;
        const float base = static_cast<float>(config_.throw_duration_ms);
        const float end = static_cast<float>(reach < 0 ? config_.throw_reach_short_ms : config_.throw_reach_long_ms);
        throw_duration_ns_ = static_cast<uint64_t>((base + std::fabs(reach) * (end - base)) * 1e6f);
    }
    MotionState m;
    m.valid = true;
    // Resting, lying face up: gravity along -z (assumption, see header).
    const int rest = std::clamp(config_.rest_gravity_axis, 0, 2);
    const int accel_axis = std::clamp(config_.throw_accel_axis, 0, 3);  // 3: all axes
    const int gyro_axis = std::clamp(config_.throw_gyro_axis, 0, 3);
    m.accel[rest] = config_.rest_gravity_sign;
    if (throw_start_ns_ != 0) {
        const uint64_t duration = throw_duration_ns_;
        const uint64_t t = now_ns - throw_start_ns_;
        if (now_ns < throw_start_ns_ || t >= duration || duration == 0) {
            throw_start_ns_ = 0;
        } else {
            // One sine period: the swing speeds up, then stops.
            const float phase = static_cast<float>(t) / static_cast<float>(duration);
            const float swing = std::sin(phase * 2.0f * kPi);
            const float pulse = std::sin(phase * kPi);
            const float peak_accel = config_.throw_peak_accel_g * throw_strength_;
            const float peak_gyro = config_.throw_peak_gyro_rps * throw_strength_;
            for (int a = 0; a < 3; ++a) {
                if (accel_axis == a || accel_axis == 3)
                    m.accel[a] += peak_accel * swing;
                if (gyro_axis == a || gyro_axis == 3)
                    m.gyro[a] = peak_gyro * pulse;
            }
            if (rest != accel_axis && accel_axis != 3)
                m.accel[rest] = config_.rest_gravity_sign + (peak_accel * 0.5f) * pulse;
            // Sideways: the same swing, scaled by the side; the turn has an
            // axis and gain of its own to the left.
            const bool left = throw_side_ < 0.0f;
            const float turn_gain = left ? config_.throw_side_left_gain : config_.throw_side_gain;
            const float accel_gain = left ? config_.throw_side_left_accel_gain : config_.throw_side_accel_gain;
            const float side = std::isnan(accel_gain) ? throw_side_ * config_.throw_side_gain
                                                      : std::abs(throw_side_) * accel_gain;
            const float turn = throw_side_ * turn_gain;
            const int sa = config_.throw_side_accel_axis;
            const int sg = left ? config_.throw_side_left_gyro_axis : config_.throw_side_gyro_axis;
            const float sg_sign = left ? config_.throw_side_left_gyro_sign : config_.throw_side_gyro_sign;
            if (side != 0.0f && sa >= 0 && sa < 3)
                m.accel[sa] += config_.throw_side_accel_sign * side * peak_accel * swing;
            if (turn != 0.0f && sg >= 0 && sg < 3)
                m.gyro[sg] += sg_sign * turn * peak_gyro * pulse;
        }
    }
    return m;
}

StickState ControllerMapper::hold(const StickState& in, StickState& last) const {
    const int32_t limit = static_cast<int32_t>(config_.stick_hold * 32767.0f);
    const bool centered = in.x == 0 && in.y == 0;
    if (limit > 0 && !centered && (last.x != 0 || last.y != 0) && std::abs(in.x - last.x) <= limit &&
        std::abs(in.y - last.y) <= limit)
        return last;
    last = in;
    return in;
}

ControllerState ControllerMapper::map(const HostPad& pad, uint64_t now_ns) {
    ControllerState s;
    if (!pad.connected) {
        throw_was_pressed_ = false;
        throw_start_ns_ = 0;
        last_left_ = last_right_ = {};
        return s;
    }
    // Holding the throw control throws once; letting go and pressing again
    // throws again.
    const bool throw_started = pad.throw_gesture && !throw_was_pressed_;
    throw_was_pressed_ = pad.throw_gesture;
    s.style = config_.style;
    s.device = pad.device;
    uint64_t b = pad.buttons;
    if (pad.zl >= config_.trigger_threshold)
        b |= kButtonZL;
    if (pad.zr >= config_.trigger_threshold)
        b |= kButtonZR;
    const StickState left =
        hold(to_stick(pad.left_x, pad.left_y, config_.stick_deadzone, config_.stick_outer), last_left_);
    const StickState right =
        hold(to_stick(pad.right_x, pad.right_y, config_.stick_deadzone, config_.stick_outer), last_right_);
    const bool single = config_.style == ControllerStyle::JoyconLeft || config_.style == ControllerStyle::JoyconRight;

    switch (config_.style) {
    case ControllerStyle::JoyconRight:
        // Sideways right Joy-Con: its stick is the right stick, A/B/X/Y and
        // Plus stay, shoulders become SL/SR. No rotation: the host delivers
        // "as the game should see it" (open point, horizontal hold).
        s.buttons = (b & (kFaceButtons | kButtonPlus)) | bit_if(b & kButtonMinus, kButtonPlus) |
                    bit_if(b & (kButtonStickL | kButtonStickR), kButtonStickR) |
                    bit_if(b & (kButtonL | kButtonZL), kButtonRightSL) |
                    bit_if(b & (kButtonR | kButtonZR), kButtonRightSR);
        // No d-pad on a right Joy-Con: the host d-pad steers like the stick.
        s.right = left.x != 0 || left.y != 0     ? left
                  : right.x != 0 || right.y != 0 ? right
                                                 : dpad_stick(b);
        break;
    case ControllerStyle::JoyconLeft:
        // Sideways left Joy-Con: its face buttons are the d-pad buttons; the
        // host's A/B/X/Y land on them by compass position.
        s.buttons = (b & (kDpad | kButtonMinus)) | bit_if(b & kButtonA, kButtonRight) |
                    bit_if(b & kButtonB, kButtonDown) | bit_if(b & kButtonX, kButtonUp) |
                    bit_if(b & kButtonY, kButtonLeft) | bit_if(b & kButtonPlus, kButtonMinus) |
                    bit_if(b & (kButtonStickL | kButtonStickR), kButtonStickL) |
                    bit_if(b & (kButtonL | kButtonZL), kButtonLeftSL) |
                    bit_if(b & (kButtonR | kButtonZR), kButtonLeftSR);
        s.left = left.x != 0 || left.y != 0 ? left : right;
        break;
    case ControllerStyle::None:
        return ControllerState{};
    default:  // Handheld, ProController, JoyconPair: everything as it is
        s.buttons = b & ~(kButtonLeftSL | kButtonLeftSR | kButtonRightSL | kButtonRightSR);
        s.left = left;
        s.right = right;
        break;
    }

    if (pad.motion.valid) {
        for (int a = 0; a < 3; ++a) {
            const int from = config_.motion_axis[a];
            s.motion.accel[a] = config_.motion_sign[a] * pad.motion.accel[from];
            s.motion.gyro[a] = config_.gyro_gain * config_.motion_sign[a] * pad.motion.gyro[from];
        }
        s.motion.valid = true;
        // A throw control on top of real sensors (a phone's own gyro on slot
        // 0, swiping on its touch screen): the swing adds to what they report.
        if (single && config_.synthesize_motion && (throw_started || throw_start_ns_ != 0) &&
            config_.throw_replace_sensors) {
            const MotionState swing = synthetic_motion(throw_started, pad, now_ns);
            if (throw_start_ns_ != 0)  // still swinging: the device's pose left out
                s.motion = swing;
        } else if (single && config_.synthesize_motion && (throw_started || throw_start_ns_ != 0)) {
            MotionState swing = synthetic_motion(throw_started, pad, now_ns);
            swing.accel[std::clamp(config_.rest_gravity_axis, 0, 2)] -= config_.rest_gravity_sign;  // the swing only
            for (int a = 0; a < 3; ++a) {
                s.motion.accel[a] += swing.accel[a];
                s.motion.gyro[a] += swing.gyro[a];
            }
        }
    } else if (single && config_.synthesize_motion) {
        s.motion = synthetic_motion(throw_started, pad, now_ns);
    }
    return s;
}

PadInput::PadInput(std::vector<MapperConfig> slots) {
    for (const MapperConfig& c : slots)
        mappers_.emplace_back(c);
    pads_.resize(mappers_.size());
    states_.resize(mappers_.size());
}

uint64_t PadInput::now_ns() const {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

void PadInput::poll() {
    std::lock_guard lock(mutex_);
    std::fill(pads_.begin(), pads_.end(), HostPad{});
    sample(pads_);
    const uint64_t now = now_ns();
    for (size_t i = 0; i < mappers_.size(); ++i)
        states_[i] = mappers_[i].map(pads_[i], now);
    touch_ = TouchScreenState{};
    sample_touch(touch_);
}

ControllerState PadInput::controller(size_t index) const {
    std::lock_guard lock(mutex_);
    return index < states_.size() ? states_[index] : ControllerState{};
}

TouchScreenState PadInput::touch_screen() const {
    std::lock_guard lock(mutex_);
    return touch_;
}

void PadInput::set_slot(size_t index, const MapperConfig& config) {
    std::lock_guard lock(mutex_);
    if (index < mappers_.size())
        mappers_[index] = ControllerMapper(config);
}

MapperConfig PadInput::slot(size_t index) const {
    std::lock_guard lock(mutex_);
    return index < mappers_.size() ? mappers_[index].config() : MapperConfig{};
}

void PadInput::set_vibration(size_t index, float low, float high) {
    std::lock_guard lock(mutex_);  // backends see rumble() and sample() in order
    if (index < mappers_.size())
        rumble(index, std::clamp(low, 0.0f, 1.0f), std::clamp(high, 0.0f, 1.0f));
}

std::vector<size_t> PlayerPick::update(const std::vector<uint64_t>& ids, const std::vector<uint64_t>& buttons) {
    // Player 1 gone: choose again, already with this sample's presses.
    if (player1_ != 0 && std::find(ids.begin(), ids.end(), player1_) == ids.end()) {
        player1_ = 0;
        picking_ = mode_ == Mode::Press;
    }
    std::vector<std::pair<uint64_t, uint64_t>> held;
    held.reserve(ids.size());
    for (size_t i = 0; i < ids.size(); ++i) {
        const uint64_t now = i < buttons.size() ? buttons[i] : 0;
        uint64_t before = now;  // a pad that just showed up has pressed nothing yet
        for (const auto& [id, b] : held_)
            if (id == ids[i])
                before = b;
        if (picking_ && (now & ~before) != 0) {
            player1_ = ids[i];  // the first in the backend's order wins a tie
            picking_ = false;
        }
        held.emplace_back(ids[i], now);
    }
    held_ = std::move(held);
    std::vector<size_t> order;
    order.reserve(ids.size());
    for (size_t i = 0; i < ids.size(); ++i)
        if (ids[i] == player1_)
            order.push_back(i);
    for (size_t i = 0; i < ids.size(); ++i)
        if (ids[i] != player1_)
            order.push_back(i);
    return order;
}

void PlayerPick::repick() { picking_ = mode_ == Mode::Press; }

void PlayerPick::set_mode(Mode mode) {
    mode_ = mode;
    picking_ = mode_ == Mode::Press && player1_ == 0;
    if (mode_ == Mode::Order)
        player1_ = 0;
}

bool parse_motion_frame(const std::string& spec, MapperConfig& config) {
    int axis[3];
    float sign[3];
    bool used[3] = {false, false, false};
    size_t pos = 0;
    for (int i = 0; i < 3; ++i) {
        const size_t end = std::min(spec.find(',', pos), spec.size());
        std::string_view item = std::string_view(spec).substr(pos, end - pos);
        pos = end + 1;
        sign[i] = 1.0f;
        if (!item.empty() && (item[0] == '-' || item[0] == '+')) {
            sign[i] = item[0] == '-' ? -1.0f : 1.0f;
            item.remove_prefix(1);
        }
        if (item.size() != 1 || item[0] < 'x' || item[0] > 'z' || used[item[0] - 'x'])
            return false;
        axis[i] = item[0] - 'x';
        used[axis[i]] = true;
        if ((i < 2) != (end < spec.size()))
            return false;  // exactly three items
    }
    for (int i = 0; i < 3; ++i) {
        config.motion_axis[i] = axis[i];
        config.motion_sign[i] = sign[i];
    }
    return true;
}

bool parse_stick_range(const std::string& spec, MapperConfig& config) {
    float v[3] = {0, 0, 0};
    size_t count = 0, pos = 0;
    while (count < 3) {
        const size_t comma = spec.find(',', pos);
        const std::string part = spec.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        char* end = nullptr;
        v[count++] = std::strtof(part.c_str(), &end);
        if (part.empty() || *end != '\0')
            return false;
        if (comma == std::string::npos)
            break;
        pos = comma + 1;
        if (count == 3)
            return false;  // a fourth value
    }
    if (count < 2)
        return false;
    if (!(v[0] >= 0.0f && v[0] < v[1] && v[1] <= 1.0f && v[2] >= 0.0f && v[2] < 1.0f))
        return false;  // also rejects NaN
    config.stick_deadzone = v[0];
    config.stick_outer = v[1];
    config.stick_hold = v[2];
    return true;
}

bool parse_throw_spec(const std::string& spec, MapperConfig& config) {
    MapperConfig c = config;
    auto axis = [](std::string_view v, int& out, bool all_ok) {
        if (all_ok && v == "all") {
            out = 3;
            return true;
        }
        if (v.size() != 1 || v[0] < 'x' || v[0] > 'z')
            return false;
        out = v[0] - 'x';
        return true;
    };
    size_t pos = 0;
    while (pos < spec.size()) {
        const size_t end = std::min(spec.find(',', pos), spec.size());
        const std::string_view item = std::string_view(spec).substr(pos, end - pos);
        pos = end + 1;
        const size_t eq = item.find('=');
        if (eq == std::string_view::npos)
            return false;
        const std::string_view key = item.substr(0, eq);
        const std::string value(item.substr(eq + 1));
        char* rest = nullptr;
        const float number = std::strtof(value.c_str(), &rest);
        const bool numeric = !value.empty() && rest && *rest == '\0' && std::isfinite(number);
        if (key == "ms" && numeric && number >= 0 && number <= 60000)
            c.throw_duration_ms = static_cast<uint32_t>(number);
        else if (key == "g" && numeric)
            c.throw_peak_accel_g = number;
        else if (key == "rps" && numeric)
            c.throw_peak_gyro_rps = number;
        else if (key == "accel") {
            if (!axis(value, c.throw_accel_axis, true))
                return false;
        } else if (key == "gyro") {
            if (!axis(value, c.throw_gyro_axis, true))
                return false;
        } else if (key == "rest") {
            std::string_view v = value;
            c.rest_gravity_sign = 1.0f;
            if (!v.empty() && (v[0] == '-' || v[0] == '+')) {
                c.rest_gravity_sign = v[0] == '-' ? -1.0f : 1.0f;
                v.remove_prefix(1);
            }
            if (!axis(v, c.rest_gravity_axis, false))
                return false;
        } else if (key == "side" || key == "yaw" || key == "leftyaw") {
            // yaw: the turn of both sides, leftyaw (after it) only of the left.
            int& out = key == "side" ? c.throw_side_accel_axis
                       : key == "yaw" ? c.throw_side_gyro_axis
                                      : c.throw_side_left_gyro_axis;
            float& sign = key == "side" ? c.throw_side_accel_sign
                          : key == "yaw" ? c.throw_side_gyro_sign
                                         : c.throw_side_left_gyro_sign;
            std::string_view v = value;
            if (v == "none") {
                out = -1;
                if (key == "yaw")
                    c.throw_side_left_gyro_axis = -1;
                continue;
            }
            sign = 1.0f;
            if (!v.empty() && (v[0] == '-' || v[0] == '+')) {
                sign = v[0] == '-' ? -1.0f : 1.0f;
                v.remove_prefix(1);
            }
            if (!axis(v, out, false))
                return false;
            if (key == "yaw") {
                c.throw_side_left_gyro_axis = out;
                c.throw_side_left_gyro_sign = sign;
            }
        } else if (key == "sidegain" && numeric && number >= 0 && number <= 10) {
            c.throw_side_gain = number;
            c.throw_side_left_gain = number;
        } else if (key == "leftgain" && numeric && number >= 0 && number <= 10) {
            c.throw_side_left_gain = number;
        } else if ((key == "sideaccel" || key == "leftaccel") && numeric && number >= -10 && number <= 10) {
            // Signed: the left one is not mirrored (+ = the axis' + side).
            if (key == "sideaccel")
                c.throw_side_accel_gain = c.throw_side_left_accel_gain = number;
            else
                c.throw_side_left_accel_gain = number;
        } else if (key == "dir" && numeric && number >= -1 && number <= 1) {
            c.throw_force_side = number;
        } else if (key == "strength" && numeric && number >= 1 && number <= 2) {
            c.throw_force_strength = number;
        } else if (key == "reach" && numeric && number >= -1 && number <= 1) {
            c.throw_force_reach = number;
        } else if ((key == "short" || key == "long") && numeric && number >= 100 && number <= 2000) {
            (key == "short" ? c.throw_reach_short_ms : c.throw_reach_long_ms) = static_cast<uint32_t>(number);
        } else if (key == "sensors" && (value == "add" || value == "replace")) {
            c.throw_replace_sensors = value == "replace";
        } else if (key == "tempo" && numeric && number >= 1 && number <= 2) {
            c.throw_strength_max = number;
        } else {
            return false;
        }
    }
    config = c;
    return true;
}

std::vector<MapperConfig> default_slots() {
    MapperConfig handheld;
    handheld.style = ControllerStyle::Handheld;
    MapperConfig joycon;
    joycon.style = ControllerStyle::JoyconRight;
    return {handheld, joycon};
}

}  // namespace lgp::platform
