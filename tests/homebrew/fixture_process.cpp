#include "fixture_process.hpp"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <stdexcept>

#include "lgp/recomp/loader.hpp"
#include "lgp/runtime/system.hpp"

namespace lgp_fixture {
namespace {

namespace rc = lgp::recomp;
using namespace lgp::rt;

constexpr uint64_t kMiB = 0x10'0000;
constexpr uint64_t kMainStackSize = 1 * kMiB;  // homebrew NPDM default (0x100000)

uint64_t page_up(uint64_t v) { return (v + kPageSize - 1) & ~(kPageSize - 1); }

uint64_t fnv1a(const uint8_t* p, uint64_t n) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint64_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

uint64_t segment_offset(const rc::Module& m, rc::SegmentKind kind) {
    for (const rc::Segment& s : m.segments)
        if (s.kind == kind) return s.offset;
    throw std::runtime_error("fixture module without segment kind " + std::to_string(static_cast<int>(kind)));
}

std::string escape(const std::string& s) {
    std::string out;
    for (unsigned char c : s) {
        if (c >= 0x20 && c < 0x7F && c != '\\') {
            out += static_cast<char>(c);
        } else {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\x%02x", c);
            out += buf;
        }
    }
    return out;
}

std::string describe_exit(const ProcessExit& e) {
    std::string s = exit_reason_name(e.reason);
    if (e.reason == ProcessExit::Reason::Crash) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), " halt %u info 0x%" PRIx64 " pc 0x%" PRIx64, e.halt, e.halt_info, e.pc);
        s += buf;
    } else if (e.reason == ProcessExit::Reason::StartFailed) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), " result 0x%x", e.result.raw);
        s += buf;
    }
    return s;
}

}  // namespace

rc::Module load_fixture(const std::string& path) {
    rc::Module m = rc::load_module_file(path);
    // Not linked: the image stays unrelocated, libnx's __nx_dynamic applies
    // the relocations itself (RELR is not idempotent, so it must not happen twice).
    m.base = kModuleBase;
    return m;
}

EndState run_fixture(const rc::Module& m, const RunOptions& options) {
    const uint64_t image = page_up(m.image.size());
    const uint64_t ro = segment_offset(m, rc::SegmentKind::Ro);
    const uint64_t data = segment_offset(m, rc::SegmentKind::Data);

    ProcessConfig cfg;
    // libnx sizes its heap from TotalMemorySize - UsedMemorySize (minus 2 MiB).
    cfg.total_memory_size = 32 * kMiB;
    cfg.custom_layout = true;
    const uint64_t base = options.base;
    cfg.layout = Layout::packed(base, page_up(std::max<uint64_t>(image + options.extra_code, 4 * kMiB)), 2 * kMiB,
                                1 * kMiB, 1 * kMiB, 32 * kMiB);
    cfg.memory_options.at_guest_address = options.at_guest_address;
    cfg.sched_mode = options.sched_mode;
    cfg.trace_svcs = options.trace_svcs;
    Kernel k(cfg);
    const Layout& l = k.memory().layout();

    // Text RX, ro R (both Code), data+bss RW (CodeData, so libnx may
    // re-protect its RELRO part read-only), like Horizon's loader.
    auto map = [&](uint64_t off, uint64_t end, MemState state, uint8_t perm) {
        if (end <= off) return;
        if (k.map_owned(base + off, end - off, state, perm).failed())
            throw std::runtime_error("cannot map the fixture module");
    };
    map(0, ro, MemState::Code, kPermRX);
    map(ro, data, MemState::Code, kPermR);
    map(data, image, MemState::CodeData, kPermRW);
    std::memcpy(k.memory().host_ptr(base, m.image.size()), m.image.data(), m.image.size());
    if (k.map_owned(l.stack.end() - kMainStackSize, kMainStackSize, MemState::Stack, kPermRW).failed())
        throw std::runtime_error("cannot map the main thread stack");
    if (options.after_load)
        options.after_load(k);

    MainThreadParams main;
    main.entry = base + m.entry_offset;
    main.stack_top = l.stack.end();
    const ProcessExit exit = k.run(main);

    EndState s;
    s.exit = describe_exit(exit);
    s.debug_output = k.debug_output();
    GuestMemory& mem = k.memory();
    for (uint64_t addr = l.start(); addr < l.end();) {
        const BlockInfo b = mem.query(addr);
        addr = b.addr + b.size;
        if (b.state == MemState::Free) continue;
        EndState::Block out;
        out.addr = b.addr;
        out.size = b.size;
        out.state = static_cast<uint32_t>(b.state);
        out.perm = b.perm;
        out.hash = fnv1a(mem.host_ptr(b.addr, b.size), b.size);
        s.blocks.push_back(out);
    }
    s.data_addr = base + data;
    const uint8_t* d = mem.host_ptr(s.data_addr, image - data);
    s.data.assign(d, d + (image - data));
    set_active_kernel(nullptr);
    return s;
}

std::string to_text(const EndState& s) {
    std::ostringstream o;
    o << "exit " << s.exit << "\n";
    for (const std::string& line : s.debug_output)
        o << "debug " << escape(line) << "\n";
    char buf[128];
    for (const EndState::Block& b : s.blocks) {
        std::snprintf(buf, sizeof(buf), "block 0x%" PRIx64 " size 0x%" PRIx64 " state 0x%x perm %u fnv 0x%016" PRIx64 "\n",
                      b.addr, b.size, b.state, b.perm, b.hash);
        o << buf;
    }
    return o.str();
}

bool compare(const EndState& e, const EndState& a, const char* en, const char* an) {
    bool ok = true;
    if (e.exit != a.exit) {
        std::printf("  exit differs: %s '%s', %s '%s'\n", en, e.exit.c_str(), an, a.exit.c_str());
        ok = false;
    }
    if (e.debug_output != a.debug_output) {
        std::printf("  debug output differs (%zu vs %zu lines)\n", e.debug_output.size(), a.debug_output.size());
        for (size_t i = 0; i < std::max(e.debug_output.size(), a.debug_output.size()); ++i) {
            const std::string el = i < e.debug_output.size() ? escape(e.debug_output[i]) : "<none>";
            const std::string al = i < a.debug_output.size() ? escape(a.debug_output[i]) : "<none>";
            if (el != al) std::printf("    line %zu: %s '%s', %s '%s'\n", i, en, el.c_str(), an, al.c_str());
        }
        ok = false;
    }
    if (e.data != a.data) {
        for (size_t i = 0; i < std::min(e.data.size(), a.data.size()); ++i)
            if (e.data[i] != a.data[i]) {
                std::printf("  module data differs first at 0x%" PRIx64 ": %s %02x, %s %02x\n", e.data_addr + i, en,
                            e.data[i], an, a.data[i]);
                break;
            }
        ok = false;
    }
    const size_t n = std::max(e.blocks.size(), a.blocks.size());
    for (size_t i = 0; i < n; ++i) {
        if (i >= e.blocks.size() || i >= a.blocks.size()) {
            std::printf("  block count differs: %s %zu, %s %zu\n", en, e.blocks.size(), an, a.blocks.size());
            ok = false;
            break;
        }
        const EndState::Block& x = e.blocks[i];
        const EndState::Block& y = a.blocks[i];
        if (x.addr != y.addr || x.size != y.size || x.state != y.state || x.perm != y.perm || x.hash != y.hash) {
            std::printf("  block differs: %s 0x%" PRIx64 "+0x%" PRIx64 " state 0x%x perm %u fnv 0x%016" PRIx64
                        ", %s 0x%" PRIx64 "+0x%" PRIx64 " state 0x%x perm %u fnv 0x%016" PRIx64 "\n",
                        en, x.addr, x.size, x.state, x.perm, x.hash, an, y.addr, y.size, y.state, y.perm, y.hash);
            ok = false;
        }
    }
    return ok;
}

}  // namespace lgp_fixture
