#!/usr/bin/env python3
"""Builds wgpu-native from source with our patches.

The released wgpu-native (cmake/LgpWebGpu.cmake downloads it) stays the
default. This script builds the pinned tag with a chosen set of patches into
a directory laid out like a release, for -DLGP_WGPU_NATIVE_DIR=<out>:

  python3 tools/webgpu/build_wgpu_native.py --out <dir> [--android]
      [--patches driver-cache,lazy-naga-ir,keep-framebuffers|none] [--src <checkout>] [--work <dir>]

Patches (--patches, comma separated; default: driver-cache, with --android
driver-cache,lazy-naga-ir):
  driver-cache  wgpu-native-pipeline-cache.patch on wgpu-native: the driver
                pipeline cache (VkPipelineCache). The WebGPU backend uses it
                when the headers define WGPU_LGP_PIPELINE_CACHE and keeps its
                data next to the pipeline cache file (<file>.driver). Only
                Vulkan has the feature; elsewhere the renderer does not ask
                for it.
  lazy-naga-ir  tools/wgpu/lazy-naga-ir.patch on wgpu (cloned at the version
                the checkout's Cargo.lock pins, next to it as <checkout>-wgpu,
                wired in through [patch.crates-io]): on Metal and Vulkan a
                shader module keeps only its WGSL text, each pipeline parses it
                again (docs/knowledge/webgpu-shader-speicher.md). In the
                default set for --android (NVIDIA Shield: native heap -615 MiB,
                docs/analysis/36-android-native-heap.md); at run time
                LGP_WGPU_LAZY_IR=0 turns it off. tools/wgpu/metal-drop-library.patch
                rides along on the same checkout: with
                LGP_WGPU_METAL_DROP_LIBRARY=1 at run time a Metal pipeline lets
                go of its stages' MTLLibrary once built (default: kept, as
                upstream; docs/analysis/32-metal-speicher-je-pipeline.md).
                tools/wgpu/metal-precompiled.patch rides along too: Metal
                libraries compiled offline (LGP_WGPU_METAL_LIBS=<dir>), the MSL
                dump for them (LGP_WGPU_METAL_DUMP_MSL=<dir>), a cold MSL->AIR
                step without clearing Metal's cache (LGP_WGPU_METAL_SALT=<text>)
                and compile times (LGP_WGPU_METAL_STATS=1); without these
                variables it
                changes nothing (docs/analysis/46-metal-shader-vorkompilieren.md).
  keep-framebuffers
                tools/wgpu/vulkan-keep-framebuffers.patch on the same wgpu
                checkout: with LGP_WGPU_KEEP_FRAMEBUFFERS=1 at run time a
                Vulkan command encoder keeps its framebuffers across resets
                instead of creating one per render pass again (default: off,
                as upstream; docs/knowledge/webgpu-backend-draw-kosten.md).

--patch-only only prepares the checkout (--src, required then) and builds
nothing; tools/wgpu/patch_wgpu_native.sh and through it
tools/apple/build_wgpu_native.sh use that. Without --src the script clones
into <work>/wgpu-native and resets it to the tag on every run, so a different
--patches takes effect; a checkout given with --src is patched in place
(patches are only added, never removed; BUILD-INFO names what it carries),
and a second run changes nothing.

Needs git, a Rust toolchain (rustup; the checkout's rust-toolchain.toml picks
the version) and libclang for bindgen; --android needs ANDROID_NDK_HOME (the
script adds the Rust target and takes libclang from the NDK unless
LIBCLANG_PATH is set). Wissen: docs/knowledge/webgpu-backend-draw-kosten.md
(driver-cache), docs/knowledge/webgpu-shader-speicher.md (lazy-naga-ir).
"""
import argparse
import glob
import os
import pathlib
import platform
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
TAG = "v29.0.1.1"  # LGP_WGPU_NATIVE_VERSION
REPO = "https://github.com/gfx-rs/wgpu-native.git"
WGPU_REPO = "https://github.com/gfx-rs/wgpu.git"
ANDROID_TARGET = "aarch64-linux-android"
ANDROID_API = 29  # ANDROID_PLATFORM of the android preset
PATCHES = ("driver-cache", "lazy-naga-ir", "keep-framebuffers")
DRIVER_CACHE_PATCH = os.path.join(HERE, "wgpu-native-pipeline-cache.patch")
LAZY_IR_PATCH = os.path.join(ROOT, "tools", "wgpu", "lazy-naga-ir.patch")
METAL_LIBRARY_PATCH = os.path.join(ROOT, "tools", "wgpu", "metal-drop-library.patch")
KEEP_FRAMEBUFFERS_PATCH = os.path.join(ROOT, "tools", "wgpu", "vulkan-keep-framebuffers.patch")
METAL_PRECOMPILED_PATCH = os.path.join(ROOT, "tools", "wgpu", "metal-precompiled.patch")
LAZY_IR_MARK = "# LGP: local wgpu"
# Every crate of the wgpu workspace that wgpu-native pulls in, so that no
# second copy from crates.io ends up in the graph.
LAZY_IR_CRATES = [
    ("naga", "naga"),
    ("wgpu-core", "wgpu-core"),
    ("wgpu-hal", "wgpu-hal"),
    ("wgpu-types", "wgpu-types"),
    ("wgpu-naga-bridge", "wgpu-naga-bridge"),
    ("wgpu-core-deps-apple", "wgpu-core/platform-deps/apple"),
    ("wgpu-core-deps-windows-linux-android", "wgpu-core/platform-deps/windows-linux-android"),
    ("wgpu-core-deps-wasm", "wgpu-core/platform-deps/wasm"),
    ("wgpu-core-deps-emscripten", "wgpu-core/platform-deps/emscripten"),
]


def run(cmd, cwd=None, env=None):
    print("+ " + " ".join(cmd), flush=True)
    subprocess.run(cmd, cwd=cwd, env=env, check=True)


def applied(repo, patch):
    return subprocess.run(["git", "apply", "--reverse", "--check", patch], cwd=repo,
                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0


def git_apply(repo, patch, name):
    """Applies patch in repo unless it is applied already."""
    if applied(repo, patch):
        print("%s already applied" % name, flush=True)
        return
    run(["git", "apply", patch], cwd=repo)


def parse_patches(text):
    if text.strip() in ("", "none"):
        return []
    names = [p.strip() for p in text.split(",") if p.strip()]
    for n in names:
        if n not in PATCHES:
            sys.exit("unknown patch %r (known: %s, none)" % (n, ", ".join(PATCHES)))
    return names


def checkout(work):
    """Clones the pinned tag into <work>/wgpu-native and resets it to it."""
    src = os.path.join(work, "wgpu-native")
    if not os.path.exists(os.path.join(src, ".git")):  # a worktree has a .git file
        run(["git", "clone", "--depth", "1", "--branch", TAG, "--recurse-submodules", "--shallow-submodules", REPO, src])
    else:
        # A checkout of another tag (shallow): fetch the pinned one first.
        run(["git", "fetch", "--depth", "1", "origin", "tag", TAG], cwd=src)
    # --force also resets Cargo.toml and Cargo.lock, so the patch set of an
    # earlier run does not linger.
    run(["git", "checkout", "--force", TAG], cwd=src)
    run(["git", "submodule", "update", "--init", "--recursive", "--depth", "1"], cwd=src)
    run(["git", "clean", "-fdq", "--", "ffi", "src", "build.rs"], cwd=src)
    return src


def wgpu_version(src):
    with open(os.path.join(src, "Cargo.lock"), encoding="utf-8") as f:
        m = re.search(r'^name = "wgpu-hal"\nversion = "([0-9.]+)"', f.read(), re.M)
    if not m:
        sys.exit("no wgpu-hal in %s/Cargo.lock" % src)
    return m.group(1)


def local_wgpu(src):
    """Clones wgpu at the version src pins next to it and wires it in through
    [patch.crates-io]; returns its directory and version."""
    version = wgpu_version(src)
    # Next to the checkout, not inside: cargo would take its crates for
    # members of wgpu-native's workspace.
    wgpu = src.rstrip("/\\") + "-wgpu"
    if not os.path.exists(os.path.join(wgpu, ".git")):
        run(["git", "clone", "--depth", "1", "--branch", "v" + version, WGPU_REPO, wgpu])
    else:
        # From a wgpu-native tag with another wgpu: switch to this version
        # (dropping the patch applied to the old one), or [patch.crates-io]
        # would point at crates cargo does not use.
        at = subprocess.run(["git", "describe", "--tags", "--exact-match"], cwd=wgpu, capture_output=True,
                            text=True).stdout.strip()
        if at != "v" + version:
            run(["git", "fetch", "--depth", "1", "origin", "tag", "v" + version], cwd=wgpu)
            run(["git", "checkout", "--force", "v" + version], cwd=wgpu)
    cargo_toml = os.path.join(src, "Cargo.toml")
    with open(cargo_toml, encoding="utf-8") as f:
        text = f.read()
    if LAZY_IR_MARK not in text:
        # Relative to the checkout: an absolute Git Bash path (/d/...) would
        # not resolve for cargo on Windows.
        rel = "../" + os.path.basename(wgpu)
        lines = ["", LAZY_IR_MARK + " with patches from tools/wgpu",
                 "[patch.crates-io]"]
        lines += ['%s = { path = "%s/%s" }' % (name, rel, path) for name, path in LAZY_IR_CRATES]
        with open(cargo_toml, "a", encoding="utf-8", newline="\n") as f:
            f.write("\n".join(lines) + "\n")
    return wgpu, version


def apply_lazy_ir(src):
    wgpu, version = local_wgpu(src)
    git_apply(wgpu, LAZY_IR_PATCH, "wgpu %s: lazy-naga-ir.patch" % version)
    git_apply(wgpu, METAL_LIBRARY_PATCH, "wgpu %s: metal-drop-library.patch" % version)
    # On top of both: its context includes their lines.
    git_apply(wgpu, METAL_PRECOMPILED_PATCH, "wgpu %s: metal-precompiled.patch" % version)
    print("wgpu-native in %s builds with wgpu %s + lazy-naga-ir.patch + metal-drop-library.patch"
          " + metal-precompiled.patch" % (src, version), flush=True)


def apply_keep_framebuffers(src):
    wgpu, version = local_wgpu(src)
    git_apply(wgpu, KEEP_FRAMEBUFFERS_PATCH, "wgpu %s: vulkan-keep-framebuffers.patch" % version)
    print("wgpu-native in %s builds with wgpu %s + vulkan-keep-framebuffers.patch" % (src, version), flush=True)


def patches_in(src):
    """The patches a checkout actually carries (a --src checkout may have
    more than asked for from an earlier run)."""
    found = []
    if applied(src, DRIVER_CACHE_PATCH):
        found.append("driver-cache")
    with open(os.path.join(src, "Cargo.toml"), encoding="utf-8") as f:
        local = LAZY_IR_MARK in f.read()
    wgpu = src.rstrip("/\\") + "-wgpu"
    if local and applied(wgpu, LAZY_IR_PATCH):
        found.append("lazy-naga-ir")
        # Rides along with lazy-naga-ir (off at run time by default).
        if applied(wgpu, METAL_LIBRARY_PATCH):
            found.append("metal-drop-library")
        if applied(wgpu, METAL_PRECOMPILED_PATCH):
            found.append("metal-precompiled")
    if local and applied(wgpu, KEEP_FRAMEBUFFERS_PATCH):
        found.append("keep-framebuffers")
    return found


def apply_patches(src, patches):
    if "driver-cache" in patches:
        git_apply(src, DRIVER_CACHE_PATCH, "wgpu-native: wgpu-native-pipeline-cache.patch")
    if "lazy-naga-ir" in patches:
        apply_lazy_ir(src)
    if "keep-framebuffers" in patches:
        apply_keep_framebuffers(src)


def android_env(env):
    ndk = env.get("ANDROID_NDK_HOME") or env.get("ANDROID_NDK_ROOT")
    if not ndk:
        sys.exit("--android needs ANDROID_NDK_HOME")
    hosts = glob.glob(os.path.join(ndk, "toolchains", "llvm", "prebuilt", "*"))
    if not hosts:
        sys.exit("no NDK LLVM toolchain under %s" % ndk)
    bin_dir = os.path.join(hosts[0], "bin")
    ext = ".cmd" if platform.system() == "Windows" else ""
    clang = os.path.join(bin_dir, "%s%d-clang%s" % (ANDROID_TARGET, ANDROID_API, ext))
    key = ANDROID_TARGET.upper().replace("-", "_")
    env = dict(env)
    env["CARGO_TARGET_%s_LINKER" % key] = clang
    env["CC_" + ANDROID_TARGET.replace("-", "_")] = clang
    env["AR_" + ANDROID_TARGET.replace("-", "_")] = os.path.join(bin_dir, "llvm-ar" + (".exe" if ext else ""))
    # bindgen's libclang does not find the NDK headers by --sysroot alone
    # (Windows: "math.h not found"), so name both include directories; clang
    # takes forward slashes on every host.
    sysroot = pathlib.Path(hosts[0], "sysroot").as_posix()
    env["BINDGEN_EXTRA_CLANG_ARGS_" + ANDROID_TARGET.replace("-", "_")] = " ".join([
        "--target=%s%d" % (ANDROID_TARGET, ANDROID_API),
        "--sysroot=" + sysroot,
        "-isystem", sysroot + "/usr/include",
        "-isystem", "%s/usr/include/%s" % (sysroot, ANDROID_TARGET),
    ])
    # libclang for bindgen: the NDK's own unless LIBCLANG_PATH names one.
    if not env.get("LIBCLANG_PATH"):
        for d in (bin_dir, os.path.join(hosts[0], "lib"), os.path.join(hosts[0], "lib64")):
            if glob.glob(os.path.join(d, "libclang*")):
                env["LIBCLANG_PATH"] = d
                break
    return env


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", help="release-like directory (include/webgpu, lib)")
    ap.add_argument("--android", action="store_true", help="build for Android arm64 (static library)")
    ap.add_argument("--patches", default=None,
                    help="comma-separated: %s, or none (default: driver-cache; "
                         "with --android driver-cache,lazy-naga-ir)" % ", ".join(PATCHES))
    ap.add_argument("--src", default=None, help="existing wgpu-native checkout, patched in place")
    ap.add_argument("--work", default=None, help="checkout directory without --src (default: <out>/../wgpu-native-work)")
    ap.add_argument("--patch-only", action="store_true", help="only patch the --src checkout, build nothing")
    a = ap.parse_args(argv)
    if a.patches is None:
        a.patches = "driver-cache,lazy-naga-ir" if a.android else "driver-cache"
    patches = parse_patches(a.patches)
    if a.patch_only:
        if not a.src:
            ap.error("--patch-only needs --src")
        src = os.path.abspath(a.src)
        if not os.path.isfile(os.path.join(src, "Cargo.lock")):
            sys.exit("%s/Cargo.lock not found" % src)
        apply_patches(src, patches)
        return 0
    if not a.out:
        ap.error("--out is required")
    out = os.path.abspath(a.out)
    if a.src:
        src = os.path.abspath(a.src)
        if not os.path.isfile(os.path.join(src, "Cargo.lock")):
            sys.exit("%s/Cargo.lock not found" % src)
    else:
        work = os.path.abspath(a.work or os.path.join(out, os.pardir, "wgpu-native-work"))
        os.makedirs(work, exist_ok=True)
        src = checkout(work)
    apply_patches(src, patches)
    extra = [p for p in patches_in(src) if p not in patches and p not in ("metal-drop-library", "metal-precompiled")]
    if extra:
        print("note: %s also carries %s from an earlier run" % (src, ", ".join(extra)), flush=True)
    patches = patches_in(src)  # what the build has, for BUILD-INFO
    env = dict(os.environ)
    cmd = ["cargo", "build", "--release"]
    if not {"lazy-naga-ir", "keep-framebuffers"} & set(patches):
        cmd.append("--locked")  # the tag's Cargo.lock; [patch.crates-io] rewrites it
    target_dir = os.path.join(src, "target", "release")
    if a.android:
        env = android_env(env)
        # For the toolchain the checkout's rust-toolchain.toml pins.
        run(["rustup", "target", "add", ANDROID_TARGET], cwd=src)
        cmd += ["--target", ANDROID_TARGET]
        target_dir = os.path.join(src, "target", ANDROID_TARGET, "release")
    run(cmd, cwd=src, env=env)
    inc = os.path.join(out, "include", "webgpu")
    lib = os.path.join(out, "lib")
    os.makedirs(inc, exist_ok=True)
    os.makedirs(lib, exist_ok=True)
    shutil.copy2(os.path.join(src, "ffi", "wgpu.h"), inc)
    shutil.copy2(os.path.join(src, "ffi", "webgpu-headers", "webgpu.h"), inc)
    names = ["libwgpu_native.a"] if a.android else [
        "libwgpu_native.a", "libwgpu_native.so", "libwgpu_native.dylib", "wgpu_native.dll", "wgpu_native.dll.lib",
        "wgpu_native.lib"]
    copied = [n for n in names if os.path.exists(os.path.join(target_dir, n))]
    for n in copied:
        shutil.copy2(os.path.join(target_dir, n), lib)
    label = ",".join(patches) or "none"
    with open(os.path.join(out, "BUILD-INFO"), "w", encoding="utf-8") as f:
        f.write("%s %s patches=%s\n" % (TAG, ANDROID_TARGET if a.android else "host", label))
    print("wgpu-native %s (patches: %s) in %s (%s)" % (TAG, label, out, ", ".join(copied)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
