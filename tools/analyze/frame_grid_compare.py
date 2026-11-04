#!/usr/bin/env python3
"""Compares the `--frame-grid` lines of two lgp_game logs (e.g. browser vs native).

Prints numbers only: per frame the largest and mean per-channel difference of
the 32x18 cell means, and the cells that differ by more than --threshold.
The logs themselves derive from game images and stay in LGP_PRIVATE_DIR.
"""
import argparse
import re

CELLS = 32 * 18


def load(path):
    frames, current = {}, None
    with open(path, errors="replace") as f:
        for line in f:
            m = re.search(r"frame (\d+): \d+x\d+", line)
            if m:
                current = int(m.group(1))
                continue
            m = re.search(r"grid 32x18:((?: [0-9a-f]{6})+)", line)
            if m and current is not None:
                frames[current] = [tuple(int(c[i:i + 2], 16) for i in (0, 2, 4)) for c in m.group(1).split()]
    return frames


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("log_a")
    ap.add_argument("log_b")
    ap.add_argument("--threshold", type=int, default=16, help="cell difference that counts as a mismatch")
    args = ap.parse_args()
    a, b = load(args.log_a), load(args.log_b)
    worst = 0
    broken = sorted(n for n in set(a) & set(b) if len(a[n]) != CELLS or len(b[n]) != CELLS)
    if broken:
        for n in broken:
            print(f"frame {n}: incomplete grid ({len(a[n])} vs {len(b[n])} of {CELLS} cells)")
        return 2
    for n in sorted(set(a) & set(b)):
        diffs = [max(abs(x - y) for x, y in zip(p, q)) for p, q in zip(a[n], b[n])]
        bad = [i for i, d in enumerate(diffs) if d > args.threshold]
        worst = max(worst, max(diffs))
        cells = f" (cells x,y: {', '.join(f'{i % 32},{i // 32}' for i in bad[:12])})" if bad else ""
        print(f"frame {n}: max cell diff {max(diffs)}, mean {sum(diffs) / len(diffs):.2f}, "
              f"cells > {args.threshold}: {len(bad)}{cells}")
    only = sorted(set(a) ^ set(b))
    if only:
        print("frames in only one log:", only)
    if not set(a) & set(b):
        print("no common frames with a grid line (run both with --frame-stats N --frame-grid)")
        return 2
    return 1 if worst > args.threshold else 0


if __name__ == "__main__":
    raise SystemExit(main())
