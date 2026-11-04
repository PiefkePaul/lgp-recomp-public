// SIMD&FP lifter module: vec_int (P2.4) -- Advanced SIMD integer instructions.
//
// Covers the integer opcodes of the vector and scalar classes three same,
// three different, two-register misc, across lanes, pairwise (ADDP), shift by
// immediate and by element (Arm ARM DDI 0487, C4.1.6x encoding tables), as
// implemented by ARMv8.0-A plus the Crypto extension's PMULL (1Q). Later
// extensions (v8.1 RDM SQRDMLAH/SQRDMLSH, v8.2 SDOT/UDOT) decode as
// unallocated. FP opcodes of the same classes are routed to vec_fp by
// is_fp_op() and never reach this file.
//
// The emitted code calls the lane maps and functors of lgp/lifted/simd_int.hpp.
#include <initializer_list>

#include "internal.hpp"

namespace lgp::recomp::a64::simd::detail {

namespace {

const char* const kSigned[4] = {"int8_t", "int16_t", "int32_t", "int64_t"};
const char* const kUnsigned[4] = {"uint8_t", "uint16_t", "uint32_t", "uint64_t"};
const char* const kNs = "lgp::lifted::simd::";

std::string ty(unsigned size, bool sgn) { return sgn ? kSigned[size] : kUnsigned[size]; }
std::string op(const char* name) { return std::string(kNs) + name; }

std::string call(const char* fn, std::initializer_list<std::string> targs, std::initializer_list<std::string> args) {
    std::string s = std::string(kNs) + fn;
    if (targs.size()) {
        s += "<";
        bool first = true;
        for (const auto& t : targs) {
            if (!first) s += ", ";
            s += t;
            first = false;
        }
        s += ">";
    }
    s += "(";
    bool first = true;
    for (const auto& a : args) {
        if (!first) s += ", ";
        s += a;
        first = false;
    }
    return s + ")";
}

std::string set(unsigned d, const std::string& expr) { return vreg(d) + " = " + expr + ";"; }
std::string V(unsigned r) { return vreg(r); }
std::string b(bool v) { return v ? "true" : "false"; }

// Mnemonic pair by U bit.
const char* pick(bool u, const char* u0, const char* u1) { return u ? u1 : u0; }

// Element size of a shift-by-immediate encoding from immh (non-zero).
unsigned immh_size(uint32_t immh) {
    if (immh & 8) return 3;
    if (immh & 4) return 2;
    if (immh & 2) return 1;
    return 0;
}

struct Fields {
    unsigned rd, rn, rm;
    bool q, u;
    unsigned size;
    explicit Fields(uint32_t r)
        : rd(bits(r, 4, 0)), rn(bits(r, 9, 5)), rm(bits(r, 20, 16)), q(bit(r, 30) != 0), u(bit(r, 29) != 0),
          size(bits(r, 23, 22)) {}
};

// ---- three same -------------------------------------------------------------------------------

Lifted three_same(uint32_t raw, bool scalar, bool emit) {
    const Fields f(raw);
    const uint32_t opc = bits(raw, 15, 11);
    const unsigned n = scalar ? 1u : ((f.q ? 16u : 8u) >> f.size);
    const bool q64bad = !scalar && f.size == 3 && !f.q;  // 64-bit lanes need Q = 1
    auto v2 = [&](const char* name, const std::string& t, const std::string& fn) {
        if (!emit) return ok(name);
        return ok(name, set(f.rd, call("v2", {t, fn}, {"ctx", V(f.rn), V(f.rm), num(n)})));
    };
    auto v3 = [&](const char* name, const std::string& t, const std::string& fn) {
        if (!emit) return ok(name);
        return ok(name, set(f.rd, call("v3", {t, fn}, {"ctx", V(f.rd), V(f.rn), V(f.rm), num(n)})));
    };
    auto pw = [&](const char* name, const std::string& t, const std::string& fn) {
        if (!emit) return ok(name);
        return ok(name, set(f.rd, call("pw", {t, fn}, {"ctx", V(f.rn), V(f.rm), num(n)})));
    };
    const std::string t = ty(f.size, !f.u);  // signedness from U for the S*/U* pairs

    if (scalar) {
        // Scalar: SQADD/UQADD, SQSUB/UQSUB, SQSHL/UQSHL, SQRSHL/UQRSHL (all sizes); the
        // others only with 64-bit elements; SQDMULH/SQRDMULH with 16/32-bit elements.
        switch (opc) {
        case 0x01:
        case 0x05:
        case 0x09:
        case 0x0B: break;
        case 0x06:
        case 0x07:
        case 0x08:
        case 0x0A:
        case 0x10:
        case 0x11:
            if (f.size != 3) return invalid();
            break;
        case 0x16:
            if (f.size != 1 && f.size != 2) return invalid();
            break;
        default: return invalid();
        }
    }

    switch (opc) {
    case 0x00:
        if (f.size == 3) return invalid();
        return v2(pick(f.u, "shadd", "uhadd"), t, op("HAdd"));
    case 0x01:
        if (q64bad) return invalid();
        return v2(pick(f.u, "sqadd", "uqadd"), t, op("QAdd"));
    case 0x02:
        if (f.size == 3) return invalid();
        return v2(pick(f.u, "srhadd", "urhadd"), t, op("RHAdd"));
    case 0x03: {
        static const char* const kNames[2][4] = {{"and", "bic", "orr", "orn"}, {"eor", "bsl", "bit", "bif"}};
        const char* name = kNames[f.u][f.size];
        if (!emit) return ok(name);
        std::string e;
        auto half = [&](const char* h) {
            const std::string N = std::string("n_.") + h, M = std::string("m_.") + h, D = std::string("d_.") + h;
            switch ((f.u ? 4 : 0) | f.size) {
            case 0: return N + " & " + M;
            case 1: return N + " & ~" + M;
            case 2: return N + " | " + M;
            case 3: return N + " | ~" + M;
            case 4: return N + " ^ " + M;
            case 5: return M + " ^ ((" + M + " ^ " + N + ") & " + D + ")";  // BSL
            case 6: return D + " ^ ((" + D + " ^ " + N + ") & " + M + ")";  // BIT
            default: return D + " ^ ((" + D + " ^ " + N + ") & ~" + M + ")";  // BIF
            }
        };
        e = "const lgp::V128 n_ = " + V(f.rn) + ", m_ = " + V(f.rm) + ", d_ = " + V(f.rd) + "; (void)d_; " +
            set(f.rd, op("narrow_q") + "(lgp::V128{" + half("lo") + ", " + half("hi") + "}, " + b(f.q) + ")");
        return ok(name, e);
    }
    case 0x04:
        if (f.size == 3) return invalid();
        return v2(pick(f.u, "shsub", "uhsub"), t, op("HSub"));
    case 0x05:
        if (q64bad) return invalid();
        return v2(pick(f.u, "sqsub", "uqsub"), t, op("QSub"));
    case 0x06:
        if (q64bad) return invalid();
        return v2(pick(f.u, "cmgt", "cmhi"), t, op("CmGt"));
    case 0x07:
        if (q64bad) return invalid();
        return v2(pick(f.u, "cmge", "cmhs"), t, op("CmGe"));
    case 0x08:
        if (q64bad) return invalid();
        return v2(pick(f.u, "sshl", "ushl"), t, op("ShlReg<false, false>"));
    case 0x09:
        if (q64bad) return invalid();
        return v2(pick(f.u, "sqshl", "uqshl"), t, op("ShlReg<false, true>"));
    case 0x0A:
        if (q64bad) return invalid();
        return v2(pick(f.u, "srshl", "urshl"), t, op("ShlReg<true, false>"));
    case 0x0B:
        if (q64bad) return invalid();
        return v2(pick(f.u, "sqrshl", "uqrshl"), t, op("ShlReg<true, true>"));
    case 0x0C:
        if (f.size == 3) return invalid();
        return v2(pick(f.u, "smax", "umax"), t, op("Max"));
    case 0x0D:
        if (f.size == 3) return invalid();
        return v2(pick(f.u, "smin", "umin"), t, op("Min"));
    case 0x0E:
        if (f.size == 3) return invalid();
        return v2(pick(f.u, "sabd", "uabd"), t, op("Abd"));
    case 0x0F:
        if (f.size == 3) return invalid();
        return v3(pick(f.u, "saba", "uaba"), t, op("Aba"));
    case 0x10:
        if (q64bad) return invalid();
        return v2(pick(f.u, "add", "sub"), ty(f.size, false), op(f.u ? "Sub" : "Add"));
    case 0x11:
        if (q64bad) return invalid();
        return v2(pick(f.u, "cmtst", "cmeq"), ty(f.size, false), op(f.u ? "CmEq" : "CmTst"));
    case 0x12:
        if (f.size == 3) return invalid();
        return v3(pick(f.u, "mla", "mls"), ty(f.size, false), op(f.u ? "Mls" : "Mla"));
    case 0x13:
        if (f.u) {
            if (f.size != 0) return invalid();
            return v2("pmul", "uint8_t", op("Pmul"));
        }
        if (f.size == 3) return invalid();
        return v2("mul", ty(f.size, false), op("Mul"));
    case 0x14:
        if (f.size == 3) return invalid();
        return pw(pick(f.u, "smaxp", "umaxp"), t, op("Max"));
    case 0x15:
        if (f.size == 3) return invalid();
        return pw(pick(f.u, "sminp", "uminp"), t, op("Min"));
    case 0x16:
        if (f.size != 1 && f.size != 2) return invalid();
        return v2(pick(f.u, "sqdmulh", "sqrdmulh"), ty(f.size, true), op(f.u ? "QDMulH<true>" : "QDMulH<false>"));
    case 0x17:
        if (f.u || q64bad) return invalid();
        return pw("addp", ty(f.size, false), op("Add"));
    default: return invalid();
    }
}

// ---- three different ------------------------------------------------------------------------

Lifted three_diff(uint32_t raw, bool scalar, bool emit) {
    const Fields f(raw);
    const uint32_t opc = bits(raw, 15, 12);
    const bool part = !scalar && f.q;
    const unsigned nn = scalar ? 1u : (8u >> f.size);  // wide result lanes
    const unsigned first = part ? nn : 0u;

    if (scalar) {
        if (f.u || (f.size != 1 && f.size != 2)) return invalid();
        if (opc != 0x9 && opc != 0xB && opc != 0xD) return invalid();
    }
    if (opc == 0xE) {  // PMULL/PMULL2
        if (f.u || f.size == 1 || f.size == 2) return invalid();
        if (!emit) return ok("pmull");
        return ok("pmull", set(f.rd, call(f.size == 0 ? "pmull8" : "pmull64", {}, {V(f.rn), V(f.rm), b(part)})));
    }
    if (f.size == 3 || opc == 0xF) return invalid();

    const bool sgn = !f.u;
    const std::string tn = ty(f.size, sgn), tw = ty(f.size + 1, sgn);
    auto l2 = [&](const char* name, const char* fn) {
        if (!emit) return ok(name);
        return ok(name, set(f.rd, call("l2", {tn, tw, op(fn)}, {"ctx", V(f.rn), V(f.rm), num(first), num(nn)})));
    };
    auto l3 = [&](const char* name, const char* fn) {
        if (!emit) return ok(name);
        return ok(name,
                  set(f.rd, call("l3", {tn, tw, op(fn)}, {"ctx", V(f.rd), V(f.rn), V(f.rm), num(first), num(nn)})));
    };
    auto w2 = [&](const char* name, const char* fn) {
        if (!emit) return ok(name);
        return ok(name, set(f.rd, call("w2", {tn, tw, op(fn)}, {"ctx", V(f.rn), V(f.rm), num(first), num(nn)})));
    };
    auto n2 = [&](const char* name, const char* fn) {
        if (!emit) return ok(name);
        return ok(name, set(f.rd, call("n2", {ty(f.size + 1, false), ty(f.size, false), op(fn)},
                                        {"ctx", V(f.rd), V(f.rn), V(f.rm), num(nn), b(part)})));
    };

    switch (opc) {
    case 0x0: return l2(pick(f.u, "saddl", "uaddl"), "Add");
    case 0x1: return w2(pick(f.u, "saddw", "uaddw"), "Add");
    case 0x2: return l2(pick(f.u, "ssubl", "usubl"), "Sub");
    case 0x3: return w2(pick(f.u, "ssubw", "usubw"), "Sub");
    case 0x4: return n2(pick(f.u, "addhn", "raddhn"), f.u ? "AddHn<true>" : "AddHn<false>");
    case 0x5: return l3(pick(f.u, "sabal", "uabal"), "Aba");
    case 0x6: return n2(pick(f.u, "subhn", "rsubhn"), f.u ? "SubHn<true>" : "SubHn<false>");
    case 0x7: return l2(pick(f.u, "sabdl", "uabdl"), "Abd");
    case 0x8: return l3(pick(f.u, "smlal", "umlal"), "Mla");
    case 0x9:
        if (f.u || f.size == 0) return invalid();
        return l3("sqdmlal", "QDMlal");
    case 0xA: return l3(pick(f.u, "smlsl", "umlsl"), "Mls");
    case 0xB:
        if (f.u || f.size == 0) return invalid();
        return l3("sqdmlsl", "QDMlsl");
    case 0xC: return l2(pick(f.u, "smull", "umull"), "Mul");
    case 0xD:
        if (f.u || f.size == 0) return invalid();
        return l2("sqdmull", "QDMull");
    default: return invalid();
    }
}

// ---- two-register misc --------------------------------------------------------------------------

Lifted two_misc(uint32_t raw, bool scalar, bool emit) {
    const Fields f(raw);
    const uint32_t opc = bits(raw, 16, 12);
    const unsigned n = scalar ? 1u : ((f.q ? 16u : 8u) >> f.size);
    const bool q64bad = !scalar && f.size == 3 && !f.q;
    auto v1 = [&](const char* name, const std::string& t, const char* fn) {
        if (!emit) return ok(name);
        return ok(name, set(f.rd, call("v1", {t, op(fn)}, {"ctx", V(f.rn), num(n)})));
    };
    // Narrowing: element size `size` is the destination size.
    auto narrow = [&](const char* name, bool src_signed, bool dst_signed, const char* fn) {
        if (f.size == 3) return invalid();
        if (!emit) return ok(name);
        const unsigned nn = scalar ? 1u : (8u >> f.size);
        return ok(name, set(f.rd, call("n1", {ty(f.size + 1, src_signed), ty(f.size, dst_signed), op(fn)},
                                        {"ctx", V(f.rd), V(f.rn), num(nn), b(!scalar && f.q)})));
    };

    if (scalar) {
        switch (opc) {
        case 0x03:
        case 0x07:
        case 0x14: break;
        case 0x12:
            if (!f.u) return invalid();
            break;
        case 0x08:
        case 0x09:
        case 0x0B:
            if (f.size != 3) return invalid();
            break;
        case 0x0A:
            if (f.size != 3 || f.u) return invalid();
            break;
        default: return invalid();
        }
    }

    switch (opc) {
    case 0x00: {  // REV64 / REV32
        if (f.u ? f.size >= 2 : f.size == 3) return invalid();
        const char* name = f.u ? "rev32" : "rev64";
        if (!emit) return ok(name);
        const unsigned cmask = ((f.u ? 4u : 8u) >> f.size) - 1;
        return ok(name, set(f.rd, call("rev", {ty(f.size, false)}, {V(f.rn), num(n), num(cmask)})));
    }
    case 0x01:  // REV16
        if (f.u || f.size != 0) return invalid();
        if (!emit) return ok("rev16");
        return ok("rev16", set(f.rd, call("rev", {"uint8_t"}, {V(f.rn), num(n), "1"})));
    case 0x02:
    case 0x06: {  // [SU]ADDLP, [SU]ADALP
        if (f.size == 3) return invalid();
        const bool acc = opc == 0x06;
        const char* name = acc ? pick(f.u, "sadalp", "uadalp") : pick(f.u, "saddlp", "uaddlp");
        if (!emit) return ok(name);
        const unsigned nw = (f.q ? 16u : 8u) >> (f.size + 1);
        return ok(name, set(f.rd, call("paddl", {ty(f.size, !f.u), ty(f.size + 1, !f.u)},
                                        {V(f.rd), V(f.rn), num(nw), b(acc)})));
    }
    case 0x03: {  // SUQADD / USQADD (accumulate into Rd)
        if (q64bad) return invalid();
        const char* name = pick(f.u, "suqadd", "usqadd");
        if (!emit) return ok(name);
        return ok(name, set(f.rd, call("v2", {ty(f.size, !f.u), op(f.u ? "UsQAdd" : "SuQAdd")},
                                        {"ctx", V(f.rd), V(f.rn), num(n)})));
    }
    case 0x04:
        if (f.size == 3) return invalid();
        return v1(pick(f.u, "cls", "clz"), ty(f.size, !f.u), f.u ? "Clz" : "Cls");
    case 0x05:
        if (!f.u) {
            if (f.size != 0) return invalid();
            return v1("cnt", "uint8_t", "Cnt");
        }
        if (f.size > 1) return invalid();
        if (!emit) return ok(f.size ? "rbit" : "not");
        // Byte lanes: the size field selects NOT (00) or RBIT (01), not the element size.
        return ok(f.size ? "rbit" : "not", set(f.rd, call("v1", {"uint8_t", op(f.size ? "Rbit" : "Not")},
                                                        {"ctx", V(f.rn), num(f.q ? 16 : 8)})));
    case 0x07:
        if (q64bad) return invalid();
        return v1(pick(f.u, "sqabs", "sqneg"), ty(f.size, true), f.u ? "QNeg" : "QAbs");
    case 0x08:
        if (q64bad) return invalid();
        return v1(pick(f.u, "cmgt", "cmge"), ty(f.size, true), f.u ? "CmGe0" : "CmGt0");
    case 0x09:
        if (q64bad) return invalid();
        return v1(pick(f.u, "cmeq", "cmle"), ty(f.size, true), f.u ? "CmLe0" : "CmEq0");
    case 0x0A:
        if (f.u || q64bad) return invalid();
        return v1("cmlt", ty(f.size, true), "CmLt0");
    case 0x0B:
        if (q64bad) return invalid();
        return v1(pick(f.u, "abs", "neg"), ty(f.size, true), f.u ? "Neg" : "Abs");
    case 0x12:
        if (f.u) return narrow("sqxtun", true, false, "QXtn");
        return narrow("xtn", false, false, "Xtn");
    case 0x13: {  // SHLL
        if (!f.u || f.size == 3) return invalid();
        if (!emit) return ok("shll");
        const unsigned nn = 8u >> f.size;
        return ok("shll", set(f.rd, call("l1i", {ty(f.size, false), ty(f.size + 1, false), op("ShlI")},
                                          {"ctx", V(f.rn), num(f.q ? nn : 0), num(nn), num(8u << f.size)})));
    }
    case 0x14: return narrow(pick(f.u, "sqxtn", "uqxtn"), !f.u, !f.u, "QXtn");
    default: return invalid();
    }
}

// ---- across lanes / scalar pairwise -------------------------------------------------------------

Lifted across_lanes(uint32_t raw, bool emit) {
    const Fields f(raw);
    const uint32_t opc = bits(raw, 16, 12);
    if (f.size == 3 || (f.size == 2 && !f.q)) return invalid();
    const unsigned n = (f.q ? 16u : 8u) >> f.size;
    auto acr = [&](const char* name, const std::string& t, const char* fn) {
        if (!emit) return ok(name);
        return ok(name, set(f.rd, call("across", {t, op(fn)}, {"ctx", V(f.rn), num(n)})));
    };
    switch (opc) {
    case 0x03: {
        const char* name = pick(f.u, "saddlv", "uaddlv");
        if (!emit) return ok(name);
        return ok(name, set(f.rd, call("acrossl", {ty(f.size, !f.u), ty(f.size + 1, !f.u)}, {V(f.rn), num(n)})));
    }
    case 0x0A: return acr(pick(f.u, "smaxv", "umaxv"), ty(f.size, !f.u), "Max");
    case 0x1A: return acr(pick(f.u, "sminv", "uminv"), ty(f.size, !f.u), "Min");
    case 0x1B:
        if (f.u) return invalid();
        return acr("addv", ty(f.size, false), "Add");
    default: return invalid();
    }
}

Lifted scalar_pairwise(uint32_t raw, bool emit) {
    const Fields f(raw);
    if (bits(raw, 16, 12) != 0x1B || f.u || f.size != 3) return invalid();
    if (!emit) return ok("addp");
    return ok("addp", set(f.rd, call("v2", {"uint64_t", op("Add")},
                                      {"ctx", V(f.rn), call("bcast", {"uint64_t"}, {V(f.rn), "1"}), "1"})));
}

// ---- shift by immediate ---------------------------------------------------------------------------

Lifted shift_imm(uint32_t raw, bool scalar, bool emit) {
    const Fields f(raw);
    const uint32_t immh = bits(raw, 22, 19);
    const uint32_t immhb = bits(raw, 22, 16);
    const uint32_t opc = bits(raw, 15, 11);
    const unsigned size = immh_size(immh);
    const unsigned esize = 8u << size;
    const int rshift = static_cast<int>(2 * esize - immhb);
    const int lshift = static_cast<int>(immhb - esize);
    const unsigned n = scalar ? 1u : ((f.q ? 16u : 8u) >> size);
    const bool sgn = !f.u;

    const bool narrowing = opc >= 0x10 && opc <= 0x14;
    if (narrowing) {
        if (size == 3) return invalid();
    } else if (scalar) {
        // Only SQSHL/UQSHL/SQSHLU (immediate) exist for all element sizes.
        if (opc != 0x0C && opc != 0x0E && size != 3) return invalid();
    } else if (size == 3 && !f.q) {
        return invalid();
    }
    if (scalar && opc == 0x14) return invalid();  // no scalar SSHLL

    auto v1i = [&](const char* name, const std::string& t, const char* fn, int s) {
        if (!emit) return ok(name);
        return ok(name, set(f.rd, call("v1i", {t, op(fn)}, {"ctx", V(f.rn), num(n), std::to_string(s)})));
    };
    auto v2i = [&](const char* name, const std::string& t, const char* fn, int s) {
        if (!emit) return ok(name);
        return ok(name, set(f.rd, call("v2i", {t, op(fn)}, {"ctx", V(f.rd), V(f.rn), num(n), std::to_string(s)})));
    };
    auto n1i = [&](const char* name, bool src_signed, bool dst_signed, const char* fn) {
        if (!emit) return ok(name);
        const unsigned nn = scalar ? 1u : (8u >> size);
        return ok(name, set(f.rd, call("n1i", {ty(size + 1, src_signed), ty(size, dst_signed), op(fn)},
                                        {"ctx", V(f.rd), V(f.rn), num(nn), b(!scalar && f.q), std::to_string(rshift)})));
    };

    switch (opc) {
    case 0x00: return v1i(pick(f.u, "sshr", "ushr"), ty(size, sgn), "Shr", rshift);
    case 0x02: return v2i(pick(f.u, "ssra", "usra"), ty(size, sgn), "Sra", rshift);
    case 0x04: return v1i(pick(f.u, "srshr", "urshr"), ty(size, sgn), "Rshr", rshift);
    case 0x06: return v2i(pick(f.u, "srsra", "ursra"), ty(size, sgn), "Rsra", rshift);
    case 0x08:
        if (!f.u) return invalid();
        return v2i("sri", ty(size, false), "Sri", rshift);
    case 0x0A:
        if (f.u) return v2i("sli", ty(size, false), "Sli", lshift);
        return v1i("shl", ty(size, false), "ShlI", lshift);
    case 0x0C:
        if (!f.u) return invalid();
        return v1i("sqshlu", ty(size, true), "QShlU", lshift);
    case 0x0E: return v1i(pick(f.u, "sqshl", "uqshl"), ty(size, sgn), "QShlI", lshift);
    case 0x10:
        if (f.u) return n1i("sqshrun", true, false, "QShrN<false>");
        if (scalar) return invalid();
        return n1i("shrn", false, false, "ShrN<false>");
    case 0x11:
        if (f.u) return n1i("sqrshrun", true, false, "QShrN<true>");
        if (scalar) return invalid();
        return n1i("rshrn", false, false, "ShrN<true>");
    case 0x12: return n1i(pick(f.u, "sqshrn", "uqshrn"), sgn, sgn, "QShrN<false>");
    case 0x13: return n1i(pick(f.u, "sqrshrn", "uqrshrn"), sgn, sgn, "QShrN<true>");
    case 0x14: {
        const char* name = pick(f.u, "sshll", "ushll");
        if (!emit) return ok(name);
        const unsigned nn = 8u >> size;
        return ok(name, set(f.rd, call("l1i", {ty(size, sgn), ty(size + 1, sgn), op("ShlI")},
                                        {"ctx", V(f.rn), num(f.q ? nn : 0), num(nn), std::to_string(lshift)})));
    }
    default: return invalid();
    }
}

// ---- by element -------------------------------------------------------------------------------------

Lifted indexed(uint32_t raw, bool scalar, bool emit) {
    const Fields f(raw);
    const uint32_t opc = bits(raw, 15, 12);
    const uint32_t L = bit(raw, 21), M = bit(raw, 20), H = bit(raw, 11);
    unsigned idx, rm;
    if (f.size == 1) {
        idx = (H << 2) | (L << 1) | M;
        rm = bits(raw, 19, 16);
    } else if (f.size == 2) {
        idx = (H << 1) | L;
        rm = (M << 4) | bits(raw, 19, 16);
    } else {
        return invalid();
    }
    if (scalar) {
        if (f.u) return invalid();
        switch (opc) {
        case 0x3:
        case 0x7:
        case 0xB:
        case 0xC:
        case 0xD: break;
        default: return invalid();
        }
    }
    const bool part = !scalar && f.q;
    const unsigned n = scalar ? 1u : ((f.q ? 16u : 8u) >> f.size);  // same-size lanes
    const unsigned nn = scalar ? 1u : (8u >> f.size);                // long: wide lanes
    const unsigned first = part ? nn : 0u;
    const bool sgn = !f.u;
    auto elem = [&](bool s) { return call("bcast", {ty(f.size, s)}, {V(rm), num(idx)}); };
    auto same = [&](const char* name, const char* fn, bool acc, bool s) {
        if (!emit) return ok(name);
        if (acc)
            return ok(name, set(f.rd, call("v3", {ty(f.size, s), op(fn)}, {"ctx", V(f.rd), V(f.rn), elem(s), num(n)})));
        return ok(name, set(f.rd, call("v2", {ty(f.size, s), op(fn)}, {"ctx", V(f.rn), elem(s), num(n)})));
    };
    auto lng = [&](const char* name, const char* fn, bool acc, bool s) {
        if (!emit) return ok(name);
        const std::string tn = ty(f.size, s), tw = ty(f.size + 1, s);
        if (acc)
            return ok(name, set(f.rd, call("l3", {tn, tw, op(fn)},
                                            {"ctx", V(f.rd), V(f.rn), elem(s), num(first), num(nn)})));
        return ok(name, set(f.rd, call("l2", {tn, tw, op(fn)}, {"ctx", V(f.rn), elem(s), num(first), num(nn)})));
    };

    switch (opc) {
    case 0x0:
        if (!f.u) return invalid();
        return same("mla", "Mla", true, false);
    case 0x2: return lng(pick(f.u, "smlal", "umlal"), "Mla", true, sgn);
    case 0x3:
        if (f.u) return invalid();
        return lng("sqdmlal", "QDMlal", true, true);
    case 0x4:
        if (!f.u) return invalid();
        return same("mls", "Mls", true, false);
    case 0x6: return lng(pick(f.u, "smlsl", "umlsl"), "Mls", true, sgn);
    case 0x7:
        if (f.u) return invalid();
        return lng("sqdmlsl", "QDMlsl", true, true);
    case 0x8:
        if (f.u) return invalid();
        return same("mul", "Mul", false, false);
    case 0xA: return lng(pick(f.u, "smull", "umull"), "Mul", false, sgn);
    case 0xB:
        if (f.u) return invalid();
        return lng("sqdmull", "QDMull", false, true);
    case 0xC:
        if (f.u) return invalid();
        return same("sqdmulh", "QDMulH<false>", false, true);
    case 0xD:
        if (f.u) return invalid();  // SQRDMLAH is ARMv8.1 (RDM)
        return same("sqrdmulh", "QDMulH<true>", false, true);
    default: return invalid();  // SDOT/UDOT (v8.2), SQRDMLSH (v8.1), FMLAL (v8.2)
    }
}

}  // namespace

Lifted lift_vec_int(Class cls, uint32_t raw, bool emit) {
    switch (cls) {
    case Class::ThreeSame: return three_same(raw, false, emit);
    case Class::ScalarThreeSame: return three_same(raw, true, emit);
    case Class::ThreeDiff: return three_diff(raw, false, emit);
    case Class::ScalarThreeDiff: return three_diff(raw, true, emit);
    case Class::TwoMisc: return two_misc(raw, false, emit);
    case Class::ScalarTwoMisc: return two_misc(raw, true, emit);
    case Class::AcrossLanes: return across_lanes(raw, emit);
    case Class::ScalarPairwise: return scalar_pairwise(raw, emit);
    case Class::ShiftImm: return shift_imm(raw, false, emit);
    case Class::ScalarShiftImm: return shift_imm(raw, true, emit);
    case Class::Indexed: return indexed(raw, false, emit);
    case Class::ScalarIndexed: return indexed(raw, true, emit);
    default: return invalid();
    }
}

}  // namespace lgp::recomp::a64::simd::detail
