#include "lgp/recomp/elf_symbols.hpp"

#include <cstring>

#include "lgp/recomp/loader.hpp"

namespace lgp::recomp {

namespace {
template <class T> T rd(std::span<const uint8_t> b, size_t off) {
    if (off + sizeof(T) > b.size()) throw FormatError("elf: truncated");
    T v;
    std::memcpy(&v, b.data() + off, sizeof(T));
    return v;
}
}  // namespace

ElfSymbols read_elf_symbols(std::span<const uint8_t> b) {
    ElfSymbols out;
    if (b.size() < 64 || std::memcmp(b.data(), "\x7f" "ELF", 4) != 0) throw FormatError("elf: bad magic");
    if (b[4] != 2 || b[5] != 1) throw FormatError("elf: need ELF64 little-endian");
    out.entry = rd<uint64_t>(b, 0x18);
    uint64_t shoff = rd<uint64_t>(b, 0x28);
    uint16_t shentsize = rd<uint16_t>(b, 0x3A), shnum = rd<uint16_t>(b, 0x3C);
    if (shentsize < 64) throw FormatError("elf: bad section header size");
    if (shoff > b.size() || shoff + static_cast<uint64_t>(shnum) * shentsize > b.size())
        throw FormatError("elf: section headers outside file");
    // All offsets are validated against the file size before narrowing to size_t (32 bit on Wasm).
    struct Sec { uint32_t type; size_t off, size; uint32_t link; };
    std::vector<Sec> secs;
    for (uint16_t i = 0; i < shnum; ++i) {
        size_t sh = static_cast<size_t>(shoff) + static_cast<size_t>(i) * shentsize;
        uint64_t off = rd<uint64_t>(b, sh + 0x18), size = rd<uint64_t>(b, sh + 0x20);
        uint32_t type = rd<uint32_t>(b, sh + 4);
        if (type != 8 /*SHT_NOBITS*/ && (off > b.size() || size > b.size() - off))
            throw FormatError("elf: section outside file");
        secs.push_back({type, static_cast<size_t>(off), static_cast<size_t>(type == 8 ? 0 : size), rd<uint32_t>(b, sh + 0x28)});
    }
    auto read_table = [&](const Sec& s) {
        if (s.link >= secs.size()) throw FormatError("elf: symtab without strtab");
        const Sec& str = secs[s.link];
        for (size_t k = 0; k + 24 <= s.size; k += 24) {
            Symbol sym;
            uint32_t name = rd<uint32_t>(b, s.off + k);
            uint8_t info = b[s.off + k + 4];
            sym.bind = static_cast<uint8_t>(info >> 4);
            sym.type = static_cast<uint8_t>(info & 0xF);
            sym.shndx = rd<uint16_t>(b, s.off + k + 6);
            sym.value = rd<uint64_t>(b, s.off + k + 8);
            sym.size = rd<uint64_t>(b, s.off + k + 16);
            if (name < str.size) {
                const char* p = reinterpret_cast<const char*>(b.data() + str.off + name);
                const char* e = static_cast<const char*>(std::memchr(p, 0, str.size - name));
                sym.name.assign(p, e ? static_cast<size_t>(e - p) : str.size - name);
            }
            out.symbols.push_back(std::move(sym));
        }
    };
    for (const auto& s : secs)
        if (s.type == 2) { read_table(s); out.from_symtab = true; }
    if (!out.from_symtab)
        for (const auto& s : secs)
            if (s.type == 11) read_table(s);  // SHT_DYNSYM
    return out;
}

ElfSymbols read_elf_symbols_file(const std::string& path) {
    auto bytes = read_file(path);
    return read_elf_symbols(bytes);
}

}  // namespace lgp::recomp
