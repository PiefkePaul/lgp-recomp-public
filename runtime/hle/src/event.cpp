#include "lgp/hle/event.hpp"

namespace lgp::hle {

Event::Event() : event_(std::make_shared<rt::KReadableEvent>()) {}

void Event::signal(rt::Kernel& kernel) { kernel.scheduler().signal_readable(*event_); }

void Event::clear(rt::Kernel& kernel) { kernel.scheduler().clear_readable(*event_); }

bool Event::is_signaled(rt::Kernel& kernel) const { return kernel.scheduler().is_readable_signaled(*event_); }

rt::Result Event::make_handle(rt::Kernel& kernel, rt::Handle* out) { return kernel.handles().add(event_, out); }

}  // namespace lgp::hle
