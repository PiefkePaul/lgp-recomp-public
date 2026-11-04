// prepare_host() of boot executables in the browser (lgp/boot/host.hpp):
// the page passes OPFS paths (--exefs /opfs/lgp/data/exefs ...), so OPFS has
// to be mounted before main() reads them. main() runs on a pthread
// (PROXY_TO_PTHREAD), where mounting may block. host_devices(): the page's
// gamepads and keyboard for hid, Web Audio for audout, WebCodecs for the
// movie:: HLE (without VideoDecoder its decoders fail to open and the HLE
// plays black pictures; checking here would start the video worker for
// programs without videos), a page dialog for the software keyboard.
//
// finish_host(): a pthread prints by posting a message to the main thread.
// When main() returns, the exit is proxied to the main thread through
// Emscripten's call queue, which is signalled via Atomics.waitAsync and can
// overtake print messages still queued from this thread; once the runtime is
// exiting, the main thread no longer handles them (seen in Safari: the boot
// summary went missing). So the thread sends a marker through the print
// channel itself and waits until the main thread has handled it.
#include <emscripten.h>
#include <emscripten/threading.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <thread>

#include "lgp/boot/host.hpp"
#include "lgp/platform/web/opfs.hpp"
#include "lgp/platform/web/web_audio.hpp"
#include "lgp/platform/web/web_input.hpp"
#include "lgp/platform/web/web_text_input.hpp"
#include "lgp/platform/web/web_video.hpp"

namespace lgp::boot {

namespace {

std::atomic<int32_t> g_output_drained{0};
constexpr const char* kDrainMarker = "\x01lgp-output-drained ";

// Main thread: print messages from workers go through Module.print; it
// recognises the marker, sets the flag and drops the line.
void install_drain_marker() {
    MAIN_THREAD_EM_ASM({
        const prefix = UTF8ToString($0);
        const orig = Module['print'];
        if (typeof orig !== 'function' || orig.lgpDrain) return;
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
        wrapped.lgpDrain = true;
        Module['print'] = wrapped;
    }, kDrainMarker);
}

}  // namespace

bool prepare_host(std::string* error) {
    install_drain_marker();
    return platform::web::mount_opfs(error);
}

void finish_host() {
    std::fflush(stdout);
    std::fflush(stderr);
    if (emscripten_is_main_runtime_thread())
        return;  // prints are synchronous there
    install_drain_marker();  // usage errors exit before prepare_host()
    g_output_drained.store(0);
    EM_ASM({ out(UTF8ToString($0) + $1); }, kDrainMarker, &g_output_drained);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (g_output_drained.load() == 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

uint32_t time_stats_tick_us() { return 1000; }

HostDevices host_devices(const std::vector<platform::MapperConfig>& slots) {
    static const std::unique_ptr<platform::web::WebInput> input = [&slots] {
        platform::web::WebInputOptions options;
        options.slots = slots;
        return platform::web::WebInput::create(options);
    }();
    static platform::web::WebAudio audio;
    static platform::web::WebVideo video;
    static platform::web::WebTextInput text;
    HostDevices d;
    d.input = input.get();
    d.audio = platform::web::WebAudio::available() ? &audio : nullptr;
    d.video = &video;
    d.text = &text;
    return d;
}

}  // namespace lgp::boot
