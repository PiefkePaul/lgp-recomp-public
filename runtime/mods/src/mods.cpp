#include "lgp/mods/mods.hpp"

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <memory>

#include "lgp/lifted/cpu_state.hpp"
#include "lgp/mods/widescreen.hpp"
#include "lgp/runtime/dispatch.hpp"
#include "lgp/runtime/hooks.hpp"
#include "lgp/runtime/log.hpp"
#include "lgp/runtime/patch.hpp"
#include "lgp/runtime/render_hints.hpp"
#include "mod_impl.hpp"

namespace lgp::mods {

namespace {

const std::vector<const detail::ModImpl*>& impls() {
    static const std::vector<const detail::ModImpl*> v = {&detail::wild_battles_mod(),
                                                          &detail::original_encounters_mod(),
                                                          &detail::battle_balls_mod(),
                                                          &detail::battle_throw_mod(),
                                                          &detail::widescreen_mod(),
                                                          &detail::random_encounters_mod(),
                                                          &detail::classic_exp_mod(),
                                                          &detail::online_button_mod(),
                                                          &detail::skip_controller_screen_mod(),
                                                          &detail::audio_free_guard_mod()};
    return v;
}

const detail::ModImpl* find_impl(std::string_view id) {
    for (const detail::ModImpl* m : impls())
        if (m->info.id == id)
            return m;
    return nullptr;
}

std::vector<ModFile>& started() {
    static std::vector<ModFile> v;
    return v;
}

struct LogDoor {
    uint64_t va = 0;
    uint32_t limit = 0;
    std::atomic<uint32_t> calls{0};
    std::vector<HookDump> dumps;  // StartOptions::dumps at this door
    std::vector<HookAct> acts;    // StartOptions::acts at this door
};
std::unique_ptr<LogDoor[]>& log_doors() {
    static std::unique_ptr<LogDoor[]> v;
    return v;
}
size_t g_log_door_count = 0;

void log_hook(lgp::CpuState* ctx, lgp::LiftedFn original, void* user) {
    auto* d = static_cast<LogDoor*>(user);
    const uint32_t n = d->calls.fetch_add(1, std::memory_order_relaxed);
    const bool logged = n < d->limit;
    if (logged)
        rt::log(rt::LogLevel::Info,
                "hook log: 0x%" PRIx64 " call %u x0=0x%" PRIx64 " x1=0x%" PRIx64 " x2=0x%" PRIx64 " x3=0x%" PRIx64
                " lr=0x%" PRIx64,
                d->va, n + 1, ctx->x[0], ctx->x[1], ctx->x[2], ctx->x[3], ctx->x[30]);
    if ((!logged || d->dumps.empty()) && d->acts.empty()) {
        original(ctx);
        return;
    }
    bool after = false;
    for (const HookDump& dump : d->dumps) {
        after |= dump.after;
        if (logged && !dump.after)
            for (const std::string& line : hook_dump_lines(dump, *ctx, n + 1))
                rt::log(rt::LogLevel::Info, "%s", line.c_str());
    }
    const lgp::CpuState entry = *ctx;  // the registers the dumps and acts after the call start from
    uint64_t ret = 0;
    if (!run_hook_acts(d->acts, false, ctx, entry, n + 1, &ret))
        return;  // a ret act took the call, or the guest halted
    original(ctx);
    if (ctx->halt != kHaltNone)
        return;
    ret = ctx->x[0];
    if (logged && after)
        for (const HookDump& dump : d->dumps)
            if (dump.after)
                for (const std::string& line : hook_dump_lines(dump, entry, n + 1, ret))
                    rt::log(rt::LogLevel::Info, "%s", line.c_str());
    const uint64_t x0 = ctx->x[0];
    run_hook_acts(d->acts, true, ctx, entry, n + 1, &ret);
    ctx->x[0] = x0;  // the original's result, whatever the acts after it called
}

std::vector<bool (*)()>& catch_exp_tests() {
    static std::vector<bool (*)()> v;
    return v;
}

void on_catch_exp_amount(lgp::CpuState* ctx, lgp::LiftedFn original, void*) {
    original(ctx);
    if (ctx->halt != kHaltNone)
        return;
    for (bool (*test)() : catch_exp_tests())
        if (test()) {
            ctx->x[0] = 0;
            return;
        }
}

}  // namespace

namespace detail {

bool zero_catch_exp(const ModFile& file, bool (*test)(), std::string* why) {
    if (!hook_role(file, "catch_exp_amount", &on_catch_exp_amount, nullptr, why))
        return false;
    std::vector<bool (*)()>& tests = catch_exp_tests();
    if (std::find(tests.begin(), tests.end(), test) == tests.end())
        tests.push_back(test);
    return true;
}

bool hook_role(const ModFile& file, const char* role, void (*fn)(lgp::CpuState*, lgp::LiftedFn, void*), void* user,
               std::string* why) {
    const std::optional<uint64_t> va = file.hook(role);
    if (!va) {
        *why = "no `hook " + std::string(role) + "` in " + file.path.filename().string();
        return false;
    }
    if (!rt::set_hook(*va, fn, user)) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "0x%" PRIx64, *va);
        *why = "hook " + std::string(role) + " " + buf + " has no door: lift the game with lgp_lift --mods " +
               file.path.parent_path().string();
        return false;
    }
    return true;
}

bool call_guest(lgp::CpuState* ctx, uint64_t va) {
    const LiftedFn fn = rt::lookup_function(va);
    if (fn == nullptr) {
        if (ctx->halt == kHaltNone) {
            ctx->halt = kHaltBadJump;
            ctx->halt_info = va;
        }
        return false;
    }
    ctx->pc = va;
    fn(ctx);
    return ctx->halt == kHaltNone;
}

}  // namespace detail

const std::vector<ModInfo>& known_mods() {
    static const std::vector<ModInfo> v = [] {
        std::vector<ModInfo> out;
        for (const detail::ModImpl* m : impls())
            out.push_back(m->info);
        return out;
    }();
    return v;
}

// A ball that catches leaves the result after which the post-battle process
// skips the storing; wild-battles sends it on to the catch process (P8.20).
const char* needed_mod(std::string_view id) { return id == "battle-balls" ? "wild-battles" : nullptr; }

namespace {
// The needed mod started and does what is needed of it: wild-battles has to
// store a Pokémon caught in a battle, else battle-balls would lose it.
bool need_ready(const char* needs) {
    return enabled(needs) && (std::string_view(needs) != "wild-battles" || detail::wild_battles_catches());
}
}  // namespace

const ModInfo* find_mod(std::string_view id) {
    const detail::ModImpl* m = find_impl(id);
    return m ? &m->info : nullptr;
}

std::vector<std::string> parse_mod_list(std::string_view list) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < list.size()) {
        while (i < list.size() && (list[i] == ',' || list[i] == ' ' || list[i] == '\t'))
            ++i;
        size_t j = i;
        while (j < list.size() && list[j] != ',' && list[j] != ' ' && list[j] != '\t')
            ++j;
        if (j > i) {
            std::string id(list.substr(i, j - i));
            if (std::find(out.begin(), out.end(), id) == out.end())
                out.push_back(std::move(id));
        }
        i = j;
    }
    return out;
}

std::string join_mod_list(const std::vector<std::string>& ids, char sep) {
    std::string out;
    for (const std::string& id : ids) {
        if (!out.empty())
            out += sep;
        out += id;
    }
    return out;
}

bool start(const StartOptions& options, StartResult* result, std::string* error) {
    stop();
    for (const std::string& id : options.enabled)
        if (find_impl(id) == nullptr) {
            *error = "unknown mod " + id;
            return false;
        }
    for (const detail::ModImpl* impl : impls()) {
        const std::string& id = impl->info.id;
        const bool on = std::find(options.enabled.begin(), options.enabled.end(), id) != options.enabled.end();
        if (!on && !(options.standby && impl->info.live))
            continue;
        const char* needs = needed_mod(id);
        if (needs != nullptr &&
            std::find(options.enabled.begin(), options.enabled.end(), needs) == options.enabled.end()) {
            if (on)
                result->messages.push_back(id + ": not started, it needs " + std::string(needs));
            continue;
        }
        std::error_code ec;
        if (impl->info.data) {
            const std::filesystem::path data = options.dir / id / "romfs";
            if (!std::filesystem::is_directory(data, ec)) {
                if (on)
                    result->messages.push_back(id + ": not started, no " + data.string());
                continue;
            }
            ModFile mf;
            mf.id = id;
            mf.path = data;
            impl->on->store(on, std::memory_order_relaxed);
            (on ? result->active : result->standby).push_back(id);
            started().push_back(std::move(mf));
            continue;
        }
        const std::filesystem::path file = options.dir / (id + ".cfg");
        if (!std::filesystem::exists(file, ec)) {
            if (on)
                result->messages.push_back(id + ": not started, no " + file.string());
            continue;
        }
        ModFile mf;
        if (std::string load_error; !load_mod_file(file, &mf, &load_error)) {
            if (on) {
                *error = load_error;
                return false;
            }
            result->messages.push_back(id + ": not started (switched off), " + load_error);
            continue;
        }
        // A mod that fails halfway leaves no hooks behind: the hooks of the
        // mods started before stay, the rest of this one goes.
        std::vector<std::pair<uint64_t, bool>> before;
        for (uint64_t va : mf.hook_entries())
            before.emplace_back(va, rt::find_hook(va) != nullptr);
        std::vector<std::pair<uint64_t, bool>> patches_before;
        for (uint64_t va : mf.patch_entries())
            patches_before.emplace_back(va, rt::detail::patch_installed(va));
        std::string why;
        if (!impl->install(mf, options, &why)) {
            for (const auto& [va, had] : before)
                if (!had)
                    rt::set_hook(va, nullptr);
            for (const auto& [va, had] : patches_before)
                if (!had)
                    rt::set_patch(va, nullptr);
            result->messages.push_back(id + ": not started" + (on ? "" : " (switched off)") + ", " + why);
            continue;
        }
        impl->on->store(on, std::memory_order_relaxed);
        if (!impl->info.ready)
            result->messages.push_back(id + ": started; experimental, it does not change the game yet");
        if (!on)
            result->messages.push_back(id + ": ready, switched off (the menu switches it on)");
        (on ? result->active : result->standby).push_back(id);
        started().push_back(std::move(mf));
    }
    // A mod whose needed mod was named but did not start (no file, a role
    // missing) goes again; only data mods need others, they have no hooks.
    for (size_t i = 0; i < started().size();) {
        const std::string id = started()[i].id;
        const char* needs = needed_mod(id);
        if (needs == nullptr || need_ready(needs)) {
            ++i;
            continue;
        }
        const detail::ModImpl* impl = find_impl(id);
        impl->on->store(false, std::memory_order_relaxed);
        started().erase(started().begin() + static_cast<std::ptrdiff_t>(i));
        for (std::vector<std::string>* list : {&result->active, &result->standby})
            list->erase(std::remove(list->begin(), list->end(), id), list->end());
        result->messages.push_back(id + ": not started, " + needs +
                                   (enabled(needs) ? " stores no catch (from post_result missing)" : " did not start"));
    }
    if (options.hook_log > 0) {
        std::vector<uint64_t> free;
        for (uint64_t va : rt::hookable_entries())
            if (rt::find_hook(va) == nullptr)
                free.push_back(va);
        log_doors() = std::make_unique<LogDoor[]>(free.size());
        g_log_door_count = free.size();
        for (size_t i = 0; i < free.size(); ++i) {
            log_doors()[i].va = free[i];
            log_doors()[i].limit = options.hook_log;
            for (const HookDump& dump : options.dumps)
                if (dump.va == free[i])
                    log_doors()[i].dumps.push_back(dump);
            for (const HookAct& act : options.acts)
                if (act.va == free[i])
                    log_doors()[i].acts.push_back(act);
            rt::set_hook(free[i], &log_hook, &log_doors()[i]);
        }
        result->messages.push_back("hook log: " + std::to_string(free.size()) + " doors, first " +
                                   std::to_string(options.hook_log) + " calls each");
    }
    for (const HookDump& dump : options.dumps)
        if (const auto* slot = rt::find_hook(dump.va); slot == nullptr || slot->fn != &log_hook) {
            char va[32];
            std::snprintf(va, sizeof va, "0x%" PRIx64, dump.va);
            result->messages.push_back(std::string("hook dump: ") + va + " " + dump.expr +
                                       " ignored, needs --hook-log and a door without a mod hook there");
        }
    for (const HookAct& act : options.acts)
        if (const auto* slot = rt::find_hook(act.va); slot == nullptr || slot->fn != &log_hook) {
            char va[32];
            std::snprintf(va, sizeof va, "0x%" PRIx64, act.va);
            result->messages.push_back(std::string("hook act: ") + va + " " + act.text +
                                       " ignored, needs --hook-log and a door without a mod hook there");
        }
    return true;
}

bool enabled(std::string_view id) { return mod_file(id) != nullptr; }

bool needs_met(std::string_view id) {
    const char* needs = needed_mod(id);
    return needs == nullptr || (enabled(id) && need_ready(needs));
}

bool set_active(std::string_view id, bool on) {
    const detail::ModImpl* impl = find_impl(id);
    if (impl == nullptr || !impl->info.live || !enabled(id))
        return false;
    impl->on->store(on, std::memory_order_relaxed);
    return true;
}

bool active(std::string_view id) {
    const detail::ModImpl* impl = find_impl(id);
    return impl != nullptr && enabled(id) && impl->on->load(std::memory_order_relaxed);
}

const ModFile* mod_file(std::string_view id) {
    for (const ModFile& f : started())
        if (f.id == id)
            return &f;
    return nullptr;
}

std::map<std::string, std::vector<uint8_t>> romfs_files(const std::filesystem::path& dir,
                                                        const std::vector<std::string>& ids,
                                                        std::vector<std::string>* messages,
                                                        std::map<std::string, std::string>* owners) {
    namespace fs = std::filesystem;
    std::map<std::string, std::vector<uint8_t>> out;
    std::map<std::string, std::string> owner;
    for (const std::string& id : ids) {
        if (const char* needs = needed_mod(id);
            needs != nullptr && std::find(ids.begin(), ids.end(), needs) == ids.end()) {
            messages->push_back(id + ": files left out, it needs " + needs);
            continue;
        }
        const fs::path root = dir / id / "romfs";
        std::error_code ec;
        if (!fs::is_directory(root, ec))
            continue;
        std::vector<fs::path> files;
        for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec))
            if (it->is_regular_file(ec))
                files.push_back(it->path());
        std::sort(files.begin(), files.end());
        for (const fs::path& f : files) {
            const std::string path = "/" + f.lexically_relative(root).generic_string();
            std::ifstream in(f, std::ios::binary);
            std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            if (!in.good() && !in.eof()) {
                messages->push_back(id + ": cannot read " + f.string());
                continue;
            }
            if (auto it = owner.find(path); it != owner.end()) {
                // A mod that needs another may still be left out after start()
                // (romfs_keep); the earlier mod's file must not be lost then.
                if (needed_mod(id) != nullptr) {
                    messages->push_back(id + ": " + path + " left out, " + it->second + " replaces it already");
                    continue;
                }
                messages->push_back(id + ": " + path + " replaces the file of " + it->second);
            }
            owner[path] = id;
            out[path] = std::move(data);
        }
    }
    if (owners != nullptr)
        *owners = std::move(owner);
    return out;
}

uint64_t hook_log_calls() {
    uint64_t n = 0;
    for (size_t i = 0; log_doors() && i < g_log_door_count; ++i)
        n += log_doors()[i].calls.load(std::memory_order_relaxed);
    return n;
}

void stop() {
    rt::clear_hooks();
    rt::clear_patches();
    rt::set_repeat_hints({});
    detail::stop_widescreen_data();
    detail::set_widescreen_active({});
    started().clear();
    catch_exp_tests().clear();
    for (const detail::ModImpl* impl : impls())
        impl->on->store(false, std::memory_order_relaxed);
    log_doors().reset();
    g_log_door_count = 0;
}

}  // namespace lgp::mods
