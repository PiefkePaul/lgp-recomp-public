#!/usr/bin/env python3
"""Show or change the party's current HP in a Let's Go savedata.bin (test copies).

  lgp_save_party.py show SAVE
  lgp_save_party.py set-hp SAVE --out NEW [--hp 1] [--faint-except SLOT]

`set-hp` writes a new file and never touches SAVE: every party member gets
`--hp` (clamped to its max HP, at least 1); with `--faint-except SLOT`
(1-based party slot) all other members get 0 HP, so only that one can still
fight. A party with no member left above 0 HP is refused.

Format, as described by the public PKHeX sources (read for the layout only, no
code taken over):
- block table ("BEEF" footer) at 0xB8600: two u64 stamps, 4 magic bytes, then
  per block {u32 length, u16 id, u16 checksum}; checksum = CRC-16/ARC (reflected
  0x8005, init 0, no final xor) over the block;
- block 8 at 0x05A00: u16 storage index of the six party slots (>= 1000 = empty);
- block 9 at 0x05C00: 1000 Pokemon of 0x104 bytes each, encrypted. Bytes
  0xE8..0x104 (party stats: status u32 at 0xE8, level at 0xEC, current HP u16
  at 0xF0, max HP at 0xF2, CP at 0xFE) are xored with an LCG seeded with the
  u32 at 0x00 (x = x * 0x41C64E6D + 0x6073, key = x >> 16 per u16). The
  Pokemon's own checksum (u16 at 0x06) only covers 0x08..0xE8, so changing
  the party stats leaves it valid; only block 9's checksum changes.

Saves are the player's own data: run this only inside LGP_PRIVATE_DIR, never
commit inputs or outputs.
"""
import argparse
import os
import struct
import sys

BLOCK_TABLE = 0xB8800 - 0x200
BLOCKS = [  # (offset, length) per block id
    (0x00000, 0x00D90), (0x00E00, 0x00200), (0x01000, 0x00168), (0x01200, 0x01800),
    (0x02A00, 0x020E8), (0x04C00, 0x00930), (0x05600, 0x00004), (0x05800, 0x00130),
    (0x05A00, 0x00012), (0x05C00, 0x3F7A0), (0x45400, 0x00008), (0x45600, 0x00E90),
    (0x46600, 0x010A4), (0x47800, 0x000F0), (0x47A00, 0x06010), (0x4DC00, 0x00200),
    (0x4DE00, 0x00098), (0x4E000, 0x00068), (0x4E200, 0x69780), (0xB7A00, 0x000B0),
    (0xB7C00, 0x00940),
]
PARTY_BLOCK = 8
STORAGE_BLOCK = 9
SLOT_SIZE = 0x104
STORED_SIZE = 0xE8
SLOT_COUNT = 1000
SAVE_SIZE = 0x100000


class SaveError(Exception):
    pass


def _crc16_table():
    table = []
    for i in range(256):
        crc = i
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
        table.append(crc)
    return table


_CRC16 = _crc16_table()


def crc16_arc(data):
    crc = 0
    for b in data:
        crc = _CRC16[(crc ^ b) & 0xFF] ^ (crc >> 8)
    return crc


def checksum_offset(block_id):
    return BLOCK_TABLE + 0x14 + block_id * 8 + 6


def block_checksum_ok(save, block_id):
    ofs, length = BLOCKS[block_id]
    stored = struct.unpack_from("<H", save, checksum_offset(block_id))[0]
    return crc16_arc(save[ofs:ofs + length]) == stored


def check_layout(save):
    if len(save) != SAVE_SIZE:
        raise SaveError(f"size {len(save)} bytes, expected {SAVE_SIZE}")
    for block_id, (_, length) in enumerate(BLOCKS):
        entry = BLOCK_TABLE + 0x14 + block_id * 8
        got_len, got_id = struct.unpack_from("<IH", save, entry)
        if got_len != length or got_id != block_id:
            raise SaveError(f"block table entry {block_id}: length 0x{got_len:X} id {got_id}, "
                            f"expected 0x{length:X} id {block_id}")
    bad = [i for i in range(len(BLOCKS)) if not block_checksum_ok(save, i)]
    if bad:
        raise SaveError(f"block checksums wrong before any change: {bad}")


def lcg_xor(data, seed):
    out = bytearray(data)
    for i in range(0, len(out), 2):
        seed = (seed * 0x41C64E6D + 0x6073) & 0xFFFFFFFF
        key = seed >> 16
        out[i] ^= key & 0xFF
        out[i + 1] ^= key >> 8
    return out


def slot_offset(index):
    return BLOCKS[STORAGE_BLOCK][0] + index * SLOT_SIZE


def party_indices(save):
    ofs = BLOCKS[PARTY_BLOCK][0]
    indices = struct.unpack_from("<6H", save, ofs)
    return [i for i in indices if i < SLOT_COUNT]


def read_member(save, index):
    ofs = slot_offset(index)
    raw = save[ofs:ofs + SLOT_SIZE]
    seed = struct.unpack_from("<I", raw, 0)[0]
    stored = lcg_xor(raw[8:STORED_SIZE], seed)
    words = struct.unpack_from(f"<{len(stored) // 2}H", stored)
    pkm_ok = (sum(words) & 0xFFFF) == struct.unpack_from("<H", raw, 6)[0]
    stats = lcg_xor(raw[STORED_SIZE:SLOT_SIZE], seed)
    status, level = struct.unpack_from("<IB", stats, 0)
    hp, hp_max = struct.unpack_from("<HH", stats, 8)
    cp = struct.unpack_from("<H", stats, 0x16)[0]
    return {"index": index, "seed": seed, "pkm_ok": pkm_ok, "status": status, "level": level,
            "hp": hp, "hp_max": hp_max, "cp": cp, "stats": stats}


def write_member_hp(save, member, hp, clear_status):
    stats = bytearray(member["stats"])
    struct.pack_into("<H", stats, 8, hp)
    if clear_status:
        struct.pack_into("<I", stats, 0, 0)
    ofs = slot_offset(member["index"]) + STORED_SIZE
    save[ofs:ofs + len(stats)] = lcg_xor(stats, member["seed"])


def fix_checksum(save, block_id):
    ofs, length = BLOCKS[block_id]
    struct.pack_into("<H", save, checksum_offset(block_id), crc16_arc(save[ofs:ofs + length]))


def party(save):
    check_layout(save)
    members = [read_member(save, i) for i in party_indices(save)]
    if not members:
        raise SaveError("party is empty")
    bad = [m["index"] for m in members if not m["pkm_ok"] or m["hp_max"] == 0]
    if bad:
        raise SaveError(f"party slots point at storage entries that do not decode: {bad}")
    return members


def print_party(members, out):
    for slot, m in enumerate(members, 1):
        print(f"slot {slot}: storage {m['index']}, level {m['level']}, HP {m['hp']}/{m['hp_max']}, "
              f"CP {m['cp']}, status 0x{m['status']:X}", file=out)


def set_hp(save, hp, faint_except=None):
    members = party(save)
    if faint_except is not None and not 1 <= faint_except <= len(members):
        raise SaveError(f"--faint-except {faint_except}: party has {len(members)} members")
    if hp < 1:
        raise SaveError("--hp must be at least 1 (use --faint-except to faint members)")
    for slot, m in enumerate(members, 1):
        if faint_except is not None and slot != faint_except:
            write_member_hp(save, m, 0, clear_status=True)
        else:
            write_member_hp(save, m, min(hp, m["hp_max"]), clear_status=False)
    fix_checksum(save, STORAGE_BLOCK)
    after = party(save)  # re-reads and re-checks every block checksum
    if not any(m["hp"] > 0 for m in after):
        raise SaveError("no party member left above 0 HP")
    return after


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("show")
    s.add_argument("save")
    e = sub.add_parser("set-hp")
    e.add_argument("save")
    e.add_argument("--out", required=True, help="new file; must not exist")
    e.add_argument("--hp", type=int, default=1)
    e.add_argument("--faint-except", type=int, help="1-based party slot that keeps --hp, all others 0 HP")
    args = ap.parse_args(argv)
    try:
        with open(args.save, "rb") as f:
            save = bytearray(f.read())
        if args.cmd == "show":
            print_party(party(save), sys.stdout)
            return 0
        if os.path.exists(args.out):
            raise SaveError(f"{args.out} exists; refusing to overwrite")
        print("before:")
        print_party(party(save), sys.stdout)
        after = set_hp(save, args.hp, args.faint_except)
        with open(args.out, "xb") as f:
            f.write(save)
        print("after:")
        print_party(after, sys.stdout)
        return 0
    except (OSError, SaveError) as err:
        print(f"lgp_save_party: {err}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
