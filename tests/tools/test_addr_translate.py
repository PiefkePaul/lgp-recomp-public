"""Unit tests for tools/analyze/addr_translate.py on synthetic NSOs (no game data).

Run: python -m unittest -v test_addr_translate (from tests/tools)
"""
import io
import json
import os
import sys
import tempfile
import unittest
from contextlib import redirect_stdout

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "tools", "analyze"))
sys.path.insert(0, os.path.dirname(__file__))
import addr_translate as at  # noqa: E402
from test_version_diff import NOP, RET, adrp, code, make_nso, movz  # noqa: E402

BASE = 0x08000000
TEXT0 = 0x30  # make_nso writes MOD0 over the first function, so each test module starts with a filler
FN = 0x10     # every test function is four instructions
RO = 0x100    # .ro starts here while the text stays below 0x100 bytes


def add_x(rd: int, rn: int, imm: int) -> int:
    return 0x91000000 | (imm << 10) | (rn << 5) | rd


def fn(k: int) -> bytes:
    return code(movz(k), NOP, NOP, RET)


def ref_fn(slot: int, target: int) -> bytes:
    """A function at function slot `slot` that computes the image offset `target` (adrp + add)."""
    pc = TEXT0 + slot * FN
    return code(adrp(8, (target >> 12) - (pc >> 12)), add_x(8, 8, target & 0xFFF), NOP, RET)


def extra_at(n_functions: int) -> int:
    """Image offset of make_nso's ro_extra: .eh_frame_hdr (12 bytes + 8 per function) comes first."""
    return RO + 12 + 8 * (n_functions + 1)


class TranslateTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)

    def exefs(self, name: str, functions: list, ro_extra: bytes = b"", bss: int = 0) -> str:
        d = os.path.join(self.tmp.name, name)
        os.makedirs(d)
        nso = bytearray(make_nso([code(NOP, NOP, NOP, RET)] + functions, name.encode().ljust(16, b"\0"), ro_extra))
        nso[0x3C:0x40] = bss.to_bytes(4, "little")  # NSO header: .bss size
        with open(os.path.join(d, "main"), "wb") as f:
            f.write(nso)
        return d

    def image(self, old: str, new: str) -> at.ImageMap:
        return at.ImageMap(old, new, modules=("main",))

    def test_moved_function_keeps_the_offset_inside(self):
        old = self.exefs("old", [fn(1), fn(2)])
        new = self.exefs("new", [fn(9), fn(1), fn(2)])
        img = self.image(old, new)
        r = img.translate_va(BASE + TEXT0 + FN + 4)  # second instruction of fn(2)
        self.assertEqual((r.new, r.status), (BASE + TEXT0 + 2 * FN + 4, "moved"))

    def test_changed_function_is_not_carried_over(self):
        old = self.exefs("old", [fn(1), fn(2)])
        new = self.exefs("new", [fn(1), fn(7)])
        r = self.image(old, new).translate_va(BASE + TEXT0 + FN)
        self.assertIsNone(r.new)
        self.assertIn("changed", r.status)

    def test_unique_bytes_find_moved_data(self):
        blob = b"bin/some/unique/path.bseq\0"
        old = self.exefs("old", [fn(1), fn(2)], ro_extra=blob)
        new = self.exefs("new", [fn(9), fn(1), fn(2)], ro_extra=blob)
        r = self.image(old, new).translate_va(BASE + extra_at(2) + 4)
        self.assertEqual((r.new, r.status), (BASE + extra_at(3) + 4, "bytes"))

    def test_code_reference_finds_data_whose_bytes_repeat(self):
        blob = b"\x11\x22\x33\x44" * 8 + b"\x11\x22\x33\x44" * 8  # no unique window
        old_t, new_t = extra_at(2) + 32, extra_at(3) + 32
        old = self.exefs("old", [fn(1), ref_fn(1, old_t)], ro_extra=blob)
        new = self.exefs("new", [fn(9), fn(1), ref_fn(2, new_t)], ro_extra=blob)
        img = self.image(old, new)
        r = img.translate_va(BASE + old_t)
        self.assertEqual(r.new, BASE + new_t)
        self.assertIn("code", r.status)
        # A field behind the referenced start follows it.
        r = img.translate_va(BASE + old_t + 8)
        self.assertEqual(r.new, BASE + new_t + 8)
        self.assertIn("code+offset", r.status)

    def test_bss_by_code_reference_and_offset(self):
        bss_start = RO + 0x100 + 0x100  # .ro and .data are one 0x100 page each here
        old = self.exefs("old", [fn(1), ref_fn(1, bss_start + 0x10)], bss=0x100)
        new = self.exefs("new", [fn(9), fn(1), ref_fn(2, bss_start + 0x10)], bss=0x100)
        r = self.image(old, new).translate_va(BASE + bss_start + 0x10)
        self.assertEqual((r.new, r.status), (BASE + bss_start + 0x10, "code+bss offset"))
        moved = self.exefs("moved", [fn(9), fn(1), ref_fn(2, bss_start + 0x18)], bss=0x100)
        r = self.image(old, moved).translate_va(BASE + bss_start + 0x10)
        self.assertIsNone(r.new)
        self.assertIn("disagree", r.status)

    def test_relative_pointers_from_rela_and_relr(self):
        import struct
        from types import SimpleNamespace
        img = bytearray(0x400)
        struct.pack_into("<QQq", img, 0x100, 0x200, at.R_AARCH64_RELATIVE, 0x3a0)  # RELA: [0x200] = 0x3a0
        struct.pack_into("<QQq", img, 0x118, 0x208, 0x101, 0x10)                  # another type: left out
        struct.pack_into("<QQ", img, 0x140, 0x300, 0b101)  # RELR: 0x300, then bitmap bit 2 = 0x308 + 8
        struct.pack_into("<Q", img, 0x300, 0x3b0)
        struct.pack_into("<Q", img, 0x310, 0x3c0)
        mod = SimpleNamespace(image=bytes(img), dynamic={7: [0x100], 8: [0x30], 36: [0x140], 35: [0x10]})
        self.assertEqual(at.relative_pointers(mod), {0x200: 0x3a0, 0x300: 0x3b0, 0x310: 0x3c0})

    def test_overwritten_adrp_register_is_no_page(self):
        """Codex P2 on #545: adrp x8; movz x8, #1; add x8, x8, #lo is no reference to page + lo."""
        target = extra_at(1) + 0x20
        clobbered = code(adrp(8, (target >> 12) - (TEXT0 >> 12)), movz(1) | 8, add_x(8, 8, target & 0xFFF), RET)
        mod = at.inv.load_exefs(self.exefs("old", [clobbered]))["main"]
        self.assertNotIn(target, at.code_references(mod))
        kept = code(adrp(8, (target >> 12) - (TEXT0 >> 12)), movz(1), add_x(8, 8, target & 0xFFF), RET)  # x0 written
        mod = at.inv.load_exefs(self.exefs("kept", [kept]))["main"]
        self.assertIn(target, at.code_references(mod))

    def test_branch_target_ends_the_page(self):
        target = extra_at(1) + 0x20
        b_next = 0x14000001  # b .+4: the add below is also reached by a branch
        fn_ = code(adrp(8, (target >> 12) - (TEXT0 >> 12)), b_next, add_x(8, 8, target & 0xFFF), RET)
        mod = at.inv.load_exefs(self.exefs("old", [fn_]))["main"]
        self.assertNotIn(target, at.code_references(mod))

    def test_unsigned_jump_table_entries(self):
        """Codex P2 on #545: a 1-byte table marked unsigned reads 0x80.. as 128.., not as negative."""
        old_tab = extra_at(2)
        old = self.exefs("old", [fn(1), ref_fn(1, old_tab)], ro_extra=bytes([0x80, 0x81]) + b"\0" * 14)
        img = self.image(old, old)
        line = f"jumptable br=0x{BASE + TEXT0:x} table=0x{BASE + old_tab:x} entry=1 count=2 shift=2 base=0x{BASE:x} "
        self.assertEqual(img.jump_targets(at._jump_fields(line + "unsigned"), new=False), [BASE + 0x200, BASE + 0x204])
        self.assertEqual(img.jump_targets(at._jump_fields(line), new=False), [BASE - 0x200, BASE - 0x1FC])
        self.assertEqual(at._jump_fields(line + "unsigned signed").get("signed"), 1)
        with self.assertRaises(ValueError):
            at._jump_fields(line + "bogus")

    def test_window_must_be_unique_in_the_old_segment_too(self):
        """Codex P2 on #545: two old objects with the same 16-byte prefix; the new build keeps only the second.
        The first one's address must not be carried over to the second's place."""
        a = b"same-prefix-1234" + b"A" * 16  # objects differ only after 16 bytes
        b = b"same-prefix-1234" + b"B" * 16
        old = self.exefs("old", [fn(1)], ro_extra=a + b)
        new = self.exefs("new", [fn(1)], ro_extra=b"\x01" * 32 + b)  # object a is gone
        r = self.image(old, new).translate_va(BASE + extra_at(1))  # start of a
        self.assertNotEqual(r.new, BASE + extra_at(1) + 32, "a carried over to b's place")
        # b's own start: its 32-byte window is unique on both sides.
        r = self.image(old, new).translate_va(BASE + extra_at(1) + 32)
        self.assertEqual((r.new, r.status), (BASE + extra_at(1) + 32, "bytes"))

    def test_bytes_and_code_must_agree(self):
        blob = b"unique-object-bytes-here\0\0\0\0\0\0\0\0"
        old_t = extra_at(2)
        old = self.exefs("old", [fn(1), ref_fn(1, old_t)], ro_extra=blob)
        # The new code points 8 bytes further than where the bytes went.
        new = self.exefs("new", [fn(9), fn(1), ref_fn(2, extra_at(3) + 8)], ro_extra=blob)
        r = self.image(old, new).translate_va(BASE + old_t)
        self.assertIsNone(r.new)
        self.assertIn("disagree", r.status)

    def test_cfg_rewrites_addresses_only(self):
        old = self.exefs("old", [fn(1), fn(2)])
        new = self.exefs("new", [fn(9), fn(1), fn(2)])
        text = (f"hook door 0x{BASE + TEXT0 + FN:08x}\nvalue offset 0x10\nvalue hash 0x5d8153cafff4aba6\n"
                f"# note 0x{BASE + TEXT0:08x}\n")
        out, rows, complete = at.translate_cfg(text, self.image(old, new))
        self.assertTrue(complete)
        self.assertEqual(out.splitlines()[0], f"hook door 0x{BASE + TEXT0 + 2 * FN:08x}")
        self.assertEqual(out.splitlines()[1:], text.splitlines()[1:])
        self.assertEqual(len(rows), 1)

    def test_cli_writes_complete_files_and_marks_partial_ones(self):
        old = self.exefs("old", [fn(1), fn(2)], ro_extra=b"table-bytes-unique-0123456789\0")
        new = self.exefs("new", [fn(9), fn(1), fn(3)], ro_extra=b"table-bytes-unique-0123456789\0")
        kit = os.path.join(self.tmp.name, "kit")
        os.makedirs(os.path.join(kit, "mods", "m"))
        with open(os.path.join(kit, "game.cfg"), "w") as f:
            f.write(f"function 0x{BASE + TEXT0:08x}\n")
        with open(os.path.join(kit, "mods", "bad.cfg"), "w") as f:
            f.write(f"hook gone 0x{BASE + TEXT0 + FN:08x}\n")
        with open(os.path.join(kit, "mods", "m", "derive.json"), "w") as f:
            json.dump({"lengths": {"module": "main", "offset": f"0x{extra_at(2):x}", "count": 1}}, f)
        out_dir, report = os.path.join(self.tmp.name, "out"), os.path.join(self.tmp.name, "r.tsv")
        args = [old, new, kit, "--out", out_dir, "--report", report, "--modules", "main"]
        with redirect_stdout(io.StringIO()) as so:
            self.assertEqual(at.main(args), 1)
        self.assertIn("2 complete, 1 with unresolved", so.getvalue())
        self.assertNotIn("0x", so.getvalue())  # counts only on stdout
        with open(os.path.join(out_dir, "game.cfg")) as f:
            self.assertEqual(f.read(), f"function 0x{BASE + TEXT0 + FN:08x}\n")
        with open(os.path.join(out_dir, "mods", "m", "derive.json")) as f:
            self.assertEqual(json.load(f)["lengths"]["offset"], f"0x{extra_at(3):x}")
        self.assertFalse(os.path.exists(os.path.join(out_dir, "mods", "bad.cfg")))
        with redirect_stdout(io.StringIO()):
            self.assertEqual(at.main(args + ["--partial"]), 1)
        with open(os.path.join(out_dir, "mods", "bad.cfg")) as f:
            self.assertTrue(f.read().startswith(at.UNRESOLVED_MARK))

    def test_jump_table_targets_are_checked(self):
        # A 4-byte relative table in .ro whose entries lead into the second function (a moved one).
        def table(target_off: int, table_off: int) -> bytes:
            return (target_off - table_off).to_bytes(4, "little", signed=True) * 2

        old_tab, new_tab = extra_at(2), extra_at(3)
        old = self.exefs("old", [fn(1), ref_fn(1, old_tab)], ro_extra=table(TEXT0, old_tab) + b"\0" * 8)
        good = self.exefs("good", [fn(9), fn(1), ref_fn(2, new_tab)], ro_extra=table(TEXT0 + FN, new_tab) + b"\0" * 8)
        bad = self.exefs("bad", [fn(9), fn(1), ref_fn(2, new_tab)], ro_extra=table(TEXT0, new_tab) + b"\0" * 8)
        line = f"jumptable br=0x{BASE + TEXT0 + FN + 8:x} table=0x{BASE + old_tab:x} entry=4 count=2 shift=0\n"
        _, rows, complete = at.translate_cfg(line, self.image(old, good))
        self.assertTrue(complete)
        self.assertTrue(any("jump targets checked" in r.status for _, _, r in rows))
        _, _, complete = at.translate_cfg(line, self.image(old, bad))
        self.assertFalse(complete)


if __name__ == "__main__":
    unittest.main()
