#!/usr/bin/env python3
"""Builds the ready-made Windows package of the build kit: lgp-bauset-windows-x64-<commit>.zip.

Inside, lgp-recomp/ is this repo at HEAD (git archive) plus lgp-recomp/bauset/ with everything build.py needs on a
Windows PC that has nothing installed: Python (embeddable), CMake, Ninja, Node.js and llvm-mingw, each at a pinned version
checked against its SHA-256. A friend unpacks it, double-clicks lgp-recomp/lgp-bauen.cmd and drops the dump (NSP/XCI) or the dump ZIP in.
No game data: without --kit-files the package is the same for everyone (the CI job kit-bauset builds it).

--kit-files DIR puts game.cfg and mods/ into lgp-recomp/kit-files/, which build.py takes when the dump has none
(release packages only; the files hold game addresses and never go into git or CI).
--no-tools leaves bauset/ out: lgp-build-kit-<commit>.zip, the source code with kit-files/ for Mac and Linux.
With --kit-files the package is a public release: run this script from the folder public_snapshot.py --git wrote;
from the private repository it stops (PRIVATE_ONLY).
--private-kit is for the private kit for friends (public_snapshot.py --private-kit): the package may then carry the
dump unpacking and is named lgp-kit-…, so it is never taken for a public one; the internal notes still stop it.

  python3 tools/kit/make_bauset.py --out DIR [--cache DIR] [--kit-files DIR] [--no-tools] [--private-kit]
"""
from __future__ import annotations

import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import time
import urllib.request
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import build as kb  # noqa: E402

# (name in bauset/, URL, SHA-256, top folder in the archive to strip or None, archive paths to leave out, licence)
TOOLS = [
    ("python", "https://www.python.org/ftp/python/3.12.10/python-3.12.10-embed-amd64.zip",
     "4acbed6dd1c744b0376e3b1cf57ce906f9dc9e95e68824584c8099a63025a3c3", None, (), "PSF-2.0 (LICENSE.txt)"),
    ("cmake", "https://github.com/Kitware/CMake/releases/download/v3.31.9/cmake-3.31.9-windows-x86_64.zip",
     "dc16867d0c0ffb35026976aff119335d5b47def44e76ae82ffafdc1f98b3d59d", "cmake-3.31.9-windows-x86_64",
     ("bin/cmake-gui.exe", "man/", "doc/cmake-3.31/html/"), "BSD-3-Clause (doc/cmake-3.31/Copyright.txt)"),
    ("ninja", "https://github.com/ninja-build/ninja/releases/download/v1.13.1/ninja-win.zip",
     "26a40fa8595694dec2fad4911e62d29e10525d2133c9a4230b66397774ae25bf", None, (), "Apache-2.0"),
    (kb.LLVM_MINGW[1],
     f"https://github.com/mstorsjo/llvm-mingw/releases/download/{kb.LLVM_MINGW[0]}/{kb.LLVM_MINGW[1]}.zip",
     kb.LLVM_MINGW[2], kb.LLVM_MINGW[1], kb.LLVM_MINGW_SKIP,
     "Apache-2.0 with LLVM exception, mingw-w64: ZPL and others (LICENSE.TXT)"),
]

README = """LGP build kit for Windows (64 bit)
=================================

1. Unpack this ZIP somewhere with about 20 GB free (a path without special characters is safest).
2. Double-click lgp-recomp\\lgp-bauen.cmd.
{drop}
The finished game is in lgp-recomp\\build\\kit\\build-desktop\\runtime\\boot (lgp_game.exe with the game beside it).
The game and the build folders contain game data from your dump: do not pass them on.
In detail, also for Android and iPhone/iPad: lgp-recomp\\tools\\kit\\GUIDE.md

Included (unchanged, each under its own licence):
{tools}
"""


def fetch(url: str, sha: str, cache: Path) -> Path:
    cache.mkdir(parents=True, exist_ok=True)
    target = cache / url.rsplit("/", 1)[1]
    if target.is_file() and hashlib.sha256(target.read_bytes()).hexdigest() == sha:
        return target
    print(f"== {url}", flush=True)
    part = target.with_suffix(target.suffix + ".part")
    with urllib.request.urlopen(url) as r, open(part, "wb") as out:
        shutil.copyfileobj(r, out, 1 << 20)
    got = hashlib.sha256(part.read_bytes()).hexdigest()
    if got != sha:
        part.unlink()
        raise SystemExit(f"{url}: SHA-256 {got}, expected {sha}")
    part.replace(target)
    return target


def unpack(archive: Path, dest: Path, strip: str | None, skip: tuple[str, ...]) -> dict[Path, int]:
    """Unpacks the archive into dest; returns the Unix mode of every file (git archive: 0o755 for scripts, else
    0o644), so the package keeps them even when the file system (Windows) has no executable bit."""
    modes: dict[Path, int] = {}
    with zipfile.ZipFile(archive) as z:
        for info in z.infolist():
            name = kb.safe_member(info.filename)
            if name is None:
                continue
            if strip:
                if not name.startswith(strip + "/"):
                    continue
                name = name[len(strip) + 1:]
            if not name or name.startswith(skip):
                continue
            out = dest / name
            out.parent.mkdir(parents=True, exist_ok=True)
            with z.open(info) as src, open(out, "wb") as dst:
                shutil.copyfileobj(src, dst, 1 << 20)
            # git archive records a Unix mode only for executable files; the others get 0o644 here, not the
            # 0o666/0o777 a Windows file system would report.
            mode = 0o755 if (info.external_attr >> 16) & 0o111 else 0o644  # only the executable bit counts
            modes[out] = mode
            if os.name != "nt":
                out.chmod(mode)
    return modes


def copy_kit_files(src: Path, dest: Path) -> int:
    """game.cfg and mods/ (as build.py's kit_file_names lists them) into dest, and the same for every other build's
    subfolder <build ID>/ that src has (build.py kit_files_for); returns the number of files."""
    names = kb.kit_file_names(src)
    if "game.cfg" not in names:
        sys.exit(f"--kit-files {src} has no game.cfg")
    copied = 0
    for sub in [""] + [b for b in kb.SUPPORTED_BUILDS if b != kb.MAIN_BUILD_ID and (src / b).is_dir()]:
        folder = src / sub if sub else src
        own = [n for n in kb.kit_file_names(folder) if n == "game.cfg" or n.startswith("mods/")]
        if sub and "game.cfg" not in own:
            sys.exit(f"--kit-files {folder} has no game.cfg")
        for n in own:
            target = dest / sub / n if sub else dest / n
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(folder / n, target)
        copied += len(own)
    return copied


# Paths that only the private repository has (tools/publish/public_snapshot.py leaves them out). A release package
# (--kit-files) is public: it must come from the snapshot, never carry the decryption or the internal notes.
# The private kit (--private-kit) may carry the second group, never the first.
PRIVATE_NOTES = ("CLAUDE.md", "docs/project-setup/", "docs/analysis/", "docs/agents/")
PRIVATE_ONLY = PRIVATE_NOTES + ("tools/extract/",)


def private_paths(repo: Path, paths: tuple[str, ...] = PRIVATE_ONLY) -> list[str]:
    """Paths of HEAD in repo that the public snapshot leaves out (or the given ones)."""
    names = subprocess.run(["git", "-C", str(repo), "ls-tree", "-r", "--name-only", "HEAD"], capture_output=True,
                           text=True, check=True).stdout.splitlines()
    return sorted({p for p in paths for n in names if n == p or (p.endswith("/") and n.startswith(p))})


def make(out: Path, cache: Path, repo: Path = kb.REPO, tools=TOOLS, kit_files: Path | None = None,
         private_kit: bool = False) -> Path:
    if private_kit and (found := private_paths(repo, PRIVATE_NOTES)):
        raise SystemExit(f"error: {repo} is the private repository ({', '.join(found)}); the private kit is built "
                         "from tools/publish/public_snapshot.py --private-kit --git, then this script from that folder")
    if kit_files and not private_kit and (found := private_paths(repo)):
        raise SystemExit(f"error: {repo} is the private repository ({', '.join(found)}); a release package with "
                         "--kit-files is built from the public snapshot: run tools/publish/public_snapshot.py --git, "
                         "then this script from that folder")
    commit = subprocess.run(["git", "-C", str(repo), "rev-parse", "--short=10", "HEAD"], capture_output=True,
                            text=True, check=True).stdout.strip()
    stamp = entry_time(repo)
    stage = out / f"lgp-stage-{commit}"
    shutil.rmtree(stage, ignore_errors=True)
    root = stage / "lgp-recomp"
    root.mkdir(parents=True)
    tar = subprocess.run(["git", "-C", str(repo), "archive", "--format=zip", "HEAD"], capture_output=True, check=True)
    (stage / "repo.zip").write_bytes(tar.stdout)
    modes = unpack(stage / "repo.zip", root, None, ())
    (stage / "repo.zip").unlink()
    bauset = root / "bauset"
    for name, url, sha, strip, skip, _ in tools:
        modes.update(unpack(fetch(url, sha, cache), bauset / name, strip, skip))
    if kit_files:
        print(f"kit files: {copy_kit_files(kit_files, root / 'kit-files')} (game.cfg, mods/)")
    if tools:
        lines = "\n".join(f"- {name}: {url} (SHA-256 {sha[:16]}…), licence {lic}" for name, url, sha, _, _, lic in tools)
        (stage / "README.txt").write_text(README.format(tools=lines, drop=DROP), encoding="utf-8", newline="\r\n")
        package = out / f"{'lgp-kit-' if private_kit else 'lgp-'}bauset-windows-x64-{commit}.zip"
    else:
        package = out / f"{'lgp-kit' if private_kit else 'lgp-build-kit'}-{commit}.zip"
    package.unlink(missing_ok=True)
    with zipfile.ZipFile(package, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as z:
        for p in sorted(stage.rglob("*")):
            if p.is_file():
                info = zipfile.ZipInfo.from_file(p, p.relative_to(stage).as_posix())
                info.compress_type = zipfile.ZIP_DEFLATED
                info.date_time = stamp
                # Every entry carries a Unix mode: 0o755 where git (or a tool archive) says so, else 0o644, also for
                # README.txt and kit-files/ (lgp-bauen.command must stay executable for a double-click on the Mac).
                info.external_attr = (0o100000 | modes.get(p, 0o644)) << 16
                info.create_system = 3  # Unix: on Windows ZipInfo says DOS, and unzip then ignores the mode
                with open(p, "rb") as src, z.open(info, "w") as dst:
                    shutil.copyfileobj(src, dst, 1 << 20)
    shutil.rmtree(stage)
    print(f"{package} ({package.stat().st_size / 1048576:.0f} MiB)")
    return package

def entry_time(repo: Path) -> tuple[int, int, int, int, int, int]:
    """One time for every ZIP entry: the commit's day minus one, in UTC.
    A ZIP entry holds a local time without a zone, and unpacking takes it as the unpacking computer's local time.
    The packing computer's clock (CET) put the files an hour into the future for a Mac in UTC, and Ninja then
    stopped with "manifest 'build.ninja' still dirty after 100 tries" (Mac VM, 2026-11-03). A day back covers every
    time zone (UTC-12 to UTC+14), and the same commit always gives the same entries."""
    seconds = int(subprocess.run(["git", "-C", str(repo), "log", "-1", "--format=%ct", "HEAD"], capture_output=True,
                                 text=True, check=True).stdout.strip())
    day = time.gmtime(seconds - 86400)
    return (max(day.tm_year, 1980), day.tm_mon, day.tm_mday, 0, 0, 0)


# Step 3 of the README: the public kit (no dump unpacking, build.DUMP is None) takes an unpacked game folder.
DROP = ("""3. Drag your dump (NSP/XCI) or the dump ZIP from the kit page into the window, press Enter, confirm "Start?" with y.
   The assistant takes the keys and the firmware from Ryujinx if it is installed; otherwise it asks for them.
""" if kb.DUMP else """3. Drag your unpacked game folder (exefs/ plus romfs.bin or romfs/) into the window, press Enter, confirm "Start?"
   with y. game.cfg and the mods come from lgp-recomp\\kit-files, if the package brings them.
""")


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--out", required=True, help="folder for the ZIP")
    p.add_argument("--cache", help="folder for the downloads (default <out>/downloads)")
    p.add_argument("--kit-files", help="folder with game.cfg and mods/ to put into kit-files/ (release packages only)")
    p.add_argument("--no-tools", action="store_true", help="source code only (lgp-build-kit-<commit>.zip, Mac/Linux)")
    p.add_argument("--private-kit", action="store_true",
                   help="package of the private kit for friends (lgp-kit-…, from public_snapshot.py --private-kit)")
    args = p.parse_args(argv)
    out = Path(args.out).resolve()
    make(out, Path(args.cache).resolve() if args.cache else out / "downloads", tools=[] if args.no_tools else TOOLS,
         kit_files=Path(args.kit_files).resolve() if args.kit_files else None, private_kit=args.private_kit)
    return 0


if __name__ == "__main__":
    sys.exit(main())
