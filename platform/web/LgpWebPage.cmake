# lgp_add_web_page(<target> [TITLE <text>] [ARGS <default argv>])
#
# Turns an Emscripten executable into the browser front end (P6): the
# target is linked to <target>.js and gets <target>.html next to it, a page
# with the local file picker that copies the user's own game data into OPFS
# (lgp_web.js, opfs_copy_worker.js, copied alongside) and starts the program
# on demand. ARGS is the argv the page passes by default; ?args=... in the
# URL appends to it, ?argv=... replaces it. Serve the directory with
# tools/web/serve.py (COOP/COEP for threads); never host it publicly.
set(LGP_WEB_PAGE_DIR "${CMAKE_CURRENT_LIST_DIR}" CACHE INTERNAL "platform/web sources")

function(lgp_add_web_page target)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "TITLE;ARGS" "")
    if(NOT arg_TITLE)
        set(arg_TITLE "${target}")
    endif()
    set(LGP_WEB_TITLE "${arg_TITLE}")
    set(LGP_WEB_SCRIPT "${target}.js")
    set(LGP_WEB_ARGS "${arg_ARGS}")
    set(_html "${CMAKE_CURRENT_BINARY_DIR}/${target}.page.html")
    configure_file("${LGP_WEB_PAGE_DIR}/game.html.in" "${_html}" @ONLY)
    set_target_properties(${target} PROPERTIES SUFFIX ".js"
        LINK_DEPENDS "${LGP_WEB_PAGE_DIR}/js/lgp_web.js;${LGP_WEB_PAGE_DIR}/js/opfs_copy_worker.js")
    target_link_libraries(${target} PRIVATE lgp_platform_web)
    # The page loads <target>.js only when the user starts the program, with
    # Module (arguments, canvas, output) already set up by lgp_web.js.
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_html}" "$<TARGET_FILE_DIR:${target}>/${target}.html"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${LGP_WEB_PAGE_DIR}/js/lgp_web.js"
                "${LGP_WEB_PAGE_DIR}/js/opfs_copy_worker.js" "$<TARGET_FILE_DIR:${target}>"
        # The page's icon (tools/icon/gen_icons.py).
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${LGP_WEB_PAGE_DIR}/../icon/lgp-icon.svg"
                "${LGP_WEB_PAGE_DIR}/../icon/lgp-icon-180.png" "$<TARGET_FILE_DIR:${target}>"
        VERBATIM)
endfunction()
