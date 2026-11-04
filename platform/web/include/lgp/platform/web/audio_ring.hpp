// Single-producer single-consumer ring of interleaved 16-bit frames in
// shared memory (P5.2). The producer is audout (WebAudio sink, any wasm
// thread); the consumer is the AudioWorklet (audio_worklet.js), which reads
// the same bytes of the wasm memory (a SharedArrayBuffer) through Int32Array
// and Int16Array views with Atomics. The layout is therefore fixed:
//
//   int32 [0] write     frames written so far (mod 2^32), producer only
//   int32 [1] read      frames consumed so far (mod 2^32), consumer only
//   int32 [2] capacity  frames, a power of two
//   int32 [3] channels
//   int32 [4] paused    non-zero: the consumer outputs silence, keeps read
//   int32 [5] underruns frames of silence played while not paused
//   int32 [6] closed    non-zero: the consumer stops for good
//   int32 [7] reserved
//   int16 [16 + ...]    capacity * channels samples
//
// Plain C++ (no platform headers) so the Node test can exercise it, and
// read() mirrors the worklet's loop for that test.
#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace lgp::platform::web {

class AudioRing {
public:
    static constexpr size_t kHeaderBytes = 32;

    struct Header {
        std::atomic<uint32_t> write;
        std::atomic<uint32_t> read;
        uint32_t capacity;
        uint32_t channels;
        std::atomic<uint32_t> paused;
        std::atomic<uint32_t> underruns;
        std::atomic<uint32_t> closed;
        uint32_t reserved;
    };
    static_assert(sizeof(Header) == kHeaderBytes, "layout shared with audio_worklet.js");
    static_assert(std::atomic<uint32_t>::is_always_lock_free, "the worklet uses Atomics on the same words");

    // Bytes of memory for a ring of capacity frames (rounded up to a power of two).
    static size_t bytes(size_t capacity, uint32_t channels) {
        return kHeaderBytes + round_capacity(capacity) * channels * sizeof(int16_t);
    }
    static size_t round_capacity(size_t capacity) {
        size_t c = 1;
        while (c < capacity) c <<= 1;
        return c;
    }

    // Lays a ring out in memory (bytes(capacity, channels), 4-byte aligned).
    AudioRing(void* memory, size_t capacity, uint32_t channels)
        : header_(static_cast<Header*>(memory)),
          samples_(reinterpret_cast<int16_t*>(static_cast<uint8_t*>(memory) + kHeaderBytes)) {
        std::memset(memory, 0, bytes(capacity, channels));
        header_->capacity = static_cast<uint32_t>(round_capacity(capacity));
        header_->channels = channels;
    }

    uint32_t capacity() const { return header_->capacity; }
    uint32_t channels() const { return header_->channels; }
    void* memory() const { return header_; }

    size_t queued() const {
        return header_->write.load(std::memory_order_acquire) - header_->read.load(std::memory_order_acquire);
    }

    // Producer: copies up to frames frames; returns how many fit.
    size_t write(const int16_t* samples, size_t frames) {
        const uint32_t w = header_->write.load(std::memory_order_relaxed);
        const uint32_t r = header_->read.load(std::memory_order_acquire);
        const uint32_t cap = header_->capacity, ch = header_->channels;
        const size_t n = std::min<size_t>(frames, cap - (w - r));
        const uint32_t at = w & (cap - 1);
        const size_t first = std::min<size_t>(n, cap - at);
        std::memcpy(samples_ + size_t{at} * ch, samples, first * ch * sizeof(int16_t));
        std::memcpy(samples_, samples + first * ch, (n - first) * ch * sizeof(int16_t));
        header_->write.store(w + static_cast<uint32_t>(n), std::memory_order_release);
        return n;
    }

    // Consumer (tests; the browser's consumer is audio_worklet.js): fills
    // frames frames, silence beyond what is queued or while paused.
    // Returns the frames taken from the ring.
    size_t read(int16_t* out, size_t frames) {
        const uint32_t r = header_->read.load(std::memory_order_relaxed);
        const uint32_t w = header_->write.load(std::memory_order_acquire);
        const uint32_t cap = header_->capacity, ch = header_->channels;
        const bool paused = header_->paused.load(std::memory_order_relaxed) != 0;
        const size_t n = paused ? 0 : std::min<size_t>(frames, w - r);
        for (size_t i = 0; i < n; ++i)
            std::memcpy(out + i * ch, samples_ + size_t{(r + i) & (cap - 1)} * ch, ch * sizeof(int16_t));
        std::fill(out + n * ch, out + frames * ch, int16_t{0});
        if (!paused && n < frames)
            header_->underruns.fetch_add(static_cast<uint32_t>(frames - n), std::memory_order_relaxed);
        header_->read.store(r + static_cast<uint32_t>(n), std::memory_order_release);
        return n;
    }

    void set_paused(bool paused) { header_->paused.store(paused ? 1 : 0, std::memory_order_release); }
    void close() { header_->closed.store(1, std::memory_order_release); }
    uint32_t underruns() const { return header_->underruns.load(std::memory_order_relaxed); }

private:
    Header* header_;
    int16_t* samples_;
};

}  // namespace lgp::platform::web
