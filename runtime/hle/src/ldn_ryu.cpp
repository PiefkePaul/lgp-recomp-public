// RyuLink: one ldn network through a RyuLDN server (see ldn_ryu.hpp).
// Wire format: a 12-byte header (magic "RLDN", packet id, version 1, two
// bytes padding, s32 body size) and the body; bodies are the packed ldn
// structures (little-endian, addresses as host-order u32 like ldn keeps
// them). Packet ids and layouts after the MIT server (Network/Types).
#include "ldn_ryu.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "ldn_ws.hpp"
#include "lgp/runtime/host_work.hpp"
#include "lgp/runtime/log.hpp"

namespace lgp::hle::ldn {
namespace {

using rt::LogLevel;

constexpr uint32_t kMagic = 'R' | 'L' << 8 | 'D' << 16 | 'N' << 24;
constexpr uint8_t kVersion = 1;
constexpr size_t kMaxPacket = 131072;  // the server refuses larger packets

enum PacketId : uint8_t {
    kInitialize = 0,
    kPassphrase = 1,
    kCreateAccessPoint = 2,
    kCreateAccessPointPrivate = 3,
    kSyncNetwork = 7,
    kScan = 10,
    kScanReply = 11,
    kScanReplyEnd = 12,
    kConnectPrivate = 14,
    kConnected = 15,
    kDisconnect = 16,
    kProxyConfig = 17,
    kProxyData = 20,
    kSetAcceptPolicy = 22,
    kSetAdvertiseData = 23,
    kPing = 254,
    kNetworkError = 255,
};

// NetworkError of the server.
const char* network_error_name(int32_t e) {
    switch (e) {
    case 1: return "port unreachable";
    case 2: return "too many players";
    case 3: return "version too low";
    case 4: return "version too high";
    case 5: return "connect failure";
    case 6: return "room not found";
    case 7: return "connect timeout";
    case 8: return "connect rejected";
    case 9: return "reject failed";
    case 127: return "banned by the server";
    case -1: return "unknown (room id taken?)";
    default: return "?";
    }
}

#pragma pack(push, 1)
struct Header {
    uint32_t magic;
    uint8_t type;
    uint8_t version;
    uint8_t reserved[2];
    int32_t size;
};
struct InitializeMessage {
    uint8_t id[16];
    uint8_t mac[6];
};
struct PassphraseMessage {
    char passphrase[128];
};
struct AddressEntry {
    uint32_t ip;
    uint8_t mac[6];
    uint16_t reserved;
};
struct RyuNetworkConfig {
    char game_version[16];
    uint8_t private_ip[16];
    int32_t address_family;
    uint16_t external_proxy_port;  // 0: no P2P proxy
    uint16_t internal_proxy_port;
};
struct CreateAccessPointPrivateRequest {
    SecurityConfig security;
    SecurityParameter parameter;
    UserConfig user;
    NetworkConfig network;
    AddressEntry addresses[8];
    RyuNetworkConfig ryu;
    // advertise data follows
};
// The server reads 0xC0 bytes (its managed layout puts NetworkConfig, which
// holds an s64, on an 8-byte boundary; the declared size 0xBC is too small).
struct ConnectPrivateRequest {
    SecurityConfig security;
    SecurityParameter parameter;
    UserConfig user;
    uint32_t local_communication_version;
    uint32_t option;
    uint8_t padding[4];
    NetworkConfig network;
};
struct ProxyInfo {
    uint32_t src_ip;
    uint16_t src_port;
    uint32_t dst_ip;
    uint16_t dst_port;
    int32_t protocol;  // System.Net.Sockets.ProtocolType: 17 = UDP
};
struct ProxyDataHeader {
    ProxyInfo info;
    uint32_t length;
};
struct ProxyConfig {
    uint32_t ip;
    uint32_t mask;
};
struct PingMessage {
    uint8_t requester;  // 0: the server asks, the client sends the same message back
    uint8_t id;
};
struct DisconnectMessage {
    uint32_t ip;
};
#pragma pack(pop)
static_assert(sizeof(Header) == 0xC);
static_assert(sizeof(InitializeMessage) == 0x16);
static_assert(sizeof(RyuNetworkConfig) == 0x28);
static_assert(sizeof(CreateAccessPointPrivateRequest) == 0x13C);
static_assert(sizeof(ConnectPrivateRequest) == 0xC0);
static_assert(sizeof(ProxyInfo) == 0x10);
static_assert(sizeof(ProxyDataHeader) == 0x14);

constexpr int32_t kProtocolUdp = 17;
constexpr uint32_t kDefaultMask = 0xFFFF0000;  // the server's 10.114.0.0/16
constexpr uint32_t kConnectTimeoutMs = 3000;
constexpr uint32_t kReplyTimeoutMs = 3000;
constexpr uint32_t kRetryMs = 15000;           // after a failed connect to the server
constexpr uint32_t kServerSilentMs = 30000;    // it pings every 10 s when idle
constexpr size_t kMaxQueued = 256;
// The server also takes 192.168.0.255 for the room's broadcast address.
constexpr uint32_t kLanBroadcast = 0xC0A800FF;

// A broadcast into the room: everyone, the subnet's broadcast address, the
// one above, and x.y.z.255 inside the subnet as on a /24 (the virtual
// addresses are handed out from .1 up; .255 is never one in practice).
bool is_broadcast(uint32_t dst, uint32_t own, uint32_t mask) {
    return dst == 0xFFFFFFFFu || dst == kLanBroadcast || (dst | mask) == 0xFFFFFFFFu ||
           ((dst & mask) == (own & mask) && (dst & 0xFF) == 0xFF);
}

// ScanFilter flags as the server reads them (one byte; IntentId and NetworkId
// are combinations there, separate bits in ldn_lan.hpp).
uint8_t server_filter_flags(uint32_t flags) {
    uint8_t out = static_cast<uint8_t>(flags & (kFilterLocalCommunicationId | kFilterSessionId | kFilterSceneId));
    if (flags & kFilterIntentId)
        out |= kFilterLocalCommunicationId | kFilterSceneId;
    if (flags & kFilterNetworkId)
        out |= kFilterLocalCommunicationId | kFilterSceneId | kFilterSessionId;
    return out;
}

std::string ip_string(uint32_t ip) {
    char s[16];
    std::snprintf(s, sizeof(s), "%u.%u.%u.%u", ip >> 24, ip >> 16 & 0xFF, ip >> 8 & 0xFF, ip & 0xFF);
    return s;
}

}  // namespace
}  // namespace lgp::hle::ldn

namespace lgp::hle {
std::string ryuldn_passphrase(const std::string& room) {
    if (room.empty())
        return {};
    uint32_t h = 2166136261u;
    for (const unsigned char c : room) {
        h ^= c;
        h *= 16777619u;
    }
    char s[17];
    std::snprintf(s, sizeof(s), "Ryujinx-%08x", h);
    return s;
}

bool parse_ryuldn_server(const std::string& text, LdnLanConfig* config) {
    std::string rest = text;
    bool websocket = false;
    std::string path = "/";
    const bool ws = rest.compare(0, 5, "ws://") == 0;
    const bool tls = rest.compare(0, 6, "wss://") == 0;
    if (ws || tls) {
        websocket = true;
        rest = rest.substr(tls ? 6 : 5);
        const size_t slash = rest.find('/');
        if (slash != std::string::npos) {
            path = rest.substr(slash);
            rest.resize(slash);
        }
    } else if (rest.find('/') != std::string::npos) {
        return false;  // other schemes are not supported
    }
    uint64_t port = tls ? 443 : websocket ? 80 : 30456;
    const size_t colon = rest.rfind(':');
    if (colon != std::string::npos) {
        const std::string digits = rest.substr(colon + 1);
        if (digits.empty() || digits.size() > 5 || digits.find_first_not_of("0123456789") != std::string::npos)
            return false;
        port = std::strtoull(digits.c_str(), nullptr, 10);
        rest.resize(colon);
    }
    if (rest.empty() || port == 0 || port > 65535)
        return false;
    config->ryuldn_server = rest;
    config->ryuldn_port = static_cast<uint16_t>(port);
    config->ryuldn_websocket = websocket;
    config->ryuldn_tls = tls;
    config->ryuldn_path = path;
    return true;
}
}  // namespace lgp::hle

namespace lgp::hle::ldn {

RyuLink::RyuLink(LdnLanConfig config) : config_(std::move(config)), passphrase_(ryuldn_passphrase(config_.ryuldn_room)) {
    std::random_device device;
    const uint64_t now = static_cast<uint64_t>(Clock::now().time_since_epoch().count());
    std::seed_seq seed{device(), device(), static_cast<unsigned>(now), static_cast<unsigned>(now >> 32)};
    random_.seed(seed);
}

RyuLink::~RyuLink() {
    close();
    std::unique_lock lock(mutex_);
    drop_connection(lock);
}

void RyuLink::random_bytes(uint8_t* out, size_t size) {
    std::lock_guard lock(random_mutex_);
    for (size_t i = 0; i < size; ++i)
        out[i] = static_cast<uint8_t>(random_() >> 56);
}

void RyuLink::set_listener(Listener* listener) {
    std::lock_guard lock(listener_mutex_);
    listener_ = listener;
}

bool RyuLink::open() {
    std::unique_lock lock(mutex_);
    if (!config_.lan)
        return false;
    opened_ = true;
    attempted_ = false;  // a new ldn session tries the server again at once
    ensure_connected(lock);
    return true;
}

void RyuLink::close() {
    disconnect();
    std::unique_lock lock(mutex_);
    opened_ = false;
    queues_.clear();
    // The connection stays: games finalize ldn between scan and connect, and
    // a reconnect costs a round trip. It goes with the link or the server.
}

bool RyuLink::is_open() const {
    std::lock_guard lock(mutex_);
    return opened_;
}

bool RyuLink::ensure_connected(std::unique_lock<std::mutex>& lock) {
    if (connected_)
        return true;
    const Clock::time_point now = Clock::now();
    if (attempted_ && now - last_attempt_ < std::chrono::milliseconds(kRetryMs))
        return false;
    attempted_ = true;
    last_attempt_ = now;
    drop_connection(lock);
    const std::string server = config_.ryuldn_server;
    const uint16_t port = config_.ryuldn_port;
    lock.unlock();  // the name lookup and connect may take seconds
    std::string tls_error, ws_error;
    std::unique_ptr<platform::ILanStream> stream;
    {
        rt::HostWorkScope idle(true);  // the guest thread's core runs other threads meanwhile (see wait())
        stream = config_.ryuldn_tls
                     ? config_.lan->connect_tls(server, port, kConnectTimeoutMs, config_.ryuldn_ca_file, &tls_error)
                     : config_.lan->connect_tcp(server, port, kConnectTimeoutMs);
        if (stream && config_.ryuldn_websocket)
            stream = websocket_connect(std::move(stream), server, port, config_.ryuldn_tls, config_.ryuldn_path,
                                       kReplyTimeoutMs, &ws_error);
    }
    lock.lock();
    if (!stream) {
        if (!tls_error.empty())
            rt::log(LogLevel::Warn, "ldn ryu: TLS to %s:%u failed: %s (next try in %u s)", server.c_str(), port,
                    tls_error.c_str(), kRetryMs / 1000);
        else if (!ws_error.empty())
            rt::log(LogLevel::Warn, "ldn ryu: WebSocket to %s:%u%s failed: %s (next try in %u s)", server.c_str(),
                    port, config_.ryuldn_path.c_str(), ws_error.c_str(), kRetryMs / 1000);
        else
            rt::log(LogLevel::Warn, "ldn ryu: cannot reach the RyuLDN server %s:%u (next try in %u s)",
                    server.c_str(), port, kRetryMs / 1000);
        return false;
    }
    stream_ = std::move(stream);
    last_heard_ = Clock::now();
    platform::ILanStream* raw = stream_.get();
    reader_alive_ = true;
    thread_ = std::thread([this, raw] { reader(raw); });
    init_ = Pending{};
    init_.waiting = true;
    InitializeMessage init{};
    std::memcpy(init.id, client_id_.data(), sizeof(init.id));
    std::memcpy(init.mac, mac_.data(), sizeof(init.mac));
    send_packet(kInitialize, &init, sizeof(init));
    if (!wait(lock, init_, kReplyTimeoutMs) || init_.error != 0) {
        rt::log(LogLevel::Warn, "ldn ryu: %s:%u does not answer as a RyuLDN server", server.c_str(), port);
        drop_connection(lock);
        return false;
    }
    PassphraseMessage pass{};
    std::memcpy(pass.passphrase, passphrase_.data(), std::min(passphrase_.size(), sizeof(pass.passphrase) - 1));
    send_packet(kPassphrase, &pass, sizeof(pass));
    connected_ = true;
    rt::log(LogLevel::Info, "ldn ryu: connected to %s:%u%s (%s room), MAC %02x:%02x:%02x:%02x:%02x:%02x",
            server.c_str(), port,
            config_.ryuldn_tls ? " over WebSocket with TLS" : config_.ryuldn_websocket ? " over WebSocket" : "",
            passphrase_.empty() ? "public" : "private", mac_[0], mac_[1], mac_[2], mac_[3], mac_[4], mac_[5]);
    return true;
}

void RyuLink::drop_connection(std::unique_lock<std::mutex>& lock) {
    connected_ = false;
    if (stream_)
        stream_->shutdown();
    if (thread_.joinable()) {
        std::thread t = std::move(thread_);
        lock.unlock();  // the reader takes mutex_ for each packet
        t.join();
        lock.lock();
    }
    stream_.reset();
}

bool RyuLink::send_packet(uint8_t type, const void* body, size_t size, const void* extra, size_t extra_size) {
    if (!stream_)
        return false;
    std::vector<uint8_t> p(sizeof(Header) + size + extra_size);
    const Header h{kMagic, type, kVersion, {}, static_cast<int32_t>(size + extra_size)};
    std::memcpy(p.data(), &h, sizeof(h));
    if (size != 0)
        std::memcpy(p.data() + sizeof(h), body, size);
    if (extra_size != 0)
        std::memcpy(p.data() + sizeof(h) + size, extra, extra_size);
    std::lock_guard lock(send_mutex_);
    return stream_->send_all(p.data(), p.size());
}

bool RyuLink::wait(std::unique_lock<std::mutex>& lock, Pending& p, uint32_t timeout_ms) {
    {
        // The guest thread of the HLE request only waits for the server (a
        // scan waits 200 ms plus the round trip): its core runs other guest
        // threads meanwhile, as on Horizon, where the client blocks in the
        // IPC. Holding the core slot made the render thread on the same core
        // wait as long (analysis of the PC runs 24.10.2026: up to 8 ms per
        // frame at 200 ms delay).
        rt::HostWorkScope idle(true);
        cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] { return p.done || !reader_alive_; });
        lock.unlock();  // back to the slot without mutex_: a slot holder may be in send()
    }
    lock.lock();
    p.waiting = false;
    if (!p.done && reader_alive_) {
        // The protocol has no request ids: a late answer would be taken for the next request's. Retire the
        // connection (the reader reports a lost room as usual); the next request connects again.
        rt::log(LogLevel::Warn, "ldn ryu: no answer from the server in %u ms, reconnecting", timeout_ms);
        drop_connection(lock);
    }
    return p.done;
}

void RyuLink::reset_network() {
    role_ = Role::None;
    network_ = NetworkInfo{};
    node_id_ = -1;
    have_server_random_ = false;
    queues_.clear();
}

void RyuLink::patch_info(NetworkInfo* info) const {
    // The server does not keep the SecurityParameter's random part (zero in
    // its NetworkInfo); put back the one the game gave us.
    if (have_server_random_)
        std::memcpy(info->server_random, server_random_, sizeof(server_random_));
}

bool RyuLink::host(NetworkInfo* info, bool private_network) {
    std::unique_lock lock(mutex_);
    if (!opened_ || !ensure_connected(lock))
        return false;
    if (role_ != Role::None) {
        const DisconnectMessage leave{};
        send_packet(kDisconnect, &leave, sizeof(leave));  // the server needs the body
        reset_network();
    }
    CreateAccessPointPrivateRequest req{};
    req.security.security_mode = info->security_mode;
    std::memcpy(req.parameter.server_random, info->server_random, sizeof(req.parameter.server_random));
    std::memcpy(req.parameter.session_id, info->network_id.session_id, sizeof(req.parameter.session_id));
    std::memcpy(req.user.user_name, info->nodes[0].user_name, sizeof(req.user.user_name) - 1);
    req.network.intent_id = info->network_id.intent_id;
    req.network.channel = info->common.channel;
    req.network.node_count_max = info->node_count_max;
    req.network.local_communication_version = info->nodes[0].local_communication_version;
    std::memcpy(req.ryu.game_version, "lgp", 3);
    req.ryu.address_family = 2;  // InterNetwork
    const size_t adv = std::min<size_t>(info->advertise_data_size, sizeof(info->advertise_data));
    join_ = Pending{};
    join_.waiting = true;
    send_packet(kCreateAccessPointPrivate, &req, sizeof(req), info->advertise_data, adv);
    if (!wait(lock, join_, kReplyTimeoutMs)) {
        rt::log(LogLevel::Warn, "ldn ryu: no answer to CreateAccessPointPrivate");
        return false;
    }
    if (join_.error != 0) {
        rt::log(LogLevel::Warn, "ldn ryu: the server did not open the room: %s (%d)", network_error_name(join_.error),
                join_.error);
        return false;
    }
    // The server drops the server random, so stations only get it back when they
    // pass it themselves (ConnectPrivate). For CreateNetwork the stations join
    // with Connect and the random of the scan (zero): keep the server's zero
    // so both sides agree.
    std::memcpy(server_random_, info->server_random, sizeof(server_random_));
    have_server_random_ = private_network;
    network_ = join_.info;
    patch_info(&network_);
    // Keep what the game set and the server does not carry over.
    network_.station_accept_policy = info->station_accept_policy;
    role_ = Role::Host;
    node_id_ = 0;
    *info = network_;
    rt::log(LogLevel::Info, "ldn ryu: hosting room as %s (session %02x%02x%02x%02x...)",
            ip_string(network_.nodes[0].ip_addr).c_str(), network_.network_id.session_id[0],
            network_.network_id.session_id[1], network_.network_id.session_id[2], network_.network_id.session_id[3]);
    return true;
}

void RyuLink::update_host(const NetworkInfo& info) {
    std::lock_guard lock(mutex_);
    if (role_ != Role::Host || !connected_)
        return;
    const size_t adv = std::min<size_t>(info.advertise_data_size, sizeof(info.advertise_data));
    if (adv != network_.advertise_data_size || std::memcmp(info.advertise_data, network_.advertise_data, adv) != 0) {
        network_.advertise_data_size = static_cast<uint16_t>(adv);
        std::memcpy(network_.advertise_data, info.advertise_data, sizeof(network_.advertise_data));
        send_packet(kSetAdvertiseData, info.advertise_data, adv);
    }
    if (info.station_accept_policy != network_.station_accept_policy) {
        network_.station_accept_policy = info.station_accept_policy;
        send_packet(kSetAcceptPolicy, &info.station_accept_policy, 1);
    }
}

void RyuLink::destroy() {
    std::lock_guard lock(mutex_);
    if (role_ != Role::Host)
        return;
    const DisconnectMessage msg{};
    send_packet(kDisconnect, &msg, sizeof(msg));  // the server closes the room and tells the stations
    rt::log(LogLevel::Info, "ldn ryu: room closed");
    reset_network();
}

std::vector<NetworkInfo> RyuLink::scan(const ScanFilter& filter, int16_t channel) {
    std::unique_lock lock(mutex_);
    if (!opened_ || !ensure_connected(lock))
        return {};
    ScanFilter f = filter;
    f.flags = server_filter_flags(filter.flags);
    scan_results_.clear();
    scanning_ = true;
    scan_done_ = Pending{};
    scan_done_.waiting = true;
    send_packet(kScan, &f, sizeof(f));
    const bool complete = wait(lock, scan_done_, kReplyTimeoutMs);  // the server waits 200 ms before it answers
    scanning_ = false;
    std::vector<NetworkInfo> found;
    size_t other_channel = 0;
    for (const NetworkInfo& n : scan_results_) {
        if (!matches(filter, n))
            continue;
        if (channel != 0 && n.common.channel != 0 && n.common.channel != channel)
            ++other_channel;
        else
            found.push_back(n);
    }
    rt::log(found.empty() ? LogLevel::Debug : LogLevel::Info,
            "ldn ryu: scan%s: %zu room(s) from the server, %zu on another channel than %d, found %zu",
            complete ? "" : " (no end mark)", scan_results_.size(), other_channel, channel, found.size());
    return found;
}

rt::Result RyuLink::connect(const uint8_t (&session_id)[0x10], const uint8_t* server_random, const UserConfig& user,
                            int16_t version, NetworkInfo* out) {
    std::unique_lock lock(mutex_);
    if (!opened_ || !ensure_connected(lock))
        return ldn_result::kNetworkNotFound;
    if (role_ != Role::None) {
        const DisconnectMessage leave{};
        send_packet(kDisconnect, &leave, sizeof(leave));  // the server needs the body
        reset_network();
    }
    ConnectPrivateRequest req{};
    req.security.security_mode = 1;
    std::memcpy(req.parameter.session_id, session_id, sizeof(req.parameter.session_id));
    if (server_random != nullptr)
        std::memcpy(req.parameter.server_random, server_random, sizeof(req.parameter.server_random));
    req.user = user;
    req.local_communication_version = static_cast<uint32_t>(static_cast<uint16_t>(version));
    join_ = Pending{};
    join_.waiting = true;
    send_packet(kConnectPrivate, &req, sizeof(req));
    if (!wait(lock, join_, kReplyTimeoutMs)) {
        rt::log(LogLevel::Warn, "ldn ryu: no answer to ConnectPrivate");
        return ldn_result::kNetworkNotFound;
    }
    if (join_.error != 0) {
        rt::log(LogLevel::Info, "ldn ryu: joining refused: %s (%d)", network_error_name(join_.error), join_.error);
        return ldn_result::kNetworkNotFound;
    }
    have_server_random_ = server_random != nullptr;
    if (have_server_random_)
        std::memcpy(server_random_, server_random, sizeof(server_random_));
    network_ = join_.info;
    patch_info(&network_);
    node_id_ = -1;
    for (int8_t i = 0; i < 8; ++i)
        if (network_.nodes[i].is_connected && network_.nodes[i].ip_addr == proxy_ip_)
            node_id_ = i;
    if (node_id_ < 0) {
        for (int8_t i = 0; i < 8; ++i)
            if (network_.nodes[i].is_connected && std::memcmp(network_.nodes[i].mac_addr, mac_.data(), 6) == 0)
                node_id_ = i;
    }
    if (node_id_ < 0) {
        rt::log(LogLevel::Warn, "ldn ryu: joined, but our node is not in the room");
        const DisconnectMessage leave{};
        send_packet(kDisconnect, &leave, sizeof(leave));
        reset_network();
        return ldn_result::kNetworkNotFound;
    }
    role_ = Role::Station;
    *out = network_;
    rt::log(LogLevel::Info, "ldn ryu: joined room as node %d (%s)", node_id_,
            ip_string(network_.nodes[node_id_].ip_addr).c_str());
    return rt::kSuccess;
}

void RyuLink::disconnect() {
    std::unique_lock lock(mutex_);
    if (role_ == Role::None)
        return;
    const DisconnectMessage msg{};
    send_packet(kDisconnect, &msg, sizeof(msg));
    rt::log(LogLevel::Info, role_ == Role::Host ? "ldn ryu: room closed" : "ldn ryu: left the room");
    reset_network();
}

bool RyuLink::in_network() const {
    std::lock_guard lock(mutex_);
    return role_ != Role::None;
}

uint32_t RyuLink::ip() const {
    std::lock_guard lock(mutex_);
    return role_ == Role::None ? 0 : network_.nodes[node_id_].ip_addr;
}

uint32_t RyuLink::mask() const {
    std::lock_guard lock(mutex_);
    return proxy_mask_ != 0 ? proxy_mask_ : kDefaultMask;
}

bool RyuLink::routes(uint32_t ip) const {
    std::lock_guard lock(mutex_);
    if (role_ == Role::None)
        return false;
    const uint32_t m = proxy_mask_ != 0 ? proxy_mask_ : kDefaultMask;
    return ip == 0xFFFFFFFFu || ip == kLanBroadcast || (ip & m) == (network_.nodes[node_id_].ip_addr & m);
}

bool RyuLink::send(uint16_t src_port, uint32_t dst_ip, uint16_t dst_port, const void* data, size_t size) {
    std::lock_guard lock(mutex_);
    if (role_ == Role::None || !connected_)
        return false;
    const uint32_t own = network_.nodes[node_id_].ip_addr;
    const uint32_t m = proxy_mask_ != 0 ? proxy_mask_ : kDefaultMask;
    if (dst_ip == own) {
        enqueue(dst_port, own, src_port, static_cast<const uint8_t*>(data), size);
        return true;
    }
    // Broadcasts go to the subnet's broadcast address, which the server
    // sends to every member, the sender included.
    if (is_broadcast(dst_ip, own, m))
        dst_ip = (own & m) | ~m;
    ProxyDataHeader h{};
    h.info.src_ip = own;
    h.info.src_port = src_port;
    h.info.dst_ip = dst_ip;
    h.info.dst_port = dst_port;
    h.info.protocol = kProtocolUdp;
    h.length = static_cast<uint32_t>(size);
    ++datagrams_out_;
    return send_packet(kProxyData, &h, sizeof(h), data, size);
}

bool RyuLink::pop(uint16_t port, Datagram* out) {
    std::lock_guard lock(mutex_);
    const auto it = queues_.find(port);
    if (it == queues_.end() || it->second.empty())
        return false;
    *out = std::move(it->second.front());
    it->second.pop_front();
    return true;
}

int RyuLink::peek(uint16_t port) const {
    std::lock_guard lock(mutex_);
    const auto it = queues_.find(port);
    if (it == queues_.end() || it->second.empty())
        return -1;
    return static_cast<int>(it->second.front().data.size());
}

void RyuLink::enqueue(uint16_t port, uint32_t src_ip, uint16_t src_port, const uint8_t* data, size_t size) {
    std::deque<Datagram>& q = queues_[port];
    if (q.size() >= kMaxQueued)
        q.pop_front();
    Datagram d;
    d.src_ip = src_ip;
    d.src_port = src_port;
    d.data.assign(data, data + size);
    q.push_back(std::move(d));
}

void RyuLink::reader(platform::ILanStream* stream) {
    std::vector<uint8_t> buf;
    std::vector<uint8_t> chunk(65536);
    bool alive = true;
    while (alive) {
        const int n = stream->recv(chunk.data(), chunk.size(), 1000);
        Notice notice;
        {
            std::lock_guard lock(mutex_);
            if (n < 0) {
                alive = false;
            } else if (n > 0) {
                last_heard_ = Clock::now();
                buf.insert(buf.end(), chunk.begin(), chunk.begin() + n);
                size_t at = 0;
                while (buf.size() - at >= sizeof(Header)) {
                    Header h;
                    std::memcpy(&h, buf.data() + at, sizeof(h));
                    if (h.magic != kMagic || h.version != kVersion || h.size < 0 ||
                        static_cast<size_t>(h.size) + sizeof(h) > kMaxPacket) {
                        rt::log(LogLevel::Warn, "ldn ryu: bad packet from the server (magic %08x, version %u)",
                                h.magic, h.version);
                        alive = false;
                        break;
                    }
                    if (buf.size() - at < sizeof(h) + static_cast<size_t>(h.size))
                        break;
                    handle(h.type, buf.data() + at + sizeof(h), static_cast<size_t>(h.size), &notice);
                    at += sizeof(h) + static_cast<size_t>(h.size);
                }
                buf.erase(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(at));
            } else if (Clock::now() - last_heard_ > std::chrono::milliseconds(kServerSilentMs)) {
                // The server pings every client that stayed silent for 10 s, in a room or not.
                rt::log(LogLevel::Warn, "ldn ryu: nothing from the server for %u s", kServerSilentMs / 1000);
                alive = false;
            }
            if (datagrams_out_ + datagrams_in_ != datagrams_logged_ &&
                Clock::now() - last_traffic_log_ >= std::chrono::seconds(5)) {
                rt::log(LogLevel::Info, "ldn ryu: game datagrams so far: %llu sent, %llu received",
                        static_cast<unsigned long long>(datagrams_out_),
                        static_cast<unsigned long long>(datagrams_in_));
                datagrams_logged_ = datagrams_out_ + datagrams_in_;
                last_traffic_log_ = Clock::now();
            }
            if (!alive) {
                if (connected_)
                    rt::log(LogLevel::Warn, "ldn ryu: connection to the server lost");
                connected_ = false;
                reader_alive_ = false;  // wakes whoever waits for an answer
                attempted_ = false;     // reconnect at the next scan, host or connect
                if (role_ == Role::Station) {
                    reset_network();
                    notice.lost = true;
                    notice.reason = kDisconnectSignalLost;
                } else if (role_ == Role::Host) {
                    // ldn has no way to tell an access point its network is gone. Keep the room as the game
                    // knows it, alone like a host whose server was never reached; the next host() opens a new
                    // one on the server.
                    rt::log(LogLevel::Warn, "ldn ryu: hosting alone until the game opens a new room");
                }
            }
        }
        cv_.notify_all();
        if (notice.changed || notice.lost) {
            std::lock_guard lock(listener_mutex_);
            if (listener_ != nullptr) {
                if (notice.lost)
                    listener_->network_lost(notice.reason);
                else
                    listener_->network_changed(notice.info);
            }
        }
    }
}

void RyuLink::handle(uint8_t type, const uint8_t* body, size_t size, Notice* ev) {
    switch (type) {
    case kInitialize: {
        if (size < sizeof(InitializeMessage) || !init_.waiting)
            return;
        InitializeMessage m;
        std::memcpy(&m, body, sizeof(m));
        std::memcpy(client_id_.data(), m.id, client_id_.size());
        std::memcpy(mac_.data(), m.mac, mac_.size());
        init_.done = true;
        return;
    }
    case kProxyConfig: {
        if (size < sizeof(ProxyConfig))
            return;
        ProxyConfig c;
        std::memcpy(&c, body, sizeof(c));
        proxy_ip_ = c.ip;
        proxy_mask_ = c.mask;
        return;
    }
    case kConnected: {
        if (size < sizeof(NetworkInfo) || !join_.waiting)
            return;
        std::memcpy(&join_.info, body, sizeof(NetworkInfo));
        join_.done = true;
        return;
    }
    case kNetworkError: {
        if (size < sizeof(int32_t))
            return;
        int32_t e = 0;
        std::memcpy(&e, body, sizeof(e));
        rt::log(LogLevel::Info, "ldn ryu: server error: %s (%d)", network_error_name(e), e);
        if (e == 1)  // port unreachable: only about the P2P proxy, which we do not offer
            return;
        if (join_.waiting) {
            join_.error = e;
            join_.done = true;
        } else if (init_.waiting) {
            init_.error = e;
            init_.done = true;
        }
        return;
    }
    case kSyncNetwork: {
        if (size < sizeof(NetworkInfo) || role_ == Role::None)
            return;
        NetworkInfo info;
        std::memcpy(&info, body, sizeof(info));
        patch_info(&info);
        if (role_ == Role::Host) {
            // A station joined or left; the rest stays as the game set it.
            info.station_accept_policy = network_.station_accept_policy;
            if (info.node_count != network_.node_count)
                rt::log(LogLevel::Info, "ldn ryu: room has %u node(s) now", info.node_count);
        }
        network_ = info;
        ev->changed = true;
        ev->info = network_;
        return;
    }
    case kScanReply: {
        if (size < sizeof(NetworkInfo) || !scanning_)
            return;
        NetworkInfo info;
        std::memcpy(&info, body, sizeof(info));
        scan_results_.push_back(info);
        return;
    }
    case kScanReplyEnd:
        if (scan_done_.waiting)
            scan_done_.done = true;
        return;
    case kDisconnect:
        if (role_ != Role::Station)
            return;
        rt::log(LogLevel::Info, "ldn ryu: the room was closed");
        reset_network();
        ev->lost = true;
        return;
    case kProxyData: {
        if (size < sizeof(ProxyDataHeader) || role_ == Role::None)
            return;
        ProxyDataHeader h;
        std::memcpy(&h, body, sizeof(h));
        if (h.info.protocol != kProtocolUdp || h.length > size - sizeof(h))
            return;
        const uint32_t own = network_.nodes[node_id_].ip_addr;
        const uint32_t m = proxy_mask_ != 0 ? proxy_mask_ : kDefaultMask;
        const bool broadcast = h.info.dst_ip == ((own & m) | ~m) || h.info.dst_ip == 0xFFFFFFFFu;
        if (!broadcast && h.info.dst_ip != own)
            return;
        enqueue(h.info.dst_port, h.info.src_ip, h.info.src_port, body + sizeof(h), h.length);
        ++datagrams_in_;
        return;
    }
    case kPing: {
        if (size < sizeof(PingMessage))
            return;
        PingMessage p;
        std::memcpy(&p, body, sizeof(p));
        if (p.requester == 0)
            send_packet(kPing, &p, sizeof(p));
        return;
    }
    default:
        return;  // P2P proxy, TCP proxy and reject packets: not used
    }
}

}  // namespace lgp::hle::ldn
