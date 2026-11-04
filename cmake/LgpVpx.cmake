# libvpx for in-game videos (P5/P7, movie:: HLE): VP9 decoded in software on
# platforms without a system decoder, above all iOS (ADR-0008: the iOS build
# had black videos). BSD-3-Clause, see THIRD_PARTY.md.
#
# Built from the pinned 1.17.0 release tarball with libvpx's own configure
# script (ExternalProject, VP9 decoder only, static, no programs/tests/docs).
# LGP_VPX_DIR points at an unpacked libvpx-1.17.0 tree instead (offline).
# Needs a POSIX shell, make and perl; x86_64 additionally nasm or yasm, else
# the portable C build (generic-gnu, slower) is used (so on CI runners without
# nasm/yasm).
# Default on only for iOS (no FFmpeg, no system decoder used). Can be switched
# on for macOS, Linux (CI runs the decoder test that way) and Android (NDK
# toolchain; a fallback where MediaCodec hands out tiled formats, the default
# there stays MediaCodec, lgp_platform_android). Not for Windows (configure
# wants MSYS, also when cross-building for Android on a Windows host) or
# Emscripten (WebCodecs). tvOS and the iOS simulator: not yet, libvpx's
# configure only knows the iPhoneOS SDK for Apple devices and x86 simulators.
# On iOS configure also adds -fembed-bitcode (ignored by current Xcode) and a
# minimum of iOS 7, overridden here with the project's deployment target.
# Only the compilers, the target and the Apple deployment target/arch are
# passed on, not CMAKE_C_FLAGS: libvpx picks its own optimisation flags.
# Result: target Vpx::vpx and LGP_VPX_AVAILABLE.

set(_lgp_vpx_default OFF)
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(_lgp_vpx_default ON)
endif()
option(LGP_ENABLE_VPX "Decode VP9 videos with libvpx (downloads libvpx 1.17.0)" ${_lgp_vpx_default})
set(LGP_VPX_DIR "" CACHE PATH "Unpacked libvpx-1.17.0 source tree to use instead of downloading it")

set(LGP_VPX_AVAILABLE OFF)
if(NOT LGP_ENABLE_VPX)
    return()
endif()

if(WIN32 OR CMAKE_HOST_WIN32 OR EMSCRIPTEN OR CMAKE_SYSTEM_NAME STREQUAL "tvOS")
    message(FATAL_ERROR "LGP_ENABLE_VPX: libvpx is not built for ${CMAKE_SYSTEM_NAME} on a "
                        "${CMAKE_HOST_SYSTEM_NAME} host here (see cmake/LgpVpx.cmake)")
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "iOS" AND (CMAKE_OSX_SYSROOT MATCHES "[Ss]imulator"
                                         OR CMAKE_OSX_ARCHITECTURES MATCHES "x86_64"))
    message(FATAL_ERROR "LGP_ENABLE_VPX: the iOS simulator is not set up (libvpx has x86 simulator targets only); "
                        "configure with -DLGP_ENABLE_VPX=OFF")
endif()
list(LENGTH CMAKE_OSX_ARCHITECTURES _vpx_arch_count)
if(APPLE AND _vpx_arch_count GREATER 1)
    message(FATAL_ERROR "LGP_ENABLE_VPX: libvpx is built for one architecture, "
                        "CMAKE_OSX_ARCHITECTURES is '${CMAKE_OSX_ARCHITECTURES}'")
endif()
if(LGP_VPX_DIR)
    file(STRINGS "${LGP_VPX_DIR}/CHANGELOG" _vpx_release LIMIT_COUNT 1)
    if(NOT _vpx_release MATCHES "v1\\.17\\.0 ")
        message(FATAL_ERROR "LGP_ENABLE_VPX: LGP_VPX_DIR must be libvpx 1.17.0, CHANGELOG starts with '${_vpx_release}'")
    endif()
endif()
find_program(LGP_MAKE_PROGRAM NAMES gmake make REQUIRED)
find_program(LGP_PERL_PROGRAM perl REQUIRED)

# libvpx's configure target for this build.
set(_vpx_env "")
set(_vpx_target generic-gnu)
string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _vpx_cpu)
if(APPLE AND CMAKE_OSX_ARCHITECTURES)
    string(TOLOWER "${CMAKE_OSX_ARCHITECTURES}" _vpx_cpu)  # e.g. x86_64 on an arm64 Mac
endif()
set(_vpx_extra_cflags "")
if(CMAKE_SYSTEM_NAME STREQUAL "iOS" AND LGP_IOS_CROSS)
    # No xcrun (preset ios-cross): libvpx's darwin targets call xcrun for compiler and SDK, so the plain arm64
    # target (NEON, no hand-written assembly in the VP9 decoder; runtime CPU detection takes sysctl under
    # __APPLE__) with the toolchain's clang, SDK and ld64.lld in CC/CXX/LD (below). Draft, not yet built with a
    # real SDK: the first PC run linked with the host's ld and g++ ("unable to link executables").
    set(_vpx_target arm64-linux-gcc)
    # Target and SDK ride in CC (below).
elseif(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(_vpx_target arm64-darwin-gcc)  # iPhoneOS SDK via xcrun, NEON
    if(CMAKE_OSX_DEPLOYMENT_TARGET)
        set(_vpx_extra_cflags "-miphoneos-version-min=${CMAKE_OSX_DEPLOYMENT_TARGET}")
    endif()
elseif(ANDROID)
    if(NOT CMAKE_ANDROID_ARCH_ABI STREQUAL "arm64-v8a")
        message(FATAL_ERROR "LGP_ENABLE_VPX: only arm64-v8a is set up for Android")
    endif()
    set(_vpx_target arm64-android-gcc)
elseif(APPLE)
    if(CMAKE_OSX_DEPLOYMENT_TARGET)
        set(_vpx_extra_cflags "-mmacosx-version-min=${CMAKE_OSX_DEPLOYMENT_TARGET}")
    endif()
    if(_vpx_cpu MATCHES "arm64|aarch64")
        set(_vpx_target arm64-darwin20-gcc)  # darwin20 targets add -arch themselves
    else()
        find_program(LGP_NASM_PROGRAM NAMES nasm yasm)
        if(LGP_NASM_PROGRAM)
            set(_vpx_target x86_64-darwin20-gcc)
        endif()
    endif()
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    if(_vpx_cpu MATCHES "aarch64|arm64")
        set(_vpx_target arm64-linux-gcc)
    elseif(_vpx_cpu MATCHES "x86_64|amd64")
        find_program(LGP_NASM_PROGRAM NAMES nasm yasm)
        if(LGP_NASM_PROGRAM)
            set(_vpx_target x86_64-linux-gcc)
        endif()
    endif()
endif()
if(ANDROID)
    # The NDK's clang for the app's ABI and API level; clang also assembles
    # (no -c in ASFLAGS: the arm64 decoder has no hand-written assembly today).
    set(_vpx_cc "${CMAKE_C_COMPILER} --target=${CMAKE_C_COMPILER_TARGET}")
    set(_vpx_env "CC=${_vpx_cc}" "CXX=${CMAKE_CXX_COMPILER} --target=${CMAKE_CXX_COMPILER_TARGET}"
                 "AS=${_vpx_cc}" "LD=${_vpx_cc}" AR=${CMAKE_AR} NM=${CMAKE_NM} STRIP=${CMAKE_STRIP})
elseif(CMAKE_SYSTEM_NAME STREQUAL "iOS" AND LGP_IOS_CROSS)
    set(_vpx_flags "--target=arm64-apple-ios${CMAKE_OSX_DEPLOYMENT_TARGET} -isysroot ${CMAKE_OSX_SYSROOT} -fuse-ld=lld --ld-path=${LGP_IOS_LD64}")
    set(_vpx_cc "${CMAKE_C_COMPILER} ${_vpx_flags}")
    set(_vpx_env "CC=${_vpx_cc}" "CXX=${CMAKE_CXX_COMPILER} ${_vpx_flags}" "AS=${_vpx_cc}" "LD=${_vpx_cc}"
                 AR=${CMAKE_AR} NM=${CMAKE_NM} STRIP=${CMAKE_STRIP})
elseif(NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
    # The project's compilers (arm64-darwin-gcc takes the iPhoneOS SDK's own).
    set(_vpx_env CC=${CMAKE_C_COMPILER} CXX=${CMAKE_CXX_COMPILER} AR=${CMAKE_AR})
endif()

set(_vpx_extra "")
if(_vpx_extra_cflags)
    set(_vpx_extra "--extra-cflags=${_vpx_extra_cflags}")
endif()
cmake_host_system_information(RESULT _vpx_jobs QUERY NUMBER_OF_LOGICAL_CORES)
if(NOT _vpx_jobs GREATER 0)
    set(_vpx_jobs 4)
endif()

set(_vpx_prefix "${CMAKE_BINARY_DIR}/_deps/libvpx-install")
set(_vpx_lib "${_vpx_prefix}/lib/libvpx.a")
if(LGP_VPX_DIR)
    set(_vpx_source SOURCE_DIR "${LGP_VPX_DIR}")
else()
    set(_vpx_source
        URL https://storage.googleapis.com/downloads.webmproject.org/releases/webm/libvpx-1.17.0.tar.gz
        URL_HASH SHA256=7f98aae5c52c46e85b7b26feabcab9b1554605e771bd1df1d6e6a00ee22fa8ed
        DOWNLOAD_EXTRACT_TIMESTAMP ON)
endif()

include(ExternalProject)
ExternalProject_Add(libvpx_build
    ${_vpx_source}
    PREFIX "${CMAKE_BINARY_DIR}/_deps/libvpx"
    CONFIGURE_COMMAND ${CMAKE_COMMAND} -E env ${_vpx_env}
        <SOURCE_DIR>/configure --target=${_vpx_target} --prefix=${_vpx_prefix}
        --disable-examples --disable-tools --disable-docs --disable-unit-tests
        --disable-vp8 --disable-vp9-encoder --enable-vp9-decoder
        --disable-webm-io --disable-libyuv --enable-pic --enable-multithread
        --disable-install-bins --disable-install-srcs ${_vpx_extra}
    # BUILD_ROOT=.: libvpx's Makefile takes BUILD_ROOT from the environment
    # (BUILD_ROOT?=.) for its include path, and Xcode exports BUILD_ROOT
    # (its products folder) to every build step, so vpx_config.h went missing.
    BUILD_COMMAND ${LGP_MAKE_PROGRAM} -j${_vpx_jobs} BUILD_ROOT=.
    INSTALL_COMMAND ${LGP_MAKE_PROGRAM} install BUILD_ROOT=.
    BUILD_BYPRODUCTS "${_vpx_lib}"
    LOG_CONFIGURE ON
    LOG_BUILD ON
    LOG_INSTALL ON
    LOG_OUTPUT_ON_FAILURE ON)

file(MAKE_DIRECTORY "${_vpx_prefix}/include")
add_library(Vpx::vpx STATIC IMPORTED GLOBAL)
set_target_properties(Vpx::vpx PROPERTIES IMPORTED_LOCATION "${_vpx_lib}"
                                          INTERFACE_INCLUDE_DIRECTORIES "${_vpx_prefix}/include")
find_package(Threads REQUIRED)
target_link_libraries(Vpx::vpx INTERFACE Threads::Threads)
add_dependencies(Vpx::vpx libvpx_build)
set(LGP_VPX_AVAILABLE ON)
message(STATUS "libvpx video decoding: on (${_vpx_target})")
