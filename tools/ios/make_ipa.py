#!/usr/bin/env python3
"""Packs an iOS .app into an .ipa for a sideloader or xtool (docs/knowledge/ios-ohne-mac.md).

    make_ipa.py --app build/ios-cross/runtime/boot/lgp_game.app --out lgp_game.ipa
                [--entitlements build/ios-cross/lgp_game.entitlements]
                [--signer auto|codesign|rcodesign|ldid|none] [--signer-path PATH]
    make_ipa.py --show lgp_game.ipa|lgp_game.app|<executable>

With --entitlements the executable is first signed ad hoc with them (codesign on a Mac, else rcodesign or ldid,
whichever is installed), so the entitlements travel inside the binary: sideloaders (Impactor, SideStore, AltStore)
and `xtool install` take them from the executable's signature to ask Apple for the matching capabilities and
re-sign the whole bundle (xtool: XKit DeveloperServicesAddAppOperation, 7f7d5b7); without a signature they sign
with none. The .ipa is Payload/<name>.app zipped; symlinks and file modes are kept.

--show prints the entitlements in the executable's signature (and those the bundle's embedded.mobileprovision
allows, if it has one), to check an .ipa before it goes to the sideloader.

The app holds game data when it was built with LGP_APP_GAME_DIR: the .ipa stays on this computer.
"""
from __future__ import annotations

import argparse
import os
import plistlib
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path


def executable(app: Path) -> Path:
    with open(app / "Info.plist", "rb") as f:
        info = plistlib.load(f)
    exe = app / info["CFBundleExecutable"]
    if not exe.is_file():
        sys.exit(f"make_ipa: {exe} missing (CFBundleExecutable)")
    return exe


def sign_adhoc(app: Path, entitlements: Path, signer: str, signer_path: str | None = None) -> str:
    """Signs only the executable ad hoc: xtool reads the entitlements from its signature and re-signs the bundle."""
    if signer == "none":
        return "none"
    if signer == "auto":
        order = (["codesign"] if sys.platform == "darwin" else []) + ["rcodesign", "ldid"]
    else:
        order = [signer]
    if signer_path:
        order = ["ldid" if Path(signer_path).name.startswith("ldid") else "rcodesign"]
    for tool in order:
        path = signer_path or shutil.which(tool)
        if not path:
            continue
        if tool == "codesign":
            # Replaces the signature Xcode made (a build without a team has none) with an ad-hoc one.
            cmd = [path, "--force", "--sign", "-", "--entitlements", str(entitlements), str(executable(app))]
        elif tool == "rcodesign":
            # --entitlements-xml-file since apple-codesign 0.2x (build.py pins 0.29.0)
            cmd = [path, "sign", "--entitlements-xml-file", str(entitlements), str(executable(app))]
        else:
            cmd = [path, f"-S{entitlements}", str(executable(app))]
        print("$ " + " ".join(cmd), flush=True)
        if subprocess.run(cmd).returncode != 0:
            sys.exit(f"make_ipa: {tool} failed")
        return tool
    if signer != "auto":
        sys.exit(f"make_ipa: {signer} not found")
    print("make_ipa: neither rcodesign nor ldid found; the entitlements are NOT in the binary, the signer may drop "
          "them (install rcodesign: https://github.com/indygreg/apple-platform-rs/releases)", file=sys.stderr)
    return "none"


# Code signature layout (Apple's cs_blobs.h): LC_CODE_SIGNATURE points at a big-endian SuperBlob whose slot 5 holds
# the entitlements plist. Same reader as platform/os/src/code_signature.cpp (the app logs it at start).
LC_CODE_SIGNATURE, SUPERBLOB, ENTITLEMENTS_BLOB, ENTITLEMENTS_SLOT = 0x1D, 0xFADE0CC0, 0xFADE7171, 5
CPU_ARM64 = 0x0100000C


def _thin_entitlements(b: bytes, base: int, size: int) -> bytes:
    le = lambda at: int.from_bytes(b[at:at + 4], "little")
    be = lambda at: int.from_bytes(b[at:at + 4], "big")
    if size < 32 or base + size > len(b) or le(base) != 0xFEEDFACF:
        return b""
    ncmds, at = le(base + 16), base + 32
    end = at + le(base + 20)
    for _ in range(ncmds):
        if at + 8 > end:
            break
        cmd, cmdsize = le(at), le(at + 4)
        if cmdsize < 8:
            break
        if cmd == LC_CODE_SIGNATURE:
            off, length = base + le(at + 8), le(at + 12)
            if be(off) != SUPERBLOB:
                return b""
            for k in range(be(off + 8)):
                index = off + 12 + 8 * k
                if index + 8 > off + length:
                    break
                if be(index) == ENTITLEMENTS_SLOT:
                    blob = off + be(index + 4)
                    if be(blob) != ENTITLEMENTS_BLOB:
                        return b""
                    return b[blob + 8:blob + be(blob + 4)]
            return b""
        at += cmdsize
    return b""


def macho_entitlements(b: bytes) -> dict | None:
    """The entitlements in a Mach-O's code signature (fat: the arm64 slice), None without any."""
    raw = b""
    if b[:4] == b"\xca\xfe\xba\xbe":
        archs = [(int.from_bytes(b[8 + 20 * i:12 + 20 * i], "big"),
                  int.from_bytes(b[16 + 20 * i:20 + 20 * i], "big"),
                  int.from_bytes(b[20 + 20 * i:24 + 20 * i], "big")) for i in range(int.from_bytes(b[4:8], "big"))]
        pick = next((a for a in archs if a[0] == CPU_ARM64), archs[0] if archs else None)
        if pick:
            raw = _thin_entitlements(b, pick[1], pick[2])
    else:
        raw = _thin_entitlements(b, 0, len(b))
    return plistlib.loads(raw) if raw else None


def profile_entitlements(profile: bytes) -> dict | None:
    """The Entitlements of an embedded.mobileprovision (a CMS-signed plist), None without a plist."""
    start, end = profile.find(b"<?xml"), profile.rfind(b"</plist>")
    if start < 0 or end < start:
        return None
    return plistlib.loads(profile[start:end + 8]).get("Entitlements", {})


def show(path: Path) -> int:
    """Prints the entitlements of an .ipa, .app or executable."""
    profile = None
    if path.suffix == ".ipa":
        with zipfile.ZipFile(path) as z:
            info_name = next((n for n in z.namelist() if n.count("/") == 2 and n.endswith(".app/Info.plist")), None)
            if not info_name:
                sys.exit(f"make_ipa: {path} has no Payload/<name>.app/Info.plist")
            app_dir = info_name[:-len("Info.plist")]
            exe = z.read(app_dir + plistlib.loads(z.read(info_name))["CFBundleExecutable"])
            if app_dir + "embedded.mobileprovision" in z.namelist():
                profile = z.read(app_dir + "embedded.mobileprovision")
    elif path.is_dir():
        exe = executable(path).read_bytes()
        if (path / "embedded.mobileprovision").is_file():
            profile = (path / "embedded.mobileprovision").read_bytes()
    else:
        exe = path.read_bytes()
    ents = macho_entitlements(exe)
    if ents is None:
        print("signature: no entitlements (unsigned, or signed without them)")
    else:
        print("signature:")
        for key, value in sorted(ents.items()):
            print(f"  {key} = {value!r}")
    allowed = profile_entitlements(profile) if profile else None
    if allowed is not None:
        print("profile (embedded.mobileprovision):")
        for key, value in sorted(allowed.items()):
            print(f"  {key} = {value!r}")
    else:
        print("profile: none (the sideloader adds its own)")
    return 0


def pack(app: Path, out: Path) -> None:
    tmp = out.with_name(out.name + ".part")
    with zipfile.ZipFile(tmp, "w", zipfile.ZIP_DEFLATED, compresslevel=6, allowZip64=True) as z:
        for root, dirs, files in os.walk(app):
            dirs.sort()
            for name in sorted(dirs + files):
                p = Path(root) / name
                arc = "Payload/" + app.name + "/" + p.relative_to(app).as_posix()
                if p.is_symlink():
                    info = zipfile.ZipInfo(arc)
                    info.create_system = 3
                    info.external_attr = (0o120777 << 16)
                    z.writestr(info, os.readlink(p))
                    if name in dirs:
                        dirs.remove(name)  # do not walk into a linked folder
                elif p.is_file():
                    z.write(p, arc)  # keeps the mode bits (the executable stays executable)
    os.replace(tmp, out)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--app")
    ap.add_argument("--out")
    ap.add_argument("--entitlements")
    ap.add_argument("--signer", choices=("auto", "codesign", "rcodesign", "ldid", "none"), default="auto")
    ap.add_argument("--signer-path", help="rcodesign or ldid outside PATH (build.py's tool cache)")
    ap.add_argument("--show", metavar="IPA|APP|EXE", help="print the entitlements of an .ipa, .app or executable")
    a = ap.parse_args(argv)
    if a.show:
        return show(Path(a.show))
    if not a.app or not a.out:
        ap.error("--app and --out are needed (or --show)")
    app, out = Path(a.app).resolve(), Path(a.out).resolve()
    if not (app / "Info.plist").is_file():
        sys.exit(f"make_ipa: {app} is not an app bundle (no Info.plist)")
    executable(app)
    used = sign_adhoc(app, Path(a.entitlements).resolve(), a.signer, a.signer_path) if a.entitlements else "none"
    pack(app, out)
    print(f"make_ipa: {out} ({out.stat().st_size / 2**20:.0f} MiB, entitlements embedded with: {used})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
