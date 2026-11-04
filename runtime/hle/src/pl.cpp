// pl:u (shared fonts), see lgp/hle/pl.hpp.
#include "lgp/hle/pl.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "lgp/runtime/log.hpp"

#if LGP_HLE_FREE_FONT
#include "lgp/hle/free_font.hpp"
#endif

namespace lgp::hle {
namespace {

using rt::LogLevel;

constexpr uint32_t kBfttfMagic = 0x18029A7F;
constexpr uint32_t kFileKey = 0x06186249;    // .bfttf in the archive (little-endian words)
constexpr uint32_t kMemoryKey = 0x49621806;  // header in shared memory

struct FontFile {
    uint64_t data_id;
    const char* name;
};
// Index = nn::pl::SharedFontType.
constexpr FontFile kFonts[kSharedFontCount] = {
    {0x0100000000000811, "nintendo_udsg-r_std_003.bfttf"},
    {0x0100000000000814, "nintendo_udsg-r_org_zh-cn_003.bfttf"},
    {0x0100000000000814, "nintendo_udsg-r_ext_zh-cn_003.bfttf"},
    {0x0100000000000813, "nintendo_udjxh-db_zh-tw_003.bfttf"},
    {0x0100000000000812, "nintendo_udsg-r_ko_003.bfttf"},
    {0x0100000000000810, "nintendo_ext_003.bfttf"},
};

uint32_t bswap32(uint32_t v) {
    return (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24);
}

uint32_t load_le32(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

void void_ignore(Request&, Response&) {}

struct FontState {
    FontState(platform::IStorage* storage, std::span<const uint8_t> fallback) : game(storage), stand_in(fallback) {}
    platform::IStorage* game;
    std::span<const uint8_t> stand_in;  // TrueType for types 0-4 without the firmware's standard font
    std::mutex mutex;
    bool loaded = false;
    std::array<uint32_t, kSharedFontCount> offset{}, size{};
    std::vector<uint8_t> image;  // shared memory contents
    std::shared_ptr<rt::KSharedMemory> memory;

    std::vector<uint8_t> read_font(const FontFile& f, bool quiet) {
        if (game == nullptr)
            return {};
        char path[96];
        std::snprintf(path, sizeof(path), "/systemdata/%016llx/%s", static_cast<unsigned long long>(f.data_id), f.name);
        std::unique_ptr<platform::IFile> file;
        if (game->open(path, platform::kOpenRead, &file) != platform::StorageError::Ok) {
            if (!quiet)
                rt::log(LogLevel::Warn, "pl: %s not found in the game storage, font stays empty", path);
            return {};
        }
        std::vector<uint8_t> bytes(static_cast<size_t>(file->size()));
        uint64_t got = 0;
        if (file->read(0, bytes.data(), bytes.size(), &got) != platform::StorageError::Ok || got != bytes.size())
            return {};
        std::vector<uint8_t> ttf = decrypt_bfttf(bytes);
        if (ttf.empty())
            rt::log(LogLevel::Warn, "pl: %s is no BFTTF file, font stays empty", path);
        return ttf;
    }

    // Places one font (8-byte header + TrueType) at *at; false if it does not fit.
    bool place(uint32_t* at, std::span<const uint8_t> ttf, uint32_t* out_offset) {
        if (*at + 8 + ttf.size() > image.size())
            return false;
        const uint32_t magic = kBfttfMagic ^ kMemoryKey;
        const uint32_t enc_size = bswap32(static_cast<uint32_t>(ttf.size()) ^ kMemoryKey);
        std::memcpy(image.data() + *at, &magic, 4);
        std::memcpy(image.data() + *at + 4, &enc_size, 4);
        *out_offset = *at + 8;
        if (!ttf.empty())
            std::memcpy(image.data() + *at + 8, ttf.data(), ttf.size());
        *at += 8 + static_cast<uint32_t>(ttf.size());
        return true;
    }

    // Caller holds mutex.
    void load() {
        if (loaded)
            return;
        loaded = true;
        image.assign(kSharedFontMemorySize, 0);
        // Without the firmware's standard font the stand-in serves every
        // type but the Nintendo extension (button icons), all at one offset
        // (as nxnative does); the other firmware fonts are then not looked
        // for either, so a mixed set cannot happen.
        const std::vector<uint8_t> standard = read_font(kFonts[0], !stand_in.empty());
        const bool use_stand_in = standard.empty() && !stand_in.empty();
        uint32_t at = 0;
        uint32_t stand_in_offset = 0;
        if (use_stand_in && !place(&at, stand_in, &stand_in_offset)) {
            rt::log(LogLevel::Error, "pl: the stand-in font (%zu bytes) does not fit into the shared memory",
                    stand_in.size());
        } else if (use_stand_in) {
            rt::log(LogLevel::Info,
                    "pl: no system fonts from the firmware (/systemdata/%016llx); using the free stand-in font "
                    "(M PLUS Rounded 1c, %zu bytes) for types 0-4",
                    static_cast<unsigned long long>(kFonts[0].data_id), stand_in.size());
            for (int i = 0; i < 5; ++i) {
                offset[i] = stand_in_offset;
                size[i] = static_cast<uint32_t>(stand_in.size());
            }
        }
        for (int i = 0; i < kSharedFontCount; ++i) {
            if (use_stand_in && i < 5)
                continue;
            const std::vector<uint8_t> ttf = i == 0 ? standard : read_font(kFonts[i], use_stand_in);
            if (!place(&at, ttf, &offset[i])) {
                rt::log(LogLevel::Warn, "pl: font %d does not fit into the shared memory, left empty", i);
                continue;
            }
            size[i] = static_cast<uint32_t>(ttf.size());
        }
        // Without the standard font LGP's font engine finds no glyphs and the
        // game dereferences a null pointer soon after (guest address 0x38);
        // say why before that happens.
        if (size[0] == 0)
            rt::log(LogLevel::Error,
                    "pl: the standard system font (/systemdata/%016llx/%s) is missing; a game that draws text "
                    "will crash. Copy systemdata/ from your own console dump into the game folder, or build with "
                    "LGP_ENABLE_FREE_FONT (docs/knowledge/horizon-pl-shared-font.md)",
                    static_cast<unsigned long long>(kFonts[0].data_id), kFonts[0].name);
    }

    // Caller holds mutex.
    std::shared_ptr<rt::KSharedMemory> shared_memory(rt::Kernel& kernel) {
        load();
        if (!memory) {
            memory = std::make_shared<rt::KSharedMemory>(kernel, kSharedFontMemorySize, rt::kPermR);
            std::memcpy(memory->data(), image.data(), image.size());
        }
        return memory;
    }
};

class PlatformServiceManager final : public Service {
public:
    explicit PlatformServiceManager(std::shared_ptr<FontState> state) : Service("pl:u"), state_(std::move(state)) {
        on(0, void_ignore);  // RequestLoad(type): the fonts are always loaded
        on(1, [](Request&, Response& resp) { resp.push<uint32_t>(1); });  // GetLoadState(type): 1 = loaded
        on(2, [this](Request& req, Response& resp) {  // GetFontSize(type)
            const uint32_t type = req.pop<uint32_t>();
            std::lock_guard lock(state_->mutex);
            state_->load();
            resp.push<uint32_t>(type < kSharedFontCount ? state_->size[type] : 0);
        });
        on(3, [this](Request& req, Response& resp) {  // GetSharedMemoryAddressOffset(type)
            const uint32_t type = req.pop<uint32_t>();
            std::lock_guard lock(state_->mutex);
            state_->load();
            resp.push<uint32_t>(type < kSharedFontCount ? state_->offset[type] : 0);
        });
        on(4, [this](Request& req, Response& resp) {  // GetSharedMemoryNativeHandle
            std::lock_guard lock(state_->mutex);
            Handle h = rt::kInvalidHandle;
            resp.result = req.kernel().handles().add(state_->shared_memory(req.kernel()), &h);
            if (resp.result.ok())
                resp.copy_handles.push_back(h);
        });
        on(5, [this](Request& req, Response& resp) {  // GetSharedFontInOrderOfPriority(u64 language code)
            // Standard first, then the rest in type order (Ryujinx answers the
            // same for every language); out buffers: types, offsets, sizes.
            std::lock_guard lock(state_->mutex);
            state_->load();
            std::array<uint32_t, kSharedFontCount> types{}, offsets{}, sizes{};
            for (int i = 0; i < kSharedFontCount; ++i) {
                types[i] = static_cast<uint32_t>(i);
                offsets[i] = state_->offset[i];
                sizes[i] = state_->size[i];
            }
            const uint64_t n = std::min<uint64_t>(kSharedFontCount, req.out_buffer(0).size / 4);
            if (!req.write_out(0, types.data(), n * 4) || !req.write_out(1, offsets.data(), n * 4) ||
                !req.write_out(2, sizes.data(), n * 4)) {
                resp.result = rt::kernel_result::kInvalidPointer;
                return;
            }
            resp.push<uint8_t>(1);  // loaded
            resp.push<uint32_t>(static_cast<uint32_t>(n));
        });
    }

private:
    std::shared_ptr<FontState> state_;
};

}  // namespace

std::vector<uint8_t> decrypt_bfttf(const std::vector<uint8_t>& bfttf) {
    if (bfttf.size() < 8 || (load_le32(bfttf.data()) ^ kFileKey) != kBfttfMagic)
        return {};
    std::vector<uint8_t> out((bfttf.size() - 8) / 4 * 4);
    for (size_t i = 0; i < out.size(); i += 4) {
        const uint32_t v = load_le32(bfttf.data() + 8 + i) ^ kFileKey;
        std::memcpy(out.data() + i, &v, 4);
    }
    return out;
}

std::span<const uint8_t> default_stand_in_font() {
#if LGP_HLE_FREE_FONT
    return free_system_font();
#else
    return {};
#endif
}

void register_pl(ServiceManager& sm, platform::IStorage* game, std::span<const uint8_t> stand_in) {
    auto state = std::make_shared<FontState>(game, stand_in);
    sm.register_service("pl:u", [state] { return std::make_shared<PlatformServiceManager>(state); });
}

}  // namespace lgp::hle
