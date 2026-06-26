#!/usr/bin/env python3
"""Synthesize Russian phrases with espeak-ng and robotize them (ring modulation),
output low-rate WAVs sized for the K210 (default 11025 Hz, stereo 16-bit).

Usage: robovoice.py <out_dir> <rate> <stereo:0|1>   (then reads phrases below)
or import and call gen(text, path).
"""
import os, sys, wave, subprocess, struct, math
import numpy as np

ESPEAK = r"C:\msys64\mingw64\bin\espeak-ng.exe"
TMP = os.path.join(os.environ.get("TEMP", "."), "_espk.wav")


TXT = os.path.join(os.environ.get("TEMP", "."), "_espk.txt")


def synth(text):
    # Pass text via a UTF-8 FILE (not argv) so Cyrillic isn't mangled by the
    # Windows ANSI code page -> espeak would otherwise read garbage.
    with open(TXT, "w", encoding="utf-8") as f:
        f.write(text)
    subprocess.run([ESPEAK, "-v", "ru", "-s", "165", "-p", "40",
                    "-w", TMP, "-f", TXT], check=True)
    w = wave.open(TMP, "rb")
    sr = w.getframerate(); ch = w.getnchannels(); sw = w.getsampwidth()
    raw = w.readframes(w.getnframes()); w.close()
    a = np.frombuffer(raw, dtype=np.int16).astype(np.float32)
    if ch == 2:
        a = a.reshape(-1, 2).mean(axis=1)
    return a / 32768.0, sr


def robotize(x, sr, carrier=30.0):
    # Gentle robotization: a light tremolo-style ring mod keeps speech clearly
    # intelligible while adding a robotic warble. espeak already supplies the
    # metallic timbre, so we stay subtle (no comb echo, no hard clipping).
    n = len(x)
    t = np.arange(n) / sr
    rm = x * (0.80 + 0.20 * np.sin(2 * math.pi * carrier * t))
    return rm


def resample(x, sr_in, sr_out):
    if sr_in == sr_out:
        return x
    n_out = int(len(x) * sr_out / sr_in)
    xp = np.linspace(0, len(x) - 1, n_out)
    return np.interp(xp, np.arange(len(x)), x)


def gen(text, path, rate=11025, stereo=True, gain=0.9, robot=True):
    x, sr = synth(text)
    if robot:
        x = robotize(x, sr)
    x = resample(x, sr, rate)
    m = np.max(np.abs(x)) or 1.0
    x = (x / m) * gain
    pcm = np.clip(x * 32767, -32768, 32767).astype('<i2')
    if stereo:
        pcm = np.repeat(pcm[:, None], 2, axis=1).reshape(-1)
        ch = 2
    else:
        ch = 1
    w = wave.open(path, "wb")
    w.setnchannels(ch); w.setsampwidth(2); w.setframerate(rate)
    w.writeframes(pcm.tobytes()); w.close()
    return os.path.getsize(path)


if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else "."
    rate = int(sys.argv[2]) if len(sys.argv) > 2 else 11025
    stereo = (sys.argv[3] != "0") if len(sys.argv) > 3 else True
    os.makedirs(out, exist_ok=True)
    # one quick test phrase
    sz = gen("Привет, кожаные мешки. Система активна.",
             os.path.join(out, "test.wav"), rate, stereo)
    print("wrote test.wav", sz, "bytes", rate, "Hz", "stereo" if stereo else "mono")
