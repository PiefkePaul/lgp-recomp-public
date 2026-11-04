# Patch sites of game mods end to end: a small AArch64 program (host clang +
# ld.lld, packed as NRO like tests/indirect) is lifted with --patch-site and
# runs on the runtime with and without the host function. Label "patch".
if(CMAKE_CROSSCOMPILING)
    return()
endif()

find_program(LGP_GUEST_CLANG NAMES clang clang-20 clang-19 clang-18 clang-17 DOC "clang used to build AArch64 test programs")
find_program(LGP_GUEST_LLD NAMES ld.lld ld.lld-20 ld.lld-19 ld.lld-18 ld.lld-17 DOC "ld.lld used to link AArch64 test programs")
find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)
if(NOT LGP_GUEST_CLANG OR NOT LGP_GUEST_LLD)
    message(WARNING "tests/patch: clang or ld.lld not found, the patch-site tests (-L patch) are not built")
    return()
endif()

set(_indirect "${CMAKE_CURRENT_SOURCE_DIR}/../indirect")
set(_dir "${CMAKE_CURRENT_BINARY_DIR}")
set(_cflags --target=aarch64-none-elf -fPIC -ffreestanding -nostdlib)
set(_ldflags -pie --no-dynamic-linker --eh-frame-hdr -z norelro -z max-page-size=0x1000 --pack-dyn-relocs=none
             -T "${_indirect}/guest/link.ld")
add_custom_command(
    OUTPUT "${_dir}/program.elf" "${_dir}/program.nro"
    COMMAND "${LGP_GUEST_CLANG}" ${_cflags} -c "${_indirect}/guest/crt0.S" -o "${_dir}/crt0.o"
    COMMAND "${LGP_GUEST_CLANG}" ${_cflags} -c "${CMAKE_CURRENT_SOURCE_DIR}/program.S" -o "${_dir}/program.o"
    COMMAND "${LGP_GUEST_LLD}" ${_ldflags} "${_dir}/crt0.o" "${_dir}/program.o" -o "${_dir}/program.elf"
    COMMAND "${Python3_EXECUTABLE}" "${_indirect}/elf2nro.py" "${_dir}/program.elf" "${_dir}/program.nro"
    DEPENDS "${_indirect}/guest/crt0.S" "${CMAKE_CURRENT_SOURCE_DIR}/program.S" "${_indirect}/guest/link.ld"
            "${_indirect}/elf2nro.py"
    COMMENT "Building the patch-site test program"
    VERBATIM)

# Both code shapes: registers in locals (default) and through CpuState.
foreach(shape IN ITEMS locals ctx)
    set(_extra)
    if(shape STREQUAL "ctx")
        set(_extra --no-register-locals)
    endif()
    set(_lifted "${_dir}/generated/${shape}_lifted.cpp")
    add_custom_command(
        OUTPUT "${_lifted}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${_dir}/generated"
        COMMAND lgp_lift "${_dir}/program.nro" -o "${_lifted}" --elf "${_dir}/program.elf"
                --patch-site patch_site --patch-site imm_site --patch-site orr_site ${_extra} --summary
        DEPENDS lgp_lift "${_dir}/program.nro" "${_dir}/program.elf"
        COMMENT "Recompiling the patch-site test program (${shape})"
        VERBATIM)
    add_executable(lgp_test_patch_${shape} test_patch.cpp "${_lifted}")
    target_link_libraries(lgp_test_patch_${shape} PRIVATE lgp_runtime_core lgp_lifted_headers lgp_recomp_core)
    add_test(NAME patch.${shape} COMMAND lgp_test_patch_${shape} "${_dir}/program.nro" "${_dir}/program.elf")
    set_tests_properties(patch.${shape} PROPERTIES LABELS "patch;runtime")
endforeach()

# A branch cannot be a patch site: lgp_lift refuses it.
add_test(NAME patch.branch_rejected
         COMMAND lgp_lift "${_dir}/program.nro" -o "${_dir}/generated/rejected.cpp" --elf "${_dir}/program.elf"
                 --patch-site branch_site)
set_tests_properties(patch.branch_rejected PROPERTIES LABELS "patch" WILL_FAIL TRUE)
# A mod file whose patch site lies outside the lifted code is named in --bad-mods-out (tools/kit/build.py).
add_test(NAME patch.bad_mods_named
         COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/check_bad_mods.py" $<TARGET_FILE:lgp_lift>
                 "${_dir}/program.nro" "${_dir}/program.elf" "${_dir}/bad-mods")
set_tests_properties(patch.bad_mods_named PROPERTIES LABELS "patch")
# Depends on the program built above.
add_custom_target(lgp_test_patch_program ALL DEPENDS "${_dir}/program.nro")
