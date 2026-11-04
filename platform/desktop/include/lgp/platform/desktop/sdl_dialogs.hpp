// The desktop app's system dialogs through SDL3 (P7.1): picking the game
// folder and telling the user what went wrong when there is no console (a
// double-clicked app). Main thread only; they block until the user answers.
#pragma once

#include <optional>
#include <string>
#include <vector>

namespace lgp::platform::desktop {

// The platform's folder picker (UTF-8 paths). nullopt if the user cancelled;
// also nullopt with *error if the platform has no picker (Linux without the
// desktop portal and without zenity).
std::optional<std::string> pick_folder(const std::string& title, const std::string& start_folder, std::string* error);

enum class MessageKind { Info, Warning, Error };

// A message box with buttons; returns the index of the pressed button, -1 if
// it was closed otherwise or could not be shown. The first button is the
// default for Enter, the last one for Escape.
int show_message(MessageKind kind, const std::string& title, const std::string& text,
                 const std::vector<std::string>& buttons);

// The app's page in the system's Settings app (iOS: "app-settings:", as
// UIApplicationOpenSettingsURLString). False with *error where it did not open.
bool open_app_settings(std::string* error);

}  // namespace lgp::platform::desktop
