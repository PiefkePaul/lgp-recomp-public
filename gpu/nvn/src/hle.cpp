// Thunks and proc table of the NVN-HLE (hle.hpp).
#include <algorithm>
#include <array>
#include <atomic>
#include <cinttypes>
#include <utility>

#include "guest_access.hpp"
#include "hle_impl.hpp"
#include "lgp/runtime/dispatch.hpp"
#include "lgp/runtime/host_work.hpp"
#include "lgp/runtime/log.hpp"
#include "lgp/runtime/time_stats.hpp"

namespace lgp::gpu::nvn {

namespace {

using rt::LogLevel;

std::atomic<Hle::Impl*> g_active{nullptr};

void halt(CpuState* ctx, uint32_t reason, uint64_t info) {
    if (ctx->halt == kHaltNone) {
        ctx->halt = reason;
        ctx->halt_info = info;
    }
}

void enter(CpuState* ctx, uint32_t index) {
    Hle::Impl* h = g_active.load(std::memory_order_acquire);
    if (h == nullptr) {
        rt::log(LogLevel::Error, "nvn: thunk %u called without an installed HLE", index);
        halt(ctx, kHaltUndefined, 0);
        return;
    }
    // P6 time accounting: waiting for the lock, then recording (renderer
    // calls inside switch to TimeCat::Gpu themselves).
    rt::TimeScope timing(rt::TimeCat::NvnLock);
    std::unique_lock lock(h->mutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        // The holder may have given its slot up for a renderer call and will
        // want it back before it unlocks: wait without the slot, else the
        // two threads wait for each other (host_work.hpp).
        rt::HostWorkScope away;
        lock.lock();
    }
    if (rt::time_stats_enabled())
        rt::time_stats_switch(rt::TimeCat::Nvn);
    if (index >= h->proc_count) {
        rt::log(LogLevel::Error, "nvn: thunk %u was never handed out", index);
        halt(ctx, kHaltBadJump, h->thunk_va(index));
        return;
    }
    Hle::Impl::Proc& p = h->procs[index];
    p.calls.fetch_add(1, std::memory_order_relaxed);
    if (h->options.trace)
        rt::log(LogLevel::Info, "nvn: %s(0x%" PRIx64 ", 0x%" PRIx64 ", 0x%" PRIx64 ", 0x%" PRIx64 ", 0x%" PRIx64
                                ") s0=%g s1=%g",
                p.name.c_str(), ctx->x[0], ctx->x[1], ctx->x[2], ctx->x[3], ctx->x[4],
                static_cast<double>(guest::float_arg(ctx, 0)), static_cast<double>(guest::float_arg(ctx, 1)));

    guest::FaultScope faults;
    if (p.fn != nullptr) {
        p.fn(*h, ctx);
    } else {
        const bool first = h->unimplemented.insert(p.name).second;
        if (h->options.unimplemented == UnimplementedMode::Halt) {
            rt::log(LogLevel::Error, "nvn: %s is not implemented (HLE), guest halted", p.name.c_str());
            halt(ctx, kHaltUndefined, h->thunk_va(index));
            return;
        }
        if (first)
            rt::log(LogLevel::Warn, "nvn: %s is not implemented (HLE), returning 0", p.name.c_str());
        if (p.command) {
            auto it = h->command_buffers.find(ctx->x[0]);
            if (it != h->command_buffers.end() && it->second.recording)
                it->second.list->commands.push_back(CmdUnsupported{index, p.name.c_str()});
        }
        ctx->x[0] = 0;
    }
    if (faults.faulted()) {
        rt::log(LogLevel::Error, "nvn: %s: guest pointer 0x%" PRIx64 " not accessible, guest halted", p.name.c_str(),
                faults.address());
        halt(ctx, kHaltMemFault, faults.address());
    }
}

// One host function per thunk address: the dispatch table maps a guest VA
// to a plain LiftedFn without context, so the index is a template argument.
template <uint32_t I>
void thunk(CpuState* ctx) {
    enter(ctx, I);
}

template <size_t... I>
constexpr std::array<LiftedFn, sizeof...(I)> make_thunks(std::index_sequence<I...>) {
    return {&thunk<static_cast<uint32_t>(I)>...};
}

constexpr auto kThunks = make_thunks(std::make_index_sequence<Hle::kMaxProcs>{});

}  // namespace

uint32_t Hle::Impl::proc_index(std::string_view name) {
    std::lock_guard lock(mutex);
    const std::string key(name);
    if (auto it = proc_by_name.find(key); it != proc_by_name.end())
        return it->second;
    if (proc_count >= kMaxProcs) {
        rt::log(LogLevel::Error, "nvn: thunk block full, no address for %s", key.c_str());
        return UINT32_MAX;
    }
    const uint32_t index = proc_count;
    Proc& p = procs[index];
    p.name = key;
    const ProcImpl* impl = find_proc_impl(name);
    p.fn = impl != nullptr ? impl->fn : nullptr;
    p.command = is_command_buffer_proc(name);
    proc_by_name.emplace(key, index);
    ++proc_count;  // published under the mutex that enter() takes
    return index;
}

Hle::Hle(const HleOptions& options, Backend& backend) : impl_(std::make_unique<Impl>(options, backend)) {
    size_t n = 0;
    const DeviceInteger* defaults = default_device_integers(&n);
    for (size_t i = 0; i < n; ++i)
        impl_->device_integers[defaults[i].key] = defaults[i].value;
    for (const DeviceInteger& d : options.device_integers)
        impl_->device_integers[d.key] = d.value;
    impl_->proc_index("nvnBootstrapLoader");  // index 0 = thunk_base
}

Hle::~Hle() { uninstall(); }

void Hle::install() {
    std::vector<LiftedEntry> entries(kMaxProcs);
    for (uint32_t i = 0; i < kMaxProcs; ++i)
        entries[i] = {impl_->thunk_va(i), kThunks[i]};
    rt::register_functions(entries.data(), entries.size());
    g_active.store(impl_.get(), std::memory_order_release);
}

void Hle::replace_bootstrap_loader(uint64_t va) {
    const LiftedEntry entry{va, kThunks[0]};
    rt::register_functions(&entry, 1);
}

void Hle::uninstall() {
    Impl* expected = impl_.get();
    g_active.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel);
}

uint64_t Hle::bootstrap_loader_va() const { return impl_->thunk_va(0); }

int32_t Hle::present_interval(uint64_t window) const {
    std::lock_guard lock(impl_->mutex);
    const auto it = impl_->windows.find(window);
    return it != impl_->windows.end() ? it->second.present_interval : 1;
}

uint64_t Hle::proc_address(std::string_view name) {
    // Real NVN resolves exactly its own function names; everything else is
    // NULL. Without the driver's name list every "nvn…" name gets a thunk.
    if (name.size() < 4 || name.size() > 128 || name.substr(0, 3) != "nvn")
        return 0;
    const uint32_t index = impl_->proc_index(name);
    return index == UINT32_MAX ? 0 : impl_->thunk_va(index);
}

std::vector<ProcStats> Hle::stats() const {
    std::lock_guard lock(impl_->mutex);
    std::vector<ProcStats> out;
    for (uint32_t i = 0; i < impl_->proc_count; ++i) {
        const Impl::Proc& p = impl_->procs[i];
        out.push_back({p.name, impl_->thunk_va(i), p.fn != nullptr, p.calls.load(std::memory_order_relaxed)});
    }
    return out;
}

std::vector<std::string> Hle::unimplemented_calls() const {
    std::lock_guard lock(impl_->mutex);
    return {impl_->unimplemented.begin(), impl_->unimplemented.end()};
}

int32_t Hle::last_present_interval() const {
    return impl_->last_present_interval.load(std::memory_order_relaxed);
}

uint64_t Hle::untranslated_programs() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->untranslated;
}

std::vector<std::string> Hle::untranslated_reasons() const {
    std::lock_guard lock(impl_->mutex);
    std::vector<std::string> out;
    for (const auto& [reason, n] : impl_->untranslated_reasons)
        out.push_back(std::to_string(n) + "x " + reason);
    return out;
}

size_t Hle::implemented_count() { return proc_impl_count(); }

bool Hle::is_implemented(std::string_view name) { return find_proc_impl(name) != nullptr; }

Hle::Impl::CommandBuffer* Hle::Impl::recording(uint64_t cmd, const char* fn) {
    auto it = command_buffers.find(cmd);
    if (it == command_buffers.end() || !it->second.recording) {
        rt::log(LogLevel::Warn, "nvn: %s on command buffer 0x%" PRIx64 " that is not recording, ignored", fn, cmd);
        return nullptr;
    }
    return &it->second;
}

bool Hle::Impl::origin_lower_left(uint64_t device) const {
    auto it = devices.find(device);
    return it == devices.end() || it->second.origin_mode == static_cast<uint32_t>(WindowOriginMode::LowerLeft);
}

bool Hle::Impl::depth_minus_w(uint64_t device) const {
    auto it = devices.find(device);
    return it == devices.end() || it->second.depth_mode == static_cast<uint32_t>(DepthMode::NearIsMinusW);
}

BackendCallCounter g_backend_calls[size_t(BackendCall::Count)];

std::vector<BackendCallTime> backend_call_times() {
    static const char* const kKinds[] = {"programs", "textures", "texture writes", "lists", "presents", "finish"};
    static_assert(std::size(kKinds) == size_t(BackendCall::Count));
    std::vector<BackendCallTime> out;
    for (size_t i = 0; i < size_t(BackendCall::Count); ++i)
        out.push_back({kKinds[i], g_backend_calls[i].ns.load(std::memory_order_relaxed),
                       g_backend_calls[i].calls.load(std::memory_order_relaxed)});
    return out;
}

}  // namespace lgp::gpu::nvn
