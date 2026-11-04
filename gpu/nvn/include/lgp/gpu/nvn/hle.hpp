// NVN high-level emulation (ADR-0003 option A, roadmap P4.1).
//
// The game reaches NVN only through function pointers: main imports
// nvnBootstrapLoader from sdk, asks it for nvnDeviceGetProcAddress and loads
// every NVN function by name into its own slots
// (docs/knowledge/nvn-bootstrap-und-treiber.md). The HLE answers those
// lookups with guest addresses of its own: a block of thunk addresses
// registered in the runtime's dispatch table (ADR-0004), one per NVN name.
// A BLR from lifted code to a slot therefore lands in a host function that
// reads the AAPCS64 arguments from CpuState and guest memory. The NVN driver
// in sdk never runs.
//
// Names the HLE does not implement still get a thunk. Calling one halts the
// guest thread with the name in the log (UnimplementedMode::Halt, default),
// or returns 0 and is counted (UnimplementedMode::Continue, for bring-up
// runs that want to see how far a frame gets). Unimplemented command-buffer
// commands are recorded as CmdUnsupported in Continue mode.
//
// Object model: Device, Queue, MemoryPool, Buffer, Texture, Window, Sync and
// CommandBuffer state lives on the host, keyed by the guest address of the
// object; builders and TextureView live inline in their guest storage
// (abi.hpp). GPU addresses are identity-mapped: the GPU address of pool
// storage is its guest VA, so the backend reads vertex/uniform data with the
// normal guest-memory accessors.
//
// One Hle per process (the thunks find it through a process-wide pointer,
// like the active kernel). All entry points are thread-safe.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "lgp/gpu/nvn/abi.hpp"
#include "lgp/gpu/nvn/backend.hpp"

namespace lgp::gpu::nvn {

enum class UnimplementedMode { Halt, Continue };

// Translates the stages of an NVN program (nvnProgramSetShaders) into the
// backend's ProgramDesc. vertex/fragment are SPH + Maxwell code (NVN code
// header already stripped); texture slot k of the result is the bound texture
// index (TEX handle - handle_base). An empty fragment is a vertex-only
// program (depth/shadow passes; the result has no fragment_wgsl). Returns false with an error message if
// the program cannot be translated (the HLE then makes nvnProgramSetShaders
// fail, as a driver rejecting the binary would).
using ProgramTranslator = std::function<bool(const std::vector<uint8_t>& vertex, const std::vector<uint8_t>& fragment,
                                             uint32_t handle_base, ProgramDesc* out, std::string* error)>;
// The built-in translator: gpu/shader (maxwell2wgsl) at run time, the
// fallback path of ADR-0003. An offline shader cache plugs in through
// HleOptions::program_translator.
bool translate_maxwell_program(const std::vector<uint8_t>& vertex, const std::vector<uint8_t>& fragment,
                               uint32_t handle_base, ProgramDesc* out, std::string* error);
// The fragment WGSL translate_maxwell_program emits for this stage (it does
// not depend on the vertex partner); for tools comparing programs.
bool translate_maxwell_fragment(const std::vector<uint8_t>& fragment, std::string* wgsl, std::string* error);
// Render scale (WebGpuBackend, LGP_WEBGPU_RENDER_SCALE): translations after
// this call emit wgsl::Options::render_scale = scale (guest sizes and texel
// coordinates for scaled render targets). Process-wide, default 1 = off,
// which leaves the WGSL as before.
void set_render_scale_shaders(float scale);

// What a compute stage needs besides its code: from the stage's control
// section (abi.hpp, kNvnControlComputeWorkgroup and following).
struct ComputeInfo {
    uint32_t workgroup[3] = {1, 1, 1};
    uint32_t shared_memory_bytes = 0;  // 0 = 16 KiB (wgsl::Options)
    uint32_t local_memory_bytes = 0;   // per thread; 0 = translator default
};
// Reads ComputeInfo for a compute stage: control = NVNshaderData::control
// (guest VA), code = the stage's code. Returns false with an error message
// if the stage cannot run (nvnProgramSetShaders then fails).
using ComputeInfoReader =
    std::function<bool(uint64_t control, const std::vector<uint8_t>& code, ComputeInfo* out, std::string* error)>;
// The built-in reader's parser: `control` = the first `size` bytes of a
// control section (at least kNvnControlComputeEnd). False with an error for
// a section that is no compute stage or whose values no GPU could run
// (workgroup of 0 or more than 1024 threads, more than 48 KiB shared memory).
bool parse_compute_control(const uint8_t* control, size_t size, ComputeInfo* out, std::string* error);
// Built-in translation of a compute stage (Maxwell code without SPH) into the
// backend's compute binding convention (backend.hpp, ProgramDesc).
bool translate_maxwell_compute(const std::vector<uint8_t>& code, const ComputeInfo& info, uint32_t handle_base,
                               ProgramDesc* out, std::string* error);

struct HleOptions {
    // Guest VA of the thunk block (kMaxProcs * 4 bytes). It must not overlap
    // lifted code; put it directly behind the last loaded module so the
    // dispatch table (which spans all registered addresses) stays small.
    uint64_t thunk_base = 0;
    UnimplementedMode unimplemented = UnimplementedMode::Halt;
    // Log every call with its name and first integer arguments (argument
    // logging for ADR-0003's open questions: which fixed-function features
    // are really enabled).
    bool trace = false;
    // Overrides for nvnDeviceGetInteger (key -> value), applied on top of
    // default_device_integers().
    std::vector<DeviceInteger> device_integers;
    // TEX handle of bound texture slot 0 (abi.hpp, UNVERIFIED).
    uint32_t bound_texture_handle_base = kBoundTextureHandleBase;
    // Shader translation for nvnProgramSetShaders; empty = translate_maxwell_program.
    ProgramTranslator program_translator;
    // Compute stages: workgroup size and shared memory from the control
    // section. Empty = the built-in reader (parse_compute_control on the
    // guest's control section; layout UNVERIFIED for LGP, abi.hpp).
    ComputeInfoReader compute_info;
    // A shader the translator cannot handle yet: false = nvnProgramSetShaders
    // fails (as for a broken binary); true = it succeeds like the real driver,
    // the program stays without shaders and its draws are skipped. Games
    // treat a failing SetShaders as fatal; the boot program turns this on with
    // --nvn-unimplemented continue. Hle::untranslated_programs() counts them.
    bool accept_untranslated_programs = false;
};

// Waits for the GPU the guest asks for, since the start (--time-stats line
// "nvn waits"): nvnQueueFinish and nvnSyncWait on a fenced sync, with the time
// spent in the backend (with AsyncBackend: until the queue has run that far).
struct WaitStats {
    uint64_t finish_calls = 0, finish_ns = 0;
    uint64_t sync_calls = 0, sync_ns = 0;
};
WaitStats wait_stats();

struct ProcStats {
    std::string name;
    uint64_t va = 0;
    bool implemented = false;
    uint64_t calls = 0;
};

class Hle {
public:
    // Thunks available for NVN names (NVN 5.4 has 498 functions).
    static constexpr uint32_t kMaxProcs = 1024;

    Hle(const HleOptions& options, Backend& backend);
    ~Hle();
    Hle(const Hle&) = delete;
    Hle& operator=(const Hle&) = delete;

    // Registers the thunk block in the dispatch table and makes this the
    // process's HLE. The thunk for nvnBootstrapLoader is thunk_base itself.
    void install();
    // Also answer calls to `va` (the sdk export nvnBootstrapLoader, ADR-0005:
    // replacement by symbol) with the HLE's bootstrap loader.
    void replace_bootstrap_loader(uint64_t va);
    // Leaves the dispatch table as it is but stops this instance from being
    // the process's HLE (tests).
    void uninstall();

    uint64_t bootstrap_loader_va() const;
    // What nvnDeviceGetProcAddress returns for `name`: a thunk for every
    // "nvn…" name, 0 for anything else.
    uint64_t proc_address(std::string_view name);

    // Present interval of the NVNwindow at guest address `window` (vsyncs per
    // present, nvnWindowSetPresentInterval), 1 for an unknown window.
    int32_t present_interval(uint64_t window) const;

    // Every name handed out so far, in thunk order.
    std::vector<ProcStats> stats() const;
    // Names that were called without an implementation (sorted).
    std::vector<std::string> unimplemented_calls() const;
    // Programs accepted without shaders (accept_untranslated_programs) and the
    // first translator message of each kind, for the end-of-run summary.
    uint64_t untranslated_programs() const;
    std::vector<std::string> untranslated_reasons() const;
    // Present interval (vertical blanks per frame, nvnWindowSetPresentInterval)
    // of the window of the last nvnQueuePresentTexture; -1 before the first.
    // The HLE presents at once; pacing is up to the caller (lgp_boot).
    int32_t last_present_interval() const;
    // Number of NVN functions the HLE implements.
    static size_t implemented_count();
    static bool is_implemented(std::string_view name);

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

// Host time guest threads spent in the backend calls of the NVN-HLE, by
// kind (--time-stats: the lines "gpu call <kind>"; what the guest's "gpu"
// column holds). Counted only while time stats are on.
struct BackendCallTime {
    const char* kind;
    uint64_t ns = 0;
    uint64_t calls = 0;
};
std::vector<BackendCallTime> backend_call_times();

}  // namespace lgp::gpu::nvn
