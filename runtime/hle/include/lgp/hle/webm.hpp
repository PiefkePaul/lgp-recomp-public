// WebM (Matroska subset) demuxer for in-game videos (P5, movie:: HLE).
// Written from the public Matroska/EBML specification (RFC 8794, RFC 9559),
// not from any game or SDK code.
//
// parse_webm reads the track list and the position, size and timestamp of
// every frame; the frame data stays in the source and is read on demand.
// Supported: EBML header with doctype "webm" or "matroska", Info
// (TimestampScale, Duration), Tracks (number, type, codec, codec private,
// default duration, video size, audio rate/channels), Clusters of known or
// unknown size with SimpleBlock and BlockGroup/Block, all three lacing
// kinds. Everything else (Cues, SeekHead, Tags, ...) is skipped.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace lgp::hle {

struct WebmTrack {
    uint64_t number = 0;
    uint64_t type = 0;  // 1 video, 2 audio
    std::string codec_id;  // "V_VP9", "A_VORBIS", ...
    std::vector<uint8_t> codec_private;
    uint64_t default_duration_ns = 0;
    uint32_t width = 0, height = 0;          // video
    double sampling_frequency = 0;           // audio
    uint32_t channels = 0;                   // audio
};

struct WebmFrame {
    uint64_t track = 0;    // track number
    int64_t pts_ns = 0;    // cluster timestamp + block offset, scaled
    uint64_t offset = 0;   // in the source
    uint32_t size = 0;
    bool keyframe = false;
};

struct WebmFile {
    uint64_t timestamp_scale_ns = 1000000;
    int64_t duration_ns = 0;  // 0 if the file has no Duration
    std::vector<WebmTrack> tracks;
    std::vector<WebmFrame> frames;  // in file order

    const WebmTrack* track(uint64_t number) const;
};

// Reads exactly len bytes at offset; false past the end or on an error.
using WebmRead = std::function<bool(uint64_t offset, void* dst, size_t len)>;

// size: bytes in the source. False with *error set if the file is not a
// readable WebM/Matroska file.
bool parse_webm(const WebmRead& read, uint64_t size, WebmFile* out, std::string* error);

// The same, a bit at a time: open() reads the header, Info and Tracks up to
// the first Cluster; every next() adds at most one block's frames to
// out->frames (or reads one other element). out->duration_ns is set once
// done(). For a player that indexes only as far as it plays, instead of
// reading the whole file before the first frame.
class WebmIndexer {
public:
    WebmIndexer();
    ~WebmIndexer();
    WebmIndexer(WebmIndexer&&) noexcept;
    WebmIndexer& operator=(WebmIndexer&&) noexcept;

    // out must outlive the indexer. False with *error set as parse_webm.
    bool open(WebmRead read, uint64_t size, WebmFile* out, std::string* error);
    // False with *error set on a broken element; the indexer is then done.
    bool next(std::string* error);
    bool done() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace lgp::hle
