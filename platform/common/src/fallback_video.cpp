#include "lgp/platform/fallback_video.hpp"

#include <cstdio>
#include <vector>

namespace lgp::platform {
namespace {

class FallbackDecoder final : public IVideoDecoder {
public:
    FallbackDecoder(std::unique_ptr<IVideoDecoder> primary, IVideo& fallback, const VideoStreamInfo& info)
        : current_(std::move(primary)), fallback_(&fallback), info_(info) {}

    bool decode(const uint8_t* data, size_t size, int64_t pts_us, std::vector<VideoPicture>* out) override {
        if (fallback_ == nullptr)
            return current_->decode(data, size, pts_us, out);
        if (data != nullptr)
            kept_.push_back({std::vector<uint8_t>(data, data + size), pts_us});
        const size_t before = out->size();
        if (current_->decode(data, size, pts_us, out)) {
            if (out->size() > before) {  // the primary works: stop keeping frames
                fallback_ = nullptr;
                kept_.clear();
                return true;
            }
            // No picture after the whole stream, or after more frames than
            // any decoder buffers: one that drops what it cannot output
            // fails silently, so this counts as failing too.
            if (data == nullptr && !kept_.empty())
                return switch_to_fallback(true, out);
            if (kept_.size() >= FallbackVideo::kMaxKeptFrames)
                return switch_to_fallback(false, out);
            return true;
        }
        out->resize(before);
        return switch_to_fallback(data == nullptr, out);
    }

private:
    struct Frame {
        std::vector<uint8_t> data;
        int64_t pts_us;
    };

    // The fallback decodes everything kept so far (and the end, if that was
    // the call that failed).
    bool switch_to_fallback(bool eos, std::vector<VideoPicture>* out) {
        std::unique_ptr<IVideoDecoder> next = fallback_->open_decoder(info_);
        fallback_ = nullptr;
        if (!next) {
            kept_.clear();
            return false;
        }
        std::fprintf(stderr, "video: the first decoder failed or showed nothing before its first picture; %zu frames to the "
                     "fallback\n",
                     kept_.size());
        current_ = std::move(next);
        bool ok = true;
        for (const Frame& f : kept_)
            ok = current_->decode(f.data.data(), f.data.size(), f.pts_us, out) && ok;
        kept_.clear();
        if (eos)
            ok = current_->decode(nullptr, 0, 0, out) && ok;
        return ok;
    }

    std::unique_ptr<IVideoDecoder> current_;
    IVideo* fallback_;  // nullptr once decided
    VideoStreamInfo info_;
    std::vector<Frame> kept_;
};

}  // namespace

std::unique_ptr<IVideoDecoder> FallbackVideo::open_decoder(const VideoStreamInfo& info) {
    if (std::unique_ptr<IVideoDecoder> primary = primary_.open_decoder(info))
        return std::make_unique<FallbackDecoder>(std::move(primary), fallback_, info);
    return fallback_.open_decoder(info);
}

}  // namespace lgp::platform
