#!/usr/bin/env python3
"""Package a native program as an Android APK, without Gradle (P7.3).

    build_apk.py --sdk <ANDROID_HOME> --manifest AndroidManifest.xml
                 --java-src <dir>... --lib <libX.so> --out X.apk [--res <dir>]
                 --keystore <debug.keystore> [--javac javac] [--jar jar]
                 [--keytool keytool] [--min-sdk 29] [--target-sdk 35]
                 [--asset NAME=FILE]... [--store-libs] [--strip llvm-strip]

Steps: aapt2 compile (--res, e.g. the TV banner) and link -> javac (all .java below
the --java-src dirs, against android.jar) -> d8 -> classes.dex and
lib/<abi>/<lib> (--abi, arm64-v8a by default) into the APK -> zipalign -> apksigner with a debug key
(created with keytool on first use; keep one keystore for all builds, see
docs/knowledge/android-ndk-build.md). The newest build-tools and the newest
platform of the SDK are used unless --build-tools/--platform name them.

--strip removes the debug sections from the libraries in the APK (llvm-strip
--strip-debug, as the Android Gradle plugin does; the symbol table stays, so
simpleperf still names functions). The NDK compiles with -g in every build type,
which makes most of a large library. The library in the build tree keeps them.

--store-libs stores the libraries uncompressed; zipalign -p then puts them on page
boundaries, so Android loads them straight from the APK. Needs
android:extractNativeLibs="false" in the manifest (LGP_ANDROID_EXTRACT_NATIVE_LIBS=OFF).

--asset puts FILE into the APK as assets/NAME (e.g. the pipeline cache seed,
LGP_APP_PIPELINE_SEED).

An APK built from game code is game code: it stays on the own PC and the own
phone (docs/knowledge/android-ndk-build.md).
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import zipfile


def version_key(name):
    parts = []
    for p in name.replace("-", ".").split("."):
        parts.append((0, int(p)) if p.isdigit() else (1, p))
    return parts


def newest(dir_path, prefix=""):
    names = [n for n in os.listdir(dir_path) if n.startswith(prefix) and os.path.isdir(os.path.join(dir_path, n))]
    if not names:
        sys.exit(f"build_apk: nothing in {dir_path}")
    return sorted(names, key=version_key)[-1]


def tool(build_tools, name):
    for suffix in ("", ".exe", ".bat"):
        p = os.path.join(build_tools, name + suffix)
        if os.path.exists(p):
            return p
    sys.exit(f"build_apk: {name} not found in {build_tools}")


def run(cmd, **kw):
    r = subprocess.run(cmd, **kw)
    if r.returncode != 0:
        sys.exit(f"build_apk: failed ({r.returncode}): {' '.join(cmd)}")


def create_keystore(keytool, keystore):
    # Several APK targets of one build tree share the key and ninja packs them in
    # parallel. keytool refuses an existing file, so the key is made under a name
    # of its own and linked into place; whoever links first wins and the others
    # keep that key (replacing it would leave APKs signed with different keys).
    folder = os.path.dirname(os.path.abspath(keystore))
    os.makedirs(folder, exist_ok=True)
    tmp = os.path.join(folder, f".{os.path.basename(keystore)}.{os.getpid()}.tmp")
    if os.path.exists(tmp):
        os.remove(tmp)
    try:
        run([keytool, "-genkeypair", "-keystore", tmp, "-storepass", "android", "-alias", "androiddebugkey",
             "-keypass", "android", "-keyalg", "RSA", "-keysize", "2048", "-validity", "10000", "-dname",
             "CN=Android Debug,O=Android,C=US"], stdout=subprocess.DEVNULL)
        try:
            os.link(tmp, keystore)
        except FileExistsError:
            pass
        except OSError:
            # No hard links (exFAT, some network shares): exclusive create keeps
            # "first wins"; a racing build may read a half-written key and fail
            # loudly, never sign with a different one.
            try:
                fd = os.open(keystore, os.O_CREAT | os.O_EXCL | os.O_WRONLY | getattr(os, "O_BINARY", 0))
            except FileExistsError:
                pass
            else:
                with os.fdopen(fd, "wb") as dst, open(tmp, "rb") as src:
                    shutil.copyfileobj(src, dst)
    finally:
        if os.path.exists(tmp):
            os.remove(tmp)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--sdk", required=True)
    ap.add_argument("--build-tools", help="build-tools version (default: newest)")
    ap.add_argument("--platform", help="platform, e.g. android-35 (default: newest)")
    ap.add_argument("--manifest", required=True)
    ap.add_argument("--java-src", action="append", default=[])
    ap.add_argument("--res", help="Android resource directory (drawable/ ...)")
    ap.add_argument("--lib", action="append", required=True, help="native library for lib/<abi>/")
    ap.add_argument("--abi", default="arm64-v8a", help="arm64-v8a (devices) or x86_64 (emulator image)")
    ap.add_argument("--out", required=True)
    ap.add_argument("--keystore", required=True)
    ap.add_argument("--javac", default="javac")
    ap.add_argument("--jar", default="jar")
    ap.add_argument("--keytool", default="keytool")
    ap.add_argument("--min-sdk", default="29")
    ap.add_argument("--target-sdk", default="35")
    ap.add_argument("--asset", action="append", default=[], help="NAME=FILE: FILE as assets/NAME")
    ap.add_argument("--strip", help="llvm-strip: debug sections out of the libraries in the APK")
    ap.add_argument("--store-libs", action="store_true",
                    help="libraries uncompressed and page-aligned (manifest: extractNativeLibs=false)")
    a = ap.parse_args()

    bt = os.path.join(a.sdk, "build-tools", a.build_tools or newest(os.path.join(a.sdk, "build-tools")))
    platform = os.path.join(a.sdk, "platforms", a.platform or newest(os.path.join(a.sdk, "platforms"), "android-"))
    android_jar = os.path.join(platform, "android.jar")
    if not os.path.exists(android_jar):
        sys.exit(f"build_apk: {android_jar} missing (sdkmanager \"platforms;android-35\")")

    with tempfile.TemporaryDirectory(prefix="lgp_apk_") as work:
        base = os.path.join(work, "base.apk")
        compiled = []
        if a.res:
            res_zip = os.path.join(work, "res.zip")
            run([tool(bt, "aapt2"), "compile", "--dir", a.res, "-o", res_zip])
            compiled = [res_zip]
        run([tool(bt, "aapt2"), "link", "-o", base, "--manifest", a.manifest, "-I", android_jar,
             "--min-sdk-version", a.min_sdk, "--target-sdk-version", a.target_sdk] + compiled)

        sources = []
        for d in a.java_src:
            for root, _, files in os.walk(d):
                sources += [os.path.join(root, f) for f in files if f.endswith(".java")]
        classes = os.path.join(work, "classes")
        os.makedirs(classes)
        argfile = os.path.join(work, "sources.txt")
        with open(argfile, "w", encoding="utf-8") as f:
            f.write("\n".join('"' + s.replace("\\", "/") + '"' for s in sources))
        run([a.javac, "-nowarn", "-Xlint:-options", "-encoding", "utf-8", "-source", "8", "-target", "8",
             "-bootclasspath", android_jar, "-cp", android_jar, "-d", classes, "@" + argfile])
        classes_jar = os.path.join(work, "classes.jar")
        run([a.jar, "cf", classes_jar, "-C", classes, "."])
        dex_dir = os.path.join(work, "dex")
        os.makedirs(dex_dir)
        run([tool(bt, "d8"), "--release", "--min-api", a.min_sdk, "--lib", android_jar, "--output", dex_dir,
             classes_jar])

        unaligned = os.path.join(work, "unaligned.apk")
        shutil.copyfile(base, unaligned)
        with zipfile.ZipFile(unaligned, "a", zipfile.ZIP_DEFLATED) as z:
            z.write(os.path.join(dex_dir, "classes.dex"), "classes.dex")
            for lib in a.lib:
                if a.strip:
                    stripped = os.path.join(work, "stripped-" + os.path.basename(lib))
                    run([a.strip, "--strip-debug", "-o", stripped, lib])
                    print(f"build_apk: {os.path.basename(lib)} {os.path.getsize(lib) / 2**20:.1f} MiB, "
                          f"without debug sections {os.path.getsize(stripped) / 2**20:.1f} MiB")
                    lib_path = stripped
                else:
                    lib_path = lib
                z.write(lib_path, f"lib/{a.abi}/{os.path.basename(lib)}",
                        compress_type=zipfile.ZIP_STORED if a.store_libs else None)
            for asset in a.asset:
                name, sep, path = asset.partition("=")
                if not sep or not name or not os.path.isfile(path):
                    sys.exit(f"build_apk: --asset {asset}: expected NAME=FILE with an existing file")
                z.write(path, f"assets/{name}")
        aligned = os.path.join(work, "aligned.apk")
        run([tool(bt, "zipalign"), "-p", "-f", "4", unaligned, aligned])

        if not os.path.exists(a.keystore):
            create_keystore(a.keytool, a.keystore)
        tmp_out = a.out + ".tmp"
        run([tool(bt, "apksigner"), "sign", "--ks", a.keystore, "--ks-pass", "pass:android", "--key-pass",
             "pass:android", "--out", tmp_out, aligned])
        os.replace(tmp_out, a.out)
        idsig = tmp_out + ".idsig"
        if os.path.exists(idsig):
            os.remove(idsig)
    print(f"build_apk: {a.out}")
    # Android refuses to update an installed app signed with another key (INSTALL_FAILED_UPDATE_INCOMPATIBLE);
    # the only way out, uninstalling, deletes the app's saves. Print the digest so a wrong key shows up here.
    certs = subprocess.run([tool(bt, "apksigner"), "verify", "--print-certs", a.out], capture_output=True, text=True)
    for line in certs.stdout.splitlines():
        if "SHA-256 digest" in line:
            print(f"build_apk: key {a.keystore}, certificate SHA-256 {line.split(':', 1)[1].strip()}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
