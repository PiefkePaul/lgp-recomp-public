# Copies a game folder into an app package (LGP_APP_GAME_DIR, Paul 2026-10-14:
# the dump may live inside the app). Run as a script:
#   cmake -DSRC=<game dir> -DDST=<package>/game -P cmake/LgpCopyGame.cmake
# SRC may also be a single file (LGP_APP_PIPELINE_SEED); DST is then the file
# to write, copied when size or time stamp differ (an older replacement too,
# rsync -t), keeping the time stamp of SRC.
# -DSTAMP=<file> records the destination, one line per destination (Xcode
# builds Debug and Release, iphoneos and simulator into different folders
# with one stamp); -DREMOVE_STAMP=<file> alone removes every destination
# recorded there (the option was cleared) and the stamp, so only what this
# script copied is ever removed.
# In an Xcode build phase -DDST_XCODE_RESOURCES=<name> puts it at
# $TARGET_BUILD_DIR/$UNLOCALIZED_RESOURCES_FOLDER_PATH/<name> instead (the
# .app on iOS/tvOS, Contents/Resources on macOS).
# file(COPY) keeps time stamps and skips files whose copy has the same one,
# so a rebuild does not copy 4 GiB again. Symbolic links in SRC (romfs.bin on
# another drive) are copied as the files they point to: a link in the package
# points nowhere on the device and codesign rejects it. Files and folders
# that are gone from SRC are removed from DST. The package is a build output:
# it holds game data and never goes into the repository.
cmake_minimum_required(VERSION 3.25)  # script mode: if(IN_LIST) and friends
if(REMOVE_STAMP)
    if(EXISTS "${REMOVE_STAMP}")
        file(STRINGS "${REMOVE_STAMP}" _olds)
        foreach(_old IN LISTS _olds)
            if(NOT _old STREQUAL "" AND (EXISTS "${_old}" OR IS_SYMLINK "${_old}"))
                message(STATUS "LGP_APP_GAME_DIR: removing ${_old} (no longer configured)")
                file(REMOVE_RECURSE "${_old}")
            endif()
        endforeach()
        file(REMOVE "${REMOVE_STAMP}")
    endif()
    return()
endif()
if(NOT EXISTS "${SRC}")
    message(FATAL_ERROR "LGP_APP_GAME_DIR: ${SRC} does not exist")
endif()
if(DST_XCODE_RESOURCES)
    if(NOT DEFINED ENV{TARGET_BUILD_DIR} OR NOT DEFINED ENV{UNLOCALIZED_RESOURCES_FOLDER_PATH})
        message(FATAL_ERROR "LGP_APP_GAME_DIR: not run by Xcode (TARGET_BUILD_DIR is not set)")
    endif()
    set(DST "$ENV{TARGET_BUILD_DIR}/$ENV{UNLOCALIZED_RESOURCES_FOLDER_PATH}/${DST_XCODE_RESOURCES}")
endif()
if(DST STREQUAL "")
    message(FATAL_ERROR "LGP_APP_GAME_DIR: no destination")
endif()
message(STATUS "LGP_APP_GAME_DIR: ${SRC} -> ${DST}")

# Copies one resolved file (the target of a link) to dst, unless dst is a
# regular file of the same size and time stamp. file(COPY) keeps the time
# stamp (COPY_FILE does not), so the next build sees the copy as current; it
# goes through a staging folder because file(COPY) keeps the name.
function(lgp_copy_resolved src dst)
    file(TIMESTAMP "${src}" _src_time "%s" UTC)
    if(EXISTS "${dst}" AND NOT IS_SYMLINK "${dst}" AND NOT IS_DIRECTORY "${dst}")
        file(SIZE "${src}" _src_size)
        file(SIZE "${dst}" _dst_size)
        file(TIMESTAMP "${dst}" _dst_time "%s" UTC)
        if(_src_size EQUAL _dst_size AND _src_time EQUAL _dst_time)
            return()
        endif()
    endif()
    get_filename_component(_name "${src}" NAME)
    get_filename_component(_dir "${dst}" DIRECTORY)
    set(_stage "${_dir}/.lgp-copy-stage")
    file(REMOVE_RECURSE "${_stage}")
    file(COPY "${src}" DESTINATION "${_stage}")
    file(REMOVE_RECURSE "${dst}")
    file(RENAME "${_stage}/${_name}" "${dst}")
    file(REMOVE_RECURSE "${_stage}")
endfunction()

# Adds DST to STAMP unless it is listed already.
macro(lgp_record_stamp)
    if(STAMP)
        set(_listed)
        if(EXISTS "${STAMP}")
            file(STRINGS "${STAMP}" _listed)
        endif()
        if(NOT DST IN_LIST _listed)
            file(APPEND "${STAMP}" "${DST}\n")
        endif()
    endif()
endmacro()

function(lgp_copy_tree src dst)
    file(MAKE_DIRECTORY "${dst}")
    file(GLOB _entries LIST_DIRECTORIES true "${src}/*")
    foreach(e IN LISTS _entries)
        get_filename_component(_name "${e}" NAME)
        set(_to "${dst}/${_name}")
        if(IS_SYMLINK "${e}")
            file(REAL_PATH "${e}" _real)
            if(NOT EXISTS "${_real}")
                message(FATAL_ERROR "LGP_APP_GAME_DIR: ${e} points to ${_real}, which does not exist")
            endif()
            if(IS_SYMLINK "${_to}")
                file(REMOVE "${_to}")
            endif()
            if(IS_DIRECTORY "${_real}")
                lgp_copy_tree("${_real}" "${_to}")
            else()
                lgp_copy_resolved("${_real}" "${_to}")
            endif()
        elseif(IS_DIRECTORY "${e}")
            if(IS_SYMLINK "${_to}" OR (EXISTS "${_to}" AND NOT IS_DIRECTORY "${_to}"))
                file(REMOVE "${_to}")
            endif()
            lgp_copy_tree("${e}" "${_to}")
        else()
            if(IS_SYMLINK "${_to}" OR IS_DIRECTORY "${_to}")
                file(REMOVE_RECURSE "${_to}")
            endif()
            file(COPY "${e}" DESTINATION "${dst}")
        endif()
    endforeach()
endfunction()

if(NOT IS_DIRECTORY "${SRC}")
    file(REAL_PATH "${SRC}" _real)
    get_filename_component(_dst_dir "${DST}" DIRECTORY)
    file(MAKE_DIRECTORY "${_dst_dir}")
    lgp_copy_resolved("${_real}" "${DST}")
    lgp_record_stamp()
    return()
endif()
lgp_copy_tree("${SRC}" "${DST}")
file(GLOB_RECURSE _dst RELATIVE "${DST}" LIST_DIRECTORIES true "${DST}/*")
foreach(rel IN LISTS _dst)
    if(NOT EXISTS "${SRC}/${rel}" AND (EXISTS "${DST}/${rel}" OR IS_SYMLINK "${DST}/${rel}"))
        file(REMOVE_RECURSE "${DST}/${rel}")
    endif()
endforeach()
lgp_record_stamp()
