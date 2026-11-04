#include "lgp/runtime/kernel.hpp"

#include <algorithm>
#include <cinttypes>
#include <cstring>

#include "lgp/runtime/log.hpp"
#include "lgp/runtime/system.hpp"
#include "lgp/runtime/time_stats.hpp"

namespace lgp::rt {
namespace {

namespace kr = kernel_result;

bool aligned(uint64_t v) { return (v & (kPageSize - 1)) == 0; }

// States whose permission the owner may change (svcSetMemoryPermission) or
// whose Uncached attribute it may toggle (svcSetMemoryAttribute), and that it
// may lend out (svcCreateTransferMemory). Horizon keys these on per-state
// flags; this is our reading of which states carry them, limited to states
// our kernel creates.
constexpr uint32_t kReprotectStates =
    state_bit(MemState::Normal) | state_bit(MemState::CodeData) | state_bit(MemState::AliasCodeData);
constexpr uint32_t kAttrChangeStates = kReprotectStates;
constexpr uint32_t kTransferStates = state_bit(MemState::Normal) | state_bit(MemState::CodeData);

// Shared memory and similar mappings may go anywhere in the address space
// except the heap and alias regions.
bool can_contain_mapping(const Layout& l, uint64_t addr, uint64_t size) {
    return Region{l.start(), l.size()}.contains(addr, size) && !l.heap.overlaps(addr, size) &&
           !l.alias.overlaps(addr, size);
}

}  // namespace

// --- objects ---------------------------------------------------------------

KTransferMemory::KTransferMemory(Kernel& kernel, uint64_t addr, uint64_t size, uint8_t perm)
    : kernel_(kernel), addr_(addr), size_(size), perm_(perm) {}

KTransferMemory::~KTransferMemory() {
    std::lock_guard lock(kernel_.memory_mutex_);
    kernel_.memory().set_perm(addr_, size_, kPermRW);
    kernel_.memory().set_attr(addr_, size_, kAttrLocked, kAttrNone);
}

KSharedMemory::KSharedMemory(Kernel& kernel, uint64_t size, uint8_t user_perm)
    : kernel_(kernel), size_(size), user_perm_(user_perm), buffer_(size) {}

KSharedMemory::~KSharedMemory() {
    if (mapped_addr_ != 0) {
        std::lock_guard lock(kernel_.memory_mutex_);
        kernel_.memory().unmap(mapped_addr_, size_);
    }
}

uint8_t* KSharedMemory::data() {
    if (mapped_addr_ != 0)
        return kernel_.memory().host_ptr(mapped_addr_, size_);
    return buffer_.data();
}

// --- kernel ----------------------------------------------------------------

Kernel::Kernel(const ProcessConfig& config)
    : config_(config),
      memory_(std::make_unique<GuestMemory>(
          config.custom_layout ? config.layout : Layout::standard(config.total_memory_size), config.memory_options)),
      handles_(config.handle_table_size),
      clock_(config.clock_mode),
      scheduler_(std::make_unique<Scheduler>(*this, config.sched_mode)) {
    if (!config_.custom_layout)
        config_.layout = memory_->layout();
}

ProcessExit Kernel::run(const MainThreadParams& params) {
    set_active_kernel(this);
    return scheduler_->run(params);
}

Kernel::~Kernel() = default;

Result Kernel::map_owned(uint64_t addr, uint64_t size, MemState state, uint8_t perm) {
    std::lock_guard lock(memory_mutex_);
    const Result rc = memory_->map(addr, size, state, perm);
    if (rc.ok())
        used_memory_ += size;
    return rc;
}

Result Kernel::unmap_owned(uint64_t addr, uint64_t size) {
    std::lock_guard lock(memory_mutex_);
    const Result rc = memory_->unmap(addr, size);
    if (rc.ok())
        used_memory_ -= std::min(used_memory_, size);
    return rc;
}

std::vector<std::string> Kernel::debug_output() const {
    std::lock_guard lock(debug_mutex_);
    return debug_output_;
}

Result Kernel::set_heap_size(uint64_t size, uint64_t* out_addr) {
    if (size % kHeapAlignment != 0)
        return kr::kInvalidSize;
    const Region& heap = memory_->layout().heap;
    if (size > heap.size)
        return kr::kOutOfMemory;

    std::lock_guard lock(memory_mutex_);
    if (size > heap_size_) {
        const uint64_t grow = size - heap_size_;
        if (grow > config_.total_memory_size - std::min(config_.total_memory_size, used_memory_))
            return kr::kLimitReached;
        const Result rc = memory_->map(heap.base + heap_size_, grow, MemState::Normal, kPermRW);
        if (rc.failed())
            return rc;
        used_memory_ += grow;
    } else if (size < heap_size_) {
        const uint64_t shrink = heap_size_ - size;
        // Only untouched heap can go: no locked (lent/aliased) pages, plain RW.
        if (!memory_->check(heap.base + size, shrink, MemState::Normal, kPermRW, kAttrNone))
            return kr::kInvalidCurrentMemory;
        memory_->unmap(heap.base + size, shrink);
        used_memory_ -= shrink;
    }
    heap_size_ = size;
    *out_addr = heap.base;
    return kSuccess;
}

Result Kernel::set_memory_permission(uint64_t addr, uint64_t size, uint32_t perm) {
    if (!aligned(addr))
        return kr::kInvalidAddress;
    if (!aligned(size) || size == 0)
        return kr::kInvalidSize;
    if (addr + size <= addr)
        return kr::kInvalidCurrentMemory;
    if (perm != kPermNone && perm != kPermR && perm != kPermRW)
        return kr::kInvalidNewMemoryPermission;
    std::lock_guard lock(memory_mutex_);
    if (!memory_->check_states(addr, size, kReprotectStates, 0, kAttrNone, 0,
                                 kAttrLocked | kAttrIpcLocked | kAttrDeviceShared))
        return kr::kInvalidCurrentMemory;
    memory_->set_perm(addr, size, static_cast<uint8_t>(perm));
    return kSuccess;
}

Result Kernel::set_memory_attribute(uint64_t addr, uint64_t size, uint32_t mask, uint32_t value) {
    if (!aligned(addr))
        return kr::kInvalidAddress;
    if (!aligned(size) || size == 0)
        return kr::kInvalidSize;
    const uint32_t both = mask | value;
    if (both != mask || (both | kAttrUncached) != kAttrUncached)
        return kr::kInvalidCombination;
    std::lock_guard lock(memory_mutex_);
    // Locked/IPC-locked/device-shared memory keeps its attributes.
    if (!memory_->check_states(addr, size, kAttrChangeStates, 0, kAttrNone, 0,
                               kAttrLocked | kAttrIpcLocked | kAttrDeviceShared))
        return kr::kInvalidCurrentMemory;
    memory_->set_attr(addr, size, static_cast<uint8_t>(mask), static_cast<uint8_t>(value));
    return kSuccess;
}

// svcMapMemory aliases heap pages at a stack-region address. Instead of a real
// alias the contents move: the source is inaccessible while mapped, so a copy
// in each direction is indistinguishable for the guest (and Wasm cannot alias
// pages at all).
Result Kernel::map_memory(uint64_t dst, uint64_t src, uint64_t size) {
    if (!aligned(dst) || !aligned(src))
        return kr::kInvalidAddress;
    if (!aligned(size) || size == 0)
        return kr::kInvalidSize;
    if (dst + size <= dst || src + size <= src)
        return kr::kInvalidCurrentMemory;
    std::lock_guard lock(memory_mutex_);
    GuestMemory& m = *memory_;
    if (!m.contains(src, size))
        return kr::kInvalidCurrentMemory;
    if (!m.layout().stack.contains(dst, size))
        return kr::kInvalidMemoryRegion;
    if (!m.check(src, size, MemState::Normal, kPermRW, kAttrNone))
        return kr::kInvalidCurrentMemory;
    if (!m.is_free(dst, size))
        return kr::kInvalidCurrentMemory;

    const Result rc = m.map(dst, size, MemState::Stack, kPermRW);
    if (rc.failed())
        return rc;
    std::memcpy(m.host_ptr(dst, size), m.host_ptr(src, size), size);
    m.set_perm(src, size, kPermNone);
    m.set_attr(src, size, kAttrLocked, kAttrLocked);
    return kSuccess;
}

Result Kernel::unmap_memory(uint64_t dst, uint64_t src, uint64_t size) {
    if (!aligned(dst) || !aligned(src))
        return kr::kInvalidAddress;
    if (!aligned(size) || size == 0)
        return kr::kInvalidSize;
    if (dst + size <= dst || src + size <= src)
        return kr::kInvalidCurrentMemory;
    std::lock_guard lock(memory_mutex_);
    GuestMemory& m = *memory_;
    if (!m.contains(src, size))
        return kr::kInvalidCurrentMemory;
    if (!m.layout().stack.contains(dst, size))
        return kr::kInvalidMemoryRegion;
    if (!m.check(src, size, MemState::Normal, kPermNone, kAttrLocked))
        return kr::kInvalidCurrentMemory;
    if (!m.check(dst, size, MemState::Stack, kPermRW, kAttrNone))
        return kr::kInvalidMemoryRegion;

    m.set_attr(src, size, kAttrLocked, kAttrNone);
    m.set_perm(src, size, kPermRW);
    std::memcpy(m.host_ptr(src, size), m.host_ptr(dst, size), size);
    return m.unmap(dst, size);
}

Result Kernel::query_memory(uint64_t info_addr, uint64_t addr, uint32_t* page_info) {
    std::lock_guard lock(memory_mutex_);
    const BlockInfo b = memory_->query(addr);
    MemoryInfo info{};
    info.addr = b.addr;
    info.size = b.size;
    info.state = static_cast<uint32_t>(b.state);
    info.attr = b.attr;
    info.perm = b.perm;
    if (!memory_->copy_to_user(info_addr, &info, sizeof(info)))
        return kr::kInvalidPointer;
    *page_info = 0;
    return kSuccess;
}

Result Kernel::map_shared_memory(Handle handle, uint64_t addr, uint64_t size, uint32_t perm) {
    if (!aligned(addr))
        return kr::kInvalidAddress;
    if (!aligned(size) || size == 0)
        return kr::kInvalidSize;
    if (addr + size <= addr)
        return kr::kInvalidCurrentMemory;
    if (perm != kPermR && perm != kPermRW)
        return kr::kInvalidNewMemoryPermission;
    auto shm = handles_.get_as<KSharedMemory>(handle);
    if (!shm)
        return kr::kInvalidHandle;
    if ((perm & ~shm->user_perm()) != 0)
        return kr::kInvalidNewMemoryPermission;
    if (size != shm->size())
        return kr::kInvalidSize;
    std::lock_guard lock(memory_mutex_);
    if (!can_contain_mapping(memory_->layout(), addr, size))
        return kr::kInvalidMemoryRegion;
    if (shm->mapped_addr_ != 0)
        return kr::kInvalidState;  // limitation: one mapping per object
    if (!memory_->is_free(addr, size))
        return kr::kInvalidCurrentMemory;
    const Result rc = memory_->map(addr, size, MemState::Shared, static_cast<uint8_t>(perm));
    if (rc.failed())
        return rc;
    std::memcpy(memory_->host_ptr(addr, size), shm->buffer_.data(), size);
    shm->mapped_addr_ = addr;
    return kSuccess;
}

Result Kernel::unmap_shared_memory(Handle handle, uint64_t addr, uint64_t size) {
    if (!aligned(addr))
        return kr::kInvalidAddress;
    if (!aligned(size) || size == 0)
        return kr::kInvalidSize;
    auto shm = handles_.get_as<KSharedMemory>(handle);
    if (!shm)
        return kr::kInvalidHandle;
    std::lock_guard lock(memory_mutex_);
    if (shm->mapped_addr_ != addr || size != shm->size())
        return kr::kInvalidMemoryRegion;
    std::memcpy(shm->buffer_.data(), memory_->host_ptr(addr, size), size);
    memory_->unmap(addr, size);
    shm->mapped_addr_ = 0;
    return kSuccess;
}

Result Kernel::create_transfer_memory(uint64_t addr, uint64_t size, uint32_t perm, Handle* out) {
    if (!aligned(addr))
        return kr::kInvalidAddress;
    if (!aligned(size) || size == 0)
        return kr::kInvalidSize;
    if (addr + size <= addr)
        return kr::kInvalidCurrentMemory;
    if (perm != kPermNone && perm != kPermR && perm != kPermRW)
        return kr::kInvalidNewMemoryPermission;
    std::lock_guard lock(memory_mutex_);
    if (!memory_->check_states(addr, size, kTransferStates, kPermRW, kAttrNone))
        return kr::kInvalidCurrentMemory;
    auto tmem = std::make_shared<KTransferMemory>(*this, addr, size, static_cast<uint8_t>(perm));
    const Result rc = handles_.add(tmem, out);
    if (rc.failed()) {
        tmem.reset();  // nothing locked yet; the destructor restores RW (already RW)
        return rc;
    }
    memory_->set_perm(addr, size, static_cast<uint8_t>(perm));
    memory_->set_attr(addr, size, kAttrLocked, kAttrLocked);
    return kSuccess;
}

Result Kernel::get_info(uint32_t id0, Handle handle, uint64_t id1, uint64_t* out) {
    const Layout& l = memory_->layout();
    switch (id0) {
    case kInfoCoreMask:
    case kInfoPriorityMask:
    case kInfoAliasRegionAddress:
    case kInfoAliasRegionSize:
    case kInfoHeapRegionAddress:
    case kInfoHeapRegionSize:
    case kInfoTotalMemorySize:
    case kInfoUsedMemorySize:
    case kInfoAslrRegionAddress:
    case kInfoAslrRegionSize:
    case kInfoStackRegionAddress:
    case kInfoStackRegionSize:
    case kInfoSystemResourceSizeTotal:
    case kInfoSystemResourceSizeUsed:
    case kInfoProgramId:
    case kInfoUserExceptionContextAddress:
    case kInfoTotalNonSystemMemorySize:
    case kInfoUsedNonSystemMemorySize:
    case kInfoIsApplication: {
        if (id1 != 0)
            return kr::kInvalidCombination;
        if (handle != kPseudoHandleCurrentProcess)
            return kr::kInvalidHandle;
        uint64_t v = 0;
        switch (id0) {
        case kInfoCoreMask: v = config_.core_mask; break;
        case kInfoPriorityMask: v = config_.priority_mask; break;
        case kInfoAliasRegionAddress: v = l.alias.base; break;
        case kInfoAliasRegionSize: v = l.alias.size; break;
        case kInfoHeapRegionAddress: v = l.heap.base; break;
        case kInfoHeapRegionSize: v = l.heap.size; break;
        case kInfoTotalMemorySize:
        case kInfoTotalNonSystemMemorySize: v = config_.total_memory_size; break;
        case kInfoUsedMemorySize:
        case kInfoUsedNonSystemMemorySize: {
            std::lock_guard lock(memory_mutex_);
            v = used_memory_;
            break;
        }
        case kInfoAslrRegionAddress: v = l.start(); break;
        case kInfoAslrRegionSize: v = l.size(); break;
        case kInfoStackRegionAddress: v = l.stack.base; break;
        case kInfoStackRegionSize: v = l.stack.size; break;
        case kInfoProgramId: v = config_.program_id; break;
        case kInfoIsApplication: v = 1; break;
        case kInfoUserExceptionContextAddress: {
            // Horizon gives every process one thread-local-sized region (KProcess's
            // process local region) for its user exception context; the SDK writes
            // into it right at start (dying message region at +0x1c0).
            std::lock_guard lock(uec_mutex_);
            if (user_exception_context_ == 0) {
                const Result rc = scheduler_->allocate_process_local_region(&user_exception_context_);
                if (rc.failed())
                    return rc;
            }
            v = user_exception_context_;
            break;
        }
        default: v = 0; break;  // system resource sizes: none
        }
        *out = v;
        return kSuccess;
    }
    case kInfoDebuggerAttached:
        if (handle != kInvalidHandle || id1 != 0)
            return kr::kInvalidCombination;
        *out = 0;
        return kSuccess;
    case kInfoRandomEntropy:
        if (handle != kInvalidHandle)
            return kr::kInvalidHandle;
        if (id1 >= config_.random_entropy.size())
            return kr::kInvalidCombination;
        *out = config_.random_entropy[id1];
        return kSuccess;
    default:
        return kr::kInvalidEnumValue;
    }
}

Result Kernel::output_debug_string(uint64_t addr, uint64_t size) {
    std::string text(static_cast<size_t>(std::min<uint64_t>(size, 0x10000)), '\0');
    {
        std::lock_guard lock(memory_mutex_);
        if (!memory_->copy_from_user(text.data(), addr, text.size()))
            return kr::kInvalidPointer;
    }
    log(LogLevel::Info, "guest: %s", text.c_str());
    std::lock_guard debug_lock(debug_mutex_);
    debug_output_.push_back(std::move(text));
    return kSuccess;
}

void Kernel::register_named_port(const std::string& name, NamedPortConnect connect) {
    std::lock_guard lock(ports_mutex_);
    named_ports_[name] = std::move(connect);
}

Result Kernel::create_session(std::shared_ptr<SessionHandler> handler, Handle* out) {
    return handles_.add(std::make_shared<KClientSession>(std::move(handler)), out);
}

Result Kernel::connect_to_named_port(uint64_t name_addr, Handle* out) {
    // Names are at most 11 characters plus the terminator.
    char name[12] = {};
    {
        std::lock_guard lock(memory_mutex_);
        for (size_t i = 0; i < sizeof(name); ++i) {
            if (!memory_->copy_from_user(&name[i], name_addr + i, 1))
                return kr::kInvalidPointer;
            if (name[i] == '\0')
                break;
        }
    }
    if (name[sizeof(name) - 1] != '\0')
        return kr::kOutOfRange;
    NamedPortConnect connect;
    {
        std::lock_guard lock(ports_mutex_);
        const auto it = named_ports_.find(name);
        if (it == named_ports_.end()) {
            log(LogLevel::Warn, "svcConnectToNamedPort: no port \"%s\"", name);
            return kr::kNotFound;
        }
        connect = it->second;
    }
    return create_session(connect(), out);
}

SvcOutcome Kernel::svc(uint32_t imm, uint64_t* x) {
    uint64_t args[8];
    if (config_.trace_svcs)
        std::memcpy(args, x, sizeof(args));

    KThread* const self = Scheduler::current();
    // The app is in the background (Scheduler::pause): wait here, before and
    // after the svc, until it is back.
    if (self != nullptr)
        scheduler_->pause_point(*self);
    // The process is ending: the thread leaves guest code instead.
    if (self != nullptr && scheduler_->exiting())
        return SvcOutcome::ExitThread;
    // Virtual clock: every svc of a guest thread costs guest time; deadlines
    // it passes (timeouts, periodic host callbacks) fire here.
    if (self != nullptr && clock_.is_virtual()) {
        clock_.charge(kVirtualSvcCostNs);
        scheduler_->on_clock_advanced();
    }
    // Thread svcs need the calling guest thread; host code (tests, the
    // Unicorn executor) calling svc() directly has none.
    const auto no_thread = [&] {
        log(LogLevel::Error, "svc 0x%02x (%s) called outside a guest thread", imm, svc_name(imm));
        return SvcOutcome::Unimplemented;
    };

    SvcOutcome outcome = SvcOutcome::Continue;
    Result rc = kSuccess;
    bool has_result = true;  // false: x0 is a plain value or untouched
    switch (imm) {
    case kSvcSetHeapSize: {
        uint64_t addr = 0;
        rc = set_heap_size(x[1], &addr);
        x[1] = rc.ok() ? addr : 0;
        break;
    }
    case kSvcSetMemoryPermission:
        rc = set_memory_permission(x[0], x[1], static_cast<uint32_t>(x[2]));
        break;
    case kSvcSetMemoryAttribute:
        rc = set_memory_attribute(x[0], x[1], static_cast<uint32_t>(x[2]), static_cast<uint32_t>(x[3]));
        break;
    case kSvcMapMemory:
        rc = map_memory(x[0], x[1], x[2]);
        break;
    case kSvcUnmapMemory:
        rc = unmap_memory(x[0], x[1], x[2]);
        break;
    case kSvcQueryMemory: {
        uint32_t page_info = 0;
        rc = query_memory(x[0], x[2], &page_info);
        x[1] = page_info;
        break;
    }
    case kSvcExitProcess:
        outcome = SvcOutcome::ExitProcess;
        if (self != nullptr)
            scheduler_->request_exit(self, ProcessExit::Reason::ExitProcess);
        break;
    case kSvcCreateThread: {
        Handle h = kInvalidHandle;
        rc = scheduler_->create_thread(x[1], x[2], x[3], static_cast<int32_t>(x[4]), static_cast<int32_t>(x[5]), &h);
        x[1] = h;
        break;
    }
    case kSvcStartThread:
        rc = scheduler_->start_thread(self, static_cast<Handle>(x[0]));
        break;
    case kSvcExitThread:
        if (self == nullptr)
            return no_thread();
        outcome = SvcOutcome::ExitThread;
        break;
    case kSvcSleepThread:
        if (self == nullptr)
            return no_thread();
        scheduler_->sleep_thread(*self, static_cast<int64_t>(x[0]));
        has_result = false;
        break;
    case kSvcGetThreadPriority: {
        int32_t prio = 0;
        rc = scheduler_->get_thread_priority(static_cast<Handle>(x[1]), &prio);
        x[1] = static_cast<uint32_t>(prio);
        break;
    }
    case kSvcSetThreadPriority:
        if (self == nullptr)
            return no_thread();
        rc = scheduler_->set_thread_priority(*self, static_cast<Handle>(x[0]), static_cast<int32_t>(x[1]));
        break;
    case kSvcGetThreadCoreMask: {
        int32_t core = 0;
        uint64_t mask = 0;
        rc = scheduler_->get_thread_core_mask(static_cast<Handle>(x[2]), &core, &mask);
        x[1] = static_cast<uint32_t>(core);
        x[2] = mask;
        break;
    }
    case kSvcSetThreadCoreMask:
        if (self == nullptr)
            return no_thread();
        rc = scheduler_->set_thread_core_mask(*self, static_cast<Handle>(x[0]), static_cast<int32_t>(x[1]), x[2]);
        break;
    case kSvcGetCurrentProcessorNumber:
        if (self == nullptr)
            return no_thread();
        x[0] = static_cast<uint32_t>(scheduler_->current_processor(*self));
        has_result = false;
        break;
    case kSvcSignalEvent:
        if (self == nullptr)
            return no_thread();
        rc = scheduler_->signal_event(*self, static_cast<Handle>(x[0]));
        break;
    case kSvcClearEvent:
        rc = scheduler_->clear_event(static_cast<Handle>(x[0]));
        break;
    case kSvcResetSignal:
        rc = scheduler_->reset_signal(static_cast<Handle>(x[0]));
        break;
    case kSvcWaitSynchronization: {
        if (self == nullptr)
            return no_thread();
        int32_t index = -1;
        rc = scheduler_->wait_synchronization(*self, x[1], static_cast<int32_t>(x[2]), static_cast<int64_t>(x[3]),
                                              &index);
        x[1] = static_cast<uint32_t>(index);
        break;
    }
    case kSvcCancelSynchronization:
        rc = scheduler_->cancel_synchronization(static_cast<Handle>(x[0]));
        break;
    case kSvcArbitrateLock:
        if (self == nullptr)
            return no_thread();
        rc = scheduler_->arbitrate_lock(*self, static_cast<Handle>(x[0]), x[1], static_cast<uint32_t>(x[2]));
        break;
    case kSvcArbitrateUnlock:
        if (self == nullptr)
            return no_thread();
        rc = scheduler_->arbitrate_unlock(*self, x[0]);
        break;
    case kSvcWaitProcessWideKeyAtomic:
        if (self == nullptr)
            return no_thread();
        rc = scheduler_->wait_process_wide_key(*self, x[0], x[1], static_cast<uint32_t>(x[2]),
                                               static_cast<int64_t>(x[3]));
        break;
    case kSvcSignalProcessWideKey:
        if (self == nullptr)
            return no_thread();
        scheduler_->signal_process_wide_key(*self, x[0], static_cast<int32_t>(x[1]));
        has_result = false;
        break;
    case kSvcWaitForAddress:
        if (self == nullptr)
            return no_thread();
        rc = scheduler_->wait_for_address(*self, x[0], static_cast<uint32_t>(x[1]), static_cast<int32_t>(x[2]),
                                          static_cast<int64_t>(x[3]));
        break;
    case kSvcSignalToAddress:
        if (self == nullptr)
            return no_thread();
        rc = scheduler_->signal_to_address(*self, x[0], static_cast<uint32_t>(x[1]), static_cast<int32_t>(x[2]),
                                           static_cast<int32_t>(x[3]));
        break;
    case kSvcGetSystemTick:
        x[0] = system_tick();
        has_result = false;
        break;
    case kSvcGetThreadId: {
        uint64_t id = 0;
        rc = scheduler_->get_thread_id(static_cast<Handle>(x[1]), &id);
        x[1] = id;
        break;
    }
    case kSvcGetProcessId: {
        // Only the own process exists; thread handles name it as well.
        uint64_t tid = 0;
        if (static_cast<Handle>(x[1]) != kPseudoHandleCurrentProcess &&
            scheduler_->get_thread_id(static_cast<Handle>(x[1]), &tid).failed()) {
            rc = kr::kInvalidHandle;
            break;
        }
        x[1] = config_.process_id;
        break;
    }
    case kSvcConnectToNamedPort: {
        Handle h = kInvalidHandle;
        rc = connect_to_named_port(x[1], &h);
        x[1] = h;
        break;
    }
    case kSvcSendSyncRequest:
    case kSvcSendSyncRequestWithUserBuffer: {
        const bool user_buffer = imm == kSvcSendSyncRequestWithUserBuffer;
        const Handle h = static_cast<Handle>(user_buffer ? x[2] : x[0]);
        uint64_t msg = 0;
        uint64_t size = 0;
        if (user_buffer) {
            msg = x[0];
            size = x[1];
            if (!aligned(msg) || !aligned(size) || size == 0) {
                rc = kr::kInvalidAddress;
                break;
            }
        } else {
            if (self == nullptr)
                return no_thread();
            msg = self->tls_addr();
            size = 0x100;  // the IPC part of the TLS region
        }
        auto session = handles_.get_as<KClientSession>(h);
        if (!session) {
            rc = kr::kInvalidHandle;
            break;
        }
        {
            TimeScope ipc(TimeCat::Ipc);
            outcome = session->handler()->handle_request(*this, msg, size, &rc);
        }
        if (outcome == SvcOutcome::Unimplemented)
            return outcome;
        // On Horizon the client blocks while another process serves the
        // request, so even lower-priority threads of its core get to run. A
        // thread that polls a service in a loop (LGP's audio thread asks
        // audout for released buffers without waiting) would otherwise hold
        // its core slot forever in the baton modes. In the free mode this is
        // only a host yield.
        if (self != nullptr && outcome == SvcOutcome::Continue)
            scheduler_->yield_hint(*self);
        break;
    }
    case kSvcCreateEvent: {
        Handle w = kInvalidHandle;
        Handle r = kInvalidHandle;
        rc = scheduler_->create_event(&w, &r);
        x[1] = w;
        x[2] = r;
        break;
    }
    case kSvcMapSharedMemory:
        rc = map_shared_memory(static_cast<Handle>(x[0]), x[1], x[2], static_cast<uint32_t>(x[3]));
        break;
    case kSvcUnmapSharedMemory:
        rc = unmap_shared_memory(static_cast<Handle>(x[0]), x[1], x[2]);
        break;
    case kSvcCreateTransferMemory: {
        Handle h = kInvalidHandle;
        rc = create_transfer_memory(x[1], x[2], static_cast<uint32_t>(x[3]), &h);
        x[1] = h;
        break;
    }
    case kSvcCloseHandle:
        rc = handles_.close(static_cast<Handle>(x[0]));
        break;
    case kSvcBreak:
        last_break_ = {x[0], x[1], x[2]};
        log(LogLevel::Error, "guest svcBreak(reason=0x%" PRIx64 ", arg=0x%" PRIx64 ", size=0x%" PRIx64 ")", x[0], x[1],
            x[2]);
        outcome = SvcOutcome::Break;
        if (self != nullptr)
            scheduler_->request_exit(self, ProcessExit::Reason::Break, 0, x[0]);
        break;
    case kSvcOutputDebugString:
        rc = output_debug_string(x[0], x[1]);
        break;
    case kSvcGetInfo: {
        uint64_t v = 0;
        rc = get_info(static_cast<uint32_t>(x[1]), static_cast<Handle>(x[2]), x[3], &v);
        x[1] = v;
        break;
    }
    default:
        log(LogLevel::Error, "unimplemented svc 0x%02x (%s)", imm, svc_name(imm));
        return SvcOutcome::Unimplemented;
    }
    if (self != nullptr && outcome == SvcOutcome::Continue)
        scheduler_->pause_point(*self);
    // A thread woken because the process ends leaves guest code.
    if (outcome == SvcOutcome::Continue && self != nullptr && scheduler_->exiting())
        return SvcOutcome::ExitThread;
    if (outcome == SvcOutcome::Continue && has_result)
        x[0] = rc.raw;

    if (config_.trace_svcs) {
        log(LogLevel::Trace,
            "svc %s(0x%" PRIx64 ", 0x%" PRIx64 ", 0x%" PRIx64 ", 0x%" PRIx64 ") -> 0x%" PRIx64 ", 0x%" PRIx64,
            svc_name(imm), args[0], args[1], args[2], args[3], x[0], x[1]);
    }
    return outcome;
}

const char* svc_name(uint32_t imm) {
    switch (imm) {
    case kSvcSetHeapSize: return "SetHeapSize";
    case kSvcSetMemoryPermission: return "SetMemoryPermission";
    case kSvcSetMemoryAttribute: return "SetMemoryAttribute";
    case kSvcMapMemory: return "MapMemory";
    case kSvcUnmapMemory: return "UnmapMemory";
    case kSvcQueryMemory: return "QueryMemory";
    case kSvcExitProcess: return "ExitProcess";
    case kSvcCreateThread: return "CreateThread";
    case kSvcStartThread: return "StartThread";
    case kSvcExitThread: return "ExitThread";
    case kSvcSleepThread: return "SleepThread";
    case kSvcGetThreadPriority: return "GetThreadPriority";
    case kSvcSetThreadPriority: return "SetThreadPriority";
    case kSvcGetThreadCoreMask: return "GetThreadCoreMask";
    case kSvcSetThreadCoreMask: return "SetThreadCoreMask";
    case kSvcGetCurrentProcessorNumber: return "GetCurrentProcessorNumber";
    case kSvcSignalEvent: return "SignalEvent";
    case kSvcClearEvent: return "ClearEvent";
    case kSvcMapSharedMemory: return "MapSharedMemory";
    case kSvcUnmapSharedMemory: return "UnmapSharedMemory";
    case kSvcCreateTransferMemory: return "CreateTransferMemory";
    case kSvcCloseHandle: return "CloseHandle";
    case kSvcResetSignal: return "ResetSignal";
    case kSvcWaitSynchronization: return "WaitSynchronization";
    case kSvcCancelSynchronization: return "CancelSynchronization";
    case kSvcArbitrateLock: return "ArbitrateLock";
    case kSvcArbitrateUnlock: return "ArbitrateUnlock";
    case kSvcWaitProcessWideKeyAtomic: return "WaitProcessWideKeyAtomic";
    case kSvcSignalProcessWideKey: return "SignalProcessWideKey";
    case kSvcGetSystemTick: return "GetSystemTick";
    case kSvcConnectToNamedPort: return "ConnectToNamedPort";
    case kSvcSendSyncRequest: return "SendSyncRequest";
    case kSvcSendSyncRequestWithUserBuffer: return "SendSyncRequestWithUserBuffer";
    case kSvcGetProcessId: return "GetProcessId";
    case kSvcGetThreadId: return "GetThreadId";
    case kSvcBreak: return "Break";
    case kSvcOutputDebugString: return "OutputDebugString";
    case kSvcGetInfo: return "GetInfo";
    case kSvcMapPhysicalMemory: return "MapPhysicalMemory";
    case kSvcUnmapPhysicalMemory: return "UnmapPhysicalMemory";
    case kSvcWaitForAddress: return "WaitForAddress";
    case kSvcSignalToAddress: return "SignalToAddress";
    case kSvcCreateEvent: return "CreateEvent";
    default: return "?";
    }
}

}  // namespace lgp::rt
