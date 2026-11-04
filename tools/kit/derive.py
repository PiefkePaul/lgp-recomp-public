#!/usr/bin/env python3
"""Builds a data mod's changed game files from the player's own RomFS, by a recipe from the kit.

Data mods (`<game>/mods/<id>/romfs/`) may need files that are game files changed by us, such as the K.O.
sequence of wild-battles (an ee620 cut down, docs/analysis/55-fangen-im-kampf.md Fang-36). Those must not be
handed out. The kit hands out a recipe instead, `mods/<id>/derive.json`, and this tool (or `build.py` while it
stages the game folder) writes the files from the player's own dump:

    {"lengths": {"module": "main", "offset": "0x…", "count": 200},  # payload lengths of the SESD commands
                                                # (tools/battle/sesd.py), read from the player's ExeFS
     "files": [
       {"out": "romfs/bin/battle/seq/ex620.bseq",
        "sesd": {"frames": 160, "records": [{"from": "romfs:bin/battle/seq/ee620.bseq", "index": 0}, ...]},
        "sha256": "…"},                         # optional: the file as tested; another result is refused
       {"out": "romfs/bin/x/clip.gfbanm", "copy": "bin/y/clip.gfbanm"},
       {"out": "romfs/bin/x/clip2.gfbanm", "copy": "bin/y/pack.gfpak", "entry": 3},   # file 3 of the archive
       {"out": "romfs/bin/pokelib/item/battle_pocket.dat",
        "battle_pocket": {"from": "bin/pokelib/item/battle_pocket.dat", "items": [1, 2, 3, 4, 12], "value": 2}}
     ]}

"lengths" names a table in a module of the player's ExeFS: "offset" in the module's memory image (main+OFF),
"count" entries for the commands "first" (default 0) and up, each "width" bytes (1, 2 or 4, default 4) every
"stride" bytes (default width) at "field" within the entry (default 0); an optional "sha256" of the table's
count × stride bytes refuses another game version. A file name (JSON as for sesd.py --lengths) works for private recipes.

"out" is relative to the mod folder and stays inside it; "sesd" is a tools/battle/sesd.py build spec (or the
name of a JSON file next to the recipe) whose "from": "romfs:PATH" records come from the RomFS; "copy" takes a
RomFS file as it is, or with "entry" one file of a .gfpak archive (its index as tools/analyze/gfpak.py lists
it, or its path hash as a hex string); "battle_pocket" sets item masks like tools/items/battle_pocket.py apply
("offset" and "force" as there). The RomFS is romfs.bin as the kit page writes it (a plain RomFS image, not the compressed
one of the game folder) or an unpacked folder. Inputs and outputs are game data and stay on this computer.

    derive.py apply --romfs <romfs.bin|folder> --exefs <exefs> --mod <mods/ID> [--recipe derive.json]
    derive.py all --romfs <romfs.bin|folder> --exefs <exefs> --mods <mods>
    derive.py check <derive.json…>      # may this recipe go out? (no game bytes)

`check` refuses what a published recipe must not hold: a lengths file (a table from the game code), any
"payload", "hex:" fields other than zeros, records "from" files other than romfs:, and files without sha256.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import struct
import sys
from pathlib import Path, PurePosixPath

_TOOLS = Path(__file__).resolve().parents[1]
for _sub in ("battle", "analyze", "items"):
    sys.path.insert(0, str(_TOOLS / _sub))
import nso_inventory as ni  # noqa: E402
import sesd  # noqa: E402

RECIPE = "derive.json"
NONE = 0xFFFFFFFF


class DeriveError(Exception):
    pass


class RomfsImage:
    """A plain RomFS image (level 3, header size 0x50), files by path."""

    def __init__(self, path: Path):
        self.path = path
        with open(path, "rb") as f:
            head = f.read(0x50)
            if len(head) < 0x50 or struct.unpack_from("<Q", head, 0)[0] != 0x50:
                raise DeriveError(f"{path}: not a RomFS image (the compressed romfs.bin of the game folder? "
                                  "this needs the romfs.bin of the dump)")
            dir_off, dir_size = struct.unpack_from("<QQ", head, 0x18)
            file_off, file_size = struct.unpack_from("<QQ", head, 0x38)
            self.data_off = struct.unpack_from("<Q", head, 0x48)[0]
            f.seek(dir_off)
            dirs = f.read(dir_size)
            f.seek(file_off)
            files = f.read(file_size)
        self.files: dict[str, tuple[int, int]] = {}
        stack, guard = [(0, "")], 0
        while stack:
            d, prefix = stack.pop()
            guard += 1
            if guard > 1_000_000 or d + 0x18 > len(dirs):
                raise DeriveError(f"{path}: broken RomFS directory tree")
            f_at = struct.unpack_from("<I", dirs, d + 0x0C)[0]
            while f_at != NONE:
                if f_at + 0x20 > len(files):
                    raise DeriveError(f"{path}: broken RomFS file table")
                _, sibling, off, size, _, name_len = struct.unpack_from("<IIQQII", files, f_at)
                name = files[f_at + 0x20:f_at + 0x20 + name_len].decode("utf-8")
                self.files[prefix + name] = (self.data_off + off, size)
                f_at = sibling
            sub = struct.unpack_from("<I", dirs, d + 8)[0]
            while sub != NONE:
                if sub + 0x18 > len(dirs):
                    raise DeriveError(f"{path}: broken RomFS directory tree")
                sibling = struct.unpack_from("<I", dirs, sub + 4)[0]
                name_len = struct.unpack_from("<I", dirs, sub + 0x14)[0]
                name = dirs[sub + 0x18:sub + 0x18 + name_len].decode("utf-8")
                stack.append((sub, prefix + name + "/"))
                sub = sibling

    def read(self, rel: str) -> bytes:
        entry = self.files.get(rel.lstrip("/"))
        if entry is None:
            raise DeriveError(f"{rel} is not in the RomFS")
        with open(self.path, "rb") as f:
            f.seek(entry[0])
            data = f.read(entry[1])
        if len(data) != entry[1]:
            raise DeriveError(f"{rel}: the RomFS image is too short")
        return data


class RomfsFolder:
    def __init__(self, path: Path):
        self.path = path

    def read(self, rel: str) -> bytes:
        p = self.path / safe_rel(rel)
        if not p.is_file():
            raise DeriveError(f"{rel} is not in the RomFS")
        return p.read_bytes()


def open_romfs(path) -> RomfsImage | RomfsFolder:
    path = Path(path)
    return RomfsFolder(path) if path.is_dir() else RomfsImage(path)


def safe_rel(rel: str) -> PurePosixPath:
    p = PurePosixPath(rel.lstrip("/"))
    if not p.parts or ".." in p.parts or p.is_absolute() or "\\" in rel:
        raise DeriveError(f"path {rel!r} not allowed")
    return p


def gfpak_entry(archive: bytes, entry, name: str) -> bytes:
    """One file of a .gfpak: entry is its index (int) or its path hash (hex string)."""
    import gfpak
    try:
        files = list(gfpak.entries(archive))
    except (ValueError, struct.error, gfpak.si.ni.FormatError) as e:
        raise DeriveError(f"{name}: {e}") from None
    if isinstance(entry, str):
        want = int(entry, 16)
        found = [f for f in files if f[3] == want]
    elif isinstance(entry, int) and not isinstance(entry, bool):
        found = [f for f in files if f[0] == entry]
    else:
        raise DeriveError(f"{name}: `entry` must be an index or a hex path hash")
    if not found:
        raise DeriveError(f"{name}: no file {entry} in the archive")
    if found[0][2] is None:
        raise DeriveError(f"{name}: file {entry} has an unknown compression ({found[0][1]})")
    return found[0][2]


def battle_pocket(romfs, spec: dict) -> bytes:
    import battle_pocket as bp
    try:
        data = romfs.read(str(safe_rel(spec["from"])))
        items = spec.get("items", list(bp.BALLS))
        if not isinstance(items, list) or not all(isinstance(n, int) and n >= 0 for n in items):
            raise ValueError("`items` must be a list of item numbers")
        out, _ = bp.patch(data, items, int(spec.get("value", bp.BATTLE_ITEM)), int(spec.get("offset", 0)),
                          bool(spec.get("force", False)))
    except KeyError:
        raise DeriveError("battle_pocket: no `from`") from None
    except (ValueError, IndexError, TypeError) as e:
        raise DeriveError(f"battle_pocket: {e}") from None
    return out


def module_lengths(spec: dict, exefs: Path | None) -> dict[int, int]:
    """The SESD payload lengths from a table in an ExeFS module (see the header)."""
    if exefs is None:
        raise DeriveError("the recipe reads the SESD lengths from the game's ExeFS (--exefs)")
    try:
        name = str(safe_rel(spec["module"]))
        offset, count = _int(spec["offset"]), _int(spec["count"])
        first, width = _int(spec.get("first", 0)), _int(spec.get("width", 4))
        stride, field = _int(spec.get("stride", width)), _int(spec.get("field", 0))
    except KeyError as e:
        raise DeriveError(f"lengths: no {e}") from None
    if width not in (1, 2, 4) or count <= 0 or offset < 0 or first < 0 or stride < field + width:
        raise DeriveError("lengths: bad table layout")
    try:
        image = ni.load_module(str(Path(exefs) / name), name).image
    except (OSError, ni.FormatError) as e:
        raise DeriveError(f"lengths: {e}") from None
    end = offset + count * stride
    if end > len(image):
        raise DeriveError(f"lengths: the table runs past the end of {name}")
    table = image[offset:end]
    want = spec.get("sha256")
    if want and hashlib.sha256(table).hexdigest() != want.lower():
        raise DeriveError(f"lengths: the table in {name} differs (another game version?)")
    fmt = {1: "<B", 2: "<H", 4: "<I"}[width]
    return {first + i: struct.unpack_from(fmt, table, i * stride + field)[0] for i in range(count)}


def _int(value) -> int:
    if isinstance(value, bool):
        raise DeriveError(f"not a number: {value!r}")
    try:
        return int(value, 0) if isinstance(value, str) else int(value)
    except (TypeError, ValueError):
        raise DeriveError(f"not a number: {value!r}") from None


def apply(romfs, mod: Path, recipe: Path | None = None, exefs: Path | None = None) -> list[Path]:
    """Writes the files of mod/derive.json (or `recipe`) into `mod`; returns them."""
    recipe = recipe or mod / RECIPE
    try:
        spec = json.loads(recipe.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as e:
        raise DeriveError(f"{recipe}: {e}") from None
    if not isinstance(spec.get("files"), list) or not spec["files"]:
        raise DeriveError(f"{recipe}: no `files`")
    lengths = None
    written = []
    for i, item in enumerate(spec["files"]):
        out = item.get("out")
        if not isinstance(out, str) or not safe_rel(out).parts[0] == "romfs":
            raise DeriveError(f"{recipe}: entry {i}: `out` must be below romfs/")
        if "sesd" in item:
            if lengths is None:
                if isinstance(spec.get("lengths"), dict):
                    lengths = module_lengths(spec["lengths"], exefs)
                elif isinstance(spec.get("lengths"), str):
                    lengths = sesd.load_lengths(recipe.parent / safe_rel(spec["lengths"]))
                else:
                    raise DeriveError(f"{recipe}: no `lengths`")
            seq = item["sesd"]
            if isinstance(seq, str):
                seq = json.loads((recipe.parent / safe_rel(seq)).read_text(encoding="utf-8"))
            data = sesd.build(seq, str(recipe.parent), lengths, romfs.read)
        elif isinstance(item.get("copy"), str):
            data = romfs.read(str(safe_rel(item["copy"])))
            if "entry" in item:
                data = gfpak_entry(data, item["entry"], item["copy"])
        elif isinstance(item.get("battle_pocket"), dict):
            data = battle_pocket(romfs, item["battle_pocket"])
        else:
            raise DeriveError(f"{recipe}: entry {i}: `sesd`, `copy` or `battle_pocket` needed")
        want = item.get("sha256")
        if want and hashlib.sha256(data).hexdigest() != want.lower():
            raise DeriveError(f"{out}: the result differs from the tested file (another dump or game "
                              "version?)")
        dst = mod / safe_rel(out)
        dst.parent.mkdir(parents=True, exist_ok=True)
        dst.write_bytes(data)
        written.append(dst)
    return written


def apply_all(romfs, mods: Path, exefs: Path | None = None) -> list[Path]:
    """Every mods/<id>/derive.json."""
    written = []
    for recipe in sorted(mods.glob(f"*/{RECIPE}")):
        written += apply(romfs, recipe.parent, exefs=exefs)
    return written


def check(recipe: Path) -> list[str]:
    """What keeps a recipe from going out (empty: nothing). Zero runs in hex: are fine, they hold no game data."""
    problems = []
    try:
        spec = json.loads(recipe.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as e:
        return [f"{e}"]
    if "lengths" in spec and not isinstance(spec["lengths"], dict):
        problems.append("`lengths` is a file; a published recipe reads the table from the ExeFS")

    def fields(where, values):
        for f in values:
            if isinstance(f, str) and f.startswith("hex:") and f[4:].strip("0"):
                problems.append(f"{where}: {f[:24]}… holds bytes (hex: only for zeros)")

    for i, item in enumerate(spec.get("files", [])):
        where = f"file {i} ({item.get('out')})"
        if not item.get("sha256"):
            problems.append(f"{where}: no sha256")
        seq = item.get("sesd")
        if isinstance(seq, str):
            try:
                seq = json.loads((recipe.parent / safe_rel(seq)).read_text(encoding="utf-8"))
            except (OSError, json.JSONDecodeError, DeriveError) as e:
                problems.append(f"{where}: {e}")
                continue
        for j, r in enumerate((seq or {}).get("records", [])):
            at = f"{where} record {j}"
            if "payload" in r:
                problems.append(f"{at}: `payload` (use fields, or from + patch)")
            if "from" in r and not str(r["from"]).startswith("romfs:"):
                problems.append(f"{at}: from {r['from']} is not in the RomFS")
            fields(at, r.get("fields", []))
            fields(at, (r.get("patch") or {}).values() if isinstance(r.get("patch"), dict) else [])
    return problems


def main(argv=None) -> int:
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = p.add_subparsers(dest="command", required=True)
    a = sub.add_parser("apply", help="one mod's recipe")
    a.add_argument("--romfs", required=True)
    a.add_argument("--exefs", help="the game's ExeFS, for recipes that read the SESD lengths from it")
    a.add_argument("--mod", required=True, help="the mod folder (mods/<id>), the files go below it")
    a.add_argument("--recipe", help=f"default: <mod>/{RECIPE}")
    b = sub.add_parser("all", help=f"every mods/<id>/{RECIPE}")
    b.add_argument("--romfs", required=True)
    b.add_argument("--exefs")
    b.add_argument("--mods", required=True)
    c = sub.add_parser("check", help="may these recipes go out?")
    c.add_argument("recipes", nargs="+")
    args = p.parse_args(argv)
    if args.command == "check":
        bad = 0
        for r in args.recipes:
            problems = check(Path(r))
            bad += bool(problems)
            print(f"{r}: " + ("ok" if not problems else f"{len(problems)} problems"))
            for problem in problems:
                print(f"  {problem}")
        return 1 if bad else 0
    exefs = Path(args.exefs) if args.exefs else None
    try:
        romfs = open_romfs(args.romfs)
        if args.command == "apply":
            out = apply(romfs, Path(args.mod), Path(args.recipe) if args.recipe else None, exefs)
        else:
            out = apply_all(romfs, Path(args.mods), exefs)
    except (DeriveError, sesd.SesdError, OSError, ValueError, json.JSONDecodeError) as e:
        print(f"derive: {e}", file=sys.stderr)
        return 1
    print(f"wrote {len(out)} files")
    return 0


if __name__ == "__main__":
    sys.exit(main())
