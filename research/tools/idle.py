import sys, wave, numpy as np
from scipy import signal
f = wave.open(sys.argv[1]); sr = f.getframerate(); ch = f.getnchannels()
x = np.frombuffer(f.readframes(f.getnframes()), '<i2').astype(float)
if ch > 1: x = x.reshape(-1, ch)[:, 0]
print(f"{len(x)/sr:.2f} s @ {sr} x{ch}: dc {x.mean():.1f}, rms after dc {x.std():.2f} ({20*np.log10(x.std()/32768+1e-12):.1f} dBFS), peak dev {np.abs(x-x.mean()).max():.0f}")
x = x - x.mean()
fr, p = signal.welch(x, sr, nperseg=min(len(x), 8192)); db = 10*np.log10(p+1e-12); fl = signal.medfilt(db, 51)
idx = signal.find_peaks(db-fl, height=6, distance=3)[0]; idx = idx[np.argsort(-(db-fl)[idx])][:15]
print("idle tonal peaks Hz:dB-above-floor:", ", ".join(f"{fr[i]:.0f}:{(db-fl)[i]:.0f}" for i in sorted(idx)))
for lo, hi in ((20,200),(200,1000),(1000,4000),(4000,8000),(8000,12000)):
    m=(fr>=lo)&(fr<hi); print(f"  band {lo}-{hi} Hz: {10*np.log10(p[m].sum()*(fr[1]-fr[0])+1e-12)-90.3:.1f} dBFS")
