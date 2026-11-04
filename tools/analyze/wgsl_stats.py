#!/usr/bin/env python3
"""Shape of translated WGSL programs, joined with their compile times.

Which compute shaders make a cold start slow, and why? The WebGPU backend
dumps every program's WGSL with LGP_WEBGPU_DUMP_DIR (compute programs as
prog<id>-<hash>.cs.wgsl) and logs slow programs and compute pipelines with
their hash:

  nvn webgpu: compute program 69 (0123456789abcdef, WGSL 812345 bytes) created in 56800.0 ms ...
  nvn webgpu: compute pipeline 0123456789abcdef (WGSL 812345 bytes) ready after 56800.0 ms (async)

This script counts what tends to make a shader compiler slow in each file:
size, lines, blocks of the dispatcher loop (`loop { switch pc { ... } }`, the
translator's form for branches), the lockstep loop (barriers in divergent
code), loops, global memory calls (each a switch over all slots), how
many accesses search the slot table and how many go straight to their slot
(storage descriptor resolved by the translator), the
Maxwell registers declared as variables and the words of per-thread arrays
(local memory, the SSY/PBK stacks). With --log it adds the slowest compile
time seen for each hash, so runs on several devices can be put side by side
(one --log per device). Runs with LGP_WEBGPU_DEBUG=gputime and --time-stats
also log the GPU execution time of the busiest compute programs:

  gpu time, frames 2701-3000: compute 12.30 ms/frame (...); compute programs ms/frame: 0123456789abcdef 9.80, ...

For those the table gets a column with the highest ms/frame of any window.

Draw pipelines are logged the same way, once per slow variant (a graphics
program gets one pipeline per render state it is drawn with):

  nvn webgpu: draw pipeline 0123456789abcdef (WGSL 23456 bytes) ready after 114.0 ms (on use)

With --all such logs add a column per device with the program's draw
pipeline time summed over its variants and their count, e.g. "1368.0 (12)".

The dumped WGSL is translated game code: keep the dump directory and any
output below $LGP_PRIVATE_DIR. Only the numbers may be shared.

With --profile FILE... it instead counts constructs inside single modules:
calls per function name (translator helpers, builtins, conversions),
constant buffer reads with a constant or a run-time index, bitcasts,
statements, if/loop/switch and private variables. Names and counts only,
no code, so the output can be shared.

  python3 tools/analyze/wgsl_stats.py <dump-dir> [--log NAME=FILE ...] [--all] [--top N]
  python3 tools/analyze/wgsl_stats.py --profile <file.wgsl ...> [--top N]
"""
import argparse
import os
import re
import sys

COMPUTE_NAME = re.compile(r"^prog(\d+)-([0-9a-f]{16})\.cs\.wgsl$")
GRAPHICS_NAME = re.compile(r"^prog(\d+)(?:-([0-9a-f]{16}))?\.(vs|fs)\.wgsl$")  # older dumps: no hash
LOG_PIPELINE = re.compile(r"compute pipeline ([0-9a-f]{16}) \(WGSL (\d+) bytes\) ready after ([0-9.]+) ms")
LOG_DRAW = re.compile(r"draw pipeline ([0-9a-f]{16}) \(WGSL (\d+) bytes\) ready after ([0-9.]+) ms")
LOG_PROGRAM = re.compile(r"(compute|graphics) program (\d+) \(([0-9a-f]{16}), WGSL (\d+) bytes\) created in ([0-9.]+) ms")
LOG_GPU = re.compile(r"gpu time, frames \d+-\d+: .*compute programs ms/frame: (.*)$")
GPU_ITEM = re.compile(r"([0-9a-f]{16}) ([0-9.]+)")
BLOCK_CASE = re.compile(r"^\s*case \d+u: \{$", re.M)  # dispatcher blocks; helper switches are one-liners
GMEM_CALL = re.compile(r"\bgmem_(?:ld|st|st_sub|addr)\(")
GMEM_DEF = re.compile(r"^fn gmem_(?:ld|st|st_sub|addr)\(", re.M)
# Global memory accesses: through the slot table search, or straight to their slot
# (statically resolved descriptor, wgsl::Options::storage_desc_offset).
GMEM_SEARCH = re.compile(r"(?<!fn )\bgmem_addr\(")
GMEM_DIRECT = re.compile(r"(?<!fn )\bgmem_addr_at\(")
# Why an access searches (maxwell2wgsl comment before it), shortened for the table.
GMEM_WHY = re.compile(r"// gmem search: (.+)$", re.M)
WHY_SHORT = {"not resolved": "unresolved", "32-bit address": "32bit", "address from data": "data",
             "address from several sources": "mixed", "low and high word disagree": "lohi",
             "more uniform blocks than windows": "windows"}
REG_VAR = re.compile(r"^var<private> r\d+\b", re.M)
PRIVATE_ARRAY = re.compile(r"^\s*var(?:<private>)? \w+: array<\w+, (\d+)>", re.M)  # per-thread arrays (local memory, stacks)


def why(text):
    """'data 3, mixed 1': the reasons of the searching accesses, most frequent first."""
    counts = {}
    for reason in GMEM_WHY.findall(text):
        r = WHY_SHORT.get(reason.strip(), reason.strip())
        counts[r] = counts.get(r, 0) + 1
    return ", ".join("%s %d" % (r, n) for r, n in sorted(counts.items(), key=lambda x: (-x[1], x[0]))) or "-"


def shape(text):
    """Counts for one WGSL module."""
    return {
        "bytes": len(text.encode()),
        "lines": text.count("\n"),
        "blocks": len(BLOCK_CASE.findall(text)),
        "lockstep": "ls_state" in text,
        "loops": len(re.findall(r"\bloop \{", text)) + len(re.findall(r"\bfor \(", text)),
        "gmem_calls": len(GMEM_CALL.findall(text)) - len(GMEM_DEF.findall(text)),
        "gmem_search": len(GMEM_SEARCH.findall(text)),
        "gmem_direct": len(GMEM_DIRECT.findall(text)),
        "search_why": why(text),
        "registers": len(REG_VAR.findall(text)),
        "private_words": sum(int(n) for n in PRIVATE_ARRAY.findall(text)),
    }


def read_dump(directory, include_graphics):
    """[(label, hash or None, shape)] for the dumped programs."""
    rows = []
    for name in sorted(os.listdir(directory)):
        m = COMPUTE_NAME.match(name)
        g = GRAPHICS_NAME.match(name)
        if not m and not (include_graphics and g):
            continue
        path = os.path.join(directory, name)
        if os.path.getsize(path) == 0:
            continue  # a program without that stage (older dumps wrote empty files)
        with open(path, encoding="utf-8", errors="replace") as f:
            s = shape(f.read())
        if m:
            rows.append(("compute " + m.group(1), m.group(2), s))
        else:
            rows.append((("vertex " if g.group(3) == "vs" else "fragment ") + g.group(1), g.group(2), s))
    return rows


def read_log(path):
    """({hash: slowest compile ms}, {hash: highest GPU ms/frame}, {hash: [draw pipeline ms summed, count]})
    from a run's log."""
    times, gpu, draw = {}, {}, {}
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = LOG_DRAW.search(line)
            if m:
                d = draw.setdefault(m.group(1), [0.0, 0])
                d[0] += float(m.group(3))
                d[1] += 1
                continue
            m = LOG_GPU.search(line)
            if m:
                for h, ms in GPU_ITEM.findall(m.group(1)):
                    gpu[h] = max(gpu.get(h, 0.0), float(ms))
                continue
            m = LOG_PIPELINE.search(line)
            if m:
                h, ms = m.group(1), float(m.group(3))
            else:
                m = LOG_PROGRAM.search(line)
                if not m:
                    continue
                h, ms = m.group(3), float(m.group(5))
            times[h] = max(times.get(h, 0.0), ms)
    return times, gpu, draw


def table(rows, logs, top):
    """logs: [(name, compile times, GPU times, draw pipeline times)]."""
    cols = ["program", "hash", "bytes", "lines", "blocks", "lockstep", "loops", "gmem calls", "gmem search",
            "gmem direct", "search why", "registers", "private words"]
    cols += [name + " ms" for name, _, _, _ in logs]
    gpu_logs = [(name, g) for name, _, g, _ in logs if g]
    cols += [name + " gpu ms/frame" for name, _ in gpu_logs]
    draw_logs = [(name, d) for name, _, _, d in logs if d]
    cols += [name + " draw ms (n)" for name, _ in draw_logs]

    def key(r):
        slowest = max((t.get(r[1], 0.0) for _, t, _, _ in logs), default=0.0)
        drawn = max((d.get(r[1], [0.0])[0] for _, d in draw_logs), default=0.0)
        busiest = max((g.get(r[1], 0.0) for _, g in gpu_logs), default=0.0)
        return (-max(slowest, drawn), -busiest, -r[2]["bytes"])

    out = ["| " + " | ".join(cols) + " |", "|" + "---|" * len(cols)]
    for label, h, s in sorted(rows, key=key)[:top]:
        cells = [label, h or "-", str(s["bytes"]), str(s["lines"]), str(s["blocks"]),
                 "yes" if s["lockstep"] else "no", str(s["loops"]), str(s["gmem_calls"]), str(s["gmem_search"]),
                 str(s["gmem_direct"]), s["search_why"], str(s["registers"]),
                 str(s["private_words"])]
        for _, t, _, _ in logs:
            cells.append("%.1f" % t[h] if h in t else "-")
        for _, g in gpu_logs:
            cells.append("%.2f" % g[h] if h in g else "-")
        for _, d in draw_logs:
            cells.append("%.1f (%d)" % tuple(d[h]) if h in d else "-")
        out.append("| " + " | ".join(cells) + " |")
    return "\n".join(out)


# name(, also with template arguments: bitcast<u32>(, vec4<f32>(, array<u32, 4>(
CALL = re.compile(r"\b([A-Za-z_][A-Za-z0-9_]*)\s*(?:<(?:[^<>()]|<[^<>()]*>)*>)?\s*\(")
CBUF_CONST = re.compile(r"\bcb\d+\[\d+u?\]")
CBUF_ANY = re.compile(r"\bcb\d+\[")
KEYWORDS = {"if", "loop", "switch", "for", "while", "return", "fn",  # statements
            "location", "binding", "group", "builtin", "interpolate", "workgroup_size"}  # attributes


def profile(text, top):
    """Counts of constructs in one module, as (label, count) rows."""
    calls = {}
    for m in CALL.finditer(text):
        name = m.group(1)
        if name not in KEYWORDS:
            calls[name] = calls.get(name, 0) + 1
    cbuf_all = len(CBUF_ANY.findall(text))
    cbuf_const = len(CBUF_CONST.findall(text))
    rows = [
        ("bytes", len(text.encode())),
        ("lines", text.count("\n")),
        ("statements", text.count(";")),
        ("functions", len(re.findall(r"^fn ", text, re.M))),
        ("if", len(re.findall(r"\bif \(", text))),
        ("loop", len(re.findall(r"\bloop \{", text))),
        ("switch", len(re.findall(r"\bswitch\b", text))),
        ("var<private>", text.count("var<private>")),
        ("cbuf reads, constant index", cbuf_const),
        ("cbuf reads, run-time index", cbuf_all - cbuf_const),
    ]
    for name, n in sorted(calls.items(), key=lambda kv: (-kv[1], kv[0]))[:top]:
        rows.append((f"call {name}", n))
    return rows


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dump", nargs="?", help="LGP_WEBGPU_DUMP_DIR of a run")
    ap.add_argument("--profile", nargs="+", metavar="FILE", help="count constructs in these modules instead")
    ap.add_argument("--log", action="append", default=[], metavar="NAME=FILE",
                    help="log of a run on some device (repeatable), e.g. android=run.log")
    ap.add_argument("--all", action="store_true", help="graphics programs too")
    ap.add_argument("--top", type=int, default=20)
    a = ap.parse_args(argv)
    if a.profile:
        profiles = []
        for path in a.profile:
            with open(path, encoding="utf-8") as f:
                profiles.append(dict(profile(f.read(), a.top)))
        labels = []
        for p in profiles:
            labels += [k for k in p if k not in labels]
        names = [os.path.basename(p) for p in a.profile]
        width = max(len(k) for k in labels)
        print(" " * width + "".join(f"  {n:>12}" for n in names))
        for k in labels:
            print(f"{k:<{width}}" + "".join(f"  {p.get(k, 0):>12}" for p in profiles))
        return 0
    if not a.dump:
        ap.error("a dump directory or --profile")
    logs = []
    for spec in a.log:
        name, sep, path = spec.partition("=")
        if not sep:
            ap.error("--log NAME=FILE")
        logs.append((name,) + read_log(path))
    rows = read_dump(a.dump, a.all)
    if not rows:
        print("no prog*.wgsl files in %s (run with LGP_WEBGPU_DUMP_DIR)" % a.dump, file=sys.stderr)
        return 1
    print(table(rows, logs, a.top))
    return 0


if __name__ == "__main__":
    sys.exit(main())
