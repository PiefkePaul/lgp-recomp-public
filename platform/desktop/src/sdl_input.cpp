// SDL3 gamepads and keyboard as IInput, see lgp/platform/desktop/sdl_input.hpp.
#include "lgp/platform/desktop/sdl_input.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>

namespace lgp::platform::desktop {

namespace {

constexpr float kTwoPi = 6.28318530718f;

float axis(int16_t v) { return std::max(-1.0f, static_cast<float>(v) / 32767.0f); }

// SDL sensor frame (x right, y up, z towards the player; gravity +y when
// held upright) to the Switch's (lying face up at rest: gravity -z). The
// mapping is an assumption until checked against the real game.
void to_switch_frame(const float* sdl, float* out, float scale) {
    out[0] = -sdl[0] * scale;
    out[1] = sdl[2] * scale;
    out[2] = -sdl[1] * scale;
}

#if defined(SDL_PLATFORM_ANDROID)
// Keys of TV remotes (Android TV, NVIDIA Shield) that SDL leaves as a
// keyboard. A remote with a D-pad source usually becomes an SDL gamepad
// without right stick and shoulders instead (SDL_HINT_TV_REMOTE_AS_JOYSTICK,
// Pad::remote), whose
// automatic mapping already makes centre, Back and Menu A, B and Start. As
// keys, the D-pad arrives as arrow keys; its centre (Enter), Back and Menu
// become A, B and Plus. A keyboard's Enter is A here as well. Holding Menu
// also opens the app's pause (AppMenu), since such a remote has no Home key.
uint64_t remote_button(SDL_Scancode code) {
    switch (code) {
    case SDL_SCANCODE_RETURN: return kButtonA;
    case SDL_SCANCODE_AC_BACK: return kButtonB;
    case SDL_SCANCODE_MENU: return kButtonPlus;
    default: return 0;
    }
}
#endif

// A device sensor sample (SDL: x right, y up, z out of the screen, in
// portrait) to the frame of a controller held in front of the player, as
// gamepads report it, for the landscape the display is in. LANDSCAPE has the
// device's right side up, LANDSCAPE_FLIPPED its left side.
void device_to_held(const float* d, int orientation, float* out) {
    switch (orientation) {
    case SDL_ORIENTATION_LANDSCAPE:
        out[0] = -d[1], out[1] = d[0], out[2] = d[2];
        break;
    case SDL_ORIENTATION_LANDSCAPE_FLIPPED:
        out[0] = d[1], out[1] = -d[0], out[2] = d[2];
        break;
    case SDL_ORIENTATION_PORTRAIT_FLIPPED:
        out[0] = -d[0], out[1] = -d[1], out[2] = d[2];
        break;
    default:
        out[0] = d[0], out[1] = d[1], out[2] = d[2];
        break;
    }
}

bool single_joycon(const MapperConfig& c) {
    return c.style == ControllerStyle::JoyconLeft || c.style == ControllerStyle::JoyconRight;
}

}  // namespace

std::unique_ptr<SdlInput> SdlInput::create(const SdlInputOptions& options, std::string* error) {
    // A single Joy-Con upright, as LGP holds it: SDL's default turns it
    // sideways into a mini gamepad, which rotates the face buttons (X arrives
    // as A) and swaps the sensor axes x/z (Paul 2026-10-22, real right
    // Joy-Con on the PC). Pairs are not affected. The environment wins.
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_VERTICAL_JOY_CONS, "1");
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        *error = std::string("SDL gamepad: ") + SDL_GetError();
        return nullptr;
    }
    std::unique_ptr<SdlInput> in(new SdlInput(options));
    // Pads already plugged in also arrive as SDL_EVENT_GAMEPAD_ADDED; opening
    // them here makes them usable before the first event pump.
    int count = 0;
    if (SDL_JoystickID* ids = SDL_GetGamepads(&count)) {
        for (int i = 0; i < count; ++i)
            in->add(ids[i]);
        SDL_free(ids);
    }
    if (options.motion && options.device_motion)
        in->open_device_sensors();
    return in;
}

void SdlInput::open_device_sensors() {
    if (!SDL_InitSubSystem(SDL_INIT_SENSOR)) {
        SDL_Log("lgp: device sensors: %s", SDL_GetError());
        return;
    }
    int count = 0;
    if (SDL_SensorID* ids = SDL_GetSensors(&count)) {
        for (int i = 0; i < count; ++i) {
            const SDL_SensorType type = SDL_GetSensorTypeForID(ids[i]);
            SDL_Sensor** slot = type == SDL_SENSOR_GYRO ? &device_gyro_ : type == SDL_SENSOR_ACCEL ? &device_accel_ : nullptr;
            if (slot != nullptr && *slot == nullptr)
                *slot = SDL_OpenSensor(ids[i]);
        }
        SDL_free(ids);
    }
    if (device_gyro_ == nullptr || device_accel_ == nullptr) {
        if (device_gyro_ != nullptr)
            SDL_CloseSensor(device_gyro_);
        if (device_accel_ != nullptr)
            SDL_CloseSensor(device_accel_);
        device_gyro_ = device_accel_ = nullptr;
        SDL_QuitSubSystem(SDL_INIT_SENSOR);
        return;
    }
    update_orientation();
    SDL_Log("lgp: device gyro and accelerometer -> motion of slot 0 without a gamepad");
}

void SdlInput::update_orientation() {
    orientation_ = SDL_GetCurrentDisplayOrientation(SDL_GetPrimaryDisplay());
    if (orientation_ == SDL_ORIENTATION_UNKNOWN)
        orientation_ = SDL_ORIENTATION_LANDSCAPE;  // the app runs in landscape
}

SdlInput::SdlInput(const SdlInputOptions& options)
    : PadInput(options.slots), options_(options), pick_(options.player1) {
    touch_.set_controls(options_.touch);
    update_swipe_action();
}

void SdlInput::set_player1_controller(const MapperConfig& config) {
    {
        std::lock_guard lock(state_mutex_);
        if (options_.slots.empty())
            return;
        options_.slots[0] = config;
        update_swipe_action();
    }
    set_slot(0, config);  // the mappers' lock, never held together with state_mutex_ here
}

void SdlInput::set_touch_throw(bool on, bool with_gamepad) {
    std::lock_guard lock(state_mutex_);
    options_.touch_throw = on;
    options_.touch_throw_with_gamepad = with_gamepad;
    update_swipe_action();
}

void SdlInput::set_touch_only_scene(bool on) {
    std::lock_guard lock(state_mutex_);
    if (on == touch_only_scene_)
        return;
    touch_only_scene_ = on;
    if (on)
        hidden_before_scene_ = touch_.hidden();
    touch_.set_hidden(on || hidden_before_scene_);
    SDL_Log("lgp: touch: scene with touches only %s, on-screen controls %s", on ? "began" : "ended",
            touch_.hidden() ? "hidden" : "shown");
}

void SdlInput::update_swipe_action() {
    // A swipe throws like the game expects it from player 1's controller:
    // the throw gesture of a single Joy-Con, A in handheld mode. While a
    // gamepad plays player 1 only if the settings say so (Paul, 2026-10-21:
    // the hands are on the pad, a stray swipe would throw). A pad plays once
    // a button on it was pressed; one that is only paired does not.
    const Pad* pad = player_pad(0);
    const bool swipe = options_.touch_throw && !options_.slots.empty() &&
                       (options_.touch_throw_with_gamepad || pad == nullptr || !pad->used);
    touch_.set_swipe_action(!swipe                              ? TouchPad::SwipeAction::None
                            : single_joycon(options_.slots[0]) ? TouchPad::SwipeAction::Throw
                                                               : TouchPad::SwipeAction::PressA);
}

bool SdlInputOptions::default_touch() {
#if defined(SDL_PLATFORM_ANDROID)
    return !SDL_IsTV();  // Android TV has no touch screen
#elif defined(SDL_PLATFORM_TVOS)
    return false;  // Apple TV: no touch screen (SDL_PLATFORM_IOS is set there too)
#elif defined(SDL_PLATFORM_IOS)
    return true;
#else
    return false;
#endif
}

bool SdlInputOptions::default_tv_remotes() {
#if defined(SDL_PLATFORM_ANDROID)
    return SDL_IsTV();
#elif defined(SDL_PLATFORM_TVOS)
    return true;  // the Siri Remote is always there, usually before any controller
#else
    return false;
#endif
}

void SdlInput::set_touch_aspect(float aspect) {
    std::lock_guard lock(state_mutex_);
    touch_.set_aspect(aspect);
}

void SdlInput::set_touch_window_points(float height) {
    std::lock_guard lock(state_mutex_);
    touch_.set_window_points(height);
}

void SdlInput::set_touch_insets(const SafeInsets& insets) {
    std::lock_guard lock(state_mutex_);
    touch_.set_insets(insets);
}

void SdlInput::set_touch_image_mapper(TouchPad::ImageMapper mapper) {
    std::lock_guard lock(state_mutex_);
    touch_.set_image_mapper(std::move(mapper));
}

void SdlInput::set_touch_layout(float scale, float opacity, bool mirrored) {
    std::lock_guard lock(state_mutex_);
    touch_.set_scale(scale);
    touch_.set_opacity(opacity);
    touch_.set_mirrored(mirrored);
}

void SdlInput::set_touch_enabled(bool enabled) {
    std::lock_guard lock(state_mutex_);
    if (!enabled)
        touch_.release_all();
    touch_.set_controls(enabled);
    options_.touch = enabled;
}

std::vector<OverlayCircle> SdlInput::touch_overlay() const {
    std::lock_guard lock(state_mutex_);
    if (!options_.touch)
        return {};
    // With a controller the overlay goes away until the screen is touched again.
    if (!pads_.empty() && (last_touch_ms_ == 0 || SDL_GetTicks() - last_touch_ms_ > 10000))
        return {};
    return touch_.overlay();
}

SdlInput::~SdlInput() {
    for (Pad& p : pads_)
        SDL_CloseGamepad(p.gamepad);
    SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    if (device_gyro_ != nullptr) {
        SDL_CloseSensor(device_gyro_);
        SDL_CloseSensor(device_accel_);
        SDL_QuitSubSystem(SDL_INIT_SENSOR);
    }
}

SdlInput::Pad* SdlInput::find(uint32_t id) {
    for (Pad& p : pads_)
        if (p.id == id)
            return &p;
    return nullptr;
}

SdlInput::Pad* SdlInput::slot_pad(size_t slot) {
    for (Pad& p : pads_)
        if (!p.remote && slot-- == 0)
            return &p;
    return nullptr;
}

SdlInput::Pad* SdlInput::player_pad(size_t slot) {
    return slot < slot_ids_.size() ? find(slot_ids_[slot]) : nullptr;
}

void SdlInput::pick_player1() {
    std::lock_guard lock(state_mutex_);
    pick_.repick();
    SDL_Log("lgp: player 1: the next button press on a gamepad chooses");
}

void SdlInput::set_player1_mode(PlayerPick::Mode mode) {
    std::lock_guard lock(state_mutex_);
    pick_.set_mode(mode);
}

std::string SdlInput::player1_name() const {
    std::lock_guard lock(state_mutex_);
    for (const Pad& p : pads_)
        if (!slot_ids_.empty() && p.id == slot_ids_[0])
            return p.name;
    return {};
}

void SdlInput::npad_disconnected(size_t index) {
    if (index != 0)
        return;
    std::lock_guard lock(state_mutex_);
    if (pick_.mode() == PlayerPick::Mode::Press) {
        pick_.repick();
        SDL_Log("lgp: player 1: the game disconnected slot 0, the next button press on a gamepad chooses");
    }
}

void SdlInput::add(uint32_t id) {
    std::lock_guard lock(state_mutex_);
    if (find(id) != nullptr)
        return;
    SDL_Gamepad* g = SDL_OpenGamepad(id);
    if (g == nullptr)
        return;
    Pad p;
    p.id = id;
    p.gamepad = g;
    const char* name = SDL_GetGamepadName(g);
    p.name = name != nullptr ? name : "gamepad";
    p.state.connected = true;
    // A remote has neither a right stick nor shoulder buttons. A single Joy-Con
    // reports SL/SR as paddles, so they count as shoulders here too.
    p.remote = options_.tv_remotes && !SDL_GamepadHasAxis(g, SDL_GAMEPAD_AXIS_RIGHTX) &&
               !SDL_GamepadHasButton(g, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) &&
               !SDL_GamepadHasButton(g, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) &&
               !SDL_GamepadHasButton(g, SDL_GAMEPAD_BUTTON_LEFT_PADDLE1) &&
               !SDL_GamepadHasButton(g, SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1);
    if (options_.motion && SDL_GamepadHasSensor(g, SDL_SENSOR_GYRO) && SDL_GamepadHasSensor(g, SDL_SENSOR_ACCEL)) {
        p.has_motion = SDL_SetGamepadSensorEnabled(g, SDL_SENSOR_GYRO, true) &&
                       SDL_SetGamepadSensorEnabled(g, SDL_SENSOR_ACCEL, true);
    }
    if (p.remote) {
        SDL_Log("lgp: gamepad %u \"%s\" is a TV remote: it joins slot 0", id, p.name.c_str());
    } else {
        const size_t slot = static_cast<size_t>(std::count_if(pads_.begin(), pads_.end(), [](const Pad& q) { return !q.remote; }));
        SDL_Log("lgp: gamepad %u \"%s\" -> slot %zu%s", id, p.name.c_str(), slot, p.has_motion ? " (motion)" : "");
    }
    if (options_.log) {  // what the device offers (tvOS: a controller sent no events, 15.10.2026)
        SDL_Joystick* joy = SDL_GetGamepadJoystick(g);
        SDL_Log("lgp: gamepad %u: type %s, vendor %04x product %04x, %d axes %d buttons, connection %d",
                id, SDL_GetGamepadStringForType(SDL_GetGamepadType(g)), SDL_GetGamepadVendor(g),
                SDL_GetGamepadProduct(g), joy != nullptr ? SDL_GetNumJoystickAxes(joy) : -1,
                joy != nullptr ? SDL_GetNumJoystickButtons(joy) : -1, int(SDL_GetGamepadConnectionState(g)));
    }
    pads_.push_back(std::move(p));
}

void SdlInput::set_button(Pad& pad, int sdl_button, bool down) {
    uint64_t bit = 0;
    switch (sdl_button) {
    case SDL_GAMEPAD_BUTTON_SOUTH:
    case SDL_GAMEPAD_BUTTON_EAST:
    case SDL_GAMEPAD_BUTTON_WEST:
    case SDL_GAMEPAD_BUTTON_NORTH:
        switch (SDL_GetGamepadButtonLabel(pad.gamepad, static_cast<SDL_GamepadButton>(sdl_button))) {
        case SDL_GAMEPAD_BUTTON_LABEL_A:
        case SDL_GAMEPAD_BUTTON_LABEL_CROSS: bit = kButtonA; break;
        case SDL_GAMEPAD_BUTTON_LABEL_B:
        case SDL_GAMEPAD_BUTTON_LABEL_CIRCLE: bit = kButtonB; break;
        case SDL_GAMEPAD_BUTTON_LABEL_X:
        case SDL_GAMEPAD_BUTTON_LABEL_SQUARE: bit = kButtonX; break;
        case SDL_GAMEPAD_BUTTON_LABEL_Y:
        case SDL_GAMEPAD_BUTTON_LABEL_TRIANGLE: bit = kButtonY; break;
        default:  // no label: Xbox positions
            bit = sdl_button == SDL_GAMEPAD_BUTTON_SOUTH  ? kButtonA
                  : sdl_button == SDL_GAMEPAD_BUTTON_EAST ? kButtonB
                  : sdl_button == SDL_GAMEPAD_BUTTON_WEST ? kButtonX
                                                          : kButtonY;
            break;
        }
        break;
    case SDL_GAMEPAD_BUTTON_BACK: bit = kButtonMinus; break;
    case SDL_GAMEPAD_BUTTON_START: bit = kButtonPlus; break;
    case SDL_GAMEPAD_BUTTON_LEFT_STICK: bit = kButtonStickL; break;
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK: bit = kButtonStickR; break;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:
    case SDL_GAMEPAD_BUTTON_LEFT_PADDLE1: bit = kButtonL; break;  // single Joy-Con SL
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER:
    case SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1: bit = kButtonR; break;  // single Joy-Con SR
    case SDL_GAMEPAD_BUTTON_DPAD_UP: bit = kButtonUp; break;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: bit = kButtonDown; break;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: bit = kButtonLeft; break;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: bit = kButtonRight; break;
    default: return;  // guide, capture, other paddles
    }
    if (down)
        pad.state.buttons |= bit;
    else
        pad.state.buttons &= ~bit;
}

void SdlInput::handle_event(const SDL_Event& e) {
    if (options_.log) {
        // When the app leaves the foreground, and which keys UIKit/Android
        // sent as keys (tvOS: Menu on the remote left the app, 15.10.2026).
        const char* what = nullptr;
        switch (e.type) {
        case SDL_EVENT_WILL_ENTER_BACKGROUND: what = "will enter background"; break;
        case SDL_EVENT_DID_ENTER_BACKGROUND: what = "in the background"; break;
        case SDL_EVENT_DID_ENTER_FOREGROUND: what = "in the foreground"; break;
        case SDL_EVENT_WINDOW_FOCUS_LOST: what = "window focus lost"; break;
        case SDL_EVENT_WINDOW_FOCUS_GAINED: what = "window focus gained"; break;
        default: break;
        }
        if (what != nullptr)
            SDL_Log("lgp: input: %s at %llu ms", what, static_cast<unsigned long long>(SDL_GetTicks()));
        if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat)
            SDL_Log("lgp: input: key %s down at %llu ms", SDL_GetScancodeName(e.key.scancode),
                    static_cast<unsigned long long>(SDL_GetTicks()));
    }
    switch (e.type) {
    case SDL_EVENT_GAMEPAD_ADDED:
        add(e.gdevice.which);
        return;
    default:
        break;
    }
    std::lock_guard lock(state_mutex_);
    switch (e.type) {
    case SDL_EVENT_GAMEPAD_REMOVED:
        for (auto it = pads_.begin(); it != pads_.end(); ++it) {
            if (it->id == e.gdevice.which) {
                SDL_Log("lgp: gamepad %u \"%s\" removed", it->id, it->name.c_str());
                SDL_CloseGamepad(it->gamepad);
                pads_.erase(it);  // later pads move up one slot
                break;
            }
        }
        break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
        if (Pad* p = find(e.gbutton.which)) {
            set_button(*p, e.gbutton.button, e.gbutton.down);
            p->used |= e.gbutton.down;  // also a press that ends before the next sample()
            count_event(*p, e.gbutton.timestamp, &Pad::Stats::button);
        }
        break;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        if (Pad* p = find(e.gaxis.which)) {
            count_event(*p, e.gaxis.timestamp, &Pad::Stats::axis);
            switch (e.gaxis.axis) {
            case SDL_GAMEPAD_AXIS_LEFTX: p->state.left_x = axis(e.gaxis.value); break;
            case SDL_GAMEPAD_AXIS_LEFTY: p->state.left_y = -axis(e.gaxis.value); break;  // SDL: down positive
            case SDL_GAMEPAD_AXIS_RIGHTX: p->state.right_x = axis(e.gaxis.value); break;
            case SDL_GAMEPAD_AXIS_RIGHTY: p->state.right_y = -axis(e.gaxis.value); break;
            case SDL_GAMEPAD_AXIS_LEFT_TRIGGER: p->state.zl = std::max(0.0f, axis(e.gaxis.value)); break;
            case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER: p->state.zr = std::max(0.0f, axis(e.gaxis.value)); break;
            default: break;
            }
            p->trigger_peak[0] = std::max(p->trigger_peak[0], p->state.zl);
            p->trigger_peak[1] = std::max(p->trigger_peak[1], p->state.zr);
            p->used |= p->trigger_peak[0] >= 0.5f || p->trigger_peak[1] >= 0.5f;  // ZL/ZR as in sample()
        }
        break;
    case SDL_EVENT_GAMEPAD_SENSOR_UPDATE:
        if (Pad* p = find(e.gsensor.which); p != nullptr && p->has_motion) {
            count_event(*p, e.gsensor.timestamp, &Pad::Stats::sensor);
            if (e.gsensor.sensor == SDL_SENSOR_ACCEL)
                to_switch_frame(e.gsensor.data, p->state.motion.accel, 1.0f / SDL_STANDARD_GRAVITY);
            else if (e.gsensor.sensor == SDL_SENSOR_GYRO)
                to_switch_frame(e.gsensor.data, p->state.motion.gyro, 1.0f / kTwoPi);
            p->state.motion.valid = true;
        }
        break;
    case SDL_EVENT_SENSOR_UPDATE:
        if (device_gyro_ != nullptr) {
            const bool gyro = e.sensor.which == SDL_GetSensorID(device_gyro_);
            if (!gyro && e.sensor.which != SDL_GetSensorID(device_accel_))
                break;
            float held[3];
            device_to_held(e.sensor.data, orientation_, held);
            if (gyro)
                to_switch_frame(held, device_motion_.gyro, 1.0f / kTwoPi);
            else
                to_switch_frame(held, device_motion_.accel, 1.0f / SDL_STANDARD_GRAVITY);
            device_motion_.valid = true;
        }
        break;
    case SDL_EVENT_DISPLAY_ORIENTATION:
        if (device_gyro_ != nullptr)
            update_orientation();
        break;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
#if defined(SDL_PLATFORM_ANDROID)
        if (const uint64_t bit = remote_button(e.key.scancode); bit != 0) {
            remote_ = e.key.down ? remote_ | bit : remote_ & ~bit;
            break;
        }
#endif
        if (e.key.scancode < keys_.size())
            keys_[e.key.scancode] = e.key.down;
        break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        keys_.fill(false);  // key-ups go to the window that has the focus
        remote_ = 0;
        touch_.release_all();
        break;
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        if (e.window.data2 > 0) {
            touch_.set_aspect(static_cast<float>(e.window.data1) / static_cast<float>(e.window.data2));
            if (SDL_Window* window = SDL_GetWindowFromID(e.window.windowID))
                if (const float scale = SDL_GetWindowDisplayScale(window); scale > 0)
                    touch_.set_window_points(static_cast<float>(e.window.data2) / scale);
        }
        break;
    case SDL_EVENT_WINDOW_SAFE_AREA_CHANGED:
        if (SDL_Window* window = SDL_GetWindowFromID(e.window.windowID)) {
            SDL_Rect safe{};
            int w = 0, h = 0;
            if (SDL_GetWindowSafeArea(window, &safe) && SDL_GetWindowSize(window, &w, &h))
                touch_.set_insets(safe_insets_from_rect(float(safe.x), float(safe.y), float(safe.w), float(safe.h),
                                                        float(w), float(h)));
        }
        break;
    case SDL_EVENT_FINGER_DOWN:
    case SDL_EVENT_FINGER_MOTION:
    case SDL_EVENT_FINGER_UP:
    case SDL_EVENT_FINGER_CANCELED:
        // Without the on-screen controller fingers still reach the game's
        // touch screen (TouchPad::set_controls).
        if (options_.touch)
            last_touch_ms_ = SDL_GetTicks();
        if (e.type == SDL_EVENT_FINGER_DOWN)
            touch_.finger_down(e.tfinger.fingerID, e.tfinger.x, e.tfinger.y, e.tfinger.timestamp);
        else if (e.type == SDL_EVENT_FINGER_MOTION)
            touch_.finger_move(e.tfinger.fingerID, e.tfinger.x, e.tfinger.y, e.tfinger.timestamp);
        else
            touch_.finger_up(e.tfinger.fingerID, e.type == SDL_EVENT_FINGER_CANCELED);
        if (options_.log && e.type != SDL_EVENT_FINGER_MOTION) {
            // Where each finger lands on the game's touch screen (a small
            // target missed: wrong mapping, an overlay button, the system).
            TouchPoint p;
            const bool inside = touch_.map_to_game(e.tfinger.x, e.tfinger.y, &p);
            SDL_Log("lgp: touch: %s finger %" SDL_PRIu64 " window %.3f,%.3f -> game %.0f,%.0f%s%s",
                    e.type == SDL_EVENT_FINGER_DOWN ? "down" : e.type == SDL_EVENT_FINGER_UP ? "up" : "cancel",
                    static_cast<Uint64>(e.tfinger.fingerID), e.tfinger.x, e.tfinger.y, p.x, p.y,
                    inside ? "" : " (off the image)", touch_.hidden() ? " (controls hidden)" : "");
        }
        break;
    default:
        break;
    }
}

void SdlInput::count_event(Pad& pad, uint64_t timestamp_ns, uint32_t Pad::Stats::*counter) {
    if (!options_.log)
        return;
    Pad::Stats& st = pad.stats;
    ++(st.*counter);
    if (st.last_event_ns != 0 && timestamp_ns > st.last_event_ns)
        st.max_event_gap_ns = std::max(st.max_event_gap_ns, timestamp_ns - st.last_event_ns);
    st.last_event_ns = timestamp_ns;
}

void SdlInput::service() {
    std::lock_guard lock(state_mutex_);
    const uint64_t now = SDL_GetTicks();
    for (Pad& p : pads_) {
        const float want[2] = {std::max(p.rumble[0], p.peak[0]), std::max(p.rumble[1], p.peak[1])};
        p.peak[0] = p.peak[1] = 0;
        // SDL rumble runs for a duration; renew it while the game keeps it on.
        const bool changed = want[0] != p.sent[0] || want[1] != p.sent[1];
        const bool on = want[0] > 0 || want[1] > 0;
        if (!changed && !(on && now - p.sent_ms > 150))
            continue;
        if (!SDL_RumbleGamepad(p.gamepad, static_cast<uint16_t>(std::lround(want[0] * 0xFFFF)),
                               static_cast<uint16_t>(std::lround(want[1] * 0xFFFF)), on ? 250 : 0) &&
            !p.rumble_failed) {
            p.rumble_failed = true;  // once per pad: some drivers have no motors
            SDL_Log("lgp: gamepad %u \"%s\": rumble failed: %s", p.id, p.name.c_str(), SDL_GetError());
        }
        p.sent[0] = want[0];
        p.sent[1] = want[1];
        p.sent_ms = now;
        ++p.stats.rumble_writes;
        p.stats.max_rumble[0] = std::max(p.stats.max_rumble[0], want[0]);
        p.stats.max_rumble[1] = std::max(p.stats.max_rumble[1], want[1]);
    }
    if (!options_.log)
        return;
    if (last_service_ms_ != 0)
        max_service_gap_ms_ = std::max(max_service_gap_ms_, now - last_service_ms_);
    last_service_ms_ = now;
    if (log_start_ms_ == 0)
        log_start_ms_ = now;
    if (now - log_start_ms_ < 1000)
        return;
    for (size_t i = 0; i < pads_.size(); ++i) {
        Pad& p = pads_[i];
        Pad::Stats& st = p.stats;
        SDL_Log("lgp: input 1s pad %zu \"%s\": events axis %u sensor %u button %u, max event gap %.1f ms, "
                "stick drops left %u right %u, left stick max %.3f held min %.3f, rumble writes %u (max %.2f/%.2f), main loop max gap %llu ms",
                i, p.name.c_str(), st.axis, st.sensor, st.button, static_cast<double>(st.max_event_gap_ns) * 1e-6,
                st.left.drops, st.right.drops, st.left_max, st.left_held_min > 1.5f ? 0.0f : st.left_held_min,
                st.rumble_writes, st.max_rumble[0], st.max_rumble[1],
                static_cast<unsigned long long>(max_service_gap_ms_));
        const uint64_t last = st.last_event_ns;
        st.axis = st.sensor = st.button = st.rumble_writes = 0;
        st.max_event_gap_ns = 0;
        st.last_event_ns = last;
        st.max_rumble[0] = st.max_rumble[1] = 0;
        st.left.drops = st.right.drops = 0;
        st.left_max = 0;
        st.left_held_min = 2;
    }
    log_start_ms_ = now;
    max_service_gap_ms_ = 0;
}

size_t SdlInput::gamepad_count() const {
    std::lock_guard lock(state_mutex_);
    return pads_.size();
}

void SdlInput::sample(std::vector<HostPad>& pads) {
    std::lock_guard lock(state_mutex_);
    const uint64_t now_ns = options_.log ? SDL_GetTicksNS() : 0;
    // Slot order: the pads without remotes, the one chosen as player 1 first.
    std::vector<uint64_t> ids, held;
    for (size_t i = 0; Pad* pad = slot_pad(i); ++i) {
        ids.push_back(pad->id);
        // Analog triggers count as ZL/ZR (the mapper's default threshold).
        // A trigger pressed and released since the last sample counts as held.
        const float zl = std::max(pad->state.zl, pad->trigger_peak[0]);
        const float zr = std::max(pad->state.zr, pad->trigger_peak[1]);
        held.push_back(pad->state.buttons | (zl >= 0.5f ? kButtonZL : 0) | (zr >= 0.5f ? kButtonZR : 0));
        pad->used |= held.back() != 0;
    }
    std::vector<uint32_t> order;
    for (size_t k : pick_.update(ids, held))
        order.push_back(static_cast<uint32_t>(ids[k]));
    if (order != slot_ids_) {
        slot_ids_ = std::move(order);
        // Vibration follows the slot to its new pad; a pad without a slot stops.
        for (Pad& p : pads_)
            p.rumble[0] = p.rumble[1] = 0;
        for (size_t i = 0; i < slot_rumble_.size() && i < slot_ids_.size(); ++i)
            if (Pad* p = find(slot_ids_[i])) {
                p->rumble[0] = slot_rumble_[i][0];
                p->rumble[1] = slot_rumble_[i][1];
            }
    }
    if (pick_.player1() != logged_player1_) {
        logged_player1_ = static_cast<uint32_t>(pick_.player1());
        if (const Pad* p = find(logged_player1_))
            SDL_Log("lgp: player 1: gamepad %u \"%s\" (button press), slot 0", p->id, p->name.c_str());
    }
    for (size_t i = 0; i < pads.size(); ++i) {
        Pad* pad = player_pad(i);
        if (pad == nullptr)
            break;
        pads[i] = pad->state;
        pads[i].device = pad->id;
        // Pressed and released since the last sample: the peak, for the mapper's threshold.
        pads[i].zl = std::max(pads[i].zl, pad->trigger_peak[0]);
        pads[i].zr = std::max(pads[i].zr, pad->trigger_peak[1]);
        if (options_.log) {  // the raw SDL stick, at hid's sampling tick
            const HostPad& st = pad->state;
            const float left_len = std::hypot(st.left_x, st.left_y);
            Pad::Stats& stats = pad->stats;
            stats.left.sample(left_len, now_ns);
            stats.left_max = std::max(stats.left_max, left_len);
            if (left_len >= 0.5f)
                stats.left_held_min = std::min(stats.left_held_min, left_len);
            stats.right.sample(std::hypot(st.right_x, st.right_y), now_ns);
        }
        // A single Joy-Con without a gyro throws with the right stick click.
        if (single_joycon(options_.slots[i]) && !pad->has_motion && (pads[i].buttons & kButtonStickR)) {
            pads[i].buttons &= ~kButtonStickR;
            pads[i].throw_gesture = true;
        }
    }
    for (Pad& p : pads_)
        p.trigger_peak[0] = p.trigger_peak[1] = 0;
    if (!pads.empty()) {
        for (const Pad& p : pads_) {
            if (p.remote) {
                pads[0].buttons |= p.state.buttons;
                pads[0].connected = true;
                // The Siri Remote's touch surface is a stick; the farther one wins.
                if (std::hypot(p.state.left_x, p.state.left_y) > std::hypot(pads[0].left_x, pads[0].left_y)) {
                    pads[0].left_x = p.state.left_x;
                    pads[0].left_y = p.state.left_y;
                }
            }
        }
    }
    if (options_.keyboard && !pads.empty()) {
        options_.keys.apply(keys_.data(), &pads[0]);
        pads[0].buttons |= remote_;
        pads[0].connected = true;
    }
    // The device's own sensors move player 1 while no gamepad holds slot 0.
    if (device_motion_.valid && !pads.empty() && slot_pad(0) == nullptr && !pads[0].motion.valid)
        pads[0].motion = device_motion_;
    if (options_.touch && !pads.empty()) {
        update_swipe_action();  // a gamepad may have come or gone
        float side = 0, strength = 0, reach = 0;
        if (options_.log && touch_.pending_swipe(&side, &strength, &reach)) {
            const TouchPad::SwipeAction action = touch_.swipe_action();
            SDL_Log("lgp: touch: swipe side %.2f strength %.2f reach %.2f -> %s", side, strength, reach,
                    action == TouchPad::SwipeAction::Throw    ? "throw gesture (single Joy-Con)"
                    : action == TouchPad::SwipeAction::PressA ? "A (handheld or full controller)"
                                                              : "nothing");
        }
        touch_.apply(&pads[0], SDL_GetTicksNS());  // same clock as the event timestamps
    }
}

void SdlInput::sample_touch(TouchScreenState& touch) {
    std::lock_guard lock(state_mutex_);
    touch_.take_touches(&touch);
}

void SdlInput::rumble(size_t slot, float low, float high) {
    std::lock_guard lock(state_mutex_);
    if (slot >= slot_rumble_.size())
        slot_rumble_.resize(slot + 1, {0.0f, 0.0f});
    slot_rumble_[slot] = {low, high};
    if (Pad* pad = player_pad(slot)) {
        Pad& p = *pad;
        p.rumble[0] = low;
        p.rumble[1] = high;
        p.peak[0] = std::max(p.peak[0], low);
        p.peak[1] = std::max(p.peak[1], high);
    }
}

}  // namespace lgp::platform::desktop
