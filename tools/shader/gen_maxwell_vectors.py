#!/usr/bin/env python3
"""Generates Maxwell decoder test vectors with envydis (envytools, MIT).

For every instruction form the decoder supports, random words are built from
the opcode plus random values in the fields the decoder models (FIX entries
replace field values envydis would reject). Each word is disassembled by
envydis at address 8 + 32 * k (alone in its bundle) and written as

    <word hex> <address hex> <envydis text>

to tests/gpu/maxwell_vectors.txt, which tests/gpu/test_maxwell_decoder.cpp
compares against lgp::gpu::maxwell::disassemble(). Words envydis cannot
decode are dropped. Build envydis from https://github.com/envytools/envytools
(cmake; needs flex, bison, libxml2-dev) and pass its path:

    python3 tools/shader/gen_maxwell_vectors.py --envydis path/to/envydis [-n 24] [-o out]

The fixed seed makes the output reproducible for a given envydis version.
"""
import argparse
import random
import re
import subprocess
from pathlib import Path

NOP = 0x50b0000000070f00
SCHED = 0x001f8000fc0007e0


def envydis_single(envydis, words):
    """Each word alone in its bundle at address 32 * k + 8."""
    stream = []
    for w in words:
        stream += [SCHED, w, NOP, NOP]
    txt = " ".join("0x%08x 0x%08x" % (w & 0xffffffff, w >> 32) for w in stream)
    out = subprocess.run([envydis, "-m", "gm107", "-n", "-w"], input=txt, capture_output=True, text=True,
                         check=True).stdout
    res = {}
    for line in out.splitlines():
        m = re.match(r"\s*([0-9a-f]+): [0-9a-f]{8} [0-9a-f]{8}\s+(.*)$", line)
        if m and int(m.group(1), 16) % 32 == 8:
            res[int(m.group(1), 16) // 32] = m.group(2).strip()
    return [res.get(i, "???") for i in range(len(words))]


def bits(*ranges):
    m = 0
    for lo, n in ranges: m |= ((1 << n) - 1) << lo
    return m
G = bits((16, 4))  # guard
R = bits((0,8),(8,8)); B_REG = bits((20,8)); B_C = bits((20,19)); B_I = bits((20,19),(56,1)); B_32 = bits((20,32))
OPS = {
 # name: (value, modeled-bits)
 "fadd_r": (0x5c58<<48, G|R|B_REG|bits((39,2),(44,1),(45,1),(46,1),(48,1),(49,1),(50,1))),
 "fadd_i": (0x3858<<48, G|R|B_I|bits((39,2),(44,1),(45,1),(46,1),(48,1),(49,1),(50,1))),
 "fadd_c": (0x4c58<<48, G|R|B_C|bits((39,2),(44,1),(45,1),(46,1),(48,1),(49,1),(50,1))),
 "fadd32i": (0x0800<<48, G|R|B_32|bits((53,1),(54,1),(55,1),(56,1),(57,1))),
 "fmul_r": (0x5c68<<48, G|R|B_REG|bits((39,2),(41,3),(44,2),(48,1),(50,1))),
 "fmul_i": (0x3868<<48, G|R|B_I|bits((39,2),(41,3),(44,2),(48,1),(50,1))),
 "fmul_c": (0x4c68<<48, G|R|B_C|bits((39,2),(41,3),(44,2),(48,1),(50,1))),
 "fmul32i": (0x1e00<<48, G|R|B_32|bits((53,2),(55,1))),
 "ffma_r": (0x5980<<48, G|R|B_REG|bits((39,8),(48,1),(49,1),(50,1),(51,2),(53,2))),
 "ffma_i": (0x3280<<48, G|R|B_I|bits((39,8),(48,1),(49,1),(50,1),(51,2),(53,2))),
 "ffma_c": (0x4980<<48, G|R|B_C|bits((39,8),(48,1),(49,1),(50,1),(51,2),(53,2))),
 "ffma_rc": (0x5180<<48, G|R|B_C|bits((39,8),(48,1),(49,1),(50,1),(51,2),(53,2))),
 "fmnmx_r": (0x5c60<<48, G|R|B_REG|bits((39,4),(44,1),(45,1),(46,1),(48,1),(49,1))),
 "fmnmx_i": (0x3860<<48, G|R|B_I|bits((39,4),(44,1),(45,1),(46,1),(48,1),(49,1))),
 "fmnmx_c": (0x4c60<<48, G|R|B_C|bits((39,4),(44,1),(45,1),(46,1),(48,1),(49,1))),
 "fsetp_r": (0x5bb0<<48, G|bits((0,6),(6,2),(8,8))|B_REG|bits((39,4),(43,2),(45,2),(47,5))),
 "fsetp_i": (0x36b0<<48, G|bits((0,6),(6,2),(8,8))|B_I|bits((39,4),(43,2),(45,2),(47,5))),
 "fsetp_c": (0x4bb0<<48, G|bits((0,6),(6,2),(8,8))|B_C|bits((39,4),(43,2),(45,2),(47,5))),
 "fset_r": (0x5800<<48, G|R|B_REG|bits((39,4),(43,2),(45,2),(48,5),(53,3))),
 "fset_i": (0x3000<<48, G|R|B_I|bits((39,4),(43,2),(45,2),(48,5),(53,3))),
 "fset_c": (0x4800<<48, G|R|B_C|bits((39,4),(43,2),(45,2),(48,5),(53,3))),
 "mufu": (0x5080<<48, G|R|bits((20,3),(46,1),(48,1),(50,1))),
 "rro_r": (0x5c90<<48, G|bits((0,8))|B_REG|bits((39,1),(45,1),(49,1))),
 "rro_i": (0x3890<<48, G|bits((0,8))|B_I|bits((39,1),(45,1),(49,1))),
 "rro_c": (0x4c90<<48, G|bits((0,8))|B_C|bits((39,1),(45,1),(49,1))),
 "f2f_r": (0x5ca8<<48, G|bits((0,8))|B_REG|bits((45,1),(49,1),(50,1),(44,1))|(2<<8)|(2<<10)),
 "f2f_i": (0x38a8<<48, G|bits((0,8))|B_I|bits((45,1),(49,1),(50,1),(44,1))|(2<<8)|(2<<10)),
 "f2f_c": (0x4ca8<<48, G|bits((0,8))|B_C|bits((45,1),(49,1),(50,1),(44,1))|(2<<8)|(2<<10)),
 "f2i_r": (0x5cb0<<48, G|bits((0,8))|B_REG|bits((39,2),(45,1),(49,1),(44,1),(12,1))|(2<<8)|(2<<10)),
 "f2i_i": (0x38b0<<48, G|bits((0,8))|B_I|bits((39,2),(45,1),(49,1),(44,1),(12,1))|(2<<8)|(2<<10)),
 "i2f_r": (0x5cb8<<48, G|bits((0,8))|B_REG|bits((39,2),(45,1),(49,1),(13,1))|(2<<8)|(2<<10)),
 "i2f_c": (0x4cb8<<48, G|bits((0,8))|B_C|bits((39,2),(45,1),(49,1),(13,1))|(2<<8)|(2<<10)),
 "iadd_r": (0x5c10<<48, G|R|B_REG|bits((43,1),(47,4))),
 "iadd_i": (0x3810<<48, G|R|B_I|bits((43,1),(47,4))),
 "iadd_c": (0x4c10<<48, G|R|B_C|bits((43,1),(47,4))),
 "iadd32i": (0x1c00<<48, G|R|B_32|bits((52,3),(56,1))),
 "iscadd_r": (0x5c18<<48, G|R|B_REG|bits((39,5),(47,3))),
 "iscadd_i": (0x3818<<48, G|R|B_I|bits((39,5),(47,3))),
 "shl_r": (0x5c48<<48, G|R|B_REG|bits((39,1),(43,1),(47,1))),
 "shl_i": (0x3848<<48, G|R|B_I|bits((39,1),(43,1),(47,1))),
 "shr_r": (0x5c28<<48, G|R|B_REG|bits((39,2),(44,1),(47,2))),
 "shr_i": (0x3828<<48, G|R|B_I|bits((39,2),(44,1),(47,2))),
 "lop_r": (0x5c40<<48, G|R|B_REG|bits((39,7),(47,4))),
 "lop_i": (0x3840<<48, G|R|B_I|bits((39,7),(47,4))),
 "lop_c": (0x4c40<<48, G|R|B_C|bits((39,7),(47,4))),
 "lop32i": (0x0400<<48, G|R|B_32|bits((52,6))),
 "isetp_r": (0x5b60<<48, G|bits((0,6),(8,8))|B_REG|bits((39,5),(45,7))),
 "isetp_i": (0x3660<<48, G|bits((0,6),(8,8))|B_I|bits((39,5),(45,7))),
 "isetp_c": (0x4b60<<48, G|bits((0,6),(8,8))|B_C|bits((39,5),(45,7))),
 "imnmx_r": (0x5c20<<48, G|R|B_REG|bits((39,4),(43,2),(47,2))),
 "sel_r": (0x5ca0<<48, G|R|B_REG|bits((39,4))),
 "sel_i": (0x38a0<<48, G|R|B_I|bits((39,4))),
 "sel_c": (0x4ca0<<48, G|R|B_C|bits((39,4))),
 "mov_r": (0x5c98<<48, G|bits((0,8))|B_REG|bits((39,4))),
 "mov_i": (0x3898<<48, G|bits((0,8))|B_I|bits((39,4))),
 "mov_c": (0x4c98<<48, G|bits((0,8))|B_C|bits((39,4))),
 "mov32i": (0x0100<<48, G|bits((0,8),(12,4))|B_32),
 "psetp": (0x5090<<48, G|bits((0,6),(12,4),(24,2),(29,4),(39,4),(45,2))),
 "ald": (0xefd8<<48, G|R|bits((20,10),(32,1),(39,8),(47,2))),
 "ast": (0xeff0<<48, G|R|bits((20,10),(39,8),(47,2))),
 "ipa": (0xe000<<48, G|R|bits((20,8),(28,10),(39,8),(47,4),(51,5))),
 "ipa_idx": ((0xe000<<48)|(1<<38), G|R|bits((20,8),(39,8),(42,1),(51,5))),
 "ldc": (0xef90<<48, G|R|bits((20,16),(36,5),(44,2))|(4<<48)),
 "texs": (0xd000<<48, G|R|bits((20,8),(28,8),(36,13),(49,1),(50,3),(53,4))),
 "tex": (0xc038<<48, G|R|bits((20,8),(28,3),(31,4),(36,13),(49,2),(54,4))),
 "exit": (0xe300<<48, G|bits((0,6))),
 "bra": (0xe240<<48, G|bits((0,5),(6,2),(20,24))),
 "ssy": (0xe290<<48, bits((20,24))),
 "pbk": (0xe2a0<<48, bits((20,24))),
 "sync": (0xf0f8<<48, G|bits((0,5))),
 "brk": (0xe340<<48, G|bits((0,5))),
 "kil": (0xe330<<48, G|bits((0,5))),
 "nop": (0x50b0<<48, G|bits((8,6),(20,16))),
 "depbar": (0xf0f0<<48, G|bits((0,6),(20,10))),
 # P4.2 additions (real game shaders)
 "iset_r": (0x5b50<<48, G|R|B_REG|bits((39,4),(43,2),(45,2),(47,5))),
 "iset_i": (0x3650<<48, G|R|B_I|bits((39,4),(43,2),(45,2),(47,5))),
 "iset_c": (0x4b50<<48, G|R|B_C|bits((39,4),(43,2),(45,2),(47,5))),
 "iadd3_r": (0x5cc0<<48, G|R|B_REG|bits((31,8),(39,8),(47,5))),
 "iadd3_i": (0x38c0<<48, G|R|B_I|bits((39,8),(47,5))),
 "iadd3_c": (0x4cc0<<48, G|R|B_C|bits((39,8),(47,5))),
 "i2i_r": (0x5ce0<<48, G|bits((0,8),(8,6))|B_REG|bits((41,2),(45,1),(47,1),(49,2))),
 "i2i_i": (0x38e0<<48, G|bits((0,8),(8,6))|B_I|bits((41,2),(45,1),(47,1),(49,2))),
 "i2i_c": (0x4ce0<<48, G|bits((0,8),(8,6))|B_C|bits((41,2),(45,1),(47,1),(49,2))),
 "r2p_r": (0x5cf0<<48, G|bits((8,8))|B_REG|bits((40,3))),
 "r2p_i": (0x38f0<<48, G|bits((8,8))|B_I|bits((40,3))),
 "r2p_c": (0x4cf0<<48, G|bits((8,8))|B_C|bits((40,3))),
 "xmad_r": (0x5b00<<48, G|R|B_REG|bits((35,4),(39,8),(47,7))),
 "xmad_i": (0x3600<<48, G|R|bits((20,16),(36,3),(39,8),(47,7))),
 "xmad_c": (0x4e00<<48, G|R|B_C|bits((39,8),(47,10))),
 "xmad_rc": (0x5100<<48, G|R|B_C|bits((39,8),(47,8))),
 "bfe_r": (0x5c00<<48, G|R|B_REG|bits((40,1),(47,2))),
 "bfe_i": (0x3800<<48, G|R|B_I|bits((40,1),(47,2))),
 "bfe_c": (0x4c00<<48, G|R|B_C|bits((40,1),(47,2))),
 "csetp": (0x50a0<<48, G|bits((0,6),(8,5),(39,4),(45,2))),
 "pset": (0x5088<<48, G|bits((0,8),(12,4),(24,2),(29,4),(39,4),(44,4))),
 # P4.3 additions (texture queries/fetches, compute, address arithmetic)
 "tlds": ((0xda00<<48), G|R|bits((20,8),(28,8),(36,13),(49,1))),
 "txq": (0xdf48<<48, G|R|bits((31,4),(36,13),(49,1))),
 "tmml": (0xdf58<<48, G|R|bits((28,3),(31,4),(35,1),(36,13),(49,1))),
 "s2r": (0xf0c8<<48, G|bits((0,8))),
 "ldg": (0xeed0<<48, G|R|bits((20,24),(45,3))),
 "stg": (0xeed8<<48, G|R|bits((20,24),(45,3))),
 "lds": (0xef48<<48, G|R|bits((20,24),(44,1))),
 "sts": (0xef58<<48, G|R|bits((20,24))),
 "ldl": (0xef40<<48, G|R|bits((20,24),(44,2))),
 "stl": (0xef50<<48, G|R|bits((20,24),(44,2))),
 "atom": (0xed00<<48, G|R|bits((20,8),(28,20),(48,1))),
 "red": (0xebf8<<48, G|R|bits((28,20),(48,1))),
 "atoms": (0xec00<<48, G|R|bits((20,8),(30,22))),
 "bar": ((0xf0a8<<48)|(0x80<<32)|(7<<39), G|bits((20,8))),
 "membar": (0xef98<<48, G|bits((0,2),(8,2))),
 "lea_r": (0x5bd0<<48, G|R|B_REG|bits((39,5),(45,6))),
 "lea_c": (0x4bd0<<48, G|R|B_C|bits((39,5),(45,6))),
 "lea_i": (0x36d0<<48, G|R|B_I|bits((39,5),(45,6))),
 "leahi_r": (0x5bd8<<48, G|R|B_REG|bits((28,5),(37,2),(39,8),(47,4))),
 "leahi_c": (0x1800<<48, G|R|B_C|bits((39,8),(47,4),(51,5),(56,2))),
 "shfl_r": (0x5bf8<<48, G|R|B_REG|bits((37,2),(39,8),(47,4))),
 "shfl_i": (0x36f8<<48, G|R|B_I|bits((37,2),(39,8),(47,4))),
 "shfr_r": (0x5cf8<<48, G|R|B_REG|bits((37,2),(39,8),(47,4))),
 "shfr_i": (0x38f8<<48, G|R|bits((20,6),(37,2),(39,8),(47,4))),
 # texture gather (TLD4): component 56..57, offset mode 54..55, dc 50
 "tld4": (0xc838<<48, G|R|bits((20,8),(28,3),(31,4),(35,1),(36,13),(49,2),(54,4))),}
def setf(w, lo, n, v): return (w & ~(((1 << n) - 1) << lo)) | (v << lo)
def getf(w, lo, n): return (w >> lo) & ((1 << n) - 1)
def no3(lo):
    return lambda w, r: setf(w, lo, 2, r.randrange(3)) if getf(w, lo, 2) == 3 else w
def no1(lo):
    return lambda w, r: setf(w, lo, 2, r.choice([0, 2, 3])) if getf(w, lo, 2) == 1 else w
def chain(*fs):
    def f(w, r):
        for g in fs: w = g(w, r)
        return w
    return f
def fsize(lo): return lambda w, r: setf(w, lo, 2, r.randrange(1, 4) if lo != 8 or True else 0)
def f2f_fix(w, r):
    w = setf(w, 8, 2, 2); w = setf(w, 10, 2, 2)
    m = r.choice([0, 3, 4, 5, 6, 7]); w = setf(w, 39, 2, m & 3); w = setf(w, 42, 1, m >> 2); return w
FIX = {
 "fmul_r": chain(no3(44), lambda w, r: setf(w, 41, 3, r.randrange(7))),
 "fmul32i": no3(53),
 "ffma": no3(53), "fsetp": no3(45), "fset": no3(45), "psetp": chain(no3(24), no3(45)),
 "f2f_r": f2f_fix,
 "f2f_i": f2f_fix,
 "f2f_c": f2f_fix,
 "f2i": lambda w, r: setf(w, 10, 2, r.randrange(1, 4)),
 "i2f": lambda w, r: setf(w, 8, 2, r.randrange(1, 4)),
 "isetp": chain(no3(45), lambda w, r: setf(w, 47, 1, 0)),
 "ipa": chain(lambda w, r: setf(w, 8, 8, 0xff), no3(52)),
 "ipa_idx": no3(52),
 "texs": chain(lambda w, r: setf(w, 59, 1, 1), lambda w, r: setf(w, 53, 4, r.randrange(14)),
               lambda w, r: setf(w, 50, 3, r.randrange(8) if getf(w, 28, 8) == 0xff else r.randrange(5))),
 "tex": lambda w, r: setf(w, 55, 3, r.choice([0, 1, 2, 3, 6, 7])),
 "iset": no3(45),
 "iadd3_r": chain(no3(31), no3(33), no3(35), no3(37)),
 "i2i": chain(no3(8), no3(10)),
 "xmad_r": lambda w, r: setf(w, 50, 3, r.randrange(5)) if getf(w, 50, 3) > 4 else w,
 "csetp": no3(45),
 "pset": chain(no3(24), no3(45)),
 "tlds": chain(lambda w, r: setf(w, 53, 4, r.choice([0, 1, 2, 4, 5, 6, 7, 8, 12])),
               lambda w, r: setf(w, 50, 3, r.randrange(8) if getf(w, 28, 8) == 0xff else r.randrange(5))),
 "txq": lambda w, r: setf(w, 22, 6, r.choice([1, 2, 5, 0x10, 0x12, 0x14, 0x16])),
 "s2r": lambda w, r: setf(w, 20, 8, r.choice([0, 0x20, 0x21, 0x22, 0x23, 0x25, 0x26, 0x27, 0x28, 0x32])),
 "ldg": lambda w, r: setf(w, 48, 3, r.randrange(7)),
 "stg": lambda w, r: setf(w, 48, 3, r.randrange(7)),
 "lds": lambda w, r: setf(w, 48, 3, r.randrange(7)),
 "sts": lambda w, r: setf(w, 48, 3, r.randrange(7)),
 "ldl": lambda w, r: setf(w, 48, 3, r.randrange(7)),
 "stl": lambda w, r: setf(w, 48, 3, r.randrange(7)),
 "atom": chain(lambda w, r: setf(w, 52, 4, r.choice([0, 1, 2, 3, 4, 5, 6, 7, 8, 10])),
               lambda w, r: setf(w, 49, 3, r.randrange(6))),
 "red": chain(lambda w, r: setf(w, 23, 3, r.randrange(8)), lambda w, r: setf(w, 20, 3, r.randrange(6))),
 "atoms": chain(lambda w, r: setf(w, 52, 4, r.randrange(9)), lambda w, r: setf(w, 28, 2, r.randrange(4))),
 "bar": lambda w, r: setf(w, 8, 8, 0xff),
 "shfl": no1(37), "shfr": no1(37),
 "tld4": no3(54),}
FIX["fmul_i"] = FIX["fmul_c"] = FIX["fmul_r"]
FIX["xmad_i"] = FIX["xmad_r"]
for k in list(OPS):
    base = k.split("_")[0]
    if k not in FIX and base in FIX: FIX[k] = FIX[base]
def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--envydis", required=True)
    ap.add_argument("-n", type=int, default=24, help="words per instruction form")
    ap.add_argument("-o", default=str(Path(__file__).resolve().parents[2] / "tests/gpu/maxwell_vectors.txt"))
    args = ap.parse_args()
    rng = random.Random(1234)
    lines = []
    for name, (value, mask) in OPS.items():
        fix = FIX.get(name, lambda w, r: w)
        words = [fix(value | (rng.getrandbits(64) & mask), rng) for _ in range(args.n)]
        for k, (w, text) in enumerate(zip(words, envydis_single(args.envydis, words))):
            if "???" not in text:
                lines.append("%016x %x %s" % (w, 8 + 32 * k, text))
    with open(args.o, "w", newline="\n") as f:
        f.write("# Generated by tools/shader/gen_maxwell_vectors.py from envydis (envytools, MIT).\n")
        f.write("# <word> <address> <envydis disassembly>\n")
        f.write("\n".join(lines) + "\n")
    print(f"{len(lines)} vectors -> {args.o}")


if __name__ == "__main__":
    main()
