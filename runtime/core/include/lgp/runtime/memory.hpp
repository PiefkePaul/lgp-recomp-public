// Guest address space: compact layout below 4 GiB, block map, host backing.
//
// ADR-0001 option A: the runtime (our "kernel") decides the layout, and all
// guest regions live in one compact range so that wasm32 can back it with a
// single linear-memory block. Desktop and web use the same region bases; only
// the heap region (sized from TotalMemorySize) and therefore the end of the
// address space differ.
//
// Default layout (guest VAs):
//   0x0000'0000 .. 0x0800'0000  not part of the address space (null guard)
//   0x0800'0000 .. 0x0E00'0000  code region   96 MiB  module images (LGP ≈ 72 MiB)
//   0x0E00'0000 .. 0x1200'0000  stack region  64 MiB  svcMapMemory targets (thread stacks)
//   0x1200'0000 .. 0x1300'0000  alias region  16 MiB  IPC buffers (unused by an HLE runtime)
//   0x1300'0000 .. 0x2000'0000  misc region  208 MiB  shared/transfer mappings, TLS pages
//   0x2000'0000 .. +heap        heap region         = TotalMemorySize rounded up to 2 MiB
//
// GuestMemory is not thread-safe; the kernel serialises memory svcs.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>

#include "lgp/platform/host_block.hpp"
#include "lgp/runtime/guest_mem.hpp"
#include "lgp/runtime/result.hpp"
#include "lgp/runtime/svc_types.hpp"

namespace lgp::rt {

inline constexpr uint64_t kHeapAlignment = 0x20'0000;  // svcSetHeapSize granularity

// ADR-0001: 3072 MiB in the browser, ≈ 3.21 GiB (what the console reports
// to LGP) on desktop; chosen by the platform layer.
inline constexpr uint64_t kDefaultTotalMemorySize = platform::kDefaultTotalMemorySize;

struct Region {
    uint64_t base = 0;
    uint64_t size = 0;

    constexpr uint64_t end() const { return base + size; }
    constexpr bool contains(uint64_t addr, uint64_t len) const {
        return addr >= base && len <= size && addr - base <= size - len;
    }
    constexpr bool overlaps(uint64_t addr, uint64_t len) const { return addr < end() && base < addr + len; }
};

struct Layout {
    Region code;
    Region stack;
    Region alias;
    Region misc;
    Region heap;

    uint64_t start() const { return code.base; }
    uint64_t end() const { return heap.end(); }
    uint64_t size() const { return end() - start(); }
    Region aslr() const { return {start(), size()}; }

    // The fixed production layout above.
    static Layout standard(uint64_t total_memory_size);
    // Same structure with caller-chosen sizes, regions packed back to back
    // from `start` (tests use small sizes to keep wasm allocations small).
    static Layout packed(uint64_t start, uint64_t code, uint64_t stack, uint64_t alias, uint64_t misc,
                         uint64_t heap);
};

struct BlockInfo {
    uint64_t addr = 0;
    uint64_t size = 0;
    MemState state = MemState::Free;
    uint8_t perm = kPermNone;
    uint8_t attr = kAttrNone;
};

using platform::HostBlock;

class GuestMemory {
public:
    struct Options {
        // Publish this address space as lgp::rt::g_view (the fast path).
        bool publish = true;
        // Guest address == host address (native execution of the original
        // code, docs/analysis/56 stage C): the host block is placed at
        // layout.start(), which may then lie above 4 GiB (the layout itself
        // stays smaller than 4 GiB). Throws if the host cannot place it there.
        bool at_guest_address = false;
    };

    GuestMemory(const Layout& layout, const Options& options);
    explicit GuestMemory(const Layout& layout);
    ~GuestMemory();
    GuestMemory(const GuestMemory&) = delete;
    GuestMemory& operator=(const GuestMemory&) = delete;

    const Layout& layout() const { return layout_; }
    bool contains(uint64_t addr, uint64_t size) const { return Region{layout_.start(), layout_.size()}.contains(addr, size); }

    // Block containing addr. Outside the address space: an Inaccessible block
    // covering the gap below it or everything above it.
    BlockInfo query(uint64_t addr) const;

    // True if [addr, addr+size) is inside the address space and every block in
    // it has the given state, perm and attr (under the masks).
    bool check(uint64_t addr, uint64_t size, MemState state, uint8_t perm, uint8_t attr, uint8_t perm_mask = 0xFF,
               uint8_t attr_mask = 0xFF) const;
    // Same, but any state in the set (bit n = MemState n) is accepted.
    bool check_states(uint64_t addr, uint64_t size, uint32_t state_set, uint8_t perm, uint8_t attr,
                      uint8_t perm_mask = 0xFF, uint8_t attr_mask = 0xFF) const;
    bool is_free(uint64_t addr, uint64_t size) const;

    // Map fresh, zero-filled memory. The range must be free.
    Result map(uint64_t addr, uint64_t size, MemState state, uint8_t perm);
    // Return a range to Free; its contents are dropped (later maps read zero).
    Result unmap(uint64_t addr, uint64_t size);
    // Change state/perm/attr of mapped memory without touching contents.
    void set_state(uint64_t addr, uint64_t size, MemState state);
    void set_perm(uint64_t addr, uint64_t size, uint8_t perm);
    void set_attr(uint64_t addr, uint64_t size, uint8_t mask, uint8_t value);

    // Lowest free, aligned range of `size` bytes inside `region`.
    std::optional<uint64_t> find_free(const Region& region, uint64_t size, uint64_t align = kPageSize) const;

    // Kernel-side access, ignoring guest permissions. The range must be mapped
    // (any state other than Free/Inaccessible); returns nullptr otherwise.
    uint8_t* host_ptr(uint64_t addr, uint64_t size);
    // Guest bytes the host OS holds in RAM (HostBlock::resident).
    bool host_resident(uint64_t* bytes) const { return host_->resident(bytes); }
    // The host block behind guest memory, for statistics (region_stats);
    // block offset = guest address - layout().start().
    const HostBlock& host_block() const { return *host_; }
    HostBlock& host_block() { return *host_; }
    // Copies with user-pointer semantics (what the kernel does for svc
    // pointer arguments): the guest must be allowed to write/read the range.
    bool copy_to_user(uint64_t addr, const void* src, uint64_t size);
    bool copy_from_user(void* dst, uint64_t addr, uint64_t size) const;

    // Write-watch (ADR-0001 point 4): flag pages, collect and clear dirty bits.
    void set_watch(uint64_t addr, uint64_t size, bool on);
    // Write watchpoints (lgp/runtime/write_watch.hpp): stores to these pages
    // call note_trapped_write(). Kept across map/unmap/protect.
    void set_trap(uint64_t addr, uint64_t size, bool on);
    // Calls fn(page_addr) for each dirty watched page in range and clears it.
    template <typename Fn>
    void take_dirty(uint64_t addr, uint64_t size, Fn&& fn);

    const GuestView& view() const { return view_; }

private:
    struct Block {
        uint64_t size;
        MemState state;
        uint8_t perm;
        uint8_t attr;
    };
    using BlockMap = std::map<uint64_t, Block>;

    BlockMap::iterator split_at(uint64_t addr);
    void coalesce(uint64_t from, uint64_t to);
    template <typename Fn>
    void update(uint64_t addr, uint64_t size, Fn&& fn);
    template <typename Pred>
    bool all_blocks(uint64_t addr, uint64_t size, Pred&& pred) const;
    void sync_pages(uint64_t addr, uint64_t size);
    uint64_t page_index(uint64_t addr) const { return (addr - layout_.start()) >> kPageShift; }

    Layout layout_;
    Options options_;
    BlockMap blocks_;
    std::unique_ptr<HostBlock> host_;
    std::unique_ptr<uint8_t[]> page_flags_;
    GuestView view_;
};

template <typename Fn>
void GuestMemory::take_dirty(uint64_t addr, uint64_t size, Fn&& fn) {
    for (uint64_t page = addr & ~(kPageSize - 1); page < addr + size; page += kPageSize) {
        if (!contains(page, kPageSize))
            continue;
        uint8_t& flags = page_flags_[page_index(page)];
        if (std::atomic_ref<uint8_t>(flags).fetch_and(static_cast<uint8_t>(~kPageDirty)) & kPageDirty)
            fn(page);
    }
}

constexpr uint32_t state_bit(MemState s) { return 1u << static_cast<uint32_t>(s); }

}  // namespace lgp::rt
