#!/usr/bin/env python3
"""Writes the VP9 test video of the video backends (P5/P6).

A small self-made WebM, no game data: 64x48, 30 frames at 30 fps, lossless
VP9 (profile 0, 4:2:0, near-lossless, two passes) with alt-ref frames, so
hidden frames inside superframes occur like in real encodes (a moving noise
band across the middle makes the encoder use them). Every frame has four
flat luma quadrants and chroma that differs left/right (Cb) and top/bottom
(Cr), all depending on the frame number, so a test can check size, order,
plane layout and the Cb/Cr order of the decoded NV12 pictures at the
quadrant centres. The pattern is mirrored in tests/platform/video_pattern.hpp;
change both together.

    python3 tools/video/gen_test_webm.py tests/platform/data/vp9_pattern.webm [--ffmpeg ffmpeg]
    python3 tools/video/gen_test_webm.py --audio tests/platform/data/av_pattern.webm

--audio adds a sound track like the games' videos have: Vorbis, 48 kHz
stereo, 1 s, a 440 Hz sine on the left and 660 Hz on the right at half
scale (AUDIO_* below). Needs FFmpeg with libvpx-vp9 (and libvorbis).
"""
import argparse
import os
import random
import subprocess
import sys
import tempfile

WIDTH, HEIGHT, FRAMES, FPS = 64, 48, 30, 30
BAND_ROWS = range(20, 28)  # luma rows with the moving noise (not checked)
AUDIO_RATE, AUDIO_LEFT_HZ, AUDIO_RIGHT_HZ, AUDIO_AMPLITUDE = 48000, 440, 660, 0.5


# All colours stay inside the RGB gamut (BT.601 and BT.709), so the
# backends' RGB fallbacks can reproduce them.
def luma(frame: int, qx: int, qy: int) -> int:
    return 70 + 20 * (qy * 2 + qx) + 3 * frame


def cb(frame: int, right: bool) -> int:
    return 137 - frame % 5 if right else 119 + frame % 5


def cr(frame: int, bottom: bool) -> int:
    return 130 + frame % 5 if bottom else 122


def yuv420_frame(i: int, noise: list) -> bytes:
    out = bytearray()
    for y in range(HEIGHT):
        for x in range(WIDTH):
            if y in BAND_ROWS:
                out.append(noise[(y - BAND_ROWS.start) * 256 + (x + 3 * i) % 256])
            else:
                out.append(luma(i, int(x >= WIDTH // 2), int(y >= HEIGHT // 2)))
    cw, ch = WIDTH // 2, HEIGHT // 2
    for y in range(ch):
        for x in range(cw):
            out.append(cb(i, x >= cw // 2))
    for y in range(ch):
        for x in range(cw):
            out.append(cr(i, y >= ch // 2))
    return bytes(out)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("out")
    ap.add_argument("--ffmpeg", default="ffmpeg")
    ap.add_argument("--audio", action="store_true", help="add the Vorbis sound track")
    args = ap.parse_args()
    rng = random.Random(1)
    noise = [rng.randrange(256) for _ in range(256 * len(BAND_ROWS))]
    raw = b"".join(yuv420_frame(i, noise) for i in range(FRAMES))
    audio_in, audio_out = [], []
    if args.audio:
        expr = (f"{AUDIO_AMPLITUDE}*sin({AUDIO_LEFT_HZ}*2*PI*t)|{AUDIO_AMPLITUDE}*sin({AUDIO_RIGHT_HZ}*2*PI*t)")
        audio_in = ["-f", "lavfi", "-i", f"aevalsrc={expr}:s={AUDIO_RATE}:d={FRAMES / FPS}"]
        audio_out = ["-c:a", "libvorbis", "-q:a", "6"]
    with tempfile.TemporaryDirectory() as tmp:
        for p in (1, 2):
            cmd = [args.ffmpeg, "-hide_banner", "-loglevel", "error", "-y",
                   "-f", "rawvideo", "-pix_fmt", "yuv420p", "-s", f"{WIDTH}x{HEIGHT}", "-r", str(FPS), "-i", "-",
                   *(audio_in if p == 2 else []), *(audio_out if p == 2 else ["-an"]),
                   "-c:v", "libvpx-vp9", "-crf", "4", "-b:v", "0", "-g", "300", "-auto-alt-ref", "1",
                   "-lag-in-frames", "16", "-pass", str(p), "-passlogfile", os.path.join(tmp, "pass"),
                   "-row-mt", "0", "-threads", "1", "-fflags", "+bitexact", "-flags:v", "+bitexact",
                   "-map_metadata", "-1", "-f", "webm", args.out if p == 2 else os.devnull]
            subprocess.run(cmd, input=raw, check=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
