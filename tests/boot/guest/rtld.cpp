// "rtld" of the native boot test (tests/boot, boot.sdk_native_*): segments on
// 4 KiB boundaries like the console's rtld, so text, ro and data share one
// 16 KiB page. The boot program plays rtld's part; this code never runs.
#include "guest.hpp"

uint32_t lgp_rtld_word = 1;

extern "C" __attribute__((used)) void lgp_rtld_init() { lgp_rtld_word = 2; }
