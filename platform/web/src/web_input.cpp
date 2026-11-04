// Browser gamepads and keyboard as IInput, see lgp/platform/web/web_input.hpp.
#include "lgp/platform/web/web_input.hpp"

#include <emscripten/em_asm.h>
#include <emscripten/emscripten.h>
#include <emscripten/html5.h>

#include <cstring>

namespace lgp::platform::web {

namespace {

// W3C "standard" gamepad mapping.
enum : int {
    kBottom = 0, kRight = 1, kLeft = 2, kTop = 3, kLB = 4, kRB = 5, kLT = 6, kRT = 7,
    kBack = 8, kStart = 9, kL3 = 10, kR3 = 11, kUp = 12, kDown = 13, kDLeft = 14, kDRight = 15,
};

// A text field of the page has focus (the software keyboard dialog,
// web_text_input.hpp): typing is for it, not for the controllers.
bool typing_into_page() {
    return EM_ASM_INT({
        const a = document.activeElement;
        return a && (a.tagName === 'INPUT' || a.tagName === 'TEXTAREA' || a.isContentEditable) ? 1 : 0;
    }) != 0;
}

bool on_key(int type, const EmscriptenKeyboardEvent* e, void* user) {
    auto* in = static_cast<WebInput*>(user);
    if (typing_into_page()) {
        in->clear_keys();
        return false;
    }
    if (e->repeat)
        return hid_usage_from_dom_code(e->code) != 0;
    in->key(e->code, type == EMSCRIPTEN_EVENT_KEYDOWN);
    // Keep the browser from scrolling with the arrow keys and Space.
    return hid_usage_from_dom_code(e->code) != 0 && !e->ctrlKey && !e->metaKey && !e->altKey;
}

bool on_blur(int, const EmscriptenFocusEvent*, void* user) {
    static_cast<WebInput*>(user)->clear_keys();
    return false;
}

bool is_nintendo(const char* id) {
    return std::strstr(id, "Nintendo") != nullptr || std::strstr(id, "057e") != nullptr ||
           std::strstr(id, "Pro Controller") != nullptr || std::strstr(id, "Joy-Con") != nullptr;
}

const char* key_target(const WebInputOptions& o) {
    return o.key_target != nullptr ? o.key_target : EMSCRIPTEN_EVENT_TARGET_WINDOW;
}

bool single_joycon(const MapperConfig& c) {
    return c.style == ControllerStyle::JoyconLeft || c.style == ControllerStyle::JoyconRight;
}

}  // namespace

std::unique_ptr<WebInput> WebInput::create(const WebInputOptions& options) {
    std::unique_ptr<WebInput> in(new WebInput(options));
    if (options.keyboard) {
        const pthread_t main_thread = EM_CALLBACK_THREAD_CONTEXT_MAIN_RUNTIME_THREAD;
        emscripten_set_keydown_callback_on_thread(key_target(options), in.get(), true, on_key, main_thread);
        emscripten_set_keyup_callback_on_thread(key_target(options), in.get(), true, on_key, main_thread);
        emscripten_set_blur_callback_on_thread(EMSCRIPTEN_EVENT_TARGET_WINDOW, in.get(), true, on_blur, main_thread);
    }
    return in;
}

WebInput::WebInput(const WebInputOptions& options)
    : PadInput(options.slots), options_(options), pad_index_(options.slots.size(), -1),
      sent_(options.slots.size() * 2, 0.0f), sent_ms_(options.slots.size(), 0.0) {}

WebInput::~WebInput() {
    if (options_.keyboard) {
        const pthread_t main_thread = EM_CALLBACK_THREAD_CONTEXT_MAIN_RUNTIME_THREAD;
        emscripten_set_keydown_callback_on_thread(key_target(options_), nullptr, true, nullptr, main_thread);
        emscripten_set_keyup_callback_on_thread(key_target(options_), nullptr, true, nullptr, main_thread);
        emscripten_set_blur_callback_on_thread(EMSCRIPTEN_EVENT_TARGET_WINDOW, nullptr, true, nullptr, main_thread);
    }
}

void WebInput::key(const char* dom_code, bool down) {
    const uint8_t usage = hid_usage_from_dom_code(dom_code);
    if (usage != 0)
        keys_[usage].store(down);
    key_events_.fetch_add(1);
}

void WebInput::clear_keys() {
    for (auto& k : keys_)
        k.store(false);
}

void WebInput::sample(std::vector<HostPad>& pads) {
    std::fill(pad_index_.begin(), pad_index_.end(), -1);
    if (emscripten_sample_gamepad_data() == EMSCRIPTEN_RESULT_SUCCESS) {
        const int count = emscripten_get_num_gamepads();
        size_t slot = 0;
        for (int i = 0; i < count && slot < pads.size(); ++i) {
            EmscriptenGamepadEvent g;
            if (emscripten_get_gamepad_status(i, &g) != EMSCRIPTEN_RESULT_SUCCESS || !g.connected ||
                std::strcmp(g.mapping, "standard") != 0 || g.numButtons < 16 || g.numAxes < 4)
                continue;
            HostPad& p = pads[slot];
            const bool nintendo = is_nintendo(g.id);
            const auto pressed = [&g](int b) { return g.digitalButton[b]; };
            const auto bit = [&](int b, uint64_t v) {
                if (pressed(b))
                    p.buttons |= v;
            };
            bit(kBottom, nintendo ? kButtonB : kButtonA);
            bit(kRight, nintendo ? kButtonA : kButtonB);
            bit(kLeft, nintendo ? kButtonY : kButtonX);
            bit(kTop, nintendo ? kButtonX : kButtonY);
            bit(kLB, kButtonL);
            bit(kRB, kButtonR);
            bit(kBack, kButtonMinus);
            bit(kStart, kButtonPlus);
            bit(kL3, kButtonStickL);
            bit(kR3, kButtonStickR);
            bit(kUp, kButtonUp);
            bit(kDown, kButtonDown);
            bit(kDLeft, kButtonLeft);
            bit(kDRight, kButtonRight);
            p.zl = static_cast<float>(g.analogButton[kLT]);
            p.zr = static_cast<float>(g.analogButton[kRT]);
            p.left_x = static_cast<float>(g.axis[0]);
            p.left_y = static_cast<float>(-g.axis[1]);  // the API: down positive
            p.right_x = static_cast<float>(g.axis[2]);
            p.right_y = static_cast<float>(-g.axis[3]);
            p.connected = true;
            if (single_joycon(options_.slots[slot]) && (p.buttons & kButtonStickR)) {
                p.buttons &= ~kButtonStickR;
                p.throw_gesture = true;
            }
            pad_index_[slot] = i;
            ++slot;
        }
    }
    if (options_.keyboard && !pads.empty()) {
        bool down[256];
        for (size_t i = 0; i < 256; ++i)
            down[i] = keys_[i].load();
        options_.keys.apply(down, &pads[0]);
        pads[0].connected = true;
    }
}

void WebInput::rumble(size_t slot, float low, float high) {
    // Under PadInput's lock, like sample(): pad_index_ is from the last poll.
    if (slot >= pad_index_.size() || pad_index_[slot] < 0)
        return;
    const double now = emscripten_get_now();
    const bool on = low > 0 || high > 0;
    if (sent_[slot * 2] == low && sent_[slot * 2 + 1] == high && !(on && now - sent_ms_[slot] > 150))
        return;
    sent_[slot * 2] = low;
    sent_[slot * 2 + 1] = high;
    sent_ms_[slot] = now;
    // Chrome's "dual-rumble": strong = low-frequency motor, weak = high.
    MAIN_THREAD_ASYNC_EM_ASM(
        {
            const pads = navigator.getGamepads ? navigator.getGamepads() : [];
            const pad = pads[$0];
            if (pad && pad.vibrationActuator && pad.vibrationActuator.playEffect)
                pad.vibrationActuator.playEffect("dual-rumble", {duration: $1 > 0 || $2 > 0 ? 250 : 0,
                                                                 strongMagnitude: $1, weakMagnitude: $2})
                    .catch(() => {});
        },
        pad_index_[slot], low, high);
}

}  // namespace lgp::platform::web
