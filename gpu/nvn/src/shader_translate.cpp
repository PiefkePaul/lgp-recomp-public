// Run-time shader translation for nvnProgramSetShaders (hle.hpp): Maxwell
// SPH + code -> WGSL with gpu/shader, in the WebGPU backend's binding
// convention (backend.hpp, ProgramDesc).
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <string>

#include "lgp/gpu/nvn/hle.hpp"
#include "lgp/gpu/shader/ir.h"
#include "lgp/gpu/shader/sph.h"
#include "lgp/gpu/shader/wgsl.h"
#include "lgp/runtime/log.hpp"

namespace lgp::gpu::nvn {

namespace {

std::atomic<float> render_scale_shaders{1.0f};

// A stage left in the dispatcher form (loop { switch pc }) is logged with its
// reason: on the Adreno 640 that form drew wrong colors (P7.3), the
// structured one did not.
void note_dispatcher(const std::string& wgsl, const char* stage) {
    static const char kTag[] = "// Control flow: dispatcher";
    const size_t at = wgsl.find(kTag);
    if (at == std::string::npos) return;
    const size_t end = wgsl.find('\n', at);
    rt::log(rt::LogLevel::Info, "nvn: %s shader in the dispatcher form:%s", stage,
            wgsl.substr(at + sizeof(kTag) - 1, end - (at + sizeof(kTag) - 1)).c_str());
}

bool emit_stage(const std::vector<uint8_t>& code, const wgsl::Options& options, wgsl::Result* out,
                std::string* error, const char* stage) {
    const auto lifted = ir::lift(code.data(), code.size());
    if (!lifted.ok) {
        *error = std::string(stage) + ": " + lifted.error;
        return false;
    }
    *out = wgsl::emit(lifted.program, options);
    if (!out->ok) {
        *error = std::string(stage) + ": " + out->error;
        return false;
    }
    note_dispatcher(out->code, stage);
    return true;
}

bool slots(const std::vector<uint32_t>& handles, uint32_t base, std::vector<uint32_t>* out, std::string* error) {
    for (uint32_t handle : handles) {
        if (handle < base) {
            *error = "texture handle " + std::to_string(handle) + " below the bound-texture base " +
                     std::to_string(base) + " (bindless textures are not supported yet)";
            return false;
        }
        if ((handle - base) % kBoundTextureHandleStride != 0) {
            *error = "texture handle " + std::to_string(handle) + " is no bound-texture slot (odd word of a handle)";
            return false;
        }
        out->push_back((handle - base) / kBoundTextureHandleStride);
    }
    return true;
}

// Options of a graphics stage: constant buffers and textures per stage in
// their own groups (vertex 0/2, fragment 1/3).
wgsl::Options graphics_options(bool fragment) {
    wgsl::Options o;
    o.cbuf_group = fragment ? 1 : 0;
    o.texture_group = fragment ? 3 : 2;
    o.texture_swizzle = true;
    o.sampler_lod_bias = true;
    o.sampler_border = true;
    o.render_scale = render_scale_shaders.load(std::memory_order_relaxed);
    // Global memory loads share the stage's texture group (4 groups only).
    o.graphics_global_memory = true;
    o.global_group = o.texture_group;
    o.global_binding_base = wgsl::kGraphicsGlobalBindingBase;
    return o;
}

}  // namespace

void set_render_scale_shaders(float scale) { render_scale_shaders.store(scale, std::memory_order_relaxed); }

bool translate_maxwell_program(const std::vector<uint8_t>& vertex, const std::vector<uint8_t>& fragment,
                               uint32_t handle_base, ProgramDesc* out, std::string* error) {
    // An empty fragment stage: a vertex-only program (depth and shadow
    // passes bind no fragment program); the backend draws without
    // fragment shader.
    const bool has_fragment = !fragment.empty();
    shader::Sph fs_sph;
    if (has_fragment && !shader::parse_sph(fragment, fs_sph, *error)) {
        *error = "fragment SPH: " + *error;
        return false;
    }
    // The vertex shader is emitted for its fragment partner (interpolation
    // lives in the fragment SPH).
    wgsl::Options vs_opt = graphics_options(false), fs_opt = graphics_options(true);
    if (has_fragment)
        vs_opt.vertex_output_interp = wgsl::fragment_input_interpolation(fs_sph);
    wgsl::Result vs, fs;
    if (!emit_stage(vertex, vs_opt, &vs, error, "vertex") ||
        (has_fragment && !emit_stage(fragment, fs_opt, &fs, error, "fragment")))
        return false;
    ProgramDesc d;
    d.texture_swizzle = true;
    d.vertex_wgsl = std::move(vs.code);
    d.fragment_wgsl = std::move(fs.code);
    d.vertex_cbufs = vs.bindings.cbuf_banks;
    d.fragment_cbufs = fs.bindings.cbuf_banks;
    d.vertex_texture_dims = vs.bindings.texture_dims;
    d.fragment_texture_dims = fs.bindings.texture_dims;
    d.vertex_global_memory = vs.bindings.global_memory;
    d.vertex_texture_arrays = vs.bindings.texture_arrays;
    d.fragment_texture_arrays = fs.bindings.texture_arrays;
    d.vertex_texture_depth = vs.bindings.texture_depth;
    d.fragment_texture_depth = fs.bindings.texture_depth;
    d.fragment_global_memory = fs.bindings.global_memory;
    for (const wgsl::Result* r : {&vs, &fs})
        if (r->bindings.global_memory && 2 * r->bindings.texture_handles.size() + 1 > wgsl::kGraphicsGlobalBindingBase) {
            *error = "too many textures next to global memory";
            return false;
        }
    if (!slots(vs.bindings.texture_handles, handle_base, &d.vertex_textures, error) ||
        !slots(fs.bindings.texture_handles, handle_base, &d.fragment_textures, error))
        return false;
    *out = std::move(d);
    return true;
}

bool translate_maxwell_fragment(const std::vector<uint8_t>& fragment, std::string* wgsl, std::string* error) {
    wgsl::Result fs;
    if (!emit_stage(fragment, graphics_options(true), &fs, error, "fragment"))
        return false;
    *wgsl = std::move(fs.code);
    return true;
}

bool parse_compute_control(const uint8_t* control, size_t size, ComputeInfo* out, std::string* error) {
    const auto u32 = [control](uint64_t offset) {
        uint32_t v;
        std::memcpy(&v, control + offset, sizeof(v));
        return v;
    };
    if (size < kNvnControlComputeEnd || u32(0) != kNvnControlMagic) {
        *error = "no NVN control section (magic 0x98761234)";
        return false;
    }
    if (u32(kNvnControlStage) != kNvnStageCompute) {
        *error = "control section of stage " + std::to_string(u32(kNvnControlStage)) + ", not compute (5)";
        return false;
    }
    ComputeInfo info;
    uint64_t threads = 1;
    for (int i = 0; i < 3; ++i) {
        info.workgroup[i] = u32(kNvnControlComputeWorkgroup + 4 * i);
        threads *= info.workgroup[i];
    }
    // Maxwell runs at most 1024 threads per CTA (z at most 64) and has 48 KiB
    // shared memory: anything else is no layout this reader knows.
    if (threads == 0 || threads > 1024 || info.workgroup[2] > 64) {
        *error = "workgroup size " + std::to_string(info.workgroup[0]) + "x" + std::to_string(info.workgroup[1]) +
                 "x" + std::to_string(info.workgroup[2]) + " is no Maxwell CTA (control layout differs?)";
        return false;
    }
    info.shared_memory_bytes = u32(kNvnControlComputeSharedMemory);
    if (info.shared_memory_bytes > 48 * 1024) {
        *error = "shared memory " + std::to_string(info.shared_memory_bytes) +
                 " bytes is more than Maxwell has (control layout differs?)";
        return false;
    }
    // The translator addresses local memory from 0 up; the negative part
    // ("high") has no counterpart there and was 0 in every public example.
    if (const uint32_t high = u32(kNvnControlComputeLocalMemoryHigh); high != 0) {
        *error = "high local memory " + std::to_string(high) + " bytes is not supported by the translator";
        return false;
    }
    info.local_memory_bytes = u32(kNvnControlComputeLocalMemoryLow);
    *out = info;
    return true;
}

bool translate_maxwell_compute(const std::vector<uint8_t>& code, const ComputeInfo& info, uint32_t handle_base,
                               ProgramDesc* out, std::string* error) {
    const auto lifted = ir::lift_compute(code.data(), code.size());
    if (!lifted.ok) {
        *error = "compute: " + lifted.error;
        return false;
    }
    wgsl::Options opt;
    opt.cbuf_group = 0;
    opt.texture_group = 1;
    opt.global_group = 2;
    opt.workgroup_size = {info.workgroup[0], info.workgroup[1], info.workgroup[2]};
    opt.shared_memory_bytes = info.shared_memory_bytes;
    opt.local_memory_bytes = info.local_memory_bytes;
    opt.texture_swizzle = true;
    opt.sampler_lod_bias = true;
    opt.sampler_border = true;
    opt.render_scale = render_scale_shaders.load(std::memory_order_relaxed);
    // LGP_WEBGPU_DEBUG=computeprobe: count global memory accesses per dispatch
    // (webgpu_backend.cpp, docs/knowledge/nvn-hle.md).
    static const bool probe = [] {
        const char* env = std::getenv("LGP_WEBGPU_DEBUG");
        return env != nullptr && std::strstr(env, "computeprobe") != nullptr;
    }();
    opt.gmem_probe = probe;
    // Accesses traced to one storage descriptor go straight to its slot (much
    // less code for the driver to compile); LGP_WEBGPU_DEBUG=gmemsearch keeps
    // the table search everywhere, for comparison (docs/knowledge/nvn-hle.md).
    static const bool search = [] {
        const char* env = std::getenv("LGP_WEBGPU_DEBUG");
        return env != nullptr && std::strstr(env, "gmemsearch") != nullptr;
    }();
    if (!search) {
        opt.storage_desc_offset = kStorageBufferDescOffsetCompute;
        opt.uniform_desc_offset = kComputeUniformDescOffset;
        opt.storage_desc_bank = kDriverConstantBank;
    }
    wgsl::Result cs = wgsl::emit(lifted.program, opt);
    if (!cs.ok) {
        *error = "compute: " + cs.error;
        return false;
    }
    note_dispatcher(cs.code, "compute");
    ProgramDesc d;
    d.texture_swizzle = true;
    d.compute_wgsl = std::move(cs.code);
    d.compute_cbufs = cs.bindings.cbuf_banks;
    d.compute_global_memory = cs.bindings.global_memory;
    d.compute_gmem_probe = cs.bindings.gmem_probe;
    d.compute_texture_dims = cs.bindings.texture_dims;
    d.compute_texture_arrays = cs.bindings.texture_arrays;
    d.compute_texture_depth = cs.bindings.texture_depth;
    if (!slots(cs.bindings.texture_handles, handle_base, &d.compute_textures, error))
        return false;
    *out = std::move(d);
    return true;
}

}  // namespace lgp::gpu::nvn
