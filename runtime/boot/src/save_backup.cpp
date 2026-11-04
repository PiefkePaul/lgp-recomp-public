// Backup of the app's save data in the cloud, see save_backup.hpp.
#include "lgp/boot/save_backup.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iterator>
#include <system_error>

#include "lgp/boot/app.hpp"
#include "lgp/platform/ui_text.hpp"

namespace lgp::boot {

namespace fs = std::filesystem;

namespace {

constexpr char kMagic[8] = {'L', 'G', 'P', 'S', 'A', 'V', 'E', '1'};
constexpr const char* kSeqFile = ".lgp-cloud";
constexpr const char* kRestoreDir = ".lgp-restore";
constexpr const char* kBeforeRestoreDir = ".lgp-before-restore";
// A cloud state this device replaced with its own (see restore()).
constexpr const char* kReplacedFile = ".lgp-cloud-replaced.bin";
// Its sequence number while the start screen still offers it (cloud_choice()).
constexpr const char* kChoiceFile = ".lgp-cloud-choice";

// Top-level entries of <data> that are backed up. settings-<tag>.ini are the
// settings of other kinds of device sharing the record (see settings_tag).
bool backed_up(const std::string& name) {
    const bool tagged_settings = name.size() > 13 && name.rfind("settings-", 0) == 0 &&
                                 name.compare(name.size() - 4, 4, ".ini") == 0;
    return name == "settings.ini" || tagged_settings || name == "saves" || name.rfind("saves-", 0) == 0;
}

// The record's plain settings.ini belongs to tvOS: it was the only uploader
// before devices shared the record, and its records stay as they were.
constexpr const char* kPlainSettingsOwner = "tvos";

std::string tagged_settings(const std::string& tag) { return "settings-" + tag + ".ini"; }

// The settings of device kind tag in the archive.
std::string archived_settings(const std::string& tag) {
    return tag == kPlainSettingsOwner ? "settings.ini" : tagged_settings(tag);
}

// The archive's top-level name for a top-level entry of the data folder, and
// back: this device's settings.ini is its tag's settings, another kind's
// settings stay inert files in the data folder (settings-<tag>.ini) and go up
// again with the next upload. Without a tag names stay as they are.
std::string archive_top(const std::string& local, const std::string& tag) {
    if (tag.empty())
        return local;
    if (local == "settings.ini")
        return archived_settings(tag);
    if (local == tagged_settings(kPlainSettingsOwner))
        return "settings.ini";
    return local;
}
std::string local_top(const std::string& archived, const std::string& tag) {
    if (tag.empty())
        return archived;
    if (archived == archived_settings(tag))
        return "settings.ini";
    if (archived == "settings.ini")
        return tagged_settings(kPlainSettingsOwner);
    return archived;
}

bool journal_name(const std::string& name) { return name.rfind(".lgp-", 0) == 0; }

bool save_root(const std::string& name) { return name == "saves" || name.rfind("saves-", 0) == 0; }

// Whether a save slot below data holds any file.
bool has_saves(const fs::path& data) {
    std::error_code ec;
    for (fs::directory_iterator top(data, ec), end; !ec && top != end; top.increment(ec)) {
        const std::string name = path_to_utf8(top->path().filename());
        if (!save_root(name))
            continue;
        for (fs::recursive_directory_iterator it(top->path(), ec), e; !ec && it != e; it.increment(ec))
            if (it->is_regular_file(ec))
                return true;
    }
    return false;
}

uint64_t fnv1a(const uint8_t* p, size_t n) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; ++i)
        h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}

void put(std::vector<uint8_t>* out, uint64_t v, int bytes) {
    for (int i = 0; i < bytes; ++i)
        out->push_back(uint8_t(v >> (8 * i)));
}

bool read_file(const fs::path& path, std::vector<uint8_t>* out) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;
    out->assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return !in.bad();
}

// The slot (0/1) a journaled save folder has committed, or -1 (not journaled).
int committed_slot(const fs::path& dir) {
    for (const char* name : {".lgp-journal", ".lgp-journal.tmp"}) {
        std::vector<uint8_t> text;
        if (read_file(dir / name, &text) && !text.empty() && (text[0] == '0' || text[0] == '1'))
            return text[0] - '0';
    }
    return -1;
}

struct Packer {
    std::vector<uint8_t>* out = nullptr;
    uint32_t count = 0;
    std::string error;

    void entry(uint8_t kind, const std::string& rel, const std::vector<uint8_t>& bytes) {
        out->push_back(kind);
        put(out, rel.size(), 4);
        out->insert(out->end(), rel.begin(), rel.end());
        put(out, bytes.size(), 8);
        out->insert(out->end(), bytes.begin(), bytes.end());
        ++count;
    }

    // dir's contents under the archive path rel ("saves/0100…/account/…").
    bool folder(const fs::path& dir, const std::string& rel) {
        entry(0, rel, {});
        const int slot = committed_slot(dir);
        if (slot >= 0)  // journaled: its committed state, flat
            return contents(dir / (".lgp-" + std::to_string(slot)), rel);
        return contents(dir, rel);
    }

    bool contents(const fs::path& dir, const std::string& rel) {
        std::error_code ec;
        std::vector<fs::directory_entry> entries;
        for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
            entries.push_back(*it);
        if (ec) {
            error = "cannot list " + path_to_utf8(dir) + ": " + ec.message();
            return false;
        }
        std::sort(entries.begin(), entries.end());
        for (const fs::directory_entry& e : entries) {
            const std::string name = path_to_utf8(e.path().filename());
            if (journal_name(name))
                continue;
            const std::string sub = rel + "/" + name;
            if (e.is_directory(ec)) {
                if (!folder(e.path(), sub))
                    return false;
            } else if (e.is_regular_file(ec)) {
                std::vector<uint8_t> bytes;
                if (!read_file(e.path(), &bytes)) {
                    error = "cannot read " + path_to_utf8(e.path());
                    return false;
                }
                entry(1, sub, bytes);
            }
        }
        return true;
    }
};

// A path from the archive: relative, '/'-separated, no "..", ".", or empty
// parts, and a backed-up top-level name.
bool safe_path(const std::string& rel) {
    if (rel.empty() || rel.front() == '/' || rel.find('\\') != std::string::npos || rel.find('\0') != std::string::npos)
        return false;
    size_t start = 0;
    bool first = true;
    while (start <= rel.size()) {
        const size_t end = std::min(rel.find('/', start), rel.size());
        const std::string part = rel.substr(start, end - start);
        if (part.empty() || part == "." || part == ".." || (first && !backed_up(part)))
            return false;
        first = false;
        start = end + 1;
    }
    return true;
}

struct Entry {
    uint8_t kind;
    std::string path;
    size_t offset, size;  // of the bytes in the archive
};

bool parse(const std::vector<uint8_t>& a, std::vector<Entry>* entries, std::string* error) {
    if (a.size() < sizeof(kMagic) + 4 + 8 || !std::equal(kMagic, kMagic + sizeof(kMagic), a.begin())) {
        *error = "not a save archive";
        return false;
    }
    uint64_t sum = 0;
    for (int i = 0; i < 8; ++i)
        sum |= uint64_t(a[a.size() - 8 + size_t(i)]) << (8 * i);
    if (sum != fnv1a(a.data(), a.size() - 8)) {
        *error = "save archive damaged (checksum)";
        return false;
    }
    const size_t end = a.size() - 8;
    size_t at = sizeof(kMagic);
    auto get = [&](int bytes, uint64_t* v) {
        if (end - at < size_t(bytes))
            return false;
        *v = 0;
        for (int i = 0; i < bytes; ++i)
            *v |= uint64_t(a[at + size_t(i)]) << (8 * i);
        at += size_t(bytes);
        return true;
    };
    uint64_t count = 0;
    if (!get(4, &count)) {
        *error = "save archive truncated";
        return false;
    }
    for (uint64_t i = 0; i < count; ++i) {
        uint64_t kind = 0, len = 0, size = 0;
        if (!get(1, &kind) || kind > 1 || !get(4, &len) || end - at < len) {
            *error = "save archive damaged (entry)";
            return false;
        }
        Entry e{uint8_t(kind), std::string(a.begin() + long(at), a.begin() + long(at + len)), 0, 0};
        at += len;
        if (!get(8, &size) || end - at < size || !safe_path(e.path)) {
            *error = "save archive damaged (" + e.path + ")";
            return false;
        }
        e.offset = at;
        e.size = size_t(size);
        at += size_t(size);
        entries->push_back(std::move(e));
    }
    if (at != end) {
        *error = "save archive damaged (trailing bytes)";
        return false;
    }
    return true;
}

std::string clock_text(const char* format = "%H:%M") {
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
#if defined(_MSC_VER)
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    char buf[32] = {};
    std::strftime(buf, sizeof(buf), format, &tm);
    return buf;
}

// Writes the settings of other kinds of device from archive into data
// (settings-<tag>.ini), so that the next upload carries them along.
void keep_foreign_settings(const std::vector<uint8_t>& archive, const fs::path& data, const std::string& tag) {
    std::vector<Entry> entries;
    std::string error;
    if (tag.empty() || !parse(archive, &entries, &error))
        return;
    for (const Entry& e : entries) {
        if (e.kind != 1 || e.path.find('/') != std::string::npos)
            continue;
        const std::string local = local_top(e.path, tag);
        if (local == "settings.ini" || local.rfind("settings-", 0) != 0)
            continue;  // this device's settings, or not settings
        std::ofstream out(data / fs::path(std::u8string(local.begin(), local.end())), std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(archive.data() + e.offset), std::streamsize(e.size));
    }
}

}  // namespace

bool pack_saves(const fs::path& data, std::vector<uint8_t>* out, std::string* error, const std::string& settings_tag) {
    out->assign(kMagic, kMagic + sizeof(kMagic));
    put(out, 0, 4);  // count, filled in below
    Packer p;
    p.out = out;
    std::error_code ec;
    std::vector<fs::directory_entry> top;
    for (fs::directory_iterator it(data, ec), end; !ec && it != end; it.increment(ec))
        top.push_back(*it);
    if (ec) {
        *error = "cannot list " + path_to_utf8(data) + ": " + ec.message();
        return false;
    }
    std::sort(top.begin(), top.end());
    for (const fs::directory_entry& e : top) {
        const std::string name = path_to_utf8(e.path().filename());
        if (!backed_up(name))
            continue;
        // Tagged: settings.ini goes up as this device's, a stray file under
        // its tag's name would only shadow it.
        if (!settings_tag.empty() && name == tagged_settings(settings_tag))
            continue;
        const std::string archived = archive_top(name, settings_tag);
        bool ok = true;
        if (e.is_directory(ec)) {
            ok = p.folder(e.path(), archived);
        } else if (e.is_regular_file(ec)) {
            std::vector<uint8_t> bytes;
            ok = read_file(e.path(), &bytes);
            if (ok)
                p.entry(1, archived, bytes);
            else
                p.error = "cannot read " + path_to_utf8(e.path());
        }
        if (!ok) {
            *error = p.error;
            return false;
        }
    }
    for (int i = 0; i < 4; ++i)
        (*out)[sizeof(kMagic) + size_t(i)] = uint8_t(p.count >> (8 * i));
    put(out, fnv1a(out->data(), out->size()), 8);
    return true;
}

bool unpack_saves(const std::vector<uint8_t>& archive, const fs::path& data, std::string* error,
                  const std::string& settings_tag) {
    std::vector<Entry> entries;
    if (!parse(archive, &entries, error))
        return false;
    std::error_code ec;
    const fs::path stage = data / kRestoreDir;
    fs::remove_all(stage, ec);
    std::vector<std::string> tops;
    // Settings belong to the device (args, controller, graphics): with a tag
    // the archive's only replace a settings.ini that is gone (tvOS emptied
    // its cache). iPhone and iPad share the tag "ios" and must not take each
    // other's.
    std::error_code exists_ec;
    const bool keep_settings = !settings_tag.empty() && fs::exists(data / "settings.ini", exists_ec);
    for (const Entry& e : entries) {
        const size_t slash = e.path.find('/');
        const std::string top = local_top(e.path.substr(0, slash), settings_tag);
        if (keep_settings && top == "settings.ini")
            continue;
        const std::string rel = slash == std::string::npos ? top : top + e.path.substr(slash);
        const fs::path to = stage / fs::path(std::u8string(rel.begin(), rel.end()));
        if (std::find(tops.begin(), tops.end(), top) == tops.end())
            tops.push_back(top);
        if (e.kind == 0) {
            fs::create_directories(to, ec);
        } else {
            fs::create_directories(to.parent_path(), ec);
            std::ofstream out(to, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(archive.data() + e.offset), std::streamsize(e.size));
            if (!out)
                ec = std::make_error_code(std::errc::io_error);
        }
        if (ec) {
            *error = "cannot write " + path_to_utf8(to) + ": " + ec.message();
            fs::remove_all(stage, ec);
            return false;
        }
    }
    // Swap in, keeping what was there; on a failure the entries already
    // swapped go back, so data is as before.
    const fs::path before = data / kBeforeRestoreDir;
    fs::remove_all(before, ec);
    fs::create_directories(before, ec);
    std::vector<fs::path> swapped;
    for (const std::string& top : tops) {
        const fs::path name(std::u8string(top.begin(), top.end()));
        if (fs::exists(data / name, ec))
            fs::rename(data / name, before / name, ec);
        if (!ec)
            fs::rename(stage / name, data / name, ec);
        if (!ec) {
            swapped.push_back(name);
            continue;
        }
        *error = "cannot move " + top + " into place: " + ec.message();
        if (!fs::exists(data / name) && fs::exists(before / name))
            fs::rename(before / name, data / name, ec);  // this one's original
        for (const fs::path& done : swapped) {
            fs::remove_all(data / done, ec);
            if (fs::exists(before / done))
                fs::rename(before / done, data / done, ec);
        }
        fs::remove_all(stage, ec);
        return false;
    }
    fs::remove_all(stage, ec);
    return true;
}

SaveBackup::SaveBackup(std::unique_ptr<platform::ICloudSave> cloud, fs::path data, std::string settings_tag)
    : cloud_(std::move(cloud)), data_(std::move(data)), settings_tag_(std::move(settings_tag)) {
    // "<seq>" or "<seq> dirty" (saves changed since the cloud last took them).
    std::vector<uint8_t> text;
    if (read_file(data_ / kSeqFile, &text)) {
        known_ = true;
        const std::string t(text.begin(), text.end());
        seq_ = std::strtoull(t.c_str(), nullptr, 10);
        dirty_ = t.find("dirty") != std::string::npos;
    }
    // A choice not taken yet stays offered after a restart.
    std::vector<uint8_t> choice;
    std::error_code ec;
    if (read_file(data_ / kChoiceFile, &choice) && fs::exists(data_ / kReplacedFile, ec))
        choice_ = std::strtoull(std::string(choice.begin(), choice.end()).c_str(), nullptr, 10);
    thread_ = std::thread([this] { worker(); });
}

SaveBackup::~SaveBackup() {
    wait_idle(stop_wait);
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
    }
    wake_.notify_all();
    thread_.join();
}

void SaveBackup::restore() {
    std::string detail;
    const platform::CloudStatus status = cloud_->status(status_timeout, &detail);
    std::printf("cloud saves: %s%s%s\n", platform::cloud_status_name(status), detail.empty() ? "" : ": ",
                detail.c_str());
    if (status != platform::CloudStatus::Available) {
        // Only tvOS may delete the saves (Library/Caches); on iOS they stay in Documents.
        const std::string loss = settings_tag_ == "tvos" ? std::string(" ") + platform::tr("tvOS may delete them.") : "";
        set_note((status == platform::CloudStatus::NoAccount
                      ? std::string(platform::tr("Warning: no iCloud account. Saves are not backed up."))
                      : platform::trf("Warning: the iCloud backup does not work right now ({}). Saves are not backed "
                                      "up.",
                                      {detail})) +
                 loss);
        return;
    }
    bool found = false;
    platform::CloudRecord record;
    std::string error;
    if (!cloud_->fetch(kRecordKey, fetch_timeout, &found, &record, &error)) {
        std::printf("cloud saves: fetch failed: %s\n", error.c_str());
        set_note(platform::trf("Warning: saves in iCloud cannot be fetched ({}).", {error}));
        return;
    }
    std::unique_lock lock(mutex_);
    const uint64_t local = seq_;
    // Saves already here at the first start with the cloud were never
    // uploaded: not restored over (a conflict below keeps both).
    const bool dirty = dirty_ || (!known_ && has_saves(data_));
    lock.unlock();
    if (found && record.seq > local && !dirty) {
        // Newer in the cloud and nothing unsaved here (another device, or
        // the folder was emptied: then local is 0).
        if (!unpack_saves(record.data, data_, &error, settings_tag_)) {
            std::printf("cloud saves: restore failed: %s\n", error.c_str());
            set_note(platform::trf("Warning: restoring from iCloud failed ({}).", {error}));
            return;  // not usable: no upload may replace the cloud state
        }
        set_state(record.seq, false);
        std::error_code ec;  // an older choice is moot now
        fs::remove(data_ / kReplacedFile, ec);
        fs::remove(data_ / kChoiceFile, ec);
        std::printf("cloud saves: restored state %llu (%zu bytes)\n", static_cast<unsigned long long>(record.seq),
                    record.data.size());
        lock.lock();
        usable_ = true;
        choice_ = 0;
        lock.unlock();
        set_note(platform::tr("Saves restored from iCloud."));
        return;
    }
    if (found && record.seq > local) {
        // Both sides changed: saves here that never reached the cloud, and
        // a newer cloud state from elsewhere. The progress on this device
        // wins; the cloud state is kept beside it, not lost.
        std::ofstream out(data_ / kReplacedFile, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(record.data.data()), std::streamsize(record.data.size()));
        if (!out.flush()) {
            std::printf("cloud saves: conflict, cannot keep the cloud state: %s\n", path_to_utf8(data_ / kReplacedFile).c_str());
            set_note(platform::tr("Warning: iCloud has another save; not backed up."));
            return;
        }
        std::printf("cloud saves: conflict, cloud state %llu kept in %s, this device's saves replace it\n",
                    static_cast<unsigned long long>(record.seq), kReplacedFile);
        {
            std::ofstream seq(data_ / kChoiceFile, std::ios::trunc);
            seq << record.seq << "\n";
        }
        // The upload below replaces the record: other devices' settings go along.
        keep_foreign_settings(record.data, data_, settings_tag_);
        set_state(record.seq, true);
        lock.lock();
        choice_ = record.seq;
        usable_ = true;
        lock.unlock();
        committed();  // the start screen offers the other state (cloud_choice())
        return;
    }
    lock.lock();
    usable_ = true;
    lock.unlock();
    set_note(platform::tr("Saves are backed up in iCloud."));
    // The cloud lacks this device's state: first run, saves since the last
    // upload (one that failed or did not finish before the app ended), or
    // the conflict above.
    if (!found || record.seq < local || dirty)
        committed();
}

void SaveBackup::committed() {
    std::vector<uint8_t> archive;
    std::string error;
    if (!pack_saves(data_, &archive, &error, settings_tag_)) {
        std::printf("cloud saves: cannot pack: %s\n", error.c_str());
        set_note(platform::trf("Warning: save not backed up ({}).", {error}));
        return;
    }
    {
        std::lock_guard lock(mutex_);
        dirty_ = true;
        if (usable_)
            pending_ = std::move(archive);
    }
    // Remembered across starts: an upload that does not happen now is
    // made up for by the next restore().
    save_state();
    wake_.notify_all();
}

void SaveBackup::worker() {
    std::unique_lock lock(mutex_);
    int failures = 0;  // uploads failed in a row
    for (;;) {
        wake_.wait(lock, [&] { return stop_ || pending_.has_value(); });
        if (stop_)
            return;
        platform::CloudRecord record;
        record.data = std::move(*pending_);
        pending_.reset();
        record.seq = seq_ + 1;
        uploading_ = true;
        lock.unlock();
        std::string error;
        bool newer = false;
        const bool ok = cloud_->store(kRecordKey, record, store_timeout, &newer, &error);
        if (ok) {
            {
                std::lock_guard l(mutex_);
                seq_ = record.seq;
                dirty_ = pending_.has_value();  // a newer save is waiting
            }
            save_state();
            std::printf("cloud saves: uploaded state %llu (%zu bytes)\n", static_cast<unsigned long long>(record.seq),
                        record.data.size());
            set_note(platform::trf("Saves backed up in iCloud ({}).", {clock_text()}));
        } else {
            std::printf("cloud saves: upload failed: %s\n", error.c_str());
            set_note(platform::trf("Warning: last save not in iCloud ({}).", {error}));
        }
        lock.lock();
        uploading_ = false;
        if (ok)
            failures = 0;
        if (!ok && !newer && !pending_ && !stop_) {
            // Try again later unless a newer save replaced it meanwhile. A
            // newer record in the cloud is not overwritten (store refuses),
            // so retrying only helps when the network was the problem. The
            // first attempts follow soon (the first upload after a start can
            // run into the timeout, measured on the Apple TV), later ones
            // back off to retry_delay.
            const int delay = std::min(retry_delay, retry_first << std::min(failures, 10));
            ++failures;
            std::printf("cloud saves: retrying in %d ms\n", delay);
            pending_ = std::move(record.data);
            wake_.notify_all();
            wake_.wait_for(lock, std::chrono::milliseconds(delay), [&] { return stop_; });
        }
        wake_.notify_all();
    }
}

bool SaveBackup::wait_idle(int timeout_ms) {
    std::unique_lock lock(mutex_);
    return wake_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] { return !pending_ && !uploading_; });
}

uint64_t SaveBackup::local_seq() const {
    std::lock_guard lock(mutex_);
    return seq_;
}

uint64_t SaveBackup::cloud_choice() const {
    std::lock_guard lock(mutex_);
    return choice_;
}

bool SaveBackup::use_cloud_state(std::string* error) {
    std::vector<uint8_t> archive;
    if (cloud_choice() == 0 || !read_file(data_ / kReplacedFile, &archive)) {
        *error = "kein iCloud-Stand zur Wahl";
        return false;
    }
    // Every save slot of this device moves aside first, also those the cloud
    // state lacks: the result is that state alone, not a mixture.
    const fs::path kept = data_ / (".lgp-device-saves-" + clock_text("%Y%m%d-%H%M%S"));
    std::error_code ec;
    fs::create_directories(kept, ec);
    std::vector<fs::path> moved;
    const auto put_back = [&] {
        std::error_code e;
        for (const fs::path& name : moved)
            fs::rename(kept / name, data_ / name, e);
        fs::remove(kept, e);  // only if empty
    };
    for (fs::directory_iterator it(data_, ec), end; !ec && it != end; it.increment(ec)) {
        if (!save_root(path_to_utf8(it->path().filename())))
            continue;
        moved.push_back(it->path().filename());
    }
    for (const fs::path& name : moved) {
        std::error_code e;
        fs::rename(data_ / name, kept / name, e);
        if (e) {
            *error = "cannot move " + path_to_utf8(name) + " aside: " + e.message();
            moved.erase(std::find(moved.begin(), moved.end(), name), moved.end());
            put_back();
            return false;
        }
    }
    if (ec || !unpack_saves(archive, data_, error, settings_tag_)) {
        if (ec)
            *error = "cannot list " + path_to_utf8(data_) + ": " + ec.message();
        put_back();
        return false;
    }
    // What unpack_saves replaced besides (this device's settings) joins them.
    bool complete = true;
    for (fs::directory_iterator it(data_ / kBeforeRestoreDir, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e;
        fs::rename(it->path(), kept / it->path().filename(), e);
        complete &= !e;
    }
    fs::remove(data_ / kBeforeRestoreDir, ec);  // only if empty
    fs::remove(data_ / kReplacedFile, ec);
    fs::remove(data_ / kChoiceFile, ec);
    if (!complete)
        std::printf("cloud saves: part of this device's files stay in %s\n", kBeforeRestoreDir);
    std::printf("cloud saves: took cloud state %llu, this device's saves kept in %s\n",
                static_cast<unsigned long long>(cloud_choice()), path_to_utf8(kept.filename()).c_str());
    {
        std::lock_guard lock(mutex_);
        choice_ = 0;
    }
    committed();  // the cloud holds this device's saves since restore(): up again
    set_note(platform::trf("iCloud state loaded. This device's previous state is in {}.", {path_to_utf8(kept.filename())}));
    return true;
}

std::string SaveBackup::note() const {
    std::lock_guard lock(mutex_);
    return note_;
}

void SaveBackup::set_note(std::string text) {
    std::lock_guard lock(mutex_);
    note_ = std::move(text);
}

void SaveBackup::set_state(uint64_t seq, bool dirty) {
    {
        std::lock_guard l(mutex_);
        seq_ = seq;
        dirty_ = dirty;
    }
    save_state();
}

void SaveBackup::save_state() {
    // The file lock orders the writes; the values are read under it, so the
    // last write carries the latest state.
    std::lock_guard lock(file_mutex_);
    uint64_t seq = 0;
    bool dirty = false;
    {
        std::lock_guard l(mutex_);
        seq = seq_;
        dirty = dirty_;
    }
    // A cut-off file would read as 0 and let the cloud win: write aside, then
    // rename over.
    const fs::path tmp = data_ / (std::string(kSeqFile) + ".tmp");
    {
        std::ofstream out(tmp, std::ios::trunc);
        out << seq << (dirty ? " dirty" : "") << "\n";
        if (!out.flush()) {
            std::printf("cloud saves: cannot write %s\n", path_to_utf8(tmp).c_str());
            return;
        }
    }
    std::error_code ec;
    fs::rename(tmp, data_ / kSeqFile, ec);
    if (ec)
        std::printf("cloud saves: cannot write %s: %s\n", kSeqFile, ec.message().c_str());
}

}  // namespace lgp::boot
