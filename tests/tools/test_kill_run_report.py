"""Unit test for tools/analyze/kill_run_report.py (synthetic logs only).

Run: python -m unittest -v test_kill_run_report (from tests/tools)
"""
import io
import json
import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(__file__)
sys.path.insert(0, os.path.join(HERE, "..", "..", "tools", "analyze"))
import kill_run_report  # noqa: E402

# What lgp_game --time-stats 100 prints, cut short as by a signal 9.
CONSOLE = """boot: lgp_game 0.1
time stats: clock ticks every 1 us
time: frames 1-100: 4.10 s wall, 24.4 fps (41.0 ms per frame)
time:   thread 3 (prio 44, core 0): guest 20.00 ms/frame
gpu memory, frames 1-100: buffers live 10 (12.5 MiB, 3 created, 0 destroyed), textures live 40 (~150.0 MiB), queue writes 1.00 MiB/frame
host memory parts, frame 100 (MiB): footprint 1500.0, ledger graphics 862.0, IOAccelerator 173.0, MALLOC_SMALL 380.0
host memory, frame 100: footprint 1500.0 MiB, peak 1520.0 MiB, available 590.0 MiB, guest resident 400.0 MiB
time: frames 101-200: 4.00 s wall, 25.0 fps (40.0 ms per frame)
gpu memory, frames 101-200: buffers live 11 (13.0 MiB, 1 created, 0 destroyed), textures live 44 (~170.0 MiB), queue writes 1.00 MiB/frame
host memory, frame 200: footprint 1600.0 MiB, peak 1900.0 MiB, available 480.0 MiB, guest resident 550.0 MiB, metal allocated 700.0 MiB (working set 2900.0 MiB), purgeable 12.0 MiB, volatile 450.0 MiB, system free 40.0 MiB, file cache 900.0 MiB, compressed 300.0 MiB, level 4 %
host memory parts, frame 200 (MiB): footprint 1600.0, ledger graphics 1338.0, IOAccelerator 631.0, MALLOC_SMALL 385.0
fs: save committed
"""

SYSTEM = """2026-10-14 18:00:00 kernel: memorystatus: killing_specific_process pid 812 [lgp_game] (per-process-limit 100) 2098000KB
2026-10-14 18:00:00 SpringBoard: unrelated line
2026-10-14 18:00:01 runningboardd: [app<de.piefkepaul.lgp.game>:812] Process exited: signal 9 lgp_game
"""


def run(args):
    out = io.StringIO()
    kill_run_report.main(args, out=out)
    return out.getvalue()


class KillRunReportTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.addCleanup(self.dir.cleanup)

    def write(self, name, text):
        path = os.path.join(self.dir.name, name)
        with open(path, "w", encoding="utf-8") as f:
            f.write(text)
        return path

    def test_memory_curve(self):
        text = run([self.write("c.txt", CONSOLE), "--limit-mib", "2098"])
        self.assertIn("last frame in the output: 200", text)
        self.assertIn("498 MiB below the limit of 2098 MiB", text)
        self.assertIn("guest resident: 400 -> 550 MiB", text)
        self.assertIn("metal allocated: 700 -> 700 MiB (max 700 at frame 200; working set 2900 MiB)", text)
        self.assertIn("purgeable: nonvolatile 12 MiB, volatile 450 MiB at frame 200", text)
        self.assertIn("system: free 40 MiB, file cache 900 MiB, compressed 300 MiB, level 4 % at frame 200", text)
        # Peak 1900 between samples 1500 and 1600: a spike of 300 MiB.
        self.assertIn("frames 100-200: 300 MiB above both samples", text)
        self.assertIn("fs: save committed", text)
        self.assertNotIn("  host memory", text)
        # Largest rise first; the total footprint is not a part.
        self.assertIn("parts with the largest rise", text)
        self.assertIn("  ledger graphics: +476 at frames 100-200; 862 -> 1338, max 1338 at frame 200", text)
        self.assertLess(text.index("ledger graphics: +"), text.index("IOAccelerator: +458"))
        self.assertLess(text.index("IOAccelerator: +458"), text.index("MALLOC_SMALL: +5"))
        self.assertNotIn("  footprint: +", text)

    def test_system_and_jetsam(self):
        jetsam = json.dumps({"bug_type": "298"}) + "\n" + json.dumps({
            "largestProcess": "lgp_game",
            "memoryStatus": {"pageSize": 16384},
            "processes": [{"name": "lgp_game", "reason": "per-process-limit", "rpages": 134272},
                          {"name": "other", "rpages": 10}]})
        text = run([self.write("c.txt", CONSOLE), "--system", self.write("s.txt", SYSTEM),
                    "--jetsam", self.write("j.ips", jetsam)])
        self.assertIn("kill reasons naming lgp_game: per-process-limit (1)", text)
        self.assertIn("signal 9", text)
        self.assertNotIn("unrelated", text)
        self.assertIn("lgp_game: reason per-process-limit, 2098 MiB resident", text)
        self.assertNotIn("other:", text)

    def test_without_stats(self):
        text = run([self.write("c.txt", "boot: start\n")])
        self.assertIn("no 'host memory' lines", text)


if __name__ == "__main__":
    unittest.main()
