#include "lgp/hle/webm.hpp"

#include <cstdio>
#include <cstring>
#include <memory>
#include <utility>

namespace lgp::hle {
namespace {

// Element ids (with their length marker, as written in the specification).
constexpr uint32_t kEbml = 0x1A45DFA3, kDocType = 0x4282;
constexpr uint32_t kSegment = 0x18538067, kSeekHead = 0x114D9B74, kInfo = 0x1549A966, kTracks = 0x1654AE6B,
                   kCluster = 0x1F43B675, kCues = 0x1C53BB6B, kTags = 0x1254C367, kChapters = 0x1043A770,
                   kAttachments = 0x1941A469;
constexpr uint32_t kTimestampScale = 0x2AD7B1, kDuration = 0x4489;
constexpr uint32_t kTrackEntry = 0xAE, kTrackNumber = 0xD7, kTrackType = 0x83, kCodecId = 0x86,
                   kCodecPrivate = 0x63A2, kDefaultDuration = 0x23E383, kVideo = 0xE0, kPixelWidth = 0xB0,
                   kPixelHeight = 0xBA, kAudio = 0xE1, kSamplingFrequency = 0xB5, kChannels = 0x9F;
constexpr uint32_t kTimestamp = 0xE7, kSimpleBlock = 0xA3, kBlockGroup = 0xA0, kBlock = 0xA1;
constexpr uint64_t kUnknownSize = ~uint64_t{0};

bool top_level(uint32_t id) {
    switch (id) {
    case kSeekHead: case kInfo: case kTracks: case kCluster: case kCues: case kTags: case kChapters:
    case kAttachments:
        return true;
    default:
        return false;
    }
}

struct Element {
    uint32_t id = 0;
    uint64_t data = 0;  // offset of the payload
    uint64_t size = 0;  // kUnknownSize if not given
    uint64_t end() const { return size == kUnknownSize ? kUnknownSize : data + size; }
};

class Parser {
public:
    Parser(WebmRead read, uint64_t size, WebmFile* out) : read_(std::move(read)), size_(size), out_(out) {}

    // EBML header and the start of the Segment; then the Segment's elements up
    // to the first Cluster (Info and Tracks come before it in practice).
    bool open(std::string* error) {
        Element e;
        if (!header(0, &e) || e.id != kEbml)
            return fail(error, "no EBML header");
        std::string doctype;
        for (uint64_t pos = e.data; pos < e.end();) {
            Element c;
            if (!header(pos, &c) || c.size == kUnknownSize)
                return fail(error, "broken EBML header");
            if (c.id == kDocType && !string(c, &doctype))
                return fail(error, "broken DocType");
            pos = c.end();
        }
        if (doctype != "webm" && doctype != "matroska")
            return fail(error, "doctype \"" + doctype + "\" is not webm");
        Element seg;
        uint64_t pos = e.end();
        for (;;) {
            if (pos >= size_ || !header(pos, &seg))
                return fail(error, "no Segment");
            if (seg.id == kSegment)
                break;
            if (seg.size == kUnknownSize)
                return fail(error, "unknown-size element before the Segment");
            pos = seg.end();
        }
        seg_end_ = seg.size == kUnknownSize || seg.end() > size_ ? size_ : seg.end();
        pos_ = seg.data;
        while (!done_ && !in_cluster_)
            if (!step(error))
                return false;
        if (out_->tracks.empty())
            return fail(error, "no tracks");
        return true;
    }

    // One step: the next element of the current Cluster (a block), or the
    // next element of the Segment. False with *error set on a broken element.
    bool step(std::string* error) {
        if (done_)
            return true;
        if (in_cluster_)
            return cluster_step(error);
        Element c;
        if (pos_ >= seg_end_ || !header(pos_, &c)) {
            finish();  // end, or a truncated file: keep what was read
            return true;
        }
        if (c.size == kUnknownSize && c.id != kCluster)
            return fail(error, "unknown-size element in the Segment");
        bool ok = true;
        switch (c.id) {
        case kInfo:
            ok = info(c, &duration_);
            out_->duration_ns = int64_t(duration_ * double(out_->timestamp_scale_ns));
            break;
        case kTracks: ok = tracks(c); break;
        case kCluster:
            in_cluster_ = true;
            cluster_unknown_ = c.size == kUnknownSize;
            cluster_end_ = cluster_unknown_ ? seg_end_ : c.end();
            cluster_pos_ = c.data;
            cluster_timestamp_ = 0;
            return true;
        default: break;
        }
        if (!ok)
            return fail(error, "broken element 0x" + hex(c.id) + " at offset " + std::to_string(c.data));
        pos_ = c.end();
        return true;
    }

    bool done() const { return done_; }

private:
    void finish() { done_ = true; }

    // A child of a Cluster. A Cluster of unknown size ends at the next
    // top-level element, where the Segment continues.
    bool cluster_step(std::string* error) {
        Element c;
        if (cluster_pos_ >= cluster_end_ || !header(cluster_pos_, &c) ||
            (cluster_unknown_ && top_level(c.id))) {
            in_cluster_ = false;
            pos_ = cluster_pos_;
            return true;
        }
        bool ok = c.size != kUnknownSize;
        if (ok && c.id == kTimestamp)
            ok = uint(c, &cluster_timestamp_);
        else if (ok && c.id == kSimpleBlock)
            ok = block(c, cluster_timestamp_, true);
        else if (ok && c.id == kBlockGroup)
            ok = children(c, [&](const Element& b) { return b.id != kBlock || block(b, cluster_timestamp_, false); });
        if (!ok)
            return fail(error, "broken element 0x" + hex(c.id) + " at offset " + std::to_string(c.data));
        cluster_pos_ = c.end();
        return true;
    }

    static bool fail(std::string* error, const std::string& what) {
        if (error != nullptr)
            *error = what;
        return false;
    }
    static std::string hex(uint32_t v) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%X", v);
        return buf;
    }

    // Variable-size integer at *pos: element id (marker kept) or size.
    bool vint(uint64_t* pos, uint64_t* value, bool id, int* length = nullptr) {
        uint8_t first;
        if (!read_(*pos, &first, 1))
            return false;
        int len = 1;
        while (len <= 8 && !(first & (0x80 >> (len - 1))))
            ++len;
        if (len > (id ? 4 : 8))
            return false;
        uint8_t rest[8] = {};
        if (len > 1 && !read_(*pos + 1, rest, size_t(len - 1)))
            return false;
        uint64_t v = id ? first : first & (0xFF >> len);
        bool all_ones = v == uint64_t(0xFF >> len);
        for (int i = 0; i < len - 1; ++i) {
            v = (v << 8) | rest[i];
            all_ones = all_ones && rest[i] == 0xFF;
        }
        *pos += uint64_t(len);
        *value = !id && all_ones ? kUnknownSize : v;
        if (length != nullptr)
            *length = len;
        return true;
    }

    bool header(uint64_t pos, Element* e) {
        uint64_t id;
        if (!vint(&pos, &id, true) || !vint(&pos, &e->size, false))
            return false;
        e->id = uint32_t(id);
        e->data = pos;
        return e->size == kUnknownSize || e->data + e->size <= size_;
    }

    bool uint(const Element& e, uint64_t* v) {
        uint8_t b[8];
        if (e.size > 8 || !read_(e.data, b, size_t(e.size)))
            return false;
        *v = 0;
        for (uint64_t i = 0; i < e.size; ++i)
            *v = (*v << 8) | b[i];
        return true;
    }
    bool floating(const Element& e, double* v) {
        uint8_t b[8];
        if ((e.size != 4 && e.size != 8) || !read_(e.data, b, size_t(e.size)))
            return false;
        uint64_t bits = 0;
        for (uint64_t i = 0; i < e.size; ++i)
            bits = (bits << 8) | b[i];
        if (e.size == 4) {
            float f;
            const uint32_t b32 = uint32_t(bits);
            std::memcpy(&f, &b32, 4);
            *v = f;
        } else {
            std::memcpy(v, &bits, 8);
        }
        return true;
    }
    bool bytes(const Element& e, std::vector<uint8_t>* v) {
        if (e.size > (64u << 20))
            return false;
        v->resize(size_t(e.size));
        return e.size == 0 || read_(e.data, v->data(), v->size());
    }
    bool string(const Element& e, std::string* s) {
        std::vector<uint8_t> b;
        if (!bytes(e, &b))
            return false;
        s->assign(b.begin(), b.end());
        s->resize(std::strlen(s->c_str()));  // strings may be zero-padded
        return true;
    }

    // Calls f(child) for every child of e; false on a broken child.
    template <typename F>
    bool children(const Element& e, F&& f) {
        for (uint64_t pos = e.data; pos < e.end();) {
            Element c;
            if (!header(pos, &c) || c.size == kUnknownSize || c.end() > e.end() || !f(c))
                return false;
            pos = c.end();
        }
        return true;
    }

    bool info(const Element& e, double* duration) {
        return children(e, [&](const Element& c) {
            if (c.id == kTimestampScale)
                return uint(c, &out_->timestamp_scale_ns) && out_->timestamp_scale_ns != 0;
            if (c.id == kDuration)
                return floating(c, duration);
            return true;
        });
    }

    bool tracks(const Element& e) {
        return children(e, [&](const Element& entry) {
            if (entry.id != kTrackEntry)
                return true;
            WebmTrack t;
            uint64_t v = 0;
            const bool ok = children(entry, [&](const Element& c) {
                switch (c.id) {
                case kTrackNumber: return uint(c, &t.number);
                case kTrackType: return uint(c, &t.type);
                case kCodecId: return string(c, &t.codec_id);
                case kCodecPrivate: return bytes(c, &t.codec_private);
                case kDefaultDuration: return uint(c, &t.default_duration_ns);
                case kVideo:
                    return children(c, [&](const Element& v2) {
                        if (v2.id == kPixelWidth && uint(v2, &v))
                            t.width = uint32_t(v);
                        else if (v2.id == kPixelHeight && uint(v2, &v))
                            t.height = uint32_t(v);
                        return true;
                    });
                case kAudio:
                    return children(c, [&](const Element& a) {
                        if (a.id == kSamplingFrequency)
                            return floating(a, &t.sampling_frequency);
                        if (a.id == kChannels && uint(a, &v))
                            t.channels = uint32_t(v);
                        return true;
                    });
                default: return true;
                }
            });
            if (ok && t.number != 0)
                out_->tracks.push_back(std::move(t));
            return ok;
        });
    }

    bool block(const Element& e, uint64_t cluster_timestamp, bool simple) {
        uint64_t pos = e.data, track;
        if (!vint(&pos, &track, false) || track == kUnknownSize)
            return false;
        uint8_t hdr[3];
        if (pos + 3 > e.end() || !read_(pos, hdr, 3))
            return false;
        pos += 3;
        const int16_t relative = int16_t(uint16_t(hdr[0] << 8 | hdr[1]));
        const uint8_t flags = hdr[2];
        const int64_t pts = (int64_t(cluster_timestamp) + relative) * int64_t(out_->timestamp_scale_ns);
        const bool key = simple ? (flags & 0x80) != 0 : relative == 0;  // BlockGroup: no flag, assume key
        const int lacing = (flags >> 1) & 3;
        std::vector<uint64_t> sizes;
        if (lacing != 0) {
            uint8_t count_minus_one;
            if (pos >= e.end() || !read_(pos++, &count_minus_one, 1))
                return false;
            const size_t count = size_t(count_minus_one) + 1;
            if (lacing == 1) {  // Xiph
                for (size_t i = 0; i + 1 < count; ++i) {
                    uint64_t s = 0;
                    for (uint8_t b = 255; b == 255;) {
                        if (pos >= e.end() || !read_(pos++, &b, 1))
                            return false;
                        s += b;
                    }
                    sizes.push_back(s);
                }
            } else if (lacing == 3) {  // EBML: first size, then signed differences
                uint64_t s;
                if (!vint(&pos, &s, false) || s == kUnknownSize)
                    return false;
                sizes.push_back(s);
                for (size_t i = 1; i + 1 < count; ++i) {
                    uint64_t raw;
                    int len;
                    if (!vint(&pos, &raw, false, &len) || raw == kUnknownSize)
                        return false;
                    const int64_t diff = int64_t(raw) - ((int64_t{1} << (7 * len - 1)) - 1);
                    s = uint64_t(int64_t(s) + diff);
                    sizes.push_back(s);
                }
            }
            uint64_t used = 0;
            for (uint64_t s : sizes)
                used += s;
            if (pos + used > e.end())
                return false;
            const uint64_t rest = e.end() - pos - used;
            if (lacing == 2) {  // fixed: equal parts
                if (rest % count != 0)
                    return false;
                sizes.assign(count, rest / count);
            } else {
                sizes.push_back(rest);
            }
        } else {
            sizes.push_back(e.end() - pos);
        }
        const WebmTrack* t = out_->track(track);
        const int64_t step = t != nullptr ? int64_t(t->default_duration_ns) : 0;
        for (size_t i = 0; i < sizes.size(); ++i) {
            out_->frames.push_back({track, pts + int64_t(i) * step, pos, uint32_t(sizes[i]), key});
            pos += sizes[i];
        }
        return true;
    }

    WebmRead read_;
    uint64_t size_;
    WebmFile* out_;
    uint64_t pos_ = 0, seg_end_ = 0;  // next element of the Segment
    double duration_ = 0;
    bool done_ = false;
    bool in_cluster_ = false, cluster_unknown_ = false;
    uint64_t cluster_pos_ = 0, cluster_end_ = 0, cluster_timestamp_ = 0;
};

}  // namespace

const WebmTrack* WebmFile::track(uint64_t number) const {
    for (const WebmTrack& t : tracks)
        if (t.number == number)
            return &t;
    return nullptr;
}

struct WebmIndexer::Impl {
    Parser parser;
};

WebmIndexer::WebmIndexer() = default;
WebmIndexer::~WebmIndexer() = default;
WebmIndexer::WebmIndexer(WebmIndexer&&) noexcept = default;
WebmIndexer& WebmIndexer::operator=(WebmIndexer&&) noexcept = default;

bool WebmIndexer::open(WebmRead read, uint64_t size, WebmFile* out, std::string* error) {
    *out = WebmFile{};
    impl_ = std::make_unique<Impl>(Impl{Parser(std::move(read), size, out)});
    if (impl_->parser.open(error))
        return true;
    impl_.reset();
    return false;
}

bool WebmIndexer::next(std::string* error) {
    if (!impl_ || impl_->parser.done())
        return false;
    const bool ok = impl_->parser.step(error);
    if (!ok)
        impl_.reset();
    return ok;
}

bool WebmIndexer::done() const { return !impl_ || impl_->parser.done(); }

bool parse_webm(const WebmRead& read, uint64_t size, WebmFile* out, std::string* error) {
    WebmIndexer index;
    if (!index.open(read, size, out, error))
        return false;
    while (!index.done())
        if (!index.next(error))
            return false;
    return true;
}

}  // namespace lgp::hle
