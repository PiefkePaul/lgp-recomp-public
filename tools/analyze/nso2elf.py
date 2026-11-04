#!/usr/bin/env python3
"""Convert an NSO/NRO module into a section-less ELF64 that Ghidra's ELF loader understands,
plus a sidecar JSON with the .eh_frame function table (FDE starts, sizes, LSDA flags).

Ghidra 12 has no built-in NSO loader. The decompressed module image already contains a
standard .dynamic table (found via MOD0), so wrapping it in program headers is enough:
Ghidra then applies the RELA/JMPREL relocations and names the dynamic symbols itself.

Layout of the output ELF (ET_DYN, EM_AARCH64, module base 0):
    PT_LOAD .text R-X, PT_LOAD .ro R--, PT_LOAD .data+.bss RW-, PT_DYNAMIC, PT_GNU_EH_FRAME
File offset = 0x1000 + memory offset, so virtual addresses equal module offsets.

Inputs are the user's locally extracted modules; outputs belong in $LGP_PRIVATE_DIR, never in
the repository (they contain game code).

Usage:
    python -I tools/analyze/nso2elf.py <module.nso|module.nro> <out.elf> [--meta out.json]
"""
from __future__ import annotations

import argparse
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nso_inventory as inv  # noqa: E402

PT_LOAD, PT_DYNAMIC, PT_GNU_EH_FRAME = 1, 2, 0x6474E550
PF_X, PF_W, PF_R = 1, 2, 4
EM_AARCH64, ET_DYN = 183, 3
FILE_BASE = 0x1000


class EhFrameError(inv.FormatError):
    pass


def _uleb(buf: bytes, i: int) -> tuple[int, int]:
    val = shift = 0
    while True:
        b = buf[i]
        i += 1
        val |= (b & 0x7F) << shift
        shift += 7
        if not b & 0x80:
            return val, i


def _sleb(buf: bytes, i: int) -> tuple[int, int]:
    val = shift = 0
    while True:
        b = buf[i]
        i += 1
        val |= (b & 0x7F) << shift
        shift += 7
        if not b & 0x80:
            if b & 0x40:
                val -= 1 << shift
            return val, i


_ENC_FORMATS = {0x00: "<Q", 0x02: "<H", 0x03: "<I", 0x04: "<Q", 0x0A: "<h", 0x0B: "<i", 0x0C: "<q"}


def _read_encoded(buf: bytes, i: int, enc: int, datarel_base: int = 0) -> tuple[int, int]:
    """Read a DW_EH_PE-encoded pointer at image offset i; returns (value, next offset)."""
    if enc == 0xFF:  # DW_EH_PE_omit
        return 0, i
    fmt = _ENC_FORMATS.get(enc & 0x0F)
    if fmt is None:
        raise EhFrameError(f"unsupported pointer encoding 0x{enc:02x}")
    raw = struct.unpack_from(fmt, buf, i)[0]
    app = enc & 0x70
    if app == 0x10:  # pcrel
        raw += i
    elif app == 0x30:  # datarel
        raw += datarel_base
    elif app != 0x00:
        raise EhFrameError(f"unsupported pointer application 0x{app:02x}")
    return raw & 0xFFFFFFFFFFFFFFFF, i + struct.calcsize(fmt)


def _parse_cie(img: bytes, off: int) -> dict:
    length = struct.unpack_from("<I", img, off)[0]
    if length == 0xFFFFFFFF:
        raise EhFrameError("64-bit DWARF CIE not supported")
    i = off + 8  # skip length + CIE id
    version = img[i]
    i += 1
    end = img.index(b"\0", i)
    aug = img[i:end].decode("ascii", "replace")
    i = end + 1
    _, i = _uleb(img, i)  # code alignment
    _, i = _sleb(img, i)  # data alignment
    if version == 1:
        i += 1
    else:
        _, i = _uleb(img, i)
    cie = {"aug": aug, "fde_enc": 0x00, "lsda_enc": 0xFF, "personality": False}
    if aug.startswith("z"):
        _, i = _uleb(img, i)
        for ch in aug[1:]:
            if ch == "R":
                cie["fde_enc"] = img[i]
                i += 1
            elif ch == "L":
                cie["lsda_enc"] = img[i]
                i += 1
            elif ch == "P":
                penc = img[i]
                _, i = _read_encoded(img, i + 1, penc & 0x7F)
                cie["personality"] = True
            elif ch in "SB":
                pass
            else:
                raise EhFrameError(f"unknown CIE augmentation {aug!r}")
    return cie


def parse_eh_frame(mod: inv.NsoModule) -> list[dict]:
    """FDE list via .eh_frame_hdr (MOD0): start, size, has_lsda, has_personality per function."""
    img = mod.image
    mod0 = struct.unpack_from("<I", img, 4)[0]
    if mod0 + 0x1C > len(img) or img[mod0:mod0 + 4] != b"MOD0":
        return []
    _dyn, _bs, _be, eh_s, eh_e = struct.unpack_from("<5i", img, mod0 + 4)
    if eh_e <= eh_s:
        return []
    hdr = mod0 + eh_s
    if img[hdr] != 1:
        raise EhFrameError(f".eh_frame_hdr version {img[hdr]}")
    ptr_enc, cnt_enc, tab_enc = img[hdr + 1], img[hdr + 2], img[hdr + 3]
    _eh_frame, i = _read_encoded(img, hdr + 4, ptr_enc, hdr)
    count, i = _read_encoded(img, i, cnt_enc, hdr)
    if tab_enc != 0x3B:  # datarel | sdata4, what every toolchain emits
        raise EhFrameError(f"unsupported .eh_frame_hdr table encoding 0x{tab_enc:02x}")
    cies: dict[int, dict] = {}
    out = []
    for k in range(count):
        loc, fde = struct.unpack_from("<ii", img, i + k * 8)
        loc, fde = loc + hdr, fde + hdr
        cie_ptr = struct.unpack_from("<I", img, fde + 4)[0]
        cie_off = fde + 4 - cie_ptr
        cie = cies.get(cie_off) or cies.setdefault(cie_off, _parse_cie(img, cie_off))
        j = fde + 8
        start, j = _read_encoded(img, j, cie["fde_enc"])
        size, j = _read_encoded(img, j, cie["fde_enc"] & 0x0F)
        lsda = 0
        if cie["aug"].startswith("z"):
            _, j = _uleb(img, j)
            if cie["lsda_enc"] != 0xFF:
                lsda, j = _read_encoded(img, j, cie["lsda_enc"])
        if start != loc:
            raise EhFrameError(f"FDE start 0x{start:x} disagrees with table entry 0x{loc:x}")
        out.append({"start": start, "size": size, "lsda": lsda != 0, "personality": cie["personality"]})
    return out


def build_elf(mod: inv.NsoModule) -> bytes:
    text, ro, data = mod.seg("text"), mod.seg("ro"), mod.seg("data")
    phdrs = [
        (PT_LOAD, PF_R | PF_X, text.mem_offset, text.mem_size, text.mem_size, 0x1000),
        (PT_LOAD, PF_R, ro.mem_offset, ro.mem_size, ro.mem_size, 0x1000),
        (PT_LOAD, PF_R | PF_W, data.mem_offset, data.mem_size, data.mem_size + mod.bss_size, 0x1000),
    ]
    mod0 = struct.unpack_from("<I", mod.image, 4)[0]
    if mod.image[mod0:mod0 + 4] == b"MOD0":
        dyn_rel, _bs, _be, eh_s, eh_e = struct.unpack_from("<5i", mod.image, mod0 + 4)
        dyn = mod0 + dyn_rel
        dyn_size = 16 * (sum(len(v) for v in mod.dynamic.values()) + 1)
        phdrs.append((PT_DYNAMIC, PF_R | PF_W, dyn, dyn_size, dyn_size, 8))
        if eh_e > eh_s:
            phdrs.append((PT_GNU_EH_FRAME, PF_R, mod0 + eh_s, eh_e - eh_s, eh_e - eh_s, 4))
    phoff = 0x40
    ehdr = bytearray(0x40)
    ehdr[0:16] = b"\x7fELF" + bytes([2, 1, 1, 0]) + bytes(8)
    struct.pack_into("<HHIQQQIHHHHHH", ehdr, 16, ET_DYN, EM_AARCH64, 1, 0, phoff, 0, 0,
                     0x40, 56, len(phdrs), 64, 0, 0)
    ph = bytearray()
    for p_type, flags, vaddr, filesz, memsz, align in phdrs:
        ph += struct.pack("<IIQQQQQQ", p_type, flags, FILE_BASE + vaddr, vaddr, vaddr, filesz, memsz, align)
    head = bytes(ehdr) + bytes(ph)
    if len(head) > FILE_BASE:
        raise inv.FormatError("program headers do not fit before FILE_BASE")
    return head + bytes(FILE_BASE - len(head)) + mod.image


def build_meta(mod: inv.NsoModule, fdes: list[dict]) -> dict:
    return {
        "module": mod.name,
        "module_id": mod.module_id,
        "segments": {s.name: [s.mem_offset, s.mem_size] for s in mod.segments},
        "bss_size": mod.bss_size,
        "fde_count": len(fdes),
        "fde_with_lsda": sum(f["lsda"] for f in fdes),
        "fde_with_personality": sum(f["personality"] for f in fdes),
        "fdes": [[f["start"], f["size"], int(f["lsda"])] for f in fdes],
    }


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=(__doc__ or "").split("\n\n")[0])
    ap.add_argument("module")
    ap.add_argument("out_elf")
    ap.add_argument("--meta", help="write FDE table and segment layout as JSON")
    args = ap.parse_args(argv)
    try:
        mod = inv.load_module(args.module)
        elf = build_elf(mod)
        fdes = parse_eh_frame(mod)
    except (inv.FormatError, struct.error, IndexError, ValueError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    with open(args.out_elf, "wb") as f:
        f.write(elf)
    if args.meta:
        with open(args.meta, "w", encoding="utf-8") as f:
            json.dump(build_meta(mod, fdes), f)
    print(f"{mod.name}: {len(elf)} bytes ELF, {len(fdes)} FDEs "
          f"({sum(x['lsda'] for x in fdes)} with LSDA)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
