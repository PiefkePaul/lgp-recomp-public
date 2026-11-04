// Entry point of an Android app (P7.3): SDL's activity loads the program's
// shared library and calls SDL_main on its own thread. SDL_main.h renames
// main() below to SDL_main; the program's real main() was compiled as
// lgp_program_main (LGP_MAIN_NAME, cmake/LgpAndroidApk.cmake).
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

extern "C" int lgp_program_main(int argc, char** argv);

int main(int argc, char* argv[]) {
    return lgp_program_main(argc, argv);
}
