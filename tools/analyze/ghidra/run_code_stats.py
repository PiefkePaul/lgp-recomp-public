#!/usr/bin/env python3
"""Run the P1.5 Ghidra code statistics for ExeFS modules (headless, one project per module).

Steps per module: nso2elf.py (ELF + FDE sidecar) -> analyzeHeadless -import with
LgpSeedFunctions.java as pre-script and LgpCodeStats.java as post-script -> <module>.stats.json.

Everything is written below --work, which must live in $LGP_PRIVATE_DIR (ELF files and Ghidra
projects contain game code). Only the stats JSON is meant to be summarised into docs/analysis/.

Usage:
    python -I tools/analyze/ghidra/run_code_stats.py --exefs $LGP_PRIVATE_DIR/extracted/exefs \
        --work $LGP_PRIVATE_DIR/ghidra [--modules main sdk ...] [--ghidra <install dir>] \
        [--maxmem 12G] [--parallel 2]
"""
from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
NSO2ELF = os.path.join(HERE, "..", "nso2elf.py")
DEFAULT_MODULES = ["rtld", "main", "subsdk0", "subsdk1", "sdk"]


def find_headless(ghidra_dir: str | None) -> str:
    name = "analyzeHeadless.bat" if os.name == "nt" else "analyzeHeadless"
    if ghidra_dir:
        path = os.path.join(ghidra_dir, "support", name)
        if os.path.isfile(path):
            return path
        raise SystemExit(f"error: {path} not found")
    found = shutil.which(name)
    if not found:
        raise SystemExit("error: analyzeHeadless not on PATH; pass --ghidra <install dir>")
    return found


def private_dir_check(work: str) -> None:
    priv = os.environ.get("LGP_PRIVATE_DIR")
    repo = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
    w = os.path.abspath(work)
    if os.path.commonpath([w, repo]) == repo:
        raise SystemExit(f"error: --work {w} is inside the repository; use $LGP_PRIVATE_DIR")
    if priv and os.path.commonpath([w, os.path.abspath(priv)]) != os.path.abspath(priv):
        print(f"warning: --work is outside LGP_PRIVATE_DIR ({priv})", file=sys.stderr)


def run_module(mod: str, args: argparse.Namespace, headless: str) -> int:
    elf_dir = os.path.join(args.work, "elf")
    out_dir = os.path.join(args.work, "stats")
    proj_dir = os.path.join(args.work, "proj")
    log_dir = os.path.join(args.work, "logs")
    for d in (elf_dir, out_dir, proj_dir, log_dir):
        os.makedirs(d, exist_ok=True)
    elf = os.path.join(elf_dir, f"{mod}.elf")
    meta = os.path.join(elf_dir, f"{mod}.meta.json")
    stats = os.path.join(out_dir, f"{mod}.stats.json")
    rc = subprocess.call([sys.executable, "-I", NSO2ELF, os.path.join(args.exefs, mod), elf, "--meta", meta])
    if rc:
        return rc
    cmd = [headless, proj_dir, f"lgp_{mod}", "-import", elf, "-overwrite", "-loader", "ElfLoader",
           "-scriptPath", HERE, "-preScript", "LgpSeedFunctions.java", meta,
           "-postScript", "LgpCodeStats.java", meta, stats,
           "-max-cpu", str(args.max_cpu), "-log", os.path.join(log_dir, f"{mod}.app.log"),
           "-scriptlog", os.path.join(log_dir, f"{mod}.script.log")]
    env = dict(os.environ, GHIDRA_HEADLESS_MAXMEM=args.maxmem)
    with open(os.path.join(log_dir, f"{mod}.stdout.log"), "w", encoding="utf-8") as log:
        rc = subprocess.call(cmd, stdout=log, stderr=subprocess.STDOUT, env=env)
    ok = rc == 0 and os.path.isfile(stats)
    print(f"{mod}: {'ok' if ok else f'FAILED (rc={rc})'} -> {stats}")
    return 0 if ok else (rc or 1)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=(__doc__ or "").split("\n\n")[0])
    ap.add_argument("--exefs", required=True)
    ap.add_argument("--work", required=True)
    ap.add_argument("--modules", nargs="+", default=DEFAULT_MODULES)
    ap.add_argument("--ghidra", help="Ghidra install dir (default: analyzeHeadless on PATH)")
    ap.add_argument("--maxmem", default="12G")
    ap.add_argument("--max-cpu", type=int, default=max(1, (os.cpu_count() or 2) - 2))
    ap.add_argument("--parallel", type=int, default=1)
    args = ap.parse_args(argv)
    private_dir_check(args.work)
    headless = find_headless(args.ghidra)
    with ThreadPoolExecutor(max_workers=args.parallel) as pool:
        rcs = list(pool.map(lambda m: run_module(m, args, headless), args.modules))
    return max(rcs, default=0)


if __name__ == "__main__":
    sys.exit(main())
