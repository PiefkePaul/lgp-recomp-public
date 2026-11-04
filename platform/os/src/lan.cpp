// IPv4 UDP sockets of the host for ldn over LAN and the TCP connection to a
// RyuLDN server (lgp/platform/lan.hpp).
#include "lgp/platform/lan.hpp"

#include <cstdio>
#include <cstdlib>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
// winsock2.h before windows.h (iphlpapi.h pulls that in).
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <mstcpip.h>  // SIO_UDP_CONNRESET
#ifndef SIO_UDP_CONNRESET
// Some SDK versions only declare it for kernel-mode builds.
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

#include <mutex>
#elif !defined(__EMSCRIPTEN__)
#define LGP_LAN_POSIX 1
#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#endif
#if defined(__APPLE__)
#include <TargetConditionals.h>

#include "../../apple/src/lan_bonjour.hpp"
#endif
#include "lan_tls.hpp"

namespace lgp::platform {

bool parse_lan_endpoint(const std::string& text, LanEndpoint* out) {
    unsigned a = 0, b = 0, c = 0, d = 0, port = 0;
    char tail = 0;
    int n = std::sscanf(text.c_str(), "%u.%u.%u.%u:%u%c", &a, &b, &c, &d, &port, &tail);
    if (n != 5) {
        port = 0;
        n = std::sscanf(text.c_str(), "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail);
        if (n != 4)
            return false;
    }
    if (a > 255 || b > 255 || c > 255 || d > 255 || port > 65535)
        return false;
    out->ip = a << 24 | b << 16 | c << 8 | d;
    out->port = static_cast<uint16_t>(port);
    return true;
}

std::string lan_endpoint_string(const LanEndpoint& e) {
    char s[32];
    std::snprintf(s, sizeof(s), "%u.%u.%u.%u:%u", e.ip >> 24, e.ip >> 16 & 0xFF, e.ip >> 8 & 0xFF, e.ip & 0xFF,
                  e.port);
    return s;
}

#if defined(LGP_LAN_POSIX)
namespace {

class PosixLanSocket final : public ILanSocket {
public:
    PosixLanSocket(int fd, uint16_t port) : fd_(fd), port_(port) {}
    ~PosixLanSocket() override { ::close(fd_); }

    uint16_t port() const override { return port_; }

    bool send_to(const LanEndpoint& to, const void* data, size_t size) override {
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(to.port);
        a.sin_addr.s_addr = htonl(to.ip);
        for (;;) {
            const ssize_t n = ::sendto(fd_, data, size, 0, reinterpret_cast<const sockaddr*>(&a), sizeof(a));
            if (n >= 0)
                return static_cast<size_t>(n) == size;
            if (errno != EINTR)
                return false;
        }
    }

    int recv_from(void* data, size_t capacity, LanEndpoint* from, int timeout_ms) override {
        if (timeout_ms != 0) {  // < 0: no time limit
            pollfd p{fd_, POLLIN, 0};
            if (::poll(&p, 1, timeout_ms) <= 0)
                return -1;
        }
        sockaddr_in a{};
        socklen_t len = sizeof(a);
        ssize_t n;
        do
            n = ::recvfrom(fd_, data, capacity, 0, reinterpret_cast<sockaddr*>(&a), &len);
        while (n < 0 && errno == EINTR);
        if (n < 0)
            return -1;
        if (from != nullptr) {
            from->ip = ntohl(a.sin_addr.s_addr);
            from->port = ntohs(a.sin_port);
        }
        return static_cast<int>(n);
    }

private:
    int fd_;
    uint16_t port_;
};

class PosixLanStream final : public ILanStream {
public:
    explicit PosixLanStream(int fd) : fd_(fd) {}
    ~PosixLanStream() override { ::close(fd_); }

    bool send_all(const void* data, size_t size) override {
        const char* p = static_cast<const char*>(data);
        while (size != 0) {
#if defined(MSG_NOSIGNAL)
            const ssize_t n = ::send(fd_, p, size, MSG_NOSIGNAL);
#else
            const ssize_t n = ::send(fd_, p, size, 0);  // SO_NOSIGPIPE set on the socket
#endif
            if (n > 0) {
                p += n;
                size -= static_cast<size_t>(n);
                continue;
            }
            if (n < 0 && errno == EINTR)
                continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                pollfd q{fd_, POLLOUT, 0};
                if (::poll(&q, 1, 5000) <= 0)
                    return false;  // the peer stopped reading for 5 s
                continue;
            }
            return false;
        }
        return true;
    }

    int recv(void* data, size_t capacity, int timeout_ms) override {
        pollfd q{fd_, POLLIN, 0};
        int r;
        do
            r = ::poll(&q, 1, timeout_ms);
        while (r < 0 && errno == EINTR);
        if (r == 0)
            return 0;
        if (r < 0)
            return -1;
        ssize_t n;
        do
            n = ::recv(fd_, data, capacity, 0);
        while (n < 0 && errno == EINTR);
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return 0;
        return n > 0 ? static_cast<int>(n) : -1;
    }

    void shutdown() override { ::shutdown(fd_, SHUT_RDWR); }

    int wait_readable(int timeout_ms) override {
        pollfd q{fd_, POLLIN, 0};
        int r;
        do
            r = ::poll(&q, 1, timeout_ms);
        while (r < 0 && errno == EINTR);
        return r > 0 ? 1 : r == 0 ? 0 : -1;
    }

private:
    int fd_;
};

class PosixLan final : public ILan {
public:
#if defined(__APPLE__)
    PosixLan() : bonjour_(BonjourLan::create()) {}
    void advertise(uint16_t port) override {
        if (bonjour_)
            bonjour_->advertise(port);
    }
    void stop_advertising() override {
        if (bonjour_)
            bonjour_->stop_advertising();
    }
    void start_discovery() override {
        if (bonjour_)
            bonjour_->start_browsing();
    }
    void stop_discovery() override {
        if (bonjour_)
            bonjour_->stop_browsing();
    }
    std::vector<LanEndpoint> discovered() override { return bonjour_ ? bonjour_->found() : std::vector<LanEndpoint>{}; }
#if TARGET_OS_IPHONE
    // iOS/tvOS refuse broadcasts without the multicast entitlement, which
    // these apps do not have: discovery uses Bonjour and unicast only.
    bool can_broadcast() const override { return false; }
#endif

private:
    std::unique_ptr<BonjourLan> bonjour_;

public:
#endif
    std::unique_ptr<ILanSocket> open_udp(uint16_t port) override {
        const int fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (fd < 0)
            return nullptr;
        const int one = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));  // may be refused (iOS); not fatal
        // Games send bursts (LGP: several datagrams within a millisecond);
        // a large receive buffer keeps them while the pump thread catches up.
        const int rcvbuf = 1 << 20;
        ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
#if defined(SO_NOSIGPIPE)
        ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(port);
        a.sin_addr.s_addr = htonl(INADDR_ANY);
        socklen_t len = sizeof(a);
        if (::bind(fd, reinterpret_cast<const sockaddr*>(&a), sizeof(a)) != 0 ||
            ::getsockname(fd, reinterpret_cast<sockaddr*>(&a), &len) != 0 ||
            ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK) != 0) {
            ::close(fd);
            return nullptr;
        }
        return std::make_unique<PosixLanSocket>(fd, ntohs(a.sin_port));
    }

    std::unique_ptr<ILanStream> connect_tls(const std::string& host, uint16_t port, int timeout_ms,
                                            const std::string& ca_file, std::string* error) override {
        std::unique_ptr<ILanStream> tcp = connect_tcp(host, port, timeout_ms);
        if (!tcp) {
            if (error != nullptr)
                *error = "cannot reach " + host + ":" + std::to_string(port);
            return nullptr;
        }
        return tls_client(std::move(tcp), host, timeout_ms, ca_file, error);
    }

    std::unique_ptr<ILanStream> connect_tcp(const std::string& host, uint16_t port, int timeout_ms) override {
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* list = nullptr;
        const std::string service = std::to_string(port);
        if (::getaddrinfo(host.c_str(), service.c_str(), &hints, &list) != 0 || list == nullptr)
            return nullptr;
        std::unique_ptr<ILanStream> out;
        for (const addrinfo* a = list; a != nullptr && !out; a = a->ai_next) {
            const int fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (fd < 0)
                continue;
            const int one = 1;
            ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));  // small game packets
#if defined(SO_NOSIGPIPE)
            ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
            ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
            bool ok = ::connect(fd, a->ai_addr, a->ai_addrlen) == 0;
            if (!ok && errno == EINPROGRESS) {
                pollfd q{fd, POLLOUT, 0};
                int err = 0;
                socklen_t len = sizeof(err);
                ok = ::poll(&q, 1, timeout_ms) > 0 && ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) == 0 &&
                     err == 0;
            }
            if (ok)
                out = std::make_unique<PosixLanStream>(fd);
            else
                ::close(fd);
        }
        ::freeaddrinfo(list);
        return out;
    }

    std::vector<LanInterface> interfaces() override {
        std::vector<LanInterface> out;
        ifaddrs* list = nullptr;
        if (::getifaddrs(&list) != 0)
            return out;
        for (const ifaddrs* i = list; i != nullptr; i = i->ifa_next) {
            if (i->ifa_addr == nullptr || i->ifa_addr->sa_family != AF_INET || i->ifa_netmask == nullptr)
                continue;
            // Only broadcast-capable LANs (Wi-Fi, Ethernet): cellular data
            // and VPN tunnels are point-to-point and get no probes.
            if ((i->ifa_flags & IFF_UP) == 0 || (i->ifa_flags & IFF_LOOPBACK) != 0 ||
                (i->ifa_flags & IFF_POINTOPOINT) != 0 || (i->ifa_flags & IFF_BROADCAST) == 0)
                continue;
            LanInterface f;
            f.ip = ntohl(reinterpret_cast<const sockaddr_in*>(i->ifa_addr)->sin_addr.s_addr);
            f.mask = ntohl(reinterpret_cast<const sockaddr_in*>(i->ifa_netmask)->sin_addr.s_addr);
            out.push_back(f);
        }
        ::freeifaddrs(list);
        return out;
    }
};

}  // namespace

std::unique_ptr<ILan> make_host_lan() { return std::make_unique<PosixLan>(); }
#elif defined(_WIN32)
namespace {

// Winsock: the same as the POSIX version; WSAStartup once per process (never
// cleaned up, the sockets may live until exit).
bool winsock_ready() {
    static std::once_flag once;
    static bool ok = false;
    std::call_once(once, [] {
        WSADATA data;
        ok = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    });
    return ok;
}

class WinLanSocket final : public ILanSocket {
public:
    WinLanSocket(SOCKET s, uint16_t port) : s_(s), port_(port) {}
    ~WinLanSocket() override { ::closesocket(s_); }

    uint16_t port() const override { return port_; }

    bool send_to(const LanEndpoint& to, const void* data, size_t size) override {
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(to.port);
        a.sin_addr.s_addr = htonl(to.ip);
        const int n = ::sendto(s_, static_cast<const char*>(data), static_cast<int>(size), 0,
                               reinterpret_cast<const sockaddr*>(&a), sizeof(a));
        return n != SOCKET_ERROR && static_cast<size_t>(n) == size;
    }

    int recv_from(void* data, size_t capacity, LanEndpoint* from, int timeout_ms) override {
        if (timeout_ms != 0) {  // < 0: no time limit
            WSAPOLLFD p{};
            p.fd = s_;
            p.events = POLLRDNORM;
            if (::WSAPoll(&p, 1, timeout_ms) <= 0)
                return -1;
        }
        sockaddr_in a{};
        for (;;) {
            int len = sizeof(a);
            const int n = ::recvfrom(s_, static_cast<char*>(data), static_cast<int>(capacity), 0,
                                     reinterpret_cast<sockaddr*>(&a), &len);
            if (n == SOCKET_ERROR) {
                // A datagram larger than the buffer arrives cut (WSAEMSGSIZE);
                // an ICMP "port unreachable" from an earlier send shows up as
                // WSAECONNRESET on a UDP socket: skip it, it is no datagram.
                const int err = ::WSAGetLastError();
                if (err == WSAEMSGSIZE) {
                    if (from != nullptr) {
                        from->ip = ntohl(a.sin_addr.s_addr);
                        from->port = ntohs(a.sin_port);
                    }
                    return static_cast<int>(capacity);
                }
                if (err == WSAECONNRESET)
                    continue;
                return -1;
            }
            if (from != nullptr) {
                from->ip = ntohl(a.sin_addr.s_addr);
                from->port = ntohs(a.sin_port);
            }
            return n;
        }
    }

private:
    SOCKET s_;
    uint16_t port_;
};

class WinLanStream final : public ILanStream {
public:
    explicit WinLanStream(SOCKET s) : s_(s) {}
    ~WinLanStream() override { ::closesocket(s_); }

    bool send_all(const void* data, size_t size) override {
        const char* p = static_cast<const char*>(data);
        while (size != 0) {
            const int n = ::send(s_, p, static_cast<int>(size), 0);
            if (n > 0) {
                p += n;
                size -= static_cast<size_t>(n);
                continue;
            }
            if (n == SOCKET_ERROR && ::WSAGetLastError() == WSAEWOULDBLOCK) {
                WSAPOLLFD q{};
                q.fd = s_;
                q.events = POLLWRNORM;
                if (::WSAPoll(&q, 1, 5000) <= 0)
                    return false;
                continue;
            }
            return false;
        }
        return true;
    }

    int recv(void* data, size_t capacity, int timeout_ms) override {
        WSAPOLLFD q{};
        q.fd = s_;
        q.events = POLLRDNORM;
        const int r = ::WSAPoll(&q, 1, timeout_ms);
        if (r == 0)
            return 0;
        if (r < 0)
            return -1;
        const int n = ::recv(s_, static_cast<char*>(data), static_cast<int>(capacity), 0);
        if (n == SOCKET_ERROR && ::WSAGetLastError() == WSAEWOULDBLOCK)
            return 0;
        return n > 0 ? n : -1;
    }

    int wait_readable(int timeout_ms) override {
        WSAPOLLFD q{};
        q.fd = s_;
        q.events = POLLRDNORM;
        const int r = ::WSAPoll(&q, 1, timeout_ms);
        return r > 0 ? 1 : r == 0 ? 0 : -1;
    }

    void shutdown() override { ::shutdown(s_, SD_BOTH); }

private:
    SOCKET s_;
};

class WinLan final : public ILan {
public:
    std::unique_ptr<ILanStream> connect_tls(const std::string& host, uint16_t port, int timeout_ms,
                                            const std::string& ca_file, std::string* error) override {
        std::unique_ptr<ILanStream> tcp = connect_tcp(host, port, timeout_ms);
        if (!tcp) {
            if (error != nullptr)
                *error = "cannot reach " + host + ":" + std::to_string(port);
            return nullptr;
        }
        return tls_client(std::move(tcp), host, timeout_ms, ca_file, error);
    }

    std::unique_ptr<ILanStream> connect_tcp(const std::string& host, uint16_t port, int timeout_ms) override {
        if (!winsock_ready())
            return nullptr;
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* list = nullptr;
        const std::string service = std::to_string(port);
        if (::getaddrinfo(host.c_str(), service.c_str(), &hints, &list) != 0 || list == nullptr)
            return nullptr;
        std::unique_ptr<ILanStream> out;
        for (const addrinfo* a = list; a != nullptr && !out; a = a->ai_next) {
            const SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (s == INVALID_SOCKET)
                continue;
            const BOOL one = TRUE;
            ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));
            u_long nonblocking = 1;
            ::ioctlsocket(s, FIONBIO, &nonblocking);
            bool ok = ::connect(s, a->ai_addr, static_cast<int>(a->ai_addrlen)) == 0;
            if (!ok && ::WSAGetLastError() == WSAEWOULDBLOCK) {
                WSAPOLLFD q{};
                q.fd = s;
                q.events = POLLWRNORM;
                int err = 0;
                int len = sizeof(err);
                ok = ::WSAPoll(&q, 1, timeout_ms) > 0 && (q.revents & (POLLERR | POLLHUP)) == 0 &&
                     ::getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &len) == 0 && err == 0;
            }
            if (ok)
                out = std::make_unique<WinLanStream>(s);
            else
                ::closesocket(s);
        }
        ::freeaddrinfo(list);
        return out;
    }

    std::unique_ptr<ILanSocket> open_udp(uint16_t port) override {
        if (!winsock_ready())
            return nullptr;
        const SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s == INVALID_SOCKET)
            return nullptr;
        const BOOL one = TRUE;
        ::setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&one), sizeof(one));
        const int rcvbuf = 1 << 20;  // bursts, as in the POSIX version
        ::setsockopt(s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&rcvbuf), sizeof(rcvbuf));
        // Without this, an ICMP port unreachable makes later recvfrom calls
        // fail with WSAECONNRESET (probes to addresses without the app).
        DWORD off = FALSE, bytes = 0;
        ::WSAIoctl(s, SIO_UDP_CONNRESET, &off, sizeof(off), nullptr, 0, &bytes, nullptr, nullptr);
        // Exclusive: a second instance must not share the access point's
        // port (Windows would otherwise let it bind the same one).
        ::setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&one), sizeof(one));
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(port);
        a.sin_addr.s_addr = htonl(INADDR_ANY);
        int len = sizeof(a);
        u_long nonblocking = 1;
        if (::bind(s, reinterpret_cast<const sockaddr*>(&a), sizeof(a)) != 0 ||
            ::getsockname(s, reinterpret_cast<sockaddr*>(&a), &len) != 0 ||
            ::ioctlsocket(s, FIONBIO, &nonblocking) != 0) {
            ::closesocket(s);
            return nullptr;
        }
        return std::make_unique<WinLanSocket>(s, ntohs(a.sin_port));
    }

    std::vector<LanInterface> interfaces() override {
        std::vector<LanInterface> out;
        ULONG size = 16 * 1024;
        std::vector<unsigned char> buf;
        ULONG r = ERROR_BUFFER_OVERFLOW;
        for (int tries = 0; tries < 3 && r == ERROR_BUFFER_OVERFLOW; ++tries) {
            buf.resize(size);
            r = ::GetAdaptersAddresses(AF_INET,
                                       GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                       nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()), &size);
        }
        if (r != NO_ERROR)
            return out;
        for (const IP_ADAPTER_ADDRESSES* a = reinterpret_cast<const IP_ADAPTER_ADDRESSES*>(buf.data()); a != nullptr;
             a = a->Next) {
            // Wi-Fi and Ethernet that are up; no loopback, tunnels or PPP.
            if (a->OperStatus != IfOperStatusUp ||
                (a->IfType != IF_TYPE_ETHERNET_CSMACD && a->IfType != IF_TYPE_IEEE80211))
                continue;
            for (const IP_ADAPTER_UNICAST_ADDRESS* u = a->FirstUnicastAddress; u != nullptr; u = u->Next) {
                if (u->Address.lpSockaddr == nullptr || u->Address.lpSockaddr->sa_family != AF_INET ||
                    u->OnLinkPrefixLength > 32)
                    continue;
                LanInterface f;
                f.ip = ntohl(reinterpret_cast<const sockaddr_in*>(u->Address.lpSockaddr)->sin_addr.s_addr);
                f.mask = u->OnLinkPrefixLength == 0 ? 0 : 0xFFFFFFFFu << (32 - u->OnLinkPrefixLength);
                if ((f.ip >> 16) == 0xA9FE)
                    continue;  // 169.254/16: no DHCP answer, nobody to find there
                out.push_back(f);
            }
        }
        return out;
    }
};

}  // namespace

std::unique_ptr<ILan> make_host_lan() {
    if (!winsock_ready())
        return nullptr;
    return std::make_unique<WinLan>();
}
#else
std::unique_ptr<ILan> make_host_lan() { return nullptr; }
#endif

}  // namespace lgp::platform
