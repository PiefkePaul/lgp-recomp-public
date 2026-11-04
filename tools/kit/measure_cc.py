#!/usr/bin/env python3
"""Peak memory and CPU time of every compile and link step of a kit build (build.py --measure LOG).

As CMake compiler/linker launcher:  measure_cc.py LOG <compiler> <args…>
    runs the step and appends one line to LOG: peak MiB, CPU seconds, wall seconds, exit code, output file.
    Peak = largest process of the step including its children (clang driver → lld): Linux/macOS ru_maxrss of the
    children, Windows the job object's PeakProcessMemoryUsed (commit; for the compiler about its working set).
Summary:                             measure_cc.py report LOG [--top N]

LOG holds paths of the build tree, no game data; it stays on the machine anyway (work folder).
"""
from __future__ import annotations

import argparse
import os
import subprocess
import sys
import time
from pathlib import Path

FIELDS = ("peak_mib", "cpu_s", "wall_s", "exit", "output")


def _output_of(args: list[str]) -> str:
    for flag in ("-o", "/Fo", "-Fo", "--output"):
        if flag in args[:-1]:
            return args[args.index(flag) + 1]
    for a in args:
        if a.startswith(("/Fo", "-Fo")) and len(a) > 3:
            return a[3:]
        if a.startswith("--output="):
            return a.split("=", 1)[1]
    return args[0] if args else "?"


def _run_posix(cmd: list[str]) -> tuple[int, float, float]:
    import resource
    rc = subprocess.call(cmd)
    ru = resource.getrusage(resource.RUSAGE_CHILDREN)
    peak = ru.ru_maxrss / (1 << 20) if sys.platform == "darwin" else ru.ru_maxrss / 1024  # bytes vs. KiB
    return rc, peak, ru.ru_utime + ru.ru_stime


def _run_windows(cmd: list[str]) -> tuple[int, float, float]:
    import ctypes
    from ctypes import wintypes

    class IoCounters(ctypes.Structure):
        _fields_ = [(n, ctypes.c_ulonglong) for n in ("r_ops", "w_ops", "o_ops", "r_bytes", "w_bytes", "o_bytes")]

    class BasicLimit(ctypes.Structure):
        _fields_ = [("PerProcessUserTimeLimit", ctypes.c_longlong), ("PerJobUserTimeLimit", ctypes.c_longlong),
                    ("LimitFlags", wintypes.DWORD), ("MinimumWorkingSetSize", ctypes.c_size_t),
                    ("MaximumWorkingSetSize", ctypes.c_size_t), ("ActiveProcessLimit", wintypes.DWORD),
                    ("Affinity", ctypes.c_size_t), ("PriorityClass", wintypes.DWORD),
                    ("SchedulingClass", wintypes.DWORD)]

    class ExtendedLimit(ctypes.Structure):
        _fields_ = [("BasicLimitInformation", BasicLimit), ("IoInfo", IoCounters),
                    ("ProcessMemoryLimit", ctypes.c_size_t), ("JobMemoryLimit", ctypes.c_size_t),
                    ("PeakProcessMemoryUsed", ctypes.c_size_t), ("PeakJobMemoryUsed", ctypes.c_size_t)]

    class Accounting(ctypes.Structure):
        _fields_ = [("TotalUserTime", ctypes.c_longlong), ("TotalKernelTime", ctypes.c_longlong),
                    ("ThisPeriodTotalUserTime", ctypes.c_longlong), ("ThisPeriodTotalKernelTime", ctypes.c_longlong),
                    ("TotalPageFaultCount", wintypes.DWORD), ("TotalProcesses", wintypes.DWORD),
                    ("ActiveProcesses", wintypes.DWORD), ("TotalTerminatedProcesses", wintypes.DWORD)]

    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    k32.CreateJobObjectW.restype = wintypes.HANDLE
    k32.GetCurrentProcess.restype = wintypes.HANDLE
    # This launcher joins a job of its own (nested jobs: Windows 8+); the compiler and what it starts inherit it.
    job = k32.CreateJobObjectW(None, None)
    if not job or not k32.AssignProcessToJobObject(wintypes.HANDLE(job), wintypes.HANDLE(k32.GetCurrentProcess())):
        return subprocess.call(cmd), -1.0, -1.0
    rc = subprocess.call(cmd)
    ext, acc = ExtendedLimit(), Accounting()
    k32.QueryInformationJobObject(wintypes.HANDLE(job), 9, ctypes.byref(ext), ctypes.sizeof(ext), None)
    k32.QueryInformationJobObject(wintypes.HANDLE(job), 1, ctypes.byref(acc), ctypes.sizeof(acc), None)
    return rc, ext.PeakProcessMemoryUsed / (1 << 20), (acc.TotalUserTime + acc.TotalKernelTime) / 1e7


def launch(log: Path, cmd: list[str]) -> int:
    start = time.monotonic()
    try:
        rc, peak, cpu = (_run_windows if os.name == "nt" else _run_posix)(cmd)
    except OSError as e:
        print(f"measure_cc: {e}", file=sys.stderr)
        return 127
    line = f"{peak:.0f}\t{cpu:.1f}\t{time.monotonic() - start:.1f}\t{rc}\t{_output_of(cmd[1:])}\n"
    with open(log, "a", encoding="utf-8") as f:  # one short write per step: lines of parallel steps stay whole
        f.write(line)
    return rc


def read_log(log: Path) -> list[dict]:
    rows = []
    for raw in log.read_text(encoding="utf-8").splitlines():
        parts = raw.split("\t")
        if len(parts) == len(FIELDS):
            rows.append(dict(peak_mib=float(parts[0]), cpu_s=float(parts[1]), wall_s=float(parts[2]),
                             exit=int(parts[3]), output=parts[4]))
    return rows


def report(log: Path, top: int) -> str:
    rows = read_log(log)
    if not rows:
        return f"{log}: no steps"
    peaks = sorted(r["peak_mib"] for r in rows)
    p90 = peaks[min(len(peaks) - 1, int(len(peaks) * 0.9))]
    out = [f"Steps: {len(rows)}, failed: {sum(r['exit'] != 0 for r in rows)}",
           f"Peak RAM per step: max {peaks[-1]:.0f} MiB, 90 % <= {p90:.0f} MiB, median {peaks[len(peaks) // 2]:.0f} MiB",
           f"CPU time in all: {sum(r['cpu_s'] for r in rows) / 60:.1f} min (with N cores of the same kind about this time / N)",
           f"The {top} largest:"]
    for r in sorted(rows, key=lambda r: -r["peak_mib"])[:top]:
        out.append(f"  {r['peak_mib']:7.0f} MiB  {r['cpu_s']:7.1f} s CPU  {Path(r['output']).name}")
    return "\n".join(out)


def main(argv: list[str] | None = None) -> int:
    argv = sys.argv[1:] if argv is None else argv
    if argv[:1] == ["report"]:
        p = argparse.ArgumentParser(prog="measure_cc.py report")
        p.add_argument("log", type=Path)
        p.add_argument("--top", type=int, default=10)
        args = p.parse_args(argv[1:])
        print(report(args.log, args.top))
        return 0
    if len(argv) < 2:
        print(__doc__, file=sys.stderr)
        return 2
    return launch(Path(argv[0]), argv[1:])


if __name__ == "__main__":
    sys.exit(main())
