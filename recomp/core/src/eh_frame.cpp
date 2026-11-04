// .eh_frame_hdr / .eh_frame parsing: function starts (and ranges) from the unwind tables.
// Details: docs/knowledge/eh-frame-function-bounds.md.
#include <algorithm>

#include "lgp/recomp/loader.hpp"

namespace lgp::recomp {

namespace {

struct Cursor {
    const Module& m;
    uint64_t pos;
    uint8_t u8() { if (pos + 1 > m.image.size()) throw FormatError("eh_frame: truncated"); return m.image[pos++]; }
    uint16_t u16() { uint16_t lo = u8(); uint16_t hi = u8(); return static_cast<uint16_t>(lo | (hi << 8)); }
    uint32_t u32() { uint32_t v = m.read_u32(pos); pos += 4; return v; }
    uint64_t u64() { uint64_t v = m.read_u64(pos); pos += 8; return v; }
    uint64_t uleb() {
        uint64_t v = 0; int shift = 0; uint8_t b;
        do { b = u8(); if (shift < 64) v |= static_cast<uint64_t>(b & 0x7F) << shift; shift += 7; } while (b & 0x80);
        return v;
    }
    int64_t sleb() {
        int64_t v = 0; int shift = 0; uint8_t b;
        do { b = u8(); if (shift < 64) v |= static_cast<int64_t>(b & 0x7F) << shift; shift += 7; } while (b & 0x80);
        if (shift < 64 && (b & 0x40)) v |= -(int64_t(1) << shift);
        return v;
    }
    // DW_EH_PE encoded pointer. `datarel` = start of .eh_frame_hdr. Returns module offset.
    uint64_t encoded(uint8_t enc, uint64_t datarel, bool apply = true) {
        if (enc == 0xFF) return 0;  // DW_EH_PE_omit
        uint64_t field = pos, v;
        switch (enc & 0x0F) {
            case 0x00: v = u64(); break;
            case 0x01: v = uleb(); break;
            case 0x02: v = u16(); break;
            case 0x03: v = u32(); break;
            case 0x04: v = u64(); break;
            case 0x09: v = static_cast<uint64_t>(sleb()); break;
            case 0x0A: v = static_cast<uint64_t>(static_cast<int16_t>(u16())); break;
            case 0x0B: v = static_cast<uint64_t>(static_cast<int32_t>(u32())); break;
            case 0x0C: v = u64(); break;
            default: throw FormatError("eh_frame: unsupported pointer encoding");
        }
        if (!apply) return v;
        switch (enc & 0x70) {
            case 0x00: return v;               // absptr (module-relative, since the image is unlinked)
            case 0x10: return field + v;       // pcrel
            case 0x30: return datarel + v;     // datarel
            default: throw FormatError("eh_frame: unsupported pointer application");
        }
    }
};

struct CieInfo {
    uint8_t fde_enc = 0x00;
    uint8_t lsda_enc = 0xFF;  // 0xFF: the CIE has no 'L'
    bool z = false;           // FDEs carry augmentation data
    bool ok = false;
};

CieInfo parse_cie(const Module& m, uint64_t cie_off) {
    CieInfo c;
    Cursor cur{m, cie_off};
    uint32_t len = cur.u32();
    if (len == 0 || len == 0xFFFFFFFF) return c;
    uint64_t end = cur.pos + len;
    if (cur.u32() != 0) return c;  // CIE id
    uint8_t version = cur.u8();
    std::string aug;
    for (uint8_t ch; (ch = cur.u8()) != 0;) aug.push_back(static_cast<char>(ch));
    if (aug.find("eh") != std::string::npos) cur.u64();
    cur.uleb();                 // code alignment
    cur.sleb();                 // data alignment
    if (version == 1) cur.u8(); else cur.uleb();  // return address register
    if (!aug.empty() && aug[0] == 'z') {
        c.z = true;
        cur.uleb();  // augmentation data length
        for (size_t i = 1; i < aug.size(); ++i) {
            switch (aug[i]) {
                case 'L': c.lsda_enc = cur.u8(); break;
                case 'P': { uint8_t enc = cur.u8(); cur.encoded(enc, 0, false); break; }
                case 'R': c.fde_enc = cur.u8(); break;
                case 'S': case 'B': case 'G': break;
                default: return c;  // unknown augmentation: give up on the range
            }
        }
    }
    (void)end;
    c.ok = true;
    return c;
}

}  // namespace

void parse_eh_frame(Module& m) {
    m.fde_starts.clear();
    m.fde_ranges.clear();
    m.lsda_fde_starts.clear();
    if (!m.eh_frame_hdr_size) return;
    uint64_t hdr = m.eh_frame_hdr_offset;
    Cursor cur{m, hdr};
    if (cur.u8() != 1) return;  // version
    uint8_t ptr_enc = cur.u8(), count_enc = cur.u8(), table_enc = cur.u8();
    uint64_t eh_frame = cur.encoded(ptr_enc, hdr);
    uint64_t count = cur.encoded(count_enc, hdr, false);
    (void)eh_frame;
    if (table_enc == 0xFF) return;  // no binary search table
    std::vector<std::pair<uint64_t, uint64_t>> entries;  // (initial_loc, fde)
    entries.reserve(static_cast<size_t>(std::min<uint64_t>(count, 1u << 22)));
    const Segment& text = m.segment(SegmentKind::Text);
    for (uint64_t i = 0; i < count && cur.pos < hdr + m.eh_frame_hdr_size; ++i) {
        uint64_t loc = cur.encoded(table_enc, hdr);
        uint64_t fde = cur.encoded(table_enc, hdr);
        if (loc < text.offset || loc >= text.offset + text.size || (loc & 3)) continue;
        entries.emplace_back(loc, fde);
    }
    std::sort(entries.begin(), entries.end());
    entries.erase(std::unique(entries.begin(), entries.end(),
                              [](auto& a, auto& b) { return a.first == b.first; }),
                  entries.end());
    for (auto& [loc, fde] : entries) {
        m.fde_starts.push_back(loc);
        // FDE: length, CIE pointer (relative, backwards), pc_begin, pc_range.
        try {
            Cursor f{m, fde};
            uint32_t len = f.u32();
            if (len == 0 || len == 0xFFFFFFFF) continue;
            uint64_t cie_ptr_pos = f.pos;
            uint32_t cie_rel = f.u32();
            if (cie_rel == 0 || cie_rel > cie_ptr_pos) continue;
            CieInfo cie = parse_cie(m, cie_ptr_pos - cie_rel);
            if (!cie.ok) continue;
            uint64_t pc_begin = f.encoded(cie.fde_enc, hdr);
            uint64_t pc_range = f.encoded(cie.fde_enc & 0x0F, hdr, false);
            if (pc_begin != loc || pc_range == 0 || pc_range > text.size) continue;
            m.fde_ranges.emplace_back(loc, loc + pc_range);
            if (cie.z && cie.lsda_enc != 0xFF) {
                f.uleb();  // augmentation data length
                if (f.encoded(cie.lsda_enc, hdr, false) != 0) m.lsda_fde_starts.push_back(loc);
            }
        } catch (const std::exception&) {
            // Unparseable FDE: keep the start, drop the range.
        }
    }
}

}  // namespace lgp::recomp
