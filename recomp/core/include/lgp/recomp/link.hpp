// Link step: assign bases in rtld load order, resolve symbols, apply relocations.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "lgp/recomp/module.hpp"

namespace lgp::recomp {

struct LinkOptions {
    uint64_t first_base = 0x0800'0000;   // ADR-0001 may change this; nothing else depends on it
    uint64_t page_size = 0x1000;
    bool sort_load_order = true;          // rtld order: rtld, main, subsdk0..9, sdk; unknown names keep input order
    bool strict = false;                  // unresolved strong symbol / unsupported reloc -> throw
};

struct Import {
    std::string name;
    uint32_t from_module = 0;  // index into LinkedImage::modules
    uint32_t sym_index = 0;    // dynsym index in from_module
    uint64_t slot_va = 0;      // first GOT/PLT slot that was patched (0 if the symbol has no slot)
    uint64_t target_va = 0;    // 0 = unresolved
    int32_t to_module = -1;    // -1 = unresolved
    bool weak = false;
};

struct LinkDiagnostic {
    enum class Kind { UnresolvedSymbol, UnsupportedRelocation, RelocationOutOfRange, MissingNeeded } kind;
    std::string module;
    std::string text;
};

struct LinkStats {
    uint64_t relocations_applied = 0;
    uint64_t relocations_unsupported = 0;
    uint64_t imports_resolved = 0;
    uint64_t imports_unresolved_weak = 0;
    uint64_t imports_unresolved_strong = 0;
};

struct LinkedImage {
    std::vector<Module> modules;
    std::vector<Import> imports;
    std::vector<LinkDiagnostic> diagnostics;
    LinkStats stats;

    const Module* module_at(uint64_t va) const;
    std::optional<std::pair<const Module*, uint64_t>> locate(uint64_t va) const;
    const Segment* segment_at(uint64_t va) const;
    bool in_text(uint64_t va) const;
    uint32_t read_u32(uint64_t va) const;   // throws std::out_of_range outside every module
    uint64_t read_u64(uint64_t va) const;
    uint8_t read_u8(uint64_t va) const;
    // Defined symbol whose VA equals va exactly (first match: global before weak before local).
    const Symbol* symbol_at(uint64_t va, const Module** owner = nullptr) const;
    // Symbol defined anywhere, by name, in load order.
    std::optional<uint64_t> symbol_va(const std::string& name) const;
    const Import* import_by_slot(uint64_t slot_va) const;
    uint64_t end_va() const;
};

// Consumes the modules (moves them into the image).
LinkedImage link(std::vector<Module> modules, const LinkOptions& opt = {});

// rtld load order rank (rtld=0, main=1, subsdkN=2+N, sdk=12, other=100).
int load_order_rank(const std::string& name);

}  // namespace lgp::recomp
