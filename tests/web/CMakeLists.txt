# Browser tests (P4.4): Emscripten pages in Chromium through Playwright, with
# WebGPU on SwiftShader. Label "browser". Skipped (exit 77) without
# Playwright/Chromium or a WebGPU adapter; LGP_PLAYWRIGHT_DIR points at a
# Playwright package outside the global npm root.
find_program(LGP_NODE NAMES node DOC "Node.js for the browser tests")
if(NOT LGP_NODE)
    return()
endif()

# P6.1: the page logic of platform/web (which picked files go where in OPFS,
# argv) in Node, on every build. Label "web".
add_test(NAME web.page_logic COMMAND "${LGP_NODE}" "${CMAKE_CURRENT_SOURCE_DIR}/lgp_web_test.mjs")
set_tests_properties(web.page_logic PROPERTIES LABELS "web")

if(NOT EMSCRIPTEN)
    return()
endif()
find_program(LGP_XVFB_RUN NAMES xvfb-run DOC "xvfb-run: headed Chromium without a display")
set(_runner "${LGP_NODE}" "${CMAKE_CURRENT_SOURCE_DIR}/browser_test.mjs")
if(LGP_XVFB_RUN)
    set(_runner "${LGP_XVFB_RUN}" -a -s "-screen 0 1600x1200x24" ${_runner})
endif()

# P6.1: OPFS as IStorage. The page's folder picker copies a made-up dump
# (test_data.mjs) into OPFS, the program checks it through IStorage and
# writes a save; a second page load checks that both persisted.
add_executable(lgp_test_opfs_storage opfs_storage_test.cpp)
target_link_libraries(lgp_test_opfs_storage PRIVATE lgp_platform_web lgp_runtime_hle)
lgp_add_web_page(lgp_test_opfs_storage TITLE "lgp OPFS storage test")
set(_test_data "${CMAKE_CURRENT_BINARY_DIR}/test_data")
add_test(NAME browser.opfs_storage_data COMMAND "${LGP_NODE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_data.mjs" "${_test_data}")
set_tests_properties(browser.opfs_storage_data PROPERTIES LABELS "browser;web" FIXTURES_SETUP lgp_web_test_data)
add_test(NAME browser.opfs_storage
         COMMAND ${_runner} --dir "$<TARGET_FILE_DIR:lgp_test_opfs_storage>" --page lgp_test_opfs_storage.html
                 --pick "${_test_data}/lgp_test_dump" --args "--phase import" --expect "^opfs storage import: ok$"
                 --then-args "--phase persist" --then-expect "^opfs storage persist: ok$" --no-canvas-check)
set_tests_properties(browser.opfs_storage PROPERTIES LABELS "browser;web" SKIP_RETURN_CODE 77 TIMEOUT 180
                     FIXTURES_REQUIRED lgp_web_test_data)

# P5.3: save export/import of the page (ZIP download, file input) on the same
# page, without running the program. Headless Chromium is enough (no WebGPU).
add_test(NAME browser.save_backup
         COMMAND "${LGP_NODE}" "${CMAKE_CURRENT_SOURCE_DIR}/save_backup_test.mjs"
                 --dir "$<TARGET_FILE_DIR:lgp_test_opfs_storage>" --page lgp_test_opfs_storage.html)
set_tests_properties(browser.save_backup PROPERTIES LABELS "browser;web" SKIP_RETURN_CODE 77 TIMEOUT 120)

# P6.1: the boot program in the browser on homebrew fixtures from a native
# run (LGP_FIXTURE_IMPORT_DIR, see tests/homebrew): the module is picked from
# a folder like the user's dump, read from OPFS, and the run has to match the
# Unicorn end state. services (fsp-srv RomFS and saves on OPFS) only when its
# lifted code was exported too (tests/boot/generated/services_nso_lifted.cpp).
if(LGP_FIXTURE_IMPORT_DIR)
    set(_web_argv "--exefs /opfs/lgp/data/exefs --game /opfs/lgp/data --save /opfs/lgp/save")
    foreach(fmt IN ITEMS nso nro)
        set(_target lgp_web_boot_basic_${fmt})
        lgp_add_boot_executable(${_target} "${LGP_FIXTURE_IMPORT_DIR}/basic_${fmt}_lifted.cpp")
        add_dependencies(lgp_fixture_runners ${_target})
        set(_dump "${CMAKE_CURRENT_BINARY_DIR}/basic_${fmt}_dump")
        file(MAKE_DIRECTORY "${_dump}")
        file(COPY_FILE "${LGP_FIXTURE_IMPORT_DIR}/basic.${fmt}" "${_dump}/basic.${fmt}" ONLY_IF_DIFFERENT)
        add_test(NAME browser.boot_basic_${fmt}
                 COMMAND ${_runner} --dir "$<TARGET_FILE_DIR:${_target}>" --page ${_target}.html --pick "${_dump}"
                         --argv "${_web_argv} --sched baton" --expect "^exit: "
                         --fixture-expected "${LGP_FIXTURE_IMPORT_DIR}/basic_${fmt}.expected" --no-canvas-check)
        set_tests_properties(browser.boot_basic_${fmt} PROPERTIES LABELS "browser;fixtures;boot" SKIP_RETURN_CODE 77
                             TIMEOUT 300)
    endforeach()
    if(EXISTS "${LGP_FIXTURE_IMPORT_DIR}/services_nso_lifted.cpp")
        lgp_add_boot_executable(lgp_web_boot_services_nso "${LGP_FIXTURE_IMPORT_DIR}/services_nso_lifted.cpp")
        add_dependencies(lgp_fixture_runners lgp_web_boot_services_nso)
        set(_dump "${CMAKE_CURRENT_BINARY_DIR}/services_dump")
        file(MAKE_DIRECTORY "${_dump}/exefs" "${_dump}/romfs/dir")
        file(COPY_FILE "${LGP_FIXTURE_IMPORT_DIR}/services.nso" "${_dump}/exefs/services.nso" ONLY_IF_DIFFERENT)
        file(WRITE "${_dump}/romfs/hello.txt" "hello from romfs\n")
        find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)
        execute_process(COMMAND "${Python3_EXECUTABLE}" -c "import sys; open(sys.argv[1], 'wb').write(bytes(range(256)))"
                                "${_dump}/romfs/dir/nested.bin")
        add_test(NAME browser.boot_services_nso
                 COMMAND ${_runner} --dir "$<TARGET_FILE_DIR:lgp_web_boot_services_nso>"
                         --page lgp_web_boot_services_nso.html --pick "${_dump}"
                         --argv "${_web_argv} --program-id 0100c0ffee000001 --language de --nickname lgp --time 1700000000 --keyboard none"
                         --expect "^exit: " --lines "${PROJECT_SOURCE_DIR}/tests/boot/expected/services.txt"
                         --absent FAILED --no-canvas-check)
        set_tests_properties(browser.boot_services_nso PROPERTIES LABELS "browser;fixtures;boot" SKIP_RETURN_CODE 77
                             TIMEOUT 300)
    endif()
endif()

# P6: what the browser offers the web build (tests/web/feature_probe.html).
# Fails only when something the build cannot run without is missing; the
# other lines document browser differences (run with LGP_BROWSER=safari).
add_test(NAME browser.feature_probe
         COMMAND ${_runner} --dir "${CMAKE_CURRENT_SOURCE_DIR}" --page feature_probe.html
                 --expect "^probe: required features: all present$" --no-canvas-check)
set_tests_properties(browser.feature_probe PROPERTIES LABELS "browser;web" SKIP_RETURN_CODE 77 TIMEOUT 120)

# P5.1: key events in the page reach the web input backend (no WebGPU).
set(_shell "${PROJECT_SOURCE_DIR}/platform/web/shell.html")
add_executable(lgp_web_input_probe web_input_probe.cpp)
target_link_libraries(lgp_web_input_probe PRIVATE lgp_platform_web)
set_target_properties(lgp_web_input_probe PROPERTIES SUFFIX ".html" LINK_DEPENDS "${_shell}")
target_link_options(lgp_web_input_probe PRIVATE "--shell-file=${_shell}")
add_test(NAME browser.web_input
         COMMAND ${_runner} --dir "$<TARGET_FILE_DIR:lgp_web_input_probe>" --page lgp_web_input_probe.html
                 --expect "^input: ok" --no-canvas-check --press-after "^input ready$" --press "z,ArrowUp,d")
set_tests_properties(browser.web_input PROPERTIES LABELS "browser;platform" SKIP_RETURN_CODE 77 TIMEOUT 120)

# Software keyboard as a page dialog: typing goes into the dialog, not to
# the controllers.
add_executable(lgp_web_text_input_probe web_text_input_probe.cpp)
target_link_libraries(lgp_web_text_input_probe PRIVATE lgp_platform_web)
set_target_properties(lgp_web_text_input_probe PROPERTIES SUFFIX ".html" LINK_DEPENDS "${_shell}")
target_link_options(lgp_web_text_input_probe PRIVATE "--shell-file=${_shell}")
add_test(NAME browser.web_text_input
         COMMAND ${_runner} --dir "$<TARGET_FILE_DIR:lgp_web_text_input_probe>" --page lgp_web_text_input_probe.html
                 --expect "^text input: ok" --no-canvas-check --press-after "^keyboard ready$" --press "x,Enter")
set_tests_properties(browser.web_text_input PROPERTIES LABELS "browser;platform" SKIP_RETURN_CODE 77 TIMEOUT 120)

# Error applet message (P3.5): the same kind of dialog with only an OK button.
add_executable(lgp_web_message_probe web_message_probe.cpp)
target_link_libraries(lgp_web_message_probe PRIVATE lgp_platform_web)
set_target_properties(lgp_web_message_probe PROPERTIES SUFFIX ".html" LINK_DEPENDS "${_shell}")
target_link_options(lgp_web_message_probe PRIVATE "--shell-file=${_shell}")
add_test(NAME browser.web_message
         COMMAND ${_runner} --dir "$<TARGET_FILE_DIR:lgp_web_message_probe>" --page lgp_web_message_probe.html
                 --expect "^message: ok" --no-canvas-check --press-after "^message ready$" --press "Enter")
set_tests_properties(browser.web_message PROPERTIES LABELS "browser;platform" SKIP_RETURN_CODE 77 TIMEOUT 120)

# P5.2: the WebAudio sink plays a tone through its AudioWorklet (Chromium
# starts the AudioContext without a gesture: --autoplay-policy in
# browser_test.mjs). Uses the project's shell page; no canvas output.
if(TARGET lgp_platform_web)
    add_executable(lgp_test_web_audio_browser web_audio_browser.cpp)
    target_link_libraries(lgp_test_web_audio_browser PRIVATE lgp_platform_web)
    set(_shell "${PROJECT_SOURCE_DIR}/platform/web/shell.html")
    set_target_properties(lgp_test_web_audio_browser PROPERTIES SUFFIX ".html" LINK_DEPENDS "${_shell}")
    target_link_options(lgp_test_web_audio_browser PRIVATE -sALLOW_MEMORY_GROWTH=1 "--shell-file=${_shell}")
    add_test(NAME browser.web_audio
             COMMAND ${_runner} --dir "$<TARGET_FILE_DIR:lgp_test_web_audio_browser>"
                     --page lgp_test_web_audio_browser.html --expect "^web audio: ok" --no-canvas-check
                     --needs-user-gesture)
    set_tests_properties(browser.web_audio PROPERTIES LABELS "browser;audio" SKIP_RETURN_CODE 77 TIMEOUT 120)
endif()

# P6: in-game videos through WebCodecs (platform/web/src/web_video.cpp): the
# self-made VP9 test video decodes to the expected NV12 pictures on all three
# copy paths. Playwright's Chromium decodes VP9 (H.264 needs Chrome).
if(TARGET lgp_platform_web)
    add_executable(lgp_test_web_video_browser web_video_browser.cpp)
    target_link_libraries(lgp_test_web_video_browser PRIVATE lgp_platform_web lgp_runtime_hle)
    set(_shell "${PROJECT_SOURCE_DIR}/platform/web/shell.html")
    set_target_properties(lgp_test_web_video_browser PROPERTIES SUFFIX ".html"
                          LINK_DEPENDS "${_shell};${LGP_TEST_VIDEO}")
    target_link_options(lgp_test_web_video_browser PRIVATE -sALLOW_MEMORY_GROWTH=1 "--shell-file=${_shell}"
                        "--embed-file=${LGP_TEST_VIDEO}@/vp9.webm")
    add_test(NAME browser.web_video
             COMMAND ${_runner} --dir "$<TARGET_FILE_DIR:lgp_test_web_video_browser>"
                     --page lgp_test_web_video_browser.html --expect "^web video: ok" --no-canvas-check)
    set_tests_properties(browser.web_video PROPERTIES LABELS "browser;video" SKIP_RETURN_CODE 77 TIMEOUT 120)
endif()

if(NOT TARGET lgp_webgpu_demo)
    return()
endif()
# The demo keeps at most two frames in flight, so a slow GPU (lavapipe on a
# busy runner) slows the page down instead of piling up work: allow 100 s,
# and ctest 150 s, so a page that does not exit still gets its hang report.
add_test(NAME browser.webgpu_demo
         COMMAND ${_runner} --dir "$<TARGET_FILE_DIR:lgp_webgpu_demo>" --page lgp_webgpu_demo.html
                 --args "--frames 600" --expect "^frames: 600, WebGPU errors: 0$" --timeout 100000)
set_tests_properties(browser.webgpu_demo PROPERTIES LABELS "browser;webgpu" SKIP_RETURN_CODE 77 TIMEOUT 150)
# The surface is tried again when getContext('webgpu') fails at first (Safari
# on iPadOS, sporadically on the first start).
add_test(NAME browser.webgpu_demo_surface_retry
         COMMAND ${_runner} --dir "$<TARGET_FILE_DIR:lgp_webgpu_demo>" --page lgp_webgpu_demo.html
                 --args "--frames 600" --expect "^frames: 600, WebGPU errors: 0$" --webgpu-context-fails 2
                 --timeout 100000 --lines "${CMAKE_CURRENT_SOURCE_DIR}/webgpu_demo_surface_retry.lines")
set_tests_properties(browser.webgpu_demo_surface_retry PROPERTIES LABELS "browser;webgpu" SKIP_RETURN_CODE 77
                     TIMEOUT 150)
# The same with WebGPU and the canvas on a pthread of their own (off by default).
add_test(NAME browser.webgpu_demo_gpu_thread
         COMMAND ${_runner} --dir "$<TARGET_FILE_DIR:lgp_webgpu_demo>" --page lgp_webgpu_demo.html
                 --args "--frames 600 --gpu-thread" --expect "^frames: 600, WebGPU errors: 0$" --timeout 100000
                 --lines "${CMAKE_CURRENT_SOURCE_DIR}/webgpu_demo_gpu_thread.lines")
set_tests_properties(browser.webgpu_demo_gpu_thread PROPERTIES LABELS "browser;webgpu" SKIP_RETURN_CODE 77 TIMEOUT 150)

# Analysis 21, proposal A1: WebGPU device and canvas on a dedicated pthread
# (OffscreenCanvas) instead of the browser's main thread; measures the
# round trip of synchronous and asynchronous hand-offs to either thread.
add_executable(lgp_webgpu_worker_probe webgpu_worker_probe.cpp)
target_link_libraries(lgp_webgpu_worker_probe PRIVATE lgp_webgpu)
set_target_properties(lgp_webgpu_worker_probe PROPERTIES SUFFIX ".html" LINK_DEPENDS "${_shell}")
target_link_options(lgp_webgpu_worker_probe PRIVATE "--shell-file=${_shell}" -sOFFSCREENCANVAS_SUPPORT=1
                    "-sOFFSCREENCANVASES_TO_PTHREAD=#canvas")
add_test(NAME browser.webgpu_worker_probe
         COMMAND ${_runner} --dir "$<TARGET_FILE_DIR:lgp_webgpu_worker_probe>" --page lgp_webgpu_worker_probe.html
                 --expect "^webgpu worker probe: ok$")
set_tests_properties(browser.webgpu_worker_probe PROPERTIES LABELS "browser;webgpu" SKIP_RETURN_CODE 77 TIMEOUT 180)

# P4.1 part 2: the lifted AArch64 NVN test program draws and checks pixels
# (tests/nvn, built when a native tree provided the generated files). It
# renders offscreen and exits quickly, so the canvas is not sampled.
if(TARGET lgp_test_nvn_triangle_web)
    add_test(NAME browser.nvn_triangle
             COMMAND ${_runner} --dir "$<TARGET_FILE_DIR:lgp_test_nvn_triangle_web>"
                     --page lgp_test_nvn_triangle_web.html --args "/nvn_triangle.img"
                     --expect "^nvn triangle: ok$" --no-canvas-check)
    set_tests_properties(browser.nvn_triangle PROPERTIES LABELS "browser;webgpu;nvn" SKIP_RETURN_CODE 77 TIMEOUT 120)
endif()

# Pipeline cache: draw pipelines recorded by one backend, created ahead with
# createRenderPipelineAsync by the next (tests/nvn/test_pipeline_cache.cpp).
if(TARGET lgp_test_pipeline_cache_web)
    add_test(NAME browser.pipeline_cache
             COMMAND ${_runner} --dir "$<TARGET_FILE_DIR:lgp_test_pipeline_cache_web>"
                     --page lgp_test_pipeline_cache_web.html --args "gpu"
                     --expect "^all checks passed$" --no-canvas-check)
    set_tests_properties(browser.pipeline_cache PROPERTIES LABELS "browser;webgpu;nvn" SKIP_RETURN_CODE 77 TIMEOUT 120)
endif()

# More vertex streams than the browser's 8 vertex buffers: merged streams
# (tests/nvn/test_webgpu_vertex_streams.cpp).
if(TARGET lgp_test_webgpu_vertex_streams_web)
    add_test(NAME browser.webgpu_vertex_streams
             COMMAND ${_runner} --dir "$<TARGET_FILE_DIR:lgp_test_webgpu_vertex_streams_web>"
                     --page lgp_test_webgpu_vertex_streams_web.html
                     --expect "^all checks passed$" --no-canvas-check)
    set_tests_properties(browser.webgpu_vertex_streams PROPERTIES LABELS "browser;webgpu;nvn" SKIP_RETURN_CODE 77
                         TIMEOUT 120)
endif()
