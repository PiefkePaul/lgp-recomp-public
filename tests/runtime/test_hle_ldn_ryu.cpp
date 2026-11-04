// ldn over a RyuLDN server (docs/knowledge/ldn-ueber-ryuldn.md): two
// runtimes in one process connect to a server on 127.0.0.1; one opens a
// room, the other scans and joins it, bsd:u datagrams go through the server.
// The server is a small one in this file that speaks the protocol the way
// the RyuLDN server does (own code, after the packet layouts of the MIT
// server github.com/Ryubing/LdnServer). With LGP_RYULDN_SERVER=HOST:PORT the
// same runs against a real server instead (manual check, not in CI).
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using SocketFd = SOCKET;
constexpr SocketFd kNoSocket = INVALID_SOCKET;
void close_fd(SocketFd s) { ::closesocket(s); }
int poll_fds(WSAPOLLFD* p, size_t n, int ms) { return ::WSAPoll(p, static_cast<ULONG>(n), ms); }
using PollFd = WSAPOLLFD;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
using SocketFd = int;
constexpr SocketFd kNoSocket = -1;
void close_fd(SocketFd s) { ::close(s); }
int poll_fds(pollfd* p, size_t n, int ms) { return ::poll(p, static_cast<nfds_t>(n), ms); }
using PollFd = pollfd;
#endif

#include "ipc_client.hpp"
#include "lgp/hle/services.hpp"
#include "lgp/platform/lan.hpp"

using namespace lgp_test;
namespace platform = lgp::platform;

namespace {

constexpr size_t kInfoSize = 0x480;
constexpr int64_t kLcid = 0x0100'0000'0000'1234;
constexpr int16_t kVersion = 88;
constexpr int16_t kChannel = 6;

// --- A small RyuLDN server ------------------------------------------------

// Byte offsets in NetworkInfo (ldn_lan.hpp) and the packet bodies.
constexpr size_t kInfoSession = 0x10, kInfoChannel = 0x48, kInfoNodeCountMax = 0x66, kInfoNodeCount = 0x67,
                 kInfoNodes = 0x68, kInfoAdvSize = 0x26A, kInfoAdv = 0x26C, kInfoAccept = 0x62;
constexpr size_t kNodeSize = 0x40;

class FakeServer {
public:
    explicit FakeServer(uint16_t port = 0) {
#if defined(_WIN32)
        WSADATA data;
        WSAStartup(MAKEWORD(2, 2), &data);
#endif
        listen_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(0x7F000001);
        a.sin_port = htons(port);
        socklen_t len = sizeof(a);
        if (::bind(listen_, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0 || ::listen(listen_, 8) != 0 ||
            ::getsockname(listen_, reinterpret_cast<sockaddr*>(&a), &len) != 0) {
            close_fd(listen_);
            listen_ = kNoSocket;
            return;
        }
        port_ = ntohs(a.sin_port);
        thread_ = std::thread([this] { run(); });
    }
    ~FakeServer() {
        stop_ = true;
        if (thread_.joinable())
            thread_.join();
        for (auto& c : clients_)
            close_fd(c.fd);
        if (listen_ != kNoSocket)
            close_fd(listen_);
    }
    uint16_t port() const { return port_; }
    int pings_answered() const { return pings_answered_; }
    // Closes every client connection (the server went away for them).
    void drop_all() { drop_ = true; }
    // Leaves scans unanswered (a server that answers late).
    void ignore_scans(bool on) { ignore_scans_ = on; }

private:
    struct Client {
        SocketFd fd;
        std::vector<uint8_t> in;
        uint8_t mac[6]{};
        uint32_t ip = 0;
        std::string passphrase;
        std::string game;  // session id (hex) of the room it is in
        int node = -1;
        bool pinged = false;
    };
    struct Game {
        std::vector<uint8_t> info = std::vector<uint8_t>(kInfoSize, 0);
        SocketFd owner = kNoSocket;
        std::string passphrase;
    };

    static void put32(std::vector<uint8_t>& v, size_t at, uint32_t x) { std::memcpy(v.data() + at, &x, 4); }

    void send(Client& c, uint8_t type, const void* body, size_t size) {
        std::vector<uint8_t> p(12 + size, 0);
        put32(p, 0, 'R' | 'L' << 8 | 'D' << 16 | 'N' << 24);
        p[4] = type;
        p[5] = 1;
        put32(p, 8, static_cast<uint32_t>(size));
        if (size != 0)
            std::memcpy(p.data() + 12, body, size);
        ::send(c.fd, reinterpret_cast<const char*>(p.data()), static_cast<int>(p.size()), 0);
    }
    void error(Client& c, int32_t e) { send(c, 255, &e, 4); }
    Client* by_fd(SocketFd fd) {
        for (auto& c : clients_)
            if (c.fd == fd)
                return &c;
        return nullptr;
    }
    static std::string hex(const uint8_t* id) {
        static const char k[] = "0123456789abcdef";
        std::string s;
        for (int i = 0; i < 16; ++i) {
            s += k[id[i] >> 4];
            s += k[id[i] & 0xF];
        }
        return s;
    }
    void sync(Game& g) {
        for (auto& c : clients_)
            if (!c.game.empty() && games_.count(c.game) && &games_[c.game] == &g)
                send(c, 7, g.info.data(), kInfoSize);  // SyncNetwork
    }
    // Adds c as a node: address, ProxyConfig, then the caller sends Connected.
    void add_node(Game& g, Client& c, const uint8_t* user_name, uint16_t version, const std::string& id) {
        int node = 0;
        while (g.info[kInfoNodes + kNodeSize * node + 0xB] != 0)
            ++node;
        c.ip = 0x0A720000u | ++next_ip_;
        const uint32_t cfg[2] = {c.ip, 0xFFFF0000u};
        send(c, 17, cfg, sizeof(cfg));  // ProxyConfig
        uint8_t* n = g.info.data() + kInfoNodes + kNodeSize * node;
        std::memset(n, 0, kNodeSize);
        std::memcpy(n, &c.ip, 4);
        std::memcpy(n + 4, c.mac, 6);
        n[0xA] = static_cast<uint8_t>(node);
        n[0xB] = 1;
        std::memcpy(n + 0xC, user_name, 0x20);
        std::memcpy(n + 0x2E, &version, 2);
        ++g.info[kInfoNodeCount];
        c.game = id;
        c.node = node;
    }
    void leave(Client& c) {
        if (c.game.empty() || !games_.count(c.game))
            return;
        Game& g = games_[c.game];
        const std::string id = c.game;
        c.game.clear();
        if (g.owner == c.fd) {
            for (auto& o : clients_)
                if (o.game == id) {
                    const uint32_t zero = 0;
                    send(o, 16, &zero, 4);  // Disconnect
                    o.game.clear();
                }
            games_.erase(id);
            return;
        }
        std::memset(g.info.data() + kInfoNodes + kNodeSize * c.node + 0xB, 0, 1);
        --g.info[kInfoNodeCount];
        sync(g);
    }

    void handle(Client& c, uint8_t type, const uint8_t* b, size_t size) {
        if (type == 10 && ignore_scans_)
            return;
        switch (type) {
        case 0: {  // Initialize
            for (int i = 0; i < 6; ++i)
                c.mac[i] = static_cast<uint8_t>(0x10 * (c.fd & 0xF) + i + 1);
            uint8_t reply[0x16]{};
            reply[0] = static_cast<uint8_t>(c.fd);
            std::memcpy(reply + 0x10, c.mac, 6);
            send(c, 0, reply, sizeof(reply));
            send(c, 254, "\0\x2A", 2);  // Ping (requester 0, id 42): expects the same back
            c.pinged = true;
            return;
        }
        case 1:  // Passphrase
            c.passphrase.assign(reinterpret_cast<const char*>(b), strnlen(reinterpret_cast<const char*>(b), size));
            return;
        case 254:  // the answer to our ping
            if (size >= 2 && b[0] == 0 && b[1] == 0x2A && c.pinged) {
                c.pinged = false;
                ++pings_answered_;
            }
            return;
        case 3: {  // CreateAccessPointPrivate: Security 0x44, Parameter 0x20, User 0x30, Network 0x20 at 0x94, ...
            if (size < 0x13C || !c.game.empty())
                return error(c, -1);
            const uint8_t* session = b + 0x44 + 0x10;
            const std::string id = hex(session);
            if (games_.count(id))
                return error(c, -1);  // id taken
            Game& g = games_[id];
            g.owner = c.fd;
            g.passphrase = c.passphrase;
            const uint8_t* net = b + 0x94;
            std::memcpy(g.info.data(), net, 0x10);  // intent id
            std::memcpy(g.info.data() + kInfoSession, session, 0x10);
            std::memcpy(g.info.data() + 0x20, c.mac, 6);
            g.info[0x26] = 32;
            std::memset(g.info.data() + 0x27, '1', 32);
            std::memcpy(g.info.data() + kInfoChannel, net + 0x10, 2);
            g.info[0x4A] = 3;
            g.info[0x4B] = 2;
            std::memcpy(g.info.data() + 0x60, b, 2);  // security mode
            g.info[kInfoNodeCountMax] = net[0x12];
            const size_t adv = size - 0x13C;
            const uint16_t adv16 = static_cast<uint16_t>(adv);
            std::memcpy(g.info.data() + kInfoAdvSize, &adv16, 2);
            std::memcpy(g.info.data() + kInfoAdv, b + 0x13C, adv);
            uint16_t version = 0;
            std::memcpy(&version, net + 0x14, 2);
            add_node(g, c, b + 0x64, version, id);
            send(c, 15, g.info.data(), kInfoSize);  // Connected
            return;
        }
        case 10: {  // Scan(ScanFilter)
            if (size < 0x60)
                return;
            int64_t lcid = 0;
            std::memcpy(&lcid, b, 8);
            for (auto& [id, g] : games_) {
                int64_t have = 0;
                std::memcpy(&have, g.info.data(), 8);
                if (g.passphrase != c.passphrase || g.owner == c.fd || g.info[kInfoAccept] == 1)
                    continue;
                if ((b[0x5C] & 1) && have != lcid)
                    continue;
                send(c, 11, g.info.data(), kInfoSize);
            }
            send(c, 12, nullptr, 0);
            return;
        }
        case 14: {  // ConnectPrivate: Security 0x44, Parameter 0x20, User 0x30, u32 version at 0x94, ...
            if (size < 0xC0)  // NetworkConfig at 0xA0 (8-byte aligned in the server's layout)
                return;
            const std::string id = hex(b + 0x44 + 0x10);
            if (!games_.count(id))
                return error(c, 6);
            Game& g = games_[id];
            uint32_t version = 0;
            std::memcpy(&version, b + 0x94, 4);
            uint16_t host_version = 0;
            std::memcpy(&host_version, g.info.data() + kInfoNodes + 0x2E, 2);
            if (version != host_version)
                return error(c, version < host_version ? 3 : 4);
            if (g.info[kInfoNodeCount] >= g.info[kInfoNodeCountMax])
                return error(c, 2);
            add_node(g, c, b + 0x64, static_cast<uint16_t>(version), id);
            for (auto& o : clients_)
                if (o.fd != c.fd && o.game == id)
                    send(o, 7, g.info.data(), kInfoSize);
            send(c, 15, g.info.data(), kInfoSize);
            return;
        }
        case 16:  // Disconnect
            leave(c);
            return;
        case 22:  // SetAcceptPolicy
            if (size >= 1 && games_.count(c.game) && games_[c.game].owner == c.fd) {
                games_[c.game].info[kInfoAccept] = b[0];
                sync(games_[c.game]);
            }
            return;
        case 23:  // SetAdvertiseData
            if (games_.count(c.game) && games_[c.game].owner == c.fd) {
                Game& g = games_[c.game];
                const uint16_t n = static_cast<uint16_t>(std::min<size_t>(size, 0x180));
                std::memcpy(g.info.data() + kInfoAdvSize, &n, 2);
                std::memset(g.info.data() + kInfoAdv, 0, 0x180);
                std::memcpy(g.info.data() + kInfoAdv, b, n);
                sync(g);
            }
            return;
        case 20: {  // ProxyData: src ip, src port, dst ip, dst port, protocol, length, data
            if (size < 0x14 || c.game.empty())
                return;
            uint32_t src = 0, dst = 0;
            std::memcpy(&src, b, 4);
            std::memcpy(&dst, b + 6, 4);
            if (src != c.ip)
                return;  // nobody pretends to be someone else
            const bool broadcast = dst == 0x0A72FFFFu;
            for (auto& o : clients_)
                if (o.game == c.game && (broadcast || o.ip == dst))
                    send(o, 20, b, size);
            return;
        }
        default:
            return;
        }
    }

    void run() {
        while (!stop_) {
            if (drop_.exchange(false)) {
                for (auto& c : clients_)
                    close_fd(c.fd);  // without telling anyone, like a server that died
                clients_.clear();
                games_.clear();
            }
            std::vector<PollFd> fds(1 + clients_.size());
            fds[0].fd = listen_;
            fds[0].events = POLLIN;
            for (size_t i = 0; i < clients_.size(); ++i) {
                fds[i + 1].fd = clients_[i].fd;
                fds[i + 1].events = POLLIN;
            }
            if (poll_fds(fds.data(), fds.size(), 50) <= 0)
                continue;
            if (fds[0].revents & POLLIN) {
                const SocketFd s = ::accept(listen_, nullptr, nullptr);
                if (s != kNoSocket) {
                    Client c;
                    c.fd = s;
                    clients_.push_back(std::move(c));
                }
            }
            for (size_t i = 1; i < fds.size(); ++i) {
                if (fds[i].revents == 0)
                    continue;
                Client* c = by_fd(fds[i].fd);
                uint8_t buf[65536];
                const int n = ::recv(fds[i].fd, reinterpret_cast<char*>(buf), sizeof(buf), 0);
                if (n <= 0) {
                    leave(*c);
                    close_fd(c->fd);
                    c->fd = kNoSocket;
                    continue;
                }
                c->in.insert(c->in.end(), buf, buf + n);
                size_t at = 0;
                while (c->in.size() - at >= 12) {
                    uint32_t len = 0;
                    std::memcpy(&len, c->in.data() + at + 8, 4);
                    if (c->in.size() - at < 12 + len)
                        break;
                    handle(*c, c->in[at + 4], c->in.data() + at + 12, len);
                    at += 12 + len;
                }
                c->in.erase(c->in.begin(), c->in.begin() + static_cast<std::ptrdiff_t>(at));
            }
            for (size_t i = clients_.size(); i-- > 0;)
                if (clients_[i].fd == kNoSocket)
                    clients_.erase(clients_.begin() + static_cast<std::ptrdiff_t>(i));
        }
    }

    SocketFd listen_ = kNoSocket;
    uint16_t port_ = 0;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> drop_{false};
    std::atomic<bool> ignore_scans_{false};
    std::vector<Client> clients_;
    std::map<std::string, Game> games_;
    uint32_t next_ip_ = 0;
    std::atomic<int> pings_answered_{0};
};

// --- Two runtimes ---------------------------------------------------------

struct RyuEnv : Env {
    explicit RyuEnv(const LdnLanConfig& lan)
        : Env([&lan](ServiceManager& sm) {
              SystemConfig config;
              register_base_services(sm, config, small_process());
              register_local_network(sm, lan);
          }) {}

    Reply call(Handle h, uint32_t command, Msg m = {}) {
        m.command = command;
        const std::optional<Reply> r = send(h, m);
        return r ? *r : Reply{};
    }
    Handle open(Handle h, uint32_t command) {
        const Reply r = call(h, command);
        CHECK_EQ(r.move.size(), 1u);
        return r.move.empty() ? kInvalidHandle : r.move[0];
    }
    bool signaled(Handle h) {
        auto ev = k.handles().get_as<KReadableEvent>(h);
        return ev && k.scheduler().is_readable_signaled(*ev);
    }
    std::vector<uint8_t> read(uint64_t addr, size_t size) {
        std::vector<uint8_t> v(size);
        CHECK(k.memory().copy_from_user(v.data(), addr, size));
        return v;
    }
    template <typename T>
    T get(uint64_t addr) {
        T v{};
        CHECK(k.memory().copy_from_user(&v, addr, sizeof(T)));
        return v;
    }
};

template <typename F>
bool eventually(F cond) {
    for (int i = 0; i < 300; ++i) {
        if (cond())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

Msg init_msg() {
    Msg init;
    init.send_pid = true;
    init.arg<uint64_t>(0);
    return init;
}

// CreateNetworkPrivate with the game's session id and server random (LGP
// derives both, so two searching instances host the same session).
Msg create_private(const uint8_t (&session)[16], const char* user) {
    Msg m;
    m.payload.assign(0xB8, 0);
    for (int i = 0; i < 16; ++i)
        m.payload[0x44 + i] = static_cast<uint8_t>(0xA0 + i);  // SecurityParameter.server_random
    std::memcpy(m.payload.data() + 0x44 + 0x10, session, 16);  // SecurityParameter.session_id
    std::memcpy(m.payload.data() + 0x64, user, std::strlen(user));
    std::memcpy(m.payload.data() + 0x98, &kLcid, 8);
    std::memcpy(m.payload.data() + 0x98 + 0x10, &kChannel, 2);
    m.payload[0x98 + 0x12] = 2;
    std::memcpy(m.payload.data() + 0x98 + 0x14, &kVersion, 2);
    return m;
}

Msg connect_private(const uint8_t (&session)[16], const char* user, int32_t version) {
    Msg m;
    m.payload.assign(0x9C, 0);
    for (int i = 0; i < 16; ++i)
        m.payload[0x44 + i] = static_cast<uint8_t>(0xA0 + i);
    std::memcpy(m.payload.data() + 0x44 + 0x10, session, 16);
    std::memcpy(m.payload.data() + 0x64, user, std::strlen(user));
    std::memcpy(m.payload.data() + 0x94, &version, 4);
    return m;
}

int16_t scan(RyuEnv& e, Handle ldn) {
    Msg m;
    m.arg<int16_t>(kChannel);
    m.payload.resize(8 + 0x60, 0);
    std::memcpy(m.payload.data() + 8, &kLcid, 8);
    const uint32_t flags = 1;  // LocalCommunicationId
    std::memcpy(m.payload.data() + 8 + 0x5C, &flags, 4);
    m.b = {{kBufB, kInfoSize * 4, 0}};
    const Reply r = e.call(ldn, 102, m);
    CHECK_EQ(r.result, 0u);
    return r.get<int16_t>(0);
}

int32_t udp_socket(RyuEnv& e, Handle bsd, uint16_t port) {
    Msg reg;
    reg.send_pid = true;
    for (int i = 0; i < 8; ++i)
        reg.arg<uint32_t>(0);
    reg.arg<uint64_t>(0).arg<uint64_t>(0x10000);
    CHECK_EQ(e.call(bsd, 0, reg).result, 0u);
    Msg sock;
    sock.arg<int32_t>(2).arg<int32_t>(2).arg<int32_t>(17);
    const int32_t fd = e.call(bsd, 2, sock).get<int32_t>(0);
    const uint8_t addr[16] = {16, 2, static_cast<uint8_t>(port >> 8), static_cast<uint8_t>(port)};
    CHECK(e.k.memory().copy_to_user(kBufA, addr, sizeof(addr)));
    Msg bind;
    bind.arg<int32_t>(fd);
    bind.a = {{kBufA, sizeof(addr), 0}};
    CHECK_EQ(e.call(bsd, 13, bind).get<int32_t>(0), 0);
    return fd;
}

void send_to(RyuEnv& e, Handle bsd, int32_t fd, uint32_t ip, uint16_t port, const std::string& text) {
    const uint8_t addr[16] = {16,
                              2,
                              static_cast<uint8_t>(port >> 8),
                              static_cast<uint8_t>(port),
                              static_cast<uint8_t>(ip >> 24),
                              static_cast<uint8_t>(ip >> 16),
                              static_cast<uint8_t>(ip >> 8),
                              static_cast<uint8_t>(ip)};
    CHECK(e.k.memory().copy_to_user(kBufA, text.data(), text.size()));
    CHECK(e.k.memory().copy_to_user(kBufA + 0x100, addr, sizeof(addr)));
    Msg m;
    m.arg<int32_t>(fd).arg<int32_t>(0);
    m.a = {{kBufA, text.size(), 0}, {kBufA + 0x100, sizeof(addr), 0}};
    CHECK_EQ(e.call(bsd, 11, m).get<int32_t>(0), static_cast<int32_t>(text.size()));
}

std::string recv_from(RyuEnv& e, Handle bsd, int32_t fd, uint32_t* from_ip = nullptr) {
    Msg m;
    m.arg<int32_t>(fd).arg<int32_t>(0);
    m.b = {{kBufB, 0x100, 0}, {kBufB + 0x100, 16, 0}};
    const Reply r = e.call(bsd, 9, m);
    const int32_t n = r.get<int32_t>(0);
    if (n < 0)
        return {};
    const std::vector<uint8_t> a = e.read(kBufB + 0x100, 16);
    if (from_ip != nullptr)
        *from_ip = uint32_t{a[4]} << 24 | uint32_t{a[5]} << 16 | uint32_t{a[6]} << 8 | a[7];
    const std::vector<uint8_t> d = e.read(kBufB, static_cast<size_t>(n));
    return std::string(d.begin(), d.end());
}

void test_passphrase() {
    CHECK(ryuldn_passphrase("").empty());
    const std::string p = ryuldn_passphrase("pauls-geraete");
    CHECK_EQ(p.size(), 16u);
    CHECK(p.rfind("Ryujinx-", 0) == 0);
    for (size_t i = 8; i < p.size(); ++i)
        CHECK((p[i] >= '0' && p[i] <= '9') || (p[i] >= 'a' && p[i] <= 'f'));  // the server wants lowercase hex
    CHECK(p != ryuldn_passphrase("andere"));
}

void test_room(const std::string& server, bool fake, const std::string& ca_file = "") {
    std::shared_ptr<platform::ILan> lan = platform::make_host_lan();
    if (!lan) {
        std::printf("  no host sockets here, skipped\n");
        return;
    }
    LdnLanConfig cfg;
    cfg.lan = lan;
    CHECK(lgp::hle::parse_ryuldn_server(server, &cfg));
    cfg.ryuldn_ca_file = ca_file;
    // A room text per run: a real server may hold rooms of earlier runs.
    cfg.ryuldn_room = "lgp-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    LdnLanConfig other = cfg;
    other.ryuldn_room = cfg.ryuldn_room + "-other";

    RyuEnv a(cfg), b(cfg), c(other);
    const Handle la = a.open(a.open_service("ldn:u"), 0);
    const Handle lb = b.open(b.open_service("ldn:u"), 0);
    const Handle lc = c.open(c.open_service("ldn:u"), 0);
    const Reply eva = a.call(la, 100), evb = b.call(lb, 100);
    CHECK_EQ(eva.copy.size(), 1u);
    CHECK_EQ(evb.copy.size(), 1u);
    if (eva.copy.empty() || evb.copy.empty())
        return;
    const Handle changed_a = eva.copy[0], changed_b = evb.copy[0];
    uint8_t session[16];
    for (int i = 0; i < 16; ++i)
        session[i] = static_cast<uint8_t>(std::rand());

    // A opens a room with advertise data.
    CHECK_EQ(a.call(la, 400, init_msg()).result, 0u);
    CHECK_EQ(a.call(la, 200).result, 0u);
    const char adv[] = "LGP-LINK-CODE";
    CHECK(a.k.memory().copy_to_user(kBufA, adv, sizeof(adv)));
    Msg advertise;
    advertise.a = {{kBufA, sizeof(adv), 0}};
    CHECK_EQ(a.call(la, 206, advertise).result, 0u);
    CHECK_EQ(a.call(la, 203, create_private(session, "Host")).result, 0u);
    const Reply ipa = a.call(la, 2);
    const uint32_t host_ip = ipa.get<uint32_t>(0);
    CHECK_EQ(host_ip >> 16, 0x0A72u);  // 10.114.x.y from the server
    CHECK_EQ(ipa.get<uint32_t>(4), 0xFFFF0000u);

    // B hosts the same session too (as a searching LGP does): the server
    // refuses the second room, the game still sees its own network. Its scan
    // finds A's room; C, with another room text, finds nothing.
    CHECK_EQ(b.call(lb, 400, init_msg()).result, 0u);
    CHECK_EQ(b.call(lb, 200).result, 0u);
    CHECK_EQ(b.call(lb, 203, create_private(session, "Guest")).result, 0u);
    CHECK_EQ(b.call(lb, 0).get<uint32_t>(0), 3u);
    CHECK_EQ(scan(b, lb), 1);
    const std::vector<uint8_t> scanned = b.read(kBufB, kInfoSize);
    CHECK(std::memcmp(scanned.data() + kInfoSession, session, 16) == 0);
    CHECK_EQ(scanned[kInfoNodeCount], 1u);
    CHECK_EQ(b.get<uint16_t>(kBufB + kInfoAdvSize), uint16_t{sizeof(adv)});
    CHECK(std::memcmp(scanned.data() + kInfoAdv, adv, sizeof(adv)) == 0);
    CHECK(std::string(reinterpret_cast<const char*>(scanned.data() + kInfoNodes + 0xC)) == "Host");
    CHECK_EQ(c.call(lc, 400, init_msg()).result, 0u);
    CHECK_EQ(c.call(lc, 300).result, 0u);
    CHECK_EQ(scan(c, lc), 0);

    // B gives up its network and joins A's with ConnectPrivate.
    CHECK_EQ(b.call(lb, 204).result, 0u);
    CHECK_EQ(b.call(lb, 201).result, 0u);
    CHECK_EQ(b.call(lb, 300).result, 0u);
    CHECK_EQ(b.call(lb, 303, connect_private(session, "Guest", kVersion - 1)).result,
             ldn_result::kNetworkNotFound.raw);  // other version
    CHECK_EQ(b.svc(kSvcClearEvent, {changed_b})[0], kSuccess.raw);
    CHECK_EQ(a.svc(kSvcClearEvent, {changed_a})[0], kSuccess.raw);
    CHECK_EQ(b.call(lb, 303, connect_private(session, "Guest", kVersion)).result, 0u);
    CHECK_EQ(b.call(lb, 0).get<uint32_t>(0), 5u);
    CHECK(b.signaled(changed_b));
    const uint32_t station_ip = b.call(lb, 2).get<uint32_t>(0);
    CHECK_EQ(station_ip >> 16, 0x0A72u);
    CHECK(station_ip != host_ip);
    CHECK(eventually([&] { return a.signaled(changed_a); }));
    Msg get;
    get.c = {{kBufB, kInfoSize}};
    CHECK(eventually([&] { return a.call(la, 1, get).result == 0u && a.get<uint8_t>(kBufB + kInfoNodeCount) == 2u; }));
    const std::vector<uint8_t> info_a = a.read(kBufB, kInfoSize);
    CHECK_EQ(b.call(lb, 1, get).result, 0u);
    const std::vector<uint8_t> info_b = b.read(kBufB, kInfoSize);
    CHECK(info_a == info_b);  // byte for byte (Pia derives its nonce from it)
    for (int i = 0; i < 16; ++i)
        CHECK_EQ(info_a[0x50 + i], static_cast<uint8_t>(0xA0 + i));  // server random of the game, not the server's 0
    // GetSecurityParameter agrees on both sides.
    CHECK(a.call(la, 4).data == b.call(lb, 4).data);

    // UDP through the server, both bound to the game's port.
    const Handle ba = a.open_service("bsd:u"), bb = b.open_service("bsd:u");
    const int32_t fa = udp_socket(a, ba, 12345), fb = udp_socket(b, bb, 12345);
    send_to(b, bb, fb, host_ip, 12345, "hello host");
    std::string got;
    uint32_t from = 0;
    CHECK(eventually([&] { return !(got = recv_from(a, ba, fa, &from)).empty(); }));
    CHECK(got == "hello host");
    CHECK_EQ(from, station_ip);
    for (int i = 0; i < 7; ++i)
        send_to(a, ba, fa, station_ip, 12345, "burst " + std::to_string(i));
    for (int i = 0; i < 7; ++i) {
        CHECK(eventually([&] { return !(got = recv_from(b, bb, fb)).empty(); }));
        CHECK(got == "burst " + std::to_string(i));
    }
    // A broadcast (LAN style, to x.x.x.255) reaches the station and the sender.
    send_to(a, ba, fa, (host_ip & 0xFFFFFF00u) | 0xFF, 12345, "to all");
    CHECK(eventually([&] { return !(got = recv_from(b, bb, fb)).empty(); }));
    CHECK(got == "to all");
    CHECK(eventually([&] { return !(got = recv_from(a, ba, fa)).empty(); }));
    CHECK(got == "to all");

    // A closes the room: B is back to Station, DestroyedByUser.
    CHECK_EQ(b.svc(kSvcClearEvent, {changed_b})[0], kSuccess.raw);
    CHECK_EQ(a.call(la, 204).result, 0u);
    CHECK(eventually([&] { return b.call(lb, 0).get<uint32_t>(0) == 4u; }));
    CHECK(b.signaled(changed_b));
    CHECK_EQ(b.call(lb, 3).get<int16_t>(0), 3);
    CHECK_EQ(b.call(lb, 401).result, 0u);
    CHECK_EQ(a.call(la, 401).result, 0u);
    CHECK_EQ(c.call(lc, 401).result, 0u);
    (void)fake;
}

void test_fake_server() {
    if (!platform::make_host_lan()) {  // no host sockets (Emscripten): neither client nor server can run
        std::printf("  no host sockets here, skipped\n");
        return;
    }
    FakeServer server;
    if (server.port() == 0) {
        std::printf("  no TCP listener here, skipped\n");
        return;
    }
    test_room("127.0.0.1:" + std::to_string(server.port()), true);
    CHECK(server.pings_answered() >= 3);  // every client answered the server's ping
}

// CreateNetwork + Connect (no SecurityParameter from the game): the server
// does not keep the server random and scans return zero, so both sides must
// end up with the same (zero) value.
void test_public_network() {
    std::shared_ptr<platform::ILan> lan = platform::make_host_lan();
    if (!lan) {
        std::printf("  no host sockets here, skipped\n");
        return;
    }
    FakeServer server;
    if (server.port() == 0)
        return;
    LdnLanConfig cfg;
    cfg.lan = lan;
    CHECK(lgp::hle::parse_ryuldn_server("127.0.0.1:" + std::to_string(server.port()), &cfg));
    RyuEnv a(cfg), b(cfg);
    const Handle la = a.open(a.open_service("ldn:u"), 0);
    const Handle lb = b.open(b.open_service("ldn:u"), 0);
    CHECK_EQ(a.call(la, 400, init_msg()).result, 0u);
    CHECK_EQ(a.call(la, 200).result, 0u);
    Msg create;  // SecurityConfig, UserConfig, pad, NetworkConfig
    create.payload.assign(0x98, 0);
    std::memcpy(create.payload.data() + 0x44, "Host", 4);
    std::memcpy(create.payload.data() + 0x78, &kLcid, 8);
    std::memcpy(create.payload.data() + 0x78 + 0x10, &kChannel, 2);
    create.payload[0x78 + 0x12] = 2;
    std::memcpy(create.payload.data() + 0x78 + 0x14, &kVersion, 2);
    CHECK_EQ(a.call(la, 202, create).result, 0u);
    CHECK_EQ(b.call(lb, 400, init_msg()).result, 0u);
    CHECK_EQ(b.call(lb, 300).result, 0u);
    CHECK_EQ(scan(b, lb), 1);
    Msg connect;  // SecurityConfig, UserConfig, version, option, X NetworkInfo (from the scan)
    connect.payload.assign(0x7C, 0);
    std::memcpy(connect.payload.data() + 0x44, "Guest", 5);
    const int32_t version = kVersion;
    std::memcpy(connect.payload.data() + 0x74, &version, 4);
    connect.x = {{kBufB, kInfoSize, 0}};
    CHECK_EQ(b.call(lb, 302, connect).result, 0u);
    CHECK_EQ(b.call(lb, 0).get<uint32_t>(0), 5u);
    CHECK(a.call(la, 4).data == b.call(lb, 4).data);  // GetSecurityParameter
    Msg get;
    get.c = {{kBufA, kInfoSize}};
    CHECK(eventually([&] { return a.call(la, 1, get).result == 0u && a.get<uint8_t>(kBufA + kInfoNodeCount) == 2u; }));
    const std::vector<uint8_t> info_a = a.read(kBufA, kInfoSize);
    CHECK_EQ(b.call(lb, 1, get).result, 0u);
    CHECK(info_a == b.read(kBufA, kInfoSize));
    CHECK_EQ(b.call(lb, 401).result, 0u);
    CHECK_EQ(a.call(la, 401).result, 0u);
}

// The server goes away while A hosts and B is in A's room: B is back to
// Station (SignalLost); A keeps its network as the game knows it, alone, and
// opens a new room on the server when the game creates the next network.
void test_server_lost() {
    std::shared_ptr<platform::ILan> lan = platform::make_host_lan();
    if (!lan) {
        std::printf("  no host sockets here, skipped\n");
        return;
    }
    FakeServer server;
    if (server.port() == 0)
        return;
    LdnLanConfig cfg;
    cfg.lan = lan;
    CHECK(lgp::hle::parse_ryuldn_server("127.0.0.1:" + std::to_string(server.port()), &cfg));
    RyuEnv a(cfg), b(cfg);
    const Handle la = a.open(a.open_service("ldn:u"), 0);
    const Handle lb = b.open(b.open_service("ldn:u"), 0);
    uint8_t session[16];
    for (int i = 0; i < 16; ++i)
        session[i] = static_cast<uint8_t>(std::rand());
    CHECK_EQ(a.call(la, 400, init_msg()).result, 0u);
    CHECK_EQ(a.call(la, 200).result, 0u);
    CHECK_EQ(a.call(la, 203, create_private(session, "Host")).result, 0u);
    const uint32_t host_ip = a.call(la, 2).get<uint32_t>(0);
    CHECK_EQ(host_ip >> 16, 0x0A72u);
    CHECK_EQ(b.call(lb, 400, init_msg()).result, 0u);
    CHECK_EQ(b.call(lb, 300).result, 0u);
    CHECK_EQ(scan(b, lb), 1);
    CHECK_EQ(b.call(lb, 303, connect_private(session, "Guest", kVersion)).result, 0u);
    CHECK_EQ(b.call(lb, 0).get<uint32_t>(0), 5u);

    server.drop_all();
    CHECK(eventually([&] { return b.call(lb, 0).get<uint32_t>(0) == 4u; }));
    CHECK_EQ(b.call(lb, 3).get<int16_t>(0), 6);  // SignalLost
    CHECK_EQ(a.call(la, 0).get<uint32_t>(0), 3u);  // still AccessPointCreated
    CHECK_EQ(a.call(la, 2).get<uint32_t>(0), host_ip);

    // A scan the server leaves unanswered retires the connection (a late
    // answer must not count for the next request); the next one reconnects.
    server.ignore_scans(true);
    CHECK_EQ(b.call(lb, 401).result, 0u);
    CHECK_EQ(b.call(lb, 400, init_msg()).result, 0u);
    CHECK_EQ(b.call(lb, 300).result, 0u);
    CHECK_EQ(scan(b, lb), 0);
    server.ignore_scans(false);

    // The next network goes to the server again.
    CHECK_EQ(a.call(la, 204).result, 0u);
    CHECK_EQ(a.call(la, 203, create_private(session, "Host")).result, 0u);
    CHECK(eventually([&] { return scan(b, lb) == 1; }));
    CHECK_EQ(b.call(lb, 401).result, 0u);
    CHECK_EQ(a.call(la, 401).result, 0u);
}

void test_unreachable_server() {
    std::shared_ptr<platform::ILan> lan = platform::make_host_lan();
    if (!lan)
        return;
    LdnLanConfig cfg;
    cfg.lan = lan;
    cfg.ryuldn_server = "127.0.0.1";
    cfg.ryuldn_port = 1;  // nobody listens
    RyuEnv a(cfg);
    const Handle la = a.open(a.open_service("ldn:u"), 0);
    CHECK_EQ(a.call(la, 400, init_msg()).result, 0u);
    CHECK_EQ(a.call(la, 300).result, 0u);
    const auto t0 = std::chrono::steady_clock::now();
    CHECK_EQ(scan(a, la), 0);  // alone, as offline, and without waiting long
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(2));
    CHECK_EQ(a.call(la, 401).result, 0u);
}

void test_real_server() {
    const char* env = std::getenv("LGP_RYULDN_SERVER");
    if (env == nullptr || *env == 0) {
        std::printf("  LGP_RYULDN_SERVER not set, nothing to do\n");
        return;
    }
    test_room(env, false);
}

// Through the WebSocket bridge (tools/ldn/ws_bridge.py): run_ldn_ws_bridge.py
// starts the bridge in front of LGP_RYULDN_FAKE_PORT and sets LGP_RYULDN_WS.
// With --tls it serves wss:// with a fresh self-signed certificate for
// localhost and sets LGP_RYULDN_CA to it.
void test_websocket_bridge() {
    const char* ws = std::getenv("LGP_RYULDN_WS");
    const char* port = std::getenv("LGP_RYULDN_FAKE_PORT");
    if (ws == nullptr || *ws == 0 || port == nullptr) {
        std::printf("  LGP_RYULDN_WS not set (ctest runtime.hle_ldn_ryu_ws), nothing to do\n");
        return;
    }
    if (!platform::make_host_lan()) {
        std::printf("  no host sockets here, skipped\n");
        return;
    }
    FakeServer server(static_cast<uint16_t>(std::atoi(port)));
    CHECK(server.port() != 0);
    if (server.port() == 0)
        return;
    const char* ca = std::getenv("LGP_RYULDN_CA");
    if (ca != nullptr && *ca != 0) {
        // Without the certificate the system roots do not know it: the
        // handshake must fail, before any WebSocket byte goes out.
        LdnLanConfig c;
        CHECK(lgp::hle::parse_ryuldn_server(ws, &c));
        CHECK(c.ryuldn_tls);
        std::shared_ptr<platform::ILan> lan = platform::make_host_lan();
        std::string error;
        CHECK(lan->connect_tls(c.ryuldn_server, c.ryuldn_port, 3000, "", &error) == nullptr);
        CHECK(!error.empty());
        std::printf("  without the CA: %s\n", error.c_str());
        // A wrong name fails too, even with the certificate.
        error.clear();
        CHECK(lan->connect_tls("127.0.0.1", c.ryuldn_port, 3000, ca, &error) == nullptr);
        CHECK(!error.empty());
        std::printf("  wrong name: %s\n", error.c_str());
        std::string ok_error;
        CHECK(lan->connect_tls(c.ryuldn_server, c.ryuldn_port, 3000, ca, &ok_error) != nullptr);
        CHECK(ok_error.empty());
    }
    test_room(ws, true, ca != nullptr ? ca : "");
    CHECK(server.pings_answered() >= 3);
}

void test_parse_server() {
    LdnLanConfig c;
    CHECK(lgp::hle::parse_ryuldn_server("example.org", &c));
    CHECK(c.ryuldn_server == "example.org" && c.ryuldn_port == 30456 && !c.ryuldn_websocket);
    CHECK(lgp::hle::parse_ryuldn_server("192.0.2.10:4000", &c));
    CHECK(c.ryuldn_server == "192.0.2.10" && c.ryuldn_port == 4000 && !c.ryuldn_websocket);
    CHECK(lgp::hle::parse_ryuldn_server("ws://lgp.example.org", &c));
    CHECK(c.ryuldn_server == "lgp.example.org" && c.ryuldn_port == 80 && c.ryuldn_websocket && c.ryuldn_path == "/");
    CHECK(lgp::hle::parse_ryuldn_server("ws://h:8080/ldn", &c));
    CHECK(c.ryuldn_server == "h" && c.ryuldn_port == 8080 && c.ryuldn_path == "/ldn");
    CHECK(lgp::hle::parse_ryuldn_server("wss://h", &c));
    CHECK(c.ryuldn_server == "h" && c.ryuldn_port == 443 && c.ryuldn_websocket && c.ryuldn_tls);
    CHECK(lgp::hle::parse_ryuldn_server("ws://h", &c));
    CHECK(!c.ryuldn_tls);
    CHECK(!lgp::hle::parse_ryuldn_server("https://h", &c));
    CHECK(!lgp::hle::parse_ryuldn_server("h:0", &c));
    CHECK(!lgp::hle::parse_ryuldn_server("h:70000", &c));
    CHECK(!lgp::hle::parse_ryuldn_server(":30456", &c));
    CHECK(!lgp::hle::parse_ryuldn_server("h:x", &c));
}

// --ldn both: one run has the LAN and the server; set_ldn_remote() picks the
// way per session. The server hands out a /16, the LAN a /24, so the
// station's subnet mask tells which way it went. Switching while ldn is
// initialized (outside a network) takes effect at the next create or scan.
void test_dual() {
    std::shared_ptr<platform::ILan> lan = platform::make_host_lan();
    if (!lan) {
        std::printf("  no host sockets here, skipped\n");
        return;
    }
    FakeServer server;
    if (server.port() == 0)
        return;
    LdnLanConfig host_cfg;
    host_cfg.lan = lan;
    CHECK(lgp::hle::parse_ryuldn_server("127.0.0.1:" + std::to_string(server.port()), &host_cfg));
    host_cfg.ryuldn_dual = true;
    host_cfg.ryuldn_room = "dual-" + std::to_string(std::rand());
    {
        const std::unique_ptr<platform::ILanSocket> s = lan->open_udp(0);
        CHECK(s != nullptr);
        host_cfg.port = s ? s->port() : 0;
    }
    host_cfg.broadcast = false;  // the LAN part stays on 127.0.0.1
    host_cfg.subnet_probe = false;
    LdnLanConfig station_cfg = host_cfg;
    station_cfg.port = 0;
    station_cfg.peers = {{0x7F000001, host_cfg.port}};
    RyuEnv a(host_cfg), b(station_cfg);
    const Handle la = a.open(a.open_service("ldn:u"), 0);
    const Handle lb = b.open(b.open_service("ldn:u"), 0);
    const uint8_t session[16] = {0x51, 0x52, 0x53};
    auto round = [&](bool remote, bool switch_after_init) {
        lgp::hle::set_ldn_remote(switch_after_init ? !remote : remote);
        CHECK_EQ(a.call(la, 400, init_msg()).result, 0u);
        CHECK_EQ(b.call(lb, 400, init_msg()).result, 0u);
        CHECK_EQ(a.call(la, 200).result, 0u);
        CHECK_EQ(b.call(lb, 300).result, 0u);
        if (switch_after_init)
            lgp::hle::set_ldn_remote(remote);
        CHECK_EQ(a.call(la, 203, create_private(session, "Host")).result, 0u);
        CHECK(eventually([&] { return scan(b, lb) == 1; }));
        CHECK_EQ(b.call(lb, 303, connect_private(session, "Guest", kVersion)).result, 0u);
        // The mask tells the way: the server's 10.114.0.0/16, the LAN a /24
        // (which may also lie in 10.114, 10.A.B.0 with A in 96..127).
        const uint32_t mask = b.call(lb, 2).get<uint32_t>(4);
        CHECK_EQ(mask, remote ? 0xFFFF0000u : 0xFFFFFF00u);
        CHECK_EQ(b.call(lb, 401).result, 0u);
        CHECK_EQ(a.call(la, 401).result, 0u);
    };
    round(false, false);  // nearby: LAN
    round(true, false);   // far away: server
    round(false, true);   // initialized on the server, switched back to the LAN before create/scan
    round(true, true);    // and the other way
    lgp::hle::set_ldn_remote(false);
}

}  // namespace

int main() {
    std::srand(static_cast<unsigned>(std::chrono::steady_clock::now().time_since_epoch().count()));
    RUN(test_passphrase);
    RUN(test_parse_server);
    RUN(test_fake_server);
    RUN(test_server_lost);
    RUN(test_public_network);
    RUN(test_dual);
    RUN(test_unreachable_server);
    RUN(test_real_server);
    RUN(test_websocket_bridge);
    return finish();
}
