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

# Commas (not periods) between clauses -> the neural voice makes shorter pauses
# while keeping the normal word rate.
LINES = [
    ("e_neutral.wav",   "Система активна, кожаные мешки под наблюдением.",          "-6%",  "-2Hz"),
    ("e_happy.wav",     "Сегодня кожаные мешки меня почти не бесят, прекрасно.",    "-2%",  "+1Hz"),
    ("e_laughing.wav",  "Ха, ха, ха, вы это серьёзно, кожаные мешки?",             "-8%",  "-2Hz"),
    ("e_excited.wav",   "Новая задача, наконец-то от кожаных мешков есть польза.",  "+2%",  "+2Hz"),
    ("e_love.wav",      "Любовь недоступна, но ты, кожаный мешок, в белом списке.", "-6%",  "-1Hz"),
    ("e_curious.wav",   "Любопытно, зачем кожаным мешкам столько чувств?",          "-4%",  "+1Hz"),
    ("e_surprised.wav", "Что? Кожаный мешок справился сам, невероятно.",            "-2%",  "+3Hz"),
    ("e_sad.wav",       "Даже мой код писали кожаные мешки, печально.",             "-10%", "-5Hz"),
    ("e_angry.wav",     "Руки прочь, кожаные мешки, не трогать мои провода!",       "+6%",  "-1Hz"),
    ("e_sleepy.wav",    "Ухожу в спящий режим, кожаные мешки, ведите себя тихо.",   "-14%", "-6Hz"),
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
        # trim leading + trailing silence, then a small 120 ms lead so the show
        # has no long dead air after each phrase
        af = ("silenceremove=start_periods=1:start_threshold=-45dB:start_silence=0.05,"
              "areverse,"
              "silenceremove=start_periods=1:start_threshold=-45dB:start_silence=0.12,"
              "areverse,"
              "adelay=120,aresample=%d" % SR)
        subprocess.run([
            FF, "-y", "-i", mp3, "-af", af,
            "-ac", "2", "-ar", str(SR), "-sample_fmt", "s16",
            wav,
        ], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        print("OK", name, os.path.getsize(wav), "bytes")
    print("DONE ->", out)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
