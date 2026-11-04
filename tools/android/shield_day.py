#!/usr/bin/env python3
"""Steps of the NVIDIA Shield test plan (docs/analysis/33-shield-testplan.md) over adb.

Runs from the PC that holds the game data; nothing leaves it except what adb
pushes to the device. Every step prints what it checked and writes its logs
below --out (default $LGP_PRIVATE_DIR/shield/<step>/<time>). One run at a time.

  shield_day.py check-apk (APK | --manifest AndroidManifest.xml[.in])
  shield_day.py device                                   # plan step 0
  shield_day.py install APK                              # step 4, update only
  shield_day.py push-game EXEFS_DIR ROMFS_BIN [--systemdata DIR]   # step 4
  shield_day.py input [--seconds 180]                    # step 6
  shield_day.py smoke --press FILE [--env K=V ...]       # steps 5 and 5a
  shield_day.py abab --var NAME --order "2 16 16 2" --save-dir DIR [--press FILE]

Rules as on the OnePlus (docs/knowledge/android-ndk-build.md):
- install: only `adb install -r`. Before it, the certificate of the installed
  APK (pulled with `pm path`) and of the new one (apksigner) must match, else
  it stops: a new key would mean uninstalling, which deletes the saves. The
  MD5 of every file below files/saves and files/saves-<n> is taken before and after and must not
  change.
- smoke: --clock virtual (memory and image comparable, fps not), its own save
  folder files/smoke-saves/<time> (empty, or a copy of --save-dir), the app's
  saves checked by MD5; one `dumpsys meminfo` when the log reaches
  --meminfo-at (default 8350, where the Apple TV peaked).
- Before every start the previous lgp.log becomes lgp.prev.log, so a run never
  reads the last run's end.
- abab: --clock host, every run on a fresh device copy of one fixed save
  (files/abab-saves/<tag>/<run>, the game's own saves stay untouched), a run
  only starts below --max-temp (default 35 C); report from
  tools/analyze/perf_run_report.py with one --group per value.

The app splits its "args" extra at whitespace, so --press files and --env
values must not contain spaces. Environment: LGP_ADB (adb executable),
LGP_ADB_SERIAL (e.g. 192.168.1.20:5555), ANDROID_HOME (aapt2 and apksigner
from the newest build-tools).
"""
import argparse
import datetime
import hashlib
import os
import re
import shlex
import subprocess
import sys
import tempfile
import time
import xml.etree.ElementTree as ET

PACKAGE = "org.lgp.recomp.game"
ACTIVITY = "org.lgp.recomp.LgpActivity"
FILES = f"/sdcard/Android/data/{PACKAGE}/files"
ANDROID_NS = "{http://schemas.android.com/apk/res/android}"
REPO = os.path.realpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
MIN_FREE_GIB = 6.0
# The standard system font (pl:u); without it LGP crashes before frame 1 (guest null pointer, address 0x38).
STD_FONT = ("0100000000000811", "nintendo_udsg-r_std_003.bfttf")


# ---- pure helpers (unit tested in tests/tools/test_shield_day.py) ----

def check_manifest(xml_text):
    """Problems that keep the app off Android TV, from AndroidManifest.xml (or its .in template)."""
    root = ET.fromstring(xml_text)
    problems = []
    features = {f.get(ANDROID_NS + "name"): f.get(ANDROID_NS + "required", "true")
                for f in root.iter("uses-feature")}
    if features.get("android.hardware.touchscreen") != "false":
        problems.append("android.hardware.touchscreen is not declared required=false")
    if features.get("android.software.leanback") != "false":
        problems.append("android.software.leanback is not declared required=false (TV and phones)")
    app = root.find("application")
    if app is None or not app.get(ANDROID_NS + "banner"):
        problems.append("application has no android:banner")
    leanback = False
    for activity in root.iter("activity"):
        for f in activity.iter("intent-filter"):
            actions = {a.get(ANDROID_NS + "name") for a in f.iter("action")}
            cats = {c.get(ANDROID_NS + "name") for c in f.iter("category")}
            if "android.intent.action.MAIN" in actions and "android.intent.category.LEANBACK_LAUNCHER" in cats:
                leanback = True
    if not leanback:
        problems.append("no activity with MAIN + LEANBACK_LAUNCHER")
    return problems


def check_badging(text):
    """The same checks on `aapt2 dump badging` output of a built APK."""
    problems = []
    if "leanback-launchable-activity:" not in text:
        problems.append("no leanback-launchable-activity")
    if not re.search(r"^application: .*banner='[^']+'", text, re.M):
        problems.append("application has no banner")
    if re.search(r"^uses-feature: name='android.hardware.touchscreen'", text, re.M):
        problems.append("android.hardware.touchscreen is required")
    if not re.search(r"^native-code: .*'arm64-v8a'", text, re.M):
        problems.append("no arm64-v8a native code")
    return problems


def find_systemdata(exefs, explicit=None):
    """The systemdata folder to push: --systemdata, else systemdata/ next to the exefs folder; None if it has no
    standard font."""
    d = explicit or os.path.join(os.path.dirname(os.path.abspath(exefs)), "systemdata")
    return d if os.path.isfile(os.path.join(d, *STD_FONT)) else None


def cert_sha256(apksigner_text):
    """SHA-256 of the first signer from `apksigner verify --print-certs`."""
    m = re.search(r"certificate SHA-256 digest: ([0-9a-f]{64})", apksigner_text)
    return m.group(1) if m else None


def parse_md5_list(text):
    """`md5sum` lines -> {path: md5}; coreutils marks binary mode with "*" before the name (Windows)."""
    out = {}
    for line in text.splitlines():
        m = re.match(r"([0-9a-f]{32}) [ *](.+)$", line.strip())
        if m:
            out[m.group(2)] = m.group(1)
    return out


def max_temperature(thermal_text, zone_text=""):
    """Highest CPU/GPU/skin temperature in C from `dumpsys thermalservice` ("Current temperatures from HAL"),
    else from "<zone type> <millidegrees>" lines of /sys/class/thermal; None if nothing is readable."""
    values = []
    hal = thermal_text.split("Current temperatures from HAL", 1)
    if len(hal) == 2:
        block = hal[1].split("Current cooling devices", 1)[0]
        for m in re.finditer(r"mValue=(-?[\d.]+), mType=(-?\d+)", block):
            if int(m.group(2)) in (0, 1, 3):  # CPU, GPU, SKIN
                values.append(float(m.group(1)))
    if not values:
        for line in zone_text.splitlines():
            parts = line.split()
            if len(parts) == 2 and re.fullmatch(r"-?\d+", parts[1]):
                v = int(parts[1])
                v = v / 1000.0 if abs(v) > 200 else float(v)
                if 0 < v < 150:
                    values.append(v)
    return max(values) if values else None


def parse_meminfo(text):
    """App summary of `dumpsys meminfo <package>` in MiB: TOTAL PSS, Graphics, GL mtrack, EGL mtrack."""
    out = {}
    m = re.search(r"TOTAL PSS:\s+(\d+)", text) or re.search(r"^\s*TOTAL\s+(\d+)", text, re.M)
    if m:
        out["total pss"] = int(m.group(1)) / 1024.0
    for name in ("Graphics", "GL mtrack", "EGL mtrack"):
        m = re.search(r"^\s*" + re.escape(name) + r":?\s+(\d+)", text, re.M)
        if m:
            out[name.lower()] = int(m.group(1)) / 1024.0
    return out


def last_frame(log_text):
    """Last frame a --time-stats window reached, 0 if none."""
    frames = re.findall(r"time: frames \d+-(\d+):", log_text)
    return int(frames[-1]) if frames else 0


def app_args(args):
    """The "args" extra; refuses words the app would split."""
    for a in args:
        if not a or re.search(r"\s", a):
            raise ValueError(f"argument contains whitespace (the app splits at it): {a!r}")
    return " ".join(args)


def posix_dirname(path):
    return path.rsplit("/", 1)[0] if "/" in path else "."


def frames_in_flight_max(log_text):
    depths = [int(x) for x in re.findall(r"frames in flight \d+ \(max (\d+)\)", log_text)]
    return max(depths) if depths else None


# ---- device side ----

def adb(*args, check=True, capture=True, timeout=None):
    cmd = [os.environ.get("LGP_ADB", "adb")]
    if os.environ.get("LGP_ADB_SERIAL"):
        cmd += ["-s", os.environ["LGP_ADB_SERIAL"]]
    r = subprocess.run(cmd + list(args), stdout=subprocess.PIPE if capture else None,
                       stderr=subprocess.PIPE if capture else None, text=True, timeout=timeout)
    if check and r.returncode != 0:
        sys.exit(f"shield_day: adb {' '.join(args)} failed ({r.returncode}): {(r.stderr or '').strip()}")
    return (r.stdout or "") if capture else ""


def shell(command, check=True, timeout=None):
    return adb("shell", command, check=check, timeout=timeout)


def build_tool(name):
    home = os.environ.get("ANDROID_HOME") or os.environ.get("ANDROID_SDK_ROOT")
    if home:
        bt = os.path.join(home, "build-tools")
        if os.path.isdir(bt):
            def key(v):
                return [int(x) if x.isdigit() else 0 for x in re.split(r"[.-]", v)]
            for version in sorted(os.listdir(bt), key=key, reverse=True):
                for suffix in ("", ".exe", ".bat"):
                    p = os.path.join(bt, version, name + suffix)
                    if os.path.exists(p):
                        return p
    return name


def run_tool(cmd):
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if r.returncode != 0:
        sys.exit(f"shield_day: {' '.join(cmd)} failed:\n{r.stdout}")
    return r.stdout


def out_dir(a, step):
    if a.out:
        d = a.out
    else:
        base = os.environ.get("LGP_PRIVATE_DIR")
        if not base:
            sys.exit("shield_day: LGP_PRIVATE_DIR is not set (logs belong there) and no --out given")
        d = os.path.join(base, "shield", step, datetime.datetime.now().strftime("%Y%m%d-%H%M%S"))
    os.makedirs(d, exist_ok=True)
    return d


class Notes:
    def __init__(self, path):
        self.f = open(path, "a", encoding="utf-8")

    def __call__(self, line):
        print(line)
        self.f.write(line + "\n")
        self.f.flush()


def saves_md5_command(files_dir):
    """Shell command hashing every file of the app's save folders (saves, saves-<n>), paths relative to
    files_dir; the script's own copies (smoke-saves, abab-saves) are not among them."""
    return (f"cd {files_dir} 2>/dev/null && for d in saves saves-*; do "
            f"[ -d \"$d\" ] && find \"$d\" -type f -exec md5sum {{}} +; done; true")


def saves_md5():
    return parse_md5_list(shell(saves_md5_command(FILES), check=False))


def temperature():
    return max_temperature(shell("dumpsys thermalservice", check=False),
                           shell("for z in /sys/class/thermal/thermal_zone*; do "
                                 "echo \"$(cat $z/type) $(cat $z/temp)\"; done 2>/dev/null", check=False))


def app_running():
    return shell(f"pidof {PACKAGE}", check=False).strip() != ""


def require_fonts():
    font = f"{FILES}/game/systemdata/{STD_FONT[0]}/{STD_FONT[1]}"
    if shell(f"[ -f {font} ] && echo yes", check=False).strip() != "yes":
        sys.exit(f"shield_day: STOP: {font} is missing on the device; the game would crash before frame 1. "
                 "Run push-game with the systemdata folder of your dump first.")


def start_app(args):
    require_fonts()
    shell("am force-stop " + PACKAGE, check=False)
    # wait_run must not read the previous run's log (it ends with "exit code").
    shell(f"[ -f {FILES}/logs/lgp.log ] && mv -f {FILES}/logs/lgp.log {FILES}/logs/lgp.prev.log", check=False)
    shell(f"am start -n {PACKAGE}/{ACTIVITY} -e args {shlex.quote(app_args(args))}")


def wait_run(note, timeout_s, on_frame=None):
    """Polls the app's log until the program ends; returns the log text."""
    start = time.time()
    log = ""
    time.sleep(5)
    while True:
        log = shell(f"cat {FILES}/logs/lgp.log", check=False)
        if on_frame:
            on_frame(last_frame(log))
        if "lgp_boot: exit code" in log or not app_running():
            break
        if time.time() - start > timeout_s:
            note(f"   gave up after {timeout_s} s, stopping the app")
            shell("am force-stop " + PACKAGE, check=False)
            break
        time.sleep(10)
    time.sleep(3)
    note(f"   ran {time.time() - start:.0f} s, last frame {last_frame(log)}")
    return shell(f"cat {FILES}/logs/lgp.log", check=False)


def wait_cool(note, max_temp, max_wait_s):
    start = time.time()
    while True:
        t = temperature()
        if t is None:
            note("   temperature not readable, starting anyway")
            return None
        if t < max_temp:
            note(f"   temperature {t:.1f} C")
            return t
        if time.time() - start > max_wait_s:
            note(f"   still {t:.1f} C after {max_wait_s} s, starting anyway (run flagged)")
            return t
        time.sleep(30)


# ---- steps ----

def cmd_check_apk(a):
    if a.manifest:
        with open(a.manifest, encoding="utf-8") as f:
            text = f.read()
        problems = check_manifest(re.sub(r"@[A-Z_]+@", "x", text))
        what = a.manifest
    else:
        problems = check_badging(run_tool([build_tool("aapt2"), "dump", "badging", a.apk]))
        what = a.apk
    for p in problems:
        print(f"MISSING {p}")
    print(f"{what}: {'fit for Android TV' if not problems else f'{len(problems)} problem(s)'}")
    return 1 if problems else 0


def cmd_device(a):
    d = out_dir(a, "device")
    note = Notes(os.path.join(d, "notes.txt"))
    model = shell("getprop ro.product.model").strip()
    abis = shell("getprop ro.product.cpu.abilist").strip()
    note(f"model {model}, Android {shell('getprop ro.build.version.release').strip()}, "
         f"SDK {shell('getprop ro.build.version.sdk').strip()}")
    note(f"abilist {abis}")
    note("meminfo: " + " | ".join(shell("cat /proc/meminfo").splitlines()[:3]))
    note("display: " + " ".join(shell("wm size").split()))
    df = shell("df -k /sdcard").splitlines()
    free_gib = None
    if len(df) >= 2:
        free_gib = int(df[-1].split()[3]) / 1048576.0
    note(f"free on /sdcard: {free_gib:.1f} GiB" if free_gib is not None else "free on /sdcard: unknown")
    t = temperature()
    note(f"temperature {t:.1f} C" if t is not None else "temperature not readable")
    ok = "arm64-v8a" in abis.split(",") and (free_gib is None or free_gib >= MIN_FREE_GIB)
    note("step 0 passed" if ok else f"STOP: needs arm64-v8a and >= {MIN_FREE_GIB} GiB free (plan step 0)")
    return 0 if ok else 1


def cmd_install(a):
    d = out_dir(a, "install")
    note = Notes(os.path.join(d, "notes.txt"))
    signer = build_tool("apksigner")
    new_cert = cert_sha256(run_tool([signer, "verify", "--print-certs", a.apk]))
    note(f"new APK {a.apk}: sha256 {hashlib.sha256(open(a.apk, 'rb').read()).hexdigest()}, certificate {new_cert}")
    if not new_cert:
        sys.exit("shield_day: no certificate in the new APK")
    paths = [l.split(":", 1)[1].strip() for l in shell(f"pm path {PACKAGE}", check=False).splitlines()
             if l.startswith("package:")]
    if paths:
        with tempfile.TemporaryDirectory() as tmp:
            local = os.path.join(tmp, "installed.apk")
            adb("pull", paths[0], local)
            old_cert = cert_sha256(run_tool([signer, "verify", "--print-certs", local]))
        note(f"installed certificate {old_cert}")
        if old_cert != new_cert:
            note("STOP: certificates differ; installing would need an uninstall, which deletes the saves")
            return 1
    else:
        note("not installed yet (first install)")
    before = saves_md5()
    note(f"saves before: {len(before)} files")
    out = adb("install", "-r", a.apk, check=False)
    note("install -r: " + " ".join(out.split()))
    if "Success" not in out:
        return 1
    after = saves_md5()
    changed = sorted(set(before.items()) ^ set(after.items()))
    if changed:
        note(f"SAVES CHANGED by the install: {changed}")
        return 1
    note(f"saves after: {len(after)} files, all MD5 unchanged")
    launcher = shell("cmd package query-activities -a android.intent.action.MAIN "
                     "-c android.intent.category.LEANBACK_LAUNCHER", check=False)
    note("in the TV launcher (LEANBACK_LAUNCHER)" if PACKAGE in launcher
         else "NOT in the LEANBACK_LAUNCHER query (or the query is not supported): check the home screen by eye")
    return 0


def push_dir(local, remote):
    # adb push DIR TARGET copies DIR's contents as TARGET only while TARGET does not exist.
    if shell(f"[ -d {remote} ] && echo yes", check=False).strip() == "yes":
        for name in sorted(os.listdir(local)):
            path = os.path.join(local, name)
            if os.path.isdir(path):
                push_dir(path, f"{remote}/{name}")
            else:
                adb("push", "--sync", path, f"{remote}/{name}", capture=False)
    else:
        adb("push", local, remote, capture=False)


def cmd_push_game(a):
    systemdata = find_systemdata(a.exefs, a.systemdata)
    if not systemdata:
        print(f"shield_day: STOP: no {'/'.join(STD_FONT)} in "
              f"{a.systemdata or os.path.join(os.path.dirname(os.path.abspath(a.exefs)), 'systemdata')}; "
              "the system fonts come from your own console dump (docs/knowledge/horizon-pl-shared-font.md). "
              "Pass --systemdata DIR.")
        return 1
    shell(f"mkdir -p {FILES}/game")
    local = os.path.getsize(a.romfs)
    remote = shell(f"stat -c %s {FILES}/game/romfs.bin 2>/dev/null", check=False).strip()
    if remote == str(local):
        print(f"romfs.bin already there ({local} bytes), not pushed again")
    else:
        adb("push", a.romfs, f"{FILES}/game/romfs.bin", capture=False)
    push_dir(a.exefs, f"{FILES}/game/exefs")
    push_dir(systemdata, f"{FILES}/game/systemdata")
    print(shell(f"ls -l {FILES}/game {FILES}/game/exefs {FILES}/game/systemdata"))
    return 0


def cmd_input(a):
    d = out_dir(a, "input")
    note = Notes(os.path.join(d, "notes.txt"))
    adb("logcat", "-c", check=False)
    start_app(["--app", "--input-log"])
    note(f"app started; press the buttons of plan step 6 now ({a.seconds} s)")
    time.sleep(a.seconds)
    logcat = adb("logcat", "-d", "-s", "SDL", "lgp", check=False)
    log = shell(f"cat {FILES}/logs/lgp.log", check=False)
    with open(os.path.join(d, "logcat.txt"), "w", encoding="utf-8") as f:
        f.write(logcat)
    with open(os.path.join(d, "lgp.log"), "w", encoding="utf-8") as f:
        f.write(log)
    lines = [l for l in (logcat + log).splitlines() if "gamepad" in l or "rumble" in l]
    for l in dict.fromkeys(lines):
        note("   " + l.strip())
    if any("-> slot 1" in l for l in lines):
        note("FINDING: a second slot was taken (the controller must be player 1 with the remote connected)")
    return 0


def cmd_smoke(a):
    d = out_dir(a, "smoke")
    note = Notes(os.path.join(d, "notes.txt"))
    press = open(a.press, encoding="utf-8").read().strip()
    # Its own save folder, so the run never writes the app's saves (Paul's games).
    save = f"smoke-saves/{os.path.basename(d)}"
    if a.save_dir:
        shell(f"mkdir -p {FILES}/smoke-saves")
        adb("push", a.save_dir, f"{FILES}/{save}")
    else:
        shell(f"mkdir -p {FILES}/{save}")
    args = ["--app", "--time-stats", "100", "--clock", "virtual", "--save", save, "--press", press,
            "--frames", str(a.frames)]
    for e in a.env:
        args += ["--env", e]
    note(f"smoke: frames {a.frames}, env {a.env or 'none'}, press sha256 "
         f"{hashlib.sha256(press.encode()).hexdigest()[:16]}")
    wait_cool(note, a.max_temp, a.cool_wait)
    own_saves = saves_md5()
    taken = {}

    def on_frame(frame):
        if "meminfo" not in taken and frame >= a.meminfo_at:
            text = shell(f"dumpsys meminfo {PACKAGE}", check=False)
            with open(os.path.join(d, f"meminfo-{frame}.txt"), "w", encoding="utf-8") as f:
                f.write(text)
            taken["meminfo"] = (frame, parse_meminfo(text))
            note(f"   meminfo at frame {frame}: " + ", ".join(f"{k} {v:.0f} MiB" for k, v in taken["meminfo"][1].items()))

    start_app(args)
    log = wait_run(note, a.timeout, on_frame)
    path = os.path.join(d, "lgp.log")
    with open(path, "w", encoding="utf-8") as f:
        f.write(log)
    t = temperature()
    note(f"   temperature after {t:.1f} C" if t is not None else "   temperature not readable")
    fif = re.search(r"frames in flight: .*", log)
    note(f"   {fif.group(0) if fif else 'FINDING: no frames in flight line (TV not recognised?)'}; "
         f"deepest {frames_in_flight_max(log)}")
    surface = re.findall(r".*(?:surface|swapchain).*\d+ ?x ?\d+.*", log, re.I)
    note(f"   surface: {surface[-1].strip() if surface else 'no line found'}")
    if "meminfo" not in taken:
        note(f"   no meminfo: the run never reached frame {a.meminfo_at}")
    if saves_md5() != own_saves:
        note("WARNING: the app's own saves changed during the run")
    subprocess.run([sys.executable, os.path.join(REPO, "tools", "analyze", "kill_run_report.py"), path])
    return 0


def cmd_abab(a):
    order = a.order.split()
    tag = a.tag or a.var.replace("LGP_WEBGPU_", "").lower().replace("_", "-")
    d = out_dir(a, tag)
    note = Notes(os.path.join(d, "notes.txt"))
    press = open(a.press, encoding="utf-8").read().strip() if a.press else None
    note(f"abab {a.var}: order {' '.join(order)}, frames {a.frames}, max temp {a.max_temp} C, "
         f"save {a.save_dir}, extra {a.extra or 'none'}")
    info = shell(f"dumpsys package {PACKAGE} | grep -m2 -E 'versionName|lastUpdateTime'", check=False)
    note("app: " + " ".join(info.split()))
    own_saves = saves_md5()
    runs = []
    for i, value in enumerate(order, 1):
        label = f"{tag}{value}-{i}"
        save = f"abab-saves/{tag}-{os.path.basename(d)}/{label}"
        note(f"== {label}: {a.var}={value}, save {save}")
        wait_cool(note, a.max_temp, a.cool_wait)
        # A fresh target: adb push copies the folder's contents as that target.
        shell(f"mkdir -p {FILES}/{posix_dirname(save)}")
        adb("push", a.save_dir, f"{FILES}/{save}")
        args = ["--app", "--clock", "host", "--env", f"{a.var}={value}", "--save", save,
                "--frames", str(a.frames), "--time-stats", "100", "--keyboard", "none", "--no-movie-audio",
                "--audio", "none"]
        if press:
            args += ["--press", press]
        args += shlex.split(a.extra)
        start_app(args)
        log = wait_run(note, a.timeout)
        path = os.path.join(d, label + ".log")
        with open(path, "w", encoding="utf-8") as f:
            f.write(log)
        if "nvn: frames" not in log:
            note("   no 'nvn: frames' line: ended early or cut off")
        note(f"   frames in flight max {frames_in_flight_max(log)}; "
             f"lines naming {a.var}: {[l for l in log.splitlines() if a.var in l and 'app: arguments' not in l][:2]}")
        runs.append((value, path))
        if i < len(order):
            time.sleep(a.pause)
    if saves_md5() != own_saves:
        note("WARNING: the game's own saves changed during the runs")
    groups = []
    for value in dict.fromkeys(order):
        logs = ",".join(p for v, p in runs if v == value)
        groups += ["--group", f"{tag}{value}={logs}"]
    rep = subprocess.run([sys.executable, os.path.join(REPO, "tools", "analyze", "perf_run_report.py"), *groups,
                          "--from", str(a.from_frame)], stdout=subprocess.PIPE, text=True)
    with open(os.path.join(d, "report.txt"), "w", encoding="utf-8") as f:
        f.write(rep.stdout)
    print(rep.stdout)
    return rep.returncode


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("--out", help="output folder (default $LGP_PRIVATE_DIR/shield/<step>/<time>)")
    sub = p.add_subparsers(dest="step", required=True)
    s = sub.add_parser("check-apk")
    g = s.add_mutually_exclusive_group(required=True)
    g.add_argument("apk", nargs="?")
    g.add_argument("--manifest")
    sub.add_parser("device")
    s = sub.add_parser("install")
    s.add_argument("apk")
    s = sub.add_parser("push-game")
    s.add_argument("exefs")
    s.add_argument("romfs")
    s.add_argument("--systemdata", help="system data folder of the dump (default: systemdata/ next to EXEFS_DIR)")
    s = sub.add_parser("input")
    s.add_argument("--seconds", type=int, default=180)
    for name in ("smoke", "abab"):
        s = sub.add_parser(name)
        s.add_argument("--press", required=name == "smoke")
        s.add_argument("--frames", type=int, default=10500 if name == "smoke" else 3000)
        s.add_argument("--max-temp", type=float, default=35.0)
        s.add_argument("--cool-wait", type=int, default=1800, help="seconds to wait for --max-temp")
        s.add_argument("--timeout", type=int, default=3600, help="seconds before a run is given up")
    s = sub.choices["smoke"]
    s.add_argument("--env", action="append", default=[], help="KEY=VALUE, repeatable")
    s.add_argument("--meminfo-at", type=int, default=8350)
    s.add_argument("--save-dir", help="local save folder copied to the device for this run (default: empty save)")
    s = sub.choices["abab"]
    s.add_argument("--var", required=True)
    s.add_argument("--order", required=True)
    s.add_argument("--save-dir", required=True)
    s.add_argument("--tag")
    s.add_argument("--pause", type=int, default=180)
    s.add_argument("--from-frame", type=int, default=1)
    s.add_argument("--extra", default="")
    a = p.parse_args(argv)
    return {"check-apk": cmd_check_apk, "device": cmd_device, "install": cmd_install, "push-game": cmd_push_game,
            "input": cmd_input, "smoke": cmd_smoke, "abab": cmd_abab}[a.step](a)


if __name__ == "__main__":
    sys.exit(main())
