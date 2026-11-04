// pctl (parental controls): no restrictions are set, so the service only has
// to exist and accept initialization.
#include "lgp/hle/services.hpp"

namespace lgp::hle {
namespace {

class ParentalControlService final : public Service {
public:
    ParentalControlService() : Service("pctl:IParentalControlService") {
        on(1, [](Request&, Response&) {});  // Initialize
    }
};

class ParentalControlServiceFactory final : public Service {
public:
    ParentalControlServiceFactory() : Service("pctl") {
        // CreateService / CreateServiceWithoutInitialize (u64 reserved, pid)
        const auto create = [](Request&, Response& resp) {
            resp.push_object(std::make_shared<ParentalControlService>());
        };
        on(0, create);
        on(1, create);
    }
};

}  // namespace

void register_pctl(ServiceManager& sm) {
    sm.register_service("pctl", [] { return std::make_shared<ParentalControlServiceFactory>(); });
}

}  // namespace lgp::hle
