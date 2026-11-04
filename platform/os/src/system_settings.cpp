// Settings in the system's Settings app (system_settings.hpp): the sync rule,
// and no store where the platform has none.
#include "lgp/platform/system_settings.hpp"

namespace lgp::platform {

void sync_system_settings(ISettingsStore& store, std::vector<std::pair<std::string, std::string>>* values,
                          std::vector<std::string>* taken) {
    for (auto& [key, value] : *values) {
        const std::optional<std::string> shown = store.get(key);
        const std::optional<std::string> written = store.get("lgp.mirror." + key);
        if (shown && shown != written) {
            value = *shown;
            taken->push_back(key);
        }
        store.set(key, value);
        store.set("lgp.mirror." + key, value);
    }
}

#if !defined(LGP_PLATFORM_SYSTEM_SETTINGS)
std::unique_ptr<ISettingsStore> system_settings() { return nullptr; }
#endif

}  // namespace lgp::platform
