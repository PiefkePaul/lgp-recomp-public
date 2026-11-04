#include <istream>
#include <ostream>
#include <string>

#include "lgp/platform/text_input_backends.hpp"

namespace lgp::platform {

uint32_t utf16_length(const std::string& utf8) {
    uint32_t n = 0;
    for (const char c : utf8) {
        const auto b = static_cast<uint8_t>(c);
        if ((b & 0xC0) != 0x80)
            n += (b >> 3) == 0x1E ? 2 : 1;  // a 4-byte sequence is a surrogate pair
    }
    return n;
}

bool text_fits(const TextRequest& request, const std::string& text) {
    const uint32_t n = utf16_length(text);
    return n >= 1 && n >= request.min_length && (request.max_length == 0 || n <= request.max_length);
}

bool FixedTextInput::ask(const TextRequest&, std::string* text) {
    *text = text_;
    return true;
}

void ConsoleTextInput::show_message(const MessageRequest& message) {
    std::lock_guard lock(mutex_);
    out_ << "\n== " << message.code << " ==\n";
    if (!message.text.empty())
        out_ << message.text << "\n";
    out_ << "[Enter: OK] " << std::flush;
    std::string line;
    std::getline(in_, line);
}

bool ConsoleTextInput::ask(const TextRequest& request, std::string* text) {
    std::lock_guard lock(mutex_);
    for (;;) {
        out_ << "\n== software keyboard ==\n";
        if (!request.header.empty())
            out_ << request.header << "\n";
        if (!request.sub.empty())
            out_ << request.sub << "\n";
        if (!request.message.empty())
            out_ << "(" << request.message << ")\n";
        if (request.min_length != 0 || request.max_length != 0)
            out_ << "length " << request.min_length << ".." << request.max_length << "\n";
        out_ << (request.guide.empty() ? std::string("text") : request.guide);
        if (!request.initial.empty())
            out_ << " [" << request.initial << "]";
        out_ << ": " << std::flush;
        std::string line;
        if (!std::getline(in_, line))
            return false;
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty())
            line = request.initial;
        if (text_fits(request, line)) {
            *text = line;
            return true;
        }
        out_ << "length " << utf16_length(line) << " is outside " << request.min_length << ".."
             << request.max_length << "\n";
    }
}

}  // namespace lgp::platform
