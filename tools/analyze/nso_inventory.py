#!/usr/bin/env python3
"""Inventory of a Switch ExeFS directory (NSO modules + main.npdm).

Reads the NSOs that the user extracted locally (never from the repo), and
reports per-module metadata (sizes, build IDs, SDK/middleware strings,
relocation counts) plus the cross-module import/export relationships that
the Horizon loader (rtld) resolves at runtime.

The output contains only metadata and symbol *statistics*; no code bytes.
Pure Python, no third-party dependencies (LZ4 block decoding is built in).

Also accepts a single NSO or NRO file (homebrew fixtures are usually NROs).

Usage:
    python tools/analyze/nso_inventory.py <exefs_dir|file.nso|file.nro>
        [--json out.json] [--markdown out.md] [--imports out.tsv]
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import struct
import sys
from collections import Counter, defaultdict
from dataclasses import dataclass, field

# Load order used by rtld for symbol lookup: main first, then subsdk0..9, then sdk.
LOAD_ORDER = ["rtld", "main"] + [f"subsdk{i}" for i in range(10)] + ["sdk"]

DT_NULL, DT_NEEDED, DT_PLTRELSZ, DT_RELA, DT_RELASZ = 0, 1, 2, 7, 8
DT_INIT, DT_FINI, DT_REL, DT_RELSZ, DT_JMPREL = 12, 13, 17, 18, 23
DT_INIT_ARRAYSZ, DT_FINI_ARRAYSZ = 27, 28
DT_HASH, DT_STRTAB, DT_SYMTAB, DT_STRSZ = 4, 5, 6, 10
DT_GNU_HASH = 0x6FFFFEF5
DT_RELRSZ, DT_RELR = 35, 36  # packed relative relocations (newer toolchains, e.g. devkitA64 r29)

R_AARCH64_NAMES = {
    257: "ABS64",
    1025: "GLOB_DAT",
    1026: "JUMP_SLOT",
    1027: "RELATIVE",
    1030: "TLSDESC",
}


class FormatError(Exception):
    pass


def lz4_block_decompress(src: bytes, out_size: int) -> bytes:
    """Decode a raw LZ4 block (no frame header) of known decompressed size."""
    dst = bytearray()
    i, n = 0, len(src)
    while i < n:
        token = src[i]
        i += 1
        lit = token >> 4
        if lit == 15:
            while True:
                b = src[i]
                i += 1
                lit += b
                if b != 255:
                    break
        dst += src[i:i + lit]
        i += lit
        if i >= n:
            break  # last sequence carries literals only
        off = src[i] | (src[i + 1] << 8)
        i += 2
        if off == 0 or off > len(dst):
            raise FormatError(f"lz4: bad match offset {off}")
        mlen = token & 15
        if mlen == 15:
            while True:
                b = src[i]
                i += 1
                mlen += b
                if b != 255:
                    break
        mlen += 4
        if len(dst) + mlen > out_size:
            raise FormatError(f"lz4: output exceeds expected {out_size} bytes")
        start = len(dst) - off
        if off >= mlen:
            dst += dst[start:start + mlen]
        else:  # overlapping copy
            for k in range(mlen):
                dst.append(dst[start + k])
    if len(dst) != out_size:
        raise FormatError(f"lz4: decoded {len(dst)} bytes, expected {out_size}")
    return bytes(dst)


@dataclass
class Segment:
    name: str
    file_offset: int
    mem_offset: int
    mem_size: int
    file_size: int
    compressed: bool
    hash_checked: bool
    sha256: bytes


@dataclass
class Symbol:
    name: str
    bind: int  # 0 local, 1 global, 2 weak
    type: int  # 0 notype, 1 object, 2 func, 6 tls
    shndx: int
    value: int
    size: int

    @property
    def defined(self) -> bool:
        return self.shndx != 0


@dataclass
class NsoModule:
    name: str
    file_size: int
    file_sha256: str
    module_id: str
    flags: int
    segments: list[Segment]
    bss_size: int
    image: bytes = b""
    ro_path_basename: str = ""
    api_info: list[str] = field(default_factory=list)
    symbols: list[Symbol] = field(default_factory=list)
    dynamic: dict[int, list[int]] = field(default_factory=dict)
    reloc_counts: Counter = field(default_factory=Counter)
    hashes_ok: bool = True
    eh_frame_hdr_size: int = 0
    fde_count: int = 0

    def seg(self, name: str) -> Segment:
        return next(s for s in self.segments if s.name == name)

    @property
    def image_size(self) -> int:
        d = self.seg("data")
        return d.mem_offset + d.mem_size + self.bss_size


# Upper bound for a single module image; real modules are far smaller (main: ~31 MiB).
# Protects against malformed headers requesting gigabyte allocations.
MAX_IMAGE_SIZE = 512 << 20


def load_module(path: str, name: str | None = None, verify: bool = True) -> NsoModule:
    """Parse an NSO (ExeFS module) or NRO (homebrew / ro module), detected by magic."""
    with open(path, "rb") as f:
        head = f.read(0x14)
    if head[:4] == b"NSO0":
        return parse_nso(path, name, verify)
    if head[0x10:0x14] == b"NRO0":
        return parse_nro(path, name)
    raise FormatError(f"{path}: neither NSO0 nor NRO0")


def parse_nso(path: str, name: str | None = None, verify: bool = True) -> NsoModule:
    with open(path, "rb") as f:
        raw = f.read()
    if len(raw) < 0x100 or raw[:4] != b"NSO0":
        raise FormatError(f"{path}: not an NSO0 file")
    try:
        return _parse_nso_bytes(raw, name or os.path.basename(path), verify)
    except (struct.error, IndexError, ValueError, UnicodeDecodeError) as e:
        raise FormatError(f"{path}: malformed NSO ({e})") from e


def _parse_nso_bytes(raw: bytes, name: str, verify: bool) -> NsoModule:
    flags = struct.unpack_from("<I", raw, 0x0C)[0]
    file_sizes = struct.unpack_from("<3I", raw, 0x60)
    segments = []
    for idx, seg_name in enumerate(("text", "ro", "data")):
        foff, moff, msize = struct.unpack_from("<3I", raw, 0x10 + idx * 0x10)
        segments.append(Segment(
            name=seg_name, file_offset=foff, mem_offset=moff, mem_size=msize,
            file_size=file_sizes[idx],
            compressed=bool(flags & (1 << idx)),
            hash_checked=bool(flags & (1 << (idx + 3))),
            sha256=raw[0xA0 + idx * 0x20:0xC0 + idx * 0x20],
        ))
    bss_size = struct.unpack_from("<I", raw, 0x3C)[0]
    _check_layout(segments, len(raw))
    mod = NsoModule(
        name=name,
        file_size=len(raw),
        file_sha256=hashlib.sha256(raw).hexdigest(),
        module_id=raw[0x40:0x60].hex(),
        flags=flags,
        segments=segments,
        bss_size=bss_size,
    )

    image = bytearray(segments[2].mem_offset + segments[2].mem_size)
    for s in segments:
        blob = raw[s.file_offset:s.file_offset + s.file_size]
        data = lz4_block_decompress(blob, s.mem_size) if s.compressed else blob[:s.mem_size]
        if verify and hashlib.sha256(data).digest() != s.sha256:
            mod.hashes_ok = False
        image[s.mem_offset:s.mem_offset + len(data)] = data
    mod.image = bytes(image)

    ro = mod.seg("ro")
    # .ro starts with {u32 unk; u32 len; char path[len]} on SDK-built modules.
    unk, plen = struct.unpack_from("<II", mod.image, ro.mem_offset)
    if unk == 0 and 0 < plen < 0x200:
        p = mod.image[ro.mem_offset + 8:ro.mem_offset + 8 + plen].split(b"\0")[0]
        mod.ro_path_basename = p.decode("utf-8", "replace").replace("\\", "/").rsplit("/", 1)[-1]

    api_off, api_size, dynstr_off, dynstr_size, dynsym_off, dynsym_size = struct.unpack_from("<6I", raw, 0x88)
    _read_embedded(mod, api_off, api_size, dynstr_off, dynstr_size, dynsym_off, dynsym_size)
    _parse_mod0(mod)
    if not mod.symbols:
        _symbols_from_dynamic(mod)
    return mod


def parse_nro(path: str, name: str | None = None) -> NsoModule:
    """Parse an uncompressed NRO (as built by devkitA64/libnx elf2nro)."""
    with open(path, "rb") as f:
        raw = f.read()
    if len(raw) < 0x80 or raw[0x10:0x14] != b"NRO0":
        raise FormatError(f"{path}: not an NRO0 file")
    try:
        return _parse_nro_bytes(raw, name or os.path.basename(path))
    except (struct.error, IndexError, ValueError, UnicodeDecodeError) as e:
        raise FormatError(f"{path}: malformed NRO ({e})") from e


def _parse_nro_bytes(raw: bytes, name: str) -> NsoModule:
    _version, nro_size, flags = struct.unpack_from("<3I", raw, 0x14)
    segments = []
    for idx, seg_name in enumerate(("text", "ro", "data")):
        off, size = struct.unpack_from("<II", raw, 0x20 + idx * 8)
        # NRO segments are stored uncompressed at file offset == memory offset.
        segments.append(Segment(seg_name, off, off, size, size, False, False, b""))
    bss_size = struct.unpack_from("<I", raw, 0x38)[0]
    if nro_size > len(raw):
        raise FormatError(f"NRO header size 0x{nro_size:x} exceeds file size 0x{len(raw):x}")
    _check_layout(segments, nro_size)
    mod = NsoModule(
        name=name,
        file_size=len(raw),
        file_sha256=hashlib.sha256(raw).hexdigest(),
        module_id=raw[0x40:0x60].hex(),
        flags=flags,
        segments=segments,
        bss_size=bss_size,
    )
    d = segments[2]
    mod.image = raw[:d.mem_offset + d.mem_size]
    # Bytes after nro_size are the homebrew asset section (icon/NACP/RomFS); ignored.
    api_off, api_size, dynstr_off, dynstr_size, dynsym_off, dynsym_size = struct.unpack_from("<6I", raw, 0x68)
    _read_embedded(mod, api_off, api_size, dynstr_off, dynstr_size, dynsym_off, dynsym_size)
    _parse_mod0(mod)
    if not mod.symbols:
        _symbols_from_dynamic(mod)
    return mod


def _check_layout(segments: list[Segment], file_size: int) -> None:
    """Reject headers whose segments overlap, run backwards or leave the file."""
    prev_end = 0
    for s in segments:
        if s.file_offset + s.file_size > file_size:
            raise FormatError(f".{s.name}: file range 0x{s.file_offset:x}+0x{s.file_size:x} beyond end of file")
        if s.mem_offset < prev_end:
            raise FormatError(f".{s.name}: memory offset 0x{s.mem_offset:x} overlaps previous segment")
        prev_end = s.mem_offset + s.mem_size
    if prev_end > MAX_IMAGE_SIZE:
        raise FormatError(f"image size 0x{prev_end:x} exceeds limit")


def _read_embedded(mod: NsoModule, api_off: int, api_size: int, dynstr_off: int, dynstr_size: int,
                   dynsym_off: int, dynsym_size: int) -> None:
    """api_info and dynsym/dynstr as referenced by the header (offsets relative to .ro)."""
    ro = mod.seg("ro")

    def ro_slice(off: int, size: int) -> bytes:
        if off + size > ro.mem_size:
            raise FormatError(f"embedded range 0x{off:x}+0x{size:x} outside .ro")
        return mod.image[ro.mem_offset + off:ro.mem_offset + off + size]

    if api_size:
        mod.api_info = [s.decode("utf-8", "replace") for s in ro_slice(api_off, api_size).split(b"\0") if s]
    if dynsym_size:
        mod.symbols = _decode_symbols(ro_slice(dynsym_off, dynsym_size), ro_slice(dynstr_off, dynstr_size))


def _decode_symbols(symtab: bytes, strtab: bytes) -> list[Symbol]:
    out = []
    for k in range(len(symtab) // 24):
        st_name, st_info, _other, shndx, value, size = struct.unpack_from("<IBBHQQ", symtab, k * 24)
        end = strtab.find(b"\0", st_name)
        sname = strtab[st_name:end if end >= 0 else None].decode("utf-8", "replace")
        out.append(Symbol(sname, st_info >> 4, st_info & 0xF, shndx, value, size))
    return out


def _symbol_count(mod: NsoModule, symtab: int, strtab: int) -> int:
    """Number of dynsym entries: DT_HASH nchain, else DT_GNU_HASH walk, else symtab..strtab gap."""
    img = mod.image
    if DT_HASH in mod.dynamic:
        return struct.unpack_from("<I", img, mod.dynamic[DT_HASH][0] + 4)[0]
    if DT_GNU_HASH in mod.dynamic:
        h = mod.dynamic[DT_GNU_HASH][0]
        nbuckets, symoffset, bloom_size, _shift = struct.unpack_from("<4I", img, h)
        buckets = h + 16 + bloom_size * 8
        chains = buckets + nbuckets * 4
        if chains > len(img):
            raise FormatError("DT_GNU_HASH table outside module image")
        last = max(struct.unpack_from(f"<{nbuckets}I", img, buckets), default=0)
        if last < symoffset:
            return symoffset
        while not struct.unpack_from("<I", img, chains + (last - symoffset) * 4)[0] & 1:
            last += 1
        return last + 1
    if strtab > symtab:
        return (strtab - symtab) // 24
    raise FormatError("cannot determine dynsym size (no DT_HASH/DT_GNU_HASH)")


def _symbols_from_dynamic(mod: NsoModule) -> None:
    """Fallback when the header carries no dynsym range (typical for NROs)."""
    if DT_SYMTAB not in mod.dynamic or DT_STRTAB not in mod.dynamic:
        return
    symtab, strtab = mod.dynamic[DT_SYMTAB][0], mod.dynamic[DT_STRTAB][0]
    strsz = mod.dynamic.get(DT_STRSZ, [len(mod.image) - strtab])[0]
    count = _symbol_count(mod, symtab, strtab)
    if symtab + count * 24 > len(mod.image) or strtab + strsz > len(mod.image):
        raise FormatError("dynsym/dynstr outside module image")
    mod.symbols = _decode_symbols(mod.image[symtab:symtab + count * 24], mod.image[strtab:strtab + strsz])


def _parse_mod0(mod: NsoModule) -> None:
    img = mod.image
    mod0_off = struct.unpack_from("<I", img, 4)[0]
    if mod0_off + 0x1C > len(img) or img[mod0_off:mod0_off + 4] != b"MOD0":
        return
    dyn_rel, _bss_s, _bss_e, eh_s, eh_e = struct.unpack_from("<5i", img, mod0_off + 4)
    mod.eh_frame_hdr_size = eh_e - eh_s
    hdr = mod0_off + eh_s
    # eh_s is signed relative to MOD0: .eh_frame_hdr (.ro) may lie before a MOD0 in .data.
    if eh_e > eh_s and 0 <= hdr and hdr + 12 <= len(img) and img[hdr] == 1:
        # .eh_frame_hdr: version, eh_frame_ptr_enc, fde_count_enc, table_enc, eh_frame_ptr, fde_count
        ptr_enc, cnt_enc = img[hdr + 1], img[hdr + 2]
        if ptr_enc & 0x0F in (0x03, 0x0B) and cnt_enc == 0x03:  # 4-byte ptr, udata4 count
            mod.fde_count = struct.unpack_from("<I", img, hdr + 8)[0]
    dyn = mod0_off + dyn_rel
    if dyn < 0:  # struct.unpack_from would silently index from the end
        raise FormatError(f"MOD0: .dynamic offset {dyn_rel} points before the image")
    while dyn + 16 <= len(img):
        tag, val = struct.unpack_from("<qQ", img, dyn)
        dyn += 16
        if tag == DT_NULL:
            break
        mod.dynamic.setdefault(tag, []).append(val)

    def count_rela(off_tag: int, size_tag: int) -> None:
        if off_tag in mod.dynamic and size_tag in mod.dynamic:
            off, size = mod.dynamic[off_tag][0], mod.dynamic[size_tag][0]
            for k in range(size // 24):
                if off + k * 24 + 24 > len(img):
                    break
                info = struct.unpack_from("<Q", img, off + k * 24 + 8)[0]
                rtype = info & 0xFFFFFFFF
                mod.reloc_counts[R_AARCH64_NAMES.get(rtype, f"type{rtype}")] += 1

    count_rela(DT_RELA, DT_RELASZ)
    count_rela(DT_JMPREL, DT_PLTRELSZ)
    relr = _count_relr(mod)
    if relr:
        mod.reloc_counts["RELR"] += relr


def _count_relr(mod: NsoModule) -> int:
    """Number of locations a DT_RELR table relocates (each one is an implicit RELATIVE).

    Encoding: an even entry is an address (one relocation, next base = addr + 8);
    an odd entry is a bitmap whose bits 1..63 mark base + (bit - 1) * 8, then base += 63 * 8.
    """
    if DT_RELR not in mod.dynamic or DT_RELRSZ not in mod.dynamic:
        return 0
    off, size = mod.dynamic[DT_RELR][0], mod.dynamic[DT_RELRSZ][0]
    count = 0
    for k in range(size // 8):
        if off + k * 8 + 8 > len(mod.image):
            break
        entry = struct.unpack_from("<Q", mod.image, off + k * 8)[0]
        count += 1 if entry & 1 == 0 else bin(entry >> 1).count("1")
    return count


def parse_npdm_summary(path: str) -> dict:
    """Minimal META/ACI0 summary: main thread params and declared services."""
    with open(path, "rb") as f:
        raw = f.read()
    if raw[:4] != b"META":
        raise FormatError(f"{path}: not a META file")
    flags = raw[0x0C]
    out = {
        "is_64bit": bool(flags & 1),
        "address_space_type": (flags >> 1) & 7,
        "main_thread_priority": raw[0x0E],
        "main_thread_core": raw[0x0F],
        "system_resource_size": struct.unpack_from("<I", raw, 0x14)[0],
        "version": struct.unpack_from("<I", raw, 0x18)[0],
        "main_thread_stack_size": struct.unpack_from("<I", raw, 0x1C)[0],
        "name": raw[0x20:0x30].split(b"\0")[0].decode(),
    }
    aci_off, _aci_size = struct.unpack_from("<II", raw, 0x70)
    aci = raw[aci_off:]
    if aci[:4] == b"ACI0":
        out["program_id"] = f"{struct.unpack_from('<Q', aci, 0x10)[0]:016x}"
        sac_off, sac_size = struct.unpack_from("<II", aci, 0x28)
        services, p, end = [], sac_off, sac_off + sac_size
        while p < end:
            ctrl = aci[p]
            p += 1
            ln = (ctrl & 7) + 1
            services.append(("host:" if ctrl & 0x80 else "") + aci[p:p + ln].decode("ascii", "replace"))
            p += ln
        out["services"] = sorted(services)
    return out


def namespace_of(sym: str) -> str:
    """Coarse namespace bucket for an Itanium-mangled or C symbol name."""
    if sym.startswith(("_ZN", "_ZNK")):
        s = sym[4:] if sym.startswith("_ZNK") else sym[3:]
        parts = []
        while s and s[0].isdigit() and len(parts) < 2:
            j = 0
            while j < len(s) and s[j].isdigit():
                j += 1
            ln = int(s[:j])
            parts.append(s[j:j + ln])
            s = s[j + ln:]
        if parts and parts[0] == "nn" and len(parts) > 1:
            return "nn::" + parts[1]
        return parts[0] if parts else "c++(other)"
    if sym.startswith("_ZNSt") or sym.startswith("_ZSt") or sym.startswith("_ZTVNSt") or sym.startswith("_ZTINSt"):
        return "std"
    if sym.startswith("_Z"):
        return "c++(other)"
    for prefix in ("nvn", "glslc_", "NvOs", "NvRm", "__cxa", "__nnmusl", "nnos", "nninit"):
        if sym.startswith(prefix):
            return prefix
    return "libc/other"


SYM_BIND_NAMES = {0: "local", 1: "global", 2: "weak"}
SYM_TYPE_NAMES = {0: "notype", 1: "object", 2: "func", 3: "section", 4: "file", 6: "tls"}


def _module_order(mods: dict[str, NsoModule]) -> list[str]:
    """rtld load order for ExeFS names; any other module (single NRO/NSO) after them."""
    return [m for m in LOAD_ORDER if m in mods] + sorted(m for m in mods if m not in LOAD_ORDER)


def _providers(mods: dict[str, NsoModule]) -> dict[str, str]:
    """First module in load order that exports each symbol name (rtld global lookup)."""
    providers: dict[str, str] = {}
    for m in _module_order(mods):
        if m == "rtld":
            continue
        for s in mods[m].symbols:
            if s.defined and s.bind in (1, 2) and s.name and s.name not in providers:
                providers[s.name] = m
    return providers


def import_rows(mods: dict[str, NsoModule]) -> list[dict]:
    """One row per imported symbol name: importer, symbol, bind, type, provider ('' if unresolved)."""
    providers = _providers(mods)
    rows = []
    for m in _module_order(mods):
        for s in mods[m].symbols:
            if s.defined or not s.name or s.bind == 0:
                continue
            rows.append({
                "importer": m,
                "symbol": s.name,
                "bind": SYM_BIND_NAMES.get(s.bind, str(s.bind)),
                "type": SYM_TYPE_NAMES.get(s.type, str(s.type)),
                "provider": providers.get(s.name, ""),
            })
    return rows


def resolve_imports(mods: dict[str, NsoModule]) -> dict:
    providers = _providers(mods)
    edges: dict[str, Counter] = {}
    unresolved: dict[str, dict] = {}
    by_ns: dict[str, dict[str, Counter]] = {}
    for m in _module_order(mods):
        c, ns = Counter(), defaultdict(Counter)
        unres = {"strong": 0, "weak": 0, "strong_names_sample": []}
        for s in mods[m].symbols:
            if s.defined or not s.name or s.bind == 0:
                continue
            prov = providers.get(s.name)
            if prov is None:
                if s.bind == 2:
                    unres["weak"] += 1
                else:
                    unres["strong"] += 1
                    if len(unres["strong_names_sample"]) < 20:
                        unres["strong_names_sample"].append(s.name)
                continue
            c[prov] += 1
            ns[prov][namespace_of(s.name)] += 1
        edges[m], unresolved[m], by_ns[m] = c, unres, ns
    return {"edges": edges, "unresolved": unresolved, "by_namespace": by_ns}


def _trim_zero_bytes(hex_id: str) -> str:
    """Drop trailing zero *bytes* of a hex ID (build IDs are 16 or 20 bytes padded to 32)."""
    while hex_id.endswith("00") and len(hex_id) > 2:
        hex_id = hex_id[:-2]
    return hex_id


def module_summary(mod: NsoModule) -> dict:
    syms = mod.symbols
    imports = [s for s in syms if not s.defined and s.name and s.bind != 0]
    exports = [s for s in syms if s.defined and s.name and s.bind in (1, 2)]
    text = mod.seg("text")
    return {
        "name": mod.name,
        "file_size": mod.file_size,
        "file_sha256": mod.file_sha256,
        "module_id": _trim_zero_bytes(mod.module_id),
        "hashes_ok": mod.hashes_ok,
        "ro_path_basename": mod.ro_path_basename,
        "segments": {s.name: {"mem_offset": s.mem_offset, "mem_size": s.mem_size,
                              "file_size": s.file_size, "compressed": s.compressed}
                     for s in mod.segments},
        "bss_size": mod.bss_size,
        "image_size": mod.image_size,
        "text_insn_estimate": text.mem_size // 4,
        "api_info": mod.api_info,
        "dynsym_count": len(syms),
        "import_count": len(imports),
        "export_count": len(exports),
        "export_func_count": sum(1 for s in exports if s.type == 2),
        "reloc_counts": dict(mod.reloc_counts),
        "init_array_bytes": mod.dynamic.get(DT_INIT_ARRAYSZ, [0])[0],
        "needed_count": len(mod.dynamic.get(DT_NEEDED, [])),
        "eh_frame_hdr_size": mod.eh_frame_hdr_size,
        "fde_count": mod.fde_count,
    }


def load_exefs(exefs_dir: str) -> dict[str, NsoModule]:
    mods: dict[str, NsoModule] = {}
    for name in LOAD_ORDER:
        p = os.path.join(exefs_dir, name)
        if os.path.isfile(p):
            mods[name] = parse_nso(p, name)
    if "main" not in mods:
        raise FormatError(f"{exefs_dir}: no 'main' NSO found")
    return mods


def load_modules(path: str) -> dict[str, NsoModule]:
    """An ExeFS directory, or a single NSO/NRO file (e.g. a homebrew fixture)."""
    if os.path.isdir(path):
        return load_exefs(path)
    mod = load_module(path)
    return {mod.name: mod}


def build_inventory(path: str, mods: dict[str, NsoModule] | None = None) -> dict:
    if mods is None:
        mods = load_modules(path)
    inv = {
        "modules": [module_summary(mods[n]) for n in _module_order(mods)],
        "links": resolve_imports(mods),
    }
    npdm = os.path.join(path, "main.npdm")
    if os.path.isdir(path) and os.path.isfile(npdm):
        inv["npdm"] = parse_npdm_summary(npdm)
    return inv


def write_imports_tsv(rows: list[dict], out_path: str) -> None:
    cols = ["importer", "symbol", "bind", "type", "provider"]
    with open(out_path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\t".join(cols) + "\n")
        for r in sorted(rows, key=lambda r: (r["importer"], r["provider"], r["symbol"])):
            f.write("\t".join(r[c] for c in cols) + "\n")


def to_markdown(inv: dict) -> str:
    """Tables only; meant as raw material for docs/analysis, not as the doc itself."""
    out = ["| module | file | .text | .ro | .data | .bss | image | insns (approx.) | Imports | Exports | Build-ID |",
           "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|"]
    for m in inv["modules"]:
        s = m["segments"]
        out.append(f"| {m['name']} | {m['file_size']:,} | 0x{s['text']['mem_size']:x} | 0x{s['ro']['mem_size']:x} "
                   f"| 0x{s['data']['mem_size']:x} | 0x{m['bss_size']:x} | 0x{m['image_size']:x} "
                   f"| {m['text_insn_estimate']:,} | {m['import_count']:,} | {m['export_count']:,} | `{m['module_id']}` |")
    out += ["", "| importer -> provider | symbols |", "|---|---:|"]
    for imp, c in inv["links"]["edges"].items():
        for prov, n in sorted(c.items(), key=lambda kv: -kv[1]):
            out.append(f"| {imp} -> {prov} | {n:,} |")
    return "\n".join(out) + "\n"


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Inventory of a Switch ExeFS directory or a single NSO/NRO.")
    ap.add_argument("path", help="ExeFS directory (main, subsdk*, sdk, main.npdm) or one NSO/NRO file")
    ap.add_argument("--json", help="write full inventory as JSON (keep it in LGP_PRIVATE_DIR)")
    ap.add_argument("--markdown", help="write summary tables as Markdown")
    ap.add_argument("--imports", help="write every imported symbol name with its provider as TSV")
    args = ap.parse_args(argv)
    try:
        mods = load_modules(args.path)
    except FormatError as e:
        print(f"error: {e}", file=sys.stderr)
        return 2
    inv = build_inventory(args.path, mods)
    if args.imports:
        write_imports_tsv(import_rows(mods), args.imports)
    if args.json:
        with open(args.json, "w", encoding="utf-8") as f:
            json.dump(inv, f, indent=2, default=lambda o: dict(o))
    md = to_markdown(inv)
    if args.markdown:
        with open(args.markdown, "w", encoding="utf-8") as f:
            f.write(md)
    else:
        sys.stdout.write(md)
    return 0


if __name__ == "__main__":
    sys.exit(main())
