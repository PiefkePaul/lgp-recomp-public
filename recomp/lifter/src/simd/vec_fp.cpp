// SIMD&FP lifter module: Advanced SIMD floating point (P2.4).
//
// The FP opcodes (see is_fp_op) of the classes three same, two-register misc,
// across lanes, pairwise, shift by immediate and by element, vector and
// scalar (Arm ARM C4.1.6, ARMv8.0). Per-lane semantics live in
// lgp/lifted/simd_fp.hpp on top of lgp/lifted/fp.hpp; this module decodes and
// emits calls. Unallocated in ARMv8.0 (and on the Cortex-A57): every FP16
// arithmetic form (ARMv8.2), FMLAL/FMLSL, FCMLA/FCADD, FRINT32/64, BFloat16,
// vector double precision with Q = 0, and across-lanes forms other than 4S.
#include "internal.hpp"

namespace lgp::recomp::a64::simd::detail {
namespace {

const std::string kNs = "lgp::lifted::simd::";
const std::string kFpcr = "lgp_fpcr";
const char* kFpcrRounding = "lgp::lifted::fp::fpcr_rounding(lgp_fpcr)";

const char* utype(bool dbl) { return dbl ? "uint64_t" : "uint32_t"; }
const char* qstr(bool q) { return q ? "true" : "false"; }
const char* bstr(bool b) { return b ? "true" : "false"; }

const char* rounding_name(unsigned r) {
    switch (r) {
    case 0: return "lgp::lifted::fp::kTieEven";
    case 1: return "lgp::lifted::fp::kPosInf";
    case 2: return "lgp::lifted::fp::kNegInf";
    case 3: return "lgp::lifted::fp::kZero";
    case 4: return "lgp::lifted::fp::kTieAway";
    default: return "lgp::lifted::fp::kOdd";
    }
}

// Operation names of simd_fp.hpp.
enum Bin : uint8_t { Add, Sub, Mul, Mulx, Div, Max, Min, MaxNum, MinNum, Abd, RecpS, RsqrtS, CmEq, CmGe, CmGt, AcGe, AcGt };
const char* const kBinNames[] = {"Add",  "Sub",   "Mul",    "Mulx",   "Div",  "Max",  "Min",  "MaxNum", "MinNum",
                                 "Abd",  "RecpS", "RsqrtS", "CmEq",   "CmGe", "CmGt", "AcGe", "AcGt"};
std::string bin(Bin b) { return kNs + "FpBin::" + kBinNames[b]; }

enum Un : uint8_t { Abs, Neg, Sqrt, RecpE, RsqrtE, RecpX, URecpE, URsqrtE, CmGtZ, CmGeZ, CmEqZ, CmLeZ, CmLtZ };
const char* const kUnNames[] = {"Abs",    "Neg",     "Sqrt",  "RecpE", "RsqrtE", "RecpX", "URecpE",
                                "URsqrtE", "CmGtZ",  "CmGeZ", "CmEqZ", "CmLeZ",  "CmLtZ"};
std::string un(Un u) { return kNs + "FpUn::" + kUnNames[u]; }

std::string vset(unsigned rd, const std::string& expr) { return vreg(rd) + " = " + expr + ";"; }

// ---- three same (vector and scalar) -------------------------------------------
enum class K3 : uint8_t { None, Bin, Pair, Fma };
struct Op3 {
    const char* name;
    K3 kind;
    Bin op;
    bool sub;  // FMLS
};

Lifted three_same(uint32_t r, bool emit, bool scalar) {
    const bool u = bit(r, 29), a = bit(r, 23), sz = bit(r, 22), q = bit(r, 30);
    const unsigned opc = bits(r, 15, 11) - 0b11000;  // 0..7
    // Index: U:a:opcode<2:0>.
    static const Op3 kVec[32] = {
        // U = 0, a = 0
        {"fmaxnm", K3::Bin, MaxNum, false},
        {"fmla", K3::Fma, Add, false},
        {"fadd", K3::Bin, Add, false},
        {"fmulx", K3::Bin, Mulx, false},
        {"fcmeq", K3::Bin, CmEq, false},
        {nullptr, K3::None, Add, false},  // FMLAL (ARMv8.2)
        {"fmax", K3::Bin, Max, false},
        {"frecps", K3::Bin, RecpS, false},
        // U = 0, a = 1
        {"fminnm", K3::Bin, MinNum, false},
        {"fmls", K3::Fma, Add, true},
        {"fsub", K3::Bin, Sub, false},
        {nullptr, K3::None, Add, false},
        {nullptr, K3::None, Add, false},
        {nullptr, K3::None, Add, false},  // FMLSL (ARMv8.2)
        {"fmin", K3::Bin, Min, false},
        {"frsqrts", K3::Bin, RsqrtS, false},
        // U = 1, a = 0
        {"fmaxnmp", K3::Pair, MaxNum, false},
        {nullptr, K3::None, Add, false},  // FMLAL2 (ARMv8.2)
        {"faddp", K3::Pair, Add, false},
        {"fmul", K3::Bin, Mul, false},
        {"fcmge", K3::Bin, CmGe, false},
        {"facge", K3::Bin, AcGe, false},
        {"fmaxp", K3::Pair, Max, false},
        {"fdiv", K3::Bin, Div, false},
        // U = 1, a = 1
        {"fminnmp", K3::Pair, MinNum, false},
        {nullptr, K3::None, Add, false},  // FMLSL2 (ARMv8.2)
        {"fabd", K3::Bin, Abd, false},
        {nullptr, K3::None, Add, false},
        {"fcmgt", K3::Bin, CmGt, false},
        {"facgt", K3::Bin, AcGt, false},
        {"fminp", K3::Pair, Min, false},
        {nullptr, K3::None, Add, false},
    };
    const Op3& op = kVec[(u << 4) | (a << 3) | opc];
    if (op.kind == K3::None) return invalid();
    const unsigned rd = bits(r, 4, 0), rn = bits(r, 9, 5), rm = bits(r, 20, 16);
    const std::string T = utype(sz);
    if (scalar) {
        // Scalar subset: FMULX, FCMEQ, FRECPS, FRSQRTS, FCMGE, FACGE, FABD, FCMGT, FACGT.
        if (op.kind != K3::Bin) return invalid();
        switch (op.op) {
        case Mulx:
        case CmEq:
        case RecpS:
        case RsqrtS:
        case CmGe:
        case AcGe:
        case Abd:
        case CmGt:
        case AcGt: break;
        default: return invalid();
        }
        if (!emit) return ok(op.name);
        const unsigned bytes = sz ? 8 : 4;
        return ok(op.name, set_vscalar(rd, kNs + "fp2<" + bin(op.op) + ", " + T + ">(" + kFpcr + ", " +
                                               vscalar(rn, bytes) + ", " + vscalar(rm, bytes) + ")"));
    }
    if (sz && !q) return invalid();  // 1D
    if (!emit) return ok(op.name);
    const std::string n = vreg(rn), m = vreg(rm);
    switch (op.kind) {
    case K3::Bin:
        return ok(op.name, vset(rd, kNs + "fp2v<" + bin(op.op) + ", " + T + ">(" + kFpcr + ", " + n + ", " + m + ", " +
                                        qstr(q) + ")"));
    case K3::Pair:
        return ok(op.name, vset(rd, kNs + "fp2v_pair<" + bin(op.op) + ", " + T + ">(" + kFpcr + ", " + n + ", " + m +
                                        ", " + qstr(q) + ")"));
    default:
        return ok(op.name, vset(rd, kNs + "fmav<" + bstr(op.sub) + ", " + T + ">(" + kFpcr + ", " + vreg(rd) + ", " +
                                        n + ", " + m + ", " + qstr(q) + ")"));
    }
}

// ---- two-register misc (vector and scalar) -------------------------------------
Lifted unary(const char* name, Un op, unsigned rd, unsigned rn, bool sz, bool q, bool scalar, bool emit) {
    if (!emit) return ok(name);
    const std::string T = utype(sz);
    if (scalar)
        return ok(name, set_vscalar(rd, kNs + "fp1<" + un(op) + ", " + T + ">(" + kFpcr + ", " +
                                            vscalar(rn, sz ? 8 : 4) + ")"));
    return ok(name, vset(rd, kNs + "fp1v<" + un(op) + ", " + T + ">(" + kFpcr + ", " + vreg(rn) + ", " + qstr(q) + ")"));
}

Lifted to_int(const char* name, const char* mode, bool is_unsigned, unsigned rd, unsigned rn, bool sz, bool q,
              bool scalar, bool emit, unsigned fbits = 0) {
    if (!emit) return ok(name);
    const std::string T = utype(sz);
    if (scalar)
        return ok(name, set_vscalar(rd, kNs + "to_fixed<" + T + ">(" + kFpcr + ", " + vscalar(rn, sz ? 8 : 4) + ", " +
                                            num(fbits) + ", " + bstr(is_unsigned) + ", " + mode + ")"));
    return ok(name, vset(rd, kNs + "to_fixed_v<" + T + ">(" + kFpcr + ", " + vreg(rn) + ", " + qstr(q) + ", " +
                                 num(fbits) + ", " + bstr(is_unsigned) + ", " + mode + ")"));
}

Lifted from_int(const char* name, bool is_unsigned, unsigned rd, unsigned rn, bool sz, bool q, bool scalar, bool emit,
                unsigned fbits = 0) {
    if (!emit) return ok(name);
    const std::string T = utype(sz);
    if (scalar)
        return ok(name, set_vscalar(rd, kNs + "from_fixed<" + T + ">(" + kFpcr + ", " + vscalar(rn, sz ? 8 : 4) + ", " +
                                            num(fbits) + ", " + bstr(is_unsigned) + ")"));
    return ok(name, vset(rd, kNs + "from_fixed_v<" + T + ">(" + kFpcr + ", " + vreg(rn) + ", " + qstr(q) + ", " +
                                 num(fbits) + ", " + bstr(is_unsigned) + ")"));
}

Lifted two_misc(uint32_t r, bool emit, bool scalar) {
    const bool u = bit(r, 29), a = bit(r, 23), sz = bit(r, 22), q = scalar ? true : bit(r, 30);
    const uint32_t opcode = bits(r, 16, 12);
    const unsigned rd = bits(r, 4, 0), rn = bits(r, 9, 5);

    // Narrowing/widening conversions: sz selects the formats, Q the half.
    if (opcode == 0x16 || opcode == 0x17) {
        if (a) return invalid();  // BFCVTN (ARMv8.6) and reserved
        if (opcode == 0x16 && !u) {  // FCVTN{2}
            if (scalar) return invalid();
            const char* name = q ? "fcvtn2" : "fcvtn";
            if (!emit) return ok(name);
            const std::string types = sz ? "uint32_t, uint64_t" : "uint16_t, uint32_t";
            return ok(name, vset(rd, kNs + "fcvtn<" + types + ">(" + kFpcr + ", " + vreg(rd) + ", " + vreg(rn) + ", " +
                                         qstr(q) + ", " + kFpcrRounding + ")"));
        }
        if (opcode == 0x16 && u) {  // FCVTXN{2}: double -> single, round to odd
            if (!sz) return invalid();
            if (scalar) {
                if (!emit) return ok("fcvtxn");
                return ok("fcvtxn", set_vscalar(rd, "lgp::lifted::fp::convert<uint32_t, uint64_t>(" + kFpcr + ", " +
                                                        vscalar(rn, 8) + ", lgp::lifted::fp::kOdd)"));
            }
            const char* name = q ? "fcvtxn2" : "fcvtxn";
            if (!emit) return ok(name);
            return ok(name, vset(rd, kNs + "fcvtn<uint32_t, uint64_t>(" + kFpcr + ", " + vreg(rd) + ", " + vreg(rn) +
                                         ", " + qstr(q) + ", lgp::lifted::fp::kOdd)"));
        }
        if (u || scalar) return invalid();
        const char* name = q ? "fcvtl2" : "fcvtl";  // FCVTL{2}
        if (!emit) return ok(name);
        const std::string types = sz ? "uint64_t, uint32_t" : "uint32_t, uint16_t";
        return ok(name, vset(rd, kNs + "fcvtl<" + types + ">(" + kFpcr + ", " + vreg(rn) + ", " + qstr(q) + ")"));
    }

    if (!scalar && sz && !q) return invalid();  // 1D

    switch (opcode) {
    case 0x0C:  // FCMGT/FCMGE #0
    case 0x0D:  // FCMEQ/FCMLE #0
    case 0x0E:  // FCMLT #0
    case 0x0F:  // FABS/FNEG (vector only)
    {
        if (!a) return invalid();  // size 0x: integer/FP16 space, unallocated here
        if (opcode == 0x0C) return u ? unary("fcmge", CmGeZ, rd, rn, sz, q, scalar, emit)
                                     : unary("fcmgt", CmGtZ, rd, rn, sz, q, scalar, emit);
        if (opcode == 0x0D) return u ? unary("fcmle", CmLeZ, rd, rn, sz, q, scalar, emit)
                                     : unary("fcmeq", CmEqZ, rd, rn, sz, q, scalar, emit);
        if (opcode == 0x0E) return u ? invalid() : unary("fcmlt", CmLtZ, rd, rn, sz, q, scalar, emit);
        if (scalar) return invalid();
        return u ? unary("fneg", Neg, rd, rn, sz, q, false, emit) : unary("fabs", Abs, rd, rn, sz, q, false, emit);
    }
    case 0x18:
    case 0x19: {  // FRINT*: vector only
        if (scalar) return invalid();
        // o2 = a, o1 = opcode<0>; U selects FRINTA/FRINTX/FRINTI.
        const char* name = nullptr;
        std::string mode;
        const unsigned o1 = opcode & 1;
        if (!u) {
            static const char* kN[] = {"frintn", "frintm", "frintp", "frintz"};
            static const unsigned kR[] = {0, 2, 1, 3};
            name = kN[(a << 1) | o1];
            mode = rounding_name(kR[(a << 1) | o1]);
        } else if (!a && !o1) {
            name = "frinta";
            mode = rounding_name(4);
        } else if (o1) {
            name = a ? "frinti" : "frintx";
            mode = kFpcrRounding;
        } else {
            return invalid();
        }
        if (!emit) return ok(name);
        return ok(name, vset(rd, kNs + "frintv<" + utype(sz) + ">(" + kFpcr + ", " + vreg(rn) + ", " + qstr(q) + ", " +
                                     mode + ")"));
    }
    case 0x1A:
    case 0x1B: {  // FCVT{N,P,M,Z}{S,U}
        static const char* kS[] = {"fcvtns", "fcvtps", "fcvtms", "fcvtzs"};
        static const char* kU[] = {"fcvtnu", "fcvtpu", "fcvtmu", "fcvtzu"};
        static const unsigned kR[] = {0, 1, 2, 3};
        const unsigned k = ((opcode & 1) << 1) | a;
        return to_int(u ? kU[k] : kS[k], rounding_name(kR[k]), u, rd, rn, sz, q, scalar, emit);
    }
    case 0x1C:
        if (!a) return to_int(u ? "fcvtau" : "fcvtas", rounding_name(4), u, rd, rn, sz, q, scalar, emit);
        if (scalar || sz) return invalid();  // URECPE/URSQRTE: vector, 32-bit only
        return u ? unary("ursqrte", URsqrtE, rd, rn, false, q, false, emit)
                 : unary("urecpe", URecpE, rd, rn, false, q, false, emit);
    case 0x1D:
        if (!a) return from_int(u ? "ucvtf" : "scvtf", u, rd, rn, sz, q, scalar, emit);
        return u ? unary("frsqrte", RsqrtE, rd, rn, sz, q, scalar, emit)
                 : unary("frecpe", RecpE, rd, rn, sz, q, scalar, emit);
    case 0x1F:
        if (!a) return invalid();  // FRINT64Z/FRINT64X (ARMv8.5)
        if (scalar) return u ? invalid() : unary("frecpx", RecpX, rd, rn, sz, true, true, emit);
        return u ? unary("fsqrt", Sqrt, rd, rn, sz, q, false, emit) : invalid();
    default: return invalid();  // FRINT32 (ARMv8.5) and reserved
    }
}

// ---- across lanes, scalar pairwise -------------------------------------------------
Lifted across_lanes(uint32_t r, bool emit) {
    const bool u = bit(r, 29), a = bit(r, 23), sz = bit(r, 22), q = bit(r, 30);
    const uint32_t opcode = bits(r, 16, 12);
    if (!u || sz || !q) return invalid();  // U = 0: FP16 forms (ARMv8.2); only 4S in ARMv8.0
    const char* name;
    Bin op;
    if (opcode == 0x0C) {
        name = a ? "fminnmv" : "fmaxnmv";
        op = a ? MinNum : MaxNum;
    } else if (opcode == 0x0F) {
        name = a ? "fminv" : "fmaxv";
        op = a ? Min : Max;
    } else {
        return invalid();
    }
    if (!emit) return ok(name);
    return ok(name, set_vscalar(bits(r, 4, 0), kNs + "fp2_reduce4<" + bin(op) + ">(" + kFpcr + ", " +
                                                    vreg(bits(r, 9, 5)) + ")"));
}

Lifted scalar_pairwise(uint32_t r, bool emit) {
    const bool u = bit(r, 29), a = bit(r, 23), sz = bit(r, 22);
    const uint32_t opcode = bits(r, 16, 12);
    if (!u) return invalid();  // U = 0: FP16 forms (ARMv8.2)
    const char* name;
    Bin op;
    switch (opcode) {
    case 0x0C:
        name = a ? "fminnmp" : "fmaxnmp";
        op = a ? MinNum : MaxNum;
        break;
    case 0x0D:
        if (a) return invalid();
        name = "faddp";
        op = Add;
        break;
    case 0x0F:
        name = a ? "fminp" : "fmaxp";
        op = a ? Min : Max;
        break;
    default: return invalid();
    }
    if (!emit) return ok(name);
    return ok(name, set_vscalar(bits(r, 4, 0), kNs + "fp2_pair<" + bin(op) + ", " + utype(sz) + ">(" + kFpcr + ", " +
                                                    vreg(bits(r, 9, 5)) + ")"));
}

// ---- shift by immediate: fixed-point conversions --------------------------------------
Lifted shift_imm(uint32_t r, bool emit, bool scalar) {
    const bool u = bit(r, 29), q = scalar ? true : bit(r, 30);
    const uint32_t immh = bits(r, 22, 19), immhb = bits(r, 22, 16);
    const uint32_t opcode = bits(r, 15, 11);
    bool sz;
    if (immh & 0b1000)
        sz = true;
    else if (immh & 0b0100)
        sz = false;
    else
        return invalid();  // half precision (ARMv8.2) or reserved
    if (!scalar && sz && !q) return invalid();
    const unsigned fbits = (sz ? 128 : 64) - immhb;
    const unsigned rd = bits(r, 4, 0), rn = bits(r, 9, 5);
    if (opcode == 0x1C) return from_int(u ? "ucvtf" : "scvtf", u, rd, rn, sz, q, scalar, emit, fbits);
    if (opcode == 0x1F)
        return to_int(u ? "fcvtzu" : "fcvtzs", rounding_name(3), u, rd, rn, sz, q, scalar, emit, fbits);
    return invalid();
}

// ---- by element ------------------------------------------------------------------------
Lifted indexed(uint32_t r, bool emit, bool scalar) {
    const bool u = bit(r, 29), q = scalar ? true : bit(r, 30);
    const uint32_t size = bits(r, 23, 22);
    const uint32_t opcode = bits(r, 15, 12);
    if (!(size & 2)) return invalid();  // size 00: FP16 (ARMv8.2); 01: reserved
    const bool sz = size & 1;
    const unsigned h = bit(r, 11), l = bit(r, 21);
    unsigned index;
    if (sz) {
        if (l) return invalid();
        if (!scalar && !q) return invalid();
        index = h;
    } else {
        index = (h << 1) | l;
    }
    const unsigned rm = bits(r, 20, 16), rn = bits(r, 9, 5), rd = bits(r, 4, 0);
    const char* name;
    int kind;  // 0: FMLA, 1: FMLS, 2: FMUL, 3: FMULX
    if (opcode == 0x1 && !u) {
        name = "fmla";
        kind = 0;
    } else if (opcode == 0x5 && !u) {
        name = "fmls";
        kind = 1;
    } else if (opcode == 0x9) {
        name = u ? "fmulx" : "fmul";
        kind = u ? 3 : 2;
    } else {
        return invalid();  // FCMLA (ARMv8.3) and reserved
    }
    if (!emit) return ok(name);
    const std::string T = utype(sz);
    const std::string elem = kNs + "lane<" + T + ">(" + vreg(rm) + ", " + num(index) + ")";
    const unsigned bytes = sz ? 8 : 4;
    if (scalar) {
        if (kind <= 1)
            return ok(name, set_vscalar(rd, kNs + "fma<" + bstr(kind == 1) + ", " + T + ">(" + kFpcr + ", " +
                                                vscalar(rd, bytes) + ", " + vscalar(rn, bytes) + ", " + elem + ")"));
        return ok(name, set_vscalar(rd, kNs + "fp2<" + bin(kind == 3 ? Mulx : Mul) + ", " + T + ">(" + kFpcr + ", " +
                                            vscalar(rn, bytes) + ", " + elem + ")"));
    }
    if (kind <= 1)
        return ok(name, vset(rd, kNs + "fmav_elem<" + bstr(kind == 1) + ", " + T + ">(" + kFpcr + ", " + vreg(rd) +
                                     ", " + vreg(rn) + ", " + elem + ", " + qstr(q) + ")"));
    return ok(name, vset(rd, kNs + "fp2v_elem<" + bin(kind == 3 ? Mulx : Mul) + ", " + T + ">(" + kFpcr + ", " +
                                 vreg(rn) + ", " + elem + ", " + qstr(q) + ")"));
}

}  // namespace

Lifted lift_vec_fp(Class cls, uint32_t raw, bool emit) {
    switch (cls) {
    case Class::ThreeSame: return three_same(raw, emit, false);
    case Class::ScalarThreeSame: return three_same(raw, emit, true);
    case Class::TwoMisc: return two_misc(raw, emit, false);
    case Class::ScalarTwoMisc: return two_misc(raw, emit, true);
    case Class::AcrossLanes: return across_lanes(raw, emit);
    case Class::ScalarPairwise: return scalar_pairwise(raw, emit);
    case Class::ShiftImm: return shift_imm(raw, emit, false);
    case Class::ScalarShiftImm: return shift_imm(raw, emit, true);
    case Class::Indexed: return indexed(raw, emit, false);
    case Class::ScalarIndexed: return indexed(raw, emit, true);
    default: return invalid();
    }
}

}  // namespace lgp::recomp::a64::simd::detail
