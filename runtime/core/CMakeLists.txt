# Runtime core (P2.6/P3): guest memory (ADR-0001), the Horizon kernel and the
# runtime header for lifted code (guest.hpp). No third-party dependencies and
# no OS code (that is in platform/), so the wasm preset builds it unchanged.

# Guest-memory access mode (guest_mem.hpp). The definitions are PUBLIC: every
# translation unit that includes the inline accessors, lifted code included,
# must agree, so a different mode means a separate library variant.
option(LGP_GUEST_MEM_CHECKED "Bounds/permission-check every guest access (default: Debug builds only)" OFF)
option(LGP_GUEST_MEM_WRITE_WATCH "Track stores to watched pages (ADR-0001 point 4, GPU pools)" OFF)
option(LGP_GUEST_MEM64 "No 32-bit address truncation (memory64 build, untested)" OFF)

function(lgp_add_runtime_core target)
    cmake_parse_arguments(ARG "" "" "DEFINITIONS" ${ARGN})
    set(sources
        src/clock.cpp
        src/dispatch.cpp
        src/guest_mem.cpp
        src/handle_table.cpp
        src/hooks.cpp
        src/kernel.cpp
        src/log.cpp
        src/memory.cpp
        src/patch.cpp
        src/render_hints.cpp
        src/scheduler.cpp
        src/synchronization.cpp
        src/system.cpp
        src/time_stats.cpp
        src/write_watch.cpp)
    list(TRANSFORM sources PREPEND "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/")
    add_library(${target} STATIC ${sources})
    target_include_directories(${target} PUBLIC "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/include")
    target_compile_features(${target} PUBLIC cxx_std_20)
    # lgp_lifted_headers: CpuState (the guest registers of each thread).
    # lgp_platform: host memory and threads; runtime/ has no OS code (P3.6).
    target_link_libraries(${target} PUBLIC lgp_platform lgp_lifted_headers)
    target_compile_definitions(${target} PUBLIC ${ARG_DEFINITIONS})
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra)
    endif()
endfunction()

set(_lgp_mem_defs)
if(LGP_GUEST_MEM_CHECKED)
    list(APPEND _lgp_mem_defs LGP_GUEST_MEM_CHECKED=1)
else()
    list(APPEND _lgp_mem_defs $<$<CONFIG:Debug>:LGP_GUEST_MEM_CHECKED=1>)
endif()
if(LGP_GUEST_MEM_WRITE_WATCH)
    list(APPEND _lgp_mem_defs LGP_GUEST_MEM_WRITE_WATCH=1)
endif()
if(LGP_GUEST_MEM64)
    list(APPEND _lgp_mem_defs LGP_GUEST_MEM64=1)
endif()
lgp_add_runtime_core(lgp_runtime_core DEFINITIONS ${_lgp_mem_defs})
