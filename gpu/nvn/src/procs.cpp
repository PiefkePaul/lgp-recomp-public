// The NVN functions the HLE implements (P4.1: the path to a visible clear).
//
// Signatures are those of the public NVN C API as used by games (function
// names are what the proc loader asks for; argument order as remembered from
// public homebrew/modding code). Each one is UNVERIFIED until checked
// against the driver implementation in the game's own sdk module or a
// trace of the real game (docs/knowledge/nvn-hle.md, checklist ("Prüfliste")).
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <iterator>
#include <mutex>
#include <set>
#include <tuple>
#include <utility>

#include "guest_access.hpp"
#include "hle_impl.hpp"
#include "lgp/runtime/log.hpp"
#include "lgp/runtime/system.hpp"

namespace lgp::gpu::nvn {

namespace {

// wait_stats(): process-wide, relaxed (one reader per --time-stats window).
struct WaitCounters {
    std::atomic<uint64_t> finish_calls{0}, finish_ns{0}, sync_calls{0}, sync_ns{0};
};
WaitCounters g_waits;

uint64_t now_ns() {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count());
}

// The first callers of nvnQueueFinish (x30), once each: where a game waits
// for the GPU in every frame (docs/analysis/49).
void note_finish_caller(uint64_t lr) {
    static std::mutex mutex;
    static std::set<uint64_t> seen;
    std::lock_guard lock(mutex);
    if (seen.size() >= 8 || !seen.insert(lr).second)
        return;
    rt::log(rt::LogLevel::Info, "nvn: nvnQueueFinish called from 0x%" PRIx64, lr);
}

using rt::LogLevel;
using H = Hle::Impl;
using guest::bool_arg;
using guest::float_arg;
using guest::int_arg;
using guest::ptr_arg;
using guest::uint_arg;

void ret(CpuState* c, uint64_t v) { c->x[0] = v; }
void ret_bool(CpuState* c, bool v) { c->x[0] = v ? 1 : 0; }

// ---------------------------------------------------------------------------
// Inline state of builders and TextureView in their guest storage (abi.hpp).
// The magic tells "SetDefaults ran" from uninitialised memory.

constexpr uint32_t make_magic(char a, char b) {
    return 0x4C470000u | (static_cast<uint32_t>(static_cast<uint8_t>(a)) << 8) | static_cast<uint8_t>(b);
}

struct DeviceBuilderState {
    static constexpr uint32_t kMagic = make_magic('D', 'B');
    static constexpr size_t kStorage = kDeviceBuilderSize;
    uint32_t magic = kMagic;
    uint32_t flags = 0;
};

struct QueueBuilderState {
    static constexpr uint32_t kMagic = make_magic('Q', 'B');
    static constexpr size_t kStorage = kQueueBuilderSize;
    uint32_t magic = kMagic;
    uint32_t flags = 0;
    uint64_t device = 0;
    int32_t compute_memory_size = 0;
    int32_t reserved = 0;
};

struct MemoryPoolBuilderState {
    static constexpr uint32_t kMagic = make_magic('M', 'B');
    static constexpr size_t kStorage = kMemoryPoolBuilderSize;
    uint32_t magic = kMagic;
    uint32_t flags = 0;
    uint64_t device = 0;
    uint64_t storage = 0;
    uint64_t size = 0;
};

struct BufferBuilderState {
    static constexpr uint32_t kMagic = make_magic('B', 'B');
    static constexpr size_t kStorage = kBufferBuilderSize;
    uint32_t magic = kMagic;
    uint32_t reserved = 0;
    uint64_t device = 0;
    uint64_t pool = 0;
    uint64_t offset = 0;
    uint64_t size = 0;
};

struct TextureBuilderState {
    static constexpr uint32_t kMagic = make_magic('T', 'B');
    static constexpr size_t kStorage = kTextureBuilderSize;
    uint32_t magic = kMagic;
    uint32_t flags = 0;
    uint32_t target = static_cast<uint32_t>(TextureTarget::Target2D);
    uint32_t format = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t depth = 1;
    uint32_t levels = 1;
    uint32_t samples = 0;
    uint32_t reserved = 0;
    uint64_t stride = 0;
    uint64_t device = 0;
    uint64_t pool = 0;
    uint64_t offset = 0;
    uint64_t packaged_layout = 0;
};

struct TextureViewState {
    static constexpr uint32_t kMagic = make_magic('T', 'V');
    static constexpr size_t kStorage = kTextureViewSize;
    enum : uint32_t { kLevels = 1, kLayers = 2, kFormat = 4, kTarget = 8, kDepthStencil = 16, kSwizzle = 32 };
    uint32_t magic = kMagic;
    uint32_t set = 0;  // which fields were set (the getters report it)
    uint32_t format = 0;
    uint32_t target = 0;
    uint32_t depth_stencil_mode = 0;
    uint16_t base_level = 0, num_levels = 0, base_layer = 0, num_layers = 0;
    uint8_t swizzle[4] = {};
};

struct WindowBuilderState {
    static constexpr uint32_t kMagic = make_magic('W', 'B');
    static constexpr size_t kStorage = kWindowBuilderSize;
    static constexpr uint32_t kMaxTextures = 4;
    uint32_t magic = kMagic;
    int32_t present_interval = 1;
    uint64_t device = 0;
    uint64_t native_window = 0;
    uint32_t num_textures = 0;
    uint32_t reserved = 0;
    uint64_t textures[kMaxTextures] = {};
};

template <typename S>
constexpr bool fits() {
    return sizeof(S) <= S::kStorage && std::is_trivially_copyable_v<S>;
}
static_assert(fits<DeviceBuilderState>() && fits<QueueBuilderState>() && fits<MemoryPoolBuilderState>() &&
              fits<BufferBuilderState>() && fits<TextureBuilderState>() && fits<TextureViewState>() &&
              fits<WindowBuilderState>());

// Reads the inline state at va. Missing magic (game skipped SetDefaults):
// start from defaults and say so once per type.
template <typename S>
S load_state(uint64_t va, const char* fn) {
    S s = guest::load<S>(va);
    if (s.magic != S::kMagic) {
        static std::atomic<bool> warned{false};
        if (!warned.exchange(true))
            rt::log(LogLevel::Warn, "nvn: %s on object 0x%" PRIx64 " without SetDefaults, using defaults", fn, va);
        s = S{};
    }
    return s;
}

template <typename S>
void store_state(uint64_t va, const S& s) {
    guest::store(va, s);
}

// Read-modify-write helper for setters: `update<S>(va, fn, [](S& s) {...})`.
template <typename S, typename F>
void update(uint64_t va, const char* fn, F&& f) {
    S s = load_state<S>(va, fn);
    f(s);
    store_state(va, s);
}

template <typename Map>
typename Map::mapped_type* find(Map& map, uint64_t va, const char* fn) {
    auto it = map.find(va);
    if (it == map.end()) {
        rt::log(LogLevel::Warn, "nvn: %s on unknown object 0x%" PRIx64, fn, va);
        return nullptr;
    }
    return &it->second;
}

// ---------------------------------------------------------------------------
// Bootstrap

void BootstrapLoader(H& h, CpuState* c);  // defined after the table
void DeviceGetProcAddress(H& h, CpuState* c);

// ---------------------------------------------------------------------------
// Device

void DeviceBuilderSetDefaults(H&, CpuState* c) { store_state(ptr_arg(c, 0), DeviceBuilderState{}); }
void DeviceBuilderSetFlags(H&, CpuState* c) {
    update<DeviceBuilderState>(ptr_arg(c, 0), __func__, [&](auto& s) { s.flags = uint_arg(c, 1); });
}

void DeviceInitialize(H& h, CpuState* c) {
    const uint64_t dev = ptr_arg(c, 0);
    const auto b = load_state<DeviceBuilderState>(ptr_arg(c, 1), __func__);
    H::Device d;
    d.flags = b.flags;
    h.devices[dev] = d;
    rt::log(LogLevel::Info, "nvn: device 0x%" PRIx64 " initialized (flags 0x%x, depth mode NEAR_IS_MINUS_W until set)",
            dev, b.flags);
    ret_bool(c, true);
}
void DeviceFinalize(H& h, CpuState* c) { h.devices.erase(ptr_arg(c, 0)); }

void DeviceGetInteger(H& h, CpuState* c) {
    const uint32_t key = uint_arg(c, 1);
    int32_t value = 0;
    auto it = h.device_integers.find(key);
    if (it != h.device_integers.end())
        value = it->second;
    if (h.logged_device_keys.insert(key).second)
        rt::log(it != h.device_integers.end() ? LogLevel::Info : LogLevel::Warn,
                "nvn: nvnDeviceGetInteger(%u) -> %d%s", key, value,
                it != h.device_integers.end() ? "" : " (unknown key)");
    guest::store<int32_t>(ptr_arg(c, 2), value);
}

void DeviceGetTextureHandle(H&, CpuState* c) { ret(c, texture_handle(uint_arg(c, 1), uint_arg(c, 2))); }
void DeviceGetTexelFetchHandle(H&, CpuState* c) { ret(c, texture_handle(uint_arg(c, 1), 0)); }

void DeviceFinalizeCommandHandle(H& h, CpuState* c) { h.handles_finalized += h.command_handles.erase(ptr_arg(c, 1)); }

void DeviceGetDepthMode(H& h, CpuState* c) {
    auto* d = find(h.devices, ptr_arg(c, 0), __func__);
    ret(c, d ? d->depth_mode : 0);
}
void DeviceSetDepthMode(H& h, CpuState* c) {
    if (auto* d = find(h.devices, ptr_arg(c, 0), __func__)) {
        d->depth_mode = uint_arg(c, 1);
        rt::log(LogLevel::Info, "nvn: depth mode %s", d->depth_mode == uint32_t(DepthMode::NearIsZero)
                                                         ? "NEAR_IS_ZERO"
                                                         : "NEAR_IS_MINUS_W (clip z mapped to [0, w])");
    }
}
void DeviceGetWindowOriginMode(H& h, CpuState* c) {
    auto* d = find(h.devices, ptr_arg(c, 0), __func__);
    ret(c, d ? d->origin_mode : static_cast<uint32_t>(WindowOriginMode::LowerLeft));
}
void DeviceSetWindowOriginMode(H& h, CpuState* c) {
    if (auto* d = find(h.devices, ptr_arg(c, 0), __func__))
        d->origin_mode = uint_arg(c, 1);
}

// ---------------------------------------------------------------------------
// Sync. A fence counts as signalled as soon as it is placed; waiting on it
// lets the backend finish what was still in flight when it was placed
// (lists still queued with AsyncBackend, compute results written back;
// Backend::finish_until).

void SyncInitialize(H& h, CpuState* c) {
    h.syncs[ptr_arg(c, 0)] = {};
    ret_bool(c, true);
}
void SyncFinalize(H& h, CpuState* c) { h.syncs.erase(ptr_arg(c, 0)); }
void SyncWait(H& h, CpuState* c) {
    auto* s = find(h.syncs, ptr_arg(c, 0), __func__);
    if (s != nullptr && s->signaled) {
        const uint64_t t0 = now_ns();
        h.backend.finish_until(s->mark);
        g_waits.sync_calls.fetch_add(1, std::memory_order_relaxed);
        g_waits.sync_ns.fetch_add(now_ns() - t0, std::memory_order_relaxed);
        ret(c, static_cast<uint32_t>(SyncWaitResult::AlreadySignaled));
        return;
    }
    // Never fenced: on hardware this waits for the timeout (or forever).
    rt::log(LogLevel::Warn, "nvn: nvnSyncWait on a sync that was never fenced (timeout %" PRIu64 ")", ptr_arg(c, 1));
    ret(c, static_cast<uint32_t>(s ? SyncWaitResult::TimeoutExpired : SyncWaitResult::Failed));
}

// ---------------------------------------------------------------------------
// Queue

void QueueBuilderSetDefaults(H&, CpuState* c) { store_state(ptr_arg(c, 0), QueueBuilderState{}); }
void QueueBuilderSetDevice(H&, CpuState* c) {
    update<QueueBuilderState>(ptr_arg(c, 0), __func__, [&](auto& s) { s.device = ptr_arg(c, 1); });
}
void QueueBuilderSetComputeMemorySize(H&, CpuState* c) {
    update<QueueBuilderState>(ptr_arg(c, 0), __func__, [&](auto& s) { s.compute_memory_size = int_arg(c, 1); });
}

void QueueInitialize(H& h, CpuState* c) {
    const auto b = load_state<QueueBuilderState>(ptr_arg(c, 1), __func__);
    h.queues[ptr_arg(c, 0)] = {b.device};
    ret_bool(c, true);
}
void QueueFinalize(H& h, CpuState* c) { h.queues.erase(ptr_arg(c, 0)); }

// ---------------------------------------------------------------------------
// Statistics counters (nvnCommandBufferReportCounter/ResetCounter). The GPU
// counts in hardware; the HLE estimates the counts from the draws of the
// submitted lists, in command order, and writes NVNcounterData when the list
// has been handed to the backend:
// - TIMESTAMP, TIMESTAMP_TOP: counter 0, timestamp = host time at submit in
//   GPU timestamp units (abi.hpp). GPU time measured by the game is therefore
//   ~0 (the backend's real GPU time is not measured).
// - INPUT_VERTICES, VERTEX_SHADER_INVOCATIONS: vertices (indices) * instances.
// - INPUT_PRIMITIVES, PRIMITIVES_GENERATED, CLIPPER_INPUT_PRIMITIVES: the
//   primitives these assemble to; CLIPPER_OUTPUT_PRIMITIVES the same unless
//   rasterizer discard is on (no clipping or culling is modelled).
// - SAMPLES_PASSED, FRAGMENT_SHADER_INVOCATIONS: no occlusion data; each
//   draw that rasterizes adds its primitive count, so "anything visible since
//   the reset" tests see the draw as visible and a pass without draws as 0.
// - Tessellation, geometry and transform feedback counters stay 0.

uint64_t primitives(Topology t, uint64_t vertices) {
    switch (t) {
    case Topology::Points: return vertices;
    case Topology::Lines: return vertices / 2;
    case Topology::LineStrip: return vertices >= 2 ? vertices - 1 : 0;
    case Topology::Triangles: return vertices / 3;
    case Topology::TriangleStrip: return vertices >= 3 ? vertices - 2 : 0;
    }
    return 0;
}

uint64_t gpu_timestamp() {
    // system_tick() runs at 19.2 MHz: ns = ticks * 625 / 12, timestamp = ns * 384 / 625 = ticks * 32.
    static_assert(rt::kCounterFrequency == 19'200'000 && kGpuTimestampNumerator == 384 &&
                  kGpuTimestampDenominator == 625);
    return rt::system_tick() * 32;
}

void count_draw(H::Queue& q, Topology t, uint64_t vertices, uint64_t instances) {
    using T = CounterTypeNvn;
    const uint64_t v = vertices * instances, p = primitives(t, vertices) * instances;
    q.counters[uint32_t(T::InputVertices)] += v;
    q.counters[uint32_t(T::VertexShaderInvocations)] += v;
    q.counters[uint32_t(T::InputPrimitives)] += p;
    q.counters[uint32_t(T::PrimitivesGenerated)] += p;
    q.counters[uint32_t(T::ClipperInputPrimitives)] += p;
    if (!q.rasterizer_discard) {
        q.counters[uint32_t(T::ClipperOutputPrimitives)] += p;
        q.counters[uint32_t(T::SamplesPassed)] += p;
        q.counters[uint32_t(T::FragmentShaderInvocations)] += p;
    }
}

struct CounterWrite {
    uint64_t address;
    uint64_t data[2];
};

// Runs the list's counter commands on the queue's counters; the reports to
// write once the list has been executed.
std::vector<CounterWrite> submit_counters(H::Queue& q, const CommandList& list) {
    std::vector<CounterWrite> writes;
    for (const Command& cmd : list.commands) {
        if (auto* d = std::get_if<CmdSetRasterizerDiscard>(&cmd)) {
            q.rasterizer_discard = d->enable;
        } else if (auto* d = std::get_if<CmdDraw>(&cmd)) {
            count_draw(q, d->topology, d->vertex_count, d->instance_count);
        } else if (auto* d = std::get_if<CmdDrawIndexed>(&cmd)) {
            count_draw(q, d->topology, d->index_count, d->instance_count);
        } else if (auto* r = std::get_if<CmdResetCounter>(&cmd)) {
            if (r->counter < std::size(q.counters))
                q.counters[r->counter] = 0;
        } else if (auto* r = std::get_if<CmdReportCounter>(&cmd)) {
            const auto type = static_cast<CounterTypeNvn>(r->counter);
            if (type == CounterTypeNvn::Timestamp || type == CounterTypeNvn::TimestampTop) {
                writes.push_back({r->address, {0, gpu_timestamp()}});
            } else if (r->counter < std::size(q.counters) && type != CounterTypeNvn::ZcullStats) {
                writes.push_back({r->address, {q.counters[r->counter], gpu_timestamp()}});
            } else {
                static std::atomic<bool> warned{false};
                if (!warned.exchange(true))
                    rt::log(LogLevel::Warn, "nvn: nvnCommandBufferReportCounter(type %u) not supported, nothing written",
                            r->counter);
            }
        }
    }
    return writes;
}

// Texture handles resolve against the pools bound when the list runs (GPU
// state that carries over from earlier submits on the queue), so the list
// is executed as a copy with CmdBindTexture filled in.
void QueueSubmitCommands(H& h, CpuState* c) {
    H::Queue* q = find(h.queues, ptr_arg(c, 0), __func__);
    H::Queue fallback;
    if (q == nullptr)
        q = &fallback;
    const int32_t n = int_arg(c, 1);
    const uint64_t handles = ptr_arg(c, 2);
    for (int32_t i = 0; i < n; ++i) {
        const uint64_t handle = guest::load<uint64_t>(handles + uint64_t(i) * 8);
        auto it = h.command_handles.find(handle);
        if (it == h.command_handles.end()) {
            rt::log(LogLevel::Warn, "nvn: nvnQueueSubmitCommands: unknown command handle 0x%" PRIx64, handle);
            continue;
        }
        CommandList list = *it->second;
        for (Command& cmd : list.commands) {
            if (projection_scan_on())
                if (auto* u = std::get_if<CmdBindUniformBuffer>(&cmd))
                    projection_scan(0, true, u->address, u->size);
            if (auto* p = std::get_if<CmdSetTexturePools>(&cmd)) {
                if (p->texture_pool != 0) q->texture_pool = p->texture_pool;
                if (p->sampler_pool != 0) q->sampler_pool = p->sampler_pool;
            } else if (auto* b = std::get_if<CmdBindTexture>(&cmd)) {
                resolve_texture_binding(h, q->texture_pool, q->sampler_pool, *b);
            }
        }
        if (cbuf_find_on())
            cbuf_find(h, list);
        if (draw_stats_on())
            draw_stats_note(h, list);
        std::vector<CounterWrite> writes = submit_counters(*q, list);
        h.backend.execute_owned(std::move(list));
        // The reports land once the list has run, as on the GPU (with
        // AsyncBackend later, on its worker). A bad address faults here, on
        // the guest thread; the write itself then cannot fail.
        std::erase_if(writes, [](const CounterWrite& w) {
            if (guest::accessible(w.address, kCounterDataSize, true))
                return false;
            guest::FaultScope::record(w.address);
            return true;
        });
        if (!writes.empty())
            h.backend.after_queued([writes = std::move(writes)] {
                for (const CounterWrite& w : writes)
                    guest::write(w.address, w.data, kCounterDataSize);
            });
    }
}
void QueueFlush(H&, CpuState*) {}
void QueueFinish(H& h, CpuState* c) {
    note_finish_caller(c->x[30]);
    const uint64_t t0 = now_ns();
    h.backend.finish();
    g_waits.finish_calls.fetch_add(1, std::memory_order_relaxed);
    g_waits.finish_ns.fetch_add(now_ns() - t0, std::memory_order_relaxed);
}
void QueueFenceSync(H& h, CpuState* c) {
    if (auto* s = find(h.syncs, ptr_arg(c, 1), __func__)) {
        s->signaled = true;
        s->mark = h.backend.fence_mark();
    }
}
void QueueWaitSync(H&, CpuState* c) { ret_bool(c, true); }
void QueueGetError(H&, CpuState* c) { ret(c, kQueueGetErrorResultGpuNoError); }

// ---------------------------------------------------------------------------
// Memory pools and buffers. GPU address = guest VA of the storage.

void MemoryPoolBuilderSetDefaults(H&, CpuState* c) { store_state(ptr_arg(c, 0), MemoryPoolBuilderState{}); }
void MemoryPoolBuilderSetDevice(H&, CpuState* c) {
    update<MemoryPoolBuilderState>(ptr_arg(c, 0), __func__, [&](auto& s) { s.device = ptr_arg(c, 1); });
}
void MemoryPoolBuilderSetFlags(H&, CpuState* c) {
    update<MemoryPoolBuilderState>(ptr_arg(c, 0), __func__, [&](auto& s) { s.flags = uint_arg(c, 1); });
}
void MemoryPoolBuilderSetStorage(H&, CpuState* c) {
    update<MemoryPoolBuilderState>(ptr_arg(c, 0), __func__, [&](auto& s) {
        s.storage = ptr_arg(c, 1);
        s.size = ptr_arg(c, 2);
    });
}

void MemoryPoolInitialize(H& h, CpuState* c) {
    const auto b = load_state<MemoryPoolBuilderState>(ptr_arg(c, 1), __func__);
    if (b.storage == 0 || !guest::accessible(b.storage, b.size, false)) {
        rt::log(LogLevel::Error, "nvn: nvnMemoryPoolInitialize: storage 0x%" PRIx64 "+0x%" PRIx64 " not accessible",
                b.storage, b.size);
        ret_bool(c, false);
        return;
    }
    h.pools[ptr_arg(c, 0)] = {b.device, b.storage, b.size, b.flags};
    ret_bool(c, true);
}
void MemoryPoolFinalize(H& h, CpuState* c) { h.pools.erase(ptr_arg(c, 0)); }
void MemoryPoolMap(H& h, CpuState* c) {
    auto* p = find(h.pools, ptr_arg(c, 0), __func__);
    ret(c, p ? p->storage : 0);
}
void MemoryPoolGetBufferAddress(H& h, CpuState* c) {
    auto* p = find(h.pools, ptr_arg(c, 0), __func__);
    ret(c, p ? p->storage : 0);
}
void MemoryPoolGetFlags(H& h, CpuState* c) {
    auto* p = find(h.pools, ptr_arg(c, 0), __func__);
    ret(c, p ? p->flags : 0);
}
// CPU and "GPU" share the guest memory; nothing to flush yet. The WebGPU
// backend will use these as upload hints (ADR-0003, risk 4).
void MemoryPoolFlushMappedRange(H&, CpuState*) {}
void MemoryPoolInvalidateMappedRange(H&, CpuState*) {}
void MemoryPoolSetDebugLabel(H&, CpuState*) {}

void BufferBuilderSetDefaults(H&, CpuState* c) { store_state(ptr_arg(c, 0), BufferBuilderState{}); }
void BufferBuilderSetDevice(H&, CpuState* c) {
    update<BufferBuilderState>(ptr_arg(c, 0), __func__, [&](auto& s) { s.device = ptr_arg(c, 1); });
}
void BufferBuilderSetStorage(H&, CpuState* c) {
    update<BufferBuilderState>(ptr_arg(c, 0), __func__, [&](auto& s) {
        s.pool = ptr_arg(c, 1);
        s.offset = ptr_arg(c, 2);
        s.size = ptr_arg(c, 3);
    });
}
void BufferInitialize(H& h, CpuState* c) {
    const auto b = load_state<BufferBuilderState>(ptr_arg(c, 1), __func__);
    auto* pool = find(h.pools, b.pool, __func__);
    if (pool == nullptr || b.offset > pool->size || b.size > pool->size - b.offset) {
        rt::log(LogLevel::Error, "nvn: nvnBufferInitialize: range 0x%" PRIx64 "+0x%" PRIx64 " outside pool 0x%" PRIx64,
                b.offset, b.size, b.pool);
        ret_bool(c, false);
        return;
    }
    h.buffers[ptr_arg(c, 0)] = {b.pool, pool->storage + b.offset, b.size};
    ret_bool(c, true);
}
void BufferFinalize(H& h, CpuState* c) { h.buffers.erase(ptr_arg(c, 0)); }
void BufferMap(H& h, CpuState* c) {
    auto* b = find(h.buffers, ptr_arg(c, 0), __func__);
    ret(c, b ? b->address : 0);
}
void BufferGetAddress(H& h, CpuState* c) {
    auto* b = find(h.buffers, ptr_arg(c, 0), __func__);
    ret(c, b ? b->address : 0);
}
void BufferFlushMappedRange(H&, CpuState*) {}
void BufferInvalidateMappedRange(H&, CpuState*) {}
void BufferSetDebugLabel(H&, CpuState*) {}

// ---------------------------------------------------------------------------
// Textures

void TextureBuilderSetDefaults(H&, CpuState* c) { store_state(ptr_arg(c, 0), TextureBuilderState{}); }
#define LGP_TB_SETTER(Name, field, expr)                                                                   \
    void TextureBuilderSet##Name(H&, CpuState* c) {                                                       \
        update<TextureBuilderState>(ptr_arg(c, 0), "nvnTextureBuilderSet" #Name, [&](auto& s) { s.field = (expr); }); \
    }
LGP_TB_SETTER(Device, device, ptr_arg(c, 1))
LGP_TB_SETTER(Flags, flags, uint_arg(c, 1))
LGP_TB_SETTER(Target, target, uint_arg(c, 1))
LGP_TB_SETTER(Width, width, uint_arg(c, 1))
LGP_TB_SETTER(Height, height, uint_arg(c, 1))
LGP_TB_SETTER(Depth, depth, uint_arg(c, 1))
LGP_TB_SETTER(Levels, levels, uint_arg(c, 1))
LGP_TB_SETTER(Format, format, uint_arg(c, 1))
LGP_TB_SETTER(Samples, samples, uint_arg(c, 1))
LGP_TB_SETTER(Stride, stride, ptr_arg(c, 1))
// The driver copies the low 12 bits of the 8-byte NVNpackagedTextureLayout
// (three 4-bit fields) into the builder and sets a "packaged" flag.
LGP_TB_SETTER(PackagedTextureLayout, packaged_layout,
              ptr_arg(c, 1) == 0 ? 0 : (uint64_t{1} << 32) | (guest::load<uint16_t>(ptr_arg(c, 1)) & 0xFFFu))
#undef LGP_TB_SETTER
void TextureBuilderSetSize2D(H&, CpuState* c) {
    update<TextureBuilderState>(ptr_arg(c, 0), __func__, [&](auto& s) {
        s.width = uint_arg(c, 1);
        s.height = uint_arg(c, 2);
    });
}
void TextureBuilderSetStorage(H&, CpuState* c) {
    update<TextureBuilderState>(ptr_arg(c, 0), __func__, [&](auto& s) {
        s.pool = ptr_arg(c, 1);
        s.offset = ptr_arg(c, 2);
    });
}
void TextureBuilderGetStorageSize(H&, CpuState* c) {
    const auto s = load_state<TextureBuilderState>(ptr_arg(c, 0), __func__);
    ret(c, texture_storage_size(s.format, s.target, s.width, s.height, s.depth, s.levels));
}
void TextureBuilderGetStorageAlignment(H&, CpuState* c) { ret(c, kTextureStorageAlignment); }

// LGP_NVN_SIZE_ORIGINS=1 (diagnosis for wider pictures,
// docs/analysis/50-breitbild-ohne-strecken.md): which guest code gives which
// sizes to textures, viewports, scissors and the window crop. One line per
// distinct (call, size, return address), at most 256.
bool size_origins_on() {
    static const bool on = [] {
        const char* e = std::getenv("LGP_NVN_SIZE_ORIGINS");
        return e != nullptr && *e != '\0' && *e != '0';
    }();
    return on;
}
void size_origin(const char* what, CpuState* c, int64_t x, int64_t y, int64_t w, int64_t h, const char* extra = nullptr) {
    if (!size_origins_on())
        return;
    static std::mutex m;
    static std::set<std::tuple<const char*, int64_t, int64_t, int64_t, int64_t, uint64_t>> seen;
    const uint64_t lr = c->x[30];
    {
        std::lock_guard lock(m);
        if (seen.size() >= 256 || !seen.insert({what, x, y, w, h, lr}).second)
            return;
    }
    rt::log(LogLevel::Warn, "nvn size: %s %" PRId64 ",%" PRId64 " %" PRId64 "x%" PRId64 " from 0x%" PRIx64 "%s%s", what, x,
            y, w, h, lr, extra != nullptr ? ", " : "", extra != nullptr ? extra : "");
}

void TextureInitialize(H& h, CpuState* c) {
    const uint64_t va = ptr_arg(c, 0);
    const auto b = load_state<TextureBuilderState>(ptr_arg(c, 1), __func__);
    uint64_t storage = 0;
    uint32_t pool_flags = 0;
    if (b.pool != 0) {
        if (auto* pool = find(h.pools, b.pool, __func__)) {
            storage = pool->storage + b.offset;
            pool_flags = pool->flags;
        }
    }
    if (auto old = h.textures.find(va); old != h.textures.end()) {
        h.backend.destroy_texture(old->second.id);
        h.textures.erase(old);
    }
    H::Texture t;
    t.id = h.next_texture_id++;
    t.desc = {b.target, b.format, b.width, b.height, b.depth, b.levels, b.samples, storage};
    // Packaged layout: field 0 (bits 0-3) is the level-0 block height, log2
    // GOBs (matches every texture file LGP loads; fields 1/2 are 0 there).
    if (b.packaged_layout != 0)
        t.desc.gob_height_log2 = int32_t(b.packaged_layout & 0xF);
    if (b.packaged_layout != 0)
        rt::log(LogLevel::Debug, "nvn: texture #%u packaged layout fields %u %u %u", t.id,
                unsigned(b.packaged_layout & 0xF), unsigned((b.packaged_layout >> 4) & 0xF),
                unsigned((b.packaged_layout >> 8) & 0xF));
    // Linear textures (glyph caches the CPU writes, LGP's 2048x1024 R8
    // atlases): rows at the builder's stride, or packed to 32 bytes.
    if (b.flags & (kTextureFlagLinear | kTextureFlagLinearRenderTarget)) {
        const auto fi = format_info(b.format);
        const uint64_t bw = fi ? fi->block_width : 1, bpb = fi ? fi->bytes_per_pixel : 4;
        const uint64_t row = (uint64_t(b.width) + bw - 1) / bw * bpb;
        t.desc.linear_stride = uint32_t(b.stride != 0 ? b.stride
                                                      : (row + kLinearTextureStrideAlignment - 1) /
                                                            kLinearTextureStrideAlignment * kLinearTextureStrideAlignment);
    }
    t.flags = b.flags;
    t.stride = static_cast<uint32_t>(b.stride);
    t.pool = b.pool;
    t.offset = b.offset;
    const auto info = format_info(b.format);
    if (size_origins_on()) {
        // Render target or asset? Targets usually are compressible (flag 0x8)
        // in a GPU-only pool; files come with a packaged layout.
        char extra[160];
        std::snprintf(extra, sizeof(extra), "%s, %u levels, flags 0x%x, pool flags 0x%x, packaged 0x%" PRIx64,
                      info ? std::string(info->name).c_str() : "unknown format", b.levels, b.flags, pool_flags,
                      uint64_t(b.packaged_layout));
        size_origin("texture", c, 0, 0, b.width, b.height, extra);
    }
    rt::log(LogLevel::Debug, "nvn: texture 0x%" PRIx64 " = #%u %ux%ux%u %s, target %u, %u levels, flags 0x%x, stride %u",
            va, t.id, b.width, b.height, b.depth, info ? std::string(info->name).c_str() : "unknown format", b.target,
            b.levels, b.flags, t.stride);
    if (!info)
        rt::log(LogLevel::Warn, "nvn: texture 0x%" PRIx64 " has format %u, which the HLE does not know", va, b.format);
    h.backend.create_texture(t.id, t.desc);
    h.textures[va] = t;
    // Pool slots this NVNtexture was registered in follow the new texture
    // (H::texture_slots).
    if (auto slots = h.texture_slots.find(va); slots != h.texture_slots.end()) {
        auto& list = slots->second;
        size_t kept = 0, followed = 0;
        for (const auto& [pool_va, slot] : list) {
            auto pool = h.texture_pools.find(pool_va);
            if (pool == h.texture_pools.end())
                continue;
            auto e = pool->second.textures.find(slot);
            if (e == pool->second.textures.end() || e->second.object != va)
                continue;
            e->second.texture = t.id;
            list[kept++] = {pool_va, slot};
            ++followed;
        }
        list.resize(kept);
        if (list.empty())
            h.texture_slots.erase(slots);
        if (followed != 0)
            rt::log(LogLevel::Debug, "nvn: texture 0x%" PRIx64 " rebuilt in place: %zu pool slots follow #%u", va,
                    followed, t.id);
    }
    ret_bool(c, true);
}
void TextureFinalize(H& h, CpuState* c) {
    auto it = h.textures.find(ptr_arg(c, 0));
    if (it == h.textures.end())
        return;
    h.backend.destroy_texture(it->second.id);
    h.textures.erase(it);
}
#define LGP_TEX_GETTER(Name, expr)                                    \
    void TextureGet##Name(H& h, CpuState* c) {                        \
        auto* t = find(h.textures, ptr_arg(c, 0), "nvnTextureGet" #Name); \
        ret(c, t ? uint64_t(expr) : 0);                               \
    }
LGP_TEX_GETTER(Width, t->desc.width)
LGP_TEX_GETTER(Height, t->desc.height)
LGP_TEX_GETTER(Depth, t->desc.depth)
LGP_TEX_GETTER(Levels, t->desc.levels)
LGP_TEX_GETTER(Format, t->desc.format)
LGP_TEX_GETTER(Target, t->desc.target)
LGP_TEX_GETTER(Samples, t->desc.samples)
LGP_TEX_GETTER(Flags, t->flags)
#undef LGP_TEX_GETTER
void TextureSetDebugLabel(H&, CpuState*) {}

// TextureView: a small value type the game fills and passes by pointer.
void TextureViewSetDefaults(H&, CpuState* c) { store_state(ptr_arg(c, 0), TextureViewState{}); }
void TextureViewSetLevels(H&, CpuState* c) {
    update<TextureViewState>(ptr_arg(c, 0), __func__, [&](auto& s) {
        s.base_level = static_cast<uint16_t>(uint_arg(c, 1));
        s.num_levels = static_cast<uint16_t>(uint_arg(c, 2));
        s.set |= TextureViewState::kLevels;
    });
}
void TextureViewSetLayers(H&, CpuState* c) {
    update<TextureViewState>(ptr_arg(c, 0), __func__, [&](auto& s) {
        s.base_layer = static_cast<uint16_t>(uint_arg(c, 1));
        s.num_layers = static_cast<uint16_t>(uint_arg(c, 2));
        s.set |= TextureViewState::kLayers;
    });
}
void TextureViewSetFormat(H&, CpuState* c) {
    update<TextureViewState>(ptr_arg(c, 0), __func__, [&](auto& s) {
        s.format = uint_arg(c, 1);
        s.set |= TextureViewState::kFormat;
    });
}
void TextureViewSetTarget(H&, CpuState* c) {
    update<TextureViewState>(ptr_arg(c, 0), __func__, [&](auto& s) {
        s.target = uint_arg(c, 1);
        s.set |= TextureViewState::kTarget;
    });
}
void TextureViewSetDepthStencilMode(H&, CpuState* c) {
    update<TextureViewState>(ptr_arg(c, 0), __func__, [&](auto& s) {
        s.depth_stencil_mode = uint_arg(c, 1);
        s.set |= TextureViewState::kDepthStencil;
    });
}
void TextureViewSetSwizzle(H&, CpuState* c) {
    update<TextureViewState>(ptr_arg(c, 0), __func__, [&](auto& s) {
        for (int i = 0; i < 4; ++i)
            s.swizzle[i] = static_cast<uint8_t>(uint_arg(c, 1 + i));
        s.set |= TextureViewState::kSwizzle;
    });
}
}  // namespace

SwizzleRgba texture_view_swizzle(uint64_t view) {
    SwizzleRgba out = kIdentitySwizzle;
    if (view == 0)
        return out;
    const auto s = load_state<TextureViewState>(view, __func__);
    if (!(s.set & TextureViewState::kSwizzle))
        return out;
    for (int i = 0; i < 4; ++i) {
        // NVNtextureSwizzle: ZERO, ONE, R, G, B, A = 0..5.
        if (s.swizzle[i] <= static_cast<uint8_t>(Swizzle::A))
            out[i] = static_cast<Swizzle>(s.swizzle[i]);
        else if (static std::atomic<bool> warned{false}; !warned.exchange(true))
            rt::log(LogLevel::Warn, "nvn: texture view swizzle %u unknown, identity used", unsigned(s.swizzle[i]));
    }
    return out;
}

void texture_view_base(uint64_t view, uint32_t* level, uint32_t* layer) {
    *level = *layer = 0;
    if (view == 0)
        return;
    const auto s = load_state<TextureViewState>(view, __func__);
    if (s.set & TextureViewState::kLevels)
        *level = s.base_level;
    if (s.set & TextureViewState::kLayers)
        *layer = s.base_layer;
}

void note_registered_view(uint64_t view, const TextureDesc& desc, uint32_t texture_id, int32_t pool_id) {
    if (view == 0)
        return;
    const auto s = load_state<TextureViewState>(view, __func__);
    const uint32_t extra = s.set & ~uint32_t{TextureViewState::kSwizzle};
    if (extra == 0)
        return;
    // Every registration counts; the first 32 distinct view settings are logged
    // in full, the total at powers of two.
    static std::atomic<uint64_t> count{0};
    const uint64_t n = ++count;
    if ((n & (n - 1)) == 0)
        rt::log(LogLevel::Info, "nvn: %" PRIu64 " texture registrations with a view beyond swizzle (not applied)", n);
    static std::mutex mutex;
    static std::set<std::array<uint32_t, 9>> seen;
    const std::array<uint32_t, 9> key = {extra,        s.base_level,  s.num_levels, s.base_layer,
                                         s.num_layers, s.format,      s.target,     s.depth_stencil_mode,
                                         desc.format};
    std::lock_guard lock(mutex);
    if (seen.size() >= 32 || !seen.insert(key).second)
        return;
    rt::log(LogLevel::Info,
            "nvn: RegisterTexture view (not applied): pool id %d, texture #%u format %u target %u %ux%ux%u levels %u; "
            "view set 0x%x levels %u+%u layers %u+%u format %u target %u depth/stencil mode %u",
            pool_id, texture_id, desc.format, desc.target, desc.width, desc.height, desc.depth, desc.levels, extra,
            unsigned(s.base_level), unsigned(s.num_levels), unsigned(s.base_layer), unsigned(s.num_layers), s.format,
            s.target, s.depth_stencil_mode);
}

namespace {
// Getters: NVNboolean "was this set", values through out-pointers.
void TextureViewGetLevels(H&, CpuState* c) {
    const auto s = load_state<TextureViewState>(ptr_arg(c, 0), __func__);
    guest::store<int32_t>(ptr_arg(c, 1), s.base_level);
    guest::store<int32_t>(ptr_arg(c, 2), s.num_levels);
    ret_bool(c, s.set & TextureViewState::kLevels);
}
void TextureViewGetLayers(H&, CpuState* c) {
    const auto s = load_state<TextureViewState>(ptr_arg(c, 0), __func__);
    guest::store<int32_t>(ptr_arg(c, 1), s.base_layer);
    guest::store<int32_t>(ptr_arg(c, 2), s.num_layers);
    ret_bool(c, s.set & TextureViewState::kLayers);
}
void TextureViewGetFormat(H&, CpuState* c) {
    const auto s = load_state<TextureViewState>(ptr_arg(c, 0), __func__);
    guest::store<uint32_t>(ptr_arg(c, 1), s.format);
    ret_bool(c, s.set & TextureViewState::kFormat);
}
void TextureViewGetTarget(H&, CpuState* c) {
    const auto s = load_state<TextureViewState>(ptr_arg(c, 0), __func__);
    guest::store<uint32_t>(ptr_arg(c, 1), s.target);
    ret_bool(c, s.set & TextureViewState::kTarget);
}
void TextureViewGetDepthStencilMode(H&, CpuState* c) {
    const auto s = load_state<TextureViewState>(ptr_arg(c, 0), __func__);
    guest::store<uint32_t>(ptr_arg(c, 1), s.depth_stencil_mode);
    ret_bool(c, s.set & TextureViewState::kDepthStencil);
}

// ---------------------------------------------------------------------------
// Window

void WindowBuilderSetDefaults(H&, CpuState* c) { store_state(ptr_arg(c, 0), WindowBuilderState{}); }
void WindowBuilderSetDevice(H&, CpuState* c) {
    update<WindowBuilderState>(ptr_arg(c, 0), __func__, [&](auto& s) { s.device = ptr_arg(c, 1); });
}
void WindowBuilderSetNativeWindow(H&, CpuState* c) {
    update<WindowBuilderState>(ptr_arg(c, 0), __func__, [&](auto& s) { s.native_window = ptr_arg(c, 1); });
}
void WindowBuilderSetPresentInterval(H&, CpuState* c) {
    update<WindowBuilderState>(ptr_arg(c, 0), __func__, [&](auto& s) { s.present_interval = int_arg(c, 1); });
}
void WindowBuilderSetTextures(H&, CpuState* c) {
    const int32_t n = int_arg(c, 1);
    const uint64_t array = ptr_arg(c, 2);
    update<WindowBuilderState>(ptr_arg(c, 0), __func__, [&](auto& s) {
        if (n < 0 || n > static_cast<int32_t>(WindowBuilderState::kMaxTextures))
            rt::log(LogLevel::Error, "nvn: nvnWindowBuilderSetTextures with %d textures, the HLE keeps at most %u", n,
                    WindowBuilderState::kMaxTextures);
        s.num_textures = static_cast<uint32_t>(std::clamp<int32_t>(n, 0, WindowBuilderState::kMaxTextures));
        for (uint32_t i = 0; i < s.num_textures; ++i)
            s.textures[i] = guest::load<uint64_t>(array + uint64_t(i) * 8);
    });
}

void WindowInitialize(H& h, CpuState* c) {
    const auto b = load_state<WindowBuilderState>(ptr_arg(c, 1), __func__);
    H::Window w;
    w.device = b.device;
    w.native_window = b.native_window;
    w.present_interval = b.present_interval;
    w.textures.assign(b.textures, b.textures + b.num_textures);
    if (w.textures.empty()) {
        rt::log(LogLevel::Error, "nvn: nvnWindowInitialize without textures");
        ret_bool(c, false);
        return;
    }
    h.windows[ptr_arg(c, 0)] = w;
    rt::log(LogLevel::Info, "nvn: window 0x%" PRIx64 " initialized with %zu textures", ptr_arg(c, 0),
            w.textures.size());
    ret_bool(c, true);
}
void WindowFinalize(H& h, CpuState* c) { h.windows.erase(ptr_arg(c, 0)); }

// Round robin: the HLE presents synchronously, so the next texture is free.
uint32_t acquire(H::Window& w) {
    const uint32_t index = w.next;
    w.next = (w.next + 1) % static_cast<uint32_t>(w.textures.size());
    return index;
}
void WindowAcquireTexture(H& h, CpuState* c) {
    auto* w = find(h.windows, ptr_arg(c, 0), __func__);
    if (w == nullptr) {
        ret(c, 1);
        return;
    }
    guest::store<int32_t>(ptr_arg(c, 2), static_cast<int32_t>(acquire(*w)));
    if (ptr_arg(c, 1) != 0) {
        if (auto* s = find(h.syncs, ptr_arg(c, 1), __func__))
            s->signaled = true;
    }
    ret(c, kAcquireTextureSuccess);
}
void QueueAcquireTexture(H& h, CpuState* c) {
    auto* w = find(h.windows, ptr_arg(c, 1), __func__);
    if (w == nullptr) {
        ret(c, 1);
        return;
    }
    guest::store<int32_t>(ptr_arg(c, 2), static_cast<int32_t>(acquire(*w)));
    ret(c, kAcquireTextureSuccess);
}
void WindowSetCrop(H& h, CpuState* c) {
    size_origin("crop", c, int_arg(c, 1), int_arg(c, 2), int_arg(c, 3), int_arg(c, 4));
    if (auto* w = find(h.windows, ptr_arg(c, 0), __func__))
        w->crop = {int_arg(c, 1), int_arg(c, 2), int_arg(c, 3), int_arg(c, 4)};
}
void WindowGetPresentInterval(H& h, CpuState* c) {
    auto* w = find(h.windows, ptr_arg(c, 0), __func__);
    ret(c, w ? static_cast<uint32_t>(w->present_interval) : 0);
}
void WindowSetPresentInterval(H& h, CpuState* c) {
    if (auto* w = find(h.windows, ptr_arg(c, 0), __func__))
        w->present_interval = int_arg(c, 1);
}
void WindowGetNativeWindow(H& h, CpuState* c) {
    auto* w = find(h.windows, ptr_arg(c, 0), __func__);
    ret(c, w ? w->native_window : 0);
}

std::atomic<uint64_t> g_presents{0};

void QueuePresentTexture(H& h, CpuState* c) {
    auto* w = find(h.windows, ptr_arg(c, 1), __func__);
    const int32_t index = int_arg(c, 2);
    if (w == nullptr || index < 0 || index >= static_cast<int32_t>(w->textures.size())) {
        rt::log(LogLevel::Error, "nvn: nvnQueuePresentTexture: bad window 0x%" PRIx64 " or index %d", ptr_arg(c, 1),
                index);
        return;
    }
    auto* t = find(h.textures, w->textures[index], __func__);
    if (t == nullptr)
        return;
    if (h.last_present_interval.exchange(w->present_interval, std::memory_order_relaxed) != w->present_interval)
        rt::log(LogLevel::Info, "nvn: present interval %d", w->present_interval);
    // Window coordinates are memory rows (with the lower-left origin row 0
    // is the bottom of the picture): the crop applies as is, and the display
    // shows the result flipped. LGP renders its 1280x720 handheld frame at
    // viewport and crop (0, 0) into a 1920x1080 window texture.
    if (draw_stats_on())
        draw_stats_flush();  // before the count moves on: the frame just finished
    g_presents.fetch_add(1, std::memory_order_relaxed);
    if (!h.backend.present({t->id, w->crop, h.origin_lower_left(w->device)})) {
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true))
            rt::log(LogLevel::Info, "nvn: renderer reports the window closed");
    }
}

// ---------------------------------------------------------------------------
// Command buffers. Commands are recorded on the host; the game's command and
// control memory is accepted but not written.

void CommandBufferInitialize(H& h, CpuState* c) {
    H::CommandBuffer cb;
    cb.device = ptr_arg(c, 1);
    h.command_buffers[ptr_arg(c, 0)] = std::move(cb);
    ret_bool(c, true);
}
void CommandBufferFinalize(H& h, CpuState* c) {
    auto it = h.command_buffers.find(ptr_arg(c, 0));
    if (it == h.command_buffers.end())
        return;
    for (const auto& entry : it->second.handles)
        h.command_handles.erase(entry.first);
    h.command_buffers.erase(it);
}
// Memory given to a command buffer (again): on hardware the lists recorded
// into it before are overwritten from now on, so their handles are released
// here. Games reuse command memory every frame and rarely finalize handles
// (DeviceFinalizeCommandHandle); without this the handle table grows.
void add_memory(H& h, H::CommandBuffer* cb, H::CommandBuffer::MemoryRange r, H::CommandBuffer::MemoryRange* current) {
    if (r.end <= r.begin)
        return;
    size_t released = 0;
    for (auto it = cb->handles.begin(); it != cb->handles.end();) {
        bool reused = false;
        for (const auto& m : it->second)
            reused |= m.begin < r.end && r.begin < m.end;
        if (reused) {
            h.command_handles.erase(it->first);
            it = cb->handles.erase(it);
            ++released;
        } else {
            ++it;
        }
    }
    h.handles_reused += released;
    if (static std::atomic<bool> logged{false}; released != 0 && !logged.exchange(true))
        rt::log(LogLevel::Info, "nvn: command memory reused, %zu command handle(s) recorded into it released",
                released);
    *current = r;
    if (cb->recording)
        cb->recording_memory.push_back(r);
}
// AddCommandMemory(cmd, memoryPool, offset, size): GPU memory in a pool.
void CommandBufferAddCommandMemory(H& h, CpuState* c) {
    auto* cb = find(h.command_buffers, ptr_arg(c, 0), __func__);
    auto pool = h.pools.find(ptr_arg(c, 1));
    if (cb == nullptr || pool == h.pools.end())
        return;
    const uint64_t begin = pool->second.storage + ptr_arg(c, 2);
    add_memory(h, cb, {begin, begin + ptr_arg(c, 3)}, &cb->command_memory);
}
// AddControlMemory(cmd, memory, size): CPU memory.
void CommandBufferAddControlMemory(H& h, CpuState* c) {
    if (auto* cb = find(h.command_buffers, ptr_arg(c, 0), __func__))
        add_memory(h, cb, {ptr_arg(c, 1), ptr_arg(c, 1) + ptr_arg(c, 2)}, &cb->control_memory);
}
void CommandBufferSetMemoryCallback(H& h, CpuState* c) {
    if (auto* cb = find(h.command_buffers, ptr_arg(c, 0), __func__))
        cb->memory_callback = ptr_arg(c, 1);
}
void CommandBufferSetMemoryCallbackData(H& h, CpuState* c) {
    if (auto* cb = find(h.command_buffers, ptr_arg(c, 0), __func__))
        cb->memory_callback_data = ptr_arg(c, 1);
}
void CommandBufferGetCommandMemoryUsed(H&, CpuState* c) { ret(c, 0); }

void CommandBufferBeginRecording(H& h, CpuState* c) {
    auto* cb = find(h.command_buffers, ptr_arg(c, 0), __func__);
    if (cb == nullptr)
        return;
    if (cb->recording)
        rt::log(LogLevel::Warn, "nvn: nvnCommandBufferBeginRecording while recording, previous commands dropped");
    cb->recording = true;
    cb->recording_memory.clear();
    for (const auto& m : {cb->command_memory, cb->control_memory})
        if (m.end > m.begin)
            cb->recording_memory.push_back(m);
    cb->list = std::make_shared<CommandList>();
    cb->list->origin_lower_left = h.origin_lower_left(cb->device);
    cb->list->depth_minus_w = h.depth_minus_w(cb->device);
}
void CommandBufferEndRecording(H& h, CpuState* c) {
    auto* cb = h.recording(ptr_arg(c, 0), __func__);
    if (cb == nullptr) {
        ret(c, 0);
        return;
    }
    cb->recording = false;
    const uint64_t handle = 0x4C47'0000'0000'0000ull | h.next_handle++;
    h.command_handles[handle] = std::move(cb->list);
    cb->handles[handle] = std::move(cb->recording_memory);
    cb->recording_memory.clear();
    // Report each new high from 4096 on, doubling: a bounded table (handles
    // released on memory reuse or finalize) stops at one size, a leak keeps
    // doubling and turns into a warning.
    if (h.command_handles.size() >= h.handle_report_at) {
        rt::log(h.handle_report_at >= 16384 ? LogLevel::Warn : LogLevel::Info,
                "nvn: %zu command handles alive (new high; released so far: %llu on memory reuse, %llu finalized)",
                h.command_handles.size(), (unsigned long long)h.handles_reused,
                (unsigned long long)h.handles_finalized);
        h.handle_report_at *= 2;
    }
    ret(c, handle);
}
// nvnCommandBufferCallCommands / CopyCommands(cmd, count, handles): the
// commands of finished recordings become part of the current one (call or
// copy makes no difference here: lists are host objects, never patched).
void CommandBufferCallCommands(H& h, CpuState* c) {
    auto* cb = h.recording(ptr_arg(c, 0), __func__);
    if (cb == nullptr)
        return;
    const int32_t n = int_arg(c, 1);
    const uint64_t handles = ptr_arg(c, 2);
    for (int32_t i = 0; i < n; ++i) {
        const uint64_t handle = guest::load<uint64_t>(handles + uint64_t(i) * 8);
        auto it = h.command_handles.find(handle);
        if (it == h.command_handles.end()) {
            rt::log(LogLevel::Warn, "nvn: %s: unknown command handle 0x%" PRIx64, __func__, handle);
            continue;
        }
        const auto& src = it->second->commands;
        cb->list->commands.insert(cb->list->commands.end(), src.begin(), src.end());
    }
}
void CommandBufferCopyCommands(H& h, CpuState* c) { CommandBufferCallCommands(h, c); }

void CommandBufferIsRecording(H& h, CpuState* c) {
    auto it = h.command_buffers.find(ptr_arg(c, 0));
    ret_bool(c, it != h.command_buffers.end() && it->second.recording);
}

TextureId texture_id(H& h, uint64_t va) {
    if (va == 0)
        return 0;
    auto it = h.textures.find(va);
    if (it == h.textures.end()) {
        rt::log(LogLevel::Warn, "nvn: render target 0x%" PRIx64 " is not an initialized texture", va);
        return 0;
    }
    return it->second.id;
}

void CommandBufferSetRenderTargets(H& h, CpuState* c) {
    auto* cb = h.recording(ptr_arg(c, 0), __func__);
    if (cb == nullptr)
        return;
    const int32_t n = std::clamp(int_arg(c, 1), 0, 8);
    const uint64_t colors = ptr_arg(c, 2);
    CmdSetRenderTargets cmd;
    for (int32_t i = 0; i < n; ++i)
        cmd.colors.push_back(texture_id(h, guest::load<uint64_t>(colors + uint64_t(i) * 8)));
    cmd.depth_stencil = texture_id(h, ptr_arg(c, 4));
    // Views (x3, x5) select levels/layers; the backends render level 0,
    // layer 0 for now.
    cb->list->commands.push_back(std::move(cmd));
}
void CommandBufferClearColor(H& h, CpuState* c) {
    auto* cb = h.recording(ptr_arg(c, 0), __func__);
    if (cb == nullptr)
        return;
    CmdClearColor cmd;
    cmd.index = uint_arg(c, 1);
    guest::read(cmd.rgba, ptr_arg(c, 2), sizeof(cmd.rgba));
    cmd.mask = uint_arg(c, 3);
    cb->list->commands.push_back(cmd);
}
void CommandBufferClearDepthStencil(H& h, CpuState* c) {
    auto* cb = h.recording(ptr_arg(c, 0), __func__);
    if (cb == nullptr)
        return;
    CmdClearDepthStencil cmd;
    cmd.depth = float_arg(c, 0);
    cmd.depth_mask = bool_arg(c, 1);
    cmd.stencil = uint_arg(c, 2);
    cmd.stencil_mask = uint_arg(c, 3);
    cb->list->commands.push_back(cmd);
}
void CommandBufferSetScissor(H& h, CpuState* c) {
    auto* cb = h.recording(ptr_arg(c, 0), __func__);
    if (cb == nullptr)
        return;
    size_origin("scissor", c, int_arg(c, 1), int_arg(c, 2), int_arg(c, 3), int_arg(c, 4));
    cb->list->commands.push_back(
        CmdSetScissors{0, {Rect{int_arg(c, 1), int_arg(c, 2), int_arg(c, 3), int_arg(c, 4)}}});
}
void CommandBufferSetScissors(H& h, CpuState* c) {
    auto* cb = h.recording(ptr_arg(c, 0), __func__);
    if (cb == nullptr)
        return;
    CmdSetScissors cmd;
    cmd.first = uint_arg(c, 1);
    const int32_t count = std::clamp(int_arg(c, 2), 0, 16);
    cmd.rects.resize(count);
    for (int32_t i = 0; i < count; ++i)
        guest::read(&cmd.rects[i], ptr_arg(c, 3) + uint64_t(i) * 16, 16);
    for (const Rect& r : cmd.rects)
        size_origin("scissors", c, r.x, r.y, r.width, r.height);
    cb->list->commands.push_back(std::move(cmd));
}
void CommandBufferSetViewports(H& h, CpuState* c) {
    auto* cb = h.recording(ptr_arg(c, 0), __func__);
    if (cb == nullptr)
        return;
    CmdSetViewports cmd;
    cmd.first = uint_arg(c, 1);
    const int32_t count = std::clamp(int_arg(c, 2), 0, 16);
    cmd.xywh.resize(size_t(count) * 4);
    guest::read(cmd.xywh.data(), ptr_arg(c, 3), cmd.xywh.size() * sizeof(float));
    for (size_t i = 0; i + 3 < cmd.xywh.size(); i += 4)
        size_origin("viewport", c, std::lround(cmd.xywh[i]), std::lround(cmd.xywh[i + 1]), std::lround(cmd.xywh[i + 2]),
                    std::lround(cmd.xywh[i + 3]));
    cb->list->commands.push_back(std::move(cmd));
}
void CommandBufferSetDepthRange(H& h, CpuState* c) {
    auto* cb = h.recording(ptr_arg(c, 0), __func__);
    if (cb == nullptr)
        return;
    cb->list->commands.push_back(CmdSetDepthRanges{0, {float_arg(c, 0), float_arg(c, 1)}});
}
void CommandBufferSetDepthRanges(H& h, CpuState* c) {
    auto* cb = h.recording(ptr_arg(c, 0), __func__);
    if (cb == nullptr)
        return;
    CmdSetDepthRanges cmd;
    cmd.first = uint_arg(c, 1);
    const int32_t count = std::clamp(int_arg(c, 2), 0, 16);
    cmd.near_far.resize(size_t(count) * 2);
    guest::read(cmd.near_far.data(), ptr_arg(c, 3), cmd.near_far.size() * sizeof(float));
    cb->list->commands.push_back(std::move(cmd));
}
void CommandBufferBarrier(H& h, CpuState* c) {
    auto* cb = h.recording(ptr_arg(c, 0), __func__);
    if (cb == nullptr)
        return;
    cb->list->commands.push_back(CmdBarrier{uint_arg(c, 1)});
}

// ---------------------------------------------------------------------------

#define P(name) {"nvn" #name, &name}
// Sorted by name (checked at first lookup).
constexpr ProcImpl kProcs[] = {
    {"nvnBootstrapLoader", &BootstrapLoader},
    P(BufferBuilderSetDefaults),
    P(BufferBuilderSetDevice),
    P(BufferBuilderSetStorage),
    P(BufferFinalize),
    P(BufferFlushMappedRange),
    P(BufferGetAddress),
    P(BufferInitialize),
    P(BufferInvalidateMappedRange),
    P(BufferMap),
    P(BufferSetDebugLabel),
    P(CommandBufferAddCommandMemory),
    P(CommandBufferAddControlMemory),
    P(CommandBufferBarrier),
    P(CommandBufferBeginRecording),
    P(CommandBufferCallCommands),
    P(CommandBufferClearColor),
    P(CommandBufferClearDepthStencil),
    P(CommandBufferCopyCommands),
    P(CommandBufferEndRecording),
    P(CommandBufferFinalize),
    P(CommandBufferGetCommandMemoryUsed),
    P(CommandBufferInitialize),
    P(CommandBufferIsRecording),
    P(CommandBufferSetDepthRange),
    P(CommandBufferSetDepthRanges),
    P(CommandBufferSetMemoryCallback),
    P(CommandBufferSetMemoryCallbackData),
    P(CommandBufferSetRenderTargets),
    P(CommandBufferSetScissor),
    P(CommandBufferSetScissors),
    P(CommandBufferSetViewports),
    P(DeviceBuilderSetDefaults),
    P(DeviceBuilderSetFlags),
    P(DeviceFinalize),
    P(DeviceFinalizeCommandHandle),
    P(DeviceGetDepthMode),
    P(DeviceGetInteger),
    P(DeviceGetProcAddress),
    P(DeviceGetTexelFetchHandle),
    P(DeviceGetTextureHandle),
    P(DeviceGetWindowOriginMode),
    P(DeviceInitialize),
    P(DeviceSetDepthMode),
    P(DeviceSetWindowOriginMode),
    P(MemoryPoolBuilderSetDefaults),
    P(MemoryPoolBuilderSetDevice),
    P(MemoryPoolBuilderSetFlags),
    P(MemoryPoolBuilderSetStorage),
    P(MemoryPoolFinalize),
    P(MemoryPoolFlushMappedRange),
    P(MemoryPoolGetBufferAddress),
    P(MemoryPoolGetFlags),
    P(MemoryPoolInitialize),
    P(MemoryPoolInvalidateMappedRange),
    P(MemoryPoolMap),
    P(MemoryPoolSetDebugLabel),
    P(QueueAcquireTexture),
    P(QueueBuilderSetComputeMemorySize),
    P(QueueBuilderSetDefaults),
    P(QueueBuilderSetDevice),
    P(QueueFenceSync),
    P(QueueFinalize),
    P(QueueFinish),
    P(QueueFlush),
    P(QueueGetError),
    P(QueueInitialize),
    P(QueuePresentTexture),
    P(QueueSubmitCommands),
    P(QueueWaitSync),
    P(SyncFinalize),
    P(SyncInitialize),
    P(SyncWait),
    P(TextureBuilderGetStorageAlignment),
    P(TextureBuilderGetStorageSize),
    P(TextureBuilderSetDefaults),
    P(TextureBuilderSetDepth),
    P(TextureBuilderSetDevice),
    P(TextureBuilderSetFlags),
    P(TextureBuilderSetFormat),
    P(TextureBuilderSetHeight),
    P(TextureBuilderSetLevels),
    P(TextureBuilderSetPackagedTextureLayout),
    P(TextureBuilderSetSamples),
    P(TextureBuilderSetSize2D),
    P(TextureBuilderSetStorage),
    P(TextureBuilderSetStride),
    P(TextureBuilderSetTarget),
    P(TextureBuilderSetWidth),
    P(TextureFinalize),
    P(TextureGetDepth),
    P(TextureGetFlags),
    P(TextureGetFormat),
    P(TextureGetHeight),
    P(TextureGetLevels),
    P(TextureGetSamples),
    P(TextureGetTarget),
    P(TextureGetWidth),
    P(TextureInitialize),
    P(TextureSetDebugLabel),
    P(TextureViewGetDepthStencilMode),
    P(TextureViewGetFormat),
    P(TextureViewGetLayers),
    P(TextureViewGetLevels),
    P(TextureViewGetTarget),
    P(TextureViewSetDefaults),
    P(TextureViewSetDepthStencilMode),
    P(TextureViewSetFormat),
    P(TextureViewSetLayers),
    P(TextureViewSetLevels),
    P(TextureViewSetSwizzle),
    P(TextureViewSetTarget),
    P(WindowAcquireTexture),
    P(WindowBuilderSetDefaults),
    P(WindowBuilderSetDevice),
    P(WindowBuilderSetNativeWindow),
    P(WindowBuilderSetPresentInterval),
    P(WindowBuilderSetTextures),
    P(WindowFinalize),
    P(WindowGetNativeWindow),
    P(WindowGetPresentInterval),
    P(WindowInitialize),
    P(WindowSetCrop),
    P(WindowSetPresentInterval),
};
#undef P

// Both loaders answer with HLE thunks. nvnBootstrapLoader is documented for
// "nvnDeviceGetProcAddress" but games may ask it for others as well.
uint64_t lookup(H& h, uint64_t name_va) {
    const std::string name = guest::read_cstr(name_va, 128);
    if (name.size() < 4 || name.compare(0, 3, "nvn") != 0)
        return 0;
    const uint32_t index = h.proc_index(name);
    return index == UINT32_MAX ? 0 : h.thunk_va(index);
}

void BootstrapLoader(H& h, CpuState* c) { ret(c, lookup(h, ptr_arg(c, 0))); }
void DeviceGetProcAddress(H& h, CpuState* c) { ret(c, lookup(h, ptr_arg(c, 1))); }

}  // namespace

uint64_t presents_so_far() { return g_presents.load(std::memory_order_relaxed); }

const ProcImpl* find_proc_impl(std::string_view name) {
    static const bool sorted = std::is_sorted(std::begin(kProcs), std::end(kProcs),
                                              [](const ProcImpl& a, const ProcImpl& b) { return a.name < b.name; });
    if (!sorted) {
        rt::log(LogLevel::Error, "nvn: proc table not sorted");
        return nullptr;
    }
    auto it = std::lower_bound(std::begin(kProcs), std::end(kProcs), name,
                               [](const ProcImpl& p, std::string_view n) { return p.name < n; });
    if (it != std::end(kProcs) && it->name == name)
        return it;
    size_t n = 0;
    const ProcImpl* draw = draw_proc_table(&n);
    static const bool draw_sorted =
        std::is_sorted(draw, draw + n, [](const ProcImpl& a, const ProcImpl& b) { return a.name < b.name; });
    if (!draw_sorted) {
        rt::log(LogLevel::Error, "nvn: draw proc table not sorted");
        return nullptr;
    }
    auto d = std::lower_bound(draw, draw + n, name, [](const ProcImpl& p, std::string_view v) { return p.name < v; });
    return d != draw + n && d->name == name ? d : nullptr;
}

size_t proc_impl_count() {
    size_t n = 0;
    draw_proc_table(&n);
    return std::size(kProcs) + n;
}

WaitStats wait_stats() {
    return {g_waits.finish_calls.load(std::memory_order_relaxed), g_waits.finish_ns.load(std::memory_order_relaxed),
            g_waits.sync_calls.load(std::memory_order_relaxed), g_waits.sync_ns.load(std::memory_order_relaxed)};
}

}  // namespace lgp::gpu::nvn
