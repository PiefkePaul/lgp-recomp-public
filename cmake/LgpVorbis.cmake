# libvorbis + libogg for the sound track of in-game videos (P5, movie:: HLE):
# WebM videos carry Vorbis audio, decoded on the host natively and in the
# browser alike (no WebCodecs AudioDecoder needed).
#
# Built from the pinned release tarballs (libogg 1.3.6, libvorbis 1.3.7) as
# static libraries with our own small target definitions: only libogg's bit
# packer/framing and libvorbis' codec library (no vorbisfile, vorbisenc,
# programs or tests). LGP_OGG_DIR / LGP_VORBIS_DIR point at unpacked trees
# instead (offline builds). Result: target Lgp::vorbis and
# LGP_VORBIS_AVAILABLE. Without it the HLE does not offer video sound tracks.

option(LGP_ENABLE_VORBIS "Decode the Vorbis sound of in-game videos (downloads libogg 1.3.6 and libvorbis 1.3.7)" ON)
set(LGP_OGG_DIR "" CACHE PATH "Unpacked libogg-1.3.6 source tree to use instead of downloading it")
set(LGP_VORBIS_DIR "" CACHE PATH "Unpacked libvorbis-1.3.7 source tree to use instead of downloading it")

set(LGP_VORBIS_AVAILABLE OFF)
if(NOT LGP_ENABLE_VORBIS)
    return()
endif()

enable_language(C)
include(FetchContent)
if(LGP_OGG_DIR)
    set(FETCHCONTENT_SOURCE_DIR_LGP_OGG "${LGP_OGG_DIR}")
endif()
if(LGP_VORBIS_DIR)
    set(FETCHCONTENT_SOURCE_DIR_LGP_VORBIS "${LGP_VORBIS_DIR}")
endif()
FetchContent_Declare(lgp_ogg
    URL https://github.com/xiph/ogg/releases/download/v1.3.6/libogg-1.3.6.tar.gz
        https://downloads.xiph.org/releases/ogg/libogg-1.3.6.tar.gz
    URL_HASH SHA256=83e6704730683d004d20e21b8f7f55dcb3383cdf84c0daedf30bde175f774638
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    SOURCE_SUBDIR lgp-no-cmake)
FetchContent_Declare(lgp_vorbis
    URL https://github.com/xiph/vorbis/releases/download/v1.3.7/libvorbis-1.3.7.tar.gz
        https://downloads.xiph.org/releases/vorbis/libvorbis-1.3.7.tar.gz
    URL_HASH SHA256=0e982409a9c3fc82ee06e08205b1355e5c6aa4c36bca58146ef399621b0ce5ab
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    SOURCE_SUBDIR lgp-no-cmake)
# Only the sources are needed: SOURCE_SUBDIR names a directory without a
# CMakeLists.txt, so their own CMake projects are not added.
FetchContent_MakeAvailable(lgp_ogg lgp_vorbis)

# libogg's configured header: fixed-width types from <stdint.h>.
set(_ogg_gen "${CMAKE_BINARY_DIR}/generated/lgp_ogg")
file(CONFIGURE OUTPUT "${_ogg_gen}/ogg/config_types.h" CONTENT
"#ifndef __CONFIG_TYPES_H__
#define __CONFIG_TYPES_H__
#include <stdint.h>
typedef int16_t ogg_int16_t;
typedef uint16_t ogg_uint16_t;
typedef int32_t ogg_int32_t;
typedef uint32_t ogg_uint32_t;
typedef int64_t ogg_int64_t;
typedef uint64_t ogg_uint64_t;
#endif
")

add_library(lgp_ogg STATIC "${lgp_ogg_SOURCE_DIR}/src/bitwise.c" "${lgp_ogg_SOURCE_DIR}/src/framing.c")
target_include_directories(lgp_ogg PUBLIC "${lgp_ogg_SOURCE_DIR}/include" "${_ogg_gen}")

set(_vorbis_sources mdct smallft block envelope window lsp lpc analysis synthesis psy info floor1 floor0 res0
                    mapping0 registry codebook sharedbook lookup bitrate)
list(TRANSFORM _vorbis_sources PREPEND "${lgp_vorbis_SOURCE_DIR}/lib/")
list(TRANSFORM _vorbis_sources APPEND ".c")
add_library(lgp_vorbis STATIC ${_vorbis_sources})
target_include_directories(lgp_vorbis PUBLIC "${lgp_vorbis_SOURCE_DIR}/include"
                                      PRIVATE "${lgp_vorbis_SOURCE_DIR}/lib")
target_link_libraries(lgp_vorbis PUBLIC lgp_ogg)
foreach(t lgp_ogg lgp_vorbis)
    set_target_properties(${t} PROPERTIES POSITION_INDEPENDENT_CODE ON C_STANDARD 99)
    if(MSVC)
        target_compile_definitions(${t} PRIVATE _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_DEPRECATE)
        target_compile_options(${t} PRIVATE /W0)
    else()
        target_compile_options(${t} PRIVATE -w)  # third-party code, not ours to fix
    endif()
endforeach()
if(NOT MSVC AND NOT EMSCRIPTEN)
    find_library(LGP_LIBM m)
    if(LGP_LIBM)
        target_link_libraries(lgp_vorbis PUBLIC "${LGP_LIBM}")
    endif()
endif()
add_library(Lgp::vorbis ALIAS lgp_vorbis)
set(LGP_VORBIS_AVAILABLE ON)
