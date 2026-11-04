"""Unit tests for tools/encounters/gen1_tables.py on a small synthetic CSV set (no real tables).

Run: python -m unittest -v test_gen1_tables (from tests/tools)
"""
import json
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "tools", "encounters"))
import gen1_tables as g1  # noqa: E402

FILES = {
    "versions.csv": "id,version_group_id,identifier\n1,1,red\n2,1,blue\n3,2,yellow\n",
    "encounter_methods.csv": "id,identifier,order\n1,walk,1\n2,surf,2\n3,gift,3\n",
    "encounter_slots.csv": "id,version_group_id,encounter_method_id,slot,rarity\n"
                           "10,1,1,1,20\n11,1,1,2,80\n20,2,1,1,20\n21,2,1,2,20\n22,2,1,3,60\n23,2,2,1,100\n24,2,3,1,100\n",
    "location_areas.csv": "id,location_id,game_index,identifier\n100,7,1,\n101,8,2,1f\n",
    "locations.csv": "id,region_id,identifier\n7,1,route-a\n8,1,cave-b\n",
    "location_area_encounter_rates.csv": "location_area_id,encounter_method_id,version_id,rate\n100,1,3,25\n",
    "encounters.csv": "id,version_id,location_area_id,encounter_slot_id,pokemon_id,min_level,max_level\n"
                      # red: ignored for yellow
                      "1,1,100,10,19,2,2\n"
                      # yellow route-a walk: species 16 in two slots (added up), 19 once
                      "2,3,100,20,16,3,3\n3,3,100,21,16,5,5\n4,3,100,22,19,2,4\n"
                      # yellow cave-b surf
                      "5,3,101,23,54,10,15\n"
                      # slot of another version group: ignored
                      "6,3,100,10,25,9,9\n"
                      # a gift: no random encounter
                      "7,3,100,24,133,25,25\n",
}


class Gen1TablesTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        for name, text in FILES.items():
            with open(os.path.join(self.tmp.name, name), "w", encoding="utf-8") as f:
                f.write(text)

    def tearDown(self):
        self.tmp.cleanup()

    def test_yellow(self):
        data = g1.build(self.tmp.name, "yellow")
        self.assertEqual(data["version"], "yellow")
        self.assertEqual([a["location"] for a in data["areas"]], ["cave-b", "route-a"])
        route = data["areas"][1]
        self.assertEqual(route["area"], "area")
        walk = route["methods"]["walk"]
        self.assertEqual(walk["rate"], 25)
        self.assertEqual(walk["slots"], [
            {"species": 19, "weight": 60, "min_level": 2, "max_level": 4},
            {"species": 16, "weight": 40, "min_level": 3, "max_level": 5},
        ])
        surf = data["areas"][0]["methods"]["surf"]
        self.assertIsNone(surf["rate"])
        self.assertEqual(surf["slots"], [{"species": 54, "weight": 100, "min_level": 10, "max_level": 15}])
        self.assertEqual(g1.summary(data), "yellow: 2 areas (surf 1, walk 1)")

    def test_red_and_cli(self):
        data = g1.build(self.tmp.name, "red")
        self.assertEqual(data["areas"][0]["methods"]["walk"]["slots"],
                         [{"species": 19, "weight": 20, "min_level": 2, "max_level": 2}])
        out = os.path.join(self.tmp.name, "out.json")
        self.assertEqual(g1.main([self.tmp.name, "--version", "yellow", "--out", out]), 0)
        with open(out, encoding="utf-8") as f:
            self.assertEqual(json.load(f)["areas"][1]["methods"]["walk"]["rate"], 25)
        self.assertEqual(g1.main([os.path.join(self.tmp.name, "nothing"), "--out", out]), 1)

    def test_walk_rates(self):
        rates = os.path.join(self.tmp.name, "rates.csv")
        with open(rates, "w", encoding="utf-8") as f:
            f.write("# from a test\nlocation/area,rate\nroute-a/area,20\n")
        out = os.path.join(self.tmp.name, "out.json")
        self.assertEqual(g1.main([self.tmp.name, "--out", out, "--walk-rates", rates]), 0)
        with open(out, encoding="utf-8") as f:
            self.assertEqual(json.load(f)["areas"][1]["methods"]["walk"]["rate"], 20)
        for bad in ("nowhere/area,10\n", "route-a/area,300\n", "cave-b/area,10\n"):
            with open(rates, "w", encoding="utf-8") as f:
                f.write(bad)
            self.assertEqual(g1.main([self.tmp.name, "--out", out, "--walk-rates", rates]), 1)


if __name__ == "__main__":
    unittest.main()
