#!/usr/bin/env python3
"""Cross-check lgp_recomp (C++) against tools/analyze/nso_inventory.py (Python) on synthetic
NSO/NRO files written by the C++ loader test. Both must agree on segments, module id,
symbol counts and relocation counts (acceptance criterion P2.1: relocations match the
reference).

Usage:
  test_vs_reference.py <lgp_recomp_test_loader> <lgp_recomp> <nso_inventory.py>
  test_vs_reference.py --files <module>... -- <lgp_recomp> <nso_inventory.py>   (e.g. the homebrew fixtures)
"""
import json
import os
import subprocess
import sys
import tempfile


def main() -> int:
    failures = 0
    with tempfile.TemporaryDirectory() as tmp:
        if sys.argv[1] == "--files":
            sep = sys.argv.index("--")
            files = sys.argv[2:sep]
            cli, inventory = sys.argv[sep + 1:sep + 3]
        else:
            test_bin, cli, inventory = sys.argv[1:4]
            env = dict(os.environ, LGP_SYNTH_OUT=tmp)
            subprocess.run([test_bin, "write_synthetic_images"], check=True, env=env, stdout=subprocess.DEVNULL)
            files = [os.path.join(tmp, "synthetic.nso"), os.path.join(tmp, "synthetic.nro")]
        for path in files:
            name = os.path.basename(path)
            cpp_json = os.path.join(tmp, "cpp.json")
            subprocess.run([cli, "info", path, "--json", cpp_json], check=True, stdout=subprocess.DEVNULL)
            with open(cpp_json) as f:
                cpp = json.load(f)["modules"][0]
            ref_json = os.path.join(tmp, "ref.json")
            subprocess.run([sys.executable, "-I", inventory, path, "--json", ref_json], check=True,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            with open(ref_json) as f:
                ref = json.load(f)["modules"][0]

            def check(label, a, b):
                nonlocal failures
                if a != b:
                    print(f"MISMATCH {name} {label}: cpp={a!r} ref={b!r}")
                    failures += 1

            for seg in cpp["segments"]:
                if seg["kind"] == "bss":
                    check("bss size", int(seg["size"], 16), ref["bss_size"])
                    continue
                r = ref["segments"][seg["kind"]]
                check(f".{seg['kind']} size", int(seg["size"], 16), r["mem_size"])
                check(f".{seg['kind']} offset", int(seg["offset"], 16), r["mem_offset"])
                check(f".{seg['kind']} compressed", seg["compressed"], r["compressed"])
            check("image_size", int(cpp["image_size"], 16), ref["image_size"])
            check("module_id", cpp["module_id"].rstrip("0"), ref["module_id"].rstrip("0"))
            check("internal_name", cpp["internal_name"], ref["ro_path_basename"])
            check("api_info", cpp["api_info"], ref["api_info"])
            check("dynsym total", cpp["dynsym"]["total"], ref["dynsym_count"])
            check("dynsym undefined", cpp["dynsym"]["undefined"], ref["import_count"])
            check("exports", len(cpp["exports"]), ref["export_count"])
            check("fde_count", cpp["fde_count"], ref["fde_count"])
            check("eh_frame_hdr_size", cpp["eh_frame_hdr_size"], ref["eh_frame_hdr_size"])
            ref_rc = ref["reloc_counts"]
            for rtype in ("RELATIVE", "ABS64", "GLOB_DAT", "JUMP_SLOT"):
                check(f"reloc {rtype}", cpp["relocations"].get(rtype, 0), ref_rc.get(rtype, 0))
            # RELR is only counted by parser versions after PR #9; compare when present.
            for key in ("RELR", "RELR_RELATIVE"):
                if key in ref_rc:
                    check("reloc RELR", cpp["relocations"]["RELR_RELATIVE"], ref_rc[key])
            print(f"ok {name}: relocations {cpp['relocations']}, {cpp['dynsym']['total']} dynsym, {cpp['fde_count']} FDEs")
    if failures:
        print(f"{failures} mismatches")
        return 1
    print("reference parser and C++ loader agree")
    return 0


if __name__ == "__main__":
    sys.exit(main())
