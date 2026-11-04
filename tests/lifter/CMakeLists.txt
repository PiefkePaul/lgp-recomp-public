# Lifter tests (roadmap P2.3), ctest label "lifter".

# Decoder unit tests; also built by the WebAssembly preset and run in Node.
add_executable(lgp_test_a64_decode test_a64_decode.cpp)
target_link_libraries(lgp_test_a64_decode PRIVATE lgp_recomp_lifter)
add_test(NAME lifter.a64_decode COMMAND lgp_test_a64_decode)
set_tests_properties(lifter.a64_decode PROPERTIES LABELS "lifter")

# FP estimate steps against the Arm ARM pseudocode (no Unicorn needed).
add_executable(lgp_test_fp_estimate test_fp_estimate.cpp)
target_link_libraries(lgp_test_fp_estimate PRIVATE lgp_lifted_headers)
add_test(NAME lifter.fp_estimate COMMAND lgp_test_fp_estimate)
set_tests_properties(lifter.fp_estimate PROPERTIES LABELS "lifter")

# Generated differential cases: the generator runs on the build host, so it is
# skipped when cross-compiling (Emscripten).
if(NOT CMAKE_CROSSCOMPILING)
    set(LGP_LIFTER_SHARDS 8 CACHE STRING "Number of generated lifter test shards (parallel compilation)")
    set(LGP_LIFTER_CASES_PER_GROUP 200 CACHE STRING "Random cases per encoding group")
    set(LGP_LIFTER_SEED 0x4C475033 CACHE STRING "Seed of the lifter case generator")

    add_executable(lgp_lifter_casegen casegen.cpp)
    target_link_libraries(lgp_lifter_casegen PRIVATE lgp_recomp_lifter lgp_lifted_headers)
    target_include_directories(lgp_lifter_casegen PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

    set(_gen_dir "${CMAKE_CURRENT_BINARY_DIR}/generated")
    file(MAKE_DIRECTORY "${_gen_dir}")
    set(_gen_sources "${_gen_dir}/index.cpp")
    math(EXPR _last_shard "${LGP_LIFTER_SHARDS} - 1")
    foreach(_s RANGE ${_last_shard})
        list(APPEND _gen_sources "${_gen_dir}/shard_${_s}.cpp")
    endforeach()
    add_custom_command(
        OUTPUT ${_gen_sources}
        COMMAND lgp_lifter_casegen "${_gen_dir}" ${LGP_LIFTER_SHARDS} ${LGP_LIFTER_CASES_PER_GROUP} ${LGP_LIFTER_SEED}
        DEPENDS lgp_lifter_casegen
        COMMENT "Generating and lifting differential test cases"
        VERBATIM)

    if(LGP_ENABLE_UNICORN)
        add_executable(lgp_test_lifter_difftest
            test_lifter_difftest.cpp
            recompiled_executor.cpp
            ${_gen_sources})
        target_include_directories(lgp_test_lifter_difftest PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
        target_compile_definitions(lgp_test_lifter_difftest PRIVATE LGP_RUNTIME_HEADER="guest_stub.hpp")
        target_link_libraries(lgp_test_lifter_difftest PRIVATE lgp_recomp_lifter lgp_lifted_headers lgp_difftest_unicorn)
        add_test(NAME lifter.difftest_unicorn COMMAND lgp_test_lifter_difftest)
        set_tests_properties(lifter.difftest_unicorn PROPERTIES LABELS "lifter;difftest;unicorn" TIMEOUT 600)

        # Decoder vs. Unicorn on random words: which encodings are UNDEFINED.
        add_executable(lgp_test_decode_validity test_decode_validity.cpp)
        target_include_directories(lgp_test_decode_validity PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
        target_link_libraries(lgp_test_decode_validity PRIVATE lgp_recomp_lifter lgp_difftest_unicorn)
        add_test(NAME lifter.decode_validity COMMAND lgp_test_decode_validity 200000)
        set_tests_properties(lifter.decode_validity PROPERTIES LABELS "lifter;unicorn" TIMEOUT 600)
    endif()
endif()
