"""nso_inventory.py against the real homebrew fixtures from tests/fixtures/ (P0.7/P1.2).

The fixtures are built by devkitA64 + libnx (own code only). Their location comes
from LGP_FIXTURE_DIR, which tests/CMakeLists.txt sets when LGP_BUILD_FIXTURES is ON.
binutils' readelf output of the same ELF (basic.readelf.txt) serves as the
independent reference for relocations and the build ID.

Run: LGP_FIXTURE_DIR=<dir> python -m unittest -v test_fixture_inventory
"""
import json
import os
import re
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(HERE, "..", "..", "tools", "analyze", "nso_inventory.py")
sys.path.insert(0, os.path.dirname(TOOL))
import nso_inventory as inv  # noqa: E402

FIXTURE_DIR = os.environ.get("LGP_FIXTURE_DIR", "")


def readelf_reference(path: str) -> dict:
    with open(path, encoding="utf-8") as f:
        text = f.read()
    relr = re.search(r"'\.relr\.dyn' at offset \S+ contains \d+ entries which relocate (\d+) locations", text)
    rela = re.search(r"'\.rela\.dyn' at offset \S+ contains (\d+) entr", text)
    build_id = re.search(r"Build ID: ([0-9a-f]+)", text)
    return {
        "relr": int(relr.group(1)) if relr else 0,
        "rela": int(rela.group(1)) if rela else 0,
        "build_id": build_id.group(1) if build_id else None,
    }


# Skipped only when LGP_FIXTURE_DIR is unset; a set but missing dir is a failure.
@unittest.skipUnless(FIXTURE_DIR, "LGP_FIXTURE_DIR not set; fixtures need devkitA64")
class FixtureInventoryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.ref = readelf_reference(os.path.join(FIXTURE_DIR, "basic.readelf.txt"))
        cls.nro = inv.load_module(os.path.join(FIXTURE_DIR, "basic.nro"))
        cls.nso = inv.load_module(os.path.join(FIXTURE_DIR, "basic.nso"))

    def test_fixtures_are_small(self):
        # Acceptance P0.7: the fixture stays small (libnx crt0 + newlib + own code).
        for name in ("basic.nro", "basic.nso"):
            self.assertLess(os.path.getsize(os.path.join(FIXTURE_DIR, name)), 512 * 1024, name)

    def test_build_id_matches_elf_note(self):
        self.assertIsNotNone(self.ref["build_id"])
        for mod in (self.nro, self.nso):
            self.assertEqual(inv.module_summary(mod)["module_id"], self.ref["build_id"], mod.name)

    def test_relocations_match_readelf(self):
        self.assertGreater(self.ref["relr"] + self.ref["rela"], 0, "fixture should need relocations")
        for mod in (self.nro, self.nso):
            counts = dict(mod.reloc_counts)
            self.assertEqual(counts.pop("RELR", 0), self.ref["relr"], mod.name)
            self.assertEqual(sum(counts.values()), self.ref["rela"], mod.name)

    def test_nso_hashes_and_same_layout_as_nro(self):
        self.assertTrue(self.nso.hashes_ok)
        self.assertTrue(any(s.compressed for s in self.nso.segments), "elf2nso compresses segments")
        for seg in ("text", "ro", "data"):
            self.assertEqual(self.nso.seg(seg).mem_offset, self.nro.seg(seg).mem_offset, seg)
            # The NRO pads segments to pages, the NSO keeps the exact sizes.
            self.assertLessEqual(self.nso.seg(seg).mem_size, self.nro.seg(seg).mem_size, seg)
        self.assertEqual(self.nso.ro_path_basename, "basic")

    def test_static_homebrew_has_no_imports(self):
        # libnx homebrew is linked statically: nothing to resolve at load time.
        for mod in (self.nro, self.nso):
            summary = inv.module_summary(mod)
            self.assertEqual(summary["import_count"], 0, mod.name)
            self.assertEqual(summary["needed_count"], 0, mod.name)

    def test_cli_writes_json_and_imports(self):
        with tempfile.TemporaryDirectory() as d:
            out_json, out_tsv = os.path.join(d, "inv.json"), os.path.join(d, "imports.tsv")
            proc = subprocess.run(
                [sys.executable, TOOL, os.path.join(FIXTURE_DIR, "basic.nro"), "--json", out_json, "--imports", out_tsv],
                capture_output=True, text=True)
            self.assertEqual(proc.returncode, 0, proc.stderr)
            self.assertIn("basic.nro", proc.stdout)
            with open(out_json, encoding="utf-8") as f:
                data = json.load(f)
            self.assertEqual(data["modules"][0]["reloc_counts"].get("RELR"), self.ref["relr"])
            with open(out_tsv, encoding="utf-8") as f:
                self.assertEqual(f.read().splitlines(), ["importer\tsymbol\tbind\ttype\tprovider"])


if __name__ == "__main__":
    unittest.main()
