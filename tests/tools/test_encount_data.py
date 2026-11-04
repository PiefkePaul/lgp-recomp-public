"""Unit tests for tools/encounters/encount_data.py on synthetic spawn tables (no game data).

Run: python -m unittest -v test_encount_data (from tests/tools)
"""
import os
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "tools", "encounters"))
import encount_data as ed  # noqa: E402


def area(zone, ground=(), water=(), sky=(), max_count=4):
    a = {name: 0 for name, kind in ed.AREA if kind not in ("slots", "bool")}
    a.update({name: False for name, kind in ed.AREA if kind == "bool"})
    a["zone"] = zone
    for lname, slots in (("ground", ground), ("water", water), ("sky", sky)):
        a[lname + ".slots"] = [{"weight": w, "species": s, "form": 0} for s, w in slots]
        a[lname + ".weight_sum"] = sum(w for _, w in slots)
        a[lname + ".allowed"] = bool(slots)
        a[lname + ".max_count"] = max_count if slots else 0
        a[lname + ".rate"] = 10 if slots else 0
        a[lname + ".level_min"], a[lname + ".level_max"] = (3, 5) if slots else (0, 0)
    for lname in ("old_rod", "good_rod", "super_rod"):
        a[lname + ".slots"] = []
    a["_extra_fields"] = 0
    return a


def handmade():
    """A buffer laid out the other way round from our writer (vtables after their
    tables, only some fields present): root -> [area{zone, ground.slots=[slot]}]."""
    zone_field = 0
    slots_field = ed.AREA.index(("ground.slots", "slots"))
    b = bytearray(64 * 4)
    struct.pack_into("<I", b, 0, 16)                     # root table at 16
    # root table at 16: soffset to vtable at 24, field 0 at +4 -> vector at 32
    struct.pack_into("<i", b, 16, 16 - 24)
    struct.pack_into("<I", b, 20, 32 - 20)
    struct.pack_into("<HHH", b, 24, 6, 8, 4)
    # vector at 32: one area table at 40
    struct.pack_into("<II", b, 32, 1, 40 - 36)
    # area table at 40: soffset, zone (u64) at +8, slots ref at +16
    nf = slots_field + 1
    vt = 64
    struct.pack_into("<i", b, 40, 40 - vt)
    struct.pack_into("<Q", b, 48, 0x1122334455667788)
    struct.pack_into("<I", b, 56, 160 - 56)
    offs = [0] * nf
    offs[zone_field] = 8
    offs[slots_field] = 16
    struct.pack_into(f"<HH{nf}H", b, vt, 4 + 2 * nf, 20, *offs)
    # slot vector at 160 with one slot table at 168: weight 70, species 16, form absent
    struct.pack_into("<II", b, 160, 1, 168 - 164)
    struct.pack_into("<i", b, 168, 168 - 180)
    struct.pack_into("<ii", b, 172, 70, 16)
    struct.pack_into("<HHHH", b, 180, 8, 12, 4, 8)
    return bytes(b)


TABLES = {"version": "yellow", "areas": [
    {"location": "kanto-route-1", "area": "area", "methods": {
        "walk": {"slots": [{"species": 16, "weight": 70, "min_level": 2, "max_level": 7},
                           {"species": 19, "weight": 30, "min_level": 2, "max_level": 4}]}}},
    {"location": "sea-route", "area": "area", "methods": {
        "surf": {"slots": [{"species": 72, "weight": 100, "min_level": 30, "max_level": 40}]},
        "old-rod": {"slots": [{"species": 129, "weight": 100, "min_level": 5, "max_level": 5}]},
        "super-rod": {"slots": [{"species": 72, "weight": 50, "min_level": 15, "max_level": 25}]}}},
]}


def strip(x):
    """parse() output without the presence lists, to compare with area()."""
    if isinstance(x, list):
        return [strip(v) for v in x]
    if isinstance(x, dict):
        return {k: strip(v) for k, v in x.items() if k != "_present"}
    return x


def present(data, zone):
    """Fields the file has for the zone (from the vtable), without the zone id."""
    for a in ed.parse(data):
        if a["zone"] == zone:
            return set(a["_present"]) - {"zone"}
    raise KeyError(zone)


class EncountDataTest(unittest.TestCase):
    def test_reads_other_layout(self):
        areas = ed.parse(handmade())
        self.assertEqual(len(areas), 1)
        self.assertEqual(areas[0]["zone"], 0x1122334455667788)
        self.assertEqual(strip(areas[0]["ground.slots"]), [{"weight": 70, "species": 16, "form": 0}])
        self.assertEqual(areas[0]["ground.slots"][0]["_present"], ["weight", "species"])
        self.assertEqual(areas[0]["_present"], ["zone", "ground.slots"])
        self.assertEqual(areas[0]["ground.max_count"], 0)  # absent -> read as 0
        self.assertFalse(areas[0]["ground.allowed"])
        # an absent weight sum is not compared; with no list to compare the layout
        # stays unconfirmed
        lines, ok = ed.check(handmade())
        self.assertFalse(ok)
        self.assertIn("weight sum = sum of slot weights: 0/0 (lists without the field: 1)", lines)
        # a rewrite keeps exactly the fields that were there
        again = ed.serialize(areas)
        self.assertEqual(ed.parse(again), areas)
        self.assertEqual(present(again, 0x1122334455667788), {"ground.slots"})

    def test_roundtrip_and_check(self):
        areas = [area(1, ground=[(41, 75), (74, 25)], max_count=10), area(2, water=[(54, 100)], sky=[(16, 1)])]
        data = ed.serialize(areas)
        self.assertEqual(strip(ed.parse(data)), areas)
        lines, ok = ed.check(data)
        self.assertTrue(ok, lines)
        self.assertEqual(lines[0], "areas 2, lists with slots 3, slots 4")
        # A wrong sum is caught.
        areas[0]["ground.weight_sum"] = 99
        self.assertFalse(ed.check(ed.serialize(areas))[1])

    def test_presence_kept(self):
        # an explicit 0 stays explicit, an absent field stays absent
        a = area(1, ground=[(41, 60), (74, 40)])
        a["_present"] = ["zone", "ground.allowed", "ground.duration", "ground.weight_sum", "ground.slots"]
        a["ground.duration"] = 0
        data = ed.serialize([a])
        self.assertEqual(present(data, 1), {"ground.allowed", "ground.duration", "ground.weight_sum", "ground.slots"})
        out, _ = ed.apply(data, TABLES, [(1, "ground", ["kanto-route-1/area"])])
        self.assertEqual(present(out, 1), {"ground.allowed", "ground.duration", "ground.weight_sum", "ground.slots",
                                           "ground.level_min", "ground.level_max"})

    def test_spread(self):
        slots = [{"weight": 70, "species": 16, "form": 0}, {"weight": 30, "species": 19, "form": 0}]
        self.assertEqual([(s["species"], s["weight"]) for s in ed.spread(slots, 4)],
                         [(16, 18), (16, 17), (16, 35), (19, 30)])
        self.assertEqual([s["weight"] for s in ed.spread([dict(slots[0], weight=1)], 2)], [1, 1])

    def test_apply(self):
        b_area = area(0xB, water=[(54, 100)], max_count=3)
        b_area["_present"] = [n for n, _ in ed.AREA if n != "water.max_count" and (b_area[n] or n == "zone")]
        data = ed.serialize([area(0xA, ground=[(41, 50), (74, 30), (95, 20)], max_count=4), b_area])
        notes = []
        out, done = ed.apply(data, TABLES, [(0xA, "ground", ["kanto-route-1/area"])], max_scale=2, notes=notes)
        self.assertEqual(done, 1)
        a, b = ed.parse(out)
        # Route 1 has two species, the file three slots: Taubsi's 70 is split
        self.assertEqual([(s["species"], s["weight"]) for s in a["ground.slots"]], [(16, 35), (16, 35), (19, 30)])
        self.assertEqual((a["ground.weight_sum"], a["ground.level_min"], a["ground.level_max"]), (100, 2, 7))
        self.assertEqual(a["ground.max_count"], 8)
        # zone B has no max count field: it stays absent (the game's default is unknown)
        self.assertNotIn("water.max_count", present(out, 0xB))
        self.assertEqual(strip(b), strip(ed.parse(data)[1]))
        self.assertEqual(notes, ["--max-scale: 1 lists without a max count field left alone"])
        self.assertTrue(ed.check(out)[1])
        # more species than slots only with --grow
        with self.assertRaisesRegex(ValueError, "only 1 slots"):
            ed.apply(data, TABLES, [(0xB, "water", ["sea-route/area"])])
        out, _ = ed.apply(data, TABLES, [(0xB, "water", ["sea-route/area"])], grow=True, notes=notes)
        self.assertEqual(strip(ed.parse(out)[1]["water.slots"]), [{"weight": 150, "species": 72, "form": 0},
                                                                  {"weight": 100, "species": 129, "form": 0}])
        self.assertEqual(ed.parse(out)[1]["water.level_min"], 5)
        with self.assertRaises(ValueError):
            ed.apply(data, TABLES, [(0xC, "ground", ["kanto-route-1/area"])])
        with self.assertRaises(ValueError):
            ed.apply(data, TABLES, [(0xA, "ground", ["nowhere/area"])])

    def test_note_for_list_without_spawn_fields(self):
        a = area(1, sky=[(41, 50), (74, 50)])
        notes = []
        ed.apply(ed.serialize([a]), TABLES, [(1, "ground", ["kanto-route-1/area"])], grow=True, notes=notes)
        self.assertEqual(len(notes), 1)
        self.assertIn("no allowed/max_count/rate", notes[0])

    def test_in_place(self):
        data = ed.serialize([area(0xA, ground=[(41, 50), (74, 30), (95, 20)], max_count=4)])
        out, _ = ed.apply(data, TABLES, [(0xA, "ground", ["kanto-route-1/area"])], max_scale=2, in_place=True)
        self.assertEqual(len(out), len(data))
        (a,) = ed.parse(out)
        self.assertEqual([(s["species"], s["weight"]) for s in a["ground.slots"]], [(16, 35), (16, 35), (19, 30)])
        self.assertEqual((a["ground.weight_sum"], a["ground.level_max"], a["ground.max_count"]), (100, 7, 8))
        # a field the file leaves out cannot be patched
        b = area(0xA, ground=[(41, 50), (74, 50)])
        b["ground.level_min"] = 0
        with self.assertRaisesRegex(ValueError, "level_min: field not in the file"):
            ed.apply(ed.serialize([b]), TABLES, [(0xA, "ground", ["kanto-route-1/area"])], in_place=True)

    def test_shared_vtables(self):
        areas = [area(i, ground=[(41, 100)]) for i in range(1, 6)]
        data = ed.serialize(areas)
        self.assertEqual(strip(ed.parse(data)), areas)
        full = len(ed.AREA)
        self.assertLess(len(data), 5 * (4 + 2 * full) + 5 * 64)

    def test_zone_rates(self):
        tables = {"areas": [
            {"location": "kanto-route-1", "area": "area", "methods": {"walk": {"rate": 25, "slots": []}}},
            {"location": "mt-moon", "area": "1f", "methods": {"walk": {"rate": 10, "slots": []}}},
            {"location": "sea-route", "area": "area", "methods": {"surf": {"rate": 5, "slots": []}}},
        ]}
        zones = [(0xA, "ground", ["kanto-route-1/area"]), (0xB, "ground", ["mt-moon/1f", "kanto-route-1/area"]),
                 (0xC, "water", ["sea-route/area"]), (0xD, "ground", ["sea-route/area"])]
        notes = []
        self.assertEqual(ed.zone_rates(tables, zones, notes), [("kanto_route_1", 0xA, 25), ("mt_moon", 0xB, 18)])
        self.assertEqual(notes, ["zone 0xb: rates [10, 25], took 18", "zone 0xd: no walk rate in the tables, left out"])
        with self.assertRaises(ValueError):
            ed.zone_rates(tables, [(0xA, "ground", ["nowhere/area"])])
        with tempfile.TemporaryDirectory() as d:
            zpath = os.path.join(d, "zones.tsv")
            with open(zpath, "w", encoding="utf-8") as f:
                f.write("000000000000000a\tground\tkanto-route-1/area\n")
            tpath = os.path.join(d, "t.json")
            with open(tpath, "w", encoding="utf-8") as f:
                import json
                json.dump(tables, f)
            out = os.path.join(d, "rates.cfg")
            self.assertEqual(ed.main(["rates", "--tables", tpath, "--zones", zpath, "--out", out]), 0)
            with open(out, encoding="utf-8") as f:
                self.assertIn("zone_rate kanto_route_1 0xa 25\n", f.read())

    def test_zone_slots(self):
        tables = {"areas": [
            {"location": "kanto-route-1", "area": "area", "methods": {"walk": {"rate": 25, "slots": [
                {"species": 16, "weight": 55, "min_level": 2, "max_level": 5},
                {"species": 19, "weight": 45, "min_level": 2, "max_level": 4}]}}},
            {"location": "mt-moon", "area": "1f", "methods": {"walk": {"rate": 10, "slots": [
                {"species": 41, "weight": 100, "min_level": 8, "max_level": 8}]}}},
            {"location": "sea-route", "area": "area", "methods": {"surf": {"rate": 5, "slots": []}}},
        ]}
        zones = [(0xA, "ground", ["kanto-route-1/area"]), (0xB, "ground", ["mt-moon/1f", "kanto-route-1/area"]),
                 (0xC, "water", ["sea-route/area"]), (0xD, "ground", ["sea-route/area"])]
        notes = []
        self.assertEqual(ed.zone_slots(tables, zones, notes), [
            ("kanto_route_1", 0xA, [(55, 16, 2, 5), (45, 19, 2, 4)]),
            ("mt_moon", 0xB, [(100, 41, 8, 8), (55, 16, 2, 5), (45, 19, 2, 4)])])
        self.assertEqual(notes, ["zone 0xb: slots of 2 areas together",
                                 "zone 0xd: no walking slots in the tables, left out"])
        with tempfile.TemporaryDirectory() as d:
            zpath = os.path.join(d, "zones.tsv")
            with open(zpath, "w", encoding="utf-8") as f:
                f.write("000000000000000a\tground\tkanto-route-1/area\n")
            tpath = os.path.join(d, "t.json")
            with open(tpath, "w", encoding="utf-8") as f:
                import json
                json.dump(tables, f)
            out = os.path.join(d, "slots.cfg")
            self.assertEqual(ed.main(["slots", "--tables", tpath, "--zones", zpath, "--out", out]), 0)
            with open(out, encoding="utf-8") as f:
                text = f.read()
            self.assertIn("slot kanto_route_1 0xa 55 16 2-5\n", text)
            self.assertIn("slot kanto_route_1 0xa 45 19 2-4\n", text)

    def test_cli(self):
        with tempfile.TemporaryDirectory() as d:
            src = os.path.join(d, "in.bin")
            with open(src, "wb") as f:
                f.write(ed.serialize([area(0xA, ground=[(41, 60), (74, 40)])]))
            zones = os.path.join(d, "zones.tsv")
            with open(zones, "w", encoding="utf-8") as f:
                f.write("# zone\tlist\tareas\n000000000000000a\tground\tkanto-route-1/area\n")
            tables = os.path.join(d, "t.json")
            with open(tables, "w", encoding="utf-8") as f:
                import json
                json.dump(TABLES, f)
            out = os.path.join(d, "out.bin")
            self.assertEqual(ed.main(["check", src]), 0)
            for mode in ([], ["--in-place"]):
                self.assertEqual(ed.main(["apply", src, "--tables", tables, "--zones", zones, "--out", out] + mode), 0)
                with open(out, "rb") as f:
                    self.assertEqual(ed.parse(f.read())[0]["ground.slots"][0]["species"], 16)
            with open(zones, "w", encoding="utf-8") as f:
                f.write("0a ground\n")
            self.assertEqual(ed.main(["apply", src, "--tables", tables, "--zones", zones, "--out", out]), 1)


if __name__ == "__main__":
    unittest.main()
