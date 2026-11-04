// The host's local network for local wireless play over a LAN ("ldn over
// LAN", docs/analysis/44-multiplayer-stand.md): plain IPv4 UDP sockets, which
// the runtime's ldn:u/bsd:u use to find and reach other instances of the app.
// The runtime only sees this interface; the OS code is in os/src/lan.cpp.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace lgp::platform {

// IPv4 address and UDP port, both in host byte order (127.0.0.1 = 0x7F000001).
struct LanEndpoint {
    uint32_t ip = 0;
    uint16_t port = 0;
    bool operator==(const LanEndpoint& o) const { return ip == o.ip && port == o.port; }
    bool operator!=(const LanEndpoint& o) const { return !(*this == o); }
};

// "a.b.c.d" and "a.b.c.d:port" (port 0 when missing); false when malformed.
bool parse_lan_endpoint(const std::string& text, LanEndpoint* out);
std::string lan_endpoint_string(const LanEndpoint& e);

// One IPv4 address of an interface that is up and can broadcast (Wi-Fi,
// Ethernet; no loopback, cellular or VPN tunnel).
struct LanInterface {
    uint32_t ip = 0;
    uint32_t mask = 0;
};

// A non-blocking UDP socket bound to all interfaces, broadcasts allowed where
// the host lets the app send them (iOS: only with the multicast entitlement).
class ILanSocket {
public:
    virtual ~ILanSocket() = default;
    virtual uint16_t port() const = 0;  // the bound port
    // false when the host refused the datagram (no route, not permitted).
    virtual bool send_to(const LanEndpoint& to, const void* data, size_t size) = 0;
    // Waits up to timeout_ms (0: does not wait, < 0: no limit) for one datagram. Returns its
    // size (cut to capacity) or -1 when none arrived.
    virtual int recv_from(void* data, size_t capacity, LanEndpoint* from, int timeout_ms) = 0;
};

// A connected TCP stream (RyuLDN server, lgp/hle ldn_ryu). Blocking sends;
// receives wait with a time limit. shutdown() may be called from another
// thread and makes a waiting recv() return -1.
class ILanStream {
public:
    virtual ~ILanStream() = default;
    // false when the connection is gone.
    virtual bool send_all(const void* data, size_t size) = 0;
    // Waits up to timeout_ms (< 0: no limit) for data. Returns the bytes read (> 0), 0 when none came in time,
    // -1 when the connection is closed or failed.
    virtual int recv(void* data, size_t capacity, int timeout_ms) = 0;
    virtual void shutdown() = 0;
    // Waits up to timeout_ms (< 0: no limit) until recv() would not wait: 1 then, 0 on timeout, -1 when closed.
    // Default: 1 (callers then wait in recv()). TLS uses it to wait without holding its lock.
    virtual int wait_readable(int /*timeout_ms*/) { return 1; }
};

class ILan {
public:
    virtual ~ILan() = default;
    // UDP socket on `port` (0: the host picks one); nullptr when that fails.
    virtual std::unique_ptr<ILanSocket> open_udp(uint16_t port) = 0;
    // The host's IPv4 interfaces, for broadcast and subnet probes.
    virtual std::vector<LanInterface> interfaces() = 0;

    // Service discovery where the host has it (Apple: Bonjour, without the
    // multicast entitlement broadcasts need). The access point announces its
    // port; stations collect announced endpoints and probe them by unicast.
    // Default: none (broadcast, /24 and peer probes only).
    virtual void advertise(uint16_t /*port*/) {}
    virtual void stop_advertising() {}
    virtual void start_discovery() {}
    virtual void stop_discovery() {}
    virtual std::vector<LanEndpoint> discovered() { return {}; }
    // false where the host refuses broadcasts (iOS, tvOS): no broadcast probes.
    virtual bool can_broadcast() const { return true; }

    // TCP connection to host (name or IPv4 address) and port, IPv4 only,
    // within timeout_ms; nullptr when that fails. Default: none.
    virtual std::unique_ptr<ILanStream> connect_tcp(const std::string& /*host*/, uint16_t /*port*/,
                                                    int /*timeout_ms*/) {
        return nullptr;
    }
    // The same with TLS on top (wss:// to the RyuLDN server): the handshake
    // checks the server's certificate for `host` against ca_file (PEM; empty:
    // the system's trusted roots). nullptr with the reason in *error when
    // the connection, the handshake or the check fails, or the build has no
    // TLS (os/src/lan_tls.cpp, Mbed TLS; Apple checks with its trust store).
    virtual std::unique_ptr<ILanStream> connect_tls(const std::string& /*host*/, uint16_t /*port*/,
                                                    int /*timeout_ms*/, const std::string& /*ca_file*/,
                                                    std::string* error) {
        if (error != nullptr)
            *error = "no TLS in this build";
        return nullptr;
    }
};

// Whether connect_tls can work in this build (Mbed TLS linked in).
bool lan_tls_available();

// The host's sockets (POSIX: macOS, iOS, Linux, Android; Winsock on
// Windows). nullptr in the browser (no UDP) or when the host has none.
std::unique_ptr<ILan> make_host_lan();

}  // namespace lgp::platform
