#!/usr/bin/env python3
"""Carry the game addresses of lift configs, mod files and kit recipes over to another game version.

All mod and lift files (game.cfg, mods/<id>.cfg, mods/<id>/derive.json) name guest addresses of one
build of the game (Let's Go Pikachu 1.0.0 so far). Another build (Eevee, update 1.0.2) moves almost
every function by a few bytes and changes a handful. This tool takes the old and the new ExeFS,
aligns the functions with version_diff.py and rewrites every address it can prove:

  code (.text)   the address lies in a function that version_diff counts as same or moved: the new
                 address is the function's new start plus the same offset (its masked body is equal,
                 so the instruction there is the same one). An address in a changed or removed
                 function, or between functions, is not carried over.
  data (.ro,     the bytes from the address on (16 up to 256) must occur exactly once in the same
  .data)         segment of the new module; failing that, a window centred on the address. Data that
                 holds relocated pointers or differs between the builds is not carried over.
  .bss           by the instruction that computes the address; while .bss keeps its size, the offset from its
                 start must agree.

Addresses are the lift's guest addresses: the modules named by --modules (default main, sdk, the
lifted ones, ADR-0005) are placed like recomp's link step from --base (default 0x08000000) on, each
at the next page after the one before. In a .cfg every hex number inside one of these images is an
address (the rest of the line, small offsets and hashes, stays as it is; text after '#' too). In a
derive.json every object with "module" and "offset" names a table at module+offset.

A file is written to --out only when every address in it was carried over, unless --partial: then
lines with an unresolved address are commented out with a marker and the exit code is 1.
The report (--report, TSV with file, line, old, new, kind, status) names addresses and stays in
$LGP_PRIVATE_DIR like every output; stdout prints counts only.

Usage:
    python3 tools/analyze/addr_translate.py <old_exefs> <new_exefs> --out DIR [--report out.tsv]
        [--partial] [--modules main,sdk] [--base 0x08000000] FILE_OR_DIR...
"""
from __future__ import annotations

import argparse
import bisect
import json
import os
import re
import struct
import sys
from collections import Counter
from dataclasses import dataclass

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nso_inventory as inv  # noqa: E402
import version_diff as vd  # noqa: E402

PAGE = 0x1000
DEFAULT_BASE = 0x08000000
DEFAULT_MODULES = ("main", "sdk")
WINDOWS = (16, 32, 64, 128, 256)
UNRESOLVED_MARK = "# UNRESOLVED (addr_translate): "
HEX = re.compile(r"(?<![0-9A-Za-z_])0x([0-9A-Fa-f]+)(?![0-9A-Za-z_])")
ADRP_WINDOW = 32  # instructions an ADRP page counts for its consumer
REF_SLACK = 0x40  # a field up to this far behind a referenced object start is found via that start


def _sext(v: int, bits: int) -> int:
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


def _adrp(w: int, pc: int) -> tuple[int, int] | None:
    """(rd, page) of an ADRP, else None."""
    if (w & 0x9F000000) != 0x90000000:
        return None
    imm = _sext((((w >> 5) & 0x7FFFF) << 2) | ((w >> 29) & 3), 21)
    return w & 31, (pc & ~0xFFF) + (imm << 12)


def _page_user(w: int) -> tuple[int, int, int | None] | None:
    """(rn, low offset, written register or None) of an ADD-immediate / LDR / STR (unsigned offset)."""
    if (w & 0xFF000000) == 0x91000000:  # add xd, xn, #imm{, lsl 12}
        imm = (w >> 10) & 0xFFF
        return (w >> 5) & 31, imm << 12 if w & (1 << 22) else imm, w & 31
    if (w & 0x3B000000) == 0x39000000:  # ldr/str (unsigned offset), GPR and SIMD&FP
        size, vector, opc = w >> 30, (w >> 26) & 1, (w >> 22) & 3
        scale = 4 if vector and opc & 2 else size
        loads = opc != 0
        return (w >> 5) & 31, ((w >> 10) & 0xFFF) << scale, (w & 31) if loads and not vector else None
    return None


def _pc_relative(w: int, pc: int) -> int | None:
    """Target of an ADR or a literal load, else None."""
    if (w & 0x9F000000) == 0x10000000:  # adr
        return pc + _sext((((w >> 5) & 0x7FFFF) << 2) | ((w >> 29) & 3), 21)
    if (w & 0x3B000000) == 0x18000000:  # ldr (literal), GPR and SIMD&FP
        return pc + (_sext((w >> 5) & 0x7FFFF, 19) << 2)
    return None


def code_references(mod: inv.NsoModule) -> dict[int, list[tuple[int, int]]]:
    """Image offset -> [(consumer offset, instructions back to its ADRP, 0 for ADR/literal)] of .text."""
    t = mod.seg("text")
    words = memoryview(mod.image[t.mem_offset:t.mem_offset + t.mem_size]).cast("I")
    pages: dict[int, tuple[int, int]] = {}  # register -> (page, index of the ADRP)
    out: dict[int, list[tuple[int, int]]] = {}
    # A page only counts on a straight path from its ADRP: none across a branch or into a branch target (another
    # path may arrive there with another value), and none past an instruction that may write the register.
    targets = set()
    for i, w in enumerate(words):
        if vd._is_branch(w):
            targets.add(i + vd._pc_target(w)[0])
    for i, w in enumerate(words):
        pc = t.mem_offset + i * 4
        if i in targets:
            pages.clear()
        a = _adrp(w, pc)
        if a is not None:
            pages[a[0]] = (a[1], i)
            continue
        target = _pc_relative(w, pc)
        if target is not None:
            out.setdefault(target, []).append((pc, 0))
            pages.pop(w & 31, None)
            continue
        u = _page_user(w)
        if u is None:
            _forget_written(w, pages)
            continue
        rn, low, written = u
        page = pages.get(rn)
        if page is not None and i - page[1] <= ADRP_WINDOW:
            out.setdefault(page[0] + low, []).append((pc, i - page[1]))
        if written is not None:
            pages.pop(written, None)
    return out


def _forget_written(w: int, pages: dict[int, tuple[int, int]]) -> None:
    """Drops every page a register may lose with instruction `w` (conservative: a dropped page only costs a match)."""
    if (w & 0xFFFFF01F) == 0xD503201F:  # hints (NOP, BTI, …) write nothing
        return
    if (w & 0x1C000000) == 0x14000000:  # branches, exception generation, system: control may leave
        pages.clear()
    elif (w & 0x0A000000) == 0x08000000:  # loads and stores: Rt, Rt2 of pairs, Rn with writeback
        for r in (w & 31, (w >> 10) & 31, (w >> 5) & 31):
            pages.pop(r, None)
    else:  # data processing writes Rd
        pages.pop(w & 31, None)


R_AARCH64_RELATIVE = 1027


def relative_pointers(mod: inv.NsoModule) -> dict[int, int]:
    """Place -> target (image offsets) of every RELATIVE relocation (DT_RELA, and DT_RELR with the addend in place)."""
    img, out = mod.image, {}
    if inv.DT_RELA in mod.dynamic and inv.DT_RELASZ in mod.dynamic:
        base, size = mod.dynamic[inv.DT_RELA][0], mod.dynamic[inv.DT_RELASZ][0]
        for k in range(size // 24):
            if base + k * 24 + 24 > len(img):
                break
            place, info, addend = struct.unpack_from("<QQq", img, base + k * 24)
            if info & 0xFFFFFFFF == R_AARCH64_RELATIVE:
                out[place] = addend
    if inv.DT_RELR in mod.dynamic and inv.DT_RELRSZ in mod.dynamic:
        base, size, where = mod.dynamic[inv.DT_RELR][0], mod.dynamic[inv.DT_RELRSZ][0], 0
        for k in range(size // 8):
            entry = struct.unpack_from("<Q", img, base + k * 8)[0]
            places = [entry] if entry & 1 == 0 else [where + (b - 1) * 8 for b in range(1, 64) if entry >> b & 1]
            where = entry + 8 if entry & 1 == 0 else where + 63 * 8
            for p in places:
                if p + 8 <= len(img):
                    out[p] = struct.unpack_from("<Q", img, p)[0]
    return out


def reference_target(mod: inv.NsoModule, pc: int, distance: int) -> int | None:
    """What the consumer at `pc` computes in `mod`, with its ADRP `distance` instructions before it."""
    w = int.from_bytes(mod.image[pc:pc + 4], "little")
    if distance == 0:
        return _pc_relative(w, pc)
    u = _page_user(w)
    back = pc - 4 * distance
    a = _adrp(int.from_bytes(mod.image[back:back + 4], "little"), back) if back >= 0 else None
    if u is None or a is None or a[0] != u[0]:
        return None
    return a[1] + u[1]


@dataclass
class Result:
    new: int | None  # module-relative offset in the new build, None if not carried over
    kind: str        # text, ro, data, bss, outside
    status: str      # same, moved, data, or why it failed


class ModuleMap:
    """Old -> new offsets of one module, from version_diff's function rows and segment bytes."""

    def __init__(self, old: inv.NsoModule, new: inv.NsoModule, rows: list[dict] | None = None):
        self.old, self.new = old, new
        if rows is None:
            rows = vd.diff_module(old, new)["function_rows"]
        rows = sorted((r for r in rows if r.get("old") is not None), key=lambda r: r["old"])
        self.starts = [r["old"] for r in rows]
        self.rows = rows
        self._ref_index: dict[int, list[tuple[int, int]]] | None = None
        self._ref_sorted: list[int] | None = None
        self._pointers: tuple[dict[int, int], dict[int, int]] | None = None

    def _segment(self, mod: inv.NsoModule, off: int) -> str:
        for s in mod.segments:
            if s.mem_offset <= off < s.mem_offset + s.mem_size:
                return s.name
        d = mod.seg("data")
        if d.mem_offset + d.mem_size <= off < mod.image_size:
            return "bss"
        return "outside"

    def translate(self, off: int) -> Result:
        kind = self._segment(self.old, off)
        if kind == "text":
            return self._text(off)
        if kind in ("ro", "data"):
            return self._data(off, kind)
        if kind == "bss":
            return self._bss(off)
        return Result(None, kind, "outside the module")

    def _bss(self, off: int) -> Result:
        """.bss has no bytes: only the code that computes the address finds it, or a relocated pointer to it (a GOT
        slot the code loads) found again at the slot's new place. Where .bss keeps its size, the offset from its
        start is the second check (a reordered .bss would show up as a disagreement)."""
        by_code, how = self._by_code(off)
        if by_code is None:
            by_code, how = self._by_pointer(off)
        if by_code is None:
            return Result(None, "bss", "no code reference or relocated pointer found")
        start = lambda m: m.seg("data").mem_offset + m.seg("data").mem_size
        if self.old.bss_size == self.new.bss_size:
            by_layout = start(self.new) + off - start(self.old)
            if by_layout != by_code:
                return Result(None, "bss", "code reference and .bss offset disagree")
            return Result(by_code, "bss", how + "+bss offset")
        return Result(by_code, "bss", how)

    def _text(self, off: int) -> Result:
        i = bisect.bisect_right(self.starts, off) - 1
        if i < 0 or off >= self.rows[i]["old"] + self.rows[i]["old_size"]:
            return Result(None, "text", "between functions")
        r = self.rows[i]
        if r["status"] not in ("same", "moved"):
            return Result(None, "text", f"in a {r['status']} function")
        return Result(r["new"] + off - r["old"], "text", r["status"])

    def _data(self, off: int, kind: str) -> Result:
        so, sn = self.old.seg(kind), self.new.seg(kind)
        old_seg = self.old.image[so.mem_offset:so.mem_offset + so.mem_size]
        new_seg = self.new.image[sn.mem_offset:sn.mem_offset + sn.mem_size]
        if old_seg == new_seg and so.mem_offset == sn.mem_offset:
            return Result(off, kind, "same")
        by_bytes = self._by_bytes(off, so.mem_offset, old_seg, sn.mem_offset, new_seg)
        by_code, how = self._by_code(off)
        if by_bytes is not None and by_code is not None:
            if by_bytes != by_code:
                return Result(None, kind, "bytes and code references disagree")
            return Result(by_bytes, kind, "bytes+" + how)
        if by_bytes is not None:
            return Result(by_bytes, kind, "bytes")
        if by_code is not None:
            # Bytes that occur more than once (constants) still confirm the place when they match there.
            rel, nrel = off - so.mem_offset, by_code - sn.mem_offset
            same = 0 <= nrel and old_seg[rel:rel + 4] == new_seg[nrel:nrel + 4]
            return Result(by_code, kind, how + (", bytes equal" if same else ""))
        return Result(None, kind, "neither bytes nor a code reference found")

    # A data address the code computes (ADRP + ADD/LDR/STR, ADR, literal load) is found again by the
    # same instruction in the moved function. Jump tables and other data whose bytes hold code
    # offsets have no stable bytes, but their reference in the code is stable.
    def _refs(self) -> dict[int, list[tuple[int, int]]]:
        index = self._ref_index
        if index is None:
            index = self._ref_index = code_references(self.old)
        return index

    def _by_code(self, off: int) -> tuple[int | None, str]:
        refs, how, extra = self._refs().get(off), "code", 0
        if not refs:
            below = [t for t in self._ref_targets() if off - REF_SLACK <= t < off]
            if not below:
                return None, ""
            base = max(below)
            refs, how, extra = self._refs()[base], "code+offset", off - base
        found = set()
        for consumer, distance in refs:
            r = self._text(consumer)
            if r.new is None:
                continue
            t = reference_target(self.new, r.new, distance)
            if t is not None:
                found.add(t + extra)
        return (found.pop(), how) if len(found) == 1 else (None, "")

    def _by_pointer(self, off: int) -> tuple[int | None, str]:
        """Every relocated pointer to `off` (RELATIVE relocations), carried over as data, must point to one place."""
        if self._pointers is None:
            self._pointers = (relative_pointers(self.old), relative_pointers(self.new))
        old_ptrs, new_ptrs = self._pointers
        found = set()
        for place in (p for p, target in old_ptrs.items() if target == off):
            kind = self._segment(self.old, place)
            r = self._data(place, kind) if kind in ("ro", "data") else Result(None, kind, "")
            if r.new is None or r.new not in new_ptrs:
                return None, ""
            found.add(new_ptrs[r.new])
        return (found.pop(), "pointer") if len(found) == 1 else (None, "")

    def _ref_targets(self) -> list[int]:
        if self._ref_sorted is None:
            self._ref_sorted = sorted(self._refs())
        return self._ref_sorted

    @staticmethod
    def _by_bytes(off: int, old_base: int, old_seg: bytes, new_base: int, new_seg: bytes) -> int | None:
        rel = off - old_base
        for width in WINDOWS:
            for start in (rel, rel - width // 2):
                start = max(0, start)
                needle = old_seg[start:start + width]
                if len(needle) < 8 or not any(needle):
                    continue  # all zero (or too short at the end): matches anywhere
                # Unique on both sides: a window that also stands elsewhere in the old segment could have moved
                # with that other object (Codex P2 on #545); a longer one is tried next.
                if old_seg.find(needle) != start or old_seg.find(needle, start + 1) >= 0:
                    continue
                hit = new_seg.find(needle)
                if hit >= 0 and new_seg.find(needle, hit + 1) < 0:
                    return new_base + hit + (rel - start)
        return None


class ImageMap:
    """Guest addresses of the lifted modules in the old build -> the same places in the new one."""

    def __init__(self, old_exefs: str, new_exefs: str, modules=DEFAULT_MODULES, base: int = DEFAULT_BASE,
                 diff: dict | None = None):
        old, new = inv.load_exefs(old_exefs), inv.load_exefs(new_exefs)
        rows = {m["name"]: m.get("function_rows") for m in diff["modules"]} if diff else {}
        self.maps: dict[str, ModuleMap] = {}
        self.old_bases: list[tuple[int, int, str]] = []  # (base, image size, name)
        self.new_base: dict[str, int] = {}
        ob = nb = base
        for name in modules:
            if name not in old or name not in new:
                raise inv.FormatError(f"module {name} missing in one of the ExeFS folders")
            self.maps[name] = ModuleMap(old[name], new[name], rows.get(name))
            self.old_bases.append((ob, old[name].image_size, name))
            self.new_base[name] = nb
            ob = _align(ob + old[name].image_size)
            nb = _align(nb + new[name].image_size)

    def module_of(self, va: int) -> tuple[str, int] | None:
        for b, size, name in self.old_bases:
            if b <= va < b + size:
                return name, va - b
        return None

    def translate_va(self, va: int) -> Result:
        hit = self.module_of(va)
        if hit is None:
            return Result(None, "outside", "not an address")
        name, off = hit
        r = self.maps[name].translate(off)
        return Result(None if r.new is None else self.new_base[name] + r.new, f"{name}:{r.kind}", r.status)

    def read(self, va: int, size: int, new: bool) -> bytes | None:
        hit = self.module_of(va) if not new else next(
            ((n, va - b) for n, b in self.new_base.items() if 0 <= va - b < self.maps[n].new.image_size), None)
        if hit is None:
            return None
        mod = self.maps[hit[0]].new if new else self.maps[hit[0]].old
        return mod.image[hit[1]:hit[1] + size]

    def jump_targets(self, fields: dict[str, int], new: bool) -> list[int] | None:
        """Targets of a game.cfg `jumptable` (recomp/core/src/config.cpp: target = base + (entry << shift))."""
        size, count = fields.get("entry", 4), fields.get("count", 0)
        shift = fields.get("shift", 2 if size in (1, 2) else 0)
        raw = self.read(fields["table"], size * count, new)
        if raw is None or len(raw) != size * count or size not in (1, 2, 4, 8):
            return None
        base = fields.get("base", fields["table"])
        signed = size != 8 and not fields.get("unsigned", 0)  # signed unless the line says unsigned
        values = [int.from_bytes(raw[i * size:(i + 1) * size], "little", signed=signed) for i in range(count)]
        return values if size == 8 else [base + (v << shift) for v in values]

    def translate_offset(self, module: str, off: int) -> Result:
        if module not in self.maps:
            return Result(None, module, "module not mapped")
        r = self.maps[module].translate(off)
        return Result(r.new, f"{module}:{r.kind}", r.status)


def _align(v: int) -> int:
    return (v + PAGE - 1) & ~(PAGE - 1)


def _fmt(value: int, digits: str) -> str:
    text = f"{value:0{len(digits)}x}" if digits[:1] == "0" else f"{value:x}"
    return "0x" + (text.upper() if digits.isupper() and not digits.isdigit() else text)


def translate_cfg(text: str, image: ImageMap, partial: bool = False) -> tuple[str, list[tuple[int, int, Result]], bool]:
    """Returns (new text, [(line number, old VA, result)], complete)."""
    rows, out, complete = [], [], True
    for no, line in enumerate(text.splitlines(keepends=True), 1):
        body, hash_, comment = line.partition("#")
        bad = False

        def repl(m: re.Match) -> str:
            nonlocal bad
            va = int(m.group(1), 16)
            if image.module_of(va) is None:
                return m.group(0)
            r = image.translate_va(va)
            rows.append((no, va, r))
            if r.new is None:
                bad = True
                return m.group(0)
            return _fmt(r.new, m.group(1))

        new_body = HEX.sub(repl, body)
        if not bad and body.split()[:1] == ["jumptable"]:
            if _jump_table_fits(body, new_body, image):
                for k, (n, va, r) in enumerate(rows):
                    if n == no and not r.kind.endswith(":text"):
                        rows[k] = (n, va, Result(r.new, r.kind, r.status + ", jump targets checked"))
            else:
                rows.append((no, 0, Result(None, "jumptable", "entries do not lead to the moved targets")))
                bad = True
        if bad:
            complete = False
            out.append(UNRESOLVED_MARK + line if partial else line)
        else:
            out.append(new_body + hash_ + comment)
    return "".join(out), rows, complete


def _jump_fields(line: str) -> dict[str, int]:
    """key=value numbers of a jumptable line; the flags signed/unsigned (config.cpp) as 1, the last one wins."""
    fields: dict[str, int] = {}
    for k, eq, v in (a.partition("=") for a in line.split()[1:]):
        if eq:
            fields[k] = int(v, 0)
        elif k in ("signed", "unsigned"):
            fields.pop("signed", None)
            fields.pop("unsigned", None)
            fields[k] = 1
        else:
            raise ValueError(f"unknown jumptable key {k}")
    return fields


def _jump_table_fits(old_line: str, new_line: str, image: ImageMap) -> bool:
    """Every old target of the table, carried over as code, is what the new table yields."""
    try:
        old_t = image.jump_targets(_jump_fields(old_line), new=False)
        new_t = image.jump_targets(_jump_fields(new_line), new=True)
    except (KeyError, ValueError):
        return False
    if old_t is None or new_t is None:
        return False
    return [image.translate_va(t).new for t in old_t] == new_t


def translate_json(data, image: ImageMap, rows: list) -> bool:
    """Rewrites every {"module", "offset"} table reference in place; False if one stays unresolved."""
    ok = True
    if isinstance(data, dict):
        if isinstance(data.get("module"), str) and isinstance(data.get("offset"), (str, int)):
            raw = data["offset"]
            off = int(raw, 0) if isinstance(raw, str) else raw
            r = image.translate_offset(data["module"], off)
            rows.append((0, off, r))
            if r.new is None:
                ok = False
            else:
                data["offset"] = f"0x{r.new:x}" if isinstance(raw, str) else r.new
        for v in data.values():
            ok = translate_json(v, image, rows) and ok
    elif isinstance(data, list):
        for v in data:
            ok = translate_json(v, image, rows) and ok
    return ok


def _inputs(paths: list[str]) -> list[tuple[str, str]]:
    """(path, path relative to its argument) of every .cfg and derive.json below the arguments."""
    found = []
    for p in paths:
        if os.path.isfile(p):
            found.append((p, os.path.basename(p)))
            continue
        for d, _dirs, names in os.walk(p):
            for n in sorted(names):
                if n.endswith(".cfg") or n == "derive.json":
                    full = os.path.join(d, n)
                    found.append((full, os.path.relpath(full, p)))
    return found


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Carry addresses of cfg/mod/recipe files over to another game build.")
    ap.add_argument("old_exefs")
    ap.add_argument("new_exefs")
    ap.add_argument("inputs", nargs="+", help=".cfg/derive.json files or folders with them (e.g. a kit folder)")
    ap.add_argument("--out", required=True, help="output folder (keeps the relative paths; LGP_PRIVATE_DIR)")
    ap.add_argument("--report", help="TSV of every address (names addresses: LGP_PRIVATE_DIR only)")
    ap.add_argument("--diff", help="version_diff.py --json output of the same two folders (saves the alignment)")
    ap.add_argument("--partial", action="store_true", help="also write files with unresolved addresses (lines marked)")
    ap.add_argument("--modules", default=",".join(DEFAULT_MODULES), help="lifted modules in load order")
    ap.add_argument("--base", type=lambda s: int(s, 0), default=DEFAULT_BASE)
    args = ap.parse_args(argv)
    try:
        diff = None
        if args.diff:
            with open(args.diff, encoding="utf-8") as f:
                diff = json.load(f)
        image = ImageMap(args.old_exefs, args.new_exefs, tuple(args.modules.split(",")), args.base, diff)
    except (inv.FormatError, OSError, ValueError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 2

    report, counts, files_ok, files_bad = [], Counter(), 0, 0
    for path, rel in _inputs(args.inputs):
        with open(path, encoding="utf-8") as f:
            text = f.read()
        rows: list = []
        if path.endswith(".json"):
            data = json.loads(text)
            complete = translate_json(data, image, rows)
            new_text = json.dumps(data, ensure_ascii=False, indent=1) + "\n"
        else:
            new_text, rows, complete = translate_cfg(text, image, args.partial)
        for no, old, r in rows:
            counts[(r.kind, r.status if r.new is not None else "unresolved")] += 1
            report.append((rel, no, f"0x{old:x}", "" if r.new is None else f"0x{r.new:x}", r.kind, r.status))
        files_ok += complete
        files_bad += not complete
        if complete or args.partial:
            dest = os.path.join(args.out, rel)
            os.makedirs(os.path.dirname(dest) or ".", exist_ok=True)
            with open(dest, "w", encoding="utf-8", newline="\n") as f:
                f.write(new_text)
    if args.report:
        with open(args.report, "w", encoding="utf-8", newline="\n") as f:
            f.write("file\tline\told\tnew\tkind\tstatus\n")
            for row in report:
                f.write("\t".join(str(c) for c in row) + "\n")

    print(f"files: {files_ok} complete, {files_bad} with unresolved addresses")
    print("| kind | status | addresses |\n|---|---|---:|")
    for (kind, status), n in sorted(counts.items()):
        print(f"| {kind} | {status} | {n} |")
    return 0 if files_bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
