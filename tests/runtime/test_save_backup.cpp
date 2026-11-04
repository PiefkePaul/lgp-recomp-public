// Cloud backup of the app's saves (lgp/boot/save_backup.hpp, P7.4) against a
// cloud in memory: what the archive holds, restore of a newer cloud state,
// upload after a save, and that an older state never replaces a newer one.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "lgp/boot/save_backup.hpp"
#include "lgp/platform/ui_text.hpp"

using namespace lgp;
namespace fs = std::filesystem;

namespace {

int g_failures = 0;

#define CHECK(x)                                                         \
    do {                                                                 \
        if (!(x)) {                                                      \
            std::fprintf(stderr, "FAILED %s:%d: %s\n", __FILE__, __LINE__, #x); \
            ++g_failures;                                                \
        }                                                                \
    } while (0)

void write(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary | std::ios::trunc) << text;
}

std::string read(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// A cloud like CloudKit's private database, in memory.
struct MemoryCloud final : platform::ICloudSave {
    std::mutex mutex;
    std::map<std::string, platform::CloudRecord> records;
    platform::CloudStatus state = platform::CloudStatus::Available;
    bool fail_store = false;
    bool fail_fetch = false;
    int stores = 0;

    // The test's side, under the same lock as the backup's thread.
    void set_fail(bool v) {
        std::lock_guard lock(mutex);
        fail_store = v;
    }
    void set_seq(uint64_t seq) {
        std::lock_guard lock(mutex);
        records[boot::SaveBackup::kRecordKey].seq = seq;
    }
    uint64_t seq() {
        std::lock_guard lock(mutex);
        auto it = records.find(boot::SaveBackup::kRecordKey);
        return it != records.end() ? it->second.seq : 0;
    }
    int store_count() {
        std::lock_guard lock(mutex);
        return stores;
    }

    platform::CloudStatus status(int, std::string* detail) override {
        *detail = state == platform::CloudStatus::Available ? "" : "test";
        return state;
    }
    bool fetch(const std::string& key, int, bool* found, platform::CloudRecord* out, std::string* error) override {
        std::lock_guard lock(mutex);
        if (fail_fetch) {
            *error = "timed out";
            return false;
        }
        auto it = records.find(key);
        *found = it != records.end();
        if (*found)
            *out = it->second;
        return true;
    }
    bool store(const std::string& key, const platform::CloudRecord& r, int, bool* newer, std::string* error) override {
        std::lock_guard lock(mutex);
        ++stores;
        *newer = false;
        if (fail_store) {
            *error = "offline";
            return false;
        }
        auto it = records.find(key);
        if (it != records.end() && it->second.seq >= r.seq) {
            *newer = true;
            *error = "newer";
            return false;
        }
        records[key] = r;
        return true;
    }
};

// A second handle on one cloud (two devices, one iCloud account).
struct SharedCloud final : platform::ICloudSave {
    MemoryCloud* cloud;
    explicit SharedCloud(MemoryCloud* c) : cloud(c) {}
    platform::CloudStatus status(int t, std::string* d) override { return cloud->status(t, d); }
    bool fetch(const std::string& k, int t, bool* f, platform::CloudRecord* o, std::string* e) override {
        return cloud->fetch(k, t, f, o, e);
    }
    bool store(const std::string& k, const platform::CloudRecord& r, int t, bool* n, std::string* e) override {
        return cloud->store(k, r, t, n, e);
    }
};

// A save folder as runtime/hle leaves it: journal, committed slot, work copy.
void journaled_save(const fs::path& dir, int slot, const std::string& content) {
    write(dir / ".lgp-journal", std::to_string(slot) + "\n");
    write(dir / (".lgp-" + std::to_string(slot)) / "slot0.bin", content);
    write(dir / (".lgp-" + std::to_string(1 - slot)) / "slot0.bin", "older");
    write(dir / ".lgp-work" / "slot0.bin", "uncommitted");
}

void test_archive(const fs::path& work) {
    const fs::path data = work / "data";
    journaled_save(data / "saves" / "0100" / "account" / "u1", 1, "committed");
    write(data / "saves-2" / "flat.bin", "flat");
    write(data / "settings.ini", "volume = 50\n");
    write(data / "logs" / "lgp.log", "not backed up");
    write(data / "game" / "exefs" / "main", "not backed up");
    fs::create_directories(data / "saves" / "empty");
    std::vector<uint8_t> archive;
    std::string error;
    CHECK(boot::pack_saves(data, &archive, &error));

    const fs::path to = work / "restored";
    write(to / "saves" / "stale.bin", "replaced");
    write(to / "logs" / "lgp.log", "kept");
    CHECK(boot::unpack_saves(archive, to, &error));
    CHECK(read(to / "saves" / "0100" / "account" / "u1" / "slot0.bin") == "committed");  // flat, mount takes over
    CHECK(!fs::exists(to / "saves" / "0100" / "account" / "u1" / ".lgp-work"));
    CHECK(!fs::exists(to / "saves" / "stale.bin"));
    CHECK(read(to / ".lgp-before-restore" / "saves" / "stale.bin") == "replaced");
    CHECK(read(to / "saves-2" / "flat.bin") == "flat");
    CHECK(read(to / "settings.ini") == "volume = 50\n");
    CHECK(fs::is_directory(to / "saves" / "empty"));
    CHECK(read(to / "logs" / "lgp.log") == "kept");
    CHECK(!fs::exists(to / "game"));

    // Damage anywhere: nothing is written.
    std::vector<uint8_t> bad = archive;
    bad[bad.size() / 2] ^= 0x40;
    const fs::path untouched = work / "untouched";
    write(untouched / "saves" / "a.bin", "a");
    CHECK(!boot::unpack_saves(bad, untouched, &error));
    CHECK(read(untouched / "saves" / "a.bin") == "a");
    bad.resize(10);
    CHECK(!boot::unpack_saves(bad, untouched, &error));
}

void test_backup(const fs::path& work) {
    const fs::path a = work / "device-a", b = work / "device-b";
    auto make = [](const fs::path& data) {
        auto c = std::make_unique<MemoryCloud>();
        MemoryCloud* raw = c.get();
        auto backup = std::make_unique<boot::SaveBackup>(std::move(c), data);
        backup->retry_first = 5;
        backup->retry_delay = 20;
        backup->stop_wait = 50;
        return std::pair(std::move(backup), raw);
    };

    // First start with local saves and an empty cloud: uploaded.
    journaled_save(a / "saves" / "0100" / "account" / "u1", 0, "v1");
    auto [backup, cloud] = make(a);
    backup->restore();
    CHECK(backup->wait_idle(2000));
    CHECK(cloud->records.count(boot::SaveBackup::kRecordKey) == 1);
    CHECK(backup->local_seq() == 1);
    CHECK(backup->note().find("iCloud") != std::string::npos);

    // A save: uploaded with the next sequence number.
    journaled_save(a / "saves" / "0100" / "account" / "u1", 1, "v2");
    backup->committed();
    CHECK(backup->wait_idle(2000));
    CHECK(cloud->records[boot::SaveBackup::kRecordKey].seq == 2);

    // The system emptied the folder: the next start restores from the cloud.
    const platform::CloudRecord kept = cloud->records[boot::SaveBackup::kRecordKey];
    backup.reset();
    fs::remove_all(a);
    fs::create_directories(a);
    auto [again, c2] = make(a);
    c2->records[boot::SaveBackup::kRecordKey] = kept;
    // The notes are in the interface language (lgp/platform/ui_text.hpp).
    platform::set_ui_language(platform::UiLanguage::German);
    again->restore();
    platform::set_ui_language(platform::UiLanguage::English);
    CHECK(read(a / "saves" / "0100" / "account" / "u1" / "slot0.bin") == "v2");
    CHECK(again->local_seq() == 2);
    CHECK(again->note().find("wiederhergestellt") != std::string::npos);

    // An older local state never replaces a newer cloud record.
    c2->set_seq(9);
    again->committed();
    CHECK(again->wait_idle(2000));
    CHECK(c2->seq() == 9);
    CHECK(again->note().find("Warning") != std::string::npos);

    // Offline: retried until it works.
    c2->set_seq(2);
    c2->set_fail(true);
    again->committed();
    CHECK(!again->wait_idle(200));
    c2->set_fail(false);
    CHECK(again->wait_idle(2000));
    CHECK(c2->seq() == 3);

    // An upload that never happened (offline until the app ended) is made up
    // for at the next start.
    journaled_save(a / "saves" / "0100" / "account" / "u1", 0, "v3");
    c2->set_fail(true);
    again->committed();
    platform::CloudRecord at3;  // c2 goes with its backup
    {
        std::lock_guard lock(c2->mutex);
        at3 = c2->records[boot::SaveBackup::kRecordKey];
    }
    again.reset();
    CHECK(at3.seq == 3);
    auto [next, c4] = make(a);
    c4->records[boot::SaveBackup::kRecordKey] = at3;
    next->restore();
    CHECK(next->wait_idle(2000));
    CHECK(c4->seq() == 4);
    CHECK(read(a / "saves" / "0100" / "account" / "u1" / ".lgp-0" / "slot0.bin") == "v3");  // not restored over

    // Both changed: saves here not uploaded, a newer state from elsewhere.
    // This device wins, the cloud state is kept beside it.
    journaled_save(a / "saves" / "0100" / "account" / "u1", 1, "v4");
    c4->set_fail(true);
    next->committed();
    platform::CloudRecord other;
    {
        std::lock_guard lock(c4->mutex);
        other = c4->records[boot::SaveBackup::kRecordKey];
    }
    next.reset();
    other.seq = 7;
    auto [both, c5] = make(a);
    c5->records[boot::SaveBackup::kRecordKey] = other;
    both->restore();
    CHECK(both->wait_idle(2000));
    CHECK(c5->seq() == 8);
    CHECK(read(a / "saves" / "0100" / "account" / "u1" / ".lgp-1" / "slot0.bin") == "v4");
    CHECK(fs::file_size(a / ".lgp-cloud-replaced.bin") == other.data.size());
    both.reset();

    // Fetch failed at the start: nothing uploaded, the save is kept for later.
    auto [blind, c6] = make(a);
    c6->fail_fetch = true;
    blind->restore();
    blind->committed();
    CHECK(blind->wait_idle(200));
    CHECK(c6->store_count() == 0);
    CHECK(read(a / ".lgp-cloud").find("dirty") != std::string::npos);

    // No iCloud account: a warning, nothing restored or uploaded.
    auto [none, c3] = make(b);
    c3->state = platform::CloudStatus::NoAccount;
    none->restore();
    CHECK(none->note().find("no iCloud account") != std::string::npos);
    CHECK(none->note().find("tvOS") == std::string::npos);  // iOS keeps its saves
    CHECK(c3->store_count() == 0);

    // Only tvOS warns that the system may delete the saves.
    for (const char* tag : {"ios", "tvos"}) {
        auto c = std::make_unique<MemoryCloud>();
        c->state = platform::CloudStatus::NoAccount;
        boot::SaveBackup tagged(std::move(c), b, tag);
        tagged.restore();
        CHECK((tagged.note().find("tvOS") != std::string::npos) == (std::string_view(tag) == "tvos"));
    }
}

// Device B uploads a newer state; device A, still on the older one, saves:
// within A's run the cloud keeps B's state.
void test_two_devices(const fs::path& work) {
    MemoryCloud cloud;
    const fs::path a = work / "two-a", b = work / "two-b";
    journaled_save(a / "saves" / "0100" / "account" / "u1", 0, "a1");
    boot::SaveBackup da(std::make_unique<SharedCloud>(&cloud), a);
    da.retry_first = 5;
    da.retry_delay = 20;
    da.restore();
    CHECK(da.wait_idle(2000));
    CHECK(cloud.seq() == 1);
    boot::SaveBackup db(std::make_unique<SharedCloud>(&cloud), b);
    db.restore();  // takes state 1 from A
    CHECK(read(b / "saves" / "0100" / "account" / "u1" / "slot0.bin") == "a1");
    journaled_save(b / "saves" / "0100" / "account" / "u1", 1, "b2");
    db.committed();
    CHECK(db.wait_idle(2000));
    CHECK(cloud.seq() == 2);
    journaled_save(a / "saves" / "0100" / "account" / "u1", 1, "a2");
    da.committed();
    CHECK(da.wait_idle(2000));
    CHECK(cloud.seq() == 2);
    platform::CloudRecord kept;
    bool found = false;
    std::string error;
    cloud.fetch(boot::SaveBackup::kRecordKey, 0, &found, &kept, &error);
    const fs::path check = work / "two-check";
    CHECK(boot::unpack_saves(kept.data, check, &error));
    CHECK(read(check / "saves" / "0100" / "account" / "u1" / "slot0.bin") == "b2");
    CHECK(da.note().find("Warning") != std::string::npos);
}

// A device that played before it had the cloud: its saves are not restored
// over; the other state is offered and, when taken, the device's saves stay
// in a folder of their own.
void test_cloud_choice(const fs::path& work) {
    MemoryCloud cloud;
    const fs::path tv = work / "choice-tv", pad = work / "choice-pad";
    journaled_save(tv / "saves" / "0100" / "account" / "u1", 0, "tv");
    {
        boot::SaveBackup dt(std::make_unique<SharedCloud>(&cloud), tv);
        dt.restore();
        CHECK(dt.wait_idle(2000));
    }
    CHECK(cloud.seq() == 1);

    journaled_save(pad / "saves" / "0100" / "account" / "u1", 0, "pad");
    boot::SaveBackup dp(std::make_unique<SharedCloud>(&cloud), pad);
    dp.retry_first = 5;
    dp.retry_delay = 20;
    dp.restore();
    CHECK(dp.wait_idle(2000));
    CHECK(read(pad / "saves" / "0100" / "account" / "u1" / ".lgp-0" / "slot0.bin") == "pad");
    CHECK(dp.cloud_choice() == 1);
    CHECK(fs::exists(pad / ".lgp-cloud-replaced.bin"));
    CHECK(cloud.seq() == 2);  // the pad's saves went up

    std::string error;
    CHECK(dp.use_cloud_state(&error));
    CHECK(dp.wait_idle(2000));
    CHECK(read(pad / "saves" / "0100" / "account" / "u1" / "slot0.bin") == "tv");
    CHECK(dp.cloud_choice() == 0);
    CHECK(!fs::exists(pad / ".lgp-cloud-replaced.bin"));
    CHECK(!fs::exists(pad / ".lgp-before-restore"));
    bool kept = false;
    for (const auto& e : fs::directory_iterator(pad))
        if (e.path().filename().string().rfind(".lgp-device-saves-", 0) == 0)
            kept = read(e.path() / "saves" / "0100" / "account" / "u1" / ".lgp-0" / "slot0.bin") == "pad";
    CHECK(kept);
    CHECK(cloud.seq() == 3);  // the tv state is the newest again
    platform::CloudRecord now;
    bool found = false;
    cloud.fetch(boot::SaveBackup::kRecordKey, 0, &found, &now, &error);
    const fs::path check = work / "choice-check";
    CHECK(boot::unpack_saves(now.data, check, &error));
    CHECK(read(check / "saves" / "0100" / "account" / "u1" / "slot0.bin") == "tv");
    CHECK(!dp.use_cloud_state(&error));  // nothing left to choose
}

// The choice outlives a restart, takes the cloud state alone (no slot of
// this device stays), and the conflict upload keeps the TV's settings.
void test_cloud_choice_restart(const fs::path& work) {
    MemoryCloud cloud;
    const fs::path tv = work / "restart-tv", pad = work / "restart-pad";
    write(tv / "settings.ini", "args = tv\n");
    journaled_save(tv / "saves" / "0100" / "account" / "u1", 0, "tv");
    {
        boot::SaveBackup dt(std::make_unique<SharedCloud>(&cloud), tv, "tvos");
        dt.restore();
        CHECK(dt.wait_idle(2000));
    }
    write(pad / "settings.ini", "args = pad\n");
    journaled_save(pad / "saves" / "0100" / "account" / "u1", 0, "pad");
    write(pad / "saves-2" / "s.bin", "pad slot 2");
    {
        boot::SaveBackup dp(std::make_unique<SharedCloud>(&cloud), pad, "ios");
        dp.restore();
        CHECK(dp.wait_idle(2000));
        CHECK(dp.cloud_choice() == 1);
    }
    CHECK(read(pad / "settings-tvos.ini") == "args = tv\n");
    std::string error;
    platform::CloudRecord now;
    bool found = false;
    cloud.fetch(boot::SaveBackup::kRecordKey, 0, &found, &now, &error);
    const fs::path check = work / "restart-check";
    CHECK(boot::unpack_saves(now.data, check, &error, "tvos"));
    CHECK(read(check / "settings.ini") == "args = tv\n");  // the TV's settings stayed in the record

    boot::SaveBackup again(std::make_unique<SharedCloud>(&cloud), pad, "ios");
    CHECK(again.cloud_choice() == 1);
    again.restore();
    CHECK(again.wait_idle(2000));
    CHECK(again.cloud_choice() == 1);
    CHECK(again.use_cloud_state(&error));
    CHECK(again.wait_idle(2000));
    CHECK(read(pad / "saves" / "0100" / "account" / "u1" / "slot0.bin") == "tv");
    CHECK(!fs::exists(pad / "saves-2"));
    CHECK(read(pad / "settings.ini") == "args = pad\n");
    CHECK(!fs::exists(pad / ".lgp-cloud-choice"));
    bool kept = false;
    for (const auto& e : fs::directory_iterator(pad))
        if (e.path().filename().string().rfind(".lgp-device-saves-", 0) == 0)
            kept = read(e.path() / "saves-2" / "s.bin") == "pad slot 2";
    CHECK(kept);
    boot::SaveBackup last(std::make_unique<SharedCloud>(&cloud), pad, "ios");
    CHECK(last.cloud_choice() == 0);
}

// iPhone and iPad share the tag "ios": each keeps its own settings.ini, a
// folder without one (emptied cache) gets the record's back.
void test_settings_same_tag(const fs::path& work) {
    const fs::path phone = work / "same-phone", pad = work / "same-pad", fresh = work / "same-fresh";
    write(phone / "settings.ini", "args = phone\n");
    write(phone / "saves" / "s.bin", "phone save");
    std::vector<uint8_t> archive;
    std::string error;
    CHECK(boot::pack_saves(phone, &archive, &error, "ios"));
    write(pad / "settings.ini", "args = pad\n");
    CHECK(boot::unpack_saves(archive, pad, &error, "ios"));
    CHECK(read(pad / "saves" / "s.bin") == "phone save");
    CHECK(read(pad / "settings.ini") == "args = pad\n");
    CHECK(boot::unpack_saves(archive, fresh, &error, "ios"));
    CHECK(read(fresh / "settings.ini") == "args = phone\n");
}

// iPhone and Apple TV share one record: the saves move, each kind keeps its
// own settings, and a record of the tvOS app before sharing still restores.
void test_settings_tags(const fs::path& work) {
    const fs::path tv = work / "tag-tv", phone = work / "tag-phone";
    write(tv / "settings.ini", "args = tv\n");
    write(tv / "saves" / "s.bin", "tv save");
    std::vector<uint8_t> archive;
    std::string error;
    CHECK(boot::pack_saves(tv, &archive, &error, "tvos"));
    std::vector<uint8_t> untagged;
    CHECK(boot::pack_saves(tv, &untagged, &error));
    CHECK(archive == untagged);  // tvOS records look as before

    write(phone / "settings.ini", "args = phone\n");
    CHECK(boot::unpack_saves(archive, phone, &error, "ios"));
    CHECK(read(phone / "saves" / "s.bin") == "tv save");
    CHECK(read(phone / "settings.ini") == "args = phone\n");
    CHECK(read(phone / "settings-tvos.ini") == "args = tv\n");  // inert, goes up again

    write(phone / "saves" / "s.bin", "phone save");
    CHECK(boot::pack_saves(phone, &archive, &error, "ios"));
    CHECK(boot::unpack_saves(archive, tv, &error, "tvos"));
    CHECK(read(tv / "saves" / "s.bin") == "phone save");
    CHECK(read(tv / "settings.ini") == "args = tv\n");
    CHECK(read(tv / "settings-ios.ini") == "args = phone\n");
    CHECK(!fs::exists(tv / "settings-tvos.ini"));

    // A device without settings yet takes none from another kind.
    const fs::path pad = work / "tag-pad";
    CHECK(boot::unpack_saves(archive, pad, &error, "ios"));
    CHECK(read(pad / "settings.ini") == "args = phone\n");  // same kind
    const fs::path tv2 = work / "tag-tv2";
    std::vector<uint8_t> phone_only;
    fs::remove(phone / "settings-tvos.ini");
    CHECK(boot::pack_saves(phone, &phone_only, &error, "ios"));
    CHECK(boot::unpack_saves(phone_only, tv2, &error, "tvos"));
    CHECK(!fs::exists(tv2 / "settings.ini"));
    CHECK(read(tv2 / "saves" / "s.bin") == "phone save");
}

}  // namespace

int main() {
    const fs::path work = fs::temp_directory_path() / "lgp-save-backup-test";
    fs::remove_all(work);
    test_archive(work);
    test_backup(work);
    test_two_devices(work);
    test_cloud_choice(work);
    test_cloud_choice_restart(work);
    test_settings_tags(work);
    test_settings_same_tag(work);
    fs::remove_all(work);
    if (g_failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("all checks passed\n");
    return 0;
}
