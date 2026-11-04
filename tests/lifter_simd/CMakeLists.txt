# SIMD&FP lifter tests (roadmap P2.4), ctest label "lifter_simd".
# Reuses the recompiled executor and the test runtime of tests/lifter (P2.3).

if(NOT CMAKE_CROSSCOMPILING)
    set(LGP_SIMD_SHARDS 8 CACHE STRING "Number of generated SIMD&FP lifter test shards (parallel compilation)")
    set(LGP_SIMD_CASES_PER_GROUP 120 CACHE STRING "Random SIMD&FP cases per encoding group")
    set(LGP_SIMD_SEED 0x4C475034 CACHE STRING "Seed of the SIMD&FP case generator")

    set(_lifter_tests "${CMAKE_CURRENT_SOURCE_DIR}/../lifter")
    file(GLOB _group_files CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/groups_*.inc")

    add_executable(lgp_simd_casegen casegen_simd.cpp)
    target_link_libraries(lgp_simd_casegen PRIVATE lgp_recomp_lifter lgp_lifted_headers)
    target_include_directories(lgp_simd_casegen PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${_lifter_tests}")

    set(_gen_dir "${CMAKE_CURRENT_BINARY_DIR}/generated")
    file(MAKE_DIRECTORY "${_gen_dir}")
    set(_gen_sources "${_gen_dir}/index.cpp")
    math(EXPR _last_shard "${LGP_SIMD_SHARDS} - 1")
    foreach(_s RANGE ${_last_shard})
        list(APPEND _gen_sources "${_gen_dir}/shard_${_s}.cpp")
    endforeach()
    add_custom_command(
        OUTPUT ${_gen_sources}
        COMMAND lgp_simd_casegen "${_gen_dir}" ${LGP_SIMD_SHARDS} ${LGP_SIMD_CASES_PER_GROUP} ${LGP_SIMD_SEED}
        DEPENDS lgp_simd_casegen ${_group_files}
        COMMENT "Generating and lifting SIMD&FP differential test cases"
        VERBATIM)

    # Soft-float path of fp.hpp against the host FPU under fesetround() (no Unicorn needed).
    add_executable(lgp_test_fp_soft test_fp_soft.cpp)
    target_include_directories(lgp_test_fp_soft PRIVATE "${_lifter_tests}")
    target_link_libraries(lgp_test_fp_soft PRIVATE lgp_lifted_headers)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(lgp_test_fp_soft PRIVATE -frounding-math)
    endif()
    add_test(NAME lifter_simd.fp_soft COMMAND lgp_test_fp_soft 300000)
    set_tests_properties(lifter_simd.fp_soft PROPERTIES LABELS "lifter;lifter_simd" TIMEOUT 600)

    if(LGP_ENABLE_UNICORN)
        add_executable(lgp_test_simd_difftest
            test_simd_difftest.cpp
            "${_lifter_tests}/recompiled_executor.cpp"
            ${_gen_sources})
        target_include_directories(lgp_test_simd_difftest PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${_lifter_tests}")
        target_compile_definitions(lgp_test_simd_difftest PRIVATE LGP_RUNTIME_HEADER="guest_stub.hpp")
        target_link_libraries(lgp_test_simd_difftest PRIVATE lgp_recomp_lifter lgp_lifted_headers lgp_difftest_unicorn)
        add_test(NAME lifter_simd.difftest_unicorn COMMAND lgp_test_simd_difftest)
        set_tests_properties(lifter_simd.difftest_unicorn PROPERTIES LABELS "lifter;lifter_simd;difftest;unicorn" TIMEOUT 900)

        add_executable(lgp_test_simd_validity test_simd_validity.cpp)
        target_include_directories(lgp_test_simd_validity PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${_lifter_tests}")
        target_link_libraries(lgp_test_simd_validity PRIVATE lgp_recomp_lifter lgp_difftest_unicorn)
        add_test(NAME lifter_simd.decode_validity COMMAND lgp_test_simd_validity 300000 --strict)
        set_tests_properties(lifter_simd.decode_validity PROPERTIES LABELS "lifter;lifter_simd;unicorn" TIMEOUT 900)
    endif()
endif()
