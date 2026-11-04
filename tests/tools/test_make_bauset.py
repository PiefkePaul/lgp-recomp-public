"""Unit test for tools/kit/make_bauset.py: a tiny repo and made-up tool ZIPs over file:// (no download).

Run: python -m unittest -v test_make_bauset (from tests/tools)
"""
import hashlib
import os
import subprocess
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest import mock

HERE = os.path.dirname(__file__)
sys.path.insert(0, os.path.join(HERE, "..", "..", "tools", "kit"))
import build as kb  # noqa: E402
import make_bauset as mb  # noqa: E402


def write_zip(path, files):
    with zipfile.ZipFile(path, "w") as z:
        for name, data in files.items():
            z.writestr(name, data)
    return hashlib.sha256(path.read_bytes()).hexdigest()


class MakeBauset(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.repo = self.tmp / "repo"
        self.repo.mkdir()
        (self.repo / "lgp-bauen.cmd").write_bytes(b"@echo off\r\n")
        (self.repo / "lgp-bauen.command").write_text("#!/bin/sh\n")
        (self.repo / "untracked.txt").write_text("not in the package")
        git = ["git", "-C", str(self.repo), "-c", "user.name=t", "-c", "user.email=t@t", "-c", "core.hooksPath=/dev/null"]
        subprocess.run(git[:3] + ["init", "-q"], check=True)
        subprocess.run(git + ["add", "lgp-bauen.cmd", "lgp-bauen.command"], check=True)
        subprocess.run(git + ["update-index", "--chmod=+x", "lgp-bauen.command"], check=True)
        subprocess.run(git + ["commit", "-q", "-m", "x"], check=True)
        src = self.tmp / "src"
        src.mkdir()
        sha_py = write_zip(src / "py.zip", {"python.exe": b"p"})
        sha_cm = write_zip(src / "cm.zip", {"cm-1/bin/cmake.exe": b"c", "cm-1/bin/cmake-gui.exe": b"g",
                                             "cm-1/man/x.1": b"m", "other/y": b"o"})
        self.tools = [("python", (src / "py.zip").as_uri(), sha_py, None, (), "PSF"),
                      ("cmake", (src / "cm.zip").as_uri(), sha_cm, "cm-1", ("bin/cmake-gui.exe", "man/"), "BSD")]

    def test_package_has_repo_tools_and_readme(self):
        package = mb.make(self.tmp / "out", self.tmp / "cache", self.repo, self.tools)
        self.assertRegex(package.name, r"^lgp-bauset-windows-x64-[0-9a-f]{10}\.zip$")
        with zipfile.ZipFile(package) as z:
            names = set(z.namelist())
            readme = z.read("README.txt").decode()
        self.assertEqual(names, {"README.txt", "lgp-recomp/lgp-bauen.cmd", "lgp-recomp/lgp-bauen.command",
                                 "lgp-recomp/bauset/python/python.exe",
                                 "lgp-recomp/bauset/cmake/bin/cmake.exe"})
        self.assertIn("licence BSD", readme)
        self.assertIn("\r\n", readme)
        # The stage folder is gone, the download cache stays for the next run.
        self.assertEqual([p.name for p in (self.tmp / "out").iterdir() if p.is_dir()], [])
        self.assertTrue((self.tmp / "cache" / "py.zip").is_file())

    def test_kit_files_go_into_the_package_and_no_tools_leaves_bauset_out(self):
        kit = self.tmp / "mods-package"
        (kit / "mods" / "wild-battles").mkdir(parents=True)
        (kit / "game.cfg").write_text("c")
        (kit / "mods" / "wild-battles.cfg").write_text("w")
        (kit / "mods" / "wild-battles" / "derive.json").write_text("{}")
        (kit / "notes.txt").write_text("not for the package")
        package = mb.make(self.tmp / "out", self.tmp / "cache", self.repo, [], kit)
        self.assertRegex(package.name, r"^lgp-build-kit-[0-9a-f]{10}\.zip$")
        with zipfile.ZipFile(package) as z:
            names = set(z.namelist())
        self.assertEqual(names, {"lgp-recomp/lgp-bauen.cmd", "lgp-recomp/lgp-bauen.command", "lgp-recomp/kit-files/game.cfg",
                                 "lgp-recomp/kit-files/mods/wild-battles.cfg",
                                 "lgp-recomp/kit-files/mods/wild-battles/derive.json"})

    def test_release_package_refuses_the_private_repository(self):
        """A release package once came from the private repository (commit hash of main in the name): it would have
        shipped the decryption and the internal notes. With --kit-files only the public snapshot is accepted."""
        (self.repo / "tools" / "extract").mkdir(parents=True)
        (self.repo / "tools" / "extract" / "x.py").write_text("x")
        (self.repo / "CLAUDE.md").write_text("private")
        git = ["git", "-C", str(self.repo), "-c", "user.name=t", "-c", "user.email=t@t", "-c", "core.hooksPath=/dev/null"]
        subprocess.run(git + ["add", "-A"], check=True)
        subprocess.run(git + ["commit", "-q", "-m", "private"], check=True)
        kit = self.tmp / "mods-package"
        kit.mkdir()
        (kit / "game.cfg").write_text("c")
        with self.assertRaises(SystemExit) as e:
            mb.make(self.tmp / "out", self.tmp / "cache", self.repo, [], kit)
        self.assertIn("CLAUDE.md, tools/extract/", str(e.exception))
        self.assertFalse((self.tmp / "out").exists())
        # Without kit files (the CI package) the repository may be the private one.
        mb.make(self.tmp / "out", self.tmp / "cache", self.repo, [])

    def test_private_kit_package(self):
        """--private-kit (the kit for friends, lgp-kit): the dump unpacking may come along, the internal notes may not,
        and the name says lgp-kit so the package is never taken for a public one."""
        git = ["git", "-C", str(self.repo), "-c", "user.name=t", "-c", "user.email=t@t", "-c", "core.hooksPath=/dev/null"]
        (self.repo / "tools" / "extract").mkdir(parents=True)
        (self.repo / "tools" / "extract" / "x.py").write_text("x")
        subprocess.run(git + ["add", "-A"], check=True)
        subprocess.run(git + ["commit", "-q", "-m", "kit"], check=True)
        kit = self.tmp / "mods-package"
        kit.mkdir()
        (kit / "game.cfg").write_text("c")
        package = mb.make(self.tmp / "out", self.tmp / "cache", self.repo, [], kit, private_kit=True)
        self.assertTrue(package.name.startswith("lgp-kit-"), package.name)
        with zipfile.ZipFile(package) as z:
            self.assertIn("lgp-recomp/tools/extract/x.py", z.namelist())
            self.assertIn("lgp-recomp/kit-files/game.cfg", z.namelist())
        (self.repo / "CLAUDE.md").write_text("private")
        subprocess.run(git + ["add", "-A"], check=True)
        subprocess.run(git + ["commit", "-q", "-m", "private"], check=True)
        with self.assertRaises(SystemExit) as e:
            mb.make(self.tmp / "out2", self.tmp / "cache", self.repo, [], kit, private_kit=True)
        self.assertIn("CLAUDE.md", str(e.exception))
        self.assertFalse((self.tmp / "out2").exists())

    def test_kit_files_of_other_builds_come_along(self):
        other = next(b for b in mb.kb.SUPPORTED_BUILDS if b != mb.kb.MAIN_BUILD_ID)
        kit = self.tmp / "mods-package"
        (kit / other / "mods").mkdir(parents=True)
        (kit / "game.cfg").write_text("c")
        (kit / other / "game.cfg").write_text("o")
        (kit / other / "mods" / "wild-battles.cfg").write_text("w")
        (kit / other / "pipeline-seed.lgppipes").write_text("seed stays out")
        package = mb.make(self.tmp / "out", self.tmp / "cache", self.repo, [], kit)
        with zipfile.ZipFile(package) as z:
            names = {n for n in z.namelist() if "/kit-files/" in n}
        self.assertEqual(names, {"lgp-recomp/kit-files/game.cfg", f"lgp-recomp/kit-files/{other}/game.cfg",
                                 f"lgp-recomp/kit-files/{other}/mods/wild-battles.cfg"})

    def test_scripts_stay_executable(self):
        """Codex P1 on #543: the Mac double-clicks lgp-bauen.command, so its 0o755 from git must reach the ZIP,
        also when the package is made on Windows (no executable bit on disk)."""
        real_unpack = mb.unpack

        def windows_unpack(archive, dest, strip, skip):  # a Windows file system reports 0o777 for every file
            modes = real_unpack(archive, dest, strip, skip)
            for p in dest.rglob("*"):
                if p.is_file():
                    p.chmod(0o777)
            return modes

        with mock.patch.object(mb.os, "name", "nt"), mock.patch.object(mb, "unpack", windows_unpack), \
                mock.patch.object(sys, "platform", "win32"):  # ZipInfo picks DOS as creator system then
            package = mb.make(self.tmp / "out", self.tmp / "cache", self.repo, [])
        with zipfile.ZipFile(package) as z:
            mode = {i.filename: (i.external_attr >> 16) & 0o777 for i in z.infolist()}
            systems = {i.create_system for i in z.infolist()}
        self.assertEqual(mode["lgp-recomp/lgp-bauen.command"], 0o755)
        self.assertEqual(mode["lgp-recomp/lgp-bauen.cmd"], 0o644)
        self.assertEqual(systems, {3}, "unzip ignores Unix modes of entries that say DOS")

    def test_entries_are_not_in_the_future_anywhere(self):
        """Mac VM 2026-11-03: entries in the packer's local time (CET) lay an hour ahead for a Mac in UTC, and Ninja
        stopped with "build.ninja still dirty". Every entry gets the commit's day minus one, whatever the clock says."""
        import calendar
        import time as _time
        package = mb.make(self.tmp / "out", self.tmp / "cache", self.repo, [])
        with zipfile.ZipFile(package) as z:
            stamps = {i.date_time for i in z.infolist()}
        self.assertEqual(len(stamps), 1, stamps)
        commit = int(subprocess.run(["git", "-C", str(self.repo), "log", "-1", "--format=%ct"], capture_output=True,
                                    text=True, check=True).stdout)
        stamp = stamps.pop()
        day = _time.gmtime(commit - 86400)
        self.assertEqual(stamp, (day.tm_year, day.tm_mon, day.tm_mday, 0, 0, 0))
        # Read as local time in UTC-12, the furthest behind, the entry is still before the commit
        self.assertLess(calendar.timegm(stamp) + 12 * 3600, commit)

    def test_kit_files_without_game_cfg_stop(self):
        (self.tmp / "empty").mkdir()
        with self.assertRaises(SystemExit) as e:
            mb.make(self.tmp / "out", self.tmp / "cache", self.repo, [], self.tmp / "empty")
        self.assertIn("no game.cfg", str(e.exception))

    def test_every_entry_has_a_unix_mode(self):
        """PC2 on af7e388: README.txt, kit-files and the tools under bauset/ still said DOS (-rw-rw-rw- on unzip)."""
        kit = self.tmp / "mods-package"
        (kit / "mods").mkdir(parents=True)
        (kit / "game.cfg").write_text("c")
        (kit / "mods" / "a.cfg").write_text("a")
        real_from_file = zipfile.ZipInfo.from_file

        def dos_from_file(*args, **kwargs):  # what ZipInfo does on Windows (sys.platform would break file:// here)
            info = real_from_file(*args, **kwargs)
            info.create_system = 0
            info.external_attr = 0x20
            return info

        with mock.patch.object(zipfile.ZipInfo, "from_file", staticmethod(dos_from_file)):
            package = mb.make(self.tmp / "out", self.tmp / "cache", self.repo, self.tools, kit)
        with zipfile.ZipFile(package) as z:
            entries = z.infolist()
        self.assertEqual({i.create_system for i in entries}, {3})
        mode = {i.filename: (i.external_attr >> 16) & 0o777 for i in entries}
        for name in ("README.txt", "lgp-recomp/kit-files/game.cfg", "lgp-recomp/bauset/python/python.exe"):
            self.assertEqual(mode[name], 0o644, name)
        self.assertEqual(mode["lgp-recomp/lgp-bauen.command"], 0o755)

    def test_wrong_checksum_stops(self):
        tools = [self.tools[0][:2] + ("0" * 64,) + self.tools[0][3:]]
        with self.assertRaises(SystemExit) as e:
            mb.make(self.tmp / "out", self.tmp / "cache", self.repo, tools)
        self.assertIn("SHA-256", str(e.exception))
        self.assertFalse(any((self.tmp / "cache").glob("*")))


class BausetTools(unittest.TestCase):
    def test_build_py_prefers_the_package(self):
        bauset = Path(tempfile.mkdtemp())
        (bauset / "ninja").mkdir()
        (bauset / "ninja" / kb.exe_name("ninja")).write_bytes(b"")
        (bauset / kb.LLVM_MINGW[1]).mkdir()
        with mock.patch.object(kb, "BAUSET", bauset), mock.patch.dict(os.environ, {"LGP_KIT_TOOLS": ""}):
            self.assertEqual(kb.bauset_dirs(), [str(bauset / "ninja")])
            self.assertEqual(kb.find_tool("ninja"), str(bauset / "ninja" / kb.exe_name("ninja")))
            self.assertEqual(kb.tool_cache(), bauset)
            path = kb.with_tool_dirs({"PATH": "/usr/bin"})["PATH"].split(os.pathsep)
            self.assertEqual(path[:2], [str(bauset / "ninja"), "/usr/bin"])


if __name__ == "__main__":
    unittest.main()
