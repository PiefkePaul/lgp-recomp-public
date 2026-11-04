// Browser test of the error message dialog (WebTextInput::show_message):
// the probe shows a message; once it prints "message ready" the runner
// presses Enter, which must close the dialog through its OK button and
// return to the caller.
#include <cstdio>

#include "lgp/platform/web/web_text_input.hpp"

using namespace lgp::platform;

int main() {
    web::WebTextInput dialogs;
    std::printf("message ready\n");
    std::fflush(stdout);
    dialogs.show_message({"error 2110-1100", "No connection."});
    std::printf("message: ok\n");
    return 0;
}
