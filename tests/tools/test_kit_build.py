"""Unit test for tools/kit/build.py on made-up kits (no game data, no compiler run).

Run: python -m unittest -v test_kit_build (from tests/tools)
"""
import io
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
import zipfile
from contextlib import redirect_stdout
from pathlib import Path
from unittest import mock

HERE = os.path.dirname(__file__)
sys.path.insert(0, os.path.join(HERE, "..", "..", "tools", "kit"))
import build as kb  # noqa: E402

KIT_JSON = {"program_id": "010003F003A34000", "lgp_supported": True,
            "exefs": [{"name": "main", "build_id": kb.MAIN_BUILD_ID + "00000000"}, {"name": "sdk"}]}
FILES = {"exefs/main": b"m", "exefs/sdk": b"s", "exefs/main.npdm": b"n", "romfs.bin": b"r" * 100,
         "game.cfg": b"c", "mods/wild-battles.cfg": b"w", "mods/classic-exp.cfg": b"e", "mods/original-encounters/romfs/a.bin": b"a",
         "systemdata/0100000000000811/std.bfttf": b"f", "systemdata/0100000000000823/0": b"n",
         "lgp-kit.json": json.dumps(KIT_JSON).encode()}


def write_zip(path, files):
    with zipfile.ZipFile(path, "w") as z:
        for name, data in files.items():
            z.writestr(name, data)


class KitCheck(unittest.TestCase):
    def test_good_kit_warns_only_about_seed(self):
        warnings = kb.check_kit(KIT_JSON, set(FILES))
        self.assertEqual(len(warnings), 1)
        self.assertIn("pipeline-seed", warnings[0])

    def test_rejects_other_games(self):
        other = dict(KIT_JSON, program_id="0100abcd00000000", lgp_supported=False)
        with self.assertRaisesRegex(kb.KitError, "not"):
            kb.check_kit(other, set(FILES))
        evoli = dict(KIT_JSON, lgp_supported=None, exefs=[{"name": "main", "build_id": "aa" * 16}])
        with self.assertRaises(kb.KitError):
            kb.check_kit(evoli, set(FILES))

    def test_other_supported_builds(self):
        for build, (title, name) in kb.SUPPORTED_BUILDS.items():
            info = dict(KIT_JSON, program_id=title.upper(), exefs=[{"name": "main", "build_id": build + "00" * 16}])
            self.assertEqual(kb.kit_build(info), build, name)
            kb.check_kit(info, set(FILES))
        eevee = next(b for b, (t, _) in kb.SUPPORTED_BUILDS.items() if t == kb.TITLE_ID_2)
        wrong_title = dict(KIT_JSON, exefs=[{"name": "main", "build_id": eevee}])  # the first edition's program ID
        self.assertIsNone(kb.kit_build(wrong_title))
        with self.assertRaisesRegex(kb.KitError, "supported game versions"):
            kb.check_kit(wrong_title, set(FILES))

    def test_kit_files_per_build(self):
        other = next(b for b in kb.SUPPORTED_BUILDS if b != kb.MAIN_BUILD_ID)
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "mods").mkdir()
            (root / "game.cfg").write_bytes(b"c")
            (root / "mods" / "a.cfg").write_bytes(b"a")
            self.assertEqual(kb.kit_files_for(root, kb.MAIN_BUILD_ID), root)
            with self.assertRaisesRegex(kb.KitError, other):
                kb.kit_files_for(root, other)
            (root / other).mkdir()
            (root / other / "game.cfg").write_bytes(b"e")
            self.assertEqual(kb.kit_files_for(root, other), root / other)
            # Without a manifest the root's files are all files but the other builds' folders.
            self.assertEqual(kb.kit_file_names(root), ["game.cfg", "mods/a.cfg"])
            self.assertEqual(kb.kit_file_names(root / other), ["game.cfg"])

    def test_fetch_kit_files_takes_the_builds_along(self):
        other = next(b for b in kb.SUPPORTED_BUILDS if b != kb.MAIN_BUILD_ID)
        served = {"manifest.json": json.dumps({"files": [{"path": "game.cfg"}], "builds": [other]}).encode(),
                  "game.cfg": b"pikachu", f"{other}/manifest.json": json.dumps({"files": [{"path": "game.cfg"}]}).encode(),
                  f"{other}/game.cfg": b"other"}
        def urlopen(url, timeout=None):
            return _Response(served[url.split("/kit/", 1)[1]])
        with tempfile.TemporaryDirectory() as tmp, mock.patch.object(kb.urllib.request, "urlopen", urlopen), \
                redirect_stdout(io.StringIO()):
            dest = kb.fetch_kit_files("http://kit.test", Path(tmp) / "files")
            self.assertEqual((dest / "game.cfg").read_bytes(), b"pikachu")
            self.assertEqual((kb.kit_files_for(dest, other) / "game.cfg").read_bytes(), b"other")
            served["manifest.json"] = json.dumps({"files": [], "builds": ["ab" * 16]}).encode()
            with self.assertRaisesRegex(kb.KitError, "unknown build"):
                kb.fetch_kit_files("http://kit.test", Path(tmp) / "files")

    def test_needs_game_cfg_and_modules(self):
        with self.assertRaisesRegex(kb.KitError, "game.cfg"):
            kb.check_kit(KIT_JSON, set(FILES) - {"game.cfg"})
        with self.assertRaisesRegex(kb.KitError, "exefs/sdk"):
            kb.check_kit(KIT_JSON, set(FILES) - {"exefs/sdk"})
        with self.assertRaisesRegex(kb.KitError, "romfs.bin"):
            kb.check_kit(KIT_JSON, set(FILES) - {"romfs.bin"})

    def test_no_firmware_is_a_warning(self):
        names = {n for n in FILES if not n.startswith("systemdata/")}
        warnings = kb.check_kit(KIT_JSON, names)
        self.assertTrue(any("stand-in font" in w and "0100000000000823" in w for w in warnings))

    def test_no_mods_is_a_warning(self):
        names = {n for n in FILES if not n.startswith("mods/")}
        self.assertTrue(any("mod" in w for w in kb.check_kit(KIT_JSON, names)))

    def test_member_paths(self):
        self.assertEqual(kb.safe_member("exefs/main"), "exefs/main")
        self.assertIsNone(kb.safe_member("mods/"))
        for bad in ("../x", "/etc/x", "a/../../x", "C:/x", "a//b", "./a"):
            with self.assertRaises(kb.KitError, msg=bad):
                kb.safe_member(bad)


class Staging(unittest.TestCase):
    def test_zip_and_folder_stage_the_same(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            write_zip(tmp / "kit.zip", FILES)
            folder = tmp / "kit"
            for name, data in FILES.items():
                (folder / name).parent.mkdir(parents=True, exist_ok=True)
                (folder / name).write_bytes(data)
            for i, kit in enumerate((tmp / "kit.zip", folder)):
                info, names = kb.kit_listing(kit)
                self.assertEqual(names, set(FILES))
                self.assertTrue(info["lgp_supported"])
                game, raw = tmp / f"game{i}", tmp / f"raw{i}.bin"
                kb.stage_game(kit, game, raw)
                self.assertEqual(raw.read_bytes(), FILES["romfs.bin"])
                staged = {p.relative_to(game).as_posix() for p in game.rglob("*") if p.is_file()}
                self.assertEqual(staged, set(FILES) - {"romfs.bin"})
                self.assertEqual((game / "mods/wild-battles.cfg").read_bytes(), b"w")

    def test_damaged_zip_is_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            write_zip(tmp / "kit.zip", FILES)
            data = bytearray((tmp / "kit.zip").read_bytes())
            at = data.index(FILES["romfs.bin"])
            data[at] ^= 0xFF  # one flipped byte in romfs.bin: its CRC-32 no longer matches
            (tmp / "kit.zip").write_bytes(bytes(data))
            with self.assertRaisesRegex(kb.KitError, "damaged"):
                kb.stage_game(tmp / "kit.zip", tmp / "game", tmp / "raw.bin")

    def test_zip_without_kit_json(self):
        with tempfile.TemporaryDirectory() as tmp:
            write_zip(Path(tmp, "x.zip"), {"a": b"1"})
            with self.assertRaisesRegex(kb.KitError, "lgp-kit.json"):
                kb.kit_listing(Path(tmp, "x.zip"))

    def test_zip_slip_is_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            write_zip(Path(tmp, "x.zip"), dict(FILES, **{"../evil": b"1"}))
            with self.assertRaises(kb.KitError):
                kb.kit_listing(Path(tmp, "x.zip"))


class GameStamp(unittest.TestCase):
    """The build stamp of a game folder outside the app (--game-separate)."""

    def stamp(self, game: Path, stage_key: str = "k") -> str:
        b = kb.Builder.__new__(kb.Builder)
        b.game, b.stage_key = game, stage_key
        return b.game_stamp()

    def test_stamp_follows_kit_and_mod_files(self):
        with tempfile.TemporaryDirectory() as tmp:
            game = Path(tmp)
            (game / "mods" / "data" / "romfs").mkdir(parents=True)
            (game / "game.cfg").write_text("a")
            (game / "mods" / "wild-battles.cfg").write_text("b")
            first = self.stamp(game)
            self.assertRegex(first, r"^[0-9a-f]{16}$")
            self.assertEqual(self.stamp(game), first, "same inputs, same stamp")
            (game / "romfs.bin").write_bytes(b"x")  # covered by the kit identity in the stage key, not read
            self.assertEqual(self.stamp(game), first)
            self.assertNotEqual(self.stamp(game, "other kit"), first)
            (game / "mods" / "wild-battles.cfg").write_text("c")
            self.assertNotEqual(self.stamp(game), first, "another mod file")
            (game / "mods" / "wild-battles.cfg").write_text("b")
            (game / "mods" / "data" / "romfs" / "f.bin").write_bytes(b"1")
            self.assertNotEqual(self.stamp(game), first, "a new data mod file")


class DryRun(unittest.TestCase):
    """The plan only; the toolchain check is left out, so it does not depend on what the runner has installed."""

    def setUp(self):
        self._check = kb.Toolchain.check
        kb.Toolchain.check = lambda tc: True

    def tearDown(self):
        kb.Toolchain.check = self._check

    def run_build(self, *extra):
        with tempfile.TemporaryDirectory() as tmp:
            write_zip(Path(tmp, "kit.zip"), FILES)
            out = io.StringIO()
            with redirect_stdout(out):
                rc = kb.main(["build", "--kit", str(Path(tmp, "kit.zip")), "--work", str(Path(tmp, "w")),
                              "--dry-run", *extra])
            self.assertFalse(Path(tmp, "w").exists(), "a dry run writes nothing")
            return rc, out.getvalue()

    def test_desktop_plan(self):
        with mock.patch.object(kb, "ffmpeg_ready", lambda: True):
            rc, out = self.run_build()
        self.assertEqual(rc, 0, out)
        self.assertIn("--target lgp_lift lgp_romfs_pack", out)
        self.assertTrue(any("lgp_romfs_pack" in line and "--compress" in line for line in out.splitlines()), out)
        lift = next(line for line in out.splitlines() if "lgp_lift" in line and "--split" in line)
        self.assertIn("--config", lift)
        self.assertIn("--mods", lift)
        self.assertIn("exefs/main", lift.replace("\\", "/"))
        self.assertIn("2 mod files", out)
        self.assertIn("-DLGP_APP_GAME_DIR=", out)
        self.assertIn("lgp_game_package_game", out)
        self.assertIn("-DPython3_EXECUTABLE=" + kb.cmake_path(sys.executable), out)  # not one from the registry
        self.assertIn('"-DLGP_APP_DEFAULT_MODS=original-encounters wild-battles"', out)
        if os.name == "nt":  # no libvpx there (its configure wants MSYS): FFmpeg from the tool cache
            self.assertNotIn("-DLGP_ENABLE_VPX=ON", out)
            self.assertIn("-DLGP_FFMPEG_DIR=" + kb.cmake_path(kb.ffmpeg_dir()), out)
        else:  # macOS and Linux build libvpx, so the videos play without FFmpeg
            self.assertIn("-DLGP_ENABLE_VPX=ON", out)
        for line in out.splitlines():  # CMake scripts treat a backslash as an escape (Windows paths)
            for arg in line.split():
                if arg.startswith(("-DLGP_GAME_LIFTED=", "-DLGP_APP_GAME_DIR=")):
                    self.assertNotIn("\\", arg)

    def test_ffmpeg_dir(self):
        rc, out = self.run_build("--ffmpeg-dir", "none")
        self.assertEqual(rc, 0, out)
        # empty, not left out: CMake would keep the folder of an earlier run in its cache
        self.assertTrue(any(arg.strip('"') == "-DLGP_FFMPEG_DIR=" for arg in out.split()), out)
        with tempfile.TemporaryDirectory() as ff:
            rc, out = self.run_build("--ffmpeg-dir", ff)
            self.assertEqual(rc, 0, out)
            self.assertIn("-DLGP_FFMPEG_DIR=" + kb.cmake_path(Path(ff).resolve()), out)

    def test_overlapping_work_and_kit_are_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            kit = Path(tmp, "w", "game")
            for name, data in FILES.items():
                (kit / name).parent.mkdir(parents=True, exist_ok=True)
                (kit / name).write_bytes(data)
            out = io.StringIO()
            with redirect_stdout(out):
                rc = kb.main(["build", "--kit", str(kit), "--work", str(Path(tmp, "w"))])
            self.assertEqual(rc, 1)
            self.assertIn("overlap", out.getvalue())
            self.assertTrue((kit / "exefs/main").is_file(), "the dump stays")

    def test_no_compress(self):
        rc, out = self.run_build("--no-compress")
        self.assertEqual(rc, 0, out)
        self.assertNotIn("--compress", out)

    def test_ios_needs_team_on_a_mac(self):
        with mock.patch.object(kb.sys, "platform", "darwin"):
            rc, out = self.run_build("--target", "ios")
        self.assertEqual(rc, 1)
        self.assertIn("--ios-team", out)

    def test_ios_without_a_mac_plan(self):
        with mock.patch.object(kb.sys, "platform", "linux"):
            rc, out = self.run_build("--target", "ios", "--bundle-prefix", "de.freund.lgp")
        self.assertEqual(rc, 0, out)
        configure = next(line for line in out.splitlines() if "--preset ios-cross" in line)
        self.assertIn("-DLGP_IOS_MEMORY_ENTITLEMENTS=limit", configure)
        self.assertIn("-DLGP_IOS_BUNDLE_PREFIX=de.freund.lgp", configure)
        self.assertIn("-DLGP_APP_GAME_DIR=", configure)
        self.assertIn("--target lgp_game lgp_game_package_game", out)
        self.assertIn("make_ipa.py", out)
        self.assertIn("Impactor", out)  # --installer sideload is the default without a Mac
        self.assertNotIn("xtool install", out)

    def test_ios_without_a_mac_with_xtool(self):
        with mock.patch.object(kb.sys, "platform", "linux"):
            rc, out = self.run_build("--target", "ios", "--installer", "xtool")
        self.assertEqual(rc, 0, out)
        self.assertIn("xtool install", out)
        self.assertIn("install --target ios --installer xtool", out)  # sideload is the default of install
        self.assertNotIn("Impactor", out)

    def test_ios_sideload_on_a_mac_needs_no_team(self):
        with mock.patch.object(kb.sys, "platform", "darwin"):
            rc, out = self.run_build("--target", "ios", "--installer", "sideload")
        self.assertEqual(rc, 0, out)
        configure = next(line for line in out.splitlines() if "--preset ios" in line)
        self.assertIn("-DLGP_IOS_CLOUD_SAVES=OFF", configure)
        self.assertIn("-DLGP_IOS_MEMORY_ENTITLEMENTS=limit", configure)
        build = next(line for line in out.splitlines() if "--target lgp_game" in line)
        self.assertIn("CODE_SIGNING_ALLOWED=NO", build)
        self.assertNotIn("-allowProvisioningUpdates", build)
        ipa = next(line for line in out.splitlines() if "make_ipa.py" in line)
        self.assertIn("--signer codesign", ipa)
        self.assertIn("Impactor", out)

    def test_ios_without_memory_entitlements_puts_the_guest_on_a_file(self):
        with mock.patch.object(kb.sys, "platform", "linux"):
            rc, out = self.run_build("--target", "ios", "--memory-entitlements", "none")
        self.assertEqual(rc, 0, out)
        configure = next(line for line in out.splitlines() if "--preset ios-cross" in line)
        self.assertIn("-DLGP_IOS_MEMORY_ENTITLEMENTS=none", configure)
        self.assertIn("-DLGP_HOST_BLOCK_FILE_DEFAULT=ON", configure)
        # On a Mac too (Codex on #473: none only dropped the entitlements there)
        with mock.patch.object(kb.sys, "platform", "darwin"):
            rc, out = self.run_build("--target", "ios", "--ios-team", "TEAM000001", "--memory-entitlements", "none")
        self.assertEqual(rc, 0, out)
        configure = next(line for line in out.splitlines() if "--preset ios" in line)
        self.assertIn("-DLGP_IOS_MEMORY_ENTITLEMENTS=none", configure)
        self.assertIn("-DLGP_HOST_BLOCK_FILE_DEFAULT=ON", configure)
        with mock.patch.object(kb.sys, "platform", "darwin"):  # auto on a Mac: both, named explicitly
            rc, out = self.run_build("--target", "ios", "--ios-team", "TEAM000001")
        configure = next(line for line in out.splitlines() if "--preset ios" in line)
        self.assertIn("-DLGP_IOS_MEMORY_ENTITLEMENTS=both", configure)
        self.assertIn("-DLGP_HOST_BLOCK_FILE_DEFAULT=OFF", configure)
        # Any other mode switches it off again, or the cache of a none build in the same work folder would keep it
        with mock.patch.object(kb.sys, "platform", "linux"):
            rc, out = self.run_build("--target", "ios", "--memory-entitlements", "limit")
        configure = next(line for line in out.splitlines() if "--preset ios-cross" in line)
        self.assertIn("-DLGP_HOST_BLOCK_FILE_DEFAULT=OFF", configure)
        self.assertIn("-DLGP_HOST_BLOCK_FILE_RANGE_DEFAULT=", configure)

    def test_ios_free_team_signs_the_memory_limit_only(self):
        with mock.patch.object(kb.sys, "platform", "linux"):
            rc, out = self.run_build("--target", "ios")
        self.assertEqual(rc, 0, out)
        configure = next(line for line in out.splitlines() if "--preset ios-cross" in line)
        self.assertIn("-DLGP_IOS_MEMORY_ENTITLEMENTS=limit", configure)
        self.assertIn("-DLGP_HOST_BLOCK_FILE_DEFAULT=OFF", configure)

    def test_memory_entitlements_default(self):
        b = kb.Builder(kb.parse_args(["build", "--kit", "k", "--target", "ios"]), kb.Toolchain("ios"))
        self.assertEqual(b.memory_entitlements(), "limit")  # free team: Apple refuses extended-virtual-addressing
        with mock.patch.object(kb, "FREE_TEAM_MEMORY_ENTITLEMENTS", "none"):
            self.assertEqual(b.memory_entitlements(), "none")
        b.args.ios_team = "ABCDE12345"  # a paid team gets both, unless the app goes to a sideloader
        b.args.installer = "xtool"
        self.assertEqual(b.memory_entitlements(), "both")
        b.args.installer = "sideload"
        self.assertEqual(b.memory_entitlements(), "limit")
        b.args.memory_entitlements = "none"
        self.assertEqual(b.memory_entitlements(), "none")

    def test_ios_plan(self):
        kb.Toolchain.check = lambda tc: True  # Xcode only exists on a Mac; the plan is the same
        with mock.patch.object(kb.sys, "platform", "darwin"):
            rc, out = self.run_build("--target", "ios", "--ios-team", "TEAM")
        self.assertEqual(rc, 0, out)
        configure = next(line for line in out.splitlines() if "--preset ios" in line)
        self.assertRegex(configure, r"-DLGP_APP_GAME_DIR=\S+")  # the game inside the app
        self.assertIn("-DLGP_APP_GAME_STAMP= ", configure + " ")  # a stamp of an earlier slim build is cleared
        self.assertIn("-DLGP_IOS_CLOUD_SAVES=ON", configure)  # explicit: a sideload build's OFF stays cached
        self.assertNotIn("does not contain the game", out)

    def test_ios_game_separate_plan(self):
        kb.Toolchain.check = lambda tc: True
        with mock.patch.object(kb.sys, "platform", "darwin"):
            rc, out = self.run_build("--target", "ios", "--ios-team", "TEAM", "--game-separate")
        self.assertEqual(rc, 0, out)
        configure = next(line for line in out.splitlines() if "--preset ios" in line)
        self.assertIn("-DLGP_APP_GAME_DIR= ", configure + " ")  # empty: no game in the app
        self.assertRegex(configure, r"-DLGP_APP_GAME_STAMP=[0-9a-f]{16}\b")
        self.assertIn("does not contain the game", out)
        self.assertIn("Finder", out)

    def test_ios_without_a_mac_game_separate(self):
        with mock.patch.object(kb.sys, "platform", "linux"):
            rc, out = self.run_build("--target", "ios", "--game-separate")
        self.assertEqual(rc, 0, out)
        configure = next(line for line in out.splitlines() if "--preset ios-cross" in line)
        self.assertIn("-DLGP_APP_GAME_DIR= ", configure + " ")
        self.assertRegex(configure, r"-DLGP_APP_GAME_STAMP=[0-9a-f]{16}\b")
        self.assertIn("Impactor", out)  # default without a Mac: sideload
        self.assertNotIn("xtool install", out)
        self.assertIn("Apple Devices", out)
        build = next(line for line in out.splitlines() if "--build" in line and "build-ios-cross" in line)
        self.assertNotIn("lgp_game_package_game", build, "a fresh slim tree has no copy target")

    def test_game_separate_only_for_ios(self):
        rc, out = self.run_build("--game-separate")
        self.assertEqual(rc, 1)
        self.assertIn("--target ios", out)

    def test_android_plan(self):
        rc, out = self.run_build("--target", "android")
        self.assertEqual(rc, 0, out)
        lines = out.splitlines()
        configure = next(line for line in lines if "--preset android-app" in line)
        self.assertIn('"-DLGP_APP_DEFAULT_MODS=original-encounters wild-battles"', configure)
        self.assertIn("-DLGP_ANDROID_KEYSTORE=", configure)
        self.assertNotIn("-DLGP_APP_GAME_DIR=", configure)  # the game goes into split APKs
        self.assertNotIn("-DLGP_APP_PIPELINE_SEED=", configure)  # the kit has no seed
        self.assertIn("--target lgp_game_apk", out)
        splits = next(line for line in lines if "build_game_splits.py" in line)
        self.assertIn("--keystore", splits)
        self.assertIn("--package org.lgp.recomp.game", splits)
        self.assertIn("adb install-multiple", out)
        self.assertNotIn("-DANDROID_ABI=", configure)  # the preset's arm64-v8a

    def test_android_emulator_abi(self):
        rc, out = self.run_build("--target", "android", "--abi", "x86_64")
        self.assertEqual(rc, 0, out)
        configure = next(line for line in out.splitlines() if "--preset android-app" in line)
        self.assertIn("-DANDROID_ABI=x86_64", configure)
        self.assertIn("build-android-x86_64", configure)  # own tree beside the arm64 one
        splits = next(line for line in out.splitlines() if "build_game_splits.py" in line)
        self.assertIn("android-x86_64", splits)



class PlainFolder(unittest.TestCase):
    """A game folder unpacked by other means (no lgp-kit.json): identified by main's build ID, game.cfg and the
    mods from --kit-files, the RomFS as romfs.bin or as files under romfs/."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name).resolve()  # Windows runners: TEMP as an 8.3 short path (RUNNER~1)
        self.game = self.root / "unpacked"
        self.write_game(kb.MAIN_BUILD_ID + "00" * 16)
        (self.game / "romfs" / "bin").mkdir(parents=True)
        (self.game / "romfs" / "bin" / "a.dat").write_bytes(b"a")
        self.files = self.root / "kitfiles"
        (self.files / "mods").mkdir(parents=True)
        (self.files / "game.cfg").write_text("cfg")
        (self.files / "mods" / "wild-battles.cfg").write_text("mod")
        self._check = kb.Toolchain.check
        kb.Toolchain.check = lambda tc: True

    def tearDown(self):
        kb.Toolchain.check = self._check
        self.tmp.cleanup()

    def write_game(self, build_id: str, program: int = int(kb.TITLE_ID, 16)):
        exefs = self.game / "exefs"
        exefs.mkdir(parents=True, exist_ok=True)
        npdm = bytearray(0x80)
        npdm[0:4] = b"META"
        npdm[0x70:0x74] = (0x80).to_bytes(4, "little")
        aci = bytearray(0x40)
        aci[0:4] = b"ACI0"
        aci[0x10:0x18] = program.to_bytes(8, "little")
        (exefs / "main.npdm").write_bytes(bytes(npdm + aci))
        head = bytearray(0x100)
        head[0:4] = b"NSO0"
        head[0x40:0x60] = bytes.fromhex(build_id)
        (exefs / "main").write_bytes(bytes(head))
        (exefs / "sdk").write_bytes(b"NSO0")

    def run_main(self, *argv):
        out = io.StringIO()
        with redirect_stdout(out):
            rc = kb.main(list(argv))
        return rc, out.getvalue()

    def test_identified_by_build_id(self):
        info, names = kb.kit_listing(self.game)
        self.assertTrue(info["plain"])
        self.assertEqual(info["program_id"], kb.TITLE_ID)
        names |= set(kb.kit_file_names(self.files))
        warnings = kb.check_kit(info, names)
        self.assertTrue(any("firmware" in w for w in warnings), warnings)
        with self.assertRaisesRegex(kb.KitError, "--kit-files"):
            kb.check_kit(info, names - {"game.cfg"})
        self.write_game("aa" * 32)
        with self.assertRaisesRegex(kb.KitError, "supported game versions"):
            kb.check_kit(*kb.kit_listing(self.game))
        (self.game / "exefs" / "main").unlink()
        with self.assertRaisesRegex(kb.KitError, "exefs/main"):
            kb.kit_listing(self.game)

    def test_dry_run_packs_the_romfs_folder(self):
        work = self.root / "w"
        rc, out = self.run_main("build", "--kit", str(self.game), "--kit-files", str(self.files), "--work", str(work),
                                "--dry-run")
        self.assertEqual(rc, 0, out)
        self.assertIn("1 mod files", out)
        pack = next(line for line in out.splitlines() if "lgp_romfs_pack" in line and "--compress" in line)
        self.assertIn(str(self.game / "romfs"), pack)
        self.assertFalse(work.exists(), "a dry run writes nothing")

    def test_notes_the_assistant_showed_are_not_repeated(self):
        """PC2 VM run 3: a note of check_kit came twice, once from the assistant and once from build()."""
        work = self.root / "w"
        rc, out = self.run_main("build", "--kit", str(self.game), "--kit-files", str(self.files), "--work", str(work),
                                "--dry-run")
        note = next(line for line in out.splitlines() if line.startswith("Note: "))[len("Note: "):]
        with mock.patch.object(kb, "NOTES_SHOWN", {note}):
            rc, out = self.run_main("build", "--kit", str(self.game), "--kit-files", str(self.files), "--work",
                                    str(work), "--dry-run")
        self.assertEqual(rc, 0, out)
        self.assertNotIn(note, out)

    def test_stage_takes_the_kit_files_over_the_folder(self):
        (self.game / "game.cfg").write_text("old")
        (self.game / "nsp").mkdir()
        (self.game / "nsp" / "dump.nsp").write_bytes(b"PFS0")  # whatever else lies in the folder stays out
        (self.game / "extract.log").write_text("log")
        args = kb.parse_args(["build", "--kit", str(self.game), "--kit-files", str(self.files),
                              "--work", str(self.root / "w"), "--no-compress"])
        cmds = []
        with redirect_stdout(io.StringIO()), mock.patch.object(kb.Builder, "run", lambda b, cmd, cwd=None: cmds.append(cmd)):
            b = kb.Builder(args, kb.Toolchain("desktop"))
            b.extra_files = b.kit_files()
            b.stage()
            game = b.game
            self.assertEqual((game / "game.cfg").read_text(), "cfg")
            self.assertEqual((game / "mods" / "wild-battles.cfg").read_text(), "mod")
            self.assertTrue((game / "exefs" / "main").is_file())
            self.assertFalse((game / "romfs").exists(), "the RomFS files are packed, not copied")
            self.assertFalse((game / "nsp").exists())
            self.assertFalse((game / "extract.log").exists())
            self.assertEqual(cmds[-1][1:], [str(self.game / "romfs"), str(game / "romfs.bin")])
            self.assertTrue((self.game / "romfs" / "bin" / "a.dat").is_file(), "the user's folder stays")
            first = b.game_key()
            (self.game / "extract.log").write_text("log 2")
            self.assertEqual(b.game_key(), first, "files left out do not restage the game folder")
            (self.files / "mods" / "wild-battles.cfg").write_text("mod v2")
            self.assertNotEqual(b.game_key(), first, "other kit files, another game folder")

    def test_compressed_romfs_bin_is_unpacked_first(self):
        """A game folder build.py made before (compressed romfs.bin) as --kit: the mod recipes and the packing
        need the plain image, so it is unpacked first."""
        shutil.rmtree(self.game / "romfs")
        (self.game / "romfs.bin").write_bytes(b"LGPROMZ\0" + b"z" * 8)
        args = kb.parse_args(["build", "--kit", str(self.game), "--kit-files", str(self.files),
                              "--work", str(self.root / "w"), "--no-compress"])
        cmds = []

        def run(b, cmd, cwd=None):
            cmds.append(cmd)
            Path(cmd[-1]).write_bytes(b"plain")

        with redirect_stdout(io.StringIO()), mock.patch.object(kb.Builder, "run", run):
            b = kb.Builder(args, kb.Toolchain("desktop"))
            b.extra_files = b.kit_files()
            b.stage()
            self.assertEqual(len(cmds), 1, cmds)
            self.assertNotIn("--compress", cmds[0])
            self.assertEqual((b.game / "romfs.bin").read_bytes(), b"plain")
            self.assertTrue((self.game / "romfs.bin").read_bytes().startswith(b"LGPROMZ"), "the user's file stays")

    def test_public_snapshot_builds_without_the_dump_path(self):
        """The public snapshot drops the dump path and its loader (tools/publish/public_snapshot.py): build.py must
        still run and take a plain game folder."""
        sys.path.insert(0, str(kb.REPO / "tools" / "publish"))
        import public_snapshot
        text, errors = public_snapshot.apply_markers((kb.REPO / "tools" / "kit" / "build.py").read_text(encoding="utf-8"))
        self.assertEqual(errors, [])
        for ref in public_snapshot.LEGAL_REFERENCES:
            self.assertNotIn(ref, text)
        copy = self.root / "public" / "tools" / "kit" / "build.py"
        copy.parent.mkdir(parents=True)
        copy.write_text(text, encoding="utf-8")
        r = subprocess.run([sys.executable, "-I", str(copy), "build", "--help"], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertNotIn("--dump", r.stdout)
        self.assertIn("--kit-files", r.stdout)
        r = subprocess.run([sys.executable, "-I", str(copy), "build", "--kit", str(self.game), "--kit-files",
                            str(self.files), "--work", str(self.root / "w"), "--dry-run"],
                           capture_output=True, text=True, encoding="utf-8")
        self.assertIn("lgp_lift", r.stdout, r.stdout + r.stderr)
        self.assertNotIn("Traceback", r.stderr)

    def test_kit_url_from_the_environment_only_when_named(self):
        with mock.patch.dict(os.environ, {"LGP_KIT_URL": "http://kit.example"}):
            self.assertEqual(kb.parse_args(["build", "--kit", "k.zip"]).kit_url, "")
            self.assertEqual(kb.parse_args(["build", "--kit", "k", "--kit-url", "http://x"]).kit_url, "http://x")


class StaleCache(unittest.TestCase):
    def test_backslash_paths_drop_the_cache(self):
        with tempfile.TemporaryDirectory() as tmp:
            tree = Path(tmp)
            (tree / "CMakeFiles").mkdir()
            cache = tree / "CMakeCache.txt"
            cache.write_text("LGP_GAME_LIFTED:STRING=C:/ok/lifted\nLGP_APP_GAME_DIR:PATH=C:/ok/game\n")
            with redirect_stdout(io.StringIO()):
                kb.drop_stale_cache(tree, False)
            self.assertTrue(cache.is_file(), "forward slashes stay")
            cache.write_text("LGP_GAME_LIFTED:STRING=D:\\Dev\\lifted\n")
            with redirect_stdout(io.StringIO()):
                kb.drop_stale_cache(tree, False)
            self.assertFalse(cache.exists())
            self.assertFalse((tree / "CMakeFiles").exists())

    def test_other_compiler_drops_the_cache(self):
        with tempfile.TemporaryDirectory() as tmp:
            cache = Path(tmp, "CMakeCache.txt")
            cache.write_text("CMAKE_CXX_COMPILER:FILEPATH=C:/Program Files/LLVM/bin/clang++.exe\n")
            with redirect_stdout(io.StringIO()):
                kb.drop_stale_cache(Path(tmp), False, "C:/Program Files/LLVM/bin/clang++.exe")
            self.assertTrue(cache.is_file(), "same compiler: kept")
            with redirect_stdout(io.StringIO()):
                kb.drop_stale_cache(Path(tmp), False, "C:/Users/x/llvm-mingw/bin/x86_64-w64-mingw32-clang++.exe")
            self.assertFalse(cache.exists())

    def test_settings_game_line(self):
        with tempfile.TemporaryDirectory() as tmp:
            old = os.environ.get("LGP_APP_DIR")
            os.environ["LGP_APP_DIR"] = tmp
            try:
                self.assertIsNone(kb.settings_game_override())
                Path(tmp, "settings.ini").write_text("# x\ngame = D:/x/extracted\n", encoding="utf-8")
                self.assertEqual(kb.settings_game_override()[1], "D:/x/extracted")
            finally:
                if old is None:
                    del os.environ["LGP_APP_DIR"]
                else:
                    os.environ["LGP_APP_DIR"] = old


class Relift(unittest.TestCase):
    """A failed lift keeps the previous lifted/ tree, so a retry can configure and lift again."""

    def test_failed_lift_keeps_old_tree(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            args = kb.parse_args(["build", "--kit", str(tmp / "kit.zip"), "--work", str(tmp / "w")])
            b = kb.Builder(args, kb.Toolchain("desktop"))
            for name in ("exefs/main", "exefs/sdk", "game.cfg"):
                (b.game / name).parent.mkdir(parents=True, exist_ok=True)
                (b.game / name).write_bytes(b"x")
            b.lifted.mkdir(parents=True)
            (b.lifted / "lifted_000.cpp").write_text("old")

            def fail(cmd, cwd=None):
                Path(cmd[cmd.index("-o") + 1]).write_text("partial")
                raise kb.KitError("lift failed")
            b.run = fail
            with redirect_stdout(io.StringIO()), self.assertRaises(kb.KitError):
                b.lift()
            self.assertEqual((b.lifted / "lifted_000.cpp").read_text(), "old")
            self.assertFalse((b.lifted / "lifted.cpp").exists())

            b.run = lambda cmd, cwd=None: Path(cmd[cmd.index("-o") + 1]).write_text("new")
            with redirect_stdout(io.StringIO()):
                b.lift()
            self.assertEqual(sorted(p.name for p in b.lifted.iterdir()), ["lifted.cpp"])
            self.assertFalse(b.lifted.with_name("lifted.new").exists())


class LeaveOutBadMods(unittest.TestCase):
    """A mod file whose patch sites do not fit this game build stops only that mod, not the build (Mac M1)."""

    def builder(self, tmp):
        args = kb.parse_args(["build", "--kit", str(tmp / "kit.zip"), "--work", str(tmp / "w")])
        b = kb.Builder(args, kb.Toolchain("desktop"))
        for name in ("exefs/main", "exefs/sdk", "game.cfg", "mods/widescreen.cfg", "mods/wild-battles.cfg"):
            (b.game / name).parent.mkdir(parents=True, exist_ok=True)
            (b.game / name).write_bytes(b"x")
        b.work.mkdir(parents=True, exist_ok=True)
        return b

    @staticmethod
    def fake_lift(bad_name, rc=1):
        calls = []

        def run(cmd, cwd=None, env=None):
            calls.append(cmd)
            mods = Path(cmd[cmd.index("--mods") + 1]) if "--mods" in cmd else None
            if mods and (mods / bad_name).is_file():
                if rc == 1:
                    Path(cmd[cmd.index("--bad-mods-out") + 1]).write_text(bad_name + "\n")
                return subprocess.CompletedProcess(cmd, rc)
            Path(cmd[cmd.index("-o") + 1]).write_text("lifted")
            return subprocess.CompletedProcess(cmd, 0)
        return run, calls

    def test_bad_mod_is_left_out_and_the_rest_lifted(self):
        with tempfile.TemporaryDirectory() as tmp:
            b = self.builder(Path(tmp))
            run, calls = self.fake_lift("widescreen.cfg")
            out = io.StringIO()
            with mock.patch.object(kb.subprocess, "run", run), redirect_stdout(out):
                b.lift()
            self.assertEqual(len(calls), 2)
            self.assertFalse((b.game / "mods" / "widescreen.cfg").exists())
            self.assertTrue((b.game / "mods-left-out" / "widescreen.cfg").is_file())
            self.assertTrue((b.game / "mods" / "wild-battles.cfg").is_file())
            self.assertEqual((b.lifted / "lifted.cpp").read_text(), "lifted")
            self.assertIn("the mod widescreen does not fit this game build", out.getvalue())
            self.assertEqual(b.default_mods(), "wild-battles")
            # the stamp names the mods really lifted: the next run with the same folder lifts nothing
            with mock.patch.object(kb.subprocess, "run", run), redirect_stdout(io.StringIO()) as again:
                b.lift()
            self.assertEqual(len(calls), 2)
            self.assertIn("Lift unchanged", again.getvalue())

    def test_other_lift_failures_still_stop(self):
        with tempfile.TemporaryDirectory() as tmp:
            b = self.builder(Path(tmp))
            run, _ = self.fake_lift("widescreen.cfg", rc=3)
            with mock.patch.object(kb.subprocess, "run", run), redirect_stdout(io.StringIO()), \
                    self.assertRaisesRegex(kb.KitError, "exit code 3"):
                b.lift()
            self.assertTrue((b.game / "mods" / "widescreen.cfg").is_file())


class DefaultMods(unittest.TestCase):
    def test_staged_mods_without_the_off_ones(self):
        with tempfile.TemporaryDirectory() as tmp:
            args = kb.parse_args(["build", "--kit", str(Path(tmp, "kit.zip")), "--work", tmp])
            b = kb.Builder(args, kb.Toolchain("desktop"))
            for name in ("mods/wild-battles.cfg", "mods/online-button.cfg", "mods/classic-exp.cfg",
                         "mods/original-encounters/romfs/bin/a.bin", "mods/notes.txt"):
                (b.game / name).parent.mkdir(parents=True, exist_ok=True)
                (b.game / name).write_bytes(b"x")
            self.assertEqual(b.default_mods(), "original-encounters wild-battles")


class FetchCompiler(unittest.TestCase):
    """llvm-mingw from a made-up release: checked against its SHA-256, other targets left out."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.saved = (kb.LLVM_MINGW, os.environ.get("LGP_KIT_TOOLS"), kb.urllib.request.urlopen)
        os.environ["LGP_KIT_TOOLS"] = self.tmp.name
        buf = io.BytesIO()
        write_zip(buf, {"lm/bin/x86_64-w64-mingw32-clang++.exe": b"c", "lm/x86_64-w64-mingw32/lib/a.a": b"a",
                        "lm/aarch64-w64-mingw32/lib/b.a": b"b", "lm/python/x.py": b"p"})
        self.data = buf.getvalue()
        kb.urllib.request.urlopen = lambda url: _Response(self.data)

    def tearDown(self):
        kb.LLVM_MINGW, tools, kb.urllib.request.urlopen = self.saved
        if tools is None:
            del os.environ["LGP_KIT_TOOLS"]
        else:
            os.environ["LGP_KIT_TOOLS"] = tools
        self.tmp.cleanup()

    def test_unpacks_only_x86_64(self):
        import hashlib
        kb.LLVM_MINGW = ("v", "lm", hashlib.sha256(self.data).hexdigest())
        with redirect_stdout(io.StringIO()):
            bin_dir = kb.fetch_llvm_mingw()
        root = bin_dir.parent
        self.assertTrue((bin_dir / "x86_64-w64-mingw32-clang++.exe").is_file())
        self.assertTrue((root / "x86_64-w64-mingw32/lib/a.a").is_file())
        self.assertFalse((root / "aarch64-w64-mingw32").exists())
        self.assertFalse((root / "python").exists())
        self.assertEqual(sorted(p.name for p in Path(self.tmp.name).iterdir()), ["lm"], "no .part/.tmp left")

    def test_wrong_hash_is_refused(self):
        kb.LLVM_MINGW = ("v", "lm", "00" * 32)
        with redirect_stdout(io.StringIO()), self.assertRaisesRegex(kb.KitError, "checksum"):
            kb.fetch_llvm_mingw()
        self.assertEqual(list(Path(self.tmp.name).iterdir()), [])


class FetchFfmpeg(unittest.TestCase):
    """FFmpeg from a made-up release: checked against its SHA-256, unpacked into the tool cache, the build uses it."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.saved = (kb.FFMPEG, kb.urllib.request.urlopen)
        self.env = mock.patch.dict(os.environ, {"LGP_KIT_TOOLS": self.tmp.name})
        self.env.start()
        buf = io.BytesIO()
        write_zip(buf, {"ff/bin/avcodec-61.dll": b"c", "ff/bin/avutil-59.dll": b"u",
                        "ff/include/libavcodec/avcodec.h": b"h", "ff/lib/avcodec.lib": b"l", "ff/LICENSE.txt": b"L"})
        self.data = buf.getvalue()
        kb.urllib.request.urlopen = lambda url: _Response(self.data)

    def tearDown(self):
        kb.FFMPEG, kb.urllib.request.urlopen = self.saved
        self.env.stop()
        self.tmp.cleanup()

    def test_unpacks_into_the_tool_cache(self):
        import hashlib
        kb.FFMPEG = ("t", "ff", hashlib.sha256(self.data).hexdigest())
        with redirect_stdout(io.StringIO()):
            root = kb.fetch_ffmpeg()
        self.assertEqual(root, kb.ffmpeg_dir())
        for name in ("bin/avcodec-61.dll", "include/libavcodec/avcodec.h", "lib/avcodec.lib", "LICENSE.txt"):
            self.assertTrue((root / name).is_file(), name)
        self.assertEqual(sorted(p.name for p in Path(self.tmp.name).iterdir()), ["ff"], "no .part/.tmp left")
        kb.urllib.request.urlopen = None  # a second call finds it and downloads nothing
        with redirect_stdout(io.StringIO()):
            self.assertEqual(kb.fetch_ffmpeg(), root)

    def test_certificate_error_falls_back_to_curl(self):
        """A fresh Windows: Python does not know github.com's root yet, curl.exe (Schannel) does."""
        import hashlib
        import ssl
        kb.FFMPEG = ("t", "ff", hashlib.sha256(self.data).hexdigest())

        def refuse(url):
            raise kb.urllib.error.URLError(ssl.SSLCertVerificationError("unable to get local issuer certificate"))

        def curl(cmd, **kw):
            Path(cmd[cmd.index("-o") + 1]).write_bytes(self.data)
            return mock.Mock(returncode=0)
        kb.urllib.request.urlopen = refuse
        with mock.patch.object(kb, "windows_curl", lambda: "curl.exe"), \
                mock.patch.object(kb.subprocess, "run", curl), redirect_stdout(io.StringIO()) as out:
            root = kb.fetch_ffmpeg()
        self.assertTrue((root / "bin/avcodec-61.dll").is_file())
        self.assertIn("curl", out.getvalue())

    def test_certificate_error_without_curl_says_so(self):
        import ssl
        kb.FFMPEG = ("t", "ff", "00" * 32)

        def refuse(url):
            raise kb.urllib.error.URLError(ssl.SSLCertVerificationError("unable to get local issuer certificate"))
        kb.urllib.request.urlopen = refuse
        with mock.patch.object(kb, "windows_curl", lambda: None), redirect_stdout(io.StringIO()), \
                self.assertRaisesRegex(kb.KitError, "internet connection.*HTTPS"):
            kb.fetch_ffmpeg()
        self.assertEqual(list(Path(self.tmp.name).iterdir()), [])

    def test_wrong_hash_is_refused(self):
        kb.FFMPEG = ("t", "ff", "00" * 32)
        with redirect_stdout(io.StringIO()), self.assertRaisesRegex(kb.KitError, "checksum"):
            kb.fetch_ffmpeg()
        self.assertEqual(list(Path(self.tmp.name).iterdir()), [])

    def test_pinned_release(self):
        tag, folder, sha = self.saved[0]
        self.assertTrue(folder.endswith("-win64-lgpl-shared-7.1"), "an LGPL build (linked dynamically), not GPL")
        self.assertRegex(sha, r"^[0-9a-f]{64}$")


class AndroidTools(unittest.TestCase):
    """SDK/NDK lookup and fetch-android on made-up folders; sdkmanager itself is not run."""

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.env = mock.patch.dict(os.environ, {"LGP_KIT_TOOLS": str(self.tmp / "tools"), "LGP_ANDROID_SDK": "",
                                                "ANDROID_HOME": "", "ANDROID_SDK_ROOT": "", "ANDROID_NDK_HOME": "",
                                                "ANDROID_NDK_ROOT": "", "LOCALAPPDATA": str(self.tmp / "local"),
                                                "HOME": str(self.tmp / "home")})
        self.env.start()

    def tearDown(self):
        self.env.stop()

    def make_sdk(self, sdk, ndks=("27.2.1", "30.0.16248370")):
        for name in ("platforms/android-35/android.jar", "build-tools/35.0.0/apksigner"):
            (sdk / name).parent.mkdir(parents=True, exist_ok=True)
            (sdk / name).write_bytes(b"")
        for v in ndks:
            (sdk / "ndk" / v).mkdir(parents=True)
            (sdk / "ndk" / v / "source.properties").write_text(f"Pkg.Desc = Android NDK\nPkg.Revision = {v}\n")

    def test_finds_sdk_and_new_enough_ndk(self):
        sdk = self.tmp / "sdk"
        self.make_sdk(sdk)
        os.environ["ANDROID_HOME"] = str(sdk)
        self.assertEqual(kb.android_sdk(), sdk)
        self.assertEqual(kb.android_ndk(sdk), sdk / "ndk" / "30.0.16248370")
        os.environ["ANDROID_NDK_HOME"] = str(sdk / "ndk" / "27.2.1")  # too old: r27c lacks std::atomic_ref
        self.assertEqual(kb.android_ndk(sdk), sdk / "ndk" / "30.0.16248370")

    def test_without_sdk_installs_into_the_tool_cache(self):
        self.assertEqual(kb.android_sdk(), self.tmp / "tools" / "android-sdk")
        self.assertIsNone(kb.android_ndk(kb.android_sdk()))

    def test_keystore_outlives_work_folders(self):
        os.environ["LGP_PRIVATE_DIR"] = str(self.tmp / "private")
        self.assertEqual(kb.android_keystore(), self.tmp / "private" / "android" / "lgp-debug.keystore")
        del os.environ["LGP_PRIVATE_DIR"]
        self.assertNotIn(str(self.tmp / "tools"), str(kb.android_keystore()))

    def test_fetch_android_loads_sdkmanager_and_installs(self):
        buf = io.BytesIO()
        write_zip(buf, {"cmdline-tools/bin/sdkmanager": b"#!/bin/sh\n", "cmdline-tools/bin/sdkmanager.bat": b"@rem\r\n",
                        "cmdline-tools/lib/x.jar": b"j", "other/y": b"o"})
        data = buf.getvalue()
        import hashlib
        tools = {host: (name, hashlib.sha1(data).hexdigest()) for host, (name, _) in kb.ANDROID_CMDLINE_TOOLS.items()}
        calls = []
        with mock.patch.object(kb, "ANDROID_CMDLINE_TOOLS", tools), \
                mock.patch.object(kb, "find_jdk", lambda: self.tmp / "jdk"), \
                mock.patch.object(kb.urllib.request, "urlopen", lambda url: _Response(data)), \
                mock.patch.object(kb.subprocess, "run",
                                  lambda cmd, env=None: calls.append((cmd, env)) or mock.Mock(returncode=0)), \
                redirect_stdout(io.StringIO()):
            sdk = kb.fetch_android()
        latest = sdk / "cmdline-tools" / "latest"
        self.assertTrue((latest / "lib" / "x.jar").is_file())
        self.assertFalse((sdk / "other").exists())
        self.assertEqual([p.name for p in sdk.iterdir()], ["cmdline-tools"], "no .part left")
        cmd, env = calls[0]
        self.assertEqual(Path(cmd[0]).parent, latest / "bin")
        self.assertEqual(cmd[1:3], [f"--sdk_root={sdk}", "--install"])
        self.assertEqual(tuple(cmd[3:]), kb.ANDROID_PACKAGES)
        self.assertEqual(env["JAVA_HOME"], str(self.tmp / "jdk"))

    @unittest.skipIf(os.name == "nt", "fake adb is a shell script")
    def test_install_android(self):
        sdk = self.tmp / "sdk"
        (sdk / "platform-tools").mkdir(parents=True)
        log = self.tmp / "adb.log"
        state = self.tmp / "state"
        adb = sdk / "platform-tools" / "adb"
        adb.write_text(f'#!/bin/sh\necho "$@" >> "{log}"\n'
                       f'if [ "$1" = devices ]; then echo "List of devices attached"; cat "{state}"; fi\n')
        adb.chmod(0o755)
        out = self.tmp / "android"
        out.mkdir()
        with redirect_stdout(io.StringIO()):
            with self.assertRaisesRegex(kb.KitError, "lacks"):
                kb.install_android(out, sdk)
            for name in ("lgp_game.apk", "game-exefs.apk", "game-romfs-0.apk"):
                (out / name).write_bytes(b"")
            state.write_text("")
            with self.assertRaisesRegex(kb.KitError, "USB debugging"):
                kb.install_android(out, sdk)
            state.write_text("ABC\tunauthorized\n")
            with self.assertRaisesRegex(kb.KitError, "Allow USB debugging"):
                kb.install_android(out, sdk)
            state.write_text("ABC\tdevice\n")
            kb.install_android(out, sdk)
        install = log.read_text().splitlines()[-1].split()
        self.assertEqual(install[:2], ["install-multiple", "-r"])
        self.assertEqual([Path(a).name for a in install[2:]], ["lgp_game.apk", "game-exefs.apk", "game-romfs-0.apk"])

    def test_fetch_android_needs_a_jdk(self):
        with mock.patch.object(kb, "find_jdk", lambda: None), self.assertRaisesRegex(kb.KitError, "JDK"):
            kb.fetch_android()


class _Response(io.BytesIO):
    headers: dict = {}

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()


class Assistant(unittest.TestCase):
    def test_dropped_paths(self):
        self.assertEqual(kb.dropped_path(' "C:/Spiele/dump.zip" '), Path("C:/Spiele/dump.zip"))
        self.assertEqual(kb.dropped_path("'/a b/x.zip'"), Path("/a b/x.zip"))
        if os.name != "nt":
            self.assertEqual(kb.dropped_path("/a\\ b/x.zip"), Path("/a b/x.zip"))

    def test_asks_again_for_a_bad_kit_and_stops_without_yes(self):
        with tempfile.TemporaryDirectory() as tmp:
            write_zip(Path(tmp, "kit.zip"), FILES)
            kits = iter([str(Path(tmp, "missing.zip")), f'"{Path(tmp, "kit.zip")}"'])

            def answer(prompt=""):
                if "dump" in prompt or "game folder" in prompt:  # the public snapshot has no dump path
                    return next(kits)
                if "Start?" in prompt:
                    return "n"
                return ""  # target: default

            check = kb.Toolchain.check
            kb.Toolchain.check = lambda tc: True
            kb.input = answer
            try:
                out = io.StringIO()
                with redirect_stdout(out):
                    rc = kb.main([])
            finally:
                kb.Toolchain.check = check
                del kb.input
            self.assertEqual(rc, 1)
            text = out.getvalue()
            self.assertIn("neither a ZIP nor a folder", text)
            self.assertIn("[3/3] Building", text)
            self.assertNotIn("$ ", text, "nothing runs without a yes")

    def test_kit_zip_without_game_cfg_asks_for_the_mods_folder(self):
        """A kit-page dump without the lift configuration takes game.cfg and mods/ from the mod package (PC2 VM run 2)."""
        with tempfile.TemporaryDirectory() as tmp:
            write_zip(Path(tmp, "kit.zip"), {n: d for n, d in FILES.items()
                                             if n != "game.cfg" and not n.startswith("mods/")})
            mods = Path(tmp, "lgp-mods")
            (mods / "mods").mkdir(parents=True)
            (mods / "game.cfg").write_text("c")
            (mods / "mods" / "wild-battles.cfg").write_text("w")
            asked = []

            def answer(prompt=""):
                if "game.cfg and mods/" in prompt:
                    asked.append(prompt)
                    return str(mods)
                if "dump" in prompt or "game folder" in prompt:
                    return str(Path(tmp, "kit.zip"))
                if "Start?" in prompt:
                    return "n"
                return ""

            check = kb.Toolchain.check
            kb.Toolchain.check = lambda tc: True
            kb.input = answer
            try:
                out = io.StringIO()
                with redirect_stdout(out):
                    rc = kb.main([])
            finally:
                kb.Toolchain.check = check
                del kb.input
            self.assertEqual(rc, 1)
            self.assertEqual(len(asked), 1)
            self.assertNotIn("lacks game.cfg", out.getvalue())
            self.assertIn("[3/3] Building", out.getvalue())


    def test_package_kit_files_are_taken_without_asking(self):
        """A release package brings game.cfg and mods/ in kit-files/: the assistant takes them (Paul 01.11.)."""
        with tempfile.TemporaryDirectory() as tmp:
            write_zip(Path(tmp, "kit.zip"), {n: d for n, d in FILES.items()
                                             if n != "game.cfg" and not n.startswith("mods/")})
            bundled = Path(tmp, "kit-files")
            (bundled / "mods").mkdir(parents=True)
            (bundled / "game.cfg").write_text("c")
            (bundled / "mods" / "wild-battles.cfg").write_text("w")
            asked = []

            def answer(prompt=""):
                if "game.cfg and mods/" in prompt:
                    asked.append(prompt)
                    return ""
                if "dump" in prompt or "game folder" in prompt:
                    return str(Path(tmp, "kit.zip"))
                if "Start?" in prompt:
                    return "n"
                return ""

            check = kb.Toolchain.check
            kb.Toolchain.check = lambda tc: True
            kb.input = answer
            try:
                out = io.StringIO()
                with redirect_stdout(out), mock.patch.object(kb, "BUNDLED_KIT_FILES", bundled), \
                        mock.patch.dict(os.environ, {"LGP_KIT_URL": ""}):
                    rc = kb.main([])
            finally:
                kb.Toolchain.check = check
                del kb.input
            self.assertEqual(rc, 1)
            self.assertEqual(asked, [])
            self.assertIn("come from the package", out.getvalue())
            self.assertLessEqual(out.getvalue().count("No pipeline seed"), 1)
            self.assertIn("[3/3] Building", out.getvalue())

    def test_neutral_version_message(self):
        with self.assertRaises(kb.KitError) as e:
            kb.check_kit({"program_id": "0100000000000000", "exefs": []}, set())
        self.assertIn("not one of the supported game versions", str(e.exception))
        for title in ("Pikachu", "Eevee"):
            self.assertNotIn(title, str(e.exception))


class XtoolVersion(unittest.TestCase):
    def check(self, stdout):
        done = subprocess.CompletedProcess([], 0, stdout=stdout, stderr="")
        with mock.patch.object(kb.subprocess, "run", return_value=done):
            return kb.xtool_version("xtool")

    def test_version_is_read_not_judged(self):
        self.assertEqual(self.check("1.21.0\n"), "1.21.0")
        self.assertIsNone(self.check(""))


class AssistantMacSideload(unittest.TestCase):
    """On a Mac without an Apple team in Xcode (or with a free one) the assistant offers the sideload .ipa."""

    def ask_build(self, teams, answers):
        seen = []
        real = kb.parse_args

        def record(argv):
            seen.append(list(argv))
            return real(argv)

        def answer(prompt=""):
            return next((a for key, a in answers.items() if key in prompt), "")

        kb.input = answer
        try:
            with mock.patch.object(kb.sys, "platform", "darwin"), mock.patch.object(kb, "xcode_teams", lambda: teams), \
                    mock.patch.object(kb, "parse_args", record), redirect_stdout(io.StringIO()):
                rc = kb.assistant_build(["build", "--kit", "k.zip", "--target", "ios"], "ios", kb.Toolchain("ios"))
        finally:
            del kb.input
        self.assertEqual(rc, 1)  # stopped at "Start?"
        return seen[-1]

    def test_no_team_offers_sideload(self):
        argv = self.ask_build([], {"sideloader": "j", "Start?": "n"})
        self.assertIn("sideload", argv)
        self.assertNotIn("--ios-team", argv)

    def test_free_team_may_sideload(self):
        free = [{"teamID": "FREE123456", "teamName": "Max Muster (Personal Team)", "free": True}]
        argv = self.ask_build(free, {"sideloader": "j", "Start?": "n"})
        self.assertIn("sideload", argv)
        argv = self.ask_build(free, {"sideloader": "n", "Start?": "n"})
        self.assertEqual(argv[argv.index("--ios-team") + 1], "FREE123456")
        self.assertIn("--no-cloud-saves", argv)

    def test_paid_team_may_sideload(self):
        paid = [{"teamID": "PAID123456", "teamName": "Max Muster", "free": False}]
        argv = self.ask_build(paid, {"sideloader": "j", "Start?": "n"})
        self.assertIn("sideload", argv)
        self.assertNotIn("--ios-team", argv)

    def test_command_line_sideload_skips_the_question(self):
        paid = [{"teamID": "PAID123456", "teamName": "Max Muster", "free": False}]
        with mock.patch.object(kb, "ASSISTANT_INSTALLER", "sideload"):
            argv = self.ask_build(paid, {"sideloader": "n", "Start?": "n"})
        self.assertEqual(argv[argv.index("--installer") + 1], "sideload")
        with mock.patch.object(kb, "assistant", lambda: kb.ASSISTANT_INSTALLER and 7), \
                mock.patch.object(kb, "ASSISTANT_INSTALLER", ""):
            self.assertEqual(kb.main(["--installer", "sideload"]), 7)


class Install(unittest.TestCase):
    def run_main(self, *argv):
        out = io.StringIO()
        with redirect_stdout(out):
            rc = kb.main(list(argv))
        return rc, out.getvalue()

    def test_android_installs_all_splits(self):
        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp, "android")
            folder.mkdir()
            for name in ("lgp_game.apk", "game-exefs.apk", "game-romfs-1.apk", "game-romfs-0.apk"):
                (folder / name).write_bytes(b"apk")
            rc, out = self.run_main("install", "--target", "android", "--work", tmp, "--dry-run")
        self.assertEqual(rc, 0, out)
        cmd = next(line for line in out.splitlines() if "install-multiple" in line)
        names = [Path(a).name for a in cmd.split() if a.endswith(".apk")]
        self.assertEqual(names, ["lgp_game.apk", "game-exefs.apk", "game-romfs-0.apk", "game-romfs-1.apk"])

    def test_ios_without_a_mac_explains_the_sideloader(self):
        with tempfile.TemporaryDirectory() as tmp, mock.patch.object(kb.sys, "platform", "linux"):
            rc, out = self.run_main("install", "--target", "ios", "--work", tmp, "--dry-run")
        self.assertEqual(rc, 0, out)
        self.assertIn("sideloader", next(line for line in out.splitlines() if "lgp_game.ipa" in line))
        self.assertNotIn("$ xtool", out)

    def test_ios_without_a_mac_uses_xtool(self):
        with tempfile.TemporaryDirectory() as tmp, mock.patch.object(kb.sys, "platform", "linux"):
            rc, out = self.run_main("install", "--target", "ios", "--installer", "xtool", "--work", tmp, "--dry-run")
        self.assertEqual(rc, 0, out)
        self.assertIn("xtool", next(line for line in out.splitlines() if "lgp_game.ipa" in line))

    def test_missing_build_is_explained(self):
        with tempfile.TemporaryDirectory() as tmp, mock.patch.object(kb.sys, "platform", "linux"):
            rc, out = self.run_main("install", "--target", "ios", "--work", tmp)
        self.assertEqual(rc, 1)
        self.assertIn("run `build.py build --target ios` first", out)

    def test_xcode_teams(self):
        import plistlib
        prefs = {"IDEProvisioningTeamByIdentifier": {"acc-1": [
            {"teamID": "FREE000001", "teamName": "Max Muster (Personal Team)", "isFreeProvisioningTeam": True},
            {"teamID": "ABCDE12345", "teamName": "Firma", "isFreeProvisioningTeam": False}]}}
        done = mock.Mock(stdout=plistlib.dumps(prefs))
        with mock.patch.object(kb.sys, "platform", "darwin"), mock.patch.object(kb.subprocess, "run", return_value=done):
            teams = kb.xcode_teams()
        self.assertEqual({t["teamID"]: t["free"] for t in teams}, {"FREE000001": True, "ABCDE12345": False})


class TtyStdin(io.StringIO):
    def isatty(self):
        return True


class WindowsToWsl(unittest.TestCase):
    """iPhone/iPad from Windows: the assistant unpacks there and continues in WSL (not run on a real PC yet)."""

    @unittest.skipIf(os.name == "nt", "the WSL side runs on Linux (Windows paths do not start with /mnt/)")
    def test_paths_dragged_from_explorer_into_wsl(self):
        with mock.patch.object(kb, "in_wsl", lambda: True):
            self.assertEqual(kb.dropped_path(r'"D:\Spiele\Pokémon\lgp.nsp"'), Path("/mnt/d/Spiele/Pokémon/lgp.nsp"))
            self.assertEqual(kb.dropped_path("/home/a/x.nsp"), Path("/home/a/x.nsp"))
        with mock.patch.object(kb, "in_wsl", lambda: False), mock.patch.object(kb, "REPO", Path("/mnt/d/lgp-recomp")), \
                mock.patch.dict(os.environ, {"LGP_PRIVATE_DIR": ""}):
            self.assertEqual(kb.default_work(), Path("/mnt/d/lgp-recomp/build/kit"))
        with mock.patch.object(kb, "in_wsl", lambda: True), mock.patch.object(kb, "REPO", Path("/mnt/d/lgp-recomp")), \
                mock.patch.dict(os.environ, {"LGP_PRIVATE_DIR": ""}):
            self.assertEqual(kb.default_work(), Path.home() / "lgp-kit-build")

    def test_hands_the_kit_to_wsl(self):
        calls = []

        def run(cmd, **kw):
            calls.append((cmd, kw.get("env")))
            return mock.Mock(returncode=0, stdout="/mnt/d/lgp-recomp\n")

        with tempfile.TemporaryDirectory() as tmp, mock.patch.object(kb, "wsl_distro", lambda: "Ubuntu-24.04"), \
                mock.patch.object(kb.subprocess, "run", run), mock.patch.object(kb.sys, "stdin", TtyStdin()), \
                redirect_stdout(io.StringIO()):
            rc = kb.assistant_via_wsl(["--kit", tmp])
        self.assertEqual(rc, 0)
        cmd, env = calls[-1]
        self.assertEqual(cmd[:6], ["wsl.exe", "-d", "Ubuntu-24.04", "--cd", "~", "--exec"])
        self.assertEqual(cmd[-1], "/mnt/d/lgp-recomp/tools/kit/build.py")
        wslpath = calls[0][0]  # --exec and slashes: the default shell ate D:\… (PC, 2026-10-25)
        self.assertEqual(wslpath[3:6], ["--exec", "wslpath", "-a"])
        self.assertNotIn("\\", wslpath[-1])
        self.assertEqual(env["LGP_KIT_TARGET"], "ios")
        self.assertIn("LGP_KIT_KIT/p", env["WSLENV"].split(":"))  # WSL translates the Windows path

    def test_piped_answers_reach_wsl(self):
        """Answers from a file: Python on Windows buffers them all, WSL read EOF (PC, 2026-10-26)."""
        inputs = []

        def run(cmd, **kw):
            inputs.append(kw.get("input"))
            return mock.Mock(returncode=0, stdout="/mnt/d/lgp-recomp\n")

        with tempfile.TemporaryDirectory() as tmp, mock.patch.object(kb, "wsl_distro", lambda: "Ubuntu-24.04"), \
                mock.patch.object(kb.subprocess, "run", run), \
                mock.patch.object(kb.sys, "stdin", io.StringIO("de.example\nj\n")), redirect_stdout(io.StringIO()):
            self.assertEqual(kb.assistant_via_wsl(["--kit", tmp]), 0)
        self.assertEqual(inputs[-1], "de.example\nj\n")

    @unittest.skipIf(os.name == "nt", "the WSL side runs on Linux (Windows paths do not start with /mnt/)")
    def test_mirrors_the_repo_out_of_mnt(self):
        """Building from /mnt/… crawled (40+ min configure on the PC); the copy in the WSL home took 73 s."""
        with tempfile.TemporaryDirectory() as tmp:
            src, dst = Path(tmp, "src"), Path(tmp, "dst")
            for rel in ("tools/kit/build.py", "cmake/a.cmake", ".git/HEAD", "build/kit/x", "bauset/n.exe",
                        "tools/__pycache__/c.pyc"):
                (src / rel).parent.mkdir(parents=True, exist_ok=True)
                (src / rel).write_text(rel)
            (src / ".git").rename(src / ".gitdir")
            (src / ".git").write_text("gitdir: /mnt/d/repo/.git/worktrees/x")  # a worktree: .git is a file
            (dst / "build").mkdir(parents=True)
            (dst / "build" / "keep").write_text("work")
            (dst / "old.txt").write_text("gone from src")
            self.assertEqual(kb.mirror_repo(src, dst), 3)  # build.py, a.cmake, .gitdir/HEAD
            self.assertEqual((dst / "cmake/a.cmake").read_text(), "cmake/a.cmake")
            for rel in (".git", "bauset", "tools/__pycache__", "old.txt", "build/kit"):  # .git file left out too
                self.assertFalse((dst / rel).exists(), rel)
            self.assertTrue((dst / "build/keep").exists())  # build output in the copy stays
            self.assertEqual(kb.mirror_repo(src, dst), 0)  # unchanged: nothing copied

            calls = []
            with mock.patch.object(kb, "in_wsl", lambda: True), mock.patch.object(kb, "REPO", src), \
                    mock.patch.dict(os.environ, {"LGP_KIT_WSL_REPO": str(dst)}), \
                    mock.patch.object(kb.os, "execve", lambda *a: calls.append(a)), redirect_stdout(io.StringIO()):
                self.assertIsNone(kb.run_from_wsl_mirror([]))  # src is not under /mnt/
                with mock.patch.object(kb, "REPO", Path("/mnt/d/lgp-recomp")), \
                        mock.patch.object(kb, "mirror_repo", lambda a, b: 0):
                    self.assertEqual(kb.run_from_wsl_mirror(["build", "--kit", "k"]), 0)
            exe, args, env = calls[0]
            self.assertEqual(args[1:], [str(dst / "tools/kit/build.py"), "build", "--kit", "k"])
            self.assertEqual(env["LGP_KIT_WSL_MIRROR"], "0")
            self.assertEqual(env["LGP_KIT_REPO_ORIGIN"], "/mnt/d/lgp-recomp")
            with mock.patch.object(kb, "in_wsl", lambda: True), mock.patch.object(kb, "REPO", dst), \
                    mock.patch.dict(os.environ, {"LGP_PRIVATE_DIR": "", "LGP_KIT_REPO_ORIGIN": "/mnt/d/x"}):
                self.assertEqual(kb.default_work(), Path.home() / "lgp-kit-build")

    def test_cache_from_another_source_folder_is_dropped(self):
        """The /mnt/… build tree of an assistant run before e0d0aa2 would stop CMake in the mirror."""
        with tempfile.TemporaryDirectory() as tmp:
            tree = Path(tmp)
            (tree / "CMakeFiles").mkdir()
            (tree / "CMakeCache.txt").write_text("CMAKE_HOME_DIRECTORY:INTERNAL=/mnt/d/lgp-recomp\n")
            with redirect_stdout(io.StringIO()) as out:
                kb.drop_stale_cache(tree, False)
            self.assertFalse((tree / "CMakeCache.txt").exists())
            self.assertIn("another source folder", out.getvalue())
            (tree / "CMakeCache.txt").write_text(f"CMAKE_HOME_DIRECTORY:INTERNAL={kb.REPO}\n")
            kb.drop_stale_cache(tree, False)
            self.assertTrue((tree / "CMakeCache.txt").exists())  # same source: kept

    def test_without_wsl_offers_the_install(self):
        kb.input = lambda prompt="": "n"
        try:
            with mock.patch.object(kb, "wsl_distro", lambda: None), redirect_stdout(io.StringIO()) as out:
                rc = kb.assistant_via_wsl(["--kit", "x"])
        finally:
            del kb.input
        self.assertEqual(rc, 1)
        self.assertIn("WSL with Ubuntu is still missing", out.getvalue())

    @unittest.skipIf(os.name == "nt", "the WSL side runs on Linux (Windows paths do not start with /mnt/)")
    def test_wsl_side_skips_the_questions_windows_asked(self):
        asked = []

        def answer(prompt=""):
            asked.append(prompt)
            return "n" if "Start?" in prompt else ""

        with tempfile.TemporaryDirectory() as tmp:
            write_zip(Path(tmp, "kit.zip"), FILES)
            kb.input = answer
            try:
                with mock.patch.dict(os.environ, {"LGP_KIT_TARGET": "ios", "LGP_KIT_KIT": str(Path(tmp, "kit.zip"))}), \
                        mock.patch.object(kb.Toolchain, "check", lambda tc: True), \
                        mock.patch.object(kb.sys, "platform", "linux"), redirect_stdout(io.StringIO()) as out:
                    rc = kb.main([])
            finally:
                del kb.input
        self.assertEqual(rc, 1, out.getvalue())
        self.assertFalse(any("Build for what" in a or "dump" in a for a in asked), asked)
        self.assertTrue(any("bundle ID" in a for a in asked), asked)



class EnsureTools(unittest.TestCase):
    """The assistant's tool step: each offer once, an optional tool (FFmpeg) may be declined or fail."""

    class FakeToolchain:
        def __init__(self, required=(), optional=()):
            self.required, self.opt = list(required), list(optional)
            self.notes, self.checks = [], 0

        def check(self):
            self.checks += 1
            self.problems, self.optional = list(self.required), list(self.opt)
            self.installs = [("fetch " + p, ["fetch", p]) for p in self.problems + self.optional]
            return not self.problems

    def run_ensure(self, tc, answer, rc=1):
        kb.input = lambda prompt="": answer
        try:
            with mock.patch.object(kb.subprocess, "run", lambda cmd: mock.Mock(returncode=rc)), \
                    redirect_stdout(io.StringIO()) as out:
                ok = kb.ensure_tools(tc)
        finally:
            del kb.input
        return ok, out.getvalue()

    def test_declining_ffmpeg_goes_on(self):
        ok, out = self.run_ensure(self.FakeToolchain(optional=["FFmpeg"]), "n")
        self.assertTrue(ok, out)
        self.assertIn("Going on without it", out)

    def test_failed_optional_install_is_offered_once(self):
        tc = self.FakeToolchain(optional=["FFmpeg"])
        ok, out = self.run_ensure(tc, "y")
        self.assertTrue(ok, out)
        self.assertEqual(out.count("I can install this"), 1, out)
        self.assertIn("exit code 1", out)
        self.assertNotIn("winget", out)

    def test_failed_required_install_stops_once(self):
        tc = self.FakeToolchain(required=["CMake"])
        ok, out = self.run_ensure(tc, "y")
        self.assertFalse(ok)
        self.assertEqual(out.count("I can install this"), 1, out)
        self.assertIn("Please install what is missing", out)


@unittest.skipIf(os.name == "nt", "a Linux check")
class LinuxDesktopTools(unittest.TestCase):
    """The Linux desktop build brings libvpx along (videos without FFmpeg), so it needs make and perl."""

    def test_make_and_perl_for_libvpx(self):
        with tempfile.TemporaryDirectory() as tmp:
            for name in ("clang", "clang++"):
                Path(tmp, name).write_text("#!/bin/sh\n")
                os.chmod(Path(tmp, name), 0o755)
            env = {"PATH": tmp, "HOME": tmp, "LGP_KIT_TOOLS": str(Path(tmp, "cache"))}
            real_which = kb.shutil.which
            which = lambda n, *a, **kw: "/usr/bin/apt-get" if n == "apt-get" else real_which(n, *a, **kw)
            with mock.patch.dict(os.environ, env, clear=True), mock.patch.object(kb.sys, "platform", "linux"), \
                    mock.patch.object(kb.shutil, "which", which):
                tc = kb.Toolchain("desktop")
                tc.check()
        self.assertTrue(any("make is missing (for libvpx" in p for p in tc.problems), tc.problems)
        self.assertTrue(any("perl is missing (for libvpx" in p for p in tc.problems), tc.problems)
        self.assertIn(["sudo", "apt-get", "install", "-y", "make", "perl", "nasm"], [c for _, c in tc.installs])


@unittest.skipIf(os.name == "nt", "the iOS build without a Mac runs on Linux/WSL")
class IosCrossTools(unittest.TestCase):
    """clang ≥ 19 for the SDK's libc++; apt offers the newest clang-N it has (PC, Ubuntu 26.04: clang 18 failed)."""

    def setUp(self):
        self.bin = Path(tempfile.mkdtemp())
        for name in ("ld64.lld-21", "llvm-ar-21", "llvm-ranlib-21", "xtool"):
            (self.bin / name).write_text("#!/bin/sh\n")
            os.chmod(self.bin / name, 0o755)

    def clang(self, name, version):
        (self.bin / name).write_text(f"#!/bin/sh\necho 'Ubuntu clang version {version}.1.0'\n")
        os.chmod(self.bin / name, 0o755)

    def check(self, apt_has=None):
        def apt_cache(cmd, **kw):
            ok = apt_has is not None and cmd[:2] == ["apt-cache", "show"] and cmd[2] == f"clang-{apt_has}"
            return mock.Mock(returncode=0 if ok else 100, stdout="Package: x" if ok else "")
        real_run = kb.subprocess.run
        run = lambda cmd, **kw: apt_cache(cmd, **kw) if cmd[0] == "apt-cache" else real_run(cmd, **kw)
        env = {"PATH": str(self.bin), "HOME": str(self.bin), "LGP_KIT_TOOLS": str(self.bin / "cache")}
        real_which = kb.shutil.which
        which = lambda n, *a, **kw: "/usr/bin/apt-get" if n == "apt-get" else real_which(n, *a, **kw)
        with mock.patch.dict(os.environ, env, clear=True), mock.patch.object(kb.sys, "platform", "linux"), \
                mock.patch.object(kb.shutil, "which", which), \
                mock.patch.object(kb.subprocess, "run", run), mock.patch.object(kb, "ios_sdk", lambda: self.bin):
            tc = kb.Toolchain("ios")
            tc.check()
        return tc

    def test_clang_18_is_too_old(self):
        self.clang("clang", 18)
        tc = self.check(apt_has=20)
        self.assertTrue(any("clang ≥ 19 is missing (found: clang 18)" in p for p in tc.problems), tc.problems)
        cmds = [c for _, c in tc.installs]
        self.assertIn(["sudo", "apt-get", "install", "-y", "clang-20", "lld-20", "llvm-20", "make", "perl",
                       "python3-pil", "usbmuxd"], cmds)

    def test_newest_versioned_clang_wins(self):
        self.clang("clang", 18)
        self.clang("clang-21", 21)
        tc = self.check()
        self.assertFalse(any("clang ≥" in p for p in tc.problems), tc.problems)
        self.assertFalse(any("ld64.lld" in p for p in tc.problems), tc.problems)

    def test_signer_needed_xtool_only_noted(self):
        """xtool takes the entitlements from the executable's signature, so make_ipa needs rcodesign; xtool itself
        only installs (PC, 2026-10-25: the build stopped without it)."""
        self.clang("clang-21", 21)
        (self.bin / "xtool").unlink()
        tc = self.check()
        self.assertTrue(any("rcodesign is missing" in p for p in tc.problems), tc.problems)
        self.assertFalse(any("xtool is missing" in p for p in tc.problems), tc.problems)
        # The SDK is there (ios_sdk patched), so xtool is not needed at all: installing goes through a sideloader.
        self.assertFalse(any("xtool is needed" in n for n in tc.notes), tc.notes)

        self.assertTrue(any(c[-1] == "fetch-rcodesign" for _, c in tc.installs), tc.installs)
        with mock.patch.dict(os.environ, {"LGP_KIT_TOOLS": str(self.bin / "cache")}):
            d = kb.rcodesign_dir()
            d.mkdir(parents=True)
            (d / "rcodesign").write_text("#!/bin/sh\n")
            os.chmod(d / "rcodesign", 0o755)
        tc = self.check()
        self.assertFalse(any("rcodesign" in p for p in tc.problems), tc.problems)


class PortableAndWeak(unittest.TestCase):
    """Paul 2026-10-26: runs on weak machines and installs nothing for good."""

    def test_jobs_follow_free_memory(self):
        self.assertEqual(kb.auto_jobs(16, 4096), (4096 - kb.RESERVE_MIB) // kb.JOB_MIB)  # many cores, little RAM
        self.assertEqual(kb.auto_jobs(4, 64 * 1024), 4)  # plenty of RAM: one per core
        self.assertEqual(kb.auto_jobs(8, 1500), 1)  # never below one
        self.assertEqual(kb.auto_jobs(6, None), 6)  # unknown: as before

    def test_jobs_reach_every_cmake_build(self):
        with tempfile.TemporaryDirectory() as tmp:
            tc = kb.Toolchain("desktop")
            tc.env.pop("CMAKE_BUILD_PARALLEL_LEVEL", None)
            args = kb.parse_args(["build", "--kit", tmp, "--work", tmp, "--jobs", "3", "--dry-run"])
            builder = kb.Builder(args, tc)
            self.assertEqual(tc.env["CMAKE_BUILD_PARALLEL_LEVEL"], "3")
            self.assertIn("with --jobs", builder.jobs_note)
            # Without --measure the launchers are cleared, so a measured build's cache does not keep its log
            self.assertTrue(builder.launcher_args())
            self.assertTrue(all(a.endswith("_LAUNCHER=") for a in builder.launcher_args()))
            args = kb.parse_args(["build", "--kit", tmp, "--work", tmp, "--measure", "m.tsv", "--dry-run"])
            launchers = kb.Builder(args, tc).launcher_args()
            self.assertIn("-DCMAKE_CXX_LINKER_LAUNCHER=", launchers[-1])
            self.assertIn("measure_cc.py", launchers[1])

    def test_one_job_from_little_memory_says_so(self):
        """PC 2026-11-04: 1 job (0.8 GB free, a VM still running) took 26 minutes instead of 7, without a word why."""
        with tempfile.TemporaryDirectory() as tmp:
            def note(free, extra=()):
                tc = kb.Toolchain("desktop")
                tc.env.pop("CMAKE_BUILD_PARALLEL_LEVEL", None)
                args = kb.parse_args(["build", "--kit", tmp, "--work", tmp, "--dry-run", *extra])
                with mock.patch.object(kb, "free_memory_mib", lambda: free), \
                        mock.patch.object(kb.os, "cpu_count", lambda: 24):
                    return kb.Builder(args, tc).jobs_note
            self.assertIn("Closing other programs", note(800))
            self.assertNotIn("Closing other programs", note(16000))
            self.assertNotIn("Closing other programs", note(800, ["--jobs", "1"]))  # asked for it

    def test_free_memory_is_read(self):
        if sys.platform.startswith("linux"):
            self.assertGreater(kb.free_memory_mib() or 0, 0)

    def test_future_modification_times_are_set_to_now(self):
        """A package unpacked west of where it was packed holds files ahead of the clock; Ninja then never settles."""
        import time as _time
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for rel in ("a.cpp", "sub/b.h", "work/game.bin", ".git/HEAD", "build/x.o"):
                (root / rel).parent.mkdir(parents=True, exist_ok=True)
                (root / rel).write_text("")
            ahead = _time.time() + 3600
            for rel in ("a.cpp", "sub/b.h", "work/game.bin", ".git/HEAD", "build/x.o"):
                os.utime(root / rel, (ahead, ahead))
            past = _time.time() - 3600
            (root / "old.txt").write_text("")
            os.utime(root / "old.txt", (past, past))
            self.assertEqual(kb.clamp_future_mtimes(root, skip=(root / "work",)), 2)
            self.assertLess((root / "a.cpp").stat().st_mtime, ahead - 60)
            self.assertLess((root / "sub" / "b.h").stat().st_mtime, ahead - 60)
            self.assertAlmostEqual((root / "old.txt").stat().st_mtime, past, delta=1)
            for rel in ("work/game.bin", ".git/HEAD", "build/x.o"):  # left alone
                self.assertAlmostEqual((root / rel).stat().st_mtime, ahead, delta=1)

    def test_outside_folder_names_the_tools_there(self):
        with tempfile.TemporaryDirectory() as tmp:
            cache = Path(tmp, "kit-tools")
            (cache / "node-v24.21.0-darwin-arm64").mkdir(parents=True)
            (cache / "x.tmp").mkdir()
            (cache / "node.tar.part").write_text("")
            with mock.patch.object(kb, "tool_cache", lambda: cache), \
                    mock.patch.object(kb, "user_data_dir", lambda: Path(tmp, "none")):
                lines = kb.outside_folder(Path(tmp, "work-missing"))
            self.assertEqual(lines, [f"{cache}: tools (node-v24.21.0-darwin-arm64)"])

    def test_package_offers_no_installs(self):
        tc = kb.Toolchain("desktop")
        tc.problems = ["CMake ≥ 3.25 is missing", "Ninja is missing", f"JDK ≥ {kb.MIN_JDK} is missing", "LLVM/clang is missing"]
        with mock.patch.object(kb, "portable", lambda: True), mock.patch.object(kb.shutil, "which", lambda n: n):
            tc.plan_installs()
        commands = [" ".join(c) for _, c in tc.installs]
        self.assertFalse(any("winget" in c or "pip" in c for c in commands), commands)
        self.assertTrue(any(c.endswith("fetch-jdk") for c in commands), commands)

    def test_jdk_unpacks_into_the_tool_cache(self):
        import tarfile
        with tempfile.TemporaryDirectory() as tmp:
            src = Path(tmp, "src")
            (src / "jdk-x" / "bin").mkdir(parents=True)
            (src / "jdk-x" / "bin" / kb.exe_name("javac")).write_text("")
            archive = Path(tmp, "jdk.tar.gz")
            with tarfile.open(archive, "w:gz") as tf:
                tf.add(src / "jdk-x", "jdk-x")
            host = "linux-x64"
            fake = ("21", "jdk-x", {host: ("tar.gz", "0")})

            def download(url, part, algo, digest, what):
                self.assertIn("aka.ms/download-jdk/", url)
                part.write_bytes(archive.read_bytes())

            with mock.patch.object(kb, "JDK", fake), mock.patch.object(kb, "jdk_host", lambda: host), \
                    mock.patch.object(kb, "tool_cache", lambda: Path(tmp, "cache")), \
                    mock.patch.object(kb, "download", download), redirect_stdout(io.StringIO()):
                home = kb.fetch_jdk()
                self.assertEqual(home, Path(tmp, "cache", "microsoft-jdk-21-linux-x64"))
                self.assertTrue((home / "bin" / kb.exe_name("javac")).is_file())
                self.assertEqual(kb.fetch_jdk(), home)  # second call: no download
                self.assertFalse(list(Path(tmp, "cache").glob("*.part")))

    def test_outside_folder_lists_what_stays(self):
        with tempfile.TemporaryDirectory() as tmp:
            cache, data = Path(tmp, "cache"), Path(tmp, "data")
            cache.mkdir()
            data.mkdir()
            with mock.patch.object(kb, "tool_cache", lambda: cache), mock.patch.object(kb, "user_data_dir", lambda: data):
                lines = kb.outside_folder(kb.REPO / "build" / "kit")
            self.assertEqual(len(lines), 2)
            self.assertIn("signing key", lines[1])
            with mock.patch.object(kb, "tool_cache", lambda: kb.REPO / "bauset"), \
                    mock.patch.object(kb, "user_data_dir", lambda: Path(tmp, "none")):
                self.assertEqual(kb.outside_folder(kb.REPO / "build" / "kit"), [])


class MeasureCc(unittest.TestCase):
    def test_launch_and_report(self):
        sys.path.insert(0, str(kb.REPO / "tools" / "kit"))
        import measure_cc
        if os.name == "nt":
            self.skipTest("POSIX path; Windows uses the job object")
        with tempfile.TemporaryDirectory() as tmp:
            log = Path(tmp, "m.tsv")
            rc = measure_cc.launch(log, [sys.executable, "-c", "x = bytearray(64 << 20)", "-o", "a.o"])
            self.assertEqual(rc, 0)
            self.assertEqual(measure_cc.launch(log, [sys.executable, "-c", "raise SystemExit(3)"]), 3)
            rows = measure_cc.read_log(log)
            self.assertEqual([r["exit"] for r in rows], [0, 3])
            self.assertEqual(rows[0]["output"], "a.o")
            self.assertGreaterEqual(rows[0]["peak_mib"], 64)
            text = measure_cc.report(log, 1)
            self.assertIn("Steps: 2, failed: 1", text)
            self.assertIn("a.o", text)

if __name__ == "__main__":
    unittest.main()
