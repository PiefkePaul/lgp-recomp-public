// Guest threads, the scheduler and the kernel synchronization objects (P3.2).
//
// ADR-0002: every guest thread is a host thread (1:1). The scheduler decides
// which of them may run guest code at a time, in one of three modes:
//
//   Free       all guest threads run in parallel;
//   CoreBaton  at most one running guest thread per guest core (Horizon runs
//              threads of one core one at a time; the default for the game);
//   Baton      exactly one running guest thread in the whole process. With
//              no timeouts involved, runs are deterministic: the next thread
//              is always picked by (priority, queue order), and only the
//              running thread changes the queues. With the virtual clock
//              (clock.hpp) timeouts are deterministic too.
//
// A thread hands its slot over when it blocks in the kernel (waits, sleeps,
// yields) or when it makes a higher-priority thread of its slot ready.
// Same-priority threads are not time-sliced against each other, like on
// Horizon; a spin-wait without an svc only hands over at YIELD hints and loop
// back-edges (LGP_YIELD/LGP_BACKEDGE in guest.hpp).
//
// All scheduler state is guarded by one mutex. A waiting host thread parks on
// its own condition variable; the thread that wakes it does the bookkeeping
// (queues, slot hand-over) under that mutex.
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "lgp/lifted/cpu_state.hpp"
#include "lgp/runtime/handle_table.hpp"
#include "lgp/runtime/result.hpp"

namespace lgp::platform {
class HostThread;  // platform/os
}  // namespace lgp::platform

namespace lgp::rt {

class Kernel;
class Scheduler;
class KThread;
using platform::HostThread;

enum class SchedMode : uint8_t { Free, CoreBaton, Baton };
const char* sched_mode_name(SchedMode mode);

// Number of guest cores (CPU ids 0..3; applications get 0..2).
inline constexpr int kCoreCount = 4;
inline constexpr int32_t kLowestPriority = 63;
// Bit 30 of a guest mutex word: other threads wait for it (svcArbitrateLock).
inline constexpr uint32_t kMutexHasWaiters = 0x40000000;
// svcCreateThread/svcSetThreadCoreMask core ids.
inline constexpr int32_t kCoreDontCare = -1;
inline constexpr int32_t kCoreUseProcessValue = -2;
inline constexpr int32_t kCoreNoUpdate = -3;

// Anything svcWaitSynchronization can wait on.
class KSyncObject : public KObject {
public:
    // Called with the scheduler lock held.
    virtual bool is_signaled() const = 0;

private:
    friend class Scheduler;
    std::vector<KThread*> waiters_;  // scheduler lock
};

// Readable half of an event (svcCreateEvent); stays signaled until cleared.
class KReadableEvent final : public KSyncObject {
public:
    const char* type_name() const override { return "ReadableEvent"; }
    bool is_signaled() const override { return signaled_; }

private:
    friend class Scheduler;
    bool signaled_ = false;  // scheduler lock
};

// Writable half: what svcSignalEvent takes.
class KWritableEvent final : public KObject {
public:
    explicit KWritableEvent(std::shared_ptr<KReadableEvent> readable) : readable_(std::move(readable)) {}
    const char* type_name() const override { return "WritableEvent"; }
    const std::shared_ptr<KReadableEvent>& readable() const { return readable_; }

private:
    std::shared_ptr<KReadableEvent> readable_;
};

enum class ThreadState : uint8_t {
    Created,     // svcCreateThread done, not started
    Ready,       // wants a slot
    Running,     // holds its slot (or runs freely)
    Waiting,     // blocked in the kernel
    Terminated,  // exited; signaled for svcWaitSynchronization
};

// A guest thread. Signaled once it has terminated.
class KThread final : public KSyncObject, public std::enable_shared_from_this<KThread> {
public:
    explicit KThread(uint64_t id) : id_(id) {}
    const char* type_name() const override { return "Thread"; }
    bool is_signaled() const override { return state_ == ThreadState::Terminated; }

    uint64_t id() const { return id_; }
    uint64_t tls_addr() const { return tls_addr_; }
    // Guest registers. Owned by the thread's host thread while it runs.
    CpuState& cpu() { return cpu_; }

private:
    friend class Scheduler;

    enum class WaitKind : uint8_t { None, Sync, Mutex, CondVar, Sleep, Address };

    const uint64_t id_;
    CpuState cpu_{};
    uint64_t entry_ = 0;
    uint64_t tls_addr_ = 0;
    uint64_t host_stack_size_ = 0;

    // Everything below: scheduler lock.
    ThreadState state_ = ThreadState::Created;
    int32_t priority_ = 0;
    int32_t core_ = 0;
    uint64_t affinity_ = 0;
    int held_slot_ = -1;
    bool tls_freed_ = false;
    uint64_t ready_seq_ = 0;
    std::condition_variable cv_;
    // Bumped with every notify of cv_: a thread spinning before it parks
    // (Scheduler::set_park_spin_ns) watches it without the lock.
    std::atomic<uint32_t> wake_gen_{0};
    // When another thread handed this one its slot (steady clock ns, 0 =
    // none pending): the time until it runs is the wakeup latency.
    uint64_t dispatched_ns_ = 0;

    WaitKind wait_kind_ = WaitKind::None;
    std::vector<KSyncObject*> wait_objects_;  // kept alive by the waiting svc
    int32_t wait_index_ = -1;
    Result wait_result_{};
    uint64_t mutex_addr_ = 0;  // Mutex wait / mutex to re-take after a CondVar wait
    uint64_t cv_key_ = 0;
    uint32_t mutex_tag_ = 0;  // value the waiter writes into the mutex word (its handle)
    bool sync_cancelled_ = false;
    // Virtual clock: guest time at which the wait times out, and the order in
    // which waits began (ties between equal deadlines).
    uint64_t deadline_ns_ = ~uint64_t{0};
    uint64_t wait_seq_ = 0;
};

// Parameters of the process's main thread (from the NPDM and the loader).
struct MainThreadParams {
    uint64_t entry = 0;
    uint64_t stack_top = 0;
    int32_t priority = 44;
    int32_t core = 0;
};

// How a process run ended.
struct ProcessExit {
    enum class Reason : uint8_t {
        ExitProcess,  // svcExitProcess
        Break,        // svcBreak
        Crash,        // a thread halted (fault, undefined instruction, bad jump, BRK, ...)
        AllThreadsExited,
        StartFailed,  // the main thread could not be created
        Stopped,      // ended from the host (Scheduler::stop, e.g. a watchdog)
    };
    Reason reason = Reason::AllThreadsExited;
    uint64_t thread_id = 0;  // thread that ended the process (not for AllThreadsExited)
    uint32_t halt = 0;       // HaltReason of that thread (Crash)
    uint64_t halt_info = 0;
    uint64_t pc = 0;
    Result result{};  // StartFailed
};
const char* exit_reason_name(ProcessExit::Reason reason);

class Scheduler {
public:
    Scheduler(Kernel& kernel, SchedMode mode);
    ~Scheduler();
    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;

    SchedMode mode() const { return mode_; }
    // A guest thread about to wait first spins up to `ns` (lock released)
    // and takes a wake that comes in that time without sleeping. 0 (the
    // default) parks at once. Set before run().
    void set_park_spin_ns(uint64_t ns) { park_spin_ns_ = ns; }
    // HostWorkScope (host_work.hpp): the running thread gives its slot up
    // (true if it held one, core baton mode only) and later takes it back.
    bool leave_slot(KThread& self);
    void return_to_slot(KThread& self);
    // The guest thread on the calling host thread, or nullptr (host code,
    // tests that call Kernel::svc directly).
    static KThread* current();

    // Runs the process: creates and starts the main thread (x0 = 0,
    // x1 = its handle, Horizon's entry ABI), then blocks until every guest
    // thread has ended. Entry points are resolved with lookup_function().
    ProcessExit run(const MainThreadParams& params);

    // svc implementations. `self` is the calling thread.
    Result create_thread(uint64_t entry, uint64_t arg, uint64_t stack_top, int32_t priority, int32_t core,
                         Handle* out);
    Result start_thread(KThread* self, Handle handle);
    void sleep_thread(KThread& self, int64_t ns);
    Result get_thread_priority(Handle handle, int32_t* out);
    Result set_thread_priority(KThread& self, Handle handle, int32_t priority);
    Result get_thread_core_mask(Handle handle, int32_t* core, uint64_t* mask);
    Result set_thread_core_mask(KThread& self, Handle handle, int32_t core, uint64_t mask);
    int32_t current_processor(KThread& self);
    Result get_thread_id(Handle handle, uint64_t* out);

    Result create_event(Handle* writable, Handle* readable);
    Result signal_event(KThread& self, Handle handle);
    Result clear_event(Handle handle);
    Result reset_signal(Handle handle);
    // Host-side event control for HLE services and host threads: wakes the
    // waiters but never preempts the caller (an HLE handler may hold service
    // locks that a preempting thread would need).
    void signal_readable(KReadableEvent& event);
    void clear_readable(KReadableEvent& event);
    bool is_readable_signaled(const KReadableEvent& event) const;
    Result wait_synchronization(KThread& self, uint64_t handles_addr, int32_t count, int64_t timeout_ns,
                                int32_t* index);
    Result cancel_synchronization(Handle handle);
    Result arbitrate_lock(KThread& self, Handle owner, uint64_t addr, uint32_t tag);
    Result arbitrate_unlock(KThread& self, uint64_t addr);
    Result wait_process_wide_key(KThread& self, uint64_t mutex_addr, uint64_t key, uint32_t tag, int64_t timeout_ns);
    void signal_process_wide_key(KThread& self, uint64_t key, int32_t count);
    // svcWaitForAddress / svcSignalToAddress (address arbiter on a 32-bit word;
    // nn::os semaphores and newer SDK sync use them). Types as in Horizon:
    // wait 0 if less than, 1 decrement and wait if less than, 2 if equal;
    // signal 0 plain, 1 increment if equal, 2 modify by waiting count if equal.
    Result wait_for_address(KThread& self, uint64_t addr, uint32_t type, int32_t value, int64_t timeout_ns);
    Result signal_to_address(KThread& self, uint64_t addr, uint32_t type, int32_t value, int32_t count);

    // YIELD/WFE/WFI hint and the periodic back-edge poll: in the baton modes,
    // hand the slot to any other ready thread of the slot (spin-waits must let
    // the thread they wait for run, whatever its priority).
    void yield_hint(KThread& self);
    // Periodic preemption point (back-edge poll) in CoreBaton mode: hands the
    // slot over if a higher-priority thread of the slot became ready while
    // this one ran. Host threads (HLE services, timers) make threads ready
    // without preempting the running guest thread; on Horizon the woken
    // thread would preempt at once. Without this, a thread that never blocks
    // starves a higher-priority one woken from the host (LGP: the audio
    // thread waiting for its audout reply while the main thread spins).
    void preempt_poll(KThread& self);
    // Virtual clock (clock.hpp): a guest thread moved guest time forward.
    // Runs the periodic host callbacks that are due, then times out the waits
    // whose deadline has passed. Cheap when nothing is due.
    void on_clock_advanced();
    // Ends the process: every thread is asked to terminate; blocked threads
    // wake up and leave guest code at their next svc or back-edge poll.
    void request_exit(KThread* by, ProcessExit::Reason reason, uint32_t halt = 0, uint64_t halt_info = 0,
                      uint64_t pc = 0);
    bool exiting() const { return exiting_.load(std::memory_order_acquire); }
    // Ends the process from a host thread (ProcessExit::Reason::Stopped).
    void stop() { request_exit(nullptr, ProcessExit::Reason::Stopped); }
    // Suspends the process from a host thread (the app goes to the background,
    // P7): every guest thread stops at its next svc (entry and return), its next
    // wake-up and its next function-entry poll (LGP_BACKEDGE_ENTRY), natively
    // run code (runtime/native) also where a kick signal finds it, and stays
    // there until resume(). Waits up to timeout_ms until no guest thread runs
    // any more, i.e. every one is blocked in the scheduler; true if that was
    // reached. A thread busy in guest code without svcs or calls, or blocked
    // outside the scheduler (a host lock), keeps pause() waiting until the
    // timeout. Guest time keeps running (as on Horizon while an application is
    // suspended). Ending the process (request_exit) releases paused threads
    // and ends the wait; pause() then returns false while a thread still runs.
    bool pause(int64_t timeout_ms);
    void resume();
    bool paused() const { return paused_.load(std::memory_order_acquire); }
    // Guest threads not blocked in the scheduler (diagnostics, tests).
    int active_threads() const;
    // Called on the guest thread at svc entry and return (Kernel::svc) and by
    // the periodic poll: blocks while the process is paused.
    void pause_point(KThread& self);
    // One line per guest thread for diagnosing hangs: id, state, priority,
    // what it waits on, pc/x30 and the return addresses of its frame-pointer
    // chain (x29). Registers of running threads are read without stopping
    // them, so those lines are only approximate.
    std::vector<std::string> thread_report();
    // One thread-local-sized (0x200) region that belongs to the process, not a thread
    // (Horizon's process local region, used for the user exception context).
    Result allocate_process_local_region(uint64_t* out) { return allocate_tls(out); }

private:
    using Lock = std::unique_lock<std::mutex>;

    std::string frame_chain(uint64_t fp);

    std::shared_ptr<KThread> thread_from_handle(Handle handle, KThread* self) const;
    // Thread-local storage regions (0x200 bytes each, in ThreadLocal pages).
    Result allocate_tls(uint64_t* out);
    void free_tls(uint64_t addr);
    void thread_main(std::shared_ptr<KThread> thread);
    void finish_thread(KThread& thread);

    int slot_of(const KThread& t) const;
    void make_ready(KThread& t, bool front);
    void dispatch(int slot);
    void release_slot(KThread& t);
    void wait_for_running(KThread& t, Lock& lock);
    // Every wait of a guest thread on its cv_ goes through these: the thread
    // counts as inactive while it waits (pause()).
    void park(KThread& t, Lock& lock);
    bool park_until(KThread& t, Lock& lock, std::chrono::steady_clock::time_point deadline);
    // Wakes `t` from park()/park_until() (lock held).
    static void notify(KThread& t);
    // Another thread lets `t` run: notes when, for the wakeup latency.
    static void mark_dispatched(KThread& t);
    // park_spin_ns_ > 0: spins until `t` is notified (true, lock held again)
    // or `deadline` passes (false).
    bool spin_for_wake(KThread& t, Lock& lock, std::chrono::steady_clock::time_point deadline);
    // Blocks while paused (lock held).
    void pause_gate(KThread& t, Lock& lock);
    // Blocks the running thread until it is woken (or the deadline passes,
    // then it wakes itself with kTimedOut). Returns its wait result.
    Result block(KThread& t, Lock& lock, int64_t timeout_ns);
    void wake(KThread& t, Result result, int32_t index = -1);
    void unlink_wait(KThread& t);
    void signal_locked(KReadableEvent& r);
    void preempt_check(KThread& self, Lock& lock);
    // Puts the running thread back into the ready queue and waits for a slot.
    void requeue(KThread& self, Lock& lock, bool front);
    KThread* best_ready(int slot, const KThread* exclude) const;

    // Virtual clock. idle_locked: no guest thread runs or is ready, at least
    // one waits. idle_advance moves guest time to the next deadline then
    // (timed wait or periodic callback) and fires it; false if there is none.
    bool idle_locked() const;
    bool idle_advance(Lock& lock);
    // Times out every wait with deadline <= now, earliest deadline first.
    void expire_locked(uint64_t now);
    // Runs the due periodic callbacks (without the lock), then the timeouts,
    // with dispatching held back: the threads they make ready get their slots
    // together, by priority, after all of them fired.
    void fire_due(Lock& lock, uint64_t now);

    // Guest mutex words (svcArbitrateLock & co.). nullptr if not a user-RW,
    // 4-byte aligned word.
    uint32_t* user_word(uint64_t addr);
    // Hands the mutex at addr to its best waiter (or clears it).
    Result unlock_mutex(uint64_t addr);
    // A thread signaled on a condition variable takes its mutex, or queues
    // behind its owner.
    void acquire_after_signal(KThread& t);

    Kernel& kernel_;
    const SchedMode mode_;

    mutable std::mutex lock_;
    std::vector<std::shared_ptr<KThread>> threads_;  // not yet terminated
    std::vector<KThread*> ready_;
    KThread* slots_[kCoreCount] = {};
    uint64_t next_seq_ = 1ull << 62;
    uint64_t front_seq_ = 1ull << 62;
    std::vector<KThread*> mutex_waiters_;
    std::vector<KThread*> cv_waiters_;
    std::vector<KThread*> address_waiters_;  // key in cv_key_
    std::atomic<bool> exiting_{false};
    // pause()/resume(): active_ counts started guest threads that are not
    // waiting on their cv_ (scheduler lock).
    std::atomic<bool> paused_{false};
    int active_ = 0;
    std::condition_variable pause_cv_;
    ProcessExit exit_{};
    bool exit_set_ = false;
    int live_host_threads_ = 0;
    std::condition_variable process_cv_;
    std::vector<std::unique_ptr<HostThread>> host_threads_;
    uint64_t next_thread_id_ = 1;
    // Virtual clock state (scheduler lock unless atomic).
    const bool virtual_clock_;
    std::atomic<uint64_t> earliest_deadline_{~uint64_t{0}};  // may be early, never late
    uint64_t next_wait_seq_ = 0;
    int hold_dispatch_ = 0;
    uint64_t park_spin_ns_ = 0;
    bool timers_running_ = false;

    std::mutex tls_mutex_;
    std::vector<uint64_t> tls_pages_;
    std::vector<uint64_t> free_tls_;
};

}  // namespace lgp::rt
