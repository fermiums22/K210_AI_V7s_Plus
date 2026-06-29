#!/usr/bin/env python3
"""Prepare SD-card assets for the C face show.

Input:
  docs/src/faces/f_<emotion>.jpg and f_<emotion>_t.jpg
  docs/src/sound/e_<emotion>.wav

Output:
  sdcard/show/f_<emotion>.rgb      RGB565 little-endian, 320x240
  sdcard/show/f_<emotion>_t.rgb    RGB565 little-endian, 320x240
  sdcard/show/e_<emotion>.wav      copied 16 kHz stereo PCM16 WAV
  sdcard/show/e_<emotion>.env      one byte per 40 ms window, level 0..15
"""
from pathlib import Path
import shutil
import struct
import sys
import wave

EMOTIONS = [
    "neutral", "happy", "laughing", "excited", "love",
    "curious", "surprised", "sad", "angry", "sleepy",
]
WIN_MS = 40
WIDTH = 320
HEIGHT = 240


def require_pillow():
    try:
        from PIL import Image
        return Image
    except ModuleNotFoundError:
        print("Pillow is required. Install with: py -m pip install pillow", file=sys.stderr)
        raise


def convert_face(Image, src: Path, dst: Path):
    im = Image.open(src).convert("RGB")
    if im.size != (WIDTH, HEIGHT):
        im = im.resize((WIDTH, HEIGHT))

    out = bytearray()
    for r, g, b in im.getdata():
        v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
        out += struct.pack("<H", v)
    dst.write_bytes(out)


def envelope(src: Path):
    with wave.open(str(src), "rb") as w:
        ch = w.getnchannels()
        sw = w.getsampwidth()
        sr = w.getframerate()
        n = w.getnframes()
        if ch != 2 or sw != 2 or sr != 16000:
            raise ValueError(f"{src} must be stereo PCM16 16000 Hz")
        raw = w.readframes(n)

    samples = struct.unpack("<" + "h" * (len(raw) // 2), raw)
    mono = [(samples[i] + samples[i + 1]) // 2 for i in range(0, len(samples), 2)]
    peak = max((abs(s) for s in mono), default=1) or 1
    win = sr * WIN_MS // 1000

    levels = bytearray()
    for i in range(0, len(mono), win):
        seg = mono[i:i + win]
        lvl = round(max((abs(s) for s in seg), default=0) * 15 / peak)
        levels.append(max(0, min(15, lvl)))
    return levels


def main():
    root = Path(__file__).resolve().parents[1]
    faces = root / "docs" / "src" / "faces"
    sounds = root / "docs" / "src" / "sound"
    out = root / "sdcard" / "show"
    out.mkdir(parents=True, exist_ok=True)

    Image = require_pillow()
    for emo in EMOTIONS:
        convert_face(Image, faces / f"f_{emo}.jpg", out / f"f_{emo}.rgb")
        convert_face(Image, faces / f"f_{emo}_t.jpg", out / f"f_{emo}_t.rgb")
        shutil.copy2(sounds / f"e_{emo}.wav", out / f"e_{emo}.wav")
        (out / f"e_{emo}.env").write_bytes(envelope(sounds / f"e_{emo}.wav"))
        print(f"{emo:10s} ok")

    print(f"wrote {out}")
    print("Copy the contents of sdcard/ to the root of the microSD card.")


if __name__ == "__main__":
    main()
