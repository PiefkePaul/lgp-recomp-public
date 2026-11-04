// Native executor (see native.hpp).
#include "lgp/runtime/native.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <csetjmp>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include "lgp/platform/host_thread.hpp"
#include "lgp/platform/native_signals.hpp"
#include "lgp/recomp/native_gate.hpp"
#include "lgp/runtime/dispatch.hpp"
#include "lgp/runtime/guest.hpp"
#include "lgp/runtime/kernel.hpp"
#include "lgp/runtime/log.hpp"
#include "lgp/runtime/system.hpp"
#include "lgp/runtime/thread.hpp"

#if defined(__aarch64__) && (defined(__GNUC__) || defined(__clang__))
#define LGP_NATIVE_EXEC 1
#else
#define LGP_NATIVE_EXEC 0
#endif

namespace lgp::rt::native {
namespace {

namespace g = lgp::recomp::native_gate;
using g::GateFrame;

// Packing of MRS/MSR system register fields as in read_sysreg():
// (op0 << 14) | (op1 << 11) | (CRn << 7) | (CRm << 3) | op2.
constexpr uint32_t sysreg(uint32_t op0, uint32_t op1, uint32_t crn, uint32_t crm, uint32_t op2) {
    return (op0 << 14) | (op1 << 11) | (crn << 7) | (crm << 3) | op2;
}
// The same from an MRS/MSR word: bits 19..5 hold o0 (op0 - 2), op1, CRn, CRm, op2.
constexpr uint32_t sysreg_of(uint32_t word) { return ((word >> 5) & 0x7FFF) | 0x8000; }
static_assert(sysreg_of(0xD53BE042) == sysreg(3, 3, 14, 0, 2));  // mrs x2, cntvct_el0
[[maybe_unused]] constexpr uint32_t kTpidrEl0 = sysreg(3, 3, 13, 0, 2);
[[maybe_unused]] constexpr uint32_t kTpidrroEl0 = sysreg(3, 3, 13, 0, 3);

[[maybe_unused]] void set_halt(CpuState* ctx, uint32_t reason, uint64_t info) {
    if (ctx->halt == kHaltNone) {
        ctx->halt = reason;
        ctx->halt_info = info;
    }
}

[[maybe_unused]] void frame_to_ctx(const GateFrame& f, CpuState* ctx, unsigned q_count) {
    std::memcpy(ctx->x, f.x, sizeof f.x);
    ctx->sp = f.sp;
    ctx->pc = f.pc;
    unpack_nzcv(*ctx, f.nzcv);
    ctx->fpsr = static_cast<uint32_t>(f.fpsr);
    for (unsigned i = 0; i < q_count; ++i) ctx->vreg[i] = {f.q[i][0], f.q[i][1]};
}

}  // namespace

// Per host thread. The assembly below reads the first fields by offset.
struct NativeThread {
    uint64_t host_sp = 0;     // 0: host SP inside lgp_native_enter (its saved registers)
    uint64_t host_x18 = 0;    // 8
    uint64_t host_fpcr = 0;   // 16
    uint64_t guest_fpcr = 0;  // 24: FPCR to load on entry, FPCR found on return
    GateFrame* state = nullptr;  // 32: registers on entry, after a return
    CpuState* ctx = nullptr;     // 40
    // kInGuest, kInHost, kParked (read by the kicker, __atomic builtins).
    uint32_t phase = 0;
    // While the thread is in the gate: the gate frame, which lies below the
    // guest SP. Guest code called from there (run() nested) starts below it.
    uint64_t stack_floor = 0;
    // Counter reads at the gate (gate_sysreg(): every 64th polls like a back-edge).
    uint32_t counter_reads = 0;
    // Gate entries (written by the thread, read by the kicker) and a time
    // slice request from the kicker (on_signal()), __atomic builtins.
    uint64_t gates = 0;
    uint32_t preempt = 0;
    // A signal in guest code (on_signal()): what happened, the registers.
    platform::SignalKind fault_kind = platform::SignalKind::Segv;
    uint64_t fault_addr = 0;
    platform::SignalContext fault{};
#if LGP_NATIVE_EXEC
    sigjmp_buf exit;  // leave(): back to run(); the signal mask only with LGP_NATIVE_SAVE_MASK=1
#endif
};
#if LGP_NATIVE_EXEC  // the assembly's offsets (64-bit pointers)
static_assert(offsetof(NativeThread, host_sp) == 0 && offsetof(NativeThread, host_x18) == 8);
static_assert(offsetof(NativeThread, host_fpcr) == 16 && offsetof(NativeThread, guest_fpcr) == 24);
static_assert(offsetof(NativeThread, state) == 32 && offsetof(NativeThread, ctx) == 40);
// The assembly hard-codes the frame layout as well.
static_assert(g::kSp == 248 && g::kPc == 256 && g::kNzcv == 272 && g::kFpsr == 280 && g::kQ == 288);
#endif

namespace {
[[maybe_unused]] thread_local NativeThread t_native;
// stats(): this thread's counts (outside NativeThread, which a nested run()
// restores), read by stats() through the registry, added to g_retired when
// the thread leaves its outermost run().
struct Counts {
    uint64_t entries = 0, svcs = 0, host_calls = 0, sysregs = 0, kicks = 0;
};
[[maybe_unused]] thread_local Counts t_counts;
#if LGP_NATIVE_EXEC
void bump(uint64_t& c) { __atomic_store_n(&c, c + 1, __ATOMIC_RELAXED); }
// NativeThread::phase. kInGuest also covers the few instructions between the
// gate and guest code, where a kick is dropped (the kicker sends another).
constexpr uint32_t kInGuest = 0;
constexpr uint32_t kInHost = 1;   // gate, SVC, runtime
constexpr uint32_t kParked = 2;   // paused inside the kick handler
void set_phase(NativeThread& nt, uint32_t p) { __atomic_store_n(&nt.phase, p, __ATOMIC_RELEASE); }
#endif
}  // namespace

}  // namespace lgp::rt::native

#if LGP_NATIVE_EXEC

using lgp::rt::native::NativeThread;

extern "C" {
// Assembly below.
void lgp_native_enter(lgp::recomp::native_gate::GateFrame* state, NativeThread* nt);
void lgp_native_return();
void lgp_native_gate_host(lgp::recomp::native_gate::GateFrame* frame);

// Called from the assembly on the guest stack, before x18 is the host's: a
// leaf without x18 use (no shadow call stack instrumentation).
#if defined(__clang__)
__attribute__((no_sanitize("shadow-call-stack")))
#endif
NativeThread* lgp_native_thread() {
    return &lgp::rt::native::t_native;
}

void lgp_native_gate_dispatch(NativeThread* nt, lgp::recomp::native_gate::GateFrame* f);
[[noreturn]] void lgp_native_fault_exit(NativeThread* nt);
}

// Symbol names as the assembler sees them ("_name" on Mach-O).
#define LGP_STR2(x) #x
#define LGP_STR(x) LGP_STR2(x)
#define LGP_SYM(name) LGP_STR(__USER_LABEL_PREFIX__) #name

// clang-format off
asm(R"(
    .text
    .p2align 4
    .globl )" LGP_SYM(lgp_native_enter) R"(
)" LGP_SYM(lgp_native_enter) R"(:
    // x0 = state (GateFrame layout), x1 = NativeThread. Saves what AAPCS64
    // asks a callee to keep, then becomes the guest.
    sub sp, sp, #176
    stp x19, x20, [sp, #0]
    stp x21, x22, [sp, #16]
    stp x23, x24, [sp, #32]
    stp x25, x26, [sp, #48]
    stp x27, x28, [sp, #64]
    stp x29, x30, [sp, #80]
    stp d8, d9, [sp, #96]
    stp d10, d11, [sp, #112]
    stp d12, d13, [sp, #128]
    stp d14, d15, [sp, #144]
    mov x9, sp
    str x9, [x1, #0]
    str x18, [x1, #8]
    mrs x9, fpcr
    str x9, [x1, #16]
    ldr x9, [x1, #24]
    msr fpcr, x9
    add x9, x0, #288
    ldp q0, q1, [x9, #0]
    ldp q2, q3, [x9, #32]
    ldp q4, q5, [x9, #64]
    ldp q6, q7, [x9, #96]
    ldp q8, q9, [x9, #128]
    ldp q10, q11, [x9, #160]
    ldp q12, q13, [x9, #192]
    ldp q14, q15, [x9, #224]
    ldp q16, q17, [x9, #256]
    ldp q18, q19, [x9, #288]
    ldp q20, q21, [x9, #320]
    ldp q22, q23, [x9, #352]
    ldp q24, q25, [x9, #384]
    ldp q26, q27, [x9, #416]
    ldp q28, q29, [x9, #448]
    ldp q30, q31, [x9, #480]
    ldr x9, [x0, #280]
    msr fpsr, x9
    ldr x9, [x0, #272]
    msr nzcv, x9
    ldr x9, [x0, #248]
    mov sp, x9
    // x16 (IP0) carries the target: scratch at every call boundary. x18 stays
    // the host's, the rewritten code does not use it.
    ldr x16, [x0, #256]
    ldp x2, x3, [x0, #16]
    ldp x4, x5, [x0, #32]
    ldp x6, x7, [x0, #48]
    ldp x8, x9, [x0, #64]
    ldp x10, x11, [x0, #80]
    ldp x12, x13, [x0, #96]
    ldp x14, x15, [x0, #112]
    ldr x17, [x0, #136]
    ldp x19, x20, [x0, #152]
    ldp x21, x22, [x0, #168]
    ldp x23, x24, [x0, #184]
    ldp x25, x26, [x0, #200]
    ldp x27, x28, [x0, #216]
    ldp x29, x30, [x0, #232]
    ldp x0, x1, [x0, #0]
    br x16

    .p2align 4
    .globl )" LGP_SYM(lgp_native_return) R"(
)" LGP_SYM(lgp_native_return) R"(:
    // The guest code entered by lgp_native_enter returned here (x30 of the
    // entry). Results are in x0..x8 and q0..q7; x9..x17 are dead.
    sub sp, sp, #224
    stp x0, x1, [sp, #0]
    stp x2, x3, [sp, #16]
    stp x4, x5, [sp, #32]
    stp x6, x7, [sp, #48]
    mrs x9, nzcv
    stp x8, x9, [sp, #64]
    stp q0, q1, [sp, #80]
    stp q2, q3, [sp, #112]
    stp q4, q5, [sp, #144]
    stp q6, q7, [sp, #176]
    mrs x9, fpsr
    str x9, [sp, #208]
    bl )" LGP_SYM(lgp_native_thread) R"(
    ldr x1, [x0, #32]
    ldp x2, x3, [sp, #0]
    stp x2, x3, [x1, #0]
    ldp x2, x3, [sp, #16]
    stp x2, x3, [x1, #16]
    ldp x2, x3, [sp, #32]
    stp x2, x3, [x1, #32]
    ldp x2, x3, [sp, #48]
    stp x2, x3, [x1, #48]
    ldp x2, x3, [sp, #64]
    str x2, [x1, #64]
    str x3, [x1, #272]
    ldr x2, [sp, #208]
    str x2, [x1, #280]
    stp x19, x20, [x1, #152]
    stp x21, x22, [x1, #168]
    stp x23, x24, [x1, #184]
    stp x25, x26, [x1, #200]
    stp x27, x28, [x1, #216]
    str x29, [x1, #232]
    add x2, x1, #288
    ldp q0, q1, [sp, #80]
    stp q0, q1, [x2, #0]
    ldp q0, q1, [sp, #112]
    stp q0, q1, [x2, #32]
    ldp q0, q1, [sp, #144]
    stp q0, q1, [x2, #64]
    ldp q0, q1, [sp, #176]
    stp q0, q1, [x2, #96]
    add x2, sp, #224
    str x2, [x1, #248]
    mrs x2, fpcr
    str x2, [x0, #24]
    // Back to the host: its FPCR, x18 and stack, then return from
    // lgp_native_enter.
    ldr x2, [x0, #16]
    msr fpcr, x2
    ldr x18, [x0, #8]
    ldr x2, [x0, #0]
    mov sp, x2
    ldp x19, x20, [sp, #0]
    ldp x21, x22, [sp, #16]
    ldp x23, x24, [sp, #32]
    ldp x25, x26, [sp, #48]
    ldp x27, x28, [sp, #64]
    ldp x29, x30, [sp, #80]
    ldp d8, d9, [sp, #96]
    ldp d10, d11, [sp, #112]
    ldp d12, d13, [sp, #128]
    ldp d14, d15, [sp, #144]
    add sp, sp, #176
    ret

    .p2align 4
    .globl )" LGP_SYM(lgp_native_gate_host) R"(
)" LGP_SYM(lgp_native_gate_host) R"(:
    // Host function of the gate (native_gate.hpp): x0 = frame = SP, x30 =
    // return into the gate entry. Every register but SP may be clobbered.
    // Host code runs with the host FPCR; the guest FPCR waits in
    // guest_fpcr (a guest rounding mode must not reach HLE arithmetic).
    mov x19, x0
    mov x20, x30
    bl )" LGP_SYM(lgp_native_thread) R"(
    mov x21, x0
    mrs x9, fpcr
    str x9, [x0, #24]
    ldr x9, [x0, #16]
    msr fpcr, x9
    ldr x18, [x0, #8]
    ldr x9, [x0, #0]
    mov sp, x9
    mov x1, x19
    bl )" LGP_SYM(lgp_native_gate_dispatch) R"(
    ldr x9, [x21, #24]
    msr fpcr, x9
    mov sp, x19
    mov x30, x20
    ret
)");
// clang-format on

namespace lgp::rt::native {
namespace {

uint64_t read_fpcr() {
    uint64_t v;
    asm volatile("mrs %0, fpcr" : "=r"(v));
    return v;
}

void write_fpcr(uint64_t v) { asm volatile("msr fpcr, %0" : : "r"(v)); }

// Leaves guest code from the gate: ctx gets the state at the site, run()
// continues after its setjmp. Runs on the host stack.
[[noreturn]] void leave(NativeThread* nt, const GateFrame& f) {
    frame_to_ctx(f, nt->ctx, 32);
    nt->ctx->fpcr = static_cast<uint32_t>(nt->guest_fpcr);
    siglongjmp(nt->exit, 1);
}

bool in_guest_memory(uint64_t pc) { return pc - g_view.guest_base < g_view.size; }

// Signal filter (native_signals.hpp), in the handler: a signal whose PC lies
// in guest memory (module text or patch area) belongs to the guest thread.
// The thread continues in lgp_native_fault_exit on its host stack, which ends
// the guest thread like a halt of lifted code. A kick outside guest code is
// dropped: such a thread is in the runtime and leaves at its SVC.
//
// A kick while the process is paused parks the thread right here, in the
// handler, until resume() (or the exit). Not async-signal-safe by the book
// (scheduler mutex and condition variable), but the interrupted code is guest
// code, which holds no host lock and is inside no host function.
bool on_signal(platform::SignalKind kind, uint64_t addr, platform::SignalContext& c) {
    NativeThread* nt = &t_native;
    if (nt->ctx == nullptr)
        return kind == platform::SignalKind::Kick;
    // A jump out of guest memory (unmapped or host memory) faults on the
    // fetch: the fault address is the PC. In phase kInGuest only guest code
    // and the entry trampoline run, and the trampoline jumps nowhere computed.
    const bool fetch_fault = (kind == platform::SignalKind::Segv || kind == platform::SignalKind::Bus) &&
                             addr == c.pc && __atomic_load_n(&nt->phase, __ATOMIC_ACQUIRE) == kInGuest;
    if (!in_guest_memory(c.pc) && !fetch_fault)
        return kind == platform::SignalKind::Kick;
    if (kind == platform::SignalKind::Kick && g_backedge_stop.load(std::memory_order_relaxed) == 0) {
        Scheduler& sched = active_kernel().scheduler();
        KThread* self = Scheduler::current();
        const bool slice = __atomic_exchange_n(&nt->preempt, 0u, __ATOMIC_ACQ_REL) != 0;
        if (self == nullptr || (!slice && !sched.paused()))
            return true;
        bump(t_counts.kicks);
        // For thread reports while parked: where the thread stands.
        std::memcpy(nt->ctx->x, c.x, sizeof nt->ctx->x);
        nt->ctx->sp = c.sp;
        nt->ctx->pc = c.pc;
        set_phase(*nt, kParked);
        // End of a time slice (kicker_main()): what a lifted back-edge poll
        // does, for code that ran this long without reaching the gate.
        if (slice) {
            if (sched.mode() == SchedMode::Baton)
                sched.yield_hint(*self);
            else
                sched.preempt_poll(*self);
        }
        if (sched.paused())
            sched.pause_point(*self);
        set_phase(*nt, kInGuest);
        if (g_backedge_stop.load(std::memory_order_relaxed) == 0)
            return true;  // on with the interrupted code
        // Ended meanwhile: leave as below.
    }
    nt->fault_kind = kind;
    nt->fault_addr = addr;
    nt->fault = c;
    c.x[0] = reinterpret_cast<uint64_t>(nt);
    c.x[18] = nt->host_x18;
    c.sp = nt->host_sp;
    c.pc = reinterpret_cast<uint64_t>(&lgp_native_fault_exit);
    return true;
}

// Host threads currently inside the outermost run() (kicked on exit and pause).
struct Registered {
    uint64_t token;
    NativeThread* nt;
    uint64_t seen_gates = ~uint64_t{0};  // kicker_main(): gate count at the last slice scan
    Counts* counts;
};
// Never destroyed, like KickRequests: the detached kicker scans them every
// slice (slice_scan()) and may still do so while the process runs its static
// destructors (macOS: "mutex lock failed: Invalid argument" at exit).
std::mutex& g_threads_mutex = *new std::mutex;
std::vector<Registered>& g_threads = *new std::vector<Registered>;
Counts g_retired;  // under g_threads_mutex

// Kicks the threads in guest code; false when there are none.
bool kick_guest_threads() {
    // Pairs with the fence in to_guest(): the request (set before the kicker
    // woke) or the thread's kInGuest is seen (Dekker).
    std::atomic_thread_fence(std::memory_order_seq_cst);
    std::lock_guard lock(g_threads_mutex);
    bool any = false;
    for (const Registered& r : g_threads) {
        if (__atomic_load_n(&r.nt->phase, __ATOMIC_ACQUIRE) != kInGuest)
            continue;
        platform::kick_thread(r.token);
        any = true;
    }
    return any;
}

// The kicker thread: after an exit or pause request it kicks the threads in
// guest code every 2 ms until none is left (at most 2 s, then pause() has
// timed out long ago). One kick per request is not enough: a thread on its way
// from the gate into guest code drops it and would run on without an SVC.
// A thread the scan finds outside guest code sees the request itself on its
// way back (to_guest()).
//
// Never destroyed: the detached kicker still waits on the condition variable
// when the process exits (glibc's destructor would wait for it).
struct KickRequests {
    std::mutex mutex;
    std::condition_variable cv;
    uint64_t count = 0;
};
KickRequests& kick_requests() {
    static KickRequests* k = new KickRequests;
    return *k;
}
std::atomic<bool> g_paused{false};
// A scheduler with slots (Baton, CoreBaton) runs native code: the kicker ends
// time slices (slice_scan()).
std::atomic<bool> g_slices{false};
constexpr auto kSlice = std::chrono::milliseconds(2);

// Native code has no back-edge polls. A thread that was in guest code at the
// last scan and has not passed the gate since has run a whole slice without
// a scheduling point (a spin-wait without SVC under the single baton would
// keep its slot forever): it gets a kick that hands the slot on (on_signal()).
// Not deterministic: --clock virtual runs natively are not bit-for-bit
// repeatable.
void slice_scan() {
    std::atomic_thread_fence(std::memory_order_seq_cst);
    std::lock_guard lock(g_threads_mutex);
    for (Registered& r : g_threads) {
        const uint64_t gates = __atomic_load_n(&r.nt->gates, __ATOMIC_RELAXED);
        const bool in_guest = __atomic_load_n(&r.nt->phase, __ATOMIC_ACQUIRE) == kInGuest;
        if (in_guest && gates == r.seen_gates) {
            __atomic_store_n(&r.nt->preempt, 1u, __ATOMIC_RELEASE);
            platform::kick_thread(r.token);
        }
        r.seen_gates = in_guest ? gates : ~uint64_t{0};
    }
}

void kicker_main() {
    KickRequests& k = kick_requests();
    uint64_t seen = 0;
    for (;;) {
        {
            std::unique_lock lock(k.mutex);
            const auto requested = [&] { return k.count != seen; };
            if (g_slices.load(std::memory_order_relaxed)) {
                if (!k.cv.wait_for(lock, kSlice, requested)) {
                    lock.unlock();
                    slice_scan();
                    continue;
                }
            } else {
                k.cv.wait(lock, [&] { return requested() || g_slices.load(std::memory_order_relaxed); });
            }
            seen = k.count;
        }
        for (int i = 0; i < 1000; ++i) {
            const bool wanted = g_backedge_stop.load(std::memory_order_relaxed) != 0 ||
                                g_paused.load(std::memory_order_acquire);
            if (!wanted || !kick_guest_threads())
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
}

}  // namespace
}  // namespace lgp::rt::native

namespace lgp::rt::native {
namespace {

// The step from host code (gate, run()) into guest code: kInGuest first, then
// the requests, so an exit or pause is either seen here or the thread is
// kicked (fence in kick_guest_threads()). A pause is waited out here; false:
// the process ends, do not enter.
bool to_guest(NativeThread& nt) {
    for (;;) {
        set_phase(nt, kInGuest);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        if (g_backedge_stop.load(std::memory_order_relaxed) != 0) {
            set_phase(nt, kInHost);
            return false;
        }
        KThread* self = Scheduler::current();
        if (!g_paused.load(std::memory_order_relaxed) || self == nullptr)
            return true;
        set_phase(nt, kInHost);
        active_kernel().scheduler().pause_point(*self);
    }
}

void on_interrupt(GuestInterrupt what) {
    g_paused.store(what == GuestInterrupt::Pause, std::memory_order_release);
    if (what == GuestInterrupt::Resume)
        return;  // parked threads wake up in the scheduler
    KickRequests& k = kick_requests();
    std::lock_guard lock(k.mutex);
    ++k.count;
    k.cv.notify_one();
}

void install_handlers() {
    static std::once_flag once;
    std::call_once(once, [] {
        if (!platform::install_signal_filter(&on_signal))
            log(LogLevel::Warn, "native execution: no signal handlers, a fault in guest code ends the process");
        std::thread(kicker_main).detach();
        set_interrupt_listener(&on_interrupt);
    });
}

}  // namespace
}  // namespace lgp::rt::native

extern "C" void lgp_native_fault_exit(NativeThread* nt) {
    using namespace lgp;
    using namespace lgp::rt;
    using namespace lgp::rt::native;
    using platform::SignalKind;
    CpuState* ctx = nt->ctx;
    const platform::SignalContext& c = nt->fault;
    std::memcpy(ctx->x, c.x, sizeof ctx->x);
    ctx->sp = c.sp;
    ctx->pc = c.pc;
    unpack_nzcv(*ctx, c.pstate);
    ctx->fpcr = static_cast<uint32_t>(read_fpcr());
    uint32_t word = 0;
    if (nt->fault_kind == SignalKind::Ill || nt->fault_kind == SignalKind::Trap)
        std::memcpy(&word, reinterpret_cast<const void*>(static_cast<uintptr_t>(c.pc)), 4);
    switch (nt->fault_kind) {
    case SignalKind::Segv:
    case SignalKind::Bus:
        // Fetch fault: a branch to an address without guest code, as lifted
        // code reports it.
        if (nt->fault_addr == c.pc)
            set_halt(ctx, kHaltBadJump, c.pc);
        else
            set_halt(ctx, kHaltMemFault, nt->fault_addr);
        break;
    case SignalKind::Trap: set_halt(ctx, kHaltBrk, (word >> 5) & 0xFFFF); break;
    case SignalKind::Kick: set_halt(ctx, kHaltExit, 0); break;
    case SignalKind::Ill:
    case SignalKind::Fpe: set_halt(ctx, kHaltUndefined, word); break;
    }
    write_fpcr(nt->host_fpcr);
    siglongjmp(nt->exit, 1);
}

namespace {
void gate_sysreg(NativeThread* nt, lgp::recomp::native_gate::GateFrame& f);
void gate_host_call(NativeThread* nt, lgp::recomp::native_gate::GateFrame& f);
}  // namespace

extern "C" void lgp_native_gate_dispatch(NativeThread* nt, lgp::recomp::native_gate::GateFrame* f) {
    using namespace lgp;
    using namespace lgp::rt;
    using namespace lgp::rt::native;
    CpuState* ctx = nt->ctx;
    const uint32_t w = f->word;
    set_phase(*nt, kInHost);
    __atomic_store_n(&nt->gates, nt->gates + 1, __ATOMIC_RELAXED);
    nt->stack_floor = reinterpret_cast<uint64_t>(f);
    if (w == g::kHostCallWord) {
        bump(t_counts.host_calls);
        gate_host_call(nt, *f);
    } else if ((w & 0xFFE0001Fu) == 0xD4000001u) {  // SVC #imm
        bump(t_counts.svcs);
        const uint32_t imm = (w >> 5) & 0xFFFF;
        // The thread's CpuState is what the rest of the runtime sees of it
        // (crash reports, thread context queries).
        frame_to_ctx(*f, ctx, 32);
        switch (active_kernel().svc(imm, f->x)) {
        case SvcOutcome::Continue:
            break;
        case SvcOutcome::ExitProcess:
        case SvcOutcome::Break:
        case SvcOutcome::ExitThread:
            set_halt(ctx, kHaltExit, imm);
            leave(nt, *f);
        case SvcOutcome::Unimplemented:
            set_halt(ctx, kHaltUndefined, w);
            leave(nt, *f);
        }
    } else {
        bump(t_counts.sysregs);
        gate_sysreg(nt, *f);
    }
    // A kick that arrived while this thread was in the gate found it outside
    // guest code; an exit or pause is noticed here instead.
    if (!to_guest(*nt)) {
        set_halt(ctx, kHaltExit, 0);
        leave(nt, *f);
    }
}

namespace {
// A call of the host function registered at f.pc (native_gate::kHostCallWord):
// it runs on the thread's CpuState like a call from lifted code, and may call
// guest code itself (run() nests). Its registers become the guest's; the
// stub returns to x30.
void gate_host_call(NativeThread* nt, lgp::recomp::native_gate::GateFrame& fr) {
    using namespace lgp;
    using namespace lgp::rt;
    using namespace lgp::rt::native;
    GateFrame* f = &fr;
    CpuState* ctx = nt->ctx;
    frame_to_ctx(*f, ctx, 32);
    ctx->fpcr = static_cast<uint32_t>(nt->guest_fpcr);
    // The table only: the resolver answers run() for guest code.
    const LiftedFn fn = table_lookup(f->pc);
    if (fn == nullptr) {
        set_halt(ctx, kHaltBadJump, f->pc);
        leave(nt, *f);
    }
    fn(ctx);
    nt->guest_fpcr = ctx->fpcr;
    if (ctx->halt != kHaltNone)
        leave(nt, *f);
    std::memcpy(f->x, ctx->x, sizeof f->x);
    f->nzcv = pack_nzcv(*ctx);
    f->fpsr = ctx->fpsr;
    for (unsigned i = 0; i < 32; ++i) {
        f->q[i][0] = ctx->vreg[i].lo;
        f->q[i][1] = ctx->vreg[i].hi;
    }
}

// MRS/MSR at a gate.
void gate_sysreg(NativeThread* nt, lgp::recomp::native_gate::GateFrame& fr) {
    using namespace lgp;
    using namespace lgp::rt;
    using namespace lgp::rt::native;
    GateFrame* f = &fr;
    CpuState* ctx = nt->ctx;
    const uint32_t w = f->word;
    const uint32_t enc = sysreg_of(w);
    const uint32_t rt = w & 31;
    if ((w & 0xFFF00000u) == 0xD5300000u) {  // MRS Xt, <sysreg>
        uint64_t v;
        if (enc == kTpidrEl0)
            v = ctx->tpidr_el0;
        else if (enc == kTpidrroEl0)
            v = ctx->tpidrro_el0;
        else
            v = read_sysreg(enc);  // counters; aborts on anything else
        if (rt != 31)
            f->x[rt] = v;
        // Native code has no back-edge polls: under the single baton a
        // spin-wait without SVC would keep the baton forever (lifted code
        // hands it over in backedge_poll()). One that reads the counter
        // polls here, every 64th read, so runs stay deterministic. An exit
        // it notices is taken by to_guest() after the gate.
        if (enc != kTpidrEl0 && enc != kTpidrroEl0 && (++nt->counter_reads & 63) == 0) {
            frame_to_ctx(*f, ctx, 32);
            backedge_poll(ctx);
        }
    } else if ((w & 0xFFF00000u) == 0xD5100000u && enc == kTpidrEl0) {  // MSR TPIDR_EL0, Xt
        ctx->tpidr_el0 = rt == 31 ? 0 : f->x[rt];
    } else {
        log(LogLevel::Error, "native gate: unexpected instruction 0x%08x at 0x%llx", w,
            static_cast<unsigned long long>(f->pc));
        set_halt(ctx, kHaltUndefined, w);
        leave(nt, *f);
    }
}
}  // namespace

namespace lgp::rt::native {

bool available() { return true; }

Stats stats() {
    std::lock_guard lock(g_threads_mutex);
    Counts sum = g_retired;
    for (const Registered& r : g_threads) {
        const Counts& c = *r.counts;
        sum.entries += __atomic_load_n(&c.entries, __ATOMIC_RELAXED);
        sum.svcs += __atomic_load_n(&c.svcs, __ATOMIC_RELAXED);
        sum.host_calls += __atomic_load_n(&c.host_calls, __ATOMIC_RELAXED);
        sum.sysregs += __atomic_load_n(&c.sysregs, __ATOMIC_RELAXED);
        sum.kicks += __atomic_load_n(&c.kicks, __ATOMIC_RELAXED);
    }
    return {sum.entries, sum.svcs, sum.host_calls, sum.sysregs, sum.kicks};
}

void install_gate(uint64_t host_fn_slot) {
    const uint64_t fn = reinterpret_cast<uint64_t>(&lgp_native_gate_host);
    std::memcpy(reinterpret_cast<void*>(static_cast<uintptr_t>(host_fn_slot)), &fn, sizeof fn);
}

namespace {
void enter(CpuState* ctx, NativeThread& nt);
}  // namespace

void run(CpuState* ctx) {
    NativeThread& nt = t_native;
    // A nested call (guest code called from inside an SVC) keeps the outer
    // call's host stack, state and exit point.
    NativeThread outer;
    std::memcpy(static_cast<void*>(&outer), static_cast<const void*>(&nt), sizeof nt);
    const bool outermost = outer.ctx == nullptr;
    install_handlers();
    platform::ensure_signal_stack();
    if (outermost && Scheduler::current() != nullptr &&
        active_kernel().scheduler().mode() != SchedMode::Free && !g_slices.exchange(true)) {
        KickRequests& k = kick_requests();  // the kicker starts scanning
        std::lock_guard lock(k.mutex);
        k.cv.notify_one();
    }
    if (outermost) {
        std::lock_guard lock(g_threads_mutex);
        g_threads.push_back({platform::current_thread_token(), &nt, ~uint64_t{0}, &t_counts});
    }
    enter(ctx, nt);
    if (outermost) {
        std::lock_guard lock(g_threads_mutex);
        g_threads.erase(std::find_if(g_threads.begin(), g_threads.end(), [&](const Registered& r) { return r.nt == &nt; }));
        g_retired.entries += t_counts.entries;
        g_retired.svcs += t_counts.svcs;
        g_retired.host_calls += t_counts.host_calls;
        g_retired.sysregs += t_counts.sysregs;
        g_retired.kicks += t_counts.kicks;
        t_counts = {};
    }
    std::memcpy(static_cast<void*>(&nt), static_cast<const void*>(&outer), sizeof nt);
}

}  // namespace lgp::rt::native

namespace lgp::rt::native {
namespace {

void enter(CpuState* ctx, NativeThread& nt) {

    GateFrame st{};
    std::memcpy(st.x, ctx->x, sizeof st.x);
    // Called from inside the gate: the gate frame below ctx->sp stays intact.
    const uint64_t sp = ctx->sp;
    const uint64_t given = nt.stack_floor != 0 && sp > nt.stack_floor ? nt.stack_floor : sp;
    st.sp = given;
    st.pc = ctx->pc;
    st.nzcv = pack_nzcv(*ctx);
    st.fpsr = ctx->fpsr;
    for (int i = 0; i < 32; ++i) {
        st.q[i][0] = ctx->vreg[i].lo;
        st.q[i][1] = ctx->vreg[i].hi;
    }
    const uint64_t return_to = ctx->x[30];
    st.x[30] = reinterpret_cast<uint64_t>(&lgp_native_return);
    nt.guest_fpcr = ctx->fpcr;
    nt.state = &st;
    nt.ctx = ctx;
    bump(t_counts.entries);
    // Saving the signal mask costs a system call per entry and is not needed:
    // leave() runs outside signal handlers, and a fault leaves its handler
    // before lgp_native_fault_exit (sigreturn restores the mask). Kept for a
    // comparison (system time, docs/knowledge/originalcode-nativ-ausfuehren.md).
    static const int save_mask = [] {
        const char* v = std::getenv("LGP_NATIVE_SAVE_MASK");
        return v != nullptr && v[0] == '1' ? 1 : 0;
    }();
    if (sigsetjmp(nt.exit, save_mask) == 0) {
        // Also covers an exit requested before this thread was registered
        // (no kick reached it).
        if (!to_guest(nt)) {
            set_halt(ctx, kHaltExit, 0);
            return;
        }
        lgp_native_enter(&st, &nt);
        set_phase(nt, kInHost);
        // Returned: callee-saved registers, results and SP as the code left
        // them; the scratch registers have no defined value after a return.
        frame_to_ctx(st, ctx, 8);
        if (ctx->sp == given)
            ctx->sp = sp;  // returned with the SP it was given: the caller's
        ctx->x[30] = return_to;
        ctx->pc = return_to;
        ctx->fpcr = static_cast<uint32_t>(nt.guest_fpcr);
    } else {
        // leave() or a signal: ctx holds the state at the site, halt is set.
        set_phase(nt, kInHost);
        write_fpcr(nt.host_fpcr);
    }
}

}  // namespace

}  // namespace lgp::rt::native

#else  // !LGP_NATIVE_EXEC

namespace lgp::rt::native {

bool available() { return false; }

Stats stats() { return {}; }

void install_gate(uint64_t) {}

void run(CpuState* ctx) {
    log(LogLevel::Error, "native execution of guest code needs an AArch64 host");
    if (ctx->halt == kHaltNone) {
        ctx->halt = kHaltUndefined;
        ctx->halt_info = ctx->pc;
    }
}

}  // namespace lgp::rt::native

#endif
