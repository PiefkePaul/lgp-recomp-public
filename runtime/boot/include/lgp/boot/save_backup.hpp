// Backup of the app's save data in the user's cloud (P7.4, tvOS: the saves
// live in Library/Caches, which the system may empty while the app is not
// running; docs/knowledge/tvos-als-zielplattform.md). The cloud itself is
// lgp/platform/cloud_save.hpp; this file decides what is backed up and when.
//
// What: below the app's data folder settings.ini and every save slot
// (saves/, saves-<n>/), each journaled save as its committed state
// (runtime/hle save_data: .lgp-journal names the slot; restored flat, which
// the next mount takes over). Logs, the game, pause-menu backups and the
// journal's work copies are left out. One archive, one cloud record.
//
// When: restore() at the start of the app, before settings and slots are
// read; committed() after each save (FsConfig::on_commit) packs at once and
// uploads in the background. A sequence number in <data>/.lgp-cloud orders
// the two sides, with a mark for saves the cloud has not taken yet (also
// across starts: the next restore() uploads them). The cloud wins when it is
// newer and nothing here is unsaved (another device, or the folder was
// emptied); an upload never replaces a newer cloud record. If both changed,
// this device's saves win and the cloud state is kept in
// <data>/.lgp-cloud-replaced.bin (an archive for unpack_saves); the start
// screen then offers it (cloud_choice(), use_cloud_state()). The first start
// with the cloud (no <data>/.lgp-cloud yet) counts saves already in the
// folder as unsaved: they are not restored over. Without a usable cloud (no
// account, fetch failed) nothing is uploaded.
//
// Several kinds of device (iPhone/iPad and Apple TV) share one record: the
// saves move between them, the settings do not. With a settings tag the
// device's settings.ini is archived as settings-<tag>.ini (tvOS: as plain
// settings.ini, as before sharing); on restore only its own becomes
// settings.ini, and only where the folder has none (tvOS emptied its cache):
// iPhone and iPad share the tag "ios", and a device's settings.ini stays its
// own. The others are kept as inert files settings-<tag>.ini so that the
// next upload carries them along. Without a tag settings.ini is archived and
// restored as is.
#pragma once

#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "lgp/platform/cloud_save.hpp"

namespace lgp::boot {

// The archive of the backed-up files below data (empty folders included).
bool pack_saves(const std::filesystem::path& data, std::vector<uint8_t>* out, std::string* error,
                const std::string& settings_tag = {});
// Writes an archive into data. Each top-level entry it holds replaces the one
// in data; the replaced ones are kept in <data>/.lgp-before-restore/ until
// the next restore. Nothing is written when the archive is damaged.
bool unpack_saves(const std::vector<uint8_t>& archive, const std::filesystem::path& data, std::string* error,
                  const std::string& settings_tag = {});

class SaveBackup {
public:
    static constexpr const char* kRecordKey = "saves";

    SaveBackup(std::unique_ptr<platform::ICloudSave> cloud, std::filesystem::path data, std::string settings_tag = {});
    // Waits a little for an upload still running (the last save).
    ~SaveBackup();
    SaveBackup(const SaveBackup&) = delete;
    SaveBackup& operator=(const SaveBackup&) = delete;

    // Blocks until the cloud answered or timed out. Restores a newer cloud
    // state; uploads the local one when the cloud has none or an older one.
    void restore();
    // After a save: packs now (the caller holds the save's lock), uploads
    // later. Saves in quick succession upload only the last.
    void committed();
    // A line for the start screen: whether the saves are backed up.
    std::string note() const;
    // The sequence number of a newer cloud state this device's saves
    // replaced (kept beside them), else 0. Stays offered across restarts
    // until taken or a regular restore makes it moot.
    uint64_t cloud_choice() const;
    // The user takes that cloud state after all: it replaces the saves here,
    // all of which (every saves/saves-* slot) move to
    // <data>/.lgp-device-saves-<time>/ and stay there, and it goes up again
    // as the newest state. False and *error if it cannot.
    bool use_cloud_state(std::string* error);
    // Test hooks: wait until no upload is pending (false on timeout).
    bool wait_idle(int timeout_ms);
    uint64_t local_seq() const;

    // Timeouts (ms); tests make them short. A failed upload is retried after
    // retry_first, doubling up to retry_delay.
    // restore() blocks the start for at most status_timeout + fetch_timeout.
    int status_timeout = 5000, fetch_timeout = 10000, store_timeout = 60000, retry_first = 5000, retry_delay = 60000;
    int stop_wait = 10000;  // the destructor's wait for a pending upload

private:
    void worker();
    void set_note(std::string text);
    void set_state(uint64_t seq, bool dirty);
    void save_state();  // seq_ and dirty_ to <data>/.lgp-cloud

    std::unique_ptr<platform::ICloudSave> cloud_;
    const std::filesystem::path data_;
    const std::string settings_tag_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::optional<std::vector<uint8_t>> pending_;  // newest archive not uploaded yet
    bool uploading_ = false;
    bool stop_ = false;
    bool usable_ = false;  // restore() reached the cloud and found no reason to stay out
    uint64_t seq_ = 0;     // of the state both sides agree on
    bool dirty_ = false;   // saves changed since the cloud took state seq_
    bool known_ = false;   // <data>/.lgp-cloud existed: this folder was backed up before
    uint64_t choice_ = 0;  // cloud_choice()
    std::mutex file_mutex_;  // <data>/.lgp-cloud
    std::string note_;
    std::thread thread_;
};

}  // namespace lgp::boot
