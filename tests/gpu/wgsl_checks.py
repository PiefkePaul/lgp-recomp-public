#!/usr/bin/env python3
"""Translates the own test shaders (tests/gpu/shaders/*.hex) with maxwell2wgsl.

    wgsl_checks.py translate <maxwell2wgsl> <out dir>   write all .wgsl (fixture for the other tests)
    wgsl_checks.py disasm <maxwell2wgsl>                 disassembly == the envyas source comments
    wgsl_checks.py naga <out dir>                        validate every .wgsl with naga (exit 77 if absent)
    wgsl_checks.py reject <maxwell2wgsl> <tmp dir>       code that must be rejected, not mistranslated
    wgsl_checks.py structured <out dir>                  every module with branches structured (lockstep compute, fs_irreducible: dispatcher)
                                                          came out structured (the default)

Besides one module per shader, vertex shaders are emitted in the variants the
render test links them in: "<vs>.<fs>.wgsl" (interpolation taken from the
fragment shader's SPH via --link-fs), "vs_alu_mix.flat.wgsl" / "vs_int_ops.flat.wgsl", and the
compute shaders with storage descriptors resolved statically ("<cs>.static.wgsl", --storage-desc;
"cs_memory.moved.wgsl" with slot 2 read from slot 5, --move-slot; "cs_ube.static.vsel.wgsl" with
LGP_WGSL_VECTOR_SELECT=1).
"""
import os
import shutil
import re
import subprocess
import sys
from pathlib import Path

SHADERS = Path(__file__).resolve().parent / "shaders"
SKIP = 77
VARIANTS = [
    ("vs_transform.fs_textured", "vs_transform", ["--link-fs", str(SHADERS / "fs_textured.hex")]),
    ("vs_transform.fs_alpha_loop", "vs_transform", ["--link-fs", str(SHADERS / "fs_alpha_loop.hex")]),
    ("vs_alu_mix.flat", "vs_alu_mix", ["--interp", "0=flat", "--interp", "1=flat"]),
    ("vs_int_ops.flat", "vs_int_ops", ["--interp", "0=flat", "--interp", "1=flat", "--interp", "2=flat"]),
    ("vs_int_input.int", "vs_int_input", ["--interp", "0=flat", "--int-input", "1=u", "--int-input", "2=i"]),
    ("fs_gather.swizzle", "fs_gather", ["--swizzle"]),
] + [
    (f"{cs}.static", cs, ["--storage-desc", "0x310", "--uniform-desc", "0x210"])
    for cs in ("cs_memory", "cs_lockstep", "cs_ube")
] + [
    ("cs_memory.moved", "cs_memory", ["--storage-desc", "0x310", "--move-slot", "2=5"]),
    ("cs_ube.static.vsel", "cs_ube", ["--storage-desc", "0x310", "--uniform-desc", "0x210"],
     {"LGP_WGSL_VECTOR_SELECT": "1"}),
] + [
    (f"vs_float_input.{kind}", "vs_float_input", ["--interp", "0=flat", "--int-input", f"1={kind}"])
    for kind in ("us", "is", "pu", "pi", "psn", "pus", "pis")
]


def run(cmd, env=None):
    r = subprocess.run(cmd, capture_output=True, text=True, env=None if env is None else {**os.environ, **env})
    if r.returncode != 0:
        print(f"FAILED: {' '.join(cmd)}\n{r.stdout}{r.stderr}")
    return r


def translate(tool, out):
    out = Path(out)
    out.mkdir(parents=True, exist_ok=True)
    jobs = [(p.stem, p.stem, []) for p in sorted(SHADERS.glob("*.hex"))] + VARIANTS
    ok = True
    for name, src, extra, *env in jobs:
        ok &= run([tool, *extra, "-o", str(out / f"{name}.wgsl"), str(SHADERS / f"{src}.hex")],
                  env[0] if env else None).returncode == 0
    # Implicit derivatives in a fragment shader need the uniformity check
    # off: predicated instructions are `if` blocks, and Tint (Chrome) rejects
    # textureSample/Bias/Compare and dpdx/dpdy in non-uniform control flow.
    for f in sorted(out.glob("*.wgsl")):
        text = f.read_text()
        if ("// Stage: fragment" in text and re.search(r"\b(textureSample(Bias|Compare)?|dpd[xy])\(", text)
                and "diagnostic(off, derivative_uniformity);" not in text):
            print(f"FAILED: {f.name}: implicit derivatives without diagnostic(off, derivative_uniformity)")
            ok = False
    print(f"translated {len(jobs)} modules to {out}")
    return 0 if ok else 1


def disasm(tool):
    failures = 0
    for hex_file in sorted(SHADERS.glob("*.hex")):
        want = []
        for line in hex_file.read_text().splitlines():
            if line.startswith(("#", "sph", "compute")) or not line.strip():
                continue
            word, _, comment = line.partition("#")
            want.append((word.strip(), comment.strip()))
        r = run([tool, "--disasm", str(hex_file)])
        got = [line.split(None, 2) for line in r.stdout.splitlines()]
        if r.returncode != 0 or len(got) != len(want):
            print(f"{hex_file.name}: {len(got)} disassembled words, {len(want)} expected")
            failures += 1
            continue
        for (word, comment), (addr, gword, text) in zip(want, got):
            if word != gword or comment != text:
                print(f"{hex_file.name} {addr} {word}: source '{comment}', disassembly '{text}'")
                failures += 1
    print(f"disassembly check: {failures} mismatch(es)")
    return 1 if failures else 0


# Programs the translator must refuse (it never translates silently wrong):
# (name, compute?, instruction words, expected error text).
REJECT = [
    # MEMBAR followed by an ordinary instruction: WGSL has no stand-alone fence.
    ("membar_alone", True, ["ef98000000070000", "f0c8000002170000", "e30000000007000f"], "MEMBAR"),
]


def reject(tool, tmp):
    tmp = Path(tmp)
    tmp.mkdir(parents=True, exist_ok=True)
    failures = 0
    for name, compute, words, want in REJECT:
        path = tmp / f"{name}.hex"
        lines = (["compute 1 1 1 0"] if compute else []) + ["001f8000fc0007e0"] + words
        path.write_text("\n".join(lines) + "\n")
        r = subprocess.run([tool, "-o", str(tmp / f"{name}.wgsl"), str(path)], capture_output=True, text=True)
        if r.returncode == 0 or want not in r.stderr + r.stdout:
            print(f"{name}: expected a rejection mentioning '{want}', got exit {r.returncode}: {r.stderr.strip()}")
            failures += 1
    print(f"rejection check: {len(REJECT) - failures}/{len(REJECT)} refused as expected")
    return 1 if failures else 0


def naga(out):
    exe = os.environ.get("NAGA") or shutil.which("naga")
    if not exe:
        print("SKIP: naga not found (cargo install naga-cli)")
        return SKIP
    files = sorted(Path(out).glob("*.wgsl"))
    bad = [f.name for f in files if run([exe, str(f)]).returncode != 0]
    print(f"naga: {len(files) - len(bad)}/{len(files)} modules valid")
    return 1 if bad or not files else 0


def structured(out):
    files = sorted(Path(out).glob("*.wgsl"))
    bad = []
    for f in files:
        m = re.search(r"^// Control flow: (.*)$", f.read_text(), re.M)
        if not m:
            continue
        # Lockstep compute and an irreducible loop keep the dispatcher (the fallback).
        want = "structured" if not f.name.startswith(("cs_lockstep", "fs_irreducible")) else None
        if (m.group(1) == "structured") != (want == "structured"):
            bad.append(f"{f.name} ({m.group(1)})")
    print(f"structured: {len(bad)} module(s) in the wrong form" + (": " + ", ".join(bad) if bad else ""))
    return 1 if bad or not files else 0


if __name__ == "__main__":
    cmd, args = sys.argv[1], sys.argv[2:]
    sys.exit({"translate": lambda: translate(*args), "disasm": lambda: disasm(*args), "naga": lambda: naga(*args),
     "reject": lambda: reject(*args), "structured": lambda: structured(*args)}[cmd]())
