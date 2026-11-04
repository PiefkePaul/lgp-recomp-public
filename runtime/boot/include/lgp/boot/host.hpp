// Host set-up of a boot executable before it reads any file: the platform
// layer defines it per target (lgp_add_boot_executable picks the file), so
// the boot program itself stays free of host code (P3.6).
//   desktop: nothing (runtime/boot/src/host_default.cpp)
//   web:     mounts the browser's OPFS at /opfs (platform/web/src/boot_host.cpp)
// The same file provides the host's default devices for hid and audout.
#pragma once

#include <string>
#include <vector>

#include "lgp/platform/audio.hpp"
#include "lgp/platform/input.hpp"
#include "lgp/platform/pad_input.hpp"
#include "lgp/platform/text_input.hpp"
#include "lgp/platform/video.hpp"

namespace lgp::boot {

// false (and *error) if the host cannot provide its file systems.
bool prepare_host(std::string* error);

// Controllers, sound and video decoding the host offers without options;
// nullptr where it has none. desktop: none (the boot program opens SDL and
// FFmpeg itself); web: Gamepad API + keyboard (P5.1), Web Audio (P5.2) and
// WebCodecs (P6, in-game videos). They live until the process ends.
// slots: controller mapping for the host's input, as the boot program built
// it from --controller, --stick & co.; only the first call uses it.
struct HostDevices {
    platform::IInput* input = nullptr;
    platform::IAudio* audio = nullptr;
    platform::IVideo* video = nullptr;
    platform::ITextInput* text = nullptr;  // software keyboard (web: a page dialog)
};
HostDevices host_devices(const std::vector<platform::MapperConfig>& slots);

// Called once right before main() returns. web: waits until the page has
// received everything this thread printed (Safari loses output that is still
// in flight when the runtime exits); desktop: nothing.
void finish_host();

// Default for --time-stats-tick (rt::set_time_stats_tick_us): desktop 0
// (exact clock); web 1000, Safari grew its memory by 11-20 MB/s with a
// performance.now() per switch.
uint32_t time_stats_tick_us();

}  // namespace lgp::boot
