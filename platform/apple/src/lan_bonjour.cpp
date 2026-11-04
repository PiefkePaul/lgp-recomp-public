// Bonjour discovery for ldn over LAN (lan_bonjour.hpp) with the DNS-SD C API:
// DNSServiceRegister announces a port without binding it a second time (the
// data stays on the existing UDP socket), and Resolve + GetAddrInfo give an
// IPv4 address directly. kDNSServiceFlagsIncludeP2P is the DNS-SD form of
// Network.framework's include_peer_to_peer (AWDL); only discovery uses it, the
// BSD data socket itself does not send over AWDL.
#include "lan_bonjour.hpp"

#include <arpa/inet.h>
#include <dispatch/dispatch.h>
#include <dns_sd.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <cstdio>
#include <map>
#include <mutex>
#include <string>

namespace lgp::platform {
namespace {

constexpr const char* kServiceType = "_lgp-ldn._udp";

class DnssdBonjour final : public BonjourLan {
public:
    explicit DnssdBonjour(dispatch_queue_t queue) : queue_(queue) {}
    ~DnssdBonjour() override {
        stop_advertising();
        stop_browsing();
        dispatch_release(queue_);
    }

    void advertise(uint16_t port) override {
        Call c{this, port};
        dispatch_sync_f(queue_, &c, [](void* p) {
            Call& c = *static_cast<Call*>(p);
            c.self->drop_registration();
            const DNSServiceErrorType err =
                DNSServiceRegister(&c.self->registration_, kDNSServiceFlagsIncludeP2P, kDNSServiceInterfaceIndexAny,
                                   nullptr, kServiceType, nullptr, nullptr, htons(c.port), 0, nullptr,
                                   &DnssdBonjour::registered, c.self);
            if (err != kDNSServiceErr_NoError) {
                std::fprintf(stderr, "ldn lan: Bonjour register failed (%d)\n", static_cast<int>(err));
                c.self->registration_ = nullptr;
                return;
            }
            DNSServiceSetDispatchQueue(c.self->registration_, c.self->queue_);
        });
    }

    void stop_advertising() override {
        dispatch_sync_f(queue_, this, [](void* p) { static_cast<DnssdBonjour*>(p)->drop_registration(); });
    }

    void start_browsing() override {
        dispatch_sync_f(queue_, this, [](void* p) {
            DnssdBonjour& self = *static_cast<DnssdBonjour*>(p);
            if (self.browse_ != nullptr)
                return;
            const DNSServiceErrorType err =
                DNSServiceBrowse(&self.browse_, kDNSServiceFlagsIncludeP2P, kDNSServiceInterfaceIndexAny,
                                 kServiceType, nullptr, &DnssdBonjour::browsed, &self);
            if (err != kDNSServiceErr_NoError) {
                std::fprintf(stderr, "ldn lan: Bonjour browse failed (%d)\n", static_cast<int>(err));
                self.browse_ = nullptr;
                return;
            }
            DNSServiceSetDispatchQueue(self.browse_, self.queue_);
        });
    }

    void stop_browsing() override {
        dispatch_sync_f(queue_, this, [](void* p) {
            DnssdBonjour& self = *static_cast<DnssdBonjour*>(p);
            if (self.browse_ != nullptr)
                DNSServiceRefDeallocate(self.browse_);
            self.browse_ = nullptr;
            for (auto& [name, r] : self.services_)
                r->drop();
            self.services_.clear();
            std::lock_guard lock(self.mutex_);
            self.found_.clear();
        });
    }

    std::vector<LanEndpoint> found() override {
        std::lock_guard lock(mutex_);
        std::vector<LanEndpoint> out;
        for (const auto& [name, e] : found_)
            out.push_back(e);
        return out;
    }

private:
    struct Call {
        DnssdBonjour* self;
        uint16_t port;
    };
    // One service seen by the browser, resolved to an IPv4 endpoint.
    struct Service {
        DnssdBonjour* self = nullptr;
        std::string name;
        uint16_t port = 0;
        DNSServiceRef resolve = nullptr;
        DNSServiceRef address = nullptr;
        void drop() {
            if (resolve != nullptr)
                DNSServiceRefDeallocate(resolve);
            if (address != nullptr)
                DNSServiceRefDeallocate(address);
            resolve = address = nullptr;
        }
    };

    void drop_registration() {
        if (registration_ != nullptr)
            DNSServiceRefDeallocate(registration_);
        registration_ = nullptr;
    }

    static void DNSSD_API registered(DNSServiceRef, DNSServiceFlags, DNSServiceErrorType err, const char* name,
                                     const char*, const char*, void*) {
        if (err != kDNSServiceErr_NoError)
            std::fprintf(stderr, "ldn lan: Bonjour register failed (%d)\n", static_cast<int>(err));
        else
            std::fprintf(stderr, "ldn lan: Bonjour announces \"%s\"\n", name);
    }

    static void DNSSD_API browsed(DNSServiceRef, DNSServiceFlags flags, uint32_t interface, DNSServiceErrorType err,
                                  const char* name, const char* type, const char* domain, void* context) {
        DnssdBonjour& self = *static_cast<DnssdBonjour*>(context);
        if (err != kDNSServiceErr_NoError) {
            std::fprintf(stderr, "ldn lan: Bonjour browse error (%d)\n", static_cast<int>(err));
            return;
        }
        const std::string key = std::string(name) + "%" + std::to_string(interface);
        if ((flags & kDNSServiceFlagsAdd) == 0) {
            const auto it = self.services_.find(key);
            if (it != self.services_.end()) {
                it->second->drop();
                self.services_.erase(it);
            }
            std::lock_guard lock(self.mutex_);
            self.found_.erase(key);
            return;
        }
        if (self.services_.count(key) != 0)
            return;
        auto s = std::make_unique<Service>();
        s->self = &self;
        s->name = key;
        if (DNSServiceResolve(&s->resolve, kDNSServiceFlagsIncludeP2P, interface, name, type, domain,
                              &DnssdBonjour::resolved, s.get()) != kDNSServiceErr_NoError) {
            s->resolve = nullptr;
            return;
        }
        DNSServiceSetDispatchQueue(s->resolve, self.queue_);
        self.services_[key] = std::move(s);
    }

    static void DNSSD_API resolved(DNSServiceRef, DNSServiceFlags, uint32_t interface, DNSServiceErrorType err,
                                   const char*, const char* host, uint16_t port, uint16_t, const unsigned char*,
                                   void* context) {
        Service& s = *static_cast<Service*>(context);
        if (err != kDNSServiceErr_NoError || s.address != nullptr)
            return;
        s.port = ntohs(port);
        if (DNSServiceGetAddrInfo(&s.address, kDNSServiceFlagsIncludeP2P, interface, kDNSServiceProtocol_IPv4, host,
                                  &DnssdBonjour::addressed, &s) != kDNSServiceErr_NoError) {
            s.address = nullptr;
            return;
        }
        DNSServiceSetDispatchQueue(s.address, s.self->queue_);
    }

    static void DNSSD_API addressed(DNSServiceRef, DNSServiceFlags flags, uint32_t, DNSServiceErrorType err,
                                    const char*, const struct sockaddr* address, uint32_t, void* context) {
        Service& s = *static_cast<Service*>(context);
        if (err != kDNSServiceErr_NoError || address == nullptr || address->sa_family != AF_INET ||
            (flags & kDNSServiceFlagsAdd) == 0)
            return;
        LanEndpoint e;
        e.ip = ntohl(reinterpret_cast<const sockaddr_in*>(address)->sin_addr.s_addr);
        e.port = s.port;
        std::lock_guard lock(s.self->mutex_);
        if (s.self->found_.count(s.name) == 0)
            std::fprintf(stderr, "ldn lan: Bonjour found %s\n", lan_endpoint_string(e).c_str());
        s.self->found_[s.name] = e;
    }

    dispatch_queue_t queue_;
    DNSServiceRef registration_ = nullptr;  // only touched on queue_
    DNSServiceRef browse_ = nullptr;
    std::map<std::string, std::unique_ptr<Service>> services_;
    std::mutex mutex_;  // found_: queue_ writes, the scan reads
    std::map<std::string, LanEndpoint> found_;
};

}  // namespace

std::unique_ptr<BonjourLan> BonjourLan::create() {
    dispatch_queue_t queue = dispatch_queue_create("lgp.ldn.bonjour", DISPATCH_QUEUE_SERIAL);
    if (queue == nullptr)
        return nullptr;
    return std::make_unique<DnssdBonjour>(queue);
}

}  // namespace lgp::platform
