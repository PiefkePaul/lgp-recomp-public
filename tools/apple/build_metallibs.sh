#!/bin/sh
# Compiles the MSL that wgpu wrote with LGP_WGPU_METAL_DUMP_MSL=<dir> (needs
# wgpu-native with tools/wgpu/metal-precompiled.patch) into one .metallib per
# file, for LGP_WGPU_METAL_LIBS=<out> or <game>/metal-<platform>/
# (docs/analysis/46-metal-shader-vorkompilieren.md):
#
#   tools/apple/build_metallibs.sh <msl-dir> <out-dir> macos|ios|tvos
#
# Mac only, with Xcode's Metal toolchain (Xcode 26: `xcodebuild
# -downloadComponent MetalToolchain`). The MSL is derived from the game's
# shaders: both directories belong under $LGP_PRIVATE_DIR, never into the
# repository. Existing outputs newer than their source are kept, so a second
# run after more dumping only compiles the new files.
#
# Each file's first line (`// lgp-msl: 3.2`) names the MSL version the device
# compiled it with; the same version and -fpreserve-invariance (as wgpu's
# MTLCompileOptions) are used here. The output is AIR, the same for every GPU
# of the platform; the GPU code is still built on the device when the
# pipeline is (Metal's own shader cache keeps that).
#
# Without a deployment target the compiler would take the SDK's OS version,
# and an older device could reject the library. Each library gets the app's
# deployment target (CMakePresets.json: iOS 16.0, tvOS 17.0; macOS 13.0), or
# the oldest OS that has its MSL version if that is newer (MSL 3.2: OS 18 and
# macOS 15).
#
#   LGP_METALLIB_JOBS     parallel compiles (default: number of CPUs)
#   LGP_METALLIB_MIN_OS   the app's deployment target, if not the default
set -eu

src=${1:?usage: $0 <msl-dir> <out-dir> macos|ios|tvos}
out=${2:?usage: $0 <msl-dir> <out-dir> macos|ios|tvos}
platform=${3:?usage: $0 <msl-dir> <out-dir> macos|ios|tvos}
case $platform in
    macos) sdk=macosx min_flag=-mmacosx-version-min app_min=13.0 ;;
    ios) sdk=iphoneos min_flag=-mios-version-min app_min=16.0 ;;
    tvos) sdk=appletvos min_flag=-mtvos-version-min app_min=17.0 ;;
    *) echo "$0: platform must be macos, ios or tvos" >&2; exit 2 ;;
esac
xcrun --sdk "$sdk" --find metal >/dev/null 2>&1 || {
    echo "$0: no metal compiler for $sdk (install the Metal toolchain: xcodebuild -downloadComponent MetalToolchain)" >&2
    exit 1
}
mkdir -p "$out"
jobs=${LGP_METALLIB_JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || echo 4)}
app_min=${LGP_METALLIB_MIN_OS:-$app_min}

# The -std value for a version; MSL 3 has one name on every platform, MSL 2
# one per platform.
std_for() {
    case $1 in
        3.*) echo "metal$1" ;;
        2.*) if [ "$platform" = macos ]; then echo "macos-metal$1"; else echo "ios-metal$1"; fi ;;
        *) return 1 ;;
    esac
}

# The oldest OS with an MSL version (iOS and tvOS share the numbers).
os_for() {
    case $platform:$1 in
        macos:3.2) echo 15.0 ;; macos:3.1) echo 14.0 ;; macos:3.0) echo 13.0 ;;
        macos:2.4) echo 12.0 ;; macos:2.3) echo 11.0 ;; macos:*) echo 10.15 ;;
        *:3.2) echo 18.0 ;; *:3.1) echo 17.0 ;; *:3.0) echo 16.0 ;;
        *:2.4) echo 15.0 ;; *:2.3) echo 14.0 ;; *) echo 13.0 ;;
    esac
}

# The newer of two versions like 16.0.
newer() {
    if [ "$(printf '%s\n%s\n' "$1" "$2" | sort -t. -k1,1n -k2,2n | tail -n 1)" = "$1" ]; then echo "$1"; else echo "$2"; fi
}

total=0
count=0
todo=$(mktemp)
trap 'rm -f "$todo"' EXIT
for f in "$src"/*.metal; do
    [ -e "$f" ] || { echo "$0: no .metal files in $src" >&2; exit 1; }
    total=$((total + 1))
    name=$(basename "$f" .metal)
    lib="$out/$name.metallib"
    [ -e "$lib" ] && [ ! "$f" -nt "$lib" ] && continue
    version=$(sed -n '1s#^// lgp-msl: \([0-9.]*\)$#\1#p' "$f")
    std=$(std_for "$version") || { echo "$0: $f: no '// lgp-msl:' line" >&2; exit 1; }
    # NUL-separated, so paths with spaces or quotes reach metal unchanged.
    min="$min_flag=$(newer "$app_min" "$(os_for "$version")")"
    printf '%s\0%s\0%s\0%s\0' "$std" "$min" "$f" "$lib" >>"$todo"
    count=$((count + 1))
done
echo "metallibs: $total sources, $count to compile ($sdk, $jobs jobs)"
if [ "$count" -gt 0 ]; then
    # shellcheck disable=SC2016
    xargs -0 -n 4 -P "$jobs" sh -c 'xcrun --sdk "$0" metal -std="$1" "$2" -fpreserve-invariance -o "$4.tmp" "$3" && mv "$4.tmp" "$4"' \
        "$sdk" <"$todo"
fi
done_count=$(find "$out" -name '*.metallib' | wc -l | tr -d ' ')
echo "metallibs: $done_count in $out"
