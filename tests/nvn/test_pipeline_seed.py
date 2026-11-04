#!/usr/bin/env python3
"""lgp_pipeline_seed (gpu/webgpu/tools/pipeline_seed.cpp) on the project's own test shaders.

Usage: test_pipeline_seed.py <lgp_pipeline_seed> <tests/gpu/shaders dir> <source dir>

Builds a stage directory like `tools/analyze/bnsh.py extract` writes it (stage files plus
programs.tsv) from own shaders, then checks catalog, info and merge, and that the tool refuses to
write a seed inside the source tree.
"""
import os
import shutil
import struct
import subprocess
import sys
import tempfile

MAGIC = b"LGPPIPE1"
PROGRAM_BIT = 0x8000_0000


def read_hex(path):
    """Same reading as read_hex in tests/nvn/test_nvn_webgpu_draw.cpp: SPH words, then 64-bit code words."""
    out = bytearray()
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.split("#", 1)[0].split()
            if not line or line[0] == "compute":
                continue
            if line[0] == "sph":
                for w in line[1:]:
                    out += struct.pack("<I", int(w, 16))
            else:
                out += struct.pack("<Q", int(line[0], 16))
    return bytes(out)


def records(path):
    with open(path, "rb") as f:
        data = f.read()
    assert data[:8] == MAGIC, path
    (fp_len,) = struct.unpack_from("<I", data, 8)
    at = 12 + fp_len
    out = []
    while at + 12 <= len(data):
        h, word = struct.unpack_from("<QI", data, at)
        n = word & ~PROGRAM_BIT
        out.append((h, bool(word & PROGRAM_BIT), data[at + 12:at + 12 + n]))
        at += 12 + n
    assert at == len(data), "damaged file"
    return out


def write_seed(path, recs):
    fp = b"test seed"
    with open(path, "wb") as f:
        f.write(MAGIC + struct.pack("<I", len(fp)) + fp)
        for h, program, body in recs:
            f.write(struct.pack("<QI", h, len(body) | (PROGRAM_BIT if program else 0)) + body)


def run(tool, *args, ok=True):
    p = subprocess.run([tool, *args], capture_output=True, text=True)
    sys.stdout.write(p.stdout + p.stderr)
    if ok and p.returncode != 0:
        raise SystemExit(f"FAIL: exit {p.returncode}: {' '.join(args)}")
    return p


def expect(text, needle):
    if needle not in text:
        raise SystemExit(f"FAIL: expected {needle!r}")


def main():
    tool, shaders, source = sys.argv[1:4]
    with tempfile.TemporaryDirectory() as d:
        stages = os.path.join(d, "stages")
        os.makedirs(stages)
        for name, sid, kind in (("vs_transform", "v1", "vertex"), ("fs_textured", "p1", "pixel"),
                                ("fs_alpha_loop", "p2", "pixel"), ("cs_memory", "c1", "compute")):
            with open(os.path.join(stages, f"{sid}.{kind}.bin"), "wb") as f:
                f.write(read_hex(os.path.join(shaders, f"{name}.hex")))
        with open(os.path.join(stages, "programs.tsv"), "w", encoding="utf-8") as f:
            f.write("graphics\tv1\tp1\t2\n"        # vertex + pixel
                    "graphics\tv1\tp2\t1\n"        # other pixel partner: another program
                    "graphics\tv1\t-\t1\n"         # vertex only (depth pass)
                    "graphics\tv1\tp1\t1\n"        # the same program again (another container)
                    "graphics\tv9\tp1\t1\n"        # stage file missing
                    "compute\tc1\t64\t1\t1\t272\t0\t1\n")
        cat = os.path.join(d, "catalog.lgppipes")
        out = run(tool, "catalog", stages, "--out", cat, "--jobs", "3").stdout
        expect(out, "translated: graphics 4 of 5, compute 1 of 1")
        expect(out, "distinct programs: 3 graphics, 1 compute")
        expect(out, "failed: 1 x stage file missing")
        recs = records(cat)
        if len(recs) != 4 or not all(program for _, program, _ in recs):
            raise SystemExit(f"FAIL: catalog has {len(recs)} records, expected 4 programs")
        if b"@vertex" not in recs[0][2] and b"@vertex" not in recs[1][2]:
            raise SystemExit("FAIL: no WGSL vertex entry point in the catalog")

        # A seed as a run writes it: one catalog program with two recipes, one foreign program.
        seen_hash = recs[0][0]
        seed = os.path.join(d, "seed.lgppipes")
        write_seed(seed, [(seen_hash, True, recs[0][2]), (seen_hash, False, b"recipe A"),
                          (seen_hash, False, b"recipe B"), (0x1234, True, b"not a program"),
                          (0x1234, False, b"recipe C")])
        out = run(tool, "catalog", stages, "--seed", seed).stdout
        expect(out, "seed programs in the catalog: 1 of 2")
        expect(out, "seed recipes whose program is in the catalog: 2 of 3")
        expect(out, "catalog programs a run has seen: 1 of 4")

        # Explaining seed programs the catalog lacks: a catalog with fewer programs, the full
        # catalog as the seed.
        stages2 = os.path.join(d, "stages2")
        shutil.copytree(stages, stages2)
        with open(os.path.join(stages2, "programs.tsv"), "w", encoding="utf-8") as f:
            f.write("graphics\tv1\tp1\t1\npixel\tp1\t1\npixel\tp2\t1\n")
        out = run(tool, "catalog", stages2, "--seed", cat).stdout
        expect(out, "seed programs in the catalog: 1 of 4")
        expect(out, "pixel stages translated alone: 2 of 2")
        expect(out, "1 x vertex only:")
        expect(out, "1 x compute: WGSL not in the containers")
        if out.count("linked differently at run time") + out.count("vertex WGSL differs") != 1:
            raise SystemExit("FAIL: v1+p2 not explained as linked differently")
        # More candidate links from a second list close the gap.
        with open(os.path.join(stages2, "programs-within-file.tsv"), "w", encoding="utf-8") as f:
            f.write("graphics\tv1\tp2\t1\n")
        out = run(tool, "catalog", stages2, "--seed", cat, "--programs", "programs.tsv",
                  "--programs", "programs-within-file.tsv").stdout
        expect(out, "program lists: 2 graphics, 0 compute lines")
        expect(out, "seed programs in the catalog: 2 of 4")

        merged = os.path.join(d, "merged.lgppipes")
        out = run(tool, "merge", "--out", merged, cat, seed, seed).stdout
        expect(out, f"{merged}: 5 programs, 3 recipes")
        mrecs = records(merged)
        kinds = [program for _, program, _ in mrecs]
        if kinds != [True] * 5 + [False] * 3:
            raise SystemExit(f"FAIL: merged order {kinds}, expected programs first")
        # The output may be one of the inputs: read before it is replaced.
        inplace = os.path.join(d, "inplace.lgppipes")
        shutil.copyfile(merged, inplace)
        out = run(tool, "merge", "--out", inplace, inplace, seed).stdout
        expect(out, f"{inplace}: 5 programs, 3 recipes")
        if os.path.exists(inplace + ".part"):
            raise SystemExit("FAIL: temporary file left behind")
        out = run(tool, "info", merged).stdout
        expect(out, "programs: 5 (3 graphics, 1 compute, 1 undecodable)")
        expect(out, "draw recipes: 3 for 2 programs")
        expect(out, "programs with recipes: 2 of 5")

        # Seeds are derived from game data: never written into the source tree.
        p = run(tool, "merge", "--out", os.path.join(source, "build", "refused.lgppipes"), seed, ok=False)
        if p.returncode == 0 or "inside the source tree" not in p.stderr:
            raise SystemExit("FAIL: wrote a seed inside the source tree")
        p = run(tool, "merge", "--out", os.path.join(d, "x.bin"), seed, ok=False)
        if p.returncode == 0:
            raise SystemExit("FAIL: accepted an output without .lgppipes")
    print("OK")


if __name__ == "__main__":
    main()
