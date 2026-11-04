#!/usr/bin/env python3
"""Loose app icons for an iOS build without actool (preset ios-cross, cmake/LgpIos.cmake).

    loose_icons.py <picture.png|.jpg> <out-dir>

Writes the iPhone and iPad sizes as AppIcon<size>@<n>x[~ipad].png (the names CFBundleIconFiles AppIcon60x60,
AppIcon76x76, AppIcon83.5x83.5 resolve to) and prints one file name per line. Scales with Pillow; without it the
picture is copied unscaled under every name (iOS shows it, the home screen scales it; Verdacht, not checked on a
device).
"""
import shutil
import sys
from pathlib import Path

SIZES = {  # file name -> edge in pixels
    "AppIcon60x60@2x.png": 120,
    "AppIcon60x60@3x.png": 180,
    "AppIcon76x76@2x~ipad.png": 152,
    "AppIcon83.5x83.5@2x~ipad.png": 167,
}


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    src, out = Path(argv[0]), Path(argv[1])
    out.mkdir(parents=True, exist_ok=True)
    try:
        from PIL import Image
    except ImportError:
        Image = None
        print("loose_icons: no Pillow, copying the picture unscaled", file=sys.stderr)
    for name, edge in SIZES.items():
        if Image is None:
            if src.suffix.lower() != ".png":
                print(f"loose_icons: {src} is not a PNG and Pillow is missing (pip install Pillow)", file=sys.stderr)
                return 1
            shutil.copyfile(src, out / name)
            continue
        with Image.open(src) as im:
            # No alpha: iOS fills transparent icon pixels with black.
            im.convert("RGB").resize((edge, edge), Image.LANCZOS).save(out / name, "PNG")
    print("\n".join(SIZES))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
