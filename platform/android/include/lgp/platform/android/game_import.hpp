// First start of the Android app without game data (P7.3): asks for the
// dump's folder through the system picker and copies exefs/ and romfs.bin or
// romfs/ into `dest` (org.lgp.recomp.GameImport). Only in app builds (SDL's
// activity); blocks until the user is done.
#pragma once

#include <string>

namespace lgp::platform::android {

enum class ImportResult { Imported, Cancelled, Failed };

// message: shown before the picker (why the game is needed).
ImportResult import_game(const std::string& message, const std::string& dest, std::string* error);

}  // namespace lgp::platform::android
