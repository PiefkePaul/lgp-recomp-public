// Text input backends without a window (lgp/platform/text_input.hpp):
//   FixedTextInput    answers every request with the same text (scripted
//                     runs: lgp_boot --keyboard text:NAME)
//   ConsoleTextInput  asks on a terminal: prompt on `out`, one line from `in`;
//                     an empty line keeps the initial text, end of input
//                     cancels; messages wait for Enter
// FixedTextInput shows no messages (scripted runs must not block).
#pragma once

#include <iosfwd>
#include <mutex>

#include "lgp/platform/text_input.hpp"

namespace lgp::platform {

// Length of UTF-8 text in UTF-16 units.
uint32_t utf16_length(const std::string& utf8);
// Whether `text` satisfies the request's limits (and is not empty).
bool text_fits(const TextRequest& request, const std::string& text);

class FixedTextInput final : public ITextInput {
public:
    explicit FixedTextInput(std::string text) : text_(std::move(text)) {}
    bool ask(const TextRequest& request, std::string* text) override;

private:
    std::string text_;
};

class ConsoleTextInput final : public ITextInput {
public:
    ConsoleTextInput(std::istream& in, std::ostream& out) : in_(in), out_(out) {}
    bool ask(const TextRequest& request, std::string* text) override;
    void show_message(const MessageRequest& message) override;

private:
    std::mutex mutex_;
    std::istream& in_;
    std::ostream& out_;
};

}  // namespace lgp::platform
