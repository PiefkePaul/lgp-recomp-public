// "subsdk1" of the native boot test (tests/boot, boot.sdk_native_*): stands for
// the multimedia module, whose movie:: exports the movie HLE replaces. The
// native start leaves such a module out like the lift does (ADR-0005), so this
// code never runs; if it were loaded, its initializer would end the process.
#include "guest.hpp"

extern "C" __attribute__((visibility("default"), used)) void lgp_movie_buffer_ctor() __asm__("_ZN5movie6BufferC1Ev");
extern "C" void lgp_movie_buffer_ctor() {}

extern "C" __attribute__((used)) void lgp_multimedia_init() { __builtin_trap(); }
