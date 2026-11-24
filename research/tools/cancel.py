import sys, wave, numpy as np
from scipy import signal
def load(p):
    f=wave.open(p); sr=f.getframerate(); x=np.frombuffer(f.readframes(f.getnframes()),'<i2').astype(float); return x, sr
def periodic_frac(x, per):
    m=(len(x)//per)*per; F=x[:m].reshape(-1,per); avg=F.mean(0)
    # expected rms of the mean of pure noise: rms/sqrt(rows)
    return np.sqrt(np.mean(avg**2)), np.std(F)/np.sqrt(F.shape[0])
def canceller(x, per=192, shift=6):
    avg=np.zeros(per); y=np.empty_like(x)
    for i,v in enumerate(x):
        k=i%per; avg[k]+= (v-avg[k])/(1<<shift); y[i]=v-avg[k]
    return y
def comb(x, sr, lo, hi, f0=125):
    N=len(x); P=np.abs(np.fft.rfft(x*np.hanning(N)))**2; db=10*np.log10(P+1e-9)
    s=[db[int(round(k*f0*N/sr))]-np.median(db[int(round(k*f0*N/sr))-30:int(round(k*f0*N/sr))+30]) for k in range(max(1,int(lo/f0)),int(hi/f0))]
    return np.mean(s)
x, sr = load(sys.argv[1]); x = x - x.mean()
b,a=signal.butter(4,3000,'highpass',fs=sr); h=signal.filtfilt(b,a,x)
for per in (190,191,192,193,194,384):
    pa, noise = periodic_frac(h, per); print(f"fold {per}: periodic rms {pa:.2f} (noise would give {noise:.2f})")
for sh in (5,6,7):
    y = canceller(x, 192, sh)
    skip = int(0.4*sr) if len(x) > sr else int(0.25*sr)
    print(f"canceller 1/{1<<sh}: comb 5-9k {comb(x[skip:],sr,5000,9000):.1f} -> {comb(y[skip:],sr,5000,9000):.1f} dB; 3-5k {comb(x[skip:],sr,3000,5000):.1f} -> {comb(y[skip:],sr,3000,5000):.1f}; rms {x[skip:].std():.2f} -> {y[skip:].std():.2f}")
if len(sys.argv) > 2:
    y = canceller(x, 192, int(sys.argv[2])); y=np.clip(np.round(y), -32768, 32767).astype('<i2')
    w=wave.open(sys.argv[3],'wb'); w.setnchannels(1); w.setsampwidth(2); w.setframerate(sr); w.writeframes(y.tobytes())
