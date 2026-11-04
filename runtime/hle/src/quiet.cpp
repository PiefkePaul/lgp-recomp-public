// Services the game only reports to or prepares for (P3.4 part 3): prepo:u
// (play reports), arp:r (application properties for those reports), btm:u
// and bt (BLE for the Poké Ball Plus and Pokémon GO: scans run and find
// nothing, so no device is ever connected) and lm (logging).
// Plus apm (performance configuration): LGP never opens it, but libnx's
// application start does. And audren:u for audio device questions only (LGP
// mixes itself and plays through audout:u; the reference run shows just
// GetAudioDeviceServiceWithRevisionInfo, ListAudioDeviceNameAuto and
// SetAudioDeviceOutputVolumeAuto, never OpenAudioRenderer). They succeed without effect; command ids from
// switchbrew ("BCAT services", "Glue services", "BTM services", "Bluetooth
// Driver services", "Log services", "PTM services").
#include <algorithm>
#include <cinttypes>
#include <cstring>
#include <iterator>
#include <map>
#include <mutex>

#include "lgp/hle/services.hpp"
#include "lgp/runtime/log.hpp"

namespace lgp::hle {
namespace {

using rt::LogLevel;

// --- prepo:u ---------------------------------------------------------------------

class PrepoService final : public Service {
public:
    PrepoService() : Service("prepo:u") {
        // SaveReport* in all revisions (10100..10107): an event id (X buffer)
        // and a msgpack report (A buffer), optionally with a user id. Dropped.
        for (uint32_t cmd = 10100; cmd <= 10107; ++cmd)
            on(cmd, [](Request& req, Response&) {
                const auto& a = req.hipc().a;
                rt::log(LogLevel::Debug, "prepo: dropped report (cmd %u, %" PRIu64 " bytes)", req.command(),
                        a.empty() ? uint64_t{0} : a[0].size);
            });
    }
};

// --- arp:r -----------------------------------------------------------------------

class ArpReader final : public Service {
public:
    explicit ArpReader(const ApplicationInfo& app) : Service("arp:r"), app_(app) {
        // GetApplicationLaunchProperty(u64 pid, or the instance id on 10.0+).
        // There is one application, so any id names it; the instance id is the pid.
        on(0, [this](Request& req, Response& resp) {
            const uint64_t id = req.pop<uint64_t>();
            if (id != req.kernel().config().process_id)
                rt::log(LogLevel::Warn, "arp: launch property for unknown id %" PRIx64, id);
            resp.push<uint64_t>(app_.program_id);
            resp.push<uint32_t>(app_.version);
            resp.push<uint8_t>(app_.storage_id);
            resp.push<uint8_t>(0);  // patch storage: none
            resp.push<uint8_t>(0);  // application kind: application
            resp.push<uint8_t>(0);
        });
        // GetApplicationInstanceId(u64 pid) on 10.0+. Before that, 3 was
        // GetApplicationControlPropertyWithApplicationId with an NACP out
        // buffer; that variant would need the NACP, which is not parsed.
        on(3, [](Request& req, Response& resp) {
            const uint64_t pid = req.pop<uint64_t>();
            if (req.out_buffer(0).size != 0)
                rt::log(LogLevel::Warn, "arp: cmd 3 with an out buffer (pre-10.0 NACP query), buffer left as is");
            resp.push<uint64_t>(pid);
        });
    }

private:
    const ApplicationInfo app_;
};

// --- btm:u, bt ---------------------------------------------------------------------

// A console without any BLE device in range (switchbrew "BTM services",
// libnx btmu.h). Scans start and stop and find nothing; nothing is ever
// connected or paired. Commands that need a connection handle (BleConnect,
// GATT, MTU) stay unimplemented: without a scan result the SDK
// has no address to connect to, and switchbrew documents no result for
// "no such connection", so reaching one is worth a stop and a log line.
class BtmUserCore final : public Service {
public:
    BtmUserCore() : Service("btm:IBtmUserCore") {
        // AcquireBle{Scan,Connection,Pairing,ServiceDiscovery,MtuConfig}Event
        // -> bool, event. They never fire.
        for (const uint32_t cmd : {0u, 17u, 21u, 26u, 33u})
            on(cmd, [this, cmd](Request& req, Response& resp) {
                Event& ev = events_[cmd];
                rt::Handle h = rt::kInvalidHandle;
                resp.result = ev.make_handle(req.kernel(), &h);
                if (!resp.result.ok())
                    return;
                resp.push<uint8_t>(1);
                resp.copy_handles.push_back(h);
            });
        // GetBleScanFilterParameter(u16 id) -> BleAdvertisePacketParameter
        // {u16 company_id, u8 pattern[6]}. The console reads it from system
        // data; zeros here are an assumption, harmless because no scan finds
        // anything.
        on(1, [](Request& req, Response& resp) {
            const uint16_t id = req.pop<uint16_t>();
            rt::log(LogLevel::Debug, "btm: GetBleScanFilterParameter(%u)", id);
            resp.push<uint64_t>(0);
        });
        // GetBleScanFilterParameter2(u16 id) -> GattAttributeUuid {u32 size,
        // u8 uuid[16]}. Same assumption: a zero 128-bit UUID.
        on(2, [](Request& req, Response& resp) {
            const uint16_t id = req.pop<uint16_t>();
            rt::log(LogLevel::Debug, "btm: GetBleScanFilterParameter2(%u)", id);
            resp.push<uint32_t>(0x10);
            for (int i = 0; i < 4; ++i)
                resp.push<uint32_t>(0);
        });
        // StartBleScanForGeneral/ForPaired(pid, BleAdvertisePacketParameter,
        // ARUID), StartBleScanForSmartDevice(pid, GattAttributeUuid, ARUID).
        for (const uint32_t cmd : {3u, 6u, 8u})
            on(cmd, [cmd](Request&, Response&) {
                rt::log(LogLevel::Info, "btm: BLE scan %u started, no device in range", cmd);
            });
        for (const uint32_t cmd : {4u, 7u, 9u})  // StopBleScanFor{General,Paired,SmartDevice}
            on(cmd, [](Request&, Response&) {});
        // GetBleScanResultsFor{General,SmartDevice}(pid, ARUID, B buffer) -> u8 total
        for (const uint32_t cmd : {5u, 10u})
            on(cmd, [](Request&, Response& resp) { resp.push<uint8_t>(0); });
        // BleGetConnectionState(pid, ARUID, C buffer of BleConnectionInfo) -> u8 total
        on(20, [](Request&, Response& resp) { resp.push<uint8_t>(0); });
        // BleGetPairedDevices(BleAdvertisePacketParameter, C buffer) -> u8 total
        on(25, [](Request&, Response& resp) { resp.push<uint8_t>(0); });
        // Register/UnregisterBleGattDataPath(pid, BleDataPath {u8, pad, GattAttributeUuid},
        // ARUID): where GATT data for a service UUID goes. Nothing ever
        // arrives, so it is only logged.
        for (const uint32_t cmd : {36u, 37u})
            on(cmd, [](Request& req, Response&) {
                rt::log(LogLevel::Debug, "btm: %sregister BLE GATT data path", req.command() == 37 ? "un" : "");
            });
    }

private:
    std::map<uint32_t, Event> events_;
};

class BtmUser final : public Service {
public:
    BtmUser() : Service("btm:u") {
        on(0, [](Request&, Response& resp) { resp.push_object(std::make_shared<BtmUserCore>()); });  // GetCore
    }
};

class BluetoothUser final : public Service {
public:
    BluetoothUser() : Service("bt") {
        on(9, [this](Request& req, Response& resp) {  // RegisterBleEvent(pid, ARUID) -> event
            rt::Handle h = rt::kInvalidHandle;
            resp.result = ble_.make_handle(req.kernel(), &h);
            if (resp.result.ok())
                resp.copy_handles.push_back(h);
        });
    }

private:
    Event ble_;
};

// --- lm --------------------------------------------------------------------------

class Logger final : public Service {
public:
    Logger() : Service("lm:ILogger") {
        // Log(A buffer: LogPacketHeader + fields). Only counted; the fields
        // are a TLV stream the runtime has no use for yet.
        on(0, [](Request& req, Response&) {
            rt::log(LogLevel::Trace, "lm: log packet of %" PRIu64 " bytes", req.in_buffer(0).size);
        });
        on(1, [](Request&, Response&) {});  // SetDestination(u32)
    }
};

class LogService final : public Service {
public:
    LogService() : Service("lm") {
        on(0, [](Request&, Response& resp) { resp.push_object(std::make_shared<Logger>()); });  // OpenLogger(u64, pid)
    }
};

// --- apm ---------------------------------------------------------------------------

class ApmSession final : public Service {
public:
    ApmSession() : Service("apm:ISession") {
        on(0, [this](Request& req, Response&) {  // SetPerformanceConfiguration(u32 mode, u32 config)
            const uint32_t mode = req.pop<uint32_t>();
            const uint32_t config = req.pop<uint32_t>();
            std::lock_guard lock(mutex_);
            configs_[mode] = config;
        });
        on(1, [this](Request& req, Response& resp) {  // GetPerformanceConfiguration(u32 mode) -> u32
            const uint32_t mode = req.pop<uint32_t>();
            std::lock_guard lock(mutex_);
            const auto it = configs_.find(mode);
            // Unset: Ryujinx's default (CPU 1020 MHz, GPU 384 MHz, EMC 1600 MHz).
            // Not checked against hardware; nothing in LGP reads it.
            resp.push<uint32_t>(it != configs_.end() ? it->second : 0x00010000u);
        });
    }

private:
    std::mutex mutex_;
    std::map<uint32_t, uint32_t> configs_;
};

class ApmManager final : public Service {
public:
    explicit ApmManager(std::shared_ptr<const AppletState> state) : Service("apm") {
        on(0, [](Request&, Response& resp) { resp.push_object(std::make_shared<ApmSession>()); });  // OpenSession
        on(1, [state](Request&, Response& resp) {  // GetPerformanceMode -> 0 normal, 1 boost (docked)
            resp.push<uint32_t>(state->docked() ? 1 : 0);
        });
    }
};

// --- audren:u (audio device questions) ---------------------------------------------

// Device names as the console reports them (switchbrew "Audio services",
// 0x100-byte entries).
constexpr const char* kAudioDevices[] = {"AudioStereoJackOutput", "AudioBuiltInSpeakerOutput", "AudioTvOutput"};
constexpr size_t kAudioDeviceNameSize = 0x100;

class AudioDevice final : public Service {
public:
    AudioDevice() : Service("audren:IAudioDevice") {
        auto list = [](Request& req, Response& resp) {  // ListAudioDeviceName(Auto) -> count, names (B buffer)
            const uint64_t room = req.out_buffer(0).size / kAudioDeviceNameSize;
            const uint32_t n = uint32_t(std::min<uint64_t>(room, std::size(kAudioDevices)));
            std::vector<char> names(n * kAudioDeviceNameSize, 0);
            for (uint32_t i = 0; i < n; ++i)
                std::strncpy(names.data() + i * kAudioDeviceNameSize, kAudioDevices[i], kAudioDeviceNameSize - 1);
            if (n != 0 && !req.write_out(0, names.data(), names.size()))
                resp.result = rt::kernel_result::kInvalidPointer;
            resp.push<uint32_t>(n);
        };
        auto active = [](Request& req, Response&) {  // GetActiveAudioDeviceName(Auto): the speaker (handheld)
            char name[kAudioDeviceNameSize] = {};
            std::strncpy(name, kAudioDevices[1], sizeof(name) - 1);
            (void)req.write_out(0, name, std::min<uint64_t>(sizeof(name), req.out_buffer(0).size));
        };
        auto set_volume = [this](Request& req, Response&) {  // SetAudioDeviceOutputVolume(Auto)(f32, name)
            volume_ = req.pop<float>();
        };
        auto get_volume = [this](Request&, Response& resp) { resp.push<float>(volume_); };
        on(0, list);
        on(1, set_volume);
        on(2, get_volume);
        on(3, active);
        on(5, [](Request&, Response& resp) { resp.push<uint32_t>(2); });  // GetActiveChannelCount
        on(6, list);
        on(7, set_volume);
        on(8, get_volume);
        on(10, active);
    }

private:
    float volume_ = 1.0f;
};

class AudioRendererManager final : public Service {
public:
    AudioRendererManager() : Service("audren:u") {
        // GetAudioDeviceService(u64 aruid), GetAudioDeviceServiceWithRevisionInfo(u32 revision, u64 aruid).
        on(2, [](Request&, Response& resp) { resp.push_object(std::make_shared<AudioDevice>()); });
        on(4, [](Request&, Response& resp) { resp.push_object(std::make_shared<AudioDevice>()); });
    }
};

}  // namespace

void register_apm(ServiceManager& sm, std::shared_ptr<const AppletState> state) {
    sm.register_service("apm", [state] { return std::make_shared<ApmManager>(state); });
}

void register_quiet_services(ServiceManager& sm, const ApplicationInfo& app) {
    sm.register_service("prepo:u", [] { return std::make_shared<PrepoService>(); });
    sm.register_service("arp:r", [app] { return std::make_shared<ArpReader>(app); });
    sm.register_service("btm:u", [] { return std::make_shared<BtmUser>(); });
    sm.register_service("bt", [] { return std::make_shared<BluetoothUser>(); });
    sm.register_service("lm", [] { return std::make_shared<LogService>(); });
    sm.register_service("audren:u", [] { return std::make_shared<AudioRendererManager>(); });
}

}  // namespace lgp::hle
