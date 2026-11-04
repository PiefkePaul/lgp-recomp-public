#include "lgp/runtime/handle_table.hpp"

namespace lgp::rt {

HandleTable::HandleTable(uint32_t capacity) : entries_(capacity) {
    free_slots_.reserve(capacity);
    for (uint32_t i = capacity; i-- > 0;)
        free_slots_.push_back(i);
}

Result HandleTable::add(std::shared_ptr<KObject> object, Handle* out) {
    std::lock_guard lock(mutex_);
    if (free_slots_.empty())
        return kernel_result::kOutOfHandles;
    const uint32_t slot = free_slots_.back();
    free_slots_.pop_back();
    // 15-bit generation, never 0, so no handle is 0 or collides with 0xFFFF8xxx.
    const uint16_t gen = next_generation_;
    next_generation_ = static_cast<uint16_t>(next_generation_ % 0x7FFF + 1);
    entries_[slot] = Entry{std::move(object), gen};
    ++count_;
    *out = (static_cast<Handle>(gen) << kSlotBits) | slot;
    return kSuccess;
}

bool HandleTable::lookup(Handle handle, uint32_t* slot) const {
    const uint32_t s = handle & ((1u << kSlotBits) - 1);
    const uint32_t gen = handle >> kSlotBits;
    if (gen == 0 || gen > 0x7FFF || s >= entries_.size())
        return false;
    const Entry& e = entries_[s];
    if (!e.object || e.generation != gen)
        return false;
    *slot = s;
    return true;
}

std::shared_ptr<KObject> HandleTable::get(Handle handle) const {
    std::lock_guard lock(mutex_);
    uint32_t slot = 0;
    return lookup(handle, &slot) ? entries_[slot].object : nullptr;
}

Result HandleTable::close(Handle handle) {
    std::shared_ptr<KObject> released;  // destroyed after the lock is dropped
    {
        std::lock_guard lock(mutex_);
        uint32_t slot = 0;
        if (!lookup(handle, &slot))
            return kernel_result::kInvalidHandle;
        released = std::move(entries_[slot].object);
        entries_[slot] = Entry{};
        free_slots_.push_back(slot);
        --count_;
    }
    return kSuccess;
}

uint32_t HandleTable::count() const {
    std::lock_guard lock(mutex_);
    return count_;
}

}  // namespace lgp::rt
