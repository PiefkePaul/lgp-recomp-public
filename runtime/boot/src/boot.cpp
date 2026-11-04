#include "lgp/boot/boot.hpp"

// guest.hpp needs the complete CpuState (lifted.hpp includes it the same way).
#include "lgp/lifted/cpu_state.hpp"
// clang-format off
#include "lgp/runtime/guest.hpp"
// clang-format on

#include <algorithm>
#include <cstdlib>
#include <array>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cmath>
#include <functional>
#include <iterator>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "lgp/boot/frame_pacer.hpp"
#include "lgp/boot/keyboard.hpp"
#include "lgp/boot/stall_report.hpp"
#include "lgp/gpu/nvn/hle.hpp"
#include "lgp/hle/fs.hpp"
#include "lgp/hle/audout.hpp"
#include "lgp/hle/hid.hpp"
#include "lgp/hle/pl.hpp"
#include "lgp/hle/romfs.hpp"
#include "lgp/mods/mods.hpp"
#include "lgp/mods/widescreen.hpp"
#include "lgp/platform/host_directory_storage.hpp"
#include "lgp/platform/segment_storage.hpp"
#include "lgp/recomp/cfg.hpp"
#include "lgp/recomp/link.hpp"
#include "lgp/recomp/loader.hpp"
#include "lgp/recomp/native_cache.hpp"
#include "lgp/recomp/native_image.hpp"
#include "lgp/recomp/native_rewrite.hpp"
#include "lgp/runtime/dispatch.hpp"
#include "lgp/runtime/host_work.hpp"
#include "lgp/runtime/log.hpp"
#include "lgp/runtime/native.hpp"
#include "lgp/runtime/patch.hpp"
#include "lgp/runtime/write_watch.hpp"
#include "lgp/runtime/system.hpp"
#include "lgp/runtime/time_stats.hpp"

#include "import_shims.hpp"
#include "movie_hle.hpp"
#include "ngc_hle.hpp"

namespace lgp::boot {
namespace {

namespace rc = lgp::recomp;
using rt::LogLevel;

constexpr uint64_t kPage = 0x1000;
constexpr uint32_t kSvcExitProcess = 0x07;

uint64_t page_up(uint64_t v) { return (v + kPage - 1) & ~(kPage - 1); }

// Boot page behind the last module. The first page holds only dispatch
// keys for host functions (never mapped, never executed as guest code); the
// second is mapped RW for the data rtld would own.
constexpr uint64_t kThunkEntry = 0x00;       // main thread entry (Sdk)
constexpr uint64_t kThunkNotify = 0x04;      // notify_exception_handler_ready
constexpr uint64_t kThunkInitialize = 0x08;  // call_initializator
constexpr uint64_t kThunkLookup = 0x0C;      // nn::ro lookup_global_auto
constexpr uint64_t kThunkReturn = 0x10;      // x30 while the boot code calls guest code
// nn::init::Start's argument address: the first page (never mapped). The SDK
// (nnosInitialize) parses ProgramArguments only if this address is mapped RW;
// otherwise argc/argv keep their built-in defaults (argc 1, argv[0] a valid
// string), which games rely on. An RW page with an empty header gives argc 0
// and argv[0] == NULL instead (LGP dereferenced it during its start).
// Native start: the first page holds code (BootPages::argument).
constexpr uint64_t kArgumentAddress = 0x000;
// In the data page: the nn::ro lists.
constexpr uint64_t kAutoLoadList = 0x800;    // ModuleObjectList {front, back}, empty
constexpr uint64_t kManualLoadList = 0x810;  // same
constexpr uint64_t kRoDebugFlag = 0x820;     // bool, false
// GraphicsMode::Hle: the NVN-HLE's thunk block (keys only, like the first
// page), directly behind the data page so the dispatch table stays small.
// Undefined imports (import_shims.hpp): two pages of dispatch keys (the shims,
// then the movie HLE's virtual methods) and a mapped RW page for its vtables.
//
// Offsets of these parts in units of one page, natively of 16 KiB: there the
// key pages hold the host-call branches (R+X) and the data pages must not share
// a host page with them (W^X on 16 KiB pages, Apple).
struct BootPages {
    uint64_t unit = kPage;
    uint64_t data() const { return unit; }
    uint64_t nvn_thunks() const { return 2 * unit; }
    uint64_t import_shims() const { return 3 * unit; }
    uint64_t import_shim_data() const { return 3 * unit + round(2 * kPage); }
    uint64_t import_shim_data_size() const { return kPage * (1 + MovieHle::kAllocPages); }
    // Natively an extra page left unmapped behind the rest.
    uint64_t argument() const {
        return unit == kPage ? kArgumentAddress : import_shim_data() + round(import_shim_data_size());
    }
    uint64_t end() const { return import_shim_data() + round(import_shim_data_size()) + (unit == kPage ? 0 : unit); }
    uint64_t round(uint64_t v) const { return (v + unit - 1) & ~(unit - 1); }
};

// nn::init::Start(Handle, uintptr_t, void (*)(), void (*)()) in any SDK.
constexpr const char* kInitStartPrefix = "_ZN2nn4init5StartE";

const std::vector<std::string> kGraphicsSymbols = {
    "nvnBootstrapLoader",
    "_ZN2nv20SetGraphicsAllocator",          // nv::SetGraphicsAllocator(...)
    "_ZN2nv28SetGraphicsDevtoolsAllocator",  // nv::SetGraphicsDevtoolsAllocator(...)
    "_ZN2nv18InitializeGraphics",            // nv::InitializeGraphics(void*, size_t)
    "_ZN2nv16FinalizeGraphics",              // nv::FinalizeGraphics()
};
constexpr size_t kMaxGraphicsStops = 8;

// What the host callbacks need while the process runs. One per runtime.
struct Process {
    rt::Kernel* kernel = nullptr;
    const rc::LinkedImage* image = nullptr;
    BootResult* result = nullptr;
    uint64_t boot_base = 0;
    BootPages pages;
    uint64_t start_va = 0;
    uint64_t argument_va = 0;
    std::vector<uint64_t> initializers;  // DT_INIT addresses in call order
    std::vector<std::string> initializer_modules;
    std::vector<std::string> graphics_names;  // per stop thunk
    GraphicsMode graphics = GraphicsMode::Stop;
    std::array<std::atomic<bool>, kMaxGraphicsStops> skip_logged{};
    // GraphicsMode::Hle: the HLE's nvnQueuePresentTexture, wrapped to count
    // frames and end the process at the frame limit or on request.
    LiftedFn present_fn = nullptr;
    std::atomic<uint64_t> frames{0};
    uint64_t frame_limit = 0;
    const std::atomic<bool>* stop_requested = nullptr;
    std::function<void(uint64_t)> on_present;
    // Present pacing (NvnOptions::pace_hz): the HLE's present interval, paced.
    gpu::nvn::Hle* nvn = nullptr;
    bool paced = false;
    std::mutex pacer_mutex;
    FramePacer pacer;
    std::atomic<uint64_t> paced_wait_ns{0};
    uint64_t last_vsync = 0;  // virtual clock: vsync index of the last present
    // LGP_POKE_FRAME: written again after every present (diagnosis: does the
    // game use a field we keep overriding?).
    std::vector<rt::ValuePatch> frame_pokes;
    // LGP_WATCH_FROM_FRAME: write watches log from this present on (0 = always).
    uint64_t watch_from_frame = 0;
    bool exception_handler_ready = false;
    bool initializers_done = false;
    std::mutex mutex;
};
Process* g_process = nullptr;

void set_halt(CpuState* ctx, uint32_t reason, uint64_t info) {
    if (ctx->halt == kHaltNone) {
        ctx->halt = reason;
        ctx->halt_info = info;
    }
}

// BL from host code into guest code at va. Guest code returns by RET, which
// returns from the host function; x30 is restored for the caller. Nothing with
// a destructor may live across this call: a guest fault unwinds with longjmp
// to the scheduler (run_lifted).
bool call_guest(CpuState* ctx, uint64_t va) {
    const LiftedFn fn = rt::lookup_function(va);
    if (fn == nullptr) {
        set_halt(ctx, kHaltBadJump, va);
        ctx->pc = va;
        return false;
    }
    const uint64_t lr = ctx->x[30];
    ctx->x[30] = g_process->boot_base + kThunkReturn;
    ctx->pc = va;
    fn(ctx);
    ctx->x[30] = lr;
    return ctx->halt == kHaltNone;
}

// Main thread entry for an SDK title: what rtld does after relocating
// (oss-rtld __rtld_start_app): nn::init::Start(main thread handle, argument
// area, notify_exception_handler_ready, call_initializator). It never
// returns on hardware; it ends with svcExitProcess.
void entry_thunk(CpuState* ctx) {
    Process& p = *g_process;
    const uint64_t thread_handle = ctx->x[1] & 0xFFFFFFFFu;
    rt::log(LogLevel::Info, "boot: nn::init::Start at 0x%" PRIx64 " (main thread handle 0x%" PRIx64 ")", p.start_va,
            thread_handle);
    ctx->x[0] = thread_handle;
    ctx->x[1] = p.argument_va;
    ctx->x[2] = p.boot_base + kThunkNotify;
    ctx->x[3] = p.boot_base + kThunkInitialize;
    if (!call_guest(ctx, p.start_va))
        return;
    rt::log(LogLevel::Warn, "boot: nn::init::Start returned, ending the process");
    rt::svc_from_lifted(ctx, kSvcExitProcess);
}

void notify_thunk(CpuState*) {
    rt::log(LogLevel::Info, "boot: SDK reports its exception handler ready");
    g_process->exception_handler_ready = true;
}

// rtld's __rtld_modules_init: DT_INIT of every module, last loaded first.
void initialize_thunk(CpuState* ctx) {
    Process& p = *g_process;
    if (p.initializers_done) {
        rt::log(LogLevel::Warn, "boot: module initializers requested twice, ignored");
        return;
    }
    p.initializers_done = true;
    // Callee-saved registers survive the calls (AAPCS64 in the guest code);
    // the caller of this thunk expects nothing else.
    for (size_t i = 0; i < p.initializers.size(); ++i) {
        rt::log(LogLevel::Info, "boot: initializer of %s at 0x%" PRIx64, p.initializer_modules[i].c_str(),
                p.initializers[i]);
        if (!call_guest(ctx, p.initializers[i]))
            return;
    }
}

// nn::ro's lookup_global_auto(const char* name): symbol address in the
// auto-loaded modules, 0 if there is none.
void lookup_thunk(CpuState* ctx) {
    Process& p = *g_process;
    char name[512];
    size_t n = 0;
    for (; n + 1 < sizeof(name); ++n) {
        if (!p.kernel->memory().copy_from_user(&name[n], ctx->x[0] + n, 1)) {
            set_halt(ctx, kHaltMemFault, ctx->x[0] + n);
            return;
        }
        if (name[n] == '\0')
            break;
    }
    name[n] = '\0';
    const std::optional<uint64_t> va = p.image->symbol_va(name);
    rt::log(LogLevel::Debug, "boot: nn::ro lookup of %s -> 0x%" PRIx64, name, va.value_or(0));
    ctx->x[0] = va.value_or(0);
}

void return_thunk(CpuState* ctx) {
    // Guest code jumped to the marker x30 instead of returning (tail call to
    // x30 after the stack was unwound by hand). Not expected from SDK code.
    rt::log(LogLevel::Error, "boot: guest code branched to the boot return marker");
    set_halt(ctx, kHaltBadJump, ctx->pc);
}

void graphics_stop(CpuState* ctx, size_t index) {
    Process& p = *g_process;
    if (p.graphics == GraphicsMode::Hle) {
        // nv:: driver setup with the NVN-HLE: nothing to set up, return to
        // the caller (all of them return void).
        if (!p.skip_logged[index].exchange(true))
            rt::log(LogLevel::Info, "boot: %s skipped (NVN-HLE, the driver does not run)",
                    p.graphics_names[index].c_str());
        return;
    }
    {
        std::lock_guard lock(p.mutex);
        if (p.result->graphics_stop.empty()) {
            p.result->graphics_stop = p.graphics_names[index];
            p.result->graphics_stop_caller = ctx->x[30];
            rt::log(LogLevel::Info, "boot: first graphics call reached: %s (from 0x%" PRIx64 "), ending the process",
                    p.graphics_names[index].c_str(), ctx->x[30]);
        }
    }
    rt::svc_from_lifted(ctx, kSvcExitProcess);
}

template <size_t I>
void graphics_thunk(CpuState* ctx) {
    graphics_stop(ctx, I);
}

template <size_t... I>
constexpr std::array<LiftedFn, sizeof...(I)> make_graphics_thunks(std::index_sequence<I...>) {
    return {&graphics_thunk<I>...};
}
constexpr auto kGraphicsThunks = make_graphics_thunks(std::make_index_sequence<kMaxGraphicsStops>{});

// Debugging: LGP_TRACE_FN="0xVA,0xVA,..." logs the first calls of these lifted
// functions (x0-x3, caller) and their return value (x0).
constexpr size_t kMaxTracedFns = 16;
struct TracedFn {
    uint64_t va = 0;
    LiftedFn fn = nullptr;
    std::atomic<uint32_t> calls{0};
};
std::array<TracedFn, kMaxTracedFns> g_traced;

template <size_t I>
void traced_fn(CpuState* ctx) {
    TracedFn& t = g_traced[I];
    const bool log = t.calls.fetch_add(1) < 32;
    const uint64_t lr = ctx->x[30];
    if (log)
        rt::log(LogLevel::Warn, "trace: 0x%" PRIx64 "(0x%" PRIx64 ", 0x%" PRIx64 ", 0x%" PRIx64 ", 0x%" PRIx64
                ") from 0x%" PRIx64, t.va, ctx->x[0], ctx->x[1], ctx->x[2], ctx->x[3], lr);
    t.fn(ctx);
    if (log)
        rt::log(LogLevel::Warn, "trace: 0x%" PRIx64 " -> 0x%" PRIx64 "%s", t.va, ctx->x[0],
                ctx->halt != kHaltNone ? " (halted)" : "");
}

template <size_t... I>
constexpr std::array<LiftedFn, sizeof...(I)> make_traced(std::index_sequence<I...>) {
    return {&traced_fn<I>...};
}
constexpr auto kTracedFns = make_traced(std::make_index_sequence<kMaxTracedFns>{});

void install_function_traces() {
    const char* env = std::getenv("LGP_TRACE_FN");
    if (env == nullptr)
        return;
    std::string spec = env;
    size_t n = 0;
    for (size_t at = 0; at < spec.size() && n < kMaxTracedFns;) {
        const size_t end = std::min(spec.find(',', at), spec.size());
        const uint64_t va = std::strtoull(spec.substr(at, end - at).c_str(), nullptr, 0);
        at = end + 1;
        const LiftedFn fn = rt::lookup_function(va);
        if (fn == nullptr) {
            rt::log(LogLevel::Warn, "trace: no lifted function at 0x%" PRIx64, va);
            continue;
        }
        g_traced[n].va = va;
        g_traced[n].fn = fn;
        const LiftedEntry e = {va, kTracedFns[n]};
        rt::register_functions(&e, 1);
        ++n;
    }
}

// Diagnosis for game mods before they have a mod file (docs/analysis/50):
//   LGP_PATCH_IMM=VA=VALUE,...  the MOVZ/MOVN/MOVK at VA yields VALUE (VA must
//                               be a patch site of the lifted code, lgp_lift --patch-site)
//   LGP_POKE=VA=VALUE,...       VALUE into guest memory at VA after loading (rodata too)
// VALUE as in rt::parse_value_patches (0x…, decimal, f:21/9, d:…).
void install_value_patches(rt::GuestMemory& memory, uint64_t* watch_from_frame) {
    struct Var {
        const char* name;
        bool immediate;
    };
    for (const Var var : {Var{"LGP_PATCH_IMM", true}, Var{"LGP_POKE", false}}) {
        const char* env = std::getenv(var.name);
        if (env == nullptr || *env == '\0')
            continue;
        std::vector<rt::ValuePatch> patches;
        std::string error;
        if (!rt::parse_value_patches(env, &patches, &error)) {
            rt::log(LogLevel::Warn, "%s: %s, ignored", var.name, error.c_str());
            continue;
        }
        for (const rt::ValuePatch& p : patches) {
            const bool ok = var.immediate ? rt::set_immediate_patch(memory, p.va, p.value, &error)
                                          : rt::poke_guest(memory, p.va, p.value, p.bytes, &error);
            if (ok)
                rt::log(LogLevel::Warn, "%s: 0x%" PRIx64 " = 0x%" PRIx64, var.name, p.va, p.value);
            else
                rt::log(LogLevel::Warn, "%s: 0x%" PRIx64 ": %s", var.name, p.va, error.c_str());
        }
    }
    // Register logs at patch sites (diagnosis: which object does a function get?).
    if (const char* env = std::getenv("LGP_PATCH_LOG"); env != nullptr && *env != '\0') {
        std::vector<rt::WriteWatch> sites;  // VA[:N], N passes (default 8)
        std::string error;
        if (!rt::parse_write_watches(env, &sites, &error))
            rt::log(LogLevel::Warn, "LGP_PATCH_LOG: %s, ignored", error.c_str());
        else
            for (const rt::WriteWatch& s : sites) {
                rt::set_register_log(s.va, static_cast<unsigned>(s.size));
                rt::log(LogLevel::Warn, "LGP_PATCH_LOG: 0x%" PRIx64 ", %" PRIu64 " passes", s.va, s.size);
            }
    }
    // Write watchpoints (diagnosis: who fills a field?).
    if (const char* env = std::getenv("LGP_WATCH_WRITE"); env != nullptr && *env != '\0') {
        std::vector<rt::WriteWatch> watches;
        std::string error;
        if (!rt::parse_write_watches(env, &watches, &error))
            rt::log(LogLevel::Warn, "LGP_WATCH_WRITE: %s, ignored", error.c_str());
        else
            for (const rt::WriteWatch& w : watches) {
                if (rt::add_write_watch(memory, w.va, w.size, &error))
                    rt::log(LogLevel::Warn, "LGP_WATCH_WRITE: 0x%" PRIx64 " size %" PRIu64, w.va, w.size);
                else
                    rt::log(LogLevel::Warn, "LGP_WATCH_WRITE: 0x%" PRIx64 ": %s", w.va, error.c_str());
            }
        const char* zero = std::getenv("LGP_WATCH_SKIP_ZERO");
        const char* lines = std::getenv("LGP_WATCH_MAX_LINES");
        const bool skip_zero = zero != nullptr && *zero != '\0' && *zero != '0';
        rt::set_write_watch_filter(skip_zero, lines != nullptr ? std::strtoull(lines, nullptr, 0) : 0);
        if (const char* from = std::getenv("LGP_WATCH_FROM_FRAME"); from != nullptr && *from != '\0')
            *watch_from_frame = std::strtoull(from, nullptr, 0);
        rt::set_write_watch_armed(*watch_from_frame == 0);
        if (skip_zero || *watch_from_frame != 0 || lines != nullptr)
            rt::log(LogLevel::Warn, "LGP_WATCH_WRITE: %sfrom frame %" PRIu64 ", at most %" PRIu64 " lines",
                    skip_zero ? "no zero stores, " : "", *watch_from_frame, rt::write_watch_max_lines());
    }
}

// Virtual clock: a present returns at the next vsync, like a swap with the
// window's present interval on the console. Vsync k is at k / kVirtualVsyncHz s.
void virtual_vsync(Process& p, uint64_t window) {
    rt::GuestClock& clock = p.kernel->clock();
    if (!clock.is_virtual())
        return;
    const int32_t interval = p.nvn != nullptr ? std::max(p.nvn->present_interval(window), 1) : 1;
    constexpr uint64_t kNsPerSecond = 1'000'000'000;
    const uint64_t now = clock.now_ns();
    const uint64_t at_or_after_now = (now * kVirtualVsyncHz + kNsPerSecond - 1) / kNsPerSecond;
    const uint64_t vsync = std::max(at_or_after_now, p.last_vsync + static_cast<uint64_t>(interval));
    // Diagnosis LGP_VCLOCK_LOG=FROM[:COUNT]: the guest time a frame used before
    // its present (svc and counter-read costs, idle jumps), to tell two runs
    // apart whose guest work differs (e.g. more draws) from a change in logic.
    static const std::pair<uint64_t, uint64_t> log_range = [] {
        const char* env = std::getenv("LGP_VCLOCK_LOG");
        if (env == nullptr || *env == '\0') return std::pair<uint64_t, uint64_t>{~uint64_t{0}, 0};
        char* end = nullptr;
        const uint64_t from = std::strtoull(env, &end, 0);
        const uint64_t count = *end == ':' ? std::strtoull(end + 1, nullptr, 0) : 600;
        return std::pair{from, count};
    }();
    static std::atomic<uint64_t> presents{0};
    if (const uint64_t n = presents.fetch_add(1, std::memory_order_relaxed) + 1;
        n >= log_range.first && n - log_range.first < log_range.second) {
        const uint64_t last = p.last_vsync * kNsPerSecond / kVirtualVsyncHz;
        rt::log(LogLevel::Info, "vclock: present %" PRIu64 " at %" PRIu64 " ns, %" PRIu64 " ns after vsync %" PRIu64
                " (next %" PRIu64 ")", n, now, now >= last ? now - last : 0, p.last_vsync, vsync);
    }
    p.last_vsync = vsync;
    clock.advance_to(vsync * kNsPerSecond / kVirtualVsyncHz);
    p.kernel->scheduler().on_clock_advanced();
}

// LGP_STALL_PEEK (stall_peek_lines): under the kernel's memory lock, since a
// stall stops presents, not memory SVCs. Only try it: a thread that hangs
// while holding the lock must not hang the report as well.
void log_stall_peeks(rt::Kernel& kernel) {
    const char* spec = std::getenv("LGP_STALL_PEEK");
    if (spec == nullptr || *spec == '\0')
        return;
    std::unique_lock lock(kernel.memory_mutex(), std::try_to_lock);
    if (!lock.owns_lock()) {
        rt::log(LogLevel::Warn, "  peek skipped: guest memory map busy");
        return;
    }
    const auto read64 = [&](uint64_t va, uint64_t* out) {
        const uint8_t* p = kernel.memory().host_ptr(va, 8);
        if (p == nullptr)
            return false;
        std::memcpy(out, p, 8);
        return true;
    };
    for (const std::string& line : stall_peek_lines(spec, read64))
        rt::log(LogLevel::Warn, "  %s", line.c_str());
}

void log_stall(rt::Kernel& kernel, hle::AudioOutState& audout, const StallReport& stall) {
    rt::log(LogLevel::Warn, "boot: no present for %.1f s after frame %" PRIu64 " (guest not paused); state:",
            stall.seconds(std::chrono::steady_clock::now()), stall.frames());
    for (const std::string& line : kernel.scheduler().thread_report())
        rt::log(LogLevel::Warn, "  %s", line.c_str());
    const std::vector<std::string> requests = hle::requests_in_flight(100'000'000);
    if (requests.empty())
        rt::log(LogLevel::Warn, "  no HLE request older than 0.1 s in flight");
    for (const std::string& line : requests)
        rt::log(LogLevel::Warn, "  request %s", line.c_str());
    const std::vector<std::string> categories = rt::time_stats_current();
    if (categories.empty())
        rt::log(LogLevel::Warn, "  (what each thread is inside: needs --time-stats)");
    for (const std::string& line : categories)
        rt::log(LogLevel::Warn, "  %s", line.c_str());
    for (const std::string& line : audout.report())
        rt::log(LogLevel::Warn, "  %s", line.c_str());
    for (const std::string& line : MovieHle::report())
        rt::log(LogLevel::Warn, "  %s", line.c_str());
    log_stall_peeks(kernel);
}

void present_thunk(CpuState* ctx) {
    Process& p = *g_process;
    const uint64_t window = ctx->x[1];  // nvnQueuePresentTexture(queue, window, texture index)
    p.present_fn(ctx);
    if (ctx->halt != kHaltNone)
        return;
    const uint64_t n = p.frames.fetch_add(1) + 1;
    if (p.watch_from_frame != 0 && n == p.watch_from_frame) {
        rt::set_write_watch_armed(true);
        rt::log(LogLevel::Warn, "watch: armed at frame %" PRIu64, n);
    }
    for (const rt::ValuePatch& poke : p.frame_pokes) {
        std::string error;
        if (!rt::poke_guest(p.kernel->memory(), poke.va, poke.value, poke.bytes, &error) && n == 1)
            rt::log(LogLevel::Warn, "LGP_POKE_FRAME: 0x%" PRIx64 ": %s", poke.va, error.c_str());
    }
    if (p.on_present)
        p.on_present(n);
    virtual_vsync(p, window);
    if (p.paced && p.nvn != nullptr) {
        // Wait like the console's display would, as svcSleepThread so the
        // scheduler lets other guest threads run meanwhile.
        const uint64_t tick = rt::system_tick();
        const auto now_ns = int64_t(tick / rt::kCounterFrequency * 1'000'000'000ull +
                                    tick % rt::kCounterFrequency * 1'000'000'000ull / rt::kCounterFrequency);
        int64_t wait = 0;
        {
            std::lock_guard lock(p.pacer_mutex);
            wait = p.pacer.after_present(now_ns, p.nvn->last_present_interval());
        }
        if (wait > 0) {
            p.paced_wait_ns.fetch_add(uint64_t(wait), std::memory_order_relaxed);
            const uint64_t x0 = ctx->x[0];
            ctx->x[0] = uint64_t(wait);
            rt::svc_from_lifted(ctx, rt::kSvcSleepThread);
            if (ctx->halt != kHaltNone)
                return;
            ctx->x[0] = x0;
        }
    }
    // Progress for long runs (powers of two, so a stuck game shows its last count).
    if ((n & (n - 1)) == 0)
        rt::log(LogLevel::Info, "boot: %" PRIu64 " frames presented", n);
    if (p.frame_limit != 0 && n == p.frame_limit) {
        rt::log(LogLevel::Info, "boot: frame limit %" PRIu64 " reached, ending the process", n);
        p.result->frame_limit_reached = true;
        rt::svc_from_lifted(ctx, kSvcExitProcess);
    } else if (p.stop_requested != nullptr && p.stop_requested->load()) {
        rt::log(LogLevel::Info, "boot: stop requested (window closed) after %" PRIu64 " frames", n);
        rt::svc_from_lifted(ctx, kSvcExitProcess);
    }
}

const rc::Segment* find_segment(const rc::Module& m, rc::SegmentKind kind) {
    for (const rc::Segment& s : m.segments)
        if (s.kind == kind)
            return &s;
    return nullptr;
}

// Guest memory for the GPU backend (NvnOptions::attach_memory): the kernel's
// host mapping, without guest permission checks (GPU access to memory pools).
class KernelMemoryView final : public gpu::nvn::GuestMemoryView {
public:
    explicit KernelMemoryView(rt::Kernel& kernel) : kernel_(kernel) {}
    bool read(uint64_t va, void* dst, uint64_t size) const override {
        const uint8_t* p = kernel_.memory().host_ptr(va, size);
        if (p == nullptr)
            return false;
        std::memcpy(dst, p, size);
        return true;
    }
    bool equal(uint64_t va, const void* data, uint64_t size) const override {
        const uint8_t* p = kernel_.memory().host_ptr(va, size);
        return p != nullptr && std::memcmp(p, data, size) == 0;
    }
    const uint8_t* view(uint64_t va, uint64_t size) const override { return kernel_.memory().host_ptr(va, size); }
    bool write(uint64_t va, const void* src, uint64_t size) const override {
        uint8_t* p = kernel_.memory().host_ptr(va, size);
        if (p == nullptr)
            return false;
        std::memcpy(p, src, size);
        return true;
    }

private:
    rt::Kernel& kernel_;
};

// Text RX, ro R (Code), data+bss RW (CodeData), like Horizon's loader.
bool map_module(rt::Kernel& k, const rc::Module& m, std::string* error) {
    const rc::Segment* ro = find_segment(m, rc::SegmentKind::Ro);
    const rc::Segment* data = find_segment(m, rc::SegmentKind::Data);
    if (ro == nullptr || data == nullptr) {
        *error = "module " + m.name + " lacks a ro or data segment";
        return false;
    }
    const uint64_t end = page_up(m.image.size());
    struct Part {
        uint64_t off, end;
        rt::MemState state;
        uint8_t perm;
    } parts[] = {{0, ro->offset, rt::MemState::Code, rt::kPermRX},
                 {ro->offset, data->offset, rt::MemState::Code, rt::kPermR},
                 {data->offset, end, rt::MemState::CodeData, rt::kPermRW}};
    for (const Part& part : parts) {
        if (part.end <= part.off)
            continue;
        if (k.map_owned(m.base + part.off, part.end - part.off, part.state, part.perm).failed()) {
            char buf[160];
            std::snprintf(buf, sizeof(buf), "cannot map %s at 0x%" PRIx64 "+0x%" PRIx64 " (outside the code region?)",
                          m.name.c_str(), m.base + part.off, part.end - part.off);
            *error = buf;
            return false;
        }
    }
    // Through the host pointer: text and ro are not user-writable.
    std::memcpy(k.memory().host_ptr(m.base, m.image.size()), m.image.data(), m.image.size());
    return true;
}

// The layout the code was lifted for must be the one loaded now.
bool check_layout(const rc::LinkedImage& image, const LiftedCode& code, std::string* error) {
    if (code.module_count == 0) {
        *error = "the lifted code has no module table (generated by an older lgp_lift?)";
        return false;
    }
    if (code.module_count != image.modules.size()) {
        *error = "the code was lifted from " + std::to_string(code.module_count) + " modules, " +
                 std::to_string(image.modules.size()) + " were given";
        return false;
    }
    for (size_t i = 0; i < code.module_count; ++i) {
        const LiftedModule& want = code.modules[i];
        const rc::Module& have = image.modules[i];
        const std::string id = have.hex_module_id();
        if (have.name != want.name || have.base != want.base || have.image.size() != want.size ||
            id != want.module_id) {
            char buf[512];
            std::snprintf(buf, sizeof(buf),
                          "module %zu differs from the lifted layout: lifted %s at 0x%" PRIx64 " size 0x%" PRIx64
                          " id %.16s..., loaded %s at 0x%" PRIx64 " size 0x%" PRIx64 " id %.16s...",
                          i, want.name, want.base, want.size, want.module_id, have.name.c_str(), have.base,
                          static_cast<uint64_t>(have.image.size()), id.c_str());
            *error = buf;
            return false;
        }
    }
    return true;
}

std::optional<uint64_t> find_init_start(const rc::LinkedImage& image) {
    for (const rc::Module& m : image.modules)
        for (const rc::Symbol& s : m.dynsym)
            if (s.defined() && s.type == rc::kSttFunc && s.name.rfind(kInitStartPrefix, 0) == 0)
                return m.base + s.value;
    return std::nullopt;
}

// Defined function symbols matching one of the graphics names (exact for
// C names, prefix for mangled C++ names).
std::vector<std::pair<std::string, uint64_t>> find_graphics_entries(const rc::LinkedImage& image) {
    std::vector<std::pair<std::string, uint64_t>> out;
    for (const rc::Module& m : image.modules)
        for (const rc::Symbol& s : m.dynsym) {
            if (!s.defined() || s.type != rc::kSttFunc)
                continue;
            for (const std::string& g : kGraphicsSymbols) {
                const bool match = g.rfind("_Z", 0) == 0 ? s.name.rfind(g, 0) == 0 : s.name == g;
                if (match && out.size() < kMaxGraphicsStops)
                    out.emplace_back(s.name, m.base + s.value);
            }
        }
    return out;
}

// Points the nn::ro globals rtld fills in at the boot program's (empty)
// module lists and lookup function, in every module that has them.
void bind_ro_globals(rt::Kernel& k, const rc::LinkedImage& image, uint64_t data_va, uint64_t lookup_va) {
    const std::pair<const char*, uint64_t> globals[] = {
        {"_ZN2nn2ro6detail15g_pAutoLoadListE", data_va + kAutoLoadList},
        {"_ZN2nn2ro6detail17g_pManualLoadListE", data_va + kManualLoadList},
        {"_ZN2nn2ro6detail14g_pRoDebugFlagE", data_va + kRoDebugFlag},
        {"_ZN2nn2ro6detail34g_pLookupGlobalAutoFunctionPointerE", lookup_va},
    };
    for (const rc::Module& m : image.modules)
        for (const rc::Symbol& s : m.dynsym)
            for (const auto& [name, value] : globals)
                if (s.defined() && s.name == name) {
                    k.memory().copy_to_user(m.base + s.value, &value, sizeof(value));
                    rt::log(LogLevel::Debug, "boot: %s in %s -> 0x%" PRIx64, name, m.name.c_str(), value);
                }
}

// --- Native start (BootOptions::native) --------------------------------------

constexpr uint64_t kNativePage = rc::kNativePageSize;
uint64_t native_up(uint64_t v) { return (v + kNativePage - 1) & ~(kNativePage - 1); }

// The standard layout moved to `base` (guest address == host address).
rt::Layout native_layout(uint64_t base, uint64_t total_memory_size) {
    rt::Layout l = rt::Layout::standard(total_memory_size);
    const uint64_t delta = base - l.code.base;
    for (rt::Region* r : {&l.code, &l.stack, &l.alias, &l.misc, &l.heap})
        r->base += delta;
    return l;
}

// Native code (module text, patch area) for the resolver: guest threads and
// host calls into guest code start there through rt::native::run.
std::vector<std::pair<uint64_t, uint64_t>> g_native_code;
LiftedFn resolve_native(uint64_t va) {
    for (const auto& [begin, end] : g_native_code)
        if (va >= begin && va < end)
            return &rt::native::run;
    return nullptr;
}

// What the native start keeps from the rewrite until the host calls are in.
struct NativeState {
    rc::NativePatch patch;
    std::vector<std::pair<uint64_t, uint64_t>> code;      // ranges to make R+X at the end
    std::vector<std::pair<uint64_t, uint64_t>> branches;  // where host-call branches may go
};

// --- Native rewrite cache (BootOptions::Native::cache) ---

std::vector<uint8_t> read_binary_file(const std::string& path) {
    std::ifstream f(std::filesystem::path(path), std::ios::binary);
    if (!f)
        return {};
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

std::string read_text_file(const std::string& path) {
    if (path.empty())
        return {};
    const std::vector<uint8_t> b = read_binary_file(path);
    return std::string(b.begin(), b.end());
}

// Next to the target, then renamed over it: a run that ends half way leaves
// the old file or none, never a torn one.
bool write_file_atomically(const std::string& path, const std::vector<uint8_t>& bytes, std::string* error) {
    const std::filesystem::path target(path);
    std::filesystem::path tmp = target;
    tmp += ".tmp";
    std::error_code ec;
    if (target.has_parent_path())
        std::filesystem::create_directories(target.parent_path(), ec);
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!f) {
            *error = "cannot write " + tmp.string();
            return false;
        }
    }
    std::filesystem::rename(tmp, target, ec);
    if (ec) {
        *error = ec.message();
        std::filesystem::remove(tmp, ec);
        return false;
    }
    return true;
}

// A cached patch must fit the image it is applied to: the same modules at
// the same bases, every word inside a module's text, the area where asked.
// (The key covers all of it; this guards against a key collision.)
bool native_record_fits(const rc::LinkedImage& image, const rc::NativeRewriteRecord& rec, uint64_t area_base,
                        std::string* why) {
    const rc::NativePatch& p = rec.patch;
    if (p.area_base != area_base || p.modules.size() > image.modules.size()) {
        *why = "other layout";
        return false;
    }
    for (const rc::NativeModulePatch& mp : p.modules) {
        const auto m = std::find_if(image.modules.begin(), image.modules.end(),
                                    [&](const rc::Module& x) { return x.name == mp.module && x.base == mp.base; });
        if (m == image.modules.end()) {
            *why = "other modules";
            return false;
        }
        for (const auto& [va, word] : mp.words)
            if (!image.in_text(va) || image.module_at(va) != &*m) {
                *why = "a word outside its module's text";
                return false;
            }
    }
    return true;
}

// The rewrite in memory (lgp_recomp native-rewrite without files): words into
// the mapped modules, the patch area behind the boot pages, the key pages as
// code. Nothing is executable yet.
bool native_rewrite(rt::Kernel& k, const rc::LinkedImage& image, const std::vector<rc::Module>& mapped,
                    const BootOptions& options, const Process& process, NativeState* st, BootResult* result) {
    const auto t0 = std::chrono::steady_clock::now();
    rt::GuestMemory& mem = k.memory();
    const rt::Layout& l = mem.layout();
    rc::NativePatchOptions po;
    po.area_base = process.boot_base + process.pages.end();
    BootResult::NativeStats& n = result->native;
    const std::string& cache = options.native.cache;
    uint64_t key = 0;
    rc::NativeRewriteRecord rec;
    bool hit = false;
    if (!cache.empty()) {
        key = rc::native_cache_key(image, read_text_file(options.native.config), po.area_base,
                                   options.native.cache_salt);
        std::string why = "no file";
        const std::vector<uint8_t> bytes = read_binary_file(cache);
        hit = !bytes.empty() && rc::decode_native_cache(bytes, key, &rec, &why) &&
              native_record_fits(image, rec, po.area_base, &why);
        if (!hit)
            rt::log(LogLevel::Info, "boot: native cache %s not used (%s), rewriting", cache.c_str(), why.c_str());
    }
    if (!hit) {
        rec = {};
        rc::AnalysisConfig ac;
        if (!options.native.config.empty())
            ac = rc::rebase_config(rc::load_config_file(options.native.config), rc::LinkOptions{}.first_base,
                                   options.native.base);
        const rc::AnalysisResult analysis = rc::analyze(image, ac);
        const std::vector<rc::NativeRewrite> plan = rc::plan_native_rewrite(image, analysis);
        rec.patch = rc::build_native_patch(image, plan, po);
        for (const rc::NativeRewrite& r : plan) {
            rec.x18_unpatched += r.x18_unpatched.size();
            rec.gate_unpatched += r.gate_exclusive + r.gate_x18;
        }
        if (!cache.empty()) {
            std::string error;
            if (write_file_atomically(cache, rc::encode_native_cache(rec, key), &error))
                rt::log(LogLevel::Info, "boot: native cache %s written", cache.c_str());
            else
                rt::log(LogLevel::Warn, "boot: native cache %s not written: %s", cache.c_str(), error.c_str());
        }
    }
    n.cache = cache.empty() ? BootResult::NativeStats::Cache::Off
                            : hit ? BootResult::NativeStats::Cache::Hit : BootResult::NativeStats::Cache::Miss;
    n.x18_unpatched = rec.x18_unpatched;
    n.gate_unpatched = rec.gate_unpatched;
    st->patch = std::move(rec.patch);
    const rc::NativePatch& p = st->patch;
    if (p.area_base + p.area.size() > l.code.end()) {
        result->error = "native start: the patch area does not fit into the code region";
        return false;
    }
    for (const rc::NativeModulePatch& mp : p.modules)
        for (const auto& [va, word] : mp.words)
            std::memcpy(mem.host_ptr(va, 4), &word, 4);
    if (k.map_owned(p.area_base, p.area.size(), rt::MemState::Code, rt::kPermRX).failed()) {
        result->error = "native start: cannot map the patch area";
        return false;
    }
    std::memcpy(mem.host_ptr(p.area_base, p.area.size()), p.area.data(), p.area.size());
    rt::native::install_gate(p.host_fn_slot);
    st->code.emplace_back(p.area_base, p.code_size);
    g_native_code.emplace_back(p.area_base, p.area_base + p.code_size);
    // Text R+X, data RW: they must not share a 16 KiB host page. A module
    // where they do keeps its text writable and not executable: rtld (4 KiB
    // segments, text, ro and data in one 16 KiB page), whose part the boot
    // program plays, so its code never runs. A call into such text faults
    // and ends the thread as a bad jump to that address.
    for (const rc::Module& m : mapped) {
        const rc::Segment* ro = find_segment(m, rc::SegmentKind::Ro);
        const rc::Segment* data = find_segment(m, rc::SegmentKind::Data);
        rt::log(LogLevel::Info, "boot: native: %s at 0x%" PRIx64 " size 0x%zx", m.name.c_str(), m.base, m.image.size());
        if (native_up(m.base + ro->offset) > m.base + data->offset) {
            rt::log(LogLevel::Warn, "boot: native: %s has its data on a 16 KiB page of its code, its code stays "
                    "not executable", m.name.c_str());
            ++result->native.modules_not_executable;
        } else {
            st->code.emplace_back(m.base, ro->offset);
        }
        st->branches.emplace_back(m.base, m.base + ro->offset);
        g_native_code.emplace_back(m.base, m.base + ro->offset);
    }
    // The key pages: host-call branches (filled in before the run).
    for (const uint64_t off : {uint64_t{0}, process.pages.nvn_thunks(), process.pages.import_shims()}) {
        const uint64_t va = process.boot_base + off;
        if (k.map_owned(va, process.pages.unit, rt::MemState::Code, rt::kPermRX).failed()) {
            result->error = "native start: cannot map the boot key pages";
            return false;
        }
        st->code.emplace_back(va, process.pages.unit);
        st->branches.emplace_back(va, va + process.pages.unit);
    }
    n.used = true;
    n.renamed = p.sites_renamed;
    n.gate_stubs = p.gate_stubs;
    n.spill_stubs = p.spill_stubs;
    for (const rc::NativeModulePatch& mp : p.modules)
        n.words += mp.words.size();
    n.rewrite_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
    rt::log(LogLevel::Info,
            "boot: native rewrite in %" PRIu64 " ms: %" PRIu64 " words, %" PRIu64 " renamed, %" PRIu64
            " gate stubs, %" PRIu64 " stack-slot stubs; unpatched: %" PRIu64 " x18 sites, %" PRIu64 " gate sites",
            n.rewrite_ms, n.words, n.renamed, n.gate_stubs, n.spill_stubs, n.x18_unpatched, n.gate_unpatched);
    return true;
}

// --- Native code image (BootOptions::Native::image_out / image) ---
// "LGPNIMG1", u64 native base, u64 count, count x {u64 va, u64 len, u64
// offset} (little endian), then the pages: every range in whole 16 KiB pages
// at a 16 KiB aligned file offset, so the file maps as it is.
constexpr char kImageMagic[8] = {'L', 'G', 'P', 'N', 'I', 'M', 'G', '1'};
constexpr uint64_t kImageHeader = 24, kImageEntry = 24;

// The code ranges in whole 16 KiB pages, sorted, touching ones merged.
std::vector<std::pair<uint64_t, uint64_t>> native_image_ranges(const NativeState& st) {
    std::vector<std::pair<uint64_t, uint64_t>> r;
    for (const auto& [va, len] : st.code)
        if (len != 0)
            r.emplace_back(va & ~(kNativePage - 1), native_up(va + len));
    std::sort(r.begin(), r.end());
    std::vector<std::pair<uint64_t, uint64_t>> merged;
    for (const auto& x : r)
        if (!merged.empty() && x.first <= merged.back().second)
            merged.back().second = std::max(merged.back().second, x.second);
        else
            merged.push_back(x);
    return merged;
}

uint64_t image_data_start(size_t count) { return native_up(kImageHeader + kImageEntry * count); }

void put_u64(std::vector<uint8_t>& b, uint64_t at, uint64_t v) { std::memcpy(b.data() + at, &v, 8); }

bool write_native_image(const rt::GuestMemory& mem, const std::vector<std::pair<uint64_t, uint64_t>>& ranges,
                        uint64_t base, const std::string& path, uint64_t* bytes_out, std::string* error) {
    const uint64_t start = mem.layout().start();
    uint64_t off = image_data_start(ranges.size());
    std::vector<uint8_t> b(off, 0);
    std::memcpy(b.data(), kImageMagic, 8);
    put_u64(b, 8, base);
    put_u64(b, 16, ranges.size());
    for (size_t i = 0; i < ranges.size(); ++i) {
        const auto& [begin, end] = ranges[i];
        const uint64_t e = kImageHeader + kImageEntry * i;
        put_u64(b, e, begin);
        put_u64(b, e + 8, end - begin);
        put_u64(b, e + 16, off);
        const uint8_t* src = mem.host_block().base() + (begin - start);
        b.insert(b.end(), src, src + (end - begin));
        off += end - begin;
    }
    *bytes_out = off;
    return write_file_atomically(path, b, error);
}

// Checks that the image holds exactly these ranges with the bytes memory has
// now, then maps its pages over them. A host that maps no file executable
// (macOS: EPERM for files without a code signature) makes the checked memory
// executable instead (*mapped false), as without an image.
bool map_native_image(rt::GuestMemory& mem, const std::vector<std::pair<uint64_t, uint64_t>>& ranges,
                      uint64_t base, const std::string& path, uint64_t* bytes_out, bool* mapped,
                      std::string* error) {
    std::ifstream f(std::filesystem::path(path), std::ios::binary);
    if (!f) {
        *error = "cannot open " + path;
        return false;
    }
    std::vector<uint8_t> head(image_data_start(ranges.size()));
    f.read(reinterpret_cast<char*>(head.data()), static_cast<std::streamsize>(head.size()));
    uint64_t file_base = 0, count = 0;
    std::memcpy(&file_base, head.data() + 8, 8);
    std::memcpy(&count, head.data() + 16, 8);
    if (!f || std::memcmp(head.data(), kImageMagic, 8) != 0) {
        *error = path + " is no native code image";
        return false;
    }
    if (file_base != base || count != ranges.size()) {
        char buf[160];
        std::snprintf(buf, sizeof buf, "image for base 0x%" PRIx64 " with %" PRIu64 " ranges, this start: 0x%" PRIx64
                      " with %zu", file_base, count, base, ranges.size());
        *error = buf;
        return false;
    }
    const uint64_t start = mem.layout().start();
    std::vector<uint64_t> offsets(ranges.size());
    std::vector<uint8_t> chunk(uint64_t{1} << 20);
    uint64_t total = 0;
    for (size_t i = 0; i < ranges.size(); ++i) {
        const auto& [begin, end] = ranges[i];
        uint64_t va = 0, len = 0;
        const uint64_t e = kImageHeader + kImageEntry * i;
        std::memcpy(&va, head.data() + e, 8);
        std::memcpy(&len, head.data() + e + 8, 8);
        std::memcpy(&offsets[i], head.data() + e + 16, 8);
        if (va != begin || len != end - begin || offsets[i] % kNativePage != 0) {
            char buf[160];
            std::snprintf(buf, sizeof buf, "image range %zu is 0x%" PRIx64 "+0x%" PRIx64 ", this start has 0x%" PRIx64
                          "+0x%" PRIx64, i, va, len, begin, end - begin);
            *error = buf;
            return false;
        }
        f.seekg(static_cast<std::streamoff>(offsets[i]));
        const uint8_t* mine = mem.host_block().base() + (begin - start);
        for (uint64_t pos = 0; pos < len; pos += chunk.size()) {
            const size_t n = static_cast<size_t>(std::min<uint64_t>(chunk.size(), len - pos));
            f.read(reinterpret_cast<char*>(chunk.data()), static_cast<std::streamsize>(n));
            if (!f) {
                *error = path + " ends early";
                return false;
            }
            if (std::memcmp(chunk.data(), mine + pos, n) != 0) {
                size_t at = 0;
                while (chunk[at] == mine[pos + at])
                    ++at;
                char buf[160];
                std::snprintf(buf, sizeof buf, "image differs from this start at 0x%" PRIx64
                              " (another game, config, base, --graphics or program build?)", begin + pos + at);
                *error = buf;
                return false;
            }
        }
        total += len;
    }
    f.close();
    *bytes_out = total;
    *mapped = true;
    size_t done = 0;
    try {
        for (; done < ranges.size(); ++done)
            mem.host_block().map_code_file(ranges[done].first - start, ranges[done].second - ranges[done].first,
                                           path, offsets[done]);
        return true;
    } catch (const std::exception& ex) {
        if (done != 0) {  // part mapped, part not: the host refused one range only
            *error = ex.what();
            return false;
        }
        rt::log(LogLevel::Warn, "boot: native image %s not mapped (%s), its checked pages made executable instead",
                path.c_str(), ex.what());
    }
    *mapped = false;
    try {
        for (const auto& [begin, end] : ranges)
            mem.host_block().make_executable(begin - start, end - begin);
    } catch (const std::exception& ex) {
        *error = ex.what();
        return false;
    }
    return true;
}

// Host-call stubs for every registered (host) function, then everything
// native R+X (or mapped from the code image, or written to it). Last step
// before the run.
bool native_finish(rt::Kernel& k, NativeState& st, const BootOptions::Native& options, BootResult* result) {
    rt::GuestMemory& mem = k.memory();
    const rt::Layout& l = mem.layout();
    const std::vector<uint64_t> vas = rt::registered_functions();
    for (const uint64_t va : vas) {
        const bool ok = std::any_of(st.branches.begin(), st.branches.end(),
                                    [&](const auto& r) { return va >= r.first && va + 4 <= r.second; });
        if (!ok) {
            char buf[128];
            std::snprintf(buf, sizeof buf, "native start: host function at 0x%" PRIx64 " outside code", va);
            result->error = buf;
            return false;
        }
    }
    const uint64_t base = native_up(st.patch.area_base + st.patch.area.size());
    const rc::NativeHostCalls hc = rc::build_native_host_calls(vas, base, st.patch.gate_entry);
    const uint64_t size = native_up(std::max<uint64_t>(hc.code.size(), 4));
    if (base + size > l.code.end() || k.map_owned(base, size, rt::MemState::Code, rt::kPermRX).failed()) {
        result->error = "native start: cannot map the host-call stubs";
        return false;
    }
    std::memcpy(mem.host_ptr(base, hc.code.size()), hc.code.data(), hc.code.size());
    for (const auto& [va, word] : hc.words)
        std::memcpy(mem.host_ptr(va, 4), &word, 4);
    st.code.emplace_back(base, hc.code.size());
    result->native.host_calls = vas.size();
    rt::log(LogLevel::Info, "boot: native: %zu host functions behind host-call stubs", vas.size());
    BootResult::NativeStats& n = result->native;
    if (!options.image_out.empty() || !options.image.empty()) {
        const std::vector<std::pair<uint64_t, uint64_t>> ranges = native_image_ranges(st);
        n.image_ranges = ranges.size();
        std::string error;
        if (!options.image_out.empty()) {
            if (!write_native_image(mem, ranges, options.base, options.image_out, &n.image_bytes, &error)) {
                result->error = "native image: " + error;
                return false;
            }
            n.image = BootResult::NativeStats::Image::Written;
            rt::log(LogLevel::Info, "boot: native image %s written: %zu ranges, %" PRIu64 " bytes",
                    options.image_out.c_str(), ranges.size(), n.image_bytes);
            return true;
        }
        bool mapped = false;
        if (!map_native_image(mem, ranges, options.base, options.image, &n.image_bytes, &mapped, &error)) {
            result->error = "native image " + options.image + ": " + error;
            return false;
        }
        n.image = mapped ? BootResult::NativeStats::Image::Mapped : BootResult::NativeStats::Image::Checked;
        rt::log(LogLevel::Info, "boot: native code %s %s: %zu ranges, %" PRIu64 " bytes",
                mapped ? "mapped from" : "checked against", options.image.c_str(), ranges.size(), n.image_bytes);
        return true;
    }
    for (const auto& [va, len] : st.code)
        if (len != 0)
            mem.host_block().make_executable(va - l.start(), len);
    return true;
}

// Diagnosis: LGP_IGNORE_SYSTEMDATA=1 runs as if the game folder had no
// systemdata/ (stand-in font, profanity filter on the host), to compare with
// the firmware's data without a second copy of the game.
bool ignore_system_data() {
    const char* env = std::getenv("LGP_IGNORE_SYSTEMDATA");
    return env != nullptr && std::string_view(env) == "1";
}

// System data archive NgWord2 (the profanity filter's word lists).
constexpr uint64_t kNgWord2DataId = 0x0100000000000823;

// Whether the game storage holds a system data archive as fsp-srv opens it
// (/systemdata/<id>.bin or the extracted /systemdata/<id>/).
bool has_system_data(platform::IStorage* game, uint64_t data_id) {
    if (game == nullptr || ignore_system_data())
        return false;
    char path[48];
    std::snprintf(path, sizeof(path), "/systemdata/%016" PRIx64, data_id);
    platform::DirEntry e;
    if (game->stat(path, &e) == platform::StorageError::Ok && e.is_directory)
        return true;
    std::strcat(path, ".bin");
    return game->stat(path, &e) == platform::StorageError::Ok && !e.is_directory;
}

// The game storage of `options` (game_dir plus game_files); *game stays
// null without either.
bool open_game_storage(const BootOptions& options, std::unique_ptr<platform::IStorage>* game, std::string* error) {
    if (!options.game_dir.empty() && !(*game = platform::HostDirectoryStorage::open_root(options.game_dir, true))) {
        *error = "game directory " + options.game_dir + " does not exist";
        return false;
    }
    if (!options.game_files.empty()) {
        std::vector<std::pair<std::string, std::vector<platform::FileSegment>>> files;
        for (const auto& [name, spec] : options.game_files) {
            files.emplace_back(name, std::vector<platform::FileSegment>{});
            if (!platform::parse_file_segments(spec, &files.back().second, error)) {
                *error = "game file " + name + ": " + *error;
                return false;
            }
        }
        auto added = platform::AddedFilesStorage::create(std::move(*game), std::move(files), error);
        if (!added) {
            *error = "game file: " + *error;
            return false;
        }
        *game = std::move(added);
    }
    return true;
}

}  // namespace

const std::vector<std::string>& graphics_entry_symbols() { return kGraphicsSymbols; }

const char* start_kind_name(StartKind kind) {
    switch (kind) {
    case StartKind::Auto: return "auto";
    case StartKind::Sdk: return "sdk";
    case StartKind::Homebrew: return "homebrew";
    }
    return "?";
}

bool BootResult::ok() const {
    return started && (exit.reason == rt::ProcessExit::Reason::ExitProcess ||
                       exit.reason == rt::ProcessExit::Reason::AllThreadsExited);
}

BootResult boot(const BootOptions& options, const LiftedCode& code) {
    BootResult result;
    auto fail = [&](std::string why) {
        result.error = std::move(why);
        return result;
    };
    if (g_process != nullptr)
        return fail("another boot is running in this process");
    if (options.module_paths.empty())
        return fail("no modules given");
    if (options.graphics == GraphicsMode::Hle && options.nvn.backend == nullptr)
        return fail("GraphicsMode::Hle needs a renderer backend");
    const bool native = options.native.enabled;
    if (native) {
        if (!options.native.image_out.empty() && !options.native.image.empty())
            return fail("a native code image is either written or used, not both");
        // Writing the image runs no guest code: any host.
        if (!rt::native::available() && options.native.image_out.empty())
            return fail("the native start needs an AArch64 host");
        if (code.entry_count != 0)
            return fail("the native start runs without lifted code");
        if (!options.mods.enabled.empty() || options.mods.hook_log > 0)
            return fail("mods need the hook doors of lifted code, the native start has none");
        if (options.native.base % kNativePage != 0)
            return fail("the native base must be 16 KiB aligned");
    }

    // 1. Load and link exactly as lgp_lift did.
    std::vector<rc::Module> modules;
    try {
        for (const std::string& path : options.module_paths)
            modules.push_back(rc::load_module_file(path));
    } catch (const std::exception& e) {
        return fail(std::string("cannot load module: ") + e.what());
    }
    if (native) {
        // Without lifted code every ExeFS module comes along. One that
        // exports what an HLE replaces stays out, as it stays out of the lift
        // (ADR-0005: the multimedia module behind movie::); its imports then
        // reach the HLE through the import shims.
        std::erase_if(modules, [&](const rc::Module& m) {
            for (const rc::Symbol& s : m.dynsym)
                if (s.defined() && MovieHle::instance().handler(s.name)) {
                    rt::log(LogLevel::Info, "boot: native: %s left out, the movie HLE replaces its %s", m.name.c_str(),
                            s.name.c_str());
                    result.native.modules_left_out.push_back(m.name);
                    return true;
                }
            return false;
        });
    }
    rc::LinkOptions lo;
    lo.first_base = UINT64_MAX;
    for (size_t i = 0; i < code.module_count; ++i)
        lo.first_base = std::min(lo.first_base, code.modules[i].base);
    if (code.module_count == 0)
        lo.first_base = rc::LinkOptions{}.first_base;
    if (native) {
        lo.first_base = options.native.base;
        lo.page_size = kNativePage;
    }
    const auto image = std::make_unique<rc::LinkedImage>(rc::link(std::move(modules), lo));
    if (!native && !check_layout(*image, code, &result.error))
        return result;

    for (const rc::Import& imp : image->imports)
        if (imp.target_va == 0 && !imp.weak)
            result.unresolved_imports.push_back(image->modules[imp.from_module].name + ": " + imp.name);
    std::sort(result.unresolved_imports.begin(), result.unresolved_imports.end());
    result.unresolved_imports.erase(std::unique(result.unresolved_imports.begin(), result.unresolved_imports.end()),
                                    result.unresolved_imports.end());
    if (!result.unresolved_imports.empty())
        rt::log(LogLevel::Warn, "boot: %zu imports are not defined by any loaded module (first: %s)",
                result.unresolved_imports.size(), result.unresolved_imports.front().c_str());

    const std::optional<uint64_t> init_start = find_init_start(*image);
    result.kind = options.start;
    if (result.kind == StartKind::Auto)
        result.kind = init_start ? StartKind::Sdk : StartKind::Homebrew;
    if (result.kind == StartKind::Sdk && !init_start)
        return fail("no module defines nn::init::Start (an SDK older than 3.x would need rtld's old start sequence, "
                    "which is not implemented)");
    if (result.kind == StartKind::Homebrew && image->modules.size() != 1)
        return fail("homebrew start needs exactly one module");

    // Homebrew: the unrelocated image (libnx's crt0 relocates itself; RELR is
    // not idempotent, so the loader must not apply it as well).
    std::vector<rc::Module> homebrew;
    if (result.kind == StartKind::Homebrew) {
        homebrew.push_back(rc::load_module_file(options.module_paths.front()));
        homebrew.front().base = image->modules.front().base;
    }
    const std::vector<rc::Module>& to_map = result.kind == StartKind::Homebrew ? homebrew : image->modules;

    // 2. Process configuration (NPDM wins over the defaults).
    rt::ProcessConfig cfg = options.process;
    uint64_t stack_size = options.main_stack_size;
    int32_t priority = 44, core = 0;
    if (options.npdm) {
        const Npdm& n = *options.npdm;
        cfg.program_id = n.program_id;
        if (n.core_mask)
            cfg.core_mask = *n.core_mask;
        if (n.priority_mask)
            cfg.priority_mask = *n.priority_mask;
        cfg.default_core = n.main_thread_core;
        priority = n.main_thread_priority;
        core = n.main_thread_core;
        if (n.main_thread_stack_size != 0)
            stack_size = n.main_thread_stack_size;
    }
    stack_size = page_up(stack_size);

    rt::reset_dispatch();
    Process process;
    process.graphics = options.graphics;
    process.frame_limit = options.nvn.frame_limit;
    process.stop_requested = options.nvn.stop_requested;
    process.on_present = options.nvn.on_present;
    process.paced = options.nvn.pace_hz > 0;
    process.pacer = FramePacer(options.nvn.pace_hz);
    process.result = &result;
    process.image = image.get();
    uint64_t end = 0;
    for (const rc::Module& m : image->modules)
        end = std::max(end, m.base + page_up(m.image.size()));
    process.boot_base = end;
    if (native) {
        process.pages.unit = kNativePage;
        process.boot_base = native_up(end);
        cfg.custom_layout = true;
        cfg.layout = native_layout(options.native.base, cfg.total_memory_size);
        cfg.memory_options.at_guest_address = true;
        // Horizon's address space has 39 bits, and so have the address fields
        // the SDK writes into IPC buffer descriptors (Switchbrew, HIPC): above
        // that, services see truncated buffer addresses (the suspected cause
        // of LGP's 0xE601 InvalidPointer abort at 0x80_0000_0000, Mac 23.10.).
        if (cfg.layout.end() > (uint64_t{1} << 39))
            rt::log(LogLevel::Warn, "boot: native layout ends at 0x%" PRIx64 ", above the 39-bit address space: IPC "
                    "buffer addresses will be truncated", cfg.layout.end());
    }
    // Restores the resolver and forgets the native code on every way out.
    struct NativeReset {
        bool on;
        rt::IndirectResolver previous;
        ~NativeReset() {
            if (on) {
                rt::set_indirect_resolver(previous);
                g_native_code.clear();
            }
        }
    } native_reset{native, rt::indirect_resolver()};
    NativeState native_state;

    try {
        rt::Kernel kernel(cfg);
        process.kernel = &kernel;
        // LGP_PARK_SPIN_US: guest threads spin this long before they sleep
        // in a wait (diagnosis, P7.3: docs/knowledge/android-ndk-build.md).
        if (const char* e = std::getenv("LGP_PARK_SPIN_US"); e != nullptr && *e != '\0') {
            const uint64_t us = std::strtoull(e, nullptr, 10);
            kernel.scheduler().set_park_spin_ns(us * 1000);
            rt::log(LogLevel::Info, "sched: waits spin %" PRIu64 " us before they sleep (LGP_PARK_SPIN_US)", us);
        }
        // LGP_RENDER_RELEASE_SLOT=1: renderer calls give the core's slot to
        // another guest thread meanwhile (core baton only, host_work.hpp).
        const char* release = std::getenv("LGP_RENDER_RELEASE_SLOT");
        rt::enable_host_work_release(release != nullptr && std::strcmp(release, "1") == 0);
        if (rt::host_work_release_enabled()) {
            rt::log(LogLevel::Info, "sched: renderer calls give the slot up (LGP_RENDER_RELEASE_SLOT)%s",
                    cfg.sched_mode == rt::SchedMode::CoreBaton ? "" : ", but only the core baton mode has per-core slots");
        }
        // Detached again before the kernel goes, also on an exception.
        struct ControlAttach {
            ProcessControl* control;
            ~ControlAttach() {
                if (control != nullptr)
                    control->attach(nullptr);
            }
        } control_attach{options.control};
        if (options.control != nullptr)
            options.control->attach(&kernel);
        const rt::Layout& l = kernel.memory().layout();
        for (const rc::Module& m : to_map)
            if (!map_module(kernel, m, &result.error))
                return result;
        if (kernel.map_owned(l.stack.end() - stack_size, stack_size, rt::MemState::Stack, rt::kPermRW).failed())
            return fail("cannot map the main thread stack");
        if (native) {
            if (!native_rewrite(kernel, *image, to_map, options, process, &native_state, &result))
                return result;
            rt::set_indirect_resolver(&resolve_native);
        }

        // 3. HLE services. Keyboard and message boxes go through
        // UserWaitTextInput: while the player types, missing presents are no
        // stall. Declared first, so it outlives the handlers holding it.
        std::unique_ptr<UserWaitTextInput> text_input;
        hle::ServiceManager sm;
        hle::SystemConfig system = options.system;
        if (options.control != nullptr)
            system.docked = options.control->requested_docked().value_or(system.docked);
        mods::set_widescreen_docked(system.docked);
        const hle::BaseServices base = hle::register_base_services(sm, system, cfg);
        if (options.control != nullptr)
            options.control->attach_applet(base.applet);  // dropped again by control_attach
        if (options.ldn_lan.lan)
            hle::register_local_network(sm, options.ldn_lan);
        if (options.text_input != nullptr) {
            text_input = std::make_unique<UserWaitTextInput>(options.text_input);
            base.applet->set_keyboard_handler(keyboard_handler(text_input.get()));
            base.applet->set_message_handler(message_handler(text_input.get()));
        }
        // hid: titles open it during their start and abort without it (LGP).
        // The host tick below samples options.input (no controllers without).
        hle::HidConfig hid_config;
        hid_config.input = options.input;
        hid_config.log_input = options.input_log;
        hid_config.freeze_orientation = options.freeze_motion_pose;
        const std::shared_ptr<hle::HidState> hid = hle::register_hid(sm, hid_config);
        // audout:u paced by the host clock; samples go to options.audio if set.
        const std::shared_ptr<hle::AudioOutState> audout = hle::register_audout(sm, options.audio);
        std::unique_ptr<platform::IStorage> game;
        std::unique_ptr<platform::HostDirectoryStorage> save;
        {
            std::string error;
            if (!open_game_storage(options, &game, &error))
                return fail(error);
        }
        if (!options.save_dir.empty() &&
            !(save = platform::HostDirectoryStorage::open_root(options.save_dir, false)))
            return fail("save directory " + options.save_dir + " does not exist");
        hle::FsConfig fs;
        fs.game = game.get();
        fs.save = save.get();
        fs.program_id = cfg.program_id;
        fs.on_commit = options.on_save_commit;
        {
            // Data mods: RomFS files from <mods dir>/<id>/romfs/ (lgp/mods/mods.hpp).
            std::filesystem::path mods_dir = options.mods.dir;
            if (mods_dir.empty() && !options.game_dir.empty())
                mods_dir = std::filesystem::path(options.game_dir) / "mods";
            if (!mods_dir.empty() && !options.mods.enabled.empty()) {
                std::vector<std::string> notes;
                std::map<std::string, std::string> owners;
                fs.romfs_replace = mods::romfs_files(mods_dir, options.mods.enabled, &notes, &owners);
                for (const std::string& n : notes)
                    rt::log(LogLevel::Warn, "mods: %s", n.c_str());
                // Asked after mods::start(): a mod whose needed mod did not start brings no files.
                fs.romfs_keep = [owners = std::move(owners)](const std::string& path) {
                    auto it = owners.find(path);
                    if (it == owners.end() || mods::needs_met(it->second))
                        return true;
                    rt::log(LogLevel::Warn, "mods: %s of %s left out, the mod did not start", path.c_str(),
                            it->second.c_str());
                    return false;
                };
            }
        }
        // The widescreen mod's moved layouts, changed as the game reads them.
        fs.romfs_changes.begin = &mods::widescreen_layouts_begin;
        fs.romfs_changes.wants = &mods::widescreen_layouts_wants;
        fs.romfs_changes.change = &mods::widescreen_layouts_change;
        fs.romfs_changes.epoch = &mods::widescreen_layouts_epoch;
        hle::register_fs(sm, std::move(fs));
        if (ignore_system_data())
            rt::log(LogLevel::Warn, "boot: LGP_IGNORE_SYSTEMDATA=1: systemdata/ of the game folder is not used");
        // System fonts from <game>/systemdata, else the stand-in font.
        hle::register_pl(sm, ignore_system_data() ? nullptr : game.get());
        sm.install(kernel);

        // 4. Code: the lifted functions, then the boot's host functions
        // (later registrations win, so the graphics stops replace the
        // recompiled sdk functions at those addresses).
        rt::register_functions(code.entries, code.entry_count);
        std::vector<LiftedEntry> host = {{process.boot_base + kThunkReturn, &return_thunk}};
        rt::MainThreadParams main;
        main.stack_top = l.stack.end();
        main.priority = priority;
        main.core = core;
        if (result.kind == StartKind::Sdk) {
            const uint64_t data_va = process.boot_base + process.pages.data();
            if (kernel.map_owned(data_va, kPage, rt::MemState::CodeData, rt::kPermRW).failed())
                return fail("cannot map the boot data page behind the modules");
            for (const uint64_t list : {kAutoLoadList, kManualLoadList}) {
                const uint64_t self[2] = {data_va + list, data_va + list};  // empty: front = back = the list
                kernel.memory().copy_to_user(data_va + list, self, sizeof(self));
            }
            bind_ro_globals(kernel, *image, data_va, process.boot_base + kThunkLookup);
            process.start_va = *init_start;
            process.argument_va = process.boot_base + process.pages.argument();
            for (auto it = image->modules.rbegin(); it != image->modules.rend(); ++it) {
                // rtld is not part of its own auto-load list.
                if (it->name == "rtld")
                    continue;
                rt::log(LogLevel::Info, "boot: %s: DT_INIT %s0x%" PRIx64 ", .init_array %zu entries", it->name.c_str(),
                        it->init != 0 ? "" : "none ", it->init != 0 ? it->base + it->init : 0,
                        it->init_array.size());
                std::vector<uint64_t> calls;
                if (it->init != 0) {
                    calls.push_back(it->base + it->init);
                } else if (!it->init_array.empty()) {
                    if (options.run_init_arrays) {
                        for (const uint64_t off : it->init_array)
                            calls.push_back(it->base + off);
                    } else {
                        rt::log(LogLevel::Warn,
                                "boot: %s has an .init_array but no DT_INIT; rtld runs only DT_INIT (see "
                                "--init-arrays)",
                                it->name.c_str());
                    }
                }
                for (const uint64_t va : calls) {
                    if (rt::lookup_function(va) == nullptr) {
                        rt::log(LogLevel::Warn, "boot: initializer of %s at 0x%" PRIx64 " was not lifted, skipped",
                                it->name.c_str(), va);
                        result.skipped_initializers.push_back(it->name);
                        continue;
                    }
                    process.initializers.push_back(va);
                    process.initializer_modules.push_back(it->name);
                }
            }
            host.push_back({process.boot_base + kThunkEntry, &entry_thunk});
            host.push_back({process.boot_base + kThunkNotify, &notify_thunk});
            host.push_back({process.boot_base + kThunkInitialize, &initialize_thunk});
            host.push_back({process.boot_base + kThunkLookup, &lookup_thunk});
            main.entry = process.boot_base + kThunkEntry;
        } else {
            main.entry = to_map.front().base + to_map.front().entry_offset;
        }
        std::vector<uint64_t> bootstrap_loaders;
        if (options.graphics != GraphicsMode::Continue) {
            for (const auto& [name, va] : find_graphics_entries(*image)) {
                if (options.graphics == GraphicsMode::Hle && name == "nvnBootstrapLoader") {
                    bootstrap_loaders.push_back(va);  // answered by the HLE below
                    continue;
                }
                const size_t i = process.graphics_names.size();
                process.graphics_names.push_back(name);
                host.push_back({va, kGraphicsThunks[i]});
                rt::log(LogLevel::Debug, "boot: graphics %s at %s (0x%" PRIx64 ")",
                        options.graphics == GraphicsMode::Hle ? "no-op" : "stop", name.c_str(), va);
            }
        }
        if (kernel.map_owned(process.boot_base + process.pages.import_shim_data(),
                             process.pages.import_shim_data_size(), rt::MemState::CodeData, rt::kPermRW).failed())
            return fail("cannot map the import shim page");
        if (const size_t n = install_import_shims(kernel, *image, process.boot_base + process.pages.import_shims(),
                                                  process.boot_base + process.pages.import_shim_data(), &host))
            rt::log(LogLevel::Info, "boot: %zu undefined imports bound to host shims", n);
        {
            // The movie HLE signals the game's nn::os events through the SDK.
            uint64_t signal_event = 0;
            for (const rc::Module& m : image->modules)
                for (const rc::Symbol& s : m.dynsym)
                    if (s.defined() && s.name == "_ZN2nn2os11SignalEventEPNS0_9EventTypeE")
                        signal_event = m.base + s.value;
            MovieHle::instance().set_guest(&call_guest, signal_event);
            MovieHle::instance().configure(game.get(), options.video, options.movie_audio);
        }
        // Before `game` goes away, and on this thread rather than in exit().
        struct MovieRelease {
            ~MovieRelease() { MovieHle::instance().release(); }
        } movie_release;
        rt::register_functions(host.data(), host.size());

        // NVN-HLE: thunks for every NVN proc, the sdk's nvnBootstrapLoader
        // replaced by the HLE's, presents wrapped for the frame counter.
        std::unique_ptr<gpu::nvn::Hle> nvn;
        if (options.graphics == GraphicsMode::Hle) {
            gpu::nvn::HleOptions ho;
            ho.thunk_base = process.boot_base + process.pages.nvn_thunks();
            ho.unimplemented = options.nvn.unimplemented_continue ? gpu::nvn::UnimplementedMode::Continue
                                                                  : gpu::nvn::UnimplementedMode::Halt;
            ho.trace = options.nvn.trace;
            // "continue" also lets shaders the translator cannot handle yet
            // through (their draws are skipped) instead of failing SetShaders.
            ho.accept_untranslated_programs = options.nvn.unimplemented_continue;
            nvn = std::make_unique<gpu::nvn::Hle>(ho, *options.nvn.backend);
            nvn->install();
            process.nvn = nvn.get();
            if (bootstrap_loaders.empty())
                rt::log(LogLevel::Warn, "boot: no module exports nvnBootstrapLoader, the NVN-HLE is unreachable");
            for (const uint64_t va : bootstrap_loaders) {
                nvn->replace_bootstrap_loader(va);
                rt::log(LogLevel::Debug, "boot: nvnBootstrapLoader at 0x%" PRIx64 " -> NVN-HLE", va);
            }
            const uint64_t present_va = nvn->proc_address("nvnQueuePresentTexture");
            process.present_fn = rt::lookup_function(present_va);
            if (process.present_fn != nullptr) {
                const LiftedEntry present = {present_va, &present_thunk};
                rt::register_functions(&present, 1);
            }
        }

        if (!native)
            install_function_traces();
        else if (std::getenv("LGP_TRACE_FN") != nullptr)
            rt::log(LogLevel::Warn, "trace: LGP_TRACE_FN needs lifted code, ignored by the native start");

        // Game mods: every function is registered, no guest thread runs yet.
        struct ModsStop {
            ~ModsStop() { mods::stop(); }
        } mods_stop;
        // Live mods with a file are installed switched off as well, for the menu.
        mods::StartOptions mo = options.mods;
        if (mo.dir.empty() && !options.game_dir.empty())
            mo.dir = std::filesystem::path(options.game_dir) / "mods";
        mo.standby = true;
        std::error_code mods_ec;
        if (!native &&
            (!mo.enabled.empty() || mo.hook_log > 0 || (!mo.dir.empty() && std::filesystem::is_directory(mo.dir, mods_ec)))) {
            mo.memory = &kernel.memory();
            // Diagnosis and runs without a window: the screen's aspect, "21/9" or "2.333".
            if (const char* env = std::getenv("LGP_WIDESCREEN_ASPECT"); env != nullptr && *env != '\0') {
                char* end = nullptr;
                double a = std::strtod(env, &end);
                if (end != nullptr && *end == '/') a /= std::strtod(end + 1, &end);
                if (end != nullptr && *end == '\0' && a > 0 && std::isfinite(a))
                    mo.display_aspect = a;
                else
                    rt::log(LogLevel::Warn, "LGP_WIDESCREEN_ASPECT: expected W/H or a number, ignored");
            }
            // Diagnosis: guest memory at hook-log doors (lgp/mods/mods.hpp).
            if (const char* env = std::getenv("LGP_HOOK_DUMP"); env != nullptr && *env != '\0') {
                std::string dump_error;
                if (!mods::parse_hook_dumps(env, &mo.dumps, &dump_error)) {
                    rt::log(LogLevel::Warn, "LGP_HOOK_DUMP: %s, ignored", dump_error.c_str());
                    mo.dumps.clear();
                }
            }
            if (const char* env = std::getenv("LGP_HOOK_ACT"); env != nullptr && *env != '\0') {
                std::string act_error;
                if (!mods::parse_hook_acts(env, &mo.acts, &act_error)) {
                    rt::log(LogLevel::Warn, "LGP_HOOK_ACT: %s, ignored", act_error.c_str());
                    mo.acts.clear();
                }
            }
            // online-button: "far away" sends the next ldn session to the server.
            mods::set_ldn_route_handler(&hle::set_ldn_remote);
            mods::StartResult mr;
            std::string mod_error;
            if (!mods::start(mo, &mr, &mod_error))
                return fail("mods: " + mod_error);
            result.mods_active = mr.active;
            result.mods_messages = mr.messages;
            // At the start too: apps that the system ends (Android, iOS) never
            // reach the summary line "mods: on" at the end.
            if (!mr.active.empty())
                rt::log(LogLevel::Info, "mods: active at start: %s", mods::join_mod_list(mr.active).c_str());
        }
        // Without the firmware's NgWord2 archive the SDK's profanity filter
        // would stop the game after the intro; its methods are answered on
        // the host instead (ngc_hle.hpp). After the mods: mods::start()
        // clears every hook first.
        struct NgcStop {
            const rc::LinkedImage* image = nullptr;
            ~NgcStop() {
                if (image)
                    remove_ngc_hle(*image);
            }
        } ngc_stop;
        if (!has_system_data(game.get(), kNgWord2DataId)) {
            if (native) {
                rt::log(LogLevel::Warn, "boot: no systemdata/%016" PRIx64 " (NgWord2) and the native start has no "
                                        "hooks; the game stops after the intro without it", kNgWord2DataId);
            } else {
                const NgcHleResult ngc = install_ngc_hle(kernel, *image);
                ngc_stop.image = image.get();
                if (ngc.exported == 0)
                    rt::log(LogLevel::Debug, "boot: no systemdata/%016" PRIx64 " (NgWord2); no module exports the "
                                            "profanity filter", kNgWord2DataId);
                else if (ngc.installed < ngc.exported)
                    rt::log(LogLevel::Warn, "boot: no systemdata/%016" PRIx64 " (NgWord2) and only %zu of %zu "
                                            "profanity filter methods hooked; lift again with this lgp_lift, or "
                                            "copy systemdata/ from your firmware", kNgWord2DataId, ngc.installed,
                            ngc.exported);
                else
                    rt::log(LogLevel::Info, "boot: no systemdata/%016" PRIx64 " (NgWord2); profanity filter "
                                            "answered on the host (%zu methods)", kNgWord2DataId, ngc.installed);
            }
        }
        // Diagnosis patches and watches after the mods: mods::start() clears
        // every patch first. Removed on every way out of here (the watch list
        // must not outlive this GuestMemory).
        struct DiagnosisStop {
            rt::GuestMemory& memory;
            ~DiagnosisStop() {
                rt::clear_patches();
                rt::clear_write_watches(memory);
            }
        } diagnosis_stop{kernel.memory()};
        install_value_patches(kernel.memory(), &process.watch_from_frame);
        if (const char* env = std::getenv("LGP_POKE_FRAME"); env != nullptr && *env != '\0') {
            std::string error;
            if (!rt::parse_value_patches(env, &process.frame_pokes, &error)) {
                rt::log(LogLevel::Warn, "LGP_POKE_FRAME: %s, ignored", error.c_str());
                process.frame_pokes.clear();
            }
            for (const rt::ValuePatch& poke : process.frame_pokes)
                rt::log(LogLevel::Warn, "LGP_POKE_FRAME: 0x%" PRIx64 " = 0x%" PRIx64 " after every present", poke.va,
                        poke.value);
        }
        if (native && !native_finish(kernel, native_state, options.native, &result))
            return result;
        if (result.native.image == BootResult::NativeStats::Image::Written)
            return result;  // BootOptions::Native::image_out: no run

        // 5. Run.
        rt::log(LogLevel::Info, "boot: %s start, %zu modules, %zu lifted functions, entry 0x%" PRIx64,
                start_kind_name(result.kind), image->modules.size(), code.entry_count, main.entry);
        g_process = &process;
        result.started = true;
        // Host tick for the services that follow guest time (hid sampling,
        // audout buffer release), every 5 ms: from a host thread on the host
        // clock, or as a periodic callback of the virtual clock (on the guest
        // thread that moves time past it, so at fixed points of the run).
        constexpr uint64_t kTickNs = 5'000'000;
        const bool virtual_clock = kernel.clock().is_virtual();
        if (virtual_clock)
            kernel.clock().add_periodic(kTickNs, [&](uint64_t now) {
                hid->update(kernel, now + 1);
                audout->update(kernel, now + 1);
            });
        std::atomic<bool> ticking{true};
        std::thread ticker([&] {
            const auto t0 = std::chrono::steady_clock::now();
            const auto deadline = t0 + std::chrono::seconds(options.watchdog_seconds);
            while (ticking.load()) {
                if (options.watchdog_seconds != 0 && std::chrono::steady_clock::now() >= deadline) {
                    rt::log(LogLevel::Warn, "boot: watchdog after %u s, guest threads:", options.watchdog_seconds);
                    for (const std::string& line : kernel.scheduler().thread_report())
                        rt::log(LogLevel::Warn, "  %s", line.c_str());
                    kernel.scheduler().stop();
                    break;
                }
                if (!virtual_clock) {
                    const uint64_t now = static_cast<uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0)
                            .count()) + 1;
                    hid->update(kernel, now);
                    audout->update(kernel, now);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        });
        // Present-stall report on its own thread: the ticker can itself be
        // what hangs (audout's update waits for a lock an append holds).
        std::thread stall_watch;
        if (nvn && options.stall_report_seconds != 0)
            stall_watch = std::thread([&] {
                StallReport stall(options.stall_report_seconds);
                while (ticking.load()) {
                    const auto now = std::chrono::steady_clock::now();
                    const bool user = text_input && text_input->waiting();
                    if (stall.due(process.frames.load(), kernel.scheduler().paused() || user, now))
                        log_stall(kernel, *audout, stall);
                    else if (stall.resumed())
                        rt::log(LogLevel::Warn, "boot: presents resumed at frame %" PRIu64 " after %.1f s",
                                stall.frames(), stall.seconds(now));
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
            });
        KernelMemoryView memory_view(kernel);
        if (options.nvn.attach_memory)
            options.nvn.attach_memory(&memory_view);
        result.exit = kernel.run(main);
        result.hook_log_calls = mods::hook_log_calls();
        if (options.nvn.attach_memory)
            options.nvn.attach_memory(nullptr);
        ticking = false;
        ticker.join();
        if (stall_watch.joinable())
            stall_watch.join();
        g_process = nullptr;
        result.debug_output = kernel.debug_output();
        if (nvn) {
            result.frames = process.frames.load();
            result.paced_wait_ns = process.paced_wait_ns.load();
            // Includes nvnQueuePresentTexture, which the boot looks up itself.
            for (const gpu::nvn::ProcStats& ps : nvn->stats()) {
                ++result.nvn_procs;
                result.nvn_procs_implemented += ps.implemented ? 1 : 0;
            }
            result.nvn_unimplemented = nvn->unimplemented_calls();
            result.nvn_untranslated = nvn->untranslated_programs();
            result.nvn_untranslated_reasons = nvn->untranslated_reasons();
            process.nvn = nullptr;
            nvn.reset();
        }
        for (const auto& [va, hits] : rt::patch_hits())
            rt::log(LogLevel::Warn, "patch: 0x%" PRIx64 " passed %" PRIu64 " times", va, hits);
        if (rt::write_watch_hits() != 0)
            rt::log(LogLevel::Warn, "watch: %" PRIu64 " watched stores", rt::write_watch_hits());
        rt::set_active_kernel(nullptr);
    } catch (const std::exception& e) {
        g_process = nullptr;
        rt::set_active_kernel(nullptr);
        return fail(std::string("boot failed: ") + e.what());
    }
    result.unresolved = rt::unresolved_targets();
    result.unresolved_config = rt::unresolved_config();
    return result;
}

// --- ProcessControl ---------------------------------------------------------------

bool ProcessControl::pause(int64_t timeout_ms) {
    std::lock_guard lock(mutex_);
    if (closing_)
        return false;
    ++pauses_;
    return kernel_ == nullptr || kernel_->scheduler().pause(timeout_ms);
}

void ProcessControl::resume() {
    std::lock_guard lock(mutex_);
    if (pauses_ == 0 || --pauses_ > 0)
        return;
    if (kernel_ != nullptr)
        kernel_->scheduler().resume();
}

bool ProcessControl::guest_resident(uint64_t* bytes) const {
    std::lock_guard lock(mutex_);
    return kernel_ != nullptr && kernel_->memory().host_resident(bytes);
}

bool ProcessControl::guest_regions(uint64_t bucket, GuestRegions* out) const {
    std::lock_guard lock(mutex_);
    if (kernel_ == nullptr)
        return false;
    const rt::GuestMemory& memory = kernel_->memory();
    const platform::HostBlock& block = memory.host_block();
    out->guest_start = memory.layout().start();
    out->file_begin = block.file_begin();
    out->file_end = block.file_end();
    return block.region_stats(bucket, &out->regions);
}

bool ProcessControl::extend_file_window(uint64_t begin, platform::HostBlock::MoveStats* stats, std::string* error) {
    std::lock_guard lock(mutex_);
    if (kernel_ == nullptr) {
        *error = "no running process";
        return false;
    }
    std::lock_guard memory_lock(kernel_->memory_mutex());
    return kernel_->memory().host_block().extend_file_window(begin, stats, error);
}

void ProcessControl::close() {
    std::lock_guard lock(mutex_);
    closing_ = true;
    if (pauses_ == 0)
        return;
    pauses_ = 0;
    if (kernel_ != nullptr)
        kernel_->scheduler().resume();
}

void ProcessControl::stop() {
    std::lock_guard lock(mutex_);
    if (kernel_ != nullptr)
        kernel_->scheduler().stop();
}

bool ProcessControl::closing() const {
    std::lock_guard lock(mutex_);
    return closing_;
}

bool ProcessControl::paused() const {
    std::lock_guard lock(mutex_);
    return pauses_ > 0;
}

int ProcessControl::pause_count() const {
    std::lock_guard lock(mutex_);
    return pauses_;
}

bool ProcessControl::set_docked(bool docked) {
    std::lock_guard lock(mutex_);
    docked_ = docked;
    mods::set_widescreen_docked(docked);  // the widescreen mod rests in the TV mode
    return kernel_ != nullptr && applet_ != nullptr && applet_->set_docked(*kernel_, docked);
}

std::optional<bool> ProcessControl::requested_docked() const {
    std::lock_guard lock(mutex_);
    return docked_;
}

void ProcessControl::attach_applet(std::shared_ptr<hle::AppletState> applet) {
    std::lock_guard lock(mutex_);
    applet_ = std::move(applet);
    // A set_docked() between boot() reading the request and now.
    if (applet_ != nullptr && kernel_ != nullptr && docked_)
        applet_->set_docked(*kernel_, *docked_);
}

void ProcessControl::attach(rt::Kernel* kernel) {
    std::lock_guard lock(mutex_);
    kernel_ = kernel;
    if (kernel_ == nullptr)
        applet_.reset();
    if (kernel_ != nullptr && pauses_ > 0)
        kernel_->scheduler().pause(0);
}

bool read_game_file(const BootOptions& options, std::string_view path, std::vector<uint8_t>* out, std::string* error) {
    std::unique_ptr<platform::IStorage> game;
    if (!open_game_storage(options, &game, error))
        return false;
    if (!game) {
        *error = "no game storage";
        return false;
    }
    std::unique_ptr<platform::IFile> f;
    std::shared_ptr<hle::DataSource> source;
    uint64_t base = 0, size = 0;
    if (game->open("/romfs" + std::string(path), platform::kOpenRead, &f) == platform::StorageError::Ok) {
        source = std::make_shared<hle::FileSource>(std::move(f));
        size = source->size();
    } else if (game->open("/romfs.bin", platform::kOpenRead, &f) == platform::StorageError::Ok) {
        source = hle::open_image_source(std::move(f), error);
        if (!source)
            return false;
        if (!hle::romfs_find(*source, path, &base, &size)) {
            *error = std::string(path) + " is not in romfs.bin";
            return false;
        }
    } else {
        *error = "neither romfs/ nor romfs.bin";
        return false;
    }
    out->resize(size_t(size));
    if (!source->read(base, out->data(), size)) {
        *error = "cannot read " + std::string(path);
        return false;
    }
    return true;
}

}  // namespace lgp::boot
