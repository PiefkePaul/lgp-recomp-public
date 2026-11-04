// gpu_memory (host_env.hpp) on Apple: the Metal device's own count.
#import <Metal/Metal.h>

#include "lgp/platform/host_env.hpp"

namespace lgp::platform {

bool gpu_memory(GpuMemory* out) {
    // The system default device is the one wgpu-hal opens on iOS/tvOS and
    // on single-GPU Macs; kept for the process.
    static id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (device == nil)
        return false;
    out->allocated = uint64_t(device.currentAllocatedSize);
    out->working_set = uint64_t(device.recommendedMaxWorkingSetSize);
    return true;
}

}  // namespace lgp::platform
