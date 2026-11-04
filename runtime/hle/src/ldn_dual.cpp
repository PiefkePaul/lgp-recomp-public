// DualLink: ldn over the LAN or the RyuLDN server, picked per session
// (ldn_dual.hpp).
#include "ldn_dual.hpp"

#include <cstdlib>
#include <cstring>

#include "lgp/runtime/host_work.hpp"
#include "lgp/runtime/log.hpp"

namespace lgp::hle {

using rt::LogLevel;

namespace {
// LGP_LDN_REMOTE=1 starts on the server (tests, a run without the mod).
std::atomic<bool> g_remote{[] {
    const char* v = std::getenv("LGP_LDN_REMOTE");
    return v != nullptr && std::strcmp(v, "1") == 0;
}()};

// mutex_ of a DualLink. Its holder may wait inside the server link with its
// core slot given up (RyuLink::open connects): a thread that finds it taken
// waits without its slot too, else the holder cannot get its slot back
// (host_work.hpp).
std::unique_lock<std::mutex> take(std::mutex& m) {
    std::unique_lock lock(m, std::try_to_lock);
    if (!lock.owns_lock()) {
        rt::HostWorkScope away(true);
        lock.lock();
    }
    return lock;
}
}  // namespace

void set_ldn_remote(bool remote) { g_remote.store(remote, std::memory_order_release); }
bool ldn_remote() { return g_remote.load(std::memory_order_acquire); }

namespace ldn {

DualLink::DualLink(std::shared_ptr<Link> nearby, std::shared_ptr<Link> server)
    : nearby_(std::move(nearby)), server_(std::move(server)) {
    nearby_->set_listener(this);
    server_->set_listener(this);
}

DualLink::~DualLink() {
    nearby_->set_listener(nullptr);
    server_->set_listener(nullptr);
}

void DualLink::set_listener(Link::Listener* listener) { listener_.store(listener, std::memory_order_release); }

// Only the active link has a network, so whatever it reports is ours.
void DualLink::network_changed(const NetworkInfo& info) {
    if (Link::Listener* l = listener_.load(std::memory_order_acquire))
        l->network_changed(info);
}

void DualLink::network_lost(int16_t reason) {
    if (Link::Listener* l = listener_.load(std::memory_order_acquire))
        l->network_lost(reason);
}

bool DualLink::open() {
    const std::unique_lock lock = take(mutex_);
    if (active())
        return true;
    Link* want = ldn_remote() ? server_.get() : nearby_.get();
    if (!want->open())
        return false;
    rt::log(LogLevel::Info, "ldn: this session goes %s", want == server_.get() ? "through the server" : "over the LAN");
    active_.store(want, std::memory_order_release);
    return true;
}

void DualLink::close() {
    const std::unique_lock lock = take(mutex_);
    if (Link* a = active())
        a->close();
    active_.store(nullptr, std::memory_order_release);
}

bool DualLink::is_open() const {
    const Link* a = active();
    return a != nullptr && a->is_open();
}

Link* DualLink::select() {
    const std::unique_lock lock = take(mutex_);
    Link* a = active();
    if (a == nullptr)
        return nullptr;
    Link* want = ldn_remote() ? server_.get() : nearby_.get();
    if (want == a || a->in_network())
        return a;
    if (!want->open())  // stay where we are; the other way has no transport
        return a;
    a->close();
    rt::log(LogLevel::Info, "ldn: switched %s", want == server_.get() ? "to the server" : "to the LAN");
    active_.store(want, std::memory_order_release);
    return want;
}

bool DualLink::host(NetworkInfo* info, bool private_network) {
    Link* a = select();
    return a != nullptr && a->host(info, private_network);
}

void DualLink::update_host(const NetworkInfo& info) {
    if (Link* a = active())
        a->update_host(info);
}

void DualLink::destroy() {
    if (Link* a = active())
        a->destroy();
}

void DualLink::random_bytes(uint8_t* out, size_t size) {
    Link* a = active();
    (a != nullptr ? a : nearby_.get())->random_bytes(out, size);
}

std::vector<NetworkInfo> DualLink::scan(const ScanFilter& filter, int16_t channel) {
    Link* a = select();
    return a != nullptr ? a->scan(filter, channel) : std::vector<NetworkInfo>{};
}

rt::Result DualLink::connect(const uint8_t (&session_id)[0x10], const uint8_t* server_random, const UserConfig& user,
                             int16_t version, NetworkInfo* out) {
    Link* a = select();
    if (a == nullptr)
        return ldn_result::kNetworkNotFound;
    return a->connect(session_id, server_random, user, version, out);
}

void DualLink::disconnect() {
    if (Link* a = active())
        a->disconnect();
}

bool DualLink::in_network() const {
    const Link* a = active();
    return a != nullptr && a->in_network();
}

uint32_t DualLink::ip() const {
    const Link* a = active();
    return a != nullptr ? a->ip() : 0;
}

uint32_t DualLink::mask() const {
    const Link* a = active();
    return a != nullptr ? a->mask() : 0;
}

bool DualLink::routes(uint32_t ip) const {
    const Link* a = active();
    return a != nullptr && a->routes(ip);
}

bool DualLink::send(uint16_t src_port, uint32_t dst_ip, uint16_t dst_port, const void* data, size_t size) {
    Link* a = active();
    return a != nullptr && a->send(src_port, dst_ip, dst_port, data, size);
}

bool DualLink::pop(uint16_t port, Datagram* out) {
    Link* a = active();
    return a != nullptr && a->pop(port, out);
}

int DualLink::peek(uint16_t port) const {
    const Link* a = active();
    return a != nullptr ? a->peek(port) : -1;
}

}  // namespace ldn
}  // namespace lgp::hle
