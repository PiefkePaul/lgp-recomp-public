# `hid` as HLE: Npads, shared memory, motion sensor, vibration

Cross-game knowledge from P5.1 part 1 (`runtime/hle/src/hid.cpp`, `hid_shared_memory.hpp`, tests `runtime.hle_hid`). Command numbers, argument layouts and the shared memory layout come from switchbrew ("HID services", "HID Shared Memory") and libnx `hid.h`/`hid.c` (only as interface description, no code taken). Which commands exist follows the Ryujinx reference run of LGP; everything else halts the guest.

## Basic principle

- **Inputs never flow over IPC.** `IHidServer` only configures (which Npads, which styles, hold type, sensors on/off). The SDK reads the states itself from a 0x40000-byte shared memory that it gets via `CreateAppletResource` → `IAppletResource::GetSharedMemoryHandle` and maps **read-only**. `GetNpadState`, `GetSixAxisSensorState`, `GetNpadStyleSet` etc. are pure SDK functions without IPC.
- The runtime therefore needs a **writer on the host clock**: `HidState::update(kernel, now_ns)` polls `IInput` and appends a new entry to each LIFO. Who calls `update` how often is up to the embedding program (a host thread, e.g. every 5 ms or once per frame). The real console samples Npads every few milliseconds; the exact rate has not been measured here.
- Writes first hold the `HidState` mutex, then `Kernel::memory_mutex()`, because `KSharedMemory::data()` is only valid until the next map/unmap. IPC handlers take the same order; there is no reverse path.

## Layout of the shared memory

| Offset | Size | Content |
|---|---:|---|
| 0x00000 | 0x400 | DebugPad |
| 0x00400 | 0x3000 | TouchScreen |
| 0x03400 / 0x03800 | 0x400 each | Mouse, Keyboard |
| 0x03C00 | 0x1000 | BasicXpad (Digitizer from 10.0.0) |
| 0x04C00 / 0x04E00 / 0x05000 | 0x200 each | Home, Sleep, Capture button |
| 0x05200 | 0x800 | InputDetector |
| 0x05A00 | 0x4000 | UniquePad (up to 4.1.0) |
| 0x09A00 | 10 × 0x5000 | Npads: index 0–7 = No1–No8, 8 = Handheld, 9 = Other |
| 0x3BA00 | 0x800 | Gesture |
| 0x3C200 | 0x20 | ConsoleSixAxisSensor (from 5.0.0) |

**Npad entry** (`NpadInternalState`, 0x43F0 bytes + 0xC10 padding): `style_set` (0x0), `joy_assignment_mode` (0x4: 0 Dual, 1 Single), FullKey color (0x8: attribute + body/buttons), Joy colors (0x14: attribute + left + right), seven button LIFOs from 0x28, 0x350 each (FullKey, Handheld, JoyDual, JoyLeft, JoyRight, Palma, SystemExt), six motion LIFOs from 0x1758, 0x708 each (FullKey, Handheld, JoyDual left, JoyDual right, JoyLeft, JoyRight), `device_type` (0x4188), `system_properties` (0x4190), battery levels (0x419C, 0–4), then footer/NFC union, GC trigger LIFO (0x4228) and the Lark/Lucia/Lager types. The offsets are pinned in the header via `static_assert` and were compiled once against the libnx structures (all identical).

**LIFO protocol** (applies to all sources): header `{unused, buffer_count = 17, tail, count}`, then 17 entries `{sampling_number, state}`, where `state` begins with the same `sampling_number`. The writer puts the new entry at `(tail + 1) % 17`, then `tail`, then `count` (at most 16). The reader takes `tail` as the newest entry and walks back `count` entries; it detects torn reads by unequal sampling numbers and gaps by consecutive entries not increasing by exactly 1. Because `count` stays ≤ 16, the reader never reads the slot currently being written.

## Which controller appears where

- The runtime maps `IInput` slot *i* to Npad No(*i*+1); a slot in style `Handheld` always lands on the Handheld Npad (with two Handheld slots the first wins).
- A controller only appears if its style is in `SetSupportedNpadStyleSet` and its Npad ID in `SetSupportedNpadIdType`. Default before the first call: all standard styles (FullKey, Handheld, JoyDual, JoyLeft, JoyRight) and all IDs. **This is how LGP rejects the Pro Controller and the Joy-Con pair**, without the runtime having to know the game.
- Single Joy-Cons: in assignment mode Dual (default) a single left Joy-Con shows the style `JoyDual` with only `IsLeftConnected`; only `SetNpadJoyAssignmentModeSingle(ByDefault)` turns it into `JoyLeft` or `JoyRight`. LGP sets Single for its player Npads.
- Every tick writes into **all seven** button LIFOs of each Npad: the real state into the LIFO of the shown style (and mirrored into SystemExt), an empty state into the others. This keeps all LIFOs current, no matter which one the SDK reads.
- Attributes in the button state: bit 0 connected, 1 wired, 2/3 left connected/wired, 4/5 right. Handheld sets all six, single Joy-Cons "connected + their side".
- `device_type`: Handheld = HandheldLeft|HandheldRight (bits 2, 3), JoyLeft bit 4, JoyRight bit 5, FullKey bit 0. `system_properties`: bits 3–5 "powered" (Dual/left/right), 11 ABXY-oriented, 12 SL/SR-oriented, 13/14 Plus/Minus present, 15 direction buttons present.
- Without a controller: style 0, color attribute `NoController` (2), device type and battery 0.
- `DisconnectNpad` disconnects the Npad until the slot shows something else (unplugged or another style) **or a button is newly pressed there**. A real console also requires a reconnect; a Joy-Con re-registers by button press (SL+SR). A host pad cannot be unplugged; without the button press the Npad would stay disconnected forever. In one-Joy-Con mode LGP disconnects Npad No1 and waits for exactly this re-registration (found on the PC on 2026-10-13). A button already held when disconnecting does not count; only releasing and pressing again. hid also reports the disconnect to the backend (`IInput::npad_disconnected(slot)`): `SdlInput` then lets the next new button press on any pad choose player 1, like the controller that registers first on the console (`host-input-auf-switch-controller.md`, "Wer Spieler 1 ist" (who player 1 is)).
- `AcquireNpadStyleSetUpdateEventHandle` returns a new copy handle to the Npad's event on every call and signals it immediately, like Ryujinx. After that every change of the shown style signals. Whether real hardware signals on acquisition has not been checked.

## Motion sensor (SixAxisSensor)

- Handle (u32, same for vibration): bits 0–7 style index (3 FullKey, 4 Handheld, 5 JoyDual, 6 JoyLeft, 7 JoyRight), bits 8–15 Npad ID (Handheld = 0x20), bits 16–23 device (0 left, 1 right, 2 both). The SDK computes the handles locally (`GetSixAxisSensorHandles` is not IPC).
- Entries are only produced between `StartSixAxisSensor` and `StopSixAxisSensor`. In the reference run LGP calls `Stop` over 11,000 times (apparently every frame), `Start` only 4 times.
- State: `delta_time` in ns, acceleration in G, angular velocity in revolutions/s, `angle` = integrated revolutions, `direction` = 3×3 orientation matrix (here propagated from the angular velocity, Rodrigues rotation + Gram-Schmidt), attribute bit 0 "connected".
- **Without motion data** (`MotionState::valid == false`) or if the Npad does not show the handle's style, the runtime writes entries with attribute 0 instead of an invented rest pose.
- `IsSixAxisSensorAtRest` here is "all three axes below 0.005 rev/s" or "no data". Fusion parameters (default 0.03/0.4) and zero-drift mode (default 1) are only stored.

## Vibration

- `GetVibrationDeviceInfo(handle)` returns `{type 1 = linear resonant actuator, position 1 left / 2 right}`. `CreateActiveVibrationDeviceList` → `ActivateVibrationDevice(handle)` is a pure acknowledgment.
- `SendVibrationValue(s)`: values `{amp_low, freq_low, amp_high, freq_high}` (f32). The runtime passes the amplitudes on to `IInput::set_vibration(slot, low, high)`; with two motors on one Npad the stronger side wins per band. Only handles of the style the Npad currently shows reach a motor: in one-Joy-Con mode LGP sends its values to the JoyRight handle and at the same time constant zeros to the JoyDual handle of the same Npad. Without this check the zeros overwrote the real values. In Handheld style LGP sends no vibration at all. Frequencies are lost, because host gamepads mostly know only two amplitudes.

## Implemented commands (`IHidServer`)

0 `CreateAppletResource`, 1 `ActivateDebugPad`, 11 `ActivateTouchScreen`, 66/67 `Start`/`StopSixAxisSensor`, 68–72 fusion, 79–81 zero drift, 82 `IsSixAxisSensorAtRest`, 91 `ActivateGesture`, 100/101 supported style set, 102 `SetSupportedNpadIdType` (X buffer with u32 IDs), 103/104/109 `(De)ActivateNpad(WithRevision)`, 106 `AcquireNpadStyleSetUpdateEventHandle`, 107 `DisconnectNpad`, 120/121 hold type, 122/123/124 Joy assignment, 128/129 Handheld activation, 200/201/203/206 vibration, 522 `SetIsPalmaAllConnectable`, 525 `SetPalmaBoostMode`.

Argument layouts as in the libnx structures: `(u32, pad, aruid)`, but `(aruid, u64)` for 120/128, `(u8, pad, u32, aruid)` for 69, `(handle, f32, f32, pad, aruid)` for 70. An unknown Npad ID halts the guest, because the console's answer to it is not documented.

## Open points

- **Horizontal hold (`SetNpadJoyHoldType(1)`)**: whether the console itself rotates the stick axes and buttons of a single Joy-Con held sideways or the game does it has not been checked. `IInput` therefore delivers "as the game should see it" for now. Check on the PC with the real game (P5.1, acceptance test). How the host backends map keyboard and gamepads onto `IInput`: [`host-input-auf-switch-controller.md`](host-input-auf-switch-controller.md).
- DebugPad gets empty entries after activation (no DebugPad). Touchscreen and gestures come from `IInput::touch_screen()`: touch points with `Start`/`End`, gestures from our own unverified model (`hid_gesture.hpp`, details in [`host-input-auf-switch-controller.md`](host-input-auf-switch-controller.md#touchscreen-handy-tablet)). Without a finger the touch entries stay empty, and the gesture LIFO gets no entries.
- **Poké Ball Plus (Palma)** is missing: `ControllerStyle` does not know it, and the Palma commands except 522/525 halt the guest.
- The colors of single Joy-Cons in `full_key_color` and the battery levels (always full) are plausible assumptions, not measured.
