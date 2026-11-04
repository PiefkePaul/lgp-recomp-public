// Kernel objects and the per-process handle table.
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "lgp/runtime/result.hpp"
#include "lgp/runtime/svc_types.hpp"

namespace lgp::rt {

class KObject {
public:
    virtual ~KObject() = default;
    virtual const char* type_name() const = 0;
};

// Handles look like Horizon's: (generation << 15) | slot, never 0 and never a
// pseudo-handle. Slots are reused; the generation catches stale handles.
class HandleTable {
public:
    // NPDM HandleTableSize; Horizon's maximum is 1024.
    explicit HandleTable(uint32_t capacity = 1024);

    Result add(std::shared_ptr<KObject> object, Handle* out);
    std::shared_ptr<KObject> get(Handle handle) const;
    template <typename T>
    std::shared_ptr<T> get_as(Handle handle) const {
        return std::dynamic_pointer_cast<T>(get(handle));
    }
    // Drops the table's reference; the object lives on while others hold it.
    Result close(Handle handle);
    uint32_t count() const;

private:
    struct Entry {
        std::shared_ptr<KObject> object;
        uint16_t generation = 0;
    };
    static constexpr uint32_t kSlotBits = 15;
    bool lookup(Handle handle, uint32_t* slot) const;

    mutable std::mutex mutex_;
    std::vector<Entry> entries_;
    std::vector<uint32_t> free_slots_;
    uint16_t next_generation_ = 1;
    uint32_t count_ = 0;
};

}  // namespace lgp::rt
