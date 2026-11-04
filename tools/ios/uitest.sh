#!/bin/sh
# Drives the iOS app on a connected device through its XCUITest bundle (P7,
# docs/knowledge/ios-app-bauen.md) and exports the screenshots. Mac only; the
# build tree must be configured with -DLGP_IOS_UI_TESTS=ON (preset ios).
#
#   tools/ios/uitest.sh <build-dir> <device-id> <out-dir> '<script>' ['<app args>']
#
# <script>: steps for LgpUITests.m, e.g. 'wait:15;shot:start;tap:0.5,0.55;wait:3;shot:after'.
# <app args>: launch arguments (default "--app"), split at spaces, so no paths
# with spaces in them. Screenshots of the game are
# game images: keep <out-dir> below $LGP_PRIVATE_DIR.
set -eu
if [ $# -lt 4 ]; then
    sed -n '2,12p' "$0" >&2
    exit 2
fi
build=$1 device=$2 out=$3 script=$4 args=${5:---app}
target=${LGP_UITEST_APP:-lgp_game}
mkdir -p "$out"
result="$out/run.xcresult"
rm -rf "$result"
# xcodebuild hands TEST_RUNNER_<NAME> to the test runner as <NAME>. The app's
# scheme carries the UI test bundle as its testable (XCTEST_TESTEE).
# A failed test still has screenshots worth seeing: export them, then exit
# with xcodebuild's status.
status=0
TEST_RUNNER_LGP_UITEST_SCRIPT=$script TEST_RUNNER_LGP_UITEST_ARGS=$args \
    xcodebuild test -project "$build/lgp_recomp.xcodeproj" -scheme "$target" \
        -only-testing "${target}_uitests" \
        -configuration Release -destination "id=$device" -resultBundlePath "$result" \
        -allowProvisioningUpdates || status=$?
if [ -d "$result" ]; then
    xcrun xcresulttool export attachments --path "$result" --output-path "$out" || true
    echo "screenshots and manifest.json in $out"
fi
exit "$status"
