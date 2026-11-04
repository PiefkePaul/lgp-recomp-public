// am internals shared by am.cpp and the library applets (swkbd.cpp): the
// IStorage objects and the state of a running library applet.
#pragma once

#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

#include "lgp/hle/services.hpp"

namespace lgp::hle::am_detail {

// IStorage contents: a host buffer (CreateStorage, launch parameters) or the
// guest pages of a transfer memory (CreateTransferMemoryStorage).
class StorageData {
public:
    explicit StorageData(std::vector<uint8_t> data) : data_(std::move(data)) {}
    explicit StorageData(std::shared_ptr<rt::KTransferMemory> tmem) : tmem_(std::move(tmem)) {}

    uint64_t size() const { return tmem_ ? tmem_->size() : data_.size(); }

    // The whole contents as host bytes (library applets parse their input).
    bool contents(const Request& req, std::vector<uint8_t>* out) {
        std::lock_guard lock(mutex_);
        if (!tmem_) {
            *out = data_;
            return true;
        }
        out->resize(static_cast<size_t>(tmem_->size()));
        return out->empty() || req.read_mapped(tmem_->addr(), out->data(), out->size());
    }

    rt::Result read(const Request& req, uint64_t offset, uint64_t addr, uint64_t len) {
        if (offset > size() || len > size() - offset)
            return am_result::kOutOfBounds;
        std::lock_guard lock(mutex_);
        std::vector<uint8_t> tmp(len);
        if (tmem_) {
            if (!req.read_mapped(tmem_->addr() + offset, tmp.data(), len))
                return rt::kernel_result::kInvalidPointer;
        } else {
            std::memcpy(tmp.data(), data_.data() + offset, len);
        }
        return req.write_guest(addr, tmp.data(), len) ? rt::kSuccess : rt::kernel_result::kInvalidPointer;
    }

    rt::Result write(const Request& req, uint64_t offset, uint64_t addr, uint64_t len) {
        if (offset > size() || len > size() - offset)
            return am_result::kOutOfBounds;
        std::lock_guard lock(mutex_);
        std::vector<uint8_t> tmp(len);
        if (!req.read_guest(addr, tmp.data(), len))
            return rt::kernel_result::kInvalidPointer;
        if (tmem_)
            return req.write_mapped(tmem_->addr() + offset, tmp.data(), len) ? rt::kSuccess
                                                                             : rt::kernel_result::kInvalidPointer;
        std::memcpy(data_.data() + offset, tmp.data(), len);
        return rt::kSuccess;
    }

private:
    std::mutex mutex_;
    std::vector<uint8_t> data_;
    std::shared_ptr<rt::KTransferMemory> tmem_;
};

class StorageAccessor final : public Service {
public:
    explicit StorageAccessor(std::shared_ptr<StorageData> data)
        : Service("am:IStorageAccessor"), data_(std::move(data)) {
        on(0, [this](Request&, Response& resp) { resp.push<int64_t>(static_cast<int64_t>(data_->size())); });
        // Write(s64 offset, in buffer 0x21), Read(s64 offset, out buffer 0x22).
        on(10, [this](Request& req, Response& resp) {
            const int64_t offset = req.pop<int64_t>();
            const HipcBuffer b = req.in_buffer(0);
            resp.result = offset < 0 ? am_result::kOutOfBounds
                                     : data_->write(req, static_cast<uint64_t>(offset), b.addr, b.size);
        });
        on(11, [this](Request& req, Response& resp) {
            const int64_t offset = req.pop<int64_t>();
            const HipcBuffer b = req.out_buffer(0);
            resp.result = offset < 0 ? am_result::kOutOfBounds
                                     : data_->read(req, static_cast<uint64_t>(offset), b.addr, b.size);
        });
    }

private:
    std::shared_ptr<StorageData> data_;
};

class Storage final : public Service {
public:
    explicit Storage(std::shared_ptr<StorageData> data) : Service("am:IStorage"), data_(std::move(data)) {
        on(0, [this](Request&, Response& resp) { resp.push_object(std::make_shared<StorageAccessor>(data_)); });
    }
    const std::shared_ptr<StorageData>& data() const { return data_; }

private:
    std::shared_ptr<StorageData> data_;
};

// A library applet as the application sees it through
// ILibraryAppletAccessor: in data pushed before Start, interactive data in
// both directions while it runs, out data and a result when it is done. The
// applet runs inside the calling guest thread (Start, PushInteractiveInData).
class LibraryApplet {
public:
    virtual ~LibraryApplet() = default;

    void push_in(std::shared_ptr<StorageData> data);
    void push_interactive_in(rt::Kernel& kernel, const Request& req, std::shared_ptr<StorageData> data);
    void start(rt::Kernel& kernel, const Request& req);
    std::shared_ptr<StorageData> pop_out(rt::Kernel& kernel);
    std::shared_ptr<StorageData> pop_interactive_out(rt::Kernel& kernel);
    bool completed() const;
    rt::Result result() const;

    Event& state_changed_event() { return state_changed_; }
    Event& out_event() { return out_event_; }
    Event& interactive_out_event() { return interactive_out_event_; }

protected:
    // Subclasses: called with the in data on Start, and per interactive
    // in data. They answer with emit_* and finish().
    virtual void on_start(const Request& req, std::vector<std::shared_ptr<StorageData>> in) = 0;
    virtual void on_interactive_in(const Request& req, std::vector<uint8_t> data) = 0;

    void emit_out(std::vector<uint8_t> data);
    void emit_interactive_out(std::vector<uint8_t> data);
    void finish(rt::Result result = rt::kSuccess);

private:
    void update_events(rt::Kernel& kernel);

    mutable std::mutex mutex_;
    std::vector<std::shared_ptr<StorageData>> in_;
    std::deque<std::shared_ptr<StorageData>> out_, interactive_out_;
    bool started_ = false;
    bool completed_ = false;
    rt::Result result_{};
    Event state_changed_, out_event_, interactive_out_event_;
};

// AppletId 0x11 (LibraryAppletSwkbd), the non-inline software keyboard.
std::shared_ptr<LibraryApplet> make_software_keyboard(std::shared_ptr<AppletState> state);
// AppletId 0x0E (LibraryAppletError): logs the error, closes at once.
std::shared_ptr<LibraryApplet> make_error_applet(std::shared_ptr<AppletState> state);
// AppletId 0x0F (LibraryAppletNetConnect): nifm's failed request, closes at once.
std::shared_ptr<LibraryApplet> make_net_connect();
// AppletId 0x10 (LibraryAppletPlayerSelect): the one user, no Nintendo Account.
std::shared_ptr<LibraryApplet> make_player_select(std::shared_ptr<AppletState> state);

}  // namespace lgp::hle::am_detail
