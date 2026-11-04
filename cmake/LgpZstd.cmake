# zstd for compressed RomFS images (romfs.bin with a block index, read at
# random by fsp-srv; lgp_romfs_pack --compress writes them).
#
# Built from the pinned 1.5.7 release tarball as a static library, natively
# and with Emscripten (single-threaded, no assembly, no legacy formats, no
# dictionary builder). LGP_ZSTD_DIR points at an unpacked zstd-1.5.7 tree
# instead (offline builds; same as FETCHCONTENT_SOURCE_DIR_ZSTD).
# Result: target Lgp::zstd and LGP_ZSTD_AVAILABLE. Without it a compressed
# romfs.bin is refused with a message; plain images work as before.

option(LGP_ENABLE_ZSTD "Read compressed RomFS images with zstd (downloads zstd 1.5.7)" ON)
set(LGP_ZSTD_DIR "" CACHE PATH "Unpacked zstd-1.5.7 source tree to use instead of downloading it")

set(LGP_ZSTD_AVAILABLE OFF)
if(NOT LGP_ENABLE_ZSTD)
    return()
endif()

enable_language(C)
include(FetchContent)
if(LGP_ZSTD_DIR)
    set(FETCHCONTENT_SOURCE_DIR_ZSTD "${LGP_ZSTD_DIR}")
endif()
# Only the sources are needed: zstd's own CMake project (build/cmake) also
# builds programs and a shared library.
FetchContent_Declare(zstd
    URL https://github.com/facebook/zstd/releases/download/v1.5.7/zstd-1.5.7.tar.gz
    URL_HASH SHA256=eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    SOURCE_SUBDIR do-not-configure)
FetchContent_MakeAvailable(zstd)

set(_zstd_lib "${zstd_SOURCE_DIR}/lib")
if(NOT EXISTS "${_zstd_lib}/zstd.h")
    message(WARNING "zstd: no lib/zstd.h under ${zstd_SOURCE_DIR}; compressed RomFS images are not readable")
    return()
endif()
file(GLOB _zstd_sources
    "${_zstd_lib}/common/*.c"
    "${_zstd_lib}/compress/*.c"
    "${_zstd_lib}/decompress/*.c")
add_library(lgp_zstd STATIC ${_zstd_sources})
target_include_directories(lgp_zstd SYSTEM PUBLIC "${_zstd_lib}")
# The x86-64 Huffman decoder in assembly is left out everywhere (MSVC and
# Emscripten cannot build it); the C path is used.
target_compile_definitions(lgp_zstd PRIVATE ZSTD_DISABLE_ASM=1 ZSTD_LEGACY_SUPPORT=0 ZSTD_MULTITHREAD_SUPPORT_DEFAULT=0)
set_target_properties(lgp_zstd PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(MSVC)
    target_compile_options(lgp_zstd PRIVATE /W0)
else()
    target_compile_options(lgp_zstd PRIVATE -w)
endif()
add_library(Lgp::zstd ALIAS lgp_zstd)
set(LGP_ZSTD_AVAILABLE ON)
