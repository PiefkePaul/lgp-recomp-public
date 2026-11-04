#!/bin/sh
# Builds wgpu-native for Android arm64 from a checkout with our patches and
# lays it out like a wgpu-native release for -DLGP_WGPU_NATIVE_DIR.
#
#   tools/android/build_wgpu_native.sh <wgpu-native-checkout> <out-dir>
#
# Thin wrapper around tools/webgpu/build_wgpu_native.py --android, which can
# also clone the checkout itself (without --src). LGP_WGPU_NATIVE_PATCHES
# picks the patches (default driver-cache,lazy-naga-ir: lazy-naga-ir gives
# shader modules that keep only their WGSL text,
# docs/knowledge/webgpu-shader-speicher.md; keep-framebuffers adds Vulkan
# framebuffers that outlive encoder resets with LGP_WGPU_KEEP_FRAMEBUFFERS=1,
# docs/knowledge/webgpu-backend-draw-kosten.md). Needs ANDROID_NDK_HOME (r27 or
# newer), rustup and git; on Windows run the Python script directly. The
# checkout is the release tag the project pins, with submodules:
#   git clone --depth 1 --branch v29.0.1.1 --recurse-submodules \
#       https://github.com/gfx-rs/wgpu-native.git <checkout>
# Default features, like the release archive; only the patches differ.
set -eu

src=${1:?usage: $0 <wgpu-native-checkout> <out-dir>}
out=${2:?usage: $0 <wgpu-native-checkout> <out-dir>}
here=$(cd "$(dirname "$0")" && pwd)
py=python3
command -v "$py" >/dev/null 2>&1 || py=python
exec "$py" "$here/../webgpu/build_wgpu_native.py" --android --src "$src" --out "$out" \
    --patches "${LGP_WGPU_NATIVE_PATCHES:-driver-cache,lazy-naga-ir}"
