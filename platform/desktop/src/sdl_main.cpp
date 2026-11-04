// main() of a boot program where SDL has to own it (iOS, P7.2b): SDL_main.h
// turns this main into SDL_main and supplies the real one, which starts
// UIApplicationMain with SDL's scene delegate and calls back here once the
// window scene is connected. The boot program's own entry is lgp_boot_main
// (runtime/boot/src/main.cpp with LGP_BOOT_SDL_MAIN).
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <cstdio>
#include <cstdlib>

extern "C" int lgp_boot_main(int argc, char** argv);

namespace {

// SDL_Log goes to NSLog on Apple; lgp_boot_main redirects only stdout and
// stderr to <data>/logs/lgp.log. Write it to stderr as well, so lines like
// "lgp: gamepad ... -> slot 0" and the --input-log summaries land in lgp.log
// (seen there on the Apple TV, tools/apple/tvos_input_report.sh). Whether
// NSLog writes a second copy of each line there is not checked yet.
void log_to_stderr(void* userdata, int category, SDL_LogPriority priority, const char* message) {
    std::fprintf(stderr, "%s\n", message);
    std::fflush(stderr);
    if (auto def = reinterpret_cast<SDL_LogOutputFunction>(userdata))
        def(nullptr, category, priority, message);
}

}  // namespace

int main(int argc, char* argv[]) {
    // On iOS SDL keeps the app alive after its main returns (an empty window
    // until the user swipes it away). The game has ended (ExitProcess or an
    // error), so end the process, as on the desktop; scripted runs over
    // devicectl then see it finish.
    SDL_SetLogOutputFunction(log_to_stderr, reinterpret_cast<void*>(SDL_GetDefaultLogOutputFunction()));
    std::exit(lgp_boot_main(argc, argv));
}
