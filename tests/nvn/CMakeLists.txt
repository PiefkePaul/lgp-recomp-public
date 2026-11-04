# NVN-HLE (P4.1, ADR-0003). Label "nvn".

# NVN calls through the dispatch table with the guest ABI, CPU backend.
add_executable(lgp_test_nvn_hle test_nvn_hle.cpp)
target_link_libraries(lgp_test_nvn_hle PRIVATE lgp_gpu_nvn lgp_platform_common lgp_lifted_headers)
if(EMSCRIPTEN)
    target_link_options(lgp_test_nvn_hle PRIVATE -sALLOW_MEMORY_GROWTH=1 -sMAXIMUM_MEMORY=4GB)
endif()
add_test(NAME nvn.hle COMMAND lgp_test_nvn_hle)
set_tests_properties(nvn.hle PROPERTIES LABELS "nvn")
add_test(NAME nvn.projection_scan COMMAND lgp_test_nvn_hle --projection-scan)
set_tests_properties(nvn.projection_scan PROPERTIES LABELS "nvn"
                     PASS_REGULAR_EXPRESSION "\\+0x40 projection \\(rows\\) aspect 2\\.333[0-9] fovy 50\\.00 deg.*\\+0x100 projection \\(columns\\) aspect 1\\.777[0-9] fovy 50\\.00.*\\+0x1c0 view-projection \\(rows\\) aspect 1\\.333[0-9] fovy 40\\.00 deg from 0x0.*\\+0x1c0 view-projection \\(rows\\) aspect 1\\.333[0-9] fovy 40\\.00 deg from submit.*nvn cbuf find: frame 0 value 2\\.14451 = 2\\.14451 at cbuf 0x[0-9a-f]+ \\+0x54 \\(vertex c\\[3\\]\\) draw into 8x8.*nvn draws: frame 1 into 8x8: 1 draws, 0 with no camera matrix; projection 1\\.7778 fovy 50\\.0"
                     FAIL_REGULAR_EXPRESSION "\\+0x180 ")

# Asynchronous execution (LGP_WEBGPU_ASYNC_EXECUTE): order, fences, present bound.
add_executable(lgp_test_async_backend test_async_backend.cpp)
target_link_libraries(lgp_test_async_backend PRIVATE lgp_gpu_nvn)
add_test(NAME nvn.async_backend COMMAND lgp_test_async_backend)
set_tests_properties(nvn.async_backend PROPERTIES LABELS "nvn")

# P4.3: block-linear layout and deswizzling, NVN format table.
add_executable(lgp_test_texture_layout test_texture_layout.cpp)
target_link_libraries(lgp_test_texture_layout PRIVATE lgp_gpu_nvn)
add_test(NAME nvn.texture_layout COMMAND lgp_test_texture_layout)
set_tests_properties(nvn.texture_layout PROPERTIES LABELS "nvn")

# P4.4: WebGPU backend against the CPU reference (needs a WebGPU adapter,
# lavapipe in CI; skipped with exit code 77 otherwise).
if(TARGET lgp_gpu_webgpu)
    add_executable(lgp_test_nvn_webgpu test_nvn_webgpu.cpp)
    target_link_libraries(lgp_test_nvn_webgpu PRIVATE lgp_gpu_webgpu lgp_platform_common)
    lgp_webgpu_copy_runtime(lgp_test_nvn_webgpu)
    add_test(NAME nvn.webgpu COMMAND lgp_test_nvn_webgpu)
    set_tests_properties(nvn.webgpu PROPERTIES LABELS "nvn;webgpu" SKIP_RETURN_CODE 77)

    # Warnings for device poll loops that keep waiting (no GPU needed).
    add_executable(lgp_test_poll_watch test_poll_watch.cpp)
    target_link_libraries(lgp_test_poll_watch PRIVATE lgp_gpu_webgpu)
    add_test(NAME nvn.poll_watch COMMAND lgp_test_poll_watch)
    set_tests_properties(nvn.poll_watch PROPERTIES LABELS "nvn;webgpu")

    # Draws with own Maxwell test shaders, translated by gpu/shader.
    add_executable(lgp_test_nvn_webgpu_draw test_nvn_webgpu_draw.cpp)
    target_link_libraries(lgp_test_nvn_webgpu_draw PRIVATE lgp_gpu_webgpu lgp_gpu_shader)
    lgp_webgpu_copy_runtime(lgp_test_nvn_webgpu_draw)
    if(EMSCRIPTEN)
        # Node reads the shader files through the host file system.
        target_link_options(lgp_test_nvn_webgpu_draw PRIVATE -sNODERAWFS=1)
    endif()
    add_test(NAME nvn.webgpu_draw COMMAND lgp_test_nvn_webgpu_draw "${PROJECT_SOURCE_DIR}/tests/gpu/shaders")
    set_tests_properties(nvn.webgpu_draw PROPERTIES LABELS "nvn;webgpu" SKIP_RETURN_CODE 77)
    if(NOT EMSCRIPTEN)
        # The browser's async pipelines (its default), with wgpu-native's
        # synchronous stand-in.
        add_test(NAME nvn.webgpu_draw_async COMMAND lgp_test_nvn_webgpu_draw "${PROJECT_SOURCE_DIR}/tests/gpu/shaders")
        set_tests_properties(nvn.webgpu_draw_async PROPERTIES LABELS "nvn;webgpu" SKIP_RETURN_CODE 77
                             ENVIRONMENT "LGP_WEBGPU_ASYNC_PIPELINES=1")
        # ... with a list's missing pipelines created on several threads.
        add_test(NAME nvn.webgpu_draw_threads COMMAND lgp_test_nvn_webgpu_draw "${PROJECT_SOURCE_DIR}/tests/gpu/shaders")
        set_tests_properties(nvn.webgpu_draw_threads PROPERTIES LABELS "nvn;webgpu" SKIP_RETURN_CODE 77
                             ENVIRONMENT "LGP_WEBGPU_PIPELINE_THREADS=4")
        # ... with every result lost: the wait's timeout creates them synchronously.
        add_test(NAME nvn.webgpu_draw_async_lost COMMAND lgp_test_nvn_webgpu_draw "${PROJECT_SOURCE_DIR}/tests/gpu/shaders")
        set_tests_properties(nvn.webgpu_draw_async_lost PROPERTIES LABELS "nvn;webgpu" SKIP_RETURN_CODE 77
                             ENVIRONMENT "LGP_WEBGPU_ASYNC_PIPELINES=1;LGP_WEBGPU_DEBUG=dropasync")
        # ... without waiting for draw pipelines (LGP_WEBGPU_PIPELINE_NOWAIT):
        # the stand-in finishes them at once, so every draw still happens.
        add_test(NAME nvn.webgpu_draw_async_nowait COMMAND lgp_test_nvn_webgpu_draw "${PROJECT_SOURCE_DIR}/tests/gpu/shaders")
        set_tests_properties(nvn.webgpu_draw_async_nowait PROPERTIES LABELS "nvn;webgpu" SKIP_RETURN_CODE 77
                             ENVIRONMENT "LGP_WEBGPU_ASYNC_PIPELINES=1;LGP_WEBGPU_PIPELINE_NOWAIT=1")
        # BCn on the CPU with the reference decoder (LGP_WEBGPU_BC_DECODER, gpu/texture/bc.hpp).
        add_test(NAME nvn.webgpu_draw_bc_reference COMMAND lgp_test_nvn_webgpu_draw "${PROJECT_SOURCE_DIR}/tests/gpu/shaders")
        set_tests_properties(nvn.webgpu_draw_bc_reference PROPERTIES LABELS "nvn;webgpu" SKIP_RETURN_CODE 77
                             ENVIRONMENT "LGP_WEBGPU_BC_DECODER=reference")
        # Lazy textures (GPU side on first use) and the texture report (P7.3).
        add_test(NAME nvn.webgpu_draw_lazy COMMAND lgp_test_nvn_webgpu_draw "${PROJECT_SOURCE_DIR}/tests/gpu/shaders")
        set_tests_properties(nvn.webgpu_draw_lazy PROPERTIES LABELS "nvn;webgpu" SKIP_RETURN_CODE 77
                             ENVIRONMENT "LGP_WEBGPU_LAZY_TEXTURES=1;LGP_WEBGPU_TEXTURE_REPORT=1")
        # Dispatches in list order (LGP_WEBGPU_COMPUTE_FIRST=0), with the
        # asynchronous write-back.
        # Buffers of finished lists serve the next ones (LGP_WEBGPU_BUFFER_POOL).
        add_test(NAME nvn.webgpu_draw_buffer_pool COMMAND lgp_test_nvn_webgpu_draw "${PROJECT_SOURCE_DIR}/tests/gpu/shaders")
        set_tests_properties(nvn.webgpu_draw_buffer_pool PROPERTIES LABELS "nvn;webgpu" SKIP_RETURN_CODE 77
                             ENVIRONMENT "LGP_WEBGPU_BUFFER_POOL=1")
        # Queue writes submitted and waited for after every 4 KiB (LGP_WEBGPU_UPLOAD_FLUSH_MB).
        add_test(NAME nvn.webgpu_draw_upload_flush COMMAND lgp_test_nvn_webgpu_draw "${PROJECT_SOURCE_DIR}/tests/gpu/shaders")
        set_tests_properties(nvn.webgpu_draw_upload_flush PROPERTIES LABELS "nvn;webgpu" SKIP_RETURN_CODE 77
                             ENVIRONMENT "LGP_WEBGPU_UPLOAD_FLUSH_MB=0.004")
        add_test(NAME nvn.webgpu_draw_in_order COMMAND lgp_test_nvn_webgpu_draw "${PROJECT_SOURCE_DIR}/tests/gpu/shaders")
        set_tests_properties(nvn.webgpu_draw_in_order PROPERTIES LABELS "nvn;webgpu" SKIP_RETURN_CODE 77
                             ENVIRONMENT "LGP_WEBGPU_COMPUTE_FIRST=0")
        # Write-back waits on the read-back's submission (LGP_WEBGPU_POLL_SUBMIT_INDEX):
        # compute first with the synchronous write-back, then in list order
        # with the asynchronous one (after_writebacks).
        add_test(NAME nvn.webgpu_draw_poll_submit_index COMMAND lgp_test_nvn_webgpu_draw "${PROJECT_SOURCE_DIR}/tests/gpu/shaders")
        set_tests_properties(nvn.webgpu_draw_poll_submit_index PROPERTIES LABELS "nvn;webgpu" SKIP_RETURN_CODE 77
                             ENVIRONMENT "LGP_WEBGPU_POLL_SUBMIT_INDEX=1")
        add_test(NAME nvn.webgpu_draw_poll_submit_index_in_order COMMAND lgp_test_nvn_webgpu_draw "${PROJECT_SOURCE_DIR}/tests/gpu/shaders")
        set_tests_properties(nvn.webgpu_draw_poll_submit_index_in_order PROPERTIES LABELS "nvn;webgpu" SKIP_RETURN_CODE 77
                             ENVIRONMENT "LGP_WEBGPU_POLL_SUBMIT_INDEX=1;LGP_WEBGPU_COMPUTE_FIRST=0")
        # BCn decoding helpers (LGP_WEBGPU_DECODE_THREADS): the test's BC textures are
        # too small to be split, so this covers making and ending the pool; the split: gpu.bc.
        add_test(NAME nvn.webgpu_draw_decode_threads COMMAND lgp_test_nvn_webgpu_draw "${PROJECT_SOURCE_DIR}/tests/gpu/shaders")
        set_tests_properties(nvn.webgpu_draw_decode_threads PROPERTIES LABELS "nvn;webgpu" SKIP_RETURN_CODE 77
                             ENVIRONMENT "LGP_WEBGPU_DECODE_THREADS=2")
        # Vertex streams partly written by a dispatch of the list get its
        # output copied over on the GPU (LGP_WEBGPU_PATCH_DISPATCH_OUTPUT).
        add_test(NAME nvn.webgpu_draw_patch_dispatch_output COMMAND lgp_test_nvn_webgpu_draw "${PROJECT_SOURCE_DIR}/tests/gpu/shaders")
        set_tests_properties(nvn.webgpu_draw_patch_dispatch_output PROPERTIES LABELS "nvn;webgpu" SKIP_RETURN_CODE 77
                             ENVIRONMENT "LGP_WEBGPU_PATCH_DISPATCH_OUTPUT=1")
        # Draw passes stay open across dispatches submitted ahead.
        add_test(NAME nvn.webgpu_draw_keep_pass COMMAND lgp_test_nvn_webgpu_draw "${PROJECT_SOURCE_DIR}/tests/gpu/shaders")
        set_tests_properties(nvn.webgpu_draw_keep_pass PROPERTIES LABELS "nvn;webgpu" SKIP_RETURN_CODE 77
                             ENVIRONMENT "LGP_WEBGPU_KEEP_PASS_ON_DISPATCH=1")
        # Whole-texture clears as load ops of the next draw pass, and draw
        # passes kept open when the same targets are set again.
        add_test(NAME nvn.webgpu_draw_fold_clears COMMAND lgp_test_nvn_webgpu_draw "${PROJECT_SOURCE_DIR}/tests/gpu/shaders")
        set_tests_properties(nvn.webgpu_draw_fold_clears PROPERTIES LABELS "nvn;webgpu" SKIP_RETURN_CODE 77
                             ENVIRONMENT "LGP_WEBGPU_FOLD_CLEARS=1;LGP_WEBGPU_KEEP_PASS_SAME_TARGETS=1")
        # The same with dispatches in list order (pending clears flushed before them).
        add_test(NAME nvn.webgpu_draw_fold_clears_in_order COMMAND lgp_test_nvn_webgpu_draw "${PROJECT_SOURCE_DIR}/tests/gpu/shaders")
        set_tests_properties(nvn.webgpu_draw_fold_clears_in_order PROPERTIES LABELS "nvn;webgpu" SKIP_RETURN_CODE 77
                             ENVIRONMENT "LGP_WEBGPU_COMPUTE_FIRST=0;LGP_WEBGPU_FOLD_CLEARS=1;LGP_WEBGPU_KEEP_PASS_SAME_TARGETS=1")
        # Geometry cache with fingerprints instead of guest copies (LGP_WEBGPU_GEOMETRY_FP).
        add_test(NAME nvn.webgpu_draw_geometry_fp COMMAND lgp_test_nvn_webgpu_draw "${PROJECT_SOURCE_DIR}/tests/gpu/shaders")
        set_tests_properties(nvn.webgpu_draw_geometry_fp PROPERTIES LABELS "nvn;webgpu" SKIP_RETURN_CODE 77
                             ENVIRONMENT "LGP_WEBGPU_GEOMETRY_FP=1")
        # New draw pipelines counted by what sets them apart (diagnosis).
        add_test(NAME nvn.webgpu_draw_pipeline_variants COMMAND lgp_test_nvn_webgpu_draw "${PROJECT_SOURCE_DIR}/tests/gpu/shaders")
        set_tests_properties(nvn.webgpu_draw_pipeline_variants PROPERTIES LABELS "nvn;webgpu" SKIP_RETURN_CODE 77
                             ENVIRONMENT "LGP_WEBGPU_DEBUG=pipelinevariants")
        # GPU time per pass from timestamp queries (diagnosis).
        add_test(NAME nvn.webgpu_draw_gputime COMMAND lgp_test_nvn_webgpu_draw "${PROJECT_SOURCE_DIR}/tests/gpu/shaders")
        set_tests_properties(nvn.webgpu_draw_gputime PROPERTIES LABELS "nvn;webgpu" SKIP_RETURN_CODE 77
                             ENVIRONMENT "LGP_WEBGPU_DEBUG=gputime")
        # Draws into large targets squeezed into the top-left quarter (diagnosis).
        add_test(NAME nvn.webgpu_draw_halffill COMMAND lgp_test_nvn_webgpu_draw "${PROJECT_SOURCE_DIR}/tests/gpu/shaders")
        set_tests_properties(nvn.webgpu_draw_halffill PROPERTIES LABELS "nvn;webgpu" SKIP_RETURN_CODE 77
                             ENVIRONMENT "LGP_WEBGPU_DEBUG=halffill")
        # Draws into the listed targets scaled to 0.75 (diagnosis, docs/analysis/41-fuellflaeche-hauptziel.md).
        add_test(NAME nvn.webgpu_draw_fill_scale COMMAND lgp_test_nvn_webgpu_draw "${PROJECT_SOURCE_DIR}/tests/gpu/shaders")
        set_tests_properties(nvn.webgpu_draw_fill_scale PROPERTIES LABELS "nvn;webgpu" SKIP_RETURN_CODE 77
                             ENVIRONMENT "LGP_WEBGPU_FILL_SCALE=0.75;LGP_WEBGPU_FILL_TARGETS=1280X720")
        # Render targets at half size (LGP_WEBGPU_RENDER_SCALE, prototype).
        add_test(NAME nvn.webgpu_draw_render_scale COMMAND lgp_test_nvn_webgpu_draw "${PROJECT_SOURCE_DIR}/tests/gpu/shaders")
        set_tests_properties(nvn.webgpu_draw_render_scale PROPERTIES LABELS "nvn;webgpu" SKIP_RETURN_CODE 77
                             ENVIRONMENT "LGP_WEBGPU_RENDER_SCALE=0.5;LGP_WEBGPU_RENDER_SCALE_MIN=700")
        # A malformed target list turns the switch off (no silent fallback to every large target).
        add_test(NAME nvn.webgpu_draw_fill_scale_bad_targets COMMAND lgp_test_nvn_webgpu_draw "${PROJECT_SOURCE_DIR}/tests/gpu/shaders")
        set_tests_properties(nvn.webgpu_draw_fill_scale_bad_targets PROPERTIES LABELS "nvn;webgpu" SKIP_RETURN_CODE 77
                             ENVIRONMENT "LGP_WEBGPU_FILL_SCALE=0.75;LGP_WEBGPU_FILL_TARGETS=1280x720x;LGP_TEST_FILL_SCALE_OFF=1")
        # LGP_WEBGPU_HALFRES_PROGRAMS: additive draws of listed programs at half resolution.
        add_test(NAME nvn.webgpu_draw_halfres_programs COMMAND lgp_test_nvn_webgpu_draw "${PROJECT_SOURCE_DIR}/tests/gpu/shaders")
        set_tests_properties(nvn.webgpu_draw_halfres_programs PROPERTIES LABELS "nvn;webgpu" SKIP_RETURN_CODE 77
                             ENVIRONMENT "LGP_WEBGPU_HALFRES_PROGRAMS=0,1,2,3,4,5,6,7,8,9,a,b,c,d,e,f")
        # Both together: the stand-in follows the scaled target (half of its host size).
        add_test(NAME nvn.webgpu_draw_halfres_render_scale COMMAND lgp_test_nvn_webgpu_draw "${PROJECT_SOURCE_DIR}/tests/gpu/shaders")
        set_tests_properties(nvn.webgpu_draw_halfres_render_scale PROPERTIES LABELS "nvn;webgpu" SKIP_RETURN_CODE 77
                             ENVIRONMENT "LGP_WEBGPU_HALFRES_PROGRAMS=0,1,2,3,4,5,6,7,8,9,a,b,c,d,e,f;LGP_WEBGPU_RENDER_SCALE=0.5;LGP_WEBGPU_RENDER_SCALE_MIN=700")
        # Passes named after their targets, Vulkan debug utils on (diagnosis).
        add_test(NAME nvn.webgpu_draw_passlabels COMMAND lgp_test_nvn_webgpu_draw "${PROJECT_SOURCE_DIR}/tests/gpu/shaders")
        set_tests_properties(nvn.webgpu_draw_passlabels PROPERTIES LABELS "nvn;webgpu" SKIP_RETURN_CODE 77
                             ENVIRONMENT "LGP_WEBGPU_DEBUG=passlabels")
    endif()

    # Pipeline cache: the file (refused paths, damaged tail, other GPU) and
    # pipelines created ahead in a second backend, by the worker thread and
    # with createRenderPipelineAsync (the browser's way).
    add_executable(lgp_test_pipeline_cache test_pipeline_cache.cpp)
    target_link_libraries(lgp_test_pipeline_cache PRIVATE lgp_gpu_webgpu)
    lgp_webgpu_copy_runtime(lgp_test_pipeline_cache)
    if(EMSCRIPTEN)
        target_link_options(lgp_test_pipeline_cache PRIVATE -sNODERAWFS=1 -sALLOW_MEMORY_GROWTH=1 -sMAXIMUM_MEMORY=1GB)  # 17 MiB test records
    endif()
    add_test(NAME nvn.pipeline_cache_file COMMAND lgp_test_pipeline_cache file "${PROJECT_SOURCE_DIR}")
    set_tests_properties(nvn.pipeline_cache_file PROPERTIES LABELS "nvn")
    add_test(NAME nvn.pipeline_cache COMMAND lgp_test_pipeline_cache gpu)
    set_tests_properties(nvn.pipeline_cache PROPERTIES LABELS "nvn;webgpu" SKIP_RETURN_CODE 77)
    if(NOT EMSCRIPTEN)
        add_test(NAME nvn.pipeline_cache_async COMMAND lgp_test_pipeline_cache gpu)
        set_tests_properties(nvn.pipeline_cache_async PROPERTIES LABELS "nvn;webgpu" SKIP_RETURN_CODE 77
                             ENVIRONMENT "LGP_WEBGPU_ASYNC_PIPELINES=1")
        # ... with the draw pipelines recorded to the cache when they arrive (no wait).
        add_test(NAME nvn.pipeline_cache_async_nowait COMMAND lgp_test_pipeline_cache gpu)
        set_tests_properties(nvn.pipeline_cache_async_nowait PROPERTIES LABELS "nvn;webgpu" SKIP_RETURN_CODE 77
                             ENVIRONMENT "LGP_WEBGPU_ASYNC_PIPELINES=1;LGP_WEBGPU_PIPELINE_NOWAIT=1")
    endif()
    if(EMSCRIPTEN)
        # The same in the browser (createRenderPipelineAsync, files in MEMFS):
        # browser.pipeline_cache in tests/web.
        add_executable(lgp_test_pipeline_cache_web test_pipeline_cache.cpp)
        target_link_libraries(lgp_test_pipeline_cache_web PRIVATE lgp_gpu_webgpu)
        set(_shell "${PROJECT_SOURCE_DIR}/platform/web/shell.html")
        set_target_properties(lgp_test_pipeline_cache_web PROPERTIES SUFFIX ".html" LINK_DEPENDS "${_shell}")
        target_link_options(lgp_test_pipeline_cache_web PRIVATE -sALLOW_MEMORY_GROWTH=1 "--shell-file=${_shell}")
    endif()

    # More vertex streams than vertex buffers (Chrome: 8): merged streams;
    # in the browser too (browser.webgpu_vertex_streams in tests/web).
    add_executable(lgp_test_webgpu_vertex_streams test_webgpu_vertex_streams.cpp)
    target_link_libraries(lgp_test_webgpu_vertex_streams PRIVATE lgp_gpu_webgpu)
    lgp_webgpu_copy_runtime(lgp_test_webgpu_vertex_streams)
    add_test(NAME nvn.webgpu_vertex_streams COMMAND lgp_test_webgpu_vertex_streams)
    set_tests_properties(nvn.webgpu_vertex_streams PROPERTIES LABELS "nvn;webgpu" SKIP_RETURN_CODE 77)
    if(EMSCRIPTEN)
        add_executable(lgp_test_webgpu_vertex_streams_web test_webgpu_vertex_streams.cpp)
        target_link_libraries(lgp_test_webgpu_vertex_streams_web PRIVATE lgp_gpu_webgpu)
        set_target_properties(lgp_test_webgpu_vertex_streams_web PROPERTIES SUFFIX ".html" LINK_DEPENDS "${_shell}")
        target_link_options(lgp_test_webgpu_vertex_streams_web PRIVATE -sALLOW_MEMORY_GROWTH=1
                            "--shell-file=${_shell}")
    endif()
endif()
# The demo end to end: a render thread presents 120 frames, headless and (on
# the desktop, with SDL3) in a window; xvfb-run provides the display in CI.
if(TARGET lgp_webgpu_demo AND NOT EMSCRIPTEN)
    add_test(NAME nvn.webgpu_demo_headless COMMAND lgp_webgpu_demo --headless --frames 120)
    set_tests_properties(nvn.webgpu_demo_headless PROPERTIES LABELS "nvn;webgpu" SKIP_RETURN_CODE 77)
    if(TARGET lgp_platform_desktop)
        find_program(LGP_XVFB_RUN NAMES xvfb-run DOC "xvfb-run: windows without a display")
        if(LGP_XVFB_RUN)
            add_test(NAME nvn.webgpu_demo_window
                     COMMAND "${LGP_XVFB_RUN}" -a -s "-screen 0 1600x900x24" $<TARGET_FILE:lgp_webgpu_demo> --frames 120)
        else()
            add_test(NAME nvn.webgpu_demo_window COMMAND lgp_webgpu_demo --frames 120)
        endif()
        set_tests_properties(nvn.webgpu_demo_window PROPERTIES LABELS "nvn;webgpu" SKIP_RETURN_CODE 77)
    endif()
endif()

# End to end: an AArch64 guest program (guest/nvn_clear.cpp) built with the
# host clang + ld.lld like the P2.5 test program (crt0, linker script and NRO
# packer from tests/indirect), recompiled with lgp_lift and run against the
# HLE. Needs no Unicorn; the generator runs on the build host, so not when
# cross-compiling.

# WASM: the draw test runs from what a native build generated (the lifted C++
# and the linked image, see below) because the recompiler's loader is not
# built for the web. tests/web runs the page in Chromium (label "browser").
# LGP_NVN_GUEST_IMPORT_DIR = tests/nvn of a built native tree.
if(EMSCRIPTEN AND TARGET lgp_gpu_webgpu)
    set(LGP_NVN_GUEST_IMPORT_DIR "${PROJECT_SOURCE_DIR}/build/default/tests/nvn" CACHE PATH
        "tests/nvn directory of a built native tree (lifted NVN guest programs for the WASM build)")
    set(_imported_lifted "${LGP_NVN_GUEST_IMPORT_DIR}/generated/nvn_triangle_lifted.cpp")
    set(_imported_image "${LGP_NVN_GUEST_IMPORT_DIR}/guest/nvn_triangle.img")
    if(EXISTS "${_imported_lifted}" AND EXISTS "${_imported_image}")
        add_executable(lgp_test_nvn_triangle_web test_nvn_triangle.cpp "${_imported_lifted}")
        target_compile_definitions(lgp_test_nvn_triangle_web PRIVATE LGP_TRIANGLE_IMAGE_ONLY=1)
        target_link_libraries(lgp_test_nvn_triangle_web PRIVATE lgp_gpu_webgpu lgp_lifted_headers)
        set(_shell "${PROJECT_SOURCE_DIR}/platform/web/shell.html")
        set_target_properties(lgp_test_nvn_triangle_web PROPERTIES SUFFIX ".html"
                              LINK_DEPENDS "${_shell};${_imported_image}")
        target_link_options(lgp_test_nvn_triangle_web PRIVATE -sALLOW_MEMORY_GROWTH=1 "--shell-file=${_shell}"
                            "--embed-file=${_imported_image}@/nvn_triangle.img")
    else()
        message(STATUS "tests/nvn: no native NVN guest build in ${LGP_NVN_GUEST_IMPORT_DIR}, "
                       "browser.nvn_triangle is not built")
    endif()
endif()

if(CMAKE_CROSSCOMPILING)
    return()
endif()
find_program(LGP_GUEST_CLANG NAMES clang clang-20 clang-19 clang-18 clang-17 DOC "clang used to build AArch64 test programs")
find_program(LGP_GUEST_LLD NAMES ld.lld ld.lld-20 ld.lld-19 ld.lld-18 ld.lld-17 DOC "ld.lld used to link AArch64 test programs")
if(NOT LGP_GUEST_CLANG OR NOT LGP_GUEST_LLD)
    message(WARNING "tests/nvn: clang or ld.lld not found, nvn.guest_clear is not built")
    return()
endif()

set(_indirect "${CMAKE_CURRENT_SOURCE_DIR}/../indirect")
set(_dir "${CMAKE_CURRENT_BINARY_DIR}/guest")
set(_cflags --target=aarch64-none-elf -O2 -fPIC -ffreestanding -fno-builtin -nostdlib -fno-exceptions -fno-rtti
            -fvisibility=hidden -funwind-tables)
set(_ldflags -pie --no-dynamic-linker --eh-frame-hdr -z norelro -z max-page-size=0x1000 --pack-dyn-relocs=none
             -T "${_indirect}/guest/link.ld")
add_custom_command(
    OUTPUT "${_dir}/nvn_clear.elf" "${_dir}/nvn_clear.nro"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${_dir}"
    COMMAND "${LGP_GUEST_CLANG}" ${_cflags} -c "${_indirect}/guest/crt0.S" -o "${_dir}/crt0.o"
    COMMAND "${LGP_GUEST_CLANG}" ${_cflags} -std=c++17 -c "${CMAKE_CURRENT_SOURCE_DIR}/guest/nvn_clear.cpp"
            -o "${_dir}/nvn_clear.o"
    COMMAND "${LGP_GUEST_LLD}" ${_ldflags} "${_dir}/crt0.o" "${_dir}/nvn_clear.o" -o "${_dir}/nvn_clear.elf"
    COMMAND "${Python3_EXECUTABLE}" "${_indirect}/elf2nro.py" "${_dir}/nvn_clear.elf" "${_dir}/nvn_clear.nro"
    DEPENDS "${_indirect}/guest/crt0.S" "${_indirect}/guest/link.ld" "${_indirect}/elf2nro.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/guest/nvn_clear.cpp"
    COMMENT "Building the NVN guest test program"
    VERBATIM)
set(_lifted "${CMAKE_CURRENT_BINARY_DIR}/generated/nvn_clear_lifted.cpp")
add_custom_command(
    OUTPUT "${_lifted}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
    COMMAND lgp_lift "${_dir}/nvn_clear.nro" -o "${_lifted}" --summary
    DEPENDS lgp_lift "${_dir}/nvn_clear.nro"
    COMMENT "Recompiling the NVN guest test program"
    VERBATIM)
add_executable(lgp_test_nvn_guest test_nvn_guest.cpp "${_lifted}")
target_link_libraries(lgp_test_nvn_guest PRIVATE lgp_gpu_nvn lgp_platform_common lgp_lifted_headers lgp_recomp_core)
add_test(NAME nvn.guest_clear COMMAND lgp_test_nvn_guest "${_dir}/nvn_clear.nro" "${_dir}/nvn_clear.elf")
set_tests_properties(nvn.guest_clear PROPERTIES LABELS "nvn")

# P4.1 part 2: the draw path. guest/nvn_triangle.cpp embeds own Maxwell test
# shaders (tests/gpu/shaders) as a generated header.
set(_shaders "${PROJECT_SOURCE_DIR}/tests/gpu/shaders")
set(_shader_header "${_dir}/nvn_test_shaders.h")
add_custom_command(
    OUTPUT "${_shader_header}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${_dir}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/guest/shaders_to_header.py" "${_shader_header}"
            "kVsTransform=${_shaders}/vs_transform.hex:nvnheader" "kFsTextured=${_shaders}/fs_textured.hex"
            "kCsMemory=${_shaders}/cs_memory.hex" "kCsLockstep=${_shaders}/cs_lockstep.hex"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/guest/shaders_to_header.py" "${_shaders}/vs_transform.hex"
            "${_shaders}/fs_textured.hex" "${_shaders}/cs_memory.hex" "${_shaders}/cs_lockstep.hex"
    COMMENT "Embedding the NVN test shaders"
    VERBATIM)
add_custom_command(
    OUTPUT "${_dir}/nvn_triangle.elf" "${_dir}/nvn_triangle.nro"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${_dir}"
    COMMAND "${LGP_GUEST_CLANG}" ${_cflags} -c "${_indirect}/guest/crt0.S" -o "${_dir}/crt0_triangle.o"
    COMMAND "${LGP_GUEST_CLANG}" ${_cflags} -std=c++17 -I "${_dir}" -c
            "${CMAKE_CURRENT_SOURCE_DIR}/guest/nvn_triangle.cpp" -o "${_dir}/nvn_triangle.o"
    COMMAND "${LGP_GUEST_LLD}" ${_ldflags} "${_dir}/crt0_triangle.o" "${_dir}/nvn_triangle.o"
            -o "${_dir}/nvn_triangle.elf"
    COMMAND "${Python3_EXECUTABLE}" "${_indirect}/elf2nro.py" "${_dir}/nvn_triangle.elf" "${_dir}/nvn_triangle.nro"
    DEPENDS "${_indirect}/guest/crt0.S" "${_indirect}/guest/link.ld" "${_indirect}/elf2nro.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/guest/nvn_triangle.cpp" "${_shader_header}"
    COMMENT "Building the NVN draw test program"
    VERBATIM)
set(_lifted_triangle "${CMAKE_CURRENT_BINARY_DIR}/generated/nvn_triangle_lifted.cpp")
add_custom_command(
    OUTPUT "${_lifted_triangle}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
    COMMAND lgp_lift "${_dir}/nvn_triangle.nro" -o "${_lifted_triangle}" --summary
    DEPENDS lgp_lift "${_dir}/nvn_triangle.nro"
    COMMENT "Recompiling the NVN draw test program"
    VERBATIM)
# For the WASM build (tests/web): the generated files of this native build.
add_custom_target(lgp_nvn_guest_programs ALL DEPENDS "${_lifted}" "${_lifted_triangle}")

# The same guest programs on the WebGPU backend (P4.4, P4.1 part 2).
if(TARGET lgp_gpu_webgpu)
    add_executable(lgp_test_nvn_triangle test_nvn_triangle.cpp "${_lifted_triangle}")
    target_link_libraries(lgp_test_nvn_triangle PRIVATE lgp_gpu_webgpu lgp_lifted_headers lgp_recomp_core)
    lgp_webgpu_copy_runtime(lgp_test_nvn_triangle)
    add_test(NAME nvn.guest_triangle_webgpu
             COMMAND lgp_test_nvn_triangle "${_dir}/nvn_triangle.nro" "${_dir}/nvn_triangle.elf")
    set_tests_properties(nvn.guest_triangle_webgpu PROPERTIES LABELS "nvn;webgpu" SKIP_RETURN_CODE 77)
    # The linked image for the WASM build (tests/web), and a run from it.
    add_custom_command(TARGET lgp_test_nvn_triangle POST_BUILD
                       COMMAND lgp_test_nvn_triangle --dump-image "${_dir}/nvn_triangle.nro" "${_dir}/nvn_triangle.elf"
                               "${_dir}/nvn_triangle.img"
                       VERBATIM)
    add_test(NAME nvn.guest_triangle_webgpu_image COMMAND lgp_test_nvn_triangle "${_dir}/nvn_triangle.img")
    set_tests_properties(nvn.guest_triangle_webgpu_image PROPERTIES LABELS "nvn;webgpu" SKIP_RETURN_CODE 77)
    # The same with the backend behind the guest (AsyncBackend).
    add_test(NAME nvn.guest_triangle_webgpu_async
             COMMAND lgp_test_nvn_triangle "${_dir}/nvn_triangle.nro" "${_dir}/nvn_triangle.elf")
    set_tests_properties(nvn.guest_triangle_webgpu_async PROPERTIES LABELS "nvn;webgpu" SKIP_RETURN_CODE 77
                         ENVIRONMENT "LGP_WEBGPU_ASYNC_EXECUTE=1")

    add_executable(lgp_test_nvn_guest_webgpu test_nvn_guest.cpp "${_lifted}")
    target_compile_definitions(lgp_test_nvn_guest_webgpu PRIVATE LGP_TEST_WEBGPU=1)
    target_link_libraries(lgp_test_nvn_guest_webgpu PRIVATE lgp_gpu_webgpu lgp_platform_common lgp_lifted_headers
                                                            lgp_recomp_core)
    lgp_webgpu_copy_runtime(lgp_test_nvn_guest_webgpu)
    add_test(NAME nvn.guest_clear_webgpu
             COMMAND lgp_test_nvn_guest_webgpu "${_dir}/nvn_clear.nro" "${_dir}/nvn_clear.elf")
    set_tests_properties(nvn.guest_clear_webgpu PROPERTIES LABELS "nvn;webgpu" SKIP_RETURN_CODE 77)
endif()

# lgp_pipeline_seed (gpu/webgpu/tools): catalog, info and merge on own test
# shaders; no GPU needed.
if(TARGET lgp_pipeline_seed)
    add_test(NAME nvn.pipeline_seed_tool
             COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_pipeline_seed.py"
                     "$<TARGET_FILE:lgp_pipeline_seed>" "${PROJECT_SOURCE_DIR}/tests/gpu/shaders" "${PROJECT_SOURCE_DIR}")
    set_tests_properties(nvn.pipeline_seed_tool PROPERTIES LABELS "nvn;tools")
endif()
