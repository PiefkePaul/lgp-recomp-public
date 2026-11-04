#include "lgp/runtime/memory.hpp"

#include <cassert>
#include <cstring>
#include <iterator>
#include <stdexcept>

#include "lgp/platform/host_block.hpp"

namespace lgp::rt {
namespace {

constexpr uint64_t kMiB = 0x10'0000;

uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }

bool page_aligned(uint64_t v) { return (v & (kPageSize - 1)) == 0; }

bool is_mapped_state(MemState s) { return s != MemState::Free && s != MemState::Inaccessible; }

uint8_t page_bits(const MemState state, uint8_t perm) {
    if (!is_mapped_state(state))
        return 0;
    uint8_t bits = 0;
    if (perm & (kPermR | kPermX))
        bits |= kPageRead;
    if (perm & kPermW)
        bits |= kPageWrite | kPageRead;
    return bits;
}

}  // namespace

Layout Layout::standard(uint64_t total_memory_size) {
    Layout l;
    l.code = {0x0800'0000, 96 * kMiB};
    l.stack = {l.code.end(), 64 * kMiB};
    l.alias = {l.stack.end(), 16 * kMiB};
    l.misc = {l.alias.end(), 0x2000'0000 - l.alias.end()};
    l.heap = {0x2000'0000, align_up(total_memory_size, kHeapAlignment)};
    return l;
}

Layout Layout::packed(uint64_t start, uint64_t code, uint64_t stack, uint64_t alias, uint64_t misc, uint64_t heap) {
    Layout l;
    l.code = {start, code};
    l.stack = {l.code.end(), stack};
    l.alias = {l.stack.end(), alias};
    l.misc = {l.alias.end(), misc};
    l.heap = {l.misc.end(), heap};
    return l;
}

GuestMemory::GuestMemory(const Layout& layout) : GuestMemory(layout, Options{}) {}

GuestMemory::GuestMemory(const Layout& layout, const Options& options) : layout_(layout), options_(options) {
    for (const Region* r : {&layout_.code, &layout_.stack, &layout_.alias, &layout_.misc, &layout_.heap}) {
        if (!page_aligned(r->base) || !page_aligned(r->size))
            throw std::invalid_argument("guest layout regions must be page aligned");
    }
    if (options_.at_guest_address ? layout_.size() > (uint64_t{1} << 32)
                                  : layout_.end() > (uint64_t{1} << 32) && !LGP_GUEST_MEM64)
        throw std::invalid_argument("guest layout must end below 4 GiB (ADR-0001)");

    host_ = std::make_unique<HostBlock>(layout_.size(), options_.at_guest_address ? layout_.start() : 0);
    page_flags_ = std::make_unique<uint8_t[]>(layout_.size() >> kPageShift);  // zero = no access
    blocks_.emplace(layout_.start(), Block{layout_.size(), MemState::Free, kPermNone, kAttrNone});
    view_ = GuestView{host_->base(), layout_.start(), layout_.size(), page_flags_.get()};
    view_.bias = reinterpret_cast<uintptr_t>(view_.base) - static_cast<uintptr_t>(view_.guest_base);
    if (options_.publish)
        g_view = view_;
}

GuestMemory::~GuestMemory() {
    if (g_view.base == view_.base)
        g_view = GuestView{};
}

BlockInfo GuestMemory::query(uint64_t addr) const {
    if (addr < layout_.start())
        return {0, layout_.start(), MemState::Inaccessible, kPermNone, kAttrNone};
    if (addr >= layout_.end())
        return {layout_.end(), ~layout_.end() + 1, MemState::Inaccessible, kPermNone, kAttrNone};
    auto it = std::prev(blocks_.upper_bound(addr));
    return {it->first, it->second.size, it->second.state, it->second.perm, it->second.attr};
}

template <typename Pred>
bool GuestMemory::all_blocks(uint64_t addr, uint64_t size, Pred&& pred) const {
    if (size == 0 || !contains(addr, size))
        return false;
    auto it = std::prev(blocks_.upper_bound(addr));
    const uint64_t end = addr + size;
    for (; it != blocks_.end() && it->first < end; ++it) {
        if (!pred(it->second))
            return false;
    }
    return true;
}

bool GuestMemory::check(uint64_t addr, uint64_t size, MemState state, uint8_t perm, uint8_t attr, uint8_t perm_mask,
                        uint8_t attr_mask) const {
    return check_states(addr, size, state_bit(state), perm, attr, perm_mask, attr_mask);
}

bool GuestMemory::check_states(uint64_t addr, uint64_t size, uint32_t state_set, uint8_t perm, uint8_t attr,
                               uint8_t perm_mask, uint8_t attr_mask) const {
    return all_blocks(addr, size, [&](const Block& b) {
        return (state_set & state_bit(b.state)) != 0 && (b.perm & perm_mask) == perm && (b.attr & attr_mask) == attr;
    });
}

bool GuestMemory::is_free(uint64_t addr, uint64_t size) const {
    return check(addr, size, MemState::Free, kPermNone, kAttrNone);
}

GuestMemory::BlockMap::iterator GuestMemory::split_at(uint64_t addr) {
    auto it = std::prev(blocks_.upper_bound(addr));
    if (it->first == addr)
        return it;
    Block tail = it->second;
    const uint64_t head_size = addr - it->first;
    tail.size -= head_size;
    it->second.size = head_size;
    return blocks_.emplace_hint(std::next(it), addr, tail);
}

void GuestMemory::coalesce(uint64_t from, uint64_t to) {
    auto it = std::prev(blocks_.upper_bound(from));
    if (it != blocks_.begin())
        --it;
    while (it != blocks_.end() && it->first <= to) {
        auto next = std::next(it);
        if (next == blocks_.end())
            break;
        const Block& a = it->second;
        const Block& b = next->second;
        if (a.state == b.state && a.perm == b.perm && a.attr == b.attr) {
            it->second.size += b.size;
            blocks_.erase(next);
        } else {
            it = next;
        }
    }
}

template <typename Fn>
void GuestMemory::update(uint64_t addr, uint64_t size, Fn&& fn) {
    assert(contains(addr, size) && page_aligned(addr) && page_aligned(size));
    const uint64_t end = addr + size;
    auto it = split_at(addr);
    if (end < layout_.end())
        split_at(end);
    for (it = blocks_.find(addr); it != blocks_.end() && it->first < end; ++it)
        fn(it->second);
    coalesce(addr, end);
    sync_pages(addr, size);
}

void GuestMemory::sync_pages(uint64_t addr, uint64_t size) {
    const uint64_t end = addr + size;
    auto it = std::prev(blocks_.upper_bound(addr));
    for (; it != blocks_.end() && it->first < end; ++it) {
        const uint64_t lo = std::max(addr, it->first);
        const uint64_t hi = std::min(end, it->first + it->second.size);
        const uint8_t bits = page_bits(it->second.state, it->second.perm);
        for (uint64_t page = page_index(lo); page < page_index(hi); ++page)
            page_flags_[page] = static_cast<uint8_t>((page_flags_[page] & (kPageWatch | kPageDirty | kPageTrap)) | bits);
    }
}

Result GuestMemory::map(uint64_t addr, uint64_t size, MemState state, uint8_t perm) {
    if (!page_aligned(addr) || !page_aligned(size) || size == 0)
        return kernel_result::kInvalidAddress;
    if (!is_free(addr, size))
        return kernel_result::kInvalidCurrentMemory;
    host_->commit_zeroed(addr - layout_.start(), size);
    update(addr, size, [&](Block& b) {
        b.state = state;
        b.perm = perm;
        b.attr = kAttrNone;
    });
    return kSuccess;
}

Result GuestMemory::unmap(uint64_t addr, uint64_t size) {
    if (!page_aligned(addr) || !page_aligned(size) || size == 0 || !contains(addr, size))
        return kernel_result::kInvalidAddress;
    update(addr, size, [](Block& b) {
        b.state = MemState::Free;
        b.perm = kPermNone;
        b.attr = kAttrNone;
    });
    host_->decommit(addr - layout_.start(), size);
    return kSuccess;
}

void GuestMemory::set_state(uint64_t addr, uint64_t size, MemState state) {
    update(addr, size, [&](Block& b) { b.state = state; });
}

void GuestMemory::set_perm(uint64_t addr, uint64_t size, uint8_t perm) {
    update(addr, size, [&](Block& b) { b.perm = perm; });
}

void GuestMemory::set_attr(uint64_t addr, uint64_t size, uint8_t mask, uint8_t value) {
    update(addr, size, [&](Block& b) { b.attr = static_cast<uint8_t>((b.attr & ~mask) | (value & mask)); });
}

std::optional<uint64_t> GuestMemory::find_free(const Region& region, uint64_t size, uint64_t align) const {
    if (size == 0)
        return std::nullopt;
    auto it = std::prev(blocks_.upper_bound(std::max(region.base, layout_.start())));
    for (; it != blocks_.end() && it->first < region.end(); ++it) {
        if (it->second.state != MemState::Free)
            continue;
        const uint64_t lo = align_up(std::max(it->first, region.base), align);
        const uint64_t hi = std::min(it->first + it->second.size, region.end());
        if (lo < hi && hi - lo >= size)
            return lo;
    }
    return std::nullopt;
}

uint8_t* GuestMemory::host_ptr(uint64_t addr, uint64_t size) {
    const bool mapped = all_blocks(addr, size, [](const Block& b) { return is_mapped_state(b.state); });
    return mapped ? host_->base() + (addr - layout_.start()) : nullptr;
}

bool GuestMemory::copy_to_user(uint64_t addr, const void* src, uint64_t size) {
    if (size == 0)
        return true;
    if (!all_blocks(addr, size, [](const Block& b) { return (page_bits(b.state, b.perm) & kPageWrite) != 0; }))
        return false;
    std::memcpy(host_->base() + (addr - layout_.start()), src, size);
    return true;
}

bool GuestMemory::copy_from_user(void* dst, uint64_t addr, uint64_t size) const {
    if (size == 0)
        return true;
    if (!all_blocks(addr, size, [](const Block& b) { return (page_bits(b.state, b.perm) & kPageRead) != 0; }))
        return false;
    std::memcpy(dst, host_->base() + (addr - layout_.start()), size);
    return true;
}

void GuestMemory::set_watch(uint64_t addr, uint64_t size, bool on) {
    for (uint64_t page = addr & ~(kPageSize - 1); page < addr + size; page += kPageSize) {
        if (!contains(page, kPageSize))
            continue;
        uint8_t& flags = page_flags_[page_index(page)];
        flags = static_cast<uint8_t>(on ? (flags | kPageWatch) : (flags & ~(kPageWatch | kPageDirty)));
    }
}

void GuestMemory::set_trap(uint64_t addr, uint64_t size, bool on) {
    for (uint64_t page = addr & ~(kPageSize - 1); page < addr + size; page += kPageSize) {
        if (!contains(page, kPageSize))
            continue;
        uint8_t& flags = page_flags_[page_index(page)];
        flags = static_cast<uint8_t>(on ? (flags | kPageTrap) : (flags & ~kPageTrap));
    }
}

}  // namespace lgp::rt
