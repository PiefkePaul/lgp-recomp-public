#!/usr/bin/env python3
"""Compare the jump tables of two `lgp_recomp functions --json` runs (before/after a change).

Prints counts only (safe to share): tables, bounded tables, unresolved BRs, BRs that became
tables, tables that disappeared or changed, and how many case targets are new. With --list
the BR addresses are printed as well; those belong in $LGP_PRIVATE_DIR, not into the repo.

Usage:
    python -I tools/analyze/compare_jump_tables.py <before.json> <after.json> [--list]
"""
from __future__ import annotations

import argparse
import json


def load(path: str) -> dict:
    with open(path, encoding="utf-8") as f:
        d = json.load(f)
    tables: dict[int, dict] = {}
    for fn in d.get("functions", []):
        for t in fn.get("jump_tables", []):
            tables[int(t["br"], 16)] = {
                "count": t["count"],
                "bounded": bool(t["bound_known"]),
                "targets": tuple(int(x, 16) for x in t["targets"]),
            }
    unresolved = {int(x["pc"], 16) for x in d.get("diagnostics", []) if x["kind"] == "unresolved_indirect_jump"}
    return {"tables": tables, "unresolved": unresolved}


def compare(before: dict, after: dict) -> dict:
    b, a = before["tables"], after["tables"]
    new = sorted(set(a) - set(b))
    lost = sorted(set(b) - set(a))
    changed = sorted(br for br in set(a) & set(b) if a[br] != b[br])
    old_targets = {x for t in b.values() for x in t["targets"]}
    new_targets = {x for t in a.values() for x in t["targets"]} - old_targets
    return {
        "tables": (len(b), len(a)),
        "bounded": (sum(t["bounded"] for t in b.values()), sum(t["bounded"] for t in a.values())),
        "unresolved": (len(before["unresolved"]), len(after["unresolved"])),
        "new": new,
        "new_from_unresolved": sorted(br for br in new if br in before["unresolved"]),
        "lost": lost,
        "changed": changed,
        "new_targets": len(new_targets),
    }


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("before")
    ap.add_argument("after")
    ap.add_argument("--list", action="store_true", help="also print BR addresses (keep private)")
    args = ap.parse_args()
    r = compare(load(args.before), load(args.after))
    print("| | before | after |\n|---|---:|---:|")
    print(f"| jump tables | {r['tables'][0]} | {r['tables'][1]} |")
    print(f"| of these bounded by cmp | {r['bounded'][0]} | {r['bounded'][1]} |")
    print(f"| unresolved BR | {r['unresolved'][0]} | {r['unresolved'][1]} |")
    print()
    print(f"new tables: {len(r['new'])} (of these unresolved before: {len(r['new_from_unresolved'])})")
    print(f"dropped tables: {len(r['lost'])}")
    print(f"changed tables (count/targets): {len(r['changed'])}")
    print(f"new case targets: {r['new_targets']}")
    if args.list:
        for name in ("new", "lost", "changed"):
            for br in r[name]:
                print(f"{name} 0x{br:x}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
