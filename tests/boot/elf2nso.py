#!/usr/bin/env python3
"""Packs a module-shaped AArch64 ELF (three page-aligned PT_LOAD segments RX,
R, RW+bss from address 0, as tests/boot/guest/module.ld lays it out) into an
uncompressed NSO. Only for own test modules built with the host clang;
homebrew uses devkitPro's elf2nso.

Layout (switchbrew "NSO"): 0x100-byte header, then the three segments. The
module id is the first 32 bytes of the ELF's SHA-256 (any stable value).

Usage: elf2nso.py <in.elf> <out.nso>
"""
import hashlib
import struct
import sys

PT_LOAD = 1
PAGE = 0x1000


def load_segments(data):
    if data[:4] != b"\x7fELF" or data[4] != 2 or data[5] != 1:
        raise SystemExit("not a little-endian ELF64 file")
    (e_phoff,) = struct.unpack_from("<Q", data, 0x20)
    e_phentsize, e_phnum = struct.unpack_from("<HH", data, 0x36)
    segs = []
    for i in range(e_phnum):
        p_type, p_flags, p_offset, p_vaddr, _, p_filesz, p_memsz, _ = struct.unpack_from(
            "<IIQQQQQQ", data, e_phoff + i * e_phentsize)
        if p_type == PT_LOAD:
            segs.append((p_flags, p_offset, p_vaddr, p_filesz, p_memsz))
    return segs


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    data = open(sys.argv[1], "rb").read()
    segs = load_segments(data)
    if [s[0] for s in segs] != [5, 4, 6]:
        raise SystemExit("expected three PT_LOAD segments RX, R, RW")
    if segs[0][2] != 0:
        raise SystemExit(".text must be linked at address 0")
    header = bytearray(0x100)
    header[0:4] = b"NSO0"
    payload = bytearray()
    end = 0
    for i, (flags, off, va, filesz, memsz) in enumerate(segs):
        if va % PAGE or va < end:
            raise SystemExit("segments must be page aligned and ascending")
        if i < 2 and filesz != memsz:
            raise SystemExit("bss outside the data segment")
        seg = data[off:off + filesz]
        struct.pack_into("<III", header, 0x10 + i * 0x10, 0x100 + len(payload), va, filesz)
        struct.pack_into("<I", header, 0x60 + i * 4, filesz)
        header[0xA0 + i * 0x20:0xC0 + i * 0x20] = hashlib.sha256(seg).digest()
        payload += seg
        end = va + filesz
    rw = segs[2]
    bss = rw[4] - rw[3]
    struct.pack_into("<I", header, 0x3C, (bss + PAGE - 1) // PAGE * PAGE if bss else 0)
    header[0x40:0x60] = hashlib.sha256(data).digest()
    with open(sys.argv[2], "wb") as f:
        f.write(header + payload)


if __name__ == "__main__":
    main()
