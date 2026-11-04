// The lifted-code tables of a boot program without lifted code
// (lgp_add_boot_executable NATIVE): the original code runs natively
// (BootOptions::native, docs/analysis/56 stage D).
#include <cstddef>

#include "lgp/lifted/cpu_state.hpp"
#include "lgp/runtime/system.hpp"

extern const lgp::LiftedEntry lgp_lifted_entries[] = {{0, nullptr}};
extern const size_t lgp_lifted_entries_count = 0;
extern const lgp::LiftedModule lgp_lifted_entries_modules[] = {{"", 0, 0, ""}};
extern const size_t lgp_lifted_entries_modules_count = 0;
