import sys, wave, numpy as np
from scipy import signal
f = wave.open(sys.argv[1]); sr = f.getframerate()
x = np.frombuffer(f.readframes(f.getnframes()), '<i2').astype(float)
w = sr//50; n=len(x)//w; r=np.sqrt(np.mean(x[:n*w].reshape(n,w)**2,1))
op = np.concatenate([x[i*w:(i+1)*w] for i in range(n) if r[i] > 0])
N = 1 << 17
P = np.abs(np.fft.rfft(op[:N] * np.hanning(min(N, len(op))), N))**2 if len(op) >= N else np.abs(np.fft.rfft(np.pad(op*np.hanning(len(op)), (0, N-len(op)))))**2
fr = np.fft.rfftfreq(N, 1/sr); db = 10*np.log10(P+1e-9)
def comb(f0, lo=3000, hi=11500):
    ks = np.arange(int(lo/f0)+1, int(hi/f0)+1); s = []
    for k in ks:
        i = int(round(k*f0*N/sr)); on = db[i-2:i+3].max(); off = np.median(db[i-40:i+40]); s.append(on-off)
    return np.mean(s)
cands = [62.5, 100, 120, 125, 166.67, 200, 250, 333.3, 500, 1000, 41.67, 83.33]
for c in cands: print(f"comb {c:7.2f} Hz ({sr/c:6.1f} samples): {comb(c):5.1f} dB above local median")
best = max(np.arange(40, 1100, 0.25), key=comb); print("best comb", best, "Hz", round(comb(best),1), "dB; period", sr/best, "samples")
# fold the high band at that period: a repeating click shows as a consistent pattern
b,a = signal.butter(6, 3000, 'highpass', fs=sr); h = signal.filtfilt(b,a,x)
per = int(round(sr/125)); m = (len(h)//per)*per; fold = h[:m].reshape(-1, per)
keep = np.array([r[min(i*per//w, n-1)] > 0 for i in range(fold.shape[0])])
avg = fold[keep].mean(0); print("folded high band at 192 samples: peak |avg|", np.abs(avg).max().round(1), "at", np.argmax(np.abs(avg)), "; rms of avg", np.sqrt(np.mean(avg**2)).round(2), "vs per-frame rms", np.sqrt(np.mean(fold[keep]**2)).round(1))
