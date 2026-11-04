"""Unit tests for tools/kit/derive.py (data mod files from the player's RomFS) on a synthetic RomFS image.

Run: python -m unittest -v test_kit_derive (from tests/tools)
"""
import contextlib
import hashlib
import io
import json
import os
import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "tools", "kit"))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import derive  # noqa: E402
from test_version_diff import make_nso  # noqa: E402
sesd = derive.sesd

# Made-up lengths, not the game's table.
LENGTHS = {0x4E: 16, 0x6A: 8, 0x31: 0}
NONE = 0xFFFFFFFF


def romfs_image(files):
    """A plain RomFS image (level 3) of {"a/b.bin": bytes}; hash tables empty, the reader walks the tree."""
    tree = {}
    for path, data in files.items():
        node = tree
        *dirs, name = path.split("/")
        for d in dirs:
            node = node.setdefault(d, {})
        node[name] = data
    dir_meta, file_meta, blob = bytearray(), bytearray(), bytearray()

    def add_dir(node, name, parent):
        at = len(dir_meta)
        dir_meta.extend(struct.pack("<IIIIII", parent, NONE, NONE, NONE, NONE, len(name.encode())))
        dir_meta.extend(name.encode().ljust((len(name.encode()) + 3) & ~3, b"\0"))
        prev_file = None
        for key, value in node.items():
            if isinstance(value, bytes):
                f_at = len(file_meta)
                file_meta.extend(struct.pack("<IIQQII", at, NONE, len(blob), len(value), NONE, len(key.encode())))
                file_meta.extend(key.encode().ljust((len(key.encode()) + 3) & ~3, b"\0"))
                blob.extend(value)
                blob.extend(b"\0" * (-len(blob) % 16))
                if prev_file is None:
                    struct.pack_into("<I", dir_meta, at + 0x0C, f_at)
                else:
                    struct.pack_into("<I", file_meta, prev_file + 4, f_at)
                prev_file = f_at
        prev_dir = None
        for key, value in node.items():
            if isinstance(value, dict):
                d_at = add_dir(value, key, at)
                if prev_dir is None:
                    struct.pack_into("<I", dir_meta, at + 8, d_at)
                else:
                    struct.pack_into("<I", dir_meta, prev_dir + 4, d_at)
                prev_dir = d_at
        return at

    add_dir(tree, "", 0)
    dir_off = 0x50
    file_off = dir_off + len(dir_meta)
    data_off = (file_off + len(file_meta) + 15) & ~15
    head = struct.pack("<QQQQQQQQQQ", 0x50, dir_off, 0, dir_off, len(dir_meta), file_off, 0, file_off,
                       len(file_meta), data_off)
    image = bytearray(head + dir_meta + file_meta)
    image.extend(b"\0" * (data_off - len(image)))
    return bytes(image + blob)


def gfpak(files):
    """An uncompressed GFLXPACK with [(path hash, bytes)]."""
    count = len(files)
    table = 0x30
    hashes = table + 0x18 * count
    data_at = hashes + 8 * count
    head = b"GFLXPACK" + struct.pack("<IIIIQQQ", 0x1000, 0, count, 0, table, hashes, 0)
    entries, blob = bytearray(), bytearray()
    for _h, payload in files:
        entries.extend(struct.pack("<HHIIIQ", 0, 0, len(payload), len(payload), 0, data_at + len(blob)))
        blob.extend(payload)
    return head + bytes(entries) + b"".join(struct.pack("<Q", h) for h, _ in files) + bytes(blob)


def sequence():
    records = [
        {"start": 0, "end": 10, "group": 7, "channel": 0, "flags": 1, "cmd": 0x4E,
         "payload": struct.pack("<IIII", 1, 0, 2, 4)},
        {"start": 5, "end": 5, "group": 7, "channel": 0, "flags": 0, "cmd": 0x6A, "payload": b"fall\0\0\0\0"},
        {"start": 20, "end": 20, "group": 1, "channel": 2, "flags": 0, "cmd": 0x31, "payload": b""},
    ]
    return sesd.serialize(30, records, LENGTHS)


class DeriveTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        self.seq = sequence()
        self.files = {"bin/battle/seq/ee620.bseq": self.seq, "bin/a.txt": b"hello", "top.bin": b"\1\2\3",
                      "bin/pack.gfpak": gfpak([(0x1111, b"first"), (0xABCDEF0123456789, b"second")]),
                      "bin/item/pocket.dat": bytes([0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1])}
        self.image = self.dir / "romfs.bin"
        self.image.write_bytes(romfs_image(self.files))
        self.mod = self.dir / "mods" / "wild-battles"
        self.mod.mkdir(parents=True)
        (self.mod / "lengths.json").write_text(json.dumps({f"{k:#x}": v for k, v in LENGTHS.items()}))

    def tearDown(self):
        self.tmp.cleanup()

    def recipe(self, files):
        (self.mod / "derive.json").write_text(json.dumps({"lengths": "lengths.json", "files": files}))

    def test_image_lists_every_file(self):
        image = derive.open_romfs(self.image)
        self.assertEqual(set(image.files), set(self.files))
        for path, data in self.files.items():
            self.assertEqual(image.read(path), data)
        with self.assertRaises(derive.DeriveError):
            image.read("bin/missing.bin")

    def test_sesd_and_copy(self):
        spec = {"frames": 25, "records": [{"from": "romfs:bin/battle/seq/ee620.bseq", "index": 1, "start": 2},
                                          {"from": "romfs:bin/battle/seq/ee620.bseq", "index": 2}]}
        expected = sesd.build(spec, str(self.mod), LENGTHS, derive.open_romfs(self.image).read)
        self.recipe([{"out": "romfs/bin/battle/seq/ex620.bseq", "sesd": spec,
                      "sha256": hashlib.sha256(expected).hexdigest()},
                     {"out": "romfs/bin/copy.txt", "copy": "bin/a.txt"}])
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(derive.main(["all", "--romfs", str(self.image), "--mods", str(self.dir / "mods")]), 0)
        out = (self.mod / "romfs/bin/battle/seq/ex620.bseq").read_bytes()
        self.assertEqual(out, expected)
        version, frames, records = sesd.parse(out, LENGTHS)
        self.assertEqual((frames, len(records), records[0]["start"], records[0]["cmd"]), (25, 2, 2, 0x6A))
        self.assertEqual((self.mod / "romfs/bin/copy.txt").read_bytes(), b"hello")

    def test_gfpak_entry_and_battle_pocket(self):
        self.recipe([{"out": "romfs/a.gfbanm", "copy": "bin/pack.gfpak", "entry": 1},
                     {"out": "romfs/b.gfbanm", "copy": "bin/pack.gfpak", "entry": "0x1111"},
                     {"out": "romfs/bin/item/pocket.dat",
                      "battle_pocket": {"from": "bin/item/pocket.dat", "items": [1, 2, 3, 4, 12]}}])
        derive.apply(derive.open_romfs(self.image), self.mod)
        self.assertEqual((self.mod / "romfs/a.gfbanm").read_bytes(), b"second")
        self.assertEqual((self.mod / "romfs/b.gfbanm").read_bytes(), b"first")
        pocket = (self.mod / "romfs/bin/item/pocket.dat").read_bytes()
        self.assertEqual([pocket[i] for i in (0, 1, 2, 3, 4, 12, 13, 17)], [0, 2, 2, 2, 2, 2, 1, 1])
        image = derive.open_romfs(self.image)
        for files in ([{"out": "romfs/x", "copy": "bin/pack.gfpak", "entry": 2}],        # no such index
                      [{"out": "romfs/x", "copy": "bin/pack.gfpak", "entry": "0x2222"}],  # no such hash
                      [{"out": "romfs/x", "copy": "top.bin", "entry": 0}],                # not an archive
                      [{"out": "romfs/x", "battle_pocket": {"from": "bin/item/pocket.dat", "items": [13]}}],
                      [{"out": "romfs/x", "battle_pocket": {"from": "bin/item/pocket.dat", "items": [99]}}],
                      [{"out": "romfs/x", "battle_pocket": {"items": [1]}}]):
            self.recipe(files)
            with self.assertRaises(derive.DeriveError, msg=files):
                derive.apply(image, self.mod)
        self.assertFalse((self.mod / "romfs/x").exists())

    def test_sesd_patch(self):
        read = derive.open_romfs(self.image).read
        spec = {"frames": 30, "records": [{"from": "romfs:bin/battle/seq/ee620.bseq", "index": 0,
                                           "patch": {"0x4": "u32:9", "12": "hex:ff"}}]}
        records = sesd.parse(sesd.build(spec, str(self.mod), LENGTHS, read), LENGTHS)[2]
        self.assertEqual(records[0]["payload"], struct.pack("<IIIB", 1, 9, 2, 0xFF) + b"\0\0\0")
        for patch in ({"14": "u32:1"}, {"x": "u8:1"}, {"0": 5}, {"0": "q8:1"}, ["0"]):
            spec["records"][0]["patch"] = patch
            with self.assertRaises(sesd.SesdError, msg=patch):
                sesd.build(spec, str(self.mod), LENGTHS, read)

    def lengths_table(self):
        """main with the lengths as u16 at +4 of 8-byte entries for commands 0x31..0x6a; returns its offset."""
        table = bytearray()
        for cmd in range(0x31, 0x6B):
            table += struct.pack("<IHH", 0xDEAD, LENGTHS.get(cmd, 0), 7)
        marker = b"LENGTHS!"
        nso = make_nso([struct.pack("<I", 0xD65F03C0)], b"\x11" * 0x20, marker + bytes(table))
        exefs = self.dir / "exefs"
        exefs.mkdir(exist_ok=True)
        (exefs / "main").write_bytes(nso)
        image = derive.ni.load_module(str(exefs / "main"), "main").image
        offset = image.index(marker) + len(marker)
        return exefs, offset, hashlib.sha256(bytes(table)).hexdigest()

    def test_lengths_from_the_exefs(self):
        exefs, offset, table_sha = self.lengths_table()
        spec = {"frames": 25, "records": [{"from": "romfs:bin/battle/seq/ee620.bseq", "index": 1},
                                          {"from": "romfs:bin/battle/seq/ee620.bseq", "index": 0,
                                           "patch": {"0": "u32:3"}}]}
        expected = sesd.build(spec, str(self.mod), LENGTHS, derive.open_romfs(self.image).read)
        table = {"module": "main", "offset": hex(offset), "count": 0x3A, "first": "0x31", "width": 2,
                 "stride": 8, "field": 4, "sha256": table_sha}
        (self.mod / "derive.json").write_text(json.dumps({"lengths": table, "files": [
            {"out": "romfs/ex620.bseq", "sesd": spec, "sha256": hashlib.sha256(expected).hexdigest()}]}))
        image = derive.open_romfs(self.image)
        derive.apply(image, self.mod, exefs=exefs)
        self.assertEqual((self.mod / "romfs/ex620.bseq").read_bytes(), expected)
        with self.assertRaises(derive.DeriveError):  # no ExeFS
            derive.apply(image, self.mod)
        for change in ({"sha256": "00" * 32}, {"offset": hex(offset + 2)}, {"count": 10 ** 7}, {"width": 3},
                       {"module": "../main"}, {"module": "sdk"}, {"offset": None}):
            (self.mod / "derive.json").write_text(json.dumps({"lengths": {**table, **change}, "files": [
                {"out": "romfs/ex620.bseq", "sesd": spec}]}))
            with self.assertRaises(derive.DeriveError, msg=change):
                derive.apply(image, self.mod, exefs=exefs)
        # build.py passes the game folder's ExeFS.
        import build
        (self.mod / "derive.json").write_text(json.dumps({"lengths": table, "files": [
            {"out": "romfs/ex620.bseq", "sesd": spec}]}))
        with contextlib.redirect_stdout(io.StringIO()):
            build.derive_mod_files(self.dir, self.image)
        self.assertEqual((self.mod / "romfs/ex620.bseq").read_bytes(), expected)

    def test_check_for_publishing(self):
        good = {"lengths": {"module": "main", "offset": "0x10", "count": 2}, "files": [
            {"out": "romfs/a.bseq", "sha256": "ab" * 32, "sesd": {"frames": 9, "records": [
                {"from": "romfs:x.bseq", "index": 0, "patch": {"0": "f32:1.5", "4": "hex:0000"}},
                {"cmd": "0x6a", "fields": ["text:fall", "hex:00000000"]}]}},
            {"out": "romfs/b.dat", "sha256": "cd" * 32, "copy": "b.dat"}]}
        recipe = self.mod / "derive.json"
        recipe.write_text(json.dumps(good))
        self.assertEqual(derive.check(recipe), [])
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(derive.main(["check", str(recipe)]), 0)
        bad = json.loads(json.dumps(good))
        bad["lengths"] = "lengths.json"
        records = bad["files"][0]["sesd"]["records"]
        records[0]["patch"]["8"] = "hex:00ff"
        records[1]["fields"].append("hex:12")
        records.append({"cmd": "0x31", "payload": ""})
        records.append({"from": "local.bseq", "index": 0})
        del bad["files"][1]["sha256"]
        recipe.write_text(json.dumps(bad))
        self.assertEqual(len(derive.check(recipe)), 6)
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(derive.main(["check", str(recipe)]), 1)

    def test_unpacked_folder(self):
        folder = self.dir / "romfs"
        for path, data in self.files.items():
            (folder / path).parent.mkdir(parents=True, exist_ok=True)
            (folder / path).write_bytes(data)
        self.recipe([{"out": "romfs/x.bin", "copy": "top.bin"}])
        derive.apply(derive.open_romfs(folder), self.mod)
        self.assertEqual((self.mod / "romfs/x.bin").read_bytes(), b"\1\2\3")

    def test_refusals(self):
        image = derive.open_romfs(self.image)
        for files in ([{"out": "../x.bin", "copy": "top.bin"}],              # outside the mod
                      [{"out": "cfg/x.bin", "copy": "top.bin"}],             # not below romfs/
                      [{"out": "romfs/x.bin", "copy": "../top.bin"}],        # outside the RomFS
                      [{"out": "romfs/x.bin"}],                              # nothing to do
                      [{"out": "romfs/x.bin", "copy": "top.bin", "sha256": "00" * 32}]):  # another dump
            self.recipe(files)
            with self.assertRaises(derive.DeriveError, msg=files):
                derive.apply(image, self.mod)
        self.assertFalse((self.mod / "romfs/x.bin").exists())
        with self.assertRaises(derive.DeriveError):  # the compressed romfs.bin of a game folder
            bad = self.dir / "packed.bin"
            bad.write_bytes(b"LGPZ" + bytes(0x60))
            derive.open_romfs(bad)

    def test_build_stage_step(self):
        import build  # tools/kit/build.py, staging the game folder
        self.recipe([{"out": "romfs/x.bin", "copy": "top.bin"}])
        with contextlib.redirect_stdout(io.StringIO()):
            build.derive_mod_files(self.dir, self.image)
        self.assertEqual((self.mod / "romfs/x.bin").read_bytes(), b"\1\2\3")
        self.recipe([{"out": "romfs/x.bin", "copy": "missing.bin"}])
        with contextlib.redirect_stdout(io.StringIO()), self.assertRaises(build.KitError):
            build.derive_mod_files(self.dir, self.image)

    def test_sesd_cli_romfs(self):
        spec = self.dir / "spec.json"
        spec.write_text(json.dumps({"frames": 30, "records": [{"from": "romfs:bin/battle/seq/ee620.bseq",
                                                               "index": 0}]}))
        out = self.dir / "out.bseq"
        args = ["build", str(spec), "--lengths", str(self.mod / "lengths.json"), "--out", str(out)]
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(sesd.main(args), 1)  # romfs: without --romfs
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(sesd.main(args + ["--romfs", str(self.image)]), 0)
        self.assertEqual(len(sesd.parse(out.read_bytes(), LENGTHS)[2]), 1)


if __name__ == "__main__":
    unittest.main()
