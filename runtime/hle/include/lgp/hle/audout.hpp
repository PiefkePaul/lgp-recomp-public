// audout:u (P5.2, first part): the PCM output LGP uses directly (Wwise mixes
// itself, docs/analysis/02-services.md). The guest appends buffers of
// interleaved signed 16-bit frames and waits on the buffer event until they
// are "released", i.e. played. The pace comes from the host clock: the host
// calls AudioOutState::update() periodically (the boot program every few
// milliseconds), which releases every buffer whose playing time has passed
// and signals the event. The samples also go to a platform::IAudio sink if
// one is configured (nullptr: timing only, no sound), with the volume
// applied. Host clock and sound card drift apart; the sink's queue is bounded
// and frames it does not accept are dropped (no growing latency), an
// underrun plays silence (docs/knowledge/horizon-audout-hwopus.md).
//
// Command ids and layouts from switchbrew "Audio services" (IAudioOutManager,
// IAudioOut); the commands follow the Ryujinx reference run. Only the one
// device "DeviceOut", 48 kHz, PcmInt16, 2 or 6 channels as requested (0 -> 2;
// LGP asks for 5.1, as on a docked console; the sink gets a stereo downmix).
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "lgp/hle/service.hpp"
#include "lgp/platform/audio.hpp"

namespace lgp::hle {

class AudioOutState {
public:
    explicit AudioOutState(platform::IAudio* audio) : audio_(audio) {}
    // Releases what has finished playing by now_ns (monotonic host time) and
    // signals the buffer events. Call from one host thread.
    void update(rt::Kernel& kernel, uint64_t now_ns);

    struct Out;  // one opened IAudioOut
    std::shared_ptr<Out> open(rt::Kernel& kernel, uint32_t channels);
    platform::IAudio* audio() const { return audio_; }
    // One line per open IAudioOut (state, queued and released buffers,
    // frames played, appends, GetReleased calls and what they returned,
    // underruns); "busy" if its lock is held. For hang reports,
    // from any host thread.
    std::vector<std::string> report();

private:
    platform::IAudio* audio_;
    std::mutex mutex_;
    std::vector<std::weak_ptr<Out>> outs_;
    uint32_t opened_ = 0;
};

// Registers audout:u. audio may be nullptr. The returned state is what the
// host ticks; it must be ticked for the guest's buffers to be released.
std::shared_ptr<AudioOutState> register_audout(ServiceManager& sm, platform::IAudio* audio);

}  // namespace lgp::hle
