cmake_minimum_required(VERSION 3.25)

project(lgp_recomp
    VERSION 0.0.1
    DESCRIPTION "Static recompilation of an own Switch game dump (ARM64 -> C++ -> WebAssembly)"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
# Objective-C++ for the Apple platform code (platform/apple/*.mm). Enabled
# here: enable_language in platform/ alone left CMAKE_OBJCXX_COMPILE_OBJECT
# unset when configuring on macOS.
if(APPLE)
    enable_language(OBJCXX)
endif()

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR "In-source builds are not allowed. Use: cmake --preset default")
endif()

option(LGP_INSTALL_GIT_HOOKS "Point git core.hooksPath at .githooks (game-data guard)" ON)

# tvOS (P7.4): CMake sets IOS only for iOS, so the Apple TV gets its own
# flag; LGP_APPLE_MOBILE covers both (UIKit apps, no dylibs, no Documents).
if(CMAKE_SYSTEM_NAME STREQUAL "tvOS")
    set(LGP_TVOS ON)
else()
    set(LGP_TVOS OFF)
endif()
if(IOS OR LGP_TVOS)
    set(LGP_APPLE_MOBILE ON)
else()
    set(LGP_APPLE_MOBILE OFF)
endif()

# Hypervisor.framework test oracle (ADR-0006): only on macOS / Apple Silicon.
if(APPLE AND NOT LGP_APPLE_MOBILE AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
    set(_lgp_hv_default ON)
else()
    set(_lgp_hv_default OFF)
endif()
option(LGP_ENABLE_HV "Build the Hypervisor.framework differential-test oracle" ${_lgp_hv_default})

find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)

# Android (P7.3): test programs run on the phone over adb (tools/android/adb_run.py).
if(ANDROID AND NOT CMAKE_CROSSCOMPILING_EMULATOR)
    set(CMAKE_CROSSCOMPILING_EMULATOR "${Python3_EXECUTABLE};${CMAKE_SOURCE_DIR}/tools/android/adb_run.py")
endif()

# Activate the game-data pre-commit guard for every checkout that configures
# the project. Worktrees share the repository config, so this covers them too.
if(LGP_INSTALL_GIT_HOOKS AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
    find_package(Git QUIET)
    if(GIT_FOUND)
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" config --get core.hooksPath
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            OUTPUT_VARIABLE _lgp_hooks_path
            OUTPUT_STRIP_TRAILING_WHITESPACE
            RESULT_VARIABLE _lgp_hooks_result)
        if(NOT _lgp_hooks_path STREQUAL ".githooks")
            if(_lgp_hooks_result EQUAL 0)
                message(WARNING "core.hooksPath was '${_lgp_hooks_path}', replacing it with .githooks")
            endif()
            execute_process(
                COMMAND "${GIT_EXECUTABLE}" config core.hooksPath .githooks
                WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
            message(STATUS "Game-data guard enabled (git core.hooksPath = .githooks)")
        endif()
    endif()
endif()

# Unicorn ARM64 oracle for the differential tests (ADR-0006, test-only, GPL-2.0).
# Not on Emscripten, iOS or tvOS; on Windows off until the build there has been
# verified; not on Android (P7.3: a test oracle for the desktop, never on the phone).
if(EMSCRIPTEN OR WIN32 OR ANDROID OR LGP_APPLE_MOBILE)
    set(_lgp_unicorn_default OFF)
else()
    set(_lgp_unicorn_default ON)
endif()
option(LGP_ENABLE_UNICORN "Build the Unicorn differential-test oracle (downloads Unicorn 2.1.4)" ${_lgp_unicorn_default})
if(LGP_ENABLE_UNICORN)
    enable_language(C)
endif()

# Homebrew fixtures (tests/fixtures) need devkitA64 + libnx from devkitPro.
if(NOT EMSCRIPTEN AND DEFINED ENV{DEVKITPRO} AND EXISTS "$ENV{DEVKITPRO}/cmake/Switch.cmake")
    set(_lgp_fixtures_default ON)
else()
    set(_lgp_fixtures_default OFF)
endif()
option(LGP_BUILD_FIXTURES "Build the homebrew test fixtures with devkitA64 (needs DEVKITPRO)" ${_lgp_fixtures_default})

# ADR-0002: guest threads are host threads, on the web pthreads in Web
# Workers. Every object must be built with -pthread (shared memory, atomics),
# so this is global for Emscripten builds. PROXY_TO_PTHREAD runs main() in a
# worker, which may block (Atomics.wait) while the scheduler spawns threads;
# the pool keeps pthread_create from needing a trip through the event loop.
# EXIT_RUNTIME: without it the idle pool workers keep Node alive after main().
# -fwasm-exceptions everywhere (P6.1): the loader (lgp_recomp_core) throws,
# and setjmp/longjmp (guest faults, host_jmp.hpp) must then use the same Wasm
# exception mechanism in every object of a program, so mixing is not an option.
# -mtail-call: guaranteed tail calls for lifted code (LGP_MUSTTAIL, ADR-0004).
# Without it every B/BR to another function entry is a call + return, and
# chains of them (LGP's script interpreter jumps from handler to handler)
# grow the engine stack until Safari's worker stack overflows (Chrome 112,
# Firefox 121, Safari 18.2 support Wasm tail calls).
if(EMSCRIPTEN)
    add_compile_options(-pthread -fwasm-exceptions -mtail-call)
    add_link_options(-pthread -fwasm-exceptions -mtail-call -sPROXY_TO_PTHREAD=1 -sPTHREAD_POOL_SIZE=8 -sEXIT_RUNTIME=1)
endif()
# MinGW (llvm-mingw, the build kit's Windows toolchain): programs carry libc++,
# libunwind and winpthreads in themselves, so lgp_game.exe needs no DLL from
# the toolchain next to it (only wgpu_native.dll and the system's).
if(MINGW)
    add_link_options($<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,EXECUTABLE>:-static>)
endif()

# iOS/iPadOS and tvOS app bundles and ccache for Xcode (P7.2, P7.4). Defines
# lgp_ios_app(). First, so the dependencies below compile through ccache too.
if(LGP_APPLE_MOBILE)
    include(cmake/LgpIos.cmake)
endif()
# webgpu.h for the WebGPU renderer (P4.4): wgpu-native on the desktop,
# emdawnwebgpu on the web. Defines lgp_webgpu and LGP_WEBGPU_AVAILABLE.
include(cmake/LgpWebGpu.cmake)
# SDL3 desktop window (P4.4). Defines SDL3::SDL3-static and LGP_SDL_AVAILABLE.
include(cmake/LgpSdl.cmake)
# Android apps (P7.3): windowed programs as shared libraries in an APK.
include(cmake/LgpAndroidApk.cmake)
# libopus for hwopus (P5.2). Defines Opus::opus and LGP_OPUS_AVAILABLE.
include(cmake/LgpOpus.cmake)
# zstd for compressed RomFS images. Defines Lgp::zstd and LGP_ZSTD_AVAILABLE.
include(cmake/LgpZstd.cmake)
# Free stand-in for the system font (pl:u). Defines lgp_free_font and LGP_FREE_FONT_AVAILABLE.
include(cmake/LgpFreeFont.cmake)
# libvorbis for the sound of in-game videos (P5). Defines Lgp::vorbis and LGP_VORBIS_AVAILABLE.
include(cmake/LgpVorbis.cmake)
# libavcodec for in-game videos (P5). Defines Ffmpeg::avcodec and LGP_FFMPEG_AVAILABLE.
include(cmake/LgpFfmpeg.cmake)
# libvpx for in-game videos without FFmpeg (iOS). Defines Vpx::vpx and LGP_VPX_AVAILABLE.
include(cmake/LgpVpx.cmake)
# Mbed TLS for wss:// online play. Defines MbedTLS::mbedtls and LGP_MBEDTLS_AVAILABLE.
include(cmake/LgpMbedTls.cmake)

# P6: NaN results of the lifted FP fast paths as the host produces them
# (lgp/lifted/fp.hpp): applies to lgp_game only, not to the tests that compare
# NaN bits against an oracle. On by default after the PC check
# (docs/analysis/18-fp-host-nan-pruefung.md); -DLGP_FP_HOST_NAN=OFF restores
# exact Arm NaNs. Defined here, before runtime/ reads it for lgp_game: as an
# option in recomp/ (added after runtime/) it was still unset on the first
# configure of a new tree, so lgp_game got exact NaNs there (iOS, Android).
option(LGP_FP_HOST_NAN "lgp_game: lifted FP fast paths keep host NaN payloads" ON)
option(LGP_FP_HOST_NAN_ARM64 "lgp_game: host NaNs also on 64-bit Arm targets (off: exact Arm NaNs there)" OFF)
# Diagnosis: bit mask of the operation classes that keep host NaNs (HostNanClass
# in lgp/lifted/fp.hpp: 1 add, 2 sub, 4 mul, 8 div, 16 sqrt, 32 vector); empty = all.
set(LGP_FP_HOST_NAN_CLASSES "" CACHE STRING "lgp_game: host NaN only for these FP classes (bit mask, empty = all)")

# Components (tools/) get their own add_subdirectory() once they contain code.
add_subdirectory(platform)
add_subdirectory(runtime)
add_subdirectory(recomp)
add_subdirectory(gpu)

include(CTest)
# Memory probe for the mobile ports (P7.2a/P7.3a); no game code.
if(NOT EMSCRIPTEN)
    add_subdirectory(tools/memprobe)
endif()
if(BUILD_TESTING)
    add_subdirectory(tests)
    # Benchmarks (P6): built and smoke-tested with the tests.
    add_subdirectory(bench)
endif()
