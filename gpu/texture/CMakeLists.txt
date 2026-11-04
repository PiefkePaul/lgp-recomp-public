# Texture decoding on the CPU: ASTC (P4.3) for hosts without
# TextureCompressionASTC, BC1-BC5/BC7 (P7.3) for hosts without
# TextureCompressionBC (mobile GPUs). No dependencies, builds for every target.
add_library(lgp_gpu_texture STATIC src/astc.cpp src/bc.cpp src/parallel.cpp)
find_package(Threads REQUIRED)
target_link_libraries(lgp_gpu_texture PUBLIC Threads::Threads)
target_include_directories(lgp_gpu_texture PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(lgp_gpu_texture PUBLIC cxx_std_20)
if(MSVC)
    target_compile_options(lgp_gpu_texture PRIVATE /W4)
else()
    target_compile_options(lgp_gpu_texture PRIVATE -Wall -Wextra)
endif()
