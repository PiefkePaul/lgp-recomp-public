// Library applets besides the software keyboard, as HLE without any UI:
//
// - error (AppletId 0x0E): nn::err::ShowError and friends. The error is
//   logged with its code ("2110-1100") and message and handed to the host
//   (AppletState::show_message), which may show it until the user presses
//   OK; then the applet closes.
// - netConnect (AppletId 0x0F): what nifm's GetAppletInfo names after a
//   failed network request. It closes at once with the request's error, so
//   nn::nifm::HandleNetworkRequestResult returns 2110-1100.
// - playerSelect (AppletId 0x10): nn::account::ShowUserSelector picks the
//   one user without asking, unless the caller excluded it or requires a
//   Nintendo Account. EnsureNetworkServiceAccountAvailable (UiMode 2) ends
//   as cancelled by the user: there is no Nintendo Account to link.
//
// Storage layouts from switchbrew "Error Applet" and "Profile Selector" and
// libnx psel.h/nifm_la.c (interface documentation only). For error and
// playerSelect, in storage 0 is CommonArguments.
#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <string>

#include "am_internal.hpp"
#include "lgp/runtime/log.hpp"

namespace lgp::hle {
namespace {

using am_detail::StorageData;
using rt::LogLevel;

template <typename T>
T get(const std::vector<uint8_t>& v, size_t at) {
    T x{};
    if (at + sizeof(T) <= v.size())
        std::memcpy(&x, v.data() + at, sizeof(T));
    return x;
}

std::string utf8_at(const std::vector<uint8_t>& v, size_t at, size_t max) {
    std::string s;
    for (size_t i = at; i < v.size() && i < at + max && v[i] != 0; ++i)
        s.push_back(static_cast<char>(v[i]));
    return s;
}

// "2XXX-YYYY" as the console shows a Result.
std::string result_code(uint32_t raw) {
    char s[16];
    std::snprintf(s, sizeof(s), "%04u-%04u", 2000 + (raw & 0x1FF), (raw >> 9) & 0x1FFF);
    return s;
}

std::string error_code(uint64_t code) {
    char s[24];
    std::snprintf(s, sizeof(s), "%04u-%04u", static_cast<uint32_t>(code), static_cast<uint32_t>(code >> 32));
    return s;
}

// --- error ---------------------------------------------------------------------

// nn::err::ErrorType
enum : uint8_t {
    kSystemData = 0,
    kSystemError = 1,
    kApplicationError = 2,
    kEula = 3,
    kParentalControl = 4,
    kRecordedSystemData = 5,
    kRecordedSystemError = 6,
    kRecordedApplicationError = 7,
    kSystemUpdateEula = 8,
};

class ErrorApplet final : public am_detail::LibraryApplet {
public:
    explicit ErrorApplet(std::shared_ptr<AppletState> state) : state_(std::move(state)) {}

private:
    void on_start(const Request& req, std::vector<std::shared_ptr<StorageData>> in) override {
        std::vector<uint8_t> p;
        if (in.size() < 2 || !in[1]->contents(req, &p) || p.empty()) {
            rt::log(LogLevel::Warn, "error applet: started without a startup parameter (%zu storages)", in.size());
        } else {
            const ErrorMessage m = describe(p);
            rt::log(LogLevel::Warn, "error applet: %s%s%s", m.code.c_str(), m.text.empty() ? "" : ": ",
                    m.text.c_str());
            state_->show_message(m);
        }
        // ErrorViewerReturnValue {version, destination Nowhere}: the game may
        // pop it to learn whether to jump somewhere; nobody jumps here.
        emit_out({0, 0});
        finish();
    }

    void on_interactive_in(const Request&, std::vector<uint8_t> data) override {
        rt::log(LogLevel::Warn, "error applet: unexpected interactive data (%zu bytes)", data.size());
    }

    static ErrorMessage describe(const std::vector<uint8_t>& p) {
        ErrorMessage m;
        switch (p[0]) {
        case kSystemData:  // +0x6 IsErrorCode, +0x8 ErrorCode, +0x10 Result
            if (get<uint8_t>(p, 6) != 0)
                m.code = "error " + error_code(get<uint64_t>(p, 8));
            else
                m.code = "error " + result_code(get<uint32_t>(p, 0x10));
            break;
        case kSystemError:  // +0x8 ErrorCode, +0x18 dialog message
            m.code = "error " + error_code(get<uint64_t>(p, 8));
            m.text = utf8_at(p, 0x18, 0x800);
            break;
        case kApplicationError:  // +0x8 application error code, +0x14 dialog message
            m.code = "application error " + std::to_string(get<uint32_t>(p, 8));
            m.text = utf8_at(p, 0x14, 0x800);
            break;
        default:
            m.code = "error type " + std::to_string(p[0]);
            break;
        }
        return m;
    }

    std::shared_ptr<AppletState> state_;
};

// --- netConnect --------------------------------------------------------------------

// Out data as the nifm caller reads it (libnx nifm_la.c, modelled on
// nn::nifm::HandleNetworkRequestResult): s32 ret, then a Result, which the
// caller returns. A shorter storage is a bad applet exit. That ret != 0
// means "not connected" is an assumption; the caller only checks the size
// for it.
class NetConnectApplet final : public am_detail::LibraryApplet {
private:
    void on_start(const Request&, std::vector<std::shared_ptr<StorageData>> in) override {
        rt::log(LogLevel::Warn, "net connect applet: no network, error %s (%zu in storages)",
                result_code(nifm_result::kNetworkNotAvailable.raw).c_str(), in.size());
        std::vector<uint8_t> out(8, 0);
        const int32_t ret = 1;
        const uint32_t raw = nifm_result::kNetworkNotAvailable.raw;
        std::memcpy(out.data(), &ret, 4);
        std::memcpy(out.data() + 4, &raw, 4);
        emit_out(std::move(out));
        finish();
    }

    void on_interactive_in(const Request&, std::vector<uint8_t> data) override {
        rt::log(LogLevel::Warn, "net connect applet: unexpected interactive data (%zu bytes)", data.size());
    }
};

// --- playerSelect ------------------------------------------------------------------

// nn::account::UiMode
constexpr uint32_t kUiModeUserSelector = 0;
constexpr uint32_t kUiModeEnsureNetworkServiceAccount = 2;

class PlayerSelectApplet final : public am_detail::LibraryApplet {
public:
    explicit PlayerSelectApplet(std::shared_ptr<AppletState> state) : state_(std::move(state)) {}

private:
    void on_start(const Request& req, std::vector<std::shared_ptr<StorageData>> in) override {
        std::vector<uint8_t> ui;
        if (in.size() < 2 || !in[1]->contents(req, &ui) || ui.size() < 4) {
            rt::log(LogLevel::Warn, "player select: started without UiSettings, cancelling");
            close(acc_result::kCancelledByUser);
            return;
        }
        const uint32_t mode = get<uint32_t>(ui, 0);
        if (mode == kUiModeUserSelector) {
            select_user(ui);
            return;
        }
        if (mode == kUiModeEnsureNetworkServiceAccount)
            rt::log(LogLevel::Info, "player select: no Nintendo Account to link, cancelled");
        else
            rt::log(LogLevel::Warn, "player select: UiMode %u not offered, cancelled", mode);
        close(acc_result::kCancelledByUser);
    }

    void on_interactive_in(const Request&, std::vector<uint8_t> data) override {
        rt::log(LogLevel::Warn, "player select: unexpected interactive data (%zu bytes)", data.size());
    }

    // UiSettings (libnx PselUiSettingsV1): +0x8 invalid_uid_list (8 uids the
    // caller excludes), +0x90 is_network_service_account_required.
    void select_user(const std::vector<uint8_t>& ui) {
        if (get<uint8_t>(ui, 0x90) != 0) {
            rt::log(LogLevel::Info, "player select: Nintendo Account required, none linked, cancelled");
            close(acc_result::kCancelledByUser);
            return;
        }
        const auto& uid = state_->config().user_id;
        for (size_t i = 0; i < 8; ++i) {
            const size_t at = 8 + i * 0x10;
            if (at + 0x10 <= ui.size() && std::memcmp(ui.data() + at, uid.data(), 0x10) == 0) {
                rt::log(LogLevel::Info, "player select: the only user is excluded, cancelled");
                close(acc_result::kCancelledByUser);
                return;
            }
        }
        close(rt::kSuccess);
    }

    // UiReturnArg: u64 result, then the selected uid.
    void close(rt::Result result) {
        std::vector<uint8_t> out(0x18, 0);
        const uint64_t raw = result.raw;
        std::memcpy(out.data(), &raw, 8);
        if (result.ok())
            std::memcpy(out.data() + 8, state_->config().user_id.data(), 0x10);
        emit_out(std::move(out));
        finish();
    }

    std::shared_ptr<AppletState> state_;
};

}  // namespace

namespace am_detail {
std::shared_ptr<LibraryApplet> make_error_applet(std::shared_ptr<AppletState> state) {
    return std::make_shared<ErrorApplet>(std::move(state));
}
std::shared_ptr<LibraryApplet> make_net_connect() { return std::make_shared<NetConnectApplet>(); }
std::shared_ptr<LibraryApplet> make_player_select(std::shared_ptr<AppletState> state) {
    return std::make_shared<PlayerSelectApplet>(std::move(state));
}
}  // namespace am_detail

}  // namespace lgp::hle
