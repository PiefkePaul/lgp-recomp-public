// bsd:u (BSD sockets) and ssl (TLS) for a console without any network.
// Games initialise both as part of their communication menus, even for
// local wireless play (LGP: nn::ssl and nn::socket before a local trade).
// Command ids and layouts from switchbrew "Sockets services", "SSL
// services" and libnx bsd.c/socket.c (interface documentation only). Every
// call answers {s32 ret, s32 errno} plus its extra outputs; on failure ret
// is -1 and errno a Linux value (switchbrew: the errno values are Linux's).
//
// Sockets exist but reach nobody. Local wireless play runs IP over the ldn
// network the game created itself: as its host LGP opens a UDP socket right
// after ldn CreateNetwork and tears the network down again when that fails
// (PC run 2026-10-13: Socket(AF_INET, SOCK_DGRAM, UDP) -> error, Finalize,
// over and over). So a UDP socket works like one on a network without any
// peer: bind, connect, options and send succeed, sent datagrams vanish and
// nothing ever arrives (recv: EAGAIN). TCP connects fail (ENETUNREACH).
// Assumptions: that a real console creates sockets without a network at
// all (socket() needs no interface in FreeBSD), the errno choices, and
// EAGAIN for a blocking recv, which on a real console would wait forever;
// blocking inside the handler would hold the session lock (and with the
// baton scheduler the guest), so the call returns instead.
//
// With ldn over LAN (register_local_network, docs/analysis/44-multiplayer-
// stand.md) UDP sockets reach the other nodes of the ldn network: datagrams
// to an address of the ldn subnet (or a broadcast) go through the ldn Link,
// datagrams it received for a socket's bound port come out of Recv/RecvFrom,
// and Poll/Select/FIONREAD see them. Still without waiting: an empty queue
// answers EAGAIN as before.
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

#include "ldn_dual.hpp"
#include "ldn_lan.hpp"
#include "ldn_ryu.hpp"
#include "lgp/hle/services.hpp"
#include "lgp/runtime/log.hpp"

namespace lgp::hle {
namespace {

using rt::LogLevel;

// Linux errno values.
constexpr int32_t kEBADF = 9;
constexpr int32_t kEAGAIN = 11;
constexpr int32_t kEINVAL = 22;
constexpr int32_t kEAFNOSUPPORT = 97;
constexpr int32_t kEOPNOTSUPP = 95;
constexpr int32_t kENETUNREACH = 101;
constexpr int32_t kENOTCONN = 107;

constexpr int32_t kAfInet = 2;
constexpr int32_t kSockStream = 1, kSockDgram = 2;
constexpr int32_t kSockTypeMask = 0xFFFF;   // FreeBSD SOCK_NONBLOCK/SOCK_CLOEXEC above
constexpr int32_t kSockNonblock = 0x20000000;
constexpr int32_t kFGetfl = 3, kFSetfl = 4;
constexpr int32_t kNonblockNx = 0x800;  // O_NONBLOCK as bsd sees it (libnx socket.c)
constexpr uint32_t kFionbio = 0x8004667E, kFionread = 0x4004667F;
constexpr int16_t kPollIn = 0x1, kPollOut = 0x4, kPollNval = 0x20;
constexpr int32_t kFirstFd = 3;  // assumption: 0-2 stay free, as in a FreeBSD process

void ok(Response& resp, int32_t ret) {
    resp.push<int32_t>(ret);
    resp.push<int32_t>(0);
}

void socket_error(Response& resp, int32_t err) {
    resp.push<int32_t>(-1);
    resp.push<int32_t>(err);
}

struct Socket {
    int32_t type = 0;
    bool nonblocking = false;
    bool connected = false;
    std::array<uint8_t, 16> name{};  // sockaddr_in (FreeBSD: sin_len, sin_family, port, address)
    std::array<uint8_t, 16> peer{};  // UDP: the default destination set by Connect
};

uint16_t sockaddr_port(const std::array<uint8_t, 16>& a) { return static_cast<uint16_t>(a[2] << 8 | a[3]); }
uint32_t sockaddr_ip(const std::array<uint8_t, 16>& a) {
    return uint32_t{a[4]} << 24 | uint32_t{a[5]} << 16 | uint32_t{a[6]} << 8 | a[7];
}
std::array<uint8_t, 16> make_sockaddr(uint32_t ip, uint16_t port) {
    return {16, kAfInet, static_cast<uint8_t>(port >> 8), static_cast<uint8_t>(port), static_cast<uint8_t>(ip >> 24),
            static_cast<uint8_t>(ip >> 16), static_cast<uint8_t>(ip >> 8), static_cast<uint8_t>(ip)};
}

// The process's descriptors, shared by all bsd:u sessions (the SDK spreads
// its calls over several).
struct SocketTable {
    std::mutex mutex;
    std::map<int32_t, Socket> sockets;
    int32_t next_fd = kFirstFd;
    uint16_t next_port = 49152;
    bool warned_blocking_recv = false;
    std::shared_ptr<ldn::Link> link;  // nullptr: offline
};

class BsdService final : public Service {
public:
    BsdService(const char* name, std::shared_ptr<SocketTable> table) : Service(name), table_(std::move(table)) {
        // RegisterClient(LibraryConfigData 0x20, u64 pid placeholder, u64
        // transfer memory size, PID, copy handle) -> 4 bytes (switchbrew;
        // libnx reads 8 and passes them to StartMonitoring as a pid). What
        // the value means is not documented; the SDK aborts when it is not
        // 0 (PC run: a process id there ended in an abort right after this
        // call, inferred), so 0. The transfer memory backs socket buffers;
        // nothing is ever buffered here, so it is never touched.
        on(0, [this](Request&, Response& resp) {
            rt::log(LogLevel::Info, table_->link ? "bsd: RegisterClient (UDP reaches the ldn network over LAN)"
                                                 : "bsd: RegisterClient (no network: sockets reach nobody)");
            resp.push<uint64_t>(0);
        });
        on(1, [](Request&, Response&) {});  // StartMonitoring(PID, u64)
        // Socket, SocketExempt(s32 domain, s32 type, s32 protocol)
        for (const uint32_t cmd : {2u, 3u})
            on(cmd, [this](Request& req, Response& resp) {
                const int32_t domain = req.pop<int32_t>();
                const int32_t type = req.pop<int32_t>();
                const int32_t protocol = req.pop<int32_t>();
                const int32_t base = type & kSockTypeMask;
                if (domain != kAfInet || (base != kSockStream && base != kSockDgram)) {
                    rt::log(LogLevel::Warn, "bsd: Socket(%d, %d, %d) not supported", domain, type, protocol);
                    socket_error(resp, kEAFNOSUPPORT);
                    return;
                }
                std::lock_guard lock(table_->mutex);
                const int32_t fd = table_->next_fd++;
                Socket& s = table_->sockets[fd];
                s.type = base;
                s.nonblocking = (type & kSockNonblock) != 0;
                s.name[0] = 16;
                s.name[1] = kAfInet;
                rt::log(LogLevel::Info, "bsd: Socket(%d, %d, %d) -> %d%s", domain, type, protocol, fd,
                        table_->link ? "" : " (no peer will ever answer)");
                ok(resp, fd);
            });
        on(5, [this](Request& req, Response& resp) { select(req, resp); });
        on(6, [this](Request& req, Response& resp) { poll(req, resp); });
        // Recv(fd, flags), RecvFrom(fd, flags) (+ u32 addrlen), Read(fd)
        for (const uint32_t cmd : {8u, 9u, 25u})
            on(cmd, [this](Request& req, Response& resp) {
                const int32_t fd = req.pop<int32_t>();
                std::lock_guard lock(table_->mutex);
                Socket* s = find(fd);
                if (s == nullptr)
                    return socket_error(resp, kEBADF);
                if (s->type == kSockStream && !s->connected)
                    return socket_error(resp, kENOTCONN);
                ldn::Datagram d;
                if (s->type == kSockDgram && table_->link && table_->link->pop(sockaddr_port(s->name), &d)) {
                    const uint64_t room = req.out_buffer(0).size;
                    const size_t n = static_cast<size_t>(std::min<uint64_t>(d.data.size(), room));
                    req.write_out(0, d.data.data(), n);
                    ok(resp, static_cast<int32_t>(n));
                    if (req.command() == 9) {  // RecvFrom: the sender
                        const std::array<uint8_t, 16> from = make_sockaddr(d.src_ip, d.src_port);
                        req.write_out(1, from.data(), from.size());
                        resp.push<uint32_t>(static_cast<uint32_t>(from.size()));
                    }
                    return;
                }
                if (!s->nonblocking && !table_->warned_blocking_recv) {
                    table_->warned_blocking_recv = true;
                    rt::log(LogLevel::Warn, "bsd: blocking receive on %d answered EAGAIN (%s)", fd,
                            table_->link ? "does not wait for the LAN" : "nothing will arrive");
                }
                socket_error(resp, kEAGAIN);
            });
        // Send(fd, flags), SendTo(fd, flags), Write(fd): the data vanishes.
        for (const uint32_t cmd : {10u, 11u, 24u})
            on(cmd, [this](Request& req, Response& resp) {
                const int32_t fd = req.pop<int32_t>();
                std::lock_guard lock(table_->mutex);
                Socket* s = find(fd);
                if (s == nullptr)
                    return socket_error(resp, kEBADF);
                if (s->type == kSockStream && !s->connected)
                    return socket_error(resp, kENOTCONN);
                if (s->type == kSockDgram && table_->link) {
                    std::array<uint8_t, 16> to = s->peer;
                    if (req.command() == 11 && !req.read_in_as(1, &to))
                        to = s->peer;
                    if (table_->link->routes(sockaddr_ip(to))) {
                        if (sockaddr_port(s->name) == 0)
                            bind_port(*s, 0);
                        std::vector<uint8_t> data;
                        if (!req.read_in(0, &data))
                            return socket_error(resp, kEINVAL);
                        table_->link->send(sockaddr_port(s->name), sockaddr_ip(to), sockaddr_port(to), data.data(),
                                           data.size());
                    }
                }
                ok(resp, static_cast<int32_t>(req.in_buffer(0).size));
            });
        on(12, [this](Request& req, Response& resp) {  // Accept(fd) -> u32 addrlen: nobody calls
            const int32_t fd = req.pop<int32_t>();
            std::lock_guard lock(table_->mutex);
            if (find(fd) == nullptr)
                return socket_error(resp, kEBADF);
            socket_error(resp, kEAGAIN);
        });
        on(13, [this](Request& req, Response& resp) {  // Bind(fd, in sockaddr)
            const int32_t fd = req.pop<int32_t>();
            std::lock_guard lock(table_->mutex);
            Socket* s = find(fd);
            if (s == nullptr)
                return socket_error(resp, kEBADF);
            std::array<uint8_t, 16> addr{};
            if (!req.read_in_as(0, &addr))
                return socket_error(resp, kEINVAL);
            addr[0] = 16;
            addr[1] = kAfInet;
            s->name = addr;
            bind_port(*s, sockaddr_port(addr));
            rt::log(LogLevel::Info, "bsd: Bind(%d, %u.%u.%u.%u:%u)", fd, addr[4], addr[5], addr[6], addr[7],
                    sockaddr_port(s->name));
            ok(resp, 0);
        });
        on(14, [this](Request& req, Response& resp) {  // Connect(fd, in sockaddr)
            const int32_t fd = req.pop<int32_t>();
            std::lock_guard lock(table_->mutex);
            Socket* s = find(fd);
            if (s == nullptr)
                return socket_error(resp, kEBADF);
            if (s->type == kSockStream) {
                rt::log(LogLevel::Info, "bsd: TCP Connect(%d) -> ENETUNREACH (no network)", fd);
                return socket_error(resp, kENETUNREACH);
            }
            s->connected = true;  // UDP: only sets the default destination
            req.read_in_as(0, &s->peer);
            ok(resp, 0);
        });
        on(15, [this](Request& req, Response& resp) {  // GetPeerName(fd) -> u32 addrlen
            const int32_t fd = req.pop<int32_t>();
            std::lock_guard lock(table_->mutex);
            socket_error(resp, find(fd) == nullptr ? kEBADF : kENOTCONN);
        });
        on(16, [this](Request& req, Response& resp) {  // GetSockName(fd) -> u32 addrlen, out sockaddr
            const int32_t fd = req.pop<int32_t>();
            std::lock_guard lock(table_->mutex);
            Socket* s = find(fd);
            if (s == nullptr)
                return socket_error(resp, kEBADF);
            req.write_out(0, s->name.data(), s->name.size());
            ok(resp, 0);
            resp.push<uint32_t>(static_cast<uint32_t>(s->name.size()));
        });
        on(17, [this](Request& req, Response& resp) {  // GetSockOpt(fd, level, name) -> u32 optlen
            const int32_t fd = req.pop<int32_t>();
            std::lock_guard lock(table_->mutex);
            if (find(fd) == nullptr)
                return socket_error(resp, kEBADF);
            // Zero for everything; SO_ERROR 0 is the one games ask for.
            const int32_t zero = 0;
            req.write_out(0, &zero, sizeof(zero));
            ok(resp, 0);
            resp.push<uint32_t>(std::min<uint32_t>(4, static_cast<uint32_t>(req.out_buffer(0).size)));
        });
        on(18, [this](Request& req, Response& resp) {  // Listen(fd, backlog)
            const int32_t fd = req.pop<int32_t>();
            std::lock_guard lock(table_->mutex);
            const Socket* s = find(fd);
            if (s == nullptr)
                return socket_error(resp, kEBADF);
            if (s->type != kSockStream)
                return socket_error(resp, kEOPNOTSUPP);
            ok(resp, 0);
        });
        on(19, [this](Request& req, Response& resp) {  // Ioctl(fd, request, bufcount, 4 in, 4 out)
            const int32_t fd = req.pop<int32_t>();
            const uint32_t request = req.pop<uint32_t>();
            std::lock_guard lock(table_->mutex);
            Socket* s = find(fd);
            if (s == nullptr)
                return socket_error(resp, kEBADF);
            if (request == kFionbio) {
                int32_t enable = 0;
                req.read_in_as(0, &enable);
                s->nonblocking = enable != 0;
            } else if (request == kFionread) {
                const int next =
                    s->type == kSockDgram && table_->link ? table_->link->peek(sockaddr_port(s->name)) : -1;
                const int32_t bytes = next < 0 ? 0 : next;
                req.write_out(0, &bytes, sizeof(bytes));
            } else {
                rt::log(LogLevel::Warn, "bsd: Ioctl(%d, 0x%x) ignored", fd, request);
            }
            ok(resp, 0);
        });
        on(20, [this](Request& req, Response& resp) {  // Fcntl(fd, cmd, flags)
            const int32_t fd = req.pop<int32_t>();
            const int32_t cmd = req.pop<int32_t>();
            const int32_t flags = req.pop<int32_t>();
            std::lock_guard lock(table_->mutex);
            Socket* s = find(fd);
            if (s == nullptr)
                return socket_error(resp, kEBADF);
            if (cmd == kFGetfl)
                return ok(resp, s->nonblocking ? kNonblockNx : 0);
            if (cmd != kFSetfl)
                return socket_error(resp, kEINVAL);
            s->nonblocking = (flags & kNonblockNx) != 0;
            ok(resp, 0);
        });
        on(21, [this](Request& req, Response& resp) {  // SetSockOpt(fd, level, name, in value)
            const int32_t fd = req.pop<int32_t>();
            std::lock_guard lock(table_->mutex);
            if (find(fd) == nullptr)
                return socket_error(resp, kEBADF);
            ok(resp, 0);
        });
        on(22, [this](Request& req, Response& resp) {  // Shutdown(fd, how)
            const int32_t fd = req.pop<int32_t>();
            std::lock_guard lock(table_->mutex);
            if (find(fd) == nullptr)
                return socket_error(resp, kEBADF);
            ok(resp, 0);
        });
        on(23, [](Request&, Response& resp) { ok(resp, 0); });  // ShutdownAllSockets(how)
        on(26, [this](Request& req, Response& resp) {          // Close(fd)
            const int32_t fd = req.pop<int32_t>();
            std::lock_guard lock(table_->mutex);
            if (table_->sockets.erase(fd) == 0)
                return socket_error(resp, kEBADF);
            ok(resp, 0);
        });
        // Open (/dev/bpf), Sysctl, DuplicateSocket (bsd:s only), RecvMMsg,
        // SendMMsg: not seen in a game yet.
        for (const uint32_t cmd : {4u, 7u, 27u, 29u, 30u})
            on(cmd, [](Request& req, Response& resp) {
                rt::log(LogLevel::Warn, "bsd: command %u not supported", req.command());
                socket_error(resp, kEOPNOTSUPP);
            });
    }

private:
    // Port 0: pick one (as the stack does at bind or the first send).
    void bind_port(Socket& s, uint16_t port) {
        if (port == 0)
            port = table_->next_port++;
        s.name[0] = 16;
        s.name[1] = kAfInet;
        s.name[2] = static_cast<uint8_t>(port >> 8);
        s.name[3] = static_cast<uint8_t>(port);
    }

    bool readable(const Socket& s) {
        return s.type == kSockDgram && table_->link && table_->link->peek(sockaddr_port(s.name)) >= 0;
    }

    Socket* find(int32_t fd) {
        const auto it = table_->sockets.find(fd);
        return it == table_->sockets.end() ? nullptr : &it->second;
    }

    // Poll(nfds, timeout ms, in pollfd[], out pollfd[]): sockets are always
    // writable, readable only with a datagram from the ldn network. Returns at once even with a timeout (see
    // the blocking note at the top).
    void poll(Request& req, Response& resp) {
        struct PollFd {
            int32_t fd;
            int16_t events;
            int16_t revents;
        };
        std::vector<uint8_t> raw;
        if (!req.read_in(0, &raw))
            return socket_error(resp, kEINVAL);
        std::vector<PollFd> fds(raw.size() / sizeof(PollFd));
        std::memcpy(fds.data(), raw.data(), fds.size() * sizeof(PollFd));
        int32_t ready = 0;
        {
            std::lock_guard lock(table_->mutex);
            for (PollFd& p : fds) {
                p.revents = 0;
                if (p.fd < 0)
                    continue;
                const Socket* s = find(p.fd);
                if (s == nullptr)
                    p.revents = kPollNval;
                else if (s->type == kSockDgram || s->connected)
                    p.revents = static_cast<int16_t>(p.events & (kPollOut | (readable(*s) ? kPollIn : 0)));
                ready += p.revents != 0 ? 1 : 0;
            }
        }
        req.write_out(0, fds.data(), fds.size() * sizeof(PollFd));
        ok(resp, ready);
    }

    // Select(nfds, timeval, in read/write/except sets, out the same three):
    // readable with a datagram from the ldn network, every UDP socket
    // writable. Returns at once.
    void select(Request& req, Response& resp) {
        const int32_t nfds = req.pop<int32_t>();
        std::vector<uint8_t> sets[3];
        for (size_t i = 0; i < 3; ++i)
            req.read_in(i, &sets[i]);
        int32_t ready = 0;
        {
            std::lock_guard lock(table_->mutex);
            std::vector<uint8_t>& r = sets[0];
            for (int32_t fd = 0; fd < nfds && size_t(fd / 8) < r.size(); ++fd) {
                const uint8_t bit = static_cast<uint8_t>(1u << (fd % 8));
                if ((r[fd / 8] & bit) == 0)
                    continue;
                const Socket* s = find(fd);
                if (s != nullptr && readable(*s))
                    ++ready;
                else
                    r[fd / 8] &= static_cast<uint8_t>(~bit);
            }
            std::vector<uint8_t>& w = sets[1];
            for (int32_t fd = 0; fd < nfds && size_t(fd / 8) < w.size(); ++fd) {
                const uint8_t bit = static_cast<uint8_t>(1u << (fd % 8));
                if ((w[fd / 8] & bit) == 0)
                    continue;
                const Socket* s = find(fd);
                if (s != nullptr && (s->type == kSockDgram || s->connected))
                    ++ready;
                else
                    w[fd / 8] &= static_cast<uint8_t>(~bit);
            }
        }
        std::fill(sets[2].begin(), sets[2].end(), uint8_t{0});
        for (size_t i = 0; i < 3; ++i)
            req.write_out(i, sets[i].data(), sets[i].size());
        ok(resp, ready);
    }

    std::shared_ptr<SocketTable> table_;
};

class SslContext final : public Service {
public:
    SslContext() : Service("ssl:ISslContext") {
        on(0, [this](Request& req, Response&) {  // SetOption(u32 ContextOption, s32 value)
            const uint32_t option = req.pop<uint32_t>();
            const int32_t value = req.pop<int32_t>();
            if (option == 1)
                cross_process_ = value;
        });
        on(1, [this](Request& req, Response& resp) {  // GetOption(u32 ContextOption) -> s32
            const uint32_t option = req.pop<uint32_t>();
            resp.push<int32_t>(option == 1 ? cross_process_.load() : 0);
        });
        on(3, [](Request&, Response& resp) { resp.push<uint32_t>(0); });  // GetConnectionCount
    }

private:
    std::atomic<int32_t> cross_process_{0};
};

class SslService final : public Service {
public:
    SslService() : Service("ssl") {
        // CreateContext(u32 SslVersion, u64 pid placeholder, PID) -> ISslContext
        on(0, [](Request& req, Response& resp) {
            const uint32_t version = req.pop<uint32_t>();
            rt::log(LogLevel::Debug, "ssl: CreateContext(version 0x%x)", version);
            resp.push_object(std::make_shared<SslContext>());
        });
        on(5, [](Request& req, Response&) {  // SetInterfaceVersion(u32)
            rt::log(LogLevel::Debug, "ssl: SetInterfaceVersion(%u)", req.pop<uint32_t>());
        });
    }
};

}  // namespace

void register_sockets(ServiceManager& sm) {
    auto table = std::make_shared<SocketTable>();
    sm.register_service("bsd:u", [table] { return std::make_shared<BsdService>("bsd:u", table); });
    sm.register_service("ssl", [] { return std::make_shared<SslService>(); });
}

namespace ldn {
void register_bsd_service(ServiceManager& sm, std::shared_ptr<Link> link) {
    register_sockets(sm);
    auto table = std::make_shared<SocketTable>();
    table->link = std::move(link);
    sm.register_service("bsd:u", [table] { return std::make_shared<BsdService>("bsd:u", table); });
}
}  // namespace ldn

void register_local_network(ServiceManager& sm, const LdnLanConfig& config) {
    if (!config.lan) {
        register_ldn(sm);
        register_sockets(sm);
        return;
    }
    std::shared_ptr<ldn::Link> link;
    if (!config.ryuldn_server.empty() && config.ryuldn_dual)
        link = std::make_shared<ldn::DualLink>(std::make_shared<ldn::LanLink>(config),
                                               std::make_shared<ldn::RyuLink>(config));
    else if (!config.ryuldn_server.empty())
        link = std::make_shared<ldn::RyuLink>(config);
    else
        link = std::make_shared<ldn::LanLink>(config);
    ldn::register_ldn_service(sm, link);
    ldn::register_bsd_service(sm, link);
}

}  // namespace lgp::hle
