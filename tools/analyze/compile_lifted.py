#!/usr/bin/env python3
"""Compile a lifted C++ file (lgp_recomp lift / lgp_lift output) in parallel chunks and count errors.

A whole-game lift is ~0.5-1 GB of C++; one translation unit is not practical. The file is split
at function definitions into units of --per functions, each repeating the prelude (include and
declarations). Units, objects and the error log are written below --out-dir, which must be in
$LGP_PRIVATE_DIR (they are translated game code). Only the summary on stdout may be shared.

Usage:
    python -I tools/analyze/compile_lifted.py <lifted.cpp> --out-dir <dir> [--per 1000]
        [--mode syntax|obj] [--jobs N] [--opt -O1] [--cxx clang++]
"""
from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
from collections import Counter
from concurrent.futures import ThreadPoolExecutor

REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
DEF_RE = re.compile(r"^void \w+\(lgp::CpuState\* ctx\) \{")


def split(src: str, out_dir: str, per: int) -> tuple[list[str], int]:
    with open(src, encoding="utf-8") as f:
        lines = f.read().split("\n")
    starts = [i for i, line in enumerate(lines) if DEF_RE.match(line)]
    if not starts:
        raise SystemExit(f"error: no lifted function definitions in {src}")
    prelude = "\n".join(lines[:starts[0]]) + "\n"
    bounds = starts + [len(lines)]
    os.makedirs(out_dir, exist_ok=True)
    units = []
    for k in range(0, len(starts), per):
        path = os.path.join(out_dir, f"unit_{k // per:04d}.cpp")
        with open(path, "w", encoding="utf-8") as f:
            f.write(prelude)
            f.write("\n".join(lines[bounds[k]:bounds[min(k + per, len(starts))]]))
            f.write("\n")
        units.append(path)
    return units, len(starts)


def normalise(msg: str) -> str:
    """Error text without addresses and identifiers, so equal causes group together."""
    return re.sub(r"0x[0-9a-fA-F]+|\blgp_fn_\w+|'[^']*'", "<x>", msg)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=(__doc__ or "").split("\n\n")[0])
    ap.add_argument("lifted")
    ap.add_argument("--out-dir", required=True)
    ap.add_argument("--per", type=int, default=1000)
    ap.add_argument("--mode", choices=("syntax", "obj"), default="syntax")
    ap.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) - 2))
    ap.add_argument("--opt", default="-O1")
    ap.add_argument("--cxx", default="clang++")
    args = ap.parse_args(argv)
    out = os.path.abspath(args.out_dir)
    if os.path.commonpath([out, REPO]) == REPO:
        raise SystemExit(f"error: --out-dir {out} is inside the repository; use $LGP_PRIVATE_DIR")

    units, nfun = split(args.lifted, out, args.per)
    flags = ["-std=c++20", "-ferror-limit=0", "-ffp-contract=off", "-fno-fast-math", "-w",
             f"-I{REPO}/recomp/lifter/include", f"-I{REPO}/runtime/core/include", f"-I{REPO}/platform/include"]

    def compile_unit(path: str) -> tuple[int, str]:
        cmd = [args.cxx, *flags, path]
        if args.mode == "syntax":
            cmd.append("-fsyntax-only")
        else:
            cmd += ["-c", args.opt, "-o", path[:-4] + ".o"]
            if os.name == "nt":
                cmd.append("-fms-runtime-lib=dll")  # match the /MD CMake libraries for linking
        r = subprocess.run(cmd, capture_output=True, text=True, errors="replace")
        return r.returncode, r.stderr

    errors: Counter = Counter()
    failed = 0
    with ThreadPoolExecutor(max_workers=args.jobs) as ex, \
            open(os.path.join(out, "errors.log"), "w", encoding="utf-8") as log:
        for rc, err in ex.map(compile_unit, units):
            failed += rc != 0
            log.write(err)
            for line in err.splitlines():
                m = re.search(r"\berror: (.*)", line)
                if m:
                    errors[normalise(m.group(1))] += 1
    print(f"functions {nfun}  units {len(units)}  failed units {failed}  errors {sum(errors.values())}")
    for msg, n in errors.most_common(20):
        print(f"  {n:7d}  {msg}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
