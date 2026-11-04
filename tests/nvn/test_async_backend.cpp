// AsyncBackend (lgp/gpu/nvn/async_backend.hpp): order, fences, present bound,
// one inner call at a time. No GPU, a recording backend.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../runtime/check.hpp"
#include "lgp/gpu/nvn/async_backend.hpp"

namespace nvn = lgp::gpu::nvn;

namespace {

// Records every call; execute and present can be held at a gate.
class Recorder final : public nvn::Backend {
public:
    void create_program(nvn::ProgramId id, const nvn::ProgramDesc&) override { log("program " + std::to_string(id)); }
    void destroy_program(nvn::ProgramId id) override { log("~program " + std::to_string(id)); }
    void create_texture(nvn::TextureId id, const nvn::TextureDesc&) override { log("texture " + std::to_string(id)); }
    void destroy_texture(nvn::TextureId id) override { log("~texture " + std::to_string(id)); }
    void write_texture(nvn::TextureId id, uint32_t, const nvn::Rect&, const void* data, uint32_t,
                       const nvn::TexelSlices&) override {
        log("write " + std::to_string(id) + " " + std::to_string(*static_cast<const uint8_t*>(data)));
    }
    void execute(const nvn::CommandList& list) override {
        pass_gate();
        log("execute " + std::to_string(list.commands.size()));
        executed.fetch_add(1);
    }
    bool present(const nvn::PresentInfo& info) override {
        pass_gate();
        log("present " + std::to_string(info.texture));
        return info.texture != 99;  // 99: "window closed"
    }
    void finish() override { log("finish"); }
    uint64_t fence_mark() override { return 100 + executed.load(); }
    void finish_until(uint64_t mark) override { log("finish_until " + std::to_string(mark)); }

    void close_gate() {
        std::lock_guard lock(gate_mutex_);
        open_ = false;
    }
    void open_gate() {
        {
            std::lock_guard lock(gate_mutex_);
            open_ = true;
        }
        gate_cv_.notify_all();
    }
    std::vector<std::string> calls() {
        std::lock_guard lock(log_mutex_);
        return log_;
    }

    std::atomic<int> executed{0};
    std::atomic<int> overlaps{0};

private:
    void pass_gate() {
        std::unique_lock lock(gate_mutex_);
        gate_cv_.wait(lock, [this] { return open_; });
    }
    void log(std::string s) {
        if (inside_.fetch_add(1) != 0)
            overlaps.fetch_add(1);
        std::this_thread::sleep_for(std::chrono::microseconds(50));  // widen any overlap
        {
            std::lock_guard lock(log_mutex_);
            log_.push_back(std::move(s));
        }
        inside_.fetch_sub(1);
    }

    std::atomic<int> inside_{0};
    std::mutex log_mutex_;
    std::vector<std::string> log_;
    std::mutex gate_mutex_;
    std::condition_variable gate_cv_;
    bool open_ = true;
};

nvn::CommandList list_of(size_t n) {
    nvn::CommandList l;
    l.commands.resize(n, nvn::CmdBarrier{});
    return l;
}

void test_order_and_fences() {
    Recorder r;
    nvn::AsyncBackend a(r);
    r.close_gate();
    a.create_texture(1, {});
    a.execute(list_of(3));
    const uint64_t fence = a.fence_mark();
    a.execute_owned(list_of(5));
    // Queued, not run: execute returned while the inner one waits at the gate.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK_EQ(r.executed.load(), 0);
    std::thread opener([&r] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        r.open_gate();
    });
    a.finish_until(fence);  // waits for the first list only (the gate opens meanwhile)
    opener.join();
    CHECK(r.executed.load() >= 1);
    a.drain();
    const auto calls = r.calls();
    // The fence's inner mark is taken after the first list: 100 + 1.
    bool saw_until = false;
    for (const auto& c : calls)
        saw_until |= c == "finish_until 101";
    CHECK(saw_until);
    std::vector<std::string> run;
    for (const auto& c : calls)
        if (c.rfind("finish_until", 0) != 0)
            run.push_back(c);
    CHECK(run.size() == 3);
    if (run.size() == 3) {
        CHECK(run[0] == "texture 1");
        CHECK(run[1] == "execute 3");
        CHECK(run[2] == "execute 5");
    }
    CHECK_EQ(r.overlaps.load(), 0);
}

void test_write_texture_drains() {
    Recorder r;
    nvn::AsyncBackend a(r);
    a.execute(list_of(1));
    a.destroy_texture(4);
    uint8_t byte = 7;
    a.write_texture(4, 0, {}, &byte, 1, {});
    byte = 0;  // the data is no longer used after the call
    const auto calls = r.calls();
    CHECK(calls.size() == 3);
    if (calls.size() == 3) {
        CHECK(calls[0] == "execute 1");
        CHECK(calls[1] == "~texture 4");
        CHECK(calls[2] == "write 4 7");
    }
}

void test_present_bound() {
    Recorder r;
    nvn::AsyncBackend::Options o;
    o.max_presents_in_flight = 1;
    nvn::AsyncBackend a(r, o);
    r.close_gate();
    CHECK(a.present({1, {}, false}));  // queued, returns at once
    std::atomic<bool> second_started{false}, second_done{false};
    std::thread t([&] {
        second_started = true;
        a.present({2, {}, false});  // waits: one present is in flight
        second_done = true;
    });
    // On a loaded runner the thread may start late: opening the gate before
    // it reaches present() would let it pass without waiting.
    while (!second_started.load()) std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    CHECK(!second_done.load());
    r.open_gate();
    t.join();
    CHECK(second_done.load());
    a.present({99, {}, false});
    a.drain();
    // present() returns the previous present's result; hold the worker at the
    // gate so present 3 cannot finish before that is read.
    r.close_gate();
    CHECK(!a.present({3, {}, false}));  // the closed window from present 99
    r.open_gate();
    a.finish();
    const auto st = a.stats();
    CHECK(st.present_waits >= 1);
    CHECK_EQ(st.tasks, 4u);
    CHECK_EQ(r.overlaps.load(), 0);
}

void test_forgotten_fence() {
    Recorder r;
    nvn::AsyncBackend a(r);
    const uint64_t first = a.fence_mark();
    for (int i = 0; i < 1100; ++i)
        a.fence_mark();
    a.drain();  // all fences recorded: the first one's mark is dropped by now
    a.finish_until(first);  // its mark was dropped: everything
    const auto calls = r.calls();
    CHECK(!calls.empty() && calls.back() == "finish");
}

// Counter reports (after_queued) run after the lists before them, not at
// submit: the guest only sees them once the list has executed.
void test_after_queued() {
    Recorder r;
    nvn::AsyncBackend a(r);
    r.close_gate();
    a.execute(list_of(2));
    std::atomic<int> seen{-1};
    a.after_queued([&] { seen = r.executed.load(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK_EQ(seen.load(), -1);  // the list waits at the gate, so does the report
    r.open_gate();
    a.finish();
    CHECK_EQ(seen.load(), 1);
}

// run_in_order: after the queued lists, on the worker, and returns once done.
void test_run_in_order() {
    Recorder r;
    nvn::AsyncBackend a(r);
    r.close_gate();
    a.execute(list_of(4));
    std::thread opener([&r] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        r.open_gate();
    });
    int seen = -1;
    a.run_in_order([&] { seen = r.executed.load(); });
    CHECK_EQ(seen, 1);
    opener.join();
}

// drain_for: false while a task hangs (the window thread goes on), true
// once the queue is done.
void test_drain_for() {
    Recorder r;
    nvn::AsyncBackend a(r);
    CHECK(a.drain_for(0));  // nothing queued
    r.close_gate();
    a.execute(list_of(1));
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(!a.drain_for(30));
    CHECK(std::chrono::steady_clock::now() - t0 >= std::chrono::milliseconds(25));  // timer slack
    r.open_gate();
    CHECK(a.drain_for(5000));
    CHECK_EQ(r.executed.load(), 1);
}

}  // namespace

int main() {
    RUN(test_order_and_fences);
    RUN(test_write_texture_drains);
    RUN(test_present_bound);
    RUN(test_forgotten_fence);
    RUN(test_after_queued);
    RUN(test_run_in_order);
    RUN(test_drain_for);
    return finish();
}
