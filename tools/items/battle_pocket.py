#!/usr/bin/env python3
"""Show balls in the bag during battles: rewrite battle_pocket.dat of Let's Go.

battle_pocket.dat (RomFS) holds one byte per item number, a bit mask of how the
item shows in the battle bag: 0 = not shown, bit 1 = healing item with a target
choice ("use on which Pokémon?"), bit 2 = battle item used at once (PC Fang-3,
2026-10-23: 3 shows a ball twice and asks for a target, 4 hides it, 2 shows it
once and throws at once). In 1.0.0 the balls are 0, which is the only reason
they are missing from the battle bag; the battle server still has the ball
branch with the Gen-7 catch formula (Fang-1/Fang-2,
docs/analysis/55-fangen-im-kampf.md). This tool sets the balls to a mask
(default 2) and writes the result as the file of the data mod battle-balls:

    battle_pocket.py check <battle_pocket.dat>          # size and counts only
    battle_pocket.py show <battle_pocket.dat> --items 1,2,3,4,12,17
    battle_pocket.py apply <battle_pocket.dat> --out <mods>/battle-balls/romfs/<path>/battle_pocket.dat
                           [--items 1,2,3,4,12] [--value 2 | --like ITEM] [--offset N] [--force]

Item numbers follow the Gen-7 item list (Master Ball 1, Ultra Ball 2, Great Ball 3,
Poké Ball 4, Premier Ball 12, Potion 17); the PC run showed Poké Ball and Great
Ball with them at offset 0. --offset handles a header. The file is game data: input and output
stay in $LGP_PRIVATE_DIR.
"""
import argparse
import collections
import sys

BALLS = (1, 2, 3, 4, 12)
POTION = 17
BATTLE_ITEM = 2  # mask bit: shown, used at once (no target choice)


def parse_items(text):
    items = []
    for part in text.split(","):
        part = part.strip()
        if not part:
            continue
        n = int(part, 0)
        if n < 0:
            raise ValueError(f"item {n} < 0")
        items.append(n)
    if not items:
        raise ValueError("no items")
    return items


def at(data, offset, item):
    i = offset + item
    if i < 0:
        raise IndexError(f"item {item} with offset {offset} is before the start of the file")
    if i >= len(data):
        raise IndexError(f"item {item} (byte {i:#x}) is past the end of the file ({len(data)} bytes)")
    return i


def patch(data, items, value, offset=0, force=False):
    """Returns (new bytes, [(item, old)]) with value written at each item.

    Without force, an item that already has another mask than 0 or value is an
    error: then the file is not what this tool expects."""
    out = bytearray(data)
    changes = []
    for item in items:
        i = at(out, offset, item)
        old = out[i]
        if old not in (0, value) and not force:
            raise ValueError(f"item {item} has mask {old}, expected 0 (use --force)")
        if old != value:
            out[i] = value
            changes.append((item, old))
    return bytes(out), changes


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("check")
    c.add_argument("file")
    s = sub.add_parser("show")
    s.add_argument("file")
    s.add_argument("--items", default=",".join(map(str, BALLS + (POTION,))))
    s.add_argument("--offset", type=lambda t: int(t, 0), default=0)
    a = sub.add_parser("apply")
    a.add_argument("file")
    a.add_argument("--out", required=True)
    a.add_argument("--items", default=",".join(map(str, BALLS)))
    g = a.add_mutually_exclusive_group()
    g.add_argument("--value", type=lambda t: int(t, 0))
    g.add_argument("--like", type=lambda t: int(t, 0), help="copy the mask of this item (17 = Potion asks for a target)")
    a.add_argument("--offset", type=lambda t: int(t, 0), default=0)
    a.add_argument("--force", action="store_true")
    args = ap.parse_args(argv)

    with open(args.file, "rb") as f:
        data = f.read()
    try:
        if args.cmd == "check":
            counts = collections.Counter(data)
            print(f"{len(data)} bytes, {counts.get(0, 0)} zero, {len(data) - counts.get(0, 0)} shown")
            for mask in sorted(counts):
                if mask:
                    print(f"mask {mask}: {counts[mask]} items")
            return 0
        if args.cmd == "show":
            for item in parse_items(args.items):
                print(f"item {item}: mask {data[at(data, args.offset, item)]}")
            return 0
        if args.like is not None:
            value = data[at(data, args.offset, args.like)]
        else:
            value = BATTLE_ITEM if args.value is None else args.value
        if not 0 < value < 256:
            print(f"battle_pocket: mask {value} does not show the item (reference item not shown in battle?)",
                  file=sys.stderr)
            return 1
        out, changes = patch(data, parse_items(args.items), value, args.offset, args.force)
    except (ValueError, IndexError) as e:
        print(f"battle_pocket: {e}", file=sys.stderr)
        return 1
    with open(args.out, "wb") as f:
        f.write(out)
    print(f"{len(changes)} items set to mask {value}" + "".join(f", item {i} from {o}" for i, o in changes))
    return 0


if __name__ == "__main__":
    sys.exit(main())
