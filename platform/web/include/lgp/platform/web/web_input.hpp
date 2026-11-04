// Browser controllers for hid (P5.1): the Gamepad API and keyboard events as
// IInput, for the Emscripten build. Same slot layout as the desktop: gamepad
// k (in navigator.getGamepads() order, "standard" mapping only) is slot k,
// the keyboard adds to slot 0, which counts as connected while the keyboard
// is enabled.
//
// Threads (ADR-0002, PROXY_TO_PTHREAD): key events are handled on the main
// browser thread and only set flags; poll() runs on hid's sampling thread
// and reads the gamepads through Emscripten's synchronously proxied calls.
// The Gamepad API has no button labels: positions follow the Xbox layout
// (bottom = A), except for pads whose id names Nintendo (bottom = B, as
// printed). It has no motion data either, so a single Joy-Con slot gets the
// synthetic throw (right stick click or Space).
#pragma once

#include <array>
#include <atomic>
#include <memory>
#include <vector>

#include "lgp/platform/pad_input.hpp"

namespace lgp::platform::web {

struct WebInputOptions {
    std::vector<MapperConfig> slots = default_slots();
    KeyboardMap keys = KeyboardMap::defaults();
    bool keyboard = true;
    // CSS selector of the element that receives key events ("#canvas"
    // needs a tabindex and the focus); nullptr: the whole window.
    const char* key_target = nullptr;
};

class WebInput final : public PadInput {
public:
    static std::unique_ptr<WebInput> create(const WebInputOptions& options);
    ~WebInput() override;

    // Key events seen so far (tests and the page's diagnostics).
    uint64_t key_events() const { return key_events_.load(); }

    // Called by the key callbacks on the main browser thread.
    void key(const char* dom_code, bool down);
    void clear_keys();

protected:
    void sample(std::vector<HostPad>& pads) override;
    void rumble(size_t slot, float low, float high) override;

private:
    explicit WebInput(const WebInputOptions& options);

    WebInputOptions options_;
    std::array<std::atomic<bool>, 256> keys_{};
    std::atomic<uint64_t> key_events_{0};
    std::vector<int> pad_index_;  // slot -> navigator.getGamepads() index, -1 none
    std::vector<float> sent_;     // last rumble per slot (low, high)
    std::vector<double> sent_ms_;
};

}  // namespace lgp::platform::web
