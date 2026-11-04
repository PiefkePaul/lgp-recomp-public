#!/usr/bin/env python3
"""Compare the lifted-code benchmark variants (docs/knowledge/lifted-code-performance.md).

usage: measure.py <build>/bench/lifter [--repeat R] [--scale S] [--callgrind] [--cg-scale S]
                  [--stem <native build>/bench/lifter/generated/program] [--node]

For every variant binary found (lgp_bench_lifted_ctx, lgp_bench_lifted, lgp_bench_lifted_hostnan) prints
  - wall-clock time per kernel, lifted and native (best of R runs),
  - with --callgrind: executed host instructions per kernel (valgrind, exact and
    independent of machine load; noisy cloud runners make times unreliable),
  - size of the lifted object file's code (.text + .text.*, Mach-O __text).
Output is a Markdown table. WebAssembly build: --node runs the .js files with Node and --stem
points at the program the native build generated (LGP_BENCH_IMPORT_DIR).
"""
import argparse
import csv
import io
import os
import re
import shutil
import subprocess
import sys

VARIANTS = [("ctx", "lgp_bench_lifted_ctx", "program_lifted_ctx.cpp"), ("default", "lgp_bench_lifted", "program_lifted.cpp"),
            ("hostnan", "lgp_bench_lifted_hostnan", "program_lifted.cpp")]


def run_csv(binary, stem, args, node=False):
    out = subprocess.run((["node"] if node else []) + [binary, stem, "--csv"] + args, check=False, capture_output=True, text=True)
    if out.returncode != 0 or "MISMATCH" in out.stdout:
        sys.exit(f"{binary} failed (exit {out.returncode}, wrong checksum or error):\n{out.stdout}{out.stderr}")
    rows = list(csv.reader(io.StringIO(out.stdout)))
    return {r[0]: r for r in rows[1:]}


def callgrind(binary, stem, kernel, scale, toggle):
    out_file = os.path.join(os.path.dirname(binary), "callgrind.tmp")
    cmd = ["valgrind", "--tool=callgrind", f"--callgrind-out-file={out_file}", "--collect-atstart=no",
           f"--toggle-collect={toggle}", binary, stem, "--kernel", str(kernel), "--scale", str(scale), "--repeat", "1"]
    out = subprocess.run(cmd, check=False, capture_output=True, text=True)
    os.remove(out_file) if os.path.exists(out_file) else None
    if out.returncode != 0 or "MISMATCH" in out.stdout:
        sys.exit(f"{binary} under callgrind failed (exit {out.returncode}):\n{out.stdout}{out.stderr[-2000:]}")
    m = re.search(r"Collected : (\d+)", out.stderr)
    if not m:
        sys.exit(f"callgrind failed:\n{out.stderr[-2000:]}")
    return int(m.group(1))


def text_size(obj):
    tool = shutil.which("size") or shutil.which("llvm-size")
    if not tool:
        return None
    out = subprocess.run([tool, "-A", obj], check=False, capture_output=True, text=True).stdout
    total = 0
    for line in out.splitlines():
        parts = line.split()
        # ELF: .text, .text.*; Mach-O (llvm-size -A): __text
        name = parts[0] if parts else ""
        if len(parts) >= 2 and (name in (".text", "__text") or name.startswith(".text.")) and parts[1].isdigit():
            total += int(parts[1])
    return total


def find_object(bench_dir, binary_name, source_name):
    for root, _, files in os.walk(os.path.join(bench_dir, "CMakeFiles", binary_name + ".dir")):
        for f in files:
            if f.startswith(source_name) and f.endswith((".o", ".obj")):
                return os.path.join(root, f)
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("bench_dir")
    ap.add_argument("--repeat", type=int, default=5)
    ap.add_argument("--scale", type=float, default=1.0)
    ap.add_argument("--callgrind", action="store_true")
    ap.add_argument("--cg-scale", type=float, default=0.1)
    ap.add_argument("--stem")
    ap.add_argument("--node", action="store_true")
    a = ap.parse_args()
    stem = a.stem or os.path.join(a.bench_dir, "generated", "program")
    with open(stem + ".meta") as f:
        entry = int(next(l.split()[1] for l in f if l.startswith("entry")))
    entry_fn = f"lgp_fn_{entry:x}*"

    results = {}
    kernels = None
    for name, binary_name, source in VARIANTS:
        binary = os.path.join(a.bench_dir, binary_name + (".js" if a.node else ""))
        if not a.node and not os.path.exists(binary) and os.path.exists(binary + ".exe"):
            binary += ".exe"  # Windows
        if not os.path.exists(binary):
            continue
        extra = ["--scale", str(a.scale), "--repeat", str(a.repeat)]
        rows = run_csv(binary, stem, extra, a.node)
        kernels = kernels or list(rows)
        r = {"rows": rows}
        obj = find_object(a.bench_dir, binary_name, source)
        r["text"] = text_size(obj) if obj and not a.node else None
        wasm = binary[:-3] + ".wasm"
        r["wasm"] = os.path.getsize(wasm) if a.node and os.path.exists(wasm) else None
        r["obj"] = os.path.getsize(obj) if obj and a.node else None
        if a.callgrind:
            r["ir"] = {}
            for k, kname in enumerate(kernels):
                r["ir"][kname] = callgrind(binary, stem, k, a.cg_scale, entry_fn)
            if "native_ir" not in results:
                results["native_ir"] = {
                    kname: callgrind(binary, stem, k, a.cg_scale, "lgp_bench_native::bench_main*")
                    for k, kname in enumerate(kernels)
                }
        results[name] = r

    if kernels is None:
        sys.exit(f"no benchmark binaries in {a.bench_dir}")
    names = [n for n, _, _ in VARIANTS if n in results]
    head = "| Kernel | native ms | " + " | ".join(f"{n} ms" for n in names)
    sep = "|---|---:|" + "---:|" * len(names)
    if a.callgrind:
        head += " | native Minstr | " + " | ".join(f"{n} Minstr (×)" for n in names)
        sep += "---:|" + "---:|" * len(names)
    print(head + " |")
    print(sep)
    for k in kernels:
        native = min(float(results[n]["rows"][k][2]) for n in names)
        line = f"| {k} | {native:.2f} | " + " | ".join(
            f"{float(results[n]['rows'][k][3]):.2f} ({float(results[n]['rows'][k][3]) / native:.1f}×)" for n in names)
        if a.callgrind:
            ni = results["native_ir"][k]
            line += f" | {ni / 1e6:.1f} | " + " | ".join(
                f"{results[n]['ir'][k] / 1e6:.1f} ({results[n]['ir'][k] / ni:.1f}×)" for n in names)
        print(line + " |")
    for n in names:
        if results[n]["text"] is not None:
            print(f"\n{n}: lifted object .text = {results[n]['text']} bytes", end="")
        if results[n]["wasm"] is not None:
            print(f"\n{n}: whole .wasm = {results[n]['wasm']} bytes", end="")
        if results[n]["obj"] is not None:
            print(f"\n{n}: lifted wasm object = {results[n]['obj']} bytes", end="")
    print()


if __name__ == "__main__":
    main()
