// P4.4: draws on the WebGPU backend with own Maxwell test shaders
// (tests/gpu/shaders, translated by gpu/shader at run time like the offline
// tool would). Vertex and constant buffers live in (simulated) guest memory
// and are read through GuestMemoryView, as the NVN-HLE will hand them over.
//
// vs_transform + fs_alpha_loop: the same scene as gpu.render's alpha-loop
// case (KIL alpha test, SSY/SYNC loop), here through NVN command lists and
// into an RGBA8 texture, plus a viewport in NVN's lower-left window origin.
//
// usage: lgp_test_nvn_webgpu_draw <tests/gpu/shaders dir>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "../runtime/check.hpp"
#include "lgp/gpu/nvn/hle.hpp"
#include "lgp/gpu/shader/ir.h"
#include "lgp/gpu/shader/sph.h"
#include "lgp/gpu/shader/wgsl.h"
#include "lgp/gpu/webgpu/webgpu_backend.hpp"
#include "lgp/platform/host_env.hpp"

namespace nvn = lgp::gpu::nvn;
namespace pw = lgp::platform::webgpu;
namespace wgsl = lgp::gpu::wgsl;
using lgp::gpu::webgpu::WebGpuBackend;

namespace {

// Guest memory stand-in: one block at a fixed guest address.
class BlockMemory final : public nvn::GuestMemoryView {
public:
    static constexpr uint64_t kBase = 0x2000'0000;
    std::vector<uint8_t> bytes = std::vector<uint8_t>(0x60000, 0);
    bool read(uint64_t va, void* dst, uint64_t size) const override {
        if (va < kBase || va - kBase + size > bytes.size())
            return false;
        std::memcpy(dst, bytes.data() + (va - kBase), size);
        return true;
    }
    // In place, like the kernel's view; counted.
    bool equal(uint64_t va, const void* data, uint64_t size) const override {
        ++equal_calls;
        ++equal_at[va];
        return va >= kBase && va - kBase + size <= bytes.size() &&
               std::memcmp(bytes.data() + (va - kBase), data, size) == 0;
    }
    bool write(uint64_t va, const void* src, uint64_t size) const override {
        if (va < kBase || va - kBase + size > bytes.size())
            return false;
        std::memcpy(const_cast<uint8_t*>(bytes.data()) + (va - kBase), src, size);
        return true;
    }
    mutable uint64_t equal_calls = 0;
    mutable std::map<uint64_t, uint64_t> equal_at;  // by address (textures and cached geometry both compare)
    uint64_t put(uint64_t offset, const void* data, size_t size) {
        std::memcpy(bytes.data() + offset, data, size);
        return kBase + offset;
    }
};

// Only read(): exercises GuestMemoryView::equal's default (chunked reads).
class ReadOnlyMemory final : public nvn::GuestMemoryView {
public:
    explicit ReadOnlyMemory(const BlockMemory& m) : m_(m) {}
    bool read(uint64_t va, void* dst, uint64_t size) const override { return m_.read(va, dst, size); }

private:
    const BlockMemory& m_;
};

// .hex files from tools/shader/assemble_test_shader.py: "sph" lines with
// 32-bit words, then one 64-bit instruction word per line.
std::vector<uint8_t> read_hex(const std::string& path) {
    std::ifstream in(path);
    std::vector<uint8_t> out;
    std::string line;
    auto put = [&](uint64_t v, int bytes) {
        for (int i = 0; i < bytes; ++i) out.push_back(uint8_t(v >> (8 * i)));
    };
    while (std::getline(in, line)) {
        if (auto p = line.find('#'); p != std::string::npos) line.resize(p);
        std::istringstream ls(line);
        std::string tok;
        if (!(ls >> tok)) continue;
        if (tok == "sph")
            while (ls >> tok) put(std::stoull(tok, nullptr, 16), 4);
        else if (tok == "compute")
            continue;  // workgroup and shared size: the test passes them itself
        else
            put(std::stoull(tok, nullptr, 16), 8);
    }
    return out;
}

bool translate(const std::vector<uint8_t>& bin, const wgsl::Options& options, wgsl::Result* out) {
    const auto lifted = lgp::gpu::ir::lift(bin.data(), bin.size());
    if (!lifted.ok) {
        std::printf("  lift: %s\n", lifted.error.c_str());
        return false;
    }
    *out = wgsl::emit(lifted.program, options);
    if (!out->ok)
        std::printf("  emit: %s\n", out->error.c_str());
    return out->ok;
}

uint32_t f2u(float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}

// Two triangles, attributes constant per quad: position (4), colour (4), uv (2).
void quad(std::vector<float>& v, float x0, float y0, float x1, float y1, const float rgba[4]) {
    const float corners[6][2] = {{x0, y0}, {x1, y0}, {x0, y1}, {x0, y1}, {x1, y0}, {x1, y1}};
    for (const auto& c : corners) {
        v.insert(v.end(), {c[0], c[1], 0.0f, 1.0f});
        v.insert(v.end(), rgba, rgba + 4);
        v.insert(v.end(), {0.0f, 0.0f});
    }
}

// LGP_WEBGPU_FOLD_CLEARS / LGP_WEBGPU_KEEP_PASS_SAME_TARGETS as the backend
// reads them: 0/1, unset = the platform default (on under Android).
bool pass_switch(const char* name) {
    if (const char* v = std::getenv(name); v != nullptr && *v != '\0') return std::string(v) != "0";
#if defined(__ANDROID__)
    return true;
#else
    return false;
#endif
}

bool texel_is(const std::vector<uint8_t>& px, uint32_t w, uint32_t x, uint32_t y, int r, int g, int b, int a) {
    const uint8_t* p = px.data() + (size_t(y) * w + x) * 4;
    const bool ok = std::abs(p[0] - r) <= 1 && std::abs(p[1] - g) <= 1 && std::abs(p[2] - b) <= 1 &&
                    std::abs(p[3] - a) <= 1;
    if (!ok)
        std::printf("  texel (%u,%u) = %u %u %u %u, want %d %d %d %d\n", x, y, p[0], p[1], p[2], p[3], r, g, b, a);
    return ok;
}

}  // namespace

void set_env(const char* name, const char* value) {
#if defined(_MSC_VER)
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <shader dir>\n", argv[0]);
        return 2;
    }
    const std::string dir = argv[1];
    pw::RendererOptions options;
    options.size = {64, 64};
    pw::WebGpuRenderer renderer(options);
    if (!renderer.ok()) {
        std::printf("skipped: %s\n", renderer.error().c_str());
        return 77;
    }
    std::printf("  adapter: %s\n", renderer.adapter_name().c_str());

    // Translate: the vertex shader is emitted for its fragment partner
    // (interpolation), constant buffers per stage in their own bind group.
    const std::vector<uint8_t> vs_bin = read_hex(dir + "/vs_transform.hex");
    const std::vector<uint8_t> fs_bin = read_hex(dir + "/fs_alpha_loop.hex");
    lgp::gpu::shader::Sph fs_sph;
    std::string error;
    CHECK(lgp::gpu::shader::parse_sph(fs_bin, fs_sph, error));
    wgsl::Options vs_opt, fs_opt;
    vs_opt.vertex_output_interp = wgsl::fragment_input_interpolation(fs_sph);
    vs_opt.cbuf_group = 0;
    vs_opt.texture_group = 2;
    fs_opt.cbuf_group = 1;
    fs_opt.texture_group = 3;
    wgsl::Result vs, fs;
    CHECK(translate(vs_bin, vs_opt, &vs));
    CHECK(translate(fs_bin, fs_opt, &fs));
    if (!vs.ok || !fs.ok)
        return finish();

    BlockMemory memory;
    {
        // GuestMemoryView::equal's default: several chunks, a difference in
        // the last byte, an unmapped tail.
        std::vector<uint8_t> v(10000);
        for (size_t i = 0; i < v.size(); ++i) v[i] = uint8_t(i * 7);
        const uint64_t va = memory.put(0x40000, v.data(), v.size());
        const ReadOnlyMemory ro(memory);
        CHECK(ro.equal(va, v.data(), v.size()));
        v.back() ^= 1;
        CHECK(!ro.equal(va, v.data(), v.size()));
        CHECK(!ro.equal(BlockMemory::kBase + memory.bytes.size() - 16, v.data(), 32));
    }
    // Asynchronous write-back is opt-in. LGP_WEBGPU_COMPUTE_FIRST=0 (the
    // in-order recording) runs with it; the default, compute first, with the
    // default synchronous write-back.
    const char* cf_env = std::getenv("LGP_WEBGPU_COMPUTE_FIRST");
    const bool compute_first = cf_env == nullptr || *cf_env == '\0' || std::string(cf_env) != "0";
    if (!compute_first)
        set_env("LGP_WEBGPU_ASYNC_WRITEBACK", "1");
    WebGpuBackend backend(renderer);
    backend.set_pass_diagnosis(true);  // "target set" and "store" counts
    backend.set_guest_memory(&memory);
    nvn::ProgramDesc program{vs.code, fs.code, vs.bindings.cbuf_banks, fs.bindings.cbuf_banks};
    backend.create_program(1, program);
    backend.create_program(2, {"this is not wgsl", fs.code, {}, fs.bindings.cbuf_banks});
    CHECK_EQ(backend.stats().invalid_programs, 1u);

    // c3 (vertex, NVN uniform block 0) and c0 (fragment): identity matrix with m00 = 0.5 (alpha reference for
    // the fragment shader) and m01 = 3 as integer (loop count), as in gpu.render.
    std::vector<uint32_t> cb(16, 0);
    for (int i = 0; i < 16; ++i) cb[i] = f2u(i % 5 == 0 ? 1.0f : 0.0f);
    cb[0] = f2u(0.5f);
    cb[1] = 3;
    const uint64_t cb_va = memory.put(0x0, cb.data(), cb.size() * 4);
    std::vector<float> verts;
    const float killed[4] = {1, 0.5f, 0.25f, 0.25f};  // alpha 0.25 < 0.5
    const float kept[4] = {1, 0.5f, 0.25f, 1.0f};     // rgb * (3 * 0.25)
    quad(verts, -2, 1, 0, -1, killed);
    quad(verts, 0, 1, 2, -1, kept);
    const uint64_t vb_va = memory.put(0x1000, verts.data(), verts.size() * 4);

    nvn::TextureDesc td;
    td.target = uint32_t(nvn::TextureTarget::Target2D);
    td.format = uint32_t(nvn::Format::RGBA8);
    td.width = 8;
    td.height = 8;
    backend.create_texture(1, td);
    backend.create_texture(2, td);

    nvn::CommandList list;  // lower-left origin
    nvn::CmdSetVertexLayout layout;
    layout.attribs = {{0, nvn::VertexFormat::Float32x4, 0, 0},
                      {1, nvn::VertexFormat::Float32x4, 0, 16},
                      {2, nvn::VertexFormat::Float32x2, 0, 32}};
    layout.strides = {40};
    list.commands.push_back(nvn::CmdBindProgram{1});
    list.commands.push_back(layout);
    list.commands.push_back(nvn::CmdBindVertexBuffer{0, vb_va, verts.size() * 4});
    list.commands.push_back(nvn::CmdBindUniformBuffer{nvn::ShaderStage::Vertex, nvn::kUniformBufferBankBase, cb_va, 64});
    list.commands.push_back(nvn::CmdBindUniformBuffer{nvn::ShaderStage::Fragment, 0, cb_va, 64});
    // Target 1: whole viewport (set explicitly: the list runs twice, and the
    // viewport of the first run stays in effect like NVN state).
    list.commands.push_back(nvn::CmdSetRenderTargets{{1}, 0});
    list.commands.push_back(nvn::CmdSetViewports{0, {0, 0, 8, 8}});
    list.commands.push_back(nvn::CmdClearColor{0, {0.2f, 0.2f, 0.2f, 1.0f}, nvn::kClearColorMaskRGBA});
    list.commands.push_back(nvn::CmdDraw{nvn::Topology::Triangles, 0, 12, 0, 1});
    // Target 2: viewport (0,0,8,4) = memory rows 0-3 (shown at the bottom, lower-left origin).
    list.commands.push_back(nvn::CmdSetRenderTargets{{2}, 0});
    list.commands.push_back(nvn::CmdClearColor{0, {0.2f, 0.2f, 0.2f, 1.0f}, nvn::kClearColorMaskRGBA});
    list.commands.push_back(nvn::CmdSetViewports{0, {0, 0, 8, 4}});
    list.commands.push_back(nvn::CmdDraw{nvn::Topology::Triangles, 0, 12, 0, 1});
    // The invalid program: skipped, no validation error, no abort.
    list.commands.push_back(nvn::CmdBindProgram{2});
    list.commands.push_back(nvn::CmdDraw{nvn::Topology::Triangles, 0, 12, 0, 1});
    uint64_t submits0 = 0, submit_ns0 = 0, warm0 = 0, warm_ns0 = 0;
    backend.submit_times(&submits0, &submit_ns0, &warm0, &warm_ns0);
    backend.execute(list);
    backend.execute(list);  // second time: cached pipelines
    {
        // The queue-submit time source of --time-stats counts every list.
        uint64_t submits = 0, submit_ns = 0, warm = 0, warm_ns = 0;
        backend.submit_times(&submits, &submit_ns, &warm, &warm_ns);
        CHECK(submits >= submits0 + 2);
        CHECK(submit_ns > submit_ns0);
    }

    std::vector<uint8_t> px;
    CHECK(backend.read_texture(1, &px, nullptr, nullptr));
    if (px.size() == 8 * 8 * 4) {
        CHECK(texel_is(px, 8, 2, 4, 51, 51, 51, 255));    // left quad killed by the alpha test
        CHECK(texel_is(px, 8, 5, 4, 191, 96, 48, 255));   // 0.75, 0.375, 0.1875 after the loop
        CHECK(texel_is(px, 8, 5, 0, 191, 96, 48, 255));
        CHECK(texel_is(px, 8, 5, 7, 191, 96, 48, 255));
    }
    CHECK(backend.read_texture(2, &px, nullptr, nullptr));
    if (px.size() == 8 * 8 * 4) {
        CHECK(texel_is(px, 8, 5, 5, 51, 51, 51, 255));    // memory rows 4-7 untouched
        CHECK(texel_is(px, 8, 5, 2, 191, 96, 48, 255));   // rows 0-3 (window y 0-3) drawn
        CHECK(texel_is(px, 8, 2, 1, 51, 51, 51, 255));
    }
    const WebGpuBackend::Stats st = backend.stats();
    CHECK_EQ(st.draws, 4u);
    CHECK_EQ(st.skipped_commands, 2u);
    CHECK_EQ(st.invalid_pipelines, 0u);
    CHECK_EQ(renderer.error_count(), 0u);

    // Fixed-function alpha test (NVN ColorState + SetAlphaRef, the shader's
    // lgp_alpha_func/lgp_alpha_ref): the right quad writes alpha 1.0, so
    // GREATER 0.5 keeps it, LESS 0.5 discards it, and only the reference set
    // by CmdSetAlphaRef decides GREATER 1.5.
    {
        backend.create_texture(157, td);
        struct Case {
            nvn::CompareFunc func;
            float ref;
            bool drawn;
        };
        const Case cases[] = {{nvn::CompareFunc::Greater, 0.5f, true},
                              {nvn::CompareFunc::Less, 0.5f, false},
                              {nvn::CompareFunc::Greater, 1.5f, false},
                              {nvn::CompareFunc::Always, 1.5f, true}};
        for (const Case& t : cases) {
            nvn::CommandList al;
            al.commands.push_back(nvn::CmdBindProgram{1});
            al.commands.push_back(layout);
            al.commands.push_back(nvn::CmdBindVertexBuffer{0, vb_va, verts.size() * 4});
            al.commands.push_back(
                nvn::CmdBindUniformBuffer{nvn::ShaderStage::Vertex, nvn::kUniformBufferBankBase, cb_va, 64});
            al.commands.push_back(nvn::CmdBindUniformBuffer{nvn::ShaderStage::Fragment, 0, cb_va, 64});
            al.commands.push_back(nvn::CmdSetRenderTargets{{157}, 0});
            al.commands.push_back(nvn::CmdSetViewports{0, {0, 0, 8, 8}});
            al.commands.push_back(nvn::CmdClearColor{0, {0.2f, 0.2f, 0.2f, 1.0f}, nvn::kClearColorMaskRGBA});
            al.commands.push_back(nvn::CmdSetAlphaTest{t.func});
            al.commands.push_back(nvn::CmdSetAlphaRef{t.ref});
            al.commands.push_back(nvn::CmdDraw{nvn::Topology::Triangles, 0, 12, 0, 1});
            backend.execute(al);
            CHECK(backend.read_texture(157, &px, nullptr, nullptr));
            if (px.size() == 8 * 8 * 4) {
                if (t.drawn)
                    CHECK(texel_is(px, 8, 5, 4, 191, 96, 48, 255));
                else
                    CHECK(texel_is(px, 8, 5, 4, 51, 51, 51, 255));
            }
        }
    }

    // Uploads are reused within a list (same guest range, same bytes), but a
    // CmdCopyBuffer in between changes guest memory: the draw after it must
    // see the new loop count (2 instead of 3: rgb * 0.5).
    {
        std::vector<uint32_t> cb2 = cb;
        cb2[1] = 2;
        const uint64_t cb2_va = memory.put(0x3B000, cb2.data(), cb2.size() * 4);
        memory.put(0x3C000, cb.data(), cb.size() * 4);  // restored at the start of the list
        backend.create_texture(15, td);
        nvn::CommandList cl;
        cl.commands.push_back(nvn::CmdCopyBuffer{BlockMemory::kBase + 0x3C000, cb_va, cb.size() * 4});
        cl.commands.push_back(nvn::CmdBindProgram{1});
        cl.commands.push_back(nvn::CmdSetRenderTargets{{1}, 0});
        cl.commands.push_back(nvn::CmdSetViewports{0, {0, 0, 8, 8}});
        cl.commands.push_back(nvn::CmdClearColor{0, {0.2f, 0.2f, 0.2f, 1.0f}, nvn::kClearColorMaskRGBA});
        cl.commands.push_back(nvn::CmdDraw{nvn::Topology::Triangles, 0, 12, 0, 1});
        cl.commands.push_back(nvn::CmdCopyBuffer{cb2_va, cb_va, cb2.size() * 4});
        cl.commands.push_back(nvn::CmdSetRenderTargets{{15}, 0});
        cl.commands.push_back(nvn::CmdClearColor{0, {0.2f, 0.2f, 0.2f, 1.0f}, nvn::kClearColorMaskRGBA});
        cl.commands.push_back(nvn::CmdDraw{nvn::Topology::Triangles, 0, 12, 0, 1});
        backend.execute(cl);
        CHECK(backend.read_texture(1, &px, nullptr, nullptr));
        if (px.size() == 8 * 8 * 4)
            CHECK(texel_is(px, 8, 5, 4, 191, 96, 48, 255));  // count 3
        CHECK(backend.read_texture(15, &px, nullptr, nullptr));
        if (px.size() == 8 * 8 * 4)
            CHECK(texel_is(px, 8, 5, 4, 128, 64, 32, 255));  // count 2 after the copy
        memory.put(0x0, cb.data(), cb.size() * 4);
        const WebGpuBackend::Stats st2 = backend.stats();
        CHECK(st2.cbuf_reused > 0);
        CHECK(st2.bind_groups_reused > 0);
        CHECK_EQ(renderer.error_count(), 0u);
    }
    // A compute program whose module is valid but whose pipeline is not
    // (workgroup over the limits): rejected at create_program, or with async
    // pipelines (LGP_WEBGPU_ASYNC_PIPELINES, the browser) at its first
    // dispatch; either way the dispatch is skipped and nothing reaches a submit.
    {
        nvn::ProgramDesc cs;
        cs.compute_wgsl = R"(
@group(2) @binding(0) var<storage, read_write> gm0: array<u32>;
@compute @workgroup_size(1024, 1024, 1)
fn main(@builtin(global_invocation_id) id: vec3<u32>) {
    gm0[id.x] = 1u;
}
)";
        cs.compute_global_memory = true;
        WebGpuBackend own(renderer);  // its own counters
        own.set_guest_memory(&memory);
        own.create_program(9, cs);
        const uint64_t buf = memory.put(0x3C000, verts.data(), 64);
        nvn::CommandList cl;
        cl.commands.push_back(nvn::CmdBindProgram{9, true});
        cl.commands.push_back(nvn::CmdBindStorageBuffer{nvn::ShaderStage::Compute, 0, buf, 64});
        cl.commands.push_back(nvn::CmdDispatchCompute{{1, 1, 1}, 0});
        own.execute(cl);
        own.execute(cl);  // known invalid now: no second attempt
        const WebGpuBackend::Stats st = own.stats();
        CHECK_EQ(st.dispatches, 0u);
        CHECK_EQ(st.dispatch_lists, 0u);  // skipped: nothing waits for it
        CHECK_EQ(st.invalid_programs + st.invalid_pipelines, 1u);
        CHECK_EQ(renderer.error_count(), 0u);
    }
    // A dispatch writes vertex data that a draw later in the same list reads:
    // the result goes back to guest memory only after the list, so the draw
    // must take the vertices from the dispatch's storage buffer. The compute
    // shader sets the alpha of the left quad's six vertices to 1.0, so the
    // quad the alpha test killed above is drawn.
    {
        nvn::ProgramDesc cs;
        cs.compute_wgsl = R"(
@group(2) @binding(0) var<storage, read_write> gm0: array<u32>;
@compute @workgroup_size(1)
fn main(@builtin(global_invocation_id) id: vec3<u32>) {
    gm0[id.x * 10u + 7u] = bitcast<u32>(1.0);
}
)";
        cs.compute_global_memory = true;
        backend.create_program(3, cs);
        const uint64_t cvb = memory.put(0x3D000, verts.data(), verts.size() * 4);
        nvn::CommandList cl;
        cl.commands.push_back(nvn::CmdBindProgram{3, true});
        cl.commands.push_back(nvn::CmdBindStorageBuffer{nvn::ShaderStage::Compute, 0, cvb, verts.size() * 4});
        cl.commands.push_back(nvn::CmdDispatchCompute{{6, 1, 1}, 0});
        cl.commands.push_back(nvn::CmdBindProgram{1});
        cl.commands.push_back(layout);
        cl.commands.push_back(nvn::CmdBindVertexBuffer{0, cvb, verts.size() * 4});
        cl.commands.push_back(nvn::CmdSetRenderTargets{{1}, 0});
        cl.commands.push_back(nvn::CmdSetViewports{0, {0, 0, 8, 8}});
        cl.commands.push_back(nvn::CmdClearColor{0, {0.2f, 0.2f, 0.2f, 1.0f}, nvn::kClearColorMaskRGBA});
        cl.commands.push_back(nvn::CmdDraw{nvn::Topology::Triangles, 0, 12, 0, 1});
        const WebGpuBackend::Stats before = backend.stats();
        backend.execute(cl);
        const WebGpuBackend::Stats after = backend.stats();
        CHECK_EQ(after.dispatches - before.dispatches, 1u);
        CHECK_EQ(after.compute_vertex_streams - before.compute_vertex_streams, 1u);
        // The dispatch comes first, the draw after it (Stats::draws_*_dispatch).
        CHECK_EQ(after.lists - before.lists, 1u);
        CHECK_EQ(after.dispatch_lists - before.dispatch_lists, 1u);
        CHECK_EQ(after.draws_before_dispatch - before.draws_before_dispatch, 0u);
        CHECK_EQ(after.draws_between_dispatches - before.draws_between_dispatches, 0u);
        CHECK_EQ(after.draws_after_dispatch - before.draws_after_dispatch, 1u);
        // Compute first (default): the dispatch goes ahead of the list and
        // its result is read back from there; the draw reads it all the same.
        CHECK_EQ(after.dispatches_first - before.dispatches_first, compute_first ? 1u : 0u);
        CHECK_EQ(after.dispatches_in_order - before.dispatches_in_order, compute_first ? 0u : 1u);
        CHECK_EQ(after.lists_written_back_first - before.lists_written_back_first, compute_first ? 1u : 0u);
        CHECK(backend.read_texture(1, &px, nullptr, nullptr));
        if (px.size() == 8 * 8 * 4) {
            CHECK(texel_is(px, 8, 2, 4, 191, 96, 48, 255));  // left quad: alpha 1.0 from the dispatch
            CHECK(texel_is(px, 8, 5, 4, 191, 96, 48, 255));
        }
        float alpha = 0;  // written back after the list
        CHECK(memory.read(cvb + 7 * 4, &alpha, 4));
        CHECK(alpha == 1.0f);
        // A copy after the dispatch puts the original vertices back: the
        // draw and the writeback see the copy, not the dispatch's output.
        const uint64_t orig = memory.put(0x3D800, verts.data(), verts.size() * 4);
        nvn::CommandList ol;
        ol.commands.push_back(nvn::CmdBindProgram{3, true});
        ol.commands.push_back(nvn::CmdDispatchCompute{{6, 1, 1}, 0});
        ol.commands.push_back(nvn::CmdCopyBuffer{orig, cvb, verts.size() * 4});
        ol.commands.push_back(nvn::CmdBindProgram{1});
        ol.commands.push_back(nvn::CmdClearColor{0, {0.2f, 0.2f, 0.2f, 1.0f}, nvn::kClearColorMaskRGBA});
        ol.commands.push_back(nvn::CmdDraw{nvn::Topology::Triangles, 0, 12, 0, 1});
        backend.execute(ol);
        CHECK(backend.read_texture(1, &px, nullptr, nullptr));
        if (px.size() == 8 * 8 * 4) {
            CHECK(texel_is(px, 8, 2, 4, 51, 51, 51, 255));  // left quad killed again
            CHECK(texel_is(px, 8, 5, 4, 191, 96, 48, 255));
        }
        CHECK(memory.read(cvb + 7 * 4, &alpha, 4));
        CHECK(alpha == verts[7]);
        // Compute first: the copy into the output keeps the read-back in
        // the list, after the copy.
        if (compute_first) {
            const WebGpuBackend::Stats st = backend.stats();
            CHECK_EQ(st.lists_compute_first - after.lists_compute_first, 1u);
            CHECK_EQ(st.lists_written_back_first - after.lists_written_back_first, 0u);
        }
        // A dispatch after a draw that reads the range stays in order (the
        // draw must see the first dispatch's output, not the second's).
        memory.put(0x3D000, verts.data(), verts.size() * 4);
        nvn::CommandList tl;
        tl.commands.push_back(nvn::CmdBindProgram{3, true});
        tl.commands.push_back(nvn::CmdDispatchCompute{{6, 1, 1}, 0});
        tl.commands.push_back(nvn::CmdBindProgram{1});
        tl.commands.push_back(nvn::CmdClearColor{0, {0.2f, 0.2f, 0.2f, 1.0f}, nvn::kClearColorMaskRGBA});
        tl.commands.push_back(nvn::CmdDraw{nvn::Topology::Triangles, 0, 12, 0, 1});
        tl.commands.push_back(nvn::CmdBindProgram{3, true});
        tl.commands.push_back(nvn::CmdDispatchCompute{{6, 1, 1}, 0});
        const WebGpuBackend::Stats t0 = backend.stats();
        backend.execute(tl);
        const WebGpuBackend::Stats t1 = backend.stats();
        CHECK_EQ(t1.dispatches_first - t0.dispatches_first, compute_first ? 1u : 0u);
        CHECK_EQ(t1.dispatches_in_order - t0.dispatches_in_order, compute_first ? 1u : 2u);
        CHECK_EQ(t1.lists_written_back_first - t0.lists_written_back_first, 0u);
        CHECK(backend.read_texture(1, &px, nullptr, nullptr));
        if (px.size() == 8 * 8 * 4)
            CHECK(texel_is(px, 8, 2, 4, 191, 96, 48, 255));  // left quad drawn
        CHECK(memory.read(cvb + 7 * 4, &alpha, 4));
        CHECK(alpha == 1.0f);
        // LGP_WEBGPU_BUFFER_POOL: the buffers of a finished list (storage,
        // read-back, slot tables) serve the next one with the same sizes.
        if (const char* pool = std::getenv("LGP_WEBGPU_BUFFER_POOL"); pool != nullptr && std::string(pool) == "1") {
            backend.finish();
            const WebGpuBackend::Stats b0 = backend.stats();
            memory.put(0x3D000, verts.data(), verts.size() * 4);
            backend.execute(tl);
            backend.finish();
            const WebGpuBackend::Stats b1 = backend.stats();
            CHECK(b1.buffer_pool_reused > b0.buffer_pool_reused);
            CHECK_EQ(b1.buffer_pool_created, b0.buffer_pool_created);  // nothing new for the same list
            CHECK(memory.read(cvb + 7 * 4, &alpha, 4));
            CHECK(alpha == 1.0f);
        }
        // The dispatch writes only the left quad's vertices (its storage
        // range is part of the vertex buffer): dispatch_output does not
        // hold the stream, so it is read from guest memory, where the
        // result arrives after the list. With LGP_WEBGPU_PATCH_DISPATCH_OUTPUT
        // the overlap is copied over on the GPU and the left quad is drawn.
        {
            const char* pv_env = std::getenv("LGP_WEBGPU_PATCH_DISPATCH_OUTPUT");
            const bool patch = pv_env != nullptr && *pv_env != '\0' && std::string(pv_env) != "0";
            const uint64_t pva = memory.put(0x3D400, verts.data(), verts.size() * 4);
            nvn::CommandList pl;
            pl.commands.push_back(nvn::CmdBindProgram{3, true});
            pl.commands.push_back(nvn::CmdBindStorageBuffer{nvn::ShaderStage::Compute, 0, pva, 6 * 10 * 4});
            pl.commands.push_back(nvn::CmdDispatchCompute{{6, 1, 1}, 0});
            pl.commands.push_back(nvn::CmdBindProgram{1});
            pl.commands.push_back(nvn::CmdBindVertexBuffer{0, pva, verts.size() * 4});
            pl.commands.push_back(nvn::CmdClearColor{0, {0.2f, 0.2f, 0.2f, 1.0f}, nvn::kClearColorMaskRGBA});
            pl.commands.push_back(nvn::CmdDraw{nvn::Topology::Triangles, 0, 12, 0, 1});
            const WebGpuBackend::Stats p0 = backend.stats();
            backend.execute(pl);
            const WebGpuBackend::Stats p1 = backend.stats();
            CHECK_EQ(p1.compute_vertex_streams - p0.compute_vertex_streams, patch ? 1u : 0u);
            CHECK(backend.read_texture(1, &px, nullptr, nullptr));
            if (px.size() == 8 * 8 * 4) {
                if (patch)
                    CHECK(texel_is(px, 8, 2, 4, 191, 96, 48, 255));  // left quad: alpha from the dispatch
                else  // pins the known fault without the switch
                    CHECK(texel_is(px, 8, 2, 4, 51, 51, 51, 255));  // old guest bytes: killed
                CHECK(texel_is(px, 8, 5, 4, 191, 96, 48, 255));
            }
            CHECK(memory.read(pva + 7 * 4, &alpha, 4));
            CHECK(alpha == 1.0f);  // written back after the list either way
            // A buffer copy into the vertex range (outside the dispatch's
            // output) between two draws: the second draw patches again from
            // the copied guest bytes instead of reusing the first patch.
            {
                const uint64_t cva = memory.put(0x3DA00, verts.data(), verts.size() * 4);
                const float low = killed[3];
                const uint64_t lva = memory.put(0x3DC00, &low, 4);
                nvn::CommandList cl;
                cl.commands.push_back(nvn::CmdBindProgram{3, true});
                cl.commands.push_back(nvn::CmdBindStorageBuffer{nvn::ShaderStage::Compute, 0, cva, 6 * 10 * 4});
                cl.commands.push_back(nvn::CmdDispatchCompute{{6, 1, 1}, 0});
                cl.commands.push_back(nvn::CmdBindProgram{1});
                cl.commands.push_back(nvn::CmdBindVertexBuffer{0, cva, verts.size() * 4});
                cl.commands.push_back(nvn::CmdClearColor{0, {0.2f, 0.2f, 0.2f, 1.0f}, nvn::kClearColorMaskRGBA});
                cl.commands.push_back(nvn::CmdDraw{nvn::Topology::Triangles, 0, 12, 0, 1});
                for (uint64_t v = 6; v < 12; ++v)  // right quad: alpha below the test
                    cl.commands.push_back(nvn::CmdCopyBuffer{lva, cva + v * 10 * 4 + 7 * 4, 4});
                cl.commands.push_back(nvn::CmdClearColor{0, {0.2f, 0.2f, 0.2f, 1.0f}, nvn::kClearColorMaskRGBA});
                cl.commands.push_back(nvn::CmdDraw{nvn::Topology::Triangles, 0, 12, 0, 1});
                const WebGpuBackend::Stats c0 = backend.stats();
                backend.execute(cl);
                const WebGpuBackend::Stats c1 = backend.stats();
                CHECK_EQ(c1.compute_vertex_streams - c0.compute_vertex_streams, patch ? 2u : 0u);
                CHECK(backend.read_texture(1, &px, nullptr, nullptr));
                if (px.size() == 8 * 8 * 4) {
                    if (patch)
                        CHECK(texel_is(px, 8, 2, 4, 191, 96, 48, 255));  // left quad: dispatch output
                    CHECK(texel_is(px, 8, 5, 4, 51, 51, 51, 255));       // right quad: copied alpha, killed
                }
            }
            // A second dispatch binds the whole vertex range (another storage
            // range than the first one wrote) and copies each vertex's alpha
            // to component 9: with the patch it reads the first dispatch's
            // output, without it the guest bytes from before the list.
            nvn::ProgramDesc cs2;
            cs2.compute_wgsl = R"(
@group(2) @binding(0) var<storage, read_write> gm0: array<u32>;
@compute @workgroup_size(1)
fn main(@builtin(global_invocation_id) id: vec3<u32>) {
    gm0[id.x * 10u + 9u] = gm0[id.x * 10u + 7u];
}
)";
            cs2.compute_global_memory = true;
            backend.create_program(4, cs2);
            const uint64_t sva = memory.put(0x3D600, verts.data(), verts.size() * 4);
            nvn::CommandList sl;
            sl.commands.push_back(nvn::CmdBindProgram{3, true});
            sl.commands.push_back(nvn::CmdBindStorageBuffer{nvn::ShaderStage::Compute, 0, sva, 6 * 10 * 4});
            sl.commands.push_back(nvn::CmdDispatchCompute{{6, 1, 1}, 0});
            sl.commands.push_back(nvn::CmdBindProgram{4, true});
            sl.commands.push_back(nvn::CmdBindStorageBuffer{nvn::ShaderStage::Compute, 0, sva, verts.size() * 4});
            sl.commands.push_back(nvn::CmdDispatchCompute{{6, 1, 1}, 0});
            backend.execute(sl);
            backend.finish();  // asynchronous write-back (LGP_WEBGPU_COMPUTE_FIRST=0 runs with it)
            float copied = 0;
            CHECK(memory.read(sva + 9 * 4, &copied, 4));
            CHECK(copied == (patch ? 1.0f : verts[7]));
            if (patch) {
                CHECK(memory.read(sva + 7 * 4, &alpha, 4));
                CHECK(alpha == 1.0f);  // the second write-back carries the first dispatch's result
            }
            // A range bound again after an overlapping range's dispatch: the
            // first range's buffer gets that newer output before its second
            // dispatch, and the older range gets the latest bytes before the
            // write-back.
            if (patch) {
                const uint64_t rva = memory.put(0x3DE00, verts.data(), verts.size() * 4);
                nvn::CommandList rl;
                rl.commands.push_back(nvn::CmdBindProgram{4, true});  // [9] = [7], left quad
                rl.commands.push_back(nvn::CmdBindStorageBuffer{nvn::ShaderStage::Compute, 0, rva, 6 * 10 * 4});
                rl.commands.push_back(nvn::CmdDispatchCompute{{6, 1, 1}, 0});
                rl.commands.push_back(nvn::CmdBindProgram{3, true});  // [7] = 1, through the whole range
                rl.commands.push_back(nvn::CmdBindStorageBuffer{nvn::ShaderStage::Compute, 0, rva, verts.size() * 4});
                rl.commands.push_back(nvn::CmdDispatchCompute{{6, 1, 1}, 0});
                rl.commands.push_back(nvn::CmdBindProgram{4, true});  // the left quad again
                rl.commands.push_back(nvn::CmdBindStorageBuffer{nvn::ShaderStage::Compute, 0, rva, 6 * 10 * 4});
                rl.commands.push_back(nvn::CmdDispatchCompute{{6, 1, 1}, 0});
                backend.execute(rl);
                backend.finish();
                CHECK(memory.read(rva + 9 * 4, &copied, 4));
                CHECK(copied == 1.0f);  // read the whole-range dispatch's alpha
                CHECK(memory.read(rva + 7 * 4, &alpha, 4));
                CHECK(alpha == 1.0f);
            }
            backend.destroy_program(4);
        }
        // Pass causes (Stats::pass_reasons): a dispatch between two draws
        // that touches nothing the draws use. In order it ends the draw
        // pass; ahead it does so only without LGP_WEBGPU_KEEP_PASS_ON_DISPATCH.
        {
            const char* keep_env = std::getenv("LGP_WEBGPU_KEEP_PASS_ON_DISPATCH");
#if defined(__ANDROID__)
            const bool keep_default = true;
#else
            const bool keep_default = false;
#endif
            const bool keep = compute_first && (keep_env != nullptr && *keep_env != '\0' ? std::string(keep_env) != "0"
                                                                                          : keep_default);
            // LGP_WEBGPU_FOLD_CLEARS: the clear becomes the first draw pass's load op.
            const bool fold = pass_switch("LGP_WEBGPU_FOLD_CLEARS");
            const bool keep_same = pass_switch("LGP_WEBGPU_KEEP_PASS_SAME_TARGETS");
            const uint64_t other = memory.put(0x3D800, verts.data(), verts.size() * 4);
            memory.put(0x3D000, verts.data(), verts.size() * 4);
            nvn::CommandList pl;
            pl.commands.push_back(nvn::CmdBindProgram{1});
            pl.commands.push_back(nvn::CmdBindVertexBuffer{0, cvb, verts.size() * 4});  // blocks above bind others
            pl.commands.push_back(nvn::CmdClearColor{0, {0.2f, 0.2f, 0.2f, 1.0f}, nvn::kClearColorMaskRGBA});
            pl.commands.push_back(nvn::CmdDraw{nvn::Topology::Triangles, 0, 6, 0, 1});
            pl.commands.push_back(nvn::CmdBindProgram{3, true});
            pl.commands.push_back(nvn::CmdBindStorageBuffer{nvn::ShaderStage::Compute, 0, other, verts.size() * 4});
            pl.commands.push_back(nvn::CmdDispatchCompute{{6, 1, 1}, 0});
            pl.commands.push_back(nvn::CmdBindProgram{1});
            pl.commands.push_back(nvn::CmdDraw{nvn::Topology::Triangles, 6, 6, 0, 1});
            // The same targets again: with LGP_WEBGPU_KEEP_PASS_SAME_TARGETS
            // the pass stays open for the next draw.
            pl.commands.push_back(nvn::CmdSetRenderTargets{{1}, 0});
            pl.commands.push_back(nvn::CmdDraw{nvn::Topology::Triangles, 6, 6, 0, 1});
            const WebGpuBackend::Stats p0 = backend.stats();
            backend.execute(pl);
            const WebGpuBackend::Stats p1 = backend.stats();
            auto cause = [&](const char* why) {
                const auto a = p1.pass_reasons.find(why), b = p0.pass_reasons.find(why);
                return (a != p1.pass_reasons.end() ? a->second : 0) - (b != p0.pass_reasons.end() ? b->second : 0);
            };
            CHECK_EQ(p1.dispatches_first - p0.dispatches_first, compute_first ? 1u : 0u);
            CHECK_EQ(cause("own pass: colour clear"), fold ? 0u : 1u);
            CHECK_EQ(cause("folded: colour clear (no own pass)"), fold ? 1u : 0u);
            CHECK_EQ(cause("draw pass ended: dispatch (ahead)"), compute_first && !keep ? 1u : 0u);
            CHECK_EQ(cause("draw pass ended: dispatch (in order)"), compute_first ? 0u : 1u);
            CHECK_EQ(cause("draw pass ended: render targets set again (same)"), keep_same ? 0u : 1u);
            CHECK_EQ(cause("kept: render targets set again (same)"), keep_same ? 1u : 0u);
            CHECK_EQ(cause("draw pass ended: end of list"), 1u);
            const uint64_t draw_passes = (keep ? 1u : 2u) + (keep_same ? 0u : 1u);
            CHECK_EQ(p1.draw_passes - p0.draw_passes, draw_passes);
            CHECK_EQ(p1.render_passes - p0.render_passes, draw_passes + (fold ? 0u : 1u));
            CHECK_EQ(p1.attachment_sets_new - p0.attachment_sets_new, 0u);  // target 1 was drawn into before
            CHECK(backend.read_texture(1, &px, nullptr, nullptr));
            if (px.size() == 8 * 8 * 4) {
                CHECK(texel_is(px, 8, 2, 4, 51, 51, 51, 255));  // cvb holds the original vertices (the dispatch wrote `other`): left quad killed
                CHECK(texel_is(px, 8, 5, 4, 191, 96, 48, 255));
            }
            // Store counters (pass diagnosis): target 1 drawn, then a
            // dispatch, then cleared whole and drawn again. An in-order
            // dispatch may read anything, so the first store counts as "read
            // later"; ahead it is no event and the clear makes the store a
            // discard candidate.
            {
                nvn::CommandList bl;
                bl.commands.push_back(nvn::CmdBindProgram{1});
                bl.commands.push_back(nvn::CmdBindVertexBuffer{0, cvb, verts.size() * 4});
                bl.commands.push_back(nvn::CmdDraw{nvn::Topology::Triangles, 0, 6, 0, 1});
                bl.commands.push_back(nvn::CmdBindProgram{3, true});
                bl.commands.push_back(nvn::CmdBindStorageBuffer{nvn::ShaderStage::Compute, 0, other, verts.size() * 4});
                bl.commands.push_back(nvn::CmdDispatchCompute{{6, 1, 1}, 0});
                bl.commands.push_back(nvn::CmdBindProgram{1});
                bl.commands.push_back(nvn::CmdClearColor{0, {0.2f, 0.2f, 0.2f, 1.0f}, nvn::kClearColorMaskRGBA});
                bl.commands.push_back(nvn::CmdDraw{nvn::Topology::Triangles, 6, 6, 0, 1});
                const WebGpuBackend::Stats b0 = backend.stats();
                backend.execute(bl);
                const WebGpuBackend::Stats b1 = backend.stats();
                auto store = [&](const char* why) {
                    const auto a = b1.pass_reasons.find(why), b = b0.pass_reasons.find(why);
                    return (a != b1.pass_reasons.end() ? a->second : 0) - (b != b0.pass_reasons.end() ? b->second : 0);
                };
                CHECK_EQ(store("store: colour attachment, read later"), compute_first ? 0u : 1u);
                CHECK_EQ(store("store: colour attachment, overwritten later before any read (discard candidate)"),
                         compute_first ? 1u : 0u);
                CHECK_EQ(store("store: colour attachment, not used again in the list"), 1u);
                memory.put(0x3D800, verts.data(), verts.size() * 4);  // `other`, which the dispatch wrote
            }
            // The same resource on both sides of a dispatch ahead: a draw
            // before it reads cvb from guest memory, the dispatch writes cvb,
            // a draw after it reads the dispatch's output. With
            // LGP_WEBGPU_KEEP_PASS_ON_DISPATCH all of it stays in one pass.
            memory.put(0x3D000, verts.data(), verts.size() * 4);
            nvn::CommandList sl;
            sl.commands.push_back(nvn::CmdBindProgram{1});
            sl.commands.push_back(nvn::CmdClearColor{0, {0.2f, 0.2f, 0.2f, 1.0f}, nvn::kClearColorMaskRGBA});
            sl.commands.push_back(nvn::CmdDraw{nvn::Topology::Triangles, 6, 6, 0, 1});
            sl.commands.push_back(nvn::CmdBindProgram{3, true});
            sl.commands.push_back(nvn::CmdBindStorageBuffer{nvn::ShaderStage::Compute, 0, cvb, verts.size() * 4});
            sl.commands.push_back(nvn::CmdDispatchCompute{{6, 1, 1}, 0});
            sl.commands.push_back(nvn::CmdBindProgram{1});
            sl.commands.push_back(nvn::CmdDraw{nvn::Topology::Triangles, 0, 6, 0, 1});
            const WebGpuBackend::Stats s0 = backend.stats();
            backend.execute(sl);
            const WebGpuBackend::Stats s1 = backend.stats();
            CHECK_EQ(s1.dispatches_first - s0.dispatches_first, compute_first ? 1u : 0u);
            CHECK_EQ(s1.compute_vertex_streams - s0.compute_vertex_streams, 1u);
            CHECK_EQ(s1.draws_before_dispatch - s0.draws_before_dispatch, 1u);
            CHECK_EQ(s1.draws_after_dispatch - s0.draws_after_dispatch, 1u);
            CHECK_EQ(s1.draw_passes - s0.draw_passes, keep ? 1u : 2u);
            CHECK(backend.read_texture(1, &px, nullptr, nullptr));
            if (px.size() == 8 * 8 * 4) {
                CHECK(texel_is(px, 8, 2, 4, 191, 96, 48, 255));  // left quad: alpha 1.0 from the dispatch
                CHECK(texel_is(px, 8, 5, 4, 191, 96, 48, 255));  // right quad, drawn before it
            }
            CHECK(memory.read(cvb + 7 * 4, &alpha, 4));
            CHECK(alpha == 1.0f);
            memory.put(0x3D000, verts.data(), verts.size() * 4);
        }
        CHECK_EQ(renderer.error_count(), 0u);
        backend.destroy_program(3);
    }
    // Geometry cache across lists (LGP_WEBGPU_GEOMETRY_CACHE): unchanged
    // vertices are bound from the earlier upload, changed ones are written
    // again, and a copy into the range between two draws of one list leaves
    // the first draw with the old bytes.
    {
        const uint64_t bytes = verts.size() * 4;
        const uint64_t gva = memory.put(0x3E000, verts.data(), bytes);
        auto draw_into = [&](nvn::CommandList& l, nvn::TextureId target) {
            l.commands.push_back(nvn::CmdSetRenderTargets{{target}, 0});
            l.commands.push_back(nvn::CmdSetViewports{0, {0, 0, 8, 8}});
            l.commands.push_back(nvn::CmdClearColor{0, {0.2f, 0.2f, 0.2f, 1.0f}, nvn::kClearColorMaskRGBA});
            l.commands.push_back(nvn::CmdDraw{nvn::Topology::Triangles, 0, 12, 0, 1});
        };
        nvn::CommandList gl;
        gl.commands.push_back(nvn::CmdBindProgram{1});
        gl.commands.push_back(layout);
        gl.commands.push_back(nvn::CmdBindVertexBuffer{0, gva, bytes});
        gl.commands.push_back(nvn::CmdBindUniformBuffer{nvn::ShaderStage::Vertex, nvn::kUniformBufferBankBase, cb_va, 64});
        gl.commands.push_back(nvn::CmdBindUniformBuffer{nvn::ShaderStage::Fragment, 0, cb_va, 64});
        draw_into(gl, 1);
        backend.execute(gl);
        WebGpuBackend::Stats before = backend.stats();
        backend.execute(gl);
        WebGpuBackend::Stats after = backend.stats();
        CHECK_EQ(after.geometry_uploads - before.geometry_uploads, 0u);
        CHECK_EQ(after.geometry_cached - before.geometry_cached, 1u);
        // Left quad now opaque: written again, drawn.
        std::vector<float> lit = verts;
        for (int v = 0; v < 6; ++v) lit[size_t(v) * 10 + 7] = 1.0f;
        memory.put(0x3E000, lit.data(), bytes);
        before = backend.stats();
        backend.execute(gl);
        after = backend.stats();
        CHECK_EQ(after.geometry_uploads - before.geometry_uploads, 1u);
        CHECK(backend.read_texture(1, &px, nullptr, nullptr));
        if (px.size() == 8 * 8 * 4) CHECK(texel_is(px, 8, 2, 4, 191, 96, 48, 255));
        // Draw (opaque), copy the original vertices over the range, draw
        // again into target 2 (left quad killed): each draw its own bytes.
        const uint64_t orig_va = memory.put(0x3E800, verts.data(), bytes);
        nvn::CommandList ml;
        ml.commands = gl.commands;
        ml.commands.push_back(nvn::CmdCopyBuffer{orig_va, gva, bytes});
        draw_into(ml, 2);
        backend.execute(ml);
        CHECK(backend.read_texture(1, &px, nullptr, nullptr));
        if (px.size() == 8 * 8 * 4) CHECK(texel_is(px, 8, 2, 4, 191, 96, 48, 255));
        CHECK(backend.read_texture(2, &px, nullptr, nullptr));
        if (px.size() == 8 * 8 * 4) {
            CHECK(texel_is(px, 8, 2, 4, 51, 51, 51, 255));
            CHECK(texel_is(px, 8, 5, 4, 191, 96, 48, 255));
        }
        // The next list sees the copied (original) bytes.
        backend.execute(gl);
        CHECK(backend.read_texture(1, &px, nullptr, nullptr));
        if (px.size() == 8 * 8 * 4) CHECK(texel_is(px, 8, 2, 4, 51, 51, 51, 255));
        // A low-memory warning drops the cache: the next list uploads its
        // vertices again and draws the same picture.
        lgp::platform::note_low_memory();
        before = backend.stats();
        backend.execute(gl);
        after = backend.stats();
        CHECK_EQ(after.geometry_uploads - before.geometry_uploads, 1u);
        CHECK_EQ(after.geometry_cached - before.geometry_cached, 0u);
        CHECK(backend.read_texture(1, &px, nullptr, nullptr));
        if (px.size() == 8 * 8 * 4) CHECK(texel_is(px, 8, 2, 4, 51, 51, 51, 255));
        // Ping-pong between targets 1 and 2 (Stats::pass_reasons "target
        // set: ..."): the third pass goes back to target 1, and with nothing
        // between reading or writing it, its draws could join the first; the
        // fifth goes back to target 1 after a copy into it and could not.
        {
            nvn::CommandList pp;
            pp.commands = gl.commands;  // draws into 1
            pp.commands.push_back(nvn::CmdSetRenderTargets{{2}, 0});
            pp.commands.push_back(nvn::CmdDraw{nvn::Topology::Triangles, 0, 12, 0, 1});
            pp.commands.push_back(nvn::CmdSetRenderTargets{{1}, 0});
            pp.commands.push_back(nvn::CmdDraw{nvn::Topology::Triangles, 0, 12, 0, 1});
            pp.commands.push_back(nvn::CmdSetRenderTargets{{2}, 0});
            pp.commands.push_back(nvn::CmdDraw{nvn::Topology::Triangles, 0, 12, 0, 1});
            nvn::CmdCopyTextureToTexture copy;
            copy.src = {2, 0, 0, 0, 0, 0, 8, 8, 1};
            copy.dst = {1, 0, 0, 0, 0, 0, 8, 8, 1};
            pp.commands.push_back(copy);
            pp.commands.push_back(nvn::CmdSetRenderTargets{{1}, 0});
            pp.commands.push_back(nvn::CmdDraw{nvn::Topology::Triangles, 0, 12, 0, 1});
            const WebGpuBackend::Stats q0 = backend.stats();
            backend.execute(pp);
            const WebGpuBackend::Stats q1 = backend.stats();
            auto cause = [&](const char* why) {
                const auto a = q1.pass_reasons.find(why), b = q0.pass_reasons.find(why);
                return (a != q1.pass_reasons.end() ? a->second : 0) - (b != q0.pass_reasons.end() ? b->second : 0);
            };
            CHECK_EQ(q1.draw_passes - q0.draw_passes, 5u);
            // Passes 3 (target 1), 4 (target 2) and 5 (target 1) go back.
            CHECK_EQ(cause("target set: back to an earlier one"), 3u);
            // 3 could join 1, 4 could join 2 (3 neither reads nor writes 2);
            // 5 not: the copy between writes target 1.
            CHECK_EQ(cause("target set: back to an earlier one, draws could join it"), 2u);
            // Stored attachments and their next use in the list: 1 is read
            // by passes 3 and the copy, 2 by pass 4 and the copy; the last
            // pass's target is not used again.
            CHECK_EQ(cause("store: colour attachment, read later"), 4u);
            CHECK_EQ(cause("store: colour attachment, not used again in the list"), 1u);
            CHECK_EQ(cause("store: colour attachment, overwritten later before any read (discard candidate)"), 0u);
        }
        // Target 1 stored, then cleared whole before anything reads it: its
        // first store is a discard candidate (folded clear or own clear pass).
        {
            nvn::CommandList dl;
            dl.commands = gl.commands;  // clears and draws into 1
            dl.commands.push_back(nvn::CmdSetRenderTargets{{2}, 0});
            dl.commands.push_back(nvn::CmdDraw{nvn::Topology::Triangles, 0, 12, 0, 1});
            dl.commands.push_back(nvn::CmdSetRenderTargets{{1}, 0});
            dl.commands.push_back(nvn::CmdClearColor{0, {0.2f, 0.2f, 0.2f, 1.0f}, nvn::kClearColorMaskRGBA});
            dl.commands.push_back(nvn::CmdDraw{nvn::Topology::Triangles, 0, 12, 0, 1});
            const WebGpuBackend::Stats d0 = backend.stats();
            backend.execute(dl);
            const WebGpuBackend::Stats d1 = backend.stats();
            auto cause = [&](const char* why) {
                const auto a = d1.pass_reasons.find(why), b = d0.pass_reasons.find(why);
                return (a != d1.pass_reasons.end() ? a->second : 0) - (b != d0.pass_reasons.end() ? b->second : 0);
            };
            CHECK_EQ(cause("store: colour attachment, overwritten later before any read (discard candidate)"), 1u);
            CHECK_EQ(cause("store: colour attachment, not used again in the list"), 2u);
            CHECK_EQ(cause("store: colour attachment, read later"), 0u);
        }
        CHECK_EQ(renderer.error_count(), 0u);
    }
    // Descriptors of a compute shader in the driver constant buffer (abi.hpp):
    // storage buffer 0 at c0[0x310], and NVN uniform block 8 (bank 11, as
    // LGP binds it) at c0[0x210 + 8 * 16], read through global memory as NVN
    // compute shaders do. cs_ube writes out[tid] = ubo[tid] + 1.
    {
        const std::vector<uint8_t> bin = read_hex(dir + "/cs_ube.hex");
        const auto lifted = lgp::gpu::ir::lift_compute(bin.data(), bin.size());
        CHECK(lifted.ok);
        wgsl::Options opt;
        opt.cbuf_group = 0;
        opt.texture_group = 1;
        opt.global_group = 2;
        opt.workgroup_size = {4, 1, 1};
        const wgsl::Result r = wgsl::emit(lifted.program, opt);
        CHECK(r.ok);
        nvn::ProgramDesc cs;
        cs.compute_wgsl = r.code;
        cs.compute_cbufs = r.bindings.cbuf_banks;
        cs.compute_global_memory = r.bindings.global_memory;
        backend.create_program(4, cs);
        const uint32_t ubo[4] = {10, 20, 30, 40};
        const uint64_t ubo_va = memory.put(0x3E000, ubo, sizeof(ubo));
        const uint32_t zeros[4] = {};
        const uint64_t out_va = memory.put(0x3E100, zeros, sizeof(zeros));
        nvn::CommandList cl;
        cl.commands.push_back(nvn::CmdBindProgram{4, true});
        cl.commands.push_back(nvn::CmdBindUniformBuffer{nvn::ShaderStage::Compute, nvn::kUniformBufferBankBase + 8, ubo_va,
                                                        sizeof(ubo)});
        cl.commands.push_back(nvn::CmdBindStorageBuffer{nvn::ShaderStage::Compute, 0, out_va, sizeof(zeros)});
        cl.commands.push_back(nvn::CmdDispatchCompute{{1, 1, 1}, 0});
        const WebGpuBackend::Stats before = backend.stats();
        const uint64_t mark_before = backend.fence_mark();
        backend.execute(cl);
        const uint64_t mark_after = backend.fence_mark();
        CHECK_EQ(mark_after, mark_before + 1);
        // A fence placed before the list needs none of its results: no wait.
        backend.finish_until(mark_before);
        CHECK_EQ(backend.stats().finish_pending, before.finish_pending);
        // The results reach guest memory after the call (asynchronous
        // write-back); waiting on a fence placed after it (nvnSyncWait) or
        // finish() (nvnQueueFinish) waits for them.
        backend.finish_until(mark_after);
        const WebGpuBackend::Stats after = backend.stats();
        CHECK_EQ(after.dispatches - before.dispatches, 1u);
        CHECK_EQ(after.compute_uniform_slots - before.compute_uniform_slots, 1u);
        CHECK_EQ(after.writebacks - before.writebacks, 1u);
        uint32_t out[4] = {};
        CHECK(memory.read(out_va, out, sizeof(out)));
        CHECK_EQ(out[0], 11u);
        CHECK_EQ(out[3], 41u);
        // A later list reads the earlier results without finish() in
        // between: its call waits for them. out2 = out + 1.
        {
            const uint64_t out2_va = memory.put(0x3E180, zeros, sizeof(zeros));
            nvn::CommandList ql;
            ql.commands.push_back(nvn::CmdBindUniformBuffer{nvn::ShaderStage::Compute, nvn::kUniformBufferBankBase + 8,
                                                            out_va, sizeof(zeros)});
            ql.commands.push_back(nvn::CmdBindStorageBuffer{nvn::ShaderStage::Compute, 0, out2_va, sizeof(zeros)});
            ql.commands.push_back(nvn::CmdDispatchCompute{{1, 1, 1}, 0});
            CHECK(memory.write(out_va, zeros, sizeof(zeros)));
            backend.execute(cl);  // out = ubo + 1 again, not yet in guest memory
            backend.execute(ql);
            backend.finish();
            uint32_t out2[4] = {};
            CHECK(memory.read(out2_va, out2, sizeof(out2)));
            CHECK_EQ(out2[0], 12u);
            CHECK_EQ(out2[3], 42u);
            // The guest writes into the range before the results arrive:
            // its bytes stay, the results land around them.
            CHECK(memory.write(out_va, zeros, sizeof(zeros)));
            backend.execute(cl);
            const uint32_t mine = 777;
            CHECK(memory.write(out_va + 8, &mine, 4));
            backend.finish();
            CHECK(memory.read(out_va, out, sizeof(out)));
            CHECK_EQ(out[0], 11u);
            CHECK_EQ(out[2], 777u);
            CHECK_EQ(out[3], 41u);
        }
        // Detaching the guest memory (process exit) first waits for the
        // results still on their way back into it.
        CHECK(memory.write(out_va, zeros, sizeof(zeros)));
        backend.execute(cl);
        backend.set_guest_memory(nullptr);
        CHECK(memory.read(out_va, out, sizeof(out)));
        CHECK_EQ(out[0], 11u);
        CHECK_EQ(out[3], 41u);
        backend.set_guest_memory(&memory);
        // Global loads have no bounds: with only 8 bytes bound, the shader
        // still reads words 2 and 3 behind them (the slot holds a window).
        nvn::CommandList sl;
        sl.commands.push_back(nvn::CmdBindUniformBuffer{nvn::ShaderStage::Compute, nvn::kUniformBufferBankBase + 8,
                                                        ubo_va, 8});
        sl.commands.push_back(nvn::CmdDispatchCompute{{1, 1, 1}, 0});
        std::memset(out, 0, sizeof(out));
        CHECK(memory.write(out_va, out, sizeof(out)));
        backend.execute(sl);
        backend.finish();
        CHECK(memory.read(out_va, out, sizeof(out)));
        CHECK_EQ(out[1], 21u);
        CHECK_EQ(out[3], 41u);
        // All 8 storage slots bound (as LGP's program 68 does): the constant
        // buffer still reaches the shader through its own table entry.
        nvn::CommandList fl;
        const uint32_t pad[8] = {};
        for (uint32_t k = 1; k < 8; ++k)
            fl.commands.push_back(nvn::CmdBindStorageBuffer{nvn::ShaderStage::Compute, k,
                                                            memory.put(0x3E200 + 0x20 * k, pad, sizeof(pad)), 0x20});
        fl.commands.push_back(nvn::CmdDispatchCompute{{1, 1, 1}, 0});
        std::memset(out, 0, sizeof(out));
        CHECK(memory.write(out_va, out, sizeof(out)));
        const WebGpuBackend::Stats full_before = backend.stats();
        backend.execute(fl);
        backend.finish();
        CHECK(memory.read(out_va, out, sizeof(out)));
        CHECK_EQ(out[0], 11u);
        CHECK_EQ(out[3], 41u);
        CHECK_EQ(backend.stats().compute_uniform_slots - full_before.compute_uniform_slots, 1u);
        CHECK_EQ(renderer.error_count(), 0u);
        backend.destroy_program(4);

        // LGP_WEBGPU_DEBUG=computeprobe's counters: with the uniform block
        // bound, loads and stores hit; without it, the loads read outside
        // every range while the stores still land.
        opt.gmem_probe = true;
        const wgsl::Result pr = wgsl::emit(lifted.program, opt);
        CHECK(pr.ok && pr.bindings.gmem_probe);
        cs.compute_wgsl = pr.code;
        cs.compute_gmem_probe = true;
        backend.create_program(5, cs);
        nvn::CommandList pl;
        pl.commands.push_back(nvn::CmdBindProgram{5, true});
        pl.commands.push_back(nvn::CmdDispatchCompute{{1, 1, 1}, 0});
        pl.commands.push_back(nvn::CmdBindUniformBuffer{nvn::ShaderStage::Compute, nvn::kUniformBufferBankBase + 8, 0, 0});
        pl.commands.push_back(nvn::CmdDispatchCompute{{1, 1, 1}, 0});
        backend.execute(pl);
        backend.finish();
        const WebGpuBackend::Stats probed = backend.stats();
        auto diag = [&](const char* k) {
            auto a = probed.diagnostics.find(k);
            auto b = after.diagnostics.find(k);
            return (a == probed.diagnostics.end() ? 0 : a->second) - (b == after.diagnostics.end() ? 0 : b->second);
        };
        CHECK_EQ(diag("probe: dispatches counted"), 2u);
        CHECK_EQ(diag("probe: dispatch stores into a bound range"), 2u);
        CHECK_EQ(diag("probe: dispatch loads outside every range (read 0)"), 1u);
        CHECK_EQ(diag("probe: dispatch ran no invocation"), 0u);
        CHECK_EQ(diag("probe: dispatch without c1 (constant pool)"), 2u);
        CHECK(memory.read(out_va, out, sizeof(out)));
        CHECK_EQ(out[0], 1u);  // ubo read 0 outside every range
        CHECK_EQ(renderer.error_count(), 0u);
        backend.destroy_program(5);

        // Accesses resolved to their slot (wgsl::Options::storage_desc_offset):
        // cs_ube with its descriptors moved to storage buffers 0 and 1, so
        // out(slot 1)[tid] = in(slot 0)[tid] + 1. With one range bound in both
        // slots the backend binds it once and points gs1 at slot 0: the
        // increment happens in place.
        std::vector<uint8_t> moved = bin;
        auto patch = [&](uint64_t from, uint64_t to) {
            bool found = false;
            for (size_t at = 0; at + 8 <= moved.size(); at += 8) {
                uint64_t w = 0;
                std::memcpy(&w, moved.data() + at, 8);
                if (w == from) {
                    std::memcpy(moved.data() + at, &to, 8);
                    found = true;
                }
            }
            CHECK(found);
        };
        patch(0x4c9807800c470008ull, 0x4c9807800c870008ull);  // mov $r8 c0[0x310] -> c0[0x320]
        patch(0x4c9807800c570009ull, 0x4c9807800c970009ull);  // mov $r9 c0[0x314] -> c0[0x324]
        patch(0x4c9807800a470002ull, 0x4c9807800c470002ull);  // mov $r2 c0[0x290] -> c0[0x310]
        patch(0x4c9807800a570003ull, 0x4c9807800c570003ull);  // mov $r3 c0[0x294] -> c0[0x314]
        const auto copy = lgp::gpu::ir::lift_compute(moved.data(), moved.size());
        CHECK(copy.ok);
        opt.gmem_probe = false;
        opt.storage_desc_offset = nvn::kStorageBufferDescOffsetCompute;
        const wgsl::Result sr = wgsl::emit(copy.program, opt);
        CHECK(sr.ok);
        CHECK(sr.bindings.static_storage_slots == std::vector<uint32_t>({0, 1}));
        CHECK_EQ(sr.bindings.gmem_dynamic_accesses, 0u);
        cs.compute_wgsl = sr.code;
        cs.compute_cbufs = sr.bindings.cbuf_banks;
        cs.compute_gmem_probe = false;
        backend.create_program(6, cs);
        const uint32_t vals[4] = {5, 6, 7, 8};
        const uint64_t in_va = memory.put(0x3E400, vals, sizeof(vals));
        const uint64_t copy_va = memory.put(0x3E500, zeros, sizeof(zeros));
        auto moved_count = [&] {
            const auto d = backend.stats().diagnostics;
            const auto it = d.find("dispatch: a range the shader reads directly is bound in another slot");
            return it == d.end() ? uint64_t{0} : it->second;
        };
        nvn::CommandList ml;
        ml.commands.push_back(nvn::CmdBindProgram{6, true});
        ml.commands.push_back(nvn::CmdBindStorageBuffer{nvn::ShaderStage::Compute, 0, in_va, sizeof(vals)});
        ml.commands.push_back(nvn::CmdBindStorageBuffer{nvn::ShaderStage::Compute, 1, copy_va, sizeof(zeros)});
        ml.commands.push_back(nvn::CmdDispatchCompute{{1, 1, 1}, 0});
        const uint64_t moved_before = moved_count();
        backend.execute(ml);
        backend.finish();
        CHECK(memory.read(copy_va, out, sizeof(out)));
        CHECK_EQ(out[0], 6u);
        CHECK_EQ(out[3], 9u);
        CHECK_EQ(moved_count(), moved_before);
        nvn::CommandList al;
        al.commands.push_back(nvn::CmdBindStorageBuffer{nvn::ShaderStage::Compute, 1, in_va, sizeof(vals)});
        al.commands.push_back(nvn::CmdDispatchCompute{{1, 1, 1}, 0});
        backend.execute(al);
        backend.finish();
        CHECK(memory.read(in_va, out, sizeof(out)));
        CHECK_EQ(out[0], 6u);
        CHECK_EQ(out[3], 9u);
        CHECK_EQ(moved_count(), moved_before + 1);
        CHECK_EQ(renderer.error_count(), 0u);
        backend.destroy_program(6);

        // cs_ube with its uniform block read directly too (constant window
        // 0, wgsl::Options::uniform_desc_offset). With the block inside a
        // bound storage slot the backend moves it there.
        opt.uniform_desc_offset = nvn::kComputeUniformDescOffset;
        const wgsl::Result ur = wgsl::emit(lifted.program, opt);
        CHECK(ur.ok);
        CHECK(ur.bindings.static_uniform_blocks == std::vector<uint32_t>({8}));
        CHECK_EQ(ur.bindings.gmem_dynamic_accesses, 0u);
        cs.compute_wgsl = ur.code;
        cs.compute_cbufs = ur.bindings.cbuf_banks;
        backend.create_program(7, cs);
        const uint64_t ubo2_va = memory.put(0x3E600, ubo, sizeof(ubo));
        const uint64_t out3_va = memory.put(0x3E700, zeros, sizeof(zeros));
        nvn::CommandList ul;
        ul.commands.push_back(nvn::CmdBindProgram{7, true});
        ul.commands.push_back(nvn::CmdBindStorageBuffer{nvn::ShaderStage::Compute, 1, 0, 0});
        ul.commands.push_back(nvn::CmdBindUniformBuffer{nvn::ShaderStage::Compute, nvn::kUniformBufferBankBase + 8,
                                                        ubo2_va, sizeof(ubo)});
        ul.commands.push_back(nvn::CmdBindStorageBuffer{nvn::ShaderStage::Compute, 0, out3_va, sizeof(zeros)});
        ul.commands.push_back(nvn::CmdDispatchCompute{{1, 1, 1}, 0});
        const uint64_t ube_before = moved_count();
        backend.execute(ul);
        backend.finish();
        CHECK(memory.read(out3_va, out, sizeof(out)));
        CHECK_EQ(out[0], 11u);
        CHECK_EQ(out[3], 41u);
        CHECK_EQ(moved_count(), ube_before);
        nvn::CommandList cl2;
        cl2.commands.push_back(nvn::CmdBindStorageBuffer{nvn::ShaderStage::Compute, 1, ubo2_va,
                                                         0x40});
        cl2.commands.push_back(nvn::CmdDispatchCompute{{1, 1, 1}, 0});
        CHECK(memory.write(out3_va, zeros, sizeof(zeros)));
        backend.execute(cl2);
        backend.finish();
        CHECK(memory.read(out3_va, out, sizeof(out)));
        CHECK_EQ(out[0], 11u);
        CHECK_EQ(out[3], 41u);
        CHECK_EQ(moved_count(), ube_before + 1);
        // Its block moved to the storage slot, its window is free: four more
        // blocks the shader would search for all get a window.
        const char* kNoWindow = "dispatch: more constant buffers in global memory than table entries";
        auto no_window = [&] {
            const auto d = backend.stats().skip_reasons;
            const auto it = d.find(kNoWindow);
            return it == d.end() ? uint64_t{0} : it->second;
        };
        nvn::CommandList fw;
        for (uint32_t b = 0; b < 4; ++b)
            fw.commands.push_back(nvn::CmdBindUniformBuffer{nvn::ShaderStage::Compute, nvn::kUniformBufferBankBase + b,
                                                            memory.put(0x3E800 + 0x100 * b, ubo, sizeof(ubo)),
                                                            sizeof(ubo)});
        fw.commands.push_back(nvn::CmdDispatchCompute{{1, 1, 1}, 0});
        CHECK(memory.write(out3_va, zeros, sizeof(zeros)));
        const uint64_t slots_before = backend.stats().compute_uniform_slots, no_window_before = no_window();
        backend.execute(fw);
        backend.finish();
        CHECK(memory.read(out3_va, out, sizeof(out)));
        CHECK_EQ(out[0], 11u);
        CHECK_EQ(out[3], 41u);
        CHECK_EQ(backend.stats().compute_uniform_slots - slots_before, 4u);
        CHECK_EQ(no_window(), no_window_before);
        CHECK_EQ(renderer.error_count(), 0u);
        backend.destroy_program(7);
    }
    backend.destroy_program(1);

    // P4.3: textures whose contents are only in their pool storage
    // (block-linear, as the game writes them) read back deswizzled; 16-bit
    // colours come back widened to RGBA8.
    {
        nvn::TextureDesc pd;
        pd.target = uint32_t(nvn::TextureTarget::Target2D);
        pd.format = uint32_t(nvn::Format::RGBA8);
        pd.width = 20;
        pd.height = 12;
        const nvn::TextureLayout lay = nvn::texture_layout(pd.format, pd.target, pd.width, pd.height, 1, 1);
        std::vector<uint8_t> pool(size_t(lay.total_size), 0);
        const nvn::LevelLayout& lv = lay.levels[0];
        for (uint32_t y = 0; y < pd.height; ++y)
            for (uint32_t x = 0; x < pd.width; ++x) {
                const uint8_t texel[4] = {uint8_t(x * 10), uint8_t(y * 20), uint8_t(x + y), 255};
                std::memcpy(pool.data() + nvn::block_linear_offset(x * 4, y, lv.pitch, lv.gob_height), texel, 4);
            }
        pd.storage = memory.put(0x10000, pool.data(), pool.size());
        backend.create_texture(10, pd);
        CHECK(backend.read_texture(10, &px, nullptr, nullptr));
        if (px.size() == size_t(pd.width) * pd.height * 4) {
            CHECK(texel_is(px, pd.width, 0, 0, 0, 0, 0, 255));
            CHECK(texel_is(px, pd.width, 19, 11, 190, 220, 30, 255));
            CHECK(texel_is(px, pd.width, 7, 3, 70, 60, 10, 255));
        }

        nvn::TextureDesc rd = pd;
        rd.format = uint32_t(nvn::Format::RGB565);
        rd.width = 4;
        rd.height = 2;
        const nvn::TextureLayout l565 = nvn::texture_layout(rd.format, rd.target, rd.width, rd.height, 1, 1);
        std::vector<uint8_t> p565(size_t(l565.total_size), 0);
        const uint16_t red = 31, green = 63 << 5, blue = uint16_t(31u << 11);
        const uint16_t row0[4] = {red, green, blue, 0xFFFF};
        for (uint32_t x = 0; x < 4; ++x)
            std::memcpy(p565.data() + nvn::block_linear_offset(x * 2, 0, l565.levels[0].pitch, l565.levels[0].gob_height),
                        &row0[x], 2);
        rd.storage = memory.put(0x20000, p565.data(), p565.size());
        backend.create_texture(11, rd);
        CHECK(backend.read_texture(11, &px, nullptr, nullptr));
        if (px.size() == 4 * 2 * 4) {
            CHECK(texel_is(px, 4, 0, 0, 255, 0, 0, 255));
            CHECK(texel_is(px, 4, 1, 0, 0, 255, 0, 255));
            CHECK(texel_is(px, 4, 2, 0, 0, 0, 255, 255));
            CHECK(texel_is(px, 4, 3, 0, 255, 255, 255, 255));
            CHECK(texel_is(px, 4, 0, 1, 0, 0, 0, 255));
        }
    }
    // ClearTexture: only the region (texture space), whatever is bound;
    // CopyBuffer: guest bytes copied in command order.
    {
        nvn::TextureDesc cd = td;
        backend.create_texture(14, cd);
        const uint32_t src_words[2] = {0x11223344, 0x55667788};
        const uint64_t src = memory.put(0x3A000, src_words, sizeof(src_words));
        nvn::CommandList cl;
        cl.commands.push_back(nvn::CmdSetRenderTargets{{14}, 0});
        cl.commands.push_back(nvn::CmdClearColor{0, {0.0f, 0.0f, 0.0f, 1.0f}, nvn::kClearColorMaskRGBA});
        nvn::CmdClearTexture ct;
        ct.texture = 14;
        ct.region = {2, 1, 3, 4};
        ct.rgba[0] = 1.0f;
        ct.rgba[3] = 1.0f;
        cl.commands.push_back(ct);
        cl.commands.push_back(nvn::CmdCopyBuffer{src, BlockMemory::kBase + 0x3A100, sizeof(src_words)});
        backend.execute(cl);
        CHECK(backend.read_texture(14, &px, nullptr, nullptr));
        if (px.size() == 8 * 8 * 4) {
            CHECK(texel_is(px, 8, 2, 1, 255, 0, 0, 255));
            CHECK(texel_is(px, 8, 4, 4, 255, 0, 0, 255));
            CHECK(texel_is(px, 8, 5, 4, 0, 0, 0, 255));
            CHECK(texel_is(px, 8, 2, 5, 0, 0, 0, 255));
        }
        uint32_t copied[2] = {};
        CHECK(memory.read(BlockMemory::kBase + 0x3A100, copied, sizeof(copied)));
        CHECK(copied[0] == src_words[0] && copied[1] == src_words[1]);
    }

    // Texture view swizzle (wgsl::Options::texture_swizzle): the texel of a
    // pool texture (200, 100, 50, 255) sampled through (ONE, ZERO, R, G).
    {
        const std::vector<uint8_t> tex_bin = read_hex(dir + "/fs_textured.hex");
        lgp::gpu::shader::Sph tex_sph;
        CHECK(lgp::gpu::shader::parse_sph(tex_bin, tex_sph, error));
        wgsl::Options tvs_opt = vs_opt, tfs_opt = fs_opt;
        tvs_opt.vertex_output_interp = wgsl::fragment_input_interpolation(tex_sph);
        tvs_opt.texture_swizzle = tfs_opt.texture_swizzle = true;
        wgsl::Result tvs, tfs;
        CHECK(translate(vs_bin, tvs_opt, &tvs));
        CHECK(translate(tex_bin, tfs_opt, &tfs));
        nvn::ProgramDesc tp{tvs.code, tfs.code, tvs.bindings.cbuf_banks, tfs.bindings.cbuf_banks, {}, {0}};
        tp.texture_swizzle = true;
        backend.create_program(3, tp);

        nvn::TextureDesc sd;
        sd.target = uint32_t(nvn::TextureTarget::Target2D);
        sd.format = uint32_t(nvn::Format::RGBA8);
        sd.width = 2;
        sd.height = 2;
        const nvn::TextureLayout sl = nvn::texture_layout(sd.format, sd.target, 2, 2, 1, 1);
        std::vector<uint8_t> spool(size_t(sl.total_size), 0);
        for (uint32_t y = 0; y < 2; ++y)
            for (uint32_t x = 0; x < 2; ++x) {
                const uint8_t texel[4] = {200, 100, 50, 255};
                std::memcpy(spool.data() + nvn::block_linear_offset(x * 4, y, sl.levels[0].pitch,
                                                                    sl.levels[0].gob_height),
                            texel, 4);
            }
        sd.storage = memory.put(0x30000, spool.data(), spool.size());
        backend.create_texture(12, sd);
        nvn::TextureDesc target = td;
        backend.create_texture(13, target);

        std::vector<uint32_t> ident(16, 0);
        for (int i = 0; i < 16; i += 5) ident[i] = f2u(1.0f);
        const uint64_t ident_va = memory.put(0x38000, ident.data(), ident.size() * 4);
        std::vector<float> full;
        const float white[4] = {1, 1, 1, 1};
        quad(full, -1, -1, 1, 1, white);
        const uint64_t full_va = memory.put(0x39000, full.data(), full.size() * 4);

        nvn::CommandList sl_list;
        sl_list.commands.push_back(nvn::CmdBindProgram{3});
        sl_list.commands.push_back(layout);
        sl_list.commands.push_back(nvn::CmdBindVertexBuffer{0, full_va, full.size() * 4});
        sl_list.commands.push_back(
            nvn::CmdBindUniformBuffer{nvn::ShaderStage::Vertex, nvn::kUniformBufferBankBase, ident_va, 64});
        sl_list.commands.push_back(nvn::CmdSetRenderTargets{{13}, 0});
        sl_list.commands.push_back(nvn::CmdSetViewports{0, {0, 0, 8, 8}});  // earlier lists' viewport carries over
        nvn::CmdBindTexture bind;
        bind.stage = nvn::ShaderStage::Fragment;
        bind.slot = 0;
        bind.texture = 12;
        bind.swizzle = {nvn::Swizzle::One, nvn::Swizzle::Zero, nvn::Swizzle::R, nvn::Swizzle::G};
        sl_list.commands.push_back(bind);
        sl_list.commands.push_back(nvn::CmdDraw{nvn::Topology::Triangles, 0, 6, 0, 1});
        backend.execute(sl_list);
        CHECK(backend.read_texture(13, &px, nullptr, nullptr));
        if (px.size() == 8 * 8 * 4) {
            CHECK(texel_is(px, 8, 1, 1, 255, 0, 200, 100));
            CHECK(texel_is(px, 8, 6, 6, 255, 0, 200, 100));
        }

        // Whole-texture clears that are read before any draw pass has the
        // texture as attachment (LGP_WEBGPU_FOLD_CLEARS): a draw sampling it
        // and a copy from it. Both get the clear as its own pass first.
        {
            backend.create_texture(16, td);
            backend.create_texture(17, td);
            auto reasons_since = [&](const WebGpuBackend::Stats& b, const char* why) {
                const WebGpuBackend::Stats a = backend.stats();
                const auto x = a.pass_reasons.find(why), y = b.pass_reasons.find(why);
                return (x != a.pass_reasons.end() ? x->second : 0) - (y != b.pass_reasons.end() ? y->second : 0);
            };
            nvn::CommandList cs = sl_list;
            cs.commands.insert(cs.commands.begin(), {nvn::CmdSetRenderTargets{{16}, 0},
                                                     nvn::CmdClearColor{0, {0.4f, 0.6f, 0.8f, 1.0f},
                                                                        nvn::kClearColorMaskRGBA}});
            for (nvn::Command& c : cs.commands)
                if (auto* b = std::get_if<nvn::CmdBindTexture>(&c)) {
                    b->texture = 16;
                    b->swizzle = nvn::kIdentitySwizzle;
                }
            const WebGpuBackend::Stats s0 = backend.stats();
            backend.execute(cs);
            CHECK_EQ(reasons_since(s0, "own pass: colour clear"), 1u);
            CHECK_EQ(reasons_since(s0, "folded: colour clear (no own pass)"), 0u);
            CHECK(backend.read_texture(13, &px, nullptr, nullptr));
            if (px.size() == 8 * 8 * 4) CHECK(texel_is(px, 8, 3, 3, 102, 153, 204, 255));

            nvn::CommandList cc;
            cc.commands.push_back(nvn::CmdSetRenderTargets{{16}, 0});
            cc.commands.push_back(nvn::CmdClearColor{0, {0.1f, 0.2f, 0.3f, 1.0f}, nvn::kClearColorMaskRGBA});
            nvn::CmdCopyTextureToTexture copy;
            copy.src = {16, 0, 0, 0, 0, 0, 8, 8, 1};
            copy.dst = {17, 0, 0, 0, 0, 0, 8, 8, 1};
            cc.commands.push_back(copy);
            const WebGpuBackend::Stats s1 = backend.stats();
            backend.execute(cc);
            CHECK_EQ(reasons_since(s1, "own pass: colour clear"), 1u);
            CHECK(backend.read_texture(17, &px, nullptr, nullptr));
            if (px.size() == 8 * 8 * 4) {
                CHECK(texel_is(px, 8, 0, 0, 26, 51, 77, 255));
                CHECK(texel_is(px, 8, 7, 7, 26, 51, 77, 255));
            }
            backend.destroy_texture(16);
            backend.destroy_texture(17);
        }

        // Store counters (pass diagnosis): texture 16 cleared whole with
        // CmdClearTexture (folded into its draw pass with
        // LGP_WEBGPU_FOLD_CLEARS), drawn, then sampled by the next pass:
        // its store counts as "read later". Without the diagnosis nothing
        // is counted.
        {
            const bool fold = pass_switch("LGP_WEBGPU_FOLD_CLEARS");
            backend.create_texture(16, td);
            nvn::CmdClearTexture clear16;
            clear16.texture = 16;
            clear16.region = {0, 0, 8, 8};
            clear16.rgba[3] = 1.0f;
            nvn::CommandList rl;
            rl.commands.push_back(clear16);
            for (const nvn::Command& c : sl_list.commands) {
                if (std::holds_alternative<nvn::CmdSetRenderTargets>(c))
                    rl.commands.push_back(nvn::CmdSetRenderTargets{{16}, 0});
                else
                    rl.commands.push_back(c);
            }
            rl.commands.push_back(nvn::CmdSetRenderTargets{{13}, 0});
            nvn::CmdBindTexture from16;
            from16.stage = nvn::ShaderStage::Fragment;
            from16.slot = 0;
            from16.texture = 16;
            from16.swizzle = nvn::kIdentitySwizzle;
            rl.commands.push_back(from16);
            rl.commands.push_back(nvn::CmdDraw{nvn::Topology::Triangles, 0, 6, 0, 1});
            auto reasons_since = [&](const WebGpuBackend::Stats& b, const char* why) {
                const WebGpuBackend::Stats a = backend.stats();
                const auto x = a.pass_reasons.find(why), y = b.pass_reasons.find(why);
                return (x != a.pass_reasons.end() ? x->second : 0) - (y != b.pass_reasons.end() ? y->second : 0);
            };
            const WebGpuBackend::Stats r0 = backend.stats();
            backend.execute(rl);
            CHECK_EQ(reasons_since(r0, "folded: colour clear (no own pass)"), fold ? 1u : 0u);
            CHECK_EQ(reasons_since(r0, "store: colour attachment, read later"), 1u);
            CHECK_EQ(reasons_since(r0, "store: colour attachment, not used again in the list"), 1u);
            CHECK_EQ(reasons_since(r0, "store: colour attachment, overwritten later before any read (discard candidate)"), 0u);
            CHECK(backend.read_texture(13, &px, nullptr, nullptr));
            if (px.size() == 8 * 8 * 4) CHECK(texel_is(px, 8, 4, 4, 255, 0, 200, 100));  // 12 through 16
            // Default path: no diagnosis, no store or target-set counts.
            backend.set_pass_diagnosis(false);
            const WebGpuBackend::Stats r1 = backend.stats();
            backend.execute(rl);
            backend.set_pass_diagnosis(true);
            const WebGpuBackend::Stats r2 = backend.stats();
            for (const auto& [why, n] : r2.pass_reasons) {
                if (why.rfind("store: ", 0) != 0 && why.rfind("target set: ", 0) != 0)
                    continue;
                const auto b = r1.pass_reasons.find(why);
                CHECK_EQ(n - (b != r1.pass_reasons.end() ? b->second : 0), 0u);
            }
            CHECK(backend.read_texture(13, &px, nullptr, nullptr));
            if (px.size() == 8 * 8 * 4) CHECK(texel_is(px, 8, 4, 4, 255, 0, 200, 100));
            backend.destroy_texture(16);
        }

        // Pitch-linear pool texture (TextureDesc::linear_stride, glyph caches):
        // rows with padding, checked at every present against the bytes of
        // the last upload. Unchanged guest memory is not uploaded again; a
        // changed texel shows after the next present.
        {
            nvn::TextureDesc ld = sd;
            ld.linear_stride = 16;
            std::vector<uint8_t> lpool(32, 0xEE);
            auto fill = [&](uint8_t r, uint8_t g) {
                for (uint32_t y = 0; y < 2; ++y)
                    for (uint32_t x = 0; x < 2; ++x) {
                        const uint8_t texel[4] = {r, g, 50, 255};
                        std::memcpy(lpool.data() + y * 16 + x * 4, texel, 4);
                    }
                return memory.put(0x37000, lpool.data(), lpool.size());
            };
            ld.storage = fill(200, 100);
            backend.create_texture(15, ld);
            nvn::CommandList ll = sl_list;
            for (nvn::Command& c : ll.commands)
                if (auto* b = std::get_if<nvn::CmdBindTexture>(&c)) b->texture = 15;
            backend.execute(ll);
            CHECK(backend.read_texture(13, &px, nullptr, nullptr));
            if (px.size() == 8 * 8 * 4) CHECK(texel_is(px, 8, 3, 3, 255, 0, 200, 100));
            CHECK(backend.present({13, {}}));
            const uint64_t uploads = backend.stats().uploads;
            const uint64_t compares = memory.equal_at[ld.storage];
            backend.execute(ll);  // checked again, unchanged: compared in place
            CHECK_EQ(backend.stats().uploads, uploads);
            CHECK_EQ(memory.equal_at[ld.storage], compares + 1);
            CHECK(backend.present({13, {}}));
            fill(10, 20);
            backend.execute(ll);
            CHECK_EQ(backend.stats().uploads, uploads + 1);
            CHECK(backend.read_texture(13, &px, nullptr, nullptr));
            if (px.size() == 8 * 8 * 4) CHECK(texel_is(px, 8, 3, 3, 255, 0, 10, 20));
            backend.destroy_texture(15);
        }

        // Border colour on a colour lookup (wgsl::Options::sampler_border):
        // u = 2.0 with CLAMP_TO_BORDER on u and a transparent white border
        // gives (1, 1, 1, 0) instead of the edge texel (200, 100, 50, 255).
        tfs_opt.sampler_border = true;
        wgsl::Result bfs;
        CHECK(translate(tex_bin, tfs_opt, &bfs));
        nvn::ProgramDesc btp{tvs.code, bfs.code, tvs.bindings.cbuf_banks, bfs.bindings.cbuf_banks, {}, {0}};
        btp.texture_swizzle = true;
        backend.create_program(70, btp);
        std::vector<float> outside;
        quad(outside, -1, -1, 1, 1, white);
        for (size_t v = 0; v < outside.size(); v += 10) outside[v + 8] = 2.0f, outside[v + 9] = 0.5f;
        const uint64_t outside_va = memory.put(0x39400, outside.data(), outside.size() * 4);
        nvn::CommandList bl;
        bl.commands.push_back(nvn::CmdBindProgram{70});
        bl.commands.push_back(nvn::CmdBindVertexBuffer{0, outside_va, outside.size() * 4});
        nvn::CmdBindTexture bbind = bind;
        bbind.swizzle = nvn::kIdentitySwizzle;
        bbind.sampler.wrap[0] = nvn::AddressMode::ClampToBorder;
        bbind.sampler.border[0] = bbind.sampler.border[1] = bbind.sampler.border[2] = 1.0f;
        bl.commands.push_back(bbind);
        bl.commands.push_back(nvn::CmdDraw{nvn::Topology::Triangles, 0, 6, 0, 1});
        backend.execute(bl);
        CHECK(backend.read_texture(13, &px, nullptr, nullptr));
        if (px.size() == 8 * 8 * 4) CHECK(texel_is(px, 8, 3, 3, 255, 255, 255, 0));
        backend.destroy_program(70);
    }
    // 3D texture (P4.3, LGP's colour LUT is RGBA8 32x32x32): pool storage in
    // the 3D block-linear layout, sampled by TEXS t3d. Slice z has colour
    // (40 z, 255 - 40 z, 10 z, 255); w = 0.125 hits slice 0, w = 0.625 slice 2
    // of 4 (slice centres, so the filter does not matter).
    {
        const std::vector<uint8_t> fs3_bin = read_hex(dir + "/fs_textured_3d.hex");
        lgp::gpu::shader::Sph fs3_sph;
        CHECK(lgp::gpu::shader::parse_sph(fs3_bin, fs3_sph, error));
        wgsl::Options vs3_opt = vs_opt, fs3_opt = fs_opt;
        vs3_opt.vertex_output_interp = wgsl::fragment_input_interpolation(fs3_sph);
        vs3_opt.texture_swizzle = fs3_opt.texture_swizzle = true;
        wgsl::Result vs3, fs3;
        CHECK(translate(vs_bin, vs3_opt, &vs3));
        CHECK(translate(fs3_bin, fs3_opt, &fs3));
        CHECK(fs3.bindings.texture_dims == std::vector<uint32_t>{3});
        CHECK(fs3.code.find("texture_3d<f32>") != std::string::npos);
        nvn::ProgramDesc p3{vs3.code, fs3.code, vs3.bindings.cbuf_banks, fs3.bindings.cbuf_banks, {}, {0}};
        p3.fragment_texture_dims = fs3.bindings.texture_dims;
        p3.texture_swizzle = true;
        backend.create_program(4, p3);

        nvn::TextureDesc vd;
        vd.target = uint32_t(nvn::TextureTarget::Target3D);
        vd.format = uint32_t(nvn::Format::RGBA8);
        vd.width = 4;
        vd.height = 4;
        vd.depth = 4;
        const nvn::TextureLayout vl = nvn::texture_layout(vd.format, vd.target, 4, 4, 4, 1);
        const nvn::LevelLayout& v0 = vl.levels[0];
        std::vector<uint8_t> vpool(size_t(vl.total_size), 0);
        for (uint32_t z = 0; z < 4; ++z)
            for (uint32_t y = 0; y < 4; ++y)
                for (uint32_t x = 0; x < 4; ++x) {
                    const uint8_t texel[4] = {uint8_t(40 * z), uint8_t(255 - 40 * z), uint8_t(10 * z), 255};
                    std::memcpy(vpool.data() + nvn::block_linear_offset(v0, x * 4, y, z), texel, 4);
                }
        vd.storage = memory.put(0x3A000, vpool.data(), vpool.size());
        backend.create_texture(14, vd);
        backend.create_texture(15, td);

        std::vector<uint32_t> ident(16, 0);
        for (int i = 0; i < 16; i += 5) ident[i] = f2u(1.0f);
        const uint64_t ident_va = memory.put(0x3C000, ident.data(), ident.size() * 4);
        std::vector<float> two;
        const float w0[4] = {0.125f, 0, 0, 1}, w2[4] = {0.625f, 0, 0, 1};
        quad(two, -1, -1, 0, 1, w0);
        quad(two, 0, -1, 1, 1, w2);
        const uint64_t two_va = memory.put(0x3D000, two.data(), two.size() * 4);

        nvn::CommandList l3;
        l3.commands.push_back(nvn::CmdBindProgram{4});
        l3.commands.push_back(layout);
        l3.commands.push_back(nvn::CmdBindVertexBuffer{0, two_va, two.size() * 4});
        l3.commands.push_back(
            nvn::CmdBindUniformBuffer{nvn::ShaderStage::Vertex, nvn::kUniformBufferBankBase, ident_va, 64});
        l3.commands.push_back(nvn::CmdSetRenderTargets{{15}, 0});
        nvn::CmdBindTexture bind3;
        bind3.stage = nvn::ShaderStage::Fragment;
        bind3.slot = 0;
        bind3.texture = 14;
        l3.commands.push_back(bind3);
        l3.commands.push_back(nvn::CmdDraw{nvn::Topology::Triangles, 0, 12, 0, 1});
        backend.execute(l3);
        CHECK(backend.read_texture(15, &px, nullptr, nullptr));
        if (px.size() == 8 * 8 * 4) {
            CHECK(texel_is(px, 8, 1, 3, 0, 255, 0, 255));     // slice 0
            CHECK(texel_is(px, 8, 6, 3, 80, 175, 20, 255));   // slice 2
        }
        // A 3D texture in a 2D binding (and the reverse) binds the dummy instead of failing validation.
        bind3.texture = 12;
        nvn::CommandList mismatch;
        mismatch.commands = l3.commands;
        mismatch.commands[5] = bind3;
        backend.execute(mismatch);
        CHECK_EQ(backend.stats().skip_reasons.count("texture: 2D/3D does not match the shader"), 1u);
    }
    // nvnCommandBufferCopyTextureToTexture: plain copies, a copy inside one
    // texture, scaled (nearest) and mirrored blits. Source: a 4x4 pool texture
    // (only in pool storage, uploaded on the first copy), texel (x, y) =
    // (60 x, 60 y, 7, 255).
    {
        nvn::TextureDesc cd;
        cd.target = uint32_t(nvn::TextureTarget::Target2D);
        cd.format = uint32_t(nvn::Format::RGBA8);
        cd.width = 4;
        cd.height = 4;
        const nvn::TextureLayout cl = nvn::texture_layout(cd.format, cd.target, 4, 4, 1, 1);
        std::vector<uint8_t> cpool(size_t(cl.total_size), 0);
        for (uint32_t y = 0; y < 4; ++y)
            for (uint32_t x = 0; x < 4; ++x) {
                const uint8_t texel[4] = {uint8_t(60 * x), uint8_t(60 * y), 7, 255};
                std::memcpy(cpool.data() + nvn::block_linear_offset(cl.levels[0], x * 4, y, 0), texel, 4);
            }
        cd.storage = memory.put(0x3E000, cpool.data(), cpool.size());
        backend.create_texture(20, cd);
        backend.create_texture(21, td);  // 8x8 RGBA8 targets
        backend.create_texture(22, td);

        using Copy = nvn::CmdCopyTextureToTexture;
        auto copy = [](nvn::TextureId from, int32_t sx, int32_t sy, int32_t sw, int32_t sh, nvn::TextureId to,
                       int32_t dx, int32_t dy, int32_t dw, int32_t dh, uint32_t flags) {
            Copy c;
            c.src = {from, 0, 0, sx, sy, 0, sw, sh, 1};
            c.dst = {to, 0, 0, dx, dy, 0, dw, dh, 1};
            c.flags = flags;
            return c;
        };
        nvn::CommandList cl_list;
        cl_list.commands.push_back(nvn::CmdSetRenderTargets{{21}, 0});
        cl_list.commands.push_back(nvn::CmdClearColor{0, {0, 0, 0, 1}, nvn::kClearColorMaskRGBA});
        cl_list.commands.push_back(nvn::CmdSetRenderTargets{{22}, 0});
        cl_list.commands.push_back(nvn::CmdClearColor{0, {0, 0, 0, 1}, nvn::kClearColorMaskRGBA});
        cl_list.commands.push_back(copy(20, 1, 1, 2, 2, 21, 5, 0, 2, 2, 0));  // plain, offset
        cl_list.commands.push_back(copy(21, 5, 0, 2, 2, 21, 6, 1, 2, 2, 0));  // overlapping, same texture
        cl_list.commands.push_back(copy(20, 0, 0, 2, 2, 22, 0, 0, 4, 4, 0));  // 2x scale, nearest
        cl_list.commands.push_back(copy(20, 0, 0, 4, 1, 22, 4, 7, 4, 1, nvn::kCopyFlagMirrorX));
        cl_list.commands.push_back(copy(20, 0, 0, 9, 9, 22, 0, 0, 9, 9, 0));  // outside: skipped
        const auto before = backend.stats();
        backend.execute(cl_list);
        const auto after = backend.stats();
        CHECK_EQ(after.texture_copies - before.texture_copies, 4u);
        CHECK_EQ(after.blits - before.blits, 2u);
        CHECK_EQ(after.skip_reasons.count("copy texture: region outside the texture"), 1u);
        CHECK(backend.read_texture(21, &px, nullptr, nullptr));
        if (px.size() == 8 * 8 * 4) {
            CHECK(texel_is(px, 8, 5, 0, 60, 60, 7, 255));    // first copy, kept where not overwritten
            CHECK(texel_is(px, 8, 6, 1, 60, 60, 7, 255));    // second copy: old (5, 0)
            CHECK(texel_is(px, 8, 7, 1, 120, 60, 7, 255));   // old (6, 0)
            CHECK(texel_is(px, 8, 7, 2, 120, 120, 7, 255));  // old (6, 1) before it was overwritten
            CHECK(texel_is(px, 8, 0, 0, 0, 0, 0, 255));
        }
        CHECK(backend.read_texture(22, &px, nullptr, nullptr));
        if (px.size() == 8 * 8 * 4) {
            CHECK(texel_is(px, 8, 0, 0, 0, 0, 7, 255));
            CHECK(texel_is(px, 8, 1, 1, 0, 0, 7, 255));
            CHECK(texel_is(px, 8, 2, 1, 60, 0, 7, 255));
            CHECK(texel_is(px, 8, 3, 3, 60, 60, 7, 255));
            CHECK(texel_is(px, 8, 4, 7, 180, 0, 7, 255));    // mirrored row 0
            CHECK(texel_is(px, 8, 7, 7, 0, 0, 7, 255));
            CHECK(texel_is(px, 8, 5, 5, 0, 0, 0, 255));      // untouched
        }
    }
    // ASTC decoded on the CPU (force_cpu_astc, as on devices without
    // texture-compression-astc): a 10x4 ASTC 4x4 texture from pool storage,
    // three blocks wide: two LDR void-extent blocks with known colours and an
    // all-zero block (reserved block mode -> error colour magenta). Width 10
    // is not a multiple of the block width; the host texture is exactly 10x4.
    {
        backend.force_cpu_astc(true);
        auto void_extent = [](uint16_t r, uint16_t g, uint16_t b, uint16_t a, uint8_t* out) {
            const uint8_t head[8] = {0xFC, 0xFD, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
            std::memcpy(out, head, 8);
            const uint16_t c[4] = {r, g, b, a};
            std::memcpy(out + 8, c, 8);
        };
        nvn::TextureDesc ad;
        ad.target = uint32_t(nvn::TextureTarget::Target2D);
        ad.format = uint32_t(nvn::Format::ASTC_4x4);
        ad.width = 10;
        ad.height = 4;
        const nvn::TextureLayout lay = nvn::texture_layout(ad.format, ad.target, ad.width, ad.height, 1, 1);
        const nvn::LevelLayout& lv = lay.levels[0];
        CHECK_EQ(lv.blocks_x, 3u);
        std::vector<uint8_t> pool(size_t(lay.total_size), 0);
        uint8_t block[16];
        void_extent(0xC8C8, 0x6464, 0x3232, 0xFFFF, block);  // (200, 100, 50, 255)
        std::memcpy(pool.data() + nvn::block_linear_offset(0, 0, lv.pitch, lv.gob_height), block, 16);
        void_extent(0x0A0A, 0x1414, 0x1E1E, 0x2828, block);  // (10, 20, 30, 40)
        std::memcpy(pool.data() + nvn::block_linear_offset(16, 0, lv.pitch, lv.gob_height), block, 16);
        ad.storage = memory.put(0x3F000, pool.data(), pool.size());
        const auto before = backend.stats();
        backend.create_texture(30, ad);
        CHECK(backend.read_texture(30, &px, nullptr, nullptr));
        CHECK_EQ(px.size(), size_t(10 * 4 * 4));
        if (px.size() == 10 * 4 * 4) {
            CHECK(texel_is(px, 10, 0, 0, 200, 100, 50, 255));
            CHECK(texel_is(px, 10, 3, 3, 200, 100, 50, 255));
            CHECK(texel_is(px, 10, 4, 0, 10, 20, 30, 40));
            CHECK(texel_is(px, 10, 7, 3, 10, 20, 30, 40));
            CHECK(texel_is(px, 10, 8, 0, 255, 0, 255, 255));  // error colour
            CHECK(texel_is(px, 10, 9, 3, 255, 0, 255, 255));
        }
        const auto after = backend.stats();
        CHECK_EQ(after.astc_cpu_decoded - before.astc_cpu_decoded, 1u);
        CHECK_EQ(after.astc_error_blocks - before.astc_error_blocks, 1u);
        backend.force_cpu_astc(false);
    }
    // BCn decoded on the CPU (force_cpu_bc, as on Adreno/Mali): a 10x4 BC1
    // texture, three blocks: solid red (3-colour mode, index 0), black (index
    // 1 of a 4-colour block) and transparent black (index 3, 3-colour mode).
    {
        backend.force_cpu_bc(true);
        auto bc1 = [](uint16_t c0, uint16_t c1, uint32_t indices, uint8_t* out) {
            std::memcpy(out, &c0, 2);
            std::memcpy(out + 2, &c1, 2);
            std::memcpy(out + 4, &indices, 4);
        };
        nvn::TextureDesc bd;
        bd.target = uint32_t(nvn::TextureTarget::Target2D);
        bd.format = uint32_t(nvn::Format::RGBA_DXT1);
        bd.width = 10;
        bd.height = 4;
        const nvn::TextureLayout lay = nvn::texture_layout(bd.format, bd.target, bd.width, bd.height, 1, 1);
        const nvn::LevelLayout& lv = lay.levels[0];
        CHECK_EQ(lv.blocks_x, 3u);
        std::vector<uint8_t> pool(size_t(lay.total_size), 0);
        uint8_t block[8];
        bc1(0xF800, 0xF800, 0x00000000, block);
        std::memcpy(pool.data() + nvn::block_linear_offset(0, 0, lv.pitch, lv.gob_height), block, 8);
        bc1(0x001F, 0x0000, 0x55555555, block);
        std::memcpy(pool.data() + nvn::block_linear_offset(8, 0, lv.pitch, lv.gob_height), block, 8);
        bc1(0x0000, 0x07E0, 0xFFFFFFFF, block);
        std::memcpy(pool.data() + nvn::block_linear_offset(16, 0, lv.pitch, lv.gob_height), block, 8);
        bd.storage = memory.put(0x48000, pool.data(), pool.size());
        const auto before = backend.stats();
        backend.create_texture(31, bd);
        CHECK(backend.read_texture(31, &px, nullptr, nullptr));
        CHECK_EQ(px.size(), size_t(10 * 4 * 4));
        if (px.size() == 10 * 4 * 4) {
            CHECK(texel_is(px, 10, 0, 0, 255, 0, 0, 255));
            CHECK(texel_is(px, 10, 3, 3, 255, 0, 0, 255));
            CHECK(texel_is(px, 10, 4, 0, 0, 0, 0, 255));
            CHECK(texel_is(px, 10, 7, 3, 0, 0, 0, 255));
            CHECK(texel_is(px, 10, 8, 0, 0, 0, 0, 0));
            CHECK(texel_is(px, 10, 9, 3, 0, 0, 0, 0));
        }
        const auto after = backend.stats();
        CHECK_EQ(after.bc_cpu_decoded - before.bc_cpu_decoded, 1u);
        // Over the texture budget, unused pool textures lose their GPU side;
        // the next use uploads them again (P7.3).
        backend.set_texture_budget(1);
        for (int i = 0; i < 4; ++i)
            CHECK(backend.present({13, {}}));
        const auto evicted = backend.stats();
        CHECK(evicted.textures_evicted > after.textures_evicted);
        backend.set_texture_budget(0);
        px.clear();
        CHECK(backend.read_texture(31, &px, nullptr, nullptr));
        if (px.size() == 10 * 4 * 4) {
            CHECK(texel_is(px, 10, 0, 0, 255, 0, 0, 255));
            CHECK(texel_is(px, 10, 9, 3, 0, 0, 0, 0));
        }
        CHECK_EQ(backend.stats().bc_cpu_decoded - evicted.bc_cpu_decoded, 1u);
        // Evicted again, then presented directly: present uploads it first.
        backend.set_texture_budget(1);
        for (int i = 0; i < 4; ++i)
            CHECK(backend.present({13, {}}));
        backend.set_texture_budget(0);
        const auto before_present = backend.stats();
        CHECK(before_present.textures_evicted > evicted.textures_evicted);
        CHECK(backend.present({31, {}}));
        CHECK_EQ(backend.stats().bc_cpu_decoded - before_present.bc_cpu_decoded, 1u);
        backend.force_cpu_bc(false);
    }

    // Shadow lookups (TEXS.DC): a depth texture bound as texture_depth_2d with
    // a comparison sampler from the NVN sampler state. Depth cleared to 0.75,
    // reference 0.5 (c0[0]): LESS passes, GREATER fails. The unbound stand-in
    // (depth 0) would give the opposite results.
    {
        const std::vector<uint8_t> sfs_bin = read_hex(dir + "/fs_shadow.hex");
        wgsl::Result sfs;
        CHECK(translate(sfs_bin, fs_opt, &sfs));
        CHECK(sfs.bindings.texture_depth == std::vector<bool>{true});
        nvn::ProgramDesc sp;
        sp.vertex_wgsl = vs.code;
        sp.fragment_wgsl = sfs.code;
        sp.vertex_cbufs = vs.bindings.cbuf_banks;
        sp.fragment_cbufs = sfs.bindings.cbuf_banks;
        sp.fragment_textures = {0};
        sp.fragment_texture_depth = sfs.bindings.texture_depth;
        backend.create_program(50, sp);
        nvn::TextureDesc dd = td;
        dd.format = uint32_t(nvn::Format::Depth32F);
        backend.create_texture(51, dd);
        backend.create_texture(52, td);
        backend.create_texture(53, td);
        nvn::CommandList sl;
        sl.commands.push_back(nvn::CmdSetRenderTargets{{52}, 51});
        sl.commands.push_back(nvn::CmdClearDepthStencil{0.75f, true, 0, 0});
        sl.commands.push_back(nvn::CmdBindProgram{50});
        sl.commands.push_back(layout);
        sl.commands.push_back(nvn::CmdBindVertexBuffer{0, vb_va, verts.size() * 4});
        sl.commands.push_back(
            nvn::CmdBindUniformBuffer{nvn::ShaderStage::Vertex, nvn::kUniformBufferBankBase, cb_va, 64});
        sl.commands.push_back(nvn::CmdBindUniformBuffer{nvn::ShaderStage::Fragment, 0, cb_va, 64});
        sl.commands.push_back(nvn::CmdSetViewports{0, {0, 0, 8, 8}});
        for (const auto& [target, func] : {std::pair{52u, nvn::CompareFunc::Less}, {53u, nvn::CompareFunc::Greater}}) {
            nvn::CmdBindTexture bt;
            bt.stage = nvn::ShaderStage::Fragment;
            bt.slot = 0;
            bt.texture = 51;
            bt.sampler.compare = true;
            bt.sampler.compare_func = func;
            sl.commands.push_back(nvn::CmdSetRenderTargets{{target}, 0});
            sl.commands.push_back(nvn::CmdClearColor{0, {0.2f, 0.2f, 0.2f, 1.0f}, nvn::kClearColorMaskRGBA});
            sl.commands.push_back(bt);
            sl.commands.push_back(nvn::CmdDraw{nvn::Topology::Triangles, 0, 12, 0, 1});
        }
        const auto before = backend.stats();
        backend.execute(sl);
        CHECK_EQ(backend.stats().invalid_pipelines, before.invalid_pipelines);
        CHECK_EQ(backend.stats().unbound_textures, before.unbound_textures);
        CHECK(backend.read_texture(52, &px, nullptr, nullptr));
        if (px.size() == 8 * 8 * 4) CHECK(texel_is(px, 8, 3, 3, 255, 255, 255, 255));  // 0.5 < 0.75
        CHECK(backend.read_texture(53, &px, nullptr, nullptr));
        if (px.size() == 8 * 8 * 4) CHECK(texel_is(px, 8, 3, 3, 0, 0, 0, 255));  // 0.5 > 0.75 fails

        // The same lookup on an RGBA8 texture from the memory pool (LGP binds
        // a 16x16 RGBA8 texture to a shadow lookup): compared against the red
        // channel through a Depth16Unorm stand-in. Red 191 (~0.749).
        nvn::TextureDesc cd = td;
        cd.format = uint32_t(nvn::Format::RGBA8);
        const nvn::TextureLayout clay = nvn::texture_layout(cd.format, cd.target, cd.width, cd.height, 1, 1);
        std::vector<uint8_t> cpool(size_t(clay.total_size), 0);
        for (uint32_t y = 0; y < cd.height; ++y)
            for (uint32_t x = 0; x < cd.width; ++x) {
                const uint8_t texel[4] = {191, 0, 0, 255};
                std::memcpy(cpool.data() +
                                nvn::block_linear_offset(x * 4, y, clay.levels[0].pitch, clay.levels[0].gob_height),
                            texel, 4);
            }
        cd.storage = memory.put(0x40000, cpool.data(), cpool.size());
        backend.create_texture(160, cd);
        nvn::CommandList cl;
        cl.commands.push_back(nvn::CmdBindProgram{50});
        for (const auto& [target, func] : {std::pair{52u, nvn::CompareFunc::Less}, {53u, nvn::CompareFunc::Greater}}) {
            nvn::CmdBindTexture bt;
            bt.stage = nvn::ShaderStage::Fragment;
            bt.slot = 0;
            bt.texture = 160;
            bt.sampler.compare = true;
            bt.sampler.compare_func = func;
            cl.commands.push_back(nvn::CmdSetRenderTargets{{target}, 0});
            cl.commands.push_back(nvn::CmdClearColor{0, {0.2f, 0.2f, 0.2f, 1.0f}, nvn::kClearColorMaskRGBA});
            cl.commands.push_back(bt);
            cl.commands.push_back(nvn::CmdDraw{nvn::Topology::Triangles, 0, 12, 0, 1});
        }
        const auto before_rgba = backend.stats();
        backend.execute(cl);
        CHECK_EQ(backend.stats().invalid_pipelines, before_rgba.invalid_pipelines);
        CHECK_EQ(backend.stats().unbound_textures, before_rgba.unbound_textures);
        CHECK(backend.read_texture(52, &px, nullptr, nullptr));
        if (px.size() == 8 * 8 * 4) CHECK(texel_is(px, 8, 3, 3, 255, 255, 255, 255));  // 0.5 < 0.749
        CHECK(backend.read_texture(53, &px, nullptr, nullptr));
        if (px.size() == 8 * 8 * 4) CHECK(texel_is(px, 8, 3, 3, 0, 0, 0, 255));  // 0.5 > 0.749 fails
        backend.destroy_texture(160);
        backend.destroy_program(50);

        // Border addressing (wgsl::Options::sampler_border): the same shader
        // with u = 2.0 instead of 0.5, a CLAMP_TO_BORDER sampler on u with
        // border depth 0. Outside the map the lookup compares with the border
        // (0.5 < 0 fails, 0.5 > 0 passes), not with the edge texel (0.75),
        // which gave the opposite results before.
        std::vector<uint8_t> bfs_bin = sfs_bin;
        const uint64_t mov_half = 0x0103f0000007f000ull, mov_two = 0x010400000007f000ull;  // mov32i $r0 0.5 / 2.0
        bool patched = false;
        for (size_t o = 0; o + 8 <= bfs_bin.size() && !patched; o += 8) {
            uint64_t w;
            std::memcpy(&w, bfs_bin.data() + o, 8);
            if (w == mov_half) {
                std::memcpy(bfs_bin.data() + o, &mov_two, 8);
                patched = true;
            }
        }
        CHECK(patched);
        wgsl::Options bfs_opt = fs_opt;
        bfs_opt.texture_swizzle = bfs_opt.sampler_border = true;
        wgsl::Result bfs;
        CHECK(translate(bfs_bin, bfs_opt, &bfs));
        CHECK(bfs.code.find("tex_border(") != std::string::npos);
        nvn::ProgramDesc bp = sp;
        bp.fragment_wgsl = bfs.code;
        bp.texture_swizzle = true;
        backend.create_program(54, bp);
        nvn::CommandList bl;
        bl.commands.push_back(nvn::CmdBindProgram{54});
        for (const auto& [target, func] : {std::pair{52u, nvn::CompareFunc::Less}, {53u, nvn::CompareFunc::Greater}}) {
            nvn::CmdBindTexture bt;
            bt.stage = nvn::ShaderStage::Fragment;
            bt.slot = 0;
            bt.texture = 51;
            bt.sampler.compare = true;
            bt.sampler.compare_func = func;
            bt.sampler.wrap[0] = nvn::AddressMode::ClampToBorder;
            bl.commands.push_back(nvn::CmdSetRenderTargets{{target}, 0});
            bl.commands.push_back(nvn::CmdClearColor{0, {0.2f, 0.2f, 0.2f, 1.0f}, nvn::kClearColorMaskRGBA});
            bl.commands.push_back(bt);
            bl.commands.push_back(nvn::CmdDraw{nvn::Topology::Triangles, 0, 12, 0, 1});
        }
        const auto before_border = backend.stats();
        backend.execute(bl);
        CHECK_EQ(backend.stats().invalid_pipelines, before_border.invalid_pipelines);
        CHECK_EQ(backend.stats().unbound_textures, before_border.unbound_textures);
        CHECK(backend.read_texture(52, &px, nullptr, nullptr));
        if (px.size() == 8 * 8 * 4) CHECK(texel_is(px, 8, 3, 3, 0, 0, 0, 255));  // 0.5 < 0 (border) fails
        CHECK(backend.read_texture(53, &px, nullptr, nullptr));
        if (px.size() == 8 * 8 * 4) CHECK(texel_is(px, 8, 3, 3, 255, 255, 255, 255));  // 0.5 > 0 (border)
        // Samplers are kept per distinct description (Stats::samplers):
        // three new LOD clamps add three, the same three again add none.
        {
            nvn::CommandList ll;
            ll.commands.push_back(nvn::CmdBindProgram{54});
            ll.commands.push_back(nvn::CmdSetRenderTargets{{52u}, 0});
            for (float lod : {1.0f, 2.0f, 3.0f}) {
                nvn::CmdBindTexture bt;
                bt.stage = nvn::ShaderStage::Fragment;
                bt.slot = 0;
                bt.texture = 51;
                bt.sampler.compare = true;
                bt.sampler.compare_func = nvn::CompareFunc::Less;
                bt.sampler.wrap[0] = nvn::AddressMode::ClampToBorder;
                bt.sampler.min_lod = lod;
                ll.commands.push_back(bt);
                ll.commands.push_back(nvn::CmdDraw{nvn::Topology::Triangles, 0, 12, 0, 1});
            }
            const uint64_t samplers_before = backend.stats().samplers;
            CHECK(samplers_before >= 2);  // the border draws' two
            backend.execute(ll);
            CHECK_EQ(backend.stats().samplers, samplers_before + 3);
            backend.execute(ll);
            CHECK_EQ(backend.stats().samplers, samplers_before + 3);
        }
        // A destroyed render target leaves the depth partners (Stats::depth_partners).
        {
            backend.create_texture(59, td);
            const uint64_t partners_before = backend.stats().depth_partners;
            nvn::CommandList pl;
            pl.commands.push_back(nvn::CmdSetRenderTargets{{59u}, 0});
            pl.commands.push_back(nvn::CmdClearColor{0, {0.0f, 0.0f, 0.0f, 1.0f}, nvn::kClearColorMaskRGBA});
            backend.execute(pl);
            CHECK_EQ(backend.stats().depth_partners, partners_before + 1);
            backend.destroy_texture(59);
            CHECK_EQ(backend.stats().depth_partners, partners_before);
        }
        backend.destroy_program(54);

        // Reference clamp (wgsl::kCompareClampRef): on fixed-point depth the
        // hardware clamps the reference to [0, 1]; WebGPU may back Depth24Plus
        // with float. Depth 1.0, reference 1.5, LESS_EQUAL: passes after the clamp.
        wgsl::Result cfs;
        CHECK(translate(sfs_bin, bfs_opt, &cfs));
        nvn::ProgramDesc cp = sp;
        cp.fragment_wgsl = cfs.code;
        cp.texture_swizzle = true;
        backend.create_program(56, cp);
        nvn::TextureDesc d24 = td;
        d24.format = uint32_t(nvn::Format::Depth24);
        backend.create_texture(55, d24);
        const float ref15[4] = {1.5f, 0.0f, 0.0f, 0.0f};
        const uint64_t ref_va = memory.put(0x47000, ref15, sizeof(ref15));
        nvn::CommandList rl;
        rl.commands.push_back(nvn::CmdSetRenderTargets{{52}, 55});
        rl.commands.push_back(nvn::CmdClearDepthStencil{1.0f, true, 0, 0});
        rl.commands.push_back(nvn::CmdSetRenderTargets{{52}, 0});
        rl.commands.push_back(nvn::CmdClearColor{0, {0.2f, 0.2f, 0.2f, 1.0f}, nvn::kClearColorMaskRGBA});
        rl.commands.push_back(nvn::CmdBindProgram{56});
        rl.commands.push_back(nvn::CmdBindUniformBuffer{nvn::ShaderStage::Fragment, 0, ref_va, 16});
        nvn::CmdBindTexture rt_bind;
        rt_bind.stage = nvn::ShaderStage::Fragment;
        rt_bind.slot = 0;
        rt_bind.texture = 55;
        rt_bind.sampler.compare = true;
        rt_bind.sampler.compare_func = nvn::CompareFunc::LessEqual;
        rl.commands.push_back(rt_bind);
        rl.commands.push_back(nvn::CmdDraw{nvn::Topology::Triangles, 0, 12, 0, 1});
        rl.commands.push_back(nvn::CmdBindUniformBuffer{nvn::ShaderStage::Fragment, 0, cb_va, 64});
        const auto before_ref = backend.stats();
        backend.execute(rl);
        CHECK_EQ(backend.stats().unbound_textures, before_ref.unbound_textures);
        {
            // LGP_WEBGPU_FOLD_CLEARS: the colour clear folds into the draw
            // pass; the depth clear of the sampled texture 55 cannot (the
            // draw samples it, no pass has it as attachment) and gets its
            // own pass before the draw.
            const bool fold = pass_switch("LGP_WEBGPU_FOLD_CLEARS");
            const auto after_ref = backend.stats();
            auto cause = [&](const char* why) {
                const auto a = after_ref.pass_reasons.find(why), b = before_ref.pass_reasons.find(why);
                return (a != after_ref.pass_reasons.end() ? a->second : 0) -
                       (b != before_ref.pass_reasons.end() ? b->second : 0);
            };
            CHECK_EQ(cause("own pass: depth/stencil clear"), 1u);
            CHECK_EQ(cause("own pass: colour clear"), fold ? 0u : 1u);
            CHECK_EQ(cause("folded: colour clear (no own pass)"), fold ? 1u : 0u);
            CHECK_EQ(after_ref.render_passes - before_ref.render_passes, fold ? 2u : 3u);
        }
        CHECK(backend.read_texture(52, &px, nullptr, nullptr));
        if (px.size() == 8 * 8 * 4) CHECK(texel_is(px, 8, 3, 3, 255, 255, 255, 255));  // clamp(1.5) <= 1
        backend.destroy_texture(55);
        backend.destroy_program(56);

        // Percentage-closer filtering in the shader, LGP's shadow pattern:
        // ten TEXS 2D.DC lookups on handle 0 at (u, v) plus offsets from the
        // constant buffer, summed and scaled by 0.1, through a nearest compare
        // sampler. Depth map columns 0-3 hold 0.25, columns 4-7 hold 0.75;
        // three taps land on the left half, seven on the right. Reference
        // 0.5 with LESS: the mean is 0.7. Collapsed taps or a wrong
        // coordinate/reference operand give 0 or 1 instead.
        const std::vector<uint8_t> pcf_bin = read_hex(dir + "/fs_shadow_pcf.hex");
        wgsl::Result pfs;
        CHECK(translate(pcf_bin, bfs_opt, &pfs));
        size_t compares = 0;
        for (size_t at = pfs.code.find("textureSampleCompare"); at != std::string::npos;
             at = pfs.code.find("textureSampleCompare", at + 1))
            ++compares;
        CHECK_EQ(compares, 10u);
        {
            // LGP_WGSL_COMPARE_CONST (diagnosis): no lookup is left, each yields 1.
            wgsl::Options copt = bfs_opt;
            copt.compare_const = true;
            wgsl::Result cfs;
            CHECK(translate(pcf_bin, copt, &cfs));
            CHECK(cfs.code.find("textureSampleCompare") == std::string::npos);
            CHECK(cfs.code.find("vec4<f32>(1.0)") != std::string::npos);
            // The shader has to make a pipeline too (naga rejected a let of
            // the constant after override processing).
            nvn::ProgramDesc cp = sp;
            cp.fragment_wgsl = cfs.code;
            cp.fragment_cbufs = cfs.bindings.cbuf_banks;
            cp.fragment_texture_depth = cfs.bindings.texture_depth;
            cp.texture_swizzle = true;
            backend.create_program(64, cp);
        }
        nvn::ProgramDesc pp = sp;
        pp.fragment_wgsl = pfs.code;
        pp.fragment_cbufs = pfs.bindings.cbuf_banks;
        pp.fragment_texture_depth = pfs.bindings.texture_depth;
        pp.texture_swizzle = true;
        backend.create_program(63, pp);
        backend.create_texture(61, dd);
        backend.create_texture(62, td);
        std::vector<float> pcb(0x60 / 4, 0.0f);
        pcb[0] = 0.5f;                     // reference
        pcb[1] = pcb[2] = 4.5f / 8.0f;     // texel (4, 4)
        const int tap_columns[10] = {1, 2, 3, 4, 5, 6, 7, 4, 5, 6};
        for (int k = 0; k < 10; ++k) pcb[4 + 2 * k] = float(tap_columns[k] - 4) / 8.0f;
        const uint64_t pcb_va = memory.put(0x47100, pcb.data(), pcb.size() * 4);
        nvn::CommandList pl;
        pl.commands.push_back(nvn::CmdSetRenderTargets{{62}, 61});
        pl.commands.push_back(nvn::CmdClearDepthStencil{0.75f, true, 0, 0});
        pl.commands.push_back(nvn::CmdSetScissors{0, {{0, 0, 4, 8}}});
        pl.commands.push_back(nvn::CmdClearDepthStencil{0.25f, true, 0, 0});
        pl.commands.push_back(nvn::CmdSetScissors{0, {{0, 0, 8, 8}}});
        pl.commands.push_back(nvn::CmdSetRenderTargets{{62}, 0});
        pl.commands.push_back(nvn::CmdClearColor{0, {0.2f, 0.2f, 0.2f, 1.0f}, nvn::kClearColorMaskRGBA});
        pl.commands.push_back(nvn::CmdBindProgram{63});
        pl.commands.push_back(nvn::CmdBindUniformBuffer{nvn::ShaderStage::Fragment, 0, pcb_va, 0x60});
        nvn::CmdBindTexture pcf_bind;
        pcf_bind.stage = nvn::ShaderStage::Fragment;
        pcf_bind.slot = 0;
        pcf_bind.texture = 61;
        pcf_bind.sampler.min = pcf_bind.sampler.mag = nvn::Filter::Nearest;  // like LGP's shadow sampler
        pcf_bind.sampler.compare = true;
        pcf_bind.sampler.compare_func = nvn::CompareFunc::Less;
        pl.commands.push_back(pcf_bind);
        pl.commands.push_back(nvn::CmdDraw{nvn::Topology::Triangles, 0, 12, 0, 1});
        pl.commands.push_back(nvn::CmdBindUniformBuffer{nvn::ShaderStage::Fragment, 0, cb_va, 64});
        const auto before_pcf = backend.stats();
        backend.execute(pl);
        CHECK_EQ(backend.stats().invalid_pipelines, before_pcf.invalid_pipelines);
        CHECK_EQ(backend.stats().unbound_textures, before_pcf.unbound_textures);
        CHECK(backend.read_texture(62, &px, nullptr, nullptr));
        if (px.size() == 8 * 8 * 4) {
            CHECK(texel_is(px, 8, 3, 3, 179, 179, 179, 255));  // 7 of 10 taps pass
            CHECK(texel_is(px, 8, 6, 5, 179, 179, 179, 255));  // same taps everywhere
        }
        // The same draw with LGP_WGSL_COMPARE_CONST's shader: every tap passes.
        pl.commands[7] = nvn::CmdBindProgram{64};
        const auto before_const = backend.stats();
        backend.execute(pl);
        CHECK_EQ(backend.stats().invalid_pipelines, before_const.invalid_pipelines);
        CHECK(backend.read_texture(62, &px, nullptr, nullptr));
        if (px.size() == 8 * 8 * 4) CHECK(texel_is(px, 8, 3, 3, 255, 255, 255, 255));
        backend.destroy_program(64);
        backend.destroy_texture(61);
        backend.destroy_texture(62);
        backend.destroy_program(63);
    }

    // Depth textures sampled without compare (plain TEXS on a texture_2d<f32>
    // binding): the backend binds the depth aspect as unfilterable-float with
    // a non-filtering sampler (layout variant per bound texture). LGP binds a
    // Depth24Stencil8 (NVN format 53) target like this. Depth 0.75 / 0.25
    // arrives in .r; colour (1, 1, 1, 1) keeps it.
    {
        const std::vector<uint8_t> tex_bin = read_hex(dir + "/fs_textured.hex");
        lgp::gpu::shader::Sph tex_sph;
        CHECK(lgp::gpu::shader::parse_sph(tex_bin, tex_sph, error));
        wgsl::Options tvs_opt = vs_opt;
        tvs_opt.vertex_output_interp = wgsl::fragment_input_interpolation(tex_sph);
        wgsl::Result tvs, tfs;
        CHECK(translate(vs_bin, tvs_opt, &tvs));
        CHECK(translate(tex_bin, fs_opt, &tfs));
        CHECK(tfs.bindings.texture_depth == std::vector<bool>{false});
        backend.create_program(54, {tvs.code, tfs.code, tvs.bindings.cbuf_banks, tfs.bindings.cbuf_banks, {}, {0}});
        std::vector<uint32_t> ident(16, 0);
        for (int i = 0; i < 16; i += 5) ident[i] = f2u(1.0f);
        const uint64_t ident_va = memory.put(0x25000, ident.data(), ident.size() * 4);
        std::vector<float> full;
        const float white[4] = {1, 1, 1, 1};
        quad(full, -1, -1, 1, 1, white);
        const uint64_t full_va = memory.put(0x26000, full.data(), full.size() * 4);
        const std::pair<uint32_t, nvn::Format> depths[] = {{55, nvn::Format::Depth24Stencil8},
                                                           {56, nvn::Format::Depth32F}};
        for (const auto& [id, format] : depths) {
            nvn::TextureDesc dd = td;
            dd.format = uint32_t(format);
            backend.create_texture(id, dd);
        }
        backend.create_texture(57, td);
        backend.create_texture(58, td);
        nvn::CommandList dl;
        dl.commands.push_back(nvn::CmdSetRenderTargets{{57}, 55});
        dl.commands.push_back(nvn::CmdClearDepthStencil{0.75f, true, 0, 0});
        dl.commands.push_back(nvn::CmdSetRenderTargets{{58}, 56});
        dl.commands.push_back(nvn::CmdClearDepthStencil{0.25f, true, 0, 0});
        dl.commands.push_back(nvn::CmdBindProgram{54});
        dl.commands.push_back(layout);
        dl.commands.push_back(nvn::CmdBindVertexBuffer{0, full_va, full.size() * 4});
        dl.commands.push_back(
            nvn::CmdBindUniformBuffer{nvn::ShaderStage::Vertex, nvn::kUniformBufferBankBase, ident_va, 64});
        dl.commands.push_back(nvn::CmdSetViewports{0, {0, 0, 8, 8}});
        for (const auto& [target, depth] : {std::pair{57u, 55u}, {58u, 56u}}) {
            nvn::CmdBindTexture bt;
            bt.stage = nvn::ShaderStage::Fragment;
            bt.slot = 0;
            bt.texture = depth;  // linear filtering in the NVN sampler: becomes nearest
            dl.commands.push_back(nvn::CmdSetRenderTargets{{target}, 0});
            dl.commands.push_back(nvn::CmdClearColor{0, {0, 0, 0, 0}, nvn::kClearColorMaskRGBA});
            dl.commands.push_back(bt);
            dl.commands.push_back(nvn::CmdDraw{nvn::Topology::Triangles, 0, 6, 0, 1});
        }
        const auto before = backend.stats();
        backend.execute(dl);
        CHECK_EQ(backend.stats().invalid_pipelines, before.invalid_pipelines);
        CHECK_EQ(backend.stats().unbound_textures, before.unbound_textures);
        for (const auto& [target, red] : {std::pair{57u, 191}, {58u, 64}}) {
            CHECK(backend.read_texture(target, &px, nullptr, nullptr));
            if (px.size() == 8 * 8 * 4) {
                const uint8_t* p = px.data() + (3 * 8 + 3) * 4;
                if (std::abs(p[0] - red) > 1)
                    std::printf("  depth sampled as colour: red %u, want %d\n", p[0], red);
                CHECK(std::abs(p[0] - red) <= 1);  // other channels: WebGPU leaves them open
            }
        }

        // The same depth texture as attachment and texture at once (LGP does
        // this with depth writes off): read-only attachment, the texture is
        // read. With depth writes on WebGPU forbids it: stand-in (0) and a
        // counted reason, no usage conflict (wgpu panicked there).
        nvn::CommandList al;
        al.commands.push_back(nvn::CmdBindProgram{54});
        al.commands.push_back(layout);
        al.commands.push_back(nvn::CmdBindVertexBuffer{0, full_va, full.size() * 4});
        al.commands.push_back(
            nvn::CmdBindUniformBuffer{nvn::ShaderStage::Vertex, nvn::kUniformBufferBankBase, ident_va, 64});
        al.commands.push_back(nvn::CmdSetViewports{0, {0, 0, 8, 8}});
        nvn::CmdBindTexture self;
        self.stage = nvn::ShaderStage::Fragment;
        self.slot = 0;
        self.texture = 55;
        al.commands.push_back(self);
        for (const auto& [target, write] : {std::pair{57u, false}, {58u, true}}) {
            nvn::CmdSetDepthStencil ds;
            ds.depth_test = true;
            ds.depth_write = write;
            ds.depth_func = nvn::CompareFunc::Always;
            al.commands.push_back(nvn::CmdSetRenderTargets{{target}, 55});
            al.commands.push_back(nvn::CmdClearColor{0, {0, 0, 0, 0}, nvn::kClearColorMaskRGBA});
            al.commands.push_back(ds);
            al.commands.push_back(nvn::CmdDraw{nvn::Topology::Triangles, 0, 6, 0, 1});
        }
        const auto before_self = backend.stats();
        backend.execute(al);
        CHECK_EQ(backend.stats().invalid_pipelines, before_self.invalid_pipelines);
        CHECK_EQ(backend.stats().unbound_textures, before_self.unbound_textures + 1);  // the writing draw
        for (const auto& [target, red] : {std::pair{57u, 191}, {58u, 0}}) {
            CHECK(backend.read_texture(target, &px, nullptr, nullptr));
            if (px.size() == 8 * 8 * 4) {
                const uint8_t* p = px.data() + (3 * 8 + 3) * 4;
                if (std::abs(p[0] - red) > 1)
                    std::printf("  depth target sampled: red %u, want %d\n", p[0], red);
                CHECK(std::abs(p[0] - red) <= 1);
            }
        }
        backend.destroy_program(54);
    }

    // Integer vertex formats (RGBA8UI bone indices, RGBA8I): the shader reads
    // raw integers; the backend retypes the vertex inputs for this layout.
    {
        const std::vector<uint8_t> ivs_bin = read_hex(dir + "/vs_int_input.hex");
        const std::vector<uint8_t> ifs_bin = read_hex(dir + "/fs_flat_scale.hex");
        lgp::gpu::shader::Sph ifs_sph;
        CHECK(lgp::gpu::shader::parse_sph(ifs_bin, ifs_sph, error));
        wgsl::Options ivs_opt = vs_opt, ifs_opt = fs_opt;
        ivs_opt.vertex_output_interp = wgsl::fragment_input_interpolation(ifs_sph);
        wgsl::Result ivs, ifs;
        CHECK(translate(ivs_bin, ivs_opt, &ivs));
        CHECK(translate(ifs_bin, ifs_opt, &ifs));
        backend.create_program(40, {ivs.code, ifs.code, ivs.bindings.cbuf_banks, ifs.bindings.cbuf_banks});
        struct Vertex {
            float pos[4];
            uint8_t idx[4];
            int8_t sidx[4];
        };
        std::vector<Vertex> iv;
        for (const auto& c : {std::array<float, 2>{-3, 1}, {1, -3}, {1, 1}})
            iv.push_back({{c[0], c[1], 0, 1}, {3, 5, 200, 7}, {-4, 0, 0, 0}});
        static_assert(sizeof(Vertex) == 24);
        const uint64_t iv_va = memory.put(0x24000, iv.data(), iv.size() * sizeof(Vertex));
        nvn::TextureDesc it = td;
        backend.create_texture(41, it);
        nvn::CommandList il;
        nvn::CmdSetVertexLayout ilayout;
        ilayout.attribs = {{0, nvn::VertexFormat::Float32x4, 0, 0},
                           {1, nvn::VertexFormat::Uint8x4, 0, 16},
                           {2, nvn::VertexFormat::Sint8x4, 0, 20}};
        ilayout.strides = {24};
        il.commands.push_back(nvn::CmdBindProgram{40});
        il.commands.push_back(ilayout);
        il.commands.push_back(nvn::CmdBindVertexBuffer{0, iv_va, iv.size() * sizeof(Vertex)});
        il.commands.push_back(nvn::CmdSetRenderTargets{{41}, 0});
        il.commands.push_back(nvn::CmdSetViewports{0, {0, 0, 8, 8}});
        il.commands.push_back(nvn::CmdClearColor{0, {0, 0, 0, 0}, nvn::kClearColorMaskRGBA});
        il.commands.push_back(nvn::CmdDraw{nvn::Topology::Triangles, 0, 3, 0, 1});
        const auto before = backend.stats();
        backend.execute(il);
        CHECK_EQ(backend.stats().invalid_pipelines, before.invalid_pipelines);
        CHECK(backend.read_texture(41, &px, nullptr, nullptr));
        if (px.size() == 8 * 8 * 4)
            CHECK(texel_is(px, 8, 4, 4, 8, 200, 7, 4));  // (3 + 5, 200, 7, -(-4))
        backend.destroy_program(40);
    }

    // Vertex formats WebGPU lacks (NVN vertex-only 81-108): a packed
    // 10:10:10:2 word read as signed integers converted to float (RGB10A2_I2F),
    // fetched as uint32 and unpacked in the vertex shader variant.
    {
        const std::vector<uint8_t> pvs_bin = read_hex(dir + "/vs_float_input.hex");
        const std::vector<uint8_t> pfs_bin = read_hex(dir + "/fs_flat_scale.hex");
        lgp::gpu::shader::Sph pfs_sph;
        CHECK(lgp::gpu::shader::parse_sph(pfs_bin, pfs_sph, error));
        wgsl::Options pvs_opt = vs_opt;
        pvs_opt.vertex_output_interp = wgsl::fragment_input_interpolation(pfs_sph);
        wgsl::Result pvs, pfs;
        CHECK(translate(pvs_bin, pvs_opt, &pvs));
        CHECK(translate(pfs_bin, fs_opt, &pfs));
        backend.create_program(42, {pvs.code, pfs.code, pvs.bindings.cbuf_banks, pfs.bindings.cbuf_banks});
        struct Vertex {
            float pos[4];
            uint32_t packed;
        };
        const auto word = [](int x, int y, int z, int w) {
            return uint32_t(x & 1023) | uint32_t(y & 1023) << 10 | uint32_t(z & 1023) << 20 | uint32_t(w & 3) << 30;
        };
        std::vector<Vertex> pv;
        for (const auto& c : {std::array<float, 2>{-3, 1}, {1, -3}, {1, 1}})
            pv.push_back({{c[0], c[1], 0, 1}, word(8, 200, 7, -2)});
        static_assert(sizeof(Vertex) == 20);
        const uint64_t pv_va = memory.put(0x27000, pv.data(), pv.size() * sizeof(Vertex));
        backend.create_texture(43, td);
        nvn::CommandList pl;
        nvn::CmdSetVertexLayout playout;
        playout.attribs = {{0, nvn::VertexFormat::Float32x4, 0, 0}, {1, nvn::VertexFormat::SScaled10_10_10_2, 0, 16}};
        playout.strides = {20};
        pl.commands.push_back(nvn::CmdBindProgram{42});
        pl.commands.push_back(playout);
        pl.commands.push_back(nvn::CmdBindVertexBuffer{0, pv_va, pv.size() * sizeof(Vertex)});
        pl.commands.push_back(nvn::CmdSetRenderTargets{{43}, 0});
        pl.commands.push_back(nvn::CmdSetViewports{0, {0, 0, 8, 8}});
        pl.commands.push_back(nvn::CmdClearColor{0, {0, 0, 0, 0}, nvn::kClearColorMaskRGBA});
        pl.commands.push_back(nvn::CmdDraw{nvn::Topology::Triangles, 0, 3, 0, 1});
        const auto before = backend.stats();
        backend.execute(pl);
        CHECK_EQ(backend.stats().invalid_pipelines, before.invalid_pipelines);
        CHECK(backend.read_texture(43, &px, nullptr, nullptr));
        if (px.size() == 8 * 8 * 4)
            CHECK(texel_is(px, 8, 4, 4, 8, 200, 7, 2));  // (8, 200, 7, -(-2))
        backend.destroy_program(42);
    }

    // ---- NVN state set on the command buffer and instancing (P4, nvn-hle.md
    // "Zustandsbefehle"). State carries over between lists, so every list
    // below starts by setting what it relies on.
    auto reset_state = [](nvn::CommandList& l, std::vector<nvn::TextureId> colors, nvn::TextureId depth = 0) {
        l.commands.push_back(nvn::CmdSetRenderTargets{std::move(colors), depth});
        l.commands.push_back(nvn::CmdSetViewports{0, {0, 0, 8, 8}});
        l.commands.push_back(nvn::CmdSetDepthRanges{0, {0.0f, 1.0f}});
        l.commands.push_back(nvn::CmdSetScissors{0, {{0, 0, 8, 8}}});
        l.commands.push_back(nvn::CmdSetBlendEnables{0});
        l.commands.push_back(nvn::CmdSetChannelMasks{});
        l.commands.push_back(nvn::CmdSetRaster{});
        l.commands.push_back(nvn::CmdSetDepthStencil{});
        l.commands.push_back(nvn::CmdSetDepthBias{});
        l.commands.push_back(nvn::CmdSetDepthClamp{});
        l.commands.push_back(nvn::CmdSetRasterizerDiscard{});
        l.commands.push_back(nvn::CmdClearColor{0, {0, 0, 0, 1}, nvn::kClearColorMaskRGBA});
    };
    // Handwritten programs: 140 = instanced columns (a0 corner per vertex, a1
    // column per instance, a2 colour per instance stream), 141 = position as
    // given, white.
    const char* kInstVs = R"(
struct VOut { @builtin(position) pos: vec4<f32>, @location(0) color: vec4<f32>, }
@vertex fn main(@location(0) a0: vec4<f32>, @location(1) a1: vec4<f32>, @location(2) a2: vec4<f32>) -> VOut {
    return VOut(vec4<f32>(-1.0 + 0.5 * (a1.x + a0.x), -1.0 + 2.0 * a0.y, 0.5, 1.0), a2);
}
)";
    const char* kColorFs = R"(
@fragment fn main(@location(0) c: vec4<f32>) -> @location(0) vec4<f32> { return c; }
)";
    const char* kPosVs = R"(
@vertex fn main(@location(0) a0: vec4<f32>) -> @builtin(position) vec4<f32> { return a0; }
)";
    const char* kWhiteFs = R"(
@fragment fn main() -> @location(0) vec4<f32> { return vec4<f32>(1.0); }
)";
    backend.create_program(140, {kInstVs, kColorFs, {}, {}});
    backend.create_program(141, {kPosVs, kWhiteFs, {}, {}});
    CHECK_EQ(backend.stats().invalid_programs, 1u);  // still only program 2
    for (nvn::TextureId id = 150; id <= 153; ++id) backend.create_texture(id, td);

    // Instanced vertex streams: divisor 1 (stepMode instance) and 2 (runs of
    // instances; element = base instance + instance / divisor).
    {
        const float corners[6][4] = {{0, 0, 0, 1}, {1, 0, 0, 1}, {0, 1, 0, 1}, {0, 1, 0, 1}, {1, 0, 0, 1}, {1, 1, 0, 1}};
        const float columns[4][4] = {{0, 0, 0, 0}, {1, 0, 0, 0}, {2, 0, 0, 0}, {3, 0, 0, 0}};
        const float colors[4][4] = {{1, 0, 0, 1}, {0, 1, 0, 1}, {0, 0, 1, 1}, {1, 1, 1, 1}};
        const uint64_t corners_va = memory.put(0x34000, corners, sizeof(corners));
        const uint64_t columns_va = memory.put(0x34100, columns, sizeof(columns));
        const uint64_t colors_va = memory.put(0x34200, colors, sizeof(colors));
        nvn::CmdSetVertexLayout inst;
        inst.attribs = {{0, nvn::VertexFormat::Float32x4, 0, 0},
                        {1, nvn::VertexFormat::Float32x4, 1, 0},
                        {2, nvn::VertexFormat::Float32x4, 2, 0}};
        inst.strides = {16, 16, 16};
        inst.divisors = {0, 1, 2};
        auto draw_into = [&](nvn::TextureId target, const nvn::CmdSetVertexLayout& l, uint32_t first, uint32_t n) {
            nvn::CommandList il;
            reset_state(il, {target});
            il.commands.push_back(nvn::CmdBindProgram{140});
            il.commands.push_back(l);
            il.commands.push_back(nvn::CmdBindVertexBuffer{0, corners_va, sizeof(corners)});
            il.commands.push_back(nvn::CmdBindVertexBuffer{1, columns_va, sizeof(columns)});
            il.commands.push_back(nvn::CmdBindVertexBuffer{2, colors_va, sizeof(colors)});
            il.commands.push_back(nvn::CmdDraw{nvn::Topology::Triangles, 0, 6, first, n});
            backend.execute(il);
        };
        draw_into(150, inst, 0, 4);
        draw_into(151, inst, 1, 3);
        nvn::CmdSetVertexLayout per_instance = inst;
        per_instance.divisors = {0, 1, 1};
        draw_into(152, per_instance, 0, 4);
        const int want[3][4][3] = {{{255, 0, 0}, {255, 0, 0}, {0, 255, 0}, {0, 255, 0}},
                                   {{0, 0, 0}, {0, 255, 0}, {0, 255, 0}, {0, 0, 255}},
                                   {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 255}}};
        for (int t = 0; t < 3; ++t) {
            CHECK(backend.read_texture(nvn::TextureId(150 + t), &px, nullptr, nullptr));
            if (px.size() == 8 * 8 * 4)
                for (int col = 0; col < 4; ++col)
                    CHECK(texel_is(px, 8, uint32_t(2 * col + 1), 3, want[t][col][0], want[t][col][1], want[t][col][2],
                                   255));
        }
        // Instances past the end of a per-instance stream's bound (256-byte
        // rounded) range: skipped, not a WebGPU validation error.
        const auto before = backend.stats();
        draw_into(153, per_instance, 2, 20);
        CHECK_EQ(backend.stats().skip_reasons.count("draw: instanced stream shorter than the instances"), 1u);
        CHECK_EQ(backend.stats().draws, before.draws);
    }

    // Rasterizer discard: draws are dropped (and the state carries over).
    {
        const float full[6][4] = {{-1, -1, 0.5f, 1}, {1, -1, 0.5f, 1}, {-1, 1, 0.5f, 1},
                                  {-1, 1, 0.5f, 1},  {1, -1, 0.5f, 1}, {1, 1, 0.5f, 1}};
        const uint64_t full_va = memory.put(0x34400, full, sizeof(full));
        nvn::CmdSetVertexLayout pos;
        pos.attribs = {{0, nvn::VertexFormat::Float32x4, 0, 0}};
        pos.strides = {16};
        nvn::CommandList dl;
        reset_state(dl, {153});
        dl.commands.push_back(nvn::CmdBindProgram{141});
        dl.commands.push_back(pos);
        dl.commands.push_back(nvn::CmdBindVertexBuffer{0, full_va, sizeof(full)});
        dl.commands.push_back(nvn::CmdSetRasterizerDiscard{true});
        nvn::CommandList again;  // no state commands: discard still on
        again.commands.push_back(nvn::CmdDraw{nvn::Topology::Triangles, 0, 6, 0, 1});
        const auto before = backend.stats();
        backend.execute(dl);
        backend.execute(again);
        CHECK(backend.read_texture(153, &px, nullptr, nullptr));
        if (px.size() == 8 * 8 * 4)
            CHECK(texel_is(px, 8, 4, 4, 0, 0, 0, 255));
        CHECK_EQ(backend.stats().discarded_draws - before.discarded_draws, 1u);
        nvn::CommandList on;
        on.commands.push_back(nvn::CmdSetRasterizerDiscard{false});
        on.commands.push_back(nvn::CmdDraw{nvn::Topology::Triangles, 0, 6, 0, 1});
        backend.execute(on);
        CHECK(backend.read_texture(153, &px, nullptr, nullptr));
        if (px.size() == 8 * 8 * 4)
            CHECK(texel_is(px, 8, 4, 4, 255, 255, 255, 255));

        // Depth clamp: a quad behind the far plane is clipped, with depth
        // clamp (unclippedDepth) it is drawn.
        const float far_quad[6][4] = {{-1, -1, 1.5f, 1}, {1, -1, 1.5f, 1}, {-1, 1, 1.5f, 1},
                                      {-1, 1, 1.5f, 1},  {1, -1, 1.5f, 1}, {1, 1, 1.5f, 1}};
        const uint64_t far_va = memory.put(0x34600, far_quad, sizeof(far_quad));
        nvn::CommandList cl;
        reset_state(cl, {152});
        cl.commands.push_back(nvn::CmdBindProgram{141});
        cl.commands.push_back(pos);
        cl.commands.push_back(nvn::CmdBindVertexBuffer{0, far_va, sizeof(far_quad)});
        cl.commands.push_back(nvn::CmdDraw{nvn::Topology::Triangles, 0, 6, 0, 1});
        backend.execute(cl);
        CHECK(backend.read_texture(152, &px, nullptr, nullptr));
        if (px.size() == 8 * 8 * 4)
            CHECK(texel_is(px, 8, 4, 4, 0, 0, 0, 255));
        nvn::CommandList clamp;
        clamp.commands.push_back(nvn::CmdSetDepthClamp{true});
        clamp.commands.push_back(nvn::CmdDraw{nvn::Topology::Triangles, 0, 6, 0, 1});
        backend.execute(clamp);
        const bool has_clip_control = wgpuDeviceHasFeature(renderer.device(), WGPUFeatureName_DepthClipControl);
        std::printf("  depth-clip-control: %s\n", has_clip_control ? "yes" : "no (depth clamp not testable)");
        CHECK(backend.read_texture(152, &px, nullptr, nullptr));
        if (px.size() == 8 * 8 * 4 && has_clip_control)
            CHECK(texel_is(px, 8, 4, 4, 255, 255, 255, 255));

        // Polygon offset: with depth 0.5 cleared and LESS, a quad at 0.5 fails
        // (left half) unless a negative depth bias pulls it in front (right).
        nvn::TextureDesc dd = td;
        dd.format = uint32_t(nvn::Format::Depth32F);
        backend.create_texture(154, dd);
        const float halves[12][4] = {{-1, -1, 0.5f, 1}, {0, -1, 0.5f, 1}, {-1, 1, 0.5f, 1},
                                     {-1, 1, 0.5f, 1},  {0, -1, 0.5f, 1}, {0, 1, 0.5f, 1},
                                     {0, -1, 0.5f, 1},  {1, -1, 0.5f, 1}, {0, 1, 0.5f, 1},
                                     {0, 1, 0.5f, 1},   {1, -1, 0.5f, 1}, {1, 1, 0.5f, 1}};
        const uint64_t halves_va = memory.put(0x34800, halves, sizeof(halves));
        nvn::CommandList bl;
        reset_state(bl, {151}, 154);
        bl.commands.push_back(nvn::CmdClearDepthStencil{0.5f, true, 0, 0});
        nvn::CmdSetDepthStencil less;
        less.depth_test = true;
        less.depth_func = nvn::CompareFunc::Less;
        bl.commands.push_back(less);
        bl.commands.push_back(nvn::CmdBindProgram{141});
        bl.commands.push_back(pos);
        bl.commands.push_back(nvn::CmdBindVertexBuffer{0, halves_va, sizeof(halves)});
        bl.commands.push_back(nvn::CmdSetDepthBias{0.0f, -100.0f, 0.0f});  // not enabled for triangles yet
        bl.commands.push_back(nvn::CmdDraw{nvn::Topology::Triangles, 0, 6, 0, 1});
        nvn::CmdSetRaster biased;
        biased.depth_bias = nvn::kDepthBiasTriangles;
        bl.commands.push_back(biased);
        bl.commands.push_back(nvn::CmdDraw{nvn::Topology::Triangles, 6, 6, 0, 1});
        backend.execute(bl);
        CHECK(backend.read_texture(151, &px, nullptr, nullptr));
        if (px.size() == 8 * 8 * 4) {
            CHECK(texel_is(px, 8, 1, 4, 0, 0, 0, 255));
            CHECK(texel_is(px, 8, 6, 4, 255, 255, 255, 255));
        }

        // Vertex-only program (no fragment WGSL; depth and shadow passes): it
        // writes depth but no color. The left half drawn with it at 0.5 then
        // hides a white quad at 0.5 (LESS) there; the right half shows it.
        // Once with a color target bound (stub fragment shader, empty write
        // masks), once as a depth-only pass (no fragment stage).
        backend.create_program(143, {kPosVs, "", {}, {}});
        // The HLE's translator makes such a program from a vertex stage and
        // an empty fragment stage.
        nvn::ProgramDesc vonly;
        CHECK(nvn::translate_maxwell_program(vs_bin, {}, nvn::kBoundTextureHandleBase, &vonly, &error));
        CHECK(!vonly.vertex_wgsl.empty() && vonly.fragment_wgsl.empty() && vonly.fragment_cbufs.empty());
        backend.create_program(144, vonly);
        CHECK_EQ(backend.stats().invalid_programs, 1u);
        nvn::CmdSetDepthStencil write_less = less;
        write_less.depth_write = true;
        for (const bool depth_only : {false, true}) {
            nvn::CommandList vl;
            reset_state(vl, depth_only ? std::vector<nvn::TextureId>{} : std::vector<nvn::TextureId>{151}, 154);
            vl.commands.push_back(nvn::CmdClearDepthStencil{1.0f, true, 0, 0});
            vl.commands.push_back(write_less);
            vl.commands.push_back(nvn::CmdBindProgram{143});
            vl.commands.push_back(pos);
            vl.commands.push_back(nvn::CmdBindVertexBuffer{0, halves_va, sizeof(halves)});
            vl.commands.push_back(nvn::CmdDraw{nvn::Topology::Triangles, 0, 6, 0, 1});
            if (depth_only) {
                vl.commands.push_back(nvn::CmdSetRenderTargets{{151}, 154});
                vl.commands.push_back(nvn::CmdClearColor{0, {0, 0, 0, 1}, nvn::kClearColorMaskRGBA});
            }
            vl.commands.push_back(nvn::CmdBindProgram{141});
            vl.commands.push_back(nvn::CmdDraw{nvn::Topology::Triangles, 0, 12, 0, 1});
            backend.execute(vl);
            CHECK(backend.read_texture(151, &px, nullptr, nullptr));
            if (px.size() == 8 * 8 * 4) {
                std::printf("  vertex-only program (%s): left %u, right %u\n",
                            depth_only ? "depth-only pass" : "with color target", px[(4 * 8 + 1) * 4],
                            px[(4 * 8 + 6) * 4]);
                CHECK(texel_is(px, 8, 1, 4, 0, 0, 0, 255));
                CHECK(texel_is(px, 8, 6, 4, 255, 255, 255, 255));
            }
        }

        // NVN depth mode NEAR_IS_MINUS_W (CommandList::depth_minus_w): clip z
        // in [-w, w] as in OpenGL. A quad at z = -0.5 lies in front (window
        // depth 0.25 < 0.3); without the mapping WebGPU clips it (z < 0).
        const char* kMinusWVs = R"(
override lgp_depth_minus_w: bool = false;
@vertex fn main(@location(0) a0: vec4<f32>) -> @builtin(position) vec4<f32> {
    var p = a0;
    if (lgp_depth_minus_w) {
        p.z = (p.z + p.w) * 0.5;
    }
    return p;
}
)";
        backend.create_program(145, {kMinusWVs, kWhiteFs, {}, {}});
        float near_quad[6][4];
        for (int v = 0; v < 6; ++v) {
            std::memcpy(near_quad[v], halves[v], sizeof(near_quad[v]));
            near_quad[v][2] = -0.5f;
        }
        const uint64_t near_va = memory.put(0x34A00, near_quad, sizeof(near_quad));
        for (const bool minus_w : {true, false}) {
            nvn::CommandList ml;
            ml.depth_minus_w = minus_w;
            reset_state(ml, {151}, 154);
            ml.commands.push_back(nvn::CmdClearColor{0, {0, 0, 0, 1}, nvn::kClearColorMaskRGBA});
            ml.commands.push_back(nvn::CmdClearDepthStencil{0.3f, true, 0, 0});
            ml.commands.push_back(less);
            ml.commands.push_back(nvn::CmdBindProgram{145});
            ml.commands.push_back(pos);
            ml.commands.push_back(nvn::CmdBindVertexBuffer{0, near_va, sizeof(near_quad)});
            ml.commands.push_back(nvn::CmdDraw{nvn::Topology::Triangles, 0, 6, 0, 1});
            backend.execute(ml);
            CHECK(backend.read_texture(151, &px, nullptr, nullptr));
            if (px.size() == 8 * 8 * 4) {
                const int v = minus_w ? 255 : 0;
                CHECK(texel_is(px, 8, 1, 4, v, v, v, 255));
                CHECK(texel_is(px, 8, 6, 4, 0, 0, 0, 255));  // right half not drawn
            }
        }

        // Depth/stencil clears with LGP_WEBGPU_FOLD_CLEARS. Folded, the clear
        // becomes the attachment's load op; a stand-in attachment (depth
        // texture of another size) cannot take it and the game's texture
        // gets the clear as its own pass.
        const bool fold = pass_switch("LGP_WEBGPU_FOLD_CLEARS");
        auto reasons_since = [&](const WebGpuBackend::Stats& b, const char* why) {
            const WebGpuBackend::Stats a = backend.stats();
            const auto x = a.pass_reasons.find(why), y = b.pass_reasons.find(why);
            return (x != a.pass_reasons.end() ? x->second : 0) - (y != b.pass_reasons.end() ? y->second : 0);
        };
        // Stencil 5 cleared; EQUAL against reference 5 draws the left half,
        // against 4 not the right half.
        {
            nvn::TextureDesc sd = td;
            sd.format = uint32_t(nvn::Format::Depth24Stencil8);
            backend.create_texture(158, sd);
            nvn::CommandList sl;
            reset_state(sl, {151}, 158);
            sl.commands.push_back(nvn::CmdClearDepthStencil{1.0f, true, 5, 0xFF});
            sl.commands.push_back(nvn::CmdSetStencilValues{nvn::CmdSetStencilValues::ValueMask, 3, 0xFF});
            sl.commands.push_back(nvn::CmdSetStencilValues{nvn::CmdSetStencilValues::Reference, 3, 5});
            nvn::CmdSetDepthStencil eq;
            eq.stencil_test = true;
            eq.front.func = eq.back.func = nvn::CompareFunc::Equal;
            sl.commands.push_back(eq);
            sl.commands.push_back(nvn::CmdBindProgram{141});
            sl.commands.push_back(pos);
            sl.commands.push_back(nvn::CmdBindVertexBuffer{0, halves_va, sizeof(halves)});
            sl.commands.push_back(nvn::CmdDraw{nvn::Topology::Triangles, 0, 6, 0, 1});
            sl.commands.push_back(nvn::CmdSetStencilValues{nvn::CmdSetStencilValues::Reference, 3, 4});
            sl.commands.push_back(nvn::CmdDraw{nvn::Topology::Triangles, 6, 6, 0, 1});
            const WebGpuBackend::Stats s0 = backend.stats();
            backend.execute(sl);
            CHECK_EQ(reasons_since(s0, "folded: depth/stencil clear (no own pass)"), fold ? 1u : 0u);
            CHECK_EQ(reasons_since(s0, "own pass: depth/stencil clear"), fold ? 0u : 1u);
            CHECK_EQ(reasons_since(s0, "store: depth attachment, not used again in the list"), 1u);
            CHECK(backend.read_texture(151, &px, nullptr, nullptr));
            if (px.size() == 8 * 8 * 4) {
                CHECK(texel_is(px, 8, 1, 4, 255, 255, 255, 255));
                CHECK(texel_is(px, 8, 6, 4, 0, 0, 0, 255));
            }
            backend.destroy_texture(158);
        }
        // Depth stores (pass diagnosis) and a depth-only clear of a
        // Depth24Stencil8 texture: stencil 5 cleared, the left half drawn
        // with EQUAL 5; depth cleared alone (folded: stencil load op Load),
        // the right half drawn with EQUAL 5, so the stencil survived; then
        // depth and stencil cleared whole and drawn once more. The first
        // depth store is read by the depth-only clear, the second is
        // overwritten whole (discard candidate).
        {
            nvn::TextureDesc sd = td;
            sd.format = uint32_t(nvn::Format::Depth24Stencil8);
            backend.create_texture(162, sd);
            nvn::CommandList sl;
            reset_state(sl, {151}, 162);
            sl.commands.push_back(nvn::CmdClearColor{0, {0.0f, 0.0f, 0.0f, 1.0f}, nvn::kClearColorMaskRGBA});
            sl.commands.push_back(nvn::CmdClearDepthStencil{1.0f, true, 5, 0xFF});
            sl.commands.push_back(nvn::CmdSetStencilValues{nvn::CmdSetStencilValues::ValueMask, 3, 0xFF});
            sl.commands.push_back(nvn::CmdSetStencilValues{nvn::CmdSetStencilValues::Reference, 3, 5});
            nvn::CmdSetDepthStencil eq;
            eq.stencil_test = true;
            eq.front.func = eq.back.func = nvn::CompareFunc::Equal;
            sl.commands.push_back(eq);
            sl.commands.push_back(nvn::CmdBindProgram{141});
            sl.commands.push_back(pos);
            sl.commands.push_back(nvn::CmdBindVertexBuffer{0, halves_va, sizeof(halves)});
            sl.commands.push_back(nvn::CmdDraw{nvn::Topology::Triangles, 0, 6, 0, 1});
            sl.commands.push_back(nvn::CmdClearDepthStencil{0.5f, true, 0, 0});
            sl.commands.push_back(nvn::CmdDraw{nvn::Topology::Triangles, 6, 6, 0, 1});
            sl.commands.push_back(nvn::CmdClearDepthStencil{1.0f, true, 0, 0xFF});
            sl.commands.push_back(nvn::CmdDraw{nvn::Topology::Triangles, 0, 6, 0, 1});
            const WebGpuBackend::Stats s0 = backend.stats();
            backend.execute(sl);
            CHECK_EQ(reasons_since(s0, "folded: depth/stencil clear (no own pass)"), fold ? 3u : 0u);
            CHECK_EQ(reasons_since(s0, "own pass: depth/stencil clear"), fold ? 0u : 3u);
            CHECK_EQ(reasons_since(s0, "store: depth attachment, read later"), 1u);
            CHECK_EQ(reasons_since(s0, "store: depth attachment, overwritten later before any read (discard candidate)"), 1u);
            CHECK_EQ(reasons_since(s0, "store: depth attachment, not used again in the list"), 1u);
            CHECK(backend.read_texture(151, &px, nullptr, nullptr));
            if (px.size() == 8 * 8 * 4) {
                CHECK(texel_is(px, 8, 1, 4, 255, 255, 255, 255));
                CHECK(texel_is(px, 8, 6, 4, 255, 255, 255, 255));  // stencil 5 kept by the depth-only clear
            }
            backend.destroy_texture(162);
        }
        // 16x16 depth texture: 0 cleared with a 16x16 colour target, then 1
        // with the 8x8 colour target 151 (stand-in attachment) and a draw.
        // A draw with LESS at 0.5 against the 16x16 colour target passes
        // only if the second clear reached the game's texture.
        {
            nvn::TextureDesc bd = td;
            bd.width = bd.height = 16;
            backend.create_texture(161, bd);
            bd.format = uint32_t(nvn::Format::Depth32F);
            backend.create_texture(159, bd);
            const nvn::CmdSetScissors big{0, {{0, 0, 16, 16}}}, small{0, {{0, 0, 8, 8}}};
            nvn::CommandList zl;
            reset_state(zl, {161}, 159);
            zl.commands.push_back(big);
            zl.commands.push_back(nvn::CmdClearDepthStencil{0.0f, true, 0, 0});
            backend.execute(zl);
            nvn::CommandList ol;
            reset_state(ol, {151}, 159);
            ol.commands.push_back(big);
            ol.commands.push_back(nvn::CmdClearDepthStencil{1.0f, true, 0, 0});
            ol.commands.push_back(small);
            ol.commands.push_back(less);
            ol.commands.push_back(nvn::CmdBindProgram{141});
            ol.commands.push_back(pos);
            ol.commands.push_back(nvn::CmdBindVertexBuffer{0, halves_va, sizeof(halves)});
            ol.commands.push_back(nvn::CmdDraw{nvn::Topology::Triangles, 0, 6, 0, 1});
            const WebGpuBackend::Stats s0 = backend.stats();
            backend.execute(ol);
            CHECK_EQ(reasons_since(s0, "own pass: depth/stencil clear"), 1u);
            CHECK_EQ(reasons_since(s0, "folded: depth/stencil clear (no own pass)"), 0u);
            // The stand-in, not the game's texture, is the stored attachment.
            CHECK_EQ(reasons_since(s0, "store: depth attachment, not used again in the list"), 0u);
            nvn::CommandList vl;
            reset_state(vl, {161}, 159);
            vl.commands.push_back(less);
            vl.commands.push_back(nvn::CmdBindProgram{141});
            vl.commands.push_back(pos);
            vl.commands.push_back(nvn::CmdBindVertexBuffer{0, halves_va, sizeof(halves)});
            vl.commands.push_back(nvn::CmdDraw{nvn::Topology::Triangles, 0, 6, 0, 1});
            backend.execute(vl);
            CHECK(backend.read_texture(161, &px, nullptr, nullptr));
            if (px.size() == 16 * 16 * 4) {
                CHECK(texel_is(px, 16, 1, 4, 255, 255, 255, 255));
                CHECK(texel_is(px, 16, 6, 4, 0, 0, 0, 255));
            }
            backend.destroy_texture(159);
            backend.destroy_texture(161);
        }
    }

    // Vertex buffer slots: WebGPU counts every slot up to the last against
    // maxVertexBuffers (8 guaranteed), NVN has 16 streams. A layout with 10
    // stream states of which only stream 9 is read, plus an input no
    // attribute feeds (zero buffer), must still draw: 2 WebGPU buffers.
    {
        const char* kTwoInputVs = R"(
struct VOut { @builtin(position) pos: vec4<f32>, @location(0) color: vec4<f32>, }
@vertex fn main(@location(0) a0: vec4<f32>, @location(1) a1: vec4<f32>) -> VOut {
    return VOut(a0, a1 + vec4<f32>(0.0, 1.0, 0.0, 1.0));
}
)";
        backend.create_program(145, {kTwoInputVs, kColorFs, {}, {}});
        CHECK_EQ(backend.stats().invalid_programs, 1u);
        const float full[6][4] = {{-1, -1, 0.5f, 1}, {1, -1, 0.5f, 1}, {-1, 1, 0.5f, 1},
                                  {-1, 1, 0.5f, 1},  {1, -1, 0.5f, 1}, {1, 1, 0.5f, 1}};
        const uint64_t full_va = memory.put(0x36000, full, sizeof(full));
        nvn::CmdSetVertexLayout wide;
        wide.attribs = {{0, nvn::VertexFormat::Float32x4, 9, 0}};
        wide.strides.assign(10, 16);
        nvn::CommandList wl;
        reset_state(wl, {150});
        wl.commands.push_back(nvn::CmdBindProgram{145});
        wl.commands.push_back(wide);
        for (uint32_t stream = 0; stream < 10; ++stream)  // like the game: every stream bound
            wl.commands.push_back(nvn::CmdBindVertexBuffer{stream, full_va, sizeof(full)});
        wl.commands.push_back(nvn::CmdDraw{nvn::Topology::Triangles, 0, 6, 0, 1});
        const auto before = backend.stats();
        backend.execute(wl);
        CHECK_EQ(backend.stats().invalid_pipelines, before.invalid_pipelines);
        CHECK_EQ(backend.stats().draws - before.draws, 1u);
        CHECK(backend.read_texture(150, &px, nullptr, nullptr));
        if (px.size() == 8 * 8 * 4)
            CHECK(texel_is(px, 8, 4, 4, 0, 255, 0, 255));
    }

    // Sampler LOD bias through the shader (wgsl::Options::sampler_lod_bias):
    // a full-target quad with uv 0..1 samples an 8x8 texture at LOD 0; the
    // bias picks level 1 or 2 (levels red, green, blue, white).
    {
        const std::vector<uint8_t> tex_bin = read_hex(dir + "/fs_textured.hex");
        lgp::gpu::shader::Sph tex_sph;
        CHECK(lgp::gpu::shader::parse_sph(tex_bin, tex_sph, error));
        wgsl::Options bvs_opt = vs_opt, bfs_opt = fs_opt;
        bvs_opt.vertex_output_interp = wgsl::fragment_input_interpolation(tex_sph);
        bvs_opt.texture_swizzle = bfs_opt.texture_swizzle = true;
        bvs_opt.sampler_lod_bias = bfs_opt.sampler_lod_bias = true;
        wgsl::Result bvs, bfs;
        CHECK(translate(vs_bin, bvs_opt, &bvs));
        CHECK(translate(tex_bin, bfs_opt, &bfs));
        CHECK(bfs.code.find("textureSampleBias(") != std::string::npos);
        CHECK(bfs.code.find("fn tex_lod_bias") != std::string::npos);
        nvn::ProgramDesc bp{bvs.code, bfs.code, bvs.bindings.cbuf_banks, bfs.bindings.cbuf_banks, {}, {0}};
        bp.texture_swizzle = true;
        backend.create_program(142, bp);
        CHECK_EQ(backend.stats().invalid_programs, 1u);

        nvn::TextureDesc md = td;
        md.levels = 4;
        backend.create_texture(155, md);
        const uint8_t level_colors[4][4] = {{255, 0, 0, 255}, {0, 255, 0, 255}, {0, 0, 255, 255}, {255, 255, 255, 255}};
        for (uint32_t level = 0; level < 4; ++level) {
            const uint32_t size = 8u >> level;
            std::vector<uint8_t> texels;
            for (uint32_t i = 0; i < size * size; ++i) texels.insert(texels.end(), level_colors[level], level_colors[level] + 4);
            backend.write_texture(155, level, nvn::Rect{0, 0, int32_t(size), int32_t(size)}, texels.data(), size * 4);
        }
        std::vector<uint32_t> ident(16, 0);
        for (int i = 0; i < 16; i += 5) ident[i] = f2u(1.0f);
        const uint64_t ident_va = memory.put(0x34A00, ident.data(), ident.size() * 4);
        std::vector<float> uvquad;
        const float corners[6][2] = {{-1, -1}, {1, -1}, {-1, 1}, {-1, 1}, {1, -1}, {1, 1}};
        for (const auto& c : corners)
            uvquad.insert(uvquad.end(), {c[0], c[1], 0.0f, 1.0f, 1, 1, 1, 1, (c[0] + 1) / 2, (c[1] + 1) / 2});
        const uint64_t uv_va = memory.put(0x34C00, uvquad.data(), uvquad.size() * 4);
        nvn::CmdSetVertexLayout uv_layout;
        uv_layout.attribs = {{0, nvn::VertexFormat::Float32x4, 0, 0},
                             {1, nvn::VertexFormat::Float32x4, 0, 16},
                             {2, nvn::VertexFormat::Float32x2, 0, 32}};
        uv_layout.strides = {40};
        const float biases[4] = {0.0f, 1.0f, 2.0f, -1.0f};
        const int want[4] = {0, 1, 2, 0};  // level; a negative bias stays at level 0
        for (int i = 0; i < 4; ++i) {
            const nvn::TextureId target = nvn::TextureId(150 + i);
            nvn::CommandList ll;
            reset_state(ll, {target});
            ll.commands.push_back(nvn::CmdBindProgram{142});
            ll.commands.push_back(uv_layout);
            ll.commands.push_back(nvn::CmdBindVertexBuffer{0, uv_va, uvquad.size() * 4});
            ll.commands.push_back(
                nvn::CmdBindUniformBuffer{nvn::ShaderStage::Vertex, nvn::kUniformBufferBankBase, ident_va, 64});
            nvn::CmdBindTexture bind;
            bind.stage = nvn::ShaderStage::Fragment;
            bind.slot = 0;
            bind.texture = 155;
            bind.sampler.min = bind.sampler.mag = bind.sampler.mip = nvn::Filter::Nearest;
            bind.sampler.mipmapped = true;
            bind.sampler.lod_bias = biases[i];
            ll.commands.push_back(bind);
            ll.commands.push_back(nvn::CmdDraw{nvn::Topology::Triangles, 0, 6, 0, 1});
            backend.execute(ll);
            CHECK(backend.read_texture(target, &px, nullptr, nullptr));
            if (px.size() == 8 * 8 * 4) {
                const uint8_t* c = level_colors[want[i]];
                CHECK(texel_is(px, 8, 3, 3, c[0], c[1], c[2], 255));
            }
        }
    }

    // CopyBufferToTexture into slices 1..2 of a 3D texture, image stride
    // larger than the packed image (nvnCommandBufferSetCopyImageStride).
    {
        nvn::TextureDesc vd;
        vd.target = uint32_t(nvn::TextureTarget::Target3D);
        vd.format = uint32_t(nvn::Format::RGBA8);
        vd.width = vd.height = vd.depth = 4;
        backend.create_texture(156, vd);
        std::vector<uint8_t> slices(256 + 64, 0x7F);
        nvn::CmdCopyBufferToTexture cp;
        cp.address = memory.put(0x35000, slices.data(), slices.size());
        cp.texture = 156;
        cp.width = cp.height = 4;
        cp.z = 1;
        cp.depth = 2;
        cp.image_stride = 256;
        nvn::CommandList xl;
        xl.commands.push_back(cp);
        cp.z = 3;  // slices 3..4: outside, skipped
        xl.commands.push_back(cp);
        const auto before = backend.stats();
        backend.execute(xl);
        CHECK_EQ(backend.stats().copies - before.copies, 1u);
        CHECK_EQ(backend.stats().skipped_commands - before.skipped_commands, 1u);
    }
    // LGP_WEBGPU_DEBUG=gputime (nvn.webgpu_draw_gputime): every pass above
    // timed, compute per program; LGP_TEST_NO_TIMESTAMPS when the adapter
    // has no timestamp-query.
    if (const char* debug = std::getenv("LGP_WEBGPU_DEBUG"); debug != nullptr && std::strstr(debug, "gputime")) {
        backend.finish();
        const WebGpuBackend::Stats st = backend.stats();
        std::printf("gputime: %llu passes, compute %llu (%llu ns, %zu programs), render %llu (%llu ns)\n",
                    (unsigned long long)st.gpu_time_passes, (unsigned long long)st.gpu_time_compute.passes,
                    (unsigned long long)st.gpu_time_compute.ns, st.gpu_time_programs.size(),
                    (unsigned long long)st.gpu_time_render.passes, (unsigned long long)st.gpu_time_render.ns);
        if (std::getenv("LGP_TEST_NO_TIMESTAMPS") == nullptr) {
            CHECK(st.gpu_time_compute.passes > 0);
            CHECK(st.gpu_time_render.passes > 0);
            CHECK(!st.gpu_time_programs.empty());
            CHECK_EQ(st.gpu_time_passes, st.gpu_time_compute.passes + st.gpu_time_render.passes);
            // Every render pass names its target.
            uint64_t by_target = 0;
            for (const auto& [name, t] : st.gpu_time_targets) {
                CHECK(!name.empty());
                by_target += t.passes;
            }
            CHECK_EQ(by_target, st.gpu_time_render.passes);
            // Draws of the timed draw passes, by program.
            uint64_t program_draws = 0;
            for (const auto& [hash, t] : st.gpu_time_draw_programs) program_draws += t.passes;
            CHECK(program_draws > 0);
            CHECK(program_draws <= st.draws);
        }
    }
    // LGP_WEBGPU_DEBUG=pipelinevariants (nvn.webgpu_draw_pipeline_variants):
    // every draw pipeline created above counted once, by what sets it apart
    // from its program's earlier ones.
    if (const char* debug = std::getenv("LGP_WEBGPU_DEBUG"); debug != nullptr && std::strstr(debug, "pipelinevariants")) {
        const WebGpuBackend::Stats st = backend.stats();
        uint64_t variants = 0, firsts = 0, others = 0;
        for (const auto& [name, n] : st.diagnostics)
            if (name.rfind("pipeline variant: ", 0) == 0) {
                std::printf("  %s: %llu\n", name.c_str(), (unsigned long long)n);
                variants += n;
                (name == "pipeline variant: first of its program" ? firsts : others) += n;
            }
        CHECK_EQ(variants, st.draw_pipelines_created);
        CHECK(firsts > 0);
        CHECK(others > 0);  // several cases draw one program with other state
    }
    // LGP_WEBGPU_DEBUG=halffill (nvn.webgpu_draw_halffill): a draw into a
    // 1280x720 target lands in the top-left quarter, the clear stays full;
    // the 8x8 targets above are too small to be touched.
    if (const char* debug = std::getenv("LGP_WEBGPU_DEBUG"); debug != nullptr && std::strstr(debug, "halffill")) {
        nvn::TextureDesc hd = td;
        hd.width = 1280;
        hd.height = 720;
        backend.create_texture(160, hd);
        backend.create_program(161, program);  // program 1 is gone by now
        nvn::CommandList hl;
        hl.commands.push_back(nvn::CmdBindProgram{161});
        hl.commands.push_back(layout);
        hl.commands.push_back(nvn::CmdBindVertexBuffer{0, vb_va, verts.size() * 4});
        hl.commands.push_back(nvn::CmdBindUniformBuffer{nvn::ShaderStage::Vertex, nvn::kUniformBufferBankBase, cb_va, 64});
        hl.commands.push_back(nvn::CmdBindUniformBuffer{nvn::ShaderStage::Fragment, 0, cb_va, 64});
        hl.commands.push_back(nvn::CmdSetRenderTargets{{160}, 0});
        hl.commands.push_back(nvn::CmdSetViewports{0, {0, 0, 1280, 720}});
        hl.commands.push_back(nvn::CmdSetScissors{0, {{0, 0, 1280, 720}}});
        hl.commands.push_back(nvn::CmdClearColor{0, {0.2f, 0.2f, 0.2f, 1.0f}, nvn::kClearColorMaskRGBA});
        hl.commands.push_back(nvn::CmdDraw{nvn::Topology::Triangles, 0, 12, 0, 1});
        const auto before = backend.stats();
        backend.execute(hl);
        CHECK(backend.read_texture(160, &px, nullptr, nullptr));
        if (px.size() == size_t(1280) * 720 * 4) {
            CHECK(texel_is(px, 1280, 480, 180, 191, 96, 48, 255));  // kept quad, x 320..639 of the quarter
            CHECK(texel_is(px, 1280, 960, 180, 51, 51, 51, 255));   // drawn without halffill
            CHECK(texel_is(px, 1280, 480, 540, 51, 51, 51, 255));   // rows 360.. stay cleared
        }
        const auto after = backend.stats();
        const std::string what = "halffill: draws into the top-left quarter";
        const uint64_t n0 = before.diagnostics.count(what) ? before.diagnostics.at(what) : 0;
        const uint64_t n1 = after.diagnostics.count(what) ? after.diagnostics.at(what) : 0;
        CHECK_EQ(n1 - n0, 1u);
        CHECK_EQ(n0, 0u);
    }
    // LGP_WEBGPU_FILL_SCALE=0.75 with LGP_WEBGPU_FILL_TARGETS=1280x720
    // (nvn.webgpu_draw_fill_scale): the same draw lands in the top-left
    // 960x540, the clear stays full; the 8x8 targets above are not listed.
    if (const char* scale = std::getenv("LGP_WEBGPU_FILL_SCALE"); scale != nullptr && *scale != '\0') {
        nvn::TextureDesc hd = td;
        hd.width = 1280;
        hd.height = 720;
        backend.create_texture(170, hd);
        backend.create_program(171, program);
        nvn::CommandList hl;
        hl.commands.push_back(nvn::CmdBindProgram{171});
        hl.commands.push_back(layout);
        hl.commands.push_back(nvn::CmdBindVertexBuffer{0, vb_va, verts.size() * 4});
        hl.commands.push_back(nvn::CmdBindUniformBuffer{nvn::ShaderStage::Vertex, nvn::kUniformBufferBankBase, cb_va, 64});
        hl.commands.push_back(nvn::CmdBindUniformBuffer{nvn::ShaderStage::Fragment, 0, cb_va, 64});
        hl.commands.push_back(nvn::CmdSetRenderTargets{{170}, 0});
        hl.commands.push_back(nvn::CmdSetViewports{0, {0, 0, 1280, 720}});
        hl.commands.push_back(nvn::CmdSetScissors{0, {{0, 0, 1280, 720}}});
        hl.commands.push_back(nvn::CmdClearColor{0, {0.2f, 0.2f, 0.2f, 1.0f}, nvn::kClearColorMaskRGBA});
        hl.commands.push_back(nvn::CmdDraw{nvn::Topology::Triangles, 0, 12, 0, 1});
        // nvn.webgpu_draw_fill_scale_bad_targets: a malformed target list
        // switches the scale off instead of widening it to every target.
        const char* off_env = std::getenv("LGP_TEST_FILL_SCALE_OFF");
        const bool off = off_env != nullptr && std::string(off_env) == "1";
        const auto before = backend.stats();
        backend.execute(hl);
        CHECK(backend.read_texture(170, &px, nullptr, nullptr));
        if (px.size() == size_t(1280) * 720 * 4) {
            CHECK(texel_is(px, 1280, 720, 270, 191, 96, 48, 255));   // quad in both cases
            if (off) {
                CHECK(texel_is(px, 1280, 1100, 270, 191, 96, 48, 255));  // full-size quad
            } else {
                CHECK(texel_is(px, 1280, 1100, 270, 51, 51, 51, 255));   // drawn without the scale
                CHECK(texel_is(px, 1280, 720, 600, 51, 51, 51, 255));    // rows 540.. stay cleared
            }
        }
        const auto after = backend.stats();
        const std::string what = "fill scale: draws into a scaled top-left part";
        const uint64_t n0 = before.diagnostics.count(what) ? before.diagnostics.at(what) : 0;
        const uint64_t n1 = after.diagnostics.count(what) ? after.diagnostics.at(what) : 0;
        CHECK_EQ(n1 - n0, off ? 0u : 1u);
        CHECK_EQ(n0, 0u);  // the 8x8 targets are not listed
    }
    // watch_programs: a draw of a watched program is reported once; without
    // a list nothing is.
    {
        backend.create_texture(190, td);
        backend.create_program(191, program);
        nvn::CommandList wl;
        wl.commands.push_back(nvn::CmdBindProgram{191});
        wl.commands.push_back(layout);
        wl.commands.push_back(nvn::CmdBindVertexBuffer{0, vb_va, verts.size() * 4});
        wl.commands.push_back(nvn::CmdBindUniformBuffer{nvn::ShaderStage::Vertex, nvn::kUniformBufferBankBase, cb_va, 64});
        wl.commands.push_back(nvn::CmdBindUniformBuffer{nvn::ShaderStage::Fragment, 0, cb_va, 64});
        wl.commands.push_back(nvn::CmdSetRenderTargets{{190}, 0});
        wl.commands.push_back(nvn::CmdDraw{nvn::Topology::Triangles, 0, 3, 0, 1});
        backend.execute(wl);
        CHECK(!backend.take_watched_drawn());
        backend.watch_programs("0,1,2,3,4,5,6,7,8,9,a,b,c,d,e,f");
        backend.execute(wl);
        CHECK(backend.take_watched_drawn());
        CHECK(!backend.take_watched_drawn());
        backend.watch_programs("");
        backend.execute(wl);
        CHECK(!backend.take_watched_drawn());
        // By the guest code hash (ProgramDesc::guest_hash), which a
        // translator change leaves alone; another guest hash does not match.
        nvn::ProgramDesc guest = program;
        guest.guest_hash = 0x5eedc0de12345678ull;
        backend.create_program(191, guest);
        backend.watch_programs("5eedc0de");
        backend.execute(wl);
        CHECK(backend.take_watched_drawn());
        backend.watch_programs("5eedc0df");
        backend.execute(wl);
        CHECK(!backend.take_watched_drawn());
        backend.watch_programs("");
    }
    // LGP_WEBGPU_HALFRES_PROGRAMS listing every program
    // (nvn.webgpu_draw_halfres_programs): an additive draw into a 1280x720
    // target renders at half resolution and is added in its place (inside
    // the quad the same colour, the edges blur); the same draw without
    // blending stays at full resolution.
    if (const char* half = std::getenv("LGP_WEBGPU_HALFRES_PROGRAMS"); half != nullptr && *half != '\0') {
        nvn::TextureDesc hd = td;
        hd.width = 1280;
        hd.height = 720;
        backend.create_texture(180, hd);
        backend.create_texture(182, hd);
        backend.create_program(181, program);
        auto add_draw = [&](nvn::CommandList& hl, nvn::TextureId target, bool additive, bool clear = true,
                            int32_t w = 1280, int32_t h = 720) {
            hl.commands.push_back(nvn::CmdBindProgram{181});
            hl.commands.push_back(layout);
            hl.commands.push_back(nvn::CmdBindVertexBuffer{0, vb_va, verts.size() * 4});
            hl.commands.push_back(
                nvn::CmdBindUniformBuffer{nvn::ShaderStage::Vertex, nvn::kUniformBufferBankBase, cb_va, 64});
            hl.commands.push_back(nvn::CmdBindUniformBuffer{nvn::ShaderStage::Fragment, 0, cb_va, 64});
            hl.commands.push_back(nvn::CmdSetRenderTargets{{target}, 0});
            hl.commands.push_back(nvn::CmdSetViewports{0, {0, 0, float(w), float(h)}});
            hl.commands.push_back(nvn::CmdSetScissors{0, {{0, 0, w, h}}});
            if (clear)
                hl.commands.push_back(nvn::CmdClearColor{0, {0.2f, 0.2f, 0.2f, 1.0f}, nvn::kClearColorMaskRGBA});
            nvn::BlendEquation add;
            add.src_color = add.dst_color = add.src_alpha = add.dst_alpha = nvn::BlendFactor::One;
            hl.commands.push_back(nvn::CmdSetBlendEquation{0, add});
            hl.commands.push_back(nvn::CmdSetBlendEnables{uint8_t(additive ? 1 : 0)});
            hl.commands.push_back(nvn::CmdDraw{nvn::Topology::Triangles, 0, 12, 0, 1});
        };
        nvn::CommandList hl;
        add_draw(hl, 180, true);
        add_draw(hl, 182, false);
        const auto before = backend.stats();
        backend.execute(hl);
        const auto after = backend.stats();
        // With LGP_WEBGPU_RENDER_SCALE as well (nvn.webgpu_draw_halfres_render_scale)
        // the targets live at s times their size and the stand-in at half of that.
        const char* scale_env = std::getenv("LGP_WEBGPU_RENDER_SCALE");
        const float hs = scale_env != nullptr && *scale_env != '\0' ? std::strtof(scale_env, nullptr) : 1.0f;
        const uint32_t hw = uint32_t(std::lround(1280 * hs)), hh = uint32_t(std::lround(720 * hs));
        auto at = [&](int v) { return int(std::lround(v * hs)); };
        uint32_t rw = 0, rh = 0;
        CHECK(backend.read_texture(180, &px, &rw, &rh));
        CHECK_EQ(rw, hw);
        CHECK_EQ(rh, hh);
        if (px.size() == size_t(hw) * hh * 4) {
            CHECK(texel_is(px, hw, at(960), at(360), 242, 147, 99, 255));  // quad + clear, in place
            CHECK(texel_is(px, hw, at(1100), at(600), 242, 147, 99, 255));
            CHECK(texel_is(px, hw, at(320), at(360), 51, 51, 51, 255));    // beside the quad: the clear
        }
        CHECK(backend.read_texture(182, &px, nullptr, nullptr));
        if (px.size() == size_t(hw) * hh * 4)
            CHECK(texel_is(px, hw, at(960), at(360), 191, 96, 48, 255));  // not blended: the quad's own colour
        auto count = [](const auto& st, const std::string& what) -> uint64_t {
            return st.diagnostics.count(what) ? st.diagnostics.at(what) : 0;
        };
        CHECK_EQ(count(after, "half-res: draws at half resolution") - count(before, "half-res: draws at half resolution"),
                 1u);
        CHECK_EQ(count(after, "half-res: blending not additive") - count(before, "half-res: blending not additive"), 1u);
        // Switched off at run time (the app's settings), then on with a list again.
        backend.set_halfres_effects(false);
        nvn::CommandList off;
        add_draw(off, 180, true);
        backend.execute(off);
        const auto after_off = backend.stats();
        CHECK_EQ(count(after_off, "half-res: draws at half resolution"), count(after, "half-res: draws at half resolution"));
        backend.set_halfres_effects(true, "0x0,1,2,3,4,5,6,7,8,9,A,b,c,d,e,f");
        backend.execute(off);
        CHECK_EQ(count(backend.stats(), "half-res: draws at half resolution"),
                 count(after_off, "half-res: draws at half resolution") + 1);
        // Without a clear before it, the add pass is the target's first GPU
        // write and marks it as such (rendered_textures, no re-upload).
        backend.create_texture(183, hd);
        nvn::CommandList first_write;
        add_draw(first_write, 183, true, /*clear=*/false);
        const auto before_first = backend.stats();
        backend.execute(first_write);
        // Pass diagnosis sees the add pass read the stand-in it stored.
        auto reason = [](const auto& st, const std::string& what) -> uint64_t {
            return st.pass_reasons.count(what) ? st.pass_reasons.at(what) : 0;
        };
        const std::string read_later = "store: colour attachment, read later";
        CHECK_EQ(reason(backend.stats(), read_later) - reason(before_first, read_later), 1u);
        bool rendered = false;
        for (const auto& t : backend.rendered_textures()) rendered = rendered || t.id == 183;
        CHECK(rendered);
        // An odd size: the stand-in rounds up (641x361) and its last texels
        // are drawn too, so the real target's right and bottom edge keep the
        // full colour instead of fading into the stand-in's clear.
        nvn::TextureDesc od = hd;
        od.width = 1281;
        od.height = 721;
        backend.create_texture(184, od);
        nvn::CommandList odd;
        add_draw(odd, 184, true, true, 1281, 721);
        backend.execute(odd);
        CHECK(backend.read_texture(184, &px, &rw, &rh));
        if (rw > 0 && rh > 0 && px.size() == size_t(rw) * rh * 4) {
            CHECK(texel_is(px, rw, int(rw) - 1, int(rh) / 2, 242, 147, 99, 255));
            CHECK(texel_is(px, rw, int(rw) - 1, int(rh) - 1, 242, 147, 99, 255));
        }
    }
    // LGP_WEBGPU_RENDER_SCALE=0.5 with LGP_WEBGPU_RENDER_SCALE_MIN=700
    // (nvn.webgpu_draw_render_scale): a 1280x720 render target lives at
    // 640x360, its clears land in host texels, and a shader's texel fetch
    // (TLDS) at guest coordinates reads the right texel; the 8x8 targets
    // above stay as they are.
    if (const char* scale = std::getenv("LGP_WEBGPU_RENDER_SCALE"); scale != nullptr && *scale != '\0') {
        const float rs = std::strtof(scale, nullptr);
        nvn::TextureDesc hd = td;
        hd.width = 1280;
        hd.height = 720;
        backend.create_texture(180, hd);
        nvn::CommandList cl;
        cl.commands.push_back(nvn::CmdSetRenderTargets{{180}, 0});
        cl.commands.push_back(nvn::CmdSetScissors{0, {{0, 0, 1280, 720}}});
        cl.commands.push_back(nvn::CmdClearColor{0, {0, 0, 0, 1}, nvn::kClearColorMaskRGBA});
        cl.commands.push_back(nvn::CmdSetScissors{0, {{640, 0, 640, 720}}});  // right half red
        cl.commands.push_back(nvn::CmdClearColor{0, {1, 0, 0, 1}, nvn::kClearColorMaskRGBA});
        backend.execute(cl);
        uint32_t rw = 0, rh = 0;
        CHECK(backend.read_texture(180, &px, &rw, &rh));
        CHECK_EQ(rw, 640u);
        CHECK_EQ(rh, 360u);
        if (px.size() == size_t(640) * 360 * 4) {
            CHECK(texel_is(px, 640, 319, 100, 0, 0, 0, 255));
            CHECK(texel_is(px, 640, 320, 100, 255, 0, 0, 255));
        }
        // A target that already holds something when it first becomes one
        // (here a texture clear) is scaled with its contents.
        {
            backend.create_texture(181, hd);
            nvn::CommandList wl;
            wl.commands.push_back(nvn::CmdClearTexture{181, {0, 0, 1280, 720}, {0, 1, 0, 1}, 0xF});
            backend.execute(wl);
            const auto before = backend.stats();
            nvn::CommandList sl;
            sl.commands.push_back(nvn::CmdSetRenderTargets{{181}, 0});
            sl.commands.push_back(nvn::CmdSetScissors{0, {{640, 0, 640, 720}}});  // right half blue
            sl.commands.push_back(nvn::CmdClearColor{0, {0, 0, 1, 1}, nvn::kClearColorMaskRGBA});
            backend.execute(sl);
            const std::string what = "render scale: targets scaled with their contents";
            const uint64_t n0 = before.diagnostics.count(what) ? before.diagnostics.at(what) : 0;
            const auto after = backend.stats();
            const uint64_t n1 = after.diagnostics.count(what) ? after.diagnostics.at(what) : 0;
            CHECK_EQ(n1 - n0, 1u);
            CHECK(backend.read_texture(181, &px, &rw, &rh));
            CHECK_EQ(rw, 640u);
            CHECK_EQ(rh, 360u);
            if (px.size() == size_t(640) * 360 * 4) {
                CHECK(texel_is(px, 640, 100, 100, 0, 255, 0, 255));
                CHECK(texel_is(px, 640, 500, 100, 0, 0, 255, 255));
            }
            // The present crop is in guest texels: the green left half,
            // read back at host size.
            CHECK(backend.present({181, {0, 0, 640, 720}}));
            CHECK(backend.read_presented(&px, &rw, &rh));
            CHECK_EQ(rw, 320u);
            CHECK_EQ(rh, 360u);
            if (px.size() == size_t(320) * 360 * 4)
                CHECK(texel_is(px, 320, 319, 200, 0, 255, 0, 255));
            // Guest texels uploaded into the scaled target land in its host
            // rectangle (64x64 yellow at guest 1000,500).
            std::vector<uint8_t> yellow(64 * 64 * 4);
            for (size_t i = 0; i < yellow.size(); i += 4) {
                yellow[i] = yellow[i + 1] = yellow[i + 3] = 255;
                yellow[i + 2] = 0;
            }
            const uint64_t up_va = memory.put(0x50000, yellow.data(), yellow.size());
            nvn::CommandList ul;
            nvn::CmdCopyBufferToTexture up;
            up.address = up_va;
            up.texture = 181;
            up.x = 1000;
            up.y = 500;
            up.width = up.height = 64;
            up.row_stride = 64 * 4;
            ul.commands.push_back(up);
            const auto before_up = backend.stats();
            backend.execute(ul);
            CHECK_EQ(backend.stats().skipped_commands, before_up.skipped_commands);
            CHECK(backend.read_texture(181, &px, &rw, &rh));
            if (px.size() == size_t(640) * 360 * 4) {
                CHECK(texel_is(px, 640, 515, 265, 255, 255, 0, 255));  // inside (guest 1030,530)
                CHECK(texel_is(px, 640, 540, 265, 0, 0, 255, 255));    // beside it: still blue
            }
        }
        // TLDS at guest (x, 100) into an unscaled 8x8 target: red = the fetched texel's red.
        const std::vector<uint8_t> fetch_bin = read_hex(dir + "/fs_tex_fetch.hex");
        lgp::gpu::shader::Sph fetch_sph;
        CHECK(lgp::gpu::shader::parse_sph(fetch_bin, fetch_sph, error));
        wgsl::Options rvs_opt = vs_opt, rfs_opt = fs_opt;
        rvs_opt.vertex_output_interp = wgsl::fragment_input_interpolation(fetch_sph);
        rvs_opt.texture_swizzle = rfs_opt.texture_swizzle = true;
        rvs_opt.render_scale = rfs_opt.render_scale = rs;
        wgsl::Result rvs, rfs;
        CHECK(translate(vs_bin, rvs_opt, &rvs));
        CHECK(translate(fetch_bin, rfs_opt, &rfs));
        CHECK(rfs.code.find("tex_host_xy(0u, ") != std::string::npos);
        CHECK(rfs.code.find("tex_guest_dim(1u, ") != std::string::npos);  // TXQ reads handle 1
        CHECK(rfs.code.find("const lgp_render_scale: f32 = 0.5;") != std::string::npos);
        CHECK(rvs.code.find("lgp_render_scale") == std::string::npos);  // no fetches: unchanged
        nvn::ProgramDesc rp{rvs.code, rfs.code, rvs.bindings.cbuf_banks, rfs.bindings.cbuf_banks, {}, {0, 1}};
        rp.texture_swizzle = true;
        const uint64_t invalid_before = backend.stats().invalid_programs;
        backend.create_program(182, rp);
        CHECK_EQ(backend.stats().invalid_programs, invalid_before);
        std::vector<uint32_t> ident(16, 0);
        for (int i = 0; i < 16; i += 5) ident[i] = f2u(1.0f);
        const uint64_t ident_va = memory.put(0x4E000, ident.data(), ident.size() * 4);
        std::vector<float> full;
        const float corners[6][2] = {{-1, -1}, {1, -1}, {-1, 1}, {-1, 1}, {1, -1}, {1, 1}};
        for (const auto& c : corners) full.insert(full.end(), {c[0], c[1], 0.0f, 1.0f, 1, 1, 1, 1, 0, 0});
        const uint64_t full_va = memory.put(0x4E100, full.data(), full.size() * 4);
        nvn::CmdSetVertexLayout full_layout;
        full_layout.attribs = {{0, nvn::VertexFormat::Float32x4, 0, 0},
                               {1, nvn::VertexFormat::Float32x4, 0, 16},
                               {2, nvn::VertexFormat::Float32x2, 0, 32}};
        full_layout.strides = {40};
        const int32_t xs[2] = {1000, 300};  // guest texels: red half, black half
        const int want_red[2] = {255, 0};
        for (int i = 0; i < 2; ++i) {
            const nvn::TextureId target = nvn::TextureId(183 + i);
            backend.create_texture(target, td);
            const int32_t coords[4] = {xs[i], 100, 0, 0};
            const uint64_t coords_va = memory.put(0x4E400 + 0x100 * uint64_t(i), coords, sizeof(coords));
            nvn::CommandList fl;
            reset_state(fl, {target});
            fl.commands.push_back(nvn::CmdBindProgram{182});
            fl.commands.push_back(full_layout);
            fl.commands.push_back(nvn::CmdBindVertexBuffer{0, full_va, full.size() * 4});
            fl.commands.push_back(
                nvn::CmdBindUniformBuffer{nvn::ShaderStage::Vertex, nvn::kUniformBufferBankBase, ident_va, 64});
            fl.commands.push_back(nvn::CmdBindUniformBuffer{nvn::ShaderStage::Fragment, 0, coords_va, 16});
            nvn::CmdBindTexture bind;
            bind.stage = nvn::ShaderStage::Fragment;
            bind.slot = 0;
            bind.texture = 180;
            fl.commands.push_back(bind);
            bind.slot = 1;  // the shader's second fetch and its size query
            fl.commands.push_back(bind);
            fl.commands.push_back(nvn::CmdDraw{nvn::Topology::Triangles, 0, 6, 0, 1});
            const auto before = backend.stats();
            backend.execute(fl);
            CHECK_EQ(backend.stats().draws - before.draws, 1u);
            CHECK(backend.read_texture(target, &px, nullptr, nullptr));
            if (px.size() == 8 * 8 * 4)
                CHECK_EQ(int(px[(3 * 8 + 3) * 4]), want_red[i]);
        }
        // Two colour targets, one scaled (181) and one that cannot scale
        // (RGBA32F, not blit-filterable): the other-sized one is left out of
        // the pass instead of failing it.
        {
            nvn::TextureDesc fd = hd;
            fd.format = uint32_t(nvn::Format::RGBA32F);
            backend.create_texture(186, fd);
            const int32_t coords[4] = {1000, 100, 0, 0};
            const uint64_t coords_va = memory.put(0x4E600, coords, sizeof(coords));
            nvn::CommandList ml;
            reset_state(ml, {181, 186});
            ml.commands.push_back(nvn::CmdBindProgram{182});
            ml.commands.push_back(full_layout);
            ml.commands.push_back(nvn::CmdBindVertexBuffer{0, full_va, full.size() * 4});
            ml.commands.push_back(
                nvn::CmdBindUniformBuffer{nvn::ShaderStage::Vertex, nvn::kUniformBufferBankBase, ident_va, 64});
            ml.commands.push_back(nvn::CmdBindUniformBuffer{nvn::ShaderStage::Fragment, 0, coords_va, 16});
            nvn::CmdBindTexture bind;
            bind.stage = nvn::ShaderStage::Fragment;
            bind.slot = 0;
            bind.texture = 180;
            ml.commands.push_back(bind);
            bind.slot = 1;
            ml.commands.push_back(bind);
            ml.commands.push_back(nvn::CmdDraw{nvn::Topology::Triangles, 0, 6, 0, 1});
            const auto before = backend.stats();
            backend.execute(ml);
            const auto after = backend.stats();
            const std::string what = "render scale: colour target of another size left out of the pass";
            const uint64_t n0 = before.diagnostics.count(what) ? before.diagnostics.at(what) : 0;
            const uint64_t n1 = after.diagnostics.count(what) ? after.diagnostics.at(what) : 0;
            CHECK(n1 > n0);
            CHECK_EQ(after.draws - before.draws, 1u);
        }
        // A pass whose only colour target cannot scale: counted as "none of
        // them can scale", with the target's size, format and reason.
        {
            nvn::CommandList ml;
            reset_state(ml, {186});
            const auto before = backend.stats();
            backend.execute(ml);
            const auto after = backend.stats();
            auto grew = [&](const std::string& what) {
                const uint64_t n0 = before.diagnostics.count(what) ? before.diagnostics.at(what) : 0;
                const uint64_t n1 = after.diagnostics.count(what) ? after.diagnostics.at(what) : 0;
                return n1 > n0;
            };
            CHECK(grew("render scale: colour targets left unscaled (none of them can scale)"));
            bool reason = false;
            for (const auto& [name, n] : after.diagnostics)
                reason = reason || (name.rfind("render scale: colour target 1280x720 format ", 0) == 0 &&
                                    name.find("(integer or 32-bit float format)") != std::string::npos);
            CHECK(reason);
        }
    }
    CHECK_EQ(renderer.error_count(), 0u);
    return finish();
}
