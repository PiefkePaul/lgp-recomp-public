// nifm:u (network interface manager) for a console without internet: local
// wireless is switched on, but no access point is configured, so every
// request to bring the network up fails. Command ids, layouts and the
// RequestState values from switchbrew "Network Interface services" and libnx
// nifm.h (interface documentation only); the result is switchbrew's
// "Request failed, network not available, no connection reset" (2110-1100).
//
// What the SDK does with that (nn::nifm::SubmitNetworkRequestAndWait,
// IsNetworkAvailable, HandleNetworkRequestResult): the request ends in state
// Free with an error result, and HandleNetworkRequestResult asks
// GetAppletInfo which library applet to run, runs it and returns the Result
// from the applet's out data (libnx nifm_la.c). That is the HLE netConnect
// applet (applets.cpp), which hands back 2110-1100. The error applet does
// not fit: its 2-byte ErrorViewerReturnValue is a bad exit to that caller.
//
// Diagnostic LGP_NIFM_ONLINE=1 (set_nifm_online): requests are accepted and
// the console reports a Wi-Fi connection, for trying how far an "online"
// game path gets with local play underneath (docs/knowledge/ldn-ueber-ryuldn.md).
// Nothing behind it reaches the internet; profile and IP configuration stay
// unavailable.
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "lgp/hle/services.hpp"
#include "lgp/runtime/log.hpp"

namespace lgp::hle {
namespace {

using rt::LogLevel;

// nn::nifm::RequestState
constexpr uint32_t kRequestStateFree = 1;
constexpr uint32_t kRequestStateAccepted = 3;

std::atomic<bool> g_online{[] {
    const char* v = std::getenv("LGP_NIFM_ONLINE");
    return v != nullptr && v[0] == '1';
}()};

// AppletId LibraryAppletNetConnect, LibraryAppletMode AllForeground. Which
// applet the console names here is not documented; netConnect is the one
// whose out data the SDK caller understands (see above).
constexpr uint32_t kAppletNetConnect = 0x0F;
constexpr uint32_t kAppletModeAllForeground = 0;

class ScanRequest final : public Service {
public:
    ScanRequest() : Service("nifm:IScanRequest") {
        on(0, [this](Request& req, Response&) { done_.signal(req.kernel()); });  // Submit: finds nothing
        on(1, [](Request&, Response& resp) { resp.push<uint8_t>(0); });         // IsProcessing
        on(2, [](Request&, Response&) {});                                      // GetResult
        on(3, [this](Request& req, Response& resp) {                            // GetSystemEventReadableHandle
            rt::Handle h = rt::kInvalidHandle;
            resp.result = done_.make_handle(req.kernel(), &h);
            if (resp.result.ok())
                resp.copy_handles.push_back(h);
        });
    }

private:
    Event done_;
};

class NetworkRequest final : public Service {
public:
    NetworkRequest() : Service("nifm:IRequest") {
        on(0, [this](Request&, Response& resp) {  // GetRequestState
            std::lock_guard lock(mutex_);
            resp.push<uint32_t>(accepted_ ? kRequestStateAccepted : kRequestStateFree);
        });
        on(1, [this](Request&, Response& resp) {  // GetResult
            std::lock_guard lock(mutex_);
            if (submitted_ && !accepted_)
                resp.result = nifm_result::kNetworkNotAvailable;
        });
        on(2, [this](Request& req, Response& resp) {  // GetSystemEventReadableHandles
            rt::Handle state = rt::kInvalidHandle;
            rt::Handle other = rt::kInvalidHandle;
            resp.result = state_changed_.make_handle(req.kernel(), &state);
            if (resp.result.ok())
                resp.result = other_.make_handle(req.kernel(), &other);
            if (!resp.result.ok())
                return;
            resp.copy_handles.push_back(state);
            resp.copy_handles.push_back(other);
        });
        on(3, [this](Request& req, Response&) {  // Cancel: an accepted request goes back to Free
            bool was_accepted = false;
            {
                std::lock_guard lock(mutex_);
                was_accepted = accepted_;
                if (was_accepted)
                    accepted_ = submitted_ = false;  // offline the failed request stays as it was
            }
            if (was_accepted)
                state_changed_.signal(req.kernel());
        });
        on(4, [this](Request& req, Response&) {  // Submit: fails at once, the state stays Free
            const bool online = g_online.load();
            {
                std::lock_guard lock(mutex_);
                submitted_ = true;
                accepted_ = online;
            }
            if (online)
                rt::log(LogLevel::Info, "nifm: network request submitted, accepted (LGP_NIFM_ONLINE)");
            else
                rt::log(LogLevel::Info, "nifm: network request submitted, no network (2110-1100)");
            state_changed_.signal(req.kernel());
        });
        // Requirement setters: stored nowhere, the answer is the same for all.
        // SetRequirement, SetRequirementPreset, SetPriority, SetNetworkProfileId,
        // SetRejectable, SetConnectionConfirmationOption, SetPersistent,
        // SetInstant, SetSustainable, SetRawPriority, SetGreedy, SetSharable,
        // SetRequirementByRevision, SetKeptInSleep.
        for (const uint32_t cmd : {5u, 6u, 8u, 9u, 10u, 11u, 12u, 13u, 14u, 15u, 16u, 17u, 18u, 23u})
            on(cmd, [](Request&, Response&) {});
        // GetAppletInfo(u32 theme color, B buffer) -> u32 applet id, u32 mode,
        // u32 size. The buffer holds {s32 size, data} entries ending with
        // size -1; each entry becomes one in storage of the applet.
        on(21, [this](Request& req, Response& resp) {
            {
                std::lock_guard lock(mutex_);
                if (!submitted_) {
                    rt::log(LogLevel::Warn, "nifm: GetAppletInfo before Submit");
                    resp.unimplemented = true;
                    return;
                }
            }
            // No in data for the HLE applet: just the end marker.
            const int32_t end = -1;
            std::vector<uint8_t> data(4);
            std::memcpy(data.data(), &end, 4);
            if (req.out_buffer(0).size < data.size() || !req.write_out(0, data.data(), data.size())) {
                resp.result = rt::kernel_result::kInvalidPointer;
                return;
            }
            resp.push<uint32_t>(kAppletNetConnect);
            resp.push<uint32_t>(kAppletModeAllForeground);
            resp.push<uint32_t>(static_cast<uint32_t>(data.size()));
        });
    }

private:
    std::mutex mutex_;
    bool submitted_ = false;
    bool accepted_ = false;
    Event state_changed_, other_;
};

class GeneralService final : public Service {
public:
    explicit GeneralService(std::shared_ptr<std::atomic<uint32_t>> next_client)
        : Service("nifm:IGeneralService"), next_client_(std::move(next_client)) {
        on(1, [this](Request& req, Response&) {  // GetClientId(out 0x1A ClientId)
            const uint32_t id = (*next_client_)++;
            (void)req.write_out(0, &id, sizeof(id));
        });
        on(2, [](Request&, Response& resp) { resp.push_object(std::make_shared<ScanRequest>()); });
        on(4, [](Request& req, Response& resp) {  // CreateRequest(s32 RequirementPreset)
            const int32_t preset = req.pop<int32_t>();
            rt::log(LogLevel::Debug, "nifm: CreateRequest(preset %d)", preset);
            resp.push_object(std::make_shared<NetworkRequest>());
        });
        // No connection: no current profile, address or configuration.
        // GetCurrentNetworkProfile, GetCurrentIpAddress, GetCurrentIpConfigInfo,
        // GetInternetConnectionStatus.
        // With LGP_NIFM_ONLINE an address (192.168.0.2) and a Wi-Fi connection.
        for (const uint32_t cmd : {5u, 15u})
            on(cmd, [](Request&, Response& resp) { resp.result = nifm_result::kNetworkNotAvailable; });
        on(12, [](Request&, Response& resp) {
            if (!g_online.load()) {
                resp.result = nifm_result::kNetworkNotAvailable;
                return;
            }
            resp.push<uint32_t>(0x0200A8C0);  // network byte order
        });
        on(18, [](Request&, Response& resp) {  // -> u8 type (1 Wi-Fi), u8 strength, u8 state (4 connected)
            if (!g_online.load()) {
                resp.result = nifm_result::kNetworkNotAvailable;
                return;
            }
            resp.push<uint8_t>(1);
            resp.push<uint8_t>(3);
            resp.push<uint8_t>(4);
        });
        on(17, [](Request&, Response& resp) { resp.push<uint8_t>(1); });  // IsWirelessCommunicationEnabled
        on(20, [](Request&, Response& resp) { resp.push<uint8_t>(0); });  // IsEthernetCommunicationEnabled
        on(21, [](Request&, Response& resp) { resp.push<uint8_t>(g_online.load() ? 1 : 0); });  // IsAnyInternetRequestAccepted
        on(22, [](Request&, Response& resp) { resp.push<uint8_t>(g_online.load() ? 1 : 0); });  // IsAnyForegroundRequestAccepted
    }

private:
    std::shared_ptr<std::atomic<uint32_t>> next_client_;
};

class StaticService final : public Service {
public:
    explicit StaticService(std::shared_ptr<std::atomic<uint32_t>> next_client)
        : Service("nifm:u"), next_client_(std::move(next_client)) {
        // CreateGeneralServiceOld, CreateGeneralService(pid, u64 reserved)
        const auto create = [this](Request&, Response& resp) {
            resp.push_object(std::make_shared<GeneralService>(next_client_));
        };
        on(4, create);
        on(5, create);
    }

private:
    std::shared_ptr<std::atomic<uint32_t>> next_client_;
};

}  // namespace

void set_nifm_online(bool online) {
    g_online.store(online);
}

void register_nifm(ServiceManager& sm) {
    auto next_client = std::make_shared<std::atomic<uint32_t>>(1);
    sm.register_service("nifm:u", [next_client] { return std::make_shared<StaticService>(next_client); });
}

}  // namespace lgp::hle
