#!/bin/sh
# Builds wgpu-native as a static library for an Apple target (P7.4: tvOS,
# which has no release asset; iOS for a patched build) and lays it out like a
# release, for
# -DLGP_WGPU_NATIVE_DIR=<out>/wgpu-<target>:
#   <out>/wgpu-<target>/include/webgpu/{webgpu,wgpu}.h
#   <out>/wgpu-<target>/lib/libwgpu_native.a
#
#   tools/apple/build_wgpu_native.sh tvos|ios [OUT_DIR]
#
# Mac only (Xcode with the tvOS SDK, rustup). No game data is involved; the
# output may live anywhere (default: build/wgpu-native). The version matches
# the iOS release the repository downloads (cmake/LgpWebGpu.cmake); the
# features are only what the renderer uses (WGSL shaders, Metal). For tvOS
# the checkout is patched so the Metal surface exists (see below).
#
#   LGP_WGPU_NATIVE_TAG       tag to build (default v29.0.1.1)
#   LGP_WGPU_NATIVE_FEATURES  cargo features (default wgsl,metal)
#   LGP_WGPU_NATIVE_SRC       existing checkout to use instead of cloning
#   TVOS_DEPLOYMENT_TARGET    minimum tvOS (default 17.0, as the tvos preset)
#   IPHONEOS_DEPLOYMENT_TARGET minimum iOS (default 16.0, as the ios preset)
#   LGP_WGPU_NATIVE_LAZY_IR=0|1 apply tools/wgpu/lazy-naga-ir.patch (shader
#                             modules keep only their WGSL text,
#                             docs/knowledge/webgpu-shader-speicher.md) and
#                             metal-drop-library.patch (off unless
#                             LGP_WGPU_METAL_DROP_LIBRARY=1 at run time) and
#                             metal-precompiled.patch (offline metallibs,
#                             only with LGP_WGPU_METAL_LIBS at run time);
#                             default 1 for tvOS (lazy IR kept the Apple TV
#                             under its memory limit; every tvOS build now
#                             carries both patches), 0 for iOS (not measured
#                             there); needs
#                             tools/wgpu/patch_wgpu_native.sh
#   LGP_WGPU_NATIVE_TOOLCHAIN rustup toolchain (default stable, see below)
#
# aarch64-apple-tvos is a Tier 2 Rust target (rustup ships its std). If
# `rustup target add` refuses it on an older toolchain, update with
# `rustup update stable`; LGP_WGPU_NATIVE_BUILD_STD=1 builds std from source
# with nightly instead (needs `rustup component add rust-src --toolchain nightly`).
set -eu

platform=${1:-}
case "$platform" in
    tvos)
        triple=aarch64-apple-tvos
        sdk=appletvos
        clang_target=arm64-apple-tvos
        : "${TVOS_DEPLOYMENT_TARGET:=17.0}"
        export TVOS_DEPLOYMENT_TARGET
        lazy_ir_default=1
        ;;
    ios)
        triple=aarch64-apple-ios
        sdk=iphoneos
        clang_target=arm64-apple-ios
        : "${IPHONEOS_DEPLOYMENT_TARGET:=16.0}"
        export IPHONEOS_DEPLOYMENT_TARGET
        lazy_ir_default=0
        ;;
    *)
        echo "usage: $0 tvos|ios [OUT_DIR]" >&2
        exit 2
        ;;
esac

root=$(cd "$(dirname "$0")/../.." && pwd)
out=${2:-$root/build/wgpu-native}
tag=${LGP_WGPU_NATIVE_TAG:-v29.0.1.1}
features=${LGP_WGPU_NATIVE_FEATURES:-wgsl,metal}
dest="$out/wgpu-$platform"

command -v xcrun >/dev/null || { echo "$0: needs Xcode (xcrun)" >&2; exit 1; }
command -v cargo >/dev/null || { echo "$0: needs Rust (rustup, cargo)" >&2; exit 1; }
SDKROOT=$(xcrun --sdk "$sdk" --show-sdk-path)
export SDKROOT

if [ -n "${LGP_WGPU_NATIVE_SRC:-}" ]; then
    src=$LGP_WGPU_NATIVE_SRC
else
    src="$out/src-$tag"
    if [ ! -d "$src/.git" ]; then
        mkdir -p "$out"
        git clone --depth 1 --branch "$tag" --recurse-submodules --shallow-submodules \
            https://github.com/gfx-rs/wgpu-native.git "$src"
    fi
fi

# wgpu-native gates its Metal surface on
# any(target_os = "ios", target_os = "macos"); on tvOS
# wgpuInstanceCreateSurface then panics with "Unsupported Surface" (seen on
# the Apple TV, 2026-10-14). Add tvos to those cfgs in src/; the pattern is
# gone once patched, so a second run changes nothing.
patched=none
if [ "$platform" = tvos ]; then
    pattern='any(target_os = "ios", target_os = "macos")'
    files=$(grep -rlF "$pattern" "$src/src" || true)
    if [ -n "$files" ]; then
        for f in $files; do
            sed -i.orig 's/any(target_os = "ios", target_os = "macos")/any(target_os = "ios", target_os = "tvos", target_os = "macos")/g' "$f"
            rm -f "$f.orig"
        done
    fi
    if grep -rqF 'target_os = "tvos"' "$src/src"; then
        patched=tvos-metal-surface
    else
        echo "$0: tvOS surface patch did not apply (wgpu-native $tag changed?)" >&2
        exit 1
    fi
fi

# Shader modules keep only their WGSL text (Metal; tvOS default, see above).
# The patch sits in a sibling wgpu checkout wired in through [patch.crates-io].
if [ "${LGP_WGPU_NATIVE_LAZY_IR:-$lazy_ir_default}" = 1 ]; then
    [ -x "$root/tools/wgpu/patch_wgpu_native.sh" ] || {
        echo "$0: LGP_WGPU_NATIVE_LAZY_IR=1 needs tools/wgpu/patch_wgpu_native.sh" >&2; exit 1; }
    "$root/tools/wgpu/patch_wgpu_native.sh" "$src"
fi
if grep -q '^# LGP: local wgpu' "$src/Cargo.toml"; then
    if [ "$patched" = none ]; then patched=lazy-naga-ir; else patched="$patched+lazy-naga-ir"; fi
    if git -C "$src-wgpu" apply --reverse --check "$root/tools/wgpu/metal-drop-library.patch" 2>/dev/null; then
        patched="$patched+metal-drop-library"
    fi
    if git -C "$src-wgpu" apply --reverse --check "$root/tools/wgpu/metal-precompiled.patch" 2>/dev/null; then
        patched="$patched+metal-precompiled"
    fi
fi

# wgpu-native's build.rs passes the SDK to bindgen only for iOS and macOS;
# for other Apple targets it comes from BINDGEN_EXTRA_CLANG_ARGS.
export BINDGEN_EXTRA_CLANG_ARGS="-isysroot $SDKROOT --target=$clang_target"
CC=$(xcrun --sdk "$sdk" --find clang)
export CC
triple_env=$(echo "$triple" | tr 'a-z-' 'A-Z_')
eval "export CARGO_TARGET_${triple_env}_LINKER=\"\$CC\""

# wgpu-native's rust-toolchain.toml pins a Rust release (1.93 for v29.0.1.1)
# that has no prebuilt tvOS std (E0463 "can't find crate for std"); the
# current stable has one. RUSTUP_TOOLCHAIN overrides the file.
build_std=
if [ "${LGP_WGPU_NATIVE_BUILD_STD:-0}" = 1 ]; then
    RUSTUP_TOOLCHAIN=nightly
    build_std=-Zbuild-std
else
    RUSTUP_TOOLCHAIN=${LGP_WGPU_NATIVE_TOOLCHAIN:-stable}
    rustup target add --toolchain "$RUSTUP_TOOLCHAIN" "$triple"
fi
export RUSTUP_TOOLCHAIN

# Only the static library: the crate also declares a cdylib, which the app
# never links.
(cd "$src" && cargo rustc --release --lib --crate-type staticlib \
    --target "$triple" $build_std --no-default-features --features "$features")

mkdir -p "$dest/include/webgpu" "$dest/lib"
cp "$src/ffi/webgpu-headers/webgpu.h" "$src/ffi/wgpu.h" "$dest/include/webgpu/"
cp "$src/target/$triple/release/libwgpu_native.a" "$dest/lib/"
printf '%s %s features=%s patch=%s rust=%s\n' "$tag" "$triple" "$features" "$patched" "$(rustc --version)" > "$dest/BUILD-INFO"
echo "wgpu-native $tag for $triple: -DLGP_WGPU_NATIVE_DIR=$dest"
