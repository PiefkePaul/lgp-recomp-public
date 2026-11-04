// Host-side values of NVN's small state objects (BlendState, ColorState,
// ChannelMaskState, DepthStencilState, MultisampleState, PolygonState,
// VertexAttribState, VertexStreamState) and of SamplerBuilder.
//
// Games keep these objects on the stack, copy them by value and pass arrays
// of them. Their real sizes are small and UNVERIFIED (abi.hpp), so the HLE
// does not lay its state out in the guest storage. Instead every distinct
// value is interned in a host table, and the guest storage holds a 32-bit
// reference (type tag in the top byte, table index + 1 below). Copies made by
// the game copy the reference and stay valid; the only size assumption is
// ">= 4 bytes", and the HLE never writes past the first 4 bytes.
//
// The tables only grow with distinct values (a game has a few hundred state
// combinations, each setter call site adds at most one intermediate value),
// so nothing is ever freed. All fields are uint32_t, so values have no
// padding and compare bytewise.
#pragma once

#include <atomic>
#include <cinttypes>
#include <cstdint>
#include <cstring>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include "lgp/gpu/nvn/abi.hpp"
#include "lgp/runtime/log.hpp"

namespace lgp::gpu::nvn {

// Defaults follow OpenGL's (NVN mirrors them); UNVERIFIED like the enums.
struct BlendStateValue {
    static constexpr uint32_t kTag = 0xA1;
    static constexpr const char* kName = "BlendState";
    uint32_t target = 0;
    uint32_t src_color = uint32_t(BlendFuncNvn::One), dst_color = uint32_t(BlendFuncNvn::Zero);
    uint32_t src_alpha = uint32_t(BlendFuncNvn::One), dst_alpha = uint32_t(BlendFuncNvn::Zero);
    uint32_t color_eq = uint32_t(BlendEquationNvn::Add), alpha_eq = uint32_t(BlendEquationNvn::Add);
};
struct ColorStateValue {
    static constexpr uint32_t kTag = 0xA2;
    static constexpr const char* kName = "ColorState";
    uint32_t blend_enable_mask = 0;
    uint32_t logic_op = kLogicOpCopy;
    uint32_t alpha_func = uint32_t(CompareFuncNvn::Always);
};
struct ChannelMaskStateValue {
    static constexpr uint32_t kTag = 0xA3;
    static constexpr const char* kName = "ChannelMaskState";
    uint32_t masks = 0xFFFFFFFFu;  // 4 bits (RGBA, bit 0 = R) per colour target
};
struct DepthStencilStateValue {
    static constexpr uint32_t kTag = 0xA4;
    static constexpr const char* kName = "DepthStencilState";
    uint32_t depth_test = 0, depth_write = 1, depth_func = uint32_t(CompareFuncNvn::Less);
    uint32_t stencil_test = 0;
    uint32_t func[2] = {uint32_t(CompareFuncNvn::Always), uint32_t(CompareFuncNvn::Always)};  // front, back
    uint32_t fail[2] = {uint32_t(StencilOpNvn::Keep), uint32_t(StencilOpNvn::Keep)};
    uint32_t depth_fail[2] = {uint32_t(StencilOpNvn::Keep), uint32_t(StencilOpNvn::Keep)};
    uint32_t pass[2] = {uint32_t(StencilOpNvn::Keep), uint32_t(StencilOpNvn::Keep)};
};
struct MultisampleStateValue {
    static constexpr uint32_t kTag = 0xA5;
    static constexpr const char* kName = "MultisampleState";
    uint32_t enable = 1, samples = 0, alpha_to_coverage = 0;
};
struct PolygonStateValue {
    static constexpr uint32_t kTag = 0xA6;
    static constexpr const char* kName = "PolygonState";
    uint32_t cull = uint32_t(FaceNvn::None);
    uint32_t front_face = uint32_t(FrontFaceNvn::Ccw);
    uint32_t mode = uint32_t(PolygonModeNvn::Fill);
    uint32_t offset_enables = 0;
};
struct VertexAttribStateValue {
    static constexpr uint32_t kTag = 0xA7;
    static constexpr const char* kName = "VertexAttribState";
    uint32_t format = 0;  // NVNformat, 0 = attribute disabled
    uint32_t offset = 0;
    uint32_t stream = 0;
};
struct VertexStreamStateValue {
    static constexpr uint32_t kTag = 0xA8;
    static constexpr const char* kName = "VertexStreamState";
    uint32_t stride = 0;
    uint32_t divisor = 0;
};
struct SamplerBuilderValue {
    static constexpr uint32_t kTag = 0xA9;
    static constexpr const char* kName = "SamplerBuilder";
    uint32_t min_filter = uint32_t(MinFilterNvn::NearestMipmapLinear);
    uint32_t mag_filter = uint32_t(MagFilterNvn::Linear);
    uint32_t wrap[3] = {uint32_t(WrapModeNvn::Repeat), uint32_t(WrapModeNvn::Repeat), uint32_t(WrapModeNvn::Repeat)};
    uint32_t min_lod_bits = 0;           // float bits, 0.0
    uint32_t max_lod_bits = 0x447A0000;  // 1000.0
    uint32_t lod_bias_bits = 0;
    uint32_t max_anisotropy_bits = 0x3F800000;  // 1.0
    uint32_t compare_mode = 0, compare_func = 0, reduction = 0;
    uint32_t border[4] = {};
};

template <typename T>
class StateTable {
    static_assert(std::is_trivially_copyable_v<T> && std::has_unique_object_representations_v<T>,
                  "interned state must compare bytewise");

public:
    // Reference for v (interning it on first sight).
    uint32_t intern(const T& v) {
        const std::string key(reinterpret_cast<const char*>(&v), sizeof(T));
        if (auto it = index_.find(key); it != index_.end())
            return reference(it->second);
        if (values_.size() >= kMaxIndex) {
            rt::log(rt::LogLevel::Error, "nvn: %s table full, state change dropped", T::kName);
            return reference(0);
        }
        values_.push_back(v);
        index_.emplace(key, uint32_t(values_.size() - 1));
        return reference(uint32_t(values_.size() - 1));
    }
    // Value of a reference; defaults (logged once) for anything else, i.e.
    // storage the game never ran SetDefaults on.
    T lookup(uint32_t ref, const char* fn) {
        const uint32_t index = (ref & 0xFFFFFFu) - 1;
        if ((ref >> 24) == T::kTag && index < values_.size())
            return values_[index];
        if (!warned_.exchange(true))
            rt::log(rt::LogLevel::Warn, "nvn: %s on a %s without SetDefaults (0x%08x), using defaults", fn, T::kName,
                    ref);
        return T{};
    }
    size_t size() const { return values_.size(); }

private:
    static constexpr uint32_t kMaxIndex = 0xFFFFFE;
    static uint32_t reference(uint32_t index) { return (T::kTag << 24) | (index + 1); }

    std::vector<T> values_;
    std::unordered_map<std::string, uint32_t> index_;
    std::atomic<bool> warned_{false};
};

struct StateTables {
    StateTable<BlendStateValue> blend;
    StateTable<ColorStateValue> color;
    StateTable<ChannelMaskStateValue> channel_mask;
    StateTable<DepthStencilStateValue> depth_stencil;
    StateTable<MultisampleStateValue> multisample;
    StateTable<PolygonStateValue> polygon;
    StateTable<VertexAttribStateValue> vertex_attrib;
    StateTable<VertexStreamStateValue> vertex_stream;
    StateTable<SamplerBuilderValue> sampler_builder;

    template <typename T>
    StateTable<T>& table();
};

template <> inline StateTable<BlendStateValue>& StateTables::table() { return blend; }
template <> inline StateTable<ColorStateValue>& StateTables::table() { return color; }
template <> inline StateTable<ChannelMaskStateValue>& StateTables::table() { return channel_mask; }
template <> inline StateTable<DepthStencilStateValue>& StateTables::table() { return depth_stencil; }
template <> inline StateTable<MultisampleStateValue>& StateTables::table() { return multisample; }
template <> inline StateTable<PolygonStateValue>& StateTables::table() { return polygon; }
template <> inline StateTable<VertexAttribStateValue>& StateTables::table() { return vertex_attrib; }
template <> inline StateTable<VertexStreamStateValue>& StateTables::table() { return vertex_stream; }
template <> inline StateTable<SamplerBuilderValue>& StateTables::table() { return sampler_builder; }

}  // namespace lgp::gpu::nvn
