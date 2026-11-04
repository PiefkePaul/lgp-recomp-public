#include "lgp/gpu/nvn/abi.hpp"

#include <algorithm>
#include <iterator>

namespace lgp::gpu::nvn {

namespace {

struct FormatRow {
    uint32_t id;
    std::string_view name;
    uint32_t bytes_per_block, block_width, block_height;
    FormatKind kind;
    bool srgb;
};

// NVNformat -> size and kind, measured in the format table of LGP's driver
// (sdk, 0x30-byte entries indexed by the format; bytes per block at +0x10,
// block size at +0x08, sRGB flag at +0x12). Formats 25-33 and 81-108 exist
// only as vertex formats and are left out. Names: public NVN enum order.
constexpr FormatRow kFormats[] = {
    {1, "R8", 1, 1, 1, FormatKind::Color, false},
    {2, "R8SN", 1, 1, 1, FormatKind::Color, false},
    {3, "R8UI", 1, 1, 1, FormatKind::Color, false},
    {4, "R8I", 1, 1, 1, FormatKind::Color, false},
    {5, "R16F", 2, 1, 1, FormatKind::Color, false},
    {6, "R16", 2, 1, 1, FormatKind::Color, false},
    {7, "R16SN", 2, 1, 1, FormatKind::Color, false},
    {8, "R16UI", 2, 1, 1, FormatKind::Color, false},
    {9, "R16I", 2, 1, 1, FormatKind::Color, false},
    {10, "R32F", 4, 1, 1, FormatKind::Color, false},
    {11, "R32UI", 4, 1, 1, FormatKind::Color, false},
    {12, "R32I", 4, 1, 1, FormatKind::Color, false},
    {13, "RG8", 2, 1, 1, FormatKind::Color, false},
    {14, "RG8SN", 2, 1, 1, FormatKind::Color, false},
    {15, "RG8UI", 2, 1, 1, FormatKind::Color, false},
    {16, "RG8I", 2, 1, 1, FormatKind::Color, false},
    {17, "RG16F", 4, 1, 1, FormatKind::Color, false},
    {18, "RG16", 4, 1, 1, FormatKind::Color, false},
    {19, "RG16SN", 4, 1, 1, FormatKind::Color, false},
    {20, "RG16UI", 4, 1, 1, FormatKind::Color, false},
    {21, "RG16I", 4, 1, 1, FormatKind::Color, false},
    {22, "RG32F", 8, 1, 1, FormatKind::Color, false},
    {23, "RG32UI", 8, 1, 1, FormatKind::Color, false},
    {24, "RG32I", 8, 1, 1, FormatKind::Color, false},
    {34, "RGB32F", 12, 1, 1, FormatKind::Color, false},
    {35, "RGB32UI", 12, 1, 1, FormatKind::Color, false},
    {36, "RGB32I", 12, 1, 1, FormatKind::Color, false},
    {37, "RGBA8", 4, 1, 1, FormatKind::Color, false},
    {38, "RGBA8SN", 4, 1, 1, FormatKind::Color, false},
    {39, "RGBA8UI", 4, 1, 1, FormatKind::Color, false},
    {40, "RGBA8I", 4, 1, 1, FormatKind::Color, false},
    {41, "RGBA16F", 8, 1, 1, FormatKind::Color, false},
    {42, "RGBA16", 8, 1, 1, FormatKind::Color, false},
    {43, "RGBA16SN", 8, 1, 1, FormatKind::Color, false},
    {44, "RGBA16UI", 8, 1, 1, FormatKind::Color, false},
    {45, "RGBA16I", 8, 1, 1, FormatKind::Color, false},
    {46, "RGBA32F", 16, 1, 1, FormatKind::Color, false},
    {47, "RGBA32UI", 16, 1, 1, FormatKind::Color, false},
    {48, "RGBA32I", 16, 1, 1, FormatKind::Color, false},
    {49, "STENCIL8", 1, 1, 1, FormatKind::Stencil, false},
    {50, "DEPTH16", 2, 1, 1, FormatKind::Depth, false},
    {51, "DEPTH24", 4, 1, 1, FormatKind::Depth, false},
    {52, "DEPTH32F", 4, 1, 1, FormatKind::Depth, false},
    {53, "DEPTH24_STENCIL8", 4, 1, 1, FormatKind::DepthStencil, false},
    {54, "DEPTH32F_STENCIL8", 8, 1, 1, FormatKind::DepthStencil, false},
    {55, "RGBX8_SRGB", 4, 1, 1, FormatKind::Color, true},
    {56, "RGBA8_SRGB", 4, 1, 1, FormatKind::Color, true},
    {57, "RGBA4", 2, 1, 1, FormatKind::Color, false},
    {58, "RGB5", 2, 1, 1, FormatKind::Color, false},
    {59, "RGB5A1", 2, 1, 1, FormatKind::Color, false},
    {60, "RGB565", 2, 1, 1, FormatKind::Color, false},
    {61, "RGB10A2", 4, 1, 1, FormatKind::Color, false},
    {62, "RGB10A2UI", 4, 1, 1, FormatKind::Color, false},
    {63, "R11G11B10F", 4, 1, 1, FormatKind::Color, false},
    {64, "RGB9E5F", 4, 1, 1, FormatKind::Color, false},
    {65, "RGB_DXT1", 8, 4, 4, FormatKind::Color, false},
    {66, "RGBA_DXT1", 8, 4, 4, FormatKind::Color, false},
    {67, "RGBA_DXT3", 16, 4, 4, FormatKind::Color, false},
    {68, "RGBA_DXT5", 16, 4, 4, FormatKind::Color, false},
    {69, "RGB_DXT1_SRGB", 8, 4, 4, FormatKind::Color, true},
    {70, "RGBA_DXT1_SRGB", 8, 4, 4, FormatKind::Color, true},
    {71, "RGBA_DXT3_SRGB", 16, 4, 4, FormatKind::Color, true},
    {72, "RGBA_DXT5_SRGB", 16, 4, 4, FormatKind::Color, true},
    {73, "RGTC1_UNORM", 8, 4, 4, FormatKind::Color, false},
    {74, "RGTC1_SNORM", 8, 4, 4, FormatKind::Color, false},
    {75, "RGTC2_UNORM", 16, 4, 4, FormatKind::Color, false},
    {76, "RGTC2_SNORM", 16, 4, 4, FormatKind::Color, false},
    {77, "BPTC_UNORM", 16, 4, 4, FormatKind::Color, false},
    {78, "BPTC_UNORM_SRGB", 16, 4, 4, FormatKind::Color, true},
    {79, "BPTC_SFLOAT", 16, 4, 4, FormatKind::Color, false},
    {80, "BPTC_UFLOAT", 16, 4, 4, FormatKind::Color, false},
    {121, "RGBA_ASTC_4x4", 16, 4, 4, FormatKind::Color, false},
    {122, "RGBA_ASTC_5x4", 16, 5, 4, FormatKind::Color, false},
    {123, "RGBA_ASTC_5x5", 16, 5, 5, FormatKind::Color, false},
    {124, "RGBA_ASTC_6x5", 16, 6, 5, FormatKind::Color, false},
    {125, "RGBA_ASTC_6x6", 16, 6, 6, FormatKind::Color, false},
    {126, "RGBA_ASTC_8x5", 16, 8, 5, FormatKind::Color, false},
    {127, "RGBA_ASTC_8x6", 16, 8, 6, FormatKind::Color, false},
    {128, "RGBA_ASTC_8x8", 16, 8, 8, FormatKind::Color, false},
    {129, "RGBA_ASTC_10x5", 16, 10, 5, FormatKind::Color, false},
    {130, "RGBA_ASTC_10x6", 16, 10, 6, FormatKind::Color, false},
    {131, "RGBA_ASTC_10x8", 16, 10, 8, FormatKind::Color, false},
    {132, "RGBA_ASTC_10x10", 16, 10, 10, FormatKind::Color, false},
    {133, "RGBA_ASTC_12x10", 16, 12, 10, FormatKind::Color, false},
    {134, "RGBA_ASTC_12x12", 16, 12, 12, FormatKind::Color, false},
    {135, "RGBA_ASTC_4x4_SRGB", 16, 4, 4, FormatKind::Color, true},
    {136, "RGBA_ASTC_5x4_SRGB", 16, 5, 4, FormatKind::Color, true},
    {137, "RGBA_ASTC_5x5_SRGB", 16, 5, 5, FormatKind::Color, true},
    {138, "RGBA_ASTC_6x5_SRGB", 16, 6, 5, FormatKind::Color, true},
    {139, "RGBA_ASTC_6x6_SRGB", 16, 6, 6, FormatKind::Color, true},
    {140, "RGBA_ASTC_8x5_SRGB", 16, 8, 5, FormatKind::Color, true},
    {141, "RGBA_ASTC_8x6_SRGB", 16, 8, 6, FormatKind::Color, true},
    {142, "RGBA_ASTC_8x8_SRGB", 16, 8, 8, FormatKind::Color, true},
    {143, "RGBA_ASTC_10x5_SRGB", 16, 10, 5, FormatKind::Color, true},
    {144, "RGBA_ASTC_10x6_SRGB", 16, 10, 6, FormatKind::Color, true},
    {145, "RGBA_ASTC_10x8_SRGB", 16, 10, 8, FormatKind::Color, true},
    {146, "RGBA_ASTC_10x10_SRGB", 16, 10, 10, FormatKind::Color, true},
    {147, "RGBA_ASTC_12x10_SRGB", 16, 12, 10, FormatKind::Color, true},
    {148, "RGBA_ASTC_12x12_SRGB", 16, 12, 12, FormatKind::Color, true},
    {149, "BGR565", 2, 1, 1, FormatKind::Color, false},
    {150, "BGR5", 2, 1, 1, FormatKind::Color, false},
    {151, "BGR5A1", 2, 1, 1, FormatKind::Color, false},
    {152, "A1BGR5", 2, 1, 1, FormatKind::Color, false},
    {153, "BGRX8", 4, 1, 1, FormatKind::Color, false},
    {154, "BGRA8", 4, 1, 1, FormatKind::Color, false},
    {155, "BGRX8_SRGB", 4, 1, 1, FormatKind::Color, true},
    {156, "BGRA8_SRGB", 4, 1, 1, FormatKind::Color, true},
};

}  // namespace

std::optional<FormatInfo> format_info(uint32_t format) {
    const auto it = std::lower_bound(std::begin(kFormats), std::end(kFormats), format,
                                     [](const FormatRow& r, uint32_t f) { return r.id < f; });
    if (it == std::end(kFormats) || it->id != format)
        return std::nullopt;
    FormatInfo info{it->name, it->bytes_per_block, it->block_width, it->block_height, it->kind, it->srgb, false};
    switch (static_cast<Format>(format)) {
    case Format::RGBA8:
    case Format::RGBX8_SRGB:
    case Format::RGBA8_SRGB: info.soft_renderable = true; break;
    default: break;
    }
    return info;
}

namespace {

// NVNdeviceInfo keys and answers, read from the driver in LGP's own sdk
// (nvnDeviceGetInteger is a switch over the key; most cases return a constant,
// either directly or from a second switch). Values: measured. Names: only
// where the public header order is known, otherwise INFO_<key>; UNVERIFIED.
// Not here: keys whose case reads the initialised device object (23, 24, 43,
// 45, 55, 58, 61-65; 56 see below) and stay unknown (logged, 0) until a game asks.
// 59 is "not <device flag>" and answers 1 for a device created without that
// flag (assumption: LGP does not set it). Analysis: docs/analysis/10-nvn-pruefliste.md.
constexpr DeviceInteger kDefaults[] = {
    {0, 53, "API_MAJOR_VERSION"},
    {1, 209, "API_MINOR_VERSION"},
    {2, 14, "UNIFORM_BUFFER_BINDINGS_PER_STAGE"},
    {3, 0x10000, "MAX_UNIFORM_BUFFER_SIZE"},
    {4, 0x100, "UNIFORM_BUFFER_ALIGNMENT"},
    {5, 8, "COLOR_BUFFER_BINDINGS"},
    {6, 16, "VERTEX_BUFFER_BINDINGS"},
    {7, 4, "TRANSFORM_FEEDBACK_BUFFER_BINDINGS"},
    {8, 16, "SHADER_STORAGE_BUFFER_BINDINGS_PER_STAGE"},
    {9, 32, "TEXTURE_BINDINGS_PER_STAGE"},
    {10, 16, "COUNTER_ALIGNMENT"},
    {11, 4, "TRANSFORM_FEEDBACK_BUFFER_ALIGNMENT"},
    {12, 4, "TRANSFORM_FEEDBACK_CONTROL_ALIGNMENT"},
    {13, 4, "INDIRECT_DRAW_ALIGNMENT"},
    {14, 16, "VERTEX_ATTRIBUTES"},
    {15, 32, "TEXTURE_DESCRIPTOR_SIZE"},
    {16, 32, "SAMPLER_DESCRIPTOR_SIZE"},
    {17, 0x100, "RESERVED_TEXTURE_DESCRIPTORS"},
    {18, 0x100, "RESERVED_SAMPLER_DESCRIPTORS"},
    {19, 4, "COMMAND_BUFFER_COMMAND_ALIGNMENT"},
    {20, 8, "COMMAND_BUFFER_CONTROL_ALIGNMENT"},
    {21, 0x200, "COMMAND_BUFFER_MIN_COMMAND_SIZE"},
    {22, 0x100, "COMMAND_BUFFER_MIN_CONTROL_SIZE"},
    {25, 0x1000, "INFO_25"},
    {26, 0x20000, "INFO_26"},
    {27, 16, "INFO_27"},
    {28, 0x600, "INFO_28"},
    {29, 0x400, "INFO_29"},
    {30, 64, "INFO_30"},
    {31, 0x600, "INFO_31"},
    {32, 0xffff, "INFO_32"},
    {33, 0xffff, "INFO_33"},
    {34, 0xffff, "INFO_34"},
    {35, 8, "INFO_35"},
    {36, 0x100000, "INFO_36"},
    {37, 0x1000, "INFO_37"},
    {38, 16, "INFO_38"},
    {39, 0x1000, "INFO_39"},
    {40, 1, "INFO_40"},
    {41, 1, "INFO_41"},
    {42, 1, "INFO_42"},
    {44, 15, "INFO_44"},
    {46, 1, "INFO_46"},
    {47, 1, "INFO_47"},
    {48, 15, "INFO_48"},
    {49, 5, "INFO_49"},
    {50, 1, "INFO_50"},
    {51, 8, "INFO_51"},
    {52, 8, "INFO_52"},
    {53, 4, "INFO_53"},
    {54, 32, "INFO_54"},
    // 56 reads field +0x24 of the device, copied at nvnDeviceInitialize from a
    // GPU description the driver queries (its +0x00/+0x04 hold 0x120/0xB, the
    // nvgpu characteristics' arch/impl, whose +0x24 is 0x20 on the console as
    // Ryujinx reports it). LGP divides a movie frame's width by it (alignment),
    // so 0 crashed the title video. Value inferred, not measured on hardware.
    {56, 32, "INFO_56"},
    {57, 128, "INFO_57"},
    {59, 1, "INFO_59"},
    {60, 4, "INFO_60"},
    {66, 1, "INFO_66"},
    {67, 1, "INFO_67"},
    {68, 1, "INFO_68"},
    {69, 1, "INFO_69"},
    {70, 5, "INFO_70"},
    {71, 1, "INFO_71"},
    {72, 1, "INFO_72"},
    {73, 1, "INFO_73"},
    {74, 1, "INFO_74"},
    {75, 1, "INFO_75"},
    {76, 1, "INFO_76"},
    {77, 16, "INFO_77"},
    {78, 0x400, "INFO_78"},
    {79, 32, "INFO_79"},
    {80, 0x1000, "INFO_80"},
    {81, 0x10000, "INFO_81"},
    {82, 0x10000, "INFO_82"},
    {83, 0x100, "INFO_83"},
    {84, 0x4000, "INFO_84"},
    {85, 0x40000, "INFO_85"},
    {86, 0x1000, "INFO_86"},
    {87, 1, "INFO_87"},
    {88, 4, "INFO_88"},
    {89, 2, "INFO_89"},
    {90, 0, "INFO_90"},
    {91, 0x1000, "INFO_91"},
    {92, 0x4000, "INFO_92"},
    {93, 0x1000, "INFO_93"},
    {94, 128, "INFO_94"},
    {95, 32, "INFO_95"},
    {96, 0xffff, "INFO_96"},
    {97, 1, "INFO_97"},
};

uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) / a * a; }

}  // namespace

const DeviceInteger* default_device_integers(size_t* count) {
    *count = std::size(kDefaults);
    return kDefaults;
}

TextureLayout texture_layout(uint32_t format, uint32_t target, uint32_t width, uint32_t height, uint32_t depth,
                             uint32_t levels, int32_t gob_height_log2) {
    const auto info = format_info(format);
    const uint32_t bpb = info ? info->bytes_per_pixel : 4;
    const uint32_t bw = info ? info->block_width : 1, bh = info ? info->block_height : 1;
    depth = std::max(depth, 1u);
    switch (static_cast<TextureTarget>(target)) {
    case TextureTarget::Target3D:
        return block_linear_layout(bpb, bw, bh, width, height, depth, 1, levels, gob_height_log2, true);
    case TextureTarget::TargetCubemap: return block_linear_layout(bpb, bw, bh, width, height, 1, 6, levels, gob_height_log2);
    case TextureTarget::TargetCubemapArray:
        return block_linear_layout(bpb, bw, bh, width, height, 1, 6 * depth, levels, gob_height_log2);
    case TextureTarget::Target1DArray:
    case TextureTarget::Target2DArray:
    case TextureTarget::Target2DMultisampleArray:
        return block_linear_layout(bpb, bw, bh, width, height, 1, depth, levels, gob_height_log2);
    default: return block_linear_layout(bpb, bw, bh, width, height, 1, 1, levels, gob_height_log2);
    }
}

uint64_t texture_storage_size(uint32_t format, uint32_t target, uint32_t width, uint32_t height, uint32_t depth,
                              uint32_t levels) {
    return align_up(texture_layout(format, target, width, height, depth, levels).total_size, kTextureStorageAlignment);
}

}  // namespace lgp::gpu::nvn
