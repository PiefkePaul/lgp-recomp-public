#!/usr/bin/env python3
"""Run maxwell2wgsl over all shader stages extracted by bnsh.py and count results by failure reason.

For each vertex stage that the BNSH pairs with a pixel stage, the pixel stage is passed as
--link-fs (WebGPU needs matching interpolation on both sides). Errors are grouped by their
message with addresses and instruction words removed; "unknown or unsupported opcode" errors are
additionally grouped by the Maxwell opcode name if --envydis is given (envytools' disassembler,
e.g. "wsl envydis"). With --validate, successful WGSL is checked with wgpu (naga).

Inputs and outputs are game data (shader code, WGSL): keep them in $LGP_PRIVATE_DIR. Only the
aggregated report (--markdown / --json) may go into docs/analysis/.

Usage:
    python -I tools/analyze/shader_coverage.py <stages dir> --tool <maxwell2wgsl> [--out <dir>]
        [--json report.json] [--markdown report.md] [--validate] [--envydis "wsl envydis"] [--census]
        [--ryujinx-shader-cache <dir>] [--jobs N]
"""
from __future__ import annotations

import argparse
import json
import os
import re
import shlex
import struct
import subprocess
import sys
from collections import Counter, defaultdict
from concurrent.futures import ThreadPoolExecutor

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

# Stages maxwell2wgsl translates (compute with --compute: code without SPH).
TRANSLATED = ("vertex", "pixel", "compute")

ADDR_RE = re.compile(r"^(?:error: )?.*?\.bin: (?:0x[0-9a-f]+: )?(?:[0-9a-f]{16}: )?")


def reason(stderr: str) -> str:
    line = next((l for l in stderr.splitlines() if l.strip()), "no output")
    line = ADDR_RE.sub("", line.strip())
    line = re.sub(r"0x[0-9a-f]+", "<n>", line)
    return line


def fail_word(stderr: str) -> int | None:
    m = re.search(r"0x[0-9a-f]+: ([0-9a-f]{16}):", stderr)
    return int(m.group(1), 16) if m else None


def envydis_names(words: list[int], cmd: str) -> dict[int, str]:
    """Opcode mnemonic per 64-bit word via envydis (gm107). Only names are kept."""
    if not words:
        return {}
    # envydis decodes 32-byte bundles (scheduling word first): put each word in its own bundle.
    sched, nop = "0x001fc400 0x001ffc00", "0x00070f00 0x50b00000"
    text = " ".join(f"{sched} 0x{w & 0xFFFFFFFF:08x} 0x{w >> 32:08x} {nop} {nop}" for w in words)
    r = subprocess.run(shlex.split(cmd) + ["-m", "gm107", "-w", "-n"], input=text, capture_output=True, text=True)
    names = {}
    by_addr = {}
    for line in r.stdout.splitlines():
        m = re.match(r"\s*([0-9a-f]+):\s+[0-9a-f]{8} [0-9a-f]{8}\s+(.*)", line)
        if m:
            by_addr[int(m.group(1), 16)] = m.group(2)
    for k, w in enumerate(words):
        parts = [p for p in by_addr.get(32 * k + 8, "?").split() if not p.lstrip("!").startswith("$p")]
        names[w] = parts[0] if parts else "?"
    return names


def compute_args(m: dict) -> list[str]:
    """maxwell2wgsl options of a manifest entry: compute stages (no SPH) with the workgroup and
    memory sizes from their control section (bnsh.py extract), if it read as compute."""
    if m["stage"] != "compute":
        return []
    c = m.get("compute")
    if not c:
        return ["--compute"]
    return ["--compute", "--workgroup", ",".join(map(str, c["workgroup"])), "--shared", str(c["shared"]),
            "--local", str(c["local"])]


def opcode_census(args: argparse.Namespace, manifest: dict) -> dict:
    """All words the decoder rejects (not only the first per shader), named by envydis.

    Returns per opcode the number of shaders containing it, and a greedy order: which opcode,
    once supported, makes the most shaders fully decodable (modes of known opcodes not included).
    """
    def disasm(item):
        sid, m = item
        if m["stage"] not in TRANSLATED:
            return sid, set()
        path = os.path.join(args.stages, f"{sid}.{m['stage']}.bin")
        extra = compute_args(m)
        r = subprocess.run([args.tool, "--disasm", *extra, path], capture_output=True, text=True, errors="replace")
        words = set()
        for line in r.stdout.splitlines():
            m2 = re.match(r"([0-9a-f]+): ([0-9a-f]{16})  (.*)$", line)
            if not m2:
                continue
            addr, text = int(m2.group(1), 16), m2.group(3)
            if text == "???":
                words.add(int(m2.group(2), 16))
            b = re.match(r"bra (0x[0-9a-f]+)$", text)
            if b and addr - 8 <= int(b.group(1), 16) <= addr:
                break  # "bra self" ends the program; what follows is padding
        return sid, words

    with ThreadPoolExecutor(max_workers=args.jobs) as ex:
        per_shader = dict(ex.map(disasm, manifest.items()))
    words = sorted(set().union(*per_shader.values())) if per_shader else []
    names = envydis_names(words, args.envydis)
    need = {sid: {names.get(w, "?") for w in ws} for sid, ws in per_shader.items()}
    count = Counter(op for ops in need.values() for op in ops)
    remaining = {sid: set(ops) for sid, ops in need.items() if ops}
    order = []
    clean = sum(1 for sid, ops in need.items() if not ops and manifest[sid]["stage"] in TRANSLATED)
    while remaining:
        gain = Counter()
        for ops in remaining.values():
            if len(ops) == 1:
                gain[next(iter(ops))] += 1
        pick = gain.most_common(1)[0][0] if gain else Counter(op for ops in remaining.values() for op in ops).most_common(1)[0][0]
        for ops in remaining.values():
            ops.discard(pick)
        done = [sid for sid, ops in remaining.items() if not ops]
        for sid in done:
            del remaining[sid]
        clean += len(done)
        order.append([pick, clean])
    return {"shaders_with_opcode": dict(count.most_common()), "greedy_decodable": order}


def runtime_coverage(args: argparse.Namespace, manifest: dict, ok_by_sid: dict) -> dict:
    """Coverage over the stages the game really used (Ryujinx guest shader cache, P1.4).

    A cached shader is SPH + code up to its end; the BNSH stage additionally holds padding, so
    a stage matches when the cached code is a prefix of it. Unmatched graphics shaders are
    translated directly (written below --out).
    """
    import ryujinx_cache as rc  # needs the "brotli" package for compressed entries
    d = args.ryujinx_shader_cache
    guest = rc.read_guest_shader_cache(os.path.join(d, "guest.toc"), os.path.join(d, "guest.data"))
    codes = {}
    by_head = defaultdict(list)
    for sid, m in manifest.items():
        with open(os.path.join(args.stages, f"{sid}.{m['stage']}.bin"), "rb") as f:
            codes[sid] = f.read()
        by_head[codes[sid][:0x60]].append(sid)
    res = Counter()
    for k, g in enumerate(guest):
        cands = [s for s in by_head.get(g.code[:0x60], []) if codes[s].startswith(g.code)]
        if cands:
            res["matched"] += 1
            res["matched_ok"] += any(ok_by_sid.get(s) for s in cands)
            continue
        res["unmatched"] += 1
        w0 = struct.unpack_from("<I", g.code, 0)[0] if len(g.code) >= 4 else 0
        stype = (w0 >> 10) & 15
        if (w0 & 0x3FF) not in (0x61, 0x62) or stype not in (1, 5):
            res["unmatched_not_vs_ps"] += 1
            continue
        path = os.path.join(args.out or args.stages, f"runtime_{k:04d}.bin")
        with open(path, "wb") as f:
            f.write(g.code)
        r = subprocess.run([args.tool, path, "-o", path + ".wgsl"], capture_output=True, text=True, errors="replace")
        res["unmatched_vs_ps"] += 1
        res["unmatched_ok"] += r.returncode == 0
    res["total"] = len(guest)
    return dict(res)


def run(args: argparse.Namespace) -> dict:
    with open(os.path.join(args.stages, "manifest.json"), encoding="utf-8") as f:
        manifest = json.load(f)
    out_dir = args.out
    if out_dir:
        os.makedirs(out_dir, exist_ok=True)

    def one(item):
        sid, m = item
        st = m["stage"]
        path = os.path.join(args.stages, f"{sid}.{st}.bin")
        if st not in TRANSLATED:
            return sid, st, False, f"stage {st} not handled by the translator", None, None
        cmd = [args.tool, path] + compute_args(m)
        if st == "vertex" and m.get("pixel"):
            cmd += ["--link-fs", os.path.join(args.stages, f"{m['pixel']}.pixel.bin")]
        wgsl = os.path.join(out_dir, f"{sid}.{st}.wgsl") if out_dir else None
        if wgsl:
            cmd += ["-o", wgsl]
        r = subprocess.run(cmd, capture_output=True, text=True, errors="replace")
        if r.returncode == 0:
            return sid, st, True, "", None, wgsl
        return sid, st, False, reason(r.stderr), fail_word(r.stderr), None

    with ThreadPoolExecutor(max_workers=args.jobs) as ex:
        results = list(ex.map(one, manifest.items()))

    by_stage = defaultdict(Counter)
    reasons = defaultdict(Counter)
    unknown_words: dict[int, list[str]] = defaultdict(list)
    ok_files = []
    for sid, st, ok, why, word, wgsl in results:
        by_stage[st]["total"] += 1
        if ok:
            by_stage[st]["ok"] += 1
            if wgsl:
                ok_files.append((sid, st, wgsl))
        else:
            reasons[st][why] += 1
            if word is not None and "unknown or unsupported opcode" in why:
                unknown_words[word].append(st)
    opcodes = Counter()
    if args.envydis and unknown_words:
        names = envydis_names(sorted(unknown_words), args.envydis)
        for w, sts in unknown_words.items():
            opcodes[names.get(w, "?")] += len(sts)
    runtime = runtime_coverage(args, manifest, {sid: ok for sid, _st, ok, *_ in results}) if args.ryujinx_shader_cache else {}
    census = {}
    if args.census and args.envydis:
        census = opcode_census(args, manifest)
    validation = Counter()
    val_errors = Counter()
    if args.validate and ok_files:
        import wgpu  # type: ignore
        device = wgpu.utils.get_default_device()
        for sid, st, path in ok_files:
            with open(path, encoding="utf-8") as f:
                code = f.read()
            try:
                device.create_shader_module(code=code)
                validation[st + "_valid"] += 1
            except Exception as e:  # noqa: BLE001 - wgpu raises various error types
                validation[st + "_invalid"] += 1
                msg = str(e).splitlines()
                val_errors[re.sub(r"\d+", "<n>", next((l for l in msg if "error" in l.lower()), msg[0] if msg else "?"))[:160]] += 1
    return {
        "stages": {k: dict(v) for k, v in by_stage.items()},
        "reasons": {k: dict(v.most_common()) for k, v in reasons.items()},
        "unsupported_opcodes": dict(opcodes.most_common()),
        "census": census,
        "runtime": runtime,
        "validation": dict(validation),
        "validation_errors": dict(val_errors.most_common(20)),
    }


def to_markdown(rep: dict) -> str:
    out = ["| stage | total | translated | share |", "|---|---:|---:|---:|"]
    for st, c in sorted(rep["stages"].items()):
        out.append(f"| {st} | {c['total']} | {c.get('ok', 0)} | {100.0 * c.get('ok', 0) / c['total']:.1f} % |")
    out += ["", "| stage | failure reason (first message per shader) | count |", "|---|---|---:|"]
    for st, rs in sorted(rep["reasons"].items()):
        for why, n in rs.items():
            out.append(f"| {st} | {why.replace('|', '/')} | {n} |")
    if rep["unsupported_opcodes"]:
        out += ["", "| first unsupported opcode | shaders |", "|---|---:|"]
        out += [f"| `{k}` | {v} |" for k, v in rep["unsupported_opcodes"].items()]
    if rep.get("census"):
        out += ["", "| opcode (all occurrences) | shaders with this opcode |", "|---|---:|"]
        out += [f"| `{k}` | {v} |" for k, v in rep["census"]["shaders_with_opcode"].items()]
        out += ["", "greedy order (opcode supported → shaders fully decodable): " + ", ".join(f"{op} → {n}" for op, n in rep["census"]["greedy_decodable"])]
    if rep.get("runtime"):
        rt = rep["runtime"]
        out += ["", f"used at runtime (Ryujinx cache): {rt.get('total', 0)} shaders, {rt.get('matched', 0)} of them found in the "
                    f"BNSH ({rt.get('matched_ok', 0)} translated), {rt.get('unmatched', 0)} not found "
                    f"({rt.get('unmatched_vs_ps', 0)} VS/PS, {rt.get('unmatched_ok', 0)} of them translated; "
                    f"{rt.get('unmatched_not_vs_ps', 0)} other)."]
    if rep["validation"]:
        out += ["", "WGSL validation (wgpu/naga): " + ", ".join(f"{k} {v}" for k, v in sorted(rep["validation"].items()))]
        out += [f"- {k}: {v}" for k, v in rep["validation_errors"].items()]
    return "\n".join(out) + "\n"


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=(__doc__ or "").split("\n\n")[0])
    ap.add_argument("stages")
    ap.add_argument("--tool", required=True)
    ap.add_argument("--out", help="directory for generated WGSL (game data)")
    ap.add_argument("--json")
    ap.add_argument("--markdown")
    ap.add_argument("--validate", action="store_true")
    ap.add_argument("--envydis")
    ap.add_argument("--census", action="store_true", help="count all undecodable opcodes (needs --envydis)")
    ap.add_argument("--ryujinx-shader-cache", help="directory with guest.toc/guest.data: coverage of shaders used at run time")
    ap.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) - 2))
    args = ap.parse_args(argv)
    rep = run(args)
    if args.json:
        with open(args.json, "w", encoding="utf-8") as f:
            json.dump(rep, f, indent=1)
    md = to_markdown(rep)
    if args.markdown:
        with open(args.markdown, "w", encoding="utf-8") as f:
            f.write(md)
    else:
        sys.stdout.buffer.write(md.encode("utf-8"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
