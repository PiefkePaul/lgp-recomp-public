// Turns the benchmark NRO into a loaded, relocated image for the harness:
// <out>.img (bytes from the module base, bss included) and <out>.meta (text:
// base, entry, size and one "segment <offset> <size> <r|rw|rx>" line each).
// Keeps the loader (lgp_recomp_core, C++ exceptions) out of the harness, so
// the WebAssembly build of the harness only needs the runtime core.
//
// usage: lgp_bench_image <program.nro> <out-stem>
#include <cinttypes>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "lgp/recomp/link.hpp"
#include "lgp/recomp/loader.hpp"

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s <program.nro> <out-stem>\n", argv[0]);
        return 2;
    }
    namespace rc = lgp::recomp;
    std::vector<rc::Module> mods;
    mods.push_back(rc::load_module_file(argv[1]));
    rc::LinkOptions lo;
    lo.first_base = 0x0800'0000;  // lgp_lift's default --base
    const rc::LinkedImage linked = rc::link(std::move(mods), lo);
    const rc::Module& m = linked.modules.front();
    uint64_t size = m.image.size();
    for (const rc::Segment& s : m.segments)
        if (s.offset + s.size > size) size = s.offset + s.size;
    size = (size + 0xFFF) & ~uint64_t{0xFFF};
    std::vector<uint8_t> bytes(m.image.begin(), m.image.end());
    bytes.resize(size, 0);

    const std::string stem = argv[2];
    std::ofstream img(stem + ".img", std::ios::binary);
    img.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    std::ofstream meta(stem + ".meta");
    meta << "base " << m.base << "\nentry " << (m.base + m.entry_offset) << "\nsize " << size << "\n";
    for (const rc::Segment& s : m.segments) {
        if (s.size == 0) continue;
        const char* perm = s.kind == rc::SegmentKind::Text ? "rx" : s.kind == rc::SegmentKind::Ro ? "r" : "rw";
        meta << "segment " << s.offset << " " << s.size << " " << perm << "\n";
    }
    return img && meta ? 0 : 1;
}
