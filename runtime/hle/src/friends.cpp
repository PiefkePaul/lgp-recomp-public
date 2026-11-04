// friend:u (friends) for a console that is never online and never meets
// another console: the service objects exist, their events never fire.
// Command ids from switchbrew "Friend services". The play-history commands
// the game imports (nn::friends::GetPlayHistoryRegistrationKey,
// AddPlayHistory: 10700, 10702) are not implemented, because the layout of
// the registration key is not documented; they stop the guest with a log
// line. LGP can only need them after it met another console, which the
// offline ldn:u never lets happen.
#include "lgp/hle/services.hpp"

namespace lgp::hle {
namespace {

class FriendService final : public Service {
public:
    FriendService() : Service("friend:IFriendService") {
        on(0, [this](Request& req, Response& resp) {  // GetCompletionEvent
            rt::Handle h = rt::kInvalidHandle;
            resp.result = completion_.make_handle(req.kernel(), &h);
            if (resp.result.ok())
                resp.copy_handles.push_back(h);
        });
        on(1, [](Request&, Response&) {});  // Cancel: nothing runs in the background
    }

private:
    Event completion_;
};

class NotificationService final : public Service {
public:
    NotificationService() : Service("friend:INotificationService") {
        on(0, [this](Request& req, Response& resp) {  // GetEvent
            rt::Handle h = rt::kInvalidHandle;
            resp.result = event_.make_handle(req.kernel(), &h);
            if (resp.result.ok())
                resp.copy_handles.push_back(h);
        });
        on(1, [](Request&, Response&) {});  // Clear
    }

private:
    Event event_;
};

class ServiceCreator final : public Service {
public:
    ServiceCreator() : Service("friend:u") {
        on(0, [](Request&, Response& resp) { resp.push_object(std::make_shared<FriendService>()); });
        on(1, [](Request&, Response& resp) {  // CreateNotificationService(uid)
            resp.push_object(std::make_shared<NotificationService>());
        });
    }
};

}  // namespace

void register_friends(ServiceManager& sm) {
    sm.register_service("friend:u", [] { return std::make_shared<ServiceCreator>(); });
}

}  // namespace lgp::hle
