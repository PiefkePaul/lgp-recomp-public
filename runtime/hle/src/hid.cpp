// hid (P5.1): IHidServer, IAppletResource, IActiveVibrationDeviceList and the
// shared memory writer. See lgp/hle/hid.hpp and
// docs/knowledge/horizon-hid-npad-shared-memory.md.
#include "lgp/hle/hid.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "hid_gesture.hpp"
#include "hid_shared_memory.hpp"
#include "lgp/runtime/log.hpp"

namespace lgp::hle {

using platform::ControllerState;
using platform::ControllerStyle;
using rt::LogLevel;

namespace {
constexpr float kTraceTurn = 0.5f;  // rev/s: --input-log traces faster turns (swings, throws)

using hidmem::NpadLifo;
using hidmem::SixAxisLifo;

constexpr uint32_t kColorOk = 0;
constexpr uint32_t kColorNoController = 2;

// nn::hid::NpadAttribute bits (common state).
constexpr uint32_t kAttrConnected = 1u << 0;
constexpr uint32_t kAttrWired = 1u << 1;
constexpr uint32_t kAttrLeftConnected = 1u << 2;
constexpr uint32_t kAttrLeftWired = 1u << 3;
constexpr uint32_t kAttrRightConnected = 1u << 4;
constexpr uint32_t kAttrRightWired = 1u << 5;

// DeviceType bits.
constexpr uint32_t kDeviceFullKey = 1u << 0;
constexpr uint32_t kDeviceHandheldLeft = 1u << 2;
constexpr uint32_t kDeviceHandheldRight = 1u << 3;
constexpr uint32_t kDeviceJoyLeft = 1u << 4;
constexpr uint32_t kDeviceJoyRight = 1u << 5;

// NpadSystemProperties bits.
constexpr uint64_t kPropPoweredDual = 1ull << 3;
constexpr uint64_t kPropPoweredLeft = 1ull << 4;
constexpr uint64_t kPropPoweredRight = 1ull << 5;
constexpr uint64_t kPropAbxyOriented = 1ull << 11;
constexpr uint64_t kPropSlSrOriented = 1ull << 12;
constexpr uint64_t kPropPlusAvailable = 1ull << 13;
constexpr uint64_t kPropMinusAvailable = 1ull << 14;
constexpr uint64_t kPropDirectionalAvailable = 1ull << 15;

constexpr uint32_t kBatteryFull = 4;

constexpr uint32_t kSixAxisConnected = 1u << 0;

// nn::hid::TouchAttribute (libnx HidTouchAttribute).
constexpr uint32_t kTouchStart = 1u << 0;
constexpr uint32_t kTouchEnd = 1u << 1;
// Contact size reported for every finger; hosts give none we could map.
// Plausible value, not measured on the console.
constexpr uint32_t kTouchDiameter = 15;
// Diagnosis LGP_HID_TOUCH_MAX_X=N: touch x up to N instead of 1279 (with the
// widescreen mod's LGP_WIDESCREEN_TOUCH=wide, whether the game reads touches
// across the wider picture).
std::atomic<float> g_touch_sx{1.0f}, g_touch_sy{1.0f};
long touch_max_x() {
    static const long max = [] {
        const char* env = std::getenv("LGP_HID_TOUCH_MAX_X");
        if (env == nullptr || *env == '\0') return 1279L;
        char* end = nullptr;
        const long n = std::strtol(env, &end, 10);
        if (end == env || *end != '\0' || n < 1279 || n > 8191) {
            std::fprintf(stderr, "LGP_HID_TOUCH_MAX_X: expected 1279..8191, ignored\n");
            return 1279L;
        }
        return n;
    }();
    return max;
}

// Sensor handles and vibration handles share one layout: style index (bits
// 0..7: 3 FullKey, 4 Handheld, 5 JoyDual, 6 JoyLeft, 7 JoyRight), npad id
// (8..15) and device index (16..23: 0 left, 1 right, 2 both/none).
struct DeviceHandle {
    uint8_t style_index;
    uint32_t npad_id;
    uint8_t device;
};
DeviceHandle decode_handle(uint32_t v) {
    return {static_cast<uint8_t>(v & 0xFF), (v >> 8) & 0xFF, static_cast<uint8_t>((v >> 16) & 0xFF)};
}
// Npad style bit a handle's style index belongs to, 0 if unknown.
uint32_t handle_style(uint8_t style_index) {
    switch (style_index) {
    case 3: return npad_style::kFullKey;
    case 4: return npad_style::kHandheld;
    case 5: return npad_style::kJoyDual;
    case 6: return npad_style::kJoyLeft;
    case 7: return npad_style::kJoyRight;
    default: return 0;
    }
}

size_t lifo_of_style(uint32_t style) {
    switch (style) {
    case npad_style::kFullKey: return static_cast<size_t>(NpadLifo::FullKey);
    case npad_style::kHandheld: return static_cast<size_t>(NpadLifo::Handheld);
    case npad_style::kJoyDual: return static_cast<size_t>(NpadLifo::JoyDual);
    case npad_style::kJoyLeft: return static_cast<size_t>(NpadLifo::JoyLeft);
    case npad_style::kJoyRight: return static_cast<size_t>(NpadLifo::JoyRight);
    default: return static_cast<size_t>(NpadLifo::Count);
    }
}

bool has_left(ControllerStyle s) {
    return s == ControllerStyle::Handheld || s == ControllerStyle::JoyconLeft || s == ControllerStyle::JoyconPair;
}
bool has_right(ControllerStyle s) {
    return s == ControllerStyle::Handheld || s == ControllerStyle::JoyconRight || s == ControllerStyle::JoyconPair;
}

// What one npad shows for the controller behind it.
struct Presentation {
    uint32_t attributes = 0;
    uint32_t device_type = 0;
    uint64_t properties = 0;
    bool left = false;
    bool right = false;
};

Presentation present(uint32_t style, ControllerStyle source) {
    Presentation p;
    p.left = has_left(source);
    p.right = has_right(source);
    switch (style) {
    case npad_style::kHandheld:
        p.attributes = kAttrConnected | kAttrWired | kAttrLeftConnected | kAttrLeftWired | kAttrRightConnected |
                       kAttrRightWired;
        p.device_type = kDeviceHandheldLeft | kDeviceHandheldRight;
        p.properties = kPropPoweredDual | kPropPoweredLeft | kPropPoweredRight | kPropAbxyOriented |
                       kPropPlusAvailable | kPropMinusAvailable | kPropDirectionalAvailable;
        break;
    case npad_style::kFullKey:
        p.attributes = kAttrConnected;
        p.device_type = kDeviceFullKey;
        p.properties = kPropPoweredDual | kPropAbxyOriented | kPropPlusAvailable | kPropMinusAvailable |
                       kPropDirectionalAvailable;
        p.left = p.right = false;
        break;
    case npad_style::kJoyDual:
        p.attributes = kAttrConnected | (p.left ? kAttrLeftConnected : 0) | (p.right ? kAttrRightConnected : 0);
        p.device_type = (p.left ? kDeviceJoyLeft : 0) | (p.right ? kDeviceJoyRight : 0);
        p.properties = kPropAbxyOriented | (p.left ? kPropPoweredLeft | kPropMinusAvailable | kPropDirectionalAvailable : 0) |
                       (p.right ? kPropPoweredRight | kPropPlusAvailable : 0);
        break;
    case npad_style::kJoyLeft:
        p.attributes = kAttrConnected | kAttrLeftConnected;
        p.device_type = kDeviceJoyLeft;
        p.properties = kPropPoweredLeft | kPropSlSrOriented | kPropMinusAvailable | kPropDirectionalAvailable;
        break;
    case npad_style::kJoyRight:
        p.attributes = kAttrConnected | kAttrRightConnected;
        p.device_type = kDeviceJoyRight;
        p.properties = kPropPoweredRight | kPropSlSrOriented | kPropPlusAvailable;
        break;
    default:
        break;
    }
    return p;
}

// Rotates the orientation matrix by the angular velocity w (revolutions per
// second, controller frame) over dt seconds, then re-orthonormalises it.
void rotate(float m[3][3], const float w[3], float dt) {
    constexpr float kTwoPi = 6.2831853f;
    const float wx = w[0] * kTwoPi, wy = w[1] * kTwoPi, wz = w[2] * kTwoPi;
    const float speed = std::sqrt(wx * wx + wy * wy + wz * wz);
    const float theta = speed * dt;
    if (theta <= 0.0f)
        return;
    const float x = wx / speed, y = wy / speed, z = wz / speed;
    const float c = std::cos(theta), s = std::sin(theta), t = 1.0f - c;
    const float r[3][3] = {{t * x * x + c, t * x * y - s * z, t * x * z + s * y},
                           {t * x * y + s * z, t * y * y + c, t * y * z - s * x},
                           {t * x * z - s * y, t * y * z + s * x, t * z * z + c}};
    float out[3][3];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            out[i][j] = m[i][0] * r[0][j] + m[i][1] * r[1][j] + m[i][2] * r[2][j];
    // Gram-Schmidt on the rows.
    for (int i = 0; i < 3; ++i) {
        for (int k = 0; k < i; ++k) {
            const float d = out[i][0] * out[k][0] + out[i][1] * out[k][1] + out[i][2] * out[k][2];
            for (int j = 0; j < 3; ++j)
                out[i][j] -= d * out[k][j];
        }
        const float n = std::sqrt(out[i][0] * out[i][0] + out[i][1] * out[i][1] + out[i][2] * out[i][2]);
        for (int j = 0; j < 3; ++j)
            m[i][j] = n > 0.0f ? out[i][j] / n : (i == j ? 1.0f : 0.0f);
    }
}

}  // namespace

// --- HidState ----------------------------------------------------------------------

HidState::HidState(const HidConfig& config) : config_(config), gestures_(std::make_unique<GestureTracker>()) {
    supported_ids_.fill(true);
}

HidState::~HidState() = default;

int HidState::npad_index(uint32_t id) {
    if (id <= npad_id::kNo8)
        return static_cast<int>(id);
    if (id == npad_id::kHandheld)
        return 8;
    if (id == npad_id::kOther)
        return 9;
    return -1;
}

std::shared_ptr<rt::KSharedMemory> HidState::shared_memory(rt::Kernel& kernel) {
    if (!shm_) {
        shm_ = std::make_shared<rt::KSharedMemory>(kernel, hidmem::kSharedMemorySize, rt::kPermR);
        refresh(kernel);
    }
    return shm_;
}

void HidState::set_supported_ids(const std::vector<uint32_t>& ids) {
    supported_ids_.fill(false);
    for (uint32_t id : ids) {
        const int i = npad_index(id);
        if (i >= 0)
            supported_ids_[static_cast<size_t>(i)] = true;
    }
}

void HidState::disconnect(size_t index) {
    Npad& n = npads_[index];
    if (n.slot < 0)
        return;
    n.disconnected = true;
    n.disconnected_style = controllers_[static_cast<size_t>(n.slot)].style;
    n.held_since_disconnect = controllers_[static_cast<size_t>(n.slot)].buttons;
    n.disconnected_device = controllers_[static_cast<size_t>(n.slot)].device;
    rt::log(LogLevel::Info, "hid: DisconnectNpad(%zu), slot %d waits for a new button press", index, n.slot);
    // The backend may choose who registers next (the first pad pressed).
    if (config_.input)
        config_.input->npad_disconnected(static_cast<size_t>(n.slot));
}

void HidState::refresh(rt::Kernel& kernel) {
    std::array<uint32_t, hidmem::kNpadCount> style{};
    std::array<int, hidmem::kNpadCount> slot;
    slot.fill(-1);
    // DisconnectNpad holds until the slot shows something else (unplugged,
    // or another style) or until a button is newly pressed there: a real
    // Joy-Con reconnects when the player presses a button (SL+SR on the
    // game's controller screen), and a host pad has no other way to do it.
    for (size_t i = 0; i < npads_.size(); ++i) {
        Npad& n = npads_[i];
        if (!n.disconnected)
            continue;
        if (n.slot < 0 || static_cast<size_t>(n.slot) >= controllers_.size() ||
            controllers_[static_cast<size_t>(n.slot)].style != n.disconnected_style) {
            n.disconnected = false;
            continue;
        }
        const uint64_t buttons = controllers_[static_cast<size_t>(n.slot)].buttons;
        const uint64_t device = controllers_[static_cast<size_t>(n.slot)].device;
        if (device != n.disconnected_device) {
            // Another pad took the slot (PlayerPick): its buttons are its own
            // press, even the ones the old pad held at the disconnect.
            n.held_since_disconnect = 0;
            n.disconnected_device = device;
        }
        if (buttons & ~n.held_since_disconnect) {
            n.disconnected = false;
            rt::log(LogLevel::Info, "hid: npad %zu reconnected by a button press on slot %d", i, n.slot);
        } else {
            n.held_since_disconnect = buttons;  // released buttons count again
        }
    }
    for (size_t i = 0; i < controllers_.size(); ++i) {
        const ControllerStyle s = controllers_[i].style;
        if (s == ControllerStyle::None)
            continue;
        const size_t index = s == ControllerStyle::Handheld ? 8 : i;
        if (index >= 8 && s != ControllerStyle::Handheld)
            continue;  // more slots than player npads
        if (slot[index] >= 0)
            continue;  // first slot wins (two handheld controllers)
        Npad& n = npads_[index];
        if (n.disconnected && n.slot == static_cast<int>(i))
            continue;  // still the controller the game disconnected
        uint32_t st = 0;
        switch (s) {
        case ControllerStyle::Handheld: st = npad_style::kHandheld; break;
        case ControllerStyle::ProController: st = npad_style::kFullKey; break;
        case ControllerStyle::JoyconPair: st = npad_style::kJoyDual; break;
        case ControllerStyle::JoyconLeft: st = n.joy_mode == 1 ? npad_style::kJoyLeft : npad_style::kJoyDual; break;
        case ControllerStyle::JoyconRight: st = n.joy_mode == 1 ? npad_style::kJoyRight : npad_style::kJoyDual; break;
        case ControllerStyle::None: break;
        }
        if ((st & supported_styles_) == 0 || !supported_ids_[index])
            continue;
        style[index] = st;
        slot[index] = static_cast<int>(i);
    }
    for (size_t i = 0; i < hidmem::kNpadCount; ++i) {
        Npad& n = npads_[i];
        // A disconnected npad keeps its slot for the test above.
        if (!(n.disconnected && style[i] == 0))
            n.slot = slot[i];
        if (n.style != style[i]) {
            rt::log(LogLevel::Debug, "hid: npad %zu presented style 0x%x -> 0x%x (slot %d)", i, n.style, style[i], n.slot);
            n.style = style[i];
            n.style_event.signal(kernel);
        }
    }
    if (shm_) {
        std::lock_guard mem_lock(kernel.memory_mutex());
        write_headers(*reinterpret_cast<hidmem::SharedMemory*>(shm_->data()));
    }
}

void HidState::write_headers(hidmem::SharedMemory& mem) const {
    for (size_t i = 0; i < hidmem::kNpadCount; ++i) {
        const Npad& n = npads_[i];
        hidmem::NpadInternalState& s = mem.npad[i].state;
        s.joy_assignment_mode = n.joy_mode;
        if (n.style == 0) {
            s.style_set = 0;
            s.device_type = 0;
            s.system_properties = 0;
            s.full_key_color = {kColorNoController, {0, 0}};
            s.joy_color = {kColorNoController, {0, 0}, {0, 0}};
            std::fill(std::begin(s.battery_level), std::end(s.battery_level), 0u);
            continue;
        }
        const Presentation p = present(n.style, controllers_[static_cast<size_t>(n.slot)].style);
        const hidmem::ControllerColor left{config_.joy_left_body, config_.joy_left_buttons};
        const hidmem::ControllerColor right{config_.joy_right_body, config_.joy_right_buttons};
        const hidmem::ControllerColor pro{config_.full_key_body, config_.full_key_buttons};
        s.style_set = n.style;
        s.device_type = p.device_type;
        s.system_properties = p.properties;
        // Single-controller color: the Pro Controller, or the one Joy-Con.
        if (n.style == npad_style::kFullKey) {
            s.full_key_color = {kColorOk, pro};
            s.joy_color = {kColorNoController, {0, 0}, {0, 0}};
        } else {
            s.full_key_color = {kColorOk, p.left ? left : right};
            s.joy_color = {kColorOk, p.left ? left : hidmem::ControllerColor{0, 0},
                           p.right ? right : hidmem::ControllerColor{0, 0}};
        }
        s.battery_level[0] = (n.style == npad_style::kFullKey || n.style == npad_style::kHandheld) ? kBatteryFull : 0;
        s.battery_level[1] = p.left ? kBatteryFull : 0;
        s.battery_level[2] = p.right ? kBatteryFull : 0;
    }
}

void HidState::push_samples(hidmem::SharedMemory& mem, uint64_t now_ns) {
    for (size_t i = 0; i < hidmem::kNpadCount; ++i) {
        Npad& n = npads_[i];
        hidmem::NpadInternalState& s = mem.npad[i].state;
        hidmem::NpadCommonState active{};
        size_t active_lifo = static_cast<size_t>(NpadLifo::Count);
        if (n.style != 0) {
            const ControllerState& c = controllers_[static_cast<size_t>(n.slot)];
            active.buttons = c.buttons;
            active.stick_l = {c.left.x, c.left.y};
            active.stick_r = {c.right.x, c.right.y};
            active.attributes = present(n.style, c.style).attributes;
            active_lifo = lifo_of_style(n.style);
        }
        const uint64_t pressed = active.buttons & ~n.pushed_buttons;
        n.pushed_buttons = active.buttons;
        if (pressed != 0) {
            const auto count = static_cast<uint32_t>(std::popcount(pressed));
            n.presses += count;
            n.window_presses += count;
            n.window_buttons |= pressed;
        }
        // Every lifo gets a sample each tick; the ones of other styles an
        // empty state, so all of them stay current for the reader. SystemExt
        // mirrors the active controller (system applets read it).
        for (size_t l = 0; l < static_cast<size_t>(NpadLifo::Count); ++l) {
            hidmem::NpadCommonState state{};
            if (l == active_lifo || (n.style != 0 && l == static_cast<size_t>(NpadLifo::SystemExt)))
                state = active;
            state.sampling_number = n.sampling[l]++;
            hidmem::push(s.lifos[l], state);
        }
    }
    for (auto& [handle, sensor] : six_axis_) {
        if (sensor.started)
            push_six_axis(mem, handle, sensor, now_ns);
        else if (sensor.idle_ticks < UINT32_MAX)
            ++sensor.idle_ticks;
    }
    if (touch_active_) {
        // Fingers of this tick (Start when new), then the ones that left
        // since the last tick, once more at their last position with End.
        hidmem::TouchScreenState t{};
        t.sampling_number = touch_sampling_++;
        const auto contains = [](const platform::TouchScreenState& s, uint32_t id) {
            for (uint32_t i = 0; i < s.count; ++i)
                if (s.points[i].id == id)
                    return true;
            return false;
        };
        const auto add = [&t](const platform::TouchPoint& p, uint32_t attributes) {
            if (t.count >= static_cast<int32_t>(std::size(t.touches)))
                return;
            hidmem::TouchState& s = t.touches[t.count++];
            s.attributes = attributes;
            s.finger_id = p.id;
            const float sx = g_touch_sx.load(std::memory_order_relaxed), sy = g_touch_sy.load(std::memory_order_relaxed);
            const long max_x = std::max(touch_max_x(), std::lround(1280.0f * sx) - 1),
                       max_y = std::lround(720.0f * sy) - 1;
            s.x = static_cast<uint32_t>(std::clamp(std::lround(p.x * sx), 0L, max_x));
            s.y = static_cast<uint32_t>(std::clamp(std::lround(p.y * sy), 0L, std::max(719L, max_y)));
            s.diameter_x = s.diameter_y = kTouchDiameter;
        };
        for (uint32_t i = 0; i < touch_now_.count; ++i)
            add(touch_now_.points[i], contains(touch_last_, touch_now_.points[i].id) ? 0 : kTouchStart);
        for (uint32_t i = 0; i < touch_last_.count; ++i)
            if (!contains(touch_now_, touch_last_.points[i].id))
                add(touch_last_.points[i], kTouchEnd);
        hidmem::push(mem.touch_screen.lifo, t);
    }
    touch_last_ = touch_now_;
    if (gesture_active_) {
        hidmem::GestureState g{};
        if (gestures_->update(touch_now_, now_ns, &g)) {
            g.sampling_number = gesture_sampling_++;
            hidmem::push(mem.gesture.lifo, g);
        }
    }
    if (debug_pad_active_) {
        hidmem::DebugPadState d{};  // no debug pad: attributes 0
        d.sampling_number = debug_pad_sampling_++;
        hidmem::push(mem.debug_pad.lifo, d);
    }
}

void HidState::push_six_axis(hidmem::SharedMemory& mem, uint32_t handle, SixAxis& sensor, uint64_t now_ns) {
    const DeviceHandle h = decode_handle(handle);
    const int index = npad_index(h.npad_id);
    if (index < 0)
        return;
    SixAxisLifo lifo;
    switch (h.style_index) {
    case 3: lifo = SixAxisLifo::FullKey; break;
    case 4: lifo = SixAxisLifo::Handheld; break;
    case 5: lifo = h.device == 1 ? SixAxisLifo::JoyDualRight : SixAxisLifo::JoyDualLeft; break;
    case 6: lifo = SixAxisLifo::JoyLeft; break;
    case 7: lifo = SixAxisLifo::JoyRight; break;
    default: return;
    }
    const Npad& n = npads_[static_cast<size_t>(index)];
    hidmem::SixAxisSensorState st{};
    st.delta_time = sensor.last_ns == 0 || now_ns < sensor.last_ns ? 0 : now_ns - sensor.last_ns;
    sensor.last_ns = now_ns;
    st.sampling_number = sensor.sampling_number++;
    const bool connected = n.style != 0 && n.style == handle_style(h.style_index);
    const platform::MotionState* m = connected ? &controllers_[static_cast<size_t>(n.slot)].motion : nullptr;
    if (m && m->valid) {
        const float dt = static_cast<float>(st.delta_time) * 1e-9f;
        if (!config_.freeze_orientation) {
            for (int a = 0; a < 3; ++a)
                sensor.angle[a] += m->gyro[a] * dt;
            rotate(sensor.direction, m->gyro, dt);
        }
        st.acceleration = {m->accel[0], m->accel[1], m->accel[2]};
        st.angular_velocity = {m->gyro[0], m->gyro[1], m->gyro[2]};
        st.attributes = kSixAxisConnected;
        // --input-log: the samples of a swing as the game reads them (a real
        // throw recorded once can be rebuilt for the touch swipe).
        const float turn = std::fabs(m->gyro[0]) + std::fabs(m->gyro[1]) + std::fabs(m->gyro[2]);
        if (config_.log_input && turn >= kTraceTurn)
            rt::log(LogLevel::Info,
                    "hid: motion npad %d style index %u t %.3f s: accel %.2f %.2f %.2f g, gyro %.2f %.2f %.2f rev/s, "
                    "angle %.3f %.3f %.3f, direction x %.2f %.2f %.2f y %.2f %.2f %.2f z %.2f %.2f %.2f",
                    index, h.style_index, static_cast<double>(now_ns) * 1e-9, m->accel[0], m->accel[1], m->accel[2],
                    m->gyro[0], m->gyro[1], m->gyro[2], sensor.angle[0], sensor.angle[1], sensor.angle[2],
                    sensor.direction[0][0], sensor.direction[0][1], sensor.direction[0][2], sensor.direction[1][0],
                    sensor.direction[1][1], sensor.direction[1][2], sensor.direction[2][0], sensor.direction[2][1],
                    sensor.direction[2][2]);
    }
    // Without motion data the sample says "not connected" (attributes 0)
    // instead of inventing a resting pose.
    st.angle = {sensor.angle[0], sensor.angle[1], sensor.angle[2]};
    std::memcpy(st.direction, sensor.direction, sizeof(st.direction));
    hidmem::push(mem.npad[index].state.six_axis[static_cast<size_t>(lifo)], st);
}

void HidState::update(rt::Kernel& kernel, uint64_t now_ns) {
    if (config_.input)
        config_.input->poll();
    std::lock_guard lock(mutex_);
    const size_t count = config_.input ? config_.input->controller_count() : 0;
    controllers_.resize(count);
    for (size_t i = 0; i < count; ++i)
        controllers_[i] = config_.input->controller(i);
    // L and R pressed together reset the motion sensors' orientation, a
    // recentre the console does not have: our direction has no gravity
    // correction and drifts.
    chords_.resize(count);
    bool recenter = false;
    for (size_t i = 0; i < count; ++i)
        recenter |= filter_recenter(controllers_[i], chords_[i], now_ns);
    if (recenter) {
        for (auto& [handle, sensor] : six_axis_)
            sensor.reset_orientation();
        rt::log(LogLevel::Info, "hid: L+R: motion sensor orientation reset");
    }
    touch_now_ = config_.input ? config_.input->touch_screen() : platform::TouchScreenState{};
    touch_now_.count = std::min<uint32_t>(touch_now_.count, platform::kMaxTouches);
    refresh(kernel);
    if (config_.log_input)
        log_input_tick(now_ns);
    if (!shm_)
        return;
    std::lock_guard mem_lock(kernel.memory_mutex());
    push_samples(*reinterpret_cast<hidmem::SharedMemory*>(shm_->data()), now_ns);
}

bool HidState::filter_recenter(platform::ControllerState& c, Chord& chord, uint64_t now_ns) {
    // A single Joy-Con turns the host's shoulders into SL/SR (PadInput).
    constexpr uint64_t kPairs[] = {platform::kButtonL | platform::kButtonR,
                                   platform::kButtonRightSL | platform::kButtonRightSR,
                                   platform::kButtonLeftSL | platform::kButtonLeftSR};
    // Long enough for two fingers that mean to press together, short enough
    // not to notice on a lone shoulder press.
    constexpr uint64_t kChordWaitNs = 80'000'000;
    constexpr uint64_t kAllPairs = kPairs[0] | kPairs[1] | kPairs[2];
    if (c.style == platform::ControllerStyle::None) {
        chord = Chord{};
        return false;
    }
    const uint64_t down = c.buttons & kAllPairs;
    if (chord.active) {
        if (down == 0)
            chord.active = false;
        c.buttons &= ~kAllPairs;
        return false;
    }
    for (const uint64_t pair : kPairs) {
        if ((down & pair) == pair) {
            chord = Chord{};
            chord.active = true;
            c.buttons &= ~kAllPairs;
            return true;
        }
    }
    // A held-back tap that ended before the wait: show it now, a little late.
    if (chord.tap != 0 && now_ns < chord.tap_until_ns)
        c.buttons |= chord.tap;
    else
        chord.tap = 0;
    if (down == 0) {
        if (chord.first_ns != 0 && chord.held != 0) {
            chord.tap = chord.held;
            chord.tap_until_ns = now_ns + kChordWaitNs;
            c.buttons |= chord.tap;
        }
        chord.first_ns = 0;
        chord.held = 0;
        return false;
    }
    if (chord.first_ns == 0 && chord.held == 0) {
        chord.first_ns = now_ns != 0 ? now_ns : 1;
        chord.held = down;
    }
    if (chord.first_ns != 0 && now_ns - chord.first_ns < kChordWaitNs)
        c.buttons &= ~kAllPairs;  // wait for the second button
    else
        chord.first_ns = 0;  // a lone press after all: passes from now on
    return false;
}

bool HidState::is_at_rest(uint32_t handle) const {
    const DeviceHandle h = decode_handle(handle);
    const int index = npad_index(h.npad_id);
    if (index < 0)
        return true;
    const Npad& n = npads_[static_cast<size_t>(index)];
    if (n.style == 0 || n.style != handle_style(h.style_index))
        return true;
    const platform::MotionState& m = controllers_[static_cast<size_t>(n.slot)].motion;
    if (!m.valid)
        return true;
    constexpr float kGyroRest = 0.005f;  // rev/s, about 1.8 degrees per second
    return std::fabs(m.gyro[0]) < kGyroRest && std::fabs(m.gyro[1]) < kGyroRest && std::fabs(m.gyro[2]) < kGyroRest;
}

void HidState::vibrate(uint32_t command, uint32_t handle, const float value[4]) {
    const DeviceHandle h = decode_handle(handle);
    const int index = npad_index(h.npad_id);
    // Only the handle of the presented style reaches a motor. LGP keeps
    // sending zeros to the JoyDual handle of npad No1 while it rumbles the
    // JoyRight one; both map to the right side and the zeros won.
    const char* verdict = "motor";
    bool to_motor = false;
    if (index < 0 || !config_.input) {
        verdict = "dropped: no npad";
        ++vibration_counts_.dropped_npad;
    } else if (npads_[static_cast<size_t>(index)].style != handle_style(h.style_index)) {
        verdict = "dropped: style not presented";
        ++vibration_counts_.dropped_style;
    } else {
        ++vibration_counts_.sent;
        to_motor = true;
    }
    if (config_.log_input) {
        (to_motor ? log_.vib_sent : log_.vib_dropped)++;
        auto [it, fresh] = vibration_last_.try_emplace(handle);
        const std::array<float, 4> v = {value[0], value[1], value[2], value[3]};
        if (fresh || it->second != v) {
            it->second = v;
            rt::log(LogLevel::Info,
                    "hid: vibration #%u handle 0x%x (npad 0x%x, style index %u, device %u): low %.3f @ %.0f Hz, "
                    "high %.3f @ %.0f Hz -> %s (presented style 0x%x)",
                    command, handle, h.npad_id, h.style_index, h.device, v[0], v[1], v[2], v[3], verdict,
                    index < 0 ? 0u : npads_[static_cast<size_t>(index)].style);
        }
    }
    if (!to_motor)
        return;
    Npad& n = npads_[static_cast<size_t>(index)];
    const size_t side = h.device == 1 || h.style_index == 7 ? 1 : 0;
    const float low = std::clamp(value[0], 0.0f, 1.0f), high = std::clamp(value[2], 0.0f, 1.0f);
    if (!config_.log_input && (low != n.rumble[side][0] || high != n.rumble[side][1]))
        rt::log(LogLevel::Debug, "hid: vibration npad %d style index %u side %zu: %.2f/%.2f (slot %d, style 0x%x)",
                index, h.style_index, side, low, high, n.slot, n.style);
    n.rumble[side][0] = low;
    n.rumble[side][1] = high;
    if (n.slot < 0 || n.style == 0)
        return;
    // One motor pair per host controller: the stronger side wins.
    config_.input->set_vibration(static_cast<size_t>(n.slot), std::max(n.rumble[0][0], n.rumble[1][0]),
                                 std::max(n.rumble[0][1], n.rumble[1][1]));
}

HidState::VibrationCounts HidState::vibration_counts() const {
    std::lock_guard lock(mutex_);
    return vibration_counts_;
}

uint64_t HidState::button_presses(uint32_t npad_id) const {
    std::lock_guard lock(mutex_);
    const int i = npad_index(npad_id);
    return i < 0 ? 0 : npads_[static_cast<size_t>(i)].presses;
}

void HidState::log_input_tick(uint64_t now_ns) {
    LogWindow& w = log_;
    if (w.last_tick_ns != 0 && now_ns > w.last_tick_ns)
        w.max_tick_gap_ns = std::max(w.max_tick_gap_ns, now_ns - w.last_tick_ns);
    w.last_tick_ns = now_ns;
    ++w.ticks;
    // Stick drops as the game reads them, per presented npad.
    for (size_t i = 0; i < npads_.size(); ++i) {
        Npad& n = npads_[i];
        if (n.style == 0)
            continue;
        const ControllerState& c = controllers_[static_cast<size_t>(n.slot)];
        const auto len = [](const platform::StickState& st) {
            return std::sqrt(float(st.x) * float(st.x) + float(st.y) * float(st.y)) / 32767.0f;
        };
        const float l = std::max(len(c.left), len(c.right));
        n.stick_drops.sample(l, now_ns);
        n.stick_max = std::max(n.stick_max, l);
        if (l >= 0.5f)
            n.stick_held_min = std::min(n.stick_held_min, l);
        // The stick the player steers with: the right one on a single right Joy-Con.
        const platform::StickState& st = len(c.right) > len(c.left) ? c.right : c.left;
        const platform::StickState& last = n.stick_last;
        if (len(st) >= 0.5f && len(last) >= 0.5f && (st.x != last.x || st.y != last.y)) {
            ++n.stick_changes;
            const float turn = std::fabs(std::remainder(
                std::atan2(float(st.y), float(st.x)) - std::atan2(float(last.y), float(last.x)),
                2.0f * 3.14159265f));
            n.stick_turn_max = std::max(n.stick_turn_max, turn * 57.2957795f);
        }
        n.stick_last = st;
    }
    // New fingers on the touch screen (Pikachu play, menus in handheld mode).
    for (uint32_t i = 0; i < touch_now_.count; ++i) {
        bool known = false;
        for (uint32_t j = 0; j < touch_last_.count; ++j)
            known |= touch_last_.points[j].id == touch_now_.points[i].id;
        if (known)
            continue;
        if (w.touch_starts++ == 0) {
            w.touch_first_x = touch_now_.points[i].x;
            w.touch_first_y = touch_now_.points[i].y;
        }
    }
    if (w.start_ns == 0)
        w.start_ns = now_ns;
    if (now_ns - w.start_ns < 1'000'000'000)
        return;
    std::string drops, sticks, presses;
    uint32_t total_drops = 0;
    for (size_t i = 0; i < npads_.size(); ++i) {
        Npad& n = npads_[i];
        if (n.stick_drops.drops != 0) {
            drops += " npad" + std::to_string(i) + "=" + std::to_string(n.stick_drops.drops);
            total_drops += n.stick_drops.drops;
        }
        if (n.stick_max >= 0.5f) {
            char buf[96];
            std::snprintf(buf, sizeof(buf), " npad%zu=%.3f/%.3f (%u changes, max turn %.1f deg)", i, n.stick_max,
                          n.stick_held_min, n.stick_changes, n.stick_turn_max);
            sticks += buf;
        }
        if (n.window_presses != 0) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), " npad%zu=%u (0x%" PRIx64 ")", i, n.window_presses, n.window_buttons);
            presses += buf;
        }
        n.window_presses = 0;
        n.window_buttons = 0;
        n.stick_changes = 0;
        n.stick_turn_max = 0;
        n.stick_drops.drops = 0;
        n.stick_max = 0;
        n.stick_held_min = 2;
    }
    // Quiet seconds stay out of the log unless the tick stalled.
    if (w.vib_sent + w.vib_dropped + w.at_rest_yes + w.at_rest_no + total_drops != 0 || !sticks.empty() ||
        !presses.empty() || w.max_tick_gap_ns > 50'000'000 || w.touch_starts != 0)
        rt::log(LogLevel::Info,
                "hid: 1s: ticks %" PRIu64 " (max gap %.1f ms), vibration values %" PRIu64 " to motor / %" PRIu64
                " dropped, IsSixAxisSensorAtRest yes %" PRIu64 " / no %" PRIu64 ", stick drops:%s, stick max/held min:%s"
                ", button presses:%s, touches %u (first at %.0f,%.0f, screen %s)",
                w.ticks, static_cast<double>(w.max_tick_gap_ns) * 1e-6, w.vib_sent, w.vib_dropped, w.at_rest_yes,
                w.at_rest_no, drops.empty() ? " none" : drops.c_str(), sticks.empty() ? " -" : sticks.c_str(),
                presses.empty() ? " none" : presses.c_str(), w.touch_starts, static_cast<double>(w.touch_first_x), static_cast<double>(w.touch_first_y),
                touch_active_ ? "read by the game" : "not activated");
    const uint64_t last = w.last_tick_ns;
    w = LogWindow{};
    w.start_ns = now_ns;
    w.last_tick_ns = last;
}

uint32_t HidState::presented_style(uint32_t id) const {
    std::lock_guard lock(mutex_);
    const int i = npad_index(id);
    return i < 0 ? 0 : npads_[static_cast<size_t>(i)].style;
}

uint32_t HidState::supported_style_set() const {
    std::lock_guard lock(mutex_);
    return supported_styles_;
}

uint32_t HidState::joy_hold_type() const {
    std::lock_guard lock(mutex_);
    return hold_type_;
}

bool HidState::six_axis_started(uint32_t handle) const {
    std::lock_guard lock(mutex_);
    const auto it = six_axis_.find(handle);
    return it != six_axis_.end() && it->second.started;
}

// --- services ----------------------------------------------------------------------

class HidAppletResource final : public Service {
public:
    explicit HidAppletResource(std::shared_ptr<HidState> state) : Service("hid:IAppletResource"), state_(std::move(state)) {
        on(0, [this](Request& req, Response& resp) {  // GetSharedMemoryHandle
            std::shared_ptr<rt::KSharedMemory> shm;
            {
                std::lock_guard lock(state_->mutex_);
                shm = state_->shared_memory(req.kernel());
            }
            Handle h = rt::kInvalidHandle;
            resp.result = req.kernel().handles().add(shm, &h);
            if (resp.result.ok())
                resp.copy_handles.push_back(h);
        });
    }

private:
    std::shared_ptr<HidState> state_;
};

class HidActiveVibrationDeviceList final : public Service {
public:
    HidActiveVibrationDeviceList() : Service("hid:IActiveVibrationDeviceList") {
        on(0, [](Request& req, Response&) { req.pop<uint32_t>(); });  // ActivateVibrationDevice(handle)
    }
};

class HidServer final : public Service {
public:
    explicit HidServer(std::shared_ptr<HidState> state) : Service("hid"), state_(std::move(state)) {
        HidState& s = *state_;
        on(0, [this](Request&, Response& resp) {  // CreateAppletResource(aruid, pid)
            resp.push_object(std::make_shared<HidAppletResource>(state_));
        });
        on(1, [&s](Request&, Response&) {  // ActivateDebugPad(aruid)
            std::lock_guard lock(s.mutex_);
            s.activate_debug_pad();
        });
        on(11, [&s](Request&, Response&) {  // ActivateTouchScreen(aruid)
            std::lock_guard lock(s.mutex_);
            s.activate_touch_screen();
        });
        on(66, [&s](Request& req, Response&) {  // StartSixAxisSensor(handle, aruid)
            const uint32_t handle = req.pop<uint32_t>();
            const DeviceHandle h = decode_handle(handle);
            rt::log(LogLevel::Debug, "hid: StartSixAxisSensor(0x%x: style index %u, npad %u, device %u)", handle,
                    h.style_index, h.npad_id, h.device);
            std::lock_guard lock(s.mutex_);
            HidState::SixAxis& sensor = s.six_axis(handle);
            // A sensor restarted after a pause (a new aim, not the game's
            // stop/start within a few frames) begins at the fresh-start pose
            // again, like the first start: no drift carried over.
            constexpr uint32_t kRestartTicks = 30;
            if (!sensor.started && sensor.idle_ticks >= kRestartTicks) {
                sensor.reset_orientation();
                sensor.last_ns = 0;  // the first sample must not integrate the pause
            }
            sensor.started = true;
            sensor.idle_ticks = 0;
        });
        on(67, [&s](Request& req, Response&) {  // StopSixAxisSensor(handle, aruid)
            const uint32_t handle = req.pop<uint32_t>();
            rt::log(LogLevel::Debug, "hid: StopSixAxisSensor(0x%x)", handle);
            std::lock_guard lock(s.mutex_);
            s.six_axis(handle).started = false;
        });
        on(68, [&s](Request& req, Response& resp) {  // IsSixAxisSensorFusionEnabled(handle, aruid) -> bool
            std::lock_guard lock(s.mutex_);
            resp.push<uint8_t>(s.six_axis(req.pop<uint32_t>()).fusion ? 1 : 0);
        });
        on(69, [&s](Request& req, Response&) {  // EnableSixAxisSensorFusion(bool, handle, aruid)
            const bool enable = req.pop<uint8_t>() != 0;
            std::lock_guard lock(s.mutex_);
            s.six_axis(req.pop<uint32_t>()).fusion = enable;
        });
        on(70, [&s](Request& req, Response&) {  // SetSixAxisSensorFusionParameters(handle, f32, f32, aruid)
            const uint32_t handle = req.pop<uint32_t>();
            const float p0 = req.pop<float>();
            const float p1 = req.pop<float>();
            std::lock_guard lock(s.mutex_);
            s.six_axis(handle).fusion_params[0] = p0;
            s.six_axis(handle).fusion_params[1] = p1;
        });
        on(71, [&s](Request& req, Response& resp) {  // GetSixAxisSensorFusionParameters(handle, aruid)
            std::lock_guard lock(s.mutex_);
            const HidState::SixAxis& sensor = s.six_axis(req.pop<uint32_t>());
            resp.push(sensor.fusion_params[0]);
            resp.push(sensor.fusion_params[1]);
        });
        on(72, [&s](Request& req, Response&) {  // ResetSixAxisSensorFusionParameters(handle, aruid)
            std::lock_guard lock(s.mutex_);
            HidState::SixAxis& sensor = s.six_axis(req.pop<uint32_t>());
            sensor.fusion_params[0] = HidState::SixAxis{}.fusion_params[0];
            sensor.fusion_params[1] = HidState::SixAxis{}.fusion_params[1];
        });
        on(79, [&s](Request& req, Response&) {  // SetGyroscopeZeroDriftMode(handle, mode, aruid)
            const uint32_t handle = req.pop<uint32_t>();
            const uint32_t mode = req.pop<uint32_t>();
            std::lock_guard lock(s.mutex_);
            s.six_axis(handle).zero_drift_mode = mode;
        });
        on(80, [&s](Request& req, Response& resp) {  // GetGyroscopeZeroDriftMode(handle, aruid) -> u32
            std::lock_guard lock(s.mutex_);
            resp.push<uint32_t>(s.six_axis(req.pop<uint32_t>()).zero_drift_mode);
        });
        on(81, [&s](Request& req, Response&) {  // ResetGyroscopeZeroDriftMode(handle, aruid)
            std::lock_guard lock(s.mutex_);
            s.six_axis(req.pop<uint32_t>()).zero_drift_mode = HidState::SixAxis{}.zero_drift_mode;
        });
        on(82, [&s](Request& req, Response& resp) {  // IsSixAxisSensorAtRest(handle, aruid) -> bool
            std::lock_guard lock(s.mutex_);
            const bool rest = s.is_at_rest(req.pop<uint32_t>());
            (rest ? s.log_.at_rest_yes : s.log_.at_rest_no)++;
            resp.push<uint8_t>(rest ? 1 : 0);
        });
        on(91, [&s](Request&, Response&) {  // ActivateGesture(u32, aruid)
            std::lock_guard lock(s.mutex_);
            s.activate_gesture();
        });
        on(100, [&s](Request& req, Response&) {  // SetSupportedNpadStyleSet(style_set, aruid)
            const uint32_t styles = req.pop<uint32_t>();
            std::lock_guard lock(s.mutex_);
            s.set_supported_styles(styles);
            s.refresh(req.kernel());
        });
        on(101, [&s](Request&, Response& resp) {  // GetSupportedNpadStyleSet(aruid) -> u32
            std::lock_guard lock(s.mutex_);
            resp.push<uint32_t>(s.supported_styles_);
        });
        on(102, [&s](Request& req, Response& resp) {  // SetSupportedNpadIdType(aruid, X ids)
            std::vector<uint8_t> raw;
            if (!req.read_in(0, &raw, 0x100)) {
                resp.result = rt::kernel_result::kInvalidPointer;
                return;
            }
            std::vector<uint32_t> ids(raw.size() / 4);
            std::memcpy(ids.data(), raw.data(), ids.size() * 4);
            std::lock_guard lock(s.mutex_);
            s.set_supported_ids(ids);
            s.refresh(req.kernel());
        });
        on(103, [&s](Request&, Response&) {  // ActivateNpad(aruid)
            std::lock_guard lock(s.mutex_);
            s.activate_npad(0);
        });
        on(104, [](Request&, Response&) {});  // DeactivateNpad(aruid)
        on(106, [this](Request& req, Response& resp) {  // AcquireNpadStyleSetUpdateEventHandle(id, aruid, u64)
            const int index = checked_npad(req, resp);
            if (index < 0)
                return;
            std::lock_guard lock(state_->mutex_);
            Event& ev = state_->style_event(static_cast<size_t>(index));
            Handle h = rt::kInvalidHandle;
            resp.result = ev.make_handle(req.kernel(), &h);
            if (resp.result.failed())
                return;
            resp.copy_handles.push_back(h);
            // Signaled on acquire, as in Ryujinx (whose run is the reference);
            // the SDK then reads the current style set.
            ev.signal(req.kernel());
        });
        on(107, [this](Request& req, Response& resp) {  // DisconnectNpad(id, aruid)
            const int index = checked_npad(req, resp);
            if (index < 0)
                return;
            std::lock_guard lock(state_->mutex_);
            state_->disconnect(static_cast<size_t>(index));
            state_->refresh(req.kernel());
        });
        on(109, [&s](Request& req, Response&) {  // ActivateNpadWithRevision(revision, aruid)
            const int32_t revision = req.pop<int32_t>();
            std::lock_guard lock(s.mutex_);
            s.activate_npad(revision);
        });
        on(120, [&s](Request& req, Response&) {  // SetNpadJoyHoldType(aruid, u64)
            req.pop<uint64_t>();
            const uint64_t type = req.pop<uint64_t>();
            std::lock_guard lock(s.mutex_);
            s.set_hold_type(static_cast<uint32_t>(type));
        });
        on(121, [&s](Request&, Response& resp) {  // GetNpadJoyHoldType(aruid) -> u64
            std::lock_guard lock(s.mutex_);
            resp.push<uint64_t>(s.hold_type_);
        });
        on(122, [this](Request& req, Response& resp) {  // SetNpadJoyAssignmentModeSingleByDefault(id, aruid)
            set_joy_mode(req, resp, 1);
        });
        on(123, [this](Request& req, Response& resp) {  // SetNpadJoyAssignmentModeSingle(id, aruid, u64 type)
            set_joy_mode(req, resp, 1);
        });
        on(124, [this](Request& req, Response& resp) {  // SetNpadJoyAssignmentModeDual(id, aruid)
            set_joy_mode(req, resp, 0);
        });
        on(128, [&s](Request& req, Response&) {  // SetNpadHandheldActivationMode(aruid, u64)
            req.pop<uint64_t>();
            const uint64_t mode = req.pop<uint64_t>();
            std::lock_guard lock(s.mutex_);
            s.set_handheld_activation(static_cast<uint32_t>(mode));
        });
        on(129, [&s](Request&, Response& resp) {  // GetNpadHandheldActivationMode(aruid) -> u64
            std::lock_guard lock(s.mutex_);
            resp.push<uint64_t>(s.handheld_activation());
        });
        on(200, [](Request& req, Response& resp) {  // GetVibrationDeviceInfo(handle) -> {type, position}
            const DeviceHandle h = decode_handle(req.pop<uint32_t>());
            uint32_t position = 0;
            switch (h.style_index) {
            case 3:
            case 4:
            case 5: position = h.device == 0 ? 1 : h.device == 1 ? 2 : 0; break;
            case 6: position = 1; break;
            case 7: position = 2; break;
            default:
                rt::log(LogLevel::Error, "hid: vibration device of style index %u not implemented", h.style_index);
                resp.unimplemented = true;
                return;
            }
            resp.push<uint32_t>(1);  // LinearResonantActuator
            resp.push<uint32_t>(position);
        });
        on(201, [&s](Request& req, Response&) {  // SendVibrationValue(handle, value, aruid)
            const uint32_t handle = req.pop<uint32_t>();
            float v[4];  // amp_low, freq_low, amp_high, freq_high
            for (float& f : v)
                f = req.pop<float>();
            std::lock_guard lock(s.mutex_);
            s.vibrate(201, handle, v);
        });
        on(203, [](Request&, Response& resp) {  // CreateActiveVibrationDeviceList
            resp.push_object(std::make_shared<HidActiveVibrationDeviceList>());
        });
        on(206, [&s](Request& req, Response& resp) {  // SendVibrationValues(aruid, X handles, X values)
            std::vector<uint8_t> handles, values;
            if (!req.read_in(0, &handles, 0x100) || !req.read_in(1, &values, 0x400)) {
                resp.result = rt::kernel_result::kInvalidPointer;
                return;
            }
            const size_t count = std::min(handles.size() / 4, values.size() / 16);
            std::lock_guard lock(s.mutex_);
            for (size_t i = 0; i < count; ++i) {
                uint32_t handle = 0;
                float v[4];
                std::memcpy(&handle, handles.data() + 4 * i, 4);
                std::memcpy(v, values.data() + 16 * i, 16);
                s.vibrate(206, handle, v);  // amp_low, freq_low, amp_high, freq_high
            }
        });
        on(522, [&s](Request& req, Response&) {  // SetIsPalmaAllConnectable(bool, aruid)
            const bool v = req.pop<uint8_t>() != 0;
            std::lock_guard lock(s.mutex_);
            s.set_palma_all_connectable(v);
        });
        on(525, [&s](Request& req, Response&) {  // SetPalmaBoostMode(bool)
            const bool v = req.pop<uint8_t>() != 0;
            std::lock_guard lock(s.mutex_);
            s.set_palma_boost_mode(v);
        });
    }

private:
    // Pops the npad id; an id outside No1..No8/Other/Handheld stops the guest
    // (the console's answer to that is not documented).
    static int checked_npad(Request& req, Response& resp) {
        const uint32_t id = req.pop<uint32_t>();
        const int index = HidState::npad_index(id);
        if (index < 0) {
            rt::log(LogLevel::Error, "hid: invalid npad id 0x%x in command %u", id, req.command());
            resp.unimplemented = true;
        }
        return index;
    }
    void set_joy_mode(Request& req, Response& resp, uint32_t mode) {
        const int index = checked_npad(req, resp);
        if (index < 0)
            return;
        rt::log(LogLevel::Debug, "hid: npad %d joy assignment %s (command %u)", index, mode == 1 ? "single" : "dual",
                req.command());
        std::lock_guard lock(state_->mutex_);
        state_->set_joy_mode(static_cast<size_t>(index), mode);
        state_->refresh(req.kernel());
    }

    std::shared_ptr<HidState> state_;
};

void set_touch_scale(float sx, float sy) {
    g_touch_sx.store(sx > 0 ? sx : 1.0f, std::memory_order_relaxed);
    g_touch_sy.store(sy > 0 ? sy : 1.0f, std::memory_order_relaxed);
}

std::shared_ptr<HidState> register_hid(ServiceManager& sm, const HidConfig& config) {
    auto state = std::make_shared<HidState>(config);
    sm.register_service("hid", [state] { return std::make_shared<HidServer>(state); });
    return state;
}

}  // namespace lgp::hle
