"""Unit test for tools/android/shield_day.py (no device; synthetic adb output only).

Run: python -m unittest -v test_shield_day (from tests/tools)
"""
import os
import re
import sys
import unittest

HERE = os.path.dirname(__file__)
sys.path.insert(0, os.path.join(HERE, "..", "..", "tools", "android"))
import shield_day  # noqa: E402

TEMPLATE = os.path.join(HERE, "..", "..", "platform", "android", "AndroidManifest.xml.in")

BADGING = """package: name='org.lgp.recomp.game' versionCode='1' versionName='0.1'
application-label:'LGP'
application: label='LGP' icon='' banner='res/drawable/banner.xml'
launchable-activity: name='org.lgp.recomp.LgpActivity'  label='LGP' icon=''
leanback-launchable-activity: name='org.lgp.recomp.LgpActivity'  label='LGP' icon='' banner=''
uses-feature: name='android.hardware.vulkan.version'
uses-feature-not-required: name='android.hardware.touchscreen'
uses-feature-not-required: name='android.software.leanback'
native-code: 'arm64-v8a'
"""

THERMAL = """IsStatusOverride: false
Thermal Status: 0
Cached temperatures:
	Temperature{mValue=60.0, mType=0, mName=CPU0, mStatus=0}
HAL Ready: true
HAL connection:
	ThermalHAL 2.0 connected: yes
Current temperatures from HAL:
	Temperature{mValue=33.5, mType=0, mName=CPU0, mStatus=0}
	Temperature{mValue=34.2, mType=1, mName=GPU, mStatus=0}
	Temperature{mValue=45.0, mType=2, mName=battery, mStatus=0}
	Temperature{mValue=31.0, mType=3, mName=skin, mStatus=0}
Current cooling devices from HAL:
	CoolingDevice{mValue=0, mType=2, mName=fan}
"""

MEMINFO = """Applications Memory Usage (in Kilobytes):
** MEMINFO in pid 4242 [org.lgp.recomp.game] **
 App Summary
                       Pss(KB)                        Rss(KB)
                        ------                         ------
           Java Heap:     1200                          9000
         Native Heap:   800000                        800000
            Graphics:   512000                        512000
       Private Other:    10240
              System:     2048
               TOTAL PSS:  1325488            TOTAL RSS:  1400000       TOTAL SWAP PSS:        0
"""


class ShieldDayTest(unittest.TestCase):
    def test_template_fits_tv(self):
        with open(TEMPLATE, encoding="utf-8") as f:
            text = re.sub(r"@[A-Z_]+@", "x", f.read())
        self.assertEqual(shield_day.check_manifest(text), [])

    def test_manifest_problems(self):
        xml = """<manifest xmlns:android="http://schemas.android.com/apk/res/android" package="x">
  <uses-feature android:name="android.hardware.touchscreen" />
  <application><activity android:name="A"><intent-filter>
    <action android:name="android.intent.action.MAIN" />
    <category android:name="android.intent.category.LAUNCHER" />
  </intent-filter></activity></application></manifest>"""
        problems = shield_day.check_manifest(xml)
        self.assertEqual(len(problems), 4, problems)

    def test_badging(self):
        self.assertEqual(shield_day.check_badging(BADGING), [])
        bad = BADGING.replace("leanback-launchable-activity", "x").replace(
            "uses-feature-not-required: name='android.hardware.touchscreen'",
            "uses-feature: name='android.hardware.touchscreen'")
        self.assertEqual(len(shield_day.check_badging(bad)), 2)

    def test_cert(self):
        text = "Signer #1 certificate DN: CN=Android Debug\nSigner #1 certificate SHA-256 digest: " + "ab" * 32 + "\n"
        self.assertEqual(shield_day.cert_sha256(text), "ab" * 32)
        self.assertIsNone(shield_day.cert_sha256("DOES NOT VERIFY"))

    def test_md5_list(self):
        text = "d41d8cd98f00b204e9800998ecf8427e  ./saves-1/.lgp-journal\n0cc175b9c0f1b6a831c399e269772661  ./a b\n"
        self.assertEqual(shield_day.parse_md5_list(text),
                         {"./saves-1/.lgp-journal": "d41d8cd98f00b204e9800998ecf8427e",
                          "./a b": "0cc175b9c0f1b6a831c399e269772661"})
        # coreutils in binary mode (Git for Windows): "<hash> *<name>".
        self.assertEqual(shield_day.parse_md5_list("d41d8cd98f00b204e9800998ecf8427e *saves/.lgp-0\n"),
                         {"saves/.lgp-0": "d41d8cd98f00b204e9800998ecf8427e"})

    def test_temperature(self):
        # HAL values only (not the cached ones), battery ignored.
        self.assertEqual(shield_day.max_temperature(THERMAL), 34.2)
        self.assertEqual(shield_day.max_temperature("", "cpu-thermal 41500\ngpu-thermal 39000\nfake -1\n"), 41.5)
        self.assertIsNone(shield_day.max_temperature("", ""))

    def test_meminfo(self):
        m = shield_day.parse_meminfo(MEMINFO)
        self.assertAlmostEqual(m["total pss"], 1325488 / 1024.0)
        self.assertAlmostEqual(m["graphics"], 500.0)

    def test_log_helpers(self):
        log = ("time: frames 1-100: 4.1 s wall, 24.4 fps\n"
               "gpu memory, frames 1-100: frames in flight 2 (max 2), presents waited 0\n"
               "time: frames 101-200: 4.0 s wall, 25.0 fps\n"
               "gpu memory, frames 101-200: frames in flight 1 (max 3)\n")
        self.assertEqual(shield_day.last_frame(log), 200)
        self.assertEqual(shield_day.last_frame(""), 0)
        self.assertEqual(shield_day.frames_in_flight_max(log), 3)

    def test_saves_md5_command(self):
        # Runs the command on a local tree like the app's files folder (POSIX sh, md5sum).
        import shutil
        import subprocess
        import tempfile
        if not shutil.which("md5sum") or not shutil.which("sh"):
            self.skipTest("needs sh and md5sum")
        with tempfile.TemporaryDirectory() as d:
            for rel in ("saves/.lgp-journal", "saves-2/.lgp-0", "smoke-saves/x/a", "abab-saves/t/r/b", "game/romfs.bin"):
                os.makedirs(os.path.dirname(os.path.join(d, rel)), exist_ok=True)
                with open(os.path.join(d, rel), "w") as f:
                    f.write(rel)
            # "." with cwd: a Windows path (backslashes, drive letter) would not reach sh intact.
            out = subprocess.run(["sh", "-c", shield_day.saves_md5_command(".")], cwd=d, stdout=subprocess.PIPE,
                                 text=True, check=True).stdout
            self.assertEqual(sorted(shield_day.parse_md5_list(out)), ["saves-2/.lgp-0", "saves/.lgp-journal"])
            empty = os.path.join(d, "none")
            os.makedirs(empty)
            out = subprocess.run(["sh", "-c", shield_day.saves_md5_command(".")], cwd=empty, stdout=subprocess.PIPE,
                                 text=True, check=True).stdout
            self.assertEqual(shield_day.parse_md5_list(out), {})

    def test_app_args(self):
        self.assertEqual(shield_day.app_args(["--app", "--press", "100:A,200:B"]), "--app --press 100:A,200:B")
        with self.assertRaises(ValueError):
            shield_day.app_args(["--press", "100:A 200:B"])


    def test_find_systemdata(self):
        import tempfile
        with tempfile.TemporaryDirectory() as d:
            exefs = os.path.join(d, "exefs")
            os.makedirs(exefs)
            self.assertIsNone(shield_day.find_systemdata(exefs))
            fonts = os.path.join(d, "systemdata", shield_day.STD_FONT[0])
            os.makedirs(fonts)
            self.assertIsNone(shield_day.find_systemdata(exefs))  # folder without the standard font
            open(os.path.join(fonts, shield_day.STD_FONT[1]), "wb").close()
            self.assertEqual(shield_day.find_systemdata(exefs), os.path.join(d, "systemdata"))
            other = os.path.join(d, "elsewhere")
            self.assertIsNone(shield_day.find_systemdata(exefs, other))  # an explicit folder wins, even if empty


if __name__ == "__main__":
    unittest.main()
