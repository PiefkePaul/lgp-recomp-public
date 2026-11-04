// iOS/tvOS: the application's actual lifecycle state, for SdlWindow (see
// uikit_app_in_background() in lgp/platform/desktop/sdl_window.hpp).
#import <UIKit/UIKit.h>

#include "lgp/platform/desktop/sdl_window.hpp"

namespace lgp::platform::desktop {

bool uikit_app_in_background() {
    @autoreleasepool {
        return UIApplication.sharedApplication.applicationState == UIApplicationStateBackground;
    }
}

}  // namespace lgp::platform::desktop
