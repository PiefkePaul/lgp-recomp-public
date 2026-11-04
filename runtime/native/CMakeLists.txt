# Native execution of the rewritten original code (docs/analysis/56, stage C):
# the executor that runs guest threads directly on an AArch64 host and the
# host side of the gate (lgp/recomp/native_gate.hpp). Builds everywhere;
# lgp::rt::native::available() is false on other hosts.
add_library(lgp_runtime_native STATIC src/native.cpp)
target_include_directories(lgp_runtime_native PUBLIC include)
# native_gate.hpp is header-only and free of recompiler dependencies.
target_include_directories(lgp_runtime_native PRIVATE "${PROJECT_SOURCE_DIR}/recomp/core/include")
target_link_libraries(lgp_runtime_native PUBLIC lgp_runtime_core)
if(MSVC)
    target_compile_options(lgp_runtime_native PRIVATE /W4)
else()
    target_compile_options(lgp_runtime_native PRIVATE -Wall -Wextra)
endif()
