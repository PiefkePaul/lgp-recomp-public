# Boot program (P3.4): loader + kernel + HLE services + recompiled code.
# lgp_boot is the library; every set of lifted code gets its own executable
# via lgp_add_boot_executable(), because the lifted functions are linked in.
#
# The game: configure with -DLGP_GAME_LIFTED=<dir or list of .cpp> (lgp_lift
# output, e.g. `lgp_lift … -o $LGP_PRIVATE_DIR/build/lifted/lifted.cpp
# --split 64`; generated from the game's modules, so it stays in
# $LGP_PRIVATE_DIR and is never committed) to build lgp_game.
#
# On Emscripten (P6.1) every boot executable becomes a page (lgp_add_web_page,
# platform/web): <target>.html with the local file picker, data from OPFS.

add_library(lgp_boot STATIC src/app.cpp src/boot.cpp src/import_shims.cpp src/movie_hle.cpp src/keyboard.cpp src/ngc_hle.cpp
            src/npdm.cpp             src/press_script.cpp src/save_backup.cpp)
target_include_directories(lgp_boot PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(lgp_boot PUBLIC lgp_runtime_hle lgp_runtime_core lgp_gpu_nvn lgp_lifted_headers
                               PRIVATE lgp_recomp_core lgp_platform_common lgp_runtime_native)
target_link_libraries(lgp_boot PUBLIC lgp_mods)
if(MINGW)
    # localtime_r (app.cpp, save_backup.cpp, main.cpp of each boot program): mingw-w64 declares it only with this.
    target_compile_definitions(lgp_boot PUBLIC _POSIX_THREAD_SAFE_FUNCTIONS)
endif()
# The RyuLDN server the app goes online with while settings.ini names none
# (ldn_server =, lgp/boot/app.hpp; docs/knowledge/ldn-ueber-ryuldn.md): Paul's
# own server behind a Cloudflare tunnel, encrypted (wss://) when the build has
# TLS (Mbed TLS). Empty: online only with a server set.
if(LGP_MBEDTLS_AVAILABLE)
    set(_lgp_ldn_server "wss://lgp.server-work.de")
else()
    set(_lgp_ldn_server "ws://lgp.server-work.de")
endif()
set(LGP_LDN_SERVER_DEFAULT "${_lgp_ldn_server}" CACHE STRING
    "RyuLDN server of the app when settings.ini names none (HOST[:PORT] or ws[s]://HOST[:PORT][/PATH], empty: none)")
# Trees configured before wss:// keep the old default in their cache; ws:// as
# a fallback goes in settings.ini (ldn_server =) or the menu instead.
if(LGP_MBEDTLS_AVAILABLE AND LGP_LDN_SERVER_DEFAULT STREQUAL "ws://lgp.server-work.de")
    set_property(CACHE LGP_LDN_SERVER_DEFAULT PROPERTY VALUE "${_lgp_ldn_server}")
endif()
target_compile_definitions(lgp_boot PRIVATE "LGP_LDN_SERVER_DEFAULT=\"${LGP_LDN_SERVER_DEFAULT}\"")
# Mods that are on while settings.ini has no mods line, i.e. on the first
# start (ids separated by spaces; the build kit, tools/kit/build.py, sets the
# kit's mods). Empty: none.
set(LGP_APP_DEFAULT_MODS "" CACHE STRING "Mods on while settings.ini has no mods line (ids separated by spaces)")
target_compile_definitions(lgp_boot PRIVATE "LGP_APP_DEFAULT_MODS=\"${LGP_APP_DEFAULT_MODS}\"")
if(MSVC)
    target_compile_options(lgp_boot PRIVATE /W4)
else()
    target_compile_options(lgp_boot PRIVATE -Wall -Wextra)
endif()

# lgp_add_boot_executable(<target> <lifted.cpp>... [APP] [ENTRIES_NAME <name>] [WEB_ARGS <argv>])
# lgp_add_boot_executable(<target> NATIVE [APP]): no lifted code, the original
# code runs natively (--native is the default; AArch64 hosts, docs/analysis/56).
# WEB_ARGS: appended to the default argv of the Emscripten page.
# APP (desktop only, P7.1): started without arguments or with --app, the
# program reads its options from the app's settings file and asks for the
# game folder (lgp/boot/app.hpp); without LGP_PRIVATE_DIR the pipeline cache
# goes to the app's cache folder.
# Frames in flight per device kind (P7.4; the window renderer, BROWSER the
# page's canvas): what LGP_WEBGPU_FRAMES_IN_FLIGHT defaults to there when the
# environment does not set it; 0 = no limit (the backend's own default).
# Apple TV: 3 (unthrottled command buffers piled up as driver memory, +477 MiB
# against +216 MiB with 3 in the same scene; docs/knowledge/webgpu-backend-desktop-und-browser.md).
# An existing build tree keeps its cached value.
set(_lgp_fif_default_APPLE_TV 3)
foreach(_kind IN ITEMS DESKTOP IOS APPLE_TV ANDROID ANDROID_TV BROWSER)
    if(NOT DEFINED _lgp_fif_default_${_kind})
        set(_lgp_fif_default_${_kind} 0)
    endif()
    set(LGP_FRAMES_IN_FLIGHT_${_kind} ${_lgp_fif_default_${_kind}} CACHE STRING
        "Default frames in flight (LGP_WEBGPU_FRAMES_IN_FLIGHT) of the window renderer on ${_kind}; 0 = no limit")
endforeach()

# Half-resolution effects (--effects, WebGpuBackend::set_halfres_effects):
# the programs whose additive draws may render at half resolution, hex hash
# prefixes. Default: LGP's full-screen light effect on the 1280x720 scene
# target, about 20 ms of fragment work per frame on an Adreno 640
# (docs/analysis/34-gpu-hebel-mobil-messstand.md). Per device kind
# (BROWSER: the page) what --effects defaults to: half on Android phones
# (measured +13.9 % fps in the slowest scene, analysis 34 section 13), full
# elsewhere.
set(LGP_HALFRES_PROGRAMS "263baa93" CACHE STRING
    "Programs (hex hash prefixes, comma-separated) whose additive draws --effects half renders at half resolution")
# "Play with Pikachu" draws this program and nothing else does (PC run
# 2026-10-22: X menu, team, bag, catch, Pokédex, wild battle, overworld):
# while it draws, the on-screen controls hide (touches only, no swipe).
# A prefix matches the program hash (from the translated WGSL: any
# translator change moves it; with 24a5b148 the scene no longer hid the controls on 2026-10-30)
# or the guest code hash, which only a game update changes; list the guest
# hash (LGP_WEBGPU_DEBUG=firstdraw logs both when a program first draws).
# d519effb: guest hash of the program that first draws on entering the scene
# (Android emulator 2026-10-30, two runs: began on entering, no false alarm
# in the field or the X menu).
# The cache holds "default" unless set by hand, so a new default reaches
# existing build trees; the old default 24a5b148 left in a cache counts as
# "default" too (PC2 2026-10-30: kept after build.py build).
# (CACHE INTERNAL: always rewritten, and seen where lgp_add_boot_executable is called.)
set(LGP_TOUCH_ONLY_PROGRAMS_DEFAULT "d519effb" CACHE INTERNAL "")
set(LGP_TOUCH_ONLY_PROGRAMS "default" CACHE STRING
    "Programs (hex prefixes of program or guest code hashes, comma-separated) of scenes that take touches only (the on-screen controls hide); default: ${LGP_TOUCH_ONLY_PROGRAMS_DEFAULT}, empty: off")
if(LGP_TOUCH_ONLY_PROGRAMS STREQUAL "24a5b148")
    set_property(CACHE LGP_TOUCH_ONLY_PROGRAMS PROPERTY VALUE "default")
endif()
foreach(_kind IN ITEMS DESKTOP IOS APPLE_TV ANDROID ANDROID_TV BROWSER)
    set(_default full)
    if(_kind STREQUAL "ANDROID")
        set(_default half)
    endif()
    set(LGP_EFFECTS_${_kind} ${_default} CACHE STRING "Default of --effects (full or half) on ${_kind}")
    set_property(CACHE LGP_EFFECTS_${_kind} PROPERTY STRINGS full half)
endforeach()

# Asynchronous backend execution (LGP_WEBGPU_ASYNC_EXECUTE) when the environment
# does not set it, with the WebGPU renderer: presents in flight, 0 = off.
# macOS 1: ABAB on an M1 with the host clock, frames 1021-3000: +0.9 % fps (the
# game's 30 fps cap), slowest window 24.5-25.0 -> 26.4-27.6 fps, gpu time of the
# game threads 16.3 -> 2.7 ms per frame; identical images with the virtual
# clock (docs/analysis/34-gpu-hebel-mobil-messstand.md).
# Elsewhere 0 until measured there. An existing build tree keeps its cached value.
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(_lgp_async_default 1)
else()
    set(_lgp_async_default 0)
endif()
set(LGP_ASYNC_EXECUTE_DEFAULT ${_lgp_async_default} CACHE STRING
    "Default of LGP_WEBGPU_ASYNC_EXECUTE with the WebGPU renderer (presents in flight; 0 = off)")
# Android phones and tablets 1 (Android TV is a run-time decision of the same
# APK): on a OnePlus 7 Pro with the host clock, A B B A A B over frames
# 1-4500, frames 1021-3000 21.63 -> 22.19 fps (+2.6 %), frames 3001-3300
# 10.81 -> 11.60 fps (+7.3 %), no B run below the worst A run, peak footprint
# the same (docs/analysis/34-gpu-hebel-mobil-messstand.md). Below the +3 %
# rule set before the run; switched on by the owner's decision (2026-10-21).
# Android TV (Shield) 0 until measured there.
set(LGP_ASYNC_EXECUTE_ANDROID 1 CACHE STRING
    "Default of LGP_WEBGPU_ASYNC_EXECUTE in the window on Android phones and tablets (0 = off)")
set(LGP_ASYNC_EXECUTE_ANDROID_TV 0 CACHE STRING
    "Default of LGP_WEBGPU_ASYNC_EXECUTE in the window on Android TV (0 = off)")

function(lgp_add_boot_executable target)
    if(LGP_TOUCH_ONLY_PROGRAMS STREQUAL "default")
        set(_lgp_touch_only_programs "${LGP_TOUCH_ONLY_PROGRAMS_DEFAULT}")
    else()
        set(_lgp_touch_only_programs "${LGP_TOUCH_ONLY_PROGRAMS}")
    endif()
    cmake_parse_arguments(PARSE_ARGV 1 arg "APP;NATIVE" "ENTRIES_NAME;WEB_ARGS" "")
    if(arg_NATIVE)
        list(APPEND arg_UNPARSED_ARGUMENTS "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/src/no_lifted_code.cpp")
    endif()
    if(NOT arg_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "lgp_add_boot_executable(${target}): no lifted sources")
    endif()
    # An executable; in an Android app build (P7.3) a shared library for the APK.
    lgp_add_sdl_program(${target} "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/src/main.cpp" ${arg_UNPARSED_ARGUMENTS})
    target_link_libraries(${target} PRIVATE lgp_boot lgp_lifted_headers lgp_platform_common lgp_runtime_native)
    # prepare_host() and host_devices() (lgp/boot/host.hpp): the platform's file
    # systems and its default controllers/sound.
    if(EMSCRIPTEN)
        target_sources(${target} PRIVATE "${PROJECT_SOURCE_DIR}/platform/web/src/boot_host.cpp")
        # Guest memory up to ADR-0001's web budget, grown on demand.
        target_link_options(${target} PRIVATE -sALLOW_MEMORY_GROWTH=1 -sMAXIMUM_MEMORY=4GB)
        set(_web_args "--exefs /opfs/lgp/data/exefs --game /opfs/lgp/data --save /opfs/lgp/save")
        if(LGP_WEBGPU_AVAILABLE)
            string(APPEND _web_args " --graphics hle --renderer webgpu --canvas #canvas"
                   " --pipeline-cache /opfs/lgp/cache/webgpu-pipelines.lgppipes")
        endif()
        if(arg_WEB_ARGS)
            string(APPEND _web_args " ${arg_WEB_ARGS}")
        endif()
        lgp_add_web_page(${target} TITLE "${target}" ARGS "${_web_args}")
    elseif(ANDROID)
        # MediaCodec for in-game videos (P7.3).
        target_sources(${target} PRIVATE "${PROJECT_SOURCE_DIR}/platform/android/src/boot_host.cpp")
        target_link_libraries(${target} PRIVATE lgp_platform_android)
        # The app's first start without game data: the system folder picker
        # and a copy into <app>/game (GameImport.java). SDL has no folder
        # picker on Android.
        if(arg_APP AND LGP_ANDROID_APP)
            target_sources(${target} PRIVATE "${PROJECT_SOURCE_DIR}/platform/android/src/game_import.cpp")
            target_compile_definitions(${target} PRIVATE LGP_BOOT_ANDROID_IMPORT=1 LGP_BOOT_NO_FOLDER_PICKER=1)
        endif()
    else()
        target_sources(${target} PRIVATE "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/src/host_default.cpp")
    endif()
    # Renderers for --graphics hle: soft (always), WebGPU headless and the
    # SDL3 window when they are available (P4.4).
    if(LGP_WEBGPU_AVAILABLE)
        target_link_libraries(${target} PRIVATE lgp_gpu_webgpu)
        target_compile_definitions(${target} PRIVATE LGP_BOOT_WEBGPU=1
            LGP_BOOT_ASYNC_DEFAULT=${LGP_ASYNC_EXECUTE_DEFAULT}
            "LGP_BOOT_HALFRES_PROGRAMS=\"${LGP_HALFRES_PROGRAMS}\""
            "LGP_BOOT_TOUCH_ONLY_PROGRAMS=\"${_lgp_touch_only_programs}\""
            "LGP_BOOT_EFFECTS_DESKTOP=\"${LGP_EFFECTS_DESKTOP}\"" "LGP_BOOT_EFFECTS_IOS=\"${LGP_EFFECTS_IOS}\""
            "LGP_BOOT_EFFECTS_APPLE_TV=\"${LGP_EFFECTS_APPLE_TV}\"" "LGP_BOOT_EFFECTS_ANDROID=\"${LGP_EFFECTS_ANDROID}\""
            "LGP_BOOT_EFFECTS_ANDROID_TV=\"${LGP_EFFECTS_ANDROID_TV}\"")
        if(EMSCRIPTEN)
            target_compile_definitions(${target} PRIVATE LGP_BOOT_FIF_BROWSER=${LGP_FRAMES_IN_FLIGHT_BROWSER}
                "LGP_BOOT_EFFECTS_BROWSER=\"${LGP_EFFECTS_BROWSER}\"")
        endif()
        lgp_webgpu_copy_runtime(${target})
    endif()
    if(TARGET lgp_platform_desktop)
        target_link_libraries(${target} PRIVATE lgp_platform_desktop)
        target_compile_definitions(${target} PRIVATE LGP_BOOT_SDL=1
            LGP_BOOT_FIF_DESKTOP=${LGP_FRAMES_IN_FLIGHT_DESKTOP} LGP_BOOT_FIF_IOS=${LGP_FRAMES_IN_FLIGHT_IOS}
            LGP_BOOT_FIF_APPLE_TV=${LGP_FRAMES_IN_FLIGHT_APPLE_TV} LGP_BOOT_FIF_ANDROID=${LGP_FRAMES_IN_FLIGHT_ANDROID}
            LGP_BOOT_FIF_ANDROID_TV=${LGP_FRAMES_IN_FLIGHT_ANDROID_TV}
            LGP_BOOT_ASYNC_ANDROID=${LGP_ASYNC_EXECUTE_ANDROID} LGP_BOOT_ASYNC_ANDROID_TV=${LGP_ASYNC_EXECUTE_ANDROID_TV})
    endif()
    # Sound output for audout (P5.2, --audio sdl).
    if(TARGET lgp_platform_sdl_audio)
        target_link_libraries(${target} PRIVATE lgp_platform_sdl_audio)
        target_compile_definitions(${target} PRIVATE LGP_BOOT_SDL_AUDIO=1)
    endif()
    # Video decoding for movie:: (P5, in-game videos; --video none|ffmpeg).
    if(TARGET lgp_platform_ffmpeg)
        target_link_libraries(${target} PRIVATE lgp_platform_ffmpeg)
        target_compile_definitions(${target} PRIVATE LGP_BOOT_FFMPEG=1)
        lgp_copy_ffmpeg_dlls(${target})
    endif()
    if(TARGET lgp_platform_vpx)  # --video vpx; the default without FFmpeg and a host decoder
        target_link_libraries(${target} PRIVATE lgp_platform_vpx)
        target_compile_definitions(${target} PRIVATE LGP_BOOT_VPX=1)
    endif()
    if(arg_ENTRIES_NAME)
        target_compile_definitions(${target} PRIVATE LGP_BOOT_ENTRIES=${arg_ENTRIES_NAME})
    endif()
    if(arg_NATIVE)
        target_compile_definitions(${target} PRIVATE LGP_BOOT_NATIVE_ONLY=1)
    endif()
    if(arg_APP AND NOT EMSCRIPTEN)
        target_compile_definitions(${target} PRIVATE LGP_BOOT_APP=1)
        # The app's menus (P7): start screen, save slots, settings, pause.
        if(TARGET lgp_platform_ui)
            target_link_libraries(${target} PRIVATE lgp_platform_ui lgp_platform_ui_assets SDL3::SDL3-static)
            target_compile_definitions(${target} PRIVATE LGP_BOOT_UI=1)
        endif()
        # App icon (tools/icon/gen_icons.py): Windows takes it from the exe's
        # first icon resource (Explorer, taskbar, SDL's window class); a macOS
        # app bundle (-DCMAKE_MACOSX_BUNDLE=ON) from its Resources.
        if(WIN32 AND CMAKE_RC_COMPILER)
            target_sources(${target} PRIVATE "${PROJECT_SOURCE_DIR}/platform/windows/lgp.rc")
        endif()
        get_target_property(_bundle ${target} MACOSX_BUNDLE)
        if(APPLE AND NOT LGP_APPLE_MOBILE AND _bundle)
            set(_icns "${PROJECT_SOURCE_DIR}/platform/macos/lgp.icns")
            target_sources(${target} PRIVATE "${_icns}")
            set_source_files_properties("${_icns}" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
            set_target_properties(${target} PROPERTIES MACOSX_BUNDLE_ICON_FILE lgp.icns)
            # A fixed bundle id (macOS keeps permissions such as removable-drive access and Game Mode per id);
            # without it the generated Info.plist has none. Bundle ids allow letters, digits, '-' and '.'.
            string(REGEX REPLACE "^lgp_" "" _mac_id "${target}")
            string(REPLACE "_" "-" _mac_id "${_mac_id}")
            set_target_properties(${target} PROPERTIES MACOSX_BUNDLE_GUI_IDENTIFIER "org.lgp.recomp.${_mac_id}"
                                                       MACOSX_BUNDLE_BUNDLE_NAME "LGP")
        endif()
    endif()
    # Android apps (P7.3): no console either; output in <app folder>/logs/lgp.log,
    # also when started with plain options (am start … -e args "…").
    if(LGP_ANDROID_APP AND arg_APP)
        target_compile_definitions(${target} PRIVATE LGP_BOOT_APP_LOG_ALWAYS=1)
    endif()
    # iOS/iPadOS (P7.2b): an app bundle whose main() belongs to SDL (UIKit run
    # loop, scene delegate); started without arguments or with --app (Mac:
    # xcrun devicectl device process launch … -- --app [options]), output always
    # in Documents/logs/lgp.log, the dump in Documents/game. No folder picker.
    # tvOS (P7.4) the same, in Library/Caches (no writable Documents there).
    if(LGP_APPLE_MOBILE AND arg_APP)
        if(NOT TARGET lgp_platform_desktop)
            message(FATAL_ERROR "lgp_add_boot_executable(${target}): an iOS/tvOS app needs the SDL3 window (WebGPU + SDL)")
        endif()
        target_sources(${target} PRIVATE "${PROJECT_SOURCE_DIR}/platform/desktop/src/sdl_main.cpp")
        target_link_libraries(${target} PRIVATE SDL3::SDL3-static)
        target_compile_definitions(${target} PRIVATE LGP_BOOT_SDL_MAIN=1 LGP_BOOT_NO_FOLDER_PICKER=1
                                                     LGP_BOOT_APP_LOG_ALWAYS=1)
        # Bundle ids allow letters, digits, '-' and '.': lgp_game -> <prefix>.game.
        string(REGEX REPLACE "^lgp_" "" _id "${target}")
        string(REPLACE "_" "-" _id "${_id}")
        # Only the game backs up its saves to iCloud (one container per app).
        if(target STREQUAL "lgp_game")
            lgp_ios_app(${target} ID "${_id}" NAME "LGP" CLOUD_SAVES)
        else()
            lgp_ios_app(${target} ID "${_id}" NAME "LGP")
        endif()
        lgp_ios_ui_tests(${target})  # only with -DLGP_IOS_UI_TESTS=ON
    endif()
endfunction()

set(LGP_GAME_LIFTED "" CACHE STRING
    "lgp_lift output of the game: a directory (all *.cpp in it) or a list of files; builds lgp_game")
set(LGP_APP_GAME_DIR "" CACHE PATH
    "Game folder (exefs/, romfs.bin or romfs/) to put into the lgp_game package; the app finds it without setup")
set(LGP_APP_GAME_STAMP "" CACHE STRING
    "Build stamp the app's game folder must carry in lgp-build-stamp.txt (letters and digits; the build tool sets it for a game outside the app)")
set(LGP_APP_PIPELINE_SEED "" CACHE FILEPATH
    "Pipeline cache (.lgppipes) to put into the lgp_game package as seed for the app's own cache")
# Without LGP_APP_PIPELINE_SEED a local game build packages
# $LGP_PRIVATE_DIR/pipeline-seed.lgppipes when that file exists (Apple TV,
# cold start, ABAB: 0 instead of 165 new draw pipelines, frames 701-800 23.5
# instead of 20.7 fps). The file is made locally from an own cache and stays
# out of the repository and CI; without it, or without LGP_PRIVATE_DIR (CI),
# the package has no seed as before.
option(LGP_APP_PIPELINE_SEED_AUTO
    "Without LGP_APP_PIPELINE_SEED: package $LGP_PRIVATE_DIR/pipeline-seed.lgppipes if it exists" ON)
# Profile-guided optimisation of the lifted code (docs/knowledge/lifted-code-performance.md):
# LGP_GAME_PGO=generate builds lgp_game with clang's instrumentation (the run
# writes LLVM_PROFILE_FILE, default default.profraw in the working directory),
# LGP_GAME_PGO=<file.profdata> (llvm-profdata merge) optimises with it. Only
# the lifted translation units: they hold the guest time. Never automatic: a
# profile from an older lift or another compiler only makes the build slower
# (functions without matching data), and measurement builds must not pick one
# up unnoticed. Profiles come from game code and stay in $LGP_PRIVATE_DIR.
set(LGP_GAME_PGO "" CACHE STRING
    "Lifted code of lgp_game: empty (off), generate (instrumented) or the path of a .profdata to optimise with")
option(LGP_APP_DRIVER_SEED
    "Also package <LGP_APP_PIPELINE_SEED>.driver (the driver's own pipeline cache; only of use on the same GPU and driver)"
    OFF)
# The game without lifted code (docs/analysis/56, stage D): the boot program
# rewrites the original modules in memory and runs them natively. Running
# needs an AArch64 host that may map code itself (not iOS/tvOS); on other
# desktop hosts it only writes the code image for such a device
# (--native-image-out, stage F). Holds no game code, so it builds without
# LGP_GAME_LIFTED.
# Android (stage F, test build): with LGP_ANDROID_NATIVE_APP an app of its own
# (org.lgp.recomp.native, own data folder: the game app and its saves stay
# untouched), whether an app may run the code it maps (the shell may).
option(LGP_ANDROID_NATIVE_APP "Android app build: also lgp_game_native.apk (native start, test build)" OFF)
if(NOT EMSCRIPTEN AND NOT ANDROID AND NOT LGP_APPLE_MOBILE)
    lgp_add_boot_executable(lgp_game_native NATIVE)
elseif(LGP_ANDROID_APP AND LGP_ANDROID_NATIVE_APP AND ANDROID_ABI STREQUAL "arm64-v8a")
    lgp_add_boot_executable(lgp_game_native NATIVE APP)
    lgp_add_android_apk(lgp_game_native PACKAGE org.lgp.recomp.native LABEL "LGP native")
endif()

if(LGP_GAME_LIFTED)
    set(_game_sources)
    foreach(p IN LISTS LGP_GAME_LIFTED)
        if(IS_DIRECTORY "${p}")
            file(GLOB _found CONFIGURE_DEPENDS "${p}/*.cpp")
            list(APPEND _game_sources ${_found})
        elseif(EXISTS "${p}")
            list(APPEND _game_sources "${p}")
        else()
            message(FATAL_ERROR "LGP_GAME_LIFTED: ${p} does not exist")
        endif()
    endforeach()
    if(NOT _game_sources)
        message(FATAL_ERROR "LGP_GAME_LIFTED: no .cpp files in ${LGP_GAME_LIFTED}")
    endif()
    list(LENGTH _game_sources _n)
    message(STATUS "lgp_game: ${_n} lifted translation units")
    # LGP aborts during startup (svcBreak) when svcGetInfo(TotalMemorySize)
    # leaves it less than about 3040 MiB (measured: 3025 MiB aborts, 3050 runs);
    # the web default of ADR-0001 (3072 MiB) covers that, so the page needs no
    # --memory (docs/analysis/13-browser-abnahme.md).
    lgp_add_boot_executable(lgp_game ${_game_sources} APP)
    if(LGP_GAME_PGO)
        if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            message(FATAL_ERROR "LGP_GAME_PGO needs clang (found ${CMAKE_CXX_COMPILER_ID})")
        endif()
        if(LGP_GAME_PGO STREQUAL "generate")
            set_source_files_properties(${_game_sources} TARGET_DIRECTORY lgp_game
                                        PROPERTIES COMPILE_OPTIONS "-fprofile-instr-generate")
            target_link_options(lgp_game PRIVATE -fprofile-instr-generate)
            # main.cpp names the file and writes the profile itself: apps
            # never run the runtime's exit handler (write_pgo_profile).
            target_compile_definitions(lgp_game PRIVATE LGP_GAME_PGO_GENERATE=1)
            message(STATUS "lgp_game: PGO instrumented (writes LLVM_PROFILE_FILE)")
        else()
            file(TO_CMAKE_PATH "${LGP_GAME_PGO}" _pgo)
            if(NOT EXISTS "${_pgo}" OR IS_DIRECTORY "${_pgo}")
                message(FATAL_ERROR "LGP_GAME_PGO: ${_pgo} is not a file (generate or a .profdata)")
            endif()
            # Out-of-date and missing data are counted by the warnings; they
            # stay on so a stale profile shows in the build log.
            set_source_files_properties(${_game_sources} TARGET_DIRECTORY lgp_game
                                        PROPERTIES COMPILE_OPTIONS "-fprofile-instr-use=${_pgo}"
                                                   OBJECT_DEPENDS "${_pgo}")
            message(STATUS "lgp_game: PGO with ${_pgo}")
        endif()
    endif()
    # A game outside the app (build tool --game-separate: slim iOS package,
    # game copied into Documents/game once): the app refuses a game folder
    # whose lgp-build-stamp.txt is not this build's (lgp::boot::game_fits_build).
    if(LGP_APP_GAME_STAMP)
        if(NOT LGP_APP_GAME_STAMP MATCHES "^[A-Za-z0-9._-]+$")
            message(FATAL_ERROR "LGP_APP_GAME_STAMP: only letters, digits, '.', '_' and '-'")
        endif()
        target_compile_definitions(lgp_game PRIVATE "LGP_BOOT_GAME_STAMP=\"${LGP_APP_GAME_STAMP}\"")
    endif()
    # The dump inside the app package (Paul, 2026-10-14): LGP_APP_GAME_DIR is
    # copied to "game" in the bundle (iOS/tvOS .app, macOS .app resources) or
    # next to the program, where the app looks first
    # (platform::packaged_game_dir). LGP_APP_PIPELINE_SEED (or the automatic
    # seed above) goes beside it as pipeline-seed.lgppipes: the app seeds its
    # pipeline cache from it, so a
    # fresh install or an emptied cache (tvOS may empty Library/Caches) does
    # not compile every pipeline in the game again. The package then holds
    # game data: local builds only, like the lifted code.
    # Each item is "<source>|<name>"; an empty source means the option was
    # cleared, and the next build removes what an earlier build copied there
    # (the stamp in the build tree says where), never anything else.
    set(_package_items "${LGP_APP_GAME_DIR}|game")
    set(_seed "${LGP_APP_PIPELINE_SEED}")
    # Backslashes (D:\... on Windows) would be escapes in -DSRC= and DEPENDS.
    file(TO_CMAKE_PATH "$ENV{LGP_PRIVATE_DIR}" _private_dir)
    if(NOT _seed AND LGP_APP_PIPELINE_SEED_AUTO AND NOT _private_dir STREQUAL ""
       AND EXISTS "${_private_dir}/pipeline-seed.lgppipes"
       AND NOT IS_DIRECTORY "${_private_dir}/pipeline-seed.lgppipes")
        set(_seed "${_private_dir}/pipeline-seed.lgppipes")
    endif()
    file(TO_CMAKE_PATH "${_seed}" _seed)
    if(_seed)
        message(STATUS "lgp_game: pipeline seed ${_seed}")
        if(NOT EXISTS "${_seed}" OR IS_DIRECTORY "${_seed}")
            message(FATAL_ERROR "LGP_APP_PIPELINE_SEED: ${_seed} is not a file")
        endif()
        if(LGP_APP_DRIVER_SEED AND (NOT EXISTS "${_seed}.driver"
                                    OR IS_DIRECTORY "${_seed}.driver"))
            message(FATAL_ERROR "LGP_APP_DRIVER_SEED: ${_seed}.driver is not a file")
        endif()
    endif()
    # Opt-in: a driver cache made on another GPU or driver is only dropped on
    # the device, and its gain is not measured yet.
    set(_driver_seed "")
    if(_seed AND LGP_APP_DRIVER_SEED)
        set(_driver_seed "${_seed}.driver")
    endif()
    # Android: the seed goes into the APK's assets instead (lgp_add_android_apk
    # below).
    if(NOT ANDROID)
        list(APPEND _package_items "${_seed}|pipeline-seed.lgppipes")
        list(APPEND _package_items "${_driver_seed}|pipeline-seed.lgppipes.driver")
    endif()
    set(_package_any OFF)
    set(_package_stale OFF)
    foreach(item IN LISTS _package_items)
        string(REPLACE "|" ";" item "${item}")
        list(GET item 0 _src)
        list(GET item 1 _name)
        if(_src)
            set(_package_any ON)
        elseif(EXISTS "${CMAKE_CURRENT_BINARY_DIR}/lgp_game_package_${_name}.stamp")
            set(_package_stale ON)
        endif()
    endforeach()
    if(_package_any OR _package_stale)
        if(ANDROID OR EMSCRIPTEN)
            if(_package_any)
                message(WARNING "LGP_APP_GAME_DIR/LGP_APP_PIPELINE_SEED: not supported for ${CMAKE_SYSTEM_NAME} (the package is not a folder); ignored")
            endif()
        else()
            get_target_property(_bundle lgp_game MACOSX_BUNDLE)
            set(_xcode OFF)
            if(CMAKE_GENERATOR STREQUAL "Xcode" AND (_bundle OR IOS OR CMAKE_SYSTEM_NAME MATCHES "^(tvOS|visionOS)$"))
                # $<TARGET_BUNDLE_DIR> holds an escaped ${EFFECTIVE_PLATFORM_NAME}
                # under Xcode and the copy lands beside the bundle; the script
                # takes the destination from Xcode's environment instead.
                set(_xcode ON)
            elseif(IOS OR CMAKE_SYSTEM_NAME MATCHES "^(tvOS|visionOS)$")
                set(_base "$<TARGET_BUNDLE_DIR:lgp_game>")
            elseif(APPLE AND _bundle)
                set(_base "$<TARGET_BUNDLE_CONTENT_DIR:lgp_game>/Resources")
            else()
                set(_base "$<TARGET_FILE_DIR:lgp_game>")
            endif()
            set(_commands)
            foreach(item IN LISTS _package_items)
                string(REPLACE "|" ";" item "${item}")
                list(GET item 0 _src)
                list(GET item 1 _name)
                set(_stamp "${CMAKE_CURRENT_BINARY_DIR}/lgp_game_package_${_name}.stamp")
                if(NOT _src)
                    if(EXISTS "${_stamp}")
                        list(APPEND _commands COMMAND "${CMAKE_COMMAND}" "-DREMOVE_STAMP=${_stamp}"
                                                      -P "${PROJECT_SOURCE_DIR}/cmake/LgpCopyGame.cmake")
                    endif()
                    continue()
                endif()
                if(_xcode)
                    set(_where "-DDST=" "-DDST_XCODE_RESOURCES=${_name}")
                else()
                    set(_where "-DDST=${_base}/${_name}")
                endif()
                list(APPEND _commands COMMAND "${CMAKE_COMMAND}" "-DSRC=${_src}" ${_where} "-DSTAMP=${_stamp}"
                                              -P "${PROJECT_SOURCE_DIR}/cmake/LgpCopyGame.cmake")
            endforeach()
            if(_xcode)
                # A build phase of lgp_game, so it runs before Xcode signs.
                add_custom_command(TARGET lgp_game POST_BUILD
                    ${_commands}
                    COMMENT "Copying the game into the lgp_game package"
                    VERBATIM)
            else()
                # A target of its own runs on every build, not only when
                # lgp_game is relinked, so a changed dump is copied too.
                add_custom_target(lgp_game_package_game ALL
                    ${_commands}
                    COMMENT "Copying the game into the lgp_game package"
                    VERBATIM)
                add_dependencies(lgp_game_package_game lgp_game)
            endif()
        endif()
    endif()
    # Android (preset android-app): lgp_game.apk; the dump goes to the app's
    # folder (docs/knowledge/android-ndk-build.md).
    # The pipeline seed goes into its assets; LgpActivity copies it to
    # <app folder>/package/pipeline-seed.lgppipes.
    set(_assets)
    if(_seed)
        set(_assets ASSETS "pipeline-seed.lgppipes=${_seed}")
        if(_driver_seed)
            list(APPEND _assets "pipeline-seed.lgppipes.driver=${_driver_seed}")
        endif()
    endif()
    lgp_add_android_apk(lgp_game PACKAGE org.lgp.recomp.game LABEL "LGP" ${_assets})
    # Host NaN results in the FP fast paths (LGP_FP_HOST_NAN, default on since
    # the PC check of docs/analysis/18-fp-host-nan-pruefung.md); the test boot
    # programs keep exact Arm NaNs.
    # Not on 64-bit Arm hosts (Mac, iPhone, iPad, Apple TV, Android): there
    # host NaNs hid the wallet text in the bag once the widescreen mod moved
    # its layout, while exact Arm NaNs and x86 (also under Rosetta) showed it
    # (Analysis 18, 2026-10-28; cause in the lifted code not found yet).
    # LGP_FP_HOST_NAN_ARM64=ON turns them on there again (measurements).
    set(_arm64 OFF)
    if(CMAKE_OSX_ARCHITECTURES)
        if(CMAKE_OSX_ARCHITECTURES MATCHES "arm64")
            set(_arm64 ON)
        endif()
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64|ARM64)$" OR CMAKE_ANDROID_ARCH_ABI STREQUAL "arm64-v8a"
           OR CMAKE_SYSTEM_NAME MATCHES "^(iOS|tvOS)$")
        set(_arm64 ON)
    endif()
    if(LGP_FP_HOST_NAN AND _arm64 AND NOT LGP_FP_HOST_NAN_ARM64 AND NOT EMSCRIPTEN)
        message(STATUS "lgp_game: exact Arm NaNs on this 64-bit Arm target (LGP_FP_HOST_NAN_ARM64=ON for host NaNs)")
    elseif(LGP_FP_HOST_NAN)
        target_compile_definitions(lgp_game PRIVATE LGP_FP_HOST_NAN=1)
        if(NOT LGP_FP_HOST_NAN_CLASSES STREQUAL "")
            target_compile_definitions(lgp_game PRIVATE LGP_FP_HOST_NAN_CLASSES=${LGP_FP_HOST_NAN_CLASSES})
        endif()
    else()
        # The option was off by default before; a build tree configured then
        # keeps OFF in its cache. Measurements of such a build are slower in
        # Wasm by about half the guest time (Analyse 20).
        message(WARNING "lgp_game: LGP_FP_HOST_NAN is OFF (exact Arm NaNs, much slower FP in Wasm). "
                        "If this comes from an old build cache, reconfigure with -DLGP_FP_HOST_NAN=ON.")
    endif()
endif()
if((LGP_APP_GAME_DIR OR LGP_APP_PIPELINE_SEED) AND NOT LGP_GAME_LIFTED)
    message(WARNING "LGP_APP_GAME_DIR/LGP_APP_PIPELINE_SEED is set but LGP_GAME_LIFTED is not: there is no lgp_game package; ignored")
endif()
