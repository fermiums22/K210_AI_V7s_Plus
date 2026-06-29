#!/usr/bin/env python3
import struct
import sys
import wave

WIN_MS = 40

src, dst = sys.argv[1], sys.argv[2]
with wave.open(src, "rb") as w:
    channels = w.getnchannels()
    width = w.getsampwidth()
    rate = w.getframerate()
    frames = w.getnframes()
    if channels != 2 or width != 2 or rate != 16000:
        raise SystemExit(f"{src} must be stereo PCM16 16000 Hz")
    raw = w.readframes(frames)

samples = struct.unpack("<" + "h" * (len(raw) // 2), raw)
mono = [(samples[i] + samples[i + 1]) // 2 for i in range(0, len(samples), 2)]
peak = max((abs(s) for s in mono), default=1) or 1
win = rate * WIN_MS // 1000

levels = bytearray()
for i in range(0, len(mono), win):
    mx = max((abs(s) for s in mono[i:i + win]), default=0)
    levels.append(max(0, min(15, round(mx * 15 / peak))))

with open(dst, "wb") as f:
    f.write(levels)
