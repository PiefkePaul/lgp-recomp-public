# P2.8 milestone: every homebrew fixture (tests/fixtures, devkitA64/libnx)
# runs recompiled with the same end state as under Unicorn. Label "fixtures".
#
# Native (LGP_BUILD_FIXTURES, i.e. with devkitPro): each fixture is lifted
# with lgp_lift, linked against the runtime and run twice on the same kernel
# setup, once with Unicorn executing the guest threads and once recompiled.
# The Unicorn end state is written to <fixture dir>/expected/.
#
# Imported (LGP_FIXTURE_IMPORT_DIR, used for the WebAssembly smoke test in
# CI): a directory with <name>.<nso|nro>, <name>_<fmt>_lifted.cpp and
# <name>_<fmt>.expected from a native run; the recompiled run is compared
# with the expected file. Fixture binaries and generated code never leave
# the build/CI artifacts.
set(LGP_FIXTURES basic threads)
set(LGP_FIXTURE_FORMATS nso nro)
set(LGP_FIXTURE_IMPORT_DIR "" CACHE PATH "Directory with fixture modules, lifted code and expected end states from a native run")

add_custom_target(lgp_fixture_runners)

function(lgp_fixture_runner target lifted)
    add_executable(${target} test_fixture_run.cpp fixture_process.cpp "${lifted}")
    add_dependencies(lgp_fixture_runners ${target})
    target_link_libraries(${target} PRIVATE lgp_runtime_core lgp_lifted_headers lgp_recomp_core)
    if(EMSCRIPTEN)
        # The runner reads the module and the expected file from the host file system.
        target_link_options(${target} PRIVATE -sNODERAWFS=1 -sALLOW_MEMORY_GROWTH=1 -sMAXIMUM_MEMORY=4GB)
    endif()
endfunction()

if(LGP_BUILD_FIXTURES AND NOT CMAKE_CROSSCOMPILING)
    if(NOT LGP_ENABLE_UNICORN)
        message(WARNING "tests/homebrew: LGP_BUILD_FIXTURES without LGP_ENABLE_UNICORN, the P2.8 fixture runs need the oracle")
        add_test(NAME fixtures.run.oracle_missing COMMAND "${CMAKE_COMMAND}" -E false)
        set_tests_properties(fixtures.run.oracle_missing PROPERTIES LABELS "fixtures")
        return()
    endif()
    set(_expected_dir "${LGP_FIXTURE_DIR}/expected")
    file(MAKE_DIRECTORY "${_expected_dir}")
    foreach(name IN LISTS LGP_FIXTURES)
        foreach(fmt IN LISTS LGP_FIXTURE_FORMATS)
            set(_module "${LGP_FIXTURE_DIR}/${name}.${fmt}")
            set(_lifted "${CMAKE_CURRENT_BINARY_DIR}/generated/${name}_${fmt}_lifted.cpp")
            add_custom_command(
                OUTPUT "${_lifted}"
                COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
                COMMAND lgp_lift "${_module}" -o "${_lifted}" --summary
                DEPENDS lgp_lift "${_module}"
                COMMENT "Recompiling fixture ${name}.${fmt}"
                VERBATIM)
            set(_target lgp_fixture_run_${name}_${fmt})
            lgp_fixture_runner(${_target} "${_lifted}")
            target_sources(${_target} PRIVATE unicorn_guest.cpp)
            target_compile_definitions(${_target} PRIVATE LGP_FIXTURE_ORACLE=1)
            target_link_libraries(${_target} PRIVATE lgp_difftest_unicorn)
            add_dependencies(${_target} lgp_fixtures)
            add_test(NAME fixtures.run.${name}_${fmt}
                     COMMAND ${_target} "${_module}" --oracle --write-expected "${_expected_dir}/${name}_${fmt}.expected")
            set_tests_properties(fixtures.run.${name}_${fmt} PROPERTIES LABELS "fixtures;unicorn" TIMEOUT 600)
        endforeach()
    endforeach()
elseif(LGP_FIXTURE_IMPORT_DIR)
    foreach(name IN LISTS LGP_FIXTURES)
        foreach(fmt IN LISTS LGP_FIXTURE_FORMATS)
            set(_module "${LGP_FIXTURE_IMPORT_DIR}/${name}.${fmt}")
            set(_lifted "${LGP_FIXTURE_IMPORT_DIR}/${name}_${fmt}_lifted.cpp")
            set(_expected "${LGP_FIXTURE_IMPORT_DIR}/${name}_${fmt}.expected")
            if(NOT EXISTS "${_module}" OR NOT EXISTS "${_lifted}" OR NOT EXISTS "${_expected}")
                message(FATAL_ERROR "LGP_FIXTURE_IMPORT_DIR: ${name}.${fmt}, its lifted code or expected state is missing")
            endif()
            set(_target lgp_fixture_run_${name}_${fmt})
            lgp_fixture_runner(${_target} "${_lifted}")
            add_test(NAME fixtures.run.${name}_${fmt} COMMAND ${_target} "${_module}" --expected "${_expected}")
            set_tests_properties(fixtures.run.${name}_${fmt} PROPERTIES LABELS "fixtures" TIMEOUT 600)
        endforeach()
    endforeach()
endif()

# Stage C of docs/analysis/56: the fixtures as rewritten original code, run
# natively (lgp::rt::native) and compared with the original under Unicorn
# (test_fixture_native.cpp). Labels "fixtures;native".
#
# LGP_FIXTURE_NATIVE_DIR: the fixture modules (default with devkitPro: the
# fixture build). With Unicorn the oracle runs here; on an AArch64 host the
# native run is compared with it directly, elsewhere the oracle's end state is
# written to <dir>/expected/<name>_<fmt>.native.expected. Without Unicorn (a
# cross build, e.g. AArch64 Linux under qemu) the native run compares with
# those files.
if(LGP_BUILD_FIXTURES AND NOT CMAKE_CROSSCOMPILING)
    set(_lgp_native_dir_default "${LGP_FIXTURE_DIR}")
else()
    set(_lgp_native_dir_default "")
endif()
set(LGP_FIXTURE_NATIVE_DIR "${_lgp_native_dir_default}" CACHE PATH
    "Directory with fixture modules for the native-execution tests (expected/*.native.expected for cross builds)")
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
    set(_native_host ON)
else()
    set(_native_host OFF)
endif()
if((LGP_FIXTURE_NATIVE_DIR OR _native_host) AND NOT EMSCRIPTEN AND NOT MSVC)
    add_executable(lgp_fixture_native test_fixture_native.cpp fixture_process.cpp)
    target_link_libraries(lgp_fixture_native PRIVATE lgp_runtime_native lgp_runtime_core lgp_lifted_headers lgp_recomp_core)
    set(_oracle OFF)
    if(LGP_ENABLE_UNICORN AND NOT CMAKE_CROSSCOMPILING)
        set(_oracle ON)
        target_sources(lgp_fixture_native PRIVATE unicorn_guest.cpp)
        target_compile_definitions(lgp_fixture_native PRIVATE LGP_FIXTURE_ORACLE=1)
        target_link_libraries(lgp_fixture_native PRIVATE lgp_difftest_unicorn)
    endif()
    if(LGP_BUILD_FIXTURES AND NOT CMAKE_CROSSCOMPILING)
        add_dependencies(lgp_fixture_native lgp_fixtures)
    endif()
    if(_native_host)
        # Faults and the exit kick in native code, on a synthetic module.
        add_test(NAME native.signals COMMAND lgp_fixture_native --signals)
        set_tests_properties(native.signals PROPERTIES LABELS "native;runtime" TIMEOUT 120)
    endif()
endif()
if(LGP_FIXTURE_NATIVE_DIR AND TARGET lgp_fixture_native)
    file(MAKE_DIRECTORY "${LGP_FIXTURE_NATIVE_DIR}/expected")
    foreach(name IN LISTS LGP_FIXTURES)
        foreach(fmt IN LISTS LGP_FIXTURE_FORMATS)
            set(_module "${LGP_FIXTURE_NATIVE_DIR}/${name}.${fmt}")
            set(_expected "${LGP_FIXTURE_NATIVE_DIR}/expected/${name}_${fmt}.native.expected")
            if(_oracle AND _native_host)
                add_test(NAME fixtures.native.${name}_${fmt} COMMAND lgp_fixture_native "${_module}" --oracle --native)
                set_tests_properties(fixtures.native.${name}_${fmt} PROPERTIES LABELS "fixtures;native;unicorn" TIMEOUT 600)
            elseif(_oracle)
                add_test(NAME fixtures.native_oracle.${name}_${fmt}
                         COMMAND lgp_fixture_native "${_module}" --oracle --write-expected "${_expected}")
                set_tests_properties(fixtures.native_oracle.${name}_${fmt} PROPERTIES LABELS "fixtures;native;unicorn" TIMEOUT 600)
            elseif(_native_host)
                add_test(NAME fixtures.native.${name}_${fmt} COMMAND lgp_fixture_native "${_module}" --native --expected "${_expected}")
                set_tests_properties(fixtures.native.${name}_${fmt} PROPERTIES LABELS "fixtures;native" TIMEOUT 600)
            endif()
        endforeach()
    endforeach()
endif()
