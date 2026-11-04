#!/usr/bin/env python3
"""NVN usage inventory of a Switch ExeFS (static, optionally joined with a Ryujinx PTC profile).

Answers for ADR-0003 (graphics HLE vs. LLE):

1. Does the game bind NVN by name via ``nvnBootstrapLoader`` / ``nvnDeviceGetProcAddress``?
   Detected by the NVN function-name strings in ``main`` and the loader pattern
   ``ADRP/ADD x1, "<name>" ; BLR getProcAddress ; ADRP/LDR xN, [GOT] ; STR x0, [xN]``
   (the inlined ``nvnLoadCProcs`` from the public NVN headers).
2. Which NVN functions does ``main`` reference outside that loader?  Counted as ADRP+LDR
   loads of the function-pointer slots' GOT entries (static, upper bound of what can run).
3. Which NVN functions actually ran?  The NVN driver in ``sdk`` keeps a sorted name table
   (without the ``nvn`` prefix) and a parallel table of implementation pointers; joining
   those with a Ryujinx PTC profile (translated guest functions) gives the executed set.
4. Was the GLSL compiler (``subsdk0`` = glslc) ever called?  Executed ``subsdk0`` functions
   are mapped to its exports.

Output: counts and public NVN/glslc API names only, no code bytes or addresses of game code
beyond module-relative offsets of SDK entry points.

Usage:
    python tools/analyze/nvn_inventory.py <exefs_dir> [--ptc-profile X.info --ryujinx-log run.log]
        [--json out.json] [--markdown out.md]
"""
from __future__ import annotations

import argparse
import json
import os
import re
import struct
import sys
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nso_inventory as ni  # noqa: E402
import ryujinx_cache as rc  # noqa: E402

R_RELATIVE = 1027
NVN_NAME_RE = re.compile(rb"(?<=\0)nvn[A-Z][A-Za-z0-9_]+(?=\0)")
SDK_NAME_RE = re.compile(rb"[A-Z][A-Za-z0-9]+")


# --- ARM64 address-materialisation scanner -------------------------------------------------

def _sx(v: int, bits: int) -> int:
    return v - (1 << bits) if (v >> (bits - 1)) & 1 else v


def iter_adrp_refs(text: bytes, window: int = 64):
    """Yield (pc, kind, target, rt) for ADRP followed by ADD/LDR/STR (64-bit, unsigned imm)
    on the same base register within ``window`` instructions.

    kind is "add" (address), "ldr" or "str" (memory operand). A pending ADRP page is dropped
    when its register is overwritten, on unconditional B and on RET.
    """
    n = len(text) // 4
    words = struct.unpack_from(f"<{n}I", text, 0)
    pend: dict[int, tuple[int, int]] = {}
    for i, w in enumerate(words):
        pc = i * 4
        if (w & 0x9F000000) == 0x90000000:  # ADRP
            imm = ((w >> 29) & 3) | (((w >> 5) & 0x7FFFF) << 2)
            pend[w & 31] = ((pc & ~0xFFF) + (_sx(imm, 21) << 12), pc)
            continue
        if pend:
            rn = (w >> 5) & 31
            if rn in pend:
                page, adrp_pc = pend[rn]
                if pc - adrp_pc > window * 4:
                    del pend[rn]
                elif (w & 0xFFC00000) == 0x91000000:  # ADD Xd, Xn, #imm12
                    yield pc, "add", page + ((w >> 10) & 0xFFF), w & 31
                elif (w & 0xFFC00000) == 0xF9400000:  # LDR Xt, [Xn, #imm12*8]
                    yield pc, "ldr", page + ((w >> 10) & 0xFFF) * 8, w & 31
                elif (w & 0xFFC00000) == 0xF9000000:  # STR Xt, [Xn, #imm12*8]
                    yield pc, "str", page + ((w >> 10) & 0xFFF) * 8, w & 31
            # Any instruction except a store writes Rd/Rt (bits 0-4).
            is_store = (w >> 25) & 5 == 4 and not (w >> 22) & 1
            if not is_store:
                pend.pop(w & 31, None)
            if (w & 0xFC000000) == 0x14000000 or w == 0xD65F03C0:  # B, RET
                pend.clear()


def branch_targets(text: bytes) -> Counter:
    """Counter of BL/B targets (module offsets) -> number of call sites."""
    n = len(text) // 4
    out: Counter = Counter()
    for i, w in enumerate(struct.unpack_from(f"<{n}I", text, 0)):
        if (w & 0x7C000000) == 0x14000000:  # B / BL
            out[i * 4 + _sx(w & 0x3FFFFFF, 26) * 4] += 1
    return out


def relocations(mod: ni.NsoModule) -> dict[int, tuple[int, int, int]]:
    """offset -> (type, symbol index, addend) for RELA and JMPREL."""
    out = {}
    img = mod.image
    for off_tag, size_tag in ((ni.DT_RELA, ni.DT_RELASZ), (ni.DT_JMPREL, ni.DT_PLTRELSZ)):
        if off_tag in mod.dynamic and size_tag in mod.dynamic:
            off, size = mod.dynamic[off_tag][0], mod.dynamic[size_tag][0]
            for k in range(size // 24):
                o, info, addend = struct.unpack_from("<QQq", img, off + k * 24)
                out[o] = (info & 0xFFFFFFFF, info >> 32, addend)
    return out


# --- main: NVN names and loader slots ------------------------------------------------------

def nvn_names(image: bytes) -> dict[int, str]:
    """Image offset -> NUL-terminated ``nvn*`` string."""
    return {m.start(): m.group().decode() for m in NVN_NAME_RE.finditer(image)}


def loader_slots(text_refs: list, names: dict[int, str], image: bytes) -> tuple[dict[str, int], set[int]]:
    """Map NVN name -> GOT entry of its function-pointer slot from the inlined proc loader.

    Returns (name -> GOT offset, set of pcs of the loader's slot loads).
    """
    refs = sorted(text_refs)
    ldr_after_store = []
    for pc, kind, target, rt in refs:
        if kind == "ldr" and pc + 8 <= len(image):
            nxt = struct.unpack_from("<I", image, pc + 4)[0]
            if nxt == (0xF9000000 | (rt << 5)):  # STR X0, [Xrt]
                ldr_after_store.append((pc, target))
    store_pcs = [pc for pc, _ in ldr_after_store]
    import bisect
    slots: dict[str, int] = {}
    loader_pcs: set[int] = set()
    for pc, kind, target, _ in refs:
        if kind != "add" or target not in names:
            continue
        k = bisect.bisect_right(store_pcs, pc)
        if k < len(ldr_after_store) and ldr_after_store[k][0] - pc <= 0x20:
            name = names[target]
            if name not in slots:
                slots[name] = ldr_after_store[k][1]
                loader_pcs.add(ldr_after_store[k][0])
    return slots, loader_pcs


def slot_uses(text_refs: list, slots: dict[str, int], loader_pcs: set[int]) -> Counter:
    got_to_name = {g: n for n, g in slots.items()}
    uses: Counter = Counter()
    for pc, kind, target, _ in text_refs:
        if kind == "ldr" and target in got_to_name and pc not in loader_pcs:
            uses[got_to_name[target]] += 1
    return uses


# --- sdk: NVN driver proc table -------------------------------------------------------------

def sdk_proc_table(sdk: ni.NsoModule, wanted: set[str]) -> list[tuple[str, int]]:
    """[(nvn name, implementation offset in sdk)] from the driver's sorted name block and the
    parallel RELATIVE pointer table.

    Assumption (verified for NVN in SDK 5.4): the name block lists every name of ``wanted``
    (without the ``nvn`` prefix) in sorted order, and the longest 8-byte-stride run of
    RELATIVE relocations into .text has entry i = implementation of name i.
    """
    img = sdk.image
    bare = {w[3:] for w in wanted}
    block, best = [], []
    pos = 0
    for m in re.finditer(rb"[A-Za-z0-9]+", img):
        word = m.group().decode()
        if word in bare and img[m.end():m.end() + 1] == b"\0" and m.start() == pos:
            block.append(word)
        else:
            if len(block) > len(best):
                best = block
            block = [word] if word in bare and img[m.end():m.end() + 1] == b"\0" else []
        pos = m.end() + 1
    if len(block) > len(best):
        best = block
    if len(best) < len(bare) * 0.9:
        raise ni.FormatError(f"sdk: NVN name block not found ({len(best)} of {len(bare)} names)")
    relocs = relocations(sdk)
    text_size = sdk.seg("text").mem_size
    ptrs = sorted((o, a) for o, (t, _, a) in relocs.items() if t == R_RELATIVE and 0 <= a < text_size)
    runs, cur = [], [ptrs[0]] if ptrs else []
    for p in ptrs[1:]:
        if p[0] == cur[-1][0] + 8:
            cur.append(p)
        else:
            runs.append(cur)
            cur = [p]
    runs.append(cur)
    table = max(runs, key=len)
    if len(table) < len(best):
        raise ni.FormatError(f"sdk: pointer table too short ({len(table)} < {len(best)})")
    return [("nvn" + name, table[i][1]) for i, name in enumerate(best)]


# --- report ---------------------------------------------------------------------------------

def analyze(exefs: str, ptc_profile: str | None = None, ryujinx_log: str | None = None) -> dict:
    mods = ni.load_exefs(exefs)
    main, sdk = mods["main"], mods["sdk"]
    names = nvn_names(main.image)
    text = main.image[:main.seg("text").mem_size]
    refs = list(iter_adrp_refs(text))
    slots, loader_pcs = loader_slots(refs, names, main.image)
    uses = slot_uses(refs, slots, loader_pcs)
    all_names = set(names.values()) - {"nvnBootstrapLoader"}
    rep: dict = {
        "nvn_name_strings_in_main": len(all_names),
        "loader_slots": len(slots),
        "imports_bootstrap": any(s.name == "nvnBootstrapLoader" and not s.defined for s in main.symbols),
        "static_used": sorted(n for n in slots if uses[n]),
        "static_use_sites": dict(uses),
    }
    table = sdk_proc_table(sdk, all_names)
    rep["sdk_proc_table_entries"] = len(table)
    sdk_calls = branch_targets(sdk.image[:sdk.seg("text").mem_size])
    rep["sdk_internal_callers"] = {n: sdk_calls[off] for n, off in table if sdk_calls[off]}
    if ptc_profile:
        if not ryujinx_log:
            raise SystemExit("--ptc-profile needs --ryujinx-log (module load addresses)")
        _, prof = rc.read_ptc_profile(ptc_profile)
        bases = rc.module_bases_from_log(ryujinx_log)
        rep["ptc_functions"] = len(prof)
        per_mod = Counter()
        for addr in prof:
            for name, base in bases.items():
                if name in mods and base <= addr < base + mods[name].seg("text").mem_size:
                    per_mod[name] += 1
                    break
        rep["ptc_functions_per_module"] = dict(per_mod)
        rep["runtime_executed"] = sorted(n for n, off in table if bases["sdk"] + off in prof)
        if "subsdk0" in mods:
            sub = mods["subsdk0"]
            exports = {s.value: s.name for s in sub.symbols if s.defined and s.type == 2}
            ran = sorted(a - bases["subsdk0"] for a in prof
                         if bases["subsdk0"] <= a < bases["subsdk0"] + sub.seg("text").mem_size)
            rep["subsdk0_executed_functions"] = len(ran)
            rep["subsdk0_executed_exports"] = sorted(exports[a] for a in ran if a in exports)
            rep["subsdk0_exports"] = sorted(exports.values())
    return rep


def to_markdown(rep: dict) -> str:
    out = ["# NVN inventory", ""]
    out.append(f"- NVN names in main: {rep['nvn_name_strings_in_main']}, loader slots: {rep['loader_slots']}, "
               f"bootstrap imported: {rep['imports_bootstrap']}")
    out.append(f"- statically referenced (outside the loader): {len(rep['static_used'])}")
    if "runtime_executed" in rep:
        st, rt = set(rep["static_used"]), set(rep["runtime_executed"])
        out.append(f"- executed at runtime (PTC): {len(rt)}; both: {len(st & rt)}; "
                   f"runtime only: {len(rt - st)}; static only: {len(st - rt)}")
        out.append(f"- subsdk0: {rep.get('subsdk0_executed_functions')} functions executed, "
                   f"of these exports: {', '.join(rep.get('subsdk0_executed_exports', [])) or '–'}")
        out += ["", "| function | static sites | executed |", "|---|---:|:---:|"]
        for n in sorted(st | rt):
            out.append(f"| `{n}` | {rep['static_use_sites'].get(n, 0)} | {'yes' if n in rt else '–'} |")
    else:
        out += ["", "| function | static sites |", "|---|---:|"]
        for n in rep["static_used"]:
            out.append(f"| `{n}` | {rep['static_use_sites'][n]} |")
    return "\n".join(out) + "\n"


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=(__doc__ or "").split("\n")[0])
    ap.add_argument("exefs")
    ap.add_argument("--ptc-profile")
    ap.add_argument("--ryujinx-log")
    ap.add_argument("--json")
    ap.add_argument("--markdown")
    a = ap.parse_args(argv)
    rep = analyze(a.exefs, a.ptc_profile, a.ryujinx_log)
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
