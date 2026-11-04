#!/usr/bin/env python3
"""Regenerate tests/gpu/texture/astc_vectors.txt with ARM's astcenc as oracle.

astcenc (Apache-2.0, https://github.com/ARM-software/astc-encoder) is only a
tool here, nothing of it lands in the repository. Build it once:

    git clone --depth 1 --branch 5.3.0 https://github.com/ARM-software/astc-encoder.git
    cmake -S astc-encoder -B astc-encoder/build -G Ninja -DCMAKE_BUILD_TYPE=Release -DASTCENC_ISA_NONE=ON
    cmake --build astc-encoder/build

then run:

    python3 tools/texture/gen_astc_vectors.py --astcenc astc-encoder [--per-kind 60]

The vectors only contain own synthetic images and random blocks.
"""
import argparse
import os
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--astcenc", required=True, help="astc-encoder checkout with a build/ directory")
    ap.add_argument("--build-dir", help="astcenc build directory (default: <astcenc>/build)")
    ap.add_argument("--per-kind", type=int, default=60, help="vectors per kind and footprint")
    ap.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    ap.add_argument("--out", default=str(ROOT / "tests/gpu/texture/astc_vectors.txt"))
    args = ap.parse_args()

    src = pathlib.Path(args.astcenc)
    build = pathlib.Path(args.build_dir) if args.build_dir else src / "build"
    libs = sorted(build.rglob("libastcenc-*-static.a"))
    if not libs:
        print(f"no libastcenc-*-static.a under {build}", file=sys.stderr)
        return 1
    with tempfile.TemporaryDirectory() as tmp:
        exe = pathlib.Path(tmp) / "astc_vectors_gen"
        subprocess.run([args.cxx, "-O2", "-std=c++17", "-I", str(src / "Source"),
                        str(ROOT / "tools/texture/astc_vectors_gen.cpp"), str(libs[0]), "-o", str(exe), "-lpthread"],
                       check=True)
        out = subprocess.run([str(exe), str(args.per_kind)], check=True, capture_output=True, text=True).stdout
    pathlib.Path(args.out).write_text(out)
    print(f"wrote {sum(1 for l in out.splitlines() if not l.startswith('#'))} vectors to {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
