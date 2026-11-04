// set (ISettingsServer): system language and region.
#include <algorithm>
#include <array>

#include "lgp/hle/services.hpp"

namespace lgp::hle {
namespace {

// LanguageCode list in Language enum order (switchbrew "Settings services").
// GetAvailableLanguageCodes (pre-4.0.0 command) reports the first 15.
constexpr std::array<const char*, 18> kLanguages{"ja",      "en-US", "fr",    "de",      "it",      "es",
                                                 "zh-CN",   "ko",    "nl",    "pt",      "ru",      "zh-TW",
                                                 "en-GB",   "fr-CA", "es-419", "zh-Hans", "zh-Hant", "pt-BR"};
constexpr int32_t kLegacyLanguageCount = 15;

class SettingsServer final : public Service {
public:
    explicit SettingsServer(const SystemConfig& config) : Service("set"), config_(config) {
        on(0, [this](Request&, Response& resp) { resp.push<uint64_t>(language_code(config_.language)); });
        on(1, [](Request& req, Response& resp) { write_codes(req, resp, kLegacyLanguageCount); });
        on(3, [](Request&, Response& resp) { resp.push<int32_t>(kLegacyLanguageCount); });
        on(4, [this](Request&, Response& resp) { resp.push<int32_t>(config_.region); });
        on(5, [](Request& req, Response& resp) {
            write_codes(req, resp, static_cast<int32_t>(kLanguages.size()));
        });
        on(6, [](Request&, Response& resp) { resp.push<int32_t>(static_cast<int32_t>(kLanguages.size())); });
    }

private:
    // Out buffer of LanguageCodes (0xA for command 1, 0x6 for command 5) -> s32 count.
    static void write_codes(Request& req, Response& resp, int32_t total) {
        const HipcBuffer out = req.out_buffer(0);
        const int32_t n = static_cast<int32_t>(std::min<uint64_t>(out.size / 8, static_cast<uint64_t>(total)));
        std::vector<uint64_t> codes;
        for (int32_t i = 0; i < n; ++i)
            codes.push_back(language_code(kLanguages[static_cast<size_t>(i)]));
        if (n > 0 && !req.write_out(0, codes.data(), codes.size() * 8)) {
            resp.result = rt::kernel_result::kInvalidPointer;
            return;
        }
        resp.push<int32_t>(n);
    }

    SystemConfig config_;
};

}  // namespace

void register_set(ServiceManager& sm, const SystemConfig& config) {
    sm.register_service("set", [config] { return std::make_shared<SettingsServer>(config); });
}

}  // namespace lgp::hle
