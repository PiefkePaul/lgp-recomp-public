#include "lgp/gpu/nvn/async_backend.hpp"

#include <algorithm>
#include <chrono>
#include <utility>

namespace lgp::gpu::nvn {

namespace {

uint64_t now_ns() {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count());
}

// Fences nobody waited for are forgotten after this many.
constexpr size_t kMaxMarks = 1024;

}  // namespace

AsyncBackend::AsyncBackend(Backend& inner, const Options& options)
    : inner_(inner), options_(options), thread_([this] { worker(); }) {}

AsyncBackend::~AsyncBackend() {
    {
        std::unique_lock lock(mutex_);
        wait_done(lock, enqueued_);
        stop_ = true;
    }
    work_cv_.notify_all();
    thread_.join();
}

void AsyncBackend::worker() {
    for (;;) {
        Task task;
        {
            std::unique_lock lock(mutex_);
            work_cv_.wait(lock, [this] { return stop_ || !queue_.empty(); });
            if (queue_.empty())
                return;  // stop_ and nothing left
            task = std::move(queue_.front());
            queue_.pop_front();
        }
        const uint64_t t0 = now_ns();
        {
            std::lock_guard inner(inner_mutex_);
            task.run();
        }
        const uint64_t busy = now_ns() - t0;
        task.run = nullptr;  // the list it held goes before the callers wake
        {
            std::lock_guard lock(mutex_);
            ++done_;
            if (task.present)
                --presents_pending_;
            ++stats_.tasks;
            stats_.worker_busy_ns += busy;
        }
        done_cv_.notify_all();
    }
}

uint64_t AsyncBackend::enqueue(std::function<void()> task, bool is_present) {
    uint64_t seq = 0;
    {
        std::unique_lock lock(mutex_);
        if (is_present && presents_pending_ >= options_.max_presents_in_flight) {
            const uint64_t t0 = now_ns();
            done_cv_.wait(lock, [this] { return presents_pending_ < options_.max_presents_in_flight; });
            ++stats_.present_waits;
            stats_.present_wait_ns += now_ns() - t0;
        }
        if (queue_.size() >= options_.max_queued_tasks) {
            ++stats_.queue_full_waits;
            done_cv_.wait(lock, [this] { return queue_.size() < options_.max_queued_tasks; });
        }
        queue_.push_back({std::move(task), is_present});
        seq = ++enqueued_;
        if (is_present)
            ++presents_pending_;
        stats_.max_queued = std::max<uint64_t>(stats_.max_queued, queue_.size());
    }
    work_cv_.notify_one();
    return seq;
}

void AsyncBackend::wait_done(std::unique_lock<std::mutex>& lock, uint64_t seq) {
    if (done_ >= seq)
        return;
    const uint64_t t0 = now_ns();
    done_cv_.wait(lock, [this, seq] { return done_ >= seq; });
    ++stats_.fence_waits;
    stats_.fence_wait_ns += now_ns() - t0;
}

void AsyncBackend::drain() {
    std::unique_lock lock(mutex_);
    wait_done(lock, enqueued_);
}

bool AsyncBackend::drain_for(int64_t timeout_ms) {
    std::unique_lock lock(mutex_);
    const uint64_t seq = enqueued_;
    if (done_ >= seq)
        return true;
    const uint64_t t0 = now_ns();
    const bool done = done_cv_.wait_for(lock, std::chrono::milliseconds(std::max<int64_t>(timeout_ms, 0)),
                                        [this, seq] { return done_ >= seq; });
    ++stats_.fence_waits;
    stats_.fence_wait_ns += now_ns() - t0;
    return done;
}

void AsyncBackend::run_in_order(std::function<void()> fn) {
    const uint64_t seq = enqueue(std::move(fn));
    std::unique_lock lock(mutex_);
    wait_done(lock, seq);
}

void AsyncBackend::create_program(ProgramId id, const ProgramDesc& desc) {
    enqueue([this, id, desc] { inner_.create_program(id, desc); });
}
void AsyncBackend::destroy_program(ProgramId id) {
    enqueue([this, id] { inner_.destroy_program(id); });
}
void AsyncBackend::create_texture(TextureId id, const TextureDesc& desc) {
    enqueue([this, id, desc] { inner_.create_texture(id, desc); });
}
void AsyncBackend::destroy_texture(TextureId id) {
    enqueue([this, id] { inner_.destroy_texture(id); });
}

void AsyncBackend::write_texture(TextureId id, uint32_t level, const Rect& region, const void* data,
                                 uint32_t row_stride, const TexelSlices& slices) {
    // data is only valid during the call: after everything queued, here.
    drain();
    std::lock_guard inner(inner_mutex_);
    inner_.write_texture(id, level, region, data, row_stride, slices);
}

void AsyncBackend::execute(const CommandList& list) {
    execute_owned(CommandList(list));
}
void AsyncBackend::execute_owned(CommandList&& list) {
    // std::function needs a copyable callable; the shared_ptr keeps the list
    // from being copied again.
    auto owned = std::make_shared<CommandList>(std::move(list));
    enqueue([this, owned] { inner_.execute(*owned); });
}

bool AsyncBackend::present(const PresentInfo& info) {
    enqueue([this, info] { last_present_.store(inner_.present(info), std::memory_order_release); }, true);
    return last_present_.load(std::memory_order_acquire);
}

uint64_t AsyncBackend::fence_mark() {
    return enqueue([this] {
        const uint64_t mark = inner_.fence_mark();
        std::lock_guard lock(mutex_);
        marks_[done_ + 1] = mark;  // tasks run in order: this is task done_ + 1
        while (marks_.size() > kMaxMarks)
            marks_.erase(marks_.begin());
    });
}

void AsyncBackend::after_queued(std::function<void()> fn) {
    enqueue(std::move(fn));
}

void AsyncBackend::finish_until(uint64_t seq) {
    bool known = false;
    uint64_t mark = 0;
    {
        std::unique_lock lock(mutex_);
        wait_done(lock, std::min(seq, enqueued_));
        if (auto it = marks_.find(seq); it != marks_.end()) {
            known = true;
            mark = it->second;
        }
        marks_.erase(marks_.begin(), marks_.upper_bound(seq));
    }
    std::lock_guard inner(inner_mutex_);
    if (known)
        inner_.finish_until(mark);
    else
        inner_.finish();  // forgotten fence: everything, as backends without marks do
}

void AsyncBackend::finish() {
    drain();
    std::lock_guard inner(inner_mutex_);
    inner_.finish();
}

AsyncBackend::Stats AsyncBackend::stats() const {
    std::lock_guard lock(mutex_);
    return stats_;
}

}  // namespace lgp::gpu::nvn
