// Settings in the system's Settings app (iOS/iPadOS: the app's Settings.bundle,
// Einstellungen > Apps > LGP; platform/ios/Settings.bundle). The app keeps
// settings.ini as its one source (iCloud backs it up); at each start
// sync_system_settings() takes over what the user changed in the Settings
// app since the app last wrote it there, then writes all values back so the
// Settings app shows what the app uses.
//
//   iOS/iPadOS: NSUserDefaults (platform/apple/src/system_settings.mm)
//   elsewhere:  none (system_settings() returns nullptr; tvOS has no app
//               settings in its Settings app)
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace lgp::platform {

// String values by key (the Settings app's bundle uses strings throughout).
class ISettingsStore {
public:
    virtual ~ISettingsStore() = default;
    virtual std::optional<std::string> get(const std::string& key) const = 0;
    virtual void set(const std::string& key, const std::string& value) = 0;
};

// The Settings app's store, nullptr where the platform has none.
std::unique_ptr<ISettingsStore> system_settings();

// `values`: key and value as settings.ini has them. A key whose value in
// `store` differs from what the last sync wrote there (kept under
// "lgp.mirror.<key>"; none yet: any value the user set before the app's first
// start) was changed by the user: its value replaces the one in `values` and
// its key goes to *taken. Then every value of `values` is written to the store.
void sync_system_settings(ISettingsStore& store, std::vector<std::pair<std::string, std::string>>* values,
                          std::vector<std::string>* taken);

}  // namespace lgp::platform
