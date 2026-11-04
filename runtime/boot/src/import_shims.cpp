#include "import_shims.hpp"

#include <array>
#include <cinttypes>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>

#include "lgp/lifted/cpu_state.hpp"
#include "lgp/runtime/log.hpp"
#include "movie_hle.hpp"

namespace lgp::boot {
namespace {

using rt::LogLevel;

// Names of the bound imports, index = thunk index (one process at a time).
std::vector<std::string> g_names;
std::vector<MovieHandler> g_handlers;

void trap(CpuState* ctx, size_t i) {
    rt::log(LogLevel::Error, "boot: call to the undefined import %s (from 0x%" PRIx64 ")", g_names[i].c_str(),
            ctx->x[30]);
    if (ctx->halt == kHaltNone) {
        ctx->halt = kHaltUndefined;
        ctx->halt_info = 0;
    }
}

template <size_t I>
void shim(CpuState* ctx) {
    if (I < g_handlers.size() && g_handlers[I])
        g_handlers[I](ctx);
    else
        trap(ctx, I);
}

template <size_t... I>
constexpr std::array<LiftedFn, sizeof...(I)> make_shims(std::index_sequence<I...>) {
    return {&shim<I>...};
}
constexpr auto kShims = make_shims(std::make_index_sequence<kMaxImportShims>{});

}  // namespace

size_t install_import_shims(rt::Kernel& kernel, const recomp::LinkedImage& image, uint64_t thunk_base,
                            uint64_t data_va, std::vector<LiftedEntry>* host) {
    g_names.clear();
    g_handlers.clear();
    MovieHle& movie = MovieHle::instance();
    movie.reset(kernel, data_va, thunk_base + 4 * kMaxImportShims, host);
    for (const recomp::Import& imp : image.imports) {
        if (imp.target_va != 0 || imp.weak || imp.slot_va == 0)
            continue;
        if (g_names.size() >= kMaxImportShims) {
            rt::log(LogLevel::Warn, "boot: more than %zu undefined imports, the rest stay unbound", kMaxImportShims);
            break;
        }
        const size_t i = g_names.size();
        const uint64_t va = thunk_base + 4 * i;
        uint8_t* slot = kernel.memory().host_ptr(imp.slot_va, 8);
        if (slot == nullptr) {
            rt::log(LogLevel::Warn, "boot: GOT slot 0x%" PRIx64 " of %s not writable", imp.slot_va, imp.name.c_str());
            continue;
        }
        std::memcpy(slot, &va, sizeof(va));
        g_names.push_back(imp.name);
        g_handlers.push_back(movie.handler(imp.name));
        host->push_back({va, kShims[i]});
        rt::log(LogLevel::Debug, "boot: undefined import %s (slot 0x%" PRIx64 ") -> %s", imp.name.c_str(), imp.slot_va,
                g_handlers.back() ? "movie HLE" : "trap");
    }
    return g_names.size();
}

}  // namespace lgp::boot
