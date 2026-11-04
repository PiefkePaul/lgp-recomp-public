# lgp_recomp_codegen: analysis (core) -> lifter -> C++ per module set, with the
# dispatch entries of ADR-0004 (P2.5). lgp_lift is its command line.
add_library(lgp_recomp_codegen STATIC src/codegen.cpp)
target_include_directories(lgp_recomp_codegen PUBLIC include)
target_link_libraries(lgp_recomp_codegen PUBLIC lgp_recomp_core lgp_recomp_lifter)
if(MSVC)
    target_compile_options(lgp_recomp_codegen PRIVATE /W4)
else()
    target_compile_options(lgp_recomp_codegen PRIVATE -Wall -Wextra)
endif()

add_executable(lgp_lift src/lift_main.cpp)
target_link_libraries(lgp_lift PRIVATE lgp_recomp_codegen lgp_mod_file)
if(NOT MSVC)
    target_compile_options(lgp_lift PRIVATE -Wall -Wextra)
endif()
