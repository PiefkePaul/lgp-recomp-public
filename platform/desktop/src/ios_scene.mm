// iPadOS: ask for a window as large as the screen (lgp/platform/desktop/sdl_window.hpp,
// SdlWindow::create). Since iPadOS 26 UIRequiresFullScreen is deprecated and
// apps may run in resizable windows ("Windowed Apps", Stage Manager); there is
// no public call that forces full screen. The scene's size restrictions are
// the closest: a minimum size of the whole screen leaves the system no smaller
// window to offer. Where the system ignores them (full-screen multitasking,
// iPhone) nothing changes. Unchecked on a device.
#import <UIKit/UIKit.h>

#include <SDL3/SDL.h>

#include "lgp/platform/desktop/sdl_window.hpp"

namespace lgp::platform::desktop {

void request_full_size_scene(SDL_Window* window) {
    @autoreleasepool {
        if (UIDevice.currentDevice.userInterfaceIdiom != UIUserInterfaceIdiomPad)
            return;
        UIWindow* uiwindow = (__bridge UIWindow*)SDL_GetPointerProperty(SDL_GetWindowProperties(window),
                                                                         SDL_PROP_WINDOW_UIKIT_WINDOW_POINTER, NULL);
        UIWindowScene* scene = uiwindow.windowScene;
        UISceneSizeRestrictions* restrictions = scene.sizeRestrictions;
        if (restrictions == nil) {
            SDL_Log("lgp: the scene takes no size restrictions (full screen is the system's choice)");
            return;
        }
        // The game runs in landscape only (Info.plist, SDL_HINT_ORIENTATIONS).
        const CGSize screen = scene.screen.bounds.size;
        const CGSize full = CGSizeMake(MAX(screen.width, screen.height), MIN(screen.width, screen.height));
        restrictions.minimumSize = full;
        SDL_Log("lgp: asked for a window of the whole screen (%.0fx%.0f points; LGP_IOS_FULL_SIZE=0 turns it off)",
                full.width, full.height);
    }
}

}  // namespace lgp::platform::desktop
