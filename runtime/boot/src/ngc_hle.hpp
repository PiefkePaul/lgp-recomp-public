// Stand-in for the SDK's profanity filter (nn::ngc::ProfanityFilter) when the
// game folder has no NgWord2 archive (systemdata/0100000000000823) from the
// user's firmware: the filter's methods get host hooks at the doors that
// lgp_lift gives them (lgp/recomp/sdk_hooks.hpp). Initialize succeeds without
// mounting, nothing is a profanity, nothing is masked. Idea as in nxnative's
// hle_misc.c (MIT, read only, no code taken).
#pragma once

#include <cstddef>

#include "lgp/recomp/link.hpp"
#include "lgp/runtime/kernel.hpp"

namespace lgp::boot {

struct NgcHleResult {
    size_t exported = 0;   // filter methods the modules export
    size_t installed = 0;  // of those, hooked (the lift gave them a door)
};

// Installs the hooks for every exported method with a door. kernel must
// outlive them; remove_ngc_hle() takes them out again.
NgcHleResult install_ngc_hle(rt::Kernel& kernel, const recomp::LinkedImage& image);
void remove_ngc_hle(const recomp::LinkedImage& image);

}  // namespace lgp::boot
