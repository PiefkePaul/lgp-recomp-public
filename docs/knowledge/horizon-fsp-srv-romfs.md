# `fsp-srv` and RomFS in a custom runtime

Game-independent knowledge from P3.4 (`runtime/hle/src/fs.cpp`, `runtime/hle/src/romfs.cpp`). Command numbers and layouts come from switchbrew ("Filesystem services", "Error codes"), 3dbrew ("RomFS", same level 3 format) and libnx (`fs.h`, `romfs_dev.h`/`romfs_dev.c`). No code was taken. Which commands exist is based on LGP's Ryujinx reference run; all others halt the guest.

## Why the runtime serves a RomFS *image*

Games built with the Nintendo SDK parse their RomFS themselves: `nn::fs::MountRom` opens a raw `IStorage` with `OpenDataStorageByCurrentProcess` and reads the header, hash tables and file data from it (8,560 `IStorage.Read` in the LGP reference run). fsp-srv knows no file names there. So an extracted directory tree is not enough; the runtime must deliver the bytes of a RomFS image. Two sources:

- **`/romfs.bin`** in the game storage: the RomFS image at level 3, without the IVFC wrapper.
- **`/romfs/`**: the extracted tree. `RomFsImage::build` builds a virtual image from it. Header and tables live in host memory, file data is only fetched from the tree when read (up to 64 files stay open). Advantage: files can be replaced (mods) without repacking an image.

Same scheme for system archives (`OpenDataStorageByDataId`): `/systemdata/<id>.bin` or `/systemdata/<id>/`, `<id>` as `%016x`. If both are missing, the runtime replies with 0x7D402 (TargetNotFound) and writes the path to the log.

## Compressed image with block index

A RomFS image is often several GiB in size and is read randomly by the game: many small reads across the file, plus videos in one go. Compression as a single stream (zip, `.zst` of the whole image) is unsuitable, because reading in the middle would require unpacking everything before it. That is why `lgp_romfs_pack --compress` cuts the image into fixed-size blocks and compresses each block on its own with zstd. An index at the end says where each block lies in the file. When reading, fsp-srv only unpacks the blocks the access touches. The game sees the same image as before.

| Part | Layout (little endian) |
|---|---|
| Header, 32 bytes | `"LGPROMZ\0"`, u32 version 1, u32 codec 1 (zstd), u32 block size, u32 block count, u64 size of the image |
| Blocks | from byte 32 contiguously one after another |
| Index | block count + 1 u64 offsets; block i lies in `[index[i], index[i+1])`. If a block is as long as uncompressed, it is stored raw, because zstd could not make it smaller (e.g. already compressed videos). |
| Footer, 16 bytes | u64 offset of the index, `"LGPZEND\0"` |

- The file name stays `romfs.bin`. The runtime recognizes the format by the first eight bytes (`open_image_source` in `runtime/hle/src/romfs_compressed.cpp`). So everything that expects `romfs.bin` keeps working: browser folder selection, app mode, videos (`movie::`).
- On opening, the whole index is checked: end marker, version, codec, block size 4 KiB–16 MiB, block count matching the size, offsets ascending and no block longer than uncompressed. A damaged block is only noticed on reading. Then only that read fails.
- Reading: the last 32 unpacked blocks stay in memory, LRU per source. Whole blocks of a large access go directly into the target buffer, bypassing the cache, otherwise a large access would evict all other blocks. A mutex per source protects this. fsp-srv and `movie::` each have their own source.
- Packing: `lgp_romfs_pack --compress[=LEVEL] [--block-size KIB] [--threads N] <romfs-Ordner|romfs.bin> <aus>`. Defaults: level 9, 128 KiB per block, all cores. Without `--compress` the tool unpacks a compressed image back to a regular one. The test `runtime.romfs_pack_roundtrip` checks that this yields the original byte for byte.
- Trade-off for the block size: larger blocks compress better. In exchange a small access has to unpack more (read amplification). 128 KiB is estimated and not yet measured on the game.
- The runtime builds zstd in (`LGP_ENABLE_ZSTD`, on by default, downloads zstd 1.5.7, offline `LGP_ZSTD_DIR`). Without zstd the runtime rejects a compressed image with a message; regular images keep working.

## RomFS format (level 3)

| Part | Layout |
|---|---|
| Header, 0x50 bytes | ten u64: header size (0x50), dir hash offset/size, dir table offset/size, file hash offset/size, file table offset/size, data offset. All offsets from the start of the image. On the Switch they are u64, on the 3DS u32 (header 0x28). |
| Directory entry | u32 parent, u32 next sibling, u32 first child directory, u32 first file, u32 next in hash bucket, u32 name length, name (UTF-8) padded to 4 bytes |
| File entry | u32 parent, u32 next sibling, u64 data offset (relative to the header's data offset), u64 size, u32 next in bucket, u32 name length, name |
| References | byte offsets into the respective table, `0xFFFFFFFF` = none. The root is at 0, is its own parent and has the name "". |
| Hash | `h = eltern_offset ^ 123456789`, per name byte `h = rotr(h, 5) ^ byte`; bucket = `h % Anzahl`. The 3DS computes with UTF-16 units, the Switch with UTF-8 bytes. |
| Bucket count | < 3 → 3; < 19 → n \| 1; otherwise the next number from n that is not divisible by any prime up to 17 |
| Collisions | singly linked list; an entry inserted later becomes the head of the bucket |

The builder places the data from 0x200 (like Nintendo's tools), each file aligned to 0x10, the tables after the data. The order is breadth-first and sorted by name per directory. The test (`tests/runtime/test_hle_fs.cpp`) checks the image with its own parser: walk the tree via the references and look up every path via the hash tables.

## Commands

| Object | Commands here |
|---|---|
| `fsp-srv` (IFileSystemProxy) | 1 `SetCurrentProcess` (u64, PID), 51 `OpenSaveDataFileSystem` (u8 space ID, 0x40 `SaveDataAttribute`), 200 `OpenDataStorageByCurrentProcess`, 202 `OpenDataStorageByDataId` (u8 storage ID, u64 data ID), 1005 `GetGlobalAccessLogMode` (→ u32 0) |
| `IStorage` | 0 `Read` (s64 offset, s64 size, buffer 0x46), 1 `Write` and 3 `SetSize` (→ UnsupportedOperation), 2 `Flush`, 4 `GetSize` |
| `IFileSystem` | 0 `CreateFile` (u32 option, s64 size, path), 1 `DeleteFile`, 2 `CreateDirectory`, 3 `DeleteDirectory`, 4 `DeleteDirectoryRecursively`, 5 `RenameFile`, 6 `RenameDirectory` (two paths), 7 `GetEntryType` (→ u32 0 directory, 1 file), 8 `OpenFile` (u32 mode), 9 `OpenDirectory` (u32 filter), 10 `Commit`, 11 `GetFreeSpaceSize`, 12 `GetTotalSpaceSize`, 13 `CleanDirectoryRecursively` |
| `IFile` | 0 `Read` (u32 option, s64 offset, s64 size, buffer 0x46 → s64 read), 1 `Write` (same, buffer 0x45; option bit 0 = flush), 2 `Flush`, 3 `SetSize`, 4 `GetSize` |
| `IDirectory` | 0 `Read` (buffer 0x06 → s64 count), 1 `GetEntryCount` |

- **Paths** come as X buffers (type 0x19) of 0x301 bytes, NUL-terminated, without mount name. The runtime normalizes them with `platform::normalize_path`. Relative paths, `.`/`..`, `\`, `:` and paths without NUL yield 0x2EE402 (InvalidPath). That way no guest path gets out of its save.
- **Open mode:** 1 read, 2 write, 4 append. Reading without read: 0x307402, writing without write: 0x307602, writing past the end of the file without append: 0x307202. Reading at the end of the file yields 0 bytes, beyond it 0x177A02 (OutOfRange). A read past the end only yields the existing bytes.
- **`DirectoryEntry`** (0x310 bytes): name[0x301], u8 attributes at 0x301, u8 type at 0x304 (0 directory, 1 file), s64 size at 0x308. Filter bits: 1 directories, 2 files. The runtime reads the contents once when opening.
- **`SaveDataAttribute`** (0x40 bytes): u64 program ID (0 = the caller), 16 bytes user ID at 0x08, u64 system save ID at 0x18, u8 type at 0x20 (0 System, 1 Account, 2 Bcat, 3 Device, 4 Temporary, 5 Cache), u8 rank at 0x21, u16 index at 0x22.

## Saves on the host

- Location in the save storage: `/<program>/account/<uid0><uid1>`, `/<program>/device`, `/<program>/bcat`, `/<program>/temporary`, `/<program>/cache/<index>`, `/system/<save-id>`; all IDs as `%016x`. The user ID comes from `SystemConfig` (see [`horizon-basisdienste-am-set-time-acc.md`](horizon-basisdienste-am-set-time-acc.md)) and must stay stable.
- **Created on first open:** on real hardware the system creates the saves according to the NACP at application start (`EnsureSaveData` in the application or by the menu). LGP itself calls nothing of the kind, so `OpenSaveDataFileSystem` creates the directory, as Ryujinx does too.
- **Journaling (P5.3, `runtime/hle/src/save_data.cpp`):** on the console the game sees its writes immediately; they only become permanent with `Commit`. A crash or closing all sessions without `Commit` leaves the last committed state in place. The runtime reproduces this as follows:
  - Per save there are three folders: `.lgp-work` (working copy, the guest's "/"), `.lgp-0` and `.lgp-1` (committed state and spare). In addition there is the file `.lgp-journal` with `0` or `1`; it says which slot is valid. The guest cannot see these names; its root folder is the working copy.
  - On the first open (mount) the runtime copies the committed slot to `.lgp-work`. Further sessions on the same save share this copy (registry via `weak_ptr`). Once the last session and the last open file are closed, the uncommitted changes are gone.
  - `Commit` copies the working copy into the spare slot and then switches: write `.lgp-journal.tmp`, delete the old journal, rename `.tmp`. If the journal is missing at mount but `.tmp` is there, `.tmp` applies. So every crash point lands on the old or the new state. `runtime.hle_fs` checks exactly that: a storage wrapper "crashes" after n changes (the last write half done), for every n.
  - The procedure needs no folder rename; a file rename is enough. That is why it also runs on OPFS. The model is LibHac's `DirectorySaveDataFileSystem` (working and commit folders), only read there, no code taken. LibHac switches via folder rename.
  - As on the console, `Commit` refuses with 0x327202 (`WriteModeFileNotClosed`, 6457, from Atmosphère's `fs_results.hpp`) while a file is open for writing. Files open for reading do not interfere.
  - The Temporary type is not journaled: there everything goes directly into the folder and `Commit` only calls `IStorage::commit()`. Whether the console journals Bcat and Cache saves has not been checked; here they are journaled.
  - Cost: every mount and every `Commit` copies the whole save, and the storage needs up to three times its size. For saves of a few MiB that does not matter; for large saves copy-on-first-write would be better.
  - **Down to the disk:** `HostDirectoryStorage::commit()` syncs all files written since the last call and all folders with changed entries, files first (`platform::sync_host_path`). On Linux that is `fsync`, also on folders, so that creation and renaming are durable. On macOS it is `fcntl(F_FULLFSYNC)`, because `fsync` there does not flush the drive's cache. On Windows it is `FlushFileBuffers`, on folders via a handle with `GENERIC_WRITE` and `FILE_FLAG_BACKUP_SEMANTICS` (without the flag no folder can be opened; without write access `FlushFileBuffers` refuses). NTFS supports this; file systems that do not (FAT, some network drives) report `ERROR_INVALID_FUNCTION` or similar, which, as under POSIX, does not count as an error. The journal calls `commit()` before writing the new journal file and after each switching step. This way a committed save also survives a power failure, as far as the drive reports honestly. The Windows CI (MSVC and clang) checks the path in `platform.backends`, including that flushing a folder on NTFS succeeds. `strace` shows the `fsync` calls under Linux (`platform.backends`).
  - In the browser, `fsync` under WasmFS brings the OPFS file to storage via `flush()`. Folders are skipped there.
- **Adopting foreign files:** if, at mount, something other than `.lgp-*` lies directly in the save folder, the runtime adopts it: copy over the committed state, commit, then delete the originals. This applies to saves from before P5.3 (flat in the folder), to files someone puts in by hand (such as a `savedata.bin` edited with a tool), and to the save import in the browser. A file with the same name replaces the old one, the rest stays.
- **`RenameDirectory` without folder rename in the storage:** WasmFS on OPFS answers renaming a folder with `EBUSY` (`opfs_backend.cpp`, "TODO: Support moving directories"). In the browser test that results in `Io`. fsp-srv then copies the folder and deletes the source (`detail::rename_directory`). That is not atomic, but only affects the working copy: a crash in the middle discards the `Commit` anyway. `RenameDirectory` does not occur in the LGP reference run; this is for other games.
- **No quota, no locks:** the total size is a config value (`FsConfig::save_space_size`, default 64 MiB, because the NACP is not read yet). `GetFreeSpaceSize` subtracts the occupied bytes; writing beyond the quota is not prevented. Opening the same save and the same file multiple times is allowed. Whether the console returns `TargetLocked` here has not been checked; Ryujinx allows it too.

## Result codes (module 2)

| Code | Description |
|---|---|
| 0x202 | 1 PathNotFound (also: wrong type, e.g. file instead of directory) |
| 0x402 | 2 PathAlreadyExists |
| 0x1002 | 8 DirectoryNotEmpty (known from Atmosphère sources, not confirmed on switchbrew) |
| 0x7D402 | 1002 TargetNotFound |
| 0x177A02 | 3005 OutOfRange |
| 0x271002 | 5000 Unexpected (host error) |
| 0x2EE202 | 6001 InvalidArgument (unknown open mode or save type) |
| 0x2EE402 | 6002 InvalidPath |
| 0x2F5A02 / 0x2F5C02 | 6061 InvalidOffset / 6062 InvalidSize (also: size > buffer) |
| 0x307202 | 6201 FileExtensionWithoutOpenModeAllowAppend |
| 0x307402 / 0x307602 | 6202 ReadNotPermitted / 6203 WriteNotPermitted (from Atmosphère sources, not confirmed on switchbrew) |
| 0x313802 | 6300 UnsupportedOperation (writing to RomFS) |
| 0x327202 | 6457 WriteModeFileNotClosed (`Commit` with a file open for writing; Atmosphère `fs_results.hpp`) |
