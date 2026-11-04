# P2.5 acceptance (ADR-0004): a test program with function pointers, virtual
# calls, switch jump tables, indirect tail calls and setjmp/longjmp is built
# for AArch64 with the host clang + ld.lld (freestanding, own crt0, no libnx),
# packed as NRO, recompiled with lgp_lift and run against the runtime's
# dispatch table; Unicorn runs the same NRO as the oracle. Label "indirect".
#
# Needs a clang with the AArch64 target and ld.lld on the build host, plus
# Unicorn; the generator runs on the build host, so not when cross-compiling.
if(CMAKE_CROSSCOMPILING OR NOT LGP_ENABLE_UNICORN)
    return()
endif()

find_program(LGP_GUEST_CLANG NAMES clang clang-20 clang-19 clang-18 clang-17 DOC "clang used to build AArch64 test programs")
find_program(LGP_GUEST_LLD NAMES ld.lld ld.lld-20 ld.lld-19 ld.lld-18 ld.lld-17 DOC "ld.lld used to link AArch64 test programs")
find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)
if(NOT LGP_GUEST_CLANG OR NOT LGP_GUEST_LLD)
    message(WARNING "tests/indirect: clang or ld.lld not found, the P2.5 tests (-L indirect) are not built")
    add_test(NAME indirect.toolchain_missing COMMAND "${CMAKE_COMMAND}" -E false)
    set_tests_properties(indirect.toolchain_missing PROPERTIES LABELS "indirect")
    return()
endif()

# Depth of the tail-call chain in the guest. With guaranteed tail calls
# (clang musttail, GCC >= 15) a million states run in constant host stack;
# other compilers only get a shallow chain (see lifted.hpp).
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR (CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 15))
    set(_depth 1000000)
else()
    set(_depth 2000)
endif()

set(_guest "${CMAKE_CURRENT_SOURCE_DIR}/guest")
set(_cflags --target=aarch64-none-elf -O2 -fPIC -ffreestanding -fno-builtin -nostdlib -fno-exceptions -fno-rtti
            -fvisibility=hidden -funwind-tables -DLGP_STATE_DEPTH=${_depth})
set(_ldflags -pie --no-dynamic-linker --eh-frame-hdr -z norelro -z max-page-size=0x1000 --pack-dyn-relocs=none
             --export-dynamic-symbol=setjmp --export-dynamic-symbol=longjmp -T "${_guest}/link.ld")

# Two builds: clang's default compressed jump tables (ldrb + adr + lsl #2) and
# the 32-bit form ldrsw + add that all 1 541 tables in LGP's main use (P1.5).
foreach(jt IN ITEMS jt8 jt32)
    set(_dir "${CMAKE_CURRENT_BINARY_DIR}/${jt}")
    set(_extra)
    if(jt STREQUAL "jt32")
        set(_extra -mllvm -aarch64-enable-compress-jump-tables=false)
    endif()
    add_custom_command(
        OUTPUT "${_dir}/program.elf" "${_dir}/program.nro"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${_dir}"
        COMMAND "${LGP_GUEST_CLANG}" ${_cflags} ${_extra} -c "${_guest}/crt0.S" -o "${_dir}/crt0.o"
        COMMAND "${LGP_GUEST_CLANG}" ${_cflags} ${_extra} -c "${_guest}/support.S" -o "${_dir}/support.o"
        COMMAND "${LGP_GUEST_CLANG}" ${_cflags} ${_extra} -c "${_guest}/program.cpp" -o "${_dir}/program.o"
        COMMAND "${LGP_GUEST_LLD}" ${_ldflags} "${_dir}/crt0.o" "${_dir}/program.o" "${_dir}/support.o"
                -o "${_dir}/program.elf"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/elf2nro.py" "${_dir}/program.elf" "${_dir}/program.nro"
        DEPENDS "${_guest}/crt0.S" "${_guest}/support.S" "${_guest}/program.cpp" "${_guest}/link.ld"
                "${CMAKE_CURRENT_SOURCE_DIR}/elf2nro.py"
        COMMENT "Building the P2.5 test program (${jt})"
        VERBATIM)

    # Without config: mode 1 must hit the error path. With config: everything matches.
    # Hooks (P8, lgp/runtime/hooks.hpp): the config plus hook doors on helper
    # (reached by direct BL) and op_add (reached by BLR through the dispatch table).
    foreach(cfg IN ITEMS 0 1 2)
        set(_name "${jt}")
        set(_args)
        if(cfg EQUAL 1)
            set(_name "${jt}_config")
            set(_args --elf "${_dir}/program.elf" --function two_entries+4)
        elseif(cfg EQUAL 2)
            set(_name "${jt}_hooks")
            set(_args --elf "${_dir}/program.elf" --function two_entries+4 --hook _Z6helperi --hook _Z6op_addii)
        endif()
        set(_lifted "${CMAKE_CURRENT_BINARY_DIR}/generated/${_name}_lifted.cpp")
        add_custom_command(
            OUTPUT "${_lifted}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
            COMMAND lgp_lift "${_dir}/program.nro" -o "${_lifted}" ${_args} --summary
            DEPENDS lgp_lift "${_dir}/program.nro" "${_dir}/program.elf"
            COMMENT "Recompiling the P2.5 test program (${_name})"
            VERBATIM)
        add_executable(lgp_test_indirect_${_name} test_indirect.cpp "${_lifted}")
        target_include_directories(lgp_test_indirect_${_name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../difftest")
        if(cfg EQUAL 2)
            target_compile_definitions(lgp_test_indirect_${_name} PRIVATE LGP_INDIRECT_CONFIG=1 LGP_INDIRECT_HOOKS=1
                                                                         LGP_STATE_DEPTH=${_depth})
        else()
            target_compile_definitions(lgp_test_indirect_${_name} PRIVATE LGP_INDIRECT_CONFIG=${cfg} LGP_STATE_DEPTH=${_depth})
        endif()
        target_link_libraries(lgp_test_indirect_${_name} PRIVATE
            lgp_runtime_core lgp_lifted_headers lgp_recomp_core lgp_difftest_unicorn)
        if(TARGET lgp_difftest_hv)
            # P2.7: also run the program natively on the hypervisor (macOS arm64).
            target_link_libraries(lgp_test_indirect_${_name} PRIVATE lgp_difftest_hv)
            target_compile_definitions(lgp_test_indirect_${_name} PRIVATE LGP_INDIRECT_HV=1)
            lgp_hv_sign(lgp_test_indirect_${_name})
        endif()
        add_test(NAME indirect.${_name}
                 COMMAND lgp_test_indirect_${_name} "${_dir}/program.nro" "${_dir}/program.elf")
        set_tests_properties(indirect.${_name} PROPERTIES LABELS "indirect;unicorn" TIMEOUT 600)
    endforeach()
endforeach()
