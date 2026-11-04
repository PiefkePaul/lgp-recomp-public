// Per-game analysis configuration (ADR-0004): addresses and names only, never code.
// Format: docs/knowledge/recompiler-config.md
#include <fstream>
#include <sstream>

#include "lgp/recomp/cfg.hpp"

namespace lgp::recomp {

namespace {

uint64_t parse_num(const std::string& s, const std::string& line) {
    try {
        size_t idx = 0;
        unsigned long long v = std::stoull(s, &idx, 0);
        if (idx != s.size()) throw std::invalid_argument("trailing");
        return v;
    } catch (const std::exception&) {
        throw FormatError("config: bad number '" + s + "' in line: " + line);
    }
}

}  // namespace

AnalysisConfig parse_config(const std::string& text) {
    AnalysisConfig cfg;
    std::istringstream in(text);
    std::string raw;
    while (std::getline(in, raw)) {
        auto hash = raw.find('#');
        std::string line = hash == std::string::npos ? raw : raw.substr(0, hash);
        std::istringstream ls(line);
        std::string kw;
        if (!(ls >> kw)) continue;
        std::vector<std::string> args;
        for (std::string a; ls >> a;) args.push_back(a);
        if (kw == "function") {
            for (const auto& a : args) cfg.extra_functions.push_back(parse_num(a, raw));
        } else if (kw == "noreturn") {
            for (const auto& a : args) {
                if (a.rfind("0x", 0) == 0 || a.rfind("0X", 0) == 0) cfg.noreturn_addresses.push_back(parse_num(a, raw));
                else cfg.noreturn_symbols.push_back(a);
            }
        } else if (kw == "jumptable") {
            JumpTableOverride o{};
            bool have_br = false, have_table = false, have_size = false, have_count = false, have_shift = false;
            for (const auto& a : args) {
                auto eq = a.find('=');
                std::string k = eq == std::string::npos ? a : a.substr(0, eq);
                std::string v = eq == std::string::npos ? "" : a.substr(eq + 1);
                if (k == "br") { o.br_pc = parse_num(v, raw); have_br = true; }
                else if (k == "table") { o.table_va = parse_num(v, raw); have_table = true; }
                else if (k == "entry") { o.entry_size = static_cast<uint8_t>(parse_num(v, raw)); have_size = true; }
                else if (k == "count") { o.count = static_cast<uint32_t>(parse_num(v, raw)); have_count = true; }
                else if (k == "base") o.base_va = parse_num(v, raw);
                else if (k == "shift") { o.shift = static_cast<uint8_t>(parse_num(v, raw)); have_shift = true; }
                else if (k == "signed") o.entries_signed = true;
                else if (k == "unsigned") o.entries_signed = false;
                else throw FormatError("config: unknown jumptable key '" + k + "' in line: " + raw);
            }
            if (!(have_br && have_table && have_size && have_count))
                throw FormatError("config: jumptable needs br=, table=, entry= and count=: " + raw);
            if (o.entry_size != 1 && o.entry_size != 2 && o.entry_size != 4 && o.entry_size != 8)
                throw FormatError("config: jumptable entry size must be 1, 2, 4 or 8: " + raw);
            if (!have_shift) o.shift = o.entry_size <= 2 ? 2 : 0;
            if (o.shift > 3) throw FormatError("config: jumptable shift must be 0..3: " + raw);
            cfg.jump_table_overrides.push_back(o);
        } else if (kw == "scan_gaps") {
            cfg.scan_gaps = args.empty() || args[0] == "on" || args[0] == "true" || args[0] == "1";
        } else if (kw == "data_pointers") {
            cfg.seed_data_pointers = args.empty() || args[0] == "on" || args[0] == "true" || args[0] == "1";
        } else {
            throw FormatError("config: unknown directive '" + kw + "' in line: " + raw);
        }
    }
    return cfg;
}

AnalysisConfig load_config_file(const std::string& path) {
    std::ifstream f(path);
    if (!f) throw FormatError("cannot open config " + path);
    std::stringstream ss;
    ss << f.rdbuf();
    return parse_config(ss.str());
}

AnalysisConfig rebase_config(AnalysisConfig cfg, uint64_t from, uint64_t to) {
    auto move = [&](uint64_t& va) { va = va - from + to; };
    for (uint64_t& va : cfg.extra_functions) move(va);
    for (uint64_t& va : cfg.noreturn_addresses) move(va);
    for (JumpTableOverride& j : cfg.jump_table_overrides) {
        move(j.br_pc);
        move(j.table_va);
        if (j.base_va != 0) move(j.base_va);
    }
    return cfg;
}

}  // namespace lgp::recomp
