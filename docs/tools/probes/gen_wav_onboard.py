# Generate a valid stereo 16-bit WAV directly on the board (no serial binary
# transfer). 1 s, 16 kHz, 523 Hz sine.
import math, ustruct, gc
gc.collect()
sr = 16000
dur = 1.0
freq = 523
nframes = int(sr * dur)
datasize = nframes * 4            # 2 ch * 2 bytes
hdr = (b'RIFF' + ustruct.pack('<I', 36 + datasize) + b'WAVEfmt '
       + ustruct.pack('<IHHIIHH', 16, 1, 2, sr, sr * 4, 4, 16)
       + b'data' + ustruct.pack('<I', datasize))
f = open('/flash/beep.wav', 'wb')
f.write(hdr)
buf = bytearray()
for i in range(nframes):
    s = int(20000 * math.sin(2 * math.pi * freq * i / sr))
    buf += ustruct.pack('<hh', s, s)
    if len(buf) >= 2048:
        f.write(buf)
        buf = bytearray()
if buf:
    f.write(buf)
f.close()
print("wrote /flash/beep.wav bytes:", 44 + datasize)
