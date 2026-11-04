# NVN-HLE renderer on WebGPU (ADR-0003, P4.4): executes the HLE's command
# lists with webgpu.h and presents through lgp_platform_webgpu.
add_library(lgp_gpu_webgpu STATIC src/webgpu_backend.cpp src/pipeline_cache.cpp)
# The pipeline cache (derived from game shaders) refuses paths inside the repository.
target_compile_definitions(lgp_gpu_webgpu PRIVATE "LGP_SOURCE_DIR=\"${PROJECT_SOURCE_DIR}\"")
target_include_directories(lgp_gpu_webgpu PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(lgp_gpu_webgpu PUBLIC lgp_gpu_nvn lgp_platform_webgpu PRIVATE lgp_gpu_shader lgp_gpu_texture)
if(MSVC)
    target_compile_options(lgp_gpu_webgpu PRIVATE /W4)
else()
    target_compile_options(lgp_gpu_webgpu PRIVATE -Wall -Wextra)
endif()

# Host tool: pipeline seeds (info, merge, static program catalog from the
# game's shader containers; outputs stay in $LGP_PRIVATE_DIR).
if(NOT EMSCRIPTEN AND NOT ANDROID AND NOT IOS AND NOT CMAKE_SYSTEM_NAME STREQUAL "tvOS")
    add_executable(lgp_pipeline_seed tools/pipeline_seed.cpp)
    target_compile_definitions(lgp_pipeline_seed PRIVATE "LGP_SOURCE_DIR=\"${PROJECT_SOURCE_DIR}\"")
    target_link_libraries(lgp_pipeline_seed PRIVATE lgp_gpu_webgpu lgp_gpu_nvn)
    lgp_webgpu_copy_runtime(lgp_pipeline_seed)
endif()

# Demo: animated clears through the backend in a window (desktop, SDL3) or
# the page's canvas (web, lgp_webgpu_demo.html; serve it with
# tools/web/serve.py, which sends the COOP/COEP headers threads need).
lgp_add_sdl_program(lgp_webgpu_demo demo/webgpu_demo.cpp)
target_link_libraries(lgp_webgpu_demo PRIVATE lgp_gpu_webgpu)
if(TARGET lgp_platform_desktop)
    target_link_libraries(lgp_webgpu_demo PRIVATE lgp_platform_desktop)
    target_compile_definitions(lgp_webgpu_demo PRIVATE LGP_DEMO_SDL=1)
endif()
if(EMSCRIPTEN)
    set(_shell "${PROJECT_SOURCE_DIR}/platform/web/shell.html")
    set_target_properties(lgp_webgpu_demo PROPERTIES SUFFIX ".html" LINK_DEPENDS "${_shell}")
    target_link_options(lgp_webgpu_demo PRIVATE -sALLOW_MEMORY_GROWTH=1 "--shell-file=${_shell}")
endif()
lgp_webgpu_copy_runtime(lgp_webgpu_demo)
# Android (preset android-app, P7.3): the demo as an app, a Vulkan check without game data.
lgp_add_android_apk(lgp_webgpu_demo PACKAGE org.lgp.recomp.demo LABEL "LGP WebGPU-Demo")
