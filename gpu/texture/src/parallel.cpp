#include "lgp/gpu/texture/parallel.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

namespace lgp::gpu::texture {

struct DecodePool::Impl {
    std::mutex job_mutex;  // one job at a time
    std::mutex mutex;
    std::condition_variable start_cv, done_cv;
    std::vector<std::thread> threads;
    // The current job: its number (helpers wait for a new one) and the
    // helpers still inside it.
    uint64_t job = 0;
    uint32_t busy = 0;
    bool stop = false;
    const std::function<void(size_t, size_t)>* fn = nullptr;
    size_t n = 0, chunk = 1;
    std::atomic<size_t> next{0};

    void take_ranges() {
        for (;;) {
            const size_t begin = next.fetch_add(chunk);
            if (begin >= n)
                return;
            (*fn)(begin, std::min(n, begin + chunk));
        }
    }

    void helper() {
        uint64_t seen = 0;
        for (;;) {
            {
                std::unique_lock lock(mutex);
                start_cv.wait(lock, [&] { return stop || job != seen; });
                if (stop)
                    return;
                seen = job;
            }
            take_ranges();
            {
                std::lock_guard lock(mutex);
                --busy;
            }
            done_cv.notify_one();
        }
    }
};

DecodePool::DecodePool(uint32_t threads) : impl_(std::make_unique<Impl>()) {
    impl_->threads.reserve(threads);
    try {
        for (uint32_t i = 0; i < threads; ++i)
            impl_->threads.emplace_back([impl = impl_.get()] { impl->helper(); });
    } catch (...) {
        // No thread to be had: stop the ones started (joinable threads
        // must not be destroyed) and pass the error on.
        {
            std::lock_guard lock(impl_->mutex);
            impl_->stop = true;
        }
        impl_->start_cv.notify_all();
        for (std::thread& t : impl_->threads)
            t.join();
        throw;
    }
}

DecodePool::~DecodePool() {
    {
        std::lock_guard lock(impl_->mutex);
        impl_->stop = true;
    }
    impl_->start_cv.notify_all();
    for (std::thread& t : impl_->threads)
        t.join();
}

uint32_t DecodePool::threads() const { return uint32_t(impl_->threads.size()); }

void DecodePool::run(size_t n, size_t chunk, const std::function<void(size_t, size_t)>& fn) {
    chunk = std::max<size_t>(chunk, 1);
    if (impl_->threads.empty() || n <= chunk) {
        for (size_t begin = 0; begin < n; begin += chunk)
            fn(begin, std::min(n, begin + chunk));
        return;
    }
    Impl& p = *impl_;
    std::lock_guard job_lock(p.job_mutex);
    {
        std::lock_guard lock(p.mutex);
        p.fn = &fn;
        p.n = n;
        p.chunk = chunk;
        p.next.store(0);
        p.busy = uint32_t(p.threads.size());
        ++p.job;
    }
    p.start_cv.notify_all();
    p.take_ranges();
    std::unique_lock lock(p.mutex);
    p.done_cv.wait(lock, [&] { return p.busy == 0; });
    p.fn = nullptr;
}

void decode_bc_image(BcFormat format, const uint8_t* data, uint32_t width, uint32_t height, uint8_t* out,
                     DecodePool* pool) {
    const uint32_t by = (height + 3) / 4;
    // Ranges of about 64 KiB of blocks: small enough to share a large level,
    // large enough that small levels stay on the caller (DecodePool::run).
    const size_t row_bytes = size_t((width + 3) / 4) * bc_block_bytes(format);
    const size_t chunk = std::max<size_t>(1, (64 * 1024) / std::max<size_t>(row_bytes, 1));
    if (pool == nullptr || by <= chunk) {
        decode_bc_image(format, data, width, height, out);
        return;
    }
    pool->run(by, chunk, [&](size_t begin, size_t end) {
        const uint32_t y0 = uint32_t(begin) * 4;
        const uint32_t rows = std::min(height, uint32_t(end) * 4) - y0;
        decode_bc_image(format, data + begin * row_bytes, width, rows, out + size_t(y0) * width * 4);
    });
}

}  // namespace lgp::gpu::texture
