// Jump-table recognition by forward symbolic evaluation along the unique-predecessor
// chain that ends in the BR block. Patterns handled are listed in
// docs/knowledge/jump-table-patterns.md; anything else is reported as unresolved.
#include "jump_table.hpp"

#include <array>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>

#include "jt_decode.hpp"
#include "lgp/recomp/a64_cf.hpp"

namespace lgp::recomp::detail {

namespace {

struct Expr;
using EP = std::shared_ptr<const Expr>;

struct Expr {
    enum class K : uint8_t { Unknown, Const, Reg, Add, Shl, Ext, Load } k = K::Unknown;
    uint64_t c = 0;          // Const
    uint8_t reg = 0;         // Reg
    uint8_t sh = 0;          // Shl amount
    jt::Ext ext = jt::Ext::Lsl;  // Ext kind
    uint8_t size = 0;        // Load size
    bool sgn = false;        // Load signed
    EP a, b;
};

// Every unknown value is a distinct opaque leaf: equal only to itself, but expressions can be
// built on top of it. A table index loaded from an unmodelled address must not hide the
// constant table/base around it (it used to collapse the whole expression).
EP unknown() { return std::make_shared<Expr>(); }
EP cst(uint64_t v) { auto e = std::make_shared<Expr>(); e->k = Expr::K::Const; e->c = v; return e; }
EP reg(uint8_t r) { auto e = std::make_shared<Expr>(); e->k = Expr::K::Reg; e->reg = r; return e; }

// Diagnostics: LGP_JT_TRACE=<br address, hex> prints how that BR is resolved to stderr
// (block chain, register expression, base search). Addresses and expressions only, no code.
bool g_trace = false;
void trace(const char* fmt, ...) {
    if (!g_trace) return;
    va_list ap;
    va_start(ap, fmt);
    std::fputs("jt-trace: ", stderr);
    std::vfprintf(stderr, fmt, ap);
    std::fputc('\n', stderr);
    va_end(ap);
}
std::string show(const EP& e) {
    if (!e) return "null";
    char buf[48];
    switch (e->k) {
        case Expr::K::Unknown: std::snprintf(buf, sizeof buf, "?%p", static_cast<const void*>(e.get())); return buf;
        case Expr::K::Const: std::snprintf(buf, sizeof buf, "0x%llx", static_cast<unsigned long long>(e->c)); return buf;
        case Expr::K::Reg: return "x" + std::to_string(e->reg);
        case Expr::K::Add: return "(" + show(e->a) + " + " + show(e->b) + ")";
        case Expr::K::Shl: return "(" + show(e->a) + " << " + std::to_string(e->sh) + ")";
        case Expr::K::Ext: return "ext" + std::to_string(static_cast<int>(e->ext)) + "(" + show(e->a) + ")";
        case Expr::K::Load: return "load" + std::to_string(e->size) + (e->sgn ? "s" : "") + "[" + show(e->a) + "]";
    }
    return "?";
}

bool equal(const EP& x, const EP& y) {
    if (x == y) return true;
    if (!x || !y || x->k != y->k) return false;
    switch (x->k) {
        case Expr::K::Unknown: return false;
        case Expr::K::Const: return x->c == y->c;
        case Expr::K::Reg: return x->reg == y->reg;
        case Expr::K::Add: return equal(x->a, y->a) && equal(x->b, y->b);
        case Expr::K::Shl: return x->sh == y->sh && equal(x->a, y->a);
        case Expr::K::Ext: return x->ext == y->ext && equal(x->a, y->a);
        case Expr::K::Load: return x->size == y->size && x->sgn == y->sgn && equal(x->a, y->a);
    }
    return false;
}

EP add(EP x, EP y) {
    if (x->k == Expr::K::Const && y->k == Expr::K::Const) return cst(x->c + y->c);
    if (x->k == Expr::K::Const) std::swap(x, y);           // constant on the right
    if (y->k == Expr::K::Const && y->c == 0) return x;
    if (x->k == Expr::K::Add && x->b->k == Expr::K::Const && y->k == Expr::K::Const)
        return add(x->a, cst(x->b->c + y->c));             // (a + c1) + c2
    auto e = std::make_shared<Expr>();
    e->k = Expr::K::Add; e->a = x; e->b = y;
    return e;
}
EP shl(EP x, uint8_t n) {
    if (n == 0) return x;
    if (x->k == Expr::K::Const) return cst(x->c << n);
    auto e = std::make_shared<Expr>();
    e->k = Expr::K::Shl; e->a = x; e->sh = n;
    return e;
}
EP ext(EP x, jt::Ext kind) {
    if (kind == jt::Ext::Lsl) return x;
    if (x->k == Expr::K::Const) {
        uint64_t v = x->c;
        switch (kind) {
            case jt::Ext::Uxtb: return cst(v & 0xFF);
            case jt::Ext::Uxth: return cst(v & 0xFFFF);
            case jt::Ext::Uxtw: return cst(v & 0xFFFFFFFF);
            case jt::Ext::Sxtb: return cst(static_cast<uint64_t>(static_cast<int64_t>(static_cast<int8_t>(v))));
            case jt::Ext::Sxth: return cst(static_cast<uint64_t>(static_cast<int64_t>(static_cast<int16_t>(v))));
            case jt::Ext::Sxtw: return cst(static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(v))));
            default: return x;
        }
    }
    auto e = std::make_shared<Expr>();
    e->k = Expr::K::Ext; e->a = x; e->ext = kind;
    return e;
}
EP load(const LinkedImage& img, EP addr, uint8_t size, bool sgn) {
    if (addr->k == Expr::K::Const) {
        // Constant address: fold the load if the location is mapped (literal pool, GOT).
        try {
            uint64_t v = 0;
            switch (size) {
                case 1: v = img.read_u8(addr->c); if (sgn) v = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int8_t>(v))); break;
                case 2: v = img.read_u32(addr->c) & 0xFFFF; if (sgn) v = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int16_t>(v))); break;
                case 4: v = img.read_u32(addr->c); if (sgn) v = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(v))); break;
                default: v = img.read_u64(addr->c); break;
            }
            auto e = std::make_shared<Expr>();
            e->k = Expr::K::Load; e->a = addr; e->size = size; e->sgn = sgn; e->c = v;
            e->b = cst(v);  // folded value kept in b
            return e;
        } catch (const std::out_of_range&) {
            return unknown();
        }
    }
    auto e = std::make_shared<Expr>();
    e->k = Expr::K::Load; e->a = addr; e->size = size; e->sgn = sgn;
    return e;
}
// Strip zero/sign extensions from a W register (index arithmetic is small).
EP strip(EP x) {
    while (x->k == Expr::K::Ext && (x->ext == jt::Ext::Uxtw || x->ext == jt::Ext::Sxtw)) x = x->a;
    return x;
}

struct Bound {
    EP idx;
    uint64_t count;
    bool is_unsigned;  // from HI/LS/LO/HS: idx < count also for "negative" values
};

struct State {
    std::array<EP, 32> r;
    EP cmp_lhs;
    bool cmp_valid = false;
    int64_t cmp_imm = 0;
    std::vector<Bound> bounds;
    bool opaque_moves = false;  // MOVZ/MOVN give opaque values (second pass, see resolve_indirect)

    State() { for (uint8_t i = 0; i < 32; ++i) r[i] = reg(i); }
    EP get(uint8_t n) const { return n == 31 ? cst(0) : r[n]; }  // 31 reads as XZR in our subset
    void set(uint8_t n, EP e) { if (n != 31) r[n] = std::move(e); }
};

// Instructions outside the modelled subset that set NZCV (an unmodelled one keeps the
// last CMP valid otherwise). Conservative for the DP-register class with bit 28 set (ADCS,
// CCMP, ...) and for branches/system (MSR NZCV, SETF/RMIF via the system class).
bool sets_flags(uint32_t insn) {
    const uint32_t op0 = (insn >> 25) & 0xF;
    if (op0 == 0xA || op0 == 0xB || op0 == 0xD) return true;
    const bool s = (insn >> 29) & 1;
    if ((insn & 0x1F000000u) == 0x11000000u) return s;                                  // ADDS/SUBS (imm)
    if ((insn & 0x1F000000u) == 0x0B000000u) return s;                                  // ADDS/SUBS (reg)
    if ((insn & 0x1F800000u) == 0x12000000u) return ((insn >> 29) & 3) == 3;            // ANDS (imm)
    if ((insn & 0x1F000000u) == 0x0A000000u) return ((insn >> 29) & 3) == 3;            // ANDS/BICS (reg)
    if ((insn & 0x5F203C00u) == 0x1E202000u) return true;                               // FCMP/FCMPE
    if ((insn & 0x5F200C00u) == 0x1E200400u) return true;                               // FCCMP/FCCMPE
    return false;
}

// SIMD&FP data processing that writes a general register (its Rd); everything else in the
// class writes a V register (or nothing), not Xn of the same number.
bool simd_writes_gp(uint32_t insn) {
    if ((insn & 0x5F20FC00u) == 0x1E200000u) {  // FP <-> integer conversion
        const uint32_t opcode = (insn >> 16) & 7;
        return opcode != 2 && opcode != 3 && opcode != 7;  // not SCVTF/UCVTF/FMOV to V
    }
    if ((insn & 0x5F200000u) == 0x1E000000u) {  // FP <-> fixed-point conversion
        const uint32_t opcode = (insn >> 16) & 7;
        return opcode != 2 && opcode != 3;
    }
    if ((insn & 0x9FE08400u) == 0x0E000400u) {  // Advanced SIMD copy: SMOV/UMOV
        const uint32_t imm4 = (insn >> 11) & 0xF;
        return imm4 == 5 || imm4 == 7;
    }
    return false;
}

// Loads/stores outside the modelled subset: only loads into general registers and
// written-back bases change Xn. SIMD&FP loads write V registers, stores write nothing.
void invalidate_ldst(State& st, uint32_t insn) {
    const uint8_t rt = insn & 0x1F, rn = (insn >> 5) & 0x1F, rt2 = (insn >> 10) & 0x1F, rs = (insn >> 16) & 0x1F;
    const bool v = (insn >> 26) & 1;
    const uint32_t cls = (insn >> 27) & 7;  // bit 27 is always set in this class
    if (cls == 5) {  // load/store pair (incl. no-allocate)
        if (!v && ((insn >> 22) & 1)) { st.set(rt, unknown()); st.set(rt2, unknown()); }
        const uint32_t idx = (insn >> 23) & 3;
        if (idx == 1 || idx == 3) st.set(rn, unknown());
        return;
    }
    if (cls == 7) {  // load/store register
        const bool uimm = (insn >> 24) & 1, b21 = (insn >> 21) & 1;
        const uint32_t op4 = (insn >> 10) & 3;
        if (!uimm && b21 && op4 != 2) {  // atomic memory operations, LDRAA/LDRAB
            st.set(rt, unknown());
            if (op4 & 1) st.set(rn, unknown());
            return;
        }
        if (!v && ((insn >> 22) & 3) != 0) st.set(rt, unknown());  // opc 00 = store
        if (!uimm && !b21 && (op4 == 1 || op4 == 3)) st.set(rn, unknown());  // post/pre-index
        return;
    }
    if (v) {  // SIMD&FP literal loads and structure loads/stores: only the post-index base
        if (cls == 1 && ((insn >> 23) & 1)) st.set(rn, unknown());
        return;
    }
    // Only fields these encodings really use: in a literal load bits 20:16 and 14:10 are part
    // of imm19, not registers (treating them as Rs/Rt2 lost a table base in the sdk).
    const bool load = (insn >> 22) & 1;
    if (cls == 1) {  // exclusive, load-acquire/store-release, compare-and-swap
        const bool o2 = (insn >> 23) & 1, o1 = (insn >> 21) & 1;
        if (!o2) {  // LDXR/STXR (o1: pairs, CASP)
            if (load) { st.set(rt, unknown()); if (o1) st.set(rt2, unknown()); }
            if (!load || o1) st.set(rs, unknown());                                   // status / CASP old value
            if (o1 && !((insn >> 31) & 1)) st.set(static_cast<uint8_t>((rs + 1) & 31), unknown());  // CASP pair
        } else if (o1) {  // CAS: Rs receives the old value
            st.set(rs, unknown());
        } else if (load) {  // LDAR/LDLAR
            st.set(rt, unknown());
        }
        return;
    }
    // cls 3: literal (bits 25:24 = 00; PRFM writes nothing), LDAPUR/STLUR, memory tagging.
    const uint32_t op24 = (insn >> 24) & 3;
    if (op24 == 0) {
        if (((insn >> 30) & 3) != 3) st.set(rt, unknown());
        return;
    }
    st.set(rt, unknown());
    if (op24 == 1 && ((insn >> 10) & 1)) st.set(rn, unknown());  // STG & co. pre/post-index
}

// Registers an instruction we do not model may write (conservative).
void invalidate_other(State& st, uint32_t insn) {
    if (sets_flags(insn)) st.cmp_valid = false;
    const uint32_t op0 = (insn >> 25) & 0xF;
    if ((op0 & 0x7) == 0x7) {  // SIMD&FP
        if (simd_writes_gp(insn)) st.set(insn & 0x1F, unknown());
        return;
    }
    if ((op0 & 0x5) == 0x4) { invalidate_ldst(st, insn); return; }
    st.set(insn & 0x1F, unknown());
}

void step(const LinkedImage& img, State& st, uint32_t insn, uint64_t pc) {
    CfInfo cf = classify(insn, pc);
    if (cf.kind == CfKind::Call || cf.kind == CfKind::IndirectCall) {
        for (uint8_t i = 0; i <= 18; ++i) st.set(i, unknown());
        st.set(30, unknown());
        st.cmp_valid = false;
        return;
    }
    if (cf.kind != CfKind::None) return;
    jt::Insn d = jt::decode(insn, pc);
    using jt::Op;
    switch (d.op) {
        case Op::Adr: case Op::Adrp: st.set(d.rd, cst(static_cast<uint64_t>(d.imm))); break;
        case Op::AddImm:
            if (d.rn == 31) st.set(d.rd, unknown());  // SP-relative
            else st.set(d.rd, add(st.get(d.rn), cst(static_cast<uint64_t>(d.imm))));
            break;
        case Op::SubsImm:
            st.cmp_lhs = d.rn == 31 ? unknown() : st.get(d.rn);
            st.cmp_imm = d.imm;
            st.cmp_valid = d.rn != 31;
            if (d.rd != 31) st.set(d.rd, d.rn == 31 ? unknown() : add(st.get(d.rn), cst(static_cast<uint64_t>(-d.imm))));
            break;
        case Op::AddShifted:
            if (d.is_sub) st.set(d.rd, unknown());
            else st.set(d.rd, add(st.get(d.rn), shl(st.get(d.rm), d.shift)));
            break;
        case Op::SubsShifted:
            st.cmp_valid = false;
            if (d.rd != 31) st.set(d.rd, unknown());
            break;
        case Op::AddExt:
            if (d.rn == 31) st.set(d.rd, unknown());
            else st.set(d.rd, add(st.get(d.rn), shl(ext(st.get(d.rm), d.ext), d.shift)));
            break;
        case Op::MovReg: st.set(d.rd, d.sf ? st.get(d.rm) : ext(st.get(d.rm), jt::Ext::Uxtw)); break;
        case Op::Movz:
            st.set(d.rd, st.opaque_moves ? unknown() : cst(static_cast<uint64_t>(d.imm) << d.shift));
            break;
        case Op::Movn: {
            if (st.opaque_moves) { st.set(d.rd, unknown()); break; }
            uint64_t v = ~(static_cast<uint64_t>(d.imm) << d.shift);
            st.set(d.rd, cst(d.sf ? v : (v & 0xFFFFFFFF)));
            break;
        }
        case Op::Movk: {
            EP cur = st.get(d.rd);
            if (cur->k == Expr::K::Const)
                st.set(d.rd, cst((cur->c & ~(uint64_t(0xFFFF) << d.shift)) | (static_cast<uint64_t>(d.imm) << d.shift)));
            else st.set(d.rd, unknown());
            break;
        }
        case Op::AndImm:
            if (static_cast<uint64_t>(d.imm) == 0xFFFFFFFF) st.set(d.rd, ext(st.get(d.rn), jt::Ext::Uxtw));
            else if (static_cast<uint64_t>(d.imm) == 0xFF) st.set(d.rd, ext(st.get(d.rn), jt::Ext::Uxtb));
            else if (static_cast<uint64_t>(d.imm) == 0xFFFF) st.set(d.rd, ext(st.get(d.rn), jt::Ext::Uxth));
            else st.set(d.rd, unknown());
            break;
        case Op::Ubfm: {
            unsigned regsize = d.sf ? 64 : 32;
            if (d.immr == 0 && d.imms == regsize - 1) st.set(d.rd, d.sf ? st.get(d.rn) : ext(st.get(d.rn), jt::Ext::Uxtw));
            else if (d.imms + 1u == d.immr) st.set(d.rd, shl(st.get(d.rn), static_cast<uint8_t>(regsize - d.immr)));  // LSL
            else if (d.immr == 0 && d.imms == 7) st.set(d.rd, ext(st.get(d.rn), jt::Ext::Uxtb));
            else if (d.immr == 0 && d.imms == 15) st.set(d.rd, ext(st.get(d.rn), jt::Ext::Uxth));
            else if (d.immr == 0 && d.imms == 31) st.set(d.rd, ext(st.get(d.rn), jt::Ext::Uxtw));
            else st.set(d.rd, unknown());
            break;
        }
        case Op::Sbfm:
            if (d.immr == 0 && d.imms == 7) st.set(d.rd, ext(st.get(d.rn), jt::Ext::Sxtb));
            else if (d.immr == 0 && d.imms == 15) st.set(d.rd, ext(st.get(d.rn), jt::Ext::Sxth));
            else if (d.immr == 0 && d.imms == 31) st.set(d.rd, ext(st.get(d.rn), jt::Ext::Sxtw));
            else st.set(d.rd, unknown());
            break;
        case Op::LdrLit: st.set(d.rd, load(img, cst(static_cast<uint64_t>(d.imm)), d.size, d.load_signed)); break;
        case Op::LdrRegOff:
            if (d.rn == 31) st.set(d.rd, unknown());
            else st.set(d.rd, load(img, add(st.get(d.rn), shl(ext(st.get(d.rm), d.ext), d.shift)), d.size, d.load_signed));
            break;
        case Op::LdrImm:
            if (d.rn == 31) st.set(d.rd, unknown());
            else st.set(d.rd, load(img, add(st.get(d.rn), cst(static_cast<uint64_t>(d.imm))), d.size, d.load_signed));
            break;
        case Op::Other: invalidate_other(st, insn); break;
    }
}

// Upper bound (exclusive count) implied by "cmp idx, #N" followed by a taken/not-taken B.cond.
bool bound_from_cond(uint8_t cond, bool taken, int64_t n, uint64_t& count, bool& is_unsigned) {
    if (!taken) cond ^= 1;  // invert condition (ARM: cond^1 is the inverse for cond < 14)
    if (n < 0) return false;
    is_unsigned = cond == 0x9 || cond == 0x3;
    switch (cond) {
        case 0x9:  // LS: idx <= N
        case 0xD:  // LE
            count = static_cast<uint64_t>(n) + 1; return true;
        case 0x3:  // CC/LO: idx < N
        case 0xB:  // LT
            if (n == 0) return false;
            count = static_cast<uint64_t>(n); return true;
        default: return false;
    }
}

// x + c with c a small negative constant (`sub wd, wn, #k`): returns k and x.
bool minus_const(const EP& e, uint64_t& k, EP& x) {
    if (e->k != Expr::K::Add || e->b->k != Expr::K::Const) return false;
    const uint64_t neg = 0 - e->b->c;
    if (neg == 0 || neg > 0xFFFF) return false;
    k = neg;
    x = strip(e->a);
    return true;
}

struct Pattern {
    bool ok = false;
    bool absolute = false;
    uint64_t table = 0, base = 0;
    uint8_t size = 0, shift = 0;
    bool sgn = false;
    EP idx;
};

// Try to see `load(table + idx*size)` (optionally sign/zero extended and shifted) in e.
bool match_entry(EP e, uint8_t& shift, EP& load_e) {
    shift = 0;
    if (e->k == Expr::K::Shl) { shift = e->sh; e = e->a; }
    while (e->k == Expr::K::Ext) e = e->a;
    if (e->k == Expr::K::Shl && shift == 0) { shift = e->sh; e = e->a; while (e->k == Expr::K::Ext) e = e->a; }
    if (e->k != Expr::K::Load || e->size == 8) return false;
    load_e = e;
    return true;
}

bool match_index(EP addr, uint8_t size, uint64_t& table, EP& idx) {
    if (addr->k != Expr::K::Add) return false;
    EP t = addr->b, i = addr->a;
    if (t->k != Expr::K::Const) { std::swap(t, i); if (t->k != Expr::K::Const) return false; }
    table = t->c;
    unsigned want = size == 1 ? 0 : size == 2 ? 1 : size == 4 ? 2 : 3;
    EP s = strip(i);
    if (want == 0) { idx = s; return true; }
    if (s->k == Expr::K::Shl && s->sh == want) { idx = strip(s->a); return true; }
    return false;
}

Pattern match(EP e) {
    Pattern p;
    // Absolute table: load64(table + idx*8)
    if (e->k == Expr::K::Load && e->size == 8) {
        if (match_index(e->a, 8, p.table, p.idx)) { p.ok = true; p.absolute = true; p.size = 8; p.base = p.table; }
        return p;
    }
    if (e->k != Expr::K::Add) return p;
    EP base = e->b, entry = e->a;
    if (base->k != Expr::K::Const) { std::swap(base, entry); if (base->k != Expr::K::Const) return p; }
    EP l;
    if (!match_entry(entry, p.shift, l)) return p;
    if (!match_index(l->a, l->size, p.table, p.idx)) return p;
    p.ok = true;
    p.base = base->c;
    p.size = l->size;
    p.sgn = l->sgn;
    // sign given by an explicit sxt* around the load
    for (EP x = entry; x->k == Expr::K::Shl || x->k == Expr::K::Ext; x = x->a)
        if (x->k == Expr::K::Ext && (x->ext == jt::Ext::Sxtb || x->ext == jt::Ext::Sxth || x->ext == jt::Ext::Sxtw)) p.sgn = true;
    return p;
}

std::optional<uint64_t> read_target(const LinkedImage& img, const JumpTable& t, uint32_t i) {
    try {
        uint64_t at = t.table_va + static_cast<uint64_t>(i) * t.entry_size;
        uint64_t v;
        switch (t.entry_size) {
            case 1: v = img.read_u8(at); if (t.entries_signed) v = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int8_t>(v))); break;
            case 2: v = img.read_u32(at & ~uint64_t(3)) >> ((at & 3) * 8) & 0xFFFF; if (t.entries_signed) v = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int16_t>(v))); break;
            case 4: v = img.read_u32(at); if (t.entries_signed) v = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(v))); break;
            default: return img.read_u64(at);
        }
        return t.base_va + (v << t.shift);
    } catch (const std::out_of_range&) {
        return std::nullopt;
    }
}

// Register leaves of e.
void reg_leaves(const EP& e, std::vector<uint8_t>& out) {
    if (!e) return;
    if (e->k == Expr::K::Reg) {
        for (uint8_t r : out) if (r == e->reg) return;
        out.push_back(e->reg);
        return;
    }
    if (e->k == Expr::K::Load) { reg_leaves(e->a, out); return; }  // b holds a folded value, not a leaf
    reg_leaves(e->a, out);
    reg_leaves(e->b, out);
}

// e with every Reg(r) leaf replaced by v, rebuilt through the folding constructors.
EP subst(const LinkedImage& img, const EP& e, uint8_t r, const EP& v) {
    switch (e->k) {
        case Expr::K::Reg: return e->reg == r ? v : e;
        case Expr::K::Add: return add(subst(img, e->a, r, v), subst(img, e->b, r, v));
        case Expr::K::Shl: return shl(subst(img, e->a, r, v), e->sh);
        case Expr::K::Ext: return ext(subst(img, e->a, r, v), e->ext);
        case Expr::K::Load: return e->a->k == Expr::K::Const ? e : load(img, subst(img, e->a, r, v), e->size, e->sgn);
        default: return e;
    }
}

// LDR/LDP (integer) with SP as base: in compiled code the epilogue restoring callee-saved
// registers, which does not change their value for the rest of the body.
bool sp_load(uint32_t insn) {
    if (((insn >> 5) & 31) != 31) return false;
    if ((insn & 0x3C400000u) == 0x28400000u) return true;                       // LDP (any index mode)
    if ((insn & 0x3F400000u) == 0x39400000u) return true;                       // LDR (unsigned offset)
    return (insn & 0x3B600000u) == 0x38400000u && ((insn >> 10) & 1) == 1;    // LDR pre/post-indexed
}

// STR/STP of any width (not exclusive stores): writes no register except a written-back base.
bool plain_store(uint32_t insn) {
    if ((insn & 0x3A000000u) == 0x28000000u) return ((insn >> 22) & 1) == 0;              // STP
    return (insn & 0x3A000000u) == 0x38000000u && ((insn >> 22) & 3) == 0;              // STR
}

// Value of register r at br_pc if the code of [fn_start, fn_bound) defines it by `adrp/adr r`
// (optionally followed by `add r, r, #imm`) before br_pc and nothing else can change it: a
// table base set up in front of a loop whose head ends the unique-predecessor chain. The scan
// is linear over the whole range, not over the blocks found so far, so code reached only
// through the table itself counts too.
// - The definition is the last write before br_pc; earlier writes are overwritten by it.
// - Writes behind br_pc: further adrp/add definitions (the next table of the same function,
//   sdk get_abbreviated_name) are fine. Any other write W behind it must not reach the BR: no
//   direct branch at or behind W may jump into (definition, br_pc] (sdk make_graymap reuses
//   its base register after the loop). An indirect branch behind W sets *weak; the caller then
//   only accepts a bounded table whose targets all lie in the function.
// - Calls and SVCs clobber x0-x18/x30 and x0-x7: in front of the definition they do not
//   matter, between definition and BR they do, behind the BR they count as later writes.
// - Any other unmodelled instruction counts as a write of its Rt/Rd field (conservative),
//   except stores and SP-based loads of callee-saved x19-x29 (epilogue restores).
// A wrong base still has to pass the table checks (targets aligned, inside .text).
std::optional<uint64_t> function_constant(const LinkedImage& img, uint8_t r, uint64_t fn_start, uint64_t fn_bound,
                                          uint64_t br_pc, bool* weak) {
    if (r == 31) return std::nullopt;
    std::vector<uint64_t> writes, clobbers;
    for (uint64_t pc = fn_start; pc < fn_bound; pc += 4) {
        uint32_t insn;
        try { insn = img.read_u32(pc); } catch (const std::out_of_range&) { trace("x%u: unreadable code", r); return std::nullopt; }
        CfInfo cf = classify(insn, pc);
        bool w = false;
        if (cf.kind == CfKind::Svc) {
            if (r <= 7) clobbers.push_back(pc);
        } else if ((cf.kind == CfKind::Call || cf.kind == CfKind::IndirectCall) && (r <= 18 || r == 30)) {
            clobbers.push_back(pc);
        } else if (r >= 19 && r <= 29 && sp_load(insn)) {
            w = false;
        } else if (plain_store(insn)) {
            w = ((insn >> 5) & 31) == r;  // conservative: base may be written back
        } else {
            State st;
            EP before = st.get(r);
            step(img, st, insn, pc);
            w = st.get(r) != before;
        }
        if (w) writes.push_back(pc);
    }
    auto op_at = [&](uint64_t pc) { return jt::decode(img.read_u32(pc), pc); };
    auto is_def = [&](uint64_t pc) { const jt::Insn d = op_at(pc); return d.op == jt::Op::Adrp || d.op == jt::Op::Adr; };
    auto is_add = [&](uint64_t pc) { const jt::Insn d = op_at(pc); return d.op == jt::Op::AddImm && d.rn == r; };
    // Last write before the BR, as `def` or `def; add`.
    size_t last = writes.size();
    for (size_t k = 0; k < writes.size(); ++k)
        if (writes[k] < br_pc) last = k;
    if (g_trace)
        for (uint64_t w : writes) trace("x%u: write at 0x%llx%s", r, static_cast<unsigned long long>(w), w < br_pc ? "" : " (behind br)");
    if (last == writes.size()) { trace("x%u: no write before the br", r); return std::nullopt; }
    size_t def = last;
    if (!is_def(writes[last])) {
        if (last == 0 || writes[last - 1] + 4 != writes[last] || !is_add(writes[last]) || !is_def(writes[last - 1]))
        {
            trace("x%u: last write before the br (0x%llx) is not adrp/adr[+add]", r, static_cast<unsigned long long>(writes[last]));
            return std::nullopt;
        }
        def = last - 1;
    }
    // Later writes: further definitions (each optionally followed by its add), or writes that
    // no branch from behind them leads back into the stretch the value must survive.
    uint64_t first_other = 0;
    for (size_t k = last + 1; k < writes.size() && first_other == 0; ++k) {
        if (is_def(writes[k])) continue;
        if (is_add(writes[k]) && writes[k - 1] + 4 == writes[k] && is_def(writes[k - 1])) continue;
        first_other = writes[k];
    }
    // Calls/SVCs: between definition and BR they always clobber; behind the BR they are
    // treated like the other later writes.
    for (uint64_t pc : clobbers) {
        if (pc > writes[def] && pc < br_pc) {
            trace("x%u: call/svc at 0x%llx between definition and br", r, static_cast<unsigned long long>(pc));
            return std::nullopt;
        }
        if (pc > br_pc && (first_other == 0 || pc < first_other)) first_other = pc;
    }
    if (first_other != 0) {
        for (uint64_t pc = first_other; pc < fn_bound; pc += 4) {
            const CfInfo cf = classify(img.read_u32(pc), pc);
            if (cf.kind == CfKind::Branch || cf.kind == CfKind::CondBranch) {
                if (cf.target > writes[def] && cf.target <= br_pc) {
                    trace("x%u: branch at 0x%llx behind the write at 0x%llx leads back to 0x%llx", r,
                          static_cast<unsigned long long>(pc), static_cast<unsigned long long>(first_other),
                          static_cast<unsigned long long>(cf.target));
                    return std::nullopt;
                }
            } else if (cf.kind == CfKind::IndirectBranch) {
                *weak = true;
            }
        }
    }
    State st;
    for (size_t k = def; k <= last; ++k) step(img, st, img.read_u32(writes[k]), writes[k]);
    EP v = st.get(r);
    if (v->k != Expr::K::Const) { trace("x%u: definition is not constant", r); return std::nullopt; }
    trace("x%u: constant 0x%llx (definition at 0x%llx)%s", r, static_cast<unsigned long long>(v->c),
          static_cast<unsigned long long>(writes[def]), *weak ? ", weak" : "");
    return v->c;
}

}  // namespace

IndirectResolution resolve_indirect(const LinkedImage& img, const std::map<uint64_t, BasicBlock>& blocks,
                                    uint64_t br_pc, uint64_t fn_start, uint64_t fn_bound, uint32_t max_scan) {
    IndirectResolution res;
    if (const char* want = std::getenv("LGP_JT_TRACE")) g_trace = std::strtoull(want, nullptr, 16) == br_pc;
    else g_trace = false;
    trace("br 0x%llx in function 0x%llx-0x%llx", static_cast<unsigned long long>(br_pc),
          static_cast<unsigned long long>(fn_start), static_cast<unsigned long long>(fn_bound));
    auto it = blocks.upper_bound(br_pc);
    if (it == blocks.begin()) { res.note = "br outside known blocks"; trace("%s", res.note.c_str()); return res; }
    --it;
    const BasicBlock* cur = &it->second;
    if (br_pc < cur->start || br_pc >= cur->end) { res.note = "br outside known blocks"; trace("%s", res.note.c_str()); return res; }

    // Unique-predecessor chain (at most 6 blocks).
    std::vector<const BasicBlock*> chain{cur};
    for (int depth = 0; depth < 6; ++depth) {
        const BasicBlock* pred = nullptr;
        int npred = 0;
        for (const auto& [s, b] : blocks)
            for (uint64_t succ : b.successors)
                if (succ == chain.front()->start) { ++npred; pred = &b; }
        if (npred != 1 || pred == chain.front()) break;
        bool loop = false;
        for (auto* c : chain) if (c == pred) loop = true;
        if (loop) break;
        chain.insert(chain.begin(), pred);
    }

    for (const BasicBlock* c : chain)
        trace("chain block 0x%llx-0x%llx", static_cast<unsigned long long>(c->start), static_cast<unsigned long long>(c->end));
    uint8_t br_reg = classify(img.read_u32(br_pc), br_pc).reg;
    auto evaluate = [&](bool opaque_moves) {
        State st;
        st.opaque_moves = opaque_moves;
        for (size_t bi = 0; bi < chain.size(); ++bi) {
            const BasicBlock* b = chain[bi];
            uint64_t stop = (b == cur) ? br_pc : b->end;
            for (uint64_t pc = b->start; pc < stop; pc += 4) {
                uint32_t insn = img.read_u32(pc);
                CfInfo cf = classify(insn, pc);
                if (cf.kind == CfKind::CondBranch && pc + 4 == b->end && bi + 1 < chain.size() &&
                    cf.cond_kind == CondKind::BCond && st.cmp_valid) {
                    bool taken = chain[bi + 1]->start == cf.target;
                    uint64_t count;
                    bool is_unsigned;
                    if (bound_from_cond(cf.cond, taken, st.cmp_imm, count, is_unsigned))
                        st.bounds.push_back({st.cmp_lhs, count, is_unsigned});
                }
                step(img, st, insn, pc);
            }
        }
        return st;
    };
    State st = evaluate(false);

    EP e = st.get(br_reg);
    trace("x%u = %s", br_reg, show(e).c_str());
    for (const Bound& b : st.bounds)
        trace("bound: %s < %llu (%s)", show(b.idx).c_str(), static_cast<unsigned long long>(b.count), b.is_unsigned ? "unsigned" : "signed");
    if (e->k == Expr::K::Const) {
        res.kind = IndirectResolution::Kind::Direct;
        res.target = e->c;
        res.note = "constant register";
        return res;
    }
    if (e->k == Expr::K::Load && e->size == 8 && e->a->k == Expr::K::Const && e->b) {
        res.kind = IndirectResolution::Kind::Direct;
        res.target = e->b->c;
        if (img.import_by_slot(e->a->c)) res.via_slot = e->a->c;
        res.note = res.via_slot ? "GOT slot" : "literal pool";
        return res;
    }
    bool weak_base = false;  // base from a definition that later writes might not dominate
    auto match_expr = [&](const EP& x, bool& weak) {
        Pattern m = match(x);
        if (!m.ok) {
            // Table base from before a loop head: substitute registers that are constant in the function.
            std::vector<uint8_t> leaves;
            reg_leaves(x, leaves);
            EP x2 = x;
            for (uint8_t r : leaves)
                if (auto v = function_constant(img, r, fn_start, fn_bound, br_pc, &weak)) x2 = subst(img, x2, r, cst(*v));
            if (x2 != x) m = match(x2);
        }
        return m;
    };
    Pattern p = match_expr(e, weak_base);
    if (!p.ok) {
        // A switch over a state variable inside a loop (sdk): before the table is resolved only the
        // entry path is known, and there the state is a MOVZ constant; the cases that store other
        // states and jump back are reached only through the table. The folded constant index
        // makes the table load a single fixed entry. Evaluate again with immediate moves opaque.
        // Only immediates: table addresses come from ADRP/ADR and stay constant.
        State st2 = evaluate(true);
        EP e2 = st2.get(br_reg);
        trace("again with opaque immediate moves: x%u = %s", br_reg, show(e2).c_str());
        bool weak2 = false;
        Pattern p2 = match_expr(e2, weak2);
        if (p2.ok) { st = std::move(st2); p = p2; weak_base = weak2; }
    }
    if (!p.ok) { res.note = "register expression does not match a table pattern"; trace("%s", res.note.c_str()); return res; }
    trace("table 0x%llx entry %u base 0x%llx, index %s", static_cast<unsigned long long>(p.table), p.size,
          static_cast<unsigned long long>(p.base), show(p.idx).c_str());

    JumpTable t;
    t.br_pc = br_pc;
    t.table_va = p.table;
    t.entry_size = p.size;
    t.entries_signed = p.sgn;
    t.base_va = p.absolute ? 0 : p.base;
    t.shift = p.shift;
    // The bound may sit on the index itself or differ from it by a constant offset k:
    // - cmp (v - k) but index v: the compiler moved the table address down by k entries
    //   (only v in [k, k + count) reaches the BR), so the table starts k entries later;
    // - unsigned cmp v but index v - k: values below k were handled before, v - k < count - k.
    for (const Bound& b : st.bounds) {
        const EP bi = strip(b.idx), pi = strip(p.idx);
        uint64_t k = 0, count = 0, skip = 0;
        EP x;
        if (equal(bi, pi)) count = b.count;
        else if (b.is_unsigned && minus_const(bi, k, x) && equal(x, pi)) { count = b.count; skip = k; }
        else if (b.is_unsigned && minus_const(pi, k, x) && equal(x, bi) && b.count > k) count = b.count - k;
        if (count == 0) continue;
        t.table_va = p.table + skip * p.size;
        t.count = static_cast<uint32_t>(std::min<uint64_t>(count, max_scan));
        t.bound_known = true;
    }
    if (!img.segment_at(t.table_va)) { res.note = "table address unmapped"; trace("%s", res.note.c_str()); return res; }

    auto valid = [&](uint64_t tgt) { return (tgt & 3) == 0 && tgt >= fn_start && tgt < fn_bound; };
    if (weak_base && !t.bound_known) { res.note = "table base not proven and table unbounded"; trace("%s", res.note.c_str()); return res; }
    if (t.bound_known) {
        bool all_inside = true;
        for (uint32_t i = 0; i < t.count; ++i) {
            auto tgt = read_target(img, t, i);
            if (!tgt || (*tgt & 3) || !img.in_text(*tgt)) { res.note = "bounded table has an entry outside .text"; trace("%s", res.note.c_str()); return res; }
            if (!valid(*tgt)) all_inside = false;
            t.targets.push_back(*tgt);
        }
        if (weak_base && !all_inside) { res.note = "table base not proven and an entry leaves the function"; trace("%s", res.note.c_str()); return res; }
        t.confidence = all_inside ? Confidence::High : Confidence::Medium;
    } else {
        for (uint32_t i = 0; i < max_scan; ++i) {
            auto tgt = read_target(img, t, i);
            if (!tgt || !valid(*tgt)) break;
            if (tgt == br_pc + 4 && i > 0 && t.entry_size == 8) break;  // absolute tables rarely jump right behind the BR
            t.targets.push_back(*tgt);
        }
        if (t.targets.empty()) { res.note = "unbounded table has no valid first entry"; trace("%s", res.note.c_str()); return res; }
        t.count = static_cast<uint32_t>(t.targets.size());
        t.confidence = Confidence::Low;
    }
    trace("table: %u entries, bound %s", t.count, t.bound_known ? "known" : "scanned");
    res.kind = IndirectResolution::Kind::Table;
    res.table = std::move(t);
    return res;
}

}  // namespace lgp::recomp::detail
