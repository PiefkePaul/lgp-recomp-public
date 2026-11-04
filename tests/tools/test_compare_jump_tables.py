"""Unit test for tools/analyze/compare_jump_tables.py (synthetic JSON only).

Run: python -m unittest -v test_compare_jump_tables (from tests/tools)
"""
import json
import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(__file__)
sys.path.insert(0, os.path.join(HERE, "..", "..", "tools", "analyze"))
import compare_jump_tables as cjt  # noqa: E402


def table(br, targets, bounded=True):
    return {"br": hex(br), "table": "0x0", "count": len(targets), "bound_known": bounded,
            "targets": [hex(t) for t in targets]}


class CompareJumpTables(unittest.TestCase):
    def write(self, d):
        f = tempfile.NamedTemporaryFile("w", suffix=".json", delete=False)
        json.dump(d, f)
        f.close()
        self.addCleanup(os.unlink, f.name)
        return f.name

    def test_counts(self):
        before = {"functions": [{"jump_tables": [table(0x100, [0x110, 0x120]), table(0x200, [0x210], False)]}],
                  "diagnostics": [{"kind": "unresolved_indirect_jump", "pc": "0x300"},
                                  {"kind": "jump_table_unbounded", "pc": "0x200"}]}
        after = {"functions": [{"jump_tables": [table(0x100, [0x110, 0x120]), table(0x200, [0x210, 0x214]),
                                                table(0x300, [0x310, 0x320, 0x310])]}],
                 "diagnostics": []}
        r = cjt.compare(cjt.load(self.write(before)), cjt.load(self.write(after)))
        self.assertEqual(r["tables"], (2, 3))
        self.assertEqual(r["bounded"], (1, 3))
        self.assertEqual(r["unresolved"], (1, 0))
        self.assertEqual(r["new"], [0x300])
        self.assertEqual(r["new_from_unresolved"], [0x300])
        self.assertEqual(r["lost"], [])
        self.assertEqual(r["changed"], [0x200])
        self.assertEqual(r["new_targets"], 3)  # 0x214, 0x310, 0x320


if __name__ == "__main__":
    unittest.main()
