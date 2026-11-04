"""Unit tests for tools/ios/make_ipa.py on a made-up .app (no SDK, no signer) and tools/ios/patch_sdk.py on a
made-up SDK.

Run: python -m unittest -v test_make_ipa (from tests/tools)
"""
import io
import os
import plistlib
import stat
import sys
import tempfile
import unittest
import zipfile
from contextlib import redirect_stdout
from pathlib import Path
from unittest import mock

HERE = os.path.dirname(__file__)
sys.path.insert(0, os.path.join(HERE, "..", "..", "tools", "ios"))
import make_ipa  # noqa: E402
import patch_sdk  # noqa: E402


def signed_macho(xml: bytes) -> bytes:
    """A thin arm64 Mach-O header with only LC_CODE_SIGNATURE and a SuperBlob holding the entitlements (slot 5)."""
    le = lambda v: v.to_bytes(4, "little")
    be = lambda v: v.to_bytes(4, "big")
    blob = be(0xFADE7171) + be(8 + len(xml)) + xml
    superblob = be(0xFADE0CC0) + be(12 + 8 + len(blob)) + be(1) + be(5) + be(20) + blob
    header = le(0xFEEDFACF) + le(0x0100000C) + le(0) + le(2) + le(1) + le(16) + le(0) + le(0)
    return header + le(0x1D) + le(16) + le(48) + le(len(superblob)) + superblob


class MakeIpa(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp()).resolve()  # Windows runners: TEMP as an 8.3 short path
        self.app = self.tmp / "lgp_game.app"
        (self.app / "game" / "exefs").mkdir(parents=True)
        with open(self.app / "Info.plist", "wb") as f:
            plistlib.dump({"CFBundleExecutable": "lgp_game"}, f)
        (self.app / "lgp_game").write_bytes(b"\xcf\xfa\xed\xfe")
        os.chmod(self.app / "lgp_game", 0o755)
        (self.app / "game" / "exefs" / "main").write_bytes(b"m")
        self.ents = self.tmp / "lgp_game.entitlements"
        self.ents.write_text("<plist/>")

    def test_payload_layout_and_modes(self):
        out = self.tmp / "x.ipa"
        with redirect_stdout(io.StringIO()):
            make_ipa.main(["--app", str(self.app), "--out", str(out), "--signer", "none"])
        with zipfile.ZipFile(out) as z:
            names = set(z.namelist())
            mode = z.getinfo("Payload/lgp_game.app/lgp_game").external_attr >> 16
        self.assertEqual(names, {"Payload/lgp_game.app/Info.plist", "Payload/lgp_game.app/lgp_game",
                                 "Payload/lgp_game.app/game/exefs/main"})
        if os.name != "nt":
            self.assertTrue(mode & stat.S_IXUSR, "the executable stays executable")
        self.assertFalse(out.with_name("x.ipa.part").exists())

    def test_signs_with_the_entitlements_when_a_signer_is_there(self):
        calls = []
        with mock.patch.object(make_ipa.shutil, "which", lambda t: f"/bin/{t}" if t == "ldid" else None), \
                mock.patch.object(make_ipa.subprocess, "run", lambda cmd: calls.append(cmd) or mock.Mock(returncode=0)), \
                redirect_stdout(io.StringIO()):
            make_ipa.main(["--app", str(self.app), "--out", str(self.tmp / "x.ipa"),
                           "--entitlements", str(self.ents)])
        self.assertEqual(calls, [["/bin/ldid", f"-S{self.ents}", str(self.app / "lgp_game")]])

    def test_codesign_replaces_the_signature(self):
        calls = []
        with mock.patch.object(make_ipa.shutil, "which", lambda t: f"/usr/bin/{t}" if t == "codesign" else None), \
                mock.patch.object(make_ipa.subprocess, "run", lambda cmd: calls.append(cmd) or mock.Mock(returncode=0)), \
                redirect_stdout(io.StringIO()):
            make_ipa.main(["--app", str(self.app), "--out", str(self.tmp / "x.ipa"),
                           "--entitlements", str(self.ents), "--signer", "codesign"])
        self.assertEqual(calls, [["/usr/bin/codesign", "--force", "--sign", "-", "--entitlements", str(self.ents),
                                  str(self.app / "lgp_game")]])

    def test_show_reads_signature_and_profile(self):
        ents = {"com.apple.developer.kernel.increased-memory-limit": True, "get-task-allow": True}
        (self.app / "lgp_game").write_bytes(signed_macho(plistlib.dumps(ents)))
        (self.app / "embedded.mobileprovision").write_bytes(
            b"\x30\x82cms" + plistlib.dumps({"Entitlements": {"application-identifier": "T.x"}}) + b"\x00sig")
        self.assertEqual(make_ipa.macho_entitlements((self.app / "lgp_game").read_bytes()), ents)
        out = self.tmp / "x.ipa"
        with redirect_stdout(io.StringIO()):
            make_ipa.main(["--app", str(self.app), "--out", str(out), "--signer", "none"])
        text = io.StringIO()
        with redirect_stdout(text):
            make_ipa.main(["--show", str(out)])
        self.assertIn("com.apple.developer.kernel.increased-memory-limit = True", text.getvalue())
        self.assertIn("application-identifier = 'T.x'", text.getvalue())
        text = io.StringIO()
        (self.tmp / "plain").write_bytes(b"\xcf\xfa\xed\xfe" + bytes(60))
        with redirect_stdout(text):
            make_ipa.main(["--show", str(self.tmp / "plain")])
        self.assertIn("signature: no entitlements", text.getvalue())

    def test_not_an_app(self):
        with self.assertRaises(SystemExit):
            make_ipa.main(["--app", str(self.tmp), "--out", str(self.tmp / "x.ipa")])



YAML_TBD = """--- !tapi-tbd
tbd-version:     4
targets:         [ arm64e.x1-ios, arm64e-ios ]
uuids:
  - target:          arm64e.x1-ios
    value:           00000000-0000-0000-0000-000000000000
install-name:    '/usr/lib/libSystem.B.dylib'
exports:
  - targets:         [ arm64e.x1-ios,
                       arm64e-ios ]
    symbols:         [ _arm64e_lookalike, _malloc ]
...
"""


class PatchSdk(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp()).resolve()  # Windows runners: TEMP as an 8.3 short path
        self.sdk = self.tmp / "iPhoneOS27.1.sdk"
        (self.sdk / "usr" / "lib").mkdir(parents=True)
        (self.sdk / "usr" / "include").mkdir()
        (self.sdk / "SDKSettings.json").write_text("{}")
        (self.sdk / "usr" / "include" / "stdio.h").write_text("/* header */\n")
        (self.sdk / "usr" / "lib" / "libSystem.B.tbd").write_text(YAML_TBD)
        os.symlink("libSystem.B.tbd", self.sdk / "usr" / "lib" / "libSystem.tbd")
        os.symlink("include", self.sdk / "usr" / "headers")
        self.cache = self.tmp / "cache"

    def test_yaml_gets_arm64_twins(self):
        out = patch_sdk.rewrite_tbd(YAML_TBD)
        self.assertNotIn("arm64e.x1", out)
        self.assertIn("targets:         [ arm64-ios, arm64e-ios ]", out)
        self.assertIn("target:          arm64-ios", out)
        self.assertIn("[ _arm64e_lookalike, _malloc ]", out)  # symbol lists stay as they are
        self.assertEqual(patch_sdk.rewrite_tbd("targets: [ arm64-ios ]\n"), "targets: [ arm64-ios ]\n")

    def test_json_gets_arm64_twins(self):
        import json
        tbd = {"tapi_tbd_version": 5, "main_library": {
            "target_info": [{"target": "arm64e-ios", "min_deployment": "27.0"},
                            {"target": "arm64e.x1-ios", "min_deployment": "27.0"}],
            "exported_symbols": [{"targets": ["arm64e.x1-ios"], "data": {"global": ["_f"]}}]}}
        out = json.loads(patch_sdk.rewrite_tbd(json.dumps(tbd)))["main_library"]
        self.assertEqual([t["target"] for t in out["target_info"]], ["arm64e-ios", "arm64-ios"])
        self.assertEqual(out["exported_symbols"][0]["targets"], ["arm64-ios"])
        self.assertEqual(out["exported_symbols"][0]["data"], {"global": ["_f"]})

    def test_ensure_patches_a_copy_once(self):
        out = patch_sdk.ensure(self.sdk, self.cache)
        self.assertNotEqual(out, self.sdk.resolve())
        self.assertEqual(out.name, self.sdk.name)
        self.assertNotIn("arm64e.x1", (out / "usr" / "lib" / "libSystem.B.tbd").read_text())
        self.assertIn("arm64e.x1", (self.sdk / "usr" / "lib" / "libSystem.B.tbd").read_text())  # original untouched
        self.assertTrue((out / "usr" / "lib" / "libSystem.tbd").is_symlink())
        self.assertTrue((out / "usr" / "headers").is_symlink())
        self.assertEqual((out / "usr" / "include" / "stdio.h").read_text(), "/* header */\n")
        (out / "marker").write_text("x")
        self.assertEqual(patch_sdk.ensure(self.sdk, self.cache), out)
        self.assertTrue((out / "marker").exists())  # reused, not rebuilt

    def test_ensure_keeps_an_sdk_that_needs_nothing(self):
        (self.sdk / "usr" / "lib" / "libSystem.B.tbd").write_text("targets: [ arm64-ios, arm64e-ios ]\n")
        self.assertEqual(patch_sdk.ensure(self.sdk, self.cache), self.sdk.resolve())

if __name__ == "__main__":
    unittest.main()
