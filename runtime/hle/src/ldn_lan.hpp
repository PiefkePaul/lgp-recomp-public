// ldn over LAN (docs/analysis/44-multiplayer-stand.md): the ldn:u structures
// and LanLink, which carries one ldn network between instances of this
// runtime over UDP on the host's LAN. Shared by ldn.cpp (scan, create,
// connect) and sockets.cpp (UDP sockets inside the ldn network).
//
// Layout of the structures from switchbrew "LDN services" and libnx ldn.h
// (interface documentation only), checked with static_assert. The wire
// protocol between instances is this runtime's own (no console, Ryujinx or
// ldn_mitm protocol): one host UDP socket per instance carries discovery,
// joining and the guest's datagrams, tunnelled with their virtual ldn
// addresses. Star topology: stations talk to the access point, which
// forwards between stations.
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

#include "lgp/hle/services.hpp"
#include "lgp/platform/lan.hpp"

namespace lgp::hle::ldn {

// nn::ldn::DisconnectReason (libnx LdnDisconnectReason)
constexpr int16_t kDisconnectNone = 0;
constexpr int16_t kDisconnectDestroyedByUser = 3;
constexpr int16_t kDisconnectSignalLost = 6;

#pragma pack(push, 1)
struct IntentId {
    int64_t local_communication_id;
    uint8_t reserved_x8[2];
    uint16_t scene_id;
    uint8_t reserved_xc[4];
};
struct NetworkId {
    IntentId intent_id;
    uint8_t session_id[0x10];
};
struct Ssid {
    uint8_t len;
    char str[0x21];
};
struct CommonNetworkInfo {
    uint8_t bssid[6];
    Ssid ssid;
    int16_t channel;
    int8_t link_level;
    uint8_t network_type;
    uint8_t reserved[4];
};
struct NodeInfo {
    uint32_t ip_addr;
    uint8_t mac_addr[6];
    int8_t node_id;
    uint8_t is_connected;
    char user_name[0x21];
    uint8_t platform;
    int16_t local_communication_version;
    uint8_t reserved_x30[0x10];
};
struct NetworkInfo {
    NetworkId network_id;
    CommonNetworkInfo common;
    uint8_t server_random[0x10];
    uint16_t security_mode;
    uint8_t station_accept_policy;
    uint8_t version;
    uint8_t reserved_x14[2];
    int8_t node_count_max;
    uint8_t node_count;
    NodeInfo nodes[8];
    uint8_t reserved_x218[2];
    uint16_t advertise_data_size;
    uint8_t advertise_data[0x180];
    uint8_t reserved_x39c[0x8C];
    uint64_t reserved_x428;
};
struct SecurityConfig {
    uint16_t security_mode;
    uint16_t passphrase_size;
    uint8_t passphrase[0x40];
};
struct SecurityParameter {
    uint8_t server_random[0x10];
    uint8_t session_id[0x10];
};
struct UserConfig {
    char user_name[0x21];
    uint8_t reserved[0xF];
};
struct NetworkConfig {
    IntentId intent_id;
    int16_t channel;
    int8_t node_count_max;
    uint8_t reserved_x13;
    int16_t local_communication_version;
    uint8_t reserved_x16[0xA];
};
struct ScanFilter {
    NetworkId network_id;
    uint32_t network_type;
    uint8_t bssid[6];
    Ssid ssid;
    uint8_t reserved[0x10];
    uint32_t flags;
};
#pragma pack(pop)
static_assert(sizeof(IntentId) == 0x10);
static_assert(sizeof(NetworkId) == 0x20);
static_assert(sizeof(CommonNetworkInfo) == 0x30);
static_assert(sizeof(NodeInfo) == 0x40);
static_assert(offsetof(NetworkInfo, nodes) == 0x68);
static_assert(offsetof(NetworkInfo, advertise_data_size) == 0x26A);
static_assert(sizeof(NetworkInfo) == 0x480);
static_assert(sizeof(SecurityConfig) == 0x44);
static_assert(sizeof(SecurityParameter) == 0x20);
static_assert(sizeof(UserConfig) == 0x30);
static_assert(sizeof(NetworkConfig) == 0x20);
static_assert(offsetof(ScanFilter, flags) == 0x5C);
static_assert(sizeof(ScanFilter) == 0x60);

// ScanFilter::flags (libnx LdnScanFilterFlags).
constexpr uint32_t kFilterLocalCommunicationId = 1u << 0;
constexpr uint32_t kFilterSessionId = 1u << 1;
constexpr uint32_t kFilterSceneId = 1u << 5;
constexpr uint32_t kFilterIntentId = 1u << 6;
constexpr uint32_t kFilterNetworkId = 1u << 7;

bool matches(const ScanFilter& filter, const NetworkInfo& info);

// A guest datagram inside the ldn network (addresses host byte order).
struct Datagram {
    uint32_t src_ip = 0;
    uint16_t src_port = 0;
    std::vector<uint8_t> data;
};

// Carries one ldn network to other nodes: the ldn service's view of the
// transport. LanLink (this runtime's own protocol in the LAN) and RyuLink
// (RyuLDN server, ldn_ryu.hpp) implement it. Thread-safe. The ldn service
// registers as listener for changes that arrive from the network; the
// listener runs on the link's receive thread without any link lock held,
// so it may call back into the link.
class Link {
public:
    class Listener {
    public:
        virtual ~Listener() = default;
        // The network changed (a station joined or left, the host sent new data).
        virtual void network_changed(const NetworkInfo& info) = 0;
        // The access point destroyed the network this station was in.
        virtual void network_lost(int16_t reason) = 0;
    };

    virtual ~Link() = default;

    virtual void set_listener(Listener* listener) = 0;

    // Opens the transport (ldn Initialize); false when there is none (the
    // service then stays alone in the room).
    virtual bool open() = 0;
    virtual void close() = 0;  // Finalize: leaves the network first
    virtual bool is_open() const = 0;

    // Access point. host() takes the network as ldn built it and fills in
    // what the transport decides (node 0's address and MAC); update_host()
    // takes later changes (advertise data, accept policy) but keeps the
    // stations. False from host(): nobody else will see the network.
    // private_network: CreateNetworkPrivate, the game chose the server random.
    virtual bool host(NetworkInfo* info, bool private_network) = 0;
    virtual void update_host(const NetworkInfo& info) = 0;
    virtual void destroy() = 0;  // tells the stations, then forgets the network

    // Random bytes for session ids: two instances must never create the
    // same session (the offline service numbers them per run).
    virtual void random_bytes(uint8_t* out, size_t size) = 0;

    // Station. channel != 0 keeps networks on that channel (and those that
    // did not name one), as a scan on hardware listens on one channel.
    virtual std::vector<NetworkInfo> scan(const ScanFilter& filter, int16_t channel = 0) = 0;
    // Joins the network with that session id (found by an earlier scan).
    // server_random: the guest's SecurityParameter (nullptr: unknown).
    virtual rt::Result connect(const uint8_t (&session_id)[0x10], const uint8_t* server_random, const UserConfig& user,
                               int16_t version, NetworkInfo* out) = 0;
    virtual void disconnect() = 0;  // station: leaves; access point: same as destroy()

    // Inside the network (both roles).
    virtual bool in_network() const = 0;
    virtual uint32_t ip() const = 0;    // our virtual address, 0 outside a network
    virtual uint32_t mask() const = 0;  // the network's subnet mask
    // True for an address of the ldn subnet or a broadcast into it.
    virtual bool routes(uint32_t ip) const = 0;
    virtual bool send(uint16_t src_port, uint32_t dst_ip, uint16_t dst_port, const void* data, size_t size) = 0;
    virtual bool pop(uint16_t port, Datagram* out) = 0;
    // Size of the next datagram for port, -1 when none is queued.
    virtual int peek(uint16_t port) const = 0;
};

// Carries one ldn network between instances of this runtime over UDP in the
// LAN. A pump thread reads the host socket while open.
class LanLink final : public Link {
public:
    explicit LanLink(LdnLanConfig config);
    ~LanLink() override;
    LanLink(const LanLink&) = delete;
    LanLink& operator=(const LanLink&) = delete;

    void set_listener(Listener* listener) override;
    bool open() override;
    void close() override;
    bool is_open() const override;
    uint16_t port() const;  // bound host port, 0 when closed

    // host() gives the network its virtual subnet (10.A.B.0/24 per instance)
    // and makes node 0 ours.
    bool host(NetworkInfo* info, bool private_network) override;
    void update_host(const NetworkInfo& info) override;
    void destroy() override;
    void random_bytes(uint8_t* out, size_t size) override;
    std::vector<NetworkInfo> scan(const ScanFilter& filter, int16_t channel = 0) override;
    rt::Result connect(const uint8_t (&session_id)[0x10], const uint8_t* server_random, const UserConfig& user,
                       int16_t version, NetworkInfo* out) override;
    void disconnect() override;
    bool in_network() const override;
    uint32_t ip() const override;
    static constexpr uint32_t kMask = 0xFFFFFF00;
    uint32_t mask() const override { return kMask; }
    bool routes(uint32_t ip) const override;
    bool send(uint16_t src_port, uint32_t dst_ip, uint16_t dst_port, const void* data, size_t size) override;
    bool pop(uint16_t port, Datagram* out) override;
    int peek(uint16_t port) const override;

private:
    enum class Role { None, Host, Station };
    using Clock = std::chrono::steady_clock;
    struct Station {
        platform::LanEndpoint endpoint;
        bool used = false;
        Clock::time_point last_heard{};
    };
    struct Pending {
        bool done = false;
        rt::Result result{};
        NetworkInfo info{};
        int8_t node_id = -1;
    };
    struct Notice {
        bool changed = false;
        bool lost = false;
        int16_t reason = kDisconnectDestroyedByUser;
        NetworkInfo info{};
    };

    void pump();
    void handle(const uint8_t* data, size_t size, const platform::LanEndpoint& from, Notice* ev);
    void send_packet(const platform::LanEndpoint& to, uint8_t type, const void* body, size_t size);
    void enqueue(uint16_t port, uint32_t src_ip, uint16_t src_port, const uint8_t* data, size_t size);
    void forward(uint32_t src_ip, uint16_t src_port, uint32_t dst_ip, uint16_t dst_port, const uint8_t* data,
                 size_t size, const platform::LanEndpoint* except);
    void broadcast_update();
    void keepalive(Notice* ev);  // pings and timeouts, from the pump thread
    void drop_station(size_t i);
    std::vector<platform::LanEndpoint> probe_targets() const;
    void reset_network();

    const LdnLanConfig config_;
    std::mutex random_mutex_;
    std::mt19937_64 random_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::unique_ptr<platform::ILanSocket> socket_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    Listener* listener_ = nullptr;
    std::mutex listener_mutex_;

    Role role_ = Role::None;
    NetworkInfo network_{};
    int8_t node_id_ = -1;
    platform::LanEndpoint host_endpoint_;
    Clock::time_point host_last_heard_{};
    Clock::time_point last_ping_{};
    // Guest datagrams through the tunnel, logged every few seconds while they
    // flow (does the game talk at all once joined?).
    uint64_t datagrams_out_ = 0, datagrams_in_ = 0, datagrams_logged_ = 0;
    Clock::time_point last_traffic_log_{};
    std::array<Station, 8> stations_{};
    std::map<uint16_t, std::deque<Datagram>> queues_;
    bool scanning_ = false;
    std::vector<NetworkInfo> scan_results_;
    std::vector<platform::LanEndpoint> scan_sources_;  // who sent each scan result
    uint64_t last_scan_outcome_ = ~uint64_t{0};        // addresses/answers/found of the last scan
    uint64_t instance_ = 0;  // random per LanLink: tells our own scan answers apart
    // Where each network seen in a scan lives (session id -> host endpoint).
    std::map<std::array<uint8_t, 0x10>, platform::LanEndpoint> known_hosts_;
    Pending pending_;
    bool connecting_ = false;
    platform::LanEndpoint connect_target_;
};

// ldn:u and bsd:u on top of a Link (ldn.cpp, sockets.cpp).
void register_ldn_service(ServiceManager& sm, std::shared_ptr<Link> link);
void register_bsd_service(ServiceManager& sm, std::shared_ptr<Link> link);

}  // namespace lgp::hle::ldn
