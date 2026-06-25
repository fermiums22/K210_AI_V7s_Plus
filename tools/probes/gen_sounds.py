# Generate 10 short emotion sounds as WAVs on /sd (on-board synth, reliable).
import math, ustruct, gc
SR = 16000


def write_wav(path, notes):
    frames = bytearray()
    fade = int(SR * 0.006)
    for note in notes:
        freq, dur, wave, vol = note
        n = int(SR * dur)
        for i in range(n):
            if freq <= 0:
                s = 0
            else:
                ph = 2 * math.pi * freq * i / SR
                if wave == 'sq':
                    s = vol if math.sin(ph) >= 0 else -vol
                elif wave == 'saw':
                    s = int(vol * (2 * ((freq * i / SR) % 1) - 1))
                else:
                    s = int(vol * math.sin(ph))
            env = 1.0
            if i < fade:
                env = i / fade
            elif i > n - fade:
                env = (n - i) / fade
            v = int(s * env)
            frames += ustruct.pack('<hh', v, v)
    ds = len(frames)
    hdr = (b'RIFF' + ustruct.pack('<I', 36 + ds) + b'WAVEfmt '
           + ustruct.pack('<IHHIIHH', 16, 1, 2, SR, SR * 4, 4, 16)
           + b'data' + ustruct.pack('<I', ds))
    f = open(path, 'wb'); f.write(hdr); f.write(frames); f.close()
    print("wrote", path, ds + 44)
    gc.collect()


V = 16000
SNDS = {
 'neutral':  [(784, .14, 's', 11000), (0, .05, 's', 0), (784, .14, 's', 11000)],
 'happy':    [(523, .12, 's', V), (659, .12, 's', V), (784, .14, 's', V), (1047, .22, 's', V)],
 'laughing': [(784, .09, 's', V), (0, .05, 's', 0), (659, .09, 's', V), (0, .05, 's', 0),
              (784, .09, 's', V), (0, .05, 's', 0), (659, .14, 's', V)],
 'excited':  [(523, .07, 's', V), (587, .07, 's', V), (659, .07, 's', V), (784, .07, 's', V),
              (880, .07, 's', V), (1047, .2, 's', V)],
 'love':     [(659, .18, 's', 9000), (784, .18, 's', 9000), (988, .3, 's', 9000)],
 'curious':  [(587, .14, 's', 13000), (0, .04, 's', 0), (880, .24, 's', 13000)],
 'surprised':[(440, .04, 's', V), (560, .04, 's', V), (700, .04, 's', V), (860, .04, 's', V),
              (1050, .05, 's', V), (1320, .14, 's', V)],
 'sad':      [(523, .22, 's', 10000), (440, .22, 's', 10000), (349, .36, 's', 10000)],
 'angry':    [(120, .18, 'saw', 14000), (98, .18, 'saw', 14000), (120, .22, 'saw', 14000)],
 'sleepy':   [(440, .14, 's', 8000), (392, .14, 's', 8000), (330, .16, 's', 8000), (262, .34, 's', 8000)],
}

for emo, notes in SNDS.items():
    write_wav('/sd/e_%s.wav' % emo, notes)
print("ALL SOUNDS DONE")
