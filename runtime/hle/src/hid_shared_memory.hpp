// Layout of the hid shared memory (0x40000 bytes) as the SDK reads it.
// Field names and sizes follow libnx hid.h and switchbrew "HID Shared Memory"
// (interface documentation only); the offsets are checked below.
//
// Every input source is a LIFO ring of 17 entries: the writer stores the new
// sample at (tail + 1) % 17, then publishes tail and count (at most 16, so a
// reader never sees the slot being written). Each entry carries the sampling
// number twice (atomic storage + state) so readers can detect torn reads.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace lgp::hle::hidmem {

inline constexpr uint64_t kSharedMemorySize = 0x40000;
inline constexpr uint64_t kLifoEntries = 17;
inline constexpr uint64_t kLifoMaxCount = kLifoEntries - 1;

struct LifoHeader {
    uint64_t unused;
    uint64_t buffer_count;
    uint64_t tail;
    uint64_t count;
};

template <typename State>
struct AtomicStorage {
    uint64_t sampling_number;
    State state;
};

template <typename State>
struct Lifo {
    LifoHeader header;
    AtomicStorage<State> storage[kLifoEntries];
};

struct AnalogStick {
    int32_t x;
    int32_t y;
};

struct Vector3 {
    float x;
    float y;
    float z;
};

// --- debug pad ---------------------------------------------------------------------

struct DebugPadState {
    uint64_t sampling_number;
    uint32_t attributes;  // bit 0: IsConnected
    uint32_t buttons;
    AnalogStick stick_r;
    AnalogStick stick_l;
};

struct DebugPadSection {
    Lifo<DebugPadState> lifo;
    uint8_t padding[0x138];
};

// --- touch screen ------------------------------------------------------------------

struct TouchState {
    uint64_t delta_time;
    uint32_t attributes;
    uint32_t finger_id;
    uint32_t x;
    uint32_t y;
    uint32_t diameter_x;
    uint32_t diameter_y;
    uint32_t rotation_angle;
    uint32_t reserved;
};

struct TouchScreenState {
    uint64_t sampling_number;
    int32_t count;
    uint32_t reserved;
    TouchState touches[16];
};

struct TouchScreenSection {
    Lifo<TouchScreenState> lifo;
    uint8_t padding[0x3C8];
};

// --- npad --------------------------------------------------------------------------

struct ControllerColor {
    uint32_t main;  // RGBA body
    uint32_t sub;   // RGBA buttons
};

struct FullKeyColorState {
    uint32_t attribute;  // ColorAttribute
    ControllerColor full_key;
};

struct JoyColorState {
    uint32_t attribute;
    ControllerColor left;
    ControllerColor right;
};

// nn::hid::NpadFullKeyState and friends (handheld, joy dual/left/right, ...).
struct NpadCommonState {
    uint64_t sampling_number;
    uint64_t buttons;  // NpadButton bits
    AnalogStick stick_l;
    AnalogStick stick_r;
    uint32_t attributes;  // NpadAttribute bits
    uint32_t reserved;
};

struct SixAxisSensorState {
    uint64_t delta_time;  // ns since the previous sample
    uint64_t sampling_number;
    Vector3 acceleration;      // G
    Vector3 angular_velocity;  // revolutions per second
    Vector3 angle;             // accumulated revolutions
    float direction[3][3];     // orientation, rows are the controller axes
    uint32_t attributes;       // bit 0: IsConnected, bit 1: IsInterpolated
    uint32_t reserved;
};

struct NpadGcTriggerState {
    uint64_t sampling_number;
    uint32_t trigger_l;
    uint32_t trigger_r;
};

struct NfcXcdDeviceHandleState {
    uint64_t handle;
    uint8_t is_available;
    uint8_t is_activated;
    uint8_t reserved[6];
    uint64_t sampling_number;
};

// Order of the seven button lifos in NpadInternalState.
enum class NpadLifo : uint8_t { FullKey, Handheld, JoyDual, JoyLeft, JoyRight, Palma, SystemExt, Count };
// Order of the six motion lifos.
enum class SixAxisLifo : uint8_t { FullKey, Handheld, JoyDualLeft, JoyDualRight, JoyLeft, JoyRight, Count };

struct NpadInternalState {
    uint32_t style_set;            // NpadStyleTag bits
    uint32_t joy_assignment_mode;  // 0 dual, 1 single
    FullKeyColorState full_key_color;
    JoyColorState joy_color;
    Lifo<NpadCommonState> lifos[static_cast<size_t>(NpadLifo::Count)];
    Lifo<SixAxisSensorState> six_axis[static_cast<size_t>(SixAxisLifo::Count)];
    uint32_t device_type;  // DeviceType bits
    uint32_t reserved;
    uint64_t system_properties;         // NpadSystemProperties bits
    uint32_t system_button_properties;  // bit 0: unintended home button protection
    uint32_t battery_level[3];          // 0..4: dual/handheld, left, right
    union {
        struct {  // up to 3.0.2: a LIFO of only 2 entries
            LifoHeader header;
            AtomicStorage<NfcXcdDeviceHandleState> storage[2];
        } nfc_xcd_device_handle;
        struct {
            uint32_t applet_footer_ui_attribute;
            uint8_t applet_footer_ui_type;
            uint8_t reserved_footer[0x5B];
        } footer;
    };
    uint8_t reserved_x4208[0x20];  // a mutex before 10.0.0
    Lifo<NpadGcTriggerState> gc_trigger;
    uint32_t lark_type_l_and_main;
    uint32_t lark_type_r;
    uint32_t lucia_type;
    uint32_t lager_type;
};

struct NpadEntry {
    NpadInternalState state;
    uint8_t padding[0xC10];
};

// --- gesture -----------------------------------------------------------------------

struct GesturePoint {
    uint32_t x;
    uint32_t y;
};

struct GestureState {
    uint64_t sampling_number;
    uint64_t context_number;
    uint32_t type;
    uint32_t direction;
    uint32_t x;
    uint32_t y;
    int32_t delta_x;
    int32_t delta_y;
    float velocity_x;
    float velocity_y;
    uint32_t attributes;
    float scale;
    float rotation_angle;
    int32_t point_count;
    GesturePoint points[4];
};

struct GestureSection {
    Lifo<GestureState> lifo;
    uint8_t padding[0xF8];
};

// --- whole block -------------------------------------------------------------------

// Npad entries 0..7 are No1..No8, 8 is Handheld, 9 is Other.
inline constexpr size_t kNpadCount = 10;

struct SharedMemory {
    DebugPadSection debug_pad;          // 0x00000
    TouchScreenSection touch_screen;    // 0x00400
    uint8_t mouse[0x400];               // 0x03400
    uint8_t keyboard[0x400];            // 0x03800
    uint8_t basic_xpad[0x1000];         // 0x03C00 (digitizer since 10.0.0)
    uint8_t home_button[0x200];         // 0x04C00
    uint8_t sleep_button[0x200];        // 0x04E00
    uint8_t capture_button[0x200];      // 0x05000
    uint8_t input_detector[0x800];      // 0x05200
    uint8_t unique_pad[0x4000];         // 0x05A00 (up to 4.1.0)
    NpadEntry npad[kNpadCount];         // 0x09A00
    GestureSection gesture;             // 0x3BA00
    uint8_t console_six_axis[0x20];     // 0x3C200 (5.0.0+)
    uint8_t unused[0x3DE0];             // 0x3C220
};

// Offsets as libnx and switchbrew give them.
static_assert(sizeof(LifoHeader) == 0x20);
static_assert(sizeof(DebugPadSection) == 0x400);
static_assert(sizeof(TouchScreenSection) == 0x3000);
static_assert(sizeof(NpadCommonState) == 0x28);
static_assert(sizeof(Lifo<NpadCommonState>) == 0x350);
static_assert(sizeof(SixAxisSensorState) == 0x60);
static_assert(sizeof(Lifo<SixAxisSensorState>) == 0x708);
static_assert(offsetof(NpadInternalState, full_key_color) == 0x08);
static_assert(offsetof(NpadInternalState, joy_color) == 0x14);
static_assert(offsetof(NpadInternalState, lifos) == 0x28);
static_assert(offsetof(NpadInternalState, six_axis) == 0x1758);
static_assert(offsetof(NpadInternalState, device_type) == 0x4188);
static_assert(offsetof(NpadInternalState, system_properties) == 0x4190);
static_assert(offsetof(NpadInternalState, battery_level) == 0x419C);
static_assert(offsetof(NpadInternalState, reserved_x4208) == 0x4208);
static_assert(offsetof(NpadInternalState, gc_trigger) == 0x4228);
static_assert(sizeof(NpadInternalState) == 0x43F0);
static_assert(sizeof(NpadEntry) == 0x5000);
static_assert(sizeof(GestureSection) == 0x800);
static_assert(offsetof(SharedMemory, touch_screen) == 0x400);
static_assert(offsetof(SharedMemory, npad) == 0x9A00);
static_assert(offsetof(SharedMemory, gesture) == 0x3BA00);
static_assert(offsetof(SharedMemory, console_six_axis) == 0x3C200);
static_assert(sizeof(SharedMemory) == kSharedMemorySize);

// Appends one sample to a LIFO in place. The caller keeps the sampling
// number counter. The slot written is the one readers skip (count <= 16 of
// 17 entries); tail and count are published afterwards with release stores.
template <typename State>
void push(Lifo<State>& lifo, const State& state) {
    lifo.header.buffer_count = kLifoEntries;
    const uint64_t next = lifo.header.count == 0 ? 0 : (lifo.header.tail + 1) % kLifoEntries;
    AtomicStorage<State>& slot = lifo.storage[next];
    slot.sampling_number = state.sampling_number;
    slot.state = state;
    std::atomic_ref<uint64_t>(lifo.header.tail).store(next, std::memory_order_release);
    if (lifo.header.count < kLifoMaxCount)
        std::atomic_ref<uint64_t>(lifo.header.count).store(lifo.header.count + 1, std::memory_order_release);
}

// Newest entry, or nullptr if the LIFO is empty.
template <typename State>
const State* newest(const Lifo<State>& lifo) {
    return lifo.header.count == 0 ? nullptr : &lifo.storage[lifo.header.tail % kLifoEntries].state;
}

}  // namespace lgp::hle::hidmem
