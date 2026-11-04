#include "lgp/recomp/native_cache.hpp"

#include <cstring>
#include <stdexcept>

namespace lgp::recomp {

namespace {

constexpr char kMagic[8] = {'L', 'G', 'P', 'N', 'A', 'T', 'V', '1'};

// 64-bit FNV-1a over 8-byte words (bytes for the tail): fast enough for the
// tens of MiB of a game's images, and only meant to tell inputs apart.
struct Hash {
    uint64_t h = 0xcbf29ce484222325ull;
    void word(uint64_t v) {
        h ^= v;
        h *= 0x100000001b3ull;
    }
    void bytes(const uint8_t* p, size_t n) {
        word(n);
        size_t i = 0;
        for (; i + 8 <= n; i += 8) {
            uint64_t v;
            std::memcpy(&v, p + i, 8);
            word(v);
        }
        for (; i < n; ++i) word(p[i]);
    }
    void text(std::string_view s) { bytes(reinterpret_cast<const uint8_t*>(s.data()), s.size()); }
};

struct Writer {
    std::vector<uint8_t> out;
    void u64(uint64_t v) {
        const size_t at = out.size();
        out.resize(at + 8);
        std::memcpy(out.data() + at, &v, 8);
    }
    void u32(uint32_t v) {
        const size_t at = out.size();
        out.resize(at + 4);
        std::memcpy(out.data() + at, &v, 4);
    }
    void blob(const void* p, size_t n) {
        u64(n);
        const auto* b = static_cast<const uint8_t*>(p);
        out.insert(out.end(), b, b + n);
    }
};

struct Reader {
    std::span<const uint8_t> in;
    size_t at = 0;
    void need(size_t n) {
        if (in.size() - at < n)
            throw std::runtime_error("truncated");
    }
    uint64_t u64() {
        need(8);
        uint64_t v;
        std::memcpy(&v, in.data() + at, 8);
        at += 8;
        return v;
    }
    uint32_t u32() {
        need(4);
        uint32_t v;
        std::memcpy(&v, in.data() + at, 4);
        at += 4;
        return v;
    }
    // A count of items of `size` bytes that must still fit into the input.
    uint64_t count(size_t size) {
        const uint64_t n = u64();
        if (n > (in.size() - at) / size)
            throw std::runtime_error("bad count");
        return n;
    }
};

}  // namespace

uint64_t native_cache_key(const LinkedImage& image, std::string_view config_text, uint64_t area_base,
                          std::string_view salt) {
    Hash h;
    h.word(kNativeCacheVersion);
    h.word(area_base);
    h.text(config_text);
    h.text(salt);
    h.word(image.modules.size());
    for (const Module& m : image.modules) {
        h.text(m.name);
        h.word(m.base);
        h.bytes(m.image.data(), m.image.size());
    }
    return h.h;
}

std::vector<uint8_t> encode_native_cache(const NativeRewriteRecord& r, uint64_t key) {
    const NativePatch& p = r.patch;
    Writer w;
    w.out.insert(w.out.end(), kMagic, kMagic + sizeof kMagic);
    w.u32(kNativeCacheVersion);
    w.u32(0);
    w.u64(key);
    for (const uint64_t v : {p.area_base, p.gate_entry, p.data_base, p.host_fn_slot, p.code_size, p.sites_renamed,
                             p.spill_stubs, p.gate_stubs, r.x18_unpatched, r.gate_unpatched})
        w.u64(v);
    w.blob(p.area.data(), p.area.size());
    w.u64(p.modules.size());
    for (const NativeModulePatch& m : p.modules) {
        w.blob(m.module.data(), m.module.size());
        w.u64(m.base);
        w.u64(m.words.size());
        for (const auto& [va, word] : m.words) {
            w.u64(va);
            w.u32(word);
        }
    }
    w.u64(p.stubs.size());
    for (const auto& [site, stub] : p.stubs) {
        w.u64(site);
        w.u64(stub);
    }
    Hash h;
    h.bytes(w.out.data(), w.out.size());
    w.u64(h.h);
    return std::move(w.out);
}

bool decode_native_cache(std::span<const uint8_t> bytes, uint64_t key, NativeRewriteRecord* out, std::string* why) {
    auto fail = [&](const char* reason) {
        if (why != nullptr)
            *why = reason;
        return false;
    };
    if (bytes.size() < sizeof kMagic + 16 + 8 || std::memcmp(bytes.data(), kMagic, sizeof kMagic) != 0)
        return fail("not a native rewrite cache");
    Hash h;
    h.bytes(bytes.data(), bytes.size() - 8);
    uint64_t sum;
    std::memcpy(&sum, bytes.data() + bytes.size() - 8, 8);
    if (sum != h.h)
        return fail("damaged");
    try {
        Reader rd{bytes.first(bytes.size() - 8), sizeof kMagic};
        if (rd.u32() != kNativeCacheVersion)
            return fail("other version");
        rd.u32();
        if (rd.u64() != key)
            return fail("other modules, config or build");
        NativeRewriteRecord r;
        NativePatch& p = r.patch;
        for (uint64_t* v : {&p.area_base, &p.gate_entry, &p.data_base, &p.host_fn_slot, &p.code_size,
                            &p.sites_renamed, &p.spill_stubs, &p.gate_stubs, &r.x18_unpatched, &r.gate_unpatched})
            *v = rd.u64();
        const uint64_t area = rd.count(1);
        p.area.assign(rd.in.begin() + rd.at, rd.in.begin() + rd.at + area);
        rd.at += area;
        if (p.code_size > p.area.size())
            return fail("bad code size");
        p.modules.resize(rd.count(8 + 8 + 8));
        for (NativeModulePatch& m : p.modules) {
            const uint64_t n = rd.count(1);
            m.module.assign(reinterpret_cast<const char*>(rd.in.data() + rd.at), n);
            rd.at += n;
            m.base = rd.u64();
            m.words.resize(rd.count(12));
            for (auto& [va, word] : m.words) {
                va = rd.u64();
                word = rd.u32();
            }
        }
        p.stubs.resize(rd.count(16));
        for (auto& [site, stub] : p.stubs) {
            site = rd.u64();
            stub = rd.u64();
        }
        if (rd.at != rd.in.size())
            return fail("trailing bytes");
        *out = std::move(r);
        return true;
    } catch (const std::runtime_error&) {
        return fail("truncated or bad counts");
    }
}

}  // namespace lgp::recomp
