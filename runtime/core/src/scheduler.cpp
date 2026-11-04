// Scheduler core: thread creation, slots, blocking and waking (P3.2, ADR-0002).
// The synchronization svcs built on top of it are in synchronization.cpp.
#include <algorithm>
#include <bit>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <optional>
#include <thread>
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <intrin.h>
#endif

#include "lgp/platform/host_thread.hpp"
#include "lgp/runtime/guest.hpp"
#include "lgp/runtime/host_work.hpp"
#include "lgp/runtime/kernel.hpp"
#include "lgp/runtime/log.hpp"
#include "lgp/runtime/thread.hpp"
#include "lgp/runtime/time_stats.hpp"

namespace lgp::rt {
namespace {

namespace kr = kernel_result;

constexpr uint64_t kTlsSize = 0x200;
// Host stack per guest thread: twice the guest stack (lifted frames are not
// guest frames; the ratio is a guess until P3.4 measures real code), at
// least ProcessConfig::min_host_stack_size, at most this.
constexpr uint64_t kMaxHostStack = 16ull << 20;

thread_local KThread* t_current = nullptr;

}  // namespace

const char* sched_mode_name(SchedMode mode) {
    switch (mode) {
    case SchedMode::Free: return "free";
    case SchedMode::CoreBaton: return "core-baton";
    case SchedMode::Baton: return "baton";
    }
    return "?";
}

const char* exit_reason_name(ProcessExit::Reason reason) {
    switch (reason) {
    case ProcessExit::Reason::ExitProcess: return "ExitProcess";
    case ProcessExit::Reason::Break: return "Break";
    case ProcessExit::Reason::Crash: return "Crash";
    case ProcessExit::Reason::AllThreadsExited: return "AllThreadsExited";
    case ProcessExit::Reason::StartFailed: return "StartFailed";
    case ProcessExit::Reason::Stopped: return "Stopped";
    }
    return "?";
}

std::atomic<uint32_t> g_backedge_stop{0};

namespace detail {
void notify_interrupt_listener(GuestInterrupt what);  // system.cpp (set_interrupt_listener)
}  // namespace detail

Scheduler::Scheduler(Kernel& kernel, SchedMode mode)
    : kernel_(kernel), mode_(mode), virtual_clock_(kernel.config().clock_mode == ClockMode::Virtual) {
    g_backedge_stop.store(0, std::memory_order_relaxed);
}

// " 0x<lr> 0x<lr> ...": return addresses along the guest frame-pointer chain
// (AAPCS64 frame records {previous x29, x30}), at most 16.
std::string Scheduler::frame_chain(uint64_t fp) {
    std::string out;
    for (int i = 0; i < 16 && fp != 0 && (fp & 0xF) == 0; ++i) {
        uint64_t rec[2] = {};
        if (!kernel_.memory().copy_from_user(rec, fp, sizeof(rec)))
            break;
        char buf[24];
        std::snprintf(buf, sizeof(buf), " 0x%" PRIx64, rec[1]);
        out += buf;
        if (rec[0] <= fp)
            break;  // frames grow towards higher addresses
        fp = rec[0];
    }
    return out;
}

std::vector<std::string> Scheduler::thread_report() {
    static const char* kState[] = {"created", "ready", "running", "waiting", "terminated"};
    static const char* kWait[] = {"", " sync", " mutex", " condvar", " sleep", " address"};
    std::vector<std::shared_ptr<KThread>> threads;
    {
        Lock lock(lock_);
        threads = threads_;
    }
    std::vector<std::string> out;
    for (const auto& t : threads) {
        uint8_t state, wait;
        int32_t priority, core;
        uint64_t key, mutex, affinity;
        {
            Lock lock(lock_);
            state = static_cast<uint8_t>(t->state_);
            wait = static_cast<uint8_t>(t->wait_kind_);
            priority = t->priority_;
            core = t->core_;
            affinity = t->affinity_;
            key = t->cv_key_;
            mutex = t->mutex_addr_;
        }
        const CpuState& c = t->cpu_;
        char line[256];
        std::snprintf(line, sizeof(line),
                      "thread %" PRIu64 ": %s%s, priority %d, core %d (mask 0x%" PRIx64 "), key 0x%" PRIx64
                      ", mutex 0x%" PRIx64 ", pc 0x%" PRIx64 ", x30 0x%" PRIx64 ", frames",
                      t->id_, kState[state < 5 ? state : 0], kWait[wait < 6 ? wait : 0], priority, core, affinity, key,
                      mutex, c.pc, c.x[30]);
        std::string owner;
        if (wait == static_cast<uint8_t>(KThread::WaitKind::Mutex))
            if (const uint32_t* word = user_word(mutex)) {
                const uint32_t tag = *word & ~kMutexHasWaiters;
                const std::shared_ptr<KThread> o = tag != 0 ? thread_from_handle(Handle(tag), nullptr) : nullptr;
                char buf[64];
                std::snprintf(buf, sizeof(buf), " (owner tag 0x%x = thread %" PRIu64 ")", tag, o ? o->id_ : 0);
                owner = buf;
            }
        out.push_back(line + owner + frame_chain(c.x[29]));
    }
    return out;
}

Scheduler::~Scheduler() {
    // run() joins everything; this only matters if a run was cut short or
    // threads were started without run().
    bool any = false;
    {
        Lock lock(lock_);
        any = live_host_threads_ != 0;
    }
    if (any)
        request_exit(nullptr, ProcessExit::Reason::ExitProcess);
    std::vector<std::unique_ptr<HostThread>> hosts;
    {
        Lock lock(lock_);
        hosts.swap(host_threads_);
    }
    hosts.clear();
}

KThread* Scheduler::current() { return t_current; }

std::shared_ptr<KThread> Scheduler::thread_from_handle(Handle handle, KThread* self) const {
    if (handle == kPseudoHandleCurrentThread)
        return self != nullptr ? self->shared_from_this() : nullptr;
    return kernel_.handles().get_as<KThread>(handle);
}

// --- thread-local storage -----------------------------------------------------

Result Scheduler::allocate_tls(uint64_t* out) {
    std::lock_guard tls_lock(tls_mutex_);
    if (free_tls_.empty()) {
        std::lock_guard mem_lock(kernel_.memory_mutex());
        const auto page = kernel_.memory().find_free(kernel_.memory().layout().misc, kPageSize);
        if (!page)
            return kr::kOutOfMemory;
        const Result rc = kernel_.map_owned(*page, kPageSize, MemState::ThreadLocal, kPermRW);
        if (rc.failed())
            return rc;
        tls_pages_.push_back(*page);
        // Lowest address handed out first.
        for (uint64_t off = kPageSize - kTlsSize; off != 0; off -= kTlsSize)
            free_tls_.push_back(*page + off);
        free_tls_.push_back(*page);
    }
    const uint64_t addr = free_tls_.back();
    free_tls_.pop_back();
    std::lock_guard mem_lock(kernel_.memory_mutex());
    std::memset(kernel_.memory().host_ptr(addr, kTlsSize), 0, kTlsSize);
    *out = addr;
    return kSuccess;
}

void Scheduler::free_tls(uint64_t addr) {
    std::lock_guard tls_lock(tls_mutex_);
    free_tls_.push_back(addr);
}

// --- slots and queues -----------------------------------------------------------

int Scheduler::slot_of(const KThread& t) const {
    switch (mode_) {
    case SchedMode::Free: return -1;
    case SchedMode::CoreBaton: return t.core_;
    case SchedMode::Baton: return 0;
    }
    return -1;
}

KThread* Scheduler::best_ready(int slot, const KThread* exclude) const {
    KThread* best = nullptr;
    for (KThread* t : ready_) {
        if (t == exclude || slot_of(*t) != slot)
            continue;
        if (best == nullptr || t->priority_ < best->priority_ ||
            (t->priority_ == best->priority_ && t->ready_seq_ < best->ready_seq_))
            best = t;
    }
    return best;
}

namespace {
std::atomic<uint64_t> g_wake_ns{0}, g_wakes{0};

uint64_t steady_ns() {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count());
}
}  // namespace

void Scheduler::mark_dispatched(KThread& t) {
    if (time_stats_enabled() && &t != current())
        t.dispatched_ns_ = steady_ns();
}

void scheduler_wake_latency(uint64_t* total_ns, uint64_t* count) {
    *total_ns = g_wake_ns.load(std::memory_order_relaxed);
    *count = g_wakes.load(std::memory_order_relaxed);
}

void Scheduler::make_ready(KThread& t, bool front) {
    t.ready_seq_ = front ? --front_seq_ : ++next_seq_;
    if (mode_ == SchedMode::Free) {
        t.state_ = ThreadState::Running;
        mark_dispatched(t);
        notify(t);
        return;
    }
    t.state_ = ThreadState::Ready;
    ready_.push_back(&t);
    dispatch(slot_of(t));
}

void Scheduler::dispatch(int slot) {
    if (slot < 0 || slots_[slot] != nullptr || hold_dispatch_ != 0)
        return;
    KThread* next = best_ready(slot, nullptr);
    if (next == nullptr)
        return;
    ready_.erase(std::find(ready_.begin(), ready_.end(), next));
    slots_[slot] = next;
    next->held_slot_ = slot;
    next->state_ = ThreadState::Running;
    mark_dispatched(*next);
    notify(*next);
}

void Scheduler::release_slot(KThread& t) {
    const int slot = t.held_slot_;
    if (slot < 0)
        return;
    slots_[slot] = nullptr;
    t.held_slot_ = -1;
    dispatch(slot);
}

void Scheduler::wait_for_running(KThread& t, Lock& lock) {
    if (t.state_ != ThreadState::Running && !exiting()) {
        TimeScope ready(TimeCat::Ready);
        while (t.state_ != ThreadState::Running && !exiting())
            park(t, lock);
    }
    if (t.dispatched_ns_ != 0) {
        const uint64_t now = steady_ns();
        if (now > t.dispatched_ns_) {
            g_wake_ns.fetch_add(now - t.dispatched_ns_, std::memory_order_relaxed);
            g_wakes.fetch_add(1, std::memory_order_relaxed);
        }
        t.dispatched_ns_ = 0;
    }
    pause_gate(t, lock);
}

void Scheduler::notify(KThread& t) {
    t.wake_gen_.fetch_add(1, std::memory_order_release);
    t.cv_.notify_all();
}

namespace {
// Tells the core it is in a spin loop (less power, no penalty when the loop
// ends); a no-op where the compiler offers no hint.
inline void spin_pause() {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
    _mm_pause();
#elif defined(__aarch64__) && (defined(__GNUC__) || defined(__clang__))
    __asm__ __volatile__("yield");
#elif (defined(__x86_64__) || defined(__i386__)) && (defined(__GNUC__) || defined(__clang__))
    __builtin_ia32_pause();
#endif
}
}  // namespace

bool Scheduler::spin_for_wake(KThread& t, Lock& lock, std::chrono::steady_clock::time_point deadline) {
    using Clock = std::chrono::steady_clock;
    // Every notify happens under the lock, so one that comes after the lock
    // is taken again finds the thread in cv_.wait(): checking the counter
    // once more with the lock held misses nothing.
    const uint32_t gen = t.wake_gen_.load(std::memory_order_acquire);
    const Clock::time_point until = std::min(deadline, Clock::now() + std::chrono::nanoseconds(park_spin_ns_));
    lock.unlock();
    // A wake soon after the wait starts finds the core still awake: the
    // handover skips the wakeup from an idle state (P7.3, Android).
    while (t.wake_gen_.load(std::memory_order_acquire) == gen && !exiting() && Clock::now() < until)
        spin_pause();
    lock.lock();
    return t.wake_gen_.load(std::memory_order_acquire) != gen;
}

// A return without a notify is a spurious wakeup to the callers: they check
// their condition again, as after any wait on a condition variable.
void Scheduler::park(KThread& t, Lock& lock) {
    --active_;
    if (paused())
        pause_cv_.notify_all();
    if (park_spin_ns_ == 0 || !spin_for_wake(t, lock, std::chrono::steady_clock::time_point::max()))
        t.cv_.wait(lock);
    ++active_;
}

bool Scheduler::park_until(KThread& t, Lock& lock, std::chrono::steady_clock::time_point deadline) {
    --active_;
    if (paused())
        pause_cv_.notify_all();
    bool timed_out = false;
    if (park_spin_ns_ == 0 || !spin_for_wake(t, lock, deadline))
        timed_out = t.cv_.wait_until(lock, deadline) == std::cv_status::timeout;
    ++active_;
    return timed_out;
}

void Scheduler::pause_gate(KThread& t, Lock& lock) {
    if (!paused())
        return;
    TimeScope paused_time(TimeCat::Wait);
    while (paused() && !exiting())
        park(t, lock);
}

void Scheduler::requeue(KThread& self, Lock& lock, bool front) {
    if (mode_ == SchedMode::Free)
        return;
    release_slot(self);
    make_ready(self, front);
    wait_for_running(self, lock);
}

bool Scheduler::leave_slot(KThread& self) {
    Lock lock(lock_);
    if (mode_ != SchedMode::CoreBaton || self.held_slot_ < 0)
        return false;
    // Stays Running without a slot: no wait object, nothing wakes it, the
    // virtual clock (not used in this mode) would not count it as idle.
    release_slot(self);
    return true;
}

void Scheduler::return_to_slot(KThread& self) {
    Lock lock(lock_);
    make_ready(self, /*front=*/true);
    wait_for_running(self, lock);
}

namespace {
std::atomic<bool> g_host_work_release{false};
}  // namespace

void enable_host_work_release(bool on) { g_host_work_release.store(on, std::memory_order_relaxed); }
bool host_work_release_enabled() { return g_host_work_release.load(std::memory_order_relaxed); }

HostWorkScope::HostWorkScope(bool force) {
    if (!force && !host_work_release_enabled())
        return;
    if (KThread* self = Scheduler::current(); self != nullptr)
        left_ = active_kernel().scheduler().leave_slot(*self);
}

HostWorkScope::~HostWorkScope() {
    if (left_)
        active_kernel().scheduler().return_to_slot(*Scheduler::current());
}

void Scheduler::preempt_check(KThread& self, Lock& lock) {
    if (self.held_slot_ < 0)
        return;
    const KThread* best = best_ready(self.held_slot_, &self);
    // Running thread moved to another core (svcSetThreadCoreMask): migrate.
    const bool moved = slot_of(self) != self.held_slot_;
    if (moved || (best != nullptr && best->priority_ < self.priority_))
        requeue(self, lock, /*front=*/true);
}

Result Scheduler::block(KThread& t, Lock& lock, int64_t timeout_ns) {
    using Clock = std::chrono::steady_clock;
    t.wait_result_ = kr::kTimedOut;
    t.wait_index_ = -1;
    t.state_ = ThreadState::Waiting;
    release_slot(t);
    // Until woken; the time until the slot is back counts as Ready.
    std::optional<TimeScope> waiting(std::in_place, TimeCat::Wait);
    // Negative timeouts (Horizon: -1 = forever) and anything beyond ~290 years wait forever.
    const bool finite = timeout_ns > 0 && timeout_ns < (int64_t{1} << 62);
    if (virtual_clock_) {
        // Guest time only moves when the guest works or every thread waits:
        // whoever moves it past the deadline times this wait out.
        t.deadline_ns_ = finite ? kernel_.clock().now_ns() + static_cast<uint64_t>(timeout_ns) : GuestClock::kNever;
        t.wait_seq_ = ++next_wait_seq_;
        if (t.deadline_ns_ < earliest_deadline_.load(std::memory_order_relaxed))
            earliest_deadline_.store(t.deadline_ns_, std::memory_order_release);
        while (t.state_ == ThreadState::Waiting) {
            if (exiting()) {
                wake(t, kr::kTerminationRequested);
                break;
            }
            if (!idle_advance(lock))
                park(t, lock);
        }
        waiting.reset();
        wait_for_running(t, lock);
        return t.wait_result_;
    }
    const Clock::time_point deadline =
        finite ? Clock::now() + std::chrono::nanoseconds(timeout_ns) : Clock::time_point::max();
    while (t.state_ == ThreadState::Waiting) {
        if (exiting()) {
            wake(t, kr::kTerminationRequested);
            break;
        }
        if (!finite) {
            park(t, lock);
        } else if (park_until(t, lock, deadline) && t.state_ == ThreadState::Waiting) {
            wake(t, kr::kTimedOut);
        }
    }
    waiting.reset();
    wait_for_running(t, lock);
    return t.wait_result_;
}

// --- virtual clock ------------------------------------------------------------------

bool Scheduler::idle_locked() const {
    bool waiting = false;
    for (const auto& t : threads_) {
        if (t->state_ == ThreadState::Running || t->state_ == ThreadState::Ready)
            return false;
        waiting |= t->state_ == ThreadState::Waiting;
    }
    return waiting;
}

bool Scheduler::idle_advance(Lock& lock) {
    if (!virtual_clock_ || exiting() || timers_running_ || !idle_locked())
        return false;
    GuestClock& clock = kernel_.clock();
    uint64_t next = clock.next_timer_ns();
    for (const auto& t : threads_)
        if (t->state_ == ThreadState::Waiting)
            next = std::min(next, t->deadline_ns_);
    if (next == GuestClock::kNever)
        return false;  // only events from the host can end this wait
    clock.advance_to(next);
    fire_due(lock, clock.now_ns());
    return true;
}

void Scheduler::expire_locked(uint64_t now) {
    std::vector<KThread*> due;
    uint64_t earliest = GuestClock::kNever;
    for (const auto& t : threads_) {
        if (t->state_ != ThreadState::Waiting)
            continue;
        if (t->deadline_ns_ <= now)
            due.push_back(t.get());
        else
            earliest = std::min(earliest, t->deadline_ns_);
    }
    std::sort(due.begin(), due.end(), [](const KThread* a, const KThread* b) {
        return a->deadline_ns_ != b->deadline_ns_ ? a->deadline_ns_ < b->deadline_ns_ : a->wait_seq_ < b->wait_seq_;
    });
    for (KThread* t : due)
        wake(*t, kr::kTimedOut);
    earliest_deadline_.store(earliest, std::memory_order_release);
}

void Scheduler::fire_due(Lock& lock, uint64_t now) {
    GuestClock& clock = kernel_.clock();
    ++hold_dispatch_;
    if (clock.next_timer_ns() <= now && !timers_running_) {
        timers_running_ = true;
        lock.unlock();
        clock.run_due_timers();
        lock.lock();
        timers_running_ = false;
    }
    expire_locked(now);
    if (--hold_dispatch_ == 0)
        for (int slot = 0; slot < kCoreCount; ++slot)
            dispatch(slot);
}

void Scheduler::on_clock_advanced() {
    if (!virtual_clock_)
        return;
    GuestClock& clock = kernel_.clock();
    const uint64_t now = clock.now_ns();
    if (clock.next_timer_ns() > now && earliest_deadline_.load(std::memory_order_acquire) > now)
        return;
    Lock lock(lock_);
    fire_due(lock, now);
}

void Scheduler::unlink_wait(KThread& t) {
    switch (t.wait_kind_) {
    case KThread::WaitKind::Sync:
        for (KSyncObject* o : t.wait_objects_)
            o->waiters_.erase(std::remove(o->waiters_.begin(), o->waiters_.end(), &t), o->waiters_.end());
        t.wait_objects_.clear();
        break;
    case KThread::WaitKind::Mutex:
        mutex_waiters_.erase(std::remove(mutex_waiters_.begin(), mutex_waiters_.end(), &t), mutex_waiters_.end());
        break;
    case KThread::WaitKind::CondVar:
        cv_waiters_.erase(std::remove(cv_waiters_.begin(), cv_waiters_.end(), &t), cv_waiters_.end());
        break;
    case KThread::WaitKind::Address:
        address_waiters_.erase(std::remove(address_waiters_.begin(), address_waiters_.end(), &t),
                               address_waiters_.end());
        break;
    case KThread::WaitKind::None:
    case KThread::WaitKind::Sleep:
        break;
    }
    t.wait_kind_ = KThread::WaitKind::None;
    t.deadline_ns_ = GuestClock::kNever;
}

void Scheduler::wake(KThread& t, Result result, int32_t index) {
    if (t.state_ != ThreadState::Waiting)
        return;
    unlink_wait(t);
    t.wait_result_ = result;
    t.wait_index_ = index;
    make_ready(t, false);
    // In the baton modes the thread may stay queued; it still has to notice
    // that it is no longer Waiting (its timeout must not fire any more).
    notify(t);
}

// --- threads ----------------------------------------------------------------------

Result Scheduler::create_thread(uint64_t entry, uint64_t arg, uint64_t stack_top, int32_t priority, int32_t core,
                                Handle* out) {
    const ProcessConfig& cfg = kernel_.config();
    if (core == kCoreUseProcessValue)
        core = cfg.default_core;
    if (core < 0 || core >= kCoreCount || ((cfg.core_mask >> core) & 1) == 0)
        return kr::kInvalidCoreId;
    if (priority < 0 || priority > kLowestPriority || ((cfg.priority_mask >> priority) & 1) == 0)
        return kr::kInvalidPriority;

    uint64_t guest_stack = 0;
    {
        std::lock_guard mem_lock(kernel_.memory_mutex());
        const BlockInfo b = kernel_.memory().query(stack_top - 1);
        if (b.state != MemState::Free && b.state != MemState::Inaccessible)
            guest_stack = b.size;
    }

    uint64_t tls = 0;
    const Result rc = allocate_tls(&tls);
    if (rc.failed())
        return rc;

    std::shared_ptr<KThread> t;
    {
        Lock lock(lock_);
        t = std::make_shared<KThread>(next_thread_id_++);
    }
    t->entry_ = entry;
    t->tls_addr_ = tls;
    t->host_stack_size_ = std::clamp<uint64_t>(guest_stack * 2, cfg.min_host_stack_size, kMaxHostStack);
    t->cpu_.x[0] = arg;
    t->cpu_.sp = stack_top;
    t->cpu_.pc = entry;
    t->cpu_.tpidrro_el0 = tls;
    t->priority_ = priority;
    t->core_ = core;
    t->affinity_ = uint64_t{1} << core;

    Handle h = kInvalidHandle;
    const Result add = kernel_.handles().add(t, &h);
    if (add.failed()) {
        free_tls(tls);
        return add;
    }
    {
        Lock lock(lock_);
        threads_.push_back(t);
    }
    *out = h;
    return kSuccess;
}

Result Scheduler::start_thread(KThread* self, Handle handle) {
    std::shared_ptr<KThread> t = thread_from_handle(handle, self);
    if (!t)
        return kr::kInvalidHandle;
    Lock lock(lock_);
    if (t->state_ != ThreadState::Created)
        return kr::kInvalidState;
    if (exiting())
        return kr::kTerminationRequested;
    std::unique_ptr<HostThread> host = HostThread::start(t->host_stack_size_, [this, t] { thread_main(t); });
    if (!host)
        return kr::kOutOfResource;
    host_threads_.push_back(std::move(host));
    ++live_host_threads_;
    make_ready(*t, false);
    if (self != nullptr)
        preempt_check(*self, lock);
    return kSuccess;
}

void Scheduler::thread_main(std::shared_ptr<KThread> thread) {
    KThread& t = *thread;
    t_current = &t;
    platform::set_current_thread_name(("lgp-g" + std::to_string(t.id_)).c_str());
    {
        Lock lock(lock_);
        ++active_;
        time_stats_attach(t.id_, t.priority_, t.core_);
        wait_for_running(t, lock);
    }
    CpuState& cpu = t.cpu_;
    if (!exiting()) {
        const LiftedFn fn = lookup_function(t.entry_);  // dispatch table, then the resolver
        if (fn == nullptr) {
            cpu.halt = kHaltBadJump;
            cpu.halt_info = t.entry_;
        } else {
            run_lifted(&cpu, fn);
            // RET out of the outermost lifted function has no host caller to
            // return to: on Horizon it is a jump to x30. Guest code may set
            // x30 to a function on purpose (libnx's crt0: x30 = exit, b main).
            while (cpu.halt == kHaltNone && !exiting()) {
                const LiftedFn next = lookup_function(cpu.x[30]);
                if (next == nullptr)
                    break;
                cpu.pc = cpu.x[30];
                run_lifted(&cpu, next);
            }
        }
        if (cpu.halt == kHaltNone) {
            // RET to an address without a function (nothing set x30 up).
            // Threads end with svcExitThread.
            cpu.halt = kHaltBadJump;
            cpu.halt_info = cpu.x[30];
        }
        if (cpu.halt != kHaltExit) {
            log(LogLevel::Error, "guest thread %" PRIu64 " halted: reason %u, info 0x%" PRIx64 ", pc 0x%" PRIx64
                ", x30 0x%" PRIx64 ", frames%s", t.id_, cpu.halt, cpu.halt_info, cpu.pc, cpu.x[30],
                frame_chain(cpu.x[29]).c_str());
            request_exit(&t, ProcessExit::Reason::Crash, cpu.halt, cpu.halt_info, cpu.pc);
        }
    }
    finish_thread(t);
    time_stats_detach();
    t_current = nullptr;
}

void Scheduler::finish_thread(KThread& t) {
    Lock lock(lock_);
    --active_;
    pause_cv_.notify_all();
    t.state_ = ThreadState::Terminated;
    ready_.erase(std::remove(ready_.begin(), ready_.end(), &t), ready_.end());
    release_slot(t);
    const std::vector<KThread*> waiters = t.waiters_;
    for (KThread* w : waiters) {
        const auto& objs = w->wait_objects_;
        const auto it = std::find(objs.begin(), objs.end(), static_cast<KSyncObject*>(&t));
        wake(*w, kSuccess, static_cast<int32_t>(it - objs.begin()));
    }
    // Horizon frees the TLS region when the thread object is destroyed (last
    // handle closed); we free it at exit. Only differs if another thread reads
    // a dead thread's TLS.
    if (!t.tls_freed_) {
        t.tls_freed_ = true;
        free_tls(t.tls_addr_);
    }
    threads_.erase(std::remove_if(threads_.begin(), threads_.end(), [&](const auto& p) { return p.get() == &t; }),
                   threads_.end());
    // Virtual clock: the last running thread left; move time on for the others.
    while (idle_advance(lock)) {
    }
    if (--live_host_threads_ == 0 && !exit_set_) {
        exit_set_ = true;
        exit_ = {};
        exit_.reason = ProcessExit::Reason::AllThreadsExited;
    }
    process_cv_.notify_all();
}

void Scheduler::request_exit(KThread* by, ProcessExit::Reason reason, uint32_t halt, uint64_t halt_info,
                             uint64_t pc) {
    Lock lock(lock_);
    if (!exit_set_) {
        exit_set_ = true;
        exit_.reason = reason;
        exit_.thread_id = by != nullptr ? by->id_ : 0;
        exit_.halt = halt;
        exit_.halt_info = halt_info;
        exit_.pc = pc;
    }
    exiting_.store(true, std::memory_order_release);
    g_backedge_stop.store(1, std::memory_order_relaxed);
    for (const auto& t : threads_) {
        if (t->state_ == ThreadState::Waiting)
            wake(*t, kr::kTerminationRequested);
        notify(*t);
    }
    pause_cv_.notify_all();
    detail::notify_interrupt_listener(GuestInterrupt::Exit);
}

bool Scheduler::pause(int64_t timeout_ms) {
    Lock lock(lock_);
    paused_.store(true, std::memory_order_release);
    detail::notify_interrupt_listener(GuestInterrupt::Pause);
    // An exit ends the wait (nothing would wake it), but only a guest at rest
    // counts: a thread still running may still submit GPU work.
    pause_cv_.wait_for(lock, std::chrono::milliseconds(std::max<int64_t>(timeout_ms, 0)),
                       [&] { return active_ == 0 || exiting(); });
    return active_ == 0;
}

void Scheduler::resume() {
    Lock lock(lock_);
    paused_.store(false, std::memory_order_release);
    for (const auto& t : threads_)
        notify(*t);
    detail::notify_interrupt_listener(GuestInterrupt::Resume);
}

int Scheduler::active_threads() const {
    Lock lock(lock_);
    return active_;
}

void Scheduler::pause_point(KThread& self) {
    if (!paused())
        return;
    Lock lock(lock_);
    pause_gate(self, lock);
}

ProcessExit Scheduler::run(const MainThreadParams& params) {
    Handle main = kInvalidHandle;
    const Result rc =
        create_thread(params.entry, 0, params.stack_top, params.priority, params.core, &main);
    ProcessExit result;
    if (rc.failed()) {
        result.reason = ProcessExit::Reason::StartFailed;
        result.result = rc;
        return result;
    }
    kernel_.handles().get_as<KThread>(main)->cpu_.x[1] = main;
    const Result started = start_thread(nullptr, main);
    if (started.failed()) {
        result.reason = ProcessExit::Reason::StartFailed;
        result.result = started;
        return result;
    }
    std::vector<std::unique_ptr<HostThread>> hosts;
    {
        Lock lock(lock_);
        process_cv_.wait(lock, [&] { return live_host_threads_ == 0; });
        hosts.swap(host_threads_);
        result = exit_;
    }
    hosts.clear();  // joins
    return result;
}

// --- thread svcs ------------------------------------------------------------------

void Scheduler::sleep_thread(KThread& self, int64_t ns) {
    Lock lock(lock_);
    if (ns <= 0) {
        // 0: yield to threads of the same priority; -1/-2: yield with or
        // without core migration. Without migration all three are the same.
        if (mode_ == SchedMode::Free) {
            lock.unlock();
            std::this_thread::yield();
            return;
        }
        requeue(self, lock, /*front=*/false);
        return;
    }
    self.wait_kind_ = KThread::WaitKind::Sleep;
    block(self, lock, ns);
}

void Scheduler::yield_hint(KThread& self) {
    Lock lock(lock_);
    if (mode_ == SchedMode::Free || self.held_slot_ < 0) {
        lock.unlock();
        std::this_thread::yield();
        return;
    }
    const int slot = self.held_slot_;
    KThread* next = best_ready(slot, &self);
    if (next == nullptr)
        return;
    // Hand the slot to `next` even if it has a lower priority: a spin-wait
    // may be waiting for exactly that thread.
    self.held_slot_ = -1;
    self.state_ = ThreadState::Ready;
    self.ready_seq_ = ++next_seq_;
    ready_.push_back(&self);
    ready_.erase(std::find(ready_.begin(), ready_.end(), next));
    slots_[slot] = next;
    next->held_slot_ = slot;
    next->state_ = ThreadState::Running;
    mark_dispatched(*next);
    notify(*next);
    wait_for_running(self, lock);
}

void Scheduler::preempt_poll(KThread& self) {
    if (mode_ != SchedMode::CoreBaton)
        return;
    Lock lock(lock_);
    preempt_check(self, lock);
}

Result Scheduler::get_thread_priority(Handle handle, int32_t* out) {
    std::shared_ptr<KThread> t = thread_from_handle(handle, current());
    if (!t)
        return kr::kInvalidHandle;
    Lock lock(lock_);
    *out = t->priority_;
    return kSuccess;
}

Result Scheduler::set_thread_priority(KThread& self, Handle handle, int32_t priority) {
    if (priority < 0 || priority > kLowestPriority || ((kernel_.config().priority_mask >> priority) & 1) == 0)
        return kr::kInvalidPriority;
    std::shared_ptr<KThread> t = thread_from_handle(handle, &self);
    if (!t)
        return kr::kInvalidHandle;
    Lock lock(lock_);
    t->priority_ = priority;
    if (t->state_ == ThreadState::Ready)
        dispatch(slot_of(*t));
    preempt_check(self, lock);
    return kSuccess;
}

Result Scheduler::get_thread_core_mask(Handle handle, int32_t* core, uint64_t* mask) {
    std::shared_ptr<KThread> t = thread_from_handle(handle, current());
    if (!t)
        return kr::kInvalidHandle;
    Lock lock(lock_);
    *core = t->core_;
    *mask = t->affinity_;
    return kSuccess;
}

Result Scheduler::set_thread_core_mask(KThread& self, Handle handle, int32_t core, uint64_t mask) {
    const ProcessConfig& cfg = kernel_.config();
    if (core == kCoreUseProcessValue) {
        core = cfg.default_core;
        mask = uint64_t{1} << core;
    } else {
        if ((mask | cfg.core_mask) != cfg.core_mask)
            return kr::kInvalidCoreId;
        if (mask == 0)
            return kr::kInvalidCombination;
        if (core >= 0) {
            if (core >= kCoreCount || ((mask >> core) & 1) == 0)
                return kr::kInvalidCombination;
        } else if (core != kCoreNoUpdate && core != kCoreDontCare) {
            return kr::kInvalidCoreId;
        }
    }
    std::shared_ptr<KThread> t = thread_from_handle(handle, &self);
    if (!t)
        return kr::kInvalidHandle;
    Lock lock(lock_);
    if (core == kCoreNoUpdate) {
        core = t->core_;
        if (((mask >> core) & 1) == 0)
            return kr::kInvalidCombination;
    } else if (core == kCoreDontCare) {
        // No ideal core: we still need one for the core model; take the
        // lowest allowed one (an approximation, Horizon migrates freely).
        core = std::countr_zero(mask);
    }
    t->core_ = core;
    t->affinity_ = mask;
    if (t->state_ == ThreadState::Ready)
        dispatch(slot_of(*t));
    preempt_check(self, lock);
    return kSuccess;
}

int32_t Scheduler::current_processor(KThread& self) {
    Lock lock(lock_);
    return self.core_;
}

Result Scheduler::get_thread_id(Handle handle, uint64_t* out) {
    std::shared_ptr<KThread> t = thread_from_handle(handle, current());
    if (!t)
        return kr::kInvalidHandle;
    *out = t->id_;
    return kSuccess;
}

// Back-edge poll from lifted code (guest.hpp, LGP_BACKEDGE).
bool backedge_poll(CpuState* ctx) {
    KThread* self = Scheduler::current();
    if (self == nullptr)
        return false;
    Scheduler& s = active_kernel().scheduler();
    s.pause_point(*self);
    if (s.exiting()) {
        if (ctx->halt == kHaltNone) {
            ctx->halt = kHaltExit;
            ctx->halt_info = 0;
        }
        return true;
    }
    // Only the single baton emulates several cores on one slot; there a
    // spin-wait without svc must let the other threads run.
    if (s.mode() == SchedMode::Baton)
        s.yield_hint(*self);
    else
        s.preempt_poll(*self);
    return false;
}

}  // namespace lgp::rt
