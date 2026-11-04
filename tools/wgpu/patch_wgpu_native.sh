#!/bin/sh
# Patches a wgpu-native checkout so it builds against a local wgpu checkout
# carrying lazy-naga-ir.patch: on Metal and Vulkan a shader module then keeps only its
# WGSL text instead of the naga IR and its ModuleInfo, and each pipeline parses
# it again (docs/knowledge/webgpu-shader-speicher.md). LGP_WGPU_LAZY_IR=0 at
# run time turns it off for A/B measurements. metal-drop-library.patch rides
# along (Metal pipelines let go of their MTLLibrary only with
# LGP_WGPU_METAL_DROP_LIBRARY=1 at run time), and so does
# metal-precompiled.patch (offline-compiled metallibs with LGP_WGPU_METAL_LIBS,
# MSL dump with LGP_WGPU_METAL_DUMP_MSL, compile times with
# LGP_WGPU_METAL_STATS=1; docs/analysis/46-metal-shader-vorkompilieren.md).
#
#   tools/wgpu/patch_wgpu_native.sh <wgpu-native-checkout>
#
# The work is done by tools/webgpu/build_wgpu_native.py (--patch-only
# --patches lazy-naga-ir), which also builds with any set of our patches:
# wgpu is cloned at the version the checkout's Cargo.lock pins (29.0.3 for
# wgpu-native v29.0.1.1) next to it, into <checkout>-wgpu, and wired in
# through [patch.crates-io]. A second run changes nothing. Afterwards build as
# usual, e.g. on a Mac for the host:
#   (cd <checkout> && cargo build --release --no-default-features --features wgsl,metal)
# or for iOS/tvOS with tools/apple/build_wgpu_native.sh (tvOS by default, iOS with
# LGP_WGPU_NATIVE_LAZY_IR=1).
set -eu

src=${1:?usage: $0 <wgpu-native-checkout>}
here=$(cd "$(dirname "$0")" && pwd)
py=python3
command -v "$py" >/dev/null 2>&1 || py=python
exec "$py" "$here/../webgpu/build_wgpu_native.py" --patch-only --patches lazy-naga-ir --src "$src"
