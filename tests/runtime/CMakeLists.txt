# Runtime tests (P2.6/P3). Label "runtime"; the Unicorn-backed kernel programs
# additionally carry "unicorn".

# Second runtime library with the checked accessors and write-watch compiled
# in, so those paths are also tested in Release and WASM builds. Separate
# library because the switches change inline code (see guest_mem.hpp).
lgp_add_runtime_core(lgp_runtime_core_checked DEFINITIONS LGP_GUEST_MEM_CHECKED=1 LGP_GUEST_MEM_WRITE_WATCH=1)
lgp_add_runtime_hle(lgp_runtime_hle_checked lgp_runtime_core_checked)

function(lgp_runtime_test name source lib)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE ${lib})
    if(EMSCRIPTEN)
        # Test layouts are small, but the guest block is one allocation.
        target_link_options(${name} PRIVATE -sALLOW_MEMORY_GROWTH=1 -sMAXIMUM_MEMORY=4GB)
    endif()
endfunction()

lgp_runtime_test(lgp_test_guest_memory test_guest_memory.cpp lgp_runtime_core)
add_test(NAME runtime.guest_memory COMMAND lgp_test_guest_memory)
lgp_runtime_test(lgp_test_guest_memory_checked test_guest_memory.cpp lgp_runtime_core_checked)
add_test(NAME runtime.guest_memory_checked COMMAND lgp_test_guest_memory_checked)
lgp_runtime_test(lgp_test_kernel_svc test_kernel_svc.cpp lgp_runtime_core)
add_test(NAME runtime.kernel_svc COMMAND lgp_test_kernel_svc)
set_tests_properties(runtime.guest_memory runtime.guest_memory_checked runtime.kernel_svc PROPERTIES LABELS "runtime")

# P3.2: thread and synchronization svcs, hand-written guest functions.
foreach(variant IN ITEMS "" "_checked")
    lgp_runtime_test(lgp_test_threads_svc${variant} test_threads_svc.cpp lgp_runtime_core${variant})
    add_test(NAME runtime.threads_svc${variant} COMMAND lgp_test_threads_svc${variant})
    # The same semantics on the virtual guest clock (clock.hpp).
    add_test(NAME runtime.threads_svc_virtual${variant} COMMAND lgp_test_threads_svc${variant} virtual)
    set_tests_properties(runtime.threads_svc${variant} runtime.threads_svc_virtual${variant}
                         PROPERTIES LABELS "runtime" TIMEOUT 120)
endforeach()

# Virtual guest clock: reproducible runs with timeouts and periodic host
# callbacks under the single baton (clock.hpp).
foreach(variant IN ITEMS "" "_checked")
    lgp_runtime_test(lgp_test_virtual_clock${variant} test_virtual_clock.cpp lgp_runtime_core${variant})
    add_test(NAME runtime.virtual_clock${variant} COMMAND lgp_test_virtual_clock${variant})
    set_tests_properties(runtime.virtual_clock${variant} PROPERTIES LABELS "runtime" TIMEOUT 120)
endforeach()

# P6: wall time per guest thread and category (time_stats.hpp).
lgp_runtime_test(lgp_test_time_stats test_time_stats.cpp lgp_runtime_core)
add_test(NAME runtime.time_stats COMMAND lgp_test_time_stats)
set_tests_properties(runtime.time_stats PROPERTIES LABELS "runtime" TIMEOUT 60)

# Session lock without the core slot while the holder waits without it (ldn).
lgp_runtime_test(lgp_test_ipc_slot test_ipc_slot.cpp lgp_runtime_hle)
add_test(NAME runtime.ipc_slot COMMAND lgp_test_ipc_slot)
set_tests_properties(runtime.ipc_slot PROPERTIES LABELS "runtime" TIMEOUT 30)

# P3.3: HIPC/CMIF sessions, domains and sm: through the IPC svcs.
foreach(variant IN ITEMS "" "_checked")
    lgp_runtime_test(lgp_test_ipc_hle${variant} test_ipc_hle.cpp lgp_runtime_hle${variant})
    add_test(NAME runtime.ipc_hle${variant} COMMAND lgp_test_ipc_hle${variant})
    set_tests_properties(runtime.ipc_hle${variant} PROPERTIES LABELS "runtime")
endforeach()

# P3.4: base services (appletOE, set, time:u, csrng, pctl, acc:u0).
foreach(variant IN ITEMS "" "_checked")
    lgp_runtime_test(lgp_test_hle_services${variant} test_hle_services.cpp lgp_runtime_hle${variant})
    add_test(NAME runtime.hle_services${variant} COMMAND lgp_test_hle_services${variant})
    set_tests_properties(runtime.hle_services${variant} PROPERTIES LABELS "runtime")
endforeach()

# ldn over LAN: two runtimes in one process join one ldn network over
# 127.0.0.1 and exchange bsd:u datagrams (docs/analysis/44-multiplayer-stand.md).
lgp_runtime_test(lgp_test_hle_ldn_lan test_hle_ldn_lan.cpp lgp_runtime_hle)
add_test(NAME runtime.hle_ldn_lan COMMAND lgp_test_hle_ldn_lan)
set_tests_properties(runtime.hle_ldn_lan PROPERTIES LABELS "runtime")
# ldn over a RyuLDN server: a small server in the test on 127.0.0.1, two
# runtimes open and join a room through it (docs/knowledge/ldn-ueber-ryuldn.md).
lgp_runtime_test(lgp_test_hle_ldn_ryu test_hle_ldn_ryu.cpp lgp_runtime_hle)
if(WIN32)
    target_link_libraries(lgp_test_hle_ldn_ryu PRIVATE ws2_32)
endif()
add_test(NAME runtime.hle_ldn_ryu COMMAND lgp_test_hle_ldn_ryu)
set_tests_properties(runtime.hle_ldn_ryu PROPERTIES LABELS "runtime")
# The same through the WebSocket bridge (tools/ldn/ws_bridge.py, ws://).
if(NOT EMSCRIPTEN AND Python3_EXECUTABLE)
    add_test(NAME runtime.hle_ldn_ryu_ws
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/run_ldn_ws_bridge.py"
                $<TARGET_FILE:lgp_test_hle_ldn_ryu> "${PROJECT_SOURCE_DIR}/tools/ldn/ws_bridge.py")
    set_tests_properties(runtime.hle_ldn_ryu_ws PROPERTIES LABELS "runtime")
    # wss:// (Mbed TLS) with a self-signed certificate the test makes with
    # openssl; without openssl (or without TLS in the build) it is not added.
    find_program(LGP_OPENSSL_EXECUTABLE openssl HINTS "C:/Program Files/Git/usr/bin")
    if(LGP_MBEDTLS_AVAILABLE AND LGP_OPENSSL_EXECUTABLE)
        add_test(NAME runtime.hle_ldn_ryu_wss
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/run_ldn_ws_bridge.py"
                    $<TARGET_FILE:lgp_test_hle_ldn_ryu> "${PROJECT_SOURCE_DIR}/tools/ldn/ws_bridge.py"
                    --tls "${LGP_OPENSSL_EXECUTABLE}")
        set_tests_properties(runtime.hle_ldn_ryu_wss PROPERTIES LABELS "runtime")
    endif()
endif()
# Diagnostic, not a test: an LGP-shaped ldn access point that never scans,
# for a two-process run against one lgp_game (Analyse 44).
lgp_runtime_test(lgp_ldn_fakehost ldn_fakehost.cpp lgp_runtime_hle)

# P3.4: fsp-srv (RomFS images, system data, save data) over MemoryStorage.
foreach(variant IN ITEMS "" "_checked")
    lgp_runtime_test(lgp_test_hle_fs${variant} test_hle_fs.cpp lgp_runtime_hle${variant})
    target_link_libraries(lgp_test_hle_fs${variant} PRIVATE lgp_platform_common)
    add_test(NAME runtime.hle_fs${variant} COMMAND lgp_test_hle_fs${variant})
    set_tests_properties(runtime.hle_fs${variant} PROPERTIES LABELS "runtime")
endforeach()

# P5: WebM demuxer of the movie:: HLE, on files built by the test.
lgp_runtime_test(lgp_test_hle_webm test_hle_webm.cpp lgp_runtime_hle)
add_test(NAME runtime.hle_webm COMMAND lgp_test_hle_webm)
set_tests_properties(runtime.hle_webm PROPERTIES LABELS "runtime")

# P5: Vorbis sound of in-game videos, on the self-made test video.
lgp_runtime_test(lgp_test_hle_vorbis test_hle_vorbis.cpp lgp_runtime_hle)
if(LGP_VORBIS_AVAILABLE)
    target_compile_definitions(lgp_test_hle_vorbis PRIVATE LGP_TEST_VORBIS=1)
endif()
if(EMSCRIPTEN)
    target_link_options(lgp_test_hle_vorbis PRIVATE -sNODERAWFS=1)
endif()
add_test(NAME runtime.hle_vorbis COMMAND lgp_test_hle_vorbis "${PROJECT_SOURCE_DIR}/tests/platform/data/av_pattern.webm")
set_tests_properties(runtime.hle_vorbis PROPERTIES LABELS "runtime;audio;video")

# P5: the movie:: HLE driven like the game's player, on the same test video.
if(TARGET lgp_boot)
    lgp_runtime_test(lgp_test_movie_hle test_movie_hle.cpp lgp_boot)
    target_link_libraries(lgp_test_movie_hle PRIVATE lgp_platform_common)
    if(LGP_VORBIS_AVAILABLE)
        target_compile_definitions(lgp_test_movie_hle PRIVATE LGP_TEST_VORBIS=1)
    endif()
    if(EMSCRIPTEN)
        target_link_options(lgp_test_movie_hle PRIVATE -sNODERAWFS=1)
    endif()
    add_test(NAME runtime.movie_hle COMMAND lgp_test_movie_hle "${PROJECT_SOURCE_DIR}/tests/platform/data/av_pattern.webm")
    set_tests_properties(runtime.movie_hle PROPERTIES LABELS "runtime;audio;video")
    # The boot program's --press script (frame, time and event triggers).
    lgp_runtime_test(lgp_test_press_script test_press_script.cpp lgp_boot)
    target_link_libraries(lgp_test_press_script PRIVATE lgp_platform_common)  # ControllerMapper (throw)
    add_test(NAME boot.press_script COMMAND lgp_test_press_script)
    set_tests_properties(boot.press_script PROPERTIES LABELS "boot;runtime")
    lgp_runtime_test(lgp_test_boot_keyboard test_boot_keyboard.cpp lgp_boot)
    add_test(NAME boot.keyboard COMMAND lgp_test_boot_keyboard)
    set_tests_properties(boot.keyboard PROPERTIES LABELS "boot;runtime")
    # App mode (P7.1): settings file, game folder check, command line, user folders.
    lgp_runtime_test(lgp_test_boot_app test_boot_app.cpp lgp_boot)
    if(EMSCRIPTEN)
        target_link_options(lgp_test_boot_app PRIVATE -sNODERAWFS=1)
    endif()
    add_test(NAME boot.app COMMAND lgp_test_boot_app)
    set_tests_properties(boot.app PROPERTIES LABELS "boot;runtime")
    # Cloud backup of the saves (P7.4, tvOS iCloud) against a cloud in memory.
    lgp_runtime_test(lgp_test_save_backup test_save_backup.cpp lgp_boot)
    if(EMSCRIPTEN)
        target_link_options(lgp_test_save_backup PRIVATE -sNODERAWFS=1)
    endif()
    add_test(NAME boot.save_backup COMMAND lgp_test_save_backup)
    set_tests_properties(boot.save_backup PROPERTIES LABELS "boot;runtime")
    lgp_runtime_test(lgp_test_frame_pacer test_frame_pacer.cpp lgp_boot)
    add_test(NAME boot.frame_pacer COMMAND lgp_test_frame_pacer)
    set_tests_properties(boot.frame_pacer PROPERTIES LABELS "boot;runtime")
    lgp_runtime_test(lgp_test_stall_report test_stall_report.cpp lgp_boot)
    add_test(NAME boot.stall_report COMMAND lgp_test_stall_report)
    set_tests_properties(boot.stall_report PROPERTIES LABELS "boot;runtime")
    lgp_runtime_test(lgp_test_process_control test_process_control.cpp lgp_boot)
    add_test(NAME boot.process_control COMMAND lgp_test_process_control)
    set_tests_properties(boot.process_control PROPERTIES LABELS "boot;runtime")
endif()

# P5.1: hid (npads, motion sensors, vibration) over ManualInput.
foreach(variant IN ITEMS "" "_checked")
    lgp_runtime_test(lgp_test_hle_hid${variant} test_hle_hid.cpp lgp_runtime_hle${variant})
    target_link_libraries(lgp_test_hle_hid${variant} PRIVATE lgp_platform_common)
    add_test(NAME runtime.hle_hid${variant} COMMAND lgp_test_hle_hid${variant})
    set_tests_properties(runtime.hle_hid${variant} PROPERTIES LABELS "runtime")
endforeach()

# P5.2: hwopus. With libopus the test encodes its own signal with libopus'
# encoder and checks the service's PCM against it; without, silence.
foreach(variant IN ITEMS "" "_checked")
    lgp_runtime_test(lgp_test_hle_hwopus${variant} test_hle_hwopus.cpp lgp_runtime_hle${variant})
    if(LGP_OPUS_AVAILABLE)
        target_link_libraries(lgp_test_hle_hwopus${variant} PRIVATE Opus::opus)
        target_compile_definitions(lgp_test_hle_hwopus${variant} PRIVATE LGP_TEST_OPUS=1)
    endif()
    add_test(NAME runtime.hle_hwopus${variant} COMMAND lgp_test_hle_hwopus${variant})
    set_tests_properties(runtime.hle_hwopus${variant} PROPERTIES LABELS "runtime;audio")
endforeach()

# guest.hpp against the lifter's CpuState and lifted.hpp (recomp/lifter, P2.3).
foreach(variant IN ITEMS "" "_checked")
    lgp_runtime_test(lgp_test_guest_header${variant} test_guest_header.cpp lgp_runtime_core${variant})
    target_link_libraries(lgp_test_guest_header${variant} PRIVATE lgp_lifted_headers)
    add_test(NAME runtime.guest_header${variant} COMMAND lgp_test_guest_header${variant})
    set_tests_properties(runtime.guest_header${variant} PROPERTIES LABELS "runtime")
endforeach()

# Dispatch table and unknown-target error path (P2.5, ADR-0004).
# P8: game mods on hook doors (runtime/mods).
lgp_runtime_test(lgp_test_mods test_mods.cpp lgp_mods)
# widescreen_layouts builds layouts with the platform tests' fixtures.
target_link_libraries(lgp_test_mods PRIVATE lgp_ui_layout lgp_gpu_nvn)
target_include_directories(lgp_test_mods PRIVATE "${PROJECT_SOURCE_DIR}/tests/platform")
add_test(NAME runtime.mods COMMAND lgp_test_mods)
set_tests_properties(runtime.mods PROPERTIES LABELS "runtime;mods")

lgp_runtime_test(lgp_test_dispatch test_dispatch.cpp lgp_runtime_core)
target_link_libraries(lgp_test_dispatch PRIVATE lgp_lifted_headers)
add_test(NAME runtime.dispatch COMMAND lgp_test_dispatch)
set_tests_properties(runtime.dispatch PROPERTIES LABELS "runtime")

# P2.6 acceptance: the kernel test programs lifted by the P2.3 lifter run
# against the real runtime. The generator runs on the build host, so this is
# skipped when cross-compiling (Emscripten), like the lifter's own cases.
if(NOT CMAKE_CROSSCOMPILING)
    add_executable(lgp_lift_kernel_programs lift_kernel_programs.cpp)
    target_link_libraries(lgp_lift_kernel_programs PRIVATE lgp_recomp_lifter)
    set(_lifted "${CMAKE_CURRENT_BINARY_DIR}/generated/kernel_programs_lifted.cpp")
    add_custom_command(
        OUTPUT "${_lifted}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
        COMMAND lgp_lift_kernel_programs "${_lifted}"
        DEPENDS lgp_lift_kernel_programs
        COMMENT "Lifting the kernel test programs"
        VERBATIM)
    foreach(variant IN ITEMS "" "_checked")
        lgp_runtime_test(lgp_test_kernel_lifted${variant} test_kernel_lifted.cpp lgp_runtime_core${variant})
        target_sources(lgp_test_kernel_lifted${variant} PRIVATE "${_lifted}")
        target_link_libraries(lgp_test_kernel_lifted${variant} PRIVATE lgp_lifted_headers)
        add_test(NAME runtime.kernel_lifted${variant} COMMAND lgp_test_kernel_lifted${variant})
        set_tests_properties(runtime.kernel_lifted${variant} PROPERTIES LABELS "runtime")
    endforeach()

    # P3.2 acceptance: multi-thread programs (thread_programs.s, assembled into
    # thread_program_code.hpp by gen_thread_programs.py) on the scheduler.
    add_executable(lgp_lift_thread_programs lift_thread_programs.cpp)
    target_link_libraries(lgp_lift_thread_programs PRIVATE lgp_recomp_lifter)
    set(_lifted_threads "${CMAKE_CURRENT_BINARY_DIR}/generated/thread_programs_lifted.cpp")
    add_custom_command(
        OUTPUT "${_lifted_threads}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
        COMMAND lgp_lift_thread_programs "${_lifted_threads}"
        DEPENDS lgp_lift_thread_programs
        COMMENT "Lifting the thread test programs"
        VERBATIM)
    foreach(variant IN ITEMS "" "_checked")
        lgp_runtime_test(lgp_test_threads_lifted${variant} test_threads_lifted.cpp lgp_runtime_hle${variant})
        target_sources(lgp_test_threads_lifted${variant} PRIVATE "${_lifted_threads}")
        target_include_directories(lgp_test_threads_lifted${variant} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
        # The bare spin-wait of spin_main only gives up the baton through the
        # back-edge poll (guest.hpp).
        target_compile_definitions(lgp_test_threads_lifted${variant} PRIVATE LGP_BACKEDGE_YIELD=1)
        add_test(NAME runtime.threads_lifted${variant} COMMAND lgp_test_threads_lifted${variant} threads)
        # P3.3 acceptance: ipc_main talks to sm: and a test service.
        add_test(NAME runtime.ipc_lifted${variant} COMMAND lgp_test_threads_lifted${variant} ipc)
        # The same programs on the virtual guest clock (clock.hpp).
        add_test(NAME runtime.threads_lifted_virtual${variant}
                 COMMAND lgp_test_threads_lifted${variant} threads virtual)
        add_test(NAME runtime.ipc_lifted_virtual${variant} COMMAND lgp_test_threads_lifted${variant} ipc virtual)
        # A scheduler bug shows up as a hang.
        set_tests_properties(runtime.threads_lifted${variant} runtime.ipc_lifted${variant}
                             runtime.threads_lifted_virtual${variant} runtime.ipc_lifted_virtual${variant}
                             PROPERTIES LABELS "runtime" TIMEOUT 120)
    endforeach()
endif()

if(LGP_ENABLE_UNICORN)
    add_executable(lgp_test_kernel_unicorn test_kernel_unicorn.cpp unicorn_kernel.cpp)
    target_link_libraries(lgp_test_kernel_unicorn PRIVATE lgp_runtime_core unicorn)
    add_test(NAME runtime.kernel_unicorn COMMAND lgp_test_kernel_unicorn)
    set_tests_properties(runtime.kernel_unicorn PROPERTIES LABELS "runtime;unicorn")
endif()

# lgp_romfs_pack (runtime/hle/tools): packs a small own tree (this project's
# ADRs) into an image and reports the file count.
if(TARGET lgp_romfs_pack)
    add_test(NAME runtime.romfs_pack
             COMMAND lgp_romfs_pack "${PROJECT_SOURCE_DIR}/docs/decisions" "${CMAKE_CURRENT_BINARY_DIR}/romfs_pack_test.bin")
    set_tests_properties(runtime.romfs_pack PROPERTIES LABELS "runtime"
                         PASS_REGULAR_EXPRESSION "[1-9][0-9]* files, [0-9]+ bytes"
                         FIXTURES_SETUP romfs_plain)
    # Compressed image (--compress) from that image and back: the same bytes.
    if(LGP_ZSTD_AVAILABLE)
        set(_rz "${CMAKE_CURRENT_BINARY_DIR}/romfs_pack_test")
        add_test(NAME runtime.romfs_pack_compress
                 COMMAND lgp_romfs_pack --compress=3 --block-size 4 --threads 2 "${_rz}.bin" "${_rz}.lgpz")
        set_tests_properties(runtime.romfs_pack_compress PROPERTIES LABELS "runtime"
                             PASS_REGULAR_EXPRESSION "compressed [0-9]+ bytes"
                             FIXTURES_REQUIRED romfs_plain FIXTURES_SETUP romfs_compressed)
        add_test(NAME runtime.romfs_pack_unpack COMMAND lgp_romfs_pack "${_rz}.lgpz" "${_rz}.unpacked.bin")
        set_tests_properties(runtime.romfs_pack_unpack PROPERTIES LABELS "runtime"
                             FIXTURES_REQUIRED romfs_compressed FIXTURES_SETUP romfs_unpacked)
        add_test(NAME runtime.romfs_pack_roundtrip
                 COMMAND ${CMAKE_COMMAND} -E compare_files "${_rz}.bin" "${_rz}.unpacked.bin")
        set_tests_properties(runtime.romfs_pack_roundtrip PROPERTIES LABELS "runtime"
                             FIXTURES_REQUIRED "romfs_plain;romfs_unpacked")
    endif()
endif()
