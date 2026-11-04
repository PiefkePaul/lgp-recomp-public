# NVN high-level emulation (ADR-0003, P4.1): thunks for the game's NVN proc
# slots, host-side NVN objects and command recording, plus the CPU reference
# backend. Depends on the runtime core (dispatch table, guest memory) and the
# platform interfaces and the shader translator (run-time fallback of
# ADR-0003), nothing else, so it builds for every target.
add_library(lgp_gpu_nvn STATIC
    src/abi.cpp
    src/texture_layout.cpp
    src/hle.cpp
    src/procs.cpp
    src/procs_draw.cpp
    src/shader_translate.cpp
    src/soft_backend.cpp
    src/async_backend.cpp)
target_include_directories(lgp_gpu_nvn PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(lgp_gpu_nvn PUBLIC lgp_runtime_core lgp_platform PRIVATE lgp_gpu_shader)
if(MSVC)
    target_compile_options(lgp_gpu_nvn PRIVATE /W4)
else()
    target_compile_options(lgp_gpu_nvn PRIVATE -Wall -Wextra)
endif()
