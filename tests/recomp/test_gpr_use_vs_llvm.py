#!/usr/bin/env python3
"""Cross-checks gpr_use() (native census) against LLVM's disassembler.

Random instruction words that our decoder accepts are disassembled with
llvm-mc + llvm-objdump; the general-purpose registers the text names (x/w
numbers, sp/wsp) must equal the registers gpr_use() reports as read or
written. Implicit operands the text does not show (x30 of BL/BLR/RET) are
left out on both sides. Our own random words only, no game code.

usage: test_gpr_use_vs_llvm.py <gpr_use_dump> <llvm-mc> <llvm-objdump> [--words N] [--seed S]
"""
import argparse
import os
import random
import re
import subprocess
import sys
import tempfile

REG = re.compile(r"\b[xw](\d+)\b|\b(w?sp)\b")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dump")
    ap.add_argument("llvm_mc")
    ap.add_argument("llvm_objdump")
    ap.add_argument("--words", type=int, default=200000)
    ap.add_argument("--seed", type=int, default=56)
    a = ap.parse_args()

    rng = random.Random(a.seed)
    words = [rng.getrandbits(32) for _ in range(a.words)]
    out = subprocess.run([a.dump], input="\n".join("%08x" % w for w in words), capture_output=True, text=True,
                         check=True).stdout.split()
    ours = {}
    for k in range(0, len(out), 4):
        word, valid, reads, writes = out[k:k + 4]
        if valid == "1":
            ours[int(word, 16)] = int(reads, 16) | int(writes, 16)
    valid_words = list(ours)

    with tempfile.TemporaryDirectory() as td:
        src, obj = os.path.join(td, "w.s"), os.path.join(td, "w.o")
        with open(src, "w") as f:
            f.write(".text\n" + "\n".join(".inst 0x%08x" % w for w in valid_words) + "\n")
        subprocess.run([a.llvm_mc, "-triple=aarch64", "-filetype=obj", src, "-o", obj], check=True)
        dis = subprocess.run([a.llvm_objdump, "-d", "--no-show-raw-insn", "--mattr=+v8a,+crc,+aes,+sha2", obj],
                             capture_output=True, text=True, check=True).stdout
    by_addr = {}
    for line in dis.splitlines():
        m = re.match(r"\s*([0-9a-f]+):\s+(.*)$", line)
        if m:
            by_addr[int(m.group(1), 16)] = m.group(2).split("//")[0].strip()

    bad = 0
    for k, w in enumerate(valid_words):
        text = by_addr.get(4 * k, "<none>")
        named = set()
        for num, sp in REG.findall(text):
            named.add(31 if sp else int(num))
        mine = {r for r in range(32) if ours[w] >> r & 1}
        if text.split()[0] in ("bl", "blr", "ret"):
            named.discard(30)
            mine.discard(30)
        if named != mine:
            bad += 1
            if bad <= 20:
                print("%08x %-44s llvm=%s ours=%s" % (w, text, sorted(named), sorted(mine)))
    print("gpr_use vs llvm: %d valid words checked, %d mismatches" % (len(valid_words), bad))
    return 1 if bad or len(valid_words) < a.words // 10 else 0


if __name__ == "__main__":
    sys.exit(main())
