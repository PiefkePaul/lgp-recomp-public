# webgpu.h for the WebGPU renderer (ADR-0003, roadmap P4.4).
#
#   Desktop: wgpu-native, prebuilt release from GitHub, pinned by version and
#            SHA-256 (Vulkan on Linux/Windows, Metal on macOS, DX12 on Windows).
#   Android: the same release for aarch64 (Vulkan, P7.3), linked statically so
#            a program pushed over adb and the app's libmain.so need no
#            extra .so next to them.
#   iOS:     the same release's static library for arm64 devices (Metal,
#            P7.2b); linked into the app, no dylib to embed.
#   tvOS:    no release exists: build the static library from source on the
#            Mac (tools/apple/build_wgpu_native.sh tvos, P7.4) and point
#            LGP_WGPU_NATIVE_DIR at its output.
#            LGP_WGPU_NATIVE_DIR points at an unpacked release instead
#            (offline builds); it needs include/webgpu/{webgpu,wgpu}.h and lib/.
#   Web:     Emscripten's emdawnwebgpu port (Dawn's webgpu.h on top of the
#            browser's navigator.gpu), --use-port=emdawnwebgpu.
#
# Both implement the standard webgpu.h from webgpu-native/webgpu-headers.
# Result: the INTERFACE target lgp_webgpu (headers + library) and
# LGP_WEBGPU_AVAILABLE. lgp_webgpu_copy_runtime(<target>) copies the DLL next
# to an executable on Windows (no-op elsewhere).

option(LGP_ENABLE_WEBGPU "Build the WebGPU renderer (downloads wgpu-native on the desktop)" ON)
set(LGP_WGPU_NATIVE_DIR "" CACHE PATH "Unpacked wgpu-native release to use instead of downloading one")

set(LGP_WEBGPU_AVAILABLE OFF)
set(LGP_WGPU_NATIVE_VERSION "29.0.1.1")

function(lgp_webgpu_copy_runtime target)
    if(WIN32 AND TARGET wgpu_native)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:wgpu_native>" "$<TARGET_FILE_DIR:${target}>"
            VERBATIM)
    endif()
endfunction()

if(NOT LGP_ENABLE_WEBGPU)
    return()
endif()

if(EMSCRIPTEN)
    add_library(lgp_webgpu INTERFACE)
    target_compile_options(lgp_webgpu INTERFACE --use-port=emdawnwebgpu)
    target_link_options(lgp_webgpu INTERFACE --use-port=emdawnwebgpu)
    target_compile_definitions(lgp_webgpu INTERFACE LGP_WEBGPU_EMDAWN=1)
    set(LGP_WEBGPU_AVAILABLE ON)
    return()
endif()

# Release asset per host, with its SHA-256.
set(_wgpu_url "")
string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _lgp_cpu)
if(ANDROID AND _lgp_cpu MATCHES "^(aarch64|arm64)$")
    set(_wgpu_asset wgpu-android-aarch64-release.zip)
    set(_wgpu_sha 721741f1b05a20c1738166bedf7a5efb2ba4b382da689526d3fc33de22bdd573)
elseif(ANDROID AND _lgp_cpu MATCHES "^(x86_64|amd64)$")
    # Only for the x86_64 emulator image (ANDROID_ABI=x86_64), phones are arm64.
    set(_wgpu_asset wgpu-android-x86_64-release.zip)
    set(_wgpu_sha ef16fc0644bf0e308a39ac4516742da8e22d8c201d3a542cc5baf533d272c491)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND _lgp_cpu MATCHES "^(x86_64|amd64)$")
    set(_wgpu_asset wgpu-linux-x86_64-release.zip)
    set(_wgpu_sha 95a4d90c071005a98d03eab348beaa6b07e16eb00d1dcdb9f8348f75eb97ec5a)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND _lgp_cpu MATCHES "^(aarch64|arm64)$")
    set(_wgpu_asset wgpu-linux-aarch64-release.zip)
    set(_wgpu_sha 015fcdf1dbae82e614a783cc38017e5399ae0927a889fe9b69c9b664bc61b47a)
elseif(IOS)
    # Devices only (arm64); the simulator would need wgpu-ios-aarch64-simulator.
    set(_wgpu_asset wgpu-ios-aarch64-release.zip)
    set(_wgpu_sha e36c9913b9e5095a530fa9121c50b16a4e3dd020e1eebf601f2f47ce24d56941)
elseif(LGP_TVOS)
    # No release for tvOS: LGP_WGPU_NATIVE_DIR (tools/apple/build_wgpu_native.sh).
elseif(APPLE AND _lgp_cpu MATCHES "^(arm64|aarch64)$")
    set(_wgpu_asset wgpu-macos-aarch64-release.zip)
    set(_wgpu_sha a5797a37b1adf720bcd5dcffb291edbbd5b7b14be0a3874c28e6393a655a7a3e)
elseif(APPLE AND _lgp_cpu MATCHES "^(x86_64|amd64)$")
    set(_wgpu_asset wgpu-macos-x86_64-release.zip)
    set(_wgpu_sha 8e2f7378548ddd0e2cf21e7d864dda46e953f0af724855a33778b85ead206d41)
elseif(WIN32 AND MSVC AND _lgp_cpu MATCHES "^(x86_64|amd64)$")
    set(_wgpu_asset wgpu-windows-x86_64-msvc-release.zip)
    set(_wgpu_sha 7e67d7445c42aeb85e30f88930fd8d7d83ee769e3390aeb1ada75ebf3cf78132)
elseif(MINGW AND _lgp_cpu MATCHES "^(x86_64|amd64)$")
    # The GNU build imports only the UCRT (no VCRUNTIME140.dll as the MSVC build does).
    set(_wgpu_asset wgpu-windows-x86_64-gnu-release.zip)
    set(_wgpu_sha d471e3614733c1d4ddd61bfd19868356477d0d37bf531bf8c6cb64a7f579bd2a)
elseif(WIN32 AND _lgp_cpu MATCHES "^(x86_64|amd64)$")
    # clang-cl and clang with the MSVC ABI link the MSVC build too.
    set(_wgpu_asset wgpu-windows-x86_64-msvc-release.zip)
    set(_wgpu_sha 7e67d7445c42aeb85e30f88930fd8d7d83ee769e3390aeb1ada75ebf3cf78132)
endif()

# Android: the release has no Vulkan driver cache (patch driver-cache), so the
# driver compiles every pipeline again on each start. A local build with our
# patches (tools/android/build_wgpu_native.sh <checkout>
# $LGP_PRIVATE_DIR/toolchains/wgpu-native-android) is taken by default when it
# is there (arm64 only, the script builds nothing else); CI has no
# LGP_PRIVATE_DIR and keeps the release.
option(LGP_WGPU_NATIVE_AUTO
    "Android, without LGP_WGPU_NATIVE_DIR: use $LGP_PRIVATE_DIR/toolchains/wgpu-native-android if it exists" ON)
file(TO_CMAKE_PATH "$ENV{LGP_PRIVATE_DIR}" _lgp_private_dir)
set(_wgpu_auto_dir "${_lgp_private_dir}/toolchains/wgpu-native-android")
if(LGP_WGPU_NATIVE_DIR)
    set(_wgpu_dir "${LGP_WGPU_NATIVE_DIR}")
elseif(ANDROID AND _lgp_cpu MATCHES "^(aarch64|arm64)$" AND LGP_WGPU_NATIVE_AUTO AND NOT _lgp_private_dir STREQUAL ""
       AND EXISTS "${_wgpu_auto_dir}/include/webgpu/webgpu.h" AND EXISTS "${_wgpu_auto_dir}/lib/libwgpu_native.a")
    set(_wgpu_dir "${_wgpu_auto_dir}")
elseif(DEFINED _wgpu_asset)
    if(ANDROID)
        message(STATUS "WebGPU: wgpu-native release without the driver-cache patch (no Vulkan driver cache); "
                       "build it into \$LGP_PRIVATE_DIR/toolchains/wgpu-native-android or set LGP_WGPU_NATIVE_DIR")
    endif()
    include(FetchContent)
    FetchContent_Declare(wgpu_native_release
        URL "https://github.com/gfx-rs/wgpu-native/releases/download/v${LGP_WGPU_NATIVE_VERSION}/${_wgpu_asset}"
        URL_HASH SHA256=${_wgpu_sha}
        DOWNLOAD_EXTRACT_TIMESTAMP ON)
    # The archive has no CMakeLists.txt: this only downloads and unpacks.
    FetchContent_MakeAvailable(wgpu_native_release)
    set(_wgpu_dir "${wgpu_native_release_SOURCE_DIR}")
else()
    if(LGP_TVOS)
        message(WARNING "LGP_ENABLE_WEBGPU: wgpu-native has no tvOS release; build it with "
                        "tools/apple/build_wgpu_native.sh tvos and set LGP_WGPU_NATIVE_DIR. The WebGPU renderer is not built.")
    else()
        message(WARNING "LGP_ENABLE_WEBGPU: no wgpu-native release for ${CMAKE_SYSTEM_NAME}/${CMAKE_SYSTEM_PROCESSOR}; "
                        "set LGP_WGPU_NATIVE_DIR. The WebGPU renderer is not built.")
    endif()
    return()
endif()

if(ANDROID OR LGP_APPLE_MOBILE)
    set(_wgpu_lib "${_wgpu_dir}/lib/libwgpu_native.a")
elseif(WIN32)
    set(_wgpu_lib "${_wgpu_dir}/lib/wgpu_native.dll")
    if(MINGW)
        set(_wgpu_implib "${_wgpu_dir}/lib/libwgpu_native.dll.a")
    else()
        set(_wgpu_implib "${_wgpu_dir}/lib/wgpu_native.dll.lib")
    endif()
elseif(APPLE)
    set(_wgpu_lib "${_wgpu_dir}/lib/libwgpu_native.dylib")
else()
    set(_wgpu_lib "${_wgpu_dir}/lib/libwgpu_native.so")
endif()
if(NOT EXISTS "${_wgpu_dir}/include/webgpu/webgpu.h" OR NOT EXISTS "${_wgpu_lib}")
    message(WARNING "LGP_ENABLE_WEBGPU: ${_wgpu_dir} is not a wgpu-native release; the WebGPU renderer is not built.")
    return()
endif()

if(ANDROID)
    # Static Rust library: its own system dependencies come along.
    add_library(wgpu_native STATIC IMPORTED GLOBAL)
    set_target_properties(wgpu_native PROPERTIES
        IMPORTED_LOCATION "${_wgpu_lib}"
        INTERFACE_INCLUDE_DIRECTORIES "${_wgpu_dir}/include"
        INTERFACE_LINK_LIBRARIES "android;log;dl;m")
elseif(LGP_APPLE_MOBILE)
    # Static Rust library: its system dependencies come along explicitly.
    add_library(wgpu_native STATIC IMPORTED GLOBAL)
    set_target_properties(wgpu_native PROPERTIES
        IMPORTED_LOCATION "${_wgpu_lib}"
        INTERFACE_INCLUDE_DIRECTORIES "${_wgpu_dir}/include"
        INTERFACE_LINK_LIBRARIES "-framework Metal;-framework QuartzCore;-framework CoreGraphics;-framework Foundation;-framework CoreFoundation;-framework UIKit;objc")
else()
    add_library(wgpu_native SHARED IMPORTED GLOBAL)
    set_target_properties(wgpu_native PROPERTIES
        IMPORTED_LOCATION "${_wgpu_lib}"
        INTERFACE_INCLUDE_DIRECTORIES "${_wgpu_dir}/include")
endif()
if(WIN32)
    set_target_properties(wgpu_native PROPERTIES IMPORTED_IMPLIB "${_wgpu_implib}")
elseif(APPLE AND NOT LGP_APPLE_MOBILE)
    set_target_properties(wgpu_native PROPERTIES IMPORTED_SONAME "@rpath/libwgpu_native.dylib")
elseif(NOT ANDROID AND NOT LGP_APPLE_MOBILE)
    # The release .so has no SONAME; link it by name so the binary records
    # "libwgpu_native.so" (found through the build RPATH), not a build path.
    set_target_properties(wgpu_native PROPERTIES IMPORTED_NO_SONAME TRUE)
endif()

add_library(lgp_webgpu INTERFACE)
target_link_libraries(lgp_webgpu INTERFACE wgpu_native)
target_compile_definitions(lgp_webgpu INTERFACE LGP_WEBGPU_WGPU_NATIVE=1)
set(LGP_WEBGPU_AVAILABLE ON)
message(STATUS "WebGPU: wgpu-native ${LGP_WGPU_NATIVE_VERSION} from ${_wgpu_dir}")
# Android: without lazy-naga-ir every shader module keeps its naga IR, about
# 600 MiB more native heap on the NVIDIA Shield (docs/analysis/36-android-native-heap.md).
if(ANDROID AND EXISTS "${_wgpu_dir}/BUILD-INFO")
    file(STRINGS "${_wgpu_dir}/BUILD-INFO" _wgpu_build_info LIMIT_COUNT 1)
    if(NOT _wgpu_build_info MATCHES "lazy-naga-ir")
        message(WARNING "WebGPU: ${_wgpu_dir} is built without lazy-naga-ir (${_wgpu_build_info}); "
                        "rebuild it with tools/android/build_wgpu_native.sh (default patches since 2026-10-21)")
    endif()
endif()
