"""Unit tests for tools/analyze/version_diff.py on synthetic NSOs and directories (no game data).

Run: python -m unittest -v test_version_diff (from tests/tools)
"""
import hashlib
import io
import os
import struct
import sys
import tempfile
import unittest
import unittest.mock
from contextlib import redirect_stdout

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "tools", "analyze"))
import version_diff as vd  # noqa: E402

NOP, RET = 0xD503201F, 0xD65F03C0
MOVZ_X0 = 0xD2800000  # movz x0, #imm16 << 5


def movz(v: int) -> int:
    return MOVZ_X0 | (v << 5)


def bl(off: int) -> int:
    return 0x94000000 | ((off // 4) & 0x3FFFFFF)


def adrp(rd: int, pages: int) -> int:
    return 0x90000000 | ((pages & 3) << 29) | (((pages >> 2) & 0x7FFFF) << 5) | rd


def ldr_x(rt: int, rn: int, imm: int) -> int:
    return 0xF9400000 | ((imm // 8) << 10) | (rn << 5) | rt


def code(*words: int) -> bytes:
    return struct.pack(f"<{len(words)}I", *words)


def make_nso(functions: list[bytes], module_id: bytes, ro_extra: bytes = b"") -> bytes:
    """Uncompressed NSO: .text = MOD0 header + functions, .ro = .eh_frame_hdr, .data = .dynamic."""
    text = bytearray(0x20)
    starts = []
    for fn in functions:
        starts.append(len(text))
        text += fn
    text += b"\0" * (-len(text) % 0x100)
    ro_off = len(text)
    ro = bytearray(struct.pack("<BBBBiI", 1, 0x1B, 0x03, 0x3B, 0, len(starts)))
    for s in starts:
        ro += struct.pack("<ii", s - ro_off, 0)
    ro += ro_extra
    ro += b"\0" * (-len(ro) % 0x100)
    data_off = ro_off + len(ro)
    data = bytes(0x100)  # .dynamic: one DT_NULL
    struct.pack_into("<I", text, 4, 0x10)
    struct.pack_into("<4s5i", text, 0x10, b"MOD0", data_off - 0x10, 0, 0, ro_off - 0x10, ro_off - 0x10 + 12)

    segs = [bytes(text), bytes(ro), data]
    hdr = bytearray(0x100)
    hdr[0:4] = b"NSO0"
    struct.pack_into("<I", hdr, 0x0C, 0b111000)
    file_off, mem_off = 0x100, 0
    for idx, seg in enumerate(segs):
        struct.pack_into("<3I", hdr, 0x10 + idx * 0x10, file_off, mem_off, len(seg))
        struct.pack_into("<I", hdr, 0x60 + idx * 4, len(seg))
        hdr[0xA0 + idx * 0x20:0xC0 + idx * 0x20] = hashlib.sha256(seg).digest()
        file_off += len(seg)
        mem_off += len(seg)
    hdr[0x40:0x40 + len(module_id)] = module_id
    return bytes(hdr) + b"".join(segs)


class MaskTests(unittest.TestCase):
    def test_branch_targets_do_not_change_the_hash(self):
        self.assertEqual(vd.function_hash(code(bl(0x40), RET)), vd.function_hash(code(bl(0x1000), RET)))

    def test_adrp_page_and_low_bits_are_masked(self):
        a = code(adrp(8, 1), ldr_x(0, 8, 0x10), RET)
        b = code(adrp(8, 7), ldr_x(0, 8, 0x28), RET)
        self.assertEqual(vd.function_hash(a), vd.function_hash(b))

    def test_load_offsets_off_other_bases_stay(self):
        self.assertNotEqual(vd.function_hash(code(ldr_x(0, 1, 0x10), RET)),
                            vd.function_hash(code(ldr_x(0, 1, 0x18), RET)))

    def test_branch_inside_the_function_stays(self):
        b_cond = lambda words: 0x54000000 | ((words & 0x7FFFF) << 5)  # b.eq
        self.assertNotEqual(vd.function_hash(code(b_cond(2), NOP, NOP, RET)),
                            vd.function_hash(code(b_cond(3), NOP, NOP, RET)))
        # Leaving the function: masked like a call.
        self.assertEqual(vd.function_hash(code(b_cond(40), RET)), vd.function_hash(code(b_cond(80), RET)))

    def test_field_offset_after_reloaded_base_stays(self):
        # adrp x8; ldr x8, [x8, #global]; ldr x0, [x8, #field]: only the first offset is placement.
        a = code(adrp(8, 1), ldr_x(8, 8, 0x10), ldr_x(0, 8, 0x20), RET)
        b = code(adrp(8, 3), ldr_x(8, 8, 0x30), ldr_x(0, 8, 0x20), RET)
        c = code(adrp(8, 1), ldr_x(8, 8, 0x10), ldr_x(0, 8, 0x28), RET)
        self.assertEqual(vd.function_hash(a), vd.function_hash(b))
        self.assertNotEqual(vd.function_hash(a), vd.function_hash(c))

    def test_literal_load_ends_the_adrp_base(self):
        ldr_lit = lambda rt, words: 0x58000000 | ((words & 0x7FFFF) << 5) | rt  # ldr xRt, literal
        a = code(adrp(8, 1), ldr_lit(8, 40), ldr_x(0, 8, 0x10), RET)
        b = code(adrp(8, 1), ldr_lit(8, 40), ldr_x(0, 8, 0x18), RET)
        self.assertNotEqual(vd.function_hash(a), vd.function_hash(b))

    def test_call_ends_the_adrp_base(self):
        a = code(adrp(8, 1), bl(0x100), ldr_x(0, 8, 0x10), RET)
        b = code(adrp(8, 1), bl(0x100), ldr_x(0, 8, 0x18), RET)
        self.assertNotEqual(vd.function_hash(a), vd.function_hash(b))

    def test_immediates_stay(self):
        self.assertNotEqual(vd.function_hash(code(movz(1), RET)), vd.function_hash(code(movz(2), RET)))


class AlignTests(unittest.TestCase):
    @staticmethod
    def fns(hashes: str, base: int = 0x100) -> list:
        return [(base + 0x10 * k, 0x10, h) for k, h in enumerate(hashes)]

    def statuses(self, old: str, new: str, shift: int = 0) -> list:
        return [r["status"] for r in vd.align_functions(self.fns(old), self.fns(new, 0x100 + shift))]

    def test_identical(self):
        self.assertEqual(self.statuses("abc", "abc"), ["same"] * 3)

    def test_insert_moves_the_rest(self):
        st = vd.align_functions(self.fns("abc"), self.fns("aXbc"))
        self.assertEqual([r["status"] for r in st], ["same", "added", "moved", "moved"])
        self.assertEqual((st[2]["old"], st[2]["new"]), (0x110, 0x120))

    def test_changed_and_removed(self):
        self.assertEqual(self.statuses("abcd", "aBd"), ["same", "changed", "removed", "moved"])

    def test_duplicate_bodies_align_in_order(self):
        self.assertEqual(self.statuses("xaxbx", "xaxbx", shift=0x40), ["moved"] * 5)


class BranchTargetTests(unittest.TestCase):
    def test_retargeted_call_is_changed(self):
        callee_a, callee_b = code(movz(1), RET), code(movz(2), RET)
        caller = lambda to: code(bl(to), RET)  # caller at 0x100, callees at 0x108 and 0x110
        old = vd.split_functions(caller(8) + callee_a + callee_b, 0x100, [0x100, 0x108, 0x110])
        new = vd.split_functions(caller(16) + callee_a + callee_b, 0x100, [0x100, 0x108, 0x110])
        self.assertEqual(old[0][2], new[0][2])  # same masked hash ...
        rows = vd.align_functions(old, new)
        vd.check_branch_targets(rows, old, new)
        self.assertEqual([r["status"] for r in rows], ["changed", "same", "same"])  # ... but another callee

    def test_moved_callee_keeps_the_caller(self):
        caller = lambda to: code(bl(to), RET)
        callee = code(movz(1), RET)
        old = vd.split_functions(caller(8) + callee, 0x100, [0x100, 0x108])
        new = vd.split_functions(caller(16) + code(movz(9), RET) + callee, 0x100, [0x100, 0x108, 0x110])
        rows = vd.align_functions(old, new)
        vd.check_branch_targets(rows, old, new)
        self.assertEqual([r["status"] for r in rows], ["same", "added", "moved"])


class ExefsTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)

    def exefs(self, name: str, blobs: dict) -> str:
        d = os.path.join(self.tmp.name, name)
        os.makedirs(d)
        for mod, blob in blobs.items():
            with open(os.path.join(d, mod), "wb") as f:
                f.write(blob)
        return d

    def test_update_with_one_changed_and_one_new_function(self):
        f1 = code(movz(1), bl(0x10), RET, NOP)
        f2 = code(adrp(9, 2), ldr_x(0, 9, 8), RET, NOP)
        f3 = code(movz(3), RET)
        old = self.exefs("old", {"main": make_nso([f1, f2, f3], b"\x01" * 16),
                                 "sdk": make_nso([f3], b"\x05" * 20)})
        new = self.exefs("new", {"main": make_nso([f1, code(movz(9), RET), code(movz(7), RET), f2, f3],
                                                  b"\x02" * 16, ro_extra=b"x"),
                                 "sdk": make_nso([f3], b"\x05" * 20)})
        res = vd.diff_exefs(old, new)
        main, sdk = res["modules"]
        self.assertTrue(sdk["identical"])
        self.assertEqual(sdk["functions"]["same"], 1)
        self.assertFalse(main["identical"])
        self.assertEqual(main["functions"], {"old_count": 3, "new_count": 5, "same": 1, "moved": 2,
                                             "changed": 0, "added": 2, "removed": 0})

        tsv = os.path.join(self.tmp.name, "map.tsv")
        out = io.StringIO()
        with redirect_stdout(out):
            self.assertEqual(vd.main(["exefs", old, new, "--map", tsv]), 0)
        self.assertIn("| main | no |", out.getvalue())
        with open(tsv, encoding="utf-8") as f:
            lines = f.read().splitlines()
        self.assertEqual(lines[0], "module\told\tnew\tsize\tstatus")
        self.assertEqual(len(lines), 5)  # 3 from main, 1 from the identical sdk
        self.assertEqual(lines[1].split("\t")[4], "same")

    def test_missing_main_is_an_error(self):
        d = self.exefs("empty", {})
        with redirect_stdout(io.StringIO()), unittest.mock.patch("sys.stderr", io.StringIO()):
            self.assertEqual(vd.main(["exefs", d, d]), 2)


class RomfsTests(unittest.TestCase):
    def test_changed_added_removed(self):
        with tempfile.TemporaryDirectory() as t:
            for side, files in (("a", {"bin/x": b"1", "bin/y": b"22", "z": b"3"}),
                                ("b", {"bin/x": b"1", "bin/y": b"2X", "w/v": b"4"})):
                for rel, blob in files.items():
                    p = os.path.join(t, side, rel)
                    os.makedirs(os.path.dirname(p), exist_ok=True)
                    with open(p, "wb") as f:
                        f.write(blob)
            res = vd.diff_romfs(os.path.join(t, "a"), os.path.join(t, "b"))
            self.assertEqual(res["same"], 1)
            self.assertEqual([e["path"] for e in res["changed"]], ["bin/y"])
            self.assertEqual([e["path"] for e in res["added"]], ["w/v"])
            self.assertEqual([e["path"] for e in res["removed"]], ["z"])
            summary = vd.romfs_summary(res)
            self.assertIn("| bin | 1 | 0 | 0 |", summary)


if __name__ == "__main__":
    unittest.main()
