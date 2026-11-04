// Software keyboard for the SDL window (lgp/platform/text_input.hpp): while
// the game waits for text, a small dialog window on top of the game shows
// the request, the text typed so far and the keys (Enter confirms, Escape or
// closing the dialog cancels, Backspace deletes). SDL's text input (IME
// included) delivers the characters; the dialog draws with SDL's built-in
// 8x8 debug font, which has ASCII only (other characters show as '?', the
// text itself keeps them). Without a dialog window (no renderer, tests) the
// game window's title shows the same.
//
// show_message() (the error applet) uses the same dialog without a text
// field: the code, the message wrapped to the dialog's width, and Enter or
// Escape as OK.
//
// Threads as for SdlInput: the guest thread blocks in ask(); the main thread
// passes every SDL event to handle_event() (true = it was for the keyboard,
// keep it from the controllers and the game window) and calls service() once
// per loop, which opens, draws and closes the dialog.
#pragma once

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "lgp/platform/text_input.hpp"

struct SDL_Window;
struct SDL_Renderer;
union SDL_Event;

namespace lgp::platform::desktop {

class SdlTextInput final : public ITextInput {
public:
    // window may be nullptr (tests): then no title and no SDL text input.
    explicit SdlTextInput(SDL_Window* window) : window_(window) {}

    bool ask(const TextRequest& request, std::string* text) override;
    void show_message(const MessageRequest& message) override;

    // Main thread.
    bool handle_event(const SDL_Event& event);
    void service();
    // Window closed: cancels the waiting request and answers every later one
    // with cancel, so the guest thread does not block the shutdown.
    void cancel();
    // Whether a request waits for the user (tests).
    bool active() const;

    // Drawn by the host's own UI instead of a dialog window (iOS and Android
    // have one full-screen window; a second one never shows): service() opens
    // nothing, handle_event() leaves every event to the host, which reads the
    // request with prompt() and answers it with answer().
    void set_host_drawn(bool host_drawn) { host_drawn_ = host_drawn; }
    bool host_drawn() const { return host_drawn_; }
    // Platforms without separate windows (iOS, Android).
    static bool needs_host_drawn();
    struct Prompt {
        uint64_t serial = 0;  // new for every request (also a repeated one)
        bool message = false;
        TextRequest request;    // message: header = code
        MessageRequest message_request;
        std::string note;  // why the last answer was not taken
    };
    // The request waiting for an answer (false: none).
    bool prompt(Prompt* out) const;
    // ok: the text the user confirmed (checked against the length limits:
    // false and *note when it does not fit, the request stays open); !ok:
    // cancelled. Messages take ok only.
    bool answer(bool ok, const std::string& text, std::string* note);
    // The request in one line, as in a title bar (tests).
    std::string title() const;
    // The lines the dialog shows (tests).
    std::vector<std::string> lines() const;
    // The dialog window while one is open (main thread, tests).
    SDL_Window* dialog() const { return dialog_; }

private:
    enum class State { Idle, Pending, Typing, Done };
    void finish(bool ok);  // under mutex_
    std::string title_locked() const;
    std::vector<std::string> lines_locked() const;
    void open_dialog();   // main thread, under mutex_
    void close_dialog();  // main thread, under mutex_
    void draw();          // main thread, under mutex_

    SDL_Window* window_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    State state_ = State::Idle;
    TextRequest request_;
    std::string text_;
    std::string note_;  // why Enter did nothing
    bool accepted_ = false;
    bool closed_ = false;   // cancel() called
    bool started_ = false;       // dialog/title shown (main thread only)
    bool text_started_ = false;  // SDL text input on (main thread only)
    bool message_mode_ = false;  // the request is a message, not a text
    MessageRequest message_;
    std::string saved_title_;
    SDL_Window* dialog_ = nullptr;  // main thread only
    SDL_Renderer* renderer_ = nullptr;
    uint32_t dialog_id_ = 0;  // read by handle_event under mutex_
    bool host_drawn_ = false;
    uint64_t serial_ = 0;  // requests so far
};

}  // namespace lgp::platform::desktop
