// Loader / link tests on synthetic NSO and NRO images (P2.1).
#include <fstream>

#include "a64_encode.hpp"
#include "check.hpp"
#include "lgp/recomp/link.hpp"
#include "lgp/recomp/loader.hpp"
#include "synthetic_module.hpp"

using namespace lgp::recomp;

namespace {

// A module with two functions, an exported symbol, an import with GOT + PLT slot,
// RELATIVE and RELR relocations, an FDE table and an init_array.
synth::Builder basic_builder(bool nro = false) {
    synth::Builder b;
    b.nro = nro;
    using namespace a64;
    // code at text_off(0): fn_a: stp; bl fn_b; ret   fn_b: ret
    b.text_words = {stp_x29_x30_pre(-2), bl(8), RET, RET, NOP, NOP};
    uint64_t fn_a = b.text_off(0), fn_b = b.text_off(3);
    b.symbols.push_back({"fn_a", fn_a, 12, synth::STT_FUNC, synth::STB_GLOBAL, true});
    b.symbols.push_back({"fn_b", fn_b, 4, synth::STT_FUNC, synth::STB_GLOBAL, true});
    b.symbols.push_back({"ext_func", 0, 0, synth::STT_FUNC, synth::STB_GLOBAL, false});
    b.symbols.push_back({"weak_missing", 0, 0, synth::STT_FUNC, synth::STB_WEAK, false});
    b.fdes = {{fn_a, fn_a + 12}, {fn_b, fn_b + 4}};
    b.init_array = {fn_b};
    return b;
}

std::string hexs(std::span<const uint8_t> b) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (uint8_t c : b) { s.push_back(d[c >> 4]); s.push_back(d[c & 15]); }
    return s;
}

}  // namespace

TEST(lz4_literals_and_matches) {
    std::vector<uint8_t> data;
    for (int i = 0; i < 300; ++i) data.push_back(static_cast<uint8_t>(i % 7));
    for (int i = 0; i < 100; ++i) data.push_back(static_cast<uint8_t>(i));
    auto lit = synth::lz4_literals(data);
    CHECK(lz4_block_decompress(lit, data.size()) == data);
    auto cmp = synth::lz4_compress(data);
    CHECK(cmp.size() < data.size());
    CHECK(lz4_block_decompress(cmp, data.size()) == data);
    // Overlapping match "abc" x4 + "X" from the reference python test.
    std::vector<uint8_t> blk = {0x35, 'a', 'b', 'c', 3, 0, 0x10, 'X'};
    std::vector<uint8_t> want = {'a','b','c','a','b','c','a','b','c','a','b','c','X'};
    CHECK(lz4_block_decompress(blk, 13) == want);
    CHECK_THROWS(lz4_block_decompress(synth::lz4_literals({1, 2, 3, 4}), 5), FormatError);
    std::vector<uint8_t> bad_off = {0x10, 'a', 5, 0};
    CHECK_THROWS(lz4_block_decompress(bad_off, 10), FormatError);
}

TEST(nso_uncompressed_and_compressed_parse_identically) {
    synth::Builder b = basic_builder();
    auto plain = b.build_nso(false);
    auto lz = basic_builder().build_nso(true);
    auto lzg = basic_builder().build_nso(true, true);
    CHECK(is_nso(plain));
    CHECK(!is_nro(plain));
    Module m1 = load_nso(plain, "main"), m2 = load_nso(lz, "main"), m3 = load_nso(lzg, "main");
    CHECK(m1.image == m2.image);
    CHECK(m1.image == m3.image);
    CHECK(m2.segments[0].compressed);
    CHECK(!m1.segments[0].compressed);
    for (auto& s : m1.segments) CHECK(s.hash_ok);
    for (auto& s : m2.segments) CHECK(s.hash_ok);
    CHECK_EQ(m1.segments.size(), 4u);
    CHECK_EQ(m1.segments[0].offset, 0u);
    CHECK_EQ(m1.segments[1].offset, 0x1000u);
    CHECK_EQ(m1.segments[2].offset, 0x2000u);
    CHECK_EQ(m1.segments[3].size, 0x200u);
    CHECK_EQ(m1.image.size(), 0x3200u);
    CHECK_STR_EQ(m1.internal_name, "fixture.nss");
    CHECK_STR_EQ(m1.hex_module_id().substr(0, 8), "01020304");
    CHECK_EQ(m1.api_info.size(), 1u);
    CHECK_STR_EQ(m1.api_info[0], "SDK MW+Test+Lib-1_0");
    CHECK_EQ(m1.dynsym.size(), 5u);
    CHECK_STR_EQ(m1.dynsym[1].name, "fn_a");
    CHECK(m1.dynsym[3].defined() == false);
    CHECK_EQ(m1.mod0_offset, 0x2000u);
    CHECK_EQ(m1.fde_starts.size(), 2u);
    CHECK_EQ(m1.fde_ranges.size(), 2u);
    CHECK_EQ(m1.fde_ranges[0].first, b.text_off(0));
    CHECK_EQ(m1.fde_ranges[0].second, b.text_off(0) + 12);
    CHECK_EQ(m1.fde_ranges[1].second, b.text_off(3) + 4);
    CHECK_EQ(m1.entry_offset, 0u);
    CHECK_EQ(m1.header_code_exclusion, 8u);
}

TEST(nso_hash_mismatch_is_reported_not_fatal) {
    auto bytes = basic_builder().build_nso(false);
    bytes[0x100 + 0x20] ^= 0xFF;  // flip a byte in .text after the code start
    Module m = load_nso(bytes, "main");
    CHECK(!m.segments[0].hash_ok);
    CHECK(m.segments[1].hash_ok);
    LoadOptions lo;
    lo.verify_hashes = false;
    Module m2 = load_nso(bytes, "main", lo);
    CHECK(m2.segments[0].hash_ok);
}

TEST(nso_dynsym_fallback_via_dynamic_and_hash) {
    synth::Builder b = basic_builder();
    b.header_dynsym = false;
    Module m = load_nso(b.build_nso(false), "main");
    CHECK_EQ(m.dynsym.size(), 5u);
    CHECK_STR_EQ(m.dynsym[2].name, "fn_b");
    synth::Builder c = basic_builder();
    c.header_dynsym = false;
    c.emit_hash = false;  // count from symtab..strtab gap
    Module m2 = load_nso(c.build_nso(false), "main");
    CHECK_EQ(m2.dynsym.size(), 5u);
}

TEST(nro_parse_marks_header_as_non_code) {
    synth::Builder b = basic_builder(true);
    auto bytes = b.build_nro();
    CHECK(is_nro(bytes));
    Module m = load_module(bytes, "fixture.nro");
    CHECK(m.is_nro);
    CHECK_EQ(m.header_code_exclusion, 0x80u);
    CHECK_EQ(m.image.size(), 0x3200u);  // trailing asset bytes are not part of the image
    CHECK_EQ(m.dynsym.size(), 5u);
    CHECK_EQ(m.fde_starts.size(), 2u);
    CHECK_EQ(m.fde_starts[0], b.text_off(0));
    CHECK_EQ(m.init_array_size, 8u);
}

TEST(malformed_headers_are_rejected) {
    std::vector<uint8_t> junk(0x200, 0xAB);
    CHECK_THROWS(load_module(junk, "x"), FormatError);
    auto bytes = basic_builder().build_nso(false);
    auto bad = bytes;
    synth::put32(bad, 0x18, 0x7000'0000);  // text size huge
    CHECK_THROWS(load_nso(bad, "x"), FormatError);
    bad = bytes;
    synth::put32(bad, 0x24, 0x100);  // ro mem offset overlaps text
    CHECK_THROWS(load_nso(bad, "x"), FormatError);
    bad = bytes;
    synth::put32(bad, 0x60, 0x7FFF'FFFF);  // text file size beyond EOF
    CHECK_THROWS(load_nso(bad, "x"), FormatError);
    auto nro = basic_builder().build_nro();
    synth::put32(nro, 0x18, 0x7FFF'FFF0);  // nro size > file
    CHECK_THROWS(load_nro(nro, "x"), FormatError);
}

TEST(relocations_rela_relr_and_imports_across_modules) {
    using namespace a64;
    // Module "main": imports ext_func from "sdk" via GOT (GLOB_DAT) and PLT (JUMP_SLOT),
    // has a RELATIVE pointer to fn_b, an ABS64 to its own symbol and packed RELR slots.
    synth::Builder mainb = basic_builder();
    uint64_t got = mainb.alloc_data(8), plt_slot = mainb.alloc_data(8), rel_slot = mainb.alloc_data(8), abs_slot = mainb.alloc_data(8);
    mainb.relas.push_back({got, synth::R_GLOB_DAT, "ext_func", 0});
    mainb.relas.push_back({plt_slot, synth::R_JUMP_SLOT, "ext_func", 0});
    mainb.relas.push_back({rel_slot, synth::R_RELATIVE, "", static_cast<int64_t>(mainb.text_off(3))});
    mainb.relas.push_back({abs_slot, synth::R_ABS64, "fn_a", 8});
    uint64_t weak_slot = mainb.alloc_data(8);
    mainb.relas.push_back({weak_slot, synth::R_GLOB_DAT, "weak_missing", 0});
    // RELR: 70 consecutive slots (forces two bitmap words) + one lone slot
    uint64_t relr_base = mainb.alloc_data(70 * 8);
    for (int i = 0; i < 70; ++i) {
        mainb.relr.push_back(relr_base + i * 8);
        mainb.write64(relr_base + i * 8, 0x100 + i);  // addend in place
    }
    uint64_t lone = mainb.alloc_data(8, 64);
    mainb.relr.push_back(lone);
    mainb.write64(lone, 0x3000);
    mainb.needed = {"nnSdk"};

    synth::Builder sdkb;
    sdkb.ro_path = "C:/sdk/nnSdk.nss";
    sdkb.text_words = {RET};
    sdkb.symbols.push_back({"ext_func", sdkb.text_off(0), 4, synth::STT_FUNC, synth::STB_GLOBAL, true});
    sdkb.fdes = {{sdkb.text_off(0), sdkb.text_off(0) + 4}};

    std::vector<Module> mods;
    mods.push_back(load_nso(sdkb.build_nso(true), "sdk"));   // deliberately out of load order
    mods.push_back(load_nso(mainb.build_nso(false), "main"));
    LinkedImage img = link(std::move(mods));
    CHECK_EQ(img.modules.size(), 2u);
    CHECK_STR_EQ(img.modules[0].name, "main");   // sorted: main before sdk
    CHECK_EQ(img.modules[0].base, 0x0800'0000u);
    CHECK_EQ(img.modules[1].base, 0x0800'0000u + 0x3200u + 0xE00u);  // page aligned behind main (0x3200 -> 0x4000)
    const Module& m = img.modules[0];
    const Module& sdk = img.modules[1];
    uint64_t ext_va = sdk.base + sdkb.text_off(0);
    CHECK_EQ(m.read_u64(got), ext_va);
    CHECK_EQ(m.read_u64(plt_slot), ext_va);
    CHECK_EQ(m.read_u64(rel_slot), m.base + mainb.text_off(3));
    CHECK_EQ(m.read_u64(abs_slot), m.base + mainb.text_off(0) + 8);
    CHECK_EQ(m.read_u64(weak_slot), 0u);
    for (int i = 0; i < 70; ++i) CHECK_EQ(m.read_u64(relr_base + i * 8), m.base + 0x100 + i);
    CHECK_EQ(m.read_u64(lone), m.base + 0x3000);
    // Relocation bookkeeping
    uint64_t relr_count = 0, rela_count = 0;
    for (const auto& r : m.relocations) (r.from_relr ? relr_count : rela_count)++;
    CHECK_EQ(relr_count, 71u);
    CHECK_EQ(rela_count, 6u);  // 5 explicit + 1 init_array
    CHECK_EQ(img.stats.relocations_applied, 71u + 6u + 0u);
    CHECK_EQ(img.stats.relocations_unsupported, 0u);
    CHECK_EQ(img.stats.imports_resolved, 1u);
    CHECK_EQ(img.stats.imports_unresolved_weak, 1u);
    CHECK_EQ(img.stats.imports_unresolved_strong, 0u);
    // Import table
    const Import* imp = img.import_by_slot(m.base + got);
    CHECK(imp != nullptr);
    if (imp) {
        CHECK_STR_EQ(imp->name, "ext_func");
        CHECK_EQ(imp->target_va, ext_va);
        CHECK_EQ(imp->to_module, 1);
    }
    CHECK(img.import_by_slot(m.base + plt_slot) == nullptr);  // first slot wins; PLT slot is just patched
    // init_array resolved to module offsets after relocation
    CHECK_EQ(m.init_array.size(), 1u);
    CHECK_EQ(m.init_array[0], mainb.text_off(3));
    // Queries
    CHECK(img.in_text(m.base + mainb.text_off(0)));
    CHECK(!img.in_text(m.base + got));
    CHECK(img.symbol_at(ext_va) != nullptr);
    CHECK_STR_EQ(img.symbol_at(ext_va)->name, "ext_func");
    CHECK(img.symbol_va("fn_b").value_or(0) == m.base + mainb.text_off(3));
    CHECK_THROWS(img.read_u32(0x10), std::out_of_range);
    CHECK(img.diagnostics.empty());
}

TEST(unresolved_strong_import_and_unsupported_reloc_are_diagnosed) {
    synth::Builder b = basic_builder();
    uint64_t slot = b.alloc_data(8);
    b.relas.push_back({slot, synth::R_GLOB_DAT, "ext_func", 0});
    uint64_t tls = b.alloc_data(16);
    b.relas.push_back({tls, synth::R_TLSDESC, "", 0});
    std::vector<Module> mods;
    mods.push_back(load_nso(b.build_nso(false), "main"));
    LinkedImage img = link(std::move(mods));
    CHECK_EQ(img.stats.imports_unresolved_strong, 1u);
    CHECK_EQ(img.stats.relocations_unsupported, 1u);
    CHECK_EQ(img.diagnostics.size(), 2u);
    LinkOptions strict;
    strict.strict = true;
    std::vector<Module> mods2;
    mods2.push_back(load_nso(basic_builder().build_nso(false), "main"));
    mods2.back().relocations.push_back(Relocation{0x2800, kRAarch64TlsDesc, 0, 0});
    CHECK_THROWS(link(std::move(mods2), strict), FormatError);
}

TEST(load_order_rank) {
    CHECK_EQ(load_order_rank("rtld"), 0);
    CHECK_EQ(load_order_rank("main"), 1);
    CHECK_EQ(load_order_rank("subsdk0"), 2);
    CHECK_EQ(load_order_rank("subsdk9"), 11);
    CHECK_EQ(load_order_rank("sdk"), 12);
    CHECK_EQ(load_order_rank("fixture.nro"), 100);
    CHECK_EQ(load_order_rank("main.nso"), 1);
}

TEST(relr_packing_roundtrip) {
    std::vector<uint64_t> offs = {0x100, 0x108, 0x110, 0x300, 0x308 + 63 * 8, 0x1000};
    auto words = synth::Builder::pack_relr(offs);
    // decode with the same algorithm the loader uses
    std::vector<uint64_t> got;
    uint64_t where = 0;
    for (uint64_t e : words) {
        if ((e & 1) == 0) { got.push_back(e); where = e + 8; }
        else { for (unsigned i = 0; (e >>= 1) != 0; ++i) if (e & 1) got.push_back(where + i * 8); where += 63 * 8; }
    }
    std::sort(offs.begin(), offs.end());
    CHECK(got == offs);
}

// Writes the synthetic images to a directory so the Python reference parser can read them
// (used by test_vs_reference.py).
TEST(write_synthetic_images_if_requested) {
    const char* dir = std::getenv("LGP_SYNTH_OUT");
    if (!dir) return;
    synth::Builder b = basic_builder();
    uint64_t slot = b.alloc_data(8);
    b.relas.push_back({slot, synth::R_GLOB_DAT, "ext_func", 0});
    uint64_t plt = b.alloc_data(8);
    b.relas.push_back({plt, synth::R_JUMP_SLOT, "ext_func", 0});
    uint64_t rel = b.alloc_data(8);
    b.relas.push_back({rel, synth::R_RELATIVE, "", 0x40});
    for (int i = 0; i < 5; ++i) { uint64_t s = b.alloc_data(8); b.relr.push_back(s); b.write64(s, 0x10); }
    auto nso = b.build_nso(true, true);
    synth::Builder n = basic_builder(true);
    auto nro = n.build_nro();
    std::ofstream(std::string(dir) + "/synthetic.nso", std::ios::binary).write(reinterpret_cast<const char*>(nso.data()), static_cast<std::streamsize>(nso.size()));
    std::ofstream(std::string(dir) + "/synthetic.nro", std::ios::binary).write(reinterpret_cast<const char*>(nro.data()), static_cast<std::streamsize>(nro.size()));
}

int main(int argc, char** argv) { return tst::run(argc, argv); }
