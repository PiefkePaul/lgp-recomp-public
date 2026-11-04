"""lgp_lift --mods DIR --bad-mods-out FILE: a mod file with a patch site outside the lifted code fails the lift
(exit 1) and is named in FILE; a fitting mod file is not (tools/kit/build.py leaves out only the named ones).

Run: check_bad_mods.py <lgp_lift> <program.nro> <program.elf> <work dir>
"""
import shutil
import subprocess
import sys
from pathlib import Path

lift, nro, elf, work = sys.argv[1:5]
work = Path(work)
shutil.rmtree(work, ignore_errors=True)
mods = work / "mods"
mods.mkdir(parents=True)
(mods / "too-new.cfg").write_text("patch far_away 0x10\n")  # below the module: inside no function
(mods / "fits.cfg").write_text("value something 1\n")
bad = work / "bad-mods.txt"
r = subprocess.run([lift, nro, "-o", str(work / "out.cpp"), "--elf", elf, "--mods", str(mods),
                    "--bad-mods-out", str(bad)], capture_output=True, text=True)
print(r.stderr, end="")
if r.returncode != 1:
    sys.exit(f"FAIL: exit code {r.returncode}, expected 1")
if "(mod file too-new.cfg)" not in r.stderr:
    sys.exit("FAIL: the message does not name the mod file")
names = bad.read_text().split() if bad.is_file() else []
if names != ["too-new.cfg"]:
    sys.exit(f"FAIL: {bad} names {names}, expected ['too-new.cfg']")
print("ok: only too-new.cfg named")
