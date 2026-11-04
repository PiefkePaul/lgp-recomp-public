// Profanity filter stand-in, see ngc_hle.hpp.
#include "ngc_hle.hpp"

#include <cinttypes>
#include <string>

#include "lgp/recomp/sdk_hooks.hpp"
#include "lgp/runtime/hooks.hpp"
#include "lgp/runtime/log.hpp"

namespace lgp::boot {
namespace {

using recomp::SdkHookRole;
using rt::LogLevel;

rt::Kernel* g_kernel = nullptr;

uint64_t& ret(CpuState* ctx) { return ctx->x[0]; }

void initialize(CpuState* ctx, LiftedFn, void*) {
    static bool said = false;
    if (!said) {
        said = true;
        rt::log(LogLevel::Info, "ngc: profanity filter without NgWord2 (no systemdata/0100000000000823): "
                                "nothing is filtered");
    }
    ret(ctx) = 0;  // Result success
}

void finalize(CpuState*, LiftedFn, void*) {}

void content_version(CpuState* ctx, LiftedFn, void*) { ret(ctx) = 0; }

// CheckProfanityWords(this, u32* masks, u32 patterns, const T** words, size_t n):
// one mask per word, 0 = no profanity in any pattern.
void check_words(CpuState* ctx, LiftedFn, void*) {
    const uint64_t masks = ctx->x[1], n = ctx->x[4];
    const uint32_t zero = 0;
    bool ok = masks != 0 && n <= 0x10000;
    for (uint64_t i = 0; ok && i < n; ++i)
        ok = g_kernel->memory().copy_to_user(masks + 4 * i, &zero, 4);
    if (!ok)
        rt::log(LogLevel::Warn, "ngc: CheckProfanityWords with masks 0x%" PRIx64 " for %" PRIu64 " words not written",
                masks, n);
    ret(ctx) = 0;
}

// MaskProfanityWordsInText(this, int* masked, T* text, u32 patterns): the
// text stays as it is.
void mask_text(CpuState* ctx, LiftedFn, void*) {
    const int32_t zero = 0;
    if (ctx->x[1] != 0)
        g_kernel->memory().copy_to_user(ctx->x[1], &zero, 4);
    ret(ctx) = 0;
}

rt::HookFn hook_for(SdkHookRole role) {
    switch (role) {
    case SdkHookRole::NgcInitialize: return &initialize;
    case SdkHookRole::NgcFinalize: return &finalize;
    case SdkHookRole::NgcContentVersion: return &content_version;
    case SdkHookRole::NgcCheckWords: return &check_words;
    case SdkHookRole::NgcMaskText: return &mask_text;
    }
    return nullptr;
}

}  // namespace

NgcHleResult install_ngc_hle(rt::Kernel& kernel, const recomp::LinkedImage& image) {
    g_kernel = &kernel;
    NgcHleResult r;
    for (const recomp::SdkHookSymbol& s : recomp::kSdkHookSymbols) {
        const std::optional<uint64_t> va = image.symbol_va(std::string(s.name));
        if (!va)
            continue;
        ++r.exported;
        if (rt::set_hook(*va, hook_for(s.role)))
            ++r.installed;
        else
            rt::log(LogLevel::Warn, "ngc: %.*s at 0x%" PRIx64 " has no hook door (lifted before lgp_lift knew it?)",
                    static_cast<int>(s.name.size()), s.name.data(), *va);
    }
    return r;
}

void remove_ngc_hle(const recomp::LinkedImage& image) {
    for (const recomp::SdkHookSymbol& s : recomp::kSdkHookSymbols)
        if (const std::optional<uint64_t> va = image.symbol_va(std::string(s.name)); va && rt::is_hookable(*va))
            rt::set_hook(*va, nullptr);
}

}  // namespace lgp::boot
