// fsp-srv (P3.4): the game's RomFS, system data archives and save data.
// Command ids and layouts from switchbrew "Filesystem services" and libnx
// fs.h (interface documentation only); which commands exist follows the
// Ryujinx reference run (docs/analysis/02-services.md).
//
// Two host storages back it (lgp::platform::IStorage):
//   game (read-only, the user's own dump):
//     /romfs.bin               RomFS image as dumped, or
//     /romfs/                  the extracted tree (built into an image, romfs.hpp)
//     /systemdata/<id>.bin     system data archive (OpenDataStorageByDataId),
//     /systemdata/<id>/        or its extracted tree; <id> as %016llx
//   save (writable): one directory per save data, created on first open:
//     /<program>/account/<uid0><uid1>, /<program>/device, /<program>/bcat,
//     /<program>/temporary, /<program>/cache/<index>, /system/<save id>
//
// Save data is journaled (P5.3, runtime/hle/src/save_data.hpp): the guest
// works on a copy that Commit makes durable; closing every session without
// Commit drops the changes. Temporary storage is not journaled.
// Simplifications (docs/knowledge/horizon-fsp-srv-romfs.md): no quota (the
// space size is a config value), no locks between open files.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "lgp/hle/service.hpp"
#include "lgp/platform/storage.hpp"

namespace lgp::hle {

struct FsConfig {
    platform::IStorage* game = nullptr;  // nullptr: no RomFS, no system data
    platform::IStorage* save = nullptr;  // nullptr: OpenSaveDataFileSystem fails
    // Owner of save data whose attribute names program id 0 (the caller).
    uint64_t program_id = 0;
    // GetTotalSpaceSize of a save file system; GetFreeSpaceSize is this minus
    // the bytes in use. The real value comes from the NACP, which is not parsed.
    uint64_t save_space_size = 64ull << 20;
    // Called after each successful Commit of a journaled save, with that
    // save's lock held (nothing changes it meanwhile); a backup takes its
    // copy here (tvOS cloud saves, lgp/boot/save_backup.hpp). Must not call
    // back into fsp-srv.
    std::function<void()> on_commit;
    // Files of the game's RomFS replaced by data mods (image path -> contents,
    // lgp::mods::romfs_files); if one is not in the RomFS, all are left out
    // and the game reads its own files.
    std::map<std::string, std::vector<uint8_t>> romfs_replace;
    // Asked once per file of romfs_replace when the RomFS is first opened,
    // that is after the mods started: false leaves the file out (a mod that
    // only works with another one that did not start).
    std::function<bool(const std::string& path)> romfs_keep;
    // A mod's changes to files of the game's RomFS (widescreen mod: the
    // layouts). Unset functions change nothing.
    struct RomFsChanges {
        // Asked once when the RomFS is first opened (after the mods started):
        // false leaves the RomFS as it is. *at_mount lists files changed right
        // then (they may change size and go in like data mod files, on top of
        // a data mod's copy of the same file).
        std::function<bool(std::vector<std::string>* at_mount)> begin;
        // Asked on the first read of every other file with its first bytes
        // (up to 16): true hands the whole file to `change`, which must keep
        // its size then (RomFsRewriter).
        std::function<bool(const std::string& path, std::span<const uint8_t> head)> wants;
        // Changes the contents in place; false if nothing changed.
        std::function<bool(const std::string& path, std::vector<uint8_t>& bytes)> change;
        // Optional: the file's generation (RomFsRewriter's Epoch); another
        // number on a later read changes the file anew.
        std::function<uint64_t(const std::string& path)> epoch;
    };
    RomFsChanges romfs_changes;
};

// Registers fsp-srv. The storages must outlive every session. Pass the config
// by move when it carries romfs_replace, so the file contents are not copied.
void register_fs(ServiceManager& sm, FsConfig config);

// fs result codes (module 2, switchbrew "Error codes").
namespace fs_result {
inline constexpr rt::Result kPathNotFound = rt::make_result(2, 1);                          // 0x202
inline constexpr rt::Result kPathAlreadyExists = rt::make_result(2, 2);                     // 0x402
inline constexpr rt::Result kDirectoryNotEmpty = rt::make_result(2, 8);                     // 0x1002
inline constexpr rt::Result kTargetNotFound = rt::make_result(2, 1002);                     // 0x7D402
inline constexpr rt::Result kOutOfRange = rt::make_result(2, 3005);                         // 0x177A02
inline constexpr rt::Result kUnexpected = rt::make_result(2, 5000);                         // 0x271002
inline constexpr rt::Result kInvalidArgument = rt::make_result(2, 6001);                    // 0x2EE202
inline constexpr rt::Result kInvalidPath = rt::make_result(2, 6002);                        // 0x2EE402
inline constexpr rt::Result kInvalidOffset = rt::make_result(2, 6061);                      // 0x2F5A02
inline constexpr rt::Result kInvalidSize = rt::make_result(2, 6062);                        // 0x2F5C02
inline constexpr rt::Result kFileExtensionWithoutOpenModeAllowAppend = rt::make_result(2, 6201);  // 0x307202
inline constexpr rt::Result kReadNotPermitted = rt::make_result(2, 6202);                   // 0x307402
inline constexpr rt::Result kWriteNotPermitted = rt::make_result(2, 6203);                  // 0x307602
inline constexpr rt::Result kUnsupportedOperation = rt::make_result(2, 6300);               // 0x313802
inline constexpr rt::Result kWriteModeFileNotClosed = rt::make_result(2, 6457);             // 0x327202
}  // namespace fs_result

}  // namespace lgp::hle
