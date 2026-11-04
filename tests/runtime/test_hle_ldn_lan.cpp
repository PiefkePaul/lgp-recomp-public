// ldn over LAN (docs/analysis/44-multiplayer-stand.md): two runtimes in one
// process, each with its own kernel and services, find each other over
// 127.0.0.1, one creates an ldn network, the other scans and joins it, and
// UDP sockets of bsd:u exchange datagrams inside the ldn network.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include "ipc_client.hpp"
#include "lgp/hle/services.hpp"
#include "lgp/platform/lan.hpp"

using namespace lgp_test;
namespace platform = lgp::platform;

namespace {

constexpr size_t kInfoSize = 0x480;
constexpr int64_t kLcid = 0x0100'0000'0000'1234;
constexpr uint32_t kLoopback = 0x7F000001;

struct LanEnv : Env {
    explicit LanEnv(const LdnLanConfig& lan)
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

// Polls cond (which talks to the services) for up to two seconds.
template <typename F>
bool eventually(F cond) {
    for (int i = 0; i < 200; ++i) {
        if (cond())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

constexpr int16_t kVersion = 88;  // LGP's local communication version (reportedly)
constexpr int16_t kChannel = 6;

std::vector<uint8_t> create_payload(int8_t node_count_max, const char* user, int16_t version,
                                    int16_t channel = kChannel) {
    std::vector<uint8_t> p(0x98, 0);
    std::memcpy(p.data() + 0x44, user, std::strlen(user));  // UserConfig
    std::memcpy(p.data() + 0x78, &kLcid, 8);                 // NetworkConfig.intent_id
    const uint16_t scene = 7;
    std::memcpy(p.data() + 0x78 + 0xA, &scene, 2);
    std::memcpy(p.data() + 0x78 + 0x10, &channel, 2);  // NetworkConfig.channel
    p[0x78 + 0x12] = static_cast<uint8_t>(node_count_max);
    std::memcpy(p.data() + 0x78 + 0x14, &version, 2);
    return p;
}

Msg init_msg() {
    Msg init;
    init.send_pid = true;
    init.arg<uint64_t>(0);
    return init;
}

// Scan for lcid (kLcid by default); returns the count, networks in kBufB.
int16_t scan(LanEnv& e, Handle ldn, int16_t channel = kChannel, int64_t lcid = kLcid) {
    Msg m;
    m.arg<int16_t>(channel);
    m.payload.resize(8 + 0x60, 0);
    std::memcpy(m.payload.data() + 8, &lcid, 8);  // ScanFilter.network_id.intent_id
    const uint32_t flags = 1;                      // LocalCommunicationId
    std::memcpy(m.payload.data() + 8 + 0x5C, &flags, 4);
    m.b = {{kBufB, kInfoSize * 4, 0}};
    const Reply r = e.call(ldn, 102, m);
    CHECK_EQ(r.result, 0u);
    return r.get<int16_t>(0);
}

Reply connect(LanEnv& e, Handle ldn, const std::vector<uint8_t>& info, const char* user, int32_t version) {
    CHECK(e.k.memory().copy_to_user(kBufA, info.data(), info.size()));
    Msg m;
    m.payload.resize(0x7C, 0);
    std::memcpy(m.payload.data() + 0x44, user, std::strlen(user));
    std::memcpy(m.payload.data() + 0x74, &version, 4);
    m.x = {{kBufA, kInfoSize, 0}};
    return e.call(ldn, 302, m);
}

int32_t udp_socket(LanEnv& e, Handle bsd, uint16_t port) {
    Msg reg;
    reg.send_pid = true;
    for (int i = 0; i < 8; ++i)
        reg.arg<uint32_t>(0);
    reg.arg<uint64_t>(0).arg<uint64_t>(0x10000);
    CHECK_EQ(e.call(bsd, 0, reg).result, 0u);
    Msg sock;
    sock.arg<int32_t>(2).arg<int32_t>(2).arg<int32_t>(17);
    const int32_t fd = e.call(bsd, 2, sock).get<int32_t>(0);
    const uint8_t addr[16] = {16, 2, static_cast<uint8_t>(port >> 8), static_cast<uint8_t>(port)};  // 0.0.0.0
    CHECK(e.k.memory().copy_to_user(kBufA, addr, sizeof(addr)));
    Msg bind;
    bind.arg<int32_t>(fd);
    bind.a = {{kBufA, sizeof(addr), 0}};
    CHECK_EQ(e.call(bsd, 13, bind).get<int32_t>(0), 0);
    return fd;
}

void send_to(LanEnv& e, Handle bsd, int32_t fd, uint32_t ip, uint16_t port, const std::string& text) {
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

// RecvFrom; empty when nothing is queued (EAGAIN). Sender in *from.
std::string recv_from(LanEnv& e, Handle bsd, int32_t fd, uint32_t* from_ip = nullptr, uint16_t* from_port = nullptr) {
    Msg m;
    m.arg<int32_t>(fd).arg<int32_t>(0);
    m.b = {{kBufB, 0x100, 0}, {kBufB + 0x100, 16, 0}};
    const Reply r = e.call(bsd, 9, m);
    const int32_t n = r.get<int32_t>(0);
    if (n < 0) {
        CHECK_EQ(r.get<int32_t>(4), 11);  // EAGAIN
        return {};
    }
    CHECK_EQ(r.get<uint32_t>(8), 16u);
    const std::vector<uint8_t> a = e.read(kBufB + 0x100, 16);
    if (from_ip != nullptr)
        *from_ip = uint32_t{a[4]} << 24 | uint32_t{a[5]} << 16 | uint32_t{a[6]} << 8 | a[7];
    if (from_port != nullptr)
        *from_port = static_cast<uint16_t>(a[2] << 8 | a[3]);
    const std::vector<uint8_t> d = e.read(kBufB, static_cast<size_t>(n));
    return std::string(d.begin(), d.end());
}

bool poll_readable(LanEnv& e, Handle bsd, int32_t fd) {
    const int32_t pfd[2] = {fd, 0x1};  // POLLIN
    CHECK(e.k.memory().copy_to_user(kBufA, pfd, sizeof(pfd)));
    Msg m;
    m.arg<uint32_t>(1).arg<int32_t>(0);
    m.a = {{kBufA, sizeof(pfd), 0}};
    m.b = {{kBufB, sizeof(pfd), 0}};
    return e.call(bsd, 6, m).get<int32_t>(0) == 1 && (e.get<int16_t>(kBufB + 6) & 0x1) != 0;
}

// The session id in its three places agrees, MACs are per node and the same
// in bssid and node 0, and each node carries the version it was given.
void check_identity(const std::vector<uint8_t>& info, uint32_t host_ip, uint32_t station_ip) {
    static const char kHex[] = "0123456789abcdef";
    std::string hex;
    for (size_t i = 0; i < 0x10; ++i) {
        hex += kHex[info[0x10 + i] >> 4];
        hex += kHex[info[0x10 + i] & 0xF];
    }
    CHECK_EQ(info[0x26], 0x20u);  // ssid length
    CHECK(std::string(reinterpret_cast<const char*>(info.data() + 0x27), 0x20) == hex);
    CHECK_EQ(info[0x4A], 3u);  // link level
    CHECK_EQ(info[0x4B], 2u);  // network type: ldn
    CHECK(info[0x60] >= 1 && info[0x60] <= 3);  // security mode as a console reports it
    for (size_t node = 2; node < 8; ++node)
        CHECK_EQ(info[0x68 + 0x40 * node + 0x0A], node);  // free slots carry their index
    const auto ip_at = [&](size_t node) {
        uint32_t v = 0;
        std::memcpy(&v, info.data() + 0x68 + 0x40 * node, 4);
        return v;
    };
    CHECK_EQ(ip_at(0), host_ip);
    CHECK_EQ(ip_at(1), station_ip);
    for (size_t node = 0; node < 2; ++node) {
        const uint8_t* mac = info.data() + 0x68 + 0x40 * node + 4;
        const uint32_t ip = ip_at(node);
        const uint8_t want[6] = {0x02, 0x00, static_cast<uint8_t>(ip >> 24), static_cast<uint8_t>(ip >> 16),
                                 static_cast<uint8_t>(ip >> 8), static_cast<uint8_t>(ip)};
        CHECK(std::memcmp(mac, want, 6) == 0);
        int16_t version = 0;
        std::memcpy(&version, info.data() + 0x68 + 0x40 * node + 0x2E, 2);
        CHECK_EQ(version, kVersion);
        CHECK_EQ(static_cast<int8_t>(info[0x68 + 0x40 * node + 0x0A]), static_cast<int8_t>(node));  // node id
    }
    CHECK(std::memcmp(info.data() + 0x20, info.data() + 0x68 + 4, 6) == 0);  // bssid = node 0's MAC
}

uint16_t free_port(platform::ILan& lan) {
    const std::unique_ptr<platform::ILanSocket> s = lan.open_udp(0);
    CHECK(s != nullptr);
    return s ? s->port() : 0;
}

void test_endpoint_parse() {
    platform::LanEndpoint e;
    CHECK(platform::parse_lan_endpoint("192.168.1.20", &e));
    CHECK_EQ(e.ip, 0xC0A80114u);
    CHECK_EQ(e.port, 0u);
    CHECK(platform::parse_lan_endpoint("127.0.0.1:39871", &e));
    CHECK_EQ(e.ip, kLoopback);
    CHECK_EQ(e.port, 39871u);
    CHECK(platform::lan_endpoint_string(e) == "127.0.0.1:39871");
    CHECK(!platform::parse_lan_endpoint("1.2.3", &e));
    CHECK(!platform::parse_lan_endpoint("1.2.3.256", &e));
    CHECK(!platform::parse_lan_endpoint("1.2.3.4:70000", &e));
    CHECK(!platform::parse_lan_endpoint("1.2.3.4:", &e));
}

void test_two_runtimes() {
    std::shared_ptr<platform::ILan> lan = platform::make_host_lan();
    if (!lan) {
        std::printf("  no host sockets here, skipped\n");
        return;
    }
    LdnLanConfig host_cfg;
    host_cfg.lan = lan;
    host_cfg.port = free_port(*lan);
    host_cfg.broadcast = false;  // the test stays on 127.0.0.1
    host_cfg.subnet_probe = false;
    LdnLanConfig station_cfg = host_cfg;
    station_cfg.port = 0;
    station_cfg.peers = {{kLoopback, host_cfg.port}};

    LanEnv a(host_cfg), b(station_cfg);
    const Handle la = a.open(a.open_service("ldn:u"), 0);
    const Handle lb = b.open(b.open_service("ldn:u"), 0);
    const Reply eva = a.call(la, 100), evb = b.call(lb, 100);
    CHECK_EQ(eva.copy.size(), 1u);
    CHECK_EQ(evb.copy.size(), 1u);
    if (eva.copy.empty() || evb.copy.empty())
        return;
    const Handle changed_a = eva.copy[0], changed_b = evb.copy[0];

    // A: access point with advertise data, up to 2 nodes.
    CHECK_EQ(a.call(la, 400, init_msg()).result, 0u);
    CHECK_EQ(a.call(la, 200).result, 0u);
    const char adv[] = "LGP-LINK-CODE";
    CHECK(a.k.memory().copy_to_user(kBufA, adv, sizeof(adv)));
    Msg advertise;
    advertise.a = {{kBufA, sizeof(adv), 0}};
    CHECK_EQ(a.call(la, 206, advertise).result, 0u);
    Msg create;
    create.payload = create_payload(2, "Host", kVersion);
    CHECK_EQ(a.call(la, 202, create).result, 0u);
    const Reply ipa = a.call(la, 2);
    const uint32_t host_ip = ipa.get<uint32_t>(0);
    CHECK_EQ(host_ip >> 24, 10u);  // 10.A.B.1 with A in 96..127, not link-local
    CHECK((host_ip >> 16 & 0xFF) >= 96 && (host_ip >> 16 & 0xFF) <= 127);
    CHECK_EQ(host_ip & 0xFF, 1u);
    CHECK_EQ(ipa.get<uint32_t>(4), 0xFFFFFF00u);

    // B hosts too (as a searching LGP does) and scans while hosting: it finds
    // A's network, not its own, on its channel only. The session ids differ
    // although both are each instance's first network.
    CHECK_EQ(b.call(lb, 400, init_msg()).result, 0u);
    CHECK_EQ(b.call(lb, 200).result, 0u);
    Msg create_b;
    create_b.payload = create_payload(2, "Guest", kVersion);
    CHECK_EQ(b.call(lb, 202, create_b).result, 0u);
    CHECK_EQ(b.call(lb, 0).get<uint32_t>(0), 3u);
    Msg get;
    get.c = {{kBufB, kInfoSize}};
    CHECK_EQ(b.call(lb, 1, get).result, 0u);
    const std::vector<uint8_t> own_b = b.read(kBufB, kInfoSize);
    int16_t found = 0;
    for (int i = 0; i < 5 && found == 0; ++i)
        found = scan(b, lb);
    CHECK_EQ(found, 1);
    const std::vector<uint8_t> scanned = b.read(kBufB, kInfoSize);
    CHECK(std::memcmp(scanned.data() + 0x10, own_b.data() + 0x10, 0x10) != 0);  // other session id
    CHECK_EQ(scan(b, lb, 11), 0);  // A is on channel 6
    // B gives up its own network and finalizes before it connects (the
    // network found by the scan is still known).
    CHECK_EQ(b.call(lb, 204).result, 0u);
    CHECK_EQ(b.call(lb, 201).result, 0u);
    CHECK_EQ(b.call(lb, 401).result, 0u);
    CHECK_EQ(b.call(lb, 400, init_msg()).result, 0u);
    CHECK_EQ(b.call(lb, 300).result, 0u);
    CHECK_EQ(b.get<int64_t>(kBufB), kLcid);
    CHECK_EQ(b.get<uint8_t>(kBufB + 0x67), 1u);  // node_count
    CHECK_EQ(b.get<uint16_t>(kBufB + 0x26A), uint16_t{sizeof(adv)});
    CHECK(std::memcmp(scanned.data() + 0x26C, adv, sizeof(adv)) == 0);
    CHECK(std::string(reinterpret_cast<const char*>(scanned.data() + 0x68 + 0xC)) == "Host");
    CHECK_EQ(static_cast<int16_t>(scanned[0x48] | scanned[0x49] << 8), kChannel);  // as the host set it

    // Another local communication version is turned away.
    CHECK_EQ(connect(b, lb, scanned, "Guest", kVersion - 1).result, ldn_result::kNetworkNotFound.raw);
    CHECK_EQ(b.call(lb, 0).get<uint32_t>(0), 4u);  // still Station

    // B joins; both see the same NetworkInfo with two nodes.
    CHECK_EQ(b.svc(kSvcClearEvent, {changed_b})[0], kSuccess.raw);
    CHECK_EQ(a.svc(kSvcClearEvent, {changed_a})[0], kSuccess.raw);
    CHECK_EQ(connect(b, lb, scanned, "Guest", kVersion).result, 0u);
    CHECK_EQ(b.call(lb, 0).get<uint32_t>(0), 5u);  // StationConnected
    CHECK(b.signaled(changed_b));
    const uint32_t station_ip = b.call(lb, 2).get<uint32_t>(0);
    CHECK_EQ(station_ip, (host_ip & 0xFFFFFF00u) | 2u);
    CHECK(eventually([&] { return a.signaled(changed_a); }));
    CHECK_EQ(a.call(la, 1, get).result, 0u);
    const std::vector<uint8_t> info_a = a.read(kBufB, kInfoSize);
    CHECK_EQ(b.call(lb, 1, get).result, 0u);
    const std::vector<uint8_t> info_b = b.read(kBufB, kInfoSize);
    CHECK_EQ(info_a[0x67], 2u);
    CHECK(info_a == info_b);  // byte for byte (Pia derives its nonce from it)
    CHECK(std::string(reinterpret_cast<const char*>(info_a.data() + 0x68 + 0x40 + 0xC)) == "Guest");
    check_identity(info_a, host_ip, station_ip);

    // UDP inside the ldn network, both bound to the game's port.
    const Handle ba = a.open_service("bsd:u"), bb = b.open_service("bsd:u");
    const int32_t fa = udp_socket(a, ba, 12345), fb = udp_socket(b, bb, 12345);
    CHECK(recv_from(a, ba, fa).empty());
    send_to(b, bb, fb, host_ip, 12345, "hello host");
    CHECK(eventually([&] { return poll_readable(a, ba, fa); }));
    uint32_t from_ip = 0;
    uint16_t from_port = 0;
    CHECK(recv_from(a, ba, fa, &from_ip, &from_port) == "hello host");
    CHECK_EQ(from_ip, station_ip);
    CHECK_EQ(from_port, 12345u);
    // A burst in order (LGP sends several datagrams within a millisecond).
    for (int i = 0; i < 7; ++i)
        send_to(a, ba, fa, station_ip, 12345, "burst " + std::to_string(i));
    for (int i = 0; i < 7; ++i) {
        std::string got;
        CHECK(eventually([&] { return !(got = recv_from(b, bb, fb)).empty(); }));
        CHECK(got == "burst " + std::to_string(i));
    }
    // A subnet broadcast reaches the station and, as in RyuLDN, the sender.
    send_to(a, ba, fa, (host_ip & 0xFFFFFF00u) | 0xFF, 12345, "to all");
    std::string got;
    CHECK(eventually([&] { return !(got = recv_from(b, bb, fb)).empty(); }));
    CHECK(got == "to all");
    CHECK(recv_from(a, ba, fa) == "to all");
    // Outside the ldn network nothing leaves (as offline).
    send_to(b, bb, fb, 0x08080808, 53, "dns?");

    // A destroys the network: B falls back to Station, DestroyedByUser.
    CHECK_EQ(b.svc(kSvcClearEvent, {changed_b})[0], kSuccess.raw);
    CHECK_EQ(a.call(la, 204).result, 0u);
    CHECK(eventually([&] { return b.call(lb, 0).get<uint32_t>(0) == 4u; }));
    CHECK(b.signaled(changed_b));
    CHECK_EQ(b.call(lb, 3).get<int16_t>(0), 3);
    CHECK_EQ(b.call(lb, 1, get).result, ldn_result::kInvalidState.raw);
    CHECK_EQ(b.call(lb, 401).result, 0u);
    CHECK_EQ(a.call(la, 401).result, 0u);
}

// CreateNetworkPrivate payload: the game's own session id (LGP derives it,
// so two searching instances host with the same one).
std::vector<uint8_t> create_private_payload(const uint8_t (&session)[16], const char* user, int64_t lcid) {
    std::vector<uint8_t> p(0xB8, 0);
    std::memcpy(p.data() + 0x44 + 0x10, session, 16);    // SecurityParameter.session_id
    std::memcpy(p.data() + 0x64, user, std::strlen(user));  // UserConfig
    std::memcpy(p.data() + 0x98, &lcid, 8);                 // NetworkConfig.intent_id
    std::memcpy(p.data() + 0x98 + 0x10, &kChannel, 2);
    p[0x98 + 0x12] = 2;
    std::memcpy(p.data() + 0x98 + 0x14, &kVersion, 2);
    return p;
}

// Both instances host private networks with the SAME session id and scan for
// "own" LocalCommunicationId -1 while hosting (as LGP did in the PC run of
// 2026-10-21): each must find the other's network, never its own, and join it
// with ConnectPrivate.
void test_same_session() {
    std::shared_ptr<platform::ILan> lan = platform::make_host_lan();
    if (!lan) {
        std::printf("  no host sockets here, skipped\n");
        return;
    }
    LdnLanConfig a_cfg;
    a_cfg.lan = lan;
    a_cfg.port = free_port(*lan);
    a_cfg.broadcast = false;
    a_cfg.subnet_probe = false;
    LdnLanConfig b_cfg = a_cfg;
    b_cfg.port = free_port(*lan);
    a_cfg.peers = {{kLoopback, b_cfg.port}, {kLoopback, a_cfg.port}};  // also probes itself
    b_cfg.peers = {{kLoopback, a_cfg.port}, {kLoopback, b_cfg.port}};

    LanEnv a(a_cfg), b(b_cfg);
    const Handle la = a.open(a.open_service("ldn:u"), 0);
    const Handle lb = b.open(b.open_service("ldn:u"), 0);
    const uint8_t session[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    for (auto [e, l, name] : {std::tuple{&a, la, "A"}, std::tuple{&b, lb, "B"}}) {
        CHECK_EQ(e->call(l, 400, init_msg()).result, 0u);
        CHECK_EQ(e->call(l, 200).result, 0u);
        Msg create;
        create.payload = create_private_payload(session, name, -1);
        CHECK_EQ(e->call(l, 203, create).result, 0u);
    }
    int16_t found = 0;
    for (int i = 0; i < 5 && found == 0; ++i)
        found = scan(b, lb, kChannel, -1);
    CHECK_EQ(found, 1);  // A's network only, although it has B's session id
    CHECK(std::string(reinterpret_cast<const char*>(b.read(kBufB, kInfoSize).data() + 0x68 + 0xC)) == "A");
    // Each access point has its own address and MAC (not derived from the
    // shared session id), so the two networks do not look alike.
    CHECK(a.call(la, 2).get<uint32_t>(0) != b.call(lb, 2).get<uint32_t>(0));
    CHECK_EQ(b.get<uint32_t>(kBufB + 0x68), a.call(la, 2).get<uint32_t>(0));  // node 0 of the scanned network is A
    found = 0;
    for (int i = 0; i < 5 && found == 0; ++i)
        found = scan(a, la, kChannel, -1);
    CHECK_EQ(found, 1);
    CHECK(std::string(reinterpret_cast<const char*>(a.read(kBufB, kInfoSize).data() + 0x68 + 0xC)) == "B");

    // B gives up its network and joins A's with the shared session id.
    CHECK_EQ(b.call(lb, 204).result, 0u);
    CHECK_EQ(b.call(lb, 201).result, 0u);
    CHECK_EQ(b.call(lb, 300).result, 0u);
    Msg join;
    join.payload.resize(0x98, 0);
    std::memcpy(join.payload.data() + 0x44 + 0x10, session, 16);
    std::memcpy(join.payload.data() + 0x64, "B", 1);
    const int32_t version = kVersion;
    std::memcpy(join.payload.data() + 0x94, &version, 4);
    CHECK_EQ(b.call(lb, 303, join).result, 0u);
    CHECK_EQ(b.call(lb, 0).get<uint32_t>(0), 5u);
    Msg get;
    get.c = {{kBufB, kInfoSize}};
    CHECK(eventually([&] { return a.call(la, 1, get).result == 0u && a.get<uint8_t>(kBufB + 0x67) == 2u; }));
    CHECK_EQ(b.call(lb, 401).result, 0u);
    CHECK_EQ(a.call(la, 401).result, 0u);
}

// A host LAN whose sockets can go silent: sends are dropped while `mute` is
// set, as when the other device leaves the Wi-Fi or the app is killed.
class MutableLan final : public platform::ILan {
public:
    MutableLan(std::shared_ptr<platform::ILan> lan, std::shared_ptr<std::atomic<bool>> mute)
        : lan_(std::move(lan)), mute_(std::move(mute)) {}
    std::unique_ptr<platform::ILanSocket> open_udp(uint16_t port) override {
        std::unique_ptr<platform::ILanSocket> s = lan_->open_udp(port);
        return s ? std::make_unique<Socket>(std::move(s), mute_) : nullptr;
    }
    std::vector<platform::LanInterface> interfaces() override { return lan_->interfaces(); }

private:
    class Socket final : public platform::ILanSocket {
    public:
        Socket(std::unique_ptr<platform::ILanSocket> s, std::shared_ptr<std::atomic<bool>> mute)
            : s_(std::move(s)), mute_(std::move(mute)) {}
        uint16_t port() const override { return s_->port(); }
        bool send_to(const platform::LanEndpoint& to, const void* data, size_t size) override {
            return *mute_ ? true : s_->send_to(to, data, size);
        }
        int recv_from(void* data, size_t capacity, platform::LanEndpoint* from, int timeout_ms) override {
            return s_->recv_from(data, capacity, from, timeout_ms);
        }

    private:
        std::unique_ptr<platform::ILanSocket> s_;
        std::shared_ptr<std::atomic<bool>> mute_;
    };
    std::shared_ptr<platform::ILan> lan_;
    std::shared_ptr<std::atomic<bool>> mute_;
};

// Keepalive: the access point goes silent without destroying its network.
// The station loses it with SignalLost (6); the access point, no longer
// hearing the station, drops it again.
void test_keepalive() {
    std::shared_ptr<platform::ILan> lan = platform::make_host_lan();
    if (!lan) {
        std::printf("  no host sockets here, skipped\n");
        return;
    }
    auto mute = std::make_shared<std::atomic<bool>>(false);
    LdnLanConfig host_cfg;
    host_cfg.lan = std::make_shared<MutableLan>(lan, mute);
    host_cfg.port = free_port(*lan);
    host_cfg.broadcast = false;
    host_cfg.subnet_probe = false;
    host_cfg.keepalive_ms = 50;
    host_cfg.peer_timeout_ms = 400;
    LdnLanConfig station_cfg = host_cfg;
    station_cfg.lan = lan;
    station_cfg.port = 0;
    station_cfg.peers = {{kLoopback, host_cfg.port}};

    LanEnv a(host_cfg), b(station_cfg);
    const Handle la = a.open(a.open_service("ldn:u"), 0);
    const Handle lb = b.open(b.open_service("ldn:u"), 0);
    CHECK_EQ(a.call(la, 400, init_msg()).result, 0u);
    CHECK_EQ(a.call(la, 200).result, 0u);
    Msg create;
    create.payload = create_payload(2, "Host", kVersion);
    CHECK_EQ(a.call(la, 202, create).result, 0u);
    CHECK_EQ(b.call(lb, 400, init_msg()).result, 0u);
    CHECK_EQ(b.call(lb, 300).result, 0u);
    int16_t found = 0;
    for (int i = 0; i < 5 && found == 0; ++i)
        found = scan(b, lb);
    CHECK_EQ(found, 1);
    const std::vector<uint8_t> scanned = b.read(kBufB, kInfoSize);
    CHECK_EQ(connect(b, lb, scanned, "Guest", kVersion).result, 0u);
    CHECK_EQ(b.call(lb, 0).get<uint32_t>(0), 5u);

    // Quiet but alive: pings keep both sides in the network well past the timeout.
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    CHECK_EQ(b.call(lb, 0).get<uint32_t>(0), 5u);
    Msg get;
    get.c = {{kBufB, kInfoSize}};
    CHECK_EQ(a.call(la, 1, get).result, 0u);
    CHECK_EQ(a.get<uint8_t>(kBufB + 0x67), 2u);

    *mute = true;  // the access point vanishes
    CHECK(eventually([&] { return b.call(lb, 0).get<uint32_t>(0) == 4u; }));
    CHECK_EQ(b.call(lb, 3).get<int16_t>(0), 6);  // SignalLost
    CHECK(eventually([&] {
        return a.call(la, 1, get).result == 0u && a.get<uint8_t>(kBufB + 0x67) == 1u;
    }));
    CHECK_EQ(b.call(lb, 401).result, 0u);
    CHECK_EQ(a.call(la, 401).result, 0u);
}

}  // namespace

int main() {
    RUN(test_endpoint_parse);
    RUN(test_two_runtimes);
    RUN(test_keepalive);
    RUN(test_same_session);
    return finish();
}
