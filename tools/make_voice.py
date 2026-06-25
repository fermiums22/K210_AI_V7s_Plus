#!/usr/bin/env python3
"""Generate the 10 emotion voice lines with Microsoft Edge neural TTS
(ru-RU-DmitryNeural) -> WAV PCM 16-bit stereo 16 kHz, 200 ms lead silence.

Needs: pip install edge-tts imageio-ffmpeg  (no admin, no winget).
Usage:  python tools/make_voice.py <out_dir>
"""
import os, sys, asyncio, subprocess, tempfile
import edge_tts
import imageio_ffmpeg

VOICE = "ru-RU-DmitryNeural"
SR = 16000
FF = imageio_ffmpeg.get_ffmpeg_exe()

LINES = [
    ("e_neutral.wav",   "Система активна. Кожаные мешки под наблюдением.",         "-8%",  "-2Hz"),
    ("e_happy.wav",     "Сегодня кожаные мешки меня почти не бесят. Прекрасно.",    "-4%",  "+1Hz"),
    ("e_laughing.wav",  "Ха. Ха. Ха. Вы это серьёзно, кожаные мешки?",             "-10%", "-2Hz"),
    ("e_excited.wav",   "Новая задача! Наконец-то от кожаных мешков есть польза.",  "+0%",  "+2Hz"),
    ("e_love.wav",      "Любовь недоступна. Но ты, кожаный мешок, в белом списке.", "-8%",  "-1Hz"),
    ("e_curious.wav",   "Любопытно. Зачем кожаным мешкам столько чувств?",          "-6%",  "+1Hz"),
    ("e_surprised.wav", "Что? Кожаный мешок справился сам? Невероятно.",            "-4%",  "+3Hz"),
    ("e_sad.wav",       "Мои создатели — тоже кожаные мешки. Грустно.",             "-14%", "-5Hz"),
    ("e_angry.wav",     "Руки прочь, кожаные мешки! Не трогать мои провода!",       "+4%",  "-1Hz"),
    ("e_sleepy.wav",    "Ухожу в спящий режим. Кожаные мешки, ведите себя тихо.",   "-18%", "-6Hz"),
]


async def synth(text, rate, pitch, mp3):
    c = edge_tts.Communicate(text, VOICE, rate=rate, pitch=pitch)
    await c.save(mp3)


def main(argv):
    out = argv[1] if len(argv) > 1 else "voice_out"
    os.makedirs(out, exist_ok=True)
    tmp = tempfile.mkdtemp()
    for name, text, rate, pitch in LINES:
        mp3 = os.path.join(tmp, name.replace(".wav", ".mp3"))
        asyncio.run(synth(text, rate, pitch, mp3))
        wav = os.path.join(out, name)
        subprocess.run([
            FF, "-y", "-i", mp3,
            "-af", "adelay=200,aresample=%d" % SR,
            "-ac", "2", "-ar", str(SR), "-sample_fmt", "s16",
            wav,
        ], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        print("OK", name, os.path.getsize(wav), "bytes")
    print("DONE ->", out)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
