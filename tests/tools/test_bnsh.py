"""Unit tests for tools/analyze/bnsh.py and the pure parts of tools/analyze/shader_coverage.py.

Synthetic BNSH containers only (built here byte by byte), no game data.

Run: python -m unittest discover -s tests/tools -v
"""
import json
import os
import struct
import sys
import tempfile
import unittest

HERE = os.path.dirname(__file__)
sys.path.insert(0, os.path.join(HERE, "..", "..", "tools", "analyze"))
import bnsh  # noqa: E402
import shader_coverage as sc  # noqa: E402


def sph(stage_type: int) -> bytes:
    """0x50-byte SPH with word 0 = type (1 VTG / 2 PS), version 3, shader type."""
    w0 = (2 if stage_type == 5 else 1) | (3 << 5) | (stage_type << 10)
    return struct.pack("<I", w0) + bytes(0x4C)


def build_bnsh(programs, file_name=b"test.bnsh", controls=None, reflection_wg=None):
    """programs: list of {stage: code bytes (after the NVN header)}; compute code has no header.
    controls: {stage: control section bytes} (default: magic + zeros); reflection_wg: workgroup
    size written to every program's compute reflection data."""
    controls = controls or {}
    out = bytearray(0x60)
    g = len(out)
    out += bytearray(0x60)  # grsc header area (0x60 bytes is enough here)
    varr = len(out)
    out += bytearray(0x40 * len(programs))
    stage_ptrs = []
    for prog in programs:
        po = len(out)
        out += bytearray(0x80)
        if reflection_wg is not None:
            refl = len(out)
            out += bytearray(0x30)
            cs = len(out)
            out += bytearray(0x48) + bytes(0x10)
            struct.pack_into("<3I", out, cs + bnsh.REFLECTION_WORKGROUP, *reflection_wg)
            struct.pack_into("<Q", out, refl + bnsh.REFLECTION_COMPUTE, cs)
            struct.pack_into("<Q", out, po + bnsh.PROGRAM_REFLECTION, refl)
        ptrs = {}
        for kind, code in prog.items():
            so = len(out)
            out += bytearray(0x20)
            ctrl = len(out)
            control = controls.get(kind, struct.pack("<I", bnsh.CONTROL_MAGIC) + bytes(12))
            out += control
            co = len(out)
            body = code if kind == "compute" else struct.pack("<I", bnsh.CODE_MAGIC) + bytes(0x2C) + code
            out += body
            struct.pack_into("<QQII", out, so + 0x08, ctrl, co, len(body), len(control))
            ptrs[kind] = so
        for i, kind in enumerate(bnsh.STAGES):
            if kind in ptrs:
                struct.pack_into("<Q", out, po + 8 + 8 * i, ptrs[kind])
        stage_ptrs.append(po)
    for v, po in enumerate(stage_ptrs):
        struct.pack_into("<QQ", out, varr + v * 0x40 + 0x10, po, g)
    name_off = len(out)
    out += file_name + b"\0"
    out[0:4] = b"BNSH"
    struct.pack_into("<IHBB", out, 8, 0x00020105, 0xFEFF, 12, 0)
    struct.pack_into("<IHH", out, 0x10, name_off, 0, g)
    struct.pack_into("<II", out, 0x18, 0, len(out))
    out[g:g + 4] = b"grsc"
    struct.pack_into("<HHB", out, g + 0x10, 4, 0, 0)
    struct.pack_into("<IQ", out, g + 0x1C, len(programs), varr)
    return bytes(out)


VS = sph(1) + bytes(range(64))
PS = sph(5) + bytes(range(32))
CS = bytes([0xF0, 0x07, 0x20, 0xE2]) + bytes(28)


class ParseTests(unittest.TestCase):
    def test_programs_and_stages(self):
        b = bnsh.parse(build_bnsh([{"vertex": VS, "pixel": PS}, {"compute": CS}]))
        self.assertEqual(b.version, (2, 1, 5))
        self.assertEqual(len(b.programs), 2)
        self.assertEqual(sorted(b.programs[0].stages), ["pixel", "vertex"])
        self.assertEqual(b.programs[0].stages["vertex"].code, VS)
        self.assertEqual(b.programs[0].stages["pixel"].code, PS)
        self.assertEqual(b.programs[1].stages["compute"].code, CS)  # no NVN header, no SPH

    def test_rejects_wrong_sph_type(self):
        with self.assertRaises(bnsh.BnshError):
            bnsh.parse(build_bnsh([{"vertex": PS}]))

    def test_rejects_non_bnsh_and_bad_pointer(self):
        with self.assertRaises(bnsh.BnshError):
            bnsh.parse(b"BNTX" + bytes(0x40))
        blob = bytearray(build_bnsh([{"vertex": VS}]))
        struct.pack_into("<Q", blob, 0x60 + 0x20, len(blob) + 0x100)  # variation array beyond the end
        with self.assertRaises(bnsh.BnshError):
            bnsh.parse(bytes(blob))

    def test_extract_dedups_and_pairs(self):
        blob = build_bnsh([{"vertex": VS, "pixel": PS}, {"vertex": VS, "pixel": PS}])
        data = b"junk" + blob + b"more" + blob  # embedded twice, e.g. inside an archive
        with tempfile.TemporaryDirectory() as d:
            rep = bnsh.extract([("gfpak", "a", data)], d)
            with open(os.path.join(d, "manifest.json"), encoding="utf-8") as f:
                man = json.load(f)
            files = sorted(os.listdir(d))
            with open(os.path.join(d, "programs.tsv"), encoding="utf-8") as f:
                programs = f.read().splitlines()
        self.assertEqual(rep["containers"], {"found": 2, "unique": 1, "parsed": 1})
        self.assertEqual(rep["stages"], {"vertex": 1, "pixel": 1})
        vid = next(k for k, v in man.items() if v["stage"] == "vertex")
        pid = next(k for k, v in man.items() if v["stage"] == "pixel")
        self.assertEqual(man[vid]["pixel"], pid)
        self.assertIn(f"{vid}.vertex.bin", files)
        # Both variations are the same program: one line, counted twice.
        self.assertEqual(programs, [f"graphics\t{vid}\t{pid}\t2", f"pixel\t{pid}\t4"])
        self.assertEqual(rep["programs"], {"graphics": 1, "pixel": 1})

    def test_loose_vsh_fsh_linked_by_name(self):
        vsh, fsh = build_bnsh([{"vertex": VS}]), build_bnsh([{"pixel": PS}])
        with tempfile.TemporaryDirectory() as d:
            rep = bnsh.extract([("bnsh_vsh", "cut/a.bnsh_vsh", vsh), ("bnsh_fsh", "cut/a.bnsh_fsh", fsh),
                                ("bnsh_vsh", "cut/b.bnsh_vsh", vsh)], d)
            with open(os.path.join(d, "programs.tsv"), encoding="utf-8") as f:
                programs = f.read().splitlines()
        vid, pid = bnsh.stage_id(bnsh.parse(vsh).programs[0].stages["vertex"]), \
            bnsh.stage_id(bnsh.parse(fsh).programs[0].stages["pixel"])
        self.assertIn(f"graphics\t{vid}\t{pid}\t1", programs)
        self.assertIn(f"graphics\t{vid}\t-\t1", programs)  # the vertex-only container itself
        self.assertEqual(rep["loose_vsh_fsh"], {"programs": 1, "files without their partner": 1})

    def test_within_file_combinations(self):
        # One archive with two containers: (VS, PS) and (VS2, PS2); within the file also VS+PS2, VS2+PS.
        vs2, ps2 = VS[:-8] + b"\x01" * 8, PS[:-8] + b"\x02" * 8
        data = build_bnsh([{"vertex": VS, "pixel": PS}]) + build_bnsh([{"vertex": vs2, "pixel": ps2}])
        with tempfile.TemporaryDirectory() as d:
            rep = bnsh.extract([("gfpak", "m.gfpak", data)], d)
            with open(os.path.join(d, "programs-within-file.tsv"), encoding="utf-8") as f:
                within = f.read().splitlines()
        self.assertEqual(rep["within_file_combinations"], 2)
        self.assertEqual(len(within), 2)
        self.assertTrue(all(line.startswith("graphics\t") for line in within))


def compute_ctrl(wg, shared, entry=0x100, blob=0, local=0x20):
    """Control section of a compute stage in the layout gpu/nvn assumes (abi.hpp)."""
    c = bytearray(0x800)
    struct.pack_into("<I", c, 0, bnsh.CONTROL_MAGIC)
    struct.pack_into("<I", c, bnsh.CTL_BLOB_SIZE, blob)
    struct.pack_into("<I", c, bnsh.CTL_ENTRY_OFFSET, entry)
    struct.pack_into("<I", c, bnsh.CTL_STAGE, bnsh.NVN_STAGE_COMPUTE)
    struct.pack_into("<3I", c, bnsh.CTL_CS_WORKGROUP, *wg)
    struct.pack_into("<I", c, bnsh.CTL_CS_SHARED, shared)
    struct.pack_into("<I", c, bnsh.CTL_CS_LOCAL_LO, local)
    struct.pack_into("<I", c, bnsh.CTL_CS_BARRIERS, 1)
    return bytes(c)


class ComputeTests(unittest.TestCase):
    def test_entry_offset_blob_and_fields(self):
        code = bytes(0x100) + CS + b"\xAA" * 0x20  # prefix, code, then bytes past the blob
        blob = build_bnsh([{"compute": code}], controls={"compute": compute_ctrl((64, 2, 1), 0x400, blob=0x120)},
                          reflection_wg=(64, 2, 1))
        st = bnsh.parse(blob).programs[0].stages["compute"]
        self.assertEqual(st.code, CS)
        self.assertTrue(st.skipped_zero)
        self.assertEqual(st.compute["workgroup"], [64, 2, 1])
        self.assertEqual(st.compute["shared"], 0x400)
        self.assertEqual(st.reflection_workgroup, (64, 2, 1))
        with tempfile.TemporaryDirectory() as d:
            bnsh.extract([("romfs", "a", blob)], d)
            with open(os.path.join(d, "manifest.json"), encoding="utf-8") as f:
                man = json.load(f)
            with open(os.path.join(d, "programs.tsv"), encoding="utf-8") as f:
                programs = f.read().splitlines()
        (m,) = man.values()
        self.assertEqual(programs, [f"compute\t{next(iter(man))}\t64\t2\t1\t1024\t32\t1"])
        self.assertEqual(m["compute"]["local"], 0x20)
        self.assertEqual(sc.compute_args(m), ["--compute", "--workgroup", "64,2,1", "--shared", "1024",
                                              "--local", "32"])
        self.assertEqual(sc.compute_args({"stage": "compute"}), ["--compute"])
        self.assertEqual(sc.compute_args({"stage": "pixel"}), [])

    def test_report(self):
        a = build_bnsh([{"compute": bytes(0x100) + CS}], controls={"compute": compute_ctrl((8, 8, 1), 0)},
                       reflection_wg=(8, 8, 1), file_name=b"a.bnsh")
        b = build_bnsh([{"compute": b"\x01" * 0x100 + CS}], controls={"compute": compute_ctrl((16, 1, 1), 0x100)},
                       reflection_wg=(32, 1, 1), file_name=b"b.bnsh")
        c = build_bnsh([{"compute": CS}], file_name=b"c.bnsh")  # short control: layout not readable
        rep = bnsh.compute_report([("romfs", "x", a + b + c)])
        self.assertEqual(rep["counts"], {"compute_stages": 3, "control_readable": 2, "control_not_compute_layout": 1,
                                         "prefix_zero": 1, "prefix_nonzero": 1, "workgroup_plausible": 2,
                                         "reflection_match": 1, "reflection_mismatch": 1})
        self.assertEqual(rep["entry_offset"], {"0x100": 2})


class CoverageHelperTests(unittest.TestCase):
    def test_reason_strips_path_address_and_word(self):
        err = ("error: C:\\x\\y\\abc.pixel.bin: 0x00f8: e290000040000000: unsupported: FMUL scale\n")
        self.assertEqual(sc.reason(err), "unsupported: FMUL scale")
        self.assertEqual(sc.fail_word(err), 0xE290000040000000)
        self.assertEqual(sc.reason("error: /a/b.vertex.bin: empty program"), "empty program")

    def test_markdown(self):
        rep = {"stages": {"pixel": {"total": 4, "ok": 3}}, "reasons": {"pixel": {"unknown or unsupported opcode": 1}},
               "unsupported_opcodes": {"tmml": 1}, "census": {}, "validation": {"pixel_valid": 3},
               "validation_errors": {}}
        md = sc.to_markdown(rep)
        self.assertIn("| pixel | 4 | 3 | 75.0 % |", md)
        self.assertIn("| `tmml` | 1 |", md)
        self.assertIn("pixel_valid 3", md)


if __name__ == "__main__":
    unittest.main()
