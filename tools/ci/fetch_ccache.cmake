# Downloads the pinned ccache release binary for the host (Windows x86_64 or
# Linux x86_64, static musl build) into DEST and checks its SHA-256.
# CI only: the runners have no ccache, and the devkitA64 container has no
# package for it. The binary is a build tool, it is not linked into anything.
#
#   cmake -DDEST=<dir> -P tools/ci/fetch_ccache.cmake
#
# Leaves <DEST>/ccache[.exe]; does nothing if that file is already the pinned
# version (persistent self-hosted runners keep it between jobs).

set(_version 4.13.1)
if(CMAKE_HOST_WIN32)
    set(_asset ccache-${_version}-windows-x86_64)
    set(_ext zip)
    set(_sha256 1c78a0b816a3174d4b170b96294e016a21fb4a577dfd8361e7322f77f85c6348)
    set(_exe ccache.exe)
else()
    set(_asset ccache-${_version}-linux-x86_64-musl-static)
    set(_ext tar.xz)
    set(_sha256 fabe99dd1207c8946348dc3e5539c9e0dde47d1113e3353ba9866ff2fcf405f2)
    set(_exe ccache)
endif()

if(NOT DEST)
    message(FATAL_ERROR "fetch_ccache: pass -DDEST=<dir>")
endif()
file(MAKE_DIRECTORY "${DEST}")
set(_stamp "${DEST}/ccache.version")
if(EXISTS "${DEST}/${_exe}" AND EXISTS "${_stamp}")
    file(READ "${_stamp}" _have)
    if(_have STREQUAL "${_sha256}")
        message(STATUS "fetch_ccache: ${_asset} already in ${DEST}")
        return()
    endif()
endif()

set(_archive "${DEST}/${_asset}.${_ext}")
file(DOWNLOAD
    "https://github.com/ccache/ccache/releases/download/v${_version}/${_asset}.${_ext}"
    "${_archive}"
    EXPECTED_HASH SHA256=${_sha256}
    STATUS _status)
list(GET _status 0 _code)
if(NOT _code EQUAL 0)
    message(FATAL_ERROR "fetch_ccache: download failed: ${_status}")
endif()
file(ARCHIVE_EXTRACT INPUT "${_archive}" DESTINATION "${DEST}/extract" PATTERNS "*/${_exe}")
file(COPY_FILE "${DEST}/extract/${_asset}/${_exe}" "${DEST}/${_exe}")
file(REMOVE_RECURSE "${DEST}/extract" "${_archive}")
file(WRITE "${_stamp}" "${_sha256}")
message(STATUS "fetch_ccache: ${_asset} -> ${DEST}/${_exe}")
