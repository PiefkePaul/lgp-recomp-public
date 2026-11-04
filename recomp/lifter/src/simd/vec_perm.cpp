// SIMD&FP lifter module: vec_perm (P2.4).
//
// Advanced SIMD data movement (Arm ARM DDI 0487, C4.1.95 ff.):
//   copy           DUP (element), DUP (general), INS (general), INS (element), UMOV, SMOV
//   scalar copy    DUP (element) scalar, alias MOV (scalar)
//   permute        UZP1/UZP2, TRN1/TRN2, ZIP1/ZIP2
//   extract        EXT
//   table lookup   TBL/TBX with 1-4 table registers
//   modified imm.  MOVI/MVNI/ORR/BIC (vector, immediate), MOVI (64-bit scalar/2D), FMOV (vector, immediate)
#include "internal.hpp"

namespace lgp::recomp::a64::simd::detail {
namespace {

const char* const kNs = "lgp::lifted::simd::";

const char* utype(unsigned size) {
    static const char* const k[] = {"uint8_t", "uint16_t", "uint32_t", "uint64_t"};
    return k[size];
}
const char* stype(unsigned size) {
    static const char* const k[] = {"int8_t", "int16_t", "int32_t", "int64_t"};
    return k[size];
}
std::string qstr(bool q) { return q ? "true" : "false"; }

// Element size encoded by the lowest set bit of imm5 (4 = unallocated).
unsigned imm5_size(uint32_t imm5) {
    for (unsigned s = 0; s < 4; ++s)
        if (imm5 & (1u << s)) return s;
    return 4;
}

std::string lane_expr(unsigned size, unsigned reg, unsigned idx) {
    return std::string(kNs) + "lane<" + utype(size) + ">(" + vreg(reg) + ", " + num(idx) + ")";
}

Lifted lift_copy(uint32_t raw, bool emit) {
    const bool q = bit(raw, 30);
    const bool op = bit(raw, 29);
    const uint32_t imm5 = bits(raw, 20, 16);
    const uint32_t imm4 = bits(raw, 14, 11);
    const unsigned rn = bits(raw, 9, 5), rd = bits(raw, 4, 0);
    const unsigned size = imm5_size(imm5);
    if (size > 3) return invalid();
    const unsigned idx = imm5 >> (size + 1);

    if (op) {  // INS (element)
        if (!q) return invalid();
        if (!emit) return ok("ins");
        const unsigned idx2 = imm4 >> size;
        return ok("ins", std::string(kNs) + "set_lane<" + utype(size) + ">(" + vreg(rd) + ", " + num(idx) + ", " +
                             lane_expr(size, rn, idx2) + ");");
    }
    switch (imm4) {
    case 0b0000:  // DUP (element)
        if (size == 3 && !q) return invalid();
        if (!emit) return ok("dup");
        return ok("dup", vreg(rd) + " = " + kNs + "dup<" + utype(size) + ">(" + lane_expr(size, rn, idx) + ", " +
                             qstr(q) + ");");
    case 0b0001:  // DUP (general)
        if (size == 3 && !q) return invalid();
        if (!emit) return ok("dup");
        return ok("dup", vreg(rd) + " = " + kNs + "dup<" + utype(size) + ">(static_cast<" + utype(size) + ">(" +
                             xreg(rn) + "), " + qstr(q) + ");");
    case 0b0011:  // INS (general)
        if (!q) return invalid();
        if (!emit) return ok("ins");
        return ok("ins", std::string(kNs) + "set_lane<" + utype(size) + ">(" + vreg(rd) + ", " + num(idx) +
                             ", static_cast<" + utype(size) + ">(" + xreg(rn) + "));");
    case 0b0101: {  // SMOV
        if (q ? size > 2 : size > 1) return invalid();
        if (!emit) return ok("smov");
        const std::string s = "static_cast<" + std::string(stype(size)) + ">(" + lane_expr(size, rn, idx) + ")";
        const std::string e = q ? "static_cast<uint64_t>(static_cast<int64_t>(" + s + "))"
                                : "static_cast<uint64_t>(static_cast<uint32_t>(static_cast<int32_t>(" + s + ")))";
        return ok("smov", set_x(rd, e));
    }
    case 0b0111:  // UMOV
        if (q ? size != 3 : size > 2) return invalid();
        if (!emit) return ok("umov");
        return ok("umov", set_x(rd, "static_cast<uint64_t>(" + lane_expr(size, rn, idx) + ")"));
    default: return invalid();
    }
}

Lifted lift_scalar_copy(uint32_t raw, bool emit) {
    const uint32_t imm5 = bits(raw, 20, 16);
    const unsigned size = imm5_size(imm5);
    if (bit(raw, 29) || bits(raw, 14, 11) != 0 || size > 3) return invalid();
    if (!emit) return ok("dup");
    const unsigned idx = imm5 >> (size + 1);
    return ok("dup", vreg(bits(raw, 4, 0)) + " = " + kNs + "from_u64(static_cast<uint64_t>(" +
                         lane_expr(size, bits(raw, 9, 5), idx) + "));");
}

Lifted lift_permute(uint32_t raw, bool emit) {
    const bool q = bit(raw, 30);
    const unsigned size = bits(raw, 23, 22);
    const uint32_t opc = bits(raw, 14, 12);
    if (size == 3 && !q) return invalid();
    static const char* const kNames[] = {nullptr, "uzp1", "trn1", "zip1", nullptr, "uzp2", "trn2", "zip2"};
    static const char* const kFns[] = {nullptr, "uzp", "trn", "zip", nullptr, "uzp", "trn", "zip"};
    if (!kNames[opc]) return invalid();
    if (!emit) return ok(kNames[opc]);
    const unsigned part = opc >> 2;
    return ok(kNames[opc], vreg(bits(raw, 4, 0)) + " = " + kNs + kFns[opc] + "<" + utype(size) + ">(" +
                               vreg(bits(raw, 9, 5)) + ", " + vreg(bits(raw, 20, 16)) + ", " + num(part) + ", " +
                               qstr(q) + ");");
}

Lifted lift_extract(uint32_t raw, bool emit) {
    const bool q = bit(raw, 30);
    const uint32_t imm4 = bits(raw, 14, 11);
    if (bits(raw, 23, 22) != 0 || (!q && (imm4 & 8))) return invalid();
    if (!emit) return ok("ext");
    return ok("ext", vreg(bits(raw, 4, 0)) + " = " + kNs + "ext(" + vreg(bits(raw, 9, 5)) + ", " +
                         vreg(bits(raw, 20, 16)) + ", " + num(imm4) + ", " + qstr(q) + ");");
}

Lifted lift_table(uint32_t raw, bool emit) {
    const bool q = bit(raw, 30);
    if (bits(raw, 23, 22) != 0) return invalid();
    const bool tbx = bit(raw, 12);
    const char* name = tbx ? "tbx" : "tbl";
    if (!emit) return ok(name);
    const unsigned regs = bits(raw, 14, 13) + 1;
    const unsigned rn = bits(raw, 9, 5), rm = bits(raw, 20, 16), rd = bits(raw, 4, 0);
    std::string s = "const lgp::V128 table[" + num(regs) + "] = {";
    for (unsigned i = 0; i < regs; ++i)
        s += (i ? ", " : "") + vreg((rn + i) % 32);
    s += "}; " + vreg(rd) + " = " + kNs + name + "(table, " + num(regs) + ", " + vreg(rm) + ", ";
    if (tbx) s += vreg(rd) + ", ";
    return ok(name, s + qstr(q) + ");");
}

// AdvSIMDExpandImm(): the 64-bit pattern of a modified immediate.
uint64_t expand_imm(bool op, uint32_t cmode, uint32_t imm8) {
    const uint64_t i8 = imm8;
    auto rep32 = [](uint64_t v) { return v | (v << 32); };
    auto rep16 = [](uint64_t v) { return v | (v << 16) | (v << 32) | (v << 48); };
    switch (cmode >> 1) {
    case 0: return rep32(i8);
    case 1: return rep32(i8 << 8);
    case 2: return rep32(i8 << 16);
    case 3: return rep32(i8 << 24);
    case 4: return rep16(i8);
    case 5: return rep16(i8 << 8);
    case 6: return (cmode & 1) ? rep32((i8 << 16) | 0xFFFF) : rep32((i8 << 8) | 0xFF);
    default: break;
    }
    if ((cmode & 1) == 0) {
        if (!op) return i8 * 0x0101010101010101ull;
        uint64_t r = 0;
        for (unsigned b = 0; b < 8; ++b)
            if (imm8 & (1u << b)) r |= 0xFFull << (8 * b);
        return r;
    }
    const uint64_t a = (imm8 >> 7) & 1, b = (imm8 >> 6) & 1, cdefgh = imm8 & 0x3F;
    if (!op) {  // single precision a:NOT(b):Replicate(b,5):cdefgh:Zeros(19)
        const uint64_t f = (a << 31) | ((b ^ 1) << 30) | ((b ? 0x1Full : 0) << 25) | (cdefgh << 19);
        return rep32(f);
    }
    // double precision a:NOT(b):Replicate(b,8):cdefgh:Zeros(48)
    return (a << 63) | ((b ^ 1) << 62) | ((b ? 0xFFull : 0) << 54) | (cdefgh << 48);
}

Lifted lift_mod_imm(uint32_t raw, bool emit) {
    const bool q = bit(raw, 30);
    const bool op = bit(raw, 29);
    const uint32_t cmode = bits(raw, 15, 12);
    if (bit(raw, 11)) return invalid();  // o2 = 1: FMOV (half precision) needs FEAT_FP16
    const uint32_t imm8 = (bits(raw, 18, 16) << 5) | bits(raw, 9, 5);
    const unsigned rd = bits(raw, 4, 0);

    enum { Movi, Mvni, Orr, Bic, Fmov } kind;
    if (cmode < 8 || (cmode & 0xC) == 0x8) {  // 32-bit shifted or 16-bit shifted
        if (cmode & 1)
            kind = op ? Bic : Orr;
        else
            kind = op ? Mvni : Movi;
    } else if ((cmode & 0xE) == 0xC) {  // 32-bit shifting ones (MSL)
        kind = op ? Mvni : Movi;
    } else if (cmode == 0xE) {  // 8-bit, or 64-bit byte mask (op = 1)
        kind = Movi;
    } else {  // cmode = 1111
        if (op && !q) return invalid();
        kind = Fmov;
    }
    static const char* const kNames[] = {"movi", "mvni", "orr", "bic", "fmov"};
    if (!emit) return ok(kNames[kind]);

    uint64_t imm = expand_imm(op, cmode, imm8);
    if (kind == Mvni) imm = ~imm;
    const uint64_t hi = q ? imm : 0;
    const std::string v = vreg(rd);
    switch (kind) {
    case Orr:
        return ok("orr", v + ".lo |= " + hex64(imm) + "; " + (q ? v + ".hi |= " + hex64(imm) : v + ".hi = 0") + ";");
    case Bic:
        return ok("bic", v + ".lo &= " + hex64(~imm) + "; " + (q ? v + ".hi &= " + hex64(~imm) : v + ".hi = 0") + ";");
    default: return ok(kNames[kind], v + " = lgp::V128{" + hex64(imm) + ", " + hex64(hi) + "};");
    }
}

}  // namespace

Lifted lift_vec_perm(Class cls, uint32_t raw, bool emit) {
    switch (cls) {
    case Class::Copy: return lift_copy(raw, emit);
    case Class::ScalarCopy: return lift_scalar_copy(raw, emit);
    case Class::Permute: return lift_permute(raw, emit);
    case Class::Extract: return lift_extract(raw, emit);
    case Class::Table: return lift_table(raw, emit);
    case Class::ModImm: return lift_mod_imm(raw, emit);
    default: return invalid();
    }
}

}  // namespace lgp::recomp::a64::simd::detail
