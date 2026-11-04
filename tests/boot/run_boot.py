#!/usr/bin/env python3
"""Runs a boot program (lgp_add_boot_executable) and checks its report.

  run_boot.py [--exit-code N] [--lines FILE] [--fixture-expected FILE]
              [--absent TEXT]... [--present TEXT]... [--clean DIR]... [--output FILE] [--cwd DIR]
              -- <program> <args...>

--lines: every line of FILE must appear in the program's stdout, in this
order (other lines may come in between); a line ending in `*` matches any
line that starts with the text before it. `#` lines are comments.
--fixture-expected: an end-state file of the P2.8 fixture runs
(tests/homebrew); the program's debug output must equal its `debug` lines
and its exit reason its `exit` line. Block hashes are not compared: the boot
program uses the production memory layout, the fixture runner a small one.
--absent: TEXT must not occur in stdout.
--present: some line must contain TEXT.
--clean: DIR is emptied before the run (save data of an earlier run).
--output: check FILE (written by the program, e.g. an app's log) instead of
stdout; --cwd: run the program in DIR.
"""
import argparse
import os
import shutil
import subprocess
import sys


def main():
    argv = sys.argv[1:]
    if "--" not in argv:
        raise SystemExit(__doc__)
    split = argv.index("--")
    ap = argparse.ArgumentParser()
    ap.add_argument("--exit-code", type=int, default=0)
    ap.add_argument("--lines")
    ap.add_argument("--fixture-expected")
    ap.add_argument("--absent", action="append", default=[])
    ap.add_argument("--present", action="append", default=[])
    ap.add_argument("--clean", action="append", default=[])
    ap.add_argument("--output")
    ap.add_argument("--cwd")
    a = ap.parse_args(argv[:split])
    cmd = argv[split + 1:]
    for d in a.clean:
        shutil.rmtree(d, ignore_errors=True)
        os.makedirs(d)
    p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, errors="replace", cwd=a.cwd)
    text = p.stdout
    if a.output:
        try:
            with open(a.output, encoding="utf-8", errors="replace") as f:
                text = f.read()
        except OSError as e:
            text = ""
            print(f"run_boot: cannot read {a.output}: {e}")
    out = text.splitlines()
    sys.stdout.write(text)
    sys.stderr.write(p.stderr[-20000:])
    ok = True
    if p.returncode != a.exit_code:
        print(f"run_boot: exit code {p.returncode}, expected {a.exit_code}")
        ok = False
    if a.lines:
        want = [l.rstrip("\n") for l in open(a.lines, encoding="utf-8") if l.strip() and not l.startswith("#")]
        i = 0
        for line in out:
            if i < len(want) and (line == want[i] or (want[i].endswith("*") and line.startswith(want[i][:-1]))):
                i += 1
        if i < len(want):
            print(f"run_boot: missing (in order) from line {i + 1} of {a.lines}: {want[i]!r}")
            ok = False
    if a.fixture_expected:
        exp_debug, exp_exit = [], None
        for line in open(a.fixture_expected, encoding="utf-8"):
            line = line.rstrip("\n")
            if line.startswith("debug "):
                exp_debug.append(line[len("debug "):])
            elif line.startswith("exit "):
                exp_exit = line[len("exit "):]
        got_debug = [l[len("debug: "):] for l in out if l.startswith("debug: ")]
        got_exit = next((l[len("exit: "):] for l in out if l.startswith("exit: ")), None)
        if got_debug != exp_debug:
            print(f"run_boot: debug output differs from {a.fixture_expected}")
            for n in range(max(len(got_debug), len(exp_debug))):
                e = exp_debug[n] if n < len(exp_debug) else "<none>"
                g = got_debug[n] if n < len(got_debug) else "<none>"
                if e != g:
                    print(f"  line {n}: expected {e!r}, got {g!r}")
            ok = False
        if got_exit != exp_exit:
            print(f"run_boot: exit {got_exit!r}, expected {exp_exit!r}")
            ok = False
    for present in a.present:
        if not any(present in line for line in out):
            print(f"run_boot: no line contains {present!r}")
            ok = False
    for absent in a.absent:
        if absent in text:
            print(f"run_boot: unexpected output {absent!r}")
            ok = False
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
