// The software keyboard (swkbd HLE, lgp/hle/services.hpp) on a host text
// input (lgp/platform/text_input.hpp): converts the game's UTF-16 request to
// UTF-8 and the answer back, and logs what was asked and answered. The error
// applet's message goes to the same host input.
#pragma once

#include "lgp/hle/services.hpp"
#include "lgp/platform/text_input.hpp"

namespace lgp::boot {

// A handler for AppletState::set_keyboard_handler; `input` must outlive it.
hle::KeyboardHandler keyboard_handler(platform::ITextInput* input);
// A handler for AppletState::set_message_handler; same lifetime rule.
hle::MessageHandler message_handler(platform::ITextInput* input);

}  // namespace lgp::boot
