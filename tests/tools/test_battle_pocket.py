"""Unit tests for tools/items/battle_pocket.py on a synthetic file (no game data).

Run: python -m unittest -v test_battle_pocket (from tests/tools)
"""
import contextlib
import io
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "tools", "items"))
import battle_pocket as bp  # noqa: E402


def synthetic(size=64, potion_page=2):
    data = bytearray(size)
    data[17] = potion_page  # Potion
    data[30] = 3  # some battle item
    return bytes(data)


class BattlePocketTest(unittest.TestCase):
    def run_main(self, *argv):
        out = io.StringIO()
        err = io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = bp.main(list(argv))
        return code, out.getvalue(), err.getvalue()

    def test_patch_moves_balls_to_page(self):
        out, changes = bp.patch(synthetic(), bp.BALLS, 2)
        for item in bp.BALLS:
            self.assertEqual(out[item], 2)
        self.assertEqual(changes, [(i, 0) for i in bp.BALLS])
        self.assertEqual(out[30], 3)
        self.assertEqual(len(out), 64)

    def test_patch_with_offset(self):
        data = b"HDR!" + synthetic()
        out, _ = bp.patch(data, [4], 2, offset=4)
        self.assertEqual(out[:4], b"HDR!")
        self.assertEqual(out[8], 2)

    def test_patch_refuses_unexpected_page(self):
        data = bytearray(synthetic())
        data[4] = 5
        with self.assertRaises(ValueError):
            bp.patch(bytes(data), [4], 2)
        out, changes = bp.patch(bytes(data), [4], 2, force=True)
        self.assertEqual(out[4], 2)
        self.assertEqual(changes, [(4, 5)])

    def test_patch_is_idempotent(self):
        once, _ = bp.patch(synthetic(), bp.BALLS, 2)
        twice, changes = bp.patch(once, bp.BALLS, 2)
        self.assertEqual(once, twice)
        self.assertEqual(changes, [])

    def test_item_past_end(self):
        with self.assertRaises(IndexError):
            bp.patch(synthetic(size=10), [12], 2)

    def test_parse_items(self):
        self.assertEqual(bp.parse_items("1, 0x2,4"), [1, 2, 4])
        with self.assertRaises(ValueError):
            bp.parse_items(" , ")

    def test_cli_apply_default_battle_item(self):
        with tempfile.TemporaryDirectory() as d:
            src = os.path.join(d, "battle_pocket.dat")
            dst = os.path.join(d, "out.dat")
            with open(src, "wb") as f:
                f.write(synthetic(potion_page=1))
            code, out, _ = self.run_main("apply", src, "--out", dst)
            self.assertEqual(code, 0)
            self.assertIn("5 items set to mask 2", out)
            with open(dst, "rb") as f:
                new = f.read()
            self.assertEqual([new[i] for i in bp.BALLS], [2] * 5)
            self.assertEqual(new[17], 1)

    def test_cli_apply_like_potion(self):
        with tempfile.TemporaryDirectory() as d:
            src = os.path.join(d, "battle_pocket.dat")
            dst = os.path.join(d, "out.dat")
            with open(src, "wb") as f:
                f.write(synthetic(potion_page=7))
            code, out, _ = self.run_main("apply", src, "--out", dst, "--like", "17")
            self.assertEqual(code, 0)
            self.assertIn("5 items set to mask 7", out)
            with open(dst, "rb") as f:
                new = f.read()
            self.assertEqual([new[i] for i in bp.BALLS], [7] * 5)

            code, out, _ = self.run_main("show", dst, "--items", "4,17")
            self.assertEqual(out.splitlines(), ["item 4: mask 7", "item 17: mask 7"])
            code, out, _ = self.run_main("check", src)
            self.assertIn("64 bytes, 62 zero, 2 shown", out)

    def test_cli_reference_not_shown(self):
        with tempfile.TemporaryDirectory() as d:
            src = os.path.join(d, "battle_pocket.dat")
            with open(src, "wb") as f:
                f.write(synthetic(potion_page=0))
            code, _, err = self.run_main("apply", src, "--out", os.path.join(d, "o.dat"), "--like", "17")
            self.assertEqual(code, 1)
            self.assertIn("does not show", err)
            self.assertFalse(os.path.exists(os.path.join(d, "o.dat")))


    def test_negative_offset(self):
        with self.assertRaises(IndexError):
            bp.patch(bytes(32), [1], 2, offset=-2)

if __name__ == "__main__":
    unittest.main()
