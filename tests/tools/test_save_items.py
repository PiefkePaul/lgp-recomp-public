"""Unit test for tools/save/lgp_save_items.py (synthetic savedata.bin only).

Run: python -m unittest -v test_save_items (from tests/tools)
"""
import io
import os
import struct
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout

HERE = os.path.dirname(__file__)
sys.path.insert(0, os.path.join(HERE, "..", "..", "tools", "save"))
import lgp_save_items as si  # noqa: E402
import lgp_save_party as sp  # noqa: E402


def entry(item, count, new=False):
    return item | count << si.COUNT_SHIFT | (1 << 30 if new else 0)


def make_save(raws=(entry(4, 55), entry(3, 5, new=True), entry(80, 2))):
    save = bytearray(sp.SAVE_SIZE)
    struct.pack_into("<QQ4s", save, sp.BLOCK_TABLE, 1, 2, b"BEEF")
    for block_id, (_, length) in enumerate(sp.BLOCKS):
        struct.pack_into("<IH", save, sp.BLOCK_TABLE + 0x14 + block_id * 8, length, block_id)
    struct.pack_into(f"<{len(raws)}I", save, si.pouch_offset(), *raws)
    save[0x10] = 0x5A  # something else in block 0, kept
    for block_id in range(len(sp.BLOCKS)):
        sp.fix_checksum(save, block_id)
    return save


def counts(save):
    return [(item, count) for item, count, _ in si.read_pouch(save)]


class SaveItemsTest(unittest.TestCase):
    def test_reads_pouch_in_order(self):
        self.assertEqual(counts(make_save()), [(4, 55), (3, 5), (80, 2)])

    def test_change_add_remove(self):
        save = make_save()
        before = bytes(save)
        si.set_items(save, [(4, 1), (1, 3), (2, 10), (3, 0)])
        self.assertEqual(counts(save), [(4, 1), (80, 2), (1, 3), (2, 10)])  # packed, new ones at the end
        self.assertTrue(sp.block_checksum_ok(save, si.ITEM_BLOCK))
        self.assertEqual(save[0x10], 0x5A)
        changed = [i for i in range(len(save)) if save[i] != before[i]]
        ofs, length = sp.BLOCKS[si.ITEM_BLOCK]
        table = sp.checksum_offset(si.ITEM_BLOCK)
        self.assertTrue(all(ofs <= i < ofs + length or table <= i < table + 2 for i in changed))

    def test_keeps_other_bits(self):
        save = make_save()
        si.set_items(save, [(3, 7)])
        raw = struct.unpack_from("<I", save, si.pouch_offset() + 4)[0]
        self.assertEqual(raw, entry(3, 7, new=True))

    def test_refuses_bad_counts_and_full_pouch(self):
        with self.assertRaises(sp.SaveError):
            si.set_items(make_save(), [(4, 1000)])
        full = make_save(tuple(entry(100 + i, 1) for i in range(si.POUCH_SIZE)))
        with self.assertRaises(sp.SaveError):
            si.set_items(full, [(1, 1)])

    def test_refuses_wrong_checksum(self):
        save = make_save()
        save[0x20] ^= 1
        with self.assertRaises(sp.SaveError):
            si.read_pouch(save)

    def test_cli_writes_new_file_only(self):
        with tempfile.TemporaryDirectory() as tmp:
            src = os.path.join(tmp, "savedata.bin")
            dst = os.path.join(tmp, "new.bin")
            with open(src, "wb") as f:
                f.write(make_save())
            with redirect_stdout(io.StringIO()):
                self.assertEqual(si.main(["set", src, "--out", dst, "--item", "1=3", "--item", "4=1"]), 0)
            with open(src, "rb") as f:
                self.assertEqual(counts(bytearray(f.read())), [(4, 55), (3, 5), (80, 2)])
            with open(dst, "rb") as f:
                self.assertEqual(counts(bytearray(f.read())), [(4, 1), (3, 5), (80, 2), (1, 3)])
            with redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()):
                self.assertEqual(si.main(["set", src, "--out", dst, "--item", "1=1"]), 1)  # exists


if __name__ == "__main__":
    unittest.main()
