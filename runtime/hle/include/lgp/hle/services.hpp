// Horizon base services the game needs to boot (P3.4): appletOE (am), set,
// time:u, csrng, pctl, acc:u0, plus prepo:u, arp:r, btm:u, bt, lm and apm, and
// the offline network services of P3.5 (nifm:u, ldn:u, friend:u). Command ids and layouts from switchbrew and
// libnx (interface documentation only); which commands exist here follows
// the Ryujinx reference run (docs/analysis/02-services.md), anything else
// stops the guest as an unknown command.
//
// Every value the guest can observe about the "console" comes from
// SystemConfig, so runs are reproducible and the embedding program decides
// (language, account, clock).
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "lgp/hle/event.hpp"
#include "lgp/hle/service.hpp"
#include "lgp/platform/lan.hpp"

namespace lgp::hle {

struct SystemConfig {
    // set: system language as a LanguageCode string ("en-US", "de", "ja", ...)
    // and RegionCode (0 Japan, 1 USA, 2 Europe, 3 Australia, ...).
    std::string language = "en-US";
    int32_t region = 2;
    // am/apm: handheld (720p) or docked (1080p) at the start. The reference
    // run was handheld. The host can switch later (AppletState::set_docked).
    bool docked = false;
    // acc: the single user account, preselected at launch. The save data of
    // fsp-srv is keyed by this id, so it must stay stable across runs.
    std::array<uint64_t, 2> user_id{0x4c47502d55534552ull, 0x0000000000000001ull};
    std::string user_nickname = "Player";
    // time: POSIX time (UTC seconds) when the process starts; nullopt = the
    // host's clock. One fixed time zone without DST: offset from UTC and an
    // abbreviation of at most 7 characters (also the device location name).
    std::optional<int64_t> start_posix_time;
    int32_t utc_offset_seconds = 0;
    std::string time_zone_name = "UTC";
};

// LanguageCode as set and am pass it: the string in a little-endian u64.
uint64_t language_code(const std::string& code);

// The game language for a host locale ("de-DE", "de_AT", "zh-Hant-TW", "pt-BR"):
// one that Let's Go Pikachu/Eevee ship (ja, en-US, en-GB, fr, fr-CA, de, it,
// es, es-419, ko, zh-CN, zh-TW), en-US for any other or an empty locale.
std::string language_for_locale(const std::string& locale);

// Software keyboard (library applet swkbd): what the game asks for, and the
// host's answer. Text is UTF-16 as the applet exchanges it.
struct KeyboardRequest {
    std::u16string header, sub, guide;
    std::u16string initial;       // initial text from the game
    uint32_t min_length = 0;      // UTF-16 units, 0 = none
    uint32_t max_length = 0;      // UTF-16 units, 0 = none
    bool password = false;
    // When the game's text check rejected the previous answer: its message
    // (may be empty) and how many answers it rejected so far.
    std::u16string check_message;
    uint32_t rejected = 0;
};
struct KeyboardResult {
    bool accepted = true;  // false: the user cancelled
    std::u16string text;
};
// Runs on the guest thread that started the keyboard and may block (the
// platform asks the user). Default: the initial text if it fits the length
// limits, else the account nickname; cancel once the game rejected a text.
using KeyboardHandler = std::function<KeyboardResult(const KeyboardRequest&)>;

// What the error applet shows (nn::err): the code as the console prints it
// ("2110-1100", "application error 15") and the game's message, if it gave
// one (system errors keep theirs in system data the runtime does not have).
struct ErrorMessage {
    std::string code;
    std::string text;  // UTF-8, may be empty
};
// Runs on the guest thread that started the applet and may block until the
// user dismissed the message, as on the console. Default: none (only the log).
using MessageHandler = std::function<void(const ErrorMessage&)>;

// UTF-8 <-> UTF-16 for keyboard text; malformed input becomes U+FFFD.
std::u16string utf8_to_utf16(const std::string& s);
std::string utf16_to_utf8(const std::u16string& s);

// am's per-process state (one application). The host pushes applet messages
// here (focus changes, operation mode), the guest receives them through
// ICommonStateGetter.
class AppletState {
public:
    explicit AppletState(const SystemConfig& config);

    const SystemConfig& config() const { return config_; }

    // AppletMessage ids (switchbrew "Applet Manager services").
    static constexpr uint32_t kMessageFocusStateChanged = 15;
    static constexpr uint32_t kMessageOperationModeChanged = 30;
    static constexpr uint32_t kMessagePerformanceModeChanged = 31;

    void push_message(rt::Kernel& kernel, uint32_t message);
    bool pop_message(rt::Kernel& kernel, uint32_t* out);
    // Copy handle to the message event (ICommonStateGetter::GetEventHandle),
    // signaled while messages are queued.
    rt::Result message_event_handle(rt::Kernel& kernel, rt::Handle* out);

    // LaunchParameterKind 1 (UserChannel) and 2 (PreselectedUser) queues.
    bool pop_launch_parameter(uint32_t kind, std::vector<uint8_t>* out);

    // 1 = InFocus, 2 = OutOfFocus (FocusState).
    uint8_t focus_state() const;

    // Operation mode as am (GetOperationMode, GetPerformanceMode) and apm
    // report it: starts at config().docked. set_docked() from any host
    // thread changes it like putting the console into or out of its dock:
    // when the mode really changes, OperationModeChanged and
    // PerformanceModeChanged are queued (as yuzu and Ryujinx do), and the
    // game reads the new mode and resizes its render targets itself. True
    // if the mode changed.
    bool docked() const { return docked_.load(std::memory_order_relaxed); }
    bool set_docked(rt::Kernel& kernel, bool docked);

    void set_keyboard_handler(KeyboardHandler handler);
    KeyboardResult show_keyboard(const KeyboardRequest& request);
    void set_message_handler(MessageHandler handler);
    void show_message(const ErrorMessage& message);

private:
    const SystemConfig config_;
    std::atomic<bool> docked_;
    mutable std::mutex mutex_;
    std::deque<uint32_t> messages_;
    Event message_event_;
    std::deque<std::vector<uint8_t>> user_channel_;
    std::deque<std::vector<uint8_t>> preselected_user_;
    KeyboardHandler keyboard_;
    MessageHandler message_;
};

// What arp:r reports about the running application.
struct ApplicationInfo {
    uint64_t program_id = 0;
    uint32_t version = 0;    // the title version (NACP/CNMT), 0 for the base game
    uint8_t storage_id = 5;  // NcmStorageId: 2 GameCard, 4 BuiltInUser, 5 SdCard
};

// Shared state the host keeps after registration.
struct BaseServices {
    std::shared_ptr<AppletState> applet;
};

// Registers appletOE, set, time:u, csrng, pctl, acc:u0, apm, vi, hwopus, nifm:u,
// ldn:u, friend:u and the quiet services (application info from
// process.program_id) with sm.
BaseServices register_base_services(ServiceManager& sm, const SystemConfig& config,
                                    const rt::ProcessConfig& process);

// Individual registrations (tests, other embeddings).
void register_am(ServiceManager& sm, std::shared_ptr<AppletState> state);
void register_set(ServiceManager& sm, const SystemConfig& config);
void register_time(ServiceManager& sm, const SystemConfig& config);
void register_csrng(ServiceManager& sm, const rt::ProcessConfig& process);
void register_pctl(ServiceManager& sm);
void register_acc(ServiceManager& sm, const SystemConfig& config);
// prepo:u, arp:r, btm:u, bt, lm: succeed without effect.
void register_quiet_services(ServiceManager& sm, const ApplicationInfo& app);
// apm: performance mode from the applet state's operation mode, configurations
// only stored.
void register_apm(ServiceManager& sm, std::shared_ptr<const AppletState> state);
// vi:u/vi:s/vi:m: display, managed layer and its native-window parcel for the NVN-HLE (no compositor).
void register_vi(ServiceManager& sm);
// The "offline console" (P3.5): nifm:u without any network (requests fail
// with 2110-1100), ldn:u alone in the room (no other console in range),
// friend:u whose events never fire.
void register_nifm(ServiceManager& sm);
// Diagnostic (also LGP_NIFM_ONLINE=1): nifm accepts requests and reports a
// Wi-Fi connection, nothing behind it reaches the internet.
void set_nifm_online(bool online);
void register_ldn(ServiceManager& sm);
void register_friends(ServiceManager& sm);
// bsd:u and ssl without a network: start succeeds, sockets fail (sockets.cpp).
void register_sockets(ServiceManager& sm);
// ldn over LAN (docs/analysis/44-multiplayer-stand.md): ldn:u and bsd:u that
// reach other instances of this runtime in the same LAN over UDP instead of
// the offline ones. Discovery: broadcast to `port`, a probe to every address
// of the own /24 (iOS sends no broadcast without the multicast entitlement)
// and to the given peers (port 0 there = `port`).
struct LdnLanConfig {
    std::shared_ptr<platform::ILan> lan;  // nullptr: register the offline services
    uint16_t port = 39871;                // the access point's host UDP port
    std::vector<platform::LanEndpoint> peers;
    bool broadcast = true;
    bool subnet_probe = true;
    uint32_t scan_wait_ms = 150;      // how long one Scan collects answers
    uint32_t connect_timeout_ms = 2000;
    // Keepalive inside a network: every node pings its peer each
    // keepalive_ms; a peer silent for peer_timeout_ms is gone (station: the
    // network is lost with SignalLost; access point: the station is dropped).
    uint32_t keepalive_ms = 1000;
    uint32_t peer_timeout_ms = 5000;
    // Online instead of the LAN (docs/knowledge/ldn-ueber-ryuldn.md): with a
    // server name, ldn goes through that RyuLDN server over TCP (rooms,
    // virtual addresses and datagrams on the server; port, peers and the
    // probe settings above then do not apply). ryuldn_room: rooms are only
    // found by instances with the same text (empty: public rooms).
    std::string ryuldn_server;
    uint16_t ryuldn_port = 30456;
    std::string ryuldn_room;
    // The server behind a WebSocket bridge (ws://, e.g. a Cloudflare tunnel):
    // the RyuLDN bytes travel as binary WebSocket messages to ryuldn_path.
    bool ryuldn_websocket = false;
    std::string ryuldn_path = "/";
    // wss://: TLS under the WebSocket (ILan::connect_tls); the server's
    // certificate is checked against ryuldn_ca_file (PEM), empty: the
    // system's trusted roots.
    bool ryuldn_tls = false;
    std::string ryuldn_ca_file;
    // Both at once (with a server name): ldn goes over the LAN ("nearby
    // players") or through the server ("far away"), whichever
    // set_ldn_remote() asks for at the start of a session (ldn_dual.hpp).
    bool ryuldn_dual = false;
};
void register_local_network(ServiceManager& sm, const LdnLanConfig& config);
// With ryuldn_dual: true sends the next ldn session through the server,
// false over the LAN (default; LGP_LDN_REMOTE=1 starts with true). A network
// that is up keeps its way until it ends. Meant for the online-button mod.
void set_ldn_remote(bool remote);
bool ldn_remote();
// The passphrase a RyuLDN server compares for scans: "Ryujinx-" and eight hex
// digits from the room text (FNV-1a), "" for an empty text (public rooms).
// The server only hides rooms of other passphrases from scans; it does not
// check the passphrase when someone joins.
std::string ryuldn_passphrase(const std::string& room);
// Parses HOST[:PORT] (TCP, port 30456 when missing), ws://HOST[:PORT][/PATH]
// (WebSocket, port 80, path "/") or wss://HOST[:PORT][/PATH] (WebSocket over
// TLS, port 443) into the ryuldn_* fields; false when the text is not one of
// them.
bool parse_ryuldn_server(const std::string& text, LdnLanConfig* config);
// bcat:u never online: empty delivery cache, sync requests fail (bcat.cpp).
void register_bcat(ServiceManager& sm);
// hwopus: Opus decoders (libopus when built with it, else silence of the packet's length).
void register_hwopus(ServiceManager& sm);
// True if hwopus decodes with libopus (false: silence).
bool hwopus_decodes_audio();
// What GetWorkBufferSize answers (assumed, not measured; see hwopus.cpp).
uint32_t hwopus_work_buffer_size(uint32_t sample_rate, uint32_t channels);
// Samples per channel of one Opus packet (TOC byte, RFC 6716), 0 if malformed.
uint32_t opus_packet_samples(const uint8_t* packet, size_t size, uint32_t sample_rate);

// Result codes of these services (switchbrew "Error codes").
namespace am_result {
inline constexpr rt::Result kNoDataInChannel = rt::make_result(128, 2);  // 0x480
inline constexpr rt::Result kNoMessage = rt::make_result(128, 3);        // 0x680
inline constexpr rt::Result kOutOfBounds = rt::make_result(128, 503);    // 0x3EE80
}  // namespace am_result
namespace time_result {
inline constexpr rt::Result kOffsetInvalid = rt::make_result(116, 102);  // 0xCC74, clock source mismatch
}  // namespace time_result
namespace acc_result {
inline constexpr rt::Result kCancelledByUser = rt::make_result(124, 1);  // 0x27C
inline constexpr rt::Result kUserNotExist = rt::make_result(124, 100);  // 0xC87C
inline constexpr rt::Result kNetworkServiceAccountRegistrationRequired = rt::make_result(124, 200);  // 0x1907C
}  // namespace acc_result
namespace nifm_result {
// "Request failed, network not available, no connection reset" (2110-1100).
inline constexpr rt::Result kNetworkNotAvailable = rt::make_result(110, 1100);  // 0x8986E
}  // namespace nifm_result
namespace ldn_result {
inline constexpr rt::Result kInvalidState = rt::make_result(203, 39);    // 0x4ECB
inline constexpr rt::Result kNetworkNotFound = rt::make_result(203, 65);  // 0x82CB
inline constexpr rt::Result kInvalidInput = rt::make_result(203, 96);     // 0xC0CB, "invalid input or state"
}  // namespace ldn_result
namespace bcat_result {
inline constexpr rt::Result kNotFound = rt::make_result(122, 2);                    // 0x47A
inline constexpr rt::Result kNotOpened = rt::make_result(122, 7);                   // 0xE7A
inline constexpr rt::Result kInternetRequestNotAccepted = rt::make_result(122, 8);  // 0x107A
}  // namespace bcat_result

}  // namespace lgp::hle
