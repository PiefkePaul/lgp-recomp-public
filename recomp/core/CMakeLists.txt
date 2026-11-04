# lgp_recomp_core: NSO/NRO loader, link step, function discovery and CFG.
# No third-party dependencies so the Emscripten build picks it up unchanged.
add_library(lgp_recomp_core STATIC
    src/lz4.cpp
    src/loader.cpp
    src/eh_frame.cpp
    src/link.cpp
    src/a64_cf.cpp
    src/lift_input.cpp
    src/jt_decode.cpp
    src/cfg.cpp
    src/jump_table.cpp
    src/config.cpp
    src/elf_symbols.cpp
    src/json.cpp
    src/native_census.cpp
    src/native_rewrite.cpp
    src/native_image.cpp
    src/native_cache.cpp)
target_include_directories(lgp_recomp_core PUBLIC include)
# classify() and the lifter adapter use the lifter's decoder (p2-interfaces.md, sections 2 and 4a).
target_link_libraries(lgp_recomp_core PUBLIC lgp_recomp_lifter)
target_compile_features(lgp_recomp_core PUBLIC cxx_std_20)
# The loader reports malformed input with exceptions (FormatError); Emscripten
# builds use native Wasm exceptions project-wide (root CMakeLists.txt).
if(MSVC)
    target_compile_options(lgp_recomp_core PRIVATE /W4)
else()
    target_compile_options(lgp_recomp_core PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion)
endif()
