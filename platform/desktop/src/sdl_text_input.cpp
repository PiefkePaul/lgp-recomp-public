#include "lgp/platform/desktop/sdl_text_input.hpp"

#include <SDL3/SDL.h>

#include <algorithm>

#include "lgp/platform/text_input_backends.hpp"
#include "lgp/platform/ui_text.hpp"

namespace lgp::platform::desktop {

namespace {

constexpr int kScale = 2;  // debug font 8x8 -> 16x16 pixels
constexpr int kColumns = 48;
constexpr int kDialogWidth = (kColumns + 2) * 8 * kScale;
constexpr int kMinDialogLines = 7;
constexpr int kLineHeight = 12;

// The debug font has ASCII only: German letters as ae/oe/ue/ss, other
// characters as '?'. Lines longer than the dialog end in "..." when `cut`.
std::string ascii(const std::string& s, bool cut = true) {
    std::string out;
    for (size_t i = 0; i < s.size();) {
        const auto b = static_cast<uint8_t>(s[i]);
        if (b < 0x80) {
            out += b >= 0x20 ? char(b) : ' ';
            ++i;
            continue;
        }
        const size_t n = b >= 0xF0 ? 4 : b >= 0xE0 ? 3 : 2;
        uint32_t cp = n == 2 ? b & 0x1F : n == 3 ? b & 0x0F : b & 0x07;
        for (size_t k = 1; k < n && i + k < s.size(); ++k)
            cp = (cp << 6) | (static_cast<uint8_t>(s[i + k]) & 0x3F);
        i += n;
        switch (cp) {
        case 0xE4: out += "ae"; break;
        case 0xF6: out += "oe"; break;
        case 0xFC: out += "ue"; break;
        case 0xC4: out += "Ae"; break;
        case 0xD6: out += "Oe"; break;
        case 0xDC: out += "Ue"; break;
        case 0xDF: out += "ss"; break;
        case 0xE9: case 0xE8: out += 'e'; break;
        default: out += '?'; break;
        }
    }
    return cut && out.size() > size_t(kColumns) ? out.substr(0, kColumns - 3) + "..." : out;
}

// Wraps text at spaces to lines of at most kColumns ASCII characters.
std::vector<std::string> wrap(const std::string& text) {
    std::vector<std::string> out;
    std::string line;
    const std::string a = ascii(text, false);
    size_t i = 0;
    while (i < a.size()) {
        size_t j = a.find(' ', i);
        if (j == std::string::npos)
            j = a.size();
        std::string word = a.substr(i, j - i);
        i = j + 1;
        while (word.size() > size_t(kColumns)) {  // longer than a line: split
            if (!line.empty())
                out.push_back(line), line.clear();
            out.push_back(word.substr(0, kColumns));
            word.erase(0, kColumns);
        }
        if (!line.empty() && line.size() + 1 + word.size() > size_t(kColumns))
            out.push_back(line), line.clear();
        line += (line.empty() ? "" : " ") + word;
    }
    if (!line.empty())
        out.push_back(line);
    return out;
}

void pop_utf8(std::string* s) {
    while (!s->empty()) {
        const auto b = static_cast<uint8_t>(s->back());
        s->pop_back();
        if ((b & 0xC0) != 0x80)
            break;
    }
}

}  // namespace

bool SdlTextInput::ask(const TextRequest& request, std::string* text) {
    std::unique_lock lock(mutex_);
    cv_.wait(lock, [&] { return state_ == State::Idle; });  // one request at a time
    if (closed_)
        return false;
    request_ = request;
    text_ = request.initial;
    note_ = request.message;
    ++serial_;
    state_ = State::Pending;
    cv_.wait(lock, [&] { return state_ == State::Done; });
    const bool ok = accepted_;
    if (ok)
        *text = text_;
    state_ = State::Idle;
    cv_.notify_all();
    return ok;
}

void SdlTextInput::show_message(const MessageRequest& message) {
    std::unique_lock lock(mutex_);
    cv_.wait(lock, [&] { return state_ == State::Idle; });
    if (closed_)
        return;
    message_ = message;
    message_mode_ = true;
    request_ = TextRequest{};
    request_.header = message.code;
    ++serial_;
    state_ = State::Pending;
    cv_.wait(lock, [&] { return state_ == State::Done; });
    message_mode_ = false;
    state_ = State::Idle;
    cv_.notify_all();
}

bool SdlTextInput::active() const {
    std::lock_guard lock(mutex_);
    return state_ == State::Pending || state_ == State::Typing;
}

std::string SdlTextInput::title() const {
    std::lock_guard lock(mutex_);
    return title_locked();
}

std::string SdlTextInput::title_locked() const {
    if (message_mode_)
        return message_.code + (message_.text.empty() ? "" : ": " + message_.text) + "  (" + tr("Enter OK") + ")";
    std::string t = request_.header.empty() ? std::string(tr("Text")) : request_.header;
    t += ": ";
    t += request_.password ? std::string(utf16_length(text_), '*') : text_;
    t += std::string("_  (") + tr("Enter OK, Esc cancel");
    if (request_.max_length != 0)
        t += ", " + trf("max {}", {std::to_string(request_.max_length)});
    t += ")";
    if (!note_.empty())
        t += "  " + note_;
    return t;
}

std::vector<std::string> SdlTextInput::lines() const {
    std::lock_guard lock(mutex_);
    return lines_locked();
}

std::vector<std::string> SdlTextInput::lines_locked() const {
    std::vector<std::string> out;
    if (message_mode_) {
        out.push_back(message_.code);
        for (std::string& l : wrap(message_.text))
            out.push_back(std::move(l));
        out.push_back("");
        out.push_back(tr("Enter: OK"));
        return out;
    }
    out.push_back(request_.header.empty() ? std::string(tr("Enter text")) : request_.header);
    if (!request_.sub.empty())
        out.push_back(request_.sub);
    out.push_back("> " + (request_.password ? std::string(utf16_length(text_), '*') : text_) + "_");
    out.push_back(note_);
    std::string keys = tr("Enter: OK   Esc: cancel");
    if (request_.max_length != 0)
        keys += "   " + trf("max {}", {std::to_string(request_.max_length)});
    out.push_back(keys);
    return out;
}

void SdlTextInput::open_dialog() {
    // A window of its own on top of the game: the game window's title alone
    // was easy to miss. Without a renderer the title has to do.
    const std::string name = request_.header.empty() ? std::string(tr("Enter text")) : request_.header;
    const int lines = std::max<int>(kMinDialogLines, int(lines_locked().size()) + 2);
    dialog_ = SDL_CreateWindow(name.c_str(), kDialogWidth, lines * kLineHeight * kScale, SDL_WINDOW_ALWAYS_ON_TOP);
    if (dialog_ != nullptr) {
        renderer_ = SDL_CreateRenderer(dialog_, nullptr);
        if (renderer_ == nullptr) {
            SDL_DestroyWindow(dialog_);
            dialog_ = nullptr;
        }
    }
    if (dialog_ != nullptr) {
        dialog_id_ = SDL_GetWindowID(dialog_);
        SDL_RaiseWindow(dialog_);
    }
}

void SdlTextInput::close_dialog() {
    if (renderer_ != nullptr)
        SDL_DestroyRenderer(renderer_);
    if (dialog_ != nullptr)
        SDL_DestroyWindow(dialog_);
    renderer_ = nullptr;
    dialog_ = nullptr;
    dialog_id_ = 0;
}

void SdlTextInput::draw() {
    if (renderer_ == nullptr)
        return;
    SDL_SetRenderScale(renderer_, 1, 1);
    SDL_SetRenderDrawColor(renderer_, 24, 28, 40, 255);
    SDL_RenderClear(renderer_);
    SDL_SetRenderDrawColor(renderer_, 120, 160, 255, 255);
    int w = kDialogWidth, h = 0;
    SDL_GetWindowSize(dialog_, &w, &h);
    const SDL_FRect frame{2, 2, float(w - 4), float(h - 4)};
    SDL_RenderRect(renderer_, &frame);
    SDL_SetRenderScale(renderer_, kScale, kScale);
    const std::vector<std::string> ls = lines_locked();
    float y = 8;
    for (size_t i = 0; i < ls.size(); ++i) {
        const bool input = !message_mode_ && ls[i].rfind("> ", 0) == 0;
        const bool last = i + 1 == ls.size();
        if (input)
            SDL_SetRenderDrawColor(renderer_, 255, 255, 255, 255);
        else if (last)
            SDL_SetRenderDrawColor(renderer_, 150, 150, 160, 255);
        else if (i == 0)
            SDL_SetRenderDrawColor(renderer_, 255, 220, 120, 255);
        else if (message_mode_)
            SDL_SetRenderDrawColor(renderer_, 255, 255, 255, 255);
        else
            SDL_SetRenderDrawColor(renderer_, 255, 140, 120, 255);  // note: why Enter did nothing
        SDL_RenderDebugText(renderer_, 8, y, ascii(ls[i]).c_str());
        y += kLineHeight;
    }
    SDL_RenderPresent(renderer_);
}

void SdlTextInput::finish(bool ok) {
    accepted_ = ok;
    state_ = State::Done;
    cv_.notify_all();
}

bool SdlTextInput::needs_host_drawn() {
#if defined(SDL_PLATFORM_IOS) || defined(SDL_PLATFORM_ANDROID)
    return true;
#else
    return false;
#endif
}

bool SdlTextInput::prompt(Prompt* out) const {
    std::lock_guard lock(mutex_);
    if (state_ != State::Pending && state_ != State::Typing)
        return false;
    out->serial = serial_;
    out->message = message_mode_;
    out->request = request_;
    out->message_request = message_;
    out->note = note_;
    return true;
}

bool SdlTextInput::answer(bool ok, const std::string& text, std::string* note) {
    std::lock_guard lock(mutex_);
    if (state_ != State::Pending && state_ != State::Typing)
        return true;  // nothing waits (cancelled meanwhile)
    if (message_mode_) {
        finish(true);
        return true;
    }
    if (ok && !text_fits(request_, text)) {
        note_ = trf("length {} not allowed", {std::to_string(utf16_length(text))});
        if (request_.min_length != 0)
            note_ += " " + trf("(at least {})", {std::to_string(request_.min_length)});
        if (note != nullptr)
            *note = note_;
        return false;
    }
    if (ok)
        text_ = text;
    finish(ok);
    return true;
}

bool SdlTextInput::handle_event(const SDL_Event& e) {
    std::lock_guard lock(mutex_);
    if (host_drawn_ || (state_ != State::Pending && state_ != State::Typing))
        return false;
    switch (e.type) {
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        if (dialog_id_ == 0 || e.window.windowID != dialog_id_)
            return false;  // the game window: the main loop ends the program
        finish(false);
        return true;
    case SDL_EVENT_WINDOW_EXPOSED:
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
    case SDL_EVENT_WINDOW_RESIZED:
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
    case SDL_EVENT_WINDOW_FOCUS_LOST:
    case SDL_EVENT_WINDOW_SHOWN:
    case SDL_EVENT_WINDOW_MOVED:
        // The dialog's own window events must not reach the game window
        // (a resize would resize the game's surface).
        return dialog_id_ != 0 && e.window.windowID == dialog_id_;
    case SDL_EVENT_TEXT_INPUT: {
        if (message_mode_)
            return true;
        const std::string add = e.text.text != nullptr ? e.text.text : "";
        if (request_.max_length == 0 || utf16_length(text_ + add) <= request_.max_length)
            text_ += add;
        note_.clear();
        break;
    }
    case SDL_EVENT_KEY_DOWN:
        // Keys pressed in the game window while the dialog is open: neither
        // for the dialog (focus is visibly elsewhere) nor for the game.
        if (dialog_id_ != 0 && e.key.windowID != dialog_id_)
            return true;
        if (message_mode_) {
            if (e.key.key == SDLK_RETURN || e.key.key == SDLK_KP_ENTER || e.key.key == SDLK_ESCAPE)
                finish(true);
            return true;
        }
        if (e.key.key == SDLK_RETURN || e.key.key == SDLK_KP_ENTER) {
            if (text_fits(request_, text_))
                finish(true);
            else
                note_ = trf("length {} not allowed", {std::to_string(utf16_length(text_))});
        } else if (e.key.key == SDLK_ESCAPE) {
            finish(false);
        } else if (e.key.key == SDLK_BACKSPACE) {
            pop_utf8(&text_);
        }
        break;
    case SDL_EVENT_KEY_UP:
        // The controllers still see releases: a key held when the keyboard
        // opened (the button that asked for it) would otherwise stay pressed.
        return false;
    default:
        return false;  // gamepads, window events: not for the keyboard
    }
    if (window_ != nullptr && state_ != State::Done)
        SDL_SetWindowTitle(window_, title_locked().c_str());
    return true;
}

void SdlTextInput::cancel() {
    std::lock_guard lock(mutex_);
    closed_ = true;
    if (state_ == State::Pending || state_ == State::Typing)
        finish(false);
}

void SdlTextInput::service() {
    std::lock_guard lock(mutex_);
    // The previous request ended (it may already have been followed by the
    // next one, e.g. after a rejected name): close its dialog first.
    if (state_ != State::Typing && started_) {
        if (text_started_)
            SDL_StopTextInput(dialog_ != nullptr ? dialog_ : window_);
        text_started_ = false;
        close_dialog();
        SDL_SetWindowTitle(window_, saved_title_.c_str());
        started_ = false;
    }
    if (state_ == State::Pending) {
        state_ = State::Typing;
        if (window_ != nullptr && !host_drawn_) {
            open_dialog();
            saved_title_ = SDL_GetWindowTitle(window_);
            SDL_SetWindowTitle(window_, title_locked().c_str());
            if (!message_mode_) {
                SDL_StartTextInput(dialog_ != nullptr ? dialog_ : window_);
                text_started_ = true;
            }
            started_ = true;
        }
    }
    if (state_ == State::Typing && dialog_ != nullptr)
        draw();
}

}  // namespace lgp::platform::desktop
