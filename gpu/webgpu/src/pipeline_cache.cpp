#include "lgp/gpu/webgpu/pipeline_cache.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <system_error>
#include <vector>

#include "lgp/runtime/log.hpp"

namespace lgp::gpu::webgpu {

namespace fs = std::filesystem;

namespace {

constexpr uint64_t kMaxFileBytes = kPipelineCacheFileLimit;
// A seed is read where it lies and not copied (import_records); below 2 GiB
// so a long holds its offsets on Windows too. A whole catalog is ~185 MiB.
constexpr uint64_t kMaxSeedBytes = 1ull << 30;

constexpr uint32_t kProgramRecord = 0x8000'0000u;

std::string encode(uint64_t hash, std::string_view recipe, bool program) {
    std::string out(12 + recipe.size(), '\0');
    const uint32_t len = uint32_t(recipe.size()) | (program ? kProgramRecord : 0);
    std::memcpy(out.data(), &hash, 8);
    std::memcpy(out.data() + 8, &len, 4);
    std::memcpy(out.data() + 12, recipe.data(), recipe.size());
    return out;
}

std::string header(const std::string& fingerprint) {
    std::string out(kPipelineCacheMagic, sizeof(kPipelineCacheMagic));
    const uint32_t len = uint32_t(fingerprint.size());
    out.append(reinterpret_cast<const char*>(&len), 4);
    return out + fingerprint;
}

std::string read_file(const std::string& path) {
    std::string bytes;
    if (FILE* f = std::fopen(path.c_str(), "rb")) {
        // On the heap: Emscripten's pthread stacks are 64 KiB.
        std::vector<char> buf(65536);
        for (size_t n; (n = std::fread(buf.data(), 1, buf.size(), f)) > 0 && bytes.size() <= kMaxFileBytes;)
            bytes.append(buf.data(), n);
        std::fclose(f);
    }
    return bytes;
}

// Calls fn(hash, program, contents, 12 header bytes) for each whole record
// from `at` on; returns the offset after the last one.
template <class Fn>
size_t for_each_record(const std::string& bytes, size_t at, Fn&& fn) {
    while (at + 12 <= bytes.size()) {
        uint64_t hash;
        uint32_t word;
        std::memcpy(&hash, bytes.data() + at, 8);
        std::memcpy(&word, bytes.data() + at + 8, 4);
        const bool program = (word & kProgramRecord) != 0;
        const uint32_t len = word & ~kProgramRecord;
        if (len > bytes.size() - at - 12)
            break;
        fn(hash, program, bytes.substr(at + 12, len), std::string_view(bytes).substr(at, 12));
        at += 12 + len;
    }
    return at;
}

bool write_file(const std::string& path, const std::string& bytes) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr)
        return false;
    const bool ok = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    return std::fclose(f) == 0 && ok;
}

// Visits every ProgramDesc field the backend uses, in the pipeline cache's
// order: f.str(std::string&), f.words(std::vector<uint32_t>&),
// f.bits(std::vector<bool>&), then the flags.
template <typename Desc, typename F>
void visit_program_desc(Desc& d, F& f) {
    f.str(d.vertex_wgsl);
    f.str(d.fragment_wgsl);
    f.str(d.compute_wgsl);
    f.words(d.vertex_cbufs);
    f.words(d.fragment_cbufs);
    f.words(d.compute_cbufs);
    f.words(d.vertex_textures);
    f.words(d.fragment_textures);
    f.words(d.compute_textures);
    f.words(d.vertex_texture_dims);
    f.words(d.fragment_texture_dims);
    f.words(d.compute_texture_dims);
    f.bits(d.vertex_texture_arrays);
    f.bits(d.fragment_texture_arrays);
    f.bits(d.compute_texture_arrays);
    f.bits(d.vertex_texture_depth);
    f.bits(d.fragment_texture_depth);
    f.bits(d.compute_texture_depth);
    f.flag(d.vertex_global_memory);
    f.flag(d.fragment_global_memory);
    f.flag(d.compute_global_memory);
    f.flag(d.compute_gmem_probe);
    f.flag(d.texture_swizzle);
}

}  // namespace

uint64_t fnv1a64(std::string_view bytes, uint64_t seed) {
    uint64_t h = seed;
    for (unsigned char c : bytes) {
        h ^= c;
        h *= 0x100000001b3ull;
    }
    return h;
}

bool path_inside(const std::string& path, const std::string& dir) {
    std::error_code ec;
    const fs::path p = fs::weakly_canonical(fs::absolute(path, ec), ec);
    const fs::path d = fs::weakly_canonical(fs::absolute(dir, ec), ec);
    if (d.empty())
        return false;
    auto pi = p.begin();
    for (auto di = d.begin(); di != d.end(); ++di, ++pi) {
        if (di->empty())
            continue;  // trailing separator
        if (pi == p.end() || *pi != *di)
            return false;
    }
    return true;
}

bool PipelineCacheFile::open(const std::string& path, const std::string& fingerprint, std::string* error) {
    auto fail = [error](std::string why) {
        if (error != nullptr) *error = std::move(why);
        return false;
    };
    const std::string ext = kPipelineCacheExtension;
    if (path.size() <= ext.size() || path.compare(path.size() - ext.size(), ext.size(), ext) != 0)
        return fail("the file name must end in " + ext);
#if defined(LGP_SOURCE_DIR)
    // Derived from game data: never inside the repository (build trees in it included).
    if (path_inside(path, LGP_SOURCE_DIR))
        return fail(std::string("inside the source tree ") + LGP_SOURCE_DIR + "; use $LGP_PRIVATE_DIR");
#endif
    path_ = path;
    recipes_.clear();
    programs_.clear();
    program_order_.clear();
    recipe_positions_.clear();
    program_positions_.clear();
    next_position_ = 0;
    loaded_ = 0;
    std::error_code ec;
    if (const fs::path parent = fs::path(path).parent_path(); !parent.empty())
        fs::create_directories(parent, ec);

    // Read what is there.
    const std::string bytes = read_file(path);
    const std::string head = header(fingerprint);
    bool rewrite = true;
    size_t good = 0;
    if (bytes.size() <= kMaxFileBytes && bytes.compare(0, head.size(), head) == 0) {
        rewrite = false;
        good = for_each_record(bytes, head.size(), [&](uint64_t hash, bool program, std::string contents,
                                                      std::string_view raw_head) {
            std::lock_guard lock(mutex_);
            if (known_.insert(fnv1a64(contents, fnv1a64(raw_head))).second) {  // = fnv1a64 of the record
                remember(hash, program, std::move(contents));
                ++loaded_;
            }
        });
        rewrite = good != bytes.size();  // damaged tail: write the good part again
    } else if (bytes.size() > kMaxFileBytes) {
        rt::log(rt::LogLevel::Info, "nvn webgpu: pipeline cache %s is larger than %llu MiB; starting over",
                path.c_str(), static_cast<unsigned long long>(kMaxFileBytes >> 20));
    } else if (!bytes.empty()) {
        rt::log(rt::LogLevel::Info, "nvn webgpu: pipeline cache %s is from another GPU, driver or version; "
                "starting over", path.c_str());
    }
    if (rewrite && !write_file(path, good > 0 ? bytes.substr(0, good) : head))
        return fail("cannot write " + path);
    return true;
}

void PipelineCacheFile::remember(uint64_t hash, bool program, std::string contents) {
    if (!program) {
        recipes_[hash].push_back(std::move(contents));
        recipe_positions_[hash].push_back(next_position_++);
    } else if (programs_.emplace(hash, std::move(contents)).second) {
        program_order_.push_back(hash);
        program_positions_[hash] = next_position_++;
    }
}

PipelineCacheFile::~PipelineCacheFile() {
    if (seed_file_ != nullptr)
        std::fclose(seed_file_);
}

size_t PipelineCacheFile::import_records(const std::string& path, std::string* error) {
    auto fail = [error](std::string why) {
        if (error != nullptr) *error = std::move(why);
        return size_t(0);
    };
    if (path_.empty())
        return fail("no pipeline cache open");
    if (!seed_path_.empty())
        return fail("a seed is already imported");
    std::error_code ec;
    if (fs::is_directory(path, ec))
        return fail("cannot read " + path);
    const uint64_t file_size = fs::file_size(path, ec);
    if (ec)
        return fail("cannot read " + path);
    if (file_size > kMaxSeedBytes)
        return fail(path + " is too large");
    FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr)
        return fail("cannot read " + path);
    char magic[sizeof(kPipelineCacheMagic)];
    uint32_t fp_len = 0;
    if (std::fread(magic, 1, sizeof(magic), f) != sizeof(magic) || std::memcmp(magic, kPipelineCacheMagic, sizeof(magic)) != 0 ||
        std::fread(&fp_len, 4, 1, f) != 1) {
        std::fclose(f);
        return fail(path + " is no pipeline cache");
    }
    // The fingerprint does not matter here: a recipe this device cannot use
    // only costs one failed or unused pipeline creation.
    uint64_t at = sizeof(kPipelineCacheMagic) + 4 + uint64_t(fp_len);
    if (at > file_size || std::fseek(f, long(at), SEEK_SET) != 0) {
        std::fclose(f);
        return fail(path + " is damaged");
    }
    size_t imported = 0;
    std::vector<uint64_t> seed_order;  // seed programs in file order
    std::string contents;
    std::lock_guard lock(mutex_);
    for (char head[12]; at + 12 <= file_size && std::fread(head, 1, 12, f) == 12;) {
        uint64_t hash;
        uint32_t word;
        std::memcpy(&hash, head, 8);
        std::memcpy(&word, head + 8, 4);
        const bool program = (word & kProgramRecord) != 0;
        const uint32_t len = word & ~kProgramRecord;
        if (len > file_size - at - 12)
            break;  // damaged tail
        const long offset = long(at + 12);
        at += 12 + uint64_t(len);
        if (program) {
            // The text stays in the file; the hash is the text's (program_hash).
            if (!programs_.count(hash) && !seed_programs_.count(hash)) {
                // A graphics program's first field is its vertex WGSL (never
                // empty); a compute program's is empty.
                uint32_t vertex_len = 1;
                if (len < 4 || std::fread(&vertex_len, 4, 1, f) != 1)
                    vertex_len = 1;
                seed_programs_[hash] = {offset, len};
                program_positions_[hash] = next_position_++;
                if (vertex_len == 0)
                    program_order_.push_back(hash);
                else
                    seed_order.push_back(hash);
                ++loaded_;
                ++imported;
            }
            if (std::fseek(f, long(at), SEEK_SET) != 0)
                break;
            continue;
        }
        contents.resize(len);
        if (len > 0 && std::fread(contents.data(), 1, len, f) != len)
            break;
        if (!known_.insert(fnv1a64(contents, fnv1a64(std::string_view(head, 12)))).second)
            continue;
        remember(hash, false, contents);
        ++loaded_;
        ++imported;
    }
    // Graphics programs only with a recipe (the file's own or the seed's):
    // a catalog lists thousands that no run has drawn with yet.
    for (uint64_t hash : seed_order)
        if (recipes_.count(hash))
            program_order_.push_back(hash);
    std::sort(program_order_.begin(), program_order_.end(), [this](uint64_t x, uint64_t y) {
        return program_positions_.at(x) < program_positions_.at(y);
    });
    seed_path_ = path;
    seed_file_ = f;
    return imported;
}

size_t PipelineCacheFile::program_count() const {
    std::lock_guard lock(mutex_);
    size_t n = programs_.size();
    for (const auto& [hash, where] : seed_programs_) n += programs_.count(hash) == 0;
    return n;
}

bool PipelineCacheFile::program(uint64_t hash, std::string* out) {
    std::lock_guard lock(mutex_);
    if (released_)
        return false;
    if (auto it = programs_.find(hash); it != programs_.end()) {
        *out = it->second;
        return true;
    }
    auto it = seed_programs_.find(hash);
    if (it == seed_programs_.end() || seed_file_ == nullptr || std::fseek(seed_file_, it->second.offset, SEEK_SET) != 0)
        return false;
    out->resize(it->second.size);
    if (it->second.size > 0 && std::fread(out->data(), 1, it->second.size, seed_file_) != it->second.size) {
        rt::log(rt::LogLevel::Warn, "nvn webgpu: cannot read a program from pipeline cache seed %s", seed_path_.c_str());
        return false;
    }
    return true;
}

void PipelineCacheFile::add(uint64_t program_hash, std::string_view recipe) {
    add_record(program_hash, recipe, false);
}

void PipelineCacheFile::add_program(uint64_t program_hash, std::string_view program) {
    add_record(program_hash, program, true);
}

void PipelineCacheFile::add_record(uint64_t hash, std::string_view bytes, bool program) {
    if (bytes.size() >= kProgramRecord)
        return;
    std::string record = encode(hash, bytes, program);
    std::lock_guard lock(mutex_);
    if (path_.empty() || (program && seed_programs_.count(hash)) || !known_.insert(fnv1a64(record)).second)
        return;  // known here or in the seed
    pending_.push_back(std::move(record));
}

void PipelineCacheFile::flush() {
    std::vector<std::string> records;
    {
        std::lock_guard lock(mutex_);
        if (pending_.empty())
            return;
        records.swap(pending_);
    }
    std::string bytes;
    for (const std::string& r : records) bytes += r;
    std::error_code ec;
    const uintmax_t old_size = fs::file_size(path_, ec);
    const bool size_known = !ec;
    FILE* f = std::fopen(path_.c_str(), "ab");
    bool ok = f != nullptr && std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    if (f != nullptr) ok &= std::fclose(f) == 0;
    if (!ok) {
        rt::log(rt::LogLevel::Warn, "nvn webgpu: cannot append to pipeline cache %s", path_.c_str());
        // Cut off a partly written record (open() would drop everything after
        // it) and keep the records for the next flush.
        if (size_known)
            fs::resize_file(path_, old_size, ec);
        std::lock_guard lock(mutex_);
        records.insert(records.end(), std::make_move_iterator(pending_.begin()), std::make_move_iterator(pending_.end()));
        pending_ = std::move(records);
        return;
    }
    std::lock_guard lock(mutex_);
    written_ += records.size();
}

void PipelineCacheFile::release_programs() {
    std::lock_guard lock(mutex_);  // held() reads them from another thread
    std::unordered_map<uint64_t, std::string>().swap(programs_);
    std::vector<uint64_t>().swap(program_order_);
    if (seed_file_ != nullptr)
        std::fclose(seed_file_);
    seed_file_ = nullptr;
    released_ = true;
}

PipelineCacheFile::Held PipelineCacheFile::held() const {
    std::lock_guard lock(mutex_);
    Held h;
    for (const auto& [hash, text] : programs_) h.programs += text.capacity();
    for (const auto& [hash, list] : recipes_)
        for (const std::string& r : list) h.recipes += r.capacity();
    h.known = known_.size() * sizeof(uint64_t);
    for (const std::string& r : pending_) h.pending += r.capacity();
    return h;
}

size_t PipelineCacheFile::written() const {
    std::lock_guard lock(mutex_);
    return written_;
}

// A ProgramDesc as the pipeline cache stores it (warm-up); every field is
// length-prefixed, so equal bytes mean equal programs.
std::string encode_program_desc(const nvn::ProgramDesc& d) {
    struct {
        std::string out;
        void u32(uint32_t v) { out.append(reinterpret_cast<const char*>(&v), 4); }
        void str(const std::string& v) {
            u32(uint32_t(v.size()));
            out += v;
        }
        void words(const std::vector<uint32_t>& v) {
            u32(uint32_t(v.size()));
            for (uint32_t x : v) u32(x);
        }
        void bits(const std::vector<bool>& v) {
            u32(uint32_t(v.size()));
            for (bool x : v) out += char(x ? 1 : 0);
        }
        void flag(bool v) { out += char(v ? 1 : 0); }
    } e;
    visit_program_desc(d, e);
    return std::move(e.out);
}

// Pipeline cache: what a program and its draw pipelines depend on besides
// the request (its WGSL and binding layout).
uint64_t program_hash(const nvn::ProgramDesc& d) {
    return fnv1a64(encode_program_desc(d), fnv1a64("lgp program " + std::to_string(kPipelineRecipeVersion)));
}

bool decode_program_desc(std::string_view in, nvn::ProgramDesc* out) {
    struct {
        std::string_view in;
        size_t at = 0;
        bool ok = true;
        uint32_t u32() {
            uint32_t v = 0;
            if (at + 4 > in.size()) {
                ok = false;
                return 0;
            }
            std::memcpy(&v, in.data() + at, 4);
            at += 4;
            return v;
        }
        void str(std::string& v) {
            const uint32_t n = u32();
            if (!ok || n > in.size() - at) {
                ok = false;
                return;
            }
            v.assign(in.data() + at, n);
            at += n;
        }
        void words(std::vector<uint32_t>& v) {
            const uint32_t n = u32();
            if (!ok || n > (in.size() - at) / 4) {
                ok = false;
                return;
            }
            for (uint32_t i = 0; i < n; ++i) v.push_back(u32());
        }
        void bits(std::vector<bool>& v) {
            const uint32_t n = u32();
            if (!ok || n > in.size() - at) {
                ok = false;
                return;
            }
            for (uint32_t i = 0; i < n; ++i) v.push_back(in[at + i] != 0);
            at += n;
        }
        void flag(bool& v) {
            if (at >= in.size()) {
                ok = false;
                return;
            }
            v = in[at++] != 0;
        }
    } r{in};
    nvn::ProgramDesc d;
    visit_program_desc(d, r);
    if (!r.ok || r.at != in.size() || d.vertex_wgsl.empty() == d.compute_wgsl.empty())
        return false;  // exactly one of graphics and compute
    *out = std::move(d);
    return true;
}


bool read_pipeline_records(const std::string& path,
                           const std::function<void(uint64_t hash, bool program, std::string_view contents)>& fn,
                           std::string* fingerprint, std::string* error) {
    auto fail = [error](std::string why) {
        if (error != nullptr) *error = std::move(why);
        return false;
    };
    std::string bytes;
    {
        FILE* f = std::fopen(path.c_str(), "rb");
        if (f == nullptr)
            return fail("cannot read " + path);
        std::vector<char> buf(1 << 20);
        for (size_t n; (n = std::fread(buf.data(), 1, buf.size(), f)) > 0;) bytes.append(buf.data(), n);
        std::fclose(f);
    }
    if (bytes.size() < sizeof(kPipelineCacheMagic) + 4 ||
        bytes.compare(0, sizeof(kPipelineCacheMagic), kPipelineCacheMagic, sizeof(kPipelineCacheMagic)) != 0)
        return fail(path + " is no pipeline cache");
    uint32_t fp_len;
    std::memcpy(&fp_len, bytes.data() + sizeof(kPipelineCacheMagic), 4);
    const size_t first = sizeof(kPipelineCacheMagic) + 4 + size_t(fp_len);
    if (first > bytes.size())
        return fail(path + " is damaged");
    if (fingerprint != nullptr)
        fingerprint->assign(bytes, sizeof(kPipelineCacheMagic) + 4, fp_len);
    const size_t end = for_each_record(bytes, first, [&](uint64_t hash, bool program, std::string contents,
                                                         std::string_view) { fn(hash, program, contents); });
    if (end != bytes.size() && error != nullptr)
        *error = path + ": damaged record at byte " + std::to_string(end) + ", rest ignored";
    return true;
}

bool PipelineRecordWriter::open(const std::string& path, const std::string& fingerprint, std::string* error) {
    const std::string ext = kPipelineCacheExtension;
    if (path.size() <= ext.size() || path.compare(path.size() - ext.size(), ext.size(), ext) != 0) {
        if (error != nullptr) *error = "the file name must end in " + ext;
        return false;
    }
#if defined(LGP_SOURCE_DIR)
    if (path_inside(path, LGP_SOURCE_DIR)) {
        if (error != nullptr) *error = std::string("inside the source tree ") + LGP_SOURCE_DIR + "; use $LGP_PRIVATE_DIR";
        return false;
    }
#endif
    // Into a file beside it, renamed over `path` by close(): the output may
    // also be one of the inputs (merge --out seed seed other).
    const std::string temp = path + ".part";
    file_ = std::fopen(temp.c_str(), "wb");
    if (file_ == nullptr) {
        if (error != nullptr) *error = "cannot write " + temp;
        return false;
    }
    path_ = path;
    const std::string head = header(fingerprint);
    ok_ = std::fwrite(head.data(), 1, head.size(), file_) == head.size();
    bytes_ = head.size();
    return true;
}

bool PipelineRecordWriter::add(uint64_t hash, bool program, std::string_view contents) {
    if (file_ == nullptr || contents.size() >= kProgramRecord)
        return false;
    const std::string record = encode(hash, contents, program);
    ok_ &= std::fwrite(record.data(), 1, record.size(), file_) == record.size();
    bytes_ += record.size();
    return ok_;
}

bool PipelineRecordWriter::close(std::string* error) {
    if (file_ == nullptr)
        return ok_;
    ok_ &= std::fclose(file_) == 0;
    file_ = nullptr;
    const std::string temp = path_ + ".part";
    std::error_code ec;
    if (ok_) {
        fs::rename(temp, path_, ec);  // replaces an existing file, also on Windows
        ok_ = !ec;
    }
    if (!ok_) {
        fs::remove(temp, ec);
        if (error != nullptr)
            *error = "cannot write " + path_;
    }
    return ok_;
}

}  // namespace lgp::gpu::webgpu
