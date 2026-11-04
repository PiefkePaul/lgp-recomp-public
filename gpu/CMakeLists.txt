# GPU (ADR-0003): shader translation (P4.2), CPU texture decoding (P4.3) and
# the NVN-HLE front-end (P4.1).
add_subdirectory(shader)
add_subdirectory(texture)
add_subdirectory(nvn)
# WebGPU renderer for the NVN-HLE (P4.4).
if(LGP_WEBGPU_AVAILABLE)
    add_subdirectory(webgpu)
endif()
