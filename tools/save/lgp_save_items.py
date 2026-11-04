#!/usr/bin/env python3
"""Show or change the catch pouch (balls, berries) of a Let's Go savedata.bin (test copies).

  lgp_save_items.py show SAVE
  lgp_save_items.py set SAVE --out NEW --item ID=COUNT [--item ID=COUNT ...]

`set` writes a new file and never touches SAVE: an item already in the pouch
gets COUNT (1-999), a new one goes to the first free entry, COUNT 0 removes it
and the entries after it move up (the game keeps the list packed). Ball ids:
1 Master, 2 Ultra, 3 Great, 4 Poke, 12 Premier.

Format, as described by the public PKHeX sources (read for the layout only, no
code taken over): block 0 at 0x0, 0xD90 bytes, holds the pouches; the catch
pouch is 50 u32 entries at 0x818. Per entry: bits 0-14 item id, bits 15-24
count, bit 30 "new"; the other bits are kept as they are. Block 0's checksum
in the block table (CRC-16/ARC, lgp_save_party.py) is updated, nothing else
changes.

Saves are the player's own data: run this only inside LGP_PRIVATE_DIR, never
commit inputs or outputs.
"""
import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from lgp_save_party import BLOCKS, SaveError, check_layout, fix_checksum  # noqa: E402

ITEM_BLOCK = 0
CATCH_POUCH = 0x818
POUCH_SIZE = 50
ID_MASK = 0x7FFF
COUNT_SHIFT = 15
COUNT_MASK = 0x3FF
MAX_COUNT = 999
NAMES = {1: "Master Ball", 2: "Ultra Ball", 3: "Great Ball", 4: "Poke Ball", 12: "Premier Ball"}


def pouch_offset():
    return BLOCKS[ITEM_BLOCK][0] + CATCH_POUCH


def read_pouch(save):
    """[(id, count, raw)] of the used entries, in order."""
    check_layout(save)
    entries = struct.unpack_from(f"<{POUCH_SIZE}I", save, pouch_offset())
    out = []
    for raw in entries:
        item = raw & ID_MASK
        if item == 0:
            break
        out.append((item, (raw >> COUNT_SHIFT) & COUNT_MASK, raw))
    return out


def write_pouch(save, entries):
    raws = [raw for _, _, raw in entries] + [0] * (POUCH_SIZE - len(entries))
    struct.pack_into(f"<{POUCH_SIZE}I", save, pouch_offset(), *raws)
    fix_checksum(save, ITEM_BLOCK)


def set_items(save, changes):
    """changes: [(id, count)]; returns the pouch afterwards."""
    entries = read_pouch(save)
    for item, count in changes:
        if not 1 <= item <= ID_MASK:
            raise SaveError(f"item id {item} out of range")
        if not 0 <= count <= MAX_COUNT:
            raise SaveError(f"item {item}: count {count}, expected 0-{MAX_COUNT}")
        at = next((i for i, e in enumerate(entries) if e[0] == item), None)
        if count == 0:
            if at is not None:
                del entries[at]
            continue
        if at is None:
            if len(entries) >= POUCH_SIZE:
                raise SaveError(f"catch pouch full ({POUCH_SIZE} entries)")
            entries.append((item, count, item))
            at = len(entries) - 1
        raw = entries[at][2] & ~(COUNT_MASK << COUNT_SHIFT) | (count << COUNT_SHIFT)
        entries[at] = (item, count, raw)
    write_pouch(save, entries)
    check_layout(save)  # every block checksum again
    return read_pouch(save)


def print_pouch(entries, out):
    if not entries:
        print("catch pouch empty", file=out)
    for item, count, raw in entries:
        name = NAMES.get(item, "")
        new = " (new)" if raw & (1 << 30) else ""
        print(f"item {item}{' ' + name if name else ''}: {count}{new}", file=out)


def parse_change(text):
    try:
        item, count = text.split("=")
        return int(item, 0), int(count, 0)
    except ValueError:
        raise argparse.ArgumentTypeError(f"expected ID=COUNT, got {text!r}")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("show")
    s.add_argument("save")
    e = sub.add_parser("set")
    e.add_argument("save")
    e.add_argument("--out", required=True, help="new file; must not exist")
    e.add_argument("--item", type=parse_change, action="append", required=True, metavar="ID=COUNT")
    args = ap.parse_args(argv)
    try:
        with open(args.save, "rb") as f:
            save = bytearray(f.read())
        if args.cmd == "show":
            print_pouch(read_pouch(save), sys.stdout)
            return 0
        if os.path.exists(args.out):
            raise SaveError(f"{args.out} exists; refusing to overwrite")
        print("before:")
        print_pouch(read_pouch(save), sys.stdout)
        after = set_items(save, args.item)
        with open(args.out, "xb") as f:
            f.write(save)
        print("after:")
        print_pouch(after, sys.stdout)
        return 0
    except (OSError, SaveError) as err:
        print(f"lgp_save_items: {err}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
