// Patch sites of game mods (lgp/runtime/patch.hpp) end to end: program.S,
// lifted with --patch-site patch_site --patch-site imm_site, runs with and
// without host functions at those sites, with a rodata value poked, and the
// value-patch lists of the boot program's diagnosis switches are parsed.
//
// usage: lgp_test_patch_<shape> <program.nro> <program.elf>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../runtime/check.hpp"
#include "lgp/lifted/lifted.hpp"
#include "lgp/recomp/elf_symbols.hpp"
#include "lgp/recomp/link.hpp"
#include "lgp/recomp/loader.hpp"
#include "lgp/runtime/dispatch.hpp"
#include "lgp/runtime/patch.hpp"

extern const lgp::LiftedEntry lgp_lifted_entries[];
extern const size_t lgp_lifted_entries_count;

namespace {

using namespace lgp::rt;
namespace rc = lgp::recomp;

constexpr uint64_t kMiB = 0x10'0000;
constexpr uint64_t kBase = 0x0800'0000;  // lgp_lift's default --base
constexpr uint64_t kStackSize = 64 * 1024;
constexpr uint32_t kSentinelBrk = 0x4C47;  // crt0.S

struct Image {
    rc::LinkedImage linked;
    uint64_t entry = 0, size = 0, patch_site = 0, imm_site = 0, orr_site = 0, ro_value = 0;
};

Image load(const char* nro, const char* elf) {
    Image im;
    std::vector<rc::Module> mods;
    mods.push_back(rc::load_module_file(nro));
    rc::LinkOptions lo;
    lo.first_base = kBase;
    im.linked = rc::link(std::move(mods), lo);
    const rc::Module& m = im.linked.modules.front();
    im.entry = m.base + m.entry_offset;
    im.size = (m.image.size() + 0xFFF) & ~uint64_t{0xFFF};
    for (const rc::Symbol& s : rc::read_elf_symbols_file(elf).symbols)
        if (s.name == "patch_site") im.patch_site = m.base + s.value;
        else if (s.name == "imm_site") im.imm_site = m.base + s.value;
        else if (s.name == "orr_site") im.orr_site = m.base + s.value;
        else if (s.name == "ro_value") im.ro_value = m.base + s.value;
    CHECK(im.patch_site != 0);
    CHECK(im.imm_site != 0);
    CHECK(im.ro_value != 0);
    return im;
}

struct Outcome {
    uint32_t halt = 0;
    uint64_t halt_info = 0, x0 = 0;
};

// setup runs with the kernel active and the image loaded (value patches).
Outcome run(const Image& im, void (*setup)(GuestMemory&, const Image&) = nullptr) {
    const rc::Module& m = im.linked.modules.front();
    ProcessConfig cfg;
    cfg.total_memory_size = 16 * kMiB;
    cfg.custom_layout = true;
    cfg.layout = Layout::packed(kBase, 1 * kMiB, 1 * kMiB, 1 * kMiB, 1 * kMiB, 8 * kMiB);
    Kernel k(cfg);
    const Layout& l = k.memory().layout();
    CHECK(k.map_owned(l.code.base, im.size, MemState::Code, kPermRW).ok());
    std::memcpy(k.memory().host_ptr(l.code.base, im.size), m.image.data(), m.image.size());
    CHECK(k.map_owned(l.stack.end() - kStackSize, kStackSize, MemState::Stack, kPermRW).ok());
    if (setup != nullptr) setup(k.memory(), im);  // before an active kernel, as in the boot program
    set_active_kernel(&k);

    lgp::CpuState cpu{};
    cpu.sp = l.stack.end();
    cpu.pc = im.entry;
    const lgp::LiftedFn entry = lookup_function(im.entry);
    CHECK(entry != nullptr);
    if (entry != nullptr) run_lifted(&cpu, entry);
    set_active_kernel(nullptr);
    return {cpu.halt, cpu.halt_info, cpu.x[0]};
}

struct Calls {
    int count = 0;
    uint64_t pc = 0;
};

// Instead of `add x2, x2, x1`: x2 += 100 * x1.
void times_hundred(lgp::CpuState* ctx, void* user) {
    auto* calls = static_cast<Calls*>(user);
    ++calls->count;
    calls->pc = ctx->pc;
    ctx->x[2] += 100 * ctx->x[1];
}

// Stops the guest at the first pass, as a failing mod would.
void halt_guest(lgp::CpuState* ctx, void*) {
    ctx->halt = lgp::kHaltUndefined;
    ctx->halt_info = 0x4D4F44;
}

void immediate_and_poke(GuestMemory& mem, const Image& im) {
    std::string error;
    CHECK(set_immediate_patch(mem, im.imm_site, 0x10000 + 7, &error));  // w3 = 0x10007
    CHECK(poke_guest(mem, im.ro_value, 1000, 4, &error));
    CHECK(set_immediate_patch(mem, im.orr_site, 1920 + 400, &error));  // w5 = 2320 - 1920 = 400
    // Neither a non-move-wide instruction nor unmapped memory.
    CHECK(!set_immediate_patch(mem, im.patch_site, 1, &error));
    CHECK(!poke_guest(mem, 0x10, 1, 4, &error));
}

void parse_cases() {
    std::vector<ValuePatch> v;
    std::string error;
    CHECK(parse_value_patches("0x100=5,256=0x12345678,0x200=f:21/9,0x300=d:0.5,0x400=0x100000000", &v, &error));
    CHECK_EQ(v.size(), 5u);
    if (v.size() == 5) {
        CHECK_EQ(v[0].va, 0x100u);
        CHECK_EQ(v[0].value, 5u);
        CHECK_EQ(v[0].bytes, 4u);
        CHECK_EQ(v[1].va, 256u);
        CHECK_EQ(v[1].value, 0x12345678u);
        const float f = 21.0f / 9.0f;
        uint32_t bits = 0;
        std::memcpy(&bits, &f, 4);
        CHECK_EQ(v[2].value, bits);
        CHECK_EQ(v[3].value, 0x3FE0000000000000u);
        CHECK_EQ(v[3].bytes, 8u);
        CHECK_EQ(v[4].bytes, 8u);
    }
    v.clear();
    CHECK(parse_value_patches("", &v, &error));
    CHECK(v.empty());
    for (const char* bad : {"0x100", "x=1", "0x100=", "0x100=f:", "0x100=f:1/0", "0x100=-1", "0x100=1,,0x200=2"})
        CHECK(!parse_value_patches(bad, &v, &error));
}

void expect_sentinel(const Outcome& o) {
    CHECK_EQ(o.halt, static_cast<uint32_t>(lgp::kHaltBrk));
    CHECK_EQ(o.halt_info, kSentinelBrk);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s <program.nro> <program.elf>\n", argv[0]);
        return 2;
    }
    const Image im = load(argv[1], argv[2]);
    register_functions(lgp_lifted_entries, lgp_lifted_entries_count);

    std::printf("[no patch: the original instruction]\n");
    Outcome o = run(im);
    expect_sentinel(o);
    CHECK_EQ(o.x0, 16u);

    std::printf("[patch installed at 0x%llx]\n", static_cast<unsigned long long>(im.patch_site));
    Calls calls;
    set_patch(im.patch_site, &times_hundred, &calls);
    CHECK_EQ(patch_count(), 1u);
    CHECK(patch_active(im.patch_site));
    CHECK(!patch_active(im.patch_site + 4));
    o = run(im);
    expect_sentinel(o);
    CHECK_EQ(o.x0, 1501u);
    CHECK_EQ(calls.count, 5);
    CHECK_EQ(calls.pc, im.patch_site + 4);
    const auto hits = patch_hits();
    CHECK_EQ(hits.size(), 1u);
    if (!hits.empty()) {
        CHECK_EQ(hits[0].first, im.patch_site);
        CHECK_EQ(hits[0].second, 5u);
    }

    std::printf("[a patch elsewhere does not touch the site]\n");
    set_patch(im.patch_site, nullptr);
    set_patch(im.patch_site + 4, &times_hundred, &calls);
    o = run(im);
    expect_sentinel(o);
    CHECK_EQ(o.x0, 16u);
    CHECK_EQ(calls.count, 5);
    clear_patches();
    CHECK_EQ(patch_count(), 0u);

    std::printf("[a patch that halts the guest]\n");
    set_patch(im.patch_site, &halt_guest);
    o = run(im);
    CHECK_EQ(o.halt, static_cast<uint32_t>(lgp::kHaltUndefined));
    CHECK_EQ(o.halt_info, 0x4D4F44u);

    std::printf("[removed again]\n");
    set_patch(im.patch_site, nullptr);
    o = run(im);
    expect_sentinel(o);
    CHECK_EQ(o.x0, 16u);

    std::printf("[an observing patch: the instruction still runs]\n");
    calls = Calls{};
    set_patch(im.patch_site, &times_hundred, &calls, true);
    o = run(im);
    expect_sentinel(o);
    CHECK_EQ(o.x0, 1516u);  // 100 * x1 from the patch plus x1 from the add, each pass
    CHECK_EQ(calls.count, 5);
    set_register_log(im.patch_site, 2);  // logs two lines, result unchanged
    o = run(im);
    expect_sentinel(o);
    CHECK_EQ(o.x0, 16u);
    clear_patches();

    std::printf("[immediate patch on MOVK, rodata poke]\n");
    o = run(im, &immediate_and_poke);
    expect_sentinel(o);
    CHECK_EQ(o.x0, 15u + 0x10007u + 1000u + 400u);
    clear_patches();

    std::printf("[value patch lists]\n");
    parse_cases();
    return finish();
}
