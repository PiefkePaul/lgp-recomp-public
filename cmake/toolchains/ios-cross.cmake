# iOS/iPadOS device build WITHOUT Xcode, from Linux or WSL (preset ios-cross, draft,
# docs/knowledge/ios-ohne-mac.md). Unverified: no run with a real SDK yet.
#
# Needs the iPhoneOS SDK from the user's own Xcode.xip (Apple's licence allows Xcode only on Apple hardware;
# nobody hands the SDK on, each user unpacks it from their own download, e.g. with `xtool sdk install Xcode.xip`),
# and host LLVM ≥ 19 with clang, ld64.lld, llvm-ar, llvm-ranlib (the SDK 27 libc++ uses __builtin_clzg and
# __GCC_DESTRUCTIVE_SIZE, which clang 18 lacks; built with clang 21 on the PC, 2026-10-25):
#   LGP_IOS_SDK  path to iPhoneOS<version>.sdk (cache variable or environment); without it the toolchain looks in
#                xtool's Swift SDK bundle (~/.swiftpm/swift-sdks/darwin.artifactbundle).
#   LGP_LLVM_SUFFIX  e.g. -21 for clang-21, ld64.lld-21 (Debian/Ubuntu packages); default: the newest found.
# Signing is not part of the build: tools/ios/make_ipa.py packs the .app, xtool signs and installs it.
set(CMAKE_SYSTEM_NAME iOS)
set(CMAKE_SYSTEM_PROCESSOR arm64)
set(CMAKE_OSX_ARCHITECTURES arm64 CACHE STRING "")
set(CMAKE_OSX_DEPLOYMENT_TARGET 16.0 CACHE STRING "")
set(LGP_IOS_CROSS ON CACHE BOOL "iOS build without Xcode (cmake/toolchains/ios-cross.cmake)" FORCE)

set(LGP_IOS_SDK "$ENV{LGP_IOS_SDK}" CACHE PATH "iPhoneOS SDK from the user's own Xcode.xip")
if(NOT LGP_IOS_SDK)
    file(GLOB _lgp_sdks "$ENV{HOME}/.swiftpm/swift-sdks/darwin.artifactbundle/Developer/Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS*.sdk"
                        "$ENV{HOME}/.swiftpm/swift-sdks/darwin.artifactbundle/*/Developer/Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS*.sdk")
    list(SORT _lgp_sdks)
    list(REVERSE _lgp_sdks)
    foreach(_sdk IN LISTS _lgp_sdks)
        if(NOT IS_SYMLINK "${_sdk}" OR NOT LGP_IOS_SDK)  # prefer a versioned folder over the iPhoneOS.sdk link
            set(LGP_IOS_SDK "${_sdk}" CACHE PATH "iPhoneOS SDK from the user's own Xcode.xip" FORCE)
        endif()
    endforeach()
endif()
if(NOT EXISTS "${LGP_IOS_SDK}/SDKSettings.json" AND NOT EXISTS "${LGP_IOS_SDK}/SDKSettings.plist")
    message(FATAL_ERROR "ios-cross: no iPhoneOS SDK (LGP_IOS_SDK=${LGP_IOS_SDK}). Unpack it from your own Xcode.xip, "
                        "e.g. `xtool sdk install Xcode.xip`, or set LGP_IOS_SDK.")
endif()
# lld needs plain arm64 in the SDK's .tbd stubs (iOS 27 lists mostly arm64e/arm64e.x1): a patched copy outside the
# build tree, made once (tools/ios/patch_sdk.py; the SDK itself is used if nothing needs a change).
find_program(_lgp_python NAMES python3 python REQUIRED)
execute_process(COMMAND "${_lgp_python}" "${CMAKE_CURRENT_LIST_DIR}/../../tools/ios/patch_sdk.py" ensure "${LGP_IOS_SDK}"
                OUTPUT_VARIABLE _lgp_sysroot OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE _lgp_rc)
if(NOT _lgp_rc EQUAL 0 OR NOT IS_DIRECTORY "${_lgp_sysroot}")
    message(FATAL_ERROR "ios-cross: tools/ios/patch_sdk.py failed for ${LGP_IOS_SDK}")
endif()
set(CMAKE_OSX_SYSROOT "${_lgp_sysroot}" CACHE PATH "" FORCE)

set(LGP_LLVM_SUFFIX "$ENV{LGP_LLVM_SUFFIX}" CACHE STRING "Suffix of the host LLVM tools, e.g. -21")
# Debian/Ubuntu's versioned names first, newest first (Ubuntu 24.04's plain clang is 18, too old), then plain names
# (ld64.lld comes only as ld64.lld-21 and the like there).
file(GLOB _lgp_llvm_dirs "/usr/lib/llvm-*/bin")
list(SORT _lgp_llvm_dirs COMPARE NATURAL ORDER DESCENDING)
function(_lgp_find_llvm var name)
    set(_names "${name}${LGP_LLVM_SUFFIX}")
    if(NOT LGP_LLVM_SUFFIX)
        set(_names "")
        foreach(_v RANGE 25 19 -1)
            list(APPEND _names "${name}-${_v}")
        endforeach()
        list(APPEND _names "${name}")
    endif()
    find_program(${var} NAMES ${_names} HINTS ${_lgp_llvm_dirs} ${ARGN})
endfunction()
_lgp_find_llvm(_lgp_clang clang REQUIRED)
execute_process(COMMAND "${_lgp_clang}" --version OUTPUT_VARIABLE _lgp_clang_version ERROR_QUIET)
if(_lgp_clang_version MATCHES "clang version ([0-9]+)" AND CMAKE_MATCH_1 LESS 19)
    message(FATAL_ERROR "ios-cross: ${_lgp_clang} is clang ${CMAKE_MATCH_1}; the iPhoneOS SDK's libc++ needs clang "
                        "≥ 19 (checked with 21), e.g. `sudo apt install clang-21 lld-21 llvm-21` and "
                        "-DLGP_LLVM_SUFFIX=-21.")
endif()
_lgp_find_llvm(_lgp_clangxx clang++ REQUIRED)
_lgp_find_llvm(_lgp_ld64 ld64.lld REQUIRED)
_lgp_find_llvm(CMAKE_AR llvm-ar REQUIRED)
_lgp_find_llvm(CMAKE_RANLIB llvm-ranlib REQUIRED)
_lgp_find_llvm(CMAKE_INSTALL_NAME_TOOL llvm-install-name-tool)
_lgp_find_llvm(CMAKE_NM llvm-nm)
_lgp_find_llvm(CMAKE_STRIP llvm-strip)
set(CMAKE_C_COMPILER "${_lgp_clang}")
set(CMAKE_CXX_COMPILER "${_lgp_clangxx}")
set(CMAKE_OBJC_COMPILER "${_lgp_clang}")
set(CMAKE_OBJCXX_COMPILER "${_lgp_clangxx}")
set(_lgp_target "arm64-apple-ios${CMAKE_OSX_DEPLOYMENT_TARGET}")
foreach(_lang C CXX OBJC OBJCXX ASM)
    set(CMAKE_${_lang}_COMPILER_TARGET "${_lgp_target}")
endforeach()
# ld64.lld is lld's Mach-O port. clang only passes -platform_version and -arch (which lld requires) with
# -fuse-ld=lld; --ld-path then names the binary, so Debian's versioned ld64.lld-18 works too (checked with
# clang 18: Mach-O arm64 executable).
foreach(_kind EXE SHARED MODULE)
    string(APPEND CMAKE_${_kind}_LINKER_FLAGS_INIT " -fuse-ld=lld --ld-path=${_lgp_ld64}")
endforeach()
set(LGP_IOS_LD64 "${_lgp_ld64}" CACHE FILEPATH "ld64.lld of the ios-cross toolchain (also for libvpx)" FORCE)
foreach(_lang C CXX OBJC OBJCXX)
    string(APPEND CMAKE_${_lang}_FLAGS_INIT " -include ${CMAKE_CURRENT_LIST_DIR}/ios-cross-prelude.h")
endforeach()

# Libraries and headers only from the SDK, programs from the host.
# The patched copy, else find_library hands out full paths to the original .tbd files lld rejects (PC, 2026-10-25).
set(CMAKE_FIND_ROOT_PATH "${CMAKE_OSX_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
# try_compile cannot link an app bundle without signing; static libraries are enough to probe the compiler.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
# try_compile() loads this file again in a fresh project: hand on the overrides given as -D (Codex on #473).
foreach(_lgp_var IN ITEMS LGP_IOS_SDK LGP_LLVM_SUFFIX)
    if(NOT _lgp_var IN_LIST CMAKE_TRY_COMPILE_PLATFORM_VARIABLES)
        list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES ${_lgp_var})
    endif()
endforeach()
