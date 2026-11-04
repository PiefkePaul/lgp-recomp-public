# libopus for hwopus (P5.2): the title's Opus packets are decoded on the
# host instead of the console's audio DSP.
#
# Built from the pinned 1.5.2 release tarball as a static library, natively
# and with Emscripten (float build, no DNN/DRED/OSCE extensions, no programs
# or tests). LGP_OPUS_DIR points at an unpacked opus-1.5.2 tree instead
# (offline builds; same as FETCHCONTENT_SOURCE_DIR_OPUS).
# Result: target Opus::opus and LGP_OPUS_AVAILABLE. Without it hwopus decodes
# every packet to silence of the right length.

option(LGP_ENABLE_OPUS "Decode Opus in hwopus with libopus (downloads libopus 1.5.2)" ON)
set(LGP_OPUS_DIR "" CACHE PATH "Unpacked opus-1.5.2 source tree to use instead of downloading it")

set(LGP_OPUS_AVAILABLE OFF)
if(NOT LGP_ENABLE_OPUS)
    return()
endif()

enable_language(C)
include(FetchContent)
if(LGP_OPUS_DIR)
    set(FETCHCONTENT_SOURCE_DIR_OPUS "${LGP_OPUS_DIR}")
endif()
FetchContent_Declare(opus
    URL https://github.com/xiph/opus/releases/download/v1.5.2/opus-1.5.2.tar.gz
        https://downloads.xiph.org/releases/opus/opus-1.5.2.tar.gz
    URL_HASH SHA256=65c1d2f78b9f2fb20082c38cbe47c951ad5839345876e46941612ee87f9a7ce1
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    EXCLUDE_FROM_ALL)
set(OPUS_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(OPUS_DRED OFF CACHE BOOL "" FORCE)
set(OPUS_OSCE OFF CACHE BOOL "" FORCE)
# _FORTIFY_SOURCE warns in Debug builds and -fstack-protector is not
# available under Emscripten; neither matters for a decoder fed by the title.
set(OPUS_FORTIFY_SOURCE OFF CACHE BOOL "" FORCE)
set(OPUS_STACK_PROTECTOR OFF CACHE BOOL "" FORCE)
# libopus registers its own tests whenever BUILD_TESTING is on; they are not
# built here (EXCLUDE_FROM_ALL), so keep them out of ctest.
block(SCOPE_FOR VARIABLES)
    set(BUILD_TESTING OFF)
    FetchContent_MakeAvailable(opus)
endblock()
if(TARGET Opus::opus)
    set(LGP_OPUS_AVAILABLE ON)
endif()
