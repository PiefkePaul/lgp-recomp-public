# FFmpeg's libavcodec for in-game videos (P5, movie:: HLE): VP9/H.264 frames
# decoded on the host. Optional and not downloaded: LGP_FFMPEG_DIR points at
# an FFmpeg "shared" development build (include/, lib/, bin/ with the DLLs on
# Windows), otherwise pkg-config's libavcodec/libavutil are used if present.
# Linked dynamically (LGPL). Without it videos play as black pictures.
# Result: target Ffmpeg::avcodec and LGP_FFMPEG_AVAILABLE.

set(LGP_FFMPEG_DIR "" CACHE PATH "FFmpeg shared development build (include/, lib/, bin/) for video decoding")
set(LGP_FFMPEG_AVAILABLE OFF)
if(EMSCRIPTEN)
    return()
endif()

if(LGP_FFMPEG_DIR)
    find_path(_lgp_avcodec_include libavcodec/avcodec.h PATHS "${LGP_FFMPEG_DIR}/include" NO_DEFAULT_PATH)
    find_library(_lgp_avcodec_lib avcodec PATHS "${LGP_FFMPEG_DIR}/lib" NO_DEFAULT_PATH)
    find_library(_lgp_avutil_lib avutil PATHS "${LGP_FFMPEG_DIR}/lib" NO_DEFAULT_PATH)
    if(NOT _lgp_avcodec_include OR NOT _lgp_avcodec_lib OR NOT _lgp_avutil_lib)
        message(FATAL_ERROR "LGP_FFMPEG_DIR=${LGP_FFMPEG_DIR} has no libavcodec/libavutil development files")
    endif()
    # Dynamic linking only (LGPL): refuse static archives. On Windows the .lib
    # files are import libraries only if the build ships its DLLs in bin/.
    file(GLOB LGP_FFMPEG_DLLS "${LGP_FFMPEG_DIR}/bin/*.dll")
    if(WIN32)
        if(NOT LGP_FFMPEG_DLLS)
            message(FATAL_ERROR "LGP_FFMPEG_DIR=${LGP_FFMPEG_DIR} has no DLLs in bin/: use a \"shared\" FFmpeg build")
        endif()
    else()
        foreach(_lib "${_lgp_avcodec_lib}" "${_lgp_avutil_lib}")
            if(_lib MATCHES "\\.a$")
                message(FATAL_ERROR "${_lib} is a static library: FFmpeg must be linked dynamically (LGPL)")
            endif()
        endforeach()
    endif()
    add_library(Ffmpeg::avcodec INTERFACE IMPORTED)
    target_include_directories(Ffmpeg::avcodec INTERFACE "${_lgp_avcodec_include}")
    target_link_libraries(Ffmpeg::avcodec INTERFACE "${_lgp_avcodec_lib}" "${_lgp_avutil_lib}")
    # LGP_FFMPEG_DLLS: copied next to programs (lgp_copy_ffmpeg_dlls).
    set(LGP_FFMPEG_AVAILABLE ON)
elseif(NOT CMAKE_CROSSCOMPILING)
    # Cross builds (iOS, Android) would find the build machine's libavcodec.
    find_package(PkgConfig QUIET)
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(LGP_LIBAVCODEC QUIET IMPORTED_TARGET libavcodec libavutil)
        if(LGP_LIBAVCODEC_FOUND)
            add_library(Ffmpeg::avcodec INTERFACE IMPORTED)
            target_link_libraries(Ffmpeg::avcodec INTERFACE PkgConfig::LGP_LIBAVCODEC)
            set(LGP_FFMPEG_AVAILABLE ON)
        endif()
    endif()
endif()
if(LGP_FFMPEG_AVAILABLE)
    message(STATUS "FFmpeg video decoding: on")
endif()

# Copies the FFmpeg DLLs next to a program (Windows; no-op elsewhere).
function(lgp_copy_ffmpeg_dlls target)
    if(LGP_FFMPEG_DLLS)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${LGP_FFMPEG_DLLS} "$<TARGET_FILE_DIR:${target}>")
    endif()
endfunction()
