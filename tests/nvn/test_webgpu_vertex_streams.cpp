// More vertex streams than the device has vertex buffers (Chrome allows 8,
// LGP draws with 9, mostly per instance): the WebGPU backend merges streams
// with the same step into one interleaved buffer (VertexMerge in
// webgpu_backend.cpp). A draw with 9 streams of different strides, offsets
// and formats, one with stride 0, must give the same colour with all
// buffers, with 8 and with 4 (LGP_WEBGPU_MAX_VERTEX_BUFFERS): once with all
// streams per vertex, once with streams 1-8 per instance (divisor 1, drawn
// from base instance 1, element 0 is garbage), both again with streams 1-8
// all of stride 0 (a merged buffer of one element read by every vertex or
// instance), and once per instance with streams 2 and 3 written by a
// dispatch in the same list (guest memory holds garbage there; those streams
// must not be merged, the merge copies on the CPU, and unexcluded they would
// be: the narrowest), and once per instance after a dispatch with zero
// groups over streams 1-8 (writes nothing: every stream may be merged).
// Runs natively and in the browser.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "../runtime/check.hpp"
#include "lgp/gpu/webgpu/webgpu_backend.hpp"

namespace nvn = lgp::gpu::nvn;
namespace pw = lgp::platform::webgpu;
using lgp::gpu::webgpu::WebGpuBackend;

namespace {

constexpr int kSkip = 77;

class BlockMemory final : public nvn::GuestMemoryView {
public:
    static constexpr uint64_t kBase = 0x1000'0000;
    std::vector<uint8_t> bytes = std::vector<uint8_t>(0x1000, 0);
    bool read(uint64_t va, void* dst, uint64_t size) const override {
        if (va < kBase || va - kBase + size > bytes.size())
            return false;
        std::memcpy(dst, bytes.data() + (va - kBase), size);
        return true;
    }
    bool write(uint64_t va, const void* src, uint64_t size) const override {
        if (va < kBase || va - kBase + size > bytes.size())
            return false;
        std::memcpy(const_cast<uint8_t*>(bytes.data()) + (va - kBase), src, size);
        return true;
    }
    uint64_t put(uint64_t offset, const void* data, size_t size) {
        std::memcpy(bytes.data() + offset, data, size);
        return kBase + offset;
    }
};

const char* kVertexWgsl =
    "struct O { @builtin(position) p: vec4<f32>, @location(0) c: vec4<f32> };\n"
    "@vertex fn main(@location(0) a0: vec4<f32>, @location(1) a1: vec4<f32>, @location(2) a2: vec4<f32>,\n"
    "                @location(3) a3: vec4<f32>, @location(4) a4: vec4<f32>, @location(5) a5: vec4<f32>,\n"
    "                @location(6) a6: vec4<f32>, @location(7) a7: vec4<f32>, @location(8) a8: vec4<f32>) -> O {\n"
    "  return O(a0, vec4<f32>(a1.y + a2.z, a3.x + a4.x + a5.x, a6.x + a7.x + a8.x, a1.w + a4.w));\n"
    "}\n";
// Writes element 1 of streams 2 (Unorm8x4, z = 128) and 3 (Float32 0.1),
// both stride 4, at guest 0x300 and 0x400 (see draw()).
const char* kComputeWgsl =
    "@group(2) @binding(0) var<storage, read_write> gm0: array<u32>;\n"
    "@compute @workgroup_size(1) fn main() {\n"
    "  gm0[1] = 0x00800000u; gm0[65] = bitcast<u32>(0.1);\n"
    "}\n";
const char* kFragmentWgsl = "@fragment fn main(@location(0) c: vec4<f32>) -> @location(0) vec4<f32> { return c; }\n";

void set_env(const char* name, const char* value) {
#if defined(_MSC_VER)
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

struct Result {
    std::vector<uint8_t> px;
    WebGpuBackend::Stats stats;
    uint32_t written = 0;  // guest word the dispatch wrote (element 1 of stream 2)
};

enum class Mode { PerVertex, PerInstance, FromDispatch, EmptyDispatch };

Result draw(pw::WebGpuRenderer& renderer, Mode mode, bool constant) {
    const bool instanced = mode != Mode::PerVertex;
    BlockMemory memory;
    WebGpuBackend backend(renderer);
    backend.set_guest_memory(&memory);
    nvn::TextureDesc td;
    td.target = uint32_t(nvn::TextureTarget::Target2D);
    td.width = td.height = 4;
    td.format = uint32_t(nvn::Format::RGBA8);
    backend.create_texture(1, td);
    backend.create_program(1, nvn::ProgramDesc{kVertexWgsl, kFragmentWgsl});

    nvn::CommandList list;
    nvn::CmdSetVertexLayout layout;
    using F = nvn::VertexFormat;
    // stream: stride, attribute offset, format, value bytes per element (3
    // vertices, or instances 0 and 1 with the value in element 1)
    const float pos[12] = {-1, -1, 0, 1, 3, -1, 0, 1, -1, 3, 0, 1};
    std::vector<uint64_t> addresses;
    auto stream = [&](uint32_t stride, uint32_t offset, F format, const void* element, size_t size) {
        if (constant)
            stride = 0;
        const uint32_t s = uint32_t(layout.strides.size());
        layout.strides.push_back(stride);
        layout.attribs.push_back({s, format, s, offset});
        layout.divisors.resize(s + 1, 0);
        layout.divisors[s] = instanced ? 1 : 0;
        std::vector<uint8_t> data(stride == 0 ? offset + size : 3 * size_t(stride), 0xEE);
        for (uint32_t v = instanced && stride != 0 ? 1u : 0u; v < (stride == 0 ? 1u : instanced ? 2u : 3u); ++v)
            std::memcpy(data.data() + v * stride + offset, element, size);
        addresses.push_back(memory.put(0x100 * (s + 1), data.data(), data.size()));
        list.commands.push_back(nvn::CmdBindVertexBuffer{s, addresses.back(), data.size()});
    };
    // Stream 0: positions (per vertex), the rest one value each, the same for every vertex.
    {
        layout.strides.push_back(16);
        layout.attribs.push_back({0, F::Float32x4, 0, 0});
        list.commands.push_back(nvn::CmdBindVertexBuffer{0, memory.put(0x80, pos, sizeof(pos)), sizeof(pos)});
    }
    const float v1[2] = {0.7f, 0.25f};
    const uint8_t v2[4] = {0, 0, 128, 0};
    const float v3 = 0.1f, v4[3] = {0.2f, 0.0f, 0.0f}, v5 = 0.3f, v6 = 0.05f, v7 = 0.15f, v8 = 0.2f;
    stream(12, 4, F::Float32x2, v1, sizeof(v1));  // a1 = (0.7, 0.25, 0, 1)
    stream(4, 0, F::Unorm8x4, v2, sizeof(v2));
    stream(4, 0, F::Float32, &v3, 4);
    stream(12, 0, F::Float32x3, v4, sizeof(v4));  // a4.w = 1: a1.w + a4.w = 2 -> alpha 255
    stream(8, 4, F::Float32, &v5, 4);
    stream(0, 0, F::Float32, &v6, 4);  // stride 0: every vertex reads element 0
    stream(4, 0, F::Float32, &v7, 4);
    stream(4, 0, F::Float32, &v8, 4);
    list.commands.insert(list.commands.begin(), layout);
    list.commands.insert(list.commands.begin(), nvn::CmdBindProgram{1});
    if (mode == Mode::FromDispatch || mode == Mode::EmptyDispatch) {
        const bool empty = mode == Mode::EmptyDispatch;
        nvn::ProgramDesc cs;
        cs.compute_wgsl = kComputeWgsl;
        cs.compute_global_memory = true;
        backend.create_program(2, cs);
        if (!empty)
            std::memset(memory.bytes.data() + 0x300, 0xEE, 0x200);  // only the dispatch has the values
        const nvn::Command compute[] = {
            nvn::CmdBindProgram{2, true},
            nvn::CmdBindStorageBuffer{nvn::ShaderStage::Compute, 0, BlockMemory::kBase + (empty ? 0x200 : 0x300),
                                      empty ? 0x800u : 0x200u},
            nvn::CmdDispatchCompute{{empty ? 0u : 1u, 1, 1}, 0}};
        list.commands.insert(list.commands.begin(), std::begin(compute), std::end(compute));
    }
    list.commands.push_back(nvn::CmdSetRenderTargets{{1}, 0});
    list.commands.push_back(nvn::CmdSetViewports{0, {0, 0, 4, 4}});
    list.commands.push_back(nvn::CmdClearColor{0, {0, 0, 0, 0}, nvn::kClearColorMaskRGBA});
    list.commands.push_back(nvn::CmdDraw{nvn::Topology::Triangles, 0, 3, instanced ? 1u : 0u, 1});
    backend.execute(list);
    Result r;
    CHECK(backend.read_texture(1, &r.px, nullptr, nullptr));
    r.stats = backend.stats();
    // read_texture waits for the dispatch results to be back in guest memory.
    std::memcpy(&r.written, memory.bytes.data() + 0x304, 4);
    return r;
}

bool near(int a, int b) { return std::abs(a - b) <= 1; }

}  // namespace

int main() {
    pw::RendererOptions options;
    options.size = {64, 64};
    pw::WebGpuRenderer renderer(options);
    if (!renderer.ok()) {
        std::printf("skipped: %s\n", renderer.error().c_str());
        return kSkip;
    }
    for (const bool constant : {false, true})
    for (const Mode mode : {Mode::PerVertex, Mode::PerInstance, Mode::FromDispatch, Mode::EmptyDispatch})
    for (const char* limit : {"16", "8", "4"}) {
        // From a dispatch, 4 buffers would need streams 2 and 3 merged too;
        // the dispatch writes element 1 (stride 4).
        if (mode == Mode::FromDispatch && (constant || std::strcmp(limit, "4") == 0))
            continue;
        if (mode == Mode::EmptyDispatch && constant)
            continue;
        std::printf("%s%s, vertex buffers: at most %s\n",
                    mode == Mode::PerVertex     ? "per vertex"
                    : mode == Mode::PerInstance ? "per instance"
                    : mode == Mode::FromDispatch ? "from a dispatch"
                                                 : "after an empty dispatch",
                    constant ? ", streams 1-8 stride 0" : "", limit);
        set_env("LGP_WEBGPU_MAX_VERTEX_BUFFERS", limit);
        const Result r = draw(renderer, mode, constant);
        CHECK_EQ(r.stats.draws, 1u);
        if (mode == Mode::FromDispatch) {
            CHECK_EQ(r.stats.compute_vertex_streams, 2u);
            CHECK_EQ(r.stats.writebacks, 1u);
            CHECK_EQ(r.written, 0x00800000u);
        }
        if (mode == Mode::EmptyDispatch)
            CHECK_EQ(r.stats.compute_vertex_streams, 0u);
        CHECK_EQ(r.stats.skipped_commands, 0u);
        CHECK_EQ(r.stats.invalid_pipelines, 0u);
        CHECK_EQ(r.px.size(), size_t(4 * 4 * 4));
        if (r.px.size() == 4 * 4 * 4) {
            // (0.25 + 128/255, 0.1 + 0.2 + 0.3, 0.05 + 0.15 + 0.2, 1) -> (192, 153, 102, 255)
            std::printf("pixel %u %u %u %u\n", r.px[0], r.px[1], r.px[2], r.px[3]);
            CHECK(near(r.px[0], 192) && near(r.px[1], 153) && near(r.px[2], 102) && r.px[3] == 255);
        }
        for (const auto& [reason, n] : r.stats.skip_reasons) std::printf("  skipped: %s (%llu)\n", reason.c_str(),
                                                                        (unsigned long long)n);
    }
    // Backpressure at present (here in the browser too): with one frame in
    // flight, every present waits for the GPU and returns with none running.
    {
        set_env("LGP_WEBGPU_FRAMES_IN_FLIGHT", "1");
        WebGpuBackend backend(renderer);
        nvn::TextureDesc td;
        td.target = uint32_t(nvn::TextureTarget::Target2D);
        td.width = td.height = 4;
        td.format = uint32_t(nvn::Format::RGBA8);
        backend.create_texture(1, td);
        nvn::CommandList list;
        list.commands.push_back(nvn::CmdSetRenderTargets{{1}, 0});
        list.commands.push_back(nvn::CmdClearColor{0, {0, 0, 1, 1}, nvn::kClearColorMaskRGBA});
        for (int i = 0; i < 4; ++i) {
            backend.execute(list);
            backend.present({1, {}});
            CHECK_EQ(backend.stats().frames_in_flight, 0u);
        }
        const auto st = backend.stats();
        std::printf("presents waited %llu (%llu us)\n", (unsigned long long)st.frame_waits,
                    (unsigned long long)st.frame_wait_us);
        CHECK_EQ(st.frames_in_flight_max, 1u);
    }
    CHECK_EQ(renderer.error_count(), 0u);
    return finish();
}
