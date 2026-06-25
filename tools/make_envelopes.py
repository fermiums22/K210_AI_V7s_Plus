#!/usr/bin/env python3
"""Per-emotion amplitude envelope of e_<emo>.wav: a level 0..15 every WIN_MS so
the show drives the mouth (loud = open) and the amp gate (quiet >=100 ms ->
mute) from the audio energy. This is the file-time pre-computation of exactly
the logic that will run on the LIVE wifi audio stream later.

Usage: python tools/make_envelopes.py <voice_dir> <out_envelopes.py>
"""
import sys, os, wave
import numpy as np

EMOS = ["neutral", "happy", "laughing", "excited", "love",
        "curious", "surprised", "sad", "angry", "sleepy"]
WIN_MS = 40


def envelope(path):
    w = wave.open(path, "rb")
    sr = w.getframerate(); ch = w.getnchannels(); n = w.getnframes()
    a = np.frombuffer(w.readframes(n), dtype=np.int16).astype(np.float32)
    w.close()
    if ch == 2:
        a = a.reshape(-1, 2).mean(axis=1)
    win = int(sr * WIN_MS / 1000.0)
    peak = a.max() and np.abs(a).max() or 1.0
    levels = []
    for i in range(0, len(a), win):
        seg = np.abs(a[i:i + win])
        lvl = int(round((seg.max() / peak) * 15)) if len(seg) else 0
        levels.append(max(0, min(15, lvl)))
    return levels


def main(argv):
    vd, out = argv[1], argv[2]
    env = {}
    for emo in EMOS:
        env[emo] = envelope(os.path.join(vd, "e_%s.wav" % emo))
        print("%-10s %d windows" % (emo, len(env[emo])))
    with open(out, "w", encoding="utf-8") as f:
        f.write("# auto-generated amplitude envelopes (level 0..15 per WIN_MS)\n")
        f.write("WIN_MS = %d\n" % WIN_MS)
        f.write("ENV = {\n")
        for emo in EMOS:
            # pack as a compact string of hex nibbles to keep the file small
            s = "".join("%x" % v for v in env[emo])
            f.write("    %r: %r,\n" % (emo, s))
        f.write("}\n")
    print("wrote", out)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
