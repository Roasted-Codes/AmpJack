import sys, wave, numpy as np
from scipy import signal
f = wave.open(sys.argv[1]); sr = f.getframerate()
x = np.frombuffer(f.readframes(f.getnframes()), '<i2').astype(float)
print(f"{len(x)/sr:.2f} s @ {sr}, rms {np.sqrt(np.mean(x**2)):.1f}, peak {np.abs(x).max():.0f}, dc {x.mean():.2f}")
# 20 ms windows: loud vs quiet
w = sr // 50; n = len(x) // w
r = np.sqrt(np.mean(x[:n*w].reshape(n, w)**2, 1))
print("window rms percentiles 10/50/90:", np.percentile(r, [10, 50, 90]).round(1), "exact-zero windows", int((r == 0).sum()))
def peaks(seg, label):
    if len(seg) < sr // 4: print(label, "too short"); return
    fr, p = signal.welch(seg, sr, nperseg=8192)
    db = 10*np.log10(p + 1e-12)
    floor = signal.medfilt(db, 101)
    exc = db - floor
    idx = signal.find_peaks(exc, height=10, distance=3)[0]
    idx = idx[np.argsort(-exc[idx])][:12]
    print(label, "tonal peaks (Hz : dB above floor : level dB):",
          ", ".join(f"{fr[i]:.0f}:{exc[i]:.0f}:{db[i]:.0f}" for i in sorted(idx)))
quiet = np.concatenate([x[i*w:(i+1)*w] for i in range(n) if 0 < r[i] < np.percentile(r[r > 0], 30)]) if (r > 0).any() else x[:0]
loud = np.concatenate([x[i*w:(i+1)*w] for i in range(n) if r[i] > np.percentile(r, 70)])
peaks(x, "whole:"); peaks(quiet, "quiet (gate open, not talking):"); peaks(loud, "loud (talking):")
# discontinuities: sample-to-sample jumps far above local slope
d = np.abs(np.diff(x)); thr = 8 * np.median(d[d > 0]) if (d > 0).any() else 1
dd = np.abs(np.diff(x, 2)); spikes = np.where(dd > max(12 * np.median(dd[dd>0]), 200))[0]
print(f"2nd-difference spikes: {len(spikes)} ({len(spikes)/(len(x)/sr):.1f}/s)")
if len(spikes) > 3:
    gaps = np.diff(spikes); print("  spacing (samples) most common:", np.bincount(gaps[gaps < 2400]).argsort()[-5:][::-1] if (gaps < 2400).any() else "-")
    print("  spike position mod 24 (1 ms USB frame):", np.bincount(spikes % 24, minlength=24))
