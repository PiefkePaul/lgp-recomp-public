# Test suite. Run with: ctest --preset default

# Pre-commit guard: proves NCA3/NSO0/PFS0 magic, forbidden extensions, key
# files and oversized files are refused, including an end-to-end `git commit`.
add_test(NAME guard.pre_commit
    COMMAND "${Python3_EXECUTABLE}" -m unittest -v test_guard
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/guard")
set_tests_properties(guard.pre_commit PROPERTIES LABELS "guard")

# Repository scan: no tracked file may look like game data.
add_test(NAME guard.tracked_files
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/guard/check_game_data.py" --all
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
set_tests_properties(guard.tracked_files PROPERTIES LABELS "guard")

# Toolchain smoke test: the C++20 compiler builds and runs a binary.
add_executable(lgp_smoke smoke/smoke.cpp)
add_test(NAME smoke.toolchain COMMAND lgp_smoke)
set_tests_properties(smoke.toolchain PROPERTIES LABELS "smoke")

# Analysis tools: NSO/NPDM inventory parser on synthetic NSOs/NROs (see also
# tools.fixture_inventory below for the real homebrew fixtures).
add_test(NAME tools.nso_inventory
    COMMAND "${Python3_EXECUTABLE}" -m unittest -v test_nso_inventory
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/tools")
set_tests_properties(tools.nso_inventory PROPERTIES LABELS "tools")

# Version diff (ExeFS function by function, RomFS file by file) on synthetic NSOs.
add_test(NAME tools.version_diff
    COMMAND "${Python3_EXECUTABLE}" -m unittest -v test_version_diff
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/tools")
set_tests_properties(tools.version_diff PROPERTIES LABELS "tools")

# Carrying cfg/mod/recipe addresses over to another game build, on synthetic NSOs.
add_test(NAME tools.addr_translate
    COMMAND "${Python3_EXECUTABLE}" -m unittest -v test_addr_translate
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/tools")
set_tests_properties(tools.addr_translate PROPERTIES LABELS "tools")

# Party HP editor for test copies of savedata.bin, on a synthetic save.
add_test(NAME tools.save_party
    COMMAND "${Python3_EXECUTABLE}" -m unittest -v test_save_party
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/tools")
set_tests_properties(tools.save_party PROPERTIES LABELS "tools")

# Catch pouch editor for test copies of savedata.bin, on a synthetic save.
add_test(NAME tools.save_items
    COMMAND "${Python3_EXECUTABLE}" -m unittest -v test_save_items
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/tools")
set_tests_properties(tools.save_items PROPERTIES LABELS "tools")

# Public snapshot export (tools/publish/public_snapshot.py), on a throwaway git repository.
add_test(NAME tools.public_snapshot
    COMMAND "${Python3_EXECUTABLE}" -m unittest -v test_public_snapshot
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/tools")
set_tests_properties(tools.public_snapshot PROPERTIES LABELS "tools")

# Game builder of the friends' kit (tools/kit/build.py), on made-up kits; only checks and dry runs.
add_test(NAME tools.kit_build
    COMMAND "${Python3_EXECUTABLE}" -m unittest -v test_kit_build
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/tools")
set_tests_properties(tools.kit_build PROPERTIES LABELS "tools;kit")


# Data mod files built from the player's own RomFS (tools/kit/derive.py), on a synthetic RomFS image.
add_test(NAME tools.kit_derive
    COMMAND "${Python3_EXECUTABLE}" -m unittest -v test_kit_derive
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/tools")
set_tests_properties(tools.kit_derive PROPERTIES LABELS "tools;kit")

# Ready-made Windows package of the kit (tools/kit/make_bauset.py), from a tiny repo and made-up tool ZIPs.
add_test(NAME tools.make_bauset
    COMMAND "${Python3_EXECUTABLE}" -m unittest -v test_make_bauset
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/tools")
set_tests_properties(tools.make_bauset PROPERTIES LABELS "tools;kit")

# .ipa of an iOS build without Xcode (tools/ios/make_ipa.py, preset ios-cross), on a made-up .app.
add_test(NAME tools.make_ipa
    COMMAND "${Python3_EXECUTABLE}" -m unittest -v test_make_ipa
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/tools")
set_tests_properties(tools.make_ipa PROPERTIES LABELS "tools;kit")

# Gen-1 encounter tables from PokeAPI CSVs (mod original-encounters), on synthetic CSVs.
add_test(NAME tools.gen1_tables
    COMMAND "${Python3_EXECUTABLE}" -m unittest -v test_gen1_tables
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/tools")
set_tests_properties(tools.gen1_tables PROPERTIES LABELS "tools")
add_test(NAME tools.encount_data
    COMMAND "${Python3_EXECUTABLE}" -m unittest -v test_encount_data
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/tools")
set_tests_properties(tools.encount_data PROPERTIES LABELS "tools")
# Balls in the battle bag (data mod battle-balls), on a synthetic battle_pocket.dat.
add_test(NAME tools.battle_pocket
    COMMAND "${Python3_EXECUTABLE}" -m unittest -v test_battle_pocket
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/tools")
set_tests_properties(tools.battle_pocket PROPERTIES LABELS "tools")

# Battle sequence files (SESD, .bseq) read and written, on synthetic sequences.
add_test(NAME tools.sesd
    COMMAND "${Python3_EXECUTABLE}" -m unittest -v test_sesd
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/tools")
set_tests_properties(tools.sesd PROPERTIES LABELS "tools")

# GFLXPACK archives listed and unpacked, on a synthetic archive.
add_test(NAME tools.gfpak
    COMMAND "${Python3_EXECUTABLE}" -m unittest -v test_gfpak
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/tools")
set_tests_properties(tools.gfpak PROPERTIES LABELS "tools")

# Homebrew fixtures (P0.7): a separate CMake project cross-compiled with
# devkitA64 + libnx, then inspected with nso_inventory.py (P1.2). The fixture
# binaries stay in the build directory and are never committed.
if(LGP_BUILD_FIXTURES)
    if(NOT DEFINED ENV{DEVKITPRO})
        message(FATAL_ERROR "LGP_BUILD_FIXTURES needs the DEVKITPRO environment variable")
    endif()
    include(ExternalProject)
    set(LGP_FIXTURE_DIR "${CMAKE_BINARY_DIR}/fixtures")
    ExternalProject_Add(lgp_fixtures
        SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
        BINARY_DIR "${LGP_FIXTURE_DIR}"
        CMAKE_ARGS
            "-DCMAKE_TOOLCHAIN_FILE=$ENV{DEVKITPRO}/cmake/Switch.cmake"
            -DCMAKE_BUILD_TYPE=Release
        INSTALL_COMMAND ""
        BUILD_ALWAYS ON
        BUILD_BYPRODUCTS "${LGP_FIXTURE_DIR}/basic.nro" "${LGP_FIXTURE_DIR}/basic.nso"
                         "${LGP_FIXTURE_DIR}/threads.nro" "${LGP_FIXTURE_DIR}/threads.nso"
                         "${LGP_FIXTURE_DIR}/services.nso")

    add_test(NAME tools.fixture_inventory
        COMMAND "${Python3_EXECUTABLE}" -m unittest -v test_fixture_inventory
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/tools")
    set_tests_properties(tools.fixture_inventory PROPERTIES
        LABELS "tools;fixtures"
        ENVIRONMENT "LGP_FIXTURE_DIR=${LGP_FIXTURE_DIR}")
endif()

# Differential-test harness (ADR-0006): Unicorn oracle (P0.8).
add_subdirectory(difftest/unicorn)

# Lifter (P2.3): decoder unit tests and differential tests against Unicorn.
add_subdirectory(lifter)
# SIMD&FP lifter (P2.4): differential tests and decoder validity against Unicorn.
add_subdirectory(lifter_simd)
# Lifter fuzzer: random mixed sequences against Unicorn.
add_subdirectory(lifter_fuzz)

# Ryujinx trace-log statistics (SVCs, IPC, threads, memory), synthetic lines only.
add_test(NAME tools.ryujinx_log_stats
    COMMAND "${Python3_EXECUTABLE}" -m unittest -v test_ryujinx_log_stats
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/tools")
set_tests_properties(tools.ryujinx_log_stats PROPERTIES LABELS "tools")

# P1.5 code statistics: NSO->ELF converter (incl. .eh_frame parser) and the Markdown report,
# synthetic inputs only (the Ghidra scripts themselves run locally with real modules).
add_test(NAME tools.code_stats
    COMMAND "${Python3_EXECUTABLE}" -m unittest -v test_code_stats
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/tools")
set_tests_properties(tools.code_stats PROPERTIES LABELS "tools")

# Jump-table comparison of two `lgp_recomp functions --json` runs; synthetic JSON only.
add_test(NAME tools.compare_jump_tables
    COMMAND "${Python3_EXECUTABLE}" -m unittest -v test_compare_jump_tables
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/tools")
set_tests_properties(tools.compare_jump_tables PROPERTIES LABELS "tools")

# Shape of translated WGSL joined with compile times from logs; synthetic WGSL only.
add_test(NAME tools.wgsl_stats
    COMMAND "${Python3_EXECUTABLE}" -m unittest -v test_wgsl_stats
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/tools")
set_tests_properties(tools.wgsl_stats PROPERTIES LABELS "tools")

# Why a device run ended (memory curve, system kill reason); synthetic logs only.
add_test(NAME tools.kill_run_report
    COMMAND "${Python3_EXECUTABLE}" -m unittest -v test_kill_run_report
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/tools")
set_tests_properties(tools.kill_run_report PROPERTIES LABELS "tools")

# Frame range report and ABAB comparison of --time-stats logs, simpleperf report; synthetic logs only.
add_test(NAME tools.perf_run_report
    COMMAND "${Python3_EXECUTABLE}" -m unittest -v test_perf_run_report
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/tools")
set_tests_properties(tools.perf_run_report PROPERTIES LABELS "tools")

# App icons of every platform (tools/icon/gen_icons.py): files, sizes, catalogs; no Pillow needed.
add_test(NAME tools.app_icons
    COMMAND "${Python3_EXECUTABLE}" -m unittest -v test_app_icons
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/tools")
set_tests_properties(tools.app_icons PROPERTIES LABELS "tools")

# Shield test plan over adb (analysis 33): manifest/badging checks, adb output parsers; no device.
add_test(NAME tools.shield_day
    COMMAND "${Python3_EXECUTABLE}" -m unittest -v test_shield_day
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/tools")
set_tests_properties(tools.shield_day PROPERTIES LABELS "tools")

# Graphics inventory (P1.4): ARM64 ADRP scanner, NVN loader/slot detection, sdk proc table,
# Ryujinx PTC profile and guest shader cache readers, BNSH/BNTX/GFLXPACK scanning; synthetic data only.
add_test(NAME tools.gfx_inventory
    COMMAND "${Python3_EXECUTABLE}" -m unittest -v test_gfx_inventory
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/tools")
set_tests_properties(tools.gfx_inventory PROPERTIES LABELS "tools")

# P4.2: BNSH parser/stage extractor and shader-coverage report helpers; synthetic containers only.
add_test(NAME tools.bnsh
    COMMAND "${Python3_EXECUTABLE}" -m unittest -v test_bnsh
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/tools")
set_tests_properties(tools.bnsh PROPERTIES LABELS "tools")


# Differential-test harness (ADR-0006); the Hypervisor oracle builds only on macOS arm64.
add_subdirectory(difftest)

# Recompiler analysis (loader, CFG), synthetic modules only.
add_subdirectory(recomp)

# Runtime (P2.6/P3): guest memory, kernel svcs, guest.hpp; Unicorn-backed kernel programs.
add_subdirectory(runtime)
# Platform layer (P3.6): interfaces, device-less backends, runtime/ free of host code.
add_subdirectory(platform)

# P2.5: indirect branches end to end (own AArch64 test program, Unicorn vs. recompiled).
add_subdirectory(indirect)

# Patch sites of game mods end to end (own AArch64 test program, lgp_lift --patch-site).
add_subdirectory(patch)

# P2.8: the homebrew fixtures run recompiled vs. Unicorn (and in Node from imported artifacts).
add_subdirectory(homebrew)

# P3.4: the boot program (runtime/boot) on an SDK-shaped module pair and the homebrew fixtures.
add_subdirectory(boot)

# P4 shader spike: Maxwell decoder/WGSL backend on own test shaders.
add_subdirectory(gpu)
# P4.1 NVN-HLE: proc thunks, objects, clear + present on the CPU backend.
add_subdirectory(nvn)
# P4.4 browser tests: Emscripten pages in Chromium (Playwright), WebGPU on SwiftShader.
add_subdirectory(web)

