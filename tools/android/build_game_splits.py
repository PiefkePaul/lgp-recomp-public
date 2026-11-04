#!/usr/bin/env python3
"""Pack the user's own game dump into split APKs for the Android app (P7.3).

    build_game_splits.py --game <dir with exefs/, romfs.bin [and systemdata/]> --out <dir>
                         --sdk <ANDROID_HOME> --keystore <debug.keystore of the app build>
                         [--package org.lgp.recomp.game] [--version-code 1] [--part-mib 1536]

Writes <out>/game-exefs.apk (exefs/ as assets/game/exefs/, systemdata/ as
assets/game/systemdata/ when there is one: the firmware's system fonts and
NgWord2; without them the runtime uses its stand-ins,
docs/knowledge/horizon-pl-shared-font.md; mods/ as assets/game/mods/ when the game folder has one: the mod
files the lifted hooks read, tools/kit/build.py) and
<out>/game-romfs-<n>.apk, each holding one part of romfs.bin as
assets/game/romfs.bin.<n>, stored uncompressed (AssetManager.openFd reads it
in place, GameBundle.java). The parts stay below 2 GiB because one APK cannot
pass 4 GiB: zipalign and apksigner have no Zip64 (romfs.bin of LGP: 4.2 GiB).
The splits carry no code; they must be signed with the same key and have the
same versionCode as the program's APK.

Install together with the app, from the PC:
    adb install-multiple lgp_game.apk <out>/game-*.apk
After a new program build, only the program (keeps the game splits; Verdacht,
checked on the device in docs/knowledge/android-ndk-build.md):
    adb install-multiple -p org.lgp.recomp.game lgp_game.apk

The splits ARE the game data: build them only on the own PC, from the own
dump below $LGP_PRIVATE_DIR, write them there and never commit, upload or put
them into CI.
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from build_apk import newest, run, tool  # noqa: E402
from shield_day import STD_FONT  # noqa: E402

MANIFEST = """<?xml version="1.0" encoding="utf-8"?>
<!-- Game data split (tools/android/build_game_splits.py). -->
<manifest xmlns:android="http://schemas.android.com/apk/res/android"
    package="{package}" split="{split}" android:versionCode="{version}">
    <application android:hasCode="false" />
</manifest>
"""


def build_split(a, bt, android_jar, work, split, add_assets):
    manifest = os.path.join(work, split + ".xml")
    with open(manifest, "w", encoding="utf-8") as f:
        f.write(MANIFEST.format(package=a.package, split=split, version=a.version_code))
    linked = os.path.join(work, split + ".linked.apk")
    run([tool(bt, "aapt2"), "link", "-o", linked, "--manifest", manifest, "-I", android_jar])
    unaligned = os.path.join(work, split + ".unaligned.apk")
    shutil.copyfile(linked, unaligned)
    with zipfile.ZipFile(unaligned, "a") as z:
        add_assets(z)
    aligned = os.path.join(work, split + ".aligned.apk")
    run([tool(bt, "zipalign"), "-p", "-f", "4", unaligned, aligned])
    os.remove(unaligned)
    out = os.path.join(a.out, "game-" + split.split("_", 1)[1].replace("_", "-") + ".apk")
    run([tool(bt, "apksigner"), "sign", "--ks", a.keystore, "--ks-pass", "pass:android", "--key-pass",
         "pass:android", "--out", out, aligned])
    os.remove(aligned)
    idsig = out + ".idsig"
    if os.path.exists(idsig):
        os.remove(idsig)
    print(f"build_game_splits: {out} ({os.path.getsize(out) / 2**20:.0f} MiB)")


def add_range(z, name, path, offset, length):
    info = zipfile.ZipInfo(name, date_time=(2020, 1, 1, 0, 0, 0))
    info.compress_type = zipfile.ZIP_STORED
    with open(path, "rb") as src, z.open(info, "w") as dst:
        src.seek(offset)
        left = length
        while left > 0:
            block = src.read(min(left, 8 << 20))
            if not block:
                sys.exit(f"build_game_splits: {path} ended early")
            dst.write(block)
            left -= len(block)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--game", required=True, help="dump folder with exefs/, romfs.bin and optionally systemdata/")
    ap.add_argument("--systemdata", help="system data folder (default: systemdata/ in --game)")
    ap.add_argument("--mods", help="mod folder (<id>.cfg, <id>/romfs/; default: mods/ in --game if there)")
    ap.add_argument("--out", required=True)
    ap.add_argument("--sdk", required=True)
    ap.add_argument("--keystore", required=True, help="the keystore the app's APK was signed with")
    ap.add_argument("--package", default="org.lgp.recomp.game")
    ap.add_argument("--version-code", default="1", help="must match the app's APK (AndroidManifest.xml.in)")
    ap.add_argument("--part-mib", type=int, default=1536, help="romfs.bin part size (below 2048)")
    ap.add_argument("--build-tools")
    ap.add_argument("--platform")
    a = ap.parse_args()

    exefs = os.path.join(a.game, "exefs")
    romfs_bin = os.path.join(a.game, "romfs.bin")
    if not os.path.isdir(exefs):
        sys.exit(f"build_game_splits: {exefs} missing")
    if not os.path.isfile(romfs_bin):
        sys.exit(f"build_game_splits: {romfs_bin} missing (pack romfs/ first: lgp_romfs_pack <romfs> <romfs.bin>)")
    systemdata = a.systemdata or os.path.join(a.game, "systemdata")
    mods = a.mods or os.path.join(a.game, "mods")
    if a.mods and not os.path.isdir(mods):
        sys.exit(f"build_game_splits: {mods} missing")
    if a.systemdata and not os.path.isdir(systemdata):
        sys.exit(f"build_game_splits: {systemdata} missing")
    if not os.path.isfile(os.path.join(systemdata, *STD_FONT)):
        print(f"build_game_splits: no {os.path.join(systemdata, *STD_FONT)}: the app uses the free stand-in font "
              "(docs/knowledge/horizon-pl-shared-font.md)")
    if not 1 <= a.part_mib < 2048:
        sys.exit("build_game_splits: --part-mib must be 1..2047")
    if not os.path.exists(a.keystore):
        sys.exit(f"build_game_splits: {a.keystore} missing (build the app's APK first; same key needed)")
    bt = os.path.join(a.sdk, "build-tools", a.build_tools or newest(os.path.join(a.sdk, "build-tools")))
    platform = os.path.join(a.sdk, "platforms", a.platform or newest(os.path.join(a.sdk, "platforms"), "android-"))
    android_jar = os.path.join(platform, "android.jar")
    os.makedirs(a.out, exist_ok=True)
    for old in os.listdir(a.out):
        if old.startswith("game-") and old.endswith(".apk"):
            os.remove(os.path.join(a.out, old))  # a smaller dump leaves no stale part behind

    with tempfile.TemporaryDirectory(prefix="lgp_splits_", dir=a.out) as work:
        def exefs_assets(z):
            for name in sorted(os.listdir(exefs)):
                p = os.path.join(exefs, name)
                if os.path.isfile(p):
                    z.write(p, "assets/game/exefs/" + name, zipfile.ZIP_DEFLATED)
            for folder, sub in ((systemdata, "systemdata"), (mods, "mods")):
                for top, dirs, names in os.walk(folder):  # nothing for a missing mods/
                    dirs.sort()
                    for name in sorted(names):
                        p = os.path.join(top, name)
                        rel = os.path.relpath(p, folder).replace(os.sep, "/")
                        z.write(p, f"assets/game/{sub}/" + rel, zipfile.ZIP_DEFLATED)
        build_split(a, bt, android_jar, work, "game_exefs", exefs_assets)

        size = os.path.getsize(romfs_bin)
        part = a.part_mib << 20
        for n, offset in enumerate(range(0, size, part)):
            length = min(part, size - offset)
            build_split(a, bt, android_jar, work, f"game_romfs_{n}",
                        lambda z, n=n, offset=offset, length=length:
                        add_range(z, f"assets/game/romfs.bin.{n}", romfs_bin, offset, length))
    print("install: adb install-multiple <lgp_game.apk> " + os.path.join(a.out, "game-*.apk"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
