// bcat:u (background content delivery) on a console that never went online:
// the delivery cache storage exists and is empty, and asking for a sync
// fails because there is no internet. Command ids from switchbrew "BCAT
// services", results from switchbrew "Error codes" (module 122).
//
// Games open it to look for content that was delivered earlier (in LGP
// presumably the mystery gift menu, inferred from the PC run). Undocumented
// and therefore missing: the layout behind IDeliveryCacheProgressService::GetImpl.
// RequestSyncDeliveryCache does not hand out a progress object but fails at
// once with InternetRequestNotAccepted; that a real console answers the
// same way is an assumption.
#include "lgp/hle/services.hpp"
#include "lgp/runtime/log.hpp"

namespace lgp::hle {
namespace {

using rt::LogLevel;

class DeliveryCacheFile final : public Service {
public:
    DeliveryCacheFile() : Service("bcat:IDeliveryCacheFileService") {
        on(0, [](Request&, Response& resp) {  // Open(DirectoryName, FileName)
            resp.result = bcat_result::kNotFound;
        });
        // Read, GetSize, GetDigest: nothing is ever open.
        for (const uint32_t cmd : {1u, 2u, 3u})
            on(cmd, [](Request&, Response& resp) { resp.result = bcat_result::kNotOpened; });
    }
};

class DeliveryCacheDirectory final : public Service {
public:
    DeliveryCacheDirectory() : Service("bcat:IDeliveryCacheDirectoryService") {
        on(0, [](Request&, Response& resp) {  // Open(DirectoryName)
            resp.result = bcat_result::kNotFound;
        });
        for (const uint32_t cmd : {1u, 2u})  // Read, GetCount
            on(cmd, [](Request&, Response& resp) { resp.result = bcat_result::kNotOpened; });
    }
};

class DeliveryCacheStorage final : public Service {
public:
    DeliveryCacheStorage() : Service("bcat:IDeliveryCacheStorageService") {
        on(0, [](Request&, Response& resp) { resp.push_object(std::make_shared<DeliveryCacheFile>()); });
        on(1, [](Request&, Response& resp) { resp.push_object(std::make_shared<DeliveryCacheDirectory>()); });
        // EnumerateDeliveryCacheDirectory(B buffer of DirectoryName) -> s32 count: none.
        on(10, [](Request&, Response& resp) {
            rt::log(LogLevel::Info, "bcat: delivery cache is empty (never online)");
            resp.push<int32_t>(0);
        });
    }
};

class BcatService final : public Service {
public:
    BcatService() : Service("bcat:IBcatService") {
        // RequestSyncDeliveryCache(…WithDirectoryName, …WithApplicationId…)
        for (const uint32_t cmd : {10100u, 10101u, 20100u, 20101u})
            on(cmd, [](Request& req, Response& resp) {
                rt::log(LogLevel::Warn, "bcat: sync request %u: no internet (2122-0008)", req.command());
                resp.result = bcat_result::kInternetRequestNotAccepted;
            });
        on(10200, [](Request&, Response&) {});  // CancelSyncDeliveryCacheRequest
    }
};

class BcatServiceCreator final : public Service {
public:
    BcatServiceCreator() : Service("bcat:u") {
        on(0, [](Request&, Response& resp) {  // CreateBcatService(u64 process id)
            resp.push_object(std::make_shared<BcatService>());
        });
        // CreateDeliveryCacheStorageService(u64 process id),
        // …WithApplicationId(u64 application id)
        for (const uint32_t cmd : {1u, 2u})
            on(cmd, [](Request&, Response& resp) { resp.push_object(std::make_shared<DeliveryCacheStorage>()); });
    }
};

}  // namespace

void register_bcat(ServiceManager& sm) {
    sm.register_service("bcat:u", [] { return std::make_shared<BcatServiceCreator>(); });
}

}  // namespace lgp::hle
