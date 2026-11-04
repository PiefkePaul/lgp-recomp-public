#!/usr/bin/env python3
"""Why did a run on a device end? Report from the program's console output.

For runs that end without a crash report (Apple TV: signal 9 near frame 8600,
docs/analysis/28-tvos-machbarkeit.md). Reads the output of `lgp_game ...
--time-stats N` (e.g. `xcrun devicectl device process launch --console ...`)
and, if given, what the system logged around the end:

  kill_run_report.py console.txt [--system kernel.txt] [--jetsam JetsamEvent.ips]
                     [--limit-mib 2098] [--spike-mib 32] [--tail 15]

  --system   text export of the device's log, e.g. `log collect --device-udid
             <udid> --last 30m --output x.logarchive` then `log show --archive
             x.logarchive --style compact > kernel.txt`; only lines naming the
             process, jetsam/memorystatus or a termination are used.
  --jetsam   a JetsamEvent-*.ips report (header line + JSON body).

Prints the memory curve per --time-stats window (footprint, peak, available,
guest resident, GPU buffers/textures, fps), with LGP_HOST_MEMORY_PARTS=1 the
footprint parts (ledgers, VM tags; the total footprint left out) that rose most between
two samples, the last values before the end,
whether the peak rose between two samples (a short spike the samples do not
show), the system's kill reason if one was found, and the last lines of output.
Only numbers and log lines of the program and the system; no game data.
"""
import argparse
import json
import re
import sys

HOST = re.compile(
    r"host memory, frame (\d+): footprint ([\d.]+) MiB, peak ([\d.]+) MiB, available ([\d.]+) MiB"
    r"(?:, guest resident ([\d.]+) MiB)?"
    r"(?:, metal allocated ([\d.]+) MiB \(working set ([\d.]+) MiB\))?"
    r"(?:, purgeable ([\d.]+) MiB, volatile ([\d.]+) MiB)?"
    r"(?:, system free ([\d.]+) MiB, file cache ([\d.]+) MiB, compressed ([\d.]+) MiB, level (-?\d+) %)?")
PARTS = re.compile(r"host memory parts, frame (\d+) \(MiB\): (.*)$")
GPU = re.compile(r"gpu memory, frames \d+-(\d+): buffers live \d+ \(([\d.]+) MiB.*?textures live \d+ \(~([\d.]+) MiB")
TIME = re.compile(r"time: frames \d+-(\d+): [\d.]+ s wall, ([\d.]+) fps")
FRAME = re.compile(r"\bframes? (?:\d+-)?(\d+)\b")
STATS = ("host memory,", "host memory parts,", "gpu memory,", "gpu work,", "gpu lists,", "gpu load,", "gpu passes,",
         "gpu time", "time:", "time stats:")

# Reasons memorystatus gives when it kills (xnu kern_memorystatus.c names).
REASONS = ("per-process-limit", "vm-pageshortage", "vm-compressor-space-shortage", "vm-compressor-thrashing",
           "fc-thrashing", "zone-map-exhaustion", "proc-thrashing", "idle-exit", "lowswap", "disk-space-shortage",
           "highwater", "sustained-memory-pressure")
SYSTEM_KEYS = re.compile(r"memorystatus|jetsam|killing|terminat|SIGKILL|signal 9|watchdog|EXC_RESOURCE|diskwrites"
                         r"|exceeded|runningboard|Termination", re.I)


def parse_parts(text):
    """'name v, name v, ...' (LGP_HOST_MEMORY_PARTS=1) -> {name: MiB}."""
    parts = {}
    for item in text.split(", "):
        name, _, value = item.rpartition(" ")
        try:
            parts[name] = float(value)
        except ValueError:
            pass
    return parts


def parse_console(lines, parts=None):
    rows = {}  # frame -> dict
    last_frame = 0
    tail = []
    for raw in lines:
        line = raw.rstrip("\n")
        m = PARTS.search(line)
        if m:
            if parts is not None:
                parts[int(m.group(1))] = parse_parts(m.group(2))
            last_frame = max(last_frame, int(m.group(1)))
            continue
        m = HOST.search(line)
        if m:
            f = int(m.group(1))
            r = rows.setdefault(f, {})
            r.update(footprint=float(m.group(2)), peak=float(m.group(3)), available=float(m.group(4)))
            if m.group(5) is not None:
                r["guest"] = float(m.group(5))
            if m.group(6) is not None:
                r.update(metal=float(m.group(6)), workingset=float(m.group(7)))
            if m.group(8) is not None:
                r.update(purgeable=float(m.group(8)), volatile=float(m.group(9)))
            if m.group(10) is not None:
                r.update(sysfree=float(m.group(10)), filecache=float(m.group(11)), compressed=float(m.group(12)),
                         level=int(m.group(13)))
            last_frame = max(last_frame, f)
            continue
        m = GPU.search(line)
        if m:
            f = int(m.group(1))
            rows.setdefault(f, {}).update(buffers=float(m.group(2)), textures=float(m.group(3)))
            last_frame = max(last_frame, f)
            continue
        m = TIME.search(line)
        if m:
            f = int(m.group(1))
            rows.setdefault(f, {})["fps"] = float(m.group(2))
            last_frame = max(last_frame, f)
            continue
        if any(s in line for s in STATS):
            continue
        m = FRAME.search(line)
        if m and line.startswith("frame"):
            last_frame = max(last_frame, int(m.group(1)))
        if line.strip():
            tail.append(line)
    return rows, last_frame, tail


def fmt(v, width=7):
    return f"{v:{width}.0f}" if v is not None else " " * (width - 1) + "-"


def report_memory(rows, limit, out, spike_mib=32.0):
    frames = sorted(f for f in rows if "footprint" in rows[f])
    if not frames:
        print("no 'host memory' lines (run with --time-stats N)", file=out)
        return
    print(f"{'frame':>7} {'footpr':>7} {'Δ':>6} {'peak':>7} {'avail':>7} {'guest':>7} {'gpubuf':>7} {'tex':>7} {'fps':>5}",
          file=out)
    prev = None
    spikes = []
    for f in frames:
        r = rows[f]
        d = r["footprint"] - prev["footprint"] if prev else None
        if prev and r["peak"] > max(prev["peak"], r["footprint"], prev["footprint"]) + spike_mib:
            # The peak rose above both samples: something came and went in between.
            spikes.append((prev_f, f, r["peak"] - max(r["footprint"], prev["footprint"])))
        fps = r.get("fps")
        print(f"{f:>7} {fmt(r['footprint'])} {fmt(d, 6)} {fmt(r['peak'])} {fmt(r['available'])} {fmt(r.get('guest'))} "
              f"{fmt(r.get('buffers'))} {fmt(r.get('textures'))} {fps if fps is not None else '-':>5}", file=out)
        prev, prev_f = r, f
    last = rows[frames[-1]]
    print("", file=out)
    print(f"last sample: frame {frames[-1]}, footprint {last['footprint']:.0f} MiB, available {last['available']:.0f} MiB"
          + (f", footprint + available {last['footprint'] + last['available']:.0f} MiB" if limit is None else
             f", {limit - last['footprint']:.0f} MiB below the limit of {limit:.0f} MiB"), file=out)
    print(f"highest footprint {max(rows[f]['footprint'] for f in frames):.0f} MiB, peak {max(rows[f]['peak'] for f in frames):.0f}"
          f" MiB, least available {min(rows[f]['available'] for f in frames):.0f} MiB", file=out)
    sysrows = [f for f in frames if "sysfree" in rows[f]]
    if sysrows:
        # A process killed below its own limit while the machine ran out
        # points at system-wide shortage (memorystatus vm-pageshortage & Co).
        low = min(sysrows, key=lambda f: rows[f]["sysfree"])
        lastsys = rows[sysrows[-1]]
        print(f"system: free {lastsys['sysfree']:.0f} MiB, file cache {lastsys['filecache']:.0f} MiB, compressed "
              f"{lastsys['compressed']:.0f} MiB, level {lastsys['level']} % at frame {sysrows[-1]}; least free "
              f"{rows[low]['sysfree']:.0f} MiB at frame {low}", file=out)
    metal = [f for f in frames if "metal" in rows[f]]
    if metal:
        top = max(metal, key=lambda f: rows[f]["metal"])
        print(f"metal allocated: {rows[metal[0]]['metal']:.0f} -> {rows[metal[-1]]['metal']:.0f} MiB (max "
              f"{rows[top]['metal']:.0f} at frame {top}; working set {rows[top]['workingset']:.0f} MiB)", file=out)
    purge = [f for f in frames if "purgeable" in rows[f]]
    if purge:
        top = max(purge, key=lambda f: rows[f]["volatile"])
        print(f"purgeable: nonvolatile {rows[purge[-1]]['purgeable']:.0f} MiB, volatile {rows[purge[-1]]['volatile']:.0f}"
              f" MiB at frame {purge[-1]}; most volatile {rows[top]['volatile']:.0f} MiB at frame {top}", file=out)
    guests = [rows[f]["guest"] for f in frames if "guest" in rows[f]]
    if guests:
        print(f"guest resident: {guests[0]:.0f} -> {guests[-1]:.0f} MiB (max {max(guests):.0f})", file=out)
    if spikes:
        print("peak rose between samples (short spikes the samples miss):", file=out)
        for a, b, extra in spikes:
            print(f"  frames {a}-{b}: {extra:.0f} MiB above both samples", file=out)
    else:
        print(f"peak never rose more than {spike_mib:.0f} MiB above the samples (no hidden spike)", file=out)


def report_parts(parts, out, count=8):
    """Which parts of the footprint rose most between two samples."""
    frames = sorted(parts)
    if len(frames) < 2:
        return
    rises = []
    for name in sorted({n for f in frames for n in parts[f]} - {"footprint"}):
        series = [(f, parts[f][name]) for f in frames if name in parts[f]]
        if len(series) < 2:
            continue
        best = max(zip(series, series[1:]), key=lambda ab: ab[1][1] - ab[0][1])
        (fa, va), (fb, vb) = best
        top = max(series, key=lambda fv: fv[1])
        rises.append((vb - va, name, fa, fb, series[0][1], series[-1][1], top))
    rises.sort(reverse=True)
    print("", file=out)
    print("footprint parts with the largest rise between two samples (MiB):", file=out)
    for rise, name, fa, fb, first, last, top in rises[:count]:
        print(f"  {name}: {rise:+.0f} at frames {fa}-{fb}; {first:.0f} -> {last:.0f}, max {top[1]:.0f} at frame "
              f"{top[0]}", file=out)


def report_system(lines, process, out):
    hits = [l.rstrip("\n") for l in lines if SYSTEM_KEYS.search(l) and (process in l or "memorystatus" in l
                                                                           or "jetsam" in l.lower())]
    reasons = {}
    for l in hits:
        for r in REASONS:
            if r in l and process in l:
                reasons[r] = reasons.get(r, 0) + 1
    print(f"system log: {len(hits)} relevant lines", file=out)
    if reasons:
        print("kill reasons naming " + process + ": " + ", ".join(f"{r} ({n})" for r, n in reasons.items()), file=out)
    elif hits:
        print(f"no memorystatus kill reason names {process}", file=out)
    for l in hits[-25:]:
        print("  " + l, file=out)


def report_jetsam(text, process, out):
    # Header JSON line, then the body (one JSON object).
    parts = text.split("\n", 1)
    body = json.loads(parts[1] if len(parts) > 1 and parts[1].strip() else parts[0])
    page = body.get("memoryStatus", {}).get("pageSize", 16384)
    print(f"jetsam report: largest process {body.get('largestProcess', '?')}", file=out)
    for p in body.get("processes", []):
        if p.get("name") == process or "reason" in p:
            mib = p.get("rpages", 0) * page / (1024 * 1024)
            print(f"  {p.get('name', '?')}: reason {p.get('reason', '-')}, {mib:.0f} MiB resident"
                  + (f", lifetimeMax {p['lifetimeMax'] * page / (1024 * 1024):.0f} MiB" if "lifetimeMax" in p else ""),
                  file=out)


def main(argv=None, out=sys.stdout):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("console")
    ap.add_argument("--system")
    ap.add_argument("--jetsam")
    ap.add_argument("--process", default="lgp_game")
    ap.add_argument("--limit-mib", type=float, help="jetsam limit (Apple TV 4K: 2098)")
    ap.add_argument("--spike-mib", type=float, default=32.0, help="peak rise between samples that counts as a spike")
    ap.add_argument("--tail", type=int, default=15)
    a = ap.parse_args(argv)
    with open(a.console, encoding="utf-8", errors="replace") as f:
        parts = {}
        rows, last_frame, tail = parse_console(f, parts)
    print(f"last frame in the output: {last_frame}", file=out)
    report_memory(rows, a.limit_mib, out, a.spike_mib)
    report_parts(parts, out)
    if a.system:
        print("", file=out)
        with open(a.system, encoding="utf-8", errors="replace") as f:
            report_system(f, a.process, out)
    if a.jetsam:
        print("", file=out)
        with open(a.jetsam, encoding="utf-8", errors="replace") as f:
            report_jetsam(f.read(), a.process, out)
    print("", file=out)
    print(f"last {min(a.tail, len(tail))} lines besides the statistics:", file=out)
    for l in tail[-a.tail:]:
        print("  " + l, file=out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
