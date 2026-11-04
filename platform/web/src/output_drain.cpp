#include "lgp/platform/web/output_drain.hpp"

#include <emscripten.h>
#include <emscripten/threading.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>

namespace lgp::platform::web {

namespace {

std::atomic<int32_t> g_drained{0};
constexpr const char* kMarker = "\x01lgp-output-drain ";

}  // namespace

void drain_output() {
    std::fflush(stdout);
    std::fflush(stderr);
    if (emscripten_is_main_runtime_thread())
        return;  // prints are synchronous there
    // Main thread: Module.print recognises the marker, sets the flag and
    // drops the line.
    MAIN_THREAD_EM_ASM({
        const prefix = UTF8ToString($0);
        const orig = Module['print'];
        if (typeof orig !== 'function' || orig.lgpOutputDrain) return;
        const wrapped = (...args) => {
            const t = args[0];
            if (typeof t === 'string' && t.startsWith(prefix)) {
                const addr = Number(t.slice(prefix.length));
                Atomics.store((growMemViews(), HEAP32), addr >> 2, 1);
                Atomics.notify((growMemViews(), HEAP32), addr >> 2);
                return;
            }
            orig(...args);
        };
        wrapped.lgpOutputDrain = true;
        Module['print'] = wrapped;
    }, kMarker);
    g_drained.store(0);
    EM_ASM({ out(UTF8ToString($0) + $1); }, kMarker, &g_drained);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (g_drained.load() == 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

}  // namespace lgp::platform::web
