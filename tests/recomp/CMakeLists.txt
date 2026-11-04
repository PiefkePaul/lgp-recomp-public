# Recompiler analysis tests (P2.1 loader, P2.2 CFG). Synthetic modules only.

add_executable(lgp_recomp_test_loader test_loader.cpp)
target_link_libraries(lgp_recomp_test_loader PRIVATE lgp_recomp_core)
add_test(NAME recomp.loader COMMAND lgp_recomp_test_loader)
set_tests_properties(recomp.loader PROPERTIES LABELS "recomp")

add_executable(lgp_recomp_test_cfg test_cfg.cpp)
target_link_libraries(lgp_recomp_test_cfg PRIVATE lgp_recomp_core)
add_test(NAME recomp.cfg COMMAND lgp_recomp_test_cfg)
set_tests_properties(recomp.cfg PROPERTIES LABELS "recomp")

# Cross-check against the Python reference parser (tools/analyze/nso_inventory.py):
# the loader test writes synthetic NSO/NRO files, both parsers must agree.
if(NOT CMAKE_CROSSCOMPILING)
    add_test(NAME recomp.vs_reference_parser
        COMMAND "${Python3_EXECUTABLE}" -I "${CMAKE_CURRENT_SOURCE_DIR}/test_vs_reference.py"
                "$<TARGET_FILE:lgp_recomp_test_loader>" "$<TARGET_FILE:lgp_recomp>"
                "${PROJECT_SOURCE_DIR}/tools/analyze/nso_inventory.py")
    set_tests_properties(recomp.vs_reference_parser PROPERTIES LABELS "recomp;tools")
endif()

# Real homebrew fixtures (P0.7): loader + function discovery against the ELF symbols.
# Acceptance P2.1/P2.2: relocations agree with the reference parser, function starts are
# 100 % correct against .symtab (0 missing, 0 inside a symbol body).
if(LGP_BUILD_FIXTURES)
    foreach(fmt nro nso)
        add_test(NAME recomp.fixture_check_functions_${fmt}
            COMMAND lgp_recomp check-functions "${LGP_FIXTURE_DIR}/basic.${fmt}" --elf "${LGP_FIXTURE_DIR}/basic.elf" --verbose)
        set_tests_properties(recomp.fixture_check_functions_${fmt} PROPERTIES LABELS "recomp;fixtures")
    endforeach()
    add_test(NAME recomp.fixture_vs_reference_parser
        COMMAND "${Python3_EXECUTABLE}" -I "${CMAKE_CURRENT_SOURCE_DIR}/test_vs_reference.py"
                --files "${LGP_FIXTURE_DIR}/basic.nro" "${LGP_FIXTURE_DIR}/basic.nso" --
                "$<TARGET_FILE:lgp_recomp>" "${PROJECT_SOURCE_DIR}/tools/analyze/nso_inventory.py")
    set_tests_properties(recomp.fixture_vs_reference_parser PROPERTIES LABELS "recomp;fixtures")

    # Whole chain on real homebrew code: loader → CFG → adapter → lifter, and the
    # generated C++ must compile against the real runtime header (lgp/runtime/guest.hpp).
    # Compile only; running it is P2.8. The generated files stay in the build directory.
    foreach(fmt nro nso)
        set(_lifted "${CMAKE_CURRENT_BINARY_DIR}/basic_${fmt}_lifted.cpp")
        add_custom_command(OUTPUT "${_lifted}"
            COMMAND lgp_recomp lift "${LGP_FIXTURE_DIR}/basic.${fmt}" --out "${_lifted}"
            DEPENDS lgp_recomp lgp_fixtures "${LGP_FIXTURE_DIR}/basic.${fmt}"
            COMMENT "Lifting basic.${fmt}"
            VERBATIM)
        add_library(lgp_fixture_lifted_${fmt} OBJECT "${_lifted}")
        target_link_libraries(lgp_fixture_lifted_${fmt} PRIVATE lgp_runtime_core)
    endforeach()
endif()

# classify() runs on the lifter's decoder; the original CFG classifier stays as
# an independent cross-check (docs/design/p2-interfaces.md, section 2).
add_executable(lgp_recomp_test_classify_crosscheck test_classify_crosscheck.cpp)
target_link_libraries(lgp_recomp_test_classify_crosscheck PRIVATE lgp_recomp_core)
add_test(NAME recomp.classify_crosscheck COMMAND lgp_recomp_test_classify_crosscheck)
set_tests_properties(recomp.classify_crosscheck PROPERTIES LABELS "recomp")

add_executable(lgp_recomp_test_codegen_entries test_codegen_entries.cpp)
target_link_libraries(lgp_recomp_test_codegen_entries PRIVATE lgp_recomp_codegen)
add_test(NAME recomp.codegen_entries COMMAND lgp_recomp_test_codegen_entries)
set_tests_properties(recomp.codegen_entries PROPERTIES LABELS "recomp")

add_executable(lgp_recomp_test_lift_input test_lift_input.cpp)
target_link_libraries(lgp_recomp_test_lift_input PRIVATE lgp_recomp_core)
add_test(NAME recomp.lift_input COMMAND lgp_recomp_test_lift_input)
set_tests_properties(recomp.lift_input PROPERTIES LABELS "recomp;lifter")

add_executable(lgp_recomp_test_native_census test_native_census.cpp)
target_link_libraries(lgp_recomp_test_native_census PRIVATE lgp_recomp_core)
add_test(NAME recomp.native_census COMMAND lgp_recomp_test_native_census)
set_tests_properties(recomp.native_census PROPERTIES LABELS "recomp")

# Native rewrite plan (x18 renaming); with Unicorn the rewritten functions also
# run against the original ones.
add_executable(lgp_recomp_test_native_rewrite test_native_rewrite.cpp)
target_link_libraries(lgp_recomp_test_native_rewrite PRIVATE lgp_recomp_core)
add_test(NAME recomp.native_rewrite COMMAND lgp_recomp_test_native_rewrite)
set_tests_properties(recomp.native_rewrite PROPERTIES LABELS "recomp")
if(TARGET lgp_difftest_unicorn)
    add_executable(lgp_recomp_test_native_rewrite_unicorn test_native_rewrite.cpp)
    target_link_libraries(lgp_recomp_test_native_rewrite_unicorn PRIVATE lgp_recomp_core lgp_difftest_unicorn)
    target_compile_definitions(lgp_recomp_test_native_rewrite_unicorn PRIVATE LGP_NATIVE_REWRITE_UNICORN=1)
    add_test(NAME recomp.native_rewrite_unicorn COMMAND lgp_recomp_test_native_rewrite_unicorn)
    set_tests_properties(recomp.native_rewrite_unicorn PROPERTIES LABELS "recomp;unicorn")
endif()

# Native rewrite, second part: stubs, patch area, patched NSO; with Unicorn
# every stub form also runs original against rewritten.
add_executable(lgp_recomp_test_native_image test_native_image.cpp)
target_link_libraries(lgp_recomp_test_native_image PRIVATE lgp_recomp_core)
add_test(NAME recomp.native_image COMMAND lgp_recomp_test_native_image)
set_tests_properties(recomp.native_image PROPERTIES LABELS "recomp")
if(TARGET lgp_difftest_unicorn)
    add_executable(lgp_recomp_test_native_image_unicorn test_native_image.cpp)
    target_link_libraries(lgp_recomp_test_native_image_unicorn PRIVATE lgp_recomp_core lgp_difftest_unicorn)
    target_compile_definitions(lgp_recomp_test_native_image_unicorn PRIVATE LGP_NATIVE_REWRITE_UNICORN=1)
    add_test(NAME recomp.native_image_unicorn COMMAND lgp_recomp_test_native_image_unicorn)
    set_tests_properties(recomp.native_image_unicorn PROPERTIES LABELS "recomp;unicorn")
endif()

# The stub encoder against LLVM's assembler (llvm-mc, or clang where llvm-mc
# is missing, as in the Windows LLVM package; never a skipped test).
find_program(LGP_LLVM_MC NAMES llvm-mc llvm-mc-18 llvm-mc-19 llvm-mc-20)
if(LGP_LLVM_MC)
    set(_lgp_a64_assembler "${LGP_LLVM_MC}")
else()
    find_program(LGP_CLANG_ASSEMBLER NAMES clang clang-20 clang-19 clang-18)
    set(_lgp_a64_assembler "${LGP_CLANG_ASSEMBLER}")
endif()
if(NOT CMAKE_CROSSCOMPILING AND _lgp_a64_assembler)
    add_test(NAME recomp.emit_vs_llvm
        COMMAND "${Python3_EXECUTABLE}" -I "${CMAKE_CURRENT_SOURCE_DIR}/test_emit_vs_llvm.py"
                "$<TARGET_FILE:lgp_recomp_test_native_image>" "${_lgp_a64_assembler}")
    set_tests_properties(recomp.emit_vs_llvm PROPERTIES LABELS "recomp")
endif()

# gpr_use() against LLVM's disassembler on random words (only where llvm-mc and
# llvm-objdump are installed; never a skipped test).
find_program(LGP_LLVM_MC NAMES llvm-mc llvm-mc-18 llvm-mc-19 llvm-mc-20)
find_program(LGP_LLVM_OBJDUMP NAMES llvm-objdump llvm-objdump-18 llvm-objdump-19 llvm-objdump-20)
if(NOT CMAKE_CROSSCOMPILING AND LGP_LLVM_MC AND LGP_LLVM_OBJDUMP)
    add_executable(lgp_recomp_gpr_use_dump gpr_use_dump.cpp)
    target_link_libraries(lgp_recomp_gpr_use_dump PRIVATE lgp_recomp_core)
    add_test(NAME recomp.gpr_use_vs_llvm
        COMMAND "${Python3_EXECUTABLE}" -I "${CMAKE_CURRENT_SOURCE_DIR}/test_gpr_use_vs_llvm.py"
                "$<TARGET_FILE:lgp_recomp_gpr_use_dump>" "${LGP_LLVM_MC}" "${LGP_LLVM_OBJDUMP}")
    set_tests_properties(recomp.gpr_use_vs_llvm PROPERTIES LABELS "recomp")
    add_test(NAME recomp.rename_vs_llvm
        COMMAND "${Python3_EXECUTABLE}" -I "${CMAKE_CURRENT_SOURCE_DIR}/test_rename_vs_llvm.py"
                "$<TARGET_FILE:lgp_recomp_gpr_use_dump>" "${LGP_LLVM_MC}" "${LGP_LLVM_OBJDUMP}")
    set_tests_properties(recomp.rename_vs_llvm PROPERTIES LABELS "recomp")
endif()
