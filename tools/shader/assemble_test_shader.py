#!/usr/bin/env python3
"""Assembles a self-written Maxwell test shader into the .hex text format.

Only for own test shaders (tests/gpu/shaders/*.s). Needs envyas from
envytools (MIT, https://github.com/envytools/envytools), built locally:

    python3 tools/shader/assemble_test_shader.py --envyas <path/to/envyas> in.s out.hex

Input format (envydis/envyas gm107 syntax, one instruction per line):

    .stage vertex | fragment | compute
    .workgroup <x> <y> <z>        compute: workgroup size (default 1 1 1)
    .shared <bytes>               compute: shared memory size (default 0 = translator default)
    .in  <i> <xyzw>               vertex: generic input vector i, components used
    .in  <i> <mode> <xyzw>        fragment: mode = perspective | linear | constant
    .out <i> <xyzw>               vertex: generic output vector i
    .target <t> <xyzw>            fragment: colour target t
    .depth                        fragment: writes depth
    name:                         label
    <instruction>                 branch targets (bra/ssy/pbk) may be written as @name

The output lists the 20 SPH words (compute: a "compute X Y Z SHARED" line
instead, NVN compute code has no SPH), then the code in bundles of one
scheduling word plus three instructions; every instruction line carries its
source text as a comment so the decoder test can compare disassembly.
"""
import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

SCHED = 0x001F8000FC0007E0   # arbitrary valid scheduling word (ignored by the translator)
NOP = "nop 0x0"
BRANCHES = ("bra", "ssy", "pbk")


def set_bits(words, lo, n, value):
    for i in range(n):
        b = lo + i
        if (value >> i) & 1:
            words[b // 32] |= 1 << (b % 32)


def comp_mask(s):
    return sum(1 << "xyzw".index(c) for c in s)


def build_sph(directives):
    words = [0] * 20
    stage = directives["stage"]
    if stage == "vertex":
        set_bits(words, 0, 5, 1)       # SPH type 1 (VTG)
        set_bits(words, 5, 5, 3)       # version 3
        set_bits(words, 10, 4, 1)      # shader type: vertex
        for i, mask in directives["in"]:
            set_bits(words, 192 + 4 * i, 4, mask)
        for i, mask in directives["out"]:
            set_bits(words, 432 + 4 * i, 4, mask)
    elif stage == "fragment":
        set_bits(words, 0, 5, 2)       # SPH type 2 (PS)
        set_bits(words, 5, 5, 3)
        set_bits(words, 10, 4, 5)      # shader type: pixel
        modes = {"constant": 1, "perspective": 2, "linear": 3}
        for i, mode, mask in directives["in"]:
            for c in range(4):
                if mask & (1 << c):
                    set_bits(words, 192 + (4 * i + c) * 2, 2, modes[mode])
        for t, mask in directives["target"]:
            set_bits(words, 576 + 4 * t, 4, mask)
        if directives["depth"]:
            set_bits(words, 609, 1, 1)
    else:
        sys.exit("missing or bad .stage")
    return words


def slot_address(index):
    """Byte address of the index-th instruction (scheduling words skipped)."""
    return (index // 3) * 32 + 8 + (index % 3) * 8


def assemble_one(envyas, text):
    with tempfile.TemporaryDirectory() as tmp:
        src = Path(tmp) / "in.s"
        src.write_text(text + "\n")
        out = subprocess.run([envyas, "-m", "gm107", "-w", str(src)], capture_output=True, text=True)
    words = [int(w.strip().rstrip(","), 16) for w in out.stdout.split() if w.strip()]
    if out.returncode != 0 or len(words) != 2:
        sys.exit(f"envyas failed on '{text}': {out.stderr.strip()}")
    return words[0] | (words[1] << 32)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--envyas", required=True)
    ap.add_argument("input")
    ap.add_argument("output")
    args = ap.parse_args()

    directives = {"stage": None, "in": [], "out": [], "target": [], "depth": False,
                  "workgroup": (1, 1, 1), "shared": 0}
    insns, labels = [], {}
    for raw in Path(args.input).read_text().splitlines():
        line = raw.split("//")[0].strip()
        if not line:
            continue
        if line.startswith("."):
            parts = line.split()
            key = parts[0][1:]
            if key == "stage":
                directives["stage"] = parts[1]
            elif key == "in" and len(parts) == 4:
                directives["in"].append((int(parts[1]), parts[2], comp_mask(parts[3])))
            elif key in ("in", "out", "target"):
                directives[key].append((int(parts[1]), comp_mask(parts[2])))
            elif key == "depth":
                directives["depth"] = True
            elif key == "workgroup":
                directives["workgroup"] = tuple(int(v, 0) for v in parts[1:4])
            elif key == "shared":
                directives["shared"] = int(parts[1], 0)
            else:
                sys.exit(f"unknown directive: {line}")
        elif line.endswith(":"):
            labels[line[:-1]] = slot_address(len(insns))
        else:
            insns.append(line)
    while len(insns) % 3:
        insns.append(NOP)

    lines = [f"# Generated by tools/shader/assemble_test_shader.py from {Path(args.input).name}. Do not edit.",
             "# Own test shader, no game code."]
    if directives["stage"] == "compute":
        x, y, z = directives["workgroup"]
        lines.append(f"compute {x} {y} {z} {directives['shared']}")
    else:
        sph = build_sph(directives)
        lines += ["sph " + " ".join(f"{w:08x}" for w in sph[:10]),
                  "sph " + " ".join(f"{w:08x}" for w in sph[10:])]
    for index, text in enumerate(insns):
        addr = slot_address(index)
        if index % 3 == 0:
            lines.append(f"{SCHED:016x}  # sched")
        asm = text
        mnemonic_pos = re.match(r"((?:not\s+)?\$p\d\s+|never\s+)?(\w+)", text)
        if mnemonic_pos and mnemonic_pos.group(2) in BRANCHES:
            m = re.search(r"@(\w+)|(0x[0-9a-fA-F]+)\s*$", text)
            target = labels[m.group(1)] if m.group(1) else int(m.group(2), 16)
            asm = text[:m.start()] + f"0x{target:x}"
            # envyas assembles at pc 0 and rejects negative targets: assemble a
            # zero offset (target 0x8) and patch the signed 24-bit offset.
            word = assemble_one(args.envyas, text[:m.start()] + "0x8")
            offset = (target - addr - 8) & 0xFFFFFF
            word = (word & ~(0xFFFFFF << 20)) | (offset << 20)
        else:
            word = assemble_one(args.envyas, text)
        lines.append(f"{word:016x}  # {asm}")
    Path(args.output).write_text("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
