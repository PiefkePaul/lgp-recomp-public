// appletOE (am for applications): the application proxy and its sub-objects,
// and the library applet accessor. Library applets: the software keyboard
// (swkbd.cpp), error and playerSelect (applets.cpp); any other applet id
// stops the guest.
#include <algorithm>
#include <atomic>
#include <cstring>

#include "am_internal.hpp"
#include "lgp/hle/services.hpp"
#include "lgp/runtime/log.hpp"

namespace lgp::hle {

namespace am_detail {

void LibraryApplet::push_in(std::shared_ptr<StorageData> data) {
    std::lock_guard lock(mutex_);
    in_.push_back(std::move(data));
}

void LibraryApplet::start(rt::Kernel& kernel, const Request& req) {
    std::vector<std::shared_ptr<StorageData>> in;
    {
        std::lock_guard lock(mutex_);
        if (started_)
            return;
        started_ = true;
        in = in_;
    }
    on_start(req, std::move(in));
    update_events(kernel);
}

void LibraryApplet::push_interactive_in(rt::Kernel& kernel, const Request& req, std::shared_ptr<StorageData> data) {
    std::vector<uint8_t> bytes;
    if (!data->contents(req, &bytes)) {
        rt::log(rt::LogLevel::Error, "am: interactive in data not readable");
        return;
    }
    {
        std::lock_guard lock(mutex_);
        if (completed_)
            return;
    }
    on_interactive_in(req, std::move(bytes));
    update_events(kernel);
}

std::shared_ptr<StorageData> LibraryApplet::pop_out(rt::Kernel& kernel) {
    std::shared_ptr<StorageData> d;
    {
        std::lock_guard lock(mutex_);
        if (out_.empty())
            return nullptr;
        d = std::move(out_.front());
        out_.pop_front();
    }
    update_events(kernel);
    return d;
}

std::shared_ptr<StorageData> LibraryApplet::pop_interactive_out(rt::Kernel& kernel) {
    std::shared_ptr<StorageData> d;
    {
        std::lock_guard lock(mutex_);
        if (interactive_out_.empty())
            return nullptr;
        d = std::move(interactive_out_.front());
        interactive_out_.pop_front();
    }
    update_events(kernel);
    return d;
}

bool LibraryApplet::completed() const {
    std::lock_guard lock(mutex_);
    return completed_;
}

rt::Result LibraryApplet::result() const {
    std::lock_guard lock(mutex_);
    return result_;
}

void LibraryApplet::emit_out(std::vector<uint8_t> data) {
    std::lock_guard lock(mutex_);
    out_.push_back(std::make_shared<StorageData>(std::move(data)));
}

void LibraryApplet::emit_interactive_out(std::vector<uint8_t> data) {
    std::lock_guard lock(mutex_);
    interactive_out_.push_back(std::make_shared<StorageData>(std::move(data)));
}

void LibraryApplet::finish(rt::Result result) {
    std::lock_guard lock(mutex_);
    completed_ = true;
    result_ = result;
}

void LibraryApplet::update_events(rt::Kernel& kernel) {
    std::lock_guard lock(mutex_);
    // Out events follow their queues; the state event fires on completion
    // and stays set (the application waits on it, then pops the output).
    if (out_.empty())
        out_event_.clear(kernel);
    else
        out_event_.signal(kernel);
    if (interactive_out_.empty())
        interactive_out_event_.clear(kernel);
    else
        interactive_out_event_.signal(kernel);
    if (completed_)
        state_changed_.signal(kernel);
}

}  // namespace am_detail

namespace {

using namespace am_detail;
using rt::LogLevel;

// PreselectedUser launch parameter (libnx acc.c): magic, u8 1, the user id,
// unused rest; 0x88 bytes.
constexpr uint32_t kPreselectedUserMagic = 0xC79497CA;
constexpr size_t kPreselectedUserSize = 0x88;

// Commands that only acknowledge a setting the runtime does not act on.
void ignore(Request&, Response&) {}


class CommonStateGetter final : public Service {
public:
    explicit CommonStateGetter(std::shared_ptr<AppletState> state)
        : Service("am:ICommonStateGetter"), state_(std::move(state)) {
        on(0, [this](Request& req, Response& resp) {  // GetEventHandle
            rt::Handle h = rt::kInvalidHandle;
            resp.result = state_->message_event_handle(req.kernel(), &h);
            if (resp.result.ok())
                resp.copy_handles.push_back(h);
        });
        on(1, [this](Request& req, Response& resp) {  // ReceiveMessage
            uint32_t message = 0;
            if (state_->pop_message(req.kernel(), &message))
                resp.push<uint32_t>(message);
            else
                resp.result = am_result::kNoMessage;
        });
        on(5, [this](Request&, Response& resp) {  // GetOperationMode: 0 handheld, 1 docked
            resp.push<uint8_t>(state_->docked() ? 1 : 0);
        });
        on(6, [this](Request&, Response& resp) {  // GetPerformanceMode: 0 normal, 1 boost
            resp.push<uint32_t>(state_->docked() ? 1 : 0);
        });
        on(9, [this](Request&, Response& resp) { resp.push<uint8_t>(state_->focus_state()); });
    }

private:
    std::shared_ptr<AppletState> state_;
};

class SelfController final : public Service {
public:
    SelfController() : Service("am:ISelfController") {
        on(9, [this](Request& req, Response& resp) {  // GetLibraryAppletLaunchableEvent
            // Library applets can always be launched: the event stays signaled.
            launchable_.signal(req.kernel());
            rt::Handle h = rt::kInvalidHandle;
            resp.result = launchable_.make_handle(req.kernel(), &h);
            if (resp.result.ok())
                resp.copy_handles.push_back(h);
        });
        on(11, ignore);  // SetOperationModeChangedNotification(u8)
        on(12, ignore);  // SetPerformanceModeChangedNotification(u8)
        on(13, ignore);  // SetFocusHandlingMode(u8, u8, u8)
        on(14, ignore);  // SetRestartMessageEnabled(u8)
        on(16, ignore);  // SetOutOfFocusSuspendingEnabled(u8)
        on(40, [](Request&, Response& resp) {  // CreateManagedDisplayLayer -> u64 layer id
            resp.push<uint64_t>(kManagedLayerId);
        });
        // SetIdleTimeDetectionExtension(u32): 0 disabled, 1 Extended, 2
        // ExtendedUnsafe (longer time until the console dims or sleeps).
        // There is no idle dimming here, so it is only stored. Other values
        // are invalid; switchbrew names no result for them.
        on(62, [this](Request& req, Response& resp) {
            const uint32_t mode = req.pop<uint32_t>();
            if (mode > 2) {
                rt::log(rt::LogLevel::Warn, "am: SetIdleTimeDetectionExtension(%u) out of range", mode);
                resp.unimplemented = true;
                return;
            }
            idle_extension_ = mode;
        });
        on(63, [this](Request&, Response& resp) {  // GetIdleTimeDetectionExtension -> u32
            resp.push<uint32_t>(idle_extension_);
        });
    }

    // vi's OpenLayer receives this id (P4).
    static constexpr uint64_t kManagedLayerId = 1;

private:
    Event launchable_;
    std::atomic<uint32_t> idle_extension_{0};
};

class WindowController final : public Service {
public:
    WindowController() : Service("am:IWindowController") {
        on(1, [](Request& req, Response& resp) {  // GetAppletResourceUserId
            // Applications use their pid as ARUID; hid and others take it back.
            resp.push<uint64_t>(req.kernel().config().process_id);
        });
        on(10, ignore);  // AcquireForegroundRights
    }
};

class AudioController final : public Service {
public:
    AudioController() : Service("am:IAudioController") {
        on(0, [this](Request& req, Response&) {  // SetExpectedMasterVolume(f32 main, f32 library)
            std::lock_guard lock(mutex_);
            main_ = req.pop<float>();
            library_ = req.pop<float>();
        });
        on(1, [this](Request&, Response& resp) {
            std::lock_guard lock(mutex_);
            resp.push<float>(main_);
        });
        on(2, [this](Request&, Response& resp) {
            std::lock_guard lock(mutex_);
            resp.push<float>(library_);
        });
    }

private:
    std::mutex mutex_;
    float main_ = 1.0f;
    float library_ = 1.0f;
};

void copy_event(Request& req, Response& resp, Event& ev) {
    rt::Handle h = rt::kInvalidHandle;
    resp.result = ev.make_handle(req.kernel(), &h);
    if (resp.result.ok())
        resp.copy_handles.push_back(h);
}

// IStorage passed as in interface 0, or nullptr.
std::shared_ptr<StorageData> in_storage(Request& req) {
    const auto storage = std::dynamic_pointer_cast<Storage>(req.in_object(0));
    return storage ? storage->data() : nullptr;
}

class LibraryAppletAccessor final : public Service {
public:
    explicit LibraryAppletAccessor(std::shared_ptr<LibraryApplet> applet)
        : Service("am:ILibraryAppletAccessor"), applet_(std::move(applet)) {
        on(0, [this](Request& req, Response& resp) {  // GetAppletStateChangedEvent
            copy_event(req, resp, applet_->state_changed_event());
        });
        on(1, [this](Request&, Response& resp) { resp.push<uint8_t>(applet_->completed() ? 1 : 0); });  // IsCompleted
        on(10, [this](Request& req, Response&) { applet_->start(req.kernel(), req); });                 // Start
        on(30, [this](Request&, Response& resp) { resp.result = applet_->result(); });                  // GetResult
        on(100, [this](Request& req, Response& resp) {  // PushInData(IStorage)
            auto data = in_storage(req);
            if (!data) {
                resp.result = rt::kernel_result::kInvalidHandle;
                return;
            }
            applet_->push_in(std::move(data));
        });
        on(101, [this](Request& req, Response& resp) {  // PopOutData -> IStorage
            auto data = applet_->pop_out(req.kernel());
            if (data)
                resp.push_object(std::make_shared<Storage>(std::move(data)));
            else
                resp.result = am_result::kNoDataInChannel;
        });
        on(103, [this](Request& req, Response& resp) {  // PushInteractiveInData(IStorage)
            auto data = in_storage(req);
            if (!data) {
                resp.result = rt::kernel_result::kInvalidHandle;
                return;
            }
            applet_->push_interactive_in(req.kernel(), req, std::move(data));
        });
        on(104, [this](Request& req, Response& resp) {  // PopInteractiveOutData -> IStorage
            auto data = applet_->pop_interactive_out(req.kernel());
            if (data)
                resp.push_object(std::make_shared<Storage>(std::move(data)));
            else
                resp.result = am_result::kNoDataInChannel;
        });
        on(105, [this](Request& req, Response& resp) { copy_event(req, resp, applet_->out_event()); });
        on(106, [this](Request& req, Response& resp) { copy_event(req, resp, applet_->interactive_out_event()); });
    }

private:
    std::shared_ptr<LibraryApplet> applet_;
};

class LibraryAppletCreator final : public Service {
public:
    explicit LibraryAppletCreator(std::shared_ptr<AppletState> state)
        : Service("am:ILibraryAppletCreator"), state_(std::move(state)) {
        on(0, [this](Request& req, Response& resp) {  // CreateLibraryApplet(u32 applet id, u32 mode)
            const uint32_t id = req.pop<uint32_t>();
            const uint32_t mode = req.pop<uint32_t>();
            std::shared_ptr<LibraryApplet> applet;
            if (id == kAppletSwkbd)
                applet = make_software_keyboard(state_);
            else if (id == kAppletError)
                applet = make_error_applet(state_);
            else if (id == kAppletNetConnect)
                applet = make_net_connect();
            else if (id == kAppletPlayerSelect)
                applet = make_player_select(state_);
            if (!applet) {
                rt::log(LogLevel::Error, "am: library applet 0x%x (mode %u) not implemented", id, mode);
                resp.unimplemented = true;
                return;
            }
            resp.push_object(std::make_shared<LibraryAppletAccessor>(std::move(applet)));
        });
        on(10, [](Request& req, Response& resp) {  // CreateStorage(s64 size)
            const int64_t size = req.pop<int64_t>();
            if (size < 0 || static_cast<uint64_t>(size) > Request::kMaxBufferCopy) {
                resp.result = am_result::kOutOfBounds;
                return;
            }
            resp.push_object(std::make_shared<Storage>(
                std::make_shared<StorageData>(std::vector<uint8_t>(static_cast<size_t>(size)))));
        });
        // CreateTransferMemoryStorage(u8 writable, s64 size, copy handle tmem)
        on(11, [](Request& req, Response& resp) {
            req.pop<uint8_t>();
            const int64_t size = req.pop<int64_t>();
            const auto& handles = req.hipc().copy_handles;
            auto tmem = handles.empty() ? nullptr
                                        : req.kernel().handles().get_as<rt::KTransferMemory>(handles[0]);
            if (!tmem) {
                resp.result = rt::kernel_result::kInvalidHandle;
                return;
            }
            if (size < 0 || static_cast<uint64_t>(size) > tmem->size()) {
                resp.result = am_result::kOutOfBounds;
                return;
            }
            // A copy handle stays the client's; the storage keeps the object.
            resp.push_object(std::make_shared<Storage>(std::make_shared<StorageData>(std::move(tmem))));
        });
    }

    // AppletId values (switchbrew "Applet Manager services").
    static constexpr uint32_t kAppletError = 0x0E;         // LibraryAppletError
    static constexpr uint32_t kAppletNetConnect = 0x0F;    // LibraryAppletNetConnect
    static constexpr uint32_t kAppletPlayerSelect = 0x10;  // LibraryAppletPlayerSelect
    static constexpr uint32_t kAppletSwkbd = 0x11;         // LibraryAppletSwkbd

private:
    std::shared_ptr<AppletState> state_;
};

class ApplicationFunctions final : public Service {
public:
    explicit ApplicationFunctions(std::shared_ptr<AppletState> state)
        : Service("am:IApplicationFunctions"), state_(std::move(state)) {
        on(1, [this](Request& req, Response& resp) {  // PopLaunchParameter(u32 kind)
            const uint32_t kind = req.pop<uint32_t>();
            std::vector<uint8_t> data;
            if (!state_->pop_launch_parameter(kind, &data)) {
                resp.result = am_result::kNoDataInChannel;
                return;
            }
            resp.push_object(std::make_shared<Storage>(std::make_shared<StorageData>(std::move(data))));
        });
        on(21, [this](Request&, Response& resp) {  // GetDesiredLanguage
            // Horizon picks the system language if the application supports it
            // (NACP), else a fallback. The NACP is not read yet: system language.
            resp.push<uint64_t>(language_code(state_->config().language));
        });
        on(22, [](Request& req, Response&) {  // SetTerminateResult(u32): the result the application ends with
            rt::log(LogLevel::Warn, "am: application sets its terminate result 0x%x", req.pop<uint32_t>());
        });
        on(40, [](Request&, Response& resp) {  // NotifyRunning -> u8
            resp.push<uint8_t>(0);
        });
    }

private:
    std::shared_ptr<AppletState> state_;
};

// Sub-objects the reference run opened but never called.
class EmptyObject final : public Service {
public:
    explicit EmptyObject(std::string name) : Service(std::move(name)) {}
};

class ApplicationProxy final : public Service {
public:
    explicit ApplicationProxy(std::shared_ptr<AppletState> state)
        : Service("am:IApplicationProxy"), state_(std::move(state)) {
        on(0, [this](Request&, Response& resp) { resp.push_object(std::make_shared<CommonStateGetter>(state_)); });
        on(1, [](Request&, Response& resp) { resp.push_object(std::make_shared<SelfController>()); });
        on(2, [](Request&, Response& resp) { resp.push_object(std::make_shared<WindowController>()); });
        on(3, [](Request&, Response& resp) { resp.push_object(std::make_shared<AudioController>()); });
        on(4, [](Request&, Response& resp) {
            resp.push_object(std::make_shared<EmptyObject>("am:IDisplayController"));
        });
        on(10, [](Request&, Response& resp) {
            resp.push_object(std::make_shared<EmptyObject>("am:IProcessWindingController"));
        });
        on(11, [this](Request&, Response& resp) {
            resp.push_object(std::make_shared<LibraryAppletCreator>(state_));
        });
        on(20, [this](Request&, Response& resp) {
            resp.push_object(std::make_shared<ApplicationFunctions>(state_));
        });
        on(1000, [](Request&, Response& resp) {
            resp.push_object(std::make_shared<EmptyObject>("am:IDebugFunctions"));
        });
    }

private:
    std::shared_ptr<AppletState> state_;
};

class ApplicationProxyService final : public Service {
public:
    explicit ApplicationProxyService(std::shared_ptr<AppletState> state)
        : Service("appletOE"), state_(std::move(state)) {
        // OpenApplicationProxy(pid, u64 reserved, copy handle: own process)
        on(0, [this](Request&, Response& resp) { resp.push_object(std::make_shared<ApplicationProxy>(state_)); });
    }

private:
    std::shared_ptr<AppletState> state_;
};

}  // namespace

uint64_t language_code(const std::string& code) {
    uint64_t v = 0;
    std::memcpy(&v, code.data(), std::min<size_t>(code.size(), 8));
    return v;
}

std::string language_for_locale(const std::string& locale) {
    // Subtags in lower case, '_' as '-': "zh-Hant-TW" -> {"zh", "hant", "tw"}.
    std::vector<std::string> tags(1);
    for (char c : locale) {
        if (c == '-' || c == '_' || c == '.' || c == '@') {
            if (c == '.' || c == '@')
                break;  // POSIX "de_DE.UTF-8", "de_DE@euro"
            tags.emplace_back();
        } else {
            tags.back() += static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
        }
    }
    const std::string& lang = tags[0];
    const auto has = [&](const char* tag) { return std::find(tags.begin() + 1, tags.end(), tag) != tags.end(); };
    if (lang == "de" || lang == "it" || lang == "ja" || lang == "ko")
        return lang;
    if (lang == "fr")
        return has("ca") ? "fr-CA" : "fr";
    if (lang == "en")
        return has("gb") || has("ie") || has("au") || has("nz") ? "en-GB" : "en-US";
    if (lang == "es")
        // Spain and anything unnamed: es; the Americas: Latin American Spanish.
        return tags.size() == 1 || has("es") ? "es" : "es-419";
    if (lang == "zh")
        return has("hant") || has("tw") || has("hk") || has("mo") ? "zh-TW" : "zh-CN";
    return "en-US";
}

AppletState::AppletState(const SystemConfig& config) : config_(config), docked_(config.docked) {
    // Horizon tells a starting application that it has the focus.
    messages_.push_back(kMessageFocusStateChanged);
    std::vector<uint8_t> user(kPreselectedUserSize, 0);
    std::memcpy(user.data(), &kPreselectedUserMagic, 4);
    user[4] = 1;
    std::memcpy(user.data() + 8, config_.user_id.data(), 16);
    preselected_user_.push_back(std::move(user));
}

void AppletState::push_message(rt::Kernel& kernel, uint32_t message) {
    std::lock_guard lock(mutex_);
    messages_.push_back(message);
    message_event_.signal(kernel);
}

rt::Result AppletState::message_event_handle(rt::Kernel& kernel, rt::Handle* out) {
    std::lock_guard lock(mutex_);
    // Messages queued before the kernel was reachable (the initial focus
    // message) signal the event now.
    if (!messages_.empty())
        message_event_.signal(kernel);
    return message_event_.make_handle(kernel, out);
}

bool AppletState::pop_message(rt::Kernel& kernel, uint32_t* out) {
    std::lock_guard lock(mutex_);
    if (messages_.empty())
        return false;
    *out = messages_.front();
    messages_.pop_front();
    if (messages_.empty())
        message_event_.clear(kernel);
    return true;
}

bool AppletState::pop_launch_parameter(uint32_t kind, std::vector<uint8_t>* out) {
    std::lock_guard lock(mutex_);
    std::deque<std::vector<uint8_t>>* queue = kind == 1 ? &user_channel_ : kind == 2 ? &preselected_user_ : nullptr;
    if (queue == nullptr) {
        rt::log(LogLevel::Warn, "am: PopLaunchParameter(kind %u)", kind);
        return false;
    }
    if (queue->empty())
        return false;
    *out = std::move(queue->front());
    queue->pop_front();
    return true;
}

uint8_t AppletState::focus_state() const { return 1; }

bool AppletState::set_docked(rt::Kernel& kernel, bool docked) {
    std::lock_guard lock(mutex_);
    if (docked_.exchange(docked, std::memory_order_relaxed) == docked)
        return false;
    // Both changed together, as when the console goes into or out of its dock.
    messages_.push_back(kMessageOperationModeChanged);
    messages_.push_back(kMessagePerformanceModeChanged);
    message_event_.signal(kernel);
    rt::log(LogLevel::Info, "am: operation mode %s", docked ? "docked" : "handheld");
    return true;
}

void AppletState::set_keyboard_handler(KeyboardHandler handler) {
    std::lock_guard lock(mutex_);
    keyboard_ = std::move(handler);
}

void AppletState::set_message_handler(MessageHandler handler) {
    std::lock_guard lock(mutex_);
    message_ = std::move(handler);
}

void AppletState::show_message(const ErrorMessage& message) {
    MessageHandler handler;
    {
        std::lock_guard lock(mutex_);
        handler = message_;
    }
    if (handler)
        handler(message);
}

void register_am(ServiceManager& sm, std::shared_ptr<AppletState> state) {
    sm.register_service("appletOE", [state] { return std::make_shared<ApplicationProxyService>(state); });
}

}  // namespace lgp::hle
