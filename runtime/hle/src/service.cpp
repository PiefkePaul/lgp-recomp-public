#include "lgp/hle/service.hpp"

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <mutex>

#include "lgp/runtime/host_work.hpp"
#include "lgp/runtime/log.hpp"
#include "lgp/runtime/thread.hpp"

namespace lgp::hle {
namespace {

using rt::LogLevel;
using rt::SvcOutcome;
namespace kr = rt::kernel_result;

// Requests larger than this are not plausible (user buffers are pages).
constexpr uint64_t kMaxMessage = 0x10000;

template <typename T>
T read_at(const uint8_t* buf, uint32_t off) {
    T v;
    std::memcpy(&v, buf + off, sizeof(T));
    return v;
}

// Requests in flight (requests_in_flight): one slot per host thread that
// ever served a request, registered once; a request only writes its own
// slot under the slot's (uncontended) lock, so the cost per request is a few
// stores and a reference count, no allocation and no global lock. The slot
// holds the service, so the report can name it after the request ended.
struct InFlight {
    std::mutex mutex;
    std::shared_ptr<Service> target;  // session object, then the domain object once dispatched
    int64_t command = -1;             // -1: not parsed yet
    uint64_t thread = 0;
    uint64_t start_ns = 0;  // 0: idle
    bool running = false;   // false: waiting for the session lock
};
struct InFlightState {
    std::shared_ptr<Service> target;
    int64_t command = -1;
    uint64_t thread = 0, start_ns = 0;
    bool running = false;
};
std::mutex g_slots_mutex;
std::vector<std::shared_ptr<InFlight>> g_slots;

uint64_t steady_ns() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

InFlight& my_slot() {
    thread_local std::shared_ptr<InFlight> slot = [] {
        auto s = std::make_shared<InFlight>();
        std::lock_guard lock(g_slots_mutex);
        g_slots.push_back(s);
        return s;
    }();
    return *slot;
}

// A nested request on the same host thread (a handler calling another
// session) puts the outer one back when it ends.
class InFlightScope {
public:
    explicit InFlightScope(const std::shared_ptr<Service>& object) : slot_(my_slot()) {
        const rt::KThread* t = rt::Scheduler::current();
        const uint64_t now = steady_ns();
        std::lock_guard lock(slot_.mutex);
        outer_ = {std::move(slot_.target), slot_.command, slot_.thread, slot_.start_ns, slot_.running};
        slot_.target = object;
        slot_.command = -1;
        slot_.thread = t != nullptr ? t->id() : 0;
        slot_.start_ns = now != 0 ? now : 1;
        slot_.running = false;
    }
    ~InFlightScope() {
        std::lock_guard lock(slot_.mutex);
        slot_.target = std::move(outer_.target);
        slot_.command = outer_.command;
        slot_.thread = outer_.thread;
        slot_.start_ns = outer_.start_ns;
        slot_.running = outer_.running;
    }
    void running() {
        std::lock_guard lock(slot_.mutex);
        slot_.running = true;
    }
    InFlightScope(const InFlightScope&) = delete;
    InFlightScope& operator=(const InFlightScope&) = delete;

private:
    InFlight& slot_;
    InFlightState outer_;
};

void in_flight_command(const std::shared_ptr<Service>& target, uint32_t command) {
    InFlight& slot = my_slot();
    std::lock_guard lock(slot.mutex);
    if (slot.start_ns == 0)
        return;
    slot.target = target;
    slot.command = command;
}

}  // namespace

std::vector<std::string> requests_in_flight(uint64_t min_ns) {
    std::vector<std::shared_ptr<InFlight>> slots;
    {
        std::lock_guard lock(g_slots_mutex);
        slots = g_slots;
    }
    const uint64_t now = steady_ns();
    std::vector<std::string> out;
    for (const auto& slot : slots) {
        InFlightState e;
        {
            std::lock_guard lock(slot->mutex);
            e = {slot->target, slot->command, slot->thread, slot->start_ns, slot->running};
        }
        if (e.start_ns == 0 || now < e.start_ns || now - e.start_ns < min_ns)
            continue;
        const double age = double(now - e.start_ns) / 1e9;
        const char* state = e.running ? "running" : "waiting for the session";
        const std::string name = e.target ? e.target->name() : "?";
        char line[192];
        if (e.command >= 0)
            std::snprintf(line, sizeof(line), "%s #%" PRId64 ", thread %" PRIu64 ", %.1f s, %s", name.c_str(),
                          e.command, e.thread, age, state);
        else
            std::snprintf(line, sizeof(line), "%s, thread %" PRIu64 ", %.1f s, %s", name.c_str(), e.thread, age,
                          state);
        out.push_back(line);
    }
    return out;
}

namespace {

HipcBuffer pick(const std::vector<HipcBuffer>& first, const std::vector<HipcBuffer>& second, size_t i) {
    if (i < first.size() && first[i].size != 0)
        return first[i];
    if (i < second.size() && second[i].size != 0)
        return second[i];
    return {};
}

}  // namespace

std::shared_ptr<Service> Request::in_object(size_t i) const {
    if (!in_objects_.empty())
        return i < in_services_.size() ? in_services_[i] : nullptr;
    if (i >= hipc_.copy_handles.size())
        return nullptr;
    const auto client = kernel_.handles().get_as<rt::KClientSession>(hipc_.copy_handles[i]);
    if (!client)
        return nullptr;
    const auto session = std::dynamic_pointer_cast<Session>(client->handler());
    return session ? session->object() : nullptr;
}

HipcBuffer Request::in_buffer(size_t i) const { return pick(hipc_.a, hipc_.x, i); }

HipcBuffer Request::out_buffer(size_t i) const { return pick(hipc_.b, hipc_.c, i); }

bool Request::read_guest(uint64_t addr, void* dst, uint64_t size) const {
    std::lock_guard lock(kernel_.memory_mutex());
    return kernel_.memory().copy_from_user(dst, addr, size);
}

bool Request::write_guest(uint64_t addr, const void* src, uint64_t size) const {
    std::lock_guard lock(kernel_.memory_mutex());
    return kernel_.memory().copy_to_user(addr, src, size);
}

bool Request::read_mapped(uint64_t addr, void* dst, uint64_t size) const {
    std::lock_guard lock(kernel_.memory_mutex());
    const uint8_t* p = kernel_.memory().host_ptr(addr, size);
    if (p == nullptr)
        return false;
    std::memcpy(dst, p, size);
    return true;
}

bool Request::write_mapped(uint64_t addr, const void* src, uint64_t size) const {
    std::lock_guard lock(kernel_.memory_mutex());
    uint8_t* p = kernel_.memory().host_ptr(addr, size);
    if (p == nullptr)
        return false;
    std::memcpy(p, src, size);
    return true;
}

bool Request::read_in(size_t i, std::vector<uint8_t>* out, uint64_t max) const {
    const HipcBuffer b = in_buffer(i);
    out->resize(static_cast<size_t>(std::min(b.size, max)));
    return out->empty() || read_guest(b.addr, out->data(), out->size());
}

bool Request::write_out(size_t i, const void* src, uint64_t size) const {
    const HipcBuffer b = out_buffer(i);
    const uint64_t n = std::min(size, b.size);
    return n == 0 || write_guest(b.addr, src, n);
}

bool Service::invoke(Request& req, Response& resp) {
    const auto it = handlers_.find(req.command());
    if (it == handlers_.end())
        return false;
    it->second(req, resp);
    return true;
}

SvcOutcome Session::handle_request(Kernel& kernel, uint64_t msg, uint64_t size, Result* rc) {
    const uint32_t n = static_cast<uint32_t>(std::min(size, kMaxMessage));
    std::vector<uint8_t> buf(n);
    {
        std::lock_guard lock(kernel.memory_mutex());
        const uint8_t* p = kernel.memory().host_ptr(msg, n);
        if (p == nullptr) {
            *rc = kr::kInvalidPointer;
            return SvcOutcome::Continue;
        }
        std::memcpy(buf.data(), p, n);
    }
    std::optional<HipcRequest> hipc = parse_hipc(buf.data(), n);
    if (!hipc) {
        rt::log(LogLevel::Error, "ipc: malformed HIPC message to %s", object_->name().c_str());
        return SvcOutcome::Unimplemented;
    }
    // The kernel fills in the sender's pid; whatever the client wrote is ignored.
    if (hipc->send_pid)
        hipc->pid = kernel.config().process_id;

    SvcOutcome outcome = SvcOutcome::Continue;
    *rc = rt::kSuccess;
    InFlightScope in_flight(object_);
    {
        std::unique_lock lock(*mutex_, std::try_to_lock);
        if (!lock.owns_lock()) {
            // The holder may wait inside its request with its core slot given
            // up (ldn waiting for a server) and want the slot back before it
            // unlocks: wait without the slot (host_work.hpp).
            rt::HostWorkScope away(true);
            lock.lock();
        }
        in_flight.running();
        switch (static_cast<HipcType>(hipc->type)) {
        case HipcType::Close:
            // The client closes its handle right after; nothing to answer.
            return SvcOutcome::Continue;
        case HipcType::Request:
        case HipcType::RequestWithContext:
            outcome = dispatch(kernel, buf.data(), n, *hipc, rc);
            break;
        case HipcType::Control:
        case HipcType::ControlWithContext: {
            const uint32_t off = align16(hipc->data_offset);
            if (off + sizeof(CmifInHeader) > hipc->data_offset + hipc->data_size)
                return SvcOutcome::Unimplemented;
            outcome = control(kernel, buf.data(), n, *hipc, read_at<CmifInHeader>(buf.data(), off), rc);
            break;
        }
        default:
            rt::log(LogLevel::Error, "ipc: HIPC message type %u to %s", hipc->type, object_->name().c_str());
            return SvcOutcome::Unimplemented;
        }
    }
    if (outcome != SvcOutcome::Continue)
        return outcome;
    std::lock_guard lock(kernel.memory_mutex());
    uint8_t* p = kernel.memory().host_ptr(msg, n);
    if (p == nullptr) {
        *rc = kr::kInvalidPointer;
        return SvcOutcome::Continue;
    }
    std::memcpy(p, buf.data(), n);
    return SvcOutcome::Continue;
}

SvcOutcome Session::dispatch(Kernel& kernel, uint8_t* buf, uint32_t size, const HipcRequest& hipc, Result* rc) {
    const uint32_t end = hipc.data_offset + hipc.data_size;
    uint32_t off = align16(hipc.data_offset);
    std::shared_ptr<Service> target = object_;
    std::vector<uint32_t> in_objects;
    uint32_t payload_end = end;
    if (domain_) {
        if (off + sizeof(CmifDomainInHeader) > end)
            return SvcOutcome::Unimplemented;
        const auto dh = read_at<CmifDomainInHeader>(buf, off);
        off += sizeof(CmifDomainInHeader);
        auto& objects = domain_->objects;
        const auto it = objects.find(dh.object_id);
        if (dh.type == 2) {  // close object
            if (it != objects.end())
                objects.erase(it);
            Response resp;
            reply(kernel, buf, size, resp, true);
            return SvcOutcome::Continue;
        }
        if (dh.type != 1) {
            rt::log(LogLevel::Error, "ipc: domain message type %u", dh.type);
            return SvcOutcome::Unimplemented;
        }
        if (it == objects.end()) {
            Response resp;
            resp.result = sf_result::kTargetNotFound;
            reply(kernel, buf, size, resp, true);
            return SvcOutcome::Continue;
        }
        target = it->second;
        payload_end = std::min(end, off + dh.data_size);
        for (uint32_t i = 0; i < dh.num_in_objects && payload_end + 4 * (i + 1) <= end; ++i)
            in_objects.push_back(read_at<uint32_t>(buf, payload_end + 4 * i));
    }
    if (off + sizeof(CmifInHeader) > payload_end)
        return SvcOutcome::Unimplemented;
    const auto in = read_at<CmifInHeader>(buf, off);
    if (in.magic != kCmifInMagic) {
        Response resp;
        resp.result = sf_result::kInvalidInHeader;
        reply(kernel, buf, size, resp, domain_ != nullptr);
        return SvcOutcome::Continue;
    }
    off += sizeof(CmifInHeader);
    std::vector<std::shared_ptr<Service>> in_services;
    for (const uint32_t id : in_objects) {
        const auto obj = domain_->objects.find(id);
        in_services.push_back(obj == domain_->objects.end() ? nullptr : obj->second);
    }
    in_flight_command(target, in.command_id);
    Request req(kernel, hipc, in.command_id, buf + off, payload_end - off, std::move(in_objects));
    req.set_in_services(std::move(in_services));
    Response resp;
    if (!target->invoke(req, resp) || resp.unimplemented) {
        rt::log(LogLevel::Error, "ipc: unimplemented command %u of %s", in.command_id, target->name().c_str());
        return SvcOutcome::Unimplemented;
    }
    rt::log(LogLevel::Trace, "ipc: %s #%u -> 0x%x", target->name().c_str(), in.command_id, resp.result.raw);
    reply(kernel, buf, size, resp, domain_ != nullptr);
    *rc = rt::kSuccess;
    return SvcOutcome::Continue;
}

SvcOutcome Session::control(Kernel& kernel, uint8_t* buf, uint32_t size, const HipcRequest& hipc,
                            const CmifInHeader& in, Result* rc) {
    if (in.magic != kCmifInMagic) {
        Response resp;
        resp.result = sf_result::kInvalidInHeader;
        reply(kernel, buf, size, resp, false);
        return SvcOutcome::Continue;
    }
    const uint32_t payload = align16(hipc.data_offset) + sizeof(CmifInHeader);
    Response resp;
    switch (in.command_id) {
    case 0: {  // ConvertCurrentObjectToDomain
        if (domain_) {
            rt::log(LogLevel::Error, "ipc: %s is already a domain", object_->name().c_str());
            return SvcOutcome::Unimplemented;
        }
        domain_ = std::make_shared<Domain>();
        const uint32_t id = domain_->next_object_id++;
        domain_->objects[id] = object_;
        resp.push<uint32_t>(id);
        break;
    }
    case 1: {  // CopyFromCurrentDomain
        const uint32_t id = payload + 4 <= size ? read_at<uint32_t>(buf, payload) : 0;
        if (!domain_ || domain_->objects.count(id) == 0) {
            resp.result = sf_result::kTargetNotFound;
            break;
        }
        Handle h = rt::kInvalidHandle;
        const Result r = kernel.create_session(std::make_shared<Session>(domain_->objects[id]), &h);
        if (r.failed()) {
            *rc = r;
            return SvcOutcome::Continue;
        }
        resp.move_handles.push_back(h);
        break;
    }
    case 2:    // CloneCurrentObject
    case 4: {  // CloneCurrentObjectEx (tag ignored)
        // A clone of a domain session is a second session to the same
        // domain (libnx's fs opens several this way).
        auto clone = std::make_shared<Session>(object_);
        if (domain_) {
            clone->domain_ = domain_;
            clone->mutex_ = mutex_;
        }
        Handle h = rt::kInvalidHandle;
        const Result r = kernel.create_session(std::move(clone), &h);
        if (r.failed()) {
            *rc = r;
            return SvcOutcome::Continue;
        }
        resp.move_handles.push_back(h);
        break;
    }
    case 3:  // QueryPointerBufferSize
        resp.push<uint16_t>(object_->pointer_buffer_size());
        break;
    default:
        rt::log(LogLevel::Error, "ipc: unimplemented control command %u", in.command_id);
        return SvcOutcome::Unimplemented;
    }
    // Control replies never carry a domain header.
    reply(kernel, buf, size, resp, false);
    *rc = rt::kSuccess;
    return SvcOutcome::Continue;
}

void Session::reply(Kernel& kernel, uint8_t* buf, uint32_t size, Response& resp, bool domain_reply) {
    std::vector<uint32_t> move;
    std::vector<uint32_t> ids;
    for (const auto& obj : resp.objects) {
        if (domain_reply) {
            const uint32_t id = domain_->next_object_id++;
            domain_->objects[id] = obj;
            ids.push_back(id);
        } else {
            Handle h = rt::kInvalidHandle;
            if (kernel.create_session(std::make_shared<Session>(obj), &h).failed())
                rt::log(LogLevel::Error, "ipc: no handle left for an out object of %s", obj->name().c_str());
            move.push_back(h);  // objects come first, then explicit move handles (libnx order)
        }
    }
    move.insert(move.end(), resp.move_handles.begin(), resp.move_handles.end());
    const uint32_t data_size = static_cast<uint32_t>(resp.data.size());
    const uint32_t raw = (domain_reply ? sizeof(CmifDomainOutHeader) : 0) + sizeof(CmifOutHeader) + data_size +
                         4 * static_cast<uint32_t>(ids.size());
    uint32_t p = write_hipc_response(buf, size, raw, resp.copy_handles, move);
    if (p == 0) {
        rt::log(LogLevel::Error, "ipc: reply of %u bytes does not fit the message buffer", raw);
        return;
    }
    if (domain_reply) {
        CmifDomainOutHeader dh{};
        dh.num_out_objects = static_cast<uint32_t>(ids.size());
        std::memcpy(buf + p, &dh, sizeof(dh));
        p += sizeof(dh);
    }
    const CmifOutHeader out{kCmifOutMagic, 0, resp.result.raw, 0};
    std::memcpy(buf + p, &out, sizeof(out));
    p += sizeof(out);
    if (data_size != 0)
        std::memcpy(buf + p, resp.data.data(), data_size);
    p += data_size;
    for (uint32_t id : ids) {
        std::memcpy(buf + p, &id, 4);
        p += 4;
    }
}

}  // namespace lgp::hle
