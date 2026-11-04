#include "lgp/gpu/shader/sph.h"

namespace lgp::gpu::shader {

uint32_t Sph::bits(unsigned lo, unsigned n) const {
    uint32_t v = 0;
    for (unsigned i = 0; i < n; ++i) {
        const unsigned b = lo + i;
        v |= ((words[b / 32] >> (b % 32)) & 1u) << i;
    }
    return v;
}

// Bit offsets (cla097sph.h): VTG imap generic 192..319, VTG omap generic
// 432..559 (one bit per component); PS imap generic 192..447 (two bits per
// component); PS omap target 576..607 (one bit per component), sample mask
// 608, depth 609.
bool Sph::vtg_input(unsigned i, unsigned c) const { return i < 32 && c < 4 && bits(192 + i * 4 + c, 1); }
bool Sph::vtg_output(unsigned i, unsigned c) const { return i < 32 && c < 4 && bits(432 + i * 4 + c, 1); }
PixelImap Sph::ps_input(unsigned i, unsigned c) const {
    if (i >= 32 || c >= 4) return PixelImap::Unused;
    return static_cast<PixelImap>(bits(192 + (i * 4 + c) * 2, 2));
}
bool Sph::ps_output(unsigned t, unsigned c) const { return t < 8 && c < 4 && bits(576 + t * 4 + c, 1); }
bool Sph::ps_output_sample_mask() const { return bits(608, 1) != 0; }
bool Sph::ps_output_depth() const { return bits(609, 1) != 0; }

bool parse_sph(std::span<const uint8_t> data, Sph& out, std::string& error) {
    if (data.size() < kSphBytes) {
        error = "input shorter than the 0x50-byte shader program header";
        return false;
    }
    for (size_t i = 0; i < out.words.size(); ++i) {
        out.words[i] = uint32_t{data[i * 4]} | uint32_t{data[i * 4 + 1]} << 8 | uint32_t{data[i * 4 + 2]} << 16 |
                       uint32_t{data[i * 4 + 3]} << 24;
    }
    out.sph_type = out.bits(0, 5);
    out.version = out.bits(5, 5);
    const uint32_t shader_type = out.bits(10, 4);
    out.kills_pixels = out.bits(15, 1) != 0;
    out.local_memory_size = out.bits(32, 24);
    switch (shader_type) {
    case 1: out.stage = Stage::Vertex; break;
    case 2: out.stage = Stage::TessControl; break;
    case 3: out.stage = Stage::TessEval; break;
    case 4: out.stage = Stage::Geometry; break;
    case 5: out.stage = Stage::Fragment; break;
    default: out.stage = Stage::Unknown; break;
    }
    if (out.version != 3) {
        error = "unsupported SPH version " + std::to_string(out.version) + " (expected 3)";
        return false;
    }
    const bool ps = out.stage == Stage::Fragment;
    if ((ps && out.sph_type != 2) || (!ps && out.sph_type != 1) || out.stage == Stage::Unknown) {
        error = "inconsistent SPH type " + std::to_string(out.sph_type) + " / shader type " +
                std::to_string(shader_type);
        return false;
    }
    return true;
}

}  // namespace lgp::gpu::shader
