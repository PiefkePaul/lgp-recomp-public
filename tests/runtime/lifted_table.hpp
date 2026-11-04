// Guest address → lifted function, as emitted by the lift_*_programs tools.
#pragma once

#include <cstddef>
#include <cstdint>

#include "lgp/lifted/cpu_state.hpp"

namespace lgp_test {

struct LiftedEntry {
    uint64_t va;
    lgp::LiftedFn fn;
};

}  // namespace lgp_test
