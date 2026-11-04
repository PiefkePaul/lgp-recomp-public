#include "lgp/gpu/shader/maxwell.h"

#include <array>
#include <cstdio>

namespace lgp::gpu::maxwell {
namespace {

struct Entry {
    uint64_t value;
    uint64_t mask;
    Op op;
    Form form;
};

constexpr uint64_t top(uint32_t v) { return uint64_t{v} << 48; }

// First match wins. Values/masks: top 16 bits from envytools gm107 (tabroot),
// low bits only where the table distinguishes forms (bit 5 of branches).
constexpr std::array kTable{
    Entry{top(0x5c58), top(0xfff8), Op::FADD, Form::Reg},
    Entry{top(0x3858), top(0xfef8), Op::FADD, Form::Imm20},
    Entry{top(0x4c58), top(0xfff8), Op::FADD, Form::CBuf},
    Entry{top(0x0800), top(0xfc00), Op::FADD32I, Form::Imm32},
    Entry{top(0x5c68), top(0xfff8), Op::FMUL, Form::Reg},
    Entry{top(0x3868), top(0xfef8), Op::FMUL, Form::Imm20},
    Entry{top(0x4c68), top(0xfff8), Op::FMUL, Form::CBuf},
    Entry{top(0x1e00), top(0xff00), Op::FMUL32I, Form::Imm32},
    Entry{top(0x5980), top(0xff80), Op::FFMA, Form::Reg},
    Entry{top(0x3280), top(0xfe80), Op::FFMA, Form::Imm20},
    Entry{top(0x4980), top(0xff80), Op::FFMA, Form::CBuf},
    Entry{top(0x5180), top(0xff80), Op::FFMA, Form::RegC},
    Entry{top(0x5c60), top(0xfff8), Op::FMNMX, Form::Reg},
    Entry{top(0x3860), top(0xfef8), Op::FMNMX, Form::Imm20},
    Entry{top(0x4c60), top(0xfff8), Op::FMNMX, Form::CBuf},
    Entry{top(0x5bb0), top(0xfff0), Op::FSETP, Form::Reg},
    Entry{top(0x36b0), top(0xfef0), Op::FSETP, Form::Imm20},
    Entry{top(0x4bb0), top(0xfff0), Op::FSETP, Form::CBuf},
    Entry{top(0x5800), top(0xff00), Op::FSET, Form::Reg},
    Entry{top(0x3000), top(0xfe00), Op::FSET, Form::Imm20},
    Entry{top(0x4900), top(0xff80), Op::Invalid, Form::None},  // DSET c[] shadows FSET c[]
    Entry{top(0x4800), top(0xfe00), Op::FSET, Form::CBuf},
    Entry{top(0x5080), top(0xfff8), Op::MUFU, Form::None},
    Entry{top(0x5c90), top(0xfff8), Op::RRO, Form::Reg},
    Entry{top(0x3890), top(0xfef8), Op::RRO, Form::Imm20},
    Entry{top(0x4c90), top(0xfff8), Op::RRO, Form::CBuf},
    Entry{top(0x5ca8), top(0xfff8), Op::F2F, Form::Reg},
    Entry{top(0x38a8), top(0xfef8), Op::F2F, Form::Imm20},
    Entry{top(0x4ca8), top(0xfff8), Op::F2F, Form::CBuf},
    Entry{top(0x5cb0), top(0xfff8), Op::F2I, Form::Reg},
    Entry{top(0x38b0), top(0xfef8), Op::F2I, Form::Imm20},
    Entry{top(0x4cb0), top(0xfff8), Op::F2I, Form::CBuf},
    Entry{top(0x5cb8), top(0xfff8), Op::I2F, Form::Reg},
    Entry{top(0x38b8), top(0xfef8), Op::I2F, Form::Imm20},
    Entry{top(0x4cb8), top(0xfff8), Op::I2F, Form::CBuf},
    Entry{top(0x5c10), top(0xfff8), Op::IADD, Form::Reg},
    Entry{top(0x3810), top(0xfef8), Op::IADD, Form::Imm20},
    Entry{top(0x4c10), top(0xfff8), Op::IADD, Form::CBuf},
    Entry{top(0x1c00), top(0xfe80), Op::IADD32I, Form::Imm32},
    Entry{top(0x5c18), top(0xfff8), Op::ISCADD, Form::Reg},
    Entry{top(0x3818), top(0xfef8), Op::ISCADD, Form::Imm20},
    Entry{top(0x4c18), top(0xfff8), Op::ISCADD, Form::CBuf},
    Entry{top(0x5c48), top(0xfff8), Op::SHL, Form::Reg},
    Entry{top(0x3848), top(0xfef8), Op::SHL, Form::Imm20},
    Entry{top(0x4c48), top(0xfff8), Op::SHL, Form::CBuf},
    Entry{top(0x5c28), top(0xfff8), Op::SHR, Form::Reg},
    Entry{top(0x3828), top(0xfef8), Op::SHR, Form::Imm20},
    Entry{top(0x4c28), top(0xfff8), Op::SHR, Form::CBuf},
    Entry{top(0x5c40), top(0xfff8), Op::LOP, Form::Reg},
    Entry{top(0x3840), top(0xfef8), Op::LOP, Form::Imm20},
    Entry{top(0x4c40), top(0xfff8), Op::LOP, Form::CBuf},
    Entry{top(0x0400), top(0xfc00), Op::LOP32I, Form::Imm32},
    Entry{top(0x5b60), top(0xfff0), Op::ISETP, Form::Reg},
    Entry{top(0x3660), top(0xfef0), Op::ISETP, Form::Imm20},
    Entry{top(0x4b60), top(0xfff0), Op::ISETP, Form::CBuf},
    Entry{top(0x5c20), top(0xfff8), Op::IMNMX, Form::Reg},
    Entry{top(0x3820), top(0xfef8), Op::IMNMX, Form::Imm20},
    Entry{top(0x4c20), top(0xfff8), Op::IMNMX, Form::CBuf},
    Entry{top(0x5ca0), top(0xfff8), Op::SEL, Form::Reg},
    Entry{top(0x38a0), top(0xfef8), Op::SEL, Form::Imm20},
    Entry{top(0x4ca0), top(0xfff8), Op::SEL, Form::CBuf},
    Entry{top(0x5c98), top(0xfff8), Op::MOV, Form::Reg},
    Entry{top(0x3898), top(0xfef8), Op::MOV, Form::Imm20},
    Entry{top(0x4c98), top(0xfff8), Op::MOV, Form::CBuf},
    Entry{top(0x0100), top(0xfff0), Op::MOV32I, Form::Imm32},
    Entry{top(0x5090), top(0xfff8), Op::PSETP, Form::None},
    Entry{top(0xefd8), top(0xfff8), Op::ALD, Form::None},
    Entry{top(0xeff0), top(0xfff8), Op::AST, Form::None},
    // IPA: plain form requires Ra = RZ (bits 8..15 = 0xff); bit 38 selects .IDX.
    Entry{top(0xe000) | 0xff00, top(0xff00) | (uint64_t{1} << 38) | 0xff00, Op::IPA, Form::None},
    Entry{top(0xe000) | (uint64_t{1} << 38), top(0xff00) | (uint64_t{1} << 38), Op::IPA, Form::None},
    Entry{top(0xef90), top(0xfff8), Op::LDC, Form::None},
    // TEXS: bit 59 is set on SM50/SM52 (on SM60+ a clear bit means .F16).
    Entry{top(0xd800), top(0xfe00), Op::TEXS, Form::None},
    Entry{top(0xc038), top(0xfc38), Op::TEX, Form::None},
    Entry{top(0xe300), top(0xfff0), Op::EXIT, Form::None},
    Entry{top(0xe240), top(0xfff0) | 0x20, Op::BRA, Form::None},
    Entry{top(0xe290), top(0xfff0) | 0x20, Op::SSY, Form::None},
    Entry{top(0xf0f8), top(0xfff8), Op::SYNC, Form::None},
    Entry{top(0xe2a0), top(0xfff0) | 0x20, Op::PBK, Form::None},
    Entry{top(0xe340), top(0xfff0), Op::BRK, Form::None},
    Entry{top(0xe330), top(0xfff0), Op::KIL, Form::None},
    Entry{top(0x5b50), top(0xfff0), Op::ISET, Form::Reg},
    Entry{top(0x3650), top(0xfef0), Op::ISET, Form::Imm20},
    Entry{top(0x4b50), top(0xfff0), Op::ISET, Form::CBuf},
    Entry{top(0x5cc0), top(0xfff0), Op::IADD3, Form::Reg},
    Entry{top(0x38c0), top(0xfef0), Op::IADD3, Form::Imm20},
    Entry{top(0x4cc0), top(0xfff0), Op::IADD3, Form::CBuf},
    Entry{top(0x5ce0), top(0xfff8), Op::I2I, Form::Reg},
    Entry{top(0x38e0), top(0xfef8), Op::I2I, Form::Imm20},
    Entry{top(0x4ce0), top(0xfff8), Op::I2I, Form::CBuf},
    Entry{top(0x5cf0), top(0xfff8), Op::R2P, Form::Reg},
    Entry{top(0x38f0), top(0xfef8), Op::R2P, Form::Imm20},
    Entry{top(0x4cf0), top(0xfff8), Op::R2P, Form::CBuf},
    Entry{top(0x5b00), top(0xffc0), Op::XMAD, Form::Reg},
    Entry{top(0x3600), top(0xfec0), Op::XMAD, Form::Imm16},
    Entry{top(0x4e00), top(0xfe00), Op::XMAD, Form::CBuf},
    Entry{top(0x5100), top(0xff80), Op::XMAD, Form::RegC},
    Entry{top(0x5c00), top(0xfff8), Op::BFE, Form::Reg},
    Entry{top(0x3800), top(0xfef8), Op::BFE, Form::Imm20},
    Entry{top(0x4c00), top(0xfff8), Op::BFE, Form::CBuf},
    Entry{top(0x50a0), top(0xfff8), Op::CSETP, Form::None},
    Entry{top(0x5088), top(0xfff8), Op::PSET, Form::None},
    // P4.3: texture queries/fetches, compute, address arithmetic
    Entry{top(0xda00), top(0xfe00), Op::TLDS, Form::None},  // bit 59 set on SM50/SM52 (like TEXS)
    Entry{top(0xdf48), top(0xfff8), Op::TXQ, Form::None},
    Entry{top(0xdf58), top(0xfff8), Op::TMML, Form::None},
    Entry{top(0xc838), top(0xfc38), Op::TLD4, Form::None},  // texture gather
    Entry{top(0xf0c8), top(0xfff8), Op::S2R, Form::None},
    Entry{top(0xeed0), top(0xfff8), Op::LDG, Form::None},
    Entry{top(0xeed8), top(0xfff8), Op::STG, Form::None},
    Entry{top(0xef48), top(0xfff8), Op::LDS, Form::None},
    Entry{top(0xef58), top(0xfff8), Op::STS, Form::None},
    Entry{top(0xef40), top(0xfff8), Op::LDL, Form::None},
    Entry{top(0xef50), top(0xfff8), Op::STL, Form::None},
    Entry{top(0xed00), top(0xff00), Op::ATOM, Form::None},
    Entry{top(0xebf8), top(0xfff8), Op::RED, Form::None},
    Entry{top(0xec00), top(0xff00), Op::ATOMS, Form::None},
    Entry{top(0xf0a8), top(0xfff8), Op::BAR, Form::None},
    Entry{top(0xef98), top(0xfff8), Op::MEMBAR, Form::None},
    Entry{top(0x5bd0), top(0xfff8), Op::LEA, Form::Reg},
    Entry{top(0x4bd0), top(0xfff0), Op::LEA, Form::CBuf},
    Entry{top(0x36d0), top(0xfef8), Op::LEA, Form::Imm20},
    Entry{top(0x5bd8), top(0xfff8), Op::LEA_HI, Form::Reg},
    Entry{top(0x1800), top(0xfc00), Op::LEA_HI, Form::CBuf},
    Entry{top(0x5bf8), top(0xfff8), Op::SHF_L, Form::Reg},
    Entry{top(0x36f8), top(0xfef8), Op::SHF_L, Form::Imm20},
    Entry{top(0x5cf8), top(0xfff8), Op::SHF_R, Form::Reg},
    Entry{top(0x38f8), top(0xfef8), Op::SHF_R, Form::Imm20},
    Entry{top(0x50b0), top(0xfff8), Op::NOP, Form::None},
    Entry{top(0xf0f0), top(0xfff8), Op::DEPBAR, Form::None},
};

// --- envydis-style printing --------------------------------------------------

class Printer {
public:
    explicit Printer(uint64_t w) : w_(w) {}

    void word(const char* s) {
        if (!out_.empty()) out_ += ' ';
        out_ += s;
    }
    void on(unsigned b, const char* s) {
        if (bit(w_, b)) word(s);
    }
    void hex(uint32_t v) {
        char buf[16];
        std::snprintf(buf, sizeof buf, "0x%x", v);
        word(buf);
    }
    void word64(int64_t v) {
        char buf[24];
        std::snprintf(buf, sizeof buf, "0x%llx", static_cast<unsigned long long>(v));
        word(buf);
    }
    void shex(int64_t v) {
        char buf[24];
        if (v < 0)
            std::snprintf(buf, sizeof buf, "-0x%llx", static_cast<unsigned long long>(-v));
        else
            std::snprintf(buf, sizeof buf, "0x%llx", static_cast<unsigned long long>(v));
        word(buf);
    }
    void reg(uint32_t r) {
        if (r == kRegZero) {
            word("0x0");
            return;
        }
        char buf[8];
        std::snprintf(buf, sizeof buf, "$r%u", r);
        word(buf);
    }
    void pred(uint32_t p) {
        if (p == kPredTrue) {
            word("0x1");
            return;
        }
        char buf[8];
        std::snprintf(buf, sizeof buf, "$p%u", p);
        word(buf);
    }
    // ON(not_bit, not) + predicate register (tabpred12/29/39/47 style)
    void pred_src(unsigned lo, unsigned not_bit) {
        on(not_bit, "not");
        pred(field(w_, lo, 3));
    }
    void guard() {
        const uint32_t g = field(w_, 16, 4);
        if (g == 0x7) return;
        if (g == 0xf) {
            word("never");
            return;
        }
        on(19, "not");
        pred(g & 7);
    }
    // Table lookup on a field; names[i] == nullptr prints nothing.
    template <size_t N>
    void sel(uint32_t v, const std::array<const char*, N>& names) {
        if (v < N && names[v]) word(names[v]);
    }
    // c<bank>[offset] with an optional base register.
    void cmem(uint32_t bank, uint32_t base_reg, int64_t offset) {
        std::string s = "c" + std::to_string(bank) + "[";
        mem_body(s, base_reg, offset);
        word(s.c_str());
    }
    void mem(const char* space, uint32_t base_reg, int64_t offset) {
        std::string s = std::string(space) + "[";
        mem_body(s, base_reg, offset);
        word(s.c_str());
    }
    void amem(uint32_t base_reg, uint32_t offset) {
        std::string s = "a[";
        mem_body(s, base_reg, offset);
        word(s.c_str());
    }
    std::string str() const { return out_; }

private:
    static void mem_body(std::string& s, uint32_t base_reg, int64_t offset) {
        char buf[24];
        if (base_reg != kRegZero) {
            s += "$r" + std::to_string(base_reg);
            if (offset > 0) {
                std::snprintf(buf, sizeof buf, "+0x%llx", static_cast<unsigned long long>(offset));
                s += buf;
            } else if (offset < 0) {
                std::snprintf(buf, sizeof buf, "-0x%llx", static_cast<unsigned long long>(-offset));
                s += buf;
            }
        } else {
            std::snprintf(buf, sizeof buf, "0x%llx", static_cast<unsigned long long>(offset));
            s += buf;
        }
        s += ']';
    }

    uint64_t w_;
    std::string out_;
};

constexpr std::array<const char*, 4> kRnd{nullptr, "rm", "rp", "rz"};
constexpr std::array<const char*, 7> kMemSize{"u8", "s8", "u16", "s16", "b32", "b64", "b128"};
constexpr std::array<const char*, 11> kAtomOp{"add", "min", "max", "inc", "dec", "and", "or", "xor", "exch",
                                              nullptr, "safeadd"};

int64_t sext(uint32_t v, unsigned bits) {
    const int64_t m = int64_t{1} << (bits - 1);
    return (static_cast<int64_t>(v) ^ m) - m;
}

// System register names as envydis prints them (S2R).
std::string sys_reg_name(uint32_t index) {
    static constexpr std::pair<uint32_t, const char*> kNames[] = {
        {0x00, "laneid"}, {0x01, "clock"},  {0x02, "virtcfg"}, {0x03, "virtid"}, {0x10, "prim_type"},
        {0x11, "invocation_id"}, {0x12, "y_direction"}, {0x13, "thread_kill"}, {0x14, "shader_type"},
        {0x20, "tid"},    {0x21, "tidx"},   {0x22, "tidy"},    {0x23, "tidz"},   {0x24, "cta_param"},
        {0x25, "ctaidx"}, {0x26, "ctaidy"}, {0x27, "ctaidz"},  {0x28, "ntid"},   {0x29, "cirqueueincrminusone"},
        {0x2a, "nlatc"},  {0x30, "swinlo"}, {0x31, "swinsz"},  {0x32, "smemsz"}, {0x33, "smembanks"},
        {0x34, "lwinlo"}, {0x35, "lwinsz"}, {0x36, "lmemlosz"}, {0x37, "lmemhioff"}, {0x38, "eqmask"},
        {0x39, "ltmask"}, {0x3a, "lemask"}, {0x3b, "gtmask"},  {0x3c, "gemask"}, {0x3d, "regalloc"},
        {0x50, "clocklo"}, {0x51, "clockhi"}, {0x52, "globaltimerlo"}, {0x53, "globaltimerhi"}};
    for (const auto& [i, n] : kNames)
        if (i == index) return std::string("$") + n;
    char buf[16];
    std::snprintf(buf, sizeof buf, "$s%u", index);
    return buf;
}
constexpr std::array<const char*, 16> kFCmp{"f", "lt", "eq", "le", "gt", "ne", "ge", "num",
                                            "nan", "ltu", "equ", "leu", "gtu", "neu", "geu", "t"};
constexpr std::array<const char*, 8> kICmp{"f", "lt", "eq", "le", "gt", "ne", "ge", "t"};
constexpr std::array<const char*, 4> kBop{"and", "or", "xor", nullptr};
constexpr std::array<const char*, 32> kCC{
    "f", "lt", "eq", "le", "gt", "ne", "ge", "num", "nan", "ltu", "equ", "leu", "gtu", "neu", "geu", nullptr,
    "off", "lo", "sff", "ls", "hi", "sft", "hs", "oft", "csm_ta", "csm_tr", "csm_mx", "fcsm_ta", "fcsm_tr",
    "fcsm_mx", "rle", "rgt"};

// envydis treats the 14-bit c[] offset as signed (display quirk; the hardware
// offset is unsigned, see cbuf_offset()).
int64_t envy_cbuf_offset(uint64_t w) {
    int64_t v = field(w, 20, 14);
    if (v & 0x2000) v -= 0x4000;
    return v * 4;
}

int64_t branch_target(uint64_t w, uint32_t address) {
    return static_cast<int64_t>(address) + 8 + branch_offset(w);
}

// Prints the B operand for the form; float = true uses the f20 encoding.
void print_b(Printer& p, const Instr& in, bool is_float) {
    const uint64_t w = in.raw;
    switch (in.form) {
    case Form::Reg: p.reg(rb(w)); break;
    case Form::CBuf: p.cmem(cbuf_bank(w), kRegZero, envy_cbuf_offset(w)); break;
    case Form::Imm20:
        if (is_float)
            p.hex(imm20_float(w));
        else
            p.shex(static_cast<int32_t>(imm20_int(w)));
        break;
    case Form::Imm32: p.hex(imm32(w)); break;
    case Form::Imm16: p.hex(field(w, 20, 16)); break;
    case Form::RegC:
    case Form::None: break;
    }
}

std::string print(const Instr& in, uint32_t address) {
    const uint64_t w = in.raw;
    Printer p(w);
    const bool has_guard = in.op != Op::SSY && in.op != Op::PBK;
    if (has_guard) p.guard();

    switch (in.op) {
    case Op::FADD:
        p.word("fadd");
        p.on(44, "ftz");
        p.on(50, "sat");
        p.sel(field(w, 39, 2), kRnd);
        p.on(47, "cc");
        p.reg(rd(w));
        p.on(48, "neg");
        p.on(46, "abs");
        p.reg(ra(w));
        p.on(45, "neg");
        p.on(49, "abs");
        print_b(p, in, true);
        break;
    case Op::FADD32I:
        p.word("fadd32i");
        p.on(55, "ftz");
        p.on(52, "cc");
        p.reg(rd(w));
        p.on(56, "neg");
        p.on(54, "abs");
        p.reg(ra(w));
        p.on(53, "neg");
        p.on(57, "abs");
        p.hex(imm32(w));
        break;
    case Op::FMUL:
        p.word("fmul");
        p.sel(field(w, 44, 2), std::array<const char*, 4>{nullptr, "ftz", "fmz", nullptr});
        p.sel(field(w, 41, 3), std::array<const char*, 8>{nullptr, "d2", "d4", "d8", "m8", "m4", "m2", nullptr});
        p.sel(field(w, 39, 2), kRnd);
        p.on(50, "sat");
        p.on(47, "cc");
        p.reg(rd(w));
        p.reg(ra(w));
        p.on(48, "neg");
        print_b(p, in, true);
        break;
    case Op::FMUL32I:
        p.word("fmul32i");
        p.sel(field(w, 53, 2), std::array<const char*, 4>{nullptr, "ftz", "fmz", nullptr});
        p.on(55, "sat");
        p.on(52, "cc");
        p.reg(rd(w));
        p.reg(ra(w));
        p.hex(imm32(w));
        break;
    case Op::FFMA:
        p.word("ffma");
        p.sel(field(w, 53, 2), std::array<const char*, 4>{nullptr, "ftz", "fmz", nullptr});
        p.sel(field(w, 51, 2), kRnd);
        p.on(50, "sat");
        p.on(47, "cc");
        p.reg(rd(w));
        p.reg(ra(w));
        p.on(48, "neg");
        if (in.form == Form::RegC) {
            p.reg(rc(w));
            p.on(49, "neg");
            p.cmem(cbuf_bank(w), kRegZero, envy_cbuf_offset(w));
        } else {
            print_b(p, in, true);
            p.on(49, "neg");
            p.reg(rc(w));
        }
        break;
    case Op::FMNMX:
        p.word("fmnmx");
        p.on(44, "ftz");
        p.on(47, "cc");
        p.reg(rd(w));
        p.on(48, "neg");
        p.on(46, "abs");
        p.reg(ra(w));
        p.on(45, "neg");
        p.on(49, "abs");
        print_b(p, in, true);
        p.pred_src(39, 42);
        break;
    case Op::FSETP:
        p.word("fsetp");
        p.sel(field(w, 48, 4), kFCmp);
        p.on(47, "ftz");
        p.sel(field(w, 45, 2), kBop);
        p.pred(field(w, 3, 3));
        p.pred(field(w, 0, 3));
        p.on(43, "neg");
        p.on(7, "abs");
        p.reg(ra(w));
        p.on(6, "neg");
        p.on(44, "abs");
        print_b(p, in, true);
        p.pred_src(39, 42);
        break;
    case Op::FSET:
        p.word("fset");
        p.on(52, "bf");
        p.sel(field(w, 48, 4), kFCmp);
        p.sel(field(w, 45, 2), kBop);
        p.on(55, "ftz");
        p.on(47, "cc");
        p.reg(rd(w));
        p.on(43, "neg");
        p.on(54, "abs");
        p.reg(ra(w));
        p.on(53, "neg");
        p.on(44, "abs");
        print_b(p, in, true);
        p.pred_src(39, 42);
        break;
    case Op::MUFU:
        p.word("mufu");
        p.sel(field(w, 20, 4), std::array<const char*, 9>{"cos", "sin", "ex2", "lg2", "rcp", "rsq", "rcp64h",
                                                          "rsq64h", "sqrt"});
        p.on(50, "sat");
        p.reg(rd(w));
        p.on(48, "neg");
        p.on(46, "abs");
        p.reg(ra(w));
        break;
    case Op::RRO:
        p.word("rro");
        p.word(bit(w, 39) ? "ex2" : "sincos");
        p.reg(rd(w));
        p.on(45, "neg");
        p.on(49, "abs");
        print_b(p, in, true);
        break;
    case Op::F2F:
        p.word("f2f");
        p.on(44, "ftz");
        p.sel(field(w, 10, 2), std::array<const char*, 4>{nullptr, "f16", "f32", "f64"});
        p.sel(field(w, 8, 2), std::array<const char*, 4>{nullptr, "f16", "f32", "f64"});
        {
            // tab5ca8_0: bits 39, 40, 42 (mask 0x58 << 36)
            const uint32_t m = (field(w, 39, 2)) | (field(w, 42, 1) << 2);
            p.sel(m, std::array<const char*, 8>{nullptr, nullptr, nullptr, "pass", "round", "floor", "ceil",
                                                "trunc"});
        }
        p.on(50, "sat");
        p.on(47, "cc");
        p.reg(rd(w));
        p.on(45, "neg");
        p.on(49, "abs");
        print_b(p, in, true);
        break;
    case Op::F2I:
        p.word("f2i");
        p.on(44, "ftz");
        p.sel(field(w, 8, 2) | (field(w, 12, 1) << 2),
              std::array<const char*, 8>{"u8", "u16", "u32", "u64", "s8", "s16", "s32", "u64"});
        p.sel(field(w, 10, 2), std::array<const char*, 4>{nullptr, "f16", "f32", "f64"});
        p.sel(field(w, 39, 2), std::array<const char*, 4>{nullptr, "floor", "ceil", "trunc"});
        p.on(47, "cc");
        p.reg(rd(w));
        p.on(45, "neg");
        p.on(49, "abs");
        print_b(p, in, true);
        break;
    case Op::I2F:
        p.word("i2f");
        p.sel(field(w, 8, 2), std::array<const char*, 4>{nullptr, "f16", "f32", "f64"});
        p.sel(field(w, 10, 2) | (field(w, 13, 1) << 2),
              std::array<const char*, 8>{"u8", "u16", "u32", "u64", "s8", "s16", "s32", "s64"});
        p.sel(field(w, 39, 2), kRnd);
        p.on(47, "cc");
        p.reg(rd(w));
        p.on(45, "neg");
        p.on(49, "abs");
        p.sel(field(w, 41, 2), std::array<const char*, 4>{nullptr, "b1", "b2", "b3"});
        print_b(p, in, false);
        break;
    case Op::IADD:
        p.word("iadd");
        p.on(50, "sat");
        p.on(43, "x");
        p.on(47, "cc");
        p.reg(rd(w));
        p.on(49, "neg");
        p.reg(ra(w));
        p.on(48, "neg");
        print_b(p, in, false);
        break;
    case Op::IADD32I:
        p.word("iadd32i");
        p.on(54, "sat");
        p.on(53, "x");
        p.on(52, "cc");
        p.reg(rd(w));
        p.on(56, "neg");
        p.reg(ra(w));
        p.shex(static_cast<int32_t>(imm32(w)));
        break;
    case Op::ISCADD:
        p.word("iscadd");
        p.on(47, "cc");
        p.reg(rd(w));
        p.on(49, "neg");
        p.reg(ra(w));
        p.on(48, "neg");
        print_b(p, in, false);
        p.hex(field(w, 39, 5));
        break;
    case Op::SHL:
        p.word("shl");
        p.on(39, "w");
        p.on(43, "x");
        p.on(47, "cc");
        p.reg(rd(w));
        p.reg(ra(w));
        print_b(p, in, false);
        break;
    case Op::SHR:
        p.word("shr");
        if (!bit(w, 48)) p.word("u32");
        p.on(39, "w");
        p.on(44, "x");
        p.on(40, "brev");
        p.on(47, "cc");
        p.reg(rd(w));
        p.reg(ra(w));
        print_b(p, in, false);
        break;
    case Op::LOP:
        p.word("lop");
        p.sel(field(w, 41, 2), std::array<const char*, 4>{"and", "or", "xor", "pass_b"});
        p.on(43, "x");
        p.sel(field(w, 44, 2), std::array<const char*, 4>{nullptr, "t", "z", "nz"});
        p.pred(field(w, 48, 3));
        p.on(47, "cc");
        p.reg(rd(w));
        p.on(39, "inv");
        p.reg(ra(w));
        p.on(40, "inv");
        print_b(p, in, false);
        break;
    case Op::LOP32I:
        p.word("lop32i");
        p.sel(field(w, 53, 2), std::array<const char*, 4>{"and", "or", "xor", "pass_b"});
        p.on(57, "x");
        p.on(52, "cc");
        p.reg(rd(w));
        p.on(55, "inv");
        p.reg(ra(w));
        p.on(56, "inv");
        p.hex(imm32(w));
        break;
    case Op::ISETP:
        p.word("isetp");
        p.sel(field(w, 49, 3), kICmp);
        if (!bit(w, 48)) p.word("u32");
        p.on(43, "x");
        p.sel(field(w, 45, 2), kBop);
        p.pred(field(w, 3, 3));
        p.pred(field(w, 0, 3));
        p.reg(ra(w));
        print_b(p, in, false);
        p.pred_src(39, 42);
        break;
    case Op::ISET:
        p.word("iset");
        p.on(44, "bf");
        p.sel(field(w, 49, 3), kICmp);
        if (!bit(w, 48)) p.word("u32");
        p.on(43, "x");
        p.sel(field(w, 45, 2), kBop);
        p.on(47, "cc");
        p.reg(rd(w));
        p.reg(ra(w));
        print_b(p, in, false);
        p.pred_src(39, 42);
        break;
    case Op::IADD3: {
        const bool r = in.form == Form::Reg;
        constexpr std::array<const char*, 4> kHalf{nullptr, "h0", "h1", nullptr};
        p.word("iadd3");
        if (r) p.sel(field(w, 37, 2), std::array<const char*, 4>{nullptr, "rs", "ls", nullptr});
        p.on(48, "x");
        p.on(47, "cc");
        p.reg(rd(w));
        p.on(51, "neg");
        if (r) p.sel(field(w, 35, 2), kHalf);
        p.reg(ra(w));
        p.on(50, "neg");
        if (r) p.sel(field(w, 33, 2), kHalf);
        print_b(p, in, false);
        p.on(49, "neg");
        if (r) p.sel(field(w, 31, 2), kHalf);
        p.reg(rc(w));
        break;
    }
    case Op::I2I: {
        constexpr std::array<const char*, 8> kTy{"u8", "u16", "u32", nullptr, "s8", "s16", "s32", nullptr};
        p.word("i2i");
        p.sel(field(w, 8, 2) | (field(w, 12, 1) << 2), kTy);
        p.sel(field(w, 10, 2) | (field(w, 13, 1) << 2), kTy);
        p.on(50, "sat");
        p.on(47, "cc");
        p.reg(rd(w));
        p.sel(field(w, 41, 2), std::array<const char*, 4>{nullptr, "b1", "b2", "b3"});
        p.on(45, "neg");
        p.on(49, "abs");
        print_b(p, in, false);
        break;
    }
    case Op::R2P:
        p.word("r2p");
        p.word(bit(w, 40) ? "cc" : "pr");
        p.sel(field(w, 41, 2), std::array<const char*, 4>{nullptr, "b1", "b2", "b3"});
        p.reg(ra(w));
        print_b(p, in, false);
        break;
    case Op::XMAD: {
        p.word("xmad");
        p.sel(field(w, 48, 2), std::array<const char*, 4>{nullptr, "s16 u16", "u16 s16", "s16 s16"});
        constexpr std::array<const char*, 8> kMode{nullptr, "clo", "chi", "csfu", "cbcc", nullptr, nullptr, nullptr};
        if (in.form == Form::Reg || in.form == Form::Imm16) {
            p.on(36, "psl");
            p.on(37, "mrg");
            p.sel(field(w, 50, 3), kMode);
            p.on(38, "x");
            p.on(47, "cc");
            p.reg(rd(w));
            p.on(53, "h1");
            p.reg(ra(w));
            if (in.form == Form::Reg) {
                p.on(35, "h1");
                p.reg(rb(w));
            } else {
                p.hex(field(w, 20, 16));
            }
            p.reg(rc(w));
        } else {
            if (in.form == Form::CBuf) {
                p.on(55, "psl");
                p.on(56, "mrg");
            }
            p.sel(field(w, 50, 2), kMode);
            p.on(54, "x");
            p.on(47, "cc");
            p.reg(rd(w));
            p.on(53, "h1");
            p.reg(ra(w));
            p.on(52, "h1");
            if (in.form == Form::CBuf) {
                p.cmem(cbuf_bank(w), kRegZero, envy_cbuf_offset(w));
                p.reg(rc(w));
            } else {
                p.reg(rc(w));
                p.cmem(cbuf_bank(w), kRegZero, envy_cbuf_offset(w));
            }
        }
        break;
    }
    case Op::BFE:
        p.word("bfe");
        if (!bit(w, 48)) p.word("u32");
        p.on(40, "brev");
        p.on(47, "cc");
        p.reg(rd(w));
        p.reg(ra(w));
        print_b(p, in, false);
        break;
    case Op::CSETP:
        p.word("csetp");
        p.sel(field(w, 8, 5), kCC);
        p.sel(field(w, 45, 2), kBop);
        p.pred(field(w, 3, 3));
        p.pred(field(w, 0, 3));
        p.word("cc");
        p.pred_src(39, 42);
        break;
    case Op::PSET:
        p.word("pset");
        p.on(44, "bf");
        p.sel(field(w, 24, 2), kBop);
        p.sel(field(w, 45, 2), kBop);
        p.on(47, "cc");
        p.reg(rd(w));
        p.pred_src(12, 15);
        p.pred_src(29, 32);
        p.pred_src(39, 42);
        break;
    case Op::IMNMX:
        p.word("imnmx");
        if (!bit(w, 48)) p.word("u32");
        p.sel(field(w, 43, 2), std::array<const char*, 4>{nullptr, "xlo", "xmed", "xhi"});
        p.on(47, "cc");
        p.reg(rd(w));
        p.reg(ra(w));
        print_b(p, in, false);
        p.pred_src(39, 42);
        break;
    case Op::SEL:
        p.word("sel");
        p.reg(rd(w));
        p.reg(ra(w));
        print_b(p, in, false);
        p.pred_src(39, 42);
        break;
    case Op::MOV:
        p.word("mov");
        p.reg(rd(w));
        print_b(p, in, false);
        p.hex(field(w, 39, 4));
        break;
    case Op::MOV32I:
        p.word("mov32i");
        p.reg(rd(w));
        p.hex(imm32(w));
        p.hex(field(w, 12, 4));
        break;
    case Op::PSETP:
        p.word("psetp");
        p.sel(field(w, 24, 2), kBop);
        p.sel(field(w, 45, 2), kBop);
        p.pred(field(w, 3, 3));
        p.pred(field(w, 0, 3));
        p.pred_src(12, 15);
        p.pred_src(29, 32);
        p.pred_src(39, 42);
        break;
    case Op::ALD:
        p.word("ld");
        p.on(32, "o");
        p.on(31, "p");
        p.sel(field(w, 47, 2), std::array<const char*, 4>{"b32", "b64", "b96", "b128"});
        p.reg(rd(w));
        p.amem(ra(w), field(w, 20, 10));
        p.reg(rc(w));
        break;
    case Op::AST:
        p.word("st");
        p.on(31, "p");
        p.sel(field(w, 47, 2), std::array<const char*, 4>{"b32", "b64", "b96", "b128"});
        p.amem(ra(w), field(w, 20, 10));
        p.reg(rd(w));
        p.reg(rc(w));
        break;
    case Op::IPA:
        p.word("ipa");
        if (bit(w, 38)) p.word("idx");
        p.sel(field(w, 54, 2), std::array<const char*, 4>{"pass", nullptr, "constant", "sc"});
        p.sel(field(w, 52, 2), std::array<const char*, 4>{nullptr, "centroid", "offset", nullptr});
        p.on(51, "sat");
        p.reg(rd(w));
        if (bit(w, 38)) {
            std::string s = "a[";
            s += ra(w) == kRegZero ? "0x0" : "$r" + std::to_string(ra(w));
            s += "]";
            p.word(s.c_str());
        } else {
            p.amem(ra(w), field(w, 28, 10));
        }
        p.reg(rb(w));
        p.reg(rc(w));
        if (bit(w, 38))
            p.pred_src(39, 42);
        else
            p.pred_src(47, 50);
        break;
    case Op::LDC:
        p.word("ld");
        p.sel(field(w, 44, 2), std::array<const char*, 4>{nullptr, "il", "is", "isl"});
        p.sel(field(w, 48, 3), std::array<const char*, 8>{"u8", "s8", "u16", "s16", "b32", "b64", nullptr,
                                                          nullptr});
        p.reg(rd(w));
        {
            int32_t off = static_cast<int32_t>(field(w, 20, 16));
            if (off & 0x8000) off -= 0x10000;
            p.cmem(field(w, 36, 5), ra(w), off);
        }
        break;
    case Op::TEXS: {
        p.word("texs");
        const uint32_t t = field(w, 53, 4);
        p.sel(t, std::array<const char*, 14>{"lz", nullptr, "lz", "ll", "dc", "ll dc", "lz dc", nullptr, "lz",
                                             "lz dc", nullptr, "lz", nullptr, "ll"});
        p.on(49, "nodep");
        p.reg(field(w, 28, 8));
        p.reg(rd(w));
        p.reg(ra(w));
        p.reg(rb(w));
        p.hex(field(w, 36, 13));
        p.sel(t, std::array<const char*, 14>{"t1d", "t2d", "t2d", "t2d", "t2d", "t2d", "t2d", "a2d", "a2d",
                                             "a2d", "t3d", "t3d", "tcube", "tcube"});
        const uint32_t m = field(w, 50, 3);
        if (field(w, 28, 8) == kRegZero)
            p.sel(m, std::array<const char*, 8>{"r", "g", "b", "a", "rg", "ra", "ga", "ba"});
        else
            p.sel(m, std::array<const char*, 8>{"rgb", "rga", "rba", "gba", "rgba", nullptr, nullptr, nullptr});
        break;
    }
    case Op::TEX:
        p.word("tex");
        p.sel(field(w, 55, 3), std::array<const char*, 8>{nullptr, "lz", "lb", "ll", nullptr, nullptr, "lba",
                                                          "lla"});
        p.on(54, "aoffi");
        p.on(50, "dc");
        p.on(49, "nodep");
        p.reg(rd(w));
        p.reg(ra(w));
        p.reg(rb(w));
        p.hex(field(w, 36, 13));
        p.on(28, "array");
        p.sel(field(w, 29, 2), std::array<const char*, 4>{"t1d", "t2d", "t3d", "tcube"});
        p.hex(field(w, 31, 4));
        break;
    case Op::TLDS: {
        const uint32_t t = field(w, 53, 4);
        p.word("tlds");
        static constexpr std::array<const char*, 13> kMode{"lz", "ll", "lz", nullptr, "lz aoffi", "ll", "lz mz",
                                                           "lz", "lz", nullptr, nullptr, nullptr, "ll aoffi"};
        p.sel(t, kMode);
        p.on(49, "nodep");
        p.reg(field(w, 28, 8));
        p.reg(rd(w));
        p.reg(ra(w));
        p.reg(rb(w));
        p.hex(field(w, 36, 13));
        p.sel(t, std::array<const char*, 13>{"t1d", "t1d", "t2d", nullptr, "t2d", "t2d", "t2d", "t3d", "a2d", nullptr,
                                             nullptr, nullptr, "t2d"});
        const uint32_t m = field(w, 50, 3);
        if (field(w, 28, 8) == kRegZero)
            p.sel(m, std::array<const char*, 8>{"r", "g", "b", "a", "rg", "ra", "ga", "ba"});
        else
            p.sel(m, std::array<const char*, 8>{"rgb", "rga", "rba", "gba", "rgba", nullptr, nullptr, nullptr});
        break;
    }
    case Op::TXQ: {
        p.word("txq");
        p.on(49, "nodep");
        p.reg(rd(w));
        p.reg(ra(w));
        const uint32_t q = field(w, 22, 6);
        static constexpr std::array<const char*, 0x17> kQuery{
            nullptr,  "dimension", "texture_type", nullptr, nullptr, "sample_pos", nullptr, nullptr,
            nullptr,  nullptr,     nullptr,        nullptr, nullptr, nullptr,      nullptr, nullptr,
            "filter", nullptr,     "lod",          nullptr, "wrap",  nullptr,      "border_color"};
        p.sel(q, kQuery);
        p.hex(field(w, 36, 13));
        p.hex(field(w, 31, 4));
        break;
    }
    case Op::TMML:
        p.word("tmml");
        p.word("lod");
        p.on(35, "ndv");
        p.on(49, "nodep");
        p.reg(rd(w));
        p.reg(ra(w));
        p.hex(field(w, 36, 13));
        p.on(28, "array");
        p.sel(field(w, 29, 2), std::array<const char*, 4>{"t1d", "t2d", "t3d", "tcube"});
        p.hex(field(w, 31, 4));
        break;
    case Op::TLD4:
        p.word("tld4");
        p.sel(field(w, 56, 2), std::array<const char*, 4>{"r", "g", "b", "a"});
        if (field(w, 54, 2) == 3) return "???";
        p.sel(field(w, 54, 2), std::array<const char*, 4>{nullptr, "aoffi", "ptp", nullptr});
        p.on(50, "dc");
        p.on(35, "ndv");
        p.on(49, "nodep");
        p.reg(rd(w));
        p.reg(ra(w));
        p.reg(rb(w));
        p.hex(field(w, 36, 13));
        p.on(28, "array");
        p.sel(field(w, 29, 2), std::array<const char*, 4>{"t1d", "t2d", "t3d", "tcube"});
        p.hex(field(w, 31, 4));
        break;
    case Op::S2R:
        p.word("mov");
        p.reg(rd(w));
        p.word(sys_reg_name(field(w, 20, 8)).c_str());
        break;
    case Op::LDG:
        p.word("ldg");
        p.on(45, "e");
        p.sel(field(w, 46, 2), std::array<const char*, 4>{nullptr, "cg", "ci", "cv"});
        p.sel(field(w, 48, 3), kMemSize);
        p.reg(rd(w));
        p.mem("ncg", ra(w), sext(field(w, 20, 24), 24));
        break;
    case Op::STG:
        p.word("stg");
        p.on(45, "e");
        p.sel(field(w, 46, 2), std::array<const char*, 4>{nullptr, "cg", "cs", "wt"});
        p.sel(field(w, 48, 3), kMemSize);
        p.mem("ncg", ra(w), sext(field(w, 20, 24), 24));
        p.reg(rd(w));
        break;
    case Op::LDS:
        p.word("ld");
        p.on(44, "u");
        p.sel(field(w, 48, 3), kMemSize);
        p.reg(rd(w));
        p.mem("s", ra(w), sext(field(w, 20, 24), 24));
        break;
    case Op::STS:
        p.word("st");
        p.sel(field(w, 48, 3), kMemSize);
        p.mem("s", ra(w), sext(field(w, 20, 24), 24));
        p.reg(rd(w));
        break;
    case Op::LDL:
        p.word("ld");
        p.sel(field(w, 44, 2), std::array<const char*, 4>{nullptr, "lu", "ci", "cv"});
        p.sel(field(w, 48, 3), kMemSize);
        p.reg(rd(w));
        p.mem("l", ra(w), sext(field(w, 20, 24), 24));
        break;
    case Op::STL:
        p.word("st");
        p.sel(field(w, 44, 2), std::array<const char*, 4>{nullptr, "cg", "cs", "wt"});
        p.sel(field(w, 48, 3), kMemSize);
        p.mem("l", ra(w), sext(field(w, 20, 24), 24));
        p.reg(rd(w));
        break;
    case Op::ATOM:
        p.word("atom");
        p.sel(field(w, 52, 4), kAtomOp);
        p.on(48, "e");
        p.sel(field(w, 49, 3), std::array<const char*, 6>{"u32", "s32", "u64", "ftz rn f32", "u128", "s64"});
        p.reg(rd(w));
        p.mem("g", ra(w), sext(field(w, 28, 20), 20));
        p.reg(rb(w));
        break;
    case Op::RED:
        p.word("red");
        p.sel(field(w, 23, 3), kAtomOp);
        p.on(48, "e");
        p.sel(field(w, 20, 3), std::array<const char*, 6>{"u32", "s32", "u64", "ftz rn f32", "u128", "s64"});
        p.mem("g", ra(w), sext(field(w, 28, 20), 20));
        p.reg(rd(w));
        break;
    case Op::ATOMS:
        p.word("atoms");
        p.sel(field(w, 52, 4), kAtomOp);
        p.sel(field(w, 28, 2), std::array<const char*, 4>{"u32", "s32", "u64", "s64"});
        p.reg(rd(w));
        p.mem("s", ra(w), sext(field(w, 30, 22), 22) * 4);
        p.reg(rb(w));
        break;
    case Op::BAR: {
        p.word("bar");
        // envydis' table looks at bits 32, 33, 35, 36 and 39 (bit 39 overlaps the predicate source).
        const uint32_t m = static_cast<uint32_t>((w >> 32) & 0x9b);
        switch (m) {
        case 0x02: p.word("red"); p.word("popc"); break;
        case 0x03: p.word("scan"); break;
        case 0x0a: p.word("red"); p.word("and"); break;
        case 0x12: p.word("red"); p.word("or"); break;
        case 0x80: p.word("sync"); break;
        case 0x81: p.word("arrive"); break;
        default: break;
        }
        if (bit(w, 44))
            p.hex(field(w, 8, 8));
        else
            p.reg(ra(w));
        if (bit(w, 43))
            p.hex(field(w, 20, 12));
        else
            p.reg(rb(w));
        p.pred_src(39, 42);
        break;
    }
    case Op::MEMBAR:
        p.word("membar");
        p.sel(field(w, 8, 2), std::array<const char*, 4>{"cta", "gl", "sys", "vc"});
        p.sel(field(w, 0, 2), std::array<const char*, 4>{nullptr, "ivalld", "ivallt", "ivalltd"});
        break;
    case Op::LEA:
        p.word("lea");
        p.on(46, "x");
        p.pred(field(w, 48, 3));
        p.on(47, "cc");
        p.reg(rd(w));
        p.on(45, "neg");
        p.reg(ra(w));
        print_b(p, in, false);
        p.hex(field(w, 39, 5));
        break;
    case Op::LEA_HI:
        p.word("lea");
        p.word("hi");
        if (in.form == Form::Reg) {
            p.on(38, "x");
            p.pred(field(w, 48, 3));
            p.on(47, "cc");
            p.reg(rd(w));
            p.on(37, "neg");
            p.reg(ra(w));
            p.reg(rb(w));
            p.reg(rc(w));
            p.hex(field(w, 28, 5));
        } else {
            p.on(57, "x");
            p.pred(field(w, 48, 3));
            p.on(47, "cc");
            p.reg(rd(w));
            p.on(56, "neg");
            p.reg(ra(w));
            p.cmem(cbuf_bank(w), kRegZero, envy_cbuf_offset(w));
            p.reg(rc(w));
            p.hex(field(w, 51, 5));
        }
        break;
    case Op::SHF_L:
    case Op::SHF_R:
        p.word("shf");
        p.word(in.op == Op::SHF_L ? "l" : "r");
        p.on(50, "w");
        p.sel(field(w, 37, 2), std::array<const char*, 4>{nullptr, nullptr, "u64", "s64"});
        p.sel(field(w, 48, 2), std::array<const char*, 4>{nullptr, "hi", "x", "xhi"});
        p.on(47, "cc");
        p.reg(rd(w));
        p.reg(ra(w));
        if (in.form == Form::Reg)
            p.reg(rb(w));
        else if (in.op == Op::SHF_R)
            p.hex(field(w, 20, 6));
        else
            p.shex(static_cast<int32_t>(imm20_int(w)));
        p.reg(rc(w));
        break;
    case Op::EXIT:
        p.word("exit");
        p.on(5, "\"keeprefcount\"");  // envydis prints the quotes
        p.sel(field(w, 0, 5), kCC);
        break;
    case Op::BRA:
        p.word("bra");
        p.on(7, "u");
        p.on(6, "lmt");
        p.sel(field(w, 0, 5), kCC);
        p.word64(branch_target(w, address));
        break;
    case Op::SSY:
    case Op::PBK:
        p.word(in.op == Op::SSY ? "ssy" : "pbk");
        p.word64(branch_target(w, address));
        break;
    case Op::SYNC:
    case Op::BRK:
    case Op::KIL:
        p.word(in.op == Op::SYNC ? "sync" : in.op == Op::BRK ? "brk" : "kil");
        p.sel(field(w, 0, 5), kCC);
        break;
    case Op::NOP:
        p.word("nop");
        p.on(13, "trig");
        p.sel(field(w, 8, 5), kCC);
        p.hex(field(w, 20, 16));
        break;
    case Op::DEPBAR:
        p.word("depbar");
        p.on(29, "le");
        p.hex(field(w, 26, 3));
        p.hex(field(w, 20, 6));
        p.hex(field(w, 0, 6));
        break;
    case Op::Invalid: return "???";
    }
    return p.str();
}

}  // namespace

std::optional<Instr> decode(uint64_t word) {
    for (const Entry& e : kTable) {
        if ((word & e.mask) == e.value) {
            if (e.op == Op::Invalid) return std::nullopt;
            return Instr{word, e.op, e.form};
        }
    }
    return std::nullopt;
}

std::string disassemble(const Instr& in, uint32_t address) { return print(in, address); }

}  // namespace lgp::gpu::maxwell
