"""Unit tests for tools/analyze/nso_inventory.py using synthetic NSOs only (no game data).

Run: python -m unittest discover -s tests/tools -v
"""
import hashlib
import io
import os
import random
import struct
import sys
import tempfile
import unittest
from contextlib import redirect_stderr

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "tools", "analyze"))
import nso_inventory as inv  # noqa: E402

DT_RELA, DT_RELASZ = 7, 8
DT_RELRSZ, DT_RELR = 35, 36
DT_HASH, DT_STRTAB, DT_SYMTAB, DT_STRSZ = 4, 5, 6, 10
DT_GNU_HASH = 0x6FFFFEF5
R_AARCH64_RELATIVE, R_AARCH64_GLOB_DAT = 1027, 1025
STB_WEAK = 2
STB_GLOBAL, STT_FUNC = 1, 2


def lz4_literals_only(data: bytes) -> bytes:
    """Minimal valid LZ4 block: a single literal-only sequence."""
    n = len(data)
    if n < 15:
        return bytes([n << 4]) + data
    out = bytearray([0xF0])
    rest = n - 15
    while rest >= 255:
        out.append(255)
        rest -= 255
    out.append(rest)
    return bytes(out) + data


def make_dynsym(defined: list[str], undefined: list[str]) -> tuple[bytes, bytes]:
    strtab = bytearray(b"\0")
    syms = bytearray(b"\0" * 24)  # null symbol
    for name, shndx in [(n, 1) for n in defined] + [(n, 0) for n in undefined]:
        off = len(strtab)
        strtab += name.encode() + b"\0"
        syms += struct.pack("<IBBHQQ", off, (STB_GLOBAL << 4) | STT_FUNC, 0, shndx, 0x40 if shndx else 0, 4)
    return bytes(syms), bytes(strtab)


def make_nso(defined: list[str], undefined: list[str], compress: bool = False,
             module_id: bytes = bytes(range(1, 21)), api_info: bytes = b"SDK MW+Test+Lib-1_0\0") -> bytes:
    """Build a tiny NSO: .text holds MOD0, .ro holds path/api_info/dynsym, .data holds .dynamic + one RELA."""
    seg_size = 0x200
    text = bytearray(seg_size)
    ro = bytearray(seg_size)
    data = bytearray(seg_size)
    text_off, ro_off, data_off = 0, seg_size, 2 * seg_size

    # MOD0 at text+0x10; .dynamic at data+0x0; one RELA entry at data+0x100.
    struct.pack_into("<I", text, 4, 0x10)
    dyn_abs = data_off
    struct.pack_into("<4s5i", text, 0x10, b"MOD0", dyn_abs - 0x10, 0, 0, 0, 0)
    rela_abs = data_off + 0x100
    for k, (tag, val) in enumerate([(DT_RELA, rela_abs), (DT_RELASZ, 24), (0, 0)]):
        struct.pack_into("<qQ", data, k * 16, tag, val)
    struct.pack_into("<QQq", data, 0x100, 0x20, R_AARCH64_RELATIVE, 0x40)

    # .ro: module path header, api_info, dynstr, dynsym.
    path = b"C:/somewhere/fixture.nss\0"
    struct.pack_into("<II", ro, 0, 0, len(path))
    ro[8:8 + len(path)] = path
    api_off = 0x40
    ro[api_off:api_off + len(api_info)] = api_info
    syms, strtab = make_dynsym(defined, undefined)
    str_off = 0x80
    ro[str_off:str_off + len(strtab)] = strtab
    sym_off = 0x100
    ro[sym_off:sym_off + len(syms)] = syms
    assert sym_off + len(syms) <= seg_size and str_off + len(strtab) <= sym_off

    segs = [bytes(text), bytes(ro), bytes(data)]
    payloads = [lz4_literals_only(s) if compress else s for s in segs]
    hdr = bytearray(0x100)
    hdr[0:4] = b"NSO0"
    flags = (0b111 if compress else 0) | 0b111000
    struct.pack_into("<I", hdr, 0x0C, flags)
    file_off = 0x100
    for idx, (mem_off, payload) in enumerate(zip((text_off, ro_off, data_off), payloads)):
        struct.pack_into("<3I", hdr, 0x10 + idx * 0x10, file_off, mem_off, seg_size)
        struct.pack_into("<I", hdr, 0x60 + idx * 4, len(payload))
        hdr[0xA0 + idx * 0x20:0xC0 + idx * 0x20] = hashlib.sha256(segs[idx]).digest()
        file_off += len(payload)
    struct.pack_into("<I", hdr, 0x3C, 0x1000)  # bss
    hdr[0x40:0x40 + len(module_id)] = module_id
    struct.pack_into("<6I", hdr, 0x88, api_off, len(api_info), str_off, len(strtab), sym_off, len(syms))
    return bytes(hdr) + b"".join(payloads)


class Lz4Tests(unittest.TestCase):
    def test_overlapping_match(self):
        # literals "abc", match offset 3 len 9, then final literal "X"
        block = bytes([0x35]) + b"abc" + bytes([3, 0]) + bytes([0x10]) + b"X"
        self.assertEqual(inv.lz4_block_decompress(block, 13), b"abcabcabcabcX")

    def test_long_literal_run(self):
        data = bytes(range(256)) * 3
        self.assertEqual(inv.lz4_block_decompress(lz4_literals_only(data), len(data)), data)

    def test_size_mismatch_raises(self):
        with self.assertRaises(inv.FormatError):
            inv.lz4_block_decompress(lz4_literals_only(b"abcd"), 5)

    def test_bad_offset_raises(self):
        with self.assertRaises(inv.FormatError):
            inv.lz4_block_decompress(bytes([0x10]) + b"a" + bytes([5, 0]), 10)


class NsoParseTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)

    def write(self, name, blob):
        p = os.path.join(self.tmp.name, name)
        with open(p, "wb") as f:
            f.write(blob)
        return p

    def test_parse_uncompressed_and_compressed_agree(self):
        for compress in (False, True):
            with self.subTest(compress=compress):
                mod = inv.parse_nso(self.write("main", make_nso(["nnMain"], ["malloc"], compress)), "main")
                self.assertTrue(mod.hashes_ok)
                self.assertEqual(mod.ro_path_basename, "fixture.nss")
                self.assertEqual(mod.api_info, ["SDK MW+Test+Lib-1_0"])
                self.assertEqual([s.name for s in mod.symbols], ["", "nnMain", "malloc"])
                self.assertEqual(mod.reloc_counts["RELATIVE"], 1)
                self.assertEqual(mod.image_size, 0x600 + 0x1000)

    def test_hash_mismatch_detected(self):
        blob = bytearray(make_nso(["a"], []))
        blob[0xA0] ^= 0xFF
        self.assertFalse(inv.parse_nso(self.write("main", bytes(blob))).hashes_ok)

    def test_rejects_non_nso(self):
        with self.assertRaises(inv.FormatError):
            inv.parse_nso(self.write("main", b"NCA3" + b"\0" * 0x200))

    def test_build_id_trimming(self):
        self.assertEqual(inv._trim_zero_bytes("12ca60" + "00" * 13), "12ca60")
        self.assertEqual(inv._trim_zero_bytes("ab" + "00" * 15), "ab")


class InventoryTests(unittest.TestCase):
    def test_cross_module_resolution_follows_load_order(self):
        with tempfile.TemporaryDirectory() as d:
            # main and sdk both define 'dup'; rtld order says main wins.
            files = {
                "main": make_nso(["nnMain", "malloc", "dup"], ["nn_os_Lock", "glslcCompile"]),
                "subsdk0": make_nso(["glslcCompile"], ["malloc"]),
                "sdk": make_nso(["nn_os_Lock", "dup"], ["nnMain", "dup", "missing_strong"]),
            }
            for name, blob in files.items():
                with open(os.path.join(d, name), "wb") as f:
                    f.write(blob)
            result = inv.build_inventory(d)
        edges = result["links"]["edges"]
        self.assertEqual(edges["main"], {"sdk": 1, "subsdk0": 1})
        self.assertEqual(edges["subsdk0"], {"main": 1})
        self.assertEqual(edges["sdk"], {"main": 2})
        self.assertEqual(result["links"]["unresolved"]["sdk"]["strong"], 1)
        names = [m["name"] for m in result["modules"]]
        self.assertEqual(names, ["main", "subsdk0", "sdk"])
        self.assertEqual(result["modules"][0]["module_id"], bytes(range(1, 21)).hex())

    def test_npdm_services(self):
        aci = bytearray(0x40)
        aci[0:4] = b"ACI0"
        struct.pack_into("<Q", aci, 0x10, 0x0100000000001234)
        sac = bytes([0x01]) + b"sm" + bytes([0x82]) + b"hid"  # len-1 in low bits, 0x80 = host
        struct.pack_into("<II", aci, 0x28, len(aci), len(sac))
        struct.pack_into("<II", aci, 0x30, 0, 0)  # KAC must not be read as SAC
        aci += sac
        meta = bytearray(0x80)
        meta[0:4] = b"META"
        meta[0x0C] = 1
        meta[0x0E] = 44
        struct.pack_into("<I", meta, 0x1C, 0x100000)
        meta[0x20:0x20 + 11] = b"Application"
        struct.pack_into("<II", meta, 0x70, len(meta), len(aci))
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "main.npdm")
            with open(p, "wb") as f:
                f.write(bytes(meta) + bytes(aci))
            out = inv.parse_npdm_summary(p)
        self.assertEqual(out["services"], ["host:hid", "sm"])
        self.assertEqual(out["program_id"], "0100000000001234")
        self.assertEqual(out["main_thread_stack_size"], 0x100000)

    def test_namespace_buckets(self):
        self.assertEqual(inv.namespace_of("_ZN2nn2os9LockMutexEPNS0_9MutexTypeE"), "nn::os")
        self.assertEqual(inv.namespace_of("_ZN5movie6BufferC1Ev"), "movie")
        self.assertEqual(inv.namespace_of("nvnBootstrapLoader"), "nvn")
        self.assertEqual(inv.namespace_of("malloc"), "libc/other")


def make_nro(defined: list[str], undefined: list[str], hash_style: str = "sysv", relr: bool = False) -> bytes:
    """Build a tiny NRO the way elf2nro lays it out: header inside .text, symbols only via .dynamic."""
    seg = 0x200
    text, ro, data = bytearray(seg), bytearray(seg), bytearray(seg)
    ro_abs, data_abs = seg, 2 * seg

    syms, strtab = make_dynsym(defined, undefined)
    nsyms = len(syms) // 24
    ro[0:len(syms)] = syms
    ro[0x100:0x100 + len(strtab)] = strtab
    hash_abs = ro_abs + 0x180
    if hash_style == "sysv":  # nbucket, nchain, bucket[1], chain[nsyms]
        table = struct.pack(f"<3I{nsyms}I", 1, nsyms, 1, *([0] * nsyms))
        hash_tag = DT_HASH
    else:  # gnu: nbuckets, symoffset, bloom_size, shift, bloom[1], bucket[1], chain[nsyms-1]
        chain = [0] * (nsyms - 2) + [1]  # bit 0 marks the end of the bucket's chain
        table = struct.pack(f"<4IQI{len(chain)}I", 1, 1, 1, 0, 0, 1, *chain)
        hash_tag = DT_GNU_HASH
    ro[0x180:0x180 + len(table)] = table
    assert 0x180 + len(table) <= seg

    dyn = [(DT_SYMTAB, ro_abs), (DT_STRTAB, ro_abs + 0x100), (DT_STRSZ, len(strtab)), (hash_tag, hash_abs),
           (DT_RELA, data_abs + 0x100), (DT_RELASZ, 48)]
    if relr:  # one address entry + one bitmap entry with three bits set -> 4 relative relocations
        dyn += [(DT_RELR, data_abs + 0x180), (DT_RELRSZ, 16)]
        struct.pack_into("<QQ", data, 0x180, data_abs + 0x40, (1 << 1) | (1 << 2) | (1 << 5) | 1)
    dyn.append((0, 0))
    for k, (tag, val) in enumerate(dyn):
        struct.pack_into("<qQ", data, k * 16, tag, val)
    struct.pack_into("<QQq", data, 0x100, 0x20, R_AARCH64_RELATIVE, 0x40)
    struct.pack_into("<QQq", data, 0x118, 0x28, (1 << 32) | R_AARCH64_GLOB_DAT, 0)

    # MOD0 at text+0x80, right after the NRO header.
    struct.pack_into("<4s5i", text, 0x80, b"MOD0", data_abs - 0x80, 0, 0, 0, 0)
    struct.pack_into("<I", text, 4, 0x80)
    text[0x10:0x14] = b"NRO0"
    struct.pack_into("<3I", text, 0x14, 0, 3 * seg, 0)
    for idx in range(3):
        struct.pack_into("<II", text, 0x20 + idx * 8, idx * seg, seg)
    struct.pack_into("<I", text, 0x38, 0x800)
    text[0x40:0x54] = bytes(range(0xA0, 0xB4))
    return bytes(text) + bytes(ro) + bytes(data) + b"ASET" + b"\0" * 0x20  # trailing homebrew assets


class NroParseTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)

    def write(self, name, blob):
        p = os.path.join(self.tmp.name, name)
        with open(p, "wb") as f:
            f.write(blob)
        return p

    def test_symbols_from_dynamic_with_both_hash_styles(self):
        for style in ("sysv", "gnu"):
            with self.subTest(hash_style=style):
                p = self.write("fixture.nro", make_nro(["nro_main", "helper"], ["ext_import"], style))
                mod = inv.load_module(p)
                self.assertEqual([s.name for s in mod.symbols], ["", "nro_main", "helper", "ext_import"])
                self.assertEqual(mod.reloc_counts, {"RELATIVE": 1, "GLOB_DAT": 1})
                self.assertEqual(mod.module_id[:40], bytes(range(0xA0, 0xB4)).hex())
                self.assertEqual(mod.image_size, 0x600 + 0x800)
                self.assertEqual(len(mod.image), 0x600)  # asset section not part of the image

    def test_relr_relocations_counted(self):
        mod = inv.load_module(self.write("relr.nro", make_nro(["a"], [], relr=True)))
        self.assertEqual(mod.reloc_counts, {"RELATIVE": 1, "GLOB_DAT": 1, "RELR": 4})

    def test_nro_size_beyond_file_rejected(self):
        blob = bytearray(make_nro(["a"], []))
        struct.pack_into("<I", blob, 0x18, len(blob) + 0x1000)
        with self.assertRaises(inv.FormatError):
            inv.load_module(self.write("bad.nro", bytes(blob)))

    def test_unknown_magic_rejected(self):
        with self.assertRaises(inv.FormatError):
            inv.load_module(self.write("x.bin", b"\0" * 0x100))


class RobustnessTests(unittest.TestCase):
    """Malformed input must end in FormatError, never in IndexError/struct.error/MemoryError."""

    def test_huge_segment_size_rejected_without_allocating(self):
        blob = bytearray(make_nso(["a"], []))
        struct.pack_into("<I", blob, 0x10 + 2 * 0x10 + 8, 0xFFFFFF00)  # .data mem_size
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "main")
            with open(p, "wb") as f:
                f.write(blob)
            with self.assertRaises(inv.FormatError):
                inv.parse_nso(p)

    def test_random_mutations_and_truncations(self):
        rng = random.Random(0x5EED)
        bases = {"main": make_nso(["nnMain"], ["malloc"], compress=True), "f.nro": make_nro(["e"], ["w"], "gnu")}
        with tempfile.TemporaryDirectory() as d:
            for name, base in bases.items():
                p = os.path.join(d, name)
                for _ in range(400):
                    blob = bytearray(base)
                    if rng.random() < 0.2:
                        blob = blob[:rng.randrange(0x14, len(blob))]
                    else:
                        hot = 0x100 if rng.random() < 0.6 else len(blob)  # mostly hit the header
                        for _ in range(rng.randint(1, 8)):
                            blob[rng.randrange(0x14, min(hot, len(blob)))] = rng.randrange(256)
                    with open(p, "wb") as f:
                        f.write(blob)
                    try:
                        inv.load_module(p)
                    except inv.FormatError:
                        pass


class ImportListTests(unittest.TestCase):
    def test_rows_and_tsv(self):
        with tempfile.TemporaryDirectory() as d:
            for name, blob in {
                "main": make_nso(["nnMain"], ["nn_os_Lock", "glslcCompile"]),
                "subsdk0": make_nso(["glslcCompile"], []),
                "sdk": make_nso(["nn_os_Lock"], ["nnMain", "missing"]),
            }.items():
                with open(os.path.join(d, name), "wb") as f:
                    f.write(blob)
            rows = inv.import_rows(inv.load_exefs(d))
            tsv = os.path.join(d, "imports.tsv")
            inv.write_imports_tsv(rows, tsv)
            with open(tsv, encoding="utf-8") as f:
                lines = f.read().splitlines()
        got = {(r["importer"], r["symbol"]): r["provider"] for r in rows}
        self.assertEqual(got, {("main", "nn_os_Lock"): "sdk", ("main", "glslcCompile"): "subsdk0",
                               ("sdk", "nnMain"): "main", ("sdk", "missing"): ""})
        self.assertEqual(lines[0], "importer\tsymbol\tbind\ttype\tprovider")
        self.assertIn("main\tglslcCompile\tglobal\tfunc\tsubsdk0", lines)
        self.assertEqual(len(lines), 5)

    def test_cli_on_single_nro(self):
        with tempfile.TemporaryDirectory() as d:
            nro = os.path.join(d, "fixture.nro")
            with open(nro, "wb") as f:
                f.write(make_nro(["nro_main"], ["ext_import"]))
            tsv, md = os.path.join(d, "imports.tsv"), os.path.join(d, "out.md")
            self.assertEqual(inv.main([nro, "--imports", tsv, "--markdown", md]), 0)
            with open(tsv, encoding="utf-8") as f:
                self.assertEqual(f.read().splitlines()[1], "fixture.nro\text_import\tglobal\tfunc\t")
            with open(md, encoding="utf-8") as f:
                self.assertIn("| fixture.nro |", f.read())
            bad = os.path.join(d, "bad.nro")
            with open(bad, "wb") as f:
                f.write(b"junk")
            with redirect_stderr(io.StringIO()):
                self.assertEqual(inv.main([bad]), 2)


if __name__ == "__main__":
    unittest.main()
