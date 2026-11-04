# Android apps (P7.3, docs/knowledge/android-ndk-build.md).
#
# With SDL3 on Android (preset android-app), a windowed program is a shared
# library that SDL's Java activity loads; tools/android/build_apk.py packs it
# with SDL's Java sources and platform/android/java into a signed debug APK
# (no Gradle). Needs the Android SDK (platform + build-tools; LGP_ANDROID_SDK,
# else ANDROID_HOME/ANDROID_SDK_ROOT) and a JDK.
#
#   lgp_add_sdl_program(<target> <sources>...)
#       add_executable(), or on an Android app build a shared library whose
#       main() is called through platform/android/src/sdl_main_android.cpp.
#   lgp_add_android_apk(<target> PACKAGE <id> LABEL <name> [ASSETS <name>=<file>...])
#       <target>.apk next to the library (target <target>_apk, built by ALL);
#       ASSETS go into the APK's assets/.
#
# LGP_ANDROID_APP: ON when these build apps.
# LGP_ANDROID_KEYSTORE: signing keystore, default $LGP_PRIVATE_DIR/android/lgp-debug.keystore
# (without LGP_PRIVATE_DIR: <build>/lgp-debug.keystore).
# LGP_ANDROID_STRIP_DEBUG: ON (default) packs the library without its debug sections
# (the NDK compiles with -g in every build type); the one in the build tree keeps them.
# LGP_ANDROID_EXTRACT_NATIVE_LIBS: OFF stores the library uncompressed and page-aligned in
# the APK and Android loads it from there (no second, extracted copy on /data).

set(LGP_ANDROID_APP OFF)
if(ANDROID AND LGP_SDL_AVAILABLE)
    set(LGP_ANDROID_APP ON)
endif()

function(lgp_add_sdl_program target)
    if(LGP_ANDROID_APP)
        add_library(${target} SHARED ${ARGN} "${PROJECT_SOURCE_DIR}/platform/android/src/sdl_main_android.cpp")
        target_compile_definitions(${target} PRIVATE LGP_MAIN_NAME=lgp_program_main)
        target_link_libraries(${target} PRIVATE SDL3::SDL3-static)
    else()
        add_executable(${target} ${ARGN})
    endif()
endfunction()

if(NOT LGP_ANDROID_APP)
    function(lgp_add_android_apk)
    endfunction()
    return()
endif()

option(LGP_ANDROID_STRIP_DEBUG "Android: library in the APK without debug sections (symbols stay)" ON)
option(LGP_ANDROID_EXTRACT_NATIVE_LIBS
    "Android: extract the native library at install (OFF: load it uncompressed from the APK, saves its size on /data)" ON)
set(LGP_ANDROID_SDK "" CACHE PATH "Android SDK (platforms/, build-tools/) for the APKs; default: ANDROID_HOME")
set(_lgp_sdk "${LGP_ANDROID_SDK}")
if(NOT _lgp_sdk AND DEFINED ENV{ANDROID_HOME})
    set(_lgp_sdk "$ENV{ANDROID_HOME}")
elseif(NOT _lgp_sdk AND DEFINED ENV{ANDROID_SDK_ROOT})
    set(_lgp_sdk "$ENV{ANDROID_SDK_ROOT}")
endif()
find_package(Java COMPONENTS Development)
if(NOT _lgp_sdk OR NOT EXISTS "${_lgp_sdk}/build-tools" OR NOT EXISTS "${_lgp_sdk}/platforms" OR NOT Java_FOUND)
    message(WARNING "Android apps: no Android SDK (set LGP_ANDROID_SDK or ANDROID_HOME) or no JDK; "
                    "the libraries are built, the APKs are not.")
    function(lgp_add_android_apk)
    endfunction()
    return()
endif()
get_filename_component(_lgp_java_bin "${Java_JAVAC_EXECUTABLE}" DIRECTORY)
find_program(LGP_KEYTOOL keytool HINTS "${_lgp_java_bin}" REQUIRED)
set(LGP_ANDROID_SDK_DIR "${_lgp_sdk}" CACHE INTERNAL "")
set(LGP_ANDROID_JAVA_SOURCES "${sdl3_SOURCE_DIR}/android-project/app/src/main/java"
    "${PROJECT_SOURCE_DIR}/platform/android/java" CACHE INTERNAL "")
# One signing key for every build tree and machine: Android only updates an installed app signed with the same key,
# and the way out of INSTALL_FAILED_UPDATE_INCOMPATIBLE (uninstalling) deletes the app's saves.
if(DEFINED ENV{LGP_PRIVATE_DIR} AND NOT "$ENV{LGP_PRIVATE_DIR}" STREQUAL "")
    file(TO_CMAKE_PATH "$ENV{LGP_PRIVATE_DIR}/android/lgp-debug.keystore" _lgp_ks_default)
else()
    set(_lgp_ks_default "${CMAKE_BINARY_DIR}/lgp-debug.keystore")
endif()
set(LGP_ANDROID_KEYSTORE "${_lgp_ks_default}" CACHE FILEPATH
    "Debug keystore that signs the APKs (created on first use); default: $LGP_PRIVATE_DIR/android/lgp-debug.keystore")
if(NOT EXISTS "${LGP_ANDROID_KEYSTORE}")
    message(WARNING "Android apps: ${LGP_ANDROID_KEYSTORE} does not exist; the first APK creates a NEW key. "
                    "An app installed with another key can then only be replaced by uninstalling it, which deletes "
                    "its saves. Copy the existing keystore there first (docs/knowledge/android-ndk-build.md).")
endif()
message(STATUS "Android apps: SDK ${_lgp_sdk}, JDK ${Java_VERSION_STRING}, keystore ${LGP_ANDROID_KEYSTORE}")

function(lgp_add_android_apk target)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "PACKAGE;LABEL" "ASSETS")
    set(LGP_APK_PACKAGE "${arg_PACKAGE}")
    set(LGP_APK_LABEL "${arg_LABEL}")
    set(LGP_APK_LIBRARY "${target}")
    set(pack_args "")
    if(LGP_ANDROID_EXTRACT_NATIVE_LIBS)
        set(LGP_APK_EXTRACT_NATIVE_LIBS "true")
    else()
        set(LGP_APK_EXTRACT_NATIVE_LIBS "false")
        set(pack_args --store-libs)
    endif()
    if(LGP_ANDROID_STRIP_DEBUG)
        if(NOT CMAKE_STRIP)
            message(FATAL_ERROR "LGP_ANDROID_STRIP_DEBUG: no CMAKE_STRIP (llvm-strip of the NDK)")
        endif()
        list(APPEND pack_args --strip "${CMAKE_STRIP}")
    endif()
    set(manifest "${CMAKE_CURRENT_BINARY_DIR}/${target}-apk/AndroidManifest.xml")
    configure_file("${PROJECT_SOURCE_DIR}/platform/android/AndroidManifest.xml.in" "${manifest}" @ONLY)
    set(java_args "")
    set(java_deps "")
    foreach(dir IN LISTS LGP_ANDROID_JAVA_SOURCES)
        list(APPEND java_args --java-src "${dir}")
        file(GLOB_RECURSE _java CONFIGURE_DEPENDS "${dir}/*.java")
        list(APPEND java_deps ${_java})
    endforeach()
    set(asset_args "")
    set(asset_deps "")
    foreach(asset IN LISTS arg_ASSETS)
        string(FIND "${asset}" "=" _eq)
        math(EXPR _from "${_eq} + 1")
        string(SUBSTRING "${asset}" ${_from} -1 _file)
        list(APPEND asset_args --asset "${asset}")
        list(APPEND asset_deps "${_file}")
    endforeach()
    set(apk "${CMAKE_CURRENT_BINARY_DIR}/${target}.apk")
    # A keystore copied over the old one (the right key restored) re-signs the
    # APK. Only an existing one can be a dependency: the first APK creates it.
    set(keystore_deps "")
    if(EXISTS "${LGP_ANDROID_KEYSTORE}")
        set(keystore_deps "${LGP_ANDROID_KEYSTORE}")
    endif()
    set(res_dir "${PROJECT_SOURCE_DIR}/platform/android/res")
    file(GLOB_RECURSE res_deps CONFIGURE_DEPENDS "${res_dir}/*")
    add_custom_command(
        OUTPUT "${apk}"
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/android/build_apk.py"
                --sdk "${LGP_ANDROID_SDK_DIR}" --manifest "${manifest}" ${java_args}
                --res "${res_dir}" --lib "$<TARGET_FILE:${target}>" --abi "${ANDROID_ABI}" --out "${apk}"
                --keystore "${LGP_ANDROID_KEYSTORE}"
                --javac "${Java_JAVAC_EXECUTABLE}" --jar "${Java_JAR_EXECUTABLE}" --keytool "${LGP_KEYTOOL}"
                --min-sdk "${ANDROID_PLATFORM_LEVEL}" ${pack_args} ${asset_args}
        DEPENDS ${target} "${manifest}" ${java_deps} ${res_deps} ${asset_deps} ${keystore_deps}
                "${PROJECT_SOURCE_DIR}/tools/android/build_apk.py"
        COMMENT "Packing ${target}.apk"
        VERBATIM)
    add_custom_target(${target}_apk ALL DEPENDS "${apk}")
endfunction()
