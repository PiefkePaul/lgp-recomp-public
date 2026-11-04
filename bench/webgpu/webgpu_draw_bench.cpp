// Draw-call benchmark for the NVN WebGPU backend (P6,
// docs/knowledge/webgpu-backend-draw-kosten.md). No game data: hand-written
// WGSL with the translator's binding convention (wgsl.h), constant buffers,
// vertex and index data in a simulated guest memory, small RGBA8 textures.
//
// One frame = --lists command lists with --draws draws in total. Per draw the
// list binds a per-object constant buffer (vertex bank 4) and a material
// constant buffer (fragment bank 3), every draw binds two fragment textures
// out of --textures, the program changes every --draws/--programs draws; the
// scene constant buffer (vertex bank 3), the vertex buffer and the index
// buffer are bound once per list, like a renderer that sorts by material.
// Each draw is a small quad in a grid, so GPU raster work stays negligible
// and the time is the backend's CPU cost (record, uploads, submit).
//
// Prints ms per frame (median over the measured frames; execute() alone and
// including the GPU wait) and a checksum of the target, which must not change
// when the backend is optimised.
//
// usage: lgp_webgpu_draw_bench [--draws N] [--lists N] [--frames N] [--programs N]
//                              [--textures N] [--object-bytes N] [--scene-bytes N]
//                              [--material-bytes N] [--vertex-bytes N] [--smoke]
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "lgp/gpu/webgpu/webgpu_backend.hpp"

namespace nvn = lgp::gpu::nvn;
namespace pw = lgp::platform::webgpu;
using lgp::gpu::webgpu::WebGpuBackend;

namespace {

class BlockMemory final : public nvn::GuestMemoryView {
public:
    static constexpr uint64_t kBase = 0x4000'0000;
    std::vector<uint8_t> bytes;
    explicit BlockMemory(size_t size) : bytes(size, 0) {}
    bool read(uint64_t va, void* dst, uint64_t size) const override {
        if (va < kBase || va - kBase + size > bytes.size())
            return false;
        std::memcpy(dst, bytes.data() + (va - kBase), size);
        return true;
    }
    // In place, like the kernel's view.
    bool equal(uint64_t va, const void* data, uint64_t size) const override {
        const uint8_t* p = view(va, size);
        return p != nullptr && std::memcmp(p, data, size) == 0;
    }
    const uint8_t* view(uint64_t va, uint64_t size) const override {
        return va >= kBase && va - kBase + size <= bytes.size() ? bytes.data() + (va - kBase) : nullptr;
    }
    bool write(uint64_t va, const void* src, uint64_t size) const override {
        if (va < kBase || va - kBase + size > bytes.size())
            return false;
        std::memcpy(const_cast<uint8_t*>(bytes.data()) + (va - kBase), src, size);
        return true;
    }
};

// Vertex stage: scene constants c3[0] = (scale x, scale y, -, -), object
// constants c4[0] = (offset x, offset y, -, -), c4[1] = colour.
std::string vertex_wgsl(int variant) {
    return R"(@group(0) @binding(3) var<uniform> cb3: array<vec4<u32>, 4096>;
@group(0) @binding(4) var<uniform> cb4: array<vec4<u32>, 4096>;
struct Out {
    @builtin(position) pos: vec4<f32>,
    @location(0) uv: vec2<f32>,
    @location(1) col: vec4<f32>,
};
@vertex fn main(@location(0) a0: vec4<f32>, @location(1) a1: vec4<f32>) -> Out {
    let scene = bitcast<vec4<f32>>(cb3[0]);
    let object = bitcast<vec4<f32>>(cb4[0]);
    var o: Out;
    o.pos = vec4<f32>(a0.xy * scene.xy + object.xy, 0.0, 1.0);
    o.uv = a1.xy;
    o.col = bitcast<vec4<f32>>(cb4[1]) * )" +
           std::to_string(1.0 - 0.01 * variant) + R"(;
    return o;
}
)";
}

// Fragment stage: material constants c3[0] = tint, two swizzled textures.
std::string fragment_wgsl(int variant) {
    return R"(@group(1) @binding(3) var<uniform> cb3: array<vec4<u32>, 4096>;
@group(3) @binding(0) var tex0: texture_2d<f32>;
@group(3) @binding(1) var smp0: sampler;
@group(3) @binding(2) var tex1: texture_2d<f32>;
@group(3) @binding(3) var smp1: sampler;
@group(3) @binding(4) var<uniform> tex_swizzle: array<vec4<u32>, 1>;
@fragment fn main(@location(0) uv: vec2<f32>, @location(1) col: vec4<f32>) -> @location(0) vec4<f32> {
    let tint = bitcast<vec4<f32>>(cb3[0]);
    let a = textureSampleLevel(tex0, smp0, uv, 0.0);
    let b = textureSampleLevel(tex1, smp1, uv, 0.0);
    let s = f32(tex_swizzle[0][0] & 1u) * 0.0;
    return (a * 0.5 + b * 0.5) * col * tint * )" +
           std::to_string(1.0 - 0.01 * variant) + R"( + vec4<f32>(s);
}
)";
}

uint32_t arg(int argc, char** argv, const char* name, uint32_t fallback) {
    for (int i = 1; i + 1 < argc; ++i)
        if (std::strcmp(argv[i], name) == 0)
            return uint32_t(std::strtoul(argv[i + 1], nullptr, 0));
    return fallback;
}

double median(std::vector<double> v) {
    if (v.empty())
        return 0;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

}  // namespace

int main(int argc, char** argv) {
    bool smoke = false;
    for (int i = 1; i < argc; ++i) smoke |= std::strcmp(argv[i], "--smoke") == 0;
    const uint32_t draws = std::max(1u, arg(argc, argv, "--draws", 600));
    const uint32_t lists = std::clamp(arg(argc, argv, "--lists", 4), 1u, draws);
    const uint32_t frames = std::max(1u, arg(argc, argv, "--frames", smoke ? 3 : 40));
    const uint32_t programs = std::clamp(arg(argc, argv, "--programs", 8), 1u, draws);
    const uint32_t textures = std::max(2u, arg(argc, argv, "--textures", 16));
    const uint32_t object_bytes = std::clamp(arg(argc, argv, "--object-bytes", 256), 32u, 65536u);
    const uint32_t scene_bytes = std::clamp(arg(argc, argv, "--scene-bytes", 4096), 16u, 65536u);
    const uint32_t material_bytes = std::clamp(arg(argc, argv, "--material-bytes", 256), 16u, 65536u);
    const uint32_t vertex_bytes = std::max(arg(argc, argv, "--vertex-bytes", 65536), 4u * 32u);
    constexpr uint32_t kMaterials = 32;
    constexpr uint32_t kTarget = 256;

    pw::RendererOptions options;
    options.size = {64, 64};
    pw::WebGpuRenderer renderer(options);
    if (!renderer.ok()) {
        std::printf("skipped: %s\n", renderer.error().c_str());
        return 77;
    }

    // Guest memory: scene | objects | materials | vertices | indices.
    auto align = [](uint64_t v) { return (v + 255) & ~uint64_t{255}; };
    const uint64_t scene_off = 0;
    const uint64_t object_off = align(scene_off + scene_bytes);
    const uint64_t object_stride = align(object_bytes);
    const uint64_t material_off = object_off + object_stride * draws;
    const uint64_t material_stride = align(material_bytes);
    const uint64_t vertex_off = material_off + material_stride * kMaterials;
    const uint64_t index_off = align(vertex_off + vertex_bytes);
    BlockMemory memory(size_t(index_off + 6 * 2 * 64));
    auto put = [&](uint64_t off, const void* p, size_t n) { std::memcpy(memory.bytes.data() + off, p, n); };

    const uint32_t cols = 32;
    const float cell = 2.0f / cols;
    const float scene[4] = {cell * 0.4f, cell * 0.4f, 0, 0};
    put(scene_off, scene, sizeof(scene));
    for (uint32_t d = 0; d < draws; ++d) {
        const float ox = -1.0f + cell * (float(d % cols) + 0.5f);
        const float oy = -1.0f + cell * (float((d / cols) % cols) + 0.5f);
        const float obj[8] = {ox, oy, 0, 0, 0.5f + 0.5f * float(d % 7) / 6, 1.0f, 0.5f + 0.5f * float(d % 3) / 2, 1};
        put(object_off + object_stride * d, obj, sizeof(obj));
    }
    for (uint32_t m = 0; m < kMaterials; ++m) {
        const float tint[4] = {1.0f, 0.6f + 0.4f * float(m) / kMaterials, 1.0f, 1.0f};
        put(material_off + material_stride * m, tint, sizeof(tint));
    }
    // 64 quads in the vertex buffer: position (x, y), uv (u, v) as Float32x2 each.
    for (uint32_t q = 0; q < 64; ++q) {
        const float v[4][4] = {{-1, -1, 0, 0}, {1, -1, 1, 0}, {-1, 1, 0, 1}, {1, 1, 1, 1}};
        put(vertex_off + q * sizeof(v), v, sizeof(v));
    }
    for (uint32_t q = 0; q < 64; ++q) {
        const uint16_t idx[6] = {0, 1, 2, 2, 1, 3};
        put(index_off + q * sizeof(idx), idx, sizeof(idx));
    }
    const uint64_t base = BlockMemory::kBase;

    WebGpuBackend backend(renderer);
    backend.set_guest_memory(&memory);
    for (uint32_t p = 0; p < programs; ++p) {
        nvn::ProgramDesc d;
        d.vertex_wgsl = vertex_wgsl(int(p));
        d.fragment_wgsl = fragment_wgsl(int(p));
        d.vertex_cbufs = {3, 4};
        d.fragment_cbufs = {3};
        d.fragment_textures = {0, 1};
        d.texture_swizzle = true;
        backend.create_program(1 + p, d);
    }
    nvn::TextureDesc td;
    td.target = uint32_t(nvn::TextureTarget::Target2D);
    td.format = uint32_t(nvn::Format::RGBA8);
    td.width = td.height = 8;
    std::vector<uint8_t> texels(8 * 8 * 4);
    for (uint32_t t = 0; t < textures; ++t) {
        backend.create_texture(100 + t, td);
        for (size_t i = 0; i < texels.size(); ++i) texels[i] = uint8_t((i * 37 + t * 59) & 0xFF);
        backend.write_texture(100 + t, 0, {0, 0, 8, 8}, texels.data(), 8 * 4);
    }
    nvn::TextureDesc rt = td;
    rt.width = rt.height = kTarget;
    backend.create_texture(1, rt);

    // The frame's command lists.
    std::vector<nvn::CommandList> frame(lists);
    nvn::CmdSetVertexLayout layout;
    layout.attribs = {{0, nvn::VertexFormat::Float32x2, 0, 0}, {1, nvn::VertexFormat::Float32x2, 0, 8}};
    layout.strides = {16};
    const uint32_t per_program = (draws + programs - 1) / programs;
    for (uint32_t l = 0; l < lists; ++l) {
        auto& c = frame[l].commands;
        frame[l].origin_lower_left = true;
        c.push_back(nvn::CmdSetRenderTargets{{1}, 0});
        c.push_back(nvn::CmdSetViewports{0, {0, 0, float(kTarget), float(kTarget)}});
        if (l == 0)
            c.push_back(nvn::CmdClearColor{0, {0.1f, 0.1f, 0.1f, 1.0f}, nvn::kClearColorMaskRGBA});
        c.push_back(layout);
        c.push_back(nvn::CmdBindVertexBuffer{0, base + vertex_off, vertex_bytes});
        c.push_back(nvn::CmdBindUniformBuffer{nvn::ShaderStage::Vertex, 3, base + scene_off, scene_bytes});
    }
    std::vector<bool> started(lists, false);
    for (uint32_t d = 0; d < draws; ++d) {
        const size_t l = size_t(uint64_t(d) * lists / draws);
        auto& c = frame[l].commands;
        if (d % per_program == 0 || !started[l])
            c.push_back(nvn::CmdBindProgram{1 + d / per_program});
        started[l] = true;
        c.push_back(nvn::CmdBindUniformBuffer{nvn::ShaderStage::Vertex, 4, base + object_off + object_stride * d,
                                              object_bytes});
        c.push_back(nvn::CmdBindUniformBuffer{nvn::ShaderStage::Fragment, 3,
                                              base + material_off + material_stride * (d % kMaterials),
                                              material_bytes});
        for (uint32_t slot = 0; slot < 2; ++slot) {
            nvn::CmdBindTexture bt;
            bt.slot = slot;
            bt.texture = 100 + (d * 2 + slot) % textures;
            bt.sampler.min = bt.sampler.mag = slot ? nvn::Filter::Nearest : nvn::Filter::Linear;
            c.push_back(bt);
        }
        nvn::CmdDrawIndexed di;
        di.index_count = 6;
        di.index_address = base + index_off + (d % 64) * 12;
        di.base_vertex = int32_t((d % 64) * 4);
        c.push_back(di);
    }

    using clock = std::chrono::steady_clock;
    std::vector<double> record_ms, frame_ms;
    std::vector<uint8_t> px;
    const uint32_t warmup = smoke ? 1 : 3;
    for (uint32_t f = 0; f < warmup + frames; ++f) {
        const auto t0 = clock::now();
        for (const nvn::CommandList& list : frame) backend.execute(list);
        const auto t1 = clock::now();
        backend.read_texture(1, &px, nullptr, nullptr);  // waits for the GPU
        const auto t2 = clock::now();
        if (f >= warmup) {
            record_ms.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
            frame_ms.push_back(std::chrono::duration<double, std::milli>(t2 - t0).count());
        }
    }
    uint64_t checksum = 1469598103934665603ull;
    for (uint8_t b : px) checksum = (checksum ^ b) * 1099511628211ull;

    const WebGpuBackend::Stats st = backend.stats();
    const uint64_t expected_draws = uint64_t(draws) * (warmup + frames);
    const double rec = median(record_ms), tot = median(frame_ms);
    std::printf("adapter: %s\n", renderer.adapter_name().c_str());
    std::printf("draws/frame %u, lists %u, programs %u, textures %u, cbuf bytes object %u scene %u material %u, "
                "vertex buffer %u\n",
                draws, lists, programs, textures, object_bytes, scene_bytes, material_bytes, vertex_bytes);
    std::printf("execute: %.3f ms/frame (%.2f us/draw); with GPU wait: %.3f ms/frame (%.1f fps)\n", rec,
                1000.0 * rec / draws, tot, tot > 0 ? 1000.0 / tot : 0.0);
    std::printf("cbuf uploads %llu (%.1f KiB/frame), reused %llu; vertex/index uploads %llu (%.1f KiB/frame), "
                "reused %llu, unchanged %llu; bind groups created %llu, reused %llu\n",
                (unsigned long long)st.cbuf_uploads, double(st.cbuf_upload_bytes) / 1024.0 / (warmup + frames),
                (unsigned long long)st.cbuf_reused, (unsigned long long)st.geometry_uploads,
                double(st.geometry_upload_bytes) / 1024.0 / (warmup + frames), (unsigned long long)st.geometry_reused,
                (unsigned long long)st.geometry_cached, (unsigned long long)st.bind_groups_created, (unsigned long long)st.bind_groups_reused);
    std::printf("checksum %016llx\n", (unsigned long long)checksum);
    bool ok = true;
    if (st.draws != expected_draws || st.skipped_commands != 0 || st.invalid_programs != 0 ||
        st.invalid_pipelines != 0 || renderer.error_count() != 0) {
        std::printf("FAIL: draws %llu (want %llu), skipped %llu, invalid programs %llu, pipelines %llu, errors %llu\n",
                    (unsigned long long)st.draws, (unsigned long long)expected_draws,
                    (unsigned long long)st.skipped_commands, (unsigned long long)st.invalid_programs,
                    (unsigned long long)st.invalid_pipelines, (unsigned long long)renderer.error_count());
        for (const auto& [why, n] : st.skip_reasons) std::printf("  %s: %llu\n", why.c_str(), (unsigned long long)n);
        ok = false;
    }
    // Every quad covers about 6x6 pixels of the target (grid of 32x32 cells).
    if (px.size() == size_t(kTarget) * kTarget * 4) {
        size_t drawn = 0;
        for (size_t i = 0; i < px.size(); i += 4) drawn += px[i] != px[0] || px[i + 1] != px[1] || px[i + 2] != px[2];
        const size_t cells = std::min<size_t>(draws, size_t(cols) * cols);
        std::printf("pixels drawn %zu (%zu quads)\n", drawn, cells);
        if (drawn < cells * 16) {
            std::printf("FAIL: quads missing\n");
            ok = false;
        }
    } else {
        std::printf("FAIL: target not readable\n");
        ok = false;
    }
    return ok ? 0 : 1;
}
