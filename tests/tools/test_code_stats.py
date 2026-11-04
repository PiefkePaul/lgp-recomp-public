"""Unit tests for the P1.5 code-statistics tools (synthetic inputs only, no game data):
tools/analyze/nso2elf.py and tools/analyze/ghidra/stats_report.py.

The Ghidra scripts themselves need a Ghidra install and real modules; they run locally only.

Run: python -m unittest discover -s tests/tools -v
"""
import json
import os
import struct
import sys
import tempfile
import types
import unittest

HERE = os.path.dirname(__file__)
sys.path.insert(0, os.path.join(HERE, "..", "..", "tools", "analyze"))
sys.path.insert(0, os.path.join(HERE, "..", "..", "tools", "analyze", "ghidra"))
sys.path.insert(0, HERE)
import nso2elf  # noqa: E402
import stats_report  # noqa: E402
import compare_recomp  # noqa: E402
import compile_lifted  # noqa: E402
import nso_inventory as inv  # noqa: E402
from test_nso_inventory import make_nso  # noqa: E402


def uleb(v: int) -> bytes:
    out = bytearray()
    while True:
        b = v & 0x7F
        v >>= 7
        out.append(b | (0x80 if v else 0))
        if not v:
            return bytes(out)


def pad4(b: bytearray) -> None:
    while len(b) % 4:
        b.append(0)  # DW_CFA_nop


def build_eh_image(funcs: list[tuple[int, int, bool]]) -> bytes:
    """Image with MOD0 -> .eh_frame_hdr -> .eh_frame holding one FDE per (start, size, has_lsda)."""
    img = bytearray(0x400)
    struct.pack_into("<I", img, 4, 0x10)
    eh = 0x100
    frame = bytearray()

    def cie(aug: bytes, aug_data: bytes) -> int:
        off = eh + len(frame)
        body = bytearray(struct.pack("<I", 0)) + bytes([1]) + aug + b"\0" + uleb(4) + bytes([0x78, 30])
        body += uleb(len(aug_data)) + aug_data
        pad4(body)
        frame.extend(struct.pack("<I", len(body)) + body)
        return off

    plain = cie(b"zR", bytes([0x1B]))
    # P (indirect pcrel sdata4 personality, value irrelevant), L and R pcrel sdata4
    with_lsda = cie(b"zPLR", bytes([0x9B]) + struct.pack("<i", 0) + bytes([0x1B, 0x1B]))
    table = []
    for start, size, lsda in funcs:
        fde = eh + len(frame)
        cie_off = with_lsda if lsda else plain
        body = bytearray(struct.pack("<I", fde + 4 - cie_off))
        pc_field = fde + 8
        body += struct.pack("<ii", start - pc_field, size)
        if lsda:
            lsda_field = fde + 8 + 8 + 1
            body += uleb(4) + struct.pack("<i", 0x300 - lsda_field)
        else:
            body += uleb(0)
        pad4(body)
        frame.extend(struct.pack("<I", len(body)) + body)
        table.append((start, fde))
    frame.extend(struct.pack("<I", 0))  # terminator
    img[eh:eh + len(frame)] = frame

    hdr = 0x280
    h = bytearray([1, 0x1B, 0x03, 0x3B]) + struct.pack("<iI", eh - (hdr + 4), len(table))
    for start, fde in sorted(table):
        h += struct.pack("<ii", start - hdr, fde - hdr)
    img[hdr:hdr + len(h)] = h
    struct.pack_into("<4s5i", img, 0x10, b"MOD0", 0, 0, 0, hdr - 0x10, hdr + len(h) - 0x10)
    return bytes(img)


class EhFrameTests(unittest.TestCase):
    def test_fde_table_with_and_without_lsda(self):
        mod = types.SimpleNamespace(image=build_eh_image([(0x80, 0x20, True), (0x40, 0x10, False)]))
        fdes = nso2elf.parse_eh_frame(mod)
        self.assertEqual([(f["start"], f["size"], f["lsda"]) for f in fdes],
                         [(0x40, 0x10, False), (0x80, 0x20, True)])
        self.assertEqual([f["personality"] for f in fdes], [False, True])

    def test_no_mod0_gives_empty_list(self):
        self.assertEqual(nso2elf.parse_eh_frame(types.SimpleNamespace(image=bytes(0x100))), [])

    def test_unsupported_table_encoding_raises(self):
        img = bytearray(build_eh_image([(0x40, 0x10, False)]))
        img[0x283] = 0x03
        with self.assertRaises(nso2elf.EhFrameError):
            nso2elf.parse_eh_frame(types.SimpleNamespace(image=bytes(img)))


class ElfTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        path = os.path.join(self.tmp.name, "main")
        with open(path, "wb") as f:
            f.write(make_nso(["nnMain"], ["malloc"], compress=True))
        self.nso = path
        self.mod = inv.load_module(path)

    def test_elf_layout(self):
        elf = nso2elf.build_elf(self.mod)
        self.assertEqual(elf[:4], b"\x7fELF")
        e_type, e_machine = struct.unpack_from("<HH", elf, 16)
        self.assertEqual((e_type, e_machine), (nso2elf.ET_DYN, nso2elf.EM_AARCH64))
        phoff = struct.unpack_from("<Q", elf, 32)[0]
        phnum = struct.unpack_from("<H", elf, 56)[0]
        phdrs = [struct.unpack_from("<IIQQQQQQ", elf, phoff + i * 56) for i in range(phnum)]
        loads = [p for p in phdrs if p[0] == nso2elf.PT_LOAD]
        self.assertEqual([(p[1], p[3]) for p in loads], [(5, 0), (4, 0x200), (6, 0x400)])
        self.assertEqual(loads[2][6], 0x200 + 0x1000)  # .data memsz includes .bss
        for p in loads:
            self.assertEqual(p[2], nso2elf.FILE_BASE + p[3])
        dyn = [p for p in phdrs if p[0] == nso2elf.PT_DYNAMIC]
        self.assertEqual(len(dyn), 1)
        self.assertEqual(dyn[0][3], 0x400)
        self.assertEqual(elf[nso2elf.FILE_BASE:], self.mod.image)

    def test_cli_writes_elf_and_meta(self):
        out = os.path.join(self.tmp.name, "main.elf")
        meta = os.path.join(self.tmp.name, "main.meta.json")
        self.assertEqual(nso2elf.main([self.nso, out, "--meta", meta]), 0)
        with open(meta, encoding="utf-8") as f:
            m = json.load(f)
        self.assertEqual(m["module"], "main")
        self.assertEqual(m["fde_count"], 0)
        self.assertEqual(m["segments"]["data"], [0x400, 0x200])

    def test_cli_rejects_garbage(self):
        bad = os.path.join(self.tmp.name, "bad")
        with open(bad, "wb") as f:
            f.write(b"\0" * 64)
        self.assertEqual(nso2elf.main([bad, os.path.join(self.tmp.name, "x.elf")]), 1)


def fake_stats(module: str) -> dict:
    dist = {"count": 3, "sum": 60, "min": 4, "p50": 16, "p90": 40, "p99": 40, "max": 40, "buckets": {}}
    return {
        "module": module, "module_id": "00", "ghidra_version": "test", "text_bytes": 400,
        "fde": {"count": 3, "with_lsda": 1, "with_personality": 1, "size_bytes": dist},
        "functions": {"total": 5, "thunks": 1, "no_return": 0, "at_fde_start": 3, "without_fde": 2,
                      "without_fde_thunks": 1, "fde_without_function": 0, "body_bytes_non_thunk": dist},
        "instructions": {
            "total": 100,
            "classes": {"int_alu": 50, "load_store_gpr": 30, "branch_indirect": 5, "neon_vector": 15},
            "mnemonics": {"add": 50}, "sysregs": {"mrs tpidrro_el0": 2}, "svc_immediates": {"0x6": 1},
            "calls": {"bl_total": 9, "bl_to_non_function": 0, "bl_to_function_without_fde": 1,
                      "distinct_bl_targets_without_fde": 1, "b_tailcalls_to_other_function": 2},
            "br": {"total": 3, "jumptable_recovered": 1, "unresolved": 2, "unresolved_pattern_loaded_pointer": 2},
            "blr": {"total": 2, "unresolved": 2, "pattern_load_of_load_vtable_like": 2},
            "ret": {"via_x30": 4},
            "jumptables": {"recovered": 1, "cases_total": 7, "idiom_jumptable_idiom_ldrb": 1,
                           "cases_per_table": {"count": 1, "p50": 7, "max": 7}},
            "address_taken_functions_from_code": 1,
        },
        "data_pointers": {"slots_pointing_into_text": 4, "slots_pointing_to_function_entry": 3,
                          "distinct_functions_referenced_from_data": 2, "function_pointer_runs_len_ge2": 1,
                          "function_pointer_run_slots": 2, "longest_function_pointer_run": 2,
                          "address_taken_functions_total": 3},
        "imports": {"import_thunks": 1, "tracked_call_sites": {"setjmp": 3}, "top_imports_by_call_sites": {}},
    }


class ReportTests(unittest.TestCase):
    def test_report_orders_modules_and_contains_numbers(self):
        with tempfile.TemporaryDirectory() as d:
            for name in ("sdk", "main"):
                with open(os.path.join(d, f"{name}.stats.json"), "w", encoding="utf-8") as f:
                    json.dump(fake_stats(name), f)
            out = os.path.join(d, "r.md")
            self.assertEqual(stats_report.main([d, "--out", out]), 0)
            with open(out, encoding="utf-8") as f:
                text = f.read()
        self.assertIn("| Class | main | sdk |", text)
        self.assertIn("| NEON vector | 15.00 % | 15.00 % |", text)
        self.assertIn("| setjmp | 3 | 3 |", text)
        self.assertIn("load_of_load_vtable_like", text)
        self.assertIn("jumptable_idiom_ldrb: 1", text)

    def test_empty_dir_fails(self):
        with tempfile.TemporaryDirectory() as d:
            self.assertEqual(stats_report.main([d]), 1)


class CompareRecompTests(unittest.TestCase):
    def test_functions_and_br_matrix(self):
        recomp = {
            "modules": [{"name": "rtld", "base": "0x8000000"}, {"name": "main", "base": "0x8004000"}],
            "functions": [
                {"entry": "0x8000000", "end": "0x8000010", "source": "entry", "blocks": []},  # other module
                {"entry": "0x8004100", "end": "0x8004200", "source": "fde", "blocks": [
                    {"start": "0x8004100", "end": "0x8004120", "term": "jump_table", "succ": ["0x8004140", "0x8004150"]},
                    {"start": "0x8004180", "end": "0x8004190", "term": "indirect_jump", "succ": []},
                ]},
                {"entry": "0x8004300", "end": "0x8004310", "source": "gap_scan", "blocks": [
                    {"start": "0x8004300", "end": "0x8004308", "term": "jump_table", "succ": ["0x8004304"]},
                ]},
            ],
            "diagnostics": [{"kind": "jump_table_unbounded", "pc": "0x8004304", "text": "x"},
                            {"kind": "undecodable_instruction", "pc": "0x8000004", "text": "x"}],
        }
        ghidra = {"functions": [[0x100, 256, 0], [0x500, 16, 1]],
                  "br": [[0x11c, "jumptable", 2], [0x18c, "unresolved", 0], [0x304, "unresolved", 0],
                         [0x504, "thunk", 0]]}
        with tempfile.TemporaryDirectory() as d:
            rp, gp = os.path.join(d, "f.json"), os.path.join(d, "g.json")
            for p, obj in ((rp, recomp), (gp, ghidra)):
                with open(p, "w", encoding="utf-8") as f:
                    json.dump(obj, f)
            rc = compare_recomp.load_recomp(rp, "main")
            res, details = compare_recomp.compare(rc, compare_recomp.load_ghidra(gp))
        self.assertEqual(res["functions_both"], 1)
        self.assertEqual(res["ghidra_only_thunk"], 1)
        self.assertEqual(res["recomp_only_by_source"], {"gap_scan": 1})
        m = res["br_matrix"]
        self.assertEqual(m[("jumptable", "jump_table")], 1)
        self.assertEqual(m[("unresolved", "indirect_jump")], 1)
        self.assertEqual(m[("thunk", "not_in_cfg")], 1)
        self.assertEqual(res["jumptable_target_counts"], {"same_target_count": 1})
        self.assertEqual(len(res["new_jumptables"]), 1)
        nj = res["new_jumptables"][0]
        self.assertTrue(nj["inside_function"] and nj["aligned"] and nj["unbounded"])
        self.assertEqual(rc["diags"], {"jump_table_unbounded": 1})  # rtld diagnostic not counted
        text = compare_recomp.report("main", res, rc["diags"])
        self.assertIn("| jumptable | 1 | 0 | 0 |", text)


class CompileLiftedTests(unittest.TestCase):
    def test_split_repeats_prelude_and_keeps_all_functions(self):
        src = ("#include \"lgp/lifted/lifted.hpp\"\n\nvoid lgp_fn_1(lgp::CpuState* ctx);\n"
               "void lgp_fn_2(lgp::CpuState* ctx);\n\n"
               + "".join(f"void lgp_fn_{i}(lgp::CpuState* ctx) {{\n    return;\n}}\n\n" for i in (1, 2, 3))
               + "extern const int tail = 1;\n")
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "l.cpp")
            with open(p, "w", encoding="utf-8") as f:
                f.write(src)
            units, n = compile_lifted.split(p, os.path.join(d, "u"), 2)
            texts = []
            for u in units:
                with open(u, encoding="utf-8") as f:
                    texts.append(f.read())
        self.assertEqual(n, 3)
        self.assertEqual(len(units), 2)
        for t in texts:
            self.assertTrue(t.startswith("#include \"lgp/lifted/lifted.hpp\""))
            self.assertIn("void lgp_fn_2(lgp::CpuState* ctx);", t)
        self.assertIn("void lgp_fn_2(lgp::CpuState* ctx) {", texts[0])
        self.assertIn("void lgp_fn_3(lgp::CpuState* ctx) {", texts[1])
        self.assertIn("extern const int tail", texts[1])

    def test_refuses_output_inside_repo(self):
        with self.assertRaises(SystemExit):
            compile_lifted.main([__file__, "--out-dir", os.path.join(HERE, "tmp_units")])


if __name__ == "__main__":
    unittest.main()
