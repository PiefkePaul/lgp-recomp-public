// P5.1: hid through sm: and the IPC svcs. Requests are shaped like the SDK's
// (libnx hid.c argument structs); the shared memory is read back at the raw
// offsets libnx hid.h and switchbrew give, independent of the service's own
// struct definitions.
#include <cmath>
#include <cstring>
#include <vector>

#include "ipc_client.hpp"
#include "lgp/hle/hid.hpp"
#include "lgp/platform/null_backends.hpp"

using namespace lgp_test;
using lgp::platform::ControllerState;
using lgp::platform::ControllerStyle;
using lgp::platform::ManualInput;
namespace btn = lgp::platform;

namespace {

constexpr uint64_t kAruid = 0x51;
constexpr uint64_t kShmSize = 0x40000;

// Raw layout (libnx hid.h, switchbrew "HID Shared Memory").
constexpr uint64_t kNpadBase = 0x9A00;
constexpr uint64_t kNpadEntrySize = 0x5000;
constexpr uint64_t kNpadLifos = 0x28;  // seven button lifos of 0x350
constexpr uint64_t kButtonLifoSize = 0x350;
constexpr uint64_t kSixAxisLifos = 0x1758;  // six motion lifos of 0x708
constexpr uint64_t kSixAxisLifoSize = 0x708;
constexpr uint64_t kDeviceType = 0x4188;
constexpr uint64_t kSystemProperties = 0x4190;
constexpr uint64_t kBatteryLevel = 0x419C;
constexpr uint64_t kTouchLifo = 0x400;
constexpr uint64_t kTouchEntry = 0x298;  // atomic storage of a TouchScreenState
constexpr uint64_t kGestureLifo = 0x3BA00;
constexpr uint64_t kGestureEntry = 0x68;

enum ButtonLifo { kFullKeyLifo, kHandheldLifo, kJoyDualLifo, kJoyLeftLifo, kJoyRightLifo, kPalmaLifo, kSystemExtLifo };

constexpr uint32_t kHandheldIndex = 8;

struct HidEnv : Env {
    ManualInput input;
    std::shared_ptr<HidState> hid;
    Handle server = kInvalidHandle;
    uint64_t shm_addr = 0;
    uint64_t now = 0;

    explicit HidEnv(size_t controllers = 2, bool freeze_orientation = false)
        : Env([](ServiceManager&) {}), input(controllers) {
        HidConfig config;
        config.input = &input;
        config.freeze_orientation = freeze_orientation;
        hid = register_hid(sm, config);
        server = open_service("hid");
    }

    Reply call(Handle h, uint32_t command, Msg m = {}, SvcOutcome expect = SvcOutcome::Continue) {
        m.command = command;
        m.send_pid = true;
        const std::optional<Reply> r = send(h, m, expect);
        return r ? *r : Reply{};
    }
    // Command whose only argument is (u32, pad, aruid), as _hidCmdInU32AruidNoOut.
    Reply call_u32(uint32_t command, uint32_t v, SvcOutcome expect = SvcOutcome::Continue) {
        Msg m;
        m.arg<uint32_t>(v).arg<uint64_t>(kAruid);
        return call(server, command, m, expect);
    }
    // (aruid, u64), as _hidCmdInU64AruidNoOut.
    Reply call_u64(uint32_t command, uint64_t v) {
        Msg m;
        m.arg<uint64_t>(kAruid).arg<uint64_t>(v);
        return call(server, command, m);
    }
    Reply call_aruid(uint32_t command, SvcOutcome expect = SvcOutcome::Continue) {
        Msg m;
        m.arg<uint64_t>(kAruid);
        return call(server, command, m, expect);
    }

    // CreateAppletResource + GetSharedMemoryHandle + svcMapSharedMemory.
    void map_shared_memory() {
        Msg m;
        m.arg<uint64_t>(kAruid);
        const Reply r = call(server, 0, m);
        CHECK_EQ(r.result, 0u);
        CHECK_EQ(r.move.size(), 1u);
        const Reply sh = call(r.move.at(0), 0);
        CHECK_EQ(sh.result, 0u);
        CHECK_EQ(sh.copy.size(), 1u);
        shm_addr = k.memory().layout().misc.base + 0x40000;
        CHECK_EQ(svc(kSvcMapSharedMemory, {sh.copy.at(0), shm_addr, kShmSize, kPermR})[0], kSuccess.raw);
        // Writable mappings are refused: the guest only reads hid's memory.
        CHECK_EQ(svc(kSvcMapSharedMemory, {sh.copy.at(0), shm_addr + kShmSize, kShmSize, kPermRW})[0],
                 kernel_result::kInvalidNewMemoryPermission.raw);
    }

    void tick(uint64_t step_ns = 5'000'000) {
        now += step_ns;
        hid->update(k, now);
    }

    template <typename T>
    T read(uint64_t off) {
        T v{};
        CHECK(k.memory().copy_from_user(&v, shm_addr + off, sizeof(T)));
        return v;
    }
    uint64_t npad(uint32_t index) const { return kNpadBase + index * kNpadEntrySize; }
    uint64_t lifo(uint32_t index, int which) const { return npad(index) + kNpadLifos + which * kButtonLifoSize; }
    uint64_t six_axis_lifo(uint32_t index, int which) const {
        return npad(index) + kSixAxisLifos + which * kSixAxisLifoSize;
    }

    struct LifoView {
        uint64_t buffer_count, tail, count;
        uint64_t newest;  // address offset of the newest atomic storage
    };
    // Header of a lifo whose entries are entry_size bytes (atomic storage).
    LifoView lifo_view(uint64_t at, uint64_t entry_size) {
        LifoView v{read<uint64_t>(at + 8), read<uint64_t>(at + 16), read<uint64_t>(at + 24), 0};
        v.newest = at + 0x20 + (v.tail % 17) * entry_size;
        return v;
    }

    bool signaled(Handle h) {
        auto ev = k.handles().get_as<KReadableEvent>(h);
        CHECK(ev != nullptr);
        return ev && k.scheduler().is_readable_signaled(*ev);
    }
    void clear(Handle h) {
        auto ev = k.handles().get_as<KReadableEvent>(h);
        CHECK(ev != nullptr);
        if (ev)
            k.scheduler().clear_readable(*ev);
    }
};

ControllerState handheld(uint64_t buttons = 0) {
    ControllerState c;
    c.style = ControllerStyle::Handheld;
    c.buttons = buttons;
    return c;
}

uint32_t sensor_handle(uint8_t style_index, uint32_t npad_id, uint8_t device) {
    return style_index | npad_id << 8 | uint32_t{device} << 16;
}

// --- tests -------------------------------------------------------------------------

void test_handheld_lifo() {
    HidEnv e;
    ControllerState c = handheld(btn::kButtonA | btn::kButtonPlus);
    c.left = {1000, -2000};
    c.right = {-3, 4};
    e.input.set(0, c);
    e.tick();  // before the game maps the memory: state is kept, nothing written yet
    e.map_shared_memory();

    const uint64_t n = e.npad(kHandheldIndex);
    // Headers are written as soon as the memory exists.
    CHECK_EQ(e.read<uint32_t>(n + 0), npad_style::kHandheld);
    CHECK_EQ(e.read<uint32_t>(n + 4), 0u);           // joy assignment dual
    CHECK_EQ(e.read<uint32_t>(n + 0x08), 0u);        // full key color: ok
    CHECK_EQ(e.read<uint32_t>(n + 0x14), 0u);        // joy color: ok
    CHECK_EQ(e.read<uint32_t>(n + 0x18), 0x828282FFu);  // left body (default grey)
    CHECK_EQ(e.read<uint32_t>(n + kDeviceType), (1u << 2) | (1u << 3));  // HandheldLeft | HandheldRight
    CHECK_EQ(e.read<uint64_t>(n + kSystemProperties) & ((1ull << 3) | (1ull << 11)), (1ull << 3) | (1ull << 11));
    CHECK_EQ(e.read<uint32_t>(n + kBatteryLevel), 4u);
    CHECK_EQ(e.read<uint64_t>(e.lifo(kHandheldIndex, kHandheldLifo) + 24), 0u);  // no sample yet
    // Npads without a controller report NoController colors.
    CHECK_EQ(e.read<uint32_t>(e.npad(0) + 0), 0u);
    CHECK_EQ(e.read<uint32_t>(e.npad(0) + 0x08), 2u);

    e.tick();
    auto v = e.lifo_view(e.lifo(kHandheldIndex, kHandheldLifo), 0x30);
    CHECK_EQ(v.buffer_count, 17u);
    CHECK_EQ(v.count, 1u);
    CHECK_EQ(e.read<uint64_t>(v.newest), e.read<uint64_t>(v.newest + 8));  // atomic == state sampling number
    CHECK_EQ(e.read<uint64_t>(v.newest + 0x10), btn::kButtonA | btn::kButtonPlus);
    CHECK_EQ(e.read<int32_t>(v.newest + 0x18), 1000);
    CHECK_EQ(e.read<int32_t>(v.newest + 0x1C), -2000);
    CHECK_EQ(e.read<int32_t>(v.newest + 0x20), -3);
    CHECK_EQ(e.read<int32_t>(v.newest + 0x24), 4);
    CHECK_EQ(e.read<uint32_t>(v.newest + 0x28), 0x3Fu);  // connected, wired, both sides connected and wired
    // Other lifos of the npad get empty samples; SystemExt mirrors the controller.
    const auto full = e.lifo_view(e.lifo(kHandheldIndex, kFullKeyLifo), 0x30);
    CHECK_EQ(full.count, 1u);
    CHECK_EQ(e.read<uint64_t>(full.newest + 0x10), 0u);
    CHECK_EQ(e.read<uint32_t>(full.newest + 0x28), 0u);
    const auto ext = e.lifo_view(e.lifo(kHandheldIndex, kSystemExtLifo), 0x30);
    CHECK_EQ(e.read<uint64_t>(ext.newest + 0x10), btn::kButtonA | btn::kButtonPlus);

    // Ring behaviour: sampling numbers count up by one, count stops at 16.
    const uint64_t first = e.read<uint64_t>(v.newest + 8);
    c.buttons = btn::kButtonB;
    e.input.set(0, c);
    for (int i = 0; i < 20; ++i)
        e.tick();
    v = e.lifo_view(e.lifo(kHandheldIndex, kHandheldLifo), 0x30);
    CHECK_EQ(v.count, 16u);
    CHECK_EQ(v.tail, 20u % 17u);
    CHECK_EQ(e.read<uint64_t>(v.newest + 8), first + 20);
    CHECK_EQ(e.read<uint64_t>(v.newest + 0x10), btn::kButtonB);
    const uint64_t prev = e.lifo(kHandheldIndex, kHandheldLifo) + 0x20 + ((v.tail + 16) % 17) * 0x30;
    CHECK_EQ(e.read<uint64_t>(prev + 8), first + 19);
}

void test_supported_styles_and_events() {
    HidEnv e;
    ControllerState pro;
    pro.style = ControllerStyle::ProController;
    pro.buttons = btn::kButtonX;
    e.input.set(0, pro);
    e.map_shared_memory();
    e.tick();
    CHECK_EQ(e.hid->presented_style(npad_id::kNo1), npad_style::kFullKey);

    Msg acquire;
    acquire.arg<uint32_t>(npad_id::kNo1).arg<uint64_t>(kAruid).arg<uint64_t>(0);
    const Reply ev = e.call(e.server, 106, acquire);
    CHECK_EQ(ev.result, 0u);
    CHECK_EQ(ev.copy.size(), 1u);
    const Handle h = ev.copy.at(0);
    CHECK(e.signaled(h));  // signaled on acquire
    e.clear(h);

    // LGP declares handheld and single Joy-Cons only: the Pro Controller vanishes.
    CHECK_EQ(e.call_u32(100, npad_style::kHandheld | npad_style::kJoyLeft | npad_style::kJoyRight).result, 0u);
    CHECK_EQ(e.hid->presented_style(npad_id::kNo1), 0u);
    CHECK(e.signaled(h));
    CHECK_EQ(e.read<uint32_t>(e.npad(0) + 0), 0u);
    const Reply get = e.call_aruid(101);
    CHECK_EQ(get.get<uint32_t>(0), npad_style::kHandheld | npad_style::kJoyLeft | npad_style::kJoyRight);
    e.tick();
    const auto full = e.lifo_view(e.lifo(0, kFullKeyLifo), 0x30);
    CHECK_EQ(e.read<uint32_t>(full.newest + 0x28), 0u);  // not connected
    CHECK_EQ(e.read<uint64_t>(full.newest + 0x10), 0u);

    // Supported npad ids: a handheld controller only shows while Handheld is listed.
    e.input.set(1, handheld());
    e.tick();
    CHECK_EQ(e.hid->presented_style(npad_id::kHandheld), npad_style::kHandheld);
    const uint32_t ids[] = {npad_id::kNo1, npad_id::kNo2};
    CHECK(e.k.memory().copy_to_user(kBufA, ids, sizeof(ids)));
    Msg set_ids;
    set_ids.arg<uint64_t>(kAruid);
    set_ids.x.push_back({kBufA, sizeof(ids), 0});
    CHECK_EQ(e.call(e.server, 102, set_ids).result, 0u);
    CHECK_EQ(e.hid->presented_style(npad_id::kHandheld), 0u);
    const uint32_t ids2[] = {npad_id::kHandheld, npad_id::kNo1};
    CHECK(e.k.memory().copy_to_user(kBufA, ids2, sizeof(ids2)));
    CHECK_EQ(e.call(e.server, 102, set_ids).result, 0u);
    CHECK_EQ(e.hid->presented_style(npad_id::kHandheld), npad_style::kHandheld);
}

void test_joycon_assignment() {
    HidEnv e;
    ControllerState left;
    left.style = ControllerStyle::JoyconLeft;
    left.buttons = btn::kButtonLeftSR | btn::kButtonUp;
    left.left = {0, 32767};
    e.input.set(0, left);
    e.map_shared_memory();
    e.tick();
    // Dual assignment (the default): a lone left Joy-Con is a half JoyDual.
    CHECK_EQ(e.hid->presented_style(npad_id::kNo1), npad_style::kJoyDual);
    CHECK_EQ(e.read<uint32_t>(e.npad(0) + kDeviceType), 1u << 4);
    auto dual = e.lifo_view(e.lifo(0, kJoyDualLifo), 0x30);
    CHECK_EQ(e.read<uint32_t>(dual.newest + 0x28), 0x5u);  // connected | left connected

    CHECK_EQ(e.call_u32(122, npad_id::kNo1).result, 0u);  // SetNpadJoyAssignmentModeSingleByDefault
    CHECK_EQ(e.hid->presented_style(npad_id::kNo1), npad_style::kJoyLeft);
    CHECK_EQ(e.read<uint32_t>(e.npad(0) + 4), 1u);
    CHECK_EQ(e.read<uint32_t>(e.npad(0) + 0x08), 0u);           // single color ok
    CHECK_EQ(e.read<uint32_t>(e.npad(0) + 0x0C), 0x828282FFu);  // the left Joy-Con's body
    CHECK_EQ(e.read<uint64_t>(e.npad(0) + kSystemProperties) & (1ull << 12), 1ull << 12);  // SL/SR oriented
    e.tick(100'000'000);  // a lone SR waits for SL (L+R recentre) before it shows
    const auto single = e.lifo_view(e.lifo(0, kJoyLeftLifo), 0x30);
    CHECK_EQ(e.read<uint64_t>(single.newest + 0x10), btn::kButtonLeftSR | btn::kButtonUp);
    CHECK_EQ(e.read<int32_t>(single.newest + 0x1C), 32767);
    CHECK_EQ(e.read<uint32_t>(single.newest + 0x28), 0x5u);
    dual = e.lifo_view(e.lifo(0, kJoyDualLifo), 0x30);
    CHECK_EQ(e.read<uint32_t>(dual.newest + 0x28), 0u);

    // SetNpadJoyAssignmentModeSingle(id, aruid, type) and back to dual.
    Msg single_type;
    single_type.arg<uint32_t>(npad_id::kNo2).arg<uint64_t>(kAruid).arg<uint64_t>(1);
    CHECK_EQ(e.call(e.server, 123, single_type).result, 0u);
    ControllerState right;
    right.style = ControllerStyle::JoyconRight;
    e.input.set(1, right);
    e.tick();
    CHECK_EQ(e.hid->presented_style(npad_id::kNo2), npad_style::kJoyRight);
    CHECK_EQ(e.call_u32(124, npad_id::kNo2).result, 0u);
    CHECK_EQ(e.hid->presented_style(npad_id::kNo2), npad_style::kJoyDual);
}

void test_disconnect() {
    HidEnv e;
    e.input.set(0, handheld());
    e.map_shared_memory();
    e.tick();
    CHECK_EQ(e.call_u32(107, npad_id::kHandheld).result, 0u);
    CHECK_EQ(e.hid->presented_style(npad_id::kHandheld), 0u);
    // The backend hears of it: the next pad pressed may take slot 0 (PlayerPick).
    CHECK(e.input.disconnects() == std::vector<size_t>{0});
    e.tick();
    e.tick();
    CHECK_EQ(e.hid->presented_style(npad_id::kHandheld), 0u);  // stays off while the controller stays
    e.input.set(0, ControllerState{});
    e.tick();
    e.input.set(0, handheld());
    e.tick();
    CHECK_EQ(e.hid->presented_style(npad_id::kHandheld), npad_style::kHandheld);  // reconnected
    // A button held through DisconnectNpad does not reconnect; a new press does
    // (a Joy-Con reconnects with SL+SR, a host pad cannot be unplugged).
    ControllerState held = handheld();
    held.buttons = 1u << 0;
    e.input.set(0, held);
    e.tick();
    CHECK_EQ(e.call_u32(107, npad_id::kHandheld).result, 0u);
    e.tick();
    CHECK_EQ(e.hid->presented_style(npad_id::kHandheld), 0u);
    e.input.set(0, handheld());
    e.tick();
    CHECK_EQ(e.hid->presented_style(npad_id::kHandheld), 0u);  // releasing is no press
    held.buttons = 1u << 1;
    e.input.set(0, held);
    e.tick();
    CHECK_EQ(e.hid->presented_style(npad_id::kHandheld), npad_style::kHandheld);
    // Another device takes the slot after the disconnect (PlayerPick): the
    // button the old pad held counts as the new pad's press.
    held.buttons = 1u << 0;
    held.device = 1;
    e.input.set(0, held);
    e.tick();
    CHECK_EQ(e.call_u32(107, npad_id::kHandheld).result, 0u);
    e.tick();
    CHECK_EQ(e.hid->presented_style(npad_id::kHandheld), 0u);
    held.device = 2;
    e.input.set(0, held);
    e.tick();
    CHECK_EQ(e.hid->presented_style(npad_id::kHandheld), npad_style::kHandheld);
    // An npad id hid does not know stops the guest.
    e.call_u32(107, 0x30, SvcOutcome::Unimplemented);

    // Player 2 (slot 1, npad No2) disconnected: player 1's presses on slot 0
    // leave it off, only a press on slot 1 brings it back (--press p2:).
    HidEnv two;
    two.input.set(0, handheld());
    ControllerState right;
    right.style = ControllerStyle::JoyconRight;
    two.input.set(1, right);
    two.map_shared_memory();
    two.tick();
    CHECK(two.hid->presented_style(npad_id::kNo2) != 0u);
    CHECK_EQ(two.call_u32(107, npad_id::kNo2).result, 0u);
    two.tick();
    CHECK_EQ(two.hid->presented_style(npad_id::kNo2), 0u);
    ControllerState p1 = handheld();
    p1.buttons = 1u << 0;
    two.input.set(0, p1);
    two.tick();
    two.input.set(0, handheld());
    two.tick();
    CHECK_EQ(two.hid->presented_style(npad_id::kNo2), 0u);  // player 1's A does not reconnect player 2
    CHECK_EQ(two.hid->presented_style(npad_id::kHandheld), npad_style::kHandheld);
    right.buttons = 1u << 0;
    two.input.set(1, right);
    two.tick();
    CHECK(two.hid->presented_style(npad_id::kNo2) != 0u);
}

void test_six_axis() {
    HidEnv e;
    ControllerState c = handheld();
    c.motion.valid = true;
    c.motion.accel[2] = -1.0f;
    c.motion.gyro[2] = 0.25f;  // a quarter turn per second around z
    e.input.set(0, c);
    e.map_shared_memory();
    const uint32_t h = sensor_handle(4, npad_id::kHandheld, 2);
    const uint64_t at = e.six_axis_lifo(kHandheldIndex, 1);
    e.tick();
    CHECK_EQ(e.read<uint64_t>(at + 24), 0u);  // not started: no samples
    CHECK_EQ(e.call_u32(66, h).result, 0u);
    CHECK(e.hid->six_axis_started(h));
    e.tick(1'000'000);
    e.tick(10'000'000);
    const auto v = e.lifo_view(at, 0x68);
    CHECK_EQ(v.count, 2u);
    const uint64_t s = v.newest + 8;  // state
    CHECK_EQ(e.read<uint64_t>(s + 0), 10'000'000u);  // delta time
    CHECK_EQ(e.read<uint64_t>(v.newest), e.read<uint64_t>(s + 8));
    CHECK(e.read<float>(s + 0x18) == -1.0f);  // acceleration z
    CHECK(e.read<float>(s + 0x24) == 0.25f);  // angular velocity z
    CHECK(std::fabs(e.read<float>(s + 0x30) - 0.0025f) < 1e-6f);  // angle z: 0.25 rev/s * 10 ms
    // Direction: rotated by 0.9 degrees about z.
    const float ang = 0.0025f * 6.2831853f;
    CHECK(std::fabs(e.read<float>(s + 0x34 + 0) - std::cos(ang)) < 1e-5f);
    CHECK(std::fabs(e.read<float>(s + 0x34 + 4) + std::sin(ang)) < 1e-5f);
    CHECK(std::fabs(e.read<float>(s + 0x34 + 32) - 1.0f) < 1e-5f);
    CHECK_EQ(e.read<uint32_t>(s + 0x58), 1u);  // IsConnected

    Msg m;
    m.arg<uint32_t>(h).arg<uint64_t>(kAruid);
    CHECK_EQ(e.call(e.server, 82, m).get<uint8_t>(0), 0u);  // moving
    c.motion.gyro[2] = 0.0f;
    e.input.set(0, c);
    e.tick();
    CHECK_EQ(e.call(e.server, 82, m).get<uint8_t>(0), 1u);  // at rest

    // Stop: no more samples (the game calls this every frame).
    CHECK_EQ(e.call_u32(67, h).result, 0u);
    const uint64_t count = e.lifo_view(at, 0x68).count;
    e.tick();
    CHECK_EQ(e.lifo_view(at, 0x68).count, count);

    // A sensor of a style nobody presents reports "not connected".
    const uint32_t left = sensor_handle(6, npad_id::kNo1, 0);
    CHECK_EQ(e.call_u32(66, left).result, 0u);
    e.tick();
    const auto lv = e.lifo_view(e.six_axis_lifo(0, 4), 0x68);
    CHECK_EQ(lv.count, 1u);
    CHECK_EQ(e.read<uint32_t>(lv.newest + 8 + 0x58), 0u);
    // No motion source: also "not connected", no invented pose.
    c.motion.valid = false;
    e.input.set(0, c);
    CHECK_EQ(e.call_u32(66, h).result, 0u);
    e.tick();
    CHECK_EQ(e.read<uint32_t>(e.lifo_view(at, 0x68).newest + 8 + 0x58), 0u);

    // Fusion and zero drift settings round-trip.
    Msg fusion;
    fusion.arg<uint8_t>(0).arg<uint32_t>(h).arg<uint64_t>(kAruid);
    CHECK_EQ(e.call(e.server, 69, fusion).result, 0u);
    CHECK_EQ(e.call(e.server, 68, m).get<uint8_t>(0), 0u);
    Msg params;
    params.arg<uint32_t>(h).arg<float>(0.5f).arg<float>(0.25f).arg<uint64_t>(kAruid);
    CHECK_EQ(e.call(e.server, 70, params).result, 0u);
    const Reply p = e.call(e.server, 71, m);
    CHECK(p.get<float>(0) == 0.5f && p.get<float>(4) == 0.25f);
    Msg drift;
    drift.arg<uint32_t>(h).arg<uint32_t>(2).arg<uint64_t>(kAruid);
    CHECK_EQ(e.call(e.server, 79, drift).result, 0u);
    CHECK_EQ(e.call(e.server, 80, m).get<uint32_t>(0), 2u);
    CHECK_EQ(e.call(e.server, 81, m).result, 0u);
    CHECK_EQ(e.call(e.server, 80, m).get<uint32_t>(0), 1u);
}

// --motion-pose frozen: rates and acceleration pass, the pose stays at rest.
void test_six_axis_frozen_pose() {
    HidEnv e(2, true);
    ControllerState c = handheld();
    c.motion.valid = true;
    c.motion.gyro[2] = 0.25f;
    c.motion.accel[1] = 1.0f;
    e.input.set(0, c);
    e.map_shared_memory();
    const uint32_t h = sensor_handle(4, npad_id::kHandheld, 2);
    const uint64_t at = e.six_axis_lifo(kHandheldIndex, 1);
    CHECK_EQ(e.call_u32(66, h).result, 0u);
    e.tick(1'000'000);
    e.tick(100'000'000);
    const uint64_t s = e.lifo_view(at, 0x68).newest + 8;
    CHECK(std::fabs(e.read<float>(s + 0x14) - 1.0f) < 1e-6f);   // accel y
    CHECK(std::fabs(e.read<float>(s + 0x24) - 0.25f) < 1e-6f);  // gyro z
    CHECK(std::fabs(e.read<float>(s + 0x30)) < 1e-6f);          // angle z
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            CHECK(std::fabs(e.read<float>(s + 0x34 + 4 * (3 * i + j)) - (i == j ? 1.0f : 0.0f)) < 1e-6f);
    CHECK_EQ(e.read<uint32_t>(s + 0x58), 1u);  // still connected
}

// The integrated orientation goes back to the fresh-start pose when L and R
// are pressed together, and when the game starts the sensor again after a
// pause (not after a stop/start within a few frames).
void test_six_axis_recenter() {
    HidEnv e;
    ControllerState c = handheld();
    c.motion.valid = true;
    c.motion.gyro[2] = 0.25f;
    e.input.set(0, c);
    e.map_shared_memory();
    const uint32_t h = sensor_handle(4, npad_id::kHandheld, 2);
    const uint64_t at = e.six_axis_lifo(kHandheldIndex, 1);
    const auto angle_z = [&] { return e.read<float>(e.lifo_view(at, 0x68).newest + 8 + 0x30); };
    const auto dir00 = [&] { return e.read<float>(e.lifo_view(at, 0x68).newest + 8 + 0x34); };
    CHECK_EQ(e.call_u32(66, h).result, 0u);
    e.tick(1'000'000);
    e.tick(100'000'000);  // 0.025 rev about z
    CHECK(std::fabs(angle_z() - 0.025f) < 1e-5f);
    // L+R: back to angle 0 and an identity direction (plus this tick's turn).
    c.motion.gyro[2] = 0.0f;
    c.buttons = btn::kButtonL | btn::kButtonR;
    e.input.set(0, c);
    e.tick(10'000'000);
    CHECK(std::fabs(angle_z()) < 1e-6f);
    CHECK(std::fabs(dir00() - 1.0f) < 1e-6f);
    // Held further: no second reset, the turn counts again.
    c.motion.gyro[2] = 0.25f;
    e.input.set(0, c);
    e.tick(100'000'000);
    CHECK(std::fabs(angle_z() - 0.025f) < 1e-5f);
    // Stop and start within a frame keeps the pose.
    CHECK_EQ(e.call_u32(67, h).result, 0u);
    e.tick();
    CHECK_EQ(e.call_u32(66, h).result, 0u);
    c.motion.gyro[2] = 0.0f;
    c.buttons = 0;
    e.input.set(0, c);
    e.tick();
    CHECK(std::fabs(angle_z() - 0.025f) < 1e-5f);
    // Stopped for a while, then started: fresh pose.
    CHECK_EQ(e.call_u32(67, h).result, 0u);
    for (int i = 0; i < 30; ++i)
        e.tick(16'000'000);
    c.motion.gyro[2] = 0.25f;  // turning while restarted: the pause is not integrated
    e.input.set(0, c);
    CHECK_EQ(e.call_u32(66, h).result, 0u);
    e.tick();
    CHECK(std::fabs(angle_z()) < 1e-6f);
    e.tick(100'000'000);
    CHECK(std::fabs(angle_z() - 0.025f) < 1e-5f);
}

void test_recenter_chord() {
    HidEnv e;
    ControllerState c = handheld();
    c.motion.valid = true;
    c.motion.gyro[2] = 0.25f;
    e.input.set(0, c);
    e.map_shared_memory();
    const uint32_t h = sensor_handle(4, npad_id::kHandheld, 2);
    const uint64_t at = e.six_axis_lifo(kHandheldIndex, 1);
    const auto angle_z = [&] { return e.read<float>(e.lifo_view(at, 0x68).newest + 8 + 0x30); };
    const auto presses = [&] { return e.hid->button_presses(npad_id::kHandheld); };
    const auto press = [&](uint64_t buttons) {
        c.buttons = buttons;
        e.input.set(0, c);
    };
    CHECK_EQ(e.call_u32(66, h).result, 0u);
    e.tick(1'000'000);
    e.tick(100'000'000);
    CHECK(std::fabs(angle_z() - 0.025f) < 1e-5f);
    // A lone L waits for R, then reaches the game.
    press(btn::kButtonL);
    e.tick(10'000'000);
    CHECK_EQ(presses(), 0u);
    e.tick(100'000'000);
    CHECK_EQ(presses(), 1u);
    press(0);
    e.tick();
    // A short tap released while waiting still arrives, late.
    press(btn::kButtonR);
    e.tick(10'000'000);
    CHECK_EQ(presses(), 1u);
    press(0);
    e.tick(10'000'000);
    CHECK_EQ(presses(), 2u);
    e.tick(100'000'000);
    // L, then R a little later: a reset, and neither reaches the game.
    press(btn::kButtonL);
    e.tick(20'000'000);
    c.motion.gyro[2] = 0.0f;
    press(btn::kButtonL | btn::kButtonR);
    e.tick(10'000'000);
    CHECK(std::fabs(angle_z()) < 1e-6f);
    e.tick(200'000'000);
    press(btn::kButtonR);  // let go one after the other
    e.tick(10'000'000);
    press(0);
    e.tick(10'000'000);
    e.tick(100'000'000);
    CHECK_EQ(presses(), 2u);
    // A single right Joy-Con has SL/SR where the host has L/R.
    c.motion.gyro[2] = 0.25f;
    press(0);
    e.tick(100'000'000);
    CHECK(std::fabs(angle_z() - 0.025f) < 1e-5f);
    ControllerState right;
    right.style = ControllerStyle::JoyconRight;
    right.buttons = btn::kButtonRightSL | btn::kButtonRightSR;
    e.input.set(1, right);
    c.motion.gyro[2] = 0.0f;
    press(0);
    e.tick(10'000'000);
    CHECK(std::fabs(angle_z()) < 1e-6f);
    CHECK_EQ(e.hid->button_presses(npad_id::kNo1), 0u);
}

void test_vibration() {
    HidEnv e;
    e.input.set(0, handheld());
    e.map_shared_memory();
    e.tick();
    const uint32_t hl = sensor_handle(4, npad_id::kHandheld, 0);
    const uint32_t hr = sensor_handle(4, npad_id::kHandheld, 1);
    Msg info;
    info.arg<uint32_t>(hr);
    const Reply r = e.call(e.server, 200, info);
    CHECK_EQ(r.get<uint32_t>(0), 1u);  // linear resonant actuator
    CHECK_EQ(r.get<uint32_t>(4), 2u);  // right

    const Reply list = e.call(e.server, 203);
    CHECK_EQ(list.move.size(), 1u);
    Msg act;
    act.arg<uint32_t>(hl);
    CHECK_EQ(e.call(list.move.at(0), 0, act).result, 0u);

    const uint32_t handles[] = {hl, hr};
    const float values[] = {0.3f, 160.0f, 0.5f, 320.0f, 0.6f, 160.0f, 0.2f, 320.0f};
    CHECK(e.k.memory().copy_to_user(kBufA, handles, sizeof(handles)));
    CHECK(e.k.memory().copy_to_user(kBufA + 0x100, values, sizeof(values)));
    Msg send;
    send.arg<uint64_t>(kAruid);
    send.x.push_back({kBufA, sizeof(handles), 0});
    send.x.push_back({kBufA + 0x100, sizeof(values), 1});
    CHECK_EQ(e.call(e.server, 206, send).result, 0u);
    auto vib = e.input.vibration(0);
    CHECK(vib.first == 0.6f && vib.second == 0.5f);  // stronger side per motor

    Msg one;
    one.arg<uint32_t>(hr).arg<float>(0.0f).arg<float>(160.0f).arg<float>(0.0f).arg<float>(320.0f).arg<uint64_t>(kAruid);
    CHECK_EQ(e.call(e.server, 201, one).result, 0u);
    vib = e.input.vibration(0);
    CHECK(vib.first == 0.3f && vib.second == 0.5f);

    // A handle of a style the npad does not present (LGP sends zeros to the
    // JoyDual handle next to the real one) leaves the motors alone.
    Msg other;
    other.arg<uint32_t>(sensor_handle(5, npad_id::kHandheld, 1))
        .arg<float>(0.0f).arg<float>(160.0f).arg<float>(0.0f).arg<float>(320.0f).arg<uint64_t>(kAruid);
    CHECK_EQ(e.call(e.server, 201, other).result, 0u);
    vib = e.input.vibration(0);
    CHECK(vib.first == 0.3f && vib.second == 0.5f);
    // Counted for --input-log: three values reached a motor, one did not.
    const HidState::VibrationCounts n = e.hid->vibration_counts();
    CHECK_EQ(n.sent, 3u);
    CHECK_EQ(n.dropped_style, 1u);
    CHECK_EQ(n.dropped_npad, 0u);
}

// button_presses (and the --input-log line): every newly set bit of a sample
// the game got counts once, a held button not again.
void test_button_presses() {
    HidEnv e;
    e.map_shared_memory();
    e.input.set(0, handheld(btn::kButtonA));
    e.tick();
    e.tick();
    CHECK_EQ(e.hid->button_presses(npad_id::kHandheld), 1u);
    e.input.set(0, handheld(btn::kButtonA | btn::kButtonB));
    e.tick();
    CHECK_EQ(e.hid->button_presses(npad_id::kHandheld), 2u);
    e.input.set(0, handheld());
    e.tick();
    e.input.set(0, handheld(btn::kButtonA));
    e.tick();
    CHECK_EQ(e.hid->button_presses(npad_id::kHandheld), 3u);
    CHECK_EQ(e.hid->button_presses(npad_id::kNo1), 0u);  // no controller there
    CHECK_EQ(e.hid->button_presses(0x1234), 0u);         // not an npad id
}

void test_configuration() {
    HidEnv e;
    CHECK_EQ(e.call_u64(120, 1).result, 0u);  // SetNpadJoyHoldType(horizontal)
    CHECK_EQ(e.hid->joy_hold_type(), 1u);
    CHECK_EQ(e.call_aruid(121).get<uint64_t>(0), 1u);
    CHECK_EQ(e.call_u64(128, 2).result, 0u);  // SetNpadHandheldActivationMode(none)
    CHECK_EQ(e.call_aruid(129).get<uint64_t>(0), 2u);
    CHECK_EQ(e.call_u32(109, 2).result, 0u);  // ActivateNpadWithRevision
    CHECK_EQ(e.call_aruid(1).result, 0u);     // ActivateDebugPad
    CHECK_EQ(e.call_aruid(11).result, 0u);    // ActivateTouchScreen
    CHECK_EQ(e.call_u32(91, 1).result, 0u);   // ActivateGesture
    Msg palma;
    palma.arg<uint8_t>(1).arg<uint64_t>(kAruid);
    CHECK_EQ(e.call(e.server, 522, palma).result, 0u);  // SetIsPalmaAllConnectable
    Msg boost;
    boost.arg<uint8_t>(1);
    CHECK_EQ(e.call(e.server, 525, boost).result, 0u);  // SetPalmaBoostMode

    e.map_shared_memory();
    e.tick();
    const auto touch = e.lifo_view(kTouchLifo, 0x298);
    CHECK_EQ(touch.count, 1u);
    CHECK_EQ(e.read<int32_t>(touch.newest + 8 + 8), 0);  // no touches
    const auto pad = e.lifo_view(0, 0x28);
    CHECK_EQ(pad.count, 1u);
    CHECK_EQ(e.read<uint32_t>(pad.newest + 8 + 8), 0u);  // debug pad not connected

    // Commands the reference run never used stop the guest.
    e.call_aruid(1000, SvcOutcome::Unimplemented);
}

void test_no_backend() {
    // Without an IInput backend hid still answers and writes empty npads.
    HidEnv e(0);
    e.map_shared_memory();
    e.tick();
    CHECK_EQ(e.read<uint32_t>(e.npad(kHandheldIndex)), 0u);
    CHECK_EQ(e.lifo_view(e.lifo(kHandheldIndex, kHandheldLifo), 0x30).count, 1u);
}

lgp::platform::TouchScreenState touches(std::initializer_list<lgp::platform::TouchPoint> points) {
    lgp::platform::TouchScreenState t;
    for (const auto& p : points)
        t.points[t.count++] = p;
    return t;
}

// Touch screen: fingers with Start on their first sample and End once
// after they left, at console pixels.
void test_touch_screen() {
    HidEnv e;
    CHECK_EQ(e.call_aruid(11).result, 0u);  // ActivateTouchScreen
    e.map_shared_memory();
    e.input.set_touch(touches({{3, 100.4f, 200.6f}}));
    e.tick();
    auto v = e.lifo_view(kTouchLifo, kTouchEntry);
    uint64_t st = v.newest + 8;  // TouchScreenState
    CHECK_EQ(e.read<int32_t>(st + 8), 1);
    CHECK_EQ(e.read<uint32_t>(st + 0x10 + 8), 1u);   // attributes: Start
    CHECK_EQ(e.read<uint32_t>(st + 0x10 + 12), 3u);  // finger id
    CHECK_EQ(e.read<uint32_t>(st + 0x10 + 16), 100u);
    CHECK_EQ(e.read<uint32_t>(st + 0x10 + 20), 201u);
    CHECK(e.read<uint32_t>(st + 0x10 + 24) > 0u);  // diameter
    // Held and moved, a second finger joins at the far corner (clamped).
    e.input.set_touch(touches({{3, 110, 200}, {4, 1280, 720}}));
    e.tick();
    v = e.lifo_view(kTouchLifo, kTouchEntry);
    st = v.newest + 8;
    CHECK_EQ(e.read<int32_t>(st + 8), 2);
    CHECK_EQ(e.read<uint32_t>(st + 0x10 + 8), 0u);
    CHECK_EQ(e.read<uint32_t>(st + 0x10 + 16), 110u);
    CHECK_EQ(e.read<uint32_t>(st + 0x38 + 8), 1u);
    CHECK_EQ(e.read<uint32_t>(st + 0x38 + 16), 1279u);
    CHECK_EQ(e.read<uint32_t>(st + 0x38 + 20), 719u);
    // Finger 3 lifts: reported once more with End, then gone.
    e.input.set_touch(touches({{4, 1280, 720}}));
    e.tick();
    st = e.lifo_view(kTouchLifo, kTouchEntry).newest + 8;
    CHECK_EQ(e.read<int32_t>(st + 8), 2);
    CHECK_EQ(e.read<uint32_t>(st + 0x10 + 12), 4u);
    CHECK_EQ(e.read<uint32_t>(st + 0x38 + 8), 2u);  // End
    CHECK_EQ(e.read<uint32_t>(st + 0x38 + 12), 3u);
    CHECK_EQ(e.read<uint32_t>(st + 0x38 + 16), 110u);
    e.input.set_touch({});
    e.tick();
    e.tick();
    st = e.lifo_view(kTouchLifo, kTouchEntry).newest + 8;
    CHECK_EQ(e.read<int32_t>(st + 8), 0);
}

// Gestures from the same fingers: a tap, a double tap, a swipe up, a press.
void test_gestures() {
    HidEnv e;
    CHECK_EQ(e.call_u32(91, 1).result, 0u);  // ActivateGesture
    e.map_shared_memory();
    e.tick();
    CHECK_EQ(e.lifo_view(kGestureLifo, kGestureEntry).count, 0u);  // nothing while idle
    struct G {
        uint64_t context;
        uint32_t type, direction, x, y, attributes;
        float vy;
    };
    const auto newest = [&e] {
        const uint64_t g = e.lifo_view(kGestureLifo, kGestureEntry).newest + 8;
        return G{e.read<uint64_t>(g + 8),  e.read<uint32_t>(g + 16), e.read<uint32_t>(g + 20),
                 e.read<uint32_t>(g + 24), e.read<uint32_t>(g + 28), e.read<uint32_t>(g + 48),
                 e.read<float>(g + 44)};
    };
    constexpr uint32_t kTouchType = 3, kPress = 4, kTap = 5, kPan = 6, kSwipe = 7, kComplete = 1;
    // Tap: Touch (new) then Tap.
    e.input.set_touch(touches({{0, 640, 360}}));
    e.tick();
    G g = newest();
    CHECK_EQ(g.type, kTouchType);
    CHECK_EQ(g.attributes, 1u << 4);  // IsNewTouch
    CHECK_EQ(g.context, 1u);
    CHECK_EQ(g.x, 640u);
    e.input.set_touch({});
    e.tick();
    g = newest();
    CHECK_EQ(g.type, kTap);
    CHECK_EQ(g.attributes, 0u);
    const uint64_t count = e.lifo_view(kGestureLifo, kGestureEntry).count;
    e.tick();  // idle again: no new sample
    CHECK_EQ(e.lifo_view(kGestureLifo, kGestureEntry).count, count);
    // A second tap right away: a double tap, new context.
    e.input.set_touch(touches({{0, 645, 362}}));
    e.tick();
    CHECK_EQ(newest().context, 2u);
    e.input.set_touch({});
    e.tick();
    g = newest();
    CHECK_EQ(g.type, kTap);
    CHECK_EQ(g.attributes, 1u << 8);  // IsDoubleTap
    // Swipe up: 60 px per 5 ms tick.
    e.tick(500'000'000);
    for (int i = 0; i < 6; ++i) {
        e.input.set_touch(touches({{1, 600, 600.0f - 60.0f * i}}));
        e.tick();
    }
    g = newest();
    CHECK_EQ(g.type, kPan);
    CHECK(g.vy < -1000.0f);
    e.input.set_touch({});
    e.tick();
    g = newest();
    CHECK_EQ(g.type, kSwipe);
    CHECK_EQ(g.direction, 2u);  // Up
    CHECK_EQ(g.y, 300u);
    // Held still: Press after half a second; released: Complete.
    e.input.set_touch(touches({{2, 100, 100}}));
    for (int i = 0; i < 110; ++i)
        e.tick();
    CHECK_EQ(newest().type, kPress);
    e.input.set_touch({});
    e.tick();
    CHECK_EQ(newest().type, kComplete);
}

}  // namespace

int main() {
    RUN(test_handheld_lifo);
    RUN(test_supported_styles_and_events);
    RUN(test_joycon_assignment);
    RUN(test_disconnect);
    RUN(test_six_axis);
    RUN(test_six_axis_recenter);
    RUN(test_recenter_chord);
    RUN(test_six_axis_frozen_pose);
    RUN(test_vibration);
    RUN(test_button_presses);
    RUN(test_configuration);
    RUN(test_no_backend);
    RUN(test_touch_screen);
    RUN(test_gestures);
    return finish();
}
