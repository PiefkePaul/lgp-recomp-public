# Lifted-code benchmark (P6, docs/knowledge/lifted-code-performance.md):
# guest/kernels.cpp is built for AArch64 with the host clang + ld.lld, packed
# as NRO (tests/indirect tools), recompiled with lgp_lift and run on the
# runtime core next to the same kernels compiled natively. Times are only
# meaningful in a Release build; ctest runs a short smoke pass that checks the
# checksums (label "bench").
#
# WebAssembly: LGP_BENCH_IMPORT_DIR = <native build>/bench/lifter/generated
# (program.img, program.meta, program_lifted.cpp from a native build).
set(LGP_BENCH_IMPORT_DIR "" CACHE PATH "Directory with program.img/.meta and program_lifted.cpp from a native build")

# GCC/clang only: the kernels use GNU attributes and builtins, and lifted code
# is meant for clang (musttail, ADR-0004); MSVC numbers would mean little.
if(MSVC)
    return()
endif()

set(_bench_dir "${CMAKE_CURRENT_BINARY_DIR}/generated")
if(NOT CMAKE_CROSSCOMPILING)
    find_program(LGP_GUEST_CLANG NAMES clang clang-20 clang-19 clang-18 clang-17 DOC "clang used to build AArch64 test programs")
    find_program(LGP_GUEST_LLD NAMES ld.lld ld.lld-20 ld.lld-19 ld.lld-18 ld.lld-17 DOC "ld.lld used to link AArch64 test programs")
    find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)
    if(NOT LGP_GUEST_CLANG OR NOT LGP_GUEST_LLD)
        message(STATUS "bench/lifter: clang or ld.lld not found, lifted-code benchmark not built")
        return()
    endif()
    set(_guest "${CMAKE_CURRENT_SOURCE_DIR}/guest")
    set(_indirect "${PROJECT_SOURCE_DIR}/tests/indirect")
    set(_cflags --target=aarch64-none-elf -O2 -fPIC -ffreestanding -fno-builtin -nostdlib -fno-exceptions -fno-rtti
                -fvisibility=hidden -funwind-tables -ffp-contract=off -fno-math-errno -DLGP_BENCH_GUEST)
    set(_ldflags -pie --no-dynamic-linker --eh-frame-hdr -z norelro -z max-page-size=0x1000 --pack-dyn-relocs=none
                 -T "${_indirect}/guest/link.ld")
    add_custom_command(
        OUTPUT "${_bench_dir}/program.elf" "${_bench_dir}/program.nro"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${_bench_dir}"
        COMMAND "${LGP_GUEST_CLANG}" ${_cflags} -c "${_guest}/crt0.S" -o "${_bench_dir}/crt0.o"
        COMMAND "${LGP_GUEST_CLANG}" ${_cflags} -c "${_guest}/kernels.cpp" -o "${_bench_dir}/kernels.o"
        COMMAND "${LGP_GUEST_LLD}" ${_ldflags} "${_bench_dir}/crt0.o" "${_bench_dir}/kernels.o" -o "${_bench_dir}/program.elf"
        COMMAND "${Python3_EXECUTABLE}" "${_indirect}/elf2nro.py" "${_bench_dir}/program.elf" "${_bench_dir}/program.nro"
        DEPENDS "${_guest}/crt0.S" "${_guest}/kernels.cpp" "${_indirect}/guest/link.ld" "${_indirect}/elf2nro.py"
        COMMENT "Building the benchmark guest program"
        VERBATIM)

    add_executable(lgp_bench_image bench_image.cpp)
    target_link_libraries(lgp_bench_image PRIVATE lgp_recomp_core)
    add_custom_command(
        OUTPUT "${_bench_dir}/program.img" "${_bench_dir}/program.meta" "${_bench_dir}/program_lifted.cpp"
               "${_bench_dir}/program_lifted_ctx.cpp"
        COMMAND lgp_bench_image "${_bench_dir}/program.nro" "${_bench_dir}/program"
        COMMAND lgp_lift "${_bench_dir}/program.nro" -o "${_bench_dir}/program_lifted.cpp" --summary
        # Reference variant: every register access through CpuState (the code shape before P6).
        COMMAND lgp_lift "${_bench_dir}/program.nro" -o "${_bench_dir}/program_lifted_ctx.cpp" --no-register-locals
        DEPENDS lgp_bench_image lgp_lift "${_bench_dir}/program.nro"
        COMMENT "Recompiling the benchmark guest program"
        VERBATIM)
    set(_stem "${_bench_dir}/program")
    set(_lifted "${_bench_dir}/program_lifted.cpp")
    set(_lifted_ctx "${_bench_dir}/program_lifted_ctx.cpp")
elseif(LGP_BENCH_IMPORT_DIR)
    set(_stem "${LGP_BENCH_IMPORT_DIR}/program")
    set(_lifted "${LGP_BENCH_IMPORT_DIR}/program_lifted.cpp")
    if(NOT EXISTS "${_lifted}" OR NOT EXISTS "${_stem}.img")
        message(FATAL_ERROR "LGP_BENCH_IMPORT_DIR: program.img/.meta or program_lifted.cpp missing in ${LGP_BENCH_IMPORT_DIR}")
    endif()
else()
    return()
endif()

# lgp_bench_lifted: lifted with the default options; lgp_bench_lifted_ctx: the
# same program with --no-register-locals, to compare code shapes.
function(lgp_add_bench name lifted)
    add_executable(${name} bench_lifted.cpp guest/kernels.cpp "${lifted}")
    target_link_libraries(${name} PRIVATE lgp_runtime_core lgp_lifted_headers)
    if(EMSCRIPTEN)
        target_link_options(${name} PRIVATE -sNODERAWFS=1 -sALLOW_MEMORY_GROWTH=1 -sMAXIMUM_MEMORY=4GB)
    endif()
    add_test(NAME bench.${name} COMMAND ${name} "${_stem}" --scale 0.05 --repeat 1)
    set_tests_properties(bench.${name} PROPERTIES LABELS "bench" TIMEOUT 600)
endfunction()
# Same FP rules for the native kernels as for the guest build (checksums must agree).
set_source_files_properties(guest/kernels.cpp PROPERTIES COMPILE_OPTIONS "-ffp-contract=off;-fno-math-errno")
lgp_add_bench(lgp_bench_lifted "${_lifted}")
# The same code with host NaN results in the FP fast paths (LGP_FP_HOST_NAN).
lgp_add_bench(lgp_bench_lifted_hostnan "${_lifted}")
target_compile_definitions(lgp_bench_lifted_hostnan PRIVATE LGP_FP_HOST_NAN=1)
if(_lifted_ctx)
    lgp_add_bench(lgp_bench_lifted_ctx "${_lifted_ctx}")
elseif(EXISTS "${LGP_BENCH_IMPORT_DIR}/program_lifted_ctx.cpp")
    lgp_add_bench(lgp_bench_lifted_ctx "${LGP_BENCH_IMPORT_DIR}/program_lifted_ctx.cpp")
endif()
