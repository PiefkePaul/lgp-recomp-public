# Game mods (P8, docs/knowledge/spiel-mods-hooks.md): the mod files
# (lgp_mod_file, no dependencies; lgp_lift reads them too) and the mods
# themselves on the hook doors of the runtime (lgp_mods).
add_library(lgp_mod_file STATIC src/mod_file.cpp)
target_include_directories(lgp_mod_file PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(lgp_mod_file PUBLIC cxx_std_20)

add_library(lgp_mods STATIC src/mods.cpp src/hook_dump.cpp src/wild_battles.cpp src/original_encounters.cpp
    src/battle_balls.cpp
    src/battle_throw.cpp
    src/widescreen.cpp
    src/random_encounters.cpp
    src/classic_exp.cpp
    src/online_button.cpp
    src/skip_controller_screen.cpp
    src/audio_free_guard.cpp)
target_link_libraries(lgp_mods PUBLIC lgp_mod_file lgp_runtime_core PRIVATE lgp_ui_layout)
foreach(t IN ITEMS lgp_mod_file lgp_mods)
    if(MSVC)
        target_compile_options(${t} PRIVATE /W4)
    else()
        target_compile_options(${t} PRIVATE -Wall -Wextra)
    endif()
endforeach()
