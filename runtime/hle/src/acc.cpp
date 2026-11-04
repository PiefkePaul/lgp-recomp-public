// acc:u0 (IAccountServiceForApplication): exactly one local user, from
// SystemConfig; it is also the PreselectedUser am hands to the application.
// The user has no Network Service Account (no Nintendo Account link).
#include <algorithm>
#include <array>
#include <cstring>

#include "lgp/hle/services.hpp"

namespace lgp::hle {
namespace {

using Uid = std::array<uint64_t, 2>;

constexpr size_t kUserListSize = 8;  // ACC_USER_LIST_SIZE (libnx)

struct ProfileBase {
    Uid uid;
    uint64_t last_edit_timestamp;
    char nickname[0x20];
};
static_assert(sizeof(ProfileBase) == 0x38);

class Profile final : public Service {
public:
    explicit Profile(const SystemConfig& config) : Service("acc:IProfile") {
        base_.uid = config.user_id;
        base_.last_edit_timestamp = 0;
        std::memset(base_.nickname, 0, sizeof(base_.nickname));
        std::memcpy(base_.nickname, config.user_nickname.data(),
                    std::min(config.user_nickname.size(), sizeof(base_.nickname) - 1));
        on(0, [this](Request& req, Response& resp) {  // Get(out 0x1A UserData) -> ProfileBase
            const std::array<uint8_t, 0x80> user_data{};
            if (!req.write_out(0, user_data.data(), user_data.size())) {
                resp.result = rt::kernel_result::kInvalidPointer;
                return;
            }
            resp.push(base_);
        });
        on(1, [this](Request&, Response& resp) { resp.push(base_); });  // GetBase
    }

private:
    ProfileBase base_{};
};

// IAsyncContext of a request that cannot succeed offline: done at once.
class AsyncContext final : public Service {
public:
    explicit AsyncContext(rt::Result result) : Service("acc:IAsyncContext"), result_(result) {
        on(0, [this](Request& req, Response& resp) {  // GetSystemEvent
            rt::Handle h = rt::kInvalidHandle;
            resp.result = done_.make_handle(req.kernel(), &h);
            if (!resp.result.ok())
                return;
            done_.signal(req.kernel());
            resp.copy_handles.push_back(h);
        });
        on(1, [](Request&, Response&) {});                               // Cancel
        on(2, [](Request&, Response& resp) { resp.push<uint8_t>(1); });  // HasDone
        on(3, [this](Request&, Response& resp) { resp.result = result_; });  // GetResult
    }

private:
    rt::Result result_;
    Event done_;
};

// The user's Network Service Account (Nintendo Account link). The single
// local user has none, so everything that needs it fails with
// ResultNetworkServiceAccountRegistrationRequired (124-0200). That the
// console answers CheckAvailability this way for an unlinked user is an
// assumption (switchbrew documents the code, not which command returns it).
class ManagerForApplication final : public Service {
public:
    ManagerForApplication() : Service("acc:IManagerForApplication") {
        const auto unavailable = [](Request&, Response& resp) {
            resp.result = acc_result::kNetworkServiceAccountRegistrationRequired;
        };
        on(0, unavailable);  // CheckAvailability
        on(1, unavailable);  // GetAccountId -> u64
        on(2, [](Request&, Response& resp) {  // EnsureIdTokenCacheAsync -> IAsyncContext
            resp.push_object(std::make_shared<AsyncContext>(acc_result::kNetworkServiceAccountRegistrationRequired));
        });
        on(3, unavailable);  // LoadIdTokenCache(B buffer) -> u32 size
    }
};

class AccountServiceForApplication final : public Service {
public:
    explicit AccountServiceForApplication(const SystemConfig& config) : Service("acc:u0"), config_(config) {
        on(0, [](Request&, Response& resp) { resp.push<int32_t>(1); });  // GetUserCount
        on(1, [this](Request& req, Response& resp) {                     // GetUserExistence(uid) -> bool
            resp.push<uint8_t>(req.pop<Uid>() == config_.user_id ? 1 : 0);
        });
        on(2, [this](Request& req, Response& resp) { list_users(req, resp); });  // ListAllUsers
        on(3, [this](Request& req, Response& resp) { list_users(req, resp); });  // ListOpenUsers
        on(4, [this](Request&, Response& resp) { resp.push(config_.user_id); });  // GetLastOpenedUser
        on(5, [this](Request& req, Response& resp) {                             // GetProfile(uid)
            if (req.pop<Uid>() != config_.user_id) {
                resp.result = acc_result::kUserNotExist;
                return;
            }
            resp.push_object(std::make_shared<Profile>(config_));
        });
        on(100, [](Request&, Response&) {});  // InitializeApplicationInfoV0(pid, u64)
        on(101, [this](Request& req, Response& resp) {  // GetBaasAccountManagerForApplication(uid)
            if (req.pop<Uid>() != config_.user_id) {
                resp.result = acc_result::kUserNotExist;
                return;
            }
            resp.push_object(std::make_shared<ManagerForApplication>());
        });
        on(140, [](Request&, Response&) {});  // InitializeApplicationInfo(pid, u64)
    }

private:
    // Out buffer 0xA of user ids, unused entries zero.
    void list_users(Request& req, Response& resp) {
        std::array<Uid, kUserListSize> list{};
        list[0] = config_.user_id;
        if (!req.write_out(0, list.data(), sizeof(list)))
            resp.result = rt::kernel_result::kInvalidPointer;
    }

    SystemConfig config_;
};

}  // namespace

void register_acc(ServiceManager& sm, const SystemConfig& config) {
    sm.register_service("acc:u0", [config] { return std::make_shared<AccountServiceForApplication>(config); });
}

}  // namespace lgp::hle
