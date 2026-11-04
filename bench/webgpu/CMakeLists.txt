# Draw-call benchmark for the NVN WebGPU backend (P6,
# docs/knowledge/webgpu-backend-draw-kosten.md). Times are only meaningful in
# a Release build; ctest runs a short smoke pass (labels "bench", "webgpu")
# that checks that every draw ran without validation errors.
if(NOT TARGET lgp_gpu_webgpu)
    return()
endif()
add_executable(lgp_webgpu_draw_bench webgpu_draw_bench.cpp)
target_link_libraries(lgp_webgpu_draw_bench PRIVATE lgp_gpu_webgpu)
lgp_webgpu_copy_runtime(lgp_webgpu_draw_bench)
if(NOT EMSCRIPTEN)
    add_test(NAME bench.webgpu_draws COMMAND lgp_webgpu_draw_bench --smoke)
    set_tests_properties(bench.webgpu_draws PROPERTIES LABELS "bench;webgpu" SKIP_RETURN_CODE 77)
endif()

# Host memory of shader modules and pipelines (P7, #216 Punkt 6): reads WGSL
# dumped with LGP_WEBGPU_DUMP_DIR (private); ctest only runs built-in shaders.
add_executable(lgp_wgsl_memprobe wgsl_memprobe.cpp)
target_link_libraries(lgp_wgsl_memprobe PRIVATE lgp_gpu_webgpu lgp_platform)
lgp_webgpu_copy_runtime(lgp_wgsl_memprobe)
if(NOT EMSCRIPTEN)
    add_test(NAME bench.wgsl_memprobe COMMAND lgp_wgsl_memprobe --smoke)
    set_tests_properties(bench.wgsl_memprobe PROPERTIES LABELS "bench;webgpu" SKIP_RETURN_CODE 77)
endif()
