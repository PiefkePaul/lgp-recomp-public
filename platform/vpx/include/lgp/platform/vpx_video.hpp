// IVideo on libvpx (VP9 only, software): in-game videos where neither FFmpeg
// nor a system decoder is there, above all iOS. BSD-3-Clause, see
// THIRD_PARTY.md; built by cmake/LgpVpx.cmake.
#pragma once

#include "lgp/platform/video.hpp"

namespace lgp::platform {

class VpxVideo final : public IVideo {
public:
    // nullptr for H.264 (libvpx has VP8/VP9 only).
    std::unique_ptr<IVideoDecoder> open_decoder(const VideoStreamInfo& info) override;
};

}  // namespace lgp::platform
