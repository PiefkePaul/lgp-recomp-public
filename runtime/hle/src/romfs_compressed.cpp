// Compressed RomFS images: block index, random reads, writer (romfs.hpp).
#include <algorithm>
#include <atomic>
#include <cstring>
#include <list>
#include <thread>

#include "lgp/hle/romfs.hpp"

#if defined(LGP_HLE_ZSTD)
#include <zstd.h>
#endif

namespace lgp::hle {

namespace {

using Format = CompressedImageFormat;

uint32_t get_u32(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}
uint64_t get_u64(const uint8_t* p) {
    uint64_t v;
    std::memcpy(&v, p, 8);
    return v;
}
void put_u32(uint8_t* p, uint32_t v) { std::memcpy(p, &v, 4); }
void put_u64(uint8_t* p, uint64_t v) { std::memcpy(p, &v, 8); }

bool read_exact(platform::IFile& f, uint64_t offset, void* dst, uint64_t len) {
    if (offset > f.size() || len > f.size() - offset)
        return false;
    uint64_t got = 0;
    return f.read(offset, dst, len, &got) == platform::StorageError::Ok && got == len;
}

#if defined(LGP_HLE_ZSTD)

// Decompressed blocks kept for repeated and neighbouring reads (sequential
// streams, the RomFS tables the game walks again and again).
constexpr size_t kCachedBlocks = 32;

class CompressedSource final : public DataSource {
public:
    CompressedSource(std::unique_ptr<platform::IFile> file, uint32_t block_size, uint64_t image_size,
                     std::vector<uint64_t> index)
        : file_(std::move(file)), block_size_(block_size), size_(image_size), index_(std::move(index)),
          dctx_(ZSTD_createDCtx()) {}
    ~CompressedSource() override { ZSTD_freeDCtx(dctx_); }

    uint64_t size() const override { return size_; }

    bool read(uint64_t offset, void* dst, uint64_t len) override {
        if (offset > size_ || len > size_ - offset)
            return false;
        auto* out = static_cast<uint8_t*>(dst);
        std::lock_guard lock(mutex_);
        while (len > 0) {
            const uint64_t block = offset / block_size_;
            const uint64_t in_block = offset % block_size_;
            const uint64_t block_len = block_length(block);
            const uint64_t n = std::min(len, block_len - in_block);
            if (n == block_len) {
                // A whole block: straight into the caller's buffer, past the
                // cache (a large read would only push everything else out).
                const auto hit = find(block);
                if (hit != cache_.end())
                    std::memcpy(out, hit->data.data(), n);
                else if (!decode(block, out))
                    return false;
            } else {
                const Cached* c = get(block);
                if (c == nullptr)
                    return false;
                std::memcpy(out, c->data.data() + in_block, n);
            }
            out += n;
            offset += n;
            len -= n;
        }
        return true;
    }

private:
    struct Cached {
        uint64_t block;
        std::vector<uint8_t> data;
    };

    uint64_t block_length(uint64_t block) const {
        return std::min<uint64_t>(block_size_, size_ - block * block_size_);
    }

    std::list<Cached>::iterator find(uint64_t block) {
        return std::find_if(cache_.begin(), cache_.end(), [&](const Cached& c) { return c.block == block; });
    }

    // The block, most recently used first; nullptr if it cannot be read.
    const Cached* get(uint64_t block) {
        auto it = find(block);
        if (it != cache_.end()) {
            cache_.splice(cache_.begin(), cache_, it);
            return &cache_.front();
        }
        std::vector<uint8_t> data;
        if (cache_.size() >= kCachedBlocks) {
            data = std::move(cache_.back().data);  // reuse its memory
            cache_.pop_back();
        }
        data.resize(block_length(block));
        if (!decode(block, data.data()))
            return nullptr;
        cache_.push_front({block, std::move(data)});
        return &cache_.front();
    }

    bool decode(uint64_t block, uint8_t* out) {
        const uint64_t begin = index_[block], end = index_[block + 1];
        const uint64_t stored = end - begin;
        const uint64_t want = block_length(block);
        if (stored == want)  // stored raw
            return read_exact(*file_, begin, out, want);
        packed_.resize(stored);
        if (!read_exact(*file_, begin, packed_.data(), stored))
            return false;
        const size_t got = ZSTD_decompressDCtx(dctx_, out, want, packed_.data(), stored);
        return !ZSTD_isError(got) && got == want;
    }

    std::unique_ptr<platform::IFile> file_;
    const uint32_t block_size_;
    const uint64_t size_;
    const std::vector<uint64_t> index_;
    std::mutex mutex_;
    ZSTD_DCtx* dctx_;
    std::vector<uint8_t> packed_;
    std::list<Cached> cache_;
};

#endif  // LGP_HLE_ZSTD

}  // namespace

bool compressed_images_supported() {
#if defined(LGP_HLE_ZSTD)
    return true;
#else
    return false;
#endif
}

bool is_compressed_image(platform::IFile& file) {
    char magic[8];
    return read_exact(file, 0, magic, sizeof(magic)) && std::memcmp(magic, Format::kMagic, sizeof(magic)) == 0;
}

std::shared_ptr<DataSource> open_image_source(std::unique_ptr<platform::IFile> file, std::string* error) {
    auto fail = [error](const char* why) -> std::shared_ptr<DataSource> {
        if (error != nullptr)
            *error = why;
        return nullptr;
    };
    if (!file)
        return fail("no file");
    if (!is_compressed_image(*file))
        return std::make_shared<FileSource>(std::move(file));
#if !defined(LGP_HLE_ZSTD)
    return fail("compressed RomFS image, but this build has no zstd (LGP_ENABLE_ZSTD)");
#else
    uint8_t header[Format::kHeaderSize];
    uint8_t footer[Format::kFooterSize];
    const uint64_t file_size = file->size();
    if (file_size < Format::kHeaderSize + Format::kFooterSize || !read_exact(*file, 0, header, sizeof(header)) ||
        !read_exact(*file, file_size - sizeof(footer), footer, sizeof(footer)))
        return fail("compressed RomFS image: truncated");
    if (std::memcmp(footer + 8, Format::kEndMagic, 8) != 0)
        return fail("compressed RomFS image: no end marker (truncated or still being written)");
    if (get_u32(header + 8) != Format::kVersion)
        return fail("compressed RomFS image: unknown version");
    if (get_u32(header + 12) != Format::kCodecZstd)
        return fail("compressed RomFS image: unknown codec");
    const uint32_t block_size = get_u32(header + 16);
    const uint32_t blocks = get_u32(header + 20);
    const uint64_t image_size = get_u64(header + 24);
    const uint64_t index_offset = get_u64(footer);
    if (block_size < Format::kMinBlockSize || block_size > Format::kMaxBlockSize)
        return fail("compressed RomFS image: bad block size");
    // Rounded up without image_size + block_size - 1, which wraps for sizes
    // near 2^64 and would let a header with no blocks pass.
    if (blocks != image_size / block_size + (image_size % block_size != 0 ? 1 : 0))
        return fail("compressed RomFS image: block count does not match the size");
    const uint64_t index_bytes = (uint64_t(blocks) + 1) * 8;
    if (index_offset < Format::kHeaderSize || index_offset > file_size - Format::kFooterSize ||
        file_size - Format::kFooterSize - index_offset != index_bytes)
        return fail("compressed RomFS image: bad index offset");
    std::vector<uint8_t> raw(index_bytes);
    if (!read_exact(*file, index_offset, raw.data(), raw.size()))
        return fail("compressed RomFS image: index unreadable");
    std::vector<uint64_t> index(uint64_t(blocks) + 1);
    for (size_t i = 0; i < index.size(); ++i)
        index[i] = get_u64(raw.data() + i * 8);
    if (index.front() != Format::kHeaderSize || index.back() != index_offset)
        return fail("compressed RomFS image: index does not cover the blocks");
    for (uint64_t b = 0; b < blocks; ++b) {
        const uint64_t want = std::min<uint64_t>(block_size, image_size - b * block_size);
        if (index[b + 1] <= index[b] || index[b + 1] - index[b] > want)
            return fail("compressed RomFS image: bad block in the index");
    }
    return std::make_shared<CompressedSource>(std::move(file), block_size, image_size, std::move(index));
#endif
}

bool write_compressed_image(DataSource& image, const CompressOptions& options,
                            const std::function<bool(const void* data, size_t len)>& write, std::string* error,
                            const std::function<void(uint64_t done)>& progress) {
    auto fail = [error](const std::string& why) {
        if (error != nullptr)
            *error = why;
        return false;
    };
#if !defined(LGP_HLE_ZSTD)
    (void)image, (void)options, (void)write, (void)progress;
    return fail("this build has no zstd (LGP_ENABLE_ZSTD)");
#else
    const uint32_t block_size = options.block_size;
    if (block_size < Format::kMinBlockSize || block_size > Format::kMaxBlockSize)
        return fail("block size out of range");
    const uint64_t size = image.size();
    const uint64_t blocks64 = (size + block_size - 1) / block_size;
    if (blocks64 > 0xFFFFFFFFu)
        return fail("image too large for this block size");
    const uint32_t blocks = uint32_t(blocks64);
    const int level = std::clamp(options.level, 1, ZSTD_maxCLevel());
    unsigned threads = options.threads != 0 ? options.threads : std::thread::hardware_concurrency();
    threads = std::clamp(threads, 1u, 64u);

    uint8_t header[Format::kHeaderSize] = {};
    std::memcpy(header, Format::kMagic, 8);
    put_u32(header + 8, Format::kVersion);
    put_u32(header + 12, Format::kCodecZstd);
    put_u32(header + 16, block_size);
    put_u32(header + 20, blocks);
    put_u64(header + 24, size);
    if (!write(header, sizeof(header)))
        return fail("write failed");

    std::vector<uint64_t> index;
    index.reserve(size_t(blocks) + 1);
    uint64_t at = Format::kHeaderSize;
    index.push_back(at);

    // Batches of blocks: read in order, compressed side by side, written in order.
    const size_t batch = size_t(threads) * 4;
    std::vector<std::vector<uint8_t>> plain(batch), packed(batch);
    std::vector<size_t> packed_len(batch);
    for (uint32_t first = 0; first < blocks; first += uint32_t(batch)) {
        const size_t count = std::min<size_t>(batch, blocks - first);
        for (size_t i = 0; i < count; ++i) {
            const uint64_t off = uint64_t(first + i) * block_size;
            plain[i].resize(size_t(std::min<uint64_t>(block_size, size - off)));
            if (!image.read(off, plain[i].data(), plain[i].size()))
                return fail("cannot read the image at " + std::to_string(off));
        }
        std::atomic<size_t> next{0};
        std::atomic<bool> failed{false};
        auto work = [&] {
            ZSTD_CCtx* cctx = ZSTD_createCCtx();
            for (size_t i; (i = next.fetch_add(1)) < count;) {
                packed[i].resize(ZSTD_compressBound(plain[i].size()));
                const size_t n = ZSTD_compressCCtx(cctx, packed[i].data(), packed[i].size(), plain[i].data(),
                                                   plain[i].size(), level);
                if (ZSTD_isError(n))
                    failed = true;
                packed_len[i] = n;
            }
            ZSTD_freeCCtx(cctx);
        };
        std::vector<std::thread> pool;
        const unsigned extra = unsigned(std::min<size_t>(threads, count)) - 1;
        for (unsigned t = 0; t < extra; ++t)
            pool.emplace_back(work);
        work();
        for (std::thread& t : pool)
            t.join();
        if (failed)
            return fail("zstd failed");
        for (size_t i = 0; i < count; ++i) {
            // Not smaller: stored raw (the reader tells by the length).
            const bool raw = packed_len[i] >= plain[i].size();
            const void* data = raw ? plain[i].data() : packed[i].data();
            const size_t len = raw ? plain[i].size() : packed_len[i];
            if (!write(data, len))
                return fail("write failed");
            at += len;
            index.push_back(at);
        }
        if (progress)
            progress(std::min<uint64_t>(uint64_t(first + count) * block_size, size));
    }

    std::vector<uint8_t> tail(index.size() * 8 + Format::kFooterSize);
    for (size_t i = 0; i < index.size(); ++i)
        put_u64(tail.data() + i * 8, index[i]);
    put_u64(tail.data() + index.size() * 8, at);  // the index starts where the blocks end
    std::memcpy(tail.data() + index.size() * 8 + 8, Format::kEndMagic, 8);
    if (!write(tail.data(), tail.size()))
        return fail("write failed");
    return true;
#endif
}

}  // namespace lgp::hle
