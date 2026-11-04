#include "lgp/gpu/shader/wgsl.h"

#include <algorithm>
#include <array>
#include <functional>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <optional>
#include <set>
#include <sstream>

namespace lgp::gpu::wgsl {
namespace {

using ir::Inst;
using ir::Kind;
using ir::Opcode;
using ir::Operand;
using shader::PixelImap;
using shader::Stage;

constexpr uint32_t kRZ = 255;
constexpr uint32_t kPT = 7;
constexpr uint32_t kAttrPosition = 0x70;
constexpr uint32_t kAttrGeneric = 0x80;
constexpr uint32_t kAttrGenericEnd = 0x280;
constexpr uint32_t kAttrInstanceId = 0x2f8;
constexpr uint32_t kAttrVertexId = 0x2fc;
constexpr uint32_t kAttrPointSize = 0x6c;

std::string hex(uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "0x%xu", v);
    return buf;
}
std::string u(uint32_t v) { return std::to_string(v) + "u"; }
const char* comp(uint32_t c) {
    static const char* const kNames[] = {"x", "y", "z", "w"};
    return kNames[c & 3];
}

bool is_finite_f32(uint32_t bits) { return ((bits >> 23) & 0xff) != 0xff; }

const char* interp_attr(Interp m) {
    switch (m) {
    case Interp::Linear: return "@interpolate(linear, center)";
    case Interp::Flat: return "@interpolate(flat)";
    default: return "@interpolate(perspective, center)";
    }
}

Interp to_interp(PixelImap m) {
    return m == PixelImap::Perspective    ? Interp::Perspective
           : m == PixelImap::ScreenLinear ? Interp::Linear
                                          : Interp::Flat;
}

// Options::storage_desc_offset: names of the helpers of a statically
// resolved target (id < kStaticUniformId: storage slot id, else uniform
// block id - kStaticUniformId): its constant (the table entry it reads) and
// function prefix, and the marker around them.
std::string static_const(uint32_t id) {
    return id < kStaticUniformId ? "gs" + std::to_string(id) : "gu" + std::to_string(id - kStaticUniformId);
}
std::string static_fn(uint32_t id) {
    return id < kStaticUniformId ? "gm" + std::to_string(id) : "gu" + std::to_string(id - kStaticUniformId);
}
std::string static_marker(uint32_t id, const char* where) {
    return "// static " + (id < kStaticUniformId ? "slot " + std::to_string(id)
                                                 : "block " + std::to_string(id - kStaticUniformId)) +
           " " + where + "\n";
}

// Why a global access searches the slot table (Emitter::gmem_why_).
enum : uint8_t { kWhyUnresolved, kWhy32Bit, kWhyData, kWhyMixed, kWhyMismatch, kWhyWindows };

// The helpers of target id, reading table entry e: binding e (a storage
// slot) or, for e >= kGlobalSlots, constant window e - kGlobalSlots (read
// only: stores are dropped, atomics read 0, as gmem_* do; `vsel`:
// Options::vector_select). Delimited so move_static_slots can replace them.
std::string static_block(uint32_t id, uint32_t e, bool atomics, bool vsel) {
    const std::string c = static_const(id), f = "fn " + static_fn(id);
    std::string b = static_marker(id, "begin") + "const " + c + ": u32 = " + std::to_string(e) + "u;\n";
    if (e < kGlobalSlots) {
        const std::string gm = "gm" + std::to_string(e);
        b += f + "_ld(s: vec2<u32>, k: u32) -> u32 {\n"
             "    if (s.x != " + c + ") { return 0u; }\n"
             "    return atomicLoad(&" + gm + "[(s.y >> 2u) + k]);\n}\n";
        b += f + "_st(s: vec2<u32>, k: u32, v: u32) {\n"
             "    if (s.x == " + c + ") { atomicStore(&" + gm + "[(s.y >> 2u) + k], v); }\n}\n";
        // Sub-word store: clear the bits of mask, then set v (both atomic, see gmem_st_sub).
        b += f + "_st_sub(s: vec2<u32>, mask: u32, v: u32) {\n"
             "    if (s.x == " + c + ") {\n"
             "        atomicAnd(&" + gm + "[s.y >> 2u], ~mask);\n"
             "        atomicOr(&" + gm + "[s.y >> 2u], v);\n    }\n}\n";
        if (atomics)
            b += f + "_atom(s: vec2<u32>, op: u32, v: u32) -> u32 {\n"
                 "    if (s.x != " + c + ") { return 0u; }\n"
                 "    return atom_" + gm + "(s.y >> 2u, op, v);\n}\n";
    } else {
        const std::string gmc = "gmc" + std::to_string(e - kGlobalSlots);
        b += f + "_ld(s: vec2<u32>, k: u32) -> u32 {\n"
             "    if (s.x != " + c + ") { return 0u; }\n"
             "    let w = (s.y >> 2u) + k;\n"
             "    return " + (vsel ? "vsel(" + gmc + "[(w >> 2u) & 4095u], w)" : gmc + "[(w >> 2u) & 4095u][w & 3u]") +
             ";\n}\n";
        b += f + "_st(s: vec2<u32>, k: u32, v: u32) {}\n";
        b += f + "_st_sub(s: vec2<u32>, mask: u32, v: u32) {}\n";
        if (atomics) b += f + "_atom(s: vec2<u32>, op: u32, v: u32) -> u32 { return 0u; }\n";
    }
    return b + static_marker(id, "end");
}

class Emitter {
public:
    Emitter(const ir::Program& p, const Options& o) : p_(p), opt_(o), stage_(p.sph.stage) {}
    Result run();

private:
    // --- operands ---
    std::string reg_u(uint32_t r) {
        if (r == kRZ) return "0u";
        regs_.insert(r);
        return "r" + std::to_string(r);
    }
    std::string pred(const Operand& o) {
        std::string s;
        if (o.index == kPT)
            s = "true";
        else {
            preds_.insert(o.index);
            s = "p" + std::to_string(o.index);
        }
        return o.inv ? "!" + s : s;
    }
    std::string cbuf(uint32_t bank, uint32_t base_reg, uint32_t offset) {
        banks_.insert(bank);
        const std::string b = "cb" + std::to_string(bank);
        if (base_reg == kRZ) {
            const uint32_t off = offset & 0xffffu;
            return b + "[" + u(off >> 4) + "][" + u((off >> 2) & 3) + "]";
        }
        const std::string a = "(" + reg_u(base_reg) + " + " + hex(offset) + ")";
        if (opt_.vector_select) {
            need_vsel_ = true;
            return "vsel(" + b + "[(" + a + " >> 4u) & 4095u], " + a + " >> 2u)";
        }
        return b + "[(" + a + " >> 4u) & 4095u][(" + a + " >> 2u) & 3u]";
    }
    // Raw 32-bit value of an operand, no modifiers.
    std::string raw(const Operand& o) {
        switch (o.kind) {
        case Kind::Reg: return reg_u(o.index);
        case Kind::Imm: return hex(o.imm);
        case Kind::CBuf: return cbuf(o.index, o.base, o.imm);
        case Kind::Pred: return "select(0u, 0xffffffffu, " + pred(o) + ")";
        case Kind::None: break;
        }
        return "0u";
    }
    std::string f32(const Operand& o) {
        std::string s;
        if (o.kind == Kind::Imm && !is_finite_f32(o.imm)) {
            // A constant bitcast to inf/NaN is a WGSL shader-creation error;
            // go through a private variable so it is evaluated at run time.
            s = "bitcast<f32>(nonconst_u32(" + hex(o.imm) + "))";
            need_nonconst_ = true;
        } else if (o.kind == Kind::Reg && o.index == kRZ) {
            s = "0.0";
        } else {
            s = "bitcast<f32>(" + raw(o) + ")";
        }
        if (o.abs) s = "abs(" + s + ")";
        if (o.neg) s = "(-" + s + ")";
        return s;
    }
    std::string u32v(const Operand& o) {
        std::string s = raw(o);
        if (o.neg) {
            if (o.kind == Kind::Imm)
                s = hex(0u - o.imm);
            else
                s = "(0u - " + s + ")";
        }
        if (o.inv) s = "(~" + s + ")";
        return s;
    }
    std::string i32v(const Operand& o) { return "bitcast<i32>(" + u32v(o) + ")"; }

    std::string fcmp(uint32_t op, const std::string& a, const std::string& b) {
        switch (static_cast<ir::FCmp>(op)) {
        case ir::FCmp::F: return "false";
        case ir::FCmp::Lt: return "(" + a + " < " + b + ")";
        case ir::FCmp::Eq: return "(" + a + " == " + b + ")";
        case ir::FCmp::Le: return "(" + a + " <= " + b + ")";
        case ir::FCmp::Gt: return "(" + a + " > " + b + ")";
        case ir::FCmp::Ne: return "(" + a + " < " + b + " || " + a + " > " + b + ")";
        case ir::FCmp::Ge: return "(" + a + " >= " + b + ")";
        case ir::FCmp::Num: return "(" + a + " == " + a + " && " + b + " == " + b + ")";
        case ir::FCmp::Nan: return "(" + a + " != " + a + " || " + b + " != " + b + ")";
        case ir::FCmp::Ltu: return "!(" + a + " >= " + b + ")";
        case ir::FCmp::Equ: return "!(" + a + " < " + b + " || " + a + " > " + b + ")";
        case ir::FCmp::Leu: return "!(" + a + " > " + b + ")";
        case ir::FCmp::Gtu: return "!(" + a + " <= " + b + ")";
        case ir::FCmp::Neu: return "(" + a + " != " + b + ")";
        case ir::FCmp::Geu: return "!(" + a + " < " + b + ")";
        case ir::FCmp::T: return "true";
        }
        return "false";
    }
    std::string icmp(uint32_t op, const std::string& a, const std::string& b) {
        static const char* kOps[] = {nullptr, "<", "==", "<=", ">", "!=", ">=", nullptr};
        if (op == 0) return "false";
        if (op == 7) return "true";
        return "(" + a + " " + kOps[op] + " " + b + ")";
    }
    static std::string bop(uint32_t op, const std::string& a, const std::string& b) {
        static const char* kOps[] = {" && ", " || ", " != "};
        return "(" + a + kOps[op] + b + ")";
    }

    // --- statements ---
    void line(const std::string& s) { body_ << std::string(indent_ * 4, ' ') << s << "\n"; }
    bool fail(const Inst& i, const std::string& what) {
        if (error_.empty()) {
            char buf[32];
            std::snprintf(buf, sizeof buf, "0x%04x: ", i.address);
            error_ = buf + what;
        }
        return false;
    }
    // Assigns values to destination registers (values computed first, so
    // overlapping sources and destinations behave like the hardware).
    void assign(const std::vector<uint32_t>& dst, const std::vector<std::string>& vals) {
        if (dst.size() == 1) {
            if (dst[0] != kRZ) line(reg_u(dst[0]) + " = " + vals[0] + ";");
            return;
        }
        line("{");
        ++indent_;
        for (size_t k = 0; k < dst.size(); ++k)
            if (dst[k] != kRZ) line("let t" + std::to_string(k) + " = " + vals[k] + ";");
        for (size_t k = 0; k < dst.size(); ++k)
            if (dst[k] != kRZ) line(reg_u(dst[k]) + " = t" + std::to_string(k) + ";");
        --indent_;
        line("}");
    }
    void fassign(const Inst& i, std::string v) {
        if (i.sat) v = "clamp(" + v + ", 0.0, 1.0)";
        assign(i.dst, {"bitcast<u32>(" + v + ")"});
    }
    bool emit_inst(const Inst& i);
    bool emit_interp(const Inst& i);
    bool emit_attr(const Inst& i);
    std::string swizzled(const std::string& texel, int k);
    bool emit_tex(const Inst& i);
    bool emit_fetch(const Inst& i);
    bool emit_query(const Inst& i);
    bool emit_lod(const Inst& i);
    bool emit_gather(const Inst& i);
    bool emit_mem(const Inst& i);
    bool emit_atom(const Inst& i);
    bool emit_sysreg(const Inst& i);
    void emit_add(const Inst& i);
    void emit_lea(const Inst& i);
    void emit_shf(const Inst& i);
    // Writes dst = a + b + c (all u32 expressions) and, with Inst::cc, the flags.
    void add_with_flags(const Inst& i, const std::string& a, const std::string& b, const std::string& c);
    // Texture/sampler pair for a handle; -1 (with error) if the handle was used with another type.
    // dim: 2 or 3 (texture_3d); 0 = whatever the handle is used as elsewhere (queries).
    // array: 2D array texture.
    int tex_slot(const Inst& i, bool depth, uint32_t dim = 2, bool array = false);
    // Value of register reg right before instruction at, if a MOV of an immediate in the same
    // basic block sets it (AOFFI offsets are usually materialized that way).
    std::optional<uint32_t> const_reg(const Inst& at, uint32_t reg) const;
    bool is_term(Opcode op) const;
    void emit_xmad(const Inst& i);
    void emit_i2i(const Inst& i);
    // 8/16/32-bit integer from register value v (byte_sel in bytes), sign- or zero-extended to 32 bits.
    std::string int_field(std::string v, uint32_t size, uint32_t byte_sel, bool sgn);
    // LOP with predicate output: write the register and P = true / result == 0 / result != 0.
    void logic_with_pred(const Inst& i, const std::string& v) {
        line("{");
        ++indent_;
        line("let lop_v = " + v + ";");
        assign(i.dst, {"lop_v"});
        static const char* kTest[] = {"true", "true", "lop_v == 0u", "lop_v != 0u"};
        line(pred(Operand::pred(i.pdst[0])) + " = " + kTest[i.pmode & 3] + ";");
        --indent_;
        line("}");
    }
    // Condition-code test (CSETP) on the flags written by IADD/LEA .CC (zero, sign, carry,
    // overflow) and I2I.CC (zero, sign; carry and overflow cleared). Signed "less than" is
    // sign != overflow, as for an integer compare done by subtraction.
    std::string cc_test(uint32_t cond) {
        uses_cc_ = true;
        switch (cond) {
        case 0: return "false";
        case 1: case 9: return "(cc_s != cc_o)";
        case 2: case 10: return "cc_z";
        case 3: case 11: return "(cc_z || cc_s != cc_o)";
        case 4: case 12: return "(!cc_z && cc_s == cc_o)";
        case 5: case 13: return "!cc_z";
        case 6: case 14: return "(cc_s == cc_o)";
        default: return "true";
        }
    }
    std::string exit_return();

    // Module pieces
    std::string generic_in(uint32_t idx) {
        gen_in_.insert(idx);
        return "vin" + std::to_string(idx);
    }
    std::string generic_out(uint32_t idx) {
        gen_out_.insert(idx);
        return "vout" + std::to_string(idx);
    }

    const ir::Program& p_;
    const Options& opt_;
    Stage stage_;
    std::ostringstream body_;
    int indent_ = 1;
    std::string error_;
    std::set<uint32_t> regs_, preds_, banks_, gen_in_, gen_out_;
    std::map<uint32_t, PixelImap> in_mode_;  // fragment: generic index -> interpolation
    std::vector<uint32_t> tex_handles_;
    std::vector<bool> tex_depth_;  // per handle: shadow texture + comparison sampler
    std::vector<uint32_t> tex_dim_;  // per handle: 2 or 3
    std::map<uint32_t, uint32_t> handle_dim_;  // handle -> dimension from its samples/fetches (pre-pass)
    std::vector<bool> tex_array_;  // per handle: 2D array texture
    bool uses_pos_out_ = false, uses_frag_pos_ = false, uses_vertex_id_ = false, uses_instance_id_ = false;
    bool need_nonconst_ = false, implicit_lod_ = false;
    bool need_vsel_ = false;  // Options::vector_select used by cbuf()
    bool swizzle_used_ = false;  // tex_swizzle_apply is called
    bool lod_bias_used_ = false;  // tex_lod_bias is called (Options::sampler_lod_bias)
    bool border_used_ = false;    // tex_border is called (Options::sampler_border)
    bool scale_used_ = false;     // tex_host_xy/tex_guest_dim are called (Options::render_scale)
    bool fpos_xy_used_ = false;   // gl_FragCoord.x or .y is read
    std::string border_word(int k);
    bool has_cf_ = false;
    bool uses_cc_ = false;
    bool need_rz_ = false;
    bool need_mul_rz_ = false;
    bool probe_ = false;  // Options::gmem_probe in effect
    bool uses_gmem_ = false, uses_smem_ = false, uses_lmem_ = false, gmem_atomics_ = false, smem_atomics_ = false;
    bool uses_shf_ = false;
    bool lockstep_ = false;  // compute: BAR in divergent code, emulated with a lockstep loop
    std::map<uint32_t, uint32_t> block_of_;  // leader address -> block id
    std::set<uint32_t> targets_;             // branch / SSY / PBK targets
    uint32_t shared_bytes_ = 0, local_bytes_ = 0;

    // Options::storage_desc_offset: per instruction (index into the code),
    // the slot a global access goes to (dataflow over the registers), or -1.
    // (kStaticUniformId + s: uniform block s), or -1; for -1 why (gmem_why_, kWhy*).
    std::vector<int8_t> gmem_static_;
    std::vector<uint8_t> gmem_why_;
    std::set<uint32_t> static_slots_;   // targets with a resolved access (ids)
    std::vector<uint32_t> static_blocks_;  // uniform blocks with a window, window j = index
    uint32_t gmem_static_count_ = 0, gmem_dynamic_count_ = 0;
    bool gmem_search_ = false;  // an access searches the slot table (gmem_addr)
    void resolve_gmem();
    // Options::static_stacks: SYNC/BRK address -> the target address its pop
    // always takes; empty when the stacks are not static (they stay).
    std::map<uint32_t, uint32_t> pop_target_;
    bool static_stacks_on_ = false;  // resolve_stacks() succeeded and is used
    bool structured_ = false;        // Options::structured took effect
    const char* dispatcher_why_ = "structured off";  // why not, in the "// Control flow:" line
    // Options::structured: blocks of the control flow graph (see structure()).
    struct SBlock {
        size_t begin = 0, end = 0;  // instruction range
        uint32_t taken = 0, fall = 0;  // successors (kSink: end of the shader)
        int succs = 0;              // 0 (unconditional EXIT), 1 (fall), 2 (taken if cond, else fall)
        const Inst* cond = nullptr; // guarded BRA/SYNC/BRK deciding between taken and fall
        uint32_t ipdom = 0;         // immediate post-dominator, back edges left out
        int loop = -1;              // loop this block heads
    };
    struct SLoop {
        uint32_t header = 0, exit = 0;  // exit: kNone when the loop is left only by EXIT
        std::set<uint32_t> body;
    };
    std::vector<SBlock> sblocks_;
    std::vector<SLoop> sloops_;
    std::vector<char> semitted_;
    bool sdry_ = false;
    // swalk recurses once per nesting level; deeper code keeps the dispatcher
    // (small thread stacks under Emscripten).
    int sdepth_ = 0;
    static constexpr int kMaxNesting = 128;
    bool sended_ = false;        // no path of the structured code reaches its end
    bool structure(const std::set<uint32_t>& leaders);
    int swalk(uint32_t b, uint32_t stop, int loop, bool at_header);
    void sline(const std::string& s) {
        if (!sdry_) line(s);
    }
    bool resolve_stacks();
    // Slot of global access i (resolve_gmem), or -1; counts the access.
    int gmem_slot(const Inst& i);
    void emit_gmem_decls(std::ostringstream& m) const;
    // Atomic read-modify-write on word w of the atomic<u32> array arr (op codes: see emit_atom).
    static std::string atomic_fn(const std::string& name, const std::string& arr);
};

bool Emitter::emit_interp(const Inst& i) {
    if (stage_ != Stage::Fragment) return fail(i, "IPA outside a fragment shader");
    const uint32_t a = i.aux;
    std::string v;
    if (a >= kAttrPosition && a < kAttrPosition + 16) {
        uses_frag_pos_ = true;
        fpos_xy_used_ = fpos_xy_used_ || (a - kAttrPosition) / 4 < 2;
        // Hardware interpolates 1/w for a[0x7c]; WebGPU's position.w is 1/w as well.
        v = std::string("fpos.") + comp((a - kAttrPosition) / 4);
    } else if (a >= kAttrGeneric && a < kAttrGenericEnd) {
        const uint32_t idx = (a - kAttrGeneric) / 16, c = (a / 4) % 4;
        PixelImap m = p_.sph.ps_input(idx, c);
        if (m == PixelImap::Unused) return fail(i, "IPA of an attribute the SPH marks unused");
        auto [it, inserted] = in_mode_.emplace(idx, m);
        if (!inserted && it->second != m) return fail(i, "mixed interpolation modes within one vec4 attribute");
        const std::string val = generic_in(idx) + "." + comp(c);
        // IPA.PASS returns the screen-linear value, i.e. attr/w * (1/w)^-1 is
        // not applied: for perspective attributes that is attr * position.w.
        if (m == PixelImap::Perspective) {
            uses_frag_pos_ = true;
            v = "(" + val + " * fpos.w)";
        } else {
            v = val;
        }
    } else {
        return fail(i, "IPA of unsupported attribute " + hex(a));
    }
    switch (i.aux2) {
    case 0: break;                                           // PASS
    case 1: v = "(" + v + " * " + f32(i.src[0]) + ")"; break;  // multiply (perspective correction)
    case 2: break;                                           // CONSTANT: flat value
    default: return fail(i, "IPA.SC");
    }
    fassign(i, v);
    return true;
}

bool Emitter::emit_attr(const Inst& i) {
    const bool load = i.op == Opcode::LoadAttr;
    if (stage_ != Stage::Vertex) return fail(i, "ALD/AST are only supported in vertex shaders");
    std::vector<std::string> vals;
    for (uint32_t k = 0; k < i.aux2; ++k) {
        const uint32_t a = i.aux + 4 * k;
        std::string target;
        if (a >= kAttrPosition && a < kAttrPosition + 16) {
            if (load && !i.aux3) return fail(i, "ALD of position input");
            uses_pos_out_ = true;
            target = std::string("o_pos.") + comp((a - kAttrPosition) / 4);
        } else if (a >= kAttrGeneric && a < kAttrGenericEnd) {
            const uint32_t idx = (a - kAttrGeneric) / 16, c = (a / 4) % 4;
            target = (load && !i.aux3 ? generic_in(idx) : generic_out(idx)) + "." + comp(c);
        } else if (load && a == kAttrVertexId) {
            uses_vertex_id_ = true;
            vals.push_back("vertex_id");
            continue;
        } else if (load && a == kAttrInstanceId) {
            uses_instance_id_ = true;
            vals.push_back("instance_id");
            continue;
        } else if (!load && a == kAttrPointSize) {
            continue;  // WebGPU only has 1-pixel points; point size is dropped
        } else {
            return fail(i, std::string(load ? "ALD" : "AST") + " of unsupported attribute " + hex(a));
        }
        if (load)  // vertex inputs (vinN) hold raw bits already
            vals.push_back(target.rfind("vin", 0) == 0 ? target : "bitcast<u32>(" + target + ")");
        else
            line(target + " = " + f32(i.src[k]) + ";");
    }
    if (load) assign(i.dst, vals);
    return true;
}

int Emitter::tex_slot(const Inst& i, bool depth, uint32_t dim, bool array) {
    if (dim == 0) {
        const auto d = handle_dim_.find(i.aux);
        dim = d != handle_dim_.end() ? d->second : 2;
    }
    auto it = std::find(tex_handles_.begin(), tex_handles_.end(), i.aux);
    const size_t k = static_cast<size_t>(it - tex_handles_.begin());
    if (it == tex_handles_.end()) {
        tex_handles_.push_back(i.aux);
        tex_depth_.push_back(depth);
        tex_dim_.push_back(dim);
        tex_array_.push_back(array);
    } else if (tex_depth_[k] != depth) {
        fail(i, "texture handle used both with and without depth compare");
        return -1;
    } else if (tex_dim_[k] != dim) {
        fail(i, "texture handle used both as 2D and 3D texture");
        return -1;
    } else if (tex_array_[k] != array) {
        fail(i, "texture handle used both as 2D and as 2D array texture");
        return -1;
    }
    if ((depth || array) && dim != 2) {
        fail(i, "depth compare or array on a 3D texture");
        return -1;
    }
    return static_cast<int>(k);
}

std::optional<uint32_t> Emitter::const_reg(const Inst& at, uint32_t reg) const {
    if (reg == kRZ) return 0u;
    size_t idx = static_cast<size_t>(&at - p_.code.data());
    if (targets_.count(at.address)) return std::nullopt;
    while (idx > 0) {
        const Inst& j = p_.code[--idx];
        if (is_term(j.op)) return std::nullopt;
        if (std::find(j.dst.begin(), j.dst.end(), reg) != j.dst.end()) {
            const bool plain = j.guard == kPT && !j.guard_inv && j.pdst.empty();
            if (j.op == Opcode::Mov && plain && j.src.size() == 1 && j.src[0].kind == Kind::Imm && !j.src[0].neg &&
                !j.src[0].inv)
                return j.src[0].imm;
            return std::nullopt;
        }
        if (targets_.count(j.address)) return std::nullopt;
    }
    return std::nullopt;
}

// Packed .AOFFI texel offsets: 4-bit signed x in bits 0..3, y in bits 4..7.
int32_t aoffi(uint32_t v, unsigned c) { return static_cast<int32_t>(((v >> (4 * c)) & 0xf) ^ 8) - 8; }

// A colour texel of binding k through the texture view swizzle (Options::texture_swizzle).
std::string Emitter::swizzled(const std::string& texel, int k) {
    if (!opt_.texture_swizzle) return texel;
    swizzle_used_ = true;
    return "tex_swizzle_apply(" + texel + ", tex_swizzle[" + std::to_string(k / 4) + "u][" + std::to_string(k % 4) +
           "u])";
}

// The swizzle word of binding k when compare lookups apply the sampler border
// (Options::sampler_border), "" when off.
std::string Emitter::border_word(int k) {
    if (!opt_.sampler_border || !opt_.texture_swizzle) return "";
    border_used_ = true;
    return "tex_swizzle[" + std::to_string(k / 4) + "u][" + std::to_string(k % 4) + "u]";
}

bool Emitter::emit_tex(const Inst& i) {
    if (i.tex_array && (i.depth_compare || i.tex_offset.kind == Kind::Reg))
        return fail(i, "2D array texture with depth compare or offsets");
    const int k = tex_slot(i, i.depth_compare, i.tex_dim, i.tex_array);
    if (k < 0) return false;
    const std::string t = "tex" + std::to_string(k), s = "smp" + std::to_string(k);
    const bool d3 = i.tex_dim == 3;
    std::string uv = d3 ? "vec3<f32>(" + f32(i.src[0]) + ", " + f32(i.src[1]) + ", " + f32(i.src[2]) + ")"
                        : "vec2<f32>(" + f32(i.src[0]) + ", " + f32(i.src[1]) + ")";
    // WGSL takes the array layer right after the coordinates.
    if (i.tex_array) uv += ", i32(" + u32v(i.src[2]) + ")";
    // Sources after the coordinates (LOD or bias).
    const size_t extra = d3 || i.tex_array ? 3 : 2;
    const auto lod = static_cast<ir::LodMode>(i.aux2);
    const bool implicit = stage_ == Stage::Fragment;
    // The sampler's LOD bias (Options::sampler_lod_bias), "" when off.
    std::string sbias;
    if (opt_.sampler_lod_bias && opt_.texture_swizzle) {
        sbias = "tex_lod_bias(tex_swizzle[" + std::to_string(k / 4) + "u][" + std::to_string(k % 4) + "u])";
        lod_bias_used_ = true;
    }
    auto plus_bias = [&](const std::string& level) { return sbias.empty() ? level : level + " + " + sbias; };
    // Texel offsets (.AOFFI). WGSL wants a constant expression: use the value when a MOV of an
    // immediate in the same block sets the register (the usual compiler output). Otherwise shift
    // the coordinates by offset / size of level 0, which is exact when level 0 is sampled.
    std::string off;
    // Coordinates the border test uses: the sampled position including the texel offset.
    std::string buv = "vec2<f32>(" + f32(i.src[0]) + ", " + f32(i.src[1]) + ")";
    if (i.tex_offset.kind == Kind::Reg && !d3) {
        if (const auto c = const_reg(i, i.tex_offset.index)) {
            if (*c & 0xff) {
                off = ", vec2<i32>(" + std::to_string(aoffi(*c, 0)) + ", " + std::to_string(aoffi(*c, 1)) + ")";
                buv += " + vec2<f32>(" + std::to_string(aoffi(*c, 0)) + ".0, " + std::to_string(aoffi(*c, 1)) +
                       ".0) / vec2<f32>(textureDimensions(" + t + ", 0))";
            }
        } else {
            const std::string o = "bitcast<i32>(" + reg_u(i.tex_offset.index) + ")";
            line("let toff = vec2<f32>(vec2<i32>(extractBits(" + o + ", 0u, 4u), extractBits(" + o + ", 4u, 4u)));");
            line("let tuv = " + uv + " + toff / vec2<f32>(textureDimensions(" + t + ", 0));");
            uv = "tuv";
            buv = "tuv";
        }
    }
    std::string call;
    if (i.depth_compare && opt_.compare_const) {
        line("var texel_one = vec4<f32>(1.0);");  // var: see emit_gather
        line("let texel = texel_one;");
    } else if (i.depth_compare) {
        // Shadow lookup: the comparison function comes from the sampler state at run time.
        // WGSL has no explicit-LOD or bias form other than level 0.
        std::string dref = f32(i.src.back());
        if (const std::string w = border_word(k); !w.empty()) dref = "tex_compare_ref(" + dref + ", " + w + ")";
        if (implicit && (lod == ir::LodMode::Auto || lod == ir::LodMode::Bias)) {
            call = "textureSampleCompare(" + t + ", " + s + ", " + uv + ", " + dref + off + ")";
            implicit_lod_ = true;
        } else {
            call = "textureSampleCompareLevel(" + t + ", " + s + ", " + uv + ", " + dref + off + ")";
        }
        // Outside [0, 1] on a border axis the result is the comparison with the border depth.
        if (const std::string w = border_word(k); !w.empty() && !d3)
            call = "tex_border(" + call + ", " + buv + ", " + w + ")";
        line("let texel = vec4<f32>(" + call + ");");
    } else {
        switch (lod) {
        case ir::LodMode::Auto:
            if (implicit && !sbias.empty()) {
                call = "textureSampleBias(" + t + ", " + s + ", " + uv + ", " + sbias + off + ")";
                implicit_lod_ = true;
            } else if (implicit) {
                call = "textureSample(" + t + ", " + s + ", " + uv + off + ")";
                implicit_lod_ = true;
            } else {
                call = "textureSampleLevel(" + t + ", " + s + ", " + uv + ", " + plus_bias("0.0") + off + ")";
            }
            break;
        case ir::LodMode::Zero:
            call = "textureSampleLevel(" + t + ", " + s + ", " + uv + ", " + plus_bias("0.0") + off + ")";
            break;
        case ir::LodMode::Lod:
            call = "textureSampleLevel(" + t + ", " + s + ", " + uv + ", " + plus_bias(f32(i.src[extra])) + off + ")";
            break;
        case ir::LodMode::Bias:
            // Outside fragment shaders the implicit LOD is 0, so the bias is the level.
            if (implicit) {
                call = "textureSampleBias(" + t + ", " + s + ", " + uv + ", " + plus_bias(f32(i.src[extra])) + off +
                       ")";
                implicit_lod_ = true;
            } else {
                call = "textureSampleLevel(" + t + ", " + s + ", " + uv + ", " + plus_bias(f32(i.src[extra])) + off +
                       ")";
            }
            break;
        }
        std::string texel = swizzled(call, k);
        // Outside [0, 1] on a border axis the lookup returns the border colour.
        if (const std::string w = border_word(k); !w.empty() && !d3)
            texel = "tex_border_color(" + texel + ", " + buv + ", " + w + ")";
        line("let texel = " + texel + ";");
    }
    std::vector<std::string> vals;
    for (uint32_t c = 0; c < 4; ++c)
        if (i.aux3 & (1u << c)) vals.push_back(std::string("bitcast<u32>(texel.") + comp(c) + ")");
    assign(i.dst, vals);
    return true;
}

// TLD4: gather one component of the 2x2 footprint (texture_2d, texture_2d_array or, with
// .DC, the depth comparison results). Result order as in WGSL/Vulkan: (i0,j1), (i1,j1),
// (i1,j0), (i0,j0). Offsets are 8-bit fields; WGSL only takes constant offsets in -8..7,
// otherwise the coordinates are shifted by offset / size (gathers read level 0, so this is
// exact up to float rounding at texel boundaries). .PTP gives each texel its own offset: texel
// j is the (i0,j0) texel of the footprint at the coordinates + offset j (GL/SPIR-V
// ConstOffsets semantics), i.e. component w of four gathers.
bool Emitter::emit_gather(const Inst& i) {
    const int k = tex_slot(i, i.depth_compare, 2, i.tex_array);
    if (k < 0) return false;
    const std::string t = "tex" + std::to_string(k), s = "smp" + std::to_string(k);
    const std::string layer = i.tex_array ? ", i32(" + u32v(i.src[2]) + ")" : "";
    line("let tg_uv = vec2<f32>(" + f32(i.src[0]) + ", " + f32(i.src[1]) + ");");
    // One gather at the coordinates shifted by the offset in bits lo.. of a register (empty
    // reg = no offset), the result in variable `name`.
    auto gather_at = [&](const std::string& name, const Operand* reg, unsigned lo) {
        std::string uv = "tg_uv", off, buv = "tg_uv";  // buv: position for the border test
        if (reg != nullptr && reg->kind == Kind::Reg) {
            if (const auto c = const_reg(i, reg->index)) {
                const int32_t x = static_cast<int8_t>((*c >> lo) & 0xff), y = static_cast<int8_t>((*c >> (lo + 8)) & 0xff);
                if (x >= -8 && x <= 7 && y >= -8 && y <= 7) {
                    if (x != 0 || y != 0) {
                        off = ", vec2<i32>(" + std::to_string(x) + ", " + std::to_string(y) + ")";
                        buv = "tg_uv + vec2<f32>(" + std::to_string(x) + ".0, " + std::to_string(y) +
                              ".0) / vec2<f32>(textureDimensions(" + t + ", 0))";
                    }
                } else {
                    uv = "tg_uv + vec2<f32>(" + std::to_string(x) + ".0, " + std::to_string(y) + ".0) / vec2<f32>(textureDimensions(" +
                         t + ", 0))";
                }
            } else {
                const std::string o = "bitcast<i32>(" + reg_u(reg->index) + ")";
                uv = "tg_uv + vec2<f32>(vec2<i32>(extractBits(" + o + ", " + std::to_string(lo) + "u, 8u), extractBits(" + o +
                     ", " + std::to_string(lo + 8) + "u, 8u))) / vec2<f32>(textureDimensions(" + t + ", 0))";
            }
        }
        if (uv != "tg_uv") {
            line("let " + name + "_uv = " + uv + ";");
            uv = name + "_uv";
            buv = uv;
        }
        if (i.depth_compare && opt_.compare_const) {
            // Through a var: naga 29 rejects a let of a constant expression in
            // a nested block after override processing (ExpressionAlreadyInScope).
            line("var " + name + "_one = vec4<f32>(1.0);");
            line("let " + name + " = " + name + "_one;");
            return;
        }
        if (i.depth_compare) {
            std::string dref = f32(i.src.back());
            if (const std::string w = border_word(k); !w.empty()) dref = "tex_compare_ref(" + dref + ", " + w + ")";
            std::string g = "textureGatherCompare(" + t + ", " + s + ", " + uv + layer + ", " + dref + off + ")";
            if (const std::string w = border_word(k); !w.empty())
                g = "tex_border4(" + g + ", " + buv + ", " + w + ")";
            line("let " + name + " = " + g + ";");
            return;
        }
        auto call = [&](uint32_t c) {
            return "textureGather(" + std::to_string(c) + ", " + t + ", " + s + ", " + uv + layer + off + ")";
        };
        if (!opt_.texture_swizzle) {
            line("let " + name + " = " + call(i.aux2) + ";");
            return;
        }
        // The view swizzle picks the gathered component (WGSL wants it as a constant).
        swizzle_used_ = true;
        line("var " + name + " = vec4<f32>(0.0);");
        line("switch ((tex_swizzle[" + std::to_string(k / 4) + "u][" + std::to_string(k % 4) + "u] >> " +
             std::to_string(3 * i.aux2) + "u) & 7u) {");
        ++indent_;
        line("case " + std::to_string(kSwizzleOne) + "u: { " + name + " = vec4<f32>(1.0); }");
        for (uint32_t c = 0; c < 4; ++c)
            line("case " + std::to_string(kSwizzleR + c) + "u: { " + name + " = " + call(c) + "; }");
        line("default: {}");
        --indent_;
        line("}");
    };
    if (i.tex_ptp) {
        gather_at("tg0", &i.tex_offset, 0);
        gather_at("tg1", &i.tex_offset, 16);
        gather_at("tg2", &i.tex_offset2, 0);
        gather_at("tg3", &i.tex_offset2, 16);
        line("let texel = vec4<f32>(tg0.w, tg1.w, tg2.w, tg3.w);");
    } else {
        gather_at("texel", &i.tex_offset, 0);
    }
    std::vector<std::string> vals;
    for (uint32_t c = 0; c < 4; ++c)
        if (i.aux3 & (1u << c)) vals.push_back(std::string("bitcast<u32>(texel.") + comp(c) + ")");
    assign(i.dst, vals);
    return true;
}

bool Emitter::emit_fetch(const Inst& i) {
    const int k = tex_slot(i, false, i.tex_dim);
    if (k < 0) return false;
    if (i.tex_dim == 3) {
        const std::string xyz = "vec3<i32>(" + i32v(i.src[0]) + ", " + i32v(i.src[1]) + ", " + i32v(i.src[2]) + ")";
        line("let texel = " +
             swizzled("textureLoad(tex" + std::to_string(k) + ", " + xyz + ", " + i32v(i.src[3]) + ")", k) + ";");
        std::vector<std::string> vals;
        for (uint32_t c = 0; c < 4; ++c)
            if (i.aux3 & (1u << c)) vals.push_back(std::string("bitcast<u32>(texel.") + comp(c) + ")");
        assign(i.dst, vals);
        return true;
    }
    std::string xy = "vec2<i32>(" + i32v(i.src[0]) + ", " + i32v(i.src[1]) + ")";
    if (i.tex_offset.kind == Kind::Reg && i.tex_offset.index != kRZ) {
        const std::string o = "bitcast<i32>(" + reg_u(i.tex_offset.index) + ")";
        xy = "(" + xy + " + vec2<i32>(extractBits(" + o + ", 0u, 4u), extractBits(" + o + ", 4u, 4u)))";
    }
    if (opt_.render_scale != 1.0f && opt_.texture_swizzle) {
        scale_used_ = true;
        xy = "tex_host_xy(" + std::to_string(k) + "u, " + xy + ")";
    }
    line("let texel = " + swizzled("textureLoad(tex" + std::to_string(k) + ", " + xy + ", " + i32v(i.src[2]) + ")", k) +
         ";");
    std::vector<std::string> vals;
    for (uint32_t c = 0; c < 4; ++c)
        if (i.aux3 & (1u << c)) vals.push_back(std::string("bitcast<u32>(texel.") + comp(c) + ")");
    assign(i.dst, vals);
    return true;
}

bool Emitter::emit_query(const Inst& i) {
    // TXQ.DIMENSION: (width, height, depth, mip levels) as integers. A handle not seen before
    // is assumed to be a colour texture.
    auto it = std::find(tex_handles_.begin(), tex_handles_.end(), i.aux);
    const size_t known = static_cast<size_t>(it - tex_handles_.begin());
    const bool seen = it != tex_handles_.end();
    const int k = tex_slot(i, seen && tex_depth_[known], 0, seen && tex_array_[known]);
    if (k < 0) return false;
    const std::string t = "tex" + std::to_string(k);
    if (opt_.render_scale != 1.0f && opt_.texture_swizzle && tex_dim_[size_t(k)] != 3) {
        scale_used_ = true;
        line("let tq = tex_guest_dim(" + std::to_string(k) + "u, textureDimensions(" + t + ", " + u32v(i.src[0]) +
             "));");
    } else {
        line("let tq = textureDimensions(" + t + ", " + u32v(i.src[0]) + ");");
    }
    // Depth of a 2D texture: 1 (UNVERIFIED against hardware, see docs/knowledge/maxwell-nach-wgsl.md).
    const char* const kVals[] = {"tq.x", "tq.y", tex_dim_[size_t(k)] == 3 ? "tq.z" : "1u", nullptr};
    std::vector<std::string> vals;
    for (uint32_t c = 0; c < 4; ++c)
        if (i.aux3 & (1u << c)) vals.push_back(c == 3 ? "textureNumLevels(" + t + ")" : std::string(kVals[c]));
    assign(i.dst, vals);
    return true;
}

bool Emitter::emit_lod(const Inst& i) {
    // TMML: component 0 = LOD relative to the base level (signed 8.8 fixed point), component 1 =
    // the level accessed, clamped to the texture's levels (unsigned 8.8). WGSL has no
    // textureQueryLod: computed from the coordinate derivatives (isotropic, sampler LOD bias and
    // clamps ignored). Without derivatives (vertex/compute) the LOD is 0.
    auto it = std::find(tex_handles_.begin(), tex_handles_.end(), i.aux);
    const size_t known = static_cast<size_t>(it - tex_handles_.begin());
    const bool seen = it != tex_handles_.end();
    const int k = tex_slot(i, seen && tex_depth_[known], seen && tex_array_[known]);
    if (k < 0) return false;
    const std::string t = "tex" + std::to_string(k);
    std::string rel = "0u", abs = "0u";
    if (stage_ == Stage::Fragment) {
        implicit_lod_ = true;
        line("let tl_uv = vec2<f32>(" + f32(i.src[0]) + ", " + f32(i.src[1]) + ");");
        line("let tl_dim = vec2<f32>(textureDimensions(" + t + ", 0));");
        line("let tl_dx = dpdx(tl_uv) * tl_dim;");
        line("let tl_dy = dpdy(tl_uv) * tl_dim;");
        line("let tl_lod = 0.5 * log2(max(dot(tl_dx, tl_dx), dot(tl_dy, tl_dy)));");
        line("let tl_abs = clamp(tl_lod, 0.0, f32(textureNumLevels(" + t + ") - 1u));");
        rel = "bitcast<u32>(i32(clamp(tl_lod, -128.0, 127.99609375) * 256.0))";
        abs = "u32(tl_abs * 256.0)";
    }
    std::vector<std::string> vals;
    for (uint32_t c = 0; c < 4; ++c)
        if (i.aux3 & (1u << c)) vals.push_back(c == 0 ? rel : c == 1 ? abs : "0u");
    assign(i.dst, vals);
    return true;
}

bool Emitter::emit_sysreg(const Inst& i) {
    if (stage_ != Stage::Compute) return fail(i, "S2R outside a compute shader");
    std::string v;
    switch (i.aux) {
    case ir::kSrLaneId: v = "(l_index & 31u)"; break;
    case ir::kSrTid: v = "(l_id.x | (l_id.y << 16u) | (l_id.z << 26u))"; break;
    case ir::kSrTidX: v = "l_id.x"; break;
    case ir::kSrTidY: v = "l_id.y"; break;
    case ir::kSrTidZ: v = "l_id.z"; break;
    case ir::kSrCtaIdX: v = "w_id.x"; break;
    case ir::kSrCtaIdY: v = "w_id.y"; break;
    case ir::kSrCtaIdZ: v = "w_id.z"; break;
    default: return fail(i, "S2R of an unsupported system register");
    }
    assign(i.dst, {v});
    return true;
}

bool Emitter::emit_mem(const Inst& i) {
    const bool global = i.op == Opcode::LoadGlobal || i.op == Opcode::StoreGlobal;
    const bool shared = i.op == Opcode::LoadShared || i.op == Opcode::StoreShared;
    const bool load = i.op == Opcode::LoadGlobal || i.op == Opcode::LoadShared || i.op == Opcode::LoadLocal;
    if (shared && stage_ != Stage::Compute) return fail(i, "shared memory outside a compute shader");
    if (global && stage_ != Stage::Compute && (!opt_.graphics_global_memory || !load))
        return fail(i, load ? "global memory outside a compute shader" : "global memory stores outside a compute shader");
    const auto size = static_cast<ir::MemSize>(i.aux);
    const uint32_t words = size == ir::MemSize::B128 ? 4 : size == ir::MemSize::B64 ? 2 : 1;
    std::string a;  // byte address within the memory
    std::function<std::string(uint32_t)> ld;
    std::function<std::string(uint32_t, const std::string&)> st;
    std::function<std::string(const std::string&, const std::string&)> st_sub;  // (clear mask, value)
    if (global) {
        uses_gmem_ = true;
        const int id = gmem_slot(i);
        const std::string addr = u32v(i.src[0]) + ", " + u32v(i.src[1]) + ", " + hex(i.aux2);
        // gm<k>_*/gu<s>_*: that target only (Options::storage_desc_offset), gmem_*: whichever entry holds it.
        const std::string fn = id >= 0 ? static_fn(uint32_t(id)) + "_" : "gmem_";
        line(id >= 0 ? "let ga = gmem_addr_at(" + addr + ", " + static_const(uint32_t(id)) + ");"
                     : "let ga = gmem_addr(" + addr + ");");
        a = "ga.y";
        ld = [fn](uint32_t k) { return fn + "ld(ga, " + u(k) + ")"; };
        st = [fn](uint32_t k, const std::string& v) { return fn + "st(ga, " + u(k) + ", " + v + ");"; };
        st_sub = [fn](const std::string& m, const std::string& v) { return fn + "st_sub(ga, " + m + ", " + v + ");"; };
    } else if (shared) {
        uses_smem_ = true;
        line("let ma = " + u32v(i.src[0]) + " + " + hex(i.aux2) + ";");
        a = "ma";
        ld = [](uint32_t k) { return "atomicLoad(&smem[(ma >> 2u) + " + u(k) + "])"; };
        st = [](uint32_t k, const std::string& v) { return "atomicStore(&smem[(ma >> 2u) + " + u(k) + "], " + v + ");"; };
        st_sub = [](const std::string& m, const std::string& v) {
            return "atomicAnd(&smem[ma >> 2u], ~" + m + "); atomicOr(&smem[ma >> 2u], " + v + ");";
        };
    } else {
        uses_lmem_ = true;
        line("let ma = " + u32v(i.src[0]) + " + " + hex(i.aux2) + ";");
        a = "ma";
        ld = [](uint32_t k) { return "lmem[(ma >> 2u) + " + u(k) + "]"; };
        st = [](uint32_t k, const std::string& v) { return "lmem[(ma >> 2u) + " + u(k) + "] = " + v + ";"; };
        st_sub = [](const std::string& m, const std::string& v) {
            return "lmem[ma >> 2u] = (lmem[ma >> 2u] & ~" + m + ") | " + v + ";";
        };
    }
    const bool byte = size == ir::MemSize::U8 || size == ir::MemSize::S8;
    const bool half = size == ir::MemSize::U16 || size == ir::MemSize::S16;
    const std::string sh = byte ? "((" + a + " & 3u) * 8u)" : "((" + a + " & 2u) * 8u)";
    if (load) {
        std::vector<std::string> vals;
        if (byte || half) {
            const std::string bits = byte ? "8u" : "16u";
            if (size == ir::MemSize::S8 || size == ir::MemSize::S16)
                vals.push_back("bitcast<u32>(extractBits(bitcast<i32>(" + ld(0) + "), " + sh + ", " + bits + "))");
            else
                vals.push_back("extractBits(" + ld(0) + ", " + sh + ", " + bits + ")");
        } else {
            for (uint32_t k = 0; k < words; ++k) vals.push_back(ld(k));
        }
        assign(i.dst, vals);
        return true;
    }
    const size_t first = global ? 2 : 1;
    if (byte || half) {
        const std::string m = byte ? "0xffu" : "0xffffu";
        line("let msh = " + sh + ";");
        line(st_sub("(" + m + " << msh)", "((" + u32v(i.src[first]) + " & " + m + ") << msh)"));
        return true;
    }
    for (uint32_t k = 0; k < words; ++k) line(st(k, u32v(i.src[first + k])));
    return true;
}

bool Emitter::emit_atom(const Inst& i) {
    if (stage_ != Stage::Compute) return fail(i, "atomics outside a compute shader");
    // Operation codes of the generated helpers: AtomOp, with signed MIN/MAX as 9/10.
    uint32_t op = i.aux3;
    if (i.atom_signed && op == static_cast<uint32_t>(ir::AtomOp::Min)) op = 9;
    if (i.atom_signed && op == static_cast<uint32_t>(ir::AtomOp::Max)) op = 10;
    std::string call;
    if (i.op == Opcode::AtomGlobal) {
        uses_gmem_ = gmem_atomics_ = true;
        const int id = gmem_slot(i);
        const std::string addr = u32v(i.src[0]) + ", " + u32v(i.src[1]) + ", " + hex(i.aux2);
        line(id >= 0 ? "let ga = gmem_addr_at(" + addr + ", " + static_const(uint32_t(id)) + ");"
                     : "let ga = gmem_addr(" + addr + ");");
        call = (id >= 0 ? static_fn(uint32_t(id)) + "_atom(ga, " : "gmem_atom(ga, ") + u(op) + ", " +
               u32v(i.src[2]) + ")";
    } else {
        uses_smem_ = smem_atomics_ = true;
        call = "smem_atom((" + u32v(i.src[0]) + " + " + hex(i.aux2) + ") >> 2u, " + u(op) + ", " + u32v(i.src[1]) + ")";
    }
    if (i.dst.empty() || i.dst[0] == kRZ)
        line("_ = " + call + ";");
    else
        assign(i.dst, {call});
    return true;
}

void Emitter::add_with_flags(const Inst& i, const std::string& a, const std::string& b, const std::string& c) {
    line("let ad_a = " + a + ";");
    line("let ad_b = " + b + ";");
    line("let ad_s1 = ad_a + ad_b;");
    line("let ad_s = ad_s1 + " + c + ";");
    if (i.cc) {
        uses_cc_ = true;
        // Extended (.X) adds chain the zero flag, so a 64-bit result is zero only if both halves are.
        line(std::string("cc_z = ad_s == 0u") + (i.carry_in ? " && cc_z;" : ";"));
        line("cc_s = (ad_s >> 31u) != 0u;");
        line("cc_c = ad_s1 < ad_a || ad_s < ad_s1;");
        line("cc_o = ((~(ad_a ^ ad_b) & (ad_a ^ ad_s)) >> 31u) != 0u;");
    }
    assign(i.dst, {"ad_s"});
}

void Emitter::emit_add(const Inst& i) {
    // With flags or carry in, a negated operand is computed like the hardware: a + ~b + 1.
    Operand a = i.src[0], b = i.src[1];
    std::string c = "0u";
    for (Operand* o : {&a, &b})
        if (o->neg) {
            o->neg = false;
            o->inv = true;
            c = "1u";
        }
    if (i.carry_in) {
        uses_cc_ = true;
        c = "select(0u, 1u, cc_c)";
    }
    line("{");
    ++indent_;
    add_with_flags(i, u32v(a), u32v(b), c);
    --indent_;
    line("}");
}

void Emitter::emit_lea(const Inst& i) {
    std::string shifted;
    if (i.op == Opcode::Lea) {
        shifted = "(" + u32v(i.src[0]) + " << " + u(i.aux) + ")";
    } else {
        // High word of (src[2]:src[0]) << aux.
        shifted = i.aux == 0 ? u32v(i.src[2])
                             : "((" + u32v(i.src[2]) + " << " + u(i.aux) + ") | (" + u32v(i.src[0]) + " >> " + u(32 - i.aux) + "))";
    }
    std::string c = "0u";
    if (i.carry_in) {
        uses_cc_ = true;
        c = "select(0u, 1u, cc_c)";
    }
    line("{");
    ++indent_;
    add_with_flags(i, shifted, u32v(i.src[1]), c);
    --indent_;
    line("}");
}

void Emitter::emit_shf(const Inst& i) {
    // Funnel shift of the 64-bit value (src[2]:src[0]). Without .W the amount saturates at the
    // type width (32 or 64), with .W it wraps (Ryujinx/NAK semantics).
    uses_shf_ = true;
    const uint32_t f = i.aux;
    const bool wide = f & ir::kShf64;
    const std::string amt = u32v(i.src[1]);
    const std::string n = (f & ir::kShfWrap) ? "(" + amt + " & " + u(wide ? 63 : 31) + ")"
                                            : "min(" + amt + ", " + u(wide ? 64 : 32) + ")";
    std::string v;
    if (f & ir::kShfRight)
        v = "shf_r(" + u32v(i.src[0]) + ", " + u32v(i.src[2]) + ", " + n + ", " + ((f & ir::kShfSigned) ? "true" : "false") +
            ", " + ((f & ir::kShfHigh) ? "true" : "false") + ")";
    else
        v = "shf_l(" + u32v(i.src[0]) + ", " + u32v(i.src[2]) + ", " + n + ")";
    assign(i.dst, {v});
}

void Emitter::emit_xmad(const Inst& i) {
    // a and b are 16-bit halves (sign-extended if signed); the product wraps to 32 bits,
    // which is the same for signed and unsigned inputs once they are extended.
    auto half = [&](const std::string& v, bool hi, bool sgn) -> std::string {
        if (sgn) return hi ? "bitcast<u32>(bitcast<i32>(" + v + ") >> 16u)" : "bitcast<u32>(bitcast<i32>(" + v + " << 16u) >> 16u)";
        return hi ? "(" + v + " >> 16u)" : "(" + v + " & 0xffffu)";
    };
    const uint32_t fl = i.aux;
    line("{");
    ++indent_;
    line("let xa = " + raw(i.src[0]) + ";");
    line("let xb = " + raw(i.src[1]) + ";");
    line("let xc = " + raw(i.src[2]) + ";");
    std::string prod = "(" + half("xa", fl & ir::kXMadHiA, fl & ir::kXMadSignA) + " * " +
                       half("xb", fl & ir::kXMadHiB, fl & ir::kXMadSignB) + ")";
    if (fl & ir::kXMadPsl) prod = "(" + prod + " << 16u)";
    std::string c = "xc";
    switch (static_cast<ir::XMadMode>(i.aux2)) {
    case ir::XMadMode::None: break;
    case ir::XMadMode::CLo: c = "(xc & 0xffffu)"; break;
    case ir::XMadMode::CHi: c = "(xc >> 16u)"; break;
    case ir::XMadMode::CBcc: c = "(xc + (xb << 16u))"; break;
    }
    std::string v = "(" + prod + " + " + c + ")";
    if (fl & ir::kXMadMrg) v = "((" + v + " & 0xffffu) | (xb << 16u))";
    assign(i.dst, {v});
    --indent_;
    line("}");
}

std::string Emitter::int_field(std::string v, uint32_t size, uint32_t byte_sel, bool sgn) {
    if (size == 2)
        return v;
    const uint32_t bits = size == 0 ? 8 : 16;
    const std::string field = "(" + v + " >> " + u(byte_sel * 8) + ")";
    if (sgn)
        return "bitcast<u32>(bitcast<i32>(" + field + " << " + u(32 - bits) + ") >> " + u(32 - bits) + ")";
    return "(" + field + " & " + hex((1u << bits) - 1) + ")";
}

void Emitter::emit_i2i(const Inst& i) {
    const uint32_t st = i.aux;
    std::string v = int_field(raw(i.src[0]), st & 3, i.aux3, (st & 4) != 0);
    if (i.src[0].abs) v = "bitcast<u32>(abs(bitcast<i32>(" + v + ")))";
    if (i.src[0].neg) v = "(0u - " + v + ")";
    if (!i.cc) {
        assign(i.dst, {v});
        return;
    }
    uses_cc_ = true;
    line("{");
    ++indent_;
    line("let i2i_v = " + v + ";");
    assign(i.dst, {"i2i_v"});
    line("cc_z = i2i_v == 0u;");
    line("cc_s = (i2i_v >> 31u) != 0u;");
    line("cc_c = false;");
    line("cc_o = false;");
    --indent_;
    line("}");
}

std::string Emitter::exit_return() {
    if (stage_ == Stage::Vertex) return "return finish_vertex();";
    if (stage_ == Stage::Compute) return lockstep_ ? "ls_state = 2u; atomicSub(&ls_running, 1u);" : "return;";
    return "return finish_fragment();";
}

bool Emitter::emit_inst(const Inst& i) {
    const bool guarded = i.guard != kPT || i.guard_inv;
    if (guarded) {
        line("if (" + pred(Operand::pred(i.guard, i.guard_inv)) + ") {");
        ++indent_;
    }
    auto f = [&](size_t k) { return f32(i.src[k]); };
    auto uv = [&](size_t k) { return u32v(i.src[k]); };
    bool ok = true;
    switch (i.op) {
    case Opcode::Mov:
        if (i.pdst.empty())
            assign(i.dst, {uv(0)});
        else
            logic_with_pred(i, uv(0));
        break;
    case Opcode::FMov: fassign(i, f(0)); break;
    case Opcode::FAdd: fassign(i, "(" + f(0) + " + " + f(1) + ")"); break;
    case Opcode::FMul: {
        // Result scale by a power of two (.D2/.D4/.D8/.M8/.M4/.M2).
        static const char* kScale[] = {nullptr, "0.5", "0.25", "0.125", "8.0", "4.0", "2.0"};
        std::string v = "(" + f(0) + " * " + f(1) + ")";
        if (i.aux == 3) {
            need_mul_rz_ = true;
            v = "mul_rz(" + f(0) + ", " + f(1) + ")";
        }
        if (i.scale) v = "(" + v + " * " + kScale[i.scale] + ")";
        fassign(i, v);
        break;
    }
    case Opcode::FFma: fassign(i, "fma(" + f(0) + ", " + f(1) + ", " + f(2) + ")"); break;
    case Opcode::FMin:
        fassign(i, "select(max(" + f(0) + ", " + f(1) + "), min(" + f(0) + ", " + f(1) + "), " + pred(i.src[2]) + ")");
        break;
    case Opcode::FRcp: fassign(i, "(1.0 / " + f(0) + ")"); break;
    case Opcode::FRsq: fassign(i, "inverseSqrt(" + f(0) + ")"); break;
    case Opcode::FSqrt: fassign(i, "sqrt(" + f(0) + ")"); break;
    case Opcode::FSin: fassign(i, "sin(" + f(0) + ")"); break;
    case Opcode::FCos: fassign(i, "cos(" + f(0) + ")"); break;
    case Opcode::FEx2: fassign(i, "exp2(" + f(0) + ")"); break;
    case Opcode::FLg2: fassign(i, "log2(" + f(0) + ")"); break;
    case Opcode::FRound: fassign(i, "round(" + f(0) + ")"); break;
    case Opcode::FFloor: fassign(i, "floor(" + f(0) + ")"); break;
    case Opcode::FCeil: fassign(i, "ceil(" + f(0) + ")"); break;
    case Opcode::FTrunc: fassign(i, "trunc(" + f(0) + ")"); break;
    case Opcode::F2I:
    case Opcode::F2U: {
        static const char* kRnd[] = {"round", "floor", "ceil", "trunc"};
        std::string v = std::string(kRnd[i.aux & 3]) + "(" + f(0) + ")";
        // 8/16-bit results saturate to their range (WGSL's conversion saturates to 32 bits).
        if (i.aux2 == 0 || i.aux2 == 1) {
            const bool s = i.op == Opcode::F2I, b8 = i.aux2 == 0;
            v = "clamp(" + v + ", " + (s ? (b8 ? "-128.0" : "-32768.0") : "0.0") + ", " +
                (s ? (b8 ? "127.0" : "32767.0") : (b8 ? "255.0" : "65535.0")) + ")";
        }
        assign(i.dst, {i.op == Opcode::F2I ? "bitcast<u32>(i32(" + v + "))" : "u32(" + v + ")"});
        break;
    }
    case Opcode::I2F:
    case Opcode::U2F: {
        // Integer operand modifiers: abs first, then negation (two's complement).
        Operand s = i.src[0];
        s.neg = s.abs = false;
        std::string v = int_field(raw(s), i.aux2, i.aux3, i.op == Opcode::I2F);
        if (i.src[0].abs) v = "bitcast<u32>(abs(bitcast<i32>(" + v + ")))";
        if (i.src[0].neg) v = "(0u - " + v + ")";
        const bool rz = i.aux == 3;
        if (rz) need_rz_ = true;
        if (i.op == Opcode::U2F) {
            assign(i.dst, {"bitcast<u32>(f32(" + (rz ? "rz_u32(" + v + ")" : v) + "))"});
        } else if (!rz) {
            assign(i.dst, {"bitcast<u32>(f32(bitcast<i32>(" + v + ")))"});
        } else {
            // Toward zero: convert the magnitude truncated to 24 significant bits, then sign.
            line("{");
            ++indent_;
            line("let i2f_v = bitcast<i32>(" + v + ");");
            line("let i2f_m = f32(rz_u32(bitcast<u32>(abs(i2f_v))));");
            assign(i.dst, {"bitcast<u32>(select(i2f_m, -i2f_m, i2f_v < 0))"});
            --indent_;
            line("}");
        }
        break;
    }
    case Opcode::IAdd:
        if (i.cc || i.carry_in)
            emit_add(i);
        else
            assign(i.dst, {"(" + uv(0) + " + " + uv(1) + ")"});
        break;
    case Opcode::IScAdd: assign(i.dst, {"((" + uv(0) + " << " + u(i.aux) + ") + " + uv(1) + ")"}); break;
    case Opcode::Shl:
    case Opcode::ShrU:
    case Opcode::ShrS: {
        const std::string a = uv(0), b = uv(1);
        std::string v;
        if (i.op == Opcode::ShrS) {
            // Arithmetic shift: amounts >= 32 fill with the sign bit.
            const std::string amt = i.aux ? "(" + b + " & 31u)" : "min(" + b + ", 31u)";
            v = "bitcast<u32>(bitcast<i32>(" + a + ") >> " + amt + ")";
        } else {
            const char* sh = i.op == Opcode::Shl ? " << " : " >> ";
            if (i.aux)
                v = "(" + a + sh + "(" + b + " & 31u))";
            else
                v = "select(" + a + sh + "(" + b + " & 31u), 0u, " + b + " > 31u)";
        }
        assign(i.dst, {v});
        break;
    }
    case Opcode::And:
    case Opcode::Or:
    case Opcode::Xor: {
        const char* op = i.op == Opcode::And ? " & " : i.op == Opcode::Or ? " | " : " ^ ";
        const std::string v = "(" + uv(0) + op + uv(1) + ")";
        if (i.pdst.empty()) {
            assign(i.dst, {v});
        } else {
            logic_with_pred(i, v);
        }
        break;
    }
    case Opcode::IAdd3: assign(i.dst, {"(" + uv(0) + " + " + uv(1) + " + " + uv(2) + ")"}); break;
    case Opcode::XMad: emit_xmad(i); break;
    case Opcode::I2I: emit_i2i(i); break;
    case Opcode::Bfe: {
        const std::string b = uv(1);
        const std::string off = "(" + b + " & 0xffu)", cnt = "((" + b + " >> 8u) & 0xffu)";
        if (i.aux)
            assign(i.dst, {"bitcast<u32>(extractBits(" + i32v(i.src[0]) + ", " + off + ", " + cnt + "))"});
        else
            assign(i.dst, {"extractBits(" + uv(0) + ", " + off + ", " + cnt + ")"});
        break;
    }
    case Opcode::R2P: {
        const std::string a = uv(0), mask = uv(1);
        const bool const_mask = i.src[1].kind == Kind::Imm;
        line("{");
        ++indent_;
        line("let r2p_v = " + a + ";");
        for (uint32_t p : i.pdst) {
            const std::string bitv = "((r2p_v >> " + u(i.aux + p) + ") & 1u) != 0u";
            const std::string P = pred(Operand::pred(p));
            if (const_mask)
                line(P + " = " + bitv + ";");
            else
                line(P + " = select(" + P + ", " + bitv + ", ((" + mask + " >> " + u(p) + ") & 1u) != 0u);");
        }
        --indent_;
        line("}");
        break;
    }
    case Opcode::ISetBool:
    case Opcode::ISetMask: {
        const std::string c = icmp(i.aux, i.aux3 ? i32v(i.src[0]) : uv(0), i.aux3 ? i32v(i.src[1]) : uv(1));
        const std::string v = bop(i.aux2, c, pred(i.src[2]));
        assign(i.dst, {std::string("select(0u, ") + (i.op == Opcode::ISetBool ? "0x3f800000u" : "0xffffffffu") + ", " + v + ")"});
        break;
    }
    case Opcode::PSetBool:
    case Opcode::PSetMask: {
        const std::string v = bop(i.aux2, bop(i.aux, pred(i.src[0]), pred(i.src[1])), pred(i.src[2]));
        assign(i.dst, {std::string("select(0u, ") + (i.op == Opcode::PSetBool ? "0x3f800000u" : "0xffffffffu") + ", " + v + ")"});
        break;
    }
    case Opcode::CSetP: {
        line("{");
        ++indent_;
        line("let c = " + cc_test(i.aux) + ";");
        const std::string acc = pred(i.src[0]);
        if (i.pdst[0] != kPT) line(pred(Operand::pred(i.pdst[0])) + " = " + bop(i.aux2, "c", acc) + ";");
        if (i.pdst[1] != kPT) line(pred(Operand::pred(i.pdst[1])) + " = " + bop(i.aux2, "!c", acc) + ";");
        --indent_;
        line("}");
        break;
    }
    case Opcode::IMin:
        assign(i.dst, {"bitcast<u32>(select(max(" + i32v(i.src[0]) + ", " + i32v(i.src[1]) + "), min(" + i32v(i.src[0]) +
                       ", " + i32v(i.src[1]) + "), " + pred(i.src[2]) + "))"});
        break;
    case Opcode::UMin:
        assign(i.dst, {"select(max(" + uv(0) + ", " + uv(1) + "), min(" + uv(0) + ", " + uv(1) + "), " +
                       pred(i.src[2]) + ")"});
        break;
    case Opcode::Select: assign(i.dst, {"select(" + uv(1) + ", " + uv(0) + ", " + pred(i.src[2]) + ")"}); break;
    case Opcode::FSetBool:
    case Opcode::FSetMask: {
        const std::string c = bop(i.aux2, fcmp(i.aux, f(0), f(1)), pred(i.src[2]));
        assign(i.dst, {i.op == Opcode::FSetBool ? "select(0u, 0x3f800000u, " + c + ")" : "select(0u, 0xffffffffu, " + c + ")"});
        break;
    }
    case Opcode::FSetP:
    case Opcode::ISetP: {
        std::string c;
        if (i.op == Opcode::FSetP)
            c = fcmp(i.aux, f(0), f(1));
        else if (i.aux3)
            c = icmp(i.aux, i32v(i.src[0]), i32v(i.src[1]));
        else
            c = icmp(i.aux, uv(0), uv(1));
        const std::string acc = pred(i.src[2]);
        line("{");
        ++indent_;
        line("let c = " + c + ";");
        if (i.pdst[0] != kPT) line(pred(Operand::pred(i.pdst[0])) + " = " + bop(i.aux2, "c", acc) + ";");
        if (i.pdst[1] != kPT) line(pred(Operand::pred(i.pdst[1])) + " = " + bop(i.aux2, "!c", acc) + ";");
        --indent_;
        line("}");
        break;
    }
    case Opcode::PSetP: {
        line("{");
        ++indent_;
        line("let c = " + bop(i.aux, pred(i.src[0]), pred(i.src[1])) + ";");
        const std::string acc = pred(i.src[2]);
        if (i.pdst[0] != kPT) line(pred(Operand::pred(i.pdst[0])) + " = " + bop(i.aux2, "c", acc) + ";");
        if (i.pdst[1] != kPT) line(pred(Operand::pred(i.pdst[1])) + " = " + bop(i.aux2, "!c", acc) + ";");
        --indent_;
        line("}");
        break;
    }
    case Opcode::LoadAttr:
    case Opcode::StoreAttr: ok = emit_attr(i); break;
    case Opcode::Interp: ok = emit_interp(i); break;
    case Opcode::LoadConst: {
        std::vector<std::string> vals;
        for (uint32_t k = 0; k < i.aux2; ++k)
            vals.push_back(cbuf(i.src[0].index, i.src[0].base, i.src[0].imm + 4 * k));
        assign(i.dst, vals);
        break;
    }
    case Opcode::TexSample:
    case Opcode::TexFetch:
    case Opcode::TexQuery:
    case Opcode::TexLod:
    case Opcode::TexGather:
    case Opcode::LoadGlobal:
    case Opcode::StoreGlobal:
    case Opcode::LoadShared:
    case Opcode::StoreShared:
    case Opcode::LoadLocal:
    case Opcode::StoreLocal:
    case Opcode::AtomGlobal:
    case Opcode::AtomShared:
        line("{");
        ++indent_;
        switch (i.op) {
        case Opcode::TexSample: ok = emit_tex(i); break;
        case Opcode::TexFetch: ok = emit_fetch(i); break;
        case Opcode::TexQuery: ok = emit_query(i); break;
        case Opcode::TexLod: ok = emit_lod(i); break;
        case Opcode::TexGather: ok = emit_gather(i); break;
        case Opcode::AtomGlobal:
        case Opcode::AtomShared: ok = emit_atom(i); break;
        default: ok = emit_mem(i); break;
        }
        --indent_;
        line("}");
        break;
    case Opcode::SysReg: ok = emit_sysreg(i); break;
    case Opcode::Lea:
    case Opcode::LeaHi: emit_lea(i); break;
    case Opcode::Shf: emit_shf(i); break;
    case Opcode::Barrier:
        if (stage_ != Stage::Compute) return fail(i, "BAR outside a compute shader");
        if (lockstep_) {
            // Wait at the barrier; the lockstep loop releases all waiting threads once no
            // thread is running any more.
            line("ls_state = 1u;");
            line("atomicSub(&ls_running, 1u);");
            line("atomicAdd(&ls_waiting, 1u);");
        } else {
            line("storageBarrier();");
            line("workgroupBarrier();");
        }
        break;
    case Opcode::Kill:
        if (stage_ != Stage::Fragment) return fail(i, "KIL outside a fragment shader");
        line("discard;");
        break;
    case Opcode::Exit: line(exit_return()); break;
    case Opcode::Branch: line("pc = " + u(block_of_.at(i.target)) + ";"); break;
    case Opcode::PushSync:
    case Opcode::PushBreak: {
        if (static_stacks_on_)
            break;  // static stacks: the pops know their targets
        const std::string st = i.op == Opcode::PushSync ? "ssy" : "pbk";
        line(st + "_stack[" + st + "_sp & 15u] = " + u(block_of_.at(i.target)) + ";");
        line(st + "_sp = " + st + "_sp + 1u;");
        break;
    }
    case Opcode::PopSync:
    case Opcode::PopBreak: {
        if (static_stacks_on_) {
            line("pc = " + u(block_of_.at(pop_target_.at(i.address))) + ";");
            break;
        }
        const std::string st = i.op == Opcode::PopSync ? "ssy" : "pbk";
        line(st + "_sp = " + st + "_sp - 1u;");
        line("pc = " + st + "_stack[" + st + "_sp & 15u];");
        break;
    }
    case Opcode::FMax:
    case Opcode::IMax:
    case Opcode::UMax: ok = fail(i, "internal: unexpected max opcode"); break;
    }
    if (guarded) {
        --indent_;
        line("}");
    }
    return ok && error_.empty();
}

// Options::storage_desc_offset. Forward dataflow over the code: per register,
// whether it holds the low or high word of storage descriptor k (loaded from
// the descriptor, plus non-pointer values), something else, or a mix. The
// lattice per register: unreached < {other, low(k), high(k)} < conflict.
namespace gmem_df {
using Val = uint8_t;
// Storage descriptor k: kLo + k, kHi + k; uniform block s: kULo + s, kUHi + s.
constexpr Val kBot = 0, kOther = 1, kTop = 2, kLo = 3, kHi = kLo + kGlobalSlots, kULo = kHi + kGlobalSlots,
              kUHi = kULo + kUniformDescCount, kEnd = kUHi + kUniformDescCount;
constexpr uint32_t kRegs = 256;
using State = std::array<Val, kRegs>;
static_assert(kEnd <= 255, "lattice values fit a byte");
inline bool pointer(Val v) { return v >= kTop; }
inline Val join(Val a, Val b) { return a == kBot ? b : b == kBot || a == b ? a : kTop; }
}  // namespace gmem_df

// Forward pass over the instructions with the SSY and PBK stacks as values
// (target addresses). Succeeds when every instruction is reached with one
// stack state, no push is predicated, no pop finds an empty stack and no
// stack grows past the 16 entries the dynamic form keeps; then fills
// pop_target_. The dynamic form behaves the same on such code.
bool Emitter::resolve_stacks() {
    const auto& code = p_.code;
    const size_t n = code.size();
    if (n == 0) return false;
    std::map<uint32_t, size_t> index_of;
    for (size_t k = 0; k < n; ++k) index_of.emplace(code[k].address, k);
    using Stacks = std::array<std::vector<uint32_t>, 2>;  // ssy, pbk
    std::vector<std::optional<Stacks>> in(n);
    std::map<uint32_t, uint32_t> targets;
    in[0] = Stacks{};
    std::vector<size_t> work{0};
    auto flow = [&](size_t t, const Stacks& st) {
        if (!in[t]) {
            in[t] = st;
            work.push_back(t);
            return true;
        }
        return *in[t] == st;
    };
    while (!work.empty()) {
        const size_t k = work.back();
        work.pop_back();
        const Inst& i = code[k];
        Stacks st = *in[k];
        const bool guarded = i.guard != kPT || i.guard_inv;
        const bool next = k + 1 < n;
        bool ok = true;
        switch (i.op) {
        case Opcode::PushSync:
        case Opcode::PushBreak: {
            auto& s = st[i.op == Opcode::PushSync ? 0 : 1];
            if (guarded || !index_of.count(i.target) || s.size() >= 16) return false;
            s.push_back(i.target);
            if (next) ok = flow(k + 1, st);
            break;
        }
        case Opcode::PopSync:
        case Opcode::PopBreak: {
            Stacks popped = st;
            auto& s = popped[i.op == Opcode::PopSync ? 0 : 1];
            if (s.empty()) return false;
            const uint32_t t = s.back();
            s.pop_back();
            targets[i.address] = t;
            ok = flow(index_of.at(t), popped);
            if (ok && guarded && next) ok = flow(k + 1, st);
            break;
        }
        case Opcode::Branch: {
            const auto t = index_of.find(i.target);
            if (t == index_of.end()) return false;
            ok = flow(t->second, st);
            if (ok && guarded && next) ok = flow(k + 1, st);
            break;
        }
        case Opcode::Exit:
            if (guarded && next) ok = flow(k + 1, st);
            break;
        default:
            if (next) ok = flow(k + 1, st);
            break;
        }
        if (!ok) return false;
    }
    // Every pop must have been reached (the emitter needs its target).
    for (const Inst& i : code)
        if ((i.op == Opcode::PopSync || i.op == Opcode::PopBreak) && !targets.count(i.address)) return false;
    pop_target_ = std::move(targets);
    return true;
}

// Options::structured. The blocks (same leaders as the dispatcher) form a
// graph whose back edges (found by a depth-first search) must close natural
// loops with one entry, the header; every loop may be left to one block only
// (or by EXIT). Post-dominators are taken on the graph without back edges,
// with a sink for EXIT and the end of the code. swalk() then emits each
// two-way branch as if/else up to the branch's immediate post-dominator, a
// loop header as loop { }, edges to the innermost header as continue and to
// its exit as break. A block reached twice means the regions overlap (code
// the walk cannot nest); structure() then fails and the dispatcher stays.
namespace {
constexpr uint32_t kNone = 0xffffffffu;
constexpr int kWalkFail = -1, kWalkEnds = 0, kWalkFalls = 1;
}

bool Emitter::structure(const std::set<uint32_t>& leaders) {
    const auto& code = p_.code;
    const uint32_t nb = uint32_t(leaders.size()), kSink = nb;
    sblocks_.assign(nb, {});
    sloops_.clear();
    for (size_t k = 0; k < code.size();) {
        SBlock& b = sblocks_[block_of_.at(code[k].address)];
        b.begin = k;
        size_t e = k + 1;
        while (e < code.size() && !leaders.count(code[e].address) && !is_term(code[e - 1].op)) ++e;
        b.end = e;
        const uint32_t next = e < code.size() ? block_of_.at(code[e].address) : kSink;
        const Inst& t = code[e - 1];
        const bool guarded = t.guard != kPT || t.guard_inv;
        b.fall = next;
        b.succs = 1;
        if (t.op == Opcode::Exit) {
            if (!guarded) b.succs = 0;
        } else if (t.op == Opcode::Branch || t.op == Opcode::PopSync || t.op == Opcode::PopBreak) {
            const uint32_t target = t.op == Opcode::Branch ? t.target : pop_target_.at(t.address);
            b.taken = block_of_.at(target);
            if (!guarded) {
                b.fall = b.taken;
            } else if (b.taken != b.fall) {
                b.succs = 2;
                b.cond = &t;
            }
        }
        k = e;
    }
    auto succs = [&](uint32_t id) {
        std::vector<uint32_t> v;
        const SBlock& b = sblocks_[id];
        if (b.succs == 2) v.push_back(b.taken);
        if (b.succs >= 1) v.push_back(b.fall);
        return v;
    };
    // Depth-first search from the entry: back edges, a reverse post-order.
    std::vector<int> state(nb + 1, 0);  // 0 new, 1 on the stack, 2 done
    std::vector<std::pair<uint32_t, uint32_t>> back;
    std::vector<uint32_t> post;
    std::vector<std::pair<uint32_t, size_t>> stack{{0, 0}};
    state[0] = 1;
    while (!stack.empty()) {
        auto& [n, i] = stack.back();
        const auto s = n == kSink ? std::vector<uint32_t>{} : succs(n);
        if (i < s.size()) {
            const uint32_t m = s[i++];
            if (state[m] == 1) {
                back.emplace_back(n, m);
            } else if (state[m] == 0) {
                state[m] = 1;
                stack.emplace_back(m, 0);
            }
            continue;
        }
        state[n] = 2;
        post.push_back(n);
        stack.pop_back();
    }
    std::set<std::pair<uint32_t, uint32_t>> is_back(back.begin(), back.end());
    std::vector<std::vector<uint32_t>> preds(nb + 1);
    for (uint32_t n = 0; n < nb; ++n)
        if (state[n] == 2)
            for (uint32_t m : succs(n)) preds[m].push_back(n);
    // Natural loops, one per header. Walking back from the latch reaches the
    // entry only past a header that does not dominate it: a second way into
    // the loop (irreducible), left to the dispatcher.
    std::map<uint32_t, std::set<uint32_t>> bodies;
    for (const auto& [from, h] : back) {
        auto& body = bodies[h];
        body.insert(h);
        std::vector<uint32_t> work{from};
        while (!work.empty()) {
            const uint32_t n = work.back();
            work.pop_back();
            if (!body.insert(n).second) continue;
            if (n == 0) return false;
            for (uint32_t p : preds[n]) work.push_back(p);
        }
    }
    for (const auto& [h, body] : bodies) {
        uint32_t exit = kNone;
        for (uint32_t n : body)
            for (uint32_t m : succs(n))
                if (!body.count(m)) {
                    if (exit != kNone && exit != m) return false;
                    exit = m;
                }
        sblocks_[h].loop = int(sloops_.size());
        sloops_.push_back({h, exit, body});
    }
    // Post-dominators without back edges, in post-order (successors first).
    std::vector<std::vector<bool>> pdom(nb + 1);
    pdom[kSink].assign(nb + 1, false);
    pdom[kSink][kSink] = true;
    for (uint32_t n : post) {
        if (n == kSink) continue;
        std::vector<bool> d;
        for (uint32_t m : succs(n)) {
            if (is_back.count({n, m})) continue;
            if (pdom[m].empty()) return false;  // not yet computed: not a DAG
            if (d.empty()) {
                d = pdom[m];
            } else {
                for (uint32_t k = 0; k <= nb; ++k) d[k] = d[k] && pdom[m][k];
            }
        }
        if (d.empty()) {  // EXIT or only back edges
            d.assign(nb + 1, false);
            d[kSink] = true;
        }
        d[n] = true;
        pdom[n] = std::move(d);
    }
    // The closest strict post-dominator is the one with the most post-dominators.
    std::vector<long> size(nb + 1);
    for (uint32_t n = 0; n <= nb; ++n) size[n] = long(std::count(pdom[n].begin(), pdom[n].end(), true));
    for (uint32_t n = 0; n < nb; ++n) {
        if (pdom[n].empty()) continue;  // unreachable
        uint32_t best = kSink;
        long most = -1;
        for (uint32_t c = 0; c <= nb; ++c)
            if (c != n && pdom[n][c] && size[c] > most) {
                most = size[c];
                best = c;
            }
        sblocks_[n].ipdom = best;
    }
    // Dry run: does every block get emitted at most once?
    semitted_.assign(nb, 0);
    sdry_ = true;
    const bool ok = swalk(0, kNone, -1, false) != kWalkFail;
    sdry_ = false;
    semitted_.assign(nb, 0);
    return ok;
}

// Emits from block b until stop (kNone: until the path ends) in the
// innermost loop `loop` (-1: none). at_header: b is that loop's header,
// entered from its loop { }. Returns kWalkFail, kWalkEnds (every path left
// through return, break or continue) or kWalkFalls (a path reaches stop, or
// the end of the code outside any loop).
int Emitter::swalk(uint32_t b, uint32_t stop, int loop, bool at_header) {
    struct Depth {
        int& d;
        ~Depth() { --d; }
    } depth{++sdepth_};
    if (sdepth_ > kMaxNesting) return kWalkFail;
    const uint32_t kSink = uint32_t(sblocks_.size());
    for (;;) {
        if (b == stop) return kWalkFalls;
        if (!at_header && loop >= 0 && b == sloops_[loop].header) {
            sline("continue;");
            return kWalkEnds;
        }
        at_header = false;
        if (loop >= 0 && b == sloops_[loop].exit) {
            sline("break;");
            return kWalkEnds;
        }
        if (b == kSink) {
            if (loop < 0) return kWalkFalls;
            sline(exit_return());
            return kWalkEnds;
        }
        SBlock& blk = sblocks_[b];
        if (blk.loop >= 0 && loop != blk.loop) {
            const int l = blk.loop;
            sline("loop {");
            if (!sdry_) ++indent_;
            if (swalk(b, kNone, l, true) == kWalkFail) return kWalkFail;
            if (!sdry_) --indent_;
            sline("}");
            if (sloops_[l].exit == kNone) return kWalkEnds;
            b = sloops_[l].exit;
            continue;
        }
        if (semitted_[b]) return kWalkFail;
        semitted_[b] = 1;
        if (!sdry_) {
            const auto& code = p_.code;
            for (size_t k = blk.begin; k < blk.end; ++k) {
                const Inst& i = code[k];
                if (i.op == Opcode::Branch || i.op == Opcode::PushSync || i.op == Opcode::PushBreak ||
                    i.op == Opcode::PopSync || i.op == Opcode::PopBreak)
                    continue;  // the structure carries them
                if (!emit_inst(i)) return kWalkFail;
            }
        }
        if (blk.succs == 0) return kWalkEnds;
        if (blk.succs == 1) {
            b = blk.fall;
            continue;
        }
        // A join outside the innermost loop: the paths leave it through break,
        // continue or EXIT, each on its own, and nothing follows the if/else.
        const uint32_t j = blk.ipdom;
        const bool leaves = loop >= 0 && !sloops_[loop].body.count(j);
        const uint32_t join = leaves ? kNone : j;
        const std::string c = sdry_ ? std::string() : pred(Operand::pred(blk.cond->guard, blk.cond->guard_inv));
        int taken = kWalkFalls, fall = kWalkFalls;
        if (blk.taken == join) {
            sline("if (!(" + c + ")) {");
        } else {
            sline("if (" + c + ") {");
            if (!sdry_) ++indent_;
            taken = swalk(blk.taken, join, loop, false);
            if (taken == kWalkFail) return kWalkFail;
            if (!sdry_) --indent_;
            sline(blk.fall == join ? "}" : "} else {");
        }
        if (blk.fall != join) {
            if (!sdry_) ++indent_;
            fall = swalk(blk.fall, join, loop, false);
            if (fall == kWalkFail) return kWalkFail;
            if (!sdry_) --indent_;
            sline("}");
        }
        if (leaves || (taken == kWalkEnds && fall == kWalkEnds)) return kWalkEnds;
        b = j;
    }
}

void Emitter::resolve_gmem() {
    using namespace gmem_df;
    const auto& code = p_.code;
    const size_t n = code.size();
    gmem_static_.assign(n, -1);
    gmem_why_.assign(n, kWhyUnresolved);
    if (n == 0) return;
    std::map<uint32_t, size_t> index_of;
    for (size_t k = 0; k < n; ++k) index_of.emplace(code[k].address, k);
    std::vector<size_t> sync_targets, break_targets;  // PopSync/PopBreak go to any of them
    for (const Inst& i : code) {
        if (i.op != Opcode::PushSync && i.op != Opcode::PushBreak) continue;
        const auto t = index_of.find(i.target);
        if (t == index_of.end()) return;
        (i.op == Opcode::PushSync ? sync_targets : break_targets).push_back(t->second);
    }
    const uint32_t base = opt_.storage_desc_offset, ubase = opt_.uniform_desc_offset;
    auto desc = [&](uint32_t bank, uint32_t reg, uint32_t offset) -> Val {
        const uint32_t off = offset & 0xffffu;
        if (bank != opt_.storage_desc_bank || reg != kRZ)
            return kOther;
        if (off >= base && off - base < 16 * kGlobalSlots) {
            const uint32_t d = off - base;
            return d % 16 == 0 ? Val(kLo + d / 16) : d % 16 == 4 ? Val(kHi + d / 16) : kOther;
        }
        if (ubase != kNoStorageDesc && off >= ubase && off - ubase < 16 * kUniformDescCount) {
            const uint32_t d = off - ubase;
            return d % 16 == 0 ? Val(kULo + d / 16) : d % 16 == 4 ? Val(kUHi + d / 16) : kOther;
        }
        return kOther;
    };
    auto val = [&](const Operand& o, const State& st) -> Val {
        if (o.kind == Kind::Reg && o.index != kRZ) return st[o.index] == kBot ? kOther : st[o.index];
        if (o.kind == Kind::CBuf) return desc(o.index, o.base, o.imm);
        return kOther;
    };
    // dst = sum of the operands: a descriptor word plus non-pointers stays that
    // word; operands with ok == false (shifted, multiplied) must not be pointers.
    auto sum = [&](const State& st, std::initializer_list<std::pair<const Operand*, bool>> ops) -> Val {
        Val r = kOther;
        for (const auto& [o, ok] : ops) {
            const Val v = val(*o, st);
            if (!pointer(v)) continue;
            if (v == kTop || !ok || o->neg || o->inv || r != kOther) return kTop;
            r = v;
        }
        return r;
    };
    std::vector<State> in(n);
    std::vector<bool> reached(n, false), queued(n, false);
    in[0].fill(kOther);
    reached[0] = true;
    std::vector<size_t> work{0};
    queued[0] = true;
    State out;
    std::vector<size_t> succ;
    while (!work.empty()) {
        const size_t k = work.back();
        work.pop_back();
        queued[k] = false;
        const Inst& i = code[k];
        const State& st = in[k];
        out = st;
        const bool guarded = i.guard != kPT || i.guard_inv;
        auto set = [&](size_t d, Val v) {
            if (d >= i.dst.size() || i.dst[d] == kRZ || i.dst[d] >= kRegs) return;
            Val& r = out[i.dst[d]];
            r = guarded ? join(r == kBot ? kOther : r, v) : v;  // predicated: old or new
        };
        auto src = [&](size_t s) -> const Operand& {
            static const Operand none{};
            return s < i.src.size() ? i.src[s] : none;
        };
        switch (i.op) {
        case Opcode::Mov: set(0, sum(st, {{&src(0), true}})); break;
        case Opcode::LoadConst:
            for (size_t d = 0; d < i.dst.size(); ++d)
                set(d, desc(src(0).index, src(0).base, src(0).imm + 4 * uint32_t(d)));
            break;
        case Opcode::IAdd: set(0, sum(st, {{&src(0), true}, {&src(1), true}})); break;
        case Opcode::IAdd3: set(0, sum(st, {{&src(0), true}, {&src(1), true}, {&src(2), true}})); break;
        case Opcode::IScAdd:
        case Opcode::Lea: set(0, sum(st, {{&src(0), i.aux == 0}, {&src(1), true}})); break;
        case Opcode::LeaHi: set(0, sum(st, {{&src(0), false}, {&src(1), true}, {&src(2), i.aux == 0}})); break;
        case Opcode::XMad: {
            const auto mode = static_cast<ir::XMadMode>(i.aux2);
            const bool c_ok = !(i.aux & ir::kXMadMrg) && (mode == ir::XMadMode::None || mode == ir::XMadMode::CBcc);
            set(0, sum(st, {{&src(0), false}, {&src(1), false}, {&src(2), c_ok}}));
            break;
        }
        case Opcode::Select: set(0, join(val(src(0), st), val(src(1), st))); break;
        // Data from memory, textures, attributes: not a descriptor word.
        case Opcode::LoadGlobal: case Opcode::LoadShared: case Opcode::LoadLocal: case Opcode::AtomGlobal:
        case Opcode::AtomShared: case Opcode::TexSample: case Opcode::TexFetch: case Opcode::TexQuery:
        case Opcode::TexLod: case Opcode::TexGather: case Opcode::LoadAttr: case Opcode::Interp: case Opcode::SysReg:
            for (size_t d = 0; d < i.dst.size(); ++d) set(d, kOther);
            break;
        default: {
            Val v = kOther;
            for (const Operand& o : i.src)
                if (pointer(val(o, st)) || (o.kind == Kind::CBuf && o.base != kRZ && pointer(val(Operand::reg(o.base), st))))
                    v = kTop;
            for (size_t d = 0; d < i.dst.size(); ++d) set(d, v);
            break;
        }
        }
        succ.clear();
        const bool next = k + 1 < n;
        switch (i.op) {
        case Opcode::Exit:
            if (guarded && next) succ.push_back(k + 1);
            break;
        case Opcode::Branch: {
            const auto t = index_of.find(i.target);
            if (t == index_of.end()) {
                gmem_static_.assign(n, -1);
                return;
            }
            succ.push_back(t->second);
            if (guarded && next) succ.push_back(k + 1);
            break;
        }
        case Opcode::PopSync:
        case Opcode::PopBreak: {
            const auto& t = i.op == Opcode::PopSync ? sync_targets : break_targets;
            succ.insert(succ.end(), t.begin(), t.end());
            if (guarded && next) succ.push_back(k + 1);
            break;
        }
        default:
            if (next) succ.push_back(k + 1);
            break;
        }
        for (size_t t : succ) {
            bool changed = !reached[t];
            if (!reached[t]) {
                in[t] = out;
                reached[t] = true;
            } else {
                for (uint32_t r = 0; r < kRegs; ++r) {
                    const Val j = join(in[t][r], out[r]);
                    changed |= j != in[t][r];
                    in[t][r] = j;
                }
            }
            if (changed && !queued[t]) {
                queued[t] = true;
                work.push_back(t);
            }
        }
    }
    // Uniform blocks: the lowest kConstantSlots with resolved accesses get the windows.
    std::set<uint32_t> blocks;
    for (size_t k = 0; k < n; ++k) {
        const Inst& i = code[k];
        if (!reached[k] || (i.op != Opcode::LoadGlobal && i.op != Opcode::StoreGlobal && i.op != Opcode::AtomGlobal))
            continue;
        if (i.src.size() < 2 || i.src[0].kind != Kind::Reg || i.src[1].kind != Kind::Reg || i.src[1].index == kRZ) {
            gmem_why_[k] = kWhy32Bit;
            continue;
        }
        const Val lo = val(i.src[0], in[k]), hi = val(i.src[1], in[k]);
        if (lo >= kLo && lo < kHi && hi == lo + kGlobalSlots) {
            gmem_static_[k] = int8_t(lo - kLo);
        } else if (lo >= kULo && lo < kUHi && hi == lo + kUniformDescCount) {
            gmem_static_[k] = int8_t(kStaticUniformId + (lo - kULo));
            blocks.insert(lo - kULo);
        } else {
            gmem_why_[k] = lo == kTop || hi == kTop                     ? kWhyMixed
                           : lo == kOther || hi == kOther               ? kWhyData
                                                                        : kWhyMismatch;
        }
    }
    for (uint32_t b : blocks)
        if (static_blocks_.size() < kConstantSlots) static_blocks_.push_back(b);
    for (size_t k = 0; k < n; ++k) {
        const int id = gmem_static_[k];
        if (id >= int(kStaticUniformId) &&
            std::find(static_blocks_.begin(), static_blocks_.end(), uint32_t(id) - kStaticUniformId) ==
                static_blocks_.end()) {
            gmem_static_[k] = -1;
            gmem_why_[k] = kWhyWindows;
        }
    }
}

int Emitter::gmem_slot(const Inst& i) {
    const size_t k = size_t(&i - p_.code.data());
    const int id = k < gmem_static_.size() ? gmem_static_[k] : -1;
    if (id >= 0) {
        static_slots_.insert(uint32_t(id));
        ++gmem_static_count_;
    } else {
        gmem_search_ = true;
        ++gmem_dynamic_count_;
        // Why it searches (diagnosis: wgsl_stats.py counts these comments).
        static const char* const kWhy[] = {"not resolved", "32-bit address", "address from data",
                                           "address from several sources", "low and high word disagree",
                                           "more uniform blocks than windows"};
        if (k < gmem_why_.size()) line(std::string("// gmem search: ") + kWhy[gmem_why_[k]]);
    }
    return id;
}

void Emitter::emit_gmem_decls(std::ostringstream& m) const {
    const uint32_t g = opt_.global_group, base = opt_.global_binding_base;
    // Graphics stages: read-only (WebGPU allows no writable storage in vertex shaders).
    const bool read_only = stage_ != Stage::Compute;
    // Compute: table entries kGlobalSlots.. map constant buffer windows (wgsl.h).
    const uint32_t entries = read_only ? kGlobalSlots : kGlobalSlots + kConstantSlots;
    const std::string none = std::to_string(entries) + "u";
    for (uint32_t k = 0; k < kGlobalSlots; ++k)
        m << "@group(" << g << ") @binding(" << base + k << ") var<storage, " << (read_only ? "read" : "read_write")
          << "> gm" << k << (read_only ? ": array<u32>;\n" : ": array<atomic<u32>>;\n");
    m << "@group(" << g << ") @binding(" << base + kGlobalSlots << ") var<uniform> gmem_tab: array<vec4<u32>, "
      << entries << ">;\n";
    for (uint32_t j = 0; !read_only && j < kConstantSlots; ++j)
        m << "@group(" << g << ") @binding(" << base + kConstantSlotBinding + j << ") var<uniform> gmc" << j
          << ": array<vec4<u32>, 4096>;\n";
    const bool probe = probe_;
    if (probe)
        m << "@group(" << g << ") @binding(" << base + kGlobalSlots + 1
          << ") var<storage, read_write> gmem_probe: array<atomic<u32>, " << kGmemProbeWords << ">;\n"
          << "fn probe_count(s: vec2<u32>, hit: u32) {\n"
             "    atomicAdd(&gmem_probe[select(hit, hit + 1u, s.x >= " << none << ")], 1u);\n"
             "}\n";
    // Address (hi:lo) + off (sign-extended) as (table entry, byte offset), if entry k holds it;
    // else (`entries` = none, 0).
    if (!static_slots_.empty())
        m << "fn gmem_addr_at(lo0: u32, hi0: u32, off: u32, k: u32) -> vec2<u32> {\n"
             "    let lo = lo0 + off;\n"
             "    let hi = hi0 + select(0u, 0xffffffffu, (off >> 31u) != 0u) + select(0u, 1u, lo < lo0);\n"
             "    let b = gmem_tab[k];\n"
             "    let o = lo - b.x;\n"
             "    return select(vec2<u32>("
          << none << ", 0u), vec2<u32>(k, o), hi - b.y - select(0u, 1u, lo < b.x) == 0u && o < b.z);\n"
                     "}\n";
    for (uint32_t id : static_slots_) {
        uint32_t e = id;  // storage slot k: entry k; uniform block: its window
        if (id >= kStaticUniformId)
            e = kGlobalSlots + uint32_t(std::find(static_blocks_.begin(), static_blocks_.end(), id - kStaticUniformId) -
                                        static_blocks_.begin());
        m << static_block(id, e, gmem_atomics_, opt_.vector_select);
    }
    if (!gmem_search_ && !probe) return;
    // Table entry and byte offset of a guest address (hi:lo) + off (sign-extended); `entries` = none.
    m << "fn gmem_addr(lo0: u32, hi0: u32, off: u32) -> vec2<u32> {\n"
         "    let lo = lo0 + off;\n"
         "    let hi = hi0 + select(0u, 0xffffffffu, (off >> 31u) != 0u) + select(0u, 1u, lo < lo0);\n"
         "    var r = vec2<u32>("
      << none
      << ", 0u);\n"
         "    for (var k = 0u; k < "
      << none
      << "; k = k + 1u) {\n"
         "        let b = gmem_tab[k];\n"
         "        let o = lo - b.x;\n"
         "        if (hi - b.y - select(0u, 1u, lo < b.x) == 0u && o < b.z) {\n"
         "            r = vec2<u32>(k, o);\n"
         "            break;\n"
         "        }\n"
         "    }\n";
    if (probe)
        m << "    if (r.x >= " << none << " && atomicAdd(&gmem_probe[" << kProbeAddressMiss << "], 1u) == 0u) {\n"
          << "        atomicStore(&gmem_probe[" << kProbeFirstMissLo << "], lo);\n"
          << "        atomicStore(&gmem_probe[" << kProbeFirstMissHi << "], hi);\n"
             "    }\n";
    m << "    return r;\n"
         "}\n";
    m << "fn gmem_ld(s: vec2<u32>, k: u32) -> u32 {\n    var r = 0u;\n    let w = (s.y >> 2u) + k;\n";
    if (probe) m << "    probe_count(s, " << kProbeLoadHit << "u);\n";
    m << "    switch s.x {\n";
    for (uint32_t k = 0; k < kGlobalSlots; ++k) {
        const std::string gm = "gm" + std::to_string(k);
        m << "        case " << k << "u: { r = " << (read_only ? gm + "[w]" : "atomicLoad(&" + gm + "[w])") << "; }\n";
    }
    // Constant buffer windows: read only (stores and atomics there are dropped).
    for (uint32_t j = 0; !read_only && j < kConstantSlots; ++j)
        m << "        case " << kGlobalSlots + j << "u: { r = "
          << (opt_.vector_select ? "vsel(gmc" + std::to_string(j) + "[(w >> 2u) & 4095u], w)"
                                 : "gmc" + std::to_string(j) + "[(w >> 2u) & 4095u][w & 3u]")
          << "; }\n";
    m << "        default: {}\n    }\n    return r;\n}\n";
    if (read_only) return;
    const std::string probe_store =
        "    probe_count(s, " + std::to_string(kProbeStoreHit) + "u);\n    if (s.x < " + std::to_string(kGlobalSlots) +
        "u) { atomicAdd(&gmem_probe[" + std::to_string(kProbeSlotStores) + "u + s.x], 1u); }\n";
    m << "fn gmem_st(s: vec2<u32>, k: u32, v: u32) {\n    let w = (s.y >> 2u) + k;\n";
    if (probe) m << probe_store;
    m << "    switch s.x {\n";
    for (uint32_t k = 0; k < kGlobalSlots; ++k) m << "        case " << k << "u: { atomicStore(&gm" << k << "[w], v); }\n";
    m << "        default: {}\n    }\n}\n";
    // Sub-word store: clear the bits of mask, then set v (both atomic, so neighbouring bytes
    // written by other threads survive).
    m << "fn gmem_st_sub(s: vec2<u32>, mask: u32, v: u32) {\n    let w = s.y >> 2u;\n";
    if (probe) m << probe_store;
    m << "    switch s.x {\n";
    for (uint32_t k = 0; k < kGlobalSlots; ++k)
        m << "        case " << k << "u: { atomicAnd(&gm" << k << "[w], ~mask); atomicOr(&gm" << k << "[w], v); }\n";
    m << "        default: {}\n    }\n}\n";
}

std::string Emitter::atomic_fn(const std::string& name, const std::string& arr) {
    const std::string p = "&" + arr + "[w]";
    return "fn " + name + "(w: u32, op: u32, v: u32) -> u32 {\n"
           "    var r = 0u;\n"
           "    switch op {\n"
           "        case 0u: { r = atomicAdd(" + p + ", v); }\n"
           "        case 1u: { r = atomicMin(" + p + ", v); }\n"
           "        case 2u: { r = atomicMax(" + p + ", v); }\n"
           "        case 5u: { r = atomicAnd(" + p + ", v); }\n"
           "        case 6u: { r = atomicOr(" + p + ", v); }\n"
           "        case 7u: { r = atomicXor(" + p + ", v); }\n"
           "        case 8u: { r = atomicExchange(" + p + ", v); }\n"
           "        default: {\n"
           "            loop {\n"
           "                let old = atomicLoad(" + p + ");\n"
           "                let x = atomicCompareExchangeWeak(" + p + ", old, atom_apply(op, old, v));\n"
           "                if (x.exchanged) {\n"
           "                    r = old;\n"
           "                    break;\n"
           "                }\n"
           "            }\n"
           "        }\n"
           "    }\n"
           "    return r;\n"
           "}\n";
}

bool is_terminator(Opcode op) {
    return op == Opcode::Branch || op == Opcode::PopSync || op == Opcode::PopBreak || op == Opcode::Exit;
}

// In lockstep mode a barrier ends its block: the thread waits there for the others.
bool Emitter::is_term(Opcode op) const { return is_terminator(op) || (lockstep_ && op == Opcode::Barrier); }

Result Emitter::run() {
    Result r;
    if (stage_ != Stage::Vertex && stage_ != Stage::Fragment && stage_ != Stage::Compute) {
        r.error = "only vertex, fragment and compute shaders are supported";
        return r;
    }
    const auto& code = p_.code;
    bool has_barrier = false, divergent_exit = false;
    for (const Inst& i : code) {
        if (i.op == Opcode::Branch || i.op == Opcode::PushSync || i.op == Opcode::PushBreak || i.op == Opcode::PopSync ||
            i.op == Opcode::PopBreak)
            has_cf_ = true;
        if (i.op == Opcode::Branch || i.op == Opcode::PushSync || i.op == Opcode::PushBreak) targets_.insert(i.target);
        const bool guarded = i.guard != kPT || i.guard_inv;
        if (i.op == Opcode::Barrier) has_barrier = true;
        if (guarded && (i.op == Opcode::Exit || i.op == Opcode::Barrier)) divergent_exit = true;
        if (i.op == Opcode::TexSample || i.op == Opcode::TexFetch) handle_dim_.emplace(i.aux, i.tex_dim);
    }
    // WGSL barriers must be in uniform control flow. Straight-line code keeps them as they are;
    // with branches or predicated EXIT/BAR every thread instead runs the block dispatcher one
    // block per step, and the workgroup meets at a real barrier after every step.
    lockstep_ = stage_ == Stage::Compute && has_barrier && (has_cf_ || divergent_exit);
    if (stage_ == Stage::Compute && opt_.storage_desc_offset != kNoStorageDesc && !opt_.gmem_probe) resolve_gmem();
    const bool dispatch = has_cf_ || lockstep_;

    if (!dispatch) {
        for (const Inst& i : code)
            if (!emit_inst(i)) break;
    } else {
        // Blocks start at the entry, at every branch/push target and after
        // every terminator; they run inside a loop { switch pc { ... } }.
        std::set<uint32_t> leaders{code.front().address};
        for (size_t k = 0; k < code.size(); ++k) {
            const Inst& i = code[k];
            if (i.op == Opcode::Branch || i.op == Opcode::PushSync || i.op == Opcode::PushBreak) leaders.insert(i.target);
            if (is_term(i.op) && k + 1 < code.size()) leaders.insert(code[k + 1].address);
        }
        uint32_t id = 0;
        for (uint32_t a : leaders) block_of_[a] = id++;
        const uint32_t kDone = id;
        if (opt_.structured && lockstep_) {
            dispatcher_why_ = "lockstep barrier";
        } else if (opt_.structured && !resolve_stacks()) {
            dispatcher_why_ = "stacks not static";
        } else if (opt_.structured) {
            static_stacks_on_ = true;
            structured_ = structure(leaders);
            static_stacks_on_ = structured_;
            dispatcher_why_ = "not structurable";
        }
        if (structured_) {
            sended_ = swalk(0, kNone, -1, false) == kWalkEnds;
        } else {
        line("var pc: u32 = 0u;");
        static_stacks_on_ = opt_.static_stacks && resolve_stacks();
        if (!static_stacks_on_) {
            line("var ssy_stack: array<u32, 16>;");
            line("var ssy_sp: u32 = 0u;");
            line("var pbk_stack: array<u32, 16>;");
            line("var pbk_sp: u32 = 0u;");
        }
        if (lockstep_) {
            line("var ls_state: u32 = 0u;  // 0 running, 1 waiting at the barrier, 2 exited");
            // Explicit reset: WebGPU zero-initializes workgroup memory, but lavapipe (via wgpu)
            // was seen reusing it between workgroups.
            line("if (l_index == 0u) {");
            line("    atomicStore(&ls_running, 0u);");
            line("    atomicStore(&ls_waiting, 0u);");
            line("}");
            line("workgroupBarrier();");
            line("atomicAdd(&ls_running, 1u);");
        }
        line("loop {");
        ++indent_;
        if (lockstep_) {
            line("if (ls_state == 0u && pc >= " + u(kDone) + ") {");
            line("    " + exit_return());
            line("}");
            line("if (ls_state == 0u) {");
            ++indent_;
        } else {
            // Leaving through a break (not a return in default) keeps both naga
            // (wants a return at the end) and Tint (warns on unreachable code) quiet.
            line("if (pc >= " + u(kDone) + ") { break; }");
        }
        line("switch pc {");
        ++indent_;
        for (size_t k = 0; k < code.size();) {
            const uint32_t bid = block_of_.at(code[k].address);
            line("case " + u(bid) + ": {");
            ++indent_;
            size_t e = k;
            bool ended = false;
            while (e < code.size()) {
                const Inst& i = code[e];
                if (e > k && leaders.count(i.address)) break;
                // Fallthrough target for a guarded terminator; a barrier always continues there
                if ((is_term(i.op) && (i.guard != kPT || i.guard_inv) && i.op != Opcode::Exit) ||
                    (lockstep_ && i.op == Opcode::Barrier)) {
                    const uint32_t next = e + 1 < code.size() ? block_of_.at(code[e + 1].address) : kDone;
                    line("pc = " + u(next) + ";");
                }
                if (!emit_inst(i)) break;
                ended = is_term(i.op) && i.guard == kPT && !i.guard_inv;
                ++e;
                if (is_term(i.op)) break;
            }
            if (!error_.empty()) break;
            // Plain fallthrough into the next block (or past the end)
            // (the lifter only stops following fallthrough at terminators, so
            // the next instruction in the list is the fallthrough successor)
            if (!ended && !(e > 0 && is_term(code[e - 1].op) && code[e - 1].op != Opcode::Exit))
                line("pc = " + u(e < code.size() ? block_of_.at(code[e].address) : kDone) + ";");
            --indent_;
            line("}");
            k = e;
        }
        line("default: {}");
        --indent_;
        line("}");
        if (lockstep_) {
            --indent_;
            line("}");
            // Rendezvous: everybody has run one block (or is waiting/exited). Release the waiting
            // threads when nobody is running any more; stop when everybody has exited.
            line("storageBarrier();");
            line("workgroupBarrier();");
            line("if (l_index == 0u) {");
            line("    let ls_r = atomicLoad(&ls_running);");
            line("    let ls_w = atomicLoad(&ls_waiting);");
            line("    ls_ctl = select(select(0u, 2u, ls_w != 0u), 1u, ls_r != 0u);");
            line("}");
            line("let ls_c = workgroupUniformLoad(&ls_ctl);");
            line("if (ls_c == 0u) { break; }");
            line("if (ls_c == 2u && ls_state == 1u) {");
            line("    ls_state = 0u;");
            line("    atomicSub(&ls_waiting, 1u);");
            line("    atomicAdd(&ls_running, 1u);");
            line("}");
        }
        --indent_;
        line("}");
        }
    }
    if (!error_.empty()) {
        r.error = error_;
        return r;
    }
    // Needs a final return unless straight-line code ends in an unconditional
    // EXIT or no path of structured code gets there (the dispatcher loop is
    // left by a break).
    if (!lockstep_ && !sended_ &&
        (dispatch || !(code.back().op == Opcode::Exit && code.back().guard == kPT && !code.back().guard_inv)))
        line(exit_return());

    // --- module assembly ---
    std::ostringstream m;
    m << "// Generated by lgp maxwell2wgsl (P4 shader spike). Do not edit.\n";
    m << "// Stage: " << (stage_ == Stage::Vertex ? "vertex" : stage_ == Stage::Compute ? "compute" : "fragment") << "\n";
    if (has_cf_ || lockstep_)
        m << "// Control flow: "
          << (structured_ ? "structured" : static_stacks_on_ ? "dispatcher, static stacks" : "dispatcher")
          << (structured_ ? "" : std::string(" (") + dispatcher_why_ + ")") << "\n";
    if (stage_ == Stage::Fragment && implicit_lod_) {
        // Maxwell computes derivatives per quad regardless of divergence. For
        // WGSL, the dispatcher loop and predicated instructions (`if (p)`)
        // make control flow non-uniform; Tint (Chrome) rejects implicit-LOD
        // sampling there, naga does not, so the check is always off.
        m << "diagnostic(off, derivative_uniformity);\n";
    }
    m << "\n";
    for (uint32_t b : banks_) m << "@group(" << opt_.cbuf_group << ") @binding(" << b << ") var<uniform> cb" << b << ": array<vec4<u32>, 4096>;\n";
    for (size_t k = 0; k < tex_handles_.size(); ++k) {
        m << "// texture handle " << hex(tex_handles_[k]) << "\n";
        m << "@group(" << opt_.texture_group << ") @binding(" << 2 * k << ") var tex" << k
          << (tex_depth_[k] && tex_array_[k] ? ": texture_depth_2d_array;\n"
              : tex_depth_[k]      ? ": texture_depth_2d;\n"
              : tex_dim_[k] == 3   ? ": texture_3d<f32>;\n"
              : tex_array_[k]      ? ": texture_2d_array<f32>;\n"
                                   : ": texture_2d<f32>;\n");
        m << "@group(" << opt_.texture_group << ") @binding(" << 2 * k + 1 << ") var smp" << k << (tex_depth_[k] ? ": sampler_comparison;\n" : ": sampler;\n");
    }
    if (opt_.texture_swizzle && !tex_handles_.empty()) {
        m << "@group(" << opt_.texture_group << ") @binding(" << 2 * tex_handles_.size()
          << ") var<uniform> tex_swizzle: array<vec4<u32>, " << (tex_handles_.size() + 3) / 4 + (scale_used_ ? 1 : 0)
          << ">;\n";
        if (scale_used_) {
            char lit[32];
            std::snprintf(lit, sizeof(lit), "%.9g", double(opt_.render_scale));
            std::string scale = lit;
            if (scale.find_first_of(".e") == std::string::npos) scale += ".0";
            m << "const lgp_render_scale: f32 = " << scale << ";\n"
                 "fn tex_scaled(k: u32) -> bool {\n"
                 "    return ((tex_swizzle[" << (tex_handles_.size() + 3) / 4 << "][k / 32u] >> (k % 32u)) & 1u) != 0u;\n"
                 "}\n"
                 "fn tex_host_xy(k: u32, xy: vec2<i32>) -> vec2<i32> {\n"
                 "    if (!tex_scaled(k)) {\n        return xy;\n    }\n"
                 "    return vec2<i32>(floor((vec2<f32>(xy) + 0.5) * lgp_render_scale));\n"
                 "}\n"
                 "fn tex_guest_dim(k: u32, d: vec2<u32>) -> vec2<u32> {\n"
                 "    if (!tex_scaled(k)) {\n        return d;\n    }\n"
                 "    return vec2<u32>(max(round(vec2<f32>(d) / lgp_render_scale), vec2<f32>(1.0)));\n"
                 "}\n";
        }
        if (swizzle_used_)
            m << "fn tex_swizzle_component(t: vec4<f32>, c: u32) -> f32 {\n"
                 "    switch c {\n"
                 "        case 0u: { return 0.0; }\n"
                 "        case 1u: { return 1.0; }\n"
                 "        case 2u: { return t.x; }\n"
                 "        case 3u: { return t.y; }\n"
                 "        case 4u: { return t.z; }\n"
                 "        default: { return t.w; }\n"
                 "    }\n"
                 "}\n"
                 "fn tex_swizzle_apply(t: vec4<f32>, s: u32) -> vec4<f32> {\n"
                 "    return vec4<f32>(tex_swizzle_component(t, s & 7u), tex_swizzle_component(t, (s >> 3u) & 7u),\n"
                 "                     tex_swizzle_component(t, (s >> 6u) & 7u), tex_swizzle_component(t, (s >> 9u) & 7u));\n"
                 "}\n";
        if (lod_bias_used_)
            m << "fn tex_lod_bias(s: u32) -> f32 {\n    return f32(bitcast<i32>(s) >> " << kLodBiasShift
              << "u) / 256.0;\n}\n";
        if (border_used_)
            m << "fn tex_outside_border(uv: vec2<f32>, s: u32) -> bool {\n"
                 "    let u = (s & " << kBorderU << "u) != 0u && (uv.x < 0.0 || uv.x > 1.0);\n"
                 "    let v = (s & " << kBorderV << "u) != 0u && (uv.y < 0.0 || uv.y > 1.0);\n"
                 "    return u || v;\n"
                 "}\n"
                 // kBorderForce only in compare words: colour words hold the LOD bias from bit 16.
                 "fn tex_outside_border_cmp(uv: vec2<f32>, s: u32) -> bool {\n"
                 "    return tex_outside_border(uv, s) || (s & " << kBorderForce << "u) != 0u;\n"
                 "}\n"
                 "fn tex_border(r: f32, uv: vec2<f32>, s: u32) -> f32 {\n"
                 "    return select(r, select(0.0, 1.0, (s & " << kBorderPass << "u) != 0u), tex_outside_border_cmp(uv, s));\n"
                 "}\n"
                 "fn tex_compare_ref(r: f32, s: u32) -> f32 {\n"
                 "    return select(r, clamp(r, 0.0, 1.0), (s & " << kCompareClampRef << "u) != 0u);\n"
                 "}\n"
                 "fn tex_border_color(t: vec4<f32>, uv: vec2<f32>, s: u32) -> vec4<f32> {\n"
                 "    let c = select(0.0, 1.0, (s & " << kBorderPass << "u) != 0u);\n"
                 "    let a = select(0.0, 1.0, (s & " << kBorderAlpha << "u) != 0u);\n"
                 "    return select(t, vec4<f32>(c, c, c, a), tex_outside_border(uv, s));\n"
                 "}\n"
                 "fn tex_border4(r: vec4<f32>, uv: vec2<f32>, s: u32) -> vec4<f32> {\n"
                 "    return select(r, vec4<f32>(select(0.0, 1.0, (s & " << kBorderPass << "u) != 0u)), tex_outside_border_cmp(uv, s));\n"
                 "}\n";
    }
    probe_ = opt_.gmem_probe && uses_gmem_ && stage_ == Stage::Compute;
    if (uses_gmem_) emit_gmem_decls(m);
    if (smem_atomics_ || gmem_atomics_) {
        // Operations that WGSL has no atomic for run as compare-exchange loops.
        m << "fn atom_apply(op: u32, a: u32, b: u32) -> u32 {\n"
             "    var r = b;\n"
             "    switch op {\n"
             "        case 3u: { r = select(a + 1u, 0u, a >= b); }  // INC: wraps at b\n"
             "        case 4u: { r = select(a - 1u, b, a == 0u || a > b); }  // DEC\n"
             "        case 9u: { r = bitcast<u32>(min(bitcast<i32>(a), bitcast<i32>(b))); }\n"
             "        case 10u: { r = bitcast<u32>(max(bitcast<i32>(a), bitcast<i32>(b))); }\n"
             "        default: {}\n"
             "    }\n"
             "    return r;\n"
             "}\n";
    }
    if (gmem_atomics_)
        for (uint32_t k = 0; k < kGlobalSlots; ++k) m << atomic_fn("atom_gm" + std::to_string(k), "gm" + std::to_string(k));
    if (gmem_atomics_ && (gmem_search_ || probe_)) {
        m << "fn gmem_atom(s: vec2<u32>, op: u32, v: u32) -> u32 {\n    var r = 0u;\n    let w = s.y >> 2u;\n";
        if (probe_) m << "    probe_count(s, " << kProbeAtomicHit << "u);\n";
        m << "    switch s.x {\n";
        for (uint32_t k = 0; k < kGlobalSlots; ++k) m << "        case " << k << "u: { r = atom_gm" << k << "(w, op, v); }\n";
        m << "        default: {}\n    }\n    return r;\n}\n";
    }
    if (uses_smem_) {
        const uint32_t bytes = opt_.shared_memory_bytes ? opt_.shared_memory_bytes : 16384;
        shared_bytes_ = (bytes + 3) / 4 * 4;
        m << "var<workgroup> smem: array<atomic<u32>, " << shared_bytes_ / 4 << ">;\n";
        if (smem_atomics_) m << atomic_fn("smem_atom_w", "smem") << "fn smem_atom(w: u32, op: u32, v: u32) -> u32 { return smem_atom_w(w, op, v); }\n";
    }
    if (uses_lmem_) {
        uint32_t bytes = opt_.local_memory_bytes ? opt_.local_memory_bytes : p_.sph.local_memory_size;
        if (bytes == 0) bytes = 4096;
        local_bytes_ = (bytes + 3) / 4 * 4;
        m << "var<private> lmem: array<u32, " << local_bytes_ / 4 << ">;\n";
    }
    if (uses_shf_) {
        m << "fn shf_l(lo: u32, hi: u32, n: u32) -> u32 {\n"
             "    var r = 0u;\n"
             "    if (n == 0u) { r = hi; } else if (n < 32u) { r = (hi << n) | (lo >> (32u - n)); } else if (n < 64u) { r = lo << (n - 32u); }\n"
             "    return r;\n"
             "}\n"
             "fn shf_r(lo: u32, hi: u32, n: u32, sgn: bool, high: bool) -> u32 {\n"
             "    let fill = select(0u, 0xffffffffu, sgn && (hi >> 31u) != 0u);\n"
             "    var rl = fill;\n"
             "    var rh = fill;\n"
             "    if (n == 0u) {\n"
             "        rl = lo;\n        rh = hi;\n"
             "    } else if (n < 32u) {\n"
             "        rl = (lo >> n) | (hi << (32u - n));\n"
             "        rh = select(hi >> n, bitcast<u32>(bitcast<i32>(hi) >> n), sgn);\n"
             "    } else if (n < 64u) {\n"
             "        rl = select(hi >> (n - 32u), bitcast<u32>(bitcast<i32>(hi) >> (n - 32u)), sgn);\n"
             "    }\n"
             "    return select(rl, rh, high);\n"
             "}\n";
    }
    if (need_mul_rz_) {
        // FMUL.FTZ.RZ computed exactly in integer arithmetic (float tricks like a Dekker split
        // are not safe: GPU compilers may contract or reassociate them). Denormal inputs and
        // results flush to zero, overflow gives the largest finite value, Inf/NaN take the
        // round-to-nearest product (identical for those).
        m << "fn mul_rz(a: f32, b: f32) -> f32 {\n"
             "    let ua = bitcast<u32>(a);\n    let ub = bitcast<u32>(b);\n"
             "    let s = (ua ^ ub) & 0x80000000u;\n"
             "    let ea = (ua >> 23u) & 0xffu;\n    let eb = (ub >> 23u) & 0xffu;\n"
             "    if (ea == 255u || eb == 255u) { return a * b; }\n"
             "    if (ea == 0u || eb == 0u) { return bitcast<f32>(s); }\n"
             "    let ma = (ua & 0x7fffffu) | 0x800000u;\n    let mb = (ub & 0x7fffffu) | 0x800000u;\n"
             "    let a0 = ma & 0xffffu;\n    let a1 = ma >> 16u;\n    let b0 = mb & 0xffffu;\n    let b1 = mb >> 16u;\n"
             "    let p00 = a0 * b0;\n    let mid = a0 * b1 + a1 * b0;\n"
             "    let lo = p00 + (mid << 16u);\n"
             "    let hi = a1 * b1 + (mid >> 16u) + select(0u, 1u, lo < p00);\n"
             "    var e = i32(ea) + i32(eb) - 127;\n"
             "    var mant: u32;\n"
             "    if ((hi & 0x8000u) != 0u) {\n        mant = (hi << 8u) | (lo >> 24u);\n        e = e + 1;\n"
             "    } else {\n        mant = (hi << 9u) | (lo >> 23u);\n    }\n"
             "    if (e >= 255) { return bitcast<f32>(s | 0x7f7fffffu); }\n"
             "    if (e <= 0) { return bitcast<f32>(s); }\n"
             "    return bitcast<f32>(s | (u32(e) << 23u) | (mant & 0x7fffffu));\n"
             "}\n";
    }
    if (need_rz_) {
        // Keeps the 24 most significant bits, so f32() of the result is exact (round toward zero).
        m << "fn rz_u32(v: u32) -> u32 {\n    let s = select(0u, 8u - countLeadingZeros(v), countLeadingZeros(v) < 8u);\n    return (v >> s) << s;\n}\n";
    }
    if (need_vsel_ || (opt_.vector_select && uses_gmem_ && stage_ == Stage::Compute)) {
        m << "fn vsel(v: vec4<u32>, i: u32) -> u32 {\n"
             "    let c = i & 3u;\n"
             "    return select(select(v.x, v.y, c == 1u), select(v.z, v.w, c == 3u), c >= 2u);\n"
             "}\n";
    }
    if (need_nonconst_) {
        m << "var<private> nonconst_zero: u32 = 0u;\n";
        m << "fn nonconst_u32(v: u32) -> u32 { return v | nonconst_zero; }\n";
    }
    m << "\n";
    for (uint32_t r : regs_) m << "var<private> r" << r << ": u32;\n";
    for (uint32_t pr : preds_) m << "var<private> p" << pr << ": bool;\n";
    if (uses_cc_) m << "var<private> cc_z: bool;\nvar<private> cc_s: bool;\nvar<private> cc_c: bool;\nvar<private> cc_o: bool;\n";

    if (stage_ == Stage::Compute) {
        m << "var<private> l_id: vec3<u32>;\nvar<private> w_id: vec3<u32>;\nvar<private> l_index: u32;\n";
        if (lockstep_)
            m << "var<workgroup> ls_running: atomic<u32>;\nvar<workgroup> ls_waiting: atomic<u32>;\nvar<workgroup> ls_ctl: u32;\n";
        m << "\noverride wg_size_x: u32 = " << opt_.workgroup_size[0] << "u;\n";
        m << "override wg_size_y: u32 = " << opt_.workgroup_size[1] << "u;\n";
        m << "override wg_size_z: u32 = " << opt_.workgroup_size[2] << "u;\n";
        m << "\n@compute @workgroup_size(wg_size_x, wg_size_y, wg_size_z)\n"
             "fn main(@builtin(local_invocation_id) lid: vec3<u32>, @builtin(workgroup_id) wid: vec3<u32>,\n"
             "        @builtin(local_invocation_index) lidx: u32) {\n"
             "    l_id = lid;\n    w_id = wid;\n    l_index = lidx;\n";
        if (probe_) m << "    atomicAdd(&gmem_probe[" << kProbeInvocations << "], 1u);\n";
    } else if (stage_ == Stage::Vertex) {
        // Vertex inputs as raw bits (integer formats feed them unconverted, retype_vertex_inputs).
        for (uint32_t i : gen_in_) m << "var<private> vin" << i << ": vec4<u32>;\n";
        if (uses_vertex_id_) m << "var<private> vertex_id: u32;\n";
        if (uses_instance_id_) m << "var<private> instance_id: u32;\n";
        m << "var<private> o_pos: vec4<f32>;\n";
        for (uint32_t i : gen_out_) m << "var<private> vout" << i << ": vec4<f32>;\n";
        // Outputs: what the shader writes plus every input of the linked
        // fragment shader (WebGPU rejects a pipeline whose fragment stage
        // reads a location the vertex stage does not provide; the GPU leaves
        // such a varying undefined, here it is zero).
        std::set<uint32_t> outs = gen_out_;
        for (const auto& [loc, mode] : opt_.vertex_output_interp)
            if (loc < 32)
                outs.insert(loc);
        m << "\nstruct VertexOutput {\n    @builtin(position) position: vec4<f32>,\n";
        for (uint32_t i : outs) {
            const auto it = opt_.vertex_output_interp.find(i);
            const Interp mode = it == opt_.vertex_output_interp.end() ? Interp::Perspective : it->second;
            m << "    @location(" << i << ") " << interp_attr(mode) << " v" << i << ": vec4<f32>,\n";
        }
        // lgp_flip_y: negate clip y, set by the pipeline for NVN's lower-left
        // window origin (rows count up from memory row 0, as on the GPU).
        // lgp_depth_minus_w: NVN depth mode NEAR_IS_MINUS_W (OpenGL clip space,
        // z in [-w, w]) to WebGPU's [0, w], set by the pipeline.
        m << "}\n\noverride lgp_flip_y: bool = false;\noverride lgp_depth_minus_w: bool = false;\n\n"
             "fn finish_vertex() -> VertexOutput {\n"
             "    var p = o_pos;\n    if (lgp_flip_y) {\n        p.y = -p.y;\n    }\n"
             "    if (lgp_depth_minus_w) {\n        p.z = (p.z + p.w) * 0.5;\n    }\n    return VertexOutput(p";
        for (uint32_t i : outs) {
            if (gen_out_.count(i))
                m << ", vout" << i;
            else
                m << ", vec4<f32>(0.0)";
        }
        m << ");\n}\n\n@vertex\nfn main(";
        bool first = true;
        auto param = [&](const std::string& s) {
            m << (first ? "" : ", ") << s;
            first = false;
        };
        for (uint32_t i : gen_in_) param("@location(" + std::to_string(i) + ") a" + std::to_string(i) + ": vec4<f32>");
        if (uses_vertex_id_) param("@builtin(vertex_index) vid: u32");
        if (uses_instance_id_) param("@builtin(instance_index) iid: u32");
        m << ") -> VertexOutput {\n";
        for (uint32_t i : gen_in_) m << "    vin" << i << " = bitcast<vec4<u32>>(a" << i << ");\n";
        if (uses_vertex_id_) m << "    vertex_id = vid;\n";
        if (uses_instance_id_) m << "    instance_id = iid;\n";
    } else {
        for (const auto& [i, mode] : in_mode_) m << "var<private> vin" << i << ": vec4<f32>;\n";
        m << "var<private> fpos: vec4<f32>;\n";
        // Options::render_scale: gl_FragCoord in guest pixels of a scaled target.
        if (opt_.render_scale != 1.0f && fpos_xy_used_) m << "override lgp_target_scale: f32 = 1.0;\n";
        // Colour outputs: each enabled target takes four consecutive registers
        // starting at R0; sample mask and depth follow (NAK convention).
        uint32_t reg = 0;
        std::vector<std::pair<uint32_t, uint32_t>> targets;  // (target, first reg)
        for (uint32_t t = 0; t < 8; ++t) {
            bool any = false;
            for (uint32_t c = 0; c < 4; ++c) any |= p_.sph.ps_output(t, c);
            if (!any) continue;
            targets.emplace_back(t, reg);
            reg += 4;
        }
        const bool depth = p_.sph.ps_output_depth();
        const uint32_t depth_reg = reg + 1;
        auto rd = [&](uint32_t r) {
            m << "var<private> r" << r << ": u32;\n";
        };
        for (auto [t, base] : targets)
            for (uint32_t c = 0; c < 4; ++c)
                if (p_.sph.ps_output(t, c) && !regs_.count(base + c)) rd(base + c);
        if (depth && !regs_.count(depth_reg)) rd(depth_reg);

        m << "\nstruct FragmentOutput {\n";
        for (auto [t, base] : targets) m << "    @location(" << t << ") c" << t << ": vec4<f32>,\n";
        if (depth) m << "    @builtin(frag_depth) depth: f32,\n";
        if (targets.empty() && !depth) m << "    @location(0) unused: vec4<f32>,\n";
        // Fixed-function alpha test on color target 0 (NVN ColorState alpha
        // test + SetAlphaRef), set by the pipeline: lgp_alpha_func is the
        // backend's CompareFunc (0 Never ... 7 Always), the fragment is
        // discarded unless "alpha <func> lgp_alpha_ref" holds.
        const bool alpha_test = !targets.empty() && targets.front().first == 0 && p_.sph.ps_output(0, 3);
        if (alpha_test)
            m << "}\n\noverride lgp_alpha_func: u32 = 7u;\noverride lgp_alpha_ref: f32 = 0.0;\n\n"
                 "fn lgp_alpha_pass(a: f32) -> bool {\n"
                 "    switch lgp_alpha_func {\n"
                 "        case 0u: { return false; }\n"
                 "        case 1u: { return a < lgp_alpha_ref; }\n"
                 "        case 2u: { return a == lgp_alpha_ref; }\n"
                 "        case 3u: { return a <= lgp_alpha_ref; }\n"
                 "        case 4u: { return a > lgp_alpha_ref; }\n"
                 "        case 5u: { return a != lgp_alpha_ref; }\n"
                 "        case 6u: { return a >= lgp_alpha_ref; }\n"
                 "        default: { return true; }\n"
                 "    }\n";
        m << "}\n\nfn finish_fragment() -> FragmentOutput {\n    let out = FragmentOutput(";
        bool first = true;
        for (auto [t, base] : targets) {
            m << (first ? "" : ", ") << "vec4<f32>(";
            for (uint32_t c = 0; c < 4; ++c) {
                if (c) m << ", ";
                if (p_.sph.ps_output(t, c))
                    m << "bitcast<f32>(r" << base + c << ")";
                else
                    m << "0.0";
            }
            m << ")";
            first = false;
        }
        if (depth) m << (first ? "" : ", ") << "bitcast<f32>(r" << depth_reg << ")";
        if (targets.empty() && !depth) m << "vec4<f32>(0.0)";
        m << ");\n";
        if (alpha_test) m << "    if (!lgp_alpha_pass(out.c0.a)) {\n        discard;\n    }\n";
        m << "    return out;\n}\n\n@fragment\nfn main(@builtin(position) frag_position: vec4<f32>";
        for (const auto& [i, mode] : in_mode_)
            m << ", @location(" << i << ") " << interp_attr(to_interp(mode)) << " a" << i << ": vec4<f32>";
        if (opt_.render_scale != 1.0f && fpos_xy_used_)
            m << ") -> FragmentOutput {\n    fpos = vec4<f32>(frag_position.xy / lgp_target_scale, frag_position.zw);\n";
        else
            m << ") -> FragmentOutput {\n    fpos = frag_position;\n";
        for (const auto& [i, mode] : in_mode_) m << "    vin" << i << " = a" << i << ";\n";
    }
    m << body_.str() << "}\n";

    r.ok = true;
    r.code = m.str();
    r.bindings.cbuf_banks.assign(banks_.begin(), banks_.end());
    r.bindings.texture_handles = tex_handles_;
    r.bindings.texture_dims = tex_dim_;
    r.bindings.texture_arrays = tex_array_;
    r.bindings.texture_depth = tex_depth_;
    r.bindings.global_memory = uses_gmem_;
    r.bindings.shared_memory_bytes = shared_bytes_;
    r.bindings.local_memory_bytes = local_bytes_;
    r.bindings.barrier_lockstep = lockstep_;
    r.bindings.gmem_probe = probe_;
    for (uint32_t id : static_slots_)
        if (id < kStaticUniformId) r.bindings.static_storage_slots.push_back(id);
    r.bindings.static_uniform_blocks = static_blocks_;
    r.bindings.gmem_static_accesses = gmem_static_count_;
    r.bindings.gmem_dynamic_accesses = gmem_dynamic_count_;
    return r;
}

}  // namespace

std::string move_static_slots(const std::string& code, const std::map<uint32_t, uint32_t>& moved) {
    std::string out = code;
    const bool vsel = code.find("fn vsel(") != std::string::npos;
    for (const auto& [id, entry] : moved) {
        if ((id >= kGlobalSlots && (id < kStaticUniformId || id >= kStaticUniformId + kUniformDescCount)) ||
            entry >= kGlobalSlots + kConstantSlots)
            return {};
        const std::string begin = static_marker(id, "begin"), end = static_marker(id, "end");
        const size_t b = out.find(begin), e = out.find(end);
        if (b == std::string::npos || e == std::string::npos || e < b) return {};
        const bool atomics = out.find("fn " + static_fn(id) + "_atom(", b) < e;
        out.replace(b, e + end.size() - b, static_block(id, entry, atomics, vsel));
    }
    return out;
}

std::map<uint32_t, Interp> fragment_input_interpolation(const shader::Sph& sph) {
    std::map<uint32_t, Interp> out;
    for (uint32_t i = 0; i < 32; ++i)
        for (uint32_t c = 0; c < 4; ++c)
            if (const PixelImap m = sph.ps_input(i, c); m != PixelImap::Unused) {
                out.emplace(i, to_interp(m));
                break;
            }
    return out;
}

bool static_stacks_default() {
    static const bool on = [] {
        const char* v = std::getenv("LGP_WGSL_STATIC_STACKS");
        return v != nullptr && *v != '\0' && std::string(v) != "0";
    }();
    return on;
}

bool structured_default() {
    static const bool on = [] {
        const char* v = std::getenv("LGP_WGSL_STRUCTURED");
        return v == nullptr || *v == '\0' || std::string(v) != "0";
    }();
    return on;
}

bool compare_const_default() {
    static const bool on = [] {
        const char* v = std::getenv("LGP_WGSL_COMPARE_CONST");
        return v != nullptr && *v != '\0' && std::string(v) != "0";
    }();
    return on;
}

bool vector_select_default() {
    static const bool on = [] {
        if (const char* v = std::getenv("LGP_WGSL_VECTOR_SELECT"); v != nullptr && *v != '\0')
            return std::string(v) != "0";
#if defined(__ANDROID__)
        return true;
#else
        return false;
#endif
    }();
    return on;
}

Result emit(const ir::Program& program, const Options& options) {
    if (program.code.empty()) {
        Result r;
        r.error = "empty program";
        return r;
    }
    Emitter e(program, options);
    return e.run();
}

std::string retype_vertex_inputs(const std::string& code, const std::map<uint32_t, VertexInputType>& types) {
    std::string out = code;
    auto replace = [&](const std::string& from, const std::string& to) {
        const size_t at = out.find(from);
        if (at == std::string::npos) return false;
        out.replace(at, from.size(), to);
        return true;
    };
    using T = VertexInputType;
    for (const auto& [loc, type] : types) {
        if (type == T::Float) continue;
        const std::string n = std::to_string(loc), a = "a" + n;
        const bool sint = type == T::Sint || type == T::SScaled;
        // Packed 10:10:10:2 word in a.x: fields unsigned or sign-extended
        // (arithmetic shift of the field moved to the top).
        const std::string p = a + ".x";
        const std::string ufields = "vec4<u32>(" + p + " & 1023u, (" + p + " >> 10u) & 1023u, (" + p +
                                    " >> 20u) & 1023u, " + p + " >> 30u)";
        const std::string sfields = "vec4<i32>(i32(" + p + " << 22u) >> 22u, i32(" + p + " << 12u) >> 22u, i32(" +
                                    p + " << 2u) >> 22u, i32(" + p + ") >> 30u)";
        std::string value;
        switch (type) {
        case T::Uint: value = a; break;
        case T::Sint: value = "bitcast<vec4<u32>>(" + a + ")"; break;
        case T::UScaled:
        case T::SScaled: value = "bitcast<vec4<u32>>(vec4<f32>(" + a + "))"; break;
        case T::PackedUint: value = ufields; break;
        case T::PackedSint: value = "bitcast<vec4<u32>>(" + sfields + ")"; break;
        case T::PackedSnorm:
            value = "bitcast<vec4<u32>>(max(vec4<f32>(" + sfields +
                    ") / vec4<f32>(511.0, 511.0, 511.0, 1.0), vec4<f32>(-1.0)))";
            break;
        case T::PackedUScaled: value = "bitcast<vec4<u32>>(vec4<f32>(" + ufields + "))"; break;
        case T::PackedSScaled: value = "bitcast<vec4<u32>>(vec4<f32>(" + sfields + "))"; break;
        case T::Float: break;
        }
        // emit() declares the parameter and copies it exactly like this; a location the shader does
        // not read is left alone.
        if (!replace("@location(" + n + ") " + a + ": vec4<f32>",
                     "@location(" + n + ") " + a + ": " + (sint ? "vec4<i32>" : "vec4<u32>")))
            continue;
        if (!replace("    vin" + n + " = bitcast<vec4<u32>>(" + a + ");\n", "    vin" + n + " = " + value + ";\n"))
            return {};
    }
    return out;
}

}  // namespace lgp::gpu::wgsl
