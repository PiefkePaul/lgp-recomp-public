// IVideo that tries one backend and falls back to another (P7.3): when the
// primary cannot open a stream, fails before its first picture (for example
// a hardware decoder whose output format is not converted, which would
// otherwise show black pictures), or shows no picture by the end of the
// stream or within kMaxKeptFrames frames, the stream goes to the fallback.
// Until the primary's first picture, every compressed frame is kept, so the
// fallback decodes the stream from its start. Once the primary delivered a
// picture it stays, later errors included. A switch holds for that stream
// only, but also after an error that would have passed: the fallback may be
// slower (software).
#pragma once

#include <memory>

#include "lgp/platform/video.hpp"

namespace lgp::platform {

class FallbackVideo final : public IVideo {
public:
    // Frames kept while the primary has shown nothing; a primary still
    // without a picture then counts as failed. Hardware decoders show their
    // first picture after a few frames and MediaCodec stops taking input
    // while its output is not taken; FFmpeg's frame threads delay by up to
    // 16 frames. The fallback decodes all kept frames in one call, so this
    // also bounds that burst (1080p NV12: about 3 MiB per picture).
    static constexpr size_t kMaxKeptFrames = 30;

    FallbackVideo(IVideo& primary, IVideo& fallback) : primary_(primary), fallback_(fallback) {}
    std::unique_ptr<IVideoDecoder> open_decoder(const VideoStreamInfo& info) override;

private:
    IVideo& primary_;
    IVideo& fallback_;
};

}  // namespace lgp::platform
