// audout:u, see lgp/hle/audout.hpp.
#include "lgp/hle/audout.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <set>
#include <string>

#include "lgp/hle/event.hpp"
#include "lgp/runtime/log.hpp"

namespace lgp::hle {

namespace {

constexpr uint32_t kSampleRate = 48000;
constexpr uint32_t kChannels = 2;  // default and the sink's layout
constexpr uint32_t kPcmInt16 = 2;  // nn::audio::SampleFormat
constexpr uint32_t kStateStarted = 0, kStateStopped = 1;
constexpr const char kDeviceName[] = "DeviceOut";
constexpr uint32_t kNameSize = 0x100;
// Buffers one IAudioOut holds: appended and not yet fetched by
// GetReleasedAudioOutBuffers. Horizon refuses more (Ryujinx:
// AudioDeviceSession, Constants.AudioDeviceBufferCountMax).
constexpr size_t kMaxBuffers = 32;
// nn::audio ResultBufferRingFull (module 153, description 8; Ryujinx ResultCode).
constexpr rt::Result kResultBufferRingFull = rt::make_result(153, 8);

// nn::audio::AudioOutBuffer as the guest passes it (in buffer of Append).
struct GuestBuffer {
    uint64_t next;
    uint64_t sample_buffer;
    uint64_t capacity;
    uint64_t data_size;
    uint64_t data_offset;
};

void ignore(Request&, Response&) {}

// Diagnosis: LGP_AUDOUT_LOG=N logs the first N Start/Stop/GetReleased calls of
// every out with the guest time (the base of svcGetSystemTick), for hangs
// where the guest stops fetching released buffers. 0 or unset = off.
uint64_t event_log_limit() {
    static const uint64_t limit = [] {
        const char* v = std::getenv("LGP_AUDOUT_LOG");
        return v != nullptr ? std::strtoull(v, nullptr, 10) : 0ull;
    }();
    return limit;
}

}  // namespace

struct AudioOutState::Out {
    struct Pending {
        uint64_t tag = 0;
        uint64_t frames = 0;
    };
    std::mutex mutex;
    uint32_t channels = kChannels;  // 2 or 6 (5.1, LGP asks for it)
    uint32_t state = kStateStopped;
    std::deque<Pending> queue;
    std::deque<uint64_t> released;
    uint64_t cursor_ns = 0;  // host time the queue head started playing, 0 = not yet
    uint64_t played = 0;     // frames
    float volume = 1.0f;
    uint64_t dropped = 0;  // frames the sink did not accept
    bool drop_warned = false;
    // Numbers for the acceptance runs (docs/analysis/11-pc-abnahme-p5.md),
    // logged every kReportFrames appended frames: no sound leaves the machine.
    struct Report {
        uint64_t appended = 0, dropped = 0, underruns = 0;
        uint64_t sum_sq[6] = {}, peak[6] = {};  // per guest channel
    } report;
    uint64_t appended_total = 0;
    uint64_t underruns = 0;  // the queue ran empty while started
    // For hang reports: what the guest did with this out.
    uint32_t index = 0;            // order of OpenAudioOut since boot
    uint64_t appends = 0;          // AppendAudioOutBuffer calls, refused ones included
    uint64_t refused = 0;          // appends refused because kMaxBuffers were held
    uint64_t starts = 0;           // StartAudioOut calls
    uint64_t release_calls = 0;    // GetReleasedAudioOutBuffers calls
    uint64_t released_to_guest = 0;  // tags those calls returned
    uint64_t last_release_call_ns = 0;  // host time (update's clock) of the last such call, 0 = none
    uint64_t now_ns = 0;           // the last update's time
    bool refuse_warned = false;
    uint64_t events_logged = 0;  // LGP_AUDOUT_LOG
    Event event;
    std::unique_ptr<platform::IAudioSink> sink;
};

std::shared_ptr<AudioOutState::Out> AudioOutState::open(rt::Kernel&, uint32_t channels) {
    auto out = std::make_shared<Out>();
    out->channels = channels;
    if (audio_ != nullptr)
        out->sink = audio_->open_sink({kSampleRate, kChannels});
    std::lock_guard lock(mutex_);
    outs_.erase(std::remove_if(outs_.begin(), outs_.end(), [](const auto& w) { return w.expired(); }), outs_.end());
    out->index = opened_++;
    outs_.push_back(out);
    return out;
}

void AudioOutState::update(rt::Kernel& kernel, uint64_t now_ns) {
    std::vector<std::shared_ptr<Out>> live;
    {
        std::lock_guard lock(mutex_);
        for (const auto& w : outs_)
            if (auto o = w.lock())
                live.push_back(std::move(o));
    }
    for (const auto& o : live) {
        std::lock_guard lock(o->mutex);
        o->now_ns = now_ns;
        if (o->state != kStateStarted)
            continue;
        bool any = false;
        while (!o->queue.empty()) {
            if (o->cursor_ns == 0)
                o->cursor_ns = now_ns;
            const Out::Pending& head = o->queue.front();
            const uint64_t end = o->cursor_ns + head.frames * 1'000'000'000ull / kSampleRate;
            if (end > now_ns)
                break;
            o->released.push_back(head.tag);
            o->played += head.frames;
            o->cursor_ns = end;
            o->queue.pop_front();
            any = true;
        }
        if (o->queue.empty()) {
            if (o->cursor_ns != 0)
                ++o->underruns;
            o->cursor_ns = 0;  // underrun: the next buffer starts when it arrives
        }
        if (any)
            o->event.signal(kernel);
    }
}

std::vector<std::string> AudioOutState::report() {
    std::vector<std::shared_ptr<Out>> live;
    {
        std::lock_guard lock(mutex_);
        for (const auto& w : outs_)
            if (auto o = w.lock())
                live.push_back(std::move(o));
    }
    std::vector<std::string> out;
    for (size_t i = 0; i < live.size(); ++i) {
        Out& o = *live[i];
        char line[512];
        std::unique_lock lock(o.mutex, std::try_to_lock);
        if (!lock.owns_lock()) {
            std::snprintf(line, sizeof(line), "audout %zu: busy (lock held)", i);
        } else {
            char last[48] = "never";
            if (o.last_release_call_ns != 0)
                std::snprintf(last, sizeof(last), "%.1f s ago",
                              double(o.now_ns - std::min(o.now_ns, o.last_release_call_ns)) / 1e9);
            std::snprintf(line, sizeof(line),
                          "audout %zu (open #%u): %u channels, %s (%llu starts), %zu queued, %zu released, "
                          "%.1f s played, %.1f s appended, %llu appends (%llu refused), %llu GetReleased calls "
                          "(last %s) returned %llu, underruns %llu, dropped %llu frames%s",
                          i, o.index, o.channels, o.state == kStateStarted ? "started" : "stopped",
                          (unsigned long long)o.starts, o.queue.size(), o.released.size(),
                          double(o.played) / kSampleRate, double(o.appended_total) / kSampleRate,
                          (unsigned long long)o.appends, (unsigned long long)o.refused,
                          (unsigned long long)o.release_calls, last, (unsigned long long)o.released_to_guest,
                          (unsigned long long)o.underruns, (unsigned long long)o.dropped,
                          o.sink ? ", sink open" : "");
        }
        out.push_back(line);
    }
    return out;
}

namespace {

class AudioOut final : public Service {
public:
    explicit AudioOut(std::shared_ptr<AudioOutState::Out> out) : Service("audout:IAudioOut"), out_(std::move(out)) {
        on(0, [this](Request&, Response& resp) {  // GetAudioOutState
            std::lock_guard lock(out_->mutex);
            resp.push<uint32_t>(out_->state);
        });
        on(1, [this](Request& req, Response&) {  // Start
            std::lock_guard lock(out_->mutex);
            out_->state = kStateStarted;
            ++out_->starts;
            log_event(req.kernel(), "Start", out_->starts);
            if (out_->sink)
                out_->sink->set_paused(false);
        });
        on(2, [this](Request& req, Response&) {  // Stop
            std::lock_guard lock(out_->mutex);
            out_->state = kStateStopped;
            log_event(req.kernel(), "Stop");
            out_->cursor_ns = 0;
            if (out_->sink)
                out_->sink->set_paused(true);
        });
        on(3, [this](Request& req, Response& resp) { append(req, resp); });  // AppendAudioOutBuffer
        on(7, [this](Request& req, Response& resp) { append(req, resp); });  // AppendAudioOutBufferAuto
        on(4, [this](Request& req, Response& resp) {  // RegisterBufferEvent
            Handle h = rt::kInvalidHandle;
            resp.result = out_->event.make_handle(req.kernel(), &h);
            if (resp.result.ok())
                resp.copy_handles.push_back(h);
        });
        on(5, [this](Request& req, Response& resp) { released(req, resp); });  // GetReleasedAudioOutBuffers
        on(8, [this](Request& req, Response& resp) { released(req, resp); });  // GetReleasedAudioOutBuffersAuto
        on(6, [this](Request& req, Response& resp) {  // ContainsAudioOutBuffer(tag)
            const uint64_t tag = req.pop<uint64_t>();
            std::lock_guard lock(out_->mutex);
            bool found = std::find(out_->released.begin(), out_->released.end(), tag) != out_->released.end();
            for (const auto& p : out_->queue) found |= p.tag == tag;
            resp.push<uint8_t>(found ? 1 : 0);
        });
        on(9, [this](Request&, Response& resp) {  // GetAudioOutBufferCount
            std::lock_guard lock(out_->mutex);
            resp.push<uint32_t>(static_cast<uint32_t>(out_->queue.size()));
        });
        on(10, [this](Request&, Response& resp) {  // GetAudioOutPlayedSampleCount
            std::lock_guard lock(out_->mutex);
            resp.push<uint64_t>(out_->played);
        });
        on(11, [this](Request& req, Response& resp) {  // FlushAudioOutBuffers: release everything now
            std::lock_guard lock(out_->mutex);
            const bool had = !out_->queue.empty();
            for (const auto& p : out_->queue) out_->released.push_back(p.tag);
            out_->queue.clear();
            out_->cursor_ns = 0;
            if (had)
                out_->event.signal(req.kernel());
            resp.push<uint8_t>(had ? 1 : 0);
        });
        on(12, [this](Request& req, Response&) {  // SetAudioOutVolume(f32)
            const float v = req.pop<float>();
            std::lock_guard lock(out_->mutex);
            out_->volume = v;
        });
        on(13, [this](Request&, Response& resp) {  // GetAudioOutVolume
            std::lock_guard lock(out_->mutex);
            resp.push<float>(out_->volume);
        });
    }

private:
    // Caller holds out_->mutex.
    void log_event(rt::Kernel& kernel, const char* what, uint64_t value = 0) {
        if (out_->events_logged >= event_log_limit())
            return;
        ++out_->events_logged;
        rt::log(rt::LogLevel::Info,
                "audout: open #%u %s %llu at guest %.3f ms (%zu queued, %zu released waiting, %llu appends)",
                out_->index, what, (unsigned long long)value, double(kernel.clock().now_ns()) / 1e6,
                out_->queue.size(), out_->released.size(), (unsigned long long)out_->appends);
    }

    static constexpr uint64_t kReportFrames = 10 * kSampleRate;  // one line per 10 s of sound

    // Caller holds out_->mutex; pcm is the guest's interleaved layout.
    void account(const std::vector<int16_t>& pcm, uint64_t frames) {
        auto& r = out_->report;
        const uint32_t ch = std::min<uint32_t>(out_->channels, 6);
        for (uint64_t i = 0; i < frames; ++i)
            for (uint32_t c = 0; c < ch; ++c) {
                const int64_t v = pcm[size_t(i * out_->channels + c)];
                r.sum_sq[c] += uint64_t(v * v);
                r.peak[c] = std::max<uint64_t>(r.peak[c], uint64_t(v < 0 ? -v : v));
            }
        r.appended += frames;
        out_->appended_total += frames;
    }

    // After the sink write, so the frames it dropped count in this line.
    void report_if_due() {
        auto& r = out_->report;
        const uint32_t ch = std::min<uint32_t>(out_->channels, 6);
        if (r.appended < kReportFrames)
            return;
        std::string levels;
        for (uint32_t c = 0; c < ch; ++c) {
            char buf[48];
            std::snprintf(buf, sizeof(buf), "%s%.0f/%llu", c == 0 ? "" : " ",
                          std::sqrt(double(r.sum_sq[c]) / double(r.appended)), (unsigned long long)r.peak[c]);
            levels += buf;
        }
        rt::log(rt::LogLevel::Info,
                "audout: %.1f s appended (total %.1f s), %u channels, dropped %llu frames, underruns %llu, "
                "rms/peak per channel %s",
                double(r.appended) / kSampleRate, double(out_->appended_total) / kSampleRate, out_->channels,
                (unsigned long long)(out_->dropped - r.dropped), (unsigned long long)(out_->underruns - r.underruns),
                levels.c_str());
        AudioOutState::Out::Report next;
        next.dropped = out_->dropped;
        next.underruns = out_->underruns;
        r = next;
    }

    void append(Request& req, Response& resp) {
        const uint64_t tag = req.pop<uint64_t>();
        GuestBuffer b{};
        if (!req.read_in_as(0, &b)) {
            resp.result = rt::kernel_result::kInvalidPointer;
            return;
        }
        std::lock_guard lock(out_->mutex);
        ++out_->appends;
        if (out_->queue.size() + out_->released.size() >= kMaxBuffers) {
            // As on the console: the buffer is not taken, nothing reaches the
            // sink. A guest that ignores this result and keeps appending
            // (its free count wrapped) no longer grows the queue or the sound.
            ++out_->refused;
            if (!out_->refuse_warned) {
                out_->refuse_warned = true;
                rt::log(rt::LogLevel::Warn,
                        "audout: open #%u holds %zu buffers, refusing more (ResultBufferRingFull); "
                        "the guest appends without fetching released ones",
                        out_->index, kMaxBuffers);
            }
            resp.result = kResultBufferRingFull;
            return;
        }
        const uint64_t frame_bytes = uint64_t{out_->channels} * 2;
        const uint64_t frames = b.data_size / frame_bytes;
        if (out_->sink && frames != 0) {
            std::vector<int16_t> pcm(static_cast<size_t>(frames * out_->channels));
            if (req.read_guest(b.sample_buffer + b.data_offset, pcm.data(), frames * frame_bytes)) {
                account(pcm, frames);
                if (out_->channels == 6) {  // 5.1 -> stereo: FL + C/2 + RL/2, FR + C/2 + RR/2 (LFE dropped)
                    std::vector<int16_t> st(static_cast<size_t>(frames * 2));
                    for (uint64_t i = 0; i < frames; ++i) {
                        const int16_t* f = &pcm[size_t(i * 6)];
                        st[size_t(i * 2)] = static_cast<int16_t>(std::clamp(f[0] + f[2] / 2 + f[4] / 2, -32768, 32767));
                        st[size_t(i * 2 + 1)] = static_cast<int16_t>(std::clamp(f[1] + f[2] / 2 + f[5] / 2, -32768, 32767));
                    }
                    pcm.swap(st);
                }
                if (out_->volume != 1.0f)
                    for (int16_t& v : pcm)
                        v = static_cast<int16_t>(std::clamp(static_cast<int>(v * out_->volume), -32768, 32767));
                // Drift policy: the guest is paced by the host clock, the sink
                // drains at the sound card's rate. A sink whose queue is full
                // (the card runs slower than the host clock, or the host
                // stalled) accepts fewer frames; the rest is dropped rather
                // than letting the latency grow. An underrun plays silence.
                const size_t accepted = out_->sink->write(pcm.data(), static_cast<size_t>(frames));
                if (accepted < frames) {
                    out_->dropped += frames - accepted;
                    if (!out_->drop_warned) {
                        out_->drop_warned = true;
                        rt::log(rt::LogLevel::Warn, "audout: sink queue full, dropping frames (host clock ahead of the sound card)");
                    }
                }
                report_if_due();
            }
        }
        out_->queue.push_back({tag, frames});
    }

    void released(Request& req, Response& resp) {
        const HipcBuffer out = req.out_buffer(0);
        std::lock_guard lock(out_->mutex);
        std::vector<uint64_t> tags;
        while (!out_->released.empty() && (tags.size() + 1) * 8 <= out.size) {
            tags.push_back(out_->released.front());
            out_->released.pop_front();
        }
        if (out_->released.empty())
            out_->event.clear(req.kernel());
        // The whole out buffer is written, unused entries as 0: the SDK's
        // GetReleasedAudioOutBuffer reads the first entry whatever the count
        // says, so a stale tag there reads as a buffer released again and
        // again (LGP's sound thread then drains "released" buffers forever).
        const size_t count = tags.size();
        ++out_->release_calls;
        out_->released_to_guest += count;
        out_->last_release_call_ns = std::max<uint64_t>(out_->now_ns, 1);
        log_event(req.kernel(), "GetReleased ->", count);
        // At most 32 entries (the buffers one AudioOut holds): out.size comes from the guest.
        tags.resize(std::max<size_t>(tags.size(), std::min<size_t>(out.size / 8, 32)), 0);
        if (!tags.empty() && !req.write_out(0, tags.data(), tags.size() * 8)) {
            resp.result = rt::kernel_result::kInvalidPointer;
            return;
        }
        resp.push<uint32_t>(static_cast<uint32_t>(count));
    }

    std::shared_ptr<AudioOutState::Out> out_;
};

class AudioOutManager final : public Service {
public:
    explicit AudioOutManager(std::shared_ptr<AudioOutState> state) : Service("audout:u"), state_(std::move(state)) {
        on(0, [](Request& req, Response& resp) { list(req, resp); });  // ListAudioOuts
        on(2, [](Request& req, Response& resp) { list(req, resp); });  // ListAudioOutsAuto
        on(1, [this](Request& req, Response& resp) { open(req, resp); });  // OpenAudioOut
        on(3, [this](Request& req, Response& resp) { open(req, resp); });  // OpenAudioOutAuto
    }

private:
    static void list(Request& req, Response& resp) {
        char name[kNameSize] = {};
        std::memcpy(name, kDeviceName, sizeof(kDeviceName));
        const bool room = req.out_buffer(0).size >= kNameSize;
        if (room && !req.write_out(0, name, kNameSize)) {
            resp.result = rt::kernel_result::kInvalidPointer;
            return;
        }
        resp.push<uint32_t>(room ? 1 : 0);
    }

    void open(Request& req, Response& resp) {
        // In: u32 sample rate, u16 channel count, u16 reserved, u64 aruid; device name buffer.
        const uint32_t rate = req.pop<uint32_t>();
        const uint16_t requested = req.pop<uint16_t>();
        const uint32_t channels = requested == 0 ? kChannels : requested;
        if ((rate != 0 && rate != kSampleRate) || (channels != 2 && channels != 6)) {
            rt::log(rt::LogLevel::Error, "audout: OpenAudioOut(%u Hz, %u channels) is not supported", rate, channels);
            resp.unimplemented = true;
            return;
        }
        char name[kNameSize] = {};
        std::memcpy(name, kDeviceName, sizeof(kDeviceName));
        if (req.out_buffer(0).size >= kNameSize && !req.write_out(0, name, kNameSize)) {
            resp.result = rt::kernel_result::kInvalidPointer;
            return;
        }
        auto out = state_->open(req.kernel(), channels);
        rt::log(rt::LogLevel::Info, "audout: OpenAudioOut(%u Hz, %u channels requested) -> %u channels, open #%u at guest %.3f ms",
                rate, requested, channels, out->index, double(req.kernel().clock().now_ns()) / 1e6);
        resp.push<uint32_t>(kSampleRate);
        resp.push<uint32_t>(channels);
        resp.push<uint32_t>(kPcmInt16);
        resp.push<uint32_t>(kStateStopped);
        resp.push_object(std::make_shared<AudioOut>(std::move(out)));
    }

    std::shared_ptr<AudioOutState> state_;
};

}  // namespace

std::shared_ptr<AudioOutState> register_audout(ServiceManager& sm, platform::IAudio* audio) {
    auto state = std::make_shared<AudioOutState>(audio);
    sm.register_service("audout:u", [state] { return std::make_shared<AudioOutManager>(state); });
    return state;
}

}  // namespace lgp::hle
