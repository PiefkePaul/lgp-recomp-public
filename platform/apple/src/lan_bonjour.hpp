// Bonjour discovery for ldn over LAN on Apple hosts (lan_bonjour.cpp): the
// access point registers _lgp-ldn._udp with its UDP port, stations browse and
// resolve to IPv4 endpoints, which the scan then probes by unicast. iOS needs
// NSLocalNetworkUsageDescription and NSBonjourServices for this, but not the
// multicast entitlement that broadcasts need. Errors are only logged; the
// broadcast, /24 and --ldn-peer probes stay as the fallback.
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "lgp/platform/lan.hpp"

namespace lgp::platform {

class BonjourLan {
public:
    static std::unique_ptr<BonjourLan> create();  // nullptr if no dispatch queue
    virtual ~BonjourLan() = default;
    virtual void advertise(uint16_t port) = 0;
    virtual void stop_advertising() = 0;
    virtual void start_browsing() = 0;
    virtual void stop_browsing() = 0;
    virtual std::vector<LanEndpoint> found() = 0;
};

}  // namespace lgp::platform
