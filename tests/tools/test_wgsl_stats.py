"""Unit test for tools/analyze/wgsl_stats.py (synthetic WGSL and logs only).

Run: python -m unittest -v test_wgsl_stats (from tests/tools)
"""
import io
import os
import sys
import tempfile
import unittest
from contextlib import redirect_stdout

HERE = os.path.dirname(__file__)
sys.path.insert(0, os.path.join(HERE, "..", "..", "tools", "analyze"))
import wgsl_stats  # noqa: E402

# The shape maxwell2wgsl gives code with branches and a barrier: helper
# switches with one-line cases, a dispatcher loop with one case per block.
DISPATCH = """fn gmem_ld(s: vec2<u32>, k: u32) -> u32 {
    switch s.x {
        case 0u: { r = gm0[w]; }
        default: {}
    }
}
var<private> lmem: array<u32, 1024>;
var<private> r0: u32;
var<private> r1: u32;
@compute @workgroup_size(64) fn main() {
    var pc: u32 = 0u;
    var ssy_stack: array<u32, 16>;
    var ls_state: u32 = 0u;
    loop {
        switch pc {
            case 0u: {
                // gmem search: address from data
                r0 = gmem_ld(gmem_addr(r0, r1, 0u), 0u);
                pc = 1u;
            }
            case 1u: {
                // gmem search: address from several sources
                gmem_st(gmem_addr(r0, r1, 4u), 0u, r0);
                gm1_st(gmem_addr_at(r0, r1, 8u, gs1), 0u, r0);
                pc = 2u;
            }
            default: {}
        }
    }
}
"""
STRAIGHT = "var<private> r0: u32;\n@compute @workgroup_size(1) fn main() {\n}\n"
HASH_A = "00000000000000aa"
HASH_B = "00000000000000bb"


class WgslStats(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.addCleanup(self.dir.cleanup)

    def write(self, name, text):
        path = os.path.join(self.dir.name, name)
        with open(path, "w") as f:
            f.write(text)
        return path

    def test_shape(self):
        s = wgsl_stats.shape(DISPATCH)
        self.assertEqual(s["blocks"], 2)
        self.assertTrue(s["lockstep"])
        self.assertEqual(s["loops"], 1)
        self.assertEqual(s["gmem_calls"], 4)  # two loads/stores, two address calls; the definition not counted
        self.assertEqual((s["gmem_search"], s["gmem_direct"]), (2, 1))
        self.assertEqual(s["search_why"], "data 1, mixed 1")
        self.assertEqual(s["registers"], 2)
        self.assertEqual(s["private_words"], 1040)
        s = wgsl_stats.shape(STRAIGHT)
        self.assertEqual((s["blocks"], s["lockstep"], s["loops"], s["gmem_calls"]), (0, False, 0, 0))

    def test_dump_and_logs(self):
        self.write("prog69-%s.cs.wgsl" % HASH_A, DISPATCH)
        self.write("prog70-%s.cs.wgsl" % HASH_B, STRAIGHT)
        self.write("prog3.vs.wgsl", STRAIGHT)  # older dumps: no hash in the name
        self.write("prog3.fs.wgsl", "")  # ... and an empty file for a missing stage
        self.write("prog4-%s.fs.wgsl" % HASH_B, STRAIGHT)
        android = self.write("android.log",
                             "[lgp info] nvn webgpu: compute pipeline %s (WGSL 900 bytes) ready after 56800.0 ms (async)\n"
                             "[lgp info] nvn webgpu: compute pipeline %s (WGSL 900 bytes) ready after 12.5 ms (async)\n"
                             % (HASH_A, HASH_A))
        pc = self.write("pc.log",
                        "[lgp info] nvn webgpu: compute program 70 (%s, WGSL 80 bytes) created in 3000.0 ms "
                        "(after present 2)\n"
                        "gpu time, frames 1-300: compute 3.00 ms/frame (90 passes), render 9.00 ms/frame (900 passes), "
                        "untimed 0; compute programs ms/frame: %s 2.50, %s 0.50\n"
                        "gpu time, frames 301-600: compute 0.10 ms/frame (9 passes), render 9.00 ms/frame (900 passes), "
                        "untimed 0; compute programs ms/frame: %s 0.10\n" % (HASH_B, HASH_B, HASH_A, HASH_B))
        self.assertEqual(wgsl_stats.read_log(pc), ({HASH_B: 3000.0}, {HASH_B: 2.5, HASH_A: 0.5}, {}))
        self.assertEqual(wgsl_stats.read_log(android), ({HASH_A: 56800.0}, {}, {}))
        self.assertEqual(len(wgsl_stats.read_dump(self.dir.name, False)), 2)
        graphics = [r for r in wgsl_stats.read_dump(self.dir.name, True) if not r[0].startswith("compute")]
        self.assertEqual([(label, h) for label, h, _ in graphics], [("vertex 3", None), ("fragment 4", HASH_B)])
        out = io.StringIO()
        with redirect_stdout(out):
            rc = wgsl_stats.main([self.dir.name, "--log", "android=" + android, "--log", "pc=" + pc])
        self.assertEqual(rc, 0)
        lines = out.getvalue().splitlines()
        self.assertIn("android ms", lines[0])
        self.assertTrue(lines[0].endswith("| pc gpu ms/frame |"))  # only logs with GPU times get the column
        self.assertTrue(lines[2].startswith("| compute 69 | %s |" % HASH_A))  # slowest first
        self.assertTrue(lines[2].endswith("| 56800.0 | - | 0.50 |"))
        self.assertTrue(lines[3].endswith("| - | 3000.0 | 2.50 |"))

    def test_draw_pipelines(self):
        self.write("prog4-%s.vs.wgsl" % HASH_B, STRAIGHT)
        self.write("prog4-%s.fs.wgsl" % HASH_B, STRAIGHT)
        self.write("prog5-%s.fs.wgsl" % HASH_A, DISPATCH)
        mac = self.write("mac.log",
                         "[lgp info] nvn webgpu: draw pipeline %s (WGSL 900 bytes) ready after 100.0 ms (on use)\n"
                         "[lgp info] nvn webgpu: draw pipeline %s (WGSL 900 bytes) ready after 20.5 ms (worker)\n"
                         "[lgp info] nvn webgpu: draw pipeline %s (WGSL 80 bytes) ready after 7.0 ms (async)\n"
                         % (HASH_A, HASH_A, HASH_B))
        self.assertEqual(wgsl_stats.read_log(mac), ({}, {}, {HASH_A: [120.5, 2], HASH_B: [7.0, 1]}))
        out = io.StringIO()
        with redirect_stdout(out):
            self.assertEqual(wgsl_stats.main([self.dir.name, "--all", "--log", "mac=" + mac]), 0)
        lines = out.getvalue().splitlines()
        self.assertTrue(lines[0].endswith("| mac ms | mac draw ms (n) |"))
        self.assertTrue(lines[2].startswith("| fragment 5 | %s |" % HASH_A))  # most draw pipeline time first
        self.assertTrue(lines[2].endswith("| - | 120.5 (2) |"))
        self.assertTrue(lines[3].endswith("| - | 7.0 (1) |"))

    def test_empty_dir(self):
        self.assertEqual(wgsl_stats.main([self.dir.name]), 1)

    def test_profile(self):
        path = self.write("one.vs.wgsl", "var<private> r0: u32;\nfn main() {\n"
                          "    r0 = bitcast<u32>(cb0[3u].x) + bitcast<u32>(cb0[(r0 >> 4u)].y);\n"
                          "    if (r0 > 1u) { r0 = max(r0, 2u); }\n}\n")
        rows = dict(wgsl_stats.profile(open(path).read(), 10))
        self.assertEqual(rows["cbuf reads, constant index"], 1)
        self.assertEqual(rows["cbuf reads, run-time index"], 1)
        self.assertEqual(rows["call bitcast"], 2)
        self.assertEqual(rows["call max"], 1)
        self.assertEqual(rows["if"], 1)
        self.assertEqual(rows["var<private>"], 1)
        out = io.StringIO()
        with redirect_stdout(out):
            self.assertEqual(wgsl_stats.main(["--profile", path]), 0)
        self.assertIn("call bitcast", out.getvalue())


if __name__ == "__main__":
    unittest.main()
