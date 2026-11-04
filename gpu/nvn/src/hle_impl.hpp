// Internal state of the NVN-HLE, shared by hle.cpp (thunks, proc table) and
// procs.cpp (the NVN functions).
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "lgp/gpu/nvn/hle.hpp"
#include "lgp/lifted/cpu_state.hpp"
#include "lgp/runtime/host_work.hpp"
#include "lgp/runtime/time_stats.hpp"
#include "states.hpp"

namespace lgp::gpu::nvn {

// An NVN function: reads its arguments from ctx (AAPCS64) and guest memory,
// writes the result to x0. Called with the HLE mutex held.
using ProcFn = void (*)(Hle::Impl& hle, CpuState* ctx);

struct ProcImpl {
    std::string_view name;
    ProcFn fn;
};
// All implemented NVN functions (procs.cpp), sorted by name.
const ProcImpl* find_proc_impl(std::string_view name);
size_t proc_impl_count();
// The draw-related part (procs_draw.cpp), sorted by name; find_proc_impl
// searches both tables.
const ProcImpl* draw_proc_table(size_t* count);

// Recording commands whose names start with this are command-buffer
// commands: in UnimplementedMode::Continue an unimplemented one is recorded
// as CmdUnsupported instead of vanishing.
inline bool is_command_buffer_proc(std::string_view name) {
    return name.rfind("nvnCommandBuffer", 0) == 0 && name != "nvnCommandBufferInitialize" &&
           name != "nvnCommandBufferFinalize";
}

// Kinds of backend_call_times(), in its order.
enum class BackendCall : uint8_t { Program, Texture, WriteTexture, Execute, Present, Finish, Count };
struct BackendCallCounter {
    std::atomic<uint64_t> ns{0};
    std::atomic<uint64_t> calls{0};
};
extern BackendCallCounter g_backend_calls[size_t(BackendCall::Count)];
// Time of one backend call by kind. Declared after the HostWorkScope, so it
// stops before the thread waits to get its scheduler slot back (that wait
// counts as ready, not gpu).
class BackendCallTimer {
public:
    explicit BackendCallTimer(BackendCall kind) : kind_(kind), timed_(rt::time_stats_enabled()) {
        if (timed_) t0_ = std::chrono::steady_clock::now();
    }
    ~BackendCallTimer() {
        if (!timed_) return;
        BackendCallCounter& c = g_backend_calls[size_t(kind_)];
        c.ns.fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                    std::chrono::steady_clock::now() - t0_)
                                    .count()),
                       std::memory_order_relaxed);
        c.calls.fetch_add(1, std::memory_order_relaxed);
    }
    BackendCallTimer(const BackendCallTimer&) = delete;
    BackendCallTimer& operator=(const BackendCallTimer&) = delete;

private:
    BackendCall kind_;
    bool timed_;
    std::chrono::steady_clock::time_point t0_;
};

// Forwards to the renderer's backend; the time inside counts as
// rt::TimeCat::Gpu (the guest thread is blocked until the GPU thread has
// recorded the work, time_stats.hpp; with AsyncBackend only queueing and
// the waits for fences and presents). With LGP_RENDER_RELEASE_SLOT the
// thread gives its scheduler slot up meanwhile (host_work.hpp).
class TimedBackend final : public Backend {
public:
    explicit TimedBackend(Backend& inner) : inner_(inner) {}
    void create_program(ProgramId id, const ProgramDesc& desc) override {
        rt::TimeScope t(rt::TimeCat::Gpu);
        rt::HostWorkScope away;
        BackendCallTimer timer(BackendCall::Program);
        inner_.create_program(id, desc);
    }
    void destroy_program(ProgramId id) override {
        rt::TimeScope t(rt::TimeCat::Gpu);
        rt::HostWorkScope away;
        BackendCallTimer timer(BackendCall::Program);
        inner_.destroy_program(id);
    }
    void create_texture(TextureId id, const TextureDesc& desc) override {
        rt::TimeScope t(rt::TimeCat::Gpu);
        rt::HostWorkScope away;
        BackendCallTimer timer(BackendCall::Texture);
        inner_.create_texture(id, desc);
    }
    void destroy_texture(TextureId id) override {
        rt::TimeScope t(rt::TimeCat::Gpu);
        rt::HostWorkScope away;
        BackendCallTimer timer(BackendCall::Texture);
        inner_.destroy_texture(id);
    }
    void write_texture(TextureId id, uint32_t level, const Rect& region, const void* data, uint32_t row_stride,
                       const TexelSlices& slices) override {
        rt::TimeScope t(rt::TimeCat::Gpu);
        rt::HostWorkScope away;
        BackendCallTimer timer(BackendCall::WriteTexture);
        inner_.write_texture(id, level, region, data, row_stride, slices);
    }
    void execute(const CommandList& list) override {
        rt::TimeScope t(rt::TimeCat::Gpu);
        rt::HostWorkScope away;
        BackendCallTimer timer(BackendCall::Execute);
        inner_.execute(list);
    }
    void execute_owned(CommandList&& list) override {
        rt::TimeScope t(rt::TimeCat::Gpu);
        rt::HostWorkScope away;
        BackendCallTimer timer(BackendCall::Execute);
        inner_.execute_owned(std::move(list));
    }
    bool present(const PresentInfo& info) override {
        rt::TimeScope t(rt::TimeCat::Gpu);
        rt::HostWorkScope away;
        BackendCallTimer timer(BackendCall::Present);
        return inner_.present(info);
    }
    void finish() override {
        rt::TimeScope t(rt::TimeCat::Gpu);
        rt::HostWorkScope away;
        BackendCallTimer timer(BackendCall::Finish);
        inner_.finish();
    }
    uint64_t fence_mark() override { return inner_.fence_mark(); }
    void finish_until(uint64_t mark) override {
        rt::TimeScope t(rt::TimeCat::Gpu);
        rt::HostWorkScope away;
        BackendCallTimer timer(BackendCall::Finish);
        inner_.finish_until(mark);
    }
    void after_queued(std::function<void()> fn) override { inner_.after_queued(std::move(fn)); }

private:
    Backend& inner_;
};

struct Hle::Impl {
    Impl(const HleOptions& o, Backend& b) : options(o), timed_backend(b), backend(timed_backend) {}

    HleOptions options;
    TimedBackend timed_backend;
    Backend& backend;
    std::recursive_mutex mutex;

    struct Proc {
        std::string name;
        ProcFn fn = nullptr;
        bool command = false;
        std::atomic<uint64_t> calls{0};
    };
    std::unique_ptr<Proc[]> procs{new Proc[kMaxProcs]};
    uint32_t proc_count = 0;
    std::unordered_map<std::string, uint32_t> proc_by_name;
    std::set<std::string> unimplemented;
    // accept_untranslated_programs: programs left without shaders, by reason.
    uint64_t untranslated = 0;
    std::map<std::string, uint64_t> untranslated_reasons;
    std::set<uint32_t> logged_device_keys;
    std::map<uint32_t, int32_t> device_integers;

    uint64_t thunk_va(uint32_t index) const { return options.thunk_base + uint64_t{index} * 4; }
    // Index for a name, allocating a thunk on first use; UINT32_MAX if full.
    uint32_t proc_index(std::string_view name);

    // ---- Host-side objects, keyed by the guest address of the NVN object.
    struct Device {
        uint32_t flags = 0;
        uint32_t depth_mode = static_cast<uint32_t>(DepthMode::NearIsMinusW);
        uint32_t origin_mode = static_cast<uint32_t>(WindowOriginMode::LowerLeft);
    };
    struct Queue {
        uint64_t device = 0;
        // Pools the last submitted command list left bound (GPU state).
        uint64_t texture_pool = 0;
        uint64_t sampler_pool = 0;
        // Statistics counters (ReportCounter/ResetCounter), by NVNcounterType,
        // estimated from the submitted draws (submit_counters in procs.cpp),
        // and the rasterizer discard state they depend on.
        uint64_t counters[17] = {};
        bool rasterizer_discard = false;
    };
    struct MemoryPool {
        uint64_t device = 0;
        uint64_t storage = 0;  // guest VA = GPU address (identity)
        uint64_t size = 0;
        uint32_t flags = 0;
    };
    struct Buffer {
        uint64_t pool = 0;
        uint64_t address = 0;  // guest VA = GPU address
        uint64_t size = 0;
    };
    struct Texture {
        TextureId id = 0;
        TextureDesc desc;
        uint32_t flags = 0;
        uint32_t stride = 0;
        uint64_t pool = 0;
        uint64_t offset = 0;
    };
    struct Window {
        uint64_t device = 0;
        uint64_t native_window = 0;
        std::vector<uint64_t> textures;  // guest addresses of NVNtexture
        int32_t present_interval = 1;
        Rect crop;
        uint32_t next = 0;  // texture index the next acquire returns
    };
    struct Sync {
        bool signaled = false;
        uint64_t mark = 0;  // Backend::fence_mark() when fenced
    };
    struct CommandBuffer {
        uint64_t device = 0;
        bool recording = false;
        std::shared_ptr<CommandList> list;
        // Guest memory range [begin, end) given to the buffer.
        struct MemoryRange {
            uint64_t begin = 0, end = 0;
        };
        MemoryRange command_memory, control_memory;  // last added
        std::vector<MemoryRange> recording_memory;  // what the current recording wrote to (on hardware)
        // Handles this buffer ended -> the memory their recording used.
        std::unordered_map<uint64_t, std::vector<MemoryRange>> handles;
        uint64_t memory_callback = 0;
        uint64_t memory_callback_data = 0;
        // Recording state that NVN resolves at record time in the HLE.
        uint64_t texture_pool = 0;
        uint64_t sampler_pool = 0;
        uint32_t copy_row_stride = 0;
        uint64_t copy_image_stride = 0;
        CmdSetVertexLayout vertex_layout;  // last bound attrib + stream states
        // Graphics stages bound so far (vertex, fragment): BindProgram with
        // stage bits can take them from different (separable) programs.
        std::shared_ptr<const std::vector<uint8_t>> bound_code[2];
        // Only a vertex stage is bound: its vertex-only program is made at
        // the next draw (not at bind, where a fragment stage often follows).
        bool vertex_only_pending = false;
    };
    struct Program {
        ProgramId id = 0;  // 0 = no valid shaders set
        bool compute = false;
        // Compiler constant pool per stage (vertex, fragment), bound to
        // c[kCompilerConstantBank] with the program; size 0 = none.
        struct Constants {
            uint64_t address = 0, size = 0;
        } constants[2];
        // Graphics stage code (SPH + Maxwell code) per stage; a separable
        // program has only one. Kept to combine stages at BindProgram.
        std::shared_ptr<const std::vector<uint8_t>> code[2];
        // Why the translator rejected it (id 0), for the bind-time log.
        std::shared_ptr<const std::string> untranslated;
    };
    struct Sampler {
        SamplerDesc desc;
    };
    // Texture and sampler pools: descriptor id -> object, written by
    // Register*. The game's pool memory is not written.
    struct TexturePool {
        uint64_t storage = 0;
        int32_t size = 0;
        struct Entry {
            TextureId texture = 0;
            SwizzleRgba swizzle = kIdentitySwizzle;
            uint64_t object = 0;  // the NVNtexture registered here
        };
        std::unordered_map<int32_t, Entry> textures;
    };
    struct SamplerPool {
        uint64_t storage = 0;
        int32_t size = 0;
        std::unordered_map<int32_t, SamplerDesc> samplers;
    };

    std::unordered_map<uint64_t, Device> devices;
    std::unordered_map<uint64_t, Queue> queues;
    std::unordered_map<uint64_t, MemoryPool> pools;
    std::unordered_map<uint64_t, Buffer> buffers;
    std::unordered_map<uint64_t, Texture> textures;
    std::unordered_map<uint64_t, Window> windows;
    std::unordered_map<uint64_t, Sync> syncs;
    std::unordered_map<uint64_t, CommandBuffer> command_buffers;
    // nvnCommandBufferEndRecording handles -> recorded lists.
    std::unordered_map<uint64_t, std::shared_ptr<CommandList>> command_handles;
    uint64_t next_handle = 1;
    // Handle table diagnostics: next size reported as a new high (doubles),
    // handles released because their memory was reused / finalized.
    size_t handle_report_at = 4096;
    uint64_t handles_reused = 0, handles_finalized = 0;
    TextureId next_texture_id = 1;
    std::atomic<int32_t> last_present_interval{-1};  // Hle::last_present_interval

    std::unordered_map<uint64_t, Program> programs;
    std::unordered_map<uint64_t, Sampler> samplers;
    std::unordered_map<uint64_t, TexturePool> texture_pools;
    // Texture pool slots by the NVNtexture registered in them. LGP finalizes
    // and initializes the same NVNtexture again (a larger version streamed in
    // after a capture) without registering it anew; the slots then follow the
    // texture rebuilt in place (TextureInitialize). Entries whose slot was
    // registered to another texture since are dropped when met.
    std::unordered_map<uint64_t, std::vector<std::pair<uint64_t, int32_t>>> texture_slots;
    std::unordered_map<uint64_t, SamplerPool> sampler_pools;
    ProgramId next_program_id = 1;
    // Translated programs by shader code (games set the same shaders on many
    // program objects); value = backend program id.
    std::unordered_map<std::string, Program> program_cache;
    StateTables states;

    // Command buffer `cmd` if it is recording, otherwise nullptr (logged).
    CommandBuffer* recording(uint64_t cmd, const char* fn);
    bool origin_lower_left(uint64_t device) const;
    bool depth_minus_w(uint64_t device) const;
};

// NVNtextureHandle -> texture and sampler through the given pools (procs_draw.cpp).
// Swizzle of an NVNtextureView (0 or no swizzle set: identity).
SwizzleRgba texture_view_swizzle(uint64_t view);
// Base level and base layer of an NVNtextureView (0 or not set: 0, 0).
void texture_view_base(uint64_t view, uint32_t* level, uint32_t* layer);
// Diagnosis: logs (Info, numbers only) views passed to RegisterTexture that
// set more than the swizzle (levels, layers, format, target, depth/stencil
// mode), which the HLE does not apply yet.
void note_registered_view(uint64_t view, const TextureDesc& desc, uint32_t texture_id, int32_t pool_id);
void resolve_texture_binding(Hle::Impl& h, uint64_t texture_pool, uint64_t sampler_pool, CmdBindTexture& cmd);
// LGP_NVN_PROJECTION_SCAN (procs_draw.cpp): looks for camera matrices in a
// uniform buffer, at bind time (lr = caller) and at submit.
bool projection_scan_on();
// Presents so far (procs.cpp), for diagnosis switches that start at a frame.
uint64_t presents_so_far();
// LGP_NVN_CBUF_FIND (procs_draw.cpp): looks for given float values in the
// uniform buffers the draws of a submitted list use.
bool cbuf_find_on();
void cbuf_find(Hle::Impl& h, const CommandList& list);
// LGP_NVN_DRAW_STATS_FRAME (procs_draw.cpp): per render target, how many draws
// of one frame see which camera matrix; logged at that frame's present.
bool draw_stats_on();
void draw_stats_note(Hle::Impl& h, const CommandList& list);
void draw_stats_flush();
void projection_scan(uint64_t lr, bool at_submit, uint64_t va, uint64_t size);

}  // namespace lgp::gpu::nvn
