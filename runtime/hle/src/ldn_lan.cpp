// LanLink: one ldn network between instances of this runtime over UDP (see
// ldn_lan.hpp). Wire format: every packet starts with Header; the bodies are
// the packed ldn structures as the guest uses them (little-endian hosts).
#include "ldn_lan.hpp"

#include <algorithm>
#include <chrono>
#include <random>
#include <cstring>

#include "lgp/runtime/host_work.hpp"
#include "lgp/runtime/log.hpp"

namespace lgp::hle::ldn {
namespace {

using rt::LogLevel;
using platform::LanEndpoint;

constexpr uint32_t kMagic = 0x4C50474C;  // "LGPL"
// 2: keepalive (kPing); a version-1 peer would be dropped as silent, so the
// versions do not talk to each other at all. 3: scans carry the instance id
// (two instances may host networks with the same session id: LGP's
// CreateNetworkPrivate takes it from the game).
constexpr uint8_t kVersion = 3;

enum Type : uint8_t {
    kScanRequest = 1,    // body: ScanRequestBody
    kScanResponse = 2,   // body: u64 instance id of the access point, NetworkInfo
    kConnectRequest = 3,  // body: ConnectBody
    kConnectAccept = 4,  // body: int8 node id, pad[3], NetworkInfo
    kConnectReject = 5,  // body: u32 result
    kNetworkUpdate = 6,  // body: NetworkInfo
    kLeave = 7,          // body: none (station -> access point)
    kDestroyed = 8,      // body: none (access point -> stations)
    kData = 9,           // body: DataHeader, payload
    kPing = 10,          // body: none (keepalive, both directions inside a network)
};

#pragma pack(push, 1)
struct Header {
    uint32_t magic;
    uint8_t version;
    uint8_t type;
    uint8_t reserved[2];
};
struct ScanRequestBody {
    int64_t local_communication_id;  // 0 = any
    uint64_t instance;               // the scanning instance (it ignores its own probe)
};
struct ConnectBody {
    uint8_t session_id[0x10];
    UserConfig user;
    int16_t version;
    uint8_t reserved[2];
};
struct AcceptBody {
    int8_t node_id;
    uint8_t reserved[3];
    NetworkInfo info;
};
struct DataHeader {
    uint32_t src_ip;
    uint32_t dst_ip;
    uint16_t src_port;
    uint16_t dst_port;
};
#pragma pack(pop)

constexpr size_t kMaxPacket = 65536;
constexpr size_t kMaxQueued = 256;  // datagrams per port before the oldest go

std::array<uint8_t, 0x10> session_key(const uint8_t (&id)[0x10]) {
    std::array<uint8_t, 0x10> k{};
    std::memcpy(k.data(), id, k.size());
    return k;
}

// 10.A.B.0/24 with A in 96..127 and B in 1..254, chosen per access point
// instance (not from the session id: two searching LGP instances host with
// the same one, and identical addresses and MACs made both networks look
// alike; about 8000 subnets keep two instances apart). Not link-local: node 0
// of a real network is reportedly never link-local, and RyuLDN also uses a
// 10/8 network (concept only; docs/analysis/44-multiplayer-stand.md).
uint32_t subnet_for_instance(uint64_t instance) {
    const uint32_t a = 96 + static_cast<uint32_t>((instance >> 8) % 32);
    const uint32_t b = 1 + static_cast<uint32_t>((instance >> 16) % 254);
    return 0x0A000000u | a << 16 | b << 8;
}

// The subnet of a network: node 0's address names it.
uint32_t subnet_of(const NetworkInfo& info) { return info.nodes[0].ip_addr & 0xFFFFFF00u; }

void make_node(NetworkInfo* info, int8_t id, uint32_t subnet, const char* user_name, int16_t version) {
    NodeInfo& n = info->nodes[id];
    n = NodeInfo{};
    n.ip_addr = subnet | static_cast<uint32_t>(id + 1);
    // Locally administered MAC from the node's address (02:00:a.b.c.d), so
    // it is unique per node and the same wherever the NetworkInfo goes.
    const uint32_t ip = n.ip_addr;
    const uint8_t mac[6] = {0x02, 0x00, static_cast<uint8_t>(ip >> 24), static_cast<uint8_t>(ip >> 16),
                            static_cast<uint8_t>(ip >> 8), static_cast<uint8_t>(ip)};
    std::memcpy(n.mac_addr, mac, sizeof(mac));
    n.node_id = id;
    n.is_connected = 1;
    std::memcpy(n.user_name, user_name, sizeof(n.user_name) - 1);
    n.local_communication_version = version;
}

}  // namespace

bool matches(const ScanFilter& f, const NetworkInfo& info) {
    const IntentId& want = f.network_id.intent_id;
    const IntentId& have = info.network_id.intent_id;
    const uint32_t flags = f.flags;
    if ((flags & (kFilterLocalCommunicationId | kFilterIntentId | kFilterNetworkId)) &&
        want.local_communication_id != have.local_communication_id)
        return false;
    if ((flags & (kFilterSceneId | kFilterIntentId | kFilterNetworkId)) && want.scene_id != have.scene_id)
        return false;
    if ((flags & (kFilterSessionId | kFilterNetworkId)) &&
        std::memcmp(f.network_id.session_id, info.network_id.session_id, sizeof(f.network_id.session_id)) != 0)
        return false;
    return true;
}

LanLink::LanLink(LdnLanConfig config) : config_(std::move(config)) {
    std::random_device device;
    const uint64_t now = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    std::seed_seq seed{device(), device(), static_cast<unsigned>(now), static_cast<unsigned>(now >> 32)};
    random_.seed(seed);
    instance_ = random_() | 1;  // never 0
}

void LanLink::random_bytes(uint8_t* out, size_t size) {
    std::lock_guard lock(random_mutex_);
    for (size_t i = 0; i < size; ++i)
        out[i] = static_cast<uint8_t>(random_() >> 56);
}

LanLink::~LanLink() { close(); }

void LanLink::set_listener(Listener* listener) {
    std::lock_guard lock(listener_mutex_);
    listener_ = listener;
}

bool LanLink::open() {
    std::lock_guard lock(mutex_);
    if (socket_)
        return true;
    if (!config_.lan)
        return false;
    socket_ = config_.lan->open_udp(config_.port);
    if (!socket_) {
        // Port taken (a second instance on this host): others reach us only
        // as a peer address, not by broadcast.
        socket_ = config_.lan->open_udp(0);
        if (socket_)
            rt::log(LogLevel::Warn, "ldn lan: port %u taken, using %u (find this one with --ldn-peer)", config_.port,
                    socket_->port());
    }
    if (!socket_) {
        rt::log(LogLevel::Warn, "ldn lan: no UDP socket, staying offline");
        return false;
    }
    rt::log(LogLevel::Info, "ldn lan: listening on UDP port %u", socket_->port());
    config_.lan->start_discovery();
    stop_ = false;
    thread_ = std::thread([this] { pump(); });
    return true;
}

void LanLink::close() {
    disconnect();
    std::unique_ptr<platform::ILanSocket> socket;
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
        // The pump may wait without a time limit (outside a network): wake
        // it with a datagram to ourselves.
        if (socket_)
            send_packet({0x7F000001u, socket_->port()}, kPing, nullptr, 0);
    }
    if (thread_.joinable())
        thread_.join();
    std::lock_guard lock(mutex_);
    if (socket_) {
        config_.lan->stop_advertising();
        config_.lan->stop_discovery();
    }
    socket = std::move(socket_);
    queues_.clear();
    // known_hosts_ stays: a game may Finalize after its scan and initialize
    // again before it connects.
}

bool LanLink::is_open() const {
    std::lock_guard lock(mutex_);
    return socket_ != nullptr;
}

uint16_t LanLink::port() const {
    std::lock_guard lock(mutex_);
    return socket_ ? socket_->port() : 0;
}

void LanLink::reset_network() {
    if (role_ == Role::Host)
        config_.lan->stop_advertising();
    role_ = Role::None;
    network_ = NetworkInfo{};
    node_id_ = -1;
    host_endpoint_ = {};
    stations_ = {};
    queues_.clear();
}

bool LanLink::host(NetworkInfo* info, bool /*private_network*/) {
    std::lock_guard lock(mutex_);
    if (!socket_)
        return false;
    reset_network();
    char name[sizeof(NodeInfo::user_name)];
    std::memcpy(name, info->nodes[0].user_name, sizeof(name));
    const int16_t version = info->nodes[0].local_communication_version;
    make_node(info, 0, subnet_for_instance(instance_), name, version);
    std::memcpy(info->common.bssid, info->nodes[0].mac_addr, sizeof(info->common.bssid));
    network_ = *info;
    role_ = Role::Host;
    node_id_ = 0;
    config_.lan->advertise(socket_->port());
    const uint32_t ip = info->nodes[0].ip_addr;
    rt::log(LogLevel::Info, "ldn lan: hosting network as %u.%u.%u.%u on UDP port %u", ip >> 24, ip >> 16 & 0xFF,
            ip >> 8 & 0xFF, ip & 0xFF, socket_->port());
    return true;
}

void LanLink::update_host(const NetworkInfo& info) {
    std::lock_guard lock(mutex_);
    if (role_ != Role::Host)
        return;
    NetworkInfo next = info;
    std::memcpy(next.nodes, network_.nodes, sizeof(next.nodes));
    next.node_count = network_.node_count;
    std::memcpy(next.common.bssid, network_.common.bssid, sizeof(next.common.bssid));
    network_ = next;
    broadcast_update();
}

void LanLink::destroy() {
    std::lock_guard lock(mutex_);
    if (role_ != Role::Host)
        return;
    for (const Station& s : stations_)
        if (s.used)
            for (int i = 0; i < 3; ++i)  // UDP: say it more than once
                send_packet(s.endpoint, kDestroyed, nullptr, 0);
    rt::log(LogLevel::Info, "ldn lan: network destroyed");
    reset_network();
}

std::vector<LanEndpoint> LanLink::probe_targets() const {
    std::vector<LanEndpoint> out;
    const auto add = [&out](LanEndpoint e) {
        if (std::find(out.begin(), out.end(), e) == out.end())
            out.push_back(e);
    };
    for (const LanEndpoint& d : config_.lan->discovered())  // Bonjour (Apple)
        add(d);
    for (LanEndpoint p : config_.peers) {
        if (p.port == 0)
            p.port = config_.port;
        add(p);
    }
    const bool broadcast = config_.broadcast && config_.lan->can_broadcast();
    const std::vector<platform::LanInterface> ifs =
        (broadcast || config_.subnet_probe) ? config_.lan->interfaces() : std::vector<platform::LanInterface>{};
    if (broadcast) {
        add({0xFFFFFFFFu, config_.port});
        for (const platform::LanInterface& i : ifs)
            add({i.ip | ~i.mask, config_.port});
    }
    if (config_.subnet_probe) {
        // Unicast to every address of the own /24 (needs no multicast
        // entitlement on iOS, only the local network permission).
        for (const platform::LanInterface& i : ifs) {
            if ((i.mask & 0xFF) != 0)
                continue;  // smaller than a /24: broadcast and peers only
            const uint32_t base = i.ip & 0xFFFFFF00u;
            for (uint32_t h = 1; h < 255; ++h)
                if ((base | h) != i.ip)
                    add({base | h, config_.port});
        }
    }
    return out;
}

std::vector<NetworkInfo> LanLink::scan(const ScanFilter& filter, int16_t channel) {
    std::vector<LanEndpoint> targets;
    {
        std::lock_guard lock(mutex_);
        if (!socket_)
            return {};
        scanning_ = true;
        scan_results_.clear();
        scan_sources_.clear();
        targets = probe_targets();
        ScanRequestBody req{};
        req.local_communication_id =
            (filter.flags & (kFilterLocalCommunicationId | kFilterIntentId | kFilterNetworkId))
                ? filter.network_id.intent_id.local_communication_id
                : 0;
        req.instance = instance_;
        for (const LanEndpoint& t : targets)
            send_packet(t, kScanRequest, &req, sizeof(req));
    }
    {
        // The guest thread only waits for answers: its core goes to the other
        // threads meanwhile (render thread on core 0). `lock` is declared
        // after `idle`, so mutex_ is free before the thread waits for its slot.
        rt::HostWorkScope idle(true);
        std::unique_lock lock(mutex_);
        cv_.wait_for(lock, std::chrono::milliseconds(config_.scan_wait_ms), [this] { return stop_.load(); });
    }
    std::unique_lock lock(mutex_);
    scanning_ = false;
    std::vector<NetworkInfo> found;
    size_t other_channel = 0, filtered = 0;
    for (const NetworkInfo& n : scan_results_) {
        if (!matches(filter, n))
            ++filtered;
        else if (channel != 0 && n.common.channel != 0 && n.common.channel != channel)
            ++other_channel;
        else
            found.push_back(n);
    }
    // Info whenever the outcome changes, so a device log at the default level
    // shows whether discovery works; every scan stays at Debug.
    const uint64_t outcome = static_cast<uint64_t>(targets.size()) << 32 | scan_results_.size() << 16 | found.size();
    const LogLevel level = outcome != last_scan_outcome_ ? LogLevel::Info : LogLevel::Debug;
    last_scan_outcome_ = outcome;
    rt::log(level,
            "ldn lan: scan probed %zu addresses, %zu answer(s), %zu filtered out (flags 0x%x, lcid 0x%llx, scene %u), "
            "%zu on another channel than %d, found %zu network(s)",
            targets.size(), scan_results_.size(), filtered, filter.flags,
            static_cast<unsigned long long>(filter.network_id.intent_id.local_communication_id),
            filter.network_id.intent_id.scene_id, other_channel, channel, found.size());
    for (const NetworkInfo& n : scan_results_)
        rt::log(level, "ldn lan:   answer: lcid 0x%llx scene %u channel %d session %02x%02x%02x%02x... nodes %u/%d",
                static_cast<unsigned long long>(n.network_id.intent_id.local_communication_id),
                n.network_id.intent_id.scene_id, n.common.channel, n.network_id.session_id[0],
                n.network_id.session_id[1], n.network_id.session_id[2], n.network_id.session_id[3], n.node_count,
                n.node_count_max);
    return found;
}

rt::Result LanLink::connect(const uint8_t (&session_id)[0x10], const uint8_t* /*server_random*/,
                            const UserConfig& user, int16_t version, NetworkInfo* out) {
    std::unique_lock lock(mutex_);
    if (!socket_)
        return ldn_result::kNetworkNotFound;
    const auto it = known_hosts_.find(session_key(session_id));
    if (it == known_hosts_.end()) {
        rt::log(LogLevel::Warn, "ldn lan: connect to a network no scan has seen");
        return ldn_result::kNetworkNotFound;
    }
    reset_network();
    connect_target_ = it->second;
    connecting_ = true;
    pending_ = Pending{};
    ConnectBody body{};
    std::memcpy(body.session_id, session_id, sizeof(body.session_id));
    body.user = user;
    body.version = version;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(config_.connect_timeout_ms);
    {
        rt::HostWorkScope idle(true);  // waits for the host only, as in scan()
        while (!pending_.done && !stop_ && std::chrono::steady_clock::now() < deadline) {
            send_packet(connect_target_, kConnectRequest, &body, sizeof(body));  // again until answered
            cv_.wait_for(lock, std::chrono::milliseconds(250), [this] { return pending_.done || stop_.load(); });
        }
        lock.unlock();  // back to the slot without mutex_
    }
    lock.lock();
    connecting_ = false;
    if (!pending_.done) {
        rt::log(LogLevel::Warn, "ldn lan: no answer from %s", platform::lan_endpoint_string(connect_target_).c_str());
        return ldn_result::kNetworkNotFound;
    }
    if (pending_.result.failed())
        return pending_.result;
    role_ = Role::Station;
    host_endpoint_ = connect_target_;
    host_last_heard_ = Clock::now();
    network_ = pending_.info;
    node_id_ = pending_.node_id;
    *out = network_;
    const uint32_t ip = network_.nodes[node_id_].ip_addr;
    rt::log(LogLevel::Info, "ldn lan: joined %s as node %d (%u.%u.%u.%u)",
            platform::lan_endpoint_string(host_endpoint_).c_str(), node_id_, ip >> 24, ip >> 16 & 0xFF,
            ip >> 8 & 0xFF, ip & 0xFF);
    return rt::kSuccess;
}

void LanLink::disconnect() {
    std::lock_guard lock(mutex_);
    if (role_ == Role::Station && socket_) {
        for (int i = 0; i < 3; ++i)
            send_packet(host_endpoint_, kLeave, nullptr, 0);
        rt::log(LogLevel::Info, "ldn lan: left the network");
        reset_network();
    } else if (role_ == Role::Host && socket_) {
        for (const Station& s : stations_)
            if (s.used)
                for (int i = 0; i < 3; ++i)
                    send_packet(s.endpoint, kDestroyed, nullptr, 0);
        reset_network();
    }
}

bool LanLink::in_network() const {
    std::lock_guard lock(mutex_);
    return role_ != Role::None;
}

uint32_t LanLink::ip() const {
    std::lock_guard lock(mutex_);
    return role_ == Role::None ? 0 : network_.nodes[node_id_].ip_addr;
}

bool LanLink::routes(uint32_t ip) const {
    std::lock_guard lock(mutex_);
    if (role_ == Role::None)
        return false;
    return ip == 0xFFFFFFFFu || (ip & kMask) == subnet_of(network_);
}

void LanLink::send_packet(const LanEndpoint& to, uint8_t type, const void* body, size_t size) {
    std::vector<uint8_t> p(sizeof(Header) + size);
    Header h{kMagic, kVersion, type, {}};
    std::memcpy(p.data(), &h, sizeof(h));
    if (size != 0)
        std::memcpy(p.data() + sizeof(h), body, size);
    socket_->send_to(to, p.data(), p.size());
}

void LanLink::enqueue(uint16_t port, uint32_t src_ip, uint16_t src_port, const uint8_t* data, size_t size) {
    std::deque<Datagram>& q = queues_[port];
    if (q.size() >= kMaxQueued)
        q.pop_front();
    Datagram d;
    d.src_ip = src_ip;
    d.src_port = src_port;
    d.data.assign(data, data + size);
    q.push_back(std::move(d));
}

void LanLink::forward(uint32_t src_ip, uint16_t src_port, uint32_t dst_ip, uint16_t dst_port, const uint8_t* data,
                      size_t size, const LanEndpoint* except) {
    std::vector<uint8_t> body(sizeof(DataHeader) + size);
    const DataHeader dh{src_ip, dst_ip, src_port, dst_port};
    std::memcpy(body.data(), &dh, sizeof(dh));
    if (size != 0)
        std::memcpy(body.data() + sizeof(dh), data, size);
    const bool broadcast = dst_ip == 0xFFFFFFFFu || (dst_ip & 0xFF) == 0xFF;
    if (role_ == Role::Station) {
        send_packet(host_endpoint_, kData, body.data(), body.size());
        return;
    }
    for (size_t i = 1; i < stations_.size(); ++i) {
        const Station& s = stations_[i];
        if (!s.used || (except != nullptr && s.endpoint == *except))
            continue;
        if (broadcast || network_.nodes[i].ip_addr == dst_ip)
            send_packet(s.endpoint, kData, body.data(), body.size());
    }
}

bool LanLink::send(uint16_t src_port, uint32_t dst_ip, uint16_t dst_port, const void* data, size_t size) {
    std::lock_guard lock(mutex_);
    if (role_ == Role::None || !socket_)
        return false;
    const uint32_t own = network_.nodes[node_id_].ip_addr;
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    // A broadcast reaches the sender too, as RyuLDN delivers it (concept
    // only; whether Pia needs or ignores its own copy is not checked).
    const bool broadcast = dst_ip == 0xFFFFFFFFu || (dst_ip & 0xFF) == 0xFF;
    if (dst_ip == own || broadcast)
        enqueue(dst_port, own, src_port, bytes, size);
    if (dst_ip == own)
        return true;
    ++datagrams_out_;
    forward(own, src_port, dst_ip, dst_port, bytes, size, nullptr);
    return true;
}

bool LanLink::pop(uint16_t port, Datagram* out) {
    std::lock_guard lock(mutex_);
    const auto it = queues_.find(port);
    if (it == queues_.end() || it->second.empty())
        return false;
    *out = std::move(it->second.front());
    it->second.pop_front();
    return true;
}

int LanLink::peek(uint16_t port) const {
    std::lock_guard lock(mutex_);
    const auto it = queues_.find(port);
    if (it == queues_.end() || it->second.empty())
        return -1;
    return static_cast<int>(it->second.front().data.size());
}

void LanLink::broadcast_update() {
    for (const Station& s : stations_)
        if (s.used)
            send_packet(s.endpoint, kNetworkUpdate, &network_, sizeof(network_));
}

void LanLink::pump() {
    std::vector<uint8_t> buf(kMaxPacket);
    while (!stop_) {
        LanEndpoint from;
        // Outside a network nothing is due, so the pump sleeps until a
        // datagram comes (no periodic wake-ups while the game only has ldn
        // initialized); inside one it wakes for the keepalive.
        int timeout_ms = -1;
        {
            std::lock_guard lock(mutex_);
            if (role_ != Role::None)
                timeout_ms = static_cast<int>(std::max<uint32_t>(1, std::min(config_.keepalive_ms, config_.peer_timeout_ms)));
        }
        const int n = socket_->recv_from(buf.data(), buf.size(), &from, timeout_ms);
        Notice notice;
        {
            std::lock_guard lock(mutex_);
            if (n >= static_cast<int>(sizeof(Header)))
                handle(buf.data(), static_cast<size_t>(n), from, &notice);
            keepalive(&notice);
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

void LanLink::handle(const uint8_t* data, size_t size, const LanEndpoint& from, Notice* ev) {
    Header h;
    std::memcpy(&h, data, sizeof(h));
    if (h.magic != kMagic || h.version != kVersion)
        return;
    const uint8_t* body = data + sizeof(h);
    const size_t body_size = size - sizeof(h);
    // Any packet from a peer of our network shows it is still there.
    if (role_ == Role::Station && from == host_endpoint_) {
        host_last_heard_ = Clock::now();
    } else if (role_ == Role::Host) {
        for (Station& s : stations_)
            if (s.used && s.endpoint == from)
                s.last_heard = Clock::now();
    }
    switch (h.type) {
    case kScanRequest: {
        if (role_ != Role::Host || body_size < sizeof(ScanRequestBody))
            return;
        ScanRequestBody req;
        std::memcpy(&req, body, sizeof(req));
        if (req.instance == instance_)
            return;  // our own probe (broadcast, /24 or a peer address that is us)
        const int64_t lcid = req.local_communication_id;
        if (lcid == 0 || lcid == network_.network_id.intent_id.local_communication_id) {
            std::vector<uint8_t> resp(sizeof(uint64_t) + sizeof(network_));
            std::memcpy(resp.data(), &instance_, sizeof(uint64_t));
            std::memcpy(resp.data() + sizeof(uint64_t), &network_, sizeof(network_));
            send_packet(from, kScanResponse, resp.data(), resp.size());
        }
        return;
    }
    case kScanResponse: {
        if (body_size < sizeof(uint64_t) + sizeof(NetworkInfo))
            return;
        uint64_t instance = 0;
        std::memcpy(&instance, body, sizeof(instance));
        if (instance == instance_)
            return;  // our own network: the session id cannot tell, two hosts may share it
        NetworkInfo info;
        std::memcpy(&info, body + sizeof(uint64_t), sizeof(info));
        known_hosts_[session_key(info.network_id.session_id)] = from;
        if (!scanning_)
            return;
        for (size_t i = 0; i < scan_sources_.size(); ++i)
            if (scan_sources_[i] == from) {  // the same host answered again (several probes reached it)
                scan_results_[i] = info;
                return;
            }
        scan_results_.push_back(info);
        scan_sources_.push_back(from);
        return;
    }
    case kConnectRequest: {
        if (role_ != Role::Host || body_size < sizeof(ConnectBody))
            return;
        ConnectBody req;
        std::memcpy(&req, body, sizeof(req));
        for (size_t i = 1; i < stations_.size(); ++i)
            if (stations_[i].used && stations_[i].endpoint == from) {  // a resend: answer again
                AcceptBody acc{};
                acc.node_id = static_cast<int8_t>(i);
                acc.info = network_;
                send_packet(from, kConnectAccept, &acc, sizeof(acc));
                return;
            }
        const auto reject = [&](const char* why) {
            rt::log(LogLevel::Info, "ldn lan: rejected %s: %s", platform::lan_endpoint_string(from).c_str(), why);
            const uint32_t raw = ldn_result::kNetworkNotFound.raw;
            send_packet(from, kConnectReject, &raw, sizeof(raw));
        };
        if (std::memcmp(req.session_id, network_.network_id.session_id, sizeof(req.session_id)) != 0)
            return reject("other session");
        if (network_.station_accept_policy == 1)  // AlwaysReject
            return reject("the game accepts no stations");
        if (req.version != network_.nodes[0].local_communication_version)
            return reject("other local communication version");
        int8_t id = -1;
        for (int8_t i = 1; i < network_.node_count_max && i < 8; ++i)
            if (!stations_[static_cast<size_t>(i)].used) {
                id = i;
                break;
            }
        if (id < 0)
            return reject("network full");
        req.user.user_name[sizeof(req.user.user_name) - 1] = 0;
        make_node(&network_, id, subnet_of(network_), req.user.user_name, req.version);
        network_.node_count = static_cast<uint8_t>(network_.node_count + 1);
        stations_[static_cast<size_t>(id)] = {from, true, Clock::now()};
        AcceptBody acc{};
        acc.node_id = id;
        acc.info = network_;
        send_packet(from, kConnectAccept, &acc, sizeof(acc));
        broadcast_update();
        rt::log(LogLevel::Info, "ldn lan: %s joined as node %d (\"%s\")", platform::lan_endpoint_string(from).c_str(),
                id, req.user.user_name);
        ev->changed = true;
        ev->info = network_;
        return;
    }
    case kConnectAccept: {
        if (!connecting_ || from != connect_target_ || body_size < sizeof(AcceptBody))
            return;
        AcceptBody acc;
        std::memcpy(&acc, body, sizeof(acc));
        if (acc.node_id < 1 || acc.node_id >= 8)
            return;
        pending_.done = true;
        pending_.result = rt::kSuccess;
        pending_.info = acc.info;
        pending_.node_id = acc.node_id;
        return;
    }
    case kConnectReject: {
        if (!connecting_ || from != connect_target_ || body_size < sizeof(uint32_t))
            return;
        uint32_t raw = 0;
        std::memcpy(&raw, body, sizeof(raw));
        pending_.done = true;
        pending_.result = rt::Result{raw};
        return;
    }
    case kNetworkUpdate: {
        if (role_ != Role::Station || from != host_endpoint_ || body_size < sizeof(NetworkInfo))
            return;
        std::memcpy(&network_, body, sizeof(network_));
        ev->changed = true;
        ev->info = network_;
        return;
    }
    case kLeave: {
        if (role_ != Role::Host)
            return;
        for (size_t i = 1; i < stations_.size(); ++i)
            if (stations_[i].used && stations_[i].endpoint == from) {
                drop_station(i);
                rt::log(LogLevel::Info, "ldn lan: node %zu left", i);
                ev->changed = true;
                ev->info = network_;
            }
        return;
    }
    case kDestroyed: {
        if (role_ != Role::Station || from != host_endpoint_)
            return;
        rt::log(LogLevel::Info, "ldn lan: the access point destroyed the network");
        reset_network();
        ev->lost = true;
        return;
    }
    case kData: {
        if (role_ == Role::None || body_size < sizeof(DataHeader))
            return;
        DataHeader dh;
        std::memcpy(&dh, body, sizeof(dh));
        const uint8_t* payload = body + sizeof(dh);
        const size_t payload_size = body_size - sizeof(dh);
        if (role_ == Role::Station && from != host_endpoint_)
            return;
        if (role_ == Role::Host) {
            bool known = false;
            for (const Station& s : stations_)
                known = known || (s.used && s.endpoint == from);
            if (!known)
                return;
        }
        const uint32_t own = network_.nodes[node_id_].ip_addr;
        const bool broadcast = dh.dst_ip == 0xFFFFFFFFu || (dh.dst_ip & 0xFF) == 0xFF;
        if (broadcast || dh.dst_ip == own) {
            enqueue(dh.dst_port, dh.src_ip, dh.src_port, payload, payload_size);
            ++datagrams_in_;
        }
        if (role_ == Role::Host && dh.dst_ip != own)
            forward(dh.src_ip, dh.src_port, dh.dst_ip, dh.dst_port, payload, payload_size, &from);
        return;
    }
    case kPing:  // only keeps the peer alive (above)
    default:
        return;
    }
}

void LanLink::drop_station(size_t i) {
    stations_[i] = {};
    network_.nodes[i] = NodeInfo{};
    network_.nodes[i].node_id = static_cast<int8_t>(i);  // a free slot keeps its index
    network_.node_count = static_cast<uint8_t>(network_.node_count - 1);
    broadcast_update();
}

void LanLink::keepalive(Notice* ev) {
    if (role_ == Role::None)
        return;
    const Clock::time_point now = Clock::now();
    const auto timeout = std::chrono::milliseconds(config_.peer_timeout_ms);
    if (role_ == Role::Station) {
        if (now - host_last_heard_ > timeout) {
            rt::log(LogLevel::Warn, "ldn lan: no word from the access point %s for %u ms, network lost",
                    platform::lan_endpoint_string(host_endpoint_).c_str(), config_.peer_timeout_ms);
            reset_network();
            ev->lost = true;
            ev->reason = kDisconnectSignalLost;
            return;
        }
    } else {
        for (size_t i = 1; i < stations_.size(); ++i)
            if (stations_[i].used && now - stations_[i].last_heard > timeout) {
                rt::log(LogLevel::Warn, "ldn lan: node %zu (%s) silent for %u ms, dropped", i,
                        platform::lan_endpoint_string(stations_[i].endpoint).c_str(), config_.peer_timeout_ms);
                drop_station(i);
                ev->changed = true;
                ev->info = network_;
            }
    }
    if (datagrams_out_ + datagrams_in_ != datagrams_logged_ && now - last_traffic_log_ >= std::chrono::seconds(5)) {
        rt::log(LogLevel::Info, "ldn lan: game datagrams so far: %llu sent, %llu received",
                static_cast<unsigned long long>(datagrams_out_), static_cast<unsigned long long>(datagrams_in_));
        datagrams_logged_ = datagrams_out_ + datagrams_in_;
        last_traffic_log_ = now;
    }
    if (now - last_ping_ < std::chrono::milliseconds(config_.keepalive_ms))
        return;
    last_ping_ = now;
    if (role_ == Role::Station) {
        send_packet(host_endpoint_, kPing, nullptr, 0);
    } else {
        for (const Station& s : stations_)
            if (s.used)
                send_packet(s.endpoint, kPing, nullptr, 0);
    }
}

}  // namespace lgp::hle::ldn
