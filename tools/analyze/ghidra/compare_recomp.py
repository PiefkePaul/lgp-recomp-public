#!/usr/bin/env python3
"""Cross-check lgp_recomp's CFG (`lgp_recomp functions --json`) against Ghidra (LgpExportCfg.java).

Compares, for one module, function entries and every BR site (jump table / tail call / PLT thunk /
unresolved) and checks the plausibility of jump tables lgp_recomp found but Ghidra did not.
Prints only aggregate counts (Markdown), which may go into docs/analysis/. With --details the
per-address lists are written as JSON; those contain addresses and belong in $LGP_PRIVATE_DIR.

Usage:
    python -I tools/analyze/ghidra/compare_recomp.py <functions.json> <module.cfg.json> \
        --module main [--details out.json]
"""
from __future__ import annotations

import argparse
import json
import sys
from collections import Counter


def load_recomp(path: str, module: str) -> dict:
    with open(path, encoding="utf-8") as f:
        d = json.load(f)
    mods = sorted(((int(m["base"], 16), m["name"]) for m in d["modules"]))
    base = next((b for b, n in mods if n == module), None)
    if base is None:
        raise SystemExit(f"error: module {module!r} not in {path}")
    later = [b for b, _ in mods if b > base]
    end = min(later) if later else 1 << 64
    funcs: dict[int, dict] = {}
    br: dict[int, dict] = {}
    for fn in d["functions"]:
        entry = int(fn["entry"], 16)
        if not base <= entry < end:
            continue
        off = entry - base
        fend = int(fn["end"], 16) - base
        funcs[off] = {"source": fn["source"], "end": fend}
        for b in fn["blocks"]:
            if b["term"] in ("jump_table", "indirect_jump"):
                pc = int(b["end"], 16) - 4 - base
                succ = [int(s, 16) - base for s in b.get("succ", [])]
                br[pc] = {"term": b["term"], "targets": succ, "func": off, "func_end": fend}
    diags = Counter()
    unbounded = set()
    for g in d.get("diagnostics", []):
        pc = int(g["pc"], 16)
        if base <= pc < end:
            diags[g["kind"]] += 1
            if g["kind"] == "jump_table_unbounded":
                unbounded.add(pc - base)
    return {"funcs": funcs, "br": br, "diags": diags, "unbounded": unbounded}


def load_ghidra(path: str) -> dict:
    with open(path, encoding="utf-8") as f:
        d = json.load(f)
    funcs = {e[0]: {"size": e[1], "thunk": bool(e[2])} for e in d["functions"]}
    br = {e[0]: {"kind": e[1], "targets": e[2]} for e in d["br"]}
    return {"funcs": funcs, "br": br}


def compare(rc: dict, gh: dict) -> tuple[dict, dict]:
    rf, gf = rc["funcs"], gh["funcs"]
    both = rf.keys() & gf.keys()
    only_r = rf.keys() - gf.keys()
    only_g = gf.keys() - rf.keys()
    res: dict = {
        "functions_recomp": len(rf), "functions_ghidra": len(gf), "functions_both": len(both),
        "recomp_only_by_source": Counter(rf[a]["source"] for a in only_r),
        "ghidra_only_thunk": sum(gf[a]["thunk"] for a in only_g),
        "ghidra_only_non_thunk": sum(not gf[a]["thunk"] for a in only_g),
    }
    matrix: Counter = Counter()
    jt_cmp: Counter = Counter()
    new_jt: list[dict] = []
    for pc in rc["br"].keys() | gh["br"].keys():
        r = rc["br"].get(pc)
        g = gh["br"].get(pc)
        rk = r["term"] if r else "not_in_cfg"
        gk = g["kind"] if g else "not_in_ghidra"
        matrix[(gk, rk)] += 1
        if r and g and rk == "jump_table" and gk == "jumptable":
            n_r, n_g = len(r["targets"]), g["targets"]
            jt_cmp["same_target_count" if n_r == n_g else ("recomp_more" if n_r > n_g else "recomp_fewer")] += 1
        if r and rk == "jump_table" and gk != "jumptable":
            t = r["targets"]
            inside = all(r["func"] <= x < r["func_end"] for x in t)
            aligned = all(x % 4 == 0 for x in t)
            new_jt.append({"pc": pc, "ghidra": gk, "targets": len(t), "inside_function": inside,
                           "aligned": aligned, "unbounded": pc in rc["unbounded"]})
    res["br_matrix"] = matrix
    res["jumptable_target_counts"] = jt_cmp
    res["new_jumptables"] = new_jt
    details = {
        "recomp_only": sorted(only_r), "ghidra_only": sorted(only_g),
        "br": [{"pc": pc, "ghidra": (gh["br"].get(pc) or {}).get("kind"),
                "recomp": (rc["br"].get(pc) or {}).get("term")} for pc in sorted(rc["br"].keys() | gh["br"].keys())],
        "new_jumptables": new_jt,
    }
    return res, details


def fmt(n: int) -> str:
    return f"{n:,}".replace(",", " ")


def report(module: str, res: dict, diags: Counter) -> str:
    out = [f"### {module}: functions", "",
           "| | count |", "|---|---:|",
           f"| lgp_recomp | {fmt(res['functions_recomp'])} |",
           f"| Ghidra | {fmt(res['functions_ghidra'])} |",
           f"| in both | {fmt(res['functions_both'])} |",
           f"| Ghidra only, thunk | {fmt(res['ghidra_only_thunk'])} |",
           f"| Ghidra only, no thunk | {fmt(res['ghidra_only_non_thunk'])} |"]
    for src, n in sorted(res["recomp_only_by_source"].items(), key=lambda kv: -kv[1]):
        out.append(f"| lgp_recomp only, source `{src}` | {fmt(n)} |")
    out += ["", f"### {module}: BR sites (row Ghidra, column lgp_recomp)", ""]
    rkinds = ["jump_table", "indirect_jump", "not_in_cfg"]
    gkinds = ["jumptable", "tailcall", "thunk", "unresolved", "not_in_ghidra"]
    out.append("| Ghidra \\ lgp_recomp | " + " | ".join(rkinds) + " |")
    out.append("|---|" + "---:|" * len(rkinds))
    for gk in gkinds:
        out.append(f"| {gk} | " + " | ".join(fmt(res["br_matrix"].get((gk, rk), 0)) for rk in rkinds) + " |")
    jt = res["jumptable_target_counts"]
    out += ["", f"jump tables in both: same target count {fmt(jt.get('same_target_count', 0))}, "
                f"lgp_recomp more {fmt(jt.get('recomp_more', 0))}, fewer {fmt(jt.get('recomp_fewer', 0))}."]
    nj = res["new_jumptables"]
    if nj:
        sizes = sorted(x["targets"] for x in nj)
        out += ["", f"jump tables only in lgp_recomp: {fmt(len(nj))} (Ghidra: "
                + ", ".join(f"{k} {fmt(v)}" for k, v in Counter(x['ghidra'] for x in nj).most_common()) + "); "
                f"all targets in their own function: {fmt(sum(x['inside_function'] for x in nj))}, "
                f"all 4-byte aligned: {fmt(sum(x['aligned'] for x in nj))}, "
                f"unbounded (diagnostic): {fmt(sum(x['unbounded'] for x in nj))}; "
                f"target count min/median/max {sizes[0]}/{sizes[len(sizes) // 2]}/{sizes[-1]}."]
    out += ["", f"### {module}: diagnostics from lgp_recomp", "", "| kind | count |", "|---|---:|"]
    out += [f"| {k} | {fmt(v)} |" for k, v in sorted(diags.items(), key=lambda kv: -kv[1])]
    return "\n".join(out) + "\n"


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=(__doc__ or "").split("\n\n")[0])
    ap.add_argument("functions_json")
    ap.add_argument("ghidra_cfg_json")
    ap.add_argument("--module", required=True)
    ap.add_argument("--details")
    args = ap.parse_args(argv)
    rc = load_recomp(args.functions_json, args.module)
    gh = load_ghidra(args.ghidra_cfg_json)
    res, details = compare(rc, gh)
    sys.stdout.buffer.write(report(args.module, res, rc["diags"]).encode("utf-8"))
    if args.details:
        with open(args.details, "w", encoding="utf-8") as f:
            json.dump(details, f)
    return 0


if __name__ == "__main__":
    sys.exit(main())
