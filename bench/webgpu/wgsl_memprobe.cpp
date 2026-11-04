// What shader modules and pipelines cost in host memory (P7, memory issue
// #216 item 6, docs/knowledge/webgpu-shader-speicher.md). No game data in here:
// it reads the WGSL the backend dumps with LGP_WEBGPU_DUMP_DIR
// (prog<id>-<hash>.{vs,fs,cs}.wgsl, translated title shaders, so the
// directory stays private) or any folder of such files.
//
// Phases, each followed by the heap in use (malloc) and, on Darwin, the
// process footprint:
//   modules    every file as a shader module (WGSL parsed and validated by naga)
//   pipelines  per program one pipeline (compute: the .cs; graphics: .vs + .fs,
//              vertex inputs as Float32x4/Uint32x4/Sint32x4 buffers, colour
//              targets RGBA8, auto layout), --variants N times with different
//              override constants (like the backend's flip/alpha variants)
//   released   all module handles released, pipelines kept
//   empty      pipelines released too
//   relieved   malloc asked to hand free pages back (Darwin
//              malloc_zone_pressure_relief, glibc malloc_trim), so that what
//              stays in the footprint is not just malloc keeping free pages
// On Darwin each phase also lists where the footprint sits
// (platform::memory_breakdown: ledgers such as "ledger graphics" and dirty
// pages per VM region tag such as IOAccelerator or MALLOC_SMALL), changes
// against the start, parts of 1 MiB or more (docs/analysis/32-metal-speicher-je-pipeline.md).
// LGP_WGPU_METAL_DROP_LIBRARY=1 with a wgpu-native carrying
// tools/wgpu/metal-drop-library.patch lets pipelines drop their MTLLibrary.
// wgpu-core keeps a reference to its shader modules in every pipeline, so
// "released" is expected to free little (Verdacht from the wgpu source,
// measured here).
//
// usage: lgp_wgsl_memprobe <dir> [--variants N] [--limit N]
//        lgp_wgsl_memprobe --smoke   (two built-in shaders, ctest)
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#if defined(__APPLE__)
#include <mach/mach.h>
#include <malloc/malloc.h>
#elif defined(__linux__)
#include <malloc.h>
#endif

#include "lgp/platform/host_env.hpp"
#include "lgp/platform/webgpu/webgpu_renderer.hpp"
#if !defined(__EMSCRIPTEN__)
#include <webgpu/wgpu.h>  // wgpuDevicePoll
#endif

namespace fs = std::filesystem;
namespace pw = lgp::platform::webgpu;

namespace {

struct Memory {
    double heap_mib = -1;       // malloc in use
    double footprint_mib = -1;  // Darwin phys_footprint (what jetsam compares)
    double seconds = 0;         // steady clock when measured
    std::vector<lgp::platform::MemoryPart> parts;  // Darwin: where the footprint sits
};

Memory measure() {
    Memory m;
    m.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
#if defined(__APPLE__)
    malloc_statistics_t st{};
    malloc_zone_statistics(nullptr, &st);
    m.heap_mib = double(st.size_in_use) / (1 << 20);
    task_vm_info_data_t info{};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS)
        m.footprint_mib = double(info.phys_footprint) / (1 << 20);
#elif defined(__linux__) && defined(__GLIBC__)
    const struct mallinfo2 mi = mallinfo2();
    m.heap_mib = double(mi.uordblks + mi.hblkhd) / (1 << 20);
#endif
    lgp::platform::memory_breakdown(&m.parts);
    return m;
}

// Lets malloc return free pages to the OS where it can.
void relieve_malloc() {
#if defined(__APPLE__)
    malloc_zone_pressure_relief(nullptr, 0);
#elif defined(__linux__) && defined(__GLIBC__)
    malloc_trim(0);
#endif
}

struct Program {
    std::string vs, fs, cs;  // WGSL
};

// Vertex inputs "@location(N) aN: vec4<T>" of the vertex entry point.
std::vector<std::pair<uint32_t, WGPUVertexFormat>> vertex_inputs(const std::string& wgsl) {
    std::vector<std::pair<uint32_t, WGPUVertexFormat>> out;
    const size_t main = wgsl.find("@vertex");
    if (main == std::string::npos)
        return out;
    const size_t open = wgsl.find('(', wgsl.find("fn ", main));
    const std::string params = wgsl.substr(open, wgsl.find("->", open) - open);
    static const std::regex re(R"(@location\((\d+)\)\s*\w+\s*:\s*vec4<(f32|u32|i32)>)");
    for (std::sregex_iterator it(params.begin(), params.end(), re), end; it != end; ++it) {
        const std::string t = (*it)[2];
        out.emplace_back(uint32_t(std::stoul((*it)[1])), t == "f32"   ? WGPUVertexFormat_Float32x4
                                                         : t == "u32" ? WGPUVertexFormat_Uint32x4
                                                                      : WGPUVertexFormat_Sint32x4);
    }
    return out;
}

// Colour outputs "@location(N) name: vec4<T>" of struct FragmentOutput.
std::vector<std::pair<uint32_t, WGPUTextureFormat>> fragment_outputs(const std::string& wgsl) {
    std::vector<std::pair<uint32_t, WGPUTextureFormat>> out;
    const size_t s = wgsl.find("struct FragmentOutput");
    if (s == std::string::npos)
        return out;
    const std::string body = wgsl.substr(s, wgsl.find('}', s) - s);
    static const std::regex re(R"(@location\((\d+)\)[^:]*:\s*vec4<(f32|u32|i32)>)");
    for (std::sregex_iterator it(body.begin(), body.end(), re), end; it != end; ++it) {
        const std::string t = (*it)[2];
        out.emplace_back(uint32_t(std::stoul((*it)[1])), t == "f32"   ? WGPUTextureFormat_RGBA8Unorm
                                                         : t == "u32" ? WGPUTextureFormat_RGBA8Uint
                                                                      : WGPUTextureFormat_RGBA8Sint);
    }
    return out;
}

// Override constants the translator declares, with one value per variant.
std::vector<WGPUConstantEntry> overrides(const std::string& wgsl, uint32_t variant) {
    std::vector<WGPUConstantEntry> c;
    auto add = [&](const char* name, double v) {
        if (wgsl.find(std::string("override ") + name) == std::string::npos)
            return;
        WGPUConstantEntry e = WGPU_CONSTANT_ENTRY_INIT;
        e.key = pw::string_view(name);
        e.value = v;
        c.push_back(e);
    };
    add("lgp_flip_y", double(variant & 1));
    add("lgp_depth_minus_w", double((variant >> 1) & 1));
    add("lgp_alpha_func", double(7 - (variant >> 2) % 8));
    return c;
}

std::string read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream s;
    s << f.rdbuf();
    return s.str();
}

const char* kSmokeVs = R"(struct VertexOutput {
    @builtin(position) position: vec4<f32>,
    @location(0) v0: vec4<f32>,
};
override lgp_flip_y: bool = false;
@vertex
fn main(@location(0) a0: vec4<f32>) -> VertexOutput {
    var o: VertexOutput;
    o.position = select(a0, vec4<f32>(a0.x, -a0.y, a0.zw), lgp_flip_y);
    o.v0 = a0;
    return o;
}
)";
const char* kSmokeFs = R"(struct FragmentOutput {
    @location(0) c0: vec4<f32>,
};
override lgp_alpha_func: u32 = 7u;
@fragment
fn main(@location(0) v0: vec4<f32>) -> FragmentOutput {
    var o: FragmentOutput;
    o.c0 = v0;
    if (lgp_alpha_func == 0u) { discard; }
    return o;
}
)";

// `prev`: the phase before, for the time this phase took.
void print(const char* phase, const Memory& m, const Memory& base, const Memory& prev) {
    std::printf("%-10s heap %8.1f MiB (%+8.1f)", phase, m.heap_mib, m.heap_mib - base.heap_mib);
    if (m.footprint_mib >= 0)
        std::printf("   footprint %8.1f MiB (%+8.1f)", m.footprint_mib, m.footprint_mib - base.footprint_mib);
    std::printf("   %8.0f ms\n", (m.seconds - prev.seconds) * 1000.0);
    if (m.parts.empty())
        return;
    std::map<std::string, double> start;
    for (const auto& p : base.parts) start[p.name] = double(p.bytes) / (1 << 20);
    std::string line;
    for (const auto& p : m.parts) {
        const double mib = double(p.bytes) / (1 << 20);
        const double was = start.count(p.name) ? start[p.name] : 0.0;
        if (mib < 1.0 && was < 1.0)
            continue;
        char item[128];
        std::snprintf(item, sizeof(item), "%s%s %.1f (%+.1f)", line.empty() ? "" : ", ", p.name.c_str(), mib, mib - was);
        line += item;
    }
    std::printf("           parts MiB: %s\n", line.c_str());
}

// Runs create() inside a validation error scope and returns its pipeline,
// or nullptr (after releasing it) when the device rejected it, so that
// rejected pipelines count neither in the total nor per pipeline. wgpu-native
// pops the scope synchronously; in the browser the answer comes later, there
// every pipeline counts and the errors land in the uncaptured count instead.
template <typename Pipeline, typename Create, typename Release>
Pipeline create_valid(WGPUDevice device, Create create, Release release) {
#if defined(__EMSCRIPTEN__)
    (void)release;
    (void)device;
    return create();
#else
    wgpuDevicePushErrorScope(device, WGPUErrorFilter_Validation);
    Pipeline pipeline = create();
    bool valid = false;
    WGPUPopErrorScopeCallbackInfo pop = WGPU_POP_ERROR_SCOPE_CALLBACK_INFO_INIT;
    pop.mode = WGPUCallbackMode_AllowSpontaneous;
    pop.userdata1 = &valid;
    pop.callback = [](WGPUPopErrorScopeStatus status, WGPUErrorType type, WGPUStringView, void* u1, void*) {
        *static_cast<bool*>(u1) = status == WGPUPopErrorScopeStatus_Success && type == WGPUErrorType_NoError;
    };
    wgpuDevicePopErrorScope(device, pop);
    if (valid)
        return pipeline;
    if (pipeline != nullptr)
        release(pipeline);
    return nullptr;
#endif
}

}  // namespace

int main(int argc, char** argv) {
    bool smoke = false;
    uint32_t variants = 1, limit = UINT32_MAX;
    std::string dir;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--smoke")
            smoke = true;
        else if (a == "--variants" && i + 1 < argc)
            variants = std::max(1u, uint32_t(std::strtoul(argv[++i], nullptr, 10)));
        else if (a == "--limit" && i + 1 < argc)
            limit = uint32_t(std::strtoul(argv[++i], nullptr, 10));
        else
            dir = a;
    }
    if (!smoke && dir.empty()) {
        std::fprintf(stderr, "usage: lgp_wgsl_memprobe <dir with prog*.{vs,fs,cs}.wgsl> [--variants N] [--limit N]\n");
        return 2;
    }

    std::map<std::string, Program> programs;  // key: file name up to the stage
    size_t wgsl_bytes = 0;
    if (smoke) {
        programs["smoke"] = Program{kSmokeVs, kSmokeFs, {}};
        variants = 2;
    } else {
        std::vector<fs::path> files;
        for (const auto& e : fs::directory_iterator(dir))
            if (e.path().extension() == ".wgsl")
                files.push_back(e.path());
        std::sort(files.begin(), files.end());
        for (const fs::path& f : files) {
            const std::string name = f.stem().string();  // prog12-<hash>.vs
            const size_t dot = name.rfind('.');
            if (dot == std::string::npos)
                continue;
            const std::string stage = name.substr(dot + 1);
            if (programs.size() >= limit && !programs.count(name.substr(0, dot)))
                continue;
            Program& p = programs[name.substr(0, dot)];
            (stage == "vs" ? p.vs : stage == "fs" ? p.fs : p.cs) = read_file(f);
        }
    }
    for (const auto& [k, p] : programs)
        wgsl_bytes += p.vs.size() + p.fs.size() + p.cs.size();

    pw::RendererOptions options;
    options.size = {64, 64};
    pw::WebGpuRenderer renderer(options);
    if (!renderer.ok()) {
        std::printf("skipped: %s\n", renderer.error().c_str());
        return 77;
    }

    std::vector<WGPUShaderModule> modules;
    std::vector<WGPURenderPipeline> render;
    std::vector<WGPUComputePipeline> compute;
    uint32_t module_count = 0, skipped = 0, rejected = 0;
    const uint64_t errors_before = renderer.error_count();
    Memory base, after_modules, after_pipelines, after_release, after_empty, after_relief;
    renderer.run_sync([&] {
        WGPUDevice device = renderer.device();
        base = measure();
        struct Built {
            WGPUShaderModule vs = nullptr, fs = nullptr, cs = nullptr;
            const Program* p = nullptr;
        };
        std::vector<Built> built;
        for (const auto& [key, p] : programs) {
            Built b;
            b.p = &p;
            if (!p.cs.empty())
                b.cs = pw::create_wgsl_module(device, key.c_str(), p.cs.c_str());
            if (!p.vs.empty())
                b.vs = pw::create_wgsl_module(device, key.c_str(), p.vs.c_str());
            if (!p.fs.empty())
                b.fs = pw::create_wgsl_module(device, key.c_str(), p.fs.c_str());
            for (WGPUShaderModule m : {b.vs, b.fs, b.cs})
                if (m != nullptr) {
                    modules.push_back(m);
                    ++module_count;
                }
            built.push_back(b);
        }
        after_modules = measure();

        for (const Built& b : built) {
            for (uint32_t v = 0; v < variants; ++v) {
                if (b.cs != nullptr) {
                    const std::vector<WGPUConstantEntry> c = overrides(b.p->cs, v);
                    WGPUComputePipelineDescriptor d = WGPU_COMPUTE_PIPELINE_DESCRIPTOR_INIT;
                    d.compute.module = b.cs;
                    d.compute.entryPoint = pw::string_view("main");
                    d.compute.constantCount = c.size();
                    d.compute.constants = c.data();
                    WGPUComputePipeline p = create_valid<WGPUComputePipeline>(
                        device, [&] { return wgpuDeviceCreateComputePipeline(device, &d); },
                        wgpuComputePipelineRelease);
                    if (p != nullptr)
                        compute.push_back(p);
                    else
                        ++rejected;
                    continue;
                }
                if (b.vs == nullptr) {
                    ++skipped;
                    continue;
                }
                const auto inputs = vertex_inputs(b.p->vs);
                std::vector<WGPUVertexAttribute> attrs(inputs.size());
                std::vector<WGPUVertexBufferLayout> buffers(inputs.size());
                for (size_t i = 0; i < inputs.size(); ++i) {
                    attrs[i] = WGPU_VERTEX_ATTRIBUTE_INIT;
                    attrs[i].format = inputs[i].second;
                    attrs[i].shaderLocation = inputs[i].first;
                    buffers[i] = WGPU_VERTEX_BUFFER_LAYOUT_INIT;
                    buffers[i].arrayStride = 16;
                    buffers[i].attributeCount = 1;
                    buffers[i].attributes = &attrs[i];
                }
                const std::vector<WGPUConstantEntry> vc = overrides(b.p->vs, v);
                WGPURenderPipelineDescriptor d = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
                d.vertex.module = b.vs;
                d.vertex.entryPoint = pw::string_view("main");
                d.vertex.bufferCount = buffers.size();
                d.vertex.buffers = buffers.data();
                d.vertex.constantCount = vc.size();
                d.vertex.constants = vc.data();
                d.primitive.topology = WGPUPrimitiveTopology_TriangleList;
                WGPUFragmentState fsd = WGPU_FRAGMENT_STATE_INIT;
                std::vector<WGPUColorTargetState> targets;
                std::vector<WGPUConstantEntry> fc;
                if (b.fs != nullptr) {
                    const auto outs = fragment_outputs(b.p->fs);
                    uint32_t count = 0;
                    for (const auto& [loc, fmt] : outs)
                        count = std::max(count, loc + 1);
                    targets.assign(count, WGPU_COLOR_TARGET_STATE_INIT);
                    for (WGPUColorTargetState& t : targets) {
                        t.format = WGPUTextureFormat_Undefined;  // hole: no target
                        t.writeMask = WGPUColorWriteMask_None;
                    }
                    for (const auto& [loc, fmt] : outs) {
                        targets[loc].format = fmt;
                        targets[loc].writeMask = WGPUColorWriteMask_All;
                    }
                    fc = overrides(b.p->fs, v);
                    fsd.module = b.fs;
                    fsd.entryPoint = pw::string_view("main");
                    fsd.targetCount = targets.size();
                    fsd.targets = targets.data();
                    fsd.constantCount = fc.size();
                    fsd.constants = fc.data();
                    // Like the backend: a program with a fragment shader keeps
                    // it without colour outputs too (depth writes, discard).
                    d.fragment = &fsd;
                }
                // No colour target (vertex only or no colour output): a
                // depth target, as the backend's depth-only passes have.
                WGPUDepthStencilState ds = WGPU_DEPTH_STENCIL_STATE_INIT;
                ds.format = WGPUTextureFormat_Depth32Float;
                ds.depthWriteEnabled = WGPUOptionalBool_True;
                ds.depthCompare = WGPUCompareFunction_Always;
                if (targets.empty())
                    d.depthStencil = &ds;
                WGPURenderPipeline p = create_valid<WGPURenderPipeline>(
                    device, [&] { return wgpuDeviceCreateRenderPipeline(device, &d); }, wgpuRenderPipelineRelease);
                if (p != nullptr)
                    render.push_back(p);
                else
                    ++rejected;
            }
        }
        after_pipelines = measure();

        for (WGPUShaderModule m : modules)
            wgpuShaderModuleRelease(m);
        modules.clear();
        after_release = measure();
        for (WGPURenderPipeline p : render)
            if (p != nullptr) wgpuRenderPipelineRelease(p);
        for (WGPUComputePipeline p : compute)
            if (p != nullptr) wgpuComputePipelineRelease(p);
#if !defined(__EMSCRIPTEN__)
        wgpuDevicePoll(device, true, nullptr);
#endif
        after_empty = measure();
        relieve_malloc();
        after_relief = measure();
    });
    const uint64_t errors = renderer.error_count() - errors_before;
    const size_t pipelines = render.size() + compute.size();

    std::printf("%zu programs, %u modules, %.1f MiB WGSL, %zu pipelines (%u variant(s) each), %u rejected, "
                "%llu other error(s)%s\n",
                programs.size(), module_count, double(wgsl_bytes) / (1 << 20), pipelines, variants, rejected,
                (unsigned long long)errors, skipped ? " (programs without a vertex stage skipped)" : "");
    print("start", base, base, base);
    print("modules", after_modules, base, base);
    print("pipelines", after_pipelines, base, after_modules);
    print("released", after_release, base, after_pipelines);
    print("empty", after_empty, base, after_release);
    print("relieved", after_relief, base, after_empty);
    if (wgsl_bytes > 0 && after_modules.heap_mib >= 0)
        std::printf("modules: %.1f bytes of heap per WGSL byte\n",
                    (after_modules.heap_mib - base.heap_mib) * (1 << 20) / double(wgsl_bytes));
    if (pipelines > 0 && after_pipelines.heap_mib >= 0)
        std::printf("pipelines: %.1f KiB of heap each\n",
                    (after_pipelines.heap_mib - after_modules.heap_mib) * 1024 / double(pipelines));
    if (pipelines > 0 && after_pipelines.footprint_mib >= 0)
        std::printf("pipelines: %.1f KiB of footprint each\n",
                    (after_pipelines.footprint_mib - after_modules.footprint_mib) * 1024 / double(pipelines));
    return smoke && (errors != 0 || rejected != 0 || pipelines != 2) ? 1 : 0;
}
