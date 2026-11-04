# Mbed TLS for wss:// to the RyuLDN server (docs/knowledge/ldn-ueber-ryuldn.md):
# TLS over the host's TCP stream in lgp_platform (ILan::connect_tls).
#
# Built from the pinned 3.6.7 LTS release tarball as static libraries (no
# programs, no tests). LGP_MBEDTLS_DIR points at an unpacked mbedtls-3.6.7
# tree instead (offline builds; same as FETCHCONTENT_SOURCE_DIR_MBEDTLS).
# Not under Emscripten (no sockets). Result: targets MbedTLS::mbedtls (with
# mbedx509, mbedcrypto) and LGP_MBEDTLS_AVAILABLE. Without it connect_tls
# fails and the app stays with ws://.

option(LGP_ENABLE_MBEDTLS "TLS (wss://) for online play with Mbed TLS (downloads Mbed TLS 3.6.7)" ON)
set(LGP_MBEDTLS_DIR "" CACHE PATH "Unpacked mbedtls-3.6.7 source tree to use instead of downloading it")

set(LGP_MBEDTLS_AVAILABLE OFF)
if(NOT LGP_ENABLE_MBEDTLS OR EMSCRIPTEN)
    return()
endif()

enable_language(C)
include(FetchContent)
if(LGP_MBEDTLS_DIR)
    set(FETCHCONTENT_SOURCE_DIR_MBEDTLS "${LGP_MBEDTLS_DIR}")
endif()
FetchContent_Declare(mbedtls
    URL https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.7/mbedtls-3.6.7.tar.bz2
    URL_HASH SHA256=a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    EXCLUDE_FROM_ALL)
set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(USE_SHARED_MBEDTLS_LIBRARY OFF CACHE BOOL "" FORCE)
set(USE_STATIC_MBEDTLS_LIBRARY ON CACHE BOOL "" FORCE)
set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
set(DISABLE_PACKAGE_CONFIG_AND_INSTALL ON CACHE BOOL "" FORCE)
block(SCOPE_FOR VARIABLES)
    set(BUILD_TESTING OFF)
    # Mbed TLS takes CMAKE_C_SIMULATE_ID for the compiler: clang with the GNU
    # driver on the MSVC ABI (Windows, our clang job) would get /W3 /utf-8,
    # which that driver reads as file names. It is clang there, so say so.
    if(CMAKE_C_SIMULATE_ID STREQUAL "MSVC" AND CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "GNU")
        set(CMAKE_C_SIMULATE_ID "")
    endif()
    FetchContent_MakeAvailable(mbedtls)
endblock()
if(TARGET mbedtls)
    if(NOT TARGET MbedTLS::mbedtls)
        add_library(MbedTLS::mbedtls ALIAS mbedtls)
    endif()
    set(LGP_MBEDTLS_AVAILABLE ON)
endif()
