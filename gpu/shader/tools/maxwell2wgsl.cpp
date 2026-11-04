// maxwell2wgsl: translates a Maxwell shader to WGSL: graphics shaders (SPH + code)
// and compute shaders (code only, NVN keeps their parameters outside the code).
//
//   maxwell2wgsl [--disasm] [-o out.wgsl] [--link-fs fs.bin|fs.hex]
//                [--interp LOC=perspective|linear|flat]...
//                [--compute] [--workgroup X,Y,Z] [--shared BYTES] [--local BYTES]
//                [--storage-desc OFFSET [--uniform-desc OFFSET] [--move-slot ID=E]...]
//                <shader.bin | shader.hex>
//
// --storage-desc: compute shaders take storage buffer k's descriptor from
// c0[OFFSET + 16 k] (NVN: 0x310); accesses traced back to one descriptor go
// to their slot directly (wgsl::Options::storage_desc_offset). Prints how
// many accesses were resolved on stderr. --uniform-desc: likewise for
// constant buffers read through global memory, uniform block s's descriptor
// at c0[OFFSET + 16 s] (NVN: 0x210). --move-slot: target ID (slot k = k,
// uniform block s = 16 + s) reads table entry E instead
// (wgsl::move_static_slots, as the backend does when one range is bound twice).
//
// WebGPU needs matching interpolation on both sides of a varying, but Maxwell
// stores it only in the fragment shader's SPH: translate a vertex shader with
// --link-fs (the fragment shader it is drawn with) or explicit --interp.
//
// Input is either the raw binary (0x50-byte SPH followed by code, as it sits
// in a shader container) or the text format used by the test shaders
// (tests/gpu/shaders/*.hex): one hex word per line, '#' starts a comment,
// "sph" lines carry 32-bit header words, other lines 64-bit code words. A
// "compute X Y Z SHARED" line marks a compute shader (no SPH) and gives its
// workgroup size and shared memory bytes, as NVN's program control data would.
// Output from real game shaders is derived game data: keep it in
// $LGP_PRIVATE_DIR, never in the repository.
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "lgp/gpu/shader/ir.h"
#include "lgp/gpu/shader/maxwell.h"
#include "lgp/gpu/shader/sph.h"
#include "lgp/gpu/shader/wgsl.h"

namespace {

bool ends_with(const std::string& s, const char* suffix) {
    const size_t n = std::strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

struct ComputeInfo {
    bool present = false;
    uint32_t size[3] = {1, 1, 1};
    uint32_t shared = 0;
};

bool read_hex(std::istream& in, std::vector<uint8_t>& out, std::string& error, ComputeInfo& compute) {
    std::string line;
    int lineno = 0;
    auto put = [&](uint64_t v, int bytes) {
        for (int i = 0; i < bytes; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
    };
    while (std::getline(in, line)) {
        ++lineno;
        if (auto p = line.find('#'); p != std::string::npos) line.resize(p);
        std::istringstream ls(line);
        std::string tok;
        if (!(ls >> tok)) continue;
        try {
            if (tok == "sph") {
                while (ls >> tok) put(std::stoull(tok, nullptr, 16), 4);
            } else if (tok == "compute") {
                compute.present = true;
                for (uint32_t& v : compute.size) {
                    ls >> tok;
                    v = static_cast<uint32_t>(std::stoul(tok, nullptr, 0));
                }
                ls >> tok;
                compute.shared = static_cast<uint32_t>(std::stoul(tok, nullptr, 0));
            } else {
                put(std::stoull(tok, nullptr, 16), 8);
            }
        } catch (const std::exception&) {
            error = "line " + std::to_string(lineno) + ": bad hex value '" + tok + "'";
            return false;
        }
    }
    return true;
}

bool load(const std::string& path, std::vector<uint8_t>& data, ComputeInfo& compute) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        std::fprintf(stderr, "error: cannot open %s\n", path.c_str());
        return false;
    }
    if (ends_with(path, ".hex")) {
        std::string error;
        if (!read_hex(f, data, error, compute)) {
            std::fprintf(stderr, "error: %s: %s\n", path.c_str(), error.c_str());
            return false;
        }
    } else {
        data.assign(std::istreambuf_iterator<char>(f), {});
    }
    return true;
}

constexpr const char* kUsage =
    "usage: maxwell2wgsl [--disasm] [-o out.wgsl] [--link-fs fs] [--interp LOC=perspective|linear|flat]...\n"
    "                    [--int-input LOC=u|i|us|is|pu|pi|psn|pus|pis]... [--swizzle]\n"
    "                    [--compute] [--workgroup X,Y,Z] [--shared BYTES] [--local BYTES]\n"
    "                    [--storage-desc OFFSET [--uniform-desc OFFSET] [--move-slot ID=E]...]\n"
    "                    <shader.bin|shader.hex>\n";

bool parse_u32(const std::string& s, uint32_t& v) {
    try {
        size_t n = 0;
        v = static_cast<uint32_t>(std::stoul(s, &n, 0));
        return n == s.size();
    } catch (const std::exception&) {
        return false;
    }
}

}  // namespace

int main(int argc, char** argv) {
    namespace wgsl = lgp::gpu::wgsl;
    bool disasm = false, compute = false;
    bool have_wg = false, have_shared = false;
    std::string input, output, link_fs;
    wgsl::Options options;
    std::map<uint32_t, wgsl::Interp> explicit_interp;
    std::map<uint32_t, wgsl::VertexInputType> int_inputs;
    std::map<uint32_t, uint32_t> moved_slots;  // --move-slot
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--disasm") {
            disasm = true;
        } else if (a == "-o" && i + 1 < argc) {
            output = argv[++i];
        } else if (a == "--compute") {
            compute = true;
        } else if (a == "--workgroup" && i + 1 < argc) {
            std::istringstream ws(argv[++i]);
            std::string part;
            for (uint32_t& v : options.workgroup_size)
                if (!std::getline(ws, part, ',') || !parse_u32(part, v) || v == 0) {
                    std::fprintf(stderr, "error: bad --workgroup '%s'\n", argv[i]);
                    return 2;
                }
            have_wg = true;
        } else if ((a == "--shared" || a == "--local") && i + 1 < argc) {
            if (!parse_u32(argv[++i], a == "--shared" ? options.shared_memory_bytes : options.local_memory_bytes)) {
                std::fprintf(stderr, "error: bad %s '%s'\n", a.c_str(), argv[i]);
                return 2;
            }
            have_shared |= a == "--shared";
        } else if (a == "--link-fs" && i + 1 < argc) {
            link_fs = argv[++i];
        } else if (a == "--interp" && i + 1 < argc) {
            const std::string spec = argv[++i];
            const auto eq = spec.find('=');
            const std::string mode = eq == std::string::npos ? "" : spec.substr(eq + 1);
            unsigned long loc = 0;
            try {
                loc = std::stoul(spec.substr(0, eq));
            } catch (const std::exception&) {
                loc = 32;
            }
            if (loc >= 32 || (mode != "perspective" && mode != "linear" && mode != "flat")) {
                std::fprintf(stderr, "error: bad --interp '%s'\n", spec.c_str());
                return 2;
            }
            explicit_interp[static_cast<uint32_t>(loc)] = mode == "flat"     ? wgsl::Interp::Flat
                                                         : mode == "linear" ? wgsl::Interp::Linear
                                                                            : wgsl::Interp::Perspective;
        } else if (a == "--storage-desc" && i + 1 < argc) {
            if (!parse_u32(argv[++i], options.storage_desc_offset)) {
                std::fprintf(stderr, "error: bad --storage-desc '%s'\n", argv[i]);
                return 2;
            }
        } else if (a == "--uniform-desc" && i + 1 < argc) {
            if (!parse_u32(argv[++i], options.uniform_desc_offset)) {
                std::fprintf(stderr, "error: bad --uniform-desc '%s'\n", argv[i]);
                return 2;
            }
        } else if (a == "--move-slot" && i + 1 < argc) {
            const std::string spec = argv[++i];
            const auto eq = spec.find('=');
            uint32_t k = 0, j = 0;
            if (eq == std::string::npos || !parse_u32(spec.substr(0, eq), k) || !parse_u32(spec.substr(eq + 1), j)) {
                std::fprintf(stderr, "error: bad --move-slot '%s'\n", spec.c_str());
                return 2;
            }
            moved_slots[k] = j;
        } else if (a == "--swizzle") {
            options.texture_swizzle = true;
        } else if (a == "--int-input" && i + 1 < argc) {
            // Vertex input LOC fed by an integer vertex format (wgsl::retype_vertex_inputs).
            const std::string spec = argv[++i];
            const auto eq = spec.find('=');
            const std::string type = eq == std::string::npos ? "" : spec.substr(eq + 1);
            // u/i raw integers, us/is integers read as float, p* packed 10:10:10:2
            // (pu/pi integers, psn snorm, pus/pis integers read as float).
            static const std::map<std::string, wgsl::VertexInputType> kKinds = {
                {"u", wgsl::VertexInputType::Uint},
                {"i", wgsl::VertexInputType::Sint},
                {"us", wgsl::VertexInputType::UScaled},
                {"is", wgsl::VertexInputType::SScaled},
                {"pu", wgsl::VertexInputType::PackedUint},
                {"pi", wgsl::VertexInputType::PackedSint},
                {"psn", wgsl::VertexInputType::PackedSnorm},
                {"pus", wgsl::VertexInputType::PackedUScaled},
                {"pis", wgsl::VertexInputType::PackedSScaled},
            };
            uint32_t loc = 0;
            const auto kind = kKinds.find(type);
            if (eq == std::string::npos || !parse_u32(spec.substr(0, eq), loc) || loc >= 32 || kind == kKinds.end()) {
                std::fprintf(stderr, "error: bad --int-input '%s'\n", spec.c_str());
                return 2;
            }
            int_inputs[loc] = kind->second;
        } else if (!a.empty() && a[0] != '-') {
            input = a;
        } else {
            std::fputs(kUsage, stderr);
            return 2;
        }
    }
    if (input.empty()) {
        std::fputs(kUsage, stderr);
        return 2;
    }
    std::vector<uint8_t> data;
    ComputeInfo cinfo;
    if (!load(input, data, cinfo)) return 1;
    if (cinfo.present) {
        compute = true;
        if (!have_wg)
            for (int k = 0; k < 3; ++k) options.workgroup_size[static_cast<size_t>(k)] = cinfo.size[k];
        if (!have_shared) options.shared_memory_bytes = cinfo.shared;
    }
    if (!link_fs.empty()) {
        std::vector<uint8_t> fs;
        ComputeInfo unused;
        if (!load(link_fs, fs, unused)) return 1;
        lgp::gpu::shader::Sph sph;
        std::string error;
        if (!lgp::gpu::shader::parse_sph(fs, sph, error) || sph.stage != lgp::gpu::shader::Stage::Fragment) {
            std::fprintf(stderr, "error: %s: not a fragment shader: %s\n", link_fs.c_str(), error.c_str());
            return 1;
        }
        options.vertex_output_interp = wgsl::fragment_input_interpolation(sph);
    }
    for (const auto& [loc, mode] : explicit_interp) options.vertex_output_interp[loc] = mode;

    std::ostringstream out;
    if (disasm) {
        namespace mw = lgp::gpu::maxwell;
        const size_t start = compute ? 0 : lgp::gpu::shader::kSphBytes;
        for (size_t off = start; off + 8 <= data.size(); off += 8) {
            const uint32_t addr = static_cast<uint32_t>(off - start);
            uint64_t w = 0;
            for (int i = 7; i >= 0; --i) w = (w << 8) | data[off + static_cast<size_t>(i)];
            char head[40];
            std::snprintf(head, sizeof head, "%04x: %016llx  ", addr, static_cast<unsigned long long>(w));
            out << head;
            if (mw::is_sched_slot(addr)) {
                out << "sched\n";
                continue;
            }
            const auto in = mw::decode(w);
            out << (in ? mw::disassemble(*in, addr) : std::string("???")) << "\n";
        }
    } else {
        const auto lifted = compute ? lgp::gpu::ir::lift_compute(data.data(), data.size())
                                    : lgp::gpu::ir::lift(data.data(), data.size());
        if (!lifted.ok) {
            std::fprintf(stderr, "error: %s: %s\n", input.c_str(), lifted.error.c_str());
            return 1;
        }
        const auto result = wgsl::emit(lifted.program, options);
        if (!result.ok) {
            std::fprintf(stderr, "error: %s: %s\n", input.c_str(), result.error.c_str());
            return 1;
        }
        if (options.storage_desc_offset != wgsl::kNoStorageDesc)
            std::fprintf(stderr, "global memory accesses: %u to their slot, %u searching the slot table\n",
                         result.bindings.gmem_static_accesses, result.bindings.gmem_dynamic_accesses);
        if (!moved_slots.empty()) {
            const std::string code = wgsl::move_static_slots(result.code, moved_slots);
            if (code.empty()) {
                std::fprintf(stderr, "error: %s: --move-slot names a target without resolved accesses\n", input.c_str());
                return 1;
            }
            out << code;
        } else if (int_inputs.empty()) {
            out << result.code;
        } else {
            const std::string code = wgsl::retype_vertex_inputs(result.code, int_inputs);
            if (code.empty()) {
                std::fprintf(stderr, "error: %s: vertex inputs cannot be retyped\n", input.c_str());
                return 1;
            }
            out << code;
        }
    }
    if (output.empty()) {
        std::cout << out.str();
    } else {
        std::ofstream o(output, std::ios::binary);
        o << out.str();
        if (!o) {
            std::fprintf(stderr, "error: cannot write %s\n", output.c_str());
            return 1;
        }
    }
    return 0;
}
