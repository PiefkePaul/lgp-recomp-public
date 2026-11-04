# Memory probe for mobile targets (P7.2a iOS/iPadOS, P7.3a Android): reserves
# and fills the guest block like the runtime does, until the system stops it.
# No game code: it may be built anywhere, also in CI (memprobe.hpp).
#
# Inside the repository build: lgp_memprobe (command line) + test tools.memprobe.
# Standalone for the devices (docs/knowledge/speicherprobe-mobil.md):
#   iOS:     cmake -S tools/memprobe -B build/memprobe-ios -G Xcode -DCMAKE_SYSTEM_NAME=iOS
#                  -DCMAKE_OSX_DEPLOYMENT_TARGET=16.0 -DLGP_IOS_TEAM=<team id>
#            → apps lgp_memprobe_entitled (increased-memory-limit,
#              extended-virtual-addressing) and lgp_memprobe_plain (without)
#   tvOS:    the same with -DCMAKE_SYSTEM_NAME=tvOS -DCMAKE_OSX_DEPLOYMENT_TARGET=17.0
#            → lgp_memprobe_entitled has only extended-virtual-addressing
#              (tvOS offers no increased-memory-limit), log in Library/Caches;
#              lgp_memprobe_imlimit tries increased-memory-limit alone (does
#              a tvOS profile sign it, does the limit move?)
#   Android: cmake -S tools/memprobe -B build/memprobe-android -G Ninja
#                  -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake
#                  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 -DANDROID_STL=c++_static
#            → lgp_memprobe for adb shell
cmake_minimum_required(VERSION 3.25)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(lgp_memprobe LANGUAGES C CXX)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
endif()
get_filename_component(_lgp_root "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

# The runtime's own reservation code, compiled in (no other part of the
# repository is needed on the device).
set(_core "${CMAKE_CURRENT_SOURCE_DIR}/memprobe.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/native_probe.cpp"
          "${_lgp_root}/platform/os/src/host_block.cpp")
set(_include "${CMAKE_CURRENT_SOURCE_DIR}" "${_lgp_root}/platform/include")

if(CMAKE_SYSTEM_NAME STREQUAL "iOS" OR CMAKE_SYSTEM_NAME STREQUAL "tvOS")
    enable_language(OBJCXX)
    set(LGP_IOS_TEAM "" CACHE STRING "Apple development team id for signing (Xcode: Settings > Accounts)")
    set(LGP_IOS_BUNDLE_PREFIX "de.piefkepaul.lgp" CACHE STRING "Bundle id prefix (must be unique per Apple team)")
    set(_variants entitled plain)
    if(CMAKE_SYSTEM_NAME STREQUAL "tvOS")
        list(APPEND _variants imlimit)
    endif()
    foreach(variant IN LISTS _variants)
        set(t lgp_memprobe_${variant})
        add_executable(${t} MACOSX_BUNDLE "${CMAKE_CURRENT_SOURCE_DIR}/ios/main.mm" ${_core})
        target_include_directories(${t} PRIVATE ${_include})
        target_compile_options(${t} PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
        target_link_libraries(${t} PRIVATE "-framework UIKit" "-framework Foundation")
        set_target_properties(${t} PROPERTIES
            MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/ios/Info.plist.in"
            MACOSX_BUNDLE_BUNDLE_NAME "LGP Mem ${variant}"
            MACOSX_BUNDLE_GUI_IDENTIFIER "${LGP_IOS_BUNDLE_PREFIX}.memprobe.${variant}"
            XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "${LGP_IOS_BUNDLE_PREFIX}.memprobe.${variant}"
            XCODE_ATTRIBUTE_CODE_SIGN_STYLE "Automatic"
            XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "${LGP_IOS_TEAM}"
            XCODE_ATTRIBUTE_GCC_OPTIMIZATION_LEVEL "s")
        if(CMAKE_SYSTEM_NAME STREQUAL "tvOS")
            set_target_properties(${t} PROPERTIES XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "3")
            set(_entitlements "${CMAKE_CURRENT_SOURCE_DIR}/ios/memprobe-tvos.entitlements")
        else()
            set_target_properties(${t} PROPERTIES XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2")
            set(_entitlements "${CMAKE_CURRENT_SOURCE_DIR}/ios/memprobe.entitlements")
        endif()
        if(variant STREQUAL "entitled")
            target_compile_definitions(${t} PRIVATE LGP_MEMPROBE_ENTITLED=1)
            set_target_properties(${t} PROPERTIES
                XCODE_ATTRIBUTE_CODE_SIGN_ENTITLEMENTS "${_entitlements}")
        elseif(variant STREQUAL "imlimit")
            target_compile_definitions(${t} PRIVATE LGP_MEMPROBE_IMLIMIT=1)
            set_target_properties(${t} PROPERTIES
                XCODE_ATTRIBUTE_CODE_SIGN_ENTITLEMENTS "${CMAKE_CURRENT_SOURCE_DIR}/ios/memprobe-tvos-imlimit.entitlements")
        endif()
    endforeach()
    return()
endif()

if(WIN32)
    return()  # the probe is for Apple and Android devices; desktop Linux/macOS run it in CI
endif()
find_package(Threads REQUIRED)
add_executable(lgp_memprobe "${CMAKE_CURRENT_SOURCE_DIR}/main_cli.cpp" ${_core})
target_include_directories(lgp_memprobe PRIVATE ${_include})
target_link_libraries(lgp_memprobe PRIVATE Threads::Threads)
target_compile_options(lgp_memprobe PRIVATE -Wall -Wextra)

if(NOT CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR AND BUILD_TESTING AND NOT CMAKE_CROSSCOMPILING)
    # A small run: the guest block is reserved like the runtime's and filled.
    add_test(NAME tools.memprobe COMMAND lgp_memprobe --guest 64 --chunk 16 --max 96 --pause 0)
    set_tests_properties(tools.memprobe PROPERTIES LABELS "tools"
        PASS_REGULAR_EXPRESSION "guest block: all 64 MiB written.*reached the limit of 96 MiB")
    # --max below one chunk writes exactly --max, not a whole chunk.
    add_test(NAME tools.memprobe_max COMMAND lgp_memprobe --guest 64 --chunk 64 --max 1 --pause 0)
    set_tests_properties(tools.memprobe_max PROPERTIES LABELS "tools"
        PASS_REGULAR_EXPRESSION "guest: written 1 MiB.*reached the limit of 1 MiB"
        FAIL_REGULAR_EXPRESSION "written 64 MiB")
    # A log that cannot be written stops the probe (exit 2) before it starts.
    add_test(NAME tools.memprobe_bad_log
             COMMAND lgp_memprobe --guest 16 --max 16 --pause 0 --log "${CMAKE_CURRENT_BINARY_DIR}/missing-dir/probe.log")
    set_tests_properties(tools.memprobe_bad_log PROPERTIES LABELS "tools" WILL_FAIL TRUE)
    # File mode: the guest block is a shared mapping of an unlinked sparse file.
    add_test(NAME tools.memprobe_file
             COMMAND lgp_memprobe --guest 32 --chunk 16 --max 48 --pause 0 --file "${CMAKE_CURRENT_BINARY_DIR}" --file-sync)
    set_tests_properties(tools.memprobe_file PROPERTIES LABELS "tools"
        PASS_REGULAR_EXPRESSION "file-backed .msync per chunk.*mapped from a sparse file.*all 32 MiB written.*reached the limit of 48 MiB")
    # First-touch timing (tvOS load stall): every variant reports a first write.
    add_test(NAME tools.memprobe_touch
             COMMAND lgp_memprobe --touch 8 --chunk 4 --file "${CMAKE_CURRENT_BINARY_DIR}")
    set_tests_properties(tools.memprobe_touch PROPERTIES LABELS "tools"
        PASS_REGULAR_EXPRESSION "touch anonymous: .*first write.*touch sparse file: .*first write.*touch file written first: .*first write"
        FAIL_REGULAR_EXPRESSION "FAILED")
    # Native probe: a page of code from a file at a fixed address below 2^39 runs
    # (the Android route; the Apple route vm_remap needs a Mac or a device).
    add_test(NAME tools.memprobe_native COMMAND lgp_memprobe --native --native-gib 1 --file "${CMAKE_CURRENT_BINARY_DIR}")
    # macOS refuses executable mappings of unsigned files; there the probe
    # runs the code from anonymous memory made executable instead.
    if(APPLE)
        set(_native_route "copied code: runs, result ok")
    else()
        set(_native_route "file-mapped code: runs, result ok")
    endif()
    set_tests_properties(tools.memprobe_native PROPERTIES LABELS "tools"
        PASS_REGULAR_EXPRESSION "native probe: base 0x[0-9a-f]+.*${_native_route}.*native probe: done"
        FAIL_REGULAR_EXPRESSION "WRONG")
endif()
