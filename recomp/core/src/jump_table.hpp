// Internal: resolve a BR Xn by symbolic backward slicing (docs/knowledge/jump-table-patterns.md).
#pragma once

#include <map>

#include "lgp/recomp/cfg.hpp"

namespace lgp::recomp::detail {

struct IndirectResolution {
    enum class Kind : uint8_t { Unresolved, Table, Direct } kind = Kind::Unresolved;
    JumpTable table;        // Kind::Table
    uint64_t target = 0;    // Kind::Direct (constant register, literal pool or GOT slot)
    uint64_t via_slot = 0;  // GOT slot the target was loaded from, 0 otherwise
    std::string note;       // why it failed / how it was found (for diagnostics)
};

IndirectResolution resolve_indirect(const LinkedImage& img, const std::map<uint64_t, BasicBlock>& blocks,
                                    uint64_t br_pc, uint64_t fn_start, uint64_t fn_bound, uint32_t max_scan);

}  // namespace lgp::recomp::detail
