// Analysis → lifter input (docs/design/p2-interfaces.md section 4a).
#include "lgp/recomp/lift_input.hpp"

#include <utility>

namespace lgp::recomp {

LiftFunction to_lift_function(const Function& fn) {
    LiftFunction lf;
    lf.entry = fn.entry;
    lf.blocks.reserve(fn.blocks.size());
    for (const BasicBlock& b : fn.blocks) lf.blocks.push_back({b.start, b.end});
    for (const JumpTable& jt : fn.jump_tables) lf.indirect_targets[jt.br_pc] = jt.targets;
    return lf;
}

std::vector<LiftFunction> to_lift_functions(const AnalysisResult& result) {
    std::vector<LiftFunction> out;
    out.reserve(result.functions.size());
    for (const Function& fn : result.functions) out.push_back(to_lift_function(fn));
    return out;
}

CodeReader code_reader(const LinkedImage& image) {
    // Only executable code: a block never extends into data, and a stray read
    // there would hand the lifter bytes that are not instructions.
    return [&image](uint64_t va, uint32_t& insn) {
        if ((va & 3) != 0 || !image.in_text(va) || !image.in_text(va + 3)) return false;
        insn = image.read_u32(va);
        return true;
    };
}

Lifter make_lifter(const LinkedImage& image, const AnalysisResult& result, LiftOptions options) {
    return Lifter(code_reader(image), result.function_entries, std::move(options));
}

}  // namespace lgp::recomp
