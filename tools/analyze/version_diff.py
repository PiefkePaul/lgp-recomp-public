#!/usr/bin/env python3
"""Compare two versions of a game: ExeFS modules function by function, RomFS file by file.

Meant for "what does update X change": both inputs are directories extracted from the
user's own dumps, and every output that names addresses or files belongs in
$LGP_PRIVATE_DIR. Only the counts printed on stdout are meant to be shared.

ExeFS:
    For every module both versions have, the build IDs, segment sizes and segment
    hashes are compared. Every module is split into functions at the starts listed in
    .eh_frame_hdr (a region runs up to the next start, so padding and literal pools
    count with the function before them). Each function is hashed with the
    address-dependent bits masked: branch/ADR/ADRP/literal immediates whose target lies
    outside the function, and the low 12 bits that an ADD/LDR/STR adds to a register
    that still holds an ADRP page. A function that only moved, or only references moved
    data, therefore keeps its hash; a changed branch inside it does not.
    Limit: which data an ADRP/ADR/literal reference points to is not checked (that
    would need a map of .ro/.data as well). A function that only reads another
    object than before counts as unchanged; its address in --map is still right. The two sequences of hashes
    are then aligned in address order (unique hashes as anchors, difflib in between).

    Results per function: same (same address and body), moved (same body, other
    address), changed (aligned against a different body, or a branch out of it now
    lands in another function than the alignment predicts), added, removed.
    --map writes old -> new start addresses of every same/moved function as TSV, which
    lets addresses from mod or patch files be carried over to the new version.

RomFS:
    Files are compared by size and SHA-256 (both trees are read once; several GB).

Usage:
    python3 tools/analyze/version_diff.py exefs <old_exefs> <new_exefs> [--json out.json] [--map out.tsv]
    python3 tools/analyze/version_diff.py romfs <old_romfs> <new_romfs> [--json out.json]
"""
from __future__ import annotations

import argparse
import bisect
import difflib
import hashlib
import json
import os
import struct
import sys
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nso_inventory as inv  # noqa: E402

# ---------------------------------------------------------------- functions


def function_starts(mod: inv.NsoModule) -> list[int]:
    """Function start offsets (image-relative) from the .eh_frame_hdr search table."""
    img = mod.image
    mod0 = struct.unpack_from("<I", img, 4)[0]
    if mod0 + 0x1C > len(img) or img[mod0:mod0 + 4] != b"MOD0":
        return []
    eh_s, eh_e = struct.unpack_from("<2i", img, mod0 + 0x10)
    return eh_frame_hdr_starts(img, mod0 + eh_s, eh_e - eh_s)


def eh_frame_hdr_starts(img: bytes, hdr: int, size: int) -> list[int]:
    if size < 12 or hdr < 0 or hdr + 12 > len(img) or img[hdr] != 1:
        return []
    ptr_enc, cnt_enc, table_enc = img[hdr + 1], img[hdr + 2], img[hdr + 3]
    # Only the layout every SDK module uses: udata4 count, datarel sdata4 table.
    if ptr_enc & 0x0F not in (0x03, 0x0B) or cnt_enc != 0x03 or table_enc != 0x3B:
        return []
    count = struct.unpack_from("<I", img, hdr + 8)[0]
    if hdr + 12 + count * 8 > len(img):
        return []
    starts = {hdr + struct.unpack_from("<i", img, hdr + 12 + k * 8)[0] for k in range(count)}
    return sorted(s for s in starts if 0 <= s < len(img))


def _sext(v: int, bits: int) -> int:
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


def _pc_target(w: int) -> tuple[int, int] | None:
    """(word offset of the target, mask that keeps everything but the immediate) of a PC-relative insn."""
    if w & 0x7C000000 == 0x14000000:          # B, BL
        return _sext(w & 0x3FFFFFF, 26), 0xFC000000
    if w & 0xFF000010 == 0x54000000:          # B.cond
        return _sext((w >> 5) & 0x7FFFF, 19), 0xFF00001F
    if w & 0x7E000000 == 0x34000000:          # CBZ, CBNZ
        return _sext((w >> 5) & 0x7FFFF, 19), 0xFF00001F
    if w & 0x7E000000 == 0x36000000:          # TBZ, TBNZ
        return _sext((w >> 5) & 0x3FFF, 14), 0xFFF8001F
    if w & 0x9F000000 == 0x10000000:          # ADR (byte offset; a target inside the function is word-aligned)
        imm = _sext((((w >> 5) & 0x7FFFF) << 2) | ((w >> 29) & 3), 21)
        return (imm // 4 if imm % 4 == 0 else 1 << 30), 0x9F00001F
    if w & 0x3B000000 == 0x18000000:          # LDR (literal), PRFM (literal)
        return _sext((w >> 5) & 0x7FFFF, 19), 0xFF00001F
    return None


def mask_insn(w: int, adrp_regs: set[int], pos: int = 0, count: int = 0) -> int:
    """The instruction word with every bit that depends on code or data placement cleared.

    pos/count: index of the word and length of its function in words. PC-relative targets inside the
    function keep their immediate (moving the function does not change them); targets outside are masked.
    adrp_regs holds the registers that still carry an ADRP page; the low 12 bits added to them are masked.
    """
    pc = _pc_target(w)
    if pc is not None:
        if w & 0xFC000000 == 0x94000000:      # BL clobbers the caller-saved registers
            adrp_regs.clear()
        elif w & 0x1F000000 == 0x10000000 or w & 0x3B000000 == 0x18000000:  # ADR, LDR (literal) write Rt
            adrp_regs.discard(w & 31)
        off, keep = pc
        return w if 0 <= pos + off < count else w & keep
    if w & 0x9F000000 == 0x90000000:          # ADRP
        adrp_regs.add(w & 31)
        return w & 0x9F00001F
    rn, rd = (w >> 5) & 31, w & 31
    out = w
    if rn in adrp_regs and (w & 0x7F800000 == 0x11000000      # ADD (immediate), 32/64-bit
                            or w & 0x3B000000 == 0x39000000):  # LDR/STR (unsigned offset)
        out = w & 0xFFC003FF
    if w & 0xFFFFFC1F == 0xD63F0000:          # BLR
        adrp_regs.clear()
    elif w & 0x3B400000 != 0x39000000:        # anything but a STR (unsigned offset) may write Rd/Rt
        adrp_regs.discard(rd)
        if w & 0x3A400000 == 0x28400000:      # LDP/LDNP also write Rt2
            adrp_regs.discard((w >> 10) & 31)
    return out


def _is_branch(w: int) -> bool:
    return (w & 0x7C000000 == 0x14000000 or w & 0xFF000010 == 0x54000000
            or w & 0x7E000000 == 0x34000000 or w & 0x7E000000 == 0x36000000)


def function_scan(code: bytes, base: int = 0) -> tuple[str, list[int]]:
    """Masked hash of a function and the addresses its branches leave it for (B/BL/B.cond/CBZ/TBZ)."""
    adrp_regs: set[int] = set()
    words = list(struct.unpack_from(f"<{len(code) // 4}I", code))
    while words and words[-1] == 0:  # alignment padding before the next function
        words.pop()
    n = len(words)
    masked = struct.pack(f"<{n}I", *(mask_insn(w, adrp_regs, k, n) for k, w in enumerate(words)))
    targets = []
    for k, w in enumerate(words):
        if _is_branch(w):
            off = _pc_target(w)[0]
            if not 0 <= k + off < n:
                targets.append(base + 4 * (k + off))
    return hashlib.sha1(masked).hexdigest(), targets


def function_hash(code: bytes) -> str:
    return function_scan(code)[0]


def split_functions(text: bytes, text_base: int, starts: list[int]) -> list[tuple[int, int, str, list[int]]]:
    """(start, size, hash, outgoing branch targets) for every start inside .text; a region ends at the next start."""
    end = text_base + len(text)
    inside = [s for s in starts if text_base <= s < end]
    out = []
    for k, s in enumerate(inside):
        e = inside[k + 1] if k + 1 < len(inside) else end
        out.append((s, e - s, *function_scan(text[s - text_base:e - text_base], s)))
    return out


def check_branch_targets(rows: list[dict], old: list, new: list) -> None:
    """Mark same/moved functions as changed when a branch out of them now lands somewhere else.

    The hash masks targets outside the function, so a call retargeted to another function would
    look like a relocation. Each old target is carried over through the alignment (start of the
    containing function, plus the offset when that function itself is unchanged) and compared.
    """
    pair = {r["old"]: r for r in rows if r["old"] is not None and r["new"] is not None}
    starts = [f[0] for f in old]
    t_old = {f[0]: f[3] for f in old if len(f) > 3}
    t_new = {f[0]: f[3] for f in new if len(f) > 3}
    for r in rows:
        if r["status"] not in ("same", "moved") or r["old"] not in t_old:
            continue
        for ta, tb in zip(t_old[r["old"]], t_new[r["new"]]):
            k = bisect.bisect_right(starts, ta) - 1
            if k < 0:
                continue  # before the first function (e.g. a stub): nothing to compare against
            fs = starts[k]
            target = pair.get(fs)
            if target is None:
                expected = None  # the callee is gone
            elif ta == fs or target["status"] in ("same", "moved"):
                expected = target["new"] + (ta - fs)
            else:
                continue  # into the middle of a changed function: offsets can differ legitimately
            if tb != expected:
                r["status"] = "changed"
                r["retargeted"] = True
                break


def align_functions(old: list[tuple[int, int, str]], new: list[tuple[int, int, str]]) -> list[dict]:
    """Pair the functions of two versions in address order."""
    ha, hb = [f[2] for f in old], [f[2] for f in new]
    ca, cb = Counter(ha), Counter(hb)
    pos_b = {h: k for k, h in enumerate(hb) if cb[h] == 1}
    # Anchors: hashes unique in both, kept only while increasing in both sequences.
    anchors = []
    for i, h in enumerate(ha):
        if ca[h] == 1 and h in pos_b and (not anchors or pos_b[h] > anchors[-1][1]):
            anchors.append((i, pos_b[h]))
    anchors.append((len(ha), len(hb)))

    rows: list[dict] = []

    def emit(status: str, a: int | None, b: int | None) -> None:
        fa = old[a] if a is not None else None
        fb = new[b] if b is not None else None
        if status == "equal":
            status = "same" if fa[0] == fb[0] else "moved"
        rows.append({"status": status,
                     "old": fa[0] if fa else None, "old_size": fa[1] if fa else None,
                     "new": fb[0] if fb else None, "new_size": fb[1] if fb else None})

    ia = ib = 0
    for xa, xb in anchors:
        sm = difflib.SequenceMatcher(None, ha[ia:xa], hb[ib:xb], autojunk=False)
        for tag, a0, a1, b0, b1 in sm.get_opcodes():
            if tag == "equal":
                for k in range(a1 - a0):
                    emit("equal", ia + a0 + k, ib + b0 + k)
            else:
                paired = min(a1 - a0, b1 - b0) if tag == "replace" else 0
                for k in range(paired):
                    emit("changed", ia + a0 + k, ib + b0 + k)
                for k in range(a0 + paired, a1):
                    emit("removed", ia + k, None)
                for k in range(b0 + paired, b1):
                    emit("added", None, ib + k)
        if xa < len(ha):
            emit("equal", xa, xb)
        ia, ib = xa + 1, xb + 1
    return rows


# ---------------------------------------------------------------- modules


def _seg_bytes(mod: inv.NsoModule, name: str) -> bytes:
    s = mod.seg(name)
    return mod.image[s.mem_offset:s.mem_offset + s.mem_size]


def diff_module(old: inv.NsoModule, new: inv.NsoModule) -> dict:
    res: dict = {
        "name": old.name,
        "module_id": {"old": inv._trim_zero_bytes(old.module_id), "new": inv._trim_zero_bytes(new.module_id)},
        "identical": old.file_sha256 == new.file_sha256,
        "api_info": {"old": old.api_info, "new": new.api_info},
        "segments": {},
        "bss_size": {"old": old.bss_size, "new": new.bss_size},
    }
    for name in ("text", "ro", "data"):
        a, b = _seg_bytes(old, name), _seg_bytes(new, name)
        res["segments"][name] = {"old_size": len(a), "new_size": len(b), "equal": a == b}
    # Also with identical code: --map must list every unchanged function of every module.
    ta, tb = old.seg("text"), new.seg("text")
    fa = split_functions(_seg_bytes(old, "text"), ta.mem_offset, function_starts(old))
    fb = split_functions(_seg_bytes(new, "text"), tb.mem_offset, function_starts(new))
    rows = align_functions(fa, fb)
    check_branch_targets(rows, fa, fb)
    counts = Counter(r["status"] for r in rows)
    res["functions"] = {"old_count": len(fa), "new_count": len(fb),
                        **{k: counts.get(k, 0) for k in ("same", "moved", "changed", "added", "removed")}}
    res["function_rows"] = rows
    return res


def diff_exefs(old_dir: str, new_dir: str) -> dict:
    old, new = inv.load_exefs(old_dir), inv.load_exefs(new_dir)
    out: dict = {"modules": [], "only_old": sorted(set(old) - set(new)), "only_new": sorted(set(new) - set(old))}
    for name in inv.LOAD_ORDER:
        if name in old and name in new:
            out["modules"].append(diff_module(old[name], new[name]))
    npdm = [os.path.join(d, "main.npdm") for d in (old_dir, new_dir)]
    if all(os.path.isfile(p) for p in npdm):
        out["npdm_equal"] = _file_sha256(npdm[0]) == _file_sha256(npdm[1])
    return out


def write_map(result: dict, path: str) -> None:
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("module\told\tnew\tsize\tstatus\n")
        for m in result["modules"]:
            for r in m.get("function_rows", []):
                if r["status"] in ("same", "moved"):
                    f.write(f"{m['name']}\t0x{r['old']:x}\t0x{r['new']:x}\t0x{r['new_size']:x}\t{r['status']}\n")


def exefs_summary(result: dict) -> str:
    out = ["| module | identical | build ID old → new | .text | .ro | .data | functions old/new | same | moved "
           "| changed | new | removed |",
           "|---|---|---|---|---|---|---|---:|---:|---:|---:|---:|"]
    for m in result["modules"]:
        seg = m["segments"]

        def cell(n: str) -> str:
            s = seg[n]
            if s["equal"]:
                return "="
            return f"0x{s['old_size']:x} → 0x{s['new_size']:x}" if s["old_size"] != s["new_size"] else "≠ (same size)"

        f = m.get("functions")
        fc = (f"{f['old_count']:,}/{f['new_count']:,} | {f['same']:,} | {f['moved']:,} | {f['changed']:,} "
              f"| {f['added']:,} | {f['removed']:,}") if f else "– | – | – | – | – | –"
        ids = m["module_id"]
        idc = f"`{ids['old']}`" if ids["old"] == ids["new"] else f"`{ids['old'][:8]}…` → `{ids['new'][:8]}…`"
        out.append(f"| {m['name']} | {'yes' if m['identical'] else 'no'} | {idc} | {cell('text')} | {cell('ro')} "
                   f"| {cell('data')} | {fc} |")
    for key, label in (("only_old", "old only"), ("only_new", "new only")):
        if result[key]:
            out.append(f"\n{label}: {', '.join(result[key])}")
    if "npdm_equal" in result:
        out.append(f"\nmain.npdm identical: {'yes' if result['npdm_equal'] else 'no'}")
    return "\n".join(out) + "\n"


# ---------------------------------------------------------------- RomFS


def _file_sha256(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def _native(path: str) -> str:
    """Windows: an extended-length path, so files named like devices (NUL, CON, ...) stay files."""
    path = os.path.abspath(path)
    return "\\\\?\\" + path if os.name == "nt" and not path.startswith("\\\\?\\") else path


def _walk(root: str) -> dict[str, int]:
    root = _native(root)
    files = {}
    for d, _dirs, names in os.walk(root):
        for n in names:
            p = os.path.join(d, n)
            files[p[len(root):].lstrip(os.sep).replace(os.sep, "/")] = os.path.getsize(p)
    return files


def diff_romfs(old_dir: str, new_dir: str) -> dict:
    a, b = _walk(old_dir), _walk(new_dir)
    old_dir, new_dir = _native(old_dir), _native(new_dir)
    changed, same = [], 0
    for rel in sorted(set(a) & set(b)):
        native = rel.replace("/", os.sep)  # extended-length paths take no forward slashes
        if a[rel] != b[rel] or _file_sha256(os.path.join(old_dir, native)) != _file_sha256(os.path.join(new_dir, native)):
            changed.append({"path": rel, "old_size": a[rel], "new_size": b[rel]})
        else:
            same += 1
    return {
        "old_count": len(a), "new_count": len(b), "same": same,
        "changed": changed,
        "added": [{"path": p, "size": b[p]} for p in sorted(set(b) - set(a))],
        "removed": [{"path": p, "size": a[p]} for p in sorted(set(a) - set(b))],
    }


def romfs_summary(result: dict) -> str:
    per_dir: dict[str, Counter] = {}
    for kind in ("changed", "added", "removed"):
        for e in result[kind]:
            top = e["path"].split("/", 1)[0] if "/" in e["path"] else "."
            per_dir.setdefault(top, Counter())[kind] += 1
    out = [f"files old {result['old_count']:,}, new {result['new_count']:,}, same {result['same']:,}, "
           f"changed {len(result['changed']):,}, added {len(result['added']):,}, removed {len(result['removed']):,}"]
    if per_dir:
        out += ["", "| folder (top level) | changed | added | removed |", "|---|---:|---:|---:|"]
        for top in sorted(per_dir):
            c = per_dir[top]
            out.append(f"| {top} | {c['changed']} | {c['added']} | {c['removed']} |")
    return "\n".join(out) + "\n"


# ---------------------------------------------------------------- CLI


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Compare ExeFS or RomFS of two game versions.")
    ap.add_argument("kind", choices=("exefs", "romfs"))
    ap.add_argument("old")
    ap.add_argument("new")
    ap.add_argument("--json", help="write the full result (addresses/paths: keep it in LGP_PRIVATE_DIR)")
    ap.add_argument("--map", help="exefs only: TSV of old -> new start of every unchanged function")
    args = ap.parse_args(argv)
    try:
        if args.kind == "exefs":
            result = diff_exefs(args.old, args.new)
            summary = exefs_summary(result)
            if args.map:
                write_map(result, args.map)
        else:
            result = diff_romfs(args.old, args.new)
            summary = romfs_summary(result)
    except (inv.FormatError, OSError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 2
    if args.json:
        with open(args.json, "w", encoding="utf-8") as f:
            json.dump(result, f, indent=1)
    sys.stdout.write(summary)
    return 0


if __name__ == "__main__":
    sys.exit(main())
