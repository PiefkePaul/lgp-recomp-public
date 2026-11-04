// App lifecycle relay for SdlWindow (P7, phones and tablets): passes
// background/foreground changes to one handler, once per change.
#pragma once

#include <functional>
#include <mutex>

namespace lgp::platform::desktop {

class LifecycleRelay {
public:
    // A handler set while the app is already in the background (the event
    // came before anyone listened) is called with true at once. nullptr
    // detaches; it returns once a handler call in progress has finished.
    void set_handler(std::function<void(bool background)> handler) {
        std::lock_guard lock(mutex_);
        handler_ = std::move(handler);
        if (handler_ && background_)
            handler_(true);
    }
    void event(bool background) {
        std::lock_guard lock(mutex_);
        if (background == background_)
            return;
        background_ = background;
        if (handler_)
            handler_(background);
    }

private:
    std::mutex mutex_;
    std::function<void(bool)> handler_;
    bool background_ = false;
};

}  // namespace lgp::platform::desktop
