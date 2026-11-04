// The Settings app's store of system_settings.hpp on iOS/iPadOS: the
// standard NSUserDefaults, which the app's Settings.bundle edits
// (platform/ios/Settings.bundle). Built with ARC (platform/CMakeLists.txt).
#include "lgp/platform/system_settings.hpp"

#if !__has_feature(objc_arc)
#error "system_settings.mm needs ARC (-fobjc-arc)"
#endif

#import <Foundation/Foundation.h>

namespace lgp::platform {
namespace {

class UserDefaultsStore final : public ISettingsStore {
public:
    std::optional<std::string> get(const std::string& key) const override {
        id value = [[NSUserDefaults standardUserDefaults] objectForKey:@(key.c_str())];
        if ([value isKindOfClass:[NSString class]])
            return std::string([(NSString*)value UTF8String]);
        if ([value isKindOfClass:[NSNumber class]])  // a toggle without string values
            return std::string([(NSNumber*)value boolValue] ? "1" : "0");
        return std::nullopt;
    }
    void set(const std::string& key, const std::string& value) override {
        NSString* text = [NSString stringWithUTF8String:value.c_str()];
        [[NSUserDefaults standardUserDefaults] setObject:(text != nil ? text : @"") forKey:@(key.c_str())];
    }
};

}  // namespace

std::unique_ptr<ISettingsStore> system_settings() { return std::make_unique<UserDefaultsStore>(); }

}  // namespace lgp::platform
