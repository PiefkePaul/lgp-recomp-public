// The platform layer (P3.6). Everything the runtime needs from the host goes
// through platform/: guest memory reservation (host_block.hpp) and host
// threads (host_thread.hpp) in platform/os, and the four services below,
// implemented per target (platform/common: null and host-directory backends;
// platform/desktop, platform/web: the real ones).
//
// runtime/ itself has no OS, windowing or browser dependency; the test
// platform.no_host_deps checks that.
#pragma once

#include "lgp/platform/audio.hpp"
#include "lgp/platform/input.hpp"
#include "lgp/platform/renderer.hpp"
#include "lgp/platform/storage.hpp"

namespace lgp::platform {

// Non-owning: the embedding program (desktop main, web glue, tests) owns the
// backends and keeps them alive while the process runs.
struct Platform {
    IRenderer* renderer = nullptr;
    IAudio* audio = nullptr;
    IInput* input = nullptr;
    IStorage* game = nullptr;  // read-only: the user's own dump (RomFS, ExeFS)
    IStorage* save = nullptr;  // save data and other writable state
};

}  // namespace lgp::platform
