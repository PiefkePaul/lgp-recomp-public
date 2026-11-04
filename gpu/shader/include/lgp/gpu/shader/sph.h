// Shader Program Header (SPH), version 3 (Maxwell/Pascal): the 0x50-byte
// header in front of every Maxwell graphics shader. Field layout from
// NVIDIA's open-gpu-doc (classes/3d/cla097sph.h, MIT).
#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>

namespace lgp::gpu::shader {

constexpr size_t kSphBytes = 0x50;

enum class Stage : uint8_t { Unknown, Vertex, TessControl, TessEval, Geometry, Fragment, Compute };

// Fragment input interpolation per component (SPH PS imap, 2 bits each).
enum class PixelImap : uint8_t { Unused = 0, Constant = 1, Perspective = 2, ScreenLinear = 3 };

struct Sph {
    std::array<uint32_t, 20> words{};
    uint32_t sph_type = 0;   // 1 = VTG, 2 = PS
    uint32_t version = 0;    // 3 on Maxwell
    Stage stage = Stage::Unknown;
    bool kills_pixels = false;
    uint32_t local_memory_size = 0;

    // Generic vector i (attribute a[0x80 + 16 * i]), component c.
    bool vtg_input(unsigned i, unsigned c) const;
    bool vtg_output(unsigned i, unsigned c) const;
    PixelImap ps_input(unsigned i, unsigned c) const;
    // Fragment colour target t, component c (omap target mask).
    bool ps_output(unsigned t, unsigned c) const;
    bool ps_output_depth() const;
    bool ps_output_sample_mask() const;

    uint32_t bits(unsigned lo, unsigned n) const;
};

// Parses the first kSphBytes; false (with an error message) if the header is
// not a version-3 VTG/PS header.
bool parse_sph(std::span<const uint8_t> data, Sph& out, std::string& error);

}  // namespace lgp::gpu::shader
