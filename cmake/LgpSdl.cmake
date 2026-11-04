# SDL3 for the desktop (P4.4 window, P5 audio, gamepads and keyboard).
#
# Built from the pinned release tarball as a static library with the video,
# audio, joystick/gamepad (incl. HIDAPI drivers for Joy-Cons and the Pro
# Controller), haptic and sensor subsystems. On Linux the audio drivers
# (PipeWire, PulseAudio, ALSA) are loaded at run time and only built where
# their headers exist; the dummy/disk drivers always exist. The WebGPU surface is created from SDL's native window
# handles (HWND, CAMetalLayer, X11 window, Wayland surface), so neither SDL's
# renderer nor its GPU API is needed. On Linux SDL loads X11/Wayland at run
# time, but needs their development headers at build time (libx11-dev,
# libwayland-dev); without them it builds without a window backend.
# Result: target SDL3::SDL3-static and LGP_SDL_AVAILABLE.

if(EMSCRIPTEN OR NOT LGP_WEBGPU_AVAILABLE)
    set(_lgp_sdl_default OFF)
else()
    set(_lgp_sdl_default ON)
endif()
option(LGP_ENABLE_SDL "Build the SDL3 desktop window (downloads SDL 3.4.16)" ${_lgp_sdl_default})

set(LGP_SDL_AVAILABLE OFF)
if(NOT LGP_ENABLE_SDL)
    return()
endif()

include(FetchContent)
FetchContent_Declare(sdl3
    URL https://github.com/libsdl-org/SDL/releases/download/release-3.4.16/SDL3-3.4.16.tar.gz
    URL_HASH SHA256=7322236cd12090c3eb40b9728be4d49c76f66ad17d04369584d4ecad5cf77c68
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    EXCLUDE_FROM_ALL)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
foreach(_sub GPU CAMERA POWER TRAY)
    set(SDL_${_sub} OFF CACHE BOOL "" FORCE)
endforeach()
# FORCE also for these: build trees configured before P5 cached them OFF.
# RENDER: the software keyboard's dialog window draws with SDL_Renderer
# (debug font); the game itself renders through WebGPU. DIALOG: the app's
# folder picker (P7.1; Windows IFileDialog, macOS NSOpenPanel, Linux the
# desktop portal over D-Bus or zenity, both looked up at run time).
foreach(_sub VIDEO RENDER AUDIO JOYSTICK HAPTIC HIDAPI SENSOR DIALOG)
    set(SDL_${_sub} ON CACHE BOOL "" FORCE)
endforeach()
set(SDL_OPENGL OFF CACHE BOOL "" FORCE)
set(SDL_OPENGLES OFF CACHE BOOL "" FORCE)
set(SDL_VULKAN OFF CACHE BOOL "" FORCE)
# Android (P7.3): SDL's Java sources go into the APK through
# tools/android/build_apk.py, SDL's own jar is not needed.
set(SDL_ANDROID_JAR OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(sdl3)
if(TARGET SDL3::SDL3-static)
    set(LGP_SDL_AVAILABLE ON)
endif()
