#!/usr/bin/env python3
"""Summarise the *.stats.json files written by LgpCodeStats.java as Markdown tables.

The JSON holds only counts and ISA mnemonic names, so the Markdown output may go into
docs/analysis/ (after review). Modules are ordered like the ExeFS load order.

Usage:
    python -I tools/analyze/ghidra/stats_report.py <stats_dir> [--out report.md]
"""
from __future__ import annotations

import argparse
import glob
import json
import os
import sys

ORDER = ["rtld", "main", "subsdk0", "subsdk1", "sdk"]
CLASSES = [
    ("int_alu", "Integer ALU"),
    ("load_store_gpr", "Load/Store GPR"),
    ("load_store_fpsimd", "Load/Store FP/SIMD"),
    ("branch_direct", "Direct branches"),
    ("branch_indirect", "Indirect branches"),
    ("fp_scalar", "FP scalar"),
    ("neon_vector", "NEON vector"),
    ("atomic_exclusive", "Exclusive (LDXR/STXR)"),
    ("atomic_lse", "LSE atomics"),
    ("load_acquire_store_release", "LDAR/STLR"),
    ("crypto_crc", "Crypto/CRC"),
    ("system", "System (SVC, MRS, barriers)"),
]


def load(stats_dir: str) -> list[dict]:
    mods = []
    for path in glob.glob(os.path.join(stats_dir, "*.stats.json")):
        with open(path, encoding="utf-8") as f:
            mods.append(json.load(f))
    rank = {n: i for i, n in enumerate(ORDER)}
    mods.sort(key=lambda m: (rank.get(m["module"], len(ORDER)), m["module"]))
    return mods


def fmt(n: float) -> str:
    return f"{int(n):,}".replace(",", " ")


def pct(part: float, whole: float) -> str:
    return f"{100.0 * part / whole:.2f} %" if whole else "–"


def table(header: list[str], rows: list[list[str]]) -> str:
    out = ["| " + " | ".join(header) + " |", "|" + "|".join("---" if i == 0 else "---:" for i in range(len(header))) + "|"]
    out += ["| " + " | ".join(r) + " |" for r in rows]
    return "\n".join(out)


def report(mods: list[dict]) -> str:
    names = [m["module"] for m in mods]
    parts = []

    rows = []
    for m in mods:
        fn, fde, ins = m["functions"], m["fde"], m["instructions"]
        d = fn["body_bytes_non_thunk"]
        rows.append([m["module"], fmt(fn["total"]), fmt(fn["thunks"]), fmt(fde["count"]),
                     fmt(fn["without_fde"] - fn["without_fde_thunks"]), fmt(fn["fde_without_function"]),
                     fmt(fde["with_lsda"]), fmt(ins["total"]), pct(4 * ins["total"], m["text_bytes"]),
                     fmt(d.get("p50", 0)), fmt(d.get("p90", 0)), fmt(d.get("p99", 0)), fmt(d.get("max", 0))])
    parts.append("### Functions\n\n" + table(
        ["Module", "Functions", "of which thunks", "FDEs", "without FDE (no thunk)", "FDE without function",
         "FDEs with LSDA", "Instructions", "Coverage .text", "Size p50 (B)", "p90", "p99", "max"], rows))

    rows = []
    for key, label in CLASSES:
        row = [label]
        for m in mods:
            ins = m["instructions"]
            row.append(pct(ins["classes"].get(key, 0), ins["total"]))
        rows.append(row)
    parts.append("### Instruction mix (share of all disassembled instructions)\n\n" +
                 table(["Class"] + names, rows))

    keys_br = ["total", "jumptable_recovered", "tailcall_resolved", "in_thunk_plt", "unresolved"]
    keys_blr = ["total", "resolved_by_ghidra", "unresolved"]
    rows = []
    for m in mods:
        ins = m["instructions"]
        br, blr, ret = ins["br"], ins["blr"], ins["ret"]
        calls = ins["calls"]
        rows.append([m["module"], fmt(calls["bl_total"]), fmt(calls["b_tailcalls_to_other_function"])] +
                    [fmt(blr.get(k, 0)) for k in keys_blr] + [fmt(br.get(k, 0)) for k in keys_br] +
                    [fmt(sum(ret.values())), fmt(ret.get("via_other_reg", 0))])
    parts.append("### Branches and calls\n\n" + table(
        ["Module", "BL", "B tail calls", "BLR", "BLR resolved", "BLR open", "BR", "BR jump table",
         "BR tail call resolved", "BR in PLT thunk", "BR open", "RET", "RET ≠ x30"], rows))

    pattern_keys = sorted({k for m in mods for k in m["instructions"]["blr"] if k.startswith("pattern_")})
    rows = [[k.removeprefix("pattern_")] + [fmt(m["instructions"]["blr"].get(k, 0)) for m in mods]
            for k in pattern_keys]
    parts.append("### BLR by origin of the target register (backward search in the basic block)\n\n" +
                 table(["Pattern"] + names, rows))

    pattern_keys = sorted({k for m in mods for k in m["instructions"]["br"] if k.startswith("unresolved_pattern_")})
    rows = [[k.removeprefix("unresolved_pattern_")] + [fmt(m["instructions"]["br"].get(k, 0)) for m in mods]
            for k in pattern_keys]
    parts.append("### Open BR by pattern\n\n" + table(["Pattern"] + names, rows))

    rows = []
    for m in mods:
        jt = m["instructions"]["jumptables"]
        d = jt.get("cases_per_table", {})
        idioms = ", ".join(f"{k.removeprefix('idiom_')}: {fmt(v)}" for k, v in jt.items() if k.startswith("idiom_"))
        rows.append([m["module"], fmt(jt.get("recovered", 0)), fmt(jt.get("cases_total", 0)),
                     fmt(d.get("p50", 0)), fmt(d.get("max", 0)), idioms or "–"])
    parts.append("### Jump tables (Ghidra switch analysis)\n\n" + table(
        ["Module", "Tables", "Cases total", "Cases p50", "Cases max", "Pattern before the BR"], rows))

    rows = []
    for m in mods:
        dp = m["data_pointers"]
        rows.append([m["module"], fmt(m["instructions"]["address_taken_functions_from_code"]),
                     fmt(dp["distinct_functions_referenced_from_data"]), fmt(dp["address_taken_functions_total"]),
                     pct(dp["address_taken_functions_total"], m["functions"]["total"]),
                     fmt(dp["function_pointer_runs_len_ge2"]), fmt(dp["function_pointer_run_slots"]),
                     fmt(dp["longest_function_pointer_run"])])
    parts.append("### Address-taken functions (possible BLR targets) and pointer tables\n\n" + table(
        ["Module", "from code (ADRP/ADD, ADR)", "from data", "total (union)", "share of functions",
         "pointer runs ≥ 2 (vtable-like)", "slots in them", "longest run"], rows))

    tracked = sorted({k for m in mods for k in m["imports"]["tracked_call_sites"]})
    rows = [[k] + [fmt(m["imports"]["tracked_call_sites"].get(k, 0)) for m in mods] for k in tracked]
    parts.append("### Call sites of selected imports\n\n" + (table(["Import"] + names, rows) if rows else "none"))

    rows = []
    for m in mods:
        regs = m["instructions"]["sysregs"]
        svc = m["instructions"]["svc_immediates"]
        rows.append([m["module"], ", ".join(f"{k}: {fmt(v)}" for k, v in regs.items()) or "–",
                     fmt(sum(svc.values()))])
    parts.append("### System registers and SVCs\n\n" + table(["Module", "MRS/MSR", "SVC instructions"], rows))
    return "\n\n".join(parts) + "\n"


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=(__doc__ or "").split("\n\n")[0])
    ap.add_argument("stats_dir")
    ap.add_argument("--out")
    args = ap.parse_args(argv)
    mods = load(args.stats_dir)
    if not mods:
        print(f"error: no *.stats.json in {args.stats_dir}", file=sys.stderr)
        return 1
    text = report(mods)
    if args.out:
        with open(args.out, "w", encoding="utf-8") as f:
            f.write(text)
    else:
        sys.stdout.buffer.write(text.encode("utf-8"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
