#!/usr/bin/env python3
"""Wild encounter tables of Pokémon Red/Blue/Yellow from PokeAPI's CSV data.

Reads a local checkout of https://github.com/PokeAPI/pokeapi (BSD-3-Clause,
data/v2/csv) and writes one JSON with, per location area and method (walk,
surf, rods), the species with level range and weight in percent (slots of the
same species added up), and the area's encounter rate if the CSV has one.

The output describes another Nintendo game's data and goes to
$LGP_PRIVATE_DIR (like the mod files), not into the repository. It is the
input for the spawn tables of the mod original-encounters
(docs/analysis/54-zufallsbegegnungen.md).

    python3 tools/encounters/gen1_tables.py <pokeapi>/data/v2/csv --version yellow --out yellow.json [--walk-rates rates.csv]

PokeAPI has no encounter rates for Red/Blue/Yellow (location_area_encounter_rates.csv
starts at Gold/Silver), so `--walk-rates` takes them from a CSV
`location/area,rate` (rate per step out of 256, e.g. from pret/pokeyellow
data/wild/maps/*.asm, the grass rate of def_grass_wildmons); it fills the
walk rate of the areas it names and fails on an area the tables do not have.
"""
import argparse
import csv
import json
import os
import sys
from collections import defaultdict

VERSIONS = ("red", "blue", "yellow")
# Random encounters only: no gifts, trades or fixed (static) Pokémon.
METHODS = ("walk", "surf", "old-rod", "good-rod", "super-rod")


def read_csv(directory, name):
    path = os.path.join(directory, name)
    with open(path, newline="", encoding="utf-8") as f:
        return list(csv.DictReader(f))


def build(csv_dir, version):
    versions = {r["identifier"]: r for r in read_csv(csv_dir, "versions.csv")}
    if version not in versions:
        raise ValueError(f"version {version} not in versions.csv")
    version_id = versions[version]["id"]
    group_id = versions[version]["version_group_id"]

    methods = {r["id"]: r["identifier"] for r in read_csv(csv_dir, "encounter_methods.csv")}
    slots = {r["id"]: r for r in read_csv(csv_dir, "encounter_slots.csv") if r["version_group_id"] == group_id}
    areas = {r["id"]: r for r in read_csv(csv_dir, "location_areas.csv")}
    locations = {r["id"]: r["identifier"] for r in read_csv(csv_dir, "locations.csv")}
    rates = {}
    if os.path.exists(os.path.join(csv_dir, "location_area_encounter_rates.csv")):
        for r in read_csv(csv_dir, "location_area_encounter_rates.csv"):
            if r["version_id"] == version_id:
                rates[(r["location_area_id"], methods.get(r["encounter_method_id"], "?"))] = int(r["rate"])

    # (area, method) -> species -> [weight, min, max]
    table = defaultdict(dict)
    for r in read_csv(csv_dir, "encounters.csv"):
        if r["version_id"] != version_id or r["encounter_slot_id"] not in slots:
            continue
        slot = slots[r["encounter_slot_id"]]
        method = methods.get(slot["encounter_method_id"], "?")
        if method not in METHODS:
            continue
        key = (r["location_area_id"], method)
        species = int(r["pokemon_id"])
        weight = int(slot["rarity"] or 0)
        lo, hi = int(r["min_level"]), int(r["max_level"])
        entry = table[key].get(species)
        if entry is None:
            table[key][species] = [weight, lo, hi]
        else:
            entry[0] += weight
            entry[1] = min(entry[1], lo)
            entry[2] = max(entry[2], hi)

    out_areas = defaultdict(lambda: {"methods": {}})
    for (area_id, method), species in table.items():
        area = areas.get(area_id, {})
        a = out_areas[area_id]
        a["area"] = area.get("identifier", "") or "area"
        a["location"] = locations.get(area.get("location_id", ""), "")
        a["methods"][method] = {
            "rate": rates.get((area_id, method)),
            "slots": [{"species": s, "weight": w, "min_level": lo, "max_level": hi}
                      for s, (w, lo, hi) in sorted(species.items(), key=lambda kv: (-kv[1][0], kv[0]))],
        }
    result = [dict(v, area_id=int(k)) for k, v in out_areas.items()]
    result.sort(key=lambda a: (a["location"], a["area"], a["area_id"]))
    return {"source": "PokeAPI data/v2/csv (BSD-3-Clause)", "version": version, "areas": result}


def apply_walk_rates(data, path):
    """Walk rates from a CSV of location/area,rate; returns how many were set."""
    by_name = {f"{a['location']}/{a['area']}": a for a in data["areas"]}
    done = 0
    with open(path, newline="", encoding="utf-8") as f:
        for n, row in enumerate(csv.reader(f), 1):
            if not row or row[0].startswith("#") or row[0] == "location/area":
                continue
            if len(row) != 2 or not row[1].strip().isdigit() or int(row[1]) > 255:
                raise ValueError(f"{path}:{n}: want location/area,rate (0-255)")
            name = row[0].strip()
            if name not in by_name:
                raise ValueError(f"{path}:{n}: no area {name} in the tables")
            walk = by_name[name]["methods"].get("walk")
            if walk is None:
                raise ValueError(f"{path}:{n}: {name} has no walk encounters")
            walk["rate"] = int(row[1])
            done += 1
    return done


def summary(data):
    methods = defaultdict(int)
    for a in data["areas"]:
        for m in a["methods"]:
            methods[m] += 1
    parts = ", ".join(f"{m} {n}" for m, n in sorted(methods.items()))
    return f"{data['version']}: {len(data['areas'])} areas ({parts})"


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("csv_dir", help="PokeAPI data/v2/csv")
    ap.add_argument("--version", choices=VERSIONS, default="yellow")
    ap.add_argument("--out", required=True, help="JSON file (to $LGP_PRIVATE_DIR)")
    ap.add_argument("--walk-rates", help="CSV location/area,rate: walk rates PokeAPI lacks for Gen 1")
    args = ap.parse_args(argv)
    try:
        data = build(args.csv_dir, args.version)
        if args.walk_rates:
            print(f"{apply_walk_rates(data, args.walk_rates)} walk rates from {args.walk_rates}")
    except (OSError, KeyError, ValueError) as e:
        print(f"gen1_tables: {e}", file=sys.stderr)
        return 1
    with open(args.out, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=1, ensure_ascii=False)
        f.write("\n")
    print(summary(data))
    return 0


if __name__ == "__main__":
    sys.exit(main())
