// Both ways of local play in one run (docs/knowledge/ldn-ueber-ryuldn.md):
// DualLink holds a LanLink ("nearby players", Wi-Fi) and a RyuLink ("players
// far away", the RyuLDN server) and lets ldn use one of them at a time.
// Which one follows set_ldn_remote(): the game's online button (a mod) picks
// the server, everything else the LAN. The choice takes effect at the next
// Initialize, scan, create or connect outside a network; a network that is
// up stays on its link until it ends.
#pragma once

#include <atomic>
#include <memory>
#include <mutex>

#include "ldn_lan.hpp"

namespace lgp::hle::ldn {

class DualLink final : public Link, private Link::Listener {
public:
    DualLink(std::shared_ptr<Link> nearby, std::shared_ptr<Link> server);
    ~DualLink() override;
    DualLink(const DualLink&) = delete;
    DualLink& operator=(const DualLink&) = delete;

    void set_listener(Link::Listener* listener) override;
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
    void network_changed(const NetworkInfo& info) override;
    void network_lost(int16_t reason) override;
    // The link set_ldn_remote() asks for; switches to it while ldn is
    // initialized and the current one is in no network.
    Link* select();
    Link* active() const { return active_.load(std::memory_order_acquire); }

    std::shared_ptr<Link> nearby_, server_;
    std::mutex mutex_;                 // open/close/select (the ldn service's thread)
    std::atomic<Link*> active_{nullptr};  // the link of the current ldn session, nullptr when closed
    std::atomic<Link::Listener*> listener_{nullptr};
};

}  // namespace lgp::hle::ldn
