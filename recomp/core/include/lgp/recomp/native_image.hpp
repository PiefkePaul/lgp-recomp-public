// Native rewrite, second part: turns the plan (native_rewrite.hpp) into code.
// Every stub site becomes a `B` into its stub in a patch area behind the last
// module; the stub runs the instruction with x18 in a stack slot (X18Spill)
// or calls the gate (GateSite, contract in native_gate.hpp) and branches back
// to the instruction after the site. Renamed sites keep their new word.
//
// Layout of the patch area: 16 KiB aligned, gate entry and stubs, then one
// 16 KiB data page (native_gate::kHostFnSlot). Run on game data, the patch
// area and the patched modules are derived game data and stay in
// $LGP_PRIVATE_DIR; only the counts may be shared.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "lgp/recomp/link.hpp"
#include "lgp/recomp/native_rewrite.hpp"

namespace lgp::recomp {

inline constexpr uint64_t kNativePageSize = 0x4000;

struct NativePatchOptions {
    uint64_t area_base = 0;  // 0: behind the last module, 16 KiB aligned
};

struct NativeModulePatch {
    std::string module;
    uint64_t base = 0;
    std::vector<std::pair<uint64_t, uint32_t>> words;  // VA -> new word (renames and branches to stubs), sorted
};

struct NativePatch {
    uint64_t area_base = 0;
    uint64_t gate_entry = 0;
    uint64_t data_base = 0;           // the data page
    uint64_t host_fn_slot = 0;        // VA of the host function pointer (runtime writes it)
    std::vector<uint8_t> area;        // code, padding, data page (zero); mapped at area_base
    uint64_t code_size = 0;           // bytes of code at the start of `area`
    std::vector<NativeModulePatch> modules;  // same order as the plan
    std::vector<std::pair<uint64_t, uint64_t>> stubs;  // site VA -> stub VA, sorted

    uint64_t sites_renamed = 0, spill_stubs = 0, gate_stubs = 0;
};

// Throws std::runtime_error when a stub is out of `B` range (±128 MiB) of its
// site or a site cannot be encoded as planned.
NativePatch build_native_patch(const LinkedImage& image, const std::vector<NativeRewrite>& plan,
                               const NativePatchOptions& opt = {});

// Host-call stubs (native_gate::kHostCallWord) for the host functions at
// `vas`, as code at `base` (16 KiB aligned), calling the gate entry of a patch
// area. `words`: the `B` to write at each VA (sorted). Throws
// std::runtime_error when a VA is out of `B` range of its stub.
struct NativeHostCalls {
    uint64_t base = 0;
    std::vector<uint8_t> code;
    std::vector<std::pair<uint64_t, uint32_t>> words;
};
NativeHostCalls build_native_host_calls(std::vector<uint64_t> vas, uint64_t base, uint64_t gate_entry);

// The NSO `nso` (the file `m` was loaded from) with the words of `p` written
// into .text. All segments are written uncompressed with fresh SHA-256 hashes;
// .rodata and .data keep their bytes. Throws FormatError for anything else
// than an NSO and std::runtime_error for a word outside .text.
std::vector<uint8_t> patch_nso(std::span<const uint8_t> nso, const Module& m, const NativeModulePatch& p);

}  // namespace lgp::recomp
