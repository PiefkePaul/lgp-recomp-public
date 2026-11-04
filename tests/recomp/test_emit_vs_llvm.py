#!/usr/bin/env python3
"""Cross-checks the native rewrite's A64 encoder (a64_emit.hpp) against LLVM's assembler.

`<test> --dump-forms` prints one line per encoder form: the word and its
assembly text, with branch targets as offsets from the instruction (`#off`).
Every line is assembled at its own address with llvm-mc or clang and must give
the same word. Our own instructions only, no game code.

usage: test_emit_vs_llvm.py <test_native_image> <llvm-mc|clang>
"""
import os
import struct
import subprocess
import sys
import tempfile


def text_section(obj):
    """Bytes of .text in a little-endian ELF64 object."""
    with open(obj, "rb") as f:
        data = f.read()
    if data[:4] != b"\x7fELF" or data[4] != 2:
        raise SystemExit("not an ELF64 object: " + obj)
    shoff, = struct.unpack_from("<Q", data, 0x28)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", data, 0x3A)
    sections = [struct.unpack_from("<IIQQQQIIQQ", data, shoff + k * shentsize) for k in range(shnum)]
    names = sections[shstrndx]
    for s in sections:
        start = names[4] + s[0]
        name = data[start:data.index(b"\0", start)]
        if name == b".text":
            return data[s[4]:s[4] + s[5]]
    raise SystemExit("no .text in " + obj)


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    test, assembler = sys.argv[1], sys.argv[2]
    lines = subprocess.run([test, "--dump-forms"], capture_output=True, text=True, check=True).stdout.splitlines()
    forms = [(int(w, 16), t) for w, t in (line.split("\t", 1) for line in lines if line)]
    with tempfile.TemporaryDirectory() as td:
        src, obj = os.path.join(td, "forms.s"), os.path.join(td, "forms.o")
        with open(src, "w") as f:
            f.write(".text\n" + "\n".join(t for _, t in forms) + "\n")
        if "clang" in os.path.basename(assembler).lower():
            cmd = [assembler, "--target=aarch64-none-elf", "-c", src, "-o", obj]
        else:
            cmd = [assembler, "-triple=aarch64", "-filetype=obj", src, "-o", obj]
        subprocess.run(cmd, check=True)
        text = text_section(obj)
    if len(text) != 4 * len(forms):
        raise SystemExit("assembled %d bytes for %d forms" % (len(text), len(forms)))
    bad = 0
    for k, (word, asm) in enumerate(forms):
        got, = struct.unpack_from("<I", text, 4 * k)
        if got != word:
            bad += 1
            print("MISMATCH %-40s ours %08x llvm %08x" % (asm, word, got))
    print("%d encoder forms, %d mismatches" % (len(forms), bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
