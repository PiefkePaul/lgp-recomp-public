#!/usr/bin/env python3
"""Shader and texture inventory of a Switch RomFS, optionally matched against Ryujinx's
guest shader cache (which Maxwell shaders did the game really hand to the GPU, and where
do they come from?).

What it scans:
* NX binary containers by magic: ``BNSH`` (NVN shader container) and ``BNTX`` (texture
  container). Their total size sits at +0x1C of the common NX header.
* Loose files (``.bnsh*``, ``.ptcl`` NintendoWare Vfx, ``.arc`` SARC, ``.bntx``, ``.gfbmdl``)
  and the LZ4-compressed entries of Game Freak ``GFLXPACK`` archives (``.gfpak``).
* Shader stages inside BNSH containers, found by the Maxwell shader program header (SPH)
  word 0: SPH type 1 (VTG) / 2 (PS), version 3, shader type 1..5. Each stage is counted once
  by a hash of its first 0xC0 bytes (approximation: two stages with the same SPH and first
  instructions merge, stages shorter than 0xC0 include trailing bytes).
* Texture formats from the ``BRTI`` blocks inside BNTX (format word at +0x1C).

Matching: every guest shader from the Ryujinx cache is looked up byte-exact (SPH included)
in all sources above plus the decompressed NSO images. Compute shaders have no SPH; they are
searched with a plain substring search and are not part of the stage count.

Output: counts per source type and per format, no shader code or asset bytes.

Usage:
    python tools/analyze/shader_inventory.py <romfs_dir> [--exefs <exefs_dir>]
        [--ryujinx-shader-cache <games/<tid>/cache/shader>] [--json out.json] [--markdown out.md]
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import struct
import sys
from collections import Counter, defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nso_inventory as ni  # noqa: E402
import ryujinx_cache as rc  # noqa: E402

SHADER_STAGES = {1: "vertex", 2: "tess_control", 3: "tess_eval", 4: "geometry", 5: "pixel"}
# SPH word 0, byte 0: type 1/2 with version 3 (bits 5..9); byte 1: version bits 3..4 = 0,
# shader type 1..5 in bits 2..5, any value in bits 6..7.
SPH_RE = re.compile(rb"[\x61\x62][" + b"".join(
    re.escape(bytes([(t << 2) | (hi << 6)])) for t in SHADER_STAGES for hi in range(4)) + rb"]")
SCANNED_EXTS = {".bnsh", ".bnsh_vsh", ".bnsh_fsh", ".ptcl", ".arc", ".bntx", ".gfbmdl", ".gfpak"}
STAGE_KEY_LEN = 0xC0
GUEST_KEY_LEN = 0x60

# BNTX surface formats (high byte: channel layout/compression, low byte: type), names as in
# the public NVN/NX texture tooling.
TEXTURE_FORMATS = {
    0x0201: "R8_UNORM", 0x0501: "R5G5B5A1_UNORM", 0x0701: "R5G6B5_UNORM", 0x0901: "R8G8_UNORM",
    0x0A01: "R16_UNORM", 0x0B01: "R8G8B8A8_UNORM", 0x0B06: "R8G8B8A8_SRGB",
    0x1A01: "BC1_UNORM", 0x1A06: "BC1_SRGB", 0x1B01: "BC2_UNORM", 0x1B06: "BC2_SRGB",
    0x1C01: "BC3_UNORM", 0x1C06: "BC3_SRGB", 0x1D01: "BC4_UNORM", 0x1D02: "BC4_SNORM",
    0x1E01: "BC5_UNORM", 0x1E02: "BC5_SNORM", 0x1F05: "BC6H_SF16", 0x1F0A: "BC6H_UF16",
    0x2001: "BC7_UNORM", 0x2006: "BC7_SRGB", 0x2D01: "ASTC_4x4_UNORM", 0x2D06: "ASTC_4x4_SRGB",
}


def lz4_block(src: bytes, size: int) -> bytes:
    try:
        import lz4.block  # type: ignore
        return lz4.block.decompress(src, uncompressed_size=size)
    except ImportError:
        return ni.lz4_block_decompress(src, size)


def gfpak_entries(data: bytes):
    """Yield (compression, payload or None) for each file of a GFLXPACK archive.

    Header: magic "GFLXPACK", u32 version, u32 relocated, u32 file count, u32 folder count,
    u64 file table, u64 hash table, u64 folder table. File entry (0x18 bytes):
    u16 level, u16 compression (0 none, 2 LZ4 block), u32 size, u32 compressed size, u32 pad,
    u64 offset. Other compression types are yielded with payload None.
    """
    if data[:8] != b"GFLXPACK":
        raise ni.FormatError("not a GFLXPACK archive")
    _ver, _rel, count, _folders, table = struct.unpack_from("<IIIIQ", data, 8)
    for i in range(count):
        _lvl, comp, size, csize, _pad, off = struct.unpack_from("<HHIIIQ", data, table + i * 0x18)
        raw = data[off:off + csize]
        if comp == 2:
            yield comp, lz4_block(raw, size)
        elif comp == 0:
            yield comp, raw[:size]
        else:
            yield comp, None


def nx_containers(data: bytes, magic: bytes):
    """Yield (offset, blob) of NX binary containers with the given 4-byte magic."""
    for m in re.finditer(re.escape(magic) + b"\0\0\0\0", data):
        s = m.start()
        if s + 0x20 > len(data):
            continue
        size = struct.unpack_from("<I", data, s + 0x1C)[0]
        if 0x20 <= size <= len(data) - s:
            yield s, data[s:s + size]


def sph_stage(data: bytes, pos: int) -> str | None:
    w = struct.unpack_from("<I", data, pos)[0]
    if (w & 0x3FF) in (0x61, 0x62):
        return SHADER_STAGES.get((w >> 10) & 15)
    return None


def stage_offsets(container: bytes):
    """Yield (offset, stage) of plausible shader program headers in a BNSH container."""
    for m in SPH_RE.finditer(container):
        p = m.start()
        if p % 0x10 == 0 and p + 0x50 <= len(container):
            st = sph_stage(container, p)
            if st:
                yield p, st


def texture_formats(bntx: bytes):
    for m in re.finditer(rb"BRTI", bntx):
        o = m.start()
        if o + 0x2C <= len(bntx):
            yield struct.unpack_from("<I", bntx, o + 0x1C)[0]


def iter_sources(romfs: str, images: dict[str, bytes]):
    """Yield (source type, relative path, bytes); images are searched first."""
    for name, data in images.items():
        yield "exefs:" + name, name, data
    for dirpath, _dirs, files in os.walk(romfs):
        for fn in sorted(files):
            ext = os.path.splitext(fn)[1].lower()
            if ext not in SCANNED_EXTS:
                continue
            path = os.path.join(dirpath, fn)
            rel = os.path.relpath(path, romfs).replace("\\", "/")
            with open(path, "rb") as f:
                data = f.read()
            if ext == ".gfpak":
                for comp, payload in gfpak_entries(data):
                    if payload is not None:
                        yield "gfpak", rel, payload
            else:
                yield ext.lstrip("."), rel, data


def analyze(romfs: str, images: dict[str, bytes], guest: list | None = None) -> dict:
    containers: dict[str, Counter] = defaultdict(Counter)
    seen_bnsh, seen_bntx, stage_keys = set(), set(), set()
    stages: Counter = Counter()
    stages_by_source: dict[str, set] = defaultdict(set)
    formats: Counter = Counter()
    guest_index: dict[bytes, list[int]] = defaultdict(list)
    no_sph: list = []  # compute shaders carry no SPH; searched with a plain find
    for g in guest or []:
        if SPH_RE.match(g.code):
            guest_index[g.code[:GUEST_KEY_LEN]].append(g.index)
        else:
            no_sph.append(g)
    guest_found: dict[int, str] = {}
    guest_by_index = {g.index: g for g in guest or []}
    for kind, rel, data in iter_sources(romfs, images):
        for _, blob in nx_containers(data, b"BNSH"):
            containers[kind]["bnsh"] += 1
            h = hashlib.sha1(blob).digest()
            if h in seen_bnsh:
                continue
            seen_bnsh.add(h)
            containers[kind]["bnsh_unique"] += 1
            for p, st in stage_offsets(blob):
                key = hashlib.sha1(blob[p:p + STAGE_KEY_LEN]).digest()
                stages_by_source[kind].add(key)
                if key not in stage_keys:
                    stage_keys.add(key)
                    stages[st] += 1
        for _, blob in nx_containers(data, b"BNTX"):
            containers[kind]["bntx"] += 1
            h = hashlib.sha1(blob).digest()
            if h in seen_bntx:
                continue
            seen_bntx.add(h)
            containers[kind]["bntx_unique"] += 1
            for fmt in texture_formats(blob):
                formats[fmt] += 1
        for g in no_sph:
            if g.index not in guest_found and g.code in data:
                guest_found[g.index] = kind
        if guest_index and len(guest_found) < len(guest_by_index):
            for m in SPH_RE.finditer(data):
                p = m.start()
                for gi in guest_index.get(data[p:p + GUEST_KEY_LEN], ()):
                    code = guest_by_index[gi].code
                    if gi not in guest_found and data[p:p + len(code)] == code:
                        guest_found[gi] = kind
    rep: dict = {
        "containers": {k: dict(v) for k, v in containers.items()},
        "bnsh_unique_total": len(seen_bnsh),
        "bntx_unique_total": len(seen_bntx),
        "stages_unique_total": len(stage_keys),
        "stages_by_kind": dict(stages),
        "stages_by_source": {k: len(v) for k, v in stages_by_source.items()},
        "texture_formats": {TEXTURE_FORMATS.get(f, hex(f)): c for f, c in formats.most_common()},
        "textures_total": sum(formats.values()),
    }
    if guest is not None:
        rep["guest_shaders"] = len(guest)
        rep["guest_compute_shaders"] = len(no_sph)
        rep["guest_found_by_source"] = dict(Counter(guest_found.values()))
        rep["guest_not_found"] = len(guest) - len(guest_found)
        rep["guest_not_found_sizes"] = sorted(len(g.code) for g in guest if g.index not in guest_found)
    return rep


def to_markdown(rep: dict) -> str:
    out = ["# Shader/texture inventory", "",
           f"- BNSH unique: {rep['bnsh_unique_total']}, shader stages unique (approx.): "
           f"{rep['stages_unique_total']} {rep['stages_by_kind']}",
           f"- BNTX unique: {rep['bntx_unique_total']}, textures: {rep['textures_total']}", "",
           "| source | BNSH | BNSH unique | stages | BNTX | BNTX unique |", "|---|---:|---:|---:|---:|---:|"]
    for k, v in sorted(rep["containers"].items()):
        out.append(f"| {k} | {v.get('bnsh', 0)} | {v.get('bnsh_unique', 0)} | {rep['stages_by_source'].get(k, 0)} "
                   f"| {v.get('bntx', 0)} | {v.get('bntx_unique', 0)} |")
    out += ["", "| texture format | count |", "|---|---:|"]
    out += [f"| {f} | {c} |" for f, c in rep["texture_formats"].items()]
    if "guest_shaders" in rep:
        out += ["", f"Ryujinx guest shaders: {rep['guest_shaders']}, found: {rep['guest_found_by_source']}, "
                    f"not found: {rep['guest_not_found']}"]
    return "\n".join(out) + "\n"


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=(__doc__ or "").split("\n")[0])
    ap.add_argument("romfs")
    ap.add_argument("--exefs", help="ExeFS directory; decompressed NSO images are searched too")
    ap.add_argument("--ryujinx-shader-cache", help="directory with guest.toc/guest.data")
    ap.add_argument("--json")
    ap.add_argument("--markdown")
    a = ap.parse_args(argv)
    guest = None
    if a.ryujinx_shader_cache:
        d = a.ryujinx_shader_cache
        guest = rc.read_guest_shader_cache(os.path.join(d, "guest.toc"), os.path.join(d, "guest.data"))
    images = {n: m.image for n, m in ni.load_exefs(a.exefs).items()} if a.exefs else {}
    rep = analyze(a.romfs, images, guest)
    if a.json:
        with open(a.json, "w", encoding="utf-8") as f:
            json.dump(rep, f, indent=1)
    md = to_markdown(rep)
    if a.markdown:
        with open(a.markdown, "w", encoding="utf-8") as f:
            f.write(md)
    else:
        sys.stdout.write(md)
    return 0


if __name__ == "__main__":
    sys.exit(main())
