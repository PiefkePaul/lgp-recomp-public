#include "movie_hle.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

#include "lgp/hle/romfs.hpp"
#include "lgp/hle/vorbis.hpp"
#include "lgp/hle/webm.hpp"
#include "lgp/mods/widescreen.hpp"
#include "lgp/platform/null_backends.hpp"
#include "lgp/runtime/log.hpp"
#include "lgp/runtime/time_stats.hpp"

namespace lgp::boot {
namespace {

using rt::LogLevel;

constexpr const char* kClassNames[MovieHle::kClasses] = {"Extractor", "Decoder"};
constexpr size_t kExtractor = 0, kDecoder = 1;

// movie:: status codes. 0x2000: no more data (the player checks for it after
// Extractor::GetSampleTrackIndex and DequeueOutputBuffer). The others are
// the HLE's own choice: the game only tells 0 and 0x2000 from "anything else".
constexpr uint32_t kStatusEndOfStream = 0x2000;
constexpr uint32_t kStatusTryAgainLater = 0x1003;  // no buffer available right now
constexpr uint32_t kStatusError = 0x1001;
// QueueInputBuffer / DequeueOutputBuffer flag for the last buffer.
constexpr uint32_t kFlagEndOfStream = 4;

// Decoder events as handed to SetEvents (the player waits on them).
constexpr size_t kEventInput = 0, kEventOutput = 1, kEventFormat = 2;

// Layout of the data page: the vtables, then strings handed to the game.
constexpr uint64_t kStringArea = MovieHle::kClasses * MovieHle::kSlots * 8;

// Logs the first calls of each function (unknown methods should not flood the log).
void log_call(const std::string& what, const CpuState* ctx, LogLevel level = LogLevel::Debug) {
    static std::mutex mutex;
    static std::map<std::string, int> counts;
    std::lock_guard lock(mutex);
    if (++counts[what] > 8)
        return;
    rt::log(level, "movie: %s(0x%" PRIx64 ", 0x%" PRIx64 ", 0x%" PRIx64 ", 0x%" PRIx64 ") from 0x%" PRIx64,
            what.c_str(), ctx->x[0], ctx->x[1], ctx->x[2], ctx->x[3], ctx->x[30]);
}

// --- State, keyed by the guest address of the game's object -------------------

using Value = std::variant<int64_t, std::string>;

struct BufferState {
    uint64_t base = 0;  // guest data
    uint64_t capacity = 0;
    int32_t offset = 0, size = 0;
    int64_t i64 = 0;
    uint32_t u32 = 0;
    // Input buffers of an HLE decoder hold their data on the host.
    std::vector<uint8_t>* host = nullptr;
};

// One decoded output: an NV12 picture or a block of interleaved 16-bit PCM.
struct Picture {
    platform::VideoPicture pic;  // audio: pic.nv12 holds the PCM bytes, size 0x0
    bool end = false;            // the end-of-stream marker after the last output
};

struct ExtractorState {
    std::shared_ptr<hle::DataSource> source;
    uint64_t base = 0;  // file offset in the source
    hle::WebmFile file;
    std::vector<uint64_t> tracks;    // exposed track numbers (video, and audio if enabled)
    std::vector<bool> selected;      // per exposed track
    std::vector<size_t> samples;     // frames of the selected tracks, in file order
    size_t cursor = 0;
    // Indexes file.frames as far as the player reads (index_ahead); null once
    // the whole file is indexed. Points into file: the map node stays put.
    std::shared_ptr<hle::WebmIndexer> index;
    std::string path;  // for the log
};

struct DecoderState {
    static constexpr int kInputs = 4, kOutputs = 4;
    static constexpr size_t kInputCapacity = 8u << 20;
    std::array<uint64_t, 4> events{};
    platform::VideoStreamInfo info;
    std::unique_ptr<platform::IVideoDecoder> decoder;
    std::unique_ptr<hle::VorbisDecoder> audio;  // set: this decoder plays a sound track
    bool input_ended = false;
    std::array<std::vector<uint8_t>, kInputs> input_data;
    std::deque<int> free_inputs;   // available to DequeueInputBuffer
    std::vector<int> held_inputs;  // returned once the decoded backlog shrinks
    std::deque<Picture> pending;   // decoded, waiting for an output buffer
    std::array<std::optional<Picture>, kOutputs> outputs;
    std::deque<int> free_outputs;
    std::deque<int> ready;  // filled, for DequeueOutputBuffer
    int64_t last_in_pts_us = -1, last_out_pts_us = -1;  // MovieHle::report
    bool format_sent = false;  // event [2] once, with the first output
};

std::mutex g_mutex;  // everything below
rt::Kernel* g_kernel = nullptr;
uint64_t g_data_va = 0;
platform::IStorage* g_game = nullptr;
platform::IVideo* g_video = nullptr;
std::atomic<uint32_t> g_video_starts{0}, g_video_ends{0};  // MovieHle::video_starts/ends
bool g_audio_tracks = false;  // offer sound tracks (MovieHle::configure)
platform::NullVideo g_null_video;
std::shared_ptr<hle::DataSource> g_romfs;  // romfs.bin, opened on first use
std::map<uint64_t, std::map<std::string, Value>> g_media;
// MediaData key the game never sees: the selected track's codec-private bytes.
constexpr const char* kCodecPrivateKey = "lgp-codec-private";
std::map<uint64_t, BufferState> g_buffers;
std::map<uint64_t, ExtractorState> g_extractors;
std::map<uint64_t, DecoderState> g_decoders;
std::map<std::string, uint64_t> g_strings;  // interned strings in the data page
uint64_t g_string_next = 0;
uint64_t g_alloc_next = 0;                       // bump pointer in the allocation pages
std::multimap<uint64_t, uint64_t> g_alloc_free;  // capacity -> va, from destroyed buffers
std::map<uint64_t, std::pair<uint64_t, uint64_t>> g_alloc_owned;  // buffer object -> {capacity, va} it owns

template <typename T>
void put(uint64_t va, const T& v) {
    if (va != 0)
        g_kernel->memory().copy_to_user(va, &v, sizeof(v));
}

uint64_t guest_string(const std::string& s) {
    auto it = g_strings.find(s);
    if (it != g_strings.end())
        return it->second;
    if (g_string_next == 0)
        g_string_next = g_data_va + kStringArea;
    const uint64_t va = g_string_next;
    if (va + s.size() + 1 > g_data_va + 0x1000)
        return 0;
    g_kernel->memory().copy_to_user(va, s.c_str(), s.size() + 1);
    g_string_next += (s.size() + 8) & ~uint64_t{7};
    g_strings[s] = va;
    return va;
}

std::string read_cstring(uint64_t va) {
    std::string s;
    for (char c; s.size() < 512 && g_kernel->memory().copy_from_user(&c, va + s.size(), 1) && c != 0;)
        s.push_back(c);
    return s;
}

// A game file ("rom:/bin/x.webm" or "/bin/x.webm"): romfs/<path> if the game
// storage has the tree, else the file inside romfs.bin.
std::shared_ptr<hle::DataSource> open_game_file(std::string path, uint64_t* base, uint64_t* size) {
    if (const size_t colon = path.find(":/"); colon != std::string::npos)
        path = path.substr(colon + 1);
    if (path.empty() || path[0] != '/')
        path = "/" + path;
    if (g_game == nullptr)
        return nullptr;
    std::unique_ptr<platform::IFile> f;
    if (g_game->open("/romfs" + path, platform::kOpenRead, &f) == platform::StorageError::Ok) {
        auto src = std::make_shared<hle::FileSource>(std::move(f));
        *base = 0;
        *size = src->size();
        return src;
    }
    if (!g_romfs && g_game->open("/romfs.bin", platform::kOpenRead, &f) == platform::StorageError::Ok) {
        std::string why;  // a compressed image gets its own block cache here
        g_romfs = hle::open_image_source(std::move(f), &why);
        if (!g_romfs)
            rt::log(rt::LogLevel::Error, "movie: /romfs.bin: %s", why.c_str());
    }
    if (g_romfs && hle::romfs_find(*g_romfs, path, base, size))
        return g_romfs;
    return nullptr;
}

std::string mime_of(const std::string& codec_id) {
    if (codec_id == "V_VP9")
        return "video/x-vnd.on2.vp9";
    if (codec_id == "V_MPEG4/ISO/AVC")
        return "video/avc";
    if (codec_id == "A_VORBIS")
        return "audio/vorbis";
    return "";
}

// Tracks the HLE hands to the game: video it can decode, Vorbis sound only
// when enabled and libvorbis is built in.
bool exposed(const hle::WebmTrack& t) {
    if (t.type == 1)
        return !mime_of(t.codec_id).empty();
    return t.type == 2 && g_audio_tracks && t.codec_id == "A_VORBIS" && hle::VorbisDecoder::available();
}

// --- Extractor --------------------------------------------------------------------

size_t exposed_index(const ExtractorState& x, uint64_t number);

// SetDataSource reads only the header; the frames are indexed while the
// player reads them (Shield, 2026-10-30: reading the whole 18.8 MiB title
// video before its first frame took 0.3-1.5 s with the file-backed guest
// block, all of it with the guest stopped). LGP_MOVIE_LAZY_INDEX=0 indexes the
// whole file in SetDataSource, as before.
bool lazy_index() {
    static const bool on = [] {
        const char* v = std::getenv("LGP_MOVIE_LAZY_INDEX");
        return !(v && *v == '0');
    }();
    return on;
}

// Index until `ahead` samples of the selected tracks follow the cursor (or
// the file ends). Each step reads at most one block header, mostly from the
// 64 KiB window. Nothing selected: nothing to index for.
constexpr size_t kIndexAhead = 8;

void index_ahead(ExtractorState& x, size_t ahead = kIndexAhead) {
    if (!x.index)
        return;
    const bool all = ahead == SIZE_MAX;
    if (!all && std::find(x.selected.begin(), x.selected.end(), true) == x.selected.end())
        return;
    std::string error;
    while (!x.index->done() && (all || x.samples.size() - std::min(x.samples.size(), x.cursor) < ahead)) {
        const size_t before = x.file.frames.size();
        if (!x.index->next(&error)) {
            rt::log(LogLevel::Warn, "movie: %s: %s, the movie ends here", x.path.c_str(), error.c_str());
            break;
        }
        for (size_t i = before; i < x.file.frames.size(); ++i) {
            const size_t t = exposed_index(x, x.file.frames[i].track);
            if (t < x.tracks.size() && x.selected[t])
                x.samples.push_back(i);
        }
    }
    if (x.index->done()) {
        if (!all)
            rt::log(LogLevel::Debug, "movie: %s indexed to the end: %zu frames", x.path.c_str(), x.file.frames.size());
        x.index.reset();
    }
}


uint32_t extractor_open(ExtractorState& x, const std::string& path) {
    uint64_t base = 0, size = 0;
    const auto start = std::chrono::steady_clock::now();
    x = ExtractorState{};
    rt::log(LogLevel::Debug, "movie: SetDataSource(\"%s\")", path.c_str());
    x.source = open_game_file(path, &base, &size);
    if (!x.source) {
        rt::log(LogLevel::Error, "movie: %s not found in the game's RomFS", path.c_str());
        return kStatusError;
    }
    x.base = base;
    const auto opened = std::chrono::steady_clock::now();
    std::string error;
    // The parser walks every block header with reads of a few bytes, spread
    // over the whole file; they go through a 64 KiB window, so the source
    // sees one large sequential read per window (Shield, 2026-10-29: the
    // title video's SetDataSource held the game for ≈ 4 s).
    struct Window {
        std::shared_ptr<hle::DataSource> src;
        uint64_t base = 0, size = 0, at = 0, len = 0, reads = 0, bytes = 0;
        std::vector<uint8_t> data;
        bool read(uint64_t off, void* dst, size_t n) {
            if (off >= at && n <= len && off - at <= len - n) {
                std::memcpy(dst, data.data() + (off - at), n);
                return true;
            }
            ++reads;
            bytes += n;
            if (n >= data.size() || off >= size || n > size - off)
                return src->read(base + off, dst, n);
            len = std::min<uint64_t>(data.size(), size - off);
            at = off;
            if (!src->read(base + at, data.data(), len)) {
                len = 0;
                return false;
            }
            bytes += len - n;
            std::memcpy(dst, data.data(), n);
            return true;
        }
    };
    auto window = std::make_shared<Window>();
    window->src = x.source;
    window->base = base;
    window->size = size;
    window->data.resize(64 << 10);
    const hle::WebmRead read = [window](uint64_t off, void* dst, size_t len) { return window->read(off, dst, len); };
    auto index = std::make_shared<hle::WebmIndexer>();
    if (!index->open(read, size, &x.file, &error)) {
        rt::log(LogLevel::Error, "movie: %s: %s", path.c_str(), error.c_str());
        return kStatusError;
    }
    x.path = path;
    x.index = std::move(index);
    if (!lazy_index())
        index_ahead(x, SIZE_MAX);
    const auto ms = [](auto a, auto b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
    rt::log(LogLevel::Info, "movie: %s %s in %.1f ms (%llu reads, %.1f MiB) after %.1f ms to find it", path.c_str(),
            x.index ? "header read" : "indexed", ms(opened, std::chrono::steady_clock::now()),
            (unsigned long long)window->reads, double(window->bytes) / 1048576.0, ms(start, opened));
    for (const hle::WebmTrack& t : x.file.tracks)
        if (exposed(t))
            x.tracks.push_back(t.number);
    x.selected.assign(x.tracks.size(), false);
    rt::log(LogLevel::Info, "movie: opened %s: %zu tracks (%zu offered), %zu frames%s, %.2f s", path.c_str(),
            x.file.tracks.size(), x.tracks.size(), x.file.frames.size(), x.index ? " so far" : "",
            double(x.file.duration_ns) / 1e9);
    return 0;
}

// Index of a track number among the exposed tracks (tracks.size() if none).
size_t exposed_index(const ExtractorState& x, uint64_t number) {
    return size_t(std::find(x.tracks.begin(), x.tracks.end(), number) - x.tracks.begin());
}

// SelectTrack adds a track (Android's MediaExtractor): the samples of all
// selected tracks come out interleaved in file order, GetSampleTrackIndex
// says which track the current one belongs to. Selecting restarts at the
// beginning (the player selects before reading).
void extractor_select(ExtractorState& x, size_t index) {
    if (index >= x.tracks.size())
        return;
    x.selected[index] = true;
    x.samples.clear();
    x.cursor = 0;
    for (size_t i = 0; i < x.file.frames.size(); ++i) {
        const size_t t = exposed_index(x, x.file.frames[i].track);
        if (t < x.tracks.size() && x.selected[t])
            x.samples.push_back(i);
    }
}

// Track format as Android MediaFormat keys.
void fill_track_format(const ExtractorState& x, size_t index, uint64_t media) {
    auto& m = g_media[media];
    if (index >= x.tracks.size())
        return;
    const hle::WebmTrack* t = x.file.track(x.tracks[index]);
    m["mime"] = mime_of(t->codec_id);
    if (t->type == 2) {
        m["sample-rate"] = int64_t(t->sampling_frequency);
        m["channel-count"] = int64_t{t->channels};
    } else {
        m["width"] = int64_t{t->width};
        m["height"] = int64_t{t->height};
    }
    m["durationUs"] = x.file.duration_ns / 1000;
    // Host-only key: the container's codec data (avcC for H.264) for the decoder.
    m[kCodecPrivateKey] = std::string(t->codec_private.begin(), t->codec_private.end());
}

bool extractor_virtual(uint64_t self, size_t slot, CpuState* ctx) {
    ExtractorState& x = g_extractors[self];
    if (slot > 1)  // not for the destructors
        index_ahead(x);
    const bool at_end = x.cursor >= x.samples.size();
    switch (slot) {
    case 0:
    case 1:  // destructors
        g_extractors.erase(self);
        ctx->x[0] = 0;
        return true;
    case 4:  // GetTrackCount(int* count)
        put(ctx->x[1], int32_t(x.tracks.size()));
        ctx->x[0] = 0;
        return true;
    case 5:  // GetTrackConfiguration(int index, MediaData* format)
        fill_track_format(x, size_t(ctx->x[1]), ctx->x[2]);
        ctx->x[0] = ctx->x[1] < x.tracks.size() ? 0 : kStatusError;
        return true;
    case 6:  // SelectTrack(int index)
        extractor_select(x, size_t(ctx->x[1]));
        ctx->x[0] = 0;
        return true;
    case 9:  // GetSampleTrackIndex(int64_t* index)
        if (at_end) {
            ctx->x[0] = kStatusEndOfStream;
        } else {
            put(ctx->x[1], int64_t(exposed_index(x, x.file.frames[x.samples[x.cursor]].track)));
            ctx->x[0] = 0;
        }
        return true;
    case 10: {  // ReadSampleData(Buffer* buffer)
        auto b = g_buffers.find(ctx->x[1]);
        if (at_end || b == g_buffers.end()) {
            ctx->x[0] = kStatusEndOfStream;
            return true;
        }
        const hle::WebmFrame& f = x.file.frames[x.samples[x.cursor]];
        std::vector<uint8_t> local;
        std::vector<uint8_t>& data = b->second.host != nullptr ? *b->second.host : local;
        data.resize(f.size);
        if (!x.source->read(x.base + f.offset, data.data(), f.size)) {
            ctx->x[0] = kStatusError;
            return true;
        }
        if (b->second.host == nullptr) {
            if (f.size > b->second.capacity) {
                ctx->x[0] = kStatusError;
                return true;
            }
            g_kernel->memory().copy_to_user(b->second.base, data.data(), f.size);
        }
        b->second.offset = 0;
        b->second.size = int32_t(f.size);
        ctx->x[0] = 0;
        return true;
    }
    case 11:  // Advance()
        if (!at_end)
            ++x.cursor;
        ctx->x[0] = at_end ? kStatusEndOfStream : 0;
        return true;
    case 12:  // GetSampleTime(int64_t* us)
        put(ctx->x[1], at_end ? int64_t{-1} : x.file.frames[x.samples[x.cursor]].pts_ns / 1000);
        ctx->x[0] = at_end ? kStatusEndOfStream : 0;
        return true;
    case 15:  // SetDataSource(reader, const char* path): the game's file reader object is
              // not used, the HLE reads the file ("data:/bin/...") from the RomFS itself
        ctx->x[0] = extractor_open(x, read_cstring(ctx->x[2]));
        return true;
    default:
        return false;
    }
}

// --- Decoder ----------------------------------------------------------------------

// Hands decoded pictures to free output buffers; returns true if one became ready.
bool fill_outputs(DecoderState& d) {
    bool any = false;
    while (!d.pending.empty() && !d.free_outputs.empty()) {
        const int i = d.free_outputs.front();
        d.free_outputs.pop_front();
        d.outputs[size_t(i)] = std::move(d.pending.front());
        d.pending.pop_front();
        d.ready.push_back(i);
        any = true;
    }
    return any;
}

// Returns held input buffers once few pictures wait (back pressure: the
// game's feeder runs as fast as buffers come back). The sound decoder may run
// further ahead: the player takes no sound before the first picture, and a
// hardware video decoder (MediaCodec) hands that out only some frames after
// its input. With the sound inputs held, the feeder stopped at the next sound
// sample, the video decoder got no more input, and the start of a movie hung
// (Shield: 10 s without a present). The video still holds the feeder
// back, so the sound runs ahead by at most the interleave gap.
constexpr size_t kVideoBacklog = 2, kSoundBacklog = 64;

bool release_inputs(DecoderState& d) {
    if (d.input_ended || d.pending.size() >= (d.audio ? kSoundBacklog : kVideoBacklog) || d.held_inputs.empty())
        return false;
    for (int i : d.held_inputs)
        d.free_inputs.push_back(i);
    d.held_inputs.clear();
    return true;
}

// A sound packet to one PCM output (none while the codec fills its overlap).
void decode_audio(DecoderState& d, const uint8_t* data, uint32_t size, int64_t pts) {
    std::vector<int16_t> pcm;
    if (d.audio->channels() == 0)
        return;  // the headers were refused (logged once in Configure)
    if (!d.audio->decode(data, size, &pcm))
        rt::log(LogLevel::Warn, "movie: decoding the sound packet at %" PRId64 " us failed", pts);
    if (pcm.empty())
        return;
    Picture out;
    out.pic.pts_us = pts;
    out.pic.nv12.resize(pcm.size() * sizeof(int16_t));
    std::memcpy(out.pic.nv12.data(), pcm.data(), out.pic.nv12.size());
    d.pending.push_back(std::move(out));
}

void decode_input(DecoderState& d, int index, uint32_t offset, uint32_t size, int64_t pts, uint32_t flags) {
    std::vector<platform::VideoPicture> pics;
    const std::vector<uint8_t>& data = d.input_data[size_t(index)];
    if (d.audio && size > 0 && uint64_t{offset} + size <= data.size())
        decode_audio(d, data.data() + offset, size, pts);
    else if (d.decoder && size > 0 && uint64_t{offset} + size <= data.size())
        if (!d.decoder->decode(data.data() + offset, size, pts, &pics))
            rt::log(LogLevel::Warn, "movie: decoding the frame at %" PRId64 " us failed", pts);
    if (flags & kFlagEndOfStream) {
        if (d.decoder)
            d.decoder->decode(nullptr, 0, pts, &pics);
        d.input_ended = true;
    }
    for (auto& p : pics) {
        // Widescreen mod: a game that stretches its video over the widened picture.
        mods::widescreen_movie_frame(p.nv12.data(), p.nv12.size(), p.width != 0 ? p.width : d.info.width,
                                     p.height != 0 ? p.height : d.info.height);
        d.pending.push_back({std::move(p), false});
    }
    if (flags & kFlagEndOfStream) {
        Picture end;
        end.end = true;
        end.pic.pts_us = pts;
        d.pending.push_back(std::move(end));
    }
}

void open_decoder(DecoderState& d, uint64_t media) {
    d.info = platform::VideoStreamInfo{};
    d.decoder.reset();
    d.audio.reset();
    auto m = g_media.find(media);
    if (m != g_media.end()) {
        auto mime = m->second.find("mime");
        if (mime != m->second.end() && std::holds_alternative<std::string>(mime->second) &&
            std::get<std::string>(mime->second).rfind("audio/", 0) == 0) {
            auto cp = m->second.find(kCodecPrivateKey);
            std::vector<uint8_t> headers;
            if (cp != m->second.end() && std::holds_alternative<std::string>(cp->second))
                headers.assign(std::get<std::string>(cp->second).begin(), std::get<std::string>(cp->second).end());
            d.audio = std::make_unique<hle::VorbisDecoder>();
            std::string error;
            if (!d.audio->open(headers, &error))
                rt::log(LogLevel::Warn, "movie: sound track: %s; decoding to nothing", error.c_str());
            return;
        }
    }
    if (m != g_media.end()) {
        auto get = [&](const char* key) -> int64_t {
            auto v = m->second.find(key);
            return v != m->second.end() && std::holds_alternative<int64_t>(v->second) ? std::get<int64_t>(v->second) : 0;
        };
        d.info.width = uint32_t(get("width"));
        d.info.height = uint32_t(get("height"));
        auto mime = m->second.find("mime");
        if (mime != m->second.end() && std::holds_alternative<std::string>(mime->second) &&
            std::get<std::string>(mime->second) == "video/avc")
            d.info.codec = platform::VideoCodec::H264;
        auto cp = m->second.find(kCodecPrivateKey);
        if (cp != m->second.end() && std::holds_alternative<std::string>(cp->second)) {
            const std::string& bytes = std::get<std::string>(cp->second);
            d.info.codec_private.assign(bytes.begin(), bytes.end());
        }
    }
    platform::IVideo* video = g_video != nullptr ? g_video : &g_null_video;
    d.decoder = video->open_decoder(d.info);
    if (!d.decoder) {
        rt::log(LogLevel::Warn, "movie: the video backend cannot decode this stream, playing black pictures");
        d.decoder = g_null_video.open_decoder(d.info);
    }
}

void start_decoder(DecoderState& d) {
    d.input_ended = false;
    d.free_inputs.clear();
    d.held_inputs.clear();
    d.pending.clear();
    d.ready.clear();
    d.free_outputs.clear();
    for (int i = 0; i < DecoderState::kInputs; ++i)
        d.free_inputs.push_back(i);
    for (int i = 0; i < DecoderState::kOutputs; ++i) {
        d.outputs[size_t(i)].reset();
        d.free_outputs.push_back(i);
    }
}

void output_format(const DecoderState& d, const platform::VideoPicture* pic, uint64_t media) {
    auto& m = g_media[media];
    if (d.audio) {
        // Interleaved signed 16-bit PCM (Android: pcm-encoding 2), as Vorbis decodes it.
        m["mime"] = std::string("audio/raw");
        m["sample-rate"] = int64_t{d.audio->sample_rate()};
        m["channel-count"] = int64_t{d.audio->channels()};
        m["pcm-encoding"] = int64_t{2};
        return;
    }
    const uint32_t w = pic != nullptr && pic->width != 0 ? pic->width : d.info.width;
    const uint32_t h = pic != nullptr && pic->height != 0 ? pic->height : d.info.height;
    m["width"] = int64_t{w};
    m["height"] = int64_t{h};
    m["nv12-y-stride"] = int64_t{w};
    m["nv12-y-offset"] = int64_t{0};
    m["nv12-uv-offset"] = int64_t{w} * h;
}

// Event [2] tells the player "output format known" (MediaCodec's format change).
// Player::Start waits up to 5 s until both of its decoders have sent it, then
// reads GetOutputFormat. Sent once per decoder, with its first output.
// LGP_MOVIE_FORMAT_EVENT=0 leaves it out (for comparison: the start then waits
// out the 5 s).
bool format_event_enabled() {
    static const bool on = [] {
        const char* v = std::getenv("LGP_MOVIE_FORMAT_EVENT");
        return !(v && *v == '0');
    }();
    return on;
}

// signal: events to signal after the lock is dropped.
bool decoder_virtual(uint64_t self, size_t slot, CpuState* ctx, std::vector<uint64_t>* signal) {
    DecoderState& d = g_decoders[self];
    auto notify = [&](size_t which) {
        if (d.events[which] != 0)
            signal->push_back(d.events[which]);
        if (which == kEventOutput && !d.format_sent && format_event_enabled()) {
            d.format_sent = true;
            if (d.events[kEventFormat] != 0)
                signal->push_back(d.events[kEventFormat]);
        }
    };
    switch (slot) {
    case 0:
    case 1:  // destructors
        g_decoders.erase(self);
        ctx->x[0] = 0;
        return true;
    case 2:  // Configure(MediaData* format)
        open_decoder(d, ctx->x[1]);
        ctx->x[0] = 0;
        return true;
    case 3:  // SetEvents(nn::os::EventType* events[4])
        g_kernel->memory().copy_from_user(d.events.data(), ctx->x[1], sizeof(d.events));
        ctx->x[0] = 0;
        return true;
    case 4:  // GetOutputFormat(MediaData* format)
        output_format(d, nullptr, ctx->x[1]);
        ctx->x[0] = 0;
        return true;
    case 5:  // Start()
        start_decoder(d);
        if (!d.audio)
            ++g_video_starts;
        notify(kEventInput);
        ctx->x[0] = 0;
        return true;
    case 9:  // DequeueInputBuffer(int* index, int* remaining)
        if (d.free_inputs.empty()) {
            ctx->x[0] = kStatusTryAgainLater;
            return true;
        }
        put(ctx->x[1], int32_t(d.free_inputs.front()));
        d.free_inputs.pop_front();
        put(ctx->x[2], int32_t(d.free_inputs.size()));
        ctx->x[0] = 0;
        return true;
    case 10: {  // GetInputBuffer(int index, Buffer* buffer)
        const uint32_t i = uint32_t(ctx->x[1]);
        if (i >= uint32_t(DecoderState::kInputs)) {
            ctx->x[0] = kStatusError;
            return true;
        }
        BufferState& b = g_buffers[ctx->x[2]];
        b = BufferState{};
        b.host = &d.input_data[i];
        b.capacity = DecoderState::kInputCapacity;
        ctx->x[0] = 0;
        return true;
    }
    case 11: {  // QueueInputBuffer(int index, int offset, int size, int64_t pts, uint32_t flags)
        const uint32_t i = uint32_t(ctx->x[1]);
        if (i >= uint32_t(DecoderState::kInputs)) {
            ctx->x[0] = kStatusError;
            return true;
        }
        decode_input(d, int(i), uint32_t(ctx->x[2]), uint32_t(ctx->x[3]), int64_t(ctx->x[4]), uint32_t(ctx->x[5]));
        d.last_in_pts_us = int64_t(ctx->x[4]);
        d.held_inputs.push_back(int(i));
        if (release_inputs(d))
            notify(kEventInput);
        if (fill_outputs(d))
            notify(kEventOutput);
        ctx->x[0] = 0;
        return true;
    }
    case 12: {  // DequeueOutputBuffer(int* index, int64_t* pts, uint32_t* flags, int* remaining)
        if (d.ready.empty()) {
            ctx->x[0] = kStatusTryAgainLater;
            return true;
        }
        const int i = d.ready.front();
        d.ready.pop_front();
        const Picture& p = *d.outputs[size_t(i)];
        put(ctx->x[1], int32_t(i));
        put(ctx->x[2], p.pic.pts_us);
        d.last_out_pts_us = p.pic.pts_us;
        put(ctx->x[3], uint32_t(p.end ? kFlagEndOfStream : 0));
        if (p.end && !d.audio)
            ++g_video_ends;
        put(ctx->x[4], int32_t(d.ready.size()));
        ctx->x[0] = 0;
        return true;
    }
    case 13: {  // GetOutputBuffer(int index, Buffer* buffer): copies the picture
        const uint32_t i = uint32_t(ctx->x[1]);
        auto b = g_buffers.find(ctx->x[2]);
        if (i >= uint32_t(DecoderState::kOutputs) || !d.outputs[i] || b == g_buffers.end()) {
            ctx->x[0] = kStatusError;
            return true;
        }
        const std::vector<uint8_t>& nv12 = d.outputs[i]->pic.nv12;  // or PCM
        const uint64_t n = std::min<uint64_t>(nv12.size(), b->second.capacity);
        if (n < nv12.size())
            rt::log(LogLevel::Warn, "movie: output %u: %zu bytes do not fit in %" PRIu64, i, nv12.size(),
                    b->second.capacity);
        if (n > 0 && b->second.base != 0)
            g_kernel->memory().copy_to_user(b->second.base, nv12.data(), n);
        b->second.offset = 0;
        b->second.size = int32_t(n);
        ctx->x[0] = 0;
        return true;
    }
    case 15: {  // ReleaseOutputBuffer(int index)
        const uint32_t i = uint32_t(ctx->x[1]);
        if (i < uint32_t(DecoderState::kOutputs) && d.outputs[i]) {
            d.outputs[i].reset();
            d.free_outputs.push_back(int(i));
        }
        if (fill_outputs(d))
            notify(kEventOutput);
        if (release_inputs(d))
            notify(kEventInput);
        ctx->x[0] = 0;
        return true;
    }
    case 18: {  // GetOutputBufferFormat(int index, MediaData* format)
        const uint32_t i = uint32_t(ctx->x[1]);
        const platform::VideoPicture* pic =
            i < uint32_t(DecoderState::kOutputs) && d.outputs[i] ? &d.outputs[i]->pic : nullptr;
        output_format(d, pic, ctx->x[2]);
        ctx->x[0] = 0;
        return true;
    }
    default:
        return false;
    }
}

// Calls that take longer than this are logged, with the time spent waiting for g_mutex
// apart from the work itself (the first kSlowCallLogs of them).
constexpr double kSlowCallMs = 50;
constexpr uint32_t kSlowCallLogs = 64;
std::atomic<uint32_t> g_slow_calls{0};

using Clock = std::chrono::steady_clock;

void note_slow_call(const std::string& what, Clock::time_point start, Clock::time_point locked,
                    Clock::time_point done) {
    const auto ms = [](Clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); };
    if (ms(done - start) < kSlowCallMs || g_slow_calls.fetch_add(1) >= kSlowCallLogs)
        return;
    rt::log(LogLevel::Info, "movie: slow call %s took %.1f ms (%.1f ms waiting for the lock)", what.c_str(),
            ms(done - start), ms(locked - start));
}

template <size_t I>
void virtual_method(CpuState* ctx) {
    const size_t cls = I / MovieHle::kSlots, slot = I % MovieHle::kSlots;
    const std::string what = std::string(kClassNames[cls]) + ":" + std::to_string(slot);
    std::vector<uint64_t> signal;
    bool known;
    {
        const rt::TimeScope scope(rt::TimeCat::Movie);  // includes waiting for g_mutex
        const auto start = Clock::now();
        std::lock_guard lock(g_mutex);
        const auto locked = Clock::now();
        known = cls == kDecoder ? decoder_virtual(ctx->x[0], slot, ctx, &signal)
                                : extractor_virtual(ctx->x[0], slot, ctx);
        note_slow_call(what, start, locked, Clock::now());
    }
    log_call(what, ctx, known ? LogLevel::Debug : LogLevel::Warn);
    if (!known)
        ctx->x[0] = 0;
    for (const uint64_t e : signal)
        MovieHle::instance().signal_event(ctx, e);
}

template <size_t... I>
constexpr std::array<LiftedFn, sizeof...(I)> make_methods(std::index_sequence<I...>) {
    return {&virtual_method<I>...};
}
constexpr auto kMethods = make_methods(std::make_index_sequence<MovieHle::kClasses * MovieHle::kSlots>{});

// --- Plain functions (Buffer, MediaData, settings) ---------------------------------

using Fn = void (*)(CpuState*);

void buffer_ctor(CpuState* c) { g_buffers[c->x[0]] = BufferState{}; }
void buffer_ctor_data(CpuState* c) {  // Buffer(void* data, size_t capacity)
    BufferState b;
    b.base = c->x[1];
    b.capacity = c->x[2];
    g_buffers[c->x[0]] = b;
}
// Guest memory for Buffer(size_t): the allocation pages behind the data page,
// reused by capacity once a buffer is destroyed. 0 when they are used up.
uint64_t alloc_guest(uint64_t capacity) {
    capacity = (capacity + 15) & ~uint64_t{15};
    if (auto it = g_alloc_free.find(capacity); it != g_alloc_free.end()) {
        const uint64_t va = it->second;
        g_alloc_free.erase(it);
        return va;
    }
    const uint64_t begin = g_data_va + 0x1000, end = begin + MovieHle::kAllocPages * 0x1000;
    if (g_alloc_next == 0)
        g_alloc_next = begin;
    if (capacity == 0 || g_alloc_next + capacity > end)
        return 0;
    const uint64_t va = g_alloc_next;
    g_alloc_next += capacity;
    return va;
}
void buffer_ctor_capacity(CpuState* c) {  // Buffer(size_t capacity): the library allocates (LGP: the sound output)
    BufferState b;
    b.base = alloc_guest(c->x[1]);
    if (b.base == 0)
        rt::log(LogLevel::Warn, "movie: Buffer(size_t 0x%" PRIx64 "): out of HLE memory", c->x[1]);
    else {
        b.capacity = c->x[1];
        g_alloc_owned[c->x[0]] = {(c->x[1] + 15) & ~uint64_t{15}, b.base};
    }
    g_buffers[c->x[0]] = b;
}
// Storage a Buffer(size_t) object owns: on its destruction (or when it is
// assigned another buffer) it passes to a copy that still uses the same
// memory, else it is free for reuse.
void release_owned(uint64_t obj) {
    auto own = g_alloc_owned.find(obj);
    if (own == g_alloc_owned.end())
        return;
    const auto alloc = own->second;
    g_alloc_owned.erase(own);
    for (const auto& [other, b] : g_buffers)
        if (other != obj && b.base == alloc.second && !g_alloc_owned.count(other)) {
            g_alloc_owned[other] = alloc;
            return;
        }
    g_alloc_free.insert(alloc);
}
void buffer_copy(CpuState* c) {  // Buffer(const Buffer&), operator=
    if (c->x[0] == c->x[1])
        return;
    auto it = g_buffers.find(c->x[1]);
    const BufferState src = it != g_buffers.end() ? it->second : BufferState{};
    if (src.base != 0 && g_buffers.count(c->x[0]) && g_buffers[c->x[0]].base == src.base) {
        g_buffers[c->x[0]] = src;  // same storage already: keep its ownership
        return;
    }
    g_buffers[c->x[0]] = src;
    release_owned(c->x[0]);
}
void buffer_dtor(CpuState* c) {
    g_buffers.erase(c->x[0]);
    release_owned(c->x[0]);
}
void buffer_base(CpuState* c) { c->x[0] = g_buffers[c->x[0]].base; }
void buffer_size(CpuState* c) { c->x[0] = uint64_t(int64_t(g_buffers[c->x[0]].size)); }
void buffer_offset(CpuState* c) { c->x[0] = uint64_t(int64_t(g_buffers[c->x[0]].offset)); }
void buffer_capacity(CpuState* c) { c->x[0] = g_buffers[c->x[0]].capacity; }
void buffer_set_range(CpuState* c) {
    BufferState& b = g_buffers[c->x[0]];
    b.offset = int32_t(c->x[1]);
    b.size = int32_t(c->x[2]);
}
void buffer_set_data(CpuState* c) {
    BufferState& b = g_buffers[c->x[0]];
    b.base = c->x[1];
    b.capacity = c->x[2];
    b.host = nullptr;
}
void buffer_set_i32(CpuState* c) { g_buffers[c->x[0]].i64 = int32_t(c->x[1]); }
void buffer_set_i64(CpuState* c) { g_buffers[c->x[0]].i64 = int64_t(c->x[1]); }
void buffer_set_u32(CpuState* c) { g_buffers[c->x[0]].u32 = uint32_t(c->x[1]); }
void buffer_get_i64(CpuState* c) { c->x[0] = uint64_t(g_buffers[c->x[0]].i64); }
void buffer_get_u32(CpuState* c) { c->x[0] = g_buffers[c->x[0]].u32; }

void media_clear(CpuState* c) { g_media[c->x[0]].clear(); }
void media_dtor(CpuState* c) { g_media.erase(c->x[0]); }

template <int Kind>  // 0 string, 1 int32, 2 int64
void media_find(CpuState* c) {
    const std::string key = read_cstring(c->x[1]);
    bool found = false;
    auto m = g_media.find(c->x[0]);
    if (m != g_media.end())
        if (auto v = m->second.find(key); v != m->second.end()) {
            if (Kind == 0 && std::holds_alternative<std::string>(v->second)) {
                const uint64_t va = guest_string(std::get<std::string>(v->second));
                put(c->x[2], va);
                found = va != 0;
            } else if (Kind != 0 && std::holds_alternative<int64_t>(v->second)) {
                const int64_t value = std::get<int64_t>(v->second);
                if (Kind == 1)
                    put(c->x[2], int32_t(value));
                else
                    put(c->x[2], value);
                found = true;
            }
        }
    c->x[0] = found ? 1 : 0;
}

void returns_zero(CpuState* c) { c->x[0] = 0; }

const std::map<std::string, Fn>& plain_functions() {
    static const std::map<std::string, Fn> fns = {
        {"_ZN5movie6BufferC1Ev", &buffer_ctor},
        {"_ZN5movie6BufferC1EPvm", &buffer_ctor_data},
        {"_ZN5movie6BufferC1Em", &buffer_ctor_capacity},
        {"_ZN5movie6BufferC1ERKS0_", &buffer_copy},
        {"_ZN5movie6BufferaSERKS0_", &buffer_copy},
        {"_ZN5movie6BufferD1Ev", &buffer_dtor},
        {"_ZNK5movie6Buffer4BaseEv", &buffer_base},
        {"_ZNK5movie6Buffer4SizeEv", &buffer_size},
        {"_ZNK5movie6Buffer6OffsetEv", &buffer_offset},
        {"_ZNK5movie6Buffer8CapacityEv", &buffer_capacity},
        {"_ZN5movie6Buffer8SetRangeEii", &buffer_set_range},
        {"_ZN5movie6Buffer18SetDataAndCapacityEPvm", &buffer_set_data},
        {"_ZN5movie6Buffer12SetInt32DataEi", &buffer_set_i32},
        {"_ZN5movie6Buffer12SetInt64DataEl", &buffer_set_i64},
        {"_ZN5movie6Buffer13SetUint32DataEj", &buffer_set_u32},
        {"_ZNK5movie6Buffer12GetInt64DataEv", &buffer_get_i64},
        {"_ZNK5movie6Buffer13GetUint32DataEv", &buffer_get_u32},
        {"_ZN5movie9MediaDataC1Ev", &media_clear},
        {"_ZN5movie9MediaData5ClearEv", &media_clear},
        {"_ZN5movie9MediaDataD1Ev", &media_dtor},
        {"_ZNK5movie9MediaData10FindStringEPKcPS2_", &media_find<0>},
        {"_ZNK5movie9MediaData9FindInt32EPKcPi", &media_find<1>},
        {"_ZNK5movie9MediaData9FindInt64EPKcPl", &media_find<2>},
        // Settings the HLE does not need: the host decides threads and memory.
        {"_ZN5movie11SetCoreMaskEm", &returns_zero},
        {"_ZN5movie12SetAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_", &returns_zero},
        {"_ZN5movie17GetThreadPriorityEv", &returns_zero},
    };
    return fns;
}

}  // namespace

uint32_t MovieHle::video_starts() { return g_video_starts.load(); }
uint32_t MovieHle::video_ends() { return g_video_ends.load(); }

std::vector<std::string> MovieHle::report() {
    std::vector<std::string> out;
    std::unique_lock lock(g_mutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        out.emplace_back("movie: busy (lock held)");
        return out;
    }
    char line[256];
    for (const auto& [self, x] : g_extractors) {
        const bool at_end = x.cursor >= x.samples.size();
        std::snprintf(line, sizeof(line), "movie extractor 0x%" PRIx64 ": sample %zu of %zu, next track %s", self,
                      x.cursor, x.samples.size(),
                      at_end ? "-" : std::to_string(x.file.frames[x.samples[x.cursor]].track).c_str());
        out.push_back(line);
    }
    for (const auto& [self, d] : g_decoders) {
        size_t held_outputs = 0;  // dequeued by the game, not yet released
        for (const auto& o : d.outputs)
            held_outputs += o.has_value() ? 1 : 0;
        held_outputs -= std::min(held_outputs, d.ready.size());
        std::snprintf(line, sizeof(line),
                      "movie decoder 0x%" PRIx64 ": %s, inputs %zu free %zu held, %zu decoded waiting, outputs %zu "
                      "ready %zu with the game %zu free, last pts in %" PRId64 " out %" PRId64 " us%s",
                      self, d.audio ? "sound" : "video", d.free_inputs.size(), d.held_inputs.size(), d.pending.size(),
                      d.ready.size(), held_outputs, d.free_outputs.size(), d.last_in_pts_us, d.last_out_pts_us,
                      d.input_ended ? ", input ended" : "");
        out.push_back(line);
    }
    return out;
}

MovieHle& MovieHle::instance() {
    static MovieHle hle;
    return hle;
}

void MovieHle::signal_event(CpuState* ctx, uint64_t event) {
    if (!call_ || signal_event_va_ == 0 || event == 0)
        return;
    const uint64_t x0 = ctx->x[0];
    ctx->x[0] = event;
    call_(ctx, signal_event_va_);
    ctx->x[0] = x0;
}

void MovieHle::configure(platform::IStorage* game, platform::IVideo* video, bool audio_tracks) {
    std::lock_guard lock(g_mutex);
    g_game = game;
    g_video = video;
    g_audio_tracks = audio_tracks;
    g_romfs.reset();
}

void MovieHle::release() {
    std::lock_guard lock(g_mutex);
    g_extractors.clear();
    g_decoders.clear();
    g_buffers.clear();
    g_media.clear();
    g_romfs.reset();
    g_game = nullptr;
}

void MovieHle::reset(rt::Kernel& kernel, uint64_t data_va, uint64_t vthunk_base, std::vector<LiftedEntry>* host) {
    std::lock_guard lock(g_mutex);
    g_kernel = &kernel;
    g_data_va = data_va;
    g_media.clear();
    g_buffers.clear();
    g_extractors.clear();
    g_decoders.clear();
    g_strings.clear();
    g_string_next = 0;
    g_alloc_next = 0;
    g_alloc_free.clear();
    g_alloc_owned.clear();
    // Vtable c at data_va + c * kSlots * 8: entry k = thunk key of (c, k).
    for (size_t c = 0; c < kClasses; ++c)
        for (size_t k = 0; k < kSlots; ++k) {
            const size_t i = c * kSlots + k;
            const uint64_t key = vthunk_base + 4 * i;
            kernel.memory().copy_to_user(data_va + 8 * i, &key, sizeof(key));
            host->push_back({key, kMethods[i]});
        }
}

MovieHandler MovieHle::handler(const std::string& name) {
    if (name.rfind("_ZN5movie", 0) != 0 && name.rfind("_ZNK5movie", 0) != 0)
        return {};
    for (size_t c = 0; c < kClasses; ++c) {
        const std::string ctor = "_ZN5movie" + std::to_string(std::char_traits<char>::length(kClassNames[c])) +
                                 kClassNames[c] + "C1";
        if (name.rfind(ctor, 0) == 0)
            return [name, c](CpuState* ctx) {
                log_call(name, ctx);
                std::lock_guard lock(g_mutex);
                const uint64_t vtable = g_data_va + 8 * c * kSlots;
                g_kernel->memory().copy_to_user(ctx->x[0], &vtable, sizeof(vtable));
                if (c == kExtractor)
                    g_extractors[ctx->x[0]] = ExtractorState{};
                else
                    g_decoders[ctx->x[0]] = DecoderState{};
            };
    }
    const auto& fns = plain_functions();
    if (auto it = fns.find(name); it != fns.end()) {
        const Fn fn = it->second;
        return [name, fn](CpuState* ctx) {
            log_call(name, ctx);
            const rt::TimeScope scope(rt::TimeCat::Movie);
            const auto start = Clock::now();
            std::lock_guard lock(g_mutex);
            const auto locked = Clock::now();
            fn(ctx);
            note_slow_call(name, start, locked, Clock::now());
        };
    }
    return [name](CpuState* ctx) {
        log_call(name, ctx, LogLevel::Warn);
        ctx->x[0] = 0;
    };
}

}  // namespace lgp::boot
