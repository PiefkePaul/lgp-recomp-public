// NSO / NRO parsing: segments, MOD0, .dynamic, dynsym, relocations (RELA/JMPREL/RELR).
// Format notes: docs/knowledge/nso-nro-format.md.
#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>

#include "lgp/recomp/loader.hpp"
#include "sha256.hpp"

namespace lgp::recomp {

namespace {

uint32_t rd32(std::span<const uint8_t> b, size_t off) {
    if (off + 4 > b.size()) throw FormatError("read past end of file");
    uint32_t v;
    std::memcpy(&v, b.data() + off, 4);
    return v;
}
uint64_t rd64(std::span<const uint8_t> b, size_t off) {
    if (off + 8 > b.size()) throw FormatError("read past end of file");
    uint64_t v;
    std::memcpy(&v, b.data() + off, 8);
    return v;
}

void check_layout(const std::vector<Segment>& segs, size_t file_size, uint64_t max_image) {
    uint64_t prev_end = 0;
    for (const auto& s : segs) {
        if (s.kind == SegmentKind::Bss) continue;
        if (s.file_offset + s.file_size > file_size)
            throw FormatError("segment file range beyond end of file");
        if (s.offset < prev_end) throw FormatError("segment memory ranges overlap or run backwards");
        if (s.offset + s.size > max_image) throw FormatError("image size exceeds limit");
        prev_end = s.offset + s.size;
    }
}

std::vector<std::string> split_strings(std::span<const uint8_t> b) {
    std::vector<std::string> out;
    std::string cur;
    for (uint8_t c : b) {
        if (c == 0) {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(static_cast<char>(c));
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

std::vector<Symbol> decode_symbols(std::span<const uint8_t> symtab, std::span<const uint8_t> strtab) {
    std::vector<Symbol> out;
    out.reserve(symtab.size() / 24);
    for (size_t k = 0; k + 24 <= symtab.size(); k += 24) {
        Symbol s;
        uint32_t st_name = rd32(symtab, k);
        uint8_t info = symtab[k + 4];
        s.bind = static_cast<uint8_t>(info >> 4);
        s.type = static_cast<uint8_t>(info & 0xF);
        std::memcpy(&s.shndx, symtab.data() + k + 6, 2);
        s.value = rd64(symtab, k + 8);
        s.size = rd64(symtab, k + 16);
        if (st_name < strtab.size()) {
            const auto* p = strtab.data() + st_name;
            const auto* e = static_cast<const uint8_t*>(std::memchr(p, 0, strtab.size() - st_name));
            s.name.assign(reinterpret_cast<const char*>(p), e ? static_cast<size_t>(e - p) : strtab.size() - st_name);
        }
        out.push_back(std::move(s));
    }
    return out;
}

void read_ro_path(Module& m) {
    const Segment& ro = m.segment(SegmentKind::Ro);
    if (ro.size < 8) return;
    uint32_t unk = m.read_u32(ro.offset), plen = m.read_u32(ro.offset + 4);
    if (unk != 0 || plen == 0 || plen >= 0x200 || ro.offset + 8 + plen > m.image.size()) return;
    std::string p(reinterpret_cast<const char*>(m.image.data() + ro.offset + 8), plen);
    p = p.substr(0, p.find('\0'));
    std::replace(p.begin(), p.end(), '\\', '/');
    auto slash = p.rfind('/');
    m.internal_name = slash == std::string::npos ? p : p.substr(slash + 1);
}

void read_embedded(Module& m, std::span<const uint8_t> hdr, size_t table_off) {
    uint32_t api_off = rd32(hdr, table_off), api_size = rd32(hdr, table_off + 4);
    uint32_t str_off = rd32(hdr, table_off + 8), str_size = rd32(hdr, table_off + 12);
    uint32_t sym_off = rd32(hdr, table_off + 16), sym_size = rd32(hdr, table_off + 20);
    const Segment& ro = m.segment(SegmentKind::Ro);
    auto ro_span = [&](uint32_t off, uint32_t size) {
        if (static_cast<uint64_t>(off) + size > ro.size) throw FormatError("embedded range outside .ro");
        return std::span<const uint8_t>(m.image.data() + ro.offset + off, size);
    };
    if (api_size) m.api_info = split_strings(ro_span(api_off, api_size));
    if (sym_size) m.dynsym = decode_symbols(ro_span(sym_off, sym_size), ro_span(str_off, str_size));
}

size_t dynsym_count(const Module& m, uint64_t symtab, uint64_t strtab) {
    if (auto h = m.dynamic_value(kDtHash)) return m.read_u32(*h + 4);  // nchain
    if (auto gh = m.dynamic_value(kDtGnuHash)) {
        uint64_t h = *gh;
        uint32_t nbuckets = m.read_u32(h), symoffset = m.read_u32(h + 4), bloom_size = m.read_u32(h + 8);
        uint64_t buckets = h + 16 + static_cast<uint64_t>(bloom_size) * 8;
        uint64_t chains = buckets + static_cast<uint64_t>(nbuckets) * 4;
        uint32_t last = 0;
        for (uint32_t i = 0; i < nbuckets; ++i) last = std::max(last, m.read_u32(buckets + i * 4));
        if (last < symoffset) return symoffset;
        while (!(m.read_u32(chains + static_cast<uint64_t>(last - symoffset) * 4) & 1)) ++last;
        return last + 1;
    }
    if (strtab > symtab) return (strtab - symtab) / 24;
    throw FormatError("cannot determine dynsym size (no DT_HASH/DT_GNU_HASH)");
}

void symbols_from_dynamic(Module& m) {
    auto symtab = m.dynamic_value(kDtSymTab), strtab = m.dynamic_value(kDtStrTab);
    if (!symtab || !strtab) return;
    uint64_t strsz = m.dynamic_value(kDtStrSz).value_or(m.image.size() - *strtab);
    size_t count = dynsym_count(m, *symtab, *strtab);
    if (*symtab + count * 24 > m.image.size() || *strtab + strsz > m.image.size())
        throw FormatError("dynsym/dynstr outside module image");
    m.dynsym = decode_symbols(std::span<const uint8_t>(m.image.data() + *symtab, count * 24),
                              std::span<const uint8_t>(m.image.data() + *strtab, strsz));
}

void read_needed(Module& m) {
    auto strtab = m.dynamic_value(kDtStrTab);
    if (!strtab) return;
    for (const auto& d : m.dynamic) {
        if (d.tag != kDtNeeded) continue;
        uint64_t off = *strtab + d.value;
        if (off >= m.image.size()) continue;
        const auto* p = reinterpret_cast<const char*>(m.image.data() + off);
        const auto* e = static_cast<const char*>(std::memchr(p, 0, m.image.size() - off));
        m.needed.emplace_back(p, e ? static_cast<size_t>(e - p) : m.image.size() - off);
    }
}

void read_rela(Module& m, int64_t off_tag, int64_t size_tag, bool jmprel) {
    auto off = m.dynamic_value(off_tag), size = m.dynamic_value(size_tag);
    if (!off || !size) return;
    if (*off + *size > m.image.size()) throw FormatError("relocation table outside module image");
    for (uint64_t k = 0; k + 24 <= *size; k += 24) {
        Relocation r;
        r.offset = m.read_u64(*off + k);
        uint64_t info = m.read_u64(*off + k + 8);
        r.type = static_cast<uint32_t>(info & 0xFFFFFFFF);
        r.sym = static_cast<uint32_t>(info >> 32);
        r.addend = static_cast<int64_t>(m.read_u64(*off + k + 16));
        r.from_jmprel = jmprel;
        m.relocations.push_back(r);
    }
}

// DT_RELR: packed relative relocations (devkitA64 r29+ / lld). Even entry = address of one
// slot; odd entry = bitmap of the 63 slots following the last address.
void read_relr(Module& m) {
    auto off = m.dynamic_value(kDtRelr), size = m.dynamic_value(kDtRelrSz);
    if (!off || !size) return;
    if (*off + *size > m.image.size()) throw FormatError("RELR table outside module image");
    uint64_t ent = m.dynamic_value(kDtRelrEnt).value_or(8);
    if (ent != 8) throw FormatError("DT_RELRENT must be 8");
    uint64_t where = 0;
    for (uint64_t k = 0; k + 8 <= *size; k += 8) {
        uint64_t e = m.read_u64(*off + k);
        if ((e & 1) == 0) {
            m.relocations.push_back(Relocation{e, kRAarch64Relative, 0, 0, true, false, false});
            where = e + 8;
        } else {
            for (unsigned i = 0; (e >>= 1) != 0; ++i) {
                if (e & 1)
                    m.relocations.push_back(Relocation{where + i * 8, kRAarch64Relative, 0, 0, true, false, false});
            }
            where += 63 * 8;
        }
    }
}

void parse_mod0(Module& m) {
    if (m.image.size() < 8) return;
    uint32_t mod0 = m.read_u32(4);
    if (static_cast<uint64_t>(mod0) + 0x1C > m.image.size() || std::memcmp(m.image.data() + mod0, "MOD0", 4) != 0)
        return;
    m.mod0_offset = mod0;
    auto rel = [&](uint64_t field) {
        int32_t v;
        std::memcpy(&v, m.image.data() + mod0 + field, 4);
        int64_t abs = static_cast<int64_t>(mod0) + v;
        if (abs < 0 || static_cast<uint64_t>(abs) > m.image.size()) throw FormatError("MOD0 offset outside image");
        return static_cast<uint64_t>(abs);
    };
    m.dynamic_offset = rel(0x4);
    uint64_t eh_s = rel(0x10), eh_e = rel(0x14);
    if (eh_e > eh_s) {
        m.eh_frame_hdr_offset = eh_s;
        m.eh_frame_hdr_size = eh_e - eh_s;
    }
    for (uint64_t d = m.dynamic_offset; d + 16 <= m.image.size(); d += 16) {
        int64_t tag = static_cast<int64_t>(m.read_u64(d));
        uint64_t val = m.read_u64(d + 8);
        if (tag == kDtNull) break;
        m.dynamic.push_back({tag, val});
    }
    read_rela(m, kDtRela, kDtRelaSz, false);
    read_rela(m, kDtJmpRel, kDtPltRelSz, true);
    read_relr(m);
    if (m.dynamic_value(kDtRel) || m.dynamic_value(kDtRelSz))
        throw FormatError("DT_REL (Elf64_Rel without addend) is not supported");
    m.init = m.dynamic_value(kDtInit).value_or(0);
    m.fini = m.dynamic_value(kDtFini).value_or(0);
    m.init_array_offset = m.dynamic_value(kDtInitArray).value_or(0);
    m.init_array_size = m.dynamic_value(kDtInitArraySz).value_or(0);
    m.fini_array_offset = m.dynamic_value(kDtFiniArray).value_or(0);
    m.fini_array_size = m.dynamic_value(kDtFiniArraySz).value_or(0);
    read_needed(m);
}

void finish(Module& m, const LoadOptions& opt) {
    parse_mod0(m);
    if (m.dynsym.empty()) symbols_from_dynamic(m);
    if (opt.parse_eh_frame && m.eh_frame_hdr_size) parse_eh_frame(m);
}

}  // namespace

// ---- Module helpers --------------------------------------------------------

const Segment& Module::segment(SegmentKind k) const {
    for (const auto& s : segments)
        if (s.kind == k) return s;
    throw std::logic_error("segment missing");
}

const Segment* Module::segment_at_offset(uint64_t off) const {
    for (const auto& s : segments)
        if (off >= s.offset && off < s.offset + s.size) return &s;
    return nullptr;
}

std::optional<uint64_t> Module::dynamic_value(int64_t tag) const {
    for (const auto& d : dynamic)
        if (d.tag == tag) return d.value;
    return std::nullopt;
}

uint32_t Module::read_u32(uint64_t off) const {
    if (off + 4 > image.size()) throw std::out_of_range("Module::read_u32");
    uint32_t v;
    std::memcpy(&v, image.data() + off, 4);
    return v;
}
uint64_t Module::read_u64(uint64_t off) const {
    if (off + 8 > image.size()) throw std::out_of_range("Module::read_u64");
    uint64_t v;
    std::memcpy(&v, image.data() + off, 8);
    return v;
}
void Module::write_u64(uint64_t off, uint64_t v) {
    if (off + 8 > image.size()) throw std::out_of_range("Module::write_u64");
    std::memcpy(image.data() + off, &v, 8);
}

std::string Module::hex_module_id() const {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (uint8_t b : module_id) {
        s.push_back(d[b >> 4]);
        s.push_back(d[b & 15]);
    }
    return s;
}

const char* relocation_type_name(uint32_t type) {
    switch (type) {
        case kRAarch64Abs64: return "ABS64";
        case kRAarch64GlobDat: return "GLOB_DAT";
        case kRAarch64JumpSlot: return "JUMP_SLOT";
        case kRAarch64Relative: return "RELATIVE";
        case kRAarch64TlsTpRel64: return "TLS_TPREL64";
        case kRAarch64TlsDesc: return "TLSDESC";
        default: return "other";
    }
}

// ---- NSO -------------------------------------------------------------------

bool is_nso(std::span<const uint8_t> b) { return b.size() >= 0x100 && std::memcmp(b.data(), "NSO0", 4) == 0; }
bool is_nro(std::span<const uint8_t> b) { return b.size() >= 0x80 && std::memcmp(b.data() + 0x10, "NRO0", 4) == 0; }

Module load_nso(std::span<const uint8_t> raw, const std::string& name, const LoadOptions& opt) {
    if (!is_nso(raw)) throw FormatError(name + ": not an NSO0 file");
    Module m;
    m.name = name;
    m.nso_flags = rd32(raw, 0x0C);
    static const SegmentKind kinds[3] = {SegmentKind::Text, SegmentKind::Ro, SegmentKind::Data};
    for (int i = 0; i < 3; ++i) {
        Segment s;
        s.kind = kinds[i];
        s.file_offset = rd32(raw, 0x10 + i * 0x10);
        s.offset = rd32(raw, 0x14 + i * 0x10);
        s.size = rd32(raw, 0x18 + i * 0x10);
        s.file_size = rd32(raw, 0x60 + i * 4);
        s.compressed = (m.nso_flags >> i) & 1;
        s.hash_checked = (m.nso_flags >> (i + 3)) & 1;
        std::memcpy(s.sha256.data(), raw.data() + 0xA0 + i * 0x20, 32);
        m.segments.push_back(s);
    }
    uint32_t bss_size = rd32(raw, 0x3C);
    std::memcpy(m.module_id.data(), raw.data() + 0x40, 32);
    check_layout(m.segments, raw.size(), opt.max_image_size);
    const Segment& data = m.segments[2];
    if (data.offset + data.size + bss_size > opt.max_image_size) throw FormatError("image size exceeds limit");
    m.image.assign(data.offset + data.size + bss_size, 0);
    for (auto& s : m.segments) {
        std::span<const uint8_t> blob(raw.data() + s.file_offset, s.file_size);
        std::vector<uint8_t> dec;
        if (s.compressed) {
            dec = lz4_block_decompress(blob, s.size);
        } else {
            if (blob.size() < s.size) throw FormatError("uncompressed segment shorter than memory size");
            dec.assign(blob.begin(), blob.begin() + static_cast<std::ptrdiff_t>(s.size));
        }
        if (opt.verify_hashes) s.hash_ok = sha256(dec) == s.sha256;
        std::copy(dec.begin(), dec.end(), m.image.begin() + static_cast<std::ptrdiff_t>(s.offset));
    }
    Segment bss;
    bss.kind = SegmentKind::Bss;
    bss.offset = data.offset + data.size;
    bss.size = bss_size;
    m.segments.push_back(bss);
    m.entry_offset = m.segments[0].offset;
    read_ro_path(m);
    read_embedded(m, raw, 0x88);
    finish(m, opt);
    // SDK/libnx crt0: `b start` at +0, MOD0 offset word at +4. The word at +4 is data.
    if (m.mod0_offset && m.segments[0].offset == 0) m.header_code_exclusion = 8;
    return m;
}

// ---- NRO -------------------------------------------------------------------

Module load_nro(std::span<const uint8_t> raw, const std::string& name, const LoadOptions& opt) {
    if (!is_nro(raw)) throw FormatError(name + ": not an NRO0 file");
    Module m;
    m.name = name;
    m.is_nro = true;
    uint32_t nro_size = rd32(raw, 0x18);
    m.nso_flags = rd32(raw, 0x1C);
    if (nro_size > raw.size()) throw FormatError("NRO header size exceeds file size");
    static const SegmentKind kinds[3] = {SegmentKind::Text, SegmentKind::Ro, SegmentKind::Data};
    for (int i = 0; i < 3; ++i) {
        Segment s;
        s.kind = kinds[i];
        s.file_offset = s.offset = rd32(raw, 0x20 + i * 8);
        s.file_size = s.size = rd32(raw, 0x24 + i * 8);
        m.segments.push_back(s);
    }
    uint32_t bss_size = rd32(raw, 0x38);
    std::memcpy(m.module_id.data(), raw.data() + 0x40, 32);
    check_layout(m.segments, nro_size, opt.max_image_size);
    const Segment& data = m.segments[2];
    if (data.offset + data.size + bss_size > opt.max_image_size) throw FormatError("image size exceeds limit");
    m.image.assign(data.offset + data.size + bss_size, 0);
    std::copy(raw.begin(), raw.begin() + static_cast<std::ptrdiff_t>(data.offset + data.size), m.image.begin());
    Segment bss;
    bss.kind = SegmentKind::Bss;
    bss.offset = data.offset + data.size;
    bss.size = bss_size;
    m.segments.push_back(bss);
    m.entry_offset = m.segments[0].offset;
    // Bytes 0x4..0x80 of .text are the NRO header (MOD0 pointer, "NRO0", sizes), not code.
    if (m.segments[0].offset == 0) m.header_code_exclusion = 0x80;
    // No .ro path header: that {0, len, path} prefix is an SDK (NSO) convention only.
    read_embedded(m, raw, 0x68);
    finish(m, opt);
    return m;
}

Module load_module(std::span<const uint8_t> bytes, const std::string& name, const LoadOptions& opt) {
    if (is_nso(bytes)) return load_nso(bytes, name, opt);
    if (is_nro(bytes)) return load_nro(bytes, name, opt);
    throw FormatError(name + ": neither NSO0 nor NRO0");
}

std::vector<uint8_t> read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw FormatError("cannot open " + path);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

Module load_module_file(const std::string& path, const LoadOptions& opt) {
    auto bytes = read_file(path);
    std::string name = path;
    auto slash = name.find_last_of("/\\");
    if (slash != std::string::npos) name = name.substr(slash + 1);
    return load_module(bytes, name, opt);
}

}  // namespace lgp::recomp
