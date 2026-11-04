// P5.1: the SDL3 input backend end to end with an SDL virtual gamepad (no
// device, no display): hot-plug, label mapping, sticks, triggers, motion
// sensors, rumble, keyboard events and TV remotes.
#include <SDL3/SDL.h>

#include <cmath>
#include <cstdio>
#include <string>

#include "../runtime/check.hpp"
#include "lgp/platform/desktop/sdl_input.hpp"

using namespace lgp::platform;

namespace {

uint16_t g_rumble[2] = {0, 0};
int g_rumble_calls = 0;

bool SDLCALL on_rumble(void*, Uint16 low, Uint16 high) {
    g_rumble[0] = low;
    g_rumble[1] = high;
    ++g_rumble_calls;
    return true;
}

bool SDLCALL on_sensors(void*, bool) { return true; }

void pump(desktop::SdlInput& in) {
    SDL_UpdateJoysticks();
    SDL_PumpEvents();
    SDL_Event e;
    while (SDL_PollEvent(&e))
        in.handle_event(e);
}

void test_virtual_gamepad() {
    std::string error;
    auto in = desktop::SdlInput::create({}, &error);
    if (!in) {
        std::printf("  SKIP: %s\n", error.c_str());
        return;
    }
    in->poll();
    CHECK(in->controller(0).style == ControllerStyle::Handheld);  // the keyboard
    CHECK(in->controller(1).style == ControllerStyle::None);

    const SDL_VirtualJoystickSensorDesc sensors[2] = {{SDL_SENSOR_GYRO, 200.0f}, {SDL_SENSOR_ACCEL, 200.0f}};
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.nsensors = 2;
    desc.sensors = sensors;
    desc.button_mask = (1u << SDL_GAMEPAD_BUTTON_COUNT) - 1;
    desc.axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1;
    desc.name = "lgp virtual pad";
    desc.Rumble = on_rumble;
    desc.SetSensorsEnabled = on_sensors;
    const SDL_JoystickID id = SDL_AttachVirtualJoystick(&desc);
    CHECK(id != 0);
    SDL_Joystick* joy = SDL_OpenJoystick(id);
    CHECK(joy != nullptr);
    pump(*in);
    CHECK(in->gamepad_count() == 1);

    SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_SOUTH, true);  // labelled A (Xbox layout)
    SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_START, true);
    SDL_SetJoystickVirtualAxis(joy, SDL_GAMEPAD_AXIS_LEFTX, 32767);
    SDL_SetJoystickVirtualAxis(joy, SDL_GAMEPAD_AXIS_LEFTY, -32768);  // up
    SDL_SetJoystickVirtualAxis(joy, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 32767);
    const float accel[3] = {0.0f, SDL_STANDARD_GRAVITY, 0.0f};  // held upright
    const float gyro[3] = {6.28318530718f, 0.0f, 0.0f};          // 1 rev/s pitch
    SDL_SendJoystickVirtualSensorData(joy, SDL_SENSOR_ACCEL, 1, accel, 3);
    SDL_SendJoystickVirtualSensorData(joy, SDL_SENSOR_GYRO, 1, gyro, 3);
    pump(*in);
    in->poll();
    ControllerState s = in->controller(0);
    CHECK(s.style == ControllerStyle::Handheld);
    CHECK(s.buttons == (kButtonA | kButtonPlus | kButtonZR));
    CHECK(s.left.x > 23000 && s.left.y > 23000);  // diagonal, clamped to the circle
    CHECK(s.motion.valid);
    CHECK(std::fabs(s.motion.accel[2] + 1.0f) < 1e-4f && std::fabs(s.motion.accel[1]) < 1e-4f);
    CHECK(std::fabs(s.motion.gyro[0] + 1.0f) < 1e-4f);

    in->set_vibration(0, 1.0f, 0.5f);
    in->service();
    CHECK(g_rumble_calls >= 1 && g_rumble[0] == 0xFFFF && g_rumble[1] == 0x8000);
    // A pulse that starts and ends between two main-loop iterations still
    // reaches the motor once, then the motor stops.
    in->set_vibration(0, 0.0f, 0.0f);
    in->service();
    CHECK(g_rumble[0] == 0 && g_rumble[1] == 0);
    in->set_vibration(0, 0.0f, 0.75f);
    in->set_vibration(0, 0.0f, 0.0f);
    in->service();
    CHECK(g_rumble[0] == 0 && g_rumble[1] == std::lround(0.75f * 0xFFFF));
    in->service();
    CHECK(g_rumble[0] == 0 && g_rumble[1] == 0);

    // Keyboard events add to slot 0.
    SDL_Event key{};
    key.type = SDL_EVENT_KEY_DOWN;
    key.key.scancode = SDL_SCANCODE_X;  // B
    key.key.down = true;
    in->handle_event(key);
    in->poll();
    CHECK((in->controller(0).buttons & kButtonB) != 0);
    SDL_Event focus{};
    focus.type = SDL_EVENT_WINDOW_FOCUS_LOST;
    in->handle_event(focus);
    in->poll();
    CHECK((in->controller(0).buttons & kButtonB) == 0);

    SDL_CloseJoystick(joy);
    SDL_DetachVirtualJoystick(id);
    pump(*in);
    CHECK(in->gamepad_count() == 0);
    in->poll();
    s = in->controller(0);
    CHECK(s.style == ControllerStyle::Handheld && s.buttons == 0 && !s.motion.valid);
}

// A trigger pressed and released between two samples still counts once as
// ZL/ZR (B9: on Android SDL holds the first trigger press back and sends it
// only together with the release).
void test_trigger_between_samples() {
    std::string error;
    auto in = desktop::SdlInput::create({}, &error);
    if (!in) {
        std::printf("  SKIP: %s\n", error.c_str());
        return;
    }
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.button_mask = (1u << SDL_GAMEPAD_BUTTON_COUNT) - 1;
    desc.axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1;
    desc.name = "lgp virtual pad";
    const SDL_JoystickID id = SDL_AttachVirtualJoystick(&desc);
    CHECK(id != 0);
    SDL_Joystick* joy = SDL_OpenJoystick(id);
    CHECK(joy != nullptr);
    pump(*in);
    in->poll();
    CHECK((in->controller(0).buttons & (kButtonZL | kButtonZR)) == 0);

    // Press and release both events in the queue before the next sample.
    SDL_SetJoystickVirtualAxis(joy, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 32767);
    SDL_UpdateJoysticks();
    SDL_SetJoystickVirtualAxis(joy, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, -32768);
    pump(*in);
    in->poll();
    CHECK((in->controller(0).buttons & kButtonZL) != 0);
    in->poll();  // once only
    CHECK((in->controller(0).buttons & kButtonZL) == 0);

    // A held trigger stays held; a trigger below the threshold never counts.
    SDL_SetJoystickVirtualAxis(joy, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 32767);
    pump(*in);
    in->poll();
    in->poll();
    CHECK((in->controller(0).buttons & kButtonZR) != 0);
    SDL_SetJoystickVirtualAxis(joy, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, -32768);
    pump(*in);
    in->poll();
    CHECK((in->controller(0).buttons & kButtonZR) == 0);
    SDL_SetJoystickVirtualAxis(joy, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, -16384);
    pump(*in);
    in->poll();
    CHECK((in->controller(0).buttons & kButtonZL) == 0);

    SDL_CloseJoystick(joy);
    SDL_DetachVirtualJoystick(id);
    pump(*in);
}

// --pad-motion off and --input-log: sensors stay off, the log runs.
void test_motion_off() {
    std::string error;
    desktop::SdlInputOptions options;
    options.motion = false;
    options.log = true;
    options.keyboard = false;
    auto in = desktop::SdlInput::create(options, &error);
    if (!in) {
        std::printf("  SKIP: %s\n", error.c_str());
        return;
    }
    const SDL_VirtualJoystickSensorDesc sensors[2] = {{SDL_SENSOR_GYRO, 200.0f}, {SDL_SENSOR_ACCEL, 200.0f}};
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.nsensors = 2;
    desc.sensors = sensors;
    desc.button_mask = (1u << SDL_GAMEPAD_BUTTON_COUNT) - 1;
    desc.axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1;
    desc.name = "lgp virtual pad";
    desc.Rumble = on_rumble;
    desc.SetSensorsEnabled = on_sensors;
    const SDL_JoystickID id = SDL_AttachVirtualJoystick(&desc);
    CHECK(id != 0);
    SDL_Joystick* joy = SDL_OpenJoystick(id);
    pump(*in);
    CHECK(in->gamepad_count() == 1);
    const float gyro[3] = {6.28318530718f, 0.0f, 0.0f};
    SDL_SendJoystickVirtualSensorData(joy, SDL_SENSOR_GYRO, 1, gyro, 3);
    SDL_SetJoystickVirtualAxis(joy, SDL_GAMEPAD_AXIS_LEFTX, 32767);
    pump(*in);
    in->poll();
    const ControllerState s = in->controller(0);
    CHECK(s.style == ControllerStyle::Handheld && s.left.x == 32767);
    CHECK(!s.motion.valid);
    in->service();
    SDL_CloseJoystick(joy);
    SDL_DetachVirtualJoystick(id);
    pump(*in);
    CHECK(in->gamepad_count() == 0);
}


// A TV remote that SDL makes a gamepad (Android TV: D-pad, centre, Back and
// Menu, no stick; no right stick, no shoulders) takes no slot: its buttons join slot 0, and a controller
// connected after it is still player 1 (slot 0), not player 2.
void test_remote_joins_slot0() {
    std::string error;
    desktop::SdlInputOptions options;
    options.keyboard = false;
    options.tv_remotes = true;  // the default on Android TV and tvOS only
    auto in = desktop::SdlInput::create(options, &error);
    if (!in) {
        std::printf("  SKIP: %s\n", error.c_str());
        return;
    }
    SDL_VirtualJoystickDesc remote;
    SDL_INIT_INTERFACE(&remote);
    remote.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    remote.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    remote.button_mask = (1u << SDL_GAMEPAD_BUTTON_SOUTH) | (1u << SDL_GAMEPAD_BUTTON_EAST) |
                         (1u << SDL_GAMEPAD_BUTTON_START) | (1u << SDL_GAMEPAD_BUTTON_DPAD_UP) |
                         (1u << SDL_GAMEPAD_BUTTON_DPAD_DOWN) | (1u << SDL_GAMEPAD_BUTTON_DPAD_LEFT) |
                         (1u << SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
    remote.name = "lgp virtual remote";
    remote.vendor_id = 0x1209;  // SDL keys mappings by GUID: own ids for each device
    remote.product_id = 0x0001;
    const SDL_JoystickID remote_id = SDL_AttachVirtualJoystick(&remote);
    CHECK(remote_id != 0);
    SDL_Joystick* remote_joy = SDL_OpenJoystick(remote_id);
    pump(*in);
    CHECK(in->gamepad_count() == 1);

    SDL_VirtualJoystickDesc pad;
    SDL_INIT_INTERFACE(&pad);
    pad.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    pad.naxes = SDL_GAMEPAD_AXIS_COUNT;
    pad.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    pad.button_mask = (1u << SDL_GAMEPAD_BUTTON_COUNT) - 1;
    pad.axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1;
    pad.name = "lgp virtual pad";
    pad.vendor_id = 0x1209;
    pad.product_id = 0x0002;
    pad.Rumble = on_rumble;
    const SDL_JoystickID pad_id = SDL_AttachVirtualJoystick(&pad);
    CHECK(pad_id != 0);
    SDL_Joystick* pad_joy = SDL_OpenJoystick(pad_id);
    pump(*in);
    CHECK(in->gamepad_count() == 2);

    SDL_SetJoystickVirtualButton(remote_joy, SDL_GAMEPAD_BUTTON_EAST, true);  // the remote's Back
    SDL_SetJoystickVirtualButton(pad_joy, SDL_GAMEPAD_BUTTON_SOUTH, true);
    SDL_SetJoystickVirtualAxis(pad_joy, SDL_GAMEPAD_AXIS_LEFTX, 32767);
    pump(*in);
    in->poll();
    const ControllerState s = in->controller(0);
    CHECK(s.style == ControllerStyle::Handheld);
    CHECK(s.buttons == (kButtonA | kButtonB));
    CHECK(s.left.x == 32767);
    CHECK(in->controller(1).style == ControllerStyle::None);  // no player 2
    // Slot 0's vibration goes to the controller, not to the remote.
    g_rumble_calls = 0;
    in->set_vibration(0, 1.0f, 1.0f);
    in->service();
    CHECK(g_rumble_calls >= 1);
    in->set_vibration(0, 0.0f, 0.0f);
    in->service();

    // The controller goes: the remote alone still drives slot 0.
    SDL_CloseJoystick(pad_joy);
    SDL_DetachVirtualJoystick(pad_id);
    pump(*in);
    in->poll();
    CHECK(in->controller(0).buttons == kButtonB);
    SDL_CloseJoystick(remote_joy);
    SDL_DetachVirtualJoystick(remote_id);
    pump(*in);
    CHECK(in->gamepad_count() == 0);
}

// The Siri Remote under SDL (tvOS): the touch surface is the left stick,
// click A, Play/Pause X, Back/Menu B. It joins slot 0 with its stick. A
// D-pad controller without sticks (SNES pad) has shoulders and keeps a slot
// of its own even with tv_remotes on.
void test_siri_remote_and_dpad_pad() {
    std::string error;
    desktop::SdlInputOptions options;
    options.keyboard = false;
    options.tv_remotes = true;
    auto in = desktop::SdlInput::create(options, &error);
    if (!in) {
        std::printf("  SKIP: %s\n", error.c_str());
        return;
    }
    SDL_VirtualJoystickDesc remote;
    SDL_INIT_INTERFACE(&remote);
    remote.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    remote.naxes = 2;
    remote.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    remote.axis_mask = (1u << SDL_GAMEPAD_AXIS_LEFTX) | (1u << SDL_GAMEPAD_AXIS_LEFTY);
    remote.button_mask = (1u << SDL_GAMEPAD_BUTTON_SOUTH) | (1u << SDL_GAMEPAD_BUTTON_EAST) |
                         (1u << SDL_GAMEPAD_BUTTON_WEST);
    remote.name = "lgp virtual siri remote";
    remote.vendor_id = 0x1209;
    remote.product_id = 0x0003;
    const SDL_JoystickID remote_id = SDL_AttachVirtualJoystick(&remote);
    CHECK(remote_id != 0);
    SDL_Joystick* remote_joy = SDL_OpenJoystick(remote_id);

    SDL_VirtualJoystickDesc snes;
    SDL_INIT_INTERFACE(&snes);
    snes.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    snes.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    snes.button_mask = (1u << SDL_GAMEPAD_BUTTON_SOUTH) | (1u << SDL_GAMEPAD_BUTTON_EAST) |
                       (1u << SDL_GAMEPAD_BUTTON_WEST) | (1u << SDL_GAMEPAD_BUTTON_NORTH) |
                       (1u << SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) | (1u << SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) |
                       (1u << SDL_GAMEPAD_BUTTON_BACK) | (1u << SDL_GAMEPAD_BUTTON_START) |
                       (1u << SDL_GAMEPAD_BUTTON_DPAD_UP) | (1u << SDL_GAMEPAD_BUTTON_DPAD_DOWN) |
                       (1u << SDL_GAMEPAD_BUTTON_DPAD_LEFT) | (1u << SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
    snes.name = "lgp virtual snes pad";
    snes.vendor_id = 0x1209;
    snes.product_id = 0x0004;
    const SDL_JoystickID snes_id = SDL_AttachVirtualJoystick(&snes);
    CHECK(snes_id != 0);
    SDL_Joystick* snes_joy = SDL_OpenJoystick(snes_id);
    pump(*in);
    CHECK(in->gamepad_count() == 2);

    SDL_SetJoystickVirtualAxis(remote_joy, 0, 32767);  // swipe right on the touch surface
    SDL_SetJoystickVirtualButton(remote_joy, SDL_GAMEPAD_BUTTON_WEST, true);  // Play/Pause
    SDL_SetJoystickVirtualButton(snes_joy, SDL_GAMEPAD_BUTTON_SOUTH, true);
    pump(*in);
    in->poll();
    ControllerState s = in->controller(0);
    CHECK(s.buttons == (kButtonA | kButtonX));
    CHECK(s.left.x == 32767);
    CHECK(in->controller(1).style == ControllerStyle::None);  // the SNES pad is player 1, not 2

    // Released, slot 0's stick is back at rest.
    SDL_SetJoystickVirtualAxis(remote_joy, 0, 0);
    pump(*in);
    in->poll();
    CHECK(in->controller(0).left.x == 0);

    // A single Joy-Con has no right stick and reports SL/SR as paddles: it
    // keeps its own slot.
    SDL_VirtualJoystickDesc joycon;
    SDL_INIT_INTERFACE(&joycon);
    joycon.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    joycon.naxes = 2;
    joycon.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    joycon.axis_mask = (1u << SDL_GAMEPAD_AXIS_LEFTX) | (1u << SDL_GAMEPAD_AXIS_LEFTY);
    joycon.button_mask = (1u << SDL_GAMEPAD_BUTTON_SOUTH) | (1u << SDL_GAMEPAD_BUTTON_EAST) |
                         (1u << SDL_GAMEPAD_BUTTON_WEST) | (1u << SDL_GAMEPAD_BUTTON_NORTH) |
                         (1u << SDL_GAMEPAD_BUTTON_LEFT_PADDLE1) | (1u << SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1);
    joycon.name = "lgp virtual single joy-con";
    joycon.vendor_id = 0x1209;
    joycon.product_id = 0x0005;
    const SDL_JoystickID joycon_id = SDL_AttachVirtualJoystick(&joycon);
    CHECK(joycon_id != 0);
    SDL_Joystick* joycon_joy = SDL_OpenJoystick(joycon_id);
    pump(*in);
    CHECK(in->gamepad_count() == 3);
    SDL_SetJoystickVirtualButton(joycon_joy, SDL_GAMEPAD_BUTTON_NORTH, true);
    pump(*in);
    in->poll();
    CHECK(in->controller(1).buttons == kButtonY);
    CHECK((in->controller(0).buttons & kButtonY) == 0);
    SDL_CloseJoystick(joycon_joy);
    SDL_DetachVirtualJoystick(joycon_id);

    SDL_CloseJoystick(snes_joy);
    SDL_DetachVirtualJoystick(snes_id);
    SDL_CloseJoystick(remote_joy);
    SDL_DetachVirtualJoystick(remote_id);
    pump(*in);
    CHECK(in->gamepad_count() == 0);
}

// Two pads: the one on which a button is pressed first is player 1, also
// when it connected second; it holds slot 0 until the game disconnects
// npad No1 (LGP's controller screen with a single Joy-Con) or the user asks
// to choose again, and then the next press decides. Order mode keeps the
// connection order.
struct PadRumble {
    uint16_t low = 0, high = 0;
    int calls = 0;
};

bool SDLCALL on_pad_rumble(void* user, Uint16 low, Uint16 high) {
    auto* r = static_cast<PadRumble*>(user);
    r->low = low;
    r->high = high;
    ++r->calls;
    return true;
}

SDL_JoystickID attach_plain_pad(const char* name, PadRumble* rumble) {
    SDL_VirtualJoystickDesc pad;
    SDL_INIT_INTERFACE(&pad);
    pad.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    pad.naxes = SDL_GAMEPAD_AXIS_COUNT;
    pad.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    pad.button_mask = (1u << SDL_GAMEPAD_BUTTON_COUNT) - 1;
    pad.axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1;
    pad.name = name;
    pad.userdata = rumble;
    pad.Rumble = on_pad_rumble;
    return SDL_AttachVirtualJoystick(&pad);
}

void test_player1_by_press() {
    std::string error;
    desktop::SdlInputOptions options;
    options.keyboard = false;
    options.tv_remotes = false;
    auto in = desktop::SdlInput::create(options, &error);
    if (!in) {
        std::printf("  SKIP: %s\n", error.c_str());
        return;
    }
    PadRumble first_rumble, second_rumble;
    const SDL_JoystickID first_id = attach_plain_pad("lgp first pad", &first_rumble);
    const SDL_JoystickID second_id = attach_plain_pad("lgp second pad", &second_rumble);
    SDL_Joystick* first = SDL_OpenJoystick(first_id);
    SDL_Joystick* second = SDL_OpenJoystick(second_id);
    pump(*in);
    in->poll();
    CHECK(in->gamepad_count() == 2);
    CHECK(in->player1_name() == "lgp first pad");  // before any press: connection order

    // A on the second pad: it becomes player 1, and the press reaches the game on slot 0.
    SDL_SetJoystickVirtualButton(second, SDL_GAMEPAD_BUTTON_SOUTH, true);
    pump(*in);
    in->poll();
    CHECK(in->player1_name() == "lgp second pad");
    CHECK(in->controller(0).style == ControllerStyle::Handheld && in->controller(0).buttons == kButtonA);
    CHECK(in->controller(1).style == ControllerStyle::JoyconRight && in->controller(1).buttons == 0);
    // Rumble for slot 0 goes to the second pad.
    in->set_vibration(0, 1.0f, 0.0f);
    in->service();
    CHECK(second_rumble.low == 0xFFFF && first_rumble.calls == 0);

    // The choice holds: a press on the first pad stays player 2.
    SDL_SetJoystickVirtualButton(second, SDL_GAMEPAD_BUTTON_SOUTH, false);
    SDL_SetJoystickVirtualButton(first, SDL_GAMEPAD_BUTTON_EAST, true);
    pump(*in);
    in->poll();
    CHECK(in->player1_name() == "lgp second pad");
    CHECK(in->controller(0).buttons == 0 && in->controller(1).buttons == kButtonB);

    // The game disconnects npad No1: a button held then does not count, the
    // next new press does (here on the first pad again).
    in->npad_disconnected(1);  // other slots do not choose again
    in->npad_disconnected(0);
    pump(*in);
    in->poll();
    CHECK(in->player1_name() == "lgp second pad");  // unchanged until a press
    SDL_SetJoystickVirtualButton(first, SDL_GAMEPAD_BUTTON_EAST, false);
    pump(*in);
    in->poll();
    SDL_SetJoystickVirtualButton(first, SDL_GAMEPAD_BUTTON_NORTH, true);
    pump(*in);
    in->poll();
    CHECK(in->player1_name() == "lgp first pad");
    CHECK(in->controller(0).buttons == kButtonY);
    CHECK(in->controller(0).device == first_id && in->controller(1).device == second_id);
    // The vibration of slot 0 moved with it: the second pad stops, the first runs.
    in->service();
    CHECK(second_rumble.low == 0 && first_rumble.low == 0xFFFF);
    in->set_vibration(0, 0.0f, 0.0f);
    in->service();
    CHECK(first_rumble.low == 0);
    SDL_SetJoystickVirtualButton(first, SDL_GAMEPAD_BUTTON_NORTH, false);

    // Settings: choose again by hand, here with an analog trigger only.
    in->pick_player1();
    SDL_SetJoystickVirtualAxis(second, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 32767);
    pump(*in);
    in->poll();
    CHECK(in->player1_name() == "lgp second pad");
    SDL_SetJoystickVirtualAxis(second, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 0);

    // Order mode: connection order, presses change nothing.
    in->set_player1_mode(PlayerPick::Mode::Order);
    pump(*in);
    in->poll();
    CHECK(in->player1_name() == "lgp first pad");
    in->pick_player1();
    in->npad_disconnected(0);
    SDL_SetJoystickVirtualButton(second, SDL_GAMEPAD_BUTTON_SOUTH, true);
    pump(*in);
    in->poll();
    CHECK(in->player1_name() == "lgp first pad");
    CHECK(in->controller(1).buttons == kButtonA);

    // Player 1 unplugged in press mode: the other pad moves up until a press decides.
    in->set_player1_mode(PlayerPick::Mode::Press);
    SDL_SetJoystickVirtualButton(second, SDL_GAMEPAD_BUTTON_SOUTH, false);
    pump(*in);
    in->poll();
    SDL_SetJoystickVirtualButton(second, SDL_GAMEPAD_BUTTON_SOUTH, true);
    pump(*in);
    in->poll();
    CHECK(in->player1_name() == "lgp second pad");
    SDL_CloseJoystick(second);
    SDL_DetachVirtualJoystick(second_id);
    pump(*in);
    in->poll();
    CHECK(in->gamepad_count() == 1);
    CHECK(in->player1_name() == "lgp first pad");
    CHECK(in->controller(1).style == ControllerStyle::None);

    SDL_CloseJoystick(first);
    SDL_DetachVirtualJoystick(first_id);
    pump(*in);
    CHECK(in->gamepad_count() == 0);
}


// A swipe on the touch screen throws (A in handheld) while no gamepad plays
// player 1; with one only when the settings allow it (touch_throw_with_gamepad).
// A pad only paired, without a button pressed, does not play yet.
void test_touch_throw_with_gamepad() {
    std::string error;
    desktop::SdlInputOptions options;
    options.keyboard = false;
    options.tv_remotes = false;
    options.touch = true;
    auto in = desktop::SdlInput::create(options, &error);
    if (!in) {
        std::printf("  SKIP: %s\n", error.c_str());
        return;
    }
    SDL_FingerID finger = 0;
    const auto swipe_buttons = [&] {
        constexpr uint64_t ms = 1'000'000;
        const float d = TouchPad::kSwipeDistance;
        const uint64_t t0 = SDL_GetTicksNS();
        SDL_Event e{};
        e.tfinger.fingerID = ++finger;
        e.tfinger.x = 0.55f;
        e.tfinger.y = 0.7f;
        e.tfinger.timestamp = t0;
        e.type = SDL_EVENT_FINGER_DOWN;
        in->handle_event(e);
        e.type = SDL_EVENT_FINGER_MOTION;
        e.tfinger.y = 0.7f - d - 0.01f;
        e.tfinger.timestamp = t0 + 60 * ms;
        in->handle_event(e);
        e.type = SDL_EVENT_FINGER_UP;
        in->handle_event(e);
        in->poll();
        const uint32_t buttons = in->controller(0).buttons;
        while (SDL_GetTicksNS() < t0 + 60 * ms + TouchPad::kSwipePressNs + ms)
            SDL_Delay(5);
        in->poll();  // the press ends
        return buttons;
    };
    CHECK_EQ(swipe_buttons(), kButtonA);  // no gamepad: the swipe is A

    PadRumble rumble;
    const SDL_JoystickID id = attach_plain_pad("lgp swipe pad", &rumble);
    SDL_Joystick* joy = SDL_OpenJoystick(id);
    pump(*in);
    in->poll();
    CHECK(in->player1_name() == "lgp swipe pad");
    CHECK_EQ(swipe_buttons(), kButtonA);  // paired, nothing pressed: still swipes
    // Pressed and released between two samples: still counts.
    SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_SOUTH, true);
    pump(*in);
    SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_SOUTH, false);
    pump(*in);
    in->poll();
    CHECK_EQ(swipe_buttons(), 0u);  // the gamepad plays player 1: no swipe

    in->set_touch_throw(true, true);
    CHECK_EQ(swipe_buttons(), kButtonA);  // allowed with a gamepad as well
    in->set_touch_throw(false, true);
    CHECK_EQ(swipe_buttons(), 0u);  // off is off

    SDL_CloseJoystick(joy);
    SDL_DetachVirtualJoystick(id);
    pump(*in);
    in->set_touch_throw(true, false);
    in->poll();
    CHECK_EQ(swipe_buttons(), kButtonA);  // the gamepad gone: swipes again
}

}  // namespace

int main() {
    RUN(test_virtual_gamepad);
    RUN(test_trigger_between_samples);
    RUN(test_motion_off);
    RUN(test_remote_joins_slot0);
    RUN(test_siri_remote_and_dpad_pad);
    RUN(test_player1_by_press);
    RUN(test_touch_throw_with_gamepad);
    return finish();
}
