"""Unit tests for tools/battle/sesd.py on synthetic sequences (no game data).

Run: python -m unittest -v test_sesd (from tests/tools)
"""
import contextlib
import io
import json
import os
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "tools", "battle"))
import sesd  # noqa: E402

# Made-up lengths, not the game's table.
LENGTHS = {0x4E: 16, 0x6A: 8, 0x31: 0, 0x70: 4, 0x71: 4}


def synthetic():
    records = [
        {"start": 0, "end": 10, "group": 7, "channel": 0, "flags": 1, "cmd": 0x4E,
         "payload": struct.pack("<IIII", 1, 0, 2, 4)},
        {"start": 5, "end": 5, "group": 7, "channel": 0, "flags": 0, "cmd": 0x6A,
         "payload": b"fall\0\0\0\0"},
        {"start": 20, "end": 20, "group": 1, "channel": 2, "flags": 0, "cmd": 0x31, "payload": b""},
    ]
    return sesd.serialize(30, records, LENGTHS), records


class SesdTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = self.tmp.name
        self.lengths = os.path.join(self.dir, "lengths.json")
        with open(self.lengths, "w", encoding="utf-8") as f:
            json.dump({f"{k:#x}": v for k, v in LENGTHS.items()}, f)

    def tearDown(self):
        self.tmp.cleanup()

    def run_main(self, *argv):
        out = io.StringIO()
        err = io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = sesd.main(list(argv))
        return code, out.getvalue(), err.getvalue()

    def test_roundtrip(self):
        data, records = synthetic()
        self.assertEqual(data[:4], b"SESD")
        self.assertEqual(struct.unpack_from("<i", data, len(data) - 4)[0], -1)
        version, frames, parsed = sesd.parse(data, LENGTHS)
        self.assertEqual((version, frames), (3, 30))
        self.assertEqual(parsed, records)
        self.assertEqual(sesd.serialize(frames, parsed, LENGTHS), data)

    def test_parse_errors(self):
        data, _ = synthetic()
        with self.assertRaisesRegex(sesd.SesdError, "magic"):
            sesd.parse(b"XXXX" + data[4:], LENGTHS)
        with self.assertRaisesRegex(sesd.SesdError, "no length"):
            sesd.parse(data, {0x4E: 16, 0x6A: 8})
        with self.assertRaisesRegex(sesd.SesdError, "end marker"):
            sesd.parse(data[:-4], LENGTHS)
        with self.assertRaisesRegex(sesd.SesdError, "after the end marker"):
            sesd.parse(data + b"\0", LENGTHS)
        with self.assertRaisesRegex(sesd.SesdError, "cut off"):
            sesd.parse(data[:16 + 12 + 4] + b"\0" * 4, LENGTHS)

    def test_serialize_checks_length(self):
        with self.assertRaisesRegex(sesd.SesdError, "payload 2 bytes"):
            sesd.serialize(10, [{"start": 0, "end": 0, "group": 0, "channel": 0, "flags": 0,
                                 "cmd": 0x6A, "payload": b"ab"}], LENGTHS)

    def test_fields(self):
        payload = sesd.fill_fields(["u8:1", "u16:0x203", "f32:1.5", "text:ab"], 16)
        self.assertEqual(payload[:3], b"\x01\x03\x02")
        self.assertEqual(struct.unpack_from("<f", payload, 3)[0], 1.5)
        self.assertEqual(payload[7:10], b"ab\0")
        self.assertEqual(payload[10:], bytes(6))
        with self.assertRaisesRegex(sesd.SesdError, "more than"):
            sesd.fill_fields(["text:abcdefgh"], 8)
        with self.assertRaisesRegex(sesd.SesdError, "unknown kind"):
            sesd.fill_fields(["x:1"], 8)

    def test_build_from_and_fields(self):
        data, records = synthetic()
        with open(os.path.join(self.dir, "src.bseq"), "wb") as f:
            f.write(data)
        spec = {"frames": 40, "records": [
            {"from": "src.bseq", "index": 0},
            {"from": "src.bseq", "index": 2, "start": 35, "end": 35},
            {"cmd": "0x6a", "start": 12, "end": 12, "group": 7, "fields": ["text:yure"]},
        ]}
        spec_path = os.path.join(self.dir, "spec.json")
        with open(spec_path, "w", encoding="utf-8") as f:
            json.dump(spec, f)
        out = os.path.join(self.dir, "out.bseq")
        code, stdout, err = self.run_main("build", spec_path, "--lengths", self.lengths, "--out", out)
        self.assertEqual(code, 0, err)
        self.assertIn("3 records, 40 frames", stdout)
        with open(out, "rb") as f:
            _, frames, built = sesd.parse(f.read(), LENGTHS)
        self.assertEqual(frames, 40)
        self.assertEqual(built[0], records[0])
        self.assertEqual((built[1]["start"], built[1]["cmd"]), (35, 0x31))
        self.assertEqual(built[2]["payload"], b"yure\0\0\0\0")
        self.assertEqual(built[2]["group"], 7)

        code, _, err = self.run_main("build", spec_path, "--lengths", self.lengths, "--out", out)
        self.assertEqual(code, 1)
        self.assertIn("exists", err)

    def test_build_errors(self):
        lengths = sesd.load_lengths(self.lengths)
        with self.assertRaisesRegex(sesd.SesdError, "after the 10 frames"):
            sesd.build({"frames": 10, "records": [{"cmd": "0x31", "end": 11}]}, self.dir, lengths)
        with self.assertRaisesRegex(sesd.SesdError, "no length"):
            sesd.build({"frames": 10, "records": [{"cmd": "0x99"}]}, self.dir, lengths)
        with self.assertRaisesRegex(sesd.SesdError, "positive"):
            sesd.build({"frames": 0}, self.dir, lengths)
        with self.assertRaisesRegex(sesd.SesdError, "group 3 already has a sound"):
            sesd.build({"frames": 10, "records": [{"cmd": "0x70", "group": 3},
                                                  {"cmd": "0x6a", "group": 3},
                                                  {"cmd": "0x71", "group": 3}]}, self.dir, lengths)
        sesd.build({"frames": 10, "records": [{"cmd": "0x70", "group": 3}, {"cmd": "0x70", "group": 4},
                                              {"cmd": "0x6a", "group": 3}]}, self.dir, lengths)

    def test_dump(self):
        data, _ = synthetic()
        path = os.path.join(self.dir, "a.bseq")
        with open(path, "wb") as f:
            f.write(data)
        code, out, err = self.run_main("dump", path, "--lengths", self.lengths)
        self.assertEqual(code, 0, err)
        self.assertIn("SESD version 3, 30 frames, 3 records", out)
        self.assertIn("ball create", out)
        self.assertNotIn("66616c6c", out)
        code, out, _ = self.run_main("dump", path, "--lengths", self.lengths, "--payload")
        self.assertIn("66616c6c", out)


if __name__ == "__main__":
    unittest.main()
