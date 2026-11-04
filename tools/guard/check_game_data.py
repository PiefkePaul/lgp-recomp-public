#!/usr/bin/env python3
"""Pre-commit guard: refuse to commit Switch game data, keys or derived binaries.

Checks every candidate file for
  1. forbidden file extensions (NSP/XCI/NCA/NSO/keys/...),
  2. a size limit,
  3. container magic bytes (NCA3/NCA2, NSO0, PFS0, HFS0, XCI HEAD, ...),
  4. key-file content (prod.keys / title.keys style "name = <hex>" lines).

Self-built homebrew fixtures below tests/fixtures/ may be .nso/.nro with
NSO0/NRO0 magic. NCA/PFS0/HFS0/XCI magic and key material are rejected
everywhere, and the size limit applies everywhere.

Modes:
  --staged   check the git index (used by .githooks/pre-commit)
  --all      check every tracked file in the working tree (used by CI)
  PATH ...   check the given files on disk

Exit code 0 = clean, 1 = violations found, 2 = usage/internal error.
Uses only the Python standard library so it runs unchanged on Windows,
macOS and Linux.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import PurePosixPath

# Hard upper bound per file. Source files, docs and homebrew fixtures stay far
# below this; game containers and assets are megabytes to gigabytes.
MAX_FILE_BYTES = 2 * 1024 * 1024

FORBIDDEN_EXTENSIONS = {
    # containers / packages
    ".nsp", ".nsz", ".xci", ".xcz", ".nca", ".ncz", ".pfs0", ".hfs0",
    # executables / metadata
    ".nso", ".nro", ".npdm", ".nacp", ".kip", ".kip1",
    # tickets / keys
    ".tik", ".cert", ".keys", ".key",
    # saves
    ".sav",
    # common game asset formats (RomFS)
    ".bfres", ".bntx", ".bfsha", ".bnsh", ".gfpak", ".gfbmdl", ".bfstm",
    ".bfwav", ".bars", ".sarc", ".szs", ".bflyt", ".bflim", ".bfttf",
    # derived from game data: draw pipeline recipes and shader hashes
    # (gpu/webgpu pipeline cache, lgp/gpu/webgpu/pipeline_cache.hpp)
    ".lgppipes",
    # derived from game shaders: Metal source dumps and offline-compiled
    # libraries (tools/apple/build_metallibs.sh)
    ".metal", ".metallib",
}

FORBIDDEN_FILENAMES = {"prod.keys", "title.keys", "dev.keys", "keys.txt"}

# Self-built homebrew fixtures (devkitA64/libnx) are allowed here.
FIXTURE_PREFIX = "tests/fixtures/"
FIXTURE_ALLOWED_EXTENSIONS = {".nso", ".nro"}
FIXTURE_ALLOWED_MAGIC = {"NSO0", "NRO0"}

# (offset, magic bytes, name). Offsets follow the public format docs
# (switchbrew): NSO/PFS0/HFS0 at 0, NRO at 0x10, XCI "HEAD" at 0x100,
# NCA header magic at 0x200 (only visible in a *decrypted* NCA header).
MAGICS: list[tuple[int, bytes, str]] = [
    (0x000, b"NSO0", "NSO0"),
    (0x000, b"PFS0", "PFS0"),
    (0x000, b"HFS0", "HFS0"),
    (0x000, b"NCA3", "NCA3"),
    (0x000, b"NCA2", "NCA2"),
    (0x010, b"NRO0", "NRO0"),
    (0x100, b"HEAD", "XCI"),
    (0x200, b"NCA3", "NCA3"),
    (0x200, b"NCA2", "NCA2"),
    (0x000, b"LGPPIPE", "LGP pipeline cache"),
    (0x000, b"MTLB", "Metal library"),
]
MAGIC_SCAN_BYTES = 0x210

# prod.keys:  "header_key = <64 hex>"      title.keys: "<32 hex> = <32 hex>"
KEY_LINE = re.compile(
    rb"^[ \t]*[0-9A-Za-z_]+[ \t]*=[ \t]*[0-9A-Fa-f]{32}(?:[0-9A-Fa-f]{32})?[ \t]*\r?$",
    re.MULTILINE,
)
KEY_LINE_THRESHOLD = 3


@dataclass
class Violation:
    path: str
    reason: str


def _suffix(path: str) -> str:
    return PurePosixPath(path).suffix.lower()


def _is_fixture(path: str) -> bool:
    return path.replace("\\", "/").startswith(FIXTURE_PREFIX)


def check_name(path: str) -> list[Violation]:
    name = PurePosixPath(path).name.lower()
    if name in FORBIDDEN_FILENAMES:
        return [Violation(path, f"forbidden file name '{name}'")]
    ext = _suffix(path)
    if ext in FORBIDDEN_EXTENSIONS:
        if _is_fixture(path) and ext in FIXTURE_ALLOWED_EXTENSIONS:
            return []
        return [Violation(path, f"forbidden extension '{ext}'")]
    return []


def check_size(path: str, size: int) -> list[Violation]:
    if size > MAX_FILE_BYTES:
        return [Violation(path, f"size {size} bytes exceeds limit {MAX_FILE_BYTES}")]
    return []


def check_content(path: str, data: bytes) -> list[Violation]:
    out: list[Violation] = []
    for offset, magic, name in MAGICS:
        if data[offset:offset + len(magic)] == magic:
            if _is_fixture(path) and name in FIXTURE_ALLOWED_MAGIC:
                continue
            out.append(Violation(path, f"{name} magic at offset 0x{offset:x}"))
    if len(KEY_LINE.findall(data)) >= KEY_LINE_THRESHOLD:
        out.append(Violation(path, "looks like a Switch key file (name = hex-key lines)"))
    return out


def _git(*args: str, input_bytes: bytes | None = None) -> bytes:
    return subprocess.run(
        ["git", *args], input=input_bytes, stdout=subprocess.PIPE, check=True
    ).stdout


def _split_z(raw: bytes) -> list[str]:
    return [p.decode("utf-8", "surrogateescape") for p in raw.split(b"\0") if p]


def staged_files() -> list[tuple[str, int, bytes]]:
    """Return (path, size, content) for every added/copied/modified staged file."""
    paths = _split_z(_git("diff", "--cached", "--name-only", "-z", "--diff-filter=ACMR"))
    if not paths:
        return []
    # Read blobs straight from the index so unstaged edits cannot mask anything.
    request = b"".join(b":" + p.encode("utf-8", "surrogateescape") + b"\n" for p in paths)
    raw = _git("cat-file", "--batch", input_bytes=request)
    result: list[tuple[str, int, bytes]] = []
    pos = 0
    for path in paths:
        eol = raw.index(b"\n", pos)
        header = raw[pos:eol].split()
        pos = eol + 1
        if len(header) < 3 or header[-1] == b"missing":
            raise RuntimeError(f"cannot read staged blob for {path!r}")
        size = int(header[2])
        result.append((path, size, raw[pos:pos + size]))
        pos += size + 1  # blob content is followed by a newline
    return result


def tracked_files() -> list[str]:
    return _split_z(_git("ls-files", "-z"))


def check_file(path: str, size: int, data: bytes) -> list[Violation]:
    return check_name(path) + check_size(path, size) + check_content(path, data)


def check_disk_file(path: str) -> list[Violation]:
    try:
        with open(path, "rb") as f:
            f.seek(0, 2)
            size = f.tell()
            f.seek(0)
            # Content checks only need small files; big ones fail on size anyway.
            data = f.read(MAX_FILE_BYTES if size <= MAX_FILE_BYTES else MAGIC_SCAN_BYTES)
    except OSError as e:
        return [Violation(path, f"cannot read file: {e}")]
    return check_file(path.replace("\\", "/"), size, data)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--staged", action="store_true", help="check the git index")
    mode.add_argument("--all", action="store_true", help="check all tracked files")
    parser.add_argument("paths", nargs="*", help="files to check")
    args = parser.parse_args(argv)

    violations: list[Violation] = []
    try:
        if args.staged:
            for path, size, data in staged_files():
                violations += check_file(path, size, data)
        elif args.all:
            for path in tracked_files():
                violations += check_disk_file(path)
        elif args.paths:
            for path in args.paths:
                violations += check_disk_file(path)
        else:
            parser.error("give --staged, --all or file paths")
    except (subprocess.CalledProcessError, RuntimeError, ValueError) as e:
        print(f"game-data guard: internal error: {e}", file=sys.stderr)
        return 2

    if violations:
        print("game-data guard: REJECTED. These files look like game data or keys:", file=sys.stderr)
        for v in violations:
            print(f"  {v.path}: {v.reason}", file=sys.stderr)
        print(
            "Game data belongs in $LGP_PRIVATE_DIR, never in the repo "
            "(see docs/project-setup/02-projekt-anweisungen.md). Do not bypass with --no-verify.",
            file=sys.stderr,
        )
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
