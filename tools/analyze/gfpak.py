#!/usr/bin/env python3
"""List and unpack GFLXPACK archives (.gfpak) of Let's Go.

The archive keeps no file names, only 64-bit path hashes. This tool numbers
the files and shows their magic, so a file can be found by its content:

    gfpak.py list <archive.gfpak> [--grep TEXT]
    gfpak.py extract <archive.gfpak> --out DIR [--index N[,N…]] [--grep TEXT]

--grep keeps only files whose (unpacked) bytes contain TEXT, for example an
animation name. Unpacked files are written as DIR/<index>.<ext> (ext from the
magic, otherwise bin). Only LZ4 block (2) and uncompressed (0) files can be
unpacked. Archives and their files are game data and stay in $LGP_PRIVATE_DIR;
the listing names no paths.
"""
import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import shader_inventory as si  # noqa: E402

MAGIC_EXT = {b"BNSH": "bnsh", b"BNTX": "bntx", b"GFLXPACK": "gfpak", b"SESD": "bseq"}


def path_hashes(data):
    """The u64 path hash of each file (the hash table holds one per file)."""
    _ver, _rel, count, _folders, _files, hashes = struct.unpack_from("<IIIIQQ", data, 8)
    if hashes == 0 or hashes + 8 * count > len(data):
        return [None] * count
    return list(struct.unpack_from(f"<{count}Q", data, hashes))


def entries(data):
    """(index, compression, payload or None, path hash) of each file."""
    hashes = path_hashes(data)
    for i, (comp, payload) in enumerate(si.gfpak_entries(data)):
        yield i, comp, payload, hashes[i] if i < len(hashes) else None


def extension(payload):
    for magic, ext in MAGIC_EXT.items():
        if payload.startswith(magic):
            return ext
    return "bin"


def selected(args, data):
    want = None
    if args.index:
        want = {int(x, 0) for x in args.index.split(",")}
    needle = args.grep.encode("utf-8") if args.grep else None
    for i, comp, payload, h in entries(data):
        if want is not None and i not in want:
            continue
        if needle is not None and (payload is None or needle not in payload):
            continue
        yield i, comp, payload, h


def cmd_list(args):
    with open(args.archive, "rb") as f:
        data = f.read()
    n = 0
    for i, comp, payload, h in selected(args, data):
        n += 1
        if payload is None:
            print(f"{i:5} compression {comp} (not unpacked)")
            continue
        hs = f"hash {h:016x} " if h is not None else ""
        print(f"{i:5} {hs}compression {comp} {len(payload):9} B {extension(payload)} magic {payload[:8].hex()}")
    print(f"{n} files")
    return 0


def cmd_extract(args):
    with open(args.archive, "rb") as f:
        data = f.read()
    os.makedirs(args.out, exist_ok=True)
    n = skipped = 0
    for i, comp, payload, _h in selected(args, data):
        if payload is None:
            skipped += 1
            continue
        with open(os.path.join(args.out, f"{i:05}.{extension(payload)}"), "wb") as f:
            f.write(payload)
        n += 1
    print(f"wrote {n} files" + (f", {skipped} with unknown compression left out" if skipped else ""))
    return 0


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = p.add_subparsers(dest="command", required=True)
    for name, fn in (("list", cmd_list), ("extract", cmd_extract)):
        s = sub.add_parser(name)
        s.add_argument("archive")
        s.add_argument("--index", help="only these files (comma-separated)")
        s.add_argument("--grep", help="only files containing this text")
        if name == "extract":
            s.add_argument("--out", required=True)
        s.set_defaults(fn=fn)
    args = p.parse_args(argv)
    try:
        return args.fn(args)
    except (OSError, ValueError, struct.error, si.ni.FormatError) as e:
        print(f"gfpak: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
