#!/usr/bin/env python3
"""Run an Android program on a phone over adb, like a local program (P7.3).

Used as CMAKE_CROSSCOMPILING_EMULATOR for the `android` preset, so
`ctest --preset android` runs the test programs on the connected device:

    adb_run.py <program> [args...]

The program is pushed to $LGP_ADB_DIR (default /data/local/tmp/lgp-run).
Arguments that name an existing host file or directory are pushed too and
replaced by their path on the device; arguments of the form --opt=PATH as
well. The repository root itself is passed unchanged (tests compare it with
the compiled-in source path; pushing the whole tree is never wanted). An
absolute path that does not exist yet but whose directory does (an output
file) becomes a fresh path on the device, below files/out/. Output files the
program writes stay on the device. Environment
variables starting with LGP_ (except LGP_ADB*) are passed on. The exit code
is the program's (adb's shell protocol, Android 7+).

Environment: LGP_ADB (adb executable, default "adb"), LGP_ADB_SERIAL
(device, as for adb -s), LGP_ADB_DIR (directory on the device).
"""
import hashlib
import os
import shlex
import subprocess
import sys


def adb_cmd():
    cmd = [os.environ.get("LGP_ADB", "adb")]
    serial = os.environ.get("LGP_ADB_SERIAL")
    if serial:
        cmd += ["-s", serial]
    return cmd


def push(host_path, device_path):
    r = subprocess.run(adb_cmd() + ["push", "--sync", host_path, device_path],
                       stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
    if r.returncode != 0:
        sys.stderr.write(f"adb_run: push {host_path} failed: {r.stderr}")
        sys.exit(r.returncode or 1)


def device_path_for(base, host_path):
    """Stable place for a host path: same host path, same device path."""
    real = os.path.realpath(host_path)
    tag = hashlib.sha1(real.encode()).hexdigest()[:12]
    return f"{base}/files/{tag}/{os.path.basename(real.rstrip(os.sep)) or 'root'}"


REPO_ROOT = os.path.realpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))


def map_arg(base, arg, pushed):
    prefix, value = "", arg
    if arg.startswith("--") and "=" in arg:
        prefix, value = arg.split("=", 1)
        prefix += "="
    if not value or not os.path.isabs(value) or os.path.realpath(value) == REPO_ROOT:
        return arg
    if not os.path.exists(value):
        if not os.path.isdir(os.path.dirname(value)):
            return arg
        dev = device_path_for(base + "/out", value)
        subprocess.run(adb_cmd() + ["shell", "mkdir", "-p", shlex.quote(os.path.dirname(dev))], check=False)
        return prefix + dev
    if value not in pushed:
        dev = device_path_for(base, value)
        subprocess.run(adb_cmd() + ["shell", "mkdir", "-p", shlex.quote(os.path.dirname(dev))], check=False)
        push(value, dev)
        pushed[value] = dev
    return prefix + pushed[value]


def main():
    if len(sys.argv) < 2:
        sys.stderr.write(__doc__)
        return 2
    base = os.environ.get("LGP_ADB_DIR", "/data/local/tmp/lgp-run")
    program = sys.argv[1]
    subprocess.run(adb_cmd() + ["shell", "mkdir", "-p", base + "/bin"], check=False)
    dev_program = f"{base}/bin/{os.path.basename(program)}"
    push(program, dev_program)
    pushed = {}
    args = [map_arg(base, a, pushed) for a in sys.argv[2:]]
    env = [f"{k}={shlex.quote(v)}" for k, v in sorted(os.environ.items())
           if k.startswith("LGP_") and not k.startswith("LGP_ADB")]
    line = " ".join([f"cd {shlex.quote(base)} &&", "chmod 755", shlex.quote(dev_program), "&&"] + env +
                    [shlex.quote(dev_program)] + [shlex.quote(a) for a in args])
    return subprocess.run(adb_cmd() + ["shell", line]).returncode


if __name__ == "__main__":
    sys.exit(main())
