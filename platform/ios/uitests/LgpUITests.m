// UI tests for the iOS app (P7, docs/knowledge/ios-app-bauen.md): taps,
// typing and screenshots the way a person drives the app, from the Mac with
// `xcodebuild test` (tools/ios/uitest.sh). The app draws everything itself
// (Dear ImGui, the game image), so there are no accessibility elements to
// find: steps address the screen by normalized coordinates (0..1, origin top
// left, the app is landscape).
//
// The script comes from the environment; xcodebuild hands TEST_RUNNER_<NAME>
// to the test runner as <NAME>:
//   LGP_UITEST_ARGS    launch arguments, space separated (default "--app")
//   LGP_UITEST_SCRIPT  steps separated by ';' (default "wait:10;shot:start"):
//     wait:SEC                 sleep (the app keeps running)
//     tap:X,Y                  one tap
//     hold:X,Y,SEC             press and hold
//     drag:X1,Y1,X2,Y2[,SEC]   press, then drag (SEC: hold before moving)
//     type:TEXT                type into the focused text input ("\n" = Return)
//     shot:NAME                screenshot, kept in the result bundle as NAME
//     home                     the device's home action (app to the background)
//     activate                 bring the app back to the foreground
//     remote:BUTTON[,SEC]      tvOS only: a Siri Remote button (up, down, left,
//                              right, select, menu, playpause, home), pressed
//                              for SEC seconds if given; these presses do
//                              not reach the GameController framework, so SDL
//                              and the game never see them (Apple TV, tvOS
//                              27.2): good for the system, not as game input
// On tvOS there is no touch screen: tap, hold, drag and type fail there, home
// is the remote's TV button.
// After the last step a screenshot "end" is taken; the app keeps its data
// (an update install, no uninstall).
#import <XCTest/XCTest.h>

@interface LgpUITests : XCTestCase
@end

@implementation LgpUITests

#if !TARGET_OS_TV
static NSArray<NSNumber*>* numbers(NSString* text) {
    NSMutableArray<NSNumber*>* out = [NSMutableArray array];
    for (NSString* part in [text componentsSeparatedByString:@","])
        [out addObject:@([part doubleValue])];
    return out;
}

static XCUICoordinate* point(XCUIApplication* app, double x, double y) {
    return [app coordinateWithNormalizedOffset:CGVectorMake(x, y)];
}
#else
// "up" -> XCUIRemoteButtonUp; NO for an unknown name.
static BOOL remote_button(NSString* name, XCUIRemoteButton* out) {
    NSDictionary<NSString*, NSNumber*>* map = @{
        @"up": @(XCUIRemoteButtonUp), @"down": @(XCUIRemoteButtonDown), @"left": @(XCUIRemoteButtonLeft),
        @"right": @(XCUIRemoteButtonRight), @"select": @(XCUIRemoteButtonSelect), @"menu": @(XCUIRemoteButtonMenu),
        @"playpause": @(XCUIRemoteButtonPlayPause), @"home": @(XCUIRemoteButtonHome),
    };
    NSNumber* b = map[name.lowercaseString];
    if (b == nil)
        return NO;
    *out = (XCUIRemoteButton)b.unsignedIntegerValue;
    return YES;
}
#endif

- (void)screenshot:(NSString*)name {
    XCTAttachment* shot = [XCTAttachment attachmentWithScreenshot:XCUIScreen.mainScreen.screenshot];
    shot.name = name;
    shot.lifetime = XCTAttachmentLifetimeKeepAlways;
    [self addAttachment:shot];
}

- (void)testScript {
    self.continueAfterFailure = NO;
    NSDictionary<NSString*, NSString*>* env = NSProcessInfo.processInfo.environment;
    XCUIApplication* app = [[XCUIApplication alloc] init];
    // Split at spaces: no quoting, so an argument cannot contain a space.
    NSMutableArray<NSString*>* args = [NSMutableArray array];
    for (NSString* arg in [(env[@"LGP_UITEST_ARGS"] ?: @"--app") componentsSeparatedByString:@" "])
        if (arg.length != 0)
            [args addObject:arg];
    app.launchArguments = args;
    [app launch];

    NSString* script = env[@"LGP_UITEST_SCRIPT"] ?: @"wait:10;shot:start";
    for (NSString* raw in [script componentsSeparatedByString:@";"]) {
        NSString* step = [raw stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
        if (step.length == 0)
            continue;
        NSRange colon = [step rangeOfString:@":"];
        NSString* verb = colon.location == NSNotFound ? step : [step substringToIndex:colon.location];
        NSString* arg = colon.location == NSNotFound ? @"" : [step substringFromIndex:colon.location + 1];
        NSLog(@"lgp uitest: %@", step);
        if ([verb isEqualToString:@"wait"]) {
            [NSThread sleepForTimeInterval:[arg doubleValue]];
#if !TARGET_OS_TV
        } else if ([verb isEqualToString:@"tap"]) {
            NSArray<NSNumber*>* v = numbers(arg);
            XCTAssertTrue(v.count == 2, @"tap:X,Y");
            [point(app, v[0].doubleValue, v[1].doubleValue) tap];
        } else if ([verb isEqualToString:@"hold"]) {
            NSArray<NSNumber*>* v = numbers(arg);
            XCTAssertTrue(v.count == 3, @"hold:X,Y,SEC");
            [point(app, v[0].doubleValue, v[1].doubleValue) pressForDuration:v[2].doubleValue];
        } else if ([verb isEqualToString:@"drag"]) {
            NSArray<NSNumber*>* v = numbers(arg);
            XCTAssertTrue(v.count == 4 || v.count == 5, @"drag:X1,Y1,X2,Y2[,SEC]");
            [point(app, v[0].doubleValue, v[1].doubleValue)
                  pressForDuration:(v.count == 5 ? v[4].doubleValue : 0.1)
                thenDragToCoordinate:point(app, v[2].doubleValue, v[3].doubleValue)];
        } else if ([verb isEqualToString:@"type"]) {
            [app typeText:[arg stringByReplacingOccurrencesOfString:@"\\n" withString:@"\n"]];
        } else if ([verb isEqualToString:@"home"]) {
            [XCUIDevice.sharedDevice pressButton:XCUIDeviceButtonHome];
#else
        } else if ([verb isEqualToString:@"remote"]) {
            NSArray<NSString*>* parts = [arg componentsSeparatedByString:@","];
            XCUIRemoteButton button = XCUIRemoteButtonSelect;
            XCTAssertTrue(parts.count <= 2 && remote_button(parts[0], &button), @"remote:BUTTON[,SEC], got '%@'", arg);
            if (parts.count == 2)
                [[XCUIRemote sharedRemote] pressButton:button forDuration:parts[1].doubleValue];
            else
                [[XCUIRemote sharedRemote] pressButton:button];
        } else if ([verb isEqualToString:@"home"]) {
            [[XCUIRemote sharedRemote] pressButton:XCUIRemoteButtonHome];
#endif
        } else if ([verb isEqualToString:@"shot"]) {
            [self screenshot:arg.length != 0 ? arg : @"shot"];
        } else if ([verb isEqualToString:@"activate"]) {
            [app activate];
        } else {
            XCTFail(@"unknown step '%@'", step);
        }
    }
    [self screenshot:@"end"];
}

@end
