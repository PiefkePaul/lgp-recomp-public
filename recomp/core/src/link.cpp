// Link step: deterministic bases, rtld-style global symbol lookup, relocation application.
#include <algorithm>
#include <cctype>
#include <cstring>
#include <unordered_map>

#include "lgp/recomp/link.hpp"

namespace lgp::recomp {

int load_order_rank(const std::string& name) {
    std::string n = name;
    auto dot = n.find('.');
    if (dot != std::string::npos) n = n.substr(0, dot);
    if (n == "rtld") return 0;
    if (n == "main") return 1;
    if (n.rfind("subsdk", 0) == 0 && n.size() == 7 && std::isdigit(static_cast<unsigned char>(n[6])))
        return 2 + (n[6] - '0');
    if (n == "sdk") return 12;
    return 100;
}

namespace {

uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }

struct Resolver {
    const std::vector<Module>& mods;
    // name -> (module index, dynsym index) of the first defined global/weak symbol in load order.
    std::unordered_map<std::string, std::pair<uint32_t, uint32_t>> table;

    explicit Resolver(const std::vector<Module>& m) : mods(m) {
        for (uint32_t mi = 0; mi < mods.size(); ++mi) {
            const auto& syms = mods[mi].dynsym;
            for (uint32_t si = 0; si < syms.size(); ++si) {
                const Symbol& s = syms[si];
                if (!s.defined() || s.name.empty() || s.bind == kStbLocal) continue;
                auto it = table.find(s.name);
                // rtld takes the first match in load order; a global beats an earlier weak.
                if (it == table.end()) table.emplace(s.name, std::make_pair(mi, si));
                else if (mods[it->second.first].dynsym[it->second.second].bind == kStbWeak && s.bind == kStbGlobal)
                    it->second = {mi, si};
            }
        }
    }
};

}  // namespace

LinkedImage link(std::vector<Module> modules, const LinkOptions& opt) {
    LinkedImage img;
    if (opt.sort_load_order) {
        std::stable_sort(modules.begin(), modules.end(), [](const Module& a, const Module& b) {
            return load_order_rank(a.name) < load_order_rank(b.name);
        });
    }
    uint64_t base = opt.first_base;
    for (auto& m : modules) {
        m.base = base;
        base = align_up(base + m.image.size(), opt.page_size);
    }
    img.modules = std::move(modules);
    Resolver res(img.modules);

    for (uint32_t mi = 0; mi < img.modules.size(); ++mi) {
        Module& m = img.modules[mi];
        // Import table: every undefined dynsym entry.
        std::unordered_map<uint32_t, size_t> import_index;
        for (uint32_t si = 0; si < m.dynsym.size(); ++si) {
            const Symbol& s = m.dynsym[si];
            if (s.defined() || s.name.empty()) continue;
            Import imp;
            imp.name = s.name;
            imp.from_module = mi;
            imp.sym_index = si;
            imp.weak = s.bind == kStbWeak;
            auto it = res.table.find(s.name);
            if (it != res.table.end()) {
                const Module& prov = img.modules[it->second.first];
                imp.to_module = static_cast<int32_t>(it->second.first);
                imp.target_va = prov.base + prov.dynsym[it->second.second].value;
                ++img.stats.imports_resolved;
            } else {
                if (imp.weak) ++img.stats.imports_unresolved_weak;
                else {
                    ++img.stats.imports_unresolved_strong;
                    img.diagnostics.push_back({LinkDiagnostic::Kind::UnresolvedSymbol, m.name,
                                               "unresolved strong symbol " + s.name});
                    if (opt.strict) throw FormatError(m.name + ": unresolved symbol " + s.name);
                }
            }
            import_index[si] = img.imports.size();
            img.imports.push_back(std::move(imp));
        }
        for (const auto& need : m.needed) {
            bool found = false;
            auto stem = [](std::string n) { auto d = n.find('.'); return d == std::string::npos ? n : n.substr(0, d); };
            for (const auto& other : img.modules)
                if (stem(other.internal_name) == stem(need) || stem(other.name) == stem(need)) found = true;
            if (!found)
                img.diagnostics.push_back({LinkDiagnostic::Kind::MissingNeeded, m.name, "DT_NEEDED " + need + " not loaded"});
        }

        // Symbol value S for relocation `r` (0 if unresolved).
        auto sym_value = [&](const Relocation& r, uint64_t& out) -> bool {
            if (r.sym == 0) { out = 0; return true; }
            if (r.sym >= m.dynsym.size()) return false;
            const Symbol& s = m.dynsym[r.sym];
            if (s.defined()) { out = m.base + s.value; return true; }
            auto it = import_index.find(r.sym);
            if (it == import_index.end()) return false;
            Import& imp = img.imports[it->second];
            if (imp.slot_va == 0) imp.slot_va = m.base + r.offset;
            out = imp.target_va;
            return true;
        };

        for (auto& r : m.relocations) {
            if (r.offset + 8 > m.image.size()) {
                img.diagnostics.push_back({LinkDiagnostic::Kind::RelocationOutOfRange, m.name,
                                           "relocation slot outside image"});
                if (opt.strict) throw FormatError(m.name + ": relocation slot outside image");
                continue;
            }
            uint64_t S = 0;
            switch (r.type) {
                case kRAarch64Relative:
                    if (r.from_relr) m.write_u64(r.offset, m.base + m.read_u64(r.offset));
                    else m.write_u64(r.offset, m.base + static_cast<uint64_t>(r.addend));
                    r.applied = true;
                    break;
                case kRAarch64Abs64:
                case kRAarch64GlobDat:
                case kRAarch64JumpSlot:
                    if (!sym_value(r, S)) {
                        img.diagnostics.push_back({LinkDiagnostic::Kind::RelocationOutOfRange, m.name,
                                                   "relocation references invalid symbol index"});
                        break;
                    }
                    m.write_u64(r.offset, S + static_cast<uint64_t>(r.addend));
                    r.applied = true;
                    break;
                default:
                    ++img.stats.relocations_unsupported;
                    img.diagnostics.push_back({LinkDiagnostic::Kind::UnsupportedRelocation, m.name,
                                               std::string("unsupported relocation type ") +
                                                   relocation_type_name(r.type) + " (" + std::to_string(r.type) + ")"});
                    if (opt.strict) throw FormatError(m.name + ": unsupported relocation type " + std::to_string(r.type));
                    break;
            }
            if (r.applied) ++img.stats.relocations_applied;
        }
        // init/fini arrays hold absolute pointers after relocation.
        auto read_array = [&](uint64_t off, uint64_t size, std::vector<uint64_t>& out) {
            for (uint64_t k = 0; k + 8 <= size && off + k + 8 <= m.image.size(); k += 8) {
                uint64_t v = m.read_u64(off + k);
                if (v >= m.base && v < m.base + m.image.size()) out.push_back(v - m.base);
            }
        };
        m.init_array.clear();
        m.fini_array.clear();
        read_array(m.init_array_offset, m.init_array_size, m.init_array);
        read_array(m.fini_array_offset, m.fini_array_size, m.fini_array);
    }
    return img;
}

// ---- LinkedImage queries -----------------------------------------------------

const Module* LinkedImage::module_at(uint64_t va) const {
    for (const auto& m : modules)
        if (va >= m.base && va < m.base + m.image.size()) return &m;
    return nullptr;
}

std::optional<std::pair<const Module*, uint64_t>> LinkedImage::locate(uint64_t va) const {
    const Module* m = module_at(va);
    if (!m) return std::nullopt;
    return std::make_pair(m, va - m->base);
}

const Segment* LinkedImage::segment_at(uint64_t va) const {
    auto l = locate(va);
    return l ? l->first->segment_at_offset(l->second) : nullptr;
}

bool LinkedImage::in_text(uint64_t va) const {
    const Segment* s = segment_at(va);
    return s && s->kind == SegmentKind::Text;
}

uint32_t LinkedImage::read_u32(uint64_t va) const {
    auto l = locate(va);
    if (!l) throw std::out_of_range("LinkedImage::read_u32: unmapped");
    return l->first->read_u32(l->second);
}
uint64_t LinkedImage::read_u64(uint64_t va) const {
    auto l = locate(va);
    if (!l) throw std::out_of_range("LinkedImage::read_u64: unmapped");
    return l->first->read_u64(l->second);
}
uint8_t LinkedImage::read_u8(uint64_t va) const {
    auto l = locate(va);
    if (!l) throw std::out_of_range("LinkedImage::read_u8: unmapped");
    return l->first->image[l->second];
}

const Symbol* LinkedImage::symbol_at(uint64_t va, const Module** owner) const {
    const Symbol* best = nullptr;
    const Module* best_mod = nullptr;
    for (const auto& m : modules) {
        if (va < m.base || va >= m.base + m.image.size()) continue;
        for (const auto& s : m.dynsym) {
            if (!s.defined() || s.name.empty() || m.base + s.value != va) continue;
            if (!best || (best->bind != kStbGlobal && s.bind == kStbGlobal) ||
                (best->bind == kStbLocal && s.bind == kStbWeak)) {
                best = &s;
                best_mod = &m;
            }
        }
    }
    if (owner) *owner = best_mod;
    return best;
}

std::optional<uint64_t> LinkedImage::symbol_va(const std::string& name) const {
    for (const auto& m : modules)
        for (const auto& s : m.dynsym)
            if (s.defined() && s.name == name) return m.base + s.value;
    return std::nullopt;
}

const Import* LinkedImage::import_by_slot(uint64_t slot_va) const {
    for (const auto& i : imports)
        if (i.slot_va == slot_va) return &i;
    return nullptr;
}

uint64_t LinkedImage::end_va() const {
    uint64_t e = 0;
    for (const auto& m : modules) e = std::max(e, m.base + m.image.size());
    return e;
}

}  // namespace lgp::recomp
