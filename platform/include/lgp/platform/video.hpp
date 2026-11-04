// IVideoDecoder: compressed video frames to NV12 pictures (P5, the movie::
// HLE for in-game videos). The runtime demuxes the container itself and hands
// over one compressed frame at a time; the backend decodes it (FFmpeg on the
// desktop, later WebCodecs in the browser). The null backend returns black
// pictures with the right size and timestamps, so a game sees a video that
// plays for its full length even without a decoder.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace lgp::platform {

enum class VideoCodec { Vp9, H264 };

struct VideoStreamInfo {
    VideoCodec codec = VideoCodec::Vp9;
    uint32_t width = 0;  // from the container, the decoder may report others
    uint32_t height = 0;
    std::vector<uint8_t> codec_private;  // container's codec data (avcC for H.264)
};

// One decoded picture, NV12 tightly packed: width*height luma bytes, then
// width*height/2 bytes of interleaved Cb/Cr (width and height even).
struct VideoPicture {
    uint32_t width = 0;
    uint32_t height = 0;
    int64_t pts_us = 0;
    std::vector<uint8_t> nv12;
};

class IVideoDecoder {
public:
    virtual ~IVideoDecoder() = default;
    // Decodes one compressed frame (data == nullptr, size 0: end of stream,
    // return what the decoder still holds). Appends the pictures that became
    // ready, in presentation order; false on a decoding error (the caller
    // keeps going with the next frame).
    virtual bool decode(const uint8_t* data, size_t size, int64_t pts_us, std::vector<VideoPicture>* out) = 0;
};

class IVideo {
public:
    virtual ~IVideo() = default;
    // nullptr if the backend cannot decode this stream.
    virtual std::unique_ptr<IVideoDecoder> open_decoder(const VideoStreamInfo& info) = 0;
};

}  // namespace lgp::platform
