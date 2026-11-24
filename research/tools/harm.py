import sys, wave, numpy as np
from scipy import signal
f = wave.open(sys.argv[1]); sr = f.getframerate()
x = np.frombuffer(f.readframes(f.getnframes()), '<i2').astype(float)
w = sr//50; n=len(x)//w; r=np.sqrt(np.mean(x[:n*w].reshape(n,w)**2,1))
op = np.concatenate([x[i*w:(i+1)*w] for i in range(n) if r[i] > 0])
fr, p = signal.welch(op, sr, nperseg=16384)
db = 10*np.log10(p+1e-12); floor = signal.medfilt(db, 61); exc = db-floor
for lo, hi in ((5000, 12000), (100, 5000)):
    m = (fr>lo)&(fr<hi); idx = signal.find_peaks(np.where(m, exc, 0), height=3, distance=5)[0]
    pk = fr[idx]; print(f"{lo}-{hi} Hz peaks ({len(pk)}):", np.round(pk[:40]).astype(int))
    if len(pk) > 3: d = np.diff(pk); print("  spacing median", np.median(d).round(1), "Hz; common:", np.round(np.sort(d)[len(d)//4:3*len(d)//4]).astype(int)[:15])
# autocorrelation of the high band envelope: what repeats
b,a = signal.butter(6, 5000, 'highpass', fs=sr); h = signal.filtfilt(b,a,op)
ac = signal.correlate(h, h, 'full', 'fft')[len(h)-1:]; ac /= ac[0]
lag = np.arange(len(ac))
for lo, hi in ((20, 2400),):
    k = lo + np.argmax(ac[lo:hi]); print(f"highband autocorr best lag {k} samples = {sr/k:.1f} Hz (r={ac[k]:.2f})")
