// WGSL backend for the shader IR (vertex, fragment and compute stages).
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "lgp/gpu/shader/ir.h"

namespace lgp::gpu::wgsl {

// Resource layout of the generated module. Bindings are fixed by convention
// so a pipeline builder can create matching bind group layouts (groups are
// configurable in Options, defaults below):
//   group(0) binding(b)      = constant buffer c[b], array<vec4<u32>, 4096>
//   group(1) binding(2k)     = texture for the k-th distinct texture handle
//                              (texture_2d<f32>, texture_3d<f32> or
//                              texture_depth_2d, see Bindings)
//   group(1) binding(2k + 1) = sampler for the same handle
// Compute shaders with global memory access (LDG/STG/ATOM/RED) also use
//   group(2) binding(k), k < 8 = storage buffer slot k, array<atomic<u32>>
//   group(2) binding(8)        = slot table, array<vec4<u32>, 8> (compute: 12): per slot
//                                (guest address low, high, size in bytes, 0)
// Guest addresses are 64-bit; an access goes to the slot whose range holds the
// address (loads outside every slot read 0, stores are dropped). The backend
// binds the guest ranges (NVN storage buffers) to the slots and copies them
// back afterwards; unused slots get a small dummy buffer and size 0.
constexpr uint32_t kGlobalSlots = 8;
// Compute only: the slot table has kConstantSlots more entries, k = 8..11,
// for constant buffers the shader reads through global memory (NVN compute
// reads its uniform blocks that way). Entry 8 + j reads
//   group(2) binding(kConstantSlotBinding + j) = array<vec4<u32>, 4096> (uniform,
//   a 64 KiB window from the entry's address); stores there are dropped.
// Uniform bindings keep all 8 storage slots free (8 is WebGPU's default limit).
constexpr uint32_t kConstantSlots = 4;
constexpr uint32_t kConstantSlotBinding = kGlobalSlots + 2;  // after the table and the probe
constexpr uint32_t kNoStorageDesc = ~0u;  // Options::storage_desc_offset: off
// move_static_slots: ids of the statically resolved targets, storage slot k
// = k, uniform block s = kStaticUniformId + s.
constexpr uint32_t kStaticUniformId = 16;
constexpr uint32_t kUniformDescCount = 16;  // Options::uniform_desc_offset: blocks s < 16

struct Bindings {
    std::vector<uint32_t> cbuf_banks;       // banks used, ascending
    std::vector<uint32_t> texture_handles;  // handles used, index = k
    std::vector<uint32_t> texture_dims;     // per binding k: 2 = texture_2d, 3 = texture_3d
    std::vector<bool> texture_arrays;       // per k: texture_2d_array instead of texture_2d
    std::vector<bool> texture_depth;        // per k: texture_depth_2d(_array) + sampler_comparison
    bool global_memory = false;             // group(global_group) is used
    uint32_t shared_memory_bytes = 0;       // var<workgroup> size (compute)
    uint32_t local_memory_bytes = 0;        // per-invocation local memory (LDL/STL)
    bool barrier_lockstep = false;          // BAR in divergent code: emulated, see knowledge doc
    bool gmem_probe = false;                // Options::gmem_probe binding is declared
    // Options::storage_desc_offset: slots k with a statically resolved access,
    // ascending; the module declares "const gs<k>: u32 = k" for each.
    std::vector<uint32_t> static_storage_slots;
    // Options::uniform_desc_offset: uniform blocks s with a statically
    // resolved access, ascending; block static_uniform_blocks[j] reads
    // constant window j (table entry kGlobalSlots + j), at most kConstantSlots.
    std::vector<uint32_t> static_uniform_blocks;
    uint32_t gmem_static_accesses = 0;   // global memory accesses going straight to their slot
    uint32_t gmem_dynamic_accesses = 0;  // ... and those searching the slot table
};

struct Result {
    bool ok = false;
    std::string error;
    std::string code;
    Bindings bindings;
};

// Interpolation of a generic varying. WebGPU requires the vertex output and
// the fragment input of a location to agree, while Maxwell keeps the mode only
// in the fragment shader's SPH. A vertex shader therefore has to be emitted
// for the fragment shader it is linked with.
enum class Interp : uint8_t { Perspective, Linear, Flat };

// Default of Options::static_stacks: $LGP_WGSL_STATIC_STACKS ("0"/"1") if
// set, else off.
bool static_stacks_default();

// Default of Options::structured: $LGP_WGSL_STRUCTURED ("0"/"1") if set,
// else on.
bool structured_default();

// Default of Options::compare_const: $LGP_WGSL_COMPARE_CONST ("0"/"1") if
// set, else off.
bool compare_const_default();

// Default of Options::vector_select: $LGP_WGSL_VECTOR_SELECT ("0"/"1") if
// set, else on for Android builds (Adreno, P7.3), off elsewhere.
bool vector_select_default();

struct Options {
    // Constant-buffer words read at a run-time offset (c[Rn + imm], and the
    // constant buffer windows of global memory) pick the vec4 component with
    // select() instead of a dynamic index v[i]. Workaround for drivers that
    // return component x for every dynamic index into a uniform vec4
    // (Verdacht: Adreno 640, all invocations read word 0; P7.3).
    bool vector_select = vector_select_default();
    // Code with branches: when every SSY/PBK stack is the same on all paths to
    // each instruction (structured code), SYNC/BRK jump to their target
    // directly and the private ssy/pbk stack arrays are left out; otherwise
    // the stacks stay. Shrinks what the driver compiles (dynamically indexed
    // private arrays). Off by default until measured on devices.
    bool static_stacks = static_stacks_default();
    // Code with branches: emit if/else and loop { } that follow the control
    // flow graph instead of the loop { switch pc { ... } } dispatcher, when
    // the stacks resolve statically (as static_stacks) and the graph has that
    // shape (reducible, every loop left to one place, every branch region
    // joined without shared blocks); otherwise the dispatcher stays. Not for
    // compute shaders in lockstep (barriers with branches). On by default:
    // cold draw pipelines -11 % on a PC (Vulkan, RTX 5070 Ti) and -10 % on
    // an M1 (Metal), programs -12/-14 %; every game shader with branches
    // structured.
    bool structured = structured_default();
    // Diagnosis (wrong image): every depth-compare sample (shadow lookup,
    // also gathered) yields 1.0 without reading the texture, so the cost of
    // the shadow lookups shows as the difference to a run without it.
    bool compare_const = compare_const_default();
    // Vertex stage: interpolation per generic output location. Locations not
    // listed use perspective/center (the WebGPU default).
    std::map<uint32_t, Interp> vertex_output_interp;
    // Bind groups of the constant buffers and textures. Maxwell gives every
    // stage its own constant buffers, so a pipeline with two stages needs
    // them in different groups (the WebGPU backend uses vertex 0/2,
    // fragment 1/3; WebGPU guarantees 4 groups).
    uint32_t cbuf_group = 0;
    uint32_t texture_group = 1;
    uint32_t global_group = 2;
    // Compute: workgroup size. Emitted as the defaults of the pipeline-overridable
    // constants wg_size_x/y/z (NVN keeps the size in the program's control section,
    // not in the code).
    std::array<uint32_t, 3> workgroup_size{1, 1, 1};
    // Compute: shared memory in bytes (0 = 16 KiB, the WebGPU default limit). Only
    // declared if the shader accesses shared memory.
    uint32_t shared_memory_bytes = 0;
    // Local memory per invocation in bytes (0 = the SPH value for graphics stages,
    // 4 KiB for compute). Only declared if the shader uses LDL/STL.
    uint32_t local_memory_bytes = 0;
    // Texture view swizzle at run time: colour texels pass through a
    // per-binding component swizzle read from
    //   @group(texture_group) @binding(2 * N) var<uniform> tex_swizzle: array<vec4<u32>, (N + 3) / 4>
    // (N = texture bindings; binding k uses word k: 3 bits per component r, g,
    // b, a from bit 0, values kSwizzleZero..kSwizzleA). Off: no such binding.
    bool texture_swizzle = false;
    // Sampler LOD bias at run time (needs texture_swizzle): bits 16..31 of
    // binding k's swizzle word hold the bias as signed fixed point with 8
    // fraction bits (kLodBiasShift), added to the LOD of every filtered
    // colour lookup: implicit (textureSampleBias), explicit and LZ (level +
    // bias) and shader-biased ones. Depth-compare lookups and texel fetches
    // ignore it (WGSL has no biased compare form).
    bool sampler_lod_bias = false;
    // Sampler border for depth-compare lookups at run time (needs
    // texture_swizzle; WebGPU has no CLAMP_TO_BORDER): bits 12 and 13 of
    // binding k's swizzle word (kBorderU, kBorderV) mark the axes with border
    // addressing; a 2D compare lookup (TEX/TEXS/TLD4 with .DC) whose
    // coordinates leave [0, 1] on such an axis returns bit 14 (kBorderPass)
    // as its result, i.e. the comparison against the border depth. A 2D
    // colour lookup (TEX/TEXS, not gathers) there returns the border colour
    // with r = g = b = bit 14 and a = bit 15 (kBorderAlpha), after the view
    // swizzle; other border colours are approximated by the backend.
    // Compare lookups ignore the LOD bias, so their bit 16 (kCompareClampRef)
    // is free: set, the reference is clamped to [0, 1] first, as the hardware
    // does for fixed-point depth formats (WebGPU may back them with float).
    // Bit 17 (kBorderForce, compare words only: colour words hold the LOD
    // bias from bit 16) treats every compare lookup as outside (diagnosis: all
    // shadow lookups return kBorderPass).
    bool sampler_border = false;
    // Render scale s (needs texture_swizzle; 1 = off): render targets may
    // live on the host at s times their guest size. The tex_swizzle array
    // gets one more vec4<u32> after the swizzle words, whose bit k (word
    // k / 32) marks binding k as scaled; s is `const lgp_render_scale`.
    // Texel fetches (TLD/TLDS) of a scaled binding read the host texel that
    // holds the guest texel's centre, size queries (TXQ) return the guest
    // size (rounded: odd guest sizes can be off by one). Fragment shaders
    // whose gl_FragCoord.xy is read divide it by `override
    // lgp_target_scale: f32 = 1.0` (s while drawing into a scaled target).
    // Only shaders that do one of these change; filtered lookups use
    // normalized coordinates and need nothing. Texel offsets and TMML stay
    // in host texels (docs/analysis/37-render-skalierung-entwurf.md).
    float render_scale = 1.0f;
    // Vertex/fragment shaders may load global memory (LDG; stores and atomics
    // still fail): read-only storage buffers
    //   @group(global_group) @binding(global_binding_base + k), k < 8, and the
    //   slot table at global_binding_base + 8,
    // so a graphics stage can share its texture group (WebGPU guarantees only 4).
    bool graphics_global_memory = false;
    uint32_t global_binding_base = 0;
    // Compute with global memory, diagnostics only: count accesses in
    //   @group(global_group) @binding(global_binding_base + kGlobalSlots + 1)
    //   var<storage, read_write> gmem_probe: array<atomic<u32>, kGmemProbeWords>
    // (word layout kProbe*; the buffer must start zeroed). That is a ninth
    // storage buffer: the device needs maxStorageBuffersPerShaderStage >= 9
    // (WebGPU guarantees 8).
    bool gmem_probe = false;
    // Compute: the storage buffer descriptors are in
    //   c[storage_desc_bank][storage_desc_offset + 16 k], k < kGlobalSlots
    // (address low, high, size; NVN: bank 0, 0x310), and the backend binds
    // descriptor k's range to slot k. An access whose address the translator
    // can trace back to exactly one descriptor (through moves and additions)
    // then checks only that slot's table entry instead of searching the
    // table, which shrinks the code the driver compiles a lot (the search,
    // inlined per access, dominated pipeline creation). When the backend
    // binds k's range in another slot j instead (WebGPU forbids binding one
    // writable range twice), move_static_slots gives the module for that.
    // kNoStorageDesc (default): every access searches the table. Ignored
    // with gmem_probe.
    uint32_t storage_desc_offset = kNoStorageDesc;
    uint32_t storage_desc_bank = 0;
    // Likewise for constant buffers read through global memory: uniform block
    // s's descriptor at c[storage_desc_bank][uniform_desc_offset + 16 s],
    // s < kUniformDescCount (NVN compute: 0x210). The blocks with resolved
    // accesses (Bindings::static_uniform_blocks) take the constant windows in
    // ascending order; the backend fills window j with block j's range, or
    // moves the block to the storage slot that holds it (move_static_slots).
    uint32_t uniform_desc_offset = kNoStorageDesc;
};

// Options::gmem_probe counters (words of gmem_probe).
inline constexpr uint32_t kProbeInvocations = 0;    // compute invocations that started
inline constexpr uint32_t kProbeLoadHit = 1;        // words loaded from a slot
inline constexpr uint32_t kProbeLoadMiss = 2;       // words loaded outside every slot (read 0)
inline constexpr uint32_t kProbeStoreHit = 3;       // stores into a slot
inline constexpr uint32_t kProbeStoreMiss = 4;      // stores outside every slot (dropped)
inline constexpr uint32_t kProbeAtomicHit = 5;
inline constexpr uint32_t kProbeAtomicMiss = 6;
inline constexpr uint32_t kProbeAddressMiss = 7;    // addresses outside every slot
inline constexpr uint32_t kProbeFirstMissLo = 8;    // one missed address (low, high word)
inline constexpr uint32_t kProbeFirstMissHi = 9;
inline constexpr uint32_t kProbeSlotStores = 10;    // + k: stores into slot k
inline constexpr uint32_t kGmemProbeWords = kProbeSlotStores + kGlobalSlots;

// Binding base for global memory in a graphics stage's texture group (after
// at most 31 textures and the swizzle uniform).
inline constexpr uint32_t kGraphicsGlobalBindingBase = 64;

inline constexpr uint32_t kSwizzleZero = 0, kSwizzleOne = 1, kSwizzleR = 2, kSwizzleG = 3, kSwizzleB = 4,
                          kSwizzleA = 5;
inline constexpr uint32_t kSwizzleIdentity = kSwizzleR | kSwizzleG << 3 | kSwizzleB << 6 | kSwizzleA << 9;
// Options::sampler_lod_bias: the bias in a swizzle word.
inline constexpr uint32_t kLodBiasShift = 16;
inline uint32_t lod_bias_bits(float bias) {
    const float b = bias < -128.0f ? -128.0f : bias > 127.99609375f ? 127.99609375f : bias;
    const int32_t fixed = static_cast<int32_t>(b * 256.0f);
    return static_cast<uint32_t>(static_cast<uint16_t>(static_cast<int16_t>(fixed))) << kLodBiasShift;
}

// Options::sampler_border: border flags in a swizzle word.
inline constexpr uint32_t kBorderU = 1u << 12, kBorderV = 1u << 13, kBorderPass = 1u << 14, kBorderAlpha = 1u << 15,
                          kCompareClampRef = 1u << 16, kBorderForce = 1u << 17;

// Interpolation per generic input location of a fragment shader, as encoded
// in its SPH (first used component decides). Feed the result into
// Options::vertex_output_interp of the vertex shader it is linked with.
std::map<uint32_t, Interp> fragment_input_interpolation(const shader::Sph& fragment_sph);

Result emit(const ir::Program& program, const Options& options = {});

// A compute module from emit() with Options::storage_desc_offset in which
// the statically resolved target id (storage slot k = k, uniform block s =
// kStaticUniformId + s) reads table entry moved[id] instead: storage slot
// e (binding e) for e < kGlobalSlots, else constant window e - kGlobalSlots.
// For a dispatch that binds slot k's range only in another slot, or whose
// uniform block lies in a storage slot. Empty if `code` has no such target.
std::string move_static_slots(const std::string& code, const std::map<uint32_t, uint32_t>& moved);

// Type of a vertex input as the vertex fetch delivers it: converted to float
// (normalized, float formats) or raw integers (UINT/SINT formats). Maxwell
// shaders read the attribute registers as bits either way; WebGPU needs the
// shader's input type to match the format's (f32 vs u32 vs i32).
// UScaled/SScaled: integer formats the shader reads as float (not
// normalised); Packed*: one u32 holding x:10 y:10 z:10 w:2 from bit 0, read as
// unsigned/signed integers, snorm, or integers converted to float. WebGPU has
// no vertex format for these, the module converts after the fetch.
enum class VertexInputType : uint8_t {
    Float, Uint, Sint, UScaled, SScaled,
    PackedUint, PackedSint, PackedSnorm, PackedUScaled, PackedSScaled,
};

// A vertex module from emit() with the inputs at the listed locations declared
// as vec4<u32> (Uint, UScaled, Packed*: the format is Uint*/Uint32) or
// vec4<i32> (Sint, SScaled) instead of vec4<f32>, and converted to the bits
// the shader body reads (unchanged). Locations the shader does not read are
// ignored. Empty if `code` is not in emit()'s form.
std::string retype_vertex_inputs(const std::string& code, const std::map<uint32_t, VertexInputType>& types);

}  // namespace lgp::gpu::wgsl
