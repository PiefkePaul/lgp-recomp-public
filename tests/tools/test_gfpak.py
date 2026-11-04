"""Unit tests for tools/analyze/gfpak.py on a synthetic archive (no game data).

Run: python -m unittest -v test_gfpak (from tests/tools)
"""
import contextlib
import io
import os
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "tools", "analyze"))
import gfpak  # noqa: E402
from test_gfx_inventory import make_gfpak  # noqa: E402


def with_hashes(pak, hashes):
    """Append a hash table to a make_gfpak archive and point the header at it."""
    out = bytearray(pak)
    struct.pack_into("<Q", out, 0x20, len(out))
    out += struct.pack(f"<{len(hashes)}Q", *hashes)
    return bytes(out)


class GfpakTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        pak = make_gfpak([(2, b"SESD config to_capture01_fall"), (0, b"BNTX raw"), (7, b"oodle?")])
        self.path = os.path.join(self.tmp.name, "a.gfpak")
        with open(self.path, "wb") as f:
            f.write(with_hashes(pak, [0x1111, 0x2222, 0x3333]))

    def tearDown(self):
        self.tmp.cleanup()

    def run_main(self, *argv):
        out = io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(io.StringIO()):
            code = gfpak.main(list(argv))
        return code, out.getvalue()

    def test_list(self):
        code, out = self.run_main("list", self.path)
        self.assertEqual(code, 0)
        self.assertIn("hash 0000000000001111", out)
        self.assertIn("bseq", out)
        self.assertIn("compression 7 (not unpacked)", out)
        self.assertIn("3 files", out)
        code, out = self.run_main("list", self.path, "--grep", "to_capture01")
        self.assertIn("1 files", out)

    def test_extract(self):
        out_dir = os.path.join(self.tmp.name, "out")
        code, out = self.run_main("extract", self.path, "--out", out_dir)
        self.assertEqual(code, 0)
        self.assertIn("wrote 2 files, 1 with unknown compression left out", out)
        self.assertEqual(sorted(os.listdir(out_dir)), ["00000.bseq", "00001.bntx"])
        with open(os.path.join(out_dir, "00000.bseq"), "rb") as f:
            self.assertEqual(f.read(), b"SESD config to_capture01_fall")
        out2 = os.path.join(self.tmp.name, "out2")
        self.run_main("extract", self.path, "--out", out2, "--index", "1")
        self.assertEqual(os.listdir(out2), ["00001.bntx"])

    def test_not_a_pack(self):
        bad = os.path.join(self.tmp.name, "bad")
        with open(bad, "wb") as f:
            f.write(b"NOTAPACK" + b"\0" * 64)
        self.assertEqual(self.run_main("list", bad)[0], 1)


if __name__ == "__main__":
    unittest.main()
