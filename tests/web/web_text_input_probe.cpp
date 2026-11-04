// Browser test of the software keyboard dialog (web_text_input.hpp): the
// probe asks for text with "Red" pre-filled; once the dialog is open with the
// text field focused it prints "keyboard ready" and the runner types x and
// presses Enter (printed earlier, the keys could reach the page before the
// dialog and count as controller input). The answer must be "x" (the
// pre-filled text is selected, typing replaces it), and the keyboard
// backend for the controllers must not have seen those keys.
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#include <emscripten/em_asm.h>

#include "lgp/platform/web/web_input.hpp"
#include "lgp/platform/web/web_text_input.hpp"

using namespace lgp::platform;

int main() {
    auto pads = web::WebInput::create({});
    web::WebTextInput keyboard;
    TextRequest r;
    r.header = "Name?";
    r.initial = "Red";
    r.max_length = 8;
    std::string text;
    bool ok = false;
    std::thread asking([&] { ok = keyboard.ask(r, &text); });
    while (!MAIN_THREAD_EM_ASM_INT({
        const d = document.querySelector('dialog.lgp-keyboard');
        return d && d.open && document.activeElement && document.activeElement.tagName === 'INPUT' ? 1 : 0;
    }))
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    std::printf("keyboard ready\n");
    std::fflush(stdout);
    asking.join();
    const auto key_events = static_cast<unsigned long long>(pads->key_events());
    if (ok && text == "x" && key_events == 0) {
        std::printf("text input: ok\n");
        return 0;
    }
    std::printf("text input: %s \"%s\", %llu key events reached the controllers\n", ok ? "answered" : "cancelled",
                text.c_str(), key_events);
    return 1;
}
