// SIMD&FP lifter module: scalar floating point (P2.4).
//
// Classes FpFixedConv, FpIntConv, FpDp1, FpCompare, FpImm, FpCondCompare,
// FpDp2, FpCondSelect, FpDp3 (Arm ARM C4.1.6). Semantics live in
// lgp/lifted/fp.hpp; this module decodes and emits calls. Half precision
// exists in ARMv8.0 only as a conversion format (FCVT); the FP16 arithmetic
// forms (type = 11) are ARMv8.2 and decode as unallocated.
#include "internal.hpp"

namespace lgp::recomp::a64::simd::detail {
namespace {

const char* utype(unsigned bytes) {
    switch (bytes) {
    case 2: return "uint16_t";
    case 4: return "uint32_t";
    default: return "uint64_t";
    }
}

// Element size in bytes of the FP `type` field (00 single, 01 double, 11 half), 0 if unallocated.
unsigned type_bytes(uint32_t type, bool allow_half) {
    switch (type) {
    case 0b00: return 4;
    case 0b01: return 8;
    case 0b11: return allow_half ? 2 : 0;
    default: return 0;
    }
}

std::string fp_call(const char* fn, unsigned bytes, const std::string& args) {
    return std::string("lgp::lifted::fp::") + fn + "<" + utype(bytes) + ">(lgp_fpcr, " + args + ")";
}

// VFPExpandImm().
uint64_t expand_imm(uint32_t imm8, unsigned bytes) {
    const unsigned n = bytes * 8;
    const unsigned e = bytes == 4 ? 8 : 11;
    const unsigned f = n - e - 1;
    const uint64_t sign = (imm8 >> 7) & 1;
    const uint64_t b6 = (imm8 >> 6) & 1;
    uint64_t exp = ((b6 ^ 1) << (e - 1)) | ((b6 ? ((1ull << (e - 3)) - 1) : 0) << 2) | ((imm8 >> 4) & 3);
    const uint64_t frac = static_cast<uint64_t>(imm8 & 0xF) << (f - 4);
    return (sign << (n - 1)) | (exp << f) | frac;
}

const char* rounding_name(unsigned r) {
    switch (r) {
    case 0: return "lgp::lifted::fp::kTieEven";
    case 1: return "lgp::lifted::fp::kPosInf";
    case 2: return "lgp::lifted::fp::kNegInf";
    case 3: return "lgp::lifted::fp::kZero";
    default: return "lgp::lifted::fp::kTieAway";
    }
}
const char* kFpcrRounding = "lgp::lifted::fp::fpcr_rounding(lgp_fpcr)";

Lifted dp3(uint32_t r, bool emit) {
    if (bit(r, 31) || bit(r, 29)) return invalid();
    const unsigned bytes = type_bytes(bits(r, 23, 22), false);
    if (!bytes) return invalid();
    const unsigned o = (bit(r, 21) << 1) | bit(r, 15);
    static const char* kNames[] = {"fmadd", "fmsub", "fnmadd", "fnmsub"};
    if (!emit) return ok(kNames[o]);
    const std::string T = utype(bytes);
    const std::string a = vscalar(bits(r, 14, 10), bytes), n = vscalar(bits(r, 9, 5), bytes),
                      m = vscalar(bits(r, 20, 16), bytes);
    auto negate = [&](const std::string& v) { return "lgp::lifted::fp::neg<" + T + ">(" + v + ")"; };
    std::string args;
    switch (o) {
    case 0: args = a + ", " + n + ", " + m; break;
    case 1: args = a + ", " + negate(n) + ", " + m; break;
    case 2: args = negate(a) + ", " + negate(n) + ", " + m; break;
    default: args = negate(a) + ", " + n + ", " + m; break;
    }
    return ok(kNames[o], set_vscalar(bits(r, 4, 0), fp_call("muladd", bytes, args)));
}

Lifted dp2(uint32_t r, bool emit) {
    if (bit(r, 31) || bit(r, 29)) return invalid();
    const unsigned bytes = type_bytes(bits(r, 23, 22), false);
    if (!bytes) return invalid();
    const uint32_t opcode = bits(r, 15, 12);
    static const char* kNames[] = {"fmul", "fdiv", "fadd", "fsub", "fmax", "fmin", "fmaxnm", "fminnm", "fnmul"};
    static const char* kFns[] = {"mul", "div", "add", "sub", "max", "min", "maxnum", "minnum", "mul"};
    if (opcode > 8) return invalid();
    if (!emit) return ok(kNames[opcode]);
    std::string e = fp_call(kFns[opcode], bytes, vscalar(bits(r, 9, 5), bytes) + ", " + vscalar(bits(r, 20, 16), bytes));
    if (opcode == 8) e = std::string("lgp::lifted::fp::neg<") + utype(bytes) + ">(" + e + ")";
    return ok(kNames[opcode], set_vscalar(bits(r, 4, 0), e));
}

Lifted dp1(uint32_t r, bool emit) {
    if (bit(r, 31) || bit(r, 29)) return invalid();
    const uint32_t type = bits(r, 23, 22);
    const uint32_t opcode = bits(r, 20, 15);
    const unsigned rd = bits(r, 4, 0), rn = bits(r, 9, 5);

    // FCVT between precisions: opcode 0001xx, opc<1:0> = destination type.
    if ((opcode & 0b111100) == 0b000100) {
        const unsigned src = type_bytes(type, true);
        const unsigned dst = type_bytes(opcode & 3, true);
        if (!src || !dst || src == dst) return invalid();
        if (!emit) return ok("fcvt");
        return ok("fcvt", set_vscalar(rd, std::string("lgp::lifted::fp::convert<") + utype(dst) + ", " + utype(src) +
                                              ">(lgp_fpcr, " + vscalar(rn, src) + ", " + kFpcrRounding + ")"));
    }
    const unsigned bytes = type_bytes(type, false);
    if (!bytes) return invalid();
    const std::string T = utype(bytes);
    const std::string n = vscalar(rn, bytes);
    switch (opcode) {
    case 0b000000: return emit ? ok("fmov", set_vscalar(rd, n)) : ok("fmov");
    case 0b000001: return emit ? ok("fabs", set_vscalar(rd, "lgp::lifted::fp::abs<" + T + ">(" + n + ")")) : ok("fabs");
    case 0b000010: return emit ? ok("fneg", set_vscalar(rd, "lgp::lifted::fp::neg<" + T + ">(" + n + ")")) : ok("fneg");
    case 0b000011: return emit ? ok("fsqrt", set_vscalar(rd, fp_call("sqrt", bytes, n))) : ok("fsqrt");
    default: break;
    }
    if (opcode >= 0b001000 && opcode <= 0b001111 && opcode != 0b001101) {
        static const char* kNames[] = {"frintn", "frintp", "frintm", "frintz", "frinta", "", "frintx", "frinti"};
        const unsigned k = opcode & 7;
        if (!emit) return ok(kNames[k]);
        const std::string mode = k <= 4 ? rounding_name(k == 4 ? 4 : k) : kFpcrRounding;
        return ok(kNames[k], set_vscalar(rd, fp_call("round_int", bytes, n + ", " + mode)));
    }
    return invalid();
}

Lifted compare(uint32_t r, bool emit) {
    if (bit(r, 31) || bit(r, 29) || bits(r, 15, 14) != 0 || bits(r, 2, 0) != 0) return invalid();
    const unsigned bytes = type_bytes(bits(r, 23, 22), false);
    if (!bytes) return invalid();
    const bool with_zero = bit(r, 3);
    const bool signaling = bit(r, 4);
    const char* name = signaling ? "fcmpe" : "fcmp";
    if (with_zero && bits(r, 20, 16) != 0) return unpredictable();  // Rm is "should be zero"
    if (!emit) return ok(name);
    const std::string m = with_zero ? std::string(utype(bytes)) + "{0}" : vscalar(bits(r, 20, 16), bytes);
    return ok(name, "lgp::lifted::set_nzcv_imm(&lgp_f, " + fp_call("compare", bytes, vscalar(bits(r, 9, 5), bytes) + ", " + m) +
                        ");");
}

Lifted imm(uint32_t r, bool emit) {
    if (bit(r, 31) || bit(r, 29) || bits(r, 9, 5) != 0) return invalid();
    const unsigned bytes = type_bytes(bits(r, 23, 22), false);
    if (!bytes) return invalid();
    if (!emit) return ok("fmov");
    return ok("fmov", set_vscalar(bits(r, 4, 0), hex64(expand_imm(bits(r, 20, 13), bytes))));
}

Lifted cond_compare(uint32_t r, bool emit) {
    if (bit(r, 31) || bit(r, 29)) return invalid();
    const unsigned bytes = type_bytes(bits(r, 23, 22), false);
    if (!bytes) return invalid();
    const char* name = bit(r, 4) ? "fccmpe" : "fccmp";
    if (!emit) return ok(name);
    return ok(name, "if (" + cond_expr(bits(r, 15, 12)) + ") lgp::lifted::set_nzcv_imm(&lgp_f, " +
                        fp_call("compare", bytes, vscalar(bits(r, 9, 5), bytes) + ", " + vscalar(bits(r, 20, 16), bytes)) +
                        "); else lgp::lifted::set_nzcv_imm(&lgp_f, " + num(bits(r, 3, 0)) + ");");
}

Lifted cond_select(uint32_t r, bool emit) {
    if (bit(r, 31) || bit(r, 29)) return invalid();
    const unsigned bytes = type_bytes(bits(r, 23, 22), false);
    if (!bytes) return invalid();
    if (!emit) return ok("fcsel");
    return ok("fcsel", set_vscalar(bits(r, 4, 0), "(" + cond_expr(bits(r, 15, 12)) + " ? " + vscalar(bits(r, 9, 5), bytes) +
                                                      " : " + vscalar(bits(r, 20, 16), bytes) + ")"));
}

// FCVT*S/U (FP -> integer) and SCVTF/UCVTF (integer -> FP) with fbits.
std::string to_int(unsigned bytes, unsigned rn, bool sf, unsigned rd, bool is_unsigned, const std::string& fbits,
                   const std::string& mode) {
    const std::string v = std::string("lgp::lifted::fp::to_fixed<") + utype(bytes) + ">(lgp_fpcr, " + vscalar(rn, bytes) +
                          ", " + fbits + ", " + (is_unsigned ? "true" : "false") + ", " + (sf ? "64" : "32") + ", " +
                          mode + ")";
    return set_x(rd, v);
}
std::string from_int(unsigned bytes, unsigned rn, bool sf, unsigned rd, bool is_unsigned, const std::string& fbits) {
    return set_vscalar(rd, std::string("lgp::lifted::fp::from_fixed<") + utype(bytes) + ">(lgp_fpcr, " + xreg(rn) + ", " +
                               fbits + ", " + (is_unsigned ? "true" : "false") + ", " + (sf ? "64" : "32") + ", " +
                               kFpcrRounding + ")");
}

Lifted fixed_conv(uint32_t r, bool emit) {
    if (bit(r, 29)) return invalid();
    const bool sf = bit(r, 31);
    const unsigned bytes = type_bytes(bits(r, 23, 22), false);
    if (!bytes) return invalid();
    const uint32_t scale = bits(r, 15, 10);
    if (!sf && !bit(scale, 5)) return invalid();  // fbits > 32 for a 32-bit integer
    const std::string fbits = num(64 - scale);
    const uint32_t rmode_op = bits(r, 20, 16);
    const unsigned rd = bits(r, 4, 0), rn = bits(r, 9, 5);
    switch (rmode_op) {
    case 0b00010: return emit ? ok("scvtf", from_int(bytes, rn, sf, rd, false, fbits)) : ok("scvtf");
    case 0b00011: return emit ? ok("ucvtf", from_int(bytes, rn, sf, rd, true, fbits)) : ok("ucvtf");
    case 0b11000:
        return emit ? ok("fcvtzs", to_int(bytes, rn, sf, rd, false, fbits, rounding_name(3))) : ok("fcvtzs");
    case 0b11001:
        return emit ? ok("fcvtzu", to_int(bytes, rn, sf, rd, true, fbits, rounding_name(3))) : ok("fcvtzu");
    default: return invalid();
    }
}

Lifted int_conv(uint32_t r, bool emit) {
    if (bit(r, 29)) return invalid();
    const bool sf = bit(r, 31);
    const uint32_t type = bits(r, 23, 22);
    const uint32_t rmode = bits(r, 20, 19);
    const uint32_t opcode = bits(r, 18, 16);
    const unsigned rd = bits(r, 4, 0), rn = bits(r, 9, 5);

    if (opcode == 0b110 || opcode == 0b111) {
        // FMOV between general and SIMD&FP registers.
        const bool to_general = opcode == 0b110;
        if (rmode == 0b00 && ((!sf && type == 0b00) || (sf && type == 0b01))) {
            if (!emit) return ok("fmov");
            if (to_general) return ok("fmov", set_x(rd, "static_cast<uint64_t>(" + vscalar(rn, sf ? 8 : 4) + ")"));
            return ok("fmov", set_vscalar(rd, sf ? xreg(rn) : "static_cast<uint32_t>(" + xreg(rn) + ")"));
        }
        if (rmode == 0b01 && sf && type == 0b10) {  // top half: Xd <-> Vd.D[1]
            if (!emit) return ok("fmov");
            if (to_general) return ok("fmov", set_x(rd, vreg(rn) + ".hi"));
            return ok("fmov", vreg(rd) + ".hi = " + xreg(rn) + ";");  // the low half is kept
        }
        return invalid();  // FP16 forms (ARMv8.2), FJCVTZS (ARMv8.3)
    }

    const unsigned bytes = type_bytes(type, false);
    if (!bytes) return invalid();
    switch (opcode) {
    case 0b000:
    case 0b001: {  // FCVT{N,P,M,Z}{S,U}
        static const char* kS[] = {"fcvtns", "fcvtps", "fcvtms", "fcvtzs"};
        static const char* kU[] = {"fcvtnu", "fcvtpu", "fcvtmu", "fcvtzu"};
        const bool u = opcode == 0b001;
        const char* name = u ? kU[rmode] : kS[rmode];
        if (!emit) return ok(name);
        return ok(name, to_int(bytes, rn, sf, rd, u, "0", rounding_name(rmode)));
    }
    case 0b010:
    case 0b011: {
        if (rmode != 0) return invalid();
        const bool u = opcode == 0b011;
        const char* name = u ? "ucvtf" : "scvtf";
        if (!emit) return ok(name);
        return ok(name, from_int(bytes, rn, sf, rd, u, "0"));
    }
    case 0b100:
    case 0b101: {
        if (rmode != 0) return invalid();
        const bool u = opcode == 0b101;
        const char* name = u ? "fcvtau" : "fcvtas";
        if (!emit) return ok(name);
        return ok(name, to_int(bytes, rn, sf, rd, u, "0", rounding_name(4)));
    }
    default: return invalid();
    }
}

}  // namespace

Lifted lift_fp_scalar(Class cls, uint32_t raw, bool emit) {
    switch (cls) {
    case Class::FpDp3: return dp3(raw, emit);
    case Class::FpDp2: return dp2(raw, emit);
    case Class::FpDp1: return dp1(raw, emit);
    case Class::FpCompare: return compare(raw, emit);
    case Class::FpImm: return imm(raw, emit);
    case Class::FpCondCompare: return cond_compare(raw, emit);
    case Class::FpCondSelect: return cond_select(raw, emit);
    case Class::FpFixedConv: return fixed_conv(raw, emit);
    case Class::FpIntConv: return int_conv(raw, emit);
    default: return invalid();
    }
}

}  // namespace lgp::recomp::a64::simd::detail
