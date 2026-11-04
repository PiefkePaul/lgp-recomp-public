// vi (display) for the NVN-HLE: an SDK title opens the default display and
// its managed layer and hands the native window to nvnWindowBuilder. With the
// NVN-HLE the window never talks to the compositor, so vi only has to hand
// out stable ids, the layer's native-window parcel and the events; binder
// transactions (TransactParcel) stay unimplemented and stop the guest.
// Command ids from switchbrew "Display services"; parcel layout as Ryujinx
// and libnx (nwindow) read it.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <string>
#include <utility>

#include "lgp/hle/event.hpp"
#include "lgp/hle/services.hpp"
#include "lgp/runtime/log.hpp"

namespace lgp::hle {
namespace {

constexpr uint64_t kDefaultDisplayId = 0;
constexpr int32_t kBinderId = 1;  // IGraphicBufferProducer of the managed layer
constexpr int64_t kWidth = 1280, kHeight = 720;  // handheld

// What GetDisplayResolution reports. LGP_VI_DISPLAY_SIZE=WxH changes it, a
// diagnosis for wider pictures (docs/analysis/50-breitbild-ohne-strecken.md):
// does the game derive its render size from it?
std::pair<int64_t, int64_t> display_size() {
    static const std::pair<int64_t, int64_t> size = [] {
        std::pair<int64_t, int64_t> s{kWidth, kHeight};
        const char* env = std::getenv("LGP_VI_DISPLAY_SIZE");
        if (env == nullptr || *env == '\0') return s;
        long long w = 0, h = 0;
        char x = 0, extra = 0;
        if (std::sscanf(env, "%lld%c%lld%c", &w, &x, &h, &extra) == 3 && (x == 'x' || x == 'X') && w > 0 && h > 0 &&
            w <= 8192 && h <= 8192) {
            s = {w, h};
            rt::log(rt::LogLevel::Warn, "vi: display resolution %lldx%lld (LGP_VI_DISPLAY_SIZE)", w, h);
        } else {
            rt::log(rt::LogLevel::Warn, "vi: LGP_VI_DISPLAY_SIZE=%s not understood (WxH), 1280x720", env);
        }
        return s;
    }();
    return size;
}

void ignore(Request&, Response&) {}

struct ViState {
    Event vsync;         // GetDisplayVsyncEvent: never signalled; lgp_boot paces NVN presents (frame_pacer.hpp)
    Event buffer_event;  // HOSBinderDriver GetNativeHandle
};

// Native window parcel of a layer: header, then a flat binder object
// (type 2, flags 0, binder id, cookie 0, service name "dispdrv").
std::vector<uint8_t> native_window_parcel(int32_t binder_id) {
    std::vector<uint8_t> payload(0x28, 0);
    const uint32_t type = 2;
    std::memcpy(payload.data() + 0x00, &type, 4);
    const int64_t id = binder_id;
    std::memcpy(payload.data() + 0x08, &id, 8);
    std::memcpy(payload.data() + 0x18, "dispdrv", 7);
    std::vector<uint8_t> out(0x10, 0);
    const uint32_t header[4] = {static_cast<uint32_t>(payload.size()), 0x10, 4,
                                static_cast<uint32_t>(0x10 + payload.size())};
    std::memcpy(out.data(), header, sizeof(header));
    out.insert(out.end(), payload.begin(), payload.end());
    out.insert(out.end(), 4, 0);  // object offsets: one entry at 0
    return out;
}

void copy_event(Request& req, Response& resp, Event& ev) {
    Handle h = rt::kInvalidHandle;
    resp.result = ev.make_handle(req.kernel(), &h);
    if (resp.result.ok())
        resp.copy_handles.push_back(h);
}

class HosBinderDriver final : public Service {
public:
    explicit HosBinderDriver(std::shared_ptr<ViState> state) : Service("vi:IHOSBinderDriver"), state_(std::move(state)) {
        on(1, ignore);  // AdjustRefcount(binder id, addval, type)
        on(2, [this](Request& req, Response& resp) {  // GetNativeHandle(binder id, type)
            copy_event(req, resp, state_->buffer_event);
        });
    }

private:
    std::shared_ptr<ViState> state_;
};

class SystemDisplayService final : public Service {
public:
    SystemDisplayService() : Service("vi:ISystemDisplayService") {
        on(2205, ignore);  // SetLayerZ
        on(2207, ignore);  // SetLayerVisibility
    }
};

class ManagerDisplayService final : public Service {
public:
    ManagerDisplayService() : Service("vi:IManagerDisplayService") {
        on(6000, ignore);  // AddToLayerStack
        on(6002, ignore);  // SetLayerVisibility
    }
};

class ApplicationDisplayService final : public Service {
public:
    explicit ApplicationDisplayService(std::shared_ptr<ViState> state)
        : Service("vi:IApplicationDisplayService"), state_(std::move(state)) {
        on(100, [this](Request&, Response& resp) { resp.push_object(std::make_shared<HosBinderDriver>(state_)); });
        on(101, [](Request&, Response& resp) { resp.push_object(std::make_shared<SystemDisplayService>()); });
        on(102, [](Request&, Response& resp) { resp.push_object(std::make_shared<ManagerDisplayService>()); });
        on(103, [this](Request&, Response& resp) {  // GetIndirectDisplayTransactionService
            resp.push_object(std::make_shared<HosBinderDriver>(state_));
        });
        on(1010, [](Request&, Response& resp) {  // OpenDisplay(DisplayName) -> display id
            resp.push<uint64_t>(kDefaultDisplayId);
        });
        on(1011, [](Request&, Response& resp) { resp.push<uint64_t>(kDefaultDisplayId); });  // OpenDefaultDisplay
        on(1020, ignore);                                                                  // CloseDisplay
        on(1101, ignore);                                                                  // SetDisplayEnabled
        on(1102, [](Request&, Response& resp) {  // GetDisplayResolution(display id) -> width, height
            const auto [w, h] = display_size();
            resp.push<int64_t>(w);
            resp.push<int64_t>(h);
        });
        on(2020, [](Request& req, Response& resp) {  // OpenLayer(DisplayName, layer id, aruid, pid) -> parcel size
            const std::vector<uint8_t> parcel = native_window_parcel(kBinderId);
            if (!req.write_out(0, parcel.data(), parcel.size())) {
                resp.result = rt::kernel_result::kInvalidPointer;
                return;
            }
            resp.push<uint64_t>(parcel.size());
        });
        on(2021, ignore);  // CloseLayer
        on(2101, ignore);  // SetLayerScalingMode
        on(2102, [](Request& req, Response& resp) {  // ConvertScalingMode(source) -> destination
            // Source: 0 None, 1 Freeze, 2 ScaleToWindow, 3 ScaleAndCrop, 4 PreserveAspectRatio.
            // Destination: 0 Freeze, 1 ScaleToWindow, 2 ScaleAndCrop, 3 None, 4 PreserveAspectRatio.
            static constexpr uint64_t kMap[] = {3, 0, 1, 2, 4};
            const uint32_t source = req.pop<uint32_t>();
            if (source >= std::size(kMap)) {
                resp.unimplemented = true;  // no measured answer for other values
                return;
            }
            resp.push<uint64_t>(kMap[source]);
        });
        on(5202, [this](Request& req, Response& resp) {  // GetDisplayVsyncEvent(display id)
            copy_event(req, resp, state_->vsync);
        });
    }

private:
    std::shared_ptr<ViState> state_;
};

// vi:u, vi:s and vi:m differ only in the command number of GetDisplayService.
class RootService final : public Service {
public:
    RootService(const char* name, uint32_t command, std::shared_ptr<ViState> state)
        : Service(name), state_(std::move(state)) {
        on(command, [this](Request&, Response& resp) {  // GetDisplayService(policy)
            resp.push_object(std::make_shared<ApplicationDisplayService>(state_));
        });
    }

private:
    std::shared_ptr<ViState> state_;
};

}  // namespace

void register_vi(ServiceManager& sm) {
    auto state = std::make_shared<ViState>();
    sm.register_service("vi:u", [state] { return std::make_shared<RootService>("vi:u", 0, state); });
    sm.register_service("vi:s", [state] { return std::make_shared<RootService>("vi:s", 1, state); });
    sm.register_service("vi:m", [state] { return std::make_shared<RootService>("vi:m", 2, state); });
}

}  // namespace lgp::hle
