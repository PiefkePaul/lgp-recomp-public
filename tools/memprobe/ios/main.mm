// iOS/iPadOS/tvOS shell of the memory probe (P7.2a, P7.4): one screen with the
// log of the previous run (the system ends the probe by killing it) and a
// start button. The log is flushed per line and lives in the app's Documents
// folder on iOS (memprobe.log, visible in the Files app when
// UIFileSharingEnabled is set) and in Library/Caches on tvOS, which has no
// writable Documents folder (the Mac copies it with `xcrun devicectl device
// copy from --domain-type appDataContainer ...`).
#import <UIKit/UIKit.h>

#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>

#include "../memprobe.hpp"

@interface ProbeController : UIViewController
@property(nonatomic, strong) UITextView* text;
@property(nonatomic, strong) UIButton* start;
@property(nonatomic, strong) UISegmentedControl* fill;
@property(nonatomic) BOOL fileSync;
@property(nonatomic) unsigned long long touchMiB;
@property(nonatomic) BOOL native;
@property(nonatomic) BOOL exitWhenDone;
@end

// Folder for the file-backed guest block (file mode): Library/Caches on both,
// it takes files on tvOS too.
static std::string CacheDir() {
    NSString* dir = NSSearchPathForDirectoriesInDomains(NSCachesDirectory, NSUserDomainMask, YES).firstObject;
    return dir.fileSystemRepresentation;
}

static NSString* LogPath() {
#if TARGET_OS_TV
    const NSSearchPathDirectory dir = NSCachesDirectory;
#else
    const NSSearchPathDirectory dir = NSDocumentDirectory;
#endif
    NSString* docs = NSSearchPathForDirectoriesInDomains(dir, NSUserDomainMask, YES).firstObject;
    return [docs stringByAppendingPathComponent:@"memprobe.log"];
}

@implementation ProbeController

- (void)viewDidLoad {
    [super viewDidLoad];
#if TARGET_OS_TV
    self.view.backgroundColor = UIColor.blackColor;  // no systemBackgroundColor on tvOS
#else
    self.view.backgroundColor = UIColor.systemBackgroundColor;
#endif
    // file: random data into a guest block backed by a file (P7.4).
    self.fill = [[UISegmentedControl alloc] initWithItems:@[ @"random", @"zero", @"file" ]];
    self.fill.selectedSegmentIndex = 0;
    self.start = [UIButton buttonWithType:UIButtonTypeSystem];
#if LGP_MEMPROBE_IMLIMIT
    [self.start setTitle:@"Start (with increased-memory-limit)" forState:UIControlStateNormal];
#elif LGP_MEMPROBE_ENTITLED && TARGET_OS_TV
    [self.start setTitle:@"Start (with extended-virtual-addressing)" forState:UIControlStateNormal];
#elif LGP_MEMPROBE_ENTITLED
    [self.start setTitle:@"Start (with memory entitlements)" forState:UIControlStateNormal];
#else
    [self.start setTitle:@"Start (no entitlements)" forState:UIControlStateNormal];
#endif
    [self.start addTarget:self action:@selector(run) forControlEvents:UIControlEventPrimaryActionTriggered];
    self.text = [[UITextView alloc] init];
#if !TARGET_OS_TV
    self.text.editable = NO;  // tvOS text views are never editable (no such property)
#endif
    self.text.font = [UIFont monospacedSystemFontOfSize:12 weight:UIFontWeightRegular];
    NSString* previous = [NSString stringWithContentsOfFile:LogPath() encoding:NSUTF8StringEncoding error:nil];
    self.text.text = previous != nil ? [@"Previous run:\n" stringByAppendingString:previous] : @"No previous run.";
    UIStackView* stack = [[UIStackView alloc] initWithArrangedSubviews:@[ self.fill, self.start, self.text ]];
    stack.axis = UILayoutConstraintAxisVertical;
    stack.spacing = 8;
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    [self.view addSubview:stack];
    UILayoutGuide* g = self.view.safeAreaLayoutGuide;
    [NSLayoutConstraint activateConstraints:@[
        [stack.topAnchor constraintEqualToAnchor:g.topAnchor constant:8],
        [stack.bottomAnchor constraintEqualToAnchor:g.bottomAnchor constant:-8],
        [stack.leadingAnchor constraintEqualToAnchor:g.leadingAnchor constant:8],
        [stack.trailingAnchor constraintEqualToAnchor:g.trailingAnchor constant:-8],
    ]];
}

// Started without a tap (the Mac's runs: `xcrun devicectl device process
// launch --device <id> <bundle id> -- --autostart [--zero | --file [--file-sync]]`,
// or the environment variables LGP_MEMPROBE_AUTOSTART=1, LGP_MEMPROBE_ZERO=1,
// LGP_MEMPROBE_FILE=1, LGP_MEMPROBE_FILE_SYNC=1). `--touch MIB` (or
// LGP_MEMPROBE_TOUCH=MIB, either also starts the run) times the first write
// instead, files in Library/Caches (memprobe.hpp 5.). `--native` (or LGP_MEMPROBE_NATIVE=1)
// runs the native probe (memprobe.hpp 6.) instead. Such a run ends the
// app when it is done, so `devicectl … --console` returns.
- (void)viewDidAppear:(BOOL)animated {
    [super viewDidAppear:animated];
    static bool started = false;
    if (started)
        return;
    NSProcessInfo* info = NSProcessInfo.processInfo;
    NSArray<NSString*>* args = info.arguments;
    NSDictionary<NSString*, NSString*>* env = info.environment;
    // A touch run has no control on screen, so asking for one starts it too.
    const bool autostart = [args containsObject:@"--autostart"] || [env[@"LGP_MEMPROBE_AUTOSTART"] isEqualToString:@"1"] ||
                           [args containsObject:@"--touch"] || env[@"LGP_MEMPROBE_TOUCH"] != nil ||
                           [args containsObject:@"--native"] || [env[@"LGP_MEMPROBE_NATIVE"] isEqualToString:@"1"];
    if (!autostart)
        return;
    started = true;
    self.exitWhenDone = YES;
    if ([args containsObject:@"--zero"] || [env[@"LGP_MEMPROBE_ZERO"] isEqualToString:@"1"])
        self.fill.selectedSegmentIndex = 1;
    if ([args containsObject:@"--file"] || [env[@"LGP_MEMPROBE_FILE"] isEqualToString:@"1"])
        self.fill.selectedSegmentIndex = 2;
    self.native = [args containsObject:@"--native"] || [env[@"LGP_MEMPROBE_NATIVE"] isEqualToString:@"1"];
    self.fileSync = [args containsObject:@"--file-sync"] || [env[@"LGP_MEMPROBE_FILE_SYNC"] isEqualToString:@"1"];
    const NSUInteger touch = [args indexOfObject:@"--touch"];
    if (touch != NSNotFound && touch + 1 < args.count)
        self.touchMiB = strtoull(args[touch + 1].UTF8String, nullptr, 10);
    else if (env[@"LGP_MEMPROBE_TOUCH"] != nil)
        self.touchMiB = strtoull(env[@"LGP_MEMPROBE_TOUCH"].UTF8String, nullptr, 10);
    [self run];
}

- (void)append:(NSString*)line {
    self.text.text = [self.text.text stringByAppendingFormat:@"%@\n", line];
    [self.text scrollRangeToVisible:NSMakeRange(self.text.text.length, 0)];
}

- (void)run {
    self.start.enabled = NO;
    self.fill.enabled = NO;
    self.text.text = @"";
    lgp::memprobe::Options options;
    options.zero_fill = self.fill.selectedSegmentIndex == 1;
    if (self.fill.selectedSegmentIndex == 2) {
        options.file_dir = CacheDir();
        options.file_sync = self.fileSync;
    }
    if (self.touchMiB > 0) {
        options.touch_mib = self.touchMiB;
        options.file_dir = CacheDir();
    }
    auto log = std::make_shared<std::ofstream>(LogPath().fileSystemRepresentation, std::ios::trunc);
#if LGP_MEMPROBE_IMLIMIT
    *log << "build: with increased-memory-limit only (does the platform's profile accept it?)" << std::endl;
#elif LGP_MEMPROBE_ENTITLED && TARGET_OS_TV
    *log << "build: with extended-virtual-addressing (tvOS has no increased-memory-limit)" << std::endl;
#elif LGP_MEMPROBE_ENTITLED
    *log << "build: with increased-memory-limit and extended-virtual-addressing" << std::endl;
#else
    *log << "build: without memory entitlements" << std::endl;
#endif
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
      const auto out = [log, self](const std::string& line) {
          *log << line << std::endl;  // flushed: the last line survives jetsam
          NSString* s = [NSString stringWithUTF8String:line.c_str()];
          dispatch_async(dispatch_get_main_queue(), ^{
            [self append:s];
          });
      };
      if (self.native) {
          options.file_dir = CacheDir();
          lgp::memprobe::run_native_probe(options, out);
      } else if (options.touch_mib > 0)
          lgp::memprobe::run_touch(options, out);
      else
          lgp::memprobe::run(options, out);
      if (self.exitWhenDone) {
          log->flush();
          std::exit(0);
      }
      dispatch_async(dispatch_get_main_queue(), ^{
        self.start.enabled = YES;
        self.fill.enabled = YES;
      });
    });
}

@end

// UIScene life cycle: apps built with current SDKs fail to launch without it
// ("UIScene life cycle is required for apps built with this SDK").
@interface ProbeSceneDelegate : UIResponder <UIWindowSceneDelegate>
@property(nonatomic, strong) UIWindow* window;
@end

@implementation ProbeSceneDelegate
- (void)scene:(UIScene*)scene
    willConnectToSession:(UISceneSession*)session
                 options:(UISceneConnectionOptions*)options {
    if (![scene isKindOfClass:[UIWindowScene class]])
        return;
    self.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene*)scene];
    self.window.rootViewController = [[ProbeController alloc] init];
    [self.window makeKeyAndVisible];
}
@end

@interface ProbeAppDelegate : UIResponder <UIApplicationDelegate>
@end

@implementation ProbeAppDelegate
- (BOOL)application:(UIApplication*)app didFinishLaunchingWithOptions:(NSDictionary*)options {
    return YES;
}
- (UISceneConfiguration*)application:(UIApplication*)app
    configurationForConnectingSceneSession:(UISceneSession*)session
                                   options:(UISceneConnectionOptions*)options {
    UISceneConfiguration* config = [[UISceneConfiguration alloc] initWithName:@"Default"
                                                                  sessionRole:session.role];
    config.delegateClass = [ProbeSceneDelegate class];
    return config;
}
@end

int main(int argc, char* argv[]) {
    @autoreleasepool {
        return UIApplicationMain(argc, argv, nil, NSStringFromClass([ProbeAppDelegate class]));
    }
}
