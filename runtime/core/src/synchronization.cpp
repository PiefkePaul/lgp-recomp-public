// Kernel synchronization svcs (P3.2): events, svcWaitSynchronization and the
// address arbiter behind guest mutexes and condition variables.
//
// Guest mutex protocol (Horizon ABI, used by libnx and nn::os): a mutex is a
// 32-bit word in guest memory holding 0 (free) or the owner's thread handle,
// with bit 30 set when other threads wait. Lock/unlock without contention
// stay in user space (exclusive load/store); svcArbitrateLock/Unlock only run
// on contention. Condition variables are a 32-bit key word that the kernel
// sets to 1 while threads wait on it.
//
// Difference to Horizon: waiters are kept per mutex address, not on the owner
// thread, so there is no priority inheritance. Nothing measured in P1.3
// depends on it (almost all LGP threads run at priority 44).
#include <algorithm>
#include <atomic>

#include "lgp/runtime/kernel.hpp"
#include "lgp/runtime/thread.hpp"

namespace lgp::rt {
namespace {

namespace kr = kernel_result;

constexpr int32_t kMaxWaitObjects = 0x40;

// Best waiter in `list` matching pred: highest priority, then longest waiting.
template <typename Pred>
KThread* best_waiter(const std::vector<KThread*>& list, Pred&& pred, int32_t (*priority)(const KThread*)) {
    KThread* best = nullptr;
    for (KThread* t : list) {
        if (pred(t) && (best == nullptr || priority(t) < priority(best)))
            best = t;
    }
    return best;
}

}  // namespace

// --- events -----------------------------------------------------------------------

Result Scheduler::create_event(Handle* writable, Handle* readable) {
    auto r = std::make_shared<KReadableEvent>();
    auto w = std::make_shared<KWritableEvent>(r);
    Handle wh = kInvalidHandle;
    Handle rh = kInvalidHandle;
    Result rc = kernel_.handles().add(w, &wh);
    if (rc.failed())
        return rc;
    rc = kernel_.handles().add(r, &rh);
    if (rc.failed()) {
        kernel_.handles().close(wh);
        return rc;
    }
    *writable = wh;
    *readable = rh;
    return kSuccess;
}

Result Scheduler::signal_event(KThread& self, Handle handle) {
    auto w = kernel_.handles().get_as<KWritableEvent>(handle);
    if (!w)
        return kr::kInvalidHandle;
    Lock lock(lock_);
    signal_locked(*w->readable());
    preempt_check(self, lock);
    return kSuccess;
}

void Scheduler::signal_locked(KReadableEvent& r) {
    if (r.signaled_)
        return;
    r.signaled_ = true;
    const std::vector<KThread*> waiters = r.waiters_;
    for (KThread* t : waiters) {
        const auto& objs = t->wait_objects_;
        const auto it = std::find(objs.begin(), objs.end(), static_cast<KSyncObject*>(&r));
        wake(*t, kSuccess, static_cast<int32_t>(it - objs.begin()));
    }
}

void Scheduler::signal_readable(KReadableEvent& event) {
    Lock lock(lock_);
    signal_locked(event);
}

void Scheduler::clear_readable(KReadableEvent& event) {
    Lock lock(lock_);
    event.signaled_ = false;
}

bool Scheduler::is_readable_signaled(const KReadableEvent& event) const {
    Lock lock(lock_);
    return event.signaled_;
}

Result Scheduler::clear_event(Handle handle) {
    std::shared_ptr<KReadableEvent> r;
    if (auto w = kernel_.handles().get_as<KWritableEvent>(handle))
        r = w->readable();
    else
        r = kernel_.handles().get_as<KReadableEvent>(handle);
    if (!r)
        return kr::kInvalidHandle;
    Lock lock(lock_);
    r->signaled_ = false;
    return kSuccess;
}

Result Scheduler::reset_signal(Handle handle) {
    auto r = kernel_.handles().get_as<KReadableEvent>(handle);
    if (!r)
        return kr::kInvalidHandle;  // processes can be reset too; not supported
    Lock lock(lock_);
    if (!r->signaled_)
        return kr::kInvalidState;
    r->signaled_ = false;
    return kSuccess;
}

// --- svcWaitSynchronization --------------------------------------------------------

Result Scheduler::wait_synchronization(KThread& self, uint64_t handles_addr, int32_t count, int64_t timeout_ns,
                                       int32_t* index) {
    *index = -1;
    if (count < 0 || count > kMaxWaitObjects)
        return kr::kOutOfRange;
    Handle handles[kMaxWaitObjects] = {};
    if (count > 0) {
        std::lock_guard mem_lock(kernel_.memory_mutex());
        if (!kernel_.memory().copy_from_user(handles, handles_addr, sizeof(Handle) * static_cast<uint64_t>(count)))
            return kr::kInvalidPointer;
    }
    // Held here, so the objects outlive the wait even if their handles are closed.
    std::vector<std::shared_ptr<KSyncObject>> objects;
    objects.reserve(static_cast<size_t>(count));
    for (int32_t i = 0; i < count; ++i) {
        std::shared_ptr<KSyncObject> o;
        if (handles[i] == kPseudoHandleCurrentThread)
            o = self.shared_from_this();
        else
            o = kernel_.handles().get_as<KSyncObject>(handles[i]);
        if (!o)
            return kr::kInvalidHandle;
        objects.push_back(std::move(o));
    }

    Lock lock(lock_);
    if (exiting())
        return kr::kTerminationRequested;
    for (int32_t i = 0; i < count; ++i) {
        if (objects[static_cast<size_t>(i)]->is_signaled()) {
            *index = i;
            return kSuccess;
        }
    }
    if (timeout_ns == 0)
        return kr::kTimedOut;
    if (self.sync_cancelled_) {
        self.sync_cancelled_ = false;
        return kr::kCancelled;
    }
    self.wait_kind_ = KThread::WaitKind::Sync;
    self.wait_objects_.clear();
    for (const auto& o : objects) {
        self.wait_objects_.push_back(o.get());
        o->waiters_.push_back(&self);
    }
    const Result rc = block(self, lock, timeout_ns);
    *index = self.wait_index_;
    return rc;
}

Result Scheduler::cancel_synchronization(Handle handle) {
    std::shared_ptr<KThread> t = thread_from_handle(handle, current());
    if (!t)
        return kr::kInvalidHandle;
    Lock lock(lock_);
    if (t->state_ == ThreadState::Waiting && t->wait_kind_ == KThread::WaitKind::Sync)
        wake(*t, kr::kCancelled);
    else
        t->sync_cancelled_ = true;
    return kSuccess;
}

// --- address arbiter: mutexes ------------------------------------------------------

uint32_t* Scheduler::user_word(uint64_t addr) {
    if ((addr & 3) != 0)
        return nullptr;
    std::lock_guard mem_lock(kernel_.memory_mutex());
    const BlockInfo b = kernel_.memory().query(addr);
    if (b.state == MemState::Free || b.state == MemState::Inaccessible || (b.perm & kPermRW) != kPermRW)
        return nullptr;
    return reinterpret_cast<uint32_t*>(kernel_.memory().host_ptr(addr, 4));
}

Result Scheduler::unlock_mutex(uint64_t addr) {
    uint32_t* word = user_word(addr);
    if (word == nullptr)
        return kr::kInvalidCurrentMemory;
    KThread* next = best_waiter(
        mutex_waiters_, [&](const KThread* t) { return t->mutex_addr_ == addr; },
        [](const KThread* t) { return t->priority_; });
    uint32_t value = 0;
    if (next != nullptr) {
        const bool more = std::count_if(mutex_waiters_.begin(), mutex_waiters_.end(), [&](const KThread* t) {
                              return t != next && t->mutex_addr_ == addr;
                          }) != 0;
        value = next->mutex_tag_ | (more ? kMutexHasWaiters : 0);
    }
    std::atomic_ref<uint32_t>(*word).store(value, std::memory_order_seq_cst);
    if (next != nullptr)
        wake(*next, kSuccess);
    return kSuccess;
}

Result Scheduler::arbitrate_lock(KThread& self, Handle owner, uint64_t addr, uint32_t tag) {
    if ((addr & 3) != 0)
        return kr::kInvalidAddress;
    uint32_t* word = user_word(addr);
    if (word == nullptr)
        return kr::kInvalidCurrentMemory;
    Lock lock(lock_);
    if (exiting())
        return kr::kTerminationRequested;
    // The owner may have released the mutex since the caller looked: let it retry.
    if (std::atomic_ref<uint32_t>(*word).load(std::memory_order_seq_cst) != (owner | kMutexHasWaiters))
        return kSuccess;
    if (!thread_from_handle(owner, &self))
        return kr::kInvalidHandle;
    self.wait_kind_ = KThread::WaitKind::Mutex;
    self.mutex_addr_ = addr;
    self.mutex_tag_ = tag;
    mutex_waiters_.push_back(&self);
    return block(self, lock, -1);
}

Result Scheduler::arbitrate_unlock(KThread& self, uint64_t addr) {
    if ((addr & 3) != 0)
        return kr::kInvalidAddress;
    Lock lock(lock_);
    const Result rc = unlock_mutex(addr);
    preempt_check(self, lock);
    return rc;
}

// --- address arbiter: condition variables ------------------------------------------

Result Scheduler::wait_process_wide_key(KThread& self, uint64_t mutex_addr, uint64_t key, uint32_t tag,
                                        int64_t timeout_ns) {
    if ((mutex_addr & 3) != 0)
        return kr::kInvalidAddress;
    Lock lock(lock_);
    if (exiting())
        return kr::kTerminationRequested;
    // Release the mutex; on timeout the caller re-locks it in user space.
    const Result rc = unlock_mutex(mutex_addr);
    if (rc.failed())
        return rc;
    if (uint32_t* k = user_word(key))
        std::atomic_ref<uint32_t>(*k).store(1, std::memory_order_seq_cst);
    if (timeout_ns == 0)
        return kr::kTimedOut;
    self.wait_kind_ = KThread::WaitKind::CondVar;
    self.cv_key_ = key;
    self.mutex_addr_ = mutex_addr;
    self.mutex_tag_ = tag;
    cv_waiters_.push_back(&self);
    return block(self, lock, timeout_ns);
}

void Scheduler::acquire_after_signal(KThread& t) {
    uint32_t* word = user_word(t.mutex_addr_);
    if (word == nullptr) {
        wake(t, kr::kInvalidCurrentMemory);
        return;
    }
    std::atomic_ref<uint32_t> w(*word);
    uint32_t v = w.load(std::memory_order_seq_cst);
    for (;;) {
        if (v == 0) {
            if (w.compare_exchange_weak(v, t.mutex_tag_, std::memory_order_seq_cst))
                break;
            continue;
        }
        if ((v & kMutexHasWaiters) != 0 || w.compare_exchange_weak(v, v | kMutexHasWaiters, std::memory_order_seq_cst)) {
            // Held: queue behind the owner; its svcArbitrateUnlock hands it over.
            t.wait_kind_ = KThread::WaitKind::Mutex;
            mutex_waiters_.push_back(&t);
            return;
        }
    }
    wake(t, kSuccess);
}

void Scheduler::signal_process_wide_key(KThread& self, uint64_t key, int32_t count) {
    Lock lock(lock_);
    const auto on_key = [&](const KThread* t) { return t->cv_key_ == key; };
    for (int32_t n = 0; count <= 0 || n < count; ++n) {
        KThread* t = best_waiter(cv_waiters_, on_key, [](const KThread* x) { return x->priority_; });
        if (t == nullptr)
            break;
        cv_waiters_.erase(std::find(cv_waiters_.begin(), cv_waiters_.end(), t));
        t->wait_kind_ = KThread::WaitKind::None;
        acquire_after_signal(*t);
    }
    if (std::none_of(cv_waiters_.begin(), cv_waiters_.end(), on_key)) {
        if (uint32_t* k = user_word(key))
            std::atomic_ref<uint32_t>(*k).store(0, std::memory_order_seq_cst);
    }
    preempt_check(self, lock);
}

// --- address arbiter: WaitForAddress / SignalToAddress -----------------------------
//
// Semantics after Horizon's KAddressArbiter as Switchbrew describes it
// (behaviour only, no code taken over): the value test and the
// optional update of the word happen under the scheduler lock, so a signal
// cannot slip between a waiter's test and its sleep. Wake order: highest
// priority, then longest waiting.

namespace {
constexpr uint32_t kArbWaitIfLessThan = 0, kArbDecrementAndWaitIfLessThan = 1, kArbWaitIfEqual = 2;
constexpr uint32_t kSignal = 0, kSignalAndIncrementIfEqual = 1, kSignalAndModifyByWaitingCountIfEqual = 2;
}  // namespace

Result Scheduler::wait_for_address(KThread& self, uint64_t addr, uint32_t type, int32_t value, int64_t timeout_ns) {
    if ((addr & 3) != 0)
        return kr::kInvalidAddress;
    if (type > kArbWaitIfEqual)
        return kr::kInvalidEnumValue;
    uint32_t* word = user_word(addr);
    if (word == nullptr)
        return kr::kInvalidCurrentMemory;
    Lock lock(lock_);
    if (exiting())
        return kr::kTerminationRequested;
    std::atomic_ref<uint32_t> w(*word);
    uint32_t v = w.load(std::memory_order_seq_cst);
    switch (type) {
    case kArbWaitIfLessThan:
        if (!(static_cast<int32_t>(v) < value))
            return kr::kInvalidState;
        break;
    case kArbDecrementAndWaitIfLessThan:
        // Decrements only when less than; the test is on the old value.
        while (static_cast<int32_t>(v) < value &&
               !w.compare_exchange_weak(v, v - 1, std::memory_order_seq_cst)) {
        }
        if (!(static_cast<int32_t>(v) < value))
            return kr::kInvalidState;
        break;
    default:  // kArbWaitIfEqual
        if (static_cast<int32_t>(v) != value)
            return kr::kInvalidState;
        break;
    }
    if (timeout_ns == 0)
        return kr::kTimedOut;
    self.wait_kind_ = KThread::WaitKind::Address;
    self.cv_key_ = addr;
    address_waiters_.push_back(&self);
    return block(self, lock, timeout_ns);
}

Result Scheduler::signal_to_address(KThread& self, uint64_t addr, uint32_t type, int32_t value, int32_t count) {
    if ((addr & 3) != 0)
        return kr::kInvalidAddress;
    if (type > kSignalAndModifyByWaitingCountIfEqual)
        return kr::kInvalidEnumValue;
    Lock lock(lock_);
    const auto on_addr = [&](const KThread* t) { return t->cv_key_ == addr; };
    if (type != kSignal) {
        uint32_t* word = user_word(addr);
        if (word == nullptr)
            return kr::kInvalidCurrentMemory;
        int32_t next = value + 1;  // kSignalAndIncrementIfEqual
        if (type != kSignalAndIncrementIfEqual) {
            const auto waiting = std::count_if(address_waiters_.begin(), address_waiters_.end(), on_addr);
            if (waiting == 0)
                next = value + 1;
            else if (count <= 0)
                next = value - 2;  // all of them wake
            else if (waiting <= count)
                next = value - 1;  // the last ones wake
            else
                next = value;  // some keep waiting
        }
        uint32_t expected = static_cast<uint32_t>(value);
        if (!std::atomic_ref<uint32_t>(*word).compare_exchange_strong(expected, static_cast<uint32_t>(next),
                                                                       std::memory_order_seq_cst))
            return kr::kInvalidState;
    }
    for (int32_t n = 0; count <= 0 || n < count; ++n) {
        KThread* t = best_waiter(address_waiters_, on_addr, [](const KThread* x) { return x->priority_; });
        if (t == nullptr)
            break;
        wake(*t, kSuccess);  // unlinks it from address_waiters_
    }
    preempt_check(self, lock);
    return kSuccess;
}

}  // namespace lgp::rt
