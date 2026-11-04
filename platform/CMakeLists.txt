# Platform layer (P3.6): everything the runtime needs from the host.
#   lgp_platform         interfaces (include/lgp/platform) + OS code (os/):
#                        guest memory reservation, host threads, fsync,
#                        the user folders of a desktop app (P7.1), UDP
#                        sockets for ldn over LAN (lan.hpp)
#   lgp_platform_common  backends without devices: null renderer/audio,
#                        manual input, in-memory and host-directory storage;
#                        host pad -> Switch controller mapping (P5.1); NV12
#                        conversion helpers for video backends; fixed and
#                        console text input (software keyboard)
#   lgp_platform_webgpu  IRenderer on WebGPU (P4.4): device, surface, GPU
#                        thread; only with LGP_WEBGPU_AVAILABLE
#   lgp_platform_desktop SDL3 window as WebGPU surface (P4.4), gamepads and
#                        keyboard (P5.1), the app's folder picker and message
#                        boxes (P7.1); only with LGP_SDL_AVAILABLE
#   lgp_ui_layout        the game's layout files (SARC/BFLYT/BFLAN), read for
#                        the menus and shifted for the widescreen mod
#   lgp_platform_ui      the app's menus (P7): start screen, save slots,
#                        settings, pause; Dear ImGui on SDL3, drawn by
#                        lgp_platform_webgpu
#   lgp_platform_sdl_audio  IAudio on SDL3 audio streams (P5.2); only with
#                        LGP_SDL_AVAILABLE (no WebGPU needed)
#   lgp_platform_ffmpeg  IVideo on libavcodec (P5, in-game videos); only with
#                        LGP_FFMPEG_AVAILABLE
#   lgp_platform_vpx     IVideo on libvpx (VP9; default on iOS, opt-in elsewhere
#                        via LGP_ENABLE_VPX); only with LGP_VPX_AVAILABLE
#   lgp_platform_android Android (P7.3): IVideo on MediaCodec
#   lgp_platform_web     browser backends, only on Emscripten: storage (P6):
#                        OPFS through WasmFS, plus lgp_add_web_page() for the
#                        page with the local file picker; Gamepad API and
#                        keyboard (P5.1); WebAudio sink on an AudioWorklet (P5.2);
#                        IVideo on WebCodecs (P6, in-game videos); software
#                        keyboard as a page dialog

find_package(Threads REQUIRED)

add_library(lgp_platform STATIC
    os/src/app_dirs.cpp
    os/src/cloud_save.cpp
    os/src/code_signature.cpp
    os/src/system_settings.cpp
    os/src/cpu_affinity.cpp
    os/src/host_block.cpp
    os/src/host_env.cpp
    os/src/host_sync.cpp
    os/src/host_thread.cpp
    os/src/native_signals.cpp
    os/src/lan.cpp
    os/src/lan_tls.cpp
    src/overlay.cpp
    src/screen_fit.cpp
    src/storage.cpp
    src/ui_text.cpp)
target_include_directories(lgp_platform PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(lgp_platform PUBLIC cxx_std_20)
target_link_libraries(lgp_platform PUBLIC Threads::Threads)
# TLS for wss:// online play (lan_tls.cpp, cmake/LgpMbedTls.cmake).
if(LGP_MBEDTLS_AVAILABLE)
    target_link_libraries(lgp_platform PRIVATE MbedTLS::mbedtls)
    target_compile_definitions(lgp_platform PRIVATE LGP_HAVE_MBEDTLS=1)
    if(WIN32)
        target_link_libraries(lgp_platform PUBLIC crypt32)  # the ROOT certificate store
    endif()
    if(APPLE)
        # The server certificate goes to the system's trust evaluation.
        target_sources(lgp_platform PRIVATE apple/src/tls_trust_apple.cpp)
        target_link_libraries(lgp_platform PUBLIC "-framework Security")
    endif()
endif()
# Guest memory on an unlinked file by default (host_block.hpp). tvOS has it on
# regardless; this turns it on for other POSIX builds (iOS candidate: keeps
# the guest pages out of phys_footprint, the jetsam limit's measure). The
# environment still wins: LGP_HOST_BLOCK_FILE=0 turns it off for one run.
option(LGP_HOST_BLOCK_FILE_DEFAULT "Back guest memory with a file unless LGP_HOST_BLOCK_FILE=0 (POSIX)" OFF)
if(LGP_HOST_BLOCK_FILE_DEFAULT)
    set_property(SOURCE os/src/host_block.cpp APPEND PROPERTY COMPILE_DEFINITIONS LGP_HOST_BLOCK_FILE_DEFAULT=1)
endif()
# Only this window of the block (MiB offsets A:B, as LGP_HOST_BLOCK_FILE_RANGE)
# goes in the file when the environment does not say; the rest stays
# anonymous. Empty: the platform's default, which is "768:" on tvOS (set in
# host_block.cpp, so tools/memprobe gets it too) and the whole block
# elsewhere; "0:" puts the whole block in the file on tvOS as well. With the
# whole block in the file the guest regions at offsets 512-1024 MiB stood
# 13-27 s on the Apple TV while the system faulted them in and wrote them
# back. With the first GiB anonymous (1024:) runs had no stall but went down to
# 90 MiB available; 768: keeps >= 438 MiB (one 13 s stall while loading in one
# of two runs, docs/analysis/42).
set(LGP_HOST_BLOCK_FILE_RANGE_DEFAULT "" CACHE STRING
    "Default file window of the guest block, MiB A:B (empty: the platform's default)")
if(NOT LGP_HOST_BLOCK_FILE_RANGE_DEFAULT STREQUAL "")
    set_property(SOURCE os/src/host_block.cpp APPEND PROPERTY COMPILE_DEFINITIONS
                 "LGP_HOST_BLOCK_FILE_RANGE_DEFAULT=\"${LGP_HOST_BLOCK_FILE_RANGE_DEFAULT}\"")
endif()
if(APPLE)
    # exclude_from_backup (CFURLSetResourcePropertyForKey); gpu_allocated
    # (host_env.hpp) reads the Metal device. OBJCXX is enabled in the root.
    target_sources(lgp_platform PRIVATE apple/src/metal_memory.mm)
    # Bonjour discovery for ldn over LAN (DNS-SD in libSystem, no extra library).
    target_sources(lgp_platform PRIVATE apple/src/lan_bonjour.cpp)
    # ARC per file: a $<COMPILE_LANGUAGE:OBJCXX> option on the target does not
    # reliably reach .mm files under the Xcode generator (Verdacht from the
    # tvOS CloudKit crash, 2026-10-20); cloud_save_cloudkit.mm refuses to build without it.
    set_source_files_properties(apple/src/metal_memory.mm PROPERTIES COMPILE_OPTIONS -fobjc-arc)
    # CoreGraphics: without it MTLCreateSystemDefaultDevice returns nil in
    # macOS command-line programs.
    target_link_libraries(lgp_platform PUBLIC "-framework CoreFoundation" "-framework Metal" "-framework Foundation"
                                              "-framework CoreGraphics")
endif()
if(WIN32)
    # lan.cpp: Winsock and GetAdaptersAddresses (ldn over LAN).
    target_link_libraries(lgp_platform PUBLIC ws2_32 iphlpapi)
endif()
if(LGP_APPLE_MOBILE)
    # Save backup in iCloud (cloud_save.hpp), iOS and tvOS; the app needs the CloudKit
    # entitlement (cmake/LgpIos.cmake CLOUD_SAVES), without it
    # make_cloud_save() returns nullptr.
    target_sources(lgp_platform PRIVATE apple/src/cloud_save_cloudkit.mm)
    target_compile_definitions(lgp_platform PRIVATE LGP_PLATFORM_CLOUDKIT=1)
    set_source_files_properties(apple/src/cloud_save_cloudkit.mm PROPERTIES COMPILE_OPTIONS -fobjc-arc)
    target_link_libraries(lgp_platform PUBLIC "-framework CloudKit" "-framework Foundation")
endif()
if(IOS AND NOT LGP_TVOS)
    # Settings in the Settings app (system_settings.hpp, platform/ios/Settings.bundle);
    # tvOS has no app settings there.
    target_sources(lgp_platform PRIVATE apple/src/system_settings.mm)
    target_compile_definitions(lgp_platform PRIVATE LGP_PLATFORM_SYSTEM_SETTINGS=1)
    set_source_files_properties(apple/src/system_settings.mm PROPERTIES COMPILE_OPTIONS -fobjc-arc)
endif()

add_library(lgp_platform_common STATIC
    common/src/fallback_video.cpp
    common/src/host_directory_storage.cpp
    common/src/memory_storage.cpp
    common/src/null_backends.cpp
    common/src/pad_input.cpp
    common/src/segment_storage.cpp
    common/src/touch_pad.cpp
    common/src/text_input.cpp
    common/src/video_convert.cpp)
target_include_directories(lgp_platform_common PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/common/include")
target_link_libraries(lgp_platform_common PUBLIC lgp_platform)

set(_lgp_platform_targets lgp_platform lgp_platform_common)
if(LGP_WEBGPU_AVAILABLE)
    add_library(lgp_platform_webgpu STATIC webgpu/src/webgpu_renderer.cpp)
    target_include_directories(lgp_platform_webgpu PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/webgpu/include")
    target_link_libraries(lgp_platform_webgpu PUBLIC lgp_platform lgp_webgpu)
    if(EMSCRIPTEN)
        # WebGpuRenderer moves the canvas to a GPU pthread of its own
        # (RendererOptions::gpu_thread). It takes the canvas from the
        # browser's main thread, so PROXY_TO_PTHREAD must not hand it to
        # main()'s thread first (the default "#canvas").
        target_link_options(lgp_platform_webgpu PUBLIC -sOFFSCREENCANVAS_SUPPORT=1 [[-sOFFSCREENCANVASES_TO_PTHREAD=""]])
    endif()
    list(APPEND _lgp_platform_targets lgp_platform_webgpu)
endif()
if(LGP_SDL_AVAILABLE AND TARGET lgp_platform_webgpu)
    add_library(lgp_platform_desktop STATIC desktop/src/sdl_dialogs.cpp desktop/src/sdl_input.cpp
        desktop/src/sdl_text_input.cpp desktop/src/sdl_window.cpp)
    target_include_directories(lgp_platform_desktop PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/desktop/include")
    target_link_libraries(lgp_platform_desktop PUBLIC lgp_platform_webgpu lgp_platform_common PRIVATE SDL3::SDL3-static)
    # Window icon (title bar, Linux window lists, macOS Dock without an app
    # bundle): icon/lgp-icon-128.png from tools/icon/gen_icons.py, embedded.
    # Not in a macOS bundle: SDL's Cocoa backend would replace the bundle's
    # multi-resolution lgp.icns in the Dock with this 128 px image.
    if(NOT LGP_APPLE_MOBILE AND NOT ANDROID AND NOT (APPLE AND CMAKE_MACOSX_BUNDLE))
        set(_icon "${CMAKE_CURRENT_SOURCE_DIR}/icon/lgp-icon-128.png")
        file(READ "${_icon}" _icon_hex HEX)
        string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," LGP_ICON_BYTES "${_icon_hex}")
        file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/app_icon_png.cpp" @ONLY CONTENT
             "// Generated from platform/icon/lgp-icon-128.png (platform/CMakeLists.txt).\n#include <cstddef>\nnamespace lgp::platform::desktop {\nextern const unsigned char kAppIconPng[] = {@LGP_ICON_BYTES@};\nextern const std::size_t kAppIconPngSize = sizeof(kAppIconPng);\n}  // namespace lgp::platform::desktop\n")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_icon}")
        target_sources(lgp_platform_desktop PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/app_icon_png.cpp")
        target_compile_definitions(lgp_platform_desktop PRIVATE LGP_DESKTOP_APP_ICON=1)
    endif()
    if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
        # iPadOS 26 windows: ask the scene for the whole screen (SdlWindow::create).
        target_sources(lgp_platform_desktop PRIVATE desktop/src/ios_scene.mm)
        # ARC per file, as for the .mm files of lgp_platform (a target-wide
        # $<COMPILE_LANGUAGE:OBJCXX> option may not reach them under Xcode).
        set_source_files_properties(desktop/src/ios_scene.mm PROPERTIES COMPILE_OPTIONS -fobjc-arc)
        target_link_libraries(lgp_platform_desktop PUBLIC "-framework UIKit")
    endif()
    if(CMAKE_SYSTEM_NAME STREQUAL "iOS" OR CMAKE_SYSTEM_NAME STREQUAL "tvOS")
        # The app's lifecycle state (SdlWindow::pump_events reattaches the surface by it).
        target_sources(lgp_platform_desktop PRIVATE desktop/src/uikit_state.mm)
        target_link_libraries(lgp_platform_desktop PUBLIC "-framework UIKit")
    endif()
    list(APPEND _lgp_platform_targets lgp_platform_desktop)
endif()

# The game's layout files (SARC, BFLYT, BFLAN; no OS code, no dependencies):
# read for the app's menus and changed by the widescreen mod as the game
# loads them (layout_shift.hpp).
add_library(lgp_ui_layout STATIC ui/src/layout.cpp ui/src/layout_shift.cpp ui/src/sarc.cpp)
target_include_directories(lgp_ui_layout PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/ui/include")
target_compile_features(lgp_ui_layout PUBLIC cxx_std_20)
list(APPEND _lgp_platform_targets lgp_ui_layout)

# The app's interface (P7, docs/analysis/27-p7-app-oberflaeche.md): Dear ImGui
# 1.92.9 (MIT, vendored in ui/third_party/imgui, THIRD_PARTY.md) on SDL3
# events; the WebGPU renderer draws its triangle lists (ui_draw.hpp).
if(LGP_SDL_AVAILABLE AND TARGET lgp_platform_webgpu)
    set(_imgui "${CMAKE_CURRENT_SOURCE_DIR}/ui/third_party/imgui")
    add_library(lgp_imgui STATIC "${_imgui}/imgui.cpp" "${_imgui}/imgui_draw.cpp" "${_imgui}/imgui_tables.cpp"
                "${_imgui}/imgui_widgets.cpp" "${_imgui}/backends/imgui_impl_sdl3.cpp")
    target_include_directories(lgp_imgui PUBLIC "${_imgui}")
    target_compile_definitions(lgp_imgui PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS IMGUI_DISABLE_DEMO_WINDOWS
                               IMGUI_DISABLE_DEBUG_TOOLS)
    target_compile_features(lgp_imgui PUBLIC cxx_std_20)
    target_link_libraries(lgp_imgui PRIVATE SDL3::SDL3-static)
    add_library(lgp_platform_ui STATIC ui/src/app_menu.cpp ui/src/game_font.cpp ui/src/layout_draw.cpp
        ui/src/layout_skin.cpp)
    target_include_directories(lgp_platform_ui PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/ui/include")
    target_link_libraries(lgp_platform_ui PUBLIC lgp_platform lgp_ui_layout PRIVATE lgp_imgui SDL3::SDL3-static)
    list(APPEND _lgp_platform_targets lgp_platform_ui)
    # The game's interface files (textures from its layout archives) for the
    # menus, read from the user's RomFS at run time.
    add_library(lgp_platform_ui_assets STATIC ui/src/bntx.cpp ui/src/layout_skin_load.cpp)
    target_include_directories(lgp_platform_ui_assets PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/ui/include")
    target_link_libraries(lgp_platform_ui_assets PUBLIC lgp_platform lgp_platform_ui PRIVATE lgp_gpu_nvn lgp_gpu_texture)
    list(APPEND _lgp_platform_targets lgp_platform_ui_assets)
endif()

if(LGP_SDL_AVAILABLE)
    add_library(lgp_platform_sdl_audio STATIC desktop/src/sdl_audio.cpp)
    target_include_directories(lgp_platform_sdl_audio PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/desktop/include")
    target_link_libraries(lgp_platform_sdl_audio PUBLIC lgp_platform PRIVATE SDL3::SDL3-static)
    list(APPEND _lgp_platform_targets lgp_platform_sdl_audio)
endif()
if(LGP_VPX_AVAILABLE)
    add_library(lgp_platform_vpx STATIC vpx/src/vpx_video.cpp)
    target_include_directories(lgp_platform_vpx PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/vpx/include")
    target_link_libraries(lgp_platform_vpx PUBLIC lgp_platform PRIVATE Vpx::vpx)
    list(APPEND _lgp_platform_targets lgp_platform_vpx)
endif()
if(LGP_FFMPEG_AVAILABLE)
    add_library(lgp_platform_ffmpeg STATIC ffmpeg/src/ffmpeg_video.cpp)
    target_include_directories(lgp_platform_ffmpeg PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/ffmpeg/include")
    target_link_libraries(lgp_platform_ffmpeg PUBLIC lgp_platform PRIVATE Ffmpeg::avcodec)
    list(APPEND _lgp_platform_targets lgp_platform_ffmpeg)
endif()
if(ANDROID)
    add_library(lgp_platform_android STATIC android/src/mediacodec_video.cpp)
    target_include_directories(lgp_platform_android PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/android/include")
    target_link_libraries(lgp_platform_android PUBLIC lgp_platform PRIVATE lgp_platform_common mediandk log)
    list(APPEND _lgp_platform_targets lgp_platform_android)
endif()
if(EMSCRIPTEN)
    # The AudioWorklet's source is embedded as a string (loaded from a Blob URL).
    set(_worklet_js "${CMAKE_CURRENT_SOURCE_DIR}/web/src/audio_worklet.js")
    set(_worklet_hpp "${CMAKE_CURRENT_BINARY_DIR}/generated/web_audio_worklet_js.hpp")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_worklet_js}")
    file(READ "${_worklet_js}" LGP_AUDIO_WORKLET_JS)
    file(CONFIGURE OUTPUT "${_worklet_hpp}" CONTENT
         "// Generated from platform/web/src/audio_worklet.js.\n#pragma once\nstatic const char kAudioWorkletSource[] = R\"lgpjs(@LGP_AUDIO_WORKLET_JS@)lgpjs\";\n"
         @ONLY)
    add_library(lgp_platform_web STATIC web/src/opfs.cpp web/src/web_audio.cpp web/src/web_input.cpp
                web/src/web_text_input.cpp web/src/web_video.cpp web/src/output_drain.cpp)
    target_include_directories(lgp_platform_web PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/web/include"
                               PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
    target_link_libraries(lgp_platform_web PUBLIC lgp_platform_common)
    # WasmFS replaces the JS file system of every program linking this; its
    # OPFS backend needs pthreads (proxying to a dedicated worker).
    target_link_options(lgp_platform_web PUBLIC -sWASMFS=1)
    list(APPEND _lgp_platform_targets lgp_platform_web)
    include(web/LgpWebPage.cmake)
endif()

foreach(target ${_lgp_platform_targets})
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra)
    endif()
endforeach()
