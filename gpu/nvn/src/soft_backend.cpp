// CPU reference backend (backend.hpp).
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "lgp/gpu/nvn/backend.hpp"
#include "lgp/runtime/log.hpp"

namespace lgp::gpu::nvn {

namespace {

uint8_t to_unorm8(float v) {
    if (!(v > 0.0f))  // also NaN
        return 0;
    if (v >= 1.0f)
        return 255;
    return static_cast<uint8_t>(std::lround(v * 255.0f));
}

float linear_to_srgb(float v) {
    if (!(v > 0.0f))
        return 0.0f;
    if (v >= 1.0f)
        return 1.0f;
    return v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
}

}  // namespace

void SoftBackend::create_texture(TextureId id, const TextureDesc& desc) {
    std::lock_guard lock(mutex_);
    Texture t;
    t.desc = desc;
    const auto info = format_info(desc.format);
    t.renderable = info && info->soft_renderable && desc.width > 0 && desc.height > 0 &&
                   uint64_t{desc.width} * desc.height <= 16384ull * 16384ull;
    t.srgb = info && info->srgb;
    if (t.renderable)
        t.rgba.assign(size_t(desc.width) * desc.height * 4, 0);
    textures_[id] = std::move(t);
}

void SoftBackend::destroy_texture(TextureId id) {
    std::lock_guard lock(mutex_);
    textures_.erase(id);
}

void SoftBackend::execute(const CommandList& list) {
    std::lock_guard lock(mutex_);
    ExecState& st = state_;  // carried over from the previous lists, like GPU state
    for (const Command& command : list.commands) {
        if (auto* c = std::get_if<CmdSetRenderTargets>(&command)) {
            st.colors = c->colors;
        } else if (auto* c = std::get_if<CmdSetScissors>(&command)) {
            if (c->first == 0 && !c->rects.empty()) {
                st.scissor = c->rects[0];
                st.scissor_set = true;
            }
        } else if (auto* c = std::get_if<CmdClearColor>(&command)) {
            auto it = c->index < st.colors.size() ? textures_.find(st.colors[c->index]) : textures_.end();
            if (it == textures_.end() || !it->second.renderable) {
                ++stats_.skipped_commands;
                continue;
            }
            Texture& t = it->second;
            const int64_t w = t.desc.width, h = t.desc.height;
            int64_t x0 = 0, x1 = w, y0 = 0, y1 = h;  // window space
            if (st.scissor_set) {
                x0 = std::clamp<int64_t>(st.scissor.x, 0, w);
                x1 = std::clamp<int64_t>(int64_t{st.scissor.x} + st.scissor.width, 0, w);
                y0 = std::clamp<int64_t>(st.scissor.y, 0, h);
                y1 = std::clamp<int64_t>(int64_t{st.scissor.y} + st.scissor.height, 0, h);
            }
            uint8_t px[4];
            for (int i = 0; i < 4; ++i)
                px[i] = to_unorm8(t.srgb && i < 3 ? linear_to_srgb(c->rgba[i]) : c->rgba[i]);
            for (int64_t y = y0; y < y1; ++y) {
                // Window row y is memory row y in both origin modes; the
                // lower-left origin flips the picture at present.
                uint8_t* p = t.rgba.data() + (y * w + x0) * 4;
                for (int64_t x = x0; x < x1; ++x, p += 4)
                    for (int i = 0; i < 4; ++i)
                        if (c->mask & (1u << i))
                            p[i] = px[i];
            }
            ++stats_.clears;
        } else if (auto* c = std::get_if<CmdClearTexture>(&command)) {
            auto it = textures_.find(c->texture);
            if (it == textures_.end() || !it->second.renderable) {
                ++stats_.skipped_commands;
                continue;
            }
            Texture& t = it->second;
            const int64_t w = t.desc.width, h = t.desc.height;
            const int64_t x0 = std::clamp<int64_t>(c->region.x, 0, w);
            const int64_t x1 = std::clamp<int64_t>(int64_t{c->region.x} + c->region.width, 0, w);
            const int64_t y0 = std::clamp<int64_t>(c->region.y, 0, h);
            const int64_t y1 = std::clamp<int64_t>(int64_t{c->region.y} + c->region.height, 0, h);
            uint8_t px[4];
            for (int i = 0; i < 4; ++i)
                px[i] = to_unorm8(t.srgb && i < 3 ? linear_to_srgb(c->rgba[i]) : c->rgba[i]);
            for (int64_t y = y0; y < y1; ++y) {
                uint8_t* p = t.rgba.data() + (y * w + x0) * 4;
                for (int64_t x = x0; x < x1; ++x, p += 4)
                    for (int i = 0; i < 4; ++i)
                        if (c->mask & (1u << i))
                            p[i] = px[i];
            }
            ++stats_.clears;
        } else if (std::holds_alternative<CmdUnsupported>(command) ||
                   std::holds_alternative<CmdClearDepthStencil>(command) || std::holds_alternative<CmdDraw>(command) ||
                   std::holds_alternative<CmdDispatchCompute>(command)) {
            ++stats_.skipped_commands;  // no depth buffers, draws or compute on the CPU path
        }
        // Viewports, depth ranges and barriers do not affect clears.
    }
}

bool SoftBackend::present(const PresentInfo& info) {
    std::unique_lock lock(mutex_);
    ++stats_.presents;
    auto it = textures_.find(info.texture);
    if (it == textures_.end() || !it->second.renderable) {
        rt::log(rt::LogLevel::Warn, "nvn soft backend: present of texture #%u it cannot show", info.texture);
    } else {
        const Texture& t = it->second;
        Rect r = info.crop;
        if (r.width <= 0 || r.height <= 0)
            r = {0, 0, static_cast<int32_t>(t.desc.width), static_cast<int32_t>(t.desc.height)};
        r.x = std::clamp<int32_t>(r.x, 0, t.desc.width);
        r.y = std::clamp<int32_t>(r.y, 0, t.desc.height);
        r.width = std::min<int32_t>(r.width, t.desc.width - r.x);
        r.height = std::min<int32_t>(r.height, t.desc.height - r.y);
        platform::FrameView view;
        view.pixels = t.rgba.data() + (size_t(r.y) * t.desc.width + r.x) * 4;
        view.width = static_cast<uint32_t>(r.width);
        view.height = static_cast<uint32_t>(r.height);
        view.stride = t.desc.width * 4;
        std::vector<uint8_t> flipped;
        if (info.flip_y && r.height > 0) {  // lower-left origin: last row on top
            flipped.resize(size_t(r.width) * r.height * 4);
            for (int32_t y = 0; y < r.height; ++y)
                std::memcpy(flipped.data() + size_t(y) * r.width * 4,
                            view.pixels + size_t(r.height - 1 - y) * view.stride, size_t(r.width) * 4);
            view.pixels = flipped.data();
            view.stride = uint32_t(r.width) * 4;
        }
        renderer_.present_rgba8(view);
    }
    lock.unlock();
    return renderer_.end_frame();
}

SoftBackend::Stats SoftBackend::stats() const {
    std::lock_guard lock(mutex_);
    return stats_;
}

bool SoftBackend::read_texel(TextureId id, uint32_t x, uint32_t y, uint8_t rgba[4]) const {
    std::lock_guard lock(mutex_);
    auto it = textures_.find(id);
    if (it == textures_.end() || !it->second.renderable || x >= it->second.desc.width || y >= it->second.desc.height)
        return false;
    const uint8_t* p = it->second.rgba.data() + (size_t(y) * it->second.desc.width + x) * 4;
    std::copy(p, p + 4, rgba);
    return true;
}

}  // namespace lgp::gpu::nvn
