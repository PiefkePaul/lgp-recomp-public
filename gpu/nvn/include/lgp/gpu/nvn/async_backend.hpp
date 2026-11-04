// Asynchronous execution of submitted work (LGP_WEBGPU_ASYNC_EXECUTE).
//
// Wraps another Backend and runs its calls on one worker thread, in order:
// nvnQueueSubmitCommands and nvnQueuePresentTexture return as soon as the work
// is queued, as on the console, where the GPU runs behind the CPU. The guest
// thread waits only where NVN says it does: nvnSyncWait waits until the work
// before its fence has run (Backend::fence_mark/finish_until), nvnQueueFinish
// for everything. Presents are bounded (max_presents_in_flight), so the queue
// never holds more than that many frames.
//
// Command lists read guest memory (vertex, index, uniform buffers) when they
// run, so later than with the synchronous backend; as on hardware, a game
// that rewrites a buffer the GPU may still read must fence first. Calls that
// pass host pointers (write_texture) drain the queue and run synchronously.
//
// Every call of the inner backend comes from the worker or, for waits
// (finish, finish_until, write_texture), from the caller while the worker
// sits between two tasks, so the inner backend still sees one call at a time.
// Its other functions (stats, read_presented, ...) are called directly as
// before; readers of the presented image call drain() first.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <thread>

#include "lgp/gpu/nvn/backend.hpp"

namespace lgp::gpu::nvn {

class AsyncBackend final : public Backend {
public:
    struct Options {
        // nvnQueuePresentTexture returns while at most this many presents are
        // queued or running (1 = the guest records frame N+1 while frame N runs).
        uint32_t max_presents_in_flight = 1;
        // Bound for long stretches without presents (loading).
        uint32_t max_queued_tasks = 4096;
    };
    explicit AsyncBackend(Backend& inner) : AsyncBackend(inner, Options{}) {}
    AsyncBackend(Backend& inner, const Options& options);
    ~AsyncBackend() override;  // drains, then ends the worker
    AsyncBackend(const AsyncBackend&) = delete;
    AsyncBackend& operator=(const AsyncBackend&) = delete;

    void create_program(ProgramId id, const ProgramDesc& desc) override;
    void destroy_program(ProgramId id) override;
    void create_texture(TextureId id, const TextureDesc& desc) override;
    void destroy_texture(TextureId id) override;
    void write_texture(TextureId id, uint32_t level, const Rect& region, const void* data, uint32_t row_stride,
                       const TexelSlices& slices) override;
    void execute(const CommandList& list) override;
    void execute_owned(CommandList&& list) override;
    // Returns the result of the latest present that has run (true before the first).
    bool present(const PresentInfo& info) override;
    void finish() override;
    uint64_t fence_mark() override;
    void finish_until(uint64_t mark) override;
    void after_queued(std::function<void()> fn) override;

    // Waits until everything queued so far has run (not finish(): compute
    // write-backs of the inner backend may still be pending).
    void drain();
    // drain() with a limit: false if the queue was not done within timeout_ms
    // (a worker stuck in the driver); the callers on the window thread go on
    // instead of freezing the app.
    bool drain_for(int64_t timeout_ms);
    // Runs fn on the worker after everything queued so far and returns once
    // it has run: readers of the inner backend (the presented image) use it,
    // so they never run beside the worker's next task.
    void run_in_order(std::function<void()> fn);

    struct Stats {
        uint64_t tasks = 0;               // calls run on the worker
        uint64_t worker_busy_ns = 0;      // time the worker spent inside the inner backend
        uint64_t present_waits = 0;       // presents that waited for an earlier one
        uint64_t present_wait_ns = 0;
        uint64_t fence_waits = 0;         // finish/finish_until/drain that had to wait
        uint64_t fence_wait_ns = 0;
        uint64_t queue_full_waits = 0;    // calls that waited for max_queued_tasks
        uint64_t max_queued = 0;          // longest queue seen
    };
    Stats stats() const;

private:
    // Returns the task's number (1-based, in enqueue order).
    uint64_t enqueue(std::function<void()> task, bool is_present = false);
    // Waits until task number `seq` has run; lock holds mutex_.
    void wait_done(std::unique_lock<std::mutex>& lock, uint64_t seq);
    void worker();

    Backend& inner_;
    const Options options_;
    // Held around every call of the inner backend.
    std::mutex inner_mutex_;
    mutable std::mutex mutex_;
    std::condition_variable work_cv_;  // worker: new task or stop
    std::condition_variable done_cv_;  // callers: a task finished
    struct Task {
        std::function<void()> run;
        bool present = false;
    };
    std::deque<Task> queue_;
    uint64_t enqueued_ = 0;            // tasks queued since start
    uint64_t done_ = 0;                // tasks run since start
    uint32_t presents_pending_ = 0;    // queued or running
    bool stop_ = false;
    // Fence sequence number -> the inner backend's mark at that point.
    std::map<uint64_t, uint64_t> marks_;
    std::atomic<bool> last_present_{true};
    Stats stats_;
    std::thread thread_;
};

}  // namespace lgp::gpu::nvn
