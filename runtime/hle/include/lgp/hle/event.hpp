// Events owned by HLE services (P3.4): the service keeps the event, the guest
// gets copy handles to its readable half and waits on them with
// svcWaitSynchronization. There is no writable half in the guest's handle
// table; only the service (or a host thread) signals.
#pragma once

#include <memory>

#include "lgp/runtime/kernel.hpp"

namespace lgp::hle {

class Event {
public:
    Event();

    // Never preempts the caller (see Scheduler::signal_readable).
    void signal(rt::Kernel& kernel);
    void clear(rt::Kernel& kernel);
    bool is_signaled(rt::Kernel& kernel) const;

    // New handle in the process' table, for a copy-handle reply. Every call
    // makes a new handle to the same event, as on Horizon.
    rt::Result make_handle(rt::Kernel& kernel, rt::Handle* out);

private:
    std::shared_ptr<rt::KReadableEvent> event_;
};

}  // namespace lgp::hle
