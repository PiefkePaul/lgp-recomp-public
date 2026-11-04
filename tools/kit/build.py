#!/usr/bin/env python3
"""Builds the playable game from a friend's own dump (tools/kit/README.md, "Building the game").

Input is the unpacked dump as a ZIP or a folder with exefs/, romfs.bin,
lgp-kit.json and, from the kit server, game.cfg and mods/; or a game folder
unpacked by other means (exefs/ plus romfs.bin or romfs/, no lgp-kit.json)
with game.cfg and mods/ from --kit-files or --kit-url. Steps:

  1. check the toolchain (doctor) and the kit (the supported game version 1.0.0, game.cfg present)
  2. stage the game folder: unpack the ZIP, compress romfs.bin (zstd blocks, lgp_romfs_pack --compress)
  3. build lgp_lift and lgp_romfs_pack in a desktop Release tree
  4. lift main + sdk with --config game.cfg and --mods <game>/mods (every hook door of every mod file)
  5. build lgp_game with the game folder inside the package (LGP_APP_GAME_DIR): desktop = this host
     (Windows, macOS, Linux), ios = iPhone/iPad from a Mac with Xcode, or (draft) from Linux/WSL without Xcode
     with the SDK from the user's own Xcode.xip and xtool to sign (docs/knowledge/ios-ohne-mac.md); android = lgp_game.apk plus the game as
     split APKs (tools/android/build_game_splits.py), from any of the three. ios with --game-separate: a slim app
     without the game; the game folder is copied into the app's Documents/game once (Finder, Apple Devices app),
     and lgp-build-stamp.txt in it lets the app refuse a game folder of another build

Everything this writes (game folder, lifted C++, the game) holds game data or translated game code and stays on
this computer: the work folder defaults to $LGP_PRIVATE_DIR/kit-build, else build/kit in the repo (git-ignored).
Steps whose inputs did not change are skipped, so a second run after `git pull` only relifts and rebuilds.

  python3 tools/kit/build.py                     assistant: asks, installs missing tools, builds
  python3 tools/kit/build.py doctor [--target desktop|ios|android] [--compiler mingw|msvc]
  python3 tools/kit/build.py fetch-compiler      Windows: loads llvm-mingw into the tool cache (no Visual Studio)
  python3 tools/kit/build.py fetch-ffmpeg        Windows: loads FFmpeg (LGPL build) for the videos into the tool cache
  python3 tools/kit/build.py fetch-android       Android SDK, NDK and adb with Google's sdkmanager (needs a JDK)
  python3 tools/kit/build.py fetch-rcodesign     iOS without a Mac: rcodesign (puts the entitlements into the app)
  python3 tools/kit/build.py fetch-jdk           JDK as a folder in the tool cache (Android; nothing installed)
  python3 tools/kit/build.py install-android [--work DIR]   the built APKs onto the USB device (adb install-multiple)
  python3 tools/kit/build.py build --kit <dump.zip|folder> [--target desktop|ios|android] [--work DIR] [--dry-run] ...
                                                 (android: --abi x86_64 for the emulator's x86_64 image)
"""
from __future__ import annotations

import argparse
import glob
import hashlib
import json
import os
import platform
import re
import shutil
import ssl
import subprocess
import sys
import tarfile
import time
import urllib.parse
import urllib.request
import zipfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
# The ready-made Windows package (tools/kit/make_bauset.py) brings Python, CMake, Ninja and llvm-mingw in here.
BAUSET = REPO / "bauset"
# game.cfg and mods/ that come with the package (make_bauset.py --kit-files, never in git): used when the dump has none.
BUNDLED_KIT_FILES = REPO / "kit-files"
TITLE_ID = "010003f003a34000"
TITLE_ID_2 = "0100187003a36000"  # the game's second edition
MAIN_BUILD_ID = "1c375692de4f4b879abe88742dc0ca60"  # main of the supported game, version 1.0.0: the kit files' root
# Builds of main the lift is made for (the kit page lists the same): build ID -> (title ID, name). Every build but
# the first takes its game.cfg and mods from the kit files' subfolder named by its build ID
# (tools/analyze/addr_translate.py carries them over, docs/analysis/60-eevee-and-1-0-2.md).
SUPPORTED_BUILDS = {
    MAIN_BUILD_ID: (TITLE_ID, "version 1.0.0"),
    "c208db6a4ef4361f28b8402be0f7cd9a": (TITLE_ID, "version 1.0.2"),
    "0a3f0d21e67dad80bb39beeaedbb0bab": (TITLE_ID_2, "second edition, version 1.0.0"),
    "5831ec64d6b696fd69e69505821b97b1": (TITLE_ID_2, "second edition, version 1.0.2"),
}
LIFT_MODULES = ("main", "sdk")  # ADR-0005: subsdk0/subsdk1 are not lifted
LIFT_SPLIT = 64
MIN_CMAKE = (3, 25)
STAMP_VERSION = 1
# Mods of the kit that stay off on the first start (the rest is on, LGP_APP_DEFAULT_MODS): classic-exp is a switch
# that is off by default, online-button comes on by itself whenever its file is there (lgp/boot/app.hpp).
DEFAULT_OFF_MODS = {"classic-exp", "online-button"}
# System data from the console's firmware. Optional: without it the runtime uses
# a free stand-in font and answers the profanity filter itself (docs/knowledge/horizon-pl-shared-font.md).
OPTIONAL_SYSTEM_DATA = {"0100000000000811": "system font", "0100000000000823": "profanity filter NgWord2"}
# Windows compiler without Visual Studio: llvm-mingw (clang, lld, mingw-w64 on the UCRT; LLVM: Apache-2.0 with LLVM
# exception, mingw-w64: ZPL and others, see its LICENSE.TXT), loaded once into the tool cache. Version, folder,
# SHA-256 of https://github.com/mstorsjo/llvm-mingw/releases/download/<version>/<folder>.zip
LLVM_MINGW = ("20251021", "llvm-mingw-20251021-ucrt-x86_64",
              "0a0ce4733b54de8c4858ecfc2a08447de57728dccf9fbf7fcba3ce7e0d0c6af8")
# Windows videos: FFmpeg's libavcodec as an LGPL "shared" build (BtbN), loaded once into the tool cache and linked
# dynamically; its DLLs go next to lgp_game.exe (cmake/LgpFfmpeg.cmake). Release tag, folder, SHA-256 of
# https://github.com/BtbN/FFmpeg-Builds/releases/download/<tag>/<folder>.zip (PC test 2026-10-30: title video and
# its sound with llvm-mingw).
FFMPEG = ("autobuild-2026-07-31-14-10", "ffmpeg-n7.1.5-12-g1fdbca85aa-win64-lgpl-shared-7.1",
          "0f376f96fb38554ccefb1b2ae9c7c6a7b351f0e60a372b38262c320e8392c5d0")
# Parts of the release for other targets (ARM, 32 bit) and its Python: not needed, about half its size.
LLVM_MINGW_SKIP = ("aarch64-w64-mingw32/", "arm64ec-w64-mingw32/", "armv7-w64-mingw32/", "i686-w64-mingw32/",
                   "python/")
# Android (target android, docs/knowledge/android-ndk-build.md): NDK r30 (tested; r27c lacks std::atomic_ref), an SDK
# platform and build-tools for the APKs, adb to install. fetch-android loads them with Google's sdkmanager (licences
# are accepted in its own prompt); the JDK (≥ 17, for javac, d8 and apksigner) comes separately.
ANDROID_PACKAGES = ("platforms;android-35", "build-tools;35.0.0", "ndk;30.0.16248370", "platform-tools")
MIN_NDK, MIN_JDK = 30, 17
# Microsoft Build of OpenJDK as a plain folder in the tool cache (fetch-jdk), so Android needs no installed JDK
# (Paul 2026-10-26: nothing installed for good). Archives via https://aka.ms/download-jdk/<name>, SHA-256 from
# <name>.sha256sum.txt; each unpacks to jdk-21.0.8+9/ (macOS: …/Contents/Home inside).
JDK = ("21.0.8", "jdk-21.0.8+9", {
    "windows-x64": ("zip", "6ecfa864b95a45cde05d92351edf06d33b778669499b485bb8414145704fae0a"),
    "linux-x64": ("tar.gz", "da1cae7d08d41f32e87712bd3feac73faf7646a30476164c9e62a598dbd30aac"),
    "linux-aarch64": ("tar.gz", "1ac838ffca60f719f95a8751746c5e4594eca108f2ef27709a80edeef955a1dc"),
    "macos-aarch64": ("tar.gz", "b6b54d72a76c16cef488ba0b3179e3d9af0dfbb573d7283e8c573dd43c400e34"),
    "macos-x64": ("tar.gz", "9e8ad632305401c2fd2d1af90a4bd23ae2c817dc599b23ea85c5b703ec4a749e"),
})
# Parallel compile jobs: one per JOB_MIB of free memory (after RESERVE_MIB for the system), at most one per core.
# PC measurement 2026-10-28 (--measure, llvm-mingw, 803 steps): the largest step is a lifted*.cpp at 654-732 MiB,
# 90 % stay below 126 MiB; 12 jobs took about 5.5 GB in all. 1 GiB per job leaves room above the largest step.
JOB_MIB, RESERVE_MIB = 1024, 1024
# Google's command-line tools 23.0 (sdkmanager) per host: archive in https://dl.google.com/android/repository/ and its
# SHA-1 from repository2-3.xml there (Google lists no SHA-256 in it).
ANDROID_CMDLINE_TOOLS = {
    "win": ("commandlinetools-win-16111833_latest.zip", "57d04f2d75eb8e8fffc5000a987e5de4b5a63e9d"),
    "mac_arm64": ("commandlinetools-mac_arm64-16111833_latest.zip", "ad03dc49bfacfd52c110b14104ea548b8a07e830"),
    "mac_x86_64": ("commandlinetools-mac_x86_64-16111833_latest.zip", "112cf9618794a997ff273537d55bee02c22abffe"),
    "linux": ("commandlinetools-linux-16111833_latest.zip", "e025545c62a8e64c7559119566a569fb1dec5f60"),
}
ANDROID_PACKAGE_ID = "org.lgp.recomp.game"
# iOS without a Mac (preset ios-cross): the memory entitlements a free Apple ID's Personal Team can sign. Mac test
# 2026-10-25 (Xcode, free team): increased-memory-limit yes, extended-virtual-addressing no ("Personal development
# teams ... do not support the Extended Virtual Addressing capability"). Whether LGP runs with the limit alone is not
# measured yet (docs/knowledge/ios-ohne-mac.md); if not, set "none" (guest block on a file) or require a paid team.
# Default of --memory-entitlements for a build without a paid team.
FREE_TEAM_MEMORY_ENTITLEMENTS = "limit"
# --installer sideload: the user installs lgp_game.ipa with one of these. They take the entitlements from the
# executable's ad-hoc signature (make_ipa.py) and ask Apple for the matching capabilities; whether one keeps
# increased-memory-limit with a free Apple ID shows only on a device (the app logs "code signature: …" at start).
# Sideloadly drops the entitlements (ns2ios-shell; not checked by us), so it is not listed.
SIDELOADERS = (
    ("Impactor", "Windows, macOS, Linux", "https://github.com/claration/impactor/releases"),
    ("SideStore", "on the iPhone/iPad itself, needs a computer once", "https://sidestore.io"),
    ("AltStore", "Windows, macOS", "https://altstore.io"),
)
XTOOL_SDK_GLOBS = ("Developer/Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS*.sdk",
                   "*/Developer/Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS*.sdk")


class KitError(Exception):
    """A problem the user has to fix; printed without a traceback."""


def say(msg: str) -> None:
    print(msg, flush=True)


def cmake_path(p) -> str:
    """Forward slashes: CMake writes some -D paths into its own scripts, where a backslash is an escape."""
    return Path(p).as_posix()


def exe_name(name: str) -> str:
    return name + ".exe" if os.name == "nt" else name


# --- the kit ----------------------------------------------------------------------------------------------------


def safe_member(name: str) -> str | None:
    """The member's relative path, or None for directory entries; rejects absolute paths and '..'."""
    if name.endswith("/"):
        return None
    parts = name.replace("\\", "/").split("/")
    if name.startswith(("/", "\\")) or re.match(r"^[A-Za-z]:", name) or any(p in ("", ".", "..") for p in parts):
        raise KitError(f"The ZIP holds an invalid path: {name!r}")
    return "/".join(parts)


def read_kit_json(text: str) -> dict:
    try:
        info = json.loads(text)
    except ValueError as e:
        raise KitError(f"lgp-kit.json is not valid JSON: {e}") from None
    if not isinstance(info, dict):
        raise KitError("lgp-kit.json is not a JSON object")
    return info


# Notes of check_kit that the assistant has shown already; build() leaves them out (PC2 VM run 3: twice).
NOTES_SHOWN: set[str] = set()


def kit_build(info: dict) -> str | None:
    """The supported build of main (SUPPORTED_BUILDS key) that lgp-kit.json or a plain folder names, else None."""
    program = str(info.get("program_id", "")).lower().removeprefix("0x")
    main = next((m for m in info.get("exefs", []) if isinstance(m, dict) and m.get("name") == "main"), None)
    build_id = str(main.get("build_id", "")).lower() if main else ""
    build = next((b for b in SUPPORTED_BUILDS if build_id.startswith(b)), None)
    if build is None or info.get("lgp_supported") is False or (program and program != SUPPORTED_BUILDS[build][0]):
        return None
    return build


def kit_files_for(folder: Path, build: str) -> Path:
    """The kit files of `build`: the folder itself for the first build (1.0.0), otherwise its subfolder named by the build ID."""
    if build == MAIN_BUILD_ID:
        return folder
    sub = folder / build
    if not (sub / "manifest.json").is_file() and not (sub / "game.cfg").is_file():
        raise KitError(f"The kit files in {folder} have nothing for {SUPPORTED_BUILDS[build][1]} (subfolder {build} "
                       "with game.cfg and mods/ missing); game.cfg and the mods of another version do not fit.")
    return sub


def check_kit(info: dict, names: set[str]) -> list[str]:
    """Checks lgp-kit.json and the file list of a kit; returns warnings, raises KitError for hard problems."""
    if kit_build(info) is None:
        program = str(info.get("program_id", "")).lower().removeprefix("0x")
        main = next((m for m in info.get("exefs", []) if isinstance(m, dict) and m.get("name") == "main"), None)
        build_id = str(main.get("build_id", "")).lower() if main else ""
        raise KitError(
            "The dump is not one of the supported game versions "
            f"(program ID {program or '?'}, build ID of main {build_id[:16] or '?'}…). "
            "The lift, game.cfg and the mods fit only those.")
    for module in LIFT_MODULES + ("main.npdm",):
        if f"exefs/{module}" not in names:
            raise KitError(f"The dump lacks exefs/{module}")
    if "romfs.bin" not in names and not (info.get("plain") and any(n.startswith("romfs/") for n in names)):
        raise KitError("The dump lacks romfs.bin" + (" (or the folder romfs/)" if info.get("plain") else ""))
    if "game.cfg" not in names and info.get("plain"):
        raise KitError("game.cfg is missing: name the folder with game.cfg and mods/ with --kit-files (or the "
                       "kit server with --kit-url).")
    if "game.cfg" not in names:
        raise KitError(
            "The dump lacks game.cfg: name the folder with game.cfg and mods/ (the mods' release package) with "
            "--kit-files, or create the dump again on the kit page and tick the lift configuration and the mods.")
    warnings = []
    missing = [f"{what} ({data_id})" for data_id, what in OPTIONAL_SYSTEM_DATA.items()
               if not any(n.startswith(f"systemdata/{data_id}/") for n in names)]
    if missing:
        warnings.append("No firmware data in the dump (" + ", ".join(missing) + "): the game uses the free stand-in "
                        "font (M PLUS Rounded 1c) and filters no names. English text looks the same; Korean and "
                        "Chinese text is missing characters without the firmware. For those languages create the "
                        "dump again with the firmware folder.")
    if not any(n.startswith("mods/") and n.endswith(".cfg") for n in names):
        warnings.append("No mod files (mods/*.cfg) in the dump: the game is built without mods.")
    if "pipeline-seed.lgppipes" not in names:
        warnings.append("No pipeline seed (pipeline-seed.lgppipes): the game stutters at first, until its "
                        "shaders have been translated once.")
    return warnings


# What a plain game folder hands on to the game folder: anything else in it (the dump itself, logs, notes) stays out.
PLAIN_KEEP = ("exefs/", "romfs/", "romfs.bin", "systemdata/", "mods/", "game.cfg", "pipeline-seed.lgppipes",
              "control.nacp", "icon.jpg")


def is_plain(kit: Path) -> bool:
    """A game folder unpacked by other means: exefs/main but no lgp-kit.json."""
    return kit.is_dir() and not (kit / "lgp-kit.json").is_file() and (kit / "exefs" / "main").is_file()


def plain_member(rel: str) -> bool:
    return any(rel == k or (k.endswith("/") and rel.startswith(k)) for k in PLAIN_KEEP)


def plain_game_info(folder: Path) -> dict:
    """What lgp-kit.json would say, for a game folder unpacked by other means (exefs/ plus romfs.bin or romfs/):
    the program ID from exefs/main.npdm (META header, ACI0), the build ID from exefs/main's NSO header."""
    info: dict = {"plain": True, "title": f"unpacked game folder {folder.name}"}
    try:
        npdm = (folder / "exefs" / "main.npdm").read_bytes()
        if npdm[:4] == b"META" and len(npdm) >= 0x80:
            aci = int.from_bytes(npdm[0x70:0x74], "little")
            if npdm[aci:aci + 4] == b"ACI0":
                info["program_id"] = f"{int.from_bytes(npdm[aci + 0x10:aci + 0x18], 'little'):016x}"
    except OSError:
        pass
    try:
        with open(folder / "exefs" / "main", "rb") as f:
            head = f.read(0x60)
        if head[:4] == b"NSO0":
            info["exefs"] = [{"name": "main", "build_id": head[0x40:0x60].hex()}]
    except OSError:
        pass
    return info


def kit_listing(kit: Path) -> tuple[dict, set[str]]:
    if zipfile.is_zipfile(kit):
        with zipfile.ZipFile(kit) as z:
            names = {n for n in (safe_member(i.filename) for i in z.infolist()) if n}
            if "lgp-kit.json" not in names:
                raise KitError(f"{kit} holds no lgp-kit.json (not a dump ZIP of the kit?)")
            return read_kit_json(z.read("lgp-kit.json").decode("utf-8")), names
    if kit.is_dir():
        names = {p.relative_to(kit).as_posix() for p in kit.rglob("*") if p.is_file()}
        if not (kit / "lgp-kit.json").is_file():
            if is_plain(kit):  # unpacked by other means: identified by main's build ID
                return plain_game_info(kit), {n for n in names if plain_member(n)}
            raise KitError(f"{kit} holds neither lgp-kit.json nor exefs/main (not a game folder?)")
        return read_kit_json((kit / "lgp-kit.json").read_text(encoding="utf-8")), names
    raise KitError(f"{kit} is neither a ZIP nor a folder")


def kit_identity(kit: Path) -> str:
    """Cheap identity of the kit input (size and mtime of the ZIP or of each file)."""
    h = hashlib.sha256()
    paths = [kit] if kit.is_file() else sorted(p for p in kit.rglob("*") if p.is_file())
    if is_plain(kit):
        paths = [p for p in paths if plain_member(p.relative_to(kit).as_posix())]
    for p in paths:
        st = p.stat()
        h.update(f"{p.relative_to(kit) if p != kit else p.name}|{st.st_size}|{st.st_mtime_ns}\n".encode())
    return h.hexdigest()


def content_identity(folder: Path) -> str:
    """Identity of a small folder by content (the kit files): --kit-url writes them anew on every run, so their
    mtimes change although nothing else did."""
    h = hashlib.sha256()
    for p in sorted(p for p in folder.rglob("*") if p.is_file()):
        h.update(f"{p.relative_to(folder).as_posix()}|".encode() + hashlib.sha256(p.read_bytes()).digest())
    return h.hexdigest()


MIN_IOS_CLANG = 19  # the iPhoneOS 27 SDK's libc++ (__builtin_clzg, __GCC_DESTRUCTIVE_SIZE); built with 21 on the PC
KIT_PATH = re.compile(r"^[A-Za-z0-9._-]+(/[A-Za-z0-9._-]+)*$")  # as safeRelativePath of the kit page

# The dump directly (--dump, instead of a ZIP from the kit page) lives in its own file, which the public snapshot
# leaves out (tools/publish/public_snapshot.py); without it build.py takes kit ZIPs and unpacked game folders only.
DUMP = None


def kit_file_names(folder: Path) -> list[str]:
    """The files of a kit-files folder (game.cfg, mods): those its manifest.json lists, else every file in it."""
    manifest = folder / "manifest.json"
    if manifest.is_file():
        try:
            names = [f["path"] for f in json.loads(manifest.read_text(encoding="utf-8"))["files"]]
        except (ValueError, KeyError, TypeError):
            raise KitError(f"{manifest} is not a kit manifest (field files with path)") from None
    else:  # every file but those of the other builds' subfolders (kit_files_for)
        names = [rel for rel in (p.relative_to(folder).as_posix() for p in folder.rglob("*") if p.is_file())
                 if rel.split("/", 1)[0] not in SUPPORTED_BUILDS]
    for rel in names:
        if not isinstance(rel, str) or not KIT_PATH.match(rel) or any(s in (".", "..") for s in rel.split("/")):
            raise KitError(f"The kit files name an invalid path: {rel!r}")
    return sorted(n for n in names if (folder / n).is_file())


def bundled_kit_files() -> Path | None:
    """The package's own game.cfg and mods/ (kit-files/ next to tools/), if it brings them."""
    return BUNDLED_KIT_FILES if (BUNDLED_KIT_FILES / "game.cfg").is_file() else None


def fetch_kit_files(url: str, dest: Path) -> Path:
    """game.cfg, mods etc. from the kit server (<url>/kit/manifest.json and the files it lists) into dest, and the
    same for every other build the manifest names under "builds" (its subfolder <build ID>/, kit_files_for)."""
    base = url.rstrip("/") + "/kit/"
    say(f"== Fetching the kit files from {url}")

    def get(rel: str) -> bytes:
        try:
            with urllib.request.urlopen(base + urllib.parse.quote(rel), timeout=60) as r:
                return r.read()
        except OSError as e:
            raise KitError(f"Kit server {url} not reachable ({base + rel}: {e}). Check the address (--kit-url) "
                           "or create the dump once as a ZIP on the kit page.") from None

    def fetch_tree(prefix: str, into: Path) -> dict:
        raw = get(prefix + "manifest.json")
        try:
            manifest = json.loads(raw.decode("utf-8"))
            paths = [f["path"] for f in manifest["files"]]
        except (ValueError, KeyError, TypeError):
            raise KitError(f"{base}{prefix}manifest.json is not a kit manifest (field files with path)") from None
        into.mkdir(parents=True, exist_ok=True)
        for rel in paths:
            if not isinstance(rel, str) or not KIT_PATH.match(rel) or any(s in (".", "..") for s in rel.split("/")):
                raise KitError(f"The kit manifest names an invalid path: {rel!r}")
            target = into / rel
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(get(prefix + rel))
            say(f"   {prefix}{rel}")
        (into / "manifest.json").write_bytes(raw)
        return manifest

    part = dest.with_name(dest.name + ".part")
    shutil.rmtree(part, ignore_errors=True)
    builds = fetch_tree("", part).get("builds", [])
    for build in builds if isinstance(builds, list) else []:
        if build not in SUPPORTED_BUILDS or build == MAIN_BUILD_ID:
            raise KitError(f"The kit manifest names an unknown build under builds: {build!r}")
        fetch_tree(build + "/", part / build)
    shutil.rmtree(dest, ignore_errors=True)
    os.replace(part, dest)
    return dest


def stage_game(kit: Path, game: Path, raw_romfs: Path, move_romfs: bool = False) -> None:
    """Copies everything but romfs.bin to `game`; romfs.bin goes to `raw_romfs` (compressed later). move_romfs: the
    kit folder is build.py's own (--dump), so romfs.bin is moved instead of copied (no second 4.4 GB)."""
    if game.exists():
        shutil.rmtree(game)
    game.mkdir(parents=True)
    if kit.is_file():
        with zipfile.ZipFile(kit) as z:
            for info in z.infolist():
                name = safe_member(info.filename)
                if not name:
                    continue
                dst = raw_romfs if name == "romfs.bin" else game / name
                dst.parent.mkdir(parents=True, exist_ok=True)
                try:
                    with z.open(info) as src, open(dst, "wb") as out:
                        shutil.copyfileobj(src, out, 16 << 20)  # checks the member's CRC-32 at its end
                except (zipfile.BadZipFile, EOFError) as e:
                    raise KitError(f"The ZIP is damaged ({name}: {e}). Please create the dump again.") from None
    else:
        plain = is_plain(kit)
        for p in kit.rglob("*"):
            if not p.is_file():
                continue
            rel = p.relative_to(kit).as_posix()
            if plain and not plain_member(rel):
                continue
            if rel.startswith("romfs/"):  # a plain game folder's RomFS files: packed from there (Builder.stage)
                continue
            dst = raw_romfs if rel == "romfs.bin" else game / rel
            dst.parent.mkdir(parents=True, exist_ok=True)
            if move_romfs and rel == "romfs.bin":
                os.replace(p, dst)
            else:
                shutil.copyfile(p, dst)


def romfs_compressed(path: Path) -> bool:
    """romfs.bin as lgp_romfs_pack --compress writes it (magic LGPROMZ, lgp/hle/romfs.hpp)."""
    try:
        with open(path, "rb") as f:
            return f.read(8) == b"LGPROMZ\0"
    except OSError:
        return False


def copy_kit_files(folder: Path, game: Path) -> None:
    """--kit-files/--kit-url over a kit: game.cfg and the mods replace the kit's own."""
    for rel in kit_file_names(folder):
        dst = game / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(folder / rel, dst)


def derive_mod_files(game: Path, raw_romfs: Path) -> None:
    """Data mod files the kit hands out as recipes (mods/<id>/derive.json), built from the dump's RomFS."""
    mods = game / "mods"
    if not mods.is_dir() or not any(mods.glob("*/derive.json")):
        return
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import derive  # noqa: E402

    say("== Creating mod files from your own dump")
    try:
        written = derive.apply_all(derive.open_romfs(raw_romfs), mods, game / "exefs")
    except (derive.DeriveError, derive.sesd.SesdError, OSError, ValueError) as e:
        raise KitError(f"Could not create the mod files: {e}") from None
    say(f"   {len(written)} files")


# --- tools ------------------------------------------------------------------------------------------------------


def bauset_dirs() -> list[str]:
    """Tool folders of the ready-made package, ahead of anything installed (the versions it was tested with)."""
    return [str(d) for d in (BAUSET / "cmake" / "bin", BAUSET / "ninja", BAUSET / "node") if d.is_dir()]


def xtool_version(xtool: str) -> str | None:
    """What `xtool --version` prints (first line), or None when it cannot be read. xtool is not pinned."""
    try:
        out = subprocess.run([xtool, "--version"], capture_output=True, text=True, timeout=30).stdout
    except (OSError, subprocess.SubprocessError):
        return None
    line = out.strip().splitlines()[0].strip() if out.strip() else ""
    return line or None


def find_tool(name: str, extra_dirs: list[str] = ()) -> str | None:
    for d in bauset_dirs():
        if (Path(d) / exe_name(name)).is_file():
            return str(Path(d) / exe_name(name))
    found = shutil.which(name)
    if found:
        return found
    for d in list(extra_dirs) + known_tool_dirs():
        p = Path(d) / exe_name(name)
        if p.is_file():
            return str(p)
    return None


def known_tool_dirs() -> list[str]:
    """Where installers put the tools without (yet) being on this process's PATH: pip --user scripts, winget."""
    import sysconfig
    dirs = []
    for scheme in (f"{os.name}_user", "osx_framework_user" if sys.platform == "darwin" else None):
        if scheme and scheme in sysconfig.get_scheme_names():
            dirs.append(sysconfig.get_path("scripts", scheme))
    if os.name == "nt":
        dirs += windows_llvm_dirs()
        dirs += [os.path.join(os.environ.get(v, ""), "CMake", "bin") for v in ("ProgramFiles", "ProgramW6432")
                 if os.environ.get(v)]
        if os.environ.get("LOCALAPPDATA"):
            dirs.append(os.path.join(os.environ["LOCALAPPDATA"], "Microsoft", "WinGet", "Links"))
    elif sys.platform == "darwin":
        dirs += ["/opt/homebrew/bin", "/usr/local/bin"]
    return [d for d in dict.fromkeys(dirs) if d and os.path.isdir(d)]


def with_tool_dirs(env: dict[str, str]) -> dict[str, str]:
    env = dict(env)
    path_key = next((k for k in env if k.upper() == "PATH"), "PATH")
    parts = env.get(path_key, "").split(os.pathsep)
    env[path_key] = os.pathsep.join(bauset_dirs() + parts + [d for d in known_tool_dirs() if d not in parts])
    return env


def windows_llvm_dirs() -> list[str]:
    return [os.path.join(os.environ.get(v, ""), "LLVM", "bin") for v in ("ProgramFiles", "ProgramW6432")
            if os.environ.get(v)]


def msvc_environment() -> dict[str, str] | None:
    """The environment of vcvars64.bat (linker, CRT and Windows SDK, also needed by clang), or None."""
    if os.environ.get("VCToolsInstallDir"):
        return dict(os.environ)
    vswhere = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"),
                   "Microsoft Visual Studio", "Installer", "vswhere.exe")
    if not vswhere.is_file():
        return None
    out = subprocess.run([str(vswhere), "-latest", "-products", "*", "-requires",
                          "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath"],
                         capture_output=True, text=True)
    root = out.stdout.strip().splitlines()
    if out.returncode != 0 or not root:
        return None
    vcvars = Path(root[0], "VC", "Auxiliary", "Build", "vcvars64.bat")
    if not vcvars.is_file():
        return None
    out = subprocess.run(f'cmd /s /c ""{vcvars}" >nul && set"', capture_output=True, text=True, shell=True)
    if out.returncode != 0:
        return None
    env = {}
    for line in out.stdout.splitlines():
        key, sep, value = line.partition("=")
        if sep:
            env[key] = value
    return env


def cmake_version(cmake: str) -> tuple[int, ...]:
    out = subprocess.run([cmake, "--version"], capture_output=True, text=True).stdout
    m = re.search(r"(\d+)\.(\d+)", out)
    return tuple(int(x) for x in m.groups()) if m else (0, 0)


def find_cmake() -> str | None:
    """The first CMake that is new enough: on PATH, then where pip/winget put one (an older one may come first)."""
    candidates = ([str(Path(d) / exe_name("cmake")) for d in bauset_dirs()] + [shutil.which("cmake")] +
                  [str(Path(d) / exe_name("cmake")) for d in known_tool_dirs()])
    found = [c for c in dict.fromkeys(candidates) if c and Path(c).is_file()]
    for c in found:
        if cmake_version(c) >= MIN_CMAKE:
            return c
    return found[0] if found else None


def portable() -> bool:
    """Run from the ready-made package (make_bauset.py): its tools live in bauset/, nothing gets installed."""
    return (BAUSET / "python").is_dir() or (BAUSET / "cmake").is_dir()


def free_memory_mib() -> int | None:
    """Memory free for new processes now (Linux MemAvailable, Windows ullAvailPhys, macOS free + inactive pages)."""
    try:
        if os.name == "nt":
            import ctypes

            class Status(ctypes.Structure):
                _fields_ = [("dwLength", ctypes.c_ulong), ("dwMemoryLoad", ctypes.c_ulong)] + [
                    (n, ctypes.c_ulonglong) for n in ("total_phys", "avail_phys", "total_pf", "avail_pf",
                                                      "total_virtual", "avail_virtual", "avail_ext_virtual")]
            s = Status()
            s.dwLength = ctypes.sizeof(s)
            return s.avail_phys >> 20 if ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(s)) else None
        if sys.platform == "darwin":
            out = subprocess.run(["vm_stat"], capture_output=True, text=True, timeout=10).stdout
            page = int(re.search(r"page size of (\d+)", out).group(1))
            pages = sum(int(m) for m in re.findall(r"Pages (?:free|inactive|speculative):\s+(\d+)", out))
            return pages * page >> 20
        m = re.search(r"MemAvailable:\s+(\d+) kB", Path("/proc/meminfo").read_text())
        return int(m.group(1)) >> 10 if m else None
    except (OSError, ValueError, AttributeError, subprocess.SubprocessError):
        return None


def auto_jobs(cpus: int, free_mib: int | None) -> int:
    """Compile jobs that fit: weak machines with many cores but little RAM would otherwise swap or abort."""
    if free_mib is None:
        return max(1, cpus)
    return max(1, min(cpus, (free_mib - RESERVE_MIB) // JOB_MIB))


def clamp_future_mtimes(root: Path, skip: tuple[Path, ...] = (), slack: float = 60.0) -> int:
    """Sets files under root whose modification time lies in the future to now; returns how many.
    ZIP entries carry local time without a zone: a package packed in UTC+1 and unpacked in UTC-5 holds sources five
    hours ahead, and Ninja then stops with "manifest 'build.ninja' still dirty after 100 tries" (Mac VM, 2026-11-03)."""
    now = time.time()
    skipped = {os.path.normcase(os.path.abspath(p)) for p in skip}
    count = 0
    for top, dirs, files in os.walk(root):
        dirs[:] = [d for d in dirs if d != ".git" and not (top == str(root) and d == "build")  # developers' trees
                   and os.path.normcase(os.path.join(top, d)) not in skipped]
        for name in files:
            path = os.path.join(top, name)
            try:
                if os.lstat(path).st_mtime > now + slack and not os.path.islink(path):
                    os.utime(path, (now, now))
                    count += 1
            except OSError:
                pass
    return count


def cached_tools() -> str:
    """The tools build.py has loaded into the tool cache, by folder name (llvm-mingw, JDK, Android SDK, Node.js …)."""
    try:
        names = sorted(p.name for p in tool_cache().iterdir() if p.is_dir() and not p.name.endswith(".tmp"))
    except OSError:
        names = []
    return ", ".join(names) or "empty"


def outside_folder(work: Path) -> list[str]:
    """What this build left outside the repo folder (shown at the end; the ready-made package deletes cleanly)."""
    found = []
    for path, what in ((tool_cache(), f"tools ({cached_tools()})"),
                       (user_data_dir(), "Android signing key: keep it, otherwise updates only work after "
                                         "uninstalling (which deletes the saves)"),
                       (work, "work folder with game data")):
        try:
            path.resolve().relative_to(REPO.resolve())
        except ValueError:
            if path.exists():
                found.append(f"{path}: {what}")
    return found


def tool_cache() -> Path:
    """Tools build.py loads itself (llvm-mingw): per user, outside the repo, kept between kits and work folders."""
    if os.environ.get("LGP_KIT_TOOLS"):
        return Path(os.environ["LGP_KIT_TOOLS"])
    if (BAUSET / LLVM_MINGW[1]).is_dir():
        return BAUSET
    base = os.environ.get("LOCALAPPDATA") or os.path.join(os.path.expanduser("~"), ".cache")
    return Path(base, "lgp-recomp", "kit-tools")


def certificate_error(e: BaseException) -> bool:
    return isinstance(e, ssl.SSLCertVerificationError) or isinstance(getattr(e, "reason", None),
                                                                      ssl.SSLCertVerificationError)


def windows_curl() -> str | None:
    return shutil.which("curl.exe") if os.name == "nt" else None


def download(url: str, part: Path, algo: str, digest: str, what: str) -> None:
    """url into part, checked against its digest (hashlib name algo); a wrong one deletes part.
    A fresh Windows lacks most root certificates until Windows' own TLS (Schannel) fetches them on first use;
    Python only sees those already in the store (VM test 2026-10-30: github.com failed). curl.exe, part of
    Windows since 10 1803, goes through Schannel, so it takes over on a certificate error."""
    say(f"== Downloading {url} ({what})")
    h = hashlib.new(algo)
    try:
        with urllib.request.urlopen(url) as r, open(part, "wb") as out:
            total, done, shown = int(r.headers.get("Content-Length") or 0), 0, -10
            while chunk := r.read(1 << 20):
                out.write(chunk)
                h.update(chunk)
                done += len(chunk)
                if total and done * 100 // total >= shown + 10:
                    shown = done * 100 // total
                    say(f"   {shown} %")
    except OSError as e:
        curl = windows_curl()
        if not (certificate_error(e) and curl):
            if part.exists():
                part.unlink()
            raise KitError(f"Download of {url} failed: {e}. Check the internet connection"
                           + (" (or a proxy/virus scanner that intercepts HTTPS)." if certificate_error(e) else ".")
                           ) from None
        say("   Python does not know the certificate yet; loading with Windows' curl instead")
        if subprocess.run([curl, "-fL", "--retry", "2", "-o", str(part), url]).returncode != 0:
            if part.exists():
                part.unlink()
            raise KitError(f"Download of {url} failed (also with curl). Check the internet connection.") from None
        h = hashlib.new(algo)
        with open(part, "rb") as f:
            while chunk := f.read(1 << 20):
                h.update(chunk)
    if h.hexdigest() != digest:
        part.unlink()
        raise KitError(f"{url.rsplit('/', 1)[1]}: wrong checksum ({h.hexdigest()}), download discarded.")


# iOS without a Mac: rcodesign (apple-codesign, MPL-2.0, static musl build) signs the executable ad hoc with the
# entitlements before xtool, which takes the entitlements only from the executable's signature (XKit
# DeveloperServicesAddAppOperation: no signature = no entitlements, so no increased-memory-limit).
RCODESIGN = ("0.29.0", {
    "x86_64": "dbe85cedd8ee4217b64e9a0e4c2aef92ab8bcaaa41f20bde99781ff02e600002",
    "aarch64": "4af92c87ddf52f5f2d1258a3b4e56c7dcb8f1b2468df744976c5f139e031961f",
})


def rcodesign_dir() -> Path:
    arch = "aarch64" if platform.machine().lower() in ("aarch64", "arm64") else "x86_64"
    return tool_cache() / f"apple-codesign-{RCODESIGN[0]}-{arch}-unknown-linux-musl"


def find_signer() -> str | None:
    """rcodesign (tool cache or PATH) or ldid, for tools/ios/make_ipa.py."""
    return find_tool("rcodesign", [str(rcodesign_dir())]) or find_tool("ldid")


def fetch_rcodesign() -> Path:
    """Downloads and unpacks the pinned rcodesign for Linux (checked against its SHA-256); returns its folder."""
    target = rcodesign_dir()
    if (target / "rcodesign").is_file():
        return target
    if not sys.platform.startswith("linux"):
        raise KitError("rcodesign is only loaded on Linux/WSL (iOS without a Mac); on a Mac, Xcode signs.")
    arch = target.name.split("-")[3]
    version, digests = RCODESIGN
    tool_cache().mkdir(parents=True, exist_ok=True)
    part = tool_cache() / f"{target.name}.tar.gz.part"
    url = (f"https://github.com/indygreg/apple-platform-rs/releases/download/apple-codesign%2F{version}/"
           f"{target.name}.tar.gz")
    download(url, part, "sha256", digests[arch], "about 11 MB")
    tmp = tool_cache() / f"{target.name}.tmp"
    shutil.rmtree(tmp, ignore_errors=True)
    with tarfile.open(part) as t:
        for member in t.getmembers():
            if member.isfile() and member.name in (f"{target.name}/rcodesign", f"{target.name}/COPYING"):
                safe_member(member.name)
                t.extract(member, tmp, **({"filter": "data"} if hasattr(tarfile, "data_filter") else {}))
    os.chmod(tmp / target.name / "rcodesign", 0o755)
    os.replace(tmp / target.name, target)
    shutil.rmtree(tmp, ignore_errors=True)
    part.unlink()
    return target


def llvm_mingw_bin() -> Path:
    return tool_cache() / LLVM_MINGW[1] / "bin"


def fetch_llvm_mingw() -> Path:
    """Downloads and unpacks the pinned llvm-mingw (checked against its SHA-256); returns its bin folder."""
    version, folder, sha = LLVM_MINGW
    target = tool_cache() / folder
    if (target / "bin" / "x86_64-w64-mingw32-clang++.exe").is_file():
        return target / "bin"
    tool_cache().mkdir(parents=True, exist_ok=True)
    part = tool_cache() / f"{folder}.zip.part"
    url = f"https://github.com/mstorsjo/llvm-mingw/releases/download/{version}/{folder}.zip"
    download(url, part, "sha256", sha, "about 180 MB")
    tmp = tool_cache() / f"{folder}.tmp"
    shutil.rmtree(tmp, ignore_errors=True)
    say("== Unpacking llvm-mingw")
    with zipfile.ZipFile(part) as z:
        for info in z.infolist():
            rel = info.filename.partition("/")[2]
            if not rel or rel.startswith(LLVM_MINGW_SKIP):
                continue
            safe_member(info.filename)
            z.extract(info, tmp)
    os.replace(tmp / folder, target)
    shutil.rmtree(tmp, ignore_errors=True)
    part.unlink()
    return target / "bin"


def ffmpeg_dir() -> Path:
    return tool_cache() / FFMPEG[1]


def ffmpeg_ready() -> bool:
    return any((ffmpeg_dir() / "bin").glob("avcodec-*.dll"))


def fetch_ffmpeg() -> Path:
    """Downloads and unpacks the pinned FFmpeg (checked against its SHA-256); returns its folder (bin/, include/, lib/)."""
    tag, folder, sha = FFMPEG
    target = ffmpeg_dir()
    if ffmpeg_ready():
        return target
    tool_cache().mkdir(parents=True, exist_ok=True)
    part = tool_cache() / f"{folder}.zip.part"
    url = f"https://github.com/BtbN/FFmpeg-Builds/releases/download/{tag}/{folder}.zip"
    download(url, part, "sha256", sha, "about 60 MB")
    tmp = tool_cache() / f"{folder}.tmp"
    shutil.rmtree(tmp, ignore_errors=True)
    say("== Unpacking FFmpeg")
    with zipfile.ZipFile(part) as z:
        for info in z.infolist():
            safe_member(info.filename)
            z.extract(info, tmp)
    os.replace(tmp / folder, target)
    shutil.rmtree(tmp, ignore_errors=True)
    part.unlink()
    return target


def user_data_dir() -> Path:
    """Per-user folder for what must outlive work folders and packages (the Android signing key)."""
    if os.name == "nt" and os.environ.get("LOCALAPPDATA"):
        return Path(os.environ["LOCALAPPDATA"], "lgp-recomp")
    if sys.platform == "darwin":
        return Path.home() / "Library" / "Application Support" / "lgp-recomp"
    return Path(os.environ.get("XDG_DATA_HOME") or Path.home() / ".local" / "share", "lgp-recomp")


def android_keystore() -> Path:
    """One signing key per user for all builds: Android only updates an app signed with the same key, and
    uninstalling (the only way around it) deletes the saves. As in cmake/LgpAndroidApk.cmake for LGP_PRIVATE_DIR."""
    if os.environ.get("LGP_PRIVATE_DIR"):
        return Path(os.environ["LGP_PRIVATE_DIR"], "android", "lgp-debug.keystore")
    return user_data_dir() / "android" / "lgp-debug.keystore"


def android_sdk_candidates() -> list[Path]:
    dirs = [os.environ.get(v) for v in ("LGP_ANDROID_SDK", "ANDROID_HOME", "ANDROID_SDK_ROOT")]
    if os.name == "nt" and os.environ.get("LOCALAPPDATA"):
        dirs.append(os.path.join(os.environ["LOCALAPPDATA"], "Android", "Sdk"))  # Android Studio
    elif sys.platform == "darwin":
        dirs.append(str(Path.home() / "Library" / "Android" / "sdk"))
    else:
        dirs.append(str(Path.home() / "Android" / "Sdk"))
    dirs.append(str(tool_cache() / "android-sdk"))
    return [Path(d) for d in dict.fromkeys(dirs) if d]


def android_sdk_ready(sdk: Path) -> bool:
    return any(sdk.glob("platforms/android-*/android.jar")) and any(sdk.glob("build-tools/*/apksigner*"))


def android_sdk() -> Path:
    """The first SDK with a platform and build-tools; else the first one that exists, else the tool cache's
    (where fetch-android installs)."""
    found = android_sdk_candidates()
    for sdk in found:
        if android_sdk_ready(sdk):
            return sdk
    return next((sdk for sdk in found if sdk.is_dir()), found[-1])


def ndk_major(ndk: Path) -> int:
    try:
        text = (ndk / "source.properties").read_text(encoding="utf-8", errors="replace")
    except OSError:
        return 0
    m = re.search(r"^Pkg\.Revision\s*=\s*(\d+)", text, re.M)
    return int(m.group(1)) if m else 0


def android_ndk(sdk: Path) -> Path | None:
    """ANDROID_NDK_HOME/ROOT if new enough, else the newest NDK ≥ r30 in the SDK."""
    for v in ("ANDROID_NDK_HOME", "ANDROID_NDK_ROOT"):
        if os.environ.get(v) and ndk_major(Path(os.environ[v])) >= MIN_NDK:
            return Path(os.environ[v])
    ndks = sorted((p for p in sdk.glob("ndk/*") if ndk_major(p) >= MIN_NDK),
                  key=lambda p: [int(x) for x in re.findall(r"\d+", p.name)])
    return ndks[-1] if ndks else None


def jdk_major(javac: Path) -> int:
    try:
        r = subprocess.run([str(javac), "-version"], capture_output=True, text=True, timeout=60)
    except (OSError, subprocess.SubprocessError):
        return 0
    m = re.search(r"javac (\d+)", r.stdout + r.stderr)
    return int(m.group(1)) if m else 0


def jdk_host() -> str:
    arch = "aarch64" if platform.machine().lower() in ("aarch64", "arm64") else "x64"
    system = "windows" if os.name == "nt" else "macos" if sys.platform == "darwin" else "linux"
    return f"{system}-{arch}"


def portable_jdk_dir() -> Path:
    return tool_cache() / f"microsoft-jdk-{JDK[0]}-{jdk_host()}"


def portable_jdk_home() -> Path:
    home = portable_jdk_dir()
    return home / "Contents" / "Home" if (home / "Contents" / "Home").is_dir() else home


def fetch_jdk() -> Path:
    """Downloads and unpacks the pinned JDK into the tool cache (checked against its SHA-256); returns its home."""
    if Path(portable_jdk_home(), "bin", exe_name("javac")).is_file():
        return portable_jdk_home()
    version, top, archives = JDK
    if jdk_host() not in archives:
        raise KitError(f"No JDK to download for {jdk_host()}: install https://adoptium.net/ (Temurin 21).")
    kind, sha = archives[jdk_host()]
    target = portable_jdk_dir()
    tool_cache().mkdir(parents=True, exist_ok=True)
    name = f"microsoft-jdk-{version}-{jdk_host()}.{kind}"
    part = tool_cache() / f"{name}.part"
    download(f"https://aka.ms/download-jdk/{name}", part, "sha256", sha, "about 200 MB")
    tmp = tool_cache() / f"{target.name}.tmp"
    shutil.rmtree(tmp, ignore_errors=True)
    say("== Unpacking the JDK")
    if kind == "zip":
        with zipfile.ZipFile(part) as z:
            for info in z.infolist():
                if safe_member(info.filename):
                    z.extract(info, tmp)
    else:
        with tarfile.open(part) as tf:
            for member in tf.getmembers():
                name = member.name.removeprefix("./")
                if name not in ("", "."):
                    safe_member(name + ("/" if member.isdir() else ""))
            tf.extractall(tmp, **({"filter": "data"} if hasattr(tarfile, "data_filter") else {}))
    shutil.rmtree(target, ignore_errors=True)
    tops = list(tmp.iterdir())
    os.replace(tops[0] if len(tops) == 1 else tmp / top, target)
    shutil.rmtree(tmp, ignore_errors=True)
    part.unlink()
    return portable_jdk_home()


def find_jdk() -> Path | None:
    """A JDK ≥ 17 (its home folder): JAVA_HOME, PATH, then where winget/Adoptium/Android Studio put one."""
    homes = [str(portable_jdk_home()), os.environ.get("JAVA_HOME")]
    on_path = shutil.which("javac")
    if on_path:
        homes.append(str(Path(on_path).resolve().parent.parent))
    if os.name == "nt":
        for v in ("ProgramFiles", "ProgramW6432"):
            if os.environ.get(v):
                for vendor in ("Microsoft", "Eclipse Adoptium", "Java"):
                    homes += sorted(glob.glob(os.path.join(os.environ[v], vendor, "jdk-*")), reverse=True)
                homes.append(os.path.join(os.environ[v], "Android", "Android Studio", "jbr"))
    elif sys.platform == "darwin":
        homes += sorted(glob.glob("/Library/Java/JavaVirtualMachines/*/Contents/Home"), reverse=True)
        homes.append("/Applications/Android Studio.app/Contents/jbr/Contents/Home")
    else:
        homes += sorted(glob.glob("/usr/lib/jvm/*"), reverse=True)
    for home in dict.fromkeys(h for h in homes if h):
        javac = Path(home, "bin", exe_name("javac"))
        if javac.is_file() and jdk_major(javac) >= MIN_JDK:
            return Path(home)
    return None


def cmdline_tools_host() -> str:
    if os.name == "nt":
        return "win"
    if sys.platform == "darwin":
        return "mac_arm64" if platform.machine() == "arm64" else "mac_x86_64"
    return "linux"


def sdkmanager(sdk: Path) -> Path | None:
    for d in [sdk / "cmdline-tools" / "latest"] + sorted(sdk.glob("cmdline-tools/*"), reverse=True):
        for name in ("sdkmanager.bat", "sdkmanager"):
            if (d / "bin" / name).is_file():
                return d / "bin" / name
    return None


def install_android(out: Path, sdk: Path) -> None:
    """adb install-multiple of lgp_game.apk and the game splits in out onto the one connected device
    (ANDROID_SERIAL picks one of several)."""
    adb = sdk / "platform-tools" / exe_name("adb")
    if not adb.is_file():
        raise KitError(f"adb is missing ({adb}): python tools/kit/build.py fetch-android")
    apks = [out / "lgp_game.apk", *sorted(out.glob("game-*.apk"))]
    if not apks[0].is_file() or len(apks) == 1:
        raise KitError(f"{out} lacks lgp_game.apk or the game-*.apk; build first (--target android)")
    listing = subprocess.run([str(adb), "devices"], capture_output=True, text=True).stdout
    rows = [line.split("\t") for line in listing.splitlines()[1:] if "\t" in line]
    ready = [r[0] for r in rows if r[1].strip() == "device"]
    if any(r[1].strip() == "unauthorized" for r in rows) and not ready:
        raise KitError("The device asks \"Allow USB debugging?\": confirm it on the device, then run this again.")
    if not ready:
        raise KitError("No Android device found: connect it by USB and switch on USB debugging in the developer options "
                       "(Settings > About phone > tap Build number 7 times).")
    if len(ready) > 1 and not os.environ.get("ANDROID_SERIAL"):
        raise KitError(f"Several devices ({', '.join(ready)}): set ANDROID_SERIAL=<one of them>.")
    say(f"== Installing on {os.environ.get('ANDROID_SERIAL') or ready[0]} ({len(apks)} APKs, about 2.3 GB; "
        "takes a few minutes, the saves stay)")
    if subprocess.run([str(adb), "install-multiple", "-r", *map(str, apks)]).returncode != 0:
        raise KitError("adb install-multiple failed (message above). If the app was built with another key "
                       "before, only uninstalling helps, and that deletes the saves.")
    say("Installed. On the device the app is called \"LGP\".")


def fetch_android() -> Path:
    """Installs ANDROID_PACKAGES with sdkmanager (loaded first if the SDK has none); returns the SDK."""
    jdk = find_jdk()
    if not jdk:
        raise KitError(f"The Android SDK needs a JDK ≥ {MIN_JDK}: https://adoptium.net/ (Temurin 21), then "
                       "start again.")
    sdk = android_sdk()
    manager = sdkmanager(sdk)
    if not manager:
        archive, sha1 = ANDROID_CMDLINE_TOOLS[cmdline_tools_host()]
        sdk.mkdir(parents=True, exist_ok=True)
        part = sdk / f"{archive}.part"
        download(f"https://dl.google.com/android/repository/{archive}", part, "sha1", sha1, "about 150 MB")
        tmp = sdk / "cmdline-tools" / "latest.tmp"
        shutil.rmtree(tmp, ignore_errors=True)
        with zipfile.ZipFile(part) as z:
            for info in z.infolist():
                rel = safe_member(info.filename)
                if rel is None or not rel.startswith("cmdline-tools/"):
                    continue
                out = tmp / rel[len("cmdline-tools/"):]
                if info.is_dir():
                    continue
                out.parent.mkdir(parents=True, exist_ok=True)
                with z.open(info) as src, open(out, "wb") as dst:
                    shutil.copyfileobj(src, dst, 1 << 20)
                mode = info.external_attr >> 16
                if mode:
                    os.chmod(out, mode & 0o777)
        shutil.rmtree(sdk / "cmdline-tools" / "latest", ignore_errors=True)
        os.replace(tmp, sdk / "cmdline-tools" / "latest")
        part.unlink()
        manager = sdkmanager(sdk)
    env = dict(os.environ, JAVA_HOME=str(jdk))
    say(f"== Android SDK into {sdk}: {', '.join(ANDROID_PACKAGES)} (about 1.2 GB in all; accept the licences with y)")
    cmd = [str(manager), f"--sdk_root={sdk}", "--install", *ANDROID_PACKAGES]
    say("$ " + " ".join(cmd))
    if subprocess.run(cmd, env=env).returncode != 0:
        raise KitError("sdkmanager reports an error (licence not accepted or download cut off).")
    return sdk



def ios_sdk() -> Path | None:
    """The iPhoneOS SDK for ios-cross: LGP_IOS_SDK, else the newest in xtool's Swift SDK bundle (from Xcode.xip)."""
    if os.environ.get("LGP_IOS_SDK"):
        sdk = Path(os.environ["LGP_IOS_SDK"])
        return sdk if (sdk / "SDKSettings.json").is_file() or (sdk / "SDKSettings.plist").is_file() else None
    bundle = Path.home() / ".swiftpm" / "swift-sdks" / "darwin.artifactbundle"
    found = sorted({p for g in XTOOL_SDK_GLOBS for p in bundle.glob(g) if (p / "SDKSettings.json").is_file()},
                   key=lambda p: (not p.is_symlink(), p.name))
    return found[-1] if found else None


def find_llvm_tool(name: str) -> str | None:
    """lld's Mach-O linker and friends as cmake/toolchains/ios-cross.cmake picks them: Debian's versioned names
    (ld64.lld-21 and the like) newest first, then the plain name."""
    return next((find_tool(f"{name}-{v}") for v in range(25, MIN_IOS_CLANG - 1, -1) if find_tool(f"{name}-{v}")),
                None) or find_tool(name)


def clang_major(clang: str) -> int:
    try:
        out = subprocess.run([clang, "--version"], capture_output=True, text=True, timeout=30).stdout
    except (OSError, subprocess.SubprocessError):
        return 0
    m = re.search(r"clang version (\d+)", out)
    return int(m.group(1)) if m else 0


def apt_llvm_version() -> int | None:
    """The newest clang-N (N ≥ MIN_IOS_CLANG) apt can install (Ubuntu 24.04: up to 20, 26.04: 21)."""
    for v in range(25, MIN_IOS_CLANG - 1, -1):
        r = subprocess.run(["apt-cache", "show", f"clang-{v}"], capture_output=True, text=True)
        if r.returncode == 0 and r.stdout.strip():
            return v
    return None


def in_wsl() -> bool:
    try:
        return "microsoft" in Path("/proc/version").read_text().lower()
    except OSError:
        return False


class Toolchain:
    def __init__(self, target: str, compiler: str = "mingw", need_node: bool = False, ffmpeg: str | None = None):
        self.target = target
        self.ffmpeg = ffmpeg  # Windows: --ffmpeg-dir (a folder or "none"); without it the tool cache's
        self.need_node = need_node  # the dump directly (--dump)
        self.compiler = compiler  # Windows: mingw (llvm-mingw from the tool cache) or msvc (Visual Studio + LLVM)
        self.env = dict(os.environ)
        self.cmake = find_tool("cmake")
        self.ninja = find_tool("ninja")
        self.cc = self.cxx = None
        self.android_sdk: Path | None = None
        self.problems: list[str] = []
        self.optional: list[str] = []  # missing, but the build goes on without it (FFmpeg: black videos)
        self.notes: list[str] = []
        self.installs: list[tuple[str, list[str]]] = []  # (what, command) that would fix a problem

    def check(self) -> bool:
        self.problems, self.optional, self.notes, self.installs = [], [], [], []
        if self.target == "extract":  # only unpacking a dump (Windows before handing over to WSL)
            if DUMP and DUMP.node_problem():
                self.problems.append(DUMP.node_problem())
            self.plan_installs()
            return not self.problems
        self.cmake, self.ninja = find_cmake(), find_tool("ninja")
        if sys.version_info < (3, 9):
            self.problems.append("Python ≥ 3.9 is needed: https://www.python.org/downloads/")
        if not self.cmake:
            self.problems.append(f"CMake ≥ {MIN_CMAKE[0]}.{MIN_CMAKE[1]} is missing: https://cmake.org/download/")
        elif cmake_version(self.cmake) < MIN_CMAKE:
            self.problems.append(f"CMake is too old ({'.'.join(map(str, cmake_version(self.cmake)))}), needed ≥ "
                                 f"{MIN_CMAKE[0]}.{MIN_CMAKE[1]}: https://cmake.org/download/")
        if not self.ninja:
            self.problems.append("Ninja is missing: https://github.com/ninja-build/ninja/releases (put ninja on the PATH)")
        if os.name == "nt" and self.target == "desktop" and not self.ffmpeg and not ffmpeg_ready():
            self.optional.append(f"FFmpeg for the videos (LGPL build {FFMPEG[1]}; without it the videos stay "
                                 "black): python tools/kit/build.py fetch-ffmpeg")
        if os.name == "nt" and self.compiler == "mingw":
            bin_dir = llvm_mingw_bin()
            self.cc, self.cxx = (str(bin_dir / f"x86_64-w64-mingw32-{c}.exe") for c in ("clang", "clang++"))
            if not Path(self.cxx).is_file():
                self.cc = self.cxx = None
                self.problems.append(f"Compiler is missing (llvm-mingw {LLVM_MINGW[0]}, no Visual Studio needed): "
                                     "python tools/kit/build.py fetch-compiler")
            else:
                path_key = next((k for k in self.env if k.upper() == "PATH"), "PATH")
                self.env[path_key] = str(bin_dir) + os.pathsep + self.env.get(path_key, "")
        elif os.name == "nt":
            env = msvc_environment()
            if env is None:
                self.problems.append(
                    "Visual Studio Build Tools with \"Desktop development with C++\" are missing (linker, CRT, Windows SDK; "
                    "clang needs them too): https://visualstudio.microsoft.com/visual-cpp-build-tools/")
            else:
                self.env = env
            llvm = windows_llvm_dirs()
            self.cc, self.cxx = find_tool("clang", llvm), find_tool("clang++", llvm)
            if not self.cxx:
                self.problems.append("LLVM/clang is missing (MSVC is not enough: only clang guarantees the tail calls of the "
                                     "lifted code): https://github.com/llvm/llvm-project/releases (LLVM-…-win64.exe)")
        elif sys.platform == "darwin":
            if not find_tool("xcrun"):
                self.problems.append("Xcode or the Command Line Tools are missing: xcode-select --install")
            for tool in ("make", "perl"):  # libvpx (videos) builds with its own configure
                if not find_tool(tool):
                    self.problems.append(f"{tool} is missing (for libvpx, the videos): xcode-select --install")
        else:
            suffix = os.environ.get("LGP_LLVM_SUFFIX")  # same clang as the ios-cross toolchain, e.g. -21
            self.cc = find_tool("clang" + suffix) if suffix else find_tool("clang") or find_llvm_tool("clang")
            self.cxx = find_tool("clang++" + suffix) if suffix else find_tool("clang++") or find_llvm_tool("clang++")
            if not self.cxx:
                self.problems.append("clang is missing (package clang or llvm)")
            self.notes.append("On Linux, SDL needs the X11 headers (e.g. libx11-dev libxcursor-dev libxi-dev "
                              "libxfixes-dev libxrandr-dev libxss-dev libxtst-dev).")
            if self.target == "desktop":  # libvpx (videos) builds with its own configure, as on macOS
                for tool in ("make", "perl"):
                    if not find_tool(tool):
                        self.problems.append(f"{tool} is missing (for libvpx, the videos): package {tool}")
                if platform.machine().lower() in ("x86_64", "amd64") and not (find_tool("nasm") or find_tool("yasm")):
                    self.notes.append("Without nasm (or yasm) libvpx is built in plain C and the videos decode "
                                      "more slowly (package nasm).")
        if self.need_node and DUMP and DUMP.node_problem():
            self.problems.append(DUMP.node_problem())
        if self.target == "ios" and sys.platform != "darwin":
            self.check_ios_cross()
        elif self.target == "ios":
            if not find_tool("xcodebuild"):
                self.problems.append("Xcode is missing (App Store); afterwards open it once and accept the licence.")
        if self.target == "android":
            self.check_android()
        self.plan_installs()
        self.env = with_tool_dirs(self.env)
        return not self.problems

    def check_ios_cross(self) -> None:
        """iPhone/iPad without a Mac (draft): SDK from the user's own Xcode.xip, ld64.lld, xtool to sign."""
        if os.name == "nt":
            self.problems.append("iPhone/iPad without a Mac builds in WSL (Ubuntu): start the assistant "
                                 "(lgp-bauen.cmd, choice 3); it sets up WSL and carries on there. "
                                 "Guide: docs/knowledge/ios-ohne-mac.md")
            return
        clang = os.environ.get("LGP_LLVM_SUFFIX") and find_tool("clang" + os.environ["LGP_LLVM_SUFFIX"]) \
            or find_llvm_tool("clang")
        if not clang or clang_major(clang) < MIN_IOS_CLANG:
            have = f" (found: clang {clang_major(clang)})" if clang else ""
            self.problems.append(f"clang ≥ {MIN_IOS_CLANG} is missing{have}: the iPhoneOS SDK's libc++ needs it, e.g. "
                                 "`sudo apt install clang-21 lld-21 llvm-21`")
        if not find_llvm_tool("ld64.lld"):
            self.problems.append("ld64.lld is missing (package lld, e.g. `sudo apt install lld`)")
        for tool in ("llvm-ar", "llvm-ranlib"):
            if not find_llvm_tool(tool):
                self.problems.append(f"{tool} is missing (package llvm, e.g. `sudo apt install llvm`)")
        if not find_signer():
            self.problems.append("rcodesign is missing (writes the memory entitlements into the app, otherwise xtool "
                                 "leaves them out): python3 tools/kit/build.py fetch-rcodesign")
        if not find_tool("xtool") and not ios_sdk():  # unpacks the SDK; signing it leaves to a sideloader
            self.notes.append("xtool is needed once to unpack the SDK from Xcode.xip (`xtool sdk install`, no Apple "
                              "ID login): https://xtool.sh/documentation/xtool/installation-linux")
        if not ios_sdk():
            self.problems.append(
                "The iPhoneOS SDK is missing: download Xcode.xip yourself from https://developer.apple.com/download/all/ "
                "(Apple ID), then `xtool sdk install Xcode.xip` (or point LGP_IOS_SDK at an iPhoneOS*.sdk). Note: "
                "Apple's Xcode licence allows Xcode only on Apple hardware; never pass the SDK on.")
        # Installing goes through a sideloader on the user's side (SIDELOADERS); only --installer xtool needs USB here.
        if sys.platform.startswith("linux") and not in_wsl():
            self.notes.append("Only for --installer xtool: usbmuxd must run (xtool guide).")
        elif in_wsl():
            self.notes.append("Only for --installer xtool: attach the iPhone with `usbipd attach --wsl` on Windows "
                              "(xtool guide).")

    def check_android(self) -> None:
        """JDK, SDK (platform, build-tools) and NDK ≥ r30 for preset android-app; sets JAVA_HOME and ANDROID_*."""
        path_key = next((k for k in self.env if k.upper() == "PATH"), "PATH")
        jdk = find_jdk()
        if not jdk:
            self.problems.append(f"JDK ≥ {MIN_JDK} is missing (javac, d8, apksigner): https://adoptium.net/ (Temurin 21)")
        else:
            self.env["JAVA_HOME"] = str(jdk)
            self.env[path_key] = str(jdk / "bin") + os.pathsep + self.env.get(path_key, "")
        sdk = android_sdk()
        ndk = android_ndk(sdk)
        if not android_sdk_ready(sdk) or not ndk:
            what = [] if android_sdk_ready(sdk) else ["platform and build-tools"]
            what += [] if ndk else [f"NDK ≥ r{MIN_NDK}"]
            self.problems.append(f"Android SDK: {' and '.join(what)} missing ({sdk}): "
                                 "python tools/kit/build.py fetch-android")
            return
        self.android_sdk = sdk
        self.env.update(ANDROID_HOME=str(sdk), ANDROID_NDK_HOME=str(ndk))
        self.env.pop("ANDROID_SDK_ROOT", None)  # an older one would win in some tools
        platform_tools = sdk / "platform-tools"
        if platform_tools.is_dir():
            self.env[path_key] = str(platform_tools) + os.pathsep + self.env.get(path_key, "")

    def plan_installs(self) -> None:
        """Commands that install what check() found missing (official sources, no admin where possible)."""
        winget = ["winget", "install", "--exact", "--accept-package-agreements", "--accept-source-agreements", "--id"]
        missing = " ".join(self.problems + self.optional)
        apt = sys.platform.startswith("linux") and shutil.which("apt-get")
        if portable():  # the ready-made package brings its tools; never install anything for good from it
            winget = None
        if apt and (not self.cmake or not self.ninja or "CMake is too old" in missing):
            # pip --user is refused on current Debian/Ubuntu (externally managed Python); Ubuntu 24.04 has 3.28
            self.installs.append(("CMake and Ninja (apt, asks for the password)",
                                  ["sudo", "apt-get", "install", "-y", "cmake", "ninja-build"]))
        elif self.target != "extract" and not portable() and (not self.cmake or not self.ninja
                                                                or "CMake is too old" in missing):
            # Kitware's and the Ninja project's official wheels; land in the user's scripts folder
            self.installs.append(("CMake and Ninja (pip, no admin rights)",
                                  [sys.executable, "-m", "pip", "install", "--user", "--upgrade", "cmake", "ninja"]))
        if os.name == "nt":
            if "llvm-mingw" in missing:
                self.installs.append((f"Compiler llvm-mingw {LLVM_MINGW[0]} (once, about 180 MB, no admin rights)",
                                      [sys.executable, str(Path(__file__).resolve()), "fetch-compiler"]))
            if "fetch-ffmpeg" in missing:
                self.installs.append(("FFmpeg for the videos (LGPL build, once, about 60 MB, no admin rights)",
                                      [sys.executable, str(Path(__file__).resolve()), "fetch-ffmpeg"]))
            if "LLVM/clang is missing" in missing and winget and shutil.which("winget"):
                self.installs.append(("LLVM/clang (winget)", winget + ["LLVM.LLVM"]))
            if "Visual Studio Build Tools" in missing and winget and shutil.which("winget"):
                self.installs.append((
                    "Visual Studio Build Tools with C++ (winget, several GB, Microsoft's licence)",
                    winget + ["Microsoft.VisualStudio.2022.BuildTools", "--override",
                              "--wait --passive --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"]))
        elif sys.platform == "darwin":
            if "xcode-select --install" in missing:
                self.installs.append(("Apple's Command Line Tools (confirm the system dialog, then wait)",
                                      ["xcode-select", "--install"]))
        if f"JDK ≥ {MIN_JDK} is missing" in missing:  # a folder in the tool cache, nothing installed
            self.installs.append((f"Microsoft's JDK {JDK[0]} as a folder (about 200 MB, no admin rights)",
                                  [sys.executable, str(Path(__file__).resolve()), "fetch-jdk"]))
        if "Node.js" in missing:
            if DUMP and DUMP.node_archive():  # a folder in the tool cache, nothing installed (as the JDK)
                self.installs.append((f"Node.js {DUMP.NODE_VERSION} as a folder (once, about 50 MB, no admin rights)",
                                      [sys.executable, str(Path(DUMP.__file__).resolve()), "fetch-node"]))
            elif os.name == "nt" and winget and shutil.which("winget"):
                self.installs.append(("Node.js LTS (winget)", winget + ["OpenJS.NodeJS.LTS"]))
            elif sys.platform == "darwin" and shutil.which("brew"):
                self.installs.append(("Node.js (Homebrew)", ["brew", "install", "node"]))
        jdk_coming = any(what.startswith("JDK") for what, _ in self.installs)
        if "fetch-android" in missing and (jdk_coming or f"JDK ≥ {MIN_JDK} is missing" not in missing):  # sdkmanager needs it
            self.installs.append(("Android SDK, NDK r30 and adb (Google's sdkmanager, about 1.2 GB, accept the licences "
                                  "with y)", [sys.executable, str(Path(__file__).resolve()), "fetch-android"]))
        llvm = apt_llvm_version() if apt and f"clang ≥ {MIN_IOS_CLANG} is missing" in missing else None
        if "fetch-rcodesign" in missing:
            self.installs.append((f"rcodesign {RCODESIGN[0]} (signs ad hoc, about 11 MB, no admin rights)",
                                  [sys.executable, str(Path(__file__).resolve()), "fetch-rcodesign"]))
        if llvm:
            self.installs.append((f"clang {llvm}, lld and LLVM tools for iOS (apt, asks for the password)",
                                  ["sudo", "apt-get", "install", "-y", f"clang-{llvm}", f"lld-{llvm}", f"llvm-{llvm}",
                                   "make", "perl", "python3-pil", "usbmuxd"]))
        elif apt and ("ld64.lld" in missing or "llvm-ar" in missing):
            self.installs.append(("LLVM linker and tools for iOS (apt, asks for the password)",
                                  ["sudo", "apt-get", "install", "-y", "lld", "llvm", "make", "perl", "python3-pil",
                                   "usbmuxd"]))
        if apt and "for libvpx" in missing:
            self.installs.append(("make and perl for libvpx, the videos (apt, asks for the password)",
                                  ["sudo", "apt-get", "install", "-y", "make", "perl", "nasm"]))
        if sys.platform != "darwin" and os.name != "nt" and "clang is missing" in missing and shutil.which("apt-get"):
            self.installs.append(("clang and SDL headers (apt, asks for the password)",
                                  ["sudo", "apt-get", "install", "-y", "clang", "lld", "libx11-dev",
                                   "libxcursor-dev", "libxi-dev", "libxfixes-dev", "libxrandr-dev", "libxss-dev",
                                   "libxtst-dev"]))

    def compiler_args(self) -> list[str]:
        if not self.cxx or sys.platform == "darwin":
            return []
        args = [f"-DCMAKE_C_COMPILER={cmake_path(self.cc)}", f"-DCMAKE_CXX_COMPILER={cmake_path(self.cxx)}"]
        if os.name == "nt" and self.compiler == "mingw":
            args.append(f"-DCMAKE_RC_COMPILER={cmake_path(Path(self.cxx).with_name('x86_64-w64-mingw32-windres.exe'))}")
        return args


# --- steps ------------------------------------------------------------------------------------------------------


class Builder:
    def __init__(self, args, tc: Toolchain):
        self.args = args
        self.tc = tc
        self.work = Path(args.work).resolve()
        # --dump: unpacked into <work>/kit, which then takes the place of the ZIP.
        self.kit = Path(args.kit).resolve() if args.kit else self.work / "kit"
        self.game = self.work / "game"
        self.lifted = self.work / "lifted"
        self.host = self.work / "build-desktop"
        self.ios = self.work / "build-ios"
        self.ios_cross = self.work / "build-ios-cross"
        # x86_64 only for the Android emulator's x86_64 image: own tree and output folder
        abi = getattr(args, "abi", "arm64-v8a")
        self.abi_suffix = "" if abi == "arm64-v8a" else f"-{abi}"
        self.android = self.work / f"build-android{self.abi_suffix}"
        self.names: set[str] = set()
        self.stage_key = ""
        jobs = getattr(args, "jobs", None)
        if "CMAKE_BUILD_PARALLEL_LEVEL" not in tc.env:  # every cmake --build, also of sub-builds
            free = free_memory_mib()
            n = jobs or auto_jobs(os.cpu_count() or 1, free)
            tc.env["CMAKE_BUILD_PARALLEL_LEVEL"] = str(n)
            self.jobs_note = (f"Parallel: {n} jobs ({os.cpu_count() or '?'} cores, "
                              f"{'?' if free is None else f'{free / 1024:.1f} GB'} free"
                              f"{', with --jobs' if jobs else ''})")
            if not jobs and n < 2 and (os.cpu_count() or 1) >= 2:
                # PC 2026-11-04: a virtual machine left running held 8 GB; 1 job made a 7-minute build take 26
                self.jobs_note += ("\nOnly one part at a time, because little memory is free: this takes much "
                                   "longer. Closing other programs (browsers, virtual machines, games) and starting "
                                   "again makes it several times faster.")
        else:
            self.jobs_note = f"Parallel: {tc.env['CMAKE_BUILD_PARALLEL_LEVEL']} jobs (CMAKE_BUILD_PARALLEL_LEVEL)"
        self.dump_key = ""  # --dump: identity of dump, keys, firmware and kit files (extract())
        self.extra_files: Path | None = None  # --kit-files/--kit-url over a kit (kit_files())

    def run(self, cmd: list[str], cwd: Path | None = None) -> None:
        say("$ " + " ".join(f'"{c}"' if " " in c else c for c in cmd))
        if self.args.dry_run:
            return
        r = subprocess.run(cmd, cwd=cwd or REPO, env=self.tc.env)
        if r.returncode != 0:
            raise KitError(f"Step failed (exit code {r.returncode}): {cmd[0]}")

    def stamp_ok(self, name: str, key: str) -> bool:
        p = self.work / f"{name}.stamp"
        return not self.args.dry_run and p.is_file() and p.read_text() == key

    def stamp_write(self, name: str, key: str) -> None:
        if not self.args.dry_run:
            (self.work / f"{name}.stamp").write_text(key)

    def launcher_args(self) -> list[str]:
        """--measure LOG: every compile and link step through tools/kit/measure_cc.py (peak RAM, CPU time)."""
        log = getattr(self.args, "measure", None)
        # Without --measure the launchers are cleared, or the cache of a measured build would keep writing its log.
        launcher = ";".join([cmake_path(Path(sys.executable)), cmake_path(Path(__file__).with_name("measure_cc.py")),
                             cmake_path(Path(log).resolve())]) if log else ""
        return [f"-DCMAKE_{kind}_LAUNCHER={launcher}" for kind in
                ("C_COMPILER", "CXX_COMPILER", "OBJC_COMPILER", "OBJCXX_COMPILER", "C_LINKER", "CXX_LINKER")]

    @staticmethod
    def python_args() -> list[str]:
        # The Python running this script (in the ready-made package bauset\python): FindPython3 would otherwise take
        # one from the Windows registry, e.g. a Store Python, even with an empty PATH (PC check of #471).
        return [f"-DPython3_EXECUTABLE={cmake_path(sys.executable)}"]

    def desktop_configure(self, extra: list[str]) -> None:
        drop_stale_cache(self.host, self.args.dry_run, self.tc.cxx)
        self.run([self.tc.cmake or "cmake", "--preset", "release", *self.launcher_args(), "-B", str(self.host),
                  "-DBUILD_TESTING=OFF", "-DLGP_ENABLE_UNICORN=OFF", *self.python_args(), *self.tc.compiler_args(),
                  *extra])

    def build_host_tools(self) -> None:
        say("== Building the tools (lgp_lift, lgp_romfs_pack)")
        # A cached LGP_GAME_LIFTED without .cpp files (lifted/ deleted by hand) would stop the configure step.
        self.desktop_configure([] if any(self.lifted.glob("*.cpp")) else ["-DLGP_GAME_LIFTED="])
        self.run([self.tc.cmake or "cmake", "--build", str(self.host), "--target", "lgp_lift", "lgp_romfs_pack"])

    def tool(self, *parts: str) -> str:
        return str(self.host.joinpath(*parts[:-1], exe_name(parts[-1])))

    def game_key(self) -> str:
        # --dump: the inputs of the unpacking (extract() sets dump_key), since <work>/kit is gone after staging
        source = f"dump|{self.dump_key}" if self.dump_key else kit_identity(self.kit)
        if not self.dump_key and self.extra_files:  # --kit-files/--kit-url over a kit or a plain game folder
            source += f"|files={content_identity(self.extra_files) if self.extra_files.is_dir() else '-'}"
        return f"{STAMP_VERSION}|{source}|compress={not self.args.no_compress}"

    def game_current(self) -> bool:
        return self.stamp_ok("game", self.game_key()) and (self.game / "romfs.bin").is_file()

    def stage(self) -> None:
        key = self.game_key()
        self.stage_key = key
        if self.game_current():
            say("== Game folder unchanged: " + str(self.game))
            return
        say("== Creating the game folder: " + str(self.game))
        # A plain game folder may hold the RomFS as files (romfs/); lgp_romfs_pack packs them into the image.
        romfs_dir = self.kit / "romfs" if self.kit.is_dir() and not (self.kit / "romfs.bin").is_file() else None
        raw = romfs_dir or self.work / "romfs-raw.bin"
        own_kit = bool(self.dump_key)
        if not self.args.dry_run:
            stage_game(self.kit, self.game, raw, move_romfs=own_kit)
            if not romfs_dir and romfs_compressed(raw):
                # a game folder build.py already made: unpack its romfs.bin for the mod recipes and the packing
                say("== Unpacking the compressed romfs.bin")
                plain = raw.with_name(raw.name + ".plain")
                self.run([self.tool("runtime", "hle", "lgp_romfs_pack"), str(raw), str(plain)])
                os.replace(plain, raw)
            if own_kit:  # what is left of <work>/kit is also in the game folder now
                shutil.rmtree(self.kit, ignore_errors=True)
            if self.extra_files:  # with a dump only the package's kit-files/ (the dump was unpacked without them)
                copy_kit_files(self.extra_files, self.game)
            derive_mod_files(self.game, raw)
        if self.args.no_compress and not romfs_dir:
            if not self.args.dry_run:
                os.replace(raw, self.game / "romfs.bin")
        else:
            say("== Packing romfs.bin" if self.args.no_compress else "== Compressing romfs.bin (zstd)")
            self.run([self.tool("runtime", "hle", "lgp_romfs_pack"), *([] if self.args.no_compress else ["--compress"]),
                      str(raw), str(self.game / "romfs.bin")])
            if not self.args.dry_run and not romfs_dir:
                raw.unlink()
        self.stamp_write("game", key)

    def lift(self) -> None:
        h = hashlib.sha256(f"{STAMP_VERSION}|split={LIFT_SPLIT}|".encode())
        inputs = [self.game / "exefs" / m for m in LIFT_MODULES] + [self.game / "game.cfg"]
        if self.args.dry_run:  # the game folder is not staged; take the mod files from the kit's listing
            mods = sorted(self.game / n for n in self.names if re.fullmatch(r"mods/[^/]+\.cfg", n))
        else:
            mods = sorted((self.game / "mods").glob("*.cfg")) if (self.game / "mods").is_dir() else []
        lift_exe = Path(self.tool("recomp", "codegen", "lgp_lift"))

        def lift_key(mod_files: list[Path]) -> str:
            k = h.copy()
            for p in inputs + mod_files + ([lift_exe] if lift_exe.is_file() else []):
                k.update(p.name.encode() + b"|" + (p.read_bytes() if p.is_file() else b""))
            return k.hexdigest()

        key = lift_key(mods)
        if self.stamp_ok("lift", key) and any(self.lifted.glob("*.cpp")):
            say("== Lift unchanged: " + str(self.lifted))
            return
        say(f"== Lifting (main + sdk, {len(mods)} mod files)")
        # Lift into lifted.new and swap only on success: a failed or interrupted lift keeps the old tree.
        fresh = self.lifted.with_name(self.lifted.name + ".new")
        if not self.args.dry_run:
            shutil.rmtree(fresh, ignore_errors=True)
            fresh.mkdir(parents=True)
        cmd = [str(lift_exe), *(str(p) for p in inputs[:-1]), "-o", str(fresh / "lifted.cpp"),
               "--split", str(LIFT_SPLIT), "--config", str(inputs[-1]), "--summary"]
        if mods:
            bad = self.work / "lift-bad-mods.txt"
            cmd += ["--mods", str(self.game / "mods"), "--bad-mods-out", str(bad)]
            # A mod file that does not fit this game build (e.g. newer than this kit) stops only that mod:
            # it moves to <game>/mods-left-out/ and the lift runs again without it.
            for _ in mods:
                if self.args.dry_run:
                    self.run(cmd)
                    break
                bad.unlink(missing_ok=True)
                say("$ " + " ".join(f'"{c}"' if " " in c else c for c in cmd))
                r = subprocess.run(cmd, cwd=REPO, env=self.tc.env)
                if r.returncode == 0:
                    break
                names = [n for n in bad.read_text(encoding="utf-8").split() if n] if bad.is_file() else []
                if r.returncode != 1 or not names:
                    raise KitError(f"Step failed (exit code {r.returncode}): {cmd[0]}")
                self.leave_out_mods(names)
                if not any((self.game / "mods").glob("*.cfg")):
                    cmd = cmd[:cmd.index("--mods")]
                    self.run(cmd)
                    break
            else:
                self.run(cmd)
        else:
            self.run(cmd)
        if not self.args.dry_run:
            shutil.rmtree(self.lifted, ignore_errors=True)
            os.replace(fresh, self.lifted)
        if not self.args.dry_run:  # the mods actually lifted: a left-out mod file is no longer in mods/
            key = lift_key(sorted((self.game / "mods").glob("*.cfg")) if (self.game / "mods").is_dir() else [])
        self.stamp_write("lift", key)

    def leave_out_mods(self, names: list[str]) -> None:
        """Moves mod files that do not fit this game build (and their data folders) to <game>/mods-left-out/."""
        out = self.game / "mods-left-out"
        out.mkdir(exist_ok=True)
        for name in names:
            cfg = self.game / "mods" / Path(name).name
            if cfg.is_file():
                os.replace(cfg, out / cfg.name)
            data = cfg.with_suffix("")
            if data.is_dir():
                shutil.rmtree(out / data.name, ignore_errors=True)
                os.replace(data, out / data.name)
            say(f"Note: the mod {cfg.stem} does not fit this game build (made for another version of the kit?), "
                f"so it is left out; the rest is built. File: {out / cfg.name}")

    def default_mods(self) -> str:
        """The kit's mods (mods/<id>.cfg, data mods mods/<id>/romfs/) that are on on the first start."""
        names = self.names if self.args.dry_run else {
            p.relative_to(self.game).as_posix() for p in (self.game / "mods").rglob("*") if p.is_file()}
        ids = {m.group(1) for n in names if (m := re.fullmatch(r"mods/([^/]+)(?:\.cfg|/romfs/.*)", n))}
        return " ".join(sorted(ids - DEFAULT_OFF_MODS))

    def build_desktop(self) -> Path:
        say("== Building the game (this computer)")
        extra = [f"-DLGP_GAME_LIFTED={cmake_path(self.lifted)}", f"-DLGP_APP_GAME_DIR={cmake_path(self.game)}",
                 "-DLGP_APP_PIPELINE_SEED_AUTO=OFF", f"-DLGP_APP_DEFAULT_MODS={self.default_mods()}"]
        if sys.platform == "darwin":
            extra += ["-DCMAKE_MACOSX_BUNDLE=ON", "-DLGP_ENABLE_VPX=ON"]
        elif os.name != "nt":  # Linux: no FFmpeg needed (FFmpeg is still taken when pkg-config finds it)
            extra.append("-DLGP_ENABLE_VPX=ON")
        # Always named, so a cached folder from an earlier run does not outlive "none" or its removal
        ffmpeg = self.args.ffmpeg_dir or (str(ffmpeg_dir()) if os.name == "nt" and ffmpeg_ready() else "none")
        if os.name == "nt" and ffmpeg == "none":
            say("Note: without FFmpeg the videos stay black (python tools/kit/build.py fetch-ffmpeg, then build again).")
        extra.append("-DLGP_FFMPEG_DIR=" + ("" if ffmpeg == "none" else cmake_path(Path(ffmpeg).resolve())))
        self.desktop_configure(extra)
        targets = ["lgp_game", "lgp_game_package_game"]
        self.run([self.tc.cmake or "cmake", "--build", str(self.host), "--target", *targets])
        boot = self.host / "runtime" / "boot"
        return boot / "lgp_game.app" if sys.platform == "darwin" else boot / exe_name("lgp_game")

    def game_stamp(self) -> str:
        """Build stamp of the game folder for an app without the game inside (--game-separate): the staged dump plus
        game.cfg and mods/, the files the lifted code's hooks and the data mods depend on. Not the lift itself, so a
        rebuild after `git pull` keeps the copy on the device; a new kit or other mod files need a new copy."""
        h = hashlib.sha256(f"{STAMP_VERSION}|{self.stage_key}|".encode())
        files = [self.game / "game.cfg"]
        if (self.game / "mods").is_dir():
            files += sorted(p for p in (self.game / "mods").rglob("*") if p.is_file())
        for p in files:
            h.update(p.relative_to(self.game).as_posix().encode() + b"|" + (p.read_bytes() if p.is_file() else b"") + b"|")
        return h.hexdigest()[:16]

    def ios_game_args(self) -> list[str]:
        """The game inside the app, or (--game-separate) a slim app whose game folder carries the build stamp."""
        if not self.args.game_separate:
            return [f"-DLGP_APP_GAME_DIR={cmake_path(self.game)}", "-DLGP_APP_GAME_STAMP="]
        stamp = self.game_stamp()
        say(f"== App without the game (--game-separate), build stamp {stamp}")
        if not self.args.dry_run:
            (self.game / "lgp-build-stamp.txt").write_text(stamp + "\n", encoding="utf-8")
        # An empty LGP_APP_GAME_DIR also removes the game an earlier build of this tree copied into the app.
        return ["-DLGP_APP_GAME_DIR=", f"-DLGP_APP_GAME_STAMP={stamp}"]

    def memory_entitlements(self) -> str:
        """both, limit or none: --memory-entitlements, else both with a paid team (--ios-team), else what a free
        team can sign (FREE_TEAM_MEMORY_ENTITLEMENTS, see ios-ohne-mac.md)."""
        if self.args.memory_entitlements != "auto":
            return self.args.memory_entitlements
        if self.installer() == "sideload":  # most sideloading is with a free Apple ID
            return FREE_TEAM_MEMORY_ENTITLEMENTS
        return "both" if self.args.ios_team else FREE_TEAM_MEMORY_ENTITLEMENTS

    def installer(self) -> str:
        """sideload (an .ipa for Impactor, SideStore, AltStore), xtool (ios without a Mac) or xcode (Mac)."""
        return installer_for(self.args)

    @staticmethod
    def memory_fallback_args(memory: str) -> list[str]:
        """--memory-entitlements none, with or without a Mac: the guest block goes onto a file as on the Apple TV
        (host_block.cpp), so less of it counts against the jetsam limit (suspicion: not measured on an iPhone)."""
        if memory != "none":  # set back explicitly: the cache of an earlier none build would keep them
            return ["-DLGP_HOST_BLOCK_FILE_DEFAULT=OFF", "-DLGP_HOST_BLOCK_FILE_RANGE_DEFAULT="]
        say("Note: no memory entitlements; guest memory from 768 MiB up on a file, as on the Apple TV.")
        return ["-DLGP_HOST_BLOCK_FILE_DEFAULT=ON", "-DLGP_HOST_BLOCK_FILE_RANGE_DEFAULT=768:"]

    def build_ios_cross(self) -> Path:
        """Without a Mac: unsigned .app with preset ios-cross, then an .ipa with the entitlements embedded."""
        say("== Building the game (iPhone/iPad, without a Mac; draft)")
        sdk = ios_sdk()
        extra = [f"-DLGP_GAME_LIFTED={cmake_path(self.lifted)}", *self.ios_game_args(),
                 "-DLGP_APP_PIPELINE_SEED_AUTO=OFF", f"-DLGP_APP_DEFAULT_MODS={self.default_mods()}"]
        if sdk:
            extra.append(f"-DLGP_IOS_SDK={cmake_path(sdk)}")
        if self.args.bundle_prefix:
            extra.append(f"-DLGP_IOS_BUNDLE_PREFIX={self.args.bundle_prefix}")
        memory = self.memory_entitlements()
        extra.append(f"-DLGP_IOS_MEMORY_ENTITLEMENTS={memory}")
        if memory == "limit":
            # A free team: higher jetsam limit, but no extended address space. The 3456 MiB guest block reserves
            # without it (iPad probe, tvOS runs that way); whether the game fits is not measured on an iPhone yet.
            say("Note: increased-memory-limit only (free Apple ID); not yet measured with the game on an iPhone.")
        extra += self.memory_fallback_args(memory)
        drop_stale_cache(self.ios_cross, self.args.dry_run)
        self.run([self.tc.cmake or "cmake", "--preset", "ios-cross", *self.launcher_args(), "-B", str(self.ios_cross), *self.python_args(),
                  *extra])
        # The copy target exists only with something to copy or to take out again (runtime/boot/CMakeLists.txt):
        # not for a fresh --game-separate tree.
        boot = self.ios_cross / "runtime" / "boot"
        package = not self.args.game_separate or any(boot.glob("lgp_game_package_*.stamp"))
        self.run([self.tc.cmake or "cmake", "--build", str(self.ios_cross), "--target", "lgp_game",
                  *(["lgp_game_package_game"] if package else [])])
        out = self.work / "ios"
        if not self.args.dry_run:
            out.mkdir(parents=True, exist_ok=True)
        ipa = out / "lgp_game.ipa"
        signer = find_signer()
        self.run([sys.executable, str(REPO / "tools" / "ios" / "make_ipa.py"),
                  "--app", str(self.ios_cross / "runtime" / "boot" / "lgp_game.app"),
                  "--entitlements", str(self.ios_cross / "lgp_game.entitlements"), "--out", str(ipa),
                  *(["--signer-path", signer] if signer else [])])
        return ipa

    def build_ios(self) -> Path:
        if sys.platform != "darwin":
            return self.build_ios_cross()
        sideload = self.installer() == "sideload"
        if not self.args.ios_team and not sideload:
            raise KitError("iPhone/iPad needs --ios-team <team ID> (Xcode > Settings > Accounts).")
        say("== Building the game (iPhone/iPad)")
        # Xcode's /usr/bin/python3 exports SDKROOT=…/MacOSX.sdk; CMake would take it as the sysroot of the iOS tree.
        self.tc.env.pop("SDKROOT", None)
        cache = self.ios / "CMakeCache.txt"
        if not self.args.dry_run and cache.is_file() and re.search(
                r"^CMAKE_OSX_SYSROOT:\w+=.*MacOSX", cache.read_text(encoding="utf-8", errors="replace"), re.M):
            say("== The iOS build folder pointed at the macOS SDK; creating it anew")
            cache.unlink()
            shutil.rmtree(self.ios / "CMakeFiles", ignore_errors=True)
        extra = [f"-DLGP_IOS_TEAM={self.args.ios_team or ''}", f"-DLGP_GAME_LIFTED={cmake_path(self.lifted)}",
                 *self.ios_game_args(), "-DLGP_APP_PIPELINE_SEED_AUTO=OFF",
                 f"-DLGP_APP_DEFAULT_MODS={self.default_mods()}"]
        if self.args.bundle_prefix:
            extra.append(f"-DLGP_IOS_BUNDLE_PREFIX={self.args.bundle_prefix}")
        # Always explicit, or a sideload build's OFF stays in this folder's cache for a later Xcode build;
        # a sideloader cannot provision the iCloud container.
        cloud = not (self.args.no_cloud_saves or sideload)
        extra.append(f"-DLGP_IOS_CLOUD_SAVES={'ON' if cloud else 'OFF'}")
        # Always explicit (auto = both with the team), or the cache of an earlier build in this folder would keep
        # its mode; limit e.g. for a free team in Xcode.
        memory = self.memory_entitlements()
        extra += [f"-DLGP_IOS_MEMORY_ENTITLEMENTS={memory}", *self.memory_fallback_args(memory)]
        drop_stale_cache(self.ios, self.args.dry_run)
        self.run([self.tc.cmake or "cmake", "--preset", "ios", "-B", str(self.ios), *self.python_args(), *extra])
        # Sideload: Xcode builds without signing (no team needed); make_ipa signs the executable ad hoc with the
        # entitlements, the sideloader signs the whole bundle with the user's Apple ID.
        # No -jobs here: CMake passes CMAKE_BUILD_PARALLEL_LEVEL (set above by free RAM) as -jobs, and xcodebuild
        # refuses a second one (Mac, 2026-10-28: exit 64).
        xcode_args = ["CODE_SIGNING_ALLOWED=NO"] if sideload else ["-allowProvisioningUpdates"]
        self.run([self.tc.cmake or "cmake", "--build", str(self.ios), "--config", "Release", "--target", "lgp_game",
                  "--", *xcode_args])
        app = self.ios / "runtime" / "boot" / "Release-iphoneos" / "lgp_game.app"
        if not sideload:
            return app
        out = self.work / "ios"
        if not self.args.dry_run:
            out.mkdir(parents=True, exist_ok=True)
        ipa = out / "lgp_game.ipa"
        self.run([sys.executable, str(REPO / "tools" / "ios" / "make_ipa.py"), "--app", str(app),
                  "--entitlements", str(self.ios / "lgp_game.entitlements"), "--signer", "codesign",
                  "--out", str(ipa)])
        return ipa

    def build_android(self) -> Path:
        """lgp_game.apk (preset android-app) and the game as split APKs beside it, all with one signing key."""
        say("== Building the game (Android)")
        sdk = self.tc.android_sdk or android_sdk()
        keystore = android_keystore()
        if not self.args.dry_run:
            keystore.parent.mkdir(parents=True, exist_ok=True)
        if not keystore.is_file():
            say(f"Note: new signing key {keystore}. Keep this file: Android updates the app only with the same "
                "key; otherwise only uninstalling helps, and that deletes the saves.")
        extra = [f"-DLGP_GAME_LIFTED={cmake_path(self.lifted)}", f"-DLGP_APP_DEFAULT_MODS={self.default_mods()}",
                 "-DLGP_APP_PIPELINE_SEED_AUTO=OFF", f"-DLGP_ANDROID_SDK={cmake_path(sdk)}",
                 f"-DLGP_ANDROID_KEYSTORE={cmake_path(keystore)}"]
        if self.abi_suffix:
            extra.append(f"-DANDROID_ABI={self.args.abi}")
        seed = self.game / "pipeline-seed.lgppipes"
        if seed.is_file() or (self.args.dry_run and "pipeline-seed.lgppipes" in self.names):
            extra.append(f"-DLGP_APP_PIPELINE_SEED={cmake_path(seed)}")  # into the APK's assets (LgpActivity)
        drop_stale_cache(self.android, self.args.dry_run)
        self.run([self.tc.cmake or "cmake", "--preset", "android-app", *self.launcher_args(), "-B", str(self.android), *self.python_args(),
                  *extra])
        self.run([self.tc.cmake or "cmake", "--build", str(self.android), "--target", "lgp_game_apk"])
        out = self.work / f"android{self.abi_suffix}"
        if not self.args.dry_run:
            out.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(self.android / "runtime" / "boot" / "lgp_game.apk", out / "lgp_game.apk")
        # exefs/, systemdata/ and mods/ in one split, romfs.bin in parts below 2 GiB (docs/knowledge/android-ndk-build.md)
        self.run([sys.executable, str(REPO / "tools" / "android" / "build_game_splits.py"), "--game", str(self.game),
                  "--out", str(out), "--sdk", str(sdk), "--keystore", str(keystore), "--package", ANDROID_PACKAGE_ID])
        return out

    def kit_files(self) -> Path | None:
        """--kit-files/--kit-url: game.cfg and the mods from a folder or the kit server. With --dump they go into the
        unpacking; over a kit or a plain game folder they are copied onto its own (stage)."""
        if self.args.kit_files:
            folder = Path(self.args.kit_files).resolve()
            if not folder.is_dir():
                raise KitError(f"--kit-files {folder} is not a folder.")
            if self.args.dry_run:  # nothing gets staged: count the kit files the lift would take
                self.names |= set(kit_file_names(folder))
            return folder
        if self.args.kit_url:
            folder = self.work / "kit-files"
            if self.args.dry_run:
                say(f"(Dry run: kit files from {self.args.kit_url} into {folder})")
            else:
                fetch_kit_files(self.args.kit_url, folder)
            return folder
        return None

    def extract(self) -> None:
        DUMP.extract(self)

    def check_paths(self) -> None:
        """The work folder is wiped in parts (game/, lifted/): it must not hold the kit, nor lie inside it."""
        def overlaps(a: Path, b: Path) -> bool:
            return a == b or a in b.parents or b in a.parents
        if self.args.dump:
            DUMP.check_paths(self, overlaps)
            return  # <work>/kit is ours
        if self.args.kit_files and overlaps(Path(self.args.kit_files).resolve(), self.work):
            raise KitError(f"--kit-files {self.args.kit_files} lies in the work folder {self.work}; please put it "
                           "somewhere else or choose another folder with --work.")
        if self.kit.is_dir() and overlaps(self.kit, self.work):
            raise KitError(f"The work folder {self.work} and the dump folder {self.kit} overlap; "
                           "please choose another folder with --work.")
        if self.kit.is_file() and any(d in self.kit.parents for d in (self.game, self.lifted, self.lifted.with_name("lifted.new"))):
            raise KitError(f"The dump ZIP lies in the work folder ({self.kit}); please put it somewhere else.")

    def build(self) -> Path:
        if self.args.game_separate and self.args.target != "ios":
            raise KitError("--game-separate is only for --target ios (desktop and Android keep the game beside "
                           "the program or in split APKs).")
        self.check_paths()
        if not self.args.dry_run:
            self.work.mkdir(parents=True, exist_ok=True)
            fixed = clamp_future_mtimes(REPO, skip=(self.work,))
            if fixed:
                say(f"{fixed} files in the repo folder had a modification time in the future (a ZIP packed in another "
                    "time zone); set to now, otherwise Ninja would rebuild without end")
        start = wall_clock()
        say(self.jobs_note)
        if self.args.dump:
            timed("Unpacking the dump", self.extract)
        else:
            self.extra_files = self.kit_files()
        if self.args.dry_run and not self.kit.exists():
            say("(Dry run: the dump's content is known only after unpacking"
                + (f"; kit files: {len(self.names)})" if self.names else ")"))
        else:
            info, self.names = kit_listing(self.kit)
            if not self.extra_files and "game.cfg" not in self.names and bundled_kit_files():
                self.extra_files = bundled_kit_files()
                say(f"game.cfg and the mods come from the package ({self.extra_files})")
            # Only now that the kit files are known (--kit-files/--kit-url or the package's): this build's folder.
            build = kit_build(info)
            if build and self.extra_files and self.extra_files.is_dir():
                self.extra_files = kit_files_for(self.extra_files, build)
            if self.extra_files and self.extra_files.is_dir():
                self.names |= set(kit_file_names(self.extra_files))
            for w in check_kit(info, self.names):
                if w not in NOTES_SHOWN:  # the assistant showed them already when the dump was chosen
                    say("Note: " + w)
            say(f"Dump: {info.get('title', '?')} {info.get('display_version') or ''}".rstrip())
        timed("Tools", self.build_host_tools)
        timed("Game folder", self.stage)
        timed("Lift", self.lift)
        step = {"ios": self.build_ios, "android": self.build_android}.get(self.args.target, self.build_desktop)
        product = timed("Game build", step)
        say("")
        say(f"Done after {format_seconds(wall_clock() - start)}: {product}")
        if self.args.target == "ios" and self.installer() == "sideload":
            say_sideload(product)
        elif self.args.target == "ios" and sys.platform != "darwin":
            say("To install: connect and unlock the iPhone/iPad by USB, then `python3 tools/kit/build.py install "
                "--target ios --installer xtool` (calls `xtool install " + str(product) + "`)")
            say("  (signs in with your Apple ID; with a free Apple ID the app runs for 7 days, then "
                "install it again, the saves stay).")
            if self.memory_entitlements() != "none":
                say("  For increased-memory-limit xtool needs tools/ios/xtool-increased-memory-limit.patch "
                    "(untested; if installing fails, build again with --memory-entitlements none).")
        elif self.args.target == "ios":
            say("To install: connect the iPhone/iPad by USB, then `python3 tools/kit/build.py install --target ios` "
                "(or Xcode > Window > Devices and Simulators, add the app with +).")
        if self.args.target == "ios":
            if self.args.game_separate:
                say("The app does not contain the game. After installing, copy this folder once into the app:")
                say(f"  {self.game}")
                say("  Mac: Finder, select the device, Files, drag the folder \"game\" onto LGP.")
                say("  Windows: Apple Devices app, select the device, Files, drag the folder \"game\" onto LGP.")
                say("  Copy it again only when the app says it belongs to another build (new kit or mod files).")
                say("  Deleting the app deletes the game folder and the saves on the device too.")
        if self.args.target == "android":
            say("To install (device by USB, developer options and USB debugging on): "
                "`python3 tools/kit/build.py install --target android`")
            say("  (calls adb install-multiple -r lgp_game.apk game-*.apk from the folder above)")
        say("The package contains game data: for your own use only, do not pass it on.")
        outside = outside_folder(self.work)
        if outside:
            say("Outside the repo folder:")
            for line in outside:
                say("  " + line)
        if getattr(self.args, "measure", None):
            say(f"Measurement: python3 tools/kit/measure_cc.py report {self.args.measure}")
        if self.args.target == "desktop":
            other = settings_game_override()
            if other:
                say(f"Note: {other[0]} contains \"game = {other[1]}\". The app then starts that game instead of the "
                    "bundled one; delete the line to take the new one.")
            if os.name == "nt":
                say("On the first start the Windows firewall asks (playing nearby over the network): "
                    "allow \"Private networks\".")
        return product


def wall_clock() -> float:
    # Not time.monotonic(): on macOS it stops while the Mac sleeps (display off), and a build of an hour showed
    # 7 minutes.
    return time.time()


def format_seconds(seconds: float) -> str:
    return f"{seconds:.0f} s" if seconds < 120 else f"{seconds / 60:.1f} min"


def timed(name: str, step):
    start = wall_clock()
    result = step()
    say(f"   ({name}: {format_seconds(wall_clock() - start)})")
    return result


def drop_stale_cache(tree: Path, dry_run: bool, cxx: str | None = None) -> None:
    """Removes a CMake cache whose paths have backslashes (written by build.py before 80a9768): CMake writes them
    into VerifyGlobs.cmake, where they are escapes, and a later -D does not reach that check in time. Also one of
    another compiler (Windows: msvc → mingw), which CMake refuses to switch in place, and one made from another
    source folder (WSL: the repo under /mnt/… before the mirror in ~/lgp-recomp-kit), which CMake refuses outright."""
    cache = tree / "CMakeCache.txt"
    if dry_run or not cache.is_file():
        return
    text = cache.read_text(encoding="utf-8", errors="replace")
    stale = re.compile(r"^(LGP_GAME_LIFTED|LGP_APP_GAME_DIR|LGP_FFMPEG_DIR):[A-Z]+=.*\\", re.M)
    old = re.search(r"^CMAKE_CXX_COMPILER:[A-Z]+=(.*)$", text, re.M)
    home = re.search(r"^CMAKE_HOME_DIRECTORY:[A-Z]+=(.*)$", text, re.M)
    why = None
    if stale.search(text):
        why = "old CMake cache with backslash paths"
    elif home and Path(home.group(1).strip()).resolve() != REPO.resolve():
        why = f"build folder from another source folder ({home.group(1).strip()})"
    elif cxx and old and Path(old.group(1).strip()) != Path(cxx):
        why = "build folder with another compiler"
    if why:
        say(f"== {why}; creating it anew")
        cache.unlink()
        shutil.rmtree(tree / "CMakeFiles", ignore_errors=True)


def settings_game_override() -> tuple[Path, str] | None:
    """`game = …` in the app's settings.ini wins over the game in the package (docs/knowledge/desktop-app-huelle.md)."""
    if os.environ.get("LGP_APP_DIR"):
        base = Path(os.environ["LGP_APP_DIR"])
    elif os.name == "nt":
        base = Path(os.environ.get("APPDATA", ""), "lgp-recomp")
    elif sys.platform == "darwin":
        base = Path.home() / "Library" / "Application Support" / "lgp-recomp"
    else:
        base = Path(os.environ.get("XDG_CONFIG_HOME") or Path.home() / ".config") / "lgp-recomp"
    ini = base / "settings.ini"
    try:
        for line in ini.read_text(encoding="utf-8").splitlines():
            key, sep, value = line.partition("=")
            if sep and key.strip() == "game" and value.strip():
                return ini, value.strip()
    except OSError:
        pass
    return None


# --- the assistant (no arguments, or double-click on lgp-bauen.cmd / lgp-bauen.command) -------------------------


def ask(question: str, default: str = "") -> str:
    hint = f" [{default}]" if default else ""
    try:
        answer = input(f"{question}{hint}: ").strip()
    except EOFError:
        raise KitError("Input ended") from None
    return answer or default


def ask_yes(question: str, default: bool = True) -> bool:
    answer = ask(question + (" (Y/n)" if default else " (y/N)")).lower()
    return default if not answer else answer in ("j", "ja", "y", "yes")


def dropped_path(text: str) -> Path:
    """A path typed or dragged into the terminal: without quotes and the escaping of macOS' Terminal."""
    text = text.strip().strip('"').strip("'")
    m = re.match(r"^([A-Za-z]):[\\/](.*)$", text)
    if m and in_wsl():  # dragged from the Windows Explorer into a WSL window
        return Path("/mnt", m.group(1).lower(), *re.split(r"[\\/]+", m.group(2)))
    if os.name != "nt":
        text = re.sub(r"\\(.)", r"\1", text)
    return Path(text).expanduser()


def open_in_file_manager(path: Path) -> None:
    try:
        if os.name == "nt":
            subprocess.run(["explorer", "/select,", str(path)])
        elif sys.platform == "darwin":
            subprocess.run(["open", "-R", str(path)])
    except OSError:
        pass


def ensure_tools(tc: Toolchain) -> bool:
    """Checks the tools and offers to install what is missing; False when the user stops or nothing helps."""
    tried: set[tuple[str, ...]] = set()  # each offer once: a failed one is not offered again and again
    while True:
        ok = tc.check()
        if ok and not tc.optional:
            break
        for problem in tc.problems:
            say("  MISSING: " + problem)
        for item in tc.optional:
            say("  OPTIONAL, MISSING: " + item)
        offers = [(what, cmd) for what, cmd in tc.installs if tuple(cmd) not in tried]
        if not offers:
            if ok:
                say("  Going on without it.")
                break
            say("\nPlease install what is missing (see above), then start the assistant again.")
            return False
        say("\nI can install this:")
        for what, _ in offers:
            say("  - " + what)
        if not ask_yes("Install now?"):
            if ok:
                say("  Going on without it.")
                break
            return False
        for what, cmd in offers:
            tried.add(tuple(cmd))
            say(f"\n== {what}")
            say("$ " + " ".join(cmd))
            rc = subprocess.run(cmd).returncode
            if rc != 0:
                say(f"This step failed (exit code {rc}); its message is above."
                    + (" With winget this often means: already installed." if cmd[0] == "winget" else ""))
        if sys.platform == "darwin" and any(c[0] == "xcode-select" for _, c in offers):
            ask("Press Enter when Apple's dialog has finished")
        say("\nChecking again …")
    for note in tc.notes:
        say("  Note: " + note)
    say("  All tools needed for the build are there." if tc.optional else "  All tools for the build are there.")
    return True


def assistant() -> int:
    NOTES_SHOWN.clear()
    say("LGP build assistant: builds the game from your own dump.")
    say("Cancel at any time with Ctrl+C. What is made here contains game data: for your own use only.\n")
    # Set by the Windows assistant when it hands an iPhone build over to WSL (assistant_via_wsl).
    preset_target, preset_kit = os.environ.get("LGP_KIT_TARGET", ""), os.environ.get("LGP_KIT_KIT", "")
    if ASSISTANT_INSTALLER == "sideload" and not preset_target:  # lgp-bauen.command --installer sideload
        preset_target = "ios"
    if preset_target in ("desktop", "ios", "android"):
        target = preset_target
    elif sys.platform == "darwin":
        say("Build for what?  1) this Mac   2) iPhone/iPad (needs Xcode and an Apple ID)   3) Android")
        target = {"2": "ios", "3": "android"}.get(ask("Choice", "1"), "desktop")
    else:
        say("Build for what?  1) this computer   2) Android (phone, tablet, Shield)   "
            + ("3) iPhone/iPad (draft, through WSL)" if os.name == "nt" else "3) iPhone/iPad (draft)"))
        target = {"2": "android", "3": "ios"}.get(ask("Choice", "1"), "desktop")
    via_wsl = os.name == "nt" and target == "ios"

    say("\n[1/3] Checking the tools")
    tc = Toolchain("extract" if via_wsl else target)
    if via_wsl:
        say("  The build runs in WSL (Linux in Windows); I check the tools there after you pick the dump.")
    elif not ensure_tools(tc):
        return 1

    say("\n[2/3] Choosing the dump")
    extra: list[str] = []
    if preset_kit:
        extra = ["--kit", preset_kit]
        say(f"  {preset_kit}")
        if os.environ.get("LGP_KIT_FILES"):
            extra += ["--kit-files", os.environ["LGP_KIT_FILES"]]
        elif os.environ.get("LGP_KIT_URL") and not (Path(preset_kit) / "lgp-kit.json").exists() \
                and not zipfile.is_zipfile(preset_kit):
            extra += ["--kit-url", os.environ["LGP_KIT_URL"]]
    while not extra:
        what = ("Drag your dump (NSP/XCI), the dump ZIP from the kit page or an unpacked game folder" if DUMP
                else "Drag your unpacked game folder (exefs/ plus romfs.bin or romfs/)")
        kit = dropped_path(ask(what + " in here and press Enter"))
        if DUMP and DUMP.is_dump(kit):
            extra = ["--dump", str(kit)]
            break
        try:
            info, names = kit_listing(kit)
            files: list[str] = []
            if info.get("plain") or "game.cfg" not in names:  # game.cfg and the mods come separately
                if os.environ.get("LGP_KIT_URL"):
                    files = ["--kit-url", os.environ["LGP_KIT_URL"]]
                    names.add("game.cfg")  # from the kit server
                elif bundled_kit_files():
                    say(f"  game.cfg and the mods come from the package ({BUNDLED_KIT_FILES})")
                    names |= set(kit_file_names(BUNDLED_KIT_FILES))
                    files = ["--kit-files", str(BUNDLED_KIT_FILES)]
                else:
                    folder = dropped_path(ask("Drag the folder with game.cfg and mods/ in here"))
                    if not folder.is_dir():
                        raise KitError(f"{folder} is not a folder.")
                    names |= set(kit_file_names(folder))
                    files = ["--kit-files", str(folder)]
            for w in check_kit(info, names):
                say("  Note: " + w)
                NOTES_SHOWN.add(w)
            extra = ["--kit", str(kit), *files]
            break
        except KitError as e:
            say("  " + str(e))
    if extra[0] == "--dump" and not DUMP.assistant_questions(tc, extra):
        return 1
    if via_wsl:
        return assistant_via_wsl(extra)
    argv = ["build", *extra, "--target", target]
    return assistant_build(argv, target, tc)


def wsl_distro() -> str | None:
    """An installed WSL distribution with apt (Ubuntu, Debian), seen from Windows."""
    try:
        r = subprocess.run(["wsl.exe", "-l", "-q"], capture_output=True, timeout=60)
    except (OSError, subprocess.SubprocessError):
        return None
    names = [n.strip() for n in r.stdout.decode("utf-16-le", errors="ignore").replace("\x00", "").splitlines()]
    return next((n for n in names if n.lower().startswith(("ubuntu", "debian"))), None)


def assistant_via_wsl(extra: list[str]) -> int:
    """Windows, iPhone/iPad: unpacks a dump here (Node from the package), then runs the assistant in WSL on the
    result, where the iOS tools live (docs/knowledge/ios-ohne-mac.md). Draft, not yet run on a PC."""
    distro = wsl_distro()
    if not distro:
        say("\niPhone/iPad without a Mac builds in WSL (Linux in Windows), and WSL with Ubuntu is still missing.")
        if ask_yes("Install WSL with Ubuntu 24.04 now (asks for admin rights, then a restart)?"):
            subprocess.run(["powershell", "-NoProfile", "-Command",
                            "Start-Process wsl -ArgumentList '--install','-d','Ubuntu-24.04' -Verb RunAs -Wait"])
            say("After the restart, open Ubuntu once from the Start menu (create a user and password), then "
                "start this assistant again.")
        return 1
    if extra[0] == "--dump":
        kit = DUMP.extract_for_wsl(extra)
    else:
        kit = Path(extra[1]).resolve()
    say(f"\n== Continuing in WSL ({distro}); if it asks for a password, it is your Ubuntu user's")
    env = dict(os.environ, LGP_KIT_TARGET="ios", LGP_KIT_KIT=str(kit))
    if "--kit-files" in extra:  # a plain game folder's game.cfg and mods
        env["LGP_KIT_FILES"] = str(Path(extra[extra.index("--kit-files") + 1]).resolve())
    env.pop("LGP_KIT_PAUSE", None)  # this window stays open anyway
    env["WSLENV"] = ":".join(filter(None, [os.environ.get("WSLENV", ""), "LGP_KIT_TARGET", "LGP_KIT_KIT/p",
                                           "LGP_KIT_FILES/p", "LGP_KIT_URL"]))
    # --exec: without it the default shell eats the backslashes of D:\… (PC, 2026-10-25: wslpath exit 1)
    r = subprocess.run(["wsl.exe", "-d", distro, "--exec", "wslpath", "-a", REPO.as_posix()], capture_output=True,
                       text=True)
    repo = r.stdout.strip()
    if r.returncode != 0 or not repo:
        detail = (r.stderr or r.stdout).strip().replace("\x00", "")
        raise KitError(f"WSL ({distro}) could not translate the repo path (wslpath, exit code {r.returncode}"
                       f"{': ' + detail if detail else ''}). If Ubuntu does not start from the Start menu, set it "
                       "up there first and try again.")
    cmd = ["wsl.exe", "-d", distro, "--cd", "~", "--exec", "python3", f"{repo}/tools/kit/build.py"]
    if sys.stdin is not None and not sys.stdin.isatty():
        # Answers piped in from a file: Python has already buffered the rest of them, so WSL would read EOF
        # (PC, 2026-10-26: "Input ended" at the bundle prefix). Hand the remaining answers on explicitly.
        return subprocess.run(cmd, env=env, input=sys.stdin.read(), text=True).returncode
    return subprocess.run(cmd, env=env).returncode


# lgp-bauen.command --installer sideload: the assistant builds the iPhone/iPad .ipa for a sideloader without asking.
ASSISTANT_INSTALLER = ""


def assistant_build(argv: list[str], target: str, tc: Toolchain) -> int:
    if target == "ios" and sys.platform != "darwin":
        prefix = ask("Your own bundle ID prefix, e.g. com.yourname.lgp")
        if prefix:
            argv += ["--bundle-prefix", prefix]
    elif target == "ios" and ASSISTANT_INSTALLER == "sideload":
        argv += ["--installer", "sideload"]
        paid = True  # sideload already turns iCloud off and asks for increased-memory-limit only
    elif target == "ios":
        teams = xcode_teams()
        team = None
        if len(teams) == 1:
            team = teams[0]
            say(f"  Apple team from Xcode: {team['teamName'] or team['teamID']} ({team['teamID']}"
                f"{', free' if team['free'] else ''})")
        elif teams:
            for i, tm in enumerate(teams, 1):
                say(f"  {i}) {tm['teamName'] or '?'} ({tm['teamID']}{', free' if tm['free'] else ''})")
            pick = ask("Which Apple team?", "1")
            team = teams[int(pick) - 1] if pick.isdigit() and 1 <= int(pick) <= len(teams) else teams[0]
        if team and ask_yes("Build an .ipa for a sideloader instead of signing in Xcode "
                            "(Impactor, SideStore, AltStore)?", False):
            argv += ["--installer", "sideload"]
            paid = True  # sideload already turns iCloud off and asks for increased-memory-limit only
        elif team:
            argv += ["--ios-team", team["teamID"]]
            paid = not team["free"]
        else:
            say("  No Apple team in Xcode (Xcode > Settings > Accounts).")
            if ask_yes("Build an .ipa for a sideloader instead (Impactor, SideStore, AltStore; no team needed)?"):
                argv += ["--installer", "sideload"]
                paid = True  # sideload already turns iCloud off and asks for increased-memory-limit only
            else:
                argv += ["--ios-team", ask("Apple team ID (Xcode > Settings > Accounts, 10 characters)")]
                paid = ask_yes("Paid Apple developer account?", False)
        prefix = ask("Your own bundle ID prefix, e.g. com.yourname.lgp")
        if prefix:
            argv += ["--bundle-prefix", prefix]
        if not paid:
            # A free team (Personal Team): no iCloud, no extended-virtual-addressing (Mac test 2026-10-25).
            argv += ["--no-cloud-saves", "--memory-entitlements", "limit"]
    args = parse_args(argv)
    say(f"\n[3/3] Building into {args.work}")
    say("The first time takes a while (downloads, compiling); later runs are faster.")
    if not ask_yes("Start?"):
        return 1
    product = Builder(args, tc).build()
    if target == "ios" and installer_for(args) == "sideload":
        pass  # the build's last lines say how to install the .ipa with a sideloader
    elif target in ("ios", "android") and ask_yes("\nInstall on the device now (connected by USB)?"):
        try:
            Installer(parse_args(["install", "--target", target, "--work", str(args.work)]), tc).install()
        except KitError as e:
            say("ERROR: " + str(e))
            say(f"To try again later: python3 tools/kit/build.py install --target {target}")
    open_in_file_manager(product)
    return 0


# --- install on a device ---------------------------------------------------------------------------------------


def xcode_teams() -> list[dict]:
    """The teams of the Apple IDs signed in to Xcode (Mac): [{teamID, teamName, free}]. Read from Xcode's settings
    (IDEProvisioningTeams / IDEProvisioningTeamByIdentifier), whichever this Xcode keeps; [] when none."""
    if sys.platform != "darwin":
        return []
    import plistlib
    try:
        raw = subprocess.run(["defaults", "export", "com.apple.dt.Xcode", "-"], capture_output=True, timeout=30).stdout
        prefs = plistlib.loads(raw)
    except (OSError, subprocess.SubprocessError, plistlib.InvalidFileException, ValueError):
        return []
    teams: dict[str, dict] = {}

    def walk(node) -> None:
        if isinstance(node, dict):
            if isinstance(node.get("teamID"), str):
                teams.setdefault(node["teamID"], {"teamID": node["teamID"], "teamName": str(node.get("teamName", "")),
                                                  "free": bool(node.get("isFreeProvisioningTeam"))})
            for v in node.values():
                walk(v)
        elif isinstance(node, list):
            for v in node:
                walk(v)

    for key in ("IDEProvisioningTeams", "IDEProvisioningTeamByIdentifier"):
        walk(prefs.get(key))
    return list(teams.values())


def ios_devices() -> list[tuple[str, str]]:
    """Connected iPhones/iPads as (identifier, name) via `xcrun devicectl list devices` (Mac)."""
    import tempfile
    with tempfile.TemporaryDirectory() as tmp:
        out = Path(tmp, "devices.json")
        r = subprocess.run(["xcrun", "devicectl", "list", "devices", "--json-output", str(out)], capture_output=True)
        if r.returncode != 0 or not out.is_file():
            return []
        try:
            devices = json.loads(out.read_text(encoding="utf-8"))["result"]["devices"]
        except (ValueError, KeyError, TypeError):
            return []
    found = []
    for d in devices:
        hw, props, conn = d.get("hardwareProperties", {}), d.get("deviceProperties", {}), d.get("connectionProperties", {})
        if hw.get("platform") == "iOS" and conn.get("pairingState") == "paired":
            found.append((d.get("identifier", ""), props.get("name", "?")))
    return found


def adb_devices(adb: str) -> list[str]:
    r = subprocess.run([adb, "devices"], capture_output=True, text=True)
    return [line.split()[0] for line in r.stdout.splitlines()[1:] if line.strip().endswith("\tdevice")]


def installer_for(args) -> str:
    """--installer as given, else sideload without a Mac and xcode on one (the Mac builds, signs and installs)."""
    chosen = getattr(args, "installer", "auto") or "auto"
    if chosen != "auto":
        return chosen
    return "xcode" if sys.platform == "darwin" else "sideload"


def say_sideload(ipa: Path) -> None:
    say(f"Install: open {ipa} with a sideloader that keeps the app's entitlements and sign in there with your "
        "Apple ID (a free one works: the app then runs 7 days, install again after that, the saves stay):")
    for name, where, url in SIDELOADERS:
        say(f"  {name} ({where}): {url}")
    say("  Not Sideloadly: it drops increased-memory-limit, and the game is then closed for lack of memory.")
    say("  If Impactor asks to reset your certificate: yes revokes an old one, and other apps signed with the same "
        "Apple ID stop working (back up their saves first, or say no).")
    say("  Check after the first start: Documents/logs/lgp.log has a line `code signature: increased-memory-limit "
        "signed yes, profile yes` when the sideloader kept it.")
    say(f"  What the .ipa carries: python3 tools/ios/make_ipa.py --show {ipa}")


class Installer:
    """build.py install: puts what `build` made onto the device, without building again. With a free Apple ID this
    is also how the app gets its next 7 days (xtool signs anew each time; on a Mac Xcode re-signs in the build)."""

    def __init__(self, args, tc: Toolchain):
        self.args, self.tc = args, tc
        self.work = Path(args.work).resolve()

    def run(self, cmd: list[str]) -> None:
        say("$ " + " ".join(f'"{c}"' if " " in c else c for c in cmd))
        if self.args.dry_run:
            return
        r = subprocess.run(cmd, env=self.tc.env)
        if r.returncode != 0:
            raise KitError(f"Installing failed (exit code {r.returncode}): {cmd[0]}")

    def need(self, path: Path) -> Path:
        if not path.exists() and not self.args.dry_run:
            raise KitError(f"{path} is missing: run `build.py build --target {self.args.target}` first.")
        return path

    def install(self) -> None:
        if self.args.target == "android":
            self.android()
        elif self.args.target == "ios" and installer_for(self.args) == "sideload":
            say_sideload(self.need(self.work / "ios" / "lgp_game.ipa"))
            return
        elif self.args.target == "ios" and sys.platform == "darwin":
            self.ios_mac()
        elif self.args.target == "ios":
            ipa = self.need(self.work / "ios" / "lgp_game.ipa")
            xtool = find_tool("xtool")
            if not xtool and not self.args.dry_run:
                raise KitError("xtool is missing: https://xtool.sh/documentation/xtool/installation-linux")
            if xtool:
                say(f"xtool: {xtool_version(xtool) or 'version unknown'} (keep it at the newest version; on error "
                    "-80046 update xtool and sign in again)")
            say("Connect and unlock the iPhone/iPad by USB; xtool signs in with your Apple ID.")
            self.run([xtool or "xtool", "install", str(ipa)])
        else:
            raise KitError("There is nothing to install for this computer: the app is in the build folder.")
        say("Installed. The package contains game data: for your own use only, do not pass it on.")

    def android(self) -> None:
        abi = getattr(self.args, "abi", "arm64-v8a")
        folder = self.need(self.work / ("android" if abi == "arm64-v8a" else f"android-{abi}"))
        apks = [folder / "lgp_game.apk"] + sorted(folder.glob("game-*.apk")) if folder.is_dir() else []
        if folder.is_dir() and not (folder / "lgp_game.apk").is_file():
            raise KitError(f"{folder} holds no lgp_game.apk: build first.")
        sdk = android_sdk()
        adb = find_tool("adb", [str(sdk / "platform-tools")])
        if not adb and not self.args.dry_run:
            raise KitError("adb is missing: python tools/kit/build.py fetch-android")
        cmd = [adb or "adb"]
        if not self.args.dry_run:
            devices = adb_devices(adb)
            if self.args.device:
                cmd += ["-s", self.args.device]
            elif len(devices) != 1:
                raise KitError("No Android device found (USB debugging on, allow the connection on the device)."
                               if not devices else
                               f"Several devices ({', '.join(devices)}): choose one with --device <serial>.")
        self.run(cmd + ["install-multiple", "-r", *map(str, apks or [folder / "lgp_game.apk"])])

    def ios_mac(self) -> None:
        app = self.need(self.work / "build-ios" / "runtime" / "boot" / "Release-iphoneos" / "lgp_game.app")
        device = self.args.device
        if not device and not self.args.dry_run:
            devices = ios_devices()
            if len(devices) != 1:
                raise KitError("No iPhone/iPad found (connect by USB, unlock, tap \"Trust\")."
                               if not devices else
                               "Several devices: choose one with --device <identifier>: "
                               + ", ".join(f"{name} ({ident})" for ident, name in devices))
            device = devices[0][0]
            say(f"Device: {devices[0][1]}")
        self.run(["xcrun", "devicectl", "device", "install", "app", "--device", device or "<device>", str(app)])
        say("The first time, on the device: Settings > General > VPN & Device Management > trust your "
            "Apple ID.")


# --- main -------------------------------------------------------------------------------------------------------


# WSL: the repo on a Windows drive (/mnt/…, 9P) makes every file access slow (PC, 2026-10-25: configuring took over
# 40 min there against 73 s from a clone in the WSL home, though slow downloads played a part), so build.py mirrors the repo into the Linux file system and
# runs from there. Top-level folders that are build output or Windows-only stay behind.
WSL_MIRROR_SKIP_TOP = {".git", "build", "bauset", "out"}
WSL_MIRROR_SKIP_ANY = {"__pycache__", "node_modules"}


def wsl_mirror_dir() -> Path:
    return Path(os.environ.get("LGP_KIT_WSL_REPO") or Path.home() / "lgp-recomp-kit")


def mirror_repo(src: Path, dst: Path) -> int:
    """Copies src's files to dst where size or time differ and removes what src no longer has; returns the count
    of files copied. Folders in WSL_MIRROR_SKIP_* are left out on both sides."""
    copied, seen = 0, set()
    for root, dirs, files in os.walk(src):
        rel = Path(root).relative_to(src)
        dirs[:] = [d for d in dirs if d not in WSL_MIRROR_SKIP_ANY and not (rel == Path(".") and d in WSL_MIRROR_SKIP_TOP)]
        (dst / rel).mkdir(parents=True, exist_ok=True)
        for name in files:
            if rel == Path(".") and name in WSL_MIRROR_SKIP_TOP:  # .git is a file in a worktree
                continue
            a, b = Path(root) / name, dst / rel / name
            seen.add(rel / name)
            st = a.stat()
            try:
                bst = b.stat()
                if bst.st_size == st.st_size and int(bst.st_mtime) == int(st.st_mtime):
                    continue
            except OSError:
                pass
            shutil.copy2(a, b)
            copied += 1
    for root, dirs, files in os.walk(dst):
        rel = Path(root).relative_to(dst)
        dirs[:] = [d for d in dirs if d not in WSL_MIRROR_SKIP_ANY and not (rel == Path(".") and d in WSL_MIRROR_SKIP_TOP)]
        for name in files:
            if rel / name not in seen and not (rel == Path(".") and name in WSL_MIRROR_SKIP_TOP):
                (Path(root) / name).unlink()
    return copied


def run_from_wsl_mirror(argv: list[str]) -> int | None:
    """In WSL with the repo under /mnt/: mirror it and run build.py from the copy (exec, so it does not return);
    None when nothing is to do (LGP_KIT_WSL_MIRROR=0 turns it off)."""
    if not (in_wsl() and str(REPO).startswith("/mnt/") and os.environ.get("LGP_KIT_WSL_MIRROR", "1") != "0"):
        return None
    dst = wsl_mirror_dir()
    say(f"== Mirroring the repo into {dst} (WSL builds very slowly from /mnt/…)")
    say(f"   {mirror_repo(REPO, dst)} files updated")
    env = dict(os.environ, LGP_KIT_WSL_MIRROR="0", LGP_KIT_REPO_ORIGIN=str(REPO))
    sys.stdout.flush()
    os.execve(sys.executable, [sys.executable, str(dst / "tools" / "kit" / "build.py"), *argv], env)
    return 0  # only reached when execve is mocked in tests


def default_work() -> Path:
    private = os.environ.get("LGP_PRIVATE_DIR")
    if private:
        return Path(private, "kit-build")
    if in_wsl() and (str(REPO).startswith("/mnt/") or os.environ.get("LGP_KIT_REPO_ORIGIN", "").startswith("/mnt/")):
        return Path.home() / "lgp-kit-build"  # building on the Windows drive from WSL is slow and fragile
    return REPO / "build" / "kit"


def parse_args(argv: list[str]) -> argparse.Namespace:
    p = argparse.ArgumentParser(description="Builds the game from your own dump (kit ZIP or unpacked game folder).")
    sub = p.add_subparsers(dest="command", required=True)
    sub.add_parser("fetch-compiler", help="Windows: load llvm-mingw into the tool cache")
    sub.add_parser("fetch-ffmpeg", help="Windows: load FFmpeg (LGPL build, for the videos) into the tool cache")
    sub.add_parser("fetch-rcodesign", help="iOS without a Mac: load rcodesign into the tool cache")
    sub.add_parser("fetch-jdk", help=f"load JDK {JDK[0]} (Microsoft) as a folder into the tool cache (for Android)")
    sub.add_parser("fetch-android", help="load the Android SDK, NDK r30 and adb with Google's sdkmanager (needs a JDK)")
    ins = sub.add_parser("install", help="put the build onto the device (without building again; also the next 7 "
                                         "days with a free Apple ID)")
    ins.add_argument("--target", choices=("ios", "android"), required=True)
    ins.add_argument("--installer", choices=("auto", "sideload", "xtool", "xcode"), default="auto",
                     help="ios: sideload = show how to install the .ipa with a sideloader (default without a Mac), "
                          "xtool = xtool install with your Apple ID, xcode = devicectl (Mac, default there)")
    ins.add_argument("--work", default=str(default_work()), help="the build's work folder")
    ins.add_argument("--device", help="device when several are connected (adb serial or devicectl identifier)")
    ins.add_argument("--abi", choices=("arm64-v8a", "x86_64"), default="arm64-v8a",
                     help="android: x86_64 = the emulator build (android-x86_64/)")
    ins.add_argument("--dry-run", action="store_true", help="only show the commands")
    inst = sub.add_parser("install-android", help="install the built APKs with adb onto the connected device")
    inst.add_argument("--work", default=str(default_work()), help="the build's work folder")
    inst.add_argument("--abi", choices=("arm64-v8a", "x86_64"), default="arm64-v8a",
                      help="x86_64: the emulator build (android-x86_64/)")
    for name in ("doctor", "build"):
        s = sub.add_parser(name)
        s.add_argument("--target", choices=("desktop", "ios", "android"), default="desktop",
                       help="desktop = this computer (Windows, macOS, Linux); ios = iPhone/iPad (Mac; draft: "
                            "Linux/WSL with the SDK from Xcode.xip and xtool); "
                            "android = APK plus the game as split APKs")
        s.add_argument("--compiler", choices=("mingw", "msvc"), default="mingw",
                       help="Windows only: mingw = llvm-mingw, loaded by build.py itself (default); "
                            "msvc = Visual Studio Build Tools + LLVM")
        if name == "build":
            src = s.add_mutually_exclusive_group(required=True)
            src.add_argument("--kit", help="dump ZIP or folder from the kit, or an unpacked game folder "
                                           "(exefs/ plus romfs.bin or romfs/, with --kit-files)")
            s.set_defaults(dump=None, keys=None, title_keys=None, firmware=None)
            if DUMP:
                DUMP.add_build_args(s, src)
            s.add_argument("--kit-url", default=os.environ.get("LGP_KIT_URL", ""),
                           help="address of the kit server for game.cfg and the mods (environment LGP_KIT_URL; with --kit "
                                "only when given explicitly)")
            s.add_argument("--kit-files", help="folder with game.cfg and mods/ (manifest.json optional) instead of "
                                               "the server; with --kit over the kit's own")
            s.add_argument("--work", default=str(default_work()),
                           help="work folder (game folder, lift, build trees); default $LGP_PRIVATE_DIR/kit-build")
            s.add_argument("--no-compress", action="store_true", help="do not compress romfs.bin")
            s.add_argument("--ffmpeg-dir", help="Windows: another FFmpeg \"shared\" build for the videos (default: the "
                                                "tool cache's, fetch-ffmpeg), none = without (black videos)")
            s.add_argument("--ios-team", help="Apple team ID (ios on a Mac with --installer xcode)")
            s.add_argument("--installer", choices=("auto", "sideload", "xtool", "xcode"), default="auto",
                           help="ios: sideload = an .ipa signed ad hoc with the entitlements, installed with "
                                "Impactor, SideStore or AltStore (default without a Mac; on a Mac without a team); "
                                "xtool = the same .ipa, installed by `build.py install` with xtool; xcode = Xcode "
                                "signs with --ios-team (Mac, default there)")
            s.add_argument("--memory-entitlements", choices=("auto", "both", "limit", "none"), default="auto",
                           help="ios: both = increased-memory-limit and extended-virtual-addressing (paid team), "
                                "limit = only the first (free Apple ID, default without --ios-team), none = neither, "
                                "guest memory then on a file")
            s.add_argument("--bundle-prefix", help="your own bundle ID prefix, e.g. com.<name>.lgp (ios)")
            s.add_argument("--game-separate", action="store_true",
                           help="ios: slim app without the game; the game folder is copied into the app once "
                                "(Finder or the Apple Devices app)")
            s.add_argument("--no-cloud-saves", action="store_true",
                           help="without iCloud saves (needed with a free Apple ID)")
            s.add_argument("--abi", choices=("arm64-v8a", "x86_64"), default="arm64-v8a",
                           help="android: arm64-v8a = phones/tablets (default); x86_64 = only for the "
                                "Android emulator's x86_64 image")
            s.add_argument("--jobs", type=int, help="parallel compile jobs (default: by free RAM and cores)")
            s.add_argument("--measure", metavar="LOG", help="peak RAM and CPU time of every compile and "
                                                            "link step into LOG (tools/kit/measure_cc.py)")
            s.add_argument("--dry-run", action="store_true", help="only check and show the commands")
    args = p.parse_args(argv)
    if getattr(args, "kit", None) and not any(a == "--kit-url" or a.startswith("--kit-url=") for a in argv):
        args.kit_url = ""  # LGP_KIT_URL is for --dump and plain game folders named on the command line
    return args


def main(argv: list[str] | None = None) -> int:
    if os.name == "nt" and not sys.stdout.isatty() and hasattr(sys.stdout, "reconfigure"):
        # Git Bash (mintty) reads a pipe as UTF-8; cmd/PowerShell consoles get Unicode from Python anyway.
        # (Tests swap in a StringIO, which has no reconfigure.)
        sys.stdout.reconfigure(encoding="utf-8")
    argv = sys.argv[1:] if argv is None else argv
    mirrored = run_from_wsl_mirror(argv)
    if mirrored is not None:
        return mirrored
    global ASSISTANT_INSTALLER
    ASSISTANT_INSTALLER = ""
    if argv in (["--installer", "sideload"], ["assistant", "--installer", "sideload"]):
        ASSISTANT_INSTALLER, argv = "sideload", []
    if not argv or argv == ["assistant"]:
        try:
            return assistant()
        except KitError as e:
            say("ERROR: " + str(e))
            return 1
        except KeyboardInterrupt:
            say("\nCancelled.")
            return 130
        finally:
            if os.environ.get("LGP_KIT_PAUSE"):  # started by double-click: keep the window open
                try:
                    input("\nPress Enter to close.")
                except (EOFError, KeyboardInterrupt):
                    pass
    args = parse_args(argv)
    if args.command == "fetch-compiler":
        try:
            say("llvm-mingw is in " + str(fetch_llvm_mingw().parent))
        except (KitError, OSError) as e:
            say("ERROR: " + str(e))
            return 1
        return 0
    if args.command == "fetch-ffmpeg":
        try:
            say("FFmpeg is in " + str(fetch_ffmpeg()))
        except (KitError, OSError) as e:
            say("ERROR: " + str(e))
            return 1
        return 0
    if args.command == "fetch-jdk":
        try:
            say("The JDK is in " + str(fetch_jdk()))
        except (KitError, OSError) as e:
            say("ERROR: " + str(e))
            return 1
        return 0
    if args.command == "fetch-rcodesign":
        try:
            say("rcodesign is in " + str(fetch_rcodesign()))
        except (KitError, OSError) as e:
            say("ERROR: " + str(e))
            return 1
        return 0
    if args.command == "install-android":
        try:
            out = Path(args.work).resolve() / ("android" if args.abi == "arm64-v8a" else f"android-{args.abi}")
            install_android(out, android_sdk())
        except (KitError, OSError) as e:
            say("ERROR: " + str(e))
            return 1
        return 0
    if args.command == "fetch-android":
        try:
            say("The Android SDK is in " + str(fetch_android()))
        except (KitError, OSError) as e:
            say("ERROR: " + str(e))
            return 1
        return 0
    if args.command == "install":
        tc = Toolchain(args.target)
        if args.target == "android":
            tc.check_android()  # JAVA_HOME and the SDK's platform-tools on PATH
        try:
            Installer(args, tc).install()
        except KitError as e:
            say("ERROR: " + str(e))
            return 1
        return 0
    tc = Toolchain(args.target, args.compiler, need_node=bool(getattr(args, "dump", None)),
                   ffmpeg=getattr(args, "ffmpeg_dir", None))
    ok = tc.check()
    for n in tc.notes:
        say("Note: " + n)
    for item in tc.optional:
        say("OPTIONAL, MISSING: " + item)
    if args.command == "doctor" or not ok:
        for problem in tc.problems:
            say("MISSING: " + problem)
        if ok:
            say("All tools for the build are there.")
        if not ok and not getattr(args, "dry_run", False):
            return 1
        if args.command == "doctor":
            return 0 if ok else 1
    try:
        Builder(args, tc).build()
    except KitError as e:
        say("ERROR: " + str(e))
        return 1
    return 0 if ok else 1  # a dry run with missing tools still shows the plan


if __name__ == "__main__":
    sys.exit(main())
