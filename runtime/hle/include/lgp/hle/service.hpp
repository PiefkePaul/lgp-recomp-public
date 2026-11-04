// HLE services (P3.3): the server side of IPC sessions, run on the calling
// guest thread. A Service implements commands; a Session (one per client
// handle) routes CMIF requests to its service, or to the objects of a domain
// after ConvertCurrentObjectToDomain.
//
// A command the service does not know stops the guest (svc Unimplemented),
// like an unimplemented svc: nothing is answered with a guessed value.
#pragma once

#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "lgp/hle/hipc.hpp"
#include "lgp/runtime/kernel.hpp"

namespace lgp::hle {

using rt::Handle;
using rt::Kernel;
using rt::Result;

class Service;

class Request {
public:
    Request(Kernel& kernel, const HipcRequest& hipc, uint32_t command, const uint8_t* data, uint32_t size,
            std::vector<uint32_t> in_objects)
        : kernel_(kernel), hipc_(hipc), command_(command), data_(data), size_(size), in_objects_(std::move(in_objects)) {}

    Kernel& kernel() const { return kernel_; }
    uint32_t command() const { return command_; }
    const HipcRequest& hipc() const { return hipc_; }
    uint64_t pid() const { return hipc_.pid; }
    const std::vector<uint32_t>& in_objects() const { return in_objects_; }
    // In interface i (e.g. an IStorage passed to PushInData): the domain
    // object with id in_objects()[i] on a domain session, else the session
    // behind copy handle i. nullptr if there is none.
    std::shared_ptr<Service> in_object(size_t i) const;
    void set_in_services(std::vector<std::shared_ptr<Service>> s) { in_services_ = std::move(s); }

    // Sequential reads of the raw payload, with natural alignment (CMIF packs
    // parameters like a C struct). Reading past the end yields zero.
    template <typename T>
    T pop() {
        T v{};
        pos_ = (pos_ + alignof(T) - 1) & ~(uint32_t{alignof(T)} - 1);
        if (pos_ + sizeof(T) <= size_)
            std::memcpy(&v, data_ + pos_, sizeof(T));
        pos_ += sizeof(T);
        return v;
    }
    uint32_t payload_size() const { return size_; }

    // Buffers as nn::sf declares them: an in buffer is an A (map alias) or X
    // (pointer) descriptor, an out buffer a B or C descriptor. AutoSelect
    // buffers come as both, one of them empty; this returns the non-empty one
    // (size 0 if there is none). Index i counts within each descriptor list,
    // which matches the buffer order as long as a command does not mix
    // map-alias and pointer buffers of the same direction.
    HipcBuffer in_buffer(size_t i) const;
    HipcBuffer out_buffer(size_t i) const;
    // Reads up to max bytes of in buffer i; false if the guest pointer is bad.
    bool read_in(size_t i, std::vector<uint8_t>* out, uint64_t max = kMaxBufferCopy) const;
    // Fixed-size struct from in buffer i; false if the buffer is too small or bad.
    template <typename T>
    bool read_in_as(size_t i, T* out) const {
        const HipcBuffer b = in_buffer(i);
        return b.size >= sizeof(T) && read_guest(b.addr, out, sizeof(T));
    }
    // Writes min(size, buffer size) bytes to out buffer i; false if the
    // guest pointer is bad.
    bool write_out(size_t i, const void* src, uint64_t size) const;
    // User-pointer copies: the guest must be allowed to access the range.
    bool read_guest(uint64_t addr, void* dst, uint64_t size) const;
    bool write_guest(uint64_t addr, const void* src, uint64_t size) const;
    // Kernel-side copies that ignore guest permissions, for memory the guest
    // handed over (transfer memory is often locked at permission None).
    bool read_mapped(uint64_t addr, void* dst, uint64_t size) const;
    bool write_mapped(uint64_t addr, const void* src, uint64_t size) const;

    static constexpr uint64_t kMaxBufferCopy = 64ull << 20;

private:
    Kernel& kernel_;
    const HipcRequest& hipc_;
    uint32_t command_;
    const uint8_t* data_;
    uint32_t size_;
    uint32_t pos_ = 0;
    std::vector<uint32_t> in_objects_;
    std::vector<std::shared_ptr<Service>> in_services_;  // resolved domain ids
};

class Response {
public:
    Result result{};
    template <typename T>
    void push(const T& v) {
        const size_t at = (data.size() + alignof(T) - 1) & ~(size_t{alignof(T)} - 1);
        data.resize(at + sizeof(T));
        std::memcpy(data.data() + at, &v, sizeof(T));
    }
    // Out interfaces: move handles to new sessions, or object ids in a domain.
    void push_object(std::shared_ptr<Service> s) { objects.push_back(std::move(s)); }

    std::vector<uint8_t> data;
    std::vector<std::shared_ptr<Service>> objects;
    std::vector<Handle> copy_handles;
    std::vector<Handle> move_handles;
    // Set by a handler that met a case it does not implement (e.g. an
    // unknown library applet): the guest stops as for an unknown command.
    bool unimplemented = false;
};

class Service : public std::enable_shared_from_this<Service> {
public:
    using Handler = std::function<void(Request&, Response&)>;

    explicit Service(std::string name) : name_(std::move(name)) {}
    virtual ~Service() = default;
    const std::string& name() const { return name_; }

    // False: unknown command (the guest is stopped).
    bool invoke(Request& req, Response& resp);
    // QueryPointerBufferSize: room for C (pointer) buffers. Ryujinx reports
    // 0x8000 for every service; the real values differ per service.
    virtual uint16_t pointer_buffer_size() const { return 0x8000; }

protected:
    void on(uint32_t command, Handler handler) { handlers_[command] = std::move(handler); }

private:
    std::string name_;
    std::map<uint32_t, Handler> handlers_;
};

// Server end of one client session.
class Session final : public rt::SessionHandler {
public:
    explicit Session(std::shared_ptr<Service> service) : object_(std::move(service)) {}
    rt::SvcOutcome handle_request(Kernel& kernel, uint64_t msg, uint64_t size, Result* rc) override;
    bool is_domain() const { return domain_ != nullptr; }
    const std::shared_ptr<Service>& object() const { return object_; }

private:
    rt::SvcOutcome dispatch(Kernel& kernel, uint8_t* buf, uint32_t size, const HipcRequest& hipc, Result* rc);
    rt::SvcOutcome control(Kernel& kernel, uint8_t* buf, uint32_t size, const HipcRequest& hipc,
                           const CmifInHeader& in, Result* rc);
    // Writes the CMIF response; out objects become handles or domain ids.
    void reply(Kernel& kernel, uint8_t* buf, uint32_t size, Response& resp, bool domain_reply);

    // Object table of a domain. Clones of a domain session share it (and the
    // lock), like Horizon's sf: object ids stay valid on every clone.
    struct Domain {
        std::map<uint32_t, std::shared_ptr<Service>> objects;
        uint32_t next_object_id = 1;
    };

    std::shared_ptr<std::mutex> mutex_ = std::make_shared<std::mutex>();
    std::shared_ptr<Service> object_;
    std::shared_ptr<Domain> domain_;  // nullptr: not a domain
};

// Requests being served right now that started at least min_ns ago, one
// line each ("audout:IAudioOut #1, thread 7, 6.2 s, running"; "waiting"
// while the session lock is held by another request). For hang reports:
// a guest thread that never returns from a host call shows up here.
std::vector<std::string> requests_in_flight(uint64_t min_ns);

// sm: (service manager). Services register a factory under their name;
// GetServiceHandle creates a new session to a fresh instance.
class ServiceManager {
public:
    using Factory = std::function<std::shared_ptr<Service>()>;
    void register_service(const std::string& name, Factory factory);
    std::shared_ptr<Service> create(const std::string& name) const;

    // Registers the "sm:" named port on the kernel.
    void install(Kernel& kernel);

private:
    mutable std::mutex mutex_;
    std::map<std::string, Factory> factories_;
};

// sm result codes (module 21).
namespace sm_result {
inline constexpr Result kInvalidClient = rt::make_result(21, 2);
inline constexpr Result kNotRegistered = rt::make_result(21, 7);
}  // namespace sm_result

// CMIF/HIPC result codes (module 10, sf).
namespace sf_result {
inline constexpr Result kInvalidHeaderSize = rt::make_result(10, 202);
inline constexpr Result kInvalidInHeader = rt::make_result(10, 211);
inline constexpr Result kTargetNotFound = rt::make_result(10, 261);
}  // namespace sf_result

}  // namespace lgp::hle
