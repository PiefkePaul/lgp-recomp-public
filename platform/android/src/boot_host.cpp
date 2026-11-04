// prepare_host() and host_devices() of boot programs on Android (P7.3,
// lgp/boot/host.hpp): paths work as given; in-game videos decode through
// MediaCodec. Controllers and sound come from SDL3 in the app build (the
// boot program opens them itself, as on the desktop).
#include "lgp/boot/host.hpp"
#include "lgp/platform/android/mediacodec_video.hpp"

namespace lgp::boot {

bool prepare_host(std::string*) {
    return true;
}

HostDevices host_devices(const std::vector<platform::MapperConfig>&) {
    static platform::android::MediaCodecVideo video;
    HostDevices d;
    d.video = &video;
    return d;
}

void finish_host() {}

uint32_t time_stats_tick_us() { return 0; }

}  // namespace lgp::boot
