// Adapter from the analysis result (P2.1/P2.2) to the lifter input (P2.3),
// docs/design/p2-interfaces.md section 4a. No logic of its own: blocks and
// jump-table targets are copied, the lifter derives calls, tail calls and
// fallthrough into neighbour functions from the entry list.
#pragma once

#include <vector>

#include "lgp/recomp/cfg.hpp"
#include "lgp/recomp/lifter.hpp"
#include "lgp/recomp/link.hpp"

namespace lgp::recomp {

LiftFunction to_lift_function(const Function& fn);
std::vector<LiftFunction> to_lift_functions(const AnalysisResult& result);

// Instruction reader over the linked image for Lifter; false outside every segment.
CodeReader code_reader(const LinkedImage& image);

// Lifter over the whole analysis: knows every function entry.
Lifter make_lifter(const LinkedImage& image, const AnalysisResult& result, LiftOptions options = {});

}  // namespace lgp::recomp
