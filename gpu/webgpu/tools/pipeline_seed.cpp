// lgp_pipeline_seed: looks into, merges and fills pipeline seeds
// (.lgppipes, lgp/gpu/webgpu/pipeline_cache.hpp).
//
//   lgp_pipeline_seed info <file.lgppipes>...
//   lgp_pipeline_seed merge --out <new.lgppipes> <in.lgppipes>...
//   lgp_pipeline_seed catalog <stages dir> [--out <programs.lgppipes>] [--seed <file.lgppipes>]...
//                     [--programs <list.tsv>]... [--jobs N]   (default list: programs.tsv)
//
// `catalog` translates every program the game's shader containers define
// (programs.tsv from `tools/analyze/bnsh.py extract`) exactly as the NVN HLE
// does at nvnProgramSetShaders, so the program hashes match the ones a run
// records. With --seed it reports how much of the catalog runs have seen.
// Seeds, stages and the catalog are derived from game data: they stay in
// $LGP_PRIVATE_DIR (the writer refuses paths inside the source tree). The
// tool prints counts and sizes only.
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "lgp/gpu/nvn/abi.hpp"
#include "lgp/gpu/nvn/hle.hpp"
#include "lgp/gpu/webgpu/pipeline_cache.hpp"
#include "lgp/runtime/log.hpp"

namespace {

namespace nvn = lgp::gpu::nvn;
namespace webgpu = lgp::gpu::webgpu;

constexpr uint64_t kGameCacheLimit = webgpu::kPipelineCacheFileLimit;  // PipelineCacheFile starts over above it

int usage() {
    std::fprintf(stderr,
                 "usage: lgp_pipeline_seed info <file.lgppipes>...\n"
                 "       lgp_pipeline_seed merge --out <new.lgppipes> <in.lgppipes>...\n"
                 "       lgp_pipeline_seed catalog <stages dir> [--out <programs.lgppipes>] "
                 "[--seed <file.lgppipes>]... [--programs <list.tsv>]... [--jobs N]\n");
    return 2;
}

std::string mib(uint64_t bytes) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f MiB", double(bytes) / (1 << 20));
    return buf;
}

struct Contents {
    std::unordered_map<uint64_t, std::string> programs;           // by hash
    std::unordered_map<uint64_t, std::vector<std::string>> recipes;  // by program hash
    size_t records = 0, duplicates = 0;
    uint64_t program_bytes = 0, recipe_bytes = 0;
};

bool load(const std::string& path, Contents* c, std::string* fingerprint) {
    std::string error;
    std::set<std::pair<uint64_t, std::string>> seen_recipes;
    const bool ok = webgpu::read_pipeline_records(
        path,
        [&](uint64_t hash, bool program, std::string_view contents) {
            ++c->records;
            if (program) {
                if (!c->programs.emplace(hash, std::string(contents)).second) {
                    ++c->duplicates;
                    return;
                }
                c->program_bytes += contents.size();
            } else {
                if (!seen_recipes.emplace(hash, std::string(contents)).second) {
                    ++c->duplicates;
                    return;
                }
                c->recipes[hash].emplace_back(contents);
                c->recipe_bytes += contents.size();
            }
        },
        fingerprint, &error);
    if (!ok) {
        std::fprintf(stderr, "lgp_pipeline_seed: %s\n", error.c_str());
        return false;
    }
    if (!error.empty())
        std::fprintf(stderr, "lgp_pipeline_seed: warning: %s\n", error.c_str());
    return true;
}

size_t recipe_count(const Contents& c) {
    size_t n = 0;
    for (const auto& [hash, list] : c.recipes) n += list.size();
    return n;
}

int cmd_info(const std::vector<std::string>& files) {
    if (files.empty())
        return usage();
    for (const std::string& path : files) {
        Contents c;
        std::string fp;
        if (!load(path, &c, &fp))
            return 1;
        size_t graphics = 0, compute = 0, damaged = 0;
        for (const auto& [hash, text] : c.programs) {
            nvn::ProgramDesc d;
            if (!webgpu::decode_program_desc(text, &d))
                ++damaged;
            else
                ++(d.is_compute() ? compute : graphics);
        }
        size_t orphan = 0, programs_with_recipes = 0;
        for (const auto& [hash, list] : c.recipes) {
            if (c.programs.count(hash))
                ++programs_with_recipes;
            else
                orphan += list.size();
        }
        std::printf("%s\n", path.c_str());
        std::printf("  fingerprint: %s\n", fp.c_str());
        std::printf("  records: %zu (%zu duplicates)\n", c.records, c.duplicates);
        std::printf("  programs: %zu (%zu graphics, %zu compute, %zu undecodable), %s\n", c.programs.size(), graphics,
                    compute, damaged, mib(c.program_bytes).c_str());
        std::printf("  draw recipes: %zu for %zu programs, %s; %zu recipes without their program\n", recipe_count(c),
                    c.recipes.size(), mib(c.recipe_bytes).c_str(), orphan);
        std::printf("  programs with recipes: %zu of %zu\n", programs_with_recipes, c.programs.size());
    }
    return 0;
}

int cmd_merge(const std::string& out, const std::vector<std::string>& inputs) {
    if (out.empty() || inputs.empty())
        return usage();
    webgpu::PipelineRecordWriter w;
    std::string error;
    if (!w.open(out, "lgp seed (merged)", &error)) {
        std::fprintf(stderr, "lgp_pipeline_seed: %s\n", error.c_str());
        return 1;
    }
    // Programs first, in input order: the game warms them in file order.
    std::unordered_set<uint64_t> programs;
    std::set<std::pair<uint64_t, std::string>> recipes;
    std::vector<std::pair<uint64_t, std::string>> recipe_order;
    for (const std::string& path : inputs) {
        size_t new_programs = 0, new_recipes = 0, records = 0;
        const bool ok = webgpu::read_pipeline_records(
            path,
            [&](uint64_t hash, bool program, std::string_view contents) {
                ++records;
                if (program) {
                    if (programs.insert(hash).second && w.add(hash, true, contents))
                        ++new_programs;
                } else if (recipes.emplace(hash, std::string(contents)).second) {
                    recipe_order.emplace_back(hash, std::string(contents));
                    ++new_recipes;
                }
            },
            nullptr, &error);
        if (!ok) {
            std::fprintf(stderr, "lgp_pipeline_seed: %s\n", error.c_str());
            return 1;
        }
        std::printf("%s: %zu records, %zu new programs, %zu new recipes\n", path.c_str(), records, new_programs,
                    new_recipes);
    }
    for (const auto& [hash, recipe] : recipe_order) w.add(hash, false, recipe);
    if (!w.close(&error)) {
        std::fprintf(stderr, "lgp_pipeline_seed: %s\n", error.c_str());
        return 1;
    }
    std::printf("%s: %zu programs, %zu recipes, %s\n", out.c_str(), programs.size(), recipe_order.size(),
                mib(w.bytes()).c_str());
    if (w.bytes() > kGameCacheLimit)
        std::printf("warning: larger than the game's %llu MiB cache limit; the game imports only the first part\n",
                    static_cast<unsigned long long>(kGameCacheLimit >> 20));
    return 0;
}

// One line of programs.tsv.
struct CatalogEntry {
    bool compute = false;
    std::string vertex, pixel;  // stage ids; pixel "-" = vertex only
    nvn::ComputeInfo info;
    unsigned variations = 0;
};

bool read_stage(const std::string& dir, const std::string& id, const char* kind, std::vector<uint8_t>* out) {
    std::ifstream f(dir + "/" + id + "." + kind + ".bin", std::ios::binary);
    if (!f)
        return false;
    out->assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

// "vertex: 0x58: unknown opcode ..." -> "unknown opcode ...": grouped like
// the HLE's untranslated-program counter.
std::string reason_of(const std::string& error) {
    const size_t colon = error.rfind(": ");
    return colon == std::string::npos ? error : error.substr(colon + 2);
}

// Why seed programs are not in the catalog: the fragment WGSL does not depend
// on the vertex partner, so a seed program whose fragment stage the
// containers have but whose program the catalog lacks was linked differently
// at run time; one whose fragment stage they lack came from elsewhere (or
// from another translator version).
void explain_missing(const std::string& dir, const Contents& seed,
                     const std::unordered_map<uint64_t, size_t>& catalog, const std::vector<const std::string*>& records,
                     const std::vector<std::string>& pixel_stages, unsigned jobs) {
    std::unordered_set<std::string> vertex_wgsl, compute_wgsl, fragment_wgsl;
    for (const auto& [hash, i] : catalog) {
        nvn::ProgramDesc d;
        if (!webgpu::decode_program_desc(*records[i], &d))
            continue;
        if (d.is_compute())
            compute_wgsl.insert(d.compute_wgsl);
        else
            vertex_wgsl.insert(d.vertex_wgsl);
    }
    std::vector<std::string> fragments(pixel_stages.size());
    std::atomic<size_t> next{0};
    auto work = [&] {
        for (size_t i; (i = next.fetch_add(1)) < pixel_stages.size();) {
            std::vector<uint8_t> code;
            std::string error;
            if (read_stage(dir, pixel_stages[i], "pixel", &code))
                nvn::translate_maxwell_fragment(code, &fragments[i], &error);
        }
    };
    std::vector<std::thread> threads;
    for (unsigned t = 1; t < jobs; ++t) threads.emplace_back(work);
    work();
    for (std::thread& t : threads) t.join();
    for (std::string& f : fragments)
        if (!f.empty())
            fragment_wgsl.insert(std::move(f));

    std::map<std::string, size_t> why;
    for (const auto& [hash, text] : seed.programs) {
        if (catalog.count(hash))
            continue;
        nvn::ProgramDesc d;
        if (!webgpu::decode_program_desc(text, &d)) {
            ++why["undecodable record"];
        } else if (d.is_compute()) {
            ++why[compute_wgsl.count(d.compute_wgsl) ? "compute: WGSL known, other bindings"
                                                     : "compute: WGSL not in the containers"];
        } else if (d.fragment_wgsl.empty()) {
            ++why[vertex_wgsl.count(d.vertex_wgsl) ? "vertex only: vertex WGSL known (only paired in the containers)"
                                                   : "vertex only: vertex WGSL not in the catalog"];
        } else if (!fragment_wgsl.count(d.fragment_wgsl)) {
            ++why["graphics: fragment stage not in the containers"];
        } else if (vertex_wgsl.count(d.vertex_wgsl)) {
            ++why["graphics: both stages known, linked differently at run time"];
        } else {
            ++why["graphics: fragment known, vertex WGSL differs (other partner or unknown vertex stage)"];
        }
    }
    std::printf("pixel stages translated alone: %zu of %zu\n", fragment_wgsl.size(), pixel_stages.size());
    for (const auto& [reason, n] : why) std::printf("  seed program not in the catalog: %zu x %s\n", n, reason.c_str());
}

int cmd_catalog(const std::string& dir, const std::string& out, const std::vector<std::string>& seeds,
                unsigned jobs, const std::vector<std::string>& lists) {
    std::vector<CatalogEntry> entries;
    std::vector<std::string> pixel_stages;  // every pixel stage (explaining seed programs)
    for (const std::string& list : lists) {
        std::ifstream tsv(dir + "/" + list);
        if (!tsv) {
            std::fprintf(stderr, "lgp_pipeline_seed: no %s/%s (tools/analyze/bnsh.py extract writes it)\n",
                         dir.c_str(), list.c_str());
            return 1;
        }
        for (std::string line; std::getline(tsv, line);) {
            std::istringstream in(line);
            std::string kind;
            CatalogEntry e;
            in >> kind;
            if (kind == "graphics") {
                in >> e.vertex >> e.pixel >> e.variations;
            } else if (kind == "compute") {
                e.compute = true;
                in >> e.vertex >> e.info.workgroup[0] >> e.info.workgroup[1] >> e.info.workgroup[2] >>
                    e.info.shared_memory_bytes >> e.info.local_memory_bytes >> e.variations;
            } else if (kind == "pixel") {
                std::string id;
                if (in >> id)
                    pixel_stages.push_back(id);
                continue;
            } else {
                continue;
            }
            if (in.fail()) {
                std::fprintf(stderr, "lgp_pipeline_seed: bad line in programs.tsv: %s\n", line.c_str());
                return 1;
            }
            entries.push_back(std::move(e));
        }
    }

    struct Result {
        uint64_t hash = 0;
        std::string record;  // encoded ProgramDesc, empty = failed
        std::string error;
    };
    std::vector<Result> results(entries.size());
    std::atomic<size_t> next{0};
    const uint32_t base = nvn::kBoundTextureHandleBase;
    auto work = [&] {
        for (size_t i; (i = next.fetch_add(1)) < entries.size();) {
            const CatalogEntry& e = entries[i];
            Result& r = results[i];
            nvn::ProgramDesc desc;
            std::vector<uint8_t> vs, fs;
            bool ok;
            if (e.compute) {
                ok = read_stage(dir, e.vertex, "compute", &vs);
                if (!ok)
                    r.error = "stage file missing";
                else
                    ok = nvn::translate_maxwell_compute(vs, e.info, base, &desc, &r.error);
            } else {
                ok = read_stage(dir, e.vertex, "vertex", &vs) &&
                     (e.pixel == "-" || read_stage(dir, e.pixel, "pixel", &fs));
                if (!ok)
                    r.error = "stage file missing";
                else
                    ok = nvn::translate_maxwell_program(vs, fs, base, &desc, &r.error);
            }
            if (ok) {
                r.hash = webgpu::program_hash(desc);
                r.record = webgpu::encode_program_desc(desc);
            }
        }
    };
    std::vector<std::thread> threads;
    for (unsigned t = 1; t < jobs; ++t) threads.emplace_back(work);
    work();
    for (std::thread& t : threads) t.join();

    // Distinct programs (several BNSH lines can translate to the same program).
    std::unordered_map<uint64_t, size_t> catalog;  // hash -> first result index
    std::map<std::string, size_t> failures;
    size_t lines[2] = {}, failed[2] = {};
    uint64_t bytes = 0, largest = 0;
    for (size_t i = 0; i < entries.size(); ++i) {
        const bool compute = entries[i].compute;
        ++lines[compute];
        if (results[i].record.empty()) {
            ++failed[compute];
            ++failures[reason_of(results[i].error)];
            continue;
        }
        if (catalog.emplace(results[i].hash, i).second) {
            bytes += results[i].record.size();
            largest = std::max<uint64_t>(largest, results[i].record.size());
        }
    }
    size_t distinct[2] = {};
    for (const auto& [hash, i] : catalog) ++distinct[entries[i].compute];
    std::printf("program lists: %zu graphics, %zu compute lines\n", lines[0], lines[1]);
    std::printf("translated: graphics %zu of %zu, compute %zu of %zu\n", lines[0] - failed[0], lines[0],
                lines[1] - failed[1], lines[1]);
    std::printf("distinct programs: %zu graphics, %zu compute; records %s, largest %.1f KiB, mean %.1f KiB\n",
                distinct[0], distinct[1], mib(bytes).c_str(), largest / 1024.0,
                catalog.empty() ? 0.0 : bytes / 1024.0 / catalog.size());
    std::vector<std::pair<size_t, std::string>> by_count;
    for (const auto& [reason, n] : failures) by_count.emplace_back(n, reason);
    std::sort(by_count.rbegin(), by_count.rend());
    for (size_t k = 0; k < by_count.size() && k < 12; ++k)
        std::printf("  failed: %zu x %s\n", by_count[k].first, by_count[k].second.c_str());

    if (!seeds.empty()) {
        Contents seed;
        for (const std::string& path : seeds)
            if (!load(path, &seed, nullptr))
                return 1;
        size_t found = 0, found_compute = 0, seed_compute = 0;
        for (const auto& [hash, text] : seed.programs) {
            nvn::ProgramDesc d;
            const bool compute = webgpu::decode_program_desc(text, &d) && d.is_compute();
            seed_compute += compute;
            if (catalog.count(hash)) {
                ++found;
                found_compute += compute;
            }
        }
        size_t recipes_covered = 0, recipes_total = 0, seen = 0;
        for (const auto& [hash, list] : seed.recipes) {
            recipes_total += list.size();
            if (catalog.count(hash))
                recipes_covered += list.size();
        }
        for (const auto& [hash, i] : catalog)
            if (seed.programs.count(hash) || seed.recipes.count(hash))
                ++seen;
        std::printf("seed: %zu programs (%zu compute), %zu draw recipes\n", seed.programs.size(), seed_compute,
                    recipes_total);
        std::printf("seed programs in the catalog: %zu of %zu (compute %zu of %zu)\n", found, seed.programs.size(),
                    found_compute, seed_compute);
        std::printf("seed recipes whose program is in the catalog: %zu of %zu\n", recipes_covered, recipes_total);
        std::printf("catalog programs a run has seen: %zu of %zu\n", seen, catalog.size());
        std::vector<const std::string*> records;
        for (const Result& r : results) records.push_back(&r.record);
        explain_missing(dir, seed, catalog, records, pixel_stages, jobs);
    }

    if (!out.empty()) {
        webgpu::PipelineRecordWriter w;
        std::string error;
        if (!w.open(out, "lgp static catalog", &error)) {
            std::fprintf(stderr, "lgp_pipeline_seed: %s\n", error.c_str());
            return 1;
        }
        // programs.tsv order (sorted by stage id): stable between runs.
        for (size_t i = 0; i < entries.size(); ++i) {
            const Result& r = results[i];
            if (!r.record.empty() && catalog.at(r.hash) == i)
                w.add(r.hash, true, r.record);
        }
        if (!w.close(&error)) {
            std::fprintf(stderr, "lgp_pipeline_seed: %s\n", error.c_str());
            return 1;
        }
        std::printf("%s: %zu programs, %s\n", out.c_str(), catalog.size(), mib(w.bytes()).c_str());
        if (w.bytes() > kGameCacheLimit)
            std::printf("warning: larger than the game's %llu MiB cache limit\n",
                        static_cast<unsigned long long>(kGameCacheLimit >> 20));
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2)
        return usage();
    // The translator logs dispatcher-form stages at Info; only problems here.
    lgp::rt::set_log_level(lgp::rt::LogLevel::Warn);
    const std::string cmd = argv[1];
    std::string out;
    std::vector<std::string> seeds, positional, lists;
    unsigned jobs = std::max(1u, std::thread::hardware_concurrency());
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--out" && i + 1 < argc)
            out = argv[++i];
        else if (a == "--seed" && i + 1 < argc)
            seeds.push_back(argv[++i]);
        else if (a == "--programs" && i + 1 < argc)
            lists.push_back(argv[++i]);
        else if (a == "--jobs" && i + 1 < argc)
            jobs = unsigned(std::max(1, std::atoi(argv[++i])));
        else if (a.size() > 1 && a[0] == '-')
            return usage();
        else
            positional.push_back(a);
    }
    if (cmd == "info")
        return cmd_info(positional);
    if (cmd == "merge")
        return cmd_merge(out, positional);
    if (cmd == "catalog" && positional.size() == 1)
        return cmd_catalog(positional[0], out, seeds, jobs,
                           lists.empty() ? std::vector<std::string>{"programs.tsv"} : lists);
    return usage();
}
