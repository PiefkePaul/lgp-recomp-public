// sm: (service manager), the named port every process connects to first.
#include <cstring>

#include "lgp/hle/service.hpp"
#include "lgp/runtime/log.hpp"

namespace lgp::hle {
namespace {

class SmService final : public Service {
public:
    explicit SmService(const ServiceManager& manager) : Service("sm:"), manager_(manager) {
        // RegisterClient (Initialize): pid via the special header, u64 reserved.
        on(0, [this](Request&, Response&) { initialized_ = true; });
        // GetServiceHandle(name: 8 bytes, zero padded) -> move handle.
        on(1, [this](Request& req, Response& resp) {
            if (!initialized_) {
                resp.result = sm_result::kInvalidClient;
                return;
            }
            const uint64_t raw = req.pop<uint64_t>();
            char name[9] = {};
            std::memcpy(name, &raw, 8);
            std::shared_ptr<Service> service = manager_.create(name);
            if (!service) {
                rt::log(rt::LogLevel::Warn, "sm: GetServiceHandle(\"%s\"): not registered", name);
                resp.result = sm_result::kNotRegistered;
                return;
            }
            Handle h = rt::kInvalidHandle;
            const Result rc = req.kernel().create_session(std::make_shared<Session>(std::move(service)), &h);
            if (rc.failed()) {
                resp.result = rc;
                return;
            }
            resp.move_handles.push_back(h);
        });
    }

private:
    const ServiceManager& manager_;
    bool initialized_ = false;
};

}  // namespace

void ServiceManager::register_service(const std::string& name, Factory factory) {
    std::lock_guard lock(mutex_);
    factories_[name] = std::move(factory);
}

std::shared_ptr<Service> ServiceManager::create(const std::string& name) const {
    Factory factory;
    {
        std::lock_guard lock(mutex_);
        const auto it = factories_.find(name);
        if (it == factories_.end())
            return nullptr;
        factory = it->second;
    }
    return factory();
}

void ServiceManager::install(Kernel& kernel) {
    // Each connection gets its own sm: instance (its own RegisterClient state).
    kernel.register_named_port("sm:", [this] { return std::make_shared<Session>(std::make_shared<SmService>(*this)); });
}

}  // namespace lgp::hle
