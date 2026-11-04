# Function-level differential test (ADR-0006 stage 3): single functions of a
# module run natively on the hypervisor and recompiled, from random register
# and memory states. See README.md in this directory.

add_executable(lgp_fn_select select_main.cpp)
target_link_libraries(lgp_fn_select PRIVATE lgp_recomp_codegen)

if(NOT LGP_ENABLE_HV)
    return()
endif()

# lgp_add_fn_difftest(<target> <lifted.cpp…>): the runner linked with one
# lgp_fn_select output (the hypervisor side needs macOS on Apple Silicon).
# Lifted code is built with -O1 like the game build (06-recomp-gegen-main.md).
function(lgp_add_fn_difftest target)
    add_executable(${target} "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/runner_main.cpp" ${ARGN})
    set_source_files_properties(${ARGN} PROPERTIES COMPILE_OPTIONS "-O1;-w")
    # The runner moves megabytes per case; unoptimised it dominates the run.
    set_source_files_properties("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/runner_main.cpp" TARGET_DIRECTORY ${target}
                                PROPERTIES COMPILE_OPTIONS "-O2")
    target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/..")
    target_link_libraries(${target} PRIVATE lgp_difftest_hv lgp_runtime_core lgp_recomp_core lgp_lifted_headers)
    lgp_hv_sign(${target})
endfunction()

set(LGP_FN_DIFFTEST_LIFTED "" CACHE PATH
    "lgp_fn_select output directory (lifted*.cpp) to build lgp_fn_difftest against; for a game it lies in LGP_PRIVATE_DIR")
if(LGP_FN_DIFFTEST_LIFTED)
    file(GLOB _fn_lifted CONFIGURE_DEPENDS "${LGP_FN_DIFFTEST_LIFTED}/lifted*.cpp")
    if(NOT _fn_lifted)
        message(FATAL_ERROR "LGP_FN_DIFFTEST_LIFTED: no lifted*.cpp in ${LGP_FN_DIFFTEST_LIFTED}")
    endif()
    lgp_add_fn_difftest(lgp_fn_difftest ${_fn_lifted})
endif()

# Self-test without game data: own AArch64 functions (guest/functions.c),
# built like the P2.5 program (host clang + ld.lld, NRO via elf2nro.py),
# selected and lifted at build time, compared on the hypervisor. Label "hv".
find_program(LGP_GUEST_CLANG NAMES clang clang-20 clang-19 clang-18 clang-17 DOC "clang used to build AArch64 test programs")
find_program(LGP_GUEST_LLD NAMES ld.lld ld.lld-20 ld.lld-19 ld.lld-18 ld.lld-17 DOC "ld.lld used to link AArch64 test programs")
find_package(Python3 3.9 COMPONENTS Interpreter)
if(NOT LGP_GUEST_CLANG OR NOT LGP_GUEST_LLD OR NOT Python3_FOUND)
    message(WARNING "tests/difftest/fn: clang, ld.lld or Python missing, difftest.fn_selftest is not built")
    return()
endif()

set(_indirect "${CMAKE_CURRENT_SOURCE_DIR}/../../indirect")
set(_dir "${CMAKE_CURRENT_BINARY_DIR}/selftest")
set(_cflags --target=aarch64-none-elf -march=armv8-a+crc -O2 -fno-math-errno -fPIC -ffreestanding -fno-builtin -nostdlib
            -fno-exceptions -fvisibility=hidden -funwind-tables)
set(_ldflags -pie --no-dynamic-linker --eh-frame-hdr -z norelro -z max-page-size=0x1000 --pack-dyn-relocs=none
             -T "${_indirect}/guest/link.ld")
add_custom_command(
    OUTPUT "${_dir}/functions.nro"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${_dir}"
    COMMAND "${LGP_GUEST_CLANG}" ${_cflags} -c "${_indirect}/guest/crt0.S" -o "${_dir}/crt0.o"
    COMMAND "${LGP_GUEST_CLANG}" ${_cflags} -c "${CMAKE_CURRENT_SOURCE_DIR}/guest/functions.c" -o "${_dir}/functions.o"
    COMMAND "${LGP_GUEST_LLD}" ${_ldflags} "${_dir}/crt0.o" "${_dir}/functions.o" -o "${_dir}/functions.elf"
    COMMAND "${Python3_EXECUTABLE}" "${_indirect}/elf2nro.py" "${_dir}/functions.elf" "${_dir}/functions.nro"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/guest/functions.c" "${_indirect}/guest/crt0.S" "${_indirect}/guest/link.ld"
            "${_indirect}/elf2nro.py"
    COMMENT "Building the function difftest self-test program"
    VERBATIM)
add_custom_command(
    OUTPUT "${_dir}/lifted/lifted.cpp" "${_dir}/lifted/candidates.tsv"
    COMMAND lgp_fn_select "${_dir}/functions.nro" --out "${_dir}/lifted" --split 1
    DEPENDS lgp_fn_select "${_dir}/functions.nro"
    COMMENT "Selecting and lifting the self-test functions"
    VERBATIM)
lgp_add_fn_difftest(lgp_fn_difftest_selftest "${_dir}/lifted/lifted.cpp")
# Every function must agree, and at least this many must be found and returned
# normally in some case (guards against a selection that silently drops them).
add_test(NAME difftest.fn_selftest
         COMMAND lgp_fn_difftest_selftest "${_dir}/functions.nro" --candidates "${_dir}/lifted/candidates.tsv"
                 --cases 64 --fpcr-mix --min-ok 25)
set_tests_properties(difftest.fn_selftest PROPERTIES LABELS "difftest;hv" TIMEOUT 600)
