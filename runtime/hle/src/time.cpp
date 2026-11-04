// time:u (IStaticService, "ForGlue" on newer firmware: same command ids).
//
// One clock for everything: the steady clock counts seconds since the
// runtime started (system_tick), the user, local and network system clocks
// are that plus a fixed offset taken once at start (SystemConfig or the
// host clock). So time never jumps backwards and two snapshots of one run
// always come from the same context.
//
// Time zones: one fixed zone from SystemConfig (offset, name), no DST and no
// tzdata. LoadTimeZoneRule writes an own marker into the opaque 0x4000-byte
// rule buffer; the conversion commands read it back.
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <mutex>
#include <set>
#include <string>

#include "lgp/hle/services.hpp"
#include "lgp/runtime/log.hpp"
#include "lgp/runtime/system.hpp"

namespace lgp::hle {
namespace {

using rt::LogLevel;

// --- layouts (switchbrew "Glue services", libnx time.h) --------------------------

struct SteadyClockTimePoint {
    int64_t time_point;  // seconds
    std::array<uint8_t, 16> source_id;
};
struct SystemClockContext {
    int64_t offset;  // posix time = offset + steady time point
    SteadyClockTimePoint steady;
};
struct CalendarTime {
    uint16_t year;
    uint8_t month;  // 1..12
    uint8_t day;    // 1..31
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
    uint8_t pad;
};
struct CalendarAdditionalInfo {
    uint32_t day_of_week;  // 0 = Sunday
    uint32_t day_of_year;  // 0-based
    char time_zone_name[8];
    uint32_t is_dst;
    int32_t utc_offset;  // seconds
};
struct LocationName {
    char name[0x24];
};
// nn::time::ClockSnapshot (GetClockSnapshot output).
struct ClockSnapshot {
    SystemClockContext user_context;
    SystemClockContext network_context;
    int64_t user_time;
    int64_t network_time;
    CalendarTime user_calendar_time;
    CalendarTime network_calendar_time;
    CalendarAdditionalInfo user_additional_info;
    CalendarAdditionalInfo network_additional_info;
    SteadyClockTimePoint steady_clock_time_point;
    LocationName location_name;
    uint8_t is_automatic_correction_enabled;
    uint8_t type;
    uint16_t pad;
};
static_assert(sizeof(SteadyClockTimePoint) == 0x18 && sizeof(SystemClockContext) == 0x20);
static_assert(sizeof(CalendarTime) == 8 && sizeof(CalendarAdditionalInfo) == 0x18);
static_assert(sizeof(LocationName) == 0x24 && sizeof(ClockSnapshot) == 0xD0);

// Own TimeZoneRule contents: marker, offset, abbreviation.
constexpr char kRuleMagic[8] = {'L', 'G', 'P', 'T', 'Z', 'R', 'L', '1'};
constexpr uint64_t kRuleSize = 0x4000;
struct OwnRule {
    char magic[8];
    int32_t utc_offset;
    char name[8];
};

constexpr std::array<uint8_t, 16> kSteadySourceId{0x6c, 0x67, 0x70, 0x2d, 0x73, 0x74, 0x65, 0x61,
                                                   0x64, 0x79, 0x2d, 0x63, 0x6c, 0x6f, 0x63, 0x6b};

// --- civil calendar (proleptic Gregorian, days since 1970-01-01) --------------------

int64_t floor_div(int64_t a, int64_t b) { return a / b - ((a % b != 0) && ((a < 0) != (b < 0))); }

int64_t days_from_civil(int64_t y, int64_t m, int64_t d) {
    // Normalise the month first (ToPosixTime accepts out-of-range fields).
    y += floor_div(m - 1, 12);
    m = m - 12 * floor_div(m - 1, 12);
    y -= m <= 2;
    const int64_t era = floor_div(y, 400);
    const int64_t yoe = y - era * 400;
    const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

void civil_from_days(int64_t z, int64_t* y, int64_t* m, int64_t* d) {
    z += 719468;
    const int64_t era = floor_div(z, 146097);
    const int64_t doe = z - era * 146097;
    const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int64_t mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = yoe + era * 400 + (*m <= 2);
}

struct Zone {
    int32_t utc_offset = 0;
    char name[8] = {};
};

void to_calendar(int64_t posix, const Zone& zone, CalendarTime* ct, CalendarAdditionalInfo* info) {
    const int64_t local = posix + zone.utc_offset;
    const int64_t days = floor_div(local, 86400);
    const int64_t secs = local - days * 86400;
    int64_t y = 0;
    int64_t m = 0;
    int64_t d = 0;
    civil_from_days(days, &y, &m, &d);
    *ct = {};
    ct->year = static_cast<uint16_t>(y);
    ct->month = static_cast<uint8_t>(m);
    ct->day = static_cast<uint8_t>(d);
    ct->hour = static_cast<uint8_t>(secs / 3600);
    ct->minute = static_cast<uint8_t>(secs / 60 % 60);
    ct->second = static_cast<uint8_t>(secs % 60);
    *info = {};
    info->day_of_week = static_cast<uint32_t>(((days + 4) % 7 + 7) % 7);  // 1970-01-01 was a Thursday
    info->day_of_year = static_cast<uint32_t>(days - days_from_civil(y, 1, 1));
    std::memcpy(info->time_zone_name, zone.name, sizeof(zone.name));
    info->is_dst = 0;
    info->utc_offset = zone.utc_offset;
}

int64_t to_posix(const CalendarTime& ct, const Zone& zone) {
    const int64_t days = days_from_civil(ct.year, ct.month, ct.day);
    return days * 86400 + ct.hour * 3600 + ct.minute * 60 + ct.second - zone.utc_offset;
}

// --- shared state ---------------------------------------------------------------------

class TimeState {
public:
    explicit TimeState(const SystemConfig& config) : config_(config) {
        zone_.utc_offset = config.utc_offset_seconds;
        std::memcpy(zone_.name, config.time_zone_name.data(), std::min<size_t>(config.time_zone_name.size(), 7));
        const int64_t start = config.start_posix_time ? *config.start_posix_time : host_posix_time();
        start_steady_ = steady_seconds();
        offset_ = start - start_steady_;
    }

    SteadyClockTimePoint steady_now() const { return {steady_seconds(), kSteadySourceId}; }
    // The context was "set" when the runtime started.
    SystemClockContext context() const { return {offset_, {start_steady_, kSteadySourceId}}; }
    int64_t posix_now() const { return offset_ + steady_seconds(); }
    const Zone& zone() const { return zone_; }
    const SystemConfig& config() const { return config_; }

    LocationName location_name() const {
        LocationName n{};
        std::memcpy(n.name, config_.time_zone_name.data(), std::min(config_.time_zone_name.size(), sizeof(n.name) - 1));
        return n;
    }

    void write_rule(uint8_t* rule) const { write_rule(rule, zone_); }
    static void write_rule(uint8_t* rule, const Zone& zone) {
        OwnRule own{};
        std::memcpy(own.magic, kRuleMagic, sizeof(kRuleMagic));
        own.utc_offset = zone.utc_offset;
        std::memcpy(own.name, zone.name, sizeof(zone.name));
        std::memcpy(rule, &own, sizeof(own));
    }

    // Logs `message` the first time `key` comes up (games repeat the same
    // request many times: LGP loads "GMT" about once per second).
    void warn_once(const std::string& key, const char* message) const {
        std::lock_guard lock(warned_mutex_);
        if (warned_.insert(key).second)
            rt::log(LogLevel::Warn, "%s", message);
    }

    // The zone a rule buffer from LoadTimeZoneRule describes. A buffer the
    // runtime did not write falls back to the configured zone.
    Zone zone_from_rule(const std::vector<uint8_t>& rule) const {
        OwnRule own{};
        if (rule.size() < sizeof(own)) {
            rt::log(LogLevel::Warn, "time: TimeZoneRule buffer of %zu bytes; using the configured zone", rule.size());
            return zone_;
        }
        std::memcpy(&own, rule.data(), sizeof(own));
        if (std::memcmp(own.magic, kRuleMagic, sizeof(kRuleMagic)) != 0) {
            warn_once("foreign rule", "time: TimeZoneRule not from LoadTimeZoneRule; using the configured zone");
            return zone_;
        }
        Zone z;
        z.utc_offset = own.utc_offset;
        std::memcpy(z.name, own.name, sizeof(z.name));
        z.name[7] = 0;
        return z;
    }

private:
    static int64_t steady_seconds() { return static_cast<int64_t>(rt::system_tick() / rt::kCounterFrequency); }
    // The host clock through the C++ library (no OS code in runtime/).
    static int64_t host_posix_time() {
        return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
            .count();
    }

    SystemConfig config_;
    Zone zone_;
    mutable std::mutex warned_mutex_;
    mutable std::set<std::string> warned_;
    int64_t start_steady_ = 0;
    int64_t offset_ = 0;
};

// --- interfaces ------------------------------------------------------------------------

class SystemClock final : public Service {
public:
    explicit SystemClock(std::shared_ptr<const TimeState> state) : Service("time:ISystemClock"), state_(std::move(state)) {
        on(0, [this](Request&, Response& resp) { resp.push<int64_t>(state_->posix_now()); });  // GetCurrentTime
        on(2, [this](Request&, Response& resp) { resp.push(state_->context()); });  // GetSystemClockContext
    }

private:
    std::shared_ptr<const TimeState> state_;
};

class SteadyClock final : public Service {
public:
    explicit SteadyClock(std::shared_ptr<const TimeState> state) : Service("time:ISteadyClock"), state_(std::move(state)) {
        on(0, [this](Request&, Response& resp) { resp.push(state_->steady_now()); });  // GetCurrentTimePoint
    }

private:
    std::shared_ptr<const TimeState> state_;
};

// tzdata names that are UTC without daylight saving time.
bool is_utc_alias(const std::string& name) {
    static const char* const kAliases[] = {
        "UTC",      "GMT",       "UCT",       "Universal", "Zulu",          "Etc/UTC",  "Etc/GMT",
        "Etc/UCT",  "GMT0",      "Etc/GMT0",  "GMT+0",     "GMT-0",         "Etc/GMT+0", "Etc/GMT-0",
        "Greenwich", "Etc/Universal", "Etc/Zulu", "Etc/Greenwich",
    };
    for (const char* a : kAliases)
        if (name == a)
            return true;
    return false;
}

class TimeZoneService final : public Service {
public:
    explicit TimeZoneService(std::shared_ptr<const TimeState> state)
        : Service("time:ITimeZoneService"), state_(std::move(state)) {
        on(0, [this](Request&, Response& resp) { resp.push(state_->location_name()); });  // GetDeviceLocationName
        on(2, [](Request&, Response& resp) { resp.push<int32_t>(1); });  // GetTotalLocationNameCount
        on(3, [this](Request& req, Response& resp) {  // LoadLocationNameList(s32 index, out 0x6) -> s32
            const int32_t index = req.pop<int32_t>();
            const LocationName name = state_->location_name();
            if (index != 0 || req.out_buffer(0).size < sizeof(name)) {
                resp.push<int32_t>(0);
                return;
            }
            if (!req.write_out(0, &name, sizeof(name))) {
                resp.result = rt::kernel_result::kInvalidPointer;
                return;
            }
            resp.push<int32_t>(1);
        });
        on(4, [this](Request& req, Response& resp) {  // LoadTimeZoneRule(LocationName, out 0x16 rule)
            const LocationName want = req.pop<LocationName>();
            const LocationName have = state_->location_name();
            std::vector<uint8_t> rule(kRuleSize, 0);
            if (std::strncmp(want.name, have.name, sizeof(want.name)) == 0) {
                state_->write_rule(rule.data());
            } else if (const std::string requested(want.name, std::find(want.name, want.name + sizeof(want.name), '\0'));
                       is_utc_alias(requested)) {
                // UTC under another name (LGP asks for "GMT"): offset 0, the
                // requested abbreviation.
                Zone utc;
                std::memcpy(utc.name, requested.data(), std::min<size_t>(requested.size(), sizeof(utc.name) - 1));
                TimeState::write_rule(rule.data(), utc);
            } else {
                const std::string message = "time: LoadTimeZoneRule(\"" + requested + "\"): only \"" +
                                            std::string(have.name) + "\" and UTC aliases are known";
                state_->warn_once("load " + requested, message.c_str());
                resp.result = rt::make_result(116, 989);  // NotFound
                return;
            }
            if (!req.write_out(0, rule.data(), rule.size()))
                resp.result = rt::kernel_result::kInvalidPointer;
        });
        on(100, [this](Request& req, Response& resp) {  // ToCalendarTime(posix, in 0x15 rule)
            const int64_t posix = req.pop<int64_t>();
            std::vector<uint8_t> rule;
            if (!req.read_in(0, &rule, sizeof(OwnRule))) {
                resp.result = rt::kernel_result::kInvalidPointer;
                return;
            }
            push_calendar(resp, posix, state_->zone_from_rule(rule));
        });
        on(101, [this](Request& req, Response& resp) {  // ToCalendarTimeWithMyRule(posix)
            push_calendar(resp, req.pop<int64_t>(), state_->zone());
        });
        on(201, [this](Request& req, Response& resp) {  // ToPosixTime(CalendarTime, in 0x15 rule, out 0xA) -> s32
            const CalendarTime ct = req.pop<CalendarTime>();
            std::vector<uint8_t> rule;
            if (!req.read_in(0, &rule, sizeof(OwnRule))) {
                resp.result = rt::kernel_result::kInvalidPointer;
                return;
            }
            write_posix(req, resp, to_posix(ct, state_->zone_from_rule(rule)));
        });
        on(202, [this](Request& req, Response& resp) {  // ToPosixTimeWithMyRule(CalendarTime, out 0xA) -> s32
            const CalendarTime ct = req.pop<CalendarTime>();
            write_posix(req, resp, to_posix(ct, state_->zone()));
        });
    }

private:
    static void push_calendar(Response& resp, int64_t posix, const Zone& zone) {
        CalendarTime ct{};
        CalendarAdditionalInfo info{};
        to_calendar(posix, zone, &ct, &info);
        resp.push(ct);
        resp.push(info);
    }

    // Without DST every local time maps to exactly one POSIX time.
    static void write_posix(Request& req, Response& resp, int64_t posix) {
        if (req.out_buffer(0).size < sizeof(posix)) {
            resp.push<int32_t>(0);
            return;
        }
        if (!req.write_out(0, &posix, sizeof(posix))) {
            resp.result = rt::kernel_result::kInvalidPointer;
            return;
        }
        resp.push<int32_t>(1);
    }

    std::shared_ptr<const TimeState> state_;
};

class StaticService final : public Service {
public:
    explicit StaticService(std::shared_ptr<const TimeState> state) : Service("time:u"), state_(std::move(state)) {
        const auto system_clock = [this](Request&, Response& resp) {
            resp.push_object(std::make_shared<SystemClock>(state_));
        };
        on(0, system_clock);  // GetStandardUserSystemClock
        on(1, system_clock);  // GetStandardNetworkSystemClock (no network: same clock)
        on(2, [this](Request&, Response& resp) { resp.push_object(std::make_shared<SteadyClock>(state_)); });
        on(3, [this](Request&, Response& resp) { resp.push_object(std::make_shared<TimeZoneService>(state_)); });
        on(4, system_clock);  // GetStandardLocalSystemClock
        on(100, [](Request&, Response& resp) {  // IsStandardUserSystemClockAutomaticCorrectionEnabled
            resp.push<uint8_t>(0);
        });
        // CalculateMonotonicSystemClockBaseTimePoint(SystemClockContext) -> s64:
        // the POSIX time at which the system tick counter was zero.
        on(300, [this](Request& req, Response& resp) {
            const SystemClockContext ctx = req.pop<SystemClockContext>();
            const SteadyClockTimePoint now = state_->steady_now();
            if (ctx.steady.source_id != now.source_id) {
                resp.result = time_result::kOffsetInvalid;
                return;
            }
            const int64_t ticks_seconds = static_cast<int64_t>(rt::system_tick() / rt::kCounterFrequency);
            resp.push<int64_t>(ctx.offset + now.time_point - ticks_seconds);
        });
        on(400, [this](Request& req, Response& resp) {  // GetClockSnapshot(u8 type, out 0x1A)
            const ClockSnapshot snap = snapshot(req.pop<uint8_t>());
            if (req.out_buffer(0).size < sizeof(snap)) {
                resp.result = rt::kernel_result::kInvalidSize;
                return;
            }
            if (!req.write_out(0, &snap, sizeof(snap)))
                resp.result = rt::kernel_result::kInvalidPointer;
        });
        // CalculateStandardUserSystemClockDifferenceByUser(in 0x19 snapshot a,
        // in 0x19 snapshot b) -> TimeSpanType (ns): how far the user moved the
        // clock between the snapshots. Contexts never change here, but compare
        // what the guest sent like Horizon would.
        on(500, [](Request& req, Response& resp) {
            ClockSnapshot a{};
            ClockSnapshot b{};
            if (!req.read_in_as(0, &a) || !req.read_in_as(1, &b)) {
                resp.result = rt::kernel_result::kInvalidPointer;
                return;
            }
            int64_t seconds = b.user_context.offset - a.user_context.offset;
            if (a.user_context.steady.source_id != b.user_context.steady.source_id ||
                (a.is_automatic_correction_enabled && b.is_automatic_correction_enabled))
                seconds = 0;
            resp.push<int64_t>(seconds * 1'000'000'000);
        });
    }

private:
    ClockSnapshot snapshot(uint8_t type) const {
        ClockSnapshot s{};
        s.user_context = state_->context();
        s.network_context = state_->context();
        s.steady_clock_time_point = state_->steady_now();
        // Same steady reading for both times, so they cannot disagree.
        s.user_time = s.user_context.offset + s.steady_clock_time_point.time_point;
        s.network_time = s.user_time;
        to_calendar(s.user_time, state_->zone(), &s.user_calendar_time, &s.user_additional_info);
        s.network_calendar_time = s.user_calendar_time;
        s.network_additional_info = s.user_additional_info;
        s.location_name = state_->location_name();
        s.is_automatic_correction_enabled = 0;
        s.type = type;
        return s;
    }

    std::shared_ptr<const TimeState> state_;
};

}  // namespace

void register_time(ServiceManager& sm, const SystemConfig& config) {
    std::shared_ptr<const TimeState> state = std::make_shared<TimeState>(config);
    sm.register_service("time:u", [state] { return std::make_shared<StaticService>(state); });
}

}  // namespace lgp::hle
