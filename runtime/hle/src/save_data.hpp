// Save data with commit semantics (P5.3) for fsp-srv: what a game writes
// becomes durable only on Commit, a crash or a close without Commit keeps
// the last committed state (docs/knowledge/horizon-fsp-srv-romfs.md).
//
// Layout of a journaled save below its root in the save storage (the guest
// sees only the working copy; it cannot reach these names, its "/" is the
// working copy):
//   .lgp-journal       "0" or "1": which slot holds the committed state
//   .lgp-journal.tmp   the next value while it is being switched
//   .lgp-0, .lgp-1     committed slot and spare
//   .lgp-work          working copy, reset from the committed slot on mount
//
// The same scheme as LibHac's DirectorySaveDataFileSystem (a working and a
// committed directory), but the switch is a one-byte file instead of a
// directory rename, which the browser's OPFS cannot do.
//
// Anything else in the root (a save from before P5.3, files put there by
// hand or by the browser's save import) is taken over on mount: copied over
// the committed state, committed, then removed.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "lgp/platform/storage.hpp"

namespace lgp::hle::detail {

using platform::IStorage;
using platform::StorageError;

// Copies file or directory `from` to `to` (which must not exist).
StorageError copy_tree(IStorage& storage, const std::string& from, const std::string& to);

// Renames a directory; where the storage cannot (OPFS), copies it and
// removes the source. The caller has checked that `to` does not exist.
StorageError rename_directory(IStorage& storage, const std::string& from, const std::string& to);

class SaveData {
public:
    // Mounts the save at root (an existing directory). journaled = false
    // (temporary storage): the guest works on root directly and Commit only
    // calls IStorage::commit.
    static std::shared_ptr<SaveData> mount(IStorage& storage, const std::string& root, bool journaled,
                                           StorageError* error);

    IStorage& storage() { return storage_; }
    // The guest's "/".
    const std::string& work_root() const { return work_; }
    // Held around every file system command, so Commit never runs in the
    // middle of another command on the same save.
    std::mutex& mutex() { return mutex_; }

    // Files opened for writing; Commit refuses while there are any.
    void writer_opened() { ++writers_; }
    void writer_closed() { --writers_; }
    int writers() const { return writers_; }

    // Makes the working copy the committed state. Call with mutex() held.
    StorageError commit();

private:
    friend class SaveRegistry;
    SaveData(IStorage& storage, std::string root, bool journaled);
    StorageError mount_journaled();
    StorageError take_over_foreign(const std::vector<std::string>& foreign);
    StorageError switch_to(int slot);
    std::string slot_path(int slot) const;

    IStorage& storage_;
    const std::string root_;
    const bool journaled_;
    std::string work_;
    int committed_ = -1;  // slot with the committed state
    std::function<void()> on_commit_;  // FsConfig::on_commit, journaled saves only
    std::mutex mutex_;
    std::atomic<int> writers_{0};
};

// One SaveData per root while any session or file keeps it alive: sessions
// of the same save share the working copy, and when the last one closes the
// uncommitted changes are dropped (the next mount starts from the committed
// state), as on the console.
class SaveRegistry {
public:
    explicit SaveRegistry(std::function<void()> on_commit = {}) : on_commit_(std::move(on_commit)) {}
    std::shared_ptr<SaveData> mount(IStorage& storage, const std::string& root, bool journaled,
                                    StorageError* error);

private:
    std::function<void()> on_commit_;
    std::mutex mutex_;
    std::map<std::string, std::weak_ptr<SaveData>> mounted_;
};

}  // namespace lgp::hle::detail
