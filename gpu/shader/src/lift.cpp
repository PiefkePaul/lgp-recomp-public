#include <cstdio>
#include <map>
#include <set>
#include <vector>

#include "lgp/gpu/shader/ir.h"
#include "lgp/gpu/shader/maxwell.h"

namespace lgp::gpu::ir {
namespace {

namespace mw = lgp::gpu::maxwell;
using mw::bit;
using mw::field;

constexpr uint32_t kRZ = mw::kRegZero;
constexpr uint32_t kPT = mw::kPredTrue;
constexpr uint32_t kTrueCC = 0xf;  // condition code "always" in branch/exit/kil

// Branch/SSY/PBK target. Compiled code often jumps to the start of a bundle, i.e. to the
// scheduling word; execution then resumes with the bundle's first instruction.
uint32_t branch_target(uint32_t addr, uint64_t w) {
    const auto t = static_cast<uint32_t>(static_cast<int64_t>(addr) + 8 + mw::branch_offset(w));
    return mw::is_sched_slot(t) ? t + 8 : t;
}

class Lifter {
public:
    Lifter(const uint8_t* code, size_t bytes) : code_(code), bytes_(bytes) {}

    bool run(std::vector<Inst>& out, std::string& error);

private:
    uint64_t word_at(uint32_t addr) const {
        uint64_t w = 0;
        for (int i = 7; i >= 0; --i) w = (w << 8) | code_[addr + static_cast<uint32_t>(i)];
        return w;
    }
    bool fail(uint32_t addr, uint64_t w, const std::string& what) {
        char buf[96];
        std::snprintf(buf, sizeof buf, "0x%04x: %016llx: ", addr, static_cast<unsigned long long>(w));
        error_ = buf + what;
        return false;
    }
    bool lift_one(uint32_t addr, const mw::Instr& in, std::vector<Inst>& out, std::vector<uint32_t>& succ,
                  bool& falls_through);

    const uint8_t* code_;
    size_t bytes_;
    std::string error_;
};

Operand operand_b(const mw::Instr& in, bool is_float) {
    const uint64_t w = in.raw;
    switch (in.form) {
    case mw::Form::Reg: return Operand::reg(mw::rb(w));
    case mw::Form::CBuf: {
        Operand o{Kind::CBuf, mw::cbuf_bank(w)};
        o.imm = mw::cbuf_offset(w);
        return o;
    }
    case mw::Form::Imm20: return Operand::imm32(is_float ? mw::imm20_float(w) : mw::imm20_int(w));
    case mw::Form::Imm32: return Operand::imm32(mw::imm32(w));
    case mw::Form::Imm16: return Operand::imm32(field(w, 20, 16));
    case mw::Form::RegC:
    case mw::Form::None: break;
    }
    return {};
}

Operand fmod(Operand o, bool neg, bool abs) {
    o.neg = neg;
    o.abs = abs;
    return o;
}

Inst make(Opcode op, uint32_t addr, uint64_t w) {
    Inst i;
    i.op = op;
    i.address = addr;
    i.guard = mw::guard_pred(w);
    i.guard_inv = mw::guard_not(w);
    return i;
}

int32_t sext(uint32_t v, unsigned bits) {
    const uint32_t m = 1u << (bits - 1);
    return static_cast<int32_t>((v ^ m) - m);
}

// TEXS/TLDS destinations: two registers each at rd0 (bits 0..7) and rd1 (bits 28..35), the
// component mask from bits 50..52 (meaning depends on whether rd1 is RZ). Sets dst and aux3.
bool texs_dests(uint64_t w, Inst& i) {
    const uint32_t rd0 = mw::rd(w), rd1 = field(w, 28, 8);
    const uint32_t m = field(w, 50, 3);
    static constexpr uint32_t kTwo[] = {0x1, 0x2, 0x4, 0x8, 0x3, 0x9, 0xa, 0xc};
    static constexpr uint32_t kFour[] = {0x7, 0xb, 0xd, 0xe, 0xf};
    uint32_t mask;
    if (rd1 == kRZ) {
        mask = kTwo[m];
    } else {
        if (m > 4) return false;
        mask = kFour[m];
    }
    // Components in order: the first two go to rd0, rd0+1, the rest to rd1, rd1+1.
    uint32_t n = 0;
    for (uint32_t c = 0; c < 4; ++c) {
        if (!(mask & (1u << c))) continue;
        const uint32_t base = n < 2 ? rd0 : rd1;
        i.dst.push_back(base == kRZ ? kRZ : base + (n & 1));
        ++n;
    }
    i.aux3 = mask;
    return true;
}

// Data registers of a memory access: one for up to 32 bits, two for 64, four for 128.
void add_mem_data(Inst& i, uint32_t rd, uint32_t size, bool load) {
    const uint32_t n = size == static_cast<uint32_t>(MemSize::B128) ? 4 : size == static_cast<uint32_t>(MemSize::B64) ? 2 : 1;
    for (uint32_t k = 0; k < n; ++k) {
        const uint32_t r = rd == kRZ ? kRZ : rd + k;
        if (load)
            i.dst.push_back(r);
        else
            i.src.push_back(Operand::reg(r));
    }
}

bool Lifter::lift_one(uint32_t addr, const mw::Instr& in, std::vector<Inst>& out, std::vector<uint32_t>& succ,
                      bool& falls_through) {
    const uint64_t w = in.raw;
    falls_through = true;
    const bool always = mw::guard_pred(w) == kPT && !mw::guard_not(w);
    if (mw::guard_pred(w) == kPT && mw::guard_not(w)) return true;  // "never": no effect

    auto unsupported = [&](const char* what) { return fail(addr, w, std::string("unsupported: ") + what); };
    auto no_cc = [&](unsigned b) { return !bit(w, b) || unsupported(".CC (condition-code write)"); };
    auto rnd_default = [&](unsigned lo) { return field(w, lo, 2) == 0 || unsupported("non-default rounding mode"); };

    switch (in.op) {
    case mw::Op::FADD: {
        if (!no_cc(47) || !rnd_default(39)) return false;
        Inst i = make(Opcode::FAdd, addr, w);
        i.dst = {mw::rd(w)};
        i.src = {fmod(Operand::reg(mw::ra(w)), bit(w, 48), bit(w, 46)), fmod(operand_b(in, true), bit(w, 45), bit(w, 49))};
        i.sat = bit(w, 50);
        out.push_back(i);
        return true;
    }
    case mw::Op::FADD32I: {
        if (!no_cc(52)) return false;
        Inst i = make(Opcode::FAdd, addr, w);
        i.dst = {mw::rd(w)};
        i.src = {fmod(Operand::reg(mw::ra(w)), bit(w, 56), bit(w, 54)), fmod(operand_b(in, true), bit(w, 53), bit(w, 57))};
        out.push_back(i);
        return true;
    }
    case mw::Op::FMUL: {
        // .RZ is emulated exactly in the backend (compilers use it before float-to-int conversions);
        // .RM/.RP are not supported.
        if (!no_cc(47)) return false;
        if (field(w, 39, 2) == 1 || field(w, 39, 2) == 2) return unsupported("non-default rounding mode");
        if (field(w, 39, 2) == 3 && field(w, 44, 2) == 0) return unsupported("FMUL.RZ without .FTZ/.FMZ");
        if (field(w, 41, 3) == 7) return unsupported("invalid FMUL scale");
        Inst i = make(Opcode::FMul, addr, w);
        i.scale = field(w, 41, 3);
        i.aux = field(w, 39, 2);
        i.dst = {mw::rd(w)};
        i.src = {Operand::reg(mw::ra(w)), fmod(operand_b(in, true), bit(w, 48), false)};
        i.sat = bit(w, 50);
        out.push_back(i);
        return true;
    }
    case mw::Op::FMUL32I: {
        if (!no_cc(52)) return false;
        Inst i = make(Opcode::FMul, addr, w);
        i.dst = {mw::rd(w)};
        i.src = {Operand::reg(mw::ra(w)), operand_b(in, true)};
        i.sat = bit(w, 55);
        out.push_back(i);
        return true;
    }
    case mw::Op::FFMA: {
        if (!no_cc(47) || !rnd_default(51)) return false;
        Inst i = make(Opcode::FFma, addr, w);
        i.dst = {mw::rd(w)};
        Operand b, c;
        if (in.form == mw::Form::RegC) {
            b = Operand::reg(mw::rc(w));
            c = Operand{Kind::CBuf, mw::cbuf_bank(w)};
            c.imm = mw::cbuf_offset(w);
        } else {
            b = operand_b(in, true);
            c = Operand::reg(mw::rc(w));
        }
        i.src = {Operand::reg(mw::ra(w)), fmod(b, bit(w, 48), false), fmod(c, bit(w, 49), false)};
        i.sat = bit(w, 50);
        out.push_back(i);
        return true;
    }
    case mw::Op::FMNMX: {
        if (!no_cc(47)) return false;
        // Predicate true selects the minimum.
        Inst mn = make(Opcode::FMin, addr, w);
        mn.dst = {mw::rd(w)};
        mn.src = {fmod(Operand::reg(mw::ra(w)), bit(w, 48), bit(w, 46)), fmod(operand_b(in, true), bit(w, 45), bit(w, 49)),
                  Operand::pred(field(w, 39, 3), bit(w, 42))};
        mn.op = Opcode::FMin;
        out.push_back(mn);
        return true;
    }
    case mw::Op::FSETP: {
        Inst i = make(Opcode::FSetP, addr, w);
        i.pdst = {field(w, 3, 3), field(w, 0, 3)};
        i.src = {fmod(Operand::reg(mw::ra(w)), bit(w, 43), bit(w, 7)), fmod(operand_b(in, true), bit(w, 6), bit(w, 44)),
                 Operand::pred(field(w, 39, 3), bit(w, 42))};
        i.aux = field(w, 48, 4);
        i.aux2 = field(w, 45, 2);
        if (i.aux2 == 3) return unsupported("invalid boolean op");
        out.push_back(i);
        return true;
    }
    case mw::Op::FSET: {
        if (!no_cc(47)) return false;
        Inst i = make(bit(w, 52) ? Opcode::FSetBool : Opcode::FSetMask, addr, w);
        i.dst = {mw::rd(w)};
        i.src = {fmod(Operand::reg(mw::ra(w)), bit(w, 43), bit(w, 54)), fmod(operand_b(in, true), bit(w, 53), bit(w, 44)),
                 Operand::pred(field(w, 39, 3), bit(w, 42))};
        i.aux = field(w, 48, 4);
        i.aux2 = field(w, 45, 2);
        if (i.aux2 == 3) return unsupported("invalid boolean op");
        out.push_back(i);
        return true;
    }
    case mw::Op::MUFU: {
        static constexpr Opcode kOps[] = {Opcode::FCos, Opcode::FSin, Opcode::FEx2, Opcode::FLg2,
                                          Opcode::FRcp, Opcode::FRsq, Opcode::Mov,  Opcode::Mov, Opcode::FSqrt};
        const uint32_t f = field(w, 20, 4);
        if (f > 8 || f == 6 || f == 7) return unsupported("MUFU function (rcp64h/rsq64h/unknown)");
        Inst i = make(kOps[f], addr, w);
        i.dst = {mw::rd(w)};
        i.src = {fmod(Operand::reg(mw::ra(w)), bit(w, 48), bit(w, 46))};
        i.sat = bit(w, 50);
        out.push_back(i);
        return true;
    }
    case mw::Op::RRO: {
        // Range reduction before MUFU.SIN/COS/EX2. The hardware MUFU expects a
        // pre-scaled operand; like Ryujinx we treat RRO as a move and MUFU as
        // the plain function (see docs/knowledge/maxwell-shader-to-wgsl.md).
        Inst i = make(Opcode::FMov, addr, w);
        i.dst = {mw::rd(w)};
        i.src = {fmod(operand_b(in, true), bit(w, 45), bit(w, 49))};
        out.push_back(i);
        return true;
    }
    case mw::Op::F2F: {
        if (!no_cc(47)) return false;
        if (field(w, 8, 2) != 2 || field(w, 10, 2) != 2) return unsupported("F2F with non-f32 types");
        if (bit(w, 41)) return unsupported("F2F half select");
        const uint32_t m = field(w, 39, 2) | (field(w, 42, 1) << 2);
        static constexpr Opcode kOps[] = {Opcode::FMov,  Opcode::Mov,    Opcode::Mov,   Opcode::FMov,
                                          Opcode::FRound, Opcode::FFloor, Opcode::FCeil, Opcode::FTrunc};
        if (m == 1 || m == 2) return unsupported("F2F rounding mode .RM/.RP");
        Inst i = make(kOps[m], addr, w);
        i.dst = {mw::rd(w)};
        i.src = {fmod(operand_b(in, true), bit(w, 45), bit(w, 49))};
        i.sat = bit(w, 50);
        out.push_back(i);
        return true;
    }
    case mw::Op::F2I: {
        if (!no_cc(47)) return false;
        // Destination 8, 16 or 32 bits (saturated to its range), source f32.
        if (field(w, 8, 2) == 3 || field(w, 10, 2) != 2) return unsupported("F2I with 64-bit or non-f32 types");
        if (bit(w, 41)) return unsupported("F2I half select");
        Inst i = make(bit(w, 12) ? Opcode::F2I : Opcode::F2U, addr, w);
        i.dst = {mw::rd(w)};
        i.src = {fmod(operand_b(in, true), bit(w, 45), bit(w, 49))};
        i.aux = field(w, 39, 2);
        i.aux2 = field(w, 8, 2);
        out.push_back(i);
        return true;
    }
    case mw::Op::I2F: {
        if (!no_cc(47)) return false;
        // Round to nearest (0) and toward zero (3) are exact in WGSL; .RM/.RP are not supported.
        if (field(w, 39, 2) == 1 || field(w, 39, 2) == 2) return unsupported("I2F rounding mode .RM/.RP");
        if (field(w, 8, 2) != 2) return unsupported("I2F to a non-32-bit float");
        // Source: 8, 16 or 32 bits, picked from the register by byte select (as in I2I).
        const uint32_t ssize = field(w, 10, 2), sel = field(w, 41, 2);
        if (ssize == 3) return unsupported("I2F from a 64-bit integer");
        if ((ssize == 2 && sel != 0) || (ssize == 1 && (sel & 1))) return unsupported("invalid I2F byte select");
        Inst i = make(bit(w, 13) ? Opcode::I2F : Opcode::U2F, addr, w);
        i.dst = {mw::rd(w)};
        i.src = {fmod(operand_b(in, false), bit(w, 45), bit(w, 49))};
        i.aux = field(w, 39, 2);
        i.aux2 = ssize;
        i.aux3 = sel;
        out.push_back(i);
        return true;
    }
    case mw::Op::IADD:
    case mw::Op::IADD32I: {
        // .CC writes the flags (zero, sign, carry, overflow); .X adds the carry flag, and its
        // operand modifiers mean bitwise NOT instead of negation (a + ~b + C = a - b - borrow).
        const bool i32 = in.op == mw::Op::IADD32I;
        if (bit(w, i32 ? 54 : 50)) return unsupported("IADD.SAT");
        const bool x = bit(w, i32 ? 53 : 43);
        const bool na = bit(w, i32 ? 56 : 49);
        const bool nb = !i32 && bit(w, 48);
        if (na && nb) return unsupported("IADD.PO");
        Inst i = make(Opcode::IAdd, addr, w);
        i.dst = {mw::rd(w)};
        Operand a = Operand::reg(mw::ra(w)), b = operand_b(in, false);
        if (x) {
            a.inv = na;
            b.inv = nb;
        } else {
            a.neg = na;
            b.neg = nb;
        }
        i.src = {a, b};
        i.cc = bit(w, i32 ? 52 : 47);
        i.carry_in = x;
        out.push_back(i);
        return true;
    }
    case mw::Op::ISCADD: {
        if (!no_cc(47)) return false;
        Inst i = make(Opcode::IScAdd, addr, w);
        i.dst = {mw::rd(w)};
        i.src = {fmod(Operand::reg(mw::ra(w)), bit(w, 49), false), fmod(operand_b(in, false), bit(w, 48), false)};
        i.aux = field(w, 39, 5);
        out.push_back(i);
        return true;
    }
    case mw::Op::SHL: {
        if (!no_cc(47)) return false;
        if (bit(w, 43)) return unsupported("SHL.X");
        Inst i = make(Opcode::Shl, addr, w);
        i.dst = {mw::rd(w)};
        i.src = {Operand::reg(mw::ra(w)), operand_b(in, false)};
        i.aux = bit(w, 39);
        out.push_back(i);
        return true;
    }
    case mw::Op::SHR: {
        if (!no_cc(47)) return false;
        if (bit(w, 44)) return unsupported("SHR.X");
        if (bit(w, 40)) return unsupported("SHR.BREV");
        Inst i = make(bit(w, 48) ? Opcode::ShrS : Opcode::ShrU, addr, w);
        i.dst = {mw::rd(w)};
        i.src = {Operand::reg(mw::ra(w)), operand_b(in, false)};
        i.aux = bit(w, 39);
        out.push_back(i);
        return true;
    }
    case mw::Op::LOP:
    case mw::Op::LOP32I: {
        const bool i32 = in.op == mw::Op::LOP32I;
        if (!no_cc(i32 ? 52 : 47)) return false;
        if (bit(w, i32 ? 57 : 43)) return unsupported("LOP.X");
        const uint32_t pmode = i32 ? 0 : field(w, 44, 2), pdst = i32 ? kPT : field(w, 48, 3);
        if (pmode == 0 && pdst != kPT) return unsupported("LOP predicate output without mode");
        const uint32_t op = i32 ? field(w, 53, 2) : field(w, 41, 2);
        Operand a = Operand::reg(mw::ra(w));
        a.inv = bit(w, i32 ? 55 : 39);
        Operand b = operand_b(in, false);
        b.inv = bit(w, i32 ? 56 : 40);
        static constexpr Opcode kOps[] = {Opcode::And, Opcode::Or, Opcode::Xor, Opcode::Mov};
        Inst i = make(kOps[op], addr, w);
        i.dst = {mw::rd(w)};
        if (op == 3)
            i.src = {b};  // PASS_B
        else
            i.src = {a, b};
        if (pmode != 0 && pdst != kPT) {
            i.pdst = {pdst};
            i.pmode = pmode;
        }
        out.push_back(i);
        return true;
    }
    case mw::Op::ISETP: {
        if (bit(w, 43)) return unsupported("ISETP.X");
        Inst i = make(Opcode::ISetP, addr, w);
        i.pdst = {field(w, 3, 3), field(w, 0, 3)};
        i.src = {Operand::reg(mw::ra(w)), operand_b(in, false), Operand::pred(field(w, 39, 3), bit(w, 42))};
        i.aux = field(w, 49, 3);
        i.aux2 = field(w, 45, 2);
        i.aux3 = bit(w, 48);
        if (i.aux2 == 3) return unsupported("invalid boolean op");
        out.push_back(i);
        return true;
    }
    case mw::Op::ISET: {
        if (!no_cc(47)) return false;
        if (bit(w, 43)) return unsupported("ISET.X");
        Inst i = make(bit(w, 44) ? Opcode::ISetBool : Opcode::ISetMask, addr, w);
        i.dst = {mw::rd(w)};
        i.src = {Operand::reg(mw::ra(w)), operand_b(in, false), Operand::pred(field(w, 39, 3), bit(w, 42))};
        i.aux = field(w, 49, 3);
        i.aux2 = field(w, 45, 2);
        i.aux3 = bit(w, 48);
        if (i.aux2 == 3) return unsupported("invalid boolean op");
        out.push_back(i);
        return true;
    }
    case mw::Op::IADD3: {
        if (!no_cc(47)) return false;
        if (bit(w, 48)) return unsupported("IADD3.X (carry in)");
        if (in.form == mw::Form::Reg && field(w, 31, 8) != 0) return unsupported("IADD3 shift or half-word select");
        Inst i = make(Opcode::IAdd3, addr, w);
        i.dst = {mw::rd(w)};
        i.src = {fmod(Operand::reg(mw::ra(w)), bit(w, 51), false), fmod(operand_b(in, false), bit(w, 50), false),
                 fmod(Operand::reg(mw::rc(w)), bit(w, 49), false)};
        out.push_back(i);
        return true;
    }
    case mw::Op::XMAD: {
        if (!no_cc(47)) return false;
        const bool rb_form = in.form == mw::Form::Reg || in.form == mw::Form::Imm16;
        if (bit(w, rb_form ? 38 : 54)) return unsupported("XMAD.X (carry in)");
        uint32_t mode = rb_form ? field(w, 50, 3) : field(w, 50, 2);
        if (mode == 3) return unsupported("XMAD.CSFU");
        if (mode > 4) return unsupported("invalid XMAD mode");
        Inst i = make(Opcode::XMad, addr, w);
        i.dst = {mw::rd(w)};
        Operand b, c;
        if (in.form == mw::Form::RegC) {
            b = Operand::reg(mw::rc(w));
            c = Operand{Kind::CBuf, mw::cbuf_bank(w)};
            c.imm = mw::cbuf_offset(w);
        } else {
            b = operand_b(in, false);
            c = Operand::reg(mw::rc(w));
        }
        uint32_t flags = 0;
        if (bit(w, 48)) flags |= ir::kXMadSignA;
        if (bit(w, 49)) flags |= ir::kXMadSignB;
        if (bit(w, 53)) flags |= ir::kXMadHiA;
        if (in.form == mw::Form::Reg ? bit(w, 35) : (in.form != mw::Form::Imm16 && bit(w, 52))) flags |= ir::kXMadHiB;
        if (in.form == mw::Form::Reg || in.form == mw::Form::Imm16) {
            if (bit(w, 36)) flags |= ir::kXMadPsl;
            if (bit(w, 37)) flags |= ir::kXMadMrg;
        } else if (in.form == mw::Form::CBuf) {
            if (bit(w, 55)) flags |= ir::kXMadPsl;
            if (bit(w, 56)) flags |= ir::kXMadMrg;
        }
        i.src = {Operand::reg(mw::ra(w)), b, c};
        i.aux = flags;
        i.aux2 = static_cast<uint32_t>(mode == 4 ? ir::XMadMode::CBcc : static_cast<ir::XMadMode>(mode));
        out.push_back(i);
        return true;
    }
    case mw::Op::I2I: {
        const uint32_t dt = field(w, 8, 2) | (field(w, 12, 1) << 2), st = field(w, 10, 2) | (field(w, 13, 1) << 2);
        if ((dt & 3) == 3 || (st & 3) == 3) return unsupported("invalid I2I type");
        if ((dt & 3) != 2) return unsupported("I2I to a sub-32-bit type");
        if (bit(w, 50)) return unsupported("I2I.SAT");
        Inst i = make(Opcode::I2I, addr, w);
        i.dst = {mw::rd(w)};
        i.src = {fmod(operand_b(in, false), bit(w, 45), bit(w, 49))};
        i.aux = st;
        i.aux2 = dt;
        i.aux3 = field(w, 41, 2);
        i.cc = bit(w, 47);
        out.push_back(i);
        return true;
    }
    case mw::Op::BFE: {
        if (!no_cc(47)) return false;
        if (bit(w, 40)) return unsupported("BFE.BREV");
        Inst i = make(Opcode::Bfe, addr, w);
        i.dst = {mw::rd(w)};
        i.src = {Operand::reg(mw::ra(w)), operand_b(in, false)};
        i.aux = bit(w, 48);
        out.push_back(i);
        return true;
    }
    case mw::Op::R2P: {
        if (bit(w, 40)) return unsupported("R2P.CC");
        Inst i = make(Opcode::R2P, addr, w);
        const Operand mask = operand_b(in, false);
        for (uint32_t p = 0; p < 7; ++p)
            if (mask.kind != Kind::Imm || (mask.imm >> p) & 1) i.pdst.push_back(p);
        i.src = {Operand::reg(mw::ra(w)), mask};
        i.aux = 8 * field(w, 41, 2);
        out.push_back(i);
        return true;
    }
    case mw::Op::CSETP: {
        const uint32_t cond = field(w, 8, 5);
        if (cond > 15) return unsupported("CSETP with an integer condition-code test");
        if (cond == 7 || cond == 8) return unsupported("CSETP .NUM/.NAN");
        Inst i = make(Opcode::CSetP, addr, w);
        i.pdst = {field(w, 3, 3), field(w, 0, 3)};
        i.src = {Operand::pred(field(w, 39, 3), bit(w, 42))};
        i.aux = cond;
        i.aux2 = field(w, 45, 2);
        if (i.aux2 == 3) return unsupported("invalid boolean op");
        out.push_back(i);
        return true;
    }
    case mw::Op::PSET: {
        if (!no_cc(47)) return false;
        Inst i = make(bit(w, 44) ? Opcode::PSetBool : Opcode::PSetMask, addr, w);
        i.dst = {mw::rd(w)};
        i.src = {Operand::pred(field(w, 12, 3), bit(w, 15)), Operand::pred(field(w, 29, 3), bit(w, 32)),
                 Operand::pred(field(w, 39, 3), bit(w, 42))};
        i.aux = field(w, 24, 2);
        i.aux2 = field(w, 45, 2);
        if (i.aux == 3 || i.aux2 == 3) return unsupported("invalid boolean op");
        out.push_back(i);
        return true;
    }
    case mw::Op::IMNMX: {
        if (!no_cc(47)) return false;
        if (field(w, 43, 2) != 0) return unsupported("IMNMX.XLO/XMED/XHI");
        Inst i = make(bit(w, 48) ? Opcode::IMin : Opcode::UMin, addr, w);
        i.dst = {mw::rd(w)};
        i.src = {Operand::reg(mw::ra(w)), operand_b(in, false), Operand::pred(field(w, 39, 3), bit(w, 42))};
        out.push_back(i);
        return true;
    }
    case mw::Op::SEL: {
        Inst i = make(Opcode::Select, addr, w);
        i.dst = {mw::rd(w)};
        i.src = {Operand::reg(mw::ra(w)), operand_b(in, false), Operand::pred(field(w, 39, 3), bit(w, 42))};
        out.push_back(i);
        return true;
    }
    case mw::Op::MOV:
    case mw::Op::MOV32I: {
        const uint32_t lanes = in.op == mw::Op::MOV32I ? field(w, 12, 4) : field(w, 39, 4);
        if (lanes != 0xf) return unsupported("MOV with partial quad-lane mask");
        Inst i = make(Opcode::Mov, addr, w);
        i.dst = {mw::rd(w)};
        i.src = {operand_b(in, false)};
        out.push_back(i);
        return true;
    }
    case mw::Op::PSETP: {
        Inst i = make(Opcode::PSetP, addr, w);
        i.pdst = {field(w, 3, 3), field(w, 0, 3)};
        i.src = {Operand::pred(field(w, 12, 3), bit(w, 15)), Operand::pred(field(w, 29, 3), bit(w, 32)),
                 Operand::pred(field(w, 39, 3), bit(w, 42))};
        i.aux = field(w, 24, 2);
        i.aux2 = field(w, 45, 2);
        if (i.aux == 3 || i.aux2 == 3) return unsupported("invalid boolean op");
        out.push_back(i);
        return true;
    }
    case mw::Op::ALD:
    case mw::Op::AST: {
        const bool load = in.op == mw::Op::ALD;
        if (mw::ra(w) != kRZ) return unsupported("indexed attribute access");
        if (bit(w, 31)) return unsupported("patch attributes");
        if (mw::rc(w) != kRZ) return unsupported("attribute vertex index (geometry/tessellation)");
        const uint32_t n = field(w, 47, 2) + 1;
        const uint32_t a = field(w, 20, 10);
        if (a % 4 != 0) return unsupported("unaligned attribute address");
        Inst i = make(load ? Opcode::LoadAttr : Opcode::StoreAttr, addr, w);
        i.aux = a;
        i.aux2 = n;
        i.aux3 = load && bit(w, 32);
        for (uint32_t k = 0; k < n; ++k) {
            const uint32_t r = mw::rd(w) == kRZ ? kRZ : mw::rd(w) + k;
            if (load)
                i.dst.push_back(r);
            else
                i.src.push_back(Operand::reg(r));
        }
        out.push_back(i);
        return true;
    }
    case mw::Op::IPA: {
        if (bit(w, 38)) return unsupported("IPA.IDX");
        if (mw::rc(w) != kRZ) return unsupported("IPA sample offset");
        if (field(w, 52, 2) != 0) return unsupported("IPA centroid/offset location");
        if (field(w, 47, 3) != kPT) return unsupported("IPA predicate output");
        Inst i = make(Opcode::Interp, addr, w);
        i.dst = {mw::rd(w)};
        i.src = {Operand::reg(mw::rb(w))};
        i.aux = field(w, 28, 10);
        i.aux2 = field(w, 54, 2);
        i.sat = bit(w, 51);
        out.push_back(i);
        return true;
    }
    case mw::Op::LDC: {
        if (field(w, 44, 2) != 0) return unsupported("LDC .IL/.IS/.ISL addressing");
        const uint32_t sz = field(w, 48, 3);
        if (sz != 4 && sz != 5) return unsupported("LDC with sub-word size");
        Inst i = make(Opcode::LoadConst, addr, w);
        Operand c{Kind::CBuf, field(w, 36, 5)};
        c.imm = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(field(w, 20, 16))));
        c.base = mw::ra(w);
        i.src = {c};
        i.aux2 = sz == 4 ? 1 : 2;
        for (uint32_t k = 0; k < i.aux2; ++k) i.dst.push_back(mw::rd(w) == kRZ ? kRZ : mw::rd(w) + k);
        out.push_back(i);
        return true;
    }
    case mw::Op::TEXS: {
        const uint32_t t = field(w, 53, 4);
        if (t != 1 && t != 2 && t != 3 && t != 4 && t != 7 && t != 8 && t != 10 && t != 11)
            return unsupported("TEXS variant other than 2D/2D.LZ/2D.LL/2D.DC/A2D/A2D.LZ/3D/3D.LZ");
        Inst i = make(Opcode::TexSample, addr, w);
        i.aux = field(w, 36, 13);
        i.aux2 = static_cast<uint32_t>(t == 1 || t == 4 || t == 7 || t == 10 ? LodMode::Auto
                                       : t == 2 || t == 8 || t == 11        ? LodMode::Zero
                                                                            : LodMode::Lod);
        i.depth_compare = t == 4;
        const uint32_t ra = mw::ra(w), rb = mw::rb(w);
        // 2D.LL: (u, v) in Ra, Ra+1 and the LOD in Rb; 2D.DC: the depth reference in Rb.
        // A2D: the layer (integer) in Ra, u in Ra+1, v in Rb (as Ryujinx; matches
        // the F2I.U16 layer and the IPA'd coordinates feeding it in a real shader).
        // 3D: (u, v) in Ra, Ra+1 and w in Rb (sources pack into Ra, Ra+1, Rb like TLDS).
        if (t == 7 || t == 8) {
            i.tex_array = true;
            i.src = {Operand::reg(ra == kRZ ? kRZ : ra + 1), Operand::reg(rb), Operand::reg(ra)};
        } else if (t == 10 || t == 11) {
            i.tex_dim = 3;
            i.src = {Operand::reg(ra), Operand::reg(ra == kRZ ? kRZ : ra + 1), Operand::reg(rb)};
        } else if (t == 3 || t == 4)
            i.src = {Operand::reg(ra), Operand::reg(ra == kRZ ? kRZ : ra + 1), Operand::reg(rb)};
        else
            i.src = {Operand::reg(ra), Operand::reg(rb)};
        if (!texs_dests(w, i)) return unsupported("invalid TEXS component mask");
        out.push_back(i);
        return true;
    }
    case mw::Op::TEX: {
        // Ra, Ra+1 = (u, v); Rb, Rb+1, ... = [LOD or bias] [packed offsets (.AOFFI)] [depth reference (.DC)].
        const uint32_t lod = field(w, 55, 3);
        if (lod != 0 && lod != 1 && lod != 2 && lod != 3) return unsupported("TEX LOD mode with array LOD (.LBA/.LLA)");
        if (bit(w, 28)) return unsupported("TEX array");
        const uint32_t dim = field(w, 29, 2);
        if (dim != 1 && dim != 2) return unsupported("TEX dimension other than 2D/3D");
        if (dim == 2 && (bit(w, 54) || bit(w, 50))) return unsupported("TEX 3D with .AOFFI or .DC");
        Inst i = make(Opcode::TexSample, addr, w);
        i.tex_dim = dim == 2 ? 3 : 2;
        i.aux = field(w, 36, 13);
        static constexpr LodMode kLod[] = {LodMode::Auto, LodMode::Zero, LodMode::Bias, LodMode::Lod};
        i.aux2 = static_cast<uint32_t>(kLod[lod]);
        const uint32_t ra = mw::ra(w);
        uint32_t rb = mw::rb(w);
        auto next_b = [&] {
            const Operand o = Operand::reg(rb);
            if (rb != kRZ) ++rb;
            return o;
        };
        i.src = {Operand::reg(ra), Operand::reg(ra == kRZ ? kRZ : ra + 1)};
        if (i.tex_dim == 3) i.src.push_back(Operand::reg(ra == kRZ ? kRZ : ra + 2));  // w in Ra+2
        if (lod >= 2) i.src.push_back(next_b());
        if (bit(w, 54)) i.tex_offset = next_b();
        if (bit(w, 50)) {
            i.depth_compare = true;
            i.src.push_back(next_b());
        }
        i.aux3 = field(w, 31, 4);
        uint32_t n = 0;
        for (uint32_t c = 0; c < 4; ++c)
            if (i.aux3 & (1u << c)) i.dst.push_back(mw::rd(w) == kRZ ? kRZ : mw::rd(w) + n++);
        out.push_back(i);
        return true;
    }
    case mw::Op::TLDS: {
        // Scalar texel fetch (integer coordinates). Source registers by variant (nouveau's
        // scalar layout: sources pack into Ra, Ra+1 and Rb, Rb+1):
        //   2 = 2D.LZ: x = Ra, y = Rb          5 = 2D.LL: x, y = Ra, Ra+1; lod = Rb
        //   4 = 2D.LZ.AOFFI: x, y = Ra, Ra+1; offsets = Rb
        //  12 = 2D.LL.AOFFI: x, y = Ra, Ra+1; lod = Rb, offsets = Rb+1
        //   7 = 3D.LZ: x, y = Ra, Ra+1; z = Rb (UNVERIFIED register order, by analogy)
        const uint32_t t = field(w, 53, 4);
        if (t != 2 && t != 4 && t != 5 && t != 7 && t != 12)
            return unsupported("TLDS variant other than 2D (LZ/LL/AOFFI) or 3D.LZ");
        Inst i = make(Opcode::TexFetch, addr, w);
        i.aux = field(w, 36, 13);
        const uint32_t ra = mw::ra(w), rb = mw::rb(w);
        const Operand ra1 = Operand::reg(ra == kRZ ? kRZ : ra + 1), rb1 = Operand::reg(rb == kRZ ? kRZ : rb + 1);
        switch (t) {
        case 2: i.src = {Operand::reg(ra), Operand::reg(rb), Operand::imm32(0)}; break;
        case 4: i.src = {Operand::reg(ra), ra1, Operand::imm32(0)}; i.tex_offset = Operand::reg(rb); break;
        case 5: i.src = {Operand::reg(ra), ra1, Operand::reg(rb)}; break;
        case 7: i.tex_dim = 3; i.src = {Operand::reg(ra), ra1, Operand::reg(rb), Operand::imm32(0)}; break;
        default: i.src = {Operand::reg(ra), ra1, Operand::reg(rb)}; i.tex_offset = rb1; break;
        }
        if (!texs_dests(w, i)) return unsupported("invalid TLDS component mask");
        out.push_back(i);
        return true;
    }
    case mw::Op::TXQ: {
        if (field(w, 22, 6) != 1) return unsupported("TXQ query other than DIMENSION");
        Inst i = make(Opcode::TexQuery, addr, w);
        i.aux = field(w, 36, 13);
        i.aux3 = field(w, 31, 4);
        i.src = {Operand::reg(mw::ra(w))};
        uint32_t n = 0;
        for (uint32_t c = 0; c < 4; ++c)
            if (i.aux3 & (1u << c)) i.dst.push_back(mw::rd(w) == kRZ ? kRZ : mw::rd(w) + n++);
        out.push_back(i);
        return true;
    }
    case mw::Op::TMML: {
        if (bit(w, 28) || field(w, 29, 2) != 1) return unsupported("TMML other than 2D");
        Inst i = make(Opcode::TexLod, addr, w);
        i.aux = field(w, 36, 13);
        i.aux3 = field(w, 31, 4);
        const uint32_t ra = mw::ra(w);
        i.src = {Operand::reg(ra), Operand::reg(ra == kRZ ? kRZ : ra + 1)};
        uint32_t n = 0;
        for (uint32_t c = 0; c < 4; ++c)
            if (i.aux3 & (1u << c)) i.dst.push_back(mw::rd(w) == kRZ ? kRZ : mw::rd(w) + n++);
        out.push_back(i);
        return true;
    }
    case mw::Op::TLD4: {
        // Texture gather. Sources as for TEX (Mesa NAK, nak_nir_lower_tex.c, SM50): Ra.. = [array
        // layer (integer)], u, v; Rb.. = [offsets: one register (.AOFFI) or two (.PTP)], [depth
        // reference (.DC)]. Offsets are 8 bits per component here (NVIDIA allows -32..31).
        if (bit(w, 35)) return unsupported("TLD4.NDV");
        const uint32_t dim = field(w, 29, 2);
        if (dim != 1) return unsupported("TLD4 dimension other than 2D");
        const uint32_t toff = field(w, 54, 2);
        if (toff == 3) return unsupported("invalid TLD4 offset mode");
        Inst i = make(Opcode::TexGather, addr, w);
        i.aux = field(w, 36, 13);
        i.aux2 = field(w, 56, 2);
        i.tex_array = bit(w, 28);
        i.depth_compare = bit(w, 50);
        uint32_t ra = mw::ra(w), rb = mw::rb(w);
        auto next = [](uint32_t& r) {
            const Operand o = Operand::reg(r);
            if (r != kRZ) ++r;
            return o;
        };
        Operand layer;
        if (i.tex_array) layer = next(ra);
        i.src.push_back(next(ra));
        i.src.push_back(next(ra));
        if (i.tex_array) i.src.push_back(layer);
        if (toff != 0) i.tex_offset = next(rb);
        if (toff == 2) {
            i.tex_ptp = true;
            i.tex_offset2 = next(rb);
        }
        if (i.depth_compare) i.src.push_back(next(rb));
        i.aux3 = field(w, 31, 4);
        uint32_t n = 0;
        for (uint32_t c = 0; c < 4; ++c)
            if (i.aux3 & (1u << c)) i.dst.push_back(mw::rd(w) == kRZ ? kRZ : mw::rd(w) + n++);
        out.push_back(i);
        return true;
    }
    case mw::Op::S2R: {
        const uint32_t sr = field(w, 20, 8);
        switch (sr) {
        case kSrLaneId: case kSrTid: case kSrTidX: case kSrTidY: case kSrTidZ:
        case kSrCtaIdX: case kSrCtaIdY: case kSrCtaIdZ: break;
        default: return unsupported("S2R of an unsupported system register");
        }
        Inst i = make(Opcode::SysReg, addr, w);
        i.dst = {mw::rd(w)};
        i.aux = sr;
        out.push_back(i);
        return true;
    }
    case mw::Op::LDG:
    case mw::Op::STG: {
        const bool load = in.op == mw::Op::LDG;
        const uint32_t sz = field(w, 48, 3);
        if (sz == 7) return unsupported("LDG.U.128");
        Inst i = make(load ? Opcode::LoadGlobal : Opcode::StoreGlobal, addr, w);
        const uint32_t ra = mw::ra(w);
        i.src = {Operand::reg(ra), Operand::reg(bit(w, 45) && ra != kRZ ? ra + 1 : kRZ)};
        i.aux = sz;
        i.aux2 = static_cast<uint32_t>(sext(field(w, 20, 24), 24));
        add_mem_data(i, mw::rd(w), sz, load);
        out.push_back(i);
        return true;
    }
    case mw::Op::LDS:
    case mw::Op::STS:
    case mw::Op::LDL:
    case mw::Op::STL: {
        const bool load = in.op == mw::Op::LDS || in.op == mw::Op::LDL;
        const bool shared = in.op == mw::Op::LDS || in.op == mw::Op::STS;
        if (in.op == mw::Op::LDS && bit(w, 44)) return unsupported("LDS.U");
        const uint32_t sz = field(w, 48, 3);
        if (sz == 7) return unsupported("invalid memory access size");
        Inst i = make(shared ? (load ? Opcode::LoadShared : Opcode::StoreShared)
                             : (load ? Opcode::LoadLocal : Opcode::StoreLocal), addr, w);
        i.src = {Operand::reg(mw::ra(w))};
        i.aux = sz;
        i.aux2 = static_cast<uint32_t>(sext(field(w, 20, 24), 24));
        add_mem_data(i, mw::rd(w), sz, load);
        out.push_back(i);
        return true;
    }
    case mw::Op::ATOM:
    case mw::Op::RED:
    case mw::Op::ATOMS: {
        const bool red = in.op == mw::Op::RED, shared = in.op == mw::Op::ATOMS;
        const uint32_t op = red ? field(w, 23, 3) : field(w, 52, 4);
        const uint32_t type = red ? field(w, 20, 3) : shared ? field(w, 28, 2) : field(w, 49, 3);
        if (op > 8) return unsupported("atomic operation (SAFEADD/unknown)");
        if (type > 1) return unsupported("atomic on a 64-bit, float or 128-bit type");
        Inst i = make(shared ? Opcode::AtomShared : Opcode::AtomGlobal, addr, w);
        i.aux3 = op;
        i.atom_signed = type == 1;
        const uint32_t ra = mw::ra(w);
        if (shared) {
            i.src = {Operand::reg(ra), Operand::reg(mw::rb(w))};
            i.aux2 = static_cast<uint32_t>(sext(field(w, 30, 22), 22) * 4);
        } else {
            i.src = {Operand::reg(ra), Operand::reg(bit(w, 48) && ra != kRZ ? ra + 1 : kRZ),
                     Operand::reg(red ? mw::rd(w) : mw::rb(w))};
            i.aux2 = static_cast<uint32_t>(sext(field(w, 28, 20), 20));
        }
        if (!red) i.dst = {mw::rd(w)};
        out.push_back(i);
        return true;
    }
    case mw::Op::BAR: {
        if (field(w, 32, 3) != 0) return unsupported("BAR other than SYNC");
        if (bit(w, 44) ? field(w, 8, 8) != 0 : mw::ra(w) != kRZ) return unsupported("BAR with a barrier other than 0");
        // A thread count (Rb or immediate; immediate 0 = all threads, which compilers emit
        // for BAR.SYNC 0x0) restricts the barrier to part of the workgroup.
        if (bit(w, 43) ? field(w, 20, 12) != 0 : mw::rb(w) != kRZ) return unsupported("BAR with a thread count");
        if (field(w, 39, 3) != kPT || bit(w, 42)) return unsupported("BAR with a predicate source");
        out.push_back(make(Opcode::Barrier, addr, w));
        return true;
    }
    case mw::Op::MEMBAR: {
        // WGSL has no stand-alone fence and its atomics are relaxed. A MEMBAR right
        // before BAR (the emitted barrier includes storageBarrier) or EXIT needs no
        // code of its own; anywhere else (e.g. publishing data before an atomic flag)
        // it cannot be expressed, so it is rejected instead of silently dropped.
        uint32_t next = addr + 8;
        while (true) {
            if (mw::is_sched_slot(next)) next += 8;
            if (next + 8 > bytes_) break;
            const auto n = mw::decode(word_at(next));
            if (n && n->op == mw::Op::NOP) {
                next += 8;
                continue;
            }
            if (n && (n->op == mw::Op::BAR || n->op == mw::Op::EXIT)) return true;
            break;
        }
        return unsupported("MEMBAR without a following BAR or EXIT (no fence in WGSL)");
    }
    case mw::Op::LEA:
    case mw::Op::LEA_HI: {
        const bool hi = in.op == mw::Op::LEA_HI;
        const bool reg_hi = hi && in.form == mw::Form::Reg;
        const bool x = hi ? bit(w, reg_hi ? 38 : 57) : bit(w, 46);
        const bool neg = hi ? bit(w, reg_hi ? 37 : 56) : bit(w, 45);
        if (neg) return unsupported("LEA.NEG");
        if (field(w, 48, 3) != kPT) return unsupported("LEA predicate output");
        Inst i = make(hi ? Opcode::LeaHi : Opcode::Lea, addr, w);
        i.dst = {mw::rd(w)};
        if (hi) {
            Operand b;
            if (reg_hi) {
                b = Operand::reg(mw::rb(w));
            } else {
                b = Operand{Kind::CBuf, mw::cbuf_bank(w)};
                b.imm = mw::cbuf_offset(w);
            }
            i.src = {Operand::reg(mw::ra(w)), b, Operand::reg(mw::rc(w))};
            i.aux = reg_hi ? field(w, 28, 5) : field(w, 51, 5);
        } else {
            i.src = {Operand::reg(mw::ra(w)), operand_b(in, false)};
            i.aux = field(w, 39, 5);
        }
        i.cc = bit(w, 47);
        i.carry_in = x;
        out.push_back(i);
        return true;
    }
    case mw::Op::SHF_L:
    case mw::Op::SHF_R: {
        if (!no_cc(47)) return false;
        if (bit(w, 49)) return unsupported("SHF.X");
        const uint32_t type = field(w, 37, 2);
        if (type == 1) return unsupported("invalid SHF type");
        Inst i = make(Opcode::Shf, addr, w);
        i.dst = {mw::rd(w)};
        // Immediate shift amounts: the low 6 bits of the immediate field.
        Operand amount = in.form == mw::Form::Reg ? Operand::reg(mw::rb(w)) : Operand::imm32(field(w, 20, 6));
        i.src = {Operand::reg(mw::ra(w)), amount, Operand::reg(mw::rc(w))};
        uint32_t f = 0;
        if (in.op == mw::Op::SHF_R) f |= ir::kShfRight;
        if (in.op == mw::Op::SHF_R && bit(w, 48)) f |= ir::kShfHigh;
        if (in.op == mw::Op::SHF_L && bit(w, 48)) return unsupported("SHF.L.HI");
        if (bit(w, 50)) f |= ir::kShfWrap;
        if (type >= 2) f |= ir::kShf64;
        if (type == 3) f |= ir::kShfSigned;
        i.aux = f;
        out.push_back(i);
        return true;
    }
    case mw::Op::EXIT:
    case mw::Op::KIL: {
        if (field(w, 0, 5) != kTrueCC) return unsupported("condition-code predicated EXIT/KIL");
        out.push_back(make(in.op == mw::Op::EXIT ? Opcode::Exit : Opcode::Kill, addr, w));
        if (in.op == mw::Op::EXIT && always) falls_through = false;
        return true;
    }
    case mw::Op::BRA: {
        if (field(w, 0, 5) != kTrueCC) return unsupported("condition-code predicated BRA");
        Inst i = make(Opcode::Branch, addr, w);
        i.target = branch_target(addr, w);
        out.push_back(i);
        succ.push_back(i.target);
        if (always) falls_through = false;
        return true;
    }
    case mw::Op::SSY:
    case mw::Op::PBK: {
        Inst i = make(in.op == mw::Op::SSY ? Opcode::PushSync : Opcode::PushBreak, addr, w);
        i.guard = kPT;
        i.guard_inv = false;
        i.target = branch_target(addr, w);
        out.push_back(i);
        succ.push_back(i.target);
        return true;
    }
    case mw::Op::SYNC:
    case mw::Op::BRK: {
        if (field(w, 0, 5) != kTrueCC) return unsupported("condition-code predicated SYNC/BRK");
        out.push_back(make(in.op == mw::Op::SYNC ? Opcode::PopSync : Opcode::PopBreak, addr, w));
        if (always) falls_through = false;
        return true;
    }
    case mw::Op::NOP:
    case mw::Op::DEPBAR: return true;  // scheduling only
    case mw::Op::Invalid: break;
    }
    return unsupported("opcode");
}

bool Lifter::run(std::vector<Inst>& out, std::string& error) {
    // Lift only reachable instructions: shaders end in EXIT followed by a
    // BRA-to-self and padding that need not decode.
    std::set<uint32_t> visited;
    std::vector<uint32_t> work{8};
    std::map<uint32_t, std::vector<Inst>> by_addr;
    while (!work.empty()) {
        uint32_t addr = work.back();
        work.pop_back();
        while (true) {
            if (mw::is_sched_slot(addr)) addr += 8;
            if (visited.count(addr)) break;
            if (addr + 8 > bytes_) {
                char buf[64];
                std::snprintf(buf, sizeof buf, "0x%04x: control flow runs past the end of the code", addr);
                error = buf;
                return false;
            }
            visited.insert(addr);
            const uint64_t w = word_at(addr);
            const auto in = mw::decode(w);
            if (!in) {
                fail(addr, w, "unknown or unsupported opcode");
                error = error_;
                return false;
            }
            std::vector<uint32_t> succ;
            bool falls_through = true;
            if (!lift_one(addr, *in, by_addr[addr], succ, falls_through)) {
                error = error_;
                return false;
            }
            for (uint32_t s : succ) {
                if (s % 8 != 0 || mw::is_sched_slot(s)) {
                    fail(addr, w, "branch target is not an instruction slot");
                    error = error_;
                    return false;
                }
                work.push_back(s);
            }
            if (!falls_through) break;
            addr += 8;
        }
    }
    for (auto& [addr, insts] : by_addr)
        for (auto& i : insts) out.push_back(std::move(i));
    return true;
}

}  // namespace

LiftResult lift(const uint8_t* data, size_t size) {
    LiftResult r;
    if (!shader::parse_sph({data, size}, r.program.sph, r.error)) return r;
    const uint8_t* code = data + shader::kSphBytes;
    const size_t bytes = size - shader::kSphBytes;
    r.program.code_bytes = static_cast<uint32_t>(bytes);
    Lifter l(code, bytes);
    r.ok = l.run(r.program.code, r.error);
    return r;
}

LiftResult lift_compute(const uint8_t* data, size_t size) {
    LiftResult r;
    r.program.sph.stage = shader::Stage::Compute;
    r.program.code_bytes = static_cast<uint32_t>(size);
    Lifter l(data, size);
    r.ok = l.run(r.program.code, r.error);
    return r;
}

}  // namespace lgp::gpu::ir
