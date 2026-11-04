# P3.4 boot program (runtime/boot): starts recompiled module sets the way the
# game will be started. Label "boot".
#
# 1. SDK-shaped pair: own "main" + "sdk" modules (host clang + ld.lld, shared
#    objects packed as NSO, plus an NPDM) that play the Nintendo-SDK start
#    protocol: sdk exports nn::init::Start, nv:: setup and nvnBootstrapLoader,
#    main nnMain; both have a DT_INIT and import from each other. The boot
#    program has to do rtld's part and stop at the first graphics call, or
#    with the NVN-HLE render the frames of tests/nvn/guest/nvn_clear.cpp.
# 2. Homebrew fixtures (tests/fixtures, needs devkitPro): basic and threads
#    through the boot program, compared with the P2.8 Unicorn end states, and
#    the services fixture against the real HLE services (sm, applet, set,
#    time, acc, fsp-srv with RomFS and save data, csrng).
# 3. Native start (docs/analysis/56, stage D): the pair rewritten in memory
#    and run natively by lgp_game_native, with the lifted pair's report. Also
#    in a cross build for AArch64 (qemu-aarch64 as emulator), where everything
#    that needs lgp_lift on the build host is left out.
if(NOT TARGET lgp_boot)
    return()
endif()

find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)
set(_run "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/run_boot.py")

# --- 1. SDK-shaped pair -----------------------------------------------------
find_program(LGP_GUEST_CLANG NAMES clang clang-20 clang-19 clang-18 clang-17 DOC "clang used to build AArch64 test programs")
find_program(LGP_GUEST_LLD NAMES ld.lld ld.lld-20 ld.lld-19 ld.lld-18 ld.lld-17 DOC "ld.lld used to link AArch64 test programs")
if(NOT LGP_GUEST_CLANG OR NOT LGP_GUEST_LLD)
    message(WARNING "tests/boot: clang or ld.lld not found, the SDK-shaped boot tests are not built")
    add_test(NAME boot.toolchain_missing COMMAND "${CMAKE_COMMAND}" -E false)
    set_tests_properties(boot.toolchain_missing PROPERTIES LABELS "boot")
else()
    set(_guest "${CMAKE_CURRENT_SOURCE_DIR}/guest")
    set(_dir "${CMAKE_CURRENT_BINARY_DIR}/sdkpair")
    set(_exefs "${_dir}/exefs")
    set(_cflags --target=aarch64-none-elf -O2 -fPIC -ffreestanding -fno-builtin -nostdlib -fno-exceptions -fno-rtti
                -fvisibility=hidden -funwind-tables -std=c++20)
    set(_ldflags -shared --hash-style=both --eh-frame-hdr -z norelro -z max-page-size=0x1000 --pack-dyn-relocs=none
                 -T "${_guest}/module.ld")
    # main also carries the P4.1 NVN guest program (3 cleared frames).
    set(_nvn_guest "${PROJECT_SOURCE_DIR}/tests/nvn/guest/nvn_clear.cpp")
    set(_extra_main_o "${_dir}/nvn_clear.o")
    set(_extra_sdk_o)
    set(_extra_main_cmd COMMAND "${LGP_GUEST_CLANG}" ${_cflags} -c "${_nvn_guest}" -o "${_dir}/nvn_clear.o")
    set(_extra_sdk_cmd)
    set(_extra_main_dep "${_nvn_guest}")
    set(_extra_sdk_dep)
    set(_nsos)
    foreach(m IN ITEMS main sdk)
        add_custom_command(
            OUTPUT "${_exefs}/${m}" "${_dir}/${m}.elf"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${_exefs}"
            COMMAND "${LGP_GUEST_CLANG}" ${_cflags} -c "${_guest}/module.S" -o "${_dir}/${m}_module.o"
            COMMAND "${LGP_GUEST_CLANG}" ${_cflags} -c "${_guest}/${m}.cpp" -o "${_dir}/${m}.o"
            ${_extra_${m}_cmd}
            COMMAND "${LGP_GUEST_LLD}" ${_ldflags} -init=lgp_${m}_init -soname=${m}
                    "${_dir}/${m}_module.o" "${_dir}/${m}.o" ${_extra_${m}_o} -o "${_dir}/${m}.elf"
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/elf2nso.py" "${_dir}/${m}.elf" "${_exefs}/${m}"
            DEPENDS "${_guest}/module.S" "${_guest}/${m}.cpp" "${_guest}/guest.hpp" "${_guest}/module.ld"
                    "${CMAKE_CURRENT_SOURCE_DIR}/elf2nso.py" ${_extra_${m}_dep}
            COMMENT "Building the boot test module ${m}"
            VERBATIM)
        list(APPEND _nsos "${_exefs}/${m}")
    endforeach()
    add_custom_command(
        OUTPUT "${_exefs}/main.npdm"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/make_npdm.py" "${_exefs}/main.npdm"
                --program-id 0x0100c0ffee000000 --stack 0x40000 --priority 44 --core 0
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/make_npdm.py" "${_exefs}/main"
        VERBATIM)
    if(TARGET lgp_game_native)
        # The pair plus an rtld with 4 KiB segments (its text stays not
        # executable natively) and a stand-in for the multimedia module (its
        # movie:: export keeps it out), in an ExeFS of its own.
        set(_exefs_native "${_dir}/exefs_native")
        file(READ "${_guest}/module.ld" _ld4k)
        string(REPLACE "ALIGN(0x4000)" "ALIGN(0x1000)" _ld4k "${_ld4k}")
        file(WRITE "${_dir}/module_4k.ld" "${_ld4k}")
        string(REPLACE "${_guest}/module.ld" "${_dir}/module_4k.ld" _ldflags4k "${_ldflags}")
        add_custom_command(
            OUTPUT "${_exefs_native}/rtld" "${_exefs_native}/subsdk1" "${_exefs_native}/main" "${_exefs_native}/sdk"
                   "${_exefs_native}/main.npdm"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${_exefs_native}"
            COMMAND "${LGP_GUEST_CLANG}" ${_cflags} -c "${_guest}/module.S" -o "${_dir}/rtld_module.o"
            COMMAND "${LGP_GUEST_CLANG}" ${_cflags} -c "${_guest}/rtld.cpp" -o "${_dir}/rtld.o"
            COMMAND "${LGP_GUEST_LLD}" ${_ldflags4k} -init=lgp_rtld_init -soname=rtld
                    "${_dir}/rtld_module.o" "${_dir}/rtld.o" -o "${_dir}/rtld.elf"
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/elf2nso.py" "${_dir}/rtld.elf"
                    "${_exefs_native}/rtld"
            COMMAND "${LGP_GUEST_CLANG}" ${_cflags} -c "${_guest}/module.S" -o "${_dir}/multimedia_module.o"
            COMMAND "${LGP_GUEST_CLANG}" ${_cflags} -c "${_guest}/multimedia.cpp" -o "${_dir}/multimedia.o"
            COMMAND "${LGP_GUEST_LLD}" ${_ldflags} -init=lgp_multimedia_init -soname=subsdk1
                    "${_dir}/multimedia_module.o" "${_dir}/multimedia.o" -o "${_dir}/multimedia.elf"
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/elf2nso.py" "${_dir}/multimedia.elf"
                    "${_exefs_native}/subsdk1"
            COMMAND "${CMAKE_COMMAND}" -E copy "${_exefs}/main" "${_exefs}/sdk" "${_exefs}/main.npdm" "${_exefs_native}"
            DEPENDS "${_guest}/module.S" "${_guest}/rtld.cpp" "${_guest}/multimedia.cpp" "${_guest}/guest.hpp"
                    "${_guest}/module.ld"
                    "${CMAKE_CURRENT_SOURCE_DIR}/elf2nso.py" "${_exefs}/main.npdm" ${_nsos}
            COMMENT "Building the native boot test ExeFS"
            VERBATIM)
        add_custom_target(lgp_boot_sdkpair_native_data ALL DEPENDS "${_exefs_native}/rtld")
        add_dependencies(lgp_game_native lgp_boot_sdkpair_native_data)
        set(_emu)
        if(CMAKE_CROSSCOMPILING)
            set(_emu ${CMAKE_CROSSCOMPILING_EMULATOR})
        endif()
        # The code image (stage F): written on any host, used where the code
        # runs (the run below, then compared with sdk_native_hle's report).
        set(_native_image "${CMAKE_CURRENT_BINARY_DIR}/native_image/sdkpair.lgpnimg")
        add_test(NAME boot.sdk_native_image_out
                 COMMAND ${_run} --clean "${CMAKE_CURRENT_BINARY_DIR}/native_image"
                         --present "native: image " --present " written, " --absent "FAILED" --absent "start:"
                         -- ${_emu} $<TARGET_FILE:lgp_game_native> --exefs "${_exefs_native}" --graphics hle
                            --native-cache none --native-image-out "${_native_image}")
        set_tests_properties(boot.sdk_native_image_out PROPERTIES LABELS "boot;native" TIMEOUT 120
                             FIXTURES_SETUP lgp_native_image)
    endif()
    if(TARGET lgp_game_native AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
        # macOS refuses executable mappings of unsigned files: there the boot
        # checks the pages against the image and makes them executable itself.
        if(APPLE)
            set(_image_line "native: code checked against the image and made executable (no file mapping), ")
        else()
            set(_image_line "native: code mapped from the image, ")
        endif()
        add_test(NAME boot.sdk_native_image
                 COMMAND ${_run} --lines "${CMAKE_CURRENT_SOURCE_DIR}/expected/sdk_graphics_hle.txt"
                         --present "${_image_line}" --absent "FAILED" --absent "sdk: driver"
                         --absent "graphics stop:"
                         -- ${_emu} $<TARGET_FILE:lgp_game_native> --exefs "${_exefs_native}" --graphics hle
                            --native-cache none --native-image "${_native_image}")
        set_tests_properties(boot.sdk_native_image PROPERTIES LABELS "boot;native" TIMEOUT 120
                             FIXTURES_REQUIRED lgp_native_image)
        # The image holds the host-call stubs of its run: without --graphics
        # hle there are fewer, so the boot refuses it.
        add_test(NAME boot.sdk_native_image_mismatch
                 COMMAND ${_emu} $<TARGET_FILE:lgp_game_native> --exefs "${_exefs_native}" --native-cache none
                         --native-image "${_native_image}")
        set_tests_properties(boot.sdk_native_image_mismatch PROPERTIES LABELS "boot;native" TIMEOUT 120
                             FIXTURES_REQUIRED lgp_native_image
                             PASS_REGULAR_EXPRESSION "native image .*: image differs from this start at 0x")
        add_test(NAME boot.sdk_native_stop
                 COMMAND ${_run} --lines "${CMAKE_CURRENT_SOURCE_DIR}/expected/sdk_graphics_stop.txt"
                         --present "native: rewrite " --absent "FAILED" --absent "sdk: driver"
                         --present "; 1 modules not executable; cache off"
                         --present "native: subsdk1 left out, the movie HLE replaces it"
                         -- ${_emu} $<TARGET_FILE:lgp_game_native> --exefs "${_exefs_native}" --native-cache none)
        add_test(NAME boot.sdk_native_hle
                 COMMAND ${_run} --lines "${CMAKE_CURRENT_SOURCE_DIR}/expected/sdk_graphics_hle.txt"
                         --present "native: rewrite " --absent "FAILED" --absent "sdk: driver"
                         --absent "graphics stop:"
                         -- ${_emu} $<TARGET_FILE:lgp_game_native> --exefs "${_exefs_native}" --graphics hle
                            --native-cache none)
        set_tests_properties(boot.sdk_native_stop boot.sdk_native_hle PROPERTIES LABELS "boot;native" TIMEOUT 120)
        # The rewrite cache: the first run writes it, the second takes the
        # rewrite from it and ends the same way.
        set(_native_cache_dir "${CMAKE_CURRENT_BINARY_DIR}/native_cache")
        add_test(NAME boot.sdk_native_cache_miss
                 COMMAND ${_run} --lines "${CMAKE_CURRENT_SOURCE_DIR}/expected/sdk_graphics_stop.txt"
                         --clean "${_native_cache_dir}" --present "; cache miss"
                         --absent "FAILED"
                         -- ${_emu} $<TARGET_FILE:lgp_game_native> --exefs "${_exefs_native}"
                            --native-cache "${_native_cache_dir}/rewrite.lgpnative")
        add_test(NAME boot.sdk_native_cache_hit
                 COMMAND ${_run} --lines "${CMAKE_CURRENT_SOURCE_DIR}/expected/sdk_graphics_stop.txt"
                         --present "; cache hit" --absent "FAILED"
                         -- ${_emu} $<TARGET_FILE:lgp_game_native> --exefs "${_exefs_native}"
                            --native-cache "${_native_cache_dir}/rewrite.lgpnative")
        set_tests_properties(boot.sdk_native_cache_miss PROPERTIES LABELS "boot;native" TIMEOUT 120
                             FIXTURES_SETUP lgp_native_cache)
        set_tests_properties(boot.sdk_native_cache_hit PROPERTIES LABELS "boot;native" TIMEOUT 120
                             FIXTURES_REQUIRED lgp_native_cache)
    endif()
    if(NOT CMAKE_CROSSCOMPILING)
    # Split into three translation units like the game (lgp_lift --split).
    # nnMain gets a hook door (game mods, P8): unused, it must change nothing;
    # boot.sdk_hook_log logs through it.
    set(_lifted "${_dir}/sdkpair_lifted.cpp" "${_dir}/sdkpair_lifted.1.cpp" "${_dir}/sdkpair_lifted.2.cpp")
    add_custom_command(
        OUTPUT ${_lifted}
        COMMAND lgp_lift ${_nsos} -o "${_dir}/sdkpair_lifted.cpp" --split 3 --summary --hook nnMain
        DEPENDS lgp_lift ${_nsos}
        COMMENT "Recompiling the boot test pair"
        VERBATIM)
    lgp_add_boot_executable(lgp_boot_sdkpair ${_lifted})
    add_custom_target(lgp_boot_sdkpair_data ALL DEPENDS "${_exefs}/main.npdm")
    add_dependencies(lgp_boot_sdkpair lgp_boot_sdkpair_data)

    add_test(NAME boot.sdk_graphics_stop
             COMMAND ${_run} --lines "${CMAKE_CURRENT_SOURCE_DIR}/expected/sdk_graphics_stop.txt"
                     --absent "FAILED" --absent "sdk: driver" --clean "${_dir}/save_stop"
                     -- $<TARGET_FILE:lgp_boot_sdkpair> --exefs "${_exefs}" --save "${_dir}/save_stop")
    add_test(NAME boot.sdk_graphics_continue
             COMMAND ${_run} --lines "${CMAKE_CURRENT_SOURCE_DIR}/expected/sdk_graphics_continue.txt"
                     --absent "FAILED" --absent "graphics stop:"
                     -- $<TARGET_FILE:lgp_boot_sdkpair> --exefs "${_exefs}" --graphics continue)
    # NVN-HLE: the nv:: setup is skipped, the HLE answers nvnBootstrapLoader,
    # main renders 3 frames and returns; with --frames 2 the boot ends the
    # process at the second present.
    add_test(NAME boot.sdk_graphics_hle
             COMMAND ${_run} --lines "${CMAKE_CURRENT_SOURCE_DIR}/expected/sdk_graphics_hle.txt"
                     --absent "FAILED" --absent "sdk: driver" --absent "graphics stop:"
                     -- $<TARGET_FILE:lgp_boot_sdkpair> --exefs "${_exefs}" --graphics hle)
    add_test(NAME boot.sdk_graphics_hle_frames
             COMMAND ${_run} --lines "${CMAKE_CURRENT_SOURCE_DIR}/expected/sdk_graphics_hle_frames.txt"
                     --absent "FAILED" --absent "sdk: driver" --absent "nnMain returned"
                     -- $<TARGET_FILE:lgp_boot_sdkpair> --exefs "${_exefs}" --graphics hle --frames 2)
    # --time-stats: the per-window lines, among them the GPU waits the guest
    # asked for (nvnQueueFinish/nvnSyncWait) and the process's system calls.
    add_test(NAME boot.sdk_time_stats_lines
             COMMAND ${_run} --absent "FAILED" --present "nvn waits, frames 1-1: queue finish "
                     --present ", csw "
                     -- $<TARGET_FILE:lgp_boot_sdkpair> --exefs "${_exefs}" --graphics hle --time-stats 1)
    set_tests_properties(boot.sdk_time_stats_lines PROPERTIES LABELS "boot;nvn" TIMEOUT 120)
    # --press with frame, time, event and stable-image triggers is accepted
    # (lgp/boot/press_script.hpp, logic in boot.press_script); a malformed one is
    # a usage error, and so is a stable trigger without --graphics hle.
    set(_press "1:a,0.05s:b:0.1s,movie-end+1s:x,movie-start#2+30:lup")
    string(APPEND _press ",stable:500ms:a,movie-end+after-stable=200ms:b")
    add_test(NAME boot.sdk_press_script
             COMMAND ${_run} --lines "${CMAKE_CURRENT_SOURCE_DIR}/expected/sdk_press_script.txt"
                     -- $<TARGET_FILE:lgp_boot_sdkpair> --exefs "${_exefs}" --graphics hle --frames 2
                     --press "${_press}")
    add_test(NAME boot.sdk_press_stable_stop
             COMMAND ${_run} --exit-code 2 -- $<TARGET_FILE:lgp_boot_sdkpair> --exefs "${_exefs}" --press "stable:1s:a")
    set_tests_properties(boot.sdk_press_stable_stop PROPERTIES
                         PASS_REGULAR_EXPRESSION "stable and \\+after-stable need --graphics hle")
    # --clock virtual: guest time follows the run, not the host, so the presses
    # fire at the same frame and guest time on every run (presents end at the
    # next 60 Hz vsync: frame 2 is presented at 33.3 ms).
    add_test(NAME boot.sdk_clock_virtual
             COMMAND ${_run} --lines "${CMAKE_CURRENT_SOURCE_DIR}/expected/sdk_clock_virtual.txt"
                     --absent "FAILED" --absent "sdk: driver"
                     -- $<TARGET_FILE:lgp_boot_sdkpair> --exefs "${_exefs}" --graphics hle --clock virtual
                     --press "2:a,0.02s:b")
    # LGP_VCLOCK_LOG: the guest time each frame used before its present.
    add_test(NAME boot.sdk_clock_virtual_log
             COMMAND lgp_boot_sdkpair --exefs "${_exefs}" --graphics hle --clock virtual)
    set_tests_properties(boot.sdk_clock_virtual_log PROPERTIES LABELS "boot;runtime" TIMEOUT 120
                         ENVIRONMENT "LGP_VCLOCK_LOG=2:1"
                         PASS_REGULAR_EXPRESSION "vclock: present 2 at [0-9]+ ns, [0-9]+ ns after vsync 1 \\(next 2\\)"
                         FAIL_REGULAR_EXPRESSION "vclock: present [13] ")
    # Value patches of the boot program (LGP_POKE / LGP_PATCH_IMM, lgp/runtime/patch.hpp):
    # applied after loading, before the kernel runs (f:A/B values included),
    # also with an input script and the virtual clock (the PC's measuring runs).
    add_test(NAME boot.sdk_value_patches COMMAND lgp_boot_sdkpair --exefs "${_exefs}")
    add_test(NAME boot.sdk_value_patches_press
             COMMAND lgp_boot_sdkpair --exefs "${_exefs}" --graphics hle --clock virtual --press "2:a,0.02s:b")
    set_tests_properties(boot.sdk_value_patches boot.sdk_value_patches_press PROPERTIES LABELS "boot;runtime;patch" TIMEOUT 120
                         ENVIRONMENT "LGP_POKE=0x8000000=f:21/9;LGP_PATCH_IMM=0x8000000=1"
                         PASS_REGULAR_EXPRESSION "LGP_POKE: 0x8000000 = 0x40155555"
                         FAIL_REGULAR_EXPRESSION "without an active kernel;LGP_PATCH_IMM: 0x8000000 = ")
    # LGP_POKE_FRAME: a value written again after every present.
    add_test(NAME boot.sdk_poke_frame COMMAND lgp_boot_sdkpair --exefs "${_exefs}" --graphics hle)
    set_tests_properties(boot.sdk_poke_frame PROPERTIES LABELS "boot;runtime;patch" TIMEOUT 120
                         ENVIRONMENT "LGP_POKE_FRAME=0x8000000=f:21/9"
                         PASS_REGULAR_EXPRESSION "LGP_POKE_FRAME: 0x8000000 = 0x40155555 after every present"
                         FAIL_REGULAR_EXPRESSION "LGP_POKE_FRAME: 0x8000000: ")
    # LGP_NVN_SIZE_ORIGINS: sizes of textures/scissors/viewports/crop with the guest return address.
    add_test(NAME boot.sdk_nvn_size_origins COMMAND lgp_boot_sdkpair --exefs "${_exefs}" --graphics hle)
    set_tests_properties(boot.sdk_nvn_size_origins PROPERTIES LABELS "boot;runtime;nvn" TIMEOUT 120
                         ENVIRONMENT "LGP_NVN_SIZE_ORIGINS=1"
                         PASS_REGULAR_EXPRESSION "nvn size: texture 0,0 64x32 from 0x[0-9a-f]+")
    add_test(NAME boot.sdk_press_bad
             COMMAND ${_run} --exit-code 2 -- $<TARGET_FILE:lgp_boot_sdkpair> --exefs "${_exefs}" --press "1x:a")
    set_tests_properties(boot.sdk_press_bad PROPERTIES PASS_REGULAR_EXPRESSION "--press: bad trigger 1x")
    # Present pacing: the test program presents 3 frames with present
    # interval 1, so --paced waits for the 60 Hz display between them
    # (lgp/boot/frame_pacer.hpp, logic in boot.frame_pacer); without a window
    # the default is unthrottled.
    add_test(NAME boot.sdk_paced
             COMMAND ${_run} --lines "${CMAKE_CURRENT_SOURCE_DIR}/expected/sdk_paced.txt" --absent "FAILED"
                     -- $<TARGET_FILE:lgp_boot_sdkpair> --exefs "${_exefs}" --graphics hle --paced)
    set_tests_properties(boot.sdk_paced PROPERTIES FAIL_REGULAR_EXPRESSION "present: waited [0-9] ms")
    add_test(NAME boot.sdk_unthrottled
             COMMAND ${_run} --lines "${CMAKE_CURRENT_SOURCE_DIR}/expected/sdk_unthrottled.txt" --absent "present: waited"
                     -- $<TARGET_FILE:lgp_boot_sdkpair> --exefs "${_exefs}" --graphics hle)
    # App mode (P7.1, lgp/boot/app.hpp): the same pair as an app. --app takes
    # the game folder from settings.ini below LGP_APP_DIR; the options after
    # --app come last and replace the window (no display in CI).
    lgp_add_boot_executable(lgp_boot_sdkpair_app ${_lifted} APP)
    add_dependencies(lgp_boot_sdkpair_app lgp_boot_sdkpair_data)
    set(_app "${_dir}/app")
    file(MAKE_DIRECTORY "${_dir}/romfs")
    file(WRITE "${_app}/settings.ini" "game = ${_dir}\nargs = --renderer soft --input none --audio none\n")
    add_test(NAME boot.sdk_app
             COMMAND ${_run} --lines "${CMAKE_CURRENT_SOURCE_DIR}/expected/sdk_app.txt"
                     --absent "FAILED" --absent "sdk: driver" --absent "pipeline cache: off"
                     -- $<TARGET_FILE:lgp_boot_sdkpair_app> --app --frames 2)
    set_tests_properties(boot.sdk_app PROPERTIES ENVIRONMENT "LGP_APP_DIR=${_app}")
    # The app as built for iOS/Android (no console, working directory "/"),
    # started with plain options: output in <app folder>/logs/lgp.log, relative
    # paths below the app folder (here exefs/ of the pair), exit code logged.
    lgp_add_boot_executable(lgp_boot_sdkpair_hosted ${_lifted} APP)
    target_compile_definitions(lgp_boot_sdkpair_hosted PRIVATE LGP_BOOT_APP_LOG_ALWAYS=1)
    add_dependencies(lgp_boot_sdkpair_hosted lgp_boot_sdkpair_data)
    add_test(NAME boot.sdk_app_hosted_args
             COMMAND ${_run} --lines "${CMAKE_CURRENT_SOURCE_DIR}/expected/sdk_app_hosted_args.txt"
                     --absent "FAILED" --absent "sdk: driver" --output "${_dir}/logs/lgp.log" --cwd "${_dir}/romfs"
                     -- $<TARGET_FILE:lgp_boot_sdkpair_hosted> --exefs exefs --graphics hle --frames 2)
    set_tests_properties(boot.sdk_app_hosted_args PROPERTIES ENVIRONMENT "LGP_APP_DIR=${_dir}"
                         RESOURCE_LOCK lgp_sdkpair_hosted_log)
    # The same build started with --app (Android: am start -e args "--app …"):
    # the log ends with the exit code, which tools/android/shield_day.py waits for.
    add_test(NAME boot.sdk_app_hosted_app
             COMMAND ${_run} --lines "${CMAKE_CURRENT_SOURCE_DIR}/expected/sdk_app_hosted_app.txt"
                     --absent "FAILED" --output "${_app}/logs/lgp.log"
                     -- $<TARGET_FILE:lgp_boot_sdkpair_hosted> --app --frames 2)
    set_tests_properties(boot.sdk_app_hosted_app PROPERTIES ENVIRONMENT "LGP_APP_DIR=${_app}"
                         RESOURCE_LOCK lgp_sdkpair_app_dir)
    set_tests_properties(boot.sdk_app PROPERTIES RESOURCE_LOCK lgp_sdkpair_app_dir)
    add_test(NAME boot.sdk_app_hosted_args_error
             COMMAND ${_run} --exit-code 2 --lines "${CMAKE_CURRENT_SOURCE_DIR}/expected/sdk_app_hosted_args_error.txt"
                     --output "${_dir}/logs/lgp.log"
                     -- $<TARGET_FILE:lgp_boot_sdkpair_hosted> --exefs missing --press "1x:a")
    set_tests_properties(boot.sdk_app_hosted_args_error PROPERTIES ENVIRONMENT "LGP_APP_DIR=${_dir}"
                         RESOURCE_LOCK lgp_sdkpair_hosted_log)
    # The app's menus (P7, lgp/platform/ui/app_menu.hpp) in a real window:
    # start screen, pause with a backup of the save slot, resume. Only where
    # xvfb-run provides a display (Linux CI, lavapipe). The pause comes at
    # frame 0, before the game starts: the game ends after 3 frames, a later
    # pause depended on the host loop's timing.
    if(TARGET lgp_platform_ui)
        find_program(LGP_XVFB_RUN NAMES xvfb-run DOC "xvfb-run: windows without a display")
        if(LGP_XVFB_RUN)
            set(_app_menu "${_dir}/app-menu")
            file(MAKE_DIRECTORY "${_app_menu}")
            file(WRITE "${_app_menu}/settings.ini" "game = ${_dir}\nargs = --audio none --input none\n")
            add_test(NAME boot.sdk_app_menu
                     COMMAND ${_run} --lines "${CMAKE_CURRENT_SOURCE_DIR}/expected/sdk_app_menu.txt"
                             --absent "FAILED" --absent "no menus" --clean "${_app_menu}/backups"
                             -- "${LGP_XVFB_RUN}" -a -s "-screen 0 1280x720x24"
                             $<TARGET_FILE:lgp_boot_sdkpair_app> --app --menu-script play,pause@0,backup,resume@20)
            set_tests_properties(boot.sdk_app_menu PROPERTIES ENVIRONMENT "LGP_APP_DIR=${_app_menu}"
                                 LABELS "boot;runtime;webgpu" TIMEOUT 120)
            # The same with asynchronous backend execution (LGP_WEBGPU_ASYNC_EXECUTE,
            # the default on macOS): backup and resume with the queue (pause@0 as above;
            # the pause comes before the game, so the queue is empty when it is taken).
            set(_app_async "${_dir}/app-menu-async")
            file(MAKE_DIRECTORY "${_app_async}")
            file(WRITE "${_app_async}/settings.ini" "game = ${_dir}\nargs = --audio none --input none\n")
            add_test(NAME boot.sdk_app_menu_async
                     COMMAND ${_run} --lines "${CMAKE_CURRENT_SOURCE_DIR}/expected/sdk_app_menu_async.txt"
                             --absent "FAILED" --absent "no menus" --clean "${_app_async}/backups"
                             -- "${LGP_XVFB_RUN}" -a -s "-screen 0 1280x720x24"
                             $<TARGET_FILE:lgp_boot_sdkpair_app> --app --menu-script play,pause@0,backup,resume@20)
            set_tests_properties(boot.sdk_app_menu_async PROPERTIES
                                 ENVIRONMENT "LGP_APP_DIR=${_app_async};LGP_WEBGPU_ASYNC_EXECUTE=1"
                                 LABELS "boot;runtime;webgpu" TIMEOUT 120)
            # Pause in the middle of the game: pause@2 holds the game at its second
            # present until the menu took the pause (MenuGate in main.cpp), so the
            # third frame comes only after backup and resume, however the host loop
            # is timed. Synchronous and with asynchronous execution (the macOS
            # default): there the pause also drains what the guest queued.
            foreach(_mode IN ITEMS sync async)
                set(_app_mid "${_dir}/app-menu-midgame-${_mode}")
                file(MAKE_DIRECTORY "${_app_mid}")
                file(WRITE "${_app_mid}/settings.ini" "game = ${_dir}\nargs = --audio none --input none\n")
                if(_mode STREQUAL "async")
                    set(_name boot.sdk_app_menu_midgame_async)
                    set(_async 1)
                else()
                    set(_name boot.sdk_app_menu_midgame)
                    set(_async 0)
                endif()
                add_test(NAME ${_name}
                         COMMAND ${_run} --lines "${CMAKE_CURRENT_SOURCE_DIR}/expected/sdk_app_menu_midgame_${_mode}.txt"
                                 --absent "FAILED" --absent "no menus" --absent "did not stop"
                                 --clean "${_app_mid}/backups"
                                 -- "${LGP_XVFB_RUN}" -a -s "-screen 0 1280x720x24"
                                 $<TARGET_FILE:lgp_boot_sdkpair_app> --app --menu-script play,pause@2,backup,resume@20)
                set_tests_properties(${_name} PROPERTIES
                                     ENVIRONMENT "LGP_APP_DIR=${_app_mid};LGP_WEBGPU_ASYNC_EXECUTE=${_async}"
                                     LABELS "boot;runtime;webgpu" TIMEOUT 120)
            endforeach()
            # Without the start screen (settings.ini start_screen = 0, the
            # default): the game starts at once with the preselected save slot.
            set(_app_nostart "${_dir}/app-menu-no-start-screen")
            file(MAKE_DIRECTORY "${_app_nostart}")
            file(WRITE "${_app_nostart}/settings.ini" "game = ${_dir}\nargs = --audio none --input none\n")
            add_test(NAME boot.sdk_app_no_start_screen
                     COMMAND ${_run} --lines "${CMAKE_CURRENT_SOURCE_DIR}/expected/sdk_app_no_start_screen.txt"
                             --absent "FAILED" --absent "no menus" --absent "menu: play"
                             -- "${LGP_XVFB_RUN}" -a -s "-screen 0 1280x720x24"
                             $<TARGET_FILE:lgp_boot_sdkpair_app> --app)
            set_tests_properties(boot.sdk_app_no_start_screen PROPERTIES ENVIRONMENT "LGP_APP_DIR=${_app_nostart}"
                                 LABELS "boot;runtime;webgpu" TIMEOUT 120)
            # The app's Pokémon Gelb (random-encounters) also without menus (--menu
            # off, as in scripted runs): Gelb's rate and no visible field Pokémon
            # reach the mod; the rows of older builds in settings.ini are dropped.
            set(_app_encounters "${_dir}/app-encounters")
            file(MAKE_DIRECTORY "${_app_encounters}")
            file(WRITE "${_app_encounters}/settings.ini"
                 "game = ${_dir}\nencounter_rate = gen1\nvisible_pokemon = 2\nargs = --audio none --input none\n")
            add_test(NAME boot.sdk_app_encounters_no_menu
                     COMMAND ${_run} --lines "${CMAKE_CURRENT_SOURCE_DIR}/expected/sdk_app_encounters_no_menu.txt"
                             --absent "FAILED"
                             -- "${LGP_XVFB_RUN}" -a -s "-screen 0 1280x720x24"
                             $<TARGET_FILE:lgp_boot_sdkpair_app> --app --menu off)
            set_tests_properties(boot.sdk_app_encounters_no_menu PROPERTIES ENVIRONMENT "LGP_APP_DIR=${_app_encounters}"
                                 LABELS "boot;runtime;webgpu" TIMEOUT 120)
            # "Hide field Pokémon" with menus (PC 02.11.: app_arguments names
            # random-encounters for it, and the menu took that for Pokémon Gelb
            # on): hiding only reaches the mod. A game folder of its own with the
            # mod file, so the other runs do not list the mod.
            set(_game_hide "${_dir}/game-hide")
            file(MAKE_DIRECTORY "${_game_hide}/mods" "${_game_hide}/romfs")
            file(CREATE_LINK "${_exefs}" "${_game_hide}/exefs" SYMBOLIC)
            file(WRITE "${_game_hide}/mods/random-encounters.cfg" "# test: no roles, the mod does not start\n")
            set(_app_hide "${_dir}/app-hide")
            file(MAKE_DIRECTORY "${_app_hide}")
            file(WRITE "${_app_hide}/settings.ini"
                 "game = ${_game_hide}\nhide_field_pokemon = 1\nargs = --audio none --input none\n")
            add_test(NAME boot.sdk_app_hide_field_pokemon
                     COMMAND ${_run} --lines "${CMAKE_CURRENT_SOURCE_DIR}/expected/sdk_app_hide_field_pokemon.txt"
                             --absent "FAILED" --absent "app: Pokémon Gelb at"
                             -- "${LGP_XVFB_RUN}" -a -s "-screen 0 1280x720x24"
                             $<TARGET_FILE:lgp_boot_sdkpair_app> --app)
            set_tests_properties(boot.sdk_app_hide_field_pokemon PROPERTIES ENVIRONMENT "LGP_APP_DIR=${_app_hide}"
                                 LABELS "boot;runtime;webgpu" TIMEOUT 120)
            # Player 1's controller switched while the game runs (settings row
            # "Controller"): SdlInput maps slot 0 with the new style at once.
            # Ending handheld leaves settings.ini as written here for the next run.
            set(_app_ctrl "${_dir}/app-menu-controller")
            file(MAKE_DIRECTORY "${_app_ctrl}")
            file(WRITE "${_app_ctrl}/settings.ini" "game = ${_dir}\ncontroller = handheld\nargs = --audio none --input sdl\n")
            add_test(NAME boot.sdk_app_menu_controller
                     COMMAND ${_run} --lines "${CMAKE_CURRENT_SOURCE_DIR}/expected/sdk_app_menu_controller.txt"
                             --absent "FAILED" --absent "no menus"
                             -- "${LGP_XVFB_RUN}" -a -s "-screen 0 1280x720x24"
                             $<TARGET_FILE:lgp_boot_sdkpair_app> --app --menu-script play,controller:joycon-right@1,controller:handheld@2)
            set_tests_properties(boot.sdk_app_menu_controller PROPERTIES
                                 ENVIRONMENT "LGP_APP_DIR=${_app_ctrl};SDL_AUDIO_DRIVER=dummy"
                                 LABELS "boot;runtime;webgpu" TIMEOUT 120)
            # The online-button file (here in --mods-dir, which app_arguments
            # does not look at): the start from the menu takes both links and
            # adds the hidden mod itself, also over an ldn_online = 0 of older
            # builds (online is always on now). The server is never contacted
            # (the pair uses no ldn).
            set(_app_online "${_dir}/app-menu-online")
            file(MAKE_DIRECTORY "${_app_online}/mods")
            file(WRITE "${_app_online}/mods/online-button.cfg" "patch far_state 0x1000\npatch far_flag 0x1004\n")
            file(WRITE "${_app_online}/settings.ini"
                 "game = ${_dir}\nldn_online = 0\nldn_server = 127.0.0.1:9\n"
                 "args = --audio none --input none --mods-dir ${_app_online}/mods\n")
            add_test(NAME boot.sdk_app_menu_online
                     COMMAND ${_run} --lines "${CMAKE_CURRENT_SOURCE_DIR}/expected/sdk_app_menu_online.txt"
                             --absent "FAILED" --absent "no menus"
                             -- "${LGP_XVFB_RUN}" -a -s "-screen 0 1280x720x24"
                             $<TARGET_FILE:lgp_boot_sdkpair_app> --app --menu-script play)
            set_tests_properties(boot.sdk_app_menu_online PROPERTIES ENVIRONMENT "LGP_APP_DIR=${_app_online}"
                                 LABELS "boot;runtime;webgpu;mods" TIMEOUT 120)
            # An --ldn off after --app wins over the server in the menu's start too.
            add_test(NAME boot.sdk_app_menu_online_off
                     COMMAND ${_run} --lines "${CMAKE_CURRENT_SOURCE_DIR}/expected/sdk_app_menu_online_off.txt"
                             --absent "FAILED" --absent "no menus" --absent "app: ldn both"
                             --absent "mods: on online-button"
                             -- "${LGP_XVFB_RUN}" -a -s "-screen 0 1280x720x24"
                             $<TARGET_FILE:lgp_boot_sdkpair_app> --app --ldn off --menu-script play)
            set_tests_properties(boot.sdk_app_menu_online_off PROPERTIES ENVIRONMENT "LGP_APP_DIR=${_app_online}"
                                 LABELS "boot;runtime;webgpu;mods" TIMEOUT 120)
            # TV mode switched while the game runs (the settings' "TV-Modus"):
            # docked@1 and handheld@2 hold the game at that present until the
            # host changed the setting, so each switch reaches the running
            # process (am OperationModeChanged, ProcessControl::set_docked).
            # Ending handheld leaves settings.ini as written here for the next run.
            set(_app_docked "${_dir}/app-menu-docked")
            file(MAKE_DIRECTORY "${_app_docked}")
            file(WRITE "${_app_docked}/settings.ini" "game = ${_dir}\ndocked = 0\nargs = --audio none --input none\n")
            add_test(NAME boot.sdk_app_menu_docked
                     COMMAND ${_run} --lines "${CMAKE_CURRENT_SOURCE_DIR}/expected/sdk_app_menu_docked.txt"
                             --absent "FAILED" --absent "no menus"
                             -- "${LGP_XVFB_RUN}" -a -s "-screen 0 1280x720x24"
                             $<TARGET_FILE:lgp_boot_sdkpair_app> --app --menu-script play,docked@1,handheld@2)
            set_tests_properties(boot.sdk_app_menu_docked PROPERTIES ENVIRONMENT "LGP_APP_DIR=${_app_docked}"
                                 LABELS "boot;runtime;webgpu" TIMEOUT 120)
            # "quit" on the start screen (settings open, game not started): the
            # app ends there; the start screen used to ignore the pause menu's
            # QuitGame and ran until it was killed.
            set(_app_quit "${_dir}/app-menu-quit")
            file(MAKE_DIRECTORY "${_app_quit}")
            file(WRITE "${_app_quit}/settings.ini" "game = ${_dir}\nargs = --audio none --input none\n")
            add_test(NAME boot.sdk_app_menu_quit
                     COMMAND ${_run} --lines "${CMAKE_CURRENT_SOURCE_DIR}/expected/sdk_app_menu_quit.txt"
                             --absent "FAILED" --absent "no menus" --absent "start: sdk"
                             -- "${LGP_XVFB_RUN}" -a -s "-screen 0 1280x720x24"
                             $<TARGET_FILE:lgp_boot_sdkpair_app> --app --menu-script settings@30,quit@400)
            set_tests_properties(boot.sdk_app_menu_quit PROPERTIES ENVIRONMENT "LGP_APP_DIR=${_app_quit}"
                                 LABELS "boot;runtime;webgpu" TIMEOUT 60)
            # Picture format from settings.ini (aspect = fill -> --aspect fill): the
            # game runs to its end with the image cut to fill the window.
            set(_app_aspect "${_dir}/app-aspect")
            file(MAKE_DIRECTORY "${_app_aspect}")
            file(WRITE "${_app_aspect}/settings.ini"
                 "game = ${_dir}\naspect = fill\nargs = --audio none --input none\n")
            add_test(NAME boot.sdk_app_aspect
                     COMMAND ${_run} --lines "${CMAKE_CURRENT_SOURCE_DIR}/expected/sdk_app_aspect.txt"
                             --absent "FAILED" --absent "no menus"
                             -- "${LGP_XVFB_RUN}" -a -s "-screen 0 1280x720x24"
                             $<TARGET_FILE:lgp_boot_sdkpair_app> --app --menu-script play)
            set_tests_properties(boot.sdk_app_aspect PROPERTIES ENVIRONMENT "LGP_APP_DIR=${_app_aspect}"
                                 LABELS "boot;runtime;webgpu" TIMEOUT 120)
            # Light effects at half resolution from settings.ini (effects = half ->
            # --effects half): the backend gets the list, the game runs to its end.
            set(_app_effects "${_dir}/app-effects")
            file(MAKE_DIRECTORY "${_app_effects}")
            file(WRITE "${_app_effects}/settings.ini"
                 "game = ${_dir}\neffects = half\nargs = --audio none --input none\n")
            add_test(NAME boot.sdk_app_effects
                     COMMAND ${_run} --lines "${CMAKE_CURRENT_SOURCE_DIR}/expected/sdk_app_effects.txt"
                             --absent "FAILED" --absent "no menus"
                             -- "${LGP_XVFB_RUN}" -a -s "-screen 0 1280x720x24"
                             $<TARGET_FILE:lgp_boot_sdkpair_app> --app --menu-script play)
            set_tests_properties(boot.sdk_app_effects PROPERTIES ENVIRONMENT "LGP_APP_DIR=${_app_effects}"
                                 LABELS "boot;runtime;webgpu" TIMEOUT 120)
            # Render scale from settings.ini (render_scale = 0.75 -> --render-scale
            # 0.75): the backend scales its render targets, the game runs to its end.
            set(_app_scale "${_dir}/app-render-scale")
            file(MAKE_DIRECTORY "${_app_scale}")
            file(WRITE "${_app_scale}/settings.ini"
                 "game = ${_dir}\nrender_scale = 0.75\nargs = --audio none --input none\n")
            add_test(NAME boot.sdk_app_render_scale
                     COMMAND ${_run} --lines "${CMAKE_CURRENT_SOURCE_DIR}/expected/sdk_app_render_scale.txt"
                             --absent "FAILED" --absent "no menus" --absent "render scale 0.75 ignored"
                             -- "${LGP_XVFB_RUN}" -a -s "-screen 0 1280x720x24"
                             $<TARGET_FILE:lgp_boot_sdkpair_app> --app --menu-script play)
            set_tests_properties(boot.sdk_app_render_scale PROPERTIES ENVIRONMENT "LGP_APP_DIR=${_app_scale}"
                                 LABELS "boot;runtime;webgpu" TIMEOUT 120)
            # Smoothing from settings.ini (smoothing = fxaa -> --smoothing fxaa): the
            # window renderer scales the image with FXAA, without WebGPU errors.
            set(_app_smooth "${_dir}/app-smoothing")
            file(MAKE_DIRECTORY "${_app_smooth}")
            file(WRITE "${_app_smooth}/settings.ini"
                 "game = ${_dir}\nsmoothing = fxaa\nargs = --audio none --input none\n")
            add_test(NAME boot.sdk_app_smoothing
                     COMMAND ${_run} --lines "${CMAKE_CURRENT_SOURCE_DIR}/expected/sdk_app_smoothing.txt"
                             --absent "FAILED" --absent "no menus" --absent "WebGPU errors"
                             -- "${LGP_XVFB_RUN}" -a -s "-screen 0 1280x720x24"
                             $<TARGET_FILE:lgp_boot_sdkpair_app> --app --menu-script play)
            set_tests_properties(boot.sdk_app_smoothing PROPERTIES ENVIRONMENT "LGP_APP_DIR=${_app_smooth}"
                                 LABELS "boot;runtime;webgpu" TIMEOUT 120)
            # A save folder that is no slot (saves = elsewhere) stays in use.
            set(_app_own "${_dir}/app-menu-own")
            file(MAKE_DIRECTORY "${_app_own}/own-saves")
            file(WRITE "${_app_own}/settings.ini"
                 "game = ${_dir}\nsaves = ${_app_own}/own-saves\nargs = --audio none --input none\n")
            add_test(NAME boot.sdk_app_menu_own_save
                     COMMAND ${_run} --lines "${CMAKE_CURRENT_SOURCE_DIR}/expected/sdk_app_menu_own_save.txt"
                             --absent "FAILED" --absent "app: save slot"
                             -- "${LGP_XVFB_RUN}" -a -s "-screen 0 1280x720x24"
                             $<TARGET_FILE:lgp_boot_sdkpair_app> --app --menu-script play)
            set_tests_properties(boot.sdk_app_menu_own_save PROPERTIES ENVIRONMENT "LGP_APP_DIR=${_app_own}"
                                 LABELS "boot;runtime;webgpu" TIMEOUT 120)
            # --save naming a slot folder starts with that slot preselected.
            set(_app_s2 "${_dir}/app-menu-slot2")
            file(MAKE_DIRECTORY "${_app_s2}/saves" "${_app_s2}/saves-2")
            # A fresh settings.ini for every run (the run below rewrites it).
            file(WRITE "${_dir}/app-menu-slot2.ini" "game = ${_dir}\nargs = --audio none --input none\n")
            add_test(NAME boot.sdk_app_menu_save_option_reset
                     COMMAND "${CMAKE_COMMAND}" -E copy "${_dir}/app-menu-slot2.ini" "${_app_s2}/settings.ini")
            set_tests_properties(boot.sdk_app_menu_save_option_reset PROPERTIES LABELS "boot;runtime"
                                 FIXTURES_SETUP app_menu_save_option_ini)
            add_test(NAME boot.sdk_app_menu_save_option
                     COMMAND ${_run} --lines "${CMAKE_CURRENT_SOURCE_DIR}/expected/sdk_app_menu_save_option.txt"
                             --absent "FAILED" --absent "app: save slot 1"
                             -- "${LGP_XVFB_RUN}" -a -s "-screen 0 1280x720x24"
                             $<TARGET_FILE:lgp_boot_sdkpair_app> --app --save "${_app_s2}/saves-2" --menu-script play)
            set_tests_properties(boot.sdk_app_menu_save_option PROPERTIES ENVIRONMENT "LGP_APP_DIR=${_app_s2}"
                                 LABELS "boot;runtime;webgpu" TIMEOUT 120 FIXTURES_SETUP app_menu_save_option
                                 FIXTURES_REQUIRED app_menu_save_option_ini)
            # ... and settings.ini still has the empty saves = afterwards.
            add_test(NAME boot.sdk_app_menu_save_option_ini
                     COMMAND "${CMAKE_COMMAND}" -DFILE=${_app_s2}/settings.ini -DABSENT=saves-2
                             -P "${CMAKE_CURRENT_SOURCE_DIR}/check_file_absent.cmake")
            set_tests_properties(boot.sdk_app_menu_save_option_ini PROPERTIES LABELS "boot;runtime"
                                 FIXTURES_REQUIRED app_menu_save_option)
            # A tap on the preselected slot is a choice: settings.ini stores saves-2.
            set(_app_tap "${_dir}/app-menu-tap")
            file(MAKE_DIRECTORY "${_app_tap}/saves" "${_app_tap}/saves-2")
            add_test(NAME boot.sdk_app_menu_slot_tap_reset
                     COMMAND "${CMAKE_COMMAND}" -E copy "${_dir}/app-menu-slot2.ini" "${_app_tap}/settings.ini")
            set_tests_properties(boot.sdk_app_menu_slot_tap_reset PROPERTIES LABELS "boot;runtime"
                                 FIXTURES_SETUP app_menu_slot_tap_ini)
            add_test(NAME boot.sdk_app_menu_slot_tap
                     COMMAND ${_run} --lines "${CMAKE_CURRENT_SOURCE_DIR}/expected/sdk_app_menu_save_option.txt"
                             --absent "FAILED" --absent "app: save slot 1"
                             -- "${LGP_XVFB_RUN}" -a -s "-screen 0 1280x720x24"
                             $<TARGET_FILE:lgp_boot_sdkpair_app> --app --save "${_app_tap}/saves-2"
                             --menu-script slot1,play)
            set_tests_properties(boot.sdk_app_menu_slot_tap PROPERTIES ENVIRONMENT "LGP_APP_DIR=${_app_tap}"
                                 LABELS "boot;runtime;webgpu" TIMEOUT 120 FIXTURES_SETUP app_menu_slot_tap
                                 FIXTURES_REQUIRED app_menu_slot_tap_ini)
            add_test(NAME boot.sdk_app_menu_slot_tap_ini
                     COMMAND "${CMAKE_COMMAND}" -DFILE=${_app_tap}/settings.ini -DPRESENT=saves-2
                             -P "${CMAKE_CURRENT_SOURCE_DIR}/check_file_absent.cmake")
            set_tests_properties(boot.sdk_app_menu_slot_tap_ini PROPERTIES LABELS "boot;runtime"
                                 FIXTURES_REQUIRED app_menu_slot_tap)
        endif()
    endif()
    # A leading "--" (launchers such as xcrun devicectl) is dropped.
    add_test(NAME boot.sdk_app_dashdash
             COMMAND ${_run} --lines "${CMAKE_CURRENT_SOURCE_DIR}/expected/sdk_app.txt"
                     --absent "FAILED" --absent "sdk: driver"
                     -- $<TARGET_FILE:lgp_boot_sdkpair_app> -- --app --frames 2)
    set_tests_properties(boot.sdk_app_dashdash PROPERTIES ENVIRONMENT "LGP_APP_DIR=${_app}"
                         LABELS "boot;runtime" TIMEOUT 120)
    # Without game = … the dump in <data>/game is used (P7.2b; iOS has no
    # folder picker). Here a symbolic link to the pair's folder.
    if(NOT WIN32)
        set(_app_default "${_dir}/app-default")
        file(MAKE_DIRECTORY "${_app_default}")
        file(WRITE "${_app_default}/settings.ini" "args = --renderer soft --input none --audio none\n")
        file(CREATE_LINK "${_dir}" "${_app_default}/game" SYMBOLIC)
        add_test(NAME boot.sdk_app_default_game
                 COMMAND ${_run} --lines "${CMAKE_CURRENT_SOURCE_DIR}/expected/sdk_app.txt"
                         --absent "FAILED" --absent "sdk: driver"
                         -- $<TARGET_FILE:lgp_boot_sdkpair_app> --app --frames 2)
        set_tests_properties(boot.sdk_app_default_game PROPERTIES ENVIRONMENT "LGP_APP_DIR=${_app_default}"
                             LABELS "boot;runtime" TIMEOUT 120)
        # A game inside the app package (LGP_APP_GAME_DIR) comes before
        # <data>/game: a copy of the program with game/ next to it, and an
        # empty <data>/game that would not start.
        set(_packaged "${_dir}/packaged")
        set(_app_packaged "${_dir}/app-packaged")
        file(MAKE_DIRECTORY "${_packaged}" "${_app_packaged}/game")
        file(WRITE "${_app_packaged}/settings.ini" "args = --renderer soft --input none --audio none\n")
        file(CREATE_LINK "${_dir}" "${_packaged}/game" SYMBOLIC)
        add_custom_command(TARGET lgp_boot_sdkpair_app POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:lgp_boot_sdkpair_app>" "${_packaged}/"
            VERBATIM)
        add_test(NAME boot.sdk_app_packaged_game
                 COMMAND ${_run} --lines "${CMAKE_CURRENT_SOURCE_DIR}/expected/sdk_app.txt"
                         --absent "FAILED" --absent "sdk: driver"
                         -- "${_packaged}/$<TARGET_FILE_NAME:lgp_boot_sdkpair_app>" --app --frames 2)
        set_tests_properties(boot.sdk_app_packaged_game PROPERTIES ENVIRONMENT "LGP_APP_DIR=${_app_packaged}"
                             LABELS "boot;runtime" TIMEOUT 120)
        # The pipeline cache seed shipped next to the packaged game
        # (LGP_APP_PIPELINE_SEED) is imported into an empty app folder's cache.
        if(LGP_WEBGPU_AVAILABLE)
            add_test(NAME boot.sdk_app_packaged_seed
                     COMMAND "${CMAKE_COMMAND}" "-DPROGRAM=${_packaged}/$<TARGET_FILE_NAME:lgp_boot_sdkpair_app>"
                             "-DGAME=${_dir}" -P "${CMAKE_CURRENT_SOURCE_DIR}/app_seed_test.cmake")
            # The seed lies next to the packaged program while it runs.
            set_tests_properties(boot.sdk_app_packaged_seed boot.sdk_app_packaged_game PROPERTIES
                                 RESOURCE_LOCK lgp_sdkpair_packaged)
            set_tests_properties(boot.sdk_app_packaged_seed PROPERTIES LABELS "boot;runtime;webgpu" TIMEOUT 120)
        endif()
    endif()
    # Code lifted from main + sdk must not run against main alone.
    add_test(NAME boot.layout_mismatch
             COMMAND ${_run} --exit-code 3 -- $<TARGET_FILE:lgp_boot_sdkpair> "${_exefs}/main")
    # Game mods (P8, lgp/mods/mods.hpp): --hook-log goes through the door on
    # nnMain; a mod without its file, or whose hook has no door, is reported
    # and left out while the game runs on; an unknown mod is a usage error.
    add_test(NAME boot.sdk_hook_log
             COMMAND ${_run} --lines "${CMAKE_CURRENT_SOURCE_DIR}/expected/sdk_hook_log.txt" --absent "FAILED"
                     -- $<TARGET_FILE:lgp_boot_sdkpair> --exefs "${_exefs}" --graphics hle --frames 2 --hook-log 3)
    set(_mods "${CMAKE_CURRENT_BINARY_DIR}/mods")
    file(MAKE_DIRECTORY "${_mods}/none")
    file(WRITE "${_mods}/nodoor/wild-battles.cfg" "# test: an address without hook door\nhook encounter_touch 0x8000000\n")
    add_test(NAME boot.sdk_mods_missing
             COMMAND ${_run} --lines "${CMAKE_CURRENT_SOURCE_DIR}/expected/sdk_mods_missing.txt" --absent "mods: on"
                     -- $<TARGET_FILE:lgp_boot_sdkpair> --exefs "${_exefs}" --graphics hle --frames 2
                     --mods wild-battles --mods-dir "${_mods}/none")
    add_test(NAME boot.sdk_mods_no_door
             COMMAND ${_run} --lines "${CMAKE_CURRENT_SOURCE_DIR}/expected/sdk_mods_no_door.txt" --absent "mods: on"
                     -- $<TARGET_FILE:lgp_boot_sdkpair> --exefs "${_exefs}" --graphics hle --frames 2
                     --mods wild-battles --mods-dir "${_mods}/nodoor")
    # Profanity filter without the firmware's NgWord2 archive (ngc_hle.hpp):
    # answered on the host; with systemdata/0100000000000823 the sdk's own runs.
    add_test(NAME boot.sdk_ngc_host
             COMMAND ${_run} --absent "FAILED" --present "main: profanity filter answered on the host"
                     -- $<TARGET_FILE:lgp_boot_sdkpair> --exefs "${_exefs}")
    set(_ngword_game "${CMAKE_CURRENT_BINARY_DIR}/ngword_game")
    file(MAKE_DIRECTORY "${_ngword_game}/systemdata/0100000000000823")
    file(WRITE "${_ngword_game}/systemdata/0100000000000823/version.dat" "test")
    add_test(NAME boot.sdk_ngc_firmware
             COMMAND ${_run} --absent "FAILED" --present "main: profanity filter of the sdk"
                     --absent "profanity filter answered"
                     -- $<TARGET_FILE:lgp_boot_sdkpair> --exefs "${_exefs}" --game "${_ngword_game}")
    # LGP_IGNORE_SYSTEMDATA=1: the same folder as if it had no systemdata/.
    add_test(NAME boot.sdk_ngc_ignore_systemdata
             COMMAND "${CMAKE_COMMAND}" -E env LGP_IGNORE_SYSTEMDATA=1
                     ${_run} --absent "FAILED" --present "main: profanity filter answered on the host"
                     -- $<TARGET_FILE:lgp_boot_sdkpair> --exefs "${_exefs}" --game "${_ngword_game}")
    set_tests_properties(boot.sdk_ngc_host boot.sdk_ngc_firmware boot.sdk_ngc_ignore_systemdata
                         PROPERTIES LABELS "boot;runtime" TIMEOUT 120)
    add_test(NAME boot.sdk_mods_unknown
             COMMAND ${_run} --exit-code 2 -- $<TARGET_FILE:lgp_boot_sdkpair> --exefs "${_exefs}" --mods nope)
    set_tests_properties(boot.sdk_mods_unknown PROPERTIES PASS_REGULAR_EXPRESSION "--mods: unknown mod nope")
    set_tests_properties(boot.sdk_hook_log boot.sdk_mods_missing boot.sdk_mods_no_door boot.sdk_mods_unknown
                         PROPERTIES LABELS "boot;runtime;mods" TIMEOUT 120)
    set_tests_properties(boot.sdk_graphics_stop boot.sdk_graphics_continue boot.sdk_graphics_hle
                         boot.sdk_graphics_hle_frames boot.layout_mismatch boot.sdk_press_script boot.sdk_press_bad
                         boot.sdk_press_stable_stop boot.sdk_paced boot.sdk_unthrottled boot.sdk_clock_virtual
                         boot.sdk_app PROPERTIES LABELS "boot;runtime" TIMEOUT 120)
    set_tests_properties(boot.layout_mismatch PROPERTIES PASS_REGULAR_EXPRESSION "lifted from 2 modules, 1 were given")
    # P5.2: --audio sdl on SDL's dummy driver (no sound hardware needed).
    if(TARGET lgp_platform_sdl_audio)
        add_test(NAME boot.sdk_audio_sdl
                 COMMAND ${_run} --lines "${CMAKE_CURRENT_SOURCE_DIR}/expected/sdk_audio_sdl.txt"
                         --absent "FAILED" --absent "no sound"
                         -- $<TARGET_FILE:lgp_boot_sdkpair> --exefs "${_exefs}" --audio sdl)
        set_tests_properties(boot.sdk_audio_sdl PROPERTIES LABELS "boot;runtime;audio" TIMEOUT 120
                             ENVIRONMENT "SDL_AUDIO_DRIVER=dummy")
    endif()
    endif()  # NOT CMAKE_CROSSCOMPILING
endif()
if(CMAKE_CROSSCOMPILING)
    return()
endif()

# cmake/LgpCopyGame.cmake (LGP_APP_GAME_DIR): links, stale entries, Xcode
# destination. Not on Windows: symbolic links need a privilege there.
if(NOT CMAKE_HOST_WIN32)
    add_test(NAME boot.copy_game
             COMMAND "${CMAKE_COMMAND}" "-DSCRIPT=${PROJECT_SOURCE_DIR}/cmake/LgpCopyGame.cmake"
                     "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/copy_game"
                     -P "${CMAKE_CURRENT_SOURCE_DIR}/copy_game_test.cmake")
    set_tests_properties(boot.copy_game PROPERTIES LABELS "boot")
endif()

# --- 2. Homebrew fixtures ---------------------------------------------------
if(NOT LGP_BUILD_FIXTURES)
    return()
endif()

function(lgp_boot_fixture name fmt)
    set(_module "${LGP_FIXTURE_DIR}/${name}.${fmt}")
    set(_lifted "${CMAKE_CURRENT_BINARY_DIR}/generated/${name}_${fmt}_lifted.cpp")
    add_custom_command(
        OUTPUT "${_lifted}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
        COMMAND lgp_lift "${_module}" -o "${_lifted}" --summary
        DEPENDS lgp_lift "${_module}"
        COMMENT "Recompiling fixture ${name}.${fmt} for the boot program"
        VERBATIM)
    lgp_add_boot_executable(lgp_boot_${name}_${fmt} "${_lifted}")
    add_dependencies(lgp_boot_${name}_${fmt} lgp_fixtures)
endfunction()

# basic and threads: same debug output and exit as their P2.8 Unicorn runs
# (the expected files come from the fixtures.run tests, so those run first).
foreach(name IN ITEMS basic threads)
    foreach(fmt IN ITEMS nso nro)
        # The expected files come from the Unicorn oracle (tests/homebrew).
        if(NOT LGP_ENABLE_UNICORN)
            continue()
        endif()
        lgp_boot_fixture(${name} ${fmt})
        set(_expected "${LGP_FIXTURE_DIR}/expected/${name}_${fmt}.expected")
        add_test(NAME boot.fixture_${name}_${fmt}
                 COMMAND ${_run} --fixture-expected "${_expected}"
                         -- $<TARGET_FILE:lgp_boot_${name}_${fmt}> "${LGP_FIXTURE_DIR}/${name}.${fmt}" --sched baton)
        set_tests_properties(boot.fixture_${name}_${fmt} PROPERTIES LABELS "boot;fixtures" TIMEOUT 600
                             DEPENDS fixtures.run.${name}_${fmt})
    endforeach()
endforeach()

# services: libnx clients against the HLE services, with a RomFS tree and a
# save directory set up here.
lgp_boot_fixture(services nso)
set(_game "${CMAKE_CURRENT_BINARY_DIR}/services_game")
file(MAKE_DIRECTORY "${_game}/romfs/dir")
file(WRITE "${_game}/romfs/hello.txt" "hello from romfs\n")
execute_process(COMMAND "${Python3_EXECUTABLE}" -c "import sys; open(sys.argv[1], 'wb').write(bytes(range(256)))"
                        "${_game}/romfs/dir/nested.bin")
add_test(NAME boot.fixture_services_nso
         COMMAND ${_run} --lines "${CMAKE_CURRENT_SOURCE_DIR}/expected/services.txt" --absent "FAILED"
                 --clean "${CMAKE_CURRENT_BINARY_DIR}/services_save"
                 -- $<TARGET_FILE:lgp_boot_services_nso> "${LGP_FIXTURE_DIR}/services.nso"
                 --game "${_game}" --save "${CMAKE_CURRENT_BINARY_DIR}/services_save" --program-id 0100c0ffee000001
                 --language de --nickname lgp --time 1700000000)
set_tests_properties(boot.fixture_services_nso PROPERTIES LABELS "boot;fixtures" TIMEOUT 600)
