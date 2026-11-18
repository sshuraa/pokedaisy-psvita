# The sound test ROM's recording: per second, the strongest frequency and how much of the signal isn't that tone
# (everything but the fundamental and, for the square, its odd harmonics), in dB below the tone.
import sys, numpy as np, wave
p = sys.argv[1]
w = wave.open(p); a = np.frombuffer(w.readframes(w.getnframes()), dtype='<i2').reshape(-1, 2)[:, 0].astype(float)
start = np.nonzero(np.abs(a) > 300)[0][0]
print(p.split('/')[-1], 'tone starts at', round(start / 48000, 2), 's')
for sec in range(0, 22):
    seg = a[start + sec * 48000 + 2400: start + (sec + 1) * 48000 - 2400]
    if len(seg) < 40000 or np.abs(seg).max() < 300:
        print(f'  {sec:2d}s: silent / end'); continue
    seg = seg - seg.mean()
    win = np.hanning(len(seg)); f = np.fft.rfft(seg * win); mag = np.abs(f) ** 2
    freqs = np.fft.rfftfreq(len(seg), 1 / 48000)
    k = np.argmax(mag[5:]) + 5; f0 = freqs[k]
    keep = np.zeros_like(mag, dtype=bool)
    square = sec >= 10
    for h in range(1, 60, 2 if square else 1):
        if not square and h > 1: break
        idx = np.argmin(np.abs(freqs - f0 * h)); keep[max(0, idx - 6): idx + 7] = True
    noise = mag[~keep][3:].sum(); sig = mag[keep].sum()
    # dropouts: 1 ms windows far quieter than the tone
    env = np.sqrt(np.convolve(seg ** 2, np.ones(48) / 48, 'valid'))
    drops = int(np.sum(env < 0.2 * np.median(env)) / 48)
    print(f'  {sec:2d}s: {f0:7.1f} Hz, {"square" if square else "sine"}, rest {10 * np.log10(noise / sig):6.1f} dB, quiet ms {drops}')
