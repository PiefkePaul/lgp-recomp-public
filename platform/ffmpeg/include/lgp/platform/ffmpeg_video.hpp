// IVideo on FFmpeg's libavcodec (desktop, optional: LGP_FFMPEG_DIR or
// pkg-config libavcodec). Linked dynamically (LGPL), see THIRD_PARTY.md.
#pragma once

#include "lgp/platform/video.hpp"

namespace lgp::platform {

class FfmpegVideo final : public IVideo {
public:
    std::unique_ptr<IVideoDecoder> open_decoder(const VideoStreamInfo& info) override;
};

}  // namespace lgp::platform
