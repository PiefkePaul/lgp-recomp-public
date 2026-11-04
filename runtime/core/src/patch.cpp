#include "lgp/runtime/patch.hpp"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>

#include "lgp/runtime/log.hpp"
#include "lgp/runtime/memory.hpp"

namespace lgp::rt {
namespace {

struct Patch {
    PatchFn fn;
    void* user;
    bool observe;
    std::shared_ptr<std::atomic<uint64_t>> hits;  // shared: run_patch() counts outside the lock
};

std::shared_mutex& patch_mutex() {
    static std::shared_mutex m;
    return m;
}
std::unordered_map<uint64_t, Patch>& patches() {
    static std::unordered_map<uint64_t, Patch> p;
    return p;
}

}  // namespace

namespace detail {
std::atomic<uint32_t> g_patches{0};

bool patch_installed(uint64_t va) {
    std::shared_lock lock(patch_mutex());
    return patches().count(va) != 0;
}
}  // namespace detail

void set_patch(uint64_t va, PatchFn fn, void* user, bool observe) {
    std::unique_lock lock(patch_mutex());
    if (fn != nullptr)
        patches()[va] = {fn, user, observe, std::make_shared<std::atomic<uint64_t>>(0)};
    else
        patches().erase(va);
    detail::g_patches.store(static_cast<uint32_t>(patches().size()), std::memory_order_release);
}

void clear_patches() {
    std::unique_lock lock(patch_mutex());
    patches().clear();
    detail::g_patches.store(0, std::memory_order_release);
}

size_t patch_count() {
    std::shared_lock lock(patch_mutex());
    return patches().size();
}

bool run_patch(CpuState* ctx, uint64_t va) {
    // Only trivially destructible locals live across fn: a guest memory fault
    // inside it leaves by longjmp (run_lifted), which skips destructors.
    PatchFn fn = nullptr;
    void* user = nullptr;
    bool observe = false;
    {
        std::shared_ptr<std::atomic<uint64_t>> hits;
        {
            std::shared_lock lock(patch_mutex());
            const auto it = patches().find(va);
            if (it == patches().end()) return false;
            fn = it->second.fn;
            user = it->second.user;
            observe = it->second.observe;
            hits = it->second.hits;
        }
        if (hits->fetch_add(1, std::memory_order_relaxed) == 0)
            log(LogLevel::Warn, "patch: first pass through 0x%" PRIx64, va);
    }
    fn(ctx, user);  // outside the lock: the function may install or remove patches
    return !observe;
}

namespace {

struct RegisterLog {
    uint64_t va;
    unsigned passes;
    std::atomic<unsigned> seen{0};
};

void log_registers(CpuState* ctx, void* user) {
    auto* r = static_cast<RegisterLog*>(user);
    if (r->seen.fetch_add(1, std::memory_order_relaxed) >= r->passes) return;
    std::string line;
    char buf[40];
    for (int i = 0; i < 31; ++i) {
        std::snprintf(buf, sizeof buf, " x%d=0x%" PRIx64, i, ctx->x[i]);
        line += buf;
    }
    std::snprintf(buf, sizeof buf, " sp=0x%" PRIx64, ctx->sp);
    line += buf;
    // s0-s31 as float (sizes, aspect ratios, angles; callee-saved s8-s15 too).
    for (int i = 0; i < 32; ++i) {
        float f = 0;
        const uint32_t bits = static_cast<uint32_t>(ctx->vreg[i].lo);
        std::memcpy(&f, &bits, 4);
        std::snprintf(buf, sizeof buf, " s%d=%g", i, static_cast<double>(f));
        line += buf;
    }
    log(LogLevel::Warn, "patch log 0x%" PRIx64 ":%s", r->va, line.c_str());
}

// Kept for the process, like the immediates below.
std::vector<std::unique_ptr<RegisterLog>>& register_logs() {
    static std::vector<std::unique_ptr<RegisterLog>> v;
    return v;
}

}  // namespace

void set_register_log(uint64_t va, unsigned passes) {
    auto r = std::make_unique<RegisterLog>();
    r->va = va;
    r->passes = passes;
    RegisterLog* raw = r.get();
    {
        std::unique_lock lock(patch_mutex());
        register_logs().push_back(std::move(r));
    }
    set_patch(va, log_registers, raw, true);
}

std::vector<std::pair<uint64_t, uint64_t>> patch_hits() {
    std::vector<std::pair<uint64_t, uint64_t>> out;
    {
        std::shared_lock lock(patch_mutex());
        for (const auto& [va, p] : patches())
            out.emplace_back(va, p.hits->load(std::memory_order_relaxed));
    }
    std::sort(out.begin(), out.end());
    return out;
}

namespace {

struct Immediate {
    uint8_t rd;
    bool sf;
    uint64_t value;
};

void write_immediate(CpuState* ctx, void* user) {
    const auto* imm = static_cast<const Immediate*>(user);
    if (imm->rd != 31) ctx->x[imm->rd] = imm->sf ? imm->value : (imm->value & 0xFFFF'FFFFu);
}

// One allocation per site, kept for the process: a guest thread may still be
// inside write_immediate() when the patch is replaced.
std::vector<std::unique_ptr<Immediate>>& immediates() {
    static std::vector<std::unique_ptr<Immediate>> v;
    return v;
}

bool parse_number(const std::string& s, uint64_t* out, unsigned* digits) {
    if (s.empty()) return false;
    char* end = nullptr;
    *out = std::strtoull(s.c_str(), &end, 0);
    if (end != s.c_str() + s.size() || s[0] == '-') return false;
    *digits = s.rfind("0x", 0) == 0 || s.rfind("0X", 0) == 0 ? static_cast<unsigned>(s.size() - 2) : 0;
    return true;
}

bool parse_real(const std::string& s, double* out) {
    const size_t slash = s.find('/');
    auto one = [](const std::string& t, double* v) {
        if (t.empty()) return false;
        char* end = nullptr;
        *v = std::strtod(t.c_str(), &end);
        return end == t.c_str() + t.size();
    };
    if (slash == std::string::npos) return one(s, out);
    double a = 0, b = 0;
    if (!one(s.substr(0, slash), &a) || !one(s.substr(slash + 1), &b) || b == 0) return false;
    *out = a / b;
    return true;
}

}  // namespace

bool set_immediate_patch(GuestMemory& memory, uint64_t va, uint64_t value, std::string* error) {
    const uint8_t* p = memory.host_ptr(va, 4);
    if (p == nullptr) {
        *error = "no guest code at this address";
        return false;
    }
    uint32_t raw = 0;
    std::memcpy(&raw, p, 4);
    // MOVN 00, MOVZ 10, MOVK 11 (opc 01 is unallocated): sf opc 100101 hw imm16 Rd.
    const bool move_wide = (raw & 0x1F80'0000u) == 0x1280'0000u && ((raw >> 29) & 3u) != 1u;
    // MOV (bitmask immediate) = ORR Rd, ZR, #imm: sf 01 100100 N immr imms 11111 Rd.
    const bool mov_bitmask = (raw & 0x7F80'03E0u) == 0x3200'03E0u;
    if (!move_wide && !mov_bitmask) {
        *error = "not MOVZ/MOVN/MOVK or MOV (bitmask immediate)";
        return false;
    }
    auto imm = std::make_unique<Immediate>(Immediate{static_cast<uint8_t>(raw & 31u), (raw >> 31) != 0, value});
    Immediate* user = imm.get();
    {
        std::unique_lock lock(patch_mutex());
        immediates().push_back(std::move(imm));
    }
    set_patch(va, &write_immediate, user);
    return true;
}

bool decode_move_immediate(uint32_t insn, unsigned* rd, uint64_t* value) {
    const bool sf = (insn >> 31) != 0;
    const uint64_t width_mask = sf ? ~uint64_t{0} : 0xFFFF'FFFFu;
    *rd = insn & 31u;
    const unsigned opc = (insn >> 29) & 3u;
    if ((insn & 0x1F80'0000u) == 0x1280'0000u && (opc == 0 || opc == 2)) {  // MOVN, MOVZ
        const unsigned hw = (insn >> 21) & 3u;
        if (!sf && hw > 1) return false;
        const uint64_t v = uint64_t((insn >> 5) & 0xFFFFu) << (16 * hw);
        *value = (opc == 0 ? ~v : v) & width_mask;
        return true;
    }
    if ((insn & 0x7F80'03E0u) != 0x3200'03E0u) return false;  // ORR Rd, ZR, #imm
    // DecodeBitMasks (Arm ARM): an element of `size` bits with s+1 ones,
    // rotated right by r, repeated over the register.
    const unsigned n = (insn >> 22) & 1u, immr = (insn >> 16) & 63u, imms = (insn >> 10) & 63u;
    if (!sf && n) return false;
    const unsigned combined = (n << 6) | (~imms & 63u);
    if (combined == 0) return false;
    unsigned len = 6;
    while (!(combined >> len & 1u)) --len;
    if (len == 0) return false;
    const unsigned size = 1u << len, s = imms & (size - 1), r = immr & (size - 1);
    if (s == size - 1) return false;
    const uint64_t esize_mask = size == 64 ? ~uint64_t{0} : (uint64_t{1} << size) - 1;
    uint64_t elem = (uint64_t{1} << (s + 1)) - 1;
    if (r != 0) elem = ((elem >> r) | (elem << (size - r))) & esize_mask;
    uint64_t v = 0;
    for (unsigned i = 0; i < 64; i += size) v |= elem << i;
    *value = v & width_mask;
    return true;
}

bool poke_guest(GuestMemory& memory, uint64_t va, uint64_t value, unsigned bytes, std::string* error) {
    uint8_t* p = bytes == 4 || bytes == 8 ? memory.host_ptr(va, bytes) : nullptr;
    if (p == nullptr) {
        *error = "not mapped";
        return false;
    }
    std::memcpy(p, &value, bytes);  // little-endian host, like the guest
    return true;
}

bool parse_value_patches(const std::string& spec, std::vector<ValuePatch>* out, std::string* error) {
    for (size_t at = 0; at <= spec.size();) {
        const size_t end = std::min(spec.find(',', at), spec.size());
        const std::string item = spec.substr(at, end - at);
        at = end + 1;
        if (item.empty()) {
            if (end == spec.size()) break;
            *error = "empty entry";
            return false;
        }
        const size_t eq = item.find('=');
        ValuePatch v;
        unsigned digits = 0;
        if (eq == std::string::npos || !parse_number(item.substr(0, eq), &v.va, &digits)) {
            *error = item + ": expected VA=VALUE";
            return false;
        }
        const std::string value = item.substr(eq + 1);
        double real = 0;
        if ((value.rfind("f:", 0) == 0 || value.rfind("d:", 0) == 0) && parse_real(value.substr(2), &real)) {
            if (value[0] == 'f') {
                const float f = static_cast<float>(real);
                uint32_t bits = 0;
                std::memcpy(&bits, &f, 4);
                v.value = bits;
            } else {
                std::memcpy(&v.value, &real, 8);
                v.bytes = 8;
            }
        } else if (parse_number(value, &v.value, &digits)) {
            if (digits > 8 || v.value > 0xFFFF'FFFFu) v.bytes = 8;
        } else {
            *error = item + ": VALUE must be an integer, f:X, f:A/B, d:X or d:A/B";
            return false;
        }
        out->push_back(v);
        if (end == spec.size()) break;
    }
    return true;
}

}  // namespace lgp::rt
