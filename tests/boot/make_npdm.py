#!/usr/bin/env python3
"""Writes a minimal NPDM (META + ACI0 with one ThreadInfo kernel capability)
for the boot tests. Field offsets from switchbrew ("NPDM"); no signatures,
no ACID contents, no file system or service access control.

Usage: make_npdm.py <out.npdm> --program-id ID --stack SIZE --priority P --core C
                    [--cores MIN-MAX] [--priorities HIGHEST-LOWEST]
"""
import argparse
import struct


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--program-id", type=lambda s: int(s, 0), required=True)
    ap.add_argument("--stack", type=lambda s: int(s, 0), required=True)
    ap.add_argument("--priority", type=int, required=True)
    ap.add_argument("--core", type=int, required=True)
    ap.add_argument("--cores", default="0-2")
    ap.add_argument("--priorities", default="24-59")
    ap.add_argument("--name", default="Application")
    a = ap.parse_args()
    min_core, max_core = (int(x) for x in a.cores.split("-"))
    highest, lowest = (int(x) for x in a.priorities.split("-"))
    # ThreadInfo: pattern 0b0111, lowest priority (numerically largest) at
    # bit 4, highest at bit 10, min core at bit 16, max core at bit 24.
    thread_info = 0x7 | (lowest << 4) | (highest << 10) | (min_core << 16) | (max_core << 24)
    kc = struct.pack("<I", thread_info)

    aci = bytearray(0x40)
    aci[0:4] = b"ACI0"
    struct.pack_into("<Q", aci, 0x10, a.program_id)
    struct.pack_into("<IIIIII", aci, 0x20, 0x40, 0, 0x40, 0, 0x40, len(kc))
    aci += kc

    meta = bytearray(0x80)
    meta[0:4] = b"META"
    meta[0x0C] = 1 | (3 << 1)  # 64-bit, 39-bit address space
    meta[0x0E] = a.priority
    meta[0x0F] = a.core
    struct.pack_into("<I", meta, 0x1C, a.stack)
    name = a.name.encode()[:0x10]
    meta[0x20:0x20 + len(name)] = name
    struct.pack_into("<IIII", meta, 0x70, 0x80, len(aci), 0, 0)
    with open(a.out, "wb") as f:
        f.write(meta + aci)


if __name__ == "__main__":
    main()
