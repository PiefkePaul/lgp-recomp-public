#!/usr/bin/env python3
"""Readers for Ryujinx (Ryubing 1.3.x) per-game caches, used as runtime evidence.

Two caches are useful for static analysis of a game without instrumenting the emulator:

* PTC profile (``games/<tid>/cache/cpu/0/<ver>-default.info``): the set of guest
  function entry addresses that ARMeilleure translated, i.e. code that actually ran
  in any profiled session. Layout (ARMeilleure ``PtcProfiler``):
    OuterHeader (29 bytes, not compressed): u64 magic "Pohd\\0\\0\\0\\0", u32 version,
        u8 endianness, Hash128 header hash
    raw deflate stream: Hash128, i32 count, count x {u64 address, i32 mode, u8 highCq, u8 blacklist}
  Versions before the blacklist field (5-byte entries) are not supported here.
* Guest shader cache (``games/<tid>/cache/shader/guest.toc`` + ``guest.data``): every
  Maxwell shader stage the game handed to the GPU, as raw guest code (including the
  0x50-byte shader program header). Layout (``DiskCacheGuestStorage``):
    toc: header {u32 magic "TOCG", u32 version, u32 pad, u32 modifications, u64, u64},
         entries {u32 offset, u32 code_size, u32 cb1_size, u32 hash}
    data at offset: cb1 bytes, u8 compression (0 none, 1 deflate, 2 brotli), compressed code

Module load addresses come from the Ryujinx log ("Loader Load: Loading <name>..." and
"LoadNsos: Loading image <n> at 0x...").

All outputs derived from these caches stay in $LGP_PRIVATE_DIR; only counts and
public API names go into docs/analysis/.
"""
from __future__ import annotations

import re
import struct
import zlib
from dataclasses import dataclass

PTC_MAGIC = b"Pohd\0\0\0\0"
PTC_OUTER_HEADER_SIZE = 29
PTC_ENTRY = struct.Struct("<QiBB")
TOC_MAGIC = 0x47434F54  # "TOCG"


class CacheFormatError(Exception):
    pass


def read_ptc_profile(path: str) -> tuple[int, dict[int, tuple[int, bool, bool]]]:
    """Return (version, {guest_address: (mode, high_cq, blacklist)})."""
    with open(path, "rb") as f:
        raw = f.read()
    return parse_ptc_profile(raw)


def parse_ptc_profile(raw: bytes) -> tuple[int, dict[int, tuple[int, bool, bool]]]:
    if raw[:8] != PTC_MAGIC or len(raw) < PTC_OUTER_HEADER_SIZE:
        raise CacheFormatError("not a PTC profile (magic 'Pohd' missing)")
    version = struct.unpack_from("<I", raw, 8)[0]
    try:
        body = zlib.decompress(raw[PTC_OUTER_HEADER_SIZE:], -15)
    except zlib.error as e:
        raise CacheFormatError(f"PTC profile: deflate failed ({e})") from e
    if len(body) < 20:
        raise CacheFormatError("PTC profile: body too short")
    count = struct.unpack_from("<i", body, 16)[0]
    if count < 0 or 20 + count * PTC_ENTRY.size != len(body):
        raise CacheFormatError(f"PTC profile: {count} entries do not fit {len(body)} bytes "
                               "(older version without blacklist field?)")
    funcs = {}
    for addr, mode, high_cq, blacklist in PTC_ENTRY.iter_unpack(body[20:]):
        funcs[addr] = (mode, bool(high_cq), bool(blacklist))
    return version, funcs


@dataclass
class GuestShader:
    index: int
    code: bytes
    cb1_size: int
    hash: int


def _decompress(alg: int, blob: bytes, size: int) -> bytes:
    if alg == 0:
        return blob[:size]
    if alg == 1:
        return zlib.decompressobj(-15).decompress(blob, size)[:size]
    if alg == 2:
        try:
            import brotli  # type: ignore
        except ImportError as e:
            raise CacheFormatError("guest shader cache is Brotli-compressed; "
                                   "install the 'brotli' package") from e
        return brotli.decompress(blob)[:size]
    raise CacheFormatError(f"unknown compression algorithm {alg}")


def read_guest_shader_cache(toc_path: str, data_path: str) -> list[GuestShader]:
    with open(toc_path, "rb") as f:
        toc = f.read()
    with open(data_path, "rb") as f:
        data = f.read()
    return parse_guest_shader_cache(toc, data)


def parse_guest_shader_cache(toc: bytes, data: bytes) -> list[GuestShader]:
    if len(toc) < 32 or struct.unpack_from("<I", toc, 0)[0] != TOC_MAGIC:
        raise CacheFormatError("not a guest.toc (magic 'TOCG' missing)")
    entries = [struct.unpack_from("<4I", toc, off) for off in range(32, len(toc) - 15, 16)]
    # Entries are appended back to back; a Brotli stream must be cut at its exact end.
    starts = sorted({e[0] for e in entries}) + [len(data)]
    next_start = dict(zip(starts, starts[1:]))
    shaders = []
    for i, (off, code_size, cb1_size, h) in enumerate(entries):
        if off + cb1_size >= len(data):
            raise CacheFormatError(f"guest.toc entry {i} points beyond guest.data")
        alg = data[off + cb1_size]
        blob = data[off + cb1_size + 1:next_start[off]]
        code = _decompress(alg, blob, code_size)
        if len(code) != code_size:
            raise CacheFormatError(f"guest shader {i}: got {len(code)} bytes, expected {code_size}")
        shaders.append(GuestShader(i, code, cb1_size, h))
    return shaders


_LOAD_NAME_RE = re.compile(r"Loader Load: Loading (\w+)\.\.\.")
_LOAD_ADDR_RE = re.compile(r"LoadNsos: Loading image (\d+) at 0x([0-9A-Fa-f]+)")


def module_bases_from_log(path: str, max_bytes: int = 4 << 20) -> dict[str, int]:
    """Module name -> load address, from the first lines of a Ryujinx log."""
    with open(path, "rb") as f:
        head = f.read(max_bytes).decode("utf-8", "replace")
    return module_bases_from_text(head)


def module_bases_from_text(text: str) -> dict[str, int]:
    names = _LOAD_NAME_RE.findall(text)
    addrs = {int(i): int(a, 16) for i, a in _LOAD_ADDR_RE.findall(text)}
    if not names or len(addrs) != len(names):
        raise CacheFormatError(f"log: {len(names)} module names vs {len(addrs)} load addresses")
    return {name: addrs[i] for i, name in enumerate(names)}
