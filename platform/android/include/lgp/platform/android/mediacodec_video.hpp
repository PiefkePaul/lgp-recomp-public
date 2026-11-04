// IVideo on Android's MediaCodec (NDK, P7.3): VP9 and H.264 through the
// phone's hardware decoders, output as ByteBuffers converted to NV12.
#pragma once

#include "lgp/platform/video.hpp"

namespace lgp::platform::android {

class MediaCodecVideo final : public IVideo {
public:
    std::unique_ptr<IVideoDecoder> open_decoder(const VideoStreamInfo& info) override;
};

}  // namespace lgp::platform::android
