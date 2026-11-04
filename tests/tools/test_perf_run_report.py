"""Unit test for tools/analyze/perf_run_report.py (synthetic logs only).

Run: python -m unittest -v test_perf_run_report (from tests/tools)
"""
import io
import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(__file__)
sys.path.insert(0, os.path.join(HERE, "..", "..", "tools", "analyze"))
import perf_run_report  # noqa: E402


def run_log(fps_a, fps_b, wb_a, wb_b, logcat=False):
    """What lgp_game --time-stats 300 --frame-stats 2700 prints for frames 2101-2700 (two windows in range
    2401-3000 would need three; the range used below is 2401-2700 plus one window outside)."""
    p = "10-14 18:00:00.123  1234  1250 I lgp     : " if logcat else ""
    lines = []
    for (first, last), fps, wb in (((2101, 2400), 30.0, 1.0), ((2401, 2700), fps_a, wb_a), ((2701, 3000), fps_b, wb_b)):
        wall = 300 / fps
        lines += [
            f"audout: 5.0 s appended (total 90.0 s), 2 channels, dropped 0 frames, underruns {3 if fps < 20 else 0}, "
            "rms/peak per channel 1/2 1/2",
            f"time: frames {first}-{last}: {wall:.2f} s wall, {fps:.1f} fps ({1000 / fps:.1f} ms per frame)",
            "time:   thread 1 (prio 44, core 0): guest 14.70, ready 5.00, wait 20.00, ipc 0.10, nvn 2.00, nvn-lock 0.00,"
            " gpu 0.00 ms/frame; gpu calls 0.0 (0 us each), ipc requests 3.0 (33 us each)",
            "time:   thread 3 (prio 44, core 1): guest 11.20, ready 1.00, wait 10.00, ipc 0.00, nvn 1.00, nvn-lock 0.00,"
            f" gpu {30 if fps < 20 else 10:.2f} ms/frame; gpu calls 81.0 (300 us each), ipc requests 0.0 (0 us each)",
            "time:   4 more threads (almost) only waited",
            f"time:   gpu thread: busy {60 if fps < 20 else 20:.2f} ms/frame (70 %), 2.0 calls (10000 us each)",
            f"gpu memory, frames {first}-{last}: buffers live 10 (12.5 MiB, 3 created, 0 destroyed), textures live 40 "
            "(~150.0 MiB), queue writes 1.00 MiB/frame, frames in flight 1 (max 2), presents waited 300 (2.0 ms/frame), "
            f"compute write-backs 300 (waited for 300, {wb:.1f} ms/frame), sync waits 0 (0 with write-backs pending)",
            f"gpu work, frames {first}-{last}: programs created 6 (292.0 ms), draw pipelines created 6 (292.0 ms; "
            "prebuilt waited 0, late 0, draws skipped while compiling 0), compute pipelines waited for 0 (0.0 ms), "
            "textures created 2, texture uploads 1.0 ms (0.50 MiB written)",
            f"gpu lists, frames {first}-{last}: lists 600, with dispatches 300; their draws before the first dispatch 10,"
            " between first and last 0, after the last 24000; compute first: dispatches 2071 ahead, 0 in order,"
            " lists 300, read back ahead 300",
            f"gpu time, frames {first}-{last}: compute 3.00 ms/frame (300 passes), render 37.40 ms/frame (600 passes),"
            " untimed 0; compute programs ms/frame: 00000000000000aa 2.50",
            f"gpu passes, frames {first}-{last}: render passes {17100 if fps < 20 else 6000} (57.0/frame), draw passes 9000 "
            "(2.7 draws each), new attachment sets 40; draw pass ended: render targets changed 5000, own pass: colour clear 900",
            f"host memory, frame {last}: footprint 1500.0 MiB, peak 1520.0 MiB, available 590.0 MiB",
        ]
        if last == 2700:
            lines.append("frame 2700: 1280x720, non-black 97.0 %, colours (sampled) 900, mean 90 100 80")
    return "".join(p + l + "\n" for l in lines)


SIMPLEPERF = """Cmdline: /data/local/tmp/simpleperf record -p 1234 --duration 20
Arch: arm64
Event: cpu-clock (type 1, config 1)
Samples: 80000
Event count: 20000000000

Overhead  Command   Shared Object       Symbol
20.00%    lgp_game  liblgp_game.so      lgp::gpu::webgpu::WebGpuBackend::draw(unsigned long)
15.00%    gpu       libwgpu_native.so   wgpu_core::command::render::encode
10.00%    lgp_game  liblgp_game.so      fn_0000000000123456
5.00%     gpu       libvulkan.so        vkCmdDraw
"""


def report(args):
    out = io.StringIO()
    rc = perf_run_report.main(args, out=out)
    return rc, out.getvalue()


class PerfRunReportTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()

    def tearDown(self):
        self.dir.cleanup()

    def write(self, name, text):
        path = os.path.join(self.dir.name, name)
        with open(path, "w", encoding="utf-8") as f:
            f.write(text)
        return path

    def test_range_totals(self):
        log = self.write("a.log", run_log(10.0, 30.0, 30.0, 2.0))
        rc, text = report([log, "--from", "2401", "--to", "3000"])
        self.assertEqual(rc, 0)
        self.assertIn(" 2401-2700 ", text)
        self.assertIn(" 2701-3000 ", text)
        self.assertNotIn(" 2101-2400 ", text)
        # 600 frames in 30 s + 10 s = 15 fps (not the mean of 10 and 30).
        self.assertIn("600 frames in 40.0 s, 15.00 fps", text)
        self.assertIn("slowest window 10.0 fps", text)
        # Write-back wait weighted by frames: (30 + 2) / 2.
        self.assertIn("write-back wait 16.00", text)
        self.assertIn("audio underruns 3", text)
        self.assertIn("thread 3: guest 11.2", text)
        self.assertIn("gpu thread busy 60.0", text)
        self.assertIn("frame 2700: 1280x720", text)
        self.assertIn("render passes 23100", text)
        self.assertIn("new attachment sets 80", text)

    def test_logcat_prefix(self):
        log = self.write("a.log", run_log(10.0, 30.0, 30.0, 2.0, logcat=True))
        rc, text = report([log, "--from", "2401", "--to", "3000"])
        self.assertIn("600 frames in 40.0 s, 15.00 fps", text)

    def test_range_not_covered(self):
        log = self.write("a.log", run_log(10.0, 30.0, 30.0, 2.0))
        rc, text = report([log, "--from", "2401", "--to", "3600"])
        self.assertIn("not exactly the range", text)
        rc, text = report([log, "--from", "3001", "--to", "3600"])
        self.assertIn("no --time-stats window inside the range", text)

    def test_abab(self):
        a1 = self.write("a1.log", run_log(10.0, 10.0, 30.0, 30.0))
        b1 = self.write("b1.log", run_log(20.0, 20.0, 5.0, 5.0))
        a2 = self.write("a2.log", run_log(11.0, 11.0, 28.0, 28.0))
        b2 = self.write("b2.log", run_log(21.0, 21.0, 6.0, 6.0))
        rc, text = report(["--group", f"A={a1},{a2}", "--group", f"B={b1},{b2}", "--from", "2401", "--to", "3000"])
        self.assertEqual(rc, 0)
        self.assertIn("== comparison, frames 2401-3000", text)
        line = next(l for l in text.splitlines() if l.strip().startswith("fps "))
        self.assertIn("10.50 ± 0.71", line)
        self.assertRegex(line, r"20\.(49|50) ± 0\.7[01]")
        self.assertRegex(text, r"B vs A: fps 10\.50 -> 20\.(49|50) \(\+95\.[12] %\), beyond spread, better")
        self.assertIn("B vs A: write-back wait 29.00 -> 5.50", text)
        self.assertRegex(text, r"B vs A: write-back wait .*beyond spread, better")

    def test_partial_run_left_out_of_groups(self):
        a1 = self.write("a1.log", run_log(10.0, 10.0, 30.0, 30.0))
        a2 = self.write("a2.log", run_log(11.0, 11.0, 28.0, 28.0))
        # Ends after frame 2700: the window 2701-3000 is missing.
        cut = run_log(20.0, 20.0, 5.0, 5.0)
        b1 = self.write("b1.log", cut[:cut.index("time: frames 2701-3000")])
        b2 = self.write("b2.log", run_log(21.0, 21.0, 6.0, 6.0))
        rc, text = report(["--group", f"A={a1},{a2}", "--group", f"B={b1},{b2}", "--from", "2401", "--to", "3000"])
        self.assertIn(f"group B: {b1} left out of the comparison (covers frames 2401-2700", text)
        self.assertNotIn(f"{b1} (300 frames)", text)
        self.assertIn(f"{b2} (600 frames)", text)
        # Without --to: the runs must cover the same frames as the first one.
        rc, text = report(["--group", f"A={a1},{a2}", "--group", f"B={b1},{b2}", "--from", "2401"])
        self.assertIn(f"group B: {b1} left out of the comparison", text)

    def test_single_run_group_has_no_spread(self):
        a1 = self.write("a1.log", run_log(10.0, 10.0, 30.0, 30.0))
        b1 = self.write("b1.log", run_log(10.2, 10.2, 29.0, 29.0))
        rc, text = report(["--group", f"A={a1}", "--group", f"B={b1}", "--from", "2401", "--to", "3000"])
        self.assertIn("group with one run has no spread", text)

    def test_simpleperf(self):
        sp = self.write("sp.txt", SIMPLEPERF)
        rc, text = report(["--simpleperf", sp, "--top", "3"])
        self.assertEqual(rc, 0)
        self.assertIn("Event cpu-clock (type 1, config 1), Samples 80000", text)
        self.assertIn("20.00 lgp_game | liblgp_game.so | lgp::gpu::webgpu::WebGpuBackend::draw(unsigned long)", text)
        self.assertNotIn("vkCmdDraw", text.split("per command")[0])
        self.assertIn("per command: lgp_game 30.0 %, gpu 20.0 %", text)
        self.assertIn("per shared object: liblgp_game.so 30.0 %, libwgpu_native.so 15.0 %", text)

    def test_simpleperf_children(self):
        sp = self.write("sp.txt", "Samples: 10\n\nChildren  Self    Command   Symbol\n"
                                  "90.00%    1.00%   lgp_game  main\n40.00%    40.00%  lgp_game  hot\n")
        rc, text = report(["--simpleperf", sp])
        self.assertIn(" 40.00 lgp_game | hot", text)
        self.assertLess(text.index(" 40.00 lgp_game | hot"), text.index("  1.00 lgp_game | main"))


if __name__ == "__main__":
    unittest.main()
