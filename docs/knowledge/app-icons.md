# App icons per platform

Cross-game knowledge, as of 2026-10-20. Applies to every app that `lgp_add_boot_executable(… APP)` builds.

## Motif and source

An original motif: a turquoise ring pierced by a yellow lightning bolt, on a dark blue gradient; wide images
(Android TV banner, tvOS Top Shelf) show the letters "LGP" next to it. No game graphics and no marks of
others: no characters, no logo, no red-and-white ball with a band (the previous banner looked like that and has been replaced).

The motif is defined once in `tools/icon/gen_icons.py` (colors, ring, bolt polygon, letters as rectangles and
ring sectors). From it the script writes all files: vector shapes as text (SVG, Android VectorDrawable) and
raster images via signed distances with anti-aliasing (numpy, Pillow). The outputs are in the repository,
so that no build needs numpy or Pillow; every file is small (largest ≈ 100 KiB, guard limit 2 MiB).

```sh
python3 tools/icon/gen_icons.py            # rewrite everything (numpy, Pillow)
python3 tools/icon/gen_icons.py --check    # check only, pure Python
```

`--check` (ctest `tools.app_icons`) compares the text files byte for byte with the script and, for the images, checks
type, size, opacity and size on disk. Two runs with the same Pillow version produce the same bytes.

## Requirements and location

| Platform | Requirement | File | Wired in via |
|---|---|---|---|
| iOS/iPadOS | Asset catalog; since Xcode 14 a single 1024 × 1024 image is enough ("Single Size", `platform: ios`, `idiom: universal`), Xcode generates the other sizes. Opaque (no alpha), square: iOS rounds it itself. | `platform/ios/Assets.xcassets/AppIcon.appiconset/AppIcon-1024.png` | `lgp_ios_app()` in `cmake/LgpIos.cmake`: catalog as a source with `MACOSX_PACKAGE_LOCATION Resources`, `ASSETCATALOG_COMPILER_APPICON_NAME=AppIcon`; actool adds `CFBundleIcons` to the Info.plist itself. |
| tvOS | "Brand Assets": app icon as an image stack with 2–5 layers (parallax), 400 × 240 (@1x) and 800 × 480 (@2x), plus 1280 × 768 for the App Store (`tv-marketing`); rearmost layer opaque, the others with alpha. Top Shelf image 1920 × 720 / 3840 × 1440 and wide 2320 × 720 / 4640 × 1440. | `platform/tvos/Assets.xcassets/AppIcon.brandassets/` (layers Back = gradient, Middle = ring, Front = bolt; `TopShelf*.imageset`) | the same place; under tvOS `AppIcon` points to the Brand Assets, actool sets the icon and Top Shelf keys. Names without spaces, Xcode does not require the template names. |
| macOS | `.icns` in `Contents/Resources`, `CFBundleIconFile`. Apple grid: body 824 of 1024 px, the rest transparent. | `platform/macos/lgp.icns` (ic07–ic14) | only when `lgp_game` is an app bundle (`-DCMAKE_MACOSX_BUNDLE=ON`): `MACOSX_BUNDLE_ICON_FILE`. Without a bundle, the window sets the Dock icon (below). |
| Windows | `.ico` with 16–256 px as the first icon resource of the exe | `platform/windows/lgp.ico`, `platform/windows/lgp.rc` | `lgp_add_boot_executable(… APP)` adds `lgp.rc` as soon as CMake has an RC compiler (MSVC `rc`, clang `llvm-rc`). Explorer, the taskbar and SDL's window class take the first icon of the exe. |
| Android | Adaptive icon from API 26 (minimum API here 29): background and foreground each 108 × 108 dp, at least the 66 dp circle in the middle is visible; a vector is enough, no PNG density levels needed. `android:roundIcon` for older launchers. | `platform/android/res/mipmap-anydpi-v26/ic_launcher*.xml`, `res/drawable/ic_launcher_{background,foreground}.xml` | `android:icon`/`android:roundIcon` in `platform/android/AndroidManifest.xml.in`; `build_apk.py` compiles `res/` with aapt2. |
| Android TV | Leanback banner 320 × 180 dp with name | `platform/android/res/drawable/banner.xml` | `android:banner` (as before) |
| Browser | Favicon; SVG for Chromium/Firefox, `apple-touch-icon` (180 px, opaque) for Safari/iOS | `platform/icon/lgp-icon.svg`, `platform/icon/lgp-icon-180.png` | `game.html.in` (`<link rel="icon">`), `lgp_add_web_page()` copies both next to the page. |
| Window (SDL) | Title bar, Linux window list, macOS Dock without an app bundle | `platform/icon/lgp-icon-128.png` | `platform/CMakeLists.txt` embeds the bytes into `lgp_platform_desktop`, `SdlWindow::create` sets them with `SDL_LoadPNG_IO` + `SDL_SetWindowIcon` (SDL ≥ 3.4). Not under iOS/tvOS/Android. |

## Pitfalls

- **Alpha on iOS:** the App Store rejects an icon with an alpha channel; on the device iOS otherwise shows black corners.
  `--check` therefore requires RGB without alpha for the iOS icon, the rearmost tvOS layer and the Top Shelf image.
- **Info.plist:** the icon keys come from the asset catalog (actool writes a partial plist, Xcode merges it with
  `MACOSX_BUNDLE_INFO_PLIST`). `platform/ios|tvos/Info.plist.in` needs no icon entries.
- **Android gradient:** the background and the gap around the bolt use the same gradient (`aapt:attr` gradient in
  viewport coordinates), so the gap is invisible.

## Own image for local test builds (`LGP_APP_ICON`)

For test devices that also hold the original game, a local iOS build of `lgp_game` can carry a different image:
`-DLGP_APP_ICON=<image.png|jpg>`, or without the option `$LGP_PRIVATE_DIR/app-icon.png` (or `.jpg`), if
present. CMake then creates its own asset catalog in the build folder and scales the image with `sips` to
1024 px (a 256 image becomes blurry in the process). If the image comes from your own dump (the game icon is stored as JPEG
`icon_<language>.dat` in the control NCA), it is game data: build only locally, never into the repo, never in CI. iOS/iPadOS only;
tvOS keeps its layered icon.

## Later extension: Icon Composer (`.icon`)

Xcode 26 (iOS/iPadOS/macOS 26) additionally knows the format `AppIcon.icon` from Icon Composer (layers with
Liquid Glass effects, light/dark/tinted variants). The asset catalog with a 1024 image is still
supported and is enough here; an `.icon` built from the same layers (gradient, ring, bolt) would be a possible later
addition.

## Verification status

Checked here: files, sizes, catalogs (`tools.app_icons`), vector shapes rendered (cairosvg) and raster images
looked at. Not checked: Xcode/actool (no Mac here), aapt2 (no Android SDK here), Windows resource, display on
iPhone, iPad, Apple TV, Shield and under Windows.
