// Software keyboard in the SDL window (sdl_text_input.hpp), driven with
// synthetic SDL events and no window: typing, length limit, Backspace over a
// multi-byte character, Enter with a too-short text, Escape, key releases
// left to the controllers, closing the window. With SDL's dummy video driver
// also the dialog window: it opens on top, closing it cancels, the game
// window does not see its events, keys pressed in the game window do not
// reach it, and a request that follows before the next service() replaces
// the dialog (no second window, the game title restored). Messages (the
// error applet) show code and wrapped text, ignore typing and close on Enter.
#include <SDL3/SDL.h>

#include <chrono>
#include <cstdio>
#include <future>
#include <string>
#include <thread>

#include "../runtime/check.hpp"
#include "lgp/platform/desktop/sdl_text_input.hpp"

using namespace lgp::platform;

namespace {

SDL_Event text_event(const char* s) {
    SDL_Event e{};
    e.type = SDL_EVENT_TEXT_INPUT;
    e.text.text = s;
    return e;
}

SDL_Event key_event(SDL_Keycode key) {
    SDL_Event e{};
    e.type = SDL_EVENT_KEY_DOWN;
    e.key.key = key;
    e.key.down = true;
    return e;
}

// Starts ask() on another thread and waits until the request is visible.
std::future<std::pair<bool, std::string>> start(desktop::SdlTextInput& in, const TextRequest& r) {
    auto f = std::async(std::launch::async, [&in, r] {
        std::string text;
        const bool ok = in.ask(r, &text);
        return std::make_pair(ok, text);
    });
    for (int i = 0; i < 2000 && !in.active(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    in.service();
    return f;
}

void test_message() {
    desktop::SdlTextInput in(nullptr);
    MessageRequest m{"application error 15",
                     "Communication with the server was interrupted. Please try again later, thank you."};
    auto f = std::async(std::launch::async, [&in, m] { in.show_message(m); });
    for (int i = 0; i < 2000 && !in.active(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    in.service();
    CHECK(in.active());
    const auto ls = in.lines();
    CHECK(ls.size() >= 4 && ls[0] == "application error 15");
    CHECK(ls.size() >= 3 && ls[1].rfind("Communication", 0) == 0 && ls[1].size() <= 48);
    CHECK(ls.size() >= 3 && ls[2].find("thank you.") != std::string::npos);
    CHECK(in.title().find("interrupted") != std::string::npos);
    CHECK(in.handle_event(text_event("x")));  // swallowed, not typed
    CHECK(in.active());
    CHECK(in.handle_event(key_event(SDLK_RETURN)));
    CHECK(f.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    CHECK(!in.active());
}

void test_type_and_confirm() {
    desktop::SdlTextInput in(nullptr);
    CHECK(!in.handle_event(text_event("x")));  // nothing asked: not for the keyboard
    TextRequest r;
    r.header = "Name";
    r.initial = "R";
    r.min_length = 2;
    r.max_length = 4;
    auto f = start(in, r);
    CHECK(in.active());
    CHECK(in.handle_event(key_event(SDLK_RETURN)));  // "R" is too short
    CHECK(in.active());
    CHECK(in.title().find("not allowed") != std::string::npos);
    CHECK(in.handle_event(text_event("\xC3\xB6")));  // ö
    CHECK(in.handle_event(text_event("abc")));       // would make 5: ignored
    CHECK(in.handle_event(text_event("a")));
    CHECK(in.title().find("R\xC3\xB6" "a_") != std::string::npos);
    CHECK(in.handle_event(key_event(SDLK_BACKSPACE)));
    CHECK(in.handle_event(key_event(SDLK_BACKSPACE)));  // removes both bytes of ö
    CHECK(in.handle_event(text_event("ed")));
    SDL_Event other{};
    other.type = SDL_EVENT_GAMEPAD_ADDED;
    CHECK(!in.handle_event(other));
    CHECK(in.handle_event(key_event(SDLK_RETURN)));
    const auto [ok, text] = f.get();
    CHECK(ok && text == "Red");
    in.service();
    CHECK(!in.active());
}

void test_cancel() {
    desktop::SdlTextInput in(nullptr);
    TextRequest r;
    r.password = true;
    r.initial = "pw";
    auto f = start(in, r);
    CHECK(in.title().find("**_") != std::string::npos);
    CHECK(in.handle_event(key_event(SDLK_ESCAPE)));
    CHECK(!f.get().first);
}

void test_key_up_passes() {
    desktop::SdlTextInput in(nullptr);
    auto f = start(in, TextRequest{});
    SDL_Event up = key_event(SDLK_A);  // the button that opened the keyboard is released
    up.type = SDL_EVENT_KEY_UP;
    up.key.down = false;
    CHECK(!in.handle_event(up));
    CHECK(in.active());
    CHECK(in.handle_event(key_event(SDLK_ESCAPE)));
    CHECK(!f.get().first);
}

void test_window_closed() {
    desktop::SdlTextInput in(nullptr);
    auto f = start(in, TextRequest{});
    in.cancel();
    CHECK(!f.get().first);
    std::string text = "unchanged";
    CHECK(!in.ask(TextRequest{}, &text));  // later requests do not block
    CHECK(text == "unchanged");
}

void test_dialog_window() {
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::printf("  skipped: no dummy video driver (%s)\n", SDL_GetError());
        return;
    }
    SDL_Window* game = SDL_CreateWindow("game", 320, 180, 0);
    CHECK(game != nullptr);
    {
        desktop::SdlTextInput in(game);
        TextRequest r;
        r.header = "Wie hei\xC3\x9Ft du?";
        r.initial = "Red";
        r.max_length = 10;
        auto f = start(in, r);
        SDL_Window* dialog = in.dialog();
        CHECK(dialog != nullptr && dialog != game);
        if (dialog != nullptr)
            CHECK(std::string(SDL_GetWindowTitle(dialog)) == r.header);
        const auto ls = in.lines();
        CHECK(!ls.empty() && ls[0] == r.header);
        CHECK(ls.size() > 1 && ls[1] == "> Red_");
        in.service();  // draws again

        SDL_Event close{};
        close.type = SDL_EVENT_WINDOW_CLOSE_REQUESTED;
        close.window.windowID = SDL_GetWindowID(game);
        CHECK(!in.handle_event(close));  // the game window's close: not the keyboard's
        CHECK(in.active());
        if (dialog != nullptr) {
            close.window.windowID = SDL_GetWindowID(dialog);
            CHECK(in.handle_event(close));  // closing the dialog cancels
        } else {
            in.handle_event(key_event(SDLK_ESCAPE));  // no dialog: end the request anyway
        }
        CHECK(!f.get().first);
        in.service();
        CHECK(in.dialog() == nullptr);
        CHECK(std::string(SDL_GetWindowTitle(game)) == "game");

        // Two requests in a row, the second before the main thread serviced
        // the end of the first (a rejected name asks again).
        auto f1 = start(in, r);
        SDL_Event enter = key_event(SDLK_RETURN);
        enter.key.windowID = SDL_GetWindowID(game);
        CHECK(in.handle_event(enter));  // pressed in the game window: swallowed
        CHECK(in.active());
        enter.key.windowID = in.dialog() != nullptr ? SDL_GetWindowID(in.dialog()) : 0;
        CHECK(in.handle_event(enter));
        CHECK(f1.get().first);
        auto f2 = start(in, r);  // services only now
        int windows = 0;
        SDL_free(SDL_GetWindows(&windows));
        CHECK_EQ(windows, in.dialog() != nullptr ? 2 : 1);
        SDL_Event esc = key_event(SDLK_ESCAPE);
        esc.key.windowID = in.dialog() != nullptr ? SDL_GetWindowID(in.dialog()) : 0;
        CHECK(in.handle_event(esc));
        CHECK(!f2.get().first);
        in.service();
        CHECK(in.dialog() == nullptr);
        CHECK(std::string(SDL_GetWindowTitle(game)) == "game");
    }
    SDL_DestroyWindow(game);
    SDL_Quit();
}

}  // namespace

// Host-drawn (iOS, Android: the app menu draws it): no events taken, the
// request read with prompt(), a too-short answer refused with a note, a
// new serial for every request.
void test_host_drawn() {
    desktop::SdlTextInput in(nullptr);
    in.set_host_drawn(true);
    TextRequest r;
    r.header = "Name";
    r.min_length = 2;
    r.max_length = 4;
    auto f = start(in, r);
    desktop::SdlTextInput::Prompt p;
    CHECK(in.prompt(&p) && !p.message && p.request.header == "Name");
    const uint64_t first = p.serial;
    CHECK(!in.handle_event(text_event("x")));        // the host's UI types
    CHECK(!in.handle_event(key_event(SDLK_RETURN)));
    std::string note;
    CHECK(!in.answer(true, "R", &note) && !note.empty() && in.active());
    CHECK(in.answer(true, "Red", &note));
    const auto [ok, text] = f.get();
    CHECK(ok && text == "Red");
    CHECK(!in.prompt(&p));
    auto g = start(in, r);
    CHECK(in.prompt(&p) && p.serial != first);
    CHECK(in.answer(false, "", nullptr));
    CHECK(!g.get().first);
    // A message: OK only.
    auto h = std::async(std::launch::async, [&in] { in.show_message(MessageRequest{"error 2110-1100", "Offline"}); });
    for (int i = 0; i < 2000 && !in.active(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(in.prompt(&p) && p.message && p.message_request.code == "error 2110-1100");
    CHECK(in.answer(true, "", nullptr));
    h.get();
}

int main() {
    RUN(test_host_drawn);
    RUN(test_message);
    RUN(test_type_and_confirm);
    RUN(test_cancel);
    RUN(test_key_up_passes);
    RUN(test_window_closed);
    RUN(test_dialog_window);
    return finish();
}
