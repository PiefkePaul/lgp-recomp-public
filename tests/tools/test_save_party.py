"""Unit test for tools/save/lgp_save_party.py (synthetic savedata.bin only).

Run: python -m unittest -v test_save_party (from tests/tools)
"""
import io
import os
import random
import struct
import sys
import tempfile
import unittest
from contextlib import redirect_stdout

HERE = os.path.dirname(__file__)
sys.path.insert(0, os.path.join(HERE, "..", "..", "tools", "save"))
import lgp_save_party as sp  # noqa: E402


def make_member(rng, level, hp, hp_max):
    raw = bytearray(rng.randbytes(sp.SLOT_SIZE))
    raw[4:6] = b"\0\0"
    stored = raw[8:sp.STORED_SIZE]
    words = struct.unpack_from(f"<{len(stored) // 2}H", stored)
    struct.pack_into("<H", raw, 6, sum(words) & 0xFFFF)
    stats = bytearray(sp.SLOT_SIZE - sp.STORED_SIZE)
    struct.pack_into("<IB", stats, 0, 0x8, level)  # poisoned
    struct.pack_into("<HH", stats, 8, hp, hp_max)
    struct.pack_into("<H", stats, 0x16, 999)
    seed = struct.unpack_from("<I", raw, 0)[0]
    raw[8:sp.STORED_SIZE] = sp.lcg_xor(stored, seed)
    raw[sp.STORED_SIZE:] = sp.lcg_xor(stats, seed)
    return raw


def make_save(party=((5, 20, 20), (12, 31, 40), (30, 90, 90)), indices=None):
    rng = random.Random(7)
    save = bytearray(sp.SAVE_SIZE)
    struct.pack_into("<QQ4s", save, sp.BLOCK_TABLE, 1, 2, b"BEEF")
    for block_id, (_, length) in enumerate(sp.BLOCKS):
        struct.pack_into("<IH", save, sp.BLOCK_TABLE + 0x14 + block_id * 8, length, block_id)
    indices = indices or [3, 0, 7][:len(party)]
    header = indices + [1001] * (6 - len(indices))
    struct.pack_into("<6H", save, sp.BLOCKS[sp.PARTY_BLOCK][0], *header)
    for index, (level, hp, hp_max) in zip(indices, party):
        ofs = sp.slot_offset(index)
        save[ofs:ofs + sp.SLOT_SIZE] = make_member(rng, level, hp, hp_max)
    for block_id in range(len(sp.BLOCKS)):
        sp.fix_checksum(save, block_id)
    return save


class SavePartyTest(unittest.TestCase):
    def test_reads_party_in_slot_order(self):
        members = sp.party(make_save())
        self.assertEqual([m["index"] for m in members], [3, 0, 7])
        self.assertEqual([(m["level"], m["hp"], m["hp_max"], m["cp"]) for m in members],
                         [(5, 20, 20, 999), (12, 31, 40, 999), (30, 90, 90, 999)])

    def test_all_to_one_hp(self):
        save = make_save()
        before = bytes(save)
        after = sp.set_hp(save, 1)
        self.assertEqual([m["hp"] for m in after], [1, 1, 1])
        self.assertEqual([m["status"] for m in after], [8, 8, 8])
        self.assertTrue(all(sp.block_checksum_ok(save, i) for i in range(len(sp.BLOCKS))))
        # Only the party stats of the three members and block 9's checksum changed.
        changed = [i for i in range(len(save)) if save[i] != before[i]]
        allowed = set(range(sp.checksum_offset(sp.STORAGE_BLOCK), sp.checksum_offset(sp.STORAGE_BLOCK) + 2))
        for index in (3, 0, 7):
            ofs = sp.slot_offset(index)
            allowed |= set(range(ofs + sp.STORED_SIZE, ofs + sp.SLOT_SIZE))
        self.assertTrue(changed)
        self.assertTrue(set(changed) <= allowed)

    def test_faint_all_but_one(self):
        save = make_save()
        after = sp.set_hp(save, 1, faint_except=2)
        self.assertEqual([(m["hp"], m["status"]) for m in after], [(0, 0), (1, 8), (0, 0)])

    def test_hp_is_clamped_to_max(self):
        after = sp.set_hp(make_save(), 50)
        self.assertEqual([m["hp"] for m in after], [20, 40, 50])

    def test_refuses_bad_input(self):
        save = make_save()
        save[sp.BLOCKS[4][0]] ^= 1
        with self.assertRaisesRegex(sp.SaveError, r"checksums wrong .*\[4\]"):
            sp.party(save)
        with self.assertRaisesRegex(sp.SaveError, "size"):
            sp.party(make_save()[:-1])
        with self.assertRaisesRegex(sp.SaveError, "faint-except 4"):
            sp.set_hp(make_save(), 1, faint_except=4)
        with self.assertRaisesRegex(sp.SaveError, "at least 1"):
            sp.set_hp(make_save(), 0)
        broken = make_save()
        ofs = sp.slot_offset(0) + 0x20  # inside member 2's checksummed part
        broken[ofs] ^= 0xFF
        sp.fix_checksum(broken, sp.STORAGE_BLOCK)
        with self.assertRaisesRegex(sp.SaveError, r"do not decode: \[0\]"):
            sp.party(broken)

    def test_cli_writes_new_file_only(self):
        with tempfile.TemporaryDirectory() as tmp:
            src = os.path.join(tmp, "savedata.bin")
            out = os.path.join(tmp, "teamko", "savedata.bin")
            os.makedirs(os.path.dirname(out))
            with open(src, "wb") as f:
                f.write(make_save())
            with open(src, "rb") as f:
                original = f.read()
            with redirect_stdout(io.StringIO()) as text:
                self.assertEqual(sp.main(["set-hp", src, "--out", out, "--faint-except", "1"]), 0)
            self.assertIn("slot 1: storage 3, level 5, HP 1/20", text.getvalue())
            with open(src, "rb") as f:
                self.assertEqual(f.read(), original)
            with open(out, "rb") as f:
                self.assertEqual([m["hp"] for m in sp.party(bytearray(f.read()))], [1, 0, 0])
            with redirect_stdout(io.StringIO()):
                self.assertEqual(sp.main(["set-hp", src, "--out", out]), 1)  # exists


if __name__ == "__main__":
    unittest.main()
