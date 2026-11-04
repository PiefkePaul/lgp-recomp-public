// Draw pipeline recipes seen in earlier runs (WebGpuBackend::set_pipeline_cache).
//
// Creating a WebGPU render pipeline costs about 20 ms on the PC (Vulkan,
// shader compilation in the driver), and an effect seen for the first time
// needs several at once: the game stalls for hundreds of milliseconds
// (docs/knowledge/webgpu-backend-draw-kosten.md). The backend records which
// pipelines it created, as the request that led to them (render target
// formats, vertex layout, fixed-function state) and a hash of the program
// they belong to, and creates them ahead of use in the next run as soon as
// that program exists again.
//
// The file is derived from the game (program hashes are hashes of translated
// game shaders, the recipes are the game's render states): it stays local,
// below $LGP_PRIVATE_DIR or in the browser's OPFS. open() refuses paths
// without the .lgppipes extension and paths inside the source tree, and the
// pre-commit guard (tools/guard/check_game_data.py) rejects the extension and
// the file's magic.
//
// The programs themselves are kept too (their WGSL and binding layout, by
// the same hash), so they can be created ahead before the game asks for them.
//
// Layout: "LGPPIPE1", u32 fingerprint length, fingerprint bytes, then records
// (u64 program hash, u32 length, bytes; length bit 31 set: a program, else a
// recipe), little endian as the host writes it. A different fingerprint
// (other GPU, driver, browser or recipe format) or a damaged file starts over
// with an empty file; records written later go to its end. A seed
// (import_records) is read where it lies and never copied into this file.
#pragma once

#include <cstdint>
#include <cstdio>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "lgp/gpu/nvn/backend.hpp"

namespace lgp::gpu::webgpu {

inline constexpr char kPipelineCacheMagic[8] = {'L', 'G', 'P', 'P', 'I', 'P', 'E', '1'};
inline constexpr const char* kPipelineCacheExtension = ".lgppipes";

// FNV-1a, 64 bit; `seed` continues an earlier hash.
uint64_t fnv1a64(std::string_view bytes, uint64_t seed = 0xcbf29ce484222325ull);

// Recipe of a draw pipeline in the pipeline cache: its key without the
// program id (State::pipeline_request in webgpu_backend.cpp); the program is
// found by its hash. Bump when the key's layout changes.
inline constexpr uint32_t kPipelineRecipeVersion = 3;

// A program record: the ProgramDesc fields the backend uses (WGSL and binding
// layout), each length-prefixed, so equal bytes mean equal programs.
std::string encode_program_desc(const nvn::ProgramDesc& d);
// Reverses encode_program_desc; false for damaged bytes or a record that is
// not exactly one of graphics and compute.
bool decode_program_desc(std::string_view in, nvn::ProgramDesc* out);
// The hash programs and their recipes are filed under.
uint64_t program_hash(const nvn::ProgramDesc& d);

// Reads a whole pipeline cache file of any size and fingerprint (tools such
// as lgp_pipeline_seed; the game reads through PipelineCacheFile) and calls
// fn for every record in file order. False and *error if the file cannot be
// read or is no pipeline cache; a damaged tail ends the records early and
// *error names it while the result stays true.
bool read_pipeline_records(const std::string& path,
                           const std::function<void(uint64_t hash, bool program, std::string_view contents)>& fn,
                           std::string* fingerprint, std::string* error);
// A cache file the game appends to (PipelineCacheFile) starts over above
// this size: records of older shader versions pile up otherwise. One run of
// LGP on Android (OnePlus 7 Pro, 2026-10-28) wrote about 33 MiB, so the old
// 32 MiB made every following run start over. A seed may be larger.
inline constexpr uint64_t kPipelineCacheFileLimit = 64ull << 20;

// Writes records in the same layout into <path>.part, renamed over <path> by
// close() (so an input may be the output; no size limit, see
// kPipelineCacheFileLimit).
class PipelineRecordWriter {
public:
    bool open(const std::string& path, const std::string& fingerprint, std::string* error);
    bool add(uint64_t hash, bool program, std::string_view contents);
    bool close(std::string* error);
    uint64_t bytes() const { return bytes_; }

private:
    std::FILE* file_ = nullptr;
    std::string path_;
    uint64_t bytes_ = 0;
    bool ok_ = true;
};

// Whether `path` names a location inside `dir` (both made absolute and
// normalized, symbolic links resolved where they exist).
bool path_inside(const std::string& path, const std::string& dir);

class PipelineCacheFile {
public:
    PipelineCacheFile() = default;
    PipelineCacheFile(const PipelineCacheFile&) = delete;
    PipelineCacheFile& operator=(const PipelineCacheFile&) = delete;
    ~PipelineCacheFile();

    // Reads the records of a file written with the same fingerprint and
    // prepares appending; creates the file (and its directory) otherwise.
    // false and *error: the path is refused or cannot be written.
    bool open(const std::string& path, const std::string& fingerprint, std::string* error);

    // Takes the records of another cache file whatever its fingerprint (a
    // cache from another device or browser, or a catalog, as a seed). Call it
    // right after open(), once per run: the seed stays a file of its own that
    // is read again at every start (read-only, beside the game) and is not
    // copied into this file, so it may be larger than this file's limit.
    // Its recipes are held like loaded ones; its program texts stay in the
    // seed file until program() reads them. Seed programs without a recipe
    // are only listed in program_order() when they are compute programs: a
    // graphics program needs a recipe for a pipeline worth creating ahead.
    // Returns the number of new records; 0 and *error if the file cannot be
    // read or is no pipeline cache.
    size_t import_records(const std::string& path, std::string* error);

    // Recipes from the file by program hash (fixed after open() and import_records()).
    const std::unordered_map<uint64_t, std::vector<std::string>>& recipes() const { return recipes_; }
    // Programs to create ahead, by hash, in file order (the file's own, then
    // the seed's; fixed after import_records()).
    const std::vector<uint64_t>& program_order() const { return program_order_; }
    // Programs known from the file and the seed (also those not in program_order()).
    size_t program_count() const;
    // A program's text (encode_program_desc) into *out: from memory or read
    // from the seed file. false if unknown, after release_programs(), or if
    // the seed cannot be read. GPU thread (the thread that reads
    // program_order()).
    bool program(uint64_t hash, std::string* out);
    // Where a record stands among the loaded ones (0 = first; imported seed
    // records after the file's own): the order the game first needed them
    // in. Recipes by program hash, parallel to recipes().
    const std::unordered_map<uint64_t, std::vector<uint32_t>>& recipe_positions() const { return recipe_positions_; }
    const std::unordered_map<uint64_t, uint32_t>& program_positions() const { return program_positions_; }
    // Frees the program texts and closes the seed once the backend has
    // created all it wants from them (about 20 MiB in LGP); program_order()
    // is empty and program() fails afterwards. Call it on the thread that
    // reads program_order().
    void release_programs();
    size_t loaded() const { return loaded_; }  // records: recipes and programs

    // Remembers a new record (any thread; duplicates are ignored). flush()
    // writes what was added since.
    void add(uint64_t program_hash, std::string_view recipe);
    void add_program(uint64_t program_hash, std::string_view program);
    // Appends the pending records to the file; call it outside the GPU
    // thread (in the browser OPFS is only reachable from a pthread).
    void flush();
    size_t written() const;
    // Bytes of record text held in memory (programs, recipes, the set of
    // known records, records not yet written); LGP_WEBGPU_TEXTURE_REPORT.
    struct Held {
        size_t programs = 0, recipes = 0, known = 0, pending = 0;
    };
    Held held() const;

private:
    std::string path_;
    void add_record(uint64_t hash, std::string_view bytes, bool program);
    void remember(uint64_t hash, bool program, std::string contents);
    std::unordered_map<uint64_t, std::vector<std::string>> recipes_;
    std::unordered_map<uint64_t, std::string> programs_;  // the file's own
    std::vector<uint64_t> program_order_;
    // Seed programs: where their text stands in the seed file. Kept after
    // release_programs() so add_program() does not copy them into this file.
    struct SeedProgram {
        long offset = 0;
        uint32_t size = 0;
    };
    std::unordered_map<uint64_t, SeedProgram> seed_programs_;
    std::string seed_path_;
    std::FILE* seed_file_ = nullptr;  // open while program() may read it
    bool released_ = false;
    std::unordered_map<uint64_t, std::vector<uint32_t>> recipe_positions_;
    std::unordered_map<uint64_t, uint32_t> program_positions_;
    uint32_t next_position_ = 0;
    size_t loaded_ = 0;
    mutable std::mutex mutex_;
    // fnv1a64 of each record (header bytes + contents), not the records
    // themselves: a second copy of every WGSL text cost megabytes. A
    // collision only keeps one new record out of the file.
    std::unordered_set<uint64_t> known_;
    std::vector<std::string> pending_;       // encoded records
    size_t written_ = 0;
};

}  // namespace lgp::gpu::webgpu
