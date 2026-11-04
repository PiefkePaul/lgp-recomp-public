// Delivers a pthread's pending console output before the program exits.
//
// In a PROXY_TO_PTHREAD build a pthread prints by posting a message to the
// main thread. When main() returns, the exit is proxied through Emscripten's
// call queue, which can overtake print messages still queued; once the
// runtime is exiting the main thread drops them (seen in Safari and Firefox:
// the last lines of a test went missing). drain_output() sends a marker
// through the same print channel and waits (at most 2 s) until the main
// thread has handled it. Call it right before returning from main().
//
// The boot program has the same mechanism in lgp::boot::finish_host()
// (platform/web/src/boot_host.cpp).
#pragma once

namespace lgp::platform::web {

void drain_output();

}  // namespace lgp::platform::web
