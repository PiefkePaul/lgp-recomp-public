// Host memory that backs the guest address space (runtime/core GuestMemory).
//
// Desktop: a 4 GiB virtual reservation (no physical memory). Mapped guest
// memory is committed read-write, everything else stays inaccessible, so a
// stray access to unmapped guest memory faults in hardware. The full 4 GiB is
// reserved even for a smaller layout so that the unchecked 32-bit offset in
// guest_mem.hpp can never leave the reservation.
//
// Guest permissions (read-only, no-access) are deliberately NOT mirrored into
// host protection: HLE services share this one mapping and must write where
// the guest may not (shared memory the guest maps read-only, transfer memory
// lent with no access). Guest permission checks are the job of the checked
// accessors (LGP_GUEST_MEM_CHECKED).
//
// File backing (P7.4, POSIX only; off unless LGP_HOST_BLOCK_FILE is set,
// on tvOS and with -DLGP_HOST_BLOCK_FILE_DEFAULT=ON on unless it is "0"):
// the guest range is a shared mapping of a sparse, already unlinked file in
// the folder LGP_HOST_BLOCK_FILE names ("1": $TMPDIR, else /tmp). Darwin
// counts such pages as "external", not in phys_footprint, which jetsam
// compares with its limit (tvOS: ≈ 2 GiB). If the file cannot be set up,
// the block falls back to anonymous memory and says so on stderr.
// The file takes storage only as the guest first writes a page; when the
// storage is full that write raises SIGBUS. So with less than 256 MiB free
// the block stays anonymous, with less free than the window it warns, and a
// SIGBUS in the window says on stderr that the storage is full.
// The price: whatever the guest writes there, the OS writes back to flash
// (Apple TV: 4.3 GB in 46 min, Analyse 28). LGP_HOST_BLOCK_FILE_RANGE=A:B
// (MiB offsets into the block, either end may be left out) backs only that
// window by the file and leaves the rest anonymous: less write-back, less
// footprint relief. Which window pays off is a measurement
// (LGP_HOST_BLOCK_REGIONS in --time-stats, region_stats()).
// LGP_HOST_BLOCK_PUNCH=0 (diagnosis): released pages in the window are
// zeroed and written back instead of punched out of the file.
// extend_file_window() moves a part of the block into the file while the
// process runs (LGP_HOST_BLOCK_FILE_LATE in the boot program): the Apple TV
// stalled for 9-24 s loading the first scene when the guest first wrote
// these pages in the file, not when it wrote them anonymously
// (docs/analysis/42).
//
// wasm32: a plain allocation of exactly the layout size; Wasm has no page
// protection, so commit/decommit only keep the zero-fill guarantee.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace lgp::platform {

// What svcGetInfo(TotalMemorySize) reports unless the process overrides it
// (ADR-0001). Browser: 3 GiB, the smallest round value LGP starts with (it
// aborts below ≈ 3040 MiB); the guest block is then 3456 MiB of the 4 GiB.
// Desktop: what the console (and Ryujinx with 4 GiB DRAM) reports to LGP,
// ≈ 3.21 GiB.
#if defined(__EMSCRIPTEN__)
inline constexpr uint64_t kDefaultTotalMemorySize = 0xC000'0000;  // 3072 MiB
#else
inline constexpr uint64_t kDefaultTotalMemorySize = 0xCD50'0000;
#endif

class HostBlock {
public:
    // `at` != 0: the block must start at that host address (native execution
    // of guest code, guest address == host address, docs/analysis/56 stage C);
    // throws if the host cannot place it there. Not on wasm32.
    explicit HostBlock(uint64_t size, uint64_t at = 0);
    ~HostBlock();
    HostBlock(const HostBlock&) = delete;
    HostBlock& operator=(const HostBlock&) = delete;

    uint8_t* base() const { return base_; }
    uint64_t size() const { return size_; }

    // Make [offset, offset+len) read-write with zero contents.
    void commit_zeroed(uint64_t offset, uint64_t len);
    // Release [offset, offset+len); a later commit_zeroed reads zero again.
    void decommit(uint64_t offset, uint64_t len);
    // Make committed [offset, offset+len) (whole host pages) read + execute
    // and drop stale instructions from the caches (native execution, stage C).
    // The pages stay readable but no longer writable until decommitted. Throws
    // where the host refuses (wasm32, a hardened runtime without JIT rights).
    void make_executable(uint64_t offset, uint64_t len);
    // Native execution from a precomputed code image (docs/analysis/56 stage
    // F): maps [file_offset, file_offset+len) of the file at `path` read +
    // execute over [offset, offset+len) of the block (whole host pages, the
    // file offset aligned like them), replacing what was there. Unlike
    // make_executable the pages are clean file pages the OS may drop and read
    // again. Throws where the host cannot (wasm32, Windows) or refuses.
    void map_code_file(uint64_t offset, uint64_t len, const std::string& path, uint64_t file_offset);
    // Bytes of the block the OS holds in RAM right now (mincore; --time-stats
    // "host memory", P7.4). Compressed or swapped pages are not counted.
    // false where the OS does not tell (Windows, the browser).
    bool resident(uint64_t* bytes) const;
    // True when the guest range is backed by a file (LGP_HOST_BLOCK_FILE).
    bool file_backed() const { return fd_ >= 0; }
    // The file-backed window [begin, end) of the block (LGP_HOST_BLOCK_FILE_RANGE);
    // empty without file backing.
    uint64_t file_begin() const { return fd_ >= 0 ? file_begin_ : 0; }
    uint64_t file_end() const { return fd_ >= 0 ? file_end_ : 0; }

    // File mode: moves [begin, file_begin()) into the file, so the window
    // becomes [begin, file_end()). Copies the committed pages that are not
    // zero into the file, then maps the file over the range with the same
    // pages committed, so contents and commit state stay. begin is rounded
    // down to a host page. Nothing else may read or write the range
    // meanwhile (guest paused, GPU idle). Works in 16 MiB chunks downwards.
    // False and *error without file backing, with begin not below
    // file_begin(), or when writing the file fails (the chunks moved so far
    // stay in the file, file_begin() says how far it got). POSIX only.
    struct MoveStats {
        uint64_t committed = 0;  // committed bytes in the range
        uint64_t written = 0;    // of those, copied into the file (not zero)
        uint64_t ns = 0;         // time of the whole move
    };
    bool extend_file_window(uint64_t begin, MoveStats* stats, std::string* error);

    // Per `bucket` bytes of the block: how much the OS holds in RAM and, where
    // it tells (Darwin: MINCORE_MODIFIED), how much of the file window is
    // dirty, i.e. not yet written back: what the OS still has to write to the
    // file; ranges that stay dirty are rewritten often (Verdacht: a snapshot,
    // not a write rate). Outside the window dirty stays 0, because anonymous
    // pages count as modified once written. false where mincore is missing
    // (Windows, the browser).
    struct Region {
        uint64_t resident = 0;
        uint64_t dirty = 0;
    };
    bool region_stats(uint64_t bucket, std::vector<Region>* out) const;
    // Whether region_stats() fills `dirty` on this platform.
    static bool dirty_known();

    // What commit_zeroed()/decommit() cost so far, summed over all blocks of
    // the process (--time-stats sources "host block …", docs/analysis/42).
    // Commit: the protection change; Release: decommit() as a whole; Punch
    // and Zero: inside the file window, the hole punching and the zeroing
    // (fallback, or LGP_HOST_BLOCK_PUNCH=0). POSIX only; zero elsewhere.
    enum class Op : uint8_t { Commit, Release, Punch, Zero, Count };
    struct OpTotals {
        uint64_t calls = 0, ns = 0, bytes = 0;
    };
    static OpTotals op_totals(Op op);
    static const char* op_name(Op op);

private:
    uint8_t* base_ = nullptr;
    uint64_t size_ = 0;      // usable guest bytes
    [[maybe_unused]] uint64_t reserved_ = 0;  // bytes reserved from the OS
    int fd_ = -1;                              // backing file (POSIX file mode), else -1
    [[maybe_unused]] uint64_t file_begin_ = 0;  // file-backed window of the block
    [[maybe_unused]] uint64_t file_end_ = 0;
    bool punch_zeroes_ = false;                // file mode: punching a hole zeroes mapped pages
    bool punch_ = true;                        // file mode: punch holes at all (LGP_HOST_BLOCK_PUNCH)
    std::vector<uint64_t> committed_;          // POSIX: one bit per host page, set while committed
};

}  // namespace lgp::platform
