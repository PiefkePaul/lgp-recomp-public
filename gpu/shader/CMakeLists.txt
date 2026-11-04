# Maxwell shader translator (P4 spike): decoder -> IR -> WGSL.
add_library(lgp_gpu_shader STATIC
    src/maxwell.cpp
    src/sph.cpp
    src/lift.cpp
    src/wgsl.cpp)
target_include_directories(lgp_gpu_shader PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(lgp_gpu_shader PUBLIC cxx_std_20)
if(NOT MSVC)
    target_compile_options(lgp_gpu_shader PRIVATE -Wall -Wextra)
endif()

# Offline tool: shader binary (SPH + code) -> WGSL or disassembly.
add_executable(maxwell2wgsl tools/maxwell2wgsl.cpp)
target_link_libraries(maxwell2wgsl PRIVATE lgp_gpu_shader)
