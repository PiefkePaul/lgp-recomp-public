// Builds synthetic NSO / NRO images for tests: our own bytes, no game data.
// Layout: .text at 0, .ro at text_size, .data at text_size+ro_size, then BSS.
// .data starts with MOD0 and .dynamic; .ro holds path header, api_info, dynsym/dynstr,
// optional DT_HASH, .eh_frame_hdr/.eh_frame and whatever the test allocates (jump tables).
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include "../../recomp/core/src/sha256.hpp"

namespace synth {

constexpr uint32_t R_ABS64 = 257, R_GLOB_DAT = 1025, R_JUMP_SLOT = 1026, R_RELATIVE = 1027, R_TLSDESC = 1031;
constexpr uint8_t STB_GLOBAL = 1, STB_WEAK = 2, STT_OBJECT = 1, STT_FUNC = 2;

inline void put32(std::vector<uint8_t>& v, size_t off, uint32_t x) { std::memcpy(v.data() + off, &x, 4); }
inline void put64(std::vector<uint8_t>& v, size_t off, uint64_t x) { std::memcpy(v.data() + off, &x, 8); }
inline void app32(std::vector<uint8_t>& v, uint32_t x) { v.resize(v.size() + 4); put32(v, v.size() - 4, x); }
inline void app64(std::vector<uint8_t>& v, uint64_t x) { v.resize(v.size() + 8); put64(v, v.size() - 8, x); }

// Literal-only LZ4 block.
inline std::vector<uint8_t> lz4_literals(const std::vector<uint8_t>& d) {
    std::vector<uint8_t> out;
    size_t n = d.size();
    if (n < 15) out.push_back(static_cast<uint8_t>(n << 4));
    else {
        out.push_back(0xF0);
        size_t rest = n - 15;
        while (rest >= 255) { out.push_back(255); rest -= 255; }
        out.push_back(static_cast<uint8_t>(rest));
    }
    out.insert(out.end(), d.begin(), d.end());
    return out;
}

// Naive greedy LZ4 compressor (matches >= 4 bytes within a 4 KiB window) to exercise match decoding.
inline std::vector<uint8_t> lz4_compress(const std::vector<uint8_t>& d) {
    std::vector<uint8_t> out;
    size_t n = d.size(), i = 0, lit_start = 0;
    auto write_len = [&](std::vector<uint8_t>& o, size_t len, bool high) {
        if (len < 15) { o.push_back(static_cast<uint8_t>(high ? len << 4 : len)); return; }
        o.push_back(static_cast<uint8_t>(high ? 0xF0 : 0x0F));
        len -= 15;
        while (len >= 255) { o.push_back(255); len -= 255; }
        o.push_back(static_cast<uint8_t>(len));
    };
    auto emit = [&](size_t lit_len, size_t off, size_t mlen) {
        std::vector<uint8_t> tok;
        uint8_t t = static_cast<uint8_t>((std::min<size_t>(lit_len, 15) << 4) | (mlen ? std::min<size_t>(mlen - 4, 15) : 0));
        out.push_back(t);
        if (lit_len >= 15) { size_t l = lit_len - 15; while (l >= 255) { out.push_back(255); l -= 255; } out.push_back(static_cast<uint8_t>(l)); }
        out.insert(out.end(), d.begin() + static_cast<std::ptrdiff_t>(lit_start), d.begin() + static_cast<std::ptrdiff_t>(lit_start + lit_len));
        if (mlen) {
            out.push_back(static_cast<uint8_t>(off & 0xFF));
            out.push_back(static_cast<uint8_t>(off >> 8));
            if (mlen - 4 >= 15) { size_t l = mlen - 4 - 15; while (l >= 255) { out.push_back(255); l -= 255; } out.push_back(static_cast<uint8_t>(l)); }
        }
        (void)write_len;
    };
    while (i < n) {
        size_t best_len = 0, best_off = 0;
        if (i + 4 <= n && n - i > 12) {  // LZ4 requires the last 5 bytes (and 12 total) literal
            size_t win = std::min<size_t>(i, 4096);
            for (size_t off = 1; off <= win; ++off) {
                size_t l = 0;
                while (i + l < n - 5 && d[i + l] == d[i - off + l] && l < 255 + 19) ++l;
                if (l >= 4 && l > best_len) { best_len = l; best_off = off; }
            }
        }
        if (best_len) {
            emit(i - lit_start, best_off, best_len);
            i += best_len;
            lit_start = i;
        } else {
            ++i;
        }
    }
    emit(n - lit_start, 0, 0);
    return out;
}

struct Sym {
    std::string name;
    uint64_t offset = 0;   // module offset, 0 for undefined
    uint64_t size = 0;
    uint8_t type = STT_FUNC, bind = STB_GLOBAL;
    bool defined = true;
};

struct Rela {
    uint64_t offset;
    uint32_t type;
    std::string sym;       // empty for RELATIVE
    int64_t addend;
};

class Builder {
public:
    uint64_t text_size = 0x1000, ro_size = 0x1000, data_size = 0x1000, bss_size = 0x200;
    bool nro = false;
    bool header_dynsym = true;     // NSO header carries dynsym/dynstr ranges
    bool emit_hash = true;         // DT_HASH (otherwise dynsym count from the symtab..strtab gap)
    bool mark_text_hash = true;
    std::vector<uint8_t> module_id = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20};
    std::string ro_path = "D:/build/fixture.nss";
    std::string api_info = "SDK MW+Test+Lib-1_0";
    std::vector<std::string> needed;
    std::vector<Sym> symbols;
    std::vector<Rela> relas;
    std::vector<uint64_t> relr;                 // module offsets of RELATIVE slots (packed)
    std::vector<std::pair<uint64_t, uint64_t>> fdes;   // [start, end) module offsets
    std::vector<uint64_t> lsda_fdes;                   // FDE starts that get a (dummy, non-zero) LSDA pointer
    std::vector<uint64_t> init_array;           // module offsets of functions (RELATIVE relocs are added)
    std::vector<uint32_t> text_words;           // written at code_offset()

    Builder() = default;

    uint64_t ro_base() const { return text_size; }
    uint64_t data_base() const { return text_size + ro_size; }
    uint64_t code_offset() const { return nro ? 0x80 : 0x10; }
    uint64_t text_off(size_t word_index) const { return code_offset() + word_index * 4; }

    // Allocate bytes in .ro / .data (module offsets).
    uint64_t alloc_ro(uint64_t size, uint64_t align = 8) { ro_cursor_ = (ro_cursor_ + align - 1) & ~(align - 1); uint64_t r = ro_cursor_; ro_cursor_ += size; return ro_base() + r; }
    uint64_t alloc_data(uint64_t size, uint64_t align = 8) { data_cursor_ = (data_cursor_ + align - 1) & ~(align - 1); uint64_t r = data_cursor_; data_cursor_ += size; return data_base() + r; }
    void write(uint64_t module_off, const std::vector<uint8_t>& bytes) { pending_writes_.emplace_back(module_off, bytes); }
    void write64(uint64_t module_off, uint64_t v) { std::vector<uint8_t> b(8); put64(b, 0, v); write(module_off, b); }
    void write32(uint64_t module_off, uint32_t v) { std::vector<uint8_t> b(4); put32(b, 0, v); write(module_off, b); }

    uint32_t sym_index(const std::string& name) const {
        for (size_t i = 0; i < symbols.size(); ++i) if (symbols[i].name == name) return static_cast<uint32_t>(i + 1);
        throw std::runtime_error("synth: unknown symbol " + name);
    }

    // Builds the full in-memory image (text|ro|data, no bss) and the dynamic metadata.
    std::vector<uint8_t> build_image() {
        ro_cursor_ = std::max<uint64_t>(ro_cursor_, 0x400);      // leave room for the fixed .ro header below
        data_cursor_ = std::max<uint64_t>(data_cursor_, 0x400);  // MOD0 + dynamic live in the first 0x400 bytes
        std::vector<uint8_t> img(data_base() + data_size, 0);
        // .text: `b code` and MOD0 pointer, then code.
        uint64_t mod0 = data_base();
        put32(img, 0, 0x14000000 | static_cast<uint32_t>((code_offset() >> 2) & 0x3FFFFFF));
        put32(img, 4, static_cast<uint32_t>(mod0));
        if (nro) std::memcpy(img.data() + 0x10, "NRO0", 4);  // real header fields filled by build_nro
        for (size_t i = 0; i < text_words.size(); ++i) put32(img, code_offset() + i * 4, text_words[i]);

        // .ro fixed part: path header, api_info, dynstr, dynsym, hash, eh_frame.
        uint64_t ro = ro_base();
        std::string path = ro_path + '\0';
        put32(img, ro, 0);
        put32(img, ro + 4, static_cast<uint32_t>(path.size()));
        std::memcpy(img.data() + ro + 8, path.data(), path.size());
        api_off_ = 0x80;
        std::memcpy(img.data() + ro + api_off_, api_info.data(), api_info.size());
        api_size_ = static_cast<uint32_t>(api_info.size() + 1);
        // dynsym then dynstr (so the gap fallback works)
        std::vector<uint8_t> strtab{0}, symtab(24, 0);
        std::map<std::string, uint32_t> needed_off;
        for (const auto& s : symbols) {
            uint32_t name_off = static_cast<uint32_t>(strtab.size());
            strtab.insert(strtab.end(), s.name.begin(), s.name.end());
            strtab.push_back(0);
            std::vector<uint8_t> e(24, 0);
            put32(e, 0, name_off);
            e[4] = static_cast<uint8_t>((s.bind << 4) | s.type);
            uint16_t shndx = s.defined ? 1 : 0;
            std::memcpy(e.data() + 6, &shndx, 2);
            put64(e, 8, s.defined ? s.offset : 0);
            put64(e, 16, s.size);
            symtab.insert(symtab.end(), e.begin(), e.end());
        }
        for (const auto& n : needed) {
            needed_off[n] = static_cast<uint32_t>(strtab.size());
            strtab.insert(strtab.end(), n.begin(), n.end());
            strtab.push_back(0);
        }
        sym_off_ = 0x100;
        if (sym_off_ + symtab.size() + strtab.size() > 0x400) throw std::runtime_error("synth: too many symbols for the fixed .ro area");
        std::memcpy(img.data() + ro + sym_off_, symtab.data(), symtab.size());
        sym_size_ = static_cast<uint32_t>(symtab.size());
        str_off_ = static_cast<uint32_t>(sym_off_ + symtab.size());
        std::memcpy(img.data() + ro + str_off_, strtab.data(), strtab.size());
        str_size_ = static_cast<uint32_t>(strtab.size());
        uint32_t nsyms = static_cast<uint32_t>(symbols.size() + 1);
        uint64_t hash_off = 0;
        if (emit_hash) {
            hash_off = alloc_ro(8 + 4 + nsyms * 4, 8);
            put32(img, hash_off, 1);
            put32(img, hash_off + 4, nsyms);
            put32(img, hash_off + 8, 0);
        }
        // .eh_frame_hdr + .eh_frame
        uint64_t eh_hdr = 0, eh_hdr_end = 0;
        if (!fdes.empty()) {
            std::vector<std::pair<uint64_t, uint64_t>> sorted = fdes;
            std::sort(sorted.begin(), sorted.end());
            uint64_t hdr_size = 12 + sorted.size() * 8;
            eh_hdr = alloc_ro(hdr_size, 4);
            eh_hdr_end = eh_hdr + hdr_size;
            // CIE
            std::vector<uint8_t> frame;
            app32(frame, 0);           // length placeholder
            app32(frame, 0);           // CIE id
            frame.push_back(1);        // version
            const bool lsda = !lsda_fdes.empty();
            if (lsda) frame.insert(frame.end(), {'z', 'L', 'R', 0});
            else frame.insert(frame.end(), {'z', 'R', 0});
            frame.push_back(1);        // code align
            frame.push_back(0x7C);     // data align -4
            frame.push_back(30);       // return register
            frame.push_back(lsda ? 2 : 1);  // aug length
            if (lsda) frame.push_back(0x1B);  // LSDA encoding: pcrel sdata4
            frame.push_back(0x1B);     // FDE encoding: pcrel sdata4
            while (frame.size() % 8) frame.push_back(0);
            put32(frame, 0, static_cast<uint32_t>(frame.size() - 4));
            uint64_t frame_off = alloc_ro(frame.size() + sorted.size() * 24, 8);
            std::vector<uint64_t> fde_pos;
            for (auto [s, e] : sorted) {
                uint64_t pos = frame.size();
                fde_pos.push_back(frame_off + pos);
                app32(frame, 0);                                           // length
                app32(frame, static_cast<uint32_t>(pos + 4));              // CIE pointer: (this field) - cie(0)
                int32_t pc_begin = static_cast<int32_t>(static_cast<int64_t>(s) - static_cast<int64_t>(frame_off + pos + 8));
                app32(frame, static_cast<uint32_t>(pc_begin));
                app32(frame, static_cast<uint32_t>(e - s));
                if (lsda) {
                    frame.push_back(4);                                    // aug length
                    const bool has = std::find(lsda_fdes.begin(), lsda_fdes.end(), s) != lsda_fdes.end();
                    app32(frame, has ? 0x100u : 0u);                       // LSDA (never read)
                } else {
                    frame.push_back(0);                                    // aug length
                }
                while ((frame.size() - pos) % 8) frame.push_back(0);
                put32(frame, pos, static_cast<uint32_t>(frame.size() - pos - 4));
            }
            std::memcpy(img.data() + frame_off, frame.data(), frame.size());
            img[eh_hdr] = 1;
            img[eh_hdr + 1] = 0x1B;  // eh_frame_ptr: pcrel sdata4
            img[eh_hdr + 2] = 0x03;  // count: udata4
            img[eh_hdr + 3] = 0x3B;  // table: datarel sdata4
            put32(img, eh_hdr + 4, static_cast<uint32_t>(static_cast<int32_t>(static_cast<int64_t>(frame_off) - static_cast<int64_t>(eh_hdr + 4))));
            put32(img, eh_hdr + 8, static_cast<uint32_t>(sorted.size()));
            for (size_t i = 0; i < sorted.size(); ++i) {
                put32(img, eh_hdr + 12 + i * 8, static_cast<uint32_t>(static_cast<int32_t>(static_cast<int64_t>(sorted[i].first) - static_cast<int64_t>(eh_hdr))));
                put32(img, eh_hdr + 16 + i * 8, static_cast<uint32_t>(static_cast<int32_t>(static_cast<int64_t>(fde_pos[i]) - static_cast<int64_t>(eh_hdr))));
            }
        }
        // .data: init_array slots (+ RELATIVE relocs), rela table, relr table, dynamic, MOD0.
        uint64_t init_arr = 0;
        if (!init_array.empty()) {
            init_arr = alloc_data(init_array.size() * 8, 8);
            for (size_t i = 0; i < init_array.size(); ++i) {
                relas.push_back({init_arr + i * 8, R_RELATIVE, "", static_cast<int64_t>(init_array[i])});
            }
        }
        std::vector<Rela> plt, plain;
        for (const auto& r : relas) (r.type == R_JUMP_SLOT ? plt : plain).push_back(r);
        auto emit_rela = [&](const std::vector<Rela>& rs, uint64_t& off, uint64_t& size) {
            size = rs.size() * 24;
            if (!size) { off = 0; return; }
            off = alloc_data(size, 8);
            for (size_t i = 0; i < rs.size(); ++i) {
                uint64_t e = off + i * 24;
                put64(img, e, rs[i].offset);
                uint64_t sym = rs[i].sym.empty() ? 0 : sym_index(rs[i].sym);
                put64(img, e + 8, (sym << 32) | rs[i].type);
                put64(img, e + 16, static_cast<uint64_t>(rs[i].addend));
            }
        };
        uint64_t rela_off, rela_size, plt_off, plt_size;
        emit_rela(plain, rela_off, rela_size);
        emit_rela(plt, plt_off, plt_size);
        uint64_t relr_off = 0, relr_size = 0;
        if (!relr.empty()) {
            std::vector<uint64_t> words = pack_relr(relr);
            relr_size = words.size() * 8;
            relr_off = alloc_data(relr_size, 8);
            for (size_t i = 0; i < words.size(); ++i) put64(img, relr_off + i * 8, words[i]);
        }
        // .dynamic at data_base()+0x20 (after MOD0)
        std::vector<std::pair<int64_t, uint64_t>> dyn;
        for (const auto& n : needed) dyn.emplace_back(1, needed_off[n]);
        dyn.emplace_back(5, ro + str_off_);   // STRTAB
        dyn.emplace_back(6, ro + sym_off_);   // SYMTAB
        dyn.emplace_back(10, str_size_);      // STRSZ
        dyn.emplace_back(11, 24);             // SYMENT
        if (emit_hash) dyn.emplace_back(4, hash_off);
        if (rela_size) { dyn.emplace_back(7, rela_off); dyn.emplace_back(8, rela_size); dyn.emplace_back(9, 24); }
        if (plt_size) { dyn.emplace_back(23, plt_off); dyn.emplace_back(2, plt_size); dyn.emplace_back(20, 7); }
        if (relr_size) { dyn.emplace_back(36, relr_off); dyn.emplace_back(35, relr_size); dyn.emplace_back(37, 8); }
        if (init_arr) { dyn.emplace_back(25, init_arr); dyn.emplace_back(27, init_array.size() * 8); }
        dyn.emplace_back(0, 0);
        uint64_t dynamic = data_base() + 0x20;
        if (0x20 + dyn.size() * 16 > 0x400) throw std::runtime_error("synth: dynamic too large");
        for (size_t i = 0; i < dyn.size(); ++i) {
            put64(img, dynamic + i * 16, static_cast<uint64_t>(dyn[i].first));
            put64(img, dynamic + i * 16 + 8, dyn[i].second);
        }
        std::memcpy(img.data() + mod0, "MOD0", 4);
        auto rel32 = [&](uint64_t field, uint64_t target) { put32(img, mod0 + field, static_cast<uint32_t>(static_cast<int32_t>(static_cast<int64_t>(target) - static_cast<int64_t>(mod0)))); };
        rel32(0x4, dynamic);
        rel32(0x8, data_base() + data_size);              // bss start
        rel32(0xC, data_base() + data_size + bss_size);   // bss end
        rel32(0x10, eh_hdr);
        rel32(0x14, eh_hdr_end);
        rel32(0x18, 0);
        for (const auto& [off, bytes] : pending_writes_) {
            if (off + bytes.size() > img.size()) throw std::runtime_error("synth: write outside image");
            std::memcpy(img.data() + off, bytes.data(), bytes.size());
        }
        if (ro_cursor_ > ro_size || data_cursor_ > data_size) throw std::runtime_error("synth: segment overflow");
        return img;
    }

    std::vector<uint8_t> build_nso(bool compress, bool greedy_lz4 = false) {
        nro = false;
        std::vector<uint8_t> img = build_image();
        std::vector<uint8_t> hdr(0x100, 0);
        std::memcpy(hdr.data(), "NSO0", 4);
        uint32_t flags = (compress ? 0b111 : 0) | (mark_text_hash ? 0b111000 : 0);
        put32(hdr, 0x0C, flags);
        uint64_t segs[3][2] = {{0, text_size}, {ro_base(), ro_size}, {data_base(), data_size}};
        std::vector<uint8_t> payload;
        uint64_t file_off = 0x100;
        for (int i = 0; i < 3; ++i) {
            std::vector<uint8_t> seg(img.begin() + static_cast<std::ptrdiff_t>(segs[i][0]),
                                     img.begin() + static_cast<std::ptrdiff_t>(segs[i][0] + segs[i][1]));
            std::vector<uint8_t> blob = compress ? (greedy_lz4 ? lz4_compress(seg) : lz4_literals(seg)) : seg;
            put32(hdr, 0x10 + i * 0x10, static_cast<uint32_t>(file_off));
            put32(hdr, 0x14 + i * 0x10, static_cast<uint32_t>(segs[i][0]));
            put32(hdr, 0x18 + i * 0x10, static_cast<uint32_t>(segs[i][1]));
            put32(hdr, 0x60 + i * 4, static_cast<uint32_t>(blob.size()));
            auto h = lgp::recomp::sha256(seg);
            std::memcpy(hdr.data() + 0xA0 + i * 0x20, h.data(), 32);
            file_off += blob.size();
            payload.insert(payload.end(), blob.begin(), blob.end());
        }
        put32(hdr, 0x3C, static_cast<uint32_t>(bss_size));
        std::memcpy(hdr.data() + 0x40, module_id.data(), std::min<size_t>(module_id.size(), 32));
        if (header_dynsym) {
            put32(hdr, 0x88, api_off_); put32(hdr, 0x8C, api_size_);
            put32(hdr, 0x90, str_off_); put32(hdr, 0x94, str_size_);
            put32(hdr, 0x98, sym_off_); put32(hdr, 0x9C, sym_size_);
        }
        hdr.insert(hdr.end(), payload.begin(), payload.end());
        return hdr;
    }

    std::vector<uint8_t> build_nro() {
        nro = true;
        std::vector<uint8_t> img = build_image();
        std::memcpy(img.data() + 0x10, "NRO0", 4);
        put32(img, 0x14, 0);                                   // version
        put32(img, 0x18, static_cast<uint32_t>(img.size()));  // size
        put32(img, 0x1C, 0);                                   // flags
        put32(img, 0x20, 0); put32(img, 0x24, static_cast<uint32_t>(text_size));
        put32(img, 0x28, static_cast<uint32_t>(ro_base())); put32(img, 0x2C, static_cast<uint32_t>(ro_size));
        put32(img, 0x30, static_cast<uint32_t>(data_base())); put32(img, 0x34, static_cast<uint32_t>(data_size));
        put32(img, 0x38, static_cast<uint32_t>(bss_size));
        std::memcpy(img.data() + 0x40, module_id.data(), std::min<size_t>(module_id.size(), 32));
        put32(img, 0x68, api_off_); put32(img, 0x6C, api_size_);
        put32(img, 0x70, str_off_); put32(img, 0x74, str_size_);
        put32(img, 0x78, sym_off_); put32(img, 0x7C, sym_size_);
        // Trailing asset section (ignored by loaders): a few junk bytes.
        img.insert(img.end(), {'A', 'S', 'E', 'T', 0, 0, 0, 0});
        return img;
    }

    static std::vector<uint64_t> pack_relr(std::vector<uint64_t> offs) {
        std::sort(offs.begin(), offs.end());
        std::vector<uint64_t> out;
        size_t i = 0;
        while (i < offs.size()) {
            out.push_back(offs[i]);
            uint64_t where = offs[i] + 8;
            ++i;
            for (;;) {
                uint64_t bits = 0;
                bool any = false;
                while (i < offs.size() && offs[i] < where + 63 * 8) {
                    bits |= uint64_t(1) << (1 + (offs[i] - where) / 8);
                    any = true;
                    ++i;
                }
                if (!any) break;
                out.push_back(bits | 1);
                where += 63 * 8;
            }
        }
        return out;
    }

private:
    uint64_t ro_cursor_ = 0x400, data_cursor_ = 0x400;
    std::vector<std::pair<uint64_t, std::vector<uint8_t>>> pending_writes_;
    uint32_t api_off_ = 0, api_size_ = 0, str_off_ = 0, str_size_ = 0, sym_off_ = 0, sym_size_ = 0;
};

}  // namespace synth
