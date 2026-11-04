// P7: LifecycleRelay (SdlWindow's background/foreground handler). No SDL.
#include <vector>

#include "../runtime/check.hpp"
#include "lgp/platform/desktop/lifecycle.hpp"

using lgp::platform::desktop::LifecycleRelay;

namespace {

void test_once_per_change() {
    LifecycleRelay relay;
    std::vector<bool> calls;
    relay.set_handler([&](bool bg) { calls.push_back(bg); });
    CHECK(calls.empty());  // in the foreground: nothing at once
    relay.event(true);
    relay.event(true);
    relay.event(false);
    relay.event(false);
    CHECK(calls == (std::vector<bool>{true, false}));
}

// The app went to the background before anyone listened (between
// SdlWindow::create and set_lifecycle_handler): not lost.
void test_early_background() {
    LifecycleRelay relay;
    relay.event(true);
    std::vector<bool> calls;
    relay.set_handler([&](bool bg) { calls.push_back(bg); });
    CHECK(calls == (std::vector<bool>{true}));
    relay.event(false);
    CHECK(calls == (std::vector<bool>{true, false}));
    // Back and forth before a listener: the foreground needs no call.
    LifecycleRelay other;
    other.event(true);
    other.event(false);
    std::vector<bool> none;
    other.set_handler([&](bool bg) { none.push_back(bg); });
    CHECK(none.empty());
}

// Detached (shutdown): later events reach nothing.
void test_detach() {
    LifecycleRelay relay;
    int calls = 0;
    relay.set_handler([&](bool) { ++calls; });
    relay.set_handler(nullptr);
    relay.event(true);
    relay.event(false);
    CHECK(calls == 0);
}

}  // namespace

int main() {
    RUN(test_once_per_change);
    RUN(test_early_background);
    RUN(test_detach);
    return finish();
}
