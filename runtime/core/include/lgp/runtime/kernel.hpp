// The runtime's Horizon kernel: one guest process, its memory, its handles,
// and the supervisor calls.
//
// svc() takes the guest registers x0..x7 in and writes the results back,
// exactly like the hardware ABI. Lifted code reaches it through LGP_SVC
// (guest.hpp); test executors (Unicorn) call it from their SVC hook.
//
// Implemented: the memory svcs and the process queries they need (P3.1),
// threads and synchronization (P3.2, thread.hpp), IPC to HLE sessions (P3.3). Every other svc returns
// SvcOutcome::Unimplemented and is logged, so a missing piece stops the guest
// loudly instead of returning a guessed value.
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "lgp/runtime/clock.hpp"
#include "lgp/runtime/handle_table.hpp"
#include "lgp/runtime/memory.hpp"
#include "lgp/runtime/thread.hpp"

namespace lgp::rt {

struct ProcessConfig {
    // What svcGetInfo(TotalMemorySize) reports; also sizes the heap region
    // (ADR-0001: a runtime switch, 3072 MiB on the web, 3.21 GiB on desktop).
    uint64_t total_memory_size = kDefaultTotalMemorySize;
    // Layout override (tests); default: Layout::standard(total_memory_size).
    bool custom_layout = false;
    Layout layout{};
    GuestMemory::Options memory_options{};

    uint64_t program_id = 0;
    // svcGetProcessId, and the pid the kernel puts into IPC requests that
    // send one. Placeholder: the first application pid after the built-in
    // processes is not measured.
    uint64_t process_id = 0x51;
    // From the NPDM (filled in by the loader). The defaults allow cores 0..2
    // and every priority; they are placeholders, not measured values.
    uint64_t core_mask = 0x7;
    uint64_t priority_mask = ~uint64_t{0};
    // NPDM ideal core: what svcCreateThread's core -2 means.
    int32_t default_core = 0;
    // ADR-0002: CoreBaton for the game; Baton for deterministic tests.
    SchedMode sched_mode = SchedMode::CoreBaton;
    // Guest time (clock.hpp): the host clock, or virtual time that only the
    // guest's own execution moves (reproducible runs together with Baton).
    ClockMode clock_mode = ClockMode::Host;
    // Lower bound for a guest thread's host stack (see scheduler.cpp).
    uint64_t min_host_stack_size = 1ull << 20;
    uint32_t handle_table_size = 1024;
    // svcGetInfo(RandomEntropy, 0..3). Fixed by default so runs are reproducible.
    std::array<uint64_t, 4> random_entropy{0x6c67702d72636d70ull, 0x52554e54494d4521ull, 0x0123456789abcdefull,
                                           0xfedcba9876543210ull};
    // Trace every svc with its arguments and results.
    bool trace_svcs = false;
};

enum class SvcOutcome {
    Continue,       // results are in x0..x7, guest continues after the SVC
    ExitProcess,    // svcExitProcess
    Break,          // svcBreak: the guest reported a fatal error
    Unimplemented,  // svc not implemented in the runtime yet
    ExitThread,     // svcExitThread, or the process is ending: leave guest code
};

class Kernel;

// IPC endpoint behind a client session handle (P3.3). Services are HLE
// (runtime/hle): the handler runs on the calling guest thread inside
// svcSendSyncRequest and reads/writes the message in guest memory.
class SessionHandler {
public:
    virtual ~SessionHandler() = default;
    // msg/size: the message buffer (the thread's TLS, or a user buffer).
    // Returns Continue with the svc result in *rc, or Unimplemented to stop
    // the guest on a request the HLE side does not know.
    virtual SvcOutcome handle_request(Kernel& kernel, uint64_t msg, uint64_t size, Result* rc) = 0;
};

class KClientSession final : public KObject {
public:
    explicit KClientSession(std::shared_ptr<SessionHandler> handler) : handler_(std::move(handler)) {}
    const char* type_name() const override { return "ClientSession"; }
    const std::shared_ptr<SessionHandler>& handler() const { return handler_; }

private:
    std::shared_ptr<SessionHandler> handler_;
};

// svcConnectToNamedPort target ("sm:"): creates the server end of a new session.
using NamedPortConnect = std::function<std::shared_ptr<SessionHandler>()>;

// Memory lent to another party (svcCreateTransferMemory). The owner keeps the
// pages at the permission it asked for, locked, until the last reference is
// gone. HLE services read and write the pages directly through guest memory.
class KTransferMemory final : public KObject {
public:
    KTransferMemory(Kernel& kernel, uint64_t addr, uint64_t size, uint8_t perm);
    ~KTransferMemory() override;
    const char* type_name() const override { return "TransferMemory"; }

    uint64_t addr() const { return addr_; }
    uint64_t size() const { return size_; }
    uint8_t perm() const { return perm_; }

private:
    Kernel& kernel_;
    uint64_t addr_;
    uint64_t size_;
    uint8_t perm_;
};

// Shared memory created by an HLE service (hid, pl, ...). Until the guest maps
// it, its contents live in a host buffer; while mapped, in guest memory. One
// mapping at a time (enough for the two uses measured in P1.3).
class KSharedMemory final : public KObject {
public:
    KSharedMemory(Kernel& kernel, uint64_t size, uint8_t user_perm);
    ~KSharedMemory() override;
    const char* type_name() const override { return "SharedMemory"; }

    uint64_t size() const { return size_; }
    uint8_t user_perm() const { return user_perm_; }
    uint64_t mapped_addr() const { return mapped_addr_; }
    // Current backing (guest mapping or host buffer). Valid until the next
    // map/unmap of this object.
    uint8_t* data();

private:
    friend class Kernel;
    Kernel& kernel_;
    uint64_t size_;
    uint8_t user_perm_;
    uint64_t mapped_addr_ = 0;
    std::vector<uint8_t> buffer_;
};

class Kernel {
public:
    explicit Kernel(const ProcessConfig& config = {});
    ~Kernel();
    Kernel(const Kernel&) = delete;
    Kernel& operator=(const Kernel&) = delete;

    const ProcessConfig& config() const { return config_; }
    GuestMemory& memory() { return *memory_; }
    HandleTable& handles() { return handles_; }
    // Serialises all changes to the address space.
    std::recursive_mutex& memory_mutex() { return memory_mutex_; }

    SvcOutcome svc(uint32_t imm, uint64_t* x);

    Scheduler& scheduler() { return *scheduler_; }
    GuestClock& clock() { return clock_; }
    const GuestClock& clock() const { return clock_; }
    // Named ports (svcConnectToNamedPort), registered by the HLE layer.
    void register_named_port(const std::string& name, NamedPortConnect connect);
    // New client session handle for an HLE session (service handles, out
    // objects of IPC replies).
    Result create_session(std::shared_ptr<SessionHandler> handler, Handle* out);
    // Runs the process from its main thread until it ends (Scheduler::run).
    // Makes this the active kernel; entry points are resolved with
    // lookup_function() (dispatch.hpp), so register the functions (or install
    // a resolver) first.
    ProcessExit run(const MainThreadParams& params);

    // Memory owned by the process (module images, main stack, TLS pages):
    // mapped zeroed and counted in UsedMemorySize.
    Result map_owned(uint64_t addr, uint64_t size, MemState state, uint8_t perm);
    Result unmap_owned(uint64_t addr, uint64_t size);

    uint64_t heap_size() const { return heap_size_; }
    uint64_t used_memory_size() const { return used_memory_; }
    // Guest-visible text from svcOutputDebugString, in call order.
    std::vector<std::string> debug_output() const;
    // svcBreak arguments of the last break.
    std::array<uint64_t, 3> last_break() const { return last_break_; }

private:
    Result set_heap_size(uint64_t size, uint64_t* out_addr);
    Result set_memory_permission(uint64_t addr, uint64_t size, uint32_t perm);
    Result set_memory_attribute(uint64_t addr, uint64_t size, uint32_t mask, uint32_t value);
    Result map_memory(uint64_t dst, uint64_t src, uint64_t size);
    Result unmap_memory(uint64_t dst, uint64_t src, uint64_t size);
    Result query_memory(uint64_t info_addr, uint64_t addr, uint32_t* page_info);
    Result map_shared_memory(Handle handle, uint64_t addr, uint64_t size, uint32_t perm);
    Result unmap_shared_memory(Handle handle, uint64_t addr, uint64_t size);
    Result create_transfer_memory(uint64_t addr, uint64_t size, uint32_t perm, Handle* out);
    Result get_info(uint32_t id0, Handle handle, uint64_t id1, uint64_t* out);
    Result output_debug_string(uint64_t addr, uint64_t size);
    Result connect_to_named_port(uint64_t name_addr, Handle* out);

    friend class KTransferMemory;
    friend class KSharedMemory;

    ProcessConfig config_;
    std::recursive_mutex memory_mutex_;
    std::unique_ptr<GuestMemory> memory_;
    HandleTable handles_;
    uint64_t heap_size_ = 0;
    uint64_t used_memory_ = 0;
    mutable std::mutex debug_mutex_;
    std::vector<std::string> debug_output_;
    std::array<uint64_t, 3> last_break_{};
    std::mutex uec_mutex_;
    uint64_t user_exception_context_ = 0;  // GetInfo(UserExceptionContextAddress), allocated on first use
    std::mutex ports_mutex_;
    std::map<std::string, NamedPortConnect> named_ports_;
    GuestClock clock_;
    // Last member: destroyed first, so threads end before memory and handles go.
    std::unique_ptr<Scheduler> scheduler_;
};

const char* svc_name(uint32_t imm);

}  // namespace lgp::rt
