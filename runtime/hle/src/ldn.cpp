// ldn:u (local wireless communication). Offline (register_ldn) the console
// is alone: the state machine works (initialize, open an access point, create
// a network, open a station, scan), but no other console is ever in range.
// Scans find nothing, a created network never gets a station, and connecting
// fails with ResultNetworkNotFound. With a LanLink (register_local_network,
// docs/analysis/44-multiplayer-stand.md) scans, networks and connects reach
// other instances of this runtime in the same LAN. Command ids, the State machine and the struct
// layouts from switchbrew "LDN services" and libnx ldn.h/ldn.c (interface
// documentation only); layouts are checked with static_assert.
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <initializer_list>
#include <mutex>
#include <optional>
#include <vector>

#include "ldn_lan.hpp"
#include "lgp/hle/services.hpp"
#include "lgp/runtime/log.hpp"

namespace lgp::hle {
namespace {

using rt::LogLevel;

// nn::ldn::State
enum class State : uint32_t {
    None = 0,
    Initialized = 1,
    AccessPoint = 2,
    AccessPointCreated = 3,
    Station = 4,
    StationConnected = 5,
};

using namespace ldn;

constexpr size_t kAdvertiseDataMax = sizeof(NetworkInfo::advertise_data);
constexpr size_t kNodeHistoryCount = 8;

// Our node: link-local address as on hardware (169.254.x.1), a locally
// administered MAC. Values are this runtime's choice.
constexpr uint32_t kHostIp = 0xA9FE0101;  // 169.254.1.1, host byte order as ldn keeps it
constexpr uint32_t kSubnetMask = 0xFFFFFF00;
constexpr uint8_t kHostMac[6] = {0x02, 0x4C, 0x47, 0x50, 0x00, 0x01};

class LocalCommunication final : public Service, public Link::Listener {
public:
    explicit LocalCommunication(std::shared_ptr<Link> link)
        : Service("ldn:IUserLocalCommunicationService"), link_(std::move(link)) {
        if (link_)
            link_->set_listener(this);
        on(0, [this](Request&, Response& resp) {  // GetState
            std::lock_guard lock(mutex_);
            resp.push<uint32_t>(static_cast<uint32_t>(state_));
        });
        on(1, [this](Request& req, Response& resp) {  // GetNetworkInfo(out 0x1A NetworkInfo)
            std::lock_guard lock(mutex_);
            if (!has_network())
                resp.result = ldn_result::kInvalidState;
            else if (!req.write_out(0, &network_, sizeof(network_)))
                resp.result = rt::kernel_result::kInvalidPointer;
        });
        on(2, [this](Request&, Response& resp) {  // GetIpv4Address -> address, mask
            std::lock_guard lock(mutex_);
            if (!has_network()) {
                resp.result = ldn_result::kInvalidState;
                return;
            }
            const uint32_t ip = link_ && link_->in_network() ? link_->ip() : kHostIp;
            resp.push<uint32_t>(ip);
            resp.push<uint32_t>(link_ && link_->in_network() ? link_->mask() : kSubnetMask);
        });
        on(3, [this](Request&, Response& resp) {  // GetDisconnectReason -> s16
            std::lock_guard lock(mutex_);
            resp.push<int16_t>(disconnect_reason_);
        });
        on(4, [this](Request&, Response& resp) {  // GetSecurityParameter
            std::lock_guard lock(mutex_);
            if (!has_network()) {
                resp.result = ldn_result::kInvalidState;
                return;
            }
            SecurityParameter p{};
            std::memcpy(p.server_random, network_.server_random, sizeof(p.server_random));
            std::memcpy(p.session_id, network_.network_id.session_id, sizeof(p.session_id));
            resp.push(p);
        });
        on(5, [this](Request&, Response& resp) {  // GetNetworkConfig
            std::lock_guard lock(mutex_);
            if (!has_network()) {
                resp.result = ldn_result::kInvalidState;
                return;
            }
            resp.push(config_);
        });
        on(100, [this](Request& req, Response& resp) {  // GetStateChangeEvent
            rt::Handle h = rt::kInvalidHandle;
            resp.result = state_changed_.make_handle(req.kernel(), &h);
            if (resp.result.ok())
                resp.copy_handles.push_back(h);
        });
        // GetNetworkInfoAndHistory(out 0x1A NetworkInfo, out 0xA NodeLatestUpdate[8]):
        // nobody ever joins or leaves, so the history is all zero.
        on(101, [this](Request& req, Response& resp) {
            std::lock_guard lock(mutex_);
            if (!has_network()) {
                resp.result = ldn_result::kInvalidState;
                return;
            }
            const std::array<uint8_t, 8 * kNodeHistoryCount> history{};
            if (!req.write_out(0, &network_, sizeof(network_)) ||
                !req.write_out(1, history.data(), std::min<uint64_t>(history.size(), req.out_buffer(1).size)))
                resp.result = rt::kernel_result::kInvalidPointer;
        });
        // Scan / ScanPrivate(s16 channel, ScanFilter, out NetworkInfo[]) -> s16 count
        // Offline nobody is in range; over LAN the scan asks the other instances.
        const auto scan = [this](Request& req, Response& resp) {
            State state;
            {
                std::lock_guard lock(mutex_);
                state = state_;
                if (state_ == State::None) {
                    resp.result = ldn_result::kInvalidState;
                    return;
                }
            }
            const auto channel = req.pop<int16_t>();
            rt::log(LogLevel::Debug, "ldn: Scan(channel %d) in state %u", channel, static_cast<uint32_t>(state));
            if (!link_ || !link_->is_open()) {
                resp.push<int16_t>(0);
                return;
            }
            auto filter = req.pop<ScanFilter>();  // after 6 bytes of padding (u64 alignment)
            // -1 is "our own LocalCommunicationId" here as in CreateNetwork
            // (resolved there to the program id, assumed): compare like for like.
            if (filter.network_id.intent_id.local_communication_id == -1)
                filter.network_id.intent_id.local_communication_id =
                    static_cast<int64_t>(req.kernel().config().program_id);
            // Not under mutex_: the scan waits for answers, and the pump
            // thread may need the listener meanwhile.
            const std::vector<NetworkInfo> found = link_->scan(filter, channel);
            const size_t room = req.out_buffer(0).size / sizeof(NetworkInfo);
            const size_t count = std::min(found.size(), room);
            if (count != 0 && !req.write_out(0, found.data(), count * sizeof(NetworkInfo))) {
                resp.result = rt::kernel_result::kInvalidPointer;
                return;
            }
            resp.push<int16_t>(static_cast<int16_t>(count));
        };
        on(102, scan);
        on(103, scan);
        on(104, [](Request&, Response&) {});  // SetWirelessControllerPolicy(u32)

        on(200, [this](Request& req, Response& resp) {  // OpenAccessPoint
            resp.result = transition(req.kernel(), {State::Initialized}, State::AccessPoint);
        });
        on(201, [this](Request& req, Response& resp) {  // CloseAccessPoint
            resp.result = transition(req.kernel(), {State::AccessPoint, State::AccessPointCreated}, State::Initialized);
            if (resp.result.ok() && link_)
                link_->destroy();
        });
        on(202, [this](Request& req, Response& resp) {  // CreateNetwork(SecurityConfig, UserConfig, pad, NetworkConfig)
            const auto sec = req.pop<SecurityConfig>();
            const auto user = req.pop<UserConfig>();
            req.pop<uint32_t>();
            const auto config = req.pop<NetworkConfig>();
            resp.result = create_network(req, sec, nullptr, user, config);
        });
        // CreateNetworkPrivate(SecurityConfig, SecurityParameter, UserConfig, pad,
        // NetworkConfig, X AddressEntry[])
        on(203, [this](Request& req, Response& resp) {
            const auto sec = req.pop<SecurityConfig>();
            const auto param = req.pop<SecurityParameter>();
            const auto user = req.pop<UserConfig>();
            req.pop<uint32_t>();
            const auto config = req.pop<NetworkConfig>();
            resp.result = create_network(req, sec, &param, user, config);
        });
        on(204, [this](Request& req, Response& resp) {  // DestroyNetwork
            resp.result = transition(req.kernel(), {State::AccessPointCreated}, State::AccessPoint);
            if (resp.result.ok()) {
                if (link_)
                    link_->destroy();
                std::lock_guard lock(mutex_);
                disconnect_reason_ = kDisconnectDestroyedByUser;
            }
        });
        on(205, [](Request&, Response&) {});  // Reject(Ipv4Address): there is nobody to reject
        on(206, [this](Request& req, Response& resp) {  // SetAdvertiseData(in buffer)
            std::vector<uint8_t> data;
            if (!req.read_in(0, &data, kAdvertiseDataMax + 1)) {
                resp.result = rt::kernel_result::kInvalidPointer;
                return;
            }
            if (data.size() > kAdvertiseDataMax) {
                rt::log(LogLevel::Warn, "ldn: advertise data of %zu bytes, cut to 0x180", data.size());
                data.resize(kAdvertiseDataMax);
            }
            std::lock_guard lock(mutex_);
            if (data != advertise_) {
                // Only size and checksum: enough to see whether two instances
                // advertise the same (a game may pick roles from it).
                uint32_t crc = 0xFFFFFFFFu;
                for (const uint8_t b : data) {
                    crc ^= b;
                    for (int k = 0; k < 8; ++k)
                        crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
                }
                rt::log(LogLevel::Debug, "ldn: SetAdvertiseData %zu bytes, crc32 %08x", data.size(), ~crc);
            }
            advertise_ = std::move(data);
            apply_advertise_data();
            if (link_ && state_ == State::AccessPointCreated)
                link_->update_host(network_);
        });
        on(207, [this](Request& req, Response&) {  // SetStationAcceptPolicy(u8)
            std::lock_guard lock(mutex_);
            accept_policy_ = req.pop<uint8_t>();
            network_.station_accept_policy = accept_policy_;
            if (link_ && state_ == State::AccessPointCreated)
                link_->update_host(network_);
        });
        on(208, [](Request&, Response&) {});  // AddAcceptFilterEntry(MacAddress)
        on(209, [](Request&, Response&) {});  // ClearAcceptFilter

        on(300, [this](Request& req, Response& resp) {  // OpenStation
            resp.result = transition(req.kernel(), {State::Initialized}, State::Station);
        });
        on(301, [this](Request& req, Response& resp) {  // CloseStation
            resp.result = transition(req.kernel(), {State::Station, State::StationConnected}, State::Initialized);
            if (resp.result.ok() && link_)
                link_->disconnect();
        });
        // Connect(SecurityConfig, UserConfig, s32 version, u32 option, X NetworkInfo),
        // ConnectPrivate(SecurityConfig, SecurityParameter, UserConfig, s32
        // version, u32 option, NetworkConfig): offline no network is in range.
        on(302, [this](Request& req, Response& resp) {
            req.pop<SecurityConfig>();
            const auto user = req.pop<UserConfig>();
            const auto version = req.pop<int32_t>();
            NetworkInfo target{};
            if (link_ && !req.read_in_as(0, &target)) {
                resp.result = rt::kernel_result::kInvalidPointer;
                return;
            }
            resp.result = connect(req, target.network_id.session_id, target.server_random, user, version);
        });
        on(303, [this](Request& req, Response& resp) {
            req.pop<SecurityConfig>();
            const auto param = req.pop<SecurityParameter>();
            const auto user = req.pop<UserConfig>();
            const auto version = req.pop<int32_t>();
            resp.result = connect(req, param.session_id, param.server_random, user, version);
        });
        on(304, [this](Request& req, Response& resp) {  // Disconnect
            resp.result = transition(req.kernel(), {State::StationConnected}, State::Station);
            if (resp.result.ok() && link_)
                link_->disconnect();
        });

        // Initialize(pid, u64), InitializeWithVersion(s32 version, pid, u64)
        const auto initialize = [this](Request& req, Response& resp) {
            resp.result = transition(req.kernel(), {State::None}, State::Initialized);
            if (resp.result.ok() && link_)
                link_->open();
        };
        on(400, initialize);
        on(402, initialize);
        on(401, [this](Request& req, Response&) {  // Finalize: closes whatever is open
            if (link_)
                link_->close();
            std::lock_guard lock(mutex_);
            set_state(req.kernel(), State::None);
        });
    }

    ~LocalCommunication() override {
        if (link_)
            link_->set_listener(nullptr);
    }

    // Link::Listener (receive thread, no link lock held).
    void network_changed(const NetworkInfo& info) override {
        std::lock_guard lock(mutex_);
        if (creating_) {  // CreateNetwork between Link::host and AccessPointCreated
            created_update_ = info;
            return;
        }
        if (!has_network() || kernel_ == nullptr)
            return;
        network_ = info;
        state_changed_.signal(*kernel_);
    }
    void network_lost(int16_t reason) override {
        std::lock_guard lock(mutex_);
        if (state_ != State::StationConnected || kernel_ == nullptr)
            return;
        disconnect_reason_ = reason;
        set_state(*kernel_, State::Station);
    }

private:
    bool has_network() const { return state_ == State::AccessPointCreated || state_ == State::StationConnected; }

    rt::Result connect(Request& req, const uint8_t (&session_id)[0x10], const uint8_t* server_random,
                       const UserConfig& user, int32_t version) {
        {
            std::lock_guard lock(mutex_);
            if (state_ != State::Station)
                return ldn_result::kInvalidState;
            if (!link_ || !link_->is_open())
                return ldn_result::kNetworkNotFound;
        }
        NetworkInfo joined{};
        // Not under mutex_: connect waits for the access point's answer.
        const rt::Result r = link_->connect(session_id, server_random, user, static_cast<int16_t>(version), &joined);
        if (r.failed())
            return r;
        std::lock_guard lock(mutex_);
        network_ = joined;
        std::memcpy(&config_.intent_id, &joined.network_id.intent_id, sizeof(config_.intent_id));
        config_.node_count_max = joined.node_count_max;
        config_.local_communication_version = static_cast<int16_t>(version);
        state_ = State::StationConnected;  // set_state would clear network_ on the way
        state_changed_.signal(req.kernel());
        return rt::kSuccess;
    }

    void set_state(rt::Kernel& kernel, State s) {
        if (s == state_)
            return;
        if (s == State::Initialized || s == State::None) {
            network_ = NetworkInfo{};
            config_ = NetworkConfig{};
        }
        state_ = s;
        state_changed_.signal(kernel);
    }

    rt::Result transition(rt::Kernel& kernel, std::initializer_list<State> from, State to) {
        std::lock_guard lock(mutex_);
        kernel_ = &kernel;
        if (std::find(from.begin(), from.end(), state_) == from.end()) {
            rt::log(LogLevel::Warn, "ldn: state %u does not allow going to %u", static_cast<uint32_t>(state_),
                    static_cast<uint32_t>(to));
            return ldn_result::kInvalidState;
        }
        rt::log(LogLevel::Debug, "ldn: state %u -> %u", static_cast<uint32_t>(state_), static_cast<uint32_t>(to));
        if (to == State::AccessPoint || to == State::Station)
            disconnect_reason_ = kDisconnectNone;
        set_state(kernel, to);
        return rt::kSuccess;
    }

    rt::Result create_network(Request& req, const SecurityConfig& sec, const SecurityParameter* param,
                              const UserConfig& user, const NetworkConfig& config) {
        std::unique_lock lock(mutex_);
        if (state_ != State::AccessPoint)
            return ldn_result::kInvalidState;
        if (config.node_count_max < 1 || config.node_count_max > 8 || config.local_communication_version < 0)
            return ldn_result::kInvalidInput;
        config_ = config;
        network_ = NetworkInfo{};
        network_.network_id.intent_id = config.intent_id;
        // -1 means "the first LocalCommunicationId of the NACP". The NACP is not
        // parsed; its first id is normally the program id (assumed).
        if (config.intent_id.local_communication_id == -1)
            network_.network_id.intent_id.local_communication_id =
                static_cast<int64_t>(req.kernel().config().program_id);
        config_.intent_id = network_.network_id.intent_id;
        ++sessions_;
        for (size_t i = 0; i < 0x10; ++i) {
            // Session id and server random: unique per network, deterministic per run.
            network_.network_id.session_id[i] = static_cast<uint8_t>(0x4C + 7 * i + 31 * sessions_);
            network_.server_random[i] = static_cast<uint8_t>(0x50 + 13 * i + 17 * sessions_);
        }
        if (link_ && link_->is_open()) {  // over LAN: unique among all instances
            link_->random_bytes(network_.network_id.session_id, sizeof(network_.network_id.session_id));
            link_->random_bytes(network_.server_random, sizeof(network_.server_random));
        }
        if (param) {
            std::memcpy(network_.network_id.session_id, param->session_id, sizeof(param->session_id));
            std::memcpy(network_.server_random, param->server_random, sizeof(param->server_random));
        }
        std::memcpy(network_.common.bssid, kHostMac, sizeof(kHostMac));
        // SSID: the session id as 32 hex digits (the real derivation is not documented).
        static const char kHex[] = "0123456789abcdef";
        for (size_t i = 0; i < 0x10; ++i) {
            network_.common.ssid.str[2 * i] = kHex[network_.network_id.session_id[i] >> 4];
            network_.common.ssid.str[2 * i + 1] = kHex[network_.network_id.session_id[i] & 0xF];
        }
        network_.common.ssid.len = 0x20;
        network_.common.channel = config.channel;  // 0 = the system picks; left 0 here
        network_.common.link_level = 3;            // best signal, we are the access point
        // 2 = ldn, as a console reports (and ldn_mitm sets); 0 never matches a
        // scan filter on the network type and a game may check it on its own.
        network_.common.network_type = 2;
        // 0 (any) is not what a console reports: there the mode used is 1-3,
        // and a retail network says 1 (Switchbrew; pokeldn sets 1).
        network_.security_mode = sec.security_mode != 0 ? sec.security_mode : 1;
        network_.station_accept_policy = accept_policy_;
        network_.node_count_max = config.node_count_max;
        network_.node_count = 1;
        for (int8_t i = 1; i < 8; ++i)
            network_.nodes[i].node_id = i;  // free slots carry their index, as a console's do
        NodeInfo& self = network_.nodes[0];
        self.ip_addr = kHostIp;
        std::memcpy(self.mac_addr, kHostMac, sizeof(kHostMac));
        self.node_id = 0;
        self.is_connected = 1;
        std::memcpy(self.user_name, user.user_name, sizeof(self.user_name) - 1);
        self.local_communication_version = config.local_communication_version;
        apply_advertise_data();
        rt::log(LogLevel::Info,
                "ldn: CreateNetwork%s: lcid 0x%llx scene %u channel %d version %d security %u session %02x%02x%02x%02x...",
                param ? "Private" : "", static_cast<unsigned long long>(network_.network_id.intent_id.local_communication_id),
                network_.network_id.intent_id.scene_id, config.channel, config.local_communication_version,
                sec.security_mode,
                network_.network_id.session_id[0], network_.network_id.session_id[1],
                network_.network_id.session_id[2], network_.network_id.session_id[3]);
        // Not under mutex_ while the link opens the room: RyuLink waits for the
        // server there with the guest thread's core slot given up, and another
        // guest thread of that core may take mutex_ meanwhile (GetState) and
        // keep the slot until it gets it.
        NetworkInfo net = network_;
        creating_ = true;
        created_update_.reset();
        lock.unlock();
        const bool open = link_ && link_->host(&net, param != nullptr);
        lock.lock();
        creating_ = false;
        if (state_ != State::AccessPoint) {  // closed meanwhile
            if (open)
                link_->destroy();
            return ldn_result::kInvalidState;
        }
        // A station that joined meanwhile came as network_changed (kept for now).
        network_ = open && created_update_ ? *created_update_ : net;
        created_update_.reset();
        apply_advertise_data();  // SetAdvertiseData/SetStationAcceptPolicy may have come meanwhile
        network_.station_accept_policy = accept_policy_;
        if (open) {
            link_->update_host(network_);
            rt::log(LogLevel::Info, "ldn: network created (up to %d nodes), open to the LAN", config.node_count_max);
        } else {
            rt::log(LogLevel::Info, "ldn: network created (up to %d nodes); no other console will join",
                    config.node_count_max);
        }
        set_state(req.kernel(), State::AccessPointCreated);
        return rt::kSuccess;
    }

    void apply_advertise_data() {
        network_.advertise_data_size = static_cast<uint16_t>(advertise_.size());
        std::memset(network_.advertise_data, 0, sizeof(network_.advertise_data));
        if (!advertise_.empty())
            std::memcpy(network_.advertise_data, advertise_.data(), advertise_.size());
    }

    mutable std::mutex mutex_;
    State state_ = State::None;
    NetworkInfo network_{};
    NetworkConfig config_{};
    std::vector<uint8_t> advertise_;
    uint8_t accept_policy_ = 0;
    int16_t disconnect_reason_ = kDisconnectNone;
    uint32_t sessions_ = 0;
    bool creating_ = false;                       // CreateNetwork waits in Link::host
    std::optional<NetworkInfo> created_update_;  // network_changed meanwhile
    Event state_changed_;
    std::shared_ptr<Link> link_;
    rt::Kernel* kernel_ = nullptr;  // for the listener (the pump thread has no request)
};

class UserServiceCreator final : public Service {
public:
    explicit UserServiceCreator(std::shared_ptr<Link> link) : Service("ldn:u") {
        // CreateUserLocalCommunicationService
        on(0, [link](Request&, Response& resp) { resp.push_object(std::make_shared<LocalCommunication>(link)); });
    }
};

}  // namespace

void register_ldn(ServiceManager& sm) {
    sm.register_service("ldn:u", [] { return std::make_shared<UserServiceCreator>(nullptr); });
}

namespace ldn {
void register_ldn_service(ServiceManager& sm, std::shared_ptr<Link> link) {
    sm.register_service("ldn:u", [link] { return std::make_shared<UserServiceCreator>(link); });
}
}  // namespace ldn

}  // namespace lgp::hle
