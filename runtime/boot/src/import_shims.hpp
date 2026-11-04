// Imports no loaded module defines (e.g. LGP's movie:: functions from
// subsdk1, which ADR-0005 does not recompile). Without a shim their GOT slot
// stays 0 and the first call jumps to address 0. The boot binds each such
// slot to a host function instead: a known HLE where there is one (movie::,
// see movie_hle.cpp), otherwise a trap that names the import and halts.
#pragma once

#include <cstdint>
#include <vector>

#include "lgp/recomp/link.hpp"
#include "lgp/runtime/kernel.hpp"
#include "lgp/runtime/system.hpp"

namespace lgp::boot {

// Thunk keys are placed at thunk_base + 4 * i (never mapped, dispatch keys
// only; room for kMaxImportShims). data_va: one mapped RW page the HLEs may
// use (vtables). Appends the host functions to *host. Returns the number of
// imports bound.
size_t install_import_shims(rt::Kernel& kernel, const recomp::LinkedImage& image, uint64_t thunk_base,
                            uint64_t data_va, std::vector<LiftedEntry>* host);

inline constexpr size_t kMaxImportShims = 1024;

}  // namespace lgp::boot
