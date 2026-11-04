// P5.1 browser test: the web input backend sees keys pressed in the page.
// The page prints "input ready"; the test runner then holds Z, ArrowUp and
// D, and the probe waits until slot 0 shows A, Up and the left stick to the
// right (main thread events -> hid's sampling thread, through the backend).
#include <chrono>
#include <cstdio>
#include <thread>

#include "lgp/platform/web/web_input.hpp"

using namespace lgp::platform;

int main() {
    auto in = web::WebInput::create({});
    std::printf("input ready\n");
    std::fflush(stdout);
    for (int i = 0; i < 3000; ++i) {
        in->poll();
        const ControllerState s = in->controller(0);
        if ((s.buttons & kButtonA) && (s.buttons & kButtonUp) && s.left.x > 30000 &&
            s.style == ControllerStyle::Handheld) {
            std::printf("input: ok after %llu key events\n", static_cast<unsigned long long>(in->key_events()));
            return 0;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::printf("input: timeout, %llu key events\n", static_cast<unsigned long long>(in->key_events()));
    return 1;
}
