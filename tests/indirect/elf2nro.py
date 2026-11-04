#!/usr/bin/env python3
"""Packs an NRO-shaped AArch64 ELF (three page-aligned PT_LOAD segments: RX, R,
RW+bss, linked at 0 as tests/indirect/guest/link.ld lays it out) into an NRO.

Only for own test programs built with the host clang; homebrew built with
devkitA64 uses elf2nro from devkitPro instead. The ELF must reserve the first
0x80 bytes of .text (branch, MOD0 offset, NRO header), see guest/crt0.S.

Usage: elf2nro.py <in.elf> <out.nro>
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
    if len(segs) != 3 or [s[0] for s in segs] != [5, 4, 6]:
        raise SystemExit("expected three PT_LOAD segments RX, R, RW")
    text, ro, rw = segs
    if text[2] != 0:
        raise SystemExit(".text must be linked at address 0")
    for flags, off, va, filesz, memsz in (text, ro):
        if filesz != memsz:
            raise SystemExit("bss outside the data segment")
    image = bytearray()
    layout = []
    for flags, off, va, filesz, memsz in segs:
        if va % PAGE or va != len(image):
            raise SystemExit("segments must be page aligned and contiguous")
        image += data[off:off + filesz]
        layout.append((va, filesz))
        pad = (-len(image)) % PAGE
        if (flags, off, va, filesz, memsz) != rw:
            image += b"\0" * pad
    bss = rw[4] - rw[3]
    bss = (bss + PAGE - 1) // PAGE * PAGE if bss else 0
    if image[0x10:0x80].strip(b"\0"):
        raise SystemExit("the first 0x80 bytes of .text must be reserved for the NRO header")
    header = struct.pack("<4sIII", b"NRO0", 0, len(image), 0)
    for va, size in layout:
        header += struct.pack("<II", va, size)
    header += struct.pack("<II", bss, 0)
    header += hashlib.sha256(bytes(image[0x80:])).digest()  # module id: any 32 bytes
    header += b"\0" * (0x70 - len(header))
    image[0x10:0x80] = header
    with open(sys.argv[2], "wb") as f:
        f.write(image)


if __name__ == "__main__":
    main()
