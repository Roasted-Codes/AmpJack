import sys, wave, numpy as np
import matplotlib; matplotlib.use("Agg"); import matplotlib.pyplot as plt
from scipy import signal
f = wave.open(sys.argv[1]); sr = f.getframerate()
x = np.frombuffer(f.readframes(f.getnframes()), '<i2').astype(float)
w = sr // 50; n = len(x)//w; r = np.sqrt(np.mean(x[:n*w].reshape(n,w)**2,1))
z = r == 0; trans = int(np.sum(z[1:] != z[:-1]))
print("gate open/close transitions:", trans, "in", len(x)/sr, "s")
b, a = signal.butter(4, [4500, 4900], 'bandpass', fs=sr); band = signal.filtfilt(b, a, x)
br = np.sqrt(np.mean(band[:n*w].reshape(n,w)**2,1))
print("4.5-4.9 kHz band vs speech rms correlation:", np.corrcoef(br, r)[0,1].round(2))
print("band rms when talking / quiet-open:", br[r > np.percentile(r,70)].mean().round(1), br[(r>0)&(r<np.percentile(r[r>0],30))].mean().round(1))
fig, ax = plt.subplots(3, 1, figsize=(14, 10))
fr, t, S = signal.spectrogram(x, sr, nperseg=1024, noverlap=768)
ax[0].pcolormesh(t, fr, 10*np.log10(S+1e-6), vmin=-30, vmax=40, shading='auto'); ax[0].set_ylabel("Hz"); ax[0].set_title("spectrogram")
ax[1].plot(np.arange(len(x))/sr, x, lw=0.3); ax[1].set_title("waveform")
ax[2].plot(np.arange(n)*0.02, 20*np.log10(r/32768+1e-9), label="20 ms rms dBFS"); ax[2].axhline(-45, color='r', ls='--', label="gate low -45"); ax[2].set_ylim(-90, -10); ax[2].legend()
plt.tight_layout(); plt.savefig(sys.argv[2], dpi=70)
