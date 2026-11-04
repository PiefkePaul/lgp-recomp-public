# Differential-test harness (ADR-0006), Unicorn executor: the CI oracle (P0.8). Unicorn is GPL-2.0 and is linked into
# test executables only; nothing under recomp/, runtime/, gpu/ or platform/ may
# depend on it.
if(LGP_ENABLE_UNICORN)
    include(FetchContent)
    # Official release sdist from PyPI (contains the full C sources under src/);
    # pinned by hash so CI and local builds use the same bytes.
    FetchContent_Declare(unicorn
        URL https://files.pythonhosted.org/packages/b2/1b/b4248aa8422e86de690cf8e85cf8feae4c33405a097d1ebe71570bdaa6f5/unicorn-2.1.4.tar.gz
        URL_HASH SHA256=00567a70e323f749b419cd86bee4f9115beab7ebba32194581c090cbb7c59cff
        SOURCE_SUBDIR src
        DOWNLOAD_EXTRACT_TIMESTAMP ON
        EXCLUDE_FROM_ALL)
    set(UNICORN_ARCH "aarch64" CACHE STRING "" FORCE)
    set(UNICORN_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(UNICORN_INSTALL OFF CACHE BOOL "" FORCE)
    set(UNICORN_LEGACY_STATIC_ARCHIVE OFF CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS OFF)
    # QEMU relies on wrapping signed arithmetic (its own build passes -fwrapv,
    # Unicorn's CMake does not). Without it clang -O2 folds the NEON helpers'
    # `a > b ? a - b : b - a` into a negated result once a - b overflows:
    # SABD/SABA on lanes with a difference >= 2^31 diverged in Release only.
    if(NOT MSVC)
        string(APPEND CMAKE_C_FLAGS " -fwrapv")
    endif()
    FetchContent_MakeAvailable(unicorn)

    add_library(lgp_difftest_unicorn STATIC ../oracle.cpp unicorn_executor.cpp)
    target_include_directories(lgp_difftest_unicorn PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
    target_link_libraries(lgp_difftest_unicorn PUBLIC unicorn)

    add_executable(lgp_test_unicorn_oracle test_unicorn_oracle.cpp)
    target_link_libraries(lgp_test_unicorn_oracle PRIVATE lgp_difftest_unicorn)
    add_test(NAME difftest.unicorn_oracle COMMAND lgp_test_unicorn_oracle)
    set_tests_properties(difftest.unicorn_oracle PROPERTIES LABELS "difftest;unicorn")
endif()
