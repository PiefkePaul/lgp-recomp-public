// Save data with commit semantics, see save_data.hpp.
#include "save_data.hpp"

#include <algorithm>
#include <vector>

#include "lgp/runtime/log.hpp"

namespace lgp::hle::detail {
namespace {

using platform::DirEntry;
using rt::LogLevel;

constexpr const char* kJournal = "/.lgp-journal";
constexpr const char* kJournalTmp = "/.lgp-journal.tmp";
constexpr const char* kWork = "/.lgp-work";
constexpr uint64_t kCopyChunk = 1ull << 20;

bool exists(IStorage& s, const std::string& path) {
    DirEntry e;
    return s.stat(path, &e) == StorageError::Ok;
}

// Removes a file or a directory tree; a missing path is fine.
StorageError remove_tree(IStorage& s, const std::string& path) {
    DirEntry e;
    const StorageError st = s.stat(path, &e);
    if (st == StorageError::NotFound)
        return StorageError::Ok;
    if (st != StorageError::Ok)
        return st;
    return e.is_directory ? s.remove_directory(path, true) : s.remove_file(path);
}

// Names in the root that belong to the journal itself.
bool is_journal_name(const std::string& name) { return name.rfind(".lgp-", 0) == 0; }

// The slot a journal file names, or -1.
int read_slot(IStorage& s, const std::string& path) {
    std::unique_ptr<platform::IFile> f;
    if (s.open(path, platform::kOpenRead, &f) != StorageError::Ok)
        return -1;
    char buf[4] = {};
    uint64_t got = 0;
    if (f->read(0, buf, sizeof(buf), &got) != StorageError::Ok || got == 0)
        return -1;
    if ((buf[0] != '0' && buf[0] != '1') || (got > 1 && buf[1] != '\n'))
        return -1;
    return buf[0] - '0';
}

StorageError write_file(IStorage& s, const std::string& path, const std::string& text) {
    StorageError e = s.create_file(path, 0);
    if (e != StorageError::Ok)
        return e;
    std::unique_ptr<platform::IFile> f;
    e = s.open(path, platform::kOpenWrite, &f);
    if (e == StorageError::Ok)
        e = f->write(0, text.data(), text.size());
    if (e == StorageError::Ok)
        e = f->flush();
    return e;
}

}  // namespace

StorageError copy_tree(IStorage& storage, const std::string& from, const std::string& to) {
    DirEntry e;
    StorageError err = storage.stat(from, &e);
    if (err != StorageError::Ok)
        return err;
    if (e.is_directory) {
        err = storage.create_directory(to);
        std::vector<DirEntry> list;
        if (err == StorageError::Ok)
            err = storage.list(from, &list);
        for (size_t i = 0; i < list.size() && err == StorageError::Ok; ++i)
            err = copy_tree(storage, from + "/" + list[i].name, to + "/" + list[i].name);
        return err;
    }
    std::unique_ptr<platform::IFile> src;
    std::unique_ptr<platform::IFile> dst;
    err = storage.open(from, platform::kOpenRead, &src);
    if (err == StorageError::Ok)
        err = storage.create_file(to, 0);
    if (err == StorageError::Ok)
        err = storage.open(to, platform::kOpenWrite, &dst);
    if (err != StorageError::Ok)
        return err;
    const uint64_t size = src->size();
    std::vector<uint8_t> buf(static_cast<size_t>(std::min(size, kCopyChunk)));
    for (uint64_t at = 0; at < size;) {
        uint64_t got = 0;
        err = src->read(at, buf.data(), std::min(size - at, kCopyChunk), &got);
        if (err != StorageError::Ok)
            return err;
        if (got == 0)
            break;  // shrank meanwhile
        err = dst->write(at, buf.data(), got);
        if (err != StorageError::Ok)
            return err;
        at += got;
    }
    return dst->flush();
}

StorageError rename_directory(IStorage& storage, const std::string& from, const std::string& to) {
    const StorageError err = storage.rename(from, to);
    if (err != StorageError::Io)
        return err;
    // WasmFS on OPFS answers a directory rename with EBUSY (Io here).
    rt::log(LogLevel::Debug, "fsp-srv: renaming %s by copying", from.c_str());
    StorageError e = copy_tree(storage, from, to);
    if (e != StorageError::Ok) {
        remove_tree(storage, to);
        return e;
    }
    e = storage.remove_directory(from, true);
    if (e != StorageError::Ok)
        rt::log(LogLevel::Error, "fsp-srv: copied %s to %s but cannot remove the source: %s", from.c_str(),
                to.c_str(), platform::storage_error_name(e));
    return e;
}

SaveData::SaveData(IStorage& storage, std::string root, bool journaled)
    : storage_(storage), root_(std::move(root)), journaled_(journaled), work_(journaled ? root_ + kWork : root_) {}

std::shared_ptr<SaveData> SaveData::mount(IStorage& storage, const std::string& root, bool journaled,
                                          StorageError* error) {
    std::shared_ptr<SaveData> save(new SaveData(storage, root, journaled));
    *error = journaled ? save->mount_journaled() : StorageError::Ok;
    if (*error != StorageError::Ok) {
        rt::log(LogLevel::Error, "fsp-srv: cannot mount save %s: %s", root.c_str(), platform::storage_error_name(*error));
        return nullptr;
    }
    return save;
}

std::string SaveData::slot_path(int slot) const { return root_ + (slot == 0 ? "/.lgp-0" : "/.lgp-1"); }

StorageError SaveData::mount_journaled() {
    std::vector<DirEntry> list;
    StorageError err = storage_.list(root_, &list);
    if (err != StorageError::Ok)
        return err;
    std::vector<std::string> foreign;
    for (const DirEntry& e : list)
        if (!is_journal_name(e.name))
            foreign.push_back(e.name);

    // Which slot is committed. A crash in switch_to() can leave only the
    // temporary journal (complete: it is written before the old one goes).
    int slot = read_slot(storage_, root_ + kJournal);
    if (slot < 0) {
        slot = read_slot(storage_, root_ + kJournalTmp);
        if (slot >= 0) {
            remove_tree(storage_, root_ + kJournal);
            err = storage_.rename(root_ + kJournalTmp, root_ + kJournal);
            if (err != StorageError::Ok)
                return err;
        }
    } else {
        remove_tree(storage_, root_ + kJournalTmp);
    }
    if (slot >= 0 && !exists(storage_, slot_path(slot))) {
        rt::log(LogLevel::Error, "fsp-srv: %s names a missing slot; save %s starts empty", kJournal + 1,
                root_.c_str());
        slot = -1;
    }
    committed_ = slot;

    if (committed_ < 0 || !foreign.empty()) {
        err = take_over_foreign(foreign);
        if (err != StorageError::Ok)
            return err;
    }
    // Leftovers of a commit that did not finish.
    remove_tree(storage_, slot_path(1 - committed_));
    err = remove_tree(storage_, work_);
    if (err == StorageError::Ok)
        err = copy_tree(storage_, slot_path(committed_), work_);
    return err;
}

StorageError SaveData::take_over_foreign(const std::vector<std::string>& foreign) {
    const int slot = committed_ < 0 ? 0 : 1 - committed_;
    StorageError err = remove_tree(storage_, slot_path(slot));
    if (err == StorageError::Ok)
        err = committed_ < 0 ? storage_.create_directory(slot_path(slot))
                             : copy_tree(storage_, slot_path(committed_), slot_path(slot));
    for (size_t i = 0; i < foreign.size() && err == StorageError::Ok; ++i) {
        const std::string dst = slot_path(slot) + "/" + foreign[i];
        err = remove_tree(storage_, dst);
        if (err == StorageError::Ok)
            err = copy_tree(storage_, root_ + "/" + foreign[i], dst);
    }
    if (err == StorageError::Ok)
        err = switch_to(slot);
    if (err != StorageError::Ok)
        return err;
    for (const std::string& name : foreign) {
        const StorageError e = remove_tree(storage_, root_ + "/" + name);
        if (e != StorageError::Ok)  // taken over already; a second mount takes it over again
            rt::log(LogLevel::Warn, "fsp-srv: cannot remove %s/%s: %s", root_.c_str(), name.c_str(),
                    platform::storage_error_name(e));
    }
    if (!foreign.empty())
        rt::log(LogLevel::Info, "fsp-srv: save %s: took over %zu entries", root_.c_str(), foreign.size());
    return StorageError::Ok;
}

// Order matters for crash safety: the new slot is complete before the
// temporary journal names it, and that is complete before the old journal
// is removed. Every point in between mounts to the old or the new state.
StorageError SaveData::switch_to(int slot) {
    const int old = committed_;
    StorageError err = storage_.commit();
    if (err == StorageError::Ok)
        err = remove_tree(storage_, root_ + kJournalTmp);
    if (err == StorageError::Ok)
        err = write_file(storage_, root_ + kJournalTmp, std::string(1, static_cast<char>('0' + slot)) + "\n");
    if (err == StorageError::Ok)
        err = storage_.commit();
    if (err == StorageError::Ok)
        err = remove_tree(storage_, root_ + kJournal);
    if (err == StorageError::Ok)
        err = storage_.rename(root_ + kJournalTmp, root_ + kJournal);
    if (err == StorageError::Ok)
        err = storage_.commit();
    if (err != StorageError::Ok)
        return err;
    committed_ = slot;
    if (old >= 0 && old != slot) {
        const StorageError e = remove_tree(storage_, slot_path(old));
        if (e != StorageError::Ok)  // removed on the next mount or commit
            rt::log(LogLevel::Warn, "fsp-srv: cannot remove the old slot of %s: %s", root_.c_str(),
                    platform::storage_error_name(e));
    }
    return StorageError::Ok;
}

StorageError SaveData::commit() {
    if (!journaled_)
        return storage_.commit();
    const int slot = 1 - committed_;
    StorageError err = remove_tree(storage_, slot_path(slot));
    if (err == StorageError::Ok)
        err = copy_tree(storage_, work_, slot_path(slot));
    if (err == StorageError::Ok)
        err = switch_to(slot);
    if (err != StorageError::Ok)
        rt::log(LogLevel::Error, "fsp-srv: commit of %s failed: %s", root_.c_str(), platform::storage_error_name(err));
    else if (on_commit_)
        on_commit_();
    return err;
}

std::shared_ptr<SaveData> SaveRegistry::mount(IStorage& storage, const std::string& root, bool journaled,
                                              StorageError* error) {
    std::lock_guard lock(mutex_);
    if (auto it = mounted_.find(root); it != mounted_.end()) {
        if (std::shared_ptr<SaveData> save = it->second.lock()) {
            *error = StorageError::Ok;
            return save;
        }
    }
    std::shared_ptr<SaveData> save = SaveData::mount(storage, root, journaled, error);
    if (save) {
        if (journaled)
            save->on_commit_ = on_commit_;
        mounted_[root] = save;
    }
    return save;
}

}  // namespace lgp::hle::detail
