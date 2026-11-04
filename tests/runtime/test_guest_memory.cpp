// Guest address space (ADR-0001): layout, block map, host backing, and the
// inline accessors of guest_mem.hpp. Built twice: against the default runtime
// library (unchecked in Release) and against a variant with
// LGP_GUEST_MEM_CHECKED + LGP_GUEST_MEM_WRITE_WATCH, so both access paths run
// natively and in Node.
#include <csetjmp>
#include <cstring>
#include <vector>

#include "check.hpp"
#include "lgp/runtime/memory.hpp"
#include "lgp/runtime/write_watch.hpp"

using namespace lgp::rt;

namespace {

constexpr uint64_t kMiB = 0x10'0000;

Layout small_layout() { return Layout::packed(0x0800'0000, 1 * kMiB, 1 * kMiB, 1 * kMiB, 1 * kMiB, 4 * kMiB); }

void test_standard_layout() {
    const Layout desktop = Layout::standard(0xCD50'0000);
    CHECK_EQ(desktop.code.base, 0x0800'0000u);
    CHECK_EQ(desktop.stack.base, 0x0E00'0000u);
    CHECK_EQ(desktop.alias.base, 0x1200'0000u);
    CHECK_EQ(desktop.misc.base, 0x1300'0000u);
    CHECK_EQ(desktop.misc.end(), 0x2000'0000u);
    CHECK_EQ(desktop.heap.base, 0x2000'0000u);
    CHECK_EQ(desktop.heap.size, 0xCD60'0000u);  // rounded up to 2 MiB
    CHECK(desktop.end() < (uint64_t{1} << 32));

    // The web layout differs only in the heap size (ADR-0001 point 2).
    const Layout web = Layout::standard(0xC000'0000);
    CHECK_EQ(web.heap.base, desktop.heap.base);
    CHECK_EQ(web.misc.base, desktop.misc.base);
    CHECK_EQ(web.end(), 0xE000'0000u);
    CHECK_EQ(web.size(), 0xD800'0000u);  // 3456 MiB of a 4 GiB wasm32 memory
}

void test_block_map() {
    GuestMemory m(small_layout());
    const Layout& l = m.layout();

    BlockInfo b = m.query(l.start());
    CHECK(b.state == MemState::Free);
    CHECK_EQ(b.addr, l.start());
    CHECK_EQ(b.size, l.size());

    b = m.query(0x1000);
    CHECK(b.state == MemState::Inaccessible);
    CHECK_EQ(b.addr, 0u);
    CHECK_EQ(b.size, l.start());
    b = m.query(l.end() + 0x5000);
    CHECK(b.state == MemState::Inaccessible);
    CHECK_EQ(b.addr, l.end());
    CHECK_EQ(b.addr + b.size, 0u);  // up to the end of the 64-bit space

    const uint64_t a = l.heap.base;
    CHECK(m.map(a, 4 * kPageSize, MemState::Normal, kPermRW).ok());
    CHECK(m.map(a + kPageSize, kPageSize, MemState::Normal, kPermRW) == kernel_result::kInvalidCurrentMemory);
    CHECK(m.map(a + 1, kPageSize, MemState::Normal, kPermRW) == kernel_result::kInvalidAddress);
    CHECK(m.check(a, 4 * kPageSize, MemState::Normal, kPermRW, kAttrNone));
    CHECK(!m.check(a, 5 * kPageSize, MemState::Normal, kPermRW, kAttrNone));

    // Splitting and re-merging.
    m.set_perm(a + kPageSize, kPageSize, kPermR);
    b = m.query(a);
    CHECK_EQ(b.size, kPageSize);
    b = m.query(a + kPageSize);
    CHECK_EQ(b.perm, kPermR);
    CHECK_EQ(b.size, kPageSize);
    m.set_perm(a + kPageSize, kPageSize, kPermRW);
    b = m.query(a + 2 * kPageSize);
    CHECK_EQ(b.addr, a);
    CHECK_EQ(b.size, 4 * kPageSize);

    // Zero after remap.
    std::memset(m.host_ptr(a, kPageSize), 0xAB, kPageSize);
    CHECK(m.unmap(a, 4 * kPageSize).ok());
    CHECK(m.host_ptr(a, kPageSize) == nullptr);
    CHECK(m.query(a).state == MemState::Free);
    CHECK_EQ(m.query(a).addr, l.start());  // merged back into one free block
    CHECK_EQ(m.query(a).size, l.size());
    CHECK(m.map(a, kPageSize, MemState::Normal, kPermRW).ok());
    const uint8_t* p = m.host_ptr(a, kPageSize);
    bool zero = true;
    for (uint64_t i = 0; i < kPageSize; ++i)
        zero = zero && p[i] == 0;
    CHECK(zero);
}

void test_find_free() {
    GuestMemory m(small_layout());
    const Layout& l = m.layout();
    auto at = m.find_free(l.stack, 2 * kPageSize);
    CHECK(at.has_value() && *at == l.stack.base);
    CHECK(m.map(l.stack.base, 3 * kPageSize, MemState::Stack, kPermRW).ok());
    at = m.find_free(l.stack, 2 * kPageSize);
    CHECK(at.has_value() && *at == l.stack.base + 3 * kPageSize);
    at = m.find_free(l.stack, 0x10000, 0x10000);
    CHECK(at.has_value() && *at == l.stack.base + 0x10000);
    CHECK(!m.find_free(l.stack, l.stack.size).has_value());
}

void test_user_copies() {
    GuestMemory m(small_layout());
    const uint64_t a = m.layout().heap.base;
    CHECK(m.map(a, kPageSize, MemState::Normal, kPermR).ok());
    const uint32_t v = 0xC0FFEE;
    CHECK(!m.copy_to_user(a, &v, sizeof(v)));  // read-only
    m.set_perm(a, kPageSize, kPermRW);
    CHECK(m.copy_to_user(a + kPageSize - 4, &v, sizeof(v)));
    CHECK(!m.copy_to_user(a + kPageSize - 2, &v, sizeof(v)));  // crosses into unmapped
    uint32_t r = 0;
    CHECK(m.copy_from_user(&r, a + kPageSize - 4, sizeof(r)));
    CHECK_EQ(r, 0xC0FFEEu);
}

// Faults never return to the accessor; the test hook records the fault and
// jumps back into expect_fault().
struct FaultLog {
    unsigned count = 0;
    uint64_t last = 0;
    bool write = false;
    std::jmp_buf env;
};

FaultLog g_faults;

void record_fault(uint64_t va, unsigned, bool write, void*) {
    ++g_faults.count;
    g_faults.last = va;
    g_faults.write = write;
    std::longjmp(g_faults.env, 1);
}

// True if fn faulted (and did not complete).
template <typename Fn>
bool expect_fault(Fn fn) {
    if (setjmp(g_faults.env) != 0)
        return true;
    fn();
    return false;
}

void test_accessors() {
    GuestMemory m(small_layout());
    const uint64_t a = m.layout().heap.base;
    CHECK(m.map(a, 2 * kPageSize, MemState::Normal, kPermRW).ok());
    g_faults = {};
    const FaultHandler previous = set_fault_handler({&record_fault, nullptr});
    FaultLog& faults = g_faults;

    store<uint64_t>(a + 8, 0x1122334455667788ull);
    CHECK_EQ(load<uint64_t>(a + 8), 0x1122334455667788ull);
    CHECK_EQ(load<uint8_t>(a + 8), 0x88u);
    CHECK_EQ(load<uint16_t>(a + 14), 0x1122u);
    // Unaligned access across a page boundary (plain loads/stores allow it).
    store<uint32_t>(a + kPageSize - 2, 0xAABBCCDDu);
    CHECK_EQ(load<uint32_t>(a + kPageSize - 2), 0xAABBCCDDu);

    CHECK(compare_and_swap<uint32_t>(a + 0x40, 0, 7));
    CHECK(!compare_and_swap<uint32_t>(a + 0x40, 0, 9));
    CHECK_EQ(load<uint32_t>(a + 0x40), 7u);
    CHECK(compare_and_swap<uint64_t>(a + 0x48, 0, ~0ull));
    CHECK_EQ(load<uint64_t>(a + 0x48), ~0ull);
    CHECK(compare_and_swap_128(a + 0x50, 0, 0, 1, 2));
    CHECK(!compare_and_swap_128(a + 0x50, 0, 0, 3, 4));
    CHECK_EQ(load<uint64_t>(a + 0x58), 2u);
    CHECK_EQ(faults.count, 0u);

    // A misaligned exclusive faults in every mode.
    CHECK(expect_fault([&] { compare_and_swap<uint32_t>(a + 0x41, 0, 1); }));
    CHECK_EQ(faults.count, 1u);
    CHECK_EQ(faults.last, a + 0x41);

#if LGP_GUEST_MEM_CHECKED
    std::printf("  (checked accessors)\n");
    faults.count = 0;
    CHECK(expect_fault([&] { (void)load<uint32_t>(a + 2 * kPageSize); }));  // unmapped
    CHECK_EQ(faults.count, 1u);
    CHECK(!faults.write);
    m.set_perm(a, kPageSize, kPermR);
    CHECK(expect_fault([&] { store<uint32_t>(a, 1); }));  // read-only
    CHECK(faults.write);
    CHECK_EQ(load<uint32_t>(a + 8), 0x55667788u);  // reads still fine
    CHECK(expect_fault([&] { store<uint32_t>(a + kPageSize - 2, 1); }));  // starts in the read-only page
    CHECK_EQ(load<uint32_t>(a + kPageSize - 2), 0xAABBCCDDu);           // and wrote nothing
    // Address truncation is caught, not wrapped (ADR-0001).
    CHECK(expect_fault([&] { (void)load<uint32_t>(a + (uint64_t{1} << 32)); }));
    CHECK(expect_fault([&] { (void)load<uint8_t>(m.layout().start() - 1); }));
    CHECK_EQ(faults.count, 5u);
    CHECK(!expect_fault([&] { (void)load<uint8_t>(a); }));
#else
    std::printf("  (unchecked accessors)\n");
#endif
    set_fault_handler(previous);
}

void test_write_watch() {
    GuestMemory m(small_layout());
    const uint64_t a = m.layout().heap.base;
    CHECK(m.map(a, 4 * kPageSize, MemState::Normal, kPermRW).ok());
    m.set_watch(a + kPageSize, 2 * kPageSize, true);
    store<uint32_t>(a + 0x10, 1);              // page 0: not watched
    store<uint32_t>(a + kPageSize + 0x10, 1);  // page 1: watched
    store<uint64_t>(a + 3 * kPageSize - 4, 1);  // pages 2 and 3, only 2 watched
    std::vector<uint64_t> dirty;
    m.take_dirty(a, 4 * kPageSize, [&](uint64_t page) { dirty.push_back(page); });
#if LGP_GUEST_MEM_WRITE_WATCH
    std::printf("  (write watch on)\n");
    CHECK_EQ(dirty.size(), 2u);
    CHECK(dirty.size() == 2 && dirty[0] == a + kPageSize && dirty[1] == a + 2 * kPageSize);
    dirty.clear();
    m.take_dirty(a, 4 * kPageSize, [&](uint64_t page) { dirty.push_back(page); });
    CHECK(dirty.empty());  // taking clears
#else
    std::printf("  (write watch off)\n");
    CHECK(dirty.empty());
#endif
    // Permission changes keep the watch flag.
    m.set_perm(a + kPageSize, kPageSize, kPermR);
    m.set_perm(a + kPageSize, kPageSize, kPermRW);
    store<uint32_t>(a + kPageSize, 2);
    dirty.clear();
    m.take_dirty(a, 4 * kPageSize, [&](uint64_t page) { dirty.push_back(page); });
    CHECK_EQ(dirty.size(), LGP_GUEST_MEM_WRITE_WATCH ? 1u : 0u);
}

void test_write_watchpoint() {
    GuestMemory m(small_layout());
    const uint64_t a = m.layout().heap.base;
    std::string error;
    // Before the heap is mapped, as for a heap object the game allocates later.
    const bool armed = add_write_watch(m, a + kPageSize + 0x98, 8, &error);
    CHECK_EQ(armed, write_watch_build());
    if (!armed) {
        std::printf("  (write watch off: %s)\n", error.c_str());
        return;
    }
    CHECK(m.map(a, 4 * kPageSize, MemState::Normal, kPermRW).ok());
    const uint64_t before = write_watch_hits();
    store<uint32_t>(a + 0x98, 1);               // other page
    store<uint32_t>(a + kPageSize + 0x90, 1);   // same page, outside the range
    store<uint64_t>(a + kPageSize + 0x94, 7);   // overlaps the first 4 bytes
    store<uint32_t>(a + kPageSize + 0x9C, 9);   // inside
    store<uint32_t>(a + kPageSize + 0xA0, 1);   // just after
    CHECK_EQ(write_watch_hits() - before, 2u);
    CHECK_EQ(load<uint32_t>(a + kPageSize + 0x9C), 9u);  // the store itself happened
    m.set_perm(a + kPageSize, kPageSize, kPermR);
    m.set_perm(a + kPageSize, kPageSize, kPermRW);
    store<uint32_t>(a + kPageSize + 0x98, 3);  // kept across permission changes
    CHECK_EQ(write_watch_hits() - before, 3u);
    // Filters: zero stores skipped, nothing while disarmed, line limit.
    set_write_watch_filter(true, 5);
    store<uint64_t>(a + kPageSize + 0x98, 0);
    CHECK_EQ(write_watch_hits() - before, 3u);
    set_write_watch_armed(false);
    store<uint32_t>(a + kPageSize + 0x98, 5);
    CHECK_EQ(write_watch_hits() - before, 3u);
    set_write_watch_armed(true);
    store<uint32_t>(a + kPageSize + 0x98, 6);
    CHECK_EQ(write_watch_hits() - before, 4u);
    CHECK_EQ(write_watch_max_lines(), 5u);
    clear_write_watches(m);
    CHECK_EQ(write_watch_max_lines(), kWriteWatchMaxLines);
    store<uint32_t>(a + kPageSize + 0x98, 4);
    CHECK_EQ(write_watch_hits(), 0u);

    std::vector<WriteWatch> parsed;
    CHECK(parse_write_watches("0x1000,0x2000:16", &parsed, &error));
    CHECK(parsed.size() == 2 && parsed[0].va == 0x1000 && parsed[0].size == 8 && parsed[1].size == 16);
    CHECK(!parse_write_watches("0x1000:0", &parsed, &error));
    CHECK(!parse_write_watches("x", &parsed, &error));
}

void test_view_published() {
    {
        GuestMemory m(small_layout());
        CHECK(g_view.base == m.view().base);
        CHECK_EQ(g_view.guest_base, m.layout().start());
        CHECK_EQ(g_view.size, m.layout().size());
    }
    CHECK(g_view.base == nullptr);  // cleared with its owner
}

#if !defined(__EMSCRIPTEN__)
// The real desktop layout only reserves address space; nothing is committed
// until the heap is set, so this is cheap even with 3.3 GiB of guest space.
void test_standard_layout_reservation() {
    GuestMemory m(Layout::standard(0xCD50'0000));
    const uint64_t heap = m.layout().heap.base;
    CHECK(m.map(heap, 0xCD40'0000, MemState::Normal, kPermRW).ok());  // the 3.14 GiB LGP asks for
    store<uint64_t>(heap + 0xCD40'0000 - 8, 42);
    CHECK_EQ(load<uint64_t>(heap + 0xCD40'0000 - 8), 42u);
}
#endif

}  // namespace

int main() {
    RUN(test_standard_layout);
    RUN(test_block_map);
    RUN(test_find_free);
    RUN(test_user_copies);
    RUN(test_accessors);
    RUN(test_write_watch);
    RUN(test_write_watchpoint);
    RUN(test_view_published);
#if !defined(__EMSCRIPTEN__)
    RUN(test_standard_layout_reservation);
#endif
    return finish();
}
