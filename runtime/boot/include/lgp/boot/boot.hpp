// Boot program core (P3.4): puts loader, kernel, HLE services and the
// recompiled code together and starts a process from a set of modules.
//
// Two start kinds:
//   Sdk       a Nintendo-SDK title (main + sdk, optionally subsdk*/rtld). The
//             modules are linked (relocated, imports bound) exactly as for
//             lifting, and the boot program does rtld's job itself (ADR-0005:
//             rtld is not recompiled): it calls the SDK's nn::init::Start with
//             host functions for rtld's two callbacks; "call initializers"
//             runs every module's DT_INIT in reverse load order, like rtld.
//   Homebrew  one libnx NRO/NSO. The image stays unrelocated (libnx's crt0
//             relocates itself) and starts at its entry with x0 = 0,
//             x1 = main thread handle.
//
// Graphics:
//   Stop      the first call into the graphics stack (nv::SetGraphicsAllocator,
//             nv::InitializeGraphics, nvnBootstrapLoader, ...) ends the
//             process cleanly and is reported. That is the P3.4 acceptance
//             point "boots up to the first graphics call".
//   Hle       the NVN-HLE (gpu/nvn, ADR-0003) answers nvnBootstrapLoader and
//             every NVN proc; the nv:: setup functions of the driver become
//             no-ops (the driver never runs, so there is nothing to set up).
//             Frames go to BootOptions::nvn.backend.
//   Continue  the recompiled driver runs (it then needs nvdrv, which does
//             not exist); for tests and diagnosis.
//
// The lifted code bakes in addresses, so boot() checks the linked layout
// against the LiftedModule table the recompiler emitted and refuses to run on
// a mismatch (other module set, other order, other build).
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "lgp/boot/npdm.hpp"
#include "lgp/gpu/nvn/backend.hpp"
#include "lgp/hle/services.hpp"
#include "lgp/mods/mods.hpp"
#include "lgp/platform/audio.hpp"
#include "lgp/platform/input.hpp"
#include "lgp/platform/text_input.hpp"
#include "lgp/platform/video.hpp"
#include "lgp/runtime/dispatch.hpp"
#include "lgp/runtime/kernel.hpp"

namespace lgp::boot {

// What the recompiler produced for one module set (lgp_lift output).
struct LiftedCode {
    const LiftedEntry* entries = nullptr;
    size_t entry_count = 0;
    const LiftedModule* modules = nullptr;
    size_t module_count = 0;
};

enum class StartKind { Auto, Sdk, Homebrew };
enum class GraphicsMode { Stop, Continue, Hle };

// GraphicsMode::Hle.
struct NvnOptions {
    gpu::nvn::Backend* backend = nullptr;  // required; outlives boot()
    // Calls of NVN functions the HLE does not implement: halt the thread
    // (default) or return 0 and count them (bring-up runs).
    bool unimplemented_continue = false;
    bool trace = false;  // log every NVN call
    // Called with the process's guest memory once it exists and with nullptr
    // when the process has ended: backends that draw (WebGPU) read vertex,
    // index and constant buffers through it (WebGpuBackend::set_guest_memory).
    std::function<void(const gpu::nvn::GuestMemoryView*)> attach_memory;
    // Called on the guest thread after every presented frame (1, 2, ...).
    std::function<void(uint64_t frame)> on_present;
    // End the process after this many presented frames (0 = never).
    uint64_t frame_limit = 0;
    // > 0: pace presents like a display of this refresh rate honouring the
    // game's present interval (nvnWindowSetPresentInterval; 2 at 60 Hz =
    // 30 fps), lgp/boot/frame_pacer.hpp. 0: present as fast as the game
    // renders (measurements, tests).
    double pace_hz = 0;
    // Set from another thread (window closed): the process ends at the next
    // present.
    const std::atomic<bool>* stop_requested = nullptr;
};

// Virtual guest clock (rt::ClockMode::Virtual): a present ends at the next
// vsync of this rate (times the window's present interval), and the
// console's date starts here unless SystemConfig sets one (2026-01-01 00:00 UTC).
inline constexpr uint64_t kVirtualVsyncHz = 60;
inline constexpr int64_t kVirtualClockPosixTime = 1'767'225'600;

// Host-side control of the running process for the app's lifecycle (P7:
// phones and tablets in the background). boot() attaches its kernel while the
// process runs. Any thread.
class ProcessControl {
public:
    // Stops every guest thread (rt::Scheduler::pause) until resume(). True if
    // the guest came to rest within timeout_ms; a pause without a running
    // process applies as soon as one starts and returns true. Pauses nest:
    // each pause() needs its own resume(), and only the last one lets the
    // guest continue (an app returning to the foreground must not end a
    // pause menu's pause).
    bool pause(int64_t timeout_ms);
    // Without a matching pause() this does nothing.
    void resume();
    // The app is ending (window closed, Android onDestroy while in the
    // background): lets the guest run again whatever pauses are held, so it
    // reaches the present that ends the process, and turns later pause()
    // calls into no-ops that return false. Cannot be undone.
    void close();
    // Ends the running process now (rt::Scheduler::stop), for a guest that
    // does not reach the present close() waits for (hangs before its first
    // frame, spins without an SVC). Exit reason Stopped.
    void stop();
    bool closing() const;
    bool paused() const;
    int pause_count() const;
    // Guest memory the host holds in RAM (HostBlock::resident); false without
    // a running process or where the OS does not tell.
    bool guest_resident(uint64_t* bytes) const;
    // Residency and dirty pages of guest memory per `bucket` bytes
    // (HostBlock::region_stats, LGP_HOST_BLOCK_REGIONS); false as above.
    struct GuestRegions {
        uint64_t guest_start = 0;  // guest address of block offset 0
        uint64_t file_begin = 0;   // file-backed window, block offsets
        uint64_t file_end = 0;
        std::vector<platform::HostBlock::Region> regions;
    };
    bool guest_regions(uint64_t bucket, GuestRegions* out) const;
    // LGP_HOST_BLOCK_FILE_LATE: moves guest memory from block offset `begin`
    // up to the file window into the file (HostBlock::extend_file_window),
    // under the kernel's memory lock. The caller has paused the guest and
    // emptied the GPU queue. False and *error without a running process.
    bool extend_file_window(uint64_t begin, platform::HostBlock::MoveStats* stats, std::string* error);
    // TV mode (docked, 1080p) or handheld (720p), like putting the console
    // into its dock or taking it out: the running game gets the change
    // messages and picks its render size itself (hle::AppletState::set_docked).
    // Before the process runs, the request replaces BootOptions::system.docked.
    // True if a running game was told about a change.
    bool set_docked(bool docked);
    // The last set_docked() value, if any.
    std::optional<bool> requested_docked() const;
    // boot(): the process's kernel, nullptr when it has ended (also drops the applet).
    void attach(rt::Kernel* kernel);
    // boot(): the process's am state, once the services are registered.
    void attach_applet(std::shared_ptr<hle::AppletState> applet);

private:
    mutable std::mutex mutex_;
    rt::Kernel* kernel_ = nullptr;
    std::shared_ptr<hle::AppletState> applet_;
    std::optional<bool> docked_;
    int pauses_ = 0;
    bool closing_ = false;
};

struct BootOptions {
    // Module files in any order (they are linked in rtld load order, like
    // lgp_lift does). Must be the set the code was lifted from.
    std::vector<std::string> module_paths;
    // main.npdm: program id, main thread stack/priority/core, thread caps.
    std::optional<Npdm> npdm;
    // Auto: Sdk if a module defines nn::init::Start, else Homebrew.
    StartKind start = StartKind::Auto;
    GraphicsMode graphics = GraphicsMode::Stop;
    NvnOptions nvn;
    // rtld runs only DT_INIT (a module's DT_INIT walks its .init_array
    // itself). Diagnostic fallback for a module that has an .init_array but
    // no DT_INIT: call the entries directly. Off by default: if the SDK runs
    // them some other way, they would run twice.
    bool run_init_arrays = false;
    // Seconds after which the boot program logs every guest thread
    // (Scheduler::thread_report) and stops the process (0 = never). For hangs.
    uint32_t watchdog_seconds = 0;
    // Once presents have started: when none follows for this many seconds
    // while the guest is not paused, log once (until presents resume) every
    // guest thread, the HLE requests still being served, with --time-stats
    // what each thread is inside, and the audout and movie:: state, and keep running. 0 = never. For hangs on devices where
    // nobody can attach a debugger (docs/analysis/42-praesent-stillstand.md).
    uint32_t stall_report_seconds = 10;

    // fsp-srv: game storage (romfs.bin or romfs/, systemdata/) and save
    // storage. Empty: no RomFS / no save data.
    std::string game_dir;
    // Files added to the game storage's root, each read from byte ranges of
    // host files (name, spec of lgp/platform/segment_storage.hpp): the Android
    // app's romfs.bin in parts inside its split APKs (--game-file).
    std::vector<std::pair<std::string, std::string>> game_files;
    std::string save_dir;
    // After each successful commit of a save (hle::FsConfig::on_commit; the
    // app's cloud backup, lgp/boot/save_backup.hpp).
    std::function<void()> on_save_commit;
    hle::SystemConfig system;
    // ldn over LAN (--ldn lan): ldn:u and bsd:u reach other instances in the
    // LAN. lan == nullptr (default): the offline console.
    hle::LdnLanConfig ldn_lan;

    // Host devices (P5). nullptr: hid without controllers, audout paced by
    // the host clock without sound. Both must outlive boot().
    platform::IInput* input = nullptr;
    // hid diagnostics (HidConfig::log_input, --input-log).
    bool input_log = false;
    // Six-axis pose stays at rest (HidConfig::freeze_orientation, --motion-pose frozen).
    bool freeze_motion_pose = false;
    platform::IAudio* audio = nullptr;
    // Video decoding for movie:: (in-game videos); nullptr: black pictures.
    platform::IVideo* video = nullptr;
    // Software keyboard (swkbd): text from the host; nullptr = the game's
    // initial text or the account nickname (lgp/boot/keyboard.hpp).
    platform::ITextInput* text_input = nullptr;
    // Pause/resume from the host (app lifecycle); may be nullptr.
    ProcessControl* control = nullptr;
    // Offer the videos' sound tracks to the game (Vorbis, decoded on the
    // host; hidden when built without libvorbis). LGP plays them and paces
    // the video by them: docs/analysis/16-videoton.md.
    bool movie_audio = true;

    // Game mods (P8, lgp/mods/mods.hpp): started on the hook doors of the
    // lifted code before guest code runs, stopped after the run. An empty
    // mods.dir means <game_dir>/mods. A mod that lacks something is logged
    // and left out; an unknown id or a broken mod file fails the boot.
    mods::StartOptions mods;

    // Native start (docs/analysis/56, stage D): the original code runs
    // natively on an AArch64 host (lgp/runtime/native.hpp) instead of lifted
    // code. The modules are linked at `base` with 16 KiB pages (guest address
    // == host address), rewritten in memory like `lgp_recomp native-rewrite`
    // (`config` as for lgp_lift --config, its addresses for the lift base
    // rc::LinkOptions{}.first_base) and reach the host functions through
    // host-call stubs (native_gate::kHostCallWord). Needs a LiftedCode
    // without functions; no mods, no LGP_TRACE_FN.
    struct Native {
        bool enabled = false;
        // Below 2^39 like the console's address space: the SDK's IPC buffer
        // descriptors carry 39 address bits. macOS 26 refuses fixed mappings
        // at 0x10_0000_0000..0x6f_ffff_ffff (Mac M1).
        uint64_t base = 0x70'0000'0000;
        std::string config;
        // File that keeps the rewrite between runs (lgp/recomp/native_cache.hpp),
        // empty: rewrite every time. Holds code derived from the game: local only.
        std::string cache;
        // Part of the cache key: a stamp of the program (any rebuild invalidates).
        std::string cache_salt;
        // Precomputed code image (docs/analysis/56 stage F), local only (game
        // code). image_out: write every page that would become executable
        // (16 KiB pages) to this file, then stop before guest code runs; works
        // on any host, the guest addresses do not depend on it. image: before
        // the run, compare those pages with the file and map the file's pages
        // read + execute over them instead of making memory executable (where
        // the host maps no file executable, macOS, the checked memory is made
        // executable); a difference fails the boot.
        std::string image_out;
        std::string image;
    } native;

    // Kernel settings; program id, stack, priority and cores are taken from
    // the NPDM when there is one. Default total memory: ADR-0001.
    rt::ProcessConfig process;
    uint64_t main_stack_size = 0x10'0000;  // without NPDM (homebrew default)
};

struct BootResult {
    bool started = false;    // false: setup failed, see error
    std::string error;
    StartKind kind = StartKind::Auto;
    rt::ProcessExit exit;
    // Name of the graphics function that ended the run (GraphicsMode::Stop).
    std::string graphics_stop;
    uint64_t graphics_stop_caller = 0;  // x30 at that call
    // GraphicsMode::Hle: presented frames, NVN names handed out to the game
    // (and how many of them the HLE implements), unimplemented ones called.
    uint64_t frames = 0;
    bool frame_limit_reached = false;
    uint64_t paced_wait_ns = 0;  // NvnOptions::pace_hz: time spent waiting for the display
    size_t nvn_procs = 0;
    size_t nvn_procs_implemented = 0;
    std::vector<std::string> nvn_unimplemented;
    // Programs accepted without shaders (unimplemented_continue) and why.
    uint64_t nvn_untranslated = 0;
    std::vector<std::string> nvn_untranslated_reasons;
    std::vector<std::string> debug_output;
    std::vector<std::string> mods_active;    // mods that started
    std::vector<std::string> mods_messages;  // why one did not start, notes (mods::StartResult)
    uint64_t hook_log_calls = 0;             // mods::StartOptions::hook_log
    // BootOptions::native: what the rewrite did. Unpatched sites run as they
    // are (a real SVC ends the host process).
    struct NativeStats {
        bool used = false;
        uint64_t words = 0, renamed = 0, gate_stubs = 0, spill_stubs = 0, host_calls = 0;
        uint64_t x18_unpatched = 0, gate_unpatched = 0;
        uint64_t modules_not_executable = 0;  // data on a 16 KiB page of the text (rtld): text left RW
        // ExeFS modules not loaded: an HLE replaces their exports, as in the lift.
        std::vector<std::string> modules_left_out;
        uint64_t rewrite_ms = 0;
        enum class Cache : uint8_t { Off, Miss, Hit } cache = Cache::Off;  // Native::cache
        // Native::image_out / image: written (the boot stopped there, started
        // stays false without an error), code mapped from it, or checked
        // against it and made executable in memory (the host maps no file
        // executable, macOS).
        enum class Image : uint8_t { None, Written, Mapped, Checked } image = Image::None;
        uint64_t image_ranges = 0, image_bytes = 0;
    } native;
    // Modules whose DT_INIT has no lifted function (not run).
    std::vector<std::string> skipped_initializers;
    // Strong imports no loaded module defines ("module: symbol"); their
    // GOT slots stay 0, a call through one halts with a bad jump to 0.
    std::vector<std::string> unresolved_imports;
    std::vector<rt::UnresolvedTarget> unresolved;
    std::string unresolved_config;  // `function 0x…` lines for the recompiler config

    // The process got as far as intended: it exited by itself, reached the
    // graphics stop or the frame limit.
    bool ok() const;
};

// Runs the process to its end. One boot per runtime at a time (the kernel,
// the dispatch table and the boot callbacks are process-wide).
BootResult boot(const BootOptions& options, const LiftedCode& code);

// One file of the game's RomFS ("/bin/font/x.bfotf") from the game storage
// the options describe (romfs/ tree, romfs.bin, --game-file), for the app's
// menus before and outside the game. False and *error if it is not there.
bool read_game_file(const BootOptions& options, std::string_view path, std::vector<uint8_t>* out, std::string* error);

// Mangled-name prefixes and names the graphics stop catches.
const std::vector<std::string>& graphics_entry_symbols();

const char* start_kind_name(StartKind kind);

}  // namespace lgp::boot
