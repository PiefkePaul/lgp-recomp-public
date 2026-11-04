// Settings in the system's Settings app (lgp/platform/system_settings.hpp):
// the sync rule against an in-memory store. What the user changed there
// since the last sync wins over settings.ini; everything else is written back.
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "../runtime/check.hpp"
#include "lgp/platform/system_settings.hpp"

using namespace lgp::platform;

namespace {

struct MemoryStore final : ISettingsStore {
    std::map<std::string, std::string> values;
    std::optional<std::string> get(const std::string& key) const override {
        const auto it = values.find(key);
        return it == values.end() ? std::nullopt : std::optional<std::string>(it->second);
    }
    void set(const std::string& key, const std::string& value) override { values[key] = value; }
};

using Values = std::vector<std::pair<std::string, std::string>>;

void test_sync() {
    MemoryStore store;
    // First start: nothing in the store; settings.ini goes there.
    Values v = {{"controller", "handheld"}, {"ldn_server", ""}};
    std::vector<std::string> taken;
    sync_system_settings(store, &v, &taken);
    CHECK(taken.empty());
    CHECK(store.values["controller"] == "handheld" && store.values["lgp.mirror.controller"] == "handheld");
    CHECK(store.values.count("ldn_server") == 1 && store.values["ldn_server"].empty());
    // The user picks Joy-Con rechts in the Settings app: taken at the next start.
    store.values["controller"] = "joycon-right";
    sync_system_settings(store, &v, &taken);
    CHECK(taken.size() == 1 && taken[0] == "controller");
    CHECK(v[0].second == "joycon-right");
    CHECK(store.values["lgp.mirror.controller"] == "joycon-right");
    // settings.ini changed elsewhere (iCloud, the menu, by hand), the store
    // untouched since: settings.ini wins and the store follows.
    taken.clear();
    v[0].second = "joycon-left";
    sync_system_settings(store, &v, &taken);
    CHECK(taken.empty());
    CHECK(store.values["controller"] == "joycon-left");
    // A value set in the Settings app before the app ever ran (no mirror yet).
    MemoryStore fresh;
    fresh.values["ldn_room"] = "pauls geraete";
    Values w = {{"ldn_room", ""}};
    sync_system_settings(fresh, &w, &taken);
    CHECK(taken.size() == 1 && w[0].second == "pauls geraete");
}

}  // namespace

int main() {
    RUN(test_sync);
    return finish();
}
