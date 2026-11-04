"""Unit test for tools/icon/gen_icons.py: the committed app icons (iOS, tvOS,
macOS, Windows, Android, browser, window) exist, have the sizes and kinds the
platforms ask for, stay under the game-data guard's size limit, and the text
files (SVG, Android XML, asset catalog Contents.json) match the generator.

Run: python -m unittest -v test_app_icons (from tests/tools)
"""
import json
import os
import struct
import sys
import unittest
import zlib

HERE = os.path.dirname(__file__)
sys.path.insert(0, os.path.join(HERE, "..", "..", "tools", "icon"))
import gen_icons  # noqa: E402


def _png(w, h, ctype, rows):
    """A tiny PNG with filter 0 rows (bytes per pixel from the colour type)."""
    raw = b"".join(b"\x00" + r for r in rows)

    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, ctype, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


class AppIconsTest(unittest.TestCase):
    def test_committed_icons(self):
        problems = gen_icons.check()
        self.assertEqual(problems, [], "\n".join(problems))

    def test_catalogs_name_existing_files(self):
        out = gen_icons.outputs()
        for rel, spec in out.items():
            if not rel.endswith("Contents.json"):
                continue
            data = json.loads(spec[1])
            base = os.path.dirname(rel)
            for key in ("images", "layers", "assets"):
                for item in data.get(key, []):
                    if "filename" in item:
                        child = f"{base}/{item['filename']}"
                        self.assertTrue(child in out or f"{child}/Contents.json" in out, child)

    def test_platform_rules(self):
        out = gen_icons.outputs()
        ios = json.loads(out["platform/ios/Assets.xcassets/AppIcon.appiconset/Contents.json"][1])
        self.assertEqual(ios["images"][0]["size"], "1024x1024")
        brand = json.loads(out[f"{gen_icons.TV_BRAND}/Contents.json"][1])
        roles = sorted(a["role"] for a in brand["assets"])
        self.assertEqual(roles, ["primary-app-icon", "primary-app-icon", "top-shelf-image", "top-shelf-image-wide"])
        # Android adaptive icon: the mark stays inside the 66 dp safe circle.
        lay = gen_icons.ANDROID_FG
        cx, cy = lay.mx + lay.ms / 2, lay.my + lay.ms / 2
        for x, y in gen_icons.BOLT_POINTS:
            r = ((lay.mx + x * lay.ms - cx) ** 2 + (lay.my + y * lay.ms - cy) ** 2) ** 0.5
            self.assertLessEqual(r, 33.0)
        self.assertLessEqual((gen_icons.RING_RADIUS + gen_icons.RING_WIDTH / 2) * lay.ms, 33.0)

    def test_png_reader(self):
        opaque = _png(2, 1, 6, [bytes([1, 2, 3, 255, 4, 5, 6, 255])])
        clear = _png(2, 1, 6, [bytes([1, 2, 3, 255, 4, 5, 6, 0])])
        self.assertEqual(gen_icons.png_info(opaque), (2, 1, 6))
        self.assertFalse(gen_icons.png_has_alpha_pixels(opaque))
        self.assertTrue(gen_icons.png_has_alpha_pixels(clear))
        with self.assertRaises(ValueError):
            gen_icons.png_info(b"GIF89a" + bytes(30))


if __name__ == "__main__":
    unittest.main()
