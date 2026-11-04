#!/usr/bin/env python3
"""Cross-checks rename_gpr() (native rewrite, x18 -> xN) against LLVM's disassembler.

Random instruction words get x18 in one or more of the operand fields
(bits 0, 5, 10, 16) and a random target register x0..x17. Where rename_gpr()
returns a word, LLVM must disassemble it to the original text with every
x18/w18 replaced by xN/wN, and nothing else changed. Words whose text names
x18 but which rename_gpr() refuses are counted, not failed (a stub handles
them). Our own random words only, no game code.

usage: test_rename_vs_llvm.py <gpr_use_dump> <llvm-mc> <llvm-objdump> [--words N] [--seed S]
"""
import argparse
import os
import random
import re
import subprocess
import sys
import tempfile

MATTR = "--mattr=+v8a,+crc,+aes,+sha2"


def disassemble(llvm_mc, llvm_objdump, words, td, name):
    src, obj = os.path.join(td, name + ".s"), os.path.join(td, name + ".o")
    with open(src, "w") as f:
        f.write(".text\n" + "\n".join(".inst 0x%08x" % w for w in words) + "\n")
    subprocess.run([llvm_mc, "-triple=aarch64", "-filetype=obj", src, "-o", obj], check=True)
    dis = subprocess.run([llvm_objdump, "-d", "--no-show-raw-insn", MATTR, obj], capture_output=True, text=True,
                         check=True).stdout
    by_addr = {}
    for line in dis.splitlines():
        m = re.match(r"\s*([0-9a-f]+):\s+(.*)$", line)
        if m:
            by_addr[int(m.group(1), 16)] = m.group(2).split("//")[0].strip()
    return [by_addr.get(4 * k, "<none>") for k in range(len(words))]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dump")
    ap.add_argument("llvm_mc")
    ap.add_argument("llvm_objdump")
    ap.add_argument("--words", type=int, default=200000)
    ap.add_argument("--seed", type=int, default=5618)
    a = ap.parse_args()

    rng = random.Random(a.seed)
    pairs = []
    for _ in range(a.words):
        w = rng.getrandbits(32)
        for s in (0, 5, 10, 16):
            if rng.random() < 0.45:
                w = (w & ~(31 << s)) | (18 << s)
        pairs.append((w, rng.randrange(18)))
    out = subprocess.run([a.dump, "--rename"], input="\n".join("%08x %d" % p for p in pairs), capture_output=True,
                         text=True, check=True).stdout.split()
    results = [(int(out[k], 16), out[k + 1] == "1", int(out[k + 2], 16)) for k in range(0, len(out), 3)]

    with tempfile.TemporaryDirectory() as td:
        old_text = disassemble(a.llvm_mc, a.llvm_objdump, [w for w, _ in pairs], td, "old")
        renamed = [(k, r[2]) for k, r in enumerate(results) if r[1]]
        # Same positions as the old words, so PC-relative targets print alike.
        new_words = [r[2] if r[1] else w for (w, _), r in zip(pairs, results)]
        new_all = disassemble(a.llvm_mc, a.llvm_objdump, new_words, td, "new")

    bad = 0
    for k, nw in renamed:
        text = new_all[k]
        to = pairs[k][1]
        want = re.sub(r"\b([xw])18\b", lambda m: "%s%d" % (m.group(1), to), old_text[k])
        if text != want:
            bad += 1
            if bad <= 20:
                print("%08x -> %08x (x%d): llvm %r, expected %r" % (pairs[k][0], nw, to, text, want))
    names18 = [k for k, t in enumerate(old_text) if re.search(r"\b[xw]18\b", t)
               and not re.search(r"\b[xw]%d\b" % pairs[k][1], t)]
    refused = [k for k in names18 if not results[k][1]]
    for k in refused[:10]:
        print("refused: %08x %s (x%d)" % (pairs[k][0], old_text[k], pairs[k][1]))
    print("rename vs llvm: %d renamed, %d mismatches; %d words name x18, %d refused" %
          (len(renamed), bad, len(names18), len(refused)))
    return 1 if bad or len(renamed) < a.words // 20 else 0


if __name__ == "__main__":
    sys.exit(main())
