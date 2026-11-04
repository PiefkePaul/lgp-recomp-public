// IRenderer: the window or canvas and the WebGPU device the graphics
// front-end draws with (ADR-0003: NVN-HLE on WebGPU; P3.6, filled in by P4).
//
// The interface deliberately carries WebGPU objects as opaque pointers:
// gpu/ includes webgpu.h and casts them, the runtime never does. A backend
// creates instance, adapter, device and the presentation surface (Dawn or
// wgpu-native plus a window on desktop, navigator.gpu plus an OffscreenCanvas
// in a worker on the web, ADR-0002). A backend without a GPU (the null
// backend, CI) returns empty handles; present_rgba8() is the CPU path for
// bring-up and tests.
#pragma once

#include <cstdint>

namespace lgp::platform {

struct SurfaceSize {
    uint32_t width = 0;
    uint32_t height = 0;
};

struct GpuHandles {
    void* instance = nullptr;  // WGPUInstance
    void* adapter = nullptr;   // WGPUAdapter
    void* device = nullptr;    // WGPUDevice
    void* queue = nullptr;     // WGPUQueue
    void* surface = nullptr;   // WGPUSurface (configured by the backend)
    uint32_t surface_format = 0;  // WGPUTextureFormat of the surface
    bool valid() const { return device != nullptr; }
};

// Tightly packed or strided RGBA8 pixels, top row first.
struct FrameView {
    const uint8_t* pixels = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t stride = 0;  // bytes per row, >= width * 4
};

class IRenderer {
public:
    virtual ~IRenderer() = default;
    // Current drawable size (the game renders 1280x720 handheld /
    // 1920x1080 docked and is scaled to this).
    virtual SurfaceSize surface_size() const = 0;
    virtual GpuHandles gpu() const = 0;
    // CPU path: show an image (tests, early bring-up without a GPU).
    virtual void present_rgba8(const FrameView& frame) = 0;
    // End of a guest frame (after the queue submit): present, pump window
    // events. False once the user closed the window; the runtime then ends
    // the process.
    virtual bool end_frame() = 0;
};

}  // namespace lgp::platform
