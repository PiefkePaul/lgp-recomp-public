// P3.1: memory svcs of lgp::rt::Kernel, called with raw registers x0..x7 the
// way lifted code (LGP_SVC) and the Unicorn executor call them.
#include <array>
#include <cstring>
#include <memory>

#include "check.hpp"
#include "lgp/runtime/kernel.hpp"

using namespace lgp::rt;
namespace kr = kernel_result;

namespace {

constexpr uint64_t kMiB = 0x10'0000;

ProcessConfig small_process(uint64_t total = 16 * kMiB, uint64_t heap_region = 16 * kMiB) {
    ProcessConfig c;
    c.total_memory_size = total;
    c.custom_layout = true;
    c.layout = Layout::packed(0x0800'0000, 1 * kMiB, 1 * kMiB, 1 * kMiB, 1 * kMiB, heap_region);
    c.program_id = 0x0100'0000'0000'1234ull;
    return c;
}

struct Regs {
    std::array<uint64_t, 8> x{};
};

Regs call(Kernel& k, uint32_t svc, std::initializer_list<uint64_t> args, SvcOutcome expect = SvcOutcome::Continue) {
    Regs r;
    size_t i = 0;
    for (uint64_t a : args)
        r.x[i++] = a;
    const SvcOutcome o = k.svc(svc, r.x.data());
    CHECK(o == expect);
    return r;
}

uint64_t get_info(Kernel& k, uint32_t id0, uint64_t id1 = 0, Handle h = kPseudoHandleCurrentProcess,
                  Result expect = kSuccess) {
    const Regs r = call(k, kSvcGetInfo, {0, id0, h, id1});
    CHECK_EQ(r.x[0], expect.raw);
    return r.x[1];
}

uint64_t set_heap(Kernel& k, uint64_t size, Result expect = kSuccess) {
    const Regs r = call(k, kSvcSetHeapSize, {0, size});
    CHECK_EQ(r.x[0], expect.raw);
    return r.x[1];
}

MemoryInfo query(Kernel& k, uint64_t info_addr, uint64_t addr) {
    const Regs r = call(k, kSvcQueryMemory, {info_addr, 0, addr});
    CHECK_EQ(r.x[0], kSuccess.raw);
    CHECK_EQ(r.x[1], 0u);  // page info
    MemoryInfo info{};
    CHECK(k.memory().copy_from_user(&info, info_addr, sizeof(info)));
    return info;
}

void test_get_info() {
    Kernel k(small_process());
    const Layout& l = k.memory().layout();
    CHECK_EQ(get_info(k, kInfoHeapRegionAddress), l.heap.base);
    CHECK_EQ(get_info(k, kInfoHeapRegionSize), l.heap.size);
    CHECK_EQ(get_info(k, kInfoAliasRegionAddress), l.alias.base);
    CHECK_EQ(get_info(k, kInfoAliasRegionSize), l.alias.size);
    CHECK_EQ(get_info(k, kInfoStackRegionAddress), l.stack.base);
    CHECK_EQ(get_info(k, kInfoStackRegionSize), l.stack.size);
    CHECK_EQ(get_info(k, kInfoAslrRegionAddress), l.start());
    CHECK_EQ(get_info(k, kInfoAslrRegionSize), l.size());
    CHECK_EQ(get_info(k, kInfoTotalMemorySize), 16 * kMiB);
    CHECK_EQ(get_info(k, kInfoUsedMemorySize), 0u);
    CHECK_EQ(get_info(k, kInfoProgramId), 0x0100'0000'0000'1234ull);
    CHECK_EQ(get_info(k, kInfoIsApplication), 1u);
    CHECK_EQ(get_info(k, kInfoCoreMask), 7u);
    CHECK_EQ(get_info(k, kInfoRandomEntropy, 2, kInvalidHandle), k.config().random_entropy[2]);
    CHECK_EQ(get_info(k, kInfoDebuggerAttached, 0, kInvalidHandle), 0u);

    // User exception context: a 0x200-byte region in thread-local memory, the same one on every
    // call. The SDK stores its dying-message region at +0x1c0 (nn::os::SetDyingMessageRegion).
    const uint64_t uec = get_info(k, kInfoUserExceptionContextAddress);
    CHECK(uec != 0);
    CHECK_EQ(get_info(k, kInfoUserExceptionContextAddress), uec);
    const uint64_t dying[2] = {0x1234, 0x100};
    CHECK(k.memory().copy_to_user(uec + 0x1c0, dying, sizeof(dying)));
    CHECK(k.memory().check(uec, 0x200, MemState::ThreadLocal, kPermRW, kAttrNone));

    get_info(k, kInfoRandomEntropy, 4, kInvalidHandle, kr::kInvalidCombination);
    get_info(k, kInfoHeapRegionAddress, 1, kPseudoHandleCurrentProcess, kr::kInvalidCombination);
    get_info(k, kInfoHeapRegionAddress, 0, 0x1234, kr::kInvalidHandle);
    get_info(k, 0x1000, 0, kPseudoHandleCurrentProcess, kr::kInvalidEnumValue);
}

void test_set_heap_size() {
    Kernel k(small_process());
    const Layout& l = k.memory().layout();
    set_heap(k, 0x1000, kr::kInvalidSize);
    set_heap(k, 18 * kMiB, kr::kOutOfMemory);

    CHECK_EQ(set_heap(k, 4 * kMiB), l.heap.base);
    CHECK_EQ(k.heap_size(), 4 * kMiB);
    CHECK_EQ(get_info(k, kInfoUsedMemorySize), 4 * kMiB);
    BlockInfo b = k.memory().query(l.heap.base);
    CHECK(b.state == MemState::Normal);
    CHECK_EQ(b.size, 4 * kMiB);
    CHECK_EQ(b.perm, kPermRW);

    // Contents survive growing; shrinking drops the tail.
    std::memset(k.memory().host_ptr(l.heap.base, 4 * kMiB), 0x5A, 4 * kMiB);
    CHECK_EQ(set_heap(k, 8 * kMiB), l.heap.base);
    CHECK_EQ(k.memory().host_ptr(l.heap.base, 1)[0], 0x5Au);
    CHECK_EQ(k.memory().host_ptr(l.heap.base + 4 * kMiB, 1)[0], 0u);
    CHECK_EQ(set_heap(k, 2 * kMiB), l.heap.base);
    CHECK(k.memory().query(l.heap.base + 2 * kMiB).state == MemState::Free);
    CHECK_EQ(get_info(k, kInfoUsedMemorySize), 2 * kMiB);

    // Shrinking over lent memory is refused.
    const Regs t = call(k, kSvcCreateTransferMemory, {0, l.heap.base + kMiB, kPageSize, kPermNone});
    CHECK_EQ(t.x[0], kSuccess.raw);
    set_heap(k, 0, kr::kInvalidCurrentMemory);
    CHECK_EQ(call(k, kSvcCloseHandle, {t.x[1]}).x[0], kSuccess.raw);
    CHECK_EQ(set_heap(k, 0), l.heap.base);
    CHECK_EQ(get_info(k, kInfoUsedMemorySize), 0u);
}

void test_heap_limit() {
    // TotalMemorySize below the heap region: the resource limit decides.
    Kernel k(small_process(8 * kMiB, 16 * kMiB));
    CHECK(k.map_owned(k.memory().layout().code.base, 2 * kMiB / 2, MemState::Code, kPermRX).ok());
    set_heap(k, 8 * kMiB, kr::kLimitReached);  // 1 MiB of images + 8 MiB > 8 MiB
    CHECK_EQ(set_heap(k, 6 * kMiB), k.memory().layout().heap.base);
    CHECK_EQ(get_info(k, kInfoUsedMemorySize), 7 * kMiB);
}

void test_query_memory() {
    Kernel k(small_process());
    const Layout& l = k.memory().layout();
    const uint64_t heap = set_heap(k, 2 * kMiB);
    const uint64_t info_at = heap + 0x800;

    MemoryInfo i = query(k, info_at, heap + 0x1234);
    CHECK_EQ(i.addr, heap);
    CHECK_EQ(i.size, 2 * kMiB);
    CHECK_EQ(i.state, static_cast<uint32_t>(MemState::Normal));
    CHECK_EQ(i.perm, static_cast<uint32_t>(kPermRW));
    CHECK_EQ(i.attr, 0u);

    i = query(k, info_at, l.stack.base);
    CHECK_EQ(i.state, static_cast<uint32_t>(MemState::Free));
    CHECK_EQ(i.addr, l.start());
    CHECK_EQ(i.size, l.heap.base - l.start());

    i = query(k, info_at, 0);
    CHECK_EQ(i.state, static_cast<uint32_t>(MemState::Inaccessible));
    CHECK_EQ(i.addr + i.size, l.start());
    i = query(k, info_at, l.end());
    CHECK_EQ(i.state, static_cast<uint32_t>(MemState::Inaccessible));
    CHECK_EQ(i.addr, l.end());

    // Output pointer must be writable guest memory.
    CHECK_EQ(call(k, kSvcQueryMemory, {l.stack.base, 0, heap}).x[0], kr::kInvalidPointer.raw);
}

void test_map_memory() {
    Kernel k(small_process());
    const Layout& l = k.memory().layout();
    const uint64_t heap = set_heap(k, 2 * kMiB);
    const uint64_t src = heap + 0x10000;
    const uint64_t dst = l.stack.base + 0x4000;
    std::memset(k.memory().host_ptr(src, 0x2000), 0x11, 0x2000);

    CHECK_EQ(call(k, kSvcMapMemory, {dst + 1, src, 0x2000}).x[0], kr::kInvalidAddress.raw);
    CHECK_EQ(call(k, kSvcMapMemory, {dst, src, 0x2001}).x[0], kr::kInvalidSize.raw);
    CHECK_EQ(call(k, kSvcMapMemory, {heap + kMiB, src, 0x2000}).x[0], kr::kInvalidMemoryRegion.raw);
    CHECK_EQ(call(k, kSvcMapMemory, {dst, l.misc.base, 0x2000}).x[0], kr::kInvalidCurrentMemory.raw);

    CHECK_EQ(call(k, kSvcMapMemory, {dst, src, 0x2000}).x[0], kSuccess.raw);
    BlockInfo d = k.memory().query(dst);
    CHECK(d.state == MemState::Stack);
    CHECK_EQ(d.perm, kPermRW);
    CHECK_EQ(d.size, 0x2000u);
    BlockInfo s = k.memory().query(src);
    CHECK_EQ(s.perm, kPermNone);
    CHECK_EQ(s.attr, kAttrLocked);
    CHECK_EQ(k.memory().host_ptr(dst, 1)[0], 0x11u);
    CHECK_EQ(get_info(k, kInfoUsedMemorySize), 2 * kMiB);  // an alias, not new memory

    // Mapping the same source twice fails (it is locked now).
    CHECK_EQ(call(k, kSvcMapMemory, {dst + 0x10000, src, 0x2000}).x[0], kr::kInvalidCurrentMemory.raw);

    k.memory().host_ptr(dst + 0x1000, 1)[0] = 0x22;
    CHECK_EQ(call(k, kSvcUnmapMemory, {dst, heap, 0x2000}).x[0], kr::kInvalidCurrentMemory.raw);
    CHECK_EQ(call(k, kSvcUnmapMemory, {dst, src, 0x2000}).x[0], kSuccess.raw);
    CHECK(k.memory().query(dst).state == MemState::Free);
    s = k.memory().query(src);
    CHECK_EQ(s.perm, kPermRW);
    CHECK_EQ(s.attr, kAttrNone);
    CHECK_EQ(s.size, 2 * kMiB);  // merged back into the heap block
    CHECK_EQ(k.memory().host_ptr(src + 0x1000, 1)[0], 0x22u);
}

void test_attributes_and_permissions() {
    Kernel k(small_process());
    const uint64_t heap = set_heap(k, 2 * kMiB);
    CHECK_EQ(call(k, kSvcSetMemoryAttribute, {heap, 0x3000, 8, 8}).x[0], kSuccess.raw);
    CHECK_EQ(k.memory().query(heap).attr, kAttrUncached);
    CHECK_EQ(call(k, kSvcSetMemoryAttribute, {heap, 0x3000, 8, 0}).x[0], kSuccess.raw);
    CHECK_EQ(k.memory().query(heap).attr, kAttrNone);
    CHECK_EQ(call(k, kSvcSetMemoryAttribute, {heap, 0x1000, 0, 8}).x[0], kr::kInvalidCombination.raw);
    CHECK_EQ(call(k, kSvcSetMemoryAttribute, {heap, 0x1000, 1, 1}).x[0], kr::kInvalidCombination.raw);
    CHECK_EQ(call(k, kSvcSetMemoryAttribute, {heap + 2 * kMiB, 0x1000, 8, 8}).x[0], kr::kInvalidCurrentMemory.raw);

    CHECK_EQ(call(k, kSvcSetMemoryPermission, {heap, 0x1000, kPermR}).x[0], kSuccess.raw);
    CHECK_EQ(k.memory().query(heap).perm, kPermR);
    CHECK_EQ(call(k, kSvcSetMemoryPermission, {heap, 0x1000, kPermRW}).x[0], kSuccess.raw);
    CHECK_EQ(call(k, kSvcSetMemoryPermission, {heap, 0x1000, kPermRX}).x[0], kr::kInvalidNewMemoryPermission.raw);
    CHECK_EQ(call(k, kSvcSetMemoryPermission, {heap, 0, kPermR}).x[0], kr::kInvalidSize.raw);
    // Code is not reprotectable.
    const uint64_t code = k.memory().layout().code.base;
    CHECK(k.map_owned(code, kPageSize, MemState::Code, kPermRX).ok());
    CHECK_EQ(call(k, kSvcSetMemoryPermission, {code, 0x1000, kPermRW}).x[0], kr::kInvalidCurrentMemory.raw);
}

void test_transfer_memory() {
    Kernel k(small_process());
    const uint64_t heap = set_heap(k, 2 * kMiB);
    CHECK_EQ(call(k, kSvcCreateTransferMemory, {0, heap, 0x2000, kPermRX}).x[0],
             kr::kInvalidNewMemoryPermission.raw);
    CHECK_EQ(call(k, kSvcCreateTransferMemory, {0, heap + 2 * kMiB, 0x2000, kPermNone}).x[0],
             kr::kInvalidCurrentMemory.raw);

    const Regs t = call(k, kSvcCreateTransferMemory, {0, heap, 0x2000, kPermR});
    CHECK_EQ(t.x[0], kSuccess.raw);
    const Handle h = static_cast<Handle>(t.x[1]);
    CHECK(h != kInvalidHandle);
    auto tmem = k.handles().get_as<KTransferMemory>(h);
    CHECK(tmem && tmem->addr() == heap && tmem->size() == 0x2000 && tmem->perm() == kPermR);
    CHECK_EQ(k.memory().query(heap).perm, kPermR);
    CHECK_EQ(k.memory().query(heap).attr, kAttrLocked);
    // Locked memory cannot be lent twice.
    CHECK_EQ(call(k, kSvcCreateTransferMemory, {0, heap, 0x1000, kPermNone}).x[0], kr::kInvalidCurrentMemory.raw);

    // An HLE service still holding the object keeps the memory lent.
    CHECK_EQ(call(k, kSvcCloseHandle, {h}).x[0], kSuccess.raw);
    CHECK_EQ(k.memory().query(heap).attr, kAttrLocked);
    tmem.reset();
    CHECK_EQ(k.memory().query(heap).attr, kAttrNone);
    CHECK_EQ(k.memory().query(heap).perm, kPermRW);
    CHECK_EQ(call(k, kSvcCloseHandle, {h}).x[0], kr::kInvalidHandle.raw);
}

void test_shared_memory() {
    Kernel k(small_process());
    const Layout& l = k.memory().layout();
    set_heap(k, 2 * kMiB);
    auto shm = std::make_shared<KSharedMemory>(k, 0x4000, kPermR);
    std::memcpy(shm->data(), "hid!", 4);
    Handle h = 0;
    CHECK(k.handles().add(shm, &h).ok());

    const uint64_t at = l.misc.base + 0x10000;
    CHECK_EQ(call(k, kSvcMapSharedMemory, {h, at, 0x4000, kPermRW}).x[0], kr::kInvalidNewMemoryPermission.raw);
    CHECK_EQ(call(k, kSvcMapSharedMemory, {h, at, 0x2000, kPermR}).x[0], kr::kInvalidSize.raw);
    CHECK_EQ(call(k, kSvcMapSharedMemory, {h, l.heap.base + 4 * kMiB, 0x4000, kPermR}).x[0],
             kr::kInvalidMemoryRegion.raw);
    CHECK_EQ(call(k, kSvcMapSharedMemory, {h, l.alias.base, 0x4000, kPermR}).x[0], kr::kInvalidMemoryRegion.raw);
    CHECK_EQ(call(k, kSvcMapSharedMemory, {0x7777, at, 0x4000, kPermR}).x[0], kr::kInvalidHandle.raw);

    CHECK_EQ(call(k, kSvcMapSharedMemory, {h, at, 0x4000, kPermR}).x[0], kSuccess.raw);
    CHECK(k.memory().query(at).state == MemState::Shared);
    CHECK_EQ(k.memory().query(at).perm, kPermR);
    CHECK(std::memcmp(k.memory().host_ptr(at, 4), "hid!", 4) == 0);
    CHECK(shm->data() == k.memory().host_ptr(at, 4));
    shm->data()[0] = 'H';  // the service updates the mapped copy
    CHECK_EQ(call(k, kSvcUnmapSharedMemory, {h, at + 0x1000, 0x4000}).x[0], kr::kInvalidMemoryRegion.raw);
    CHECK_EQ(call(k, kSvcUnmapSharedMemory, {h, at, 0x4000}).x[0], kSuccess.raw);
    CHECK(k.memory().query(at).state == MemState::Free);
    CHECK(std::memcmp(shm->data(), "Hid!", 4) == 0);
}

void test_misc_svcs() {
    Kernel k(small_process());
    const uint64_t heap = set_heap(k, 2 * kMiB);
    std::memcpy(k.memory().host_ptr(heap, 5), "hello", 5);
    CHECK_EQ(call(k, kSvcOutputDebugString, {heap, 5}).x[0], kSuccess.raw);
    CHECK(k.debug_output().size() == 1 && k.debug_output()[0] == "hello");
    CHECK_EQ(call(k, kSvcOutputDebugString, {k.memory().layout().stack.base, 5}).x[0], kr::kInvalidPointer.raw);

    call(k, kSvcExitProcess, {}, SvcOutcome::ExitProcess);
    call(k, kSvcBreak, {1, 2, 3}, SvcOutcome::Break);
    CHECK(k.last_break() == (std::array<uint64_t, 3>{1, 2, 3}));
    call(k, kSvcMapPhysicalMemory, {}, SvcOutcome::Unimplemented);
    call(k, 0x7F, {}, SvcOutcome::Unimplemented);
}

void test_handle_table() {
    HandleTable t(4);
    struct Dummy final : KObject {
        const char* type_name() const override { return "Dummy"; }
    };
    Handle hs[4];
    for (Handle& h : hs) {
        CHECK(t.add(std::make_shared<Dummy>(), &h).ok());
        CHECK(h != 0 && (h & 0xFFFF8000) != 0xFFFF8000);
    }
    Handle extra = 0;
    CHECK(t.add(std::make_shared<Dummy>(), &extra) == kr::kOutOfHandles);
    CHECK(t.close(hs[1]).ok());
    CHECK(t.get(hs[1]) == nullptr);
    CHECK(t.add(std::make_shared<Dummy>(), &extra).ok());
    CHECK(extra != hs[1]);  // same slot, new generation
    CHECK(t.get(hs[1]) == nullptr);
    CHECK(t.get(extra) != nullptr);
    CHECK(t.close(kPseudoHandleCurrentProcess) == kr::kInvalidHandle);
    CHECK_EQ(t.count(), 4u);
}

}  // namespace

int main() {
    RUN(test_get_info);
    RUN(test_set_heap_size);
    RUN(test_heap_limit);
    RUN(test_query_memory);
    RUN(test_map_memory);
    RUN(test_attributes_and_permissions);
    RUN(test_transfer_memory);
    RUN(test_shared_memory);
    RUN(test_misc_svcs);
    RUN(test_handle_table);
    return finish();
}
