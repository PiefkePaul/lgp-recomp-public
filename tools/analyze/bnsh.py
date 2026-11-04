#!/usr/bin/env python3
"""Parser for BNSH (NVN shader container) files and an extractor for their shader stages.

Format (little endian, all pointers are 64-bit file offsets; see docs/knowledge/bnsh-format.md):

    0x00  "BNSH" u32 0, u32 version (e.g. 2.1.5), u16 BOM 0xFEFF, u8 alignment shift,
          u8 target address size, u32 file name offset, u16 flags, u16 first block offset,
          u32 relocation table offset, u32 file size
    grsc  (graphics shader container) at the first block offset:
          +0x00 "grsc" u32 next block offset, u32 block size, u32 0
          +0x10 u16 target API type, u16 target API version, u8 target code type, ...
          +0x1C u32 variation count, +0x20 ptr variation array
    variation (0x40 bytes): +0x00 ptr source program, +0x08 ptr intermediate program,
          +0x10 ptr binary program, +0x18 ptr parent container
    program: +0x08..+0x30 one ptr per stage (vertex, hull, domain, geometry, pixel, compute)
    stage:  +0x08 ptr control, +0x10 ptr code, +0x18 u32 code size, +0x1C u32 control size
    code:   graphics stages: 0x30-byte NVN code header (magic 0x12345678), the 0x50-byte shader
            program header (SPH), then the Maxwell code. Compute stages: no header, no SPH; the
            code starts at the control section's entry offset (0 in LGP, 0x100 in newer glslc output).
    control: NVN shader control section (magic 0x98761234); compute fields see
            compute_control() and gpu/nvn abi.hpp (checked on LGP, nvn-hle.md item 11).
    program +0x78: ptr reflection data; its +0x28 ptr compute reflection, whose +0x3C..+0x47 hold
            the workgroup size (layout from the MIT-licensed BNSH.ksy of bnsh-decoder, UNVERIFIED;
            used only to cross-check the control section, `compute` command).

The extractor writes every unique stage as "<sha1>.<stage>.bin" (SPH + code, the input format
of gpu/shader's maxwell2wgsl) plus a manifest and "programs.tsv": every program the containers
define, as the game hands it to nvnProgramSetShaders (input of `lgp_pipeline_seed catalog`):

    graphics <vertex sha1> <pixel sha1 | ->  <variations>
    compute  <sha1> <workgroup x> <y> <z> <shared bytes> <local bytes>  <variations>
    pixel    <sha1>  <occurrences>      (every pixel stage once, for comparisons; no program)

Loose `.bnsh_vsh`/`.bnsh_fsh` files with the same name are linked variation by variation.
"programs-within-file.tsv" (same layout) adds every vertex stage with every pixel stage of the
same file that programs.tsv does not have: candidates for links the game makes at run time. Inputs are the user's local game data and all
outputs are game data: keep them in $LGP_PRIVATE_DIR, never in the repository.

Usage:
    python -I tools/analyze/bnsh.py list <file.bnsh>
    python -I tools/analyze/bnsh.py extract <romfs> --exefs <exefs> --out <dir>
    python -I tools/analyze/bnsh.py compute <romfs> --exefs <exefs>

`compute` prints only counts and sizes (no code): how the compute stages' control sections read
with the layout gpu/nvn assumes, and whether the workgroup sizes match the reflection data.
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

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

STAGES = ("vertex", "hull", "domain", "geometry", "pixel", "compute")
# SPH shader type (word 0, bits 10..13) per BNSH stage slot.
SPH_TYPE = {"vertex": 1, "hull": 2, "domain": 3, "geometry": 4, "pixel": 5}
CODE_MAGIC = 0x12345678
CONTROL_MAGIC = 0x98761234
NVN_CODE_HEADER = 0x30
SPH_BYTES = 0x50
# Control section fields (gpu/nvn/include/lgp/gpu/nvn/abi.hpp, kNvnControl*).
CTL_BLOB_SIZE = 0x704
CTL_ENTRY_OFFSET = 0x708
CTL_STAGE = 0x714
CTL_CS_WORKGROUP = 0x748
CTL_CS_SHARED = 0x754
CTL_CS_LOCAL_LO = 0x758
CTL_CS_LOCAL_HI = 0x75C
CTL_CS_CRS = 0x760
CTL_CS_BARRIERS = 0x764
CTL_CS_END = 0x768
NVN_STAGE_COMPUTE = 5
# Loose shader files: one stage per file, partner in the file with the same name.
LOOSE_EXTS = {".bnsh_vsh": "vertex", ".bnsh_fsh": "pixel"}
# BNSH reflection (UNVERIFIED, see the module docstring).
PROGRAM_REFLECTION = 0x78
REFLECTION_COMPUTE = 0x28
REFLECTION_WORKGROUP = 0x3C


class BnshError(ValueError):
    pass


@dataclass
class Stage:
    kind: str
    code: bytes        # without the NVN code header: SPH + Maxwell code (compute: code only)
    control: bytes
    file_offset: int
    compute: dict | None = None          # compute_control() of a compute stage
    skipped_zero: bool | None = None     # compute: were the bytes before the entry offset all zero?
    reflection_workgroup: tuple | None = None


@dataclass
class Program:
    variation: int
    stages: dict[str, Stage] = field(default_factory=dict)


@dataclass
class Bnsh:
    version: tuple[int, int, int]
    api_type: int
    api_version: int
    code_type: int
    programs: list[Program]


def _u16(b: bytes, o: int) -> int:
    return struct.unpack_from("<H", b, o)[0]


def _u32(b: bytes, o: int) -> int:
    return struct.unpack_from("<I", b, o)[0]


def _u64(b: bytes, o: int) -> int:
    return struct.unpack_from("<Q", b, o)[0]


def compute_control(ctrl: bytes) -> dict | None:
    """Compute fields of a control section, or None if it is too short or of another stage."""
    if len(ctrl) < CTL_CS_END or _u32(ctrl, 0) != CONTROL_MAGIC or _u32(ctrl, CTL_STAGE) != NVN_STAGE_COMPUTE:
        return None
    return {
        "entry": _u32(ctrl, CTL_ENTRY_OFFSET),
        "blob": _u32(ctrl, CTL_BLOB_SIZE),
        "workgroup": list(struct.unpack_from("<3I", ctrl, CTL_CS_WORKGROUP)),
        "shared": _u32(ctrl, CTL_CS_SHARED),
        "local": _u32(ctrl, CTL_CS_LOCAL_LO),
        "local_hi": _u32(ctrl, CTL_CS_LOCAL_HI),
        "crs": _u32(ctrl, CTL_CS_CRS),
        "barriers": _u32(ctrl, CTL_CS_BARRIERS),
    }


def _reflection_workgroup(data: bytes, prog_off: int) -> tuple | None:
    """Workgroup size from the program's reflection data, None if it does not read plausibly."""
    try:
        refl = _u64(data, prog_off + PROGRAM_REFLECTION)
        if not refl or refl + REFLECTION_COMPUTE + 8 > len(data):
            return None
        cs = _u64(data, refl + REFLECTION_COMPUTE)
        if not cs or cs + REFLECTION_WORKGROUP + 12 > len(data):
            return None
        wg = struct.unpack_from("<3I", data, cs + REFLECTION_WORKGROUP)
    except struct.error:
        return None
    return wg if all(1 <= v <= 1024 for v in wg) else None


def parse(data: bytes) -> Bnsh:
    if len(data) < 0x20 or data[:4] != b"BNSH" or _u32(data, 4) != 0:
        raise BnshError("not a BNSH file")
    if _u16(data, 0x0C) != 0xFEFF:
        raise BnshError("unsupported byte order mark")
    ver = _u32(data, 8)
    size = _u32(data, 0x1C)
    if size > len(data):
        raise BnshError(f"file size field 0x{size:x} exceeds data 0x{len(data):x}")
    data = data[:size]

    def ptr(o: int) -> int:
        p = _u64(data, o)
        if p >= size:
            raise BnshError(f"pointer 0x{p:x} at 0x{o:x} outside the file")
        return p

    g = _u16(data, 0x16)
    if data[g:g + 4] != b"grsc":
        raise BnshError(f"no grsc block at 0x{g:x}")
    api_type, api_version, code_type = _u16(data, g + 0x10), _u16(data, g + 0x12), data[g + 0x14]
    count = _u32(data, g + 0x1C)
    varr = ptr(g + 0x20)
    if varr + count * 0x40 > size:
        raise BnshError("variation array runs past the end")
    programs = []
    for v in range(count):
        base = varr + v * 0x40
        if _u64(data, base + 0x18) != g:
            raise BnshError(f"variation {v}: parent pointer does not point at grsc")
        prog_off = _u64(data, base + 0x10)
        prog = Program(variation=v)
        if prog_off:
            if prog_off >= size:
                raise BnshError(f"variation {v}: binary program outside the file")
            for i, kind in enumerate(STAGES):
                so = _u64(data, prog_off + 8 + 8 * i)
                if not so:
                    continue
                if so + 0x20 > size:
                    raise BnshError(f"variation {v} {kind}: stage outside the file")
                ctrl, code = ptr(so + 0x08), ptr(so + 0x10)
                code_size, ctrl_size = _u32(data, so + 0x18), _u32(data, so + 0x1C)
                if code + code_size > size or ctrl + ctrl_size > size:
                    raise BnshError(f"variation {v} {kind}: code/control runs past the end")
                has_header = code_size >= NVN_CODE_HEADER and _u32(data, code) == CODE_MAGIC
                if not has_header and kind != "compute":
                    # Graphics stages always carry the header; compute code starts right away.
                    raise BnshError(f"variation {v} {kind}: missing NVN code header")
                if ctrl_size < 4 or _u32(data, ctrl) != CONTROL_MAGIC:
                    raise BnshError(f"variation {v} {kind}: missing NVN control section")
                start, end = (NVN_CODE_HEADER if has_header else 0), code_size
                info = compute_control(data[ctrl:ctrl + ctrl_size]) if kind == "compute" else None
                skipped_zero = None
                if info is not None and not has_header:
                    # The driver starts compute code at the entry offset; the blob size bounds it.
                    if info["entry"] % 8 == 0 and info["entry"] + 0x20 <= code_size:
                        start = info["entry"]
                        skipped_zero = not any(data[code:code + start])
                    if start < info["blob"] <= code_size:
                        end = info["blob"]
                body = data[code + start:code + end]
                if kind in SPH_TYPE:
                    if len(body) < SPH_BYTES:
                        raise BnshError(f"variation {v} {kind}: code shorter than an SPH")
                    w0 = _u32(body, 0)
                    if (w0 >> 10) & 15 != SPH_TYPE[kind]:
                        raise BnshError(f"variation {v} {kind}: SPH shader type {(w0 >> 10) & 15} does not match")
                prog.stages[kind] = Stage(kind, body, data[ctrl:ctrl + ctrl_size], code, info, skipped_zero,
                                          _reflection_workgroup(data, prog_off) if kind == "compute" else None)
        programs.append(prog)
    return Bnsh(((ver >> 16) & 0xFFFF, (ver >> 8) & 0xFF, ver & 0xFF), api_type, api_version, code_type, programs)


def stage_id(stage: Stage) -> str:
    return hashlib.sha1(stage.code).hexdigest()


def extract(sources, out_dir: str) -> dict:
    """sources: iterable of (source type, path, bytes). Writes unique stages; returns a report."""
    import shader_inventory as si
    os.makedirs(out_dir, exist_ok=True)
    stages: dict[str, dict] = {}
    pairs: dict[str, Counter] = defaultdict(Counter)  # vertex id -> pixel ids it is drawn with
    programs: Counter = Counter()  # programs.tsv lines without the count
    containers = Counter()
    errors = Counter()
    parsed: dict[bytes, list[dict[str, str]] | None] = {}  # container -> stage ids per variation
    loose: dict[str, dict[str, list[dict[str, str]]]] = defaultdict(dict)  # file stem -> kind -> variations
    per_file: dict[str, dict[str, set]] = defaultdict(lambda: {"vertex": set(), "pixel": set()})
    for kind, path, data in sources:
        ext = os.path.splitext(path)[1].lower()
        for _off, blob in si.nx_containers(data, b"BNSH"):
            containers["found"] += 1
            h = hashlib.sha1(blob).digest()
            if h not in parsed:
                parsed[h] = None
                containers["unique"] += 1
                try:
                    b = parse(blob)
                except (BnshError, struct.error, IndexError) as e:
                    errors[str(e).split(":")[0] if ":" in str(e) else str(e)] += 1
                    continue
                containers["parsed"] += 1
                parsed[h] = [_add_program(prog, kind, out_dir, stages, pairs, programs, errors)
                             for prog in b.programs]
            elif parsed[h] is not None:
                for ids in parsed[h]:
                    for sid in ids.values():
                        stages[sid]["sources"][kind] += 1
            # Loose cutscene shaders keep vertex and pixel stage in two files with the same
            # name (.bnsh_vsh, .bnsh_fsh): the game links them, so they form programs too.
            if parsed[h] is not None and ext in LOOSE_EXTS:
                loose[os.path.splitext(path)[0]].setdefault(LOOSE_EXTS[ext], parsed[h])
            for ids in parsed[h] or ():
                for k in ("vertex", "pixel"):
                    if k in ids:
                        per_file[path][k].add(ids[k])
    linked = Counter()
    for stem, kinds in loose.items():
        vs, ps = kinds.get("vertex"), kinds.get("pixel")
        if vs is None or ps is None:
            linked["files without their partner"] += 1
            continue
        if len(vs) != len(ps):
            linked["file pairs with different variation counts"] += 1
        for v, p in zip(vs, ps):
            if set(v) == {"vertex"} and set(p) == {"pixel"}:
                programs[f"graphics\t{v['vertex']}\t{p['pixel']}"] += 1
                linked["programs"] += 1
    # Experiment: every vertex stage with every pixel stage of the same file (model archive,
    # effect file), as the game may link them across variations. Not programs the containers
    # define: a separate file, lgp_pipeline_seed catalog --programs programs-within-file.tsv.
    within = Counter()
    for path, kinds in per_file.items():
        for v in kinds["vertex"]:
            for p in kinds["pixel"]:
                line = f"graphics\t{v}\t{p}"
                if line not in programs:
                    within[line] += 1
    with open(os.path.join(out_dir, "programs-within-file.tsv"), "w", encoding="utf-8", newline="\n") as f:
        for line in sorted(within):
            f.write(f"{line}\t{within[line]}\n")
    # Every pixel stage once (lgp_pipeline_seed translates them alone to explain seed programs
    # the catalog does not have).
    for sid, e in stages.items():
        if e["stage"] == "pixel":
            programs[f"pixel\t{sid}"] += sum(e["sources"].values())
    manifest = {}
    for sid, e in stages.items():
        m = {"stage": e["stage"], "size": e["size"], "sources": dict(e["sources"])}
        if "compute" in e:
            m["compute"] = e["compute"]
        if sid in pairs:
            m["pixel"] = pairs[sid].most_common(1)[0][0]
            m["pixel_variants"] = len(pairs[sid])
        manifest[sid] = m
    with open(os.path.join(out_dir, "manifest.json"), "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=0)
    with open(os.path.join(out_dir, "programs.tsv"), "w", encoding="utf-8", newline="\n") as f:
        for line in sorted(programs):
            f.write(f"{line}\t{programs[line]}\n")
    by_stage = Counter(m["stage"] for m in manifest.values())
    return {"containers": dict(containers), "parse_errors": dict(errors), "stages": dict(by_stage),
            "stages_total": len(manifest), "programs": dict(Counter(k.split("\t")[0] for k in programs)),
            "loose_vsh_fsh": dict(linked), "within_file_combinations": len(within)}


def _add_program(prog: Program, kind: str, out_dir: str, stages: dict, pairs: dict, programs: Counter,
                 errors: Counter) -> dict[str, str]:
    """Writes the stages of one variation (once each) and counts its program; returns its stage ids."""
    ids = {}
    for st in prog.stages.values():
        sid = stage_id(st)
        ids[st.kind] = sid
        e = stages.get(sid)
        if e is None:
            e = stages[sid] = {"stage": st.kind, "size": len(st.code), "sources": Counter()}
            if st.compute is not None:
                e["compute"] = st.compute
            with open(os.path.join(out_dir, f"{sid}.{st.kind}.bin"), "wb") as f:
                f.write(st.code)
        e["sources"][kind] += 1
    if "vertex" in ids and "pixel" in ids:
        pairs[ids["vertex"]][ids["pixel"]] += 1
    if "vertex" in ids:
        programs[f"graphics\t{ids['vertex']}\t{ids.get('pixel', '-')}"] += 1
    elif "compute" in ids:
        c = prog.stages["compute"].compute
        if c is not None and not c["local_hi"]:
            wg = "\t".join(map(str, c["workgroup"]))
            programs[f"compute\t{ids['compute']}\t{wg}\t{c['shared']}\t{c['local']}"] += 1
        else:
            errors["compute stage without a readable control section"] += 1
    return ids


def compute_report(sources) -> dict:
    """Counts over all unique compute stages: does the control layout gpu/nvn assumes read
    plausibly, and does its workgroup size match the reflection data? Numbers only."""
    import shader_inventory as si
    seen, stages = set(), {}
    for _kind, _path, data in sources:
        for _off, blob in si.nx_containers(data, b"BNSH"):
            h = hashlib.sha1(blob).digest()
            if h in seen:
                continue
            seen.add(h)
            try:
                b = parse(blob)
            except (BnshError, struct.error, IndexError):
                continue
            for prog in b.programs:
                st = prog.stages.get("compute")
                if st is not None:
                    # Control, code and reflection together: programs with the same control
                    # section but other code or reflection are separate stages.
                    key = hashlib.sha1(st.control + st.code + repr(st.reflection_workgroup).encode()).digest()
                    stages.setdefault(key, st)
    rep = Counter()
    hist = defaultdict(Counter)
    for st in stages.values():
        rep["compute_stages"] += 1
        c = st.compute
        if c is None:
            rep["control_not_compute_layout"] += 1
            continue
        rep["control_readable"] += 1
        hist["entry_offset"][hex(c["entry"])] += 1
        hist["workgroup"]["x".join(map(str, c["workgroup"]))] += 1
        hist["shared_bytes"][c["shared"]] += 1
        hist["local_bytes"][c["local"]] += 1
        hist["barriers"][c["barriers"]] += 1
        if c["local_hi"]:
            rep["local_hi_nonzero"] += 1
        if st.skipped_zero is not None:
            rep["prefix_zero" if st.skipped_zero else "prefix_nonzero"] += 1
        wg, threads = c["workgroup"], c["workgroup"][0] * c["workgroup"][1] * c["workgroup"][2]
        rep["workgroup_plausible" if 1 <= threads <= 1024 and wg[2] <= 64 else "workgroup_implausible"] += 1
        if st.reflection_workgroup is None:
            rep["reflection_missing"] += 1
        elif tuple(wg) == tuple(st.reflection_workgroup):
            rep["reflection_match"] += 1
        else:
            rep["reflection_mismatch"] += 1
    return {"counts": dict(rep), **{k: {str(a): n for a, n in v.most_common()} for k, v in hist.items()}}


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=(__doc__ or "").split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    lp = sub.add_parser("list")
    lp.add_argument("file")
    ep = sub.add_parser("extract")
    ep.add_argument("romfs")
    ep.add_argument("--exefs")
    ep.add_argument("--out", required=True)
    cp = sub.add_parser("compute")
    cp.add_argument("romfs")
    cp.add_argument("--exefs")
    a = ap.parse_args(argv)
    if a.cmd == "list":
        with open(a.file, "rb") as f:
            b = parse(f.read())
        print(f"version {'.'.join(map(str, b.version))}, API type {b.api_type} v{b.api_version}, "
              f"code type {b.code_type}, {len(b.programs)} variations")
        for p in b.programs:
            print(f"  variation {p.variation}: " +
                  ", ".join(f"{s.kind} {len(s.code)} B" for s in p.stages.values()))
        return 0
    import nso_inventory as ni
    import shader_inventory as si
    images = {n: m.image for n, m in ni.load_exefs(a.exefs).items()} if a.exefs else {}
    if a.cmd == "compute":
        json.dump(compute_report(si.iter_sources(a.romfs, images)), sys.stdout, indent=1)
        print()
        return 0
    rep = extract(si.iter_sources(a.romfs, images), a.out)
    json.dump(rep, sys.stdout, indent=1)
    print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
