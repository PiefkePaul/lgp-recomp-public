#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "lgp/lifted/cpu_state.hpp"
#include "lgp/mods/mods.hpp"
#include "lgp/runtime/guest_mem.hpp"
#include "lgp/runtime/log.hpp"
#include "mod_impl.hpp"

namespace lgp::mods {

namespace {

constexpr uint32_t kMaxDump = 4096;

bool parse_expr(std::string_view s, HookDump* d) {
    if (s.rfind("sp", 0) == 0) {
        d->reg = HookDump::kSp;
        s.remove_prefix(2);
    } else if (s.rfind("tp", 0) == 0) {
        d->reg = HookDump::kTp;
        s.remove_prefix(2);
    } else if (s.rfind("ret", 0) == 0) {
        d->reg = HookDump::kRet;
        s.remove_prefix(3);
    } else if (!s.empty() && s[0] == 'x') {
        size_t n = 1;
        while (n < s.size() && s[n] >= '0' && s[n] <= '9')
            ++n;
        uint64_t reg = 0;
        if (n == 1 || n > 3 || !parse_number(s.substr(1, n - 1), &reg) || reg > 30)
            return false;
        d->reg = static_cast<unsigned>(reg);
        s.remove_prefix(n);
    } else {
        return false;
    }
    while (!s.empty()) {
        HookDump::Step step;
        if (s[0] != '>' && s[0] != '+')
            return false;
        step.load = s[0] == '>';
        s.remove_prefix(1);
        const size_t end = std::min(s.find_first_of(">+"), s.size());
        if (!parse_number(s.substr(0, end), &step.offset))
            return false;
        s.remove_prefix(end);
        d->steps.push_back(step);
    }
    return true;
}

bool readable(uint64_t va, uint64_t size) {
    return size <= kMaxDump && rt::g_view.page_flags != nullptr &&
           rt::access_allowed(va, static_cast<unsigned>(size), rt::kPageRead);
}

// The address an EXPR names; false with *bad = the pointer that cannot be read.
bool eval_expr(const HookDump& d, const CpuState& entry, uint64_t ret, uint64_t* out, uint64_t* bad) {
    uint64_t base = d.reg == HookDump::kRet  ? ret
                    : d.reg == HookDump::kSp ? entry.sp
                    : d.reg == HookDump::kTp ? entry.tpidr_el0
                                             : entry.x[d.reg];
    for (const HookDump::Step& step : d.steps) {
        base += step.offset;
        if (!step.load)
            continue;
        if (!readable(base, 8)) {
            *bad = base;
            return false;
        }
        base = rt::load<uint64_t>(base);
    }
    *out = base;
    return true;
}

bool parse_value(std::string_view s, HookAct::Value* v) {
    if (!s.empty() && s[0] == '-') {
        uint64_t n = 0;
        if (!parse_number(s.substr(1), &n))
            return false;
        v->number = 0 - n;
        return true;
    }
    if (!s.empty() && s[0] >= '0' && s[0] <= '9')
        return parse_number(s, &v->number);
    v->constant = false;
    return parse_expr(s, &v->expr);
}

bool parse_act(std::string_view item, HookAct* a) {
    const size_t c1 = item.find(':');
    if (c1 == std::string_view::npos)
        return false;
    std::string_view door = item.substr(0, c1);
    std::string_view rest = item.substr(c1 + 1);
    if (rest.size() >= 6 && rest.substr(rest.size() - 6) == ":after") {
        a->after = true;
        rest.remove_suffix(6);
    }
    if (const size_t hash = door.find('#'); hash != std::string_view::npos) {
        std::string_view sel = door.substr(hash + 1);
        door = door.substr(0, hash);
        const size_t dash = sel.find('-');
        uint64_t first = 0;
        uint64_t last = 0;
        if (!parse_number(sel.substr(0, dash), &first) || first == 0 || first > UINT32_MAX)
            return false;
        a->first = static_cast<uint32_t>(first);
        if (dash == std::string_view::npos)
            a->last = a->first;
        else if (dash + 1 < sel.size()) {
            if (!parse_number(sel.substr(dash + 1), &last) || last < first || last > UINT32_MAX)
                return false;
            a->last = static_cast<uint32_t>(last);
        }
    }
    if (!parse_number(door, &a->va))
        return false;
    const size_t c2 = rest.find(':');
    if (c2 == std::string_view::npos)
        return false;
    const std::string_view kind = rest.substr(0, c2);
    std::string_view body = rest.substr(c2 + 1);
    if (kind == "ret") {
        a->kind = HookAct::kRet;
        return !a->after && parse_value(body, &a->value);
    }
    if (kind == "retf") {  // a float in s0 (a probability, Fang-6)
        a->kind = HookAct::kRetFloat;
        const std::string text(body);
        char* end = nullptr;
        a->real = std::strtof(text.c_str(), &end);
        return !a->after && !text.empty() && end == text.c_str() + text.size();
    }
    if (kind == "set") {
        a->kind = HookAct::kSet;
        const size_t eq = body.find('=');
        if (eq == std::string_view::npos || !parse_expr(body.substr(0, eq), &a->target))
            return false;
        std::string_view value = body.substr(eq + 1);
        if (const size_t slash = value.find('/'); slash != std::string_view::npos) {
            uint64_t size = 0;
            if (!parse_number(value.substr(slash + 1), &size) || (size != 1 && size != 2 && size != 4 && size != 8))
                return false;
            a->size = static_cast<unsigned>(size);
            value = value.substr(0, slash);
        }
        return parse_value(value, &a->value);
    }
    if (kind != "call")
        return false;
    a->kind = HookAct::kCall;
    const size_t open = body.find('(');
    if (open == std::string_view::npos || body.back() != ')' || !parse_number(body.substr(0, open), &a->fn))
        return false;
    std::string_view args = body.substr(open + 1, body.size() - open - 2);
    while (!args.empty()) {
        const size_t semi = std::min(args.find(';'), args.size());
        const std::string_view arg = args.substr(0, semi);
        args.remove_prefix(std::min(semi + 1, args.size()));
        const size_t eq = arg.find('=');
        HookDump reg;
        HookAct::Value v;
        if (eq == std::string_view::npos || !parse_expr(arg.substr(0, eq), &reg) || reg.reg > 30 ||
            !reg.steps.empty() || !parse_value(arg.substr(eq + 1), &v))
            return false;
        a->args.emplace_back(reg.reg, std::move(v));
    }
    return true;
}

}  // namespace

bool parse_hook_dumps(std::string_view spec, std::vector<HookDump>* out, std::string* error) {
    while (!spec.empty()) {
        const size_t comma = std::min(spec.find(','), spec.size());
        const std::string_view item = spec.substr(0, comma);
        spec.remove_prefix(std::min(comma + 1, spec.size()));
        if (item.empty())
            continue;
        const size_t a = item.find(':');
        const size_t b = a == std::string_view::npos ? a : item.find(':', a + 1);
        const size_t c = b == std::string_view::npos ? b : item.find(':', b + 1);
        HookDump d;
        uint64_t length = 0;
        const std::string_view when = c == std::string_view::npos ? std::string_view() : item.substr(c + 1);
        d.after = when == "after";
        if (b == std::string_view::npos || !parse_number(item.substr(0, a), &d.va) ||
            !parse_expr(item.substr(a + 1, b - a - 1), &d) ||
            !parse_number(item.substr(b + 1, c == std::string_view::npos ? c : c - b - 1), &length) || length == 0 ||
            length > kMaxDump || (c != std::string_view::npos && !d.after) || (d.reg == HookDump::kRet && !d.after)) {
            *error = "expected VA:EXPR:LEN[:after] (EXPR like x1>0+0x10, ret only with :after, LEN 1 to 4096), got \"" +
                     std::string(item) + "\"";
            return false;
        }
        d.length = static_cast<uint32_t>(length);
        d.expr = std::string(item.substr(a + 1, b - a - 1));
        out->push_back(std::move(d));
    }
    return true;
}

std::vector<std::string> hook_dump_lines(const HookDump& d, const CpuState& entry, uint32_t call, uint64_t ret) {
    std::vector<std::string> lines;
    char buf[160];
    uint64_t base = 0;
    uint64_t bad = 0;
    const char* when = d.after ? " after" : "";
    if (!eval_expr(d, entry, ret, &base, &bad)) {
        std::snprintf(buf, sizeof buf, "hook dump: 0x%" PRIx64 " call %u%s %s: pointer at 0x%" PRIx64 " unreadable",
                      d.va, call, when, d.expr.c_str(), bad);
        lines.emplace_back(buf);
        return lines;
    }
    if (!readable(base, d.length)) {
        std::snprintf(buf, sizeof buf, "hook dump: 0x%" PRIx64 " call %u%s %s = 0x%" PRIx64 ": %u bytes unreadable",
                      d.va, call, when, d.expr.c_str(), base, d.length);
        lines.emplace_back(buf);
        return lines;
    }
    std::snprintf(buf, sizeof buf, "hook dump: 0x%" PRIx64 " call %u%s %s = 0x%" PRIx64 ", %u bytes", d.va, call, when,
                  d.expr.c_str(), base, d.length);
    lines.emplace_back(buf);
    for (uint32_t off = 0; off < d.length; off += 16) {
        int n = std::snprintf(buf, sizeof buf, "hook dump: 0x%" PRIx64 " call %u%s +%03x", d.va, call, when, off);
        for (uint32_t i = off; i < off + 16 && i < d.length; ++i)
            n += std::snprintf(buf + n, sizeof buf - n, " %02x", rt::load<uint8_t>(base + i));
        lines.emplace_back(buf);
    }
    return lines;
}

bool parse_hook_expr(std::string_view expr, HookDump* out) {
    *out = HookDump();
    out->expr = std::string(expr);
    return parse_expr(expr, out);
}

bool eval_hook_expr(const HookDump& expr, const CpuState& entry, uint64_t ret, uint64_t* out) {
    uint64_t bad = 0;
    return eval_expr(expr, entry, ret, out, &bad);
}

bool parse_hook_acts(std::string_view spec, std::vector<HookAct>* out, std::string* error) {
    while (!spec.empty()) {
        const size_t comma = std::min(spec.find(','), spec.size());
        const std::string_view item = spec.substr(0, comma);
        spec.remove_prefix(std::min(comma + 1, spec.size()));
        if (item.empty())
            continue;
        HookAct a;
        if (!parse_act(item, &a)) {
            *error = "expected DOOR[#K|#K-|#K-M]:call:FN(xN=V;...)|set:EXPR=V[/SIZE]|ret:V|retf:FLOAT[:after] (ret, retf not with "
                     ":after), got \"" +
                     std::string(item) + "\"";
            return false;
        }
        a.text = std::string(item.substr(item.find(':') + 1));
        out->push_back(std::move(a));
    }
    return true;
}

// Nothing here may have a destructor alive across call_guest (a guest fault
// unwinds with longjmp): the log lines are built in a stack buffer.
bool run_hook_acts(const std::vector<HookAct>& acts, bool after, CpuState* ctx, const CpuState& entry, uint32_t call,
                   uint64_t* ret, bool log) {
    char buf[200];
    for (const HookAct& a : acts) {
        if (a.after != after || call < a.first || call > a.last)
            continue;
        uint64_t bad = 0;
        auto value = [&](const HookAct::Value& v, uint64_t* out) {
            if (v.constant) {
                *out = v.number;
                return true;
            }
            return eval_expr(v.expr, entry, *ret, out, &bad);
        };
        uint64_t v = 0;
        uint64_t addr = 0;
        bool ok = true;
        switch (a.kind) {
        case HookAct::kRet:
            if ((ok = value(a.value, &v))) {
                ctx->x[0] = v;
                if (log)
                    rt::log(rt::LogLevel::Info, "hook act: 0x%" PRIx64 " call %u %s: x0 = 0x%" PRIx64 ", original skipped",
                            a.va, call, a.text.c_str(), v);
                return false;
            }
            break;
        case HookAct::kRetFloat: {
            uint32_t bits = 0;
            std::memcpy(&bits, &a.real, sizeof bits);
            ctx->vreg[0] = V128{bits, 0};
            if (log)
                rt::log(rt::LogLevel::Info, "hook act: 0x%" PRIx64 " call %u %s: s0 = %g, original skipped", a.va, call,
                        a.text.c_str(), static_cast<double>(a.real));
            return false;
        }
        case HookAct::kSet:
            if ((ok = value(a.value, &v) && eval_expr(a.target, entry, *ret, &addr, &bad))) {
                if (rt::g_view.page_flags == nullptr || !rt::access_allowed(addr, a.size, rt::kPageWrite)) {
                    std::snprintf(buf, sizeof buf, "hook act: 0x%" PRIx64 " call %u %s: 0x%" PRIx64 " not writable",
                                  a.va, call, a.text.c_str(), addr);
                    rt::log(rt::LogLevel::Warn, "%s", buf);
                    break;
                }
                switch (a.size) {
                case 1: rt::store<uint8_t>(addr, static_cast<uint8_t>(v)); break;
                case 2: rt::store<uint16_t>(addr, static_cast<uint16_t>(v)); break;
                case 4: rt::store<uint32_t>(addr, static_cast<uint32_t>(v)); break;
                default: rt::store<uint64_t>(addr, v); break;
                }
                if (log)
                    rt::log(rt::LogLevel::Info, "hook act: 0x%" PRIx64 " call %u %s: [0x%" PRIx64 "] = 0x%" PRIx64, a.va,
                            call, a.text.c_str(), addr, v);
            }
            break;
        case HookAct::kCall: {
            const CpuState saved = *ctx;
            if (after)  // the door's registers, not the ones the original returned with
                for (unsigned i = 0; i <= 8; ++i)
                    ctx->x[i] = entry.x[i];
            for (const auto& [reg, arg] : a.args) {
                if (!(ok = value(arg, &v)))
                    break;
                ctx->x[reg] = v;
            }
            if (!ok) {
                *ctx = saved;
                break;
            }
            const bool done = detail::call_guest(ctx, a.fn);
            const uint64_t x0 = ctx->x[0];
            const uint32_t halt = ctx->halt;
            const uint64_t halt_info = ctx->halt_info;
            *ctx = saved;
            if (!done) {
                ctx->halt = halt;
                ctx->halt_info = halt_info;
                rt::log(rt::LogLevel::Warn, "hook act: 0x%" PRIx64 " call %u %s: guest halted (%u, 0x%" PRIx64 ")",
                        a.va, call, a.text.c_str(), halt, halt_info);
                return false;
            }
            *ret = x0;
            if (log)
                rt::log(rt::LogLevel::Info, "hook act: 0x%" PRIx64 " call %u %s: x0 = 0x%" PRIx64, a.va, call,
                        a.text.c_str(), x0);
            break;
        }
        }
        if (!ok)
            rt::log(rt::LogLevel::Warn,
                    "hook act: 0x%" PRIx64 " call %u %s: pointer at 0x%" PRIx64 " unreadable, left out", a.va, call,
                    a.text.c_str(), bad);
    }
    return true;
}

}  // namespace lgp::mods
