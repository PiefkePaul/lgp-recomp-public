"""Unit tests for the P1.4 graphics tools on synthetic data only (no game data):
tools/analyze/nvn_inventory.py, shader_inventory.py, ryujinx_cache.py.

Run: python -m unittest discover -s tests/tools -v
"""
import os
import struct
import sys
import tempfile
import unittest
import zlib

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "tools", "analyze"))
import nso_inventory as ni  # noqa: E402
import nvn_inventory as nvn  # noqa: E402
import ryujinx_cache as rc  # noqa: E402
import shader_inventory as si  # noqa: E402


# --- AArch64 encoders -------------------------------------------------------------------------

def adrp(rd, pc, target):
    imm = ((target & ~0xFFF) - (pc & ~0xFFF)) >> 12
    return 0x90000000 | ((imm & 3) << 29) | (((imm >> 2) & 0x7FFFF) << 5) | rd


def add_imm(rd, rn, imm):
    return 0x91000000 | (imm << 10) | (rn << 5) | rd


def ldr(rt, rn, off):
    return 0xF9400000 | ((off // 8) << 10) | (rn << 5) | rt


def str_(rt, rn, off=0):
    return 0xF9000000 | ((off // 8) << 10) | (rn << 5) | rt


def bl(pc, target):
    return 0x94000000 | (((target - pc) >> 2) & 0x3FFFFFF)


BLR_X19 = 0xD63F0260
MOV_X20_X0 = 0xAA0003F4
RET = 0xD65F03C0
NOP = 0xD503201F


def code(words):
    return struct.pack(f"<{len(words)}I", *words)


class AdrpScannerTest(unittest.TestCase):
    def test_add_ldr_str_resolved(self):
        text = code([adrp(1, 0, 0x5000), add_imm(1, 1, 0x123),
                     adrp(8, 8, 0x6000), ldr(9, 8, 0x40), str_(9, 8, 0x48)])
        refs = list(nvn.iter_adrp_refs(text))
        self.assertEqual(refs, [(4, "add", 0x5123, 1), (12, "ldr", 0x6040, 9), (16, "str", 0x6048, 9)])

    def test_overwritten_base_register_is_dropped(self):
        # LDR x8, [x8, #0x10] overwrites x8; the following LDR x0, [x8] must not resolve.
        text = code([adrp(8, 0, 0x7000), ldr(8, 8, 0x10), ldr(0, 8, 0)])
        refs = list(nvn.iter_adrp_refs(text))
        self.assertEqual(refs, [(4, "ldr", 0x7010, 8)])

    def test_ret_clears_pending(self):
        text = code([adrp(8, 0, 0x7000), RET, ldr(0, 8, 0x8)])
        self.assertEqual(list(nvn.iter_adrp_refs(text)), [])

    def test_negative_page_offset(self):
        text = code([NOP] * 0x800 + [adrp(2, 0x2000, 0x1000), add_imm(2, 2, 4)])
        self.assertEqual(list(nvn.iter_adrp_refs(text)), [(0x2004, "add", 0x1004, 2)])

    def test_branch_targets(self):
        text = code([bl(0, 0x10), NOP, NOP, NOP, RET, bl(0x14, 0x10)])
        self.assertEqual(nvn.branch_targets(text)[0x10], 2)


class LoaderPatternTest(unittest.TestCase):
    NAMES = {0x9000: "nvnDeviceInitialize", 0x9020: "nvnQueueSubmit", 0x9040: "nvnQueueFinish"}
    GOT = {"nvnDeviceInitialize": 0xA000, "nvnQueueSubmit": 0xA008, "nvnQueueFinish": 0xA010}

    def build(self):
        w = []
        for addr, name in self.NAMES.items():
            pc = len(w) * 4
            got = self.GOT[name]
            w += [adrp(1, pc, addr), add_imm(1, 1, addr & 0xFFF), MOV_X20_X0, BLR_X19,
                  adrp(8, pc + 16, got), ldr(8, 8, got & 0xFFF), str_(0, 8)]
        w.append(RET)
        # Use site: load the slot pointer of nvnQueueSubmit and call it.
        pc = len(w) * 4
        w += [adrp(8, pc, 0xA008), ldr(8, 8, 0x008), ldr(8, 8, 0), 0xD63F0100, RET]
        return code(w)

    def test_slots_and_uses(self):
        text = self.build()
        refs = list(nvn.iter_adrp_refs(text))
        slots, loader_pcs = nvn.loader_slots(refs, self.NAMES, text)
        self.assertEqual(slots, self.GOT)
        self.assertEqual(len(loader_pcs), 3)
        uses = nvn.slot_uses(refs, slots, loader_pcs)
        self.assertEqual(dict(uses), {"nvnQueueSubmit": 1})

    def test_nvn_names_need_nul_boundaries(self):
        img = b"\0nvnQueueSubmit\0xnvnFoo\0nvnBar\0"
        self.assertEqual(sorted(nvn.nvn_names(img).values()), ["nvnBar", "nvnQueueSubmit"])


class FakeModule:
    def __init__(self, image, text_size, dynamic):
        self.image, self.dynamic, self._text = image, dynamic, text_size

    def seg(self, name):
        assert name == "text"
        return type("Seg", (), {"mem_size": self._text})


class SdkProcTableTest(unittest.TestCase):
    def test_sorted_names_map_to_pointer_run(self):
        names = ["BufferMap", "DeviceInitialize", "QueueSubmit"]
        text = b"\0" * 0x100
        block = b"\xff\xff" + b"\0".join(n.encode() for n in names) + b"\0\x01\x02"
        table_off = len(text) + len(block) + 6
        table_off += (-table_off) % 8
        image = bytearray(text + block)
        image += b"\0" * (table_off - len(image)) + b"\0" * 8 * len(names)
        rela_off = len(image)
        impls = [0x10, 0x40, 0x80]
        for i, impl in enumerate(impls):
            image += struct.pack("<QQq", table_off + 8 * i, nvn.R_RELATIVE, impl)
        mod = FakeModule(bytes(image), 0x100, {ni.DT_RELA: [rela_off], ni.DT_RELASZ: [24 * len(names)]})
        table = nvn.sdk_proc_table(mod, {"nvn" + n for n in names})
        self.assertEqual(table, [("nvnBufferMap", 0x10), ("nvnDeviceInitialize", 0x40), ("nvnQueueSubmit", 0x80)])

    def test_missing_block_raises(self):
        mod = FakeModule(b"\0" * 0x200, 0x100, {})
        with self.assertRaises(ni.FormatError):
            nvn.sdk_proc_table(mod, {"nvnQueueSubmit"})


# --- Ryujinx caches ---------------------------------------------------------------------------

def make_ptc(entries, version=7007):
    body = b"\0" * 16 + struct.pack("<i", len(entries))
    for addr, mode, hq, bl_ in entries:
        body += struct.pack("<QiBB", addr, mode, hq, bl_)
    comp = zlib.compressobj(wbits=-15)
    return rc.PTC_MAGIC + struct.pack("<IB", version, 1) + b"\0" * 16 + comp.compress(body) + comp.flush()


class PtcProfileTest(unittest.TestCase):
    def test_roundtrip(self):
        ver, funcs = rc.parse_ptc_profile(make_ptc([(0x8504000, 2, 1, 0), (0xB907010, 2, 0, 1)]))
        self.assertEqual(ver, 7007)
        self.assertEqual(funcs, {0x8504000: (2, True, False), 0xB907010: (2, False, True)})

    def test_bad_magic(self):
        with self.assertRaises(rc.CacheFormatError):
            rc.parse_ptc_profile(b"XXXX" + make_ptc([])[4:])

    def test_entry_size_mismatch(self):
        raw = make_ptc([(1, 0, 0, 0)])
        body = zlib.decompress(raw[29:], -15)[:-1]  # drop one byte -> pre-blacklist-like size
        comp = zlib.compressobj(wbits=-15)
        with self.assertRaises(rc.CacheFormatError):
            rc.parse_ptc_profile(raw[:29] + comp.compress(body) + comp.flush())


def make_guest_cache(codes, alg):
    toc = struct.pack("<4IQQ", rc.TOC_MAGIC, 0x10001, 0, len(codes), 0, 0)
    data = b""
    for c in codes:
        cb1 = b"\x11" * 4
        toc += struct.pack("<4I", len(data), len(c), len(cb1), 0)
        if alg == 0:
            payload = c
        else:
            z = zlib.compressobj(wbits=-15)
            payload = z.compress(c) + z.flush()
        data += cb1 + bytes([alg]) + payload
    return toc, data


class GuestShaderCacheTest(unittest.TestCase):
    CODES = [b"\x61\x04" + bytes(range(100)), b"\x62\x14" + b"\xaa" * 300]

    def test_uncompressed_and_deflate(self):
        for alg in (0, 1):
            shaders = rc.parse_guest_shader_cache(*make_guest_cache(self.CODES, alg))
            self.assertEqual([s.code for s in shaders], self.CODES)
            self.assertEqual(shaders[1].cb1_size, 4)

    def test_brotli(self):
        try:
            import brotli
        except ImportError:
            self.skipTest("brotli not installed")
        toc = struct.pack("<4IQQ", rc.TOC_MAGIC, 0x10001, 0, 1, 0, 0) + struct.pack("<4I", 0, 4, 0, 0)
        data = b"\x02" + brotli.compress(b"abcd")
        self.assertEqual(rc.parse_guest_shader_cache(toc, data)[0].code, b"abcd")

    def test_bad_magic(self):
        with self.assertRaises(rc.CacheFormatError):
            rc.parse_guest_shader_cache(b"\0" * 32, b"")

    def test_module_bases(self):
        log = ("00:00:00.351 |I| Loader Load: Loading rtld...\n"
               "00:00:00.356 |I| Loader Load: Loading main...\n"
               "00:00:00.777 |I| Loader LoadNsos: Loading image 0 at 0x0000000008500000...\n"
               "00:00:00.778 |I| Loader LoadNsos: Loading image 1 at 0x0000000008504000...\n")
        self.assertEqual(rc.module_bases_from_text(log), {"rtld": 0x8500000, "main": 0x8504000})
        with self.assertRaises(rc.CacheFormatError):
            rc.module_bases_from_text("Loader Load: Loading main...\n")


# --- RomFS containers -------------------------------------------------------------------------

def lz4_literals_only(data):
    out = bytearray()
    n = len(data)
    if n < 15:
        out.append(n << 4)
    else:
        out.append(0xF0)
        rest = n - 15
        while rest >= 255:
            out.append(255)
            rest -= 255
        out.append(rest)
    return bytes(out + data)


def nx_container(magic, payload):
    size = 0x20 + len(payload)
    return magic + b"\0\0\0\0" + b"\0" * 0x14 + struct.pack("<I", size) + payload


def sph_code(stage_type, sph_type, body):
    word0 = sph_type | (3 << 5) | (stage_type << 10)
    return struct.pack("<I", word0) + b"\0" * 0x4C + body


def make_gfpak(files):
    """files: list of (compression, payload)."""
    header_size, table = 0x30, 0x30
    blobs = [lz4_literals_only(p) if comp == 2 else p for comp, p in files]
    data_off = table + 0x18 * len(files)
    out = bytearray(b"GFLXPACK" + struct.pack("<IIIIQQQ", 0x1000, 0, len(files), 0, table, 0, 0))
    out += b"\0" * (header_size - len(out))
    offs = []
    for b in blobs:
        offs.append(data_off)
        data_off += len(b)
    for (comp, p), b, o in zip(files, blobs, offs):
        out += struct.pack("<HHIIIQ", 9, comp, len(p), len(b), 0, o)
    for b in blobs:
        out += b
    return bytes(out)


class ContainerTest(unittest.TestCase):
    def test_gfpak_entries(self):
        pak = make_gfpak([(2, b"hello world, lz4!"), (0, b"raw"), (7, b"oodle?")])
        self.assertEqual(list(si.gfpak_entries(pak)), [(2, b"hello world, lz4!"), (0, b"raw"), (7, None)])
        with self.assertRaises(ni.FormatError):
            list(si.gfpak_entries(b"NOTAPACK" + b"\0" * 64))

    def test_nx_containers_respect_size(self):
        blob = nx_container(b"BNSH", b"x" * 0x10)
        data = b"pad" + blob + b"BNSH\0\0\0\0" + b"\0" * 0x14 + struct.pack("<I", 0xFFFF)  # truncated
        found = list(si.nx_containers(data, b"BNSH"))
        self.assertEqual(found, [(3, blob)])

    def test_stage_offsets(self):
        vs = sph_code(1, 1, b"\x01" * 0x40)
        ps = sph_code(5, 2, b"\x02" * 0x40)
        payload = b"\0" * 0x10 + vs + b"\0" * (0x100 - 0x10 - len(vs)) + ps
        container = nx_container(b"BNSH", payload)
        st = list(si.stage_offsets(container))
        self.assertEqual([s for _, s in st], ["vertex", "pixel"])

    def test_texture_formats(self):
        brti = b"BRTI" + b"\0" * 0x18 + struct.pack("<I", 0x1A01) + b"\0" * 0x10
        self.assertEqual(list(si.texture_formats(nx_container(b"BNTX", brti))), [0x1A01])

    def test_analyze_counts_and_matches(self):
        vs = sph_code(1, 1, b"\x05" * 0x100)
        ps = sph_code(5, 2, b"\x06" * 0x100)
        bnsh_a = nx_container(b"BNSH", b"\0" * 0x10 + vs)
        bnsh_b = nx_container(b"BNSH", b"\0" * 0x10 + ps)
        hidden = sph_code(5, 2, b"\x07" * 0x80)
        with tempfile.TemporaryDirectory() as d:
            os.makedirs(os.path.join(d, "bin"))
            with open(os.path.join(d, "bin", "a.bnsh_vsh"), "wb") as f:
                f.write(bnsh_a)
            with open(os.path.join(d, "bin", "pack.gfpak"), "wb") as f:
                f.write(make_gfpak([(2, bnsh_b), (2, bnsh_a)]))
            with open(os.path.join(d, "bin", "ignored.txt"), "wb") as f:
                f.write(bnsh_b)
            compute = b"\xf0\x07\x20\xe2" + b"\x0c" * 60  # compute shaders carry no SPH
            guest = [rc.GuestShader(0, vs, 0, 0), rc.GuestShader(1, ps, 0, 0),
                     rc.GuestShader(2, hidden, 0, 0), rc.GuestShader(3, sph_code(1, 1, b"\x09" * 8), 0, 0),
                     rc.GuestShader(4, compute, 0, 0)]
            rep = si.analyze(d, {"main": b"junk" + hidden + compute}, guest)
        self.assertEqual(rep["bnsh_unique_total"], 2)
        self.assertEqual(rep["containers"]["gfpak"], {"bnsh": 2, "bnsh_unique": 1})
        self.assertEqual(rep["stages_by_kind"], {"vertex": 1, "pixel": 1})
        self.assertEqual(rep["guest_found_by_source"], {"exefs:main": 2, "bnsh_vsh": 1, "gfpak": 1})
        self.assertEqual(rep["guest_compute_shaders"], 1)
        self.assertEqual(rep["guest_not_found"], 1)
        self.assertIn("BNSH unique: 2", si.to_markdown(rep))


if __name__ == "__main__":
    unittest.main()
