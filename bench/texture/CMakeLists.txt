# CPU BCn decoding speed, reference against fast decoder (gpu/texture/bc.hpp;
# docs/knowledge/android-ndk-build.md). Times are only meaningful in a Release
# build; ctest runs a small smoke pass (labels "bench", "texture") that checks
# both decoders write the same bytes.
add_executable(lgp_bc_bench bc_bench.cpp)
target_link_libraries(lgp_bc_bench PRIVATE lgp_gpu_texture)
add_test(NAME bench.bc_decode COMMAND lgp_bc_bench --smoke)
set_tests_properties(bench.bc_decode PROPERTIES LABELS "bench;texture")
