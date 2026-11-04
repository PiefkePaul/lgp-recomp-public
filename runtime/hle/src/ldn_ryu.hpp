// ldn over a RyuLDN server (docs/knowledge/ldn-ueber-ryuldn.md): RyuLink
// carries the ldn network through a server that speaks the RyuLDN protocol
// (the Ryujinx emulator's online play; server github.com/Ryubing/LdnServer,
// MIT) over one TCP connection per instance. The server keeps the rooms,
// hands out the virtual addresses (10.114.0.0/16) and forwards the guest's
// datagrams (ProxyData), so it also works across NATs, over the internet.
//
// Own implementation after the packet layouts of the MIT server; protocol
// version 1. Only what local wireless play over UDP needs: rooms
// (CreateAccessPointPrivate, Scan, ConnectPrivate), network updates,
// UDP proxy data and the server's pings. No P2P proxy (ExternalProxy*), no
// TCP inside the network (ProxyConnect), no Reject.
#pragma once

#include <array>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "ldn_lan.hpp"

namespace lgp::hle::ldn {

class RyuLink final : public Link {
public:
    explicit RyuLink(LdnLanConfig config);
    ~RyuLink() override;
    RyuLink(const RyuLink&) = delete;
    RyuLink& operator=(const RyuLink&) = delete;

    void set_listener(Listener* listener) override;
    // Connects to the server (failures are retried at the next scan, host
    // or connect, at most every retry_ms); true while ldn is initialized.
    bool open() override;
    void close() override;
    bool is_open() const override;

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
    uint32_t mask() const override;
    bool routes(uint32_t ip) const override;
    bool send(uint16_t src_port, uint32_t dst_ip, uint16_t dst_port, const void* data, size_t size) override;
    bool pop(uint16_t port, Datagram* out) override;
    int peek(uint16_t port) const override;

private:
    enum class Role { None, Host, Station };
    using Clock = std::chrono::steady_clock;
    struct Notice {
        bool changed = false;
        bool lost = false;
        int16_t reason = kDisconnectDestroyedByUser;
        NetworkInfo info{};
    };
    // What a request waits for (Initialize, Connected, ScanReplyEnd).
    struct Pending {
        bool waiting = false;
        bool done = false;
        int32_t error = 0;  // NetworkError from the server, 0 = none
        NetworkInfo info{};
    };

    bool ensure_connected(std::unique_lock<std::mutex>& lock);
    void drop_connection(std::unique_lock<std::mutex>& lock);
    bool send_packet(uint8_t type, const void* body, size_t size, const void* extra = nullptr, size_t extra_size = 0);
    bool wait(std::unique_lock<std::mutex>& lock, Pending& p, uint32_t timeout_ms);
    void reader(platform::ILanStream* stream);
    void handle(uint8_t type, const uint8_t* body, size_t size, Notice* ev);
    void reset_network();
    void enqueue(uint16_t port, uint32_t src_ip, uint16_t src_port, const uint8_t* data, size_t size);
    void patch_info(NetworkInfo* info) const;

    const LdnLanConfig config_;
    const std::string passphrase_;
    std::mutex random_mutex_;
    std::mt19937_64 random_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::mutex send_mutex_;  // after mutex_ when both are taken
    std::unique_ptr<platform::ILanStream> stream_;
    std::thread thread_;
    bool connected_ = false;     // the reader runs and the server answered Initialize
    bool reader_alive_ = false;  // the reader still reads from stream_
    bool opened_ = false;     // ldn initialized
    Clock::time_point last_attempt_{};
    bool attempted_ = false;
    Clock::time_point last_heard_{};
    std::array<uint8_t, 16> client_id_{};  // from the server, sent again on reconnect (keeps the MAC)
    std::array<uint8_t, 6> mac_{};
    Listener* listener_ = nullptr;
    std::mutex listener_mutex_;

    Pending init_, join_, scan_done_;
    std::vector<NetworkInfo> scan_results_;
    bool scanning_ = false;

    Role role_ = Role::None;
    NetworkInfo network_{};
    int8_t node_id_ = -1;
    uint32_t proxy_ip_ = 0;
    uint32_t proxy_mask_ = 0;
    uint8_t server_random_[0x10]{};
    bool have_server_random_ = false;
    uint64_t datagrams_out_ = 0, datagrams_in_ = 0, datagrams_logged_ = 0;
    Clock::time_point last_traffic_log_{};
    std::map<uint16_t, std::deque<Datagram>> queues_;
};

}  // namespace lgp::hle::ldn
