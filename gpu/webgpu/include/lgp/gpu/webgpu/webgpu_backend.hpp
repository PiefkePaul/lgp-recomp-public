// NVN-HLE renderer on WebGPU (ADR-0003, roadmap P4.4).
//
// Executes what the NVN-HLE records (gpu/nvn/backend.hpp) with webgpu.h:
// NVN textures become WGPUTextures, command lists become render passes, a
// present draws the texture into the window or canvas through
// WebGpuRenderer. Same contract as the CPU reference (SoftBackend), so the
// same tests run against both.
//
// Every entry point is called from guest threads and hops to the GPU thread
// with WebGpuRenderer::run() (on the web: the browser's main thread). Each
// call blocks until the GPU thread has recorded and submitted the work, not
// until the GPU has finished it.
//
// Coordinates: WebGPU's framebuffer row 0 is the first row in memory. NVN's
// default window origin is lower left, so scissors (and later viewports)
// are flipped with the target's height when CommandList::origin_lower_left
// is set; present never flips (backend.hpp).
//
// Draw state (render targets, scissor, viewport, bindings, fixed-function
// state) starts from the defaults in every command list; whether NVN carries
// it over between command buffers is open (docs/knowledge/nvn-hle.md).
//
// Compute: every guest range bound as a storage buffer becomes one GPU
// buffer per command list (same address and size = same buffer, so
// dispatches in a list see each other's results), filled from guest memory
// when the list is recorded and written back to guest memory after the GPU
// has run it; execute() of a list with dispatches therefore waits for the
// GPU. Within one list, draws and copies read guest memory as it was before
// the list (vertex, index and uniform data are uploaded at record time), so
// they do not see what a dispatch in the same list wrote; overlapping
// storage ranges of different size are separate buffers.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "lgp/gpu/nvn/backend.hpp"
#include "lgp/platform/webgpu/webgpu_renderer.hpp"

namespace lgp::gpu::webgpu {

// NVN format -> WebGPU format the backend stores it in (Undefined = none).
WGPUTextureFormat texture_format(uint32_t nvn_format);

class WebGpuBackend final : public nvn::Backend {
public:
    // The renderer must be ok() and outlive the backend.
    explicit WebGpuBackend(platform::webgpu::WebGpuRenderer& renderer);
    ~WebGpuBackend() override;
    WebGpuBackend(const WebGpuBackend&) = delete;
    WebGpuBackend& operator=(const WebGpuBackend&) = delete;

    // Guest memory for vertex and constant buffer contents (draws) and
    // storage buffers (compute, written back); must outlive the backend.
    // Without it draws read zeros and compute results are lost. Replacing or
    // detaching it (nullptr) first waits for compute results still on their
    // way back into the old one (asynchronous write-back).
    void set_guest_memory(const nvn::GuestMemoryView* memory);

    // ASTC textures: native WebGPU ASTC formats when the device has
    // TextureCompressionASTC (mobile, Apple), otherwise decoded on the CPU to
    // RGBA8 (gpu/texture). true forces the CPU path for textures created
    // afterwards (tests; LGP_WEBGPU_DEBUG=cpuastc does the same).
    void force_cpu_astc(bool force) { force_cpu_astc_ = force; }
    // BCn (BC1-BC5, BC7) likewise: native with TextureCompressionBC (desktop,
    // Apple M1 and later), else decoded to RGBA8 on the CPU (Adreno, Mali;
    // BC6H stays unsupported). LGP_WEBGPU_DEBUG=cpubc forces it too.
    void force_cpu_bc(bool force) { force_cpu_bc_ = force; }
    // Pass diagnosis in Stats::pass_reasons ("target set: ...", "store: ..."):
    // per draw pass a record of its targets and sampled textures, compared
    // at the end of each list. Off by default (cost on the GPU thread); also
    // on with LGP_WEBGPU_DEBUG=passdiag, --time-stats prints the counts.
    void set_pass_diagnosis(bool on) { pass_diagnosis_.store(on, std::memory_order_relaxed); }
    // GPU memory for textures whose contents come from the memory pool:
    // above it, unused ones lose their GPU side until used again (0 = no
    // limit; LGP_WEBGPU_TEXTURE_BUDGET_MB).
    void set_texture_budget(uint64_t bytes);
    // Half-resolution effects: additive draws of the listed programs ("hash
    // prefix,..." in hex; empty keeps the list) render into a target of
    // half the width and height and are added onto theirs (softer effect,
    // less fill). Only draws where that gives the same sum up to the
    // resolution: one colour target, destination factor one, no depth or
    // stencil test, shader without the screen position. Off by default;
    // LGP_WEBGPU_HALFRES_PROGRAMS sets the list and turns it on.
    void set_halfres_effects(bool on, const std::string& programs = {});
    // Programs to watch ("hash prefix,..." in hex, empty: none; a prefix
    // matches the program hash or the guest code hash, ProgramDesc::guest_hash,
    // which survives translator changes): after a draw
    // of one of them take_watched_drawn() returns true once (any thread), so
    // the host can tell a scene of the game is on screen (CMake
    // LGP_TOUCH_ONLY_PROGRAMS: "Play with Pikachu" takes touches only).
    void watch_programs(const std::string& programs);
    bool take_watched_drawn();
    // Render scale (docs/analysis/37-render-skalierung-entwurf.md): render
    // targets live on the host at `scale` times their guest size (0.25..2,
    // 1 = off). Translated shaders carry the scale, so only before the first
    // program; later calls are ignored with a warning, as is any call while
    // LGP_WEBGPU_RENDER_SCALE is set (the environment wins).
    void set_render_scale(float scale);

    // Pipeline cache (lgp/gpu/webgpu/pipeline_cache.hpp): the draw pipelines
    // this backend creates are recorded in `path` (a .lgppipes file below
    // $LGP_PRIVATE_DIR or in OPFS, never in the repository), and those of
    // earlier runs are created ahead of use as soon as their program exists
    // again: on the desktop by a worker thread, in the browser with
    // createRenderPipelineAsync. The programs themselves (their WGSL) are
    // kept too and created ahead after each present, a few at a time, so the
    // game's create_program finds them ready. Call it before the first
    // program is created.
    // `seed`, if not empty: another .lgppipes file (e.g. from another device
    // or browser, whose fingerprint differs, or a catalog) whose records are
    // used beside those of `path` without being copied into it
    // (PipelineCacheFile::import_records); a seed that cannot be read is
    // logged and ignored.
    // false and *error: path refused or not writable, or LGP_WEBGPU_DEBUG
    // overrides pipeline state.
    bool set_pipeline_cache(const std::string& path, std::string* error = nullptr, const std::string& seed = {});
    // Blocks until the pipelines being created ahead are done (tests and
    // measurements; a guest thread, never the GPU thread). While held
    // (hold_prebuilds) it waits for the release.
    void wait_prebuilds();
    // Creates cached programs that are not there yet for up to `budget_us`
    // on the GPU thread (as the presents do a few at a time, or
    // set_pipeline_cache all at once with LGP_WEBGPU_WARM_ALL), so their
    // pipelines are queued: the app's graphics screen before the game calls
    // it in slices and waits with prebuild_progress(). Returns the number of
    // cached programs still to create (0: all done). Stops early while held
    // (hold_prebuilds). Any thread but the GPU thread.
    size_t warm_cached_programs(uint64_t budget_us);
    // Pipelines and programs created ahead so far, of all queued since the
    // cache was set (programs being checked count once, their pipelines
    // once each). done == total: nothing left to wait for (wait_prebuilds).
    struct PrebuildProgress {
        size_t done = 0, total = 0;
    };
    PrebuildProgress prebuild_progress() const;
    // App in the background (iOS ends apps that keep the CPU busy there):
    // the prebuild worker threads take no new job until released; a
    // pipeline being created finishes. Warming every cached program at once
    // (set_pipeline_cache with warm-all) stops after the current program;
    // the next present warms the rest. Applies to a pipeline cache set later
    // too. Returns the number of jobs still running. No effect on async
    // pipelines (browser), which the browser creates. Any thread.
    size_t hold_prebuilds(bool hold);

    // Compiles the program's WGSL; an invalid program is logged and counted
    // (Stats::invalid_programs), draws with it are skipped.
    void create_program(nvn::ProgramId id, const nvn::ProgramDesc& desc) override;
    void destroy_program(nvn::ProgramId id) override;
    void create_texture(nvn::TextureId id, const nvn::TextureDesc& desc) override;
    void destroy_texture(nvn::TextureId id) override;
    void write_texture(nvn::TextureId id, uint32_t level, const nvn::Rect& region, const void* data,
                       uint32_t row_stride, const nvn::TexelSlices& slices = {}) override;
    void execute(const nvn::CommandList& list) override;
    bool present(const nvn::PresentInfo& info) override;
    // Waits until compute results of submitted lists are back in guest memory.
    void finish() override;
    uint64_t fence_mark() override;
    void finish_until(uint64_t mark) override;

    struct Stats {
        uint64_t clears = 0;            // colour and depth/stencil clears executed
        uint64_t draws = 0;
        uint64_t discarded_draws = 0;  // draws under rasterizer discard (no fragments, not issued)
        uint64_t invalid_programs = 0;   // WGSL or layout rejected by WebGPU validation
        uint64_t invalid_pipelines = 0;  // draw pipelines rejected (their draws are skipped)
        uint64_t skipped_commands = 0;  // CmdUnsupported, clears without a usable target
        uint64_t presents = 0;
        uint64_t render_passes = 0;
        uint64_t copies = 0;            // buffer-to-texture copies in command lists
        uint64_t texture_copies = 0;    // texture-to-texture copies (CmdCopyTextureToTexture)
        uint64_t blits = 0;             // of those, scaled/mirrored/converting ones drawn as a blit
        uint64_t uploads = 0;           // write_texture calls
        uint64_t astc_cpu_decoded = 0;  // ASTC levels decoded on the CPU
        uint64_t astc_error_blocks = 0; // of their blocks: invalid/HDR (decoded as magenta)
        uint64_t bc_cpu_decoded = 0;    // BCn levels decoded on the CPU (no texture-compression-bc)
        uint64_t textures_evicted = 0;  // GPU sides dropped for the texture budget
        uint64_t astc_textures = 0;     // ASTC textures created (native or decoded on the CPU)
        uint64_t textures_3d = 0;       // 3D textures created
        uint64_t astc_bindings = 0;     // draw/dispatch bindings that sampled an ASTC texture
        uint64_t bindings_3d = 0;       // ... that sampled a 3D texture
        uint64_t unbound_textures = 0;  // texture bindings a draw found empty (sampled the dummy)
        uint64_t dispatches = 0;        // compute dispatches executed
        // Where a list's dispatches sit among its draws (Analyse 20, lever 1):
        // lists recorded, those with dispatches that write guest memory back
        // (skipped dispatches and ones without storage do not count), and the draw commands of the
        // latter before the first, between the first and the last, and after
        // the last dispatch.
        uint64_t lists = 0, dispatch_lists = 0;
        uint64_t list_commands = 0;  // commands in the lists recorded
        uint64_t draws_before_dispatch = 0, draws_between_dispatches = 0, draws_after_dispatch = 0;
        // Compute first (default, LGP_WEBGPU_COMPUTE_FIRST): dispatches submitted ahead of their
        // list and those kept in order (they depend on earlier GPU work of
        // the list); lists with a compute submit ahead, and of those the
        // ones whose results were read back from it.
        uint64_t dispatches_first = 0, dispatches_in_order = 0;
        uint64_t lists_compute_first = 0, lists_written_back_first = 0;
        uint64_t compute_vertex_streams = 0;  // vertex streams read from a dispatch's output in the same list
        uint64_t compute_uniform_slots = 0;   // constant buffers a dispatch reaches through global memory
        // Lists whose dispatches wrote guest memory. By default execute()
        // waits for their results: each counts as a wait, from the submit
        // until they are in guest memory. With LGP_WEBGPU_ASYNC_WRITEBACK=1
        // the results go back after the call returned; waits are then later
        // calls that found results still in flight, with the time they waited.
        uint64_t writebacks = 0, writeback_waits = 0, writeback_wait_us = 0;
        // finish() calls (nvnQueueFinish, nvnSyncWait) and those that found write-backs pending.
        uint64_t finish_calls = 0, finish_pending = 0;
        // Upload and binding work (bench/webgpu): constant buffer and
        // vertex/index uploads written to the GPU and their bytes, bindings
        // served by an upload already made in the same list; bind groups
        // created and served from the cache.
        uint64_t cbuf_uploads = 0, cbuf_upload_bytes = 0, cbuf_reused = 0;
        uint64_t geometry_uploads = 0, geometry_upload_bytes = 0, geometry_reused = 0;
        // Vertex/index bindings served across lists by the geometry cache
        // (guest bytes unchanged since the buffer was written; no upload).
        uint64_t geometry_cached = 0;
        uint64_t bind_groups_created = 0, bind_groups_reused = 0;
        // Shader and pipeline creation (first use of an effect stalls on
        // these): programs created (shader modules, layouts, compute
        // pipeline) and draw pipelines created, with the host time spent.
        uint64_t programs_created = 0, program_create_us = 0;
        uint64_t draw_pipelines_created = 0, draw_pipeline_create_us = 0;
        uint64_t texture_upload_us = 0;  // checking and uploading textures from their pools
        // Parts of it. Block-linear textures: reading guest memory and the
        // fingerprint (texture_checks, also those that find the contents
        // unchanged), deswizzling and converting (ASTC included), the
        // writeTexture calls. Pitch-linear textures (checked every frame):
        // all of it. The rest is bookkeeping.
        uint64_t texture_read_us = 0, texture_read_bytes = 0, texture_checks = 0;
        uint64_t texture_convert_us = 0;
        uint64_t texture_write_us = 0, texture_write_bytes = 0, texture_writes = 0;
        uint64_t texture_linear_us = 0, texture_linear_bytes = 0, texture_linear_checks = 0;
        // Pipeline cache: recipes read at start, draw pipelines created ahead
        // of use and then needed, prebuilds that failed, worker time. Needed
        // before their prebuild was done: waited for (worker busy with it)
        // or late (not started, or browser: still compiling), then created
        // on use as without the cache.
        uint64_t pipeline_cache_records = 0;
        uint64_t pipeline_cache_seeded = 0;  // records of them new from the seed file
        uint64_t pipelines_prebuilt = 0, prebuilds_failed = 0, prebuild_us = 0;
        uint64_t prebuilds_waited = 0, prebuilds_late = 0;
        // LGP_WEBGPU_PIPELINE_NOWAIT: draws skipped while their pipeline compiled.
        uint64_t draws_skipped_compiling = 0;
        // Programs in the cache, created ahead (a few after each present),
        // and taken over by create_program instead of compiling.
        uint64_t pipeline_cache_programs = 0, programs_warmed = 0, programs_adopted = 0;
        // Bytes of the driver's pipeline cache from earlier runs (wgpu-native
        // with tools/webgpu/wgpu-native-pipeline-cache.patch); -1 without one.
        int64_t driver_cache_loaded = -1;
        // Compute pipelines of programs created ahead, built in the
        // background and there before (or at) their first dispatch.
        uint64_t compute_pipelines_prebuilt = 0;
        // Async pipelines (browser): dispatches whose compute pipeline was
        // still compiling and the time their lists waited for it (lists that
        // also created draw pipelines count in draw_pipeline_create_us).
        uint64_t compute_pipeline_waits = 0, compute_pipeline_wait_us = 0;
        // Backpressure at present (LGP_WEBGPU_FRAMES_IN_FLIGHT): frames
        // submitted and not yet done on the GPU now and at most, presents
        // that waited for an earlier frame and the time they waited.
        uint64_t frames_in_flight = 0, frames_in_flight_max = 0, frame_waits = 0, frame_wait_us = 0;
        // Host time inside wgpuQueueSubmit for the lists (compute part and
        // main part) and in warm_step at present (pipeline cache warm-up):
        // parts of the GPU thread's busy time that are not encoding.
        uint64_t list_submits = 0, list_submit_ns = 0, warm_steps = 0, warm_step_ns = 0;
        // GPU objects and queue writes, process-wide (all backends): live
        // buffer bytes = created - destroyed (also buffers only released,
        // which a browser frees only on garbage collection).
        uint64_t buffers_created = 0, buffers_destroyed = 0, buffer_bytes_created = 0, buffer_bytes_destroyed = 0;
        uint64_t textures_created = 0, textures_destroyed = 0;
        // Estimated (format x size x levels x layers; drivers pad and align).
        uint64_t texture_bytes_created = 0, texture_bytes_destroyed = 0;
        uint64_t queue_writes = 0, queue_write_bytes = 0;
        // LGP_WEBGPU_BUFFER_POOL=1: per-list buffers taken from the pool,
        // created for it, and bytes it holds unused (in buffer_bytes too).
        uint64_t buffer_pool_reused = 0, buffer_pool_created = 0, buffer_pool_free_bytes = 0;
        // Held for the device's life: distinct samplers, and colour targets
        // whose last depth target is noted (released with the texture).
        uint64_t samplers = 0, depth_partners = 0;
        // LGP_WEBGPU_DEBUG=gputime: GPU execution time from timestamp queries
        // around every pass (counted when the results are back, a frame or so
        // later), per compute program (key: program hash) and for all render
        // passes; passes beyond the per-list limit are not timed.
        struct GpuTime {
            uint64_t passes = 0, ns = 0;
        };
        uint64_t gpu_time_passes = 0, gpu_time_untimed = 0;
        GpuTime gpu_time_compute, gpu_time_render;
        std::map<uint64_t, GpuTime> gpu_time_programs;
        // Render passes by kind and target, as the passlabels names
        // ("draw 1280x720 c12 d5", "clear 640x360 c7", "blit …").
        std::map<std::string, GpuTime> gpu_time_targets;
        // Draw passes' time split over their graphics programs by draw count
        // (key: program hash; passes = draws). An estimate for a ranking: a
        // full-screen draw and a small one get the same share.
        std::map<uint64_t, GpuTime> gpu_time_draw_programs;
        nvn::TextureId last_presented = 0;
        nvn::Rect last_crop;  // memory rows; width/height 0 = whole texture
        bool last_flip_y = false;  // the last present showed the rows bottom to top
        // Why commands were skipped (diagnostics), reason -> count.
        std::map<std::string, uint64_t> skip_reasons;
        // Diagnostic counters of LGP_WEBGPU_DEBUG options (computeoverlap), name -> count.
        std::map<std::string, uint64_t> diagnostics;
        // Render passes by cause, cause -> count: why a draw pass ended
        // ("draw pass ended: ...") and passes of their own ("own pass: ...").
        std::map<std::string, uint64_t> pass_reasons;
        uint64_t draw_passes = 0;  // of render_passes: passes opened for draws
        // Render passes whose set of attachment views was not seen before
        // (an estimate of the framebuffers the Vulkan backend creates; view
        // handles reused after a release count as seen).
        uint64_t attachment_sets_new = 0;
    };
    Stats stats() const;
    // The queue-submit and warm-up times of Stats without copying it
    // (time sources of --time-stats, read every window).
    void submit_times(uint64_t* list_submits, uint64_t* list_submit_ns, uint64_t* warm_steps,
                      uint64_t* warm_step_ns) const;
    // Highest live buffer and texture bytes (as in Stats) and most commands
    // in one list since the previous call (process-wide); a window's peak
    // also sees per-list buffers created and destroyed between two calls.
    struct WindowPeaks {
        uint64_t buffer_bytes = 0, texture_bytes = 0, list_commands = 0;
    };
    WindowPeaks take_window_peaks();

    // Level 0, layer 0 of a 4-byte-per-texel colour texture, row 0 = first
    // row in memory, tightly packed. Waits for the GPU; tests and debugging.
    bool read_texture(nvn::TextureId id, std::vector<uint8_t>* out, uint32_t* width, uint32_t* height);
    // The last presented picture as the display shows it: crop applied and,
    // for the lower-left window origin, rows bottom to top.
    bool read_presented(std::vector<uint8_t>* out, uint32_t* width, uint32_t* height);
    // Debugging: textures the GPU has rendered or copied into, with size and NVN format.
    struct RenderedTexture {
        nvn::TextureId id;
        uint32_t width, height, format;
    };
    std::vector<RenderedTexture> rendered_textures();

    struct State;
    struct Program;

private:
    void destroy_program_locked(nvn::ProgramId id);
    // GPU thread; calls done() when the list has been submitted and its compute
    // results (if any) are back in guest memory.
    // wait_for: the submission a synchronous write-back waits on
    // (LGP_WEBGPU_POLL_SUBMIT_INDEX, wgpu-native).
    void record_list(const nvn::CommandList& list, Stats& delta, std::function<void()> done,
                     std::function<void(uint64_t)> wait_for = nullptr);

    platform::webgpu::WebGpuRenderer& renderer_;
    std::unique_ptr<State> state_;  // GPU thread only
    const nvn::GuestMemoryView* memory_ = nullptr;
    bool force_cpu_astc_ = false;
    bool force_cpu_bc_ = false;
    std::atomic<bool> pass_diagnosis_{false};
    std::atomic<bool> watched_drawn_{false};  // watch_programs
    mutable std::mutex stats_mutex_;
    Stats stats_;
    // hold_prebuilds; guards state_->prebuilds once workers may exist.
    std::mutex hold_mutex_;
    bool prebuilds_held_ = false;
};

}  // namespace lgp::gpu::webgpu
