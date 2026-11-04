#!/usr/bin/env python3
"""Read, check and rewrite the spawn tables of Let's Go (encount_data_*.bin, a FlatBuffer).

Layout (from public format notes, in our own words; field order decides the
FlatBuffers slots, docs/analysis/54-zufallsbegegnungen.md):

  root table:  0 areas (vector of area tables)
  area table:  0 zone id (u64), 1 trainer rank min (i32), 2 trainer rank max (i32),
               then the lists in this order, each a run of fields:
                 ground, water          allowed (bool), max count, duration, rate,
                                        level min, level max, weight sum, slots
                 old rod, good rod,     rate, level min, level max, weight sum, slots
                 super rod
                 sky                    like ground
  slot table:  0 weight (i32), 1 species (i32), 2 form (i16)

On the real 1.0.0 files (PC, 2026-10-22) `check` finds this layout: 42 area
fields, 70 areas sharing 7 vtables, list lengths 10/5/1/1/5/4, many fields
absent (rank always, rod scalars always, list scalars in zones without that
list, 1116 of 1820 slots without any field). Absent and an explicit 0 are NOT
the same to the game: a rewrite that wrote every field showed no field Pokémon
at all, the same rewrite keeping absent fields absent works. So the parser
records which fields are present (`_present`) and the writer writes exactly
those, plus the ones `apply` sets. Lists keep their length: Gelb species fill
the existing slots, and spare slots are made by splitting the weight of the
most common species (same frequencies). `--grow` lengthens a list when Gelb has
more species than slots (untested in the game). `--in-place` patches values
into the original bytes instead (also works in the game; only fields that are
present can change). Everything this tool reads or writes is game data and stays
in $LGP_PRIVATE_DIR; `check` prints only counts.

    encount_data.py check <encount_data_p.bin>
    encount_data.py apply <encount_data_p.bin> --tables yellow.json --zones zones.tsv --out new.bin [--max-scale 2] [--grow | --in-place]
    encount_data.py rates --tables yellow.json --zones zones.tsv --out rates.cfg
    encount_data.py slots --tables yellow.json --zones zones.tsv --out slots.cfg

zones.tsv (private, made by hand): zone id (hex) <TAB> list (ground|water|sky)
<TAB> one or more Gen-1 areas as "location/area" from gen1_tables.py, comma
separated. Each named list gets the species of those areas; water takes the
surf and rod slots together (Let's Go has no fishing).

`rates` writes the `zone_rate <name> <zone> <rate>` lines of the mod
random-encounters (its menu choice "Original", Analysis 54): Gelb's walking
encounter rate per step out of 256 for each ground zone, the rounded mean when
a zone names areas with different rates (a note says so). Append them to the
mod file; they name zone ids, so they stay private too.

`slots` writes the `slot <name> <zone> <weight> <species> <level>[-<max>]`
lines of the same mod's Gelb mode: the walking slots of each ground zone, the
weights in percent (slots of one species added up by gen1_tables.py, their
levels as a range; a zone with several areas gets each area's share). Private
as well.
"""
import argparse
import json
import struct
import sys

FULL = ("allowed", "max_count", "duration", "rate", "level_min", "level_max", "weight_sum", "slots")
ROD = ("rate", "level_min", "level_max", "weight_sum", "slots")
LISTS = (("ground", FULL), ("water", FULL), ("old_rod", ROD), ("good_rod", ROD), ("super_rod", ROD), ("sky", FULL))
LIST_NAMES = tuple(n for n, _ in LISTS)


def area_fields():
    """[(name, kind)] in slot order; kind: u64, i32, bool, slots."""
    out = [("zone", "u64"), ("rank_min", "i32"), ("rank_max", "i32")]
    for lname, fields in LISTS:
        for f in fields:
            kind = "bool" if f == "allowed" else "slots" if f == "slots" else "i32"
            out.append((lname + "." + f, kind))
    return out


AREA = area_fields()
SLOT = (("weight", "i32"), ("species", "i32"), ("form", "i16"))
SIZES = {"u64": 8, "i32": 4, "i16": 2, "bool": 1}
FORMATS = {"u64": "<Q", "i32": "<i", "i16": "<h", "bool": "<B"}


class FormatError(Exception):
    pass


# --- reading --------------------------------------------------------------------------


class Reader:
    def __init__(self, data):
        self.d = bytes(data)

    def u32(self, at):
        if at < 0 or at + 4 > len(self.d):
            raise FormatError(f"offset {at:#x} out of range")
        return struct.unpack_from("<I", self.d, at)[0]

    def table(self, at):
        """Field positions of the table at `at`: list, None for absent fields."""
        vt = at - struct.unpack_from("<i", self.d, at)[0]
        if vt < 0 or vt + 4 > len(self.d):
            raise FormatError(f"vtable of the table at {at:#x} out of range")
        vt_size, _ = struct.unpack_from("<HH", self.d, vt)
        n = (vt_size - 4) // 2
        offs = struct.unpack_from(f"<{n}H", self.d, vt + 4)
        return [at + o if o else None for o in offs]

    def scalar(self, pos, kind):
        if pos is None:
            return False if kind == "bool" else 0
        v = struct.unpack_from(FORMATS[kind], self.d, pos)[0]
        return bool(v) if kind == "bool" else v

    def ref(self, pos):
        return pos + self.u32(pos)

    def vector(self, pos):
        if pos is None:
            return []
        start = self.ref(pos)
        n = self.u32(start)
        return [self.ref(start + 4 + 4 * i) for i in range(n)]

    def fields(self, at, layout):
        pos = self.table(at)
        out = {}
        for i, (name, kind) in enumerate(layout):
            p = pos[i] if i < len(pos) else None
            if kind == "slots":
                out[name] = [self.fields(t, SLOT) for t in self.vector(p)]
            else:
                out[name] = self.scalar(p, kind)
        out["_present"] = [name for i, (name, _) in enumerate(layout) if i < len(pos) and pos[i] is not None]
        out["_extra_fields"] = sum(1 for p in pos[len(layout):] if p is not None)
        return out


    def positions(self, at, layout):
        """Like fields(), but the byte position of each field (None if absent)."""
        pos = self.table(at)
        out = {}
        for i, (name, kind) in enumerate(layout):
            p = pos[i] if i < len(pos) else None
            out[name] = [self.positions(t, SLOT) for t in self.vector(p)] if kind == "slots" else p
        return out


def area_tables(r):
    root = r.ref(0)
    pos = r.table(root)
    return r.vector(pos[0] if pos else None)


def parse(data):
    r = Reader(data)
    areas = [r.fields(t, AREA) for t in area_tables(r)]
    for a in areas:
        for s in (s for k, v in a.items() if k.endswith(".slots") for s in v):
            s.pop("_extra_fields", None)
    return areas


# --- writing --------------------------------------------------------------------------


class Writer:
    """Front-to-back FlatBuffer writer: every referenced object follows its referrer.
    Writes the fields listed in `_present` (all non-zero ones if a table has no
    such list) and shares equal vtables, as flatc does."""

    def __init__(self):
        self.b = bytearray(b"\0\0\0\0")  # root offset, patched last
        self.vtables = {}

    def align(self, n):
        while len(self.b) % n:
            self.b.append(0)

    def patch_ref(self, at, target):
        struct.pack_into("<I", self.b, at, target - at)

    def table(self, layout, values):
        """Writes the table and its children; returns its position."""
        keep = values.get("_present")
        present = [(i, name, kind) for i, (name, kind) in enumerate(layout)
                   if (name in keep if keep is not None else values[name])]
        scalars = [(i, n, k) for i, n, k in present if k != "slots"]
        refs = [(i, n, k) for i, n, k in present if k == "slots"]
        # the table first, its vtable just before it or an earlier equal one
        # (soffset positive: vtable before table)
        nfields = present[-1][0] + 1 if present else 0
        self.align(2)
        vt = len(self.b)
        self.b += b"\0" * (4 + 2 * nfields)
        self.align(8)
        at = len(self.b)
        self.b += b"\0\0\0\0"
        offsets = [0] * nfields
        # widest first keeps padding small; alignment is absolute
        for i, name, kind in sorted(scalars, key=lambda f: -SIZES[f[2]]):
            self.align(SIZES[kind])
            offsets[i] = len(self.b) - at
            v = values[name]
            self.b += struct.pack(FORMATS[kind], int(v))
        ref_pos = {}
        for i, name, _ in refs:
            self.align(4)
            offsets[i] = len(self.b) - at
            ref_pos[name] = len(self.b)
            self.b += b"\0\0\0\0"
        size = len(self.b) - at
        vtable = struct.pack(f"<HH{nfields}H", 4 + 2 * nfields, size, *offsets)
        vt = self.vtables.setdefault(vtable, vt)
        self.b[vt:vt + len(vtable)] = vtable
        struct.pack_into("<i", self.b, at, at - vt)
        for name, pos in ref_pos.items():
            self.patch_ref(pos, self.table_vector(SLOT, values[name]))
        return at

    def table_vector(self, layout, items):
        self.align(4)
        start = len(self.b)
        self.b += struct.pack("<I", len(items)) + b"\0" * (4 * len(items))
        for k, item in enumerate(items):
            self.patch_ref(start + 4 + 4 * k, self.table(layout, item))
        return start


def serialize(areas):
    w = Writer()
    # root table with one field: the vector of areas
    w.align(2)
    vt = len(w.b)
    w.b += struct.pack("<HHH", 6, 8, 4)
    w.align(4)
    root = len(w.b)
    w.b += struct.pack("<i", root - vt) + b"\0\0\0\0"
    w.patch_ref(0, root)
    w.patch_ref(root + 4, w.table_vector(AREA, areas))
    return bytes(w.b)


# --- check and apply ------------------------------------------------------------------


def check(data):
    """Counts only (no game values): (lines, ok)."""
    r = Reader(data)
    areas = parse(data)
    positions = [r.positions(t, AREA) for t in area_tables(r)]
    lists = with_sum = matched = slots = 0
    for a, pos in zip(areas, positions):
        for lname in LIST_NAMES:
            s = a[lname + ".slots"]
            if not s:
                continue
            lists += 1
            slots += len(s)
            if pos.get(lname + ".weight_sum") is None:
                continue  # left out (the one-slot rod lists): nothing to compare
            with_sum += 1
            matched += sum(x["weight"] for x in s) == a[lname + ".weight_sum"]
    extra = sum(a["_extra_fields"] for a in areas)
    rewrite = serialize(areas)
    same = parse(rewrite) == areas
    lines = [
        f"areas {len(areas)}, lists with slots {lists}, slots {slots}",
        f"weight sum = sum of slot weights: {matched}/{with_sum} (lists without the field: {lists - with_sum})",
        f"fields beyond the known layout: {extra}",
        f"rewrite parses back the same: {'yes' if same else 'NO'}, {len(rewrite)} bytes for {len(data)}",
    ]
    ok = len(areas) > 0 and with_sum > 0 and matched == with_sum and extra == 0 and same
    lines.append("layout looks right" if ok else "layout NOT confirmed")
    return lines, ok


def load_zones(path):
    zones = []
    with open(path, encoding="utf-8") as f:
        for n, line in enumerate(f, 1):
            line = line.split("#", 1)[0].strip()
            if not line:
                continue
            parts = line.split("\t")
            if len(parts) != 3 or parts[1] not in ("ground", "water", "sky"):
                raise ValueError(f"{path}:{n}: want ZONE<TAB>ground|water|sky<TAB>location/area[,…]")
            zones.append((int(parts[0], 16), parts[1], [p.strip() for p in parts[2].split(",") if p.strip()]))
    return zones


def gen1_slots(tables, names, water):
    """Merged slots (weight in percent of the named areas together) and the level range."""
    by_name = {f"{a['location']}/{a['area']}": a for a in tables["areas"]}
    methods = ("surf", "old-rod", "good-rod", "super-rod") if water else ("walk",)
    weights, lo, hi = {}, None, None
    for name in names:
        if name not in by_name:
            raise ValueError(f"no Gen-1 area {name}")
        for m in methods:
            for s in by_name[name]["methods"].get(m, {}).get("slots", []):
                weights[s["species"]] = weights.get(s["species"], 0) + s["weight"]
                lo = s["min_level"] if lo is None else min(lo, s["min_level"])
                hi = s["max_level"] if hi is None else max(hi, s["max_level"])
    slots = [{"weight": w, "species": sp, "form": 0} for sp, w in sorted(weights.items(), key=lambda kv: (-kv[1], kv[0]))]
    return slots, lo, hi


def zone_rates(tables, zones, notes=None):
    """(name, zone, rate) for every ground zone: Gelb's walk rate out of 256."""
    by_name = {f"{a['location']}/{a['area']}": a for a in tables["areas"]}
    out = []
    for zone, lname, names in zones:
        if lname != "ground":
            continue
        rates = []
        for name in names:
            if name not in by_name:
                raise ValueError(f"no Gen-1 area {name}")
            rate = by_name[name]["methods"].get("walk", {}).get("rate")
            if rate:
                rates.append(rate)
        if not rates:
            if notes is not None:
                notes.append(f"zone {zone:#x}: no walk rate in the tables, left out")
            continue
        rate = (sum(rates) + len(rates) // 2) // len(rates)
        if notes is not None and len(set(rates)) > 1:
            notes.append(f"zone {zone:#x}: rates {sorted(set(rates))}, took {rate}")
        label = "".join(ch if ch.isalnum() else "_" for ch in names[0].split("/")[0].lower()).strip("_")
        out.append((label or "area", zone, min(rate, 256)))
    return out


def zone_slots(tables, zones, notes=None):
    """(name, zone, [(weight, species, lo, hi)]) for every ground zone: Gelb's walking slots."""
    by_name = {f"{a['location']}/{a['area']}": a for a in tables["areas"]}
    out = []
    for zone, lname, names in zones:
        if lname != "ground":
            continue
        merged = {}
        used = 0
        for name in names:
            if name not in by_name:
                raise ValueError(f"no Gen-1 area {name}")
            walk = by_name[name]["methods"].get("walk")
            if not walk or not walk.get("slots"):
                continue
            used += 1
            for s in walk["slots"]:
                w, lo, hi = merged.get(s["species"], (0, s["min_level"], s["max_level"]))
                merged[s["species"]] = (w + s["weight"], min(lo, s["min_level"]), max(hi, s["max_level"]))
        if not merged:
            if notes is not None:
                notes.append(f"zone {zone:#x}: no walking slots in the tables, left out")
            continue
        if notes is not None and used > 1:
            notes.append(f"zone {zone:#x}: slots of {used} areas together")
        label = "".join(ch if ch.isalnum() else "_" for ch in names[0].split("/")[0].lower()).strip("_")
        slots = [(w, sp, lo, hi) for sp, (w, lo, hi) in sorted(merged.items(), key=lambda kv: (-kv[1][0], kv[0]))]
        out.append((label or "area", zone, slots))
    return out


def spread(slots, n):
    """n slots with the same frequencies: splits the weight of the most common
    species until there are n (only when the file has more slots than species)."""
    slots = [dict(s) for s in slots]
    while len(slots) < n:
        if max(s["weight"] for s in slots) < 2:
            for s in slots:
                s["weight"] *= 2
        big = max(range(len(slots)), key=lambda i: slots[i]["weight"])
        w = slots[big]["weight"]
        slots[big]["weight"] = w - w // 2
        slots.insert(big + 1, dict(slots[big], weight=w // 2))
    return slots


def patch(buf, pos, kind, value, what):
    if pos is None:
        raise ValueError(f"{what}: field not in the file, cannot patch in place (leave out --in-place)")
    struct.pack_into(FORMATS[kind], buf, pos, int(value))


def put(table, name, value):
    table[name] = value
    if name not in table["_present"]:
        table["_present"].append(name)


def apply(data, tables, zones, max_scale=1.0, grow=False, in_place=False, notes=None):
    """New file and the number of lists replaced (see the top). `notes` collects
    warnings: lists the file has no spawn fields for, max counts left alone."""
    notes = [] if notes is None else notes
    r = Reader(data)
    tabs = area_tables(r)
    areas = parse(data)
    index = {a["zone"]: i for i, a in enumerate(areas)}
    changes = []
    for zone, lname, names in zones:
        if zone not in index:
            raise ValueError(f"zone {zone:016x} not in the file")
        slots, lo, hi = gen1_slots(tables, names, lname == "water")
        if not slots:
            raise ValueError(f"zone {zone:016x}: no {lname} slots in {','.join(names)}")
        a = areas[index[zone]]
        what = f"zone {zone:016x} {lname}"
        have = len(a[lname + ".slots"])
        if len(slots) > have and not grow:
            raise ValueError(f"{what}: {len(slots)} species but only {have} slots in the file (--grow lengthens the list)")
        missing = [f for f in ("allowed", "max_count", "rate") if lname + "." + f not in a["_present"]]
        if missing:
            notes.append(f"{what}: the file has no {'/'.join(missing)} there (maybe no spawns in this list)")
        changes.append((index[zone], lname, spread(slots, have), lo, hi, what))

    if in_place:
        buf = bytearray(data)
        for i, lname, slots, lo, hi, what in changes:
            pos = r.positions(tabs[i], AREA)
            for k, (s, p) in enumerate(zip(slots, pos[lname + ".slots"])):
                for field, kind in SLOT:
                    if field == "form" and p[field] is None:
                        continue  # left as the file has it (all Gelb species are base forms)
                    patch(buf, p[field], kind, s[field], f"{what} slot {k} {field}")
            patch(buf, pos[lname + ".weight_sum"], "i32", sum(s["weight"] for s in slots), what + " weight_sum")
            patch(buf, pos[lname + ".level_min"], "i32", lo, what + " level_min")
            patch(buf, pos[lname + ".level_max"], "i32", hi, what + " level_max")
        if max_scale != 1.0:
            for i, t in enumerate(tabs):
                pos = r.positions(t, AREA)
                for lname, fields in LISTS:
                    key = lname + ".max_count"
                    if "max_count" in fields and pos[key] is not None:
                        patch(buf, pos[key], "i32", int(round(areas[i][key] * max_scale)), key)
        return bytes(buf), len(changes)

    for i, lname, slots, lo, hi, what in changes:
        a = areas[i]
        put(a, lname + ".slots", [dict(s, _present=[f for f, _ in SLOT]) for s in slots])
        put(a, lname + ".weight_sum", sum(s["weight"] for s in slots))
        put(a, lname + ".level_min", lo)
        put(a, lname + ".level_max", hi)
    skipped = 0
    for a in areas:
        a.pop("_extra_fields", None)
        if max_scale != 1.0:
            for lname, fields in LISTS:
                key = lname + ".max_count"
                if "max_count" not in fields:
                    continue
                if key in a["_present"]:
                    a[key] = int(round(a[key] * max_scale))
                elif a[lname + ".slots"]:
                    skipped += 1  # the game's default is unknown, so it stays absent
    if skipped:
        notes.append(f"--max-scale: {skipped} lists without a max count field left alone")
    return serialize(areas), len(changes)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("check", help="check the layout on a file (prints counts only)")
    c.add_argument("file")
    a = sub.add_parser("apply", help="write the file with Gen-1 tables")
    a.add_argument("file")
    a.add_argument("--tables", required=True, help="JSON from gen1_tables.py")
    a.add_argument("--zones", required=True, help="zones.tsv (private)")
    a.add_argument("--out", required=True)
    a.add_argument("--max-scale", type=float, default=1.0,
                   help="multiply every max count (room for invisible spawns)")
    mode = a.add_mutually_exclusive_group()
    mode.add_argument("--grow", action="store_true",
                      help="lengthen lists when Gelb has more species than slots (untested in the game)")
    mode.add_argument("--in-place", action="store_true",
                      help="patch values into the original bytes (only fields the file has)")
    r = sub.add_parser("rates", help="write zone_rate lines for random-encounters (Gelb's rate per step)")
    r.add_argument("--tables", required=True, help="JSON from gen1_tables.py")
    r.add_argument("--zones", required=True, help="zones.tsv (private)")
    r.add_argument("--out", required=True, help="the lines (private: zone ids)")
    sl = sub.add_parser("slots", help="write slot lines for random-encounters' Gelb mode (Gelb's species per zone)")
    sl.add_argument("--tables", required=True, help="JSON from gen1_tables.py")
    sl.add_argument("--zones", required=True, help="zones.tsv (private)")
    sl.add_argument("--out", required=True, help="the lines (private: zone ids)")
    args = ap.parse_args(argv)
    try:
        if args.cmd == "slots":
            with open(args.tables, encoding="utf-8") as f:
                tables = json.load(f)
            notes = []
            zones = zone_slots(tables, load_zones(args.zones), notes)
            for n in notes:
                print(f"note: {n}", file=sys.stderr)
            count = 0
            with open(args.out, "w", encoding="utf-8") as f:
                f.write("# Gelb's walking slots per zone for Gelb mode (encount_data.py slots)\n")
                for name, zone, slots in zones:
                    for w, sp, lo, hi in slots:
                        level = f"{lo}" if lo == hi else f"{lo}-{hi}"
                        f.write(f"slot {name} {zone:#x} {w} {sp} {level}\n")
                        count += 1
            print(f"{len(zones)} zones, {count} slot lines")
            return 0
        if args.cmd == "rates":
            with open(args.tables, encoding="utf-8") as f:
                tables = json.load(f)
            notes = []
            lines = zone_rates(tables, load_zones(args.zones), notes)
            for n in notes:
                print(f"note: {n}", file=sys.stderr)
            with open(args.out, "w", encoding="utf-8") as f:
                f.write("# Gelb's encounter rate per step out of 256 (encount_data.py rates)\n")
                for name, zone, rate in lines:
                    f.write(f"zone_rate {name} {zone:#x} {rate}\n")
            print(f"{len(lines)} zone_rate lines")
            return 0
        with open(args.file, "rb") as f:
            data = f.read()
        if args.cmd == "check":
            lines, ok = check(data)
            print("\n".join(lines))
            return 0 if ok else 2
        with open(args.tables, encoding="utf-8") as f:
            tables = json.load(f)
        notes = []
        out, done = apply(data, tables, load_zones(args.zones), args.max_scale, args.grow, args.in_place, notes)
        for n in notes:
            print(f"note: {n}", file=sys.stderr)
        with open(args.out, "wb") as f:
            f.write(out)
        print(f"{done} lists replaced, {len(out)} bytes")
        return 0
    except (OSError, ValueError, FormatError, struct.error) as e:
        print(f"encount_data: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
