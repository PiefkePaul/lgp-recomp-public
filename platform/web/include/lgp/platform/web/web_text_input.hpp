// Software keyboard in the browser (lgp/platform/text_input.hpp): a modal
// <dialog> on the page with the game's header, a text field (length limits,
// password mode) and OK/Cancel. The guest thread blocks in ask() while the
// page's main thread shows the dialog; the answer comes back through the
// shared wasm memory (an atomic status word, as finish_host() does).
// show_message() uses the same kind of dialog with the error code, the
// text and an OK button (error applet).
//
// While a text field has focus, WebInput leaves the keys alone, so typing
// neither reaches the controllers nor gets swallowed by them.
#pragma once

#include "lgp/platform/text_input.hpp"

namespace lgp::platform::web {

class WebTextInput final : public ITextInput {
public:
    bool ask(const TextRequest& request, std::string* text) override;
    void show_message(const MessageRequest& message) override;
};

}  // namespace lgp::platform::web
