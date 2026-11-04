#include "lgp/platform/desktop/sdl_dialogs.hpp"

#include <SDL3/SDL.h>

#include <atomic>

namespace lgp::platform::desktop {

namespace {

// SDL's video subsystem for the time of one dialog (refcounted by SDL).
struct VideoScope {
    bool ok = SDL_InitSubSystem(SDL_INIT_VIDEO);
    ~VideoScope() {
        if (ok)
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
    }
};

struct FolderResult {
    std::atomic<bool> done{false};
    bool cancelled = false;
    std::string folder;
    std::string error;
};

}  // namespace

std::optional<std::string> pick_folder(const std::string& title, const std::string& start_folder, std::string* error) {
    VideoScope video;
    if (!video.ok) {
        *error = std::string("SDL video: ") + SDL_GetError();
        return std::nullopt;
    }
    FolderResult result;
    // The callback may run on another thread (Linux portal) or from the event
    // pump below (Windows, macOS); either way after this function set it up.
    const SDL_DialogFileCallback callback = [](void* user, const char* const* files, int) {
        auto* r = static_cast<FolderResult*>(user);
        if (files == nullptr)
            r->error = SDL_GetError();
        else if (files[0] == nullptr)
            r->cancelled = true;
        else
            r->folder = files[0];
        r->done.store(true, std::memory_order_release);
    };
    const SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetStringProperty(props, SDL_PROP_FILE_DIALOG_TITLE_STRING, title.c_str());
    if (!start_folder.empty())
        SDL_SetStringProperty(props, SDL_PROP_FILE_DIALOG_LOCATION_STRING, start_folder.c_str());
    SDL_SetBooleanProperty(props, SDL_PROP_FILE_DIALOG_MANY_BOOLEAN, false);
    SDL_ShowFileDialogWithProperties(SDL_FILEDIALOG_OPENFOLDER, callback, &result, props);
    SDL_DestroyProperties(props);
    while (!result.done.load(std::memory_order_acquire))
        SDL_WaitEventTimeout(nullptr, 50);  // pumps the platform's events
    if (!result.error.empty()) {
        *error = result.error;
        return std::nullopt;
    }
    if (result.cancelled)
        return std::nullopt;
    return result.folder;
}

int show_message(MessageKind kind, const std::string& title, const std::string& text,
                 const std::vector<std::string>& buttons) {
    VideoScope video;  // works without it too on most platforms; keeps one path
    std::vector<SDL_MessageBoxButtonData> data;
    for (size_t i = 0; i < buttons.size(); ++i) {
        SDL_MessageBoxButtonData b{};
        b.buttonID = int(i);
        b.text = buttons[i].c_str();
        if (i == 0)
            b.flags |= SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT;
        if (i + 1 == buttons.size())
            b.flags |= SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT;
        data.push_back(b);
    }
    SDL_MessageBoxData box{};
    box.flags = kind == MessageKind::Error     ? SDL_MESSAGEBOX_ERROR
                : kind == MessageKind::Warning ? SDL_MESSAGEBOX_WARNING
                                               : SDL_MESSAGEBOX_INFORMATION;
    box.flags |= SDL_MESSAGEBOX_BUTTONS_LEFT_TO_RIGHT;
    box.title = title.c_str();
    box.message = text.c_str();
    box.numbuttons = int(data.size());
    box.buttons = data.data();
    int pressed = -1;
    if (!SDL_ShowMessageBox(&box, &pressed))
        return -1;
    return pressed;
}

bool open_app_settings(std::string* error) {
    if (SDL_OpenURL("app-settings:"))
        return true;
    *error = SDL_GetError();
    return false;
}

}  // namespace lgp::platform::desktop
