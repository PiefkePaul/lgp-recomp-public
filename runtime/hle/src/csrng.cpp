// csrng (IRandomInterface): random bytes. Deterministic: a SplitMix64 stream
// seeded from the process' RandomEntropy, shared by all sessions, so a run
// with the same inputs sees the same bytes. Not cryptographic; nothing in the
// game needs that offline.
#include <mutex>

#include "lgp/hle/services.hpp"

namespace lgp::hle {
namespace {

class RandomState {
public:
    explicit RandomState(const rt::ProcessConfig& process)
        : state_(process.random_entropy[0] ^ (process.random_entropy[1] << 1) ^ 0x637372'6e67ull) {}

    void fill(uint8_t* out, size_t n) {
        std::lock_guard lock(mutex_);
        for (size_t i = 0; i < n; i += 8) {
            const uint64_t v = next();
            for (size_t j = 0; j < 8 && i + j < n; ++j)
                out[i + j] = static_cast<uint8_t>(v >> (8 * j));
        }
    }

private:
    uint64_t next() {
        uint64_t z = (state_ += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }

    std::mutex mutex_;
    uint64_t state_;
};

class RandomInterface final : public Service {
public:
    explicit RandomInterface(std::shared_ptr<RandomState> state) : Service("csrng"), state_(std::move(state)) {
        on(0, [this](Request& req, Response& resp) {  // GetRandomBytes(out buffer 0x6)
            const HipcBuffer out = req.out_buffer(0);
            if (out.size > Request::kMaxBufferCopy) {
                resp.result = rt::kernel_result::kInvalidSize;
                return;
            }
            std::vector<uint8_t> bytes(static_cast<size_t>(out.size));
            state_->fill(bytes.data(), bytes.size());
            if (!bytes.empty() && !req.write_out(0, bytes.data(), bytes.size()))
                resp.result = rt::kernel_result::kInvalidPointer;
        });
    }

private:
    std::shared_ptr<RandomState> state_;
};

}  // namespace

void register_csrng(ServiceManager& sm, const rt::ProcessConfig& process) {
    auto state = std::make_shared<RandomState>(process);
    sm.register_service("csrng", [state] { return std::make_shared<RandomInterface>(state); });
}

}  // namespace lgp::hle
