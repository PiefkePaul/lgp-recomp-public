# iOS/iPadOS and tvOS app bundles (P7.2, P7.4, ADR-0007/0008). Only with
# CMAKE_SYSTEM_NAME=iOS or tvOS and the Xcode generator (presets "ios",
# "tvos"): Xcode signs the app with the team's
# automatic provisioning, which also grants the memory entitlements
# (docs/knowledge/speicherprobe-mobil.md).
#
#   LGP_IOS_TEAM           Apple development team id (Xcode > Settings > Accounts)
#   LGP_IOS_BUNDLE_PREFIX  bundle id prefix, unique per team
#
# lgp_ios_app(<target> ID <suffix> NAME <shown name>) turns an executable into
# an app bundle <prefix>.<suffix> with platform/ios/Info.plist.in (SDL's scene
# delegate, landscape, Documents visible in the Files app) and
# platform/ios/lgp_app.entitlements (increased-memory-limit and
# extended-virtual-addressing: without them the guest block leaves only about
# 1.6 GiB, docs/analysis/25-p7-speicherprobe-geraete.md).
# tvOS takes platform/tvos/ instead: Apple TV device family, no orientations
# or Files app. Its entitlements are written at configure time: none by
# default (Apple offers no increased-memory-limit there; with
# extended-virtual-addressing the memory probe was killed after ≈ 30 s,
# docs/analysis/28-tvos-machbarkeit.md; LGP_TVOS_EXTENDED_VA=ON signs with it
# anyway, for that test). With CLOUD_SAVES both platforms add the CloudKit
# keys for the container iCloud.<bundle id> (the saves' backup,
# lgp/platform/cloud_save.hpp): iOS and tvOS share the bundle id and with it
# the container, so saves move between iPhone, iPad and Apple TV; each kind of
# device keeps its own settings (LGP_CLOUD_SETTINGS_TAG, lgp/boot/save_backup.hpp).
# LGP_TVOS_CLOUD_SAVES=OFF / LGP_IOS_CLOUD_SAVES=OFF build without, e.g. for a
# team without iCloud.
# LGP_APPLE_GAME_MODE=ON marks the apps as games (LSApplicationCategoryType
# public.app-category.games, GCSupportsGameMode, LSSupportsGameMode): iOS and
# iPadOS 18+ may then run them in Game Mode (Apple: highest CPU/GPU priority,
# less background work, Bluetooth sampled twice as often). On by default
# (Paul's decision, 2026-10-21, without an ABAB measurement; OFF builds
# without the keys). tvOS has none (Apple documents the keys for iOS,
# iPadOS and macOS only; on an Apple TV the overlay showed Game Mode off).
# LGP_IOS_MEMORY_ENTITLEMENTS picks the memory entitlements of iOS apps: both
# (default), limit (increased-memory-limit only: a free Apple ID's Personal
# Team, which Apple refuses extended-virtual-addressing, Mac test 2026-10-25)
# or none (docs/knowledge/ios-ohne-mac.md).
#
# Without Xcode (preset ios-cross, cmake/toolchains/ios-cross.cmake,
# LGP_IOS_CROSS): Ninja builds an unsigned .app. There is no actool, so the
# icon goes in as loose PNGs (tools/ios/loose_icons.py, CFBundleIcons) and the
# Info.plist gets the keys Xcode would add (MinimumOSVersion, UIDeviceFamily,
# ...). The entitlements land in <build>/<target>.entitlements for the signer
# (tools/ios/make_ipa.py, xtool). Draft, not yet run with a real SDK.

# ccache: the Xcode generator ignores CMAKE_CXX_COMPILER_LAUNCHER, so Xcode's
# compiler settings point at wrapper scripts instead (they also pass links
# through). Check with `ccache -s` after a rebuild; off with -DLGP_IOS_CCACHE=OFF.
find_program(LGP_CCACHE_PROGRAM ccache)
option(LGP_IOS_CCACHE "Compile the iOS build through ccache (needs ccache in PATH)" ON)
if(LGP_IOS_CCACHE AND LGP_CCACHE_PROGRAM AND CMAKE_GENERATOR STREQUAL "Xcode")
    foreach(_tool clang clang++)
        set(_wrapper "${CMAKE_BINARY_DIR}/ccache-${_tool}")
        file(WRITE "${_wrapper}" "#!/bin/sh\nexec \"${LGP_CCACHE_PROGRAM}\" \"$(xcrun -f ${_tool})\" \"$@\"\n")
        file(CHMOD "${_wrapper}" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE
                                             WORLD_READ WORLD_EXECUTE)
    endforeach()
    set(CMAKE_XCODE_ATTRIBUTE_CC "${CMAKE_BINARY_DIR}/ccache-clang")
    set(CMAKE_XCODE_ATTRIBUTE_CXX "${CMAKE_BINARY_DIR}/ccache-clang++")
    set(CMAKE_XCODE_ATTRIBUTE_LD "${CMAKE_BINARY_DIR}/ccache-clang")
    set(CMAKE_XCODE_ATTRIBUTE_LDPLUSPLUS "${CMAKE_BINARY_DIR}/ccache-clang++")
    # The index store path changes per build and would defeat the cache.
    set(CMAKE_XCODE_ATTRIBUTE_COMPILER_INDEX_STORE_ENABLE "NO")
    message(STATUS "iOS: compiling through ${LGP_CCACHE_PROGRAM}")
endif()

set(LGP_IOS_TEAM "" CACHE STRING "Apple development team id for signing (Xcode: Settings > Accounts)")
set(LGP_IOS_BUNDLE_PREFIX "de.piefkepaul.lgp" CACHE STRING "Bundle id prefix (must be unique per Apple team)")
option(LGP_TVOS_EXTENDED_VA "tvOS: sign apps with extended-virtual-addressing (killed the memory probe, test only)" OFF)
option(LGP_TVOS_CLOUD_SAVES "tvOS: back up lgp_game's saves to iCloud (CloudKit entitlement)" ON)
option(LGP_IOS_CLOUD_SAVES "iOS/iPadOS: back up lgp_game's saves to iCloud, shared with tvOS (CloudKit entitlement)" ON)
option(LGP_APPLE_GAME_MODE "iOS/tvOS: mark the apps as games, eligible for Game Mode" ON)
set(LGP_IOS_MEMORY_ENTITLEMENTS both CACHE STRING
    "iOS memory entitlements: both (increased-memory-limit, extended-virtual-addressing), limit (the first only), none")
set_property(CACHE LGP_IOS_MEMORY_ENTITLEMENTS PROPERTY STRINGS both limit none)
if(NOT LGP_IOS_MEMORY_ENTITLEMENTS MATCHES "^(both|limit|none)$")
    message(FATAL_ERROR "LGP_IOS_MEMORY_ENTITLEMENTS must be both, limit or none, not '${LGP_IOS_MEMORY_ENTITLEMENTS}'")
endif()
if(LGP_IOS_CROSS AND CMAKE_GENERATOR STREQUAL "Xcode")
    message(FATAL_ERROR "ios-cross builds without Xcode; take preset ios on a Mac instead")
endif()
# A local picture as lgp_game's icon instead of our own (Paul, 2026-10-20:
# the game's icon on his test devices). It comes from the user's own dump
# (game data): never in the repository or CI, like LGP_APP_GAME_DIR. Without
# the option a local build takes $LGP_PRIVATE_DIR/app-icon.png (or .jpg) if
# it exists. iOS/iPadOS only; tvOS keeps its layered icon.
set(LGP_APP_ICON "" CACHE FILEPATH "Picture (PNG/JPEG) for lgp_game's iOS icon; local builds only")

function(lgp_ios_app target)
    cmake_parse_arguments(PARSE_ARGV 1 arg "CLOUD_SAVES" "ID;NAME" "")
    if(LGP_TVOS)
        set(_ios "${PROJECT_SOURCE_DIR}/platform/tvos")
        set(_family "3")
    else()
        set(_ios "${PROJECT_SOURCE_DIR}/platform/ios")
        set(_family "1,2")
    endif()
    set(_entitlements "${_ios}/lgp_app.entitlements")
    if(LGP_TVOS)
        set(_cloud ${LGP_TVOS_CLOUD_SAVES})
        set(_tag "tvos")
    else()
        set(_cloud ${LGP_IOS_CLOUD_SAVES})
        set(_tag "ios")
    endif()
    set(_cloud_keys "")
    if(arg_CLOUD_SAVES AND _cloud)
        # A CloudKit container cannot be deleted: one per app, named after it.
        set(_container "iCloud.${LGP_IOS_BUNDLE_PREFIX}.${arg_ID}")
        string(APPEND _cloud_keys
            "\t<key>com.apple.developer.icloud-services</key>\n\t<array>\n\t\t<string>CloudKit</string>\n\t</array>\n"
            "\t<key>com.apple.developer.icloud-container-identifiers</key>\n\t<array>\n"
            "\t\t<string>${_container}</string>\n\t</array>\n")
        target_compile_definitions(${target} PRIVATE "LGP_CLOUD_CONTAINER=\"${_container}\""
                                                     "LGP_CLOUD_SETTINGS_TAG=\"${_tag}\"")
    endif()
    if(NOT LGP_TVOS AND NOT LGP_IOS_MEMORY_ENTITLEMENTS STREQUAL "both")
        set(_keys "${_cloud_keys}")
        if(LGP_IOS_MEMORY_ENTITLEMENTS STREQUAL "limit")
            string(PREPEND _keys "\t<key>com.apple.developer.kernel.increased-memory-limit</key>\n\t<true/>\n")
        endif()
        set(_entitlements "${CMAKE_CURRENT_BINARY_DIR}/${target}.entitlements")
        file(CONFIGURE OUTPUT "${_entitlements}" CONTENT
             "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n<plist version=\"1.0\">\n<dict>\n${_keys}</dict>\n</plist>\n")
    elseif(NOT LGP_TVOS AND _cloud_keys)
        # The fixed memory entitlements plus the CloudKit keys.
        file(READ "${_entitlements}" _text)
        string(REPLACE "</dict>" "${_cloud_keys}</dict>" _text "${_text}")
        set(_entitlements "${CMAKE_CURRENT_BINARY_DIR}/${target}.entitlements")
        file(CONFIGURE OUTPUT "${_entitlements}" CONTENT "${_text}" @ONLY)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_ios}/lgp_app.entitlements")
    endif()
    if(LGP_TVOS)
        set(_keys "${_cloud_keys}")
        if(LGP_TVOS_EXTENDED_VA)
            string(PREPEND _keys "\t<key>com.apple.developer.kernel.extended-virtual-addressing</key>\n\t<true/>\n")
        endif()
        set(_entitlements "${CMAKE_CURRENT_BINARY_DIR}/${target}.entitlements")
        file(CONFIGURE OUTPUT "${_entitlements}" CONTENT
             "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n<plist version=\"1.0\">\n<dict>\n${_keys}</dict>\n</plist>\n")
    endif()
    # The template with this build's optional keys (@...@ only; the
    # ${MACOSX_BUNDLE_...} references stay for CMake's own bundle step).
    set(LGP_PLIST_GAME_MODE "")
    if(LGP_APPLE_GAME_MODE)
        set(LGP_PLIST_GAME_MODE
            "\t<key>LSApplicationCategoryType</key>\n\t<string>public.app-category.games</string>\n"
            "\t<key>GCSupportsGameMode</key>\n\t<true/>\n\t<key>LSSupportsGameMode</key>\n\t<true/>\n")
        string(JOIN "" LGP_PLIST_GAME_MODE ${LGP_PLIST_GAME_MODE})
    endif()
    if(LGP_IOS_CROSS)
        # What Xcode adds on its own; without MinimumOSVersion, UIDeviceFamily and the platform keys iOS refuses
        # the app. The icon keys name the loose PNGs (no asset catalog without actool).
        string(REPLACE "," "</integer><integer>" _families "${_family}")
        string(APPEND LGP_PLIST_GAME_MODE
            "\t<key>MinimumOSVersion</key>\n\t<string>${CMAKE_OSX_DEPLOYMENT_TARGET}</string>\n"
            "\t<key>UIDeviceFamily</key>\n\t<array><integer>${_families}</integer></array>\n"
            "\t<key>CFBundleSupportedPlatforms</key>\n\t<array><string>iPhoneOS</string></array>\n"
            "\t<key>DTPlatformName</key>\n\t<string>iphoneos</string>\n"
            "\t<key>UIRequiredDeviceCapabilities</key>\n\t<array><string>arm64</string></array>\n"
            "\t<key>CFBundleIcons</key>\n\t<dict><key>CFBundlePrimaryIcon</key><dict><key>CFBundleIconFiles</key>"
            "<array><string>AppIcon60x60</string></array></dict></dict>\n"
            "\t<key>CFBundleIcons~ipad</key>\n\t<dict><key>CFBundlePrimaryIcon</key><dict><key>CFBundleIconFiles</key>"
            "<array><string>AppIcon60x60</string><string>AppIcon76x76</string><string>AppIcon83.5x83.5</string></array>"
            "</dict></dict>\n")
    endif()
    set(_plist "${CMAKE_CURRENT_BINARY_DIR}/${target}.Info.plist.in")
    configure_file("${_ios}/Info.plist.in" "${_plist}" @ONLY)
    set_target_properties(${target} PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_INFO_PLIST "${_plist}"
        MACOSX_BUNDLE_BUNDLE_NAME "${arg_NAME}"
        MACOSX_BUNDLE_GUI_IDENTIFIER "${LGP_IOS_BUNDLE_PREFIX}.${arg_ID}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_BUNDLE_VERSION "1"
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "${LGP_IOS_BUNDLE_PREFIX}.${arg_ID}"
        XCODE_ATTRIBUTE_CODE_SIGN_STYLE "Automatic"
        XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "${LGP_IOS_TEAM}"
        XCODE_ATTRIBUTE_CODE_SIGN_ENTITLEMENTS "${_entitlements}"
        XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "${_family}"
        # Lifted code is large: plain DWARF in the objects, no dSYM step.
        XCODE_ATTRIBUTE_DEBUG_INFORMATION_FORMAT "dwarf"
        XCODE_ATTRIBUTE_DEAD_CODE_STRIPPING "YES")
    # CMake configures the plist when it generates the Xcode project; without
    # this a changed template only reaches the app after a manual configure.
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_ios}/Info.plist.in")
    # App icon (tools/icon/gen_icons.py): Xcode compiles the asset catalog
    # with actool and adds the icon keys (tvOS: also the Top Shelf image) to
    # the bundle's Info.plist. iOS: one 1024 px icon; tvOS: brand assets with
    # a layered icon.
    set(_assets "${_ios}/Assets.xcassets")
    set(_icon "${LGP_APP_ICON}")
    file(TO_CMAKE_PATH "$ENV{LGP_PRIVATE_DIR}" _private_dir)
    foreach(_ext png jpg jpeg)
        if(NOT _icon AND NOT _private_dir STREQUAL "" AND EXISTS "${_private_dir}/app-icon.${_ext}")
            set(_icon "${_private_dir}/app-icon.${_ext}")
        endif()
    endforeach()
    # For an ad-hoc signer (tools/ios/make_ipa.py: ios-cross, or a Mac build for a sideloader): the entitlements
    # this build asks for, at the top of the build tree.
    configure_file("${_entitlements}" "${CMAKE_BINARY_DIR}/${target}.entitlements" COPYONLY)
    if(LGP_IOS_CROSS)
        # Loose icons from the 1024 px PNG of the catalog (or LGP_APP_ICON), made at configure time.
        set(_source_icon "${_ios}/Assets.xcassets/AppIcon.appiconset/AppIcon-1024.png")
        if(_icon AND target STREQUAL "lgp_game")
            set(_source_icon "${_icon}")
        endif()
        set(_loose "${CMAKE_CURRENT_BINARY_DIR}/${target}.icons")
        execute_process(COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/ios/loose_icons.py"
                                "${_source_icon}" "${_loose}"
                        RESULT_VARIABLE _rc OUTPUT_VARIABLE _files ERROR_VARIABLE _err OUTPUT_STRIP_TRAILING_WHITESPACE)
        if(NOT _rc EQUAL 0)
            message(FATAL_ERROR "${target}: loose icons from ${_source_icon}: ${_err}")
        endif()
        string(REPLACE "\n" ";" _files "${_files}")
        foreach(_f IN LISTS _files)
            target_sources(${target} PRIVATE "${_loose}/${_f}")
            set_source_files_properties("${_loose}/${_f}" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
        endforeach()
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_source_icon}")
        if(target STREQUAL "lgp_game")
            set(_settings "${_ios}/Settings.bundle/Root.plist")
            target_sources(${target} PRIVATE "${_settings}")
            set_source_files_properties("${_settings}" PROPERTIES MACOSX_PACKAGE_LOCATION Resources/Settings.bundle)
        endif()
        return()
    endif()
    if(_icon AND target STREQUAL "lgp_game" AND NOT LGP_TVOS)
        if(NOT EXISTS "${_icon}" OR IS_DIRECTORY "${_icon}")
            message(FATAL_ERROR "LGP_APP_ICON: ${_icon} is not a file")
        endif()
        # actool wants a 1024 px PNG; sips (macOS) scales and converts.
        set(_assets "${CMAKE_CURRENT_BINARY_DIR}/${target}.xcassets")
        set(_set "${_assets}/AppIcon.appiconset")
        file(MAKE_DIRECTORY "${_set}")
        configure_file("${_ios}/Assets.xcassets/Contents.json" "${_assets}/Contents.json" COPYONLY)
        configure_file("${_ios}/Assets.xcassets/AppIcon.appiconset/Contents.json" "${_set}/Contents.json" COPYONLY)
        execute_process(COMMAND sips -s format png -z 1024 1024 "${_icon}" --out "${_set}/AppIcon-1024.png"
                        RESULT_VARIABLE _rc OUTPUT_QUIET ERROR_VARIABLE _err)
        if(NOT _rc EQUAL 0)
            message(FATAL_ERROR "LGP_APP_ICON: sips could not convert ${_icon}: ${_err}")
        endif()
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_icon}")
        message(STATUS "${target}: app icon from ${_icon} (local build only)")
    endif()
    target_sources(${target} PRIVATE "${_assets}")
    set_source_files_properties("${_assets}" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    # The plist is English; German for a German device from de.lproj (the
    # app's own menus: lgp/platform/ui_text.hpp). Xcode appends a source's
    # *.lproj folder to any location (de.lproj/de.lproj), so the files are
    # copied to a folder not named *.lproj and the location names de.lproj.
    set(_l10n "${CMAKE_CURRENT_BINARY_DIR}/l10n-de")
    configure_file("${_ios}/de.lproj/InfoPlist.strings" "${_l10n}/InfoPlist.strings" COPYONLY)
    set(_info_de "${_l10n}/InfoPlist.strings")
    target_sources(${target} PRIVATE "${_info_de}")
    set_source_files_properties("${_info_de}" PROPERTIES MACOSX_PACKAGE_LOCATION Resources/de.lproj)
    # Settings > Apps > LGP (iPhone/iPad; tvOS has no app settings in its
    # Settings app): lgp/platform/system_settings.hpp. Not yet seen on a device.
    if(target STREQUAL "lgp_game" AND NOT LGP_TVOS)
        set(_settings "${_ios}/Settings.bundle/Root.plist")
        configure_file("${_ios}/Settings.bundle/de.lproj/Root.strings" "${_l10n}/Root.strings" COPYONLY)
        set(_settings_de "${_l10n}/Root.strings")
        target_sources(${target} PRIVATE "${_settings}" "${_settings_de}")
        set_source_files_properties("${_settings}" PROPERTIES MACOSX_PACKAGE_LOCATION Resources/Settings.bundle)
        set_source_files_properties("${_settings_de}" PROPERTIES MACOSX_PACKAGE_LOCATION
                                                                Resources/Settings.bundle/de.lproj)
    endif()
    set_target_properties(${target} PROPERTIES XCODE_ATTRIBUTE_ASSETCATALOG_COMPILER_APPICON_NAME "AppIcon")
    if(NOT LGP_IOS_TEAM)
        message(WARNING "${target}: LGP_IOS_TEAM is empty; Xcode cannot sign the app for a device")
    endif()
endfunction()

# UI tests (P7): lgp_ios_ui_tests(<app target>) adds <app>_uitests, an XCUITest
# bundle (platform/ios/uitests/LgpUITests.m) that launches the app and runs a
# tap/type/screenshot script from the environment; the Mac runs it with
# tools/ios/uitest.sh (xcodebuild test). Off unless -DLGP_IOS_UI_TESTS=ON: the
# extra target is only needed on the Mac that drives the device.
option(LGP_IOS_UI_TESTS "Add an XCUITest bundle next to each iOS app (tools/ios/uitest.sh)" OFF)
if(LGP_IOS_UI_TESTS)
    enable_language(OBJC)  # file scope only, not inside the function
endif()
function(lgp_ios_ui_tests app)
    if(NOT LGP_IOS_UI_TESTS)
        return()
    endif()
    find_package(XCTest REQUIRED)
    set(_tests ${app}_uitests)
    get_target_property(_app_id ${app} XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER)
    # Not xctest_add_bundle(): that one builds a unit test bundle hosted in the
    # app (TEST_HOST, BUNDLE_LOADER, output into the app's PlugIns). A UI test
    # bundle runs in its own runner app that Xcode makes and drives the app
    # from outside, so the game app stays as it is.
    set(_source "${PROJECT_SOURCE_DIR}/platform/ios/uitests/LgpUITests.m")
    # The same bundle serves tvOS (remote steps instead of taps).
    if(LGP_TVOS)
        set(_family "3")
    else()
        set(_family "1,2")
    endif()
    add_library(${_tests} MODULE "${_source}")
    # enable_language(OBJC) above does not reach the directory that calls this
    # function, so Xcode would compile the .m as C (-x c); name the type.
    set_source_files_properties("${_source}" PROPERTIES XCODE_EXPLICIT_FILE_TYPE sourcecode.c.objc)
    target_link_libraries(${_tests} PRIVATE "-framework Foundation" ${XCTest_LIBRARIES})
    target_include_directories(${_tests} PRIVATE ${XCTest_INCLUDE_DIRS})
    add_dependencies(${_tests} ${app})
    set_target_properties(${_tests} PROPERTIES
        BUNDLE TRUE
        XCTEST TRUE
        # CMake lists an XCTest bundle under Test in the scheme of its
        # XCTEST_TESTEE (the app's scheme), not in its own scheme.
        XCTEST_TESTEE "${app}"
        XCODE_PRODUCT_TYPE "com.apple.product-type.bundle.ui-testing"
        XCODE_ATTRIBUTE_TEST_TARGET_NAME "${app}"
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "${_app_id}.uitests"
        MACOSX_BUNDLE_GUI_IDENTIFIER "${_app_id}.uitests"
        XCODE_ATTRIBUTE_CODE_SIGN_STYLE "Automatic"
        XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "${LGP_IOS_TEAM}"
        XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "${_family}"
        XCODE_ATTRIBUTE_CLANG_ENABLE_OBJC_ARC "YES"
        XCODE_GENERATE_SCHEME TRUE)
    set_target_properties(${app} PROPERTIES XCODE_GENERATE_SCHEME TRUE)
endfunction()
