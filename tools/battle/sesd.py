#!/usr/bin/env python3
"""Read and write battle sequence files (SESD, .bseq) of Let's Go.

A battle effect (switch, send-out, recall, moves) is a sequence file under
bin/battle/waza/sequence/ in the RomFS (Fang-8/9/19, docs/analysis/55-fangen-im-kampf.md):

    header   16 bytes: "SESD", u32 version (3), u32 0, u32 total frames
    record   i32 start frame, i32 end frame, u16 group, u16 channel,
             u16 flags, u16 command, then a payload of fixed length per command
    end      i32 -1

The payload lengths are not in the file; the game keeps a table of them in its
code. This tool takes them from a private JSON file ({"0x4e": 16, ...},
written from that table), so neither the table nor any sequence enters the
repo. Known commands (Fang-19, static): 0x2c camera on the target, 0x2e camera
load, 0x31 camera end, 0x4e ball create, 0x4f group end, 0x50 ball flight,
0x51 ball placement, 0x67 animation config by path, 0x6a transition by name,
0x70/0x71 sound (Fang-24/25). Each sound needs a group of its own: the game
keeps one sound per group for the end of the sequence and frees only that one,
a second sound in the group leaks and the battle end stays white (Fang-25), so
build refuses that.

    sesd.py dump <file.bseq> --lengths L.json [--payload]
    sesd.py build <spec.json> --lengths L.json --out <file.bseq>

A spec for build:

    {"frames": 260,
     "records": [
       {"from": "ee620.bseq", "index": 3},                       # a record copied as it is
       {"from": "ee620.bseq", "index": 5, "end": 240},           # ... with fields changed
       {"from": "ee420.bseq", "index": 2, "patch": {"0x204": "u32:0"}},  # ... with payload bytes changed
       {"start": 0, "end": 0, "group": 7, "channel": 0, "flags": 0, "cmd": "0x6a",
        "fields": ["text:to_capture01_fall"]}                    # a new record
     ]}

"from" paths are relative to the spec; "romfs:PATH" takes the file from the game's RomFS instead
(tools/kit/derive.py, `build --romfs`). "fields" fill the payload from the
start, the rest stays 0: u8/u16/u32/i32/f32:VALUE, text:STRING (NUL-terminated),
hex:BYTES; "payload" (hex) gives the whole payload; "patch" writes such fields at payload offsets
(keys "0x204" or "516") over the copied or given payload, so a changed game record needs no game bytes in
the spec. Records are written in the
order given. Inputs and outputs are game data and stay in $LGP_PRIVATE_DIR.
"""
import argparse
import json
import os
import struct
import sys

MAGIC = b"SESD"
HEADER = struct.Struct("<4sIII")
RECORD = struct.Struct("<iiHHHH")
END = -1

COMMANDS = {
    0x2C: "camera on target",
    0x2E: "camera load",
    0x31: "camera end",
    0x4E: "ball create",
    0x4F: "group end",
    0x50: "ball flight",
    0x51: "ball placement",
    0x67: "animation config",
    0x6A: "transition",
    0x70: "sound",
    0x71: "partner voice",
}

SOUNDS = (0x70, 0x71)


class SesdError(Exception):
    pass


def load_lengths(path):
    with open(path, encoding="utf-8") as f:
        raw = json.load(f)
    lengths = {}
    for key, value in raw.items():
        cmd = int(key, 0)
        if not 0 <= cmd <= 0xFFFF or not isinstance(value, int) or value < 0:
            raise SesdError(f"{path}: bad entry {key!r}: {value!r}")
        lengths[cmd] = value
    return lengths


def parse(data, lengths):
    """(version, frames, records); each record a dict with the payload as bytes."""
    if len(data) < HEADER.size + 4:
        raise SesdError("too short for a SESD file")
    magic, version, zero, frames = HEADER.unpack_from(data, 0)
    if magic != MAGIC:
        raise SesdError("not a SESD file (magic)")
    at = HEADER.size
    records = []
    while True:
        if at + 4 > len(data):
            raise SesdError(f"no end marker (at {at:#x})")
        (start,) = struct.unpack_from("<i", data, at)
        if start == END:
            at += 4
            break
        if at + RECORD.size > len(data):
            raise SesdError(f"record {len(records)} cut off (at {at:#x})")
        start, end, group, channel, flags, cmd = RECORD.unpack_from(data, at)
        if cmd not in lengths:
            raise SesdError(f"record {len(records)}: command {cmd:#x} has no length in the table")
        n = lengths[cmd]
        body = at + RECORD.size
        if body + n > len(data):
            raise SesdError(f"record {len(records)}: payload cut off (command {cmd:#x}, {n} bytes)")
        records.append(
            {"start": start, "end": end, "group": group, "channel": channel, "flags": flags, "cmd": cmd,
             "payload": bytes(data[body:body + n])})
        at = body + n
    if at != len(data):
        raise SesdError(f"{len(data) - at} bytes after the end marker")
    return version, frames, records


def serialize(frames, records, lengths, version=3):
    out = bytearray(HEADER.pack(MAGIC, version, 0, frames))
    for i, r in enumerate(records):
        n = lengths.get(r["cmd"])
        if n is None:
            raise SesdError(f"record {i}: command {r['cmd']:#x} has no length in the table")
        if len(r["payload"]) != n:
            raise SesdError(f"record {i}: payload {len(r['payload'])} bytes, command {r['cmd']:#x} has {n}")
        if r["start"] == END:
            raise SesdError(f"record {i}: start -1 is the end marker")
        out += RECORD.pack(r["start"], r["end"], r["group"], r["channel"], r["flags"], r["cmd"])
        out += r["payload"]
    out += struct.pack("<i", END)
    return bytes(out)


def encode_field(field):
    if not isinstance(field, str):
        raise SesdError(f"field {field!r}: must be a text like u32:0")
    kind, _, value = field.partition(":")
    if kind == "text":
        return value.encode("utf-8") + b"\0"
    try:
        if kind == "hex":
            return bytes.fromhex(value)
        if kind in ("u8", "u16", "u32", "i32"):
            return struct.pack({"u8": "<B", "u16": "<H", "u32": "<I", "i32": "<i"}[kind], int(value, 0))
        if kind == "f32":
            return struct.pack("<f", float(value))
    except (ValueError, struct.error) as e:
        raise SesdError(f"field {field!r}: {e}") from None
    raise SesdError(f"field {field!r}: unknown kind (u8, u16, u32, i32, f32, text, hex)")


def fill_fields(fields, length):
    payload = bytearray(length)
    at = 0
    for field in fields:
        raw = encode_field(field)
        if at + len(raw) > length:
            raise SesdError(f"fields need more than the payload's {length} bytes")
        payload[at:at + len(raw)] = raw
        at += len(raw)
    return bytes(payload)


def build(spec, base_dir, lengths, read_romfs=None):
    """read_romfs(path) -> bytes resolves "from": "romfs:PATH" (None: such records are refused)."""
    if not isinstance(spec.get("frames"), int) or spec["frames"] <= 0:
        raise SesdError("spec: `frames` must be a positive number")
    cache = {}
    records = []
    for i, item in enumerate(spec.get("records", [])):
        if "from" in item:
            romfs = item["from"].startswith("romfs:")
            path = item["from"] if romfs else os.path.join(base_dir, item["from"])
            if path not in cache:
                if romfs:
                    if read_romfs is None:
                        raise SesdError(f"spec record {i}: {item['from']} needs the game's RomFS (--romfs)")
                    data = read_romfs(path[len("romfs:"):])
                else:
                    with open(path, "rb") as f:
                        data = f.read()
                cache[path] = parse(data, lengths)[2]
            source = cache[path]
            index = item.get("index")
            if not isinstance(index, int) or not 0 <= index < len(source):
                raise SesdError(f"spec record {i}: index {index!r} not in {item['from']} ({len(source)} records)")
            r = dict(source[index])
        else:
            r = {"start": 0, "end": 0, "group": 0, "channel": 0, "flags": 0}
            if "cmd" not in item:
                raise SesdError(f"spec record {i}: `cmd` or `from` needed")
        for key in ("start", "end", "group", "channel", "flags"):
            if key in item:
                r[key] = int(item[key], 0) if isinstance(item[key], str) else item[key]
        if "cmd" in item:
            r["cmd"] = int(item["cmd"], 0) if isinstance(item["cmd"], str) else item["cmd"]
        n = lengths.get(r["cmd"])
        if n is None:
            raise SesdError(f"spec record {i}: command {r['cmd']:#x} has no length in the table")
        if "payload" in item:
            r["payload"] = bytes.fromhex(item["payload"])
        elif "fields" in item:
            r["payload"] = fill_fields(item["fields"], n)
        elif "payload" not in r:
            r["payload"] = bytes(n)
        if "patch" in item:
            if not isinstance(item["patch"], dict):
                raise SesdError(f"spec record {i}: `patch` must map payload offsets to fields")
            payload = bytearray(r["payload"])
            for off, field in item["patch"].items():
                try:
                    at = int(off, 0)
                except ValueError:
                    raise SesdError(f"spec record {i}: patch offset {off!r} is not a number") from None
                raw = encode_field(field)
                if at < 0 or at + len(raw) > len(payload):
                    raise SesdError(f"spec record {i}: patch {off} {field!r} past the {len(payload)}-byte payload")
                payload[at:at + len(raw)] = raw
            r["payload"] = bytes(payload)
        records.append(r)
    sound_groups = {}
    for i, r in enumerate(records):
        if r["end"] > spec["frames"]:
            raise SesdError(f"spec record {i}: ends at frame {r['end']}, after the {spec['frames']} frames")
        if r["cmd"] in SOUNDS:
            if r["group"] in sound_groups:
                raise SesdError(f"spec record {i}: group {r['group']} already has a sound (record "
                                f"{sound_groups[r['group']]}); each sound needs a group of its own (Fang-25)")
            sound_groups[r["group"]] = i
    return serialize(spec["frames"], records, lengths)


def cmd_dump(args):
    lengths = load_lengths(args.lengths)
    with open(args.file, "rb") as f:
        version, frames, records = parse(f.read(), lengths)
    print(f"SESD version {version}, {frames} frames, {len(records)} records")
    for i, r in enumerate(records):
        name = COMMANDS.get(r["cmd"], "")
        line = (f"{i:3} {r['start']:5}-{r['end']:<5} group {r['group']:3} channel {r['channel']:3} "
                f"flags {r['flags']:#06x} cmd {r['cmd']:#06x} {len(r['payload']):3} B {name}")
        if args.payload:
            line += " " + r["payload"].hex()
        print(line.rstrip())
    return 0


def cmd_build(args):
    lengths = load_lengths(args.lengths)
    with open(args.spec, encoding="utf-8") as f:
        spec = json.load(f)
    read_romfs = None
    if args.romfs:
        sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "kit"))
        import derive  # noqa: E402
        read_romfs = derive.open_romfs(args.romfs).read
    data = build(spec, os.path.dirname(os.path.abspath(args.spec)), lengths, read_romfs)
    if os.path.exists(args.out) and not args.force:
        raise SesdError(f"{args.out} exists (--force to overwrite)")
    with open(args.out, "wb") as f:
        f.write(data)
    print(f"wrote {len(data)} bytes, {len(spec.get('records', []))} records, {spec['frames']} frames")
    return 0


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = p.add_subparsers(dest="command", required=True)
    d = sub.add_parser("dump", help="list the records of a sequence")
    d.add_argument("file")
    d.add_argument("--lengths", required=True, help="JSON with the payload length per command")
    d.add_argument("--payload", action="store_true", help="also the payload bytes (game data: keep private)")
    d.set_defaults(fn=cmd_dump)
    b = sub.add_parser("build", help="write a sequence from a spec")
    b.add_argument("spec")
    b.add_argument("--lengths", required=True)
    b.add_argument("--out", required=True)
    b.add_argument("--force", action="store_true")
    b.add_argument("--romfs", help='romfs.bin or an unpacked RomFS folder, for "from": "romfs:PATH"')
    b.set_defaults(fn=cmd_build)
    args = p.parse_args(argv)
    try:
        return args.fn(args)
    except (SesdError, OSError, ValueError, KeyError, json.JSONDecodeError) as e:
        print(f"sesd: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
