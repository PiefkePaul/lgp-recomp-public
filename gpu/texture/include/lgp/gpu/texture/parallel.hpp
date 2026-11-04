// Splits CPU texture decoding over a few threads (LGP_WEBGPU_DECODE_THREADS).
//
// A small pool of persistent workers: decoding a level on the GPU thread
// costs tens of milliseconds for large BCn textures on phones and TVs (no
// texture-compression-bc), and a scene change uploads hundreds of them.
// Block rows are independent, so the work splits into row ranges; the
// calling thread takes ranges too. One job at a time (callers wait for each
// other), no dependencies beyond the standard library.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

#include "lgp/gpu/texture/bc.hpp"

namespace lgp::gpu::texture {

class DecodePool {
public:
    // threads: helpers besides the caller (0 = everything on the caller).
    explicit DecodePool(uint32_t threads);
    ~DecodePool();
    DecodePool(const DecodePool&) = delete;
    DecodePool& operator=(const DecodePool&) = delete;

    uint32_t threads() const;
    // Calls fn(begin, end) for consecutive ranges covering [0, n), each at
    // most `chunk` long, on the helpers and the caller; returns when all ran.
    // fn must not throw: the helpers would still run it after the caller left.
    void run(size_t n, size_t chunk, const std::function<void(size_t, size_t)>& fn);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// decode_bc_image (bc.hpp) with the block rows split over pool's threads;
// the same result. pool == nullptr or small images: on the caller only.
void decode_bc_image(BcFormat format, const uint8_t* data, uint32_t width, uint32_t height, uint8_t* out,
                     DecodePool* pool);

}  // namespace lgp::gpu::texture
