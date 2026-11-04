// SIMD&FP lifter module: ldst_struct (P2.4).
//
// Advanced SIMD load/store multiple structures (LD1-LD4/ST1-ST4, LD1/ST1 with
// 1-4 registers) and single structures (LD1-LD4/ST1-ST4 to one lane,
// LD1R-LD4R), without offset and with post-index (Rm = 31: immediate
// post-index by the transfer size, otherwise by Xm). Rn = 31 is SP; the
// writeback happens after all accesses (Arm ARM DDI 0487, C7.2 LD1 ff.).
#include "internal.hpp"

namespace lgp::recomp::a64::simd::detail {
namespace {

const char* utype(unsigned size) {
    static const char* const k[] = {"uint8_t", "uint16_t", "uint32_t", "uint64_t"};
    return k[size];
}

// Post-index writeback of Xn/SP (`addr` holds the base).
std::string writeback(uint32_t raw, unsigned total_bytes) {
    if (!bit(raw, 23)) return {};
    const unsigned rn = bits(raw, 9, 5), rm = bits(raw, 20, 16);
    const std::string off = rm == 31 ? "UINT64_C(" + num(total_bytes) + ")" : xreg(rm);
    const std::string dst = rn == 31 ? std::string("lgp_sp") : "lgp_x" + num(rn);
    return " " + dst + " = addr + " + off + ";";
}

std::string base(uint32_t raw) { return "uint64_t addr = " + xreg(bits(raw, 9, 5), true) + "; "; }

// The `count` registers starting at t (modulo 32) as the helpers' register
// list: pointers for loads (simd::VRefs), values for stores (simd::VVals),
// so the lifter's scan sees which registers are written (Lifter::lift).
std::string reg_list(unsigned t, unsigned count, bool load) {
    std::string s = "{{";
    for (unsigned i = 0; i < count; ++i)
        s += (i ? ", " : "") + std::string(load ? "&" : "") + vreg((t + i) % 32);
    return s + "}}";
}

Lifted lift_multiple(uint32_t raw, bool emit) {
    const bool q = bit(raw, 30);
    const bool load = bit(raw, 22);
    const uint32_t opcode = bits(raw, 15, 12);
    const unsigned size = bits(raw, 11, 10);
    unsigned rpt, selem;
    switch (opcode) {
    case 0b0000: rpt = 1; selem = 4; break;
    case 0b0010: rpt = 4; selem = 1; break;
    case 0b0100: rpt = 1; selem = 3; break;
    case 0b0110: rpt = 3; selem = 1; break;
    case 0b0111: rpt = 1; selem = 1; break;
    case 0b1000: rpt = 1; selem = 2; break;
    case 0b1010: rpt = 2; selem = 1; break;
    default: return invalid();
    }
    if (size == 3 && !q && selem != 1) return invalid();
    static const char* const kNames[2][5] = {{nullptr, "st1", "st2", "st3", "st4"},
                                             {nullptr, "ld1", "ld2", "ld3", "ld4"}};
    const char* name = kNames[load][selem];
    if (!emit) return ok(name);
    const unsigned total = (q ? 16u : 8u) * rpt * selem;
    std::string s = base(raw) + "lgp::lifted::simd::" + (load ? "ld_multi<" : "st_multi<") + utype(size) +
                    ">(ctx, addr, " + reg_list(bits(raw, 4, 0), rpt + selem - 1, load) + ", " + num(selem) + ", " +
                    num(rpt) + ", " + (q ? "true" : "false") + ");";
    return ok(name, s + writeback(raw, total));
}

Lifted lift_single(uint32_t raw, bool emit) {
    const bool q = bit(raw, 30);
    const bool load = bit(raw, 22);
    const bool r = bit(raw, 21);
    const uint32_t opcode = bits(raw, 15, 13);
    const bool s = bit(raw, 12);
    const unsigned size = bits(raw, 11, 10);
    unsigned scale = opcode >> 1;
    const unsigned selem = (((opcode & 1) << 1) | (r ? 1u : 0u)) + 1;
    bool replicate = false;
    unsigned index = 0;
    switch (scale) {
    case 3:
        if (!load || s) return invalid();
        scale = size;
        replicate = true;
        break;
    case 0: index = (q << 3) | (s << 2) | size; break;
    case 1:
        if (size & 1) return invalid();
        index = (q << 2) | (s << 1) | (size >> 1);
        break;
    case 2:
        if (size & 2) return invalid();
        if ((size & 1) == 0) {
            index = (q << 1) | s;
        } else {
            if (s) return invalid();
            index = q;
            scale = 3;
        }
        break;
    }
    static const char* const kNames[3][5] = {{nullptr, "st1", "st2", "st3", "st4"},
                                             {nullptr, "ld1", "ld2", "ld3", "ld4"},
                                             {nullptr, "ld1r", "ld2r", "ld3r", "ld4r"}};
    const char* name = kNames[replicate ? 2 : (load ? 1 : 0)][selem];
    if (!emit) return ok(name);
    const unsigned total = (1u << scale) * selem;
    const std::string regs = reg_list(bits(raw, 4, 0), selem, load);
    std::string code = base(raw) + "lgp::lifted::simd::";
    if (replicate)
        code += std::string("ld_rep<") + utype(scale) + ">(ctx, addr, " + regs + ", " + num(selem) + ", " +
                (q ? "true" : "false") + ");";
    else
        code += std::string(load ? "ld_single<" : "st_single<") + utype(scale) + ">(ctx, addr, " + regs + ", " +
                num(selem) + ", " + num(index) + ");";
    return ok(name, code + writeback(raw, total));
}

}  // namespace

Lifted lift_ldst_struct(Class cls, uint32_t raw, bool emit) {
    switch (cls) {
    case Class::LdStMultiple: return lift_multiple(raw, emit);
    case Class::LdStSingle: return lift_single(raw, emit);
    default: return invalid();
    }
}

}  // namespace lgp::recomp::a64::simd::detail
