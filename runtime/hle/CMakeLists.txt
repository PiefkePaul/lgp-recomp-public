# HLE services (P3.3+): HIPC/CMIF sessions, sm: and the Horizon services the
# game talks to. Runs on top of runtime/core.

function(lgp_add_runtime_hle target core)
    set(sources
        src/acc.cpp
        src/am.cpp
        src/applets.cpp
        src/audout.cpp
        src/bcat.cpp
        src/csrng.cpp
        src/event.cpp
        src/friends.cpp
        src/fs.cpp
        src/hid.cpp
        src/hid_gesture.cpp
        src/hipc.cpp
        src/hwopus.cpp
        src/ldn.cpp
        src/ldn_dual.cpp
        src/ldn_lan.cpp
        src/ldn_ryu.cpp
        src/ldn_ws.cpp
        src/nifm.cpp
        src/pctl.cpp
        src/pl.cpp
        src/quiet.cpp
        src/romfs.cpp
        src/romfs_compressed.cpp
        src/webm.cpp
        src/save_data.cpp
        src/service.cpp
        src/services.cpp
        src/set.cpp
        src/sockets.cpp
        src/swkbd.cpp
        src/sm.cpp
        src/time.cpp
        src/vi.cpp
        src/vorbis.cpp)
    list(TRANSFORM sources PREPEND "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/")
    add_library(${target} STATIC ${sources})
    target_include_directories(${target} PUBLIC "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/include")
    target_link_libraries(${target} PUBLIC ${core})
    # hwopus decodes with libopus when it was built (P5.2), else to silence.
    if(LGP_OPUS_AVAILABLE)
        target_link_libraries(${target} PRIVATE Opus::opus)
        target_compile_definitions(${target} PRIVATE LGP_HLE_OPUS=1)
    endif()
    # Sound tracks of in-game videos (movie:: HLE) with libvorbis when built.
    if(LGP_VORBIS_AVAILABLE)
        target_link_libraries(${target} PRIVATE Lgp::vorbis)
        target_compile_definitions(${target} PRIVATE LGP_HLE_VORBIS=1)
    endif()
    # Compressed RomFS images (romfs.bin with a block index) with zstd.
    if(LGP_ZSTD_AVAILABLE)
        target_link_libraries(${target} PRIVATE Lgp::zstd)
        target_compile_definitions(${target} PRIVATE LGP_HLE_ZSTD=1)
    endif()
    # Free stand-in for the standard system font (pl:u without firmware).
    if(LGP_FREE_FONT_AVAILABLE)
        target_link_libraries(${target} PRIVATE lgp_free_font)
        target_compile_definitions(${target} PRIVATE LGP_HLE_FREE_FONT=1)
    endif()
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra)
    endif()
endfunction()

lgp_add_runtime_hle(lgp_runtime_hle lgp_runtime_core)

# Host tool: extracted RomFS directory -> romfs.bin (not for Emscripten).
if(NOT EMSCRIPTEN)
    add_executable(lgp_romfs_pack tools/lgp_romfs_pack.cpp)
    target_link_libraries(lgp_romfs_pack PRIVATE lgp_runtime_hle lgp_platform_common)
endif()
