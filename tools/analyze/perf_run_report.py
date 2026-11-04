#!/usr/bin/env python3
"""Frame range report and A/B comparison from lgp_game logs (--time-stats N).

Reads what `lgp_game ... --time-stats N [--frame-stats ...]` prints (stdout, or
`adb logcat` with its prefixes) and, if given, `simpleperf report` text:

  perf_run_report.py RUN.log [RUN.log ...] [--from 2401] [--to 3600]
  perf_run_report.py --group A=a1.log,a2.log --group B=b1.log,b2.log [--from ... --to ...]
  perf_run_report.py RUN.log --simpleperf report.txt [--top 25]

Per log: one row per --time-stats window inside [--from, --to] (fps, write-back
and present waits, GPU time, pipelines, the busiest threads and sources, audio
underruns), then the totals over the range (fps = frames / wall time, other
values weighted by frames) and the --frame-stats lines in the range.

With --group (ABAB): the range totals per run, per group mean and sample
standard deviation, and each group's difference to the first group. A
difference counts as "beyond spread" only if it exceeds the sum of both
standard deviations; that is a coarse rule, not a significance test, and with
two runs per group says little.

--simpleperf: the top entries of a `simpleperf report` (any --sort), summed per
thread name (comm) and shared object when those columns are present.

Only numbers and names from the program's own output; the logs themselves stay
in LGP_PRIVATE_DIR.
"""
import argparse
import math
import re
import sys

TIME = re.compile(r"time: frames (\d+)-(\d+): ([\d.]+) s wall, ([\d.]+) fps")
THREAD = re.compile(r"time:\s+thread (\d+) \(prio (-?\d+), core (-?\d+)(, ended)?\):(.*?) ms/frame(.*)")
SOURCE = re.compile(r"time:\s+([^:]+): busy ([\d.]+) ms/frame \((\d+) %\), ([\d.]+) calls \(([\d.]+) us each\)")
GPU_MEM = re.compile(r"gpu memory, frames (\d+)-(\d+):.*?presents waited (\d+) \(([\d.]+) ms/frame\), "
                     r"compute write-backs (\d+) \(waited for (\d+), ([\d.]+) ms/frame\)")
GPU_WORK = re.compile(r"gpu work, frames (\d+)-(\d+): programs created (\d+) \(([\d.]+) ms\), "
                      r"draw pipelines created (\d+) \(([\d.]+) ms;.*?draws skipped while compiling (\d+)\), "
                      r"compute pipelines waited for (\d+) \(([\d.]+) ms\)")
GPU_LISTS = re.compile(r"gpu lists, frames (\d+)-(\d+): lists (\d+),.*?compute first: dispatches (\d+) ahead, "
                       r"(\d+) in order, lists (\d+), read back ahead (\d+)")
GPU_TIME = re.compile(r"gpu time, frames (\d+)-(\d+): compute ([\d.]+) ms/frame \((\d+) passes\), "
                      r"render ([\d.]+) ms/frame \((\d+) passes\)")
GPU_PASSES = re.compile(r"gpu passes, frames (\d+)-(\d+): render passes (\d+) .*?draw passes (\d+) "
                        r"\([\d.]+ draws each\)(?:, new attachment sets (\d+))?")
HOST_MEM = re.compile(r"host memory, frame (\d+): footprint ([\d.]+) MiB, peak ([\d.]+) MiB, available ([\d.]+) MiB")
AUDOUT = re.compile(r"audout: .*?underruns (\d+)")
FRAME = re.compile(r"\bframe (\d+): (\d+x\d+, non-black .*|not readable)")


class Window:
    def __init__(self, first, last, wall_s):
        self.first, self.last, self.wall_s = first, last, wall_s
        self.values = {}  # metric -> value per frame (or count for counters)
        self.threads = {}  # id -> {cat: ms/frame}
        self.sources = {}  # name -> busy ms/frame

    @property
    def frames(self):
        return self.last - self.first + 1

    @property
    def fps(self):
        return self.frames / self.wall_s if self.wall_s > 0 else 0.0


# Per-frame metrics (weighted by frames) and counters (summed) of a window.
PER_FRAME = ("present wait", "write-back wait", "gpu compute", "gpu render")
COUNTERS = ("presents waited", "write-backs", "write-backs waited", "draw pipelines", "draw pipelines ms",
            "programs", "programs ms", "skipped draws", "compute waits", "compute waits ms", "lists",
            "dispatches ahead", "dispatches in order", "read back ahead", "render passes", "draw passes",
            "new attachment sets", "audio underruns")


def parse_log(lines):
    windows = {}  # last frame -> Window
    frames = {}   # frame -> text
    pending_underruns = 0
    current = None
    for raw in lines:
        line = raw.rstrip("\n")
        m = TIME.search(line)
        if m:
            first, last = int(m.group(1)), int(m.group(2))
            current = windows.setdefault(last, Window(first, last, float(m.group(3))))
            current.wall_s = float(m.group(3))
            # audout reports have no frame number: they count for the next window.
            current.values["audio underruns"] = current.values.get("audio underruns", 0) + pending_underruns
            pending_underruns = 0
            continue
        m = THREAD.search(line)
        if m and current is not None:
            cats = {}
            for item in m.group(5).split(","):
                parts = item.split()
                if len(parts) == 2:
                    cats[parts[0]] = float(parts[1])
            current.threads[int(m.group(1))] = cats
            continue
        m = SOURCE.search(line)
        if m and current is not None:
            current.sources[m.group(1).strip()] = float(m.group(2))
            continue
        m = AUDOUT.search(line)
        if m:
            pending_underruns += int(m.group(1))
            continue
        for rx, fill in ((GPU_MEM, _gpu_mem), (GPU_WORK, _gpu_work), (GPU_LISTS, _gpu_lists), (GPU_TIME, _gpu_time),
                         (GPU_PASSES, _gpu_passes)):
            m = rx.search(line)
            if m:
                first, last = int(m.group(1)), int(m.group(2))
                w = windows.setdefault(last, Window(first, last, 0.0))
                fill(w.values, m)
                break
        else:
            m = HOST_MEM.search(line)
            if m:
                w = windows.get(int(m.group(1)))
                if w is not None:
                    w.values["footprint MiB"] = float(m.group(2))
                    w.values["available MiB"] = float(m.group(4))
                continue
            m = FRAME.search(line)
            if m:
                frames[int(m.group(1))] = m.group(2)
    return windows, frames


def _gpu_mem(v, m):
    v["presents waited"] = int(m.group(3))
    v["present wait"] = float(m.group(4))
    v["write-backs"] = int(m.group(5))
    v["write-backs waited"] = int(m.group(6))
    v["write-back wait"] = float(m.group(7))


def _gpu_work(v, m):
    v["programs"] = int(m.group(3))
    v["programs ms"] = float(m.group(4))
    v["draw pipelines"] = int(m.group(5))
    v["draw pipelines ms"] = float(m.group(6))
    v["skipped draws"] = int(m.group(7))
    v["compute waits"] = int(m.group(8))
    v["compute waits ms"] = float(m.group(9))


def _gpu_lists(v, m):
    v["lists"] = int(m.group(3))
    v["dispatches ahead"] = int(m.group(4))
    v["dispatches in order"] = int(m.group(5))
    v["read back ahead"] = int(m.group(7))


def _gpu_passes(v, m):
    v["render passes"] = int(m.group(3))
    v["draw passes"] = int(m.group(4))
    if m.group(5) is not None:
        v["new attachment sets"] = int(m.group(5))


def _gpu_time(v, m):
    v["gpu compute"] = float(m.group(3))
    v["gpu render"] = float(m.group(5))


def in_range(windows, first, last):
    """Windows with a wall time that lie inside [first, last]."""
    return [windows[k] for k in sorted(windows)
            if windows[k].wall_s > 0 and windows[k].first >= first and windows[k].last <= last]


def totals(ws):
    """Range totals: fps from frames / wall, per-frame values weighted by frames, counters summed."""
    frames = sum(w.frames for w in ws)
    wall = sum(w.wall_s for w in ws)
    out = {"frames": frames, "wall s": wall, "fps": frames / wall if wall > 0 else 0.0,
           "ms/frame": 1000.0 * wall / frames if frames else 0.0,
           "slowest window fps": min((w.fps for w in ws), default=0.0)}
    for key in PER_FRAME:
        have = [w for w in ws if key in w.values]
        if have:
            out[key] = sum(w.values[key] * w.frames for w in have) / sum(w.frames for w in have)
    for key in COUNTERS:
        have = [w for w in ws if key in w.values]
        if have:
            out[key] = sum(w.values[key] for w in have)
    # Threads and sources: ms/frame weighted by frames; a thread missing in a
    # window (it only waited) counts as 0 there.
    for name, get in (("thread", lambda w: {f"thread {t} {c}": v for t, cats in w.threads.items()
                                            for c, v in cats.items() if c != "wait"}),
                      ("source", lambda w: {f"{s} busy": v for s, v in w.sources.items()})):
        acc = {}
        for w in ws:
            for k, v in get(w).items():
                acc[k] = acc.get(k, 0.0) + v * w.frames
        for k, v in acc.items():
            out[k] = v / frames if frames else 0.0
    return out


def fmt(v):
    if isinstance(v, int):
        return str(v)
    return f"{v:.1f}" if abs(v) >= 100 else f"{v:.2f}"


def report_run(path, windows, frames, first, last, top_threads, out):
    ws = in_range(windows, first, last)
    print(f"== {path}: frames {first}-{last}", file=out)
    if not ws:
        print("  no --time-stats window inside the range (run with --time-stats N; windows must not straddle "
              "the range limits)", file=out)
        return None
    print(f"  {'frames':<11} {'fps':>5} {'ms/f':>6} {'wb wait':>7} {'pres':>6} {'gpu cs':>6} {'gpu rd':>6} "
          f"{'pipes':>5} {'pipe ms':>7} {'rb ahd':>6} {'underr':>6}", file=out)
    for w in ws:
        v = w.values
        g = lambda k, f="{:.1f}": f.format(v[k]) if k in v else "-"  # noqa: E731
        print(f"  {w.first:>5}-{w.last:<5} {w.fps:>5.1f} {1000 * w.wall_s / w.frames:>6.1f} {g('write-back wait'):>7} "
              f"{g('present wait'):>6} {g('gpu compute'):>6} {g('gpu render'):>6} {g('draw pipelines', '{}'):>5} "
              f"{g('draw pipelines ms', '{:.0f}'):>7} {g('read back ahead', '{}'):>6} "
              f"{g('audio underruns', '{}'):>6}", file=out)
        busy = sorted(((sum(v for c, v in cats.items() if c != "wait"), t, cats) for t, cats in w.threads.items()),
                      reverse=True)[:top_threads]
        for _, t, cats in busy:
            print(f"      thread {t}: " + ", ".join(f"{c} {x:.1f}" for c, x in cats.items() if x >= 0.05), file=out)
        if w.sources:
            print("      " + ", ".join(f"{s} busy {x:.1f}" for s, x in w.sources.items()), file=out)
    t = totals(ws)
    print(f"  total: {t['frames']} frames in {t['wall s']:.1f} s, {t['fps']:.2f} fps ({t['ms/frame']:.1f} ms/frame), "
          f"slowest window {t['slowest window fps']:.1f} fps", file=out)
    extra = [f"{k} {fmt(t[k])}" for k in PER_FRAME + COUNTERS if k in t]
    if extra:
        print("  " + ", ".join(extra), file=out)
    shots = [(n, s) for n, s in sorted(frames.items()) if first <= n <= last]
    for n, s in shots:
        print(f"  frame {n}: {s}", file=out)
    # Frames the windows cover, and whether without gaps (group comparisons
    # use only runs that cover the same frames).
    t["covered"] = (ws[0].first, ws[-1].last, t["frames"] == ws[-1].last - ws[0].first + 1)
    if not (ws[0].first == first and ws[-1].last == last and t["frames"] == last - first + 1):
        print(f"  note: windows cover frames {ws[0].first}-{ws[-1].last} ({t['frames']} frames), not exactly the range",
              file=out)
    return t


DEFAULT_LAST = 1 << 62


def mean_sd(xs):
    m = sum(xs) / len(xs)
    sd = math.sqrt(sum((x - m) ** 2 for x in xs) / (len(xs) - 1)) if len(xs) > 1 else 0.0
    return m, sd


# Metrics where more is better; everything else counts as cost.
HIGHER_IS_BETTER = ("fps", "slowest window fps")


def compare_groups(groups, first, last, out):
    """groups: [(name, [(path, totals)])]. Prints per-run values, mean ± sd, difference to the first group."""
    keys = []
    for _, runs in groups:
        for _, t in runs:
            for k in t:
                if k not in keys and k not in ("frames", "wall s", "covered"):
                    keys.append(k)
    # Metrics present in every run; drop tiny thread/source values.
    keys = [k for k in keys if all(k in t for _, runs in groups for _, t in runs)]
    keys = [k for k in keys if not (k.startswith("thread ") or k.endswith(" busy"))
            or max(t[k] for _, runs in groups for _, t in runs) >= 0.5]
    print(f"== comparison, frames {first}-{last}", file=out)
    for name, runs in groups:
        print(f"  {name}: " + ", ".join(f"{p} ({t['frames']} frames)" for p, t in runs), file=out)
    head = f"  {'metric':<28}" + "".join(f" {name:>30}" for name, _ in groups)
    print(head, file=out)
    base = None
    verdicts = []
    for k in keys:
        cells = []
        stats = []
        for name, runs in groups:
            xs = [t[k] for _, t in runs]
            m, sd = mean_sd(xs)
            stats.append((name, m, sd))
            cells.append(f"{fmt(m)} ± {fmt(sd)}" + (f" [{', '.join(fmt(x) for x in xs)}]" if len(xs) <= 3 else ""))
        print(f"  {k:<28}" + "".join(f" {c:>30}" for c in cells), file=out)
        base = stats[0]
        for name, m, sd in stats[1:]:
            if base[1] == 0 and m == 0:
                continue
            diff = m - base[1]
            pct = 100.0 * diff / base[1] if base[1] else float("inf")
            beyond = abs(diff) > base[2] + sd
            if k in ("fps", "ms/frame", "slowest window fps", "write-back wait", "present wait", "audio underruns"):
                better = (diff > 0) == (k in HIGHER_IS_BETTER)
                verdicts.append(f"  {name} vs {base[0]}: {k} {fmt(base[1])} -> {fmt(m)} ({pct:+.1f} %), "
                                + ("beyond spread" if beyond else "within spread")
                                + (", better" if better and diff else ", worse" if diff else ""))
    if verdicts:
        print("", file=out)
        print("  differences (beyond spread = |diff| > sd A + sd B; coarse rule, not a significance test):",
              file=out)
        for v in verdicts:
            print(v, file=out)
    if any(len(runs) < 2 for _, runs in groups):
        print("  note: a group with one run has no spread; run ABAB (at least two runs per group)", file=out)


def parse_simpleperf(lines):
    """Rows of `simpleperf report` text: [(percents, {column: value})], plus header facts."""
    facts, columns, rows = {}, None, []
    for raw in lines:
        line = raw.rstrip("\n")
        if columns is None:
            m = re.match(r"(Samples|Event count|Event|Cmdline|Arch): (.*)", line)
            if m:
                facts[m.group(1)] = m.group(2)
                continue
            if re.match(r"\s*(Overhead|Children)\s", line):
                columns = re.split(r"\s{2,}", line.strip())
            continue
        m = re.match(r"\s*((?:[\d.]+%\s+)+)(.*)", line)
        if not m:
            continue
        pcts = [float(p) for p in re.findall(r"([\d.]+)%", m.group(1))]
        npct = len(pcts)
        rest = re.split(r"\s{2,}", m.group(2).strip(), maxsplit=max(len(columns) - npct - 1, 0))
        rows.append((pcts, dict(zip(columns[npct:], rest))))
    return facts, columns or [], rows


def report_simpleperf(path, top, out):
    with open(path, encoding="utf-8", errors="replace") as f:
        facts, columns, rows = parse_simpleperf(f)
    print(f"== simpleperf {path}: " + ", ".join(f"{k} {v}" for k, v in facts.items() if k in ("Event", "Samples")),
          file=out)
    if not rows:
        print("  no rows (expected `simpleperf report` text with an Overhead column)", file=out)
        return
    # Overhead: the self share ("Self" with --children, else the only percent).
    self_idx = columns.index("Self") if "Self" in columns else 0
    names = [c for c in columns if c not in ("Overhead", "Children", "Self")]
    print("  " + " | ".join(["%"] + names), file=out)
    for pcts, cols in sorted(rows, key=lambda r: -r[0][min(self_idx, len(r[0]) - 1)])[:top]:
        print(f"  {pcts[min(self_idx, len(pcts) - 1)]:6.2f} " + " | ".join(cols.get(c, "") for c in names), file=out)
    for col in ("Command", "Shared Object"):
        if col in names:
            acc = {}
            for pcts, cols in rows:
                acc[cols.get(col, "?")] = acc.get(cols.get(col, "?"), 0.0) + pcts[min(self_idx, len(pcts) - 1)]
            print(f"  per {col.lower()}: " + ", ".join(f"{k} {v:.1f} %" for k, v in
                                                       sorted(acc.items(), key=lambda kv: -kv[1])[:top]), file=out)


def load(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        return parse_log(f)


def main(argv=None, out=sys.stdout):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("logs", nargs="*", help="lgp_game output with --time-stats N")
    ap.add_argument("--from", dest="first", type=int, default=1, help="first frame of the range")
    ap.add_argument("--to", dest="last", type=int, default=DEFAULT_LAST, help="last frame of the range")
    ap.add_argument("--group", action="append", default=[], metavar="NAME=LOG[,LOG...]",
                    help="runs of one variant; repeat per variant (the first one is the baseline)")
    ap.add_argument("--simpleperf", action="append", default=[], help="`simpleperf report` text output")
    ap.add_argument("--top", type=int, default=20, help="simpleperf rows and per-window threads shown")
    ap.add_argument("--threads", type=int, default=3, help="busiest threads shown per window")
    a = ap.parse_args(argv)
    if not a.logs and not a.group and not a.simpleperf:
        ap.error("give logs, --group or --simpleperf")
    results = {}
    for path in a.logs:
        windows, frames = load(path)
        results[path] = report_run(path, windows, frames, a.first, a.last, a.threads, out)
        print("", file=out)
    groups = []
    # Without --to the range is "to the end": then every run must cover the
    # frames the first grouped run covers.
    explicit = a.last != DEFAULT_LAST
    reference = None
    for spec in a.group:
        name, _, paths = spec.partition("=")
        runs = []
        for path in filter(None, paths.split(",")):
            if path not in results:
                windows, frames = load(path)
                results[path] = report_run(path, windows, frames, a.first, a.last, a.threads, out)
                print("", file=out)
            t = results[path]
            if t is None:
                continue
            first_f, last_f, contiguous = t["covered"]
            if reference is None and not explicit:
                reference = t["covered"]
            if not contiguous or (explicit and (first_f, last_f) != (a.first, a.last)) or \
                    (not explicit and t["covered"] != reference):
                print(f"group {name}: {path} left out of the comparison (covers frames {first_f}-{last_f}"
                      f"{'' if contiguous else ' with gaps'}, not the range of the other runs)", file=out)
                continue
            runs.append((path, t))
        if not runs:
            print(f"group {name}: no usable run", file=out)
            return 2
        groups.append((name, runs))
    if len(groups) >= 2:
        compare_groups(groups, a.first, a.last, out)
        print("", file=out)
    for path in a.simpleperf:
        report_simpleperf(path, a.top, out)
        print("", file=out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
