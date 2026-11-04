#!/usr/bin/env python3
"""iPhoneOS SDK for lld (preset ios-cross): a copy whose .tbd stubs also name plain arm64.

The iOS 27 SDK's text stubs (.tbd) name mostly arm64e and arm64e.x1 targets, without plain arm64. Apple's ld64 lets
an arm64 program link against them; lld (checked: lld 21 on the PC, 2026-10-25) stops at arm64e.x1 ("unknown
architecture") and only takes symbols listed for the program's own architecture. This writes a copy of the SDK in
which every arm64e/arm64e.x1 target in a .tbd also has an arm64 twin, and arm64e.x1 (unknown to lld) is dropped.
Everything else is hard-linked (or copied) unchanged, symlinks stay symlinks.

    patch_sdk.py ensure <iPhoneOS.sdk> [--cache DIR]   prints the SDK to use (the copy, or the SDK itself if no
                                                       .tbd needs a change); a second call reuses the copy.

The copy holds Apple's SDK and stays on this machine (cache default ~/.cache/lgp-recomp/ios-sdk, or
LGP_IOS_SDK_CACHE); never hand it on.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import sys
from pathlib import Path

VERSION = 1  # bump when the rewrite changes, so old copies are rebuilt
_TARGET = re.compile(r"^arm64e(\.x\d+)?(-.*)?$")
_YAML_LIST = re.compile(r"((?:targets|archs):\s*\[)([^\]]*)(\])")
_YAML_TARGET = re.compile(r"(target:\s*)arm64e\.x\d+(-)")


def _expand(token: str) -> list[str]:
    """arm64e-ios → arm64e-ios, arm64-ios; arm64e.x1-ios → arm64-ios (lld does not know .x1)."""
    m = _TARGET.match(token)
    if not m:
        return [token]
    twin = "arm64" + (m.group(2) or "")
    return [twin] if m.group(1) else [token, twin]


def _unique(items: list) -> list:
    seen, out = set(), []
    for item in items:
        key = json.dumps(item, sort_keys=True)
        if key not in seen:
            seen.add(key)
            out.append(item)
    return out


def _yaml_list(m: re.Match) -> str:
    raw = [t.strip() for t in m.group(2).split(",") if t.strip()]
    tokens = _unique([t for raw_token in raw for t in _expand(raw_token)])
    if tokens == raw:
        return m.group(0)
    return f"{m.group(1)} {', '.join(tokens)} {m.group(3)}"


def _json_walk(node, key: str | None = None):
    if isinstance(node, dict):
        return {k: _json_walk(v, k) for k, v in node.items()}
    if isinstance(node, list):
        if key == "targets" and all(isinstance(t, str) for t in node):
            return _unique([t for token in node for t in _expand(token)])
        items, seen = [], set()
        for item in node:
            if isinstance(item, dict) and isinstance(item.get("target"), str):  # target_info, uuids
                for t in _expand(item["target"]):
                    if t not in seen:  # one entry per target (min_deployment of the first)
                        seen.add(t)
                        items.append(_json_walk(dict(item, target=t)))
            else:
                items.append(_json_walk(item))
        return items
    return node


def rewrite_tbd(text: str) -> str:
    """The .tbd with arm64 twins (YAML tbd v3/v4 or JSON tbd v5); unchanged text if there is nothing to do."""
    if "arm64e" not in text:
        return text
    if text.lstrip().startswith("{"):
        try:
            data = json.loads(text)
        except ValueError:
            return text
        new = _json_walk(data)
        return text if new == data else json.dumps(new, indent=2) + "\n"
    out = _YAML_LIST.sub(_yaml_list, text)
    return _YAML_TARGET.sub(r"\1arm64\2", out)


def _tbds(sdk: Path):
    for root, dirs, files in os.walk(sdk):
        for name in files:
            if name.endswith(".tbd"):
                path = Path(root) / name
                if not path.is_symlink():
                    yield path


def _key(sdk: Path) -> str:
    settings = next((sdk / n for n in ("SDKSettings.json", "SDKSettings.plist") if (sdk / n).is_file()), sdk)
    st = settings.stat()
    raw = f"{VERSION}|{sdk.resolve()}|{st.st_size}|{st.st_mtime_ns}"
    return hashlib.sha256(raw.encode()).hexdigest()[:16]


def _place(src: Path, dst: Path) -> None:
    try:
        os.link(src, dst)
    except OSError:
        shutil.copy2(src, dst)


def ensure(sdk: Path, cache: Path) -> Path:
    sdk, cache = sdk.resolve(), cache.resolve()
    out = cache / f"{sdk.name.removesuffix('.sdk')}-{_key(sdk)}" / sdk.name
    stamp = out.parent / "stamp.json"
    if stamp.is_file():
        state = json.loads(stamp.read_text())
        return out if state.get("patched") else sdk
    changes = {p: new for p in _tbds(sdk) if (new := rewrite_tbd(p.read_text(errors="surrogateescape")))
               != p.read_text(errors="surrogateescape")}
    out.parent.mkdir(parents=True, exist_ok=True)
    if not changes:
        stamp.write_text(json.dumps({"patched": False, "sdk": str(sdk)}))
        return sdk
    part = out.parent / (sdk.name + ".part")
    shutil.rmtree(part, ignore_errors=True)
    for root, dirs, files in os.walk(sdk):
        rel = Path(root).relative_to(sdk)
        (part / rel).mkdir(parents=True, exist_ok=True)
        for name in dirs + files:
            src, dst = Path(root) / name, part / rel / name
            if src.is_symlink():
                os.symlink(os.readlink(src), dst)
            elif name in files:
                if src in changes:
                    dst.write_text(changes[src], errors="surrogateescape")
                else:
                    _place(src, dst)
        dirs[:] = [d for d in dirs if not (Path(root) / d).is_symlink()]
    shutil.rmtree(out, ignore_errors=True)
    os.replace(part, out)
    stamp.write_text(json.dumps({"patched": True, "sdk": str(sdk), "tbd_changed": len(changes)}))
    print(f"patch_sdk: {len(changes)} .tbd files given arm64 → {out}", file=sys.stderr)
    return out


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    e = sub.add_parser("ensure", help="print the SDK to use, patching a copy if needed")
    e.add_argument("sdk", type=Path)
    e.add_argument("--cache", type=Path, default=Path(os.environ.get("LGP_IOS_SDK_CACHE") or
                                                      Path.home() / ".cache" / "lgp-recomp" / "ios-sdk"))
    args = ap.parse_args(argv)
    if not args.sdk.is_dir():
        print(f"patch_sdk: kein SDK-Ordner: {args.sdk}", file=sys.stderr)
        return 1
    print(ensure(args.sdk, args.cache))
    return 0


if __name__ == "__main__":
    sys.exit(main())
