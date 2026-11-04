// Patch area for native execution (see native_image.hpp).
#include "lgp/recomp/native_image.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <stdexcept>

#include "lgp/recomp/a64_emit.hpp"
#include "lgp/recomp/loader.hpp"
#include "lgp/recomp/native_census.hpp"
#include "lgp/recomp/native_gate.hpp"
#include "sha256.hpp"

namespace lgp::recomp {

using a64::Inst;
using a64::Op;
namespace e = emit;
namespace g = native_gate;

namespace {

constexpr uint8_t kSp = emit::kSp;
constexpr uint32_t kX18 = 1u << 18;

std::string hex(uint64_t v) {
    char b[24];
    std::snprintf(b, sizeof b, "0x%llx", static_cast<unsigned long long>(v));
    return b;
}

uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }

struct Code {
    uint64_t base = 0;
    std::vector<uint32_t> w;
    uint64_t pc() const { return base + 4 * w.size(); }
    uint64_t at(size_t k) const { return base + 4 * k; }
    size_t put(uint32_t x) {
        w.push_back(x);
        return w.size() - 1;
    }
};

// Gate entry. On entry SP is the frame (native_gate.hpp), the stub has saved
// x16, x17 and x30 there, x16 holds the site VA and w17 the instruction word.
void emit_gate(Code& c, uint64_t host_fn_slot) {
    for (uint8_t r = 0; r < 30; r += 2)
        if (r != 16) c.put(e::stp_x(r, static_cast<uint8_t>(r + 1), kSp, 8 * r));
    c.put(e::str_x(16, kSp, g::kPc));
    c.put(e::ldst_uimm(2, 0, 17, kSp, g::kWord));
    c.put(e::str_x(30, kSp, g::kRet));
    c.put(e::add_imm(16, kSp, g::kSize));
    c.put(e::str_x(16, kSp, g::kSp));
    c.put(e::mrs(16, a64::sr::kNzcv));
    c.put(e::str_x(16, kSp, g::kNzcv));
    c.put(e::mrs(16, a64::sr::kFpsr));
    c.put(e::str_x(16, kSp, g::kFpsr));
    c.put(e::add_imm(16, kSp, g::kQ));
    for (uint8_t q = 0; q < 32; q += 2) c.put(e::stp_q(q, static_cast<uint8_t>(q + 1), 16, 16 * q));
    c.put(e::add_imm(0, kSp, 0));  // mov x0, sp
    c.put(e::adrp(16, c.pc(), host_fn_slot));
    c.put(e::ldr_x(16, 16, host_fn_slot & 0xFFF));
    c.put(e::blr(16));
    c.put(e::add_imm(16, kSp, g::kQ));
    for (uint8_t q = 0; q < 32; q += 2) c.put(e::ldp_q(q, static_cast<uint8_t>(q + 1), 16, 16 * q));
    c.put(e::ldr_x(16, kSp, g::kFpsr));
    c.put(e::msr(a64::sr::kFpsr, 16));
    c.put(e::ldr_x(16, kSp, g::kNzcv));
    c.put(e::msr(a64::sr::kNzcv, 16));
    c.put(e::ldr_x(30, kSp, g::kRet));
    for (uint8_t r = 0; r < 30; r += 2) c.put(e::ldp_x(r, static_cast<uint8_t>(r + 1), kSp, 8 * r));
    c.put(e::kRet);
}

void emit_gate_stub(Code& c, const GateSite& s, uint64_t gate) {
    c.put(e::sub_imm(kSp, kSp, g::kSize));
    c.put(e::stp_x(16, 17, kSp, 8 * 16));
    c.put(e::str_x(30, kSp, 8 * 30));
    const size_t ld_va = c.put(0), ld_word = c.put(0);
    c.put(e::bl(c.pc(), gate));
    c.put(e::ldr_x(30, kSp, 8 * 30));
    c.put(e::add_imm(kSp, kSp, g::kSize));
    c.put(e::b(c.pc(), s.va + 4));
    const uint64_t lit = c.pc();
    c.put(static_cast<uint32_t>(s.va));
    c.put(static_cast<uint32_t>(s.va >> 32));
    c.put(s.word);
    c.w[ld_va] = e::ldr_lit(3, false, 16, c.at(ld_va), lit);
    c.w[ld_word] = e::ldr_lit(2, false, 17, c.at(ld_word), lit + 8);
}

// The site's instruction with `helper` in place of x18, whose value lives in
// the stack slot (X18Spill).
void emit_spill_stub(Code& c, const X18Spill& s) {
    const Inst i = a64::decode(s.word, s.va);
    const GprUse u = gpr_use(i);
    if (i.op == Op::Invalid || !((u.reads | u.writes) & kX18) || s.helper >= 31 || s.helper == 18 ||
        ((u.reads | u.writes) & (1u << s.helper)))
        throw std::runtime_error("native patch: spill site " + hex(s.va) + " does not fit its plan");
    const uint8_t h = s.helper;
    auto save = [&] { if (!s.helper_dead) c.put(e::stur_x(h, kSp, s.save_before)); };
    auto restore = [&] { if (!s.helper_dead) c.put(e::ldur_x(h, kSp, s.save_after)); };
    auto load = [&] { c.put(e::ldur_x(h, kSp, s.slot_before)); };
    auto store = [&] { c.put(e::stur_x(h, kSp, s.slot_after)); };
    save();
    switch (i.op) {
    case Op::Adr:
    case Op::Adrp:
        c.put(e::adrp(h, c.pc(), i.target));
        if (i.op == Op::Adr) c.put(e::add_imm(h, h, static_cast<uint32_t>(i.target & 0xFFF)));
        store();
        break;
    case Op::LdrLit:
        c.put(e::adrp(h, c.pc(), i.target));
        c.put(e::add_imm(h, h, static_cast<uint32_t>(i.target & 0xFFF)));
        c.put(e::ldst_uimm(i.size_log2, i.sign_extend ? 2 : 1, h, h, 0));
        store();
        break;
    case Op::Cbz:
    case Op::Cbnz:
    case Op::Tbz:
    case Op::Tbnz: {
        load();
        const size_t k = c.put(0);
        restore();
        c.put(e::b(c.pc(), s.va + 4));
        const uint64_t taken = c.pc();
        restore();
        c.put(e::b(c.pc(), i.target));
        c.w[k] = (i.op == Op::Cbz || i.op == Op::Cbnz) ? e::cbz(i.sf, h, c.at(k), taken, i.op == Op::Cbnz)
                                                       : e::tbz(h, i.bit, c.at(k), taken, i.op == Op::Tbnz);
        return;
    }
    case Op::Br:
        if (!s.helper_dead) throw std::runtime_error("native patch: br x18 at " + hex(s.va) + " without a dead helper");
        load();
        c.put(e::br(h));
        return;
    default: {
        if (i.flow != a64::Flow::Normal)
            throw std::runtime_error("native patch: spill site " + hex(s.va) + " is a branch");
        const auto nw = rename_gpr(s.word, s.va, 18, h);
        if (!nw) throw std::runtime_error("native patch: spill site " + hex(s.va) + " cannot be re-encoded");
        if (u.reads & kX18) load();
        c.put(*nw);
        if (u.writes & kX18) store();
        break;
    }
    }
    restore();
    c.put(e::b(c.pc(), s.va + 4));
}

struct Site {
    uint64_t va;
    const X18Spill* spill;
    const GateSite* gate;
    size_t module;
};

uint32_t rd32(std::span<const uint8_t> b, size_t off) {
    uint32_t v;
    std::memcpy(&v, b.data() + off, 4);
    return v;
}
void put32(std::vector<uint8_t>& b, size_t off, uint32_t v) { std::memcpy(b.data() + off, &v, 4); }

}  // namespace

NativePatch build_native_patch(const LinkedImage& image, const std::vector<NativeRewrite>& plan,
                               const NativePatchOptions& opt) {
    if (plan.size() != image.modules.size()) throw std::runtime_error("native patch: plan does not match the image");
    NativePatch p;
    uint64_t end = 0;
    for (const Module& m : image.modules) end = std::max(end, m.base + m.image.size());
    p.area_base = opt.area_base ? opt.area_base : align_up(end, kNativePageSize);
    if (p.area_base % kNativePageSize || p.area_base < end)
        throw std::runtime_error("native patch: area base " + hex(p.area_base) + " overlaps a module or is unaligned");

    std::vector<Site> sites;
    for (size_t m = 0; m < plan.size(); ++m) {
        for (const X18Spill& s : plan[m].x18_spills) sites.push_back({s.va, &s, nullptr, m});
        for (const GateSite& s : plan[m].gate_sites) sites.push_back({s.va, nullptr, &s, m});
    }
    std::sort(sites.begin(), sites.end(), [](const Site& a, const Site& b) { return a.va < b.va; });
    for (size_t k = 1; k < sites.size(); ++k)
        if (sites[k].va == sites[k - 1].va) throw std::runtime_error("native patch: two stubs for " + hex(sites[k].va));

    auto emit_all = [&](uint64_t data_base, std::vector<uint64_t>& stub_va) {
        Code c{p.area_base, {}};
        emit_gate(c, data_base + g::kHostFnSlot);
        stub_va.clear();
        for (const Site& s : sites) {
            stub_va.push_back(c.pc());
            try {
                if (s.spill) emit_spill_stub(c, *s.spill);
                else emit_gate_stub(c, *s.gate, p.area_base);
            } catch (const std::out_of_range& ex) {
                throw std::runtime_error("native patch: stub of " + hex(s.va) + ": " + ex.what());
            }
        }
        return c;
    };
    std::vector<uint64_t> stub_va;
    // The code size does not depend on where the data page is; the second
    // pass has the real address.
    const size_t words = emit_all(p.area_base, stub_va).w.size();
    p.data_base = align_up(p.area_base + 4 * words, kNativePageSize);
    const Code c = emit_all(p.data_base, stub_va);
    p.gate_entry = p.area_base;
    p.host_fn_slot = p.data_base + g::kHostFnSlot;
    p.code_size = 4 * c.w.size();
    p.area.assign(p.data_base - p.area_base + kNativePageSize, 0);
    std::memcpy(p.area.data(), c.w.data(), p.code_size);

    std::vector<std::map<uint64_t, uint32_t>> words_of(plan.size());
    for (size_t m = 0; m < plan.size(); ++m)
        for (const auto& [va, w] : plan[m].words) words_of[m][va] = w;
    for (size_t k = 0; k < sites.size(); ++k) {
        const Site& s = sites[k];
        if (!e::b_reaches(s.va, stub_va[k]))
            throw std::runtime_error("native patch: stub " + hex(stub_va[k]) + " out of branch range of " + hex(s.va));
        auto& ws = words_of[s.module];
        if (s.gate && ws.count(s.va)) --p.sites_renamed;  // replaced by the stub branch below
        ws[s.va] = e::b(s.va, stub_va[k]);
        p.stubs.push_back({s.va, stub_va[k]});
        ++(s.spill ? p.spill_stubs : p.gate_stubs);
    }
    for (size_t m = 0; m < plan.size(); ++m) {
        p.sites_renamed += plan[m].words.size();
        NativeModulePatch mp;
        mp.module = plan[m].module;
        mp.base = image.modules[m].base;
        mp.words.assign(words_of[m].begin(), words_of[m].end());
        p.modules.push_back(std::move(mp));
    }
    return p;
}

NativeHostCalls build_native_host_calls(std::vector<uint64_t> vas, uint64_t base, uint64_t gate_entry) {
    if (base % kNativePageSize) throw std::runtime_error("native host calls: base " + hex(base) + " is unaligned");
    std::sort(vas.begin(), vas.end());
    vas.erase(std::unique(vas.begin(), vas.end()), vas.end());
    NativeHostCalls h;
    h.base = base;
    Code c{base, {}};
    for (const uint64_t va : vas) {
        const uint64_t stub = c.pc();
        if (!e::b_reaches(va, stub))
            throw std::runtime_error("native host calls: stub " + hex(stub) + " out of branch range of " + hex(va));
        h.words.emplace_back(va, e::b(va, stub));
        // As emit_gate_stub(), but back to the caller (x30 of the frame).
        c.put(e::sub_imm(kSp, kSp, g::kSize));
        c.put(e::stp_x(16, 17, kSp, 8 * 16));
        c.put(e::str_x(30, kSp, 8 * 30));
        const size_t ld_va = c.put(0), ld_word = c.put(0);
        c.put(e::bl(c.pc(), gate_entry));
        c.put(e::ldr_x(30, kSp, 8 * 30));
        c.put(e::add_imm(kSp, kSp, g::kSize));
        c.put(e::kRet);
        const uint64_t lit = c.pc();
        c.put(static_cast<uint32_t>(va));
        c.put(static_cast<uint32_t>(va >> 32));
        c.put(g::kHostCallWord);
        c.w[ld_va] = e::ldr_lit(3, false, 16, c.at(ld_va), lit);
        c.w[ld_word] = e::ldr_lit(2, false, 17, c.at(ld_word), lit + 8);
    }
    h.code.resize(4 * c.w.size());
    std::memcpy(h.code.data(), c.w.data(), h.code.size());
    return h;
}

std::vector<uint8_t> patch_nso(std::span<const uint8_t> nso, const Module& m, const NativeModulePatch& p) {
    if (!is_nso(nso)) throw FormatError(m.name + ": not an NSO0 file");
    std::vector<uint8_t> seg[3];
    uint32_t mem_off[3], first_file = static_cast<uint32_t>(nso.size());
    const uint32_t flags = rd32(nso, 0x0C);
    for (int i = 0; i < 3; ++i) {
        const uint32_t file_off = rd32(nso, 0x10 + i * 0x10), size = rd32(nso, 0x18 + i * 0x10);
        const uint32_t file_size = rd32(nso, 0x60 + i * 4);
        mem_off[i] = rd32(nso, 0x14 + i * 0x10);
        if (static_cast<uint64_t>(file_off) + file_size > nso.size()) throw FormatError(m.name + ": segment outside the file");
        first_file = std::min(first_file, file_off);
        const std::span<const uint8_t> blob = nso.subspan(file_off, file_size);
        if ((flags >> i) & 1) seg[i] = lz4_block_decompress(blob, size);
        else {
            if (file_size < size) throw FormatError(m.name + ": uncompressed segment shorter than memory size");
            seg[i].assign(blob.begin(), blob.begin() + size);
        }
    }
    for (const auto& [va, w] : p.words) {
        const uint64_t off = va - m.base;
        if (va < m.base || off < mem_off[0] || off + 4 > mem_off[0] + seg[0].size())
            throw std::runtime_error("native patch: " + hex(va) + " is not in .text of " + m.name);
        std::memcpy(seg[0].data() + (off - mem_off[0]), &w, 4);
    }
    // Header and whatever lies before the first segment (module name) stay.
    std::vector<uint8_t> out(nso.begin(), nso.begin() + std::max<uint32_t>(first_file, 0x100));
    put32(out, 0x0C, flags & ~0x7u);
    for (int i = 0; i < 3; ++i) {
        put32(out, 0x10 + i * 0x10, static_cast<uint32_t>(out.size()));
        put32(out, 0x60 + i * 4, static_cast<uint32_t>(seg[i].size()));
        const auto h = sha256(seg[i]);
        std::memcpy(out.data() + 0xA0 + i * 0x20, h.data(), 32);
        out.insert(out.end(), seg[i].begin(), seg[i].end());
    }
    return out;
}

}  // namespace lgp::recomp
