// Vorbis decoder for the sound tracks of in-game videos (P5, movie:: HLE):
// WebM stores Vorbis as raw packets plus the three header packets in the
// track's CodecPrivate (Xiph lacing, RFC 9559 / Matroska codec mappings).
// Wraps libvorbis (cmake/LgpVorbis.cmake); without it open() fails and the
// HLE does not offer the sound track.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace lgp::hle {

// Splits a Matroska Vorbis CodecPrivate into its header packets
// (identification, comment, setup); false if it is malformed.
bool split_xiph_headers(const std::vector<uint8_t>& codec_private, std::vector<std::vector<uint8_t>>* packets);

class VorbisDecoder {
public:
    VorbisDecoder();
    ~VorbisDecoder();
    VorbisDecoder(const VorbisDecoder&) = delete;
    VorbisDecoder& operator=(const VorbisDecoder&) = delete;

    // True if this build has libvorbis.
    static bool available();

    // Reads the three header packets; false (and *error) if they are not
    // a Vorbis stream this decoder can play.
    bool open(const std::vector<uint8_t>& codec_private, std::string* error);
    uint32_t sample_rate() const { return rate_; }
    uint32_t channels() const { return channels_; }

    // Decodes one audio packet and appends its samples as interleaved
    // 16-bit PCM (Vorbis channel order). The first packet yields nothing
    // (the codec's overlap). False on a broken packet.
    bool decode(const uint8_t* data, size_t size, std::vector<int16_t>* out);
    // Forget the overlap (after a seek).
    void reset();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    uint32_t rate_ = 0, channels_ = 0;
};

}  // namespace lgp::hle
