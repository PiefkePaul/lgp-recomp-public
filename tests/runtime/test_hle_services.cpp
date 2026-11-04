// P3.4/P3.5: the base services (appletOE, set, time:u, csrng, pctl, acc:u0,
// nifm:u, ldn:u, friend:u, library applets)
// through sm: and the IPC svcs, with requests shaped like the SDK's
// (buffer types as switchbrew documents them per command).
#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "ipc_client.hpp"
#include "lgp/hle/audout.hpp"
#include "lgp/hle/event.hpp"
#include "lgp/hle/services.hpp"

using namespace lgp_test;
namespace kr = kernel_result;

namespace {

constexpr int64_t kStart = 1'700'000'000;  // 2023-11-14 22:13:20 UTC, a Tuesday
constexpr uint32_t kCurrentProcess = 0xFFFF8001;

SystemConfig test_config() {
    SystemConfig c;
    c.language = "de";
    c.region = 2;
    c.user_id = {0x1111222233334444ull, 0x5555666677778888ull};
    c.user_nickname = "Paul";
    c.start_posix_time = kStart;
    c.utc_offset_seconds = 3600;
    c.time_zone_name = "CET";
    return c;
}

struct ServicesEnv : Env {
    BaseServices base;
    explicit ServicesEnv(const SystemConfig& config = test_config())
        : Env([](ServiceManager&) {}) {
        // sm: looks factories up per request, so registering after install is fine.
        base = register_base_services(sm, config, small_process());
    }

    // Sends a request without arguments or with the given payload; checks success.
    Reply call(Handle h, uint32_t command, Msg m = {}) {
        m.command = command;
        const std::optional<Reply> r = send(h, m);
        return r ? *r : Reply{};
    }
    // Command that returns one interface as a move handle.
    Handle open(Handle h, uint32_t command, Msg m = {}) {
        const Reply r = call(h, command, std::move(m));
        CHECK_EQ(r.result, 0u);
        CHECK_EQ(r.move.size(), 1u);
        return r.move.empty() ? kInvalidHandle : r.move[0];
    }
    bool signaled(Handle h) {
        auto ev = k.handles().get_as<KReadableEvent>(h);
        CHECK(ev != nullptr);
        return ev && k.scheduler().is_readable_signaled(*ev);
    }
    template <typename T>
    T read(uint64_t addr) {
        T v{};
        CHECK(k.memory().copy_from_user(&v, addr, sizeof(T)));
        return v;
    }
};

std::string code_string(uint64_t code) {
    char s[9] = {};
    std::memcpy(s, &code, 8);
    return s;
}

// --- am ----------------------------------------------------------------------------

Handle open_proxy(ServicesEnv& e) {
    const Handle oe = e.open_service("appletOE");
    Msg m;
    m.send_pid = true;
    m.copy = {kCurrentProcess};
    m.arg<uint64_t>(0);
    return e.open(oe, 0, m);
}

void test_language_for_locale() {
    CHECK(language_for_locale("de-DE") == "de");
    CHECK(language_for_locale("de_AT.UTF-8") == "de");
    CHECK(language_for_locale("en") == "en-US");
    CHECK(language_for_locale("en-GB") == "en-GB");
    CHECK(language_for_locale("fr-CA") == "fr-CA");
    CHECK(language_for_locale("FR-fr") == "fr");
    CHECK(language_for_locale("es-ES") == "es");
    CHECK(language_for_locale("es-MX") == "es-419");
    CHECK(language_for_locale("zh-Hant-TW") == "zh-TW");
    CHECK(language_for_locale("zh-CN") == "zh-CN");
    CHECK(language_for_locale("ja-JP") == "ja");
    CHECK(language_for_locale("nl-NL") == "en-US");  // not in the game
    CHECK(language_for_locale("") == "en-US");
}

void test_am_messages() {
    ServicesEnv e;
    const Handle proxy = open_proxy(e);
    const Handle getter = e.open(proxy, 0);

    const Reply ev = e.call(getter, 0);  // GetEventHandle
    CHECK_EQ(ev.result, 0u);
    CHECK_EQ(ev.copy.size(), 1u);
    const Handle event = ev.copy.empty() ? kInvalidHandle : ev.copy[0];
    CHECK(e.signaled(event));  // the initial FocusStateChanged is queued

    Reply msg = e.call(getter, 1);  // ReceiveMessage
    CHECK_EQ(msg.result, 0u);
    CHECK_EQ(msg.get<uint32_t>(0), AppletState::kMessageFocusStateChanged);
    CHECK(!e.signaled(event));
    CHECK_EQ(e.call(getter, 1).result, am_result::kNoMessage.raw);

    // The host pushes a message: event signaled, guest receives it.
    e.base.applet->push_message(e.k, AppletState::kMessageOperationModeChanged);
    CHECK(e.signaled(event));
    msg = e.call(getter, 1);
    CHECK_EQ(msg.get<uint32_t>(0), AppletState::kMessageOperationModeChanged);
    CHECK(!e.signaled(event));

    CHECK_EQ(e.call(getter, 5).get<uint8_t>(0), 0u);   // GetOperationMode: handheld
    CHECK_EQ(e.call(getter, 6).get<uint32_t>(0), 0u);  // GetPerformanceMode: normal
    CHECK_EQ(e.call(getter, 9).get<uint8_t>(0), 1u);   // GetCurrentFocusState: InFocus

    // Docked configuration.
    SystemConfig docked = test_config();
    docked.docked = true;
    ServicesEnv d(docked);
    const Handle dg = d.open(open_proxy(d), 0);
    CHECK_EQ(d.call(dg, 5).get<uint8_t>(0), 1u);
    CHECK_EQ(d.call(dg, 6).get<uint32_t>(0), 1u);
}

// The host switches between handheld and docked while the game runs (the
// app's TV mode setting): am and apm report the new mode, and the guest gets
// both change messages once per real change.
void test_am_operation_mode_switch() {
    ServicesEnv e;
    const Handle getter = e.open(open_proxy(e), 0);
    const Handle apm = e.open_service("apm");
    const Reply ev = e.call(getter, 0);
    const Handle event = ev.copy.empty() ? kInvalidHandle : ev.copy[0];
    CHECK_EQ(e.call(getter, 1).get<uint32_t>(0), AppletState::kMessageFocusStateChanged);
    CHECK(!e.signaled(event));

    CHECK(!e.base.applet->set_docked(e.k, false));  // already handheld: nothing queued
    CHECK(!e.signaled(event));

    CHECK(e.base.applet->set_docked(e.k, true));
    CHECK(e.base.applet->docked());
    CHECK(e.signaled(event));
    CHECK_EQ(e.call(getter, 1).get<uint32_t>(0), AppletState::kMessageOperationModeChanged);
    CHECK_EQ(e.call(getter, 1).get<uint32_t>(0), AppletState::kMessagePerformanceModeChanged);
    CHECK_EQ(e.call(getter, 1).result, am_result::kNoMessage.raw);
    CHECK(!e.signaled(event));
    CHECK_EQ(e.call(getter, 5).get<uint8_t>(0), 1u);   // GetOperationMode: docked
    CHECK_EQ(e.call(getter, 6).get<uint32_t>(0), 1u);  // GetPerformanceMode: boost
    CHECK_EQ(e.call(apm, 1).get<uint32_t>(0), 1u);     // apm GetPerformanceMode

    CHECK(!e.base.applet->set_docked(e.k, true));
    CHECK(!e.signaled(event));
    CHECK(e.base.applet->set_docked(e.k, false));
    CHECK_EQ(e.call(getter, 1).get<uint32_t>(0), AppletState::kMessageOperationModeChanged);
    CHECK_EQ(e.call(getter, 1).get<uint32_t>(0), AppletState::kMessagePerformanceModeChanged);
    CHECK_EQ(e.call(getter, 5).get<uint8_t>(0), 0u);
    CHECK_EQ(e.call(apm, 1).get<uint32_t>(0), 0u);
}

void test_am_proxy_objects() {
    ServicesEnv e;
    const Handle proxy = open_proxy(e);

    const Handle self = e.open(proxy, 1);
    for (uint32_t cmd : {11u, 12u, 14u, 16u}) {
        Msg m;
        m.arg<uint8_t>(1);
        CHECK_EQ(e.call(self, cmd, m).result, 0u);
    }
    Msg focus;
    focus.arg<uint8_t>(0).arg<uint8_t>(0).arg<uint8_t>(1);
    CHECK_EQ(e.call(self, 13, focus).result, 0u);
    CHECK_EQ(e.call(self, 40).get<uint64_t>(0), 1u);  // CreateManagedDisplayLayer
    CHECK_EQ(e.call(self, 63).get<uint32_t>(0), 0u);  // GetIdleTimeDetectionExtension: disabled
    Msg idle;
    idle.arg<uint32_t>(1);  // Extended
    CHECK_EQ(e.call(self, 62, idle).result, 0u);
    CHECK_EQ(e.call(self, 63).get<uint32_t>(0), 1u);
    const Reply launchable = e.call(self, 9);
    CHECK_EQ(launchable.copy.size(), 1u);
    if (!launchable.copy.empty())
        CHECK(e.signaled(launchable.copy[0]));

    const Handle window = e.open(proxy, 2);
    CHECK_EQ(e.call(window, 1).get<uint64_t>(0), 0x1234u);  // ARUID = pid
    CHECK_EQ(e.call(window, 10).result, 0u);

    const Handle audio = e.open(proxy, 3);
    CHECK(e.call(audio, 1).get<float>(0) == 1.0f);
    Msg vol;
    vol.arg<float>(0.5f).arg<float>(0.25f);
    CHECK_EQ(e.call(audio, 0, vol).result, 0u);
    CHECK(e.call(audio, 1).get<float>(0) == 0.5f);
    CHECK(e.call(audio, 2).get<float>(0) == 0.25f);

    for (uint32_t cmd : {4u, 10u, 1000u})
        CHECK(e.open(proxy, cmd) != kInvalidHandle);

    // Library applets without an HLE stop the guest.
    const Handle creator = e.open(proxy, 11);
    Msg create;
    create.command = 0;
    create.arg<uint32_t>(0x0D).arg<uint32_t>(0);  // dataErase
    e.send(creator, create, SvcOutcome::Unimplemented);
}

void test_am_launch_parameters() {
    ServicesEnv e;
    const Handle proxy = open_proxy(e);
    const Handle funcs = e.open(proxy, 20);

    Msg user;
    user.arg<uint32_t>(1);  // UserChannel: empty
    CHECK_EQ(e.call(funcs, 1, user).result, am_result::kNoDataInChannel.raw);

    Msg pre;
    pre.arg<uint32_t>(2);  // PreselectedUser
    const Handle storage = e.open(funcs, 1, pre);
    CHECK_EQ(e.call(funcs, 1, pre).result, am_result::kNoDataInChannel.raw);  // popped
    const Handle acc = e.open(storage, 0);
    CHECK_EQ(e.call(acc, 0).get<int64_t>(0), 0x88);
    Msg read;
    read.arg<int64_t>(0);
    read.b = {{kBufB, 0x88, 0}};
    CHECK_EQ(e.call(acc, 11, read).result, 0u);
    CHECK_EQ(e.read<uint32_t>(kBufB), 0xC79497CAu);
    CHECK_EQ(e.read<uint8_t>(kBufB + 4), 1u);
    CHECK_EQ(e.read<uint64_t>(kBufB + 8), 0x1111222233334444ull);
    CHECK_EQ(e.read<uint64_t>(kBufB + 16), 0x5555666677778888ull);

    CHECK(code_string(e.call(funcs, 21).get<uint64_t>(0)) == "de");  // GetDesiredLanguage
    const Reply running = e.call(funcs, 40);
    CHECK_EQ(running.result, 0u);
    CHECK_EQ(running.get<uint8_t>(0), 0u);
}

void test_am_storage() {
    ServicesEnv e;
    const Handle creator = e.open(open_proxy(e), 11);

    Msg create;
    create.arg<int64_t>(0x10);
    const Handle storage = e.open(creator, 10, create);
    const Handle acc = e.open(storage, 0);
    CHECK_EQ(e.call(acc, 0).get<int64_t>(0), 0x10);

    const char text[] = "abcd";
    CHECK(e.k.memory().copy_to_user(kBufA, text, 4));
    Msg write;
    write.arg<int64_t>(4);
    write.a = {{kBufA, 4, 0}};
    CHECK_EQ(e.call(acc, 10, write).result, 0u);
    Msg read;
    read.arg<int64_t>(0);
    read.b = {{kBufB, 0x10, 0}};
    CHECK_EQ(e.call(acc, 11, read).result, 0u);
    CHECK_EQ(e.read<uint32_t>(kBufB), 0u);
    CHECK_EQ(e.read<uint32_t>(kBufB + 4), 0x64636261u);

    Msg past = write;
    past.payload.clear();
    past.arg<int64_t>(0xE);
    CHECK_EQ(e.call(acc, 10, past).result, am_result::kOutOfBounds.raw);
    Msg negative = read;
    negative.payload.clear();
    negative.arg<int64_t>(-1);
    CHECK_EQ(e.call(acc, 11, negative).result, am_result::kOutOfBounds.raw);

    // Transfer memory storage: the guest locked the pages at permission None,
    // the service still reads and writes them.
    const uint64_t tm = kBufA + 0x1000;
    CHECK(e.k.memory().copy_to_user(tm, "xyz", 3));
    const auto x = e.svc(kSvcCreateTransferMemory, {0, tm, 0x1000, kPermNone});
    CHECK_EQ(x[0], kSuccess.raw);
    const Handle tmem = static_cast<Handle>(x[1]);
    Msg ctm;
    ctm.copy = {tmem};
    ctm.arg<uint8_t>(1).arg<int64_t>(0x1000);
    const Handle ts = e.open(creator, 11, ctm);
    const Handle tacc = e.open(ts, 0);
    CHECK_EQ(e.call(tacc, 0).get<int64_t>(0), 0x1000);
    Msg tread;
    tread.arg<int64_t>(0);
    tread.b = {{kBufB, 3, 0}};
    CHECK_EQ(e.call(tacc, 11, tread).result, 0u);
    char back[4] = {};
    CHECK(e.k.memory().copy_from_user(back, kBufB, 3));
    CHECK(std::memcmp(back, "xyz", 3) == 0);
    Msg twrite;
    twrite.arg<int64_t>(0x10);
    twrite.a = {{kBufA, 4, 0}};
    CHECK_EQ(e.call(tacc, 10, twrite).result, 0u);
    char direct[4] = {};
    CHECK(!e.k.memory().copy_from_user(direct, tm + 0x10, 4));  // still locked for the guest
    std::memcpy(direct, e.k.memory().host_ptr(tm + 0x10, 4), 4);
    CHECK(std::memcmp(direct, "abcd", 4) == 0);

    Msg too_big = ctm;
    too_big.payload.clear();
    too_big.arg<uint8_t>(1).arg<int64_t>(0x2000);
    CHECK_EQ(e.call(creator, 11, too_big).result, am_result::kOutOfBounds.raw);
}

void test_am_domain_copy_handle() {
    // nn::sf turns the proxy session into a domain; copy handles still arrive.
    ServicesEnv e;
    const Handle proxy = open_proxy(e);
    Msg convert;
    convert.type = 5;
    convert.command = 0;
    const uint32_t root = e.send(proxy, convert)->get<uint32_t>(0);
    Msg get;
    get.domain = true;
    get.object_id = root;
    get.command = 0;
    const Reply o = *e.send(proxy, get, SvcOutcome::Continue, true);
    CHECK_EQ(o.object_ids.size(), 1u);
    Msg ev;
    ev.domain = true;
    ev.object_id = o.object_ids.empty() ? 0 : o.object_ids[0];
    ev.command = 0;
    const Reply r = *e.send(proxy, ev, SvcOutcome::Continue, true);
    CHECK_EQ(r.result, 0u);
    CHECK_EQ(r.copy.size(), 1u);
    if (!r.copy.empty())
        CHECK(e.signaled(r.copy[0]));
}

// --- set ---------------------------------------------------------------------------

void test_set() {
    ServicesEnv e;
    const Handle set = e.open_service("set");
    CHECK(code_string(e.call(set, 0).get<uint64_t>(0)) == "de");
    CHECK_EQ(e.call(set, 3).get<int32_t>(0), 15);
    CHECK_EQ(e.call(set, 6).get<int32_t>(0), 18);
    CHECK_EQ(e.call(set, 4).get<int32_t>(0), 2);

    Msg list2;
    list2.b = {{kBufB, 0x40 * 8, 0}};  // type 0x6, as the SDK sizes it
    CHECK_EQ(e.call(set, 5, list2).get<int32_t>(0), 18);
    CHECK(code_string(e.read<uint64_t>(kBufB)) == "ja");
    CHECK(code_string(e.read<uint64_t>(kBufB + 3 * 8)) == "de");
    CHECK(code_string(e.read<uint64_t>(kBufB + 17 * 8)) == "pt-BR");

    Msg list1;
    list1.c = {{kBufA, 0xF * 8}};  // type 0xA
    CHECK_EQ(e.call(set, 1, list1).get<int32_t>(0), 15);
    CHECK(code_string(e.read<uint64_t>(kBufA + 14 * 8)) == "es-419");

    Msg small;
    small.b = {{kBufB, 2 * 8, 0}};
    CHECK_EQ(e.call(set, 5, small).get<int32_t>(0), 2);
}

// --- time --------------------------------------------------------------------------

struct CalendarTime {
    uint16_t year;
    uint8_t month, day, hour, minute, second, pad;
};
struct AdditionalInfo {
    uint32_t day_of_week, day_of_year;
    char name[8];
    uint32_t is_dst;
    int32_t utc_offset;
};
struct ClockContext {
    int64_t offset;
    int64_t time_point;
    uint8_t source[16];
};
static_assert(sizeof(ClockContext) == 0x20);

void check_calendar(const Reply& r, uint16_t y, uint8_t mo, uint8_t d, uint8_t h, uint8_t mi, uint8_t s, uint32_t dow,
                    uint32_t doy) {
    CHECK_EQ(r.result, 0u);
    const auto ct = r.get<CalendarTime>(0);
    const auto info = r.get<AdditionalInfo>(8);
    CHECK_EQ(ct.year, y);
    CHECK_EQ(ct.month, mo);
    CHECK_EQ(ct.day, d);
    CHECK_EQ(ct.hour, h);
    CHECK_EQ(ct.minute, mi);
    CHECK_EQ(ct.second, s);
    CHECK_EQ(info.day_of_week, dow);
    CHECK_EQ(info.day_of_year, doy);
    CHECK(std::string(info.name) == "CET");
    CHECK_EQ(info.is_dst, 0u);
    CHECK_EQ(info.utc_offset, 3600);
}

void test_time_clocks() {
    ServicesEnv e;
    const Handle time = e.open_service("time:u");
    const Handle user = e.open(time, 0);
    const int64_t now = e.call(user, 0).get<int64_t>(0);
    CHECK(now >= kStart && now < kStart + 60);
    const auto ctx = e.call(user, 2).get<ClockContext>(0);
    const Handle steady = e.open(time, 2);
    const Reply tp = e.call(steady, 0);
    const int64_t steady_now = tp.get<int64_t>(0);
    CHECK(std::memcmp(tp.data.data() + 8, ctx.source, 16) == 0);
    const int64_t again = e.call(user, 0).get<int64_t>(0);
    CHECK(again >= now && again - (ctx.offset + steady_now) <= 1 && again - (ctx.offset + steady_now) >= 0);
    // Network and local clocks are the same clock.
    for (uint32_t cmd : {1u, 4u}) {
        const int64_t t = e.call(e.open(time, cmd), 0).get<int64_t>(0);
        CHECK(t >= now && t < now + 60);
    }
    CHECK_EQ(e.call(time, 100).get<uint8_t>(0), 0u);  // automatic correction off

    // CalculateMonotonicSystemClockBaseTimePoint: steady clock = tick seconds here.
    Msg base;
    base.arg(ctx);
    const Reply b = e.call(time, 300, base);
    CHECK_EQ(b.result, 0u);
    CHECK_EQ(b.get<int64_t>(0), ctx.offset);
    ClockContext other = ctx;
    other.source[0] ^= 1;
    Msg mismatch;
    mismatch.arg(other);
    CHECK_EQ(e.call(time, 300, mismatch).result, time_result::kOffsetInvalid.raw);

    // Host clock when no start time is configured.
    SystemConfig host = test_config();
    host.start_posix_time.reset();
    ServicesEnv h(host);
    const int64_t host_now = h.call(h.open(h.open_service("time:u"), 0), 0).get<int64_t>(0);
    CHECK(host_now > kStart);
}

void test_time_zone() {
    ServicesEnv e;
    const Handle time = e.open_service("time:u");
    const Handle tz = e.open(time, 3);
    const Reply loc = e.call(tz, 0);
    CHECK(std::string(reinterpret_cast<const char*>(loc.data.data())) == "CET");
    CHECK_EQ(e.call(tz, 2).get<int32_t>(0), 1);

    // LoadTimeZoneRule(LocationName, out 0x16 rule of 0x4000 bytes)
    Msg load;
    char name[0x24] = "CET";
    load.arg(name);
    load.b = {{kBufB, 0x4000, 0}};
    CHECK_EQ(e.call(tz, 4, load).result, 0u);
    Msg unknown;
    char berlin[0x24] = "Europe/Berlin";
    unknown.arg(berlin);
    unknown.b = {{kBufA, 0x4000, 0}};
    CHECK_EQ(e.call(tz, 4, unknown).result, make_result(116, 989).raw);
    // UTC under another name (LGP loads "GMT"): offset 0, the name as asked.
    Msg gmt;
    char gmt_name[0x24] = "GMT";
    gmt.arg(gmt_name);
    gmt.b = {{kBufA, 0x4000, 0}};
    CHECK_EQ(e.call(tz, 4, gmt).result, 0u);
    Msg gmt_cal;
    gmt_cal.arg<int64_t>(kStart);
    gmt_cal.a = {{kBufA, 0x4000, 0}};
    const Reply g = e.call(tz, 100, gmt_cal);
    CHECK_EQ(g.result, 0u);
    const auto gmt_info = g.get<AdditionalInfo>(8);
    CHECK_EQ(gmt_info.utc_offset, 0);
    CHECK(std::string(gmt_info.name) == "GMT");
    CHECK_EQ(g.get<CalendarTime>(0).hour, 22);  // kStart is 23:13 CET

    // ToCalendarTime(posix, in 0x15 rule)
    Msg cal;
    cal.arg<int64_t>(kStart);
    cal.a = {{kBufB, 0x4000, 0}};
    check_calendar(e.call(tz, 100, cal), 2023, 11, 14, 23, 13, 20, 2, 317);
    // WithMyRule: leap day (local midnight) and before the epoch.
    Msg leap;
    leap.arg<int64_t>(1'709'164'800 - 3600);
    check_calendar(e.call(tz, 101, leap), 2024, 2, 29, 0, 0, 0, 4, 59);
    Msg old;
    old.arg<int64_t>(-3601);
    check_calendar(e.call(tz, 101, old), 1969, 12, 31, 23, 59, 59, 3, 364);
    // A rule the runtime did not write: configured zone (with a warning).
    std::vector<uint8_t> zeros(0x4000, 0);
    CHECK(e.k.memory().copy_to_user(kBufA, zeros.data(), zeros.size()));
    Msg foreign = cal;
    foreign.a = {{kBufA, 0x4000, 0}};
    check_calendar(e.call(tz, 100, foreign), 2023, 11, 14, 23, 13, 20, 2, 317);

    // ToPosixTime(CalendarTime, in 0x15 rule, out 0xA) -> s32 count
    const CalendarTime local{2023, 11, 14, 23, 13, 20, 0};
    Msg to;
    to.arg(local);
    to.a = {{kBufB, 0x4000, 0}};
    to.c = {{kBufA, 8}};
    const Reply p = e.call(tz, 201, to);
    CHECK_EQ(p.result, 0u);
    CHECK_EQ(p.get<int32_t>(0), 1);
    CHECK_EQ(e.read<int64_t>(kBufA), kStart);
    // WithMyRule, month out of range normalises (month 13 = January next year).
    const CalendarTime overflow{2023, 13, 1, 1, 0, 0, 0};
    Msg to2;
    to2.arg(overflow);
    to2.c = {{kBufA, 8}};
    CHECK_EQ(e.call(tz, 202, to2).get<int32_t>(0), 1);
    CHECK_EQ(e.read<int64_t>(kBufA), 1'704'067'200);  // 2024-01-01 00:00 UTC
}

void test_time_snapshot() {
    ServicesEnv e;
    const Handle time = e.open_service("time:u");
    Msg snap;
    snap.arg<uint8_t>(1);
    snap.c = {{kBufB, 0xD0}};  // type 0x1A
    CHECK_EQ(e.call(time, 400, snap).result, 0u);
    std::array<uint8_t, 0xD0> s{};
    CHECK(e.k.memory().copy_from_user(s.data(), kBufB, s.size()));
    ClockContext user{};
    std::memcpy(&user, s.data(), sizeof(user));
    int64_t user_time = 0;
    std::memcpy(&user_time, s.data() + 0x40, 8);
    int64_t steady = 0;
    std::memcpy(&steady, s.data() + 0x90, 8);
    CHECK_EQ(user_time, user.offset + steady);
    CHECK(user_time >= kStart && user_time < kStart + 60);
    CalendarTime ct{};
    std::memcpy(&ct, s.data() + 0x50, sizeof(ct));
    CHECK_EQ(ct.year, 2023u);
    CHECK(std::string(reinterpret_cast<const char*>(s.data() + 0xA8)) == "CET");
    CHECK_EQ(s[0xCC], 0u);  // automatic correction
    CHECK_EQ(s[0xCD], 1u);  // type

    Msg small = snap;
    small.c = {{kBufB, 0x80}};
    CHECK_EQ(e.call(time, 400, small).result, kr::kInvalidSize.raw);

    // CalculateStandardUserSystemClockDifferenceByUser(in 0x19, in 0x19)
    CHECK(e.k.memory().copy_to_user(kBufA, s.data(), s.size()));
    std::array<uint8_t, 0xD0> moved = s;
    int64_t offset = 0;
    std::memcpy(&offset, moved.data(), 8);
    offset += 60;
    std::memcpy(moved.data(), &offset, 8);
    CHECK(e.k.memory().copy_to_user(kBufA + 0x100, moved.data(), moved.size()));
    Msg diff;
    diff.x = {{kBufA, 0xD0, 0}, {kBufA, 0xD0, 1}};
    CHECK_EQ(e.call(time, 500, diff).get<int64_t>(0), 0);
    diff.x = {{kBufA, 0xD0, 0}, {kBufA + 0x100, 0xD0, 1}};
    CHECK_EQ(e.call(time, 500, diff).get<int64_t>(0), 60'000'000'000ll);
}

// --- csrng, pctl, acc --------------------------------------------------------------------

void test_csrng() {
    std::array<uint8_t, 0x21> first{};
    for (int run = 0; run < 2; ++run) {
        ServicesEnv e;
        const Handle rng = e.open_service("csrng");
        Msg m;
        m.b = {{kBufB, 0x21, 0}};
        CHECK_EQ(e.call(rng, 0, m).result, 0u);
        std::array<uint8_t, 0x21> a{};
        CHECK(e.k.memory().copy_from_user(a.data(), kBufB, a.size()));
        CHECK_EQ(e.call(rng, 0, m).result, 0u);
        std::array<uint8_t, 0x21> b{};
        CHECK(e.k.memory().copy_from_user(b.data(), kBufB, b.size()));
        CHECK(a != b);
        CHECK(std::count(a.begin(), a.end(), uint8_t{0}) < 8);
        // Reproducible across runs with the same RandomEntropy.
        if (run == 0)
            first = a;
        else
            CHECK(first == a);
    }
}

void test_pctl() {
    ServicesEnv e;
    const Handle factory = e.open_service("pctl");
    Msg m;
    m.send_pid = true;
    m.arg<uint64_t>(0);
    const Handle s = e.open(factory, 1, m);
    CHECK_EQ(e.call(s, 1).result, 0u);
    CHECK(e.open(factory, 0, m) != kInvalidHandle);
}

// --- vi (for the NVN-HLE) ------------------------------------------------------------------

void test_vi() {
    ServicesEnv e;
    const Handle root = e.open_service("vi:m");
    Msg policy;
    policy.arg<uint32_t>(0);
    const Handle ads = e.open(root, 2, policy);  // GetDisplayService
    Msg name;
    name.payload.assign(0x40, 0);
    std::memcpy(name.payload.data(), "Default", 7);
    const Reply disp = e.call(ads, 1010, name);  // OpenDisplay
    CHECK_EQ(disp.result, 0u);
    const uint64_t display = disp.get<uint64_t>(0);

    Msg open = name;
    open.send_pid = true;
    open.arg<uint64_t>(1).arg<uint64_t>(0);  // layer id, aruid
    open.b = {{kBufB, 0x100, 0}};
    const Reply layer = e.call(ads, 2020, open);  // OpenLayer
    CHECK_EQ(layer.result, 0u);
    const uint64_t size = layer.get<uint64_t>(0);
    CHECK(size >= 0x10 + 0x18 && size <= 0x100);
    const uint32_t payload_off = e.read<uint32_t>(kBufB + 4);
    CHECK_EQ(e.read<uint32_t>(kBufB + payload_off), 2u);  // flat binder object, type 2
    const int32_t binder = e.read<int32_t>(kBufB + payload_off + 8);
    CHECK(binder != 0);

    Msg scaling;
    scaling.arg<uint32_t>(0);
    CHECK_EQ(e.call(ads, 2102, scaling).get<uint64_t>(0), 3u);  // None -> None
    scaling = {};
    scaling.arg<uint32_t>(2);
    CHECK_EQ(e.call(ads, 2102, scaling).get<uint64_t>(0), 1u);  // ScaleToWindow

    const Handle relay = e.open(ads, 100);  // GetRelayService
    Msg native;
    native.arg<int32_t>(binder).arg<uint32_t>(0x0F);
    const Reply nh = e.call(relay, 2, native);  // GetNativeHandle
    CHECK_EQ(nh.copy.size(), 1u);
    Msg vsync;
    vsync.arg<uint64_t>(display);
    CHECK_EQ(e.call(ads, 5202, vsync).copy.size(), 1u);  // GetDisplayVsyncEvent
    CHECK(e.open(ads, 101) != kInvalidHandle);
    CHECK(e.open(ads, 102) != kInvalidHandle);
}

// hwopus: test_hle_hwopus.cpp.

// --- audout:u (paced by the host tick) -------------------------------------------------------

void test_audout() {
    ServicesEnv e;
    const auto state = register_audout(e.sm, nullptr);
    const Handle mgr = e.open_service("audout:u");
    Msg list;
    list.b = {{kBufB, 0x100, 0}};
    CHECK_EQ(e.call(mgr, 0, list).get<uint32_t>(0), 1u);
    CHECK(std::string(reinterpret_cast<const char*>(e.k.memory().host_ptr(kBufB, 16))) == "DeviceOut");

    Msg open;
    open.send_pid = true;
    open.arg<uint32_t>(0).arg<uint16_t>(2).arg<uint16_t>(0).arg<uint64_t>(0);
    open.a = {{kBufA, 0x100, 0}};
    open.b = {{kBufB, 0x100, 0}};
    open.copy = {kCurrentProcess};
    const Reply r = e.call(mgr, 1, open);
    CHECK_EQ(r.result, 0u);
    CHECK_EQ(r.get<uint32_t>(0), 48000u);
    CHECK_EQ(r.get<uint32_t>(4), 2u);
    CHECK_EQ(r.get<uint32_t>(8), 2u);  // PcmInt16
    {  // 5.1 is accepted as requested (LGP opens it)
        Msg six = open;
        six.payload.clear();
        six.arg<uint32_t>(48000).arg<uint16_t>(6).arg<uint16_t>(0).arg<uint64_t>(0);
        const Reply r6 = e.call(mgr, 1, six);
        CHECK_EQ(r6.result, 0u);
        CHECK_EQ(r6.get<uint32_t>(4), 6u);
    }
    CHECK_EQ(r.move.size(), 1u);
    if (r.move.empty()) return;
    const Handle out = r.move[0];
    const Reply ev = e.call(out, 4);  // RegisterBufferEvent
    CHECK_EQ(ev.copy.size(), 1u);

    // Two 10 ms buffers (480 frames * 4 bytes), tags 0x111 and 0x222.
    auto append = [&](uint64_t tag) {
        const uint64_t buf[5] = {0, kBufA + 0x1000, 0x780, 0x780, 0};
        CHECK(e.k.memory().copy_to_user(kBufA, buf, sizeof(buf)));
        Msg m;
        m.arg<uint64_t>(tag);
        m.a = {{kBufA, sizeof(buf), 0}};
        CHECK_EQ(e.call(out, 3, m).result, 0u);
    };
    append(0x111);
    append(0x222);
    CHECK_EQ(e.call(out, 9).get<uint32_t>(0), 2u);  // queued
    CHECK_EQ(e.call(out, 1).result, 0u);             // Start
    Msg rel;
    rel.b = {{kBufB, 0x40, 0}};
    state->update(e.k, 1'000'000);   // starts playing at 1 ms
    state->update(e.k, 10'000'000);  // 9 ms in: nothing finished
    CHECK(!e.signaled(ev.copy[0]));
    CHECK_EQ(e.call(out, 5, rel).get<uint32_t>(0), 0u);
    state->update(e.k, 11'000'000);  // first buffer done
    CHECK(e.signaled(ev.copy[0]));
    const Reply got = e.call(out, 5, rel);
    CHECK_EQ(got.get<uint32_t>(0), 1u);
    CHECK_EQ(e.read<uint64_t>(kBufB), 0x111u);
    CHECK_EQ(e.read<uint64_t>(kBufB + 8), 0u);  // unused entries are 0 (the SDK reads entry 0 whatever the count)
    CHECK(!e.signaled(ev.copy[0]));  // cleared once nothing is left to fetch
    CHECK_EQ(e.call(out, 5, rel).get<uint32_t>(0), 0u);
    CHECK_EQ(e.read<uint64_t>(kBufB), 0u);  // no stale tag from the previous call
    state->update(e.k, 21'000'000);
    CHECK_EQ(e.call(out, 5, rel).get<uint32_t>(0), 1u);
    CHECK_EQ(e.read<uint64_t>(kBufB), 0x222u);
    CHECK_EQ(e.call(out, 10).get<uint64_t>(0), 960u);  // played samples
    // Hang reports: one line per open IAudioOut (the stereo one and the 5.1 one).
    const std::vector<std::string> report = state->report();
    CHECK_EQ(report.size(), 2u);
    if (report.size() == 2) {
        CHECK(report[0].find("2 channels, started (1 starts), 0 queued, 0 released, 0.0 s played") != std::string::npos);
        CHECK(report[1].find("6 channels, stopped") != std::string::npos);
        CHECK(report[0].find("(open #0)") != std::string::npos && report[1].find("(open #1)") != std::string::npos);
        CHECK(report[0].find("started (1 starts)") != std::string::npos);
        CHECK(report[0].find("2 appends (0 refused), 4 GetReleased calls (last 0.0 s ago) returned 2") !=
              std::string::npos);
    }

    // An out holds at most 32 buffers (queued plus released but not fetched):
    // further appends are refused with ResultBufferRingFull and not queued,
    // as on the console. Fetching released ones makes room again.
    auto append_result = [&](uint64_t tag) {
        const uint64_t buf[5] = {0, kBufA + 0x1000, 0x780, 0x780, 0};
        CHECK(e.k.memory().copy_to_user(kBufA, buf, sizeof(buf)));
        Msg m;
        m.arg<uint64_t>(tag);
        m.a = {{kBufA, sizeof(buf), 0}};
        return e.call(out, 3, m).result;
    };
    for (uint64_t t = 1; t <= 32; ++t) CHECK_EQ(append_result(t), 0u);
    constexpr uint32_t kBufferRingFull = 153 | (8 << 9);
    CHECK_EQ(append_result(33), kBufferRingFull);
    CHECK_EQ(append_result(34), kBufferRingFull);
    CHECK_EQ(e.call(out, 9).get<uint32_t>(0), 32u);
    state->update(e.k, 41'000'000);  // the first starts playing
    state->update(e.k, 61'000'000);  // two finished: released, still held until fetched
    CHECK_EQ(append_result(35), kBufferRingFull);
    CHECK_EQ(e.call(out, 5, rel).get<uint32_t>(0), 2u);
    CHECK_EQ(append_result(36), 0u);
    CHECK_EQ(append_result(37), 0u);
    CHECK_EQ(append_result(38), kBufferRingFull);
    const std::vector<std::string> full = state->report();
    CHECK(!full.empty() && full[0].find("40 appends (4 refused)") != std::string::npos);
}

// Requests in flight (hle::requests_in_flight, for the boot program's
// present-stall report): a request shows up while its handler runs.
void test_requests_in_flight() {
    ServicesEnv e;
    std::vector<std::string> seen;
    struct Probe final : Service {
        explicit Probe(std::vector<std::string>* seen) : Service("lgp:prb") {
            on(7, [seen](Request&, Response&) { *seen = lgp::hle::requests_in_flight(0); });
        }
    };
    e.sm.register_service("lgp:prb", [&seen] { return std::make_shared<Probe>(&seen); });
    const Handle probe = e.open_service("lgp:prb");
    CHECK_EQ(e.call(probe, 7).result, 0u);
    CHECK_EQ(seen.size(), 1u);
    if (!seen.empty())
        CHECK(seen[0].rfind("lgp:prb #7, thread ", 0) == 0 && seen[0].find(", running") != std::string::npos);
    CHECK(lgp::hle::requests_in_flight(0).empty());  // nothing left once answered
}

// audout:u with a sink (P5.2): 5.1 is downmixed to stereo, the volume is
// applied, and frames beyond the sink's capacity are dropped (drift policy).
struct RecordingAudio final : lgp::platform::IAudio {
    struct Sink final : lgp::platform::IAudioSink {
        RecordingAudio* owner;
        lgp::platform::AudioFormat fmt;
        Sink(RecordingAudio* o, const lgp::platform::AudioFormat& f) : owner(o), fmt(f) {}
        lgp::platform::AudioFormat format() const override { return fmt; }
        size_t write(const int16_t* samples, size_t frames) override {
            const size_t n = std::min(frames, owner->capacity - owner->samples.size() / fmt.channels);
            owner->samples.insert(owner->samples.end(), samples, samples + n * fmt.channels);
            return n;
        }
        size_t queued_frames() const override { return owner->samples.size() / fmt.channels; }
        void set_paused(bool paused) override { owner->paused = paused; }
    };
    size_t capacity = 0;
    bool paused = true;
    std::vector<int16_t> samples;
    lgp::platform::AudioFormat format;
    std::unique_ptr<lgp::platform::IAudioSink> open_sink(const lgp::platform::AudioFormat& f) override {
        format = f;
        return std::make_unique<Sink>(this, f);
    }
};

void test_audout_sink() {
    ServicesEnv e;
    RecordingAudio audio;
    audio.capacity = 600;
    register_audout(e.sm, &audio);
    const Handle mgr = e.open_service("audout:u");
    Msg open;
    open.send_pid = true;
    open.arg<uint32_t>(48000).arg<uint16_t>(6).arg<uint16_t>(0).arg<uint64_t>(0);
    open.a = {{kBufA, 0x100, 0}};
    open.b = {{kBufB, 0x100, 0}};
    open.copy = {kCurrentProcess};
    const Reply r = e.call(mgr, 1, open);
    CHECK_EQ(r.move.size(), 1u);
    if (r.move.empty()) return;
    const Handle out = r.move[0];
    CHECK_EQ(audio.format.channels, 2u);  // the sink always gets stereo
    CHECK_EQ(audio.format.sample_rate, 48000u);
    CHECK_EQ(e.call(out, 1).result, 0u);  // Start
    CHECK(!audio.paused);
    Msg vol;
    vol.arg<float>(0.5f);
    CHECK_EQ(e.call(out, 12, vol).result, 0u);  // SetAudioOutVolume

    // 480 frames of 5.1: FL 1000, FR -1000, C 400, LFE 9999, RL 200, RR -200.
    std::vector<int16_t> pcm(480 * 6);
    for (size_t i = 0; i < 480; ++i) {
        const int16_t f[6] = {1000, -1000, 400, 9999, 200, -200};
        std::memcpy(&pcm[i * 6], f, sizeof(f));
    }
    CHECK(e.k.memory().copy_to_user(kBufA + 0x1000, pcm.data(), pcm.size() * 2));
    auto append = [&](uint64_t tag) {
        const uint64_t buf[5] = {0, kBufA + 0x1000, pcm.size() * 2, pcm.size() * 2, 0};
        CHECK(e.k.memory().copy_to_user(kBufA, buf, sizeof(buf)));
        Msg m;
        m.arg<uint64_t>(tag);
        m.a = {{kBufA, sizeof(buf), 0}};
        CHECK_EQ(e.call(out, 3, m).result, 0u);
    };
    append(1);
    CHECK_EQ(audio.samples.size(), 480u * 2);
    // (FL + C/2 + RL/2) * 0.5 = (1000 + 200 + 100) / 2 = 650; LFE is dropped.
    CHECK_EQ(audio.samples[0], 650);
    CHECK_EQ(audio.samples[1], -450);  // (FR + C/2 + RR/2) * 0.5
    append(2);  // only 120 more frames fit: the rest is dropped
    CHECK_EQ(audio.samples.size(), 600u * 2);
    CHECK_EQ(e.call(out, 9).get<uint32_t>(0), 2u);  // both still pace the guest
    CHECK_EQ(e.call(out, 2).result, 0u);            // Stop
    CHECK(audio.paused);
}

void test_acc() {
    ServicesEnv e;
    const Handle acc = e.open_service("acc:u0");
    Msg init;
    init.send_pid = true;
    init.arg<uint64_t>(0);
    CHECK_EQ(e.call(acc, 140, init).result, 0u);
    CHECK_EQ(e.call(acc, 100, init).result, 0u);
    CHECK_EQ(e.call(acc, 0).get<int32_t>(0), 1);

    const std::array<uint64_t, 2> uid{0x1111222233334444ull, 0x5555666677778888ull};
    const std::array<uint64_t, 2> other{1, 2};
    Msg exists;
    exists.arg(uid);
    CHECK_EQ(e.call(acc, 1, exists).get<uint8_t>(0), 1u);
    Msg missing;
    missing.arg(other);
    CHECK_EQ(e.call(acc, 1, missing).get<uint8_t>(0), 0u);

    std::vector<uint8_t> junk(0x80, 0xEE);
    CHECK(e.k.memory().copy_to_user(kBufB, junk.data(), junk.size()));
    Msg list;
    list.c = {{kBufB, 0x80}};
    CHECK_EQ(e.call(acc, 2, list).result, 0u);
    CHECK_EQ(e.read<uint64_t>(kBufB), uid[0]);
    CHECK_EQ(e.read<uint64_t>(kBufB + 8), uid[1]);
    CHECK_EQ(e.read<uint64_t>(kBufB + 0x10), 0u);
    CHECK_EQ(e.read<uint64_t>(kBufB + 0x78), 0u);
    CHECK_EQ(e.call(acc, 3, list).result, 0u);
    CHECK_EQ(e.call(acc, 4).get<uint64_t>(8), uid[1]);

    const Handle profile = e.open(acc, 5, exists);
    const Reply base = e.call(profile, 1);
    CHECK_EQ(base.get<uint64_t>(0), uid[0]);
    CHECK(std::string(reinterpret_cast<const char*>(base.data.data() + 0x18)) == "Paul");
    Msg get;
    get.c = {{kBufA, 0x80}};
    CHECK_EQ(e.call(profile, 0, get).get<uint64_t>(8), uid[1]);
    CHECK_EQ(e.call(acc, 5, missing).result, acc_result::kUserNotExist.raw);

    CHECK_EQ(e.call(acc, 101, missing).result, acc_result::kUserNotExist.raw);

    // No Network Service Account: everything that needs one fails.
    const Handle baas = e.open(acc, 101, exists);
    const uint32_t nsa = acc_result::kNetworkServiceAccountRegistrationRequired.raw;
    CHECK_EQ(e.call(baas, 0).result, nsa);
    CHECK_EQ(e.call(baas, 1).result, nsa);
    const Handle async = e.open(baas, 2);
    const Reply ev = e.call(async, 0);
    CHECK_EQ(ev.copy.size(), 1u);
    if (!ev.copy.empty())
        CHECK(e.signaled(ev.copy[0]));
    CHECK_EQ(e.call(async, 2).get<uint8_t>(0), 1u);
    CHECK_EQ(e.call(async, 3).result, nsa);
}

void test_quiet_services() {
    ServicesEnv e;
    ApplicationInfo app;
    app.program_id = 0x010003F003A34000ull;
    app.version = 0x20000;
    register_quiet_services(e.sm, app);  // replaces the default registration

    // prepo: SaveReportWithUserOld(uid, u64, pid, X event id, A report) succeeds.
    const Handle prepo = e.open_service("prepo:u");
    Msg report;
    report.send_pid = true;
    report.arg<uint64_t>(1).arg<uint64_t>(2).arg<uint64_t>(0);
    report.x = {{kBufA, 0x20, 0}};
    report.a = {{kBufA + 0x100, 0x80, 0}};
    CHECK_EQ(e.call(prepo, 10101, report).result, 0u);
    CHECK_EQ(e.call(prepo, 10100, report).result, 0u);

    // arp: the instance id is the pid; the launch property names the program.
    const Handle arp = e.open_service("arp:r");
    Msg by_pid;
    by_pid.arg<uint64_t>(small_process().process_id);
    const uint64_t instance = e.call(arp, 3, by_pid).get<uint64_t>(0);
    CHECK_EQ(instance, small_process().process_id);
    Msg by_instance;
    by_instance.arg<uint64_t>(instance);
    const Reply prop = e.call(arp, 0, by_instance);
    CHECK_EQ(prop.result, 0u);
    CHECK_EQ(prop.get<uint64_t>(0), app.program_id);
    CHECK_EQ(prop.get<uint32_t>(8), 0x20000u);
    CHECK_EQ(prop.get<uint8_t>(12), 5u);

    // btm:u: five BLE events, acquired (true) and never signaled.
    const Handle btm = e.open_service("btm:u");
    const Handle core = e.open(btm, 0);
    for (const uint32_t cmd : {0u, 17u, 21u, 26u, 33u}) {
        const Reply r = e.call(core, cmd);
        CHECK_EQ(r.result, 0u);
        CHECK_EQ(r.get<uint8_t>(0), 1u);
        CHECK_EQ(r.copy.size(), 1u);
        if (!r.copy.empty())
            CHECK(!e.signaled(r.copy[0]));
    }
    // Scan for a smart device (Pokémon GO): starts, finds nothing, stops.
    Msg filter;
    filter.arg<uint16_t>(2);
    const Reply fp2 = e.call(core, 2, filter);
    CHECK_EQ(fp2.result, 0u);
    CHECK_EQ(fp2.get<uint32_t>(0), 0x10u);  // 128-bit UUID
    filter = Msg{};
    filter.arg<uint16_t>(1);
    CHECK_EQ(e.call(core, 1, filter).result, 0u);
    Msg scan;
    scan.send_pid = true;
    scan.arg<uint32_t>(0x10);
    for (int i = 0; i < 5; ++i)
        scan.arg<uint32_t>(0);  // uuid, pad
    scan.arg<uint64_t>(0);      // ARUID
    CHECK_EQ(e.call(core, 8, scan).result, 0u);
    Msg results;
    results.send_pid = true;
    results.arg<uint64_t>(0);
    results.b = {{kBufB, 0x148 * 10, 0}};
    const Reply sr = e.call(core, 10, results);
    CHECK_EQ(sr.result, 0u);
    CHECK_EQ(sr.get<uint8_t>(0), 0u);
    CHECK_EQ(e.call(core, 9).result, 0u);
    Msg conns;
    conns.send_pid = true;
    conns.arg<uint64_t>(0);
    conns.c = {{kBufB, 0x10 * 4}};
    const Reply cs = e.call(core, 20, conns);
    CHECK_EQ(cs.result, 0u);
    CHECK_EQ(cs.get<uint8_t>(0), 0u);
    Msg path;  // BleDataPath + ARUID
    path.send_pid = true;
    path.arg<uint32_t>(1).arg<uint32_t>(0x10);
    for (int i = 0; i < 4; ++i)
        path.arg<uint32_t>(0);
    path.arg<uint64_t>(0);
    CHECK_EQ(e.call(core, 36, path).result, 0u);
    CHECK_EQ(e.call(core, 37, path).result, 0u);
    {
        // Without a scan result there is nothing to connect to: BleConnect stops.
        ServicesEnv e2;
        const Handle core2 = e2.open(e2.open_service("btm:u"), 0);
        Msg connect;
        connect.command = 18;
        connect.send_pid = true;
        connect.arg<uint64_t>(0).arg<uint64_t>(0);
        e2.send(core2, connect, SvcOutcome::Unimplemented);
    }

    const Handle bt = e.open_service("bt");
    Msg ble;
    ble.send_pid = true;
    ble.arg<uint64_t>(0);
    const Reply br = e.call(bt, 9, ble);
    CHECK_EQ(br.result, 0u);
    CHECK_EQ(br.copy.size(), 1u);

    // lm: OpenLogger, Log, SetDestination.
    const Handle lm = e.open_service("lm");
    Msg open;
    open.send_pid = true;
    open.arg<uint64_t>(0);
    const Handle logger = e.open(lm, 0, open);
    Msg log;
    log.a = {{kBufA, 0x40, 0}};
    CHECK_EQ(e.call(logger, 0, log).result, 0u);
    Msg dest;
    dest.arg<uint32_t>(0xFFFF);
    CHECK_EQ(e.call(logger, 1, dest).result, 0u);

    // audren:u: only device questions (names, volume, channel count).
    const Handle audren = e.open_service("audren:u");
    Msg rev;
    rev.arg<uint32_t>(0x52455638).arg<uint64_t>(0);
    const Handle device = e.open(audren, 4, rev);
    Msg names;
    names.b = {{kBufB, 0x100 * 4, 0}};
    CHECK_EQ(e.call(device, 6, names).get<uint32_t>(0), 3u);
    CHECK_EQ(e.read<char>(kBufB + 0x100), 'A');  // "AudioBuiltInSpeakerOutput"
    Msg one;
    one.b = {{kBufB, 0x100, 0}};
    CHECK_EQ(e.call(device, 0, one).get<uint32_t>(0), 1u);
    Msg vol;
    vol.arg<float>(0.5f);
    vol.a = {{kBufA, 0x100, 0}};
    CHECK_EQ(e.call(device, 7, vol).result, 0u);
    CHECK(e.call(device, 8).get<float>(0) == 0.5f);
    CHECK_EQ(e.call(device, 5).get<uint32_t>(0), 2u);
}

// --- swkbd (library applet) ---------------------------------------------------------

Handle make_storage(ServicesEnv& e, Handle creator, const std::vector<uint8_t>& data) {
    Msg create;
    create.arg<int64_t>(static_cast<int64_t>(data.size()));
    const Handle storage = e.open(creator, 10, create);
    const Handle acc = e.open(storage, 0);
    CHECK(e.k.memory().copy_to_user(kBufA, data.data(), data.size()));
    Msg write;
    write.arg<int64_t>(0);
    write.a = {{kBufA, data.size(), 0}};
    CHECK_EQ(e.call(acc, 10, write).result, 0u);
    CHECK_EQ(e.svc(kSvcCloseHandle, {acc})[0], kSuccess.raw);
    return storage;
}

std::vector<uint8_t> storage_bytes(ServicesEnv& e, Handle storage) {
    const Handle acc = e.open(storage, 0);
    const auto size = static_cast<uint64_t>(e.call(acc, 0).get<int64_t>(0));
    Msg read;
    read.arg<int64_t>(0);
    read.b = {{kBufB, size, 0}};
    CHECK_EQ(e.call(acc, 11, read).result, 0u);
    std::vector<uint8_t> out(size);
    CHECK(e.k.memory().copy_from_user(out.data(), kBufB, size));
    return out;
}

void put_u16(std::vector<uint8_t>& v, size_t at, const std::u16string& s) {
    std::memcpy(v.data() + at, s.data(), 2 * s.size());
}
std::u16string get_u16(const std::vector<uint8_t>& v, size_t at) {
    std::u16string s;
    for (size_t i = at; i + 2 <= v.size(); i += 2) {
        char16_t c = 0;
        std::memcpy(&c, v.data() + i, 2);
        if (c == 0)
            break;
        s.push_back(c);
    }
    return s;
}

struct Keyboard {
    Handle accessor = kInvalidHandle;
    Handle state_event = kInvalidHandle;
    Handle interactive_event = kInvalidHandle;
};

// Creates the swkbd applet and pushes common args, config and work buffer
// as the SDK does (CreateStorage, CreateTransferMemoryStorage, PushInData).
Keyboard start_keyboard(ServicesEnv& e, Handle creator, bool text_check, const std::u16string& initial,
                        uint32_t max_len) {
    Keyboard kb;
    Msg create;
    create.arg<uint32_t>(0x11).arg<uint32_t>(0);
    kb.accessor = e.open(creator, 0, create);
    const Reply ev = e.call(kb.accessor, 0);
    CHECK_EQ(ev.copy.size(), 1u);
    kb.state_event = ev.copy.empty() ? kInvalidHandle : ev.copy[0];
    const Reply iev = e.call(kb.accessor, 106);
    kb.interactive_event = iev.copy.empty() ? kInvalidHandle : iev.copy[0];

    std::vector<uint8_t> common(0x20, 0);
    common[0] = 1;
    common[4] = 0x20;
    std::vector<uint8_t> config(0x4C8, 0);
    put_u16(config, 0x24, u"Name?");
    std::memcpy(config.data() + 0x3AC, &max_len, 4);
    const uint32_t init_off = 0x14;
    const auto init_units = static_cast<uint32_t>(initial.size());
    std::memcpy(config.data() + 0x3C0, &init_off, 4);
    std::memcpy(config.data() + 0x3C4, &init_units, 4);
    config[0x3D0] = text_check ? 1 : 0;

    // Each keyboard locks its own page (transfer memory, permission None).
    static int next_page = 0;
    const uint64_t work = kBufA + 0x2000 + 0x1000 * static_cast<uint64_t>(next_page++ % 6);
    std::vector<uint8_t> wb(0x1000, 0);
    put_u16(wb, init_off, initial);
    CHECK(e.k.memory().copy_to_user(work, wb.data(), wb.size()));
    const auto x = e.svc(kSvcCreateTransferMemory, {0, work, 0x1000, kPermNone});
    Msg ctm;
    ctm.copy = {static_cast<Handle>(x[1])};
    ctm.arg<uint8_t>(1).arg<int64_t>(0x1000);
    const Handle work_storage = e.open(creator, 11, ctm);

    for (const Handle st : {make_storage(e, creator, common), make_storage(e, creator, config), work_storage}) {
        Msg push;
        push.copy = {st};
        CHECK_EQ(e.call(kb.accessor, 100, push).result, 0u);
    }
    CHECK(!e.signaled(kb.state_event));
    CHECK_EQ(e.call(kb.accessor, 10).result, 0u);  // Start
    return kb;
}

void test_swkbd() {
    ServicesEnv e;
    const Handle creator = e.open(open_proxy(e), 11);

    // Without a handler and without text check: the initial text comes back.
    Keyboard kb = start_keyboard(e, creator, false, u"Red", 10);
    CHECK(e.signaled(kb.state_event));
    CHECK_EQ(e.call(kb.accessor, 1).get<uint8_t>(0), 1u);  // IsCompleted
    CHECK_EQ(e.call(kb.accessor, 30).result, 0u);          // GetResult
    std::vector<uint8_t> out = storage_bytes(e, e.open(kb.accessor, 101));
    CHECK_EQ(out.size(), 0x7D8u);
    CHECK_EQ(out[0], 0u);
    CHECK(get_u16(out, 4) == u"Red");
    CHECK_EQ(e.call(kb.accessor, 101).result, am_result::kNoDataInChannel.raw);

    // Empty initial text: the nickname ("Paul").
    kb = start_keyboard(e, creator, false, u"", 10);
    CHECK(get_u16(storage_bytes(e, e.open(kb.accessor, 101)), 4) == u"Paul");

    // A host handler with text check: the game rejects the first answer.
    std::vector<KeyboardRequest> seen;
    e.base.applet->set_keyboard_handler([&seen](const KeyboardRequest& r) {
        seen.push_back(r);
        return KeyboardResult{true, r.rejected == 0 ? u"BadNameTooLong" : u"Gary"};
    });
    kb = start_keyboard(e, creator, true, u"Blue", 8);
    CHECK(!e.signaled(kb.state_event));
    CHECK(e.signaled(kb.interactive_event));
    std::vector<uint8_t> check = storage_bytes(e, e.open(kb.accessor, 104));
    uint64_t size = 0;
    std::memcpy(&size, check.data(), 8);
    CHECK_EQ(size, 2u * 9);  // clipped to max_len 8, plus NUL
    CHECK(get_u16(check, 8) == u"BadNameT");
    CHECK(!e.signaled(kb.interactive_event));

    std::vector<uint8_t> reply(4 + 0x7D4, 0);
    reply[0] = 1;  // Bad
    put_u16(reply, 4, u"No!");
    Msg push_reply;
    push_reply.copy = {make_storage(e, creator, reply)};
    CHECK_EQ(e.call(kb.accessor, 103, push_reply).result, 0u);
    CHECK(e.signaled(kb.interactive_event));  // asked again
    check = storage_bytes(e, e.open(kb.accessor, 104));
    CHECK(get_u16(check, 8) == u"Gary");
    reply[0] = 0;  // OK
    push_reply.copy = {make_storage(e, creator, reply)};
    CHECK_EQ(e.call(kb.accessor, 103, push_reply).result, 0u);
    CHECK(e.signaled(kb.state_event));
    out = storage_bytes(e, e.open(kb.accessor, 101));
    CHECK_EQ(out[0], 0u);
    CHECK(get_u16(out, 4) == u"Gary");

    CHECK_EQ(seen.size(), 2u);
    if (seen.size() == 2) {
        CHECK(seen[0].header == u"Name?");
        CHECK(seen[0].initial == u"Blue");
        CHECK_EQ(seen[0].max_length, 8u);
        CHECK_EQ(seen[1].rejected, 1u);
        CHECK(seen[1].check_message == u"No!");
    }

    // Cancel: CloseResult 1.
    e.base.applet->set_keyboard_handler([](const KeyboardRequest&) { return KeyboardResult{false, {}}; });
    kb = start_keyboard(e, creator, false, u"x", 0);
    CHECK_EQ(storage_bytes(e, e.open(kb.accessor, 101))[0], 1u);

    // Any other library applet stops the guest.
    Msg other;
    other.command = 0;
    other.arg<uint32_t>(0x12).arg<uint32_t>(0);  // web
    e.send(creator, other, SvcOutcome::Unimplemented);
}

// --- error and playerSelect (library applets) -----------------------------------------

std::vector<uint8_t> common_args() {
    std::vector<uint8_t> common(0x20, 0);
    common[0] = 1;
    common[4] = 0x20;
    return common;
}

// Creates applet `id`, pushes CommonArguments and `arg`, starts it and
// returns the accessor once it completed.
Handle run_applet(ServicesEnv& e, Handle creator, uint32_t id, const std::vector<uint8_t>& arg) {
    Msg create;
    create.arg<uint32_t>(id).arg<uint32_t>(0);
    const Handle accessor = e.open(creator, 0, create);
    const Reply ev = e.call(accessor, 0);
    CHECK_EQ(ev.copy.size(), 1u);
    for (const Handle st : {make_storage(e, creator, common_args()), make_storage(e, creator, arg)}) {
        Msg push;
        push.copy = {st};
        CHECK_EQ(e.call(accessor, 100, push).result, 0u);
    }
    CHECK_EQ(e.call(accessor, 10).result, 0u);  // Start
    if (!ev.copy.empty())
        CHECK(e.signaled(ev.copy[0]));
    CHECK_EQ(e.call(accessor, 1).get<uint8_t>(0), 1u);  // IsCompleted
    CHECK_EQ(e.call(accessor, 30).result, 0u);          // GetResult
    return accessor;
}

void test_applets() {
    ServicesEnv e;
    const Handle creator = e.open(open_proxy(e), 11);
    std::vector<ErrorMessage> shown;
    e.base.applet->set_message_handler([&shown](const ErrorMessage& m) { shown.push_back(m); });

    // error: ShowError(Result) as SystemData, closes at once with "jump nowhere".
    std::vector<uint8_t> system_data(0x14, 0);
    const uint32_t raw = nifm_result::kNetworkNotAvailable.raw;
    std::memcpy(system_data.data() + 0x10, &raw, 4);
    Handle acc = run_applet(e, creator, 0x0E, system_data);
    std::vector<uint8_t> out = storage_bytes(e, e.open(acc, 101));
    CHECK_EQ(out.size(), 2u);
    CHECK_EQ(out.size() == 2 ? out[1] : 0xFF, 0u);
    // ApplicationError with a message.
    std::vector<uint8_t> app_error(0x1014, 0);
    app_error[0] = 2;
    std::memcpy(app_error.data() + 0x14, "Connection failed", 17);
    acc = run_applet(e, creator, 0x0E, app_error);
    CHECK_EQ(storage_bytes(e, e.open(acc, 101)).size(), 2u);
    // Both reached the host before the applet closed.
    CHECK_EQ(shown.size(), 2u);
    if (shown.size() == 2) {
        CHECK(shown[0].code == "error 2110-1100");
        CHECK(shown[0].text.empty());
        CHECK(shown[1].code == "application error 0");
        CHECK(shown[1].text == "Connection failed");
    }

    // netConnect (nifm's failed request): s32 ret != 0, then 2110-1100.
    acc = run_applet(e, creator, 0x0F, std::vector<uint8_t>(4, 0));
    out = storage_bytes(e, e.open(acc, 101));
    CHECK_EQ(out.size(), 8u);
    if (out.size() == 8) {
        int32_t ret = 0;
        uint32_t rc = 0;
        std::memcpy(&ret, out.data(), 4);
        std::memcpy(&rc, out.data() + 4, 4);
        CHECK(ret != 0);
        CHECK_EQ(rc, nifm_result::kNetworkNotAvailable.raw);
    }

    // playerSelect: UserSelector returns the one user.
    std::vector<uint8_t> ui(0xA0, 0);
    ui[0x96] = 1;
    acc = run_applet(e, creator, 0x10, ui);
    out = storage_bytes(e, e.open(acc, 101));
    CHECK_EQ(out.size(), 0x18u);
    uint64_t result = ~0ull;
    std::array<uint64_t, 2> uid{};
    std::memcpy(&result, out.data(), 8);
    std::memcpy(uid.data(), out.data() + 8, 16);
    CHECK_EQ(result, 0u);
    CHECK_EQ(uid[0], test_config().user_id[0]);
    CHECK_EQ(uid[1], test_config().user_id[1]);
    // EnsureNetworkServiceAccountAvailable (UiMode 2): cancelled, no uid.
    ui[0] = 2;
    acc = run_applet(e, creator, 0x10, ui);
    out = storage_bytes(e, e.open(acc, 101));
    std::memcpy(&result, out.data(), 8);
    std::memcpy(uid.data(), out.data() + 8, 16);
    CHECK_EQ(result, uint64_t{acc_result::kCancelledByUser.raw});
    CHECK_EQ(uid[0], 0u);
    // UserSelector with a Nintendo Account required: none linked, cancelled.
    ui[0] = 0;
    ui[0x90] = 1;
    acc = run_applet(e, creator, 0x10, ui);
    out = storage_bytes(e, e.open(acc, 101));
    std::memcpy(&result, out.data(), 8);
    CHECK_EQ(result, uint64_t{acc_result::kCancelledByUser.raw});
    // UserSelector with the only user in invalid_uid_list: cancelled.
    ui[0x90] = 0;
    std::memcpy(ui.data() + 8 + 0x10, test_config().user_id.data(), 0x10);
    acc = run_applet(e, creator, 0x10, ui);
    out = storage_bytes(e, e.open(acc, 101));
    std::memcpy(&result, out.data(), 8);
    std::memcpy(uid.data(), out.data() + 8, 16);
    CHECK_EQ(result, uint64_t{acc_result::kCancelledByUser.raw});
    CHECK_EQ(uid[0], 0u);
    // Another user excluded: ours is still picked.
    std::fill(ui.begin() + 8 + 0x10, ui.begin() + 8 + 0x20, uint8_t{0x5A});
    acc = run_applet(e, creator, 0x10, ui);
    out = storage_bytes(e, e.open(acc, 101));
    std::memcpy(&result, out.data(), 8);
    CHECK_EQ(result, 0u);
}

// --- nifm:u, ldn:u, friend:u (offline console) -------------------------------------------

void test_nifm() {
    ServicesEnv e;
    const Handle nifm = e.open_service("nifm:u");
    Msg create;
    create.send_pid = true;
    create.arg<uint64_t>(0);
    const Handle general = e.open(nifm, 5, create);
    CHECK(e.open(nifm, 4) != kInvalidHandle);  // CreateGeneralServiceOld

    Msg client;
    client.c = {{kBufB, 4}};
    CHECK_EQ(e.call(general, 1, client).result, 0u);
    const uint32_t first = e.read<uint32_t>(kBufB);
    CHECK_EQ(e.call(general, 1, client).result, 0u);
    CHECK_EQ(e.read<uint32_t>(kBufB), first + 1);

    CHECK_EQ(e.call(general, 17).get<uint8_t>(0), 1u);  // wireless on
    CHECK_EQ(e.call(general, 20).get<uint8_t>(0), 0u);  // no ethernet
    CHECK_EQ(e.call(general, 12).result, nifm_result::kNetworkNotAvailable.raw);
    CHECK_EQ(e.call(general, 15).result, nifm_result::kNetworkNotAvailable.raw);
    CHECK_EQ(e.call(general, 18).result, nifm_result::kNetworkNotAvailable.raw);
    Msg accepted;
    accepted.x = {{kBufA, 4, 0}};
    CHECK_EQ(e.call(general, 21, accepted).get<uint8_t>(0), 0u);

    Msg preset;
    preset.arg<int32_t>(2);
    const Handle request = e.open(general, 4, preset);
    const Reply events = e.call(request, 2);
    CHECK_EQ(events.result, 0u);
    CHECK_EQ(events.copy.size(), 2u);
    CHECK_EQ(e.call(request, 0).get<uint32_t>(0), 1u);  // Free
    CHECK_EQ(e.call(request, 1).result, 0u);            // not submitted yet
    Msg flag;
    flag.arg<uint8_t>(1);
    CHECK_EQ(e.call(request, 10, flag).result, 0u);  // SetRejectable
    CHECK_EQ(e.call(request, 12, flag).result, 0u);  // SetPersistent
    if (!events.copy.empty())
        CHECK(!e.signaled(events.copy[0]));
    CHECK_EQ(e.call(request, 4).result, 0u);  // Submit
    if (!events.copy.empty())
        CHECK(e.signaled(events.copy[0]));
    CHECK_EQ(e.call(request, 0).get<uint32_t>(0), 1u);
    CHECK_EQ(e.call(request, 1).result, nifm_result::kNetworkNotAvailable.raw);

    // GetAppletInfo: the netConnect applet, no in data.
    Msg info;
    info.arg<uint32_t>(0);
    info.b = {{kBufB, 0x1000, 0}};
    const Reply ai = e.call(request, 21, info);
    CHECK_EQ(ai.result, 0u);
    CHECK_EQ(ai.get<uint32_t>(0), 0x0Fu);
    CHECK_EQ(ai.get<uint32_t>(4), 0u);
    CHECK_EQ(ai.get<uint32_t>(8), 4u);
    CHECK_EQ(e.read<int32_t>(kBufB), -1);
}

// LGP_NIFM_ONLINE: the request is accepted, the console reports Wi-Fi.
void test_nifm_online() {
    lgp::hle::set_nifm_online(true);
    ServicesEnv e;
    const Handle nifm = e.open_service("nifm:u");
    const Handle general = e.open(nifm, 4);
    const Reply ip = e.call(general, 12);
    CHECK_EQ(ip.result, 0u);
    CHECK_EQ(ip.get<uint32_t>(0), 0x0200A8C0u);
    const Reply status = e.call(general, 18);
    CHECK_EQ(status.result, 0u);
    CHECK_EQ(status.get<uint8_t>(2), 4u);  // connected
    Msg preset;
    preset.arg<int32_t>(2);
    const Handle request = e.open(general, 4, preset);
    CHECK_EQ(e.call(request, 4).result, 0u);  // Submit
    CHECK_EQ(e.call(request, 0).get<uint32_t>(0), 3u);  // Accepted
    CHECK_EQ(e.call(request, 1).result, 0u);
    CHECK_EQ(e.call(request, 3).result, 0u);            // Cancel
    CHECK_EQ(e.call(request, 0).get<uint32_t>(0), 1u);  // Free again
    CHECK_EQ(e.call(request, 4).result, 0u);            // Submit once more
    CHECK_EQ(e.call(request, 0).get<uint32_t>(0), 3u);
    lgp::hle::set_nifm_online(false);
}

constexpr size_t kLdnNetworkInfoSize = 0x480;

std::vector<uint8_t> ldn_create_payload(int64_t lcid, int8_t node_count_max, const char* user) {
    std::vector<uint8_t> p(0x98, 0);
    const uint16_t security_mode = 1, passphrase_size = 0x10;
    std::memcpy(p.data(), &security_mode, 2);
    std::memcpy(p.data() + 2, &passphrase_size, 2);
    std::memcpy(p.data() + 0x44, user, std::strlen(user));  // UserConfig
    std::memcpy(p.data() + 0x78, &lcid, 8);                  // NetworkConfig.intent_id
    const uint16_t scene = 7;
    std::memcpy(p.data() + 0x78 + 0xA, &scene, 2);
    p[0x78 + 0x12] = static_cast<uint8_t>(node_count_max);
    const int16_t version = 3;
    std::memcpy(p.data() + 0x78 + 0x14, &version, 2);
    return p;
}

void test_ldn() {
    ServicesEnv e;
    const Handle creator = e.open_service("ldn:u");
    const Handle ldn = e.open(creator, 0);
    const auto state = [&] { return e.call(ldn, 0).get<uint32_t>(0); };
    const Reply ev = e.call(ldn, 100);
    CHECK_EQ(ev.copy.size(), 1u);
    const Handle changed = ev.copy.empty() ? kInvalidHandle : ev.copy[0];

    CHECK_EQ(state(), 0u);
    CHECK_EQ(e.call(ldn, 200).result, ldn_result::kInvalidState.raw);  // OpenAccessPoint before Initialize
    Msg init;
    init.send_pid = true;
    init.arg<uint64_t>(0);
    CHECK_EQ(e.call(ldn, 400, init).result, 0u);
    CHECK_EQ(state(), 1u);
    CHECK(e.signaled(changed));
    CHECK_EQ(e.svc(kSvcClearEvent, {changed})[0], kSuccess.raw);

    // Station: the scan finds nobody, connecting fails.
    CHECK_EQ(e.call(ldn, 300).result, 0u);
    CHECK_EQ(state(), 4u);
    Msg scan;
    scan.arg<int16_t>(0);
    scan.payload.resize(8 + 0x60, 0);  // pad + ScanFilter
    scan.b = {{kBufB, kLdnNetworkInfoSize * 4, 0}};
    const Reply sr = e.call(ldn, 102, scan);
    CHECK_EQ(sr.result, 0u);
    CHECK_EQ(sr.get<int16_t>(0), 0);
    Msg connect;
    connect.payload.resize(0x7C, 0);
    connect.x = {{kBufA, kLdnNetworkInfoSize, 0}};
    CHECK_EQ(e.call(ldn, 302, connect).result, ldn_result::kNetworkNotFound.raw);
    CHECK_EQ(e.call(ldn, 1, Msg{}).result, ldn_result::kInvalidState.raw);
    CHECK_EQ(e.call(ldn, 301).result, 0u);
    CHECK_EQ(state(), 1u);

    // Access point: the network exists, with us as the only node.
    CHECK_EQ(e.call(ldn, 200).result, 0u);
    CHECK_EQ(state(), 2u);
    const char adv[] = "LGP-ADVERTISE";
    CHECK(e.k.memory().copy_to_user(kBufA, adv, sizeof(adv)));
    Msg advertise;
    advertise.a = {{kBufA, sizeof(adv), 0}};
    CHECK_EQ(e.call(ldn, 206, advertise).result, 0u);
    Msg bad;
    bad.payload = ldn_create_payload(0x0100'0000'0000'1234, 9, "Paul");
    CHECK_EQ(e.call(ldn, 202, bad).result, ldn_result::kInvalidInput.raw);
    Msg create;
    create.payload = ldn_create_payload(0x0100'0000'0000'1234, 4, "Paul");
    CHECK_EQ(e.call(ldn, 202, create).result, 0u);
    CHECK_EQ(state(), 3u);
    Msg get;
    get.c = {{kBufB, kLdnNetworkInfoSize}};
    CHECK_EQ(e.call(ldn, 1, get).result, 0u);
    CHECK_EQ(e.read<int64_t>(kBufB), int64_t{0x0100'0000'0000'1234});
    CHECK_EQ(e.read<uint16_t>(kBufB + 0xA), 7u);       // scene id
    CHECK_EQ(e.read<uint8_t>(kBufB + 0x26), 0x20u);    // ssid length
    CHECK_EQ(e.read<int8_t>(kBufB + 0x66), 4);         // node_count_max
    CHECK_EQ(e.read<uint8_t>(kBufB + 0x67), 1u);       // node_count
    CHECK_EQ(e.read<uint8_t>(kBufB + 0x68 + 0xB), 1u);  // nodes[0].is_connected
    char name[0x21] = {};
    CHECK(e.k.memory().copy_from_user(name, kBufB + 0x68 + 0xC, sizeof(name)));
    CHECK(std::string(name) == "Paul");
    CHECK_EQ(e.read<int16_t>(kBufB + 0x68 + 0x2E), 3);  // local communication version
    CHECK_EQ(e.read<uint16_t>(kBufB + 0x26A), uint16_t{sizeof(adv)});
    CHECK_EQ(e.read<char>(kBufB + 0x26C), 'L');
    const Reply ip = e.call(ldn, 2);
    CHECK_EQ(ip.result, 0u);
    CHECK_EQ(ip.get<uint32_t>(0) >> 16, 0xA9FEu);  // 169.254.x.x
    CHECK_EQ(e.call(ldn, 4).result, 0u);
    CHECK_EQ(e.call(ldn, 5).get<int8_t>(0x12), 4);

    CHECK_EQ(e.call(ldn, 204).result, 0u);  // DestroyNetwork
    CHECK_EQ(state(), 2u);
    CHECK_EQ(e.call(ldn, 3).get<int16_t>(0), 3);  // DestroyedByUser
    CHECK_EQ(e.call(ldn, 201).result, 0u);
    CHECK_EQ(e.call(ldn, 401).result, 0u);  // Finalize
    CHECK_EQ(state(), 0u);

    // -1 = the program's own LocalCommunicationId (the program id).
    CHECK_EQ(e.call(ldn, 400, init).result, 0u);
    CHECK_EQ(e.call(ldn, 200).result, 0u);
    create.payload = ldn_create_payload(-1, 2, "Paul");
    CHECK_EQ(e.call(ldn, 202, create).result, 0u);
    CHECK_EQ(e.call(ldn, 1, get).result, 0u);
    CHECK_EQ(e.read<uint64_t>(kBufB), small_process().program_id);
}

void test_sockets() {
    ServicesEnv e;
    const Handle bsd = e.open_service("bsd:u");
    Msg reg;
    reg.send_pid = true;
    for (int i = 0; i < 8; ++i)
        reg.arg<uint32_t>(0);  // LibraryConfigData
    reg.arg<uint64_t>(0).arg<uint64_t>(0x10000);
    const Reply r = e.call(bsd, 0, reg);
    CHECK_EQ(r.result, 0u);
    CHECK_EQ(r.get<uint32_t>(0), 0u);  // anything else makes the SDK abort
    Msg mon;
    mon.send_pid = true;
    mon.arg<uint64_t>(r.get<uint64_t>(0));
    CHECK_EQ(e.call(bsd, 1, mon).result, 0u);
    // A UDP socket on the ldn network: works, but nothing ever arrives.
    Msg sock;
    sock.arg<int32_t>(2).arg<int32_t>(2).arg<int32_t>(17);  // AF_INET, SOCK_DGRAM, UDP
    const Reply s = e.call(bsd, 2, sock);
    CHECK_EQ(s.result, 0u);
    const int32_t fd = s.get<int32_t>(0);
    CHECK(fd >= 0);
    CHECK_EQ(s.get<int32_t>(4), 0);
    // A second session (the SDK uses several) sees the same descriptor.
    const Handle bsd2 = e.open_service("bsd:u");
    const uint8_t addr[16] = {16, 2, 0, 0, 169, 254, 1, 1};  // 169.254.1.1, port 0
    CHECK(e.k.memory().copy_to_user(kBufA, addr, sizeof(addr)));
    Msg bind;
    bind.arg<int32_t>(fd);
    bind.a = {{kBufA, sizeof(addr), 0}};
    const Reply b = e.call(bsd2, 13, bind);
    CHECK_EQ(b.get<int32_t>(0), 0);
    Msg name;
    name.arg<int32_t>(fd);
    name.b = {{kBufB, 16, 0}};
    const Reply n = e.call(bsd, 16, name);
    CHECK_EQ(n.get<int32_t>(0), 0);
    CHECK_EQ(n.get<uint32_t>(8), 16u);
    CHECK_EQ(e.read<uint8_t>(kBufB + 1), 2u);
    CHECK((e.read<uint8_t>(kBufB + 2) << 8 | e.read<uint8_t>(kBufB + 3)) != 0);  // a port was picked
    CHECK_EQ(e.read<uint8_t>(kBufB + 4), 169u);
    // Non-blocking via Fcntl(F_SETFL, O_NONBLOCK as 0x800), read back with F_GETFL.
    Msg setfl;
    setfl.arg<int32_t>(fd).arg<int32_t>(4).arg<int32_t>(0x800);
    CHECK_EQ(e.call(bsd, 20, setfl).get<int32_t>(0), 0);
    Msg getfl;
    getfl.arg<int32_t>(fd).arg<int32_t>(3).arg<int32_t>(0);
    CHECK_EQ(e.call(bsd, 20, getfl).get<int32_t>(0), 0x800);
    // SendTo: the datagram is accepted; RecvFrom: EAGAIN.
    Msg send;
    send.arg<int32_t>(fd).arg<int32_t>(0);
    send.a = {{kBufA, 12, 0}, {kBufA, 16, 0}};
    CHECK_EQ(e.call(bsd, 11, send).get<int32_t>(0), 12);
    Msg recv;
    recv.arg<int32_t>(fd).arg<int32_t>(0);
    recv.b = {{kBufB, 0x100, 0}, {kBufB + 0x100, 16, 0}};
    const Reply rv = e.call(bsd, 9, recv);
    CHECK_EQ(rv.get<int32_t>(0), -1);
    CHECK_EQ(rv.get<int32_t>(4), 11);  // EAGAIN
    // Poll: writable, not readable.
    const int32_t pfd[2] = {fd, 0x1 | 0x4};  // fd, events POLLIN|POLLOUT (revents 0)
    CHECK(e.k.memory().copy_to_user(kBufA, pfd, sizeof(pfd)));
    Msg poll;
    poll.arg<uint32_t>(1).arg<int32_t>(100);
    poll.a = {{kBufA, sizeof(pfd), 0}};
    poll.b = {{kBufB, sizeof(pfd), 0}};
    CHECK_EQ(e.call(bsd, 6, poll).get<int32_t>(0), 1);
    CHECK_EQ(e.read<int16_t>(kBufB + 6), 0x4);
    // TCP cannot reach anyone.
    Msg tcp;
    tcp.arg<int32_t>(2).arg<int32_t>(1).arg<int32_t>(6);
    const int32_t tfd = e.call(bsd, 2, tcp).get<int32_t>(0);
    CHECK(tfd >= 0 && tfd != fd);
    Msg conn;
    conn.arg<int32_t>(tfd);
    conn.a = {{kBufA, 16, 0}};
    const Reply cr = e.call(bsd, 14, conn);
    CHECK_EQ(cr.get<int32_t>(0), -1);
    CHECK_EQ(cr.get<int32_t>(4), 101);  // ENETUNREACH
    // Close once, then the descriptor is gone.
    Msg close;
    close.arg<int32_t>(fd);
    CHECK_EQ(e.call(bsd, 26, close).get<int32_t>(0), 0);
    const Reply c = e.call(bsd, 26, close);
    CHECK_EQ(c.get<int32_t>(0), -1);
    CHECK_EQ(c.get<int32_t>(4), 9);  // EBADF

    const Handle ssl = e.open_service("ssl");
    Msg version;
    version.arg<uint32_t>(3);
    CHECK_EQ(e.call(ssl, 5, version).result, 0u);
    Msg create;
    create.send_pid = true;
    create.arg<uint32_t>(0).arg<uint64_t>(0);
    const Handle ctx = e.open(ssl, 0, create);
    Msg opt;
    opt.arg<uint32_t>(1).arg<int32_t>(1);
    CHECK_EQ(e.call(ctx, 0, opt).result, 0u);
    Msg get;
    get.arg<uint32_t>(1);
    CHECK_EQ(e.call(ctx, 1, get).get<int32_t>(0), 1);
    CHECK_EQ(e.call(ctx, 3).get<uint32_t>(0), 0u);
}

void test_bcat() {
    ServicesEnv e;
    const Handle creator = e.open_service("bcat:u");
    Msg pid;
    pid.arg<uint64_t>(0);
    const Handle bcat = e.open(creator, 0, pid);
    CHECK_EQ(e.call(bcat, 10100).result, bcat_result::kInternetRequestNotAccepted.raw);
    const Handle storage = e.open(creator, 1, pid);
    Msg dirs;
    dirs.b = {{kBufB, 0x20 * 8, 0}};
    const Reply en = e.call(storage, 10, dirs);
    CHECK_EQ(en.result, 0u);
    CHECK_EQ(en.get<int32_t>(0), 0);
    const Handle dir = e.open(storage, 1);
    Msg name;
    for (int i = 0; i < 8; ++i)
        name.arg<uint32_t>(0x74666967);  // "gift"
    CHECK_EQ(e.call(dir, 0, name).result, bcat_result::kNotFound.raw);
    CHECK_EQ(e.call(dir, 2).result, bcat_result::kNotOpened.raw);
    const Handle file = e.open(storage, 0);
    CHECK_EQ(e.call(file, 2).result, bcat_result::kNotOpened.raw);
}

void test_friends() {
    ServicesEnv e;
    const Handle creator = e.open_service("friend:u");
    const Handle friends = e.open(creator, 0);
    const Reply ev = e.call(friends, 0);
    CHECK_EQ(ev.copy.size(), 1u);
    if (!ev.copy.empty())
        CHECK(!e.signaled(ev.copy[0]));
    CHECK_EQ(e.call(friends, 1).result, 0u);
    Msg uid;
    uid.arg(test_config().user_id);
    const Handle notifications = e.open(creator, 1, uid);
    CHECK_EQ(e.call(notifications, 0).copy.size(), 1u);
    // Play history: layout unknown, stops the guest.
    Msg key;
    key.command = 10700;
    key.arg<uint8_t>(1).arg(test_config().user_id);
    key.c = {{kBufB, 0x40}};
    e.send(friends, key, SvcOutcome::Unimplemented);
}

void test_event_api() {
    ServicesEnv e;
    Event ev;
    Handle h1 = kInvalidHandle;
    Handle h2 = kInvalidHandle;
    CHECK(ev.make_handle(e.k, &h1).ok());
    CHECK(ev.make_handle(e.k, &h2).ok());
    CHECK(h1 != h2);
    CHECK(!e.signaled(h1));
    ev.signal(e.k);
    CHECK(e.signaled(h1) && e.signaled(h2));
    CHECK(ev.is_signaled(e.k));
    // The guest can clear it through its own handle (svcClearEvent).
    CHECK_EQ(e.svc(kSvcClearEvent, {h1})[0], kSuccess.raw);
    CHECK(!ev.is_signaled(e.k));
    ev.signal(e.k);
    ev.clear(e.k);
    CHECK(!e.signaled(h2));
}

}  // namespace

int main() {
    RUN(test_event_api);
    RUN(test_language_for_locale);
    RUN(test_am_messages);
    RUN(test_am_operation_mode_switch);
    RUN(test_am_proxy_objects);
    RUN(test_am_launch_parameters);
    RUN(test_am_storage);
    RUN(test_am_domain_copy_handle);
    RUN(test_set);
    RUN(test_time_clocks);
    RUN(test_time_zone);
    RUN(test_time_snapshot);
    RUN(test_csrng);
    RUN(test_pctl);
    RUN(test_vi);
    RUN(test_audout);
    RUN(test_audout_sink);
    RUN(test_requests_in_flight);
    RUN(test_acc);
    RUN(test_quiet_services);
    RUN(test_swkbd);
    RUN(test_applets);
    RUN(test_nifm);
    RUN(test_nifm_online);
    RUN(test_ldn);
    RUN(test_friends);
    RUN(test_sockets);
    RUN(test_bcat);
    return finish();
}
