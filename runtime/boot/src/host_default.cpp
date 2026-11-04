#include "lgp/boot/host.hpp"

namespace lgp::boot {

bool prepare_host(std::string*) {
    return true;  // host paths work as given
}

HostDevices host_devices(const std::vector<platform::MapperConfig>&) {
    return {};
}

void finish_host() {}

uint32_t time_stats_tick_us() { return 0; }

}  // namespace lgp::boot
