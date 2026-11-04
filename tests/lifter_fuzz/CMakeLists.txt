# Lifter fuzzer (ctest label "lifter_fuzz"): random mixed sequences of integer,
# memory, FP and Advanced SIMD instructions, Unicorn vs. lifted C++.
# docs/knowledge/lifter-fuzzing.md describes the workflow.
if(NOT CMAKE_CROSSCOMPILING AND LGP_ENABLE_UNICORN)
    set(LGP_LIFTER_FUZZ_SEED 0x4C474658 CACHE STRING "Seed of the deterministic lifter fuzz run in ctest")
    set(LGP_LIFTER_FUZZ_CASES 3000 CACHE STRING "Random cases of the deterministic lifter fuzz run in ctest")
    set(LGP_LIFTER_FUZZ_SHARDS 6 CACHE STRING "Generated source files of the lifter fuzz run (parallel compilation)")
    set(LGP_LIFTER_FUZZ_LONG_SECONDS 0 CACHE STRING
        "Run time of the optional long fuzz test lifter_fuzz.long in seconds (0: no such test)")

    set(_lifter_tests "${CMAKE_CURRENT_SOURCE_DIR}/../lifter")
    set(_simd_tests "${CMAKE_CURRENT_SOURCE_DIR}/../lifter_simd")
    set(_regressions "${CMAKE_CURRENT_SOURCE_DIR}/regressions.txt")
    file(GLOB _group_files CONFIGURE_DEPENDS "${_simd_tests}/groups_*.inc")

    add_library(lgp_lifter_fuzz_gen STATIC fuzz_gen.cpp)
    target_include_directories(lgp_lifter_fuzz_gen PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" "${_lifter_tests}" "${_simd_tests}")
    target_link_libraries(lgp_lifter_fuzz_gen PUBLIC lgp_recomp_lifter lgp_lifted_headers)

    add_executable(lgp_lifter_fuzzgen fuzzgen.cpp)
    target_link_libraries(lgp_lifter_fuzzgen PRIVATE lgp_lifter_fuzz_gen)

    set(_gen_dir "${CMAKE_CURRENT_BINARY_DIR}/generated")
    file(MAKE_DIRECTORY "${_gen_dir}")
    set(_gen_sources "${_gen_dir}/index.cpp" "${_gen_dir}/regressions.cpp")
    math(EXPR _last_shard "${LGP_LIFTER_FUZZ_SHARDS} - 1")
    foreach(_s RANGE ${_last_shard})
        list(APPEND _gen_sources "${_gen_dir}/shard_${_s}.cpp")
    endforeach()
    add_custom_command(
        OUTPUT ${_gen_sources}
        COMMAND lgp_lifter_fuzzgen "${_gen_dir}" ${LGP_LIFTER_FUZZ_SHARDS} ${LGP_LIFTER_FUZZ_SEED}
                ${LGP_LIFTER_FUZZ_CASES} 16 "${_regressions}"
        DEPENDS lgp_lifter_fuzzgen "${_regressions}" ${_group_files}
        COMMENT "Generating and lifting lifter fuzz cases"
        VERBATIM)
    # Lifted code is optimised as in a game build, also in Debug: undefined
    # behaviour in emitted code often only shows up under optimisation.
    set_source_files_properties(${_gen_sources} PROPERTIES COMPILE_OPTIONS "-O2")

    add_library(lgp_lifter_fuzz_run STATIC fuzz_run.cpp "${_lifter_tests}/recompiled_executor.cpp")
    target_include_directories(lgp_lifter_fuzz_run PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" "${_lifter_tests}" "${_simd_tests}")
    target_compile_definitions(lgp_lifter_fuzz_run PUBLIC LGP_RUNTIME_HEADER="guest_stub.hpp")
    target_link_libraries(lgp_lifter_fuzz_run PUBLIC lgp_lifter_fuzz_gen lgp_difftest_unicorn)

    add_executable(lgp_test_lifter_fuzz test_lifter_fuzz.cpp ${_gen_sources})
    target_link_libraries(lgp_test_lifter_fuzz PRIVATE lgp_lifter_fuzz_run)
    add_test(NAME lifter_fuzz.difftest COMMAND lgp_test_lifter_fuzz)
    set_tests_properties(lifter_fuzz.difftest PROPERTIES LABELS "lifter;lifter_fuzz;difftest;unicorn" TIMEOUT 600)

    # Long runs: compiles batches at run time with the host compiler and loads
    # them (POSIX dlopen); the lifted code calls back into the test runtime, so
    # the executable exports its symbols.
    if(UNIX AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        add_executable(lgp_lifter_fuzz lifter_fuzz.cpp)
        target_link_libraries(lgp_lifter_fuzz PRIVATE lgp_lifter_fuzz_run ${CMAKE_DL_LIBS})
        set_target_properties(lgp_lifter_fuzz PROPERTIES ENABLE_EXPORTS ON)
        get_target_property(_lifted_inc lgp_lifted_headers INTERFACE_INCLUDE_DIRECTORIES)
        set(_inc_flags "")
        foreach(_d ${_lifted_inc} "${_lifter_tests}" "${_simd_tests}")
            string(APPEND _inc_flags " -I${_d}")
        endforeach()
        target_compile_definitions(lgp_lifter_fuzz PRIVATE
            LGP_FUZZ_CXX="${CMAKE_CXX_COMPILER}"
            LGP_FUZZ_INCLUDES="${_inc_flags}")

        # Smoke test of the run-time compile path, plus the regression file replayed through it.
        add_test(NAME lifter_fuzz.driver
                 COMMAND lgp_lifter_fuzz --seed 7 --cases 60 --batch 60 --replay "${_regressions}"
                         --work "${CMAKE_CURRENT_BINARY_DIR}/work")
        add_test(NAME lifter_fuzz.driver_random
                 COMMAND lgp_lifter_fuzz --seed 7 --cases 60 --batch 60 --work "${CMAKE_CURRENT_BINARY_DIR}/work")
        # Release code shape (no precise fault pc, AAPCS64 at calls): registers stay
        # in locals between calls. The regressions run in this shape as well.
        add_test(NAME lifter_fuzz.driver_release
                 COMMAND lgp_lifter_fuzz --seed 8 --cases 200 --batch 200 --release-shape --replay "${_regressions}"
                         --work "${CMAKE_CURRENT_BINARY_DIR}/work")
        set_tests_properties(lifter_fuzz.driver lifter_fuzz.driver_random lifter_fuzz.driver_release PROPERTIES
            LABELS "lifter;lifter_fuzz;unicorn" TIMEOUT 600)
        if(LGP_LIFTER_FUZZ_LONG_SECONDS GREATER 0)
            add_test(NAME lifter_fuzz.long
                     COMMAND lgp_lifter_fuzz --seed ${LGP_LIFTER_FUZZ_SEED} --seconds ${LGP_LIFTER_FUZZ_LONG_SECONDS}
                             --work "${CMAKE_CURRENT_BINARY_DIR}/work")
            math(EXPR _long_timeout "${LGP_LIFTER_FUZZ_LONG_SECONDS} + 900")
            set_tests_properties(lifter_fuzz.long PROPERTIES LABELS "lifter_fuzz_long;unicorn" TIMEOUT ${_long_timeout})
        endif()
    endif()
endif()
