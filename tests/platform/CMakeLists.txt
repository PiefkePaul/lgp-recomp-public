# Platform layer (P3.6). Label "platform".

add_executable(lgp_test_platform test_platform.cpp)
target_link_libraries(lgp_test_platform PRIVATE lgp_platform_common)
if(EMSCRIPTEN)
    target_link_options(lgp_test_platform PRIVATE -sALLOW_MEMORY_GROWTH=1)
endif()
add_test(NAME platform.backends COMMAND lgp_test_platform)
set_tests_properties(platform.backends PROPERTIES LABELS "platform")

# P5.1: host pads -> Switch controllers.
add_executable(lgp_test_pad_input test_pad_input.cpp)
target_link_libraries(lgp_test_pad_input PRIVATE lgp_platform_common)
add_test(NAME platform.pad_input COMMAND lgp_test_pad_input)
set_tests_properties(platform.pad_input PROPERTIES LABELS "platform")

# P7.3: game files from byte ranges of host files (romfs.bin from split APKs).
add_executable(lgp_test_segment_storage test_segment_storage.cpp)
target_link_libraries(lgp_test_segment_storage PRIVATE lgp_platform_common)
add_test(NAME platform.segment_storage COMMAND lgp_test_segment_storage)
set_tests_properties(platform.segment_storage PROPERTIES LABELS "platform")

# P7.3: on-screen controller for touch screens.
add_executable(lgp_test_touch_pad test_touch_pad.cpp)
target_link_libraries(lgp_test_touch_pad PRIVATE lgp_platform_common)
add_test(NAME platform.touch_pad COMMAND lgp_test_touch_pad)
set_tests_properties(platform.touch_pad PROPERTIES LABELS "platform")
# Pictures of the on-screen controller (touch_preview.cpp --out FILE.ppm).
add_executable(lgp_touch_preview touch_preview.cpp)
target_link_libraries(lgp_touch_preview PRIVATE lgp_platform_common)
add_test(NAME platform.touch_preview COMMAND lgp_touch_preview)
set_tests_properties(platform.touch_preview PROPERTIES LABELS "platform")

# P7: picture format (fit, fill, stretch) and window -> game image mapping.
add_executable(lgp_test_screen_fit test_screen_fit.cpp)
target_link_libraries(lgp_test_screen_fit PRIVATE lgp_platform)
add_test(NAME platform.screen_fit COMMAND lgp_test_screen_fit)
set_tests_properties(platform.screen_fit PROPERTIES LABELS "platform")

# iOS sideloading: entitlements read back from a code signature (lgp/platform/code_signature.hpp).
add_executable(lgp_test_code_signature test_code_signature.cpp)
target_link_libraries(lgp_test_code_signature PRIVATE lgp_platform)
add_test(NAME platform.code_signature COMMAND lgp_test_code_signature)
set_tests_properties(platform.code_signature PROPERTIES LABELS "platform")

# Settings in the system's Settings app (iPhone/iPad): the sync rule.
add_executable(lgp_test_system_settings test_system_settings.cpp)
target_link_libraries(lgp_test_system_settings PRIVATE lgp_platform)
add_test(NAME platform.system_settings COMMAND lgp_test_system_settings)
set_tests_properties(platform.system_settings PROPERTIES LABELS "platform")

# The app's interface text in two languages (English code, German table).
add_executable(lgp_test_ui_text test_ui_text.cpp)
target_link_libraries(lgp_test_ui_text PRIVATE lgp_platform)
add_test(NAME platform.ui_text COMMAND lgp_test_ui_text)
set_tests_properties(platform.ui_text PROPERTIES LABELS "platform")

# Software keyboard on the host: fixed and console backends.
add_executable(lgp_test_text_input test_text_input.cpp)
target_link_libraries(lgp_test_text_input PRIVATE lgp_platform_common)
add_test(NAME platform.text_input COMMAND lgp_test_text_input)
set_tests_properties(platform.text_input PROPERTIES LABELS "platform")

# P7: app lifecycle relay of the SDL window (header only, no SDL needed).
add_executable(lgp_test_lifecycle test_lifecycle.cpp)
target_include_directories(lgp_test_lifecycle PRIVATE "${PROJECT_SOURCE_DIR}/platform/desktop/include")
add_test(NAME platform.lifecycle COMMAND lgp_test_lifecycle)
set_tests_properties(platform.lifecycle PROPERTIES LABELS "platform")

# SDL3 gamepad/keyboard backend with a virtual gamepad (no device needed).
if(TARGET lgp_platform_desktop)
    add_executable(lgp_test_sdl_text_input test_sdl_text_input.cpp)
    target_link_libraries(lgp_test_sdl_text_input PRIVATE lgp_platform_desktop SDL3::SDL3-static)
    add_test(NAME platform.sdl_text_input COMMAND lgp_test_sdl_text_input)
    set_tests_properties(platform.sdl_text_input PROPERTIES LABELS "platform")
    add_executable(lgp_test_sdl_input test_sdl_input.cpp)
    target_link_libraries(lgp_test_sdl_input PRIVATE lgp_platform_desktop SDL3::SDL3-static)
    add_test(NAME platform.sdl_input COMMAND lgp_test_sdl_input)
    set_tests_properties(platform.sdl_input PROPERTIES LABELS "platform")
endif()

# Moving header and footer units of the game's layouts (widescreen mod).
add_executable(lgp_test_layout_shift test_layout_shift.cpp)
target_link_libraries(lgp_test_layout_shift PRIVATE lgp_ui_layout lgp_gpu_nvn)
add_test(NAME platform.layout_shift COMMAND lgp_test_layout_shift)
set_tests_properties(platform.layout_shift PROPERTIES LABELS "platform")

# The app's menus (P7) on SDL events, dummy video driver.
if(TARGET lgp_platform_ui)
    add_executable(lgp_test_app_menu test_app_menu.cpp)
    target_link_libraries(lgp_test_app_menu PRIVATE lgp_platform_ui lgp_imgui SDL3::SDL3-static)
    add_test(NAME platform.app_menu COMMAND lgp_test_app_menu)
    set_tests_properties(platform.app_menu PROPERTIES LABELS "platform")
    # Pictures of the menus without a device (menu_preview.cpp --out FILE.ppm).
    add_executable(lgp_menu_preview menu_preview.cpp)
    target_link_libraries(lgp_menu_preview PRIVATE lgp_platform_ui lgp_platform_ui_assets lgp_gpu_nvn SDL3::SDL3-static)
    add_test(NAME platform.menu_preview COMMAND lgp_menu_preview)
    add_test(NAME platform.menu_preview_de COMMAND lgp_menu_preview --language de)
    set_tests_properties(platform.menu_preview platform.menu_preview_de PROPERTIES LABELS "platform")
    add_executable(lgp_test_sarc test_sarc.cpp)
    target_link_libraries(lgp_test_sarc PRIVATE lgp_platform_ui lgp_gpu_nvn)
    add_test(NAME platform.sarc COMMAND lgp_test_sarc)
    set_tests_properties(platform.sarc PROPERTIES LABELS "platform")
    add_executable(lgp_test_ui_assets test_ui_assets.cpp)
    target_link_libraries(lgp_test_ui_assets PRIVATE lgp_platform_ui_assets lgp_gpu_nvn lgp_imgui)
    add_test(NAME platform.ui_assets COMMAND lgp_test_ui_assets)
    set_tests_properties(platform.ui_assets PROPERTIES LABELS "platform")
    # Finding the game's interface files in a RomFS (sarc_list.cpp, local only).
    add_executable(lgp_sarc_list sarc_list.cpp)
    target_link_libraries(lgp_sarc_list PRIVATE lgp_platform_ui)
    add_executable(lgp_ui_dump ui_dump.cpp)
    target_link_libraries(lgp_ui_dump PRIVATE lgp_platform_ui lgp_platform_ui_assets)
    # How the game's layouts anchor what sits at the screen's edges (ui_anchors.cpp, counts only).
    add_executable(lgp_ui_anchors ui_anchors.cpp)
    target_link_libraries(lgp_ui_anchors PRIVATE lgp_platform_ui lgp_gpu_nvn)
    add_test(NAME platform.ui_anchors COMMAND lgp_ui_anchors --self-test)
    set_tests_properties(platform.ui_anchors PROPERTIES LABELS "platform")
endif()

# Acceptance: runtime/ has no OS, windowing or browser dependency.
if(Python3_EXECUTABLE)
    add_test(NAME platform.no_host_deps
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/check_no_host_deps.py"
                "${PROJECT_SOURCE_DIR}/runtime")
    add_test(NAME platform.no_host_deps_selftest
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/check_no_host_deps.py" --self-test)
    set_tests_properties(platform.no_host_deps platform.no_host_deps_selftest PROPERTIES LABELS "platform;runtime")
    # Every tr()/trf() text has its German line, Android and the Apple bundles
    # name the same texts in both languages.
    add_test(NAME platform.ui_text_table
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/check_ui_text.py" "${PROJECT_SOURCE_DIR}")
    add_test(NAME platform.ui_text_table_selftest
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/check_ui_text.py" --self-test)
    set_tests_properties(platform.ui_text_table platform.ui_text_table_selftest PROPERTIES LABELS "platform")
endif()

# P5.2: SDL3 audio sink on SDL's dummy driver (no sound hardware needed).
if(TARGET lgp_platform_sdl_audio)
    add_executable(lgp_test_sdl_audio test_sdl_audio.cpp)
    target_link_libraries(lgp_test_sdl_audio PRIVATE lgp_platform_sdl_audio)
    add_test(NAME platform.sdl_audio COMMAND lgp_test_sdl_audio)
    set_tests_properties(platform.sdl_audio PROPERTIES LABELS "platform;audio" ENVIRONMENT "SDL_AUDIO_DRIVER=dummy"
                         SKIP_RETURN_CODE 77 TIMEOUT 60)
endif()

# P5.2: ring buffer of the WebAudio sink. Natively the C++ side; under
# Emscripten (Node) also audio_worklet.js reading the ring in the wasm memory.
add_executable(lgp_test_audio_ring test_audio_ring.cpp)
target_include_directories(lgp_test_audio_ring PRIVATE "${PROJECT_SOURCE_DIR}/platform/web/include")
find_package(Threads REQUIRED)
target_link_libraries(lgp_test_audio_ring PRIVATE Threads::Threads)
if(TARGET lgp_platform_web)
    target_link_libraries(lgp_test_audio_ring PRIVATE lgp_platform_web)
    target_include_directories(lgp_test_audio_ring PRIVATE "${PROJECT_BINARY_DIR}/platform/generated")
    target_link_options(lgp_test_audio_ring PRIVATE -sALLOW_MEMORY_GROWTH=1)
endif()
add_test(NAME platform.audio_ring COMMAND lgp_test_audio_ring)
set_tests_properties(platform.audio_ring PROPERTIES LABELS "platform;audio" TIMEOUT 120)

# P6: NV12 conversion and keyframe detection of the video backends, on the
# self-made VP9 test video (tools/video/gen_test_webm.py).
set(LGP_TEST_VIDEO "${CMAKE_CURRENT_SOURCE_DIR}/data/vp9_pattern.webm" CACHE INTERNAL "VP9 test video")
add_executable(lgp_test_video_convert test_video_convert.cpp)
target_link_libraries(lgp_test_video_convert PRIVATE lgp_platform_common lgp_runtime_hle)
if(EMSCRIPTEN)
    target_link_options(lgp_test_video_convert PRIVATE -sNODERAWFS=1 -sALLOW_MEMORY_GROWTH=1)
endif()
add_test(NAME platform.video_convert COMMAND lgp_test_video_convert "${LGP_TEST_VIDEO}")
set_tests_properties(platform.video_convert PROPERTIES LABELS "platform;video")

# P7.3: a decoder that fails before its first picture hands over to another.
add_executable(lgp_test_fallback_video test_fallback_video.cpp)
target_link_libraries(lgp_test_fallback_video PRIVATE lgp_platform_common)
add_test(NAME platform.fallback_video COMMAND lgp_test_fallback_video)
set_tests_properties(platform.fallback_video PROPERTIES LABELS "platform;video")

# P5: FFmpeg video backend (only with LGP_FFMPEG_AVAILABLE): decoders open,
# broken input is reported, not fatal; the test video decodes correctly.
if(TARGET lgp_platform_ffmpeg)
    add_executable(lgp_test_ffmpeg_video test_ffmpeg_video.cpp)
    target_link_libraries(lgp_test_ffmpeg_video PRIVATE lgp_platform_ffmpeg lgp_runtime_hle)
    lgp_copy_ffmpeg_dlls(lgp_test_ffmpeg_video)
    add_test(NAME platform.ffmpeg_video COMMAND lgp_test_ffmpeg_video "${LGP_TEST_VIDEO}")
    set_tests_properties(platform.ffmpeg_video PROPERTIES LABELS "platform;video")
endif()
# libvpx (iOS and builds without FFmpeg): the same checks.
if(TARGET lgp_platform_vpx)
    add_executable(lgp_test_vpx_video test_vpx_video.cpp)
    target_link_libraries(lgp_test_vpx_video PRIVATE lgp_platform_vpx lgp_runtime_hle)
    add_test(NAME platform.vpx_video COMMAND lgp_test_vpx_video "${LGP_TEST_VIDEO}")
    set_tests_properties(platform.vpx_video PROPERTIES LABELS "platform;video")
endif()
# P7.3: the same video through Android's MediaCodec, on the phone (adb_run.py).
if(TARGET lgp_platform_android)
    add_executable(lgp_test_mediacodec_video test_mediacodec_video.cpp)
    target_link_libraries(lgp_test_mediacodec_video PRIVATE lgp_platform_android lgp_runtime_hle)
    add_test(NAME platform.mediacodec_video COMMAND lgp_test_mediacodec_video "${LGP_TEST_VIDEO}")
    set_tests_properties(platform.mediacodec_video PROPERTIES LABELS "platform;video;android")
    # Same check through the AImageReader path (YUV_420_888, docs/analysis/33).
    add_test(NAME platform.mediacodec_video_image_reader COMMAND lgp_test_mediacodec_video "${LGP_TEST_VIDEO}")
    set_tests_properties(platform.mediacodec_video_image_reader PROPERTIES LABELS "platform;video;android"
                         ENVIRONMENT "LGP_MEDIACODEC_IMAGE_READER=1")
endif()
