// Minimal ELF64 .symtab reader. Used only to check discovered function starts
// against the symbols of our own homebrew fixtures (never against game code).
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "lgp/recomp/module.hpp"

namespace lgp::recomp {

struct ElfSymbols {
    std::vector<Symbol> symbols;   // .symtab (falls back to .dynsym), value = link-time address
    uint64_t entry = 0;
    bool from_symtab = false;
};

ElfSymbols read_elf_symbols(std::span<const uint8_t> bytes);
ElfSymbols read_elf_symbols_file(const std::string& path);

}  // namespace lgp::recomp
